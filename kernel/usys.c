/* ============================================================================
 * NexusOS — POSIX-ish process plumbing for the userland — Phase 57 "apps"
 * ============================================================================
 * See usys.h for the overview and usys_abi.h for the ABI. Everything here is
 * static storage (the 4 MB heap is shared with the whole kernel): open-file
 * table, per-process fd tables, two terminals with VT100 screens, zombie
 * table, FPU save areas. Compiled at -O2 (build.bat hot list).
 *
 * Blocking model: a user process that must wait (tty input, pipe space/data,
 * child exit, sleep) is marked PROC_BLOCKED with a wait tag and yields; the
 * event source (the kernel thread pumping keys, a pipe peer, an exiting child,
 * the timer for timeouts) marks it READY again. The kernel thread itself never
 * blocks: it loops (console job) or runs desktop frames (GUI job).
 * ============================================================================ */

#include "usys.h"
#include "usys_abi.h"
#include "uproc.h"
#include "scheduler.h"
#include "paging.h"
#include "memory.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "vfs.h"
#include "ramfs.h"
#include "fat32.h"
#include "pipe.h"
#include "procfs.h"
#include "posix.h"
#include "keyboard.h"
#include "users.h"
#include "rtc.h"
#include "env.h"
#include "persist.h"
#include "version.h"

extern volatile uint32_t system_ticks;

/* ============================================================================
 * Tables
 * ============================================================================ */
#define NFD      24
#define NOF      64
#define NTTY     2
#define INQ      512
#define LINEMAX  256
#define PATHMAX  160
#define NZOMB    24

enum { OF_FREE = 0, OF_TTY, OF_RAMFS, OF_FAT, OF_PIPE_R, OF_PIPE_W, OF_DIR_RAMFS, OF_DIR_FAT, OF_NULL, OF_PROC };
enum { W_NONE = 0, W_TTY, W_PIPE, W_CHILD, W_SLEEP };
enum { K_NONE = 0, K_RAMFS, K_FAT, K_PROC, K_NULL, K_TTY };

typedef struct {
    uint8_t      type;
    uint8_t      refs;
    uint16_t     flags;          /* O_APPEND / O_NONBLOCK / accmode            */
    uint32_t     pos;
    fs_node_t*   node;           /* OF_RAMFS / OF_DIR_RAMFS                    */
    fat32_file_t ff;             /* OF_FAT / OF_DIR_FAT                        */
    int          pipe;           /* OF_PIPE_*                                  */
    int          tty;            /* OF_TTY                                     */
    char         path[PATHMAX];  /* OF_PROC (and diagnostics)                  */
} ofile_t;

typedef struct {
    bool     used;
    int8_t   fd[NFD];
    uint32_t cloexec;
    int      tty;
    char     cwd[PATHMAX];
    uint8_t  wait_kind;
    uint32_t wait_obj;
    uint32_t mmap_next;
} uext_t;

typedef struct { bool valid; uint32_t pid, ppid; int status; } zomb_t;

typedef struct {
    vt_t     vt;
    uint8_t  inq[INQ];
    int      in_head, in_tail;
    char     line[LINEMAX];
    int      linelen;
    struct nx_termios tio;
    bool     eof_pending;
    bool     active;             /* a job owns this terminal                   */
    bool     dirty;
    uint32_t fg_pid;             /* job the kernel thread launched here        */
    int      cols, rows;
    int      exit_code;          /* GUI: result of the finished job            */
    bool     exit_pending;
} tty_t;

static ofile_t  ofiles[NOF];
static uext_t   uext[MAX_PROCESSES];
static zomb_t   zomb[NZOMB];
static tty_t    ttys[NTTY];
static uint8_t  fpu_area[MAX_PROCESSES][512] __attribute__((aligned(16)));
static bool     fpu_valid[MAX_PROCESSES];
static bool     have_fxsr = false;
static int      killing_sig = 0;         /* set before uproc_kill() by usys_kill/Ctrl+C */
static uint32_t con_last_flush = 0;
static int      gui_cols = 90, gui_rows = 27;

/* spawn scratch (one spawn at a time: syscalls are never pre-empted) */
static char   sp_strs[4096];
static char*  sp_argv[32];
static char*  sp_envp[32];

/* ============================================================================
 * Small helpers
 * ============================================================================ */
static int slot_of(process_t* p) { return (int)(p - process_get_table()); }

static process_t* find_proc(uint32_t pid) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (t[i].pid == pid && t[i].state != PROC_UNUSED) return &t[i];
    return NULL;
}

static bool proc_alive(process_t* p) {
    return p && p->state != PROC_UNUSED && p->state != PROC_TERMINATED;
}

static bool uok(uint32_t a, uint32_t n) {
    process_t* p = process_get_current();
    if (!p || !p->is_user) return true;
    if (n == 0) return true;
    return as_check_user(p->cr3, a, n);
}

/* Copy a NUL-terminated user string (validated page by page). */
static int ustr(uint32_t ua, char* dst, int max) {
    process_t* p = process_get_current();
    for (int i = 0; i < max; i++) {
        uint32_t a = ua + (uint32_t)i;
        if (p && p->is_user && ((a & 0xFFF) == 0 || i == 0) && !as_check_user(p->cr3, a, 1)) return -NX_EFAULT;
        char c = *(const char*)a;
        dst[i] = c;
        if (!c) return i;
    }
    dst[max - 1] = 0;
    return -NX_ENAMETOOLONG;
}

static void ensure_ext(int s);

static uext_t* cur_ext(void) {
    process_t* p = process_get_current();
    if (!p || !p->is_user) return NULL;
    int s = slot_of(p);
    ensure_ext(s);
    return &uext[s];
}

static int ofile_alloc(void) {
    for (int i = 0; i < NOF; i++) if (ofiles[i].type == OF_FREE) { memset(&ofiles[i], 0, sizeof(ofile_t)); ofiles[i].refs = 1; return i; }
    return -1;
}

static int fd_alloc(uext_t* e, int from) {
    if (from < 0 || from >= NFD) return -1;
    for (int i = from; i < NFD; i++) if (e->fd[i] < 0) { e->cloexec &= ~(1u << i); return i; }
    return -1;
}

static void usys_wake(int kind, uint32_t obj);

static void ofile_release(int oi) {
    ofile_t* o = &ofiles[oi];
    if (o->type == OF_FREE) return;
    if (o->refs > 1) { o->refs--; return; }
    if (o->type == OF_PIPE_R) { pipe_close_read(o->pipe); usys_wake(W_PIPE, (uint32_t)o->pipe); }
    if (o->type == OF_PIPE_W) { pipe_close_write(o->pipe); usys_wake(W_PIPE, (uint32_t)o->pipe); }
    o->type = OF_FREE; o->refs = 0;
}

static ofile_t* fd_get(uext_t* e, int fd) {
    if (!e || fd < 0 || fd >= NFD || e->fd[fd] < 0) return NULL;
    return &ofiles[e->fd[fd]];
}

/* Give an unknown user process (legacy runelf) a usable table lazily. */
static void ensure_ext(int s) {
    if (uext[s].used) return;
    uext_t* e = &uext[s];
    memset(e, 0, sizeof(*e));
    e->used = true;
    for (int i = 0; i < NFD; i++) e->fd[i] = -1;
    e->tty = USYS_TTY_CONSOLE;
    strcpy(e->cwd, "/");
    e->mmap_next = NX_MMAP_BASE;
    int oi = ofile_alloc();
    if (oi >= 0) {
        ofiles[oi].type = OF_TTY; ofiles[oi].tty = USYS_TTY_CONSOLE; ofiles[oi].refs = 3;
        e->fd[0] = e->fd[1] = e->fd[2] = (int8_t)oi;
    }
}

/* ============================================================================
 * Paths
 * ============================================================================ */
/* Join cwd + rel into a normalized absolute path ("." / ".." resolved). */
static void norm_path(const char* cwd, const char* rel, char* out) {
    char tmp[PATHMAX * 2];
    int n = 0;
    if (rel[0] != '/') {
        for (int i = 0; cwd[i] && n < (int)sizeof(tmp) - 2; i++) tmp[n++] = cwd[i];
        tmp[n++] = '/';
    }
    for (int i = 0; rel[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = rel[i];
    tmp[n] = 0;

    int o = 0;
    out[o++] = '/';
    const char* p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        int cl = (int)(p - s);
        if (cl == 1 && s[0] == '.') continue;
        if (cl == 2 && s[0] == '.' && s[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; }
            continue;
        }
        if (o > 1) out[o++] = '/';
        for (int i = 0; i < cl && o < PATHMAX - 1; i++) out[o++] = s[i];
    }
    out[o] = 0;
}

static bool is_disk_path(const char* abs, const char** sub) {
    if (strncmp(abs, "/disk", 5) != 0) return false;
    if (abs[5] != 0 && abs[5] != '/') return false;
    const char* s = abs + 5;
    while (*s == '/') s++;
    if (sub) *sub = s;
    return true;
}

typedef struct { int kind; fs_node_t* node; fat32_file_t ff; } res_t;

static int resolve(const char* abs, res_t* r) {
    const char* sub;
    memset(r, 0, sizeof(*r));
    if (strcmp(abs, "/dev/null") == 0) { r->kind = K_NULL; return 0; }
    if (strcmp(abs, "/dev/tty") == 0 || strcmp(abs, "/dev/console") == 0) { r->kind = K_TTY; return 0; }
    if (strncmp(abs, "/proc/", 6) == 0 && abs[6]) { r->kind = K_PROC; return 0; }
    if (is_disk_path(abs, &sub)) {
        if (!fat32_is_mounted()) return -NX_ENOENT;
        if (!fat32_lookup(sub, &r->ff)) return -NX_ENOENT;
        r->kind = K_FAT; return 0;
    }
    fs_node_t* n = ramfs_resolve(abs, NULL);
    if (!n) return -NX_ENOENT;
    r->kind = K_RAMFS; r->node = n;
    return 0;
}

static bool res_is_dir(const res_t* r) {
    if (r->kind == K_RAMFS) return r->node->type == FS_DIRECTORY;
    if (r->kind == K_FAT) return r->ff.is_dir;
    return false;
}

/* ============================================================================
 * Blocking / waking
 * ============================================================================ */
static void block_on(int kind, uint32_t obj, uint32_t timeout_ticks) {
    process_t* cur = process_get_current();
    int s = slot_of(cur);
    uext[s].wait_kind = (uint8_t)kind;
    uext[s].wait_obj = obj;
    cur->wake_tick = timeout_ticks ? system_ticks + timeout_ticks : 0;
    cur->state = PROC_BLOCKED;
    int saved = uproc_in_syscall;          /* others run with a clean counter */
    uproc_in_syscall = 0;
    while (cur->state == PROC_BLOCKED) {
        sched_yield();
        if (cur->state == PROC_BLOCKED) __asm__ volatile("hlt");
    }
    uproc_in_syscall = saved;
    uext[s].wait_kind = W_NONE;
}

static void usys_wake(int kind, uint32_t obj) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!uext[i].used || uext[i].wait_kind != kind) continue;
        if (obj && uext[i].wait_obj && uext[i].wait_obj != obj) continue;
        if (t[i].state != PROC_BLOCKED) continue;
        t[i].state = PROC_READY;
        t[i].wake_tick = 0;
        uext[i].wait_kind = W_NONE;
    }
}

