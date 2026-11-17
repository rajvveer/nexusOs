/* signalfd(2) / signalfd4(2), rt_sigtimedwait(2), rt_sigqueueinfo(2), rt_tgsigqueueinfo(2).
 *
 * A signalfd is a mask; reading it dequeues the reader's own pending signals of that mask (its thread queue, then its
 * process queue: signals.c signals_dequeue) as struct signalfd_siginfo records (Linux signalfd_copyinfo layout rules).
 * Readiness is the reader's too: poll()/epoll evaluate it in the poller's context. When a signal is queued for some
 * thread (signal.c calls signalfd_queued), every signalfd whose mask has that signal is re-evaluated as if that thread
 * polled (a signalfd shared with another process then only wakes it spuriously: its own re-check finds nothing), so
 * epoll sets and poll() callers wake without polling. Blocked readers wake with every signal their process gets.
 * SIGKILL and SIGSTOP are never part of a mask. A mask update (signalfd on an existing signalfd) re-evaluates waiters.
 * Locking: big kernel lock. */
#include "native.h"
#include "uaccess.h"
#include "../../kernel/string.h"

#define NSEC 1000000000ULL
#define UNBLOCKABLE ((1ULL << 8) | (1ULL << 18))
struct signalfd { struct signalfd *next, *prev; uint64_t mask; };
static struct kslab signalfd_slab = {.size = sizeof(struct signalfd)};
static struct signalfd* signalfds;
static struct signal_state* evaluation;   /* signalfd_queued: whose pending signals readiness is evaluated against */

