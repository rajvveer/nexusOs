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
#include "elf.h"        /* Phase 57: USER_LOAD_BASE (legacy demand-paging window) */
#include "kguard.h"     /* Phase 55: ring-0 fault recovery */

/* Page directory: 1024 entries, each points to a page table */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

/* Page tables for the identity map. Each table maps 4MB, so IDENTITY_TABLES
 * (paging.h) tables cover IDENTITY_TABLES*4MB. Phase 53: raised from 4 (16MB)
 * to 8 (32MB) so the relocated high VESA back buffer (1920x1080x4 ~= 8MB at
 * 0x01000000) lands inside identity-mapped RAM. See framebuffer.c FB_BACKBUF_ADDR. */
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
        /* Check if address is in a valid user region (legacy single-address-
         * space window, elf.h USER_LOAD_BASE (12.5MB) up to 15MB). Phase 57:
         * the lower bound follows the loaders — 4-8MB is the kernel heap now. */
        if (fault_addr >= USER_LOAD_BASE && fault_addr < 0x00F00000) {
            uint32_t phys = pmm_alloc_page();
            if (phys != 0) {
                memset((void*)phys, 0, PAGE_SIZE);
                uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
                paging_map_page(fault_addr & 0xFFFFF000, phys, flags);
                return; /* Demand paging handled */
            }
        }
    }

    /* Phase 54: a fault blamed on a user process (ring-3 access, or a bad user
     * pointer dereferenced by the kernel while servicing that process's
     * syscall) kills ONLY that process and returns to the scheduler — the OS
     * keeps running. uproc_fault() never returns when it handles the fault. */
    {
        extern bool uproc_fault(struct registers* regs, uint32_t fault_addr);
        if (uproc_fault(regs, fault_addr)) return;   /* unreachable in practice */
    }

    /* Phase 55: a genuine ring-0 page fault in the kernel thread (NULL deref,
     * write into the read-only kernel image, stray pointer) is contained when
     * a kguard is armed: one "!KR:" serial line, then long-jump back into the
     * guard. Never returns. */
    if (kguard_should_recover(regs)) kguard_recover(regs, 14, fault_addr);

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

    /* Halt (Phase 55: through the common fatal path so the "!EX:" serial line
     * and the red crash bar appear exactly as for the other exceptions). */
    exception_fatal(14, err, regs->eip);
}

/* ============================================================================
 * Phase 55: kernel self-protection (W^X for the kernel image + guard pages)
 * ============================================================================
 * Runs once right after paging_init() (CR0.WP is already set there, so ring-0
 * writes honour read-only PTEs):
 *   - clears PAGE_WRITABLE on every identity-map PTE covering the kernel's
 *     .text + .rodata, [_text_start, _data_start) (linker.ld page-aligns the
 *     .rodata/.data boundary), so a stray write into kernel code faults;
 *   - unmaps page 0, so NULL dereferences fault instead of silently reading
 *     the real-mode IVT (nothing reads physical memory below 0x1000 after
 *     boot: the VESA info block is at 0x9000, the BDA is never used);
 *   - unmaps KSTACK_GUARD_PAGES pages at the bottom of the boot-stack region
 *     [BOOT_STACK_BASE, BOOT_STACK_TOP) (3-4MB, memory.h) so a runaway
 *     recursion faults (and, via kguard's #DF task gate, is recovered) instead
 *     of silently running into the kernel-image headroom below. (Phase 57: the
 *     heap moved to 4-8MB; the band stays at 3MB, directly under the stack.)
 * Ring-3 address spaces share these page tables (as_create copies the PDEs),
 * so they inherit all three protections.
 * ============================================================================ */
extern uint8_t _text_start[];
extern uint8_t _data_start[];

#define KSTACK_GUARD_BASE   BOOT_STACK_BASE                /* 0x300000 */
#define KSTACK_GUARD_PAGES  4                              /* 16KB band */

static uint32_t* ident_pte(uint32_t addr) {
    return &page_tables[addr >> 22][(addr >> 12) & 0x3FF];
}

