/* Signal frames follow the x86-64 userspace ABI; delivery is implemented by NexusOS. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define UNBLOCKABLE ((1ULL << 8) | (1ULL << 18))
#define SA_RESTORER 0x04000000ULL
#define SA_ONSTACK 0x08000000ULL
#define SA_NODEFER 0x40000000ULL
#define SA_RESETHAND 0x80000000ULL
uint64_t task_ptrace_sender(void); /* process.c: uid << 32 | pid of the tracer that last resumed this thread */
struct action { uint64_t handler, flags, restorer, mask; };
struct altstack { uint64_t sp; uint32_t flags, pad; uint64_t size; };
struct sigcontext64 {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, flags;
    uint16_t cs, gs, fs, ss;
    uint64_t error, trap, oldmask, cr2, fpstate, reserved[8];
};
struct ucontext64 { uint64_t flags, link; struct altstack stack; struct sigcontext64 registers; uint64_t mask; };
struct info64 { int32_t number, error, code, pad; uint64_t data[14]; };
struct signal_frame { uint64_t restorer; struct ucontext64 context; struct info64 info; };
struct queued_signal { struct queued_signal* next; struct info64 info; void* timer; /* POSIX timer cookie (posix_timers.c) */
                       uint32_t charged; /* RLIMIT_SIGPENDING charge (kabi.c sigpending_*): uid + 1, 0 none */ };
/* Entries come from a slab (kslab.c), not a page each; one with a timer cookie reports its removal to the timer. */
static struct kslab entry_slab = {.size = sizeof(struct queued_signal)};
static struct queued_signal* entry_new(void) { return kslab_alloc(&entry_slab); }
static void entry_free(struct queued_signal* entry) { if (entry->charged) sigpending_uncharge(entry->charged); kslab_free(&entry_slab, entry); }
static void entry_drop(struct queued_signal* entry) {
    if (entry->timer) posix_timer_unqueued(entry->timer, NULL);
    entry_free(entry);
}
/* A queued signal over RLIMIT_SIGPENDING without its siginfo (Linux: the pending bit only, delivered as SI_USER with
 * pid and uid 0): one such instance per number. */
static bool infoless_pending(struct queued_signal* queue, unsigned number) {
    for (; queue; queue = queue->next) if ((unsigned)queue->info.number == number && !queue->charged && !queue->timer && !queue->info.code && !queue->info.data[0]) return true;
    return false;
}
_Static_assert(sizeof(struct sigcontext64) == 256 && sizeof(struct ucontext64) == 304 &&
               sizeof(struct signal_frame) == 440 && sizeof(struct action) == 32, "signal ABI layout");
struct handler_table { unsigned references; struct action entries[65]; };
struct group_pending { unsigned references; struct queued_signal* queue; };
struct signal_state {
    struct handler_table* handlers; struct group_pending* group; struct altstack stack; uint64_t mask; struct queued_signal* queue;
    uint64_t saved_mask; bool restore_mask;
    uint64_t fault_address;   /* the last bad page-fault address (Linux thread.cr2), saved before any sleep */
};
static struct handler_table boot_handlers;
static struct group_pending boot_pending;
static struct signal_state boot_signals = {.handlers = &boot_handlers, .group = &boot_pending, .stack = {0, 2, 0, 0}};
static struct signal_state* active_signals = &boot_signals;
#define actions (active_signals->handlers->entries)
#define alternate (active_signals->stack)
#define blocked (active_signals->mask)
#define pending (active_signals->queue)
static uint8_t fpu[FPU_AREA_BYTES + 64] __attribute__((aligned(64)));
#define FP_XSTATE_MAGIC1 0x46505853U
#define FP_XSTATE_MAGIC2 0x46505845U
static uint64_t fp_frame_bytes(void) { return xsave_mask ? xsave_size + 4 : 512; }
static uint32_t mxcsr_mask;

