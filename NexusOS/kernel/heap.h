/* ============================================================================
 * NexusOS — Heap Allocator Header
 * ============================================================================
 * Simple free-list allocator for dynamic memory allocation.
 * ============================================================================ */

#ifndef HEAP_H
#define HEAP_H

#include "types.h"

/* Heap configuration */
#define HEAP_START      0x200000    /* 2MB — start of heap region */
#define HEAP_INITIAL    0x100000    /* 1MB initial heap size */
#define HEAP_MAX        0x800000    /* 8MB max heap */

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
