/* ============================================================================
 * NexusOS — Paging (Header) — Phase 21
 * ============================================================================ */

#ifndef PAGING_H
#define PAGING_H

#include "types.h"

/* Page directory/table entry flags */
#define PAGE_PRESENT   0x01
#define PAGE_WRITABLE  0x02
#define PAGE_USER      0x04
#define PAGE_PCD       0x10    /* cache disable (MMIO) */
#define PAGE_COW       0x200   /* Bit 9 (AVL field): copy-on-write marker */

/* Page size */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Identity map: IDENTITY_TABLES 4MB page tables cover [0, IDENTITY_MAP_END).
 * Everything the kernel touches by physical address (kernel, heap, DMA,
 * back buffer, every PMM page) must live below it. */
#define IDENTITY_TABLES   10
#define IDENTITY_MAP_END  (IDENTITY_TABLES * 0x00400000u)   /* 40MB */

/* Initialize paging: identity-map the first IDENTITY_MAP_END bytes */
void paging_init(void);

/* Phase 55: write-protect .text+.rodata, unmap the NULL page and the kernel
 * stack guard band (call once, right after paging_init). */
void paging_protect_kernel(void);
uint32_t paging_kstack_guard_base(void);
uint32_t paging_kstack_guard_size(void);

/* Map a virtual address to a physical address */
void paging_map_page(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);

/* Unmap a virtual address */
void paging_unmap_page(uint32_t virtual_addr);

/* Map the VESA framebuffer's physical range (Phase 14; Phase 53: takes a size
 * so framebuffers larger than 4MB, e.g. 1920x1080x32, are fully mapped). */
void paging_map_vesa_fb(uint32_t fb_phys, uint32_t fb_size);

/* Phase 55: identity-map a device MMIO range (PCI memory BAR above the 40MB
 * identity map) with PAGE_PRESENT|PAGE_WRITABLE|PAGE_PCD. Call before touching
 * the registers and before uproc_init() (new address spaces copy the PDEs). */
void paging_map_mmio(uint32_t phys, uint32_t size);

/* Get page directory and page table pointers (for VMM) */
uint32_t* paging_get_directory(void);
uint32_t* paging_get_page_table(uint32_t pd_index);

/* ============================================================================
 * Phase 54: per-process address spaces.
 * ----------------------------------------------------------------------------
 * Each user process gets its own page directory. The kernel's PDEs (the 0-40MB
 * identity map and the VESA LFB) are SHARED by copying them into every new
 * directory, so they stay supervisor-only and user code can never read kernel
 * memory. User virtual space is [USER_SPACE_START, USER_SPACE_END); its PDEs
 * and page tables are per-process. Page-table frames come from the LOW pool
 * (pmm_alloc_page) so the kernel can edit them directly through the identity
 * map; the DATA frames they point at come from the high user pool (memory.h).
 * A page directory is itself a low frame, so pd_phys == its virtual address.
 * ============================================================================ */
#define USER_SPACE_START  0x08000000u   /* 128MB */
#define USER_SPACE_END    0xC0000000u   /* 3GB   */

/* True if [va, va+len) lies entirely inside the user virtual window. */
static inline bool paging_user_window_ok(uint32_t va, uint32_t len) {
    return va >= USER_SPACE_START && va <= USER_SPACE_END && len <= USER_SPACE_END - va;
}

/* Physical address (== virtual, identity-mapped) of the kernel page directory;
 * this is process 0's (the kernel thread's) CR3. */
uint32_t paging_kernel_cr3(void);

/* The page directory currently loaded in CR3 (tracked by paging_switch). */
uint32_t paging_current_cr3(void);

/* Load a page directory into CR3 (no-op if already current). */
void paging_switch(uint32_t pd_phys);

/* Create a new user page directory (shares the kernel's PDEs). Returns its
 * physical address, or 0 on out-of-memory. */
uint32_t as_create(void);

/* Destroy a user page directory: free every user-region data frame, page
 * table and the directory itself. Shared kernel PDEs are left untouched. */
void as_destroy(uint32_t pd_phys);

/* Map one page in the given directory. `phys` must already be allocated by the
 * caller (high pool for data pages). Page-table frames are taken from the low
 * pool. `flags` are OR'd with PAGE_PRESENT (pass PAGE_WRITABLE / PAGE_USER). */
void as_map_page(uint32_t pd_phys, uint32_t vaddr, uint32_t phys, uint32_t flags);

/* Change the flags of an already-mapped page (e.g. drop PAGE_WRITABLE to make
 * the Win32 stub page read-only). PAGE_PRESENT is kept. */
void as_set_flags(uint32_t pd_phys, uint32_t vaddr, uint32_t flags);

/* True if every page of [vaddr, vaddr+len) is present AND user-accessible in
 * the given directory (used to validate user pointers before the kernel reads
 * or writes them). len==0 is vacuously true. */
bool as_check_user(uint32_t pd_phys, uint32_t vaddr, uint32_t len);

#endif /* PAGING_H */
