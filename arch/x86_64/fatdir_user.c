/* Large FAT32 directories (tests/test_x64_fatdir.py boots this as /sbin/init, as root; static, musl). The mode is the
 * first line of /nexus/fatdir.conf:
 *   build N      /home/nexus/big gets N files with long names (Wine's i386-windows naming: api-ms-win-*.dll and
 *                friends, the last alphabetically is zlib1.dll), each create followed by filler data in another
 *                directory so the directory's clusters are fragmented; then "check", then a clean power-off
 *   check N      negative lookups first (they install the directory's name filter), then every name: stat, open,
 *                read back, an upper-case lookup; getdents64 lists every name exactly once (small and large buffers,
 *                lseek back to the start), a case-insensitive readdir scan finds each name (Wine's fallback)
 *   race N SECS  "check" while other processes create/write/unlink files in the filler directory, exit holding
 *                open-unlinked files (killed by SIGKILL) and look names up in parallel; every lookup of an existing
 *                name must succeed
 * Lines "[64] FATDIR ..."; a failure prints "[64] FATDIR FAIL line=N" and exits 99. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "[64] FATDIR FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); _exit(99); } } while (0)
#define LOG(...) dprintf(1, "[64] FATDIR " __VA_ARGS__)
#define BIG "/home/nexus/big"
#define FILL "/home/nexus/filler"

static char names[2048][96];
static const char* stems[] = {"api-ms-win-core-%s-l1-1-%d.dll", "api-ms-win-crt-%s-l1-1-%d.dll", "%s%d.dll", "lib%s-%d.dll",
                              "api-ms-win-shcore-%s-l1-1-%d.dll", "ext-ms-win-%s-l1-%d.dll", "%s32_%d.drv", "%s%d.exe"};
static const char* words[] = {"file", "heap", "memory", "string", "synch", "console", "datetime", "debug", "errorhandling",
                              "handle", "interlocked", "libraryloader", "localization", "processenvironment", "profile",
                              "registry", "rtlsupport", "sysinfo", "timezone", "util", "winrt", "wow64", "xstate", "kernel",
                              "user", "gdi", "shell", "ole", "comctl", "msvcr", "ucrtbase", "vcruntime", "mscoree", "d3d"};
static void make_names(int n) {
    for (int i = 0; i < n - 1; i++) {
        const char* stem = stems[i % 8]; const char* word = words[(i / 8) % 34];
        snprintf(names[i], sizeof(names[i]), stem, word, i);
    }
    snprintf(names[n - 1], sizeof(names[n - 1]), "zlib1.dll");   /* created last: the directory's last entry */
}
static uint32_t content_of(int i) { return 0x5EED0000u ^ (uint32_t)i * 2654435761u; }
static void put(const char* path, const void* data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    CHECK(write(fd, data, n) == (ssize_t)n); close(fd);
}
static void lookup_one(int i, const char* why) {
    char path[160]; struct stat st; snprintf(path, sizeof(path), BIG "/%s", names[i]);
    if (stat(path, &st)) { LOG("LOOKUP MISS %s (%s) errno=%d\n", names[i], why, errno); CHECK(0); }
    CHECK(st.st_size == 4 + i % 300);
    int fd = open(path, O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    uint32_t v = 0; CHECK(read(fd, &v, 4) == 4 && v == content_of(i)); close(fd);
}
static void upper(int i) {
    char path[160], up[96]; snprintf(up, sizeof(up), "%s", names[i]);
    for (char* p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    snprintf(path, sizeof(path), BIG "/%s", up); struct stat st;
    if (stat(path, &st)) { LOG("LOOKUP MISS upper-case %s errno=%d\n", up, errno); CHECK(0); }
}
static void negatives(int round) {
    for (int k = 0; k < 40; k++) {
        char path[160]; struct stat st; snprintf(path, sizeof(path), BIG "/missing-%d-%d.dll", round, k);
        CHECK(stat(path, &st) < 0 && errno == ENOENT);
    }
}
struct linux_dirent64 { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; };
static void listing(int n, size_t buffer_size) {
    int fd = open(BIG, O_RDONLY | O_DIRECTORY | O_CLOEXEC); CHECK(fd >= 0);
    static unsigned char seen[2048]; memset(seen, 0, sizeof(seen));
    static char buffer[65536]; int count = 0, dots = 0;
    for (int pass = 0; pass < 2; pass++) {
        memset(seen, 0, sizeof(seen)); count = dots = 0;
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        for (;;) {
            long got = syscall(SYS_getdents64, fd, buffer, buffer_size); CHECK(got >= 0);
            if (!got) break;
            for (long at = 0; at < got; ) {
                struct linux_dirent64* d = (void*)(buffer + at); at += d->reclen;
                if (!strcmp(d->name, ".") || !strcmp(d->name, "..")) { dots++; continue; }
                int hit = -1; for (int i = 0; i < n; i++) if (!strcmp(names[i], d->name)) { hit = i; break; }
                if (hit < 0) { LOG("LISTING unexpected name %s\n", d->name); CHECK(0); }
                if (seen[hit]++) { LOG("LISTING duplicate %s\n", d->name); CHECK(0); }
                count++;
            }
        }
        if (count != n || dots != 2) { LOG("LISTING %d names (expected %d), %d dot entries, buffer %zu\n", count, n, dots, buffer_size); CHECK(0); }
    }
    close(fd);
}
static void insensitive(int n) {   /* Wine's case-insensitive fallback: readdir + strcasecmp */
    DIR* d = opendir(BIG); CHECK(d); static unsigned char seen[2048]; memset(seen, 0, sizeof(seen));
    struct dirent* e;
    while ((e = readdir(d))) for (int i = 0; i < n; i++) if (!strcasecmp(e->d_name, names[i])) seen[i] = 1;
    closedir(d);
    for (int i = 0; i < n; i++) if (!seen[i]) { LOG("READDIR SCAN misses %s\n", names[i]); CHECK(0); }
}
static void check_all(int n, int round) {
    negatives(round);
    for (int i = n - 1; i >= 0; i--) lookup_one(i, "after negative lookups");
    for (int i = 0; i < n; i += 7) upper(i);
    upper(n - 1);
    listing(n, 65536); listing(n, 600); listing(n, 280);
    insensitive(n);
}
static void build(int n) {
    CHECK(mkdir(BIG, 0755) == 0 || errno == EEXIST); CHECK(mkdir(FILL, 0755) == 0 || errno == EEXIST);
    static uint8_t filler[3 * 4096]; memset(filler, 0xF1, sizeof(filler));
    for (int i = 0; i < n; i++) {
        char path[160]; uint8_t data[4 + 300] = {0}; uint32_t v = content_of(i); memcpy(data, &v, 4);
        snprintf(path, sizeof(path), BIG "/%s", names[i]); put(path, data, 4 + i % 300);
        snprintf(path, sizeof(path), FILL "/fill%04d", i); put(path, filler, 4096 * (1 + i % 3));   /* fragments the directory */
    }
    sync();
    LOG("BUILT %d entries\n", n);
}
static void race(int n, int seconds) {
    pid_t kids[6]; int nkids = 0;
    for (int w = 0; w < 6; w++) {
        pid_t pid = fork(); CHECK(pid >= 0);
        if (pid) { kids[nkids++] = pid; continue; }
        srand((unsigned)(w * 7919 + 1)); time_t end = time(NULL) + seconds; unsigned long ops = 0;
        while (time(NULL) < end) {
            if (w < 2) { lookup_one(rand() % n, "parallel lookup"); if (!(ops % 50)) negatives((int)(ops + w * 1000000)); }
            else if (w == 2) {   /* namespace + allocation churn in another directory */
                char path[96]; snprintf(path, sizeof(path), FILL "/churn%lu", ops % 64);
                static uint8_t block[20000]; put(path, block, (size_t)(rand() % 20000));
                if (rand() & 1) unlink(path);
            } else if (w == 3) {   /* processes that die by a signal with open-unlinked files */
                pid_t c = fork(); CHECK(c >= 0);
                if (!c) {
                    char path[96]; snprintf(path, sizeof(path), FILL "/dying%d", getpid());
                    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600); static uint8_t b[50000]; if (fd >= 0) { (void)!write(fd, b, sizeof(b)); unlink(path); }
                    for (;;) pause();
                }
                usleep(2000); kill(c, SIGKILL); waitpid(c, NULL, 0);
            } else if (w == 4) { listing(n, 4096); }
            else { insensitive(n); }
            ops++;
        }
        LOG("RACE worker %d done after %lu operations\n", w, ops);
        _exit(0);
    }
    int failed = 0;
    for (int k = 0; k < nkids; k++) { int status = 0; waitpid(kids[k], &status, 0); if (!WIFEXITED(status) || WEXITSTATUS(status)) failed++; }
    CHECK(!failed);
}
int main(void) {
    char conf[64] = {0}; int fd = open("/nexus/fatdir.conf", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    CHECK(read(fd, conf, sizeof(conf) - 1) > 0); close(fd);
    char mode[16] = {0}; int n = 0, seconds = 0; CHECK(sscanf(conf, "%15s %d %d", mode, &n, &seconds) >= 2 && n > 1 && n <= 2048);
    LOG("MODE %s %d\n", mode, n);
    make_names(n);
    if (!strcmp(mode, "build")) { build(n); check_all(n, 0); }
    else if (!strcmp(mode, "check")) check_all(n, 1);
    else if (!strcmp(mode, "race")) { check_all(n, 2); race(n, seconds ? seconds : 20); check_all(n, 3); }
    else { LOG("FAIL unknown mode\n"); return 99; }
    LOG("ALL PASSED %s\n", mode);
    sync(); reboot(RB_POWER_OFF);
    return 99;
}
