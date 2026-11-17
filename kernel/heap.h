/* ============================================================================
 * NexusOS — Heap Allocator Header
 * ============================================================================
 * Simple free-list allocator for dynamic memory allocation.
 * ============================================================================ */

#ifndef HEAP_H
#define HEAP_H

#include "types.h"

/* Heap configuration (Phase 57: moved from 2-3MB to 4-8MB so real downloads
 * and apps fit; the old 2-3MB range is now kernel-image headroom, 3-4MB stays
 * the guard band + boot stack, .bss begins at 8MB). The region is identity-
 * mapped (phys == virt) and below the 32MB identity map, so it is DMA-safe. */
#define HEAP_START      0x400000    /* 4MB — start of heap region */
#define HEAP_INITIAL    0x400000    /* 4MB heap: 0x400000-0x800000 */
#define HEAP_MAX        HEAP_INITIAL /* unused: the heap never grows past
                                        HEAP_INITIAL (.bss starts at 8MB) */

/* Block header */
typedef struct block_header {
    uint32_t size;                  /* Size of data area (excludes header) */
    uint8_t  is_free;              /* 1 = free, 0 = allocated */
    struct block_header* next;     /* Next block in list */
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)

/* Core allocation functions */
void  heap_init(void);
void* kmalloc(uint32_t size);
void* kmalloc_aligned(uint32_t size, uint32_t alignment);
void* kcalloc(uint32_t count, uint32_t size);
void  kfree(void* ptr);

/* Statistics */
uint32_t heap_get_used(void);
uint32_t heap_get_free(void);

#endif /* HEAP_H */
