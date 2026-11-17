/* WoW64 CPU-mode checks (tests/test_x64_wow32.py): Linux selectors, arch_prctl, modify_ldt, LDT/TLS
 * selector loads and context switches, int $0x80, sigreturn selector validation, and - when user
 * memory below 4 GiB exists - 32-bit compatibility-mode execution: far transfers with DS/ES/FS
 * segments, faults caught by a 64-bit SIGSEGV handler and resumed, int $0x80/SYSCALL/SYSENTER from
 * 32-bit code, preemption and signals on several CPUs, and a stale LDT code segment at iretq.
 * Usage: wow32 0 (everything), wow32 e (exec probe, started by the test itself). */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define LDT_SEL(n) ((uint16_t)((n) << 3 | 7))
#define TLS_SEL(n) ((uint16_t)((n) << 3 | 3))
#define USER32_CS 0x23
#define USER_DS 0x2B
#define USER_CS 0x33
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004
enum { UD_32BIT = 1, UD_EXPAND_DOWN = 2, UD_CODE = 4, UD_READ_EXEC_ONLY = 8, UD_PAGES = 16, UD_NOT_PRESENT = 32, UD_USEABLE = 64 };
struct user_desc32 { uint32_t entry_number, base_addr, limit, flags; };

static int failures, group_start;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("[64] WOW32 FAIL line %d: %s\n", __LINE__, #cond); fflush(stdout); } } while (0)
static void group(void) { group_start = failures; }
static void pass(const char* what) {
    printf(failures == group_start ? "[64] WOW32 PASS: %s\n" : "[64] WOW32 GROUP FAILED: %s\n", what); fflush(stdout);
}

/* Raw system calls: some tests run with FS pointing away from glibc's thread block. */
static inline long sys3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r;
}
static inline long int80(long n, long b, long c, long d) {
    long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(b), "c"(c), "d"(d) : "memory"); return r;
}
static unsigned long modify_ldt_raw(long function, void* address, unsigned long bytes) { return (unsigned long)sys3(154, function, (long)address, (long)bytes); }
static int ldt_set(unsigned entry, uint32_t base, uint32_t limit, unsigned flags) {
    struct user_desc32 d = {entry, base, limit, flags}; return (int)modify_ldt_raw(0x11, &d, sizeof(d));
}
static int ldt_clear(unsigned entry) { return ldt_set(entry, 0, 0, UD_READ_EXEC_ONLY | UD_NOT_PRESENT); }
static long arch_prctl_raw(long code, unsigned long argument) { return sys3(158, code, (long)argument, 0); }
static uint64_t base_of(int fs) { uint64_t v = 0xDEAD; arch_prctl_raw(fs ? ARCH_GET_FS : ARCH_GET_GS, (unsigned long)&v); return v; }
static uint64_t ldt_entry(unsigned entry) {
    static uint64_t table[64]; memset(table, 0xAA, sizeof(table));
    if ((int)modify_ldt_raw(0, table, sizeof(table)) != (int)sizeof(table)) return 0xFFFFFFFFFFFFFFFFULL;
    return table[entry];
}
#define READ_SEGMENT(name) static inline uint16_t read_##name(void) { uint16_t v; __asm__ volatile("mov %%" #name ",%0" : "=r"(v)); return v; }
READ_SEGMENT(cs) READ_SEGMENT(ss) READ_SEGMENT(ds) READ_SEGMENT(es) READ_SEGMENT(fs) READ_SEGMENT(gs)
static inline void load_ds(uint32_t s) { __asm__ volatile("mov %0,%%ds" :: "r"(s) : "memory"); }
static inline void load_es(uint32_t s) { __asm__ volatile("mov %0,%%es" :: "r"(s) : "memory"); }
static inline void load_gs(uint32_t s) { __asm__ volatile("mov %0,%%gs" :: "r"(s) : "memory"); }
static inline uint32_t gs_read32(uint64_t offset) { uint32_t v; __asm__ volatile("movl %%gs:(%1),%0" : "=r"(v) : "r"(offset) : "memory"); return v; }

/* ---- fault recording: the handler never touches glibc thread state (FS may hold a 32-bit selector) ---- */
struct fault { int sig; uint64_t trap, err, rip, uc_flags; uint16_t cs, ss, handler_cs, handler_ss, handler_ds, handler_fs; };
static struct fault faults[16];
static volatile int fault_count, fault_armed, fault_fix_ss;
static void on_fault(int sig, siginfo_t* info, void* context) {
    (void)info;
    ucontext_t* uc = context; greg_t* g = uc->uc_mcontext.gregs;
    if (!fault_armed) {
        static const char m[] = "[64] WOW32 FAIL: unexpected fault signal\n";
        sys3(1, 1, (long)m, sizeof(m) - 1); sys3(231, 80 + sig, 0, 0);
    }
    int k = fault_count < 16 ? fault_count : 15;
    struct fault* f = &faults[k];
    f->sig = sig; f->trap = (uint64_t)g[REG_TRAPNO]; f->err = (uint64_t)g[REG_ERR]; f->rip = (uint64_t)g[REG_RIP];
    f->cs = (uint16_t)g[REG_CSGSFS]; f->ss = (uint16_t)((uint64_t)g[REG_CSGSFS] >> 48); f->uc_flags = uc->uc_flags;
    f->handler_cs = read_cs(); f->handler_ss = read_ss(); f->handler_ds = read_ds(); f->handler_fs = read_fs();
    const uint8_t* ip = (const uint8_t*)g[REG_RIP];
    /* Resume after the faulting instruction (#OF and #BP are traps: RIP is already past it). */
    if (f->trap == 4 || f->trap == 3) {}
    else if (fault_fix_ss) g[REG_CSGSFS] = (greg_t)(((uint64_t)g[REG_CSGSFS] & 0x0000FFFFFFFFFFFFULL) | ((uint64_t)USER_DS << 48));
    else if (ip[0] == 0x8B && ip[1] == 0x03) { g[REG_RIP] += 2; g[REG_RAX] = 0x5EC5E6; } /* mov (%ebx),%eax */
    else if (ip[0] == 0x0F && ip[1] == 0x05) { g[REG_RIP] += 2; g[REG_RAX] = (greg_t)0xFFFFFFDAU; } /* SYSCALL: #UD (Intel) */
    else if ((ip[0] == 0x0F && (ip[1] == 0x0B || ip[1] == 0x34)) || ip[0] == 0x8E) g[REG_RIP] += 2; /* ud2, sysenter, mov sreg */
    __atomic_store_n(&fault_count, fault_count + 1, __ATOMIC_RELEASE);
    fault_armed = fault_count < 16;
}
static void arm(void) { memset(faults, 0, sizeof(faults)); fault_count = 0; fault_fix_ss = 0; fault_armed = 1; }
static void disarm(void) { fault_armed = 0; }
/* mov %eax,<segment> as a 2-byte instruction; returns the signal it raised (0: loaded). */
static int try_load(int segment, uint32_t selector) {
    arm();
    switch (segment) {
    case 0: __asm__ volatile(".byte 0x8e, 0xc0" :: "a"(selector) : "memory"); break; /* es */
    case 2: __asm__ volatile(".byte 0x8e, 0xd0" :: "a"(selector) : "memory"); break; /* ss */
    case 3: __asm__ volatile(".byte 0x8e, 0xd8" :: "a"(selector) : "memory"); break; /* ds */
    case 4: __asm__ volatile(".byte 0x8e, 0xe0" :: "a"(selector) : "memory"); break; /* fs */
    case 5: __asm__ volatile(".byte 0x8e, 0xe8" :: "a"(selector) : "memory"); break; /* gs */
    }
    disarm();
    return fault_count ? faults[0].sig : 0;
}
static void install(int sig, void (*handler)(int, siginfo_t*, void*)) {
    struct sigaction a; memset(&a, 0, sizeof(a)); a.sa_sigaction = handler; a.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&a.sa_mask);
    sigaction(sig, &a, NULL);
}

