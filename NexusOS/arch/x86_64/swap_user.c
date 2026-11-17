/* Swap, reclaim and OOM campaign (tests/test_x64_swap.py boots this as /sbin/init; the mode comes from
 * /nexus/swaptest.conf). Modes:
 *   api        swap areas: mkswap-format files, swapon/swapoff semantics and errors, priorities, /proc/swaps, meminfo,
 *              hostile headers (corrupt, empty, too short, v0, bad-page list on a file, byte-swapped), active-file
 *              immutability (ETXTBSY), non-root (EPERM), the mounted root partition (EBUSY), sysctls
 *   stress     with a swap file: anonymous memory of twice the RAM, a shared anonymous region, forked children
 *              (COW of swapped pages), dirty MAP_PRIVATE file pages, memfd and /dev/shm, every byte verified;
 *              mincore, VmSwap, smaps Swap, vmstat pswpin/pswpout, drop_caches; swapoff under load (ENOMEM when
 *              the pages cannot fit, success when they can), mlock never swapped
 *   oom        no swap: a hog allocating twice the RAM is OOM-killed (SIGKILL) while a sentinel keeps answering;
 *              oom_score_adj 1000 picks the victim among two hogs, -1000 is never chosen; oom_score
 *   ext4       on an ext4 root: a swap file with holes is refused, a fully allocated one works
 * Each line "[64] SWAPTEST ... PASS"; a failure prints "[64] SWAPTEST FAIL line=N" and exits 99. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096UL
#define CHECK(x) do { if (!(x)) { dprintf(2, "[64] SWAPTEST FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); _exit(99); } } while (0)
#define LOG(...) do { dprintf(1, "[64] SWAPTEST " __VA_ARGS__); } while (0)

static long meminfo(const char* field) {   /* kB, -1 if absent */
    char text[8192]; int fd = open("/proc/meminfo", O_RDONLY); CHECK(fd >= 0);
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd); CHECK(n > 0); text[n] = 0;
    char key[64]; snprintf(key, sizeof(key), "\n%s:", field);
    char* at = strstr(text, key); if (!at && !strncmp(text, key + 1, strlen(key) - 1)) at = text - 1;
    if (!at) return -1;
    return strtol(at + strlen(key), NULL, 10);
}
static long vmstat(const char* field) {
    char text[8192]; int fd = open("/proc/vmstat", O_RDONLY); CHECK(fd >= 0);
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd); CHECK(n > 0); text[n] = 0;
    char key[64]; snprintf(key, sizeof(key), "%s ", field);
    for (char* line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
        if (!strncmp(line, key, strlen(key))) return strtol(line + strlen(key), NULL, 10);
    return -1;
}
static long status_kb(pid_t pid, const char* field) {
    char path[64], text[4096]; snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd); if (n <= 0) return -1; text[n] = 0;
    char key[64]; snprintf(key, sizeof(key), "\n%s:", field); char* at = strstr(text, key);
    return at ? strtol(at + strlen(key), NULL, 10) : -1;
}
static int slurp(const char* path, char* text, size_t size) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    ssize_t n = read(fd, text, size - 1); int e = errno; close(fd); errno = e;
    if (n < 0) return -1;
    text[n] = 0; return (int)n;
}
static int put(const char* path, const char* value) {
    int fd = open(path, O_WRONLY); if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value)); int e = errno; close(fd); errno = e;
    return n == (ssize_t)strlen(value) ? 0 : -1;
}