static bool default_ignore(unsigned number) { return number == 17 || number == 18 || number == 23 || number == 28; }
static bool on_altstack(uint64_t rsp) {
    return !(alternate.flags & 2) && rsp >= alternate.sp && rsp - alternate.sp < alternate.size;
}
struct signal_state* signals_current(void) { return active_signals; }
void signals_activate(struct signal_state* state) { active_signals = state ? state : &boot_signals; }
struct signal_state* signals_create(struct signal_state* source, bool share_handlers) {
    if (!mxcsr_mask) {
        memset(fpu, 0, sizeof(fpu)); __asm__ volatile("fxsave64 (%0)" :: "r"(fpu) : "memory");
        mxcsr_mask = *(uint32_t*)(fpu + 28); if (!mxcsr_mask) mxcsr_mask = 0xFFBF;
    }
    struct signal_state* state = (void*)page_alloc(); if (!state) return NULL;
    if (source && share_handlers) {
        if (source->handlers->references == 0xFFFFFFFFU) { page_free((uint64_t)state); return NULL; }
        state->handlers = source->handlers; state->handlers->references++;
    } else {
        state->handlers = (void*)page_alloc();
        if (!state->handlers) { page_free((uint64_t)state); return NULL; }
        if (source) memcpy(state->handlers->entries, source->handlers->entries, sizeof(state->handlers->entries));
        state->handlers->references = 1;
    }
    state->stack = source ? source->stack : (struct altstack){0, 2, 0, 0};
    state->mask = source ? source->mask : 0;
    state->fault_address = source ? source->fault_address : 0;
    state->group = (void*)page_alloc();
    if (!state->group) {
        if (!--state->handlers->references) page_free((uint64_t)state->handlers);
        page_free((uint64_t)state); return NULL;
    }
    state->group->references = 1;
    return state;
}
static void clear_queue(struct queued_signal* queue) {
    while (queue) { struct queued_signal* next = queue->next; entry_drop(queue); queue = next; }
}
void signals_disable_altstack(struct signal_state* state) { state->stack = (struct altstack){0, 2, 0, 0}; }
bool signals_share_pending(struct signal_state* state, struct signal_state* source) {
    if (state->group->references != 1 || state->group->queue) panic("sharing nonempty signal group");
    if (source->group->references == 0xFFFFFFFFU) return false;
    page_free((uint64_t)state->group); state->group = source->group; state->group->references++; return true;
}
bool signals_masked(struct signal_state* state, unsigned number) { return number && (state->mask & (1ULL << (number - 1))); }
bool signals_has_handler(struct signal_state* state, unsigned number) { return number && number <= 64 && state->handlers->entries[number].handler > 1; }
void signals_destroy(struct signal_state* state) {
    if (!state || state == active_signals || state == &boot_signals) panic("invalid signal context release");
    clear_queue(state->queue);
    if (!state->group->references) panic("unowned process pending signals");
    if (!--state->group->references) { clear_queue(state->group->queue); page_free((uint64_t)state->group); }
    if (!state->handlers->references) panic("unowned signal handlers");
    if (!--state->handlers->references) page_free((uint64_t)state->handlers);
    page_free((uint64_t)state);
}
struct signal_state* signals_exec(struct signal_state* source) {
    struct signal_state* state = signals_create(source, false); if (!state) return NULL;
    for (unsigned n = 1; n <= 64; n++)
        if (state->handlers->entries[n].handler != 1) memset(&state->handlers->entries[n], 0, sizeof(struct action));
    state->stack = (struct altstack){0, 2, 0, 0};
    state->queue = source->queue; source->queue = NULL;
    state->group->queue = source->group->queue; source->group->queue = NULL;
    return state;
}
static void discard(unsigned number) {
    for (unsigned group = 0; group < 2; group++) {
        struct queued_signal** link = group ? &active_signals->group->queue : &pending;
        while (*link) {
            struct queued_signal* entry = *link;
            if ((unsigned)entry->info.number == number) { *link = entry->next; entry_drop(entry); }
            else link = &entry->next;
        }
    }
}
int signal_action(uint64_t number, uint64_t address, uint64_t old, uint64_t size) {
    if (!number || number > 64 || (address && (number == 9 || number == 19)) || size != 8) return -22;
    struct action next;
    if (address) {
        if (!accessible(address, sizeof(next), false)) return -14;
        copy_from_user(&next, (void*)address, sizeof(next));
        /* Linux masks unknown flags, including glibc's sign-extended SA_RESETHAND. */
        next.flags &= 0xDC000007ULL;
        if (next.handler > 1 && (!user_range(next.handler, 1) || !(next.flags & SA_RESTORER) || !user_range(next.restorer, 1))) return -22;
        next.mask &= ~UNBLOCKABLE;
    }
    if (old && !accessible(old, sizeof(next), true)) return -14;
    if (old) copy_to_user((void*)old, &actions[number], sizeof(next));
    if (address) {
        actions[number] = next;
        if (next.handler == 1 || (!next.handler && default_ignore(number))) discard(number);
    }
    return 0;
}
int signal_mask_change(uint64_t how, uint64_t address, uint64_t old, uint64_t size) {
    if (size != 8 || (address && how > 2)) return -22;
    uint64_t value = 0;
    if (address) { if (!accessible(address, 8, false)) return -14; value = uget64(address) & ~UNBLOCKABLE; }
    if (old && !accessible(old, 8, true)) return -14;
    if (old) uput64(old, blocked);
    if (address) blocked = how == 0 ? blocked | value : how == 1 ? blocked & ~value : value;
    return 0;
}
int signal_pending_mask(uint64_t address, uint64_t size) {
    if (size != 8) return -22;
    if (!accessible(address, 8, true)) return -14;
    uint64_t mask = 0;
    for (struct queued_signal* p = pending; p; p = p->next) mask |= 1ULL << (p->info.number - 1);
    for (struct queued_signal* p = active_signals->group->queue; p; p = p->next) mask |= 1ULL << (p->info.number - 1);
    return uput64(address, mask);
}
static bool valid_altstack(struct altstack* stack) {
    if (stack->flags & ~(2U | 0x80000000U)) return false;
    return (stack->flags & 2) || (stack->size >= 2048 && user_range(stack->sp, stack->size));
}
int signal_altstack(uint64_t address, uint64_t old, uint64_t rsp) {
    struct altstack next;
    if (address) {
        if (!accessible(address, sizeof(next), false)) return -14;
        copy_from_user(&next, (void*)address, sizeof(next));
        if (on_altstack(rsp)) return -1;
        if (!(next.flags & 2) && next.size < 2048) return -12;
        if (!valid_altstack(&next)) return -22;
    }
    if (old && !accessible(old, sizeof(next), true)) return -14;
    if (old) {
        struct altstack previous = alternate;
        if (on_altstack(rsp)) previous.flags |= 1;
        copy_to_user((void*)old, &previous, sizeof(previous));
    }
    if (address) alternate = next;
    return 0;
}
int signals_enqueue(struct signal_state* state, uint64_t number, int code, uint32_t sender, uint32_t uid, int status, bool group) {
    if (number > 64) return -22;
    if (!number) return 0;
    struct action action = state->handlers->entries[number];
    if (!(state->mask & (1ULL << (number - 1))) && (action.handler == 1 ||
        (!action.handler && default_ignore(number)))) return 0;
    struct queued_signal** link = group ? &state->group->queue : &state->queue;
    while (*link) {
        if (number < 32 && (uint64_t)(*link)->info.number == number) return 0;
        link = &(*link)->next;
    }
    uint32_t charged = 0; int over = sigpending_charge(state, (unsigned)number, code, &charged);   /* kabi.c: RLIMIT_SIGPENDING */
    if (over < 0) return over;
    if (over) { if (infoless_pending(group ? state->group->queue : state->queue, (unsigned)number)) return 0; code = 0; sender = uid = 0; }
    struct queued_signal* entry = entry_new(); if (!entry) { if (charged) sigpending_uncharge(charged); return -11; }
    entry->info.number = number; entry->info.code = code; entry->charged = charged;
    entry->info.data[0] = ((uint64_t)uid << 32) | sender;
    if (number == 17 && code > 0) entry->info.data[1] = (uint32_t)status;
    *link = entry; signalfd_queued(state, (unsigned)number); return 0;
}
int signal_send(int64_t pid, uint64_t number, int code) {
    if (task_active()) return task_signal(pid, number, code);
    if (pid != 0 && pid != 1) return -3;
    return signals_enqueue(active_signals, number, code, 1, 1000, 0, false);
}
/* ---- kernel ABI additions (posix_timers.c, signalfd.c, pidfd.c) ---- */
/* Queue a caller-built Linux siginfo (128 bytes). A POSIX timer's entry (`timer` cookie) is queued even when another
 * instance of a standard signal is pending, as Linux queues a timer's preallocated sigqueue. 0 queued (or merged into
 * a pending standard signal), 1 discarded (ignored and not blocked), -11 no memory, -22 bad number. */
