/* dm-thin and thin-pool (Linux drivers/md/dm-thin.c, dm-thin-metadata.c, persistent-data/), included by dm.c, for
 * lvm2's thin pools (lvcreate -T / --thin, thin snapshots) and thin-provisioning-tools (thin_check, thin_dump).
 *
 * Metadata, as Linux writes it in 4 KiB blocks on the pool's metadata device: the superblock at block 0 (magic
 * 27022010, version 2, crc32c ^ 160774, transaction id, time, the two space-map roots, the data mapping and device
 * details roots, data block size, metadata size); btree nodes (header crc32c ^ 121107, flags internal/leaf, own block
 * number, nr/max entries -- max a multiple of 3 --, value size; keys then values); a two-level mapping tree (thin
 * device id -> its tree of virtual block -> data block << 24 | time) and the details tree (id -> mapped blocks,
 * transaction id, creation and snapshot times); space maps of 2-bit reference counts per block in bitmap blocks
 * (crc ^ 240779, 16320 entries each, count 3 = "see the overflow btree"), indexed by a btree (data) or one index block
 * (metadata, crc ^ 160478). In memory: every thin device's mappings (sorted) and a reference count per data block.
 * A commit (on a flush of a thin or pool device, after each pool message, when the pool's table goes) writes the
 * whole metadata again into blocks the committed metadata does not use, then the superblock: the old metadata stays
 * intact until the superblock switches, so a power cut leaves one consistent version (thin_check passes on either).
 * Data blocks freed in a transaction are not reused before it commits. Writes: an unshared mapped block is written
 * in place; an unmapped one gets a new block (zeroed around a partial write unless skip_block_zeroing); a block
 * shared with a snapshot is copied first (reference counts, as Linux's data space map). Out of data space: EIO with
 * error_if_no_space, else the write waits (queue_if_no_space) until the pool grows -- lvextend of the pool reloads
 * its table -- or dm_thin_pool.no_space_timeout seconds (default 60) pass. Messages: create_thin, create_snap,
 * delete, set_transaction_id. Status lines as Linux prints them. Limits: up to 2^22 data blocks per pool (256 GiB
 * with 64 KiB blocks), 1024 thin devices; not done: external origins, discards (ignore_discard behaviour), metadata
 * snapshots (reserve_metadata_snap: EINVAL), the periodic one-second commit (commits follow flushes). */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)   /* size: not a hot path (kernel.bin fits the BIOS boot partition) */
#define TP_BLOCK 4096u
#define TP_MAGIC 27022010ULL
#define TP_ENTRIES 16320u                 /* space-map entries per bitmap block */
#define TP_MAX_META_BITMAPS 255u
#define TP_MAX_DATA_BLOCKS (1u << 22)
#define TP_MAX_DEVS 1024u
#define TP_DEPTH 10
enum { BT_INTERNAL = 1, BT_LEAF = 2 };
#define CSUM_SB 160774u
#define CSUM_NODE 121107u
#define CSUM_BITMAP 240779u
#define CSUM_INDEX 160478u
uint32_t ext4_crc32c(uint32_t crc, const void* data, uint64_t length);
struct tmap { uint64_t vblock, value; };   /* value: data block << 24 | time */
struct tdev { uint64_t id, transaction_id; uint32_t creation_time, snapshotted_time; struct tmap* maps; uint64_t count, capacity; unsigned active; };
struct pool {
    struct pool* next; struct block_partition* meta; struct block_partition* data;
    uint32_t block_sectors, time, flags; uint64_t data_blocks, data_cap, meta_blocks, low_water, trans_id, cursor;
    uint8_t uuid[16];
    bool error_if_no_space, read_only, skip_zeroing, ignore_discard, no_passdown, out_of_space, dirty, failed;
    struct tdev* devs; unsigned ndevs, devcap;
    uint32_t* refs; uint8_t* committed;   /* data: reference counts now; referenced by the committed metadata */
    uint8_t* meta_used; uint64_t meta_used_count;   /* metadata blocks of the committed metadata */
    uint8_t* bounce; uint8_t* blk;
    unsigned users; volatile int busy; int space_channel;
};
struct thin { struct pool* pool; uint64_t id; };
static struct pool* pools;
static bool bit_get(const uint8_t* b, uint64_t i) { return b[i / 8] >> (i % 8) & 1; }
static void bit_set(uint8_t* b, uint64_t i, bool v) { if (v) b[i / 8] |= (uint8_t)(1u << (i % 8)); else b[i / 8] &= (uint8_t)~(1u << (i % 8)); }
static uint32_t pd_csum(const uint8_t* data, uint32_t len, uint32_t x) { return ext4_crc32c(0xFFFFFFFFu, data, len) ^ x; }
static uint32_t bt_max(uint32_t value_size) { return 3 * (((TP_BLOCK - 32) / (8 + value_size)) / 3); }
static void pool_lock(struct pool* p) {
    while (__atomic_exchange_n(&p->busy, 1, __ATOMIC_ACQUIRE)) {
        if (dm_may_wait()) kernel_io_sleep((void*)&p->busy, native_clock_ns() + 1000000ULL); else __asm__ volatile("pause");
    }
}
static void pool_unlock(struct pool* p) { __atomic_store_n(&p->busy, 0, __ATOMIC_RELEASE); task_wake_channel((void*)&p->busy); }
static int meta_read(struct pool* p, uint64_t block, uint8_t* out) { return block < p->meta_blocks ? block_part_read(p->meta, block * 8, 8, out) : -1; }
static int meta_write(struct pool* p, uint64_t block, const uint8_t* in) { return block_part_write(p->meta, block * 8, 8, in); }
static struct tdev* dev_find(struct pool* p, uint64_t id) { for (unsigned i = 0; i < p->ndevs; i++) if (p->devs[i].id == id) return &p->devs[i]; return NULL; }
static struct tdev* dev_add(struct pool* p, uint64_t id) {
    if (p->ndevs == p->devcap) {
        unsigned cap = p->devcap ? p->devcap * 2 : 16; if (cap > TP_MAX_DEVS) return NULL;
        struct tdev* n = (void*)arena_alloc(cap * sizeof(*n)); if (!n) return NULL;
        memset(n, 0, cap * sizeof(*n)); if (p->devs) { memcpy(n, p->devs, p->ndevs * sizeof(*n)); arena_free((uint64_t)p->devs, p->devcap * sizeof(*n)); }
        p->devs = n; p->devcap = cap;
    }
    struct tdev* d = &p->devs[p->ndevs++]; memset(d, 0, sizeof(*d)); d->id = id; return d;
}
static void dev_free_maps(struct tdev* d) { if (d->maps) arena_free((uint64_t)d->maps, d->capacity * sizeof(struct tmap)); d->maps = NULL; d->count = d->capacity = 0; }
static bool maps_reserve(struct tdev* d, uint64_t want) {
    if (want <= d->capacity) return true;
    uint64_t cap = d->capacity ? d->capacity * 2 : 256; while (cap < want) cap *= 2;
    struct tmap* n = (void*)arena_alloc(cap * sizeof(*n)); if (!n) return false;
    if (d->maps) { memcpy(n, d->maps, d->count * sizeof(*n)); arena_free((uint64_t)d->maps, d->capacity * sizeof(*n)); }
    d->maps = n; d->capacity = cap; return true;
}
static int64_t map_find(const struct tdev* d, uint64_t vblock) {   /* index, or -(insertion point) - 1 */
    int64_t lo = 0, hi = (int64_t)d->count - 1;
    while (lo <= hi) { int64_t mid = (lo + hi) / 2; if (d->maps[mid].vblock == vblock) return mid; if (d->maps[mid].vblock < vblock) lo = mid + 1; else hi = mid - 1; }
    return -lo - 1;
}
static bool data_resize(struct pool* p, uint64_t blocks) {   /* refs/committed for `blocks` data blocks */
    if (blocks > TP_MAX_DATA_BLOCKS) return false;
    if (blocks > p->data_cap) {
        uint32_t* r = (void*)arena_alloc(blocks * 4); uint8_t* c = (void*)arena_alloc((blocks + 7) / 8);
        if (!r || !c) { if (r) arena_free((uint64_t)r, blocks * 4); if (c) arena_free((uint64_t)c, (blocks + 7) / 8); return false; }
        memset(r, 0, blocks * 4); memset(c, 0, (blocks + 7) / 8);
        if (p->refs) { memcpy(r, p->refs, p->data_blocks * 4); memcpy(c, p->committed, (p->data_blocks + 7) / 8);
                       arena_free((uint64_t)p->refs, p->data_cap * 4); arena_free((uint64_t)p->committed, (p->data_cap + 7) / 8); }
        p->refs = r; p->committed = c; p->data_cap = blocks;
    }
    p->data_blocks = blocks; return true;
}

