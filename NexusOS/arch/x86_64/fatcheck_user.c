/* FAT32 root crash-consistency campaign (tests/test_x64_fatcheck.py boots this as /sbin/init, as root). The mode is
 * the first line of /nexus/fatcheck.conf:
 *   orphans:reboot   files written, fsynced, unlinked while still open (some also mapped MAP_SHARED and written
 *                    through the mapping after the unlink), then reboot(POWER_OFF) with all of them held: the
 *                    orderly shutdown must free their clusters and leave the volume clean
 *   orphans:hold     the same, then "[64] FATCHECK HOLDING" and wait forever: the host kills the VM (power cut)
 *   cycle:SEED       verify what earlier rounds made durable (/home/nexus/fc/journal), checkpoint the observed state
 *                    into a fresh journal, then run a random workload forever (create+fsync, atomic replace by
 *                    rename, rename, unlink, truncate, append, mkdir/rmdir, open-unlinked temporaries kept open);
 *                    the host cuts the power at a random moment
 *   verify           verify only, then power off cleanly
 * Journal records (each fsynced before the operation; "D" after it is durable):
 *   C name hash size   create (in flight: absent or any content)       W name hash size  in-place change (in flight:
 *   A name hash size   atomic replace (in flight: old or new exactly)                     present, any content)
 *   U name             unlink (in flight: old content or absent)       M a b             rename (exactly one name)
 * Lines "[64] FATCHECK ..."; a failure prints "[64] FATCHECK FAIL line=N" and exits 99. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "[64] FATCHECK FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); _exit(99); } } while (0)
#define LOG(...) dprintf(1, "[64] FATCHECK " __VA_ARGS__)
#define DIR_ "/home/nexus/fc"
#define JOURNAL DIR_ "/journal"
#define MAXF 96
#define SEED0 14695981039346656037ULL

static uint64_t rng = 88172645463325252ULL;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t fnv(const uint8_t* p, size_t n, uint64_t h) { while (n--) { h ^= *p++; h *= 1099511628211ULL; } return h; }
static uint8_t buffer[1 << 20];

static void fill(uint8_t* p, size_t n, uint64_t seed) {
    uint64_t x = seed * 2654435761ULL + 1;
    for (size_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; p[i] = (uint8_t)x; }
}
static uint64_t file_hash(const char* path, long* size) {   /* *size -1: absent */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { CHECK(errno == ENOENT); *size = -1; return 0; }
    uint64_t h = SEED0; long total = 0; ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) { h = fnv(buffer, (size_t)n, h); total += n; }
    CHECK(n == 0); close(fd); *size = total; return h;
}
static int journal = -1;
static void record(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void record(const char* fmt, ...) {
    char line[256]; va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    CHECK(n > 0 && n < (int)sizeof(line) - 1); line[n++] = '\n';
    CHECK(write(journal, line, (size_t)n) == n); CHECK(fsync(journal) == 0);
}

/* ---- journal replay ---- */
struct state { char name[40]; bool present; uint64_t hash; long size; };
static struct state files[4096]; static int nfiles;
static struct state* find(const char* name) {
    for (int i = 0; i < nfiles; i++) if (!strcmp(files[i].name, name)) return &files[i];
    CHECK(nfiles < 4096); struct state* s = &files[nfiles++]; memset(s, 0, sizeof(*s)); snprintf(s->name, sizeof(s->name), "%s", name);
    return s;
}
static bool matches(const struct state* s, const char* name) {   /* the file named `name` has s's content */
    char path[96]; long size; snprintf(path, sizeof(path), DIR_ "/%s", name);
    uint64_t h = file_hash(path, &size);
    return s->present ? size == s->size && h == s->hash : size < 0;
}
static int verify(void) {
    int fd = open(JOURNAL, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { CHECK(errno == ENOENT); LOG("VERIFY no journal yet\n"); return 0; }
    static char text[8 << 20]; ssize_t total = 0, n;
    while ((n = read(fd, text + total, sizeof(text) - 1 - (size_t)total)) > 0) total += n;
    CHECK(n == 0); close(fd); text[total] = 0;
    nfiles = 0; int records = 0;
    char op = 0, a[40] = "", b[40] = ""; uint64_t h = 0; long sz = 0;
    for (char* line = text; *line; ) {
        char* end = strchr(line, '\n'); if (!end) break;   /* a torn last record never happened */
        *end = 0; records++;
        char kind = line[0], n1[40], n2[40]; unsigned long long hv; long sv;
        if (kind == 'D') {
            struct state* s;
            if (op == 'C' || op == 'A' || op == 'W') { s = find(a); s->present = true; s->hash = h; s->size = sz; }
            else if (op == 'U') find(a)->present = false;
            else if (op == 'M') { struct state *from = find(a), *to = find(b); bool p = from->present; uint64_t fh = from->hash; long fs = from->size;
                                  to->present = p; to->hash = fh; to->size = fs; from->present = false; }
            op = 0;
        } else if (kind == 'C' || kind == 'A' || kind == 'W') {
            CHECK(sscanf(line + 2, "%39s %llx %ld", n1, &hv, &sv) == 3); op = kind; snprintf(a, 40, "%s", n1); h = hv; sz = sv;
        } else if (kind == 'U') { CHECK(sscanf(line + 2, "%39s", n1) == 1); op = kind; snprintf(a, 40, "%s", n1); }
        else if (kind == 'M') { CHECK(sscanf(line + 2, "%39s %39s", n1, n2) == 2); op = kind; snprintf(a, 40, "%s", n1); snprintf(b, 40, "%s", n2); }
        else { LOG("bad journal record '%s'\n", line); CHECK(0); }
        line = end + 1;
    }
    struct state *x = op ? find(a) : NULL, *y = op == 'M' ? find(b) : NULL;
    int checked = 0;
    for (int i = 0; i < nfiles; i++) {
        struct state* s = &files[i]; bool ok;
        char path[96]; long size; snprintf(path, sizeof(path), DIR_ "/%s", s->name); file_hash(path, &size);
        if (s == x && op == 'C') ok = true;                                   /* absent or partly written */
        else if (s == x && op == 'W') ok = size >= 0;                          /* present, content undefined */
        else if (s == x && op == 'A') { struct state now = {.present = true, .hash = h, .size = sz}; ok = matches(s, s->name) || matches(&now, s->name); }
        else if (s == x && op == 'U') { struct state gone = {0}; ok = matches(s, s->name) || matches(&gone, s->name); }
        else if ((s == x || s == y) && op == 'M') {
            struct state gone = {0};
            ok = (matches(x, a) && matches(&gone, b)) || (matches(&gone, a) && matches(x, b));
        } else ok = matches(s, s->name);
        if (!ok) { LOG("VERIFY MISMATCH %s present=%d size=%ld journal_size=%ld op=%c\n", s->name, s->present, size, s->size, op ? op : '-'); CHECK(0); }
        checked++;
    }
    LOG("VERIFY PASS %d journal records, %d names checked, operation in flight: %c\n", records, checked, op ? op : '-');
    return checked;
}

/* ---- checkpoint: a fresh journal with what is on the disk now ---- */
static char names[MAXF][40]; static bool used[MAXF];
static void checkpoint(void) {
    DIR* d = opendir(DIR_); CHECK(d);
    int tmp = open(DIR_ "/journal.new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(tmp >= 0);
    int count = 0; struct dirent* e;
    char victims[256][64]; int nvictims = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == 'f' || e->d_name[0] == 'm') {
            CHECK(count < MAXF); char path[96]; long size; snprintf(path, sizeof(path), DIR_ "/%s", e->d_name);
            uint64_t h = file_hash(path, &size); CHECK(size >= 0);
            dprintf(tmp, "C %s %llx %ld\nD\n", e->d_name, (unsigned long long)h, size);
            snprintf(names[count], 40, "%s", e->d_name); used[count++] = true;
        } else if ((!strncmp(e->d_name, "tmp.", 4) || !strncmp(e->d_name, "held.", 5)) && nvictims < 256) snprintf(victims[nvictims++], 64, "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < nvictims; i++) { char path[96]; snprintf(path, sizeof(path), DIR_ "/%s", victims[i]); CHECK(unlink(path) == 0); }
    CHECK(fsync(tmp) == 0); close(tmp);
    CHECK(rename(DIR_ "/journal.new", JOURNAL) == 0);
    journal = open(JOURNAL, O_WRONLY | O_APPEND | O_CLOEXEC); CHECK(journal >= 0);
    LOG("CHECKPOINT %d files, %d leftovers removed\n", count, nvictims);
}

/* ---- workload ---- */
static int held[16]; static int nheld;
static uint64_t content(size_t n, uint64_t seed) { fill(buffer, n, seed); return fnv(buffer, n, SEED0); }
static void write_file(const char* path, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    for (size_t at = 0; at < n; ) { ssize_t w = write(fd, buffer + at, n - at); CHECK(w > 0); at += (size_t)w; }
    CHECK(fsync(fd) == 0); close(fd);
}
static int pick(bool want_used) { for (int t = 0; t < 4 * MAXF; t++) { int i = (int)(next() % MAXF); if (used[i] == want_used) return i; } return -1; }
static void workload(unsigned seed) {
    rng ^= (uint64_t)seed * 0x9E3779B97F4A7C15ULL;
    LOG("WORKLOAD RUNNING seed=%u\n", seed);
    for (unsigned long step = 1; ; step++) {
        unsigned what = (unsigned)(next() % 10);
        int i, j; char path[96], path2[96]; uint64_t h; size_t n = (size_t)(next() % (300 * 1024));
        if (what <= 2 && (i = pick(false)) >= 0) {   /* create */
            snprintf(names[i], 40, "f%lu_%u", step, seed); snprintf(path, sizeof(path), DIR_ "/%s", names[i]);
            h = content(n, next()); record("C %s %llx %ld", names[i], (unsigned long long)h, (long)n);
            write_file(path, n); record("D"); used[i] = true;
        } else if (what == 3 && (i = pick(true)) >= 0) {   /* atomic replace: temp + fsync + rename over */
            snprintf(path, sizeof(path), DIR_ "/%s", names[i]); snprintf(path2, sizeof(path2), DIR_ "/tmp.%lu", step);
            h = content(n, next()); write_file(path2, n);
            record("A %s %llx %ld", names[i], (unsigned long long)h, (long)n);
            CHECK(rename(path2, path) == 0); record("D");
        } else if (what == 4 && (i = pick(true)) >= 0 && (j = pick(false)) >= 0) {   /* rename to a new name */
            snprintf(names[j], 40, "m%lu_%u", step, seed);
            snprintf(path, sizeof(path), DIR_ "/%s", names[i]); snprintf(path2, sizeof(path2), DIR_ "/%s", names[j]);
            record("M %s %s", names[i], names[j]); CHECK(rename(path, path2) == 0); record("D");
            used[i] = false; used[j] = true;
        } else if (what == 5 && (i = pick(true)) >= 0) {   /* unlink */
            snprintf(path, sizeof(path), DIR_ "/%s", names[i]);
            record("U %s", names[i]); CHECK(unlink(path) == 0); record("D"); used[i] = false;
        } else if (what == 6 && (i = pick(true)) >= 0) {   /* truncate shorter or longer (zeros) */
            snprintf(path, sizeof(path), DIR_ "/%s", names[i]); long size; file_hash(path, &size); CHECK(size >= 0);
            long cut = (long)(next() % (uint64_t)(size + 70000)); if (cut > (long)sizeof(buffer)) continue;
            int fd = open(path, O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
            long keep = cut < size ? cut : size; CHECK(read(fd, buffer, (size_t)keep) == keep); close(fd);
            if (cut > size) memset(buffer + size, 0, (size_t)(cut - size));
            h = fnv(buffer, (size_t)cut, SEED0);
            record("W %s %llx %ld", names[i], (unsigned long long)h, cut);
            fd = open(path, O_WRONLY | O_CLOEXEC); CHECK(fd >= 0 && ftruncate(fd, cut) == 0 && fsync(fd) == 0); close(fd); record("D");
        } else if (what == 7 && (i = pick(true)) >= 0) {   /* append */
            snprintf(path, sizeof(path), DIR_ "/%s", names[i]); long size; file_hash(path, &size); CHECK(size >= 0);
            size_t add = n / 4 + 1; if ((size_t)size + add > sizeof(buffer)) continue;
            int fd = open(path, O_RDONLY | O_CLOEXEC); CHECK(fd >= 0 && read(fd, buffer, (size_t)size) == size); close(fd);
            fill(buffer + size, add, next()); h = fnv(buffer, (size_t)size + add, SEED0);
            record("W %s %llx %ld", names[i], (unsigned long long)h, size + (long)add);
            fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC); CHECK(fd >= 0);
            CHECK(write(fd, buffer + size, add) == (ssize_t)add && fsync(fd) == 0); close(fd); record("D");
        } else if (what == 8) {   /* open, write, unlink, keep it open: an orphan the power cut must not leak */
            snprintf(path, sizeof(path), DIR_ "/held.%lu", step);
            int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); CHECK(fd >= 0);
            fill(buffer, n, step); CHECK(write(fd, buffer, n) == (ssize_t)n);
            if (step & 1) CHECK(fsync(fd) == 0);
            CHECK(unlink(path) == 0);
            if (nheld == 16) { close(held[0]); memmove(held, held + 1, 15 * sizeof(int)); nheld--; }
            held[nheld++] = fd;
        } else if (what == 9) {   /* directories */
            snprintf(path, sizeof(path), DIR_ "/d%lu", step % 7);
            if (mkdir(path, 0755) == 0) {
                snprintf(path2, sizeof(path2), "%s/inner", path); int fd = open(path2, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
                CHECK(fd >= 0); CHECK(write(fd, "x", 1) == 1); close(fd); CHECK(unlink(path2) == 0); CHECK(rmdir(path) == 0);
            } else CHECK(errno == EEXIST);
        }
        if (!(step % 50)) LOG("WORKLOAD step %lu held=%d\n", step, nheld);
    }
}

static void orphans(bool hold) {
    static const size_t sizes[] = {1000, 4096, 65536, 300000, 1 << 20, 2 << 20, 7, 123456};
    int fds[8]; uint8_t* maps[8] = {0};
    for (int i = 0; i < 8; i++) {
        char path[64]; snprintf(path, sizeof(path), DIR_ "/orphan%d", i);
        fds[i] = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); CHECK(fds[i] >= 0);
        size_t done = 0;
        while (done < sizes[i]) { size_t n = sizes[i] - done > sizeof(buffer) ? sizeof(buffer) : sizes[i] - done;
                                  fill(buffer, n, (uint64_t)i + done); CHECK(write(fds[i], buffer, n) == (ssize_t)n); done += n; }
        CHECK(fsync(fds[i]) == 0);
        if (i & 1) { maps[i] = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0); CHECK(maps[i] != MAP_FAILED); }
        CHECK(unlink(path) == 0);
        if (maps[i]) memset(maps[i], 0x5A, sizes[i] / 2);   /* stores after the unlink: cache only */
    }
    struct stat st; CHECK(fstat(fds[5], &st) == 0 && st.st_nlink == 0 && st.st_size == (off_t)sizes[5]);
    CHECK(pread(fds[5], buffer, 16, 0) == 16 && buffer[0] == 0x5A);
    LOG("ORPHANS 8 files unlinked while open\n");
    if (hold) { LOG("HOLDING\n"); for (;;) pause(); }
    LOG("POWER OFF with the orphans held\n");
    reboot(RB_POWER_OFF);
    CHECK(0);
}

int main(void) {
    char conf[64] = {0}; int fd = open("/nexus/fatcheck.conf", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    CHECK(read(fd, conf, sizeof(conf) - 1) > 0); close(fd);
    for (char* p = conf; *p; p++) if (*p == '\n' || *p == '\r') { *p = 0; break; }
    LOG("MODE %s\n", conf);
    if (!strcmp(conf, "readonly")) {   /* fsck.repair=no found problems: mounted read-only, nothing may change */
        CHECK(open("/home/nexus/fc-new", O_WRONLY | O_CREAT | O_CLOEXEC, 0644) < 0 && errno == EROFS);
        CHECK(mkdir("/home/nexus/fc-dir", 0755) < 0 && errno == EROFS);
        CHECK(unlink("/nexus/fatcheck.conf") < 0 && errno == EROFS);
        long size; file_hash("/nexus/fatcheck.conf", &size); CHECK(size > 0);
        LOG("READONLY PASS\n"); reboot(RB_POWER_OFF); CHECK(0);
    }
    CHECK(mkdir(DIR_, 0755) == 0 || errno == EEXIST);
    if (!strcmp(conf, "orphans:reboot")) orphans(false);
    if (!strcmp(conf, "orphans:hold")) orphans(true);
    if (!strcmp(conf, "verify")) { verify(); LOG("DONE\n"); reboot(RB_POWER_OFF); CHECK(0); }
    if (!strncmp(conf, "cycle:", 6)) { verify(); checkpoint(); workload((unsigned)strtoul(conf + 6, NULL, 10)); }
    LOG("FAIL unknown mode\n");
    return 99;
}