/* ---- 64-bit checks (always run) ---- */
static void test_selectors(void) {
    group();
    CHECK(read_cs() == USER_CS); CHECK(read_ss() == USER_DS);
    CHECK(read_ds() == 0 && read_es() == 0 && read_fs() == 0 && read_gs() == 0);
    pass("Linux user selectors: CS 0x33, SS 0x2b, null DS/ES/FS/GS");
}
static void test_arch_prctl(void) {
    group();
    CHECK(base_of(1) == (uint64_t)pthread_self());
    CHECK(arch_prctl_raw(ARCH_SET_GS, 0x123456789000ULL) == 0 && base_of(0) == 0x123456789000ULL && read_gs() == 0);
    CHECK(arch_prctl_raw(ARCH_SET_GS, 0x7FFFFFFFF000ULL) == -EPERM);
    CHECK(arch_prctl_raw(ARCH_SET_GS, 0x7FFFFFFFEFFFULL) == 0 && base_of(0) == 0x7FFFFFFFEFFFULL);
    CHECK(arch_prctl_raw(ARCH_SET_GS, 0x10000) == 0 && base_of(0) == 0x10000); /* below 4 GiB: Wine's WoW64 TEB */
    CHECK(arch_prctl_raw(ARCH_GET_FS, 8) == -EFAULT);
    CHECK(arch_prctl_raw(0x1011, 0) == 1);          /* ARCH_GET_CPUID */
    CHECK(arch_prctl_raw(0x1012, 1) == -ENODEV);    /* ARCH_SET_CPUID: no CPUID faulting */
    CHECK(arch_prctl_raw(0x9999, 0) == -EINVAL);
    CHECK(arch_prctl_raw(ARCH_SET_GS, 0) == 0 && base_of(0) == 0);
    pass("arch_prctl: SET/GET FS/GS (TASK_SIZE_MAX limit), GET/SET_CPUID, errors");
}
static void test_modify_ldt(void) {
    group();
    uint8_t buffer[256]; memset(buffer, 0xAA, sizeof(buffer));
    CHECK(modify_ldt_raw(0, buffer, sizeof(buffer)) == 0 && buffer[0] == 0xAA);              /* no LDT yet */
    CHECK(modify_ldt_raw(2, buffer, 200) == 128 && buffer[0] == 0 && buffer[127] == 0 && buffer[128] == 0xAA);
    CHECK(modify_ldt_raw(7, buffer, 16) == 0xFFFFFFDAUL);                                    /* int result: ENOSYS, not sign-extended */
    struct user_desc32 d = {1, 0x10000, 0xFFFFF, UD_32BIT | UD_PAGES};
    CHECK((int)modify_ldt_raw(0x11, &d, 15) == -EINVAL);
    CHECK((int)modify_ldt_raw(0x11, (void*)16, 16) == -EFAULT);
    CHECK(ldt_set(8192, 0, 0xFFFFF, UD_32BIT) == -EINVAL);
    d.flags = UD_32BIT | UD_CODE | UD_EXPAND_DOWN; CHECK((int)modify_ldt_raw(1, &d, 16) == -EINVAL);      /* conforming, old mode */
    CHECK((int)modify_ldt_raw(0x11, &d, 16) == -EINVAL);                                              /* conforming and present */
    CHECK(ldt_set(2, 1, 1, 0) == -EINVAL);                                                            /* 16-bit: no espfix64 */
    CHECK(ldt_set(8191, 0x12345000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    static uint64_t table[8192 + 64];
    CHECK((int)modify_ldt_raw(0, table, 70000) == 65536 && table[8191] == 0x12CFF3345000FFFFULL && table[0] == 0);
    CHECK(ldt_set(1, 0x10000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0 && ldt_entry(1) == 0x00CFF3010000FFFFULL);
    CHECK(ldt_set(5, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_CODE) == 0 && ldt_entry(5) == 0x00CFFB000000FFFFULL);
    CHECK(ldt_set(6, 0x8000, 0xFFF, UD_32BIT | UD_EXPAND_DOWN) == 0 && ldt_entry(6) == 0x0040F70080000FFFULL);
    CHECK(ldt_set(7, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_READ_EXEC_ONLY | UD_USEABLE) == 0 && ldt_entry(7) == 0x00DFF1000000FFFFULL);
    CHECK(ldt_set(8, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_NOT_PRESENT) == 0 && ldt_entry(8) == 0x00CF73000000FFFFULL);
    d = (struct user_desc32){7, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_USEABLE};
    CHECK((int)modify_ldt_raw(1, &d, 16) == 0 && ldt_entry(7) == 0x00CFF3000000FFFFULL);              /* old mode drops AVL */
    d = (struct user_desc32){7, 0, 0, UD_32BIT | UD_PAGES};
    CHECK((int)modify_ldt_raw(1, &d, 16) == 0 && ldt_entry(7) == 0);                                  /* old mode: base 0, limit 0 clears */
    CHECK(ldt_clear(8) == 0 && ldt_entry(8) == 0 && ldt_clear(8191) == 0);                            /* LDT_empty clears */
    pass("modify_ldt: read/default/write (old and new mode), encodings, 8192-entry limit, EINVAL/EFAULT/ENOSYS");
}
static void test_selector_loads(void) {
    group();
    static uint32_t probe = 0xFEEDF00D;
    CHECK(ldt_set(1, 0x10000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    load_gs(LDT_SEL(1));
    CHECK(read_gs() == LDT_SEL(1) && base_of(0) == 0x10000 && gs_read32((uint64_t)&probe - 0x10000) == 0xFEEDF00D);
    load_ds(LDT_SEL(1)); load_es(LDT_SEL(1));
    CHECK(read_ds() == LDT_SEL(1) && read_es() == LDT_SEL(1));
    CHECK(sys3(39, 0, 0, 0) == getpid() && sys3(24, 0, 0, 0) == 0);                                  /* getpid, sched_yield */
    struct timespec pause = {0, 3000000}; nanosleep(&pause, NULL);
    CHECK(read_ds() == LDT_SEL(1) && read_es() == LDT_SEL(1) && read_gs() == LDT_SEL(1) && base_of(0) == 0x10000);
    CHECK(try_load(2, LDT_SEL(1)) == 0 && read_ss() == LDT_SEL(1));                                   /* SS from the LDT */
    sys3(39, 0, 0, 0); CHECK(read_ss() == USER_DS);                                                   /* SYSCALL returns with SS 0x2b */
    CHECK(try_load(3, USER32_CS) == 0 && read_ds() == USER32_CS);                                     /* readable code into DS */
    CHECK(try_load(3, 0x18) == SIGSEGV && faults[0].trap == 13 && faults[0].err == 0x18);             /* kernel data */
    CHECK(try_load(0, 0x10) == SIGSEGV && faults[0].err == 0x10);                                     /* kernel code */
    CHECK(try_load(5, TLS_SEL(12)) == SIGSEGV && faults[0].err == 0x60);                              /* empty TLS slot */
    CHECK(try_load(5, 0x53) == SIGSEGV && faults[0].err == 0x50);                                     /* LDT system descriptor */
    CHECK(try_load(4, 0x83) == SIGSEGV && faults[0].err == 0x80);                                     /* a TSS */
    CHECK(try_load(2, USER32_CS) == SIGSEGV && faults[0].trap == 13);                                 /* code into SS */
    CHECK(ldt_set(9, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_NOT_PRESENT) == 0);
    CHECK(try_load(5, LDT_SEL(9)) == SIGBUS && faults[0].trap == 11 && faults[0].err == (LDT_SEL(9) & ~3u)); /* #NP */
    CHECK(faults[0].cs == USER_CS && faults[0].handler_cs == USER_CS && faults[0].handler_ss == USER_DS && (faults[0].uc_flags & 6) == 6);
    load_ds(0); load_es(0); arch_prctl_raw(ARCH_SET_GS, 0);
    CHECK(read_ds() == 0 && read_gs() == 0 && base_of(0) == 0);
    pass("64-bit selector loads: LDT data in DS/ES/GS/SS (base from the descriptor), kept across syscalls and sleeps; Linux faults (#GP/#NP)");
}
static void test_refresh_local(void) {
    group();
    CHECK(ldt_set(2, 0x2000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0 && ldt_set(3, 0x3000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    load_ds(LDT_SEL(2)); load_es(LDT_SEL(3));
    CHECK(ldt_set(2, 0x4000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0 && read_ds() == LDT_SEL(2));         /* changed entry: kept */
    CHECK(ldt_clear(2) == 0 && read_ds() == 0 && read_es() == LDT_SEL(3));                            /* deleted entry: null */
    CHECK(ldt_clear(3) == 0 && read_es() == 0);
    pass("modify_ldt refreshes this CPU's DS/ES (deleted entry -> null)");
}
static volatile int remote_phase, remote_ready, remote_bad; static volatile long remote_loops;
static void* remote_worker(void* unused) {
    (void)unused;
    load_ds(LDT_SEL(4)); load_es(LDT_SEL(4)); remote_ready = 1;
    for (;;) {
        int phase = remote_phase; uint16_t ds = read_ds(), es = read_es();
        if (phase >= 1) { remote_loops++; if (ds || es) remote_bad++; }
        if (phase == 2) break;
    }
    return NULL;
}
static void test_refresh_remote(void) {
    group();
    CHECK(ldt_set(4, 0, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    remote_phase = remote_ready = remote_bad = 0; remote_loops = 0;
    pthread_t t; CHECK(pthread_create(&t, NULL, remote_worker, NULL) == 0);
    while (!remote_ready) sched_yield();
    struct timespec pause = {0, 20000000}; nanosleep(&pause, NULL);
    CHECK(ldt_clear(4) == 0);
    remote_phase = 1;          /* modify_ldt has returned: the worker's DS/ES must already be null */
    nanosleep(&pause, NULL);
    remote_phase = 2; pthread_join(t, NULL);
    CHECK(remote_bad == 0 && remote_loops > 0);
    pass("modify_ldt refreshes DS/ES on the other CPUs running the address space before it returns");
}
/* A thread sits on an LDT stack segment; another thread deletes it. The next iretq to that thread
 * faults in the kernel and must come back as a user signal (the handler swaps SS for 0x2b). */
static volatile int stale_ready;
static void* stale_ss_worker(void* unused) {
    (void)unused;
    arm(); fault_fix_ss = 1;
    __asm__ volatile("mov %0,%%ss" :: "r"((uint32_t)LDT_SEL(10)) : "memory");
    stale_ready = 1;
    while (read_ss() == LDT_SEL(10)) __asm__ volatile("pause");
    disarm();
    return NULL;
}
static void test_stale_ss(void) {
    group();
    CHECK(ldt_set(10, 0, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    stale_ready = 0;
    pthread_t t; CHECK(pthread_create(&t, NULL, stale_ss_worker, NULL) == 0);
    while (!stale_ready) sched_yield();
    struct timespec pause = {0, 20000000}; nanosleep(&pause, NULL);
    CHECK(ldt_clear(10) == 0);
    pthread_join(t, NULL);
    CHECK(fault_count == 1 && (faults[0].sig == SIGSEGV || faults[0].sig == SIGBUS) && faults[0].ss == LDT_SEL(10) && faults[0].cs == USER_CS);
    pass("iretq onto a deleted LDT stack segment becomes a user SIGSEGV (no kernel fault)");
}
/* ---- sigreturn: forged contexts in children ---- */
static char forge_mode;
static void forge(int sig, siginfo_t* info, void* context) {
    (void)sig; (void)info;
    ucontext_t* uc = context; uint64_t* csgsfs = (uint64_t*)&uc->uc_mcontext.gregs[REG_CSGSFS];
    uint64_t ldt_code = LDT_SEL(5), value = *csgsfs;
    switch (forge_mode) {
    case 'k': value = (value & ~0xFFFFULL) | 0x10; break;            /* kernel code64 */
    case '8': value = (value & ~0xFFFFULL) | 0x08; break;            /* kernel code32 */
    case 'K': value = (value & ~0xFFFFULL) | 0x13; break;            /* kernel code64, RPL 3 */
    case 'd': value = (value & ~0xFFFFULL) | USER_DS; break;         /* data as code */
    case 's': value = (value & 0x0000FFFFFFFFFFFFULL) | (0x18ULL << 48); break;                          /* kernel SS, strict */
    case 'S': value = (value & 0x0000FFFFFFFFFFFFULL) | (0x18ULL << 48); uc->uc_flags &= ~4UL; break;    /* not strict: forced 0x2b */
    case 'f': value = (value & 0xFFFF00000000FFFFULL) | (0x1234ULL << 16) | (0x5678ULL << 32); break;    /* fs/gs words ignored */
    case 'r': value = (value & ~0xFFFFULL) | USER32_CS; uc->uc_mcontext.gregs[REG_RIP] |= 0x100000000LL; break; /* 32-bit code above 4 GiB */
    case 'l': value = (value & ~0xFFFFULL) | ldt_code; uc->uc_mcontext.gregs[REG_RIP] |= 0x100000000LL; break;  /* LDT code, RIP above 4 GiB */
    case 'n': value = (value & ~0xFFFFULL) | LDT_SEL(1); break;      /* LDT data as code */
    case 'p': value = (value & ~0xFFFFULL) | LDT_SEL(8); break;      /* not-present LDT code */
    case '3': value = (value & ~0xFFFFULL) | 0x30; break;            /* user code64 with RPL 0: forced to 3 */
    }
    *csgsfs = value;
}
static int forge_child(char mode) {
    pid_t child = fork();
    if (!child) {
        forge_mode = mode;
        ldt_set(1, 0, 0xFFFFF, UD_32BIT | UD_PAGES); ldt_set(5, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_CODE);
        ldt_set(8, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_CODE | UD_NOT_PRESENT);
        install(SIGUSR1, forge); raise(SIGUSR1);
        _exit(read_ss() == USER_DS && read_cs() == USER_CS ? 0 : 5);
    }
    int status = -1; waitpid(child, &status, 0);
    return status;
}
static void test_sigreturn(void) {
    group();
    const char killed[] = "k8Kdsrlnp";
    for (const char* m = killed; *m; m++) {
        int status = forge_child(*m);
        if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV)) { printf("[64] WOW32 sigreturn mode %c status %#x\n", *m, status); CHECK(0); }
    }
    CHECK(forge_child('S') == 0 && forge_child('f') == 0 && forge_child('3') == 0);
    pass("sigreturn accepts 0x33 (RPL forced), ignores fs/gs words, repairs a non-strict SS; refuses kernel/data/stale code and 32-bit RIP above 4 GiB");
}
static void test_fork_threads(void) {
    group();
    CHECK(ldt_set(1, 0x10000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0 && ldt_set(11, 0x11000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    load_gs(LDT_SEL(1)); load_ds(LDT_SEL(11));
    pid_t child = fork();
    if (!child) {
        int bad = read_gs() != LDT_SEL(1) || read_ds() != LDT_SEL(11) || base_of(0) != 0x10000 || ldt_entry(11) != 0x00CFF3011000FFFFULL;
        bad |= ldt_set(1, 0x20000, 0xFFFFF, UD_32BIT | UD_PAGES) != 0 || ldt_entry(1) != 0x00CFF3020000FFFFULL;
        load_gs(LDT_SEL(1)); bad |= base_of(0) != 0x20000;
        _exit(bad ? 1 : 0);
    }
    int status = -1; waitpid(child, &status, 0);
    CHECK(status == 0 && ldt_entry(1) == 0x00CFF3010000FFFFULL);
    load_gs(LDT_SEL(1)); CHECK(base_of(0) == 0x10000 && read_ds() == LDT_SEL(11));
    load_ds(0); arch_prctl_raw(ARCH_SET_GS, 0);
    pass("fork copies the LDT and DS/GS selectors; the child's LDT changes stay private");
}
static volatile int thread_result;
static void* ldt_thread(void* unused) {
    (void)unused;
    int bad = read_gs() != LDT_SEL(1) || read_fs() != 0 || base_of(1) != (uint64_t)pthread_self();   /* clone(CLONE_SETTLS) */
    bad |= ldt_set(12, 0x30000, 0xFFFFF, UD_32BIT | UD_PAGES) != 0;
    thread_result = bad ? 1 : 2;
    return NULL;
}
static void test_threads(void) {
    group();
    load_gs(LDT_SEL(1));
    pthread_t t; CHECK(pthread_create(&t, NULL, ldt_thread, NULL) == 0); pthread_join(t, NULL);
    CHECK(thread_result == 2 && ldt_entry(12) == 0x00CFF3030000FFFFULL);
    load_gs(LDT_SEL(12)); CHECK(base_of(0) == 0x30000);
    arch_prctl_raw(ARCH_SET_GS, 0);
    pass("threads share one LDT; clone copies DS/ES/GS (CLONE_SETTLS: null FS, new base)");
}
static uint8_t* low; /* 1 MiB of user memory below 4 GiB, if the kernel provides it */
static void test_exec(const char* self) {
    group();
    CHECK(ldt_set(1, 0x10000, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    pid_t child = fork();
    if (!child) {
        load_gs(LDT_SEL(1)); load_ds(LDT_SEL(1));
        if (low) { struct user_desc32* d = (void*)(low + 0x11000); *d = (struct user_desc32){12, 0x1000, 0xFFFFF, UD_32BIT}; int80(243, (long)d, 0, 0); }
        char* argv[] = {(char*)self, "e", NULL}; execv(self, argv); _exit(9);
    }
    int status = -1; waitpid(child, &status, 0);
    CHECK(status == 0);
    pass("execve drops the LDT, TLS slots and selectors");
}
static int exec_probe(void) {
    uint8_t buffer[64]; memset(buffer, 0xAA, sizeof(buffer));
    int bad = modify_ldt_raw(0, buffer, sizeof(buffer)) != 0 || read_ds() || read_es() || read_gs() || read_fs() || base_of(0);
    void* want = (void*)0x10000000;
    void* p = mmap(want, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == want) {
        struct user_desc32* d = p; *d = (struct user_desc32){12, 0, 0, 0};
        bad |= int80(244, (long)d, 0, 0) != 0 || d->base_addr || d->flags != (UD_READ_EXEC_ONLY | UD_NOT_PRESENT);
    }
    return bad;
}
static void test_int80_64(void) {
    group();
    CHECK(int80(20, 0, 0, 0) == getpid() && int80(224, 0, 0, 0) == gettid());
    CHECK(int80(0xFFFF, 0, 0, 0) == -ENOSYS && int80(243, 0, 0, 0) == -EFAULT && int80(244, 0, 0, 0) == -EFAULT);
    static struct user_desc32 high = {0xFFFFFFFFU, 0, 0xFFFFF, UD_32BIT};
    if ((uint64_t)&high >= 0x100000000ULL) CHECK(int80(243, (long)&high, 0, 0) != 0);                /* EBX is a 32-bit pointer */
    CHECK(int80(123, 0, 0, 0) == 0);                                                                   /* modify_ldt(read, NULL, 0) */
    uint64_t in[14] = {0x1111111111111111, 0x2222222222222222, 0x3333333333333333, 0x4444444444444444, 0x5555555555555555,
                       0x6666666666666666, 0x7777777777777777, 0x8888888888888888, 0x9999999999999999, 0xAAAAAAAAAAAAAAAA,
                       0xBBBBBBBBBBBBBBBB, 0xCCCCCCCCCCCCCCCC, 0xDDDDDDDDDDDDDDDD, 0}, out[14];
    in[13] = (uint64_t)in; /* RSI */
    __asm__ volatile(
        "push %%rbx; push %%rbp; push %%r12; push %%r13; push %%r14; push %%r15; push %%rdi\n\t"
        "mov 8(%%rsi),%%rcx; mov 16(%%rsi),%%rdx; mov 24(%%rsi),%%rbp; mov 32(%%rsi),%%r8; mov 40(%%rsi),%%r9\n\t"
        "mov 48(%%rsi),%%r10; mov 56(%%rsi),%%r11; mov 64(%%rsi),%%r12; mov 72(%%rsi),%%r13; mov 80(%%rsi),%%r14\n\t"
        "mov 88(%%rsi),%%r15; mov 96(%%rsi),%%rdi; mov (%%rsi),%%rbx; mov $20,%%eax\n\t"
        "int $0x80\n\t"
        "push %%rdi; mov 8(%%rsp),%%rdi\n\t"
        "mov %%rbx,(%%rdi); mov %%rcx,8(%%rdi); mov %%rdx,16(%%rdi); mov %%rbp,24(%%rdi); mov %%r8,32(%%rdi); mov %%r9,40(%%rdi)\n\t"
        "mov %%r10,48(%%rdi); mov %%r11,56(%%rdi); mov %%r12,64(%%rdi); mov %%r13,72(%%rdi); mov %%r14,80(%%rdi); mov %%r15,88(%%rdi)\n\t"
        "pop %%rax; mov %%rax,96(%%rdi); mov %%rsi,104(%%rdi)\n\t"
        "pop %%rdi; pop %%r15; pop %%r14; pop %%r13; pop %%r12; pop %%rbp; pop %%rbx"
        : : "S"(in), "D"(out) : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    CHECK(!memcmp(in, out, sizeof(in)));
    for (long number = 252; number >= 1; number -= 251) {               /* exit_group, exit */
        pid_t child = fork();
        if (!child) { int80(number, number == 252 ? 7 : 9, 0, 0); _exit(1); }
        int status = -1; waitpid(child, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (number == 252 ? 7 : 9));
    }
    install(SIGSEGV, on_fault); install(SIGILL, on_fault); install(SIGBUS, on_fault);
    arm(); __asm__ volatile("sysenter" ::: "memory"); disarm();
    CHECK(fault_count == 1 && (faults[0].sig == SIGSEGV || faults[0].sig == SIGILL));
    pass("int $0x80 from 64-bit code: IA-32 getpid/gettid/exit/exit_group, ENOSYS/EFAULT, all registers preserved; SYSENTER disabled");
}

/* ---- 32-bit compatibility mode (needs user memory below 4 GiB) ---- */
struct ctx32 {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, cs, ss, ds, es, fs, gs, pad;
    uint64_t fs_base, reserved;
    uint32_t out_ds, out_es, out_fs, out_gs, out_ss, out_cs;
};
_Static_assert(__builtin_offsetof(struct ctx32, fs_base) == 64 && __builtin_offsetof(struct ctx32, out_ds) == 80, "ctx32 layout");
void call32(struct ctx32* ctx);
extern const char wow32_blob[], wow32_back64[], wow32_basic[], wow32_fault[], wow32_calls[], wow32_spin[], wow32_park[], wow32_blob_end[];
/* call32: load the 32-bit segment registers, iretq to CS:EIP on SS:ESP. The 32-bit code comes back
 * with a far return (or far jump) to wow32_back64 (CS 0x33, below 4 GiB), which jumps to
 * call32_return through r15; r12/r13 (stack, context) survive compatibility mode. */
__asm__(".text\n.globl call32\ncall32:\n\t"
        "push %rbx; push %rbp; push %r12; push %r13; push %r14; push %r15\n\t"
        "mov %rsp,%r12; mov %rdi,%r13; lea call32_return(%rip),%r15\n\t"
        "mov 44(%r13),%eax; mov %eax,%ds\n\t"
        "mov 48(%r13),%eax; mov %eax,%es\n\t"
        "mov 52(%r13),%eax; mov %eax,%fs\n\t"
        "mov 56(%r13),%eax; mov %eax,%gs\n\t"
        "mov 40(%r13),%eax; push %rax\n\t"
        "mov 28(%r13),%eax; push %rax\n\t"
        "pushfq\n\t"
        "mov 36(%r13),%eax; push %rax\n\t"
        "mov 32(%r13),%eax; push %rax\n\t"
        "mov (%r13),%eax; mov 4(%r13),%ebx; mov 8(%r13),%ecx; mov 12(%r13),%edx\n\t"
        "mov 16(%r13),%esi; mov 20(%r13),%edi; mov 24(%r13),%ebp\n\t"
        "iretq\n"
        "call32_return:\n\t"
        "mov %eax,(%r13); mov %ebx,4(%r13); mov %ecx,8(%r13); mov %edx,12(%r13)\n\t"
        "mov %esi,16(%r13); mov %edi,20(%r13); mov %ebp,24(%r13); mov %esp,28(%r13)\n\t"
        "mov %ds,%eax; mov %eax,80(%r13); mov %es,%eax; mov %eax,84(%r13)\n\t"
        "mov %fs,%eax; mov %eax,88(%r13); mov %gs,%eax; mov %eax,92(%r13)\n\t"
        "mov %ss,%eax; mov %eax,96(%r13); mov %cs,%eax; mov %eax,100(%r13)\n\t"
        "mov $0x2b,%eax; mov %eax,%ss; mov %r12,%rsp\n\t"
        "xor %eax,%eax; mov %eax,%ds; mov %eax,%es; mov %eax,%gs\n\t"
        "mov $158,%eax; mov $0x1002,%edi; mov 64(%r13),%rsi; syscall\n\t"   /* ARCH_SET_FS: glibc's thread block */
        "pop %r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx\n\t"
        "ret\n"
        /* Position-independent blob copied below 4 GiB. */
        ".globl wow32_blob, wow32_back64, wow32_basic, wow32_fault, wow32_calls, wow32_spin, wow32_park, wow32_blob_end\n"
        ".p2align 4\nwow32_blob:\n"
        ".code64\nwow32_back64:\n\tjmp *%r15\n"
        ".code32\n"
        /* basic: EBX = data (DS); returns EAX = %fs:0, ECX = (EBX), EDX/ESI/EDI = CS/SS/DS; copies (EBX) to
         * %es:4(EBX); leaves through the far pointer 0x40(EBX) (ljmp m16:32 to 0x33:wow32_back64). */
        "wow32_basic:\n\t"
        "movl %fs:0,%eax\n\tmovl (%ebx),%ecx\n\tmovl %ecx,%es:4(%ebx)\n\t"
        "movl %cs,%edx\n\tmovl %ss,%esi\n\tmovl %ds,%edi\n\t"
        "ljmp *0x40(%ebx)\n"
        /* fault: EBX = an unmapped address. #PF (handler: EAX = 0x5EC5E6, resume), ud2 (#UD), into with OF (#OF). */
        "wow32_fault:\n\t"
        "movl $0x11111111,%eax\n\t"
        ".byte 0x8b, 0x03\n\t"            /* movl (%ebx),%eax */
        "movl %eax,%esi\n\t"
        "ud2\n\t"
        "movl $0x7FFFFFFF,%ecx\n\taddl $1,%ecx\n\tinto\n\t"
        "movl $0x5A5A,%edi\n\t"
        "lret\n"
        /* calls: int $0x80 getpid / unknown / set_thread_area(EBX); %fs from the new slot; SYSCALL; SYSENTER. */
        "wow32_calls:\n\t"
        "movl $20,%eax\n\tint $0x80\n\tmovl %eax,%esi\n\t"
        "movl $0xFFFF,%eax\n\tint $0x80\n\tmovl %eax,%edi\n\t"
        "movl $243,%eax\n\tint $0x80\n\tmovl %eax,%ebp\n\t"
        "movl (%ebx),%eax\n\tshll $3,%eax\n\torl $3,%eax\n\tmovl %eax,%fs\n\tmovl %fs:0,%edx\n\t"
        "movl $20,%eax\n\tsyscall\n\tmovl %eax,%ecx\n\t"
        "sysenter\n\t"
        "lret\n"
        /* spin: patterns in EAX/EBX/EDX/ESI/EDI, EBP = %fs:0, ECX = iterations; 8(%esp) = FS, 12(%esp) = DS = ES. */
        "wow32_spin:\n"
        "1:\tcmpl $0xA5A5A5A5,%eax\n\tjne 9f\n\tcmpl $0x3C3C3C3C,%ebx\n\tjne 9f\n\t"
        "cmpl $0x0F0F0F0F,%edx\n\tjne 9f\n\tcmpl $0x5A5A5A5A,%esi\n\tjne 9f\n\t"
        "cmpl $0xC3C3C3C3,%edi\n\tjne 9f\n\tcmpl %fs:0,%ebp\n\tjne 9f\n\t"
        "pushl %eax\n\t"
        "movl %fs,%eax\n\tcmpl 12(%esp),%eax\n\tjne 8f\n\t"
        "movl %ds,%eax\n\tcmpl 16(%esp),%eax\n\tjne 8f\n\t"
        "movl %es,%eax\n\tcmpl 16(%esp),%eax\n\tjne 8f\n\t"
        "movl %ss,%eax\n\tcmpl $0x2b,%eax\n\tjne 8f\n\t"
        "popl %eax\n\tdecl %ecx\n\tjnz 1b\n\t"
        "movl $0x600D,%ecx\n\tlret\n"
        "8:\tpopl %eax\n"
        "9:\tmovl $0xBAD,%ecx\n\tlret\n"
        /* park: spin forever (killed from outside). */
        "wow32_park:\n"
        "1:\tpause\n\tjmp 1b\n"
        ".code64\n"
        "wow32_blob_end:\n");
static uint32_t low32(const char* label) { return (uint32_t)(uintptr_t)(low + (label - wow32_blob)); }
#define LOW_DATA 0x10000
#define LOW_DESC 0x11000
#define LOW_MARK 0x12000
static void prepare(struct ctx32* c, int slot, const char* entry, uint16_t cs, uint16_t ds, uint16_t fs, uint32_t p0, uint32_t p1) {
    memset(c, 0, sizeof(*c));
    uint32_t* stack = (uint32_t*)(low + 0x20000 + (slot + 1) * 0x8000) - 4;
    stack[0] = low32(wow32_back64); stack[1] = USER_CS; stack[2] = p0; stack[3] = p1;
    c->esp = (uint32_t)(uintptr_t)stack; c->eip = low32(entry); c->cs = cs; c->ss = USER_DS; c->ds = c->es = ds; c->fs = fs;
    c->fs_base = base_of(1);
}
static int low_memory(void) {
    static const uintptr_t candidates[] = {0x10000000, 0x20000000, 0x30000000, 0x40000000, 0x01000000};
    void* p = MAP_FAILED; int error = 0;
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        void* want = (void*)candidates[i];
        p = mmap(want, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (!i) error = errno;
        if (p == want) { low = p; return 1; }
        if (p != MAP_FAILED) munmap(p, 1 << 20);
    }
    p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p != MAP_FAILED && (uintptr_t)p + (1 << 20) <= 0x100000000ULL) { low = p; return 1; }
    if (p != MAP_FAILED) munmap(p, 1 << 20);
    printf("[64] WOW32 SKIP: no user memory below 4 GiB (mmap 0x10000000 errno %d); set_thread_area and 32-bit compatibility-mode tests not run\n", error);
    fflush(stdout);
    return 0;
}
static void test_tls(void) {
    group();
    uint32_t* marks = (uint32_t*)(low + LOW_MARK);
    marks[0] = 0x7E570001; marks[16] = 0x7E570002;
    struct user_desc32* d = (void*)(low + LOW_DESC);
    *d = (struct user_desc32){0xFFFFFFFFU, (uint32_t)(uintptr_t)marks, 0xFFFFFFFFU, UD_32BIT | UD_USEABLE}; /* Wine's alloc_fs_sel */
    CHECK(int80(243, (long)d, 0, 0) == 0 && d->entry_number == 12);
    struct user_desc32* r = d + 1; *r = (struct user_desc32){12, 0, 0, 0};
    CHECK(int80(244, (long)r, 0, 0) == 0 && r->base_addr == (uint32_t)(uintptr_t)marks && r->limit == 0xFFFFF && r->flags == (UD_32BIT | UD_USEABLE));
    load_gs(TLS_SEL(12));
    CHECK(read_gs() == TLS_SEL(12) && base_of(0) == (uint64_t)(uintptr_t)marks && gs_read32(0) == 0x7E570001);
    *d = (struct user_desc32){12, (uint32_t)(uintptr_t)&marks[16], 0xFFFFF, UD_32BIT};
    CHECK(int80(243, (long)d, 0, 0) == 0 && base_of(0) == (uint64_t)(uintptr_t)&marks[16] && gs_read32(0) == 0x7E570002); /* reloaded at once */
    *d = (struct user_desc32){0xFFFFFFFFU, 0, 0xFFFFF, UD_32BIT}; CHECK(int80(243, (long)d, 0, 0) == 0 && d->entry_number == 13);
    *d = (struct user_desc32){0xFFFFFFFFU, 0, 0xFFFFF, UD_32BIT}; CHECK(int80(243, (long)d, 0, 0) == 0 && d->entry_number == 14);
    *d = (struct user_desc32){0xFFFFFFFFU, 0, 0xFFFFF, UD_32BIT}; CHECK(int80(243, (long)d, 0, 0) == -ESRCH);
    *d = (struct user_desc32){11, 0, 0xFFFFF, UD_32BIT}; CHECK(int80(243, (long)d, 0, 0) == -EINVAL);
    *d = (struct user_desc32){15, 0, 0xFFFFF, UD_32BIT}; CHECK(int80(243, (long)d, 0, 0) == -EINVAL);
    *d = (struct user_desc32){13, 0, 0xFFFFF, 0}; CHECK(int80(243, (long)d, 0, 0) == -EINVAL);                      /* 16-bit */
    *d = (struct user_desc32){13, 0, 0xFFFFF, UD_32BIT | UD_CODE}; CHECK(int80(243, (long)d, 0, 0) == -EINVAL);      /* code */
    *d = (struct user_desc32){13, 0, 0xFFFFF, UD_32BIT | UD_NOT_PRESENT}; CHECK(int80(243, (long)d, 0, 0) == -EINVAL);
    *d = (struct user_desc32){13, 0, 0, 0}; CHECK(int80(243, (long)d, 0, 0) == 0);                                   /* zeroed: clear */
    *d = (struct user_desc32){14, 0, 0, UD_READ_EXEC_ONLY | UD_NOT_PRESENT}; CHECK(int80(243, (long)d, 0, 0) == 0); /* LDT_empty: clear */
    *r = (struct user_desc32){13, 1, 1, 1}; CHECK(int80(244, (long)r, 0, 0) == 0 && !r->base_addr && r->flags == (UD_READ_EXEC_ONLY | UD_NOT_PRESENT));
    pid_t child = fork();
    if (!child) { struct user_desc32* q = (void*)(low + LOW_DESC + 64); *q = (struct user_desc32){12, 0, 0, 0}; _exit(int80(244, (long)q, 0, 0) || q->base_addr != (uint32_t)(uintptr_t)&marks[16]); }
    int status = -1; waitpid(child, &status, 0); CHECK(status == 0);                                                  /* fork copies slots */
    arch_prctl_raw(ARCH_SET_GS, 0);
    pass("set_thread_area/get_thread_area through int $0x80: allocation (12), Wine's request, reload of a live selector, ESRCH/EINVAL, clearing, fork");
}
static void test_compat_basic(void) {
    group();
    uint32_t* data = (uint32_t*)(low + LOW_DATA);
    data[0] = 0x0DDBA11; data[1] = 0; data[16] = low32(wow32_back64); data[17] = USER_CS; /* far pointer at 0x40 */
    uint32_t* marks = (uint32_t*)(low + LOW_MARK); marks[32] = 0xF5F5F5F5;
    CHECK(ldt_set(13, (uint32_t)(uintptr_t)&marks[32], 0xFFF, UD_32BIT) == 0);                        /* FS: LDT data, byte limit */
    struct ctx32 c; prepare(&c, 0, wow32_basic, USER32_CS, USER_DS, LDT_SEL(13), 0, 0);
    c.ebx = (uint32_t)(uintptr_t)data;
    call32(&c);
    CHECK(c.eax == 0xF5F5F5F5 && c.ecx == 0x0DDBA11 && data[1] == 0x0DDBA11 && c.edx == USER32_CS && c.esi == USER_DS && c.edi == USER_DS);
    CHECK(c.out_cs == USER_CS && c.out_ds == USER_DS && c.out_fs == LDT_SEL(13) && read_fs() == 0 && base_of(1) == (uint64_t)pthread_self());
    CHECK(ldt_set(14, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_CODE) == 0);                                  /* LDT code segment */
    prepare(&c, 0, wow32_basic, LDT_SEL(14), LDT_SEL(1), TLS_SEL(12), 0, 0);
    CHECK(ldt_set(1, 0, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    c.ebx = (uint32_t)(uintptr_t)data; data[0] = 0xC0DE5E6; call32(&c);
    CHECK(c.edx == LDT_SEL(14) && c.edi == LDT_SEL(1) && c.ecx == 0xC0DE5E6 && c.eax == ((uint32_t*)(low + LOW_MARK))[16]);
    pass("far transfers 64 -> 32 -> 64 bit (iretq, ljmp m16:32, lret) with DS/ES 0x2b or LDT data, FS from the LDT or a TLS slot, CS 0x23 or LDT code");
}
static void test_compat_faults(void) {
    group();
    struct ctx32 c; prepare(&c, 0, wow32_fault, USER32_CS, USER_DS, LDT_SEL(13), 0, 0);
    c.ebx = 0x1000; /* unmapped */
    arm(); call32(&c); disarm();
    CHECK(fault_count == 3 && c.esi == 0x5EC5E6 && c.edi == 0x5A5A);
    CHECK(faults[0].sig == SIGSEGV && faults[0].trap == 14 && faults[0].cs == USER32_CS && faults[0].ss == USER_DS && faults[0].rip < 0x100000000ULL);
    CHECK(faults[1].sig == SIGILL && faults[1].trap == 6 && faults[2].sig == SIGSEGV && faults[2].trap == 4 && faults[2].cs == USER32_CS);
    for (int i = 0; i < 3; i++) CHECK(faults[i].handler_cs == USER_CS && faults[i].handler_ss == USER_DS && faults[i].handler_ds == USER_DS &&
                                      faults[i].handler_fs == LDT_SEL(13) && (faults[i].uc_flags & 6) == 2);
    pass("faults in 32-bit code (#PF, #UD, #OF) reach a 64-bit handler (CS 0x33) with the 32-bit CS/EIP/SS in its context and resume 32-bit code");
}
static void test_compat_calls(void) {
    group();
    uint32_t* marks = (uint32_t*)(low + LOW_MARK); marks[48] = 0xCA11CA11;
    struct user_desc32* d = (void*)(low + LOW_DESC + 128);
    *d = (struct user_desc32){13, (uint32_t)(uintptr_t)&marks[48], 0xFFFFF, UD_32BIT};
    struct ctx32 c; prepare(&c, 0, wow32_calls, USER32_CS, USER_DS, 0, 0, 0);
    c.ebx = (uint32_t)(uintptr_t)d;
    arm(); call32(&c); disarm();
    CHECK(c.esi == (uint32_t)getpid() && c.edi == (uint32_t)-ENOSYS && c.ebp == 0 && c.edx == 0xCA11CA11 && c.out_fs == TLS_SEL(13));
    CHECK(c.ecx == (uint32_t)-ENOSYS);                                                                 /* SYSCALL: -ENOSYS (CSTAR) or #UD */
    int sysenter = fault_count - 1;
    CHECK(sysenter >= 0 && (faults[sysenter].sig == SIGSEGV || faults[sysenter].sig == SIGILL) && faults[sysenter].cs == USER32_CS);
    printf("[64] WOW32 compat SYSCALL: %s; SYSENTER: %s\n", fault_count == 2 ? "#UD (SIGILL)" : "-ENOSYS (sysretl)", faults[sysenter].sig == SIGILL ? "SIGILL" : "SIGSEGV");
    pass("int $0x80 from 32-bit code (getpid, ENOSYS, set_thread_area then %fs), SYSCALL -> ENOSYS/SIGILL, SYSENTER -> signal");
}
/* Preemption: 32-bit loops on every CPU, each thread with its own TLS slot (selector 0x63, as Wine),
 * half of them on an LDT code segment and LDT DS/ES, while the main thread sends signals. */
#define SPINNERS 6
static volatile int spin_go, spin_failed; static volatile long compat_signals, spin_rounds[SPINNERS];
static void on_usr1(int sig, siginfo_t* info, void* context) {
    (void)sig; (void)info;
    ucontext_t* uc = context;
    if ((uint16_t)uc->uc_mcontext.gregs[REG_CSGSFS] != USER_CS) __atomic_add_fetch(&compat_signals, 1, __ATOMIC_RELAXED);
}
static void* spinner(void* argument) {
    int i = (int)(intptr_t)argument;
    uint32_t* marks = (uint32_t*)(low + LOW_MARK + 0x400);
    marks[i * 16] = 0x51000000U + (uint32_t)i;
    struct user_desc32* d = (void*)(low + LOW_DESC + 0x200 + i * 32);
    *d = (struct user_desc32){12, (uint32_t)(uintptr_t)&marks[i * 16], 0xFFFFF, UD_32BIT | UD_USEABLE}; /* Wine: alloc_fs_sel(fs32_sel >> 3, teb) */
    if (int80(243, (long)d, 0, 0)) { spin_failed++; return NULL; }
    while (!spin_go) sched_yield();
    struct timespec start, now; clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        struct ctx32 c; uint16_t data = (i & 1) ? LDT_SEL(15) : USER_DS;
        prepare(&c, i + 1, wow32_spin, (i & 1) ? LDT_SEL(14) : USER32_CS, data, TLS_SEL(12), TLS_SEL(12), data);
        c.eax = 0xA5A5A5A5; c.ebx = 0x3C3C3C3C; c.edx = 0x0F0F0F0F; c.esi = 0x5A5A5A5A; c.edi = 0xC3C3C3C3;
        c.ebp = 0x51000000U + (uint32_t)i; c.ecx = 400000;
        call32(&c);
        if (c.ecx != 0x600D) { spin_failed++; printf("[64] WOW32 spinner %d corrupted: ecx=%#x\n", i, c.ecx); break; }
        spin_rounds[i]++;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000000000L + (now.tv_nsec - start.tv_nsec) < 1500000000L);
    return NULL;
}
static void test_compat_preemption(void) {
    group();
    CHECK(ldt_set(14, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_CODE) == 0 && ldt_set(15, 0, 0xFFFFF, UD_32BIT | UD_PAGES) == 0);
    install(SIGUSR1, on_usr1);
    pthread_t threads[SPINNERS]; spin_go = 0; spin_failed = 0; compat_signals = 0;
    for (int i = 0; i < SPINNERS; i++) CHECK(pthread_create(&threads[i], NULL, spinner, (void*)(intptr_t)i) == 0);
    struct rusage before; getrusage(RUSAGE_SELF, &before);
    spin_go = 1;
    for (int k = 0; k < 100; k++) {
        struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
        for (int i = 0; i < SPINNERS; i++) pthread_kill(threads[i], SIGUSR1);
    }
    for (int i = 0; i < SPINNERS; i++) pthread_join(threads[i], NULL);
    struct rusage after; getrusage(RUSAGE_SELF, &after);
    long rounds = 0; for (int i = 0; i < SPINNERS; i++) { rounds += spin_rounds[i]; CHECK(spin_rounds[i] > 0); }
    printf("[64] WOW32 preemption: %d threads, %ld rounds of 400000 iterations, %ld signals taken in 32-bit code, %ld involuntary switches\n",
           SPINNERS, rounds, compat_signals, after.ru_nivcsw - before.ru_nivcsw);
    CHECK(!spin_failed && compat_signals > 0 && after.ru_nivcsw > before.ru_nivcsw);
    pass("32-bit loops preempted and signalled on every CPU keep EAX..EBP, CS (0x23/LDT), DS/ES, SS and per-thread TLS %fs");
}
/* A thread runs 32-bit code on an LDT code segment; another deletes the segment: the next iretq to the
 * first thread faults in the kernel and must kill the process with SIGSEGV instead of the kernel. */
static void* parker(void* unused) {
    (void)unused;
    struct ctx32 c; prepare(&c, 1, wow32_park, LDT_SEL(16), USER_DS, 0, 0, 0); call32(&c); return NULL;
}
static void test_stale_cs(void) {
    group();
    pid_t child = fork();
    if (!child) {
        if (ldt_set(16, 0, 0xFFFFF, UD_32BIT | UD_PAGES | UD_CODE)) _exit(2);
        signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL);
        pthread_t t; pthread_create(&t, NULL, parker, NULL);
        struct timespec pause = {0, 50000000}; nanosleep(&pause, NULL);
        ldt_clear(16);
        pause.tv_sec = 3; nanosleep(&pause, NULL);
        _exit(3);
    }
    int status = -1; waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
    pass("iretq onto a deleted LDT code segment kills the process with SIGSEGV, not the kernel");
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    if (argv[1][0] == 'e') return exec_probe();
    setvbuf(stdout, NULL, _IOLBF, 0);
    install(SIGSEGV, on_fault); install(SIGILL, on_fault); install(SIGBUS, on_fault);
    test_selectors();
    test_arch_prctl();
    test_modify_ldt();
    test_selector_loads();
    test_refresh_local();
    test_refresh_remote();
    test_stale_ss();
    test_sigreturn();
    test_fork_threads();
    test_threads();
    int have_low = low_memory();
    test_exec(argv[0]);
    test_int80_64();
    if (have_low) {
        memcpy(low, wow32_blob, (size_t)(wow32_blob_end - wow32_blob));
        if (mprotect(low, 0x10000, PROT_READ | PROT_EXEC)) { printf("[64] WOW32 FAIL: mprotect RX below 4 GiB errno %d\n", errno); return 1; }
        test_tls();
        test_compat_basic();
        test_compat_faults();
        test_compat_calls();
        test_compat_preemption();
        test_stale_cs();
    }
    if (failures) { printf("[64] WOW32 FAILED: %d checks\n", failures); return 1; }
    printf(have_low ? "[64] WOW32 ALL PASSED: 64-bit segment API and 32-bit compatibility mode\n"
                    : "[64] WOW32 ALL PASSED: 64-bit segment API (32-bit execution skipped: no user memory below 4 GiB)\n");
    return 0;
}
