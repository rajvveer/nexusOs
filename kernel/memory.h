/* ============================================================================
 * NexusOS — Physical Memory Manager (Header)
 * ============================================================================ */

#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/* Page size: 4 KB */
#define PAGE_SIZE 4096

/* Fixed physical regions the PMM never hands out (see pmm_init). Phase 57 layout:
 *   0x000000-1MB      IVT/BDA/boot info/VGA/ROM
 *   0x100000-_kernel_end   kernel image (boot2 loads <= KERNEL_LOAD_LIMIT bytes,
 *                     i.e. up to 0x260000; headroom up to KERNEL_IMAGE_LIMIT)
 *   0x2C0000-0x300000 unused gap        0x300000 16KB stack guard band (paging.c)
 *   0x300000-0x400000 boot stack (grows down from BOOT_STACK_TOP)
 *   0x400000-0x800000 kernel heap, 4MB (heap.h HEAP_START/HEAP_INITIAL)
 *   0x800000-_bss_end .bss (linker.ld asserts _bss_end <= 0xC60000)
 *   0xC6C000-0xC80000 legacy user stacks   0xC80000-0xD00000 legacy user-image window (elf.h)
 *   0x1000000         VESA back buffer (fb_init)     WALLPAPER_CACHE  GUI cache */
#define KERNEL_LOAD_LIMIT     (2816u * 512u)  /* boot2.asm KERNEL_SECTORS*512 = 1,441,792 bytes */
#define KERNEL_IMAGE_LIMIT    0x002C0000  /* image may never reach the 3MB guard band */
#define BOOT_STACK_BASE       0x00300000  /* 3MB: guard band, then the stack */
#define BOOT_STACK_TOP        0x00400000  /* kernel_entry.asm esp (grows down) */
#define WALLPAPER_CACHE_ADDR  0x01800000  /* 24MB: decoded-wallpaper cache */
#define WALLPAPER_CACHE_SIZE  0x00800000  /* 8MB, ends at 32MB; browser arenas follow */

/* Total RAM is detected from CMOS at pmm_init and reported as-is, but pages
 * are only ever allocated below the identity map (paging.h IDENTITY_MAP_END,
 * 40MB): callers memset / DMA the returned physical address directly. */

/* Initialize the physical memory manager */
void pmm_init(void);

/* Allocate a single 4KB page, returns physical address */
uint32_t pmm_alloc_page(void);

/* Free a previously allocated page */
void pmm_free_page(uint32_t addr);

/* Reserve a physical [addr, addr+size) span so it is never allocated (e.g. the
 * VESA back buffer). Call after pmm_init (it resets the bitmap). */
void pmm_reserve_range(uint32_t addr, uint32_t size);

/* Get memory statistics: total = detected RAM, used = reserved + allocated,
 * free = pages still allocatable (identity-mapped and unused). */
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_free_pages(void);

/* ============================================================================
 * Phase 54: User-frame pool (physical RAM ABOVE the 40MB identity map).
 * ----------------------------------------------------------------------------
 * The low allocator (pmm_alloc_page) hands out identity-mapped frames the
 * kernel can touch directly (page tables, DMA, kernel stacks). User processes
 * instead back their pages with frames from this separate high pool so a user
 * frame is NEVER reachable through the kernel's identity view — the kernel
 * only ever sees it through that process's page directory while its CR3 is
 * loaded. The pool spans [USER_PMM_BASE, USER_PMM_BASE + window), where the
 * window = min(detected RAM above 40MB, USER_PMM_CAP). A bitmap tracks it. */
#define USER_PMM_BASE   0x02800000u          /* 40MB (== IDENTITY_MAP_END) */
#define USER_PMM_CAP    0x10000000u          /* cap the pool at 256MB      */

/* Allocate / free one 4KB high frame. upmm_alloc_frame returns a physical
 * address >= USER_PMM_BASE, or 0 if the pool is exhausted / absent. */
uint32_t upmm_alloc_frame(void);
void     upmm_free_frame(uint32_t phys);

/* Pool statistics (pages). */
uint32_t upmm_total_frames(void);
uint32_t upmm_free_frames(void);

#endif /* MEMORY_H */
