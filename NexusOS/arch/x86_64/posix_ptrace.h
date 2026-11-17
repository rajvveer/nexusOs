/* ptrace checks on the ext4 root, included by posixfs_user.c (run as uid 1000; /bin/posix64-root is its set-user-ID
 * root copy).
 * - Linux would_dump / ptrace_access_vm: an execute-only image and a set-user-ID exec are not dumpable. A tracer
 *   attached before such an exec (TRACEME) gets EIO from PEEK/POKE, PTRACE_ATTACH after it gets EPERM and
 *   /proc/PID/mem EACCES; a traced set-user-ID exec ignores the set-ID bit (LSM_UNSAFE_PTRACE) and stays dumpable.
 * - Linux ptrace_signal: fault signals stop a traced thread before they are delivered. PEEKUSER shows the faulting
 *   state; the tracer may cancel them (int3 goes on after the instruction, a page fault runs it again) or pass them.
 *   A passed page fault reports its own address in the handler's sigcontext CR2 (saved before the stop).
 * - Linux do_exit: a traced thread that exits drops its descriptors and memory at once; only the zombie waits for
 *   the tracer. A /proc/PID/mem descriptor does not keep an exited image (ETXTBSY) alive. */
#include <poll.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>

static const char* ptrace_self;   /* this program, readable */
/* Helper modes of an exec'd copy: D exits 10 + PR_GET_DUMPABLE, S stops itself first, W reports on fd argv[2], waits. */
static int ptrace_helper(int argc, char** argv) {
    if (argc < 2 || !argv[1][0] || argv[1][1]) return -1;
    switch (argv[1][0]) {
    case 'D': return 10 + prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    case 'S': raise(SIGSTOP); return 10 + prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    case 'W': if (argc != 3 || write(atoi(argv[2]), "r", 1) != 1) return 97; for (;;) pause();
    }
    return -1;
}
/* exit status, 128 + a terminating signal, or 256 + the signal of a ptrace stop */
static int ptrace_status(pid_t child) {
    int s; CHECK(waitpid(child, &s, 0) == child);
    return WIFEXITED(s) ? WEXITSTATUS(s) : WIFSIGNALED(s) ? 128 + WTERMSIG(s) : 256 + WSTOPSIG(s);
}
static int ptrace_stop(pid_t child) {   /* the next ptrace stop after an exec (Linux's legacy exec SIGTRAP is continued) */
    for (;;) {
        int status = ptrace_status(child); if (status != 256 + SIGTRAP) return status;
        CHECK(ptrace(PTRACE_CONT, child, 0, 0) == 0);
    }
}
static void ptrace_install(const char* path, mode_t mode) {
    int in = open(ptrace_self, O_RDONLY); struct stat st; CHECK(in >= 0 && fstat(in, &st) == 0 && st.st_size > 0);
    char* image = malloc((size_t)st.st_size); CHECK(image && pread(in, image, (size_t)st.st_size, 0) == st.st_size && close(in) == 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    CHECK(fd >= 0 && write(fd, image, (size_t)st.st_size) == st.st_size && fsync(fd) == 0 && close(fd) == 0 && chmod(path, mode) == 0);
    free(image);
}
static int ptrace_run(const char* path) {   /* 10 + the dumpable flag of `path` after its exec */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { execl(path, path, "D", (char*)NULL); _exit(98); }
    return ptrace_status(child);
}
/* TRACEME, then exec `path S`: PEEKTEXT, POKEDATA and /proc/PID/mem in its stop. Returns the PEEKTEXT errno. */
static int ptrace_traced_exec(const char* path) {
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { if (ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(97); execl(path, path, "S", (char*)NULL); _exit(98); }
    CHECK(ptrace_stop(child) == 256 + SIGSTOP);
    errno = 0; long rip = ptrace(PTRACE_PEEKUSER, child, (void*)(16 * 8), 0); CHECK(errno == 0 && rip);
    errno = 0; long rsp = ptrace(PTRACE_PEEKUSER, child, (void*)(19 * 8), 0); CHECK(errno == 0 && rsp);
    errno = 0; (void)ptrace(PTRACE_PEEKTEXT, child, (void*)rip, 0); int peek = errno;
    int poke = ptrace(PTRACE_POKEDATA, child, (void*)(rsp - 1024), (void*)(long)0x5a5a) == 0 ? 0 : errno;   /* unused stack */
    char name[64]; snprintf(name, sizeof(name), "/proc/%d/mem", child);
    int mem = open(name, O_RDONLY), opened = mem >= 0 ? 0 : errno; if (mem >= 0) close(mem);
    if (peek ? peek != EIO || poke != EIO || opened != EACCES : poke || opened)
        dprintf(2, "[64] POSIX PTRACE %s: peek=%d poke=%d mem=%d\n", path, peek, poke, opened);
    CHECK(peek ? peek == EIO && poke == EIO && opened == EACCES : !poke && !opened);
    CHECK(ptrace(PTRACE_CONT, child, 0, 0) == 0 && ptrace_status(child) == (peek ? 10 : 11));
    return peek;
}
/* exec `path W` untraced, then PTRACE_ATTACH and /proc/PID/mem from this same-UID process. Returns the ATTACH errno. */
static int ptrace_attach_after_exec(const char* path) {
    int ready[2]; CHECK(pipe(ready) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { char fd[16]; snprintf(fd, sizeof(fd), "%d", ready[1]); close(ready[0]); execl(path, path, "W", fd, (char*)NULL); _exit(98); }
    close(ready[1]); char byte; CHECK(read(ready[0], &byte, 1) == 1 && close(ready[0]) == 0);
    char name[64]; snprintf(name, sizeof(name), "/proc/%d/mem", child);
    int mem = open(name, O_RDONLY), opened = mem >= 0 ? 0 : errno; if (mem >= 0) close(mem);
    int attach = ptrace(PTRACE_ATTACH, child, 0, 0) == 0 ? 0 : errno;
    if (!attach) CHECK(ptrace_status(child) == 256 + SIGSTOP && ptrace(PTRACE_DETACH, child, 0, 0) == 0);
    if (attach ? attach != EPERM || opened != EACCES : opened != 0)
        dprintf(2, "[64] POSIX PTRACE %s: attach=%d mem=%d\n", path, attach, opened);
    CHECK(attach ? attach == EPERM && opened == EACCES : !opened);
    CHECK(kill(child, SIGKILL) == 0 && ptrace_status(child) == 128 + SIGKILL);
    return attach;
}
static void ptrace_fault_stops(void) {
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(97);
        raise(SIGSTOP);
        __asm__ volatile("int3");                         /* SIGTRAP stop; cancelled, it goes on after the int3 */
        __asm__ volatile("movl $1, 16" ::: "memory");     /* SIGSEGV stop: an unmapped address */
        _exit(96);
    }
    CHECK(ptrace_status(child) == 256 + SIGSTOP && ptrace(PTRACE_CONT, child, 0, 0) == 0);
    CHECK(ptrace_status(child) == 256 + SIGTRAP);
    errno = 0; long rip = ptrace(PTRACE_PEEKUSER, child, (void*)(16 * 8), 0); CHECK(errno == 0 && rip);
    errno = 0; long code = ptrace(PTRACE_PEEKTEXT, child, (void*)(rip - 1), 0); CHECK(errno == 0 && (code & 0xFF) == 0xCC);
    CHECK(ptrace(PTRACE_CONT, child, 0, 0) == 0 && ptrace_status(child) == 256 + SIGSEGV);
    errno = 0; long fault = ptrace(PTRACE_PEEKUSER, child, (void*)(16 * 8), 0); CHECK(errno == 0 && fault >= rip && fault - rip < 64);
    CHECK(ptrace(PTRACE_CONT, child, 0, 0) == 0 && ptrace_status(child) == 256 + SIGSEGV);   /* cancelled: runs again */
    errno = 0; CHECK(ptrace(PTRACE_PEEKUSER, child, (void*)(16 * 8), 0) == fault && errno == 0);
    CHECK(ptrace(PTRACE_CONT, child, 0, (void*)(long)SIGSEGV) == 0 && ptrace_status(child) == 128 + SIGSEGV);
}
static void ptrace_cr2_handler(int number, siginfo_t* info, void* context) {
    greg_t cr2 = ((ucontext_t*)context)->uc_mcontext.gregs[REG_CR2];
    _exit(number == SIGSEGV && info->si_addr == (void*)0x2340 && cr2 == 0x2340 ? 50 : 51);
}
/* The tracer page-faults while the tracee sits in its SIGSEGV stop, so CR2 no longer holds the tracee's address. */
static void ptrace_fault_cr2(void) {
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        struct sigaction action = {0}; action.sa_sigaction = ptrace_cr2_handler; action.sa_flags = SA_SIGINFO;
        if (sigaction(SIGSEGV, &action, NULL) || ptrace(PTRACE_TRACEME, 0, 0, 0)) _exit(97);
        raise(SIGSTOP);
        __asm__ volatile("movl $1, 0x2340" ::: "memory");
        _exit(96);
    }
    CHECK(ptrace_status(child) == 256 + SIGSTOP && ptrace(PTRACE_CONT, child, 0, 0) == 0);
    CHECK(ptrace_status(child) == 256 + SIGSEGV);
    volatile char* fresh = mmap(NULL, 64 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(fresh != MAP_FAILED);
    for (unsigned i = 0; i < 64; i++) fresh[i * 4096] = 1;   /* page faults of this process */
    CHECK(ptrace(PTRACE_CONT, child, 0, (void*)(long)SIGSEGV) == 0);
    CHECK(ptrace_status(child) == 50 && munmap((void*)fresh, 64 * 4096) == 0);
}
/* PTRACE_ATTACH a grandchild (its tracer is not its parent, so its exit waits for this tracer to collect it). Its
 * pipe end closes when it exits, before the tracer waits; the parent still gets its exit. */
static void ptrace_exit_releases(void) {
    int hold[2], go[2], report[2]; CHECK(pipe(hold) == 0 && pipe(go) == 0 && pipe(report) == 0);
    pid_t parent = fork(); CHECK(parent >= 0);
    if (!parent) {
        close(hold[0]); close(report[0]); close(go[1]);
        pid_t tracee = fork(); if (tracee < 0) _exit(90);
        if (!tracee) {
            close(report[1]); char byte;
            while (read(go[0], &byte, 1) < 0 && errno == EINTR) {}   /* the attach's SIGSTOP may interrupt the read */
            _exit(7);   /* still holding hold[1] */
        }
        close(hold[1]); close(go[0]);
        if (write(report[1], &tracee, sizeof(tracee)) != sizeof(tracee)) _exit(91);
        int status; if (waitpid(tracee, &status, 0) != tracee) _exit(92);
        _exit(WIFEXITED(status) && WEXITSTATUS(status) == 7 ? 0 : 93);
    }
    close(hold[1]); close(report[1]); close(go[0]);
    pid_t tracee; CHECK(read(report[0], &tracee, sizeof(tracee)) == sizeof(tracee) && close(report[0]) == 0);
    CHECK(ptrace(PTRACE_ATTACH, tracee, 0, 0) == 0);
    int status; CHECK(waitpid(tracee, &status, __WALL) == tracee && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    CHECK(ptrace(PTRACE_CONT, tracee, 0, 0) == 0 && write(go[1], "g", 1) == 1 && close(go[1]) == 0);
    struct pollfd wait_eof = {hold[0], POLLIN, 0};
    CHECK(poll(&wait_eof, 1, 5000) == 1);   /* its descriptors went with its exit, not with our wait */
    char byte; CHECK(read(hold[0], &byte, 1) == 0 && close(hold[0]) == 0);
    CHECK(waitpid(tracee, &status, __WALL) == tracee && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    CHECK(waitpid(parent, &status, 0) == parent && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
/* An open /proc/PID/mem must not keep the exited image, and with it its executable (ETXTBSY), alive. No procfs
 * call is made between the exit and the write open (procfs operations also drop dead images). */
static void ptrace_mem_unpins(const char* dir) {
    char path[256]; snprintf(path, sizeof(path), "%s/mem-target", dir); ptrace_install(path, 0755);
    int ready[2]; CHECK(pipe(ready) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { char fd[16]; snprintf(fd, sizeof(fd), "%d", ready[1]); close(ready[0]); execl(path, path, "W", fd, (char*)NULL); _exit(98); }
    close(ready[1]); char byte; CHECK(read(ready[0], &byte, 1) == 1 && close(ready[0]) == 0);
    char name[64]; snprintf(name, sizeof(name), "/proc/%d/mem", child);
    int mem = open(name, O_RDONLY); CHECK(mem >= 0);
    CHECK(open(path, O_WRONLY) == -1 && errno == ETXTBSY);   /* running */
    CHECK(kill(child, SIGKILL) == 0 && ptrace_status(child) == 128 + SIGKILL);
    int writer = -1;
    for (unsigned tries = 0; tries < 200 && writer < 0; tries++) {   /* exit resources are released by the reaper */
        writer = open(path, O_WRONLY);
        if (writer < 0) { CHECK(errno == ETXTBSY); nanosleep(&(struct timespec){0, 10000000}, NULL); }
    }
    if (writer < 0) dprintf(2, "[64] POSIX PTRACE /proc/PID/mem still pins %s\n", path);
    CHECK(writer >= 0 && close(writer) == 0);
    CHECK(pread(mem, &byte, 1, 0x10000) == 0 && close(mem) == 0 && unlink(path) == 0);   /* the image is gone: EOF */
}
static void ptrace_checks(const char* dir) {
    char readable[256], xonly[256];
    snprintf(readable, sizeof(readable), "%s/dump-readable", dir); snprintf(xonly, sizeof(xonly), "%s/dump-xonly", dir);
    ptrace_install(readable, 0755); ptrace_install(xonly, 0111);
    CHECK(ptrace_run(readable) == 11);
    CHECK(open(xonly, O_RDONLY) == -1 && errno == EACCES && ptrace_run(xonly) == 10);   /* execute-only */
    CHECK(ptrace_run("/bin/posix64-root") == 10);                                          /* set-user-ID root */
    CHECK(ptrace_traced_exec(readable) == 0);
    CHECK(ptrace_traced_exec(xonly) == EIO);                 /* the tracer attached before the exec */
    CHECK(ptrace_traced_exec("/bin/posix64-root") == 0);     /* traced: the set-ID bit is ignored, still dumpable */
    CHECK(ptrace_attach_after_exec(readable) == 0);
    CHECK(ptrace_attach_after_exec(xonly) == EPERM);
    CHECK(ptrace_attach_after_exec("/bin/posix64-root") == EPERM);
    CHECK(unlink(readable) == 0 && unlink(xonly) == 0);
    ptrace_fault_stops();
    ptrace_fault_cr2();
    ptrace_exit_releases();
    ptrace_mem_unpins(dir);
    puts("[64] POSIX PTRACE PASSED: execute-only and set-user-ID images not dumpable (PEEK/POKE EIO, ATTACH EPERM, /proc/PID/mem EACCES), fault signals stop a traced thread (CR2 kept), traced exit releases resources, /proc/PID/mem unpins");
}