static uint32_t signalfd_ready(void* object) {
    struct signalfd* s = object; struct signal_state* state = evaluation ? evaluation : signals_current();
    return state && signals_pending_set(state, s->mask) ? 1U : 0;
}
static void* signalfd_source(void* object) { return object; }
static void changed(struct signalfd* s) { task_wake_channel(s); event_source_changed(s, 1); }
void signalfd_queued(struct signal_state* state, unsigned number) {
    if (!signalfds || !number || number > 64) return;
    struct signal_state* saved = evaluation; evaluation = state;
    for (struct signalfd* s = signalfds; s; s = s->next) if (s->mask >> (number - 1) & 1) changed(s);
    evaluation = saved;
}
/* struct signalfd_siginfo from a kernel siginfo (Linux siginfo_layout + signalfd_copyinfo). */
static void copy_info(const int32_t* in, uint8_t* out) {
    int32_t signo = in[0], code = in[2];
    memset(out, 0, 128);
    memcpy(out + 0, &in[0], 4); memcpy(out + 4, &in[1], 4); memcpy(out + 8, &in[2], 4);   /* ssi_signo, ssi_errno, ssi_code */
    const uint8_t* u = (const uint8_t*)in + 16;   /* the siginfo union */
    enum { KILL, TIMER, POLL, FAULT, CHLD, RT, SYS } layout = KILL;
    if (code > 0 && code < 0x80) {
        static const struct { uint8_t limit, layout; } specific[32] = {
            [4] = {11, FAULT}, [5] = {6, FAULT}, [7] = {5, FAULT}, [8] = {15, FAULT}, [11] = {10, FAULT},
            [17] = {6, CHLD}, [29] = {6, POLL}, [31] = {2, SYS}};
        if (signo > 0 && signo < 32 && specific[signo].limit && code <= specific[signo].limit) layout = specific[signo].layout;
        else if (code <= 6) layout = POLL;
    } else if (code == -2) layout = TIMER;
    else if (code == -5) layout = POLL;
    else if (code < 0) layout = RT;
    switch (layout) {
    case KILL: memcpy(out + 12, u, 4); memcpy(out + 16, u + 4, 4); break;                          /* ssi_pid, ssi_uid */
    case TIMER: memcpy(out + 24, u, 4); memcpy(out + 32, u + 4, 4);                                /* ssi_tid, ssi_overrun */
        memcpy(out + 48, u + 8, 8); memcpy(out + 44, u + 8, 4); break;                             /* ssi_ptr, ssi_int */
    case POLL: memcpy(out + 28, u, 4); memcpy(out + 20, u + 8, 4); break;                          /* ssi_band, ssi_fd */
    case FAULT: memcpy(out + 72, u, 8); if (signo == 7 && (code == 4 || code == 5)) memcpy(out + 80, u + 8, 2); break;
    case CHLD: memcpy(out + 12, u, 4); memcpy(out + 16, u + 4, 4); memcpy(out + 40, u + 8, 4);     /* pid, uid, status */
        memcpy(out + 56, u + 16, 8); memcpy(out + 64, u + 24, 8); break;                           /* utime, stime */
    case RT: memcpy(out + 12, u, 4); memcpy(out + 16, u + 4, 4); memcpy(out + 48, u + 8, 8); memcpy(out + 44, u + 8, 4); break;
    case SYS: memcpy(out + 88, u, 8); memcpy(out + 84, u + 8, 4); memcpy(out + 96, u + 12, 4); break;   /* call_addr, syscall, arch */
    }
}
static int64_t signalfd_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct signalfd* s = object;
    if (write || total < 128) return -22;
    uint64_t done = 0; bool nonblock = (flags & 0x800) != 0;
    for (;;) {
        if (!iov_access(vectors, count, done, 128, true)) return done ? (int64_t)done : -14;   /* checked before dequeueing */
        int32_t info[32]; int number = signals_dequeue(s->mask, info);
        if (!number) {
            if (done) return (int64_t)done;
            if (nonblock) return -11;
            int error = task_wait_channel(s);
            if (error && !(number = signals_dequeue(s->mask, info))) return error;
            if (!number) continue;
        }
        uint8_t record[128]; copy_info(info, record);
        iov_copy(vectors, count, done, record, 128, false);
        done += 128; nonblock = true;   /* only the first record may block (Linux) */
        if (done + 128 > total) return (int64_t)done;
    }
}
static int signalfd_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) { (void)object; (void)command; (void)argument; (void)flags; return -25; }
static void signalfd_release(void* object) {
    struct signalfd* s = object;
    if (s->prev) s->prev->next = s->next; else signalfds = s->next;
    if (s->next) s->next->prev = s->prev;
    kslab_free(&signalfd_slab, s);
}
static void signalfd_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 6; stat->ino = 1 + ((uint64_t)object >> 3 & 0xFFFFFFFULL); stat->nlink = 1; stat->mode = 0600;
    stat->uid = credentials_visible_id(credentials_kernel_id(credentials_current(), false, 1), false);
    stat->gid = credentials_visible_id(credentials_kernel_id(credentials_current(), true, 1), true); stat->block_size = 4096;
}
/* /proc/PID/fdinfo (Linux signalfd_show_fdinfo: render_sigset_t of the mask, SIGKILL/SIGSTOP never in it) */
static unsigned signalfd_fdinfo(void* object, char* out, unsigned capacity) {
    return kfmt(out, capacity, 0, "sigmask:\t%016llx\n", ((struct signalfd*)object)->mask);
}
static const struct object_operations signalfd_operations = {signalfd_io, signalfd_ioctl, signalfd_ready, signalfd_source, signalfd_release, signalfd_stat};
unsigned signalfd_fdinfo_text(struct open_file* description, char* out, unsigned capacity) {   /* kernel.c fd_information_text */
    void* object = fd_object(description, &signalfd_operations); return object ? signalfd_fdinfo(object, out, capacity) : 0;
}
static int64_t signalfd_call(int32_t fd, uint64_t mask_address, uint64_t size, uint64_t flags) {
    if (flags & ~(0x800ULL | 0x80000ULL)) return -22;
    if (size != 8) return -22;
    if (!accessible(mask_address, 8, false)) return -14;
    uint64_t mask; copy_from_user(&mask, (const void*)mask_address, 8); mask &= ~UNBLOCKABLE;
    if (fd != -1) {   /* a new mask for an existing signalfd */
        struct open_file* f = fd_lookup((uint64_t)(int64_t)fd); if (!f) return -9;
        struct signalfd* s = fd_object(f, &signalfd_operations); if (!s) return -22;
        s->mask = mask; changed(s); return fd;
    }
    struct signalfd* s = kslab_alloc(&signalfd_slab); if (!s) return -12;
    s->mask = mask;
    int result = fd_create_object(&signalfd_operations, s, 2 | (flags & (0x800 | 0x80000)), "anon_inode:[signalfd]");
    if (result < 0) { kslab_free(&signalfd_slab, s); return result; }
    s->next = signalfds; if (signalfds) signalfds->prev = s; signalfds = s;
    return result;
}
/* rt_sigtimedwait(set, info, timeout, sigsetsize): Linux do_sigtimedwait (EAGAIN on timeout, EINTR for another signal). */
static int64_t sigtimedwait_call(uint64_t set_address, uint64_t info_address, uint64_t timeout_address, uint64_t size) {
    if (size != 8) return -22;
    if (!accessible(set_address, 8, false)) return -14;
    uint64_t set; copy_from_user(&set, (const void*)set_address, 8); set &= ~UNBLOCKABLE;
    uint64_t deadline = 0; bool poll_only = false;
    if (timeout_address) {
        if (!accessible(timeout_address, 16, false)) return -14;
        int64_t ts[2]; copy_from_user(ts, (const void*)timeout_address, 16);
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= (int64_t)NSEC) return -22;
        uint64_t seconds = (uint64_t)ts[0], ns = (uint64_t)ts[1], now = native_clock_ns();
        uint64_t duration = seconds > (~0ULL - ns) / NSEC ? ~0ULL : seconds * NSEC + ns;
        poll_only = !duration; deadline = duration > ~0ULL - now ? ~0ULL : now + duration;
    }
    if (info_address && !accessible(info_address, 128, true)) return -14;
    int channel = 0;
    for (;;) {
        int32_t info[32]; int number = signals_dequeue(set, info);
        int error = number || poll_only ? 0 : task_wait_until(&channel, deadline);
        if (!number && error) number = signals_dequeue(set, info);   /* a set signal may come with the wake-up */
        if (number) {
            /* the mapping is checked again: the task may have slept (Linux: the signal is consumed on EFAULT too) */
            if (info_address && !accessible(info_address, 128, true)) return -14;
            if (info_address) copy_to_user((void*)info_address, info, 128);
            return number;
        }
        if (poll_only || error == -110) return -11;
        if (error) return error;
    }
}
/* rt_sigqueueinfo(tgid, sig, info) / rt_tgsigqueueinfo(tgid, tid, sig, info): Linux refuses a forged kernel/kill/tkill
 * origin (si_code >= 0 or SI_TKILL) unless the target is the caller itself. */
