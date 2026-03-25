/* ============================================================================
 * NexusOS — Physical Memory Manager (Implementation)
 * ============================================================================
 * Bitmap-based physical page allocator.
 * Each bit represents a 4KB page: 1 = used, 0 = free.
 * ============================================================================ */

#include "memory.h"
#include "string.h"
#include "vga.h"

/* External symbols from linker script */
extern uint32_t _kernel_end;

/* Bitmap: 1 bit per page */
static uint8_t page_bitmap[BITMAP_SIZE];
static uint32_t used_pages = 0;

/* --------------------------------------------------------------------------
 * Helper: Set a bit in the bitmap (mark page as used)
 * -------------------------------------------------------------------------- */
static void bitmap_set(uint32_t page) {
    page_bitmap[page / 8] |= (1 << (page % 8));
}

/* --------------------------------------------------------------------------
 * Helper: Clear a bit in the bitmap (mark page as free)
 * -------------------------------------------------------------------------- */
static void bitmap_clear(uint32_t page) {
    page_bitmap[page / 8] &= ~(1 << (page % 8));
}

/* --------------------------------------------------------------------------
 * Helper: Test if a page is used
 * -------------------------------------------------------------------------- */
static bool bitmap_test(uint32_t page) {
    return (page_bitmap[page / 8] >> (page % 8)) & 1;
}

/* --------------------------------------------------------------------------
 * pmm_init: Initialize physical memory manager
 * Mark kernel area and low memory as used
 * -------------------------------------------------------------------------- */
void pmm_init(void) {
    /* Start with all pages free */
    memset(page_bitmap, 0, BITMAP_SIZE);
    used_pages = 0;

    /* Mark low memory (0x0 - 0x100000 = first 1MB) as reserved
     * This includes: IVT, BDA, bootloader, VGA buffer, ROM */
    uint32_t reserved_pages = 0x100000 / PAGE_SIZE;  /* 256 pages = 1MB */
    for (uint32_t i = 0; i < reserved_pages; i++) {
        bitmap_set(i);
        used_pages++;
    }

    /* Mark kernel pages as used */
    uint32_t kernel_end = (uint32_t)&_kernel_end;
    uint32_t kernel_pages = (kernel_end / PAGE_SIZE) + 1;
    for (uint32_t i = reserved_pages; i < kernel_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
        }
    }

    /* Print memory info */
    char buf[32];
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Physical memory manager initialized\n");
    vga_print("     Total: ");
    int_to_str(TOTAL_MEMORY / 1024, buf);
    vga_print(buf);
    vga_print(" KB | Free: ");
    int_to_str((TOTAL_PAGES - used_pages) * 4, buf);
    vga_print(buf);
    vga_print(" KB | Used: ");
    int_to_str(used_pages * 4, buf);
    vga_print(buf);
    vga_print(" KB\n");
}

/* --------------------------------------------------------------------------
 * pmm_alloc_page: Find and allocate a free page
 * Returns physical address, or 0 on failure
 * -------------------------------------------------------------------------- */
uint32_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            return i * PAGE_SIZE;
        }
    }
    /* Out of memory */
    vga_print_color("[ERROR] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("Out of physical memory!\n");
    return 0;
}

/* --------------------------------------------------------------------------
 * pmm_free_page: Free a previously allocated page
 * -------------------------------------------------------------------------- */
void pmm_free_page(uint32_t addr) {
    uint32_t page = addr / PAGE_SIZE;
    if (page < TOTAL_PAGES && bitmap_test(page)) {
        bitmap_clear(page);
        used_pages--;
    }
}

/* --------------------------------------------------------------------------
 * Statistics
 * -------------------------------------------------------------------------- */
uint32_t pmm_get_total_pages(void) { return TOTAL_PAGES; }
uint32_t pmm_get_used_pages(void)  { return used_pages; }
uint32_t pmm_get_free_pages(void)  { return TOTAL_PAGES - used_pages; }
