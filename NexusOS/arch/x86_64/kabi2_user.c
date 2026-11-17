/* Linux ABI conformance, part 2 (kabi, wave 2): System V IPC (semaphores with SEM_UNDO and IPC_RMID of waiters,
 * message queues, shared memory, /proc/sysvipc, the IPC sysctls, IPC namespaces), Landlock ABI v4 (filesystem, REFER,
 * TRUNCATE, TCP ports, inheritance over fork and exec, with seccomp, ptrace scoping), RLIMIT_SIGPENDING for every
 * queued signal, /proc/PID/fdinfo of eventfd/epoll/timerfd/signalfd/pidfd/inotify, the new mount API and the _ALARM
 * clocks waking a suspended machine. Run as /bin/kabi2 by the kernel for /nexus/test-kabi (tests/test_x64_kabi.py):
 * '0' as the desktop user, 'r' as root. Every failure prints "[64] KABI FAIL line=N errno=E" and exits 99. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] KABI FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define ERR(call, code) do { errno = 0; long _r = (long)(call); if (_r != -1 || errno != (code)) { dprintf(2, "[64] KABI FAIL line=%d result=%ld errno=%d want=%d\n", __LINE__, _r, errno, code); _exit(99); } } while (0)
#define CHILD_OK(pid) do { int _s; CHECK(waitpid((pid), &_s, 0) == (pid)); if (!WIFEXITED(_s) || WEXITSTATUS(_s)) { dprintf(2, "[64] KABI FAIL line=%d child status=%#x\n", __LINE__, _s); _exit(99); } } while (0)
static char* self_path;
static void sleep_ms(unsigned ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; while (nanosleep(&t, &t) && errno == EINTR) {} }
static void block(int s) { sigset_t m; sigemptyset(&m); sigaddset(&m, s); CHECK(sigprocmask(SIG_BLOCK, &m, NULL) == 0); }
static int take(int s, siginfo_t* info) { sigset_t m; sigemptyset(&m); sigaddset(&m, s); struct timespec t = {0, 0}; return sigtimedwait(&m, info, &t); }
static size_t slurp(const char* path, char* out, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    size_t n = 0; ssize_t r; while (n + 1 < capacity && (r = read(fd, out + n, capacity - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0; close(fd); return n;
}
static void fdinfo_of(int fd, char* out, size_t capacity) { char path[64]; snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd); CHECK(slurp(path, out, capacity) > 0); }
static unsigned kdev(dev_t d) { return major(d) << 20 | minor(d); }

/* ---- RLIMIT_SIGPENDING ---- */
static void sigpending_limit(void) {
    struct rlimit saved, low; CHECK(getrlimit(RLIMIT_SIGPENDING, &saved) == 0);
    low = saved; low.rlim_cur = 6; CHECK(setrlimit(RLIMIT_SIGPENDING, &low) == 0);
    int rt = SIGRTMIN + 2; block(rt); block(SIGUSR2);
    union sigval v = {.sival_int = 7}; int queued = 0;
    while (queued < 100 && sigqueue(getpid(), rt, v) == 0) queued++;
    CHECK(errno == EAGAIN && queued >= 1 && queued <= 6);
    CHECK(kill(getpid(), rt) == 0 && kill(getpid(), rt) == 0);   /* kill(): queued once, without its information */
    v.sival_int = 9; CHECK(sigqueue(getpid(), SIGUSR2, v) == 0);   /* a standard signal: delivered, information lost */
    siginfo_t info; int with = 0, without = 0;
    while (take(rt, &info) == rt) {
        if (info.si_code == SI_QUEUE && info.si_value.sival_int == 7) with++;
        else { CHECK(info.si_code == SI_USER && info.si_pid == 0); without++; }
    }
    CHECK(with == queued && without == 1);
    CHECK(take(SIGUSR2, &info) == SIGUSR2 && info.si_code == SI_USER && info.si_pid == 0);
    /* POSIX timers take from the same budget (their signal is preallocated) */
    timer_t timers[8]; int made = 0; struct sigevent ev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = rt};
    while (made < 8 && timer_create(CLOCK_MONOTONIC, &ev, &timers[made]) == 0) made++;
    CHECK(errno == EAGAIN && made == queued);
    ERR(sigqueue(getpid(), rt, v), EAGAIN);
    for (int i = 0; i < made; i++) CHECK(timer_delete(timers[i]) == 0);
    CHECK(sigqueue(getpid(), rt, v) == 0 && take(rt, &info) == rt);
    CHECK(setrlimit(RLIMIT_SIGPENDING, &saved) == 0);
    puts("[64] KABI2 PASS: RLIMIT_SIGPENDING for sigqueue, kill (lose info), standard signals, POSIX timers");
    /* PR_SET_PDEATHSIG: a grandchild hears of its parent's exit; PR_GET_PDEATHSIG; MAP_HUGETLB is ENOMEM */
    ERR(prctl(PR_SET_PDEATHSIG, 99), EINVAL);
    int report[2], go[2]; CHECK(pipe(report) == 0 && pipe(go) == 0);
    pid_t middle = fork(); CHECK(middle >= 0);
    if (!middle) {
        pid_t grand = fork(); if (grand < 0) _exit(1);
        if (!grand) {
            block(SIGUSR1); int got = 0;
            if (prctl(PR_SET_PDEATHSIG, SIGUSR1) || prctl(PR_GET_PDEATHSIG, &got) || got != SIGUSR1) _exit(2);
            if (write(go[1], "r", 1) != 1) _exit(3);   /* ready: the middle process may exit */
            sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); struct timespec t = {20, 0}; siginfo_t i;
            char result = sigtimedwait(&m, &i, &t) == SIGUSR1 ? 'y' : 'n';
            if (write(report[1], &result, 1) != 1) _exit(4);
            _exit(0);
        }
        char ready; if (read(go[0], &ready, 1) != 1) _exit(5);
        _exit(0);
    }
    CHILD_OK(middle);
    char result; CHECK(read(report[0], &result, 1) == 1 && result == 'y');
    close(report[0]); close(report[1]); close(go[0]); close(go[1]);
    int none = -1; CHECK(prctl(PR_GET_PDEATHSIG, &none) == 0 && none == 0);
    ERR(mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0), ENOMEM);
    puts("[64] KABI2 PASS: PR_SET_PDEATHSIG/PR_GET_PDEATHSIG (the parent's exit signals the child), MAP_HUGETLB ENOMEM");
}

