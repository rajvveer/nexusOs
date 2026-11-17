/* Kernel-pointer hygiene check (tests/test_x64_kptr.py), run as root in the campaign:
 *   - /proc/kallsyms lists symbols; with kptr_restrict=1 (default) every address is zero even for a CAP_SYSLOG
 *     reader, and a known kernel symbol name is present;
 *   - after writing kptr_restrict=0 the addresses become non-zero (the reader has CAP_SYSLOG);
 *   - /proc/sys/kernel/{kptr_restrict,dmesg_restrict} round-trip their written values. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static long slurp(const char* path, char* buf, long cap) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    long total = 0, n;
    while (total < cap - 1 && (n = read(fd, buf + total, cap - 1 - total)) > 0) total += n;
    close(fd); buf[total > 0 ? total : 0] = 0; return total;
}
static int writ(const char* path, const char* value) {
    int fd = open(path, O_WRONLY); if (fd < 0) return -1;
    long n = write(fd, value, strlen(value)); close(fd); return n == (long)strlen(value) ? 0 : -1;
}
/* Scan the 16-hex address field at the start of each COMPLETE line (a truncated final line from the capped read is
 * ignored). Returns whether every / any address is zero / non-zero. */
static int all_addresses_zero(const char* buf) {
    for (const char* p = buf; *p;) {
        const char* nl = strchr(p, '\n'); if (!nl) break;   /* truncated final line */
        if (nl - p >= 16) for (int i = 0; i < 16; i++) if (p[i] != '0') return 0;
        p = nl + 1;
    }
    return 1;
}
static int any_address_nonzero(const char* buf) {
    for (const char* p = buf; *p;) {
        const char* nl = strchr(p, '\n'); if (!nl) break;
        if (nl - p >= 16) for (int i = 0; i < 16; i++) if (p[i] != '0') return 1;
        p = nl + 1;
    }
    return 0;
}

int main(void) {
    static char buf[65536];
    if (slurp("/proc/kallsyms", buf, sizeof(buf)) <= 0) return 2;
    if (!strstr(buf, " native_dispatch\n") && !strstr(buf, " native_dispatch")) return 3;  /* a known kernel symbol */
    if (!all_addresses_zero(buf)) return 4;                      /* kptr_restrict=1 default: addresses hidden */
    if (writ("/proc/sys/kernel/kptr_restrict", "0\n")) return 5; /* root reveals */
    if (slurp("/proc/sys/kernel/kptr_restrict", buf, sizeof(buf)) <= 0 || buf[0] != '0') return 6;
    if (slurp("/proc/kallsyms", buf, sizeof(buf)) <= 0) return 7;
    if (!any_address_nonzero(buf)) return 8;                     /* now addresses are shown */
    if (writ("/proc/sys/kernel/kptr_restrict", "1\n")) return 9;
    if (writ("/proc/sys/kernel/dmesg_restrict", "0\n")) return 10;
    if (slurp("/proc/sys/kernel/dmesg_restrict", buf, sizeof(buf)) <= 0 || buf[0] != '0') return 11;
    if (writ("/proc/sys/kernel/dmesg_restrict", "1\n")) return 12;
    const char ok[] = "[64] KPTR USER PASS: kallsyms gating + writable kptr_restrict/dmesg_restrict\n";
    write(1, ok, sizeof(ok) - 1);
    return 0;
}
