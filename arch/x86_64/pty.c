/* Unix98 pseudo-terminals (/dev/ptmx, /dev/pts/N, /dev/tty) with Linux N_TTY line-discipline semantics:
 * canonical editing and echo, signal characters, flow control, VMIN/VTIME, window size, packet mode,
 * controlling terminals and job-control checks (SIGTTIN/SIGTTOU) for background process groups. */
#include "native.h"
#include "../../kernel/string.h"

#define PTY_MAX 1024U           /* Linux kernel.pty.max is 4096 */
#define INPUT_SIZE 4096U        /* N_TTY_BUF_SIZE */
#define OUTPUT_SIZE 16384U
/* asm-generic/termbits.h */
#define ISTRIP 0040U
#define INLCR 0100U
#define IGNCR 0200U
#define ICRNL 0400U
#define IUCLC 01000U
#define IXON 02000U
#define IXANY 04000U
#define IMAXBEL 020000U
#define IUTF8 040000U
#define PARMRK 010U
#define OPOST 01U
#define OLCUC 02U
#define ONLCR 04U
#define OCRNL 010U
#define ONOCR 020U
#define ONLRET 040U
#define TABDLY 014000U
#define XTABS 014000U
#define ISIG 01U
#define ICANON 02U
#define ECHO 010U
#define ECHOE 020U
#define ECHOK 040U
#define ECHONL 0100U
#define NOFLSH 0200U
#define TOSTOP 0400U
#define ECHOCTL 01000U
#define ECHOPRT 02000U
#define ECHOKE 04000U
#define IEXTEN 0100000U
enum { VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, VSWTC, VSTART, VSTOP, VSUSP, VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2 };
struct ktermios { uint32_t iflag, oflag, cflag, lflag; uint8_t line, cc[19]; uint32_t ispeed, ospeed; };
struct old_termio { uint16_t iflag, oflag, cflag, lflag; uint8_t line, cc[8]; };
struct window_size { uint16_t rows, columns, xpixel, ypixel; };
_Static_assert(sizeof(struct ktermios) == 44 && sizeof(struct old_termio) == 18 && sizeof(struct window_size) == 8, "termios ABI");

struct terminal_end { struct terminal* tty; bool master; };
struct terminal {
    struct terminal* next;
    unsigned index, slave_opens, master_opens;
    bool locked, other_closed, hung_up, exclusive, stopped, lnext, erasing, packet;
    uint8_t packet_status;
    struct ktermios termios; struct window_size size;
    uint32_t session, group, uid;       /* internal session/foreground group ids; slave node owner */
    uint8_t *input, *output;
    unsigned in_head, in_count, canon_count; /* input queue (slave reads); [0, canon_count) holds whole lines */
    unsigned out_head, out_count;            /* output queue (master reads) */
    unsigned column, canon_column;
    uint64_t accessed, modified, created;
    uint8_t line_end[INPUT_SIZE / 8];        /* input positions ending a canonical line (NUL there = VEOF) */
    struct terminal_end master, slave;
};
_Static_assert(sizeof(struct terminal) <= 4096, "terminal allocation");
static struct terminal* terminals;
static const struct object_operations terminal_operations;