void paging_protect_kernel(void) {
    uint32_t start = (uint32_t)_text_start & 0xFFFFF000u;
    uint32_t end   = ((uint32_t)_data_start + 0xFFFu) & 0xFFFFF000u;
    uint32_t pages = 0;

    for (uint32_t a = start; a < end && a < IDENTITY_MAP_END; a += PAGE_SIZE, pages++)
        *ident_pte(a) &= ~(uint32_t)PAGE_WRITABLE;

    *ident_pte(0) = 0;                                       /* NULL page */

    for (uint32_t i = 0; i < KSTACK_GUARD_PAGES; i++)        /* stack guard */
        *ident_pte(KSTACK_GUARD_BASE + i * PAGE_SIZE) = 0;

    /* Flush the TLB: the identity map was already live during paging_init. */
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory) : "memory");

    char b[12];
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Kernel W^X: .text+.rodata read-only (");
    int_to_str((int)pages, b); vga_print(b);
    vga_print(" pages), NULL page + 16KB stack guard unmapped\n");
}

uint32_t paging_kstack_guard_base(void) { return KSTACK_GUARD_BASE; }
uint32_t paging_kstack_guard_size(void) { return KSTACK_GUARD_PAGES * PAGE_SIZE; }

/* --------------------------------------------------------------------------
 * paging_init: Set up identity mapping and enable paging
 * -------------------------------------------------------------------------- */
