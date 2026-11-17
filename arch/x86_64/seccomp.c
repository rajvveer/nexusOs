/* Linux seccomp (SECure COMPuting): strict mode, classic-BPF filter mode, USER_NOTIF, with the
 * exact uapi behaviour Chrome/Firefox/systemd/libseccomp rely on.
 *
 * The syscall-entry hook (seccomp_enforce, called once at the top of native_dispatch for every ring-3
 * syscall on both the 64-bit and the int-0x80 path) is a single predicted branch on current->seccomp_mode
 * for unfiltered tasks, so new syscalls other agents add are filtered automatically with no cost to the
 * common case. The per-task state (mode + filter chain head) lives in struct task (process.c); this file
 * reaches it only through the small task_seccomp_* accessors, and owns everything else. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

/* ---- uapi constants (linux/seccomp.h, linux/filter.h, linux/audit.h) ---- */
#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2

#define SECCOMP_SET_MODE_STRICT        0
#define SECCOMP_SET_MODE_FILTER        1
#define SECCOMP_GET_ACTION_AVAIL       2
#define SECCOMP_GET_NOTIF_SIZES        3

#define SECCOMP_FILTER_FLAG_TSYNC            (1U << 0)
#define SECCOMP_FILTER_FLAG_LOG              (1U << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW       (1U << 2)
#define SECCOMP_FILTER_FLAG_NEW_LISTENER     (1U << 3)
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH      (1U << 4)
#define SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV (1U << 5)
#define SECCOMP_FILTER_FLAGS_ALL (SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG | \
        SECCOMP_FILTER_FLAG_SPEC_ALLOW | SECCOMP_FILTER_FLAG_NEW_LISTENER | \
        SECCOMP_FILTER_FLAG_TSYNC_ESRCH | SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV)

#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_KILL         SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_USER_NOTIF   0x7fc00000U
#define SECCOMP_RET_TRACE        0x7ff00000U
#define SECCOMP_RET_LOG          0x7ffc0000U
#define SECCOMP_RET_ALLOW        0x7fff0000U
#define SECCOMP_RET_ACTION_FULL  0xffff0000U
#define SECCOMP_RET_ACTION       0x7fff0000U
#define SECCOMP_RET_DATA         0x0000ffffU

#define AUDIT_ARCH_X86_64 0xC000003EU
#define AUDIT_ARCH_I386   0x40000003U

#define BPF_MAXINSNS 4096
#define BPF_MEMWORDS 16

/* instruction classes/sizes/modes/ops (linux/bpf_common.h) */
#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_SIZE(c) ((c) & 0x18)
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_MODE(c) ((c) & 0xe0)
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_MSH 0xa0
#define BPF_OP(c) ((c) & 0xf0)
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0
#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_SRC(c) ((c) & 0x08)
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_RVAL(c) ((c) & 0x18)
#define BPF_A 0x10
#define BPF_TAX 0x00
#define BPF_TXA 0x80

struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct sock_fprog { uint16_t len; uint16_t pad[3]; uint64_t filter; };   /* 32-bit len, aligned pointer */
struct seccomp_data { int32_t nr; uint32_t arch; uint64_t instruction_pointer; uint64_t args[6]; };
_Static_assert(sizeof(struct seccomp_data) == 64, "seccomp_data layout");
_Static_assert(sizeof(struct sock_filter) == 8, "sock_filter layout");

/* ---- notification (USER_NOTIF) ---- */
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE 1U
#define SECCOMP_ADDFD_FLAG_SETFD 1U
#define SECCOMP_ADDFD_FLAG_SEND  2U
#define SECCOMP_IOCTL_NOTIF_RECV      0xc0502100ULL  /* _IOWR('!',0, struct seccomp_notif) */
#define SECCOMP_IOCTL_NOTIF_SEND      0xc0182101ULL  /* _IOWR('!',1, struct seccomp_notif_resp) */
#define SECCOMP_IOCTL_NOTIF_ID_VALID  0x40082102ULL  /* _IOW('!',2, __u64) */
#define SECCOMP_IOCTL_NOTIF_ADDFD     0x40182103ULL  /* _IOW('!',3, struct seccomp_notif_addfd) = 24 bytes */
#define SECCOMP_IOCTL_NOTIF_ADDFD_BIG 0xc0182103ULL  /* _IOWR variant some libs use */
#define SECCOMP_IOCTL_NOTIF_SET_FLAGS 0x40082104ULL
#define SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP 1ULL
#define MAX_INSNS_PER_PATH ((1U << 18) / sizeof(struct sock_filter))   /* Linux: 32768, each older filter costs 4 more */

struct seccomp_notif { uint64_t id; uint32_t pid; uint32_t flags; struct seccomp_data data; };
struct seccomp_notif_resp { uint64_t id; int64_t val; int32_t error; uint32_t flags; };
struct seccomp_notif_sizes { uint16_t seccomp_notif; uint16_t seccomp_notif_resp; uint16_t seccomp_data; };
struct seccomp_notif_addfd { uint64_t id; uint32_t flags; uint32_t srcfd; uint32_t newfd; uint32_t newfd_flags; };
_Static_assert(sizeof(struct seccomp_notif) == 80 && sizeof(struct seccomp_notif_resp) == 24 &&
               sizeof(struct seccomp_notif_addfd) == 24, "seccomp notify uapi sizes (match the ioctl encodings)");

enum notif_state { NOTIF_INIT, NOTIF_SENT, NOTIF_REPLIED };
struct notification {
    struct notification* next;
    uint64_t id;
    uint32_t tid;              /* the blocked thread (internal pid) */
    struct seccomp_data data;
    enum notif_state state;
    int64_t reply_val; int32_t reply_error; uint32_t reply_flags;
    bool continue_syscall;     /* SECCOMP_USER_NOTIF_FLAG_CONTINUE */
    /* a single pending ADDFD request serviced before the reply wakes the tracee */
    bool addfd_pending; uint32_t addfd_srcfd, addfd_newfd, addfd_flags, addfd_newfd_flags; uint32_t addfd_tid; int addfd_result;
};

