/* ============================================================================
 * NexusOS — /proc Filesystem Header — Phase 31
 * ============================================================================
 * Virtual filesystem exposing kernel state as readable files.
 * ============================================================================ */

#ifndef PROCFS_H
#define PROCFS_H

#include "types.h"

/* Maximum /proc file content size */
#define PROCFS_MAX_SIZE 512

/* Initialize procfs */
void procfs_init(void);

/* Read a /proc path into buffer. Returns bytes written, or -1 if not found. */
int procfs_read(const char* path, char* buf, int max);

/* Check if a path is a /proc path */
bool procfs_is_proc(const char* path);

#endif /* PROCFS_H */
