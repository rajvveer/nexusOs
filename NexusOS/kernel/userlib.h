/* ============================================================================
 * NexusOS — User Library Header — Phase 20
 * ============================================================================
 * Defines the user-mode API contract. These are inline syscall wrappers
 * that user-mode ELF binaries can use to interact with the kernel.
 * ============================================================================ */

#ifndef USERLIB_H
#define USERLIB_H

#include "types.h"

/* Syscall numbers (must match syscall.c) */
#define SYS_WRITE    0
#define SYS_READ     1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_CREATE   4
#define SYS_EXIT     5
#define SYS_GETPID   6
#define SYS_YIELD    7
#define SYS_MALLOC   8
#define SYS_FREE     9
#define SYS_EXEC     10

/* Inline syscall: int 0x80 with eax=num, ebx=arg1, ecx=arg2, edx=arg3 */
static inline int32_t syscall0(int num) {
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num));
    return ret;
}

static inline int32_t syscall1(int num, uint32_t a1) {
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1));
    return ret;
}

static inline int32_t syscall2(int num, uint32_t a1, uint32_t a2) {
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2));
    return ret;
}

static inline int32_t syscall3(int num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3));
    return ret;
}

/* User API wrappers */
static inline int32_t user_write(const char* buf, uint32_t len) {
    return syscall2(SYS_WRITE, (uint32_t)buf, len);
}

static inline int32_t user_read(char* buf, uint32_t len) {
    return syscall2(SYS_READ, (uint32_t)buf, len);
}

static inline void user_exit(void) {
    syscall0(SYS_EXIT);
}

static inline int32_t user_getpid(void) {
    return syscall0(SYS_GETPID);
}

static inline void user_yield(void) {
    syscall0(SYS_YIELD);
}

static inline void* user_malloc(uint32_t size) {
    return (void*)syscall1(SYS_MALLOC, size);
}

static inline void user_free(void* ptr) {
    syscall1(SYS_FREE, (uint32_t)ptr);
}

#endif /* USERLIB_H */