/* ---- preadv/pwritev/preadv2/pwritev2 ---- */
static void vectored(void) {
    int fd = open("/tmp/kabi2-vec", O_RDWR | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0);
    char a[] = "hello ", b[] = "world"; struct iovec out[2] = {{a, 6}, {b, 5}};
    CHECK(syscall(296, fd, out, 2, 100, 0) == 11);                      /* pwritev at 100: the position stays 0 */
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    char x[6] = {0}, y[6] = {0}; struct iovec in[2] = {{x, 5}, {y, 5}};
    CHECK(syscall(295, fd, in, 2, 101, 0) == 10 && !memcmp(x, "ello ", 5) && !memcmp(y, "world", 5));
    CHECK(lseek(fd, 100, SEEK_SET) == 100);
    CHECK(syscall(327, fd, in, 1, -1L, 0L, 0) == 5 && !memcmp(x, "hello", 5) && lseek(fd, 0, SEEK_CUR) == 105);   /* -1: the position */
    CHECK(syscall(328, fd, out, 1, 0, 0, 2 /* RWF_DSYNC */) == 6);
    ERR(syscall(327, fd, in, 1, 0, 0, 0x80), EOPNOTSUPP);
    ERR(syscall(295, fd, in, 1, -5L, 0L), EINVAL);
    ERR(syscall(295, fd, in, 1025, 0, 0), EINVAL);
    ERR(syscall(295, 9999, in, 1, 0, 0), EBADF);
    int ro = open("/tmp/kabi2-vec", O_RDONLY); CHECK(ro >= 0); ERR(syscall(296, ro, out, 1, 0, 0), EBADF); close(ro);
    close(fd); unlink("/tmp/kabi2-vec");
    puts("[64] KABI2 PASS: preadv/pwritev/preadv2/pwritev2 (offsets, -1 = position, RWF flags, errors)");
}

/* ---- /proc/PID/fdinfo ---- */
static void fdinfo(void) {
    char text[4096], want[256]; struct stat st;
    int efd = eventfd(5, EFD_SEMAPHORE | EFD_CLOEXEC); CHECK(efd >= 0);
    fdinfo_of(efd, text, sizeof(text));
    snprintf(want, sizeof(want), "eventfd-count: %16llx\n", 5ULL); CHECK(strstr(text, want) && strstr(text, "eventfd-semaphore: 1\n") && strstr(text, "eventfd-id: "));
    int ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0);
    struct epoll_event e = {.events = EPOLLIN | EPOLLET, .data.u64 = 0x1234}; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &e) == 0);
    CHECK(fstat(efd, &st) == 0); fdinfo_of(ep, text, sizeof(text));
    snprintf(want, sizeof(want), "tfd: %8d events: %8x data: %16llx  pos:%lli ino:%lx sdev:%x\n", efd, (unsigned)(EPOLLIN | EPOLLET), 0x1234ULL, 0LL,
             (unsigned long)st.st_ino, kdev(st.st_dev));
    CHECK(strstr(text, want));
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC); CHECK(tfd >= 0);
    struct itimerspec its = {{2, 0}, {100, 0}}; CHECK(timerfd_settime(tfd, 0, &its, NULL) == 0);
    fdinfo_of(tfd, text, sizeof(text));
    CHECK(strstr(text, "clockid: 1\nticks: 0\nsettime flags: 00\nit_value: (") && strstr(text, "it_interval: (2, 0)\n"));
    int rfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC); CHECK(rfd >= 0);
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now); its.it_value.tv_sec = now.tv_sec + 1000; its.it_interval.tv_sec = 0;
    CHECK(timerfd_settime(rfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL) == 0);
    fdinfo_of(rfd, text, sizeof(text)); CHECK(strstr(text, "clockid: 0\n") && strstr(text, "settime flags: 03\n") && strstr(text, "it_interval: (0, 0)\n"));
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR1); sigaddset(&mask, SIGRTMIN + 1); sigaddset(&mask, SIGKILL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC); CHECK(sfd >= 0);
    fdinfo_of(sfd, text, sizeof(text));
    snprintf(want, sizeof(want), "sigmask:\t%016llx\n", (1ULL << (SIGUSR1 - 1)) | (1ULL << (SIGRTMIN + 1 - 1)));   /* SIGKILL never */
    CHECK(strstr(text, want));
    pid_t child = fork(); CHECK(child >= 0); if (!child) { pause(); _exit(0); }
    int pfd = syscall(SYS_pidfd_open, child, 0); CHECK(pfd >= 0);
    fdinfo_of(pfd, text, sizeof(text)); snprintf(want, sizeof(want), "Pid:\t%d\nNSpid:\t%d\n", child, child); CHECK(strstr(text, want));
    CHECK(kill(child, SIGKILL) == 0 && waitpid(child, NULL, 0) == child);
    fdinfo_of(pfd, text, sizeof(text)); CHECK(strstr(text, "Pid:\t-1\nNSpid:\t-1\n"));
    int in = inotify_init1(IN_CLOEXEC); CHECK(in >= 0);
    int wd = inotify_add_watch(in, "/tmp", IN_CREATE | IN_DELETE); CHECK(wd >= 1); CHECK(stat("/tmp", &st) == 0);
    fdinfo_of(in, text, sizeof(text));
    snprintf(want, sizeof(want), "inotify wd:%x ino:%lx sdev:%x mask:%x ignored_mask:0 ", wd, (unsigned long)st.st_ino, kdev(st.st_dev), IN_CREATE | IN_DELETE);
    CHECK(strstr(text, want) && strstr(text, "pos:\t0\n"));
    close(efd); close(ep); close(tfd); close(rfd); close(sfd); close(pfd); close(in);
    puts("[64] KABI2 PASS: /proc/PID/fdinfo of eventfd, epoll, timerfd, signalfd, pidfd, inotify in Linux's format");
}

