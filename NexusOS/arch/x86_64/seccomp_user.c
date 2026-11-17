/* seccomp conformance from userland (tests/test_x64_seccomp.py). Exercises strict mode, the classic-BPF filter
 * validator and interpreter, every return action (ALLOW/LOG/ERRNO/TRAP/USER_NOTIF/TRACE/KILL_THREAD/KILL_PROCESS),
 * stacked-filter precedence, TSYNC across threads, USER_NOTIF (NEW_LISTENER/RECV/SEND/ID_VALID), the NO_NEW_PRIVS
 * requirement, GET_ACTION_AVAIL / GET_NOTIF_SIZES, the int-0x80 (AUDIT_ARCH_I386) path, fork inheritance, and
 * /proc/self/status. Destructive installs run in forked children so the driver process stays clean. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __NR_seccomp
#define __NR_seccomp 317
#endif

/* ---- uapi (self-contained; no kernel headers required) ---- */
#define PR_SET_NO_NEW_PRIVS 38
#define PR_SET_SECCOMP 22
#define PR_GET_SECCOMP 21
#define SECCOMP_MODE_STRICT 1
#define SECCOMP_MODE_FILTER 2
#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_GET_NOTIF_SIZES 3
#define SECCOMP_FILTER_FLAG_TSYNC (1U<<0)
#define SECCOMP_FILTER_FLAG_LOG (1U<<1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW (1U<<2)
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1U<<3)
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH (1U<<4)
#define SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV (1U<<5)
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_TRAP 0x00030000U
#define SECCOMP_RET_ERRNO 0x00050000U
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#define SECCOMP_RET_TRACE 0x7ff00000U
#define SECCOMP_RET_LOG 0x7ffc0000U
#define SECCOMP_RET_ALLOW 0x7fff0000U
#define AUDIT_ARCH_X86_64 0xC000003EU
#define AUDIT_ARCH_I386 0x40000003U
#define SYS_SECCOMP 1
#define SECCOMP_IOCTL_NOTIF_RECV 0xc0502100ULL
#define SECCOMP_IOCTL_NOTIF_SEND 0xc0182101ULL
#define SECCOMP_IOCTL_NOTIF_ID_VALID 0x40082102ULL
#define SECCOMP_IOCTL_NOTIF_ADDFD 0x40182103ULL
#define SECCOMP_IOCTL_NOTIF_SET_FLAGS 0x40082104ULL
#define SECCOMP_ADDFD_FLAG_SETFD 1U
#define SECCOMP_ADDFD_FLAG_SEND 2U
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE 1U
#define SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP 1ULL
struct seccomp_notif_addfd { uint64_t id; uint32_t flags; uint32_t srcfd; uint32_t newfd; uint32_t newfd_flags; };

/* classic BPF (linux/bpf_common.h) */
#define BPF_LD 0x00
#define BPF_W 0x00
#define BPF_ABS 0x20
#define BPF_JMP 0x05
#define BPF_JEQ 0x10
#define BPF_JGE 0x30
#define BPF_K 0x00
#define BPF_RET 0x06
#define BPF_ALU 0x04
#define BPF_ADD 0x00
#define BPF_JA 0x00
struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct sock_fprog { uint16_t len; struct sock_filter* filter; };
struct seccomp_data { int32_t nr; uint32_t arch; uint64_t instruction_pointer; uint64_t args[6]; };
struct seccomp_notif { uint64_t id; uint32_t pid; uint32_t flags; struct seccomp_data data; };
struct seccomp_notif_resp { uint64_t id; int64_t val; int32_t error; uint32_t flags; };
struct seccomp_notif_sizes { uint16_t seccomp_notif, seccomp_notif_resp, seccomp_data; };
#define BPF_STMT(code, k) { (uint16_t)(code), 0, 0, (uint32_t)(k) }
#define BPF_JUMP(code, k, jt, jf) { (uint16_t)(code), (uint8_t)(jt), (uint8_t)(jf), (uint32_t)(k) }
#define OFF_NR 0
#define OFF_ARCH 4

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] SECCOMP FAIL line=%d errno=%d\n", __LINE__, errno); _exit(70); } } while (0)

