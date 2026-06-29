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

/* Map the VESA framebuffer's physical range (Phase 14; Phase 53: takes a size
 * so framebuffers larger than 4MB, e.g. 1920x1080x32, are fully mapped). */
void paging_map_vesa_fb(uint32_t fb_phys, uint32_t fb_size);

/* Get page directory and page table pointers (for VMM) */
uint32_t* paging_get_directory(void);
uint32_t* paging_get_page_table(uint32_t pd_index);

#endif /* PAGING_H */