struct seccomp_filter {
    uint32_t references;
    struct seccomp_filter* prev;   /* older filter (evaluated after this one) */
    uint32_t len;
    uint32_t flags;                /* SPEC_ALLOW/LOG recorded at install */
    bool log;
    /* USER_NOTIF listener state (at most one listener per filter, like Linux) */
    bool has_listener;             /* a NEW_LISTENER fd was created for this filter */
    bool listener_closed;          /* the listener fd was closed: USER_NOTIF now -ENOSYS */
    uint64_t notif_fd_flags;       /* SECCOMP_IOCTL_NOTIF_SET_FLAGS (SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP) */
    uint64_t next_id;
    struct notification* notifications;
    struct sock_filter insns[];    /* len instructions */
};

/* ---- process.c boundary (per-task state and thread-group iteration) ---- */
uint8_t task_seccomp_mode(void);
struct seccomp_filter* task_seccomp_head(void);
void task_seccomp_set(uint8_t mode, struct seccomp_filter* head);
int task_seccomp_tsync(struct seccomp_filter* added);   /* 0, or positive tid of a thread that cannot sync */
bool credentials_no_new_privs(void);
void task_seccomp_kill(int signal, bool group);         /* fatal signal: whole group, or this thread only */
int task_wait_channel_killable(void* channel);          /* process.c: only a fatal signal ends the sleep */
bool signals_should_restart(void);
struct registers* signal_seccomp_sigsys(struct registers* r, int nr, uint32_t arch, uint64_t call_addr, int errno_data);
uint32_t task_internal_pid(uint32_t visible);
uint32_t task_visible_pid(uint32_t internal);
uint32_t task_tid(void);
bool task_alive(uint32_t tid);                          /* process.c: tid names a live (non-zombie) task */
int fd_install_into(uint32_t tid, uint32_t srcfd, uint32_t newfd, bool setfd, bool close_exec);

/* ---- refcounting ---- */
void seccomp_filter_retain(struct seccomp_filter* f) {
    if (!f) return;
    if (f->references == 0xFFFFFFFFU) panic("seccomp filter refcount overflow");
    f->references++;
}
static uint64_t filter_bytes(uint32_t len) { return sizeof(struct seccomp_filter) + (uint64_t)len * sizeof(struct sock_filter); }
void seccomp_filter_release(struct seccomp_filter* f) {
    while (f) {
        if (!f->references) panic("unowned seccomp filter");
        if (--f->references) {
            /* Linux __seccomp_filter_orphan: no task uses it any more (directly or through a newer filter), only the
             * listener fd holds it: the supervisor's poll sees EPOLLHUP. */
            if (f->references == 1 && f->has_listener && !f->listener_closed) { task_wake_channel(f); event_source_changed(f, 0x10); }
            return;
        }
        struct seccomp_filter* prev = f->prev;
        for (struct notification* n = f->notifications; n;) { struct notification* nx = n->next; page_free((uint64_t)n); n = nx; }
        arena_free((uint64_t)f, filter_bytes(f->len));
        f = prev;
    }
}
unsigned seccomp_filter_count(struct seccomp_filter* f) { unsigned n = 0; for (; f; f = f->prev) n++; return n; }
/* Link `f` onto `prev` as its older filter (used by task_seccomp_tsync in process.c, which cannot see the type). */
void seccomp_filter_chain(struct seccomp_filter* f, struct seccomp_filter* prev) { f->prev = prev; }

/* ---- classic-BPF validator (Linux bpf_check_classic + seccomp_check_filter) ---- */
/* Forward jumps only, in-bounds targets, aligned word loads inside seccomp_data, bounded scratch (M[0..15]),
 * no packet/ancillary/LEN/MSH loads, a division or modulo by a zero constant is rejected, at most 4096
 * instructions, and the program must end in a return. Returns 0, or a negative errno. */
static int validate(const struct sock_filter* f, uint32_t len) {
    if (len == 0 || len > BPF_MAXINSNS) return -22;
    for (uint32_t pc = 0; pc < len; pc++) {
        const struct sock_filter* ins = &f[pc];
        uint16_t code = ins->code;
        switch (BPF_CLASS(code)) {
        case BPF_LD:
        case BPF_LDX: {
            unsigned mode = BPF_MODE(code), size = BPF_SIZE(code);
            if (BPF_CLASS(code) == BPF_LDX && mode != BPF_IMM && mode != BPF_MEM && !(mode == BPF_MSH && size == BPF_B) && mode != BPF_LEN)
                return -22;
            if (mode == BPF_LEN) { if (size != BPF_W) return -22; break; }  /* A/X = sizeof(seccomp_data) */
            if (mode == BPF_IMM) { if (size != BPF_W) return -22; break; }
            if (mode == BPF_MEM) { if (size != BPF_W || ins->k >= BPF_MEMWORDS) return -22; break; }
            if (mode == BPF_MSH) { if (BPF_CLASS(code) != BPF_LDX || size != BPF_B) return -22; break; }
            if (mode == BPF_ABS) {
                /* word/half/byte load from seccomp_data; words must be aligned (brief: aligned loads) */
                if (BPF_CLASS(code) != BPF_LD) return -22;
                uint32_t want = size == BPF_W ? 4 : size == BPF_H ? 2 : 1;
                if (ins->k > sizeof(struct seccomp_data) || want > sizeof(struct seccomp_data) - ins->k) return -22;
                if (size == BPF_W && (ins->k & 3)) return -22;
                break;
            }
            if (mode == BPF_IND) { if (BPF_CLASS(code) != BPF_LD) return -22; break; }  /* X-relative, bounds at run time */
            return -22;
        }
        case BPF_ST:
        case BPF_STX:
            if (code != (BPF_ST | BPF_MEM) && code != (BPF_STX | BPF_MEM)) return -22;
            if (ins->k >= BPF_MEMWORDS) return -22;
            break;
        case BPF_ALU: {
            unsigned op = BPF_OP(code);
            if (op == BPF_NEG) { if (code != (BPF_ALU | BPF_NEG)) return -22; break; }
            if (op > BPF_XOR) return -22;
            if (BPF_SRC(code) != BPF_K && BPF_SRC(code) != BPF_X) return -22;
            if ((op == BPF_DIV || op == BPF_MOD) && BPF_SRC(code) == BPF_K && ins->k == 0) return -22;
            break;
        }
        case BPF_JMP: {
            unsigned op = BPF_OP(code);
            if (op == BPF_JA) {
                if (code != (BPF_JMP | BPF_JA)) return -22;
                if (ins->k >= len - pc - 1) return -22;    /* forward, in-bounds */
                break;
            }
            if (op != BPF_JEQ && op != BPF_JGT && op != BPF_JGE && op != BPF_JSET) return -22;
            if (BPF_SRC(code) != BPF_K && BPF_SRC(code) != BPF_X) return -22;
            if (ins->jt >= len - pc - 1 || ins->jf >= len - pc - 1) return -22;  /* forward, in-bounds */
            break;
        }
        case BPF_RET:
            if (BPF_RVAL(code) != BPF_K && BPF_RVAL(code) != BPF_A) return -22;
            break;
        case BPF_MISC:
            if (code != (BPF_MISC | BPF_TAX) && code != (BPF_MISC | BPF_TXA)) return -22;
            break;
        default: return -22;
        }
    }
    /* mandatory return: execution must not fall off the end */
    if (BPF_CLASS(f[len - 1].code) != BPF_RET) return -22;
    return 0;
}

