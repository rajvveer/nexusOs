/* ============================================================================
 * NexusOS — NPFS: NexusOS Persistent File System (Implementation) — Phase 51
 * ============================================================================
 * The 51-phase finale: a real crash-consistent journaling filesystem.
 *
 * Journaling model (write-ahead logging):
 *   Every on-disk mutation goes through a transaction:
 *     1. BEGIN  — gather the (block_lba, block_data) pairs the txn will change.
 *     2. WRITE  — append a journal descriptor + the new block images to the
 *                 journal ring, then a COMMIT record carrying a CRC32 over the
 *                 whole transaction. The commit record's presence + matching
 *                 CRC is what makes the txn "durable".
 *     3. CHECKPOINT — copy the logged blocks to their real homes, then advance
 *                 the journal tail past the committed txn.
 *   If we crash between (2) and (3), `npfs_mount` finds a committed txn whose
 *   blocks were never checkpointed and REPLAYS it. If we crash during (2)
 *   (no/!matching commit), the partial txn is ignored — the FS is unchanged.
 *
 * Everything is integer-only and uses 512-byte block I/O via the ATA driver
 * (primary master). NPFS occupies a reserved high-LBA window and never auto-
 * formats, so the FAT32 region is safe unless the user runs `npfs format`.
 * ============================================================================ */

#include "npfs.h"
#include "ata.h"
#include "vga.h"
#include "string.h"
#include "heap.h"
#include "pkg.h"     /* pkg_crc32 — reuse the table-free CRC32 */

/* ============================================================================
 * On-disk structures (each fits in / is laid over 512-byte blocks)
 * ============================================================================ */

/* Superblock — block 0 of the window. */
typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* NPFS_MAGIC                              */
    uint32_t version;               /* format version                          */
    uint32_t block_size;            /* 512                                     */
    uint32_t total_blocks;          /* size of the window in blocks            */
    uint32_t journal_start;         /* LBA (window-relative) of the journal     */
    uint32_t journal_blocks;        /* journal capacity                         */
    uint32_t inode_start;           /* LBA of the inode table                   */
    uint32_t inode_blocks;          /* inode table size                         */
    uint32_t bitmap_start;          /* LBA of the block bitmap                  */
    uint32_t data_start;            /* LBA of the first data block              */
    uint32_t data_blocks;           /* number of data blocks                    */
    uint32_t inode_count;           /* NPFS_MAX_INODES                          */
    uint32_t next_txid;             /* monotonically increasing txn id          */
    uint32_t mounts;                /* mount counter (proves persistence)       */
    uint32_t sb_crc;               /* CRC32 over the bytes before this field   */
} npfs_super_t;

/* Inode — exactly NPFS_INODE_SIZE (128) bytes so 4 fit per 512 B block. */
typedef struct __attribute__((packed)) {
    uint32_t used;                  /* 0 = free                                */
    uint32_t size;                  /* file size in bytes                       */
    char     name[NPFS_NAME_MAX + 1];   /* 28 bytes incl. NUL                  */
    uint32_t direct[NPFS_DIRECT];   /* 12 direct data-block LBAs (0 = none)     */
    uint32_t indirect;              /* LBA of a single-indirect block (0 none)  */
    uint8_t  reserved[NPFS_INODE_SIZE - (4 + 4 + (NPFS_NAME_MAX + 1)
                       + NPFS_DIRECT * 4 + 4)];  /* pad to 128 bytes           */
} npfs_inode_t;
_Static_assert(sizeof(npfs_inode_t) == NPFS_INODE_SIZE, "npfs_inode_t must be 128 bytes");

#define NPFS_JDESC_MAGIC    0x4A444553u   /* "JDES" */
#define NPFS_JCOMMIT_MAGIC  0x4A434D54u   /* "JCMT" */
#define NPFS_VERSION        1
/* The descriptor is ONE 512 B block: 12 B header + N*4 B LBAs => N <= 125.
 * That caps a single transaction at 125 logged blocks. A max-size file write
 * stages: data + 1 indirect + 1 bitmap + 1 inode, so reserve 3 for overhead.
 * NPFS_MAX_TXN_BLOCKS_HDR sizes the on-disk descriptor array; the staged txn
 * (in RAM) matches it. */
#define NPFS_MAX_TXN_BLOCKS_HDR 125
#define NPFS_MAX_TXN_BLOCKS     NPFS_MAX_TXN_BLOCKS_HDR
#define NPFS_TXN_OVERHEAD       3         /* indirect + bitmap + inode          */
#define NPFS_MAX_WRITE_BLOCKS  (NPFS_MAX_TXN_BLOCKS - NPFS_TXN_OVERHEAD)  /* 122 */

/* Journal descriptor (block 0 of a journal txn). Followed by `count` block
 * images, then a commit record. */
typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* journal-descriptor magic                 */
    uint32_t txid;
    uint32_t count;                 /* number of block images in this txn       */
    uint32_t target_lba[NPFS_MAX_TXN_BLOCKS_HDR]; /* real LBA for each image    */
} npfs_jdesc_t;
_Static_assert(sizeof(npfs_jdesc_t) <= NPFS_BLOCK_SIZE,
               "journal descriptor must fit in one block");

/* Journal commit record. */
typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* commit magic                             */
    uint32_t txid;                  /* must match the descriptor                */
    uint32_t count;
    uint32_t crc;                   /* CRC32 over descriptor + all block images */
} npfs_jcommit_t;

/* ============================================================================
 * State
 * ============================================================================ */
static npfs_super_t sb;
static bool mounted = false;

/* When > 0, the next checkpoint is SKIPPED exactly once — used by crashtest to
 * simulate a crash after commit but before checkpoint. */
static int skip_next_checkpoint = 0;

/* ============================================================================
 * Output helpers
 * ============================================================================ */
static void pr(const char* s)               { vga_print(s); }
static void prc(const char* s, uint8_t col)  { vga_print_color(s, col); }
static void pr_num(uint32_t n) { char b[12]; int_to_str((int)n, b); vga_print(b); }

/* ============================================================================
 * Raw block I/O (window-relative helpers + absolute)
 * ============================================================================ */
static int blk_read(uint32_t abs_lba, void* buf) {
    return ata_read_sectors(abs_lba, 1, buf) == 0 ? NPFS_OK : NPFS_ERR_IO;
}
static int blk_write(uint32_t abs_lba, const void* buf) {
    return ata_write_sectors(abs_lba, 1, buf) == 0 ? NPFS_OK : NPFS_ERR_IO;
}
static uint32_t win(uint32_t rel) { return NPFS_WINDOW_LBA + rel; }

/* ============================================================================
 * Journaled write: log a set of (lba,data) blocks, commit, then checkpoint.
 * ----------------------------------------------------------------------------
 * This is the heart of the FS. All callers that mutate disk state route their
 * block writes through here so a crash can never leave a torn update.
 * ============================================================================ */
typedef struct {
    uint32_t lba[NPFS_MAX_TXN_BLOCKS];          /* absolute target LBAs         */
    uint8_t  data[NPFS_MAX_TXN_BLOCKS][NPFS_BLOCK_SIZE];
    int      count;
} npfs_txn_t;

/* A single shared transaction buffer. At ~64 KB (125 block images) it is far
 * too large for the kernel stack, and the FS is single-threaded/cooperative
 * with no nested operations, so one file-scope instance is correct and safe.
 * journal_replay() uses its own; it never overlaps a commit in progress. */
static npfs_txn_t g_txn;
static npfs_txn_t g_replay_txn;

static void txn_begin(npfs_txn_t* t) { t->count = 0; }

/* Add (or REPLACE) the staged image for a block. Replacing by LBA is essential:
 * a single transaction may touch the same metadata block (e.g. the bitmap)
 * several times, and replay applies images in order — duplicates would waste
 * journal space and the on-disk count would drift. */
static int txn_add(npfs_txn_t* t, uint32_t abs_lba, const void* block) {
    for (int i = 0; i < t->count; i++) {
        if (t->lba[i] == abs_lba) {
            memcpy(t->data[i], block, NPFS_BLOCK_SIZE);
            return NPFS_OK;
        }
    }
    if (t->count >= NPFS_MAX_TXN_BLOCKS) return NPFS_ERR_FULL;
    t->lba[t->count] = abs_lba;
    memcpy(t->data[t->count], block, NPFS_BLOCK_SIZE);
    t->count++;
    return NPFS_OK;
}

/* Return a pointer to the staged image for abs_lba within the txn, or NULL if
 * this block hasn't been staged yet. Lets allocators see prior in-txn updates
 * (the bitmap) instead of the stale on-disk copy. */
static uint8_t* txn_find(npfs_txn_t* t, uint32_t abs_lba) {
    for (int i = 0; i < t->count; i++)
        if (t->lba[i] == abs_lba) return t->data[i];
    return NULL;
}

/* Compute the CRC32 over the descriptor block + every data block image, the
 * same way replay verifies it. */
static uint32_t txn_crc(const npfs_jdesc_t* desc, const npfs_txn_t* t) {
    uint32_t crc = pkg_crc32(desc, sizeof(npfs_jdesc_t));
    /* fold each block's CRC in by chaining over the raw bytes */
    for (int i = 0; i < t->count; i++)
        crc ^= pkg_crc32(t->data[i], NPFS_BLOCK_SIZE);
    return crc;
}