static bool any_user_ready(void) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (t[i].is_user && t[i].state == PROC_READY) return true;
    return false;
}

/* ============================================================================
 * Zombies
 * ============================================================================ */
static void zomb_add(uint32_t pid, uint32_t ppid, int status) {
    int free_i = -1;
    for (int i = 0; i < NZOMB; i++) if (!zomb[i].valid) { free_i = i; break; }
    if (free_i < 0) { for (int i = 1; i < NZOMB; i++) zomb[i - 1] = zomb[i]; free_i = NZOMB - 1; }
    zomb[free_i].valid = true; zomb[free_i].pid = pid; zomb[free_i].ppid = ppid; zomb[free_i].status = status;
}

static int zomb_find(uint32_t pid, uint32_t ppid) {
    for (int i = 0; i < NZOMB; i++) {
        if (!zomb[i].valid) continue;
        if (pid != 0 && zomb[i].pid != pid) continue;
        if (zomb[i].ppid != ppid) continue;
        return i;
    }
    return -1;
}

/* ============================================================================
 * Terminals
 * ============================================================================ */
static void tty_defaults(tty_t* T) {
    memset(&T->tio, 0, sizeof(T->tio));
    T->tio.c_iflag = NX_ICRNL | NX_IXON | NX_BRKINT;
    T->tio.c_oflag = NX_OPOST | NX_ONLCR;
    T->tio.c_cflag = NX_CS8;
    T->tio.c_lflag = NX_ISIG | NX_ICANON | NX_ECHO | NX_ECHOE | NX_IEXTEN;
    T->tio.c_cc[NX_VINTR] = 3; T->tio.c_cc[NX_VEOF] = 4;
    T->tio.c_cc[NX_VMIN] = 1; T->tio.c_cc[NX_VTIME] = 0;
    T->in_head = T->in_tail = 0; T->linelen = 0; T->eof_pending = false;
}

static int tty_index(tty_t* T) { return (int)(T - ttys); }

static void con_paint(vt_t* v, int row, int col, uint8_t ch, uint8_t attr) {
    (void)v;
    vga_putchar_at((char)ch, row, col, attr);
}

static void inq_push(tty_t* T, uint8_t c) {
    int nx = (T->in_head + 1) % INQ;
    if (nx == T->in_tail) return;          /* full: drop */
    T->inq[T->in_head] = c; T->in_head = nx;
}
static int inq_count(const tty_t* T) { return (T->in_head - T->in_tail + INQ) % INQ; }

static void vt_reply_cb(vt_t* v, const char* s) {
    tty_t* T = (tty_t*)v->owner;
    while (*s) inq_push(T, (uint8_t)*s++);
    usys_wake(W_TTY, (uint32_t)tty_index(T) + 1);
}

static void con_present(bool force) {
    tty_t* T = &ttys[USYS_TTY_CONSOLE];
    if (!T->dirty) return;
    if (!force && system_ticks - con_last_flush < 1) return;
    vga_set_cursor(T->vt.cy, T->vt.cx);
    vga_flush();
    con_last_flush = system_ticks;
    T->dirty = false;
}

static void tty_write(tty_t* T, const char* s, int n) {
    int t = tty_index(T);
    if (t == USYS_TTY_CONSOLE && posix_is_capturing()) {      /* `prog > file` */
        for (int i = 0; i < n; i++) posix_capture_putc(s[i]);
        return;
    }
    bool onlcr = (T->tio.c_oflag & NX_OPOST) && (T->tio.c_oflag & NX_ONLCR);
    for (int i = 0; i < n; i++) {
        if (onlcr && s[i] == '\n') vt_putc(&T->vt, '\r');
        vt_putc(&T->vt, (uint8_t)s[i]);
    }
    T->dirty = true;
    if (t == USYS_TTY_CONSOLE) con_present(false);
}

static void tty_echo(tty_t* T, const char* s) { tty_write(T, s, (int)strlen(s)); }

static void tty_kill_fg(tty_t* T, int sig) {
    int t = tty_index(T);
    process_t* tab = process_get_table();
    bool any = false;
    for (int pass = 0; pass < 2 && !any; pass++) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (!uext[i].used || uext[i].tty != t || !tab[i].is_user || !proc_alive(&tab[i])) continue;
            if (pass == 0 && uext[i].wait_kind == W_CHILD) continue;    /* a shell waiting: spare it */
            if (&tab[i] == process_get_current()) continue;
            killing_sig = sig;
            uproc_kill(tab[i].pid, UID_ROOT);
            killing_sig = 0;
            any = true;
        }
    }
    T->linelen = 0;
}

/* One key from the keyboard (translated KEYC_* codes included). */
static void tty_input(tty_t* T, uint8_t key) {
    uint32_t lflag = T->tio.c_lflag;
    const char* seq = NULL;
    switch (key) {
    case KEYC_UP:     seq = "\x1b[A"; break;
    case KEYC_DOWN:   seq = "\x1b[B"; break;
    case KEYC_RIGHT:  seq = "\x1b[C"; break;
    case KEYC_LEFT:   seq = "\x1b[D"; break;
    case KEYC_HOME:   seq = "\x1b[H"; break;
    case KEYC_END:    seq = "\x1b[F"; break;
    case KEYC_PGUP:   seq = "\x1b[5~"; break;
    case KEYC_PGDN:   seq = "\x1b[6~"; break;
    case KEYC_INSERT: seq = "\x1b[2~"; break;
    case KEYC_DELETE: seq = "\x1b[3~"; break;
    default: break;
    }
    if (key >= 0x80 && !seq) return;       /* F-keys & friends: not for programs */

    if ((lflag & NX_ISIG) && key == 3) {   /* Ctrl+C */
        if (lflag & NX_ECHO) tty_echo(T, "^C\n");
        tty_kill_fg(T, NX_SIGINT);
        return;
    }
    if (!(lflag & NX_ICANON)) {            /* raw mode: deliver as-is */
        if (seq) { while (*seq) inq_push(T, (uint8_t)*seq++); }
        else {
            if (key == '\n' && (T->tio.c_iflag & NX_ICRNL) == 0) key = '\r';
            inq_push(T, key);
            if (lflag & NX_ECHO) { char c = (char)key; tty_write(T, &c, 1); }
        }
        usys_wake(W_TTY, (uint32_t)tty_index(T) + 1);
        return;
    }
    /* cooked (canonical) mode */
    if (seq) return;                       /* arrows etc. are ignored while line editing */
    if (key == '\b' || key == 0x7F) {
        if (T->linelen > 0) { T->linelen--; if (lflag & NX_ECHO) tty_echo(T, "\b \b"); }
        return;
    }
    if (key == 21) {                       /* Ctrl+U: kill line */
        while (T->linelen > 0) { T->linelen--; if (lflag & NX_ECHO) tty_echo(T, "\b \b"); }
        return;
    }
    if (key == 4) {                        /* Ctrl+D */
        if (T->linelen == 0) T->eof_pending = true;
        else { for (int i = 0; i < T->linelen; i++) inq_push(T, (uint8_t)T->line[i]); T->linelen = 0; }
        usys_wake(W_TTY, (uint32_t)tty_index(T) + 1);
        return;
    }
    if (key == '\n' || key == '\r') {
        if (T->linelen < LINEMAX) T->line[T->linelen++] = '\n';
        if (lflag & NX_ECHO) tty_echo(T, "\n");
        for (int i = 0; i < T->linelen; i++) inq_push(T, (uint8_t)T->line[i]);
        T->linelen = 0;
        usys_wake(W_TTY, (uint32_t)tty_index(T) + 1);
        return;
    }
    if (key == '\t') key = ' ';
    if (key >= 32 && key < 127 && T->linelen < LINEMAX - 1) {
        T->line[T->linelen++] = (char)key;
        if (lflag & NX_ECHO) { char c = (char)key; tty_write(T, &c, 1); }
    }
}

