/* Linux-compatible swap areas: swapon(2)/swapoff(2), the mkswap v1 header ("SWAPSPACE2", bad-page list, UUID/label),
 * swap partitions (any block-layer partition, /dev/<name>) and swap files on the root filesystem (FAT32 cluster chains,
 * ext4 extents; mapped once at swapon, holes refused), per-slot reference counts, the swap cache, clustered page I/O
 * through the generic block layer (block_part_read/write) and /proc/swaps. See swap.h for entries and locking. */
#include "native.h"
#include "swap.h"
#include "blockdev.h"
#include "vfs.h"
#include "ext4.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define PAGE 4096ULL
#define SECTORS_PER_PAGE 8ULL
#define SLOTS_PER_CHUNK 512ULL               /* 8-byte slot records in one page */
#define IO_PAGES 64ULL                       /* one transfer: 256 KiB through the swap I/O buffer */
#define READ_AHEAD 8ULL                      /* aligned read-ahead window (Linux page-cluster 3) */
#define SLOT_BAD 0x01                        /* header, bad-page list, or not one contiguous run on the device */
#define SLOT_DIRTY 0x02                      /* the cached frame is newer than the slot on disk */
#define SLOT_WRITEBACK 0x04                  /* a write of this slot is in flight (the slot cannot be reused) */
#define SLOT_READING 0x08                    /* a read of this slot is in flight */
#define COUNT_MAX 0xFFFEU
#define SWAP_FLAG_PREFER 0x8000U
#define SWAP_FLAG_PRIO_MASK 0x7FFFU
#define SWAP_FLAG_DISCARD 0x10000U
#define SWAP_FLAGS_VALID (SWAP_FLAG_PRIO_MASK | SWAP_FLAG_PREFER | SWAP_FLAG_DISCARD | 0x20000U | 0x40000U)
#define MAX_BADPAGES ((PAGE - 1024 - 512 - 10) / 4)
#define MAX_EXTENTS 65536ULL
#define CAP_SYS_ADMIN 21

int kernel_user_path(uint64_t address, char* path);   /* kernel.c */
fat32_file_t* kernel_file_disk(struct open_file* file);

struct swap_slot { uint16_t count; uint8_t flags, unused; uint32_t frame; }; /* frame: pfn of the cached copy, 0 = none */
_Static_assert(sizeof(struct swap_slot) * SLOTS_PER_CHUNK == PAGE, "swap slot chunk");
struct swap_extent { uint64_t offset, sector, sectors; };                   /* file bytes [offset, +sectors*512) at sector */
struct swap_area {
    bool used, writeok, partition, unusing, reserved;   /* reserved: swapon is setting it up (the I/O may sleep) */
    int priority; unsigned flags;
    uint64_t pages, good, free, cached, errors;  /* slots in the header's range, usable, allocatable, cached; I/O errors */
    uint64_t next;                               /* allocation cursor */
    struct swap_slot** chunks; uint16_t* chunk_free; uint16_t* chunk_cached; uint64_t chunk_count;
    struct swap_extent* extents; uint64_t extent_count, extent_bytes;
    const struct block_partition* part;
    struct memory_file* cache;                   /* swap file: its page cache (keeps the inode live, blocks writes) */
    uint64_t inode;
    uint8_t uuid[16]; char label[17];
    char path[FAT32_PATH_MAX];
};
static struct swap_area areas[SWAP_MAX_AREAS];
static volatile int swap_lock;
static int least_priority = -1;                  /* Linux: areas without SWAP_FLAG_PREFER get -2, -3, ... */
static unsigned rotor;                           /* round robin among areas of equal priority */
static uint8_t* io_buffer; static uint64_t io_pages;
static volatile bool io_busy; static int io_channel, read_channel;
static uint64_t total_cached, stat_in, stat_out, stat_hits, stat_ahead;
static unsigned cursor_type; static uint64_t cursor_offset = 1;   /* write-back sweep position */
/* mm.audit (debugging): each slot's page checksum at swap-out; compared at every read and cache hit. */
static uint32_t* slot_sum[SWAP_MAX_AREAS]; static uint64_t slot_sum_pages[SWAP_MAX_AREAS];
static uint32_t frame_sum(uint64_t frame) { return audit_sum(phys_to_virt(frame), PAGE); }
static void sum_check(const char* what, unsigned type, uint64_t offset, uint64_t frame) {
    if (!slot_sum[type] || offset >= slot_sum_pages[type] || !slot_sum[type][offset]) return;
    uint32_t now = frame_sum(frame);
    if (now != slot_sum[type][offset]) { audit_report(what, NULL, type, offset, frame, (uint64_t)slot_sum[type][offset] << 32 | now); slot_sum[type][offset] = now; }
}
static void sum_set(unsigned type, uint64_t offset, uint64_t frame) {
    if (slot_sum[type] && offset < slot_sum_pages[type]) slot_sum[type][offset] = frame_sum(frame);
}

static inline uint32_t aux_encode(unsigned type, uint64_t offset) { return (uint32_t)(((uint64_t)type << 27 | offset) + 1); }
static inline struct swap_slot* slot_of(struct swap_area* a, uint64_t offset) { return &a->chunks[offset / SLOTS_PER_CHUNK][offset % SLOTS_PER_CHUNK]; }
static inline bool slot_free(const struct swap_slot* s) { return !s->count && !s->frame && !s->flags; }
static inline uint64_t frame_of(const struct swap_slot* s) { return (uint64_t)s->frame << 12; }
/* Keeps the allocatable counters in step with a slot whose state was `was_free` before a change. */
static void slot_changed(struct swap_area* a, uint64_t offset, bool was_free) {
    bool now = slot_free(slot_of(a, offset));
    if (now == was_free) return;
    if (now) { a->free++; a->chunk_free[offset / SLOTS_PER_CHUNK]++; } else { a->free--; a->chunk_free[offset / SLOTS_PER_CHUNK]--; }
}
/* An entry's area and slot (swap_lock held); NULL for a malformed or stale entry. */
static struct swap_area* entry_area(uint64_t entry, uint64_t* offset) {
    if (!swap_entry_is(entry)) return NULL;
    unsigned type = swap_entry_type(entry); uint64_t at = swap_entry_offset(entry);
    if (type >= SWAP_MAX_AREAS || !areas[type].used || !at || at >= areas[type].pages) return NULL;
    *offset = at; return &areas[type];
}
/* The frame's allocation reference becomes the cache's. */
static void cache_add(struct swap_area* a, uint64_t offset, uint64_t frame) {
    struct swap_slot* s = slot_of(a, offset); uint32_t* aux = memory_frame_aux(frame);
    if (s->frame || !aux || *aux) panic("swap cache insert of a cached slot or frame");
    s->frame = (uint32_t)(frame >> 12); *aux = aux_encode((unsigned)(a - areas), offset);
    a->cached++; a->chunk_cached[offset / SLOTS_PER_CHUNK]++; total_cached++;
}
/* Drops the cached copy and the cache's frame reference. */
static void cache_remove(struct swap_area* a, uint64_t offset) {
    struct swap_slot* s = slot_of(a, offset); uint64_t frame = frame_of(s);
    uint32_t* aux = s->frame ? memory_frame_aux(frame) : NULL;
    if (!aux || *aux != aux_encode((unsigned)(a - areas), offset)) panic("swap cache entry without its frame");
    *aux = 0; s->frame = 0; s->flags &= ~SLOT_DIRTY;
    a->cached--; a->chunk_cached[offset / SLOTS_PER_CHUNK]--; total_cached--;
    page_free(frame);
}