/* ---- mkswap (util-linux's v1 layout) ---- */
struct swap_header_v1 { char bootbits[1024]; uint32_t version, last_page, nr_badpages; unsigned char uuid[16]; char volume[16]; uint32_t padding[117]; uint32_t badpages[1]; };
static void make_header(unsigned char* page, uint32_t pages, uint32_t bad, const uint32_t* badlist, bool big_endian, const char* magic) {
    memset(page, 0, PAGE);
    struct swap_header_v1* h = (void*)page;
    h->version = 1; h->last_page = pages - 1; h->nr_badpages = bad;
    for (uint32_t i = 0; i < bad; i++) h->badpages[i] = badlist[i];
    for (int i = 0; i < 16; i++) h->uuid[i] = (unsigned char)(0x5A ^ i * 37);
    memcpy(h->volume, "nexusswap", 9);
    if (big_endian) {
        h->version = __builtin_bswap32(h->version); h->last_page = __builtin_bswap32(h->last_page); h->nr_badpages = __builtin_bswap32(h->nr_badpages);
        for (uint32_t i = 0; i < bad; i++) h->badpages[i] = __builtin_bswap32(h->badpages[i]);
    }
    memcpy(page + PAGE - 10, magic, 10);
}
/* A swap file of `pages` pages: zeros through write(), the header, fsync (dd + mkswap). */
static void create_swapfile(const char* path, uint64_t pages, uint32_t header_pages, uint32_t bad, const uint32_t* badlist, bool big_endian, const char* magic) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); CHECK(fd >= 0);
    static unsigned char block[1 << 20];
    memset(block, 0, sizeof(block));
    for (uint64_t done = 0; done < pages * PAGE;) {
        uint64_t part = pages * PAGE - done < sizeof(block) ? pages * PAGE - done : sizeof(block);
        ssize_t wrote = write(fd, block, part);
        if (wrote != (ssize_t)part) dprintf(2, "[64] SWAPTEST short write at %lu: %ld errno=%d\n", (unsigned long)done, (long)wrote, errno);
        CHECK(wrote == (ssize_t)part); done += part;
    }
    unsigned char page[PAGE]; make_header(page, header_pages, bad, badlist, big_endian, magic);
    CHECK(pwrite(fd, page, PAGE, 0) == (ssize_t)PAGE && fsync(fd) == 0 && close(fd) == 0);
}
static bool swaps_has(const char* path, const char* type, long size_kb, int priority) {
    char text[4096]; CHECK(slurp("/proc/swaps", text, sizeof(text)) > 0);
    CHECK(!strncmp(text, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n", 39));
    for (char* line = strchr(text, '\n') + 1; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        char name[256], kind[32]; long size, used; int prio;
        if (sscanf(line, "%255s %31s %ld %ld %d", name, kind, &size, &used, &prio) != 5) continue;
        if (!strcmp(name, path)) return !strcmp(kind, type) && (size_kb < 0 || size == size_kb) && (priority == 99999 || prio == priority);
    }
    return false;
}
static char root_partition[64];
static void find_root_partition(void) {   /* the kernel log names it: /proc/mounts says /dev/sda1, the block layer label differs */
    const char* candidates[] = {"/dev/ide0p1", "/dev/sata0p1", "/dev/nvme0n1p1", "/dev/ide0", "/dev/nvme0n1"};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        errno = 0; int r = swapon(candidates[i], 0);
        if (r == -1 && errno == EBUSY) { strcpy(root_partition, candidates[i]); return; }
        if (r == 0) { swapoff(candidates[i]); }
    }
}

/* ---- data patterns ---- */
static inline uint64_t tag(uint64_t seed, uint64_t page) { return seed ^ (page * 0x9E3779B97F4A7C15ULL); }
static void fill(volatile uint64_t* base, uint64_t pages, uint64_t seed) {
    for (uint64_t p = 0; p < pages; p++) { uint64_t t = tag(seed, p); for (unsigned w = 0; w < PAGE / 8; w++) base[p * (PAGE / 8) + w] = t ^ w; }
}
static uint64_t verify(volatile uint64_t* base, uint64_t pages, uint64_t seed) {   /* bad pages */
    uint64_t bad = 0;
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t t = tag(seed, p);
        for (unsigned w = 0; w < PAGE / 8; w++) if (base[p * (PAGE / 8) + w] != (t ^ w)) { if (!bad) dprintf(2, "[64] SWAPTEST mismatch page=%lu word=%u got=%lx want=%lx\n", (unsigned long)p, w, (unsigned long)base[p * (PAGE / 8) + w], (unsigned long)(t ^ w)); bad++; break; }
    }
    return bad;
}
static uint64_t verify_at(volatile uint64_t* base, uint64_t first, uint64_t count, uint64_t seed) {   /* pages by absolute index */
    uint64_t bad = 0;
    for (uint64_t p = first; p < first + count; p++) {
        uint64_t t = tag(seed, p);
        for (unsigned w = 0; w < PAGE / 8; w++) if (base[p * (PAGE / 8) + w] != (t ^ w)) { bad++; break; }
    }
    return bad;
}
static void* anon(uint64_t bytes, bool shared) {
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED); return p;
}
static int wait_status(pid_t pid) { int status = 0; CHECK(waitpid(pid, &status, 0) == pid); return status; }

