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

    total_allocated = 0;

    /* Phase 57: report the real geometry (4096 KB at 0x400000). */
    char b[16];
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Heap initialized (");
    int_to_str((int)(HEAP_INITIAL / 1024u), b); vga_print(b);
    vga_print(" KB at ");
    hex_to_str(HEAP_START, b); vga_print(b);
    vga_print(")\n");
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

    /* Larger than the whole heap can never fit — and rejecting it here keeps
     * the round-up below from wrapping (0xFFFFFFFD+ would become tiny). */
    if (size <= HEAP_INITIAL) {
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
    /* Reject sizes whose over-allocation could wrap */
    if (alignment == 0 || size > HEAP_INITIAL || alignment > HEAP_INITIAL) return NULL;

    /* Over-allocate and adjust pointer. The result may point inside the block;
     * kfree() accepts any pointer into a live block, so it can be freed. */
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
    if (size != 0 && count > 0xFFFFFFFFu / size) return NULL;   /* overflow */
    uint32_t total = count * size;
    void* ptr = kmalloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

/* --------------------------------------------------------------------------
 * kfree: Free previously allocated memory
 * The block list is address-ordered and contiguous, so walk it to find the
 * live block whose data area holds ptr: this accepts kmalloc_aligned()'s
 * mid-block pointers and silently ignores anything else (stray, foreign or
 * double-freed pointers) instead of trusting a fake header at ptr-HEADER_SIZE.
 * Neighbouring free blocks are merged in place (no two free blocks are ever
 * adjacent, so one merge each way suffices).
 * -------------------------------------------------------------------------- */
void kfree(void* ptr) {
    if (ptr == NULL) return;

    uint8_t* p = (uint8_t*)ptr;
    block_header_t* prev = NULL;
    block_header_t* block = heap_start;
    while (block != NULL) {
        uint8_t* data = (uint8_t*)block + HEADER_SIZE;
        if (p < data) return;                 /* header / outside the heap */
        if (p < data + block->size) break;
        prev = block;
        block = block->next;
    }
    if (block == NULL || block->is_free) return;

    total_allocated -= block->size;
    block->is_free = 1;

    if (block->next != NULL && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }
    if (prev != NULL && prev->is_free) {
        prev->size += HEADER_SIZE + block->size;
        prev->next = block->next;
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
