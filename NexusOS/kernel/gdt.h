/* ============================================================================
 * NexusOS — GDT (Global Descriptor Table) Header
 * ============================================================================
 * The GDT tells the CPU about memory segments — what code can access what
 * memory. We set up a flat memory model (full 4GB access for kernel).
 * ============================================================================ */

#ifndef GDT_H
#define GDT_H

#include "types.h"

/* GDT Entry structure (8 bytes each) */
struct gdt_entry {
    uint16_t limit_low;      /* Limit bits 0-15 */
    uint16_t base_low;       /* Base bits 0-15 */
    uint8_t  base_middle;    /* Base bits 16-23 */
    uint8_t  access;         /* Access flags */
    uint8_t  granularity;    /* Granularity + limit bits 16-19 */
    uint8_t  base_high;      /* Base bits 24-31 */
} __attribute__((packed));

/* GDT Pointer — passed to lgdt instruction */
struct gdt_ptr {
    uint16_t limit;          /* Size of GDT - 1 */
    uint32_t base;           /* Address of first GDT entry */
} __attribute__((packed));

/* Initialize the GDT */
void gdt_init(void);

/* ASM function to flush the GDT (defined in gdt_asm.asm) */
extern void gdt_flush(uint32_t gdt_ptr);

/* Set kernel stack in TSS (for ring 3 → ring 0 transitions) */
void tss_set_kernel_stack(uint32_t stack);

#endif /* GDT_H */
