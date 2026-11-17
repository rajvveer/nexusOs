/* Preemption and blocking kernel continuations on every CPU, under the big kernel lock (smp.c). */
#include "native.h"
#include "gpu.h"
#include "swap.h"
#include "../../kernel/string.h"
#include "../../kernel/fat32.h"
#include "../../kernel/vga.h"

#define STACK_BYTES (17 * 4096ULL)
enum task_state { RUNNABLE, WAITING, ZOMBIE };
struct resource_limit { uint64_t soft, hard; };
/* RLIMIT_DATA, RLIMIT_NOFILE, RLIMIT_AS, RLIMIT_MEMLOCK (8 MiB, the Linux default since 5.16) */
static const struct resource_limit initial_limits[] = {{~0ULL, ~0ULL}, {1024, 1048576}, {~0ULL, ~0ULL}, {8ULL << 20, 8ULL << 20}};
/* Linux defaults of the others: CPU, FSIZE, STACK (fixed 8 MiB stack, reported), CORE (no core dumps are written), RSS and LOCKS
 * (not enforced by Linux either), NPROC (fork/clone), SIGPENDING, MSGQUEUE, NICE, RTPRIO, RTTIME. */
static const struct resource_limit default_limits[16] = {{~0ULL, ~0ULL}, {~0ULL, ~0ULL}, {0, 0}, {NATIVE_STACK_SIZE, ~0ULL}, {0, ~0ULL}, {~0ULL, ~0ULL},
    {63408, 63408}, {0, 0}, {0, 0}, {0, 0}, {~0ULL, ~0ULL}, {63408, 63408}, {819200, 819200}, {0, 0}, {0, 0}, {~0ULL, ~0ULL}};
struct process {
    struct process* next;
    struct pid_identity *identity, *group_id, *session_id;
    uint32_t pid, parent, group, session, threads, uid;
    uint64_t cpu_ns;
    struct resource_limit limits[4];
    struct resource_limit other_limits[16];   /* every other RLIMIT_* by number (default_limits) */
    bool executed, exiting;
    int status;
    /* Job control (stopped thread group, pending wait reports) and resource usage (getrusage/wait4). */
    bool stopped; uint8_t stop_signal, report; /* report: 1 stopped, 2 continued, not yet waited for */
    struct terminal* terminal;                 /* controlling terminal (NULL: none) */
    uint64_t faults, voluntary, involuntary, maxrss_kb;
    uint64_t child_cpu_ns, child_faults, child_voluntary, child_involuntary, child_maxrss_kb;
    int16_t oom_score_adj;                     /* /proc/pid/oom_score_adj */
    uint64_t alarm_deadline, alarm_interval;   /* ITIMER_REAL (native_clock_ns; 0: disarmed) */
    uint32_t ptracer; bool ptracer_any;        /* Yama PR_SET_PTRACER: the pid (or any) allowed to ptrace this process */
    uint32_t ns_slot[2];                        /* network / IPC namespace (namespaces.c slots; 0 = initial) */
    uint8_t exit_signal;                        /* clone/clone3 exit_signal to the parent (SIGCHLD after reparenting) */
};
struct task {
    struct task* next;
    struct process* process;
    struct task *vfork_parent, *vfork_child;
    struct address_space* space;
    struct descriptor_table* descriptors;
    struct filesystem_state* filesystem;
    struct signal_state* signals;
    struct credentials* credentials;
    struct pid_identity* identity;
    struct pid_namespace* children_namespace;
    uint64_t kernel_rsp, allocation, fs, gs, clear_tid;
    uint64_t deadline;
    uint64_t cpu_ns, run_started;
    struct futex_key futex;
    uint32_t futex_mask;
    bool futex_waiting, timed_out;
    void* channel;
    uint32_t pid;
    enum task_state state;
    int status;
    struct registers initial;
    uint8_t fpu[FPU_AREA_BYTES] __attribute__((aligned(64)));
    char executable[FAT32_PATH_MAX];
    uint64_t executable_inode;
    bool executable_deleted;
    int8_t nice; uint8_t policy, rt_priority; /* setpriority / sched_setscheduler */
    uint64_t runnable_since;                    /* when it last became ready to run */
    uint64_t vruntime;                          /* weighted CPU time (fair scheduling) */
    struct task *rq_next, *rq_prev;             /* run queue (state == RUNNABLE) */
    struct task *w_next, *w_prev;               /* wait-channel bucket */
    struct task *fx_next, *fx_prev;             /* futex bucket */
    struct task *tw_next, *tw_prev;             /* timed waiters */
    bool on_rq, w_linked, fx_linked, tw_linked;
    bool on_cpu; uint8_t cpu;                   /* running (or switching) on cpus[cpu] */
    unsigned w_bucket, fx_bucket;
    char comm[16];                              /* PR_SET_NAME */
    uint64_t faults, voluntary, involuntary;    /* RUSAGE_THREAD */
    uint64_t started;                           /* native_clock_ns() at creation (/proc stat starttime) */
    struct user_segments seg;                   /* TLS descriptors, DS/ES/FS/GS selectors (segments.c) */
    struct task* tracer;                        /* ptrace: the attached tracer thread */
    struct credentials* tracer_credentials;     /* recorded when the link was made (Linux ptracer_cred) */
    bool traced_stop, traced_reported; uint8_t traced_signal; int8_t traced_resume; struct registers* traced_regs;
    bool traced_exit;                           /* a zombie whose exit its tracer has not collected yet */
    bool io_free;                               /* in a system call that never reaches a filesystem (blockio.c) */
    bool uninterruptible;                       /* in task_sleep_uninterruptible (disk waits): /proc shows 'D' */
    uint64_t traced_sender;                     /* uid << 32 | pid of the tracer that resumed it (injected signals) */
    uint8_t seccomp_mode;                       /* seccomp.c: 0 disabled, 1 strict, 2 filter */
    struct seccomp_filter* seccomp_filters;     /* newest filter; ->prev chains to older ones (shared, refcounted) */
    uint32_t persona;                           /* personality(2): ADDR_NO_RANDOMIZE etc.; survives fork and execve */
    const struct clone3_request* clone3;        /* clone3 in progress (pidfd.c): the caller's frame, pidfd, extra flags */
    uint16_t mempolicy, ioprio;                 /* set_mempolicy mode|flags, ioprio_set value (kabi.c); inherited */
};
static void ptrace_resume(struct task* t, int signal);
static void ptrace_untrace(struct task* t);
static void ptrace_collect(struct task* t);
/* credentials.c (Linux cap_ptrace_traceme, LSM_UNSAFE_PTRACE) */
bool credentials_traceme_allowed(struct credentials* tracer, struct credentials* target);
struct credentials* credentials_exec_traced(struct credentials* source, struct credentials* tracer, struct address_space* space);
_Static_assert(sizeof(struct task) <= 4096, "task allocation size");
static struct task *tasks;
static struct process* processes;
int yama_ptrace_scope = 1;   /* /proc/sys/kernel/yama/ptrace_scope: 1 = restricted (ancestors only) by default */
int native_yama_scope(void) { return yama_ptrace_scope; }
/* Per-CPU state (re-evaluated at every use: a task may continue on another CPU after it blocks). */
#define current (cpu_self()->task)
#define scheduler_rsp (cpu_self()->scheduler_rsp)
#define loaded_fs (cpu_self()->loaded_fs)
#define loaded_gs (cpu_self()->loaded_gs)
uint64_t task_net_namespace(void) { return current ? current->process->ns_slot[NSOBJ_NET] : 0; }
uint32_t* task_nsobj_slot(unsigned kind) { return current ? &current->process->ns_slot[kind] : NULL; }
int task_set_children_namespace(struct pid_namespace* space) {   /* setns(CLONE_NEWPID) */
    if (!current || !space) return -22;
    pid_namespace_retain(space); pid_namespace_release(current->children_namespace); current->children_namespace = space; return 0;
}
int task_enter_user_namespace(struct user_namespace* space) {   /* setns(CLONE_NEWUSER): Linux userns_install */
    if (!current || !space) return -22;
    if (current->process->threads != 1) return -22;   /* tasks of one thread group share a user namespace */
    if (filesystem_users(current->filesystem) != 1) return -22;   /* nor may a root/cwd shared via CLONE_FS move */
    int error; struct credentials* next = credentials_enter(current->credentials, space, &error); if (!next) return error;
    struct credentials* previous = current->credentials; current->credentials = next;
    credentials_activate(next); credentials_destroy(previous); return 0;
}
void task_set_ptracer(int64_t pid) {
    if (!current) return;
    if (pid == -1) { current->process->ptracer_any = true; current->process->ptracer = 0; }
    else { current->process->ptracer_any = false; current->process->ptracer = (uint32_t)pid; }
}
static uint64_t min_vruntime;
static struct task* runqueue;
#define WAIT_BUCKETS 256
static struct task *wait_buckets[WAIT_BUCKETS], *futex_buckets[WAIT_BUCKETS], *timed_waiters;
static uint64_t next_deadline = ~0ULL;
static bool zombies_pending;
static void rq_add(struct task* t) {
    if (!sched_lock_held()) panic("runqueue change without the scheduler lock");
    if (t->on_rq) return;
    t->rq_prev = NULL; t->rq_next = runqueue; if (runqueue) runqueue->rq_prev = t; runqueue = t; t->on_rq = true;
}
static void rq_remove(struct task* t) {
    if (!sched_lock_held()) panic("runqueue change without the scheduler lock");
    if (!t->on_rq) return;
    if (t->rq_prev) t->rq_prev->rq_next = t->rq_next; else runqueue = t->rq_next;
    if (t->rq_next) t->rq_next->rq_prev = t->rq_prev;
    t->on_rq = false; t->rq_next = t->rq_prev = NULL;
}
static unsigned wait_hash(uint64_t value) { value ^= value >> 17; value *= 0x9E3779B97F4A7C15ULL; return (unsigned)(value >> 56) & (WAIT_BUCKETS - 1); }
static void wait_link(struct task* t) {
    if (!sched_lock_held()) panic("wait queue change without the scheduler lock");
    unsigned b = wait_hash((uint64_t)(uintptr_t)t->channel); t->w_bucket = b;
    t->w_prev = NULL; t->w_next = wait_buckets[b]; if (t->w_next) t->w_next->w_prev = t; wait_buckets[b] = t; t->w_linked = true;
}
static void wait_unlink(struct task* t) {
    if (!t->w_linked) return;
    if (t->w_prev) t->w_prev->w_next = t->w_next; else wait_buckets[t->w_bucket] = t->w_next;
    if (t->w_next) t->w_next->w_prev = t->w_prev;
    t->w_linked = false; t->w_next = t->w_prev = NULL;
}
static unsigned futex_hash(const struct futex_key* key) { return wait_hash(key->domain * 31 + key->word + key->kind); }
static void futex_link(struct task* t) {
    if (!sched_lock_held()) panic("futex queue change without the scheduler lock");
    unsigned b = futex_hash(&t->futex); t->fx_bucket = b;
    t->fx_prev = NULL; t->fx_next = futex_buckets[b]; if (t->fx_next) t->fx_next->fx_prev = t; futex_buckets[b] = t; t->fx_linked = true;
}
static void futex_unlink(struct task* t) {
    if (!t->fx_linked) return;
    if (t->fx_prev) t->fx_prev->fx_next = t->fx_next; else futex_buckets[t->fx_bucket] = t->fx_next;
    if (t->fx_next) t->fx_next->fx_prev = t->fx_prev;
    t->fx_linked = false; t->fx_next = t->fx_prev = NULL;
}
static void timed_link(struct task* t) {
    if (!sched_lock_held()) panic("timer queue change without the scheduler lock");
    if (t->tw_linked || !t->deadline) return;
    t->tw_prev = NULL; t->tw_next = timed_waiters; if (timed_waiters) timed_waiters->tw_prev = t; timed_waiters = t; t->tw_linked = true;
    if (t->deadline < next_deadline) next_deadline = t->deadline;
}
static void timed_unlink(struct task* t) {
    if (!t->tw_linked) return;
    if (t->tw_prev) t->tw_prev->tw_next = t->tw_next; else timed_waiters = t->tw_next;
    if (t->tw_next) t->tw_next->tw_prev = t->tw_prev;
    t->tw_linked = false; t->tw_next = t->tw_prev = NULL;
}
#define WAKE_CREDIT_NS 3000000ULL
/* kick, task_ready, task_ready_all: the caller holds sched_lock. */
static void wake_preempt(struct task* t);
static void kick(struct task* t) {
    if (!t->on_cpu) { if (!smp_wake_idle()) wake_preempt(t); }
    else if (t != current) smp_send_ipi(t->cpu, VEC_WAKE); /* signal or exit must be noticed now */
}
static void task_ready(struct task* t) {
    t->state = RUNNABLE; t->runnable_since = native_clock_ns(); rq_add(t); kick(t);
    /* Sleepers rejoin near the front, but never with an unbounded backlog of credit. */
    uint64_t floor = min_vruntime > WAKE_CREDIT_NS ? min_vruntime - WAKE_CREDIT_NS : 0;
    if (t->vruntime < floor) t->vruntime = floor;
}
static uint64_t task_weight(const struct task* t);
static bool should_preempt(void);
static void terminate(void) __attribute__((noreturn));
static int futex_wake(uint64_t address, bool private, int count, uint32_t mask);
static void orphan_hangup(uint32_t group);
static uint64_t cpu_busy_ns[MAX_CPUS]; /* /proc/stat, /proc/uptime */
static void job_signal(struct process* p, unsigned number);
extern uint8_t syscall_stack_top[];
extern void switch_context(uint64_t* previous, uint64_t next);
extern void resume_user(struct registers* registers) __attribute__((noreturn));

static uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}
bool task_active(void) { return current != NULL; }
unsigned task_count(void) { unsigned n = 0; for (struct task* t = tasks; t; t = t->next) if (t->state != ZOMBIE) n++; return n; }
uint64_t task_cpu_time(bool thread) {
    if (!current) return 0;
    return (thread ? current->cpu_ns : current->process->cpu_ns) + native_clock_ns() - current->run_started;
}
uint32_t task_pid(void) { return current ? current->process->pid : 1; }
uint32_t task_tid(void) { return current ? current->pid : 1; }
uint32_t task_parent_pid(void) { return current ? current->process->parent : 0; }
struct pid_namespace* task_pid_namespace(void) { return current ? current->identity->space : pid_namespace_root(); }
uint32_t task_visible_pid(uint32_t internal) {
    return current ? pid_number(pid_lookup(internal, pid_namespace_root()), task_pid_namespace()) : internal;
}
uint32_t task_internal_pid(uint32_t visible) {
    return current ? pid_number(pid_lookup(visible, task_pid_namespace()), pid_namespace_root()) : visible;
}
uint32_t task_uid(void) { return credentials_id(credentials_current(), false, 0); }
uint32_t task_gid(void) { return credentials_id(credentials_current(), true, 0); }
uint32_t task_kernel_uid(void) { return credentials_kernel_id(credentials_current(), false, 0); }
static int limit_index(unsigned resource) { return resource == 2 ? 0 : resource == 7 ? 1 : resource == 9 ? 2 : resource == 8 ? 3 : -1; }
uint64_t task_resource_soft(unsigned resource) {
    int index = limit_index(resource); if (index < 0) panic("unsupported resource quota");
    return current ? current->process->limits[index].soft : initial_limits[index].soft;
}
void task_set_clear_tid(uint64_t address) { if (!current) panic("no task for clear_tid"); current->clear_tid = address; }
static struct task* find(uint32_t pid) {
    for (struct task* t = tasks; t; t = t->next) if (t->pid == pid) return t;
    return NULL;
}
/* pm.c: whether a task sleeps at the disk I/O barrier (blockio.c) - waiting for another task's disk request */
bool task_at_channel(uint32_t pid, const void* channel) { struct task* t = find(pid); return t && t->state == WAITING && t->channel == channel; }
int task_nsobj_of(uint32_t tid, unsigned kind) { struct task* t = find(tid); return t ? (int)t->process->ns_slot[kind] : -2; }
int task_resource_limit(uint32_t pid, uint32_t resource, uint64_t next, uint64_t previous) {
    if (!current) return -3;
    if (resource >= 16) return -22;
    int index = limit_index(resource);
    struct resource_limit value;
    if (next) {
        if (!accessible(next, sizeof(value), false)) return -14;
        copy_from_user(&value, (void*)next, sizeof(value));
        if (value.soft > value.hard) return -22;
        if (resource == 7 && value.hard > 0x80000000ULL) return -1;
    }
    struct task* target = pid ? find(task_internal_pid(pid)) : current;
    if (!target || target->state == ZOMBIE) return -3;
    if (target->process != current->process && !credentials_limits_allowed(target->credentials)) return -1;
    struct resource_limit before = index < 0 ? target->process->other_limits[resource] : target->process->limits[index];
    if (next && value.hard > before.hard && !credentials_capable(credentials_current(), NULL, 24)) return -1;
    if (previous && !accessible(previous, sizeof(before), true)) return -14;
    if (next) { if (index < 0) target->process->other_limits[resource] = value; else target->process->limits[index] = value; }
    if (previous) copy_to_user((void*)previous, &before, sizeof(before));
    return 0;
}
struct pid_namespace* task_children_namespace(uint32_t tid) {
    struct task* t = find(tid); return t && t->state != ZOMBIE ? t->children_namespace : NULL;
}
struct credentials* task_credentials(uint32_t tid) {
    struct task* t = find(tid); return t && t->state != ZOMBIE ? t->credentials : NULL;
}
uint32_t task_process_id(uint32_t tid) {
    struct task* t = find(tid); return t && t->state != ZOMBIE ? t->process->pid : 0;
}
struct descriptor_table* task_files(uint32_t tid) {
    struct task* t = find(tid); return t && t->state != ZOMBIE ? t->descriptors : NULL;
}
struct filesystem_state* task_filesystem(uint32_t tid) {
    struct task* t = find(tid); return t && t->state != ZOMBIE ? t->filesystem : NULL;
}
const char* task_executable(uint32_t tid) {
    struct task* t = find(tid); return t && t->state != ZOMBIE ? t->executable : NULL;
}
bool task_executable_deleted(uint32_t tid) { struct task* t = find(tid); return t && t->executable_deleted; }
static uint64_t executable_inode(const char* path) {
    fat32_file_t file; return fat32_lookup(path, &file) ? disk_inode_id(&file) : 0;
}
int task_files_moved(const char* from, const char* to, uint64_t inode, bool directory, bool apply) {
    for (struct task* t = tasks; t; t = t->next) {
        if (!t->executable_inode || t->state == ZOMBIE) continue;
        if (!to) { if (apply && t->executable_inode == inode) t->executable_deleted = true; continue; }
        if (!directory && t->executable_inode != inode) continue;
        int error = path_moved(t->executable, from, to, apply); if (error) return error;
    }
    return 0;
}
bool task_is_dumpable(uint32_t tid) {
    struct task* t = find(tid); return t && t->state != ZOMBIE && space_dumpable(t->space);
}
bool task_inspect_allowed(uint32_t tid) {
    struct task* t = find(tid);
    return t && t->state != ZOMBIE && ((current && current->process == t->process) ||
           (credentials_inspect_allowed(t->credentials, space_dumpable(t->space)) && (!current || landlock_ptrace_allowed(current->pid, tid))));
}
uint32_t task_next_id(uint32_t group, uint32_t after) {
    uint32_t next = 0;
    if (group) {
        for (struct task* t = tasks; t; t = t->next)
            if (t->state != ZOMBIE && t->process->pid == group && t->pid > after && (!next || t->pid < next)) next = t->pid; /* a zombie's process may be reaped */
    } else for (struct process* p = processes; p; p = p->next)
        if (p->threads && p->pid > after && (!next || p->pid < next)) next = p->pid;
    return next;
}
static struct process* find_process(uint32_t pid) {
    for (struct process* p = processes; p; p = p->next) if (p->pid == pid) return p;
    return NULL;
}
unsigned task_thread_count(uint32_t group) {
    struct process* p = find_process(group); return p ? p->threads : 0;
}
/* RLIMIT_NPROC: processes whose real user is the caller's (Linux: not for root, CAP_SYS_RESOURCE or CAP_SYS_ADMIN) */
static bool nproc_exceeded(void) {
    uint64_t limit = current->process->other_limits[6].soft; if (limit == ~0ULL) return false;
    struct credentials* c = credentials_current(); uint32_t uid = credentials_kernel_id(c, false, 0);
    if (!uid || credentials_capable(c, NULL, 24) || credentials_capable(c, NULL, 21)) return false;
    uint64_t count = 0;
    for (struct task* t = tasks; t; t = t->next)
        if (t->state != ZOMBIE && t->pid == t->process->pid && t->credentials && credentials_kernel_id(t->credentials, false, 0) == uid) count++;
    return count >= limit;
}
/* RLIMIT_FSIZE of the caller (kernel.c write path) */
uint64_t task_file_size_limit(void) { return current ? current->process->other_limits[1].soft : ~0ULL; }
static struct process* process_new(struct pid_identity* identity, struct process* parent) {
    struct process* p = (void*)page_alloc(); if (!p) return NULL;
    memcpy(p->limits, parent ? parent->limits : initial_limits, sizeof(p->limits));
    memcpy(p->other_limits, parent ? parent->other_limits : default_limits, sizeof(p->other_limits));
    uint32_t pid = identity->numbers[0];
    p->identity = identity; pid_retain(identity);
    bool init = identity->numbers[identity->space->depth] == 1;
    p->group_id = parent && !init ? parent->group_id : identity; pid_retain(p->group_id);
    p->session_id = parent && !init ? parent->session_id : identity; pid_retain(p->session_id);
    p->pid = pid; p->threads = 1; p->uid = task_kernel_uid();
    p->parent = parent ? parent->pid : 0; p->group = p->group_id->numbers[0]; p->session = p->session_id->numbers[0];
    p->terminal = parent && !init ? parent->terminal : NULL;
    for (unsigned kind = 0; kind < 2; kind++) { p->ns_slot[kind] = parent ? parent->ns_slot[kind] : 0; nsobj_retain(kind, p->ns_slot[kind]); }
    p->exit_signal = 17;
    p->next = processes; processes = p; return p;
}
static void process_reap(struct process* p) {
    struct process** link = &processes; while (*link && *link != p) link = &(*link)->next;
    if (!*link || (current && current->process == p)) panic("invalid process wait record");
    *link = p->next;
    pidfd_process_changed(p->identity);   /* pidfds of a reaped process report POLLHUP (pidfd.c) */
    for (unsigned kind = 0; kind < 2; kind++) nsobj_release(kind, p->ns_slot[kind]);
    pid_release(p->group_id); pid_release(p->session_id); pid_release(p->identity); page_free((uint64_t)p);
}
static void wake_process(struct process* p) {
    sched_lock();
    for (struct task* t = tasks; t; t = t->next) if (t->process == p) {
        if (t->state == WAITING) task_ready(t);
        else if (t->state == RUNNABLE && t->on_cpu && t != current) kick(t);
    }
    sched_unlock();
}
bool task_exiting(void) { return current && current->process->exiting; }
void task_exit_group(int status) {
    if (!task_exiting()) { current->process->exiting = true; current->process->status = (status & 255) << 8; }
    wake_process(current->process);
}
static void task_start(void) {
    kernel_lock_acquire(); /* a scheduler loop switched here without the big lock */
    if (task_exiting()) terminate();
    struct registers* r = signal_dispatch(&current->initial);
    if (!r) terminate();
    segments_user_return(current->space);
    kernel_lock_release();
    resume_user(r);
}
/* Kernel stacks are STACK_BYTES of physically contiguous memory, which a system under memory pressure (free memory
 * scattered in single pages) cannot provide: thread creation then failed (pthread_create EAGAIN, zygote fork ENOMEM in
 * Chrome with swapped tabs). A pool sized by RAM (one stack per 8 MiB), filled at boot before the memory baselines
 * (task_stack_pool_init, reclaim.c) and refilled by exiting tasks, keeps guarded stacks ready; only demand beyond it
 * allocates. Big lock held. */
#define STACK_POOL_MAX 512   /* 2 GiB of RAM: 256 stacks (17 MiB, 0.85%); 64 MiB: 8 */
static uint64_t stack_pool[STACK_POOL_MAX]; static unsigned stack_pool_count, stack_pool_capacity;
static uint64_t stack_take(void) {
    if (stack_pool_count) { uint64_t s = stack_pool[--stack_pool_count]; memset((void*)(s + 4096), 0, STACK_BYTES - 4096); return s; }
    uint64_t s = arena_alloc(STACK_BYTES); if (s) kernel_guard_page(s, true);
    return s;
}
static void stack_give(uint64_t s) {
    if (stack_pool_count < stack_pool_capacity) { stack_pool[stack_pool_count++] = s; return; }
    kernel_guard_page(s, false); arena_free(s, STACK_BYTES);
}
void task_stack_pool_init(void) {
    uint64_t want = memory_total_pages() / 2048; stack_pool_capacity = want < 8 ? 8 : want > STACK_POOL_MAX ? STACK_POOL_MAX : (unsigned)want;
    while (stack_pool_count < stack_pool_capacity) {
        uint64_t s = arena_alloc(STACK_BYTES); if (!s) break;
        kernel_guard_page(s, true); stack_pool[stack_pool_count++] = s;
    }
    stack_pool_capacity = stack_pool_count;   /* full at the baselines: exits refill it to exactly this */
}
static struct task* allocate(void) {
    struct task* t = (void*)page_alloc(); if (!t) return NULL;
    t->allocation = stack_take();
    if (!t->allocation) { page_free((uint64_t)t); return NULL; }
    t->kernel_rsp = t->allocation + STACK_BYTES - 64;
    ((uint64_t*)t->kernel_rsp)[6] = (uint64_t)task_start;
    t->started = native_clock_ns(); return t;
}
/* Everything an exited thread no longer uses (Linux do_exit: exit_mm, exit_files, exit_fs). What a wait or
 * ptrace report reads (credentials, identity, status, usage) stays until the zombie is reaped. */
static void release_exit_resources(struct task* t) {
    if (t == current || t->on_cpu) panic("releasing active task stack");
    if (t->descriptors) { fd_table_release(t->descriptors); t->descriptors = NULL; }
    if (t->filesystem) { filesystem_release(t->filesystem); t->filesystem = NULL; }
    if (t->signals) { signals_destroy(t->signals); t->signals = NULL; }
    if (t->seccomp_filters) { seccomp_filter_release(t->seccomp_filters); t->seccomp_filters = NULL; }
    t->seccomp_mode = 0;
    landlock_exit(t->pid); kabi_pdeath_forget(t->pid);
    if (t->space) { space_release(t->space); t->space = NULL; }
    if (t->allocation) { stack_give(t->allocation); t->allocation = 0; }
}
static void release_resources(struct task* t) {
    release_exit_resources(t);
    if (t->credentials) { credentials_destroy(t->credentials); t->credentials = NULL; }
    if (t->tracer_credentials) { credentials_destroy(t->tracer_credentials); t->tracer_credentials = NULL; }
    if (t->children_namespace) { pid_namespace_release(t->children_namespace); t->children_namespace = NULL; }
    if (t->identity) { pid_release(t->identity); t->identity = NULL; }
}
static void unlink_task(struct task* t) {
    struct task** link = &tasks; while (*link && *link != t) link = &(*link)->next;
    if (!*link || t->allocation || t == current || t->on_cpu) panic("invalid process reaping");
    *link = t->next; page_free((uint64_t)t);
}
void task_yield(void) {
    if (!current) return;
    struct task* t = current;
    uint64_t elapsed = native_clock_ns() - t->run_started;
    t->cpu_ns += elapsed; t->process->cpu_ns += elapsed;
    t->vruntime += elapsed * 1024 / task_weight(t);
    if (t->state == WAITING) { t->voluntary++; t->process->voluntary++; } else if (t->state == RUNNABLE) { t->involuntary++; t->process->involuntary++; }
    cpu_busy_ns[cpu_self()->index % MAX_CPUS] += elapsed;
    fpu_save(t->fpu);
    segments_switch_out(&t->seg, &t->fs, &t->gs); /* selectors, FS/GS bases (this CPU's loaded_fs/gs) */
    if (t->state == RUNNABLE) t->runnable_since = native_clock_ns();
    /* The big lock belongs to the task: release it while switched out, take it back on return
     * (this CPU's scheduler loop runs without it; we may resume on another CPU). */
    bool big = kernel_lock_held();
    if (big) kernel_lock_release();
    switch_context(&t->kernel_rsp, scheduler_rsp);
    if (big) kernel_lock_acquire();
}
/* Sleep on `channel` (big lock held by the caller). WAITING is published before the big lock is
 * released in task_yield, so a waker that needs only sched_lock cannot be missed. */