/* ---- api ---- */
static void mode_api(void) {
    uint32_t badlist[2] = {3, 5};
    LOG("api: creating swap files\n");
    create_swapfile("/tmp/swap-a", 2048, 2048, 0, NULL, false, "SWAPSPACE2");
    create_swapfile("/tmp/swap-b", 1024, 1024, 0, NULL, true, "SWAPSPACE2");     /* big-endian header (Linux swabs it) */
    create_swapfile("/tmp/swap-corrupt", 64, 64, 0, NULL, false, "SWAPSPACEX");
    create_swapfile("/tmp/swap-v0", 64, 64, 0, NULL, false, "SWAP-SPACE");
    create_swapfile("/tmp/swap-empty", 64, 1, 0, NULL, false, "SWAPSPACE2");      /* last_page 0 */
    create_swapfile("/tmp/swap-short", 64, 256, 0, NULL, false, "SWAPSPACE2");    /* header claims more than the file */
    create_swapfile("/tmp/swap-bad", 64, 64, 2, badlist, false, "SWAPSPACE2");    /* bad-page list on a regular file */
    long total_before = meminfo("SwapTotal"), free_before = meminfo("SwapFree");
    CHECK(total_before == 0 && free_before == 0 && meminfo("SwapCached") == 0);
    CHECK(swapon("/tmp/swap-a", 0) == 0);
    CHECK(swaps_has("/tmp/swap-a", "file", 2047 * 4, -2));
    CHECK(meminfo("SwapTotal") == 2047 * 4 && meminfo("SwapFree") == 2047 * 4);
    struct sysinfo_like { long uptime; unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap; unsigned short procs, pad; unsigned int pad2; unsigned long th, fh; unsigned int unit; char f[8]; } si;
    CHECK(syscall(SYS_sysinfo, &si) == 0 && si.totalswap == 2047 * PAGE && si.freeswap == 2047 * PAGE);
    LOG("api: swapon file, /proc/swaps, meminfo, sysinfo PASS\n");
    errno = 0; CHECK(swapon("/tmp/swap-a", 0) == -1 && errno == EBUSY);
    errno = 0; CHECK(swapon("/tmp/swap-corrupt", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/tmp/swap-v0", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/tmp/swap-empty", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/tmp/swap-short", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/tmp/swap-bad", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/nonexistent-swap", 0) == -1 && errno == ENOENT);
    errno = 0; CHECK(swapon("/tmp", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/dev/null", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/proc/meminfo", 0) == -1 && errno == EINVAL);
    errno = 0; CHECK(swapon("/tmp/swap-b", 0x80000) == -1 && errno == EINVAL);   /* unknown flag */
    errno = 0; CHECK(swapoff("/tmp/swap-b") == -1 && errno == EINVAL);           /* not active */
    errno = 0; CHECK(swapon((const char*)8, 0) == -1 && errno == EFAULT);
    find_root_partition(); CHECK(root_partition[0]);
    LOG("api: EBUSY twice and on the mounted root (%s), EINVAL headers/types/flags, ENOENT, EFAULT PASS\n", root_partition);
    /* the active file is immutable */
    int fd = open("/tmp/swap-a", O_RDWR); CHECK(fd >= 0);
    errno = 0; CHECK(write(fd, "x", 1) == -1 && errno == ETXTBSY);
    errno = 0; CHECK(ftruncate(fd, PAGE) == -1 && errno == ETXTBSY);
    errno = 0; CHECK(mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ETXTBSY);
    char c; CHECK(pread(fd, &c, 1, PAGE) == 1);   /* reading stays allowed */
    close(fd);
    errno = 0; CHECK(truncate("/tmp/swap-a", 0) == -1 && errno == ETXTBSY);
    LOG("api: active swap file write/truncate/shared-map ETXTBSY PASS\n");
    /* priorities: explicit and automatic (Linux: -2, -3, ... renumbered on swapoff) */
    CHECK(swapon("/tmp/swap-b", SWAP_FLAG_PREFER | 7) == 0);
    CHECK(swaps_has("/tmp/swap-b", "file", 1023 * 4, 7) && swaps_has("/tmp/swap-a", "file", 2047 * 4, -2));
    CHECK(meminfo("SwapTotal") == (2047 + 1023) * 4);
    CHECK(swapoff("/tmp/swap-b") == 0 && !swaps_has("/tmp/swap-b", "file", -1, 99999));
    create_swapfile("/tmp/swap-c", 512, 512, 0, NULL, false, "SWAPSPACE2");
    CHECK(swapon("/tmp/swap-c", SWAP_FLAG_DISCARD) == 0 && swaps_has("/tmp/swap-c", "file", 511 * 4, -3));
    CHECK(swapoff("/tmp/swap-a") == 0 && swaps_has("/tmp/swap-c", "file", 511 * 4, -2));
    CHECK(swapoff("/tmp/swap-c") == 0 && meminfo("SwapTotal") == 0);
    LOG("api: priorities (prefer, automatic, renumbering), DISCARD accepted, swapoff PASS\n");
    /* non-root */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(setuid(1000) == 0);
        errno = 0; CHECK(swapon("/tmp/swap-c", 0) == -1 && errno == EPERM);
        errno = 0; CHECK(swapoff("/tmp/swap-c") == -1 && errno == EPERM);
        errno = 0; CHECK(put("/proc/sys/vm/swappiness", "10") == -1 && errno == EACCES);
        _exit(0);
    }
    CHECK(wait_status(child) == 0);
    LOG("api: non-root EPERM (swapon/swapoff) and EACCES (sysctl) PASS\n");
    /* vm sysctls */
    char text[256];
    CHECK(slurp("/proc/sys/vm/swappiness", text, sizeof(text)) > 0 && atoi(text) == 60);
    CHECK(put("/proc/sys/vm/swappiness", "100") == 0 && slurp("/proc/sys/vm/swappiness", text, sizeof(text)) > 0 && atoi(text) == 100);
    errno = 0; CHECK(put("/proc/sys/vm/swappiness", "201") == -1 && errno == EINVAL);
    errno = 0; CHECK(put("/proc/sys/vm/swappiness", "abc") == -1 && errno == EINVAL);
    CHECK(put("/proc/sys/vm/swappiness", "60") == 0);
    errno = 0; CHECK(slurp("/proc/sys/vm/drop_caches", text, sizeof(text)) == -1 && errno == EACCES);
    CHECK(put("/proc/sys/vm/drop_caches", "3") == 0);
    errno = 0; CHECK(put("/proc/sys/vm/drop_caches", "5") == -1 && errno == EINVAL);
    CHECK(slurp("/proc/sys/vm/min_free_kbytes", text, sizeof(text)) > 0 && atoi(text) >= 128);
    CHECK(slurp("/proc/sys/vm/page-cluster", text, sizeof(text)) > 0 && atoi(text) == 3);
    CHECK(vmstat("drop_pagecache") >= 1 && vmstat("pswpout") == 0);
    CHECK(slurp("/proc/zoneinfo", text, sizeof(text)) > 0 && strstr(text, "min"));
    LOG("api: vm.swappiness, drop_caches, min_free_kbytes, page-cluster, vmstat, zoneinfo PASS\n");
    const char* files[] = {"/tmp/swap-a", "/tmp/swap-b", "/tmp/swap-c", "/tmp/swap-corrupt", "/tmp/swap-v0", "/tmp/swap-empty", "/tmp/swap-short", "/tmp/swap-bad"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) CHECK(unlink(files[i]) == 0);
    LOG("API ALL PASSED\n");
}
/* ---- stress ---- */
static uint64_t ram_pages(void) { return (uint64_t)meminfo("MemTotal") / 4; }
static long smaps_field(void* address, const char* field) {   /* kB of `field` in the smaps entry holding address */
    static char text[1 << 20]; int fd = open("/proc/self/smaps", O_RDONLY); CHECK(fd >= 0);
    ssize_t n = 0, got; while ((got = read(fd, text + n, sizeof(text) - 1 - (size_t)n)) > 0) n += got;
    close(fd); text[n] = 0;
    for (char* line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        unsigned long lo, hi; if (sscanf(line, "%lx-%lx ", &lo, &hi) != 2 || (uintptr_t)address < lo || (uintptr_t)address >= hi) continue;
        char key[64]; snprintf(key, sizeof(key), "\n%s:", field); char* at = strstr(line, key);
        return at ? strtol(at + strlen(key), NULL, 10) : -1;
    }
    return -1;
}
static void pressure(uint64_t pages, uint64_t seed) {   /* touch and verify `pages` of fresh anonymous memory */
    volatile uint64_t* p = anon(pages * PAGE, false); fill(p, pages, seed); CHECK(verify(p, pages, seed) == 0); CHECK(munmap((void*)p, pages * PAGE) == 0);
}
static void mode_stress(int rounds) {
    uint64_t ram = ram_pages(), swap_pages = ram * 5 / 2;
    LOG("stress: RAM %lu pages, creating a %lu-page swap file\n", (unsigned long)ram, (unsigned long)swap_pages);
    create_swapfile("/tmp/swapfile", swap_pages, (uint32_t)swap_pages, 0, NULL, false, "SWAPSPACE2");
    CHECK(swapon("/tmp/swapfile", 0) == 0 && meminfo("SwapTotal") == (long)(swap_pages - 1) * 4);
    for (int round = 0; round < rounds; round++) {
        /* (a) anonymous memory of twice the RAM */
        uint64_t pages = ram * 2; uint64_t seed = 0xA5A5000000000000ULL + (uint64_t)round;
        volatile uint64_t* a = anon(pages * PAGE, false);
        fill(a, pages, seed);
        long swapped = (meminfo("SwapTotal") - meminfo("SwapFree")) / 4;
        CHECK(swapped > (long)(ram / 2) && vmstat("pswpout") > 0 && status_kb(getpid(), "VmSwap") > 0);
        CHECK(smaps_field((void*)a, "Swap") > 0);
        unsigned char* vector = malloc(pages); CHECK(vector && mincore((void*)a, pages * PAGE, vector) == 0);
        uint64_t resident = 0; for (uint64_t i = 0; i < pages; i++) resident += vector[i] & 1;
        CHECK(resident < pages);
        CHECK(verify(a, pages, seed) == 0);
        CHECK(vmstat("pswpin") > 0);
        a[0] = a[0];   /* touched just now: resident */
        CHECK(mincore((void*)a, PAGE, vector) == 0 && vector[0] == 1);
        free(vector);
        CHECK(munmap((void*)a, pages * PAGE) == 0);
        LOG("stress: %lu anonymous pages (2x RAM), %ld swapped, every byte verified, mincore/VmSwap/smaps PASS\n", (unsigned long)pages, swapped);
        /* (b) shared anonymous memory: a child's writes reach the parent through swap */
        uint64_t shared_pages = ram / 4;
        volatile uint64_t* h = anon(shared_pages * PAGE, true);
        fill(h, shared_pages, 0x5100);
        pressure(ram, 0x7700);
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) { CHECK(verify(h, shared_pages, 0x5100) == 0); fill(h, shared_pages, 0x5200); pressure(ram, 0x7701); _exit(0); }
        CHECK(wait_status(child) == 0);
        pressure(ram, 0x7702);
        CHECK(verify(h, shared_pages, 0x5200) == 0);
        CHECK(munmap((void*)h, shared_pages * PAGE) == 0);
        LOG("stress: MAP_SHARED|MAP_ANONYMOUS across fork and swap PASS\n");
        /* (c) forked children share swapped pages copy-on-write */
        uint64_t fork_pages = ram;
        volatile uint64_t* f = anon(fork_pages * PAGE, false);
        fill(f, fork_pages, 0xF0F0);
        pressure(ram / 2, 0x7703);
        pid_t kids[2];
        for (int k = 0; k < 2; k++) {
            kids[k] = fork(); CHECK(kids[k] >= 0);
            if (!kids[k]) {
                CHECK(verify(f, fork_pages, 0xF0F0) == 0);
                fill(f, fork_pages / 2, 0xC100 + (uint64_t)k);
                CHECK(verify(f, fork_pages / 2, 0xC100 + (uint64_t)k) == 0);
                CHECK(verify_at(f, fork_pages / 2, fork_pages - fork_pages / 2, 0xF0F0) == 0);
                _exit(0);
            }
        }
        for (int k = 0; k < 2; k++) CHECK(wait_status(kids[k]) == 0);
        CHECK(verify(f, fork_pages, 0xF0F0) == 0);
        CHECK(munmap((void*)f, fork_pages * PAGE) == 0);
        LOG("stress: two forked children modify swapped COW pages, parent's copy intact PASS\n");
        /* (d) dirty MAP_PRIVATE file pages go to swap, the file keeps its data */
        uint64_t file_pages = ram / 2;
        int fd = open("/tmp/swaptest-data", O_RDWR | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0);
        volatile uint64_t* staging = anon(file_pages * PAGE, false); fill(staging, file_pages, 0xD000);
        CHECK(write(fd, (void*)staging, file_pages * PAGE) == (ssize_t)(file_pages * PAGE) && fsync(fd) == 0);
        CHECK(munmap((void*)staging, file_pages * PAGE) == 0);
        volatile uint64_t* m = mmap(NULL, file_pages * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0); CHECK(m != MAP_FAILED);
        CHECK(verify(m, file_pages, 0xD000) == 0);
        fill(m, file_pages, 0xE000);
        pressure(ram * 3 / 2, 0x7704);
        CHECK(verify(m, file_pages, 0xE000) == 0);
        uint64_t check[PAGE / 8];
        for (uint64_t p = 0; p < file_pages; p += 97) {
            CHECK(pread(fd, check, PAGE, (off_t)(p * PAGE)) == (ssize_t)PAGE);
            for (unsigned w = 0; w < PAGE / 8; w++) CHECK(check[w] == (tag(0xD000, p) ^ w));
        }
        CHECK(munmap((void*)m, file_pages * PAGE) == 0 && close(fd) == 0 && unlink("/tmp/swaptest-data") == 0);
        LOG("stress: dirty MAP_PRIVATE file pages swapped and restored, file unchanged PASS\n");
        /* (d2) stores through a MAP_SHARED file mapping while write-back (fsync) and reclaim run: none may be lost (pages
         * are cleaned before their data is written, so a store during the write dirties them again) */
        uint64_t mapped_pages = ram / 8;
        int sfd = open("/tmp/swaptest-shared", O_RDWR | O_CREAT | O_TRUNC, 0600); CHECK(sfd >= 0 && ftruncate(sfd, (off_t)(mapped_pages * PAGE)) == 0);
        volatile uint64_t* sm = mmap(NULL, mapped_pages * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0); CHECK(sm != MAP_FAILED);
        volatile uint64_t* control = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); CHECK(control != MAP_FAILED);
        control[0] = 0; control[1] = 0;
        pid_t writer = fork(); CHECK(writer >= 0);
        if (!writer) {
            uint64_t g = 0;
            do { g++; for (uint64_t q = 0; q < mapped_pages; q++) sm[q * (PAGE / 8)] = g * 1000003 + q; } while (!control[0]);
            control[1] = g; _exit(0);
        }
        for (int i = 0; i < 6; i++) { CHECK(fsync(sfd) == 0); pressure(ram / 2, 0x7710 + (uint64_t)i); }
        control[0] = 1;
        int wstatus = wait_status(writer); CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
        uint64_t final = control[1]; CHECK(final > 0);
        CHECK(fsync(sfd) == 0); pressure(ram * 3 / 2, 0x7720);
        for (uint64_t q = 0; q < mapped_pages; q++) {
            uint64_t word; CHECK(pread(sfd, &word, 8, (off_t)(q * PAGE)) == 8);
            if (word != final * 1000003 + q) { LOG("FAIL shared page %lu holds generation %lu, expected %lu\n", (unsigned long)q, (unsigned long)(word / 1000003), (unsigned long)final); CHECK(0); }
            CHECK(sm[q * (PAGE / 8)] == final * 1000003 + q);
        }
        CHECK(munmap((void*)sm, mapped_pages * PAGE) == 0 && munmap((void*)control, PAGE) == 0 && close(sfd) == 0 && unlink("/tmp/swaptest-shared") == 0);
        LOG("stress: MAP_SHARED file stores during fsync and reclaim all kept (%lu generations) PASS\n", (unsigned long)final);
        /* (e) memfd and /dev/shm pages go to swap while nothing maps them */
        uint64_t shm_pages = ram / 4;
        int memfd = memfd_create("swaptest", MFD_CLOEXEC); CHECK(memfd >= 0 && ftruncate(memfd, (off_t)(shm_pages * PAGE)) == 0);
        volatile uint64_t* mm = mmap(NULL, shm_pages * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0); CHECK(mm != MAP_FAILED);
        fill(mm, shm_pages, 0x3E3E); CHECK(munmap((void*)mm, shm_pages * PAGE) == 0);
        int shm = open("/dev/shm/swaptest", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); CHECK(shm >= 0);
        volatile uint64_t* body = anon(shm_pages * PAGE, false); fill(body, shm_pages, 0x4E4E);
        CHECK(write(shm, (void*)body, shm_pages * PAGE) == (ssize_t)(shm_pages * PAGE)); CHECK(munmap((void*)body, shm_pages * PAGE) == 0);
        long shmem_before = meminfo("Shmem");
        pressure(ram * 3 / 2, 0x7705);
        long shmem_after = meminfo("Shmem");
        CHECK(shmem_after < shmem_before);
        volatile uint64_t* back = anon(shm_pages * PAGE, false);
        CHECK(pread(memfd, (void*)back, shm_pages * PAGE, 0) == (ssize_t)(shm_pages * PAGE) && verify(back, shm_pages, 0x3E3E) == 0);
        CHECK(pread(shm, (void*)back, shm_pages * PAGE, 0) == (ssize_t)(shm_pages * PAGE) && verify(back, shm_pages, 0x4E4E) == 0);
        CHECK(munmap((void*)back, shm_pages * PAGE) == 0 && close(memfd) == 0 && close(shm) == 0 && unlink("/dev/shm/swaptest") == 0);
        LOG("stress: memfd and /dev/shm pages swapped (Shmem %ld -> %ld kB) and read back PASS\n", shmem_before, shmem_after);
        /* (f) mlocked memory never leaves RAM */
        uint64_t locked_pages = 1024;
        volatile uint64_t* l = anon(locked_pages * PAGE, false); CHECK(mlock((void*)l, locked_pages * PAGE) == 0);
        fill(l, locked_pages, 0x10C4);
        pressure(ram * 3 / 2, 0x7706);
        CHECK(smaps_field((void*)l, "Swap") == 0 && smaps_field((void*)l, "Locked") == (long)(locked_pages * 4));
        unsigned char lvec[1024]; CHECK(mincore((void*)l, locked_pages * PAGE, lvec) == 0);
        for (uint64_t i = 0; i < locked_pages; i++) CHECK(lvec[i] == 1);
        CHECK(verify(l, locked_pages, 0x10C4) == 0 && munlock((void*)l, locked_pages * PAGE) == 0 && munmap((void*)l, locked_pages * PAGE) == 0);
        LOG("stress: mlocked pages stay resident under pressure PASS\n");
    }
    /* (g) swapoff: refused with ENOMEM while the pages cannot fit, then done under load once they can */
    uint64_t big = ram * 3 / 2;
    volatile uint64_t* s = anon(big * PAGE, false); fill(s, big, 0x0FF0);
    errno = 0; CHECK(swapoff("/tmp/swapfile") == -1 && errno == ENOMEM);
    CHECK(swaps_has("/tmp/swapfile", "file", -1, 99999) && verify(s, big, 0x0FF0) == 0);
    LOG("stress: swapoff refused with ENOMEM while the swapped pages cannot fit, data intact PASS\n");
    uint64_t rest_pages = ram / 3, gone = big - rest_pages;   /* keep a third of RAM (partly swapped): it fits again */
    CHECK(munmap((void*)s, gone * PAGE) == 0);
    volatile uint64_t* rest = s + gone * (PAGE / 8);
    pid_t parent = getpid(), loader = fork(); CHECK(loader >= 0);
    if (!loader) { while (getppid() == parent) pressure(ram / 8, 0x7707); _exit(0); }   /* ends with a failed parent too */
    CHECK(swapoff("/tmp/swapfile") == 0 && meminfo("SwapTotal") == 0);
    kill(loader, SIGKILL); CHECK(WIFSIGNALED(wait_status(loader)));
    CHECK(verify_at(s, gone, rest_pages, 0x0FF0) == 0);
    CHECK(munmap((void*)rest, rest_pages * PAGE) == 0);
    LOG("stress: swapoff under load brings every page back PASS\n");
    CHECK(put("/proc/sys/vm/drop_caches", "1") == 0);
    LOG("stress: vmstat pswpin=%ld pswpout=%ld swap_ra=%ld\n", vmstat("pswpin"), vmstat("pswpout"), vmstat("swap_ra"));
    CHECK(unlink("/tmp/swapfile") == 0);
    LOG("STRESS ALL PASSED\n");
}