/* Persist + apply a transaction with full WAL semantics. */
static int txn_commit(npfs_txn_t* t) {
    if (t->count == 0) return NPFS_OK;

    uint32_t jbase = win(sb.journal_start);   /* journal region starts here     */
    uint32_t txid  = sb.next_txid;

    /* --- 1. Write the descriptor block. --- */
    npfs_jdesc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.magic = NPFS_JDESC_MAGIC;
    desc.txid  = txid;
    desc.count = (uint32_t)t->count;
    for (int i = 0; i < t->count; i++) desc.target_lba[i] = t->lba[i];

    uint8_t block[NPFS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    memcpy(block, &desc, sizeof(desc));
    if (blk_write(jbase + 0, block) != NPFS_OK) return NPFS_ERR_IO;

    /* --- 2. Write the block images right after the descriptor. --- */
    for (int i = 0; i < t->count; i++)
        if (blk_write(jbase + 1 + (uint32_t)i, t->data[i]) != NPFS_OK)
            return NPFS_ERR_IO;

    /* --- 3. Write the COMMIT record (this is the durability point). --- */
    npfs_jcommit_t commit;
    memset(&commit, 0, sizeof(commit));
    commit.magic = NPFS_JCOMMIT_MAGIC;
    commit.txid  = txid;
    commit.count = (uint32_t)t->count;
    commit.crc   = txn_crc(&desc, t);

    memset(block, 0, sizeof(block));
    memcpy(block, &commit, sizeof(commit));
    if (blk_write(jbase + 1 + (uint32_t)t->count, block) != NPFS_OK)
        return NPFS_ERR_IO;

    /* The transaction is now durable. Bump next_txid in the in-memory super. */
    sb.next_txid = txid + 1;

    /* --- 4. CHECKPOINT: copy logged blocks to their real homes. --- */
    if (skip_next_checkpoint > 0) {
        /* Simulated crash after commit, before checkpoint — leave the journal
         * holding a committed, un-applied txn for replay to find. */
        skip_next_checkpoint--;
        return NPFS_OK;
    }

    for (int i = 0; i < t->count; i++)
        if (blk_write(t->lba[i], t->data[i]) != NPFS_OK)
            return NPFS_ERR_IO;

    /* --- 5. Invalidate the journal (clear the commit record) so we don't
     * replay an already-checkpointed txn. --- */
    memset(block, 0, sizeof(block));
    blk_write(jbase + 1 + (uint32_t)t->count, block);

    return NPFS_OK;
}

/* Replay a committed-but-uncheckpointed transaction found in the journal.
 * Returns 1 if a txn was replayed, 0 if none pending, <0 on error. */
static int journal_replay(void) {
    uint32_t jbase = win(sb.journal_start);

    uint8_t block[NPFS_BLOCK_SIZE];
    if (blk_read(jbase + 0, block) != NPFS_OK) return NPFS_ERR_IO;
    npfs_jdesc_t desc;
    memcpy(&desc, block, sizeof(desc));
    if (desc.magic != NPFS_JDESC_MAGIC) return 0;        /* empty journal       */
    if (desc.count == 0 || desc.count > NPFS_MAX_TXN_BLOCKS) return 0;

    /* Read the commit record. */
    if (blk_read(jbase + 1 + desc.count, block) != NPFS_OK) return NPFS_ERR_IO;
    npfs_jcommit_t commit;
    memcpy(&commit, block, sizeof(commit));
    if (commit.magic != NPFS_JCOMMIT_MAGIC) return 0;    /* not committed       */
    if (commit.txid != desc.txid || commit.count != desc.count) return 0;

    /* Reload the block images and verify the CRC before applying. */
    npfs_txn_t* t = &g_replay_txn;
    txn_begin(t);
    for (uint32_t i = 0; i < desc.count; i++) {
        if (blk_read(jbase + 1 + i, t->data[i]) != NPFS_OK) return NPFS_ERR_IO;
        t->lba[i] = desc.target_lba[i];
        t->count++;
    }
    if (txn_crc(&desc, t) != commit.crc) {
        prc("  [journal] commit CRC mismatch — discarding partial txn\n",
            VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        return 0;
    }

    /* Apply (checkpoint) the logged blocks. */
    for (int i = 0; i < t->count; i++)
        if (blk_write(t->lba[i], t->data[i]) != NPFS_OK) return NPFS_ERR_IO;

    /* Clear the commit record so we don't replay it again. */
    memset(block, 0, sizeof(block));
    blk_write(jbase + 1 + desc.count, block);

    prc("  [journal] replayed txn ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr_num(desc.txid); pr(" ("); pr_num(desc.count); pr(" block(s))\n");
    return 1;
}

/* ============================================================================
 * Inode table helpers
 * ============================================================================ */

/* The inode table is NPFS_MAX_INODES inodes packed into NPFS_INODE_BLOCKS
 * blocks. We read/modify/write whole blocks (and journal the write). */
#define NPFS_INODES_PER_BLOCK (NPFS_BLOCK_SIZE / (int)sizeof(npfs_inode_t))

static int inode_read(int idx, npfs_inode_t* out) {
    uint32_t blk = sb.inode_start + (uint32_t)(idx / NPFS_INODES_PER_BLOCK);
    int off = idx % NPFS_INODES_PER_BLOCK;
    uint8_t buf[NPFS_BLOCK_SIZE];
    if (blk_read(win(blk), buf) != NPFS_OK) return NPFS_ERR_IO;
    memcpy(out, buf + off * (int)sizeof(npfs_inode_t), sizeof(npfs_inode_t));
    return NPFS_OK;
}

/* Stage an inode write into a transaction (read-modify-write the block).
 * Prefer the txn's staged copy of the inode block so multiple inode updates in
 * one transaction compose instead of clobbering each other. */
static int inode_stage(npfs_txn_t* t, int idx, const npfs_inode_t* in) {
    uint32_t blk = sb.inode_start + (uint32_t)(idx / NPFS_INODES_PER_BLOCK);
    int off = idx % NPFS_INODES_PER_BLOCK;
    uint8_t buf[NPFS_BLOCK_SIZE];
    uint8_t* staged = txn_find(t, win(blk));
    if (staged) memcpy(buf, staged, NPFS_BLOCK_SIZE);
    else if (blk_read(win(blk), buf) != NPFS_OK) return NPFS_ERR_IO;
    memcpy(buf + off * (int)sizeof(npfs_inode_t), in, sizeof(npfs_inode_t));
    return txn_add(t, win(blk), buf);
}

static int inode_find(const char* name) {
    npfs_inode_t in;
    for (int i = 0; i < (int)sb.inode_count; i++) {
        if (inode_read(i, &in) != NPFS_OK) return NPFS_ERR_IO;
        if (in.used && strcmp(in.name, name) == 0) return i;
    }
    return NPFS_ERR_NOTFOUND;
}

static int inode_alloc(void) {
    npfs_inode_t in;
    for (int i = 0; i < (int)sb.inode_count; i++) {
        if (inode_read(i, &in) != NPFS_OK) return NPFS_ERR_IO;
        if (!in.used) return i;
    }
    return NPFS_ERR_FULL;
}

/* ============================================================================
 * Block bitmap helpers (one block bitmap; 1 bit per data block)
 * ============================================================================ */
/* Load the bitmap block into bm: prefer the txn's staged copy (so successive
 * allocs within one transaction see each other) and fall back to disk. */
static int bitmap_load(npfs_txn_t* t, uint8_t* bm) {
    uint8_t* staged = txn_find(t, win(sb.bitmap_start));
    if (staged) { memcpy(bm, staged, NPFS_BLOCK_SIZE); return NPFS_OK; }
    return blk_read(win(sb.bitmap_start), bm);
}

static int bitmap_alloc(npfs_txn_t* t, uint32_t* out_lba) {
    uint8_t bm[NPFS_BLOCK_SIZE];
    if (bitmap_load(t, bm) != NPFS_OK) return NPFS_ERR_IO;
    for (uint32_t b = 0; b < sb.data_blocks; b++) {
        if (!(bm[b / 8] & (1 << (b % 8)))) {
            bm[b / 8] |= (uint8_t)(1 << (b % 8));
            int r = txn_add(t, win(sb.bitmap_start), bm);
            if (r != NPFS_OK) return r;
            *out_lba = win(sb.data_start + b);
            return NPFS_OK;
        }
    }
    return NPFS_ERR_FULL;
}

static int bitmap_free_block(npfs_txn_t* t, uint32_t abs_lba) {
    if (abs_lba == 0) return NPFS_OK;
    uint32_t b = abs_lba - win(sb.data_start);
    if (b >= sb.data_blocks) return NPFS_OK;
    uint8_t bm[NPFS_BLOCK_SIZE];
    if (bitmap_load(t, bm) != NPFS_OK) return NPFS_ERR_IO;
    bm[b / 8] &= (uint8_t)~(1 << (b % 8));
    return txn_add(t, win(sb.bitmap_start), bm);
}

/* ============================================================================
 * Superblock persistence
 * ============================================================================ */
static void super_compute_crc(npfs_super_t* s) {
    s->sb_crc = pkg_crc32(s, sizeof(npfs_super_t) - sizeof(uint32_t));
}
static int super_write(void) {
    super_compute_crc(&sb);
    uint8_t buf[NPFS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &sb, sizeof(sb));
    return blk_write(win(0), buf);
}

/* ============================================================================
 * format / mount
 * ============================================================================ */
int npfs_format(void) {
    if (!ata_disk_present()) return NPFS_ERR_NODISK;

    /* Layout the window. */
    uint32_t total = ata_get_size() / NPFS_BLOCK_SIZE;
    if (total <= NPFS_WINDOW_LBA + 128) return NPFS_ERR_FULL;
    uint32_t window_blocks = total - NPFS_WINDOW_LBA;

    memset(&sb, 0, sizeof(sb));
    sb.magic          = NPFS_MAGIC;
    sb.version        = NPFS_VERSION;
    sb.block_size     = NPFS_BLOCK_SIZE;
    sb.total_blocks   = window_blocks;
    sb.journal_start  = 1;
    sb.journal_blocks = NPFS_JOURNAL_BLOCKS;
    sb.inode_start    = sb.journal_start + sb.journal_blocks;
    sb.inode_blocks   = NPFS_INODE_BLOCKS;
    sb.bitmap_start   = sb.inode_start + sb.inode_blocks;
    sb.data_start     = sb.bitmap_start + 1;
    sb.data_blocks    = window_blocks - sb.data_start;
    /* The block bitmap is a single 512-byte block = 4096 bits, so it can only
     * track 4096 data blocks. Cap data_blocks to that — every consumer
     * (bitmap_alloc, npfs_show_stats) iterates [0, data_blocks), so this keeps
     * them inside the one bitmap block. 4096 blocks = 2 MiB of data, ample for
     * 128 inodes capped at 140 blocks each. (A multi-block bitmap is a TODO.) */
    if (sb.data_blocks > NPFS_BLOCK_SIZE * 8)
        sb.data_blocks = NPFS_BLOCK_SIZE * 8;
    sb.inode_count    = NPFS_MAX_INODES;
    sb.next_txid      = 1;
    sb.mounts         = 0;

    /* Zero the journal, inode table, and bitmap (direct writes — there is no
     * filesystem yet to journal against). */
    uint8_t zero[NPFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < sb.journal_blocks; i++)
        if (blk_write(win(sb.journal_start + i), zero) != NPFS_OK) return NPFS_ERR_IO;
    for (uint32_t i = 0; i < sb.inode_blocks; i++)
        if (blk_write(win(sb.inode_start + i), zero) != NPFS_OK) return NPFS_ERR_IO;
    if (blk_write(win(sb.bitmap_start), zero) != NPFS_OK) return NPFS_ERR_IO;

    if (super_write() != NPFS_OK) return NPFS_ERR_IO;

    mounted = true;
    return NPFS_OK;
}

int npfs_mount(void) {
    if (!ata_disk_present()) return NPFS_ERR_NODISK;

    uint8_t buf[NPFS_BLOCK_SIZE];
    if (blk_read(win(0), buf) != NPFS_OK) return NPFS_ERR_IO;
    npfs_super_t cand;
    memcpy(&cand, buf, sizeof(cand));

    if (cand.magic != NPFS_MAGIC) return NPFS_ERR_NOTFOUND;   /* no NPFS here    */
    uint32_t want = pkg_crc32(&cand, sizeof(cand) - sizeof(uint32_t));
    if (want != cand.sb_crc) return NPFS_ERR_CORRUPT;

    sb = cand;

    /* Crash recovery: replay any committed-but-uncheckpointed transaction. */
    journal_replay();

    sb.mounts++;
    super_write();
    mounted = true;
    return NPFS_OK;
}

bool npfs_is_mounted(void) { return mounted; }

/* ============================================================================
 * File operations
 * ============================================================================ */
int npfs_create(const char* name) {
    if (!mounted) return NPFS_ERR_NOTMOUNTED;
    if (!name || !*name || strlen(name) > NPFS_NAME_MAX) return NPFS_ERR_TOOBIG;
    if (inode_find(name) >= 0) return NPFS_ERR_EXISTS;

    int idx = inode_alloc();
    if (idx < 0) return idx;

    npfs_inode_t in;
    memset(&in, 0, sizeof(in));
    in.used = 1;
    in.size = 0;
    strncpy(in.name, name, NPFS_NAME_MAX);
    in.name[NPFS_NAME_MAX] = '\0';

    npfs_txn_t* t = &g_txn; txn_begin(t);
    int r = inode_stage(t, idx, &in);
    if (r != NPFS_OK) return r;
    r = txn_commit(t);
    if (r != NPFS_OK) return r;
    return super_write();
}

/* indirect block helpers: read/allocate the pointer array. */
int npfs_write(const char* name, const uint8_t* data, uint32_t len) {
    if (!mounted) return NPFS_ERR_NOTMOUNTED;
    int idx = inode_find(name);
    if (idx < 0) return idx;

    uint32_t need_blocks = (len + NPFS_BLOCK_SIZE - 1) / NPFS_BLOCK_SIZE;
    /* A single write must fit one journal transaction (see NPFS_MAX_WRITE_BLOCKS
     * and the descriptor/journal sizing). This also bounds it under the inode's
     * direct+indirect capacity. */
    if (need_blocks > NPFS_MAX_WRITE_BLOCKS || need_blocks > NPFS_MAX_FILE_BLOCKS)
        return NPFS_ERR_TOOBIG;

    npfs_inode_t in;
    if (inode_read(idx, &in) != NPFS_OK) return NPFS_ERR_IO;

    npfs_txn_t* t = &g_txn; txn_begin(t);

    /* Free any previously-allocated blocks (simplest correct semantics:
     * overwrite replaces the whole file). */
    for (int i = 0; i < NPFS_DIRECT; i++) {
        if (in.direct[i]) { bitmap_free_block(t, in.direct[i]); in.direct[i] = 0; }
    }
    uint32_t indirect_ptrs[NPFS_PTRS_PER_BLOCK];
    memset(indirect_ptrs, 0, sizeof(indirect_ptrs));
    if (in.indirect) {
        uint8_t ib[NPFS_BLOCK_SIZE];
        if (blk_read(in.indirect, ib) == NPFS_OK) {
            memcpy(indirect_ptrs, ib, sizeof(indirect_ptrs));
            for (int i = 0; i < NPFS_PTRS_PER_BLOCK; i++)
                if (indirect_ptrs[i]) bitmap_free_block(t, indirect_ptrs[i]);
        }
        bitmap_free_block(t, in.indirect);
        in.indirect = 0;
        memset(indirect_ptrs, 0, sizeof(indirect_ptrs));
    }

    /* Allocate + stage each data block. NOTE: the data writes go through the
     * journal too, so a crash mid-write leaves the file's OLD (freed) state —
     * which is consistent because the inode update is in the same txn. */
    uint32_t need_indirect = need_blocks > NPFS_DIRECT ? 1 : 0;
    uint32_t indirect_lba = 0;
    if (need_indirect) {
        if (bitmap_alloc(t, &indirect_lba) != NPFS_OK) return NPFS_ERR_FULL;
    }

    for (uint32_t b = 0; b < need_blocks; b++) {
        uint32_t lba;
        if (bitmap_alloc(t, &lba) != NPFS_OK) return NPFS_ERR_FULL;

        uint8_t blk[NPFS_BLOCK_SIZE];
        memset(blk, 0, sizeof(blk));
        uint32_t chunk = len - b * NPFS_BLOCK_SIZE;
        if (chunk > NPFS_BLOCK_SIZE) chunk = NPFS_BLOCK_SIZE;
        memcpy(blk, data + b * NPFS_BLOCK_SIZE, chunk);
        if (txn_add(t, lba, blk) != NPFS_OK) return NPFS_ERR_FULL;

        if (b < NPFS_DIRECT) in.direct[b] = lba;
        else                 indirect_ptrs[b - NPFS_DIRECT] = lba;
    }

    /* Stage the indirect pointer block, if used. */
    if (need_indirect) {
        uint8_t ib[NPFS_BLOCK_SIZE];
        memset(ib, 0, sizeof(ib));
        memcpy(ib, indirect_ptrs, sizeof(indirect_ptrs));
        if (txn_add(t, indirect_lba, ib) != NPFS_OK) return NPFS_ERR_FULL;
        in.indirect = indirect_lba;
    }

    in.size = len;
    int r = inode_stage(t, idx, &in);
    if (r != NPFS_OK) return r;

    r = txn_commit(t);
    if (r != NPFS_OK) return r;
    return super_write();
}

int npfs_read(const char* name, uint8_t* out, uint32_t max, uint32_t* out_len) {
    if (!mounted) return NPFS_ERR_NOTMOUNTED;
    int idx = inode_find(name);
    if (idx < 0) return idx;

    npfs_inode_t in;
    if (inode_read(idx, &in) != NPFS_OK) return NPFS_ERR_IO;

    uint32_t n = in.size < max ? in.size : max;
    uint32_t need_blocks = (in.size + NPFS_BLOCK_SIZE - 1) / NPFS_BLOCK_SIZE;

    uint32_t indirect_ptrs[NPFS_PTRS_PER_BLOCK];
    memset(indirect_ptrs, 0, sizeof(indirect_ptrs));
    if (in.indirect) {
        uint8_t ib[NPFS_BLOCK_SIZE];
        if (blk_read(in.indirect, ib) != NPFS_OK) return NPFS_ERR_IO;
        memcpy(indirect_ptrs, ib, sizeof(indirect_ptrs));
    }

    uint32_t copied = 0;
    for (uint32_t b = 0; b < need_blocks && copied < n; b++) {
        uint32_t lba = b < NPFS_DIRECT ? in.direct[b]
                                       : indirect_ptrs[b - NPFS_DIRECT];
        if (!lba) break;
        uint8_t blk[NPFS_BLOCK_SIZE];
        if (blk_read(lba, blk) != NPFS_OK) return NPFS_ERR_IO;
        uint32_t chunk = n - copied;
        if (chunk > NPFS_BLOCK_SIZE) chunk = NPFS_BLOCK_SIZE;
        memcpy(out + copied, blk, chunk);
        copied += chunk;
    }

    if (out_len) *out_len = copied;
    return NPFS_OK;
}

int npfs_delete(const char* name) {
    if (!mounted) return NPFS_ERR_NOTMOUNTED;
    int idx = inode_find(name);
    if (idx < 0) return idx;

    npfs_inode_t in;
    if (inode_read(idx, &in) != NPFS_OK) return NPFS_ERR_IO;

    npfs_txn_t* t = &g_txn; txn_begin(t);
    for (int i = 0; i < NPFS_DIRECT; i++)
        if (in.direct[i]) bitmap_free_block(t, in.direct[i]);
    if (in.indirect) {
        uint8_t ib[NPFS_BLOCK_SIZE];
        if (blk_read(in.indirect, ib) == NPFS_OK) {
            uint32_t ptrs[NPFS_PTRS_PER_BLOCK];
            memcpy(ptrs, ib, sizeof(ptrs));
            for (int i = 0; i < NPFS_PTRS_PER_BLOCK; i++)
                if (ptrs[i]) bitmap_free_block(t, ptrs[i]);
        }
        bitmap_free_block(t, in.indirect);
    }

    npfs_inode_t empty;
    memset(&empty, 0, sizeof(empty));
    int r = inode_stage(t, idx, &empty);
    if (r != NPFS_OK) return r;
    r = txn_commit(t);
    if (r != NPFS_OK) return r;
    return super_write();
}

int npfs_size(const char* name, uint32_t* out_size) {
    if (!mounted) return NPFS_ERR_NOTMOUNTED;
    int idx = inode_find(name);
    if (idx < 0) return idx;
    npfs_inode_t in;
    if (inode_read(idx, &in) != NPFS_OK) return NPFS_ERR_IO;
    if (out_size) *out_size = in.size;
    return NPFS_OK;
}

int npfs_file_count(void) {
    if (!mounted) return 0;
    int n = 0;
    npfs_inode_t in;
    for (int i = 0; i < (int)sb.inode_count; i++)
        if (inode_read(i, &in) == NPFS_OK && in.used) n++;
    return n;
}

/* ============================================================================
 * Introspection
 * ============================================================================ */
const char* npfs_strerror(int code) {
    switch (code) {
        case NPFS_OK:              return "ok";
        case NPFS_ERR_NODISK:      return "no disk";
        case NPFS_ERR_NOTFOUND:    return "not found";
        case NPFS_ERR_EXISTS:      return "already exists";
        case NPFS_ERR_FULL:        return "filesystem full";
        case NPFS_ERR_TOOBIG:      return "file too big";
        case NPFS_ERR_IO:          return "disk I/O error";
        case NPFS_ERR_NOTMOUNTED:  return "not mounted (run 'npfs format' or 'npfs mount')";
        case NPFS_ERR_CORRUPT:     return "corrupt superblock";
        default:                   return "unknown error";
    }
}

void npfs_list(void) {
    if (!mounted) { pr("  NPFS not mounted.\n"); return; }
    prc("\n  NPFS files\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  =========\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    npfs_inode_t in;
    int n = 0;
    for (int i = 0; i < (int)sb.inode_count; i++) {
        if (inode_read(i, &in) != NPFS_OK || !in.used) continue;
        pr("    "); pr(in.name);
        int pad = NPFS_NAME_MAX + 2 - (int)strlen(in.name);
        for (int j = 0; j < pad && j < 30; j++) vga_putchar(' ');
        pr_num(in.size); pr(" bytes\n");
        n++;
    }
    if (!n) pr("    (empty)\n");
    pr("\n");
}

void npfs_show_stats(void) {
    if (!mounted) { pr("  NPFS not mounted.\n"); return; }

    /* Count used data blocks from the bitmap. */
    uint8_t bm[NPFS_BLOCK_SIZE];
    uint32_t used = 0;
    if (blk_read(win(sb.bitmap_start), bm) == NPFS_OK)
        for (uint32_t b = 0; b < sb.data_blocks; b++)
            if (bm[b / 8] & (1 << (b % 8))) used++;

    prc("\n  NPFS — NexusOS Persistent File System\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  =====================================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("    Magic/version:  NPFS v"); pr_num(sb.version); pr("\n");
    pr("    Window LBA:     "); pr_num(NPFS_WINDOW_LBA);
    pr("   ("); pr_num(sb.total_blocks); pr(" blocks, ");
    pr_num(sb.total_blocks / 2); pr(" KB)\n");
    pr("    Journal:        LBA+"); pr_num(sb.journal_start);
    pr("  ("); pr_num(sb.journal_blocks); pr(" blocks, write-ahead)\n");
    pr("    Inodes:         "); pr_num((uint32_t)npfs_file_count());
    pr(" / "); pr_num(sb.inode_count); pr(" used\n");
    pr("    Data blocks:    "); pr_num(used); pr(" / ");
    pr_num(sb.data_blocks); pr(" used\n");
    pr("    Max file size:  "); pr_num(NPFS_MAX_FILE_BLOCKS / 2); pr(" KB ");
    pr("("); pr_num(NPFS_DIRECT); pr(" direct + ");
    pr_num(NPFS_PTRS_PER_BLOCK); pr(" indirect blocks)\n");
    pr("    Next txid:      "); pr_num(sb.next_txid); pr("\n");
    pr("    Mount count:    "); pr_num(sb.mounts);
    pr("   (proves cross-reboot persistence)\n\n");
}

void npfs_journal_status(void) {
    if (!mounted) { pr("  NPFS not mounted.\n"); return; }
    uint32_t jbase = win(sb.journal_start);
    uint8_t block[NPFS_BLOCK_SIZE];

    prc("\n  NPFS Journal status\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  ===================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("    Capacity:   "); pr_num(sb.journal_blocks); pr(" blocks\n");
    pr("    Next txid:  "); pr_num(sb.next_txid); pr("\n");

    if (blk_read(jbase, block) == NPFS_OK) {
        npfs_jdesc_t d; memcpy(&d, block, sizeof(d));
        if (d.magic == NPFS_JDESC_MAGIC) {
            if (blk_read(jbase + 1 + d.count, block) == NPFS_OK) {
                npfs_jcommit_t c; memcpy(&c, block, sizeof(c));
                if (c.magic == NPFS_JCOMMIT_MAGIC && c.txid == d.txid) {
                    prc("    Pending:    txn ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
                    pr_num(d.txid); pr(" committed, awaiting replay (");
                    pr_num(d.count); pr(" blocks, CRC ");
                    char hb[12]; hex_to_str(c.crc, hb); pr(hb); pr(")\n");
                } else {
                    pr("    Pending:    none (journal clean)\n");
                }
            }
        } else {
            pr("    Pending:    none (journal clean)\n");
        }
    }
    pr("\n");
}

/* ============================================================================
 * Crash-recovery demonstration
 * ----------------------------------------------------------------------------
 * 1. Write a known file with the checkpoint SKIPPED (simulate a crash after the
 *    journal commit but before the data reaches its home blocks).
 * 2. Confirm the data is NOT yet at its home (read shows stale/empty).
 * 3. Re-mount → journal_replay() applies the committed txn.
 * 4. Read back and confirm the data is now correct.
 * ============================================================================ */
int npfs_crashtest(void) {
    if (!mounted) return NPFS_ERR_NOTMOUNTED;

    const char* fname = "crashtest.txt";
    const char* payload = "NPFS journal recovery: this survived a simulated crash.";
    uint32_t plen = (uint32_t)strlen(payload);

    prc("\n  NPFS crash-recovery test\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  ========================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Clean slate for the test file. */
    npfs_delete(fname);
    int r = npfs_create(fname);
    if (r != NPFS_OK && r != NPFS_ERR_EXISTS) {
        pr("  create failed: "); pr(npfs_strerror(r)); pr("\n");
        return r;
    }

    /* Arm the crash: skip the very next checkpoint. */
    pr("  1. Writing file with checkpoint SKIPPED (simulated crash)...\n");
    skip_next_checkpoint = 1;
    r = npfs_write(fname, (const uint8_t*)payload, plen);
    if (r != NPFS_OK) { pr("  write failed: "); pr(npfs_strerror(r)); pr("\n"); return r; }

    /* The commit is durable in the journal, but the home blocks weren't written
     * and the inode block update was also skipped — so a read sees size 0. */
    uint32_t got = 0;
    uint8_t buf[128];
    npfs_read(fname, buf, sizeof(buf), &got);
    pr("  2. Pre-replay read: "); pr_num(got);
    pr(" bytes (data not yet checkpointed)\n");

    /* Remount: this triggers journal_replay(). */
    pr("  3. Remounting (journal replay)...\n");
    mounted = false;
    r = npfs_mount();
    if (r != NPFS_OK) { pr("  remount failed: "); pr(npfs_strerror(r)); pr("\n"); return r; }

    /* Verify recovery. */
    memset(buf, 0, sizeof(buf));
    got = 0;
    npfs_read(fname, buf, sizeof(buf), &got);
    pr("  4. Post-replay read: "); pr_num(got); pr(" bytes\n");

    if (got == plen && memcmp(buf, payload, plen) == 0) {
        prc("  RESULT: PASS — journal recovered the file intact.\n\n",
            VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        return NPFS_OK;
    }
    prc("  RESULT: FAIL — recovery mismatch.\n\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    return NPFS_ERR_CORRUPT;
}

/* ============================================================================
 * Init — probe the window and mount if a filesystem is already there.
 * ============================================================================ */
void npfs_init(void) {
    if (!ata_disk_present()) {
        vga_print_color("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("NPFS: no disk\n");
        return;
    }
    int r = npfs_mount();
    if (r == NPFS_OK) {
        vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("NPFS mounted (");
        pr_num((uint32_t)npfs_file_count());
        vga_print(" files, mount #");
        pr_num(sb.mounts);
        vga_print(")\n");
    } else {
        vga_print_color("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("NPFS not present — run 'npfs format' to create it\n");
    }
}