/* ---- reading metadata ---- */
struct walk { struct pool* p; int error; uint8_t* level[TP_DEPTH]; };
typedef int (*leaf_fn)(struct walk* w, uint64_t key, const uint8_t* value, void* ctx);
static int walk_tree(struct walk* w, uint64_t root, uint32_t value_size, unsigned depth, leaf_fn fn, void* ctx, uint64_t low, uint64_t high) {
    if (depth >= TP_DEPTH || root == 0 || root >= w->p->meta_blocks) return -22;
    uint8_t* b = w->level[depth];
    if (!b && !(b = w->level[depth] = (void*)arena_alloc(TP_BLOCK))) return -12;
    if (meta_read(w->p, root, b) < 0) return -5;
    uint32_t flags = get_le32(b + 4), n = get_le32(b + 16), max = get_le32(b + 20), vs = get_le32(b + 24);
    if (pd_csum(b + 4, TP_BLOCK - 4, CSUM_NODE) != get_le32(b) || get_le64(b + 8) != root || (flags != BT_INTERNAL && flags != BT_LEAF) ||
        vs != (flags == BT_INTERNAL ? 8 : value_size) || max % 3 || (uint64_t)max * (8 + vs) > TP_BLOCK - 32 || n > max) return -22;
    bit_set(w->p->meta_used, root, true);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t key = get_le64(b + 32 + 8 * i);
        if ((i && key <= get_le64(b + 32 + 8 * (i - 1))) || key < low || key > high) return -22;
        const uint8_t* v = b + 32 + 8 * max + (uint64_t)vs * i;
        if (flags == BT_LEAF) { int e = fn(w, key, v, ctx); if (e) return e; continue; }
        uint64_t next = i + 1 < n ? get_le64(b + 32 + 8 * (i + 1)) - 1 : high;
        int e = walk_tree(w, get_le64(v), value_size, depth + 1, fn, ctx, key, next);
        if (e) return e;
        if (meta_read(w->p, root, b) < 0) return -5;   /* the level buffer was reused below */
    }
    return 0;
}
static int leaf_mark(struct walk* w, uint64_t key, const uint8_t* value, void* ctx) { (void)w; (void)key; (void)value; (void)ctx; return 0; }
static int leaf_bitmap(struct walk* w, uint64_t key, const uint8_t* value, void* ctx) {   /* a data space-map index entry */
    (void)key; (void)ctx; uint64_t b = get_le64(value);
    if (!b || b >= w->p->meta_blocks || bit_get(w->p->meta_used, b)) return -22;
    bit_set(w->p->meta_used, b, true); return 0;
}
static int leaf_details(struct walk* w, uint64_t key, const uint8_t* value, void* ctx) {
    (void)ctx; struct pool* p = w->p;
    if (key >= (1u << 24) || dev_find(p, key)) return -22;
    struct tdev* d = dev_add(p, key); if (!d) return -12;
    d->transaction_id = get_le64(value + 8); d->creation_time = get_le32(value + 16); d->snapshotted_time = get_le32(value + 20);
    return 0;
}
static int leaf_mapping(struct walk* w, uint64_t key, const uint8_t* value, void* ctx) {
    struct tdev* d = ctx; uint64_t v = get_le64(value), block = v >> 24;
    if (block >= w->p->data_blocks || (d->count && key <= d->maps[d->count - 1].vblock) || !maps_reserve(d, d->count + 1)) return -22;
    d->maps[d->count].vblock = key; d->maps[d->count].value = v; d->count++;
    if (w->p->refs[block] == 0xFFFFFFFFu) return -22;
    w->p->refs[block]++;
    return 0;
}
static int leaf_top(struct walk* w, uint64_t key, const uint8_t* value, void* ctx) {
    (void)ctx; struct tdev* d = dev_find(w->p, key);
    if (!d || d->count) return -22;   /* a device without details, or twice */
    return walk_tree(w, get_le64(value), 8, 0 + 3, leaf_mapping, d, 0, ~0ULL);   /* depth 3+: the top tree uses 0..2 */
}
static int sm_metadata_walk(struct walk* w, const uint8_t* root) {   /* index block, its bitmaps, the overflow tree */
    struct pool* p = w->p; uint64_t index = get_le64(root + 16), rc = get_le64(root + 24);
    if (!index || index >= p->meta_blocks) return -22;
    uint8_t* b = p->blk; if (meta_read(p, index, b) < 0) return -5;
    if (pd_csum(b + 4, TP_BLOCK - 4, CSUM_INDEX) != get_le32(b) || get_le64(b + 8) != index) return -22;
    bit_set(p->meta_used, index, true);
    uint64_t bitmaps = (get_le64(root) + TP_ENTRIES - 1) / TP_ENTRIES;
    if (bitmaps > TP_MAX_META_BITMAPS) return -22;
    for (uint64_t i = 0; i < bitmaps; i++) {
        uint64_t at = get_le64(b + 16 + 16 * i);
        if (!at || at >= p->meta_blocks || bit_get(p->meta_used, at)) return -22;
        bit_set(p->meta_used, at, true);
    }
    return walk_tree(w, rc, 4, 0, leaf_mark, NULL, 0, ~0ULL);
}
static int pool_load(struct pool* p, const uint8_t* sb) {
    if (pd_csum(sb + 4, TP_BLOCK - 4, CSUM_SB) != get_le32(sb) || get_le64(sb + 8) != 0 || get_le64(sb + 32) != TP_MAGIC) return -22;
    uint32_t version = get_le32(sb + 40);
    if (version < 1 || version > 2 || get_le32(sb + 340) != 8 || get_le32(sb + 336) != p->block_sectors) return -22;
    if (get_le64(sb + 344) > p->meta_blocks || get_le64(sb + 56) || get_le32(sb + 360)) return -22;   /* a held metadata snapshot, incompat flags */
    uint64_t data_nr = get_le64(sb + 64);
    if (data_nr > p->data_blocks) return -22;   /* the data device shrank */
    p->flags = get_le32(sb + 4); p->time = get_le32(sb + 44); p->trans_id = get_le64(sb + 48); memcpy(p->uuid, sb + 16, 16);
    struct walk w; memset(&w, 0, sizeof(w)); w.p = p;
    bit_set(p->meta_used, 0, true);
    int e = walk_tree(&w, get_le64(sb + 328), 24, 0, leaf_details, NULL, 0, ~0ULL);
    if (!e) e = walk_tree(&w, get_le64(sb + 320), 8, 0, leaf_top, NULL, 0, ~0ULL);
    if (!e) e = walk_tree(&w, get_le64(sb + 64 + 16), 16, 0, leaf_bitmap, NULL, 0, ~0ULL);   /* data space map: index tree + bitmaps */
    if (!e) e = walk_tree(&w, get_le64(sb + 64 + 24), 4, 0, leaf_mark, NULL, 0, ~0ULL);
    if (!e) e = sm_metadata_walk(&w, sb + 192);
    for (unsigned i = 0; i < TP_DEPTH; i++) if (w.level[i]) arena_free((uint64_t)w.level[i], TP_BLOCK);
    if (e) return e;
    p->meta_used_count = 0;
    for (uint64_t b = 0; b < p->meta_blocks; b++) p->meta_used_count += bit_get(p->meta_used, b);
    for (uint64_t b = 0; b < p->data_blocks; b++) bit_set(p->committed, b, p->refs[b] != 0);
    return 0;
}

