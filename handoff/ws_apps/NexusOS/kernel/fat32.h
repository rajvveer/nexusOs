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

/* Initialize FAT32 — reads BPB from disk, parses FAT.
 * Returns true if a valid FAT32 filesystem was found. */
bool fat32_init(void);

/* Get root directory as VFS node */
fs_node_t* fat32_get_root(void);

/* Is FAT32 mounted? */
bool fat32_is_mounted(void);

/* Create a file or directory in the FAT32 root */
int fat32_create_file(const char* name, uint8_t type);

/* Delete a file from the FAT32 root */
int fat32_delete_file(const char* name);

/* Get file count in root directory */
uint32_t fat32_get_file_count(void);

/* ---- Phase 57 apps: sub-directories, streamed reads, writes ----
 * Paths are relative to the FAT32 root ("bin/lua"), '/' separated, 8.3 names
 * matched case-insensitively. A fat32_file_t is a snapshot of one directory
 * entry (plus where it lives, so writes can update it). */
typedef struct {
    uint32_t first;        /* first cluster (0 = empty file)                  */
    uint32_t size;
    bool     is_dir;
    uint32_t dir_lba;      /* sector holding the entry (0 = the root itself)  */
    uint32_t dir_off;      /* byte offset of the entry in that sector         */
    uint16_t wdate, wtime; /* FAT write date/time                             */
    char     name[13];     /* lower-cased 8.3 name                            */
} fat32_file_t;

bool     fat32_lookup(const char* path, fat32_file_t* out);      /* "" = root */
int32_t  fat32_read_file(const fat32_file_t* f, uint32_t off, uint32_t len, void* buf);
bool     fat32_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out);
/* 0 ok, -1 bad name/io, -2 exists, -3 no parent dir, -4 disk full */
int      fat32_create_path(const char* path, bool is_dir, fat32_file_t* out);
int      fat32_write_file(fat32_file_t* f, uint32_t off, uint32_t len, const void* buf);
int      fat32_truncate_file(fat32_file_t* f, uint32_t newsize);
/* 0 ok, -1 not found, -2 is root, -3 directory not empty */
int      fat32_remove_path(const char* path);
uint32_t fat32_time_to_unix(uint16_t date, uint16_t time);

#endif /* FAT32_H */
