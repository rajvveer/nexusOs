/* POSIX interval timers (timer_create, timer_settime, timer_gettime, timer_getoverrun, timer_delete), timerfd
 * (timerfd_create/settime/gettime, read, poll/epoll, TFD_TIMER_ABSTIME, TFD_TIMER_CANCEL_ON_SET, TFD_IOC_SET_TICKS),
 * ITIMER_VIRTUAL/ITIMER_PROF, and the CPU-time clocks of other threads and processes (clock_getcpuclockid).
 *
 * One engine serves all of them. A timer's expiry is kept in its clock's own time ("domain"): MONO (CLOCK_MONOTONIC,
 * and relative CLOCK_REALTIME/TAI timers, which Linux runs on the monotonic base), REAL (absolute CLOCK_REALTIME,
 * _ALARM, TAI: realtime ns), BOOT (CLOCK_BOOTTIME, _ALARM: monotonic + time suspended) and CPU (CPU-time clocks).
 * Armed MONO/REAL/BOOT timers sit in an intrusive pairing heap ordered by `when`, the monotonic instant of their next
 * check; REAL timers are checked a little early and re-checked (realtime may be slewed), and every clock step
 * (clock_settime, settimeofday, adjtimex ADJ_SETOFFSET, resume from sleep: posix_clock_was_set) re-evaluates them and
 * cancels TFD_TIMER_CANCEL_ON_SET timerfds. Armed CPU timers are on a list and are looked at no later than CPU time
 * could reach their expiry (a process's clock advances at most cpu_count times as fast as wall time), with a bounded
 * number of clock reads per pass. The expiry pass runs from trap64's timer-interrupt path (process.c), big lock held,
 * once `next_deadline` (lowered through task_timer_deadline) is due: 1 ms granularity, like every kernel timeout here.
 * The _ALARM clocks (CAP_WAKE_ALARM) run as REALTIME (relative ones too, as Linux's alarmtimer) / BOOTTIME and wake
 * the machine (Linux alarmtimer_suspend): at suspend the earliest armed _ALARM timer is programmed into the RTC alarm
 * through power.c (power_alarm_suspend, unless the user's wakealarm comes first; the user's alarm is put back at
 * resume), and a suspend with an alarm due within 2 s is refused with EBUSY.
 *
 * Expiry: n = 1 + missed periods (Linux hrtimer_forward). A POSIX timer queues its signal (si_code SI_TIMER, si_timerid,
 * si_value) unless its previous one is still queued, in which case the expirations add to si_overrun (saturating at
 * DELAYTIMER_MAX); timer_getoverrun reports the overrun of the last delivered signal. Re-arming or deleting a timer takes
 * its queued signal back (Linux 6.13+: no stale timer signal is delivered). SIGEV_THREAD_ID signals go to that thread
 * (none once it has exited, as Linux), others to the process. A timerfd adds n to its tick count and wakes readers.
 *
 * Lifetime: POSIX timers belong to the process (deleted at exec and exit, not inherited by fork), itimers survive exec;
 * a timerfd belongs to its open file description. A queued signal holds a reference on its timer (a deleted timer is
 * freed when that signal leaves the queue). Limits: each POSIX timer's preallocated signal counts against the creator's RLIMIT_SIGPENDING (per real
 * uid, with the queued signals: signalfd.c sigpending_*; EAGAIN beyond, as Linux) and the kernel at most TIMERS_MAX timer objects in all.
 * Locking: the big kernel lock for everything (syscalls and the expiry pass); user memory is only touched before or
 * after a timer is updated, never in between. */
#include "native.h"
#include "uaccess.h"
#include "power.h"
#include "../../kernel/vga.h"
#include "../../kernel/string.h"

#define NSEC 1000000000ULL
#define KTIME_MAX 0x7FFFFFFFFFFFFFFFULL
#define DELAYTIMER_MAX 0x7FFFFFFFU
#define TIMERS_MAX 65536U
#define SI_TIMER (-2)
#define SIGEV_SIGNAL 0
#define SIGEV_NONE 1
#define SIGEV_THREAD 2
#define SIGEV_THREAD_ID 4
#define TIMER_ABSTIME 1
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2
#define TFD_IOC_SET_TICKS 0x40085400ULL
#define CAP_WAKE_ALARM 35
#define CPU_CHECKS_PER_PASS 512U
#define CPU_CHECK_MIN_NS 1000000ULL