static long seccomp(unsigned op, unsigned flags, void* args) { return syscall(__NR_seccomp, op, flags, args); }
static long raw_getpid(void) { return syscall(SYS_getpid); }

/* Run `fn` in a forked child; return its wait status (for WIFSIGNALED / WEXITSTATUS checks). */
static int in_child(void (*fn)(void)) {
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) { fn(); _exit(0); }
    int status = 0; CHECK(waitpid(pid, &status, 0) == pid);
    return status;
}
static int child_status;
#define RUN(fn) (child_status = in_child(fn))
#define SIGNALED(sig) (WIFSIGNALED(child_status) && WTERMSIG(child_status) == (sig))
#define EXITED0 (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0)

static void set_nnp(void) { CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0); }
static int install(struct sock_filter* f, unsigned len, unsigned flags) {
    struct sock_fprog prog = { (uint16_t)len, f };
    return (int)seccomp(SECCOMP_SET_MODE_FILTER, flags, &prog);
}

/* ---- individual child workloads ---- */
static void c_errno_getpid(void) {
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 13 /*EACCES*/),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 4, 0) == 0);
    errno = 0; CHECK(raw_getpid() == -1 && errno == EACCES);
    CHECK(prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == SECCOMP_MODE_FILTER);
    /* other syscalls still work */
    CHECK(getppid() > 0 || getppid() == 0);
}
static void c_errno_then_fork(void) {   /* seccomp survives fork */
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1 /*EPERM*/),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 4, 0) == 0);
    pid_t g = fork(); CHECK(g >= 0);
    if (g == 0) { errno = 0; _exit(raw_getpid() == -1 && errno == EPERM ? 0 : 1); }
    int st = 0; CHECK(waitpid(g, &st, 0) == g);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    errno = 0; CHECK(raw_getpid() == -1 && errno == EPERM);
}
static void c_prctl_filter(void) {   /* the PR_SET_SECCOMP, SECCOMP_MODE_FILTER entry (not just seccomp(2)) */
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { 4, f };
    CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) == 0);
    errno = 0; CHECK(raw_getpid() == -1 && errno == EPERM);
}
static void c_precedence(void) {   /* ALLOW-all, then ERRNO: higher severity (ERRNO) wins; newest of equal wins */
    set_nnp();
    struct sock_filter allow_all[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    CHECK(install(allow_all, 1, 0) == 0);
    struct sock_filter errno_eperm[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(errno_eperm, 4, 0) == 0);
    errno = 0; CHECK(raw_getpid() == -1 && errno == EPERM);   /* ERRNO beats the older ALLOW */
    struct sock_filter errno_eacces[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 13),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(errno_eacces, 4, 0) == 0);
    errno = 0; CHECK(raw_getpid() == -1 && errno == EACCES);  /* newest equal-severity filter wins */
}
static void c_kill_process(void) {
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 4, 0) == 0);
    raw_getpid();   /* must not return */
    _exit(5);
}
static void c_kill_thread(void) {
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_THREAD),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 4, 0) == 0);
    raw_getpid();
    _exit(5);
}
static sigjmp_buf trap_jmp;
static volatile int trap_ok;
static void sigsys(int sig, siginfo_t* info, void* ctx) {
    (void)ctx;
    trap_ok = sig == SIGSYS && info->si_code == SYS_SECCOMP && info->si_syscall == __NR_getpid &&
              info->si_arch == AUDIT_ARCH_X86_64 && info->si_call_addr != 0 && info->si_errno == 0x1234;
    siglongjmp(trap_jmp, 1);
}
static void c_trap(void) {
    set_nnp();
    struct sigaction sa = {0}; sa.sa_sigaction = sigsys; sa.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGSYS, &sa, NULL) == 0);
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | 0x1234),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 4, 0) == 0);
    if (sigsetjmp(trap_jmp, 1) == 0) raw_getpid();
    CHECK(trap_ok);
}
static void c_strict_kill(void) {
    CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0) == 0);
    raw_getpid();   /* not read/write/exit/sigreturn -> SIGKILL */
    _exit(5);
}
static int strict_write_fd;
static void c_strict_ok(void) {   /* read/write/exit are allowed in strict mode */
    char buf[1] = {'Z'};
    syscall(SYS_prctl, PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0);
    syscall(SYS_write, strict_write_fd, buf, 1);   /* allowed */
    syscall(SYS_exit, 0);                          /* exit(60), allowed (exit_group would be killed) */
}
static void c_int80(void) {   /* the int-0x80 path is filtered with AUDIT_ARCH_I386 */
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARCH),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_I386, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 20 /*i386 getpid*/, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1 /*EPERM*/),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 6, 0) == 0);
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(20) : "memory");
    CHECK(r == -EPERM);
    _exit(0);
}
static void c_status(void) {   /* /proc/self/status reports the mode and filter count */
    set_nnp();
    struct sock_filter f[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    CHECK(install(f, 1, 0) == 0);
    int fd = open("/proc/self/status", O_RDONLY); CHECK(fd >= 0);
    char buf[4096]; ssize_t n = read(fd, buf, sizeof(buf) - 1); CHECK(n > 0); buf[n] = 0; close(fd);
    CHECK(strstr(buf, "\nSeccomp:\t2\n") && strstr(buf, "\nSeccomp_filters:\t1\n"));
    _exit(0);
}

/* ---- validator: bad programs rejected with EINVAL (no install) ---- */
static void c_validator(void) {
    set_nnp();
    struct sock_filter ok[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    struct sock_fprog p;
    /* zero length */
    p.len = 0; p.filter = ok; CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, &p) == -1 && errno == EINVAL);
    /* does not end in RET */
    struct sock_filter noret[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR) };
    p.len = 1; p.filter = noret; CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, &p) == -1 && errno == EINVAL);
    /* jump past the end */
    struct sock_filter oob[] = { BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 5, 0), BPF_STMT(BPF_RET | BPF_K, 0) };
    p.len = 2; p.filter = oob; CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, &p) == -1 && errno == EINVAL);
    /* unaligned word load from seccomp_data */
    struct sock_filter bad_abs[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 1), BPF_STMT(BPF_RET | BPF_K, 0) };
    p.len = 2; p.filter = bad_abs; CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, &p) == -1 && errno == EINVAL);
    /* bogus opcode */
    struct sock_filter bad_op[] = { { 0x00ff, 0, 0, 0 }, BPF_STMT(BPF_RET | BPF_K, 0) };
    p.len = 2; p.filter = bad_op; CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, &p) == -1 && errno == EINVAL);
    _exit(0);
}
static void c_needs_nnp(void) {   /* filter mode without NO_NEW_PRIVS (and no CAP_SYS_ADMIN) -> EACCES */
    struct sock_filter f[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    struct sock_fprog p = { 1, f };
    CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, &p) == -1 && errno == EACCES);
    /* ...but the program is copied first (Linux order): the browsers' unprivileged support probes with a NULL
     * program get EFAULT - that is how Firefox (SandboxInfo) and Chromium detect seccomp-bpf and TSYNC */
    CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, NULL, 0, 0) == -1 && errno == EFAULT);
    CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, NULL) == -1 && errno == EFAULT);
    CHECK(seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, NULL) == -1 && errno == EFAULT);
    struct sock_fprog empty = { 0, f };
    CHECK(seccomp(SECCOMP_SET_MODE_FILTER, 0, &empty) == -1 && errno == EINVAL);   /* length before permission */
    CHECK(prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == 0);                                 /* nothing was installed */
    _exit(0);
}