/* ---- device addressing ---- */
static const struct swap_extent* extent_at(const struct swap_area* a, uint64_t byte) {
    uint64_t lo = 0, hi = a->extent_count;
    if (!hi) return NULL;
    while (hi - lo > 1) { uint64_t mid = (lo + hi) / 2; if (a->extents[mid].offset <= byte) lo = mid; else hi = mid; }
    return &a->extents[lo];
}
static uint64_t page_sector(const struct swap_area* a, uint64_t offset) {
    if (a->partition) return offset * SECTORS_PER_PAGE;
    const struct swap_extent* e = extent_at(a, offset * PAGE);
    return e->sector + (offset * PAGE - e->offset) / 512;
}
/* A page inside one run of the file that starts on a device block; any other slot is unusable. */
static bool page_mapped(const struct swap_area* a, uint64_t offset) {
    if (a->partition) return true;
    uint64_t byte = offset * PAGE; const struct swap_extent* e = extent_at(a, byte);
    if (!e || byte < e->offset || byte + PAGE > e->offset + e->sectors * 512) return false;
    uint64_t per = a->part->dev->block_size / 512, sector = e->sector + (byte - e->offset) / 512;
    return !((a->part->start * per + sector) % per) && sector + SECTORS_PER_PAGE <= block_part_sectors(a->part);
}
static bool contiguous(const struct swap_area* a, uint64_t offset) { return page_sector(a, offset + 1) == page_sector(a, offset) + SECTORS_PER_PAGE; }
/* Pages [offset, offset + count) form one contiguous run on the device. */
static int area_io(struct swap_area* a, uint64_t offset, uint64_t count, void* buffer, bool write) {
    uint64_t sector = page_sector(a, offset);
    return write ? block_part_write(a->part, sector, (uint32_t)(count * SECTORS_PER_PAGE), buffer)
                 : block_part_read(a->part, sector, (uint32_t)(count * SECTORS_PER_PAGE), buffer);
}
/* The I/O buffer carries one transfer at a time (a disk wait may sleep). */
static void io_acquire(void) { while (io_busy) task_wait_until(&io_channel, native_clock_ns() + 1000000ULL); io_busy = true; }
static void io_release(void) { io_busy = false; task_wake_channel(&io_channel); }
/* Sleeping is allowed: no spinlock besides the own address-space lock (space_access holds another process's lock, and
 * that process's faulting thread must not find it held by a sleeper when it resumes with the big lock). */
static bool may_block(void) { return spin_held_count() == (cpu_self()->vm_held ? 1u : 0u); }