int signals_enqueue_info(struct signal_state* state, const void* siginfo, bool group, void* timer) {
    struct info64 info; memcpy(&info, siginfo, sizeof(info));
    unsigned number = (unsigned)info.number;
    if (!number || number > 64) return -22;
    struct action action = state->handlers->entries[number];
    if (!(state->mask & (1ULL << (number - 1))) && (action.handler == 1 || (!action.handler && default_ignore(number)))) return 1;
    struct queued_signal** link = group ? &state->group->queue : &state->queue;
    while (*link) {
        if (!timer && number < 32 && (unsigned)(*link)->info.number == number) return 0;
        link = &(*link)->next;
    }
    uint32_t charged = 0; int over = timer ? 0 : sigpending_charge(state, number, info.code, &charged);   /* timers: charged at creation */
    if (over < 0) return over;
    if (over) {   /* queued without its information (Linux "lose info") */
        if (infoless_pending(group ? state->group->queue : state->queue, number)) return 0;
        memset(&info, 0, sizeof(info)); info.number = (int32_t)number;
    }
    struct queued_signal* entry = entry_new(); if (!entry) { if (charged) sigpending_uncharge(charged); return -11; }
    entry->info = info; entry->timer = timer; entry->charged = charged; *link = entry; signalfd_queued(state, number); return 0;
}
/* si_uid of kill/tkill/SIGCHLD records holds the kernel uid: the receiver sees it through its user namespace. */
static void visible_sender(struct info64* info) {
    if (info->code == 0 || info->code == -6 || info->number == 17)
        info->data[0] = (uint32_t)info->data[0] | ((uint64_t)credentials_visible_id(info->data[0] >> 32, false) << 32);
}
/* rt_sigtimedwait / signalfd read: take the lowest-numbered pending signal in `mask`, the thread's own queue first
 * (Linux dequeue_signal), oldest instance first. Returns its number (0: none) and the receiver's siginfo. */