static int32_t tty_read(tty_t* T, char* buf, uint32_t n, bool nonblock) {
    int t = tty_index(T);
    for (;;) {
        int avail = inq_count(T);
        if (avail > 0) {
            uint32_t got = 0;
            bool canon = (T->tio.c_lflag & NX_ICANON) != 0;
            while (got < n && T->in_tail != T->in_head) {
                uint8_t c = T->inq[T->in_tail];
                T->in_tail = (T->in_tail + 1) % INQ;
                buf[got++] = (char)c;
                if (canon && c == '\n') break;
            }
            return (int32_t)got;
        }
        if (T->eof_pending) { T->eof_pending = false; return 0; }
        if (nonblock) return -NX_EAGAIN;
        if (t == USYS_TTY_CONSOLE) con_present(true);
        if (!(T->tio.c_lflag & NX_ICANON) && T->tio.c_cc[NX_VMIN] == 0) {
            uint32_t vt = T->tio.c_cc[NX_VTIME];
            if (vt == 0) return 0;
            uint32_t ticks = (vt * 18u) / 10u; if (ticks == 0) ticks = 1;
            block_on(W_TTY, (uint32_t)t + 1, ticks);
            if (inq_count(T) == 0 && !T->eof_pending) return 0;    /* timed out */
            continue;
        }
        block_on(W_TTY, (uint32_t)t + 1, 0);
    }
}

/* ============================================================================
 * FPU
 * ============================================================================ */
void usys_fpu_switch(process_t* cur, process_t* next) {
    if (cur && cur->is_user) {
        int s = slot_of(cur);
        if (have_fxsr) __asm__ volatile("fxsave (%0)" :: "r"(fpu_area[s]) : "memory");
        else           __asm__ volatile("fnsave (%0); fwait" :: "r"(fpu_area[s]) : "memory");
        fpu_valid[s] = true;
    }
    if (next && next->is_user) {
        int s = slot_of(next);
        if (fpu_valid[s]) {
            if (have_fxsr) __asm__ volatile("fxrstor (%0)" :: "r"(fpu_area[s]) : "memory");
            else           __asm__ volatile("frstor (%0)" :: "r"(fpu_area[s]) : "memory");
        } else {
            if (have_fxsr) {
                memset(fpu_area[s], 0, sizeof(fpu_area[s]));
                const uint16_t control = 0x037F;
                const uint32_t mxcsr = 0x1F80;
                memcpy(fpu_area[s], &control, sizeof(control));
                memcpy(fpu_area[s] + 24, &mxcsr, sizeof(mxcsr));
                __asm__ volatile("fxrstor (%0)" :: "r"(fpu_area[s]) : "memory");
            } else {
                __asm__ volatile("fninit; fldz; fldz; fldz; fldz; fldz; fldz; fldz; fldz; fninit");
            }
        }
    }
}

static void fpu_enable(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    have_fxsr = (d & (1u << 24)) != 0;
    bool have_sse = (d & (1u << 25)) != 0;
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);                     /* EM = 0: x87 instructions execute */
    cr0 &= ~(1u << 3);                     /* TS = 0: no lazy #NM              */
    cr0 |=  (1u << 1);                     /* MP = 1                           */
    cr0 |=  (1u << 5);                     /* NE = 1: native FP exceptions     */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    if (have_fxsr) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9);                  /* OSFXSR: fxsave/fxrstor + SSE     */
        if (have_sse) cr4 |= (1u << 10);   /* OSXMMEXCPT                       */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    }
    __asm__ volatile("fninit");
}

/* ============================================================================
 * Process exit / kill hooks
 * ============================================================================ */
void usys_on_exit(process_t* p, uint32_t code) {
    int s = slot_of(p);
    fpu_valid[s] = false;
    int status;
    if (killing_sig) { status = killing_sig & 0x7F; }
    else if (code == 0xEE) status = NX_SIGSEGV;
    else status = (int)((code & 0xFF) << 8);
    if (uext[s].used) {
        for (int i = 0; i < NFD; i++) if (uext[s].fd[i] >= 0) { ofile_release(uext[s].fd[i]); uext[s].fd[i] = -1; }
        uext[s].used = false;
        uext[s].wait_kind = W_NONE;
    }
    /* orphans: forget zombies nobody will ever wait for */
    for (int i = 0; i < NZOMB; i++) if (zomb[i].valid && zomb[i].ppid == p->pid) zomb[i].valid = false;
    zomb_add(p->pid, p->ppid, status);
    for (int t = 0; t < NTTY; t++) if (ttys[t].fg_pid == p->pid) ttys[t].fg_pid = 0;
    usys_wake(W_CHILD, p->pid);
}

int32_t usys_kill(uint32_t pid, int sig) {
    process_t* p = find_proc(pid);
    if (!p || !p->is_user || !proc_alive(p)) return -1000;
    if (sig == 0) return 0;
    if (sig < 1 || sig > 31) return -NX_EINVAL;
    killing_sig = sig;
    int rc = uproc_kill(pid, users_current_uid());
    killing_sig = 0;
    if (rc == -1) return -NX_ESRCH;
    if (rc == -2) return -NX_EPERM;
    return 0;
}

bool usys_fg_active(void) {
    return ttys[USYS_TTY_CONSOLE].active || ttys[USYS_TTY_GUI].active;
}

/* ============================================================================
 * Spawning
 * ============================================================================ */
typedef struct { int kind; fs_node_t* node; fat32_file_t ff; } src_t;

static int src_read(void* ctx, uint32_t off, uint32_t len, void* dst) {
    src_t* s = (src_t*)ctx;
    if (s->kind == K_RAMFS) return (int)vfs_read(s->node, off, len, (uint8_t*)dst);
    return (int)fat32_read_file(&s->ff, off, len, dst);
}

static bool regular_file(const char* abs, res_t* r) {
    if (resolve(abs, r) != 0) return false;
    if (r->kind == K_RAMFS) return r->node->type == FS_FILE && vfs_check_perm(r->node, users_current_uid(), ACC_READ | ACC_EXEC);
    if (r->kind == K_FAT) return !r->ff.is_dir;
    return false;
}

bool usys_find_program(const char* name, const char* cwd, char* out, int max) {
    res_t r;
    char abs[PATHMAX];
    if (!name || !name[0]) return false;
    bool has_slash = false;
    for (const char* p = name; *p; p++) if (*p == '/') has_slash = true;
    if (has_slash) {
        norm_path(cwd ? cwd : "/", name, abs);
        if (!regular_file(abs, &r)) return false;
        strncpy(out, abs, max - 1); out[max - 1] = 0;
        return true;
    }
    if ((int)strlen(name) + 12 >= PATHMAX) return false;
    strcpy(abs, "/disk/bin/"); strcat(abs, name);
    if (regular_file(abs, &r)) { strncpy(out, abs, max - 1); out[max - 1] = 0; return true; }
    strcpy(abs, "/bin/"); strcat(abs, name);
    if (regular_file(abs, &r)) { strncpy(out, abs, max - 1); out[max - 1] = 0; return true; }
    norm_path(cwd ? cwd : "/", name, abs);
    if (regular_file(abs, &r)) { strncpy(out, abs, max - 1); out[max - 1] = 0; return true; }
    return false;
}

/* Default environment when a program is started from a kernel terminal. */
static int default_env(char* strs, int cap, char** envp, const char* cwd) {
    int n = 0, used = 0;
    const char* kv[8][2] = {
        { "PATH=",  "/disk/bin:/bin" },
        { "HOME=",  env_get("HOME") ? env_get("HOME") : "/" },
        { "USER=",  users_current_name() },
        { "TERM=",  "vt100" },
        { "PWD=",   cwd ? cwd : "/" },
        { "SHELL=", "/disk/bin/sh" },
        { "OS=",    "NexusOS" },
        { "LANG=",  "C" },
    };
    for (int i = 0; i < 8; i++) {
        int l = (int)strlen(kv[i][0]) + (int)strlen(kv[i][1]) + 1;
        if (used + l > cap) break;
        strcpy(strs + used, kv[i][0]); strcat(strs + used, kv[i][1]);
        envp[n++] = strs + used; used += l;
    }
    return n;
}

/* Create the process. argv/envp are kernel strings. parent_slot < 0 => the
 * kernel thread launched it (stdio = tty). */
static process_t* spawn_program(const char* abs, int argc, char* const* argv, int envc, char* const* envp,
                                int parent_slot, int tty, const char* cwd) {
    res_t r;
    if (!regular_file(abs, &r)) return NULL;
    src_t src; src.kind = r.kind; src.node = r.node; src.ff = r.ff;
    uint32_t size = (r.kind == K_RAMFS) ? r.node->size : r.ff.size;
    const char* nm = abs; for (const char* p = abs; *p; p++) if (*p == '/' && p[1]) nm = p + 1;

    process_t* p = uproc_spawn_from(nm, size, src_read, &src, argc, argv, envc, envp);
    if (!p) return NULL;
    int s = slot_of(p);
    uext_t* e = &uext[s];
    memset(e, 0, sizeof(*e));
    e->used = true;
    for (int i = 0; i < NFD; i++) e->fd[i] = -1;
    e->tty = tty;
    e->mmap_next = NX_MMAP_BASE;
    strncpy(e->cwd, cwd ? cwd : "/", PATHMAX - 1); e->cwd[PATHMAX - 1] = 0;
    fpu_valid[s] = false;
    if (parent_slot >= 0 && uext[parent_slot].used) {
        for (int i = 0; i < NFD; i++) {
            if (uext[parent_slot].cloexec & (1u << i)) continue;
            int oi = uext[parent_slot].fd[i];
            if (oi >= 0) { ofiles[oi].refs++; e->fd[i] = (int8_t)oi; }
        }
    } else {
        int oi = ofile_alloc();
        if (oi >= 0) {
            ofiles[oi].type = OF_TTY; ofiles[oi].tty = tty; ofiles[oi].refs = 3;
            e->fd[0] = e->fd[1] = e->fd[2] = (int8_t)oi;
        }
    }
    return p;
}

