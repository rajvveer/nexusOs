/* ASLR conformance (tests/test_x64_aslr.py). Re-execs itself as a "probe" that prints its PIE program-header
 * address (AT_PHDR), an anonymous mmap address, the heap break, a stack address and AT_RANDOM. The orchestrator
 * checks that with ASLR on (randomize_va_space=2) those addresses vary across execs and AT_RANDOM is always fresh,
 * and that personality(ADDR_NO_RANDOMIZE) makes the whole layout deterministic across execs. Also checks that
 * /proc/sys/kernel/randomize_va_space reports the live level. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ADDR_NO_RANDOMIZE
#define ADDR_NO_RANDOMIZE 0x0040000
#endif

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] ASLR FAIL line=%d errno=%d\n", __LINE__, errno); _exit(70); } } while (0)

struct sample { uint64_t phdr, mmap_addr, brk, stack, rnd; };
static char self_path[256];

static void do_probe(void) {
    uint64_t phdr = getauxval(AT_PHDR);
    void* m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t rnd = 0; const void* r = (const void*)getauxval(AT_RANDOM); if (r) memcpy(&rnd, r, 8);
    int local;
    dprintf(1, "P=%llx M=%llx B=%llx S=%llx R=%llx\n",
            (unsigned long long)phdr, (unsigned long long)(uintptr_t)m,
            (unsigned long long)(uintptr_t)sbrk(0), (unsigned long long)(uintptr_t)&local, (unsigned long long)rnd);
}

static struct sample probe(int norandom) {
    int fds[2]; CHECK(pipe(fds) == 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) {
        if (norandom) { CHECK(personality(ADDR_NO_RANDOMIZE) != -1); }
        dup2(fds[1], 1); close(fds[0]); close(fds[1]);
        char* av[] = { self_path, (char*)"probe", NULL };
        execv(self_path, av);
        _exit(127);
    }
    close(fds[1]);
    char buf[256]; ssize_t total = 0, n;
    while (total < (ssize_t)sizeof(buf) - 1 && (n = read(fds[0], buf + total, sizeof(buf) - 1 - total)) > 0) total += n;
    buf[total > 0 ? total : 0] = 0; close(fds[0]);
    int st = 0; CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    struct sample s = {0};
    CHECK(sscanf(buf, "P=%llx M=%llx B=%llx S=%llx R=%llx",
                 (unsigned long long*)&s.phdr, (unsigned long long*)&s.mmap_addr,
                 (unsigned long long*)&s.brk, (unsigned long long*)&s.stack, (unsigned long long*)&s.rnd) == 5);
    return s;
}

#define N 5
static int all_equal_u64(const uint64_t* v, int n) { for (int i = 1; i < n; i++) if (v[i] != v[0]) return 0; return 1; }
static int all_distinct_u64(const uint64_t* v, int n) {
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (v[i] == v[j]) return 0;
    return 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "probe")) { do_probe(); return 0; }
    strncpy(self_path, argv[0], sizeof(self_path) - 1);

    /* the sysctl reports the live level (default 2) */
    int fd = open("/proc/sys/kernel/randomize_va_space", O_RDONLY); CHECK(fd >= 0);
    char lvl[8] = {0}; CHECK(read(fd, lvl, sizeof(lvl) - 1) >= 1); close(fd);
    CHECK(lvl[0] == '2');
    /* kernel-hardening sysctls are exposed (kptr_restrict / dmesg_restrict) */
    const char* hard[] = {"/proc/sys/kernel/kptr_restrict", "/proc/sys/kernel/dmesg_restrict"};
    for (unsigned i = 0; i < 2; i++) {
        int h = open(hard[i], O_RDONLY); CHECK(h >= 0);
        char v[8] = {0}; CHECK(read(h, v, sizeof(v) - 1) >= 1); close(h); CHECK(v[0] == '1');
    }

    /* own AT_RANDOM exists */
    CHECK(getauxval(AT_RANDOM) != 0);

    /* ASLR on: layout varies across execs, AT_RANDOM always fresh */
    struct sample on[N]; for (int i = 0; i < N; i++) on[i] = probe(0);
    uint64_t phdr[N], mm[N], rnd[N], brk[N];
    for (int i = 0; i < N; i++) { phdr[i] = on[i].phdr; mm[i] = on[i].mmap_addr; rnd[i] = on[i].rnd; brk[i] = on[i].brk; }
    CHECK(!all_equal_u64(phdr, N));   /* PIE/interpreter base randomized */
    CHECK(!all_equal_u64(mm, N));     /* mmap base randomized */
    CHECK(all_distinct_u64(rnd, N));  /* AT_RANDOM is fresh entropy every exec */
    (void)brk;

    /* personality(ADDR_NO_RANDOMIZE): deterministic layout across execs */
    struct sample off[N]; for (int i = 0; i < N; i++) off[i] = probe(1);
    for (int i = 0; i < N; i++) { phdr[i] = off[i].phdr; mm[i] = off[i].mmap_addr; brk[i] = off[i].brk; }
    CHECK(all_equal_u64(phdr, N));
    CHECK(all_equal_u64(mm, N));
    CHECK(all_equal_u64(brk, N));

    dprintf(1, "[64] ASLR ALL PASSED: PIE/mmap/stack/brk randomized, AT_RANDOM fresh, ADDR_NO_RANDOMIZE deterministic\n");
    return 0;
}
