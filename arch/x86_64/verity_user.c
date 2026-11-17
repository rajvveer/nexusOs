/* /sbin/init on a dm-verity root (tests/test_x64_verity.py, static): the kernel made /dev/mapper/root from roothash= and
 * systemd.verity_root_data=/systemd.verity_root_hash= (dm_verity.inc verity_boot). Mode from the command line
 * (veritytest=...):
 *   root     / is /dev/mapper/root, read-only (EROFS), the mapping is a verity table (CRYPT-VERITY- uuid), the canary
 *            file reads back intact.
 *   eio      the host flipped a byte in canary block 3: reading it fails with EIO, block 48 still reads.
 *   restart  restart_on_corruption: reading the canary resets the machine ("NOT RESTARTED" if it returns).
 * "[64] VERITY TEST ALL PASSED" and power-off. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(1, "[64] VERITY TEST FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); sync(); reboot(RB_POWER_OFF); _exit(99); } } while (0)
#define PASS(...) do { printf("[64] VERITY TEST PASS " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static int slurp(const char* path, char* text, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, text, size - 1); close(fd); if (n < 0) return -1; text[n] = 0; return (int)n;
}
static uint8_t canary_byte(unsigned i) { uint32_t x = i * 2654435761u + 12345; x ^= x >> 13; return (uint8_t)(x ^ (x >> 7)); }
int main(void) {
    char cmdline[1024], mode[32] = "", text[8192];
    CHECK(slurp("/proc/cmdline", cmdline, sizeof(cmdline)) > 0);
    const char* m = strstr(cmdline, "veritytest="); CHECK(m);
    sscanf(m + 11, "%31[a-z]", mode);
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0);
    CHECK(strstr(text, "/dev/mapper/root / "));
    char* line = strstr(text, "/dev/mapper/root / "); CHECK(strstr(line, " ro,") || strstr(line, " ro "));
    errno = 0; CHECK(open("/verity-write-test", O_WRONLY | O_CREAT | O_CLOEXEC, 0644) < 0 && errno == EROFS);
    char name[64]; CHECK(slurp("/sys/block/dm-0/dm/name", name, sizeof(name)) > 0 && !strcmp(name, "root\n"));
    CHECK(slurp("/sys/block/dm-0/dm/uuid", name, sizeof(name)) > 0 && !strncmp(name, "CRYPT-VERITY-", 13));
    CHECK(slurp("/sys/block/dm-0/ro", name, sizeof(name)) > 0 && name[0] == '1');
    PASS("%s: / is /dev/mapper/root (verity), read-only, writes EROFS", mode);
    static uint8_t data[262144];
    int fd = open("/nexus/verity-canary.bin", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    if (!strcmp(mode, "root")) {
        CHECK(read(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
        for (unsigned i = 0; i < sizeof(data); i++) CHECK(data[i] == canary_byte(i));
        PASS("the canary file reads back intact through the hash tree");
    } else if (!strcmp(mode, "eio")) {
        CHECK(pread(fd, data, 4096, 48 * 4096) == 4096 && data[9] == canary_byte(48 * 4096 + 9));   /* far from the bad block */
        errno = 0; CHECK(pread(fd, data, 4096, 3 * 4096) < 0 && errno == EIO);   /* the corrupted block */
        PASS("the corrupted canary block fails with EIO, a block elsewhere in the file reads");
    } else if (!strcmp(mode, "restart")) {
        printf("[64] VERITY TEST READING THE CORRUPTED BLOCK\n"); fflush(stdout);
        ssize_t n = pread(fd, data, 4096, 3 * 4096);
        printf("[64] VERITY TEST NOT RESTARTED (%zd, errno %d)\n", n, errno); fflush(stdout);
        CHECK(0);
    } else CHECK(!"unknown mode");
    close(fd);
    printf("[64] VERITY TEST ALL PASSED\n"); fflush(stdout);
    sync(); reboot(RB_POWER_OFF);
    return 99;
}