/* ---- System V semaphores ---- */
union semun { int val; struct semid_ds* buf; unsigned short* array; struct seminfo* info; };
static int semops(int id, unsigned short num, short op, short flags) { struct sembuf b = {num, op, flags}; return semop(id, &b, 1); }
static void semaphores(bool root) {
    int id = semget(IPC_PRIVATE, 3, IPC_CREAT | 0600); CHECK(id >= 0);
    union semun arg; struct semid_ds ds; arg.buf = &ds;
    CHECK(semctl(id, 0, IPC_STAT, arg) == 0 && ds.sem_nsems == 3 && (ds.sem_perm.mode & 0777) == 0600 && ds.sem_perm.uid == getuid());
    arg.val = 5; CHECK(semctl(id, 1, SETVAL, arg) == 0 && semctl(id, 1, GETVAL) == 5);
    unsigned short values[3] = {1, 2, 3}; arg.array = values; CHECK(semctl(id, 0, SETALL, arg) == 0);
    unsigned short got[3] = {0}; arg.array = got; CHECK(semctl(id, 0, GETALL, arg) == 0 && got[0] == 1 && got[1] == 2 && got[2] == 3);
    struct sembuf two[2] = {{0, -1, 0}, {2, 4, 0}}; CHECK(semop(id, two, 2) == 0 && semctl(id, 0, GETVAL) == 0 && semctl(id, 2, GETVAL) == 7);
    CHECK(semctl(id, 0, GETPID) == getpid());
    ERR(semops(id, 0, -1, IPC_NOWAIT), EAGAIN);
    struct sembuf all[2] = {{1, -1, 0}, {0, -1, IPC_NOWAIT}}; ERR(semop(id, all, 2), EAGAIN);   /* all or nothing */
    CHECK(semctl(id, 1, GETVAL) == 2);
    struct timespec zero = {0, 20000000}; struct sembuf wait0 = {0, -1, 0}; ERR(semtimedop(id, &wait0, 1, &zero), EAGAIN);
    struct timespec bad = {0, 1000000000}; ERR(semtimedop(id, &wait0, 1, &bad), EINVAL);
    ERR(semops(id, 3, 1, 0), EFBIG);
    ERR(semops(id, 2, 32767, 0), ERANGE);
    arg.val = 40000; ERR(semctl(id, 0, SETVAL, arg), ERANGE);
    ERR(semop(id, NULL, 0), EINVAL);
    ERR(semop(id, two, 501), E2BIG);
    ERR(semops(id + 1, 0, 1, 0), EINVAL);
    /* a blocked semop is woken by another process; GETNCNT sees it */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { _exit(semops(id, 0, -2, 0) == 0 ? 0 : 1); }
    for (int i = 0; i < 200 && semctl(id, 0, GETNCNT) != 1; i++) sleep_ms(10);
    CHECK(semctl(id, 0, GETNCNT) == 1);
    CHECK(semops(id, 0, 2, 0) == 0); CHILD_OK(child); CHECK(semctl(id, 0, GETVAL) == 0 && semctl(id, 0, GETNCNT) == 0);
    /* SEM_UNDO: the child's adjustments are undone at its exit */
    child = fork(); CHECK(child >= 0);
    if (!child) { _exit(semops(id, 2, 3, SEM_UNDO) == 0 && semops(id, 1, -1, SEM_UNDO) == 0 ? 0 : 1); }
    CHILD_OK(child); CHECK(semctl(id, 2, GETVAL) == 7 && semctl(id, 1, GETVAL) == 2);
    /* a wait for zero; IPC_RMID wakes a blocked waiter with EIDRM */
    child = fork(); CHECK(child >= 0);
    if (!child) { int r = semops(id, 2, 0, 0); _exit(r == -1 && errno == EIDRM ? 0 : 1); }
    for (int i = 0; i < 200 && semctl(id, 2, GETZCNT) != 1; i++) sleep_ms(10);
    CHECK(semctl(id, 2, GETZCNT) == 1);
    CHECK(semctl(id, 0, IPC_RMID) == 0); CHILD_OK(child);
    ERR(semctl(id, 0, GETVAL), EINVAL);
    /* keys, IPC_EXCL, permissions, IPC_SET, info */
    key_t key = 0x4b414249 + (root ? 1 : 0);
    int k = semget(key, 2, IPC_CREAT | IPC_EXCL | 0640); CHECK(k >= 0);
    ERR(semget(key, 2, IPC_CREAT | IPC_EXCL | 0640), EEXIST);
    CHECK(semget(key, 0, 0) == k); ERR(semget(key, 3, 0), EINVAL); ERR(semget(key + 100, 1, 0), ENOENT);
    arg.buf = &ds; CHECK(semctl(k, 0, IPC_STAT, arg) == 0); ds.sem_perm.mode = 0400; CHECK(semctl(k, 0, IPC_SET, arg) == 0);
    if (!root) { ERR(semops(k, 0, 1, 0), EACCES); CHECK(semops(k, 0, 0, IPC_NOWAIT) == 0); }
    struct seminfo si; arg.info = &si; int top = semctl(0, 0, SEM_INFO, arg); CHECK(top >= 0 && si.semusz >= 1 && si.semvmx == 32767);
    CHECK(semctl(0, 0, IPC_INFO, arg) >= 0 && si.semmsl == 32000 && si.semopm == 500 && si.semmni == 32000);
    arg.buf = &ds; int found = 0;
    for (int i = 0; i <= top; i++) { int sid = semctl(i, 0, SEM_STAT, arg); if (sid == k) found++; }
    CHECK(found == 1);
    char table[8192]; CHECK(slurp("/proc/sysvipc/sem", table, sizeof(table)) > 0);
    char row[128]; snprintf(row, sizeof(row), "%10d %10d  %4o %10u", key, k, 0400, 2); CHECK(strstr(table, "       key      semid perms      nsems") && strstr(table, row));
    CHECK(semctl(k, 0, IPC_RMID) == 0);
    puts("[64] KABI2 PASS: System V semaphores: all-or-nothing semop, IPC_NOWAIT, semtimedop, SEM_UNDO at exit, GETNCNT/GETZCNT, IPC_RMID wakes EIDRM, keys, IPC_SET, SEM_INFO/SEM_STAT, /proc/sysvipc/sem");
}

