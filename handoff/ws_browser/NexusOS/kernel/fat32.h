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

#endif /* FAT32_H */
