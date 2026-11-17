/* ============================================================================
 * NexusOS — User Processes: isolation & multitasking — Phase 54
 * ============================================================================
 * Real per-process address spaces and pre-emptive multitasking for ring-3
 * user programs, layered on the existing GDT (DPL3 code/data + TSS), paging
 * (per-process page directories, see paging.h as_*), the high user-frame pool
 * (memory.h upmm_*) and the process table (process.h).
 *
 * SCHEDULING MODEL (keeps the lock-free kernel safe):
 *   - Process 0 is the kernel thread (the shell / desktop loop). It runs in
 *     ring 0 and is NEVER pre-empted: the timer IRQ only switches tasks when
 *     it interrupted ring-3 (user) code, and then only between user processes.
 *   - The kernel thread yields the CPU only at explicit safe points by calling
 *     sched_yield() (from the text shell's idle/key-wait hook, and — once the
 *     lead wires it — the desktop loop).
 *   - Syscalls run on the faulting process's own kernel stack and are never
 *     pre-empted (they execute in ring 0, which the timer guard excludes).
 *   - A user process gives up the CPU voluntarily on exit / sleep / yield, and
 *     involuntarily when its quantum expires while running in ring 3.
 * ============================================================================ */

#ifndef UPROC_H
#define UPROC_H

#include "types.h"
#include "idt.h"
#include "process.h"

/* Per-process user virtual layout (all inside [USER_SPACE_START, END), paging.h) */
#define UPROC_STACK_TOP   0x0C000000u   /* 192MB — user stack top (grows down) */
#define UPROC_STACK_PAGES 4             /* 16KB user stack                      */
#define UPROC_STUB_BASE   0x0B000000u   /* 176MB — per-process Win32 IAT stubs  */
#define UPROC_HEAP_BASE   0x10000000u   /* 256MB — brk/sbrk heap (grows up)     */
#define UPROC_HEAP_MAX    0x04000000u   /* 64MB  — heap cap                     */

/* Install the built-in demo binaries into the RAM filesystem, register the
 * current process as the kernel thread (process 0), and enable scheduling.
 * Call once, after the shell PCB is created and set current. */
void uproc_init(void);

/* Load and start the user program at VFS path `path`. If background is false
 * the caller (the kernel thread) waits — yielding — until it exits and returns
 * its exit code; otherwise it returns the new pid immediately. -1 on error. */
int  uproc_run_path(const char* path, bool background);

/* Kill a user process by pid, honouring ownership (root or owner only).
 * Returns 0 on success, -1 if not found, -2 if denied / not a user process. */
int  uproc_kill(uint32_t pid, uint32_t caller_uid);

/* Free the address space + kernel stack of every TERMINATED user process.
 * Safe to call from the kernel thread's idle loop (never reaps `current`). */
void uproc_reap(void);

/* Exception-handler hook: decide whether a CPU fault should kill a user
 * process (fault taken in ring 3, or a bad user pointer dereferenced by the
 * kernel while servicing that process's syscall) rather than halt the machine.
 * Returns true and switches to the scheduler (does not return) when it kills;
 * returns false for a genuine kernel fault so the caller prints !EX and halts. */
bool uproc_fault(struct registers* regs, uint32_t fault_addr);

/* ---- Scheduler core (wrapped by scheduler.c's schedule()/yield()) ---- */
void sched_yield(void);                     /* voluntary switch (cooperative) */
void sched_set_enabled(int on);
void uproc_timer_tick(struct registers* regs); /* involuntary (timer IRQ)     */

/* ---- Syscall back-ends (called from syscall.c) ---- */
int  uproc_sys_exit(uint32_t code);         /* never returns for a user proc  */
int  uproc_sys_sleep(uint32_t ms);
int  uproc_sys_sbrk(int32_t increment);     /* returns old break, or -1       */

/* >0 while the kernel is executing a syscall on behalf of a user process; read
 * by uproc_fault() to turn a bad user pointer into a clean process kill. */
extern volatile int uproc_in_syscall;

/* ---- Phase 57 apps: spawn a static i386 ELF from any byte source ----
 * rd(ctx, off, len, dst) must return len on success (dst is a user address in
 * the NEW process's space, mapped and current when rd is called). argv/envp
 * strings must be in kernel memory. Returns the READY process or NULL. */
typedef int (*uproc_read_fn)(void* ctx, uint32_t off, uint32_t len, void* dst);
process_t* uproc_spawn_from(const char* name, uint32_t fsize, uproc_read_fn rd, void* ctx,
                            int argc, char* const* argv, int envc, char* const* envp);

#endif /* UPROC_H */
