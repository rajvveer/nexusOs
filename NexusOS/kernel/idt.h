/* ============================================================================
 * NexusOS — IDT (Interrupt Descriptor Table) Header
 * ============================================================================ */

#ifndef IDT_H
#define IDT_H

#include "types.h"

/* IDT Entry (8 bytes) — describes one interrupt handler */
struct idt_entry {
    uint16_t base_low;       /* Handler address bits 0-15 */
    uint16_t selector;       /* Kernel code segment selector */
    uint8_t  always0;        /* Always 0 */
    uint8_t  flags;          /* Type and attributes */
    uint16_t base_high;      /* Handler address bits 16-31 */
} __attribute__((packed));

/* IDT Pointer — passed to lidt instruction */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Registers pushed by ISR/IRQ stubs */
struct registers {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};

typedef void (*isr_handler_t)(struct registers*);

void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);

extern void idt_load(uint32_t idt_ptr);

#endif /* IDT_H */