int signals_dequeue(uint64_t mask, void* siginfo) {
    for (;;) {
        struct queued_signal** chosen = NULL; unsigned number = 65;
        for (unsigned group = 0; group < 2 && !chosen; group++) {
            struct queued_signal** head = group ? &active_signals->group->queue : &pending;
            for (struct queued_signal** link = head; *link; link = &(*link)->next) {
                unsigned n = (*link)->info.number;
                if ((mask >> (n - 1) & 1) && n < number) { number = n; chosen = link; }
            }
        }
        if (!chosen) return 0;
        struct queued_signal* entry = *chosen; struct info64 info = entry->info; void* timer = entry->timer;
        *chosen = entry->next; entry_free(entry);
        if (timer && !posix_timer_unqueued(timer, &info)) continue;   /* a deleted timer's signal is dropped */
        visible_sender(&info); memcpy(siginfo, &info, sizeof(info)); return (int)number;
    }
}
/* signalfd readiness: a signal of `mask` pending for the thread `state` (its queue or its process's). */
bool signals_pending_set(struct signal_state* state, uint64_t mask) {
    for (unsigned group = 0; group < 2; group++)
        for (struct queued_signal* p = group ? state->group->queue : state->queue; p; p = p->next)
            if (mask >> (p->info.number - 1) & 1) return true;
    return false;
}
/* timer_settime/timer_delete: take a timer's queued signal back (false: not queued for this thread/process). */
bool signals_unqueue_timer(struct signal_state* state, void* timer) {
    for (unsigned group = 0; group < 2; group++) {
        struct queued_signal** link = group ? &state->group->queue : &state->queue;
        for (; *link; link = &(*link)->next) if ((*link)->timer == timer) {
            struct queued_signal* entry = *link; *link = entry->next; entry_free(entry); return true;
        }
    }
    return false;
}
/* clone3 CLONE_CLEAR_SIGHAND: handlers back to SIG_DFL, ignored signals stay ignored (Linux flush_signal_handlers). */
void signals_reset_handlers(struct signal_state* state) {
    for (unsigned n = 1; n <= 64; n++)
        if (state->handlers->entries[n].handler != 1) memset(&state->handlers->entries[n], 0, sizeof(struct action));
}
static struct queued_signal** next_signal(bool actionable) {
    struct queued_signal** chosen = NULL; unsigned number = 65;
    for (unsigned group = 0; group < 2; group++) {
        struct queued_signal** head = group ? &active_signals->group->queue : &pending;
        for (struct queued_signal** link = head; *link; link = &(*link)->next) {
            unsigned n = (*link)->info.number;
            if (!(blocked & (1ULL << (n - 1))) && n < number &&
                (!actionable || (actions[n].handler != 1 && (actions[n].handler || !default_ignore(n))))) {
                number = n; chosen = link;
            }
        }
    }
    return chosen;
}
bool signals_interrupt_pending(void) { return next_signal(true) != NULL; }
/* Linux fatal_signal_pending (after complete_signal's SIGKILL promotion): SIGKILL, or an unblocked signal whose
 * default action ends the process. Ends only killable waits (seccomp WAIT_KILLABLE_RECV). */
