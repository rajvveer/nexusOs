#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] THREAD FAIL line=%d errno=%d tid=%ld\n", __LINE__, errno, syscall(SYS_gettid)); _exit(99); } } while (0)
#define WORKERS 4
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_key_t destructor_key;
static int ready, released, sum, ids[WORKERS], main_pid, shared_fd;
static atomic_int destructors, group_signal, forked_child;
static __thread unsigned local = 17;
static __thread volatile sig_atomic_t signaled;
static void handler(int number) {
    if (number == SIGUSR1) signaled++;
    else atomic_store(&group_signal, (int)local);
}
static void destroyed(void* value) { CHECK(value != NULL); atomic_fetch_add(&destructors, 1); }
static void* worker(void* argument) {
    intptr_t index = (intptr_t)argument;
    CHECK(local == 17 && getpid() == main_pid && getppid() == 0);
    local = (unsigned)index + 100;
    ids[index] = (int)syscall(SYS_gettid); CHECK(ids[index] != main_pid);
    stack_t stack; CHECK(sigaltstack(NULL, &stack) == 0 && (stack.ss_flags & SS_DISABLE));
    CHECK(pthread_setspecific(destructor_key, (void*)(index + 1)) == 0);
    uint32_t original, mode, restored;
    __asm__ volatile("stmxcsr %0" : "=m"(original)); mode = (original & ~0x6000U) | ((unsigned)index << 13);
    __asm__ volatile("ldmxcsr %0" :: "m"(mode));
    CHECK(pthread_mutex_lock(&mutex) == 0); ready++;
    CHECK(pthread_cond_broadcast(&condition) == 0);
    while (!released) CHECK(pthread_cond_wait(&condition, &mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    if (index == 1) {
        sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
        CHECK(pthread_sigmask(SIG_UNBLOCK, &mask, NULL) == 0);
    }
    for (unsigned i = 0; i < 1000; i++) {
        CHECK(pthread_mutex_lock(&mutex) == 0);
        if (!(i % 32)) CHECK(sched_yield() == 0);
        CHECK(local == index + 100); sum++;
        __asm__ volatile("stmxcsr %0" : "=m"(restored)); CHECK((restored & 0x6000) == (mode & 0x6000));
        CHECK(pthread_mutex_unlock(&mutex) == 0);
    }
    CHECK(signaled == 1);
    if (index == 2) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) _exit(getppid() == main_pid && getpid() == syscall(SYS_gettid) ? 61 : 99);
        atomic_store(&forked_child, child);
    }
    if (index == 0) CHECK(close(shared_fd) == 0);
    __asm__ volatile("ldmxcsr %0" :: "m"(original));
    return (void*)(index + 200);
}
static uint64_t clock_ns(clockid_t clock) {
    struct timespec time; CHECK(clock_gettime(clock, &time) == 0);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}
