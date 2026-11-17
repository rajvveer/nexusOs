/* ext4 filesystem: superblock and block groups (64bit, flex_bg, sparse_super), inodes (256-byte,
 * nanosecond times, metadata_csum), extent trees (lookup, allocation, growth and splitting, removal),
 * directories (linear and dir_index/htree, both maintained), allocation, namespace operations
 * (create, link, unlink, rename, symlinks, device/fifo/socket nodes), file data (holes, unwritten
 * extents, files beyond 4 GiB), and the orphan list. All metadata changes run inside jbd2
 * transactions (ext4_journal.c); file data is written in place before its transaction commits. */
#include "ext4_internal.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

struct ext4_fs ext4;
#define ROOT_INO 2u

/* ------------------------------------------------------------------ small helpers */
uint64_t ext4_now_ns(void) { return native_realtime_ns(); }
void ext4_log(const char* a, const char* b, uint64_t value) {
    char number[24]; number64(value, number);
    vga_print(a); vga_print(b); vga_print(number); vga_print("\n");
}
static bool fail_logged;
void ext4_fail(const char* why) {
    if (!fail_logged) { fail_logged = true; vga_print("[EXT4] ERROR: "); vga_print(why); vga_print("; volume is now read-only\n"); }
    ext4.readonly = true; ext4.errors = true;
}
static bool is_dir_mode(uint32_t mode) { return (mode & S_IFMT_) == S_IFDIR_; }
static uint8_t mode_ftype(uint32_t mode) {
    switch (mode & S_IFMT_) {
    case S_IFREG_: return 1; case S_IFDIR_: return 2; case S_IFCHR_: return 3; case S_IFBLK_: return 4;
    case S_IFIFO_: return 5; case S_IFSOCK_: return 6; case S_IFLNK_: return 7; default: return 0;
    }
}
static uint32_t ftype_mode(uint8_t ft) {
    static const uint32_t modes[8] = {0, S_IFREG_, S_IFDIR_, S_IFCHR_, S_IFBLK_, S_IFIFO_, S_IFSOCK_, S_IFLNK_};
    return ft < 8 ? modes[ft] : 0;
}
static uint8_t mode_kind(uint32_t mode) {
    switch (mode & S_IFMT_) {
    case S_IFSOCK_: return EXT4_KIND_SOCKET; case S_IFLNK_: return EXT4_KIND_SYMLINK; case S_IFIFO_: return EXT4_KIND_FIFO;
    case S_IFCHR_: return EXT4_KIND_CHR; case S_IFBLK_: return EXT4_KIND_BLK; default: return 0;
    }
}

/* mount-time scratch area: bounce block, directory split maps, path buffers */
static uint8_t* scratch;      /* SCRATCH_BLOCKS * 4 KiB */
#define SCRATCH_BLOCKS 4
#define BOUNCE (scratch)
#define SPLIT_COPY (scratch + EXT4_BLOCK)
#define SPLIT_MAP (scratch + 2 * EXT4_BLOCK)

/* ------------------------------------------------------------------ groups */
struct group {
    uint64_t block_bitmap, inode_bitmap, inode_table;
    uint32_t free_blocks, free_inodes, used_dirs, itable_unused;
    uint16_t flags; bool dirty;
};
static struct group* groups;
static uint32_t orphan_head_cache;
static uint32_t next_generation;

static uint64_t group_first(uint32_t g) { return ext4.first_data_block + (uint64_t)g * ext4.blocks_per_group; }
static uint32_t group_blocks(uint32_t g) {
    uint64_t first = group_first(g);
    return ext4.blocks - first < ext4.blocks_per_group ? (uint32_t)(ext4.blocks - first) : ext4.blocks_per_group;
}
static bool group_has_super(uint32_t g) {
    if (!(ext4.ro_compat & RO_SPARSE_SUPER) || g <= 1) return true;
    for (uint32_t base = 3; base <= 7; base += 2) { uint32_t x = g; while (x % base == 0) x /= base; if (x == 1) return true; }
    return false;
}
static uint16_t gd_csum(uint32_t g, const uint8_t* desc) {
    uint8_t le[4]; put32(le, g);
    uint32_t c = ext4_crc32c(ext4.csum_seed, le, 4);
    c = ext4_crc32c(c, desc, 0x1E);
    uint8_t zero[2] = {0, 0}; c = ext4_crc32c(c, zero, 2);
    if (ext4.desc_size > 0x20) c = ext4_crc32c(c, desc + 0x20, ext4.desc_size - 0x20);
    return (uint16_t)c;
}
static void decode_group(uint32_t g, const uint8_t* d) {
    struct group* gr = &groups[g]; bool wide = ext4.desc_size >= 64;
    gr->block_bitmap = le32(d) | (wide ? (uint64_t)le32(d + 0x20) << 32 : 0);
    gr->inode_bitmap = le32(d + 4) | (wide ? (uint64_t)le32(d + 0x24) << 32 : 0);
    gr->inode_table = le32(d + 8) | (wide ? (uint64_t)le32(d + 0x28) << 32 : 0);
    gr->free_blocks = le16(d + 0xC) | (wide ? (uint32_t)le16(d + 0x2C) << 16 : 0);
    gr->free_inodes = le16(d + 0xE) | (wide ? (uint32_t)le16(d + 0x2E) << 16 : 0);
    gr->used_dirs = le16(d + 0x10) | (wide ? (uint32_t)le16(d + 0x30) << 16 : 0);
    gr->itable_unused = le16(d + 0x1C) | (wide ? (uint32_t)le16(d + 0x32) << 16 : 0);
    gr->flags = le16(d + 0x12); gr->dirty = false;
}
static int bitmap_csum_store(uint32_t g, uint8_t* d) {
    struct group* gr = &groups[g];
    if (!ext4.csum) return 0;
    struct ext4_buf* b = ext4_bread(gr->block_bitmap); if (!b) return -E_IO;
    uint32_t c = ext4_crc32c(ext4.csum_seed, b->data, ext4.blocks_per_group / 8); ext4_brelse(b);
    put16(d + 0x18, c); if (ext4.desc_size >= 64) put16(d + 0x38, c >> 16);
    b = ext4_bread(gr->inode_bitmap); if (!b) return -E_IO;
    c = ext4_crc32c(ext4.csum_seed, b->data, ext4.inodes_per_group / 8); ext4_brelse(b);
    put16(d + 0x1A, c); if (ext4.desc_size >= 64) put16(d + 0x3A, c >> 16);
    return 0;
}
static int encode_group(uint32_t g) {
    uint64_t off = (uint64_t)g * ext4.desc_size;
    struct ext4_buf* b = ext4_bread(ext4.first_data_block + 1 + off / EXT4_BLOCK); if (!b) return -E_IO;
    uint8_t* d = b->data + off % EXT4_BLOCK; struct group* gr = &groups[g]; bool wide = ext4.desc_size >= 64;
    put16(d + 0xC, gr->free_blocks); put16(d + 0xE, gr->free_inodes); put16(d + 0x10, gr->used_dirs);
    put16(d + 0x12, gr->flags); put16(d + 0x1C, gr->itable_unused);
    if (wide) { put16(d + 0x2C, gr->free_blocks >> 16); put16(d + 0x2E, gr->free_inodes >> 16); put16(d + 0x30, gr->used_dirs >> 16); put16(d + 0x32, gr->itable_unused >> 16); }
    int error = bitmap_csum_store(g, d);
    if (!error && ext4.csum) put16(d + 0x1E, gd_csum(g, d));
    if (!error) ext4_bdirty(b);
    ext4_brelse(b); gr->dirty = false;
    return error;
}

/* ------------------------------------------------------------------ deferred frees */
struct pending_free { uint64_t start; uint32_t count; };
static struct pending_free* pfrees;
static unsigned pfree_count;
#define PFREE_CAP 16384u
#define PFREE_RESERVE 512u
/* Frees applied by transactions that are still in the log: not allocatable until the checkpoint. */
static struct pending_free* quarantine;
static unsigned quarantine_count, quarantine_committed; /* [0, committed) belong to committed transactions */
#define QUARANTINE_CAP (2 * PFREE_CAP)
bool ext4_quarantine_full(void) { return quarantine_count + PFREE_CAP > QUARANTINE_CAP; }
/* Only frees of transactions that are now checkpointed leave quarantine; a checkpoint taken while a
 * transaction is being committed keeps that transaction's frees (they are not in the emptied log). */
void ext4_after_checkpoint(void) {
    unsigned keep = quarantine_count - quarantine_committed;
    ext4_move(quarantine, quarantine + quarantine_committed, (uint64_t)keep * sizeof(*quarantine));
    quarantine_count = keep; quarantine_committed = 0;
}
static void quarantine_add(uint64_t start, uint32_t count) {
    struct pending_free* last = quarantine_count ? &quarantine[quarantine_count - 1] : NULL;
    if (last && last->start + last->count == start && last->count + (uint64_t)count < 0xFFFFFFFFu) { last->count += count; return; }
    if (quarantine_count == QUARANTINE_CAP) { ext4_fail("free-block quarantine overflow"); return; }
    quarantine[quarantine_count++] = (struct pending_free){start, count};
}
/* Allowed length of [start, start+len) before a quarantined block; 0 if start itself is quarantined
 * (then *resume is the first block after that range). */
static uint32_t quarantine_clip(uint64_t start, uint32_t len, uint64_t* resume) {
    for (unsigned i = 0; i < quarantine_count; i++) {
        uint64_t qs = quarantine[i].start, qe = qs + quarantine[i].count;
        if (qe <= start || qs >= start + len) continue;
        if (qs <= start) { *resume = qe; return 0; }
        len = (uint32_t)(qs - start);
    }
    return len;
}
static void free_blocks(uint64_t start, uint32_t count) {
    if (!count) return;
    if (start < ext4.first_data_block || start >= ext4.blocks || count > ext4.blocks - start) { ext4_fail("freeing an invalid block range"); return; }
    ext4.pending_free += count;
    if (pfree_count && pfrees[pfree_count - 1].start + pfrees[pfree_count - 1].count == start &&
        pfrees[pfree_count - 1].count + (uint64_t)count < 0xFFFFFFFFu) { pfrees[pfree_count - 1].count += count; return; }
    if (pfree_count == PFREE_CAP) { ext4_fail("deferred free list overflow"); return; }
    pfrees[pfree_count++] = (struct pending_free){start, count};
}
static bool pfree_room(void) { return pfree_count + PFREE_RESERVE < PFREE_CAP; }

/* ------------------------------------------------------------------ bitmaps */
static void synthesize_block_bitmap(uint32_t g, uint8_t* bm) {
    memset(bm, 0, EXT4_BLOCK);
    uint64_t first = group_first(g); uint32_t n = group_blocks(g);
    if (group_has_super(g)) for (uint32_t i = 0; i < 1 + ext4.gdt_blocks && i < n; i++) bm[i >> 3] |= (uint8_t)(1 << (i & 7));
    uint32_t itb = (ext4.inodes_per_group * ext4.inode_size + EXT4_BLOCK - 1) / EXT4_BLOCK;
    for (uint32_t k = 0; k < ext4.groups; k++) {
        uint64_t marks[3] = {groups[k].block_bitmap, groups[k].inode_bitmap, groups[k].inode_table};
        for (int m = 0; m < 3; m++) {
            uint32_t len = m == 2 ? itb : 1;
            for (uint32_t i = 0; i < len; i++) {
                uint64_t blk = marks[m] + i;
                if (blk >= first && blk - first < n) { uint32_t bit = (uint32_t)(blk - first); bm[bit >> 3] |= (uint8_t)(1 << (bit & 7)); }
            }
        }
    }
    for (uint32_t i = n; i < EXT4_BLOCK * 8; i++) bm[i >> 3] |= (uint8_t)(1 << (i & 7));
}
static struct ext4_buf* block_bitmap(uint32_t g) {
    struct group* gr = &groups[g];
    if (gr->flags & BG_BLOCK_UNINIT) {
        struct ext4_buf* b = ext4_bnew(gr->block_bitmap); if (!b) return NULL;
        synthesize_block_bitmap(g, b->data);
        return b;
    }
    return ext4_bread(gr->block_bitmap);
}
static struct ext4_buf* inode_bitmap(uint32_t g) {
    struct group* gr = &groups[g];
    if (gr->flags & BG_INODE_UNINIT) {
        struct ext4_buf* b = ext4_bnew(gr->inode_bitmap); if (!b) return NULL;
        for (uint32_t i = ext4.inodes_per_group; i < EXT4_BLOCK * 8; i++) b->data[i >> 3] |= (uint8_t)(1 << (i & 7));
        return b;
    }
    return ext4_bread(gr->inode_bitmap);
}
static bool bit_test(const uint8_t* bm, uint32_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static void bit_set(uint8_t* bm, uint32_t i) { bm[i >> 3] |= (uint8_t)(1 << (i & 7)); }
static void bit_clear(uint8_t* bm, uint32_t i) { bm[i >> 3] &= (uint8_t)~(1 << (i & 7)); }
static int64_t find_zero(const uint8_t* bm, uint32_t from, uint32_t limit) {
    uint32_t i = from;
    while (i < limit) {
        if (!(i & 63) && i + 64 <= limit) {
            uint64_t word; memcpy(&word, bm + (i >> 3), 8);
            if (word == ~0ULL) { i += 64; continue; }
        }
        if (!bit_test(bm, i)) return i;
        i++;
    }
    return -1;
}

/* Allocate up to `want` contiguous blocks near `goal`. */
static int alloc_blocks(uint64_t goal, uint32_t want, uint64_t* start, uint32_t* got) {
    if (!want) return -E_INVAL;
    if (goal < ext4.first_data_block || goal >= ext4.blocks) goal = ext4.first_data_block;
    uint32_t g0 = (uint32_t)((goal - ext4.first_data_block) / ext4.blocks_per_group);
    uint32_t prefer = want < 16 ? want : 16;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t k = 0; k <= ext4.groups; k++) {
            uint32_t g = (g0 + k) % ext4.groups;
            if (k == ext4.groups && g != g0) break;
            struct group* gr = &groups[g];
            if (!gr->free_blocks) continue;
            struct ext4_buf* b = block_bitmap(g); if (!b) return -E_IO;
            uint32_t n = group_blocks(g), from = (k == 0 && goal >= group_first(g)) ? (uint32_t)(goal - group_first(g)) : 0;
            if (k == ext4.groups) { n = from; from = 0; } /* wrapped: the part of the goal group before the goal */
            int64_t i = find_zero(b->data, from, n);
            while (i >= 0) {
                uint32_t j = (uint32_t)i;
                while (j < n && j - (uint32_t)i < want && !bit_test(b->data, j)) j++;
                uint64_t resume = 0;
                uint32_t len = quarantine_clip(group_first(g) + (uint64_t)i, j - (uint32_t)i, &resume);
                if (!len) { /* freed by a transaction still in the log */
                    uint64_t next = resume - group_first(g);
                    i = next < n ? find_zero(b->data, (uint32_t)next, n) : -1;
                    continue;
                }
                j = (uint32_t)i + len;
                if (len >= prefer || pass) {
                    for (uint32_t x = (uint32_t)i; x < j; x++) bit_set(b->data, x);
                    if (gr->flags & BG_BLOCK_UNINIT) gr->flags &= ~BG_BLOCK_UNINIT;
                    ext4_bdirty(b); ext4_brelse(b);
                    gr->free_blocks -= len; ext4.free_blocks -= len; gr->dirty = true;
                    *start = group_first(g) + (uint64_t)i; *got = len;
                    return 0;
                }
                i = find_zero(b->data, j, n);
            }
            ext4_brelse(b);
        }
    return -E_NOSPC;
}
static int apply_pending_frees(void) {
    for (unsigned k = 0; k < pfree_count; k++) {
        uint64_t start = pfrees[k].start; uint32_t count = pfrees[k].count;
        while (count) {
            uint32_t g = (uint32_t)((start - ext4.first_data_block) / ext4.blocks_per_group);
            uint32_t i = (uint32_t)(start - group_first(g)), n = group_blocks(g) - i;
            if (n > count) n = count;
            struct ext4_buf* b = block_bitmap(g); if (!b) return -E_IO;
            for (uint32_t x = i; x < i + n; x++) {
                if (!bit_test(b->data, x)) { ext4_brelse(b); ext4_log("[EXT4] double free of block ", "", start + x - i); ext4_fail("double free"); return -E_IO; }
                bit_clear(b->data, x);
            }
            ext4_bdirty(b); ext4_brelse(b);
            groups[g].free_blocks += n; groups[g].dirty = true; ext4.free_blocks += n;
            if (ext4_journal_present()) quarantine_add(start, n);
            start += n; count -= n;
        }
    }
    pfree_count = 0; ext4.pending_free = 0;
    return 0;
}