int task_wait_channel(void* channel) {
    if (!current || !channel) return -38;
    if (task_exiting() || signals_interrupt_pending()) return -4;
    sched_lock();
    current->channel = channel; current->state = WAITING; rq_remove(current); wait_link(current);
    sched_unlock();
    task_yield();
    sched_lock(); wait_unlink(current); current->channel = NULL; sched_unlock();
    if (!current->io_free) block_io_barrier();   /* what follows may be filesystem code: not inside another task's disk wait */
    return task_exiting() || signals_interrupt_pending() ? -4 : 0;
}
/* A killable sleep (Linux TASK_KILLABLE): only exit or a fatal signal ends it, other signals leave it waiting. */
bool signals_fatal_pending(void);
int task_wait_channel_killable(void* channel) {
    if (!current || !channel) return -38;
    if (task_exiting() || signals_fatal_pending()) return -4;
    sched_lock();
    current->channel = channel; current->state = WAITING; rq_remove(current); wait_link(current);
    sched_unlock();
    task_yield();
    sched_lock(); wait_unlink(current); current->channel = NULL; sched_unlock();
    if (!current->io_free) block_io_barrier();
    return task_exiting() || signals_fatal_pending() ? -4 : 0;
}
int task_wait_until(void* channel, uint64_t deadline) {
    if (!current) return -38;
    if (deadline && native_clock_ns() >= deadline) return -110;
    sched_lock(); current->deadline = deadline; current->timed_out = false; timed_link(current); sched_unlock();
    int result = task_wait_channel(channel);
    sched_lock(); timed_unlink(current); current->deadline = 0; sched_unlock();
    return current->timed_out ? -110 : result;
}
void task_wake_channel(void* channel) {
    sched_lock();
    for (struct task *t = wait_buckets[wait_hash((uint64_t)(uintptr_t)channel)], *next; t; t = next) {
        next = t->w_next;
        if (t->state == WAITING && t->channel == channel) task_ready(t);
    }
    sched_unlock();
}
static void release_vfork(struct task* child) {
    if (child->vfork_parent) {
        child->vfork_parent->vfork_child = NULL;
        task_wake_channel(child); child->vfork_parent = NULL;
    }
}
static void clear_child_tid(void) {
    if (current->clear_tid && accessible(current->clear_tid, 4, true)) {
        uput32(current->clear_tid, 0);
        futex_wake(current->clear_tid, false, 1, 0xFFFFFFFFU);
    }
    current->clear_tid = 0;
}
static void notify_parent(struct process* p) {
    struct process* parent = find_process(p->parent); struct task* recipient = NULL;
    unsigned number = p->exit_signal <= 64 ? p->exit_signal : 0;   /* clone/clone3 exit_signal; 0: no signal */
    for (struct task* other = tasks; parent && other; other = other->next)
        if (other->process == parent && other->state != ZOMBIE) {
            recipient = other; if (!signals_masked(other->signals, number)) break;
        }
    if (recipient) {
        unsigned signal = p->status & 127;
        if (number) signals_enqueue(recipient->signals, number, signal ? 2 : 1, pid_number(p->identity, recipient->identity->space),
                                    p->uid, signal ? (int)signal : p->status >> 8, true);
        if (number == 17 && signals_reap_children(recipient->signals)) p->parent = 0;   /* autoreap: SIGCHLD only (Linux) */
        wake_process(parent);
    } else p->parent = 0;
}
static void terminate(void) {
    struct task* t = current;
    if (!t) panic("task exit outside user execution");
    clear_child_tid();
    t->status = user_termination_signal ? (int)user_termination_signal : (user_result & 255) << 8;
    struct process* p = t->process;
    if (t->pid == p->pid) p->uid = task_kernel_uid();
    if (user_termination_signal && !p->exiting) { p->status = t->status; p->exiting = true; wake_process(p); }
    if (!p->exiting && t->pid == p->pid) p->status = t->status;
    for (struct task* other = tasks; other; other = other->next) if (other->tracer == t) ptrace_untrace(other); /* tracer gone: detach */
    if (t->tracer) {   /* the tracer collects this exit (a leader traced by its parent's group: the process report does) */
        struct task* tracer = t->tracer; unsigned signal;
        if (p->exiting) t->status = p->status;
        signal = t->status & 127;
        t->traced_exit = t->pid != p->pid || tracer->process->pid != p->parent;
        if (t->traced_exit) signals_enqueue(tracer->signals, 17, signal ? 2 : 1, pid_number(t->identity, tracer->identity->space),
                                            credentials_kernel_id(t->credentials, false, 0), signal ? (int)signal : t->status >> 8, true);
        wake_process(tracer->process);
    }
    /* Linux exit_mm: the last thread drops the mappings (shm_nattch, mapped files) while it can still sleep, before any
     * waitpid can see the exit; the page tables go when the zombie is released */
    if (p->threads == 1 && t->space == space_current()) {
        /* that is filesystem work (shared-map write-back, orphan disposal): wait out another task's disk wait first,
         * also when the last call was I/O-free (a task killed in epoll_wait skipped the barrier) - blockio.c */
        t->io_free = false; block_io_barrier();
        space_exit_mappings();
    }
    sched_lock(); t->state = ZOMBIE; rq_remove(t); timed_unlink(t); futex_unlink(t); wait_unlink(t); sched_unlock();
    zombies_pending = true;
    release_vfork(t);
    if (!p->threads) panic("thread group lifetime underflow");
    if (!--p->threads) {
        native_exit_trace(NULL, p->status);
        file_locks_exit(p->pid);
        posix_timers_exit(p->pid);   /* POSIX timers and itimers end with the process (posix_timers.c) */
        sysvipc_exit(p->pid);        /* SEM_UNDO, shm_rmid_forced (sysvipc.c) */
        struct pid_namespace* space = p->identity->space;
        if (space != pid_namespace_root() && space->init == p->pid) {
            space->dead = true;
            for (struct process* member = processes; member; member = member->next)
                if (member != p && member->threads && pid_namespace_contains(space, member->identity->space)) {
                    member->exiting = true; member->status = 9; wake_process(member);
                }
        }
        struct process* reaper = NULL;
        for (struct pid_namespace* ns = space; ns && !reaper; ns = ns->parent) {
            struct process* candidate = find_process(ns->init);
            if (candidate && candidate != p && candidate->threads && !candidate->exiting) reaper = candidate;
        }
        uint64_t resident = space_resident_pages(t->space) * 4; if (resident > p->maxrss_kb) p->maxrss_kb = resident;
        if (p->pid == p->session && p->terminal) terminal_session_exit(p->terminal); /* the session loses its terminal */
        p->terminal = NULL; orphan_hangup(p->group);
        for (struct process* child = processes; child; child = child->next) if (child->parent == p->pid) {
            child->parent = reaper ? reaper->pid : 0; child->exit_signal = 17;   /* Linux reparent_leader */
            for (struct task* c = tasks; c; c = c->next) {   /* PR_SET_PDEATHSIG (kabi.c): the parent is gone */
                unsigned number = c->process == child && c->state != ZOMBIE ? kabi_pdeath_signal(c->pid) : 0;
                if (number) { signals_enqueue(c->signals, number, 0, 0, 0, 0, true); wake_process(child); }
            }
            if (!child->threads) notify_parent(child); else orphan_hangup(child->group);
        }
        notify_parent(p);
        pidfd_process_changed(p->identity);   /* pidfds of the process become readable (pidfd.c) */
    }
    task_yield(); panic("resumed terminated process");
}
/* ---- kguard: contain a recoverable kernel-mode exception (a Linux-style oops) ----
 * A stray kernel access (e.g. an SMAP violation outside a uaccess window) or a kernel-stack overflow that happens
 * while a task runs a syscall is killed like a SIGKILL, keeping the rest of the system alive, instead of panicking.
 * Containment is only safe when a task is to blame (not an IRQ or early boot) and the CPU holds nothing but the big
 * lock and that task's own address-space lock; any other held lock cannot be unwound, so the caller panics. */
bool task_kguard_containable(void) {
    struct cpu* c = cpu_self();
    if (!c->task || !kernel_lock_held()) return false;   /* no task context (IRQ/boot) or we do not own the kernel */
    space_unlock_held();                                  /* drop the task's own vm_lock (taken with the big lock) */
    return spin_held_count() == 0;                        /* any other leaf lock held: not safe to unwind -> panic */
}
void task_kguard_kill(void) {
    user_termination_signal = 9; user_result = 139;       /* SIGKILL the whole thread group, like a fatal signal */
    terminate();                                          /* noreturn: reaps this thread and switches to the scheduler */
}
/* A fault address or faulting rsp inside the current task's kernel-stack guard page (process.c lays the guard at the
 * base of the STACK_BYTES allocation): a kernel-stack overflow, delivered as #PF or (when the push itself faults) #DF. */
bool task_kernel_stack_overflow(uint64_t fault_address, uint64_t rsp) {
    struct task* t = current; if (!t || !t->allocation) return false;
    uint64_t base = t->allocation;
    return (fault_address >= base && fault_address < base + 4096ULL) || (rsp >= base && rsp < base + 2 * 4096ULL);
}
/* Device work that the BSP's scheduler loop normally does, run from a PIC interrupt that arrived
 * while user code ran (that loop may not run for a while when other CPUs take the other tasks). */
static void bsp_device_polls(void) {
    static uint64_t last; uint64_t now = native_clock_ns();
    if (!native_nic_rx_pending() && !ip_loopback_pending() && !virtio_gpu_busy() && now - last < 2000000ULL) return;
    last = now;
    struct task* running = current; current = NULL; task_context_reload();
    native_network_poll(); sound_poll(); virtio_gpu_poll(); native_io_poll(); native_gc_poll();
    current = running; task_context_reload();
}
/* ITIMER_REAL: expired timers send SIGALRM to their process and re-arm with their interval, in phase with the
 * first expiry (Linux hrtimer_forward: missed periods are skipped). Times saturate at KTIME_MAX, never wrap. */
