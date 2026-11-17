/* Physical page ownership survives individual process exits and supports COW. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "boot_info.h"
#include "swap.h"

#define PAGE 4096ULL
#define PRESENT 1ULL
#define WRITE 2ULL
#define LARGE 128ULL
#define NX (1ULL << 63)
#define ADDRESS 0x000FFFFFFFFFF000ULL
#define LOW_LIMIT 0x100000000ULL      /* DMA32 boundary; the boot direct map covers [0, LOW_LIMIT) */
#define PHYSICAL_LIMIT (1ULL << 46)   /* direct map: PML4 entries 256-383 */
#define MAX_RANGES 32
#define MAX_ZONES (2 * MAX_RANGES) /* a range above 4 GiB becomes two zones (memory_init) */
/* One zone per usable RAM range (boot_info: E820 type 1 or UEFI free memory) at or above 16 MiB,
 * split at 4 GiB so that dma32_alloc can name the zones below it. Each zone keeps its reference
 * counts in its own first pages. The low zones are reached through the boot direct map; memory_init
 * maps the zones above 4 GiB with 4 KiB pages (tables mostly from high memory). Zones are sorted by address;
 * allocations prefer the highest zone, so memory below 4 GiB stays free for devices. */
/* aux: one word per frame for the swap cache's reverse map (swap.c, under its lock). min/low/high: watermarks in
 * free pages (reclaim.c): user-page allocations keep `min` free, falling below `low` signals memory pressure. */
struct zone { uint64_t begin, end; uint32_t* references; uint32_t* aux; uint32_t pages, reserved, hint, used, min, low, high; };
static struct zone zones[MAX_ZONES];
static unsigned zone_count;
static uint64_t allocated, managed;
static volatile int pmm_lock; /* references[], used, allocated, hints: page faults allocate without the big lock */
static volatile bool pressure; /* a zone fell below its low watermark (cleared by reclaim.c) */
extern uint64_t boot_pml4[], boot_pd[];
extern uint8_t __text_start[], __bss_end[];

static void zone_init(uint64_t begin, uint64_t end) {
    uint64_t pages = (end - begin) / PAGE;
    if (pages > 0xFFFFFFF0ULL) pages = 0xFFFFFFF0ULL;
    uint32_t reserved = (uint32_t)((pages * 2 * sizeof(uint32_t) + PAGE - 1) / PAGE); /* references + aux */
    if (zone_count == MAX_ZONES || pages <= reserved + 64) return;
    struct zone* z = &zones[zone_count++];
    *z = (struct zone){.begin = begin, .end = begin + pages * PAGE, .references = phys_to_virt(begin), .pages = (uint32_t)pages,
                       .reserved = reserved, .hint = reserved};
    z->aux = z->references + pages;
    memset(z->references, 0, (uint64_t)reserved * PAGE);
    for (uint32_t i = 0; i < reserved; i++) z->references[i] = 0xFFFFFFFFU;
    managed += pages - reserved;
}
static uint64_t* direct_table(uint64_t* table, unsigned index) {
    if (!(table[index] & PRESENT)) { uint64_t frame = frame_alloc(); if (!frame) return NULL; table[index] = frame | PRESENT | WRITE; }
    return table[index] & LARGE ? NULL : phys_to_virt(table[index] & ADDRESS);
}
/* RAM above 4 GiB: 4 KiB read/write/no-execute pages (kernel stacks put guard pages there). Runs before
 * any address space exists, so every space copies the new top-level entries. */
