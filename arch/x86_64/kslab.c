/* Small fixed-size kernel objects packed into pages (POSIX timers, signal queue entries, signalfd/pidfd objects):
 * page_alloc gives one zeroed 4 KiB page per allocation, which made a queued signal or a timer cost a page each. A
 * kslab hands out `size`-byte slots from pages of at most 126 slots; an empty page goes back at once, so the boot
 * self-tests' page baselines still balance. Big kernel lock held by every caller (no lock of its own). */
#include "native.h"
#include "../../kernel/string.h"

struct kslab_page {
    struct kslab_page *next, *prev;
    struct kslab* owner;
    uint16_t used, capacity;
    uint32_t hint;           /* lowest slot that may be free */
    uint64_t bitmap[2];      /* allocated slots */
};
#define KSLAB_HEADER 48U
_Static_assert(sizeof(struct kslab_page) == KSLAB_HEADER, "kslab page header");

void* kslab_alloc(struct kslab* slab) {
    if (slab->size < 16 || slab->size > (4096 - KSLAB_HEADER) / 2 || (slab->size & 7)) panic("kslab object size");
    struct kslab_page* page = slab->partial;
    while (page && page->used == page->capacity) page = page->next;
    if (!page) {
        page = (void*)page_alloc(); if (!page) return NULL;
        unsigned capacity = (4096 - KSLAB_HEADER) / slab->size; if (capacity > 126) capacity = 126;
        page->owner = slab; page->capacity = (uint16_t)capacity;
        page->next = slab->partial; if (page->next) page->next->prev = page; slab->partial = page;
        slab->pages++;
    }
    for (unsigned i = page->hint; i < page->capacity; i++) {
        if (page->bitmap[i / 64] >> (i % 64) & 1) continue;
        page->bitmap[i / 64] |= 1ULL << (i % 64); page->used++; page->hint = i + 1; slab->objects++;
        void* object = (uint8_t*)page + KSLAB_HEADER + (uint64_t)i * slab->size;
        memset(object, 0, slab->size); return object;
    }
    panic("kslab page accounting");
}
void kslab_free(struct kslab* slab, void* object) {
    if (!object) return;
    struct kslab_page* page = (void*)((uint64_t)object & ~4095ULL);
    uint64_t offset = (uint64_t)object - (uint64_t)page - KSLAB_HEADER;
    unsigned i = (unsigned)(offset / slab->size);
    if (page->owner != slab || offset % slab->size || i >= page->capacity || !(page->bitmap[i / 64] >> (i % 64) & 1))
        panic("kslab free of a foreign object");
    page->bitmap[i / 64] &= ~(1ULL << (i % 64)); page->used--; slab->objects--;
    if (i < page->hint) page->hint = i;
    memset(object, 0, slab->size);
    if (page->used) return;
    if (page->prev) page->prev->next = page->next; else slab->partial = page->next;
    if (page->next) page->next->prev = page->prev;
    slab->pages--; page_free((uint64_t)page);
}
/* Variable-size small objects (System V messages and semaphore arrays, Landlock rules): power-of-two size classes up to
 * 2016 bytes on kslabs, larger ones from the arena. kfree needs the size the object was allocated with. */
static struct kslab classes[] = {{.size = 32}, {.size = 64}, {.size = 128}, {.size = 256}, {.size = 504}, {.size = 1008}, {.size = 2016}};
#define CLASSES (sizeof(classes) / sizeof(classes[0]))
void* kalloc(uint64_t bytes) {
    for (unsigned i = 0; i < CLASSES; i++) if (bytes <= classes[i].size) return kslab_alloc(&classes[i]);
    void* object = (void*)arena_alloc(bytes); if (object) memset(object, 0, bytes);
    return object;
}
void kfree(void* object, uint64_t bytes) {
    if (!object) return;
    for (unsigned i = 0; i < CLASSES; i++) if (bytes <= classes[i].size) { kslab_free(&classes[i], object); return; }
    arena_free((uint64_t)object, bytes);
}
