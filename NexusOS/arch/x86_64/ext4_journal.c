/* ext4 metadata block cache and jbd2 journal (ordered metadata journaling, checksum v3); the disk
 * itself is reached through ext4_dev.c. Every metadata change joins the running transaction; a commit appends the
 * descriptor + block copies to the log, flushes, writes the commit block and flushes again: the
 * transaction is durable. Committed blocks are written to their home locations lazily (the first
 * time a block is touched again, or by a checkpoint when the log fills, the cache needs room, the log
 * has been idle for 30 s, or at unmount); a checkpoint flushes, then empties the log (superblock
 * write + flush). File data is written in place before the transaction that references it commits
 * (ordered mode). Blocks freed by a transaction stay unallocatable until the log holding it has been
 * checkpointed (ext4.c quarantine), so replay never overwrites reused blocks and no revoke records
 * are needed. */
#include "ext4_internal.h"
#include "../../kernel/string.h"

/* ------------------------------------------------------------------ crc32c */
static uint32_t crc_table[256];
static bool crc_ready, crc_hw;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        crc_table[i] = c;
    }
    uint32_t a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    crc_hw = (c >> 20) & 1; /* SSE4.2 CRC32 works on general registers */
    crc_ready = true;
}
uint32_t ext4_crc32c(uint32_t crc, const void* data, uint64_t length) {
    if (!crc_ready) crc_init();
    const uint8_t* p = data;
    if (crc_hw) {
        uint64_t c = crc;
        while (length >= 8) { uint64_t v; memcpy(&v, p, 8); __asm__("crc32q %1,%0" : "+r"(c) : "rm"(v)); p += 8; length -= 8; }
        uint32_t c32 = (uint32_t)c;
        while (length--) __asm__("crc32b %1,%0" : "+r"(c32) : "rm"(*p++));
        return c32;
    }
    while (length--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* ------------------------------------------------------------------ block cache */
static struct ext4_buf* buffers;
static unsigned buffer_count, hash_size;
static struct ext4_buf** hash_heads;
static struct ext4_buf lru; /* sentinel: lru.lru_next = most recent */
static struct ext4_buf* txn_head;
static unsigned txn_count, txn_depth, txn_limit;
static uint64_t txn_first_ns;
static bool committing;
static uint8_t* staging;                 /* contiguous I/O bounce area + 3 scratch blocks */
#define STAGING_BLOCKS 64u
#define STAGING_RUN 60u                  /* blocks [60,64) are descriptor/copy/commit scratch */
#define SCRATCH(k) (staging + (uint64_t)(STAGING_RUN + (k)) * EXT4_BLOCK)
static struct ext4_buf** sort_space;     /* commit ordering */
static struct ext4_buf** ckpt_list;      /* committed blocks whose home location is stale */
static unsigned ckpt_count, ckpt_live;
static uint64_t last_commit_ns;

static unsigned bucket(uint64_t block) { return (unsigned)((block * 0x9E3779B97F4A7C15ULL) >> 40) & (hash_size - 1); }
static void lru_unlink(struct ext4_buf* b) {
    if (!b->lru_prev) return;
    b->lru_prev->lru_next = b->lru_next; b->lru_next->lru_prev = b->lru_prev; b->lru_prev = b->lru_next = NULL;
}
static void lru_front(struct ext4_buf* b) {
    lru_unlink(b);
    b->lru_next = lru.lru_next; b->lru_prev = &lru; lru.lru_next->lru_prev = b; lru.lru_next = b;
}
static void unhash(struct ext4_buf* b) {
    struct ext4_buf** link = &hash_heads[bucket(b->block)];
    while (*link && *link != b) link = &(*link)->hash_next;
    if (*link) *link = b->hash_next;
    b->hash_next = NULL;
}
static struct ext4_buf* find(uint64_t block) {
    for (struct ext4_buf* b = hash_heads[bucket(block)]; b; b = b->hash_next) if (b->block == block) return b;
    return NULL;
}
int ext4_cache_init(uint64_t bytes) {
    unsigned count = (unsigned)(bytes / (EXT4_BLOCK + sizeof(struct ext4_buf) + 2 * sizeof(void*)));
    if (count < 256) return -E_NOMEM;
    hash_size = 1; while (hash_size < count) hash_size <<= 1;
    uint64_t need = (uint64_t)count * EXT4_BLOCK + (uint64_t)count * sizeof(struct ext4_buf) +
                    (uint64_t)hash_size * sizeof(void*) + 2ULL * count * sizeof(void*) + STAGING_BLOCKS * EXT4_BLOCK;
    uint64_t base = arena_alloc(need);
    if (!base) return -E_NOMEM;
    ext4.mount_pages += (need + 4095) / 4096;
    uint8_t* p = (uint8_t*)base;
    staging = p; p += STAGING_BLOCKS * EXT4_BLOCK;
    uint8_t* data = p; p += (uint64_t)count * EXT4_BLOCK;
    buffers = (struct ext4_buf*)p; p += (uint64_t)count * sizeof(struct ext4_buf);
    hash_heads = (struct ext4_buf**)p; p += (uint64_t)hash_size * sizeof(void*);
    sort_space = (struct ext4_buf**)p; p += (uint64_t)count * sizeof(void*);
    ckpt_list = (struct ext4_buf**)p;
    lru.lru_next = lru.lru_prev = &lru;
    for (unsigned i = 0; i < count; i++) {
        buffers[i].data = data + (uint64_t)i * EXT4_BLOCK; buffers[i].block = ~0ULL;
        lru_front(&buffers[i]);
    }
    buffer_count = count;
    txn_limit = count / 2;
    return 0;
}
unsigned ext4_cache_free_buffers(void) { return buffer_count - txn_count - ckpt_live; }
void ext4_cache_drop(void) {
    for (unsigned i = 0; i < buffer_count; i++) if (!buffers[i].txn && !buffers[i].pins && !buffers[i].ckpt) buffers[i].valid = 0;
}
static struct ext4_buf* victim(void) {
    for (struct ext4_buf* b = lru.lru_prev; b != &lru; b = b->lru_prev) if (!b->pins && !b->txn && !b->ckpt) return b;
    return NULL;
}
/* A committed block is about to be used again (and maybe modified): write its committed contents home
 * first. Always safe -- the transaction holding it is durable -- and keeps the buffer free to change. */
static int write_home(struct ext4_buf* b) {
    int error = ext4_dev_write(b->block, 1, b->data);
    if (error) { ext4_fail("checkpoint write failed"); return error; }
    b->ckpt = 0; ckpt_list[b->ckpt_slot] = NULL; ckpt_live--;
    return 0;
}
static struct ext4_buf* get(uint64_t block, bool read) {
    if (block >= ext4.blocks) { ext4_log("[EXT4] metadata block out of range ", "", block); ext4_fail("block number out of range"); return NULL; }
    struct ext4_buf* b = find(block);
    if (b) {
        if (b->ckpt && write_home(b)) return NULL;
        if (!read) { memset(b->data, 0, EXT4_BLOCK); b->valid = 1; }
        if (!b->valid) {
            if (ext4_dev_read(block, 1, b->data) < 0) return NULL;
            b->valid = 1;
        }
        if (!b->txn) lru_front(b);
        b->pins++; return b;
    }
    b = victim();
    if (!b) { ext4_fail("metadata cache exhausted"); return NULL; }
    if (b->block != ~0ULL) unhash(b);
    b->block = block; b->valid = 0;
    b->hash_next = hash_heads[bucket(block)]; hash_heads[bucket(block)] = b;
    if (read) {
        if (ext4_dev_read(block, 1, b->data) < 0) { unhash(b); b->block = ~0ULL; return NULL; }
    } else memset(b->data, 0, EXT4_BLOCK);
    b->valid = 1; b->pins = 1; lru_front(b);
    return b;
}
struct ext4_buf* ext4_bread(uint64_t block) { return get(block, true); }
struct ext4_buf* ext4_bnew(uint64_t block) { return get(block, false); }
void ext4_brelse(struct ext4_buf* b) { if (b && b->pins) b->pins--; }
void ext4_bdirty(struct ext4_buf* b) {
    if (b->txn) return;
    if (!txn_depth && !committing) panic("ext4 metadata modified outside a transaction");
    if (b->ckpt) panic("ext4 committed block modified without being looked up again");
    if (!txn_count) txn_first_ns = ext4_now_ns();
    lru_unlink(b); b->txn = 1; b->txn_next = txn_head; txn_head = b; txn_count++;
}
void ext4_bforget_range(uint64_t block, uint64_t count) {
    if (count > buffer_count * 4ULL) {
        for (unsigned i = 0; i < buffer_count; i++) {
            struct ext4_buf* b = &buffers[i];
            if (b->block >= block && b->block - block < count && !b->txn && !b->pins && !b->ckpt) b->valid = 0;
        }
        return;
    }
    for (uint64_t i = 0; i < count; i++) {
        struct ext4_buf* b = find(block + i);
        if (b && !b->txn && !b->pins && !b->ckpt) b->valid = 0;
    }
}

/* ------------------------------------------------------------------ journal */
unsigned ext4_test_poweroff_after_commits;
#define JBD2_MAGIC 0xC03B3998u
#define JBD2_DESCRIPTOR 1
#define JBD2_COMMIT 2
#define JBD2_SUPERBLOCK_V2 4
#define JBD2_REVOKE 5
#define JBD2_FLAG_ESCAPE 1
#define JBD2_FLAG_SAME_UUID 2
#define JBD2_FLAG_LAST_TAG 8
#define JBD2_FEATURE_REVOKE 0x1
#define JBD2_FEATURE_64BIT 0x2
#define JBD2_FEATURE_ASYNC_COMMIT 0x4
#define JBD2_FEATURE_CSUM_V2 0x8
#define JBD2_FEATURE_CSUM_V3 0x10
#define JBD2_INCOMPAT_KNOWN (JBD2_FEATURE_REVOKE | JBD2_FEATURE_64BIT | JBD2_FEATURE_ASYNC_COMMIT | JBD2_FEATURE_CSUM_V2 | JBD2_FEATURE_CSUM_V3)
#define JMAX_EXTENTS 32
static struct { bool present, csum3, csum2, wide, live; uint32_t maxlen, first, sequence, seed, incompat, head; unsigned extent_count;
                struct ext4_jextent extents[JMAX_EXTENTS]; uint8_t uuid[16]; } jnl;
bool ext4_journal_present(void) { return jnl.present; }
static uint64_t jmap(uint32_t lblk) {
    for (unsigned i = 0; i < jnl.extent_count; i++)
        if (lblk >= jnl.extents[i].lblk && lblk - jnl.extents[i].lblk < jnl.extents[i].len) return jnl.extents[i].pblk + lblk - jnl.extents[i].lblk;
    return 0;
}
static uint32_t jnext(uint32_t j) { return j + 1 >= jnl.maxlen ? jnl.first : j + 1; }
static int jread(uint32_t j, void* out) { uint64_t p = jmap(j); return p ? ext4_dev_read(p, 1, out) : -E_IO; }
static int jwrite(uint32_t j, const void* data) { uint64_t p = jmap(j); return p ? ext4_dev_write(p, 1, data) : -E_IO; }
static uint32_t jsb_csum(const uint8_t* sb) {
    uint8_t copy[1024]; memcpy(copy, sb, 1024); memset(copy + 0xFC, 0, 4);
    return ext4_crc32c(0xFFFFFFFFu, copy, 1024);
}
static int jsb_write(uint32_t sequence, uint32_t start) {
    uint8_t* sb = SCRATCH(0);
    int error = jread(0, sb); if (error) return error;
    putbe32(sb + 0x18, sequence); putbe32(sb + 0x1C, start);
    if (jnl.csum2 || jnl.csum3) putbe32(sb + 0xFC, jsb_csum(sb));
    return jwrite(0, sb);
}
static uint32_t tag_bytes(void) {
    if (jnl.csum3) return 16;
    return (jnl.wide ? 12 : 8) + (jnl.csum2 ? 2 : 0);
}

/* Revocation table used only during recovery (lives in the staging area's tail). */
struct revoke { uint64_t block; uint32_t sequence; };
static struct revoke* revokes; static unsigned revoke_count, revoke_cap;
static void revoke_add(uint64_t block, uint32_t sequence) {
    for (unsigned i = 0; i < revoke_count; i++) if (revokes[i].block == block) {
        if ((int32_t)(sequence - revokes[i].sequence) > 0) revokes[i].sequence = sequence;
        return;
    }
    if (revoke_count < revoke_cap) revokes[revoke_count++] = (struct revoke){block, sequence};
    else ext4_fail("journal revoke table overflow");
}
static bool revoked(uint64_t block, uint32_t sequence) {
    for (unsigned i = 0; i < revoke_count; i++) if (revokes[i].block == block) return (int32_t)(revokes[i].sequence - sequence) >= 0;
    return false;
}

/* One recovery pass: scan (find end), revoke (collect), replay (write home blocks). */
static int recovery_pass(int pass, uint32_t start, uint32_t sequence, uint32_t* end_sequence, unsigned* replayed) {
    uint8_t* blk = SCRATCH(0); uint8_t* data = SCRATCH(1);
    uint32_t j = start, seq = sequence, steps = 0, tb = tag_bytes();
    while (steps++ < jnl.maxlen) {
        if (pass && seq == *end_sequence) break;
        if (jread(j, blk) < 0) return -E_IO;
        if (be32(blk) != JBD2_MAGIC || be32(blk + 8) != seq) break;
        uint32_t type = be32(blk + 4);
        if (type == JBD2_DESCRIPTOR) {
            if ((jnl.csum2 || jnl.csum3) && pass == 0) {
                uint32_t stored = be32(blk + EXT4_BLOCK - 4); putbe32(blk + EXT4_BLOCK - 4, 0);
                if (ext4_crc32c(jnl.seed, blk, EXT4_BLOCK) != stored) break;
                putbe32(blk + EXT4_BLOCK - 4, stored);
            }
            uint32_t limit = EXT4_BLOCK - ((jnl.csum2 || jnl.csum3) ? 4 : 0), off = 12;
            uint32_t data_j = j;
            for (;;) {
                if (off + tb > limit) break;
                uint8_t* t = blk + off;
                uint64_t home; uint32_t flags, tsum = 0;
                if (jnl.csum3) { home = be32(t) | (uint64_t)be32(t + 8) << 32; flags = be32(t + 4); tsum = be32(t + 12); }
                else { home = be32(t) | (jnl.wide ? (uint64_t)be32(t + 8) << 32 : 0); flags = (uint32_t)(t[6] << 8 | t[7]); }
                off += tb; if (!(flags & JBD2_FLAG_SAME_UUID)) off += 16;
                data_j = jnext(data_j); steps++;
                if (pass == 2 && !revoked(home, seq)) {
                    if (jread(data_j, data) < 0) return -E_IO;
                    if (jnl.csum3) {
                        uint8_t sb[4]; putbe32(sb, seq);
                        if (ext4_crc32c(ext4_crc32c(jnl.seed, sb, 4), data, EXT4_BLOCK) != tsum) {
                            ext4_log("[EXT4] journal block checksum mismatch, home=", "", home); ext4.errors = true;
                            goto next_tag;
                        }
                    }
                    if (flags & JBD2_FLAG_ESCAPE) putbe32(data, JBD2_MAGIC);
                    if (home >= ext4.blocks || ext4_dev_write(home, 1, data) < 0) return -E_IO;
                    (*replayed)++;
                }
            next_tag:
                if (flags & JBD2_FLAG_LAST_TAG) break;
            }
            j = jnext(data_j);
            continue;
        }
        if (type == JBD2_COMMIT) {
            if (pass == 0 && (jnl.csum2 || jnl.csum3)) {
                uint32_t stored = be32(blk + 16); putbe32(blk + 16, 0);
                if (ext4_crc32c(jnl.seed, blk, EXT4_BLOCK) != stored) break;
            }
            seq++; j = jnext(j);
            if (pass == 0) *end_sequence = seq;
            continue;
        }
        if (type == JBD2_REVOKE) {
            if (pass == 1) {
                uint32_t used = be32(blk + 12), width = jnl.wide ? 8 : 4;
                if (used > EXT4_BLOCK) used = EXT4_BLOCK;
                for (uint32_t o = 16; o + width <= used; o += width)
                    revoke_add(width == 8 ? (uint64_t)be32(blk + o) << 32 | be32(blk + o + 4) : be32(blk + o), seq);
            }
            j = jnext(j); continue;
        }
        break;
    }
    return 0;
}

int ext4_journal_load_extents(const struct ext4_jextent* extents, unsigned count, uint64_t size_blocks) {
    memset(&jnl, 0, sizeof(jnl));
    if (count > JMAX_EXTENTS || !count) { ext4_log("[EXT4] journal too fragmented", "", count); return -E_FSCORRUPT; }
    memcpy(jnl.extents, extents, count * sizeof(*extents)); jnl.extent_count = count;
    uint8_t* sb = SCRATCH(0);
    if (jread(0, sb) < 0) return -E_IO;
    if (be32(sb) != JBD2_MAGIC || (be32(sb + 4) != JBD2_SUPERBLOCK_V2 && be32(sb + 4) != 3)) return -E_FSCORRUPT;
    if (be32(sb + 12) != EXT4_BLOCK) return -E_FSCORRUPT;
    jnl.maxlen = be32(sb + 0x10); jnl.first = be32(sb + 0x14); jnl.sequence = be32(sb + 0x18);
    uint32_t start = be32(sb + 0x1C);
    if (be32(sb + 4) == JBD2_SUPERBLOCK_V2) jnl.incompat = be32(sb + 0x28);
    if (jnl.incompat & ~JBD2_INCOMPAT_KNOWN) { ext4_log("[EXT4] unknown journal features ", "", jnl.incompat); return -E_FSCORRUPT; }
    jnl.csum3 = jnl.incompat & JBD2_FEATURE_CSUM_V3; jnl.csum2 = jnl.incompat & JBD2_FEATURE_CSUM_V2;
    jnl.wide = jnl.incompat & JBD2_FEATURE_64BIT;
    if (!jnl.maxlen || jnl.maxlen > size_blocks || !jnl.first || jnl.first >= jnl.maxlen) return -E_FSCORRUPT;
    memcpy(jnl.uuid, sb + 0x30, 16);
    if ((jnl.csum2 || jnl.csum3) && be32(sb + 0xFC) != jsb_csum(sb)) { ext4_log("[EXT4] journal superblock checksum mismatch", "", 0); return -E_FSCORRUPT; }
    jnl.seed = ext4_crc32c(0xFFFFFFFFu, jnl.uuid, 16);
    jnl.present = true; jnl.head = jnl.first; jnl.live = false;
    { /* a transaction must fit the log: blocks + descriptors + commit */
        uint32_t room = EXT4_BLOCK - 12 - ((jnl.csum2 || jnl.csum3) ? 4 : 0), per = (room - 16) / tag_bytes();
        uint32_t cap = jnl.maxlen - jnl.first;
        uint32_t limit = cap > 8 ? (uint32_t)((uint64_t)(cap - 4) * per / (per + 1)) : 1;
        if (limit < txn_limit) txn_limit = limit;
    }
    if (!start) return 0;
    if (ext4.readonly) { ext4_log("[EXT4] journal needs recovery but the device is read-only", "", 0); return -E_ROFS; }
    /* recovery */
    revokes = (struct revoke*)staging;
    revoke_cap = STAGING_RUN * EXT4_BLOCK / sizeof(struct revoke); revoke_count = 0;
    uint32_t end = jnl.sequence; unsigned replayed = 0;
    int error = recovery_pass(0, start, jnl.sequence, &end, &replayed);
    if (!error) error = recovery_pass(1, start, jnl.sequence, &end, &replayed);
    if (!error) error = recovery_pass(2, start, jnl.sequence, &end, &replayed);
    if (!error) error = ext4_dev_flush();
    if (error) return error;
    jnl.sequence = end;
    if ((error = jsb_write(jnl.sequence, 0)) || (error = ext4_dev_flush())) return error;
    jnl.head = jnl.first; jnl.live = false;
    ext4_log("[EXT4] JOURNAL REPLAY blocks=", "", replayed);
    return 0;
}

int ext4_txn_begin(unsigned credits) {
    if (ext4.readonly) return -E_ROFS;
    if (txn_depth++) return 0;
    if (!ext4.dirty_on_disk) {
        /* First change of this mount: mark needs_recovery before anything reaches the journal. */
        struct ext4_buf* b = ext4_bread(0);
        if (!b) { txn_depth--; return -E_IO; }
        put32(b->data + 1024 + SB_INCOMPAT, le32(b->data + 1024 + SB_INCOMPAT) | (jnl.present ? INCOMPAT_RECOVER : 0));
        /* like Linux: a journaled volume stays VALID_FS and records the dirty state as needs_recovery */
        if (!jnl.present) put16(b->data + 1024 + SB_STATE, le16(b->data + 1024 + SB_STATE) & ~STATE_VALID);
        put32(b->data + 1024 + SB_MTIME, (uint32_t)(ext4_now_ns() / 1000000000ULL));
        put16(b->data + 1024 + SB_MNT_COUNT, le16(b->data + 1024 + SB_MNT_COUNT) + 1);
        if (ext4.csum) put32(b->data + 1024 + SB_CHECKSUM, ext4_crc32c(0xFFFFFFFFu, b->data + 1024, SB_CHECKSUM));
        int error = b->txn ? 0 : ext4_dev_write(0, 1, b->data);
        ext4_brelse(b);
        if (!error) error = ext4_dev_flush();
        if (error) { txn_depth--; ext4_fail("cannot mark the volume in use"); return error; }
        ext4.dirty_on_disk = true;
    }
    if (txn_count + credits > txn_limit || ext4_cache_free_buffers() < credits + 64) {
        int error = ext4_txn_commit();
        if (!error && ext4_cache_free_buffers() < credits + 64 + buffer_count / 4) error = ext4_checkpoint();
        if (error) { txn_depth--; return error; }
    }
    return 0;
}
void ext4_txn_end(void) { if (txn_depth) txn_depth--; }
bool ext4_txn_active(void) { return txn_depth != 0; }
unsigned ext4_txn_blocks(void) { return txn_count; }
uint64_t ext4_txn_age_ns(void) { return txn_count ? ext4_now_ns() - txn_first_ns : 0; }

static int write_staged(uint64_t* run_start, unsigned* run_len) {
    if (!*run_len) return 0;
    int error = ext4_dev_write(*run_start, *run_len, staging);
    *run_len = 0; return error;
}
/* Append one block to the staged contiguous run at physical `phys`. */
static int stage(uint64_t phys, const uint8_t* data, uint64_t* run_start, unsigned* run_len) {
    if (*run_len && (*run_start + *run_len != phys || *run_len == STAGING_RUN)) {
        int error = write_staged(run_start, run_len); if (error) return error;
    }
    if (!*run_len) *run_start = phys;
    memcpy(staging + (uint64_t)*run_len * EXT4_BLOCK, data, EXT4_BLOCK);
    (*run_len)++;
    return 0;
}
static void sort_by_block(struct ext4_buf** a, unsigned n) {
    for (unsigned gap = n / 2; gap; gap /= 2)          /* shell sort: no allocation, n <= cache size */
        for (unsigned i = gap; i < n; i++) {
            struct ext4_buf* x = a[i]; unsigned j = i;
            while (j >= gap && a[j - gap]->block > x->block) { a[j] = a[j - gap]; j -= gap; }
            a[j] = x;
        }
}
static int commit_body(void) {
    int error = ext4_prepare_commit(); if (error) return error;
    if (!txn_count) return 0;
    unsigned n = 0;
    for (struct ext4_buf* b = txn_head; b; b = b->txn_next) sort_space[n++] = b;
    uint64_t run_start = 0; unsigned run_len = 0;
    if (jnl.present) {
        uint32_t tb = tag_bytes(), per = 0;
        { uint32_t room = EXT4_BLOCK - 12 - ((jnl.csum2 || jnl.csum3) ? 4 : 0); per = (room - 16) / tb; }
        uint32_t needed = n + (n + per - 1) / per + 1;
        if (needed + 1 > jnl.maxlen - jnl.first) { ext4_fail("transaction larger than the journal"); return -E_NOSPC; }
        if (jnl.live && jnl.head + needed >= jnl.maxlen && (error = ext4_checkpoint())) return error;
        uint32_t seq = jnl.sequence;
        if (!jnl.live && (error = jsb_write(seq, jnl.first))) return error; /* the log starts with this transaction */
        uint32_t j = jnl.head;
        uint8_t seqb[4]; putbe32(seqb, seq);
        for (unsigned at = 0; at < n; ) {
            unsigned count = n - at < per ? n - at : per;
            uint8_t* descriptor = SCRATCH(0);
            memset(descriptor, 0, EXT4_BLOCK);
            putbe32(descriptor, JBD2_MAGIC); putbe32(descriptor + 4, JBD2_DESCRIPTOR); putbe32(descriptor + 8, seq);
            uint32_t off = 12;
            uint32_t dj = j;
            /* descriptor first, then the data copies */
            for (unsigned k = 0; k < count; k++) {
                struct ext4_buf* b = sort_space[at + k];
                uint8_t* t = descriptor + off;
                uint32_t flags = (k ? JBD2_FLAG_SAME_UUID : 0) | (k + 1 == count ? JBD2_FLAG_LAST_TAG : 0);
                bool escape = be32(b->data) == JBD2_MAGIC;
                if (escape) flags |= JBD2_FLAG_ESCAPE;
                if (jnl.csum3) {
                    putbe32(t, (uint32_t)b->block); putbe32(t + 4, flags); putbe32(t + 8, (uint32_t)(b->block >> 32));
                    uint32_t c = ext4_crc32c(jnl.seed, seqb, 4);
                    if (escape) { uint8_t zero[4] = {0}; c = ext4_crc32c(c, zero, 4); c = ext4_crc32c(c, b->data + 4, EXT4_BLOCK - 4); }
                    else c = ext4_crc32c(c, b->data, EXT4_BLOCK);
                    putbe32(t + 12, c);
                } else {
                    putbe32(t, (uint32_t)b->block); t[6] = (uint8_t)(flags >> 8); t[7] = (uint8_t)flags;
                    if (jnl.wide) putbe32(t + 8, (uint32_t)(b->block >> 32));
                }
                off += tb;
                if (!k) { memcpy(descriptor + off, jnl.uuid, 16); off += 16; }
            }
            if (jnl.csum2 || jnl.csum3) putbe32(descriptor + EXT4_BLOCK - 4, ext4_crc32c(jnl.seed, descriptor, EXT4_BLOCK));
            if ((error = stage(jmap(dj), descriptor, &run_start, &run_len))) return error;
            dj = jnext(dj);
            for (unsigned k = 0; k < count; k++) {
                struct ext4_buf* b = sort_space[at + k];
                if (be32(b->data) == JBD2_MAGIC) {
                    uint8_t* copy = SCRATCH(1); memcpy(copy, b->data, EXT4_BLOCK); memset(copy, 0, 4);
                    error = stage(jmap(dj), copy, &run_start, &run_len);
                } else error = stage(jmap(dj), b->data, &run_start, &run_len);
                if (error) return error;
                dj = jnext(dj);
            }
            j = dj; at += count;
        }
        if ((error = write_staged(&run_start, &run_len)) || (error = ext4_dev_flush())) return error;
        uint8_t* commit = SCRATCH(2); memset(commit, 0, EXT4_BLOCK);
        putbe32(commit, JBD2_MAGIC); putbe32(commit + 4, JBD2_COMMIT); putbe32(commit + 8, seq);
        uint64_t now = ext4_now_ns();
        putbe32(commit + 48, (uint32_t)((now / 1000000000ULL) >> 32)); putbe32(commit + 52, (uint32_t)(now / 1000000000ULL));
        putbe32(commit + 56, (uint32_t)(now % 1000000000ULL));
        if (jnl.csum2 || jnl.csum3) putbe32(commit + 16, ext4_crc32c(jnl.seed, commit, EXT4_BLOCK));
        if ((error = jwrite(j, commit)) || (error = ext4_dev_flush())) return error;
        jnl.sequence = seq + 1; jnl.head = j + 1; jnl.live = true;
        if (ext4_test_poweroff_after_commits && !--ext4_test_poweroff_after_commits) {
            /* crash-test hook: the transaction is durable but not checkpointed; recovery must replay it */
            ext4_log("[EXT4] TEST POWER-OFF after journal commit, sequence ", "", seq);
            __asm__ volatile("outl %0,%1" :: "a"(0x11u), "Nd"((uint16_t)0xF4));
            for (;;) __asm__ volatile("cli; hlt");
        }
    }
    else {
        /* no journal: write home directly (no atomicity, like ext2) */
        sort_by_block(sort_space, n);
        for (unsigned i = 0; i < n; i++) if ((error = stage(sort_space[i]->block, sort_space[i]->data, &run_start, &run_len))) return error;
        if ((error = write_staged(&run_start, &run_len)) || (error = ext4_dev_flush())) return error;
    }
    ext4.kbytes_written += (uint64_t)n * 4;
    return 0;
}
/* Write every committed-but-not-home block, flush, then empty the log (superblock write + flush). */
int ext4_checkpoint(void) {
    if (!jnl.present || (!jnl.live && !ckpt_live)) return 0;
    /* compact and sort the list in place: commit_body may be using sort_space right now */
    unsigned n = 0;
    for (unsigned i = 0; i < ckpt_count; i++) if (ckpt_list[i]) ckpt_list[n++] = ckpt_list[i];
    sort_by_block(ckpt_list, n);
    uint64_t run_start = 0; unsigned run_len = 0; int error = 0;
    for (unsigned i = 0; i < n && !error; i++) error = stage(ckpt_list[i]->block, ckpt_list[i]->data, &run_start, &run_len);
    if (!error) error = write_staged(&run_start, &run_len);
    if (!error) error = ext4_dev_flush();
    if (!error) error = jsb_write(jnl.sequence, 0);
    if (!error) error = ext4_dev_flush();
    if (error) { ext4_fail("journal checkpoint failed"); return error; }
    for (unsigned i = 0; i < n; i++) { ckpt_list[i]->ckpt = 0; lru_front(ckpt_list[i]); }
    ckpt_count = ckpt_live = 0;
    jnl.live = false; jnl.head = jnl.first;
    ext4_after_checkpoint();
    return 0;
}
bool ext4_journal_idle(uint64_t idle_ns) { return jnl.live && ext4_now_ns() - last_commit_ns >= idle_ns; }
uint64_t ext4_journal_idle_at(uint64_t idle_ns) { return jnl.live ? last_commit_ns + idle_ns : ~0ULL; }   /* tick.c: checkpoint due */
int ext4_txn_commit(void) {
    if (committing) return 0;
    if (ext4.readonly) return ext4.errors ? -E_IO : 0;
    committing = true;
    int error = 0;
    if (ext4_quarantine_full() || ckpt_count + txn_count > buffer_count) error = ext4_checkpoint();
    if (!error) error = commit_body();
    /* Leave the transaction either way; committed blocks wait for their home write (checkpoint). */
    struct ext4_buf* b = txn_head;
    while (b) {
        struct ext4_buf* next = b->txn_next; b->txn = 0; b->txn_next = NULL;
        if (!error && jnl.present) { b->ckpt = 1; b->ckpt_slot = ckpt_count; ckpt_list[ckpt_count++] = b; ckpt_live++; }
        else lru_front(b);
        b = next;
    }
    txn_head = NULL; txn_count = 0; last_commit_ns = ext4_now_ns();
    committing = false;
    if (error) { ext4_fail("journal commit failed"); return error; }
    ext4_after_commit();
    return 0;
}