/* ---- slots and the swap cache ---- */
static uint64_t slot_alloc(struct swap_area* a) {
    if (!a->free) return 0;
    uint64_t at = a->next && a->next < a->pages ? a->next : 1;
    for (uint64_t visited = 0; visited <= a->chunk_count; visited++) {
        uint64_t chunk = at / SLOTS_PER_CHUNK, end = (chunk + 1) * SLOTS_PER_CHUNK;
        if (end > a->pages) end = a->pages;
        if (a->chunk_free[chunk]) for (; at < end; at++) if (slot_free(slot_of(a, at))) { a->next = at + 1; return at; }
        at = end < a->pages ? end : 1;
    }
    return 0;
}
static struct swap_area* pick_area(void) {
    struct swap_area* best = NULL; unsigned start = rotor++;
    for (unsigned k = 0; k < SWAP_MAX_AREAS; k++) {
        struct swap_area* a = &areas[(start + k) % SWAP_MAX_AREAS];
        if (a->used && a->writeok && a->free && (!best || a->priority > best->priority)) best = a;
    }
    return best;
}
bool swap_writable(void) {
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) if (areas[i].used && areas[i].writeok && areas[i].free) return true;
    return false;
}
void swap_counts(uint64_t* total, uint64_t* free, uint64_t* cached) {
    uint64_t t = 0, f = 0;
    spin_acquire(&swap_lock);
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) if (areas[i].used) { t += areas[i].good; f += areas[i].free; }
    *cached = total_cached;
    spin_release(&swap_lock);
    *total = t; *free = f;
}
void swap_stats(uint64_t counters[4]) { counters[0] = stat_in; counters[1] = stat_out; counters[2] = stat_hits; counters[3] = stat_ahead; }
uint64_t swap_out_page(uint64_t frame, bool dirty) {
    spin_acquire(&swap_lock);
    uint32_t* aux = memory_frame_aux(frame); uint64_t entry = 0;
    if (aux && *aux) {   /* already in the swap cache (a page shared since fork): one more reference */
        uint32_t v = *aux - 1; unsigned type = v >> 27; uint64_t offset = v & (SWAP_MAX_PAGES - 1);
        struct swap_slot* s = slot_of(&areas[type], offset);
        if (s->count < COUNT_MAX) {
            sum_check(dirty ? "SWAP_CACHED_PAGE_DIRTIED" : "SWAP_CACHED_PAGE_CHANGED", type, offset, frame);
            s->count++; if (dirty) s->flags |= SLOT_DIRTY; entry = swap_entry_make(type, offset);
        }
    } else if (aux) {
        for (unsigned tries = 0; tries < SWAP_MAX_AREAS && !entry; tries++) {
            struct swap_area* a = pick_area(); if (!a) break;
            uint64_t offset = slot_alloc(a);
            if (!offset) panic("swap free-slot accounting");
            struct swap_slot* s = slot_of(a, offset); bool was = slot_free(s);
            s->count = 1; s->flags = SLOT_DIRTY; page_retain(frame); cache_add(a, offset, frame); sum_set((unsigned)(a - areas), offset, frame);
            slot_changed(a, offset, was);
            entry = swap_entry_make((unsigned)(a - areas), offset);
        }
    }
    spin_release(&swap_lock);
    return entry;
}
bool swap_dup(uint64_t entry) {
    bool ok = false; uint64_t offset;
    spin_acquire(&swap_lock);
    struct swap_area* a = entry_area(entry, &offset);
    if (a) { struct swap_slot* s = slot_of(a, offset); if (s->count && s->count < COUNT_MAX) { s->count++; ok = true; } }
    spin_release(&swap_lock);
    return ok;
}
void swap_free(uint64_t entry) {
    uint64_t offset;
    spin_acquire(&swap_lock);
    struct swap_area* a = entry_area(entry, &offset);
    if (!a || !slot_of(a, offset)->count) panic("swap entry freed twice or malformed");
    struct swap_slot* s = slot_of(a, offset); bool was = slot_free(s);
    if (!--s->count && s->frame) cache_remove(a, offset);   /* nobody refers to the slot: its RAM copy goes too */
    slot_changed(a, offset, was);
    spin_release(&swap_lock);
}
uint64_t swap_cache_get(uint64_t entry) {
    uint64_t offset, frame = 0;
    spin_acquire(&swap_lock);
    struct swap_area* a = entry_area(entry, &offset);
    if (a && slot_of(a, offset)->count && slot_of(a, offset)->frame) {
        frame = frame_of(slot_of(a, offset)); page_retain(frame); stat_hits++;
        sum_check("SWAP_CACHE_HIT_CHANGED", (unsigned)(a - areas), offset, frame);
    }
    spin_release(&swap_lock);
    return frame;
}
bool swap_take_exclusive(uint64_t entry, uint64_t frame) {
    uint64_t offset; bool taken = false;
    spin_acquire(&swap_lock);
    struct swap_area* a = entry_area(entry, &offset);
    struct swap_slot* s = a ? slot_of(a, offset) : NULL;
    if (s && s->count == 1 && s->frame && frame_of(s) == frame && page_references(frame) == 2) {
        bool was = slot_free(s);
        s->count = 0; cache_remove(a, offset); slot_changed(a, offset, was); taken = true;
    }
    spin_release(&swap_lock);
    return taken;
}
uint64_t swap_frame_entry(uint64_t frame) {
    spin_acquire(&swap_lock);
    uint32_t* aux = memory_frame_aux(frame); uint32_t v = aux ? *aux : 0;
    spin_release(&swap_lock);
    return v ? swap_entry_make((v - 1) >> 27, (v - 1) & (SWAP_MAX_PAGES - 1)) : 0;
}
unsigned swap_entry_count(uint64_t entry) {
    uint64_t offset; unsigned count = 0;
    spin_acquire(&swap_lock);
    struct swap_area* a = entry_area(entry, &offset); if (a) count = slot_of(a, offset)->count;
    spin_release(&swap_lock);
    return count;
}
bool swap_area_idle(unsigned type) { return type < SWAP_MAX_AREAS && areas[type].used && areas[type].good == areas[type].free; }
bool swap_area_unusable(unsigned type) { return type >= SWAP_MAX_AREAS || !areas[type].used || areas[type].unusing; }
bool swap_file_busy(const struct memory_file* cache) {
    if (!cache) return false;
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) if (areas[i].used && areas[i].cache == cache) return true;
    return false;
}

/* ---- swap-in ---- */
/* The slot's data in a retained frame: from the swap cache, else read from the device together with the following
 * in-use slots of its aligned read-ahead window that continue it on disk (one command). New pages enter the cache
 * clean. May sleep (disk, direct reclaim); *error: -22 stale entry, -5 I/O error, -12 no memory, -11 another task
 * reads the slot and this one may not sleep (a foreign address-space lock is held). */
uint64_t swap_read(uint64_t entry, bool may_oom, int* error) {
    uint64_t offset, frames[READ_AHEAD]; unsigned count = 0;
    struct swap_area* a;
    for (;;) {
        spin_acquire(&swap_lock);
        a = entry_area(entry, &offset);
        struct swap_slot* s = a ? slot_of(a, offset) : NULL;
        if (!s || !s->count) { spin_release(&swap_lock); *error = -22; return 0; }
        if (s->frame) {
            uint64_t frame = frame_of(s); page_retain(frame); stat_hits++;
            sum_check("SWAP_READ_HIT_CHANGED", (unsigned)(a - areas), offset, frame);
            spin_release(&swap_lock); return frame;
        }
        if (!(s->flags & SLOT_READING)) break;
        spin_release(&swap_lock);
        if (!may_block()) { *error = -11; return 0; }
        task_wait_until(&read_channel, native_clock_ns() + 1000000ULL);   /* another task reads it */
    }
    uint64_t window = 1ULL << reclaim_page_cluster(), end = offset - offset % window + window; if (end > a->pages) end = a->pages;
    for (uint64_t at = offset; at < end; at++) {
        struct swap_slot* s = slot_of(a, at);
        if (at > offset && (!s->count || s->frame || s->flags || !contiguous(a, at - 1))) break;
        s->flags |= SLOT_READING; count++;
    }
    spin_release(&swap_lock);
    unsigned have = 0; int result = -12;
    if ((frames[0] = reclaim_alloc_user(may_oom))) {
        for (have = 1; have < count && (frames[have] = frame_alloc_user()); have++) {}
        if (have > 1 && have <= io_pages && (!io_busy || may_block())) {   /* read-ahead through the shared buffer */
            io_acquire();
            result = area_io(a, offset, have, io_buffer, false) ? -5 : 0;
            if (!result) for (unsigned i = 0; i < have; i++) memcpy(phys_to_virt(frames[i]), io_buffer + (uint64_t)i * PAGE, PAGE);
            io_release();
        } else {   /* one page straight into its frame */
            for (unsigned i = 1; i < have; i++) page_free(frames[i]);
            have = 1; result = area_io(a, offset, 1, phys_to_virt(frames[0]), false) ? -5 : 0;
        }
    }
    uint64_t target = 0;
    spin_acquire(&swap_lock);
    for (unsigned i = 0; i < count; i++) {
        struct swap_slot* s = slot_of(a, offset + i); bool was = slot_free(s);
        s->flags &= ~SLOT_READING;
        uint64_t frame = i < have ? frames[i] : 0;
        if (frame && !result) sum_check("SWAP_READ_MISMATCH", (unsigned)(a - areas), offset + i, frame);
        if (frame && !result && s->count && !s->frame) {
            cache_add(a, offset + i, frame);
            if (!i) { page_retain(frame); target = frame; } else stat_ahead++;
        } else if (frame) {
            if (!i && !result) target = frame;   /* freed while it was read: the caller only drops it */
            else page_free(frame);
        }
        slot_changed(a, offset + i, was);
    }
    if (target) stat_in++;
    if (result == -5) a->errors++;
    spin_release(&swap_lock);
    task_wake_channel(&read_channel);
    if (!target) *error = result ? result : -12;
    return target;
}

