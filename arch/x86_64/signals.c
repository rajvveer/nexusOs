#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

static volatile sig_atomic_t first_count, second_count, realtime_count, ill_count, active, bad;
static unsigned char* alternate;
static int attack;
static void second(int number, siginfo_t* info, void* context) {
    (void)info; (void)context;
    if (number != SIGUSR2 || active) bad = 1;
    second_count++;
}
static void first(int number, siginfo_t* info, void* context) {
    int local;
    if (number != SIGUSR1 || info->si_pid != getpid() || info->si_uid != getuid()) bad = 2;
    if ((uintptr_t)&local < (uintptr_t)alternate || (uintptr_t)&local >= (uintptr_t)alternate + 65536) bad = 3;
    ucontext_t* saved = context;
    if (attack == 1) { saved->uc_mcontext.gregs[REG_CSGSFS] = (saved->uc_mcontext.gregs[REG_CSGSFS] & ~0xFFFFUL) | 8; return; }
    if (attack == 2) { saved->uc_mcontext.fpregs->mxcsr |= 0x80000000U; return; }
    sigset_t mask;
    if (sigprocmask(SIG_SETMASK, NULL, &mask) || !sigismember(&mask, SIGUSR1) || !sigismember(&mask, SIGUSR2)) bad = 4;
    stack_t disabled = {.ss_flags = SS_DISABLE};
    if (sigaltstack(&disabled, NULL) != -1 || errno != EPERM) bad = 5;
    int previous = second_count; active = 1;
    if (raise(SIGUSR2) || second_count != previous) bad = 6;
    active = 0; first_count++;
}
static void realtime(int number, siginfo_t* info, void* context) {
    (void)info; (void)context;
    if (number != SIGRTMIN) bad = 7;
    realtime_count++;
}
static void illegal(int number, siginfo_t* info, void* context) {
    ucontext_t* saved = context;
    if (number != SIGILL || info->si_addr != (void*)saved->uc_mcontext.gregs[REG_RIP]) bad = 8;
    saved->uc_mcontext.gregs[REG_RIP] += 2;
    saved->uc_mcontext.gregs[REG_RAX] = 0x44556677;
    __asm__ volatile("pxor %%xmm7,%%xmm7" ::: "xmm7");
    ill_count++;
}
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    attack = argv[1][0] == 'c' ? 1 : argv[1][0] == 'f' ? 2 : 0;
    alternate = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (alternate == MAP_FAILED) return 2;
    stack_t stack = {.ss_sp = alternate, .ss_size = 65536};
    if (sigaltstack(&stack, NULL)) return 3;
    struct sigaction action = {.sa_sigaction = first, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask); sigaddset(&action.sa_mask, SIGUSR2);
    if (sigaction(SIGUSR1, &action, NULL)) return 4;
    action.sa_sigaction = second; action.sa_flags = SA_SIGINFO; sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, NULL)) return 5;
    if (raise(SIGUSR1)) return 6;
    if (attack) return 7;
    if (bad || first_count != 1 || second_count != 1) return 8;
    if (sigaltstack(NULL, &stack) || (stack.ss_flags & SS_ONSTACK)) return 9;
    sigset_t mask, pending;
    sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) || raise(SIGUSR1) || raise(SIGUSR1) || first_count != 1) return 10;
    if (sigpending(&pending) || !sigismember(&pending, SIGUSR1)) return 11;
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) || first_count != 2 || second_count != 2 || bad) return 12;
    action.sa_sigaction = realtime;
    if (sigaction(SIGRTMIN, &action, NULL)) return 13;
    sigemptyset(&mask); sigaddset(&mask, SIGRTMIN);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) || raise(SIGRTMIN) || raise(SIGRTMIN) || realtime_count) return 14;
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) || realtime_count != 2 || bad) return 15;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || raise(SIGPIPE)) return 16;
    if (sigaction(SIGKILL, &action, NULL) != -1 || errno != EINVAL) return 17;
    action.sa_sigaction = illegal;
    if (sigaction(SIGILL, &action, NULL)) return 18;
    uint64_t pattern[2] = {0x123456789ABCDEF0, 0xFEDCBA9876543210}, output[2], result;
    __asm__ volatile("movdqu %2,%%xmm7; mov $0x1234,%%rax; ud2; mov %%rax,%0; movdqu %%xmm7,%1"
                     : "=r"(result), "=m"(output) : "m"(pattern) : "rax", "xmm7", "memory");
    if (ill_count != 1 || bad || result != 0x44556677 || memcmp(pattern, output, sizeof(pattern))) return 19;
    action.sa_sigaction = realtime; action.sa_flags = SA_SIGINFO | SA_RESETHAND | 0x8400;
    if (sigaction(SIGRTMIN, &action, NULL) || raise(SIGRTMIN) || realtime_count != 3 || bad) return 21;
    if (sigaction(SIGRTMIN, NULL, &action) || action.sa_handler != SIG_DFL || (action.sa_flags & 0x8400)) return 22;
    stack.ss_flags = SS_DISABLE;
    if (sigaltstack(&stack, NULL) || munmap(alternate, 65536)) return 20;
    puts("[64] SIGNAL ALL PASSED: delivery, mask/pending, altstack, RT queue, context and SSE restore");
    return 0;
}