enum { DOMAIN_MONO, DOMAIN_REAL, DOMAIN_BOOT, DOMAIN_CPU };
enum { KIND_POSIX, KIND_TIMERFD, KIND_ITIMER };
struct ktimer {
    struct ktimer* hash_next;                    /* POSIX/itimer: (owner, id) hash chain */
    struct ktimer *ph_child, *ph_next, *ph_prev; /* pairing heap of armed MONO/REAL/BOOT timers */
    struct ktimer *list_next, *list_prev;        /* armed CPU timers, or REAL/BOOT/cancel-on-set timers (clock steps) */
    uint64_t expires, interval;                  /* in the domain's clock (ns); interval 0: one-shot */
    uint64_t when;                               /* monotonic ns of the next check (heap key) */
    uint64_t ticks;                              /* timerfd: expirations not read yet */
    uint64_t sigval;
    uint32_t owner;                              /* POSIX/itimer: internal pid of the owning process */
    uint32_t measured;                           /* CPU clocks: the internal pid/tid whose CPU time counts */
    uint32_t receiver;                           /* SIGEV_THREAD_ID: internal tid of the receiving thread */
    uint32_t queued_tid;                         /* where its queued signal waits: a thread, 0 = the process queue */
    uint32_t overrun, overrun_last;
    uint32_t charged;                            /* POSIX: RLIMIT_SIGPENDING charge (signalfd.c sigpending_*) */
    uint32_t settime_flags;                      /* timerfd: the last timerfd_settime flags (fdinfo) */
    int32_t clock, id, signo;
    unsigned references;                         /* owner (table or descriptor) + 1 while its signal is queued */
    uint8_t kind, domain, notify;
    bool armed, in_heap, on_list, queued, deleted, cpu_thread, cancel_on_set, canceled;
};
struct timer_process { struct timer_process* next; uint32_t pid; unsigned count; int32_t next_id; };
static struct kslab timer_slab = {.size = (sizeof(struct ktimer) + 7) & ~7U};
static struct kslab process_slab = {.size = (sizeof(struct timer_process) + 15) & ~7U};
#define HASH_BUCKETS 256U
static struct ktimer* hash[HASH_BUCKETS];
static struct timer_process* processes;
static struct ktimer* heap_root;
static struct ktimer *cpu_timers, *rel_timers;
static unsigned timers_total;
static volatile bool clock_step_pending;

static unsigned bucket(uint32_t owner, int32_t id) {
    return (unsigned)(((uint64_t)owner * 0x9E3779B97F4A7C15ULL + (uint64_t)(uint32_t)id * 0x632BE59BD9B4E019ULL) >> 56) & (HASH_BUCKETS - 1);
}
static uint64_t sat_add(uint64_t a, uint64_t b) {
    if (a > KTIME_MAX) a = KTIME_MAX;
    if (b > KTIME_MAX) b = KTIME_MAX;
    return a > KTIME_MAX - b ? KTIME_MAX : a + b;
}

/* ---- pairing heap (intrusive: arming never allocates) ---- */
static struct ktimer* meld(struct ktimer* a, struct ktimer* b) {   /* two roots without siblings */
    if (!a) return b;
    if (!b) return a;
    if (b->when < a->when) { struct ktimer* t = a; a = b; b = t; }
    b->ph_prev = a; b->ph_next = a->ph_child; if (a->ph_child) a->ph_child->ph_prev = b; a->ph_child = b;
    return a;
}
static struct ktimer* merge_pairs(struct ktimer* first) {   /* two-pass merge of a sibling list */
    struct ktimer* pairs = NULL;
    while (first) {
        struct ktimer *a = first, *b = a->ph_next;
        first = b ? b->ph_next : NULL;
        a->ph_next = a->ph_prev = NULL;
        if (b) b->ph_next = b->ph_prev = NULL;
        struct ktimer* m = meld(a, b); m->ph_next = pairs; pairs = m;
    }
    struct ktimer* result = NULL;
    while (pairs) { struct ktimer* next = pairs->ph_next; pairs->ph_next = NULL; result = meld(result, pairs); pairs = next; }
    return result;
}
static void heap_remove(struct ktimer* t) {
    if (!t->in_heap) return;
    struct ktimer* children = t->ph_child; t->ph_child = NULL;
    if (t == heap_root) heap_root = merge_pairs(children);
    else {
        if (t->ph_prev->ph_child == t) t->ph_prev->ph_child = t->ph_next; else t->ph_prev->ph_next = t->ph_next;
        if (t->ph_next) t->ph_next->ph_prev = t->ph_prev;
        heap_root = meld(heap_root, merge_pairs(children));
    }
    t->ph_next = t->ph_prev = NULL; t->in_heap = false;
    if (heap_root) heap_root->ph_prev = NULL;
}
static void heap_insert(struct ktimer* t) {
    t->ph_child = t->ph_next = t->ph_prev = NULL; t->in_heap = true; heap_root = meld(heap_root, t);
}
static void list_insert(struct ktimer** head, struct ktimer* t) {
    if (t->on_list) return;
    t->list_prev = NULL; t->list_next = *head; if (*head) (*head)->list_prev = t; *head = t; t->on_list = true;
}
static void list_remove(struct ktimer** head, struct ktimer* t) {
    if (!t->on_list) return;
    if (t->list_prev) t->list_prev->list_next = t->list_next; else *head = t->list_next;
    if (t->list_next) t->list_next->list_prev = t->list_prev;
    t->list_next = t->list_prev = NULL; t->on_list = false;
}