/* ---- write-back ---- */
/* Next run of swap-cache pages that only the cache holds (swap_lock held): clean ones met on the way are freed, dirty
 * ones that continue each other on the device are marked WRITEBACK and copied into the I/O buffer. */
static unsigned collect_run(uint64_t* freed, unsigned* run_type, uint64_t* run_first, uint32_t* frames) {
    unsigned type = cursor_type % SWAP_MAX_AREAS, n = 0, wraps = 0; uint64_t at = cursor_offset, budget = 1 << 16;
    while (budget && wraps <= SWAP_MAX_AREAS) {
        struct swap_area* a = &areas[type];
        if (!a->used || !a->cached || at >= a->pages) { if (n) break; type = (type + 1) % SWAP_MAX_AREAS; at = 1; wraps++; continue; }
        uint64_t chunk = at / SLOTS_PER_CHUNK;
        if (!a->chunk_cached[chunk]) { if (n) break; at = (chunk + 1) * SLOTS_PER_CHUNK; budget--; continue; }
        struct swap_slot* s = slot_of(a, at); budget--;
        bool idle = s->frame && page_references(frame_of(s)) == 1 && !(s->flags & (SLOT_WRITEBACK | SLOT_READING));
        if (idle && !(s->flags & SLOT_DIRTY)) { bool was = slot_free(s); cache_remove(a, at); slot_changed(a, at, was); (*freed)++; idle = false; }
        if (idle && (!n || contiguous(a, at - 1))) {
            if (!n) { *run_type = type; *run_first = at; }
            s->flags |= SLOT_WRITEBACK; frames[n] = s->frame;
            memcpy(io_buffer + (uint64_t)n * PAGE, phys_to_virt(frame_of(s)), PAGE);
            n++; at++;
            if (n == io_pages) break;
            continue;
        }
        if (n) break;
        at++;
    }
    cursor_type = type; cursor_offset = at;
    return n;
}
unsigned swap_writeback(unsigned budget, uint64_t* freed) {
    unsigned written = 0; uint32_t frames[IO_PAGES]; *freed = 0;
    while (total_cached && io_buffer && written < budget) {
        unsigned type = 0; uint64_t first = 0;
        io_acquire();
        if (!io_buffer) { io_release(); break; }
        spin_acquire(&swap_lock);
        unsigned n = collect_run(freed, &type, &first, frames);
        spin_release(&swap_lock);
        if (!n) { io_release(); break; }
        struct swap_area* a = &areas[type];
        bool failed = area_io(a, first, n, io_buffer, true) != 0;
        io_release();
        spin_acquire(&swap_lock);
        for (unsigned i = 0; i < n; i++) {
            struct swap_slot* s = slot_of(a, first + i); bool was = slot_free(s);
            s->flags &= ~SLOT_WRITEBACK;
            if (!failed && s->frame == frames[i]) {
                s->flags &= ~SLOT_DIRTY;
                if (page_references(frame_of(s)) == 1) { cache_remove(a, first + i); (*freed)++; }
            }
            slot_changed(a, first + i, was);
        }
        if (failed) a->errors++; else { stat_out += n; written += n; }
        spin_release(&swap_lock);
        if (failed) { vga_print("[64] SWAP write error: pages stay in memory\n"); break; }
    }
    return written;
}