static bool control(uint8_t c) { return c < 32 || c == 127; }
static bool continuation(struct terminal* t, uint8_t c) { return (t->termios.iflag & IUTF8) && (c & 0xC0) == 0x80; }
static void notify(struct terminal* t) {
    task_wake_channel(t); event_source_changed(&t->master, 5); event_source_changed(&t->slave, 5);
}
/* ---- queues ---- */
static unsigned in_position(struct terminal* t, unsigned i) { return (t->in_head + i) % INPUT_SIZE; }
static uint8_t in_at(struct terminal* t, unsigned i) { return t->input[in_position(t, i)]; }
static bool line_end_at(struct terminal* t, unsigned i) { unsigned p = in_position(t, i); return (t->line_end[p / 8] >> (p % 8)) & 1; }
static void mark_line_end(struct terminal* t, unsigned i, bool end) {
    unsigned p = in_position(t, i); if (end) t->line_end[p / 8] |= 1 << (p % 8); else t->line_end[p / 8] &= ~(1 << (p % 8));
}
static void in_push(struct terminal* t, uint8_t c, bool end) {
    t->input[in_position(t, t->in_count)] = c; mark_line_end(t, t->in_count, end); t->in_count++;
    if (end) t->canon_count = t->in_count;
}
static void in_consume(struct terminal* t, unsigned bytes) {
    t->in_head = (t->in_head + bytes) % INPUT_SIZE; t->in_count -= bytes;
    t->canon_count = t->canon_count > bytes ? t->canon_count - bytes : 0;
}
static void flush_input(struct terminal* t) { t->in_head = t->in_count = t->canon_count = 0; memset(t->line_end, 0, sizeof(t->line_end)); t->lnext = t->erasing = false; }
static void flush_output(struct terminal* t) { t->out_head = t->out_count = 0; }
static unsigned out_room(struct terminal* t) { return OUTPUT_SIZE - t->out_count; }
static void out_put(struct terminal* t, uint8_t c) { t->output[(t->out_head + t->out_count++) % OUTPUT_SIZE] = c; }
/* Output post-processing of one byte (Linux do_output_char); false when the queue has no room. */
static bool post(struct terminal* t, uint8_t c) {
    uint32_t o = t->termios.oflag;
    if (!(o & OPOST)) { if (!out_room(t)) return false; out_put(t, c); return true; }
    switch (c) {
    case '\n':
        if (o & ONLRET) t->column = 0;
        if (o & ONLCR) {
            if (out_room(t) < 2) return false;
            t->canon_column = t->column = 0; out_put(t, '\r'); out_put(t, '\n'); return true;
        }
        t->canon_column = t->column; break;
    case '\r':
        if ((o & ONOCR) && !t->column) return true;
        if (o & OCRNL) { c = '\n'; if (o & ONLRET) t->canon_column = t->column = 0; break; }
        t->canon_column = t->column = 0; break;
    case '\t': {
        unsigned spaces = 8 - (t->column & 7);
        if ((o & TABDLY) == XTABS) {
            if (out_room(t) < spaces) return false;
            t->column += spaces; while (spaces--) out_put(t, ' '); return true;
        }
        t->column += spaces; break;
    }
    case '\b': if (t->column) t->column--; break;
    default:
        if (!control(c)) {
            if ((o & OLCUC) && c >= 'a' && c <= 'z') c -= 32;
            if (!continuation(t, c)) t->column++;
        }
        break;
    }
    if (!out_room(t)) return false;
    out_put(t, c); return true;
}
/* Echoed bytes go through output processing too (Linux __process_echoes); only ^X pairs are emitted raw. */
static void echo_raw(struct terminal* t, uint8_t c) { post(t, c); }
static void echo_char(struct terminal* t, uint8_t c) {
    if ((t->termios.lflag & ECHOCTL) && control(c) && c != '\t') {
        if (out_room(t) < 2) return;
        out_put(t, '^'); out_put(t, c ^ 0100); t->column += 2; return;
    }
    post(t, c);
}
static void finish_erasing(struct terminal* t) { if (t->erasing) { echo_raw(t, '/'); t->erasing = false; } }
/* ---- input processing (Linux n_tty_receive_char*) ---- */
static void packet_event(struct terminal* t, uint8_t status) { if (t->packet) t->packet_status |= status; }
static void start_output(struct terminal* t) { if (t->stopped) { t->stopped = false; t->packet_status &= ~4; packet_event(t, 8); } }
static void stop_output(struct terminal* t) { if (!t->stopped) { t->stopped = true; t->packet_status &= ~8; packet_event(t, 4); } }
static void signal_char(struct terminal* t, unsigned number, uint8_t c) {
    if (!(t->termios.lflag & NOFLSH)) { flush_input(t); flush_output(t); packet_event(t, 3); }
    if (t->group) task_signal_group(t->group, number);
    if (t->termios.iflag & IXON) start_output(t);
    if (t->termios.lflag & ECHO) echo_char(t, c);
}
static void erase(struct terminal* t, uint8_t c) {
    uint32_t l = t->termios.lflag; const uint8_t* cc = t->termios.cc;
    if (t->in_count == t->canon_count) return;
    enum { ERASE, WERASE, KILL } kind = c == cc[VERASE] ? ERASE : c == cc[VWERASE] ? WERASE : KILL;
    if (kind == KILL) {
        if (!(l & ECHO)) { t->in_count = t->canon_count; return; }
        if (!(l & ECHOK) || !(l & ECHOKE) || !(l & ECHOE)) {
            t->in_count = t->canon_count; finish_erasing(t); echo_char(t, c);
            if (l & ECHOK) echo_raw(t, '\n');
            return;
        }
    }
    unsigned alnums = 0;
    while (t->in_count > t->canon_count) {
        unsigned head = t->in_count; uint8_t erased;
        do { head--; erased = in_at(t, head); } while (continuation(t, erased) && head > t->canon_count);
        if (continuation(t, erased)) break; /* never erase part of a character */
        if (kind == WERASE) {
            if ((erased >= '0' && erased <= '9') || (erased >= 'A' && erased <= 'Z') || (erased >= 'a' && erased <= 'z') || erased == '_') alnums++;
            else if (alnums) break;
        }
        unsigned bytes = t->in_count - head; t->in_count = head;
        if (l & ECHO) {
            if (l & ECHOPRT) {
                if (!t->erasing) { echo_raw(t, '\\'); t->erasing = true; }
                for (unsigned i = 0; i < bytes; i++) echo_char(t, in_at(t, head + i));
            } else if (kind == ERASE && !(l & ECHOE)) echo_char(t, cc[VERASE]);
            else if (erased == '\t') {
                /* Columns since the previous tab (or the line start) decide how far to move back. */
                unsigned columns = 0; bool after_tab = false;
                for (unsigned i = head; i > t->canon_count;) {
                    uint8_t before = in_at(t, --i);
                    if (before == '\t') { after_tab = true; break; }
                    if (control(before)) { if (l & ECHOCTL) columns += 2; }
                    else if (!continuation(t, before)) columns++;
                }
                if (!after_tab) columns += t->canon_column;
                for (unsigned back = 8 - (columns & 7); back--;) echo_raw(t, '\b');
            } else {
                if (control(erased) && (l & ECHOCTL)) { echo_raw(t, '\b'); echo_raw(t, ' '); echo_raw(t, '\b'); }
                if (!control(erased) || (l & ECHOCTL)) { echo_raw(t, '\b'); echo_raw(t, ' '); echo_raw(t, '\b'); }
            }
        }
        if (kind == ERASE) break;
    }
    if (t->in_count == t->canon_count && (l & ECHO)) finish_erasing(t);
}
/* Accept one input byte; false when the queue is full and the writer must wait. */
static bool receive(struct terminal* t, uint8_t c) {
    struct ktermios* m = &t->termios; uint32_t i = m->iflag, l = m->lflag; const uint8_t* cc = m->cc;
    bool canonical = (l & ICANON) != 0;
    if (i & ISTRIP) c &= 0x7F;
    if ((i & IUCLC) && (l & IEXTEN) && c >= 'A' && c <= 'Z') c += 32;
    if (t->lnext) {
        if (t->in_count + (canonical ? 1 : 0) >= INPUT_SIZE) {
            if (!canonical || t->canon_count) return false;
            t->lnext = false; return true; /* canonical overflow drops the byte */
        }
        t->lnext = false; goto literal;
    }
    if (i & IXON) {
        if (c && c == cc[VSTOP]) { if (c == cc[VSTART] && t->stopped) start_output(t); else stop_output(t); return true; }
        if (c && c == cc[VSTART]) { start_output(t); return true; }
        if (t->stopped && (i & IXANY)) start_output(t);
    }
    if (l & ISIG) {
        if (c && c == cc[VINTR]) { signal_char(t, 2, c); return true; }
        if (c && c == cc[VQUIT]) { signal_char(t, 3, c); return true; }
        if (c && c == cc[VSUSP]) { signal_char(t, 20, c); return true; }
    }
    if (c == '\r') { if (i & IGNCR) return true; if (i & ICRNL) c = '\n'; }
    else if (c == '\n' && (i & INLCR)) c = '\r';
    if (canonical) {
        if (c && (c == cc[VERASE] || c == cc[VKILL] || (c == cc[VWERASE] && (l & IEXTEN)))) { erase(t, c); return true; }
        if (c && c == cc[VLNEXT] && (l & IEXTEN)) {
            t->lnext = true;
            if (l & ECHO) { finish_erasing(t); if (l & ECHOCTL) { echo_raw(t, '^'); echo_raw(t, '\b'); } }
            return true;
        }
        if (c && c == cc[VREPRINT] && (l & ECHO) && (l & IEXTEN)) {
            finish_erasing(t); echo_char(t, c); echo_raw(t, '\n');
            for (unsigned p = t->canon_count; p < t->in_count; p++) echo_char(t, in_at(t, p));
            return true;
        }
        bool newline = c == '\n', eof = c && c == cc[VEOF], eol = c && (c == cc[VEOL] || (c == cc[VEOL2] && (l & IEXTEN)));
        if (newline || eof || eol) {
            if (t->in_count >= INPUT_SIZE) return !t->canon_count; /* full: wait for the reader, or drop when no line can be read */
            if (newline && (l & (ECHO | ECHONL))) echo_raw(t, '\n');
            if (eol && (l & ECHO)) { if (t->in_count == t->canon_count) t->canon_column = t->column; echo_char(t, c); }
            if (eol && c == 0377 && (i & PARMRK) && t->in_count + 1 < INPUT_SIZE) in_push(t, c, false);
            in_push(t, eof ? 0 : c, true);
            return true;
        }
    }
literal:
    if (t->in_count + (canonical ? 1 : 0) >= INPUT_SIZE) {
        /* Canonical overflow without a complete line drops the byte (the line cannot grow); raw input waits. */
        return canonical && !t->canon_count;
    }
    if (l & ECHO) {
        finish_erasing(t);
        if (t->in_count == t->canon_count) t->canon_column = t->column;
        echo_char(t, c);
    }
    if (c == 0377 && (i & PARMRK) && t->in_count + 2 < INPUT_SIZE) in_push(t, c, false);
    in_push(t, c, false);
    return true;
}
/* ---- controlling terminal and job control ---- */
static bool capable_admin(void) { return credentials_capable(credentials_current(), NULL, 21); }
/* Linux __tty_check_change: a background group touching its controlling terminal gets `number`. */
static int job_check(struct terminal* t, unsigned number) {
    if (task_terminal() != t || !t->session || !t->group) return 0;
    uint32_t group = task_group_id();
    if (group == t->group) return 0;
    if (signals_ignored(signals_current(), number) || signals_masked(signals_current(), number)) return number == 21 ? -5 : 0;
    if (task_group_orphaned(group)) return -5;
    task_signal_group(group, number);
    return -4; /* restarted after the stop (signals_should_restart) */
}
static void disassociate(struct terminal* t, bool exiting) {
    uint32_t session = t->session, group = t->group;
    if (group) { task_signal_group(group, 1); if (!exiting) task_signal_group(group, 18); }
    t->session = t->group = 0;
    if (session) task_terminal_forget(t, session);
}
void terminal_session_exit(struct terminal* t) { if (t->session == task_session_id()) disassociate(t, true); }
static void acquire(struct terminal* t) {
    t->session = task_session_id(); t->group = task_group_id(); task_set_terminal(t);
}
/* ---- lifetime ---- */
static void terminal_free(struct terminal* t) {
    struct terminal** link = &terminals; while (*link && *link != t) link = &(*link)->next;
    if (!*link) panic("unowned pseudo-terminal");
    *link = t->next;
    task_terminal_forget(t, 0);
    arena_free((uint64_t)t->input, INPUT_SIZE + OUTPUT_SIZE); page_free((uint64_t)t);
}
static void terminal_release(void* object) {
    struct terminal_end* end = object; struct terminal* t = end->tty;
    if (end->master) {
        if (--t->master_opens) return;
        /* Master gone: the slave is hung up; its session leader gets SIGHUP and SIGCONT. */
        t->hung_up = true; t->stopped = false;
        uint32_t session = t->session;
        if (session) {
            task_signal_process(session, 1); task_signal_process(session, 18);
            task_terminal_forget(t, session);
        }
        t->session = t->group = 0;
    } else {
        if (--t->slave_opens) return;
        t->other_closed = true;
        /* The last close of the terminal ends its role as a controlling terminal. */
        if (t->session) task_terminal_forget(t, t->session);
        t->session = t->group = 0;
    }
    notify(t);
    if (!t->master_opens && !t->slave_opens) terminal_free(t);
}
static struct terminal* find_terminal(unsigned index) {
    for (struct terminal* t = terminals; t; t = t->next) if (t->index == index && !t->hung_up) return t;
    return NULL;
}
int pty_lookup(const char* name) {
    unsigned index = 0;
    if (!*name || (name[0] == '0' && name[1])) return -1;
    for (const char* p = name; *p; p++) { if (*p < '0' || *p > '9' || index >= PTY_MAX) return -1; index = index * 10 + (unsigned)(*p - '0'); }
    return index < PTY_MAX && find_terminal(index) ? (int)(DEVICE_PTS + index) : -1;
}
static void encode_times(struct linux_stat* stat, uint64_t accessed, uint64_t modified) {
    stat->atime = (int64_t)(accessed / 1000000000); stat->atime_ns = accessed % 1000000000;
    stat->mtime = stat->ctime = (int64_t)(modified / 1000000000); stat->mtime_ns = stat->ctime_ns = modified % 1000000000;
}
void pty_device_stat(unsigned device, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->nlink = 1; stat->block_size = 1024;
    stat->gid = credentials_visible_id(5, true); stat->uid = credentials_visible_id(0, false);
    if (device == DEVICE_PTMX || device == DEVICE_TTY) {
        stat->dev = 9; stat->ino = device; stat->mode = 020666; stat->rdev = device == DEVICE_PTMX ? 0x502 : 0x500; return;
    }
    unsigned index = device - DEVICE_PTS; struct terminal* t = find_terminal(index);
    stat->dev = 11; stat->ino = index + 3; stat->mode = 020620;
    stat->rdev = (index & 0xFF) | (136U << 8) | ((uint64_t)(index & ~0xFFU) << 12);
    if (t) { stat->uid = credentials_visible_id(t->uid, false); encode_times(stat, t->accessed, t->modified); }
}
int pty_directory(uint64_t position, struct directory_item* item) {
    uint64_t seen = 0;
    for (unsigned index = 0; index < PTY_MAX; index++) {
        if (!find_terminal(index)) continue;
        if (seen++ != position) continue;
        number64(index, item->name); item->inode = index + 3; item->type = 2; return 1;
    }
    return 0;
}
static int open_slave(struct terminal* t, uint64_t flags) {
    if (t->locked || t->hung_up) return -5;
    /* Node mode 0620, group tty: reading needs the owner, writing the owner or group tty. */
    struct credentials* caller = credentials_current();
    if (credentials_kernel_id(caller, false, 1) != t->uid && !credentials_capable(caller, NULL, 1) &&
        ((flags & 3) != 1 || credentials_kernel_id(caller, true, 1) != 5)) return -13;
    if (t->exclusive && !capable_admin()) return -16;
    char path[24] = "/dev/pts/"; number64(t->index, path + 9);
    int fd = fd_create_object(&terminal_operations, &t->slave, flags, path); if (fd < 0) return fd;
    t->slave_opens++; t->other_closed = false;
    if (!(flags & 0400) && task_session_leader() && !task_terminal() && !t->session && (flags & 3) != 1) acquire(t);
    notify(t); return fd;
}
int pty_open_device(unsigned device, uint64_t flags) {
    if (flags & 0x10000) return -20;
    if ((flags & 3) == 3) return -22;
    if (device == DEVICE_TTY) {
        struct terminal* t = task_terminal();
        if (!t || t->hung_up) return -6;
        return open_slave(t, flags | 0400);
    }
    if (device != DEVICE_PTMX) {
        struct terminal* t = find_terminal(device - DEVICE_PTS); if (!t) return -2;
        return open_slave(t, flags);
    }
    unsigned index = 0;
    for (; index < PTY_MAX; index++) {
        bool used = false;
        for (struct terminal* t = terminals; t; t = t->next) if (t->index == index) { used = true; break; }
        if (!used) break;
    }
    if (index == PTY_MAX) return -28;
    struct terminal* t = (void*)page_alloc(); if (!t) return -12;
    t->input = (void*)arena_alloc(INPUT_SIZE + OUTPUT_SIZE);
    if (!t->input) { page_free((uint64_t)t); return -12; }
    t->output = t->input + INPUT_SIZE;
    t->index = index; t->locked = true; t->uid = credentials_kernel_id(credentials_current(), false, 1);
    t->master = (struct terminal_end){t, true}; t->slave = (struct terminal_end){t, false};
    t->termios = (struct ktermios){.iflag = ICRNL | IXON, .oflag = OPOST | ONLCR, .cflag = 017 | 060 | 0200,
        .lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN, .ispeed = 38400, .ospeed = 38400};
    static const uint8_t defaults[17] = {3, 034, 0177, 025, 4, 0, 1, 0, 021, 023, 032, 0, 022, 017, 027, 026, 0};
    memcpy(t->termios.cc, defaults, sizeof(defaults));
    t->created = t->accessed = t->modified = native_realtime_ns();
    t->master_opens = 1; t->next = terminals; terminals = t;
    int fd = fd_create_object(&terminal_operations, &t->master, flags, "/dev/ptmx");
    if (fd < 0) { t->master_opens = 0; terminal_free(t); }
    return fd;
}
/* ---- I/O ---- */
static uint32_t ready(void* object) {
    struct terminal_end* end = object; struct terminal* t = end->tty;
    if (end->master) {
        uint32_t mask = (t->out_count || (t->packet && t->packet_status)) ? 1U : 0;
        if (t->in_count < INPUT_SIZE || ((t->termios.lflag & ICANON) && !t->canon_count)) mask |= 4;
        if (t->other_closed) mask |= 16;
        return mask;
    }
    if (t->hung_up) return 1 | 4 | 8 | 16;
    uint32_t mask = 0;
    if (t->termios.lflag & ICANON) { if (t->canon_count) mask |= 1; }
    else {
        unsigned minimum = t->termios.cc[VTIME] ? 1 : t->termios.cc[VMIN];
        if (t->in_count && t->in_count >= (minimum ? minimum : 1)) mask |= 1;
    }
    if (!t->stopped && out_room(t) >= 2) mask |= 4;
    return mask;
}
static void* source(void* object) { return object; }
static int64_t copy_ring(const struct iovec64* vectors, unsigned count, uint64_t done, uint8_t* ring, unsigned size, unsigned start, unsigned bytes) {
    if (!iov_access(vectors, count, done, bytes, true)) return -14;
    unsigned first = size - start; if (first > bytes) first = bytes;
    iov_copy(vectors, count, done, ring + start, first, false);
    if (bytes > first) iov_copy(vectors, count, done + first, ring, bytes - first, false);
    return bytes;
}
static int64_t master_read(struct terminal* t, const struct iovec64* vectors, unsigned count, uint64_t total, bool nonblock) {
    for (;;) {
        if (t->packet && t->packet_status) {
            uint8_t status = t->packet_status; t->packet_status = 0;
            if (!iov_access(vectors, count, 0, 1, true)) return -14;
            iov_copy(vectors, count, 0, &status, 1, false); return 1;
        }
        if (t->out_count) {
            uint64_t done = 0;
            if (t->packet) {
                uint8_t data = 0;
                if (!iov_access(vectors, count, 0, 1, true)) return -14;
                iov_copy(vectors, count, 0, &data, 1, false); done = 1;
            }
            unsigned bytes = total - done < t->out_count ? (unsigned)(total - done) : t->out_count;
            int64_t copied = copy_ring(vectors, count, done, t->output, OUTPUT_SIZE, t->out_head, bytes);
            if (copied < 0) return copied;
            t->out_head = (t->out_head + bytes) % OUTPUT_SIZE; t->out_count -= bytes;
            notify(t); return (int64_t)(done + bytes);
        }
        if (t->other_closed) return -5;
        if (nonblock) return -11;
        int error = task_wait_channel(t); if (error) return error;
    }
}
static int64_t slave_read(struct terminal* t, const struct iovec64* vectors, unsigned count, uint64_t total, bool nonblock) {
    int error = job_check(t, 21); if (error) return error;
    bool canonical = (t->termios.lflag & ICANON) != 0;
    unsigned minimum = 0; uint64_t time = 0, deadline = 0; bool timed = false;
    if (!canonical) {
        minimum = t->termios.cc[VMIN]; uint64_t tenths = t->termios.cc[VTIME];
        if (minimum) time = tenths * 100000000ULL;          /* inter-byte timer once data arrived */
        else { timed = true; deadline = native_clock_ns() + tenths * 100000000ULL; minimum = 1; }
        if (minimum > total) minimum = (unsigned)total;
    }
    uint64_t done = 0;
    for (;;) {
        if (t->hung_up) return (int64_t)done;
        if (canonical && t->canon_count) {
            unsigned bytes = 0; bool eof = false;
            while (bytes < t->canon_count && done + bytes < total) {
                bool end = line_end_at(t, bytes);
                if (end && !in_at(t, bytes)) { eof = true; break; }
                bytes++;
                if (end) break;
            }
            if (bytes) {
                int64_t copied = copy_ring(vectors, count, done, t->input, INPUT_SIZE, t->in_head, bytes);
                if (copied < 0) return done ? (int64_t)done : copied;
                done += bytes;
            }
            /* A VEOF delimiter ends its line without being returned; alone it reads as end of file. */
            in_consume(t, bytes + (eof ? 1 : 0));
            t->accessed = native_realtime_ns(); notify(t); return (int64_t)done;
        }
        if (!canonical && t->in_count) {
            unsigned bytes = total - done < t->in_count ? (unsigned)(total - done) : t->in_count;
            int64_t copied = copy_ring(vectors, count, done, t->input, INPUT_SIZE, t->in_head, bytes);
            if (copied < 0) return done ? (int64_t)done : copied;
            in_consume(t, bytes); done += bytes; t->accessed = native_realtime_ns(); notify(t);
            if (done >= minimum || done == total) return (int64_t)done;
            if (time) { timed = true; deadline = native_clock_ns() + time; }
            continue;
        }
        if (timed && native_clock_ns() >= deadline) return (int64_t)done;
        if (nonblock) return done ? (int64_t)done : -11;
        error = timed ? task_wait_until(t, deadline) : task_wait_channel(t);
        if (error == -110) return (int64_t)done;
        if (error) return done ? (int64_t)done : error;
    }
}
static int64_t master_write(struct terminal* t, const struct iovec64* vectors, unsigned count, uint64_t total, bool nonblock) {
    uint64_t done = 0; uint8_t chunk[256];
    while (done < total) {
        unsigned bytes = total - done < sizeof(chunk) ? (unsigned)(total - done) : (unsigned)sizeof(chunk);
        if (!iov_access(vectors, count, done, bytes, false)) return done ? (int64_t)done : -14;
        iov_copy(vectors, count, done, chunk, bytes, true);
        unsigned used = 0;
        while (used < bytes && receive(t, chunk[used])) used++;
        done += used;
        if (used) { t->modified = native_realtime_ns(); notify(t); }
        if (used < bytes) {
            if (nonblock) return done ? (int64_t)done : -11;
            int error = task_wait_channel(t); if (error) return done ? (int64_t)done : error;
        }
    }
    return (int64_t)done;
}
static int64_t slave_write(struct terminal* t, const struct iovec64* vectors, unsigned count, uint64_t total, bool nonblock) {
    if (t->hung_up) return -5;
    if (t->termios.lflag & TOSTOP) { int error = job_check(t, 22); if (error) return error; }
    uint64_t done = 0; uint8_t chunk[256];
    while (done < total) {
        if (t->hung_up) return done ? (int64_t)done : -5;
        unsigned bytes = total - done < sizeof(chunk) ? (unsigned)(total - done) : (unsigned)sizeof(chunk), used = 0;
        if (!t->stopped) {
            if (!iov_access(vectors, count, done, bytes, false)) return done ? (int64_t)done : -14;
            iov_copy(vectors, count, done, chunk, bytes, true);
            finish_erasing(t);
            while (used < bytes && post(t, chunk[used])) used++;
            done += used;
            if (used) { t->modified = native_realtime_ns(); notify(t); }
        }
        if (used < bytes) {
            if (nonblock) return done ? (int64_t)done : -11;
            int error = task_wait_channel(t); if (error) return done ? (int64_t)done : error;
        }
    }
    return (int64_t)done;
}
static int64_t terminal_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct terminal_end* end = object; struct terminal* t = end->tty; bool nonblock = (flags & 0x800) != 0;
    if (!total) return 0;
    if (total > 0x7FFFF000) total = 0x7FFFF000;
    if (end->master) return write ? master_write(t, vectors, count, total, nonblock) : master_read(t, vectors, count, total, nonblock);
    return write ? slave_write(t, vectors, count, total, nonblock) : slave_read(t, vectors, count, total, nonblock);
}
/* ---- ioctls (Linux tty_ioctl, tty_mode_ioctl, n_tty_ioctl, pty_unix98_ioctl) ---- */
static int put_int(uint64_t address, int value) {
    if (!accessible(address, 4, true)) return -14;
    return uput32(address, (uint32_t)value);
}
static int get_int(uint64_t address, int* value) {
    if (!accessible(address, 4, false)) return -14;
    *value = (int)uget32(address); return 0;
}
static void set_termios(struct terminal* t, const struct ktermios* next) {
    uint32_t changed = t->termios.lflag ^ next->lflag; bool restart = (t->termios.iflag & IXON) && !(next->iflag & IXON);
    t->termios = *next;
    if (changed & ICANON) {
        memset(t->line_end, 0, sizeof(t->line_end)); t->erasing = t->lnext = false;
        if ((next->lflag & ICANON) && t->in_count) { mark_line_end(t, t->in_count - 1, true); t->canon_count = t->in_count; }
        else t->canon_count = 0;
    }
    if (restart) start_output(t);
    notify(t);
}
static unsigned readable(struct terminal* t, bool master) {
    if (master) return t->out_count;
    if (!(t->termios.lflag & ICANON)) return t->in_count;
    unsigned bytes = 0;
    for (unsigned i = 0; i < t->canon_count; i++) if (!line_end_at(t, i) || in_at(t, i)) bytes++;
    return bytes;
}
static int terminal_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    struct terminal_end* end = object; struct terminal* t = end->tty; int value, error;
    command = (uint32_t)command;
    if (t->hung_up && !end->master && command != 0x5422 && command != 0x5401 && command != 0x5413) return -5;
    switch (command) {
    case 0x5401: case 0x802C542A: { /* TCGETS, TCGETS2 */
        unsigned size = command == 0x5401 ? 36 : 44;
        if (!accessible(argument, size, true)) return -14;
        return copy_to_user((void*)argument, &t->termios, size) ? -14 : 0;
    }
    case 0x5402: case 0x5403: case 0x5404: case 0x402C542B: case 0x402C542C: case 0x402C542D: { /* TCSETS[WF], TCSETS[WF]2 */
        if ((error = job_check(t, 22))) return error;
        bool two = command >= 0x402C542B; unsigned size = two ? 44 : 36;
        struct ktermios next = t->termios;
        if (!accessible(argument, size, false)) return -14;
        copy_from_user(&next, (void*)argument, size);
        if (!two) { next.ispeed = t->termios.ispeed; next.ospeed = t->termios.ospeed; }
        if (command == 0x5404 || command == 0x402C542D) { flush_input(t); packet_event(t, 1); }
        set_termios(t, &next); return 0;
    }
    case 0x5405: { /* TCGETA */
        struct old_termio old = {(uint16_t)t->termios.iflag, (uint16_t)t->termios.oflag, (uint16_t)t->termios.cflag, (uint16_t)t->termios.lflag, t->termios.line, {0}};
        memcpy(old.cc, t->termios.cc, 8);
        if (!accessible(argument, sizeof(old), true)) return -14;
        return copy_to_user((void*)argument, &old, sizeof(old)) ? -14 : 0;
    }
    case 0x5406: case 0x5407: case 0x5408: { /* TCSETA[WF]: the low 16 bits of each flag word */
        if ((error = job_check(t, 22))) return error;
        struct old_termio old;
        if (!accessible(argument, sizeof(old), false)) return -14;
        copy_from_user(&old, (void*)argument, sizeof(old));
        struct ktermios next = t->termios;
        next.iflag = (next.iflag & 0xFFFF0000U) | old.iflag; next.oflag = (next.oflag & 0xFFFF0000U) | old.oflag;
        next.cflag = (next.cflag & 0xFFFF0000U) | old.cflag; next.lflag = (next.lflag & 0xFFFF0000U) | old.lflag;
        next.line = old.line; memcpy(next.cc, old.cc, 8);
        if (command == 0x5408) { flush_input(t); packet_event(t, 1); }
        set_termios(t, &next); return 0;
    }
    case 0x5409: case 0x5425: /* TCSBRK, TCSBRKP: a pseudo-terminal has nothing to drain and ignores breaks */
        if ((error = job_check(t, 22))) return error;
        return 0;
    case 0x540A: /* TCXONC */
        if ((error = job_check(t, 22))) return error;
        if (argument == 0) stop_output(t);
        else if (argument == 1) start_output(t);
        else if (argument == 2 || argument == 3) {
            uint8_t c = t->termios.cc[argument == 2 ? VSTOP : VSTART];
            if (c && !post(t, c)) return -11;
        } else return -22;
        notify(t); return 0;
    case 0x540B: /* TCFLSH */
        if ((error = job_check(t, 22))) return error;
        if (argument > 2) return -22;
        if (argument != 1) { if (end->master) flush_output(t); else { flush_input(t); packet_event(t, 1); } }
        if (argument != 0) { if (end->master) flush_input(t); else { flush_output(t); packet_event(t, 2); } }
        notify(t); return 0;
    case 0x540C: t->exclusive = true; return 0;  /* TIOCEXCL */
    case 0x540D: t->exclusive = false; return 0; /* TIOCNXCL */
    case 0x80045440: return put_int(argument, t->exclusive); /* TIOCGEXCL */
    case 0x540E: /* TIOCSCTTY */
        if (task_session_leader() && t->session == task_session_id()) return 0;
        if (!task_session_leader() || task_terminal()) return -1;
        if (t->session) {
            if (argument != 1 || !capable_admin()) return -1;
            task_terminal_forget(t, t->session);
        }
        if ((flags & 3) == 1 && !capable_admin()) return -1;
        acquire(t); return 0;
    case 0x5422: /* TIOCNOTTY */
        if (task_terminal() != t) return -25;
        if (task_session_leader()) disassociate(t, false);
        task_set_terminal(NULL); return 0;
    case 0x540F: /* TIOCGPGRP */
        if (!end->master && task_terminal() != t) return -25;
        return put_int(argument, t->group ? (int)task_visible_pid(t->group) : 0);
    case 0x5429: /* TIOCGSID */
        if (!end->master && task_terminal() != t) return -25;
        if (!t->session) return -25;
        return put_int(argument, (int)task_visible_pid(t->session));
    case 0x5410: { /* TIOCSPGRP */
        if ((error = job_check(t, 22))) return error == -5 ? -25 : error;
        if (task_terminal() != t || t->session != task_session_id()) return -25;
        if ((error = get_int(argument, &value))) return error;
        if (value < 0) return -22;
        uint32_t group = value ? task_internal_pid((uint32_t)value) : 0;
        if (!group) return -3;
        if (!task_group_exists(group, t->session)) return task_group_exists(group, 0) || task_process_id(group) ? -1 : -3;
        t->group = group; return 0;
    }
    case 0x5411: return put_int(argument, 0); /* TIOCOUTQ: output is handed to the peer at once */
    case 0x541B: return put_int(argument, (int)readable(t, end->master)); /* FIONREAD / TIOCINQ */
    case 0x5412: { /* TIOCSTI */
        if (!capable_admin()) return -5;
        if (!accessible(argument, 1, false)) return -14;
        uint8_t c = uget8(argument);
        if (end->master) { if (!post(t, c)) return -11; } else receive(t, c);
        notify(t); return 0;
    }
    case 0x5413: /* TIOCGWINSZ */
        if (!accessible(argument, sizeof(t->size), true)) return -14;
        return copy_to_user((void*)argument, &t->size, sizeof(t->size)) ? -14 : 0;
    case 0x5414: { /* TIOCSWINSZ */
        struct window_size next;
        if (!accessible(argument, sizeof(next), false)) return -14;
        copy_from_user(&next, (void*)argument, sizeof(next));
        if (memcmp(&next, &t->size, sizeof(next))) { t->size = next; if (t->group) task_signal_group(t->group, 28); }
        return 0;
    }
    case 0x5424: return put_int(argument, t->termios.line); /* TIOCGETD */
    case 0x5423: /* TIOCSETD: only N_TTY */
        if ((error = get_int(argument, &value))) return error;
        return value ? -22 : 0;
    case 0x80045432: /* TIOCGDEV */
        return put_int(argument, (int)((t->index & 0xFF) | (136U << 8) | ((t->index & ~0xFFU) << 12)));
    case 0x5415: case 0x5416: case 0x5417: case 0x5418: return -25; /* modem lines: none on a pseudo-terminal */
    }
    if (!end->master) return -25;
    switch (command) { /* master-only (pty_unix98_ioctl) */
    case 0x80045430: return put_int(argument, (int)t->index); /* TIOCGPTN */
    case 0x40045431: /* TIOCSPTLCK */
        if ((error = get_int(argument, &value))) return error;
        t->locked = value != 0; return 0;
    case 0x80045439: return put_int(argument, t->locked); /* TIOCGPTLCK */
    case 0x5441: /* TIOCGPTPEER */
        if (argument & ~(3ULL | 0400ULL | 0x800ULL | 0x80000ULL | 0x101000ULL | 0x400ULL)) return -22;
        return open_slave(t, argument);
    case 0x5420: /* TIOCPKT */
        if ((error = get_int(argument, &value))) return error;
        if (value) { if (!t->packet) { t->packet = true; t->packet_status = 0; } } else t->packet = false;
        return 0;
    case 0x80045438: return put_int(argument, t->packet); /* TIOCGPKT */
    case 0x40045436: /* TIOCSIG */
        if (argument != 2 && argument != 3 && argument != 20) return -22;
        if (t->group) task_signal_group(t->group, (unsigned)argument);
        return 0;
    case 0x5437: /* TIOCVHANGUP */
        if (!capable_admin()) return -1;
        if (t->session) { task_signal_process(t->session, 1); task_signal_process(t->session, 18); task_terminal_forget(t, t->session); }
        t->session = t->group = 0; t->hung_up = true; notify(t); return 0;
    }
    return -25;
}
static void terminal_stat(void* object, struct linux_stat* stat) {
    struct terminal_end* end = object;
    pty_device_stat(end->master ? DEVICE_PTMX : DEVICE_PTS + end->tty->index, stat);
}
/* /proc/pid/stat tty_nr and tpgid. */
uint32_t terminal_device(struct terminal* t, uint32_t* foreground) {
    *foreground = t && t->group ? task_visible_pid(t->group) : 0;
    return t ? (t->index & 0xFF) | (136U << 8) | ((t->index & ~0xFFU) << 12) : 0;
}
static const struct object_operations terminal_operations = {terminal_io, terminal_ioctl, ready, source, terminal_release, terminal_stat};