/* ---- writing metadata (commit) ---- */
struct builder { struct pool* p; uint8_t* used; uint64_t scan, count; int error; uint8_t* blk; };
static uint64_t meta_alloc(struct builder* w) {
    struct pool* p = w->p;
    for (; w->scan < p->meta_blocks; w->scan++)
        if (w->scan && !bit_get(p->meta_used, w->scan) && !bit_get(w->used, w->scan)) { bit_set(w->used, w->scan, true); w->count++; return w->scan++; }
    w->error = -28; return 0;
}
static void node_finish(struct builder* w, uint64_t where) {
    put_le64(w->blk + 8, where); put_le32(w->blk, pd_csum(w->blk + 4, TP_BLOCK - 4, CSUM_NODE));
    if (!w->error && meta_write(w->p, where, w->blk) < 0) w->error = -5;
}
typedef void (*entry_fn)(void* ctx, uint64_t i, uint64_t* key, uint8_t* value);
static uint64_t build_tree(struct builder* w, uint32_t value_size, uint64_t n, entry_fn get, void* ctx) {
    uint32_t max = bt_max(value_size);
    uint64_t leaves = n ? (n + max - 1) / max : 1;
    uint64_t* level = (void*)arena_alloc(leaves * 16); if (!level) { w->error = -12; return 0; }
    for (uint64_t l = 0, at = 0; l < leaves; l++) {   /* even leaves: never under a third full */
        uint64_t take = n / leaves + (l < n % leaves);
        memset(w->blk, 0, TP_BLOCK); put_le32(w->blk + 4, BT_LEAF); put_le32(w->blk + 16, (uint32_t)take); put_le32(w->blk + 20, max); put_le32(w->blk + 24, value_size);
        for (uint64_t i = 0; i < take; i++) { uint64_t key; get(ctx, at + i, &key, w->blk + 32 + 8 * max + value_size * i); put_le64(w->blk + 32 + 8 * i, key); }
        uint64_t where = meta_alloc(w); level[2 * l] = take ? get_le64(w->blk + 32) : 0; level[2 * l + 1] = where;
        node_finish(w, where); at += take;
    }
    uint64_t count = leaves; uint32_t imax = bt_max(8);
    while (count > 1 && !w->error) {
        uint64_t parents = (count + imax - 1) / imax;
        for (uint64_t q = 0, at = 0; q < parents; q++) {
            uint64_t take = count / parents + (q < count % parents);
            memset(w->blk, 0, TP_BLOCK); put_le32(w->blk + 4, BT_INTERNAL); put_le32(w->blk + 16, (uint32_t)take); put_le32(w->blk + 20, imax); put_le32(w->blk + 24, 8);
            for (uint64_t i = 0; i < take; i++) { put_le64(w->blk + 32 + 8 * i, level[2 * (at + i)]); put_le64(w->blk + 32 + 8 * imax + 8 * i, level[2 * (at + i) + 1]); }
            uint64_t where = meta_alloc(w); uint64_t first = level[2 * at];
            node_finish(w, where);
            level[2 * q] = first; level[2 * q + 1] = where; at += take;   /* q <= at: in place */
        }
        count = parents;
    }
    uint64_t root = level[1]; arena_free((uint64_t)level, leaves * 16);
    return w->error ? 0 : root;
}
static void entry_details(void* ctx, uint64_t i, uint64_t* key, uint8_t* v) {
    struct pool* p = ctx; struct tdev* d = &p->devs[i]; *key = d->id;
    put_le64(v, d->count); put_le64(v + 8, d->transaction_id); put_le32(v + 16, d->creation_time); put_le32(v + 20, d->snapshotted_time);
}
static void entry_mapping(void* ctx, uint64_t i, uint64_t* key, uint8_t* v) { struct tdev* d = ctx; *key = d->maps[i].vblock; put_le64(v, d->maps[i].value); }
struct top_ctx { struct pool* p; uint64_t* roots; };
static void entry_top(void* ctx, uint64_t i, uint64_t* key, uint8_t* v) { struct top_ctx* t = ctx; *key = t->p->devs[i].id; put_le64(v, t->roots[i]); }
struct rc_ctx { struct pool* p; uint64_t* blocks; };
static void entry_rc(void* ctx, uint64_t i, uint64_t* key, uint8_t* v) { struct rc_ctx* r = ctx; *key = r->blocks[i]; put_le32(v, r->p->refs[r->blocks[i]]); }
struct ie_ctx { uint64_t* ie; };   /* per bitmap: blocknr, nr_free, none_free_before */
static void entry_ie(void* ctx, uint64_t i, uint64_t* key, uint8_t* v) { struct ie_ctx* c = ctx; *key = i; put_le64(v, c->ie[3 * i]); put_le32(v + 8, (uint32_t)c->ie[3 * i + 1]); put_le32(v + 12, (uint32_t)c->ie[3 * i + 2]); }
/* One space-map bitmap block for entries [first, first + TP_ENTRIES) of count(): written to `where`; nr_free etc. */
static void write_bitmap(struct builder* w, uint64_t where, uint64_t first, uint64_t total, uint32_t (*count)(struct builder*, uint64_t), uint64_t* ie) {
    memset(w->blk, 0, TP_BLOCK); uint64_t used = 0, first_free = TP_ENTRIES;
    for (uint64_t e = 0; e < TP_ENTRIES; e++) {
        uint64_t b = first + e; uint32_t c = b < total ? count(w, b) : 0; uint32_t v = c > 2 ? 3 : c;
        if (v) { used++; uint32_t bit = (uint32_t)((e % 32) * 2); uint8_t* word = w->blk + 16 + (e / 32) * 8;
                 if (v & 2) word[bit / 8] |= (uint8_t)(1u << (bit % 8)); if (v & 1) word[(bit + 1) / 8] |= (uint8_t)(1u << ((bit + 1) % 8)); }
        else if (first_free == TP_ENTRIES) first_free = e;
    }
    put_le64(w->blk + 8, where); put_le32(w->blk, pd_csum(w->blk + 4, TP_BLOCK - 4, CSUM_BITMAP));
    if (!w->error && meta_write(w->p, where, w->blk) < 0) w->error = -5;
    ie[0] = where; ie[1] = TP_ENTRIES - used; ie[2] = first_free;
}
static uint32_t count_data(struct builder* w, uint64_t b) { return w->p->refs[b]; }
static uint32_t count_meta(struct builder* w, uint64_t b) { return b == 0 || bit_get(w->used, b) ? 1 : 0; }
static int pool_commit(struct pool* p) {
    if (!p->dirty || p->failed || p->read_only) return p->failed ? -5 : 0;
    if (block_flush(p->data->dev) < 0) return -5;   /* the data before the mappings that point at it */
    struct builder w; memset(&w, 0, sizeof(w)); w.p = p; w.blk = p->blk;
    w.used = (void*)arena_alloc((p->meta_blocks + 7) / 8); if (!w.used) return -12;
    memset(w.used, 0, (p->meta_blocks + 7) / 8);
    uint64_t mbitmaps = (p->meta_blocks + TP_ENTRIES - 1) / TP_ENTRIES, dbitmaps = (p->data_blocks + TP_ENTRIES - 1) / TP_ENTRIES;
    uint64_t mreserve[TP_MAX_META_BITMAPS + 2];   /* the metadata space map's own blocks, counted in itself */
    for (uint64_t i = 0; i < mbitmaps + 2; i++) mreserve[i] = meta_alloc(&w);
    uint64_t* roots = (void*)arena_alloc((p->ndevs + 1) * 8); uint64_t* ie = (void*)arena_alloc((dbitmaps + 1) * 24 + (mbitmaps + 1) * 24);
    uint64_t overflow = 0; for (uint64_t b = 0; b < p->data_blocks; b++) overflow += p->refs[b] > 2;
    uint64_t* rcb = (void*)arena_alloc((overflow + 1) * 8);
    uint64_t details = 0, top = 0, dindex = 0, drc = 0;
    if (!roots || !ie || !rcb) w.error = -12;
    if (!w.error) {
        details = build_tree(&w, 24, p->ndevs, entry_details, p);
        for (unsigned i = 0; i < p->ndevs && !w.error; i++) roots[i] = build_tree(&w, 8, p->devs[i].count, entry_mapping, &p->devs[i]);
        struct top_ctx tc = {p, roots}; if (!w.error) top = build_tree(&w, 8, p->ndevs, entry_top, &tc);
        for (uint64_t i = 0; i < dbitmaps && !w.error; i++) write_bitmap(&w, meta_alloc(&w), i * TP_ENTRIES, p->data_blocks, count_data, ie + 3 * i);
        struct ie_ctx ic = {ie}; if (!w.error) dindex = build_tree(&w, 16, dbitmaps, entry_ie, &ic);
        uint64_t k = 0; for (uint64_t b = 0; b < p->data_blocks; b++) if (p->refs[b] > 2) rcb[k++] = b;
        struct rc_ctx rc = {p, rcb}; if (!w.error) drc = build_tree(&w, 4, overflow, entry_rc, &rc);
    }
    uint64_t* mie = ie + 3 * (dbitmaps + 1);
    if (!w.error) {   /* metadata space map: every block of this commit and the superblock count 1 */
        for (uint64_t i = 0; i < mbitmaps && !w.error; i++) write_bitmap(&w, mreserve[i], i * TP_ENTRIES, p->meta_blocks, count_meta, mie + 3 * i);
        memset(w.blk, 0, TP_BLOCK); put_le32(w.blk + 4, BT_LEAF); put_le32(w.blk + 20, bt_max(4)); put_le32(w.blk + 24, 4);   /* empty overflow tree */
        node_finish(&w, mreserve[mbitmaps + 1]);
        memset(w.blk, 0, TP_BLOCK);
        for (uint64_t i = 0; i < mbitmaps; i++) { put_le64(w.blk + 16 + 16 * i, mie[3 * i]); put_le32(w.blk + 24 + 16 * i, (uint32_t)mie[3 * i + 1]); put_le32(w.blk + 28 + 16 * i, (uint32_t)mie[3 * i + 2]); }
        put_le64(w.blk + 8, mreserve[mbitmaps]); put_le32(w.blk, pd_csum(w.blk + 4, TP_BLOCK - 4, CSUM_INDEX));
        if (meta_write(p, mreserve[mbitmaps], w.blk) < 0) w.error = -5;
    }
    if (!w.error && block_flush(p->meta->dev) < 0) w.error = -5;
    if (!w.error) {   /* the switch: the superblock */
        uint64_t allocated = 0; for (uint64_t b = 0; b < p->data_blocks; b++) allocated += p->refs[b] != 0;
        uint8_t* sb = w.blk; memset(sb, 0, TP_BLOCK);
        put_le32(sb + 4, p->flags); memcpy(sb + 16, p->uuid, 16); put_le64(sb + 32, TP_MAGIC); put_le32(sb + 40, 2); put_le32(sb + 44, p->time);
        put_le64(sb + 48, p->trans_id);
        put_le64(sb + 64, p->data_blocks); put_le64(sb + 72, allocated); put_le64(sb + 80, dindex); put_le64(sb + 88, drc);
        put_le64(sb + 192, p->meta_blocks); put_le64(sb + 200, w.count + 1); put_le64(sb + 208, mreserve[mbitmaps]); put_le64(sb + 216, mreserve[mbitmaps + 1]);
        put_le64(sb + 320, top); put_le64(sb + 328, details); put_le32(sb + 336, p->block_sectors); put_le32(sb + 340, 8); put_le64(sb + 344, p->meta_blocks);
        put_le32(sb, pd_csum(sb + 4, TP_BLOCK - 4, CSUM_SB));
        if (meta_write(p, 0, sb) < 0 || block_flush(p->meta->dev) < 0) w.error = -5;
    }
    if (roots) arena_free((uint64_t)roots, (p->ndevs + 1) * 8);
    if (ie) arena_free((uint64_t)ie, (dbitmaps + 1) * 24 + (mbitmaps + 1) * 24);
    if (rcb) arena_free((uint64_t)rcb, (overflow + 1) * 8);
    if (w.error) {
        arena_free((uint64_t)w.used, (p->meta_blocks + 7) / 8);
        vga_print(w.error == -28 ? "[64] DM thin-pool: metadata device full, the pool is read-only\n" : "[64] DM thin-pool: metadata commit failed, the pool is read-only\n");
        p->failed = true; return w.error;
    }
    bit_set(w.used, 0, true);
    arena_free((uint64_t)p->meta_used, (p->meta_blocks + 7) / 8); p->meta_used = w.used; p->meta_used_count = w.count + 1;
    for (uint64_t b = 0; b < p->data_blocks; b++) bit_set(p->committed, b, p->refs[b] != 0);
    p->dirty = false;
    task_wake_channel(&p->space_channel);   /* blocks freed by this transaction can be used now */
    return 0;
}