/* ---- System V message queues ---- */
struct message { long type; char text[64]; };
static void queues(void) {
    int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600); CHECK(q >= 0);
    struct message m = {3, "three"}; CHECK(msgsnd(q, &m, 6, 0) == 0);
    m.type = 1; strcpy(m.text, "one"); CHECK(msgsnd(q, &m, 4, 0) == 0);
    m.type = 2; strcpy(m.text, "two!"); CHECK(msgsnd(q, &m, 5, 0) == 0);
    struct msqid_ds ds; CHECK(msgctl(q, IPC_STAT, &ds) == 0 && ds.msg_qnum == 3 && ds.__msg_cbytes == 15 && ds.msg_lspid == getpid());
    struct message r;
    CHECK(msgrcv(q, &r, sizeof(r.text), -2, 0) == 4 && r.type == 1 && !strcmp(r.text, "one"));   /* lowest type <= 2 */
    CHECK(msgrcv(q, &r, sizeof(r.text), 2, MSG_EXCEPT) == 6 && r.type == 3);
    ERR(msgrcv(q, &r, 2, 0, 0), E2BIG);                                                    /* it stays queued */
    CHECK(msgrcv(q, &r, 2, 0, MSG_NOERROR) == 2 && r.type == 2 && !memcmp(r.text, "tw", 2));
    ERR(msgrcv(q, &r, sizeof(r.text), 0, IPC_NOWAIT), ENOMSG);
    m.type = 0; ERR(msgsnd(q, &m, 1, 0), EINVAL);
    m.type = 9; ERR(msgsnd(q, &m, 9000, 0), EINVAL);   /* above msgmax */
    /* MSG_COPY: the n-th message, not removed */
    m.type = 7; CHECK(msgsnd(q, &m, 3, 0) == 0); m.type = 8; CHECK(msgsnd(q, &m, 3, 0) == 0);
    ERR(msgrcv(q, &r, sizeof(r.text), 1, MSG_COPY), EINVAL);
    CHECK(msgrcv(q, &r, sizeof(r.text), 1, MSG_COPY | IPC_NOWAIT) == 3 && r.type == 8);
    CHECK(msgctl(q, IPC_STAT, &ds) == 0 && ds.msg_qnum == 2);
    CHECK(msgrcv(q, &r, sizeof(r.text), 0, 0) == 3 && msgrcv(q, &r, sizeof(r.text), 0, 0) == 3);
    /* a full queue: IPC_NOWAIT EAGAIN; msg_qbytes lowered by IPC_SET */
    CHECK(msgctl(q, IPC_STAT, &ds) == 0); ds.msg_qbytes = 10; CHECK(msgctl(q, IPC_SET, &ds) == 0);
    m.type = 1; CHECK(msgsnd(q, &m, 8, 0) == 0); ERR(msgsnd(q, &m, 8, IPC_NOWAIT), EAGAIN);
    /* a blocked receiver is woken by a sender; a blocked one gets EIDRM at IPC_RMID */
    CHECK(msgrcv(q, &r, sizeof(r.text), 0, 0) == 8);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { struct message x; _exit(msgrcv(q, &x, sizeof(x.text), 5, 0) == 3 && x.type == 5 ? 0 : 1); }
    sleep_ms(100); m.type = 5; CHECK(msgsnd(q, &m, 3, 0) == 0); CHILD_OK(child);
    child = fork(); CHECK(child >= 0);
    if (!child) { struct message x; _exit(msgrcv(q, &x, sizeof(x.text), 0, 0) == -1 && errno == EIDRM ? 0 : 1); }
    sleep_ms(150); CHECK(msgctl(q, IPC_RMID, NULL) == 0); CHILD_OK(child);
    ERR(msgsnd(q, &m, 1, 0), EINVAL);
    struct msginfo info; CHECK(msgctl(0, IPC_INFO, (struct msqid_ds*)&info) >= 0 && info.msgmax == 8192 && info.msgmnb == 16384 && info.msgmni == 32000);
    char table[8192]; CHECK(slurp("/proc/sysvipc/msg", table, sizeof(table)) > 0 && strstr(table, "       key      msqid perms      cbytes       qnum lspid lrpid"));
    puts("[64] KABI2 PASS: System V message queues: types, MSG_EXCEPT, E2BIG/MSG_NOERROR, MSG_COPY, msg_qbytes, blocking send/receive, EIDRM, IPC_INFO, /proc/sysvipc/msg");
}

