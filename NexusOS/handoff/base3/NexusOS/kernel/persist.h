/* ============================================================================
 * NexusOS — Persistent RAM filesystem (Header) — Phase 55
 * ============================================================================
 * Crash-safe snapshot of the whole ramfs tree (files, directories, owners,
 * modes) plus the user-account table, stored in a reserved window of the IDE
 * disk and restored at boot, so edits survive a reboot.
 *
 * On-disk layout (512-byte sectors, via the blkdev layer — default `hda`):
 *
 *   LBA 26624 .. 28671   slot A   (PERSIST_SLOT_SECTORS = 2048 = 1 MiB)
 *   LBA 28672 .. 30719   slot B
 *
 *   slot = [ header sector ] [ payload sectors ... ]
 *   header  = { magic "NXRS", version, generation, node_count, payload_bytes,
 *               payload_crc32, save_tick, tomb_count, header_crc32 }
 *   payload = tombstone records, then node records in parent-before-child order
 *             { parent index (0xFFFFFFFF = child of "/"), type, mode, uid,
 *               gid, size, name[32] } + file data (padded to 4 bytes).
 *   A tombstone (type 0x80) carries the absolute path of a built-in file the
 *   user deleted, so `rm readme.txt` sticks across reboots.
 *
 * Crash safety: a save always writes the OLDER slot, payload first, header
 * LAST (ata_write_sectors flushes the drive cache after every call), so a
 * crash mid-save leaves the other slot intact. Restore reads both headers,
 * verifies both CRCs (header + full payload) and takes the valid slot with
 * the higher generation.
 *
 * What is saved: every node that is not an unmodified built-in. Boot-time
 * init code (ramfs/image/audio/script/pkg/uproc inits) pre-populates demo
 * files; those are stamped FS_FLAG_BUILTIN and skipped unless the user later
 * writes/chmods/chowns/renames them (the flag is cleared on the touched node).
 * The directory chain needed to reach a saved node is always included.
 * ============================================================================ */

#ifndef PERSIST_H
#define PERSIST_H

#include "types.h"
#include "vfs.h"
#include "npfs.h"

/* ---- Geometry ------------------------------------------------------------ */
#define PERSIST_LBA            26624u          /* 13 MiB into the disk          */
#define PERSIST_SLOT_SECTORS   2048u           /* 1 MiB per slot                */
#define PERSIST_SLOTS          2u              /* A / B                         */
#define PERSIST_END_LBA        (PERSIST_LBA + PERSIST_SLOTS * PERSIST_SLOT_SECTORS)  /* 30720 */
#define PERSIST_MAX_PAYLOAD    ((PERSIST_SLOT_SECTORS - 1u) * 512u)

/* The last LBA NPFS can ever touch + 1: superblock + journal + inode table +
 * bitmap + the 4096 data blocks a single-block bitmap can address (npfs.c
 * caps data_blocks at NPFS_BLOCK_SIZE*8). The persist window must start
 * past it — checked with a _Static_assert in persist.c. */
#define NPFS_MAX_END_LBA  (NPFS_WINDOW_LBA + 1u + NPFS_JOURNAL_BLOCKS + \
                           NPFS_INODE_BLOCKS + 1u + (NPFS_BLOCK_SIZE * 8u))   /* 24802 */

#define PERSIST_MAGIC          0x5352584Eu     /* "NXRS" as little-endian u32   */
#define PERSIST_VERSION        1u
#define PERSIST_DEBOUNCE_TICKS 36u             /* 2 s at 18.2 Hz                */
#define PERSIST_MAX_TOMBS      16              /* deleted built-ins remembered  */

/* ---- Boot sequence (kernel.c) --------------------------------------------
 * persist_mark_builtins : after the last _init — stamp every node BUILTIN.
 * persist_init          : probe the disk window and restore the newest valid
 *                         snapshot (must run BEFORE login_run so restored
 *                         accounts can log in).
 * persist_boot_done     : after uproc_init installed the demo binaries —
 *                         ends the built-in phase, re-applies tombstones. */
void persist_mark_builtins(void);
void persist_init(void);
void persist_boot_done(void);

/* ---- Save / restore ------------------------------------------------------ */
int  persist_save_ex(bool verbose);    /* returns records saved, <0 on failure */
int  persist_restore(void);            /* 0 ok, -1 io/disabled, -2 no snapshot  */
void persist_flush(void);              /* save if dirty (reboot/shutdown path)  */
void persist_poll(void);               /* debounced autosave — idle pumps only  */

/* ---- Introspection / control (shell `persist`) --------------------------- */
void persist_status(void);
bool persist_is_dirty(void);
bool persist_available(void);
bool persist_autosave(void);
void persist_set_autosave(bool on);
int  persist_set_device(const char* blkdev_name);   /* 0 ok, <0 not found/too small */

/* ---- Mutation hooks (called by ramfs.c / vfs.c / users.c) ---------------- */
void persist_note_create(fs_node_t* n);   /* node just created               */
void persist_note_write(fs_node_t* n);    /* data written                    */
void persist_note_meta(fs_node_t* n);     /* chmod / chown                   */
void persist_note_unlink(fs_node_t* n);   /* about to be removed (before free)*/
void persist_note_rename(fs_node_t* n);   /* about to be renamed (old name)  */
void persist_note_users(void);            /* account table changed           */

#endif /* PERSIST_H */
