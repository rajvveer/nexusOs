/* Small-object allocator for the network stack (interfaces, routes, neighbours, addresses, handles, fragments).
 * arena_alloc() hands out whole pages and the boot self-tests compare exact page counts across campaigns while
 * routers advertise and neighbours come and go, so the stack draws from a reserve allocated at boot (before the
 * kernel page baseline). Objects live in pages of one size class (a 16-byte header, then equal slots on a free
 * list); when the reserve is exhausted further pages come from the system and are reported as background pages
 * (native_network_background_pages) until they empty again. Objects above 2 KiB use arena_alloc directly. */
#include "netdev.h"
#include "../../kernel/string.h"

#define RESERVE_PAGES 192U
#define CLASSES 7
static const unsigned class_size[CLASSES] = {32, 64, 128, 256, 512, 1024, 2048};
struct slot { struct slot* next; };
struct page_header { uint32_t magic, size_class, used, reserve; struct page_header* next_free_page; uint64_t pad; };
_Static_assert(sizeof(struct page_header) == 32, "network slab header");
#define MAGIC 0x4E45544DU
static struct slot* free_slots[CLASSES];
static uint8_t* reserve_base;
static unsigned reserve_next, grown_pages;
static uint64_t objects_live;
static unsigned class_for(unsigned size) { for (unsigned c = 0; c < CLASSES; c++) if (size <= class_size[c]) return c; return CLASSES; }
static bool page_carve(unsigned c) {
    uint8_t* page = NULL; bool reserve = false;
    if (!reserve_base) { reserve_base = (void*)arena_alloc((uint64_t)RESERVE_PAGES * 4096); reserve_next = 0; }
    if (reserve_base && reserve_next < RESERVE_PAGES) { page = reserve_base + (uint64_t)reserve_next++ * 4096; reserve = true; }
    else { page = (void*)page_alloc(); if (!page) return false; grown_pages++; }
    struct page_header* h = (void*)page; h->magic = MAGIC; h->size_class = c; h->used = 0; h->reserve = reserve;
    for (unsigned offset = sizeof(*h); offset + class_size[c] <= 4096; offset += class_size[c]) {
        struct slot* s = (void*)(page + offset); s->next = free_slots[c]; free_slots[c] = s;
    }
    return true;
}
void netmem_init(void) {
    if (!reserve_base) reserve_base = (void*)arena_alloc((uint64_t)RESERVE_PAGES * 4096);
    if (!reserve_base) panic("network memory reserve");
}
void* net_alloc(unsigned size) {
    unsigned c = class_for(size);
    if (c == CLASSES) { void* p = (void*)arena_alloc(size); if (p) { objects_live++; grown_pages += (size + 4095) / 4096; } return p; }
    if (!free_slots[c] && !page_carve(c)) return NULL;
    struct slot* s = free_slots[c]; free_slots[c] = s->next;
    struct page_header* h = (void*)((uint64_t)s & ~4095ULL); h->used++;
    memset(s, 0, class_size[c]); objects_live++;
    return s;
}
void net_free(void* p, unsigned size) {
    if (!p) return;
    unsigned c = class_for(size);
    objects_live--;
    if (c == CLASSES) { arena_free((uint64_t)p, size); grown_pages -= (size + 4095) / 4096; return; }
    struct page_header* h = (void*)((uint64_t)p & ~4095ULL);
    if (h->magic != MAGIC || h->size_class != c || !h->used) panic("network slab: invalid free");
    struct slot* s = p; s->next = free_slots[c]; free_slots[c] = s; h->used--;
    if (!h->used && !h->reserve) {
        /* an empty grown page goes back: unlink its slots from the free list first */
        for (struct slot** link = &free_slots[c]; *link;) {
            if (((uint64_t)*link & ~4095ULL) == (uint64_t)h) *link = (*link)->next; else link = &(*link)->next;
        }
        h->magic = 0; page_free((uint64_t)h); grown_pages--;
    }
}
uint64_t net_background_pages(void) { return grown_pages; }
uint64_t net_objects_live(void) { return objects_live; }
