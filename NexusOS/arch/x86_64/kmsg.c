/* Kernel log buffer: /dev/kmsg (Linux record format) and syslog(2), so util-linux dmesg and the journal (nexusd) can
 * read the kernel's messages.
 *
 * Every byte the kernel prints through put() (kernel.c; not user writes to the console) is collected into lines; each
 * line becomes a record {sequence, microseconds since boot, facility/level, text} in a ring of KMSG_SLOTS slots
 * (oldest overwritten). Text before kmsg_init() (memory not ready) is kept in a static early buffer and replayed.
 * Locking: kmsg_lock is a leaf spinlock; put() may run in any context, so a CPU that already holds it (an interrupt
 * while committing) drops the byte instead of deadlocking, and put() never wakes tasks: kmsg_poll() (native_io_poll,
 * BSP device poll) wakes blocked readers and pollers once new records exist.
 * /dev/kmsg: each read returns one record "prio,seq,usec,flags;text\n" (EINVAL when the buffer is too small, EPIPE
 * once when the reader's next record was overwritten, EAGAIN/blocking at the end); writes add a record ("<N>" sets
 * facility/level; facility 0 is replaced by LOG_USER). Reading needs CAP_SYSLOG (Debian's dmesg_restrict=1); writing
 * is checked at open like Linux's root-only 0644 node. */
#include "native.h"
#include "../../kernel/string.h"

#define KMSG_SLOTS 4096U
#define KMSG_TEXT 232U
#define CAP_SYSLOG 34
#define CAP_SYS_ADMIN 21
struct kmsg_slot { uint64_t seq, usec; uint16_t length; uint8_t level, facility; char text[KMSG_TEXT]; };
_Static_assert(sizeof(struct kmsg_slot) == 256, "kmsg slot size");
static struct kmsg_slot* ring;
static uint64_t next_seq, clear_seq, syslog_seq;   /* next record number; SYSLOG_ACTION_CLEAR mark; syslog(2) READ position */
static volatile int kmsg_lock;
static char line[KMSG_TEXT]; static unsigned line_length;
static char early[16384]; static unsigned early_length;
static bool clock_ready, wake_pending;
static uint8_t kmsg_channel;   /* wait channel / event source of every reader */

static uint8_t guess_level(const char* text, unsigned length) {
    for (unsigned i = 0; i + 4 <= length; i++) {
        if (!memcmp(text + i, "FAIL", 4) || !memcmp(text + i, "FAUL", 4) || !memcmp(text + i, "pani", 4)) return 3;
        if (i + 6 <= length && !memcmp(text + i, "ENOSYS", 6)) return 5;
    }
    return 6;
}
/* kmsg_lock held */
static void commit(const char* text, unsigned length, uint8_t facility, uint8_t level) {
    struct kmsg_slot* s = &ring[next_seq % KMSG_SLOTS];
    if (length > KMSG_TEXT) length = KMSG_TEXT;
    s->seq = next_seq++; s->usec = clock_ready ? native_clock_ns() / 1000 : 0;
    s->length = (uint16_t)length; s->facility = facility; s->level = level; memcpy(s->text, text, length);
    wake_pending = true;
}
/* The kernel's process-tracing diagnostics (every exec, interpreter load, exit, missing /proc|/sys|/dev open, IPC
 * error, the periodic statistics and the long fault dumps) are KERN_DEBUG records. They stay on the serial console
 * (tests and developers read it), but reach /dev/kmsg (dmesg, the persistent journal: an apt upgrade wrote ~16k such
 * records) only with the boot option `kmsg.trace`. USER_FAULT, FAULT_IMAGE, CALLER_IMAGE and ENOSYS stay normal. */