/* ---- classic-BPF interpreter over seccomp_data ---- */
/* seccomp_data is read in NATIVE byte order (unlike packet BPF's big-endian loads): a filter comparing the nr
 * field against a plain syscall number is what every seccomp user writes. */
static uint32_t load_bytes(const struct seccomp_data* d, uint32_t offset, unsigned size, bool* fault) {
    if (offset > sizeof(*d) || size > sizeof(*d) - offset) { *fault = true; return 0; }
    const uint8_t* base = (const uint8_t*)d + offset;
    if (size == 4) { uint32_t v; memcpy(&v, base, 4); return v; }
    if (size == 2) { uint16_t v; memcpy(&v, base, 2); return v; }
    return base[0];
}
static uint32_t run_filter(const struct sock_filter* f, uint32_t len, const struct seccomp_data* d) {
    uint32_t A = 0, X = 0, mem[BPF_MEMWORDS]; memset(mem, 0, sizeof(mem));
    for (uint32_t pc = 0; pc < len; pc++) {
        const struct sock_filter* ins = &f[pc];
        uint16_t code = ins->code; uint32_t k = ins->k; bool fault = false;
        switch (code) {
        case BPF_LD | BPF_W | BPF_ABS: A = load_bytes(d, k, 4, &fault); if (fault) return 0; break;
        case BPF_LD | BPF_H | BPF_ABS: A = load_bytes(d, k, 2, &fault); if (fault) return 0; break;
        case BPF_LD | BPF_B | BPF_ABS: A = load_bytes(d, k, 1, &fault); if (fault) return 0; break;
        case BPF_LD | BPF_W | BPF_IND: A = load_bytes(d, X + k, 4, &fault); if (fault) return 0; break;
        case BPF_LD | BPF_H | BPF_IND: A = load_bytes(d, X + k, 2, &fault); if (fault) return 0; break;
        case BPF_LD | BPF_B | BPF_IND: A = load_bytes(d, X + k, 1, &fault); if (fault) return 0; break;
        case BPF_LD | BPF_W | BPF_LEN: A = sizeof(*d); break;
        case BPF_LDX | BPF_W | BPF_LEN: X = sizeof(*d); break;
        case BPF_LD | BPF_IMM: A = k; break;
        case BPF_LDX | BPF_IMM: X = k; break;
        case BPF_LD | BPF_MEM: A = mem[k & (BPF_MEMWORDS - 1)]; break;
        case BPF_LDX | BPF_MEM: X = mem[k & (BPF_MEMWORDS - 1)]; break;
        case BPF_LDX | BPF_B | BPF_MSH: { uint32_t v = load_bytes(d, k, 1, &fault); if (fault) return 0; X = (v & 0xf) << 2; break; }
        case BPF_ST | BPF_MEM: mem[k & (BPF_MEMWORDS - 1)] = A; break;
        case BPF_STX | BPF_MEM: mem[k & (BPF_MEMWORDS - 1)] = X; break;
        case BPF_ALU | BPF_ADD | BPF_K: A += k; break;
        case BPF_ALU | BPF_SUB | BPF_K: A -= k; break;
        case BPF_ALU | BPF_MUL | BPF_K: A *= k; break;
        case BPF_ALU | BPF_DIV | BPF_K: A /= k; break;
        case BPF_ALU | BPF_MOD | BPF_K: A %= k; break;
        case BPF_ALU | BPF_OR | BPF_K: A |= k; break;
        case BPF_ALU | BPF_AND | BPF_K: A &= k; break;
        case BPF_ALU | BPF_LSH | BPF_K: A = k < 32 ? A << k : 0; break;
        case BPF_ALU | BPF_RSH | BPF_K: A = k < 32 ? A >> k : 0; break;
        case BPF_ALU | BPF_XOR | BPF_K: A ^= k; break;
        case BPF_ALU | BPF_ADD | BPF_X: A += X; break;
        case BPF_ALU | BPF_SUB | BPF_X: A -= X; break;
        case BPF_ALU | BPF_MUL | BPF_X: A *= X; break;
        case BPF_ALU | BPF_DIV | BPF_X: if (!X) return 0; A /= X; break;
        case BPF_ALU | BPF_MOD | BPF_X: if (!X) return 0; A %= X; break;
        case BPF_ALU | BPF_OR | BPF_X: A |= X; break;
        case BPF_ALU | BPF_AND | BPF_X: A &= X; break;
        case BPF_ALU | BPF_LSH | BPF_X: A = X < 32 ? A << X : 0; break;
        case BPF_ALU | BPF_RSH | BPF_X: A = X < 32 ? A >> X : 0; break;
        case BPF_ALU | BPF_XOR | BPF_X: A ^= X; break;
        case BPF_ALU | BPF_NEG: A = (uint32_t)(-(int32_t)A); break;
        case BPF_JMP | BPF_JA: pc += k; break;
        case BPF_JMP | BPF_JEQ | BPF_K: pc += (A == k) ? ins->jt : ins->jf; break;
        case BPF_JMP | BPF_JGT | BPF_K: pc += (A > k) ? ins->jt : ins->jf; break;
        case BPF_JMP | BPF_JGE | BPF_K: pc += (A >= k) ? ins->jt : ins->jf; break;
        case BPF_JMP | BPF_JSET | BPF_K: pc += (A & k) ? ins->jt : ins->jf; break;
        case BPF_JMP | BPF_JEQ | BPF_X: pc += (A == X) ? ins->jt : ins->jf; break;
        case BPF_JMP | BPF_JGT | BPF_X: pc += (A > X) ? ins->jt : ins->jf; break;
        case BPF_JMP | BPF_JGE | BPF_X: pc += (A >= X) ? ins->jt : ins->jf; break;
        case BPF_JMP | BPF_JSET | BPF_X: pc += (A & X) ? ins->jt : ins->jf; break;
        case BPF_RET | BPF_K: return k;
        case BPF_RET | BPF_A: return A;
        case BPF_MISC | BPF_TAX: X = A; break;
        case BPF_MISC | BPF_TXA: A = X; break;
        default: return 0;   /* validated out, but cBPF semantics: unknown => return 0 (KILL) */
        }
    }
    return 0;   /* unreachable after validation (mandatory return) */
}