/* ------------------------------------------------------------------ inodes */
struct inode { uint32_t ino; struct ext4_buf* buf; uint8_t* raw; };
static uint32_t inode_seed(uint32_t ino, uint32_t generation) {
    uint8_t le[4]; put32(le, ino);
    uint32_t c = ext4_crc32c(ext4.csum_seed, le, 4);
    put32(le, generation); return ext4_crc32c(c, le, 4);
}
static bool has_extra(const uint8_t* raw, unsigned field_end) {
    return ext4.inode_size > 128 && 128u + le16(raw + I_EXTRA_ISIZE) >= field_end;
}
static uint32_t inode_csum(uint32_t ino, const uint8_t* raw) {
    uint8_t zero[2] = {0, 0};
    uint32_t c = ext4_crc32c(inode_seed(ino, le32(raw + I_GENERATION)), raw, I_CSUM_LO);
    c = ext4_crc32c(c, zero, 2);
    c = ext4_crc32c(c, raw + I_CSUM_LO + 2, 128 - I_CSUM_LO - 2);
    if (ext4.inode_size > 128) {
        c = ext4_crc32c(c, raw + 128, I_CSUM_HI - 128);
        unsigned at = I_CSUM_HI;
        if (has_extra(raw, I_CSUM_HI + 2)) { c = ext4_crc32c(c, zero, 2); at += 2; }
        c = ext4_crc32c(c, raw + at, ext4.inode_size - at);
    }
    return c;
}
static int iget(uint32_t ino, struct inode* in) {
    in->buf = NULL;
    if (!ino || ino > ext4.inodes) return -E_FSCORRUPT;
    uint32_t g = (ino - 1) / ext4.inodes_per_group, index = (ino - 1) % ext4.inodes_per_group;
    uint64_t byte = (uint64_t)index * ext4.inode_size;
    struct ext4_buf* b = ext4_bread(groups[g].inode_table + byte / EXT4_BLOCK);
    if (!b) return -E_IO;
    in->ino = ino; in->buf = b; in->raw = b->data + byte % EXT4_BLOCK;
    if (ext4.csum && !b->txn) {
        uint32_t want = inode_csum(ino, in->raw), have = le16(in->raw + I_CSUM_LO);
        if (has_extra(in->raw, I_CSUM_HI + 2)) have |= (uint32_t)le16(in->raw + I_CSUM_HI) << 16; else want &= 0xFFFF;
        if (want != have) { ext4_brelse(b); in->buf = NULL; ext4_log("[EXT4] inode checksum mismatch, inode ", "", ino); return -E_FSCORRUPT; }
    }
    return 0;
}
static void iput(struct inode* in) { if (in->buf) ext4_brelse(in->buf); in->buf = NULL; }
static void idirty(struct inode* in) {
    if (ext4.csum) {
        uint32_t c = inode_csum(in->ino, in->raw);
        put16(in->raw + I_CSUM_LO, c);
        if (has_extra(in->raw, I_CSUM_HI + 2)) put16(in->raw + I_CSUM_HI, c >> 16);
    }
    ext4_bdirty(in->buf);
}
static uint32_t i_mode(const struct inode* in) { return le16(in->raw + I_MODE); }
static uint32_t i_flags(const struct inode* in) { return le32(in->raw + I_FLAGS); }
static uint32_t i_links(const struct inode* in) { return le16(in->raw + I_LINKS); }
static uint64_t i_size(const struct inode* in) { return le32(in->raw + I_SIZE_LO) | (uint64_t)le32(in->raw + I_SIZE_HI) << 32; }
static void set_size(struct inode* in, uint64_t size) { put32(in->raw + I_SIZE_LO, (uint32_t)size); put32(in->raw + I_SIZE_HI, (uint32_t)(size >> 32)); }
static uint32_t i_uid(const struct inode* in) { return le16(in->raw + I_UID) | (uint32_t)le16(in->raw + I_UID_HI) << 16; }
static uint32_t i_gid(const struct inode* in) { return le16(in->raw + I_GID) | (uint32_t)le16(in->raw + I_GID_HI) << 16; }
static uint64_t i_blocks512(const struct inode* in) {
    uint64_t v = le32(in->raw + I_BLOCKS_LO) | (uint64_t)le16(in->raw + I_BLOCKS_HI) << 32;
    return (i_flags(in) & IFL_HUGE_FILE) ? v * 8 : v;
}
static void add_blocks512(struct inode* in, int64_t delta) {
    uint64_t v = i_blocks512(in) + (uint64_t)delta;
    if (i_flags(in) & IFL_HUGE_FILE) v /= 8;
    put32(in->raw + I_BLOCKS_LO, (uint32_t)v); put16(in->raw + I_BLOCKS_HI, (uint32_t)(v >> 32));
}
static uint64_t i_file_acl(const struct inode* in) { return le32(in->raw + I_FILE_ACL_LO) | (uint64_t)le16(in->raw + I_FILE_ACL_HI) << 32; }
static int64_t get_time(const struct inode* in, unsigned base, unsigned extra) {
    int64_t sec = (int32_t)le32(in->raw + base); uint32_t ns = 0;
    if (has_extra(in->raw, extra + 4)) { uint32_t e = le32(in->raw + extra); sec += (int64_t)(e & 3) << 32; ns = e >> 2; }
    return sec * 1000000000LL + ns;
}
static void set_time(struct inode* in, unsigned base, unsigned extra, int64_t ns_total) {
    int64_t sec = ns_total / 1000000000LL; int64_t ns = ns_total % 1000000000LL;
    if (ns < 0) { ns += 1000000000LL; sec--; }
    put32(in->raw + base, (uint32_t)sec);
    if (has_extra(in->raw, extra + 4)) {
        uint32_t epoch = (uint32_t)(((sec - (int32_t)(uint32_t)sec) >> 32) & 3);
        put32(in->raw + extra, (uint32_t)(ns << 2) | epoch);
    }
}
#define ATIME I_ATIME, I_ATIME_EXTRA
#define MTIME I_MTIME, I_MTIME_EXTRA
#define CTIME I_CTIME, I_CTIME_EXTRA
#define CRTIME I_CRTIME, I_CRTIME_EXTRA
static void touch(struct inode* in, bool modify) {
    int64_t now = (int64_t)ext4_now_ns();
    set_time(in, CTIME, now); if (modify) set_time(in, MTIME, now);
}
static void fill(fat32_file_t* f, const struct inode* in, const char* name, unsigned length, uint32_t parent) {
    memset(f, 0, sizeof(*f));
    uint32_t mode = i_mode(in);
    f->ino = in->ino; f->native_ino = in->ino; f->first = in->ino;
    f->dir_lba = in->ino == ROOT_INO ? 0 : in->ino; f->dir_off = 0;
    f->mode = mode; f->uid = i_uid(in); f->gid = i_gid(in); f->nlink = i_links(in);
    f->is_dir = is_dir_mode(mode); f->native_kind = mode_kind(mode);
    f->size = i_size(in); f->blocks = i_blocks512(in);
    f->iflags = i_flags(in); f->generation = le32(in->raw + I_GENERATION);
    f->atime = get_time(in, ATIME); f->mtime = get_time(in, MTIME); f->ctime = get_time(in, CTIME);
    f->btime = ext4.inode_size > 128 && has_extra(in->raw, I_CRTIME_EXTRA + 4) ? get_time(in, CRTIME) : 0;
    if ((mode & S_IFMT_) == S_IFCHR_ || (mode & S_IFMT_) == S_IFBLK_) {
        /* old (8:8) and new encodings both equal the Linux 32-bit st_rdev value */
        uint32_t old = le32(in->raw + I_BLOCK), wide = le32(in->raw + I_BLOCK + 4);
        f->rdev = old ? old : wide;
    }
    f->detached = !f->nlink;
    if (length > FAT32_LFN_MAX) length = FAT32_LFN_MAX;
    if (name) { memcpy(f->name, name, length); f->name[length] = 0; }
    unsigned s = length < 12 ? length : 12; if (name) { memcpy(f->short_name, name, s); f->short_name[s] = 0; }
    f->parent = parent;
}

/* ------------------------------------------------------------------ extent trees */
struct xlevel { struct ext4_buf* buf; uint8_t* node; int pos; };
struct xtree { struct inode* in; int depth; struct xlevel lv[EXT_MAX_DEPTH + 2]; };
static uint16_t x_entries(const uint8_t* node) { return le16(node + 2); }
static uint16_t x_max(const uint8_t* node) { return le16(node + 4); }
static uint16_t x_depth(const uint8_t* node) { return le16(node + 6); }
static uint8_t* x_entry(uint8_t* node, unsigned i) { return node + 12 + 12 * i; }
static uint32_t x_key(uint8_t* node, unsigned i) { return le32(x_entry(node, i)); }
static uint64_t x_leaf_pblk(const uint8_t* e) { return le32(e + 8) | (uint64_t)le16(e + 6) << 32; }
static uint32_t x_leaf_len(const uint8_t* e) { uint32_t l = le16(e + 4); return l > EXT_INIT_MAX ? l - EXT_INIT_MAX : l; }
static bool x_leaf_uninit(const uint8_t* e) { return le16(e + 4) > EXT_INIT_MAX; }
static uint64_t x_index_child(const uint8_t* e) { return le32(e + 4) | (uint64_t)le16(e + 8) << 32; }
static void x_set_leaf(uint8_t* e, uint32_t lblk, uint64_t pblk, uint32_t len, bool uninit) {
    put32(e, lblk); put16(e + 4, len + (uninit ? EXT_INIT_MAX : 0)); put16(e + 6, (uint32_t)(pblk >> 32)); put32(e + 8, (uint32_t)pblk);
}
static void x_set_index(uint8_t* e, uint32_t lblk, uint64_t child) { put32(e, lblk); put32(e + 4, (uint32_t)child); put16(e + 8, (uint32_t)(child >> 32)); put16(e + 10, 0); }
static void x_release(struct xtree* t) { for (int l = 1; l <= t->depth; l++) { ext4_brelse(t->lv[l].buf); t->lv[l].buf = NULL; } }
static uint32_t x_block_csum(const struct inode* in, const uint8_t* node) {
    return ext4_crc32c(inode_seed(in->ino, le32(in->raw + I_GENERATION)), node, 12 + 12 * (unsigned)x_max(node));
}
static void x_dirty(struct xtree* t, int level) {
    if (!level) { idirty(t->in); return; }
    uint8_t* node = t->lv[level].node;
    if (ext4.csum) put32(node + 12 + 12 * x_max(node), x_block_csum(t->in, node));
    ext4_bdirty(t->lv[level].buf);
}
static bool x_header_ok(const uint8_t* node, unsigned max_limit, int expect_depth) {
    return le16(node) == EXT_MAGIC && x_entries(node) <= x_max(node) && x_max(node) && x_max(node) <= max_limit &&
           (expect_depth < 0 || x_depth(node) == expect_depth) && x_depth(node) <= EXT_MAX_DEPTH;
}
/* Position of the last entry whose key <= lblk, or -1. */
static int x_search(uint8_t* node, uint32_t lblk) {
    int lo = 0, hi = (int)x_entries(node) - 1, result = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (x_key(node, mid) <= lblk) { result = mid; lo = mid + 1; } else hi = mid - 1; }
    return result;
}
static int x_find(struct inode* in, uint32_t lblk, struct xtree* t) {
    t->in = in;
    uint8_t* root = in->raw + I_BLOCK;
    if (!x_header_ok(root, 4, -1)) { ext4_log("[EXT4] bad extent root, inode ", "", in->ino); return -E_FSCORRUPT; }
    t->depth = x_depth(root);
    t->lv[0].buf = NULL; t->lv[0].node = root;
    for (int l = 0; l < t->depth; l++) {
        uint8_t* node = t->lv[l].node;
        if (!x_entries(node)) { t->depth = l; x_release(t); t->depth = 0; return -E_FSCORRUPT; }
        int pos = x_search(node, lblk); if (pos < 0) pos = 0;
        t->lv[l].pos = pos;
        uint64_t child = x_index_child(x_entry(node, pos));
        struct ext4_buf* b = ext4_bread(child);
        if (!b) { t->depth = l; x_release(t); return -E_IO; }
        t->lv[l + 1].buf = b; t->lv[l + 1].node = b->data;
        if (!x_header_ok(b->data, (EXT4_BLOCK - 12) / 12, t->depth - l - 1)) {
            t->depth = l + 1; x_release(t); ext4_log("[EXT4] bad extent node, inode ", "", in->ino); return -E_FSCORRUPT;
        }
        if (ext4.csum && !b->txn && le32(b->data + 12 + 12 * x_max(b->data)) != x_block_csum(in, b->data)) {
            t->depth = l + 1; x_release(t); ext4_log("[EXT4] extent block checksum mismatch, inode ", "", in->ino); return -E_FSCORRUPT;
        }
    }
    t->lv[t->depth].pos = x_search(t->lv[t->depth].node, lblk);
    return 0;
}
/* Mapping of lblk: 1 = mapped (pblk, run length from lblk, uninit), 0 = hole (run = blocks to the next extent). */
static int x_map(struct inode* in, uint32_t lblk, uint64_t* pblk, uint32_t* run, bool* uninit) {
    struct xtree t; int error = x_find(in, lblk, &t); if (error) return error;
    uint8_t* leaf = t.lv[t.depth].node; int pos = t.lv[t.depth].pos;
    int result = 0;
    if (pos >= 0) {
        uint8_t* e = x_entry(leaf, pos); uint32_t start = le32(e), len = x_leaf_len(e);
        if (lblk - start < len) {
            *pblk = x_leaf_pblk(e) + (lblk - start); *run = len - (lblk - start); *uninit = x_leaf_uninit(e);
            x_release(&t); return 1;
        }
    }
    uint64_t next = 0x100000000ULL;
    if (pos + 1 < (int)x_entries(leaf)) next = x_key(leaf, pos + 1);
    else for (int l = t.depth - 1; l >= 0; l--) if (t.lv[l].pos + 1 < (int)x_entries(t.lv[l].node)) { next = x_key(t.lv[l].node, t.lv[l].pos + 1); break; }
    *run = next - lblk > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)(next - lblk);
    *pblk = 0; *uninit = false;
    x_release(&t); return result;
}
/* Legacy block-mapped files (read only): direct, indirect, double, triple. */
static int bmap_legacy(struct inode* in, uint32_t lblk, uint64_t* pblk) {
    uint32_t per = EXT4_BLOCK / 4; const uint8_t* ptrs = in->raw + I_BLOCK;
    if (lblk < 12) { *pblk = le32(ptrs + 4 * lblk); return 0; }
    lblk -= 12; uint32_t level, top;
    if (lblk < per) { level = 1; top = le32(ptrs + 48); }
    else if ((lblk -= per) < per * per) { level = 2; top = le32(ptrs + 52); }
    else { lblk -= per * per; level = 3; top = le32(ptrs + 56); }
    uint64_t block = top;
    for (uint32_t l = level; l > 0 && block; l--) {
        uint32_t span = l == 3 ? per * per : l == 2 ? per : 1;
        struct ext4_buf* b = ext4_bread(block); if (!b) return -E_IO;
        block = le32(b->data + 4 * ((lblk / span) % per)); ext4_brelse(b);
    }
    *pblk = block; return 0;
}
static int map_block(struct inode* in, uint32_t lblk, uint64_t* pblk, uint32_t* run, bool* uninit) {
    if (i_flags(in) & IFL_EXTENTS) return x_map(in, lblk, pblk, run, uninit);
    if (i_flags(in) & IFL_INLINE_DATA) return -E_OPNOTSUPP;
    int error = bmap_legacy(in, lblk, pblk); if (error) return error;
    *run = 1; *uninit = false; return *pblk ? 1 : 0;
}
static int alloc_meta_block(struct inode* in, uint64_t* block) {
    uint32_t got; uint64_t goal = groups[(in->ino - 1) / ext4.inodes_per_group].inode_table;
    int error = alloc_blocks(goal, 1, block, &got); if (error) return error;
    add_blocks512(in, 8); return 0;
}
static void fix_keys(struct xtree* t, int level, uint32_t key) {
    for (int l = level; l > 0; l--) {
        uint8_t* parent = t->lv[l - 1].node; int pos = t->lv[l - 1].pos;
        put32(x_entry(parent, pos), key); x_dirty(t, l - 1);
        if (pos) break;
    }
}
static int node_insert(struct xtree* t, int level, const uint8_t entry[12]) {
    uint8_t* node = t->lv[level].node; unsigned entries = x_entries(node), max = x_max(node);
    uint32_t key = le32(entry);
    unsigned pos = (unsigned)(x_search(node, key) + 1);
    if (entries < max) {
        ext4_move(x_entry(node, pos + 1), x_entry(node, pos), 12 * (entries - pos));
        memcpy(x_entry(node, pos), entry, 12); put16(node + 2, entries + 1);
        x_dirty(t, level);
        if (!pos && level) fix_keys(t, level, key);
        if (t->lv[level].pos >= (int)pos) t->lv[level].pos++;
        return 0;
    }
    uint64_t block; int error = alloc_meta_block(t->in, &block); if (error) return error;
    struct ext4_buf* b = ext4_bnew(block); if (!b) return -E_IO;
    uint8_t* fresh = b->data;
    if (!level) { /* root full: push its entries into a new block and deepen the tree */
        if (t->depth >= EXT_MAX_DEPTH) { ext4_brelse(b); return -E_NOSPC; }
        memcpy(fresh, node, 12 + 12 * entries); put16(fresh + 4, (EXT4_BLOCK - 12) / 12);
        put16(node + 2, 1); put16(node + 6, x_depth(node) + 1);
        x_set_index(x_entry(node, 0), x_key(fresh, 0), block);
        for (int l = t->depth; l >= 1; l--) t->lv[l + 1] = t->lv[l];
        t->lv[1].buf = b; t->lv[1].node = fresh; t->lv[1].pos = t->lv[0].pos; t->lv[0].pos = 0;
        t->depth++;
        x_dirty(t, 1); x_dirty(t, 0);
        return node_insert(t, 1, entry);
    }
    /* split: appends start an empty sibling (sequential writes pack leaves), otherwise move half */
    unsigned split = pos == entries ? entries : entries / 2, moved = entries - split;
    put16(fresh, EXT_MAGIC); put16(fresh + 4, max); put16(fresh + 6, x_depth(node)); put32(fresh + 8, 0);
    memcpy(x_entry(fresh, 0), x_entry(node, split), 12 * moved); put16(fresh + 2, moved); put16(node + 2, split);
    struct xlevel saved = t->lv[level];
    if (pos >= split) {
        unsigned p = pos - split;
        ext4_move(x_entry(fresh, p + 1), x_entry(fresh, p), 12 * (moved - p));
        memcpy(x_entry(fresh, p), entry, 12); put16(fresh + 2, moved + 1);
        x_dirty(t, level);
    } else {
        ext4_move(x_entry(node, pos + 1), x_entry(node, pos), 12 * (split - pos));
        memcpy(x_entry(node, pos), entry, 12); put16(node + 2, split + 1);
        x_dirty(t, level);
        if (!pos) fix_keys(t, level, key);
    }
    t->lv[level].buf = b; t->lv[level].node = fresh; x_dirty(t, level); t->lv[level] = saved;
    uint8_t index[12]; x_set_index(index, x_key(fresh, 0), block);
    ext4_brelse(b);
    return node_insert(t, level - 1, index);
}
/* Insert a mapping for the unmapped range [lblk, lblk + len), merging with neighbours. */
static int x_insert(struct inode* in, uint32_t lblk, uint64_t pblk, uint32_t len, bool uninit) {
    struct xtree t; int error = x_find(in, lblk, &t); if (error) return error;
    uint8_t* leaf = t.lv[t.depth].node; int pos = t.lv[t.depth].pos;
    uint32_t limit = uninit ? EXT_UNINIT_MAX : EXT_INIT_MAX;
    if (pos >= 0) {
        uint8_t* e = x_entry(leaf, pos); uint32_t l = x_leaf_len(e);
        if (x_leaf_uninit(e) == uninit && le32(e) + l == lblk && x_leaf_pblk(e) + l == pblk && l + len <= limit) {
            put16(e + 4, l + len + (uninit ? EXT_INIT_MAX : 0)); x_dirty(&t, t.depth); x_release(&t); return 0;
        }
    }
    if (pos + 1 < (int)x_entries(leaf)) {
        uint8_t* e = x_entry(leaf, pos + 1); uint32_t l = x_leaf_len(e);
        if (x_leaf_uninit(e) == uninit && lblk + len == le32(e) && pblk + len == x_leaf_pblk(e) && l + len <= limit) {
            x_set_leaf(e, lblk, pblk, l + len, uninit); x_dirty(&t, t.depth);
            if (pos + 1 == 0 && t.depth) fix_keys(&t, t.depth, lblk);
            x_release(&t); return 0;
        }
    }
    uint8_t entry[12]; x_set_leaf(entry, lblk, pblk, len, uninit);
    error = node_insert(&t, t.depth, entry);
    x_release(&t); return error;
}
/* Delete entry `pos` of node `level`; empty non-root nodes are freed and unlinked from the parent. */
static void node_delete(struct xtree* t, int level, unsigned pos) {
    uint8_t* node = t->lv[level].node; unsigned entries = x_entries(node);
    ext4_move(x_entry(node, pos), x_entry(node, pos + 1), 12 * (entries - pos - 1));
    put16(node + 2, entries - 1);
    if (entries - 1 == 0 && level > 0) {
        free_blocks(t->lv[level].buf->block, 1); add_blocks512(t->in, -8);
        node_delete(t, level - 1, (unsigned)t->lv[level - 1].pos);
        return;
    }
    if (entries - 1 == 0 && !level) put16(node + 6, 0); /* the whole tree is empty again */
    x_dirty(t, level);
    if (!pos && entries > 1 && level) fix_keys(t, level, x_key(node, 0));
}
/* Pull a lone child node back into the inode when its entries fit (Linux ext4_ext_try_to_merge_up). */
static void x_collapse(struct inode* in) {
    uint8_t* root = in->raw + I_BLOCK;
    while (x_depth(root) && x_entries(root) == 1) {
        uint64_t child = x_index_child(x_entry(root, 0));
        struct ext4_buf* b = ext4_bread(child); if (!b) return;
        unsigned n = x_entries(b->data);
        if (n > 4 || le16(b->data) != EXT_MAGIC) { ext4_brelse(b); return; }
        uint16_t depth = x_depth(b->data);
        ext4_move(x_entry(root, 0), x_entry(b->data, 0), 12 * n);
        put16(root + 2, n); put16(root + 6, depth);
        ext4_brelse(b); free_blocks(child, 1); add_blocks512(in, -8);
    }
}
/* Remove mappings within [from, to); returns 1 if more work remains (budget used up). */
static int x_remove(struct inode* in, uint32_t from, uint64_t to, bool free_data, unsigned budget) {
    while (budget--) {
        if (to <= from) { x_collapse(in); return 0; }
        uint32_t last = to - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)(to - 1);
        struct xtree t; int error = x_find(in, last, &t); if (error) return error;
        uint8_t* leaf = t.lv[t.depth].node; int pos = t.lv[t.depth].pos;
        if (pos < 0) { x_release(&t); x_collapse(in); return 0; }
        uint8_t* e = x_entry(leaf, pos);
        uint32_t start = le32(e), len = x_leaf_len(e); uint64_t end = (uint64_t)start + len, p = x_leaf_pblk(e);
        bool uninit = x_leaf_uninit(e);
        if (end <= from) { x_release(&t); x_collapse(in); return 0; }
        uint64_t cut_from = start > from ? start : from, cut_to = end < to ? end : to;
        if (free_data) { free_blocks(p + (cut_from - start), (uint32_t)(cut_to - cut_from)); add_blocks512(in, -(int64_t)(cut_to - cut_from) * 8); }
        if (start >= from && end <= to) node_delete(&t, t.depth, (unsigned)pos);
        else if (start < from && end <= to) { put16(e + 4, (from - start) + (uninit ? EXT_INIT_MAX : 0)); x_dirty(&t, t.depth); }
        else if (start >= from) {
            uint32_t shift = (uint32_t)(to - start);
            x_set_leaf(e, (uint32_t)to, p + shift, len - shift, uninit); x_dirty(&t, t.depth);
            if (!pos && t.depth) fix_keys(&t, t.depth, (uint32_t)to);
        } else {
            uint8_t right[12]; x_set_leaf(right, (uint32_t)to, p + (to - start), (uint32_t)(end - to), uninit);
            put16(e + 4, (from - start) + (uninit ? EXT_INIT_MAX : 0)); x_dirty(&t, t.depth);
            error = node_insert(&t, t.depth, right);
            x_release(&t); if (error) return error;
            to = from; continue;
        }
        x_release(&t);
        to = cut_from;
        if (free_data && !pfree_room()) return 1;
    }
    return 1;
}
/* Mark [lblk, lblk+len) (inside one unwritten extent) as written. */
static int x_convert(struct inode* in, uint32_t lblk, uint32_t len) {
    uint64_t pblk; uint32_t run; bool uninit;
    int r = x_map(in, lblk, &pblk, &run, &uninit); if (r < 0) return r;
    if (r != 1 || !uninit || run < len) return -E_FSCORRUPT;
    r = x_remove(in, lblk, (uint64_t)lblk + len, false, 4); if (r < 0) return r;
    return x_insert(in, lblk, pblk, len, false);
}
static void x_init_root(struct inode* in) {
    uint8_t* root = in->raw + I_BLOCK; memset(root, 0, 60);
    put16(root, EXT_MAGIC); put16(root + 4, 4);
}

