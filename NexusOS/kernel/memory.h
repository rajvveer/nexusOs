/* ============================================================================
 * NexusOS — Physical Memory Manager (Header)
 * ============================================================================ */

#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/* Page size: 4 KB */
#define PAGE_SIZE 4096

/* Total memory to manage: 2 GB. The PMM bitmap scales with this (1 bit/page ->
 * 64 KB of BSS at 2 GB). The fixed heap (heap.c, 0x200000-0x800000) and DMA
 * buffers stay in the identity-mapped low 16 MB regardless; the extra range is
 * available to demand paging (paging.c page-fault handler draws from the PMM).
 * NOTE: QEMU must be launched with -m 2048 for 2 GB to physically exist. */
#define TOTAL_MEMORY    (2048u * 1024u * 1024u)
#define TOTAL_PAGES     (TOTAL_MEMORY / PAGE_SIZE)

/* Bitmap size in bytes (1 bit per page) */
#define BITMAP_SIZE     (TOTAL_PAGES / 8)

/* Initialize the physical memory manager */
void pmm_init(void);

/* Allocate a single 4KB page, returns physical address */
uint32_t pmm_alloc_page(void);

/* Free a previously allocated page */
void pmm_free_page(uint32_t addr);

/* Reserve a physical [addr, addr+size) span so it is never allocated (e.g. the
 * VESA back buffer). Safe to call before or after pmm_init's kernel marking. */
void pmm_reserve_range(uint32_t addr, uint32_t size);

/* Get memory statistics */
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

#endif /* MEMORY_H */