static int64_t sigqueueinfo_call(int32_t tgid, int32_t tid, uint64_t sig, uint64_t address, bool thread) {
    int32_t info[32];
    if (!accessible(address, 128, false)) return -14;
    copy_from_user(info, (const void*)address, 128);
    if (thread && (tid <= 0 || tgid <= 0)) return -22;
    if (sig > 64) return -22;
    info[0] = (int32_t)sig;
    uint32_t self = task_visible_pid(task_tid());
    if ((info[2] >= 0 || info[2] == -6) && (uint32_t)(thread ? tid : tgid) != self) return -1;
    if (!thread && tgid <= 0) return -3;
    return task_signal_info(tgid, thread ? tid : 0, info);
}
int64_t signalfd_syscall(struct registers* r) {
    switch (r->rax) {
    case 282: return signalfd_call((int32_t)r->rdi, r->rsi, r->rdx, 0);
    case 289: return signalfd_call((int32_t)r->rdi, r->rsi, r->rdx, r->r10);
    case 128: return sigtimedwait_call(r->rdi, r->rsi, r->rdx, r->r10);
    case 129: return sigqueueinfo_call((int32_t)r->rdi, 0, r->rsi, r->rdx, false);
    case 297: return sigqueueinfo_call((int32_t)r->rdi, (int32_t)r->rsi, r->rdx, r->r10, true);
    }
    return -38;
}