/* ---- swapon / swapoff ---- */
struct header_info { uint64_t pages; uint32_t bad_count; uint32_t bad[MAX_BADPAGES]; uint8_t uuid[16]; char label[17]; };
static uint32_t swab32(uint32_t x) { return x >> 24 | (x >> 8 & 0xFF00) | (x << 8 & 0xFF0000) | x << 24; }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* Linux read_swap_header(): the area's size in pages, 0 when invalid. `size_pages` is the device/file size. */
static uint64_t parse_header(const uint8_t* page, uint64_t size_pages, bool regular, struct header_info* info) {
    if (memcmp(page + PAGE - 10, "SWAPSPACE2", 10)) { vga_print("[64] SWAP Unable to find swap-space signature\n"); return 0; }
    uint32_t version = get32(page + 1024), last = get32(page + 1028), bad = get32(page + 1032);
    bool swapped = swab32(version) == 1;
    if (swapped) { version = 1; last = swab32(last); bad = swab32(bad); }
    if (version != 1) { vga_print("[64] SWAP Unable to handle swap header version\n"); return 0; }
    if (!last) { vga_print("[64] SWAP Empty swap-file\n"); return 0; }
    uint64_t pages = (uint64_t)last + 1;
    if (pages > SWAP_MAX_PAGES) { vga_print("[64] SWAP Truncating oversized swap area\n"); pages = SWAP_MAX_PAGES; }
    if (pages > size_pages) { vga_print("[64] SWAP Swap area shorter than signature indicates\n"); return 0; }
    if (bad && regular) return 0;   /* bad-page lists are for devices */
    if (bad > MAX_BADPAGES) return 0;
    info->pages = pages; info->bad_count = bad;
    for (uint32_t i = 0; i < bad; i++) { uint32_t v = get32(page + 1536 + 4 * i); info->bad[i] = swapped ? swab32(v) : v; }
    memcpy(info->uuid, page + 1036, 16); memcpy(info->label, page + 1052, 16); info->label[16] = 0;
    return pages;
}
static int add_extent(void* context, uint64_t offset, uint64_t sector, uint64_t sectors) {
    struct swap_area* a = context;
    if (a->extent_count) {
        struct swap_extent* last = &a->extents[a->extent_count - 1];
        if (last->offset + last->sectors * 512 == offset && last->sector + last->sectors == sector) { last->sectors += sectors; return 0; }
    }
    if (a->extent_count == MAX_EXTENTS) { vga_print("[64] SWAP swapfile too fragmented\n"); return -22; }
    if ((a->extent_count + 1) * sizeof(struct swap_extent) > a->extent_bytes) {
        uint64_t bytes = a->extent_bytes ? a->extent_bytes * 2 : PAGE;
        struct swap_extent* grown = (void*)arena_alloc(bytes); if (!grown) return -12;
        if (a->extents) { memcpy(grown, a->extents, a->extent_count * sizeof(struct swap_extent)); arena_free((uint64_t)a->extents, a->extent_bytes); }
        a->extents = grown; a->extent_bytes = bytes;
    }
    a->extents[a->extent_count++] = (struct swap_extent){offset, sector, sectors};
    return 0;
}
static void area_teardown(struct swap_area* a) {
    if (a->chunks) {
        for (uint64_t i = 0; i < a->chunk_count; i++) if (a->chunks[i]) page_free((uint64_t)a->chunks[i]);
        arena_free((uint64_t)a->chunks, a->chunk_count * sizeof(void*));
    }
    if (a->chunk_free) arena_free((uint64_t)a->chunk_free, a->chunk_count * 2 * sizeof(uint16_t));
    if (a->extents) arena_free((uint64_t)a->extents, a->extent_bytes);
    if (a->cache) memory_file_release(a->cache);
    memset(a, 0, sizeof(*a));
}
static bool any_area(void) { for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) if (areas[i].used) return true; return false; }
/* stor2: kernel pages the active areas own (slot chunks and their indexes, extents, audit sums) and the I/O buffer. A
 * session may swapoff /etc/fstab areas activated before it started (or swapon new ones): run_image's ownership check
 * (kernel.c) leaves these out. */