/* ---- clocks ---- */
#define CPUCLOCK_PID(clock) ((int32_t)~((clock) >> 3))
#define CPUCLOCK_PERTHREAD(clock) (((clock) & 4) != 0)
#define CPUCLOCK_WHICH(clock) ((clock) & 3)
/* A CPU-time clock id (2, 3, or the negative encoding of clock_getcpuclockid/pthread_getcpuclockid) -> the measured
 * internal pid or tid (Linux pid_for_clock): thread clocks only within the caller's own thread group; process clocks
 * name a process (a zombie's clock stays readable until it is reaped); clock_gettime also accepts the caller's tid. */
static bool cpu_clock_target(int32_t clock, bool gettime, uint32_t* id, bool* thread) {
    if (clock == 2 || clock == 3) { *thread = clock == 3; *id = *thread ? task_tid() : task_pid(); return true; }
    if (clock >= 0 || CPUCLOCK_WHICH(clock) == 3) return false;
    *thread = CPUCLOCK_PERTHREAD(clock);
    int32_t visible = CPUCLOCK_PID(clock);
    if (visible < 0) return false;
    if (!visible) { *id = *thread ? task_tid() : task_pid(); return true; }
    uint32_t internal = task_internal_pid((uint32_t)visible); uint64_t ignored;
    if (!internal) return false;
    if (*thread) {
        if (!task_alive(internal) || task_process_id(internal) != task_pid()) return false;
        *id = internal; return true;
    }
    if (gettime && internal == task_tid()) { *id = task_pid(); return true; }
    if (!task_cpu_clock(internal, false, &ignored)) return false;   /* not a process (a non-leader thread's id) */
    *id = internal; return true;
}
bool posix_cpu_clock(int32_t clock, uint64_t* value) {   /* clock_gettime of CPU clocks given by id (kernel.c) */
    uint32_t id; bool thread;
    return cpu_clock_target(clock, true, &id, &thread) && task_cpu_clock(id, thread, value);
}
static uint64_t domain_now(const struct ktimer* t, uint64_t mono, bool* alive) {
    *alive = true;
    switch (t->domain) {
    case DOMAIN_REAL: return timekeeping_realtime(mono);
    case DOMAIN_BOOT: return mono + native_suspended_ns();
    case DOMAIN_CPU: { uint64_t value = 0; *alive = task_cpu_clock(t->measured, t->cpu_thread, &value); return value; }
    default: return mono;
    }
}
/* The monotonic instant at which to look at an armed timer again. */
static uint64_t next_check(const struct ktimer* t, uint64_t mono, uint64_t now) {
    if (now >= t->expires) return mono;
    uint64_t delta = t->expires - now;
    if (t->domain == DOMAIN_REAL && delta > 2000000) delta -= delta >> 10;   /* realtime may run slow (slew): look early */
    if (t->domain == DOMAIN_CPU) {
        unsigned rate = t->cpu_thread || !cpu_count ? 1 : cpu_count;
        delta /= rate; if (delta < CPU_CHECK_MIN_NS) delta = CPU_CHECK_MIN_NS;
    }
    return sat_add(mono, delta);
}

/* ---- arming ---- */
static void disarm(struct ktimer* t) {   /* a CANCEL_ON_SET timerfd stays on the clock-step list until settime/close */
    heap_remove(t);
    if (!(t->kind == KIND_TIMERFD && t->cancel_on_set)) list_remove(t->domain == DOMAIN_CPU ? &cpu_timers : &rel_timers, t);
    t->armed = false;
}
static void schedule(struct ktimer* t, uint64_t mono, uint64_t now) {   /* (re)queue an armed timer */
    heap_remove(t);
    t->when = next_check(t, mono, now);
    if (t->domain != DOMAIN_CPU) heap_insert(t);
    task_timer_deadline(t->when);
}
/* Remaining time and interval as Linux reports them (common_timer_get, timerfd_get_remaining): a due interval timer
 * reports its next period; a due one-shot reads 1 ns while its signal is not yet sent (0 for timerfd/SIGEV_NONE). */