/* ---- System V shared memory ---- */
static void shared_memory(void) {
    int id = shmget(IPC_PRIVATE, 10000, IPC_CREAT | 0600); CHECK(id >= 0);
    char* p = shmat(id, NULL, 0); CHECK(p != (void*)-1);
    strcpy(p, "parent"); p[9999] = 'z';
    struct shmid_ds ds; CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz == 10000 && ds.shm_nattch == 1 && ds.shm_cpid == getpid());
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        struct shmid_ds c; if (shmctl(id, IPC_STAT, &c) || c.shm_nattch != 2) _exit(1);
        if (strcmp(p, "parent") || p[9999] != 'z') _exit(2);
        strcpy(p, "child"); _exit(0);
    }
    CHILD_OK(child); CHECK(!strcmp(p, "child"));
    CHECK(shmctl(id, IPC_STAT, &ds) == 0);
    if (ds.shm_nattch != 1) { dprintf(2, "[64] KABI2 nattch=%lu\n", (unsigned long)ds.shm_nattch); CHECK(0); }
    char maps[65536]; CHECK(slurp("/proc/self/maps", maps, sizeof(maps)) > 0 && strstr(maps, "/SYSV00000000 (deleted)"));
    /* fixed addresses: SHM_RND, EINVAL without it, SHM_REMAP over a mapping */
    char* fixed = mmap(NULL, 3 * 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(fixed != MAP_FAILED);
    ERR((long)shmat(id, fixed + 1, 0), EINVAL);
    ERR((long)shmat(id, fixed, 0), EINVAL);   /* occupied, no SHM_REMAP */
    char* q = shmat(id, fixed + 1, SHM_RND | SHM_REMAP); CHECK(q == fixed && !strcmp(q, "child"));
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 2);
    CHECK(shmdt(q) == 0); ERR(shmdt(q), EINVAL); ERR(shmdt(p + 1), EINVAL);
    /* read-only attach: writing faults */
    child = fork(); CHECK(child >= 0);
    if (!child) { volatile char* r = shmat(id, NULL, SHM_RDONLY); if (r == (void*)-1 || r[0] != 'c') _exit(3); r[0] = 'x'; _exit(4); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    /* IPC_RMID while attached: SHM_DEST, key IPC_PRIVATE, still usable; gone after the last detach */
    CHECK(shmctl(id, IPC_RMID, NULL) == 0);
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && (ds.shm_perm.mode & SHM_DEST) && ds.shm_perm.__key == IPC_PRIVATE && !strcmp(p, "child"));
    CHECK(shmdt(p) == 0);
    ERR(shmctl(id, IPC_STAT, &ds), EINVAL);
    ERR(shmget(IPC_PRIVATE, 0, IPC_CREAT | 0600), EINVAL);
    struct shminfo si; CHECK(shmctl(0, IPC_INFO, (struct shmid_ds*)&si) >= 0 && si.shmmni == 4096 && si.shmmin == 1 && si.shmmax == 18446744073692774399UL);
    char table[8192]; CHECK(slurp("/proc/sysvipc/shm", table, sizeof(table)) > 0 && strstr(table, "       key      shmid perms                  size  cpid  lpid nattch"));
    puts("[64] KABI2 PASS: System V shared memory: shmat/shmdt, nattch over fork, SHM_RND/SHM_REMAP, SHM_RDONLY faults, SHM_DEST, /proc/self/maps SYSV, IPC_INFO, /proc/sysvipc/shm");
}
static void sysvipc_root(void) {
    char text[256]; CHECK(slurp("/proc/sys/kernel/sem", text, sizeof(text)) > 0 && !strcmp(text, "32000\t1024000000\t500\t32000\n"));
    CHECK(slurp("/proc/sys/kernel/shmmax", text, sizeof(text)) > 0 && !strcmp(text, "18446744073692774399\n"));
    int fd = open("/proc/sys/kernel/msgmax", O_WRONLY); CHECK(fd >= 0 && write(fd, "4096\n", 5) == 5); close(fd);
    int q = msgget(IPC_PRIVATE, 0600); CHECK(q >= 0); struct message m = {1, ""}; ERR(msgsnd(q, &m, 5000, IPC_NOWAIT), EINVAL); CHECK(msgctl(q, IPC_RMID, NULL) == 0);
    fd = open("/proc/sys/kernel/msgmax", O_WRONLY); CHECK(fd >= 0 && write(fd, "8192", 4) == 4); close(fd);
    fd = open("/proc/sys/kernel/msgmni", O_WRONLY); CHECK(fd >= 0); ERR(write(fd, "40000", 5), EINVAL); close(fd);
    /* an IPC namespace sees its own objects and limits; they go with it */
    int outer = semget(0x51504349, 1, IPC_CREAT | 0600); CHECK(outer >= 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        if (unshare(CLONE_NEWIPC)) _exit(1);
        if (semget(0x51504349, 1, 0) != -1 || errno != ENOENT) _exit(2);
        char t[8192]; if (!slurp("/proc/sysvipc/sem", t, sizeof(t)) || strchr(strchr(t, '\n') + 1, '\n')) _exit(3);   /* header only */
        int inner = semget(0x51504349, 1, IPC_CREAT | 0600); if (inner < 0) _exit(4);
        int f = open("/proc/sys/kernel/msgmax", O_WRONLY); if (f < 0 || write(f, "100", 3) != 3) _exit(5); close(f);
        _exit(0);
    }
    CHILD_OK(child);
    CHECK(slurp("/proc/sys/kernel/msgmax", text, sizeof(text)) > 0 && !strcmp(text, "8192\n"));
    CHECK(semget(0x51504349, 1, 0) == outer && semctl(outer, 0, IPC_RMID) == 0);
    puts("[64] KABI2 PASS: IPC sysctls (kernel.sem, shmmax, msgmax writable, msgmni range), IPC namespaces isolate objects and limits");
}

