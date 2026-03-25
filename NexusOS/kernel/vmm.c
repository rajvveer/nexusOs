/* ============================================================================
 * NexusOS — Virtual Memory Manager (Implementation) — Phase 21
 * ============================================================================
 * COW fork, shared memory, memory-mapped files, page reference counting.
 * ============================================================================ */

#include "vmm.h"
#include "paging.h"
#include "memory.h"
#include "string.h"
#include "vga.h"
#include "vfs.h"

/* --------------------------------------------------------------------------
 * Page Reference Counting
 * --------------------------------------------------------------------------
 * Track how many page directories reference each physical page.
 * When ref drops to 0, the page can be freed.
 * -------------------------------------------------------------------------- */

#define MAX_TRACKED_PAGES 4096  /* Track up to 16MB worth of pages */
static uint16_t page_refs[MAX_TRACKED_PAGES];

static int page_to_index(uint32_t phys) {
    uint32_t idx = phys / PAGE_SIZE;
    return (idx < MAX_TRACKED_PAGES) ? (int)idx : -1;
}

void vmm_ref_page(uint32_t phys_addr) {
    int idx = page_to_index(phys_addr);
    if (idx >= 0 && page_refs[idx] < 0xFFFF) page_refs[idx]++;
}

void vmm_unref_page(uint32_t phys_addr) {
    int idx = page_to_index(phys_addr);
    if (idx >= 0 && page_refs[idx] > 0) {
        page_refs[idx]--;
        if (page_refs[idx] == 0) {
            pmm_free_page(phys_addr);
        }
    }
}

uint32_t vmm_get_ref(uint32_t phys_addr) {
    int idx = page_to_index(phys_addr);
    return (idx >= 0) ? page_refs[idx] : 0;
}

/* --------------------------------------------------------------------------
 * Shared Memory
 * -------------------------------------------------------------------------- */

static shmem_region_t shmem_regions[SHMEM_MAX_REGIONS];