/* Precedence over stacked filters (seccomp(2) man page, highest severity wins): KILL_PROCESS, KILL_THREAD,
 * TRAP, ERRNO, USER_NOTIF, TRACE, LOG, ALLOW. Returns the full 32-bit result of the winning filter so its
 * SECCOMP_RET_DATA is preserved, and records that filter in *match (for USER_NOTIF). */
static unsigned severity(uint32_t ret) {
    switch (ret & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_KILL_PROCESS: return 7;
    case SECCOMP_RET_KILL_THREAD: return 6;
    case SECCOMP_RET_TRAP: return 5;
    case SECCOMP_RET_ERRNO: return 4;
    case SECCOMP_RET_USER_NOTIF: return 3;
    case SECCOMP_RET_TRACE: return 2;
    case SECCOMP_RET_LOG: return 1;
    default: return 0;   /* ALLOW and any non-conforming action (treated as ALLOW+log by Linux) */
    }
}
static uint32_t run_filters(struct seccomp_filter* head, const struct seccomp_data* d, struct seccomp_filter** match) {
    uint32_t best = SECCOMP_RET_ALLOW; unsigned best_sev = 0; *match = head;
    for (struct seccomp_filter* f = head; f; f = f->prev) {   /* newest first, to the oldest (Linux order) */
        uint32_t ret = run_filter(f->insns, f->len, d);
        unsigned sev = severity(ret);
        if (sev > best_sev) { best_sev = sev; best = ret; *match = f; }
    }
    return best;
}

/* ---- action availability ---- */
static bool action_available(uint32_t action) {
    switch (action) {
    case SECCOMP_RET_KILL_PROCESS: case SECCOMP_RET_KILL_THREAD: case SECCOMP_RET_TRAP:
    case SECCOMP_RET_ERRNO: case SECCOMP_RET_USER_NOTIF: case SECCOMP_RET_TRACE:
    case SECCOMP_RET_LOG: case SECCOMP_RET_ALLOW: return true;
    default: return false;
    }
}

/* ---- notification listener fd (USER_NOTIF): an FD_OBJECT backed by the filter ---- */
static const struct object_operations notif_operations;
static struct notification* notif_find(struct seccomp_filter* f, uint64_t id) {
    for (struct notification* n = f->notifications; n; n = n->next) if (n->id == id) return n;
    return NULL;
}
static int64_t notif_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    (void)object; (void)vectors; (void)count; (void)total; (void)write; (void)flags; return -22;   /* not read()/write()able */
}
static uint32_t notif_ready(void* object) {
    struct seccomp_filter* f = object; uint32_t ready = 0;
    for (struct notification* n = f->notifications; n; n = n->next) {
        if (n->state == NOTIF_INIT) ready |= 0x41;        /* EPOLLIN | EPOLLRDNORM */
        if (n->state == NOTIF_SENT) ready |= 0x104;       /* EPOLLOUT | EPOLLWRNORM */
    }
    if (f->references <= 1) ready |= 0x10;               /* EPOLLHUP: only the listener holds the filter */
    return ready;
}
static void* notif_source(void* object) { return object; }
static void notif_release(void* object) {
    struct seccomp_filter* f = object;
    f->listener_closed = true;   /* pending/future USER_NOTIF now fails with ENOSYS */
    /* wake any blocked tracees so they return -ENOSYS */
    for (struct notification* n = f->notifications; n; n = n->next) task_wake_channel(n);
    seccomp_filter_release(f);
}
static void notif_stat(void* object, struct linux_stat* stat) {
    (void)object; memset(stat, 0, sizeof(*stat)); stat->mode = 0600; stat->dev = 9; stat->block_size = 4096;
}
static int notif_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags);
static const struct object_operations notif_operations = { notif_io, notif_ioctl, notif_ready, notif_source, notif_release, notif_stat };