#define KTIME_MAX 0x7FFFFFFFFFFFFFFFULL
static uint64_t timeval_ns(int64_t seconds, int64_t microseconds) {
    return (uint64_t)seconds >= KTIME_MAX / 1000000000ULL ? KTIME_MAX : (uint64_t)seconds * 1000000000ULL + (uint64_t)microseconds * 1000ULL;
}
static uint64_t deadline_after(uint64_t now, uint64_t delay) { return now + delay > KTIME_MAX ? KTIME_MAX : now + delay; }
static void alarms_expire(uint64_t now) {
    for (struct process* p = processes; p; p = p->next) {
        if (!p->alarm_deadline || !p->threads) continue;
        if (now >= p->alarm_deadline) {
            uint64_t next = p->alarm_interval ? p->alarm_deadline + p->alarm_interval : 0;
            if (next && next <= now) next += ((now - next) / p->alarm_interval + 1) * p->alarm_interval;
            p->alarm_deadline = next > KTIME_MAX ? KTIME_MAX : next;
            struct task* recipient = NULL;
            for (struct task* t = tasks; t; t = t->next) if (t->process == p && t->state != ZOMBIE) { recipient = t; if (!signals_masked(t->signals, 14)) break; }
            if (recipient && !signals_enqueue(recipient->signals, 14, 0x80, 0, 0, 0, true)) wake_process(p);
        }
        if (p->alarm_deadline) { sched_lock(); if (p->alarm_deadline < next_deadline) next_deadline = p->alarm_deadline; sched_unlock(); }
    }
}
/* alarm (37), getitimer (36), setitimer (38); only ITIMER_REAL exists here (ITIMER_VIRTUAL/PROF: EINVAL). */
int64_t task_itimer(uint64_t call, uint64_t which, uint64_t value, uint64_t old) {
    if (!current) return -3;
    struct process* p = current->process; uint64_t now = native_clock_ns();
    /* an armed timer never reads as disarmed {0,0}: at least 1 us, also once expired but not yet processed by the
     * tick (Linux itimer_get_remtime) */
    uint64_t remaining = !p->alarm_deadline ? 0 : p->alarm_deadline > now + 1000 ? p->alarm_deadline - now : 1000;
    if (call == 37) {   /* alarm(unsigned seconds): previous remaining time, rounded to the nearest second (at least 1) */
        uint64_t previous = remaining / 1000000000ULL, fraction = remaining % 1000000000ULL;
        if ((!previous && fraction) || fraction >= 500000000ULL) previous++;
        uint32_t seconds = (uint32_t)which;
        p->alarm_interval = 0; p->alarm_deadline = seconds ? now + seconds * 1000000000ULL : 0;
        if (p->alarm_deadline) { sched_lock(); if (p->alarm_deadline < next_deadline) next_deadline = p->alarm_deadline; sched_unlock(); }
        return (int64_t)(uint32_t)previous; /* unsigned int, as on Linux */
    }
    if (which == 1 || which == 2) return posix_itimer_cpu(call, which, value, old);   /* ITIMER_VIRTUAL/PROF (posix_timers.c) */
    if (which != 0) return -22;
    uint64_t out = call == 36 ? value : old;
    if (call == 38 && value) {
        if (!accessible(value, 32, false)) return -14;
        int64_t v[4]; copy_from_user(v, (void*)value, sizeof(v));
        if (v[0] < 0 || v[1] < 0 || v[1] >= 1000000 || v[2] < 0 || v[3] < 0 || v[3] >= 1000000) return -22;
        if (out && !accessible(out, 32, true)) return -14;
        if (out) { int64_t o[4] = {(int64_t)(p->alarm_interval / 1000000000), (int64_t)(p->alarm_interval % 1000000000 / 1000),
                                    (int64_t)(remaining / 1000000000), (int64_t)(remaining % 1000000000 / 1000)}; copy_to_user((void*)out, o, sizeof(o)); }
        uint64_t interval = timeval_ns(v[0], v[1]), delay = timeval_ns(v[2], v[3]);
        p->alarm_interval = delay ? interval : 0; p->alarm_deadline = delay ? deadline_after(now, delay) : 0;
        if (p->alarm_deadline) { sched_lock(); if (p->alarm_deadline < next_deadline) next_deadline = p->alarm_deadline; sched_unlock(); }
        return 0;
    }
    if (call == 38 && !value) return -14;
    if (!out) return call == 36 ? -14 : 0;
    if (!accessible(out, 32, true)) return -14;
    int64_t o[4] = {(int64_t)(p->alarm_interval / 1000000000), (int64_t)(p->alarm_interval % 1000000000 / 1000),
                    (int64_t)(remaining / 1000000000), (int64_t)(remaining % 1000000000 / 1000)};
    copy_to_user((void*)out, o, sizeof(o)); return 0;
}
static bool preempt_due(void) { sched_lock(); bool due = should_preempt(); sched_unlock(); return due; }
#define FUTEX_SLOW ((int64_t)1 << 62)
static int64_t task_futex_fast(struct registers* r);
uint64_t task_next_deadline(void) { return __atomic_load_n(&next_deadline, __ATOMIC_RELAXED); }   /* tick.c: the boot CPU's next timer event */
struct registers* trap64(struct registers* r) {
    struct registers* entry = r;
    if (smap_enabled) __asm__ volatile("clac" ::: "cc", "memory");   /* SMAP: a ring-3 AC or an interrupted copy window ends here (iretq restores it) */
    extern uint8_t user_read32_fault[], user_read32_fixup[];
    if (r->vector == 14 && !USER_MODE(r) && r->rip == (uint64_t)user_read32_fault) { r->rip = (uint64_t)user_read32_fixup; return r; }
    extern uint8_t hw_rdmsr_fault[], hw_rdmsr_fixup[];   /* hwinfo.c: an MSR the CPU does not have */
    if (r->vector == 13 && !USER_MODE(r) && r->rip == (uint64_t)hw_rdmsr_fault) { r->rip = (uint64_t)hw_rdmsr_fixup; return r; }
    extern uint8_t apic_wrmsr_fault[], apic_wrmsr_fixup[];   /* apic.c: a hypervisor refusing x2APIC mode */
    if (r->vector == 13 && !USER_MODE(r) && r->rip == (uint64_t)apic_wrmsr_fault) { r->rip = (uint64_t)apic_wrmsr_fixup; return r; }
    /* Stale user selector: a kernel segment load retries with null; a faulting iretq becomes a user fault. */
    if ((r->vector == 11 || r->vector == 12 || r->vector == 13) && !USER_MODE(r) && segments_fixup(r)) return r;
    if (r->vector >= 32 && r->vector < 256 && r->vector != 128) { void tick_trap_count(unsigned vector); tick_trap_count((unsigned)r->vector); }   /* /proc/interrupts (tick.c) */
    if (r->vector == VEC_TLB) { smp_tlb_ipi(); if (USER_MODE(r) && current) segments_user_return(current->space); return r; }
    if (r->vector == VEC_SPURIOUS) return r;
    bool big = false; /* this task holds the big lock while in here */
    if (r->vector == VEC_LAPIC_TIMER || r->vector == VEC_WAKE) {
        if (r->vector == VEC_WAKE) smp_wake_ipi(); else lapic_eoi();
        cpufreq_tick();   /* this CPU's pending performance request (cpufreq.c) */
        if (!USER_MODE(r)) { /* halted, spinning for a lock, or in a syscall: re-checked on the way out */
            if (r->vector == VEC_WAKE) cpu_self()->resched = true;
            return r;
        }
        cpu_self()->resched = false;
        kstat.irqs++;
        if (preempt_due()) task_yield();
        if (signals_maybe_pending(current->signals) || task_exiting()) { kernel_lock_acquire(); big = true; r = signal_dispatch(r); }
        goto leave;
    }
    if (r->vector == 128 && r->rax == 202 && USER_MODE(r) && !current->seccomp_mode) {   /* seccomp tasks take the slow path so futex is filtered */
        int64_t result = task_futex_fast(r);
        if (result != FUTEX_SLOW) {
            kstat.syscalls++; r->rax = (uint64_t)result;
            if (result == -4 || signals_maybe_pending(current->signals) || task_exiting()) {
                kernel_lock_acquire(); big = true; r = native_syscall_finish(r, 202);
            }
            goto leave;
        }
    }
    if (r->vector == 14 && USER_MODE(r)) {
        uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        if (space_fault_fast(current->space, cr2, r->error)) { /* counters without the big lock: other threads may fault too */
            __atomic_fetch_add(&kstat.faults, 1, __ATOMIC_RELAXED); current->faults++; __atomic_fetch_add(&current->process->faults, 1, __ATOMIC_RELAXED);
            goto leave;
        }
    }
    bool took = !kernel_lock_held();
    if (took) kernel_lock_acquire();
    big = took;
    if (r->vector == 128 || r->vector == IA32_SYSCALL_VECTOR) kstat.syscalls++; else if (r->vector == 14) { kstat.faults++; if (current && USER_MODE(r)) { current->faults++; current->process->faults++; } } else if (r->vector >= 32) kstat.irqs++;
    if (irq_device_vector((unsigned)r->vector)) {
        irq_dispatch((unsigned)r->vector); /* registered handlers: PS/2 input, NIC, MSI devices (irq.c) */
        uint64_t now = native_clock_ns();
        if (now >= next_deadline && !pm_snapshotting()) {   /* hibernation: no wake-ups while memory is saved */
            sched_lock();
            next_deadline = ~0ULL;
            for (struct task* t = timed_waiters; t; t = t->tw_next) {
                if (t->state != WAITING || !t->deadline) continue;
                if (now >= t->deadline) { t->timed_out = true; task_ready(t); }
                else if (t->deadline < next_deadline) next_deadline = t->deadline;
            }
            sched_unlock();
            alarms_expire(now);
            posix_timers_expire(now);   /* POSIX timers, timerfd, CPU-time itimers (posix_timers.c) */
        }
        irq_eoi((unsigned)r->vector); /* 8259 or local APIC, by interrupt mode */
        if (!USER_MODE(r)) { if (took) kernel_lock_release(); return r; }
        if (cpu_count > 1) bsp_device_polls();
        if (preempt_due()) task_yield();
        r = signal_dispatch(r);
    } else {
        r = native_dispatch(r);
        if (!USER_MODE(entry)) { if (took) kernel_lock_release(); return r; } /* kernel-mode exception */
    }
leave:
    if (!r || task_exiting()) {
        if (!big) { kernel_lock_acquire(); big = true; }
        if (!r) native_exit_trace(entry, user_termination_signal ? user_termination_signal : (user_result & 255) << 8);
        terminate();
    }
    if (cpu_self()->resched) { cpu_self()->resched = false; if (preempt_due()) task_yield(); }
    r->flags = (r->flags & ~((3ULL << 12) | (1ULL << 14) | (1ULL << 18))) | 0x202;
    if (big) kernel_lock_release(); /* back to user mode (possibly on another CPU than the one that trapped) */
    serial_drain(); /* console output this syscall queued (its lock spin may acknowledge an LDT shootdown) */
    segments_user_return(current->space); /* last, after any such acknowledgement: this CPU's LDT and DS/ES match the task's */
    return r;
}
int64_t task_clone(struct registers* r) {
    kstat.forks++;
    const uint64_t supported = 0x78000000ULL | 0x01000000ULL | 0x003D0000ULL | 0x4000 | 0x1000 | 0xF00 | 0xFF;  /* +CLONE_NEWNET/NEWIPC, CLONE_PIDFD */
    uint64_t flags = r->rax == 57 ? 17 : r->rax == 58 ? 0x4111 : (uint32_t)r->rdi;   /* Linux: the upper half is ignored */
    uint64_t stack = r->rax == 57 || r->rax == 58 ? 0 : r->rsi;
    bool thread = (flags & 0x10000) != 0;
    if (!current) return -38;
    /* exit signal (low byte): any for a process (none when 0 or not a valid signal), ignored for a thread (Linux) */
    if ((flags & 0x1000) && (thread || (!current->clone3 && (flags & 0x00100000)))) return -22;   /* CLONE_PIDFD */
    if (flags & ~supported) {
        char number[24]; uint_to_str((uint32_t)flags, number);
        vga_print("[64] Unsupported clone flags="); vga_print(number); vga_print("\n"); return -38;
    }
    if (((flags & 0x800) && !(flags & 0x100)) || (thread && (!(flags & 0x800) || (flags & 0x4000)))) return -22;
    if ((flags & 0x10000000) && (flags & (0x10000 | 0x200))) return -22;
    if ((flags & 0x10000000) && filesystem_chrooted()) return -1;
    if (thread && ((flags & 0x20000000) || current->children_namespace != task_pid_namespace())) return -22;
    if ((flags & 0x08040000) == 0x08040000) return -22;   /* CLONE_NEWIPC | CLONE_SYSVSEM */
    if (flags & 0x48000000) {   /* CLONE_NEWNET / CLONE_NEWIPC: per-process network / IPC namespaces (namespaces.c) */
        if (thread) return -22;
        /* CAP_SYS_ADMIN is required in the user namespace the netns is created in; when CLONE_NEWUSER is combined
         * the new userns grants it (Linux), so only an unpaired CLONE_NEWNET needs the caller to already hold it. */
        if (!(flags & 0x10000000) && !credentials_capable(current->credentials, credentials_namespace(current->credentials), 21)) return -1;
    }
    if ((flags & 0x80000) && r->r8 && !user_range(r->r8, 1)) return -1;
    if (stack && !user_range(stack, 1)) return -14;
    if ((flags & 0x00100000) && !accessible(r->rdx, 4, true)) return -14;
    if ((flags & 0x01000000) && !user_range(r->r10, 4)) return -14;
    if (!thread && nproc_exceeded()) return -11;   /* RLIMIT_NPROC */
    reclaim_kernel_room((flags & 0x100) ? 8 : vm_fork_pages());   /* under memory pressure the copy must not fail (swap.h) */
    struct task* child = allocate(); if (!child) return -12;
    int credential_error;
    child->credentials = credentials_clone(current->credentials, (flags & 0x10000000) != 0, &credential_error);
    if (!child->credentials) { release_resources(child); page_free((uint64_t)child); return credential_error; }
    if (flags & 0x20000000) {
        child->children_namespace = pid_namespace_create(current->children_namespace, child->credentials, &credential_error);
        if (!child->children_namespace) { release_resources(child); page_free((uint64_t)child); return credential_error; }
    } else { child->children_namespace = current->children_namespace; pid_namespace_retain(child->children_namespace); }
    child->identity = pid_allocate(child->children_namespace); if (!child->identity) goto failed;
    child->pid = child->identity->numbers[0];
    child->process = thread ? current->process : process_new(child->identity, current->process); if (!child->process) goto failed;
    if (!thread) child->process->exit_signal = (uint8_t)(flags & 255);
    if (!thread) for (unsigned kind = 0; kind < 2; kind++) if (flags & (kind == NSOBJ_NET ? 0x40000000ULL : 0x08000000ULL)) {
        /* owned by the child's user namespace: the new one when CLONE_NEWUSER is combined */
        int slot = nsobj_create(kind, credentials_namespace(child->credentials)); if (slot < 0) goto failed;
        nsobj_release(kind, child->process->ns_slot[kind]); child->process->ns_slot[kind] = (uint32_t)slot;
    }
    if (flags & 0x100) { child->space = current->space; space_retain(child->space); }
    else { child->space = space_clone(current->space); if (!child->space) goto failed; }
    child->descriptors = fd_table_clone(current->descriptors, (flags & 0x400) != 0); if (!child->descriptors) goto failed;
    child->filesystem = filesystem_clone(current->filesystem, (flags & 0x200) != 0); if (!child->filesystem) goto failed;
    child->signals = signals_create(current->signals, (flags & 0x800) != 0); if (!child->signals) goto failed;
    if (thread && !signals_share_pending(child->signals, current->signals)) goto failed;
    if ((flags & 0x100) && !(flags & 0x4000)) signals_disable_altstack(child->signals);
    if (current->clone3 && current->clone3->clear_sighand) signals_reset_handlers(child->signals);   /* CLONE_CLEAR_SIGHAND */
    /* shortcut: SysV semaphore state is immutable until those mutation syscalls exist. */
    if (flags & 0x01000000) {
        space_activate(child->space);
        bool ok = accessible(r->r10, 4, true);
        if (ok) uput32(r->r10, pid_number(child->identity, child->identity->space));
        space_activate(current->space);
        if (!ok) { release_resources(child); if (!thread) process_reap(child->process); page_free((uint64_t)child); return -14; }
    }
    child->initial = current->clone3 ? *current->clone3->frame : *r; child->initial.rax = 0;   /* clone3: the caller's registers */
    if (stack) child->initial.rsp = stack;
    child->initial.flags = (child->initial.flags & ~((3ULL << 12) | (1ULL << 14) | (1ULL << 18))) | 0x202;
    child->fs = flags & 0x80000 ? r->r8 : read_msr(0xC0000100); child->gs = read_msr(0xC0000101);
    segments_clone(&child->seg, &current->seg, (flags & 0x80000) != 0); /* selectors + TLS slots (CLONE_SETTLS: FS 0) */
    fpu_save(child->fpu);
    if (flags & 0x00200000) child->clear_tid = r->r10;
    strcpy(child->executable, current->executable);
    /* the policy is inherited (SCHED_RESET_ON_FORK is not stored by sched_setscheduler; clone bit 0x40000000 is
     * CLONE_NEWNET, which must not reset it) */
    child->nice = current->nice; child->policy = current->policy; child->rt_priority = current->rt_priority;
    child->runnable_since = native_clock_ns(); child->vruntime = current->vruntime > min_vruntime ? current->vruntime : min_vruntime;
    child->executable_inode = current->executable_inode; child->executable_deleted = current->executable_deleted;
    child->seccomp_mode = current->seccomp_mode; child->seccomp_filters = current->seccomp_filters; /* seccomp survives fork/clone */
    seccomp_filter_retain(child->seccomp_filters);
    landlock_fork(current->pid, child->pid);   /* the Landlock domain too (landlock.c) */
    child->persona = current->persona;   /* personality(2) is inherited across fork/clone */
    child->mempolicy = current->mempolicy; child->ioprio = current->ioprio;   /* NUMA policy, I/O priority (kabi.c) */
    if (flags & 0x00100000) {
        if (!accessible(r->rdx, 4, true)) { release_resources(child); if (!thread) process_reap(child->process); page_free((uint64_t)child); return -14; }
        uput32(r->rdx, pid_number(child->identity, task_pid_namespace()));
    }
    if (flags & 0x1000) {   /* CLONE_PIDFD: the parent's pidfd exists before the child can run (pidfd.c) */
        int error = pidfd_clone_install(child->identity, current->clone3 ? current->clone3->pidfd : r->rdx);
        if (error) { release_resources(child); if (!thread) process_reap(child->process); page_free((uint64_t)child); return error; }
    }
    if (thread) child->process->threads++;
    pid_publish(child->identity);
    child->next = tasks; tasks = child;
    sched_lock(); rq_add(child); smp_wake_idle(); sched_unlock();
    uint32_t pid = pid_number(child->identity, task_pid_namespace());
    if (flags & 0x4000) {
        child->vfork_parent = current; current->vfork_child = child;
        while (current->vfork_child) {
            if (task_exiting()) { current->vfork_child->vfork_parent = NULL; current->vfork_child = NULL; break; }
            sched_lock(); current->channel = child; current->state = WAITING; rq_remove(current); wait_link(current); sched_unlock();
            task_yield();
            sched_lock(); wait_unlink(current); current->channel = NULL; sched_unlock();
        }
    }
    return pid;
failed:
    release_resources(child); if (child->process && !thread) process_reap(child->process); page_free((uint64_t)child); return -12;
}
int task_unshare(uint64_t flags) {
    if (!flags) return 0;
    if (flags & ~0x78000200ULL) return -38;   /* +CLONE_NEWNET/NEWIPC */
    if (!current || ((flags & 0x78000000) && current->process->threads != 1)) return -22;
    /* CAP_SYS_ADMIN in the user namespace the new net/IPC namespace is created in (the new one with CLONE_NEWUSER) */
    if ((flags & 0x48000000) && !(flags & 0x10000000) && !credentials_capable(current->credentials, credentials_namespace(current->credentials), 21)) return -1;
    if ((flags & 0x10000000) && filesystem_chrooted()) return -1;
    if ((flags & 0x20000000) && current->children_namespace != task_pid_namespace()) return -22;
    int error; struct credentials* next = credentials_clone(current->credentials, (flags & 0x10000000) != 0, &error);
    if (!next) return error;
    struct pid_namespace* space = NULL;
    if (flags & 0x20000000) {
        space = pid_namespace_create(task_pid_namespace(), next, &error);
        if (!space) { credentials_destroy(next); return error; }
    }
    struct filesystem_state* filesystem = NULL;
    if (flags & 0x10000200) {
        filesystem = filesystem_clone(current->filesystem, false);
        if (!filesystem) { if (space) pid_namespace_release(space); credentials_destroy(next); return -12; }
    }
    int slots[2] = {-1, -1};
    for (unsigned kind = 0; kind < 2; kind++) if (flags & (kind == NSOBJ_NET ? 0x40000000ULL : 0x08000000ULL)) {
        slots[kind] = nsobj_create(kind, credentials_namespace(next));
        if (slots[kind] < 0) {
            error = slots[kind]; if (kind && slots[0] >= 0) nsobj_release(NSOBJ_NET, (unsigned)slots[0]);
            if (filesystem) filesystem_release(filesystem);
            if (space) pid_namespace_release(space);
            credentials_destroy(next); return error;
        }
    }
    struct credentials* previous = current->credentials; current->credentials = next;
    if (space) { pid_namespace_release(current->children_namespace); current->children_namespace = space; }
    for (unsigned kind = 0; kind < 2; kind++) if (slots[kind] >= 0) {
        nsobj_release(kind, current->process->ns_slot[kind]); current->process->ns_slot[kind] = (uint32_t)slots[kind];
    }
    if (filesystem) {
        struct filesystem_state* previous_fs = current->filesystem; current->filesystem = filesystem;
        filesystem_activate(filesystem); filesystem_release(previous_fs);
    }
    credentials_activate(next); credentials_destroy(previous); return 0;
}
int task_exec(struct registers* r, uint64_t entry, uint64_t stack, struct address_space* space, const char* executable, uint64_t inode, bool deleted) {
    if (!current) return -38;
    /* shortcut: sibling teardown during multithreaded exec still needs an atomic commit path. */
    if (current->process->threads != 1) return -38;
    struct descriptor_table* descriptors = fd_table_clone(current->descriptors, false); if (!descriptors) return -12;
    struct credentials* credentials = credentials_exec_traced(current->credentials, current->tracer ? current->tracer_credentials : NULL, space);
    if (!credentials) { fd_table_release(descriptors); return -12; }
    struct signal_state* signals = signals_exec(current->signals);
    if (!signals) { credentials_destroy(credentials); fd_table_release(descriptors); return -12; }
    struct filesystem_state* filesystem = filesystem_clone(current->filesystem, false);
    if (!filesystem) { signals_destroy(signals); credentials_destroy(credentials); fd_table_release(descriptors); return -12; }
    clear_child_tid();
    struct address_space* previous_space = current->space;
    struct descriptor_table* previous_descriptors = current->descriptors;
    struct signal_state* previous_signals = current->signals;
    struct credentials* previous_credentials = current->credentials;
    struct filesystem_state* previous_filesystem = current->filesystem;
    current->space = space; current->descriptors = descriptors; current->signals = signals;
    current->credentials = credentials;
    current->filesystem = filesystem; filesystem_activate(filesystem); filesystem_release(previous_filesystem);
    current->clear_tid = current->fs = current->gs = 0; current->process->executed = true;
    posix_timers_exec(current->process->pid);   /* POSIX timers end at exec, itimers stay (posix_timers.c) */
    pid_retain(current->process->identity); pid_release(current->identity); current->identity = current->process->identity;
    current->pid = current->process->pid;
    strcpy(current->executable, executable); native_set_executable(executable);
    current->executable_inode = inode; current->executable_deleted = deleted;
    space_activate(space); fd_table_activate(descriptors); signals_activate(signals);
    credentials_activate(credentials); credentials_destroy(previous_credentials);
    fd_close_exec(); fd_table_release(previous_descriptors); signals_destroy(previous_signals); space_release(previous_space);
    proc_release_dead_images();   /* /proc/PID/mem etc. bound to the old image stop pinning it */
    segments_exec(&current->seg); reset_user_fpu(); /* null selectors, FS/GS bases 0, TLS slots cleared */
    *r = (struct registers){.rip = entry, .rsp = stack, .cs = USER_CS, .ss = USER_DS, .flags = 0x202, .vector = 128};
    release_vfork(current); return 0;
}
/* options: WNOHANG 1, WSTOPPED/WUNTRACED 2, WEXITED 4, WCONTINUED 8; *event: 0 exited, 1 stopped, 2 continued. */
static int wait_child(int64_t pid, uint64_t group, uint64_t options, struct process** selected, int* event, struct task** traced) {
    if (!current) return -10;
    int interrupted = 0;
    for (;;) {
        bool found = false;
        for (struct task* t = tasks; t && pid >= -1 && pid != 0 && !group; t = t->next) {   /* ptrace stops are always reported */
            if (!t->tracer || t->tracer->process != current->process || (t->state == ZOMBIE && !t->traced_exit)) continue;
            uint32_t visible = task_visible_pid(t->pid);
            if (!visible || (pid > 0 && visible != (uint64_t)pid)) continue;
            found = true;
            if (t->state == ZOMBIE ? (options & 4) != 0 : t->traced_stop && !t->traced_reported) { *traced = t; return visible; }
        }
        for (struct process* t = processes; t; t = t->next) {
            uint32_t visible = task_visible_pid(t->pid);
            if (t->parent != task_pid() || !visible || (pid > 0 && visible != (uint64_t)pid) ||
                (group && task_visible_pid(t->group) != group)) continue;
            /* __WALL: every child; else __WCLONE selects exactly the "clone" children (exit signal not SIGCHLD) */
            if (!(options & 0x40000000) && (t->exit_signal != 17) != ((options & 0x80000000) != 0)) continue;
            found = true;
            int kind = !t->threads ? ((options & 4) ? 0 : -1) : t->report == 1 && (options & 2) ? 1 : t->report == 2 && (options & 8) ? 2 : -1;
            if (kind >= 0) { *selected = t; *event = kind; return visible; }
        }
        if (!found) return -10;
        if (options & 1) return 0;
        if (interrupted) return interrupted;
        interrupted = task_wait_channel(current);
    }
}
struct rusage64 { int64_t utime[2], stime[2], maxrss, ixrss, idrss, isrss, minflt, majflt, nswap, inblock, oublock, msgsnd, msgrcv, nsignals, nvcsw, nivcsw; };
_Static_assert(sizeof(struct rusage64) == 144, "struct rusage ABI");
/* CPU time is not split by privilege level: all of it is reported as user time. */
static void rusage_copy(uint64_t address, uint64_t cpu_ns, uint64_t maxrss_kb, uint64_t faults, uint64_t voluntary, uint64_t involuntary) {
    struct rusage64 usage = {0};
    usage.utime[0] = (int64_t)(cpu_ns / 1000000000); usage.utime[1] = (int64_t)(cpu_ns % 1000000000 / 1000);
    usage.maxrss = (int64_t)maxrss_kb; usage.minflt = (int64_t)faults; usage.nvcsw = (int64_t)voluntary; usage.nivcsw = (int64_t)involuntary;
    copy_to_user((void*)address, &usage, sizeof(usage));
}
static void child_rusage(uint64_t address, struct process* child) {
    rusage_copy(address, child->cpu_ns + child->child_cpu_ns, child->maxrss_kb > child->child_maxrss_kb ? child->maxrss_kb : child->child_maxrss_kb,
                child->faults + child->child_faults, child->voluntary + child->child_voluntary, child->involuntary + child->child_involuntary);
}
/* A waited-for child's usage (and that of its waited-for descendants) moves to RUSAGE_CHILDREN. */
static void reap_child(struct process* child) {
    struct process* p = current->process;
    p->child_cpu_ns += child->cpu_ns + child->child_cpu_ns; p->child_faults += child->faults + child->child_faults;
    p->child_voluntary += child->voluntary + child->child_voluntary; p->child_involuntary += child->involuntary + child->child_involuntary;
    uint64_t rss = child->maxrss_kb > child->child_maxrss_kb ? child->maxrss_kb : child->child_maxrss_kb;
    if (rss > p->child_maxrss_kb) p->child_maxrss_kb = rss;
    process_reap(child);
}
int64_t task_wait4(int64_t pid, uint64_t status, uint64_t options, uint64_t usage) {
    if (options & ~(11ULL | 0xE0000000ULL)) return -22;   /* __WNOTHREAD, __WALL, __WCLONE accepted */
    options &= 11ULL | 0xC0000000ULL;   /* __WALL/__WCLONE select clone children (wait_child) */
    if ((status && !accessible(status, 4, true)) || (usage && !accessible(usage, sizeof(struct rusage64), true))) return -14;
    uint64_t group = pid == 0 && current ? task_visible_pid(current->process->group) : pid < -1 ? 0ULL - (uint64_t)pid : 0;
    struct process* child = NULL; struct task* traced = NULL; int event = 0; int result = wait_child(pid, group, options | 4, &child, &event, &traced);
    if (result <= 0) return result;
    if ((status && !accessible(status, 4, true)) || (usage && !accessible(usage, sizeof(struct rusage64), true))) return -14; /* after the sleep */
    if (traced) {
        bool exited = traced->state == ZOMBIE;
        if (status) uput32(status, (uint32_t)(exited ? traced->status : traced->traced_signal << 8 | 0x7F));
        if (usage) { if (exited) rusage_copy(usage, traced->cpu_ns, 0, traced->faults, traced->voluntary, traced->involuntary); else clear_user(usage, sizeof(struct rusage64)); }
        if (exited) ptrace_collect(traced); else traced->traced_reported = true;
        return result;
    }
    if (status) uput32(status, (uint32_t)(event == 1 ? child->stop_signal << 8 | 0x7F : event == 2 ? 0xFFFF : child->status));
    if (usage) child_rusage(usage, child);
    if (event) child->report = 0; else reap_child(child);
    return result;
}
static int waitid_found;
int task_waitid_found(void) { return waitid_found; }
int task_waitid(uint64_t kind, uint64_t id, uint64_t address, uint64_t options, uint64_t usage) {
    if (kind > 2 || id > 0x7FFFFFFF || (kind == 1 && !id) ||
        (options & ~(0x100000FULL | 0xE0000000ULL)) || !(options & 14)) return -22;
    if ((address && !accessible(address, 128, true)) || (usage && !accessible(usage, sizeof(struct rusage64), true))) return -14;
    int64_t pid = kind == 1 ? (int64_t)id : -1;
    uint64_t group = kind != 2 ? 0 : id ? id : current ? task_visible_pid(current->process->group) : 0;
    struct process* child = NULL; struct task* traced = NULL; int event = 0; int result = wait_child(pid, group, options, &child, &event, &traced);
    if (result < 0) return result;
    waitid_found = result;   /* pidfd.c: waitid(P_PIDFD) on a non-blocking pidfd found nothing -> EAGAIN */
    if ((address && !accessible(address, 128, true)) || (usage && !accessible(usage, sizeof(struct rusage64), true))) return -14; /* after the sleep */
    if (traced) {   /* CLD_TRAPPED, or a tracee's exit (CLD_EXITED/CLD_KILLED; its process may be gone) */
        bool exited = traced->state == ZOMBIE; int s = traced->status;
        if (address) {
            uint32_t info[32] = {0}; info[0] = 17; info[2] = exited ? ((s & 127) ? 2 : 1) : 4; info[4] = result;
            info[5] = credentials_visible_id(exited ? credentials_kernel_id(traced->credentials, false, 0) : traced->process->uid, false);
            info[6] = exited ? ((s & 127) ? s & 127 : s >> 8) : traced->traced_signal; copy_to_user((void*)address, info, sizeof(info));
        }
        if (usage) { if (exited) rusage_copy(usage, traced->cpu_ns, 0, traced->faults, traced->voluntary, traced->involuntary); else clear_user(usage, sizeof(struct rusage64)); }
        if (!(options & 0x1000000)) { if (exited) ptrace_collect(traced); else traced->traced_reported = true; }
        return 0;
    }
    if (address) {
        uint32_t info[32] = {0};
        if (child) {
            info[0] = 17; info[4] = result; info[5] = credentials_visible_id(child->uid, false);
            if (event) { info[2] = event == 1 ? 5 : 6; info[6] = event == 1 ? child->stop_signal : 18; }
            else { info[2] = (child->status & 127) ? 2 : 1; info[6] = (child->status & 127) ? child->status & 127 : child->status >> 8; }
        }
        copy_to_user((void*)address, info, sizeof(info));
    }
    if (usage) { if (child) child_rusage(usage, child); else clear_user(usage, sizeof(struct rusage64)); }
    if (child && !(options & 0x1000000)) { if (event) child->report = 0; else reap_child(child); }
    return 0;
}
/* task_signal_info (rt_sigqueueinfo, pidfd_send_signal): a caller-built siginfo replaces the SI_USER/SI_TKILL record */
static const void* custom_info;
static int custom_enqueue(struct signal_state* state, bool group) {
    int result = signals_enqueue_info(state, custom_info, group, NULL); return result == 1 ? 0 : result;
}
int task_signal(int64_t pid, uint64_t number, int code) {
    if (number > 64) return -22;
    if (!current) return -3;
    int result = -3;
    for (struct process* p = processes; p; p = p->next) {
        uint32_t visible = task_visible_pid(p->pid);
        bool matches = visible && (pid > 0 ? (uint64_t)pid == visible : pid == 0 ? p->group == current->process->group :
                       pid == -1 ? p != current->process && visible != 1 : (uint64_t)task_visible_pid(p->group) == 0ULL - (uint64_t)pid);
        if (!matches) continue;
        if (!p->threads) { result = 0; continue; }
        struct task* recipient = NULL;
        for (struct task* t = tasks; t; t = t->next) if (t->process == p && t->state != ZOMBIE) {
            recipient = t; if (!signals_masked(t->signals, number)) break;
        }
        if (!recipient) panic("process without a live thread");
        struct task* leader = find(p->pid);
        if (!credentials_signal_allowed(leader && leader->state != ZOMBIE ? leader->credentials : recipient->credentials) &&
            !(number == 18 && p->session == current->process->session)) { /* SIGCONT may reach any process of the session */
            if (result) result = -1; continue;
        }
        struct pid_namespace* space = p->identity->space;
        if (space != pid_namespace_root() && space->init == p->pid && number && !signals_has_handler(recipient->signals, number) &&
            !((number == 9 || number == 19) && space != task_pid_namespace())) { result = 0; continue; }
        job_signal(p, (unsigned)number);
        int error = custom_info && number ? custom_enqueue(recipient->signals, true) :
                    signals_enqueue(recipient->signals, number, code, pid_number(current->process->identity, space), task_kernel_uid(), 0, true);
        if (!error) {
            result = 0;
            wake_process(p);
        } else if (result) result = error;
    }
    return result;
}
int task_thread_signal(int32_t group, int32_t tid, uint64_t number) {
    if (tid <= 0 || number > 64) return -22;
    struct task* target = find(task_internal_pid((uint32_t)tid));
    if (!target || target->state == ZOMBIE || (group && task_visible_pid(target->process->pid) != (uint32_t)group)) return -3;
    if (!credentials_signal_allowed(target->credentials)) return -1;
    struct pid_namespace* space = target->identity->space;
    if (space != pid_namespace_root() && space->init == target->process->pid && number && !signals_has_handler(target->signals, number) &&
        !((number == 9 || number == 19) && space != task_pid_namespace())) return 0;
    job_signal(target->process, (unsigned)number);
    int result = custom_info && number ? custom_enqueue(target->signals, false) :
                 signals_enqueue(target->signals, number, -6, pid_number(current->process->identity, space), task_kernel_uid(), 0, false);
    sched_lock();
    if (!result && target->state == WAITING) task_ready(target);
    else if (!result && target->on_cpu && target != current) kick(target);
    sched_unlock();
    return result;
}
static int futex_wake_key(const struct futex_key* key, int count, uint32_t mask) { /* sched_lock held */
    int woke = 0;
    for (struct task *t = futex_buckets[futex_hash(key)], *next; t && woke < count; t = next) {
        next = t->fx_next;
        if (t->state == WAITING && t->futex_waiting && (t->futex_mask & mask) && t->futex.kind == key->kind &&
            t->futex.domain == key->domain && t->futex.word == key->word) {
            t->futex_waiting = false; task_ready(t); woke++;
        }
    }
    return woke;
}
static int futex_wake(uint64_t address, bool private, int count, uint32_t mask) {
    if (count < 0 || !mask) return -22;
    struct futex_key key; int error = memory_futex_key(address, private, &key); if (error) return error;
    sched_lock(); int woke = futex_wake_key(&key, count, mask); sched_unlock();
    return woke;
}
/* Private FUTEX_WAIT/WAKE(_BITSET) without the big lock. Returns FUTEX_SLOW for the locked path:
 * shared futexes, timeouts, a word not mapped yet, a possibly pending signal, odd arguments. */
