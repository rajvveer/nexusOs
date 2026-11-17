/* securityfs check (tests/test_x64_lockdown.py): /sys/kernel/security/lockdown reads the active level in brackets,
 * root can raise it (write), and lowering it is refused (the one-way ratchet). Runs as uid 0 in the campaign. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char buf[128];
    int fd = open("/sys/kernel/security/lockdown", O_RDWR); if (fd < 0) return 2;
    long n = read(fd, buf, sizeof(buf) - 1); if (n <= 0) return 3; buf[n] = 0;
    if (!strchr(buf, '[') || !strchr(buf, ']')) return 4;                 /* active level in brackets */
    if (write(fd, "confidentiality", 15) != 15) return 5;                 /* raising (>= current) is allowed */
    if (lseek(fd, 0, SEEK_SET) != 0) return 6;
    n = read(fd, buf, sizeof(buf) - 1); if (n <= 0) return 7; buf[n] = 0;
    if (!strstr(buf, "[confidentiality]")) return 8;
    if (write(fd, "none", 4) != -1) return 9;                             /* lowering is refused (ratchet) */
    if (write(fd, "bogus", 5) != -1) return 10;                           /* an unknown level is EINVAL */
    close(fd);
    const char ok[] = "[64] SECFS USER PASS: securityfs lockdown read/write + ratchet\n";
    write(1, ok, sizeof(ok) - 1);
    return 0;
}
