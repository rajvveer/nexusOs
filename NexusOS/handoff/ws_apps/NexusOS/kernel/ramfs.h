/* ============================================================================
 * NexusOS — RAM Filesystem Header
 * ============================================================================ */

#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

#define RAMFS_MAX_FILES     96
#define RAMFS_MAX_FILE_SIZE 4096

/* Initialize ramfs and mount as VFS root */
void ramfs_init(void);

/* File operations (legacy, root-scoped; kept for existing flat-root callers) */
fs_node_t* ramfs_create(const char* name, uint8_t type);
int        ramfs_delete(const char* name);
int        ramfs_rename(const char* old_name, const char* new_name); /* 0 / -1 / -2 EPERM */
uint32_t   ramfs_get_file_count(void);

/* ---- Phase 54: real directories + path resolution ----
 * A directory is an FS_DIRECTORY node whose children/next list holds its
 * entries (the fs_node tree in vfs.h). Paths may be absolute ("/a/b") or
 * relative to `cwd` (NULL == root), and understand "." and "..". */

/* Create a node `name` inside directory `parent`. Returns the node or NULL
 * (parent not a dir / duplicate / table full). Sets owner+mode. */
fs_node_t* ramfs_mknode(fs_node_t* parent, const char* name, uint8_t type);

/* Resolve a path to a node, or NULL if any component is missing. */
fs_node_t* ramfs_resolve(const char* path, fs_node_t* cwd);

/* Resolve the PARENT directory of `path` and copy the final component into
 * `leaf` (>= FS_NAME_MAX). Returns the parent dir node (or NULL). */
fs_node_t* ramfs_resolve_parent(const char* path, fs_node_t* cwd, char* leaf);

/* Directory ops. Return codes: 0 ok, -1 not found/exists/bad name,
 * -2 permission denied, -3 no/ not-a-dir parent (mkdir) or not empty (rmdir),
 * -4 not a directory / is a directory. */
int ramfs_mkdir(const char* path, fs_node_t* cwd);
int ramfs_rmdir(const char* path, fs_node_t* cwd);
int ramfs_rm_path(const char* path, fs_node_t* cwd);   /* remove a file by path */

/* Build the absolute path of `node` into out[sz] (e.g. "/home/root"). */
void ramfs_abspath(fs_node_t* node, char* out, int sz);

#endif /* RAMFS_H */