static int notif_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags;
    struct seccomp_filter* f = object;
    switch (command) {
    case SECCOMP_IOCTL_NOTIF_RECV: {
        if (!accessible(argument, sizeof(struct seccomp_notif), true)) return -14;
        struct seccomp_notif out;
        { uint8_t check[sizeof(out)]; copy_from_user(check, (const void*)argument, sizeof(check));   /* check_zeroed_user */
          for (unsigned i = 0; i < sizeof(check); i++) if (check[i]) return -22; }
        for (;;) {
            struct notification* pick = NULL;
            for (struct notification* n = f->notifications; n; n = n->next)
                if (n->state == NOTIF_INIT) { pick = n; break; }
            if (pick) {
                pick->state = NOTIF_SENT;
                memset(&out, 0, sizeof(out));
                out.id = pick->id; out.pid = task_visible_pid(pick->tid); out.flags = 0; out.data = pick->data;
                if (!accessible(argument, sizeof(out), true)) { pick->state = NOTIF_INIT; return -14; }   /* after any sleep */
                if (copy_to_user((void*)argument, &out, sizeof(out))) { pick->state = NOTIF_INIT; return -14; }   /* resent later */
                event_source_changed(f, 0x104);
                return 0;
            }
            /* block until a syscall traps into this listener (or a signal arrives) */
            int w = task_wait_channel(f);
            if (w) return w;   /* -EINTR (RECV is interruptible; WAIT_KILLABLE_RECV only matters for already-SENT) */
        }
    }
    case SECCOMP_IOCTL_NOTIF_SEND: {
        if (!accessible(argument, sizeof(struct seccomp_notif_resp), false)) return -14;
        struct seccomp_notif_resp resp; copy_from_user(&resp, (void*)argument, sizeof(resp));
        if (resp.flags & ~SECCOMP_USER_NOTIF_FLAG_CONTINUE) return -22;
        struct notification* n = notif_find(f, resp.id);
        if (!n) return -2;                 /* ENOENT: unknown id */
        if (n->state == NOTIF_REPLIED) return -2;
        if (n->state != NOTIF_SENT) return -2;
        if ((resp.flags & SECCOMP_USER_NOTIF_FLAG_CONTINUE) && (resp.error || resp.val)) return -22;
        n->reply_val = resp.val; n->reply_error = resp.error; n->reply_flags = resp.flags;
        n->continue_syscall = (resp.flags & SECCOMP_USER_NOTIF_FLAG_CONTINUE) != 0;
        n->state = NOTIF_REPLIED;
        task_wake_channel(n); event_source_changed(f, 0);
        return 0;
    }
    case SECCOMP_IOCTL_NOTIF_ID_VALID: {
        if (!accessible(argument, 8, false)) return -14;
        uint64_t id = uget64(argument);
        struct notification* n = notif_find(f, id);
        return (n && n->state == NOTIF_SENT && task_alive(n->tid)) ? 0 : -2;
    }
    case SECCOMP_IOCTL_NOTIF_ADDFD:
    case SECCOMP_IOCTL_NOTIF_ADDFD_BIG: {
        if (!accessible(argument, sizeof(struct seccomp_notif_addfd), false)) return -14;
        struct seccomp_notif_addfd req; copy_from_user(&req, (void*)argument, sizeof(req));
        if (req.flags & ~(SECCOMP_ADDFD_FLAG_SETFD | SECCOMP_ADDFD_FLAG_SEND)) return -22;
        if (req.newfd_flags & ~0x80000U) return -22;   /* only O_CLOEXEC */
        if (!(req.flags & SECCOMP_ADDFD_FLAG_SETFD) && req.newfd) return -22;
        struct notification* n = notif_find(f, req.id);
        if (!n || n->state != NOTIF_SENT || !task_alive(n->tid)) return -2;
        int installed = fd_install_into(n->tid, req.srcfd, req.newfd,
                                        (req.flags & SECCOMP_ADDFD_FLAG_SETFD) != 0, (req.newfd_flags & 0x80000U) != 0);
        if (installed < 0) return installed;
        if (req.flags & SECCOMP_ADDFD_FLAG_SEND) {   /* atomically install + complete the syscall with that fd */
            n->reply_val = installed; n->reply_error = 0; n->reply_flags = 0; n->continue_syscall = false;
            n->state = NOTIF_REPLIED; task_wake_channel(n);
        }
        return installed;
    }
    case SECCOMP_IOCTL_NOTIF_SET_FLAGS:
        if (!accessible(argument, 8, false)) return -14;
        { uint64_t value = uget64(argument); if (value & ~SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP) return -22; f->notif_fd_flags = value; return 0; }   /* no notif-fd flags defined yet */
    default: return -25;   /* ENOTTY */
    }
}

/* Add `added` to the chain head for the current task and switch to filter mode. Shares nothing. */
static int install_filter(struct seccomp_filter* added) {
    added->prev = task_seccomp_head();
    task_seccomp_set(SECCOMP_MODE_FILTER, added);
    return 0;
}

/* ---- entry hook ---- */
/* Fill seccomp_data from the trap frame for the given architecture. */
static void fill_data(struct seccomp_data* d, const struct registers* r, uint32_t arch) {
    d->nr = (int32_t)(uint32_t)r->rax;
    d->arch = arch;
    d->instruction_pointer = r->rip;
    if (arch == AUDIT_ARCH_I386) {   /* int 0x80: ebx,ecx,edx,esi,edi,ebp */
        d->args[0] = (uint32_t)r->rbx; d->args[1] = (uint32_t)r->rcx; d->args[2] = (uint32_t)r->rdx;
        d->args[3] = (uint32_t)r->rsi; d->args[4] = (uint32_t)r->rdi; d->args[5] = (uint32_t)r->rbp;
    } else {                         /* syscall: rdi,rsi,rdx,r10,r8,r9 */
        d->args[0] = r->rdi; d->args[1] = r->rsi; d->args[2] = r->rdx;
        d->args[3] = r->r10; d->args[4] = r->r8; d->args[5] = r->r9;
    }
}
static bool strict_allowed(int32_t nr, uint32_t arch) {
    if (arch != AUDIT_ARCH_X86_64) return false;   /* strict mode allows only the native arch */
    return nr == 0 || nr == 1 || nr == 15 || nr == 60;   /* read, write, rt_sigreturn, exit */
}
static unsigned notify_log_budget;
/* /proc/sys/kernel/seccomp/actions_logged (Linux seccomp_actions_logged): which actions are audited. RET_LOG is always
 * logged when "log" is enabled; the others only by filters installed with SECCOMP_FILTER_FLAG_LOG. ALLOW never. */
