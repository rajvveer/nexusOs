#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] PID FAIL line=%d errno=%d pid=%d\n", __LINE__, errno, getpid()); _exit(99); } } while (0)
static volatile sig_atomic_t sender = -1;
static void handler(int signal, siginfo_t* info, void* context) { (void)signal; (void)context; sender = info->si_pid; }
static void byte_read(int fd) {
    char c; ssize_t n; do { n = read(fd, &c, 1); } while (n < 0 && errno == EINTR); CHECK(n == 1);
}
static void wait_status(pid_t pid, int expected) {
    siginfo_t info; int error; do { error = waitid(P_PID, pid, &info, WEXITED | WNOWAIT); } while (error && errno == EINTR);
    CHECK(error == 0 && info.si_pid == pid && info.si_status == ((expected & 127) ? expected & 127 : expected >> 8));
    int status; pid_t result; do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    CHECK(result == pid && status == expected);
}
static void* thread(void* unused) {
    (void)unused; CHECK(getpid() == 1 && getppid() == 0 && syscall(SYS_gettid) > 1);
    CHECK(syscall(SYS_tgkill, 1, syscall(SYS_gettid), 0) == 0);
    CHECK(unshare(CLONE_NEWPID) == -1 && errno == EINVAL); return NULL;
}
static void orphans(void) {
    int gate[2], ids[2]; CHECK(pipe(gate) == 0 && pipe(ids) == 0);
    pid_t middle = fork(); CHECK(middle >= 0);
    if (!middle) {
        pid_t orphan = fork(); CHECK(orphan >= 0);
        if (orphan) { CHECK(write(ids[1], &orphan, sizeof(orphan)) == sizeof(orphan)); _exit(0); }
        byte_read(gate[0]); CHECK(getppid() == 1); _exit(31);
    }
    pid_t orphan; CHECK(read(ids[0], &orphan, sizeof(orphan)) == sizeof(orphan));
    wait_status(middle, 0); CHECK(write(gate[1], "x", 1) == 1); wait_status(orphan, 31 << 8);
    close(gate[0]); close(gate[1]); close(ids[0]); close(ids[1]);
}
static void nested(unsigned depth) {
    CHECK(getpid() == 1 && getppid() == 0);
    pid_t child = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, NULL);
    if (depth == 32) { CHECK(child == -1 && errno == ENOSPC); return; }
    CHECK(child > 1 || child == 0);
    if (!child) { nested(depth + 1); _exit(0); }
    wait_status(child, 0);
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (argv[1][0] == 'e') {
        CHECK(getpid() == 1 && getppid() == 0 && syscall(SYS_gettid) == 1);
        puts("[64] PID exec preserves namespace identity PASS"); return 0;
    }
    CHECK(unshare(CLONE_NEWPID) == -1 && errno == EPERM);
    CHECK(syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, NULL) == -1 && errno == EPERM);
    int pair[2], enabled = 1; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    int old_namespace = open("/proc/self/ns/pid", O_RDONLY); CHECK(old_namespace >= 0);
    struct stat initial, retained; CHECK(fstat(old_namespace, &initial) == 0);
    pid_t init = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, NULL);
    CHECK(init >= 0);
    if (!init) {
        close(pair[0]);
        CHECK(getpid() == 1 && syscall(SYS_gettid) == 1 && getppid() == 0 && getpgrp() == 1 && getsid(0) == 1);
        CHECK(setsid() == -1 && errno == EPERM);
        CHECK(stat("/proc/self/ns/pid", &retained) == 0 && retained.st_ino != initial.st_ino);
        char proc_id[24] = {0}; CHECK(readlink("/proc/self", proc_id, sizeof(proc_id) - 1) > 0 && strcmp(proc_id, "1"));
        CHECK(fstat(old_namespace, &retained) == 0 && retained.st_ino == initial.st_ino); close(old_namespace);
        struct ucred peer; socklen_t size = sizeof(peer);
        CHECK(getsockopt(pair[1], SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.pid == 0);
        CHECK(write(pair[1], "I", 1) == 1); byte_read(pair[1]); CHECK(sender == 0);
        CHECK(kill(1, SIGTERM) == 0 && kill(1, SIGKILL) == 0 && getpid() == 1);
        CHECK(kill(99999, 0) == -1 && errno == ESRCH);
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) {
            CHECK(getpid() == 2 && getppid() == 1 && syscall(SYS_gettid) == 2);
            CHECK(setsid() == 2 && getpgrp() == 2 && getsid(0) == 2);
            CHECK(kill(1, SIGUSR1) == 0);
            CHECK(write(pair[1], "C", 1) == 1); _exit(23);
        }
        CHECK(child == 2); wait_status(child, 23 << 8); CHECK(sender == 2);
        pthread_t worker; CHECK(pthread_create(&worker, NULL, thread, NULL) == 0 && pthread_join(worker, NULL) == 0);
        orphans();
        int gate[2]; CHECK(pipe(gate) == 0);
        child = fork(); CHECK(child >= 0);
        if (!child) { byte_read(gate[0]); CHECK(0); }
        _exit(0);
    }
    close(pair[1]); close(old_namespace);
    byte_read(pair[0]);
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/ns/pid", init);
    int held = open(path, O_RDONLY); CHECK(held >= 0 && fstat(held, &retained) == 0 && retained.st_ino != initial.st_ino);
    CHECK(kill(init, SIGUSR1) == 0 && write(pair[0], "g", 1) == 1);
    wait_status(init, 0);
    char payload, controls[CMSG_SPACE(sizeof(struct ucred))]; struct iovec vector = {&payload, 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_control = controls, .msg_controllen = sizeof(controls)};
    CHECK(recvmsg(pair[0], &message, 0) == 1 && payload == 'C');
    struct cmsghdr* control = CMSG_FIRSTHDR(&message); CHECK(control && control->cmsg_type == SCM_CREDENTIALS);
    struct ucred credentials; memcpy(&credentials, CMSG_DATA(control), sizeof(credentials));
    CHECK(credentials.pid > init && credentials.uid == 1000);
    CHECK(read(pair[0], &payload, 1) == 0); close(pair[0]);
    CHECK(fstat(held, &initial) == 0 && initial.st_ino == retained.st_ino); close(held);
    puts("[64] PID clone/threads, parent visibility, signals, reaping, retained IPC identities and init death PASS");
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        pid_t before = getpid(); CHECK(unshare(CLONE_NEWUSER | CLONE_NEWPID) == 0 && getpid() == before);
        CHECK(unshare(CLONE_NEWPID) == -1 && errno == EINVAL);
        CHECK(stat("/proc/self/ns/pid_for_children", &retained) == -1 && errno == ENOENT);
        CHECK(syscall(SYS_clone, CLONE_THREAD | CLONE_VM | CLONE_SIGHAND, NULL, NULL, NULL, NULL) == -1 && errno == EINVAL);
        pid_t first = fork(); CHECK(first >= 0);
        if (!first) { CHECK(getpid() == 1 && getppid() == 0); _exit(0); }
        wait_status(first, 0);
        CHECK(fork() == -1 && errno == ENOMEM);
        CHECK(stat("/proc/self/ns/pid_for_children", &retained) == 0); _exit(0);
    }
    wait_status(child, 0);
    child = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, NULL); CHECK(child >= 0);
    if (!child) { for (;;) syscall(SYS_sched_yield); }
    CHECK(kill(child, SIGKILL) == 0); wait_status(child, SIGKILL);
    child = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, NULL); CHECK(child >= 0);
    if (!child) { nested(1); _exit(0); } wait_status(child, 0);
    child = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, NULL); CHECK(child >= 0);
    if (!child) {
        char* arguments[] = {"/bin/pid64", "e", NULL}; char* environment[] = {NULL};
        execve(arguments[0], arguments, environment); CHECK(0);
    }
    wait_status(child, 0);
    puts("[64] PID ALL PASSED: real namespaces, unshare future children, dead namespace refusal, nesting and exec");
    return 0;
}
