/* ============================================================================
 * NexusOS — Physical Memory Manager (Implementation)
 * ============================================================================
 * Bitmap-based physical page allocator.
 * Each bit represents a 4KB page: 1 = used, 0 = free.
 * ============================================================================ */

#include "memory.h"
#include "paging.h"
#include "heap.h"
#include "port.h"
#include "string.h"
#include "vga.h"

/* External symbols from linker script */
extern uint32_t _kernel_end;
extern uint32_t _bss_start;
extern uint32_t _bss_end;
extern uint32_t _app_bss_start, _app_bss_end;
_Static_assert(USER_PMM_BASE == IDENTITY_MAP_END, "kernel and user physical pools overlap");

/* The bitmap only covers what may be handed out: the identity map (1 bit per
 * page -> 1 KB for 32MB). RAM above it is counted in total_pages only. */
#define PMM_MAX_PAGES   (IDENTITY_MAP_END / PAGE_SIZE)

/* Allocation order (zone starts, highest first): 15MB+, then 8-15MB, then
 * below 8MB (Phase 57: nothing is free below 8MB any more — kernel, headroom,
 * stack and the 4MB heap fill it). In the single address space the legacy
 * loaders map user images (12.5-13MB, elf.h), stacks, shmem and mmap at fixed
 * virtual addresses inside the identity map, which hides the identity view of
 * those physical pages from kernel code; allocating from 15MB first keeps
 * kernel-owned pages (DMA, page tables) out of that window as long as possible. */
static const uint32_t pmm_zones[3] = {
    0x00F00000 / PAGE_SIZE, 0x00800000 / PAGE_SIZE, 0
};

static uint8_t page_bitmap[PMM_MAX_PAGES / 8];
static uint32_t used_pages = 0;
static uint32_t total_pages = 0;   /* detected RAM */
static uint32_t alloc_pages = 0;   /* allocatable: min(RAM, identity map) */

/* Phase 54: high user-frame pool over [USER_PMM_BASE, +window). 1 bit/page. */
#define UPMM_MAX_FRAMES  (USER_PMM_CAP / PAGE_SIZE)   /* 65536 for 256MB */
static uint8_t  user_bitmap[UPMM_MAX_FRAMES / 8];
static uint32_t user_total = 0;     /* frames in the pool */
static uint32_t user_used  = 0;

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
 * detect_ram_pages: RAM size from CMOS. 0x30/0x31 = KB above 1MB (saturates
 * at ~64MB; 0x17/0x18 is the same value), 0x34/0x35 = 64KB blocks above
 * 16MB (QEMU/Bochs/SeaBIOS and most BIOSes), only trusted once >= 16MB.
 * -------------------------------------------------------------------------- */
static uint32_t cmos_word(uint8_t reg) {
    port_byte_out(0x70, reg);
    uint32_t lo = port_byte_in(0x71);
    port_byte_out(0x70, reg + 1);
    return lo | ((uint32_t)port_byte_in(0x71) << 8);
}

static uint32_t detect_ram_pages(void) {
    uint32_t ext_kb = cmos_word(0x30);
    if (ext_kb == 0) ext_kb = cmos_word(0x17);
    if (ext_kb == 0) return PMM_MAX_PAGES;          /* unknown: assume the identity-map size */
    if (ext_kb >= 15u * 1024u) {
        uint32_t blocks = cmos_word(0x34);
        if (blocks) return 4096u + blocks * 16u;     /* 16MB + n * 64KB */
    }
    return 256u + ext_kb / 4u;                       /* 1MB + ext */
}

/* --------------------------------------------------------------------------
 * pmm_init: Initialize physical memory manager
 * Mark low memory, the kernel and the fixed boot-time regions as used
 * -------------------------------------------------------------------------- */