#define LOG_KILL_PROCESS 1u
#define LOG_KILL_THREAD 2u
#define LOG_TRAP 4u
#define LOG_ERRNO 8u
#define LOG_USER_NOTIF 16u
#define LOG_TRACE 32u
#define LOG_LOG 64u
static uint32_t actions_logged = LOG_KILL_PROCESS | LOG_KILL_THREAD | LOG_TRAP | LOG_ERRNO | LOG_USER_NOTIF | LOG_TRACE | LOG_LOG;
static const struct { const char* name; uint32_t action, bit; } action_names[] = {
    {"kill_process", SECCOMP_RET_KILL_PROCESS, LOG_KILL_PROCESS}, {"kill_thread", SECCOMP_RET_KILL_THREAD, LOG_KILL_THREAD},
    {"trap", SECCOMP_RET_TRAP, LOG_TRAP}, {"errno", SECCOMP_RET_ERRNO, LOG_ERRNO}, {"user_notif", SECCOMP_RET_USER_NOTIF, LOG_USER_NOTIF},
    {"trace", SECCOMP_RET_TRACE, LOG_TRACE}, {"log", SECCOMP_RET_LOG, LOG_LOG}, {"allow", SECCOMP_RET_ALLOW, 0}};
static uint64_t logged_events;
static void seccomp_log(const struct seccomp_filter* match, uint32_t action, const struct seccomp_data* d) {
    uint32_t bit = 0; const char* name = "kill_thread";
    for (unsigned i = 0; i < sizeof(action_names) / sizeof(action_names[0]); i++)
        if (action_names[i].action == action) { bit = action_names[i].bit; name = action_names[i].name; }
    if (!(actions_logged & bit)) return;
    if (action != SECCOMP_RET_LOG && (!match || !match->log)) return;
    logged_events++;
    if (notify_log_budget >= 64) return;   /* rate limit, like audit's printk_ratelimit */
    notify_log_budget++;
    char n[24];
    vga_print("[64] SECCOMP audit pid="); uint_to_str(task_pid(), n); vga_print(n);
    vga_print(" exe="); { const char* exe = task_executable(task_tid()); vga_print(exe ? exe : "?"); }
    vga_print(" arch="); vga_print(d->arch == AUDIT_ARCH_I386 ? "i386" : "x86_64");
    vga_print(" syscall="); uint_to_str((uint32_t)d->nr, n); vga_print(n); vga_print(" action="); vga_print(name); vga_print("\n");
}
/* procext.c: /proc/sys/kernel/seccomp/{actions_avail,actions_logged} */
unsigned seccomp_actions_text(bool logged, char* out, unsigned capacity) {
    unsigned used = 0; out[0] = 0;
    for (unsigned i = 0; i < sizeof(action_names) / sizeof(action_names[0]); i++) {
        if (logged && (!action_names[i].bit || !(actions_logged & action_names[i].bit))) continue;
        unsigned n = strlen(action_names[i].name);
        if (used + n + 2 >= capacity) break;
        if (used) out[used++] = ' ';
        memcpy(out + used, action_names[i].name, n); used += n; out[used] = 0;
    }
    if (used + 1 < capacity) { out[used++] = '\n'; out[used] = 0; }
    return used;
}
int seccomp_actions_logged_write(const char* text) {   /* space-separated names; "allow" is refused (Linux) */
    uint32_t value = 0; unsigned i = 0;
    while (text[i]) {
        while (text[i] == ' ' || text[i] == '\n' || text[i] == '\t') i++;
        if (!text[i]) break;
        unsigned start = i; while (text[i] && text[i] != ' ' && text[i] != '\n' && text[i] != '\t') i++;
        bool found = false;
        for (unsigned k = 0; k < sizeof(action_names) / sizeof(action_names[0]); k++)
            if (strlen(action_names[k].name) == i - start && !strncmp(action_names[k].name, text + start, i - start)) {
                if (!action_names[k].bit) return -22;   /* allow cannot be logged */
                value |= action_names[k].bit; found = true;
            }
        if (!found) return -22;
    }
    actions_logged = value; return 0;
}
/* Returns true when the syscall must NOT be dispatched; *out is the frame to return (NULL kills the thread).
 * Returns false to let native_dispatch run the syscall (SECCOMP_RET_ALLOW/LOG, or USER_NOTIF CONTINUE). */
