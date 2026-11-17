/* Counted allocations of the storage files (storage_util.h). */
#define STORAGE_ALLOCATOR
#include "native.h"
#include "storage_util.h"

static uint64_t pages;
static uint64_t page_count(uint64_t bytes) { return (bytes + 4095) / 4096; }
uint64_t storage_alloc(uint64_t bytes) { uint64_t p = arena_alloc(bytes); if (p) pages += page_count(bytes); return p; }
uint64_t storage_dma32_alloc(uint64_t bytes) { uint64_t p = dma32_alloc(bytes); if (p) pages += page_count(bytes); return p; }
void storage_free(uint64_t address, uint64_t bytes) {
    if (!address) return;
    arena_free(address, bytes);
    uint64_t n = page_count(bytes);
    if (n > pages) panic("storage page accounting underflow");
    pages -= n;
}
uint64_t storage_background_pages(void) { return pages; }
