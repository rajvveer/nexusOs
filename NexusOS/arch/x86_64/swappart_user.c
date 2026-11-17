/* Swap partitions end to end (tests/test_x64_swappart.py boots this as /sbin/init, as root, static): /etc/fstab named an
 * MBR 0x82 partition by UUID= with pri=10 and a GPT "Linux swap" partition by LABEL= with pri=5 (and a missing device
 * with nofail). Checks: both active from boot with their priorities (/proc/swaps type "partition", SwapTotal), memory
 * beyond RAM fills the higher-priority area first and every byte reads back, swapoff of a used partition brings its
 * pages back, swapon(2) with SWAP_FLAG_PREFER sets a new priority, EBUSY for an active area, EPERM-free root path, then
 * a clean power-off. "[64] SWAPPART ..." lines; failures "[64] SWAPPART FAIL line=N". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/swap.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(1, "[64] SWAPPART FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); sync(); reboot(RB_POWER_OFF); _exit(99); } } while (0)
#define LOG(...) dprintf(1, "[64] SWAPPART " __VA_ARGS__)
struct area { char path[64]; char type[16]; long size, used, prio; };
static int areas(struct area* out, int max) {
    char text[4096]; int fd = open("/proc/swaps", O_RDONLY); CHECK(fd >= 0);
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd); CHECK(n > 0); text[n] = 0;
    int count = 0; char* line = strchr(text, '\n');   /* skip the header */
    while (line && *++line && count < max) {
        struct area* a = &out[count];
        if (sscanf(line, "%63s %15s %ld %ld %ld", a->path, a->type, &a->size, &a->used, &a->prio) == 5) count++;
        line = strchr(line, '\n');
    }
    return count;
}
static struct area* find(struct area* list, int count, const char* path) {
    for (int i = 0; i < count; i++) if (!strcmp(list[i].path, path)) return &list[i];
    return NULL;
}
static long meminfo(const char* key) {
    char text[8192]; int fd = open("/proc/meminfo", O_RDONLY); CHECK(fd >= 0);
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd); CHECK(n > 0); text[n] = 0;
    char* at = strstr(text, key); CHECK(at); return strtol(at + strlen(key) + 1, NULL, 10);
}
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char a_dev[64], b_dev[64];
    int fd = open("/nexus/swappart.conf", O_RDONLY); CHECK(fd >= 0);
    char conf[160] = {0}; CHECK(read(fd, conf, sizeof(conf) - 1) > 0); close(fd);
    CHECK(sscanf(conf, "%63s %63s", a_dev, b_dev) == 2);   /* the two partitions' /dev names */
    struct area list[8]; int count = areas(list, 8);
    struct area *a = find(list, count, a_dev), *b = find(list, count, b_dev);
    CHECK(count == 2 && a && b);
    CHECK(!strcmp(a->type, "partition") && !strcmp(b->type, "partition") && a->prio == 10 && b->prio == 5);
    CHECK(meminfo("SwapTotal:") == a->size + b->size);
    LOG("BOOT %s pri %ld %ld KiB, %s pri %ld %ld KiB\n", a->path, a->prio, a->size, b->path, b->prio, b->size);
    long ram_kb = meminfo("MemAvailable:");
    size_t bytes = (size_t)(ram_kb + a->size + b->size / 3) * 1024;   /* beyond RAM: all of A, part of B */
    uint64_t* m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(m != MAP_FAILED);
    for (size_t i = 0; i < bytes / 8; i += 512) m[i] = i * 0x9E3779B97F4A7C15ULL + 7;
    count = areas(list, 8); a = find(list, count, a_dev); b = find(list, count, b_dev); CHECK(a && b);
    LOG("FILLED %zu MiB: %s used %ld/%ld KiB, %s used %ld/%ld KiB\n", bytes >> 20, a->path, a->used, a->size, b->path, b->used, b->size);
    CHECK(a->used > 0 && b->used > 0 && a->used >= a->size - a->size / 8);   /* the higher priority filled first */
    for (size_t i = 0; i < bytes / 8; i += 512) CHECK(m[i] == i * 0x9E3779B97F4A7C15ULL + 7);
    CHECK(swapon(a_dev, 0) == -1 && errno == EBUSY);
    size_t half = (bytes / 2) & ~(size_t)4095;
    CHECK(munmap((uint8_t*)m + half, bytes - half) == 0);   /* room to take A's pages back */
    CHECK(swapoff(a_dev) == 0);
    count = areas(list, 8); CHECK(count == 1 && find(list, count, b_dev) && !find(list, count, a_dev));
    for (size_t i = 0; i < half / 8; i += 512) CHECK(m[i] == i * 0x9E3779B97F4A7C15ULL + 7);
    CHECK(swapon(a_dev, SWAP_FLAG_PREFER | (20 << SWAP_FLAG_PRIO_SHIFT)) == 0);
    count = areas(list, 8); a = find(list, count, a_dev); CHECK(count == 2 && a && a->prio == 20);
    CHECK(swapoff(b_dev) == 0 && swapoff(a_dev) == 0);
    for (size_t i = 0; i < half / 8; i += 512) CHECK(m[i] == i * 0x9E3779B97F4A7C15ULL + 7);
    CHECK(areas(list, 8) == 0 && meminfo("SwapTotal:") == 0);
    LOG("ALL PASSED: fstab UUID=/LABEL= partitions with priorities, higher priority first, data intact, swapoff/swapon\n");
    munmap(m, half);
    return 0;   /* the kernel then checks that every page is accounted for and powers off */
}
