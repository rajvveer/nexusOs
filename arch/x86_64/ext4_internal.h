/* ext4 driver internals shared by ext4.c (filesystem) and ext4_journal.c (block cache, jbd2, device).
 * On-disk structures follow Documentation/filesystems/ext4 (little-endian) and jbd2 (big-endian). */
#ifndef NEXUS_EXT4_INTERNAL_H
#define NEXUS_EXT4_INTERNAL_H
#include "native.h"
#include "ext4.h"

#define EXT4_BLOCK 4096u
#define EXT4_SECTORS_PER_BLOCK 8u

/* errno values (returned negated) */
#define E_PERM 1
#define E_NOENT 2
#define E_IO 5
#define E_NOMEM 12
#define E_BUSY 16
#define E_EXIST 17
#define E_XDEV 18
#define E_NOTDIR 20
#define E_ISDIR 21
#define E_INVAL 22
#define E_FBIG 27
#define E_NOSPC 28
#define E_ROFS 30
#define E_MLINK 31
#define E_NAMETOOLONG 36
#define E_NOTEMPTY 39
#define E_LOOP 40
#define E_NODATA 61
#define E_OPNOTSUPP 95
#define E_FSCORRUPT 117

/* superblock offsets */
#define SB_INODES_COUNT 0x0
#define SB_BLOCKS_LO 0x4
#define SB_FREE_BLOCKS_LO 0xC
#define SB_FREE_INODES 0x10
#define SB_FIRST_DATA_BLOCK 0x14
#define SB_LOG_BLOCK_SIZE 0x18
#define SB_BLOCKS_PER_GROUP 0x20
#define SB_INODES_PER_GROUP 0x28
#define SB_MTIME 0x2C
#define SB_WTIME 0x30
#define SB_MNT_COUNT 0x34
#define SB_MAGIC 0x38
#define SB_STATE 0x3A
#define SB_REV_LEVEL 0x4C
#define SB_FIRST_INO 0x54
#define SB_INODE_SIZE 0x58
#define SB_COMPAT 0x5C
#define SB_INCOMPAT 0x60
#define SB_RO_COMPAT 0x64
#define SB_UUID 0x68
#define SB_JOURNAL_INUM 0xE0
#define SB_LAST_ORPHAN 0xE8
#define SB_HASH_SEED 0xEC
#define SB_DEF_HASH 0xFC
#define SB_DESC_SIZE 0xFE
#define SB_BLOCKS_HI 0x150
#define SB_FREE_BLOCKS_HI 0x158
#define SB_FLAGS 0x160
#define SB_KBYTES_WRITTEN 0x178
#define SB_CHECKSUM_SEED 0x270
#define SB_CHECKSUM 0x3FC

#define COMPAT_HAS_JOURNAL 0x4
#define COMPAT_EXT_ATTR 0x8
#define COMPAT_DIR_INDEX 0x20
#define INCOMPAT_FILETYPE 0x2
#define INCOMPAT_RECOVER 0x4
#define INCOMPAT_EXTENTS 0x40
#define INCOMPAT_64BIT 0x80
#define INCOMPAT_FLEX_BG 0x200
#define INCOMPAT_CSUM_SEED 0x2000
#define INCOMPAT_SUPPORTED (INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG | INCOMPAT_CSUM_SEED)
#define RO_SPARSE_SUPER 0x1
#define RO_LARGE_FILE 0x2
#define RO_HUGE_FILE 0x8
#define RO_GDT_CSUM 0x10
#define RO_DIR_NLINK 0x20
#define RO_EXTRA_ISIZE 0x40
#define RO_METADATA_CSUM 0x400
#define RO_SUPPORTED (RO_SPARSE_SUPER | RO_LARGE_FILE | RO_HUGE_FILE | RO_DIR_NLINK | RO_EXTRA_ISIZE | RO_METADATA_CSUM)
#define STATE_VALID 1
#define STATE_ERROR 2

/* group descriptor flags */
#define BG_INODE_UNINIT 0x1
#define BG_BLOCK_UNINIT 0x2
#define BG_INODE_ZEROED 0x4

/* inode offsets */
#define I_MODE 0x0
#define I_UID 0x2
#define I_SIZE_LO 0x4
#define I_ATIME 0x8
#define I_CTIME 0xC
#define I_MTIME 0x10
#define I_DTIME 0x14
#define I_GID 0x18
#define I_LINKS 0x1A
#define I_BLOCKS_LO 0x1C
#define I_FLAGS 0x20
#define I_BLOCK 0x28
#define I_GENERATION 0x64
#define I_FILE_ACL_LO 0x68
#define I_SIZE_HI 0x6C
#define I_BLOCKS_HI 0x74
#define I_FILE_ACL_HI 0x76
#define I_UID_HI 0x78
#define I_GID_HI 0x7A
#define I_CSUM_LO 0x7C
#define I_EXTRA_ISIZE 0x80
#define I_CSUM_HI 0x82
#define I_CTIME_EXTRA 0x84
#define I_MTIME_EXTRA 0x88
#define I_ATIME_EXTRA 0x8C
#define I_CRTIME 0x90
#define I_CRTIME_EXTRA 0x94

#define IFL_INDEX 0x1000
#define IFL_HUGE_FILE 0x40000
#define IFL_EXTENTS 0x80000
#define IFL_EA_INODE 0x200000
#define IFL_INLINE_DATA 0x10000000

