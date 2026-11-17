/* dm.c targets for LVM2 (included by dm.c): striped (Linux drivers/md/dm-stripe.c), snapshot-origin and snapshot with
 * the persistent and transient exception stores (dm-snap.c, dm-snap-persistent.c, dm-exception-store.c).
 *
 * striped "<stripes> <chunk> (<dev> <offset>)+": chunk i of the target is chunk i/stripes of stripe i%stripes; the
 * length must divide into whole chunks per stripe.
 * snapshot-origin "<origin>": reads pass through; a write first copies, for every valid snapshot of that origin, each
 * chunk it touches that the snapshot has not saved yet into the snapshot's COW device.
 * snapshot "<origin> <cow> <P|PO|N|NO> <chunk> [<#features> <feature>...]": a chunk with an exception reads from the
 * COW, the others from the origin; a write to the snapshot saves the origin chunk first. Persistent store (P): chunk 0
 * is the header {magic "SnAp" 0x70416e53, valid, version 1, chunk size}, metadata areas of chunk/16 {old, new} entries
 * at chunk 1 + (entries+1)*n with their data chunks between them, an entry with new == 0 ends the list; a zeroed header
 * is a new store (lvcreate wipes it). Every exception is copied, flushed, then committed (its area written and
 * flushed) before the write that needed it goes on, so a power cut leaves either the old or the new state. Transient
 * (N): exceptions in memory only (a reload starts empty). When the COW is full the snapshot is invalidated (the
 * header's valid = 0, its I/O fails, status "Invalid"; with PO/NO: "Overflow", reads still work); origin writes go on.
 * Status: "<allocated>/<total sectors> <metadata sectors>" (lvs Data%), striped "<n> <devs...> 1 <A per stripe>".
 * Hostile COW metadata (bad magic/version/chunk size, entries outside the COW or the origin, duplicates, chunks that
 * are metadata) refuses the table (EINVAL). Not done: snapshot-merge (lvconvert --merge), discards through these. */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */
#define SNAP_MAGIC 0x70416e53u
struct snap_entry { uint64_t old_chunk, new_chunk; struct snap_entry* next; };
struct snap_page { struct snap_page* next; unsigned used; struct snap_entry e[(4096 - 16) / sizeof(struct snap_entry)]; };
_Static_assert(sizeof(struct snap_page) <= 4096, "snapshot entry page");
struct snapshot {
    struct block_partition* origin; struct block_partition* cow; uint64_t chunk;   /* sectors */
    bool persistent, overflow_mode, valid, overflowed;
    uint64_t cow_chunks, origin_chunks, next_free, count, area; uint32_t per_area, area_used;
    uint8_t* area_buffer; struct snap_entry** buckets; uint32_t bucket_bits; struct snap_page* pages;
    volatile int busy;
};

/* ---- striped ---- */
static struct block_partition* stripe_dev(const struct target* g, unsigned s, uint64_t* offset) {
    if (!s) { *offset = g->offset; return g->under; }
    *offset = g->more_offset[s - 1]; return g->more[s - 1];
}
static int striped_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {
    while (count) {
        uint64_t chunk = relative / g->chunk, within = relative % g->chunk, offset;
        struct block_partition* p = stripe_dev(g, (unsigned)(chunk % g->stripes), &offset);
        uint64_t at = offset + chunk / g->stripes * g->chunk + within;
        uint32_t n = g->chunk - within < count ? (uint32_t)(g->chunk - within) : count;
        if ((write ? block_part_write(p, at, n, data) : block_part_read(p, at, n, data)) < 0) return -1;
        relative += n; count -= n; data += (uint64_t)n * 512;
    }
    return 0;
}
static int parse_striped(struct target* g, char* params) {
    char* cursor = params; const char* end; uint64_t stripes, chunk;
    char* a = word(&cursor); char* b = word(&cursor);
    if (!a || !b || !parse_u64(a, &stripes, &end) || *end || !parse_u64(b, &chunk, &end) || *end) return -22;
    if (!stripes || stripes > STRIPES || !chunk || chunk > (1u << 21) || g->length % stripes || (g->length / stripes) % chunk) return -22;
    g->stripes = (unsigned)stripes; g->chunk = chunk; g->extra = (unsigned)stripes - 1;
    for (unsigned s = 0; s < stripes; s++) {
        char* dev = word(&cursor); char* off = word(&cursor); uint64_t o;
        if (!dev || !off || !parse_u64(off, &o, &end) || *end) return -22;
        struct block_partition* p = device_of(dev); if (!p) return -6;
        if (!s) { g->under = p; g->offset = o; } else { g->more[s - 1] = p; g->more_offset[s - 1] = o; }
    }
    return word(&cursor) ? -22 : 0;
}