bool signals_fatal_pending(void) {
    for (unsigned group = 0; group < 2; group++)
        for (struct queued_signal* p = group ? active_signals->group->queue : pending; p; p = p->next) {
            unsigned n = (unsigned)p->info.number;
            if (n == 9) return true;
            if (n && n <= 64 && !(blocked & (1ULL << (n - 1))) && !actions[n].handler && !default_ignore(n) && !(n >= 19 && n <= 22)) return true;
        }
    return false;
}
bool signals_maybe_pending(struct signal_state* state) {
    return state && (__atomic_load_n(&state->queue, __ATOMIC_RELAXED) || __atomic_load_n(&state->group->queue, __ATOMIC_RELAXED));
}
/* ERESTARTNOHAND: the next signal only stops the process, so pause/sigsuspend resume afterwards. */
bool signals_stop_pending(void) {
    struct queued_signal** chosen = next_signal(true);
    if (!chosen) return false;
    unsigned number = (*chosen)->info.number; return number >= 19 && number <= 22 && !actions[number].handler;
}
/* Job control: drop queued signals in `mask` (thread and process queues). */
void signals_discard(struct signal_state* state, uint64_t mask) {
    for (unsigned group = 0; group < 2; group++) {
        struct queued_signal** link = group ? &state->group->queue : &state->queue;
        while (*link) {
            struct queued_signal* entry = *link;
            if (mask & (1ULL << (entry->info.number - 1))) { *link = entry->next; entry_drop(entry); }
            else link = &entry->next;
        }
    }
}
bool signals_ignored(struct signal_state* state, unsigned number) {
    return number && number <= 64 && (state->handlers->entries[number].handler == 1 ||
           (!state->handlers->entries[number].handler && default_ignore(number)));
}
bool signals_nocldstop(struct signal_state* state) { return (state->handlers->entries[17].flags & 1) != 0; }
bool signals_reap_children(struct signal_state* state) {
    return state->handlers->entries[17].handler == 1 || (state->handlers->entries[17].flags & 2);
}
bool signals_should_restart(void) {
    struct queued_signal** chosen = next_signal(true);
    if (!chosen) return false;
    unsigned number = (*chosen)->info.number; struct action action = actions[number];
    /* A default-action stop runs no handler: the call resumes after SIGCONT (Linux ERESTARTSYS). */
    return action.handler > 1 ? (action.flags & 0x10000000) != 0 : number >= 19 && number <= 22;
}
int signals_wait_mask(uint64_t address, uint64_t size) {
    if (!address) return 0;
    if (size != 8) return -22;
    if (!accessible(address, 8, false)) return -14;
    if (active_signals->restore_mask) panic("nested temporary signal mask");
    active_signals->saved_mask = blocked; active_signals->restore_mask = true;
    blocked = uget64(address) & ~UNBLOCKABLE; return 0;
}
static struct registers* bad_frame(void) {
    vga_print("[64] Invalid signal frame; terminating user process\n"); user_termination_signal = 11; user_result = 139; return NULL;
}
static struct registers* deliver(struct registers* r, const struct info64* info) {
    unsigned number = info->number; struct action action = actions[number];
    if (action.handler == 1 || (!action.handler && default_ignore(number))) return r;
    if (!action.handler) { user_termination_signal = number; user_result = 128 + number; return NULL; }
    uint64_t top = r->rsp;
    bool use_alt = (action.flags & SA_ONSTACK) && !(alternate.flags & 2) && !on_altstack(top);
    if (use_alt) top = alternate.sp + alternate.size;
    uint64_t fp_bytes = fp_frame_bytes();
    if (top < 128 + sizeof(struct signal_frame) + fp_bytes + 160) return bad_frame();
    uint64_t address = ((top - 128 - sizeof(struct signal_frame) - fp_bytes - 64) & ~63ULL) - 8;
    uint64_t fp_address = (address + sizeof(struct signal_frame) + 63) & ~63ULL;
    if (!accessible(address, fp_address + fp_bytes - address, true)) return bad_frame();
    struct signal_frame frame = {0}; frame.restorer = action.restorer; frame.info = *info;
    visible_sender(&frame.info);
    /* UC_SIGCONTEXT_SS, plus UC_STRICT_RESTORE_SS when 64-bit code was interrupted (Linux frame_uc_flags). */
    frame.context.flags = r->cs == USER_CS ? 6 : 2; frame.context.stack = alternate;
    if (on_altstack(r->rsp)) frame.context.stack.flags |= 1;
    frame.context.mask = active_signals->restore_mask ? active_signals->saved_mask : blocked;
    active_signals->restore_mask = false;
    struct sigcontext64* s = &frame.context.registers;
    s->r8 = r->r8; s->r9 = r->r9; s->r10 = r->r10; s->r11 = r->r11;
    s->r12 = r->r12; s->r13 = r->r13; s->r14 = r->r14; s->r15 = r->r15;
    s->rdi = r->rdi; s->rsi = r->rsi; s->rbp = r->rbp; s->rbx = r->rbx;
    s->rdx = r->rdx; s->rax = r->rax; s->rcx = r->rcx; s->rsp = r->rsp; s->rip = r->rip;
    s->flags = r->flags; s->cs = (uint16_t)r->cs; s->ss = (uint16_t)r->ss; /* 0x23 when 32-bit code was interrupted */
    s->error = r->error; s->trap = r->vector == 128 || r->vector == IA32_SYSCALL_VECTOR ? 0 : r->vector; s->oldmask = frame.context.mask;
    s->cr2 = active_signals->fault_address;   /* CR2 itself may have changed: the task can sleep and move CPUs first */
    s->fpstate = fp_address;
    memset(fpu, 0, sizeof(fpu)); fpu_save(fpu);
    if (xsave_mask) {
        /* Linux fx_sw_bytes: magic1, extended size, xfeatures, xstate size; magic2 after the area. */
        *(uint32_t*)(fpu + 464) = FP_XSTATE_MAGIC1; *(uint32_t*)(fpu + 468) = (uint32_t)fp_bytes;
        *(uint64_t*)(fpu + 472) = xsave_mask; *(uint32_t*)(fpu + 480) = xsave_size;
        *(uint32_t*)(fpu + xsave_size) = FP_XSTATE_MAGIC2;
        frame.context.flags |= 1; /* UC_FP_XSTATE */
    }
    if (copy_to_user((void*)address, &frame, sizeof(frame)) || copy_to_user((void*)fp_address, fpu, fp_bytes)) return bad_frame();
    reset_user_fpu();
    if (use_alt && (alternate.flags & 0x80000000U)) alternate.flags = 2;
    blocked |= action.mask;
    if (!(action.flags & SA_NODEFER)) blocked |= 1ULL << (number - 1);
    blocked &= ~UNBLOCKABLE;
    if (action.flags & SA_RESETHAND) actions[number].handler = 0;
    r->rip = action.handler; r->rsp = address; r->rdi = number; r->cs = USER_CS; r->ss = USER_DS; /* handlers run in 64-bit mode */
    r->rsi = address + __builtin_offsetof(struct signal_frame, info);
    r->rdx = address + __builtin_offsetof(struct signal_frame, context);
    r->rax = 0; r->flags &= ~((1ULL << 8) | (1ULL << 10) | (1ULL << 18));
    return r;
}
struct registers* signal_dispatch(struct registers* r) {
    for (;;) {
        task_stop_point();
        struct queued_signal** chosen = next_signal(false);
        if (!chosen) {
            if (active_signals->restore_mask) { blocked = active_signals->saved_mask; active_signals->restore_mask = false; }
            return r;
        }
        struct queued_signal* entry = *chosen; struct info64 info = entry->info; void* timer = entry->timer;
        unsigned number = info.number;
        *chosen = entry->next; entry_free(entry);
        if (timer && !posix_timer_unqueued(timer, &info)) continue;   /* si_overrun; a deleted timer's signal is dropped */
        if (number != 9) {   /* a traced thread stops here; the tracer may cancel or replace the signal */
            int traced = task_ptrace_stop(r, (int)number);
            if (traced != (int)number) {
                if (!traced) continue;
                uint64_t sender = task_ptrace_sender();   /* Linux ptrace_signal: SI_USER from the tracer */
                number = (unsigned)traced; memset(&info, 0, sizeof(info)); info.number = (int32_t)number; info.data[0] = sender;
                if (blocked & (1ULL << (number - 1))) {   /* blocked by the tracee: requeued, delivered once unblocked */
                    signals_enqueue(active_signals, number, 0, (uint32_t)sender, (uint32_t)(sender >> 32), 0, false); continue;
                }
            }
        }
        if (actions[number].handler == 1 || (!actions[number].handler && default_ignore(number))) continue;
        if (number >= 19 && number <= 22 && !actions[number].handler) { task_stop(number); continue; }
        return deliver(r, &info);
    }
}
struct registers* signal_fault(struct registers* r, unsigned number, int code, uint64_t address) {
    if ((blocked & (1ULL << (number - 1))) || actions[number].handler == 1) actions[number].handler = 0;
    blocked &= ~(1ULL << (number - 1));
    struct info64 info = {0}; info.number = number; info.code = code; info.data[0] = address;
    if (r->vector == 14) active_signals->fault_address = address;   /* CR2, read at fault time */
    if (number != 9) {   /* Linux ptrace_signal: a traced thread stops on fault signals too (PEEKUSER: the faulting state) */
        int traced = task_ptrace_stop(r, (int)number);
        if (traced != (int)number) {
            if (!traced) return signal_dispatch(r);   /* cancelled: #PF/#GP re-run the instruction, int3 continues after it */
            uint64_t sender = task_ptrace_sender();   /* a replacement comes from the tracer (SI_USER), as in signal_dispatch */
            number = (unsigned)traced; memset(&info, 0, sizeof(info)); info.number = (int32_t)number; info.data[0] = sender;
            if (blocked & (1ULL << (number - 1))) {   /* blocked by the tracee: requeued, delivered once unblocked */
                signals_enqueue(active_signals, number, 0, (uint32_t)sender, (uint32_t)(sender >> 32), 0, false); return signal_dispatch(r);
            }
            if (actions[number].handler == 1 || (!actions[number].handler && default_ignore(number))) return signal_dispatch(r);
            if (number >= 19 && number <= 22 && !actions[number].handler) { task_stop(number); return signal_dispatch(r); }
        }
    }
    return deliver(r, &info);
}
/* SECCOMP_RET_TRAP: a synchronous SIGSYS whose siginfo carries si_call_addr/si_syscall/si_arch/si_errno exactly as
 * Linux seccomp_send_sigsys fills it (si_code = SYS_SECCOMP = 1). Like a synchronous fault signal it cannot be left
 * ignored or blocked (forced to the default so the thread cannot swallow a seccomp violation). */