void pmm_init(void) {
    /* Start with all pages free */
    memset(page_bitmap, 0, sizeof(page_bitmap));
    used_pages = 0;
    total_pages = detect_ram_pages();
    if (total_pages < IDENTITY_MAP_END / PAGE_SIZE) {
        vga_print("NexusOS needs at least 40 MB of RAM.\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    alloc_pages = (total_pages < PMM_MAX_PAGES) ? total_pages : PMM_MAX_PAGES;
    memset(&_app_bss_start, 0, (uint32_t)&_app_bss_end - (uint32_t)&_app_bss_start);

    /* Low 1MB (IVT, BDA, boot info, bootloader, VGA, ROM) + the kernel image */
    pmm_reserve_range(0, (uint32_t)&_kernel_end);
    /* Phase 57: everything else below 4MB stays off-limits too — the kernel
     * image headroom (up to KERNEL_IMAGE_LIMIT), the 3MB guard band and the
     * boot stack (BOOT_STACK_BASE..BOOT_STACK_TOP). */
    pmm_reserve_range(0, BOOT_STACK_TOP);
    /* Phase 57: the 4MB kernel heap at 4-8MB (HEAP_START, heap.h) */
    pmm_reserve_range(HEAP_START, HEAP_INITIAL);
    /* .bss at 8MB+ (page tables, this bitmap, static buffers) */
    pmm_reserve_range((uint32_t)&_bss_start,
                      (uint32_t)&_bss_end - (uint32_t)&_bss_start);
    pmm_reserve_range((uint32_t)&_app_bss_start,
                      (uint32_t)&_app_bss_end - (uint32_t)&_app_bss_start);
    /* GUI wallpaper cache. (The VESA back buffer is reserved by fb_init.) */
    pmm_reserve_range(WALLPAPER_CACHE_ADDR, WALLPAPER_CACHE_SIZE);

    /* Phase 54: size the high user-frame pool from RAM above the identity map,
     * capped at USER_PMM_CAP. All frames start free; the kernel never touches
     * them through its own view (see memory.h). */
    memset(user_bitmap, 0, sizeof(user_bitmap));
    user_used = 0;
    if ((uint32_t)total_pages > PMM_MAX_PAGES) {
        uint32_t high = total_pages - PMM_MAX_PAGES;      /* pages above 40MB */
        user_total = (high < UPMM_MAX_FRAMES) ? high : UPMM_MAX_FRAMES;
    } else {
        user_total = 0;
    }

    /* Print memory info */
    char buf[32];
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Physical memory manager initialized\n");
    vga_print("     Total: ");
    int_to_str(total_pages * 4, buf);
    vga_print(buf);
    vga_print(" KB | Free: ");
    int_to_str(pmm_get_free_pages() * 4, buf);
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
static uint32_t pmm_scan(uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            return i * PAGE_SIZE;
        }
    }
    return 0;   /* page 0 is always reserved, so 0 means "none" */
}

uint32_t pmm_alloc_page(void) {
    uint32_t end = alloc_pages;
    for (int z = 0; z < 3; z++) {
        uint32_t from = (pmm_zones[z] < end) ? pmm_zones[z] : end;
        uint32_t addr = pmm_scan(from, end);
        if (addr != 0) return addr;
        end = from;
    }

    /* Out of memory */
    vga_print_color("[ERROR] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("Out of physical memory!\n");
    return 0;
}

/* --------------------------------------------------------------------------
 * pmm_reserve_range: Mark a physical [addr, addr+size) span as used so the
 * allocator never hands those pages out. Used to protect the VESA back buffer
 * (Phase 53) which lives in otherwise-free high RAM.
 * -------------------------------------------------------------------------- */
void pmm_reserve_range(uint32_t addr, uint32_t size) {
    uint32_t first = addr / PAGE_SIZE;
    uint32_t last  = (addr + size + PAGE_SIZE - 1) / PAGE_SIZE;  /* round up */
    for (uint32_t i = first; i < last && i < alloc_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
        }
    }
}

/* --------------------------------------------------------------------------
 * pmm_free_page: Free a previously allocated page
 * -------------------------------------------------------------------------- */
void pmm_free_page(uint32_t addr) {
    uint32_t page = addr / PAGE_SIZE;
    if (page < alloc_pages && bitmap_test(page)) {
        bitmap_clear(page);
        used_pages--;
    }
}

/* --------------------------------------------------------------------------
 * Statistics
 * -------------------------------------------------------------------------- */
uint32_t pmm_get_total_pages(void) { return total_pages; }
uint32_t pmm_get_used_pages(void)  { return used_pages; }
uint32_t pmm_get_free_pages(void)  { return alloc_pages - used_pages; }

/* --------------------------------------------------------------------------
 * Phase 54: high user-frame pool
 * -------------------------------------------------------------------------- */
uint32_t upmm_alloc_frame(void) {
    for (uint32_t i = 0; i < user_total; i++) {
        if (!((user_bitmap[i / 8] >> (i % 8)) & 1)) {
            user_bitmap[i / 8] |= (uint8_t)(1 << (i % 8));
            user_used++;
            return USER_PMM_BASE + i * PAGE_SIZE;
        }
    }
    return 0;   /* pool exhausted */
}

void upmm_free_frame(uint32_t phys) {
    if (phys < USER_PMM_BASE) return;
    uint32_t i = (phys - USER_PMM_BASE) / PAGE_SIZE;
    if (i >= user_total) return;
    if ((user_bitmap[i / 8] >> (i % 8)) & 1) {
        user_bitmap[i / 8] &= (uint8_t)~(1 << (i % 8));
        if (user_used) user_used--;
    }
}

uint32_t upmm_total_frames(void) { return user_total; }
uint32_t upmm_free_frames(void)  { return user_total - user_used; }
