#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] EVENT FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static int delayed_fd;
static int counter_fd;
static pthread_t main_thread;
static volatile sig_atomic_t seen, saved_mask;
static void signal_handler(int number, siginfo_t* info, void* context) {
    (void)info;
    seen = number; saved_mask = sigismember(&((ucontext_t*)context)->uc_sigmask, SIGUSR1);
}
static uint64_t read_clock(clockid_t id) {
    struct timespec t; CHECK(clock_gettime(id, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t now(void) { return read_clock(CLOCK_MONOTONIC); }
static uint64_t worker_cpu_ns;
static void* cpu_worker(void* unused) {
    (void)unused; uint64_t before = read_clock(CLOCK_THREAD_CPUTIME_ID);
    volatile uint64_t value = 1;
    for (unsigned i = 0; i < 2000000; i++) value = value * 3 + i;
    worker_cpu_ns = read_clock(CLOCK_THREAD_CPUTIME_ID) - before; return NULL;
}
static void* delayed(void* signal) {
    CHECK(poll(NULL, 0, 80) == 0);
    if (signal) CHECK(pthread_kill(main_thread, SIGUSR1) == 0);
    else CHECK(write(delayed_fd, "D", 1) == 1);
    return NULL;
}
static void* counter_worker(void* writing) {
    CHECK(poll(NULL, 0, 60) == 0);
    eventfd_t value;
    if (writing) CHECK(eventfd_write(counter_fd, 7) == 0);
    else CHECK(eventfd_read(counter_fd, &value) == 0 && value == UINT64_MAX - 1);
    return NULL;
}
int main(void) {
    uid_t real, effective, saved; gid_t real_group, effective_group, saved_group;
    CHECK(getresuid(&real, &effective, &saved) == 0 && real == getuid() && effective == geteuid() && saved == effective);
    CHECK(getresgid(&real_group, &effective_group, &saved_group) == 0 && real_group == getgid() && effective_group == getegid() && saved_group == effective_group);
    CHECK(syscall(SYS_getresuid, &real, (void*)1, &saved) == -1 && errno == EFAULT);
    CHECK(epoll_create(0) == -1 && errno == EINVAL && epoll_create1(1) == -1 && errno == EINVAL);
    int ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0 && fcntl(ep, F_GETFD) == FD_CLOEXEC);
    int pair[2]; CHECK(pipe2(pair, O_NONBLOCK | O_CLOEXEC) == 0);
    struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 0x123456789ABCDEF0ULL}, events[8];
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &interest) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &interest) == -1 && errno == EEXIST);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &interest) == -1 && errno == EINVAL);
    int file = open("/etc/native64.dat", O_RDONLY); CHECK(file >= 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, file, &interest) == -1 && errno == EPERM);
    CHECK(epoll_wait(ep, events, 8, 0) == 0 && write(pair[1], "abc", 3) == 3);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && events[0].events == EPOLLIN && events[0].data.u64 == interest.data.u64);
    CHECK(epoll_wait(ep, events, 8, 0) == 1);
    char data[32]; CHECK(read(pair[0], data, sizeof(data)) == 3 && epoll_wait(ep, events, 8, 0) == 0);
    interest.events = EPOLLIN | EPOLLET;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, pair[0], &interest) == 0 && write(pair[1], "abc", 3) == 3);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && epoll_wait(ep, events, 8, 0) == 0);
    CHECK(read(pair[0], data, 1) == 1 && epoll_wait(ep, events, 8, 0) == 0);
    CHECK(write(pair[1], "d", 1) == 1 && epoll_wait(ep, events, 8, 0) == 1);
    interest.events = EPOLLIN | EPOLLONESHOT;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, pair[0], &interest) == 0 && epoll_wait(ep, events, 8, 0) == 1);
    CHECK(write(pair[1], "e", 1) == 1 && epoll_wait(ep, events, 8, 0) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, pair[0], &interest) == 0 && epoll_wait(ep, events, 8, 0) == 1);
    CHECK(read(pair[0], data, sizeof(data)) == 4);
    interest.events = EPOLLIN;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, pair[0], &interest) == 0);
    struct pollfd requests[] = {{pair[0], POLLIN, 0}, {pair[1], POLLOUT, 0}, {123456, POLLIN, 0}, {-1, POLLIN, 0}, {file, POLLIN | POLLOUT, 0}};
    CHECK(poll(requests, 5, 0) == 3 && requests[0].revents == 0 && requests[1].revents == POLLOUT);
    CHECK(requests[2].revents == POLLNVAL && requests[3].revents == 0 && requests[4].revents == (POLLIN | POLLOUT));
    CHECK(close(file) == 0);
    pthread_t worker; delayed_fd = pair[1]; uint64_t start = now();
    CHECK(pthread_create(&worker, NULL, delayed, NULL) == 0);
    CHECK(epoll_wait(ep, events, 8, 1000) == 1 && (events[0].events & EPOLLIN) && now() - start >= 80000000);
    CHECK(pthread_join(worker, NULL) == 0 && read(pair[0], data, sizeof(data)) == 1 && data[0] == 'D');
    start = now(); CHECK(epoll_wait(ep, events, 8, 50) == 0 && now() - start >= 50000000);
    CHECK(epoll_wait(ep, events, 0, 0) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_epoll_wait, ep, (void*)1, 8, 0) == -1 && errno == EFAULT);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, pair[0], NULL) == 0 && close(pair[0]) == 0 && close(pair[1]) == 0);

    int a[2], b[2]; CHECK(pipe(a) == 0 && pipe(b) == 0);
    interest.data.u64 = 2; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, a[0], &interest) == 0);
    interest.data.u64 = 3; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, b[0], &interest) == 0);
    CHECK(write(a[1], "a", 1) == 1 && write(b[1], "b", 1) == 1);
    CHECK(epoll_wait(ep, events, 1, 0) == 1); uint64_t first = events[0].data.u64;
    CHECK(epoll_wait(ep, events, 1, 0) == 1 && first + events[0].data.u64 == 5);
    CHECK(close(a[1]) == 0 && close(b[1]) == 0);
    CHECK(epoll_wait(ep, events, 8, 0) == 2 && (events[0].events & EPOLLHUP) && (events[1].events & EPOLLHUP));
    CHECK(close(a[0]) == 0 && close(b[0]) == 0 && epoll_wait(ep, events, 8, 0) == 0);
    CHECK(pipe(pair) == 0); int copy = dup(pair[0]); CHECK(copy >= 0);
    interest.data.u64 = 41; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &interest) == 0);
    interest.data.u64 = 42; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, copy, &interest) == 0);
    CHECK(close(pair[0]) == 0 && write(pair[1], "x", 1) == 1);
    CHECK(epoll_wait(ep, events, 8, 0) == 2 && events[0].data.u64 + events[1].data.u64 == 83);
    CHECK(read(copy, data, sizeof(data)) == 1 && close(copy) == 0 && close(pair[1]) == 0);
    CHECK(epoll_wait(ep, events, 8, 0) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    interest.events = EPOLLIN | EPOLLRDHUP; interest.data.u64 = 99;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &interest) == 0 && shutdown(pair[1], SHUT_WR) == 0);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && (events[0].events & (EPOLLIN | EPOLLRDHUP)) == (EPOLLIN | EPOLLRDHUP));
    CHECK(read(pair[0], data, 1) == 0 && close(pair[1]) == 0);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && (events[0].events & EPOLLHUP) && !(events[0].events & EPOLLERR));
    CHECK(close(pair[0]) == 0 && epoll_wait(ep, events, 8, 0) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &interest) == 0 && write(pair[0], "unread", 6) == 6 && close(pair[1]) == 0);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && (events[0].events & EPOLLERR));
    int socket_error = 0; socklen_t error_size = sizeof(socket_error);
    CHECK(getsockopt(pair[0], SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 && socket_error == ECONNRESET);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && !(events[0].events & EPOLLERR) && read(pair[0], data, 1) == 0 && close(pair[0]) == 0);

    CHECK(eventfd(0, 2) == -1 && errno == EINVAL);
    int counter = eventfd(2, EFD_CLOEXEC | EFD_NONBLOCK); CHECK(counter >= 0 && fcntl(counter, F_GETFD) == FD_CLOEXEC);
    interest.events = EPOLLIN | EPOLLOUT; interest.data.u64 = 123;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, counter, &interest) == 0);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && events[0].events == (EPOLLIN | EPOLLOUT));
    eventfd_t value; CHECK(eventfd_read(counter, &value) == 0 && value == 2);
    CHECK(eventfd_read(counter, &value) == -1 && errno == EAGAIN);
    CHECK(eventfd_write(counter, UINT64_MAX) == -1 && errno == EINVAL);
    CHECK(eventfd_write(counter, UINT64_MAX - 1) == 0 && eventfd_write(counter, 1) == -1 && errno == EAGAIN);
    CHECK(epoll_wait(ep, events, 8, 0) == 1 && events[0].events == EPOLLIN);
    CHECK(syscall(SYS_read, counter, (void*)1, 8) == -1 && errno == EFAULT);
    CHECK(eventfd_read(counter, &value) == 0 && value == UINT64_MAX - 1 && close(counter) == 0);
    CHECK(epoll_wait(ep, events, 8, 0) == 0);
    counter = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK); CHECK(counter >= 0);
    for (int i = 0; i < 3; i++) CHECK(eventfd_read(counter, &value) == 0 && value == 1);
    CHECK(eventfd_read(counter, &value) == -1 && errno == EAGAIN);
    value = 4; struct iovec vectors[] = {{&value, 3}, {(char*)&value + 3, 5}};
    CHECK(writev(counter, vectors, 2) == 8); value = 0;
    CHECK(readv(counter, vectors, 2) == 8 && value == 1);
    CHECK(close(counter) == 0);
    counter_fd = eventfd(0, 0); CHECK(counter_fd >= 0);
    CHECK(pthread_create(&worker, NULL, counter_worker, (void*)1) == 0);
    CHECK(eventfd_read(counter_fd, &value) == 0 && value == 7 && pthread_join(worker, NULL) == 0);
    CHECK(eventfd_write(counter_fd, UINT64_MAX - 1) == 0);
    CHECK(pthread_create(&worker, NULL, counter_worker, NULL) == 0);
    CHECK(eventfd_write(counter_fd, 1) == 0 && pthread_join(worker, NULL) == 0 && eventfd_read(counter_fd, &value) == 0 && value == 1);
    CHECK(close(counter_fd) == 0);
    counter = (int)syscall(SYS_eventfd, 5); CHECK(counter >= 0 && eventfd_read(counter, &value) == 0 && value == 5 && close(counter) == 0);
    puts("[64] EVENTFD PASS: counters/semaphores, overflow, epoll, vectored buffers and blocking readers/writers");

    main_thread = pthread_self();
    struct sigaction action = {.sa_sigaction = signal_handler, .sa_flags = SA_SIGINFO | SA_RESTART}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    sigset_t blocked, empty, current; sigemptyset(&blocked); sigaddset(&blocked, SIGUSR1); sigemptyset(&empty);
    CHECK(pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0);
    CHECK(pthread_create(&worker, NULL, delayed, (void*)1) == 0);
    CHECK(epoll_pwait(ep, events, 8, 1000, &empty) == -1 && errno == EINTR && seen == SIGUSR1 && saved_mask == 1);
    CHECK(pthread_join(worker, NULL) == 0 && pthread_sigmask(SIG_SETMASK, NULL, &current) == 0 && sigismember(&current, SIGUSR1));
    CHECK(epoll_pwait(ep, events, 8, 30, &empty) == 0 && pthread_sigmask(SIG_SETMASK, NULL, &current) == 0 && sigismember(&current, SIGUSR1));
    struct timespec timeout = {.tv_nsec = 20000000}; start = now();
    CHECK(syscall(SYS_ppoll, NULL, 0, &timeout, NULL, 8) == 0 && now() - start >= 20000000 && timeout.tv_sec == 0 && timeout.tv_nsec == 0);
    timeout = (struct timespec){.tv_sec = 1}; seen = saved_mask = 0;
    CHECK(pthread_create(&worker, NULL, delayed, (void*)1) == 0);
    CHECK(ppoll(NULL, 0, &timeout, &empty) == -1 && errno == EINTR && seen == SIGUSR1 && saved_mask == 1);
    CHECK(timeout.tv_sec == 1 && timeout.tv_nsec == 0 && pthread_join(worker, NULL) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0 && sigismember(&current, SIGUSR1));
    timeout = (struct timespec){.tv_nsec = 1000000000}; CHECK(ppoll(NULL, 0, &timeout, NULL) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_ppoll, NULL, 0, (void*)1, NULL, 8) == -1 && errno == EFAULT);
    puts("[64] PPOLL PASS: deadlines, raw remaining time, libc const timeout and atomic signal-mask restore");
    CHECK(pthread_sigmask(SIG_UNBLOCK, &blocked, NULL) == 0);
    timeout = (struct timespec){.tv_nsec = 20000000}; start = now();
    CHECK(syscall(SYS_nanosleep, &timeout, (void*)1) == 0 && now() - start >= 20000000);
    struct timespec remaining = {.tv_sec = 99, .tv_nsec = 88};
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &(struct timespec){0}, &remaining) == 0 && remaining.tv_sec == 99);
    timeout = (struct timespec){.tv_nsec = 1000000000}; CHECK(clock_nanosleep(CLOCK_MONOTONIC, 0, &timeout, NULL) == EINVAL);
    CHECK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, (void*)1, NULL) == -1 && errno == EFAULT);
    CHECK(clock_nanosleep(CLOCK_THREAD_CPUTIME_ID, 0, &(struct timespec){0}, NULL) == EINVAL);
    timeout = (struct timespec){.tv_sec = 1}; seen = 0;
    CHECK(pthread_create(&worker, NULL, delayed, (void*)1) == 0);
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, 0, &timeout, &remaining) == EINTR && seen == SIGUSR1);
    CHECK(timeout.tv_sec == 1 && remaining.tv_sec == 0 && remaining.tv_nsec > 0 && remaining.tv_nsec < 1000000000 && pthread_join(worker, NULL) == 0);
    CHECK(clock_gettime(CLOCK_REALTIME, &timeout) == 0); timeout.tv_sec++; remaining = (struct timespec){99, 88}; seen = 0;
    CHECK(pthread_create(&worker, NULL, delayed, (void*)1) == 0);
    CHECK(clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &timeout, &remaining) == EINTR && seen == SIGUSR1);
    CHECK(remaining.tv_sec == 99 && remaining.tv_nsec == 88 && pthread_join(worker, NULL) == 0);
    puts("[64] SLEEP PASS: real deadlines, absolute clocks, EINTR despite SA_RESTART and relative remainder");
    start = now(); uint64_t cpu = read_clock(CLOCK_THREAD_CPUTIME_ID);
    CHECK(nanosleep(&(struct timespec){.tv_nsec = 20000000}, NULL) == 0);
    CHECK(read_clock(CLOCK_THREAD_CPUTIME_ID) - cpu < now() - start);
    uint64_t process_cpu = read_clock(CLOCK_PROCESS_CPUTIME_ID); cpu = read_clock(CLOCK_THREAD_CPUTIME_ID);
    CHECK(pthread_create(&worker, NULL, cpu_worker, NULL) == 0 && pthread_join(worker, NULL) == 0 && worker_cpu_ns > 0);
    uint64_t thread_delta = read_clock(CLOCK_THREAD_CPUTIME_ID) - cpu;
    uint64_t process_delta = read_clock(CLOCK_PROCESS_CPUTIME_ID) - process_cpu;
    CHECK(process_delta >= worker_cpu_ns && process_delta > thread_delta);
    struct rlimit limit; CHECK(getrlimit(RLIMIT_DATA, &limit) == 0 && limit.rlim_cur == RLIM_INFINITY && limit.rlim_max == RLIM_INFINITY);
    CHECK(prlimit(0, RLIMIT_DATA, NULL, &limit) == 0 && limit.rlim_cur == RLIM_INFINITY);
    puts("[64] CPU CLOCK PASS: running thread/process accounting excludes sleep; data-limit queries reflect no policy quota");
    CHECK(close(ep) == 0);
    puts("[64] EVENT ALL PASSED: poll/epoll wakeups, deadlines, level/edge/oneshot, fairness, dup lifetime, EOF and atomic signal masks");
    return 0;
}