/* ---- the exception table ---- */
static uint32_t snap_hash(const struct snapshot* s, uint64_t old) { return (uint32_t)((old * 0x9E3779B97F4A7C15ULL) >> (64 - s->bucket_bits)); }
static struct snap_entry* snap_find(const struct snapshot* s, uint64_t old) {
    for (struct snap_entry* e = s->buckets[snap_hash(s, old)]; e; e = e->next) if (e->old_chunk == old) return e;
    return NULL;
}
static int snap_add(struct snapshot* s, uint64_t old, uint64_t fresh) {
    if (!s->pages || s->pages->used == sizeof(s->pages->e) / sizeof(s->pages->e[0])) {
        struct snap_page* p = (void*)arena_alloc(4096); if (!p) return -12;
        memset(p, 0, 4096); p->next = s->pages; s->pages = p;
    }
    struct snap_entry* e = &s->pages->e[s->pages->used++];
    uint32_t h = snap_hash(s, old);
    e->old_chunk = old; e->new_chunk = fresh; e->next = s->buckets[h]; s->buckets[h] = e; s->count++;
    return 0;
}
static void snap_free(struct snapshot* s) {
    if (!s) return;
    while (s->pages) { struct snap_page* p = s->pages; s->pages = p->next; arena_free((uint64_t)p, 4096); }
    if (s->buckets) arena_free((uint64_t)s->buckets, (uint64_t)sizeof(void*) << s->bucket_bits);
    if (s->area_buffer) arena_free((uint64_t)s->area_buffer, s->chunk * 512);
    arena_free((uint64_t)s, sizeof(*s));
}
static void snap_lock(struct snapshot* s) {
    while (__atomic_exchange_n(&s->busy, 1, __ATOMIC_ACQUIRE)) {
        if (dm_may_wait()) kernel_io_sleep((void*)&s->busy, native_clock_ns() + 1000000ULL); else __asm__ volatile("pause");
    }
}
static void snap_unlock(struct snapshot* s) { __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE); task_wake_channel((void*)&s->busy); }

/* ---- the persistent store ---- */
static uint64_t area_location(const struct snapshot* s, uint64_t area) { return 1 + (uint64_t)(s->per_area + 1) * area; }
static int write_header(struct snapshot* s, bool valid) {
    uint8_t sector[512]; memset(sector, 0, sizeof(sector));
    put_le32(sector, SNAP_MAGIC); put_le32(sector + 4, valid ? 1 : 0); put_le32(sector + 8, 1); put_le32(sector + 12, (uint32_t)s->chunk);
    return block_part_write(s->cow, 0, 1, sector) < 0 || block_flush(s->cow->dev) < 0 ? -5 : 0;
}
static int write_area(struct snapshot* s) {
    return block_part_write(s->cow, area_location(s, s->area) * s->chunk, (uint32_t)s->chunk, s->area_buffer) < 0 || block_flush(s->cow->dev) < 0 ? -5 : 0;
}
static void snap_invalidate(struct snapshot* s, const char* why) {
    if (!s->valid) return;
    s->valid = false;
    if (s->persistent) write_header(s, false);
    vga_print("[64] DM snapshot invalidated: "); vga_print(why); vga_print("\n");
}
static int snap_read_store(struct snapshot* s) {   /* P: the header and every metadata area (lvm reactivates from them) */
    uint8_t sector[512];
    if (block_part_read(s->cow, 0, 1, sector) < 0) return -5;
    uint32_t magic = get_le32(sector), valid = get_le32(sector + 4), version = get_le32(sector + 8), chunk = get_le32(sector + 12);
    if (!magic) {   /* new store */
        s->valid = true; s->next_free = 2; s->area = 0; s->area_used = 0;
        if (s->cow_chunks < 3) return -28;
        memset(s->area_buffer, 0, s->chunk * 512);
        return write_area(s) || write_header(s, true) ? -5 : 0;
    }
    if (magic != SNAP_MAGIC || version != 1 || chunk != s->chunk) return -22;   /* foreign, newer, or a different chunk size */
    s->valid = valid == 1;
    s->next_free = 2;
    if (!s->valid) return 0;   /* an invalidated snapshot loads, reports Invalid and fails I/O (as Linux) */
    uint64_t stride = (uint64_t)s->per_area + 1;
    for (s->area = 0; ; s->area++) {
        if (area_location(s, s->area) >= s->cow_chunks) { s->area_used = 0; break; }   /* the last area was full */
        if (block_part_read(s->cow, area_location(s, s->area) * s->chunk, (uint32_t)s->chunk, s->area_buffer) < 0) return -5;
        uint32_t i = 0;
        for (; i < s->per_area; i++) {
            uint64_t old = get_le64(s->area_buffer + i * 16), fresh = get_le64(s->area_buffer + i * 16 + 8);
            if (!fresh) break;
            if (fresh < 2 || fresh >= s->cow_chunks || fresh % stride == 1 || (s->origin_chunks && old >= s->origin_chunks) || snap_find(s, old)) return -22;
            if (snap_add(s, old, fresh)) return -12;
            if (fresh + 1 > s->next_free) s->next_free = fresh + 1;
        }
        if (i < s->per_area) { s->area_used = i; break; }
    }
    if (s->next_free % stride == 1) s->next_free++;
    if (s->next_free <= area_location(s, s->area)) s->next_free = area_location(s, s->area) + 1;
    return 0;
}
/* A new exception for origin chunk `old`: copy it into the COW and commit. Callers hold the snapshot lock. */
static int snap_allocate(struct snapshot* s, uint64_t old) {
    if (!s->valid || s->overflowed) return -5;
    if (s->next_free + 1 > s->cow_chunks) {
        if (s->overflow_mode) { s->overflowed = true; vga_print("[64] DM snapshot overflow: the COW is full\n"); }
        else snap_invalidate(s, "the COW device is full");
        return -28;
    }
    uint64_t fresh = s->next_free++;
    if (s->persistent && s->next_free % (s->per_area + 1) == 1) s->next_free++;
    uint64_t bytes = s->chunk * 512; uint8_t* copy = (void*)arena_alloc(bytes);
    if (!copy) return -12;
    uint64_t first = old * s->chunk, room = block_part_sectors(s->origin), n = first + s->chunk <= room ? s->chunk : room - first;
    memset(copy, 0, bytes);
    int e = block_part_read(s->origin, first, (uint32_t)n, copy) < 0 || block_part_write(s->cow, fresh * s->chunk, (uint32_t)s->chunk, copy) < 0 ? -5 : 0;
    arena_free((uint64_t)copy, bytes);
    if (!e && s->persistent) {   /* the data first, then the entry that points at it */
        if (block_flush(s->cow->dev) < 0) e = -5;
        if (!e) { put_le64(s->area_buffer + s->area_used * 16, old); put_le64(s->area_buffer + s->area_used * 16 + 8, fresh); e = write_area(s); }
        if (!e && ++s->area_used == s->per_area) {   /* the next area starts zeroed */
            s->area++; s->area_used = 0; memset(s->area_buffer, 0, s->chunk * 512);
            if (area_location(s, s->area) < s->cow_chunks) e = write_area(s);
        }
    }
    if (!e) e = snap_add(s, old, fresh);
    if (e) snap_invalidate(s, e == -12 ? "out of memory" : "COW I/O error");
    return e;
}

