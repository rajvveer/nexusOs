/* Stress for the lifetime of open-file descriptions (kernel.c: fd_release, descriptor_new, descriptors_prune,
 * file_views_moved), run as /bin/power64 by the kernel's power campaign (tests/test_x64_fdrace.py).
 *
 * /nexus/test-power holds "f <seconds> [<directory>]". Workers, per CPU, run until the time is up:
 *   closer   (a thread, and a forked process) open / mmap(MAP_SHARED) / dirtying pwrite / munmap / close of the test
 *            files: page-cache views (memory->views), mappings, and an inotify close event per close
 *   renamer  (a thread) renames the files back and forth while others hold them open or mapped (file_views_moved)
 *   churn    (a forked process) eventfd, epoll, socketpair + SCM_RIGHTS round trips and closes: descriptions created
 *            and pruned by I/O-free system calls (they run beside a sleeping disk wait), then anonymous pages filled
 *            with 0xA5 and released, so freed kernel pages come back holding user data
 * An inotify watch on the directory makes every close look its parent up (inotify_event). Afterwards every file is
 * checked (size, content) and the inotify descriptor read. The kernel option test.fd-release-sleep makes each file
 * close sleep in that window, which turns the race into a deterministic one.
 * Prints "[64] FDRACE DONE failures=<n> ..."; exit status = failures. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FILES 8
#define FILE_BYTES 16384
static char dir[256] = "/fdrace";
static atomic_int failures;
static atomic_long ops_close, ops_rename, ops_churn, ops_map;
static volatile bool stop;
static uint64_t deadline_ns;

static void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("[64] FDRACE "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); }
static void fail(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("[64] FDRACE FAIL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); failures++;
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec; }
static bool done(void) { return stop || now_ns() >= deadline_ns; }
static void name_of(char* out, size_t size, char prefix, int i) { snprintf(out, size, "%s/%c%d", dir, prefix, i); }
static int open_either(int i, int flags) {   /* the file is fN or (while renamed) gN */
    char a[300], b[300]; name_of(a, sizeof(a), 'f', i); name_of(b, sizeof(b), 'g', i);
    for (int tries = 0; tries < 4; tries++) {
        int fd = open(a, flags); if (fd >= 0 || errno != ENOENT) return fd;
        fd = open(b, flags); if (fd >= 0 || errno != ENOENT) return fd;
    }
    errno = ENOENT; return -1;
}

static void closer(unsigned seed) {
    while (!done()) {
        int i = (int)(seed = seed * 1103515245u + 12345u) % FILES; if (i < 0) i = -i;
        int fd = open_either(i, O_RDWR);
        if (fd < 0) { if (errno != ENOENT) fail("open file %d: %s", i, strerror(errno)); continue; }
        char pattern = (char)('A' + i), byte = 0;
        if ((seed >> 8) & 1) {
            char* map = mmap(NULL, FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) fail("mmap file %d: %s", i, strerror(errno));
            else {
                if (map[(seed >> 4) % FILE_BYTES] != pattern) fail("file %d mapped content %#x", i, map[(seed >> 4) % FILE_BYTES] & 0xFF);
                map[(seed >> 12) % FILE_BYTES] = pattern;   /* a dirty shared page, same content */
                munmap(map, FILE_BYTES); ops_map++;
            }
        }
        if (pwrite(fd, &pattern, 1, (seed >> 16) % FILE_BYTES) != 1) fail("pwrite file %d: %s", i, strerror(errno));
        if (pread(fd, &byte, 1, (seed >> 20) % FILE_BYTES) != 1 || byte != pattern) fail("file %d content %#x", i, byte & 0xFF);
        if (close(fd)) fail("close file %d: %s", i, strerror(errno));
        ops_close++;
    }
}
static void* closer_thread(void* arg) { closer((unsigned)(uintptr_t)arg); return NULL; }
static void* renamer_thread(void* arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;
    while (!done()) {
        int i = (int)((seed = seed * 1103515245u + 12345u) >> 4) % FILES;
        char a[300], b[300]; name_of(a, sizeof(a), 'f', i); name_of(b, sizeof(b), 'g', i);
        if (rename(a, b) == 0) { if (rename(b, a)) fail("rename back %s: %s", b, strerror(errno)); ops_rename++; }
        else if (errno != ENOENT) fail("rename %s: %s", a, strerror(errno));
        struct timespec t = {0, 3000000}; nanosleep(&t, NULL);   /* a few hundred a second: the closes and the churn dominate */
    }
    return NULL;
}
static void churn(void) {
    while (!done()) {
        int e = eventfd(0, 0), ep = epoll_create1(0), sv[2];
        if (e < 0 || ep < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { fail("churn descriptors: %s", strerror(errno)); return; }
        char data = 'x', control[CMSG_SPACE(sizeof(int))]; memset(control, 0, sizeof(control));
        struct iovec io = {&data, 1}; struct msghdr m = {0}; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = control; m.msg_controllen = sizeof(control);
        struct cmsghdr* c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &e, sizeof(int));
        if (sendmsg(sv[0], &m, 0) != 1) fail("sendmsg SCM_RIGHTS: %s", strerror(errno));
        close(e);   /* the description lives on only in the message */
        memset(control, 0, sizeof(control)); m.msg_controllen = sizeof(control);
        if (recvmsg(sv[1], &m, 0) != 1) fail("recvmsg SCM_RIGHTS: %s", strerror(errno));
        else { int got = -1; c = CMSG_FIRSTHDR(&m); if (c && c->cmsg_type == SCM_RIGHTS) memcpy(&got, CMSG_DATA(c), sizeof(int)); if (got >= 0) close(got); else fail("no descriptor received"); }
        close(ep); close(sv[0]); close(sv[1]);
        unsigned char* p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { memset(p, 0xA5, 65536); munmap(p, 65536); }
        ops_churn++;
    }
}
static pid_t spawn(void (*body)(unsigned), unsigned seed) {
    fflush(stdout); pid_t pid = fork();
    if (pid == 0) {
        body(seed);
        say("worker %d closes=%ld maps=%ld churn=%ld failures=%d", (int)getpid(), (long)ops_close, (long)ops_map, (long)ops_churn, (int)failures);
        _exit(failures ? 1 : 0);
    }
    if (pid < 0) fail("fork: %s", strerror(errno));
    return pid;
}
static void churn_body(unsigned seed) { (void)seed; churn(); }

