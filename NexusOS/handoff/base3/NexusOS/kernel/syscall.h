/* ============================================================================
 * NexusOS — Syscall Interface Header — Phase 22
 * ============================================================================ */

#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "idt.h"

/* Syscall numbers */
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
#define SYS_FORK     11
#define SYS_SHMGET   12
#define SYS_SHMAT    13
#define SYS_MMAP     14
#define SYS_KILL     15
#define SYS_PIPE     16
#define SYS_GETENV   17
#define SYS_SETENV   18

/* Phase 32: Dynamic linking */
#define SYS_DLOPEN   19
#define SYS_DLSYM    20
#define SYS_DLCLOSE  21

/* Phase 54: user multitasking ABI additions.
 * Arg registers: eax = call number, ebx/ecx/edx = args; return in eax.
 *   SYS_SLEEP(ebx = milliseconds)         -> blocks, yielding, then 0
 *   SYS_SBRK (ebx = signed increment)     -> old program break, or -1
 * Win32 shim calls do not use a syscall number: a per-process IAT stub issues
 * `mov eax, 0x8000|<shim id>; int 0x80` and the handler routes eax>=0x8000 to
 * win32_dispatch() (see uproc.c / win32.c). */
#define SYS_SLEEP    22
#define SYS_SBRK     23

#define NUM_SYSCALLS 24

/* Initialize the syscall interface */
void syscall_init(void);

#endif /* SYSCALL_H */