static uint64_t pages_of(uint64_t bytes) { return (bytes + PAGE - 1) / PAGE; }
uint64_t swap_kernel_pages(void) {
    uint64_t total = io_buffer ? io_pages : 0;
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) {
        const struct swap_area* a = &areas[i];
        if (!a->used) continue;
        total += a->chunk_count + pages_of(a->chunk_count * sizeof(void*)) + pages_of(a->chunk_count * 2 * sizeof(uint16_t)) + pages_of(a->extent_bytes);
        if (slot_sum[i]) total += pages_of(slot_sum_pages[i] * 4);
    }
    return total;
}
static const struct block_partition* partition_named(const char* path) {
    if (strncmp(path, "/dev/", 5)) return NULL;
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        if (p && !strcmp(p->label, path + 5)) return p;
    }
    return NULL;
}
static bool partition_mounted(const struct block_partition* p) {
    const struct block_partition* root = disk_posix() ? ext4_dev_partition() : block_root();
    /* The mounted volume, or a whole disk holding it (Linux: the block device is claimed). */
    return root && (p == root || (p->dev == root->dev && (p->number == 0 || root->number == 0)));
}
/* What a swapon/swapoff path names: a partition, or a regular file (its inode and snapshot). */
struct swap_target { const struct block_partition* part; fat32_file_t file; uint64_t inode; char path[FAT32_PATH_MAX]; };
static int resolve_target(struct swap_target* t) {   /* t->path holds the name */
    int error;
    if ((t->part = partition_named(t->path))) return 0;
    struct open_file* bound = NULL;
    if ((error = native_path_canonical((uint64_t)-100, t->path, true, &bound))) return error;
    if (bound) {   /* /proc/self/fd/N: the open file itself */
        struct regular_view view; if (fd_regular(bound, &view) != 1) return -22;
        t->file = *kernel_file_disk(bound); strcpy(t->path, view.path);
    } else {
        if ((t->part = partition_named(t->path))) return 0;
        if (proc_path(t->path) || device_lookup(t->path) >= 0) return -22;   /* not a block device or regular file */
        error = disk_lookup_status(t->path, &t->file); if (error) return error == -2 ? -2 : -5;
    }
    if (t->file.is_dir || t->file.native_kind || t->file.detached || (disk_posix() && (t->file.mode & 0170000) != 0100000)) return -22;
    t->inode = disk_inode_id(&t->file);
    return 0;
}
static int area_index(const struct swap_target* t, bool reserved) {
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) {
        if (!areas[i].used && !(reserved && areas[i].reserved)) continue;
        if (t->part ? areas[i].partition && areas[i].part == t->part : !areas[i].partition && areas[i].inode == t->inode) return (int)i;
    }
    return -1;
}
static int swap_on(struct swap_target* t, uint64_t flags) {
    if ((flags & ~(uint64_t)SWAP_FLAGS_VALID)) return -22;
    uint8_t* header = (void*)page_alloc(); struct header_info* info = (void*)page_alloc();
    struct swap_area* a = NULL; int error = 0; unsigned type = 0;
    if (!header || !info) { error = -12; goto out; }
    if ((error = resolve_target(t))) goto out;
    if (area_index(t, true) >= 0 || (t->part && partition_mounted(t->part))) { error = -16; goto out; }
    while (type < SWAP_MAX_AREAS && (areas[type].used || areas[type].reserved)) type++;
    if (type == SWAP_MAX_AREAS) { error = -1; goto out; }   /* Linux: EPERM when every swap type is taken */
    a = &areas[type]; memset(a, 0, sizeof(*a));
    a->reserved = true; a->part = t->part; a->partition = t->part != NULL; a->inode = t->inode;   /* a racing swapon gets EBUSY */
    uint64_t size_pages;
    if (t->part) {
        size_pages = block_part_sectors(t->part) / SECTORS_PER_PAGE;
        a->partition = true; a->part = t->part;
        if (size_pages < 2) { error = -22; goto fail; }
        if (block_part_read(t->part, 0, SECTORS_PER_PAGE, header)) { error = -5; goto fail; }
    } else {
        if (disk_is_readonly()) { error = -30; goto fail; }
        a->cache = memory_file_disk_get(&t->file); if (!a->cache) { error = -12; goto fail; }
        if (a->cache->writable_maps) { error = -26; goto fail; }      /* a shared writable mapping could write it */
        if ((error = memory_file_sync(a->cache, 0, ~0ULL))) goto fail;  /* mkswap's header may still be cached */
        t->file = a->cache->disk;
        size_pages = a->cache->size / PAGE;
        if (size_pages < 2) { error = -22; goto fail; }
        if (disk_read_file(&t->file, 0, PAGE, header) != (int32_t)PAGE) { error = -5; goto fail; }
        if ((error = disk_file_extents(&t->file, &a->part, add_extent, a))) { if (error != -12 && error != -5) error = -22; goto fail; }
        if (!a->part) { error = -22; goto fail; }
        a->inode = t->inode;
    }
    uint64_t pages = parse_header(header, size_pages, !t->part, info);
    if (!pages) { error = -22; goto fail; }
    a->pages = pages; a->chunk_count = (pages + SLOTS_PER_CHUNK - 1) / SLOTS_PER_CHUNK;
    a->chunks = (void*)arena_alloc(a->chunk_count * sizeof(void*));
    a->chunk_free = (void*)arena_alloc(a->chunk_count * 2 * sizeof(uint16_t));
    if (!a->chunks || !a->chunk_free) { error = -12; goto fail; }
    a->chunk_cached = a->chunk_free + a->chunk_count;
    for (uint64_t i = 0; i < a->chunk_count; i++) if (!(a->chunks[i] = (void*)page_alloc())) { error = -12; goto fail; }
    slot_of(a, 0)->flags = SLOT_BAD;   /* the header */
    for (uint32_t i = 0; i < info->bad_count; i++) {
        if (!info->bad[i] || info->bad[i] >= pages) { error = -22; goto fail; }
        slot_of(a, info->bad[i])->flags = SLOT_BAD;
    }
    uint64_t good = 0;
    for (uint64_t at = 1; at < pages; at++) {
        struct swap_slot* s = slot_of(a, at);
        if (!s->flags && !page_mapped(a, at)) s->flags = SLOT_BAD;
        if (!s->flags) { good++; a->chunk_free[at / SLOTS_PER_CHUNK]++; }
    }
    if (!good) { vga_print("[64] SWAP Empty swap-file\n"); error = -22; goto fail; }
    if (!io_buffer) {   /* DMA32 RAM: every controller transfers into it directly */
        for (io_pages = IO_PAGES; io_pages && !(io_buffer = (void*)dma32_alloc(io_pages * PAGE)); io_pages /= 2) {}
        if (!io_buffer) { error = -12; goto fail; }
    }
    a->good = a->free = good; a->next = 1; a->flags = (unsigned)flags;
    if (vm_audit_on && !slot_sum[a - areas]) {
        slot_sum[a - areas] = (uint32_t*)arena_alloc(pages * 4); slot_sum_pages[a - areas] = slot_sum[a - areas] ? pages : 0;
        if (slot_sum[a - areas]) vga_print("[64] AUDIT swap checksums on\n");
    }
    a->priority = flags & SWAP_FLAG_PREFER ? (int)(flags & SWAP_FLAG_PRIO_MASK) : --least_priority;
    memcpy(a->uuid, info->uuid, 16); memcpy(a->label, info->label, sizeof(a->label));
    strcpy(a->path, t->path);
    if (a->cache) memory_file_shrink(a->cache, ~0ULL);   /* data now lives on the device: clean cached copies go */
    spin_acquire(&swap_lock); a->used = true; a->writeok = true; a->reserved = false; spin_release(&swap_lock);
    char number[24];
    vga_print("[64] SWAP Adding "); number64(good * 4, number); vga_print(number); vga_print("k swap on "); vga_print(a->path);
    vga_print(". Priority:"); if (a->priority < 0) { vga_print("-"); number64((uint64_t)-(int64_t)a->priority, number); } else number64((uint64_t)a->priority, number);
    vga_print(number); vga_print(" extents:"); number64(a->partition ? 1 : a->extent_count, number); vga_print(number);
    vga_print(flags & SWAP_FLAG_DISCARD ? " D\n" : "\n");
    goto out;
fail:
    area_teardown(a);
out:
    if (header) page_free((uint64_t)header);
    if (info) page_free((uint64_t)info);
    return error;
}
static int swap_off(struct swap_target* t) {
    int error = resolve_target(t), index = error ? -1 : area_index(t, false);
    if (error) return error;
    if (index < 0) return -22;
    struct swap_area* a = &areas[index];
    if (a->unusing) return -16;   /* another swapoff of it is running */
    /* Fail cleanly before touching anything when the swapped-out pages cannot come back: they need what memory can
     * still give (MemAvailable) plus the free slots of the other areas (the rest of memory may move there). */
    uint64_t room = reclaim_available(), needed = a->good - a->free - a->cached;
    spin_acquire(&swap_lock);
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) if (&areas[i] != a && areas[i].used && areas[i].writeok) room += areas[i].free;
    a->writeok = false; a->unusing = true;
    spin_release(&swap_lock);
    if (needed > room) {
        spin_acquire(&swap_lock); a->writeok = true; a->unusing = false; spin_release(&swap_lock);
        return -12;
    }
    error = vm_swap_unuse((unsigned)index);
    uint64_t in_use = a->good - a->free;
    if (!error && in_use) error = -12;   /* references the walk could not bring back: keep the area */
    if (error) {
        spin_acquire(&swap_lock); a->writeok = true; a->unusing = false; spin_release(&swap_lock);
        char number[24]; vga_print("[64] SWAP swapoff failed, slots still in use="); number64(in_use, number); vga_print(number); vga_print("\n");
        return error;
    }
    spin_acquire(&swap_lock); a->used = false; spin_release(&swap_lock);
    int priority = a->priority;
    vga_print("[64] SWAP Removed swap on "); vga_print(a->path); vga_print("\n");
    area_teardown(a);
    if (priority < 0) {   /* Linux: later automatic priorities move up */
        for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) if (areas[i].used && areas[i].priority < priority) areas[i].priority++;
        least_priority++;
    }
    if (!any_area() && io_buffer && !io_busy) { arena_free((uint64_t)io_buffer, io_pages * PAGE); io_buffer = NULL; io_pages = 0; }
    return 0;
}
int64_t swap_call(struct registers* r) {
    if (!credentials_capable(credentials_current(), NULL, CAP_SYS_ADMIN)) return -1;
    struct swap_target* t = (void*)page_alloc(); if (!t) return -12;
    memset(t, 0, sizeof(*t));
    int error = kernel_user_path(r->rdi, t->path);
    if (!error) error = r->rax == 167 ? swap_on(t, r->rsi) : swap_off(t);
    page_free((uint64_t)t);
    return error;
}
void swap_init(void) {}