#define S_IFMT_ 0170000
#define S_IFSOCK_ 0140000
#define S_IFLNK_ 0120000
#define S_IFREG_ 0100000
#define S_IFBLK_ 0060000
#define S_IFDIR_ 0040000
#define S_IFCHR_ 0020000
#define S_IFIFO_ 0010000

#define EXT_MAGIC 0xF30A
#define EXT_INIT_MAX 32768u
#define EXT_UNINIT_MAX 32767u
#define EXT_MAX_DEPTH 5

static inline uint16_t le16(const void* p) { const uint8_t* b = p; return (uint16_t)(b[0] | b[1] << 8); }
static inline uint32_t le32(const void* p) { const uint8_t* b = p; return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static inline uint64_t le64(const void* p) { return le32(p) | (uint64_t)le32((const uint8_t*)p + 4) << 32; }
static inline void put16(void* p, uint32_t v) { uint8_t* b = p; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static inline void put32(void* p, uint32_t v) { uint8_t* b = p; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static inline void put64(void* p, uint64_t v) { put32(p, (uint32_t)v); put32((uint8_t*)p + 4, (uint32_t)(v >> 32)); }
static inline uint32_t be32(const void* p) { const uint8_t* b = p; return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static inline void putbe32(void* p, uint32_t v) { uint8_t* b = p; b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }

/* overlapping copy (the kernel's string.h has no memmove) */
static inline void ext4_move(void* dst, const void* src, uint64_t n) {
    uint8_t* d = dst; const uint8_t* s = src;
    if (d == s || !n) return;
    if (d < s) for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    else for (uint64_t i = n; i--; ) d[i] = s[i];
}

/* ---- crc32c (raw: no pre/post inversion, as Linux crc32c_le / ext4_chksum) ---- */
uint32_t ext4_crc32c(uint32_t crc, const void* data, uint64_t length);

/* ---- block device seam (ext4_dev.c): blockdev.h partitions when present, else kernel/ata.c ---- */
int ext4_dev_read(uint64_t block, uint32_t count, void* buffer);
int ext4_dev_write(uint64_t block, uint32_t count, const void* buffer);
int ext4_dev_flush(void);
bool ext4_dev_find(void);            /* locate the ext4 volume (ext4_dev.c) */
uint64_t ext4_dev_sectors(void);

/* ---- metadata block cache ---- */
struct ext4_buf {
    uint64_t block;
    uint8_t* data;
    struct ext4_buf *hash_next, *lru_prev, *lru_next, *txn_next;
    uint16_t pins;
    uint8_t valid, txn, ckpt;   /* ckpt: committed in the journal, home location not yet written */
    uint32_t ckpt_slot;
};
int ext4_cache_init(uint64_t bytes);                /* mount: allocates the permanent cache */
struct ext4_buf* ext4_bread(uint64_t block);         /* pinned, contents read; NULL on I/O error */
struct ext4_buf* ext4_bnew(uint64_t block);          /* pinned, zeroed, not read (new metadata) */
void ext4_brelse(struct ext4_buf* buffer);
void ext4_bdirty(struct ext4_buf* buffer);           /* join the running transaction */
void ext4_bforget_range(uint64_t block, uint64_t count); /* data written directly: drop clean copies */
unsigned ext4_cache_free_buffers(void);
void ext4_cache_drop(void);                          /* forget clean blocks (after journal replay) */

/* ---- jbd2 transactions (ordered metadata journaling) ---- */
struct ext4_jextent { uint32_t lblk, len; uint64_t pblk; };
/* mount: map the journal inode's blocks, replay committed transactions if the log is not empty */
int ext4_journal_load_extents(const struct ext4_jextent* extents, unsigned count, uint64_t size_blocks);
bool ext4_journal_present(void);
int ext4_txn_begin(unsigned credits);                /* may commit the running transaction first */
void ext4_txn_end(void);
int ext4_txn_commit(void);                           /* force: journal write, flush, commit block, flush */
int ext4_checkpoint(void);                           /* write committed blocks home, flush, empty the log */
bool ext4_journal_idle(uint64_t idle_ns);            /* log holds transactions committed >= idle_ns ago */
unsigned ext4_txn_blocks(void);
uint64_t ext4_txn_age_ns(void);
bool ext4_txn_active(void);                          /* inside begin/end */

/* ---- filesystem state used by the journal ---- */
struct ext4_fs {
    bool mounted, readonly, csum, dirty_on_disk, journal_dirty, errors;
    uint32_t start_lba;
    uint64_t blocks, free_blocks, pending_free;
    uint32_t inodes, free_inodes, inodes_per_group, blocks_per_group, groups, inode_size, desc_size, first_ino;
    uint32_t inodes_per_block, gdt_blocks, first_data_block;
    uint32_t compat, incompat, ro_compat;
    uint32_t csum_seed, hash_seed[4];
    uint8_t def_hash, uuid[16];
    bool unsigned_hash;
    uint32_t journal_ino;
    uint64_t mount_pages;
    uint64_t kbytes_written;
};
extern struct ext4_fs ext4;
/* Called by the journal right before a transaction is written: fold allocator state into metadata. */
int ext4_prepare_commit(void);
void ext4_after_commit(void);
void ext4_after_checkpoint(void);                    /* quarantined frees become allocatable */
bool ext4_quarantine_full(void);
void ext4_fail(const char* why);                     /* fatal metadata error: volume becomes read-only */
void ext4_log(const char* a, const char* b, uint64_t value);
uint64_t ext4_now_ns(void);
#endif
