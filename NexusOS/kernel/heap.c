/* ============================================================================
 * NexusOS — Heap Allocator (Implementation)
 * ============================================================================
 * Free-list allocator. Maintains a linked list of blocks, each with a header
 * indicating size and free status. On alloc, finds first-fit free block or
 * extends heap. On free, marks block free and merges adjacent free blocks.
 * ============================================================================ */

#include "heap.h"
#include "string.h"
#include "vga.h"
#include "paging.h"
#include "memory.h"

/* Head of the free list */
static block_header_t* heap_start = NULL;
static uint32_t heap_end = 0;
static uint32_t total_allocated = 0;

/* --------------------------------------------------------------------------
 * heap_init: Set up initial heap region
 * -------------------------------------------------------------------------- */
void heap_init(void) {
    /* Identity-map the heap region pages */
    uint32_t addr;
    for (addr = HEAP_START; addr < HEAP_START + HEAP_INITIAL; addr += 0x1000) {
        paging_map_page(addr, addr, PAGE_PRESENT | PAGE_WRITABLE);
    }

    /* Create initial free block spanning entire heap */
    heap_start = (block_header_t*)HEAP_START;
    heap_start->size = HEAP_INITIAL - HEADER_SIZE;
    heap_start->is_free = 1;
    heap_start->next = NULL;

    heap_end = HEAP_START + HEAP_INITIAL;
    total_allocated = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Heap initialized (1 MB at 0x200000)\n");
}

/* --------------------------------------------------------------------------
 * split_block: Split a free block if it's large enough
 * -------------------------------------------------------------------------- */
static void split_block(block_header_t* block, uint32_t size) {
    /* Only split if remaining space > header + minimum useful size (16 bytes) */
    if (block->size >= size + HEADER_SIZE + 16) {
        block_header_t* new_block = (block_header_t*)((uint8_t*)block + HEADER_SIZE + size);
        new_block->size = block->size - size - HEADER_SIZE;
        new_block->is_free = 1;
        new_block->next = block->next;

        block->size = size;
        block->next = new_block;
    }
}

/* --------------------------------------------------------------------------
 * kmalloc: Allocate memory from the heap
 * Returns pointer to allocated memory, or NULL on failure
 * -------------------------------------------------------------------------- */
void* kmalloc(uint32_t size) {
    if (size == 0) return NULL;

    /* Align size to 4 bytes */
    size = (size + 3) & ~3;

    block_header_t* current = heap_start;

    /* First-fit search */
    while (current != NULL) {
        if (current->is_free && current->size >= size) {
            split_block(current, size);
            current->is_free = 0;
            total_allocated += current->size;
            return (void*)((uint8_t*)current + HEADER_SIZE);
        }
        current = current->next;
    }

    /* No suitable block found — out of heap */
    vga_print_color("[ERROR] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("Heap exhausted!\n");
    return NULL;
}

/* --------------------------------------------------------------------------
 * kmalloc_aligned: Allocate page-aligned memory
 * -------------------------------------------------------------------------- */
void* kmalloc_aligned(uint32_t size, uint32_t alignment) {
    /* Over-allocate and adjust pointer */
    void* ptr = kmalloc(size + alignment);
    if (ptr == NULL) return NULL;

    uint32_t addr = (uint32_t)ptr;
    uint32_t aligned = (addr + alignment - 1) & ~(alignment - 1);

    /* If already aligned, return as-is */
    if (aligned == addr) return ptr;

    return (void*)aligned;
}

/* --------------------------------------------------------------------------
 * kcalloc: Allocate and zero-initialize memory
 * -------------------------------------------------------------------------- */
void* kcalloc(uint32_t count, uint32_t size) {
    uint32_t total = count * size;
    void* ptr = kmalloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

/* --------------------------------------------------------------------------
 * merge_free: Merge adjacent free blocks
 * -------------------------------------------------------------------------- */
static void merge_free(void) {
    block_header_t* current = heap_start;
    while (current != NULL && current->next != NULL) {
        if (current->is_free && current->next->is_free) {
            current->size += HEADER_SIZE + current->next->size;
            current->next = current->next->next;
            /* Don't advance — check if we can merge more */
        } else {
            current = current->next;
        }
    }
}

/* --------------------------------------------------------------------------
 * kfree: Free previously allocated memory
 * -------------------------------------------------------------------------- */
void kfree(void* ptr) {
    if (ptr == NULL) return;

    block_header_t* block = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);

    /* Sanity check: is this within heap range? */
    uint32_t addr = (uint32_t)block;
    if (addr < HEAP_START || addr >= heap_end) return;

    if (!block->is_free) {
        total_allocated -= block->size;
        block->is_free = 1;
        merge_free();
    }
}

/* --------------------------------------------------------------------------
 * Statistics
 * -------------------------------------------------------------------------- */
uint32_t heap_get_used(void) { return total_allocated; }
uint32_t heap_get_free(void) {
    uint32_t free_bytes = 0;
    block_header_t* current = heap_start;
    while (current != NULL) {
        if (current->is_free) free_bytes += current->size;
        current = current->next;
    }
    return free_bytes;
}
