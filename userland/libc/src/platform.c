/* Picolibc's POSIX hooks for the NexusOS process ABI. */
#define _POSIX_MONOTONIC_CLOCK 200809L
#include "../../../kernel/usys_abi.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <nexus.h>
#undef st_mtime

static int call(int nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory", "cc");
    return r;
}
static int result(int r) {
    if (r >= 0) return r;
    int e = -r;
    switch (e) {
    case NX_ENAMETOOLONG: e = ENAMETOOLONG; break;
    case NX_ENOSYS: e = ENOSYS; break;
    case NX_ENOTEMPTY: e = ENOTEMPTY; break;
    }
    errno = e; return -1;
}

void _exit(int code) { call(NX_SYS_EXIT, code, 0, 0); for (;;) {} }
extern char **environ;
extern int main(int, char **);
void __libc_start_main(int argc, char **argv, char **envp) {
    environ = envp;
    exit(main(argc, argv));
}
void *sbrk(intptr_t n) {
    int r = call(NX_SYS_SBRK, n, 0, 0);
    if (r < 0) { errno = ENOMEM; return (void *)-1; }
    return (void *)(uintptr_t)r;
}
static int nxflags(int flags) {
    int n = flags & O_ACCMODE;
    if (flags & O_CREAT) n |= NX_O_CREAT;
    if (flags & O_EXCL) n |= NX_O_EXCL;
    if (flags & O_TRUNC) n |= NX_O_TRUNC;
    if (flags & O_APPEND) n |= NX_O_APPEND;
    if (flags & O_NONBLOCK) n |= NX_O_NONBLOCK;
    if (flags & O_DIRECTORY) n |= NX_O_DIRECTORY;
    return n;
}
int open(const char *path, int flags, ...) {
    mode_t mode = 0666;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return result(call(NX_SYS_FD_OPEN, (uintptr_t)path, nxflags(flags), mode));
}
int close(int fd) { return result(call(NX_SYS_FD_CLOSE, fd, 0, 0)); }
ssize_t read(int fd, void *p, size_t n) { return result(call(NX_SYS_FD_READ, fd, (uintptr_t)p, n)); }
ssize_t write(int fd, const void *p, size_t n) { return result(call(NX_SYS_FD_WRITE, fd, (uintptr_t)p, n)); }
off_t lseek(int fd, off_t off, int whence) { return result(call(NX_SYS_LSEEK, fd, off, whence)); }
int ftruncate(int fd, off_t len) { return result(call(NX_SYS_FTRUNCATE, fd, len, 0)); }
int unlink(const char *p) { return result(call(NX_SYS_UNLINK, (uintptr_t)p, 0, 0)); }
int rename(const char *a, const char *b) { return result(call(NX_SYS_RENAME, (uintptr_t)a, (uintptr_t)b, 0)); }
int mkdir(const char *p, mode_t mode) { return result(call(NX_SYS_MKDIR, (uintptr_t)p, mode, 0)); }
int rmdir(const char *p) { return result(call(NX_SYS_RMDIR, (uintptr_t)p, 0, 0)); }
int chdir(const char *p) { return result(call(NX_SYS_CHDIR, (uintptr_t)p, 0, 0)); }
char *getcwd(char *p, size_t n) {
    if (!p || !n) { errno = EINVAL; return NULL; }
    return result(call(NX_SYS_GETCWD, (uintptr_t)p, n, 0)) < 0 ? NULL : p;
}
int access(const char *p, int mode) { return result(call(NX_SYS_ACCESS, (uintptr_t)p, mode, 0)); }
int isatty(int fd) { int r = result(call(NX_SYS_ISATTY, fd, 0, 0)); return r > 0; }
pid_t getpid(void) { return call(NX_SYS_GETPID, 0, 0, 0); }
pid_t getppid(void) { return call(NX_SYS_GETPPID, 0, 0, 0); }
int kill(pid_t pid, int sig) { return result(call(NX_SYS_KILL, pid, sig, 0)); }
int dup(int fd) { return result(call(NX_SYS_DUP, fd, 0, 0)); }
int dup2(int fd, int to) { return result(call(NX_SYS_DUP2, fd, to, 0)); }
int pipe(int fds[2]) { return result(call(NX_SYS_PIPE2, (uintptr_t)fds, 0, 0)); }
pid_t spawn(const char *path, char *const argv[], char *const envp[]) {
    return result(call(NX_SYS_SPAWN, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)(envp ? envp : environ)));
}
pid_t waitpid(pid_t pid, int *status, int options) {
    if (options & ~WNOHANG) { errno = EINVAL; return -1; }
    return result(call(NX_SYS_WAITPID, pid, (uintptr_t)status, options & WNOHANG ? NX_WNOHANG : 0));
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }
int fcntl(int fd, int cmd, ...) {
    int arg = 0;
    if (cmd != F_GETFL && cmd != F_GETFD) { va_list ap; va_start(ap, cmd); arg = va_arg(ap, int); va_end(ap); }
    if (cmd == F_SETFL) arg = nxflags(arg);
    int r = result(call(NX_SYS_FCNTL, fd, cmd, arg));
    if (cmd == F_GETFL && r >= 0) {
        int flags = r & NX_O_ACCMODE;
        if (r & NX_O_APPEND) flags |= O_APPEND;
        if (r & NX_O_NONBLOCK) flags |= O_NONBLOCK;
        return flags;
    }
    return r;
}
static int stat_call(int nr, uintptr_t arg, struct stat *s) {
    struct nx_stat n;
    if (!s) { errno = EFAULT; return -1; }
    int r = result(call(nr, arg, (uintptr_t)&n, 0));
    if (r < 0) return r;
    memset(s, 0, sizeof(*s));
    s->st_mode = n.st_mode; s->st_size = n.st_size; s->st_ino = n.st_ino;
    s->st_dev = n.st_dev; s->st_uid = n.st_uid; s->st_nlink = n.st_nlink;
    s->st_blksize = n.st_blksize; s->st_blocks = (n.st_size + 511) / 512;
    s->st_mtim.tv_sec = n.st_mtime;
    return 0;
}
int stat(const char *p, struct stat *s) { return stat_call(NX_SYS_STAT, (uintptr_t)p, s); }
int fstat(int fd, struct stat *s) { return stat_call(NX_SYS_FSTAT, fd, s); }
int clock_gettime(clockid_t id, struct timespec *t) {
    struct nx_timespec n;
    if (!t) { errno = EFAULT; return -1; }
    if (id != CLOCK_REALTIME && id != CLOCK_MONOTONIC) { errno = EINVAL; return -1; }
    int r = result(call(NX_SYS_CLOCK_GETTIME, id == CLOCK_REALTIME ? NX_CLOCK_REALTIME : NX_CLOCK_MONOTONIC, (uintptr_t)&n, 0));
    if (r == 0) { t->tv_sec = n.tv_sec; t->tv_nsec = n.tv_nsec; }
    return r;
}
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; struct timespec t;
    if (!tv || clock_gettime(CLOCK_REALTIME, &t)) return -1;
    tv->tv_sec = t.tv_sec; tv->tv_usec = t.tv_nsec / 1000; return 0;
}
clock_t times(struct tms *t) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return (clock_t)-1;
    clock_t now = ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / (1000000000 / CLOCKS_PER_SEC);
    if (t) { memset(t, 0, sizeof(*t)); t->tms_utime = now; }
    return now;
}
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_sec < 0 || req->tv_sec > 86400 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) { errno = EINVAL; return -1; }
    struct nx_timespec n = {req->tv_sec, req->tv_nsec};
    int r = result(call(NX_SYS_NANOSLEEP, (uintptr_t)&n, 0, 0));
    if (rem) memset(rem, 0, sizeof(*rem));
    return r;
}
int usleep(useconds_t us) { struct timespec t = {us / 1000000, (us % 1000000) * 1000}; return nanosleep(&t, NULL); }
unsigned int sleep(unsigned int sec) { struct timespec t = {sec, 0}; return nanosleep(&t, NULL) ? sec : 0; }
int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    return result(call(NX_SYS_IOCTL, fd, req, (uintptr_t)arg));
}
int tcgetattr(int fd, struct termios *t) { return ioctl(fd, NX_TCGETS, t); }
int tcsetattr(int fd, int action, const struct termios *t) {
    if (action < TCSANOW || action > TCSAFLUSH) { errno = EINVAL; return -1; }
    return ioctl(fd, NX_TCSETS + action, t);
}
DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof(*d));
    if (!d) { close(fd); return NULL; }
    d->fd = fd; return d;
}
struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return NULL; }
    int r = result(call(NX_SYS_GETDENTS, d->fd, (uintptr_t)&d->entry, sizeof(d->entry)));
    return r > 0 ? &d->entry : NULL;
}
int closedir(DIR *d) { if (!d) { errno = EBADF; return -1; } int r = close(d->fd); free(d); return r; }
int system(const char *cmd) {
    if (!cmd) return access("/disk/bin/sh", X_OK) == 0;
    char *args[] = {"sh", "-c", (char *)cmd, NULL};
    pid_t pid = spawn("/disk/bin/sh", args, environ);
    int status; return pid < 0 || waitpid(pid, &status, 0) < 0 ? -1 : status;
}