static bool direct_map(uint64_t begin, uint64_t end) {
    for (uint64_t at = begin; at < end;) {
        uint64_t va = DIRECT_MAP_BASE + at;
        uint64_t* pdpt = direct_table(boot_pml4, (unsigned)(va >> 39) & 511);
        uint64_t* directory = pdpt ? direct_table(pdpt, (unsigned)(va >> 30) & 511) : NULL;
        uint64_t* table = directory ? direct_table(directory, (unsigned)(va >> 21) & 511) : NULL;
        if (!table) return false;
        for (unsigned i = (unsigned)(va >> 12) & 511; i < 512 && at < end; i++, at += PAGE) table[i] = at | PRESENT | WRITE | NX;
    }
    return true;
}
void memory_init(void) {
    if (!boot_info.memory_count) panic("missing usable memory map");
    struct { uint64_t begin, end; } ranges[MAX_RANGES]; unsigned count = 0;
    for (unsigned i = 0; i < boot_info.memory_count; i++) {
        const struct nexus_boot_range* range = &boot_info.memory[i];
        if (range->size > ~0ULL - range->base) continue;
        uint64_t begin = range->base, end = begin + range->size;
        if (begin < 0x1000000) begin = 0x1000000;
        if (end > PHYSICAL_LIMIT) end = PHYSICAL_LIMIT;
        begin = (begin + PAGE - 1) & ~(PAGE - 1); end &= ~(PAGE - 1);
        for (unsigned high = 0; high < 2; high++) {
            uint64_t b = high ? (begin > LOW_LIMIT ? begin : LOW_LIMIT) : begin, e = high ? end : (end < LOW_LIMIT ? end : LOW_LIMIT);
            if (e <= b || e - b < 256 * PAGE) continue;
            unsigned at = count;
            if (count == MAX_RANGES) { /* keep the largest ranges */
                at = 0; for (unsigned j = 1; j < count; j++) if (ranges[j].end - ranges[j].begin < ranges[at].end - ranges[at].begin) at = j;
                if (ranges[at].end - ranges[at].begin >= e - b) continue;
            } else count++;
            ranges[at].begin = b; ranges[at].end = e;
        }
    }
    for (unsigned i = 1; i < count; i++) /* ascending addresses (a replaced slot may be out of order) */
        for (unsigned j = i; j && ranges[j - 1].begin > ranges[j].begin; j--) { __typeof__(ranges[0]) t = ranges[j]; ranges[j] = ranges[j - 1]; ranges[j - 1] = t; }
    for (unsigned i = 0; i < count; i++) if (ranges[i].end <= LOW_LIMIT) zone_init(ranges[i].begin, ranges[i].end);
    if (!managed) panic("insufficient physical memory");
    uint64_t low = managed;
    for (unsigned i = 0; i < count; i++) {
        if (ranges[i].end <= LOW_LIMIT) continue;
        /* A leading piece (1/256 of the range, at least 1 GiB) is mapped with page tables from the zones below
         * and becomes a zone; the rest of the range takes its tables from it, sparing DMA32 memory. */
        uint64_t size = ranges[i].end - ranges[i].begin, lead = size / 256 > (1ULL << 30) ? size / 256 : 1ULL << 30;
        lead = (lead + 0x1FFFFF) & ~0x1FFFFFULL;
        uint64_t split = lead < size && size - lead >= 256 * PAGE ? ranges[i].begin + lead : ranges[i].end;
        for (uint64_t at = ranges[i].begin, stop = split; at < ranges[i].end; at = stop, stop = ranges[i].end) {
            if (zone_count == MAX_ZONES) { vga_print("[64] E820 zone table full: RAM above 4 GiB dropped\n"); break; } /* no tables for it */
            if (!direct_map(at, stop)) { vga_print("[64] E820 no memory to map RAM above 4 GiB\n"); break; }
            zone_init(at, stop);
        }
    }
    /* Low zones: the boot map's 2 MiB pages become 4 KiB ones, like the zones above (kernel stack guard pages). */
    for (unsigned zi = 0; zi < zone_count && zones[zi].end <= LOW_LIMIT; zi++)
        for (uint64_t i = zones[zi].begin >> 21; i < (zones[zi].end + 0x1FFFFF) >> 21; i++) {
            if (!(boot_pd[i] & LARGE)) continue;
            uint64_t frame = frame_alloc(); if (!frame) panic("kernel stack page tables");
            uint64_t* table = phys_to_virt(frame);
            for (unsigned j = 0; j < 512; j++) table[j] = (i << 21) + (uint64_t)j * PAGE + PRESENT + WRITE + NX;
            boot_pd[i] = frame | PRESENT | WRITE | NX;
        }
    __asm__ volatile("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
    char number[24];
    vga_print("[64] E820 managed physical pages="); number64(managed, number); vga_print(number);
    vga_print(" zones="); number64(zone_count, number); vga_print(number);
    vga_print(" above_4g_pages="); number64(managed - low, number); vga_print(number); vga_print("\n");
}
/* Kernel stacks: a page of the direct map becomes (or stops being) a guard page. */
void kernel_guard_page(uint64_t address, bool guard) {
    if ((address & (PAGE - 1)) || address >= KERNEL_VMA || !memory_kernel_buffer((void*)address, PAGE)) panic("invalid kernel guard page");
    uint64_t* table = boot_pml4;
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t entry = table[(address >> shift) & 511];
        if (!(entry & PRESENT) || (entry & LARGE)) panic("missing kernel stack page table");
        table = phys_to_virt(entry & ADDRESS);
    }
    table[(address >> 12) & 511] = virt_to_phys((void*)address) | WRITE | NX | (guard ? 0 : PRESENT);
    __asm__ volatile("invlpg (%0)" :: "r"(address) : "memory");
    if (guard) smp_tlb_shootdown(0, false); /* every CPU must fault on the new guard page */
}
uint64_t memory_used_pages(void) { return allocated; }
uint64_t memory_total_pages(void) { return managed; }
bool memory_kernel_buffer(const void* buffer, uint64_t bytes) {
    uint64_t address = (uint64_t)buffer;
    if (!bytes || address > ~0ULL - bytes) return false;
    if (address >= (uint64_t)__text_start && address + bytes <= (uint64_t)__bss_end) return true;
    if (address < DIRECT_MAP_BASE || address >= DIRECT_MAP_BASE + PHYSICAL_LIMIT) return false;
    uint64_t physical = address - DIRECT_MAP_BASE;
    for (unsigned i = 0; i < zone_count; i++)
        if (physical >= zones[i].begin + (uint64_t)zones[i].reserved * PAGE && physical + bytes <= zones[i].end) return true;
    return false;
}
/* Zeroed physically contiguous pages ending at or below `limit`, highest zone first: physical address or 0. */
static uint64_t zone_alloc(uint64_t bytes, uint64_t limit, bool user) {
    if (!bytes || bytes > ~0ULL - PAGE + 1) return 0;
    uint64_t amount = (bytes + PAGE - 1) / PAGE;
    if (amount > managed - allocated || amount > 0xFFFFFFFFULL) { pressure = true; return 0; }
    uint32_t needed = (uint32_t)amount;
    spin_acquire(&pmm_lock);
    for (unsigned zi = zone_count; zi--;) {
        struct zone* z = &zones[zi];
        uint32_t available = z->pages - z->reserved - z->used;
        if (z->end > limit || available < needed || (user && available - needed < z->min)) continue;
        if (available - needed < z->low) pressure = true;
        for (unsigned pass = 0; pass < 2; pass++) {
            uint32_t start = pass ? z->reserved : z->hint;
            for (uint32_t i = start; i + needed <= z->pages;) {
                uint32_t free = 0;
                while (free < needed && !z->references[i + free]) free++;
                if (free == needed) {
                    for (uint32_t n = 0; n < needed; n++) z->references[i + n] = 1;
                    allocated += needed; z->used += needed; z->hint = i + needed;
                    if (z->hint == z->pages) z->hint = z->reserved;
                    spin_release(&pmm_lock);
                    uint64_t address = z->begin + (uint64_t)i * PAGE;
                    memset(phys_to_virt(address), 0, amount * PAGE); return address;
                }
                i += free + 1;
            }
        }
    }
    pressure = true;
    spin_release(&pmm_lock);
    return 0;
}
uint64_t arena_alloc(uint64_t bytes) { uint64_t p = zone_alloc(bytes, ~0ULL, false); return p ? (uint64_t)phys_to_virt(p) : 0; }
uint64_t dma32_alloc(uint64_t bytes) { uint64_t p = zone_alloc(bytes, LOW_LIMIT, false); return p ? (uint64_t)phys_to_virt(p) : 0; }
uint64_t page_alloc(void) { return arena_alloc(PAGE); }
uint64_t frame_alloc(void) { return zone_alloc(PAGE, ~0ULL, false); }
/* ---- reclaim support (swap.h) ---- */
uint64_t frame_alloc_user(void) { return zone_alloc(PAGE, ~0ULL, true); }
bool memory_pressure(void) { return __atomic_load_n(&pressure, __ATOMIC_RELAXED); }
void memory_pressure_clear(void) { __atomic_store_n(&pressure, false, __ATOMIC_RELAXED); }
uint64_t memory_free_pages(void) { return managed - __atomic_load_n(&allocated, __ATOMIC_RELAXED); }
unsigned memory_zone_count(void) { return zone_count; }
bool memory_zone_get(unsigned index, struct memory_zone_info* info) {
    if (index >= zone_count) return false;
    const struct zone* z = &zones[index];
    *info = (struct memory_zone_info){z->begin, z->end, z->pages - z->reserved, z->pages - z->reserved - z->used, z->min, z->low, z->high};
    return true;
}
void memory_zone_watermarks(unsigned index, uint32_t min, uint32_t low, uint32_t high) {
    if (index >= zone_count) return;
    spin_acquire(&pmm_lock); zones[index].min = min; zones[index].low = low; zones[index].high = high; spin_release(&pmm_lock);
}
/* Zone of a managed frame (physical or direct-map address), -1 otherwise; its swap-cache word. */
int memory_frame_zone(uint64_t address) {
    if (address >= DIRECT_MAP_BASE && address < DIRECT_MAP_BASE + PHYSICAL_LIMIT) address -= DIRECT_MAP_BASE;
    for (unsigned i = 0; i < zone_count; i++)
        if (address >= zones[i].begin + (uint64_t)zones[i].reserved * PAGE && address < zones[i].end) return (int)i;
    return -1;
}
uint32_t* memory_frame_aux(uint64_t address) {
    if (address >= DIRECT_MAP_BASE && address < DIRECT_MAP_BASE + PHYSICAL_LIMIT) address -= DIRECT_MAP_BASE;
    int zi = memory_frame_zone(address); if (zi < 0) return NULL;
    return &zones[zi].aux[(address - zones[zi].begin) / PAGE];
}
/* A page by physical address or by its direct-map address. */
static uint32_t page_index(uint64_t address, struct zone** owner) {
    if (address >= DIRECT_MAP_BASE && address < DIRECT_MAP_BASE + PHYSICAL_LIMIT) address -= DIRECT_MAP_BASE;
    struct zone* z = NULL;
    for (unsigned i = 0; i < zone_count && !z; i++) if (address >= zones[i].begin && address < zones[i].end) z = &zones[i];
    if (!z || address < z->begin + (uint64_t)z->reserved * PAGE || (address & (PAGE - 1))) panic("invalid physical page");
    uint32_t index = (uint32_t)((address - z->begin) / PAGE);
    if (!z->references[index] || z->references[index] == 0xFFFFFFFFU) panic("unowned physical page");
    *owner = z; return index;
}
uint32_t page_references(uint64_t address) { struct zone* z; uint32_t index = page_index(address, &z); return z->references[index]; }
void page_retain(uint64_t address) {
    spin_acquire(&pmm_lock);
    struct zone* z; uint32_t index = page_index(address, &z);
    if (z->references[index] == 0xFFFFFFFEU) panic("physical page reference overflow");
    z->references[index]++;
    spin_release(&pmm_lock);
}
void page_free(uint64_t address) {
    spin_acquire(&pmm_lock);
    struct zone* z; uint32_t index = page_index(address, &z);
    if (!--z->references[index]) { allocated--; z->used--; if (index < z->hint) z->hint = index; }
    spin_release(&pmm_lock);
}
/* ---- mm.audit (debugging, vm_reclaim.inc): frames by one global index, raw reference counts (no panic) ---- */
uint64_t memory_frame_slots(void) { uint64_t n = 0; for (unsigned i = 0; i < zone_count; i++) n += zones[i].pages; return n; }
int64_t memory_frame_slot(uint64_t address, uint32_t* references) {
    uint64_t base = 0;
    for (unsigned i = 0; i < zone_count; i++) {
        const struct zone* z = &zones[i];
        if (address >= z->begin + (uint64_t)z->reserved * PAGE && address < z->end) {
            uint64_t index = (address - z->begin) / PAGE;
            if (references) *references = z->references[index];
            return (int64_t)(base + index);
        }
        base += z->pages;
    }
    return -1;
}
uint64_t memory_slot_frame(uint64_t slot) {
    for (unsigned i = 0; i < zone_count; i++) { if (slot < zones[i].pages) return zones[i].begin + slot * PAGE; slot -= zones[i].pages; }
    return 0;
}
void arena_free(uint64_t address, uint64_t bytes) {
    if (!address) return;
    if (!bytes || bytes > ~0ULL - PAGE + 1) panic("invalid allocation size");
    uint64_t count = (bytes + PAGE - 1) / PAGE;
    for (uint64_t i = 0; i < count; i++) page_free(address + i * PAGE);
}
void memory_selftest(void) {
    uint64_t before = allocated, shared = page_alloc();
    if (!shared || *(uint64_t*)shared) panic("physical allocation/zeroing test");
    *(uint64_t*)shared = 0xC0FFEE1234567890ULL;
    page_retain(shared); page_free(virt_to_phys((void*)shared)); /* both address forms name the page */
    uint64_t separate = arena_alloc(2 * PAGE);
    if (!separate || page_references(shared) != 1 || allocated != before + 3 ||
        *(uint64_t*)shared != 0xC0FFEE1234567890ULL || *(uint64_t*)separate || *(uint64_t*)(separate + PAGE))
        panic("shared physical page lifetime test");
    /* DMA32 pages lie below 4 GiB; ordinary pages come from above it when such RAM exists. */
    uint64_t low = dma32_alloc(PAGE), frame = frame_alloc();
    bool high = zones[zone_count - 1].begin >= LOW_LIMIT;
    if (!low || virt_to_phys((void*)low) >= LOW_LIMIT || !frame || *(uint64_t*)phys_to_virt(frame) ||
        (high && (virt_to_phys((void*)shared) < LOW_LIMIT || frame < LOW_LIMIT)) || !memory_kernel_buffer((void*)low, PAGE) ||
        memory_kernel_buffer(phys_to_virt(0x1000), 8)) panic("DMA32/high-memory zone selection test");
    page_free(shared); arena_free(separate, 2 * PAGE); page_free(low); page_free(frame);
    if (allocated != before || arena_alloc(~0ULL) || arena_alloc((managed + 1) * PAGE)) panic("physical allocation rollback test");
    vga_print("[64] PASS physical page ownership/refcount cleanup\n");
}
/* Hibernation (hibernate.c): the page pool zone by zone (index < returned count), and claiming one given free page
 * (restoring an image puts its pages back at their physical addresses). */
unsigned memory_zone_view(unsigned index, uint64_t* begin, uint64_t* end, const uint32_t** references) {
    if (index >= zone_count) return 0;
    *begin = zones[index].begin; *end = zones[index].end; *references = zones[index].references;
    return zone_count;
}
bool memory_page_claim(uint64_t physical) {
    bool claimed = false;
    spin_acquire(&pmm_lock);
    for (unsigned i = 0; i < zone_count; i++) {
        struct zone* z = &zones[i];
        if (physical < z->begin + (uint64_t)z->reserved * PAGE || physical >= z->end || (physical & (PAGE - 1))) continue;
        uint32_t index = (uint32_t)((physical - z->begin) / PAGE);
        if (!z->references[index]) { z->references[index] = 1; allocated++; z->used++; claimed = true; }
        break;
    }
    spin_release(&pmm_lock);
    return claimed;
}