static void* last_thread(void* argument) {
    (void)argument;
    CHECK(getpid() == main_pid && syscall(SYS_gettid) != main_pid);
    CHECK(sched_yield() == 0);
    dprintf(1, "[64] THREAD remains alive after initial thread exit PASS\n");
    return NULL;
}
static int pair[2];
static atomic_int blocking_ready;
static int futex_word;
static atomic_int bit_waiters;
static void* bit_waiter(void* argument) {
    atomic_fetch_add(&bit_waiters, 1);
    CHECK(syscall(SYS_futex, &futex_word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, (uintptr_t)argument) == 0);
    return argument;
}
static void* pinned_reader(void* argument) {
    (void)argument; atomic_store(&blocking_ready, 1);
    char byte; CHECK(read(pair[0], &byte, 1) == 1 && byte == 'P'); return NULL;
}
static void* blocked_thread(void* argument) {
    atomic_fetch_add(&blocking_ready, 1);
    if (argument) {
        char byte; read(pair[0], &byte, 1);
    } else syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    CHECK(0); return NULL;
}
static void* fatal_thread(void* argument) {
    (void)argument;
    CHECK(sched_yield() == 0 && syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGTERM) == 0);
    CHECK(0); return NULL;
}
int main(int argc, char** argv) {
    CHECK(argc == 2); main_pid = getpid(); CHECK(syscall(SYS_gettid) == main_pid);
    pthread_t threads[WORKERS];
    if (argv[1][0] == 'l') {
        CHECK(pthread_create(&threads[0], NULL, last_thread, NULL) == 0);
        syscall(SYS_exit, 0); CHECK(0);
    }
    if (argv[1][0] == 'g' || argv[1][0] == 'f') {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        CHECK(pthread_create(&threads[0], NULL, blocked_thread, NULL) == 0);
        CHECK(pthread_create(&threads[1], NULL, blocked_thread, (void*)1) == 0);
        while (atomic_load(&blocking_ready) != 2) CHECK(sched_yield() == 0);
        CHECK(sched_yield() == 0);
        if (argv[1][0] == 'g') _exit(42);
        CHECK(pthread_create(&threads[2], NULL, fatal_thread, NULL) == 0);
        char byte; read(pair[0], &byte, 1); CHECK(0);
    }
    struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0 && sigaction(SIGUSR2, &action, NULL) == 0);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
    CHECK(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
    void* alt = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(alt != MAP_FAILED);
    stack_t stack = {.ss_sp = alt, .ss_size = 65536}; CHECK(sigaltstack(&stack, NULL) == 0);
    CHECK(pthread_key_create(&destructor_key, destroyed) == 0);
    shared_fd = open("/etc/native64.dat", O_RDONLY); CHECK(shared_fd >= 0);
    for (intptr_t i = 0; i < WORKERS; i++) CHECK(pthread_create(&threads[i], NULL, worker, (void*)i) == 0);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    while (ready != WORKERS) CHECK(pthread_cond_wait(&condition, &mutex) == 0);
    for (int i = 0; i < WORKERS; i++) {
        for (int j = 0; j < i; j++) CHECK(ids[i] != ids[j]);
        CHECK(pthread_kill(threads[i], SIGUSR1) == 0);
    }
    CHECK(kill(getpid(), SIGUSR2) == 0);
    released = 1; CHECK(pthread_cond_broadcast(&condition) == 0 && pthread_mutex_unlock(&mutex) == 0);
    for (intptr_t i = 0; i < WORKERS; i++) {
        void* result; CHECK(pthread_join(threads[i], &result) == 0 && result == (void*)(i + 200));
    }
    CHECK(sum == WORKERS * 1000 && local == 17 && atomic_load(&destructors) == WORKERS && atomic_load(&group_signal) == 101);
    int status; pid_t child = atomic_load(&forked_child); CHECK(child > main_pid);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 61);
    CHECK(fcntl(shared_fd, F_GETFD) == -1 && errno == EBADF && pthread_key_delete(destructor_key) == 0);
    CHECK(sigaltstack(NULL, &stack) == 0 && stack.ss_sp == alt);
    stack.ss_flags = SS_DISABLE; CHECK(sigaltstack(&stack, NULL) == 0 && munmap(alt, 65536) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(pthread_create(&threads[0], NULL, pinned_reader, NULL) == 0);
    while (!atomic_load(&blocking_ready)) CHECK(sched_yield() == 0);
    /* On another CPU the reader may still be between the flag and read(): let it block first. */
    struct timespec settle = {.tv_nsec = 50000000}; CHECK(nanosleep(&settle, NULL) == 0);
    CHECK(sched_yield() == 0 && close(pair[0]) == 0);
    int reused = open("/etc/native64.dat", O_RDONLY); CHECK(reused == pair[0]);
    CHECK(write(pair[1], "P", 1) == 1 && pthread_join(threads[0], NULL) == 0);
    CHECK(close(reused) == 0 && close(pair[1]) == 0);
    for (int i = 0; i < 2; i++) CHECK(pthread_create(&threads[i], NULL, bit_waiter, (void*)(uintptr_t)(1U << i)) == 0);
    while (atomic_load(&bit_waiters) != 2) CHECK(sched_yield() == 0);
    CHECK(syscall(SYS_futex, &futex_word, FUTEX_WAKE, 2, NULL, NULL, 0) == 0);
    for (int i = 1; i >= 0; i--) {
        uint64_t limit = clock_ns(CLOCK_MONOTONIC) + 2000000000;
        for (;;) {
            long woke = syscall(SYS_futex, &futex_word, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 1U << i);
            CHECK(woke == 0 || woke == 1); if (woke) break;
            CHECK(clock_ns(CLOCK_MONOTONIC) < limit && sched_yield() == 0);
        }
        void* result; CHECK(pthread_join(threads[i], &result) == 0 && result == (void*)(uintptr_t)(1U << i));
    }
    int file = open("/etc/native64.dat", O_RDONLY); CHECK(file >= 0);
    uint32_t* shared = mmap(NULL, 4096, PROT_READ, MAP_SHARED, file, 0); CHECK(shared != MAP_FAILED && close(file) == 0);
    uint32_t value = *shared; child = fork(); CHECK(child >= 0);
    if (!child) {
        file = open("/etc/native64.dat", O_RDONLY); CHECK(file >= 0);
        uint32_t* moved = mmap(NULL, 4096, PROT_READ, MAP_SHARED, file, 0); CHECK(moved != MAP_FAILED && moved != shared && close(file) == 0);
        CHECK(munmap(shared, 4096) == 0);
        struct timespec limit = {.tv_sec = 2};
        CHECK(syscall(SYS_futex, moved, FUTEX_WAIT, value, &limit, NULL, 0) == 0); _exit(0);
    }
    uint64_t until = clock_ns(CLOCK_MONOTONIC) + 2000000000;
    while (!syscall(SYS_futex, shared, FUTEX_WAKE, 1, NULL, NULL, 0)) CHECK(clock_ns(CLOCK_MONOTONIC) < until && sched_yield() == 0);
    CHECK(waitpid(child, &status, 0) == child && status == 0 && munmap(shared, 4096) == 0);
    puts("[64] THREAD fork from worker, pinned descriptor reuse and selective/cross-process futex wake PASS");
    uint64_t start = clock_ns(CLOCK_MONOTONIC);
    struct timespec timeout = {.tv_nsec = 50000000};
    CHECK(syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 0, &timeout, NULL, 0) == -1 && errno == ETIMEDOUT);
    CHECK(clock_ns(CLOCK_MONOTONIC) - start >= 50000000);
    uint64_t deadline = clock_ns(CLOCK_REALTIME) + 50000000;
    timeout = (struct timespec){deadline / 1000000000, deadline % 1000000000};
    CHECK(syscall(SYS_futex, &futex_word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME,
                  0, &timeout, NULL, FUTEX_BITSET_MATCH_ANY) == -1 && errno == ETIMEDOUT);
    puts("[64] THREAD ALL PASSED: real pthreads, TLS/FPU, mutex/condvar, join/destructors, signals, shared FDs and futex timeouts");
    return 0;
}