static int64_t task_futex_fast(struct registers* r) {
    unsigned operation = (unsigned)r->rsi & 127;
    struct task* t = current;
    if (!t || !(r->rsi & 128) || (r->rsi & ~255ULL) || (r->rdi & 3) || !user_range(r->rdi, 4)) return FUTEX_SLOW;
    uint32_t mask = operation == 9 || operation == 10 ? (uint32_t)r->r9 : 0xFFFFFFFFU;
    if (!mask) return FUTEX_SLOW;
    struct futex_key key = {(uint64_t)t->space, r->rdi, 0}; /* = memory_futex_key() for a private futex */
    if (operation == 1 || operation == 10) {
        if ((int32_t)r->rdx < 0) return FUTEX_SLOW;
        sched_lock(); int woke = futex_wake_key(&key, (int32_t)r->rdx, mask); sched_unlock();
        return woke;
    }
    if ((operation != 0 && operation != 9) || r->r10 || task_exiting() || signals_maybe_pending(t->signals)) return FUTEX_SLOW;
    uint32_t value;
    sched_lock();
    if (user_read32(r->rdi, &value)) { sched_unlock(); return FUTEX_SLOW; } /* fault it in under the big lock */
    if (value != (uint32_t)r->rdx) { sched_unlock(); return -11; }
    t->futex = key; t->futex_mask = mask; t->futex_waiting = true; futex_link(t);
    t->deadline = 0; t->timed_out = false;
    t->channel = &t->futex; t->state = WAITING; rq_remove(t); wait_link(t);
    if (task_exiting() || signals_maybe_pending(t->signals)) { /* raced with a signal or exit */
        wait_unlink(t); futex_unlink(t); t->channel = NULL; t->futex_waiting = false;
        t->state = RUNNABLE; rq_add(t); sched_unlock(); return FUTEX_SLOW;
    }
    sched_unlock();
    task_yield();
    sched_lock();
    wait_unlink(t); futex_unlink(t); t->channel = NULL;
    bool woken = !t->futex_waiting; t->futex_waiting = false;
    sched_unlock();
    if (woken) return 0;
    kernel_lock_acquire(); /* woken by a signal or exit: decide exactly as the locked path does */
    bool interrupted = task_exiting() || signals_interrupt_pending();
    kernel_lock_release();
    return interrupted ? -4 : 0;
}
int64_t task_futex(struct registers* r) {
    unsigned operation = (unsigned)r->rsi & 127;
    bool private = (r->rsi & 128) != 0;
    if (r->rsi & ~511ULL) return -22;
    if ((r->rsi & 256) && operation != 9) return -38;
    uint32_t mask = operation == 9 || operation == 10 ? (uint32_t)r->r9 : 0xFFFFFFFFU;
    if (!mask) return -22;
    if (operation == 1 || operation == 10) return futex_wake(r->rdi, private, (int32_t)r->rdx, mask);
    if (operation != 0 && operation != 9) return -38;
    if (!current) return -38;
    struct futex_key key; int error = memory_futex_key(r->rdi, private, &key); if (error) return error;
    uint64_t deadline = 0, now = native_clock_ns();
    if (r->r10) {
        if (!accessible(r->r10, 16, false)) return -14;
        int64_t timeout[2]; copy_from_user(timeout, (const void*)r->r10, 16);
        if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1000000000) return -22;
        uint64_t seconds = timeout[0], ns = timeout[1];
        uint64_t value = seconds > (~0ULL - ns) / 1000000000 ? ~0ULL : seconds * 1000000000 + ns;
        if (operation == 9) {
            uint64_t clock = r->rsi & 256 ? native_realtime_ns() : now;
            value = value > clock ? value - clock : 0;
        }
        deadline = value > ~0ULL - now ? ~0ULL : now + value;
    }
    if (task_exiting() || signals_interrupt_pending()) return -4;
    /* The word check and the queueing are one step under sched_lock: lockless wakers cannot slip in.
     * (memory_futex_key made the page present, and the big lock keeps it mapped.) */
    sched_lock();
    if (uget32(r->rdi) != (uint32_t)r->rdx) { sched_unlock(); return -11; }
    if (r->r10 && deadline <= now) { sched_unlock(); return -110; }   /* Linux: EAGAIN before ETIMEDOUT */
    current->futex = key; current->futex_mask = mask; current->futex_waiting = true; futex_link(current);
    current->deadline = deadline; current->timed_out = false; timed_link(current);
    current->channel = &current->futex; current->state = WAITING; rq_remove(current); wait_link(current);
    sched_unlock();
    task_yield();
    sched_lock();
    wait_unlink(current); current->channel = NULL;
    int result = !current->futex_waiting ? 0 : current->timed_out ? -110 : 1;
    futex_unlink(current); timed_unlink(current);
    current->futex_waiting = false; current->deadline = 0;
    sched_unlock();
    if (result == 1) result = task_exiting() || signals_interrupt_pending() ? -4 : 0;
    return result;
}
int64_t task_session(uint64_t call, int64_t pid, int64_t group) {
    if (!current) return -3;
    struct process* caller = current->process;
    if (call == 111) return task_visible_pid(caller->group);
    if (call == 112) {
        for (struct process* p = processes; p; p = p->next) if (p->group == caller->pid) return -1;
        pid_retain(caller->identity); pid_retain(caller->identity);
        pid_release(caller->group_id); pid_release(caller->session_id);
        caller->group_id = caller->session_id = caller->identity;
        caller->group = caller->session = caller->pid; caller->terminal = NULL; return task_visible_pid(caller->pid);
    }
    struct process* target = !pid ? caller : pid > 0 && pid <= 0x7FFFFFFF ? find_process(task_internal_pid((uint32_t)pid)) : NULL;
    if (!target && pid > 0 && pid <= 0x7FFFFFFF && (call == 121 || call == 124)) {
        struct task* t = find(task_internal_pid((uint32_t)pid)); if (t && t->state != ZOMBIE) target = t->process; /* a zombie's process may be reaped */
    }
    if (call == 121 || call == 124) return !target ? -3 : (int64_t)task_visible_pid(call == 121 ? target->group : target->session);
    if (pid < 0 || group < 0 || group > 0x7FFFFFFF) return -22;
    if (!target || (target != caller && target->parent != caller->pid)) return -3;
    if (target != caller && target->executed) return -13;
    if (target->session != caller->session || target->session == target->pid) return -1;
    if (!group) group = target->pid;
    else { group = task_internal_pid(group); if (!group) return -1; }
    if (group != target->pid) {
        bool exists = false;
        for (struct process* p = processes; p; p = p->next) if (p->group == (uint64_t)group && p->session == caller->session) exists = true;
        if (!exists) return -1;
    }
    struct pid_identity* identity = pid_lookup(group, pid_namespace_root());
    if (!identity) panic("process group without an identity");
    pid_retain(identity); pid_release(target->group_id); target->group_id = identity;
    target->group = group; return 0;
}
/* /proc/<pid>[/task/<tid>] process information (procext.c); `thread` selects per-thread values. */
bool task_info(uint32_t tid, bool thread, struct task_info* info) {
    struct task* t = find(tid);
    if (!t || t->state == ZOMBIE) return false;
    struct process* p = t->process; struct task* leader = find(p->pid);
    struct task* subject = thread || !leader || leader->state == ZOMBIE ? t : leader;
    memset(info, 0, sizeof(*info));
    info->pid = p->pid; info->tid = t->pid; info->ppid = p->parent; info->group = p->group; info->session = p->session;
    info->threads = p->threads; info->space = t->space; info->terminal = p->terminal; info->oom_score_adj = p->oom_score_adj;
    info->state = p->stopped ? 'T' : subject->state == RUNNABLE ? 'R' : subject->uninterruptible ? 'D' : 'S';
    if (subject->comm[0]) memcpy(info->name, subject->comm, 15);
    else { const char* base = subject->executable; for (const char* c = subject->executable; *c; c++) if (*c == '/') base = c + 1; strncpy(info->name, base, 15); }
    for (unsigned k = 0; k < 4; k++) {
        info->uid[k] = credentials_visible_id(credentials_kernel_id(t->credentials, false, k), false);
        info->gid[k] = credentials_visible_id(credentials_kernel_id(t->credentials, true, k), true);
    }
    uint64_t running = subject->on_cpu && subject->run_started ? native_clock_ns() - subject->run_started : 0;
    info->cpu_ns = (thread ? t->cpu_ns : p->cpu_ns) + running; info->start_ns = (leader ? leader : t)->started;
    info->faults = thread ? t->faults : p->faults; info->child_faults = p->child_faults; info->child_cpu_ns = p->child_cpu_ns;
    info->voluntary = thread ? t->voluntary : p->voluntary; info->involuntary = thread ? t->involuntary : p->involuntary;
    info->nice = t->nice; info->processor = t->cpu; return true;
}
void task_cpu_usage(uint64_t busy[MAX_CPUS], unsigned* running, unsigned* total) {
    for (unsigned c = 0; c < MAX_CPUS; c++) busy[c] = cpu_busy_ns[c];
    *running = *total = 0;
    for (struct task* t = tasks; t; t = t->next) if (t->state != ZOMBIE) { (*total)++; if (t->state == RUNNABLE) (*running)++; }
    for (unsigned c = 0; c < MAX_CPUS; c++) {
        struct task* t = cpus[c].task;
        if (t && t->run_started) busy[c] += native_clock_ns() - t->run_started;
    }
}
int task_set_oom_score(uint32_t tid, int value) {
    struct task* t = find(tid); if (!t || t->state == ZOMBIE) return -3;
    if (value < -1000 || value > 1000) return -22;
    if (value < t->process->oom_score_adj && !credentials_capable(credentials_current(), NULL, 24)) return -13;
    t->process->oom_score_adj = (int16_t)value; return 0;
}
/* The OOM killer (reclaim.c): the whole thread group exits as if killed by SIGKILL (status 9). */
int task_oom_kill(uint32_t pid) {
    struct process* p = find_process(pid); if (!p || !p->threads) return -3;
    if (!p->exiting) { p->exiting = true; p->status = 9; }
    wake_process(p); return 0;
}
bool task_process_alive(uint32_t pid) { struct process* p = find_process(pid); return p && p->threads; }
/* An OOM victim's memory is back only once its tasks were reaped: a zombie keeps its address space until then (and
 * its process record may already be gone, so zombies are matched by the space, never through t->process). */