/* ---- Landlock ---- */
struct ll_ruleset_attr { uint64_t fs, net; };
struct ll_path_attr { uint64_t access; int32_t fd; } __attribute__((packed));
struct ll_net_attr { uint64_t access, port; };
#define LL_EXECUTE 1ULL
#define LL_WRITE 2ULL
#define LL_READ 4ULL
#define LL_READ_DIR 8ULL
#define LL_REMOVE_DIR 16ULL
#define LL_REMOVE_FILE 32ULL
#define LL_MAKE_DIR 128ULL
#define LL_MAKE_REG 256ULL
#define LL_MAKE_SOCK 512ULL
#define LL_MAKE_SYM 4096ULL
#define LL_REFER 8192ULL
#define LL_TRUNCATE 16384ULL
#define LL_FS_ALL 0x7FFFULL
static long ll_create(void* attr, size_t size, unsigned flags) { return syscall(444, attr, size, flags); }
static long ll_add(int fd, int type, void* attr, unsigned flags) { return syscall(445, fd, type, attr, flags); }
static long ll_restrict(int fd, unsigned flags) { return syscall(446, fd, flags); }
static void ll_rule(int ruleset, const char* path, uint64_t access) {   /* (/proc refuses O_PATH here: a plain descriptor) */
    int fd = open(path, (strncmp(path, "/proc", 5) ? O_PATH : O_RDONLY) | O_CLOEXEC); if (fd < 0) dprintf(2, "[64] KABI2 rule path %s\n", path); CHECK(fd >= 0);
    struct ll_path_attr a = {access, fd}; CHECK(ll_add(ruleset, 1, &a, 0) == 0); close(fd);
}
static void mkfile(const char* path, const char* text) { int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text)); close(fd); }
static int tcp_bind(int port) { int s = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; int r = bind(s, (void*)&a, sizeof(a)); int e = errno; close(s); errno = e; return r; }
static int tcp_connect(int port) { int s = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; int r = connect(s, (void*)&a, sizeof(a)); int e = errno; close(s); errno = e; return r; }
static void landlock_sandboxed(int before) {   /* in a child, after restrict_self */
    char buffer[16];
    int fd = open("/tmp/ll/ok/a", O_RDWR); CHECK(fd >= 0 && read(fd, buffer, 1) == 1); close(fd);
    fd = open("/tmp/ll/ro/b", O_RDONLY); CHECK(fd >= 0); close(fd);
    ERR(open("/tmp/ll/ro/b", O_WRONLY), EACCES);
    ERR(open("/tmp/ll/ro/new", O_WRONLY | O_CREAT, 0644), EACCES);
    ERR(unlink("/tmp/ll/ro/b"), EACCES);
    ERR(mkdir("/tmp/ll/ro/d", 0755), EACCES);
    { int d = open("/tmp/ll/ro", O_RDONLY | O_DIRECTORY); CHECK(d >= 0); close(d); }   /* READ_DIR */
    ERR(open("/tmp/ll/other/c", O_RDONLY), EACCES);        /* no rule */
    ERR(truncate("/tmp/ll/ro/b", 0), EACCES);               /* TRUNCATE */
    ERR(open("/tmp/ll/ro/b", O_RDONLY | O_TRUNC), EACCES);
    CHECK(ftruncate(before, 0) == 0);                       /* opened before the domain: allowed */
    fd = open("/tmp/ll/ok/a", O_RDWR | O_TRUNC); CHECK(fd >= 0 && ftruncate(fd, 1) == 0); close(fd);
    mkfile("/tmp/ll/ok/n", "x"); CHECK(rename("/tmp/ll/ok/n", "/tmp/ll/ok/m") == 0);   /* same directory */
    ERR(rename("/tmp/ll/ok/m", "/tmp/ll/ro/m"), EACCES);   /* MAKE_REG missing in ro */
    ERR(link("/tmp/ll/ok/m", "/tmp/ll/ok2/m"), EXDEV);     /* reparenting needs REFER */
    ERR(rename("/tmp/ll/ok/m", "/tmp/ll/ok2/m"), EXDEV);
    CHECK(symlink("m", "/tmp/ll/ok/s") == 0 && unlink("/tmp/ll/ok/s") == 0);
    CHECK(tcp_bind(17771) == 0);
    ERR(tcp_bind(17772), EACCES);
    ERR(tcp_connect(17773), EACCES);
    { int s = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr un = {.sa_family = AF_UNSPEC}; connect(s, &un, sizeof(struct sockaddr_in)); CHECK(errno != EACCES); close(s); }
    int u = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un { sa_family_t f; char p[108]; } ua = {AF_UNIX, "/tmp/ll/ro/sock"};
    ERR(bind(u, (void*)&ua, sizeof(ua)), EACCES); close(u);
    char environ_path[64]; snprintf(environ_path, sizeof(environ_path), "/proc/%d/environ", getppid());
    ERR(open(environ_path, O_RDONLY), EACCES);              /* the parent is outside the domain */
}
static void landlock_tests(void) {
    CHECK(ll_create(NULL, 0, 1) == 4);                      /* ABI version */
    ERR(ll_create(NULL, 0, 2), EINVAL);
    struct ll_ruleset_attr attr = {LL_READ, 0};
    ERR(ll_create(&attr, 4, 0), EINVAL);
    ERR(ll_create(NULL, sizeof(attr), 0), EFAULT);
    char big[24] = {0}; memcpy(big, &attr, sizeof(attr)); big[20] = 1; ERR(ll_create(big, sizeof(big), 0), E2BIG);
    attr.fs = 1ULL << 20; ERR(ll_create(&attr, sizeof(attr), 0), EINVAL);
    attr.fs = 0; ERR(ll_create(&attr, sizeof(attr), 0), ENOMSG);
    attr.net = 4; ERR(ll_create(&attr, sizeof(attr), 0), EINVAL);
    attr.fs = LL_READ; attr.net = 0; int rs = ll_create(&attr, 8, 0); CHECK(rs >= 0 && (fcntl(rs, F_GETFD) & FD_CLOEXEC));
    char buffer[8]; ERR(read(rs, buffer, 8), EINVAL);
    struct ll_path_attr pa = {0, AT_FDCWD}; ERR(ll_add(rs, 1, &pa, 0), ENOMSG);
    pa.access = LL_WRITE; ERR(ll_add(rs, 1, &pa, 0), EINVAL);    /* not handled */
    pa.access = LL_READ; pa.fd = -1; ERR(ll_add(rs, 1, &pa, 0), EBADF);
    pa.fd = rs; ERR(ll_add(rs, 1, &pa, 0), EBADFD);
    int pipes[2]; CHECK(pipe(pipes) == 0); ERR(ll_add(pipes[0], 1, &pa, 0), EBADFD); close(pipes[0]); close(pipes[1]);
    ERR(ll_add(rs, 3, &pa, 0), EINVAL); ERR(ll_add(rs, 1, &pa, 1), EINVAL);
    ERR(ll_restrict(rs, 0), EPERM);                          /* neither no_new_privs nor CAP_SYS_ADMIN */
    close(rs);
    /* the sandbox: ok (all rights), ok2 (all but REFER), ro (read), bin (execute) */
    mkdir("/tmp/ll", 0755); mkdir("/tmp/ll/ok", 0755); mkdir("/tmp/ll/ok2", 0755); mkdir("/tmp/ll/ro", 0755); mkdir("/tmp/ll/other", 0755);
    mkfile("/tmp/ll/ok/a", "abc"); mkfile("/tmp/ll/ro/b", "bcd"); mkfile("/tmp/ll/other/c", "cde"); mkfile("/tmp/ll/ok/t", "t");
    unlink("/tmp/ll/ok/m"); unlink("/tmp/ll/ok2/m"); unlink("/tmp/ll/ok/n");
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        int before = open("/tmp/ll/ok/t", O_RDWR); CHECK(before >= 0);
        struct ll_ruleset_attr a = {LL_FS_ALL, 3}; int r = ll_create(&a, sizeof(a), 0); CHECK(r >= 0);
        ll_rule(r, "/tmp/ll/ok", LL_FS_ALL); ll_rule(r, "/tmp/ll/ok2", LL_FS_ALL & ~LL_REFER); ll_rule(r, "/tmp/ll/ro", LL_READ | LL_READ_DIR);
        ll_rule(r, "/bin", LL_EXECUTE | LL_READ | LL_READ_DIR); ll_rule(r, "/proc", LL_READ | LL_READ_DIR);
        const char* libraries[] = {"/lib", "/lib64", "/usr", "/etc/ld.so.cache"};   /* what an exec'd program loads */
        for (unsigned i = 0; i < 4; i++) { struct stat st; if (!stat(libraries[i], &st)) ll_rule(r, libraries[i], S_ISDIR(st.st_mode) ? LL_EXECUTE | LL_READ | LL_READ_DIR : LL_READ); }
        int file = open("/tmp/ll/ro/b", O_PATH); struct ll_path_attr bad = {LL_READ_DIR, file}; ERR(ll_add(r, 1, &bad, 0), EINVAL); close(file);
        struct ll_net_attr n = {1, 17771}; CHECK(ll_add(r, 2, &n, 0) == 0);
        n.port = 70000; ERR(ll_add(r, 2, &n, 0), EINVAL);
        CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
        /* with seccomp: an allow-all filter that fails getpriority with ENOTSUP */
        struct sock_filter f[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getpriority, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOTSUP),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
        struct sock_fprog prog = {sizeof(f) / sizeof(f[0]), f}; CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0);
        CHECK(ll_restrict(r, 0) == 0); close(r);
        ERR(getpriority(PRIO_PROCESS, 0), ENOTSUP);
        landlock_sandboxed(before);
        pid_t grand = fork(); CHECK(grand >= 0);   /* inherited by fork */
        if (!grand) { ERR(open("/tmp/ll/other/c", O_RDONLY), EACCES); _exit(0); }
        CHILD_OK(grand);
        grand = fork(); CHECK(grand >= 0);         /* and kept over exec (EXECUTE on /bin) */
        if (!grand) { execl(self_path, self_path, "L", (char*)NULL); _exit(98); }
        CHILD_OK(grand);
        mkfile("/tmp/ll/ok/x", "#!/bin/sh\n"); chmod("/tmp/ll/ok/x", 0755);
        struct ll_ruleset_attr e = {LL_EXECUTE, 0}; int r2 = ll_create(&e, sizeof(e), 0); CHECK(r2 >= 0);
        ll_rule(r2, "/bin", LL_EXECUTE); CHECK(ll_restrict(r2, 0) == 0); close(r2);   /* a second layer */
        grand = fork(); CHECK(grand >= 0);
        if (!grand) { execl("/tmp/ll/ok/x", "x", (char*)NULL); _exit(errno == EACCES ? 0 : 1); }
        CHILD_OK(grand);
        for (int i = 2; i < 16; i++) { int r3 = ll_create(&e, sizeof(e), 0); CHECK(r3 >= 0 && ll_restrict(r3, 0) == 0); close(r3); }
        int r4 = ll_create(&e, sizeof(e), 0); ERR(ll_restrict(r4, 0), E2BIG);
        _exit(0);
    }
    CHILD_OK(child);
    int fd = open("/tmp/ll/other/c", O_RDONLY); CHECK(fd >= 0); close(fd);   /* the parent is not restricted */
    puts("[64] KABI2 PASS: Landlock ABI v4: errors, PATH_BENEATH/NET_PORT rules, read/write/create/remove/TRUNCATE/REFER (EXDEV), TCP bind/connect, AF_UNIX MAKE_SOCK, inherited over fork and exec, with seccomp, ptrace scope, 16 layers");
}

