/* ============================================================================
 * NexusOS — Scheduler Header
 * ============================================================================ */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"
#include "idt.h"

/* Initialize the scheduler */
void scheduler_init(void);

/* Schedule next process (called from timer IRQ) */
void schedule(struct registers* regs);

/* Voluntarily yield CPU */
void yield(void);

/* Start scheduling (call after shell is set up) */
void scheduler_start(void);

/* External ASM function */
extern void context_switch(uint32_t* old_esp, uint32_t new_esp);

#endif /* SCHEDULER_H */
