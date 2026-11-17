/* ============================================================================
 * NexusOS — kguard: in-kernel crash containment — Phase 55
 * ============================================================================
 * The kernel, the Aurora desktop and every built-in app run as ONE ring-0
 * thread. Before Phase 55 any fault in that thread (NULL deref, bad pointer,
 * divide by zero, write into kernel text) halted the machine with a red !EX:
 * bar. kguard adds setjmp/longjmp-style recovery contexts:
 *
 *     kguard_ctx_t g;
 *     int rc = kguard_try(&g, "shell command");   -- 0 = armed
 *     if (rc == 0) { risky_code(); kguard_end(); }
 *     else         { -- a fault was recovered; rc = 1 + exception number }
 *
 * When a CPU exception hits ring-0 code of the kernel thread while a guard is
 * armed (and we are not inside an IRQ handler or a user syscall), the fault
 * handlers call kguard_recover(): it logs one "!KR:" serial line, resets the
 * global in-progress state that would otherwise stay stuck, pops the guard and
 * long-jumps back into kguard_try() with the fault code. The faulting frames
 * are simply abandoned (the kernel has no locks).
 *
 * A double fault caused by a kernel-stack overflow into the guard pages below
 * the boot stack is handled the same way, through a hardware task gate (the
 * CPU cannot push an exception frame on an unmapped stack, so #DF switches to
 * its own TSS + stack; see kguard_init).
 *
 * What is NOT contained (still fatal, red bar): faults inside IRQ handlers,
 * faults while a user syscall is executing kernel code on behalf of a ring-3
 * process that uproc_fault() does not blame on that process, faults with no
 * guard armed (boot, login), and double faults while the kernel thread is not
 * current. Ring-3 user processes are isolated separately (uproc_fault).
 * ============================================================================ */

#ifndef KGUARD_H
#define KGUARD_H

#include "types.h"
#include "idt.h"

/* Callee-saved register snapshot taken by kguard_try (setjmp-like). */
typedef struct {
    uint32_t ebx, esi, edi, ebp, esp, eip;
} kguard_ctx_t;

/* Arm a recovery context. Returns 0 when armed; after a recovered fault it
 * returns a second time with the fault code (1 + exception number). Must be
 * paired with kguard_end() on the normal path. Implemented in kguard_asm.asm. */
int  kguard_try(kguard_ctx_t* ctx, const char* what) __attribute__((returns_twice));

/* Disarm the innermost guard (pop). */
void kguard_end(void);

/* True if at least one guard is armed. */
bool kguard_active(void);

/* Install the double-fault task gate (needs GDT/IDT/paging up). */
void kguard_init(void);

/* Fault handlers: should this ring-0 fault be recovered into a guard? */
bool kguard_should_recover(struct registers* regs);

/* Recover from a fault: never returns (long-jumps into kguard_try). Only call
 * when kguard_should_recover() returned true. addr = CR2 for page faults. */
void kguard_recover(struct registers* regs, int int_no, uint32_t addr) __attribute__((noreturn));

/* IRQ nesting depth of the kernel thread (idt.c irq_handler maintains it). */
void kguard_irq_enter(void);
void kguard_irq_leave(void);

/* Details of the last recovered fault, for the caller's diagnostics. */
int         kguard_last_int(void);
uint32_t    kguard_last_eip(void);
uint32_t    kguard_last_addr(void);
uint32_t    kguard_recovered_count(void);
const char* kguard_exc_name(int code);        /* code = 1 + int_no, e.g. "Page fault" */
const char* kguard_exc_short(int code);       /* "#PF", "#GP", "#DE", "#UD", "#DF" ... */

/* "Page fault at 0x0010A2B4 (addr 0x00000000)" into buf (n >= 48 recommended). */
void kguard_describe(int code, char* buf, int n);

#endif /* KGUARD_H */