bool task_space_held(struct address_space* space) {
    for (struct task* t = tasks; t; t = t->next) if (space && t->space == space) return true;
    return false;
}
void task_set_io_free(bool io_free) { if (current) current->io_free = io_free; }
uint64_t task_yields(void) { return current ? current->voluntary + current->involuntary : 0; } /* changes when the task slept */
/* Kernel threads (reclaim.c kswapd): scheduled like tasks but never in `tasks` (no PID, signals, reaping or /proc).
 * They start without the big lock and take it around their work (task_kernel_sleep waits without it). */
static struct process kernel_threads;
static void kernel_thread_start(void) {
    ((void (*)(void))current->initial.rip)();
    panic("kernel thread returned");
}
/* Sleep that signals do not end (Linux TASK_UNINTERRUPTIBLE): disk waits, the block I/O barrier. */
void task_sleep_uninterruptible(void* channel, uint64_t deadline) {
    if (!current) return;
    sched_lock();
    current->deadline = deadline; current->timed_out = false; timed_link(current);
    current->channel = channel; current->state = WAITING; rq_remove(current); wait_link(current); current->uninterruptible = true;
    sched_unlock();
    task_yield();
    sched_lock(); wait_unlink(current); timed_unlink(current); current->channel = NULL; current->deadline = 0; current->uninterruptible = false; sched_unlock();
}
void task_kernel_sleep(void* channel, uint64_t deadline) {
    if (!current || current->process != &kernel_threads || kernel_lock_held()) panic("kernel thread sleep");
    sched_lock();
    current->deadline = deadline; current->timed_out = false; timed_link(current);
    current->channel = channel; current->state = WAITING; rq_remove(current); wait_link(current);
    sched_unlock();
    task_yield();
    sched_lock(); wait_unlink(current); timed_unlink(current); current->channel = NULL; current->deadline = 0; sched_unlock();
}
bool task_kernel_thread(void (*entry)(void), const char* name) {
    struct task* t = allocate(); if (!t) return false;
    ((uint64_t*)t->kernel_rsp)[6] = (uint64_t)kernel_thread_start;
    t->process = &kernel_threads; t->initial.rip = (uint64_t)entry; strncpy(t->comm, name, 15);
    reset_user_fpu(); fpu_save(t->fpu);
    sched_lock(); t->state = RUNNABLE; t->runnable_since = native_clock_ns(); rq_add(t); smp_wake_idle(); sched_unlock();
    return true;
}
/* ---- Job control: stopped thread groups, orphaned process groups, controlling terminals (pty.c) ---- */
static bool group_orphaned(uint32_t group) {
    /* POSIX: orphaned when no member has a live parent in another group of the same session. */
    for (struct process* p = processes; p; p = p->next) {
        if (!p->threads || p->group != group) continue;
        struct process* parent = find_process(p->parent);
        if (parent && parent->threads && parent->group != group && parent->session == p->session) return false;
    }
    return true;
}
static void job_notify_parent(struct process* p, int code, unsigned number) {
    struct process* parent = find_process(p->parent); struct task* recipient = NULL;
    if (!parent || !parent->threads) return;
    for (struct task* t = tasks; t; t = t->next) if (t->process == parent && t->state != ZOMBIE) {
        recipient = t; if (!signals_masked(t->signals, 17)) break;
    }
    if (recipient && !signals_nocldstop(recipient->signals))
        signals_enqueue(recipient->signals, 17, code, pid_number(p->identity, recipient->identity->space), p->uid, (int)number, true);
    wake_process(parent);
}
/* Side effects at generation (Linux prepare_signal): stop and continue signals cancel each other. */
static void job_signal(struct process* p, unsigned number) {
    if (number >= 19 && number <= 22) {
        for (struct task* t = tasks; t; t = t->next) if (t->process == p && t->state != ZOMBIE) signals_discard(t->signals, 1ULL << 17);
    } else if (number == 18) {
        for (struct task* t = tasks; t; t = t->next) if (t->process == p && t->state != ZOMBIE) signals_discard(t->signals, 0xFULL << 18);
        if (p->stopped) { p->stopped = false; p->report = 2; job_notify_parent(p, 6, 18); task_wake_channel(&p->stopped); }
    } else if (number == 9 && p->stopped) { p->stopped = false; p->report = 0; task_wake_channel(&p->stopped); }
    if (number == 9) for (struct task* t = tasks; t; t = t->next) if (t->process == p && t->traced_stop) ptrace_resume(t, 0);
}
/* A dequeued stop signal with the default action stops the whole thread group until SIGCONT/SIGKILL. */
void task_stop(unsigned number) {
    struct process* p = current->process;
    if (number != 19 && group_orphaned(p->group)) return; /* terminal stops of orphaned groups are discarded */
    if (!p->stopped && !p->exiting) {
        p->stopped = true; p->stop_signal = (uint8_t)number; p->report = 1; job_notify_parent(p, 5, number);
        sched_lock(); /* kick: other threads of the group must notice the stop now */
        for (struct task* t = tasks; t; t = t->next) if (t->process == p && t != current && t->state == RUNNABLE && t->on_cpu) kick(t);
        sched_unlock();
    }
    task_stop_point();
}
/* Every return to user mode passes here: threads of a stopped group wait (ignoring other signals). */
void task_stop_point(void) {
    struct process* p = current ? current->process : NULL;
    while (p && p->stopped && !p->exiting) {
        /* As task_wait_channel, but other signals do not end the stop (SIGCONT/SIGKILL clear it). */
        sched_lock(); current->channel = &p->stopped; current->state = WAITING; rq_remove(current); wait_link(current); sched_unlock();
        task_yield();
        sched_lock(); wait_unlink(current); current->channel = NULL; sched_unlock();
    }
}
struct terminal* task_terminal(void) { return current ? current->process->terminal : NULL; }
void task_set_terminal(struct terminal* terminal) { if (current) current->process->terminal = terminal; }
void task_terminal_forget(struct terminal* terminal, uint32_t session) {
    for (struct process* p = processes; p; p = p->next) if (p->terminal == terminal && (!session || p->session == session)) p->terminal = NULL;
}
bool task_session_leader(void) { return current && current->process->pid == current->process->session; }
uint32_t task_session_id(void) { return current ? current->process->session : 0; }
uint32_t task_group_id(void) { return current ? current->process->group : 0; }
bool task_group_exists(uint32_t group, uint32_t session) {
    for (struct process* p = processes; p; p = p->next) if (p->threads && p->group == group && p->session == session) return true;
    return false;
}
bool task_group_orphaned(uint32_t group) { return group_orphaned(group); }
/* Kernel-generated (SI_KERNEL) signals from the terminal driver. */
static int signal_kernel(struct process* p, unsigned number) {
    struct task* recipient = NULL;
    for (struct task* t = tasks; t; t = t->next) if (t->process == p && t->state != ZOMBIE) {
        recipient = t; if (!signals_masked(t->signals, number)) break;
    }
    if (!recipient) return -3;
    job_signal(p, number);
    int error = signals_enqueue(recipient->signals, number, 0x80, 0, 0, 0, true);
    if (!error) wake_process(p);
    return error;
}
int task_signal_group(uint32_t group, unsigned number) {
    int result = -3;
    for (struct process* p = processes; p; p = p->next) if (p->threads && p->group == group) {
        int error = signal_kernel(p, number); if (!error || result == -3) result = error;
    }
    return result;
}
int task_signal_process(uint32_t pid, unsigned number) {
    struct process* p = find_process(pid); return p && p->threads ? signal_kernel(p, number) : -3;
}
static void orphan_hangup(uint32_t group) {
    bool stopped = false;
    for (struct process* p = processes; p; p = p->next) if (p->threads && p->group == group && p->stopped) stopped = true;
    if (stopped && group_orphaned(group)) { task_signal_group(group, 1); task_signal_group(group, 18); }
}
int64_t task_rusage(int64_t who, uint64_t address) {
    if (!current || (who != 0 && who != -1 && who != 1)) return -22;
    if (!accessible(address, sizeof(struct rusage64), true)) return -14;
    struct process* p = current->process;
    if (who == -1) { rusage_copy(address, p->child_cpu_ns, p->child_maxrss_kb, p->child_faults, p->child_voluntary, p->child_involuntary); return 0; }
    uint64_t resident = space_resident_pages(current->space) * 4; if (resident > p->maxrss_kb) p->maxrss_kb = resident;
    if (who == 1) rusage_copy(address, task_cpu_time(true), p->maxrss_kb, current->faults, current->voluntary, current->involuntary);
    else rusage_copy(address, task_cpu_time(false), p->maxrss_kb, p->faults, p->voluntary, p->involuntary);
    return 0;
}
/* times(): clock ticks at USER_HZ = 100. */
int64_t task_times(uint64_t address) {
    if (address) {
        if (!accessible(address, 32, true)) return -14;
        uint64_t values[4] = {current ? task_cpu_time(false) / 10000000 : 0, 0, current ? current->process->child_cpu_ns / 10000000 : 0, 0};
        copy_to_user((void*)address, values, sizeof(values));
    }
    return (int64_t)(native_clock_ns() / 10000000);
}
/* Weighted fair scheduling. Weights follow Linux's nice table (nice 0 = 1024, x1.25 per step);
 * the desktop compositor runs as nice -10 and realtime-policy threads as nice -15. */