bool seccomp_enforce(struct registers* r, struct registers** out) {
    uint32_t arch = r->vector == IA32_SYSCALL_VECTOR ? AUDIT_ARCH_I386 : AUDIT_ARCH_X86_64;
    uint64_t call = (uint32_t)r->rax;   /* original syscall number, for the normal exit path (restart/signals) */
    uint8_t mode = task_seccomp_mode();
    if (mode == SECCOMP_MODE_STRICT) {
        if (strict_allowed((int32_t)(uint32_t)r->rax, arch)) return false;
        task_seccomp_kill(9, true);   /* Linux strict: SIGKILL the task (whole thread group) */
        *out = NULL; return true;
    }
    if (mode != SECCOMP_MODE_FILTER) return false;
    struct seccomp_data data; fill_data(&data, r, arch);
    struct seccomp_filter* head = task_seccomp_head();
    struct seccomp_filter* match = NULL;
    uint32_t ret = run_filters(head, &data, &match);
    uint32_t action = ret & SECCOMP_RET_ACTION_FULL, info = ret & SECCOMP_RET_DATA;
    if (action != SECCOMP_RET_ALLOW) seccomp_log(match, action, &data);
    switch (action) {
    case SECCOMP_RET_ALLOW: return false;
    case SECCOMP_RET_LOG: return false;
    case SECCOMP_RET_ERRNO: {
        uint32_t e = info > 4095 ? 4095 : info;   /* clamped, like Linux */
        r->rax = (uint64_t)(int64_t)(-(int32_t)e); *out = native_syscall_finish(r, call); return true;
    }
    case SECCOMP_RET_TRACE:
        /* PTRACE_EVENT_SECCOMP needs a tracer that set PTRACE_O_TRACESECCOMP; the ptrace subset here has no
         * syscall/event stops, so this is the Linux no-tracer path exactly: the syscall fails with ENOSYS. */
        r->rax = (uint64_t)(int64_t)-38; *out = native_syscall_finish(r, call); return true;
    case SECCOMP_RET_USER_NOTIF: {
        if (!match || !match->has_listener || match->listener_closed) { r->rax = (uint64_t)(int64_t)-38; *out = native_syscall_finish(r, call); return true; }
        struct notification* n = (void*)page_alloc();
        if (!n) { r->rax = (uint64_t)(int64_t)-38; *out = native_syscall_finish(r, call); return true; }
        n->id = ++match->next_id; n->tid = task_tid(); n->data = data; n->state = NOTIF_INIT;
        n->next = match->notifications; match->notifications = n;
        task_wake_channel(match); event_source_changed(match, 0x41);   /* a RECV-blocked or polling supervisor */
        /* Linux seccomp_do_user_notification: an interruptible wait; once the supervisor has received it, a filter with
         * WAIT_KILLABLE_RECV waits killable (only a fatal signal interrupts). Interrupted: the notification is withdrawn
         * and the call returns -ERESTARTSYS (restarted after an SA_RESTART handler, else EINTR). */
        bool interrupted = false;
        while (n->state != NOTIF_REPLIED && !match->listener_closed) {
            bool killable = (match->flags & SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV) && n->state == NOTIF_SENT;
            int w = killable ? task_wait_channel_killable(n) : task_wait_channel(n);
            if (!w || n->state == NOTIF_REPLIED || match->listener_closed) continue;
            if (!killable && (match->flags & SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV) && n->state == NOTIF_SENT && !task_exiting()) continue;
            interrupted = true; break;
        }
        /* unlink */
        for (struct notification** link = &match->notifications; *link; link = &(*link)->next)
            if (*link == n) { *link = n->next; break; }
        bool cont = n->state == NOTIF_REPLIED && n->continue_syscall;
        int64_t val = n->reply_val; int32_t err = n->reply_error;
        bool replied = n->state == NOTIF_REPLIED;
        page_free((uint64_t)n); event_source_changed(match, 0);
        if (interrupted && !replied) {
            if (!task_exiting() && signals_should_restart()) { r->rax = call; r->rip -= 2; *out = signal_dispatch(r); return true; }   /* ERESTARTSYS */
            r->rax = (uint64_t)(int64_t)-4; *out = native_syscall_finish(r, call); return true;
        }
        if (!replied) { r->rax = (uint64_t)(int64_t)-38; *out = native_syscall_finish(r, call); return true; }   /* listener gone */
        if (cont) return false;   /* CONTINUE: run the real syscall */
        r->rax = err ? (uint64_t)(int64_t)(-(err > 4095 ? 4095 : err < 0 ? -err : err)) : (uint64_t)val;
        *out = native_syscall_finish(r, call); return true;
    }
    case SECCOMP_RET_TRAP:
        *out = signal_seccomp_sigsys(r, (int32_t)(uint32_t)r->rax, arch, r->rip, (int)info);
        return true;
    case SECCOMP_RET_KILL_PROCESS:
        task_seccomp_kill(31, true); *out = NULL; return true;
    case SECCOMP_RET_KILL_THREAD:
    default:
        task_seccomp_kill(31, false); *out = NULL; return true;
    }
}

/* ---- prctl(PR_SET_SECCOMP/PR_GET_SECCOMP) and seccomp(2) ---- */
/* Installing a filter requires NO_NEW_PRIVS or CAP_SYS_ADMIN in the caller's own user namespace (Linux
 * seccomp_prepare_filter: ns_capable_noaudit(current_user_ns(), CAP_SYS_ADMIN)); strict mode needs neither. */
static bool may_install(void) {
    struct credentials* self = credentials_current();
    return credentials_no_new_privs() || credentials_capable(self, credentials_namespace(self), 21 /*CAP_SYS_ADMIN*/);
}
int64_t seccomp_get_mode(void) { return task_seccomp_mode(); }

static int64_t do_set_strict(void) {
    uint8_t mode = task_seccomp_mode();
    if (mode == SECCOMP_MODE_STRICT) return 0;
    if (mode != SECCOMP_MODE_DISABLED) return -22;
    task_seccomp_set(SECCOMP_MODE_STRICT, NULL);
    return 0;
}

/* `sbxtrace` boot option (kernel.c): one serial line per sandbox setup step - seccomp filter installs here, namespace
 * syscalls and SBX_* environment at exec in kernel.c - so tests can see which processes are sandboxed. */