/* ------------------------------------------------------------------ directory hashing */
static uint32_t rol32(uint32_t x, unsigned s) { return x << s | x >> (32 - s); }
static void str2hashbuf(const uint8_t* msg, int len, uint32_t* buf, int num, bool sign) {
    uint32_t pad = (uint32_t)len | ((uint32_t)len << 8), val; pad |= pad << 16; val = pad;
    if (len > num * 4) len = num * 4;
    for (int i = 0; i < len; i++) {
        int c = sign ? (int)(int8_t)msg[i] : (int)msg[i];
        val = (uint32_t)c + (val << 8);
        if ((i % 4) == 3) { *buf++ = val; val = pad; num--; }
    }
    if (--num >= 0) *buf++ = val;
    while (--num >= 0) *buf++ = pad;
}
#define HF(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define HG(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define HH(x, y, z) ((x) ^ (y) ^ (z))
#define ROUND(f, a, b, c, d, x, s) (a += f(b, c, d) + (x), a = rol32(a, s))
static void half_md4(uint32_t buf[4], const uint32_t in[8]) {
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
    const uint32_t K2 = 0x5A827999u, K3 = 0x6ED9EBA1u;
    ROUND(HF, a, b, c, d, in[0], 3); ROUND(HF, d, a, b, c, in[1], 7); ROUND(HF, c, d, a, b, in[2], 11); ROUND(HF, b, c, d, a, in[3], 19);
    ROUND(HF, a, b, c, d, in[4], 3); ROUND(HF, d, a, b, c, in[5], 7); ROUND(HF, c, d, a, b, in[6], 11); ROUND(HF, b, c, d, a, in[7], 19);
    ROUND(HG, a, b, c, d, in[1] + K2, 3); ROUND(HG, d, a, b, c, in[3] + K2, 5); ROUND(HG, c, d, a, b, in[5] + K2, 9); ROUND(HG, b, c, d, a, in[7] + K2, 13);
    ROUND(HG, a, b, c, d, in[0] + K2, 3); ROUND(HG, d, a, b, c, in[2] + K2, 5); ROUND(HG, c, d, a, b, in[4] + K2, 9); ROUND(HG, b, c, d, a, in[6] + K2, 13);
    ROUND(HH, a, b, c, d, in[3] + K3, 3); ROUND(HH, d, a, b, c, in[7] + K3, 9); ROUND(HH, c, d, a, b, in[2] + K3, 11); ROUND(HH, b, c, d, a, in[6] + K3, 15);
    ROUND(HH, a, b, c, d, in[1] + K3, 3); ROUND(HH, d, a, b, c, in[5] + K3, 9); ROUND(HH, c, d, a, b, in[0] + K3, 11); ROUND(HH, b, c, d, a, in[4] + K3, 15);
    buf[0] += a; buf[1] += b; buf[2] += c; buf[3] += d;
}
static void tea(uint32_t buf[4], const uint32_t in[4]) {
    uint32_t sum = 0, b0 = buf[0], b1 = buf[1], a = in[0], b = in[1], c = in[2], d = in[3];
    for (int n = 0; n < 16; n++) {
        sum += 0x9E3779B9u;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    }
    buf[0] += b0; buf[1] += b1;
}
static uint32_t dirhash(const uint8_t* name, int len, unsigned version, uint32_t* minor_out) {
    uint32_t buf[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u}, in[8], hash = 0, minor = 0;
    if (ext4.hash_seed[0] | ext4.hash_seed[1] | ext4.hash_seed[2] | ext4.hash_seed[3]) memcpy(buf, ext4.hash_seed, 16);
    bool sign = version < 3; unsigned v = version % 3;
    if (v == 0) {
        uint32_t h0 = 0x12a3fe2du, h1 = 0x37abe8f9u;
        for (int i = 0; i < len; i++) {
            int c = sign ? (int)(int8_t)name[i] : (int)name[i];
            uint32_t h = h1 + (h0 ^ (uint32_t)(c * 7152373));
            if (h & 0x80000000u) h -= 0x7fffffffu;
            h1 = h0; h0 = h;
        }
        hash = h0 << 1;
    } else if (v == 1) {
        const uint8_t* p = name;
        for (int n = len; n > 0; n -= 32, p += 32) { str2hashbuf(p, n, in, 8, sign); half_md4(buf, in); }
        minor = buf[2]; hash = buf[1];
    } else {
        const uint8_t* p = name;
        for (int n = len; n > 0; n -= 16, p += 16) { str2hashbuf(p, n, in, 4, sign); tea(buf, in); }
        hash = buf[0]; minor = buf[1];
    }
    hash &= ~1u;
    if (hash == (0x7fffffffu << 1)) hash = (0x7fffffffu - 1) << 1;
    if (minor_out) *minor_out = minor;
    return hash;
}

/* ------------------------------------------------------------------ directory blocks */
#define DIRENT_TAIL 12u
static unsigned rec_size(unsigned name_len) { return (8 + name_len + 3) & ~3u; }
static uint32_t dir_leaf_end(void) { return ext4.csum ? EXT4_BLOCK - DIRENT_TAIL : EXT4_BLOCK; }
static bool has_tail(const uint8_t* blk) {
    const uint8_t* t = blk + EXT4_BLOCK - DIRENT_TAIL;
    return !le32(t) && le16(t + 4) == DIRENT_TAIL && !t[6] && t[7] == 0xDE;
}
/* Index blocks of an htree directory, as Linux is_dx_block tells them apart: the root ("." then ".."
 * spanning the rest of the block) and interior nodes (one empty entry spanning the whole block). They
 * carry a dx_tail, never a leaf checksum -- and a root built by Linux make_indexed_dir keeps the old
 * leaf's tail bytes inside its dx_tail, so the 0xDE tail signature alone does not identify a leaf. */
static bool is_dx_block(const struct inode* dir, const uint8_t* blk) {
    if (!(i_flags(dir) & IFL_INDEX)) return false;
    if (!le32(blk) && le16(blk + 4) == EXT4_BLOCK) return true;
    return blk[6] == 1 && blk[8] == '.' && le16(blk + 4) == 12 && blk[18] == 2 && blk[20] == '.' && le16(blk + 16) == EXT4_BLOCK - 12;
}
static void leaf_csum(const struct inode* dir, uint8_t* blk) {
    if (!ext4.csum || !has_tail(blk) || is_dx_block(dir, blk)) return;
    put32(blk + EXT4_BLOCK - 4, ext4_crc32c(inode_seed(dir->ino, le32(dir->raw + I_GENERATION)), blk, EXT4_BLOCK - DIRENT_TAIL));
}
static void init_tail(uint8_t* blk) {
    if (!ext4.csum) return;
    uint8_t* t = blk + EXT4_BLOCK - DIRENT_TAIL; memset(t, 0, DIRENT_TAIL); put16(t + 4, DIRENT_TAIL); t[7] = 0xDE;
}
static void dx_csum(const struct inode* dir, uint8_t* blk, unsigned count_offset) {
    if (!ext4.csum) return;
    unsigned limit = le16(blk + count_offset), count = le16(blk + count_offset + 2), tail = count_offset + limit * 8;
    if (tail + 8 > EXT4_BLOCK) return;
    uint8_t zero[4] = {0, 0, 0, 0}; /* the tail is hashed with dt_checksum = 0 (e2fsprogs ext2fs_dx_csum) */
    uint32_t c = ext4_crc32c(inode_seed(dir->ino, le32(dir->raw + I_GENERATION)), blk, count_offset + count * 8);
    c = ext4_crc32c(ext4_crc32c(c, blk + tail, 4), zero, 4);
    put32(blk + tail + 4, c);
}
static bool dx_csum_ok(const struct inode* dir, const uint8_t* blk, unsigned count_offset) {
    if (!ext4.csum) return true;
    unsigned limit = le16(blk + count_offset), count = le16(blk + count_offset + 2), tail = count_offset + limit * 8;
    if (tail + 8 > EXT4_BLOCK || count > limit) return false;
    uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t c = ext4_crc32c(inode_seed(dir->ino, le32(dir->raw + I_GENERATION)), blk, count_offset + count * 8);
    return ext4_crc32c(ext4_crc32c(c, blk + tail, 4), zero, 4) == le32(blk + tail + 4);
}
/* Validate a leaf block once per read from disk. */
static int leaf_check(const struct inode* dir, struct ext4_buf* b) {
    uint8_t* blk = b->data; uint32_t off = 0;
    while (off < EXT4_BLOCK) {
        if (off + 8 > EXT4_BLOCK) return -E_FSCORRUPT;
        uint32_t rec = le16(blk + off + 4), nl = blk[off + 6];
        if (rec < 8 || rec % 4 || off + rec > EXT4_BLOCK || (le32(blk + off) && 8 + nl > rec)) return -E_FSCORRUPT;
        off += rec;
    }
    if (ext4.csum && has_tail(blk) && !b->txn && !is_dx_block(dir, blk) &&
        le32(blk + EXT4_BLOCK - 4) != ext4_crc32c(inode_seed(dir->ino, le32(dir->raw + I_GENERATION)), blk, EXT4_BLOCK - DIRENT_TAIL)) {
        ext4_log("[EXT4] directory block checksum mismatch, directory ", "", dir->ino);
        return -E_FSCORRUPT;
    }
    return 0;
}
static int dir_block(struct inode* dir, uint32_t lblk, struct ext4_buf** out) {
    uint64_t pblk; uint32_t run; bool uninit;
    int r = map_block(dir, lblk, &pblk, &run, &uninit); if (r < 0) return r;
    if (r == 0 || uninit) return -E_FSCORRUPT;
    struct ext4_buf* b = ext4_bread(pblk); if (!b) return -E_IO;
    *out = b; return 0;
}

/* dentry cache: (directory, name) -> inode (0 = known absent) */
struct dentry { uint32_t dir, ino; uint8_t len, ftype, used, pad; char name[52]; };
#define DCACHE_SLOTS 4096u
static struct dentry* dcache;
static unsigned dslot(uint32_t dir, const char* name, unsigned len) {
    uint32_t h = 2166136261u ^ dir * 0x9E3779B1u;
    for (unsigned i = 0; i < len; i++) h = (h ^ (uint8_t)name[i]) * 16777619u;
    return h & (DCACHE_SLOTS - 1);
}
static struct dentry* dc_find(uint32_t dir, const char* name, unsigned len) {
    if (!dcache || len > sizeof(dcache->name)) return NULL;
    struct dentry* d = &dcache[dslot(dir, name, len)];
    return d->used && d->dir == dir && d->len == len && !memcmp(d->name, name, len) ? d : NULL;
}
static void dc_put(uint32_t dir, const char* name, unsigned len, uint32_t ino, uint8_t ftype) {
    if (!dcache || len > sizeof(dcache->name)) return;
    struct dentry* d = &dcache[dslot(dir, name, len)];
    d->dir = dir; d->ino = ino; d->len = (uint8_t)len; d->ftype = ftype; d->used = 1; memcpy(d->name, name, len);
}
static void dc_purge_dir(uint32_t dir) { if (dcache) for (unsigned i = 0; i < DCACHE_SLOTS; i++) if (dcache[i].dir == dir) dcache[i].used = 0; }

