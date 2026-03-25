/* ============================================================================
 * NexusOS — GDT (Implementation) — Phase 20
 * ============================================================================
 * Flat memory model with kernel (ring 0) and user (ring 3) segments + TSS.
 * ============================================================================ */

#include "gdt.h"
#include "vga.h"
#include "string.h"

/* 6 GDT entries: null, kernel code, kernel data, user code, user data, TSS */
#define GDT_ENTRIES 6

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;

/* Task State Segment */
typedef struct __attribute__((packed)) {
    uint32_t prev_tss;
    uint32_t esp0;      /* Kernel stack pointer (ring 0) */
    uint32_t ss0;       /* Kernel stack segment */
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_entry_t;

static tss_entry_t tss;

/* --------------------------------------------------------------------------
 * gdt_set_gate: Configure a single GDT entry
 * -------------------------------------------------------------------------- */
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity  = ((limit >> 16) & 0x0F) | (gran & 0xF0);

    gdt[num].access      = access;
}

/* --------------------------------------------------------------------------
 * write_tss: Set up the TSS descriptor in the GDT
 * -------------------------------------------------------------------------- */
static void write_tss(int num, uint32_t ss0, uint32_t esp0) {
    uint32_t base = (uint32_t)&tss;
    uint32_t limit = sizeof(tss_entry_t) - 1;

    memset(&tss, 0, sizeof(tss));
    tss.ss0 = ss0;
    tss.esp0 = esp0;

    /* I/O map base = size of TSS (no I/O bitmap) */
    tss.iomap_base = sizeof(tss_entry_t);

    /* GDT entry for TSS: present, DPL=0, type=0x89 (32-bit TSS, not busy) */
    gdt_set_gate(num, base, limit, 0x89, 0x00);
}

/* --------------------------------------------------------------------------
 * gdt_init: Set up the GDT with kernel + user segments + TSS
 * -------------------------------------------------------------------------- */
void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base  = (uint32_t)&gdt;

    /* Entry 0: Null descriptor (required by CPU) */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Entry 1: Kernel Code Segment (0x08)
     * Base=0, Limit=4GB, Access: present, ring 0, executable, readable
     * Gran: 4KB pages, 32-bit */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* Entry 2: Kernel Data Segment (0x10)
     * Base=0, Limit=4GB, Access: present, ring 0, data, writable */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* Entry 3: User Code Segment (0x18, selector 0x1B with RPL=3)
     * Access: present, ring 3, executable, readable */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /* Entry 4: User Data Segment (0x20, selector 0x23 with RPL=3)
     * Access: present, ring 3, data, writable */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* Entry 5: TSS (0x28)
     * Kernel stack segment = 0x10 (kernel data), ESP0 = temporary */
    write_tss(5, 0x10, 0);

    /* Flush old GDT and install new one */
    gdt_flush((uint32_t)&gp);

    /* Load TSS */
    __asm__ volatile("mov $0x2B, %%ax; ltr %%ax" ::: "ax");

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("GDT initialized (ring 0+3, TSS)\n");
}

/* --------------------------------------------------------------------------
 * tss_set_kernel_stack: Update ESP0 in TSS for ring transitions
 * Called on context switch to set the kernel stack for the current process.
 * -------------------------------------------------------------------------- */
void tss_set_kernel_stack(uint32_t stack) {
    tss.esp0 = stack;
}