/* Split a command line into argv (quotes '...' and "..." supported). Returns
 * argc; sets *bg if a trailing '&' was present. Strings go into `store`. */
static int split_cmdline(const char* line, char* store, int cap, char** argv, int maxargs, bool* bg) {
    int argc = 0, used = 0;
    const char* p = line;
    *bg = false;
    while (*p && argc < maxargs - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '&' && (p[1] == 0 || p[1] == ' ')) { *bg = true; p++; continue; }
        argv[argc] = store + used;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) {
                    if (q == '"' && *p == '\\' && p[1]) p++;
                    if (used >= cap - 1) return 0;
                    store[used++] = *p++;
                }
                if (*p != q) return 0;
                p++;
            } else {
                if (*p == '\\' && p[1]) p++;
                if (used >= cap - 1) return 0;
                store[used++] = *p;
                p++;
            }
        }
        store[used++] = 0;
        argc++;
        if (used >= cap - 1) break;
    }
    argv[argc] = NULL;
    return argc;
}

/* ============================================================================
 * Console (text shell) job
 * ============================================================================ */
static void console_begin(void) {
    tty_t* T = &ttys[USYS_TTY_CONSOLE];
    vt_init(&T->vt, 80, 24, con_paint, vt_reply_cb, T);
    for (int r = 0; r < 24; r++)
        for (int c = 0; c < 80; c++) { uint16_t cell = vga_get_cell(r, c); vt_import(&T->vt, r, c, (uint8_t)cell, (uint8_t)(cell >> 8)); }
    int row = vga_get_cursor_row(), col = vga_get_cursor_col();
    if (row > 23) row = 23;
    vt_set_cursor(&T->vt, row, col);
    tty_defaults(T);
    T->cols = 80; T->rows = 24;
    T->active = true; T->dirty = false;
}

static void console_end(void) {
    tty_t* T = &ttys[USYS_TTY_CONSOLE];
    T->active = false;
    vga_set_cursor(T->vt.cy, T->vt.cx);
    if (T->vt.cx != 0) vga_putchar('\n');
    vga_flush();
}

/* Report + drop zombies of background jobs the kernel thread launched. */
static void reap_kernel_zombies(uint32_t keep_pid) {
    process_t* k = process_get_current();
    if (!k) return;
    for (int i = 0; i < NZOMB; i++) {
        if (!zomb[i].valid || zomb[i].ppid != k->pid || zomb[i].pid == keep_pid) continue;
        zomb[i].valid = false;
    }
}

static int wait_console_job(uint32_t pid) {
    tty_t* T = &ttys[USYS_TTY_CONSOLE];
    int status = 0;
    for (;;) {
        int zi = zomb_find(pid, 0);
        if (zi >= 0) { status = zomb[zi].status; zomb[zi].valid = false; break; }
        process_t* p = find_proc(pid);
        if (!p) break;
        while (keyboard_has_key()) tty_input(T, (uint8_t)keyboard_getchar());
        reap_kernel_zombies(pid);
        uproc_reap();
        con_present(true);
        if (any_user_ready()) {
            sched_yield();
        } else {
            keyboard_idle_hook_t idle = keyboard_get_idle_hook();
            if (idle) idle(); else sched_yield();
            __asm__ volatile("hlt");
        }
    }
    return status;
}

int usys_console_run(const char* line, const char* cwd, bool* found) {
    static char store[1024];
    char* argv[32];
    bool bg;
    char prog[PATHMAX];
    *found = false;
    int argc = split_cmdline(line, store, sizeof(store), argv, 32, &bg);
    if (argc == 0) return -1;
    if (!usys_find_program(argv[0], cwd, prog, sizeof(prog))) return -1;
    *found = true;

    tty_t* T = &ttys[USYS_TTY_CONSOLE];
    if (!T->active) console_begin();
    bool nested = false;
    static char envstore[1024];
    char* envp[16];
    int envc = default_env(envstore, sizeof(envstore), envp, cwd);
    process_t* p = spawn_program(prog, argc, argv, envc, envp, -1, USYS_TTY_CONSOLE, cwd);
    if (!p) {
        tty_echo(T, "cannot execute: "); tty_echo(T, prog); tty_echo(T, "\n");
        if (!nested) console_end();
        return 126;
    }
    if (bg) {
        char nb[12]; int_to_str((int)p->pid, nb);
        tty_echo(T, "[bg] pid "); tty_echo(T, nb); tty_echo(T, "\n");
        /* let the background job start (and print) before the prompt returns */
        uint32_t t0 = system_ticks;
        while (system_ticks - t0 < 2) { uproc_reap(); sched_yield(); __asm__ volatile("hlt"); }
        console_end();
        return 0;
    }
    T->fg_pid = p->pid;
    int status = wait_console_job(p->pid);
    T->fg_pid = 0;
    console_end();
    if (status & 0x7F) {
        char nb[12]; int_to_str(status & 0x7F, nb);
        vga_print_color("  [killed by signal ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print_color(nb, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print_color("]\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        return 128 + (status & 0x7F);
    }
    int code = (status >> 8) & 0xFF;
    if (code) {
        char nb[12]; int_to_str(code, nb);
        vga_print_color("  [exit ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print_color(nb, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print_color("]\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    }
    return code;
}

