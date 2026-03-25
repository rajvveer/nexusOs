/* ============================================================================
 * NexusOS — RAM Filesystem Header
 * ============================================================================ */

#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

#define RAMFS_MAX_FILES     64
#define RAMFS_MAX_FILE_SIZE 4096

/* Initialize ramfs and mount as VFS root */
void ramfs_init(void);

/* File operations */
fs_node_t* ramfs_create(const char* name, uint8_t type);
int        ramfs_delete(const char* name);
uint32_t   ramfs_get_file_count(void);

#endif /* RAMFS_H */