int main(int argc, char** argv) {
    char config[512] = {0}; unsigned seconds = 20;
    int cfd = open("/nexus/test-power", O_RDONLY);
    if (cfd >= 0) { ssize_t n = read(cfd, config, sizeof(config) - 1); if (n > 0) config[n] = 0; close(cfd); }
    if (config[0] == 'f') sscanf(config + 1, "%u %255s", &seconds, dir);
    else if (argc > 1) seconds = (unsigned)atoi(argv[1]);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN); if (cpus < 1) cpus = 1; if (cpus > 8) cpus = 8;
    say("START seconds=%u cpus=%ld dir=%s", seconds, cpus, dir);
    mkdir(dir, 0777);
    for (int i = 0; i < FILES; i++) {   /* fresh files (also after an earlier run renamed some) */
        char a[300], b[300]; name_of(a, sizeof(a), 'f', i); name_of(b, sizeof(b), 'g', i); unlink(b);
        int fd = open(a, O_RDWR | O_CREAT | O_TRUNC, 0666); if (fd < 0) { fail("create %s: %s", a, strerror(errno)); return 1; }
        char block[FILE_BYTES]; memset(block, 'A' + i, sizeof(block));
        if (write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) fail("fill %s", a);
        fsync(fd); close(fd);
    }
    int in = inotify_init1(IN_NONBLOCK);
    if (in < 0 || inotify_add_watch(in, dir, IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_MOVED_FROM | IN_MOVED_TO) < 0) fail("inotify on %s: %s", dir, strerror(errno));
    deadline_ns = now_ns() + (uint64_t)seconds * 1000000000ULL;
    pid_t children[32]; unsigned nchildren = 0; pthread_t threads[32]; unsigned nthreads = 0;
    for (long c = 0; c < cpus; c++) {
        children[nchildren++] = spawn(churn_body, 0);
        children[nchildren++] = spawn(closer, 7919u * (unsigned)c + 3);
        pthread_create(&threads[nthreads++], NULL, closer_thread, (void*)(uintptr_t)(104729u * (unsigned)c + 11));
        pthread_create(&threads[nthreads++], NULL, renamer_thread, (void*)(uintptr_t)(1299709u * (unsigned)c + 5));
    }
    long events = 0; char buffer[4096];
    while (!done()) {   /* drain the watch so its queue keeps taking events */
        ssize_t n = read(in, buffer, sizeof(buffer)); if (n > 0) events += n / (ssize_t)sizeof(struct inotify_event);
        struct timespec t = {0, 20000000}; nanosleep(&t, NULL);
    }
    for (unsigned i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);
    for (unsigned i = 0; i < nchildren; i++) {
        int status = 0;
        if (children[i] > 0 && waitpid(children[i], &status, 0) == children[i] && (!WIFEXITED(status) || WEXITSTATUS(status))) fail("worker %d ended with status %#x", children[i], status);
    }
    for (int i = 0; i < FILES; i++) {
        int fd = open_either(i, O_RDONLY); char block[FILE_BYTES]; struct stat st;
        if (fd < 0 || fstat(fd, &st) || st.st_size != FILE_BYTES || read(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) { fail("file %d after the run", i); if (fd >= 0) close(fd); continue; }
        for (int k = 0; k < FILE_BYTES; k++) if (block[k] != 'A' + i) { fail("file %d byte %d = %#x after the run", i, k, block[k] & 0xFF); break; }
        close(fd);
    }
    ssize_t n; while ((n = read(in, buffer, sizeof(buffer))) > 0) events += n / (ssize_t)sizeof(struct inotify_event);
    if (!events) fail("no inotify events");
    close(in);
    say("DONE failures=%d closes=%ld maps=%ld renames=%ld churn=%ld inotify_events=%ld", (int)failures, (long)ops_close, (long)ops_map,
        (long)ops_rename, (long)ops_churn, events);
    return failures ? 1 : 0;
}