struct dloc { uint32_t lblk, off, prev, ino; uint8_t ftype; bool found; };
static int scan_leaf(struct inode* dir, struct ext4_buf* b, const char* name, unsigned len, struct dloc* loc) {
    if (!b->valid) return -E_IO;
    int error = leaf_check(dir, b); if (error) return error;
    uint8_t* blk = b->data; uint32_t off = 0, prev = 0xFFFFFFFFu;
    while (off < EXT4_BLOCK) {
        uint32_t ino = le32(blk + off), rec = le16(blk + off + 4), nl = blk[off + 6];
        if (ino && nl == len && !memcmp(blk + off + 8, name, len)) {
            loc->off = off; loc->prev = prev; loc->ino = ino; loc->ftype = blk[off + 7]; loc->found = true; return 1;
        }
        prev = off; off += rec;
    }
    return 0;
}
/* htree frames: root (and one interior level) positions for a hash */
struct dxframe { struct ext4_buf* buf; unsigned at, count_offset; unsigned pos; uint32_t lblk; };
static unsigned dir_hash_version(const uint8_t* root) {
    unsigned v = root[28];
    if (v <= 2 && ext4.unsigned_hash) v += 3;
    return v;
}
static void dx_release(struct dxframe* f, int n) { for (int i = 0; i < n; i++) ext4_brelse(f[i].buf); }
static uint32_t dx_hash_at(const uint8_t* node, unsigned count_offset, unsigned i) { return i ? le32(node + count_offset + 8 * i) : 0; }
static uint32_t dx_block_at(const uint8_t* node, unsigned count_offset, unsigned i) { return le32(node + count_offset + 8 * i + 4); }
/* Walk the index for `hash`; frames[0..levels] filled (last one names the leaf). Returns levels+1 or <0;
 * -E_NODATA means the index is unusable and the caller should scan linearly. */
static int dx_probe(struct inode* dir, uint32_t hash, struct dxframe* f) {
    struct ext4_buf* b; int error = dir_block(dir, 0, &b); if (error) return error;
    uint8_t* root = b->data;
    if (le32(root + 24) || root[29] != 8 || root[30] > 1 || dir_hash_version(root) > 5 || memcmp(root + 8, ".", 1) || memcmp(root + 20, "..", 2)) {
        ext4_brelse(b); return -E_NODATA;
    }
    int levels = root[30];
    f[0].buf = b; f[0].count_offset = 32; f[0].lblk = 0;
    for (int l = 0; l <= levels; l++) {
        uint8_t* node = f[l].buf->data; unsigned co = f[l].count_offset;
        unsigned limit = le16(node + co), count = le16(node + co + 2);
        if (!count || count > limit || co + 8u * limit > EXT4_BLOCK || (ext4.csum && !f[l].buf->txn && !dx_csum_ok(dir, node, co))) {
            dx_release(f, l + 1); return -E_NODATA;
        }
        unsigned lo = 1, hi = count - 1, pos = 0;
        while (lo <= hi && hi < count) { unsigned mid = (lo + hi) / 2; if (dx_hash_at(node, co, mid) <= hash) { pos = mid; lo = mid + 1; } else { if (!mid) break; hi = mid - 1; } }
        f[l].pos = pos;
        if (l == levels) return levels + 1;
        uint32_t child = dx_block_at(node, co, pos);
        if ((error = dir_block(dir, child, &b))) { dx_release(f, l + 1); return error; }
        if (le32(b->data) || le16(b->data + 4) != EXT4_BLOCK) { ext4_brelse(b); dx_release(f, l + 1); return -E_NODATA; }
        f[l + 1].buf = b; f[l + 1].count_offset = 8; f[l + 1].lblk = child;
    }
    dx_release(f, levels + 1); return -E_NODATA;
}
/* Advance to the next leaf; returns 1 if it continues the same hash (collision run), 0 otherwise. */
static int dx_next(struct inode* dir, uint32_t hash, struct dxframe* f, int n) {
    int l = n - 1;
    while (l >= 0) {
        unsigned count = le16(f[l].buf->data + f[l].count_offset + 2);
        if (f[l].pos + 1 < count) break;
        l--;
    }
    if (l < 0) return 0;
    f[l].pos++;
    uint32_t bhash = dx_hash_at(f[l].buf->data, f[l].count_offset, f[l].pos);
    for (int k = l + 1; k < n; k++) {
        struct ext4_buf* b; uint32_t child = dx_block_at(f[k - 1].buf->data, f[k - 1].count_offset, f[k - 1].pos);
        if (dir_block(dir, child, &b)) return 0;
        ext4_brelse(f[k].buf); f[k].buf = b; f[k].lblk = child; f[k].pos = 0;
    }
    return (bhash & 1) && (bhash & ~1u) == hash;
}
static int dir_find(struct inode* dir, const char* name, unsigned len, struct dloc* loc) {
    loc->found = false;
    if ((i_flags(dir) & IFL_INDEX) && (ext4.compat & COMPAT_DIR_INDEX)) {
        struct dxframe f[3]; struct ext4_buf* root;
        int error = dir_block(dir, 0, &root); if (error) return error;
        uint32_t hash = dirhash((const uint8_t*)name, (int)len, dir_hash_version(root->data), NULL);
        ext4_brelse(root);
        int n = dx_probe(dir, hash, f);
        if (n > 0) {
            for (;;) {
                uint32_t leaf = dx_block_at(f[n - 1].buf->data, f[n - 1].count_offset, f[n - 1].pos);
                struct ext4_buf* b; error = dir_block(dir, leaf, &b);
                if (error) { dx_release(f, n); return error; }
                int r = scan_leaf(dir, b, name, len, loc); ext4_brelse(b);
                if (r) { if (r > 0) loc->lblk = leaf; dx_release(f, n); return r < 0 ? r : 0; }
                if (!dx_next(dir, hash, f, n)) break;
            }
            dx_release(f, n); return 0;
        }
        if (n != -E_NODATA) return n;
    }
    uint32_t blocks = (uint32_t)(i_size(dir) / EXT4_BLOCK);
    for (uint32_t lblk = 0; lblk < blocks; lblk++) {
        struct ext4_buf* b; int error = dir_block(dir, lblk, &b);
        if (error == -E_FSCORRUPT) continue; /* hole in a directory */
        if (error) return error;
        int r = scan_leaf(dir, b, name, len, loc); ext4_brelse(b);
        if (r < 0) return r;
        if (r) { loc->lblk = lblk; return 0; }
    }
    return 0;
}
/* Put an entry into a block if it fits. */
static bool leaf_insert(uint8_t* blk, const char* name, unsigned len, uint32_t ino, uint8_t ftype) {
    unsigned need = rec_size(len); uint32_t off = 0, end = dir_leaf_end();
    if (ext4.csum && !has_tail(blk)) end = EXT4_BLOCK;
    while (off < end) {
        uint32_t cur = le32(blk + off), rec = le16(blk + off + 4), nl = blk[off + 6];
        if (rec < 8 || off + rec > end) return false;
        uint32_t used = cur ? rec_size(nl) : 0;
        if (rec - used >= need) {
            uint32_t at = off;
            if (cur) { put16(blk + off + 4, used); at = off + used; put16(blk + at + 4, rec - used); }
            put32(blk + at, ino); blk[at + 6] = (uint8_t)len; blk[at + 7] = ftype; memcpy(blk + at + 8, name, len);
            return true;
        }
        off += rec;
    }
    return false;
}
static void leaf_init(uint8_t* blk) {
    memset(blk, 0, EXT4_BLOCK); put16(blk + 4, dir_leaf_end()); init_tail(blk);
}
/* Append a new (logical) block to a directory. */
static int dir_append_block(struct inode* dir, struct ext4_buf** out, uint32_t* lblk_out) {
    uint32_t lblk = (uint32_t)(i_size(dir) / EXT4_BLOCK);
    uint64_t goal = 0, pblk; uint32_t run, got; bool uninit;
    if (lblk && map_block(dir, lblk - 1, &goal, &run, &uninit) == 1) goal++;
    else goal = groups[(dir->ino - 1) / ext4.inodes_per_group].inode_table;
    int error = alloc_blocks(goal, 1, &pblk, &got); if (error) return error;
    if ((error = x_insert(dir, lblk, pblk, 1, false))) { free_blocks(pblk, 1); return error; }
    add_blocks512(dir, 8); set_size(dir, (uint64_t)(lblk + 1) * EXT4_BLOCK);
    struct ext4_buf* b = ext4_bnew(pblk); if (!b) return -E_IO;
    *out = b; *lblk_out = lblk; return 0;
}

struct dmap { uint32_t hash, minor; uint16_t off, size; };
static void sort_map(struct dmap* m, unsigned n) {
    for (unsigned i = 1; i < n; i++) {
        struct dmap x = m[i]; unsigned j = i;
        while (j && (m[j - 1].hash > x.hash || (m[j - 1].hash == x.hash && m[j - 1].minor > x.minor))) { m[j] = m[j - 1]; j--; }
        m[j] = x;
    }
}
/* Collect live entries of a leaf (copied to SPLIT_COPY) sorted by hash into SPLIT_MAP. */
static unsigned leaf_map(const uint8_t* blk, unsigned version, unsigned skip_dots) {
    memcpy(SPLIT_COPY, blk, EXT4_BLOCK);
    struct dmap* m = (struct dmap*)SPLIT_MAP; unsigned n = 0; uint32_t off = 0;
    while (off < EXT4_BLOCK) {
        uint32_t ino = le32(SPLIT_COPY + off), rec = le16(SPLIT_COPY + off + 4), nl = SPLIT_COPY[off + 6];
        if (rec < 8) break;
        bool dot = skip_dots && ((nl == 1 && SPLIT_COPY[off + 8] == '.') || (nl == 2 && SPLIT_COPY[off + 8] == '.' && SPLIT_COPY[off + 9] == '.'));
        if (ino && !dot && !(nl == 0 && SPLIT_COPY[off + 7] == 0xDE)) {
            m[n].hash = dirhash(SPLIT_COPY + off + 8, (int)nl, version, &m[n].minor);
            m[n].off = (uint16_t)off; m[n].size = (uint16_t)rec_size(nl); n++;
        }
        off += rec;
    }
    sort_map(m, n); return n;
}
static void leaf_fill(uint8_t* blk, const struct dmap* m, unsigned from, unsigned to) {
    leaf_init(blk);
    uint32_t off = 0, end = dir_leaf_end();
    for (unsigned i = from; i < to; i++) {
        const uint8_t* src = SPLIT_COPY + m[i].off; unsigned nl = src[6];
        memcpy(blk + off, src, 8 + nl); put16(blk + off + 4, m[i].size);
        if (i + 1 == to) put16(blk + off + 4, end - off);
        off += m[i].size;
    }
    if (from == to) put16(blk + 4, end);
}
/* Choose the split point (by bytes) and its index hash (bit 0 = continuation of an equal-hash run). */
static unsigned split_point(const struct dmap* m, unsigned n, uint32_t* hash2) {
    unsigned total = 0, half, size = 0, i;
    for (i = 0; i < n; i++) total += m[i].size;
    half = total / 2;
    for (i = 0; i < n && size + m[i].size <= half; i++) size += m[i].size;
    if (!i) i = 1;
    if (i >= n) i = n - 1;
    *hash2 = m[i].hash;
    if (m[i].hash == m[i - 1].hash) *hash2 |= 1;
    return i;
}
/* Insert an index entry (hash, lblk) after position `pos` of an index node. */
static void dx_insert_entry(uint8_t* node, unsigned co, unsigned pos, uint32_t hash, uint32_t lblk) {
    unsigned count = le16(node + co + 2);
    uint8_t* at = node + co + 8 * (pos + 1);
    ext4_move(at + 8, at, 8 * (count - pos - 1));
    put32(at, hash); put32(at + 4, lblk); put16(node + co + 2, count + 1);
}
/* Convert a full single-block linear directory into an indexed one (Linux make_indexed_dir). */
static int make_indexed(struct inode* dir, struct ext4_buf* b0) {
    uint32_t parent = 0; unsigned version = 1 + (ext4.unsigned_hash ? 3 : 0);
    { uint32_t off = 0; while (off < EXT4_BLOCK) { uint32_t rec = le16(b0->data + off + 4); if (rec < 8) break;
        if (b0->data[off + 6] == 2 && !memcmp(b0->data + off + 8, "..", 2)) { parent = le32(b0->data + off); break; } off += rec; } }
    if (!parent) return -E_FSCORRUPT;
    unsigned n = leaf_map(b0->data, version, 1);
    struct dmap* m = (struct dmap*)SPLIT_MAP;
    if (n < 2) return -E_NOSPC;
    struct ext4_buf *b1, *b2; uint32_t l1, l2;
    int error = dir_append_block(dir, &b1, &l1); if (error) return error;
    if ((error = dir_append_block(dir, &b2, &l2))) { ext4_brelse(b1); return error; }
    uint32_t hash2; unsigned split = split_point(m, n, &hash2);
    leaf_fill(b1->data, m, 0, split); leaf_fill(b2->data, m, split, n);
    leaf_csum(dir, b1->data); leaf_csum(dir, b2->data); ext4_bdirty(b1); ext4_bdirty(b2);
    ext4_brelse(b1); ext4_brelse(b2);
    uint8_t* r = b0->data; memset(r, 0, EXT4_BLOCK);
    put32(r, dir->ino); put16(r + 4, 12); r[6] = 1; r[7] = 2; r[8] = '.';
    put32(r + 12, parent); put16(r + 16, EXT4_BLOCK - 12); r[18] = 2; r[19] = 2; r[20] = '.'; r[21] = '.';
    put32(r + 24, 0); r[28] = (uint8_t)(1 + (ext4.unsigned_hash ? 3 : 0)); r[29] = 8; r[30] = 0; r[31] = 0;
    unsigned limit = (EXT4_BLOCK - 32) / 8 - (ext4.csum ? 1 : 0);
    put16(r + 32, limit); put16(r + 34, 2); put32(r + 36, l1); put32(r + 40, hash2); put32(r + 44, l2);
    dx_csum(dir, r, 32); ext4_bdirty(b0);
    put32(dir->raw + I_FLAGS, i_flags(dir) | IFL_INDEX);
    return 0;
}

/* htree insertion: insert into the leaf for the name's hash, splitting the leaf (and, when needed,
 * the index node or adding the second index level) exactly like Linux ext4_dx_add_entry. */
