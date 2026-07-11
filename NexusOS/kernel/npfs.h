/* ============================================================================
 * NexusOS — NPFS: NexusOS Persistent File System (Header) — Phase 51 (FINALE)
 * ============================================================================
 * A from-scratch, crash-consistent journaling filesystem. The headline feature
 * is a real **write-ahead journal**: every metadata/data update is written to
 * an on-disk journal (with a CRC32-checksummed commit record) BEFORE it is
 * applied to its final location, and `npfs_mount` REPLAYS any committed-but-
 * uncheckpointed transactions on startup. A crash between commit and checkpoint
 * therefore recovers cleanly (proven by `npfs crashtest`).
 *
 * It lives in a reserved high-LBA window of the primary-master IDE disk (the
 * same disk ata.c drives). It NEVER auto-formats: `npfs_mount` only succeeds if
 * a valid NPFS superblock is already present, so it cannot clobber the FAT32
 * region unless the user explicitly runs `npfs format`. Persists across reboot.
 *
 * On-disk layout within the window (block = 512 B sector):
 *   [ superblock x1 ] [ journal x NPFS_JOURNAL_BLOCKS ]
 *   [ inode table x NPFS_INODE_BLOCKS ] [ block bitmap x1 ] [ data blocks... ]
 *
 * Large-file support: each inode has 12 direct block pointers + 1 single
 * indirect block (128 pointers), so a file can span 12 + 128 = 140 blocks
 * (70 KB at 512 B/block) — small by design for this teaching FS, but the
 * direct+indirect structure is the real ext2-style scheme, not a single chain.
 * ============================================================================ */

#ifndef NPFS_H
#define NPFS_H

#include "types.h"

/* ---- Geometry (all units are 512-byte blocks unless noted) ----------------*/
#define NPFS_BLOCK_SIZE      512
#define NPFS_WINDOW_LBA      20480u    /* start LBA on the IDE disk (10 MiB in) */
#define NPFS_MAGIC           0x4E504653u /* "NPFS" little-endian-ish tag        */

#define NPFS_JOURNAL_BLOCKS  192        /* journal capacity: 1 desc + <=160 +1  */
#define NPFS_MAX_INODES      128        /* fixed inode table                    */
#define NPFS_INODE_SIZE      128        /* bytes per inode (4 per 512 B block)  */
#define NPFS_INODE_BLOCKS    32         /* 128 inodes * 128 B = 16 KiB = 32 blk */
#define NPFS_DIRECT          12         /* direct block pointers per inode      */
#define NPFS_PTRS_PER_BLOCK  (NPFS_BLOCK_SIZE / 4)   /* 128 indirect pointers   */
#define NPFS_MAX_FILE_BLOCKS (NPFS_DIRECT + NPFS_PTRS_PER_BLOCK)  /* 140        */
#define NPFS_NAME_MAX        27         /* filename length in a dir entry       */

/* ---- Result codes ---------------------------------------------------------*/
#define NPFS_OK              0
#define NPFS_ERR_NODISK     -1
#define NPFS_ERR_NOTFOUND   -2
#define NPFS_ERR_EXISTS     -3
#define NPFS_ERR_FULL       -4
#define NPFS_ERR_TOOBIG     -5
#define NPFS_ERR_IO         -6
#define NPFS_ERR_NOTMOUNTED -7
#define NPFS_ERR_CORRUPT    -8

/* ---- Public API -----------------------------------------------------------*/
void npfs_init(void);                   /* probe the window, mount if present  */

int  npfs_format(void);                 /* lay down a fresh filesystem         */
int  npfs_mount(void);                  /* read superblock + replay journal    */
bool npfs_is_mounted(void);

/* File operations (root-directory flat namespace — directories are a TODO). */
int  npfs_create(const char* name);
int  npfs_write(const char* name, const uint8_t* data, uint32_t len);
int  npfs_read(const char* name, uint8_t* out, uint32_t max, uint32_t* out_len);
int  npfs_delete(const char* name);
int  npfs_size(const char* name, uint32_t* out_size);

/* Introspection. */
int  npfs_file_count(void);
const char* npfs_strerror(int code);
void npfs_show_stats(void);             /* superblock + usage report           */
void npfs_list(void);                   /* directory listing                   */
void npfs_journal_status(void);         /* journal head/tail + last txid + CRC  */

/* Crash-recovery demonstration: write a file but SKIP the checkpoint (simulate
 * a crash after commit), then remount and prove the journal replay restored it.
 * Returns NPFS_OK if the replayed data matched. */
int  npfs_crashtest(void);

#endif /* NPFS_H */
