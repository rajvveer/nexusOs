#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] LIMIT FAIL line=%d errno=%d pid=%d\n", __LINE__, errno, getpid()); _exit(99); } } while (0)
#define MB (1024ULL * 1024)
static struct rlimit limit(int resource) { struct rlimit value; CHECK(getrlimit(resource, &value) == 0); return value; }
static void soft(int resource, rlim_t value) { struct rlimit old = limit(resource); old.rlim_cur = value; CHECK(setrlimit(resource, &old) == 0); }
static void waited(pid_t child) { int status; CHECK(child > 0 && waitpid(child, &status, 0) == child && status == 0); }
static void* set_thread_limit(void* unused) { (void)unused; soft(RLIMIT_NOFILE, 48); return NULL; }
static int send_fd(int socket, int fd) {
    char byte = 'x', control[CMSG_SPACE(sizeof(fd))] = {0}; struct iovec vector = {&byte, 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd)); return sendmsg(socket, &message, 0);
}
static void descriptor_limits(void) {
    int fd = open("/dev/null", O_RDWR), root = open("/", O_RDONLY | O_DIRECTORY); CHECK(fd == 3 && root == 4 && dup2(fd, 10) == 10);
    int memory = syscall(SYS_memfd_create, "limit-truncate", 0); CHECK(memory >= 0 && ftruncate(memory, 4096) == 0);
    struct rlimit original = limit(RLIMIT_NOFILE); soft(RLIMIT_NOFILE, 4);
    struct stat stat;
    CHECK(open("/tmp/limit-must-not-exist", O_WRONLY | O_CREAT, 0600) == -1 && errno == EMFILE);
    CHECK(lstat("/tmp/limit-must-not-exist", &stat) == -1 && errno == ENOENT);
    char path[80]; snprintf(path, sizeof(path), "/proc/self/fd/%d", memory);
    CHECK(open(path, O_RDWR | O_TRUNC) == -1 && errno == EMFILE && fstat(memory, &stat) == 0 && stat.st_size == 4096);
    CHECK(open("/dev/null", O_RDONLY) == -1 && errno == EMFILE);
    CHECK(dup(fd) == -1 && errno == EMFILE);
    CHECK(dup2(fd, 4) == -1 && errno == EBADF && fcntl(fd, F_DUPFD, 4) == -1 && errno == EINVAL);
    CHECK(dup2(10, 10) == 10 && write(10, "x", 1) == 1);
    CHECK(close(fd) == 0 && open("/dev/null", O_RDONLY) == 3 && close(3) == 0);
    soft(RLIMIT_NOFILE, 3);
    CHECK(chdir("/etc") == 0); char cwd[40]; CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/etc"));
    CHECK(fchdir(root) == 0 && open("/dev/null", O_RDONLY) == -1 && errno == EMFILE);
    CHECK(unshare(CLONE_NEWUSER) == 0 && chroot("/etc") == 0 && chdir("/") == 0);
    CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));
    CHECK(setrlimit(RLIMIT_NOFILE, &(struct rlimit){3, 8}) == 0);
    CHECK(setrlimit(RLIMIT_NOFILE, &(struct rlimit){3, 9}) == -1 && errno == EPERM);
    CHECK(setrlimit(RLIMIT_NOFILE, &(struct rlimit){9, 8}) == -1 && errno == EINVAL);
    struct rlimit alias = {2, 8}; CHECK(prlimit(0, RLIMIT_NOFILE, &alias, &alias) == 0 && alias.rlim_cur == 3 && alias.rlim_max == 8);
    CHECK(limit(RLIMIT_NOFILE).rlim_cur == 2 && original.rlim_cur >= 11);
    CHECK(close(root) == 0 && close(10) == 0 && close(memory) == 0);
}
static void inflight_limits(void) {
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int fd = open("/dev/null", O_RDONLY); CHECK(fd >= 0); soft(RLIMIT_NOFILE, 3);
    for (unsigned i = 0; i < 4; i++) CHECK(send_fd(pair[0], fd) == 1);
    CHECK(send_fd(pair[0], fd) == -1 && errno == ETOOMANYREFS);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { CHECK(send_fd(pair[0], fd) == -1 && errno == ETOOMANYREFS); _exit(0); }
    waited(child);
    char byte, control[64]; struct iovec vector = {&byte, 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(pair[1], &message, 0) == 1 && (message.msg_flags & MSG_CTRUNC) && message.msg_controllen == 0);
    CHECK(send_fd(pair[0], fd) == 1);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0 && close(fd) == 0);
}
/* RLIMIT_MEMLOCK: default 8 MiB, cumulative per address space, Linux error codes, not inherited by fork. */
static long locked_kb(void) {
    char text[4096]; int fd = open("/proc/self/status", O_RDONLY); CHECK(fd >= 0);
    ssize_t got = read(fd, text, sizeof(text) - 1); close(fd); CHECK(got > 0); text[got] = 0;
    char* line = strstr(text, "VmLck:"); CHECK(line); return strtol(line + 6, NULL, 10);
}
static void memlock_limits(void) {
    struct rlimit value = limit(RLIMIT_MEMLOCK);
    CHECK(value.rlim_cur == 8 * MB && value.rlim_max == 8 * MB);
    if (geteuid() == 0) {   /* CAP_IPC_LOCK passes the limit; then drop every privilege */
        char* big = mmap(NULL, 12 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(big != MAP_FAILED);
        CHECK(mlock(big, 12 * MB) == 0 && locked_kb() == 12 * 1024 && munmap(big, 12 * MB) == 0 && locked_kb() == 0);
        CHECK(setgroups(0, NULL) == 0 && setresgid(65534, 65534, 65534) == 0 && setresuid(65534, 65534, 65534) == 0);
    }
    char* region = mmap(NULL, 12 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(region != MAP_FAILED);
    CHECK(mlock(region, 6 * MB) == 0 && locked_kb() == 6 * 1024);
    CHECK(mlock(region + 6 * MB, 4 * MB) == -1 && errno == ENOMEM && locked_kb() == 6 * 1024);   /* cumulative */
    CHECK(mlock(region, 6 * MB) == 0 && locked_kb() == 6 * 1024);                                 /* no double count */
    CHECK(mlock(region + 6 * MB, 2 * MB) == 0 && locked_kb() == 8 * 1024);
    CHECK(syscall(SYS_mlock2, region + 8 * MB, 4096, 1) == -1 && errno == ENOMEM);              /* MLOCK_ONFAULT too */
    CHECK(munlock(region, 4 * MB) == 0 && locked_kb() == 4 * 1024);
    CHECK(syscall(SYS_mlock2, region + 8 * MB, 4 * MB, 1) == 0 && locked_kb() == 8 * 1024);
    CHECK(munmap(region + 8 * MB, 4 * MB) == 0 && locked_kb() == 4 * 1024);                     /* unmapping unlocks */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { CHECK(locked_kb() == 0 && limit(RLIMIT_MEMLOCK).rlim_cur == 8 * MB); _exit(0); }  /* fork: no locks */
    waited(child);
    CHECK(mmap(NULL, 8 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0) == MAP_FAILED && errno == EAGAIN);
    char* pinned = mmap(NULL, 4 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    CHECK(pinned != MAP_FAILED && locked_kb() == 8 * 1024);
    CHECK(mremap(pinned, 4 * MB, 5 * MB, MREMAP_MAYMOVE) == MAP_FAILED && errno == EAGAIN);      /* growing a locked area */
    CHECK(mlockall(MCL_FUTURE) == 0);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EAGAIN);
    CHECK(mlockall(MCL_CURRENT) == -1 && errno == ENOMEM);   /* the whole mapped size must fit */
    CHECK(munlockall() == 0 && locked_kb() == 0);
    void* free_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(free_page != MAP_FAILED && munmap(free_page, 4096) == 0 && locked_kb() == 0);
    int file = syscall(SYS_memfd_create, "memlock-eof", 0); CHECK(file >= 0 && ftruncate(file, 4096) == 0);
    char* past = mmap(NULL, 2 * 4096, PROT_READ, MAP_SHARED, file, 0); CHECK(past != MAP_FAILED);
    CHECK(mlock(past, 2 * 4096) == -1 && errno == ENOMEM);   /* a page past EOF cannot be populated */
    CHECK(munmap(past, 2 * 4096) == 0 && close(file) == 0 && locked_kb() == 0);
    CHECK(setrlimit(RLIMIT_MEMLOCK, &(struct rlimit){16 * MB, 8 * MB}) == -1 && errno == EINVAL);
    CHECK(setrlimit(RLIMIT_MEMLOCK, &(struct rlimit){8 * MB, 16 * MB}) == -1 && errno == EPERM);
    CHECK(setrlimit(RLIMIT_MEMLOCK, &(struct rlimit){0, 8 * MB}) == 0);
    CHECK(mlock(region, 4096) == -1 && errno == EPERM && munlock(region, 4096) == 0);
    CHECK(mlockall(MCL_CURRENT) == -1 && errno == EPERM);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0) == MAP_FAILED && errno == EPERM);
    child = fork(); CHECK(child >= 0);
    if (!child) { CHECK(limit(RLIMIT_MEMLOCK).rlim_cur == 0 && limit(RLIMIT_MEMLOCK).rlim_max == 8 * MB); _exit(0); }  /* inherited */
    waited(child);
    puts("[64] LIMIT MEMLOCK PASSED: default 8 MiB, cumulative, VmLck, MAP_LOCKED/MCL_FUTURE/mremap EAGAIN, EPERM at 0, fork");
    fflush(stdout);   /* the caller leaves with _exit */
}
static void memory_limits(void) {
    struct rlimit data = limit(RLIMIT_DATA), address = limit(RLIMIT_AS);
    volatile unsigned char* keep = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(keep != MAP_FAILED);
    keep[0] = 90; CHECK(mprotect((void*)keep, 4096, PROT_READ) == 0);
    soft(RLIMIT_DATA, 1);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    int file = open("/etc/native64.dat", O_RDONLY); CHECK(file >= 0);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0) == MAP_FAILED && errno == ENOMEM); close(file);
    CHECK(mprotect((void*)keep, 4096, PROT_READ | PROT_WRITE) == -1 && errno == ENOMEM && keep[0] == 90);
    CHECK(mmap((void*)keep, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED && errno == ENOMEM && keep[0] == 90);
    void* reserve = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(reserve != MAP_FAILED);
    CHECK(mprotect(reserve, 4096, PROT_READ | PROT_WRITE) == -1 && errno == ENOMEM);
    unsigned char* shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); CHECK(shared != MAP_FAILED);
    shared[0] = 41; CHECK(shared[0] == 41 && munmap(shared, 4096) == 0);
    uintptr_t old_break = syscall(SYS_brk, 0);
    CHECK((uintptr_t)syscall(SYS_brk, old_break + 4096) == old_break);
    CHECK(setrlimit(RLIMIT_DATA, &data) == 0 && mprotect(reserve, 4096, PROT_READ | PROT_WRITE) == 0);
    CHECK((uintptr_t)syscall(SYS_brk, old_break + 4096) == old_break + 4096 && (uintptr_t)syscall(SYS_brk, old_break) == old_break);
    CHECK(munmap(reserve, 4096) == 0);
    soft(RLIMIT_DATA, 64 * MB + 1023);
    CHECK(mmap(NULL, 64 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    reserve = mmap(NULL, MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(reserve != MAP_FAILED && munmap(reserve, MB) == 0);
    CHECK(setrlimit(RLIMIT_DATA, &data) == 0);
    soft(RLIMIT_AS, 1);
    CHECK(mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    CHECK((uintptr_t)syscall(SYS_brk, old_break + 4096) == old_break && keep[0] == 90);
    CHECK(setrlimit(RLIMIT_AS, &address) == 0 && munmap((void*)keep, 4096) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        soft(RLIMIT_DATA, 1); char* args[] = {"/bin/limits64", "e", NULL}; char* env[] = {NULL};
        CHECK(execve(args[0], args, env) == -1 && errno == ENOMEM);
        CHECK(setrlimit(RLIMIT_DATA, &data) == 0); _exit(0);
    }
    waited(child);
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (argv[1][0] == 'e' || argv[1][0] == 's') {
        CHECK(limit(RLIMIT_NOFILE).rlim_cur == 64 && limit(RLIMIT_DATA).rlim_cur == (argv[1][0] == 's' ? 4 : 64) * MB && limit(RLIMIT_AS).rlim_cur == 256 * MB);
        puts("[64] LIMIT exec preserves resource state PASS"); return 0;
    }
    struct rlimit original = limit(RLIMIT_NOFILE), old;
    CHECK(original.rlim_cur == 1024 && original.rlim_max == 1048576);
    CHECK(syscall(SYS_setrlimit, RLIMIT_NOFILE, &original) == 0 && syscall(SYS_getrlimit, RLIMIT_NOFILE, &old) == 0 && old.rlim_cur == original.rlim_cur);
    CHECK(prlimit(0, 16, NULL, &old) == -1 && errno == EINVAL);
    CHECK(prlimit(99999, RLIMIT_NOFILE, NULL, &old) == -1 && errno == ESRCH);
    CHECK(prlimit(0, RLIMIT_NOFILE, (void*)1, &old) == -1 && errno == EFAULT);
    CHECK(prlimit(0, RLIMIT_NOFILE, &(struct rlimit){8, original.rlim_max}, (void*)1) == -1 && errno == EFAULT);
    CHECK(limit(RLIMIT_NOFILE).rlim_cur == original.rlim_cur);
    CHECK(setrlimit(RLIMIT_NOFILE, &(struct rlimit){8, 0x80000001ULL}) == -1 && errno == EPERM);
    void* protected = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(protected != MAP_FAILED);
    CHECK(mprotect(protected, 4096, PROT_READ) == 0);
    CHECK(syscall(SYS_getrlimit, RLIMIT_MEMLOCK, protected) == -1 && errno == EFAULT);
    CHECK(prlimit(0, RLIMIT_MEMLOCK, NULL, protected) == -1 && errno == EFAULT);
    CHECK(mprotect(protected, 4096, PROT_READ | PROT_WRITE) == 0);
    CHECK(prlimit(0, RLIMIT_MEMLOCK, NULL, protected) == 0 && munmap(protected, 4096) == 0);
    pid_t child = fork(); CHECK(child >= 0); if (!child) { descriptor_limits(); _exit(0); } waited(child);
    child = fork(); CHECK(child >= 0); if (!child) { memlock_limits(); _exit(0); } waited(child);
    child = fork(); CHECK(child >= 0); if (!child) { inflight_limits(); _exit(0); } waited(child);
    CHECK(limit(RLIMIT_NOFILE).rlim_cur == original.rlim_cur);
    memory_limits();
    int gate[2]; CHECK(pipe(gate) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) { char byte; CHECK(read(gate[0], &byte, 1) == 1 && limit(RLIMIT_NOFILE).rlim_cur == 16); _exit(0); }
    CHECK(prlimit(child, RLIMIT_NOFILE, &(struct rlimit){16, original.rlim_max}, &old) == 0 && old.rlim_cur == original.rlim_cur);
    CHECK(write(gate[1], "g", 1) == 1); waited(child); close(gate[0]); close(gate[1]);
    soft(RLIMIT_NOFILE, 64); soft(RLIMIT_DATA, 64 * MB); soft(RLIMIT_AS, 256 * MB);
    pthread_t worker; CHECK(pthread_create(&worker, NULL, set_thread_limit, NULL) == 0 && pthread_join(worker, NULL) == 0);
    CHECK(limit(RLIMIT_NOFILE).rlim_cur == 48); soft(RLIMIT_NOFILE, 64);
    child = fork(); CHECK(child >= 0);
    if (!child) { char* args[] = {"/bin/limits64", "e", NULL}; char* env[] = {NULL}; execve(args[0], args, env); CHECK(0); }
    waited(child);
    CHECK(limit(RLIMIT_NOFILE).rlim_cur == 64);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        soft(RLIMIT_DATA, 4 * MB);
        char* args[] = {"/bin/limits64", "s", NULL}; char* env[] = {NULL}; execve(args[0], args, env); CHECK(0);
    }
    waited(child);
    puts("[64] LIMIT ALL PASSED: inherited/shared quotas, hard-limit privilege, FD/SCM enforcement, VM accounting and failed-operation preservation");
    return 0;
}