void paging_init(void) {
    /* Clear page directory */
    memset(page_directory, 0, sizeof(page_directory));

    /* Identity map the first IDENTITY_TABLES*4MB (40MB) using one page table
     * per 4MB. This covers the kernel, heap, DMA buffers, and the relocated
     * high VESA back buffer. */
    for (int t = 0; t < IDENTITY_TABLES; t++) {
        for (int i = 0; i < 1024; i++) {
            uint32_t phys = (t * 1024 + i) * PAGE_SIZE;
            page_tables[t][i] = phys | PAGE_PRESENT | PAGE_WRITABLE;
        }
        page_directory[t] = ((uint32_t)page_tables[t]) | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* Load page directory into CR3 and enable paging in CR0. WP makes ring-0
     * writes honour read-only PTEs, so kernel writes into a COW page (the only
     * read-only mappings, from vmm_fork) take the COW fault instead of
     * silently writing the shared page. */
    __asm__ volatile(
        "mov %0, %%cr3\n"       /* Load page directory base */
        "mov %%cr0, %%eax\n"
        "or $0x80010000, %%eax\n" /* Set PG (paging) + WP (write protect) */
        "mov %%eax, %%cr0\n"
        :
        : "r"(page_directory)
        : "eax"
    );

    /* Register page fault handler (ISR 14) */
    register_interrupt_handler(14, page_fault_handler);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Paging enabled (40MB, page faults)\n");
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
 * paging_map_mmio (Phase 55): identity-map a device MMIO window (PCI memory
 * BARs sit above the 40 MB identity map, e.g. 0xFEBxxxxx) as present,
 * writable, cache-disabled supervisor pages. Must run before uproc_init():
 * as_create() copies the kernel PDEs at process creation time.
 * -------------------------------------------------------------------------- */
void paging_map_mmio(uint32_t phys, uint32_t size) {
    if (size == 0) return;
    uint32_t start = phys & 0xFFFFF000u;
    uint32_t last  = (phys + size - 1) & 0xFFFFF000u;   /* inclusive, no wrap */
    if (last < start) last = 0xFFFFF000u;
    for (uint32_t a = start; ; a += PAGE_SIZE) {
        paging_map_page(a, a, PAGE_WRITABLE | PAGE_PCD);
        if (a == last) break;
    }
    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax", "memory");
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

    /* x86 checks U/S at both levels, so a user page needs a user PDE too. The
     * identity-map PDEs start kernel-only; their other PTEs keep U/S clear,
     * so this exposes nothing beyond the pages mapped with PAGE_USER. */
    page_directory[pd_index] |= flags & PAGE_USER;

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

/* ============================================================================
 * Phase 54: per-process address spaces
 * ============================================================================ */

/* The directory currently in CR3. Initialized to the kernel directory, which
 * paging_init loaded. Updated by paging_switch. */
static uint32_t current_cr3 = 0;

uint32_t paging_kernel_cr3(void)  { return (uint32_t)page_directory; }
uint32_t paging_current_cr3(void) {
    return current_cr3 ? current_cr3 : (uint32_t)page_directory;
}

void paging_switch(uint32_t pd_phys) {
    if (pd_phys == 0) pd_phys = (uint32_t)page_directory;
    if (pd_phys == paging_current_cr3()) return;
    current_cr3 = pd_phys;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

uint32_t as_create(void) {
    uint32_t pd_phys = pmm_alloc_page();        /* low pool: kernel-addressable */
    if (pd_phys == 0) return 0;
    uint32_t* pd = (uint32_t*)pd_phys;
    memset(pd, 0, PAGE_SIZE);
    /* Share every PDE the kernel maps: the 0-40MB identity map and the VESA
     * LFB. They carry no PAGE_USER, so user code cannot reach kernel memory. */
    for (int i = 0; i < 1024; i++) {
        if (page_directory[i] & PAGE_PRESENT) pd[i] = page_directory[i];
    }
    return pd_phys;
}

void as_map_page(uint32_t pd_phys, uint32_t vaddr, uint32_t phys, uint32_t flags) {
    uint32_t* pd = (uint32_t*)pd_phys;
    uint32_t i = vaddr >> 22;
    uint32_t j = (vaddr >> 12) & 0x3FF;

    if (!(pd[i] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_page();     /* low pool */
        if (pt_phys == 0) return;
        memset((void*)pt_phys, 0, PAGE_SIZE);
        pd[i] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    pd[i] |= PAGE_USER | PAGE_WRITABLE;

    uint32_t* pt = (uint32_t*)(pd[i] & 0xFFFFF000);
    pt[j] = (phys & 0xFFFFF000) | PAGE_PRESENT | flags;

    if (pd_phys == paging_current_cr3())
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

void as_set_flags(uint32_t pd_phys, uint32_t vaddr, uint32_t flags) {
    uint32_t* pd = (uint32_t*)pd_phys;
    uint32_t i = vaddr >> 22;
    uint32_t j = (vaddr >> 12) & 0x3FF;
    if (!(pd[i] & PAGE_PRESENT)) return;
    uint32_t* pt = (uint32_t*)(pd[i] & 0xFFFFF000);
    if (!(pt[j] & PAGE_PRESENT)) return;
    pt[j] = (pt[j] & 0xFFFFF000) | PAGE_PRESENT | flags;
    if (pd_phys == paging_current_cr3())
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

bool as_check_user(uint32_t pd_phys, uint32_t vaddr, uint32_t len) {
    if (len == 0) return true;
    uint32_t* pd = (uint32_t*)pd_phys;
    uint32_t first = vaddr & 0xFFFFF000;
    uint32_t last  = (vaddr + len - 1) & 0xFFFFF000;   /* caller bounds-checks wrap */
    if (vaddr + len < vaddr) return false;             /* overflow */
    for (uint32_t p = first; ; p += PAGE_SIZE) {
        uint32_t i = p >> 22;
        uint32_t j = (p >> 12) & 0x3FF;
        if (!(pd[i] & PAGE_PRESENT) || !(pd[i] & PAGE_USER)) return false;
        uint32_t* pt = (uint32_t*)(pd[i] & 0xFFFFF000);
        if (!(pt[j] & PAGE_PRESENT) || !(pt[j] & PAGE_USER)) return false;
        if (p == last) break;
    }
    return true;
}

void as_destroy(uint32_t pd_phys) {
    if (pd_phys == 0 || pd_phys == (uint32_t)page_directory) return;
    uint32_t* pd = (uint32_t*)pd_phys;
    uint32_t i_start = USER_SPACE_START >> 22;      /* 32  */
    uint32_t i_end   = USER_SPACE_END   >> 22;      /* 768 */
    for (uint32_t i = i_start; i < i_end; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        uint32_t pt_phys = pd[i] & 0xFFFFF000;
        uint32_t* pt = (uint32_t*)pt_phys;
        for (int j = 0; j < 1024; j++) {
            if (!(pt[j] & PAGE_PRESENT)) continue;
            uint32_t phys = pt[j] & 0xFFFFF000;
            if (phys >= USER_PMM_BASE) upmm_free_frame(phys);
            else                       pmm_free_page(phys);
        }
        pmm_free_page(pt_phys);
        pd[i] = 0;
    }
    pmm_free_page(pd_phys);
}
