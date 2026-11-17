/* ============================================================================
 * NexusOS — FAT32 Filesystem Driver (Header) — Phase 19
 * ============================================================================
 * Read/write FAT32 filesystem on ATA disk.
 * Supports directories, LFN, file create/delete.
 * ============================================================================ */

#ifndef FAT32_H
#define FAT32_H

#include "types.h"
#include "vfs.h"

/* FAT32 limits */
#define FAT32_MAX_OPEN   32
#define FAT32_LFN_MAX    255
#define FAT32_PATH_MAX   512
#define FAT32_NATIVE_SOCKET 1
#define FAT32_NATIVE_SYMLINK 2

/* Initialize FAT32 — reads BPB from disk, parses FAT.
 * Returns true if a valid FAT32 filesystem was found. */
bool fat32_init(void);

/* Get root directory as VFS node */
fs_node_t* fat32_get_root(void);

/* Is FAT32 mounted? */
bool fat32_is_mounted(void);
bool fat32_is_readonly(void);

/* Create a file or directory in the FAT32 root */
int fat32_create_file(const char* name, uint8_t type);

/* Delete a file from the FAT32 root */
int fat32_delete_file(const char* name);

/* Get file count in root directory */
uint32_t fat32_get_file_count(void);
typedef struct { uint32_t cluster_bytes, total_clusters, free_clusters, serial, start_lba; } fat32_space_t;
int fat32_stat_volume(fat32_space_t* space);

/* ---- Phase 57 apps: sub-directories, streamed reads, writes ----
 * Paths are relative to the FAT32 root ("bin/lua"), '/' separated, 8.3 aliases and long UTF-8 names
 * (ASCII case-insensitive). A fat32_file_t is a snapshot of one directory
 * entry (plus where it lives, so writes can update it). */
typedef struct {
    uint32_t first;        /* first cluster (0 = empty file)                  */
#ifdef NEXUS_NATIVE64
    uint64_t size;         /* ext4 roots hold files beyond 4 GiB              */
#else
    uint32_t size;
#endif
    uint32_t cursor, cursor_index; /* sequential I/O cursor */
    uint32_t linear, chain_epoch;  /* verified contiguous prefix, invalidated by FAT changes */
    bool     is_dir;
    uint8_t  native_kind; /* Nexus type marker in NT-reserved byte; zero for ordinary FAT files. */
    bool     read_only;   /* ATTR_READ_ONLY on a regular file (vfat: no write permission bits) */
    uint32_t dir_lba;      /* sector holding the entry (0 = the root itself)  */
    uint32_t dir_off;      /* byte offset of the entry in that sector         */
    uint16_t wdate, wtime; /* FAT write date/time                             */
    char     name[FAT32_LFN_MAX + 1];
    char     short_name[13];
    uint32_t parent, entry_index;
    uint8_t  lfn_count;
    bool     pending;
    bool     detached;    /* live unlinked/replaced object; no directory entry */
    uint64_t native_ino;  /* native VFS identity; never stored in FAT metadata */
    char     final_short[11];
#ifdef NEXUS_NATIVE64
    /* POSIX inode of an ext4 root (zero on FAT32), as of the last lookup/refresh (arch/x86_64/ext4.c). */
    uint32_t ino, mode, uid, gid, nlink, rdev, generation, iflags;
    uint64_t blocks;                      /* 512-byte units                       */
    int64_t  atime, mtime, ctime, btime;  /* nanoseconds since the epoch          */
    uint32_t mount;                       /* 0: the root filesystem, else a mount id (arch/x86_64/mount.h) */
#endif
} fat32_file_t;

bool     fat32_lookup(const char* path, fat32_file_t* out);      /* "" = root */
int      fat32_lookup_status(const char* path, fat32_file_t* out); /* 0, -2 absent, -5 I/O */
int      fat32_refresh_file(fat32_file_t* file);
int32_t  fat32_read_file(fat32_file_t* f, uint32_t off, uint32_t len, void* buf);
bool     fat32_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out);
int      fat32_read_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out); /* 1 entry, 0 EOF, -1 I/O */
/* 0 ok, -1 bad name/io, -2 exists, -3 no parent dir, -4 disk full */
int      fat32_create_path(const char* path, bool is_dir, fat32_file_t* out);
int      fat32_create_socket_path(const char* path, fat32_file_t* out);
int      fat32_finish_symlink(fat32_file_t* file);
int      fat32_write_file(fat32_file_t* f, uint32_t off, uint32_t len, const void* buf);
int      fat32_truncate_file(fat32_file_t* f, uint32_t newsize);
/* 0 ok, -1 not found, -2 is root, -3 directory not empty */
int      fat32_remove_path(const char* path);
/* Serialized namespace mutation, negative errno on failure. The replaced chain
 * stays owned by the caller until fat32_dispose_file after its last live handle. */
int      fat32_rename_path(const char* source, const char* destination, bool no_replace,
                          fat32_file_t* moved, fat32_file_t* replaced);
int      fat32_detach_path(const char* path, fat32_file_t* detached);
int      fat32_dispose_file(fat32_file_t* detached);
/* Stage a download under a temporary short name; publish with one sector write. */
int      fat32_begin_download(const char* path, fat32_file_t* out);
int      fat32_finish_download(fat32_file_t* file);
int      fat32_abort_download(fat32_file_t* file);
uint32_t fat32_time_to_unix(uint16_t date, uint16_t time);
int fat32_set_times(fat32_file_t* file, uint16_t date, uint16_t time);
int fat32_set_read_only(fat32_file_t* file, bool read_only); /* 0 ok, -1 I/O or stale entry */

#ifdef NEXUS_NATIVE64
void fat32_set_volume_cache(void* memory, uint64_t bytes);
void fat32_lookup_cache_init(void); /* boot: allocate the directory lookup tables */
void fat32_unmount_clean(void);     /* orderly power-off: pending FAT, FSInfo, clean flag; no writes afterwards */
/* swapon: the on-disk layout of a regular file as runs of physically contiguous clusters (file byte offset, sector of
 * *part, sectors), in file order. 0, -22 (directory, chain shorter than the size), -5 (I/O), or emit's error. */
struct block_partition;
typedef int (*fat32_extent_fn)(void* context, uint64_t offset, uint64_t sector, uint64_t sectors);
int fat32_file_extents(fat32_file_t* file, const struct block_partition** part, fat32_extent_fn emit, void* context);
#endif
#endif /* FAT32_H */