static const uint32_t nice_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548, 7620, 6100, 4904, 3906, 3121, 2501, 1991, 1586, 1277,
    1024, 820, 655, 526, 423, 335, 272, 215, 172, 137,
    110, 87, 70, 56, 45, 36, 29, 23, 18, 15 };
static uint64_t task_weight(const struct task* t) {
    int nice = t->nice;
    if (t->process->pid == 1 && nice > -10) nice = -10;
    if ((t->policy == 1 || t->policy == 2) && nice > -15) nice = -15;
    return nice_weight[nice + 20];
}
#define PREEMPT_GRANULARITY_NS 1000000ULL
#define SLICE_MAX_NS 6000000ULL
static bool should_preempt(void) {
    if (!current || pm_freezing()) return true;   /* system sleep: every task leaves its CPU (pm.c) */
    uint64_t now = native_clock_ns(), ran = now - current->run_started;
    bool other = false;
    uint64_t mine = current->vruntime + ran * 1024 / task_weight(current);
    for (struct task* t = runqueue; t; t = t->rq_next) {
        if (t == current || t->state != RUNNABLE || t->on_cpu) continue;
        other = true;
        if (t->vruntime + PREEMPT_GRANULARITY_NS < mine) return true;
    }
    return other && ran >= SLICE_MAX_NS;
}
/* No CPU is idle for the woken task `t`: interrupt the CPU whose task is furthest ahead in virtual
 * time if `t` would preempt it (sched_lock held, so each CPU's task stays put). */
static void wake_preempt(struct task* t) {
    struct cpu* me = cpu_self(); uint64_t now = native_clock_ns(), furthest = 0; int victim = -1;
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        struct cpu* c = &cpus[i]; struct task* u = c->task;
        if (!c->online || c == me || !u || u == t) continue;
        uint64_t ran = now > u->run_started ? now - u->run_started : 0;
        uint64_t v = u->vruntime + ran * 1024 / task_weight(u);
        if (v > furthest) { furthest = v; victim = (int)i; }
    }
    if (victim >= 0 && t->vruntime + PREEMPT_GRANULARITY_NS < furthest) smp_send_ipi((unsigned)victim, VEC_WAKE);
}
/* sched_yield: queue behind every runnable task. */
void task_sched_yield(void) {
    if (!current) return;
    sched_lock();
    uint64_t latest = current->vruntime;
    for (struct task* t = runqueue; t; t = t->rq_next) if (t != current && t->state == RUNNABLE && t->vruntime > latest) latest = t->vruntime;
    current->vruntime = latest + 1;
    sched_unlock();
    task_yield();
}
static struct task* find_task(int64_t visible) {
    if (!visible) return current;
    uint32_t internal = task_internal_pid((uint32_t)visible);
    for (struct task* t = tasks; t; t = t->next) if (t->pid == internal && t->state != ZOMBIE) return t;
    return NULL;
}
int64_t task_sched_call(struct registers* r) {
    uint64_t call = r->rax;
    if (call == 146 || call == 147) {
        int policy = (int)r->rdi;
        if (policy == 1 || policy == 2) return call == 146 ? 99 : 1;
        return policy == 0 || policy == 3 || policy == 5 ? 0 : -22;
    }
    int64_t who = (int32_t)(call == 140 || call == 141 ? r->rsi : r->rdi);
    if (who < 0) return -22;
    struct task* target = (call == 140 || call == 141) && r->rdi ? current : find_task(who);
    if (!target) return -3;
    switch (call) {
    case 140: return 20 - target->nice;
    case 141: { int64_t value = (int32_t)r->rdx; target->nice = value < -20 ? -20 : value > 19 ? 19 : (int8_t)value; return 0; }
    case 142: case 144: {
        int policy = call == 144 ? (int)(r->rsi & ~0x40000000ULL) : target->policy;
        uint64_t parameter = call == 144 ? r->rdx : r->rsi;
        if (!accessible(parameter, 4, false)) return -14;
        int32_t priority = (int32_t)uget32(parameter);
        if (policy == 1 || policy == 2) { if (priority < 1 || priority > 99) return -22; }
        else if (policy == 0 || policy == 3 || policy == 5) { if (priority) return -22; }
        else return -22;
        target->policy = (uint8_t)policy; target->rt_priority = (uint8_t)priority; return 0;
    }
    case 143: if (!accessible(r->rsi, 4, true)) return -14; return uput32(r->rsi, target->rt_priority);
    case 145: return target->policy;
    case 203: return 0;
    case 204:
        if (r->rsi < 8 || (r->rsi & 7)) return -22;
        if (!accessible(r->rdx, 8, true)) return -14;
        uput64(r->rdx, 1); return 8;
    }
    return -38;
}
struct user_segments* task_segments(void) { return current ? &current->seg : NULL; }
void task_set_name(const char* name) { if (current) { memset(current->comm, 0, sizeof(current->comm)); strncpy(current->comm, name, 15); } }
/* /proc/PID/task/TID/comm: a thread group may rename its own threads (Linux comm_write). */
int task_set_name_tid(uint32_t tid, const char* name) {
    struct task* t = find(tid); if (!t || t->state == ZOMBIE) return -3;
    if (!current || t->process != current->process) return -22;
    memset(t->comm, 0, sizeof(t->comm)); strncpy(t->comm, name, 15); return 0;
}
struct address_space* task_space(uint32_t tid) { struct task* t = find(tid); return t && t->state != ZOMBIE ? t->space : NULL; }
/* ---- ptrace (subset): ATTACH/SEIZE/TRACEME, signal-delivery stops reported to the tracer (waitpid with any options,
 * __WALL), PEEK/POKE TEXT/DATA, PEEKUSER of the general and debug registers (hardware breakpoints are not
 * supported: the debug registers read as 0 and only 0 can be written), CONT and DETACH with a signal. No syscall,
 * exec or clone events. A tracee in a ptrace stop waits in task_ptrace_stop until resumed (SIGKILL ends it).
 * A tracee's exit is reported to its tracer, which reaps that zombie thread (terminate). A set-user/group-ID or
 * capability gain at exec is suppressed unless the credentials recorded with the link (those of the thread that
 * asked for it: the tracer at ATTACH/SEIZE, the tracee itself at TRACEME, as Linux ptrace_link records since
 * CVE-2019-13272) hold CAP_SYS_PTRACE over the tracee; a privilege the tracer gains later does not count. ---- */
static void ptrace_resume(struct task* t, int signal) { t->traced_resume = (int8_t)signal; task_wake_channel(&t->traced_resume); }
static int ptrace_link(struct task* t, struct task* tracer) {
    int error; struct credentials* recorded = credentials_clone(current->credentials, false, &error); if (!recorded) return error;
    t->tracer = tracer; t->tracer_credentials = recorded; return 0;
}
static void ptrace_unlink(struct task* t) {
    t->tracer = NULL; if (t->tracer_credentials) credentials_destroy(t->tracer_credentials); t->tracer_credentials = NULL;
}
static void ptrace_untrace(struct task* t) { ptrace_unlink(t); t->traced_exit = false; if (t->traced_stop) ptrace_resume(t, 0); }
static void ptrace_collect(struct task* t) { t->traced_exit = false; ptrace_unlink(t); zombies_pending = true; }
uint64_t task_ptrace_sender(void) { return current ? current->traced_sender : 0; }
int task_ptrace_stop(struct registers* r, int number) {
    struct task* t = current;
    if (!t || !t->tracer || number == 9) return number;
    t->traced_stop = true; t->traced_signal = (uint8_t)number; t->traced_reported = false; t->traced_resume = -1; t->traced_regs = r;
    /* any thread of the tracer's group may wait for it, each in wait_child on its own task */
    for (struct task* w = tasks; w; w = w->next) if (w->process == t->tracer->process && w->state != ZOMBIE) task_wake_channel(w);
    while (t->traced_resume < 0 && t->tracer && !task_exiting()) {
        sched_lock(); t->channel = &t->traced_resume; t->state = WAITING; rq_remove(t); wait_link(t); sched_unlock();
        task_yield();
        sched_lock(); wait_unlink(t); t->channel = NULL; sched_unlock();
    }
    int resume = t->traced_resume > 0 ? t->traced_resume : 0;
    t->traced_stop = false; t->traced_regs = NULL; t->traced_resume = -1; return resume;
}
static int64_t user_register(struct task* t, uint64_t offset) {   /* struct user_regs_struct order */
    struct registers* r = t->traced_regs; if (!r) return 0;
    const uint64_t values[27] = {r->r15, r->r14, r->r13, r->r12, r->rbp, r->rbx, r->r11, r->r10, r->r9, r->r8, r->rax, r->rcx, r->rdx,
                                 r->rsi, r->rdi, r->rax, r->rip, r->cs, r->flags, r->rsp, r->ss, t->fs, t->gs, 0x2b, 0x2b, 0, 0};
    return (int64_t)values[offset / 8];
}
/* Yama LSM (ptrace_scope >= 1): a tracer may PTRACE_ATTACH only to its own descendant, or with CAP_SYS_PTRACE over
 * the tracee's namespace, or when the tracee allowed it with prctl(PR_SET_PTRACER) (a pid or PR_SET_PTRACER_ANY). */
static bool yama_attach_allowed(struct task* tracer, struct task* tracee) {
    if (yama_ptrace_scope <= 0) return true;
    if (credentials_capable(tracer->credentials, credentials_namespace(tracee->credentials), 19)) return true;
    struct process* cp = tracee->process;
    for (struct process* a = find_process(cp->parent); a; a = find_process(a->parent))
        if (a == tracer->process) return true;                   /* tracer is an ancestor of the tracee */
    if (cp->ptracer_any) return true;
    if (cp->ptracer)
        for (struct process* a = tracer->process; a; a = find_process(a->parent))
            if (a->pid == cp->ptracer) return true;              /* the declared ptracer or its descendants */
    return false;
}
int64_t task_ptrace(uint64_t request, int64_t pid, uint64_t address, uint64_t data) {
    if (!current) return -3;
    if (request == 0) {   /* PTRACE_TRACEME: the parent traces this thread */
        struct task* parent = find(current->process->parent);
        if (!parent || parent->state == ZOMBIE || current->tracer) return -1;
        if (!credentials_traceme_allowed(parent->credentials, current->credentials)) return -1;
        int error = ptrace_link(current, parent); if (error) return error;
        current->traced_resume = -1; return 0;
    }
    struct task* t = pid > 0 ? find(task_internal_pid((uint32_t)pid)) : NULL;
    if (!t || t->state == ZOMBIE) return -3;
    if (request == 16 || request == 0x4206) {   /* PTRACE_ATTACH, PTRACE_SEIZE */
        if (t->process == current->process || t->tracer) return -1;
        if (!credentials_inspect_allowed(t->credentials, space_dumpable(t->space))) return -1;
        if (!yama_attach_allowed(current, t)) return -1;   /* Yama ptrace_scope: ancestor / CAP_SYS_PTRACE / PR_SET_PTRACER */
        if (!landlock_ptrace_allowed(current->pid, t->pid)) return -1;   /* Landlock: the same or a nested domain */
        int error = ptrace_link(t, current); if (error) return error;
        t->traced_stop = false; t->traced_resume = -1;
        if (request == 16) {   /* a thread-directed SIGSTOP brings it into a ptrace stop */
            error = signals_enqueue(t->signals, 19, 0, pid_number(current->process->identity, t->identity->space), task_kernel_uid(), 0, false);
            if (error) { ptrace_unlink(t); return error; }
            sched_lock(); if (t->state == WAITING) task_ready(t); else if (t->on_cpu && t != current) kick(t); sched_unlock();
        }
        return 0;
    }
    if (t->tracer != current) return -3;
    bool stopped = t->traced_stop;
    /* Linux ptrace_access_vm: the memory of a non-dumpable image (an unreadable or set-ID exec, also one made after
     * the attach) is closed to a tracer without CAP_SYS_PTRACE over the tracee's namespace */
    bool memory = space_dumpable(t->space) || credentials_capable(current->credentials, credentials_namespace(t->credentials), 19);
    switch (request) {
    case 1: case 2: {   /* PEEKTEXT, PEEKDATA: the word is stored at data */
        if (!stopped) return -3;
        uint64_t word; if (!memory || space_access(t->space, address, &word, 8, false) != 8) return -5;
        if (!accessible(data, 8, true)) return -14;
        return uput64(data, word);
    }
    case 4: case 5: if (!stopped) return -3; return memory && space_access(t->space, address, &data, 8, true) == 8 ? 0 : -5;   /* POKETEXT, POKEDATA */
    case 3: {   /* PEEKUSER: user_regs_struct (216 bytes), u_debugreg[8] at 848 */
        if (!stopped) return -3;
        if ((address & 7) || (address >= 216 && (address < 848 || address >= 912))) return -5;
        if (!accessible(data, 8, true)) return -14;
        return uput64(data, (uint64_t)(address < 216 ? user_register(t, address) : 0));
    }
    case 6: if (!stopped) return -3; return address >= 848 && address < 912 && !(address & 7) && !data ? 0 : -5;   /* POKEUSER */
    case 7: case 17:   /* CONT, DETACH (with an optional signal to deliver) */
        if (data > 64) return -5;
        if (!stopped && request == 7) return -3;
        if (request == 17) ptrace_unlink(t);
        if (stopped) {   /* an injected signal comes from the tracer (Linux ptrace_signal: SI_USER, its tid and uid) */
            t->traced_sender = (uint64_t)task_kernel_uid() << 32 | pid_number(current->identity, t->identity->space);
            ptrace_resume(t, (int)data);
        }
        return 0;
    }
    return -5;
}
void task_get_name(char* name) {
    memset(name, 0, 16);
    if (!current) return;
    if (current->comm[0]) { memcpy(name, current->comm, 15); return; }
    const char* base = current->executable;
    for (const char* p = current->executable; *p; p++) if (*p == '/') base = p + 1;
    strncpy(name, base, 15);
}
/* Kernel globals (address space, descriptors, signals, credentials, filesystem) describe the task
 * that the lock-holding CPU runs, or no task in a scheduler loop. Called whenever a CPU takes the lock
 * and whenever a scheduler switches. */
static struct task* activated = (void*)1;
void task_context_reload(void) {
    struct task* t = current;
    if (t) {
        space_activate(t->space); fd_table_activate(t->descriptors); signals_activate(t->signals);
        credentials_activate(t->credentials); filesystem_activate(t->filesystem);
        if (activated != t) native_set_executable(t->executable);
    } else {
        space_activate_lazy(); fd_table_activate(NULL); signals_activate(NULL);
        credentials_activate(NULL); filesystem_activate(NULL);
    }
    activated = t;
}
/* This CPU's scheduler. Runs without the big lock; picks under sched_lock. The BSP's loop also takes
 * the big lock for device polls, write-back and reaping, and returns once no task is left;
 * application processors never return. */