static void remaining(const struct ktimer* t, uint64_t mono, int64_t out[4]) {
    uint64_t left = 0;
    if (t->armed) {
        bool alive; uint64_t now = domain_now(t, mono, &alive);
        if (alive && t->expires > now) left = t->expires - now;
        else if (alive && t->interval) left = t->interval - (now - t->expires) % t->interval;
        else left = t->kind == KIND_TIMERFD || t->notify == SIGEV_NONE ? 0 : 1;
    }
    out[0] = (int64_t)(t->interval / NSEC); out[1] = (int64_t)(t->interval % NSEC);
    out[2] = (int64_t)(left / NSEC); out[3] = (int64_t)(left % NSEC);
}
static int read_itimerspec(uint64_t address, uint64_t* interval, uint64_t* value) {
    if (!accessible(address, 32, false)) return -14;
    int64_t v[4]; copy_from_user(v, (const void*)address, sizeof(v));
    if (v[0] < 0 || v[1] < 0 || v[1] >= (int64_t)NSEC || v[2] < 0 || v[3] < 0 || v[3] >= (int64_t)NSEC) return -22;
    *interval = (uint64_t)v[0] >= KTIME_MAX / NSEC ? KTIME_MAX : (uint64_t)v[0] * NSEC + (uint64_t)v[1];
    *value = (uint64_t)v[2] >= KTIME_MAX / NSEC ? KTIME_MAX : (uint64_t)v[2] * NSEC + (uint64_t)v[3];
    return 0;
}
static int write_itimerspec(uint64_t address, const int64_t v[4]) {
    if (!accessible(address, 32, true)) return -14;
    copy_to_user((void*)address, v, 32); return 0;
}
/* Arm from a user setting (value 0 disarms). `absolute`: value is a time of the timer's clock. */
static void set_timer(struct ktimer* t, uint64_t interval, uint64_t value, bool absolute, uint64_t mono) {
    disarm(t);
    if (!value) { t->expires = t->interval = 0; return; }
    if (t->clock == 0 || t->clock == 11) t->domain = absolute ? DOMAIN_REAL : DOMAIN_MONO;
    if (t->clock == 8) { t->domain = DOMAIN_REAL; if (!absolute) { value = sat_add(timekeeping_realtime(mono), value); absolute = true; } }
    if (t->domain == DOMAIN_REAL && t->clock == 11) {   /* CLOCK_TAI = realtime + the TAI offset */
        uint64_t offset = (uint64_t)timekeeping_tai_seconds() * NSEC; value = value > offset ? value - offset : 0;
    }
    bool alive; uint64_t now = domain_now(t, mono, &alive);
    t->expires = absolute ? value : sat_add(now, value); t->interval = interval; t->armed = true;
    if (t->domain == DOMAIN_CPU) list_insert(&cpu_timers, t);
    else if (t->domain != DOMAIN_MONO) list_insert(&rel_timers, t);
    schedule(t, mono, now);
}

/* ---- POSIX timers ---- */
static struct timer_process* process_record(uint32_t pid, bool create) {
    for (struct timer_process* p = processes; p; p = p->next) if (p->pid == pid) return p;
    if (!create) return NULL;
    struct timer_process* p = kslab_alloc(&process_slab); if (!p) return NULL;
    p->pid = pid; p->next = processes; processes = p; return p;
}
static void process_record_drop(struct timer_process* record) {
    struct timer_process** link = &processes; while (*link && *link != record) link = &(*link)->next;
    if (*link) *link = record->next;
    kslab_free(&process_slab, record);
}
static struct ktimer* lookup(uint32_t owner, int32_t id, uint8_t kind) {
    for (struct ktimer* t = hash[bucket(owner, id)]; t; t = t->hash_next)
        if (t->owner == owner && t->id == id && t->kind == kind) return t;
    return NULL;
}
static void hash_insert(struct ktimer* t) { unsigned b = bucket(t->owner, t->id); t->hash_next = hash[b]; hash[b] = t; }
static void hash_remove(struct ktimer* t) {
    struct ktimer** link = &hash[bucket(t->owner, t->id)];
    while (*link && *link != t) link = &(*link)->hash_next;
    if (*link) *link = t->hash_next;
    t->hash_next = NULL;
}
static void timer_put(struct ktimer* t) {
    if (!t->references) panic("POSIX timer reference underflow");
    if (--t->references) return;
    if (t->armed || t->in_heap || t->on_list) panic("freeing an armed timer");
    timers_total--; kslab_free(&timer_slab, t);
}
/* Take a queued, not yet delivered signal of `t` back (re-arm, delete). */
static void unqueue(struct ktimer* t) {
    if (!t->queued) return;
    struct signal_state* state = task_signal_target(t->owner, t->queued_tid);
    if (state && signals_unqueue_timer(state, t)) { t->queued = false; timer_put(t); }
    /* else the receiver is exiting: destroying its queue reports the entry (posix_timer_unqueued) */
}
static void timer_destroy(struct ktimer* t) {   /* POSIX/itimer: leave the process, drop the owner's reference */
    disarm(t); unqueue(t); hash_remove(t); t->deleted = true;
    sigpending_uncharge(t->charged); t->charged = 0;
    if (t->kind == KIND_POSIX) {
        struct timer_process* record = process_record(t->owner, false);
        if (record && record->count) record->count--;
        if (record && !record->count) process_record_drop(record);
    }
    timer_put(t);
}
/* signal.c: a queued timer signal leaves its queue. `siginfo` set: it is being delivered (fills si_overrun, clears
 * the kernel-private word as Linux does with si_sys_private); NULL: discarded. False: drop it (the timer is gone). */