static int dx_add(struct inode* dir, const char* name, unsigned len, uint32_t ino, uint8_t ftype) {
    struct ext4_buf* rootb; int error = dir_block(dir, 0, &rootb); if (error) return error;
    unsigned version = dir_hash_version(rootb->data); ext4_brelse(rootb);
    uint32_t hash = dirhash((const uint8_t*)name, (int)len, version, NULL);
    struct dxframe f[3];
    int n = dx_probe(dir, hash, f);
    if (n < 0) return n == -E_NODATA ? -E_FSCORRUPT : n;
    struct dxframe* bottom = &f[n - 1];
    struct ext4_buf* leaf;
    if ((error = dir_block(dir, dx_block_at(bottom->buf->data, bottom->count_offset, bottom->pos), &leaf))) { dx_release(f, n); return error; }
    if ((error = leaf_check(dir, leaf))) { ext4_brelse(leaf); dx_release(f, n); return error; }
    if (leaf_insert(leaf->data, name, len, ino, ftype)) {
        leaf_csum(dir, leaf->data); ext4_bdirty(leaf); ext4_brelse(leaf); dx_release(f, n); return 0;
    }
    uint8_t* node = bottom->buf->data; unsigned co = bottom->count_offset;
    if (le16(node + co + 2) >= le16(node + co)) {
        struct ext4_buf* nb; uint32_t nl;
        unsigned limit = (EXT4_BLOCK - 8) / 8 - (ext4.csum ? 1 : 0);
        if (n == 1) { /* root full: move its entries into a new index node (second level) */
            if ((error = dir_append_block(dir, &nb, &nl))) goto out;
            uint8_t* x = nb->data; unsigned count = le16(node + co + 2);
            memset(x, 0, EXT4_BLOCK); put16(x + 4, EXT4_BLOCK);
            put16(x + 8, limit); put16(x + 10, count);
            memcpy(x + 12, node + co + 4, 4 + 8 * (count - 1));
            dx_csum(dir, x, 8); ext4_bdirty(nb);
            put16(node + co + 2, 1); put32(node + co + 4, nl); node[30] = 1;
            dx_csum(dir, node, co); ext4_bdirty(bottom->buf);
            f[1].buf = nb; f[1].count_offset = 8; f[1].pos = f[0].pos; f[1].lblk = nl; f[0].pos = 0; n = 2;
            bottom = &f[1]; node = x; co = 8;
        } else { /* interior node full: split it if the root has room */
            uint8_t* root = f[0].buf->data;
            if (le16(root + 34) >= le16(root + 32)) { error = -E_NOSPC; goto out; }
            if ((error = dir_append_block(dir, &nb, &nl))) goto out;
            unsigned count = le16(node + co + 2), keep = count / 2, move = count - keep;
            uint8_t* x = nb->data;
            memset(x, 0, EXT4_BLOCK); put16(x + 4, EXT4_BLOCK);
            put16(x + 8, limit); put16(x + 10, move);
            uint32_t split_hash = le32(node + co + 8 * keep);
            put32(x + 12, le32(node + co + 8 * keep + 4));
            memcpy(x + 16, node + co + 8 * (keep + 1), 8 * (move - 1));
            put16(node + co + 2, keep);
            dx_csum(dir, x, 8); ext4_bdirty(nb); dx_csum(dir, node, co); ext4_bdirty(bottom->buf);
            dx_insert_entry(root, 32, f[0].pos, split_hash, nl); dx_csum(dir, root, 32); ext4_bdirty(f[0].buf);
            if (bottom->pos >= keep) {
                ext4_brelse(bottom->buf); bottom->buf = nb; bottom->pos -= keep; bottom->lblk = nl; f[0].pos++; node = x;
            } else ext4_brelse(nb);
        }
    }
    {
        struct ext4_buf* nb; uint32_t nl;
        if ((error = dir_append_block(dir, &nb, &nl))) goto out;
        unsigned count = leaf_map(leaf->data, version, 0);
        struct dmap* m = (struct dmap*)SPLIT_MAP;
        uint32_t hash2; unsigned split = split_point(m, count, &hash2);
        leaf_fill(leaf->data, m, 0, split); leaf_fill(nb->data, m, split, count);
        uint8_t* target = hash >= (hash2 & ~1u) ? nb->data : leaf->data;
        if (!leaf_insert(target, name, len, ino, ftype)) error = -E_NOSPC;
        leaf_csum(dir, leaf->data); leaf_csum(dir, nb->data); ext4_bdirty(leaf); ext4_bdirty(nb); ext4_brelse(nb);
        dx_insert_entry(node, co, bottom->pos, hash2, nl); dx_csum(dir, node, co); ext4_bdirty(bottom->buf);
    }
out:
    ext4_brelse(leaf); dx_release(f, n);
    return error;
}
static int dir_add(struct inode* dir, const char* name, unsigned len, uint32_t ino, uint8_t ftype) {
    if ((i_flags(dir) & IFL_INDEX) && (ext4.compat & COMPAT_DIR_INDEX)) return dx_add(dir, name, len, ino, ftype);
    if (i_flags(dir) & IFL_INDEX) return -E_FSCORRUPT;
    uint32_t blocks = (uint32_t)(i_size(dir) / EXT4_BLOCK);
    for (uint32_t lblk = 0; lblk < blocks; lblk++) {
        struct ext4_buf* b; int error = dir_block(dir, lblk, &b);
        if (error == -E_FSCORRUPT) continue;
        if (error) return error;
        if ((error = leaf_check(dir, b))) { ext4_brelse(b); return error; }
        if (leaf_insert(b->data, name, len, ino, ftype)) { leaf_csum(dir, b->data); ext4_bdirty(b); ext4_brelse(b); return 0; }
        if (blocks == 1 && (ext4.compat & COMPAT_DIR_INDEX) && (i_flags(dir) & IFL_EXTENTS)) {
            error = make_indexed(dir, b); ext4_brelse(b);
            if (error) return error;
            return dx_add(dir, name, len, ino, ftype);
        }
        ext4_brelse(b);
    }
    struct ext4_buf* b; uint32_t lblk;
    int error = dir_append_block(dir, &b, &lblk); if (error) return error;
    leaf_init(b->data);
    if (!leaf_insert(b->data, name, len, ino, ftype)) error = -E_NAMETOOLONG;
    leaf_csum(dir, b->data); ext4_bdirty(b); ext4_brelse(b);
    return error;
}
static int dir_remove(struct inode* dir, const struct dloc* loc) {
    struct ext4_buf* b; int error = dir_block(dir, loc->lblk, &b); if (error) return error;
    uint8_t* blk = b->data;
    if (loc->prev != 0xFFFFFFFFu) put16(blk + loc->prev + 4, le16(blk + loc->prev + 4) + le16(blk + loc->off + 4));
    else put32(blk + loc->off, 0);
    leaf_csum(dir, blk); ext4_bdirty(b); ext4_brelse(b);
    return 0;
}
static int dir_set(struct inode* dir, const struct dloc* loc, uint32_t ino, uint8_t ftype) {
    struct ext4_buf* b; int error = dir_block(dir, loc->lblk, &b); if (error) return error;
    put32(b->data + loc->off, ino); b->data[loc->off + 7] = ftype;
    leaf_csum(dir, b->data); ext4_bdirty(b); ext4_brelse(b);
    return 0;
}
static int dir_is_empty(struct inode* dir) {
    uint32_t blocks = (uint32_t)(i_size(dir) / EXT4_BLOCK);
    for (uint32_t lblk = 0; lblk < blocks; lblk++) {
        struct ext4_buf* b; int error = dir_block(dir, lblk, &b);
        if (error == -E_FSCORRUPT) continue;
        if (error) return error;
        uint8_t* blk = b->data; uint32_t off = 0;
        while (off + 8 <= EXT4_BLOCK) {
            uint32_t ino = le32(blk + off), rec = le16(blk + off + 4), nl = blk[off + 6];
            if (rec < 8) break;
            bool dots = (nl == 1 && blk[off + 8] == '.') || (nl == 2 && blk[off + 8] == '.' && blk[off + 9] == '.');
            if (ino && !dots) { ext4_brelse(b); return 0; }
            off += rec;
        }
        ext4_brelse(b);
    }
    return 1;
}
static int set_dotdot(struct inode* dir, uint32_t parent) {
    struct ext4_buf* b; int error = dir_block(dir, 0, &b); if (error) return error;
    uint8_t* blk = b->data;
    if (blk[18] != 2 || blk[20] != '.' || blk[21] != '.') { ext4_brelse(b); return -E_FSCORRUPT; }
    put32(blk + 12, parent);
    if (i_flags(dir) & IFL_INDEX) dx_csum(dir, blk, 32); else leaf_csum(dir, blk);
    ext4_bdirty(b); ext4_brelse(b);
    return 0;
}

/* ------------------------------------------------------------------ path resolution */
static int walk_component(struct inode* dir, const char* name, unsigned len, uint32_t* ino, uint8_t* ftype) {
    if (!is_dir_mode(i_mode(dir))) return -E_NOTDIR;
    struct dentry* d = dc_find(dir->ino, name, len);
    if (d) { if (!d->ino) return -E_NOENT; *ino = d->ino; *ftype = d->ftype; return 0; }
    struct dloc loc; int error = dir_find(dir, name, len, &loc); if (error) return error;
    if (!loc.found) { dc_put(dir->ino, name, len, 0, 0); return -E_NOENT; }
    dc_put(dir->ino, name, len, loc.ino, loc.ftype);
    *ino = loc.ino; *ftype = loc.ftype; return 0;
}
/* Resolve the directory part of `path`; the last component is returned in leaf/leaflen (empty for "/"). */
static int resolve_parent(const char* path, uint32_t* dir_ino, const char** leaf, unsigned* leaflen) {
    uint32_t cur = ROOT_INO; const char* p = path; const char* last = NULL; unsigned lastlen = 0;
    for (;;) {
        while (*p == '/') p++;
        const char* s = p; while (*p && *p != '/') p++;
        unsigned len = (unsigned)(p - s);
        if (!len) break;
        if (len > 255) return -E_NAMETOOLONG;
        const char* q = p; while (*q == '/') q++;
        if (!*q) { last = s; lastlen = len; break; }
        if (len == 1 && s[0] == '.') continue;
        struct inode dir; int error = iget(cur, &dir); if (error) return error;
        uint32_t next; uint8_t ft; error = walk_component(&dir, s, len, &next, &ft); iput(&dir);
        if (error) return error;
        cur = next;
    }
    *dir_ino = cur; *leaf = last; *leaflen = lastlen;
    return 0;
}
static int resolve(const char* path, uint32_t* ino, uint32_t* parent, const char** leaf, unsigned* leaflen) {
    uint32_t dir; const char* name; unsigned len;
    int error = resolve_parent(path, &dir, &name, &len); if (error) return error;
    *parent = dir; *leaf = name; *leaflen = len;
    if (!name || (len == 1 && name[0] == '.')) { *ino = dir; return 0; }
    struct inode d; if ((error = iget(dir, &d))) return error;
    uint8_t ft; error = walk_component(&d, name, len, ino, &ft); iput(&d);
    return error;
}
static bool valid_name(const char* name, unsigned len) {
    if (!name || !len || len > 255) return false;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return false;
    for (unsigned i = 0; i < len; i++) if (!name[i] || name[i] == '/') return false;
    return true;
}

/* ------------------------------------------------------------------ orphan list */
static uint32_t sb_orphan_head(void) {
    struct ext4_buf* b = ext4_bread(0); if (!b) return 0;
    uint32_t v = le32(b->data + 1024 + SB_LAST_ORPHAN); ext4_brelse(b); return v;
}
static int sb_set_orphan_head(uint32_t ino) {
    struct ext4_buf* b = ext4_bread(0); if (!b) return -E_IO;
    put32(b->data + 1024 + SB_LAST_ORPHAN, ino); ext4_bdirty(b); ext4_brelse(b);
    orphan_head_cache = ino; return 0;
}
static bool on_orphan_list(uint32_t ino) {
    uint32_t cur = sb_orphan_head(); unsigned guard = 0;
    while (cur && guard++ < 1000000) {
        if (cur == ino) return true;
        struct inode in; if (iget(cur, &in)) return false;
        cur = le32(in.raw + I_DTIME); iput(&in);
    }
    return false;
}
static int orphan_add(struct inode* in) {
    put32(in->raw + I_DTIME, sb_orphan_head()); idirty(in);
    return sb_set_orphan_head(in->ino);
}
static int orphan_del(struct inode* in) {
    uint32_t next = le32(in->raw + I_DTIME), cur = sb_orphan_head(); unsigned guard = 0;
    if (cur == in->ino) { put32(in->raw + I_DTIME, 0); idirty(in); return sb_set_orphan_head(next); }
    while (cur && guard++ < 1000000) {
        struct inode prev; int error = iget(cur, &prev); if (error) return error;
        uint32_t after = le32(prev.raw + I_DTIME);
        if (after == in->ino) { put32(prev.raw + I_DTIME, next); idirty(&prev); iput(&prev); put32(in->raw + I_DTIME, 0); idirty(in); return 0; }
        iput(&prev); cur = after;
    }
    return 0; /* not on the list */
}

/* ------------------------------------------------------------------ inode allocation */
static int alloc_inode(uint32_t parent, bool dir, uint32_t* out) {
    uint32_t pg = (parent - 1) / ext4.inodes_per_group, start = pg;
    if (dir) { /* spread directories: the group with the most free blocks among those with free inodes */
        uint32_t best_free = 0; bool found = false;
        for (uint32_t k = 0; k < ext4.groups; k++) {
            uint32_t g = (pg + 1 + k) % ext4.groups;
            if (groups[g].free_inodes && (!found || groups[g].free_blocks > best_free)) { best_free = groups[g].free_blocks; start = g; found = true; }
        }
    }
    for (uint32_t k = 0; k < ext4.groups; k++) {
        uint32_t g = (start + k) % ext4.groups; struct group* gr = &groups[g];
        if (!gr->free_inodes) continue;
        struct ext4_buf* b = inode_bitmap(g); if (!b) return -E_IO;
        uint32_t from = g == 0 ? ext4.first_ino - 1 : 0;
        int64_t i = find_zero(b->data, from, ext4.inodes_per_group);
        if (i < 0) { ext4_brelse(b); continue; }
        bit_set(b->data, (uint32_t)i); ext4_bdirty(b); ext4_brelse(b);
        if (gr->flags & BG_INODE_UNINIT) gr->flags &= ~BG_INODE_UNINIT;
        gr->free_inodes--; ext4.free_inodes--; if (dir) gr->used_dirs++;
        if ((uint32_t)i >= ext4.inodes_per_group - gr->itable_unused) gr->itable_unused = ext4.inodes_per_group - (uint32_t)i - 1;
        gr->dirty = true;
        *out = g * ext4.inodes_per_group + (uint32_t)i + 1; return 0;
    }
    return -E_NOSPC;
}
static int free_inode(uint32_t ino, bool dir) {
    uint32_t g = (ino - 1) / ext4.inodes_per_group, i = (ino - 1) % ext4.inodes_per_group;
    struct ext4_buf* b = inode_bitmap(g); if (!b) return -E_IO;
    if (!bit_test(b->data, i)) { ext4_brelse(b); ext4_fail("double free of an inode"); return -E_FSCORRUPT; }
    bit_clear(b->data, i); ext4_bdirty(b); ext4_brelse(b);
    groups[g].free_inodes++; ext4.free_inodes++; if (dir && groups[g].used_dirs) groups[g].used_dirs--;
    groups[g].dirty = true; return 0;
}
/* Initialize a freshly allocated inode in place (the table slot may hold a deleted inode). */
static int init_inode(uint32_t ino, uint32_t mode, uint32_t uid, uint32_t gid, struct inode* in) {
    uint32_t g = (ino - 1) / ext4.inodes_per_group, index = (ino - 1) % ext4.inodes_per_group;
    uint64_t byte = (uint64_t)index * ext4.inode_size;
    struct ext4_buf* b = ext4_bread(groups[g].inode_table + byte / EXT4_BLOCK); if (!b) return -E_IO;
    in->ino = ino; in->buf = b; in->raw = b->data + byte % EXT4_BLOCK;
    memset(in->raw, 0, ext4.inode_size);
    put16(in->raw + I_MODE, mode); put16(in->raw + I_UID, uid); put16(in->raw + I_UID_HI, uid >> 16);
    put16(in->raw + I_GID, gid); put16(in->raw + I_GID_HI, gid >> 16);
    if (ext4.inode_size > 128) put16(in->raw + I_EXTRA_ISIZE, 32);
    next_generation = next_generation * 1103515245u + 12345u;
    put32(in->raw + I_GENERATION, next_generation);
    int64_t now = (int64_t)ext4_now_ns();
    set_time(in, ATIME, now); set_time(in, CTIME, now); set_time(in, MTIME, now);
    if (ext4.inode_size > 128) set_time(in, CRTIME, now);
    uint32_t type = mode & S_IFMT_;
    if ((ext4.incompat & INCOMPAT_EXTENTS) && (type == S_IFREG_ || type == S_IFDIR_ || type == S_IFLNK_)) {
        put32(in->raw + I_FLAGS, IFL_EXTENTS); x_init_root(in);
    }
    return 0;
}

/* ------------------------------------------------------------------ data helpers */
static int write_span(uint64_t pblk, uint32_t inoff, const uint8_t* src, uint64_t bytes, bool fresh) {
    uint64_t blk = pblk, left = bytes; const uint8_t* s = src; int error;
    if (inoff || left < EXT4_BLOCK) {
        uint32_t n = EXT4_BLOCK - inoff; if (n > left) n = (uint32_t)left;
        if (fresh) memset(BOUNCE, 0, EXT4_BLOCK);
        else if ((error = ext4_dev_read(blk, 1, BOUNCE))) return error;
        memcpy(BOUNCE + inoff, s, n);
        if ((error = ext4_dev_write(blk, 1, BOUNCE))) return error;
        blk++; s += n; left -= n;
    }
    uint64_t full = left / EXT4_BLOCK;
    while (full) {
        uint32_t chunk = full > 4096 ? 4096 : (uint32_t)full;
        if ((error = ext4_dev_write(blk, chunk, s))) return error;
        blk += chunk; s += (uint64_t)chunk * EXT4_BLOCK; left -= (uint64_t)chunk * EXT4_BLOCK; full -= chunk;
    }
    if (left) {
        if (fresh) memset(BOUNCE, 0, EXT4_BLOCK);
        else if ((error = ext4_dev_read(blk, 1, BOUNCE))) return error;
        memcpy(BOUNCE, s, left);
        if ((error = ext4_dev_write(blk, 1, BOUNCE))) return error;
    }
    ext4_bforget_range(pblk, (inoff + bytes + EXT4_BLOCK - 1) / EXT4_BLOCK);
    return 0;
}
static uint64_t alloc_goal(struct inode* in, uint32_t lblk) {
    uint64_t pblk; uint32_t run; bool uninit;
    if (lblk && x_map(in, lblk - 1, &pblk, &run, &uninit) == 1) return pblk + 1;
    return group_first((in->ino - 1) / ext4.inodes_per_group);
}
/* Zero bytes [from, to) inside one block of the file if that block holds written data. */
static int zero_partial(struct inode* in, uint64_t from, uint64_t to) {
    if (from >= to) return 0;
    uint64_t pblk; uint32_t run; bool uninit;
    int r = map_block(in, (uint32_t)(from / EXT4_BLOCK), &pblk, &run, &uninit);
    if (r <= 0 || uninit) return r < 0 ? r : 0;
    int error = ext4_dev_read(pblk, 1, BOUNCE); if (error) return error;
    memset(BOUNCE + from % EXT4_BLOCK, 0, to - from);
    return ext4_dev_write(pblk, 1, BOUNCE);
}
/* Free every block of a legacy block-mapped inode (truncate to zero / release). */
static void free_legacy_tree(uint64_t block, unsigned level) {
    if (!block || block >= ext4.blocks) return;
    if (level) {
        struct ext4_buf* b = ext4_bread(block);
        if (b) { for (unsigned i = 0; i < EXT4_BLOCK / 4; i++) free_legacy_tree(le32(b->data + 4 * i), level - 1); ext4_brelse(b); }
    }
    free_blocks(block, 1);
}
static void free_legacy(struct inode* in) {
    uint8_t* p = in->raw + I_BLOCK;
    for (unsigned i = 0; i < 12; i++) free_legacy_tree(le32(p + 4 * i), 0);
    free_legacy_tree(le32(p + 48), 1); free_legacy_tree(le32(p + 52), 2); free_legacy_tree(le32(p + 56), 3);
    memset(p, 0, 60); put32(in->raw + I_BLOCKS_LO, 0); put16(in->raw + I_BLOCKS_HI, 0);
}
static bool fast_symlink(const struct inode* in) {
    uint64_t ea = i_file_acl(in) ? 8 : 0;
    return (i_mode(in) & S_IFMT_) == S_IFLNK_ && !(i_flags(in) & IFL_INLINE_DATA) && i_blocks512(in) == ea;
}
/* Remove all data blocks beyond `first` (in budgeted transactions). */
static int remove_tail(uint32_t ino, uint32_t first) {
    for (;;) {
        int error = ext4_txn_begin(256); if (error) return error;
        struct inode in; if ((error = iget(ino, &in))) { ext4_txn_end(); return error; }
        int more = 0;
        /* callers pass only regular files, directories and slow symlinks */
        if (i_flags(&in) & IFL_EXTENTS) more = x_remove(&in, first, 0x100000000ULL, true, 128);
        else if (!first) free_legacy(&in);
        else if (i_blocks512(&in)) more = -E_OPNOTSUPP;
        idirty(&in); iput(&in); ext4_txn_end();
        if (more <= 0) return more;
    }
}
/* xattr block reference release (images written by Linux may carry them). */
static void release_xattr(struct inode* in) {
    uint64_t acl = i_file_acl(in);
    if (!acl || acl >= ext4.blocks) return;
    struct ext4_buf* b = ext4_bread(acl);
    if (b && le32(b->data) == 0xEA020000u) {
        uint32_t refs = le32(b->data + 4);
        if (refs > 1) {
            put32(b->data + 4, refs - 1);
            if (ext4.csum) { uint8_t le[8]; put64(le, acl); uint32_t c = ext4_crc32c(ext4.csum_seed, le, 8); put32(b->data + 16, 0); put32(b->data + 16, ext4_crc32c(c, b->data, EXT4_BLOCK)); }
            ext4_bdirty(b);
        } else free_blocks(acl, 1);
        add_blocks512(in, -8);
    }
    ext4_brelse(b);
    put32(in->raw + I_FILE_ACL_LO, 0); put16(in->raw + I_FILE_ACL_HI, 0);
}
/* Free an unlinked inode: data, xattr block, orphan entry, bitmap bit. */
static int destroy_inode(uint32_t ino) {
    struct inode in; int error = iget(ino, &in); if (error) return error;
    bool dir = is_dir_mode(i_mode(&in)), extents = i_flags(&in) & IFL_EXTENTS, fast = fast_symlink(&in);
    uint32_t type = i_mode(&in) & S_IFMT_;
    iput(&in);
    if (extents) { if ((error = remove_tail(ino, 0))) return error; }
    else if (!fast && (type == S_IFREG_ || type == S_IFDIR_ || type == S_IFLNK_)) { if ((error = remove_tail(ino, 0))) return error; }
    if ((error = ext4_txn_begin(16))) return error;
    if ((error = iget(ino, &in))) { ext4_txn_end(); return error; }
    release_xattr(&in);
    orphan_del(&in);
    put32(in.raw + I_DTIME, (uint32_t)(ext4_now_ns() / 1000000000ULL)); put16(in.raw + I_LINKS, 0);
    set_size(&in, 0); idirty(&in); iput(&in);
    error = free_inode(ino, dir);
    ext4_txn_end();
    return error;
}