/* ---- pools ---- */
static void pool_free_memory(struct pool* p) {
    for (unsigned i = 0; i < p->ndevs; i++) dev_free_maps(&p->devs[i]);
    if (p->devs) arena_free((uint64_t)p->devs, p->devcap * sizeof(struct tdev));
    if (p->refs) arena_free((uint64_t)p->refs, p->data_cap * 4);
    if (p->committed) arena_free((uint64_t)p->committed, (p->data_cap + 7) / 8);
    if (p->meta_used) arena_free((uint64_t)p->meta_used, (p->meta_blocks + 7) / 8);
    if (p->bounce) arena_free((uint64_t)p->bounce, (uint64_t)p->block_sectors * 512);
    if (p->blk) arena_free((uint64_t)p->blk, TP_BLOCK);
    arena_free((uint64_t)p, sizeof(*p));
}
static void pool_release(struct pool* p) {   /* the last table using the pool commits it and lets it go */
    if (!p || --p->users) return;
    pool_lock(p); pool_commit(p); pool_unlock(p);
    for (struct pool** q = &pools; *q; q = &(*q)->next) if (*q == p) { *q = p->next; break; }
    pool_free_memory(p);
}
static int pool_open(struct block_partition* meta, struct block_partition* data, uint32_t block_sectors, struct pool** out) {
    for (struct pool* p = pools; p; p = p->next) if (p->meta == meta) {   /* a reload: the same pool */
        if (p->block_sectors != block_sectors) return -22;
        uint64_t blocks = block_part_sectors(data) / block_sectors;
        pool_lock(p);
        bool shrink = false;
        if (blocks < p->data_blocks) for (uint64_t b = blocks; b < p->data_blocks; b++) if (p->refs[b]) shrink = true;
        pool_unlock(p);
        if (shrink) return -22;   /* data in use beyond the new end */
        p->users++; *out = p; return 0;   /* a bigger data device is taken at resume (thinpool_resume) */
    }
    struct pool* p = (void*)arena_alloc(sizeof(*p)); if (!p) return -12;
    memset(p, 0, sizeof(*p)); p->meta = meta; p->data = data; p->block_sectors = block_sectors;
    p->meta_blocks = block_part_sectors(meta) / 8; if (p->meta_blocks > 255ULL * TP_ENTRIES) p->meta_blocks = 255ULL * TP_ENTRIES;
    int e = 0;
    if (p->meta_blocks < 16) e = -22;
    if (!e && (!(p->blk = (void*)arena_alloc(TP_BLOCK)) || !(p->bounce = (void*)arena_alloc((uint64_t)block_sectors * 512)) ||
               !(p->meta_used = (void*)arena_alloc((p->meta_blocks + 7) / 8)) || !data_resize(p, block_part_sectors(data) / block_sectors))) e = -12;
    if (!e) {
        memset(p->meta_used, 0, (p->meta_blocks + 7) / 8);
        e = meta_read(p, 0, p->blk) < 0 ? -5 : 0;
        bool zero = true; for (unsigned i = 0; !e && i < TP_BLOCK; i++) if (p->blk[i]) zero = false;
        uint8_t* sb = (void*)arena_alloc(TP_BLOCK);
        if (!e && !sb) e = -12;
        if (!e && zero) { p->dirty = true; vga_print("[64] DM thin-pool: formatting the metadata\n"); }
        else if (!e) { memcpy(sb, p->blk, TP_BLOCK); e = pool_load(p, sb); if (e) vga_print("[64] DM thin-pool: metadata refused (checksums, layout or references)\n"); }
        if (sb) arena_free((uint64_t)sb, TP_BLOCK);
        if (!e && p->dirty) e = pool_commit(p);
    }
    if (e) { pool_free_memory(p); return e; }
    p->users = 1; p->next = pools; pools = p; *out = p; return 0;
}
static int parse_thinpool(struct target* g, char* params, bool readonly) {
    char* c = params; char *meta = word(&c), *data = word(&c), *bs = word(&c), *low = word(&c);
    uint64_t block, water; const char* end;
    if (!low || !parse_u64(bs, &block, &end) || *end || !parse_u64(low, &water, &end) || *end) return -22;
    if (block < 128 || block > 2097152 || block % 128) return -22;   /* 64 KiB .. 1 GiB, multiples of 64 KiB */
    bool flags[5] = {0}; static const char* const names[5] = {"skip_block_zeroing", "ignore_discard", "no_discard_passdown", "read_only", "error_if_no_space"};
    char* count_text = word(&c);
    if (count_text) {
        uint64_t count; if (!parse_u64(count_text, &count, &end) || *end || count > 5) return -22;
        for (uint64_t i = 0; i < count; i++) {
            char* o = word(&c); bool known = false; if (!o) return -22;
            for (unsigned f = 0; f < 5; f++) if (!strcmp(o, names[f])) { flags[f] = true; known = true; }
            if (!known) return -22;
        }
    }
    if (word(&c)) return -22;
    g->under = device_of(data); g->more[0] = device_of(meta); g->extra = 1;
    if (!g->under || !g->more[0]) return -6;
    if (g->under == g->more[0]) return -22;
    struct pool* p; int e = pool_open(g->more[0], g->under, (uint32_t)block, &p); if (e) return e;
    p->skip_zeroing = flags[0]; p->ignore_discard = flags[1]; p->no_passdown = flags[2]; p->read_only = flags[3] || readonly; p->error_if_no_space = flags[4];
    p->low_water = water; g->pool = p;
    return 0;
}
static int parse_thin(struct target* g, char* params) {
    char* c = params; char *pool_dev = word(&c), *id_text = word(&c);
    uint64_t id; const char* end;
    if (!id_text || !parse_u64(id_text, &id, &end) || *end || id >= (1u << 24)) return -22;
    if (word(&c)) return -22;   /* an external origin: not done */
    g->under = device_of(pool_dev); if (!g->under) return -6;
    struct mapped* m = mapped_of(g->under->dev); struct table* t = m ? m->live : NULL; struct pool* p = NULL;
    for (unsigned i = 0; t && i < t->count; i++) if (t->t[i].type == T_THINPOOL) p = t->t[i].pool;
    if (!p) return -22;
    pool_lock(p);
    struct tdev* d = dev_find(p, id);
    if (d) d->active++;
    pool_unlock(p);
    if (!d) return -61;   /* ENODATA: no such thin device */
    struct thin* th = (void*)arena_alloc(sizeof(*th)); if (!th) { pool_lock(p); d->active--; pool_unlock(p); return -12; }
    th->pool = p; th->id = id; p->users++; g->thin = th;
    return 0;
}
static void thin_target_free(struct target* g) {
    if (g->thin) {
        struct pool* p = g->thin->pool; pool_lock(p);
        struct tdev* d = dev_find(p, g->thin->id); if (d && d->active) d->active--;
        pool_commit(p); pool_unlock(p);
        arena_free((uint64_t)g->thin, sizeof(struct thin)); g->thin = NULL;
        pool_release(p);
    }
    if (g->pool) { pool_release(g->pool); g->pool = NULL; }
}

