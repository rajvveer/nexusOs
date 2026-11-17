/* /proc/PID/fdinfo/N beyond pos/flags/mnt_id/ino (kabi, wave 2), in Linux's format, which CRIU, lsof and systemd
 * parse: eventfd (eventfd-count, eventfd-id, eventfd-semaphore), epoll (one "tfd:" line per watch), and through each
 * object's operations table timerfd (posix_timers.c), signalfd (signalfd.c), pidfd (pidfd.c) and inotify
 * (inotify.c). kfmt() is the small formatter they share (also sysvipc.c's /proc/sysvipc tables). */
#include "native.h"
#include "../../kernel/string.h"

/* Appends to out[capacity] at `used` (always NUL-terminated, truncated at capacity), returns the new length.
 * Conversions: %d %u %x %o %s %c %%, flags 0 and -, a width, length modifiers l / ll (all integers are 64-bit). */
unsigned kfmt(char* out, unsigned capacity, unsigned used, const char* format, ...) {
    __builtin_va_list args; __builtin_va_start(args, format);
    if (!capacity) { __builtin_va_end(args); return 0; }
    if (used >= capacity) used = capacity - 1;
    for (const char* p = format; *p; p++) {
        if (*p != '%') { if (used + 1 < capacity) out[used++] = *p; continue; }
        p++;
        bool zero = false, left = false; unsigned width = 0;
        for (;; p++) { if (*p == '0') zero = true; else if (*p == '-') left = true; else break; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (unsigned)(*p++ - '0');
        bool wide = false;
        while (*p == 'l') { wide = true; p++; }
        char digits[66]; unsigned n = 0; const char* text = digits; bool negative = false;
        switch (*p) {
        case 'd': {
            int64_t v = wide ? __builtin_va_arg(args, int64_t) : __builtin_va_arg(args, int);
            uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v; negative = v < 0;
            do { digits[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            break;
        }
        case 'u': case 'x': case 'o': {
            uint64_t u = wide ? __builtin_va_arg(args, uint64_t) : __builtin_va_arg(args, unsigned);
            unsigned base = *p == 'u' ? 10 : *p == 'x' ? 16 : 8;
            do { digits[n++] = "0123456789abcdef"[u % base]; u /= base; } while (u);
            break;
        }
        case 's': text = __builtin_va_arg(args, const char*); if (!text) text = "(null)"; n = strlen(text); break;
        case 'c': digits[0] = (char)__builtin_va_arg(args, int); n = 1; break;
        case '%': digits[0] = '%'; n = 1; break;
        default: __builtin_va_end(args); out[used] = 0; return used;
        }
        if (*p != 's' && *p != 'c' && *p != '%') {   /* the digits were produced backwards */
            for (unsigned i = 0; i < n / 2; i++) { char t = digits[i]; digits[i] = digits[n - 1 - i]; digits[n - 1 - i] = t; }
        }
        unsigned length = n + (negative ? 1 : 0), pad = width > length ? width - length : 0;
        if (!left && !zero) while (pad && used + 1 < capacity) { out[used++] = ' '; pad--; }
        if (negative && used + 1 < capacity) out[used++] = '-';
        if (!left && zero) while (pad && used + 1 < capacity) { out[used++] = '0'; pad--; }
        for (unsigned i = 0; i < n && used + 1 < capacity; i++) out[used++] = text[i];
        while (left && pad && used + 1 < capacity) { out[used++] = ' '; pad--; }
    }
    __builtin_va_end(args);
    out[used] = 0; return used;
}
/* The kernel's dev_t (MKDEV: major << 20 | minor) of a userspace st_dev, as Linux prints sdev. */
uint32_t fdinfo_sdev(uint64_t dev) {
    uint64_t major = ((dev >> 8) & 0xFFF) | ((dev >> 32) & ~0xFFFULL), minor = (dev & 0xFF) | ((dev >> 12) & ~0xFFULL);
    return (uint32_t)(major << 20 | (minor & 0xFFFFF));
}

uint64_t counter_fdinfo(struct event_counter* counter, bool* semaphore, int* id);   /* event.c */
bool event_poll_watch(struct event_poll* poll, unsigned index, int* fd, uint32_t* events, uint64_t* data, struct open_file** description);
uint64_t fd_description_position(struct open_file* description);                   /* kernel.c */

unsigned fdinfo_eventfd(struct event_counter* counter, char* out, unsigned capacity) {   /* Linux eventfd_show_fdinfo */
    bool semaphore; int id; uint64_t value = counter_fdinfo(counter, &semaphore, &id);
    return kfmt(out, capacity, 0, "eventfd-count: %16llx\neventfd-id: %d\neventfd-semaphore: %d\n", value, id, semaphore ? 1 : 0);
}
unsigned fdinfo_epoll(struct event_poll* poll, char* out, unsigned capacity) {   /* Linux ep_show_fdinfo */
    unsigned used = 0; int fd; uint32_t events; uint64_t data; struct open_file* target;
    for (unsigned i = 0; event_poll_watch(poll, i, &fd, &events, &data, &target); i++) {
        struct linux_stat st; memset(&st, 0, sizeof(st));
        if (target) fd_description_stat(target, &st);
        used = kfmt(out, capacity, used, "tfd: %8d events: %8x data: %16llx  pos:%lld ino:%lx sdev:%x\n", fd, events, data,
                    (int64_t)(target ? fd_description_position(target) : 0), st.ino, fdinfo_sdev(st.dev));
    }
    return used;
}