/* ------------------------------------------------------------------ public namespace API */
bool ext4_is_mounted(void) { return ext4.mounted; }
bool ext4_is_readonly(void) { return ext4.readonly; }
uint64_t ext4_mount_pages(void) { return ext4.mount_pages; }

int ext4_lookup(const char* path, fat32_file_t* out) {
    if (!ext4.mounted) return -E_IO;
    uint32_t ino, parent; const char* leaf; unsigned len;
    int error = resolve(path, &ino, &parent, &leaf, &len); if (error) return error;
    struct inode in; if ((error = iget(ino, &in))) return error;
    if (ino == ROOT_INO) fill(out, &in, "/", 1, ROOT_INO);
    else fill(out, &in, leaf, len, parent);
    iput(&in); return 0;
}
int ext4_refresh(fat32_file_t* file) {
    if (!ext4.mounted || !file->ino) return -E_IO;
    struct inode in; int error = iget(file->ino, &in); if (error) return error;
    if (!i_mode(&in)) { iput(&in); return -E_NOENT; }
    fat32_file_t copy; fill(&copy, &in, file->name, (unsigned)strlen(file->name), file->parent);
    iput(&in);
    copy.pending = file->pending; copy.cursor = file->cursor; copy.cursor_index = file->cursor_index;
    *file = copy; return 0;
}
int ext4_read_dir(const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) {
    if (!ext4.mounted) return -E_IO;
    struct inode in; int error = iget(dir->ino, &in); if (error) return error;
    if (!is_dir_mode(i_mode(&in))) { iput(&in); return -E_NOTDIR; }
    uint64_t size = i_size(&in), pos = *cookie;
    while (pos < size) {
        uint32_t lblk = (uint32_t)(pos / EXT4_BLOCK), off = (uint32_t)(pos % EXT4_BLOCK);
        struct ext4_buf* b; error = dir_block(&in, lblk, &b);
        if (error == -E_FSCORRUPT) { pos = (uint64_t)(lblk + 1) * EXT4_BLOCK; continue; }
        if (error) { iput(&in); return error; }
        if ((error = leaf_check(&in, b))) { ext4_brelse(b); iput(&in); return error; }
        uint8_t* blk = b->data; uint32_t o = 0;
        while (o < off) { uint32_t rec = le16(blk + o + 4); if (rec < 8) break; o += rec; }
        while (o + 8 <= EXT4_BLOCK) {
            uint32_t ino = le32(blk + o), rec = le16(blk + o + 4), nl = blk[o + 6]; uint8_t ft = blk[o + 7];
            if (rec < 8) break;
            bool dots = (nl == 1 && blk[o + 8] == '.') || (nl == 2 && blk[o + 8] == '.' && blk[o + 9] == '.');
            if (ino && nl && !dots) {
                memset(out, 0, sizeof(*out));
                memcpy(out->name, blk + o + 8, nl); out->name[nl] = 0;
                unsigned s = nl < 12 ? nl : 12; memcpy(out->short_name, blk + o + 8, s); out->short_name[s] = 0;
                if (!(ext4.incompat & INCOMPAT_FILETYPE) || !ft || ft > 7) {
                    struct inode child; if (!iget(ino, &child)) { ft = mode_ftype(i_mode(&child)); iput(&child); }
                }
                out->ino = out->native_ino = out->first = ino; out->dir_lba = ino == ROOT_INO ? 0 : ino;
                out->mode = ftype_mode(ft); out->is_dir = ft == 2; out->native_kind = mode_kind(out->mode);
                out->parent = in.ino; out->nlink = 1;
                *cookie = (uint64_t)lblk * EXT4_BLOCK + o + rec;
                ext4_brelse(b); iput(&in); return 1;
            }
            o += rec;
        }
        ext4_brelse(b);
        pos = (uint64_t)(lblk + 1) * EXT4_BLOCK;
    }
    *cookie = size; iput(&in); return 0;
}
int ext4_create(const char* path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, const char* link_target, fat32_file_t* out) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    uint32_t pino; const char* name; unsigned len;
    int error = resolve_parent(path, &pino, &name, &len); if (error) return error;
    if (!name) return -E_EXIST; /* "/" */
    if (len > 255) return -E_NAMETOOLONG;
    if (!valid_name(name, len)) return (len == 1 || len == 2) && name[0] == '.' ? -E_EXIST : -E_INVAL;
    uint32_t type = mode & S_IFMT_; bool dir = type == S_IFDIR_;
    uint8_t ftype = mode_ftype(mode); if (!ftype) return -E_INVAL;
    unsigned target_len = link_target ? (unsigned)strlen(link_target) : 0;
    if (type == S_IFLNK_ && (!target_len || target_len >= EXT4_BLOCK)) return target_len ? -E_NAMETOOLONG : -E_NOENT;
    if ((error = ext4_txn_begin(96))) return error;
    struct inode parent; if ((error = iget(pino, &parent))) goto end;
    if (!is_dir_mode(i_mode(&parent))) { error = -E_NOTDIR; goto put_parent; }
    if (!i_links(&parent)) { error = -E_NOENT; goto put_parent; }
    if (dir && i_links(&parent) >= 65000 && !(ext4.ro_compat & RO_DIR_NLINK)) { error = -E_MLINK; goto put_parent; }
    struct dloc loc; if ((error = dir_find(&parent, name, len, &loc))) goto put_parent;
    if (loc.found) { error = -E_EXIST; goto put_parent; }
    uint32_t ino;
    if ((error = alloc_inode(pino, dir, &ino))) goto put_parent;
    struct inode in;
    if ((error = init_inode(ino, mode, uid, gid, &in))) { free_inode(ino, dir); goto put_parent; }
    put16(in.raw + I_LINKS, dir ? 2 : 1);
    if (dir) {
        uint64_t pblk; uint32_t got;
        error = alloc_blocks(groups[(ino - 1) / ext4.inodes_per_group].inode_table, 1, &pblk, &got);
        if (!error) error = x_insert(&in, 0, pblk, 1, false);
        if (!error) {
            struct ext4_buf* b = ext4_bnew(pblk);
            if (!b) error = -E_IO;
            else {
                uint8_t* blk = b->data; memset(blk, 0, EXT4_BLOCK);
                put32(blk, ino); put16(blk + 4, 12); blk[6] = 1; blk[7] = 2; blk[8] = '.';
                put32(blk + 12, pino); put16(blk + 16, dir_leaf_end() - 12); blk[18] = 2; blk[19] = 2; blk[20] = '.'; blk[21] = '.';
                init_tail(blk); leaf_csum(&in, blk); ext4_bdirty(b); ext4_brelse(b);
                add_blocks512(&in, 8); set_size(&in, EXT4_BLOCK);
            }
        }
    } else if (type == S_IFLNK_) {
        if (target_len < 60) {
            put32(in.raw + I_FLAGS, 0); memset(in.raw + I_BLOCK, 0, 60); memcpy(in.raw + I_BLOCK, link_target, target_len);
        } else {
            uint64_t pblk; uint32_t got;
            error = alloc_blocks(group_first((ino - 1) / ext4.inodes_per_group), 1, &pblk, &got);
            if (!error) error = write_span(pblk, 0, (const uint8_t*)link_target, target_len, true);
            if (!error) error = x_insert(&in, 0, pblk, 1, false);
            if (!error) add_blocks512(&in, 8);
        }
        set_size(&in, target_len);
    } else if (type == S_IFCHR_ || type == S_IFBLK_) {
        uint32_t major = (rdev & 0xFFF00) >> 8, minor = (rdev & 0xFF) | ((rdev >> 12) & 0xFFF00);
        if (major < 256 && minor < 256) put32(in.raw + I_BLOCK, rdev); else put32(in.raw + I_BLOCK + 4, rdev);
    }
    if (!error) error = dir_add(&parent, name, len, ino, ftype);
    if (error) {
        put16(in.raw + I_LINKS, 0); put32(in.raw + I_DTIME, (uint32_t)(ext4_now_ns() / 1000000000ULL));
        if (i_flags(&in) & IFL_EXTENTS) x_remove(&in, 0, 0x100000000ULL, true, 4);
        idirty(&in); iput(&in); free_inode(ino, dir); goto put_parent;
    }
    idirty(&in);
    if (dir) { uint32_t links = i_links(&parent); put16(parent.raw + I_LINKS, links >= 64999 ? 1 : links + 1); }
    touch(&parent, true); idirty(&parent);
    dc_put(pino, name, len, ino, ftype);
    if (out) fill(out, &in, name, len, pino);
    iput(&in);
put_parent:
    iput(&parent);
end:
    ext4_txn_end();
    return error;
}
int ext4_link(fat32_file_t* file, const char* path) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    uint32_t pino; const char* name; unsigned len;
    int error = resolve_parent(path, &pino, &name, &len); if (error) return error;
    if (!name) return -E_EXIST;
    if (!valid_name(name, len)) return len > 255 ? -E_NAMETOOLONG : -E_EXIST;
    if ((error = ext4_txn_begin(64))) return error;
    struct inode parent, in;
    if ((error = iget(pino, &parent))) goto end;
    if ((error = iget(file->ino, &in))) goto put_parent;
    if (is_dir_mode(i_mode(&in))) { error = -E_PERM; goto put_both; }
    if (!i_links(&in)) { error = -E_NOENT; goto put_both; }
    if (i_links(&in) >= 65000) { error = -E_MLINK; goto put_both; }
    if (!is_dir_mode(i_mode(&parent)) || !i_links(&parent)) { error = -E_NOENT; goto put_both; }
    struct dloc loc; if ((error = dir_find(&parent, name, len, &loc))) goto put_both;
    if (loc.found) { error = -E_EXIST; goto put_both; }
    uint8_t ft = mode_ftype(i_mode(&in));
    if ((error = dir_add(&parent, name, len, in.ino, ft))) goto put_both;
    put16(in.raw + I_LINKS, i_links(&in) + 1); touch(&in, false); idirty(&in);
    touch(&parent, true); idirty(&parent);
    dc_put(pino, name, len, in.ino, ft);
    fill(file, &in, file->name, (unsigned)strlen(file->name), file->parent);
put_both:
    iput(&in);
put_parent:
    iput(&parent);
end:
    ext4_txn_end(); return error;
}
static int drop_link(struct inode* victim, bool was_dir) {
    uint32_t links = i_links(victim);
    if (was_dir) links = 0; else if (links) links--;
    put16(victim->raw + I_LINKS, links); touch(victim, false);
    if (!links) return orphan_add(victim);
    idirty(victim); return 0;
}
int ext4_unlink(const char* path, int directory, fat32_file_t* victim_out) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    uint32_t pino; const char* name; unsigned len;
    int error = resolve_parent(path, &pino, &name, &len); if (error) return error;
    if (!name) return -E_BUSY;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return -E_INVAL;
    if ((error = ext4_txn_begin(64))) return error;
    struct inode parent, victim; bool have_victim = false;
    if ((error = iget(pino, &parent))) goto end;
    struct dloc loc; if ((error = dir_find(&parent, name, len, &loc))) goto put_parent;
    if (!loc.found) { error = -E_NOENT; goto put_parent; }
    if ((error = iget(loc.ino, &victim))) goto put_parent;
    have_victim = true;
    bool is_dir = is_dir_mode(i_mode(&victim));
    if (directory == 1 && !is_dir) { error = -E_NOTDIR; goto put_parent; }
    if (directory == 0 && is_dir) { error = -E_ISDIR; goto put_parent; }
    if (is_dir) {
        if (victim.ino == ROOT_INO) { error = -E_BUSY; goto put_parent; }
        int empty = dir_is_empty(&victim); if (empty < 0) { error = empty; goto put_parent; }
        if (!empty) { error = -E_NOTEMPTY; goto put_parent; }
    }
    if ((error = dir_remove(&parent, &loc))) goto put_parent;
    if ((error = drop_link(&victim, is_dir))) goto put_parent;
    if (is_dir) {
        uint32_t links = i_links(&parent); if (links > 2 && links != 1) put16(parent.raw + I_LINKS, links - 1);
        dc_purge_dir(victim.ino);
    }
    touch(&parent, true); idirty(&parent);
    dc_put(pino, name, len, 0, 0);
    if (victim_out) fill(victim_out, &victim, name, len, pino);
put_parent:
    if (have_victim) iput(&victim);
    iput(&parent);
end:
    ext4_txn_end(); return error;
}
int ext4_create_unlinked(const char* path, uint32_t mode, uint32_t uid, uint32_t gid, fat32_file_t* out) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    if ((mode & S_IFMT_) != S_IFREG_) return -E_INVAL;
    uint32_t pino; const char* name; unsigned len;
    int error = resolve_parent(path, &pino, &name, &len); if (error) return error;
    if (!name || !valid_name(name, len)) return -E_INVAL;
    if ((error = ext4_txn_begin(32))) return error;
    struct inode parent; if ((error = iget(pino, &parent))) goto end;
    if (!is_dir_mode(i_mode(&parent)) || !i_links(&parent)) { error = -E_NOENT; iput(&parent); goto end; }
    struct dloc loc; error = dir_find(&parent, name, len, &loc); iput(&parent);
    if (error) goto end;
    if (loc.found) { error = -E_EXIST; goto end; }
    uint32_t ino; if ((error = alloc_inode(pino, false, &ino))) goto end;
    struct inode in; if ((error = init_inode(ino, mode, uid, gid, &in))) { free_inode(ino, false); goto end; }
    put16(in.raw + I_LINKS, 0);
    if (!(error = orphan_add(&in))) fill(out, &in, name, len, pino);
    iput(&in);
end:
    ext4_txn_end(); return error;
}
int ext4_link_unlinked(fat32_file_t* file, bool as_symlink) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    unsigned len = (unsigned)strlen(file->name);
    if (!valid_name(file->name, len)) return -E_INVAL;
    int error = ext4_txn_begin(64); if (error) return error;
    struct inode parent, in;
    if ((error = iget(file->parent, &parent))) goto end;
    if ((error = iget(file->ino, &in))) { iput(&parent); goto end; }
    if (i_links(&in) || !is_dir_mode(i_mode(&parent)) || !i_links(&parent)) { error = i_links(&in) ? -E_INVAL : -E_NOENT; goto put; }
    struct dloc loc; if ((error = dir_find(&parent, file->name, len, &loc))) goto put;
    if (loc.found) { error = -E_EXIST; goto put; }
    if (as_symlink) put16(in.raw + I_MODE, S_IFLNK_ | 0777);
    uint8_t ft = mode_ftype(i_mode(&in));
    if ((error = dir_add(&parent, file->name, len, in.ino, ft))) goto put;
    if ((error = orphan_del(&in))) goto put;
    put16(in.raw + I_LINKS, 1); touch(&in, false); idirty(&in);
    touch(&parent, true); idirty(&parent);
    dc_put(parent.ino, file->name, len, in.ino, ft);
    fill(file, &in, file->name, len, parent.ino);
put:
    iput(&in); iput(&parent);
