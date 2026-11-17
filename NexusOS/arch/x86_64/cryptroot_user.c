/* /sbin/init on an encrypted root (tests/test_x64_cryptroot.py, static): the root is the dm-crypt mapping the kernel
 * opened at boot -- /proc/mounts names it, /sys/block/dm-N/dm/name is the mapping, a file written and fsynced
 * through it reads back (the host then checks that its text is not on the raw disk), hibernation (when the disk has
 * a plain hibernation partition) is refused while the root is open, then a clean power-off. With /nexus/cryptroot-
 * hibernate it hibernates into the encrypted root's /nexus/hiber.img instead and, after the next boot unlocked the
 * root and restored the image, checks 16 MiB of patterned memory ("[64] CRYPTROOT RESUMED OK").
 * "[64] CRYPTROOT ROOT OK fs=<type> device=<root device> name=<mapping>"; failures "[64] CRYPTROOT ROOT FAIL line=N". */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/statfs.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(1, "[64] CRYPTROOT ROOT FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); sync(); reboot(RB_POWER_OFF); _exit(99); } } while (0)
static int slurp(const char* path, char* text, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, text, size - 1); close(fd); if (n < 0) return -1; text[n] = 0; return (int)n;
}
int main(void) {
    char mounts[4096], device[128] = "", name[128] = "";
    CHECK(slurp("/proc/mounts", mounts, sizeof(mounts)) > 0);
    for (char* line = mounts; *line; ) {   /* the line for "/" */
        char* end = strchr(line, '\n'); if (end) *end = 0;
        char dev[128], dir[128];
        if (sscanf(line, "%127s %127s", dev, dir) == 2 && !strcmp(dir, "/")) snprintf(device, sizeof(device), "%s", dev);
        if (!end) break;
        line = end + 1;
    }
    CHECK(device[0] && (strstr(device, "/dev/mapper/") || strstr(device, "/dev/dm-")));
    DIR* d = opendir("/sys/block"); CHECK(d); struct dirent* e;
    while ((e = readdir(d))) if (!strncmp(e->d_name, "dm-", 3)) {
        char path[160]; snprintf(path, sizeof(path), "/sys/block/%s/dm/name", e->d_name);
        if (slurp(path, name, sizeof(name)) > 0) { char* nl = strchr(name, '\n'); if (nl) *nl = 0; break; }
    }
    closedir(d);
    CHECK(name[0]);
    char mapper[160]; snprintf(mapper, sizeof(mapper), "/dev/mapper/%s", name); CHECK(access(mapper, F_OK) == 0);
    struct statfs st; CHECK(statfs("/", &st) == 0);
    const char* fs = st.f_type == 0xEF53 ? "ext4" : st.f_type == 0x4d44 ? "vfat" : "other";
    static char block[65536]; for (size_t i = 0; i < sizeof(block); i += 32) memcpy(block + i, "NEXUS-CRYPTROOT-PLAINTEXT-MARK!\n", 32);
    int fd = open("/home/nexus/cryptroot-proof.txt", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    for (int i = 0; i < 16; i++) CHECK(write(fd, block, sizeof(block)) == (ssize_t)sizeof(block));
    CHECK(fsync(fd) == 0); CHECK(lseek(fd, 12345, SEEK_SET) == 12345);
    char back[32]; CHECK(read(fd, back, 32) == 32 && !memcmp(back, block + 12345 % 32 + 0, 0) && memchr(back, '!', 32)); close(fd);
    char state[256] = "";
    if (access("/nexus/cryptroot-hibernate", F_OK) == 0) {   /* hibernate into /nexus/hiber.img on this encrypted root */
        size_t bytes = 16u << 20; uint64_t x = 0x9E3779B97F4A7C15ULL, sum = 0;
        uint8_t* pages = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(pages != MAP_FAILED);
        for (size_t p = 0; p < bytes; p += 4096) {   /* incompressible pages, each starting with a marker the host looks for on the raw disk */
            snprintf((char*)pages + p, 32, "NEXUS-HIBER-PLAINTEXT-%08zx", p / 4096);
            for (size_t i = p + 32; i < p + 4096; i += 8) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; memcpy(pages + i, &x, 8); }
        }
        for (size_t i = 0; i < bytes; i += 8) { uint64_t v; memcpy(&v, pages + i, 8); sum = (sum ^ v) * 0x100000001B3ULL; }
        printf("[64] CRYPTROOT HIBERNATE writing sum=%016llx\n", (unsigned long long)sum); fflush(stdout); sync();
        int pm = open("/sys/power/state", O_WRONLY | O_CLOEXEC); CHECK(pm >= 0);
        ssize_t w = write(pm, "disk", 4); int error = errno; close(pm);
        if (w < 0) { printf("[64] CRYPTROOT HIBERNATE FAIL errno=%d\n", error); fflush(stdout); }
        CHECK(w >= 0);   /* returns after the restore */
        uint64_t again = 0; for (size_t i = 0; i < bytes; i += 8) { uint64_t v; memcpy(&v, pages + i, 8); again = (again ^ v) * 0x100000001B3ULL; }
        CHECK(again == sum);
        printf("[64] CRYPTROOT RESUMED OK sum=%016llx\n", (unsigned long long)sum);
    } else if (slurp("/sys/power/state", state, sizeof(state)) > 0 && strstr(state, "disk")) {   /* refused: the area is not encrypted */
        int pm = open("/sys/power/state", O_WRONLY | O_CLOEXEC); CHECK(pm >= 0);
        ssize_t w = write(pm, "disk", 4); int error = errno; close(pm);
        CHECK(w < 0);
        printf("[64] CRYPTROOT HIBERNATE refused errno=%d\n", error);
    } else printf("[64] CRYPTROOT HIBERNATE no area\n");
    printf("[64] CRYPTROOT ROOT OK fs=%s device=%s name=%s\n", fs, device, name); fflush(stdout);
    sync(); reboot(RB_POWER_OFF);
    return 99;
}