/* ---- boot: swap entries of /etc/fstab (Linux swapon -a) ----
 * "<path|UUID=..|LABEL=..> <ignored> swap <options> ..." with options sw/defaults, pri=N, discard[=once|pages],
 * nofail, noauto (skipped). UUID= and LABEL= name swap partitions by their mkswap header. Skipped entirely with the
 * kernel option "noswap" or the QEMU fw_cfg string opt/nexus/swap=off (the desktop launcher's --no-swap). */
static inline void out16(uint16_t port, uint16_t value) { __asm__ volatile("outw %0,%1" :: "a"(value), "Nd"(port)); }
static inline uint8_t in8(uint16_t port) { uint8_t value; __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port)); return value; }
static bool fw_cfg_off(void) {
    static const char name[] = "opt/nexus/swap";
    out16(0x510, 0); if (in8(0x511) != 'Q' || in8(0x511) != 'E' || in8(0x511) != 'M' || in8(0x511) != 'U') return false;
    out16(0x510, 0x19); uint32_t count = 0; for (unsigned i = 0; i < 4; i++) count = count << 8 | in8(0x511);
    if (count > 0x4000) return false;
    uint16_t selector = 0; uint32_t size = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t entry[64]; for (unsigned j = 0; j < 64; j++) entry[j] = in8(0x511);
        if (!memcmp(entry + 8, name, sizeof(name))) { size = (uint32_t)entry[0] << 24 | (uint32_t)entry[1] << 16 | (uint32_t)entry[2] << 8 | entry[3]; selector = (uint16_t)(entry[4] << 8 | entry[5]); }
    }
    if (!selector || size < 3 || size > 4) return false;
    out16(0x510, selector); char value[4] = {0};
    for (unsigned i = 0; i < size; i++) value[i] = (char)in8(0x511);
    return !memcmp(value, "off", 3);
}
static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static bool header_names(const struct block_partition* p, const char* spec) {
    uint8_t* page = (void*)page_alloc(); if (!page) return false;
    bool match = false;
    if (!block_part_read(p, 0, SECTORS_PER_PAGE, page) && !memcmp(page + PAGE - 10, "SWAPSPACE2", 10)) {
        if (!strncmp(spec, "LABEL=", 6)) match = page[1052] && strlen(spec + 6) <= 16 && !strncmp((const char*)page + 1052, spec + 6, 16);
        else {   /* UUID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
            const char* u = spec + 5; unsigned n = 0; match = true;
            for (unsigned i = 0; i < 16 && match; i++) {
                if (u[n] == '-') n++;
                int hi = hex_digit(u[n]), lo = hi < 0 ? -1 : hex_digit(u[n + 1]);
                if (hi < 0 || lo < 0 || page[1036 + i] != (uint8_t)(hi << 4 | lo)) match = false;
                else n += 2;
            }
            match = match && !u[n];
        }
    }
    page_free((uint64_t)page); return match;
}
int swap_boot(void) {
    if (boot_option("noswap") || fw_cfg_off()) { vga_print("[64] SWAP boot activation disabled\n"); return 0; }
    fat32_file_t fstab; if (!disk_lookup("/etc/fstab", &fstab) || fstab.is_dir || !fstab.size) return 0;
    uint64_t size = fstab.size < 16384 ? fstab.size : 16384;
    char* text = (void*)arena_alloc(size + 1); struct swap_target* t = (void*)page_alloc();
    int activated = 0;
    if (text && t && disk_read_file(&fstab, 0, size, text) == (int32_t)size) {
        text[size] = 0;
        for (char* line = text; line && *line;) {
            char* end = line; while (*end && *end != '\n') end++;
            char saved = *end; *end = 0;
            char* field[4] = {0}; unsigned fields = 0; char* c = line;
            while (*c == ' ' || *c == '\t') c++;
            if (*c != '#') while (*c && fields < 4) {
                field[fields++] = c; while (*c && *c != ' ' && *c != '\t' && *c != '\r') c++;
                if (*c) *c++ = 0;
                while (*c == ' ' || *c == '\t' || *c == '\r') c++;
            }
            if (fields >= 3 && !strcmp(field[2], "swap")) {
                uint64_t flags = 0; bool skip = false, nofail = false;
                for (char* option = field[3]; option && *option;) {
                    char* next = option; while (*next && *next != ',') next++;
                    char separator = *next; *next = 0;
                    if (!strncmp(option, "pri=", 4)) {
                        uint64_t v = 0; for (char* d = option + 4; *d >= '0' && *d <= '9' && v <= SWAP_FLAG_PRIO_MASK; d++) v = v * 10 + (uint64_t)(*d - '0');
                        if (v > SWAP_FLAG_PRIO_MASK) v = SWAP_FLAG_PRIO_MASK;
                        flags |= SWAP_FLAG_PREFER | v;
                    }
                    else if (!strcmp(option, "discard")) flags |= SWAP_FLAG_DISCARD;
                    else if (!strcmp(option, "discard=once")) flags |= SWAP_FLAG_DISCARD | 0x20000;
                    else if (!strcmp(option, "discard=pages")) flags |= SWAP_FLAG_DISCARD | 0x40000;
                    else if (!strcmp(option, "noauto")) skip = true;
                    else if (!strcmp(option, "nofail")) nofail = true;
                    *next = separator; option = separator ? next + 1 : next;
                }
                memset(t, 0, sizeof(*t)); int error = -2;
                if (!strncmp(field[0], "UUID=", 5) || !strncmp(field[0], "LABEL=", 6)) {
                    for (unsigned i = 0; i < block_partition_count() && error == -2; i++) {
                        const struct block_partition* p = block_partition_get(i);
                        if (p && !partition_mounted(p) && header_names(p, field[0])) { strcpy(t->path, "/dev/"); strcat(t->path, p->label); error = 0; }
                    }
                } else if (strlen(field[0]) < FAT32_PATH_MAX) { strcpy(t->path, field[0]); error = 0; }
                if (!skip) {
                    if (!error) error = swap_on(t, flags);
                    if (!error) activated++;
                    else if (!nofail) { char n[24]; vga_print("[64] SWAP fstab entry failed: "); vga_print(field[0]); vga_print(" errno="); number64((uint64_t)-error, n); vga_print(n); vga_print("\n"); }
                }
            }
            *end = saved; line = saved ? end + 1 : end;
        }
    }
    if (text) arena_free((uint64_t)text, size + 1);
    if (t) page_free((uint64_t)t);
    return activated;
}

/* ---- /proc/swaps (Linux swap_show) ---- */
static unsigned append(char* text, unsigned at, unsigned capacity, const char* s) {
    while (*s && at + 1 < capacity) text[at++] = *s++;
    if (at < capacity) text[at] = 0;
    return at;
}
int swap_proc_swaps(char* text, unsigned capacity) {
    unsigned n = append(text, 0, capacity, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) {
        struct swap_area* a = &areas[i]; if (!a->used) continue;
        char number[24]; unsigned length = strlen(a->path);
        n = append(text, n, capacity, a->path);
        for (unsigned pad = length < 40 ? 40 - length : 1; pad--;) n = append(text, n, capacity, " ");
        n = append(text, n, capacity, a->partition ? "partition\t" : "file\t\t");
        uint64_t size = a->good * 4, used = (a->good - a->free) * 4;
        number64(size, number); n = append(text, n, capacity, number); n = append(text, n, capacity, size < 10000000 ? "\t\t" : "\t");
        number64(used, number); n = append(text, n, capacity, number); n = append(text, n, capacity, used < 10000000 ? "\t\t" : "\t");
        if (a->priority < 0) { n = append(text, n, capacity, "-"); number64((uint64_t)-(int64_t)a->priority, number); } else number64((uint64_t)a->priority, number);
        n = append(text, n, capacity, number); n = append(text, n, capacity, "\n");
    }
    return (int)n;
}

/* ---- hibernation into a swap area (hibernate.c, Linux swsusp's place) ----
 * The image goes into free slots of an active area. They are taken (SLOT_HIBERNATE: not free, so neither swap-out nor
 * swapoff uses them) until hibernate.c gives them back: after a failed hibernation, or in the restored kernel, whose
 * copy of these tables still holds them. Pages already swapped out stay where they are: the restored page tables still
 * point at them. Device positions are 512-byte sectors of the area's partition. */
#define SLOT_HIBERNATE 0x10
static bool swap_partition_typed(const struct block_partition* p) {   /* findable at boot without resume= */
    static const uint8_t linux_swap[16] = {0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43, 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F};
    return (p->scheme == BLOCK_SCHEME_MBR && p->mbr_type == 0x82) || (p->scheme == BLOCK_SCHEME_GPT && !memcmp(p->type_guid, linux_swap, 16));
}
/* An active area for the image: on `part` with its header at page `header_page` of it (resume= / resume_offset=), or -
 * part NULL - the swap partition with the most free slots whose type lets the next boot find it. */
int swap_hibernate_pick(const struct block_partition* part, uint64_t header_page, uint64_t* header_sector, uint64_t* free_pages, const struct block_partition** area_part) {
    int best = -1;
    spin_acquire(&swap_lock);
    for (unsigned i = 0; i < SWAP_MAX_AREAS; i++) {
        struct swap_area* a = &areas[i];
        if (!a->used || !a->writeok || a->unusing || !a->part) continue;
        uint64_t sector = page_sector(a, 0);
        if (part ? a->part != part || sector != header_page * SECTORS_PER_PAGE : !a->partition || !swap_partition_typed(a->part)) continue;
        if (best < 0 || a->free > areas[best].free) best = (int)i;
    }
    if (best >= 0) { *header_sector = page_sector(&areas[best], 0); *free_pages = areas[best].free; *area_part = areas[best].part; }
    spin_release(&swap_lock);
    return best;
}
/* Take up to `pages` free slots of area `type`, reported as device runs (contiguous slots merged). Returns the number
 * taken (all of them stay taken until swap_hibernate_release, also when the caller gives up). */
uint64_t swap_hibernate_reserve(unsigned type, uint64_t pages, int (*run)(void* context, uint64_t sector, uint64_t sectors), void* context) {
    if (type >= SWAP_MAX_AREAS) return 0;
    struct swap_area* a = &areas[type]; uint64_t taken = 0, run_start = 0, run_sectors = 0; int error = 0;
    spin_acquire(&swap_lock);
    if (!a->used || !a->writeok || a->unusing) { spin_release(&swap_lock); return 0; }
    for (uint64_t at = 1; at < a->pages && taken < pages && !error; at++) {
        struct swap_slot* s = slot_of(a, at);
        if (!slot_free(s)) continue;
        s->flags |= SLOT_HIBERNATE; slot_changed(a, at, true); taken++;
        uint64_t sector = page_sector(a, at);
        if (run_sectors && run_start + run_sectors == sector) { run_sectors += SECTORS_PER_PAGE; continue; }
        if (run_sectors) error = run(context, run_start, run_sectors);
        run_start = sector; run_sectors = SECTORS_PER_PAGE;
    }
    if (run_sectors && !error) error = run(context, run_start, run_sectors);
    spin_release(&swap_lock);
    return error ? 0 : taken;
}
void swap_hibernate_release(unsigned type) {
    if (type >= SWAP_MAX_AREAS) return;
    struct swap_area* a = &areas[type]; uint64_t released = 0;
    spin_acquire(&swap_lock);
    if (a->used && a->chunks)
        for (uint64_t at = 1; at < a->pages; at++) {
            struct swap_slot* s = slot_of(a, at);
            if (s->flags & SLOT_HIBERNATE) { s->flags &= (uint8_t)~SLOT_HIBERNATE; slot_changed(a, at, false); released++; }
        }
    spin_release(&swap_lock);
    (void)released;
}