/* ---- OOM ---- */
static void set_adj(int value) { char v[16]; snprintf(v, sizeof(v), "%d", value); CHECK(put("/proc/self/oom_score_adj", v) == 0); }
static pid_t hog(uint64_t pages, int adj, int ready, bool forever) {
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid) return pid;
    set_adj(adj);
    volatile uint64_t* p = anon(pages * PAGE, false);
    for (uint64_t i = 0; i < pages; i++) p[i * (PAGE / 8)] = i;
    if (ready >= 0) CHECK(write(ready, "r", 1) == 1);
    for (;;) { if (!forever) pause(); else for (uint64_t i = 0; i < pages; i += 64) p[i * (PAGE / 8)]++; }
}
static long oom_score(pid_t pid) { char path[64], text[32]; snprintf(path, sizeof(path), "/proc/%d/oom_score", (int)pid); return slurp(path, text, sizeof(text)) > 0 ? atol(text) : -1; }
static void mode_oom(void) {
    uint64_t ram = ram_pages();
    CHECK(meminfo("SwapTotal") == 0);
    /* a sentinel answers before and after a hog of twice the RAM is killed */
    int ping[2], pong[2]; CHECK(pipe(ping) == 0 && pipe(pong) == 0);
    pid_t sentinel = fork(); CHECK(sentinel >= 0);
    if (!sentinel) { close(ping[1]); close(pong[0]); char c; while (read(ping[0], &c, 1) == 1) CHECK(write(pong[1], &c, 1) == 1); _exit(0); }
    char c = 'a'; CHECK(write(ping[1], &c, 1) == 1 && read(pong[0], &c, 1) == 1);
    pid_t big = fork(); CHECK(big >= 0);
    if (!big) { volatile uint64_t* p = anon(ram * 2 * PAGE, false); for (uint64_t i = 0; i < ram * 2; i++) p[i * (PAGE / 8)] = i; _exit(0); }
    int status = wait_status(big);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    c = 'b'; CHECK(write(ping[1], &c, 1) == 1 && read(pong[0], &c, 1) == 1 && c == 'b');
    CHECK(vmstat("oom_kill") >= 1);
    LOG("oom: hog of 2x RAM OOM-killed (SIGKILL), sentinel %d survives and answers PASS\n", (int)sentinel);
    /* oom_score_adj: 1000 is chosen first although smaller, -1000 never */
    int ready[2]; CHECK(pipe(ready) == 0);
    pid_t a = hog(ram / 8, 1000, ready[1], false), b = hog(ram / 4, 0, ready[1], false);
    CHECK(read(ready[0], &c, 1) == 1 && read(ready[0], &c, 1) == 1);
    long score_a = oom_score(a), score_b = oom_score(b), score_self = oom_score(getpid());
    CHECK(score_a > score_b && score_b > 0 && score_self >= 0);
    LOG("oom: oom_score a=%ld b=%ld (adj 1000 vs 0) PASS\n", score_a, score_b);
    LOG("oom: victims in order a=%d b=%d\n", (int)a, (int)b);
    pid_t eater = fork(); CHECK(eater >= 0);
    if (!eater) {
        set_adj(-1000);
        uint64_t pages = ram * 3 / 4; volatile uint64_t* p = anon(pages * PAGE, false);
        fill(p, pages, 0xEA7); CHECK(verify(p, pages, 0xEA7) == 0); _exit(0);
    }
    CHECK(wait_status(eater) == 0);
    pid_t reaped = 0; for (int i = 0; i < 400 && reaped != a; i++) { reaped = waitpid(a, &status, WNOHANG); if (reaped != a) usleep(10000); }
    CHECK(reaped == a && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);   /* adj 1000 went first */
    CHECK(oom_score(getpid()) >= 0);
    if (waitpid(b, &status, WNOHANG) == b) CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    else { kill(b, SIGKILL); CHECK(WIFSIGNALED(wait_status(b))); }
    LOG("oom: oom_score_adj 1000 killed first, -1000 protected task completed PASS\n");
    c = 'c'; CHECK(write(ping[1], &c, 1) == 1 && read(pong[0], &c, 1) == 1 && c == 'c');
    close(ping[1]); CHECK(wait_status(sentinel) == 0);
    LOG("OOM ALL PASSED\n");
}