bool posix_timer_unqueued(void* cookie, void* siginfo) {
    struct ktimer* t = cookie;
    bool deliver = !t->deleted && siginfo;
    if (deliver) {
        uint64_t* data = (uint64_t*)((uint8_t*)siginfo + 16);
        t->overrun_last = t->overrun; t->overrun = 0;
        data[0] = (uint32_t)t->id | (uint64_t)t->overrun_last << 32; data[2] = 0;
    }
    t->queued = false; timer_put(t);
    return deliver;
}
static void post_expiry(struct ktimer* t, uint64_t count) {
    if (t->kind == KIND_TIMERFD) {
        t->ticks = t->ticks > ~0ULL - count ? ~0ULL : t->ticks + count;
        task_wake_channel(t); event_source_changed(t, 1); return;
    }
    if (t->notify == SIGEV_NONE) return;
    if (t->kind == KIND_ITIMER) {   /* SIGVTALRM/SIGPROF as Linux sends them: SI_KERNEL, a standard (merging) signal */
        int32_t info[32] = {0}; info[0] = t->signo; info[2] = 0x80; task_queue_info(t->owner, 0, info, NULL); return;
    }
    if (t->queued) {   /* its previous signal is still pending: these expirations are overruns */
        uint64_t total = (uint64_t)t->overrun + count; t->overrun = total > DELAYTIMER_MAX ? DELAYTIMER_MAX : (uint32_t)total; return;
    }
    int32_t info[32] = {0};   /* struct siginfo: signo, errno, code; timer fields at 16 (tid, overrun), 24 (value) */
    info[0] = t->signo; info[2] = SI_TIMER; info[4] = t->id; memcpy(&info[6], &t->sigval, 8);
    t->references++; t->queued = true; t->queued_tid = t->notify == SIGEV_THREAD_ID ? t->receiver : 0;
    if (task_queue_info(t->owner, t->queued_tid, info, t)) { t->queued = false; timer_put(t); return; }   /* ignored or gone */
    uint64_t total = (uint64_t)t->overrun + count - 1; t->overrun = total > DELAYTIMER_MAX ? DELAYTIMER_MAX : (uint32_t)total;
}
/* The timer was found due at `now` (its domain): count the expirations, re-arm or disarm, act. */
static void expire(struct ktimer* t, uint64_t mono, uint64_t now) {
    uint64_t count = 1;
    if (t->interval) {
        uint64_t missed = (now - t->expires) / t->interval;
        count = missed >= KTIME_MAX / 2 ? KTIME_MAX / 2 : missed + 1;
        t->expires = sat_add(t->expires, count > KTIME_MAX / t->interval ? KTIME_MAX : count * t->interval);
        if (t->expires == KTIME_MAX) disarm(t); else schedule(t, mono, now);
    } else disarm(t);
    post_expiry(t, count);
}
struct sigevent64 { uint64_t value; int32_t signo, notify, tid, pad[11]; };
_Static_assert(sizeof(struct sigevent64) == 64, "struct sigevent ABI");
static void alarm_register(void);
static int clock_domain(int32_t clock, uint8_t* domain) {
    switch (clock) {
    case 0: case 1: case 11: *domain = DOMAIN_MONO; return 0;   /* REALTIME/TAI: settime picks REAL for absolute times */
    case 7: *domain = DOMAIN_BOOT; return 0;
    case 8: case 9: *domain = clock == 9 ? DOMAIN_BOOT : DOMAIN_REAL;
        if (!credentials_capable(credentials_current(), NULL, CAP_WAKE_ALARM)) return -1;
        alarm_register(); return 0;
    case 4: case 5: case 6: return -95;                          /* no timers on MONOTONIC_RAW or the coarse clocks */
    default: return -22;
    }
}
static int64_t timer_create_call(int32_t clock, uint64_t event_address, uint64_t id_address) {
    if (!task_active()) return -22;
    struct sigevent64 event = {.signo = 14, .notify = SIGEV_SIGNAL};
    if (event_address) {
        if (!accessible(event_address, sizeof(event), false)) return -14;
        copy_from_user(&event, (const void*)event_address, sizeof(event));
    }
    uint8_t domain; bool thread_clock = false; uint32_t measured = 0, receiver = 0;
    if (clock == 2 || clock == 3 || clock < 0) {
        if (!cpu_clock_target(clock, false, &measured, &thread_clock)) return -22;
        domain = DOMAIN_CPU;
    } else { int error = clock_domain(clock, &domain); if (error) return error; }
    if (event.notify == (SIGEV_SIGNAL | SIGEV_THREAD_ID)) {   /* Linux good_sigevent */
        receiver = event.tid > 0 ? task_internal_pid((uint32_t)event.tid) : 0;
        if (!receiver || !task_alive(receiver) || task_process_id(receiver) != task_pid()) return -22;
    } else if (event.notify != SIGEV_SIGNAL && event.notify != SIGEV_THREAD && event.notify != SIGEV_NONE) return -22;
    if (event.notify != SIGEV_NONE && (event.signo <= 0 || event.signo > 64)) return -22;
    struct timer_process* record = process_record(task_pid(), true); if (!record) return -11;
    uint32_t charged = 0;   /* the timer's signal is preallocated against the caller's RLIMIT_SIGPENDING (Linux) */
    struct ktimer* t = timers_total < TIMERS_MAX && sigpending_charge_current(&charged) ? kslab_alloc(&timer_slab) : NULL;
    if (!t) { sigpending_uncharge(charged); if (!record->count) process_record_drop(record); return -11; }
    t->charged = charged;
    int32_t id = record->next_id;   /* Linux: increasing ids per process, wrapping at INT_MAX */
    while (lookup(task_pid(), id, KIND_POSIX)) id = id == 0x7FFFFFFF ? 0 : id + 1;
    record->next_id = id == 0x7FFFFFFF ? 0 : id + 1; record->count++; timers_total++;
    t->kind = KIND_POSIX; t->id = id; t->owner = task_pid(); t->clock = clock; t->domain = domain; t->references = 1;
    t->notify = (uint8_t)event.notify; t->measured = measured; t->cpu_thread = thread_clock; t->receiver = receiver;
    t->signo = event.signo; t->sigval = event_address ? event.value : (uint64_t)(uint32_t)id;
    hash_insert(t);
    if (!accessible(id_address, 4, true)) { timer_destroy(t); return -14; }
    if (uput32(id_address, (uint32_t)id)) { timer_destroy(t); return -14; }
    return 0;
}
static struct ktimer* own_timer(uint64_t id) {
    if (!task_active() || (int32_t)id < 0) return NULL;
    return lookup(task_pid(), (int32_t)id, KIND_POSIX);
}