bool sandbox_trace;
static void trace_install(uint32_t flags, uint32_t length) {
    if (!sandbox_trace) return;
    char number[16]; const char* exe = task_executable(task_tid());
    vga_print("[64] SBXTRACE seccomp-filter pid="); uint_to_str(task_pid(), number); vga_print(number);
    vga_print(" insns="); uint_to_str(length, number); vga_print(number);
    vga_print(flags & SECCOMP_FILTER_FLAG_TSYNC ? " tsync=1" : " tsync=0");
    vga_print(" exe="); vga_print(exe ? exe : "?"); vga_print("\n");
}
/* Build a filter from a user struct sock_fprog, validate it, and install (optionally with TSYNC / a listener). */
static int64_t do_set_filter(uint64_t prog_addr, uint32_t flags, int* listener_fd) {
    *listener_fd = -1;
    if (flags & ~SECCOMP_FILTER_FLAGS_ALL) return -22;
    /* TSYNC reports a failing thread by its id, NEW_LISTENER returns an fd: together only with TSYNC_ESRCH (Linux). */
    if ((flags & SECCOMP_FILTER_FLAG_TSYNC) && (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER) && !(flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH)) return -22;
    if ((flags & SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV) && !(flags & SECCOMP_FILTER_FLAG_NEW_LISTENER)) return -22;
    /* Linux order (seccomp_prepare_user_filter / seccomp_prepare_filter): copy the sock_fprog (EFAULT), check its
     * length (EINVAL), THEN require NO_NEW_PRIVS or CAP_SYS_ADMIN (EACCES), then copy and validate the program; the
     * mode check comes after. Browsers detect seccomp support from an unprivileged process with a NULL program and
     * expect EFAULT (Firefox SandboxInfo::HasSeccompBPF/HasSeccompTSync, Chromium KernelSupportsSeccompBPF). */
    if (!accessible(prog_addr, sizeof(struct sock_fprog), false)) return -14;
    struct sock_fprog prog; copy_from_user(&prog, (void*)prog_addr, sizeof(prog));
    uint32_t len = prog.len;
    if (len == 0 || len > BPF_MAXINSNS) return -22;
    if (!may_install()) return -13;   /* EACCES: needs NO_NEW_PRIVS or CAP_SYS_ADMIN (Linux) */
    uint8_t mode = task_seccomp_mode();
    if (mode != SECCOMP_MODE_DISABLED && mode != SECCOMP_MODE_FILTER) return -22;
    uint64_t insn_bytes = (uint64_t)len * sizeof(struct sock_filter);
    if (!accessible(prog.filter, insn_bytes, false)) return -14;
    struct seccomp_filter* f = (void*)arena_alloc(filter_bytes(len));
    if (!f) return -12;
    memset(f, 0, sizeof(*f));
    if (copy_from_user(f->insns, (void*)prog.filter, insn_bytes)) { arena_free((uint64_t)f, filter_bytes(len)); return -14; }   /* copied before validation: no TOCTOU on user memory */
    f->len = len; f->references = 1; f->flags = flags;
    f->log = (flags & SECCOMP_FILTER_FLAG_LOG) != 0;
    int error = validate(f->insns, len);
    if (error) { arena_free((uint64_t)f, filter_bytes(len)); return error; }
    /* Linux seccomp_attach_filter: the instructions on any path, 4 extra per older filter, at most MAX_INSNS_PER_PATH. */
    uint64_t total = len;
    for (struct seccomp_filter* walker = task_seccomp_head(); walker; walker = walker->prev) total += walker->len + 4;
    if (total > MAX_INSNS_PER_PATH) { arena_free((uint64_t)f, filter_bytes(len)); return -12; }
    if (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER) {   /* has_duplicate_listener: one listener per filter chain */
        for (struct seccomp_filter* walker = task_seccomp_head(); walker; walker = walker->prev)
            if (walker->has_listener && !walker->listener_closed) { arena_free((uint64_t)f, filter_bytes(len)); return -16; }
        f->has_listener = true;
        seccomp_filter_retain(f);   /* the fd holds a reference */
        int fd = fd_create_object(&notif_operations, f, 0x80000 /*O_CLOEXEC*/, "anon_inode:[seccomp notify]");
        if (fd < 0) { seccomp_filter_release(f); seccomp_filter_release(f); return fd; }
        *listener_fd = fd;
    }
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        int tid = task_seccomp_tsync(f);   /* chains f and applies to every thread, or reports the offender */
        if (tid != 0) {
            if (*listener_fd >= 0) { fd_close((uint64_t)*listener_fd); *listener_fd = -1; }   /* drops the fd's reference */
            seccomp_filter_release(f);
            if (flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH) return -3;   /* ESRCH */
            return tid;                                                /* positive tid of the thread that could not sync */
        }
        trace_install(flags, f->len); return 0;   /* tsync installed on the calling thread too */
    }
    install_filter(f); trace_install(flags, f->len);
    return 0;
}

/* prctl(PR_SET_SECCOMP, mode, ...) : mode 1 = strict, 2 = filter (arg3 = struct sock_fprog*). */
int64_t seccomp_set_mode_prctl(uint64_t mode, uint64_t arg) {
    if (mode == SECCOMP_MODE_STRICT) return do_set_strict();
    if (mode == SECCOMP_MODE_FILTER) { int fd; int64_t r = do_set_filter(arg, 0, &fd); return r; }
    return -22;
}

/* seccomp(2) = syscall 317: (operation, flags, args). */
int64_t seccomp_syscall(struct registers* r) {
    uint64_t operation = r->rdi, flags = r->rsi, args = r->rdx;
    switch (operation) {
    case SECCOMP_SET_MODE_STRICT:
        if (flags || args) return -22;
        return do_set_strict();
    case SECCOMP_SET_MODE_FILTER: {
        int listener = -1;
        int64_t result = do_set_filter(args, (uint32_t)flags, &listener);
        if (result == 0 && (flags & SECCOMP_FILTER_FLAG_NEW_LISTENER)) return listener;
        return result;
    }
    case SECCOMP_GET_ACTION_AVAIL: {
        if (flags || !args) return -22;
        if (!accessible(args, 4, false)) return -14;
        uint32_t action = uget32(args);
        return action_available(action) ? 0 : -95;   /* EOPNOTSUPP */
    }
    case SECCOMP_GET_NOTIF_SIZES: {
        if (flags || !args) return -22;
        if (!accessible(args, sizeof(struct seccomp_notif_sizes), true)) return -14;
        struct seccomp_notif_sizes sizes = { sizeof(struct seccomp_notif), sizeof(struct seccomp_notif_resp), sizeof(struct seccomp_data) };
        return copy_to_user((void*)args, &sizes, sizeof(sizes)) ? -14 : 0;
    }
    default: return -22;
    }
}

/* ---- TSYNC support: does `thread_head` differ from `base` only by filters that are a tail of it? ----
 * For Linux, TSYNC requires every sibling's filter chain to be an ancestor of (or equal to) the installing
 * thread's chain after the new filter is added; in practice all siblings must share the same chain the
 * installer had before this call. We expose a check used by process.c. */
bool seccomp_filter_is_ancestor(struct seccomp_filter* maybe_ancestor, struct seccomp_filter* chain) {
    for (struct seccomp_filter* f = chain; f; f = f->prev) if (f == maybe_ancestor) return true;
    return maybe_ancestor == NULL;
}