bool boot_option(const char* word);
static bool trace_record(const char* text, unsigned length) {
    static const char* const tags[] = {"EXEC_ARGS ", "INTERPRETER ", "PROCESS_EXIT ", "USER_EXIT ", "EXIT_STACK ", "EXIT_IMAGE ",
        "OPEN_MISS ", "IPC_API_ERROR ", "IPC_ERROR_IMAGE ", "SYSCALL_ENOMEM ", "MEMSTAT ", "KSTAT ", "IOSTAT ", "FAULT_REGS ",
        "FAULT_AREA ", "FAULT_MEM ", "LAST_SYSCALL ", "SBXTRACE ", "mmap error=", "mmap source ", "mprotect error="};
    if (length < 6 || memcmp(text, "[64] ", 5)) return false;
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        unsigned n = (unsigned)strlen(tags[i]);
        if (length >= 5 + n && !memcmp(text + 5, tags[i], n)) return true;
    }
    return false;
}
static void commit_line(void) {
    if (!line_length) return;
    static int traced = -1;   /* boot option read once (the command line is fixed before the first record) */
    if (trace_record(line, line_length)) {
        if (traced < 0) traced = boot_option("kmsg.trace");
        if (!traced) { line_length = 0; return; }
        commit(line, line_length, 0, 7); line_length = 0; return;
    }
    commit(line, line_length, 0, guess_level(line, line_length)); line_length = 0;
}
void kmsg_putc(char c) {
    if (!ring) { if (early_length < sizeof(early)) early[early_length++] = c; return; }
    if (spin_held(&kmsg_lock)) return;
    spin_acquire(&kmsg_lock);
    if (c == '\n') commit_line();
    else if (c != '\r' && line_length < KMSG_TEXT) line[line_length++] = c;
    spin_release(&kmsg_lock);
}
void kmsg_init(void) {
    ring = (struct kmsg_slot*)arena_alloc((uint64_t)KMSG_SLOTS * sizeof(struct kmsg_slot));
    if (!ring) return;
    memset(ring, 0, (uint64_t)KMSG_SLOTS * sizeof(struct kmsg_slot));
    spin_acquire(&kmsg_lock);
    for (unsigned i = 0; i < early_length; i++) {
        if (early[i] == '\n') commit_line();
        else if (early[i] != '\r' && line_length < KMSG_TEXT) line[line_length++] = early[i];
    }
    early_length = 0;
    spin_release(&kmsg_lock);
}
void kmsg_clock_start(void) { clock_ready = true; }
void kmsg_poll(void) {
    if (!__atomic_load_n(&wake_pending, __ATOMIC_RELAXED)) return;
    wake_pending = false;
    task_wake_channel(&kmsg_channel); event_source_changed(&kmsg_channel, 1);
}
static bool may_read(void) {
    if (native_get_dmesg_restrict() == 0) return true;   /* dmesg_restrict=0: any process may read the ring (Linux) */
    struct credentials* c = credentials_current();
    return credentials_capable(c, NULL, CAP_SYSLOG) || credentials_capable(c, NULL, CAP_SYS_ADMIN);
}
/* Copy record seq (if still present) into *out; 0, -32 (overwritten) or -11 (not yet written). */
static int fetch(uint64_t seq, struct kmsg_slot* out, uint64_t* oldest) {
    spin_acquire(&kmsg_lock);
    uint64_t first = next_seq > KMSG_SLOTS ? next_seq - KMSG_SLOTS : 0; if (oldest) *oldest = first;
    int result = seq >= next_seq ? -11 : seq < first ? -32 : 0;
    if (!result) *out = ring[seq % KMSG_SLOTS];
    spin_release(&kmsg_lock);
    return result;
}
static unsigned format_record(const struct kmsg_slot* s, char* out, unsigned capacity) {
    char head[64]; unsigned n = 0, prio = (unsigned)s->facility * 8 + s->level;
    /* prio,seq,usec,flags; */
    char digits[24]; uint64_t values[3] = {prio, s->seq, s->usec};
    for (unsigned v = 0; v < 3; v++) {
        unsigned d = 0; uint64_t x = values[v]; do { digits[d++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (d) head[n++] = digits[--d];
        head[n++] = ',';
    }
    head[n - 1] = ','; head[n++] = '-'; head[n++] = ';';
    unsigned total = n;
    for (unsigned i = 0; i < s->length; i++) { unsigned char c = (unsigned char)s->text[i]; total += (c < 0x20 && c != '\t') || c >= 0x7f ? 4 : 1; }
    total += 1;
    if (total > capacity) return 0;
    memcpy(out, head, n);
    for (unsigned i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->text[i];
        if ((c < 0x20 && c != '\t') || c >= 0x7f) { out[n++] = '\\'; out[n++] = 'x'; out[n++] = "0123456789abcdef"[c >> 4]; out[n++] = "0123456789abcdef"[c & 15]; }
        else out[n++] = (char)c;
    }
    out[n++] = '\n'; return n;
}
struct kmsg_reader { uint64_t seq; bool can_read; };
static int64_t kmsg_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct kmsg_reader* r = object;
    if (write) {
        if (!total) return 0;
        char text[KMSG_TEXT + 8]; unsigned length = total < sizeof(text) ? (unsigned)total : sizeof(text);
        if (!iov_access(vectors, count, 0, length, false)) return -14;
        iov_copy(vectors, count, 0, (uint8_t*)text, length, true);
        unsigned start = 0, prio = 8 + 4;     /* LOG_USER | LOG_WARNING (default_message_loglevel) */
        if (length >= 3 && text[0] == '<') {
            unsigned value = 0, i = 1;
            while (i < length && i < 6 && text[i] >= '0' && text[i] <= '9') value = value * 10 + (unsigned)(text[i++] - '0');
            if (i > 1 && i < length && text[i] == '>' && value < 1024) { prio = value; start = i + 1; }
        }
        unsigned facility = prio >> 3; if (!facility) facility = 1;
        while (length > start && (text[length - 1] == '\n' || text[length - 1] == '\r')) length--;
        spin_acquire(&kmsg_lock); commit(text + start, length - start, (uint8_t)(facility > 255 ? 255 : facility), (uint8_t)(prio & 7)); spin_release(&kmsg_lock);
        kmsg_poll();
        return (int64_t)total;
    }
    if (!r->can_read) return -1;
    for (;;) {
        struct kmsg_slot s; uint64_t oldest; int error = fetch(r->seq, &s, &oldest);
        if (error == -32) { r->seq = oldest; return -32; }
        if (!error) {
            char out[KMSG_TEXT * 4 + 64]; unsigned n = format_record(&s, out, total < sizeof(out) ? (unsigned)total : sizeof(out));
            if (!n) return -22;
            if (!iov_access(vectors, count, 0, n, true)) return -14;
            iov_copy(vectors, count, 0, (uint8_t*)out, n, false);
            r->seq++; return n;
        }
        if (flags & 0x800) return -11;
        error = task_wait_channel(&kmsg_channel); if (error) return error;
    }
}
static int kmsg_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) { (void)object; (void)command; (void)argument; (void)flags; return -25; }
static uint32_t kmsg_ready(void* object) {
    struct kmsg_reader* r = object; uint32_t ready = 4;   /* writable */
    spin_acquire(&kmsg_lock); if (r->can_read && r->seq < next_seq) ready |= 1; spin_release(&kmsg_lock);
    return ready;
}
static void* kmsg_source(void* object) { (void)object; return &kmsg_channel; }
static void kmsg_release(void* object) { page_free((uint64_t)object); }
static void kmsg_stat(void* object, struct linux_stat* stat) {
    (void)object; memset(stat, 0, sizeof(*stat)); stat->dev = 9; stat->ino = 0x1000B; stat->nlink = 1; stat->mode = 020644;
    stat->rdev = 0x10B; stat->block_size = 4096; stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true);
}
static const struct object_operations kmsg_operations = {kmsg_io, kmsg_ioctl, kmsg_ready, kmsg_source, kmsg_release, kmsg_stat};
bool kmsg_path(const char* path) { return !strcmp(path, "/dev/kmsg"); }
int kmsg_open(uint64_t flags) {
    if (!ring) return -19;
    if (flags & 0x10000) return -20;
    bool reading = (flags & 3) != 1, writing = (flags & 3) != 0;
    struct credentials* c = credentials_current();
    if (writing && credentials_id(c, false, 1) != 0 && !credentials_capable(c, NULL, 1 /* CAP_DAC_OVERRIDE */)) return -13;
    if (reading && !may_read()) return -1;
    struct kmsg_reader* r = (void*)page_alloc(); if (!r) return -12;
    spin_acquire(&kmsg_lock); r->seq = next_seq > KMSG_SLOTS ? next_seq - KMSG_SLOTS : 0; if (r->seq < clear_seq) r->seq = clear_seq; spin_release(&kmsg_lock);
    r->can_read = reading;
    int fd = fd_create_object(&kmsg_operations, r, flags & (0x800 | 0x80000 | 3), "/dev/kmsg");
    if (fd < 0) page_free((uint64_t)r);
    return fd;
}
/* syslog(2): "<prio>[seconds.micro] text\n" lines */
static unsigned syslog_line(const struct kmsg_slot* s, char* out, unsigned capacity) {
    char head[48]; unsigned n = 0; uint64_t sec = s->usec / 1000000, micro = s->usec % 1000000;
    head[n++] = '<'; unsigned prio = (unsigned)s->facility * 8 + s->level; char d[24]; unsigned k = 0;
    do { d[k++] = (char)('0' + prio % 10); prio /= 10; } while (prio);
    while (k) head[n++] = d[--k];
    head[n++] = '>'; head[n++] = '[';
    k = 0; do { d[k++] = (char)('0' + sec % 10); sec /= 10; } while (sec);
    for (unsigned pad = k; pad < 5; pad++) head[n++] = ' ';
    while (k) head[n++] = d[--k];
    head[n++] = '.';
    for (int i = 5; i >= 0; i--) { d[i] = (char)('0' + micro % 10); micro /= 10; }
    for (int i = 0; i < 6; i++) head[n++] = d[i];
    head[n++] = ']'; head[n++] = ' ';
    if (n + s->length + 1 > capacity) return 0;
    memcpy(out, head, n); memcpy(out + n, s->text, s->length); n += s->length; out[n++] = '\n'; return n;
}
int64_t kmsg_syslog(uint64_t type, uint64_t buffer, uint64_t length) {
    if (type > 10) return -22;
    if (type != 10 && type > 1 && !may_read()) return -1;
    if (type == 10 && !may_read()) return -1;
    switch (type) {
    case 0: case 1: return 0;
    case 6: case 7: case 8: return 0;            /* console level: the console is the serial log, always on */
    case 10: return (int64_t)KMSG_SLOTS * KMSG_TEXT;
    case 5: spin_acquire(&kmsg_lock); clear_seq = next_seq; spin_release(&kmsg_lock); return 0;
    case 9: { spin_acquire(&kmsg_lock); uint64_t first = next_seq > KMSG_SLOTS ? next_seq - KMSG_SLOTS : 0, from = syslog_seq < first ? first : syslog_seq;
              uint64_t bytes = 0; for (uint64_t q = from; q < next_seq; q++) bytes += ring[q % KMSG_SLOTS].length + 20;
              spin_release(&kmsg_lock); return (int64_t)bytes; }
    }
    if ((int64_t)length < 0) return -22;
    if (!length) return 0;
    if (!buffer || !accessible(buffer, length, true)) return -14;
    if (!ring) return 0;
    uint64_t written = 0;
    if (type == 2) {   /* destructive read from the syslog position; blocks until something is there */
        for (;;) {
            struct kmsg_slot s; uint64_t oldest; int error = fetch(syslog_seq, &s, &oldest);
            if (error == -32) { syslog_seq = oldest; continue; }
            if (!error) break;
            if ((error = task_wait_channel(&kmsg_channel))) return error;
        }
        while (written < length) {
            struct kmsg_slot s; uint64_t oldest; int error = fetch(syslog_seq, &s, &oldest);
            if (error == -32) { syslog_seq = oldest; continue; }
            if (error) break;
            char out[KMSG_TEXT + 48]; unsigned n = syslog_line(&s, out, sizeof(out));
            if (!n || written + n > length) break;
            if (!accessible(buffer + written, n, true)) return written ? (int64_t)written : -14;
            if (copy_to_user((void*)(buffer + written), out, n)) return written ? (int64_t)written : -14; written += n; syslog_seq++;
        }
        return (int64_t)written;
    }
    /* 3 READ_ALL / 4 READ_CLEAR: the newest records that fit, oldest first */
    spin_acquire(&kmsg_lock);
    uint64_t first = next_seq > KMSG_SLOTS ? next_seq - KMSG_SLOTS : 0, end = next_seq;
    if (first < clear_seq) first = clear_seq;
    spin_release(&kmsg_lock);
    uint64_t start = end, bytes = 0;
    while (start > first) {
        struct kmsg_slot s; if (fetch(start - 1, &s, NULL)) break;
        uint64_t add = s.length + 22; if (bytes + add > length) break;
        bytes += add; start--;
    }
    for (uint64_t q = start; q < end; q++) {
        struct kmsg_slot s; if (fetch(q, &s, NULL)) continue;
        char out[KMSG_TEXT + 48]; unsigned n = syslog_line(&s, out, sizeof(out));
        if (!n || written + n > length) break;
        if (!accessible(buffer + written, n, true)) return written ? (int64_t)written : -14;
        if (copy_to_user((void*)(buffer + written), out, n)) return written ? (int64_t)written : -14; written += n;
    }
    if (type == 4) { spin_acquire(&kmsg_lock); clear_seq = end; spin_release(&kmsg_lock); }
    return (int64_t)written;
}
