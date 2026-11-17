/* Concurrent page-fault stress: zero-fill and copy-on-write faults from several threads and processes
 * at once, racing mmap/munmap/madvise in the same address space. Exit status 0 = every byte checked. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096UL
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "VMSTRESS FAIL: %s (line %d)\n", msg, __LINE__); _exit(1); } } while (0)
enum { FILL_PAGES = 8192, COW_PAGES = 2048, THREADS = 4, CHILDREN = 3, ROUNDS = 3 };

static uint64_t pattern(size_t page, unsigned word) { return (page * 2654435761ULL) ^ ((uint64_t)word << 40) ^ 0x5A5A0000ULL; }
static uint64_t marked(unsigned owner, unsigned thread, size_t page) { return 0xC0DE000000000000ULL | (uint64_t)owner << 40 | (uint64_t)thread << 32 | page; }

struct job { uint8_t* base; size_t pages; unsigned owner, thread, stride; };
static volatile int churn_stop;

/* Interleaved first touches: neighbouring pages (same page tables) fault on different CPUs. */
static void* fill(void* argument) {
    struct job* j = argument;
    for (size_t p = j->thread; p < j->pages; p += j->stride) {
        uint64_t* words = (uint64_t*)(j->base + p * PAGE);
        CHECK(words[0] == 0 && words[511] == 0, "fresh anonymous page is zero");
        words[0] = marked(j->owner, j->thread, p); words[511] = ~words[0];
    }
    return NULL;
}
/* Copy-on-write breaks on this thread's pages while every other page must keep the parent's data. */
static void* cow(void* argument) {
    struct job* j = argument;
    for (size_t p = j->thread; p < j->pages; p += j->stride) {
        uint64_t* words = (uint64_t*)(j->base + p * PAGE);
        CHECK(words[0] == pattern(p, 0) && words[300] == pattern(p, 300), "inherited page before write");
        words[300] = marked(j->owner, j->thread, p);
        CHECK(words[0] == pattern(p, 0) && words[300] == marked(j->owner, j->thread, p), "private copy keeps old bytes");
    }
    return NULL;
}
/* Same address space, locked paths: map/touch/unmap and discard while the other threads fault. */
static void* churn(void* argument) {
    (void)argument;
    uint8_t* scratch = mmap(NULL, 64 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(scratch != MAP_FAILED, "scratch mapping");
    for (unsigned i = 0; !churn_stop; i++) {
        uint8_t* m = mmap(NULL, 32 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(m != MAP_FAILED, "churn mapping");
        for (unsigned p = 0; p < 32; p++) m[p * PAGE + (i & 255)] = (uint8_t)(p + i);
        for (unsigned p = 0; p < 32; p++) CHECK(m[p * PAGE + (i & 255)] == (uint8_t)(p + i), "churn bytes");
        CHECK(munmap(m, 32 * PAGE) == 0, "churn unmap");
        memset(scratch, 0x77, 64 * PAGE);
        CHECK(madvise(scratch, 64 * PAGE, MADV_DONTNEED) == 0, "discard");
        for (unsigned p = 0; p < 64; p++) CHECK(scratch[p * PAGE + 17] == 0, "discarded page reads zero");
    }
    CHECK(munmap(scratch, 64 * PAGE) == 0, "scratch unmap");
    return NULL;
}
static void run_threads(void* (*body)(void*), uint8_t* base, size_t pages, unsigned owner, bool with_churn) {
    pthread_t threads[THREADS], churner; struct job jobs[THREADS];
    churn_stop = 0;
    if (with_churn) CHECK(pthread_create(&churner, NULL, churn, NULL) == 0, "churn thread");
    for (unsigned t = 0; t < THREADS; t++) {
        jobs[t] = (struct job){base, pages, owner, t, THREADS};
        CHECK(pthread_create(&threads[t], NULL, body, &jobs[t]) == 0, "worker thread");
    }
    for (unsigned t = 0; t < THREADS; t++) CHECK(pthread_join(threads[t], NULL) == 0, "join worker");
    churn_stop = 1;
    if (with_churn) CHECK(pthread_join(churner, NULL) == 0, "join churn");
}
static void verify_cow(uint8_t* base, unsigned owner) {
    for (size_t p = 0; p < COW_PAGES; p++) {
        uint64_t* words = (uint64_t*)(base + p * PAGE);
        CHECK(words[0] == pattern(p, 0), "untouched word");
        CHECK(words[300] == marked(owner, (unsigned)(p % THREADS), p), "own copy-on-write result");
    }
}

int main(void) {
    for (unsigned round = 0; round < ROUNDS; round++) {
        /* 1. zero-fill faults from four threads in one space, with map/unmap/discard racing them */
        uint8_t* fresh = mmap(NULL, FILL_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(fresh != MAP_FAILED, "fill mapping");
        run_threads(fill, fresh, FILL_PAGES, 0, true);
        for (size_t p = 0; p < FILL_PAGES; p++) {
            uint64_t* words = (uint64_t*)(fresh + p * PAGE);
            CHECK(words[0] == marked(0, (unsigned)(p % THREADS), p) && words[511] == ~words[0], "zero-fill result");
        }
        CHECK(munmap(fresh, FILL_PAGES * PAGE) == 0, "fill unmap");

        /* 2. copy-on-write faults in the parent and three children at the same time */
        uint8_t* inherited = mmap(NULL, COW_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(inherited != MAP_FAILED, "cow mapping");
        for (size_t p = 0; p < COW_PAGES; p++) {
            uint64_t* words = (uint64_t*)(inherited + p * PAGE);
            for (unsigned w = 0; w < 512; w++) words[w] = pattern(p, w);
        }
        int go[2]; CHECK(pipe(go) == 0, "start pipe");
        pid_t children[CHILDREN];
        for (unsigned c = 0; c < CHILDREN; c++) {
            children[c] = fork(); CHECK(children[c] >= 0, "fork");
            if (!children[c]) {
                char token; close(go[1]);
                CHECK(read(go[0], &token, 1) == 0, "start signal"); /* EOF: everyone forked */
                run_threads(cow, inherited, COW_PAGES, c + 1, c == 0);
                verify_cow(inherited, c + 1);
                _exit(0);
            }
        }
        close(go[0]); close(go[1]);
        run_threads(cow, inherited, COW_PAGES, 0, true);
        verify_cow(inherited, 0);
        for (unsigned c = 0; c < CHILDREN; c++) {
            int status = -1;
            CHECK(waitpid(children[c], &status, 0) == children[c], "wait child");
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child verified its copies");
        }
        verify_cow(inherited, 0); /* the children's writes never reached the parent */
        CHECK(munmap(inherited, COW_PAGES * PAGE) == 0, "cow unmap");
    }
    printf("VMSTRESS PASSED: concurrent zero-fill, copy-on-write across processes, racing map/unmap/discard\n");
    return 0;
}
