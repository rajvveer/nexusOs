/* Linux ABI conformance (kabi, wave 2): POSIX timers, timerfd, itimers, CPU clocks, signalfd, sigtimedwait,
 * sigqueueinfo, pidfd_open/send_signal/getfd, waitid(P_PIDFD), clone3 (CLONE_PIDFD, exit_signal, stack,
 * CLONE_CLEAR_SIGHAND), quotactl, getcpu and the rseq fallback - including hostile arguments.
 * Run as /bin/kabi64 by the kernel for /nexus/test-kabi (tests/test_x64_kabi.py), on 1 and on several CPUs.
 * Every failure prints "[64] KABI FAIL line=N errno=E" and exits 99. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#ifndef PIDFD_NONBLOCK
#define PIDFD_NONBLOCK O_NONBLOCK
#endif
#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x1000
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef SYS_quotactl_fd
#define SYS_quotactl_fd 443
#endif
#ifndef TFD_IOC_SET_TICKS
#define TFD_IOC_SET_TICKS _IOW('T', 0, uint64_t)
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif
#ifndef SYS_rseq
#define SYS_rseq 334
#endif
#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] KABI FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define ERR(call, code) do { errno = 0; long _r = (long)(call); if (_r != -1 || errno != (code)) { dprintf(2, "[64] KABI FAIL line=%d result=%ld errno=%d want=%d\n", __LINE__, _r, errno, code); _exit(99); } } while (0)
#define NS 1000000000LL
static char* self_path;
static uint64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * NS + t.tv_nsec; }
static uint64_t clock_ns(clockid_t id) { struct timespec t; CHECK(clock_gettime(id, &t) == 0); return (uint64_t)t.tv_sec * NS + t.tv_nsec; }
static void spin_cpu(uint64_t ns) { uint64_t start = clock_ns(CLOCK_THREAD_CPUTIME_ID); volatile uint64_t x = 1; while (clock_ns(CLOCK_THREAD_CPUTIME_ID) - start < ns) for (int i = 0; i < 1000; i++) x = x * 3 + 1; }
static void sleep_ms(unsigned ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; while (nanosleep(&t, &t) && errno == EINTR) {} }
static sigset_t only(int a, int b) { sigset_t s; sigemptyset(&s); if (a) sigaddset(&s, a); if (b) sigaddset(&s, b); return s; }
static void block(int a, int b) { sigset_t s = only(a, b); CHECK(sigprocmask(SIG_BLOCK, &s, NULL) == 0); }
static void unblock(int a, int b) { sigset_t s = only(a, b); CHECK(sigprocmask(SIG_UNBLOCK, &s, NULL) == 0); }
static int waitsig(int a, int b, siginfo_t* info, long ms) {
    sigset_t s = only(a, b); struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; return sigtimedwait(&s, info, &t);
}
static long sys_timer_create(clockid_t c, struct sigevent* e, int* id) { return syscall(SYS_timer_create, c, e, id); }
static long sys_timer_settime(int id, int flags, const struct itimerspec* n, struct itimerspec* o) { return syscall(SYS_timer_settime, id, flags, n, o); }
static long sys_timer_gettime(int id, struct itimerspec* v) { return syscall(SYS_timer_gettime, id, v); }
static long sys_timer_delete(int id) { return syscall(SYS_timer_delete, id); }
static struct itimerspec spec(long value_ns, long interval_ns) {
    struct itimerspec s = {{interval_ns / NS, interval_ns % NS}, {value_ns / NS, value_ns % NS}}; return s;
}
static long ts_ns(struct timespec t) { return t.tv_sec * NS + t.tv_nsec; }

/* ---------------- POSIX timers ---------------- */
static volatile pid_t worker_tid; static volatile int worker_go, worker_got, worker_code, worker_value;
static void* thread_receiver(void* unused) {
    (void)unused; worker_tid = gettid();
    siginfo_t info; int n = waitsig(SIGRTMIN + 2, 0, &info, 5000);
    worker_got = n; worker_code = info.si_code; worker_value = info.si_value.sival_int; return NULL;
}
static volatile int thread_timer_hits; static volatile pid_t thread_timer_tid;
static void thread_notify(union sigval value) { thread_timer_hits += value.sival_int; thread_timer_tid = gettid(); }
static volatile uint64_t cpu_worker_spent;
static void* cpu_worker(void* unused) { (void)unused; uint64_t start = clock_ns(CLOCK_THREAD_CPUTIME_ID); while (!worker_go) spin_cpu(1000000); cpu_worker_spent = clock_ns(CLOCK_THREAD_CPUTIME_ID) - start; return NULL; }
static void posix_timers(void) {
    int id; struct sigevent ev; memset(&ev, 0, sizeof(ev)); struct itimerspec v, old;
    /* hostile arguments */
    ERR(sys_timer_create(1234, NULL, &id), EINVAL);
    ERR(sys_timer_create(CLOCK_MONOTONIC_RAW, NULL, &id), EOPNOTSUPP);
    ERR(sys_timer_create(CLOCK_REALTIME_COARSE, NULL, &id), EOPNOTSUPP);
    ERR(sys_timer_create(CLOCK_MONOTONIC, (void*)8, &id), EFAULT);
    ERR(sys_timer_create(CLOCK_MONOTONIC, NULL, (void*)8), EFAULT);
    ev.sigev_notify = 77; ERR(sys_timer_create(CLOCK_MONOTONIC, &ev, &id), EINVAL);
    ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = 0; ERR(sys_timer_create(CLOCK_MONOTONIC, &ev, &id), EINVAL);
    ev.sigev_signo = 65; ERR(sys_timer_create(CLOCK_MONOTONIC, &ev, &id), EINVAL);
    pid_t stranger = fork(); CHECK(stranger >= 0); if (!stranger) { pause(); _exit(0); }   /* a thread of another process */
    ev.sigev_notify = SIGEV_THREAD_ID; ev.sigev_signo = SIGRTMIN; ev._sigev_un._tid = stranger; ERR(sys_timer_create(CLOCK_MONOTONIC, &ev, &id), EINVAL);
    kill(stranger, SIGKILL); waitpid(stranger, NULL, 0);
    ev._sigev_un._tid = -5; ERR(sys_timer_create(CLOCK_MONOTONIC, &ev, &id), EINVAL);
    ERR(sys_timer_settime(123456, 0, &v, NULL), EINVAL); ERR(sys_timer_gettime(-1, &v), EINVAL);
    ERR(syscall(SYS_timer_getoverrun, 99999), EINVAL); ERR(sys_timer_delete(99999), EINVAL);
    if (geteuid()) ERR(sys_timer_create(CLOCK_REALTIME_ALARM, NULL, &id), EPERM);
    /* default sigevent: SIGALRM with sival = id; one-shot monotonic, si_code SI_TIMER, si_timerid, si_value */
    block(SIGALRM, SIGRTMIN);
    CHECK(sys_timer_create(CLOCK_MONOTONIC, NULL, &id) == 0 && id >= 0);
    CHECK(sys_timer_gettime(id, &v) == 0 && !v.it_value.tv_sec && !v.it_value.tv_nsec && !v.it_interval.tv_nsec);
    v = spec(20000000, 0); v.it_value.tv_nsec = -1; ERR(sys_timer_settime(id, 0, &v, NULL), EINVAL);
    v.it_value.tv_nsec = NS; ERR(sys_timer_settime(id, 0, &v, NULL), EINVAL);
    ERR(sys_timer_settime(id, 0, NULL, NULL), EINVAL); ERR(sys_timer_settime(id, 0, (void*)8, NULL), EFAULT);
    v = spec(30000000, 0); uint64_t armed = mono(); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0);
    CHECK(sys_timer_gettime(id, &v) == 0 && ts_ns(v.it_value) > 0 && ts_ns(v.it_value) <= 30000000);
    siginfo_t info; CHECK(waitsig(SIGALRM, 0, &info, 3000) == SIGALRM);
    CHECK(mono() - armed >= 29000000ULL);
    CHECK(info.si_code == SI_TIMER && info.si_timerid == id && info.si_value.sival_int == id && info.si_overrun == 0);
    CHECK(sys_timer_gettime(id, &v) == 0 && ts_ns(v.it_value) == 0);   /* expired one-shot reads as disarmed */
    CHECK(sys_timer_delete(id) == 0); ERR(sys_timer_delete(id), EINVAL);
    /* periodic, signal kept blocked: one signal, expirations counted as overruns; timer_getoverrun matches */
    ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = SIGRTMIN; ev.sigev_value.sival_ptr = (void*)0x1234567890ULL;
    CHECK(sys_timer_create(CLOCK_MONOTONIC, &ev, &id) == 0);
    v = spec(2000000, 2000000); CHECK(sys_timer_settime(id, 0, &v, &old) == 0 && ts_ns(old.it_value) == 0);
    sleep_ms(120);
    CHECK(waitsig(SIGRTMIN, 0, &info, 1000) == SIGRTMIN);
    CHECK(info.si_code == SI_TIMER && info.si_timerid == id && info.si_value.sival_ptr == (void*)0x1234567890ULL);
    CHECK(info.si_overrun >= 20 && info.si_overrun <= 80);
    CHECK(syscall(SYS_timer_getoverrun, id) == info.si_overrun);
    CHECK(sys_timer_gettime(id, &v) == 0 && ts_ns(v.it_interval) == 2000000 && ts_ns(v.it_value) > 0 && ts_ns(v.it_value) <= 2000000);
    /* re-arming takes the queued signal back (no stale signal) */
    sleep_ms(10); v = spec(5 * NS, 0); CHECK(sys_timer_settime(id, 0, &v, &old) == 0 && ts_ns(old.it_interval) == 2000000);
    CHECK(waitsig(SIGRTMIN, 0, &info, 0) == -1 && errno == EAGAIN);
    /* deleting a timer with a pending signal drops that signal */
    v = spec(1000000, 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0); sleep_ms(30);
    sigset_t pending; CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGRTMIN));
    CHECK(sys_timer_delete(id) == 0);
    CHECK(sigpending(&pending) == 0 && !sigismember(&pending, SIGRTMIN));
    /* absolute times: CLOCK_REALTIME in the past fires at once, CLOCK_MONOTONIC absolute in the future */
    CHECK(sys_timer_create(CLOCK_REALTIME, &ev, &id) == 0);
    v = spec(0, 0); v.it_value.tv_sec = 1; CHECK(sys_timer_settime(id, TIMER_ABSTIME, &v, NULL) == 0);
    CHECK(waitsig(SIGRTMIN, 0, &info, 1000) == SIGRTMIN && info.si_timerid == id);
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now); v = spec(0, 0); v.it_value = now; v.it_value.tv_nsec += 40000000;
    if (v.it_value.tv_nsec >= NS) { v.it_value.tv_sec++; v.it_value.tv_nsec -= NS; }
    CHECK(sys_timer_settime(id, TIMER_ABSTIME, &v, NULL) == 0);
    CHECK(sys_timer_gettime(id, &v) == 0 && ts_ns(v.it_value) > 0 && ts_ns(v.it_value) <= 40000000);
    CHECK(waitsig(SIGRTMIN, 0, &info, 2000) == SIGRTMIN);
    CHECK(sys_timer_delete(id) == 0);
    CHECK(sys_timer_create(CLOCK_BOOTTIME, &ev, &id) == 0);
    clock_gettime(CLOCK_BOOTTIME, &now); v = spec(0, 0); v.it_value = now; v.it_value.tv_sec += 1;
    CHECK(sys_timer_settime(id, TIMER_ABSTIME, &v, NULL) == 0 && sys_timer_gettime(id, &v) == 0 && v.it_value.tv_sec == 0 && v.it_value.tv_nsec > 500000000);
    /* huge values saturate instead of wrapping */
    v = spec(0, 0); v.it_value.tv_sec = 0x7FFFFFFFFFFFFFFFLL; v.it_interval.tv_sec = 0x7FFFFFFFFFFFFFFFLL;
    CHECK(sys_timer_settime(id, 0, &v, NULL) == 0 && sys_timer_gettime(id, &v) == 0 && v.it_value.tv_sec > 1000000000LL);
    CHECK(sys_timer_delete(id) == 0);
    /* SIGEV_NONE: never signals, gettime counts down, expired one-shot reads 0 */
    ev.sigev_notify = SIGEV_NONE; CHECK(sys_timer_create(CLOCK_MONOTONIC, &ev, &id) == 0);
    v = spec(20000000, 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0); sleep_ms(40);
    CHECK(sys_timer_gettime(id, &v) == 0 && ts_ns(v.it_value) == 0); CHECK(sigpending(&pending) == 0 && !sigismember(&pending, SIGRTMIN));
    CHECK(sys_timer_delete(id) == 0);
    /* a 1 ns interval does not storm: one queued signal, the rest overruns */
    ev.sigev_notify = SIGEV_SIGNAL; CHECK(sys_timer_create(CLOCK_MONOTONIC, &ev, &id) == 0);
    v = spec(1, 1); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0); sleep_ms(20);
    CHECK(waitsig(SIGRTMIN, 0, &info, 1000) == SIGRTMIN && info.si_overrun > 1000);
    CHECK(sys_timer_delete(id) == 0); waitsig(SIGRTMIN, 0, &info, 0);
    /* SIGEV_THREAD_ID: the named thread receives it */
    pthread_t thread; block(SIGRTMIN + 2, 0); worker_tid = 0;
    CHECK(pthread_create(&thread, NULL, thread_receiver, NULL) == 0);
    while (!worker_tid) sleep_ms(1);
    ev.sigev_notify = SIGEV_THREAD_ID; ev.sigev_signo = SIGRTMIN + 2; ev._sigev_un._tid = worker_tid; ev.sigev_value.sival_int = 4242;
    CHECK(sys_timer_create(CLOCK_MONOTONIC, &ev, &id) == 0);
    v = spec(10000000, 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && worker_got == SIGRTMIN + 2 && worker_code == SI_TIMER && worker_value == 4242);
    CHECK(sigpending(&pending) == 0 && !sigismember(&pending, SIGRTMIN + 2));
    v = spec(5000000, 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0); sleep_ms(30);   /* the thread is gone: nothing sent */
    CHECK(sigpending(&pending) == 0 && !sigismember(&pending, SIGRTMIN + 2));
    CHECK(sys_timer_delete(id) == 0);
    /* glibc SIGEV_THREAD timers (a helper thread, SIGEV_THREAD_ID underneath) */
    timer_t glibc_timer; struct sigevent te; memset(&te, 0, sizeof(te));
    te.sigev_notify = SIGEV_THREAD; te.sigev_notify_function = thread_notify; te.sigev_value.sival_int = 1;
    CHECK(timer_create(CLOCK_MONOTONIC, &te, &glibc_timer) == 0);
    v = spec(5000000, 5000000); CHECK(timer_settime(glibc_timer, 0, &v, NULL) == 0);
    for (unsigned i = 0; i < 400 && thread_timer_hits < 3; i++) sleep_ms(5);
    CHECK(thread_timer_hits >= 3 && thread_timer_tid != gettid()); CHECK(timer_delete(glibc_timer) == 0);
    /* CPU-time clocks: process and thread timers fire after that much CPU time */
    ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = SIGRTMIN;
    CHECK(sys_timer_create(CLOCK_PROCESS_CPUTIME_ID, &ev, &id) == 0);
    uint64_t cpu0 = clock_ns(CLOCK_PROCESS_CPUTIME_ID); v = spec(30000000, 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0);
    CHECK(sys_timer_gettime(id, &v) == 0 && ts_ns(v.it_value) > 0 && ts_ns(v.it_value) <= 30000000);
    for (unsigned i = 0; i < 4000; i++) { spin_cpu(1000000); if (waitsig(SIGRTMIN, 0, &info, 0) == SIGRTMIN) break; CHECK(i < 3999); }
    CHECK(clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu0 >= 29000000ULL && info.si_timerid == id);
    CHECK(sys_timer_delete(id) == 0);
    worker_go = 0; CHECK(pthread_create(&thread, NULL, cpu_worker, NULL) == 0);
    clockid_t worker_clock; CHECK(pthread_getcpuclockid(thread, &worker_clock) == 0);
    CHECK(sys_timer_create(worker_clock, &ev, &id) == 0);
    v = spec(20000000, 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0);
    CHECK(waitsig(SIGRTMIN, 0, &info, 10000) == SIGRTMIN && info.si_timerid == id);
    CHECK(clock_ns(worker_clock) >= 19000000ULL);   /* another thread's CPU clock reads through clock_gettime */
    worker_go = 1; CHECK(pthread_join(thread, NULL) == 0 && cpu_worker_spent >= 19000000ULL);
    CHECK(sys_timer_delete(id) == 0);
    clockid_t mine; CHECK(clock_getcpuclockid(getpid(), &mine) == 0 && clock_ns(mine) > 0);
    ERR(clock_gettime(-0x7FFFFFF8, &now), EINVAL);   /* CPU clock of a pid that does not exist */
    /* per-process limit (RLIMIT_SIGPENDING) and creation churn */
    struct rlimit saved, low; CHECK(getrlimit(RLIMIT_SIGPENDING, &saved) == 0); low.rlim_cur = 6; low.rlim_max = saved.rlim_max;
    CHECK(setrlimit(RLIMIT_SIGPENDING, &low) == 0);   /* the soft limit only: raising the hard one back needs CAP_SYS_RESOURCE */
    int ids[8]; unsigned made = 0;
    for (; made < 8; made++) if (sys_timer_create(CLOCK_MONOTONIC, NULL, &ids[made])) break;
    CHECK(made == 6 && errno == EAGAIN);
    for (unsigned i = 0; i < made; i++) CHECK(sys_timer_delete(ids[i]) == 0);
    CHECK(setrlimit(RLIMIT_SIGPENDING, &saved) == 0);
    for (unsigned round = 0; round < 3000; round++) {
        CHECK(sys_timer_create(round & 1 ? CLOCK_REALTIME : CLOCK_MONOTONIC, NULL, &id) == 0);
        v = spec(1000000000, round & 2 ? 1000 : 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0); CHECK(sys_timer_delete(id) == 0);
    }
    /* fork: the child has no timers; exec: they are gone */
    ev.sigev_notify = SIGEV_NONE; CHECK(sys_timer_create(CLOCK_MONOTONIC, &ev, &id) == 0);
    v = spec(NS, 0); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { if (sys_timer_gettime(id, &v) == -1 && errno == EINVAL) _exit(0); _exit(1); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(sys_timer_gettime(id, &v) == 0 && v.it_value.tv_nsec + v.it_value.tv_sec > 0);
    child = fork(); CHECK(child >= 0);
    if (!child) { char number[16]; snprintf(number, sizeof(number), "%d", id); execl(self_path, self_path, "e", number, (char*)NULL); _exit(2); }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(sys_timer_delete(id) == 0);
    unblock(SIGALRM, SIGRTMIN); unblock(SIGRTMIN + 2, 0);
    puts("[64] KABI POSIX TIMERS PASS: create/settime/gettime/getoverrun/delete, SIGEV_*, clocks, CPU timers, limits, fork/exec");
}
/* setitimer ITIMER_VIRTUAL / ITIMER_PROF */
static volatile sig_atomic_t vtalrm, prof;
static void itimer_handler(int number) { if (number == SIGVTALRM) vtalrm++; else prof++; }
static void itimers(void) {
    signal(SIGVTALRM, itimer_handler); signal(SIGPROF, itimer_handler);
    struct itimerval v = {{0, 10000}, {0, 10000}}, o;
    ERR(setitimer(3, &v, NULL), EINVAL);
    v.it_value.tv_usec = 1000000; ERR(setitimer(ITIMER_VIRTUAL, &v, NULL), EINVAL); v.it_value.tv_usec = 10000;
    CHECK(setitimer(ITIMER_VIRTUAL, &v, NULL) == 0 && setitimer(ITIMER_PROF, &v, NULL) == 0);
    CHECK(getitimer(ITIMER_VIRTUAL, &o) == 0 && o.it_interval.tv_usec == 10000 && (o.it_value.tv_usec > 0 || o.it_value.tv_sec));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { struct itimerval c; if (getitimer(ITIMER_PROF, &c) || c.it_value.tv_usec || c.it_value.tv_sec) _exit(1); _exit(0); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    for (unsigned i = 0; i < 3000 && (vtalrm < 3 || prof < 3); i++) spin_cpu(1000000);
    CHECK(vtalrm >= 3 && prof >= 3);
    struct itimerval zero = {{0, 0}, {0, 0}};
    CHECK(setitimer(ITIMER_VIRTUAL, &zero, &o) == 0 && o.it_interval.tv_usec == 10000);
    CHECK(syscall(SYS_setitimer, ITIMER_PROF, NULL, NULL) == 0);   /* NULL disarms (Linux) */
    CHECK(getitimer(ITIMER_PROF, &o) == 0 && !o.it_value.tv_sec && !o.it_value.tv_usec && !o.it_interval.tv_usec);
    signal(SIGVTALRM, SIG_DFL); signal(SIGPROF, SIG_DFL);
    puts("[64] KABI ITIMER PASS: ITIMER_VIRTUAL/PROF on CPU time, getitimer, fork, NULL disarm");
}
/* ---------------- timerfd ---------------- */
static void timerfds(void) {
    ERR(timerfd_create(CLOCK_MONOTONIC_RAW, 0), EINVAL); ERR(timerfd_create(CLOCK_PROCESS_CPUTIME_ID, 0), EINVAL);
    ERR(timerfd_create(CLOCK_MONOTONIC, 0x4000), EINVAL);
    if (geteuid()) ERR(timerfd_create(CLOCK_BOOTTIME_ALARM, 0), EPERM);
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK); CHECK(fd >= 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK((fcntl(fd, F_GETFL) & (O_NONBLOCK | O_ACCMODE)) == (O_NONBLOCK | O_RDWR));
    char link[64] = {0}, proc[64]; snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    CHECK(readlink(proc, link, sizeof(link) - 1) > 0 && !strcmp(link, "anon_inode:[timerfd]"));
    uint64_t ticks; struct itimerspec v, old;
    ERR(read(fd, &ticks, 8), EAGAIN); ERR(read(fd, &ticks, 4), EINVAL); ERR(write(fd, &ticks, 8), EINVAL);
    CHECK(timerfd_gettime(fd, &v) == 0 && !ts_ns(v.it_value) && !ts_ns(v.it_interval));
    v = spec(5000000, 5000000); ERR(timerfd_settime(fd, 8, &v, NULL), EINVAL); ERR(timerfd_settime(fd, 0, (void*)8, NULL), EFAULT);
    v.it_value.tv_nsec = NS; ERR(timerfd_settime(fd, 0, &v, NULL), EINVAL);
    v = spec(5000000, 5000000); ERR(timerfd_settime(0, 0, &v, NULL), EINVAL); ERR(timerfd_settime(999, 0, &v, NULL), EBADF); ERR(timerfd_gettime(fd, (void*)8), EFAULT);
    /* periodic: ticks accumulate */
    v = spec(5000000, 5000000); CHECK(timerfd_settime(fd, 0, &v, &old) == 0 && !ts_ns(old.it_value));
    sleep_ms(60); CHECK(read(fd, &ticks, 8) == 8 && ticks >= 8 && ticks <= 30);
    CHECK(timerfd_gettime(fd, &v) == 0 && ts_ns(v.it_interval) == 5000000 && ts_ns(v.it_value) > 0 && ts_ns(v.it_value) <= 5000000);
    /* epoll and poll readiness, and the blocking read */
    int ep = epoll_create1(0); struct epoll_event e = {.events = EPOLLIN, .data.u64 = 77};
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e) == 0);
    CHECK(epoll_wait(ep, &e, 1, 2000) == 1 && e.data.u64 == 77 && (e.events & EPOLLIN));
    CHECK(read(fd, &ticks, 8) == 8 && ticks >= 1);
    v = spec(0, 0); CHECK(timerfd_settime(fd, 0, &v, &old) == 0 && ts_ns(old.it_interval) == 5000000);
    CHECK(epoll_wait(ep, &e, 1, 30) == 0);
    v = spec(20000000, 0); CHECK(timerfd_settime(fd, 0, &v, NULL) == 0);
    struct pollfd p = {fd, POLLIN, 0}; uint64_t start = mono();
    CHECK(poll(&p, 1, 3000) == 1 && (p.revents & POLLIN) && mono() - start >= 19000000ULL);
    CHECK(read(fd, &ticks, 8) == 8 && ticks == 1); ERR(read(fd, &ticks, 8), EAGAIN);
    int blocking = timerfd_create(CLOCK_BOOTTIME, 0); CHECK(blocking >= 0);
    v = spec(15000000, 0); start = mono(); CHECK(timerfd_settime(blocking, 0, &v, NULL) == 0);
    CHECK(read(blocking, &ticks, 8) == 8 && ticks == 1 && mono() - start >= 14000000ULL);
    /* absolute monotonic; settime resets the tick count */
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); v = spec(0, 0); v.it_value = now;
    CHECK(timerfd_settime(blocking, TFD_TIMER_ABSTIME, &v, NULL) == 0);   /* in the past: due at once */
    CHECK(read(blocking, &ticks, 8) == 8 && ticks == 1);
    v = spec(1000000, 1000000); CHECK(timerfd_settime(blocking, 0, &v, NULL) == 0); sleep_ms(20);
    v = spec(NS, 0); CHECK(timerfd_settime(blocking, 0, &v, NULL) == 0);
    ERR(read(fd, &ticks, 8), EAGAIN);
    /* TFD_IOC_SET_TICKS */
    uint64_t five = 5, zero = 0; CHECK(ioctl(fd, TFD_IOC_SET_TICKS, &five) == 0 && read(fd, &ticks, 8) == 8 && ticks == 5);
    ERR(ioctl(fd, TFD_IOC_SET_TICKS, &zero), EINVAL); ERR(ioctl(fd, TFD_IOC_SET_TICKS, (void*)8), EFAULT);
    /* 1 ns period: one huge tick count, no storm */
    v = spec(1, 1); CHECK(timerfd_settime(fd, 0, &v, NULL) == 0); sleep_ms(20);
    CHECK(read(fd, &ticks, 8) == 8 && ticks > 1000000ULL);
    v = spec(0, 0); CHECK(timerfd_settime(fd, 0, &v, NULL) == 0); read(fd, &ticks, 8);
    /* shared across fork: the child reads the parent's timer after the parent closed its descriptor */
    v = spec(30000000, 0); CHECK(timerfd_settime(blocking, 0, &v, NULL) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { sleep_ms(10); uint64_t t; if (read(blocking, &t, 8) != 8 || t != 1) _exit(1); _exit(0); }
    close(blocking); int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(ep); close(fd);
    puts("[64] KABI TIMERFD PASS: create/settime/gettime, read ticks, epoll/poll, ABSTIME, SET_TICKS, fork, errors");
}
/* Run as root (mode 'r'): CLOCK_REALTIME steps, the _ALARM clocks, privileged clone3 set_tid. */
static void root_checks(void) {
    CHECK(geteuid() == 0);
    struct timespec now; struct itimerspec v; uint64_t ticks;
    /* TFD_TIMER_CANCEL_ON_SET: a CLOCK_REALTIME step cancels the read; the timer itself stays armed */
    int rt = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK); CHECK(rt >= 0);
    clock_gettime(CLOCK_REALTIME, &now); v = spec(0, 0); v.it_value = now; v.it_value.tv_sec += 3600;
    CHECK(timerfd_settime(rt, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &v, NULL) == 0);
    ERR(read(rt, &ticks, 8), EAGAIN);
    clock_gettime(CLOCK_REALTIME, &now); CHECK(clock_settime(CLOCK_REALTIME, &now) == 0);
    struct pollfd q = {rt, POLLIN, 0}; CHECK(poll(&q, 1, 2000) == 1 && (q.revents & POLLIN));
    ERR(read(rt, &ticks, 8), ECANCELED); ERR(read(rt, &ticks, 8), EAGAIN);
    CHECK(timerfd_gettime(rt, &v) == 0 && v.it_value.tv_sec > 3500);
    /* a disarmed CANCEL_ON_SET timerfd is cancelled too (Linux might_cancel), until settime without the flag */
    v = spec(0, 0); CHECK(timerfd_settime(rt, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &v, NULL) == 0);
    clock_gettime(CLOCK_REALTIME, &now); CHECK(clock_settime(CLOCK_REALTIME, &now) == 0);
    CHECK(poll(&q, 1, 2000) == 1); ERR(read(rt, &ticks, 8), ECANCELED);
    CHECK(timerfd_settime(rt, 0, &v, NULL) == 0);
    clock_gettime(CLOCK_REALTIME, &now); CHECK(clock_settime(CLOCK_REALTIME, &now) == 0);
    CHECK(poll(&q, 1, 100) == 0); ERR(read(rt, &ticks, 8), EAGAIN);
    /* a clock step forward past absolute realtime timers makes them due at once (timerfd and POSIX timer) */
    int step = timerfd_create(CLOCK_REALTIME, 0); clock_gettime(CLOCK_REALTIME, &now);
    v = spec(0, 0); v.it_value = now; v.it_value.tv_sec += 3; CHECK(timerfd_settime(step, TFD_TIMER_ABSTIME, &v, NULL) == 0);
    struct sigevent ev; memset(&ev, 0, sizeof(ev)); ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = SIGRTMIN; int id;
    block(SIGRTMIN, 0); CHECK(sys_timer_create(CLOCK_REALTIME, &ev, &id) == 0 && sys_timer_settime(id, TIMER_ABSTIME, &v, NULL) == 0);
    int relative; CHECK(sys_timer_create(CLOCK_REALTIME, &ev, &relative) == 0);
    struct itimerspec later = spec(4 * NS, 0); CHECK(sys_timer_settime(relative, 0, &later, NULL) == 0);   /* relative: not moved */
    struct timespec ahead = now; ahead.tv_sec += 6; uint64_t start = mono(); CHECK(clock_settime(CLOCK_REALTIME, &ahead) == 0);
    CHECK(read(step, &ticks, 8) == 8 && ticks == 1 && mono() - start < 1500000000ULL);
    siginfo_t info; CHECK(waitsig(SIGRTMIN, 0, &info, 1500) == SIGRTMIN && info.si_timerid == id);
    CHECK(sys_timer_gettime(relative, &v) == 0 && v.it_value.tv_sec >= 2);
    clock_gettime(CLOCK_REALTIME, &now); now.tv_sec -= 6; CHECK(clock_settime(CLOCK_REALTIME, &now) == 0);
    CHECK(sys_timer_delete(id) == 0 && sys_timer_delete(relative) == 0); close(step); close(rt); unblock(SIGRTMIN, 0);
    /* the _ALARM clocks need CAP_WAKE_ALARM: root has it */
    int alarm_fd = timerfd_create(CLOCK_BOOTTIME_ALARM, 0); CHECK(alarm_fd >= 0); close(alarm_fd);
    CHECK(sys_timer_create(CLOCK_REALTIME_ALARM, NULL, &id) == 0 && sys_timer_delete(id) == 0);
    /* clone3 set_tid: privileged, but pid numbers here only grow (EINVAL) */
    struct clone_args_v2 { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup; } a;
    pid_t wanted = 5000; memset(&a, 0, sizeof(a)); a.exit_signal = SIGCHLD; a.set_tid = (uint64_t)&wanted; a.set_tid_size = 1;
    ERR(syscall(SYS_clone3, &a, sizeof(a)), EINVAL);
    puts("[64] KABI ROOT PASS: CANCEL_ON_SET/ECANCELED, clock steps re-evaluate absolute realtime timers, _ALARM clocks, set_tid");
}
/* ---------------- signalfd, sigtimedwait, sigqueueinfo ---------------- */
static volatile sig_atomic_t usr2_handled;
static void usr2_handler(int number) { (void)number; usr2_handled++; }
static int signal_fd; static volatile int reader_got;
static void* signalfd_reader(void* unused) {
    (void)unused; struct signalfd_siginfo s; reader_got = read(signal_fd, &s, sizeof(s)) == sizeof(s) ? (int)s.ssi_signo : -errno; return NULL;
}
static void signalfds(void) {
    sigset_t mask = only(SIGUSR1, SIGRTMIN + 3); sigaddset(&mask, SIGCHLD); sigaddset(&mask, SIGKILL);
    ERR(syscall(SYS_signalfd4, -1, &mask, 4, 0), EINVAL); ERR(signalfd(-1, &mask, 0x4000), EINVAL);
    ERR(signalfd(-1, (void*)8, 0), EFAULT); ERR(signalfd(999, &mask, 0), EBADF); ERR(signalfd(0, &mask, 0), EINVAL);
    block(SIGUSR1, SIGRTMIN + 3); block(SIGCHLD, 0);
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC); CHECK(fd >= 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    char link[64] = {0}, proc[64]; snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    CHECK(readlink(proc, link, sizeof(link) - 1) > 0 && !strcmp(link, "anon_inode:[signalfd]"));
    struct signalfd_siginfo s[6];
    ERR(read(fd, s, sizeof(s[0])), EAGAIN); ERR(read(fd, s, 100), EINVAL);
    /* raise = tgkill: SI_TKILL with our pid/uid */
    CHECK(raise(SIGUSR1) == 0);
    CHECK(read(fd, s, sizeof(s)) == sizeof(s[0]) && s[0].ssi_signo == SIGUSR1 && s[0].ssi_code == SI_TKILL && (pid_t)s[0].ssi_pid == getpid() && s[0].ssi_uid == getuid());
    /* several in one read, sigqueue values */
    union sigval value = {.sival_int = 31};
    CHECK(kill(getpid(), SIGUSR1) == 0 && sigqueue(getpid(), SIGRTMIN + 3, value) == 0); value.sival_int = 32; CHECK(sigqueue(getpid(), SIGRTMIN + 3, value) == 0);
    CHECK(read(fd, s, sizeof(s)) == 3 * (ssize_t)sizeof(s[0]));
    CHECK(s[0].ssi_signo == SIGUSR1 && s[0].ssi_code == SI_USER && s[1].ssi_signo == (uint32_t)SIGRTMIN + 3 && s[1].ssi_code == SI_QUEUE && s[1].ssi_int == 31 && s[2].ssi_int == 32);
    /* epoll + poll readiness follow the pending set */
    int ep = epoll_create1(0); struct epoll_event e = {.events = EPOLLIN, .data.u32 = 5}; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e) == 0);
    CHECK(epoll_wait(ep, &e, 1, 0) == 0);
    CHECK(kill(getpid(), SIGUSR1) == 0 && epoll_wait(ep, &e, 1, 1000) == 1 && e.data.u32 == 5);
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    CHECK(read(fd, s, sizeof(s)) == sizeof(s[0]) && epoll_wait(ep, &e, 1, 0) == 0 && poll(&p, 1, 0) == 0);
    /* SIGCHLD records */
    pid_t child = fork(); CHECK(child >= 0); if (!child) _exit(7);
    CHECK(epoll_wait(ep, &e, 1, 5000) == 1);
    CHECK(read(fd, s, sizeof(s)) == sizeof(s[0]) && s[0].ssi_signo == SIGCHLD && s[0].ssi_code == CLD_EXITED && (pid_t)s[0].ssi_pid == child && s[0].ssi_status == 7);
    int status; CHECK(waitpid(child, &status, 0) == child);
    /* timer signals: ssi_tid (timer id), ssi_overrun, ssi_int */
    struct sigevent ev; memset(&ev, 0, sizeof(ev)); ev.sigev_notify = SIGEV_SIGNAL; ev.sigev_signo = SIGRTMIN + 3; ev.sigev_value.sival_int = 909;
    int id; CHECK(sys_timer_create(CLOCK_MONOTONIC, &ev, &id) == 0);
    struct itimerspec v = spec(2000000, 2000000); CHECK(sys_timer_settime(id, 0, &v, NULL) == 0); sleep_ms(40);
    CHECK(read(fd, s, sizeof(s)) == sizeof(s[0]) && s[0].ssi_code == SI_TIMER && (int)s[0].ssi_tid == id && s[0].ssi_int == 909 && s[0].ssi_overrun >= 5);
    CHECK(sys_timer_delete(id) == 0); read(fd, s, sizeof(s));
    /* mask update; SIGKILL/SIGSTOP silently left out */
    sigset_t other = only(SIGUSR2, SIGKILL); CHECK(signalfd(fd, &other, 0) == fd);
    CHECK(kill(getpid(), SIGUSR1) == 0); ERR(read(fd, s, sizeof(s)), EAGAIN);
    CHECK(signalfd(fd, &mask, 0) == fd && read(fd, s, sizeof(s)) == sizeof(s[0]) && s[0].ssi_signo == SIGUSR1);
    /* a blocking read in a thread, woken by a signal another thread sends */
    signal_fd = signalfd(-1, &mask, 0); CHECK(signal_fd >= 0);
    pthread_t reader; reader_got = 0; CHECK(pthread_create(&reader, NULL, signalfd_reader, NULL) == 0);
    sleep_ms(30); CHECK(kill(getpid(), SIGRTMIN + 3) == 0); CHECK(pthread_join(reader, NULL) == 0 && reader_got == SIGRTMIN + 3);
    close(signal_fd);
    /* rt_sigtimedwait */
    siginfo_t info; struct timespec t = {0, 0};
    ERR(syscall(SYS_rt_sigtimedwait, &mask, &info, &t, 4), EINVAL);
    t.tv_nsec = NS; ERR(sigtimedwait(&mask, &info, &t), EINVAL); t.tv_nsec = 0;
    ERR(sigtimedwait(&mask, &info, &t), EAGAIN);
    t.tv_nsec = 30000000; uint64_t start = mono(); ERR(sigtimedwait(&mask, &info, &t), EAGAIN); CHECK(mono() - start >= 29000000ULL);
    CHECK(kill(getpid(), SIGUSR1) == 0 && sigtimedwait(&mask, &info, NULL) == SIGUSR1 && info.si_code == SI_USER && info.si_pid == getpid());
    struct sigaction action; memset(&action, 0, sizeof(action)); action.sa_handler = usr2_handler; sigaction(SIGUSR2, &action, NULL);
    child = fork(); CHECK(child >= 0); if (!child) { sleep_ms(30); kill(getppid(), SIGUSR2); _exit(0); }
    sigset_t usr1 = only(SIGUSR1, 0);   /* not SIGCHLD: the child exits right after its kill */
    t.tv_sec = 5; t.tv_nsec = 0; ERR(sigtimedwait(&usr1, &info, &t), EINTR); CHECK(usr2_handled == 1);
    CHECK(waitpid(child, &status, 0) == child); while (read(fd, s, sizeof(s)) > 0) {}
    /* rt_sigqueueinfo / rt_tgsigqueueinfo: forged origins only for oneself */
    siginfo_t forged; memset(&forged, 0, sizeof(forged)); forged.si_code = SI_QUEUE; forged.si_value.sival_int = 55; forged.si_pid = 4321;
    CHECK(syscall(SYS_rt_sigqueueinfo, getpid(), SIGRTMIN + 3, &forged) == 0);
    CHECK(sigtimedwait(&mask, &info, NULL) == SIGRTMIN + 3 && info.si_code == SI_QUEUE && info.si_value.sival_int == 55 && info.si_pid == 4321);
    forged.si_code = SI_USER; CHECK(syscall(SYS_rt_tgsigqueueinfo, getpid(), gettid(), SIGUSR1, &forged) == 0);
    CHECK(sigtimedwait(&mask, &info, NULL) == SIGUSR1 && info.si_code == SI_USER);
    ERR(syscall(SYS_rt_sigqueueinfo, getpid(), SIGUSR1, (void*)8), EFAULT); ERR(syscall(SYS_rt_tgsigqueueinfo, 0, 0, SIGUSR1, &forged), EINVAL);
    ERR(syscall(SYS_rt_sigqueueinfo, getpid(), 99, &forged), EINVAL);
    child = fork(); CHECK(child >= 0); if (!child) { pause(); _exit(0); }
    forged.si_code = SI_USER; ERR(syscall(SYS_rt_sigqueueinfo, child, SIGUSR1, &forged), EPERM);
    forged.si_code = SI_TKILL; ERR(syscall(SYS_rt_sigqueueinfo, child, SIGUSR1, &forged), EPERM);
    forged.si_code = SI_QUEUE; CHECK(syscall(SYS_rt_sigqueueinfo, child, SIGTERM, &forged) == 0);
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    while (read(fd, s, sizeof(s)) > 0) {}
    close(ep); close(fd); unblock(SIGUSR1, SIGRTMIN + 3); unblock(SIGCHLD, 0); signal(SIGUSR2, SIG_DFL);
    puts("[64] KABI SIGNALFD PASS: signalfd(4) records/readiness/masks, SIGCHLD, timers, sigtimedwait, sig(tg)queueinfo, errors");
}
/* ---------------- pidfd and clone3 ---------------- */
struct clone_args_v2 { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup; };
static long clone3_raw(void* args, size_t size) { return syscall(SYS_clone3, args, size); }
/* clone3 with a new stack: the child runs fn(arg) there and exits with its result */
static long clone3_stack(struct clone_args_v2* args, int (*fn)(void*), void* arg) {
    long result;
    __asm__ volatile("mov %[fn], %%r12\n\tmov %[arg], %%r13\n\tsyscall\n\ttest %%rax, %%rax\n\tjnz 1f\n\t"
                     "xor %%ebp, %%ebp\n\tand $-16, %%rsp\n\tmov %%r13, %%rdi\n\tcall *%%r12\n\tmov %%eax, %%edi\n\tmov $60, %%eax\n\tsyscall\n\tud2\n1:\n"
                     : "=a"(result) : "a"(SYS_clone3), "D"(args), "S"(sizeof(*args)), [fn] "r"(fn), [arg] "r"(arg)
                     : "rcx", "r11", "r12", "r13", "memory");
    return result;
}
static volatile int stack_child_ran;
static int on_new_stack(void* arg) { int local; stack_child_ran = (char*)&local > (char*)arg - 65536 && (char*)&local < (char*)arg ? 1 : 2; return 5; }
static volatile pid_t idle_tid;
static void* idle_thread(void* unused) { (void)unused; idle_tid = gettid(); sleep_ms(400); return NULL; }
static volatile sig_atomic_t usr1_seen;
static void usr1_handler(int number) { (void)number; usr1_seen++; }
static void pidfds(void) {
    ERR(syscall(SYS_pidfd_open, 0, 0), EINVAL); ERR(syscall(SYS_pidfd_open, -3, 0), EINVAL);
    ERR(syscall(SYS_pidfd_open, getpid(), 4), EINVAL); ERR(syscall(SYS_pidfd_open, 0x7FFFFFF0, 0), ESRCH);
    pthread_t idle; idle_tid = 0; CHECK(pthread_create(&idle, NULL, idle_thread, NULL) == 0);
    while (!idle_tid) sleep_ms(1);
    ERR(syscall(SYS_pidfd_open, idle_tid, 0), EINVAL);   /* a non-leader thread is not a process */
    CHECK(pthread_join(idle, NULL) == 0);
    char path[64];
    int self = syscall(SYS_pidfd_open, getpid(), 0); CHECK(self >= 0 && fcntl(self, F_GETFD) == FD_CLOEXEC);
    char link[64] = {0}; snprintf(path, sizeof(path), "/proc/self/fd/%d", self);
    CHECK(readlink(path, link, sizeof(link) - 1) > 0 && !strcmp(link, "anon_inode:[pidfd]"));
    char byte; ERR(read(self, &byte, 1), EINVAL);
    struct pollfd p = {self, POLLIN, 0}; CHECK(poll(&p, 1, 0) == 0);
    /* pidfd_send_signal to ourselves, with and without a siginfo */
    block(SIGUSR1, 0);
    CHECK(syscall(SYS_pidfd_send_signal, self, SIGUSR1, NULL, 0) == 0);
    siginfo_t info; CHECK(waitsig(SIGUSR1, 0, &info, 1000) == SIGUSR1 && info.si_code == SI_USER && info.si_pid == getpid());
    siginfo_t forged; memset(&forged, 0, sizeof(forged)); forged.si_signo = SIGUSR1; forged.si_code = SI_QUEUE; forged.si_value.sival_int = 66;
    CHECK(syscall(SYS_pidfd_send_signal, self, SIGUSR1, &forged, 0) == 0);
    CHECK(waitsig(SIGUSR1, 0, &info, 1000) == SIGUSR1 && info.si_code == SI_QUEUE && info.si_value.sival_int == 66);
    forged.si_signo = SIGUSR2; ERR(syscall(SYS_pidfd_send_signal, self, SIGUSR1, &forged, 0), EINVAL);
    ERR(syscall(SYS_pidfd_send_signal, self, SIGUSR1, NULL, 8), EINVAL); ERR(syscall(SYS_pidfd_send_signal, self, SIGUSR1, NULL, 3), EINVAL);
    ERR(syscall(SYS_pidfd_send_signal, 0, SIGUSR1, NULL, 0), EBADF); ERR(syscall(SYS_pidfd_send_signal, 999, SIGUSR1, NULL, 0), EBADF);
    ERR(syscall(SYS_pidfd_send_signal, self, 65, NULL, 0), EINVAL); ERR(syscall(SYS_pidfd_send_signal, self, SIGUSR1, (void*)8, 0), EFAULT);
    CHECK(syscall(SYS_pidfd_send_signal, self, 0, NULL, 0) == 0);
    /* a child: poll readable once it exited, POLLHUP once reaped; waitid(P_PIDFD); a non-blocking pidfd gives EAGAIN */
    int pipe_fds[2], status; CHECK(pipe(pipe_fds) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { close(pipe_fds[0]); write(pipe_fds[1], "x", 1); pause(); _exit(0); }
    close(pipe_fds[1]); CHECK(read(pipe_fds[0], &byte, 1) == 1);
    int pidfd = syscall(SYS_pidfd_open, child, PIDFD_NONBLOCK); CHECK(pidfd >= 0 && (fcntl(pidfd, F_GETFL) & O_NONBLOCK));
    p.fd = pidfd; CHECK(poll(&p, 1, 0) == 0);
    ERR(waitid(P_PIDFD, pidfd, &info, WEXITED), EAGAIN);
    CHECK(waitid(P_PIDFD, pidfd, &info, WEXITED | WNOHANG) == 0 && info.si_pid == 0);
    ERR(waitid(P_PIDFD, self, &info, WEXITED | WNOHANG), ECHILD); ERR(waitid(P_PIDFD, 0, &info, WEXITED), EBADF);
    /* pidfd_getfd: the child's pipe end becomes ours */
    int theirs = syscall(SYS_pidfd_getfd, pidfd, pipe_fds[1], 0);   /* the child's write end of our pipe */
    CHECK(theirs >= 0 && write(theirs, "y", 1) == 1 && read(pipe_fds[0], &byte, 1) == 1 && byte == 'y');
    ERR(syscall(SYS_pidfd_getfd, pidfd, 900, 0), EBADF); ERR(syscall(SYS_pidfd_getfd, pidfd, 1, 1), EINVAL);
    ERR(syscall(SYS_pidfd_getfd, 0, 1, 0), EBADF);
    int stolen = syscall(SYS_pidfd_getfd, pidfd, 1, 0); CHECK(stolen >= 0 && fcntl(stolen, F_GETFD) == FD_CLOEXEC); close(stolen);
    if (theirs >= 0) close(theirs);
    int ep = epoll_create1(0); struct epoll_event e = {.events = EPOLLIN, .data.u32 = 9}; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pidfd, &e) == 0);
    CHECK(epoll_wait(ep, &e, 1, 0) == 0);
    forged.si_signo = SIGTERM; forged.si_code = SI_USER; ERR(syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, &forged, 0), EPERM);
    CHECK(syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0) == 0);
    CHECK(epoll_wait(ep, &e, 1, 5000) == 1 && e.data.u32 == 9 && (e.events & EPOLLIN));
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN) && !(p.revents & POLLHUP));
    CHECK(syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0) == 0);   /* a zombie still takes (and drops) signals */
    memset(&info, 0, sizeof(info)); CHECK(waitid(P_PIDFD, pidfd, &info, WEXITED) == 0);
    CHECK(info.si_pid == child && info.si_code == CLD_KILLED && info.si_status == SIGTERM);
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLHUP));
    ERR(syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0), ESRCH); ERR(syscall(SYS_pidfd_getfd, pidfd, 0, 0), ESRCH);
    ERR(waitid(P_PIDFD, pidfd, &info, WEXITED), ECHILD);
    close(ep); close(pidfd); close(pipe_fds[0]);
    /* legacy clone(CLONE_PIDFD) stores the pidfd at parent_tid; with CLONE_PARENT_SETTID: EINVAL */
    int cloned_fd = -1;
    long r = syscall(SYS_clone, CLONE_PIDFD | SIGCHLD, 0, &cloned_fd, 0, 0); CHECK(r >= 0);
    if (!r) _exit(9);
    CHECK(cloned_fd >= 0); memset(&info, 0, sizeof(info)); CHECK(waitid(P_PIDFD, cloned_fd, &info, WEXITED) == 0 && info.si_pid == r && info.si_status == 9);
    close(cloned_fd);
    ERR(syscall(SYS_clone, CLONE_PIDFD | CLONE_PARENT_SETTID | SIGCHLD, 0, &cloned_fd, 0, 0), EINVAL);
    ERR(syscall(SYS_clone, CLONE_PIDFD | CLONE_THREAD | CLONE_SIGHAND | CLONE_VM, 0, &cloned_fd, 0, 0), EINVAL);
    /* clone3: argument validation */
    struct clone_args_v2 a; uint8_t big[200];
    memset(&a, 0, sizeof(a)); a.exit_signal = SIGCHLD;
    ERR(clone3_raw(&a, 56), EINVAL); ERR(clone3_raw(&a, 5000), E2BIG); ERR(clone3_raw((void*)8, 64), EFAULT);
    memset(big, 0, sizeof(big)); big[150] = 1; memcpy(big, &a, sizeof(a)); ERR(clone3_raw(big, 200), E2BIG);
    a.exit_signal = 65; ERR(clone3_raw(&a, sizeof(a)), EINVAL); a.exit_signal = 0x100 | SIGCHLD; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.exit_signal = SIGCHLD; a.flags = CLONE_THREAD | CLONE_SIGHAND | CLONE_VM; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.flags = 0; a.stack = (uint64_t)big; ERR(clone3_raw(&a, sizeof(a)), EINVAL); a.stack = 0; a.stack_size = 4096; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.stack_size = 0; a.flags = CLONE_SIGHAND | CLONE_VM | 0x100000000ULL; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.flags = 1ULL << 40; ERR(clone3_raw(&a, sizeof(a)), EINVAL); a.flags = 0x40; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.flags = CLONE_DETACHED; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.flags = 0; pid_t wanted = 5000; a.set_tid = (uint64_t)&wanted; a.set_tid_size = 0; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.set_tid = 0; a.set_tid_size = 1; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    a.set_tid = (uint64_t)&wanted; a.set_tid_size = 40; ERR(clone3_raw(&a, sizeof(a)), EINVAL);
    if (geteuid()) { a.set_tid_size = 1; ERR(clone3_raw(&a, sizeof(a)), EPERM); }
    a.set_tid = 0; a.set_tid_size = 0; a.flags = 0x200000000ULL; a.cgroup = 0; ERR(clone3_raw(&a, 64), EINVAL); ERR(clone3_raw(&a, sizeof(a)), EBADF);
    /* clone3 fork-like with CLONE_PIDFD; a zero-padded bigger struct is accepted */
    memset(&a, 0, sizeof(a)); int fd3 = -1; a.flags = CLONE_PIDFD; a.pidfd = (uint64_t)&fd3; a.exit_signal = SIGCHLD;
    memset(big, 0, sizeof(big)); memcpy(big, &a, sizeof(a));
    r = clone3_raw(big, 200); CHECK(r >= 0); if (!r) _exit(3);
    CHECK(fd3 >= 0 && fcntl(fd3, F_GETFD) == FD_CLOEXEC);
    p.fd = fd3; p.events = POLLIN; CHECK(poll(&p, 1, 5000) == 1);
    memset(&info, 0, sizeof(info)); CHECK(waitid(P_PIDFD, fd3, &info, WEXITED) == 0 && info.si_pid == r && info.si_status == 3); close(fd3);
    a.pidfd = (uint64_t)8; ERR(clone3_raw(&a, sizeof(a)), EFAULT);
    /* exit_signal 0: no SIGCHLD, waitable only with __WALL/__WCLONE; exit_signal SIGUSR1 is delivered */
    block(SIGCHLD, 0); while (waitsig(SIGCHLD, 0, &info, 0) == SIGCHLD) {}
    memset(&a, 0, sizeof(a)); r = clone3_raw(&a, sizeof(a)); CHECK(r >= 0); if (!r) _exit(4);
    ERR(waitpid(r, &status, 0), ECHILD); CHECK(waitpid(r, &status, __WCLONE) == r && WIFEXITED(status) && WEXITSTATUS(status) == 4);
    r = clone3_raw(&a, sizeof(a)); CHECK(r >= 0); if (!r) _exit(4);
    CHECK(waitpid(r, &status, __WALL) == r);
    sigset_t pending; CHECK(sigpending(&pending) == 0 && !sigismember(&pending, SIGCHLD));   /* exit_signal 0: none sent */
    unblock(SIGCHLD, 0);
    struct sigaction action; memset(&action, 0, sizeof(action)); action.sa_handler = usr1_handler; sigaction(SIGUSR1, &action, NULL); unblock(SIGUSR1, 0);
    a.exit_signal = SIGUSR1; r = clone3_raw(&a, sizeof(a)); CHECK(r >= 0); if (!r) _exit(6);
    CHECK(waitpid(r, &status, __WCLONE) == r && WEXITSTATUS(status) == 6);
    for (unsigned i = 0; i < 100 && !usr1_seen; i++) sleep_ms(5);
    CHECK(usr1_seen == 1); signal(SIGUSR1, SIG_DFL);
    /* clone3 with a new stack (CLONE_VM | CLONE_VFORK), and CLONE_CLEAR_SIGHAND */
    size_t size = 256 * 1024; uint8_t* stack = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(stack != MAP_FAILED);
    memset(&a, 0, sizeof(a)); a.flags = CLONE_VM | CLONE_VFORK; a.exit_signal = SIGCHLD; a.stack = (uint64_t)stack; a.stack_size = size;
    r = clone3_stack(&a, on_new_stack, stack + size); CHECK(r > 0 && stack_child_ran == 1);
    CHECK(waitpid(r, &status, 0) == r && WIFEXITED(status) && WEXITSTATUS(status) == 5);
    action.sa_handler = usr1_handler; sigaction(SIGUSR1, &action, NULL); signal(SIGUSR2, SIG_IGN);
    memset(&a, 0, sizeof(a)); a.flags = 0x100000000ULL; a.exit_signal = SIGCHLD;
    r = clone3_raw(&a, sizeof(a)); CHECK(r >= 0);
    if (!r) { struct sigaction one, two; sigaction(SIGUSR1, NULL, &one); sigaction(SIGUSR2, NULL, &two); _exit(one.sa_handler == SIG_DFL && two.sa_handler == SIG_IGN ? 0 : 1); }
    CHECK(waitpid(r, &status, 0) == r && WIFEXITED(status) && !WEXITSTATUS(status));
    struct sigaction kept; sigaction(SIGUSR1, NULL, &kept); CHECK(kept.sa_handler == usr1_handler);
    signal(SIGUSR1, SIG_DFL); signal(SIGUSR2, SIG_DFL); munmap(stack, size);
    /* glibc 2.41 creates threads and posix_spawn children through clone3 */
    pthread_t threads[16]; for (unsigned i = 0; i < 16; i++) CHECK(pthread_create(&threads[i], NULL, idle_thread, NULL) == 0);
    for (unsigned i = 0; i < 16; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    pid_t spawned; char* args[] = {self_path, "s", NULL}; extern char** environ;
    CHECK(posix_spawn(&spawned, self_path, NULL, NULL, args, environ) == 0);
    CHECK(waitpid(spawned, &status, 0) == spawned && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    close(self); unblock(SIGUSR1, 0);
    puts("[64] KABI PIDFD PASS: pidfd_open/send_signal/getfd, poll/epoll, waitid P_PIDFD, clone CLONE_PIDFD, clone3 (pidfd, exit_signal, stack, CLEAR_SIGHAND, errors)");
}
/* ---------------- quotactl, getcpu, rseq fallback ---------------- */
extern const unsigned int __rseq_size __attribute__((weak));
static void misc(void) {
    struct { uint64_t limits[8]; } dq;
    ERR(syscall(SYS_quotactl, (0x800007 << 8) | 3, "/", 0, &dq), EINVAL);
    ERR(syscall(SYS_quotactl, (0x800007 << 8) | 0, NULL, 0, &dq), ENODEV);
    CHECK(syscall(SYS_quotactl, (0x800001 << 8) | 0, NULL, 0, NULL) == 0);
    ERR(syscall(SYS_quotactl, (0x800007 << 8) | 0, "/nonexistent-device", 0, &dq), ENOENT);
    ERR(syscall(SYS_quotactl, (0x800007 << 8) | 0, "/etc/hosts", 0, &dq), ENOTBLK);
    ERR(syscall(SYS_quotactl, (0x800007 << 8) | 0, (void*)8, 0, &dq), EFAULT);
    char line[512] = {0}; FILE* mounts = fopen("/proc/mounts", "r"); CHECK(mounts && fgets(line, sizeof(line), mounts)); fclose(mounts);
    char device[256], point[256], type[64]; CHECK(sscanf(line, "%255s %255s %63s", device, point, type) == 3 && !strcmp(point, "/"));
    int root = open("/", O_RDONLY | O_DIRECTORY); CHECK(root >= 0);
    printf("[64] KABI ROOT MOUNT %s %s %s device-node=%s\n", device, point, type, access(device, F_OK) ? "missing" : "present");
    if (access(device, F_OK)) ERR(syscall(SYS_quotactl, (0x800007 << 8) | 0, device, getuid(), &dq), ENOENT);
    if (!strcmp(type, "ext4")) {   /* quota operations, accounting off: ESRCH; own uid needs no privilege */
        ERR(syscall(SYS_quotactl_fd, root, (0x800007 << 8) | 0, getuid(), &dq), ESRCH);
        ERR(syscall(SYS_quotactl_fd, root, (0x800004 << 8) | 0, 0, &dq), ESRCH);
        ERR(syscall(SYS_quotactl_fd, root, (0x800008 << 8) | 0, 0, &dq), EPERM);   /* Q_SETQUOTA needs CAP_SYS_ADMIN */
        CHECK(syscall(SYS_quotactl_fd, root, (0x800001 << 8) | 1, 0, NULL) == 0);
        if (!access(device, F_OK)) {
            ERR(syscall(SYS_quotactl, (0x800007 << 8) | 0, device, getuid(), &dq), ESRCH);
            ERR(syscall(SYS_quotactl, (0x800004 << 8) | 0, device, 0, &dq), ESRCH);
            CHECK(syscall(SYS_quotactl, (0x800001 << 8) | 1, device, 0, NULL) == 0);
        }
    } else if (!access(device, F_OK)) {   /* vfat: no quota operations at all */
        ERR(syscall(SYS_quotactl, (0x800007 << 8) | 0, device, getuid(), &dq), ENOSYS);
        ERR(syscall(SYS_quotactl_fd, root, (0x800007 << 8) | 0, getuid(), &dq), ENOSYS);
    }
    ERR(syscall(SYS_quotactl_fd, 999, (0x800007 << 8) | 0, 0, &dq), EBADF);
    close(root);
    unsigned cpu = 99, node = 99; CHECK(syscall(SYS_getcpu, &cpu, &node, NULL) == 0 && cpu < (unsigned)sysconf(_SC_NPROCESSORS_CONF) && node == 0);
    ERR(syscall(SYS_getcpu, (void*)8, NULL, NULL), EFAULT);
    CHECK(sched_getcpu() >= 0);
    /* sched_getcpu (rseq unregistered: the vDSO's getcpu) agrees with the kernel whenever no migration came between */
    unsigned agreed = 0;
    for (unsigned i = 0; i < 2000; i++) {
        int a = sched_getcpu(); unsigned k; CHECK(syscall(SYS_getcpu, &k, NULL, NULL) == 0); int c = sched_getcpu();
        CHECK(a >= 0 && a < sysconf(_SC_NPROCESSORS_CONF));
        if (a == c) { CHECK((unsigned)a == k); agreed++; }
    }
    CHECK(agreed > 1000);
    /* epoll_pwait2 (sd-event, fcitx5) and MAP_POPULATE file mappings (libime's language models) */
    int ep2 = epoll_create1(0), ev2 = eventfd(0, 0); struct epoll_event e2 = {.events = EPOLLIN, .data.u32 = 7}, out2;
    CHECK(ep2 >= 0 && ev2 >= 0 && epoll_ctl(ep2, EPOLL_CTL_ADD, ev2, &e2) == 0);
    struct timespec zero_ts = {0, 0}, wait_ts = {0, 30000000}, bad_ts = {0, 1000000000};
    CHECK(syscall(441, ep2, &out2, 1, &zero_ts, NULL, 8) == 0);
    uint64_t t0 = mono(); CHECK(syscall(441, ep2, &out2, 1, &wait_ts, NULL, 8) == 0 && mono() - t0 >= 29000000ULL);
    ERR(syscall(441, ep2, &out2, 1, &bad_ts, NULL, 8), EINVAL); ERR(syscall(441, ep2, &out2, 1, (void*)8, NULL, 8), EFAULT);
    CHECK(eventfd_write(ev2, 1) == 0 && syscall(441, ep2, &out2, 1, NULL, NULL, 8) == 1 && out2.data.u32 == 7);
    close(ev2); close(ep2);
    int self_fd = open(self_path, O_RDONLY); CHECK(self_fd >= 0);
    void* populated = mmap(NULL, 8192, PROT_READ, MAP_SHARED | MAP_POPULATE, self_fd, 0);
    CHECK(populated != MAP_FAILED && !memcmp(populated, "\x7f" "ELF", 4)); munmap(populated, 8192); close(self_fd);
    /* 32-bit arguments: Linux ignores the upper half of an int/unsigned int register (the C ABI leaves it undefined).
     * firefox-esr's memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL) arrived as 0x1_0000000B. */
    const uint64_t junk = 0xDEADBEEF00000000ULL;
    int mfd = syscall(SYS_memfd_create, "kabi", junk | 0xB); CHECK(mfd >= 0 && fcntl(mfd, F_GETFD) == FD_CLOEXEC);
    CHECK((fcntl(mfd, F_GET_SEALS) & F_SEAL_EXEC) != 0); close(mfd);
    ERR(syscall(SYS_memfd_create, "kabi", junk | 0x18), EINVAL);   /* NOEXEC_SEAL and EXEC together: still refused */
    int efd = syscall(SYS_eventfd2, junk | 3, junk | EFD_CLOEXEC); CHECK(efd >= 0 && fcntl(efd, F_GETFD) == FD_CLOEXEC);
    uint64_t counter; CHECK(read(efd, &counter, 8) == 8 && counter == 3);
    int dupped = syscall(SYS_dup3, junk | (uint32_t)efd, junk | 40, junk | O_CLOEXEC); CHECK(dupped == 40 && fcntl(40, F_GETFD) == FD_CLOEXEC);
    close(40); close(efd);
    int pipes[2]; CHECK(syscall(SYS_pipe2, pipes, junk | O_CLOEXEC) == 0); close(pipes[0]); close(pipes[1]);
    int ep = syscall(SYS_epoll_create1, junk | EPOLL_CLOEXEC); CHECK(ep >= 0); close(ep);
    int at = syscall(SYS_openat, 0xFFFFFF9CULL, "/proc/self/status", junk | O_RDONLY | O_CLOEXEC, 0);   /* AT_FDCWD zero-extended */
    CHECK(at >= 0); close(at);
    pid_t kid = fork(); CHECK(kid >= 0); if (!kid) _exit(12);
    int status; CHECK(syscall(SYS_wait4, junk | (uint32_t)kid, &status, junk | __WALL, NULL) == kid && WEXITSTATUS(status) == 12);
    CHECK(syscall(SYS_kill, junk | (uint32_t)getpid(), junk | 0) == 0);
    ERR(syscall(SYS_kill, getpid(), junk | 99), EINVAL);
    uint32_t word = 5; struct timespec zero = {0, 0};   /* futex: int op (FUTEX_WAKE_PRIVATE 129, FUTEX_WAIT_PRIVATE 128) */
    CHECK(syscall(SYS_futex, &word, junk | 129, junk | 1, NULL, NULL, 0) == 0);
    ERR(syscall(SYS_futex, &word, junk | 128, junk | 4, &zero, NULL, 0), EAGAIN);
    ERR(syscall(SYS_futex, &word, junk | 128, junk | 5, &zero, NULL, 0), ETIMEDOUT);
    int dir = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC); CHECK(dir >= 0);   /* getdents64: unsigned int fd */
    char entries[1024]; CHECK(syscall(SYS_getdents64, junk | (uint32_t)dir, entries, sizeof(entries)) > 0); close(dir);
    /* protection keys without CPU support: ENOSPC, keys -1/0 mean plain mprotect */
    ERR(syscall(SYS_pkey_alloc, 0, 0), ENOSPC); ERR(syscall(SYS_pkey_alloc, 1, 0), EINVAL); ERR(syscall(SYS_pkey_alloc, 0, 4), EINVAL);
    ERR(syscall(SYS_pkey_free, 3), EINVAL);
    void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(page != MAP_FAILED);
    CHECK(syscall(SYS_pkey_mprotect, page, 4096, PROT_READ, -1) == 0 && syscall(SYS_pkey_mprotect, page, 4096, PROT_READ, 0) == 0);
    ERR(syscall(SYS_pkey_mprotect, page, 4096, PROT_READ, 1), EINVAL);
    /* NUMA policy on one node */
    int mode = -1; unsigned long nodes = 99;
    CHECK(syscall(SYS_get_mempolicy, &mode, &nodes, 64, 0, 0) == 0 && mode == 0 && nodes == 0);
    unsigned long node0 = 1, node1 = 2;
    CHECK(syscall(SYS_set_mempolicy, 2 /* MPOL_BIND */, &node0, 64) == 0);
    CHECK(syscall(SYS_get_mempolicy, &mode, &nodes, 64, 0, 0) == 0 && mode == 2 && nodes == 1);
    ERR(syscall(SYS_set_mempolicy, 2, &node1, 64), EINVAL); ERR(syscall(SYS_set_mempolicy, 9, &node0, 64), EINVAL);
    ERR(syscall(SYS_set_mempolicy, 0, &node0, 64), EINVAL); ERR(syscall(SYS_set_mempolicy, 3, (void*)8, 64), EFAULT);
    ERR(syscall(SYS_set_mempolicy, 0xC000 | 2, &node0, 64), EINVAL);
    CHECK(syscall(SYS_get_mempolicy, &mode, NULL, 0, page, 3) == 0 && mode == 0);   /* MPOL_F_NODE|MPOL_F_ADDR: node 0 */
    ERR(syscall(SYS_get_mempolicy, &mode, NULL, 0, 0, 1), EINVAL); ERR(syscall(SYS_get_mempolicy, &mode, NULL, 0, 0, 8), EINVAL);
    CHECK(syscall(SYS_mbind, page, 4096, 2, &node0, 64, 0) == 0); ERR(syscall(SYS_mbind, (char*)page + 1, 4096, 2, &node0, 64, 0), EINVAL);
    CHECK(syscall(SYS_set_mempolicy, 0, NULL, 0) == 0 && syscall(SYS_get_mempolicy, &mode, NULL, 0, 0, 0) == 0 && mode == 0);
    munmap(page, 4096);
    /* I/O priority: unset reads best-effort (nice 0 -> 4), set/get round trip, inherited by fork */
    long prio = syscall(SYS_ioprio_get, 1, 0); CHECK(prio == (2 << 13 | 4));
    CHECK(syscall(SYS_ioprio_set, 1, 0, 3 << 13) == 0 && syscall(SYS_ioprio_get, 1, getpid()) == 3 << 13);   /* idle */
    ERR(syscall(SYS_ioprio_set, 1, 0, 2 << 13 | 8 << 3), EINVAL); ERR(syscall(SYS_ioprio_set, 1, 0, 5 << 13), EINVAL);
    ERR(syscall(SYS_ioprio_set, 1, 0, 0 << 13 | 1), EINVAL); CHECK(syscall(SYS_ioprio_set, 1, 0, 2 << 13 | 1 << 3 | 1) == 0);   /* a valid hint */ ERR(syscall(SYS_ioprio_set, 4, 0, 0), EINVAL);
    if (geteuid()) ERR(syscall(SYS_ioprio_set, 1, 0, 1 << 13), EPERM);
    ERR(syscall(SYS_ioprio_get, 1, 0x7FFFFFF0), ESRCH);
    long before_fork = syscall(SYS_ioprio_get, 1, 0); CHECK(before_fork == (2 << 13 | 1 << 3 | 1));
    kid = fork(); CHECK(kid >= 0); if (!kid) _exit(syscall(SYS_ioprio_get, 1, 0) == before_fork ? 0 : 1);
    CHECK(waitpid(kid, &status, 0) == kid && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(syscall(SYS_ioprio_get, 2, 0) >= 0 && syscall(SYS_ioprio_set, 1, 0, 0) == 0 && syscall(SYS_ioprio_get, 1, 0) == (2 << 13 | 4));
    ERR(syscall(SYS_rseq, NULL, 32, 0, 0x53053053), ENOSYS);   /* not implemented on purpose: glibc's fallback is used */
    CHECK(&__rseq_size == NULL || __rseq_size == 0);
    puts("[64] KABI MISC PASS: quotactl/quotactl_fd errno, getcpu, rseq fallback (glibc __rseq_size 0, sched_getcpu)");
}
/* /proc/mounts, /proc/self/mountinfo and statfs agree with each other and with Linux: filesystem magics, a real root
 * device, mountinfo major:minor = st_dev of the mount point (what findmnt, df and systemd match on). */
static void mounts_check(void) {
    static const struct { const char* path; unsigned long magic; } virt[] = {
        {"/proc", 0x9FA0}, {"/proc/self/status", 0x9FA0}, {"/sys", 0x62656572}, {"/sys/class", 0x62656572},
        {"/dev", 0x01021994}, {"/dev/null", 0x01021994}, {"/dev/pts", 0x1CD1}, {"/dev/shm", 0x01021994}};
    struct statfs s;
    for (unsigned i = 0; i < sizeof(virt) / sizeof(virt[0]); i++) {
        if (statfs(virt[i].path, &s)) { dprintf(2, "[64] KABI FAIL statfs %s errno=%d\n", virt[i].path, errno); _exit(99); }
        if ((unsigned long)s.f_type != virt[i].magic) { dprintf(2, "[64] KABI FAIL statfs %s type=%lx\n", virt[i].path, (unsigned long)s.f_type); _exit(99); }
    }
    int pipes[2]; CHECK(pipe(pipes) == 0 && fstatfs(pipes[0], &s) == 0 && s.f_type == 0x50495045); close(pipes[0]); close(pipes[1]);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(sock >= 0 && fstatfs(sock, &s) == 0 && s.f_type == 0x534F434B); close(sock);
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0); CHECK(tfd >= 0 && fstatfs(tfd, &s) == 0 && s.f_type == 0x09041934); close(tfd);
    /* mountinfo: every mount point's st_dev is its major:minor; /proc/mounts lists the same filesystems */
    FILE* info = fopen("/proc/self/mountinfo", "r"); CHECK(info);
    char line[1024], root_type[64] = "", root_source[256] = ""; unsigned lines = 0;
    while (fgets(line, sizeof(line), info)) {
        unsigned id, parent, major_number, minor_number; char root[256], point[256], type[64], source[256];
        char* dash = strstr(line, " - "); CHECK(dash);
        CHECK(sscanf(line, "%u %u %u:%u %255s %255s", &id, &parent, &major_number, &minor_number, root, point) == 6);
        CHECK(sscanf(dash + 3, "%63s %255s", type, source) == 2);
        struct stat st; CHECK(stat(point, &st) == 0);
        if (major(st.st_dev) != major_number || minor(st.st_dev) != minor_number) {
            dprintf(2, "[64] KABI FAIL mountinfo %s %u:%u st_dev %u:%u\n", point, major_number, minor_number, major(st.st_dev), minor(st.st_dev)); _exit(99);
        }
        if (!strcmp(point, "/")) { strcpy(root_type, type); strcpy(root_source, source); }
        lines++;
    }
    fclose(info); CHECK(lines >= 6);
    CHECK(statfs("/", &s) == 0 && (unsigned long)s.f_type == (!strcmp(root_type, "ext4") ? 0xEF53UL : 0x4D44UL));
    struct stat device; CHECK(stat(root_source, &device) == 0 && S_ISBLK(device.st_mode));   /* a real block device node */
    FILE* mounts = fopen("/proc/mounts", "r"); CHECK(mounts && fgets(line, sizeof(line), mounts));
    char m_source[256], m_point[256], m_type[64]; CHECK(sscanf(line, "%255s %255s %63s", m_source, m_point, m_type) == 3);
    CHECK(!strcmp(m_point, "/") && !strcmp(m_type, root_type) && !strcmp(m_source, root_source)); fclose(mounts);
    printf("[64] KABI MOUNTS PASS: root %s %s, mountinfo st_dev agreement (%u mounts), statfs magics\n", root_source, root_type, lines);
}
int main(int argc, char** argv) {
    self_path = argv[0]; setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && argv[1][0] == 'e') {   /* exec'd image: the parent's timers are gone */
        struct itimerspec v; return sys_timer_gettime(atoi(argv[2]), &v) == -1 && errno == EINVAL ? 0 : 1;
    }
    if (argc == 2 && argv[1][0] == 's') return 42;   /* posix_spawn child */
    if (argc == 2 && argv[1][0] == 'r') { alarm(120); root_checks(); return 0; }
    alarm(600);
    posix_timers(); itimers(); timerfds(); signalfds(); pidfds(); misc(); mounts_check();
    alarm(0);
    puts("[64] KABI ALL PASSED: POSIX timers, itimers, timerfd, signalfd, sigtimedwait/sigqueueinfo, pidfd, clone3, quotactl, getcpu, rseq");
    return 0;
}
