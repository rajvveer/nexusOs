/* ============================================================================
 * NexusOS — Virtual Memory Manager (Header) — Phase 21
 * ============================================================================
 * COW fork, shared memory, memory-mapped files, page reference counting.
 * ============================================================================ */

#ifndef VMM_H
#define VMM_H

#include "types.h"

/* Shared memory limits */
#define SHMEM_MAX_REGIONS  16
#define SHMEM_MAX_PAGES    16   /* Max 64KB per shared region */
#define SHMEM_BASE_ADDR    0x00D00000  /* 13MB — shared memory area */

/* Memory-mapped file limits */
#define MMAP_MAX_REGIONS   8
#define MMAP_BASE_ADDR     0x00E00000  /* 14MB — mmap area */

/* Shared memory region */
typedef struct {
    uint32_t key;           /* Unique identifier */
    uint32_t phys_pages[SHMEM_MAX_PAGES];
    uint32_t num_pages;
    uint32_t ref_count;     /* Number of processes attached */
    bool     active;
} shmem_region_t;

/* Memory-mapped file region */
typedef struct {
    uint32_t vaddr;
    uint32_t size;
    uint32_t file_inode;    /* VFS node inode */
    bool     active;
} mmap_region_t;

/* Initialize VMM subsystem */
void vmm_init(void);

/* COW fork: clone current page directory with COW semantics.
 * Returns new page directory physical address, or 0 on failure. */
uint32_t vmm_fork(void);

/* Shared memory operations */
int      vmm_shmem_create(uint32_t key, uint32_t size);
uint32_t vmm_shmem_attach(uint32_t key);
int      vmm_shmem_detach(uint32_t key);

/* Memory-mapped files */
uint32_t vmm_mmap(uint32_t vaddr, uint32_t size, uint32_t file_node);

/* Page reference counting */
void     vmm_ref_page(uint32_t phys_addr);
void     vmm_unref_page(uint32_t phys_addr);
uint32_t vmm_get_ref(uint32_t phys_addr);

/* Handle COW page fault. Returns true if handled. */
bool     vmm_handle_cow(uint32_t fault_addr);

#endif /* VMM_H */
