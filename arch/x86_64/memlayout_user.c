/* User address space from 64 KiB (tests/test_x64_memlayout.py), the layout Wine needs: its preloader
 * reserves 0x10000-0x110000, 0x110000-0x68000000, 0x7f000000-0x7fff0000 and 0x7ffffe000000-0x7fffffff0000
 * with MAP_FIXED|MAP_NORESERVE PROT_NONE, then commits parts (KUSER_SHARED_DATA at 0x7ffe0000, 32-bit
 * PE images at 0x400000). Built three ways (build.py):
 *   memlayout64       PIE: mode '0' = layout checks, mode 'b' = touch more than 5 GiB (needs -m 8192)
 *   memlayout-exec64  -DMEMLAYOUT_EXEC: non-PIE ET_EXEC linked at 0x400000 (glibc, dynamic)
 *   memlayout-low64   -DMEMLAYOUT_LOW: static freestanding ET_EXEC linked at 0x10000 (mmap_min_addr) */
#ifdef MEMLAYOUT_LOW
static long call3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r;
}
static const char message[] = "[64] MEMLAYOUT LOW PASS: static ET_EXEC at 0x10000\n";
__attribute__((used)) void low_main(long* stack) {
    unsigned long here = (unsigned long)&low_main;
    long ok = here >= 0x10000 && here < 0x20000 && stack[0] == 1; /* argc */
    if (ok) call3(1, 1, (long)message, sizeof(message) - 1);
    call3(60, ok ? 0 : 7, 0, 0);
    for (;;) {}
}
__asm__(".globl _start\n_start:\n mov %rsp, %rdi\n and $-16, %rsp\n call low_main\n hlt\n");
#else
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define PAGE 4096UL
#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] MEMLAYOUT FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)