/* ---- RLIMIT_SIGPENDING (Linux ucounts UCOUNT_RLIMIT_SIGPENDING) ----
 * Every queued signal with its siginfo and every POSIX timer (its preallocated signal) counts against the receiver's
 * (for a timer: the creator's) real uid, compared with that task's soft RLIMIT_SIGPENDING (Linux __sigqueue_alloc):
 * a standard signal from kill(2) or the kernel (si_code >= 0) overrides the limit (counted all the same); otherwise a
 * signal over the limit is queued without its information (SI_USER, pid and uid 0; one such instance per number),
 * except a real-time signal sent with information (sigqueue, tgkill, pidfd_send_signal, si_code != SI_USER), which
 * fails with EAGAIN. Counters per kernel uid (not per user namespace); a full table stops counting (never refuses). */
#define PENDING_USERS 256
static struct { uint32_t uid_plus1, count; } pending_users[PENDING_USERS];
static uint32_t* pending_counter(uint32_t uid) {
    unsigned free_slot = PENDING_USERS;
    for (unsigned i = 0; i < PENDING_USERS; i++) {
        if (pending_users[i].uid_plus1 == uid + 1) return &pending_users[i].count;
        if (!pending_users[i].count && free_slot == PENDING_USERS) free_slot = i;
    }
    if (free_slot == PENDING_USERS) return NULL;
    pending_users[free_slot].uid_plus1 = uid + 1; pending_users[free_slot].count = 0; return &pending_users[free_slot].count;
}
static bool pending_charge(uint32_t uid, uint64_t limit, bool override, uint32_t* charged) {
    uint32_t* counter = pending_counter(uid);
    if (!counter) { *charged = 0; return true; }
    if (!override && (uint64_t)*counter + 1 > limit) { *charged = 0; return false; }
    if (*counter != 0xFFFFFFFFU) (*counter)++;
    *charged = uid + 1; return true;
}
void sigpending_uncharge(uint32_t charged) {
    if (!charged) return;
    for (unsigned i = 0; i < PENDING_USERS; i++)
        if (pending_users[i].uid_plus1 == charged) { if (pending_users[i].count) pending_users[i].count--; return; }
}
/* signal.c: 0 charged (*charged), 1 queue it without information, -11 refuse (EAGAIN). */
int sigpending_charge(struct signal_state* state, unsigned number, int code, uint32_t* charged) {
    uint32_t uid; uint64_t limit; *charged = 0;
    if (!task_signal_owner(state, &uid, &limit)) return 0;   /* no task yet (boot): not counted */
    if (pending_charge(uid, limit, number < 32 && code >= 0, charged)) return 0;
    return number >= 32 && code != 0 ? -11 : 1;
}
/* posix_timers.c timer_create: the timer's signal is preallocated against the caller (false: EAGAIN). */
bool sigpending_charge_current(uint32_t* charged) {
    struct credentials* self = credentials_current();
    return pending_charge(self ? credentials_kernel_id(self, false, 0) : 0, task_rlimit_soft(11), false, charged);
}
uint32_t sigpending_count(uint32_t uid) {   /* /proc/PID/status SigQ */
    for (unsigned i = 0; i < PENDING_USERS; i++) if (pending_users[i].uid_plus1 == uid + 1) return pending_users[i].count;
    return 0;
}
