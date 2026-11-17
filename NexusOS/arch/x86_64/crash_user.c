/* Crash-consistency workload for the ext4 root (kernel campaign /nexus/test-crash).
 * Each boot first verifies what earlier (possibly killed) boots left behind, then appends
 * checksummed 4 KiB records to a log, atomically replaces a "current generation" file
 * (write + fsync + rename + directory fsync), creates/unlinks churn files and resizes a scratch
 * file, and prints "[64] CRASH FSYNCED <n>" only after everything up to record n is durable.
 * The host kills the VM at random points (or the kernel powers off right after a journal commit);
 * the next boot's "[64] CRASH VERIFY records=<r> generation=<g>" must report r, g >= the last n. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(1, "[64] CRASH FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define DIR_ "/home/nexus/crash"
#define RECORD 4096
struct record { uint64_t magic, seq, words[RECORD / 8 - 3]; uint64_t sum; };
_Static_assert(sizeof(struct record) == RECORD, "record size");

static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; return x ^ (x >> 33); }
static void fill(struct record* r, uint64_t seq) {
    r->magic = 0x4E45585553435241ULL; r->seq = seq; uint64_t s = 0;
    for (unsigned i = 0; i < sizeof(r->words) / 8; i++) { r->words[i] = mix(seq * 1000003 + i); s += r->words[i] ^ i; }
    r->sum = s ^ seq;
}
static int valid(const struct record* r, uint64_t seq) {
    if (r->magic != 0x4E45585553435241ULL || r->seq != seq) return 0;
    uint64_t s = 0;
    for (unsigned i = 0; i < sizeof(r->words) / 8; i++) { if (r->words[i] != mix(seq * 1000003 + i)) return 0; s += r->words[i] ^ i; }
    return r->sum == (s ^ seq);
}
static void fsync_dir(const char* path) { int d = open(path, O_RDONLY | O_DIRECTORY); CHECK(d >= 0 && fsync(d) == 0 && close(d) == 0); }
static unsigned generation_bytes(uint64_t g) { return (unsigned)(16 + (g * 7919) % 30000); }
static int check_generation(const char* path, uint64_t* out) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    char head[32] = {0}; struct stat st;
    if (fstat(fd, &st) || read(fd, head, 20) != 20 || strncmp(head, "generation ", 11)) { close(fd); return -2; }
    uint64_t g = strtoull(head + 11, NULL, 10);
    if ((unsigned)st.st_size != generation_bytes(g)) { close(fd); return -3; }
    static unsigned char body[32768];
    if (pread(fd, body, st.st_size, 0) != st.st_size) { close(fd); return -4; }
    for (unsigned i = 20; i < st.st_size; i++) if (body[i] != (unsigned char)(g + i)) { close(fd); return -5; }
    close(fd); *out = g; return 0;
}
static void write_generation(uint64_t g) {
    static unsigned char body[32768];
    unsigned n = generation_bytes(g);
    memset(body, 0, 20); snprintf((char*)body, 21, "generation %-9llu", (unsigned long long)g);
    for (unsigned i = 20; i < n; i++) body[i] = (unsigned char)(g + i);
    int fd = open(DIR_ "/next", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(fd >= 0 && write(fd, body, n) == (ssize_t)n && fsync(fd) == 0 && close(fd) == 0);
    CHECK(rename(DIR_ "/next", DIR_ "/current") == 0);
}
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char mode[32] = {0}; int m = open("/nexus/test-crash", O_RDONLY);
    if (m >= 0) { CHECK(read(m, mode, sizeof(mode) - 1) >= 0); close(m); }
    int seconds = strncmp(mode, "commit", 6) ? atoi(mode[0] ? mode : "8") : 0;
    if (mkdir(DIR_, 0755) && errno != EEXIST) CHECK(0);
    if (mkdir(DIR_ "/churn", 0755) && errno != EEXIST) CHECK(0);
    /* --- verify --- */
    int log = open(DIR_ "/log", O_CREAT | O_RDWR, 0644); CHECK(log >= 0);
    struct stat st; CHECK(fstat(log, &st) == 0);
    static struct record r;
    uint64_t records = 0;
    while ((records + 1) * RECORD <= (uint64_t)st.st_size && pread(log, &r, RECORD, records * RECORD) == RECORD && valid(&r, records + 1)) records++;
    uint64_t generation = 0;
    int g = check_generation(DIR_ "/current", &generation);
    CHECK(g == 0 || (g == -1 && records == 0));
    /* churn files: every name present must hold its own seed */
    unsigned churn = 0;
    for (uint64_t k = records > 64 ? records - 64 : 1; k <= records + 8; k++) {
        char p[96]; snprintf(p, sizeof(p), DIR_ "/churn/c%llu", (unsigned long long)k);
        int fd = open(p, O_RDONLY); if (fd < 0) continue;
        /* never fsync'd: may be empty after a crash, but ordered mode must never show stale bytes */
        uint64_t v = 0; ssize_t n = read(fd, &v, 8); CHECK((n == 0 || (n == 8 && v == mix(k))) && close(fd) == 0); churn++;
    }
    int scratch = open(DIR_ "/scratch", O_RDWR | O_CREAT, 0644); CHECK(scratch >= 0);
    CHECK(fstat(scratch, &st) == 0 && st.st_size % 65536 == 0);
    dprintf(1, "[64] CRASH VERIFY records=%llu generation=%llu churn=%u\n", (unsigned long long)records, (unsigned long long)generation, churn);
    /* the generation rename is committed no later than the record's fsync, never more than one ahead */
    if (records && (generation < records || generation > records + 1)) CHECK(0);
    CHECK(ftruncate(log, records * RECORD) == 0 && fsync(log) == 0);
    /* --- write until killed or the time limit --- */
    /* A crash after the held file's creation was committed but before its unlink was leaves it linked;
     * ordered mode means it can hold nothing but its own bytes (any prefix of them). */
    int stale = open(DIR_ "/held", O_RDONLY);
    if (stale >= 0) {
        static unsigned char seen[65536]; ssize_t n = read(stale, seen, sizeof(seen)); CHECK(n >= 0);
        for (ssize_t i = 0; i < n; i++) CHECK(seen[i] == 0x5A);
        CHECK(close(stale) == 0 && unlink(DIR_ "/held") == 0);
        dprintf(1, "[64] CRASH HELD FILE LEFT LINKED bytes=%lld\n", (long long)n);
    } else CHECK(errno == ENOENT);
    /* an unlinked file stays open while the workload runs: a crash leaves it on the orphan list */
    static unsigned char held_data[65536]; memset(held_data, 0x5A, sizeof(held_data));
    int held = open(DIR_ "/held", O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(held >= 0 && write(held, held_data, sizeof(held_data)) == sizeof(held_data) && fsync(held) == 0);
    CHECK(unlink(DIR_ "/held") == 0); fsync_dir(DIR_);
    dprintf(1, "[64] CRASH WRITING mode=%s\n", mode[0] ? mode : "8");
    struct timespec start, now; clock_gettime(CLOCK_MONOTONIC, &start);
    static unsigned char big[65536];
    for (uint64_t seq = records + 1;; seq++) {
        write_generation(seq);
        fill(&r, seq); CHECK(pwrite(log, &r, RECORD, (seq - 1) * RECORD) == RECORD);
        char p[96]; snprintf(p, sizeof(p), DIR_ "/churn/c%llu", (unsigned long long)seq);
        int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644); uint64_t v = mix(seq);
        CHECK(fd >= 0 && write(fd, &v, 8) == 8 && close(fd) == 0);
        if (seq > 16) { snprintf(p, sizeof(p), DIR_ "/churn/c%llu", (unsigned long long)(seq - 16)); unlink(p); }
        if (seq % 5 == 0) { /* grow or shrink a scratch file in 64 KiB steps */
            off_t size = (off_t)((seq * 131) % 64) * 65536;
            memset(big, (int)seq, sizeof(big));
            CHECK(ftruncate(scratch, size) == 0 && pwrite(scratch, big, sizeof(big), size) == sizeof(big));
        }
        CHECK(fsync(log) == 0 && fsync(scratch) == 0);
        fsync_dir(DIR_); fsync_dir(DIR_ "/churn");
        dprintf(1, "[64] CRASH FSYNCED %llu\n", (unsigned long long)seq);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (seconds && now.tv_sec - start.tv_sec >= seconds) break;
        /* The test volume is small and records accumulate over every boot (a faster machine writes more of them per
         * kill window): stop while the scratch file (up to 4 MiB) and the next record still fit. */
        struct statvfs fs;
        if (statvfs(DIR_, &fs) == 0 && (uint64_t)fs.f_bavail * fs.f_frsize < (5ULL << 20)) {
            dprintf(1, "[64] CRASH VOLUME NEARLY FULL after %llu\n", (unsigned long long)seq); break;
        }
    }
    CHECK(pread(held, held_data, 16, 0) == 16 && held_data[0] == 0x5A && close(held) == 0);
    CHECK(close(log) == 0 && close(scratch) == 0);
    dprintf(1, "[64] CRASH WORKLOAD FINISHED\n");
    return 0;
}
