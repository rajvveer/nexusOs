#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] PROCESS FAIL line=%d errno=%d pid=%d\n", __LINE__, errno, getpid()); _exit(99); } } while (0)
static __thread unsigned local = 41;
static volatile unsigned char image_data[8192] __attribute__((aligned(4096))) = {[0] = 35, [4096] = 66, [8191] = 91};
static volatile unsigned char image_zero[8193] __attribute__((aligned(4096)));
static volatile sig_atomic_t notified, child_pid, child_status;
static int acknowledge = -1;
static void notify(int number) { notified = number; if (acknowledge >= 0 && write(acknowledge, "A", 1) != 1) _exit(98); }
/* Waits (up to ~2 s) until `pid` sleeps interruptibly ("State:\tS", e.g. blocked in read(2)): a signal sent earlier would
 * be handled before that read starts, and the EINTR check below would race (disk waits show 'D', not 'S'). */
static void await_sleeping(pid_t pid) {
    char path[64], text[256]; snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    for (int i = 0; i < 2000; i++) {
        int fd = open(path, O_RDONLY); if (fd < 0) return;
        ssize_t n = read(fd, text, sizeof(text) - 1); close(fd);
        if (n > 0) { text[n] = 0; const char* s = strstr(text, "State:\t"); if (s && s[7] == 'S') return; }
        usleep(1000);
    }
}
static void exited(int number, siginfo_t* info, void* context) {
    (void)number; (void)context;
    child_pid = info->si_pid; child_status = info->si_status;
}
static int exec_child(int argc, char** argv) {
    CHECK(image_data[0] == 35 && image_data[4096] == 66 && image_data[8191] == 91);
    for (unsigned i = 0; i < sizeof(image_zero); i++) CHECK(image_zero[i] == 0);
    image_data[0] = 186; image_data[4096] = 187; image_zero[8192] = 188;
    CHECK(argc == 7 && !strcmp(argv[0], "custom argv zero"));
    CHECK(getenv("NEXUS_EXEC_PROBE") && !strcmp(getenv("NEXUS_EXEC_PROBE"), "payload with spaces"));
    CHECK(getenv("SECOND") && !strcmp(getenv("SECOND"), "ok") && getenv("HOME") == NULL);
    CHECK(local == 41 && getppid() == atoi(argv[4]) && getsid(0) == atoi(argv[5]));
    CHECK(!strcmp((char*)getauxval(AT_EXECFN), "/bin/process64"));
    char path[64] = {0}; CHECK(readlink("/proc/self/exe", path, sizeof(path) - 1) == 14 && !strcmp(path, "/bin/process64"));
    int keep = atoi(argv[2]), closed = atoi(argv[3]);
    CHECK(fcntl(keep, F_GETFD) == 0 && fcntl(closed, F_GETFD) == -1 && errno == EBADF);
    struct sigaction action;
    CHECK(sigaction(SIGUSR1, NULL, &action) == 0 && action.sa_handler == SIG_DFL);
    CHECK(sigaction(SIGPIPE, NULL, &action) == 0 && action.sa_handler == SIG_IGN);
    sigset_t mask; CHECK(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR2));
    CHECK(sigpending(&mask) == 0 && sigismember(&mask, SIGUSR2) == atoi(argv[6]));
    stack_t stack; CHECK(sigaltstack(NULL, &stack) == 0 && (stack.ss_flags & SS_DISABLE));
    CHECK(write(keep, "E", 1) == 1); return 23;
}
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "exec-child")) return exec_child(argc, argv);
    CHECK(image_data[0] == 35 && image_data[4096] == 66 && image_zero[8192] == 0);
    image_data[0] = 250; image_data[4096] = 251; image_zero[0] = 252; image_zero[8192] = 253;
    pid_t parent = getpid(); CHECK(parent > 0 && getppid() == 0);
    struct sigaction action = {.sa_handler = notify}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    struct sigaction child_action = {.sa_sigaction = exited, .sa_flags = SA_SIGINFO}; sigemptyset(&child_action.sa_mask);
    CHECK(sigaction(SIGCHLD, &child_action, NULL) == 0);
    CHECK(getpgrp() == parent && getsid(0) == parent && setsid() == -1 && errno == EPERM);
    CHECK(kill(0, 0) == 0 && setpgid(-1, 0) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_tkill, -1, 0) == -1 && errno == EINVAL);
    unsigned* private = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(private != MAP_FAILED); *private = 0x12345678;
    CHECK(mprotect(private, 4096, PROT_READ) == 0);
    int pair[2], enabled = 1;
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    int file = open("/etc/native64.dat", O_RDONLY); CHECK(file >= 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(getpid() != parent && getppid() == parent && local == 41);
        local = 99;
        CHECK(mprotect(private, 4096, PROT_READ | PROT_WRITE) == 0); *private = 88;
        CHECK(close(pair[0]) == 0 && lseek(file, 42, SEEK_SET) == 42);
        char payload = 'C', control[CMSG_SPACE(sizeof(int))] = {0};
        struct iovec io = {&payload, 1};
        struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        struct cmsghdr* c = CMSG_FIRSTHDR(&message); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &file, sizeof(file));
        CHECK(sendmsg(pair[1], &message, 0) == 1 && close(file) == 0);
        CHECK(read(pair[1], &payload, 1) == 1 && payload == 'A');
        __asm__ volatile("pxor %%xmm15,%%xmm15" ::: "xmm15");
        CHECK(kill(parent, SIGUSR1) == 0);
        _exit(37);
    }
    CHECK(close(pair[1]) == 0 && close(file) == 0);
    char payload, control[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))] = {0};
    struct iovec io = {&payload, 1};
    struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(pair[0], &message, MSG_CMSG_CLOEXEC) == 1 && payload == 'C');
    int received = -1, sender = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_type == SCM_RIGHTS) memcpy(&received, CMSG_DATA(c), sizeof(received));
        if (c->cmsg_type == SCM_CREDENTIALS) {
            struct ucred credentials; memcpy(&credentials, CMSG_DATA(c), sizeof(credentials));
            CHECK(credentials.uid == getuid() && credentials.gid == getgid()); sender = credentials.pid;
        }
    }
    CHECK(received >= 0 && sender == child && local == 41 && *private == 0x12345678);
    CHECK(fcntl(received, F_GETFD) == FD_CLOEXEC && lseek(received, 0, SEEK_CUR) == 42);
    CHECK(read(received, &payload, 1) == 1 && (unsigned char)payload == (unsigned char)(42 * 37 + 11));
    CHECK(close(received) == 0);
    int status; CHECK(waitpid(child, &status, WNOHANG) == 0);
    CHECK(write(pair[0], "A", 1) == 1);
    /* No syscalls in this loop: only IRQ preemption can run the child/signal. */
    uint64_t sentinel = 0x123456789ABCDEF0ULL, restored;
    __asm__ volatile("movq %2,%%xmm15; 1: cmpl $0,%1; jne 2f; pause; jmp 1b; 2: movq %%xmm15,%0"
                     : "=r"(restored) : "m"(notified), "r"(sentinel) : "xmm15", "cc", "memory");
    CHECK(restored == sentinel && notified == SIGUSR1);
    CHECK(syscall(SYS_wait4, child, (void*)1, 0, NULL) == -1 && errno == EFAULT);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    CHECK(child_pid == child && child_status == 37);
    CHECK(waitpid(child, &status, 0) == -1 && errno == ECHILD);
    CHECK(read(pair[0], &payload, 1) == 0 && close(pair[0]) == 0 && munmap(private, 4096) == 0);
    puts("[64] PROCESS fork/COW/TLS, credentials/rights, blocking IPC, timer/FPU and wait status PASS");

    child = fork(); CHECK(child >= 0);
    if (!child) { for (;;) __asm__ volatile("pause"); }
    CHECK(kill(child, SIGTERM) == 0);
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(kill(child, 0) == -1 && errno == ESRCH);
    child_action.sa_handler = SIG_DFL; child_action.sa_flags = 0;
    CHECK(sigaction(SIGCHLD, &child_action, NULL) == 0);
    int parent_tid = -77, child_tid = -88;
    child = (pid_t)syscall(SYS_clone, SIGCHLD | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID,
                          NULL, &parent_tid, &child_tid, NULL);
    CHECK(child >= 0);
    if (!child) { CHECK(parent_tid == -77 && child_tid == getpid()); _exit(0); }
    CHECK(parent_tid == child && child_tid == -88 && waitpid(child, &status, 0) == child && status == 0);
    for (int restart = 0; restart < 2; restart++) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        action.sa_flags = restart ? SA_RESTART : 0;
        CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
        acknowledge = pair[0]; child = fork(); CHECK(child >= 0);
        if (!child) {
            CHECK(close(pair[0]) == 0); await_sleeping(parent); CHECK(kill(parent, SIGUSR1) == 0);
            CHECK(read(pair[1], &payload, 1) == 1 && payload == 'A');
            CHECK(write(pair[1], "R", 1) == 1); _exit(0);
        }
        CHECK(close(pair[1]) == 0);
        ssize_t got = read(pair[0], &payload, 1);
        if (!restart) { CHECK(got == -1 && errno == EINTR); got = read(pair[0], &payload, 1); }
        CHECK(got == 1 && payload == 'R'); acknowledge = -1;
        CHECK(close(pair[0]) == 0 && waitpid(child, &status, 0) == child && status == 0);
    }
    puts("[64] PROCESS interrupted blocking reads and SA_RESTART PASS");
    for (int i = 0; i < 24; i++) {
        child = (pid_t)syscall(SYS_fork); CHECK(child >= 0);
        if (!child) {
            CHECK(getsid(0) == parent && getpgid(0) == parent);
            if (i & 1) {
                CHECK(setsid() == getpid() && getsid(0) == getpid() && getpgrp() == getpid());
                CHECK(kill(-getpgrp(), 0) == 0);
                CHECK(setpgid(0, parent) == -1 && errno == EPERM);
            } else {
                CHECK(setpgid(0, 0) == 0 && getpgrp() == getpid() && getsid(0) == parent);
                CHECK(setsid() == -1 && errno == EPERM && setpgid(0, parent) == 0);
            }
            char* p = malloc(131072); CHECK(p); memset(p, i, 131072); _exit(i);
        }
        CHECK(waitpid(-1, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == i);
    }
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    acknowledge = pair[1]; action.sa_handler = notify; action.sa_flags = 0;
    CHECK(sigaction(SIGCHLD, &action, NULL) == 0);
    child = fork(); CHECK(child >= 0); if (!child) _exit(0);
    /* Keep both endpoints open: only SIGCHLD can wake this blocked parent. */
    ssize_t got = read(pair[0], &payload, 1);
    if (got == -1) { CHECK(errno == EINTR); got = read(pair[0], &payload, 1); }
    CHECK(got == 1 && payload == 'A'); acknowledge = -1;
    CHECK(waitpid(child, &status, 0) == child && status == 0 && close(pair[0]) == 0 && close(pair[1]) == 0);
    child_action.sa_handler = SIG_IGN;
    CHECK(sigaction(SIGCHLD, &child_action, NULL) == 0);
    child = fork(); CHECK(child >= 0); if (!child) _exit(0);
    CHECK(waitpid(child, &status, 0) == -1 && errno == ECHILD);
    child_action.sa_handler = SIG_DFL;
    CHECK(sigaction(SIGCHLD, &child_action, NULL) == 0 && signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2); CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    void* alt = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(alt != MAP_FAILED);
    stack_t stack = {.ss_sp = alt, .ss_size = 65536}; CHECK(sigaltstack(&stack, NULL) == 0);
    for (int spawn = 0; spawn < 2; spawn++) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        file = open("/etc/native64.dat", O_RDONLY | O_CLOEXEC); CHECK(file >= 0);
        char keep[16], closed[16], parent_text[16], session[16];
        snprintf(keep, sizeof(keep), "%d", pair[1]); snprintf(closed, sizeof(closed), "%d", file);
        snprintf(parent_text, sizeof(parent_text), "%d", parent); snprintf(session, sizeof(session), "%d", getsid(0));
        char* arguments[] = {"custom argv zero", "exec-child", keep, closed, parent_text, session, spawn ? "0" : "1", NULL};
        char* environment[] = {"NEXUS_EXEC_PROBE=payload with spaces", "SECOND=ok", NULL};
        if (spawn) {
            posix_spawn_file_actions_t actions; CHECK(posix_spawn_file_actions_init(&actions) == 0);
            CHECK(posix_spawn_file_actions_addclose(&actions, pair[0]) == 0);
            CHECK(posix_spawn(&child, "/bin/truncated64", &actions, NULL, arguments, environment) == ENOEXEC);
            CHECK(posix_spawn(&child, "/bin/process64", &actions, NULL, arguments, environment) == 0);
            CHECK(posix_spawn_file_actions_destroy(&actions) == 0);
        } else {
            child = fork(); CHECK(child >= 0);
            if (!child) {
                local = 99; CHECK(close(pair[0]) == 0 && raise(SIGUSR2) == 0);
                CHECK(syscall(SYS_execve, (void*)1, arguments, environment) == -1 && errno == EFAULT);
                CHECK(syscall(SYS_execve, "/bin/process64", (void*)1, environment) == -1 && errno == EFAULT);
                CHECK(execve("/bin/truncated64", arguments, environment) == -1 && errno == ENOEXEC);
                CHECK(fcntl(file, F_GETFD) == FD_CLOEXEC && local == 99);
                struct sigaction preserved; CHECK(sigaction(SIGUSR1, NULL, &preserved) == 0 && preserved.sa_handler == notify);
                execve("/bin/process64", arguments, environment); CHECK(0);
            }
        }
        CHECK(close(pair[1]) == 0 && read(pair[0], &payload, 1) == 1 && payload == 'E');
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23);
        CHECK(image_data[0] == 250 && image_data[4096] == 251 && image_zero[0] == 252 && image_zero[8192] == 253);
        CHECK(fcntl(file, F_GETFD) == FD_CLOEXEC && close(file) == 0 && close(pair[0]) == 0);
    }
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    child = fork(); CHECK(child >= 0); if (!child) _exit(42);
    siginfo_t info;
    CHECK(waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0 && info.si_pid == child && info.si_status == 42 && info.si_code == CLD_EXITED);
    CHECK(waitid(P_PID, child, &info, WEXITED) == 0 && info.si_pid == child && info.si_status == 42);
    CHECK(waitid(P_PID, child, &info, WEXITED | WNOHANG) == -1 && errno == ECHILD);
    CHECK(sigaltstack(NULL, &stack) == 0 && stack.ss_sp == alt);
    stack.ss_flags = SS_DISABLE; CHECK(sigaltstack(&stack, NULL) == 0 && munmap(alt, 65536) == 0);
    puts("[64] PROCESS execve and glibc posix_spawn PASS: arguments/environment, CLOEXEC, signals and failure rollback");
    puts("[64] EXEC IMAGE ISOLATION PASSED: cached ELF data stays private across independent execs and BSS is zero");
    puts("[64] PROCESS ALL PASSED: signal termination, repeated fork/reap and ignored SIGCHLD cleanup");
    return 0;
}