/* ---- I/O ---- */
static int64_t data_alloc(struct pool* p) {   /* a free data block not referenced by the committed metadata */
    for (uint64_t n = 0; n < p->data_blocks; n++) {
        uint64_t b = (p->cursor + n) % p->data_blocks;
        if (!p->refs[b] && !bit_get(p->committed, b)) { p->cursor = b + 1; return (int64_t)b; }
    }
    return -1;
}
static bool pool_wait_space(struct pool* p) {   /* queue_if_no_space: until the pool grows (or blocks are freed) */
    char value[16]; uint64_t timeout = 60; const char* end;
    if (boot_value("dm_thin_pool.no_space_timeout", value, sizeof(value)) && (!parse_u64(value, &timeout, &end) || *end)) timeout = 60;
    if (!dm_may_wait()) return false;
    uint64_t deadline = native_clock_ns() + timeout * 1000000000ULL;
    while (native_clock_ns() < deadline) {
        kernel_io_sleep(&p->space_channel, native_clock_ns() + 100000000ULL);
        pool_lock(p); bool room = !p->out_of_space; pool_unlock(p);
        if (room) return true;
    }
    return false;
}
static int thin_block_write(struct pool* p, struct tdev* d, uint64_t vblock, uint32_t off, uint32_t n, const uint8_t* data) {
    uint32_t spb = p->block_sectors; int64_t at = map_find(d, vblock);
    if (at >= 0) {
        uint64_t old = d->maps[at].value >> 24;
        if (p->refs[old] == 1) return block_part_write(p->data, old * spb + off, n, data) < 0 ? -5 : 0;   /* unshared: in place */
    }
    int64_t fresh = data_alloc(p);
    if (fresh < 0 && p->dirty) { pool_commit(p); fresh = data_alloc(p); }   /* blocks freed in this transaction */
    if (fresh < 0) { if (!p->out_of_space) vga_print("[64] DM thin-pool: out of data space\n"); p->out_of_space = true; return -28; }
    uint64_t nb = (uint64_t)fresh;
    if (n < spb || at >= 0) {   /* the rest of the block: the shared old data, or zeroes */
        if (at >= 0) { if (block_part_read(p->data, (d->maps[at].value >> 24) * spb, spb, p->bounce) < 0) return -5; }
        else if (!p->skip_zeroing) memset(p->bounce, 0, (uint64_t)spb * 512);
        if (at >= 0 || !p->skip_zeroing) {
            memcpy(p->bounce + (uint64_t)off * 512, data, (uint64_t)n * 512);
            if (block_part_write(p->data, nb * spb, spb, p->bounce) < 0) return -5;
        } else if (block_part_write(p->data, nb * spb + off, n, data) < 0) return -5;
    } else if (block_part_write(p->data, nb * spb, spb, data) < 0) return -5;
    p->refs[nb] = 1; p->dirty = true;
    uint64_t value = nb << 24 | (p->time & 0xFFFFFF);
    if (at >= 0) { p->refs[d->maps[at].value >> 24]--; d->maps[at].value = value; return 0; }
    if (!maps_reserve(d, d->count + 1)) { p->refs[nb] = 0; return -12; }
    uint64_t pos = (uint64_t)(-at - 1);
    for (uint64_t k = d->count; k > pos; k--) d->maps[k] = d->maps[k - 1];
    d->maps[pos].vblock = vblock; d->maps[pos].value = value; d->count++;
    return 0;
}
static int thin_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {
    struct pool* p = g->thin->pool; uint32_t spb = p->block_sectors;
    while (count) {
        uint64_t vblock = relative / spb; uint32_t off = (uint32_t)(relative % spb), n = spb - off < count ? spb - off : count;
        pool_lock(p);
        struct tdev* d = dev_find(p, g->thin->id); int e = d ? 0 : -5;
        if (!e && !write) {
            int64_t at = map_find(d, vblock);
            if (at < 0) memset(data, 0, (uint64_t)n * 512);
            else e = block_part_read(p->data, (d->maps[at].value >> 24) * spb + off, n, data) < 0 ? -5 : 0;
        } else if (!e) e = p->read_only || p->failed ? -30 : thin_block_write(p, d, vblock, off, n, data);
        pool_unlock(p);
        if (e == -28) { if (p->error_if_no_space || !pool_wait_space(p)) return -1; continue; }   /* retried after the pool grew */
        if (e) return -1;
        relative += n; count -= n; data += (uint64_t)n * 512;
    }
    return 0;
}
static int thinpool_io(struct target* g, uint64_t relative, uint32_t count, uint8_t* data, bool write) {   /* the pool device: its data device */
    return under_io(g, relative, count, data, write);
}

