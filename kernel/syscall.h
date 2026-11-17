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

/* ---- Phase 57 apps: POSIX-ish userland ABI, numbers 24..54 ----
 * Defined (with their argument structs, flags and errno values) in
 * usys_abi.h, which is shared verbatim with userland/libc. Implemented in
 * usys.c and reached through the same int 0x80 gate: eax = number,
 * ebx/ecx/edx = args, eax = result or -errno.
 *   24 lseek      25 fstat      26 stat       27 unlink     28 rename
 *   29 mkdir      30 rmdir      31 getdents   32 chdir      33 getcwd
 *   34 dup        35 dup2       36 spawn      37 waitpid    38 clock_gettime
 *   39 nanosleep  40 isatty     41 ioctl      42 uname      43 access
 *   44 ftruncate  45 pipe2      46 open(fd)   47 read(fd)   48 write(fd)
 *   49 close(fd)  50 mmap_anon  51 munmap     52 fcntl      53 ttyname
 *   54 getppid
 * File descriptors are per process (0/1/2 = the launching terminal) and are
 * inherited by spawn. FPU/SSE state is saved/restored per process. */

/* Initialize the syscall interface */
void syscall_init(void);

#endif /* SYSCALL_H */
