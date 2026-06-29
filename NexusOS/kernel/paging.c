/* ============================================================================
 * NexusOS — Paging (Implementation) — Phase 21
 * ============================================================================
 * Sets up x86 paging with page directory and page tables.
 * Phase 21: Page fault handler (ISR 14), COW support, accessor functions.
 * ============================================================================ */

#include "paging.h"
#include "memory.h"
#include "string.h"
#include "vga.h"
#include "idt.h"
#include "vmm.h"

/* Page directory: 1024 entries, each points to a page table */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

/* Page tables for the identity map. Each table maps 4MB, so IDENTITY_TABLES
 * tables cover IDENTITY_TABLES*4MB. Phase 53: raised from 4 (16MB) to 8 (32MB)
 * so the relocated high VESA back buffer (1920x1080x4 ~= 8MB at 0x01000000)
 * lands inside identity-mapped RAM. See framebuffer.c FB_BACKBUF_ADDR. */
#define IDENTITY_TABLES 8
static uint32_t page_tables[IDENTITY_TABLES][1024] __attribute__((aligned(4096)));

/* Extra page tables for VESA framebuffer mapping. Each maps 4MB. Phase 53: a
 * 1920x1080x32 LFB is ~8MB, so one 4MB table is not enough — provide 4 (16MB)
 * and map as many as the framebuffer actually spans. */
#define VESA_FB_TABLES 4
static uint32_t vesa_page_tables[VESA_FB_TABLES][1024] __attribute__((aligned(4096)));

/* --------------------------------------------------------------------------
 * Page fault handler (ISR 14)
 * Error code bits: [0]=present, [1]=write, [2]=user
 * CR2 contains the faulting virtual address.
 * -------------------------------------------------------------------------- */
static void page_fault_handler(struct registers* regs) {
    uint32_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    uint32_t err = regs->err_code;
    bool present = (err & 0x1) != 0;
    bool write   = (err & 0x2) != 0;
    bool user    = (err & 0x4) != 0;

    /* COW: write fault on present page → try COW copy */
    if (present && write) {
        if (vmm_handle_cow(fault_addr)) {
            return; /* COW handled successfully */
        }
    }

    /* Demand paging: non-present user page in valid region → allocate */
    if (!present && user) {
        /* Check if address is in a valid user region (4MB–15MB) */
        if (fault_addr >= 0x00400000 && fault_addr < 0x00F00000) {
            uint32_t phys = pmm_alloc_page();
            if (phys != 0) {
                memset((void*)phys, 0, PAGE_SIZE);
                uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
                paging_map_page(fault_addr & 0xFFFFF000, phys, flags);
                return; /* Demand paging handled */
            }
        }
    }

    /* Unhandled page fault — print error and halt */
    vga_print_color("\n[PANIC] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("Page fault at 0x");
    char buf[12];
    int_to_str(fault_addr, buf);
    vga_print(buf);
    vga_print(present ? " (present" : " (not-present");
    vga_print(write ? ",write" : ",read");
    vga_print(user ? ",user)" : ",kernel)");
    vga_print("\n");

    /* Halt */
    __asm__ volatile("cli; hlt");
}

/* --------------------------------------------------------------------------
 * paging_init: Set up identity mapping and enable paging
 * -------------------------------------------------------------------------- */
void paging_init(void) {
    /* Clear page directory */
    memset(page_directory, 0, sizeof(page_directory));

    /* Identity map the first IDENTITY_TABLES*4MB (32MB) using one page table
     * per 4MB. This covers the kernel, heap, DMA buffers, and the relocated
     * high VESA back buffer. */
    for (int t = 0; t < IDENTITY_TABLES; t++) {
        for (int i = 0; i < 1024; i++) {
            uint32_t phys = (t * 1024 + i) * PAGE_SIZE;
            page_tables[t][i] = phys | PAGE_PRESENT | PAGE_WRITABLE;
        }
        page_directory[t] = ((uint32_t)page_tables[t]) | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* Load page directory into CR3 and enable paging in CR0 */
    __asm__ volatile(
        "mov %0, %%cr3\n"       /* Load page directory base */
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n" /* Set PG (paging) bit */
        "mov %%eax, %%cr0\n"
        :
        : "r"(page_directory)
        : "eax"
    );

    /* Register page fault handler (ISR 14) */
    register_interrupt_handler(14, page_fault_handler);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Paging enabled (32MB, page faults)\n");
}

/* --------------------------------------------------------------------------
 * paging_map_vesa_fb: Identity-map the VESA framebuffer (front buffer / LFB).
 * fb_size is the framebuffer byte length; we map enough 4MB page tables to
 * cover it (Phase 53: a 1920x1080x32 LFB is ~8MB and needs two tables — the
 * old single-table map faulted past the first 4MB and double-faulted at boot).
 * -------------------------------------------------------------------------- */
void paging_map_vesa_fb(uint32_t fb_phys, uint32_t fb_size) {
    uint32_t start_pd = fb_phys >> 22;                 /* first 4MB region   */
    uint32_t end_pd   = (fb_phys + fb_size - 1) >> 22; /* last 4MB region    */
    uint32_t span     = end_pd - start_pd + 1;
    if (span > VESA_FB_TABLES) span = VESA_FB_TABLES;  /* clamp to provision */

    for (uint32_t t = 0; t < span; t++) {
        uint32_t pd_index = start_pd + t;
        uint32_t base = pd_index << 22;
        for (int i = 0; i < 1024; i++) {
            vesa_page_tables[t][i] = (base + i * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITABLE;
        }
        page_directory[pd_index] = ((uint32_t)vesa_page_tables[t]) | PAGE_PRESENT | PAGE_WRITABLE;
    }

    __asm__ volatile("mov %0, %%cr3\n" : : "r"(page_directory) : "memory");
}

/* --------------------------------------------------------------------------
 * paging_map_page: Map a virtual page to a physical address
 * -------------------------------------------------------------------------- */
void paging_map_page(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_page();
        if (pt_phys == 0) return;
        memset((void*)pt_phys, 0, PAGE_SIZE);
        page_directory[pd_index] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE | flags;
    }

    uint32_t* page_table = (uint32_t*)(page_directory[pd_index] & 0xFFFFF000);
    page_table[pt_index] = physical_addr | PAGE_PRESENT | flags;

    __asm__ volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

/* --------------------------------------------------------------------------
 * paging_unmap_page: Remove a virtual page mapping
 * -------------------------------------------------------------------------- */
void paging_unmap_page(uint32_t virtual_addr) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FF;

    if (page_directory[pd_index] & PAGE_PRESENT) {
        uint32_t* page_table = (uint32_t*)(page_directory[pd_index] & 0xFFFFF000);
        page_table[pt_index] = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    }
}

/* --------------------------------------------------------------------------
 * Accessors for VMM
 * -------------------------------------------------------------------------- */
uint32_t* paging_get_directory(void) {
    return page_directory;
}

uint32_t* paging_get_page_table(uint32_t pd_index) {
    if (pd_index >= 1024) return NULL;
    if (!(page_directory[pd_index] & PAGE_PRESENT)) return NULL;
    return (uint32_t*)(page_directory[pd_index] & 0xFFFFF000);
}