int vmm_shmem_create(uint32_t key, uint32_t size) {
    /* Check if key already exists */
    for (int i = 0; i < SHMEM_MAX_REGIONS; i++) {
        if (shmem_regions[i].active && shmem_regions[i].key == key)
            return i; /* Already exists */
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < SHMEM_MAX_REGIONS; i++) {
        if (!shmem_regions[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* Calculate pages needed */
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > SHMEM_MAX_PAGES) return -1;

    /* Allocate physical pages */
    shmem_regions[slot].key = key;
    shmem_regions[slot].num_pages = pages;
    shmem_regions[slot].ref_count = 0;
    shmem_regions[slot].active = true;

    for (uint32_t p = 0; p < pages; p++) {
        uint32_t phys = pmm_alloc_page();
        if (phys == 0) {
            /* Cleanup on failure */
            for (uint32_t j = 0; j < p; j++)
                pmm_free_page(shmem_regions[slot].phys_pages[j]);
            shmem_regions[slot].active = false;
            return -1;
        }
        memset((void*)phys, 0, PAGE_SIZE);
        shmem_regions[slot].phys_pages[p] = phys;
    }

    return slot;
}

uint32_t vmm_shmem_attach(uint32_t key) {
    /* Find region */
    int slot = -1;
    for (int i = 0; i < SHMEM_MAX_REGIONS; i++) {
        if (shmem_regions[i].active && shmem_regions[i].key == key) {
            slot = i; break;
        }
    }
    if (slot < 0) return 0;

    /* Map pages into current address space */
    uint32_t base = SHMEM_BASE_ADDR + (uint32_t)slot * SHMEM_MAX_PAGES * PAGE_SIZE;
    uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    for (uint32_t p = 0; p < shmem_regions[slot].num_pages; p++) {
        paging_map_page(base + p * PAGE_SIZE, shmem_regions[slot].phys_pages[p], flags);
    }

    shmem_regions[slot].ref_count++;
    return base;
}

int vmm_shmem_detach(uint32_t key) {
    for (int i = 0; i < SHMEM_MAX_REGIONS; i++) {
        if (shmem_regions[i].active && shmem_regions[i].key == key) {
            /* Unmap pages */
            uint32_t base = SHMEM_BASE_ADDR + (uint32_t)i * SHMEM_MAX_PAGES * PAGE_SIZE;
            for (uint32_t p = 0; p < shmem_regions[i].num_pages; p++) {
                paging_unmap_page(base + p * PAGE_SIZE);
            }
            shmem_regions[i].ref_count--;

            /* Free region if no references */
            if (shmem_regions[i].ref_count == 0) {
                for (uint32_t p = 0; p < shmem_regions[i].num_pages; p++) {
                    pmm_free_page(shmem_regions[i].phys_pages[p]);
                }
                shmem_regions[i].active = false;
            }
            return 0;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Memory-Mapped Files
 * -------------------------------------------------------------------------- */

static mmap_region_t mmap_regions[MMAP_MAX_REGIONS];

uint32_t vmm_mmap(uint32_t vaddr, uint32_t size, uint32_t file_node_ptr) {
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (!mmap_regions[i].active) { slot = i; break; }
    }
    if (slot < 0) return 0;

    /* Use provided vaddr or assign default */
    if (vaddr == 0) {
        vaddr = MMAP_BASE_ADDR + (uint32_t)slot * 0x10000; /* 64KB apart */
    }

    /* Calculate pages needed */
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Allocate pages and load file data */
    fs_node_t* node = (fs_node_t*)file_node_ptr;
    uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    for (uint32_t p = 0; p < pages; p++) {
        uint32_t phys = pmm_alloc_page();
        if (phys == 0) return 0;
        memset((void*)phys, 0, PAGE_SIZE);

        /* Read file data into this page */
        if (node && node->read) {
            uint32_t offset = p * PAGE_SIZE;
            uint32_t to_read = PAGE_SIZE;
            if (offset + to_read > size) to_read = size - offset;
            if (offset < node->size) {
                node->read(node, offset, to_read, (uint8_t*)phys);
            }
        }

        paging_map_page(vaddr + p * PAGE_SIZE, phys, flags);
    }

    mmap_regions[slot].vaddr = vaddr;
    mmap_regions[slot].size = size;
    mmap_regions[slot].file_inode = node ? node->inode : 0;
    mmap_regions[slot].active = true;

    return vaddr;
}

/* --------------------------------------------------------------------------
 * COW Fork
 * --------------------------------------------------------------------------
 * Clone the current page directory. All writable user pages become
 * read-only in both parent and child. On write fault, the page is copied.
 * -------------------------------------------------------------------------- */

/* Access the current page directory (defined in paging.c) */
extern uint32_t* paging_get_directory(void);
extern uint32_t* paging_get_page_table(uint32_t pd_index);

uint32_t vmm_fork(void) {
    uint32_t* parent_pd = paging_get_directory();
    if (!parent_pd) return 0;

    /* Allocate new page directory */
    uint32_t child_pd_phys = pmm_alloc_page();
    if (child_pd_phys == 0) return 0;

    uint32_t* child_pd = (uint32_t*)child_pd_phys;
    memset(child_pd, 0, PAGE_SIZE);

    /* Copy page directory entries */
    for (int i = 0; i < 1024; i++) {
        if (!(parent_pd[i] & PAGE_PRESENT)) continue;

        if (i < 4) {
            /* Kernel pages (first 16MB): share directly, no COW */
            child_pd[i] = parent_pd[i];
            continue;
        }

        /* User pages: clone page table, mark pages COW */
        uint32_t* parent_pt = paging_get_page_table((uint32_t)i);
        if (!parent_pt) continue;

        /* Allocate new page table for child */
        uint32_t child_pt_phys = pmm_alloc_page();
        if (child_pt_phys == 0) continue;

        uint32_t* child_pt = (uint32_t*)child_pt_phys;

        for (int j = 0; j < 1024; j++) {
            if (!(parent_pt[j] & PAGE_PRESENT)) {
                child_pt[j] = 0;
                continue;
            }

            uint32_t phys = parent_pt[j] & 0xFFFFF000;

            /* Mark both parent and child as read-only + COW */
            parent_pt[j] &= ~PAGE_WRITABLE;
            parent_pt[j] |= PAGE_COW;

            child_pt[j] = (phys | (parent_pt[j] & 0xFFF));

            /* Increment reference count */
            vmm_ref_page(phys);
        }

        child_pd[i] = child_pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    /* Flush TLB since we modified parent page tables */
    __asm__ volatile("mov %0, %%cr3" : : "r"(parent_pd) : "memory");

    return child_pd_phys;
}

/* --------------------------------------------------------------------------
 * vmm_handle_cow: Handle a COW page fault
 * Called from the page fault handler when a write to a COW page occurs.
 * -------------------------------------------------------------------------- */
bool vmm_handle_cow(uint32_t fault_addr) {
    uint32_t* pd = paging_get_directory();
    uint32_t pd_idx = fault_addr >> 22;
    uint32_t pt_idx = (fault_addr >> 12) & 0x3FF;

    if (!(pd[pd_idx] & PAGE_PRESENT)) return false;

    uint32_t* pt = paging_get_page_table(pd_idx);
    if (!pt) return false;

    uint32_t entry = pt[pt_idx];
    if (!(entry & PAGE_PRESENT)) return false;
    if (!(entry & PAGE_COW)) return false;

    uint32_t old_phys = entry & 0xFFFFF000;
    uint32_t refs = vmm_get_ref(old_phys);

    if (refs <= 1) {
        /* We're the only user — just make it writable again */
        pt[pt_idx] = old_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        pt[pt_idx] &= ~PAGE_COW;
    } else {
        /* Copy the page */
        uint32_t new_phys = pmm_alloc_page();
        if (new_phys == 0) return false;

        memcpy((void*)new_phys, (void*)old_phys, PAGE_SIZE);
        vmm_unref_page(old_phys);

        pt[pt_idx] = new_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    /* Flush TLB for this address */
    __asm__ volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");

    return true;
}

/* --------------------------------------------------------------------------
 * vmm_init: Initialize VMM subsystem
 * -------------------------------------------------------------------------- */
void vmm_init(void) {
    memset(page_refs, 0, sizeof(page_refs));
    memset(shmem_regions, 0, sizeof(shmem_regions));
    memset(mmap_regions, 0, sizeof(mmap_regions));

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("VMM initialized (COW, shmem, mmap)\n");
}
