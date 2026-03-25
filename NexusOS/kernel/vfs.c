/* ============================================================================
 * NexusOS — Virtual Filesystem (Implementation)
 * ============================================================================
 * Thin dispatch layer that routes calls through fs_node function pointers.
 * ============================================================================ */

#include "vfs.h"
#include "vga.h"
#include "string.h"

static fs_node_t* fs_root = NULL;

/* Mount table */
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static int num_mounts = 0;

/* --------------------------------------------------------------------------
 * vfs_init: Initialize the VFS (root is set by the actual FS implementation)
 * -------------------------------------------------------------------------- */
void vfs_init(void) {
    fs_root = NULL;
    memset(mount_table, 0, sizeof(mount_table));
    num_mounts = 0;
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Virtual filesystem initialized\n");
}

/* --------------------------------------------------------------------------
 * vfs_read: Read from a file node
 * -------------------------------------------------------------------------- */
int32_t vfs_read(fs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node == NULL || node->read == NULL) return -1;
    return node->read(node, offset, size, buffer);
}

/* --------------------------------------------------------------------------
 * vfs_write: Write to a file node
 * -------------------------------------------------------------------------- */
int32_t vfs_write(fs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (node == NULL || node->write == NULL) return -1;
    return node->write(node, offset, size, buffer);
}

/* --------------------------------------------------------------------------
 * vfs_readdir: Get the nth child of a directory
 * -------------------------------------------------------------------------- */
fs_node_t* vfs_readdir(fs_node_t* dir, uint32_t index) {
    if (dir == NULL || dir->readdir == NULL) return NULL;
    return dir->readdir(dir, index);
}

/* --------------------------------------------------------------------------
 * vfs_finddir: Find a child node by name
 * -------------------------------------------------------------------------- */
fs_node_t* vfs_finddir(fs_node_t* dir, const char* name) {
    if (dir == NULL || dir->finddir == NULL) return NULL;
    return dir->finddir(dir, name);
}

/* --------------------------------------------------------------------------
 * vfs_get_root / vfs_set_root
 * -------------------------------------------------------------------------- */
fs_node_t* vfs_get_root(void) { return fs_root; }

void vfs_set_root(fs_node_t* root) { fs_root = root; }

/* --------------------------------------------------------------------------
 * Mount table operations
 * -------------------------------------------------------------------------- */
int vfs_mount(const char* path, fs_node_t* fs_root_node) {
    if (!path || !fs_root_node || num_mounts >= VFS_MAX_MOUNTS) return -1;

    /* Check for duplicate */
    for (int i = 0; i < num_mounts; i++) {
        if (mount_table[i].active && strcmp(mount_table[i].path, path) == 0)
            return -1;
    }

    vfs_mount_t* m = &mount_table[num_mounts];
    strncpy(m->path, path, VFS_MOUNT_PATH_MAX - 1);
    m->root = fs_root_node;
    m->active = true;
    num_mounts++;
    return 0;
}

fs_node_t* vfs_resolve_mount(const char* path) {
    if (!path) return NULL;

    /* Find longest matching mount path */
    int best = -1;
    int best_len = 0;
    for (int i = 0; i < num_mounts; i++) {
        if (!mount_table[i].active) continue;
        int len = strlen(mount_table[i].path);
        if (strncmp(path, mount_table[i].path, len) == 0 && len > best_len) {
            best = i;
            best_len = len;
        }
    }

    return (best >= 0) ? mount_table[best].root : NULL;
}

int vfs_mount_count(void) { return num_mounts; }