/* ---- shell helpers --------------------------------------------------------- */
void usys_cmd_apps(void) {
    fat32_file_t dir, e;
    if (!fat32_is_mounted() || !fat32_lookup("bin", &dir) || !dir.is_dir) {
        vga_print_color("  No /disk/bin (run 'build.bat apps' to install the userland).\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        return;
    }
    vga_print_color("\n  User programs in /disk/bin:\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    int col = 0, n = 0;
    for (uint32_t i = 0; fat32_dir_entry(&dir, i, &e); i++) {
        if (e.is_dir) continue;
        char nb[12]; int_to_str((int)((e.size + 1023) / 1024), nb);
        if (col == 0) vga_print("  ");
        vga_print_color(e.name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
        int pad = 10 - (int)strlen(e.name); while (pad-- > 0) vga_putchar(' ');
        vga_print_color(nb, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print_color("K  ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        if (++col == 4) { vga_print("\n"); col = 0; }
        n++;
    }
    if (col) vga_print("\n");
    char nb[12]; int_to_str(n, nb);
    vga_print_color("  ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color(nb, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color(" programs. Type a name to run it (args, quotes and a trailing & work).\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

void usys_cmd_which(const char* name, const char* cwd) {
    char out[PATHMAX];
    if (!name || !name[0]) { vga_print("  Usage: which <program>\n"); return; }
    if (usys_find_program(name, cwd, out, sizeof(out))) { vga_print("  "); vga_print(out); vga_print("\n"); }
    else { vga_print_color("  ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); vga_print(name); vga_print_color(": not found in /disk/bin:/bin\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); }
}

bool usys_disk_ls(const char* path) {
    const char* sub;
    if (!path || !is_disk_path(path, &sub)) return false;
    fat32_file_t d, e;
    if (!fat32_is_mounted()) { vga_print_color("  /disk: no FAT32 volume mounted.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return true; }
    if (!fat32_lookup(sub, &d)) { vga_print_color("  No such path.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return true; }
    if (!d.is_dir) { vga_print("  "); vga_print(d.name); vga_print("\n"); return true; }
    vga_print_color("\n  Files:\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    for (uint32_t i = 0; fat32_dir_entry(&d, i, &e); i++) {
        char nb[12]; int_to_str((int)e.size, nb);
        vga_print("  ");
        if (e.is_dir) { vga_print_color(e.name, VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK)); vga_print_color("/", VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK)); }
        else vga_print_color(e.name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
        vga_print("  ("); vga_print(nb); vga_print(" bytes)\n");
    }
    vga_print("\n");
    return true;
}

bool usys_disk_cat(const char* path) {
    const char* sub;
    if (!path || !is_disk_path(path, &sub)) return false;
    fat32_file_t f;
    if (!fat32_is_mounted() || !fat32_lookup(sub, &f) || f.is_dir) { vga_print_color("  Not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); vga_print(path); vga_print("\n"); return true; }
    static char buf[512];
    uint32_t off = 0;
    vga_print("  ");
    while (off < f.size) {
        int32_t n = fat32_read_file(&f, off, sizeof(buf), buf);
        if (n <= 0) break;
        for (int32_t i = 0; i < n; i++) { char c = buf[i]; if (c == '\r') continue; vga_putchar(c); if (c == '\n') vga_print("  "); }
        off += (uint32_t)n;
    }
    vga_print("\n");
    return true;
}

/* ============================================================================
 * GUI Terminal job
 * ============================================================================ */
void usys_gui_resize(int cols, int rows) {
    if (cols < 20) cols = 20;
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;
    if (rows < 5) rows = 5;
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;
    gui_cols = cols; gui_rows = rows;
}

bool usys_gui_launch(const char* line, const char* cwd) {
    static char store[1024];
    static char envstore[1024];
    char* argv[32]; char* envp[16];
    bool bg;
    char prog[PATHMAX];
    int argc = split_cmdline(line, store, sizeof(store), argv, 32, &bg);
    if (argc == 0) return false;
    if (!usys_find_program(argv[0], cwd, prog, sizeof(prog))) return false;
    tty_t* T = &ttys[USYS_TTY_GUI];
    vt_init(&T->vt, gui_cols, gui_rows, NULL, vt_reply_cb, T);
    tty_defaults(T);
    T->cols = gui_cols; T->rows = gui_rows;
    T->active = true; T->dirty = true; T->exit_pending = false;
    int envc = default_env(envstore, sizeof(envstore), envp, cwd);
    process_t* p = spawn_program(prog, argc, argv, envc, envp, -1, USYS_TTY_GUI, cwd);
    if (!p) {
        tty_echo(T, "cannot execute: "); tty_echo(T, prog); tty_echo(T, "\n");
        T->active = false; T->exit_pending = true; T->exit_code = 126;
        return true;
    }
    T->fg_pid = p->pid;
    return true;
}

bool usys_gui_job_active(void) { return ttys[USYS_TTY_GUI].active; }

void usys_gui_key(char key) {
    if (!ttys[USYS_TTY_GUI].active) return;
    tty_input(&ttys[USYS_TTY_GUI], (uint8_t)key);
}

void usys_gui_paste(const char* s) {
    if (!ttys[USYS_TTY_GUI].active || !s) return;
    while (*s) { char c = *s++; if (c == '\r') continue; tty_input(&ttys[USYS_TTY_GUI], (uint8_t)c); }
}

const vt_t* usys_gui_vt(void) { return &ttys[USYS_TTY_GUI].vt; }

bool usys_gui_dirty(void) {
    tty_t* T = &ttys[USYS_TTY_GUI];
    bool d = T->dirty || T->vt.dirty;
    T->dirty = false; T->vt.dirty = false;
    return d;
}

void usys_gui_kill(void) {
    tty_t* T = &ttys[USYS_TTY_GUI];
    process_t* tab = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!uext[i].used || uext[i].tty != USYS_TTY_GUI || !tab[i].is_user || !proc_alive(&tab[i])) continue;
        killing_sig = NX_SIGKILL; uproc_kill(tab[i].pid, UID_ROOT); killing_sig = 0;
    }
    reap_kernel_zombies(0);
    uproc_reap();
    T->active = false; T->fg_pid = 0; T->exit_pending = false;
}

bool usys_gui_take_exit(int* code) {
    tty_t* T = &ttys[USYS_TTY_GUI];
    if (T->exit_pending) { T->exit_pending = false; if (code) *code = T->exit_code; return true; }
    if (!T->active) return false;
    uint32_t pid = T->fg_pid;
    int zi = pid ? zomb_find(pid, 0) : -1;
    if (zi < 0 && pid && find_proc(pid)) {
        /* still running: give it CPU and reap finished helpers */
        reap_kernel_zombies(pid);
        uproc_reap();
        if (any_user_ready()) sched_yield();
        return false;
    }
    int status = 0;
    if (zi >= 0) { status = zomb[zi].status; zomb[zi].valid = false; }
    reap_kernel_zombies(0);
    uproc_reap();
    T->active = false; T->fg_pid = 0;
    if (code) *code = (status & 0x7F) ? 128 + (status & 0x7F) : ((status >> 8) & 0xFF);
    return true;
}

/* ============================================================================
 * Syscalls
 * ============================================================================ */
static void fill_stat(struct nx_stat* st, const ofile_t* o) {
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1; st->st_blksize = 512;
    switch (o->type) {
    case OF_RAMFS: case OF_DIR_RAMFS:
        st->st_mode = ((o->node->type == FS_DIRECTORY) ? NX_S_IFDIR : NX_S_IFREG) | (o->node->mode & 0777);
        st->st_size = o->node->size; st->st_ino = o->node->inode; st->st_uid = o->node->uid; st->st_dev = 1;
        break;
    case OF_FAT: case OF_DIR_FAT:
        st->st_mode = (o->ff.is_dir ? NX_S_IFDIR : NX_S_IFREG) | 0755;
        st->st_size = o->ff.size; st->st_ino = o->ff.first; st->st_dev = 2;
        st->st_mtime = fat32_time_to_unix(o->ff.wdate, o->ff.wtime);
        break;
    case OF_TTY:  st->st_mode = NX_S_IFCHR | 0620; st->st_dev = 3; break;
    case OF_NULL: st->st_mode = NX_S_IFCHR | 0666; st->st_dev = 3; break;
    case OF_PIPE_R: case OF_PIPE_W: st->st_mode = NX_S_IFIFO | 0600; st->st_dev = 3; break;
    case OF_PROC: st->st_mode = NX_S_IFREG | 0444; st->st_dev = 3; break;
    default: break;
    }
}

static int32_t open_from_res(uext_t* e, const res_t* r, uint32_t flags, const char* abs) {
    int oi = ofile_alloc();
    if (oi < 0) return -NX_ENFILE;
    ofile_t* o = &ofiles[oi];
    o->flags = (uint16_t)(flags & (NX_O_ACCMODE | NX_O_APPEND | NX_O_NONBLOCK));
    switch (r->kind) {
    case K_RAMFS:
        o->node = r->node;
        o->type = (r->node->type == FS_DIRECTORY) ? OF_DIR_RAMFS : OF_RAMFS;
        break;
    case K_FAT:
        o->ff = r->ff;
        o->type = r->ff.is_dir ? OF_DIR_FAT : OF_FAT;
        break;
    case K_PROC: o->type = OF_PROC; strncpy(o->path, abs, PATHMAX - 1); break;
    case K_NULL: o->type = OF_NULL; break;
    case K_TTY:  o->type = OF_TTY; o->tty = e->tty; break;
    default: o->type = OF_FREE; return -NX_ENOENT;
    }
    int fd = fd_alloc(e, 0);
    if (fd < 0) { o->type = OF_FREE; return -NX_EMFILE; }
    e->fd[fd] = (int8_t)oi;
    return fd;
}

static int32_t sys_fd_open(uext_t* e, uint32_t upath, uint32_t flags, uint32_t mode) {
    char rel[PATHMAX], abs[PATHMAX];
    (void)mode;
    int rc = ustr(upath, rel, sizeof(rel));
    if (rc < 0) return rc;
    norm_path(e->cwd, rel, abs);
    res_t r;
    rc = resolve(abs, &r);
    uint32_t acc = flags & NX_O_ACCMODE;
    bool wr = (acc == NX_O_WRONLY || acc == NX_O_RDWR);
    if (rc == 0 && (flags & NX_O_CREAT) && (flags & NX_O_EXCL)) return -NX_EEXIST;
    if (rc != 0) {
        if (!(flags & NX_O_CREAT)) return rc;
        const char* sub;
        if (is_disk_path(abs, &sub)) {
            if (!fat32_is_mounted()) return -NX_ENOENT;
            int cr = fat32_create_path(sub, false, &r.ff);
            if (cr == -3) return -NX_ENOENT;
            if (cr == -4) return -NX_ENOSPC;
            if (cr != 0) return -NX_EINVAL;
            r.kind = K_FAT;
        } else {
            char leaf[FS_NAME_MAX];
            fs_node_t* parent = ramfs_resolve_parent(abs, NULL, leaf);
            if (!parent || parent->type != FS_DIRECTORY) return -NX_ENOENT;
            if (!vfs_check_perm(parent, users_current_uid(), ACC_WRITE)) return -NX_EACCES;
            if (!leaf[0] || strlen(leaf) >= FS_NAME_MAX) return -NX_ENAMETOOLONG;
            fs_node_t* n = ramfs_mknode(parent, leaf, FS_FILE);
            if (!n) return -NX_ENOSPC;
            r.kind = K_RAMFS; r.node = n;
        }
    } else {
        if (res_is_dir(&r)) {
            if (wr) return -NX_EISDIR;
        } else if (flags & NX_O_DIRECTORY) return -NX_ENOTDIR;
        if (r.kind == K_RAMFS && r.node->type == FS_FILE) {
            uint32_t uid = users_current_uid();
            if (!vfs_check_perm(r.node, uid, wr ? ACC_WRITE : ACC_READ)) return -NX_EACCES;
            if (acc == NX_O_RDWR && !vfs_check_perm(r.node, uid, ACC_READ)) return -NX_EACCES;
        }
        if (wr && (flags & NX_O_TRUNC)) {
            if (r.kind == K_RAMFS && r.node->type == FS_FILE) { r.node->size = 0; persist_note_write(r.node); }
            else if (r.kind == K_FAT && !r.ff.is_dir) fat32_truncate_file(&r.ff, 0);
        }
    }
    return open_from_res(e, &r, flags, abs);
}

static int32_t sys_fd_read(uext_t* e, int fd, uint32_t ubuf, uint32_t n) {
    ofile_t* o = fd_get(e, fd);
    if (!o) return -NX_EBADF;
    if ((o->flags & NX_O_ACCMODE) == NX_O_WRONLY) return -NX_EBADF;
    if (n == 0) return 0;
    if (n > 0x7FFFFFFF || !uok(ubuf, n)) return -NX_EFAULT;
    char* buf = (char*)ubuf;
    bool nb = (o->flags & NX_O_NONBLOCK) != 0;
    switch (o->type) {
    case OF_TTY: return tty_read(&ttys[o->tty], buf, n, nb);
    case OF_RAMFS: {
        int32_t r = vfs_read(o->node, o->pos, n, (uint8_t*)buf);
        if (r < 0) return -NX_EACCES;
        o->pos += (uint32_t)r; return r; }
    case OF_FAT: {
        int32_t r = fat32_read_file(&o->ff, o->pos, n, buf);
        if (r < 0) return -NX_EIO;
        o->pos += (uint32_t)r; return r; }
    case OF_PIPE_R:
        for (;;) {
            pipe_t* pp = pipe_get(o->pipe);
            if (!pp) return 0;
            if (pp->count > 0) {
                int32_t r = pipe_read(o->pipe, (uint8_t*)buf, n);
                usys_wake(W_PIPE, (uint32_t)o->pipe);
                return r;
            }
            if (pp->write_closed) return 0;
            if (nb) return -NX_EAGAIN;
            block_on(W_PIPE, (uint32_t)o->pipe, 0);
        }
    case OF_NULL: return 0;
    case OF_PROC: {
        static char pb[PROCFS_MAX_SIZE + 1];
        int len = procfs_read(o->path, pb, PROCFS_MAX_SIZE);
        if (len < 0) return -NX_EIO;
        if (o->pos >= (uint32_t)len) return 0;
        uint32_t k = (uint32_t)len - o->pos; if (k > n) k = n;
        memcpy(buf, pb + o->pos, k); o->pos += k; return (int32_t)k; }
    case OF_DIR_RAMFS: case OF_DIR_FAT: return -NX_EISDIR;
    default: return -NX_EBADF;
    }
}

static int32_t sys_fd_write(uext_t* e, int fd, uint32_t ubuf, uint32_t n) {
    ofile_t* o = fd_get(e, fd);
    if (!o) return -NX_EBADF;
    if ((o->flags & NX_O_ACCMODE) == NX_O_RDONLY && o->type != OF_TTY) return -NX_EBADF;
    if (n == 0) return 0;
    if (n > 0x7FFFFFFF || !uok(ubuf, n)) return -NX_EFAULT;
    const char* buf = (const char*)ubuf;
    bool nb = (o->flags & NX_O_NONBLOCK) != 0;
    switch (o->type) {
    case OF_TTY: tty_write(&ttys[o->tty], buf, (int)n); return (int32_t)n;
    case OF_RAMFS: {
        if (o->flags & NX_O_APPEND) o->pos = o->node->size;
        int32_t r = vfs_write(o->node, o->pos, n, (const uint8_t*)buf);
        if (r < 0) return -NX_EACCES;
        if (r == 0) return -NX_ENOSPC;
        o->pos += (uint32_t)r; return r; }
    case OF_FAT: {
        if (o->flags & NX_O_APPEND) o->pos = o->ff.size;
        int r = fat32_write_file(&o->ff, o->pos, n, buf);
        if (r < 0) return (r == -2) ? -NX_ENOSPC : -NX_EIO;
        o->pos += (uint32_t)r; return r; }
    case OF_PIPE_W: {
        uint32_t done = 0;
        while (done < n) {
            pipe_t* pp = pipe_get(o->pipe);
            if (!pp || pp->read_closed) return done ? (int32_t)done : -NX_EPIPE;
            int32_t w = pipe_write(o->pipe, (const uint8_t*)buf + done, n - done);
            if (w < 0) return done ? (int32_t)done : -NX_EPIPE;
            if (w > 0) { done += (uint32_t)w; usys_wake(W_PIPE, (uint32_t)o->pipe); continue; }
            if (nb) return done ? (int32_t)done : -NX_EAGAIN;
            block_on(W_PIPE, (uint32_t)o->pipe, 0);
        }
        return (int32_t)done; }
    case OF_NULL: return (int32_t)n;
    case OF_DIR_RAMFS: case OF_DIR_FAT: return -NX_EISDIR;
    default: return -NX_EBADF;
    }
}

static int32_t sys_lseek(uext_t* e, int fd, int32_t off, int whence) {
    ofile_t* o = fd_get(e, fd);
    if (!o) return -NX_EBADF;
    uint32_t size;
    switch (o->type) {
    case OF_RAMFS: size = o->node->size; break;
    case OF_FAT:   size = o->ff.size; break;
    case OF_NULL:  return 0;
    case OF_PROC:  size = PROCFS_MAX_SIZE; break;
    case OF_DIR_RAMFS: case OF_DIR_FAT: if (whence == NX_SEEK_SET && off == 0) { o->pos = 0; return 0; } return -NX_EINVAL;
    default: return -NX_ESPIPE;
    }
    int32_t base = (whence == NX_SEEK_SET) ? 0 : (whence == NX_SEEK_CUR) ? (int32_t)o->pos : (whence == NX_SEEK_END) ? (int32_t)size : -1;
    if (base < 0) return -NX_EINVAL;
    int64_t np = (int64_t)base + off;
    if (np < 0 || np > 0x7FFFFFFF) return -NX_EINVAL;
    o->pos = (uint32_t)np;
    return np;
}

static int32_t sys_getdents(uext_t* e, int fd, uint32_t ubuf, uint32_t n) {
    ofile_t* o = fd_get(e, fd);
    if (!o) return -NX_EBADF;
    if (o->type != OF_DIR_RAMFS && o->type != OF_DIR_FAT) return -NX_ENOTDIR;
    if (!uok(ubuf, n)) return -NX_EFAULT;
    struct nx_dirent* d = (struct nx_dirent*)ubuf;
    uint32_t cnt = n / sizeof(struct nx_dirent), out = 0;
    while (out < cnt) {
        struct nx_dirent rec; memset(&rec, 0, sizeof(rec));
        if (o->type == OF_DIR_RAMFS) {
            fs_node_t* c = vfs_readdir(o->node, o->pos);
            if (c) {
                rec.d_ino = c->inode; rec.d_size = c->size;
                rec.d_type = (c->type == FS_DIRECTORY) ? NX_DT_DIR : NX_DT_REG;
                strncpy(rec.d_name, c->name, sizeof(rec.d_name) - 1);
            } else if (o->node == vfs_get_root() && fat32_is_mounted()) {
                /* synthetic /disk entry right after the last ramfs child */
                uint32_t k = 0; while (vfs_readdir(o->node, k)) k++;
                if (o->pos != k) break;
                rec.d_ino = 0xD15C; rec.d_type = NX_DT_DIR; strcpy(rec.d_name, "disk");
            } else break;
        } else {
            fat32_file_t fe;
            if (!fat32_dir_entry(&o->ff, o->pos, &fe)) break;
            rec.d_ino = fe.first; rec.d_size = fe.size;
            rec.d_type = fe.is_dir ? NX_DT_DIR : NX_DT_REG;
            strncpy(rec.d_name, fe.name, sizeof(rec.d_name) - 1);
        }
        d[out++] = rec;
        o->pos++;
    }
    return (int32_t)(out * sizeof(struct nx_dirent));
}

static int32_t sys_stat_path(uext_t* e, uint32_t upath, uint32_t ust) {
    char rel[PATHMAX], abs[PATHMAX];
    int rc = ustr(upath, rel, sizeof(rel));
    if (rc < 0) return rc;
    if (!uok(ust, sizeof(struct nx_stat))) return -NX_EFAULT;
    norm_path(e->cwd, rel, abs);
    res_t r;
    rc = resolve(abs, &r);
    if (rc < 0) return rc;
    ofile_t tmp; memset(&tmp, 0, sizeof(tmp));
    switch (r.kind) {
    case K_RAMFS: tmp.type = (r.node->type == FS_DIRECTORY) ? OF_DIR_RAMFS : OF_RAMFS; tmp.node = r.node; break;
    case K_FAT:   tmp.type = r.ff.is_dir ? OF_DIR_FAT : OF_FAT; tmp.ff = r.ff; break;
    case K_PROC:  tmp.type = OF_PROC; break;
    case K_NULL:  tmp.type = OF_NULL; break;
    default:      tmp.type = OF_TTY; break;
    }
    fill_stat((struct nx_stat*)ust, &tmp);
    return 0;
}

static int32_t sys_unlink_path(uext_t* e, uint32_t upath, bool rmdir) {
    char rel[PATHMAX], abs[PATHMAX];
    int rc = ustr(upath, rel, sizeof(rel));
    if (rc < 0) return rc;
    norm_path(e->cwd, rel, abs);
    const char* sub;
    if (is_disk_path(abs, &sub)) {
        fat32_file_t f;
        if (!fat32_is_mounted() || !fat32_lookup(sub, &f)) return -NX_ENOENT;
        if (rmdir && !f.is_dir) return -NX_ENOTDIR;
        if (!rmdir && f.is_dir) return -NX_EISDIR;
        int r = fat32_remove_path(sub);
        return r == 0 ? 0 : (r == -3) ? -NX_ENOTEMPTY : (r == -2) ? -NX_EBUSY : -NX_ENOENT;
    }
    if (rmdir) {
        int r = ramfs_rmdir(abs, NULL);
        return r == 0 ? 0 : (r == -2) ? -NX_EPERM : (r == -3) ? -NX_ENOTEMPTY : (r == -4) ? -NX_ENOTDIR : -NX_ENOENT;
    }
    int r = ramfs_rm_path(abs, NULL);
    return r == 0 ? 0 : (r == -2) ? -NX_EPERM : (r == -4) ? -NX_EISDIR : -NX_ENOENT;
}

static int32_t sys_mkdir_path(uext_t* e, uint32_t upath) {
    char rel[PATHMAX], abs[PATHMAX];
    int rc = ustr(upath, rel, sizeof(rel));
    if (rc < 0) return rc;
    norm_path(e->cwd, rel, abs);
    const char* sub;
    if (is_disk_path(abs, &sub)) {
        if (!fat32_is_mounted()) return -NX_ENOENT;
        if (!*sub) return -NX_EEXIST;
        int r = fat32_create_path(sub, true, NULL);
        return r == 0 ? 0 : (r == -2) ? -NX_EEXIST : (r == -3) ? -NX_ENOENT : (r == -4) ? -NX_ENOSPC : -NX_EINVAL;
    }
    int r = ramfs_mkdir(abs, NULL);
    return r == 0 ? 0 : (r == -2) ? -NX_EACCES : (r == -3) ? -NX_ENOENT : -NX_EEXIST;
}

static int32_t sys_rename_path(uext_t* e, uint32_t uold, uint32_t unew) {
    char a[PATHMAX], b[PATHMAX], absa[PATHMAX], absb[PATHMAX];
    int rc = ustr(uold, a, sizeof(a)); if (rc < 0) return rc;
    rc = ustr(unew, b, sizeof(b)); if (rc < 0) return rc;
    norm_path(e->cwd, a, absa); norm_path(e->cwd, b, absb);
    if (strcmp(absa, absb) == 0) return 0;
    const char* sa; const char* sb;
    bool da = is_disk_path(absa, &sa), db = is_disk_path(absb, &sb);
    if (da != db) return -NX_EXDEV;
    if (da) return -NX_EXDEV;                         /* FAT rename: mv copies */
    fs_node_t* n = ramfs_resolve(absa, NULL);
    if (!n) return -NX_ENOENT;
    if (ramfs_resolve(absb, NULL)) return -NX_EEXIST;
    char leaf[FS_NAME_MAX];
    fs_node_t* parent = ramfs_resolve_parent(absb, NULL, leaf);
    if (!parent || parent != n->parent) return -NX_EXDEV;
    if (!leaf[0] || strlen(leaf) >= FS_NAME_MAX) return -NX_ENAMETOOLONG;
    uint32_t cu = users_current_uid();
    if (cu != UID_ROOT && n->uid != cu) return -NX_EPERM;
    persist_note_rename(n);
    strcpy(n->name, leaf);
    return 0;
}

static int32_t sys_chdir_path(uext_t* e, uint32_t upath) {
    char rel[PATHMAX], abs[PATHMAX];
    int rc = ustr(upath, rel, sizeof(rel));
    if (rc < 0) return rc;
    norm_path(e->cwd, rel, abs);
    res_t r;
    rc = resolve(abs, &r);
    if (rc < 0) return rc;
    if (!res_is_dir(&r)) return -NX_ENOTDIR;
    strcpy(e->cwd, abs);
    return 0;
}

static int32_t sys_spawn(uext_t* e, uint32_t upath, uint32_t uargv, uint32_t uenvp) {
    char rel[PATHMAX], prog[PATHMAX];
    int rc = ustr(upath, rel, sizeof(rel));
    if (rc < 0) return rc;
    if (!usys_find_program(rel, e->cwd, prog, sizeof(prog))) return -NX_ENOENT;
    int used = 0, argc = 0, envc = 0;
    /* argv */
    for (;;) {
        if (argc >= 31) return -NX_E2BIG;
        uint32_t pa = uargv + 4u * (uint32_t)argc;
        if (!uok(pa, 4)) return -NX_EFAULT;
        uint32_t sp = *(uint32_t*)pa;
        if (!sp) break;
        int l = ustr(sp, sp_strs + used, (int)sizeof(sp_strs) - used);
        if (l < 0) return l;
        sp_argv[argc++] = sp_strs + used; used += l + 1;
    }
    if (argc == 0) return -NX_EINVAL;
    if (uenvp) {
        for (;;) {
            if (envc >= 31) break;
            uint32_t pa = uenvp + 4u * (uint32_t)envc;
            if (!uok(pa, 4)) return -NX_EFAULT;
            uint32_t sp = *(uint32_t*)pa;
            if (!sp) break;
            int l = ustr(sp, sp_strs + used, (int)sizeof(sp_strs) - used);
            if (l < 0) return l;
            sp_envp[envc++] = sp_strs + used; used += l + 1;
        }
    } else {
        envc = default_env(sp_strs + used, (int)sizeof(sp_strs) - used, sp_envp, e->cwd);
    }
    process_t* cur = process_get_current();
    process_t* p = spawn_program(prog, argc, sp_argv, envc, sp_envp, slot_of(cur), e->tty, e->cwd);
    if (!p) return -NX_ENOMEM;
    return (int32_t)p->pid;
}

static bool has_live_child(uint32_t mypid, int pid) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_alive(&t[i]) && t[i].ppid == mypid && (pid <= 0 || t[i].pid == (uint32_t)pid)) return true;
    return false;
}

static int32_t sys_waitpid(int pid, uint32_t ustatus, uint32_t options) {
    process_t* cur = process_get_current();
    if (ustatus && !uok(ustatus, 4)) return -NX_EFAULT;
    for (;;) {
        int zi = zomb_find(pid > 0 ? (uint32_t)pid : 0, cur->pid);
        if (zi >= 0) {
            if (ustatus) *(int*)ustatus = zomb[zi].status;
            uint32_t z = zomb[zi].pid;
            zomb[zi].valid = false;
            return (int32_t)z;
        }
        if (!has_live_child(cur->pid, pid)) return -NX_ECHILD;
        if (options & NX_WNOHANG) return 0;
        block_on(W_CHILD, pid > 0 ? (uint32_t)pid : 0, 0);
    }
}

static uint32_t civil_to_epoch(int y, int m, int d, int hh, int mm, int ss) {
    if (m < 1 || m > 12) m = 1;
    if (d < 1) d = 1;
    y -= (m <= 2);
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = (uint32_t)(era * 146097 + doe - 719468);
    return days * 86400u + (uint32_t)hh * 3600u + (uint32_t)mm * 60u + (uint32_t)ss;
}

static int32_t sys_clock_gettime(uint32_t clk, uint32_t uts) {
    if (!uok(uts, sizeof(struct nx_timespec))) return -NX_EFAULT;
    struct nx_timespec* ts = (struct nx_timespec*)uts;
    uint32_t ticks = system_ticks;
    if (clk == NX_CLOCK_REALTIME) {
        rtc_time_t t; rtc_read(&t);
        ts->tv_sec = civil_to_epoch(t.year, t.month, t.day, t.hour, t.minute, t.second);
        ts->tv_nsec = (ticks % 18u) * 55555555u;
        return 0;
    }
    if (clk == NX_CLOCK_MONOTONIC) {
        ts->tv_sec = ticks / 18u;
        ts->tv_nsec = (ticks % 18u) * 55555555u;
        return 0;
    }
    return -NX_EINVAL;
}

static int32_t sys_nanosleep(uint32_t ureq) {
    if (!uok(ureq, sizeof(struct nx_timespec))) return -NX_EFAULT;
    const struct nx_timespec* r = (const struct nx_timespec*)ureq;
    if (r->tv_sec > 86400 || r->tv_nsec >= 1000000000u) return -NX_EINVAL;
    uint32_t ms = r->tv_sec * 1000u + r->tv_nsec / 1000000u;
    uint32_t ticks = (ms * 18u) / 1000u;
    if (ticks == 0 && ms) ticks = 1;
    if (ticks == 0) { sched_yield(); return 0; }
    block_on(W_SLEEP, 0, ticks);
    return 0;
}

static int32_t sys_ioctl(uext_t* e, int fd, uint32_t req, uint32_t arg) {
    ofile_t* o = fd_get(e, fd);
    if (!o) return -NX_EBADF;
    if (o->type != OF_TTY) return -NX_ENOTTY;
    tty_t* T = &ttys[o->tty];
    switch (req) {
    case NX_TCGETS:
        if (!uok(arg, sizeof(struct nx_termios))) return -NX_EFAULT;
        memcpy((void*)arg, &T->tio, sizeof(struct nx_termios));
        return 0;
    case NX_TCSETS: case NX_TCSETSW: case NX_TCSETSF:
        if (!uok(arg, sizeof(struct nx_termios))) return -NX_EFAULT;
        memcpy(&T->tio, (const void*)arg, sizeof(struct nx_termios));
        if (req == NX_TCSETSF) { T->in_head = T->in_tail = 0; T->linelen = 0; }
        return 0;
    case NX_TIOCGWINSZ: {
        if (!uok(arg, sizeof(struct nx_winsize))) return -NX_EFAULT;
        struct nx_winsize* w = (struct nx_winsize*)arg;
        w->ws_row = (unsigned short)T->vt.rows; w->ws_col = (unsigned short)T->vt.cols;
        w->ws_xpixel = (unsigned short)(T->vt.cols * 8); w->ws_ypixel = (unsigned short)(T->vt.rows * 16);
        return 0; }
    default: return -NX_EINVAL;
    }
}

static int32_t sys_ftruncate(uext_t* e, int fd, uint32_t len) {
    ofile_t* o = fd_get(e, fd);
    if (!o) return -NX_EBADF;
    if ((o->flags & NX_O_ACCMODE) == NX_O_RDONLY) return -NX_EBADF;
    if (o->type == OF_RAMFS) {
        fs_node_t* n = o->node;
        if (len <= n->size) { n->size = len; persist_note_write(n); return 0; }
        static const uint8_t z[512];
        while (n->size < len) {
            uint32_t k = len - n->size; if (k > 512) k = 512;
            int32_t w = vfs_write(n, n->size, k, z);
            if (w <= 0) return -NX_EFBIG;
        }
        return 0;
    }
    if (o->type == OF_FAT) return fat32_truncate_file(&o->ff, len) == 0 ? 0 : -NX_EIO;
    return -NX_EINVAL;
}

static int32_t sys_mmap_anon(uext_t* e, uint32_t len) {
    process_t* cur = process_get_current();
    if (len == 0) return -NX_EINVAL;
    uint32_t pages = (len + 4095u) / 4096u;
    uint32_t base = e->mmap_next;
    if (base + pages * 4096u > 0x40000000u || base + pages * 4096u < base) return -NX_ENOMEM;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va = base + i * 4096u;
        uint32_t ph = upmm_alloc_frame();
        if (!ph) return -NX_ENOMEM;
        as_map_page(cur->cr3, va, ph, PAGE_WRITABLE | PAGE_USER);
        memset((void*)va, 0, 4096);
    }
    e->mmap_next = base + pages * 4096u;
    return (int32_t)base;
}

static int32_t sys_fcntl(uext_t* e, int fd, uint32_t cmd, uint32_t arg) {
    ofile_t* o = fd_get(e, fd);
    if (!o) return -NX_EBADF;
    switch (cmd) {
    case NX_F_GETFL: return (int32_t)o->flags;
    case NX_F_SETFL: o->flags = (uint16_t)((o->flags & NX_O_ACCMODE) | (arg & (NX_O_APPEND | NX_O_NONBLOCK))); return 0;
    case NX_F_GETFD: return (e->cloexec >> fd) & 1u;
    case NX_F_SETFD:
        if (arg & ~1u) return -NX_EINVAL;
        e->cloexec = (e->cloexec & ~(1u << fd)) | ((arg & 1u) << fd);
        return 0;
    case NX_F_DUPFD: {
        int nf = fd_alloc(e, (int)arg);
        if (nf < 0) return -NX_EMFILE;
        e->fd[nf] = e->fd[fd]; o->refs++;
        return nf; }
    default: return -NX_EINVAL;
    }
}

int32_t usys_syscall(uint32_t num, struct registers* regs) {
    uext_t* e = cur_ext();
    if (!e) return -NX_ENOSYS;                     /* the kernel thread never gets here */
    uint32_t a = regs->ebx, b = regs->ecx, c = regs->edx;
    switch (num) {
    case NX_SYS_FD_OPEN:   return sys_fd_open(e, a, b, c);
    case NX_SYS_FD_READ:   return sys_fd_read(e, (int)a, b, c);
    case NX_SYS_FD_WRITE:  return sys_fd_write(e, (int)a, b, c);
    case NX_SYS_FD_CLOSE: {
        if ((int)a < 0 || a >= NFD || e->fd[a] < 0) return -NX_EBADF;
        ofile_release(e->fd[a]); e->fd[a] = -1; return 0; }
    case NX_SYS_LSEEK:     return sys_lseek(e, (int)a, (int32_t)b, (int)c);
    case NX_SYS_FSTAT: {
        ofile_t* o = fd_get(e, (int)a);
        if (!o) return -NX_EBADF;
        if (!uok(b, sizeof(struct nx_stat))) return -NX_EFAULT;
        fill_stat((struct nx_stat*)b, o); return 0; }
    case NX_SYS_STAT:      return sys_stat_path(e, a, b);
    case NX_SYS_UNLINK:    return sys_unlink_path(e, a, false);
    case NX_SYS_RMDIR:     return sys_unlink_path(e, a, true);
    case NX_SYS_RENAME:    return sys_rename_path(e, a, b);
    case NX_SYS_MKDIR:     return sys_mkdir_path(e, a);
    case NX_SYS_GETDENTS:  return sys_getdents(e, (int)a, b, c);
    case NX_SYS_CHDIR:     return sys_chdir_path(e, a);
    case NX_SYS_GETCWD: {
        uint32_t l = strlen(e->cwd) + 1;
        if (b < l) return -NX_ERANGE;
        if (!uok(a, l)) return -NX_EFAULT;
        memcpy((void*)a, e->cwd, l); return (int32_t)l; }
    case NX_SYS_DUP: {
        ofile_t* o = fd_get(e, (int)a); if (!o) return -NX_EBADF;
        int nf = fd_alloc(e, 0); if (nf < 0) return -NX_EMFILE;
        e->fd[nf] = e->fd[a]; o->refs++; return nf; }
    case NX_SYS_DUP2: {
        ofile_t* o = fd_get(e, (int)a); if (!o) return -NX_EBADF;
        if ((int)b < 0 || b >= NFD) return -NX_EBADF;
        if (a == b) return (int32_t)b;
        if (e->fd[b] >= 0) ofile_release(e->fd[b]);
        e->cloexec &= ~(1u << b);
        e->fd[b] = e->fd[a]; o->refs++; return (int32_t)b; }
    case NX_SYS_SPAWN:     return sys_spawn(e, a, b, c);
    case NX_SYS_WAITPID:   return sys_waitpid((int)a, b, c);
    case NX_SYS_CLOCK_GETTIME: return sys_clock_gettime(a, b);
    case NX_SYS_NANOSLEEP: return sys_nanosleep(a);
    case NX_SYS_ISATTY: {
        ofile_t* o = fd_get(e, (int)a); if (!o) return -NX_EBADF;
        return o->type == OF_TTY ? 1 : 0; }
    case NX_SYS_IOCTL:     return sys_ioctl(e, (int)a, b, c);
    case NX_SYS_UNAME: {
        if (!uok(a, sizeof(struct nx_utsname))) return -NX_EFAULT;
        struct nx_utsname* u = (struct nx_utsname*)a;
        memset(u, 0, sizeof(*u));
        strcpy(u->sysname, "NexusOS");
        strcpy(u->nodename, env_get("HOSTNAME") ? env_get("HOSTNAME") : "nexus");
        strcpy(u->release, NEXUS_VERSION);
        strcpy(u->version, NEXUS_CODENAME " Phase 57 userland");
        strcpy(u->machine, "i386");
        return 0; }
    case NX_SYS_ACCESS: {
        char rel[PATHMAX], abs[PATHMAX]; res_t r;
        int rc = ustr(a, rel, sizeof(rel)); if (rc < 0) return rc;
        norm_path(e->cwd, rel, abs);
        rc = resolve(abs, &r); if (rc < 0) return rc;
        if (r.kind == K_RAMFS && (b & (NX_R_OK | NX_W_OK))) {
            uint32_t uid = users_current_uid();
            if ((b & NX_R_OK) && !vfs_check_perm(r.node, uid, ACC_READ)) return -NX_EACCES;
            if ((b & NX_W_OK) && !vfs_check_perm(r.node, uid, ACC_WRITE)) return -NX_EACCES;
        }
        return 0; }
    case NX_SYS_FTRUNCATE: return sys_ftruncate(e, (int)a, b);
    case NX_SYS_PIPE2: {
        if (!uok(a, 8)) return -NX_EFAULT;
        int id = pipe_create(); if (id < 0) return -NX_ENFILE;
        int r = ofile_alloc(); if (r < 0) { pipe_close_read(id); pipe_close_write(id); return -NX_ENFILE; }
        ofiles[r].type = OF_PIPE_R; ofiles[r].pipe = id; ofiles[r].flags = NX_O_RDONLY;
        int w = ofile_alloc(); if (w < 0) { ofiles[r].type = OF_FREE; pipe_close_read(id); pipe_close_write(id); return -NX_ENFILE; }
        ofiles[w].type = OF_PIPE_W; ofiles[w].pipe = id; ofiles[w].flags = NX_O_WRONLY;
        int f0 = fd_alloc(e, 0); if (f0 < 0) { ofiles[r].type = ofiles[w].type = OF_FREE; pipe_close_read(id); pipe_close_write(id); return -NX_EMFILE; }
        e->fd[f0] = (int8_t)r;
        int f1 = fd_alloc(e, 0); if (f1 < 0) { ofile_release(r); e->fd[f0] = -1; ofiles[w].type = OF_FREE; pipe_close_write(id); return -NX_EMFILE; }
        e->fd[f1] = (int8_t)w;
        ((int*)a)[0] = f0; ((int*)a)[1] = f1;
        return 0; }
    case NX_SYS_MMAP_ANON: return sys_mmap_anon(e, a);
    case NX_SYS_MUNMAP:    return -NX_ENOSYS;
    case NX_SYS_FCNTL:     return sys_fcntl(e, (int)a, b, c);
    case NX_SYS_TTYNAME: {
        ofile_t* o = fd_get(e, (int)a); if (!o) return -NX_EBADF;
        if (o->type != OF_TTY) return -NX_ENOTTY;
        if (c < 9) return -NX_ERANGE;
        if (!uok(b, 9)) return -NX_EFAULT;
        memcpy((void*)b, "/dev/tty", 9); return 0; }
    case NX_SYS_GETPPID: { process_t* p = process_get_current(); return (int32_t)(p ? p->ppid : 0); }
    default: return -NX_ENOSYS;
    }
}

/* ============================================================================
 * Init
 * ============================================================================ */
void usys_init(void) {
    memset(ofiles, 0, sizeof(ofiles));
    memset(uext, 0, sizeof(uext));
    memset(zomb, 0, sizeof(zomb));
    memset(fpu_valid, 0, sizeof(fpu_valid));
    for (int t = 0; t < NTTY; t++) {
        memset(&ttys[t], 0, sizeof(tty_t));
        tty_defaults(&ttys[t]);
        vt_init(&ttys[t].vt, t == 0 ? 80 : gui_cols, t == 0 ? 24 : gui_rows, NULL, vt_reply_cb, &ttys[t]);
    }
    fpu_enable();
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Userland ABI: 31 POSIX calls, fd tables, 2 VT100 ttys, FPU ");
    vga_print(have_fxsr ? "(fxsave)\n" : "(fnsave)\n");
}