end:
    ext4_txn_end(); return error;
}
int ext4_rename(const char* from, const char* to, bool no_replace, fat32_file_t* moved, fat32_file_t* replaced) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    memset(replaced, 0, sizeof(*replaced));
    uint32_t sdir, tdir; const char *sname, *tname; unsigned slen, tlen;
    int error = resolve_parent(from, &sdir, &sname, &slen); if (error) return error;
    if ((error = resolve_parent(to, &tdir, &tname, &tlen))) return error;
    if (!sname || !tname) return -E_BUSY;
    if (!valid_name(sname, slen) || !valid_name(tname, tlen)) return tlen > 255 ? -E_NAMETOOLONG : -E_INVAL;
    if ((error = ext4_txn_begin(160))) return error;
    struct inode sp, tp, src, dst; bool have_tp = false, have_src = false, have_dst = false;
    if ((error = iget(sdir, &sp))) goto end;
    if (tdir != sdir) { if ((error = iget(tdir, &tp))) goto out; have_tp = true; }
    struct inode* target_parent = have_tp ? &tp : &sp;
    if (!is_dir_mode(i_mode(target_parent)) || !i_links(target_parent)) { error = -E_NOENT; goto out; }
    struct dloc sloc, tloc;
    if ((error = dir_find(&sp, sname, slen, &sloc))) goto out;
    if (!sloc.found) { error = -E_NOENT; goto out; }
    if ((error = dir_find(target_parent, tname, tlen, &tloc))) goto out;
    if ((error = iget(sloc.ino, &src))) goto out;
    have_src = true;
    bool sdirect = is_dir_mode(i_mode(&src));
    if (tloc.found) {
        if (no_replace) { error = -E_EXIST; goto out; }
        if (tloc.ino == sloc.ino) { error = 0; fill(moved, &src, sname, slen, sdir); goto out; }
        if ((error = iget(tloc.ino, &dst))) goto out;
        have_dst = true;
        bool ddir = is_dir_mode(i_mode(&dst));
        if (sdirect && !ddir) { error = -E_NOTDIR; goto out; }
        if (!sdirect && ddir) { error = -E_ISDIR; goto out; }
        if (ddir) { int empty = dir_is_empty(&dst); if (empty < 0) { error = empty; goto out; } if (!empty) { error = -E_NOTEMPTY; goto out; } }
    }
    if (sdirect && tdir != sdir) { /* refuse moving a directory below itself */
        uint32_t cur = tdir; unsigned guard = 0;
        while (cur != ROOT_INO && guard++ < 4096) {
            if (cur == src.ino) { error = -E_INVAL; goto out; }
            struct inode c; if ((error = iget(cur, &c))) goto out;
            uint32_t up; uint8_t ft; error = walk_component(&c, "..", 2, &up, &ft); iput(&c);
            if (error) goto out;
            cur = up;
        }
        if (cur == src.ino) { error = -E_INVAL; goto out; }
    }
    if (sdirect && !tloc.found && tdir != sdir && i_links(target_parent) >= 65000 && !(ext4.ro_compat & RO_DIR_NLINK)) { error = -E_MLINK; goto out; }
    uint8_t ft = mode_ftype(i_mode(&src));
    /* the new name first, then drop the old one (a crash between cannot happen: one transaction) */
    if (tloc.found) error = dir_set(target_parent, &tloc, src.ino, ft);
    else error = dir_add(target_parent, tname, tlen, src.ino, ft);
    if (error) goto out;
    if (!tloc.found && tdir == sdir) { /* the add may have reorganised the directory */
        if ((error = dir_find(&sp, sname, slen, &sloc))) goto out;
        if (!sloc.found) { error = -E_FSCORRUPT; goto out; }
    }
    if ((error = dir_remove(&sp, &sloc))) goto out;
    if (sdirect && tdir != sdir) {
        if ((error = set_dotdot(&src, tdir))) goto out;
        uint32_t l = i_links(&sp); if (l > 2) put16(sp.raw + I_LINKS, l - 1);
        if (!(have_dst && is_dir_mode(i_mode(&dst)))) { l = i_links(target_parent); put16(target_parent->raw + I_LINKS, l >= 64999 ? 1 : l + 1); }
    } else if (sdirect && have_dst && is_dir_mode(i_mode(&dst))) {
        uint32_t l = i_links(target_parent); if (l > 2) put16(target_parent->raw + I_LINKS, l - 1);
    }
    touch(&src, false); idirty(&src);
    touch(&sp, true); idirty(&sp);
    if (have_tp) { touch(&tp, true); idirty(&tp); }
    if (have_dst) {
        bool ddir = is_dir_mode(i_mode(&dst));
        if ((error = drop_link(&dst, ddir))) goto out;
        if (ddir) dc_purge_dir(dst.ino);
        fill(replaced, &dst, tname, tlen, tdir);
    }
    if (sdirect) dc_purge_dir(src.ino);
    dc_put(sdir, sname, slen, 0, 0); dc_put(tdir, tname, tlen, src.ino, ft);
    fill(moved, &src, tname, tlen, tdir);
out:
    if (have_dst) iput(&dst);
    if (have_src) iput(&src);
    if (have_tp) iput(&tp);
    iput(&sp);
end:
    ext4_txn_end(); return error;
}
int ext4_release(fat32_file_t* file) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    struct inode in; int error = iget(file->ino, &in); if (error) return error;
    uint32_t links = i_links(&in), mode = i_mode(&in);
    iput(&in);
    if (!mode) return 0;
    if (links) { /* relinked or never orphaned */
        if (on_orphan_list(file->ino)) {
            if ((error = ext4_txn_begin(8))) return error;
            if (!(error = iget(file->ino, &in))) { orphan_del(&in); iput(&in); }
            ext4_txn_end();
        }
        return error;
    }
    if ((error = destroy_inode(file->ino))) return error;
    file->size = 0; file->first = 0;
    return 0;
}

/* ------------------------------------------------------------------ file data */
int64_t ext4_read(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer) {
    if (!ext4.mounted) return -E_IO;
    struct inode in; int error = iget(file->ino, &in); if (error) return error;
    uint64_t size = i_size(&in);
    if (offset >= size) { iput(&in); return 0; }
    if (length > size - offset) length = size - offset;
    if (length > 0x7FFFFFFFu) length = 0x7FFFFFFFu;
    uint8_t* dst = buffer;
    if (fast_symlink(&in)) { memcpy(dst, in.raw + I_BLOCK + offset, length); iput(&in); return (int64_t)length; }
    if (i_flags(&in) & IFL_INLINE_DATA) { iput(&in); return -E_OPNOTSUPP; }
    uint64_t done = 0;
    while (done < length) {
        uint64_t pos = offset + done; uint32_t lblk = (uint32_t)(pos / EXT4_BLOCK), inoff = (uint32_t)(pos % EXT4_BLOCK);
        uint64_t pblk; uint32_t run; bool uninit;
        int r = map_block(&in, lblk, &pblk, &run, &uninit);
        if (r < 0) { iput(&in); return done ? (int64_t)done : r; }
        uint64_t avail = (uint64_t)run * EXT4_BLOCK - inoff;
        if (avail > length - done) avail = length - done;
        if (r == 0 || uninit) { memset(dst + done, 0, avail); done += avail; continue; }
        if (!inoff && avail >= EXT4_BLOCK) {
            uint64_t blocks = avail / EXT4_BLOCK; if (blocks > 4096) blocks = 4096;
            if ((error = ext4_dev_read(pblk, (uint32_t)blocks, dst + done))) { iput(&in); return done ? (int64_t)done : error; }
            done += blocks * EXT4_BLOCK; continue;
        }
        uint32_t n = EXT4_BLOCK - inoff; if (n > length - done) n = (uint32_t)(length - done);
        if ((error = ext4_dev_read(pblk, 1, BOUNCE))) { iput(&in); return done ? (int64_t)done : error; }
        memcpy(dst + done, BOUNCE + inoff, n); done += n;
    }
    iput(&in); return (int64_t)done;
}
/* swapon: the file's blocks as runs (file byte offset, volume sector, sectors). Holes and inline data are refused
 * (-E_INVAL, as Linux's swapfile activation); unwritten extents are allocated space and count. */
int ext4_file_extents(fat32_file_t* file, int (*emit)(void* context, uint64_t offset, uint64_t sector, uint64_t sectors), void* context) {
    if (!ext4.mounted) return -E_IO;
    struct inode in; int error = iget(file->ino, &in); if (error) return error;
    if ((i_mode(&in) & S_IFMT_) != S_IFREG_ || (i_flags(&in) & IFL_INLINE_DATA)) { iput(&in); return -E_INVAL; }
    uint64_t blocks = (i_size(&in) + EXT4_BLOCK - 1) / EXT4_BLOCK;
    if (blocks > 0xFFFFFFFFULL) { iput(&in); return -E_INVAL; }
    for (uint64_t lblk = 0; lblk < blocks;) {
        uint64_t pblk; uint32_t run; bool uninit;
        int r = map_block(&in, (uint32_t)lblk, &pblk, &run, &uninit);
        if (r < 0) { iput(&in); return r; }
        if (r == 0 || !run) { iput(&in); return -E_INVAL; }   /* a hole */
        if (run > blocks - lblk) run = (uint32_t)(blocks - lblk);
        if ((error = emit(context, lblk * EXT4_BLOCK, pblk * EXT4_SECTORS_PER_BLOCK, (uint64_t)run * EXT4_SECTORS_PER_BLOCK))) { iput(&in); return error; }
        lblk += run;
    }
    iput(&in); return 0;
}
static int ensure_extents(struct inode* in) {
    if (i_flags(in) & IFL_EXTENTS) return 0;
    if (i_flags(in) & IFL_INLINE_DATA) return -E_OPNOTSUPP;
    const uint8_t* p = in->raw + I_BLOCK; bool empty = true;
    for (unsigned i = 0; i < 60; i++) if (p[i]) empty = false;
    if (!empty || i_blocks512(in)) return -E_OPNOTSUPP; /* legacy block maps stay read-only */
    put32(in->raw + I_FLAGS, i_flags(in) | IFL_EXTENTS); x_init_root(in); return 0;
}
int64_t ext4_write(fat32_file_t* file, uint64_t offset, uint64_t length, const void* buffer) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    if (!length) return 0;
    if (offset > EXT4_MAX_FILE_SIZE || length > EXT4_MAX_FILE_SIZE - offset) return -E_FBIG;
    if (length > 0x7FFFFFFFu) length = 0x7FFFFFFFu;
    const uint8_t* src = buffer; uint64_t done = 0; int error = 0;
    while (done < length && !error) {
        if ((error = ext4_txn_begin(320))) break;
        struct inode in; if ((error = iget(file->ino, &in))) { ext4_txn_end(); break; }
        if ((i_mode(&in) & S_IFMT_) != S_IFREG_) error = -E_INVAL;
        else error = ensure_extents(&in);
        for (unsigned steps = 0; !error && steps < 24 && done < length; steps++) {
            uint64_t pos = offset + done, remaining = length - done;
            uint32_t lblk = (uint32_t)(pos / EXT4_BLOCK), inoff = (uint32_t)(pos % EXT4_BLOCK);
            uint64_t pblk; uint32_t run; bool uninit;
            int r = x_map(&in, lblk, &pblk, &run, &uninit);
            if (r < 0) { error = r; break; }
            uint64_t span = (inoff + remaining + EXT4_BLOCK - 1) / EXT4_BLOCK;
            if (span > run) span = run;
            if (span > EXT_INIT_MAX) span = EXT_INIT_MAX;
            if (r == 0) {
                uint32_t got;
                if ((error = alloc_blocks(alloc_goal(&in, lblk), (uint32_t)span, &pblk, &got))) break;
                span = got;
            }
            uint64_t bytes = span * EXT4_BLOCK - inoff; if (bytes > remaining) bytes = remaining;
            uint32_t touched = (uint32_t)((inoff + bytes + EXT4_BLOCK - 1) / EXT4_BLOCK);
            if ((error = write_span(pblk, inoff, src + done, bytes, r == 0 || uninit))) { if (r == 0) free_blocks(pblk, (uint32_t)span); break; }
            if (r == 0) {
                if ((error = x_insert(&in, lblk, pblk, (uint32_t)span, false))) { free_blocks(pblk, (uint32_t)span); break; }
                add_blocks512(&in, (int64_t)span * 8);
            } else if (uninit && (error = x_convert(&in, lblk, touched))) break;
            done += bytes;
            if (offset + done > i_size(&in)) set_size(&in, offset + done);
        }
        idirty(&in);
        file->size = i_size(&in); file->blocks = i_blocks512(&in);
        iput(&in); ext4_txn_end();
    }
    return done ? (int64_t)done : error;
}
int ext4_truncate(fat32_file_t* file, uint64_t size) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    if (size > EXT4_MAX_FILE_SIZE) return -E_FBIG;
    int error = ext4_txn_begin(32); if (error) return error;
    struct inode in; if ((error = iget(file->ino, &in))) { ext4_txn_end(); return error; }
    uint32_t type = i_mode(&in) & S_IFMT_;
    if (type != S_IFREG_) { iput(&in); ext4_txn_end(); return type == S_IFDIR_ ? -E_ISDIR : -E_INVAL; }
    uint64_t old = i_size(&in);
    if (size >= old) {
        if (size > old) { set_size(&in, size); idirty(&in); }
        file->size = size; iput(&in); ext4_txn_end(); return 0;
    }
    if (!(i_flags(&in) & IFL_EXTENTS) && size) { iput(&in); ext4_txn_end(); return -E_OPNOTSUPP; }
    bool add_orphan = !on_orphan_list(in.ino);
    if (add_orphan && (error = orphan_add(&in))) { iput(&in); ext4_txn_end(); return error; }
    set_size(&in, size);
    if (size % EXT4_BLOCK) error = zero_partial(&in, size, (size + EXT4_BLOCK - 1) / EXT4_BLOCK * EXT4_BLOCK);
    idirty(&in); iput(&in); ext4_txn_end();
    if (error) return error;
    if ((error = remove_tail(file->ino, (uint32_t)((size + EXT4_BLOCK - 1) / EXT4_BLOCK)))) return error;
    if (!size) { /* a legacy block-mapped file emptied by truncate becomes extent-mapped */
        if ((error = ext4_txn_begin(8))) return error;
        if (!(error = iget(file->ino, &in))) {
            if (!(i_flags(&in) & IFL_EXTENTS)) { error = ensure_extents(&in); idirty(&in); }
            iput(&in);
        }
        ext4_txn_end(); if (error) return error;
    }
    if (add_orphan) {
        if ((error = ext4_txn_begin(16))) return error;
        if (!(error = iget(file->ino, &in))) { orphan_del(&in); file->blocks = i_blocks512(&in); iput(&in); }
        ext4_txn_end();
    }
    file->size = size;
    return error;
}
static int alloc_unwritten(uint32_t ino, uint32_t first, uint64_t end) {
    uint64_t lblk = first;
    while (lblk < end) {
        int error = ext4_txn_begin(320); if (error) return error;
        struct inode in; if ((error = iget(ino, &in))) { ext4_txn_end(); return error; }
        for (unsigned steps = 0; !error && steps < 24 && lblk < end; steps++) {
            uint64_t pblk; uint32_t run; bool uninit;
            int r = x_map(&in, (uint32_t)lblk, &pblk, &run, &uninit);
            if (r < 0) { error = r; break; }
            uint64_t span = end - lblk; if (span > run) span = run;
            if (r == 1) { lblk += span; continue; }
            if (span > EXT_UNINIT_MAX) span = EXT_UNINIT_MAX;
            uint32_t got;
            if ((error = alloc_blocks(alloc_goal(&in, (uint32_t)lblk), (uint32_t)span, &pblk, &got))) break;
            if ((error = x_insert(&in, (uint32_t)lblk, pblk, got, true))) { free_blocks(pblk, got); break; }
            add_blocks512(&in, (int64_t)got * 8); lblk += got;
        }
        idirty(&in); iput(&in); ext4_txn_end();
        if (error) return error;
    }
    return 0;
}
int ext4_fallocate(fat32_file_t* file, unsigned mode, uint64_t offset, uint64_t length) {
    const unsigned KEEP_SIZE = 1, PUNCH = 2, ZERO = 0x10;
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    if (mode & ~(KEEP_SIZE | PUNCH | ZERO)) return -E_OPNOTSUPP;
    if (((mode & PUNCH) && !(mode & KEEP_SIZE)) || ((mode & PUNCH) && (mode & ZERO))) return -E_INVAL;
    if (!length) return -E_INVAL;
    if (offset > EXT4_MAX_FILE_SIZE || length > EXT4_MAX_FILE_SIZE - offset) return -E_FBIG;
    uint64_t end = offset + length;
    int error = ext4_txn_begin(16); if (error) return error;
    struct inode in; if ((error = iget(file->ino, &in))) { ext4_txn_end(); return error; }
    if ((i_mode(&in) & S_IFMT_) != S_IFREG_) error = is_dir_mode(i_mode(&in)) ? -E_ISDIR : -E_NODATA;
    else error = ensure_extents(&in);
    uint64_t size = i_size(&in);
    uint64_t first_full = (offset + EXT4_BLOCK - 1) / EXT4_BLOCK, last_full = end / EXT4_BLOCK;
    if (!error && (mode & (PUNCH | ZERO))) {
        uint64_t zend = end < size ? end : size;
        if (first_full > last_full) error = zero_partial(&in, offset, zend);
        else {
            error = zero_partial(&in, offset, first_full * EXT4_BLOCK < zend ? first_full * EXT4_BLOCK : zend);
            if (!error && last_full * EXT4_BLOCK < zend) error = zero_partial(&in, last_full * EXT4_BLOCK, zend);
        }
    }
    idirty(&in); iput(&in); ext4_txn_end();
    if (error) return error;
    if ((mode & (PUNCH | ZERO)) && first_full < last_full) {
        for (;;) {
            if ((error = ext4_txn_begin(256))) return error;
            if ((error = iget(file->ino, &in))) { ext4_txn_end(); return error; }
            int more = x_remove(&in, (uint32_t)first_full, last_full, true, 128);
            idirty(&in); iput(&in); ext4_txn_end();
            if (more < 0) return more;
            if (!more) break;
        }
    }
    if (!(mode & PUNCH)) {
        uint64_t from = (mode & ZERO) ? first_full : offset / EXT4_BLOCK, to = (mode & ZERO) ? last_full : (end + EXT4_BLOCK - 1) / EXT4_BLOCK;
        if (from < to && (error = alloc_unwritten(file->ino, (uint32_t)from, to))) return error;
    }
    if ((error = ext4_txn_begin(8))) return error;
    if (!(error = iget(file->ino, &in))) {
        if (!(mode & KEEP_SIZE) && end > i_size(&in)) set_size(&in, end);
        touch(&in, (mode & (PUNCH | ZERO)) != 0); idirty(&in);
        fill(file, &in, file->name, (unsigned)strlen(file->name), file->parent); iput(&in);
    }
    ext4_txn_end();
    return error;
}
int ext4_seek_data(fat32_file_t* file, uint64_t offset, bool hole, uint64_t* result) {
    if (!ext4.mounted) return -E_IO;
    struct inode in; int error = iget(file->ino, &in); if (error) return error;
    uint64_t size = i_size(&in);
    if (offset >= size) { iput(&in); return -6; }
    uint64_t lblk = offset / EXT4_BLOCK;
    while (lblk * EXT4_BLOCK < size) {
        uint64_t pblk; uint32_t run; bool uninit;
        int r = map_block(&in, (uint32_t)lblk, &pblk, &run, &uninit);
        if (r < 0) { iput(&in); return r; }
        bool data = r == 1 && !uninit;
        if (data != hole) { uint64_t at = lblk * EXT4_BLOCK; *result = at > offset ? at : offset; iput(&in); return 0; }
        lblk += run ? run : 1;
    }
    iput(&in);
    if (hole) { *result = size; return 0; }
    return -6;
}
int ext4_setattr(fat32_file_t* file, const struct ext4_attr* attr) {
    if (!ext4.mounted) return -E_IO;
    if (ext4.readonly) return -E_ROFS;
    int error = ext4_txn_begin(8); if (error) return error;
    struct inode in; if ((error = iget(file->ino, &in))) { ext4_txn_end(); return error; }
    if (attr->valid & EXT4_ATTR_MODE) put16(in.raw + I_MODE, (i_mode(&in) & S_IFMT_) | (attr->mode & 07777));
    if (attr->valid & EXT4_ATTR_UID) { put16(in.raw + I_UID, attr->uid); put16(in.raw + I_UID_HI, attr->uid >> 16); }
    if (attr->valid & EXT4_ATTR_GID) { put16(in.raw + I_GID, attr->gid); put16(in.raw + I_GID_HI, attr->gid >> 16); }
    if (attr->valid & EXT4_ATTR_ATIME) set_time(&in, ATIME, attr->atime);
    if (attr->valid & EXT4_ATTR_MTIME) set_time(&in, MTIME, attr->mtime);
    set_time(&in, CTIME, (attr->valid & EXT4_ATTR_CTIME) ? attr->ctime : (int64_t)ext4_now_ns());
    idirty(&in);
    fill(file, &in, file->name, (unsigned)strlen(file->name), file->parent);
    iput(&in); ext4_txn_end(); return 0;
}
int ext4_statfs(struct ext4_volume* v) {
    if (!ext4.mounted) return -E_IO;
    v->block_size = EXT4_BLOCK; v->blocks = ext4.blocks; v->free_blocks = ext4.free_blocks + ext4.pending_free;
    v->files = ext4.inodes; v->free_files = ext4.free_inodes;
    v->fsid[0] = le32(ext4.uuid) ^ le32(ext4.uuid + 8); v->fsid[1] = le32(ext4.uuid + 4) ^ le32(ext4.uuid + 12);
    return 0;
}

