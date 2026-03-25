/* ============================================================================
 * NexusOS — Round-Robin Scheduler (Implementation)
 * ============================================================================
 * Simple cooperative/preemptive scheduler. Hooks into the timer IRQ (IRQ0)
 * to perform context switches between processes.
 * ============================================================================ */

#include "scheduler.h"
#include "process.h"
#include "vga.h"
#include "string.h"

/* External: set current process */
extern void process_set_current(process_t* proc);

/* Scheduling state */
static int  scheduler_enabled = 0;
static int  current_slot = -1;
static uint32_t schedule_ticks = 0;

#define TIMESLICE 5  /* Switch every 5 timer ticks */

/* --------------------------------------------------------------------------
 * scheduler_init: Initialize the scheduler
 * -------------------------------------------------------------------------- */
void scheduler_init(void) {
    scheduler_enabled = 0;
    current_slot = -1;
    schedule_ticks = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Scheduler initialized (round-robin, timeslice=5)\n");
}

/* --------------------------------------------------------------------------
 * find_next_ready: Find the next READY process after current_slot
 * -------------------------------------------------------------------------- */
static int find_next_ready(void) {
    process_t* table = process_get_table();
    int start = (current_slot + 1) % MAX_PROCESSES;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        int idx = (start + i) % MAX_PROCESSES;
        if (table[idx].state == PROC_READY) {
            return idx;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * schedule: Called from timer IRQ handler
 * Picks the next READY process and context-switches to it
 * -------------------------------------------------------------------------- */
void schedule(struct registers* regs) {
    (void)regs;

    if (!scheduler_enabled) return;

    schedule_ticks++;
    if (schedule_ticks < TIMESLICE) return;
    schedule_ticks = 0;

    process_t* table = process_get_table();
    process_t* current = process_get_current();

    /* Find next ready process */
    int next_slot = find_next_ready();
    if (next_slot < 0) return;  /* No other process to switch to */

    /* If it's the same process, skip */
    if (next_slot == current_slot) return;

    /* Save current process state */
    if (current != NULL && current->state == PROC_RUNNING) {
        current->state = PROC_READY;
    }

    /* Switch to new process */
    int old_slot = current_slot;
    current_slot = next_slot;
    process_t* next = &table[next_slot];
    next->state = PROC_RUNNING;
    next->ticks++;
    process_set_current(next);

    /* Perform context switch */
    if (old_slot >= 0 && table[old_slot].state == PROC_READY) {
        context_switch(&table[old_slot].esp, next->esp);
    } else {
        /* First switch or old process terminated — just load new context */
        context_switch(&table[old_slot >= 0 ? old_slot : 0].esp, next->esp);
    }
}

/* --------------------------------------------------------------------------
 * scheduler_start: Enable scheduling (called after shell is set up as PID 1)
 * -------------------------------------------------------------------------- */
void scheduler_start(void) {
    process_t* current = process_get_current();
    if (current != NULL) {
        process_t* table = process_get_table();
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (&table[i] == current) {
                current_slot = i;
                break;
            }
        }
    }
    scheduler_enabled = 1;
}

/* --------------------------------------------------------------------------
 * yield: Voluntarily give up the CPU
 * -------------------------------------------------------------------------- */
void yield(void) {
    __asm__ volatile("int $0x20");  /* Trigger timer interrupt */
}