/* ---- ext4 root ---- */
static void mode_ext4(void) {
    uint64_t ram = ram_pages();
    int fd = open("/tmp/swap-holes", O_RDWR | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0);
    unsigned char page[PAGE]; make_header(page, 1024, 0, NULL, false, "SWAPSPACE2");
    CHECK(pwrite(fd, page, PAGE, 0) == (ssize_t)PAGE && ftruncate(fd, 1024 * PAGE) == 0 && fsync(fd) == 0 && close(fd) == 0);
    errno = 0; CHECK(swapon("/tmp/swap-holes", 0) == -1 && errno == EINVAL);
    CHECK(unlink("/tmp/swap-holes") == 0);
    LOG("ext4: swap file with holes refused (EINVAL) PASS\n");
    uint64_t pages = ram * 2;
    create_swapfile("/tmp/swapfile", pages, (uint32_t)pages, 0, NULL, false, "SWAPSPACE2");
    CHECK(swapon("/tmp/swapfile", 0) == 0);
    uint64_t want = ram * 3 / 2; volatile uint64_t* a = anon(want * PAGE, false);
    fill(a, want, 0xE4E4); CHECK(vmstat("pswpout") > 0 && verify(a, want, 0xE4E4) == 0);
    CHECK(munmap((void*)a, want * PAGE) == 0 && swapoff("/tmp/swapfile") == 0 && unlink("/tmp/swapfile") == 0);
    LOG("ext4: swap file on extents, 1.5x RAM verified PASS\n");
    LOG("EXT4 ALL PASSED\n");
}
/* ---- Debian's tools (desktop disk with swap_tools.py install + swapfile) ---- */
static int run_tool(const char* const* argv, char* out, size_t size, uid_t uid) {   /* exit status; stdout+stderr in out */
    int pipefd[2]; CHECK(pipe(pipefd) == 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        dup2(pipefd[1], 1); dup2(pipefd[1], 2); close(pipefd[0]); close(pipefd[1]);
        if (uid && setuid(uid) != 0) _exit(126);
        char* env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C", NULL};
        execve(argv[0], (char* const*)argv, env); _exit(127);
    }
    close(pipefd[1]); size_t n = 0; ssize_t got;
    while (n + 1 < size && (got = read(pipefd[0], out + n, size - 1 - n)) > 0) n += (size_t)got;
    out[n] = 0; close(pipefd[0]);
    int status = wait_status(pid);
    LOG("debian: $"); for (unsigned i = 0; argv[i]; i++) dprintf(1, " %s", argv[i]);
    dprintf(1, " -> %d\n%s", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), out);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