/* ------------------------------------------------------------------ commit hooks */
int ext4_prepare_commit(void) {
    int error = apply_pending_frees(); if (error) return error;
    if (!ext4_txn_blocks()) return 0;
    for (uint32_t g = 0; g < ext4.groups; g++) if (groups[g].dirty && (error = encode_group(g))) return error;
    struct ext4_buf* b = ext4_bread(0); if (!b) return -E_IO;
    uint8_t* sb = b->data + 1024;
    put32(sb + SB_FREE_BLOCKS_LO, (uint32_t)ext4.free_blocks); put32(sb + SB_FREE_BLOCKS_HI, (uint32_t)(ext4.free_blocks >> 32));
    put32(sb + SB_FREE_INODES, ext4.free_inodes);
    put32(sb + SB_WTIME, (uint32_t)(ext4_now_ns() / 1000000000ULL));
    put64(sb + SB_KBYTES_WRITTEN, ext4.kbytes_written);
    if (ext4.csum) put32(sb + SB_CHECKSUM, ext4_crc32c(0xFFFFFFFFu, sb, SB_CHECKSUM));
    ext4_bdirty(b); ext4_brelse(b);
    return 0;
}
void ext4_after_commit(void) { quarantine_committed = quarantine_count; }
int ext4_sync(void) {
    if (!ext4.mounted) return 0;
    if (ext4.readonly) return ext4.errors ? -E_IO : 0;
    if (ext4_txn_blocks() || pfree_count) return ext4_txn_commit();
    return ext4_dev_flush();
}
void ext4_poll(void) {
    if (!ext4.mounted || ext4.readonly || ext4_txn_active()) return;
    if (ext4_txn_blocks() && ext4_txn_age_ns() >= 5000000000ULL) ext4_txn_commit();
    else if (!ext4_txn_blocks() && ext4_journal_idle(30000000000ULL)) ext4_checkpoint();
}
uint64_t ext4_journal_idle_at(uint64_t idle_ns);
uint64_t ext4_next_event(uint64_t now) {   /* tick.c: when ext4_poll has work (the 5 s commit, the 30 s checkpoint) */
    if (!ext4.mounted || ext4.readonly) return ~0ULL;
    if (ext4_txn_active()) return now + 1000000ULL;
    if (ext4_txn_blocks()) { uint64_t age = ext4_txn_age_ns(); return age >= 5000000000ULL ? now : now + 5000000000ULL - age; }
    return ext4_journal_idle_at(30000000000ULL);
}
int ext4_unmount_clean(void) {
    if (!ext4.mounted || ext4.readonly || !ext4.dirty_on_disk) return 0;
    int error = ext4_sync(); if (!error) error = ext4_checkpoint();
    if (error) return error;
    struct ext4_buf* b = ext4_bread(0); if (!b) return -E_IO;
    uint8_t* sb = b->data + 1024;
    put32(sb + SB_INCOMPAT, le32(sb + SB_INCOMPAT) & ~INCOMPAT_RECOVER);
    put16(sb + SB_STATE, le16(sb + SB_STATE) | STATE_VALID);
    put32(sb + SB_WTIME, (uint32_t)(ext4_now_ns() / 1000000000ULL));
    if (ext4.csum) put32(sb + SB_CHECKSUM, ext4_crc32c(0xFFFFFFFFu, sb, SB_CHECKSUM));
    error = ext4_dev_write(0, 1, b->data); ext4_brelse(b);
    if (!error) error = ext4_dev_flush();
    if (!error) ext4.dirty_on_disk = false;
    return error;
}

/* ------------------------------------------------------------------ mount */
bool ext4_probe(void) { return ext4_dev_find(); }
static int collect_extents(struct inode* in, uint8_t* node, unsigned depth_left, struct ext4_jextent* out, unsigned* count) {
    if (le16(node) != EXT_MAGIC) return -E_FSCORRUPT;
    for (unsigned i = 0; i < x_entries(node); i++) {
        uint8_t* e = x_entry(node, i);
        if (!x_depth(node)) {
            if (*count >= 32) return -E_FSCORRUPT;
            out[(*count)++] = (struct ext4_jextent){le32(e), x_leaf_len(e), x_leaf_pblk(e)};
        } else {
            if (!depth_left) return -E_FSCORRUPT;
            struct ext4_buf* b = ext4_bread(x_index_child(e)); if (!b) return -E_IO;
            int error = collect_extents(in, b->data, depth_left - 1, out, count); ext4_brelse(b);
            if (error) return error;
        }
    }
    return 0;
}
static int load_super(void) {
    struct ext4_buf* b = ext4_bread(0); if (!b) return -E_IO;
    uint8_t* sb = b->data + 1024; int error = 0;
    if (le16(sb + SB_MAGIC) != 0xEF53 || le32(sb + SB_LOG_BLOCK_SIZE) != 2 || le32(sb + SB_REV_LEVEL) < 1) { error = -E_INVAL; goto done; }
    ext4.compat = le32(sb + SB_COMPAT); ext4.incompat = le32(sb + SB_INCOMPAT); ext4.ro_compat = le32(sb + SB_RO_COMPAT);
    if (ext4.incompat & ~INCOMPAT_SUPPORTED) { ext4_log("[EXT4] unsupported incompatible features ", "", ext4.incompat & ~INCOMPAT_SUPPORTED); error = -E_INVAL; goto done; }
    ext4.csum = ext4.ro_compat & RO_METADATA_CSUM;
    if (ext4.csum && le32(sb + SB_CHECKSUM) != ext4_crc32c(0xFFFFFFFFu, sb, SB_CHECKSUM)) { ext4_log("[EXT4] superblock checksum mismatch", "", 0); error = -E_FSCORRUPT; goto done; }
    ext4.blocks = le32(sb + SB_BLOCKS_LO) | ((ext4.incompat & INCOMPAT_64BIT) ? (uint64_t)le32(sb + SB_BLOCKS_HI) << 32 : 0);
    ext4.inodes = le32(sb + SB_INODES_COUNT);
    ext4.first_data_block = le32(sb + SB_FIRST_DATA_BLOCK);
    ext4.blocks_per_group = le32(sb + SB_BLOCKS_PER_GROUP); ext4.inodes_per_group = le32(sb + SB_INODES_PER_GROUP);
    ext4.inode_size = le16(sb + SB_INODE_SIZE); ext4.first_ino = le32(sb + SB_FIRST_INO);
    ext4.desc_size = (ext4.incompat & INCOMPAT_64BIT) ? le16(sb + SB_DESC_SIZE) : 32;
    ext4.journal_ino = (ext4.compat & COMPAT_HAS_JOURNAL) ? le32(sb + SB_JOURNAL_INUM) : 0;
    memcpy(ext4.uuid, sb + SB_UUID, 16);
    for (int i = 0; i < 4; i++) ext4.hash_seed[i] = le32(sb + SB_HASH_SEED + 4 * i);
    ext4.def_hash = sb[SB_DEF_HASH]; ext4.unsigned_hash = (le32(sb + SB_FLAGS) & 2) != 0;
    ext4.csum_seed = (ext4.incompat & INCOMPAT_CSUM_SEED) ? le32(sb + SB_CHECKSUM_SEED) : ext4_crc32c(0xFFFFFFFFu, ext4.uuid, 16);
    ext4.kbytes_written = le64(sb + SB_KBYTES_WRITTEN);
    if (ext4.first_data_block || ext4.blocks_per_group != EXT4_BLOCK * 8 || !ext4.inodes_per_group ||
        ext4.inodes_per_group > EXT4_BLOCK * 8 || ext4.inode_size < 128 || ext4.inode_size > EXT4_BLOCK ||
        (ext4.inode_size & (ext4.inode_size - 1)) || ext4.desc_size < 32 || ext4.desc_size > 1024 || (ext4.desc_size & (ext4.desc_size - 1)) ||
        ext4.first_ino < 11 || ext4.blocks * EXT4_SECTORS_PER_BLOCK > ext4_dev_sectors()) { error = -E_INVAL; goto done; }
    ext4.groups = (uint32_t)((ext4.blocks - ext4.first_data_block + ext4.blocks_per_group - 1) / ext4.blocks_per_group);
    if ((uint64_t)ext4.groups * ext4.inodes_per_group != ext4.inodes) { error = -E_INVAL; goto done; }
    ext4.gdt_blocks = (uint32_t)(((uint64_t)ext4.groups * ext4.desc_size + EXT4_BLOCK - 1) / EXT4_BLOCK);
    ext4.inodes_per_block = EXT4_BLOCK / ext4.inode_size;
    if (le16(sb + SB_STATE) & STATE_ERROR) { vga_print("[EXT4] volume has errors: mounted read-only until it is checked\n"); ext4.readonly = true; }
    if (ext4.ro_compat & ~RO_SUPPORTED) { ext4_log("[EXT4] unknown read-only features, mounting read-only: ", "", ext4.ro_compat & ~RO_SUPPORTED); ext4.readonly = true; }
    if ((ext4.ro_compat & RO_GDT_CSUM) && !ext4.csum) { vga_print("[EXT4] uninit_bg checksums are not supported: read-only\n"); ext4.readonly = true; }
    { bool ext4_dev_readonly(void); if (ext4_dev_readonly()) { vga_print("[EXT4] read-only device: mounted read-only\n"); ext4.readonly = true; } }   /* a dm-verity root */
done:
    ext4_brelse(b); return error;
}
static int load_groups(bool verify) {
    for (uint32_t g = 0; g < ext4.groups; g++) {
        uint64_t off = (uint64_t)g * ext4.desc_size;
        struct ext4_buf* b = ext4_bread(ext4.first_data_block + 1 + off / EXT4_BLOCK); if (!b) return -E_IO;
        uint8_t* d = b->data + off % EXT4_BLOCK;
        if (verify && ext4.csum && le16(d + 0x1E) != gd_csum(g, d)) {
            ext4_log("[EXT4] group descriptor checksum mismatch, group ", "", g);
            ext4.readonly = true; ext4.errors = true;
        }
        decode_group(g, d); ext4_brelse(b);
        uint64_t itb = ((uint64_t)ext4.inodes_per_group * ext4.inode_size + EXT4_BLOCK - 1) / EXT4_BLOCK;
        if (groups[g].block_bitmap >= ext4.blocks || groups[g].inode_bitmap >= ext4.blocks || groups[g].inode_table + itb > ext4.blocks) return -E_FSCORRUPT;
    }
    return 0;
}
static int recover_orphans(void) {
    unsigned count = 0;
    for (;;) {
        uint32_t head = sb_orphan_head();
        if (!head) break;
        if (head > ext4.inodes || ++count > ext4.inodes) { ext4_fail("corrupt orphan list"); return -E_FSCORRUPT; }
        struct inode in; int error = iget(head, &in); if (error) return error;
        uint32_t links = i_links(&in); uint64_t size = i_size(&in); bool extents = i_flags(&in) & IFL_EXTENTS;
        iput(&in);
        if (!links) { if ((error = destroy_inode(head))) return error; continue; }
        if (extents && (error = remove_tail(head, (uint32_t)((size + EXT4_BLOCK - 1) / EXT4_BLOCK)))) return error;
        if ((error = ext4_txn_begin(8))) return error;
        if (!(error = iget(head, &in))) { orphan_del(&in); iput(&in); }
        ext4_txn_end(); if (error) return error;
    }
    if (count) { ext4_log("[EXT4] orphan inodes processed: ", "", count); return ext4_txn_commit(); }
    return 0;
}
int ext4_mount(void) {
    if (ext4.mounted) return 0;
    if (!ext4_probe()) return -E_NOENT;
    uint64_t keep_pages = ext4.mount_pages;
    memset(&ext4, 0, sizeof(ext4)); ext4.mount_pages = keep_pages;
    ext4.blocks = ext4_dev_sectors() / EXT4_SECTORS_PER_BLOCK;
    if (!scratch) {
        uint64_t pool = memory_total_pages() * 4096;
        uint64_t cache = pool >= (1536ULL << 20) ? (32ULL << 20) : pool >= (512ULL << 20) ? (8ULL << 20) : (4ULL << 20);
        int error = ext4_cache_init(cache); if (error) return error;
        uint64_t extra = SCRATCH_BLOCKS * EXT4_BLOCK + (uint64_t)(PFREE_CAP + QUARANTINE_CAP) * sizeof(struct pending_free) + (uint64_t)DCACHE_SLOTS * sizeof(struct dentry);
        uint64_t base = arena_alloc(extra); if (!base) return -E_NOMEM;
        ext4.mount_pages += (extra + 4095) / 4096;
        scratch = (uint8_t*)base; pfrees = (struct pending_free*)(scratch + SCRATCH_BLOCKS * EXT4_BLOCK);
        quarantine = pfrees + PFREE_CAP;
        dcache = (struct dentry*)(quarantine + QUARANTINE_CAP);
    }
    int error = load_super(); if (error) return error;
    uint64_t gbytes = (uint64_t)ext4.groups * sizeof(struct group);
    groups = (struct group*)arena_alloc(gbytes); if (!groups) return -E_NOMEM;
    ext4.mount_pages += (gbytes + 4095) / 4096;
    if ((error = load_groups(false))) return error;
    if (ext4.journal_ino) {
        struct inode j; if ((error = iget(ext4.journal_ino, &j))) return error;
        struct ext4_jextent extents[32]; unsigned count = 0;
        uint64_t jblocks = i_size(&j) / EXT4_BLOCK;
        error = (i_flags(&j) & IFL_EXTENTS) ? collect_extents(&j, j.raw + I_BLOCK, EXT_MAX_DEPTH, extents, &count) : -E_FSCORRUPT;
        iput(&j);
        if (error) { ext4_log("[EXT4] cannot map the journal", "", 0); return error; }
        bool needs = false;
        { struct ext4_buf* b = ext4_bread(0); if (b) { needs = (le32(b->data + 1024 + SB_INCOMPAT) & INCOMPAT_RECOVER) != 0; ext4_brelse(b); } }
        if ((error = ext4_journal_load_extents(extents, count, jblocks))) { ext4_log("[EXT4] journal load failed ", "", (uint64_t)-error); return error; }
        (void)needs;
        /* replay may have rewritten any metadata block: drop the cache and reload */
        ext4_cache_drop();
        if ((error = load_super())) return error;
    }
    if ((error = load_groups(true))) return error;
    ext4.free_blocks = 0; ext4.free_inodes = 0;
    for (uint32_t g = 0; g < ext4.groups; g++) { ext4.free_blocks += groups[g].free_blocks; ext4.free_inodes += groups[g].free_inodes; }
    native_random(&next_generation, sizeof(next_generation));
    orphan_head_cache = sb_orphan_head();
    ext4.mounted = true;
    if (!ext4.readonly && orphan_head_cache && (error = recover_orphans())) return error;
    char label[17]; struct ext4_buf* b = ext4_bread(0);
    if (b) { memcpy(label, b->data + 1024 + 0x78, 16); label[16] = 0; ext4_brelse(b); } else label[0] = 0;
    vga_print("[OK] EXT4: \""); vga_print(label); vga_print("\" ");
    char number[24]; number64(ext4.blocks * 4, number); vga_print(number); vga_print(" KiB, ");
    number64(ext4.free_blocks * 4, number); vga_print(number); vga_print(" KiB free");
    vga_print(ext4.readonly ? ", read-only\n" : "\n");
    return 0;
}
