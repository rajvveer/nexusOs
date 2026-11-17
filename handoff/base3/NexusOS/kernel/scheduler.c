/* ============================================================================
 * NexusOS — Scheduler (glue) — Phase 54
 * ============================================================================
 * The scheduling core (pick/switch/yield, address-space-aware) lives in
 * uproc.c so it sits next to the process lifecycle it drives. This file keeps
 * the historical entry points the rest of the kernel calls:
 *   - schedule(regs): invoked from the timer IRQ (kernel.c timer_callback).
 *   - yield():        the old voluntary-yield name (used by sys_yield).
 *   - scheduler_start(): enable scheduling.
 *
 * Model (see uproc.h): the kernel thread (process 0) is never pre-empted; the
 * timer only switches between user processes running in ring 3.
 * ============================================================================ */

#include "scheduler.h"
#include "uproc.h"
#include "vga.h"

void scheduler_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Scheduler initialized (non-preemptible kernel, ring-3 preempt)\n");
}

void schedule(struct registers* regs) {
    uproc_timer_tick(regs);
}

void yield(void) {
    sched_yield();
}

void scheduler_start(void) {
    sched_set_enabled(1);
}