/* ---- timerfd ---- */
static int64_t timerfd_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct ktimer* t = object;
    if (write || total < 8) return -22;
    for (;;) {
        if (t->canceled) { t->canceled = false; t->ticks = 0; return -125; }   /* ECANCELED: the clock was set */
        if (t->ticks) break;
        if (flags & 0x800) return -11;
        int error = task_wait_channel(t); if (error) return error;
    }
    if (!iov_access(vectors, count, 0, 8, true)) return -14;
    uint64_t ticks = t->ticks; t->ticks = 0;
    iov_copy(vectors, count, 0, (uint8_t*)&ticks, 8, false);
    return 8;
}
static int timerfd_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct ktimer* t = object;
    if (command != TFD_IOC_SET_TICKS) return -25;
    if (!accessible(argument, 8, false)) return -14;
    uint64_t ticks; copy_from_user(&ticks, (const void*)argument, 8);
    if (!ticks) return -22;
    t->ticks = ticks; task_wake_channel(t); event_source_changed(t, 1); return 0;
}
static uint32_t timerfd_ready(void* object) { struct ktimer* t = object; return t->ticks || t->canceled ? 1U : 0; }
static void* timerfd_source(void* object) { return object; }
static void timerfd_release(void* object) {
    struct ktimer* t = object; disarm(t); list_remove(&rel_timers, t); t->deleted = true; timer_put(t);
}
static void anon_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 6; stat->ino = 1 + ((uint64_t)object >> 3 & 0xFFFFFFFULL); stat->nlink = 1; stat->mode = 0600;
    stat->uid = credentials_visible_id(credentials_kernel_id(credentials_current(), false, 1), false);
    stat->gid = credentials_visible_id(credentials_kernel_id(credentials_current(), true, 1), true); stat->block_size = 4096;
}
/* /proc/PID/fdinfo (Linux timerfd_show): clock, unread ticks, the last settime flags, the setting as timerfd_gettime */
static unsigned timerfd_fdinfo(void* object, char* out, unsigned capacity) {
    struct ktimer* t = object; int64_t v[4]; remaining(t, native_clock_ns(), v);
    return kfmt(out, capacity, 0, "clockid: %d\nticks: %llu\nsettime flags: 0%o\nit_value: (%llu, %llu)\nit_interval: (%llu, %llu)\n",
                t->clock, t->ticks, t->settime_flags, (uint64_t)v[2], (uint64_t)v[3], (uint64_t)v[0], (uint64_t)v[1]);
}
static const struct object_operations timerfd_operations = {timerfd_io, timerfd_ioctl, timerfd_ready, timerfd_source, timerfd_release, anon_stat};
unsigned timerfd_fdinfo_text(struct open_file* description, char* out, unsigned capacity) {   /* kernel.c fd_information_text */
    void* object = fd_object(description, &timerfd_operations); return object ? timerfd_fdinfo(object, out, capacity) : 0;
}
static int64_t timerfd_create(int32_t clock, uint64_t flags) {
    if ((flags & ~(0x800ULL | 0x80000ULL)) || (clock != 0 && clock != 1 && clock != 7 && clock != 8 && clock != 9)) return -22;
    uint8_t domain; int error = clock_domain(clock, &domain); if (error) return error;
    if (timers_total >= TIMERS_MAX) return -12;
    struct ktimer* t = kslab_alloc(&timer_slab); if (!t) return -12;
    t->kind = KIND_TIMERFD; t->clock = clock; t->domain = domain; t->references = 1; t->id = -1; timers_total++;
    int fd = fd_create_object(&timerfd_operations, t, (flags & (0x800 | 0x80000)) | 2, "anon_inode:[timerfd]");
    if (fd < 0) { timers_total--; kslab_free(&timer_slab, t); }
    return fd;
}
static int64_t timerfd_call(struct registers* r) {
    if (r->rax == 283) return timerfd_create((int32_t)r->rdi, r->rsi);
    uint64_t flags = (uint32_t)r->rsi, interval = 0, value = 0; int error = 0;
    if (r->rax == 286) {   /* timerfd_settime(fd, flags, new, old): Linux checks the setting before the descriptor */
        if (!accessible(r->rdx, 32, false)) return -14;
        if ((flags & ~(uint64_t)(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) || (error = read_itimerspec(r->rdx, &interval, &value))) return error ? error : -22;
    }
    struct open_file* f = fd_lookup(r->rdi); if (!f) return -9;
    struct ktimer* t = fd_object(f, &timerfd_operations); if (!t) return -22;
    uint64_t mono = native_clock_ns(); int64_t old[4];
    if (r->rax == 287) { remaining(t, mono, old); return write_itimerspec(r->rsi, old); }
    remaining(t, mono, old);
    t->ticks = 0; t->canceled = false; disarm(t); list_remove(&rel_timers, t); t->settime_flags = (uint32_t)flags;
    t->cancel_on_set = (t->clock == 0 || t->clock == 8) && (flags & TFD_TIMER_ABSTIME) && (flags & TFD_TIMER_CANCEL_ON_SET);
    set_timer(t, interval, value, (flags & TFD_TIMER_ABSTIME) != 0, mono);
    if (t->cancel_on_set) list_insert(&rel_timers, t);   /* even disarmed, a clock step cancels it (Linux might_cancel) */
    return r->r10 ? write_itimerspec(r->r10, old) : 0;
}

/* ---- the expiry pass (process.c trap64, big lock, timer interrupt) ---- */
void posix_clock_was_set(void) { __atomic_store_n(&clock_step_pending, true, __ATOMIC_RELEASE); task_timer_deadline(0); }
static void clock_stepped(uint64_t mono) {
    for (struct ktimer *t = rel_timers, *next; t; t = next) {
        next = t->list_next;
        if (t->kind == KIND_TIMERFD && t->cancel_on_set) { t->canceled = true; task_wake_channel(t); event_source_changed(t, 1); }
        if (!t->armed) continue;
        bool alive; schedule(t, mono, domain_now(t, mono, &alive));
    }
}
void posix_timers_expire(uint64_t mono) {
    if (__atomic_exchange_n(&clock_step_pending, false, __ATOMIC_ACQ_REL)) clock_stepped(mono);
    for (unsigned guard = 0; heap_root && heap_root->when <= mono && guard < 4096; guard++) {
        struct ktimer* t = heap_root; bool alive; uint64_t now = domain_now(t, mono, &alive);
        if (now >= t->expires) expire(t, mono, now);
        else schedule(t, mono, now);   /* a REAL timer looked at early, or the clock moved back: not yet */
    }
    unsigned budget = CPU_CHECKS_PER_PASS; uint64_t later = ~0ULL;
    for (struct ktimer *t = cpu_timers, *next; t; t = next) {
        next = t->list_next;
        if (t->when > mono || !budget) { uint64_t w = t->when > mono ? t->when : mono + CPU_CHECK_MIN_NS; if (w < later) later = w; continue; }
        budget--;
        bool alive; uint64_t now = domain_now(t, mono, &alive);
        if (!alive) disarm(t);   /* the measured thread or process is gone: the timer never fires */
        else if (now >= t->expires) expire(t, mono, now);
        else schedule(t, mono, now);
    }
    if (later != ~0ULL) task_timer_deadline(later);
    if (heap_root) task_timer_deadline(heap_root->when);
}

/* ---- process lifetime (process.c) ---- */
static void delete_owned(uint32_t pid, bool itimers) {
    for (unsigned b = 0; b < HASH_BUCKETS; b++)
        for (struct ktimer *t = hash[b], *next; t; t = next) {
            next = t->hash_next;
            if (t->owner == pid && (t->kind == KIND_POSIX || itimers)) timer_destroy(t);
        }
}
void posix_timers_exec(uint32_t pid) { if (timers_total && process_record(pid, false)) delete_owned(pid, false); }   /* itimers survive exec */
void posix_timers_exit(uint32_t pid) {
    if (!timers_total) return;
    delete_owned(pid, true);
    struct timer_process* record = process_record(pid, false); if (record) process_record_drop(record);
}

/* ---- ITIMER_VIRTUAL (1) / ITIMER_PROF (2): process CPU time (all of it counts as user time here); SIGVTALRM / SIGPROF.
 * call 36 getitimer(which, value), 38 setitimer(which, value, old). ---- */
int64_t posix_itimer_cpu(uint64_t call, uint64_t which, uint64_t value, uint64_t old) {
    if (!task_active()) return -22;
    struct ktimer* t = lookup(task_pid(), -(int32_t)which, KIND_ITIMER);
    int64_t o[4] = {0, 0, 0, 0};
    if (t) remaining(t, native_clock_ns(), o);
    int64_t timeval[4] = {o[0], o[1] / 1000, o[2], o[3] / 1000};
    if (t && t->armed && !timeval[2] && !timeval[3]) timeval[3] = 1;   /* armed never reads as {0, 0} */
    uint64_t out = call == 36 ? value : old;
    if (call == 36) { if (!out || !accessible(out, 32, true)) return -14; copy_to_user((void*)out, timeval, 32); return 0; }
    int64_t v[4] = {0, 0, 0, 0};   /* a NULL new value disarms (Linux keeps this misfeature) */
    if (value && !accessible(value, 32, false)) return -14;
    if (value) copy_from_user(v, (const void*)value, sizeof(v));
    if (v[0] < 0 || v[1] < 0 || v[1] >= 1000000 || v[2] < 0 || v[3] < 0 || v[3] >= 1000000) return -22;
    if (out && !accessible(out, 32, true)) return -14;
    uint64_t interval = (uint64_t)v[0] >= KTIME_MAX / NSEC ? KTIME_MAX : (uint64_t)v[0] * NSEC + (uint64_t)v[1] * 1000;
    uint64_t delay = (uint64_t)v[2] >= KTIME_MAX / NSEC ? KTIME_MAX : (uint64_t)v[2] * NSEC + (uint64_t)v[3] * 1000;
    if (!t && delay) {
        if (timers_total >= TIMERS_MAX || !(t = kslab_alloc(&timer_slab))) return -11;
        timers_total++;
        t->kind = KIND_ITIMER; t->id = -(int32_t)which; t->owner = task_pid(); t->clock = 2; t->domain = DOMAIN_CPU;
        t->measured = task_pid(); t->references = 1; t->notify = SIGEV_SIGNAL; t->signo = which == 1 ? 26 : 27;
        hash_insert(t);
    }
    if (out) copy_to_user((void*)out, timeval, 32);
    if (t && delay) set_timer(t, interval, delay, false, native_clock_ns());
    else if (t) timer_destroy(t);
    return 0;
}

/* ---- system calls: 222 timer_create, 223 timer_settime, 224 timer_gettime, 225 timer_getoverrun, 226 timer_delete,
 * 283 timerfd_create, 286 timerfd_settime, 287 timerfd_gettime ---- */
int64_t posix_timer_call(struct registers* r) {
    switch (r->rax) {
    case 222: return timer_create_call((int32_t)r->rdi, r->rsi, r->rdx);
    case 223: {
        if (!r->rdx) return -22;   /* Linux order: the setting, then the timer id */
        uint64_t interval, value; int error = read_itimerspec(r->rdx, &interval, &value); if (error) return error;
        struct ktimer* t = own_timer(r->rdi); if (!t) return -22;
        uint64_t mono = native_clock_ns(); int64_t old[4]; remaining(t, mono, old);
        unqueue(t); t->overrun = t->overrun_last = 0;
        set_timer(t, interval, value, (r->rsi & TIMER_ABSTIME) != 0, mono);
        return r->r10 ? write_itimerspec(r->r10, old) : 0;   /* the timer is set even when old is unwritable (Linux) */
    }
    case 224: { struct ktimer* t = own_timer(r->rdi); if (!t) return -22; int64_t v[4]; remaining(t, native_clock_ns(), v); return write_itimerspec(r->rsi, v); }
    case 225: { struct ktimer* t = own_timer(r->rdi); return t ? (int64_t)t->overrun_last : -22; }
    case 226: { struct ktimer* t = own_timer(r->rdi); if (!t) return -22; timer_destroy(t); return 0; }
    case 283: case 286: case 287: return timerfd_call(r);
    }
    return -38;
}
unsigned posix_timer_objects(void) { return timers_total; }   /* leak checks */

/* ---- the _ALARM clocks wake a suspended machine (Linux alarmtimer_suspend / alarmtimer_resume) ----
 * Armed REALTIME_ALARM (REAL domain) and BOOTTIME_ALARM (BOOT domain) timers are all on rel_timers. */
int power_alarm_suspend(uint64_t when_seconds);   /* power.c: arm the RTC unless the user's wakealarm is earlier */
void power_alarm_resume(void);                     /* power.c: the user's wakealarm again */
static int alarm_suspend(void* context, unsigned state) {
    (void)context; (void)state;
    uint64_t mono = native_clock_ns(), real = timekeeping_realtime(mono), first = ~0ULL;
    for (struct ktimer* t = rel_timers; t; t = t->list_next) {
        if (!t->armed || (t->clock != 8 && t->clock != 9)) continue;
        bool alive; uint64_t now = domain_now(t, mono, &alive), left = t->expires > now ? t->expires - now : 0;
        uint64_t at = sat_add(real, left); if (at < first) first = at;
    }
    if (first == ~0ULL) return 0;
    if (first < real + 2000000000ULL) return -16;   /* due within 2 s: EBUSY, the suspend is refused (Linux) */
    int error = power_alarm_suspend((first + 999999999ULL) / 1000000000ULL);
    if (error) vga_print("[64] ALARMTIMER: the RTC cannot be set that far ahead; the alarm will not wake the machine\n");
    else vga_print("[64] ALARMTIMER: RTC wake armed\n");
    return 0;
}
static void alarm_resume(void* context, unsigned state) { (void)context; (void)state; power_alarm_resume(); }
static const struct pm_ops alarm_pm = {"alarmtimer", alarm_suspend, alarm_resume, NULL};
static void alarm_register(void) { static bool done; if (!done && !pm_register(&alarm_pm)) done = true; }