/* ---- targets ---- */
static int parse_snapshot(struct target* g, char* params, bool origin_only) {
    char* cursor = params; const char* end;
    char* origin = word(&cursor);
    if (!origin || !(g->under = device_of(origin))) return origin ? -6 : -22;
    if (origin_only) return word(&cursor) ? -22 : 0;
    char* cow = word(&cursor); char* mode = word(&cursor); char* chunk = word(&cursor); uint64_t c;
    if (!cow || !mode || !chunk || !parse_u64(chunk, &c, &end) || *end || c < 8 || c > 1024 || (c & (c - 1))) return -22;
    char* features = word(&cursor);
    if (features) {   /* discard_zeroes_cow, discard_passdown_origin (accepted, discards do not pass here) */
        uint64_t n; if (!parse_u64(features, &n, &end) || *end || n > 2) return -22;
        for (uint64_t i = 0; i < n; i++) { char* f = word(&cursor); if (!f || (strcmp(f, "discard_zeroes_cow") && strcmp(f, "discard_passdown_origin"))) return -22; }
        if (word(&cursor)) return -22;
    }
    bool persistent = mode[0] == 'P' || mode[0] == 'p', overflow = mode[1] == 'O' || mode[1] == 'o';
    if ((!persistent && mode[0] != 'N' && mode[0] != 'n') || (mode[1] && (!overflow || mode[2]))) return -22;
    struct block_partition* cow_dev = device_of(cow); if (!cow_dev) return -6;
    if (cow_dev == g->under || cow_dev->dev == g->under->dev) return -22;
    struct snapshot* s = (void*)arena_alloc(sizeof(*s)); if (!s) return -12;
    memset(s, 0, sizeof(*s));
    s->origin = g->under; s->cow = cow_dev; s->chunk = c; s->persistent = persistent; s->overflow_mode = overflow;
    s->cow_chunks = block_part_sectors(cow_dev) / c; s->origin_chunks = (block_part_sectors(g->under) + c - 1) / c;
    s->per_area = (uint32_t)(c * 512 / 16);
    uint64_t want = s->cow_chunks / 4; s->bucket_bits = 8; while (s->bucket_bits < 16 && (1ULL << s->bucket_bits) < want) s->bucket_bits++;
    s->buckets = (void*)arena_alloc((uint64_t)sizeof(void*) << s->bucket_bits);
    s->area_buffer = (void*)arena_alloc(c * 512);
    g->snap = s; g->more[0] = cow_dev; g->extra = 1;   /* free_table releases what this allocated, also on errors */
    if (!s->buckets || !s->area_buffer) return -12;
    memset(s->buckets, 0, (uint64_t)sizeof(void*) << s->bucket_bits);
    if (!persistent) { s->valid = true; s->next_free = 0; return 0; }
    int e = snap_read_store(s);
    if (e == -22) vga_print("[64] DM snapshot: COW metadata invalid or damaged: refused\n");
    return e;
}
static int snapshot_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {
    struct snapshot* s = g->snap;
    while (count) {
        uint64_t chunk = relative / s->chunk, within = relative % s->chunk;
        uint32_t n = s->chunk - within < count ? (uint32_t)(s->chunk - within) : count;
        snap_lock(s);
        int e = s->valid ? 0 : -5;
        struct snap_entry* x = e ? NULL : snap_find(s, chunk);
        if (!e && !x && write) { e = snap_allocate(s, chunk); if (!e) x = snap_find(s, chunk); }
        if (!e) {
            uint64_t at = x ? x->new_chunk * s->chunk + within : relative;
            struct block_partition* p = x ? s->cow : s->origin;
            if ((write ? block_part_write(p, at, n, data) : block_part_read(p, at, n, data)) < 0) e = -5;
        }
        snap_unlock(s);
        if (e) return -1;
        relative += n; count -= n; data += (uint64_t)n * 512;
    }
    return 0;
}
/* Before a write of [sector, sector + count) to `origin`: every valid snapshot of it saves the chunks it lacks. */
static int origin_cow(struct block_partition* origin, uint64_t sector, uint64_t count) {
    int result = 0;
    for (unsigned i = 0; i < DM_MAX; i++) {
        struct mapped* m = devices[i]; if (!m || m->gone) continue;
        struct table* t = table_get(m); if (!t) continue;
        for (unsigned k = 0; k < t->count; k++) {
            struct target* g = &t->t[k];
            if (g->type != T_SNAPSHOT || g->snap->origin != origin) continue;
            struct snapshot* s = g->snap;
            snap_lock(s);
            for (uint64_t c = sector / s->chunk; s->valid && !s->overflowed && c <= (sector + count - 1) / s->chunk; c++)
                if (!snap_find(s, c) && snap_allocate(s, c) == -5 && s->valid) result = -1;
            snap_unlock(s);
        }
        table_put(t);
    }
    return result;
}
static int origin_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {
    if (write && origin_cow(g->under, relative, count) < 0) return -1;
    return write ? block_part_write(g->under, relative, count, data) : block_part_read(g->under, relative, count, data);
}
static void snap_status(const struct target* g, bool table, char* text) {
    if (g->type == T_STRIPED) {
        put_number(text, g->stripes); strcat(text, " ");
        if (table) put_number(text, g->chunk);
        for (unsigned s = 0; s < g->stripes; s++) {
            uint64_t offset; struct block_partition* p = stripe_dev(g, s, &offset);
            if (table || s) strcat(text, " ");
            put_number(text, p->major); strcat(text, ":"); put_number(text, p->minor);
            if (table) { strcat(text, " "); put_number(text, offset); }
        }
        if (!table) { strcat(text, " 1 "); for (unsigned s = 0; s < g->stripes; s++) strcat(text, "A"); }
        return;
    }
    if (g->type == T_ORIGIN) { if (table) { put_number(text, g->under->major); strcat(text, ":"); put_number(text, g->under->minor); } return; }
    struct snapshot* s = g->snap;
    if (table) {
        put_number(text, s->origin->major); strcat(text, ":"); put_number(text, s->origin->minor); strcat(text, " ");
        put_number(text, s->cow->major); strcat(text, ":"); put_number(text, s->cow->minor);
        strcat(text, s->persistent ? (s->overflow_mode ? " PO " : " P ") : (s->overflow_mode ? " NO " : " N ")); put_number(text, s->chunk);
        return;
    }
    if (!s->valid) { strcat(text, "Invalid"); return; }
    if (s->overflowed) { strcat(text, "Overflow"); return; }
    put_number(text, s->next_free * s->chunk); strcat(text, "/"); put_number(text, block_part_sectors(s->cow)); strcat(text, " ");
    put_number(text, s->persistent ? (s->area + 2) * s->chunk : 0);
}
#pragma clang attribute pop