/* ---- messages and status ---- */
static int thinpool_message(struct target* g, char* text) {
    struct pool* p = g->pool; char* c = text; char *cmd = word(&c), *a1 = word(&c), *a2 = word(&c);
    uint64_t x = 0, y = 0; const char* end;
    if (!cmd || (a1 && (!parse_u64(a1, &x, &end) || *end)) || (a2 && (!parse_u64(a2, &y, &end) || *end)) || word(&c)) return -22;
    pool_lock(p);
    int e = 0;
    if (p->read_only || p->failed) e = -30;
    else if (!strcmp(cmd, "create_thin") && a1 && !a2) {
        if (x >= (1u << 24) || dev_find(p, x)) e = x >= (1u << 24) ? -22 : -17;
        else { struct tdev* d = dev_add(p, x); if (!d) e = -12; else { d->transaction_id = p->trans_id; d->creation_time = d->snapshotted_time = p->time; } }
    } else if (!strcmp(cmd, "create_snap") && a1 && a2) {
        struct tdev* o = dev_find(p, y);
        if (x >= (1u << 24) || dev_find(p, x)) e = x >= (1u << 24) ? -22 : -17;
        else if (!o) e = -61;
        else {
            unsigned oi = (unsigned)(o - p->devs); struct tdev* d = dev_add(p, x); o = &p->devs[oi];   /* dev_add may move the array */
            if (!d || !maps_reserve(d, o->count ? o->count : 1)) { if (d) p->ndevs--; e = -12; }
            else {
                memcpy(d->maps, o->maps, o->count * sizeof(struct tmap)); d->count = o->count;
                for (uint64_t i = 0; i < d->count; i++) p->refs[d->maps[i].value >> 24]++;
                p->time++; d->transaction_id = p->trans_id; d->creation_time = d->snapshotted_time = o->snapshotted_time = p->time;
            }
        }
    } else if (!strcmp(cmd, "delete") && a1 && !a2) {
        struct tdev* d = dev_find(p, x);
        if (!d) e = -61;
        else if (d->active) e = -16;
        else {
            for (uint64_t i = 0; i < d->count; i++) p->refs[d->maps[i].value >> 24]--;
            dev_free_maps(d); *d = p->devs[--p->ndevs];
        }
    } else if (!strcmp(cmd, "set_transaction_id") && a1 && a2) { if (x != p->trans_id) e = -22; else p->trans_id = y; }
    else e = -22;   /* reserve_metadata_snap, release_metadata_snap, trim: not done */
    if (!e) { p->dirty = true; e = pool_commit(p); }
    pool_unlock(p);
    return e;
}
static void put_devt(char* text, const struct block_partition* p) { put_number(text, p->major); strcat(text, ":"); put_number(text, p->minor); }
static void thin_status(const struct target* g, bool table, char* text) {
    if (g->type == T_THINPOOL) {
        struct pool* p = g->pool;
        if (table) {   /* Linux pool_status STATUSTYPE_TABLE + emit_flags */
            put_devt(text, g->more[0]); strcat(text, " "); put_devt(text, g->under); strcat(text, " "); put_number(text, p->block_sectors); strcat(text, " ");
            put_number(text, p->low_water); strcat(text, " ");
            unsigned count = p->skip_zeroing + p->ignore_discard + p->no_passdown + p->read_only + p->error_if_no_space;
            put_number(text, count); strcat(text, " ");
            if (p->skip_zeroing) strcat(text, "skip_block_zeroing ");
            if (p->ignore_discard) strcat(text, "ignore_discard ");
            if (p->no_passdown) strcat(text, "no_discard_passdown ");
            if (p->read_only) strcat(text, "read_only ");
            if (p->error_if_no_space) strcat(text, "error_if_no_space ");
            return;
        }
        uint64_t used = 0; for (uint64_t b = 0; b < p->data_blocks; b++) used += p->refs[b] != 0;
        put_number(text, p->trans_id); strcat(text, " "); put_number(text, p->meta_used_count); strcat(text, "/"); put_number(text, p->meta_blocks); strcat(text, " ");
        put_number(text, used); strcat(text, "/"); put_number(text, p->data_blocks); strcat(text, " - ");
        strcat(text, p->out_of_space ? "out_of_data_space " : p->read_only || p->failed ? "ro " : "rw ");
        strcat(text, p->ignore_discard ? "ignore_discard " : p->no_passdown ? "no_discard_passdown " : "discard_passdown ");
        strcat(text, p->error_if_no_space ? "error_if_no_space " : "queue_if_no_space ");
        strcat(text, p->flags & 1 ? "needs_check " : "- ");
        put_number(text, p->meta_blocks / 4 < 1024 ? p->meta_blocks / 4 : 1024);
        return;
    }
    if (table) { put_devt(text, g->under); strcat(text, " "); put_number(text, g->thin->id); return; }
    struct pool* p = g->thin->pool; pool_lock(p);
    struct tdev* d = dev_find(p, g->thin->id);
    uint64_t mapped = d ? d->count : 0, highest = d && d->count ? d->maps[d->count - 1].vblock : 0;
    pool_unlock(p);
    put_number(text, mapped * p->block_sectors); strcat(text, " ");
    if (mapped) put_number(text, (highest + 1) * p->block_sectors - 1); else strcat(text, "-");
}
static void thinpool_resume(struct target* g) {   /* Linux pool_preresume: the data device may have grown (lvextend) */
    struct pool* p = g->pool; uint64_t blocks = block_part_sectors(g->under) / p->block_sectors;
    pool_lock(p);
    bool grew = blocks > p->data_blocks && data_resize(p, blocks);
    if (grew) { p->data = g->under; p->dirty = true; p->out_of_space = false; vga_print("[64] DM thin-pool: data device grown\n"); pool_commit(p); }
    pool_unlock(p);
    if (grew) task_wake_channel(&p->space_channel);
}
static void thin_flush(struct target* g) {   /* a flush of a thin or pool device commits the pool */
    struct pool* p = g->type == T_THIN ? g->thin->pool : g->pool; if (!p) return;
    pool_lock(p); pool_commit(p); pool_unlock(p);
}
#pragma clang attribute pop
