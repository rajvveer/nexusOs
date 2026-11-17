#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] LOCK FAIL line=%d errno=%d pid=%d\n", __LINE__, errno, getpid()); _exit(99); } } while (0)
static volatile sig_atomic_t seen;
static void signal_handler(int number) { seen = number; }
static int set(int fd, int command, short type, off_t start, off_t length, short whence) {
    struct flock lock = {.l_type = type, .l_whence = whence, .l_start = start, .l_len = length};
    return fcntl(fd, command, &lock);
}
static struct flock query(int fd, int command, short type, off_t start, off_t length) {
    struct flock lock = {.l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = length};
    CHECK(fcntl(fd, command, &lock) == 0); return lock;
}
static void waited(pid_t child) { int status; CHECK(waitpid(child, &status, 0) == child && status == 0); }
static int reopen(int fd, int flags) { char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", fd); int result = open(path, flags); CHECK(result >= 0); return result; }
static void byte_read(int fd) { char byte; CHECK(read(fd, &byte, 1) == 1); }
static void ranges(int fd) {
    CHECK(set(fd, F_SETLK, F_WRLCK, 10, 20, SEEK_SET) == 0);
    CHECK(query(fd, F_GETLK, F_WRLCK, 0, 100).l_type == F_UNLCK);
    pid_t parent = getpid(), child = fork(); CHECK(child >= 0);
    if (!child) {
        struct flock lock = query(fd, F_GETLK, F_RDLCK, 0, 100);
        CHECK(lock.l_type == F_WRLCK && lock.l_start == 10 && lock.l_len == 20 && lock.l_pid == parent);
        CHECK(set(fd, F_SETLK, F_RDLCK, 10, 1, SEEK_SET) == -1 && errno == EAGAIN);
        CHECK(set(fd, F_SETLK, F_WRLCK, 0, 10, SEEK_SET) == 0); _exit(0);
    }
    waited(child); CHECK(set(fd, F_SETLK, F_UNLCK, 0, 0, SEEK_SET) == 0);
    CHECK(set(fd, F_SETLK, F_RDLCK, 0, 100, SEEK_SET) == 0 && set(fd, F_SETLK, F_WRLCK, 20, 20, SEEK_SET) == 0);
    CHECK(set(fd, F_SETLK, F_UNLCK, 25, 50, SEEK_SET) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        struct flock a = query(fd, F_GETLK, F_RDLCK, 0, 100), b = query(fd, F_GETLK, F_WRLCK, 70, 40);
        CHECK(a.l_type == F_WRLCK && a.l_start == 20 && a.l_len == 5);
        CHECK(b.l_type == F_RDLCK && b.l_start == 75 && b.l_len == 25);
        CHECK(set(fd, F_SETLK, F_WRLCK, 25, 50, SEEK_SET) == 0); _exit(0);
    }
    waited(child); CHECK(set(fd, F_SETLK, F_UNLCK, 0, 0, SEEK_SET) == 0);
    CHECK(lseek(fd, 50, SEEK_SET) == 50 && set(fd, F_SETLK, F_WRLCK, -5, -5, SEEK_CUR) == 0);
    CHECK(set(fd, F_SETLK, F_WRLCK, -10, 0, SEEK_END) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        struct flock a = query(fd, F_GETLK, F_RDLCK, 40, 1), b = query(fd, F_GETLK, F_RDLCK, 500, 1);
        CHECK(a.l_start == 40 && a.l_len == 5 && b.l_start == 246 && b.l_len == 0); _exit(0);
    }
    waited(child); CHECK(set(fd, F_SETLK, F_UNLCK, 0, 0, SEEK_SET) == 0);
    CHECK(set(fd, F_SETLK, F_WRLCK, -1, 1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(set(fd, F_SETLK, F_WRLCK, INT64_MAX, 2, SEEK_SET) == -1 && errno == EOVERFLOW);
    CHECK(syscall(SYS_fcntl, fd, F_GETLK, (void*)1) == -1 && errno == EFAULT);
}
static void descriptions(int fd) {
    int other = reopen(fd, O_RDWR), alias = dup(fd); CHECK(alias >= 0);
    CHECK(set(fd, F_OFD_SETLK, F_WRLCK, 0, 16, SEEK_SET) == 0);
    CHECK(query(alias, F_OFD_GETLK, F_RDLCK, 0, 16).l_type == F_UNLCK);
    struct flock lock = query(other, F_GETLK, F_RDLCK, 0, 16); CHECK(lock.l_type == F_WRLCK && lock.l_pid == -1);
    CHECK(set(other, F_OFD_SETLK, F_WRLCK, 0, 16, SEEK_SET) == -1 && errno == EAGAIN);
    CHECK(set(fd, F_SETLK, F_WRLCK, 0, 16, SEEK_SET) == -1 && errno == EAGAIN);
    CHECK(close(alias) == 0 && set(fd, F_OFD_SETLK, F_UNLCK, 0, 0, SEEK_SET) == 0);
    int owned = reopen(fd, O_RDWR), copy = dup(owned); CHECK(copy >= 0);
    CHECK(set(owned, F_OFD_SETLK, F_WRLCK, 40, 1, SEEK_SET) == 0 && close(owned) == 0);
    CHECK(query(fd, F_GETLK, F_RDLCK, 40, 1).l_pid == -1);
    CHECK(close(copy) == 0 && query(fd, F_GETLK, F_RDLCK, 40, 1).l_type == F_UNLCK);
    int ro = reopen(fd, O_RDONLY);
    CHECK(set(ro, F_SETLK, F_WRLCK, 0, 1, SEEK_SET) == -1 && errno == EBADF);
    CHECK(flock(ro, LOCK_EX | LOCK_NB) == 0 && flock(other, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    alias = dup(ro); CHECK(alias >= 0 && close(ro) == 0);
    CHECK(flock(other, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK && flock(alias, LOCK_UN) == 0);
    CHECK(flock(alias, LOCK_SH) == 0 && flock(other, LOCK_SH) == 0);
    CHECK(flock(alias, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK && flock(other, LOCK_UN) == 0);
    CHECK(flock(other, LOCK_EX | LOCK_NB) == 0);
    CHECK(set(fd, F_SETLK, F_WRLCK, 0, 0, SEEK_SET) == 0 && flock(other, LOCK_UN) == 0);
    CHECK(close(alias) == 0 && query(fd, F_GETLK, F_WRLCK, 0, 0).l_type == F_UNLCK);
    CHECK(close(other) == 0);
    /* Closing an independently opened descriptor releases this process's ranges. */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { CHECK(set(fd, F_SETLK, F_WRLCK, 0, 0, SEEK_SET) == 0); _exit(0); }
    waited(child);
}
static void deadlocks(int fd) {
    int ready[2]; CHECK(pipe(ready) == 0 && set(fd, F_SETLK, F_WRLCK, 0, 1, SEEK_SET) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(set(fd, F_SETLK, F_WRLCK, 1, 1, SEEK_SET) == 0 && write(ready[1], "r", 1) == 1);
        CHECK(set(fd, F_SETLKW, F_WRLCK, 0, 1, SEEK_SET) == 0); _exit(0);
    }
    byte_read(ready[0]); CHECK(poll(NULL, 0, 80) == 0);
    CHECK(set(fd, F_SETLKW, F_WRLCK, 1, 1, SEEK_SET) == -1 && errno == EDEADLK);
    CHECK(set(fd, F_SETLK, F_UNLCK, 0, 0, SEEK_SET) == 0); waited(child);
    CHECK(close(ready[0]) == 0 && close(ready[1]) == 0);
}
static void interruption(int fd, int restart) {
    struct sigaction action = {.sa_handler = signal_handler, .sa_flags = restart ? SA_RESTART : 0}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0); seen = 0;
    int ready[2]; CHECK(pipe(ready) == 0); pid_t parent = getpid(), child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(set(fd, F_SETLK, F_WRLCK, 0, 1, SEEK_SET) == 0 && write(ready[1], "r", 1) == 1);
        CHECK(poll(NULL, 0, 120) == 0 && kill(parent, SIGUSR1) == 0 && poll(NULL, 0, 120) == 0); _exit(0);
    }
    byte_read(ready[0]); int result = set(fd, F_SETLKW, F_WRLCK, 0, 1, SEEK_SET);
    CHECK(seen == SIGUSR1 && (restart ? result == 0 : result == -1 && errno == EINTR));
    waited(child); CHECK(set(fd, F_SETLK, F_UNLCK, 0, 0, SEEK_SET) == 0); close(ready[0]); close(ready[1]);
}
static int thread_fd;
static int canceled_fd, canceled_result, canceled_error;
static void* canceled_lock(void* unused) {
    (void)unused; canceled_result = set(canceled_fd, F_SETLKW, F_WRLCK, 0, 1, SEEK_SET); canceled_error = errno; return NULL;
}
static void close_waiter(int fd) {
    int ready[2], gate[2]; CHECK(pipe(ready) == 0 && pipe(gate) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(set(fd, F_SETLK, F_WRLCK, 0, 1, SEEK_SET) == 0 && write(ready[1], "r", 1) == 1);
        byte_read(gate[0]); _exit(0);
    }
    byte_read(ready[0]); canceled_fd = reopen(fd, O_RDWR);
    pthread_t worker; CHECK(pthread_create(&worker, NULL, canceled_lock, NULL) == 0);
    CHECK(poll(NULL, 0, 120) == 0 && close(canceled_fd) == 0 && pthread_join(worker, NULL) == 0);
    CHECK(canceled_result == -1 && canceled_error == EBADF);
    CHECK(write(gate[1], "g", 1) == 1); waited(child);
    close(ready[0]); close(ready[1]); close(gate[0]); close(gate[1]);
}
static void* thread_lock(void* unused) {
    (void)unused; CHECK(set(thread_fd, F_SETLK, F_WRLCK, 0, 1, SEEK_SET) == 0);
    int extra = reopen(thread_fd, O_RDONLY); CHECK(close(extra) == 0); return NULL;
}
static void execution(int fd) {
    for (int drop = 0; drop < 2; drop++) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) {
            CHECK(set(fd, F_SETLK, F_WRLCK, 11, 1, SEEK_SET) == 0);
            if (drop) (void)reopen(fd, O_RDONLY | O_CLOEXEC);
            char number[24]; snprintf(number, sizeof(number), "%d", fd);
            char* args[] = {"/bin/locks64", drop ? "dropped" : "exec", number, NULL}; char* env[] = {NULL};
            execve(args[0], args, env); CHECK(0);
        }
        waited(child);
    }
}
int main(int argc, char** argv) {
    if (argc == 3) {
        int fd = atoi(argv[2]); pid_t parent = getpid(), child = fork(); CHECK(child >= 0);
        if (!child) {
            struct flock lock = query(fd, F_GETLK, F_RDLCK, 11, 1);
            CHECK(argv[1][0] == 'd' ? lock.l_type == F_UNLCK : lock.l_type == F_WRLCK && lock.l_pid == parent); _exit(0);
        }
        waited(child); return 0;
    }
    CHECK(argc == 2);
    int fd = argv[1][0] == 'd' ? open("/tmp/native64-locks", O_RDWR | O_CREAT | O_EXCL, 0600) : memfd_create("locks", 0);
    CHECK(fd >= 0 && ftruncate(fd, 256) == 0);
    ranges(fd); descriptions(fd); deadlocks(fd); interruption(fd, 0); interruption(fd, 1);
    close_waiter(fd);
    CHECK(set(fd, F_SETLK, F_WRLCK, 0, 1, SEEK_SET) == 0); thread_fd = fd;
    pthread_t thread; CHECK(pthread_create(&thread, NULL, thread_lock, NULL) == 0 && pthread_join(thread, NULL) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { CHECK(set(fd, F_SETLK, F_WRLCK, 0, 1, SEEK_SET) == 0); _exit(0); }
    waited(child); execution(fd);
    CHECK(close(fd) == 0);
    if (argv[1][0] == 'd') CHECK(unlink("/tmp/native64-locks") == 0);
    puts("[64] LOCK ALL PASSED: record/OFD/flock contention, range conversion, deadlock, signals, close/exit and exec inheritance");
    return 0;
}