static char maps[65536];
/* /proc/self/maps holds a line starting with `prefix` (and containing `also`, when given). */
static int maps_has(const char* prefix, const char* also) {
    int fd = open("/proc/self/maps", O_RDONLY); CHECK(fd >= 0);
    size_t used = 0; ssize_t n;
    while (used < sizeof(maps) - 1 && (n = read(fd, maps + used, sizeof(maps) - 1 - used)) > 0) used += (size_t)n;
    close(fd); maps[used] = 0;
    for (char* line = maps; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        char* end = strchr(line, '\n'); size_t length = end ? (size_t)(end - line) : strlen(line);
        if (strncmp(line, prefix, strlen(prefix))) continue;
        if (!also) return 1;
        char saved = line[length]; line[length] = 0; int found = strstr(line, also) != NULL; line[length] = saved;
        if (found) return 1;
    }
    return 0;
}
static void* map(uintptr_t address, size_t size, int prot, int flags) {
    return mmap((void*)address, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
}
#ifdef MEMLAYOUT_EXEC
int main(int argc, char** argv) {
    (void)argv; CHECK(argc == 1);
    uintptr_t code = (uintptr_t)&main;
    CHECK(code >= 0x400000 && code < 0x1000000);                                   /* linked, not relocated */
    CHECK(maps_has("00400000-", "memlayout-exec64"));
    CHECK(map(0x400000, PAGE, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE) == MAP_FAILED && errno == EEXIST);
    /* brk follows the low image, plus the ASLR brk gap (randomize_va_space=2 adds up to 32 MiB, like Linux). */
    char* heap = sbrk(0); CHECK(heap != (void*)-1 && (uintptr_t)heap >= 0x400000 && (uintptr_t)heap < 0x2800000);
    CHECK(sbrk(65536) == heap); memset(heap, 0x5A, 65536); CHECK(heap[65535] == 0x5A);
    char* low = map(0x10000, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_FIXED); CHECK(low == (char*)0x10000);
    strcpy(low, "low"); CHECK(!strcmp((char*)0x10000, "low"));
    dprintf(1, "[64] MEMLAYOUT EXEC PASS: non-PIE ET_EXEC at %#lx, brk %p, MAP_FIXED 0x10000\n", (unsigned long)code, (void*)heap);
    return 0;
}
#else
static int child_status(pid_t child) { int status; CHECK(child > 0 && waitpid(child, &status, 0) == child); return status; }
static sigjmp_buf recover;
static volatile uintptr_t fault_address, handler_stack;
static void on_segv(int number, siginfo_t* info, void* context) {
    (void)number; (void)context; fault_address = (uintptr_t)info->si_addr; siglongjmp(recover, 1);
}
static void on_usr1(int number) { volatile char local = (char)number; handler_stack = (uintptr_t)&local; }

static void layout(void) {
    /* 1. Nothing maps below 64 KiB (Linux mmap_min_addr): EPERM; a lower hint is raised; NULL faults. */
    CHECK(map(0, PAGE, PROT_READ | PROT_WRITE, MAP_FIXED) == MAP_FAILED && errno == EPERM);
    CHECK(map(0x1000, PAGE, PROT_READ | PROT_WRITE, MAP_FIXED) == MAP_FAILED && errno == EPERM);
    CHECK(map(0xF000, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_FIXED) == MAP_FAILED && errno == EPERM);
    CHECK(map(0x1000, 0xF000, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE) == MAP_FAILED && errno == EPERM); /* Wine's low-64K probe */
    CHECK(mprotect(NULL, PAGE, PROT_READ) == -1 && errno == ENOMEM);
    char* hinted = map(0x1000, PAGE, PROT_READ | PROT_WRITE, 0);
    CHECK(hinted != MAP_FAILED && (uintptr_t)hinted >= 0x10000 && munmap(hinted, PAGE) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { *(volatile int*)0 = 1; _exit(0); }
    int status = child_status(child); CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    /* 2. The Wine x86_64 preloader's reservations, exactly. */
    CHECK(map(0x10000, 0x100000, PROT_NONE, MAP_FIXED | MAP_NORESERVE) == (void*)0x10000);
    CHECK(map(0x110000, 0x67ef0000, PROT_NONE, MAP_FIXED | MAP_NORESERVE) == (void*)0x110000);
    CHECK(map(0x7f000000, 0xff0000, PROT_NONE, MAP_FIXED | MAP_NORESERVE) == (void*)0x7f000000);
    CHECK(map(0x7ffffe000000, 0x1ff0000, PROT_NONE, MAP_FIXED | MAP_NORESERVE) == (void*)0x7ffffe000000);
    CHECK(maps_has("00010000-00110000 ---p", NULL) && maps_has("00110000-68000000 ---p", NULL));
    CHECK(maps_has("7f000000-7fff0000 ---p", NULL) && maps_has("7ffffe000000-7fffffff0000 ---p", NULL));
    CHECK(map(0x400000, PAGE, PROT_READ, MAP_FIXED_NOREPLACE) == MAP_FAILED && errno == EEXIST);
    CHECK(map(0x7fffffff0000, PAGE, PROT_READ, MAP_FIXED_NOREPLACE) == (void*)0x7fffffff0000); /* last 64 KiB below TASK_SIZE */
    CHECK(munmap((void*)0x7fffffff0000, PAGE) == 0);
    CHECK(map(0x7ffffffff000, PAGE, PROT_READ, MAP_FIXED) == MAP_FAILED);                      /* beyond TASK_SIZE */

    /* 3. Commits inside the reservations: DOS area, a 32-bit image base, KUSER_SHARED_DATA. */
    char* dos = map(0x10000, PAGE, PROT_READ | PROT_WRITE, MAP_FIXED); CHECK(dos == (char*)0x10000);
    strcpy(dos, "dos"); CHECK(!strcmp((char*)0x10000, "dos"));
    char* image = map(0x400000, 16 * PAGE, PROT_READ | PROT_WRITE, MAP_FIXED); CHECK(image == (char*)0x400000);
    for (unsigned i = 0; i < 16; i++) image[i * PAGE] = (char)(i + 1);
    int fd = memfd_create("kuser_shared_data", 0); CHECK(fd >= 0 && ftruncate(fd, PAGE) == 0);
    CHECK(pwrite(fd, "KUSD", 4, 0) == 4);
    char* kusd = mmap((void*)0x7ffe0000, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
    CHECK(kusd == (char*)0x7ffe0000 && !memcmp(kusd, "KUSD", 4));
    CHECK(pwrite(fd, "kusd", 4, 0) == 4 && !memcmp(kusd, "kusd", 4));                         /* coherent shared page */
    CHECK(maps_has("00010000-00011000 rw-p", NULL) && maps_has("00400000-00410000 rw-p", NULL));
    CHECK(maps_has("7ffe0000-7ffe1000 r--s", "kuser_shared_data"));
    CHECK(maps_has("00011000-00110000 ---p", NULL) && maps_has("7f000000-7ffe0000 ---p", NULL) && maps_has("7ffe1000-7fff0000 ---p", NULL));

    /* 4. mprotect, madvise, mremap and munmap on low addresses. */
    CHECK(mprotect(image, PAGE, PROT_READ) == 0 && image[0] == 1);
    child = fork(); CHECK(child >= 0);
    if (!child) { image[0] = 9; _exit(0); }
    status = child_status(child); CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(madvise(image + PAGE, PAGE, MADV_DONTNEED) == 0 && image[PAGE] == 0 && image[2 * PAGE] == 3);
    char* moved = mremap(image + 8 * PAGE, 2 * PAGE, 2 * PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, (void*)0x500000);
    CHECK(moved == (char*)0x500000 && moved[0] == 9 && moved[PAGE] == 10);
    char* grown = mremap(image + 12 * PAGE, 4 * PAGE, 8 * PAGE, MREMAP_MAYMOVE);
    CHECK(grown != MAP_FAILED && grown[0] == 13 && grown[3 * PAGE] == 16);
    CHECK(mlock(image + 2 * PAGE, 2 * PAGE) == 0 && munlock(image + 2 * PAGE, 2 * PAGE) == 0);           /* NtLockVirtualMemory */
    CHECK(mlock((void*)0x8000, PAGE) == -1 && errno == ENOMEM);
    /* the reservations make the mapped size far larger than RLIMIT_MEMLOCK: only CAP_IPC_LOCK may lock it all */
    CHECK(geteuid() == 0 ? mlockall(MCL_CURRENT) == 0 : mlockall(MCL_CURRENT) == -1 && errno == ENOMEM);
    CHECK(munlockall() == 0);
    unsigned char resident[8]; CHECK(mincore(image, 8 * PAGE, resident) == 0 && mincore(image, 16 * PAGE, resident) == -1 && errno == ENOMEM);
    CHECK(munmap(dos, PAGE) == 0 && maps_has("00010000-00110000 ---p", NULL) == 0);
    CHECK(map(0x10000, PAGE, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE) == (void*)0x10000);
    CHECK(*(volatile char*)0x10000 == 0);                                                    /* fresh zero page */

    /* 5. fork + copy-on-write of a low page; the child sees the same low layout. */
    image[2 * PAGE] = 'A';
    child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(image[2 * PAGE] == 'A' && maps_has("00400000-00401000 r--p", NULL) && maps_has("7ffe0000-7ffe1000 r--s", NULL));
        image[2 * PAGE] = 'B'; CHECK(image[2 * PAGE] == 'B' && !memcmp(kusd, "kusd", 4));
        _exit(0);
    }
    CHECK(child_status(child) == 0 && image[2 * PAGE] == 'A');

    /* 6. MAP_32BIT: below 2 GiB (Linux: [1 GiB, 2 GiB)), even with a hint above it. */
    for (unsigned i = 0; i < 8; i++) {
        char* low = mmap(i & 1 ? (void*)0x300000000ULL : NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        CHECK(low != MAP_FAILED && (uintptr_t)low >= 0x10000 && (uintptr_t)low + (1 << 20) <= 0x80000000ULL);
        low[0] = (char)i; low[(1 << 20) - 1] = (char)i;
    }
    char* ordinary = map(0, PAGE, PROT_READ | PROT_WRITE, 0); /* default placement stays above 4 GiB */
    CHECK(ordinary != MAP_FAILED && (uintptr_t)ordinary > 0xFFFFFFFFULL && munmap(ordinary, PAGE) == 0);

    /* 7. Signal frames on a low alternate stack; fault addresses inside the low reservation. */
    char* alternate = map(0x600000, 16 * PAGE, PROT_READ | PROT_WRITE, MAP_FIXED); CHECK(alternate == (char*)0x600000);
    stack_t stack = {.ss_sp = alternate, .ss_size = 16 * PAGE, .ss_flags = 0}; CHECK(sigaltstack(&stack, NULL) == 0);
    struct sigaction action = {0}; action.sa_handler = on_usr1; action.sa_flags = SA_ONSTACK; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0 && raise(SIGUSR1) == 0);
    CHECK(handler_stack >= 0x600000 && handler_stack < 0x610000);
    struct sigaction segv = {0}; segv.sa_sigaction = on_segv; segv.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&segv.sa_mask);
    CHECK(sigaction(SIGSEGV, &segv, NULL) == 0);
    if (!sigsetjmp(recover, 1)) { *(volatile char*)0x700000 = 1; CHECK(0); }
    CHECK(fault_address == 0x700000);
    if (!sigsetjmp(recover, 1)) { (void)*(volatile char*)0x8000; CHECK(0); }
    CHECK(fault_address == 0x8000);
    signal(SIGSEGV, SIG_DFL);

    /* 8. Non-PIE executables at their link addresses: glibc ET_EXEC at 0x400000, static ET_EXEC at 0x10000. */
    const char* programs[] = {"/bin/memlayout-exec64", "/bin/memlayout-low64"};
    for (unsigned i = 0; i < 2; i++) {
        child = fork(); CHECK(child >= 0);
        if (!child) { execl(programs[i], programs[i] + 5, (char*)NULL); _exit(98); }
        CHECK(child_status(child) == 0);
    }

    CHECK(munmap((void*)0x10000, 0x67ff0000) == 0 && munmap((void*)0x7f000000, 0xff0000) == 0 && munmap((void*)0x7ffffe000000, 0x1ff0000) == 0);
    CHECK(!maps_has("00400000-", NULL) && !maps_has("7ffe0000-", NULL));
    close(fd);
    dprintf(1, "[64] MEMLAYOUT ALL PASSED: user space from 0x10000, mmap_min_addr EPERM, NULL faults, Wine preloader reservations, "
               "KUSER_SHARED_DATA 0x7ffe0000, low mprotect/madvise/mremap/munmap, fork COW, MAP_32BIT, low signal frames, "
               "ET_EXEC at 0x400000 and 0x10000\n");
}

/* More than 5 GiB of anonymous memory touched by one process (RAM above 4 GiB in use). */
static void big(void) {
    FILE* info = fopen("/proc/meminfo", "r"); CHECK(info);
    unsigned long total = 0; char line[128];
    while (fgets(line, sizeof(line), info)) if (sscanf(line, "MemTotal: %lu kB", &total) == 1) break;
    fclose(info); CHECK(total >= 7UL * 1024 * 1024);
    const size_t size = 5632UL << 20, pages = size / PAGE;                                   /* 5.5 GiB */
    uint64_t* memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(memory != MAP_FAILED);
    for (size_t p = 0; p < pages; p++) memory[p * (PAGE / 8)] = p * 0x9E3779B97F4A7C15ULL;
    uint64_t sum = 0;
    for (size_t p = 0; p < pages; p++) { CHECK(memory[p * (PAGE / 8)] == p * 0x9E3779B97F4A7C15ULL); sum += memory[p * (PAGE / 8) + 1]; }
    CHECK(sum == 0);
    CHECK(munmap(memory, size) == 0);
    dprintf(1, "[64] MEMLAYOUT BIG PASS: one process touched %lu MiB (MemTotal %lu MiB)\n", (unsigned long)(size >> 20), total >> 10);
}

int main(int argc, char** argv) {
    CHECK(argc == 2 && (argv[1][0] == '0' || argv[1][0] == 'b' || argv[1][0] == 'a'));
    if (argv[1][0] != 'b') layout();
    if (argv[1][0] != '0') big();
    return 0;
}
#endif
#endif