struct registers* signal_seccomp_sigsys(struct registers* r, int nr, uint32_t arch, uint64_t call_addr, int errno_data) {
    unsigned number = 31;   /* SIGSYS */
    if ((blocked & (1ULL << (number - 1))) || actions[number].handler == 1) actions[number].handler = 0;
    blocked &= ~(1ULL << (number - 1));
    struct info64 info = {0};
    info.number = (int32_t)number; info.code = 1; info.error = errno_data;
    info.data[0] = call_addr;                                     /* si_call_addr */
    info.data[1] = (uint32_t)nr | ((uint64_t)arch << 32);         /* si_syscall (low 32), si_arch (high 32) */
    return deliver(r, &info);
}
struct registers* signal_return(struct registers* r) {
    if (r->rsp < 8 || !accessible(r->rsp - 8, sizeof(struct signal_frame), false)) return bad_frame();
    struct signal_frame frame; if (copy_from_user(&frame, (void*)(r->rsp - 8), sizeof(frame))) return bad_frame();
    struct sigcontext64* s = &frame.context.registers;
    /* Linux forces RPL 3 and ignores the fs/gs words. CS: 0x33, 0x23 or a ring-3 LDT code segment; SS: a
     * writable ring-3 data segment, replaced by 0x2b for 64-bit code without UC_STRICT_RESTORE_SS. Kernel
     * selectors are refused (an iretq fault would otherwise follow). 32-bit code resumes below 4 GiB; its RSP is
     * restored whole, as deliver() saved it (compatibility mode uses ESP; RSP[63:32] may still hold a 64-bit stack). */
    uint16_t cs = s->cs | 3, ss = s->ss | 3;
    if (!segments_user_code(cs)) return bad_frame();
    if (!segments_user_stack(ss)) { if (cs == USER_CS && !(frame.context.flags & 4)) ss = USER_DS; else return bad_frame(); }
    if (cs == USER_CS ? !user_range(s->rip, 1) || !user_range(s->rsp, 1) : s->rip > 0xFFFFFFFFULL) return bad_frame();
    frame.context.stack.flags &= ~1U;
    if (!valid_altstack(&frame.context.stack)) return bad_frame();
    if (s->fpstate) {
        if (!accessible(s->fpstate, 512, false)) return bad_frame();
        memset(fpu, 0, sizeof(fpu)); if (copy_from_user(fpu, (void*)s->fpstate, 512)) return bad_frame();
        if (*(uint32_t*)(fpu + 24) & ~mxcsr_mask) return bad_frame();
        bool extended = xsave_mask && *(uint32_t*)(fpu + 464) == FP_XSTATE_MAGIC1 &&
                        *(uint32_t*)(fpu + 480) == xsave_size && *(uint32_t*)(fpu + 468) == xsave_size + 4;
        if (extended) {
            if (!accessible(s->fpstate, xsave_size + 4, false)) return bad_frame();
            if (copy_from_user(fpu, (void*)s->fpstate, xsave_size + 4)) return bad_frame();
            if (*(uint32_t*)(fpu + xsave_size) != FP_XSTATE_MAGIC2) extended = false;
        }
        if (xsave_mask) {
            /* XRSTOR #GPs on bad headers: accept only standard-form state for enabled features. */
            if (!extended) { memset(fpu + 512, 0, sizeof(fpu) - 512); *(uint64_t*)(fpu + 512) = 3; }
            if (*(uint64_t*)(fpu + 512) & ~xsave_mask) return bad_frame();
            for (unsigned i = 520; i < 576; i++) if (fpu[i]) return bad_frame();
        }
    }
    r->r8 = s->r8; r->r9 = s->r9; r->r10 = s->r10; r->r11 = s->r11;
    r->r12 = s->r12; r->r13 = s->r13; r->r14 = s->r14; r->r15 = s->r15;
    r->rdi = s->rdi; r->rsi = s->rsi; r->rbp = s->rbp; r->rbx = s->rbx;
    r->rdx = s->rdx; r->rax = s->rax; r->rcx = s->rcx; r->rsp = s->rsp; r->rip = s->rip;
    r->flags = (s->flags & 0x240DD5ULL) | 0x202; r->cs = cs; r->ss = ss;
    blocked = frame.context.mask & ~UNBLOCKABLE; alternate = frame.context.stack;
    if (s->fpstate) fpu_restore(fpu);
    else reset_user_fpu();
    return signal_dispatch(r);
}
void signals_selftest(void) {
    uint64_t before = memory_used_pages(); struct signal_state* saved = active_signals;
    struct signal_state* parent = signals_create(NULL, false); if (!parent) panic("signal context allocation");
    parent->handlers->entries[10].handler = 1; parent->mask = 1ULL << 11;
    struct signal_state* child = signals_create(parent, false);
    struct signal_state* shared = signals_create(parent, true);
    if (!child || !shared || child->mask != parent->mask || child->handlers == parent->handlers || shared->handlers != parent->handlers)
        panic("signal context clone semantics");
    child->handlers->entries[10].handler = 0;
    if (parent->handlers->entries[10].handler != 1) panic("child changed parent signal handler");
    signals_activate(child);
    if (signal_send(1, 12, 0) || !child->queue || parent->queue) panic("pending signal isolation");
    if (!signals_share_pending(shared, parent) || signals_enqueue(parent, 12, 0, 1, 1000, 0, true) ||
        !shared->group->queue || child->group->queue) panic("process signal group sharing");
    signals_activate(shared); shared->mask = 0;
    if (!signals_interrupt_pending()) panic("shared pending signal did not follow thread mask");
    signals_activate(saved); signals_destroy(child); signals_destroy(parent);
    if (shared->handlers->entries[10].handler != 1) panic("shared signal handler lifetime");
    signals_destroy(shared);
    if (memory_used_pages() != before) panic("signal context allocation leak");
    vga_print("[64] PASS signal contexts isolate masks/pending state and retain handler ownership\n");
}