/* ---- the new mount API (root) ---- */
#ifndef FSCONFIG_SET_FLAG
#define FSCONFIG_SET_FLAG 0
#endif
#ifndef FSCONFIG_SET_STRING
#define FSCONFIG_SET_STRING 1
#endif
#ifndef FSCONFIG_SET_BINARY
#define FSCONFIG_SET_BINARY 2
#endif
#ifndef FSCONFIG_CMD_CREATE
#define FSCONFIG_CMD_CREATE 6
#endif
#ifndef FSCONFIG_CMD_RECONFIGURE
#define FSCONFIG_CMD_RECONFIGURE 7
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 4
#endif
#define OPEN_TREE_CLONE_ 1
#define MOUNT_ATTR_RDONLY_ 1ULL
struct mount_attr_ { uint64_t set, clr, propagation, userns; };
static void mount_api(bool root) {
    if (!root) {
        ERR(syscall(430, "vfat", 0), EPERM); ERR(syscall(428, AT_FDCWD, "/tmp", 1), EPERM);
        struct mount_attr_ a = {MOUNT_ATTR_RDONLY_, 0, 0, 0}; ERR(syscall(442, AT_FDCWD, "/tmp", 0, &a, sizeof(a)), EPERM);
        puts("[64] KABI2 PASS: new mount API: EPERM without CAP_SYS_ADMIN");
        return;
    }
    ERR(syscall(430, NULL, 0), EFAULT); ERR(syscall(430, "nosuchfs", 0), ENODEV); ERR(syscall(430, "vfat", 2), EINVAL);
    int fs = syscall(430, "vfat", 1); CHECK(fs >= 0 && (fcntl(fs, F_GETFD) & FD_CLOEXEC));
    ERR(syscall(431, fs, FSCONFIG_SET_FLAG, "ro", "x", 0), EINVAL);
    ERR(syscall(431, fs, 99, NULL, NULL, 0), EOPNOTSUPP);
    ERR(syscall(431, fs, FSCONFIG_SET_BINARY, "k", "v", 1), EOPNOTSUPP);
    ERR(syscall(432, fs, 0, 0), EBUSY);                            /* not created yet */
    CHECK(syscall(431, fs, FSCONFIG_SET_STRING, "source", "/dev/nonexistent", 0) == 0);
    ERR(syscall(431, fs, FSCONFIG_SET_STRING, "source", "/dev/other", 0), EINVAL);
    char message[128]; CHECK(read(fs, message, sizeof(message)) > 0 && !strncmp(message, "e ", 2)); ERR(read(fs, message, sizeof(message)), ENODATA);
    CHECK(syscall(431, fs, FSCONFIG_SET_FLAG, "ro", NULL, 0) == 0 && syscall(431, fs, FSCONFIG_SET_STRING, "uid", "0", 0) == 0);
    CHECK(syscall(431, fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0) == 0);
    ERR(syscall(431, fs, FSCONFIG_SET_FLAG, "rw", NULL, 0), EBUSY);
    int mnt = syscall(432, fs, 1, 0); CHECK(mnt >= 0);
    ERR(syscall(432, fs, 0, 0), EBUSY);
    mkdir("/tmp/mapi", 0755); mkdir("/tmp/mapi/src", 0755); mkdir("/tmp/mapi/dst", 0755); mkdir("/tmp/mapi/dst2", 0755);
    ERR(syscall(429, mnt, "", AT_FDCWD, "/tmp/mapi/dst", MOVE_MOUNT_F_EMPTY_PATH), ENOENT);   /* the device: when attached here */
    close(mnt); close(fs);
    /* a bind mount through open_tree(OPEN_TREE_CLONE) + mount_setattr + move_mount */
    mkfile("/tmp/mapi/src/file", "bound");
    ERR(syscall(428, AT_FDCWD, "/tmp/mapi/src", AT_RECURSIVE), EINVAL);
    int tree = syscall(428, AT_FDCWD, "/tmp/mapi/src", OPEN_TREE_CLONE_ | O_CLOEXEC); CHECK(tree >= 0);
    struct stat a, b; CHECK(fstat(tree, &a) == 0 && stat("/tmp/mapi/src", &b) == 0 && a.st_ino == b.st_ino);
    struct mount_attr_ attr = {MOUNT_ATTR_RDONLY_, 0, 0, 0};
    ERR(syscall(442, tree, "", AT_EMPTY_PATH, &attr, 8), EINVAL);
    struct mount_attr_ idmap = {0x100000, 0, 0, 0}; ERR(syscall(442, tree, "", AT_EMPTY_PATH, &idmap, sizeof(idmap)), EINVAL);
    CHECK(syscall(442, tree, "", AT_EMPTY_PATH, &attr, sizeof(attr)) == 0);
    CHECK(syscall(429, tree, "", AT_FDCWD, "/tmp/mapi/dst", MOVE_MOUNT_F_EMPTY_PATH) == 0);
    char text[64]; CHECK(slurp("/tmp/mapi/dst/file", text, sizeof(text)) == 5 && !strcmp(text, "bound"));
    ERR(open("/tmp/mapi/dst/new", O_WRONLY | O_CREAT, 0644), EROFS);
    char info[16384]; CHECK(slurp("/proc/self/mountinfo", info, sizeof(info)) > 0 && strstr(info, " /tmp/mapi/dst "));
    attr.set = 0; attr.clr = MOUNT_ATTR_RDONLY_;
    CHECK(syscall(442, AT_FDCWD, "/tmp/mapi/dst", 0, &attr, sizeof(attr)) == 0);
    mkfile("/tmp/mapi/dst/new", "rw");
    /* fspick + reconfigure: read-only again */
    int pick = syscall(433, AT_FDCWD, "/tmp/mapi/dst", 1); CHECK(pick >= 0);
    ERR(syscall(431, pick, FSCONFIG_CMD_CREATE, NULL, NULL, 0), EBUSY);
    CHECK(syscall(431, pick, FSCONFIG_SET_FLAG, "ro", NULL, 0) == 0 && syscall(431, pick, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0) == 0);
    ERR(open("/tmp/mapi/dst/new2", O_WRONLY | O_CREAT, 0644), EROFS); close(pick);
    ERR(syscall(433, AT_FDCWD, "/tmp/mapi/src", 0), EINVAL);   /* not a mount root */
    /* open_tree without CLONE: move the attached mount */
    int ref = syscall(428, AT_FDCWD, "/tmp/mapi/dst", O_CLOEXEC); CHECK(ref >= 0);
    CHECK(syscall(429, ref, "", AT_FDCWD, "/tmp/mapi/dst2", MOVE_MOUNT_F_EMPTY_PATH) == 0); close(ref);
    CHECK(slurp("/tmp/mapi/dst2/file", text, sizeof(text)) == 5);
    CHECK(umount2("/tmp/mapi/dst2", 0) == 0);
    struct mount_attr_ nop = {0, 0, 0, 0}; CHECK(syscall(442, AT_FDCWD, "/", 0, &nop, sizeof(nop)) == 0);
    struct mount_attr_ prop = {0, 0, MS_SHARED, 0}; CHECK(syscall(442, AT_FDCWD, "/", 0, &prop, sizeof(prop)) == 0);
    prop.propagation = 5; ERR(syscall(442, AT_FDCWD, "/", 0, &prop, sizeof(prop)), EINVAL);
    puts("[64] KABI2 PASS: new mount API: fsopen/fsconfig/fsmount errors and phases, open_tree clone + mount_setattr + move_mount, fspick reconfigure, moves, mount_setattr");
}