static int scheduler(bool bsp) {
    uint32_t cursor = 0; int result = -1;
    struct cpu* c = cpu_self();
    for (;;) {
        bool any_runnable = false, no_tasks = false;
        if (bsp) {
            kernel_lock_acquire();
            if (!tasks && !processes) { kernel_lock_release(); break; }
            uint64_t pass = native_clock_ns();
            native_network_poll(); sound_poll(); /* both return at once when idle */
            virtio_gpu_poll();
            native_io_poll();
            power_poll(); /* ACPI worker, power events, pending system sleep (power.c) */
            native_gc_poll();
            static uint64_t last_process_scan;
            bool reap = (zombies_pending || pass - last_process_scan >= 10000000ULL) && !block_io_defer();   /* releases may write */
            if (reap) { zombies_pending = false; last_process_scan = pass; }
            bool released = false;
            if (reap) for (struct task* t = tasks; t;) {
                struct task* next = t->next;
                sched_lock(); bool idle_zombie = t->state == ZOMBIE && !t->on_cpu; sched_unlock();
                if (idle_zombie && (!t->traced_exit || t->allocation)) {
                    space_activate(NULL); /* its space may still be the loaded one */
                    if (!t->traced_exit) { release_resources(t); unlink_task(t); }
                    else release_exit_resources(t); /* only the zombie waits for its tracer to collect it */
                    released = true;
                }
                t = next;
            }
            if (released) proc_release_dead_images();
            if (reap) for (struct process* p = processes; p;) {
                struct process* next = p->next;
                if (!p->threads && !p->parent) {
                    if (p->pid == 1) result = (p->status & 127) ? 128 + (p->status & 127) : p->status >> 8;
                    process_reap(p);
                }
                p = next;
            }
            if (!tasks && !processes) { kernel_lock_release(); break; }
            no_tasks = !tasks;
            sched_lock();
            for (struct task* t = runqueue; t && !any_runnable; t = t->rq_next) any_runnable = t->state == RUNNABLE && !t->on_cpu;
            sched_unlock();
            native_writeback_poll(!any_runnable);
            { void tick_bsp_pass(bool work_pending); tick_bsp_pass(zombies_pending); }   /* tick.c: when these polls need this CPU next */
            kernel_lock_release();
        }
        sched_lock();
        struct task *chosen = NULL;
        for (struct task* t = runqueue; t; t = t->rq_next) if (t->state == RUNNABLE && !t->on_cpu) {
            if (!chosen || t->vruntime < chosen->vruntime || (t->vruntime == chosen->vruntime && t->pid > cursor && t->pid < chosen->pid)) chosen = t;
        }
        if (pm_freezing()) chosen = NULL;   /* system sleep: tasks stay frozen (pm.c) */
        if (!chosen && bsp && no_tasks) { sched_unlock(); break; }
        if (!chosen && bsp && ip_loopback_pending()) { sched_unlock(); continue; } /* deliver queued loopback packets first */
        if (!chosen && !bsp && pm_freezing()) { sched_unlock(); pm_cpu_park(); continue; }
        if (!chosen) {
            /* Halt until there may be work: any interrupt for the BSP (its loop polls devices), a
             * wake-up for application processors. Wakers set wake_pending under sched_lock. */
            c->idle = true; c->wake_pending = false;
            sched_unlock();
            do cpu_idle_enter(); while (!bsp && !__atomic_load_n(&c->wake_pending, __ATOMIC_ACQUIRE));   /* HLT/MWAIT/C-states (cpufreq.c) */
            sched_lock(); c->idle = false; c->wake_pending = false; sched_unlock();
            continue;
        }
        if (chosen->vruntime > min_vruntime) min_vruntime = chosen->vruntime;
        current = chosen; cursor = chosen->pid; chosen->on_cpu = true; chosen->cpu = (uint8_t)c->index;
        sched_unlock();
        space_load_cr3(chosen->space);
        cpu_task_stack(chosen->allocation + STACK_BYTES);
        segments_switch_in(&chosen->seg, chosen->space, chosen->fs, chosen->gs); /* TLS slots, LDT, selectors, bases */
        fpu_restore(chosen->fpu);
        chosen->run_started = native_clock_ns();
        kstat.switches++; c->switches++;
        switch_context(&scheduler_rsp, chosen->kernel_rsp);
        /* The task that ran here yielded back; its kernel stack is free to run elsewhere. */
        sched_lock(); current->on_cpu = false; current = NULL; sched_unlock();
        cpu_task_stack(c->fallback_top);
    }
    return result;
}
void task_scheduler_ap(void) {
    scheduler(false);
    panic("application processor scheduler returned");
}
int task_run(uint64_t entry, uint64_t stack, struct address_space* space,
             struct descriptor_table* descriptors, struct signal_state* signals, const char* executable) {
    if (tasks || processes || current) panic("nested boot process runner");
    pid_reset();
    struct task* root = allocate();
    if (!root) { space_release(space); fd_table_release(descriptors); signals_destroy(signals); return -1; }
    root->space = space; root->descriptors = descriptors; root->signals = signals;
    root->filesystem = filesystem_clone(NULL, false);
    if (!root->filesystem) { release_resources(root); page_free((uint64_t)root); return -1; }
    int credential_error; root->credentials = credentials_clone(NULL, false, &credential_error);
    if (!root->credentials) { release_resources(root); page_free((uint64_t)root); return -1; }
    root->children_namespace = pid_namespace_root();
    root->identity = pid_allocate(root->children_namespace);
    if (!root->identity) { release_resources(root); page_free((uint64_t)root); return -1; }
    root->pid = root->identity->numbers[0];
    root->process = process_new(root->identity, NULL);
    if (!root->process) { release_resources(root); page_free((uint64_t)root); return -1; }
    strcpy(root->executable, executable);
    root->executable_inode = executable_inode(executable);
    root->initial = (struct registers){.rip = entry, .rsp = stack, .cs = USER_CS, .ss = USER_DS, .flags = 0x202};
    reset_user_fpu(); fpu_save(root->fpu);
    pid_publish(root->identity); tasks = root;
    sched_lock(); rq_add(root); smp_wake_idle(); sched_unlock();
    kernel_lock_release(); /* the scheduler loops run without the big lock */
    int result = scheduler(true);
    kernel_lock_acquire();
    space_activate(NULL);
    pid_reset(); return result;
}
/* ---- seccomp per-task state (seccomp.c owns the filters; these reach struct task) ---- */
uint8_t task_seccomp_mode(void) { return current ? current->seccomp_mode : 0; }
struct seccomp_filter* task_seccomp_head(void) { return current ? current->seccomp_filters : NULL; }
void task_seccomp_set(uint8_t mode, struct seccomp_filter* head) {
    if (current) { current->seccomp_mode = mode; current->seccomp_filters = head; }
}
bool task_alive(uint32_t tid) { struct task* t = find(tid); return t && t->state != ZOMBIE; }
uint32_t task_personality(void) { return current ? current->persona : 0; }
/* personality(2): set the persona and return the previous one; 0xFFFFFFFF only queries (Linux). The ASLR-relevant
 * ADDR_NO_RANDOMIZE bit is honoured by the ELF loader at the next execve; the value persists across exec. */
int64_t task_personality_call(uint64_t persona) {
    if (!current) return -1;
    uint32_t previous = current->persona;
    if (persona != 0xFFFFFFFFULL) current->persona = (uint32_t)persona;
    return previous;
}
void task_seccomp_info(uint32_t tid, int* mode, unsigned* filters) {
    struct task* t = find(tid);
    *mode = t ? t->seccomp_mode : 0;
    *filters = t ? seccomp_filter_count(t->seccomp_filters) : 0;
}
/* A seccomp KILL action / strict violation: fatal `signal` to the whole thread group, or (thread-only, for a
 * multithreaded process) just this thread. A single-threaded process is always killed as a group, as on Linux. */
void task_seccomp_kill(int signal, bool group) {
    bool last = current && current->process->threads == 1;
    user_termination_signal = (group || last) ? (unsigned)signal : 0;
    user_result = (group || last) ? (128 + signal) : 0;
}
/* SECCOMP_FILTER_FLAG_TSYNC: add `added` to every thread of this group. Every sibling must be unfiltered-strict-free
 * and share the caller's current chain head (the condition libseccomp/Chrome rely on); otherwise the first offender's
 * thread id is returned (positive), nothing is changed. Runs under the big kernel lock, so it is atomic against other
 * threads' syscall entries. */
int task_seccomp_tsync(struct seccomp_filter* added) {
    if (!current) return -1;
    struct process* p = current->process;
    struct seccomp_filter* base = current->seccomp_filters;
    for (struct task* t = tasks; t; t = t->next) {
        if (t->process != p || t->state == ZOMBIE || t == current) continue;
        if (t->seccomp_mode == 1 /* strict */ || !seccomp_filter_is_ancestor(t->seccomp_filters, base)) {   /* Linux is_ancestor */
            uint32_t v = task_visible_pid(t->pid); return v ? (int)v : -1;
        }
    }
    seccomp_filter_chain(added, base);   /* the caller's head ref becomes added->prev's ref (no retain/release) */
    current->seccomp_filters = added; current->seccomp_mode = 2 /* filter */;
    for (struct task* t = tasks; t; t = t->next) {
        if (t->process != p || t->state == ZOMBIE || t == current) continue;
        struct seccomp_filter* old = t->seccomp_filters;   /* an ancestor of the caller's chain (or none) */
        seccomp_filter_retain(added);
        t->seccomp_filters = added; t->seccomp_mode = 2;
        if (old) seccomp_filter_release(old);   /* drop this sibling's former head ref */
        if (t->credentials && credentials_no_new_privs()) credentials_mark_no_new_privs(t->credentials);   /* Linux: only if the caller has NNP */
    }
    return 0;
}
/* ---- Linux ABI helpers (kabi: posix_timers.c, signalfd.c, pidfd.c); big lock held by every caller ---- */
void task_timer_deadline(uint64_t deadline) { sched_lock(); if (deadline < next_deadline) next_deadline = deadline; sched_unlock(); }
uint64_t task_rlimit_soft(unsigned resource) {   /* any RLIMIT_* soft limit of the caller */
    if (resource >= 16) return 0;
    int index = limit_index(resource);
    if (!current) return index >= 0 ? initial_limits[index].soft : default_limits[resource].soft;
    return index >= 0 ? current->process->limits[index].soft : current->process->other_limits[resource].soft;
}
static struct task* live_thread(uint32_t pid) {   /* a live thread of process `pid` (its leader may have exited) */
    struct task* leader = find(pid);
    if (leader && leader->state != ZOMBIE && leader->process->pid == pid) return leader;
    for (struct task* t = tasks; t; t = t->next) if (t->state != ZOMBIE && t->process->pid == pid) return t;
    return NULL;
}
uint32_t task_live_thread(uint32_t pid) { struct task* t = live_thread(pid); return t ? t->pid : 0; }
/* The signal state whose queues hold a signal for thread `tid` of `pid` (0: the process queue), NULL once exiting. */
struct signal_state* task_signal_target(uint32_t pid, uint32_t tid) {
    if (!tid) { struct task* t = live_thread(pid); return t ? t->signals : NULL; }
    struct task* t = find(tid); return t && t->state != ZOMBIE && t->process->pid == pid ? t->signals : NULL;
}
/* Queue a kernel-built siginfo (POSIX timers) to process `pid`, or to its thread `tid`: 0 queued, 1 ignored, -3 gone. */
int task_queue_info(uint32_t pid, uint32_t tid, const void* siginfo, void* cookie) {
    struct process* p = find_process(pid); if (!p || !p->threads) return -3;
    unsigned number = (unsigned)*(const int32_t*)siginfo; if (!number || number > 64) return -22;
    struct task* recipient = NULL;
    if (tid) { recipient = find(tid); if (!recipient || recipient->state == ZOMBIE || recipient->process != p) return -3; }
    else for (struct task* t = tasks; t; t = t->next) if (t->state != ZOMBIE && t->process == p) { recipient = t; if (!signals_masked(t->signals, number)) break; }
    if (!recipient) return -3;
    job_signal(p, number);
    int result = signals_enqueue_info(recipient->signals, siginfo, !tid, cookie);
    if (result) return result;
    if (!tid) wake_process(p);
    else { sched_lock(); if (recipient->state == WAITING) task_ready(recipient); else if (recipient->on_cpu && recipient != current) kick(recipient); sched_unlock(); }
    return 0;
}
/* CPU time (ns) of thread `id` or of process `id` (all its threads, the exited ones included); false: no such
 * live thread / no such process (a zombie process still has its final time until it is reaped). */
bool task_cpu_clock(uint32_t id, bool thread, uint64_t* ns) {
    uint64_t now = native_clock_ns(), total = 0;
    if (thread) {
        struct task* t = find(id); if (!t || t->state == ZOMBIE) return false;
        sched_lock(); total = t->cpu_ns + (t->on_cpu && t->run_started && now > t->run_started ? now - t->run_started : 0); sched_unlock();
        *ns = total; return true;
    }
    struct process* p = find_process(id); if (!p) return false;
    sched_lock();
    total = p->cpu_ns;
    for (struct task* t = tasks; t; t = t->next)
        if (t->state != ZOMBIE && t->process == p && t->on_cpu && t->run_started && now > t->run_started) total += now - t->run_started;
    sched_unlock();
    *ns = total; return true;
}
int task_pidfd_state(uint32_t pid) { struct process* p = find_process(pid); return !p ? 2 : p->threads ? 0 : 1; }   /* running, exited, reaped */
/* PTRACE_MODE_ATTACH_REALCREDS (pidfd_getfd): credentials, dumpability and Yama's ptrace_scope. */
bool task_attach_allowed(uint32_t tid) {
    struct task* t = find(tid);
    return t && current && t->state != ZOMBIE && (t->process == current->process ||
           (credentials_inspect_allowed(t->credentials, space_dumpable(t->space)) && yama_attach_allowed(current, t) &&
            landlock_ptrace_allowed(current->pid, t->pid)));
}
int64_t task_clone3(const struct clone3_request* request, struct registers* arguments) {   /* pidfd.c: clone3 */
    if (!current) return -38;
    current->clone3 = request; int64_t result = task_clone(arguments); current->clone3 = NULL; return result;
}
/* rt_sigqueueinfo / rt_tgsigqueueinfo / pidfd_send_signal with a siginfo: the usual permission and namespace rules */
int task_signal_info(int64_t pid, int32_t tid, const void* siginfo) {
    uint64_t number = (uint32_t)*(const int32_t*)siginfo;
    custom_info = siginfo;
    int result = tid ? task_thread_signal((int32_t)pid, tid, number) : task_signal(pid, number, 0);
    custom_info = NULL; return result;
}
/* kabi.c: set_mempolicy/get_mempolicy (the calling thread), ioprio_set/ioprio_get (any thread, by internal tid) */
uint16_t* task_mempolicy(void) { return current ? &current->mempolicy : NULL; }
int task_ioprio(uint32_t tid, int set, uint16_t* value, int* nice, uint32_t* uid) {   /* 0, -3 no such live thread */
    struct task* t = tid ? find(tid) : current; if (!t || t->state == ZOMBIE) return -3;
    if (set) t->ioprio = *value; else *value = t->ioprio;
    if (nice) *nice = t->nice;
    if (uid) *uid = t->credentials ? credentials_kernel_id(t->credentials, false, 0) : 0;
    return 0;
}
uint32_t task_next_thread(uint32_t after) {   /* live thread ids in order (ioprio PGRP/USER walks) */
    uint32_t next = 0;
    for (struct task* t = tasks; t; t = t->next) if (t->state != ZOMBIE && t->process != &kernel_threads && t->pid > after && (!next || t->pid < next)) next = t->pid;
    return next;
}
uint32_t task_thread_group(uint32_t tid) { struct task* t = find(tid); return t && t->state != ZOMBIE ? t->process->group : 0; }
/* kabi.c RLIMIT_SIGPENDING: the real uid and the soft limit of the thread a signal state belongs to */
bool task_signal_owner(struct signal_state* state, uint32_t* uid, uint64_t* limit) {
    struct task* t = current && current->signals == state ? current : NULL;
    for (struct task* c = tasks; !t && c; c = c->next) if (c->signals == state) t = c;
    if (!t) return false;
    *uid = t->credentials ? credentials_kernel_id(t->credentials, false, 0) : 0; *limit = t->process->other_limits[11].soft;
    return true;
}
