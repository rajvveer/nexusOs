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
#define PAGE_COW       0x200   /* Bit 9 (AVL field): copy-on-write marker */

/* Page size */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Initialize paging: identity-map first 16MB */
void paging_init(void);

/* Map a virtual address to a physical address */
void paging_map_page(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);

/* Unmap a virtual address */
void paging_unmap_page(uint32_t virtual_addr);

/* Map VESA framebuffer physical address (Phase 14) */
void paging_map_vesa_fb(uint32_t fb_phys);

/* Get page directory and page table pointers (for VMM) */
uint32_t* paging_get_directory(void);
uint32_t* paging_get_page_table(uint32_t pd_index);

#endif /* PAGING_H */
