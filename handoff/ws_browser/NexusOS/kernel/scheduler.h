/* ============================================================================
 * NexusOS — Scheduler + user-process lifecycle (Header) — Phase 54
 * ============================================================================
 * Cooperative for the kernel/desktop thread; preemptive (timer-sliced) ONLY
 * among ring-3 user processes. See scheduler.c for the full model.
 * ============================================================================ */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"
#include "idt.h"

/* Initialize the scheduler. */
void scheduler_init(void);

/* Timer-IRQ hook (kernel.c timer_callback). Preempts the current task only when
 * it was interrupted in ring 3 (a user process); never touches kernel code. */
void schedule(struct registers* regs);

/* Legacy no-op kept for callers; scheduling engages when a user proc launches. */
void scheduler_start(void);

/* Voluntarily give up the CPU (SYS_YIELD). Round-robins among ring-3 procs. */
void yield(void);

/* ----------------------------------------------------------------------------
 * User process lifecycle (Phase 54). A user process has its own page directory
 * (as_create) and an IRQ-safe kernel stack; ELF/Mach-O run it in ring 3.
 * -------------------------------------------------------------------------- */

/* Allocate a user process slot with its own address space + kernel stack.
 * ring3=true => ring-3 (scheduled/preemptible); ring3=false => ring-0 image
 * (run inline by the loader, e.g. the PE/Win32 path). Returns NULL on failure.*/
process_t* uproc_create(const char* name, bool ring3);

/* Build the initial ring-3 entry frame on proc's kernel stack. */
void uproc_set_ring3_entry(process_t* proc, uint32_t entry, uint32_t user_esp);

/* Fully free a user process (address space, kernel stack, slot). */
void uproc_free(process_t* proc);

/* Foreground launch: run `proc` (already READY) and any peers to completion,
 * blocking the caller (the shell). Returns proc's exit code. */
uint32_t uproc_run(process_t* proc);

/* Cooperative background pass for the shell idle hook: give ready user procs a
 * slice, then return so the shell stays responsive. */
void uproc_background_tick(void);

/* A ring-3 process is exiting (SYS_EXIT). Does not return. */
void uproc_exit(uint32_t code);

/* Round-robin among ready ring-3 procs (SYS_YIELD body). */
void sched_yield_user(void);

/* Count live (READY/RUNNING) ring-3 user processes. */
int uproc_user_count(void);

/* Fault dispatch: if the current task is a user process, kill only it and
 * resume the scheduler (does not return); otherwise return false so the caller
 * panics. Called from the page-fault handler and the generic exception path. */
bool uproc_fault(struct registers* regs, uint32_t fault_addr);

/* External ASM context switch (switch.asm). */
extern void context_switch(uint32_t* old_esp, uint32_t new_esp);

#endif /* SCHEDULER_H */
