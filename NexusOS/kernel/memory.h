/* ============================================================================
 * NexusOS — Physical Memory Manager (Header)
 * ============================================================================ */

#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/* Page size: 4 KB */
#define PAGE_SIZE 4096

/* Total memory to manage: 32 MB (increased for Phase 27+ headroom) */
#define TOTAL_MEMORY    (32 * 1024 * 1024)
#define TOTAL_PAGES     (TOTAL_MEMORY / PAGE_SIZE)

/* Bitmap size in bytes (1 bit per page) */
#define BITMAP_SIZE     (TOTAL_PAGES / 8)

/* Initialize the physical memory manager */
void pmm_init(void);

/* Allocate a single 4KB page, returns physical address */
uint32_t pmm_alloc_page(void);

/* Free a previously allocated page */
void pmm_free_page(uint32_t addr);

/* Get memory statistics */
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

#endif /* MEMORY_H */
