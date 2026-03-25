/* ============================================================================
 * NexusOS — Virtual Filesystem Header
 * ============================================================================
 * Defines the VFS node structure and interface. All filesystem implementations
 * (ramfs, future disk fs) plug into this layer via function pointers.
 * ============================================================================ */

#ifndef VFS_H
#define VFS_H

#include "types.h"

/* File types */
#define FS_FILE      0x01
#define FS_DIRECTORY 0x02

/* Max lengths */
#define FS_NAME_MAX  32
#define FS_PATH_MAX  128

/* Forward declaration */
struct fs_node;

/* Function pointer types for filesystem operations */
typedef int32_t (*read_fn)(struct fs_node*, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef int32_t (*write_fn)(struct fs_node*, uint32_t offset, uint32_t size, const uint8_t* buffer);
typedef struct fs_node* (*readdir_fn)(struct fs_node*, uint32_t index);
typedef struct fs_node* (*finddir_fn)(struct fs_node*, const char* name);

/* Filesystem node */
typedef struct fs_node {
    char     name[FS_NAME_MAX];     /* Filename */
    uint8_t  type;                  /* FS_FILE or FS_DIRECTORY */
    uint32_t size;                  /* File size in bytes */
    uint32_t inode;                 /* Unique node ID */

    /* Data storage (implementation-specific) */
    uint8_t* data;                  /* Pointer to file data */
    uint32_t capacity;              /* Allocated capacity */

    /* Operations */
    read_fn    read;
    write_fn   write;
    readdir_fn readdir;
    finddir_fn finddir;

    /* Tree structure */
    struct fs_node* parent;
    struct fs_node* children;       /* First child (for directories) */
    struct fs_node* next;           /* Next sibling */
} fs_node_t;

/* VFS API */
void       vfs_init(void);
int32_t    vfs_read(fs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
int32_t    vfs_write(fs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer);
fs_node_t* vfs_readdir(fs_node_t* dir, uint32_t index);
fs_node_t* vfs_finddir(fs_node_t* dir, const char* name);
fs_node_t* vfs_get_root(void);

/* Mount table */
#define VFS_MAX_MOUNTS 8
#define VFS_MOUNT_PATH_MAX 32

typedef struct {
    char       path[VFS_MOUNT_PATH_MAX];
    fs_node_t* root;
    bool       active;
} vfs_mount_t;

/* Mount a filesystem root at a path (e.g. "/disk") */
int vfs_mount(const char* path, fs_node_t* fs_root);

/* Get mount by path */
fs_node_t* vfs_resolve_mount(const char* path);

/* Get mount count */
int vfs_mount_count(void);

#endif /* VFS_H */