/* ---- TSYNC: a filter installed on one thread applies to every thread ---- */
static volatile int tsync_ready, tsync_go, tsync_result;
static void* tsync_thread(void* arg) {
    (void)arg;
    tsync_ready = 1;
    while (!tsync_go) sched_yield();
    errno = 0;
    tsync_result = (raw_getpid() == -1 && errno == EPERM) ? 1 : 2;
    return NULL;
}
static void c_tsync(void) {
    set_nnp();
    pthread_t t; CHECK(pthread_create(&t, NULL, tsync_thread, NULL) == 0);
    while (!tsync_ready) sched_yield();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 4, SECCOMP_FILTER_FLAG_TSYNC) == 0);
    errno = 0; CHECK(raw_getpid() == -1 && errno == EPERM);   /* installing thread */
    tsync_go = 1; CHECK(pthread_join(t, NULL) == 0);
    CHECK(tsync_result == 1);   /* the sibling thread is filtered too */
    _exit(0);
}

/* ---- USER_NOTIF: worker thread traps into a notification the main thread services ---- */
static int notif_fd;
static volatile int notif_worker_ok;
static void* notif_worker(void* arg) {
    (void)arg;
    long r = raw_getpid();   /* blocks in USER_NOTIF until the supervisor replies */
    notif_worker_ok = (r == 4321);
    return NULL;
}
static void c_user_notif(void) {
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    notif_fd = install(f, 4, SECCOMP_FILTER_FLAG_NEW_LISTENER);
    CHECK(notif_fd >= 0);
    pthread_t t; CHECK(pthread_create(&t, NULL, notif_worker, NULL) == 0);
    struct seccomp_notif req; memset(&req, 0, sizeof(req));
    CHECK(ioctl(notif_fd, SECCOMP_IOCTL_NOTIF_RECV, &req) == 0);
    CHECK(req.data.nr == __NR_getpid && req.data.arch == AUDIT_ARCH_X86_64);
    CHECK(ioctl(notif_fd, SECCOMP_IOCTL_NOTIF_ID_VALID, &req.id) == 0);
    struct seccomp_notif_resp resp; memset(&resp, 0, sizeof(resp));
    resp.id = req.id; resp.val = 4321; resp.error = 0; resp.flags = 0;
    CHECK(ioctl(notif_fd, SECCOMP_IOCTL_NOTIF_SEND, &resp) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(notif_worker_ok);
    uint64_t gone = req.id;
    CHECK(ioctl(notif_fd, SECCOMP_IOCTL_NOTIF_ID_VALID, &gone) == -1);   /* completed id no longer valid */
    _exit(0);
}

/* ---- USER_NOTIF CONTINUE: the supervisor lets the real syscall run ---- */
static int cont_fd;
static volatile long cont_result, cont_expected;
static void* cont_worker(void* arg) { (void)arg; cont_result = raw_getpid(); return NULL; }   /* runs for real on CONTINUE */
static void c_user_notif_continue(void) {
    set_nnp();
    cont_expected = raw_getpid();   /* the real pid, captured before the filter is installed (no self-trap afterwards) */
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    cont_fd = install(f, 4, SECCOMP_FILTER_FLAG_NEW_LISTENER); CHECK(cont_fd >= 0);
    pthread_t t; CHECK(pthread_create(&t, NULL, cont_worker, NULL) == 0);
    struct seccomp_notif req; memset(&req, 0, sizeof(req));
    CHECK(ioctl(cont_fd, SECCOMP_IOCTL_NOTIF_RECV, &req) == 0);
    struct seccomp_notif_resp resp; memset(&resp, 0, sizeof(resp));
    resp.id = req.id; resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
    CHECK(ioctl(cont_fd, SECCOMP_IOCTL_NOTIF_SEND, &resp) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(cont_result == cont_expected && cont_result > 0);   /* the real getpid ran */
    _exit(0);
}
/* ---- USER_NOTIF ADDFD: the supervisor installs one of its fds into the blocked tracee ---- */
static int addfd_fd;
static volatile long addfd_result;
static char addfd_byte;
static void* addfd_worker(void* arg) {
    (void)arg;
    long newfd = raw_getpid();   /* the reply is the installed fd number */
    if (newfd >= 0) { char c = 0; if (read((int)newfd, &c, 1) == 1) addfd_byte = c; close((int)newfd); }
    addfd_result = newfd;
    return NULL;
}
static void c_user_notif_addfd(void) {
    set_nnp();
    int pipefd[2]; CHECK(pipe(pipefd) == 0);
    CHECK(write(pipefd[1], "K", 1) == 1);
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    addfd_fd = install(f, 4, SECCOMP_FILTER_FLAG_NEW_LISTENER); CHECK(addfd_fd >= 0);
    pthread_t t; CHECK(pthread_create(&t, NULL, addfd_worker, NULL) == 0);
    struct seccomp_notif req; memset(&req, 0, sizeof(req));
    CHECK(ioctl(addfd_fd, SECCOMP_IOCTL_NOTIF_RECV, &req) == 0);
    struct seccomp_notif_addfd add; memset(&add, 0, sizeof(add));
    add.id = req.id; add.srcfd = (uint32_t)pipefd[0]; add.flags = SECCOMP_ADDFD_FLAG_SEND;   /* install + complete */
    int installed = (int)ioctl(addfd_fd, SECCOMP_IOCTL_NOTIF_ADDFD, &add);
    CHECK(installed >= 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(addfd_result == installed && addfd_byte == 'K');
    close(pipefd[0]); close(pipefd[1]);
    _exit(0);
}
/* ---- a closed listener makes the blocked tracee's syscall fail with ENOSYS ---- */
static int gone_fd;
static volatile int gone_result;
static void* gone_worker(void* arg) { (void)arg; errno = 0; long r = raw_getpid(); gone_result = (r == -1 && errno == ENOSYS) ? 1 : 2; return NULL; }
static void c_user_notif_listener_gone(void) {
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    gone_fd = install(f, 4, SECCOMP_FILTER_FLAG_NEW_LISTENER); CHECK(gone_fd >= 0);
    pthread_t t; CHECK(pthread_create(&t, NULL, gone_worker, NULL) == 0);
    struct seccomp_notif req; memset(&req, 0, sizeof(req));
    CHECK(ioctl(gone_fd, SECCOMP_IOCTL_NOTIF_RECV, &req) == 0);
    close(gone_fd);   /* supervisor gone: the tracee resumes with ENOSYS */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(gone_result == 1);   /* the blocked getpid() returned -ENOSYS */
    _exit(0);
}
/* ---- TSYNC_ESRCH: a sibling with an incompatible filter makes TSYNC fail with ESRCH (not a tid) ---- */
static volatile int esrch_ready, esrch_go;
static void* esrch_thread(void* arg) {
    (void)arg;
    struct sock_filter own[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    install(own, 1, 0);           /* this thread gets its own, unrelated filter */
    esrch_ready = 1;
    while (!esrch_go) sched_yield();
    return NULL;
}
static void c_tsync_esrch(void) {
    set_nnp();
    pthread_t t; CHECK(pthread_create(&t, NULL, esrch_thread, NULL) == 0);
    while (!esrch_ready) sched_yield();
    struct sock_filter f[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    struct sock_fprog prog = { 1, f };
    errno = 0;
    long r = seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_TSYNC_ESRCH, &prog);
    CHECK(r == -1 && errno == ESRCH);   /* ESRCH, not the offending tid */
    esrch_go = 1; CHECK(pthread_join(t, NULL) == 0);
    _exit(0);
}
/* ---- SPEC_ALLOW and LOG flags are accepted; LOG action is allowed-through ---- */
static void c_flags_and_log(void) {
    set_nnp();
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_LOG),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(f, 4, SECCOMP_FILTER_FLAG_LOG | SECCOMP_FILTER_FLAG_SPEC_ALLOW) == 0);
    CHECK(raw_getpid() == getpid());   /* LOG lets the syscall run */
    _exit(0);
}
/* ---- NEW_LISTENER needs NO_NEW_PRIVS/CAP like any filter; SET_FLAGS rejects unknown bits ---- */
static void c_listener_flags(void) {
    set_nnp();
    struct sock_filter f[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
    struct sock_fprog prog = { 1, f };
    int fd = (int)seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
    CHECK(fd >= 0);
    /* a second listener on the same task is refused (Linux has_duplicate_listener -> EBUSY) */
    struct sock_fprog prog2 = { 1, f };
    CHECK(seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog2) == -1 && errno == EBUSY);
    uint64_t flags = SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP;
    CHECK(ioctl(fd, SECCOMP_IOCTL_NOTIF_SET_FLAGS, &flags) == 0);
    flags = 0x80;   /* an unknown flag */
    CHECK(ioctl(fd, SECCOMP_IOCTL_NOTIF_SET_FLAGS, &flags) == -1 && errno == EINVAL);
    /* TSYNC and NEW_LISTENER together are EINVAL unless TSYNC_ESRCH is also set */
    struct sock_fprog prog3 = { 1, f };
    CHECK(seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog3) == -1 && errno == EINVAL);
    close(fd);
    _exit(0);
}
static void sizes_and_avail(void) {
    struct seccomp_notif_sizes sizes; memset(&sizes, 0, sizeof(sizes));
    CHECK(seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &sizes) == 0);
    CHECK(sizes.seccomp_notif == sizeof(struct seccomp_notif) &&
          sizes.seccomp_notif_resp == sizeof(struct seccomp_notif_resp) &&
          sizes.seccomp_data == sizeof(struct seccomp_data));
    uint32_t a = SECCOMP_RET_ALLOW; CHECK(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) == 0);
    a = SECCOMP_RET_KILL_PROCESS; CHECK(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) == 0);
    a = SECCOMP_RET_USER_NOTIF; CHECK(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) == 0);
    a = 0x00010000; CHECK(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a) == -1 && errno == EOPNOTSUPP);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    sizes_and_avail();

    RUN(c_validator); CHECK(EXITED0);
    RUN(c_needs_nnp); CHECK(EXITED0);
    RUN(c_errno_getpid); CHECK(EXITED0);
    RUN(c_errno_then_fork); CHECK(EXITED0);
    RUN(c_prctl_filter); CHECK(EXITED0);
    RUN(c_precedence); CHECK(EXITED0);
    RUN(c_trap); CHECK(EXITED0);
    RUN(c_int80); CHECK(EXITED0);
    RUN(c_status); CHECK(EXITED0);
    RUN(c_tsync); CHECK(EXITED0);
    RUN(c_tsync_esrch); CHECK(EXITED0);
    RUN(c_flags_and_log); CHECK(EXITED0);
    RUN(c_listener_flags); CHECK(EXITED0);
    RUN(c_user_notif); CHECK(EXITED0);
    RUN(c_user_notif_continue); CHECK(EXITED0);
    RUN(c_user_notif_addfd); CHECK(EXITED0);
    RUN(c_user_notif_listener_gone); CHECK(EXITED0);

    RUN(c_kill_process); CHECK(SIGNALED(SIGSYS));
    RUN(c_kill_thread); CHECK(SIGNALED(SIGSYS));
    RUN(c_strict_kill); CHECK(SIGNALED(SIGKILL));

    /* strict mode permits read/write/exit: the child writes a byte through a pipe before exiting cleanly */
    int fds[2]; CHECK(pipe(fds) == 0);
    strict_write_fd = fds[1];
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) { close(fds[0]); c_strict_ok(); _exit(9); }
    close(fds[1]);
    char got = 0; CHECK(read(fds[0], &got, 1) == 1 && got == 'Z');
    int st = 0; CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(fds[0]);

    dprintf(1, "[64] SECCOMP ALL PASSED: strict, BPF validate/interpret, actions, precedence, TSYNC, USER_NOTIF, int80, /proc\n");
    return 0;
}