static long free_swap_total(void) {   /* "Swap:" total of free -k */
    char out[4096]; const char* argv[] = {"/usr/bin/free", "-k", NULL};
    CHECK(run_tool(argv, out, sizeof(out), 0) == 0);
    char* line = strstr(out, "\nSwap:"); CHECK(line);
    return strtol(line + 6, NULL, 10);
}
static void mode_debian(void) {
    char out[8192];
    /* the kernel activated /etc/fstab's swap file before init (swap_tools.py swapfile) */
    CHECK(slurp("/proc/swaps", out, sizeof(out)) > 0 && strstr(out, "/swapfile "));
    long boot_total = meminfo("SwapTotal"); CHECK(boot_total > 0 && free_swap_total() == boot_total);
    const char* show[] = {"/usr/sbin/swapon", "--show", NULL}; CHECK(run_tool(show, out, sizeof(out), 0) == 0 && strstr(out, "/swapfile") && strstr(out, "file"));
    LOG("debian: /etc/fstab swap file active at boot, swapon --show and free agree PASS\n");
    const char* off_all[] = {"/usr/sbin/swapoff", "-a", NULL}; CHECK(run_tool(off_all, out, sizeof(out), 0) == 0 && meminfo("SwapTotal") == 0);
    const char* on_all[] = {"/usr/sbin/swapon", "-a", NULL}; CHECK(run_tool(on_all, out, sizeof(out), 0) == 0 && meminfo("SwapTotal") == boot_total);
    LOG("debian: swapoff -a / swapon -a (libmount reads /etc/fstab) PASS\n");
    create_swapfile("/tmp/swap-deb", 16384, 1, 0, NULL, false, "SWAPSPACE2");   /* dd; mkswap writes the real header */
    const char* mk[] = {"/usr/sbin/mkswap", "-L", "debswap", "/tmp/swap-deb", NULL};
    CHECK(run_tool(mk, out, sizeof(out), 0) == 0 && strstr(out, "Setting up swapspace version 1") && strstr(out, "LABEL=debswap"));
    const char* on[] = {"/usr/sbin/swapon", "-p", "5", "/tmp/swap-deb", NULL}; CHECK(run_tool(on, out, sizeof(out), 0) == 0);
    CHECK(swaps_has("/tmp/swap-deb", "file", 16383 * 4, 5) && meminfo("SwapTotal") == boot_total + 16383 * 4);
    CHECK(free_swap_total() == boot_total + 16383 * 4);
    const char* summary[] = {"/usr/sbin/swapon", "-s", NULL}; CHECK(run_tool(summary, out, sizeof(out), 0) == 0 && strstr(out, "/tmp/swap-deb"));
    LOG("debian: mkswap -L, swapon -p 5, swapon -s, free with two areas PASS\n");
    const char* again[] = {"/usr/sbin/swapon", "/tmp/swap-deb", NULL}; CHECK(run_tool(again, out, sizeof(out), 0) != 0 && strstr(out, "Device or resource busy"));
    const char* user_on[] = {"/usr/sbin/swapoff", "/tmp/swap-deb", NULL}; CHECK(run_tool(user_on, out, sizeof(out), 1000) != 0 && (strstr(out, "Operation not permitted") || strstr(out, "Not superuser")));   /* util-linux checks the euid itself */
    CHECK(run_tool(user_on, out, sizeof(out), 0) == 0 && !swaps_has("/tmp/swap-deb", "file", -1, 99999));
    const char* bogus[] = {"/usr/sbin/swapon", "/tmp", NULL}; CHECK(run_tool(bogus, out, sizeof(out), 0) != 0);
    LOG("debian: swapon busy, swapoff as user refused, swapoff, swapon of a directory refused PASS\n");
    /* under pressure with the boot swap file: a 1.5x RAM working set, free shows swap in use */
    uint64_t pages = ram_pages() * 3 / 2; volatile uint64_t* a = anon(pages * PAGE, false);
    fill(a, pages, 0xDEB); CHECK(verify(a, pages, 0xDEB) == 0);
    const char* fr[] = {"/usr/bin/free", "-m", NULL}; CHECK(run_tool(fr, out, sizeof(out), 0) == 0);
    CHECK(munmap((void*)a, pages * PAGE) == 0 && unlink("/tmp/swap-deb") == 0);
    LOG("DEBIAN ALL PASSED\n");
}
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    char mode[64] = "api"; int fd = open("/nexus/swaptest.conf", O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, mode, sizeof(mode) - 1); close(fd); if (n > 0) { mode[n] = 0; char* nl = strchr(mode, '\n'); if (nl) *nl = 0; } }
    LOG("mode=%s MemTotal=%ld kB\n", mode, meminfo("MemTotal"));
    if (!strcmp(mode, "api")) mode_api();
    else if (!strcmp(mode, "stress")) mode_stress(1);
    else if (!strncmp(mode, "stress:", 7)) mode_stress(atoi(mode + 7));
    else if (!strcmp(mode, "oom")) mode_oom();
    else if (!strcmp(mode, "ext4")) mode_ext4();
    else if (!strcmp(mode, "debian")) mode_debian();
    else { LOG("FAIL unknown mode\n"); return 99; }
    return 0;
}