/* ---- _ALARM clocks wake a suspended machine (root) ---- */
static void alarm_wake(void) {
    /* S3 ("mem"): QEMU wakes a suspended machine on the RTC alarm (as test_x64_suspend's rtc case); suspend-to-idle
     * only ends on an SCI, which QEMU's RTC alarm does not raise */
    char states[128]; if (!slurp("/sys/power/state", states, sizeof(states)) || !strstr(states, "mem")) { puts("[64] KABI2 SKIP: no S3"); return; }
    int fd = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_CLOEXEC); CHECK(fd >= 0);
    struct itimerspec its = {{0, 0}, {1, 0}}; CHECK(timerfd_settime(fd, 0, &its, NULL) == 0);
    int power = open("/sys/power/state", O_WRONLY); CHECK(power >= 0);
    ERR(write(power, "mem", 3), EBUSY);   /* due within 2 s: the suspend is refused */
    uint64_t ticks; CHECK(read(fd, &ticks, 8) == 8);
    its.it_value.tv_sec = 4; CHECK(timerfd_settime(fd, 0, &its, NULL) == 0);
    struct timespec t0, t1; clock_gettime(CLOCK_BOOTTIME, &t0);
    CHECK(write(power, "mem", 3) == 3);   /* the RTC alarm wakes it */
    clock_gettime(CLOCK_BOOTTIME, &t1); close(power);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms >= 2000 && ms < 60000);
    CHECK(read(fd, &ticks, 8) == 8 && ticks >= 1); close(fd);
    printf("[64] KABI2 PASS: CLOCK_BOOTTIME_ALARM: EBUSY within 2 s, the RTC alarm woke S3 after %ld ms\n", ms);
}

int main(int argc, char** argv) {
    self_path = argv[0]; setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && argv[1][0] == 'L') {   /* exec'd inside the Landlock domain */
        ERR(open("/tmp/ll/other/c", O_RDONLY), EACCES);
        int fd = open("/tmp/ll/ro/b", O_RDONLY); CHECK(fd >= 0); close(fd);
        return 0;
    }
    bool root = argc == 2 && argv[1][0] == 'r';
    alarm(300);
    sigpending_limit(); vectored(); fdinfo(); semaphores(root); queues(); shared_memory(); mount_api(root);
    if (root) { sysvipc_root(); alarm_wake(); }
    else landlock_tests();
    alarm(0);
    printf("[64] KABI2 ALL PASSED (%s)\n", root ? "root" : "user");
    return 0;
}
