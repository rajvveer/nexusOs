/* ============================================================================
 * NexusOS — RAM Filesystem (Implementation)
 * ============================================================================
 * In-memory filesystem. All files live in heap-allocated buffers.
 * Single flat directory (root) containing file nodes.
 * ============================================================================ */

#include "ramfs.h"
#include "heap.h"
#include "string.h"
#include "vga.h"

/* External: set VFS root */
extern void vfs_set_root(fs_node_t* root);

/* Root directory node */
static fs_node_t root_node;
static uint32_t next_inode = 1;
static uint32_t file_count = 0;

/* --------------------------------------------------------------------------
 * ramfs_read: Read data from a ramfs file
 * -------------------------------------------------------------------------- */
static int32_t ramfs_read(fs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node == NULL || node->data == NULL) return -1;
    if (offset >= node->size) return 0;

    uint32_t bytes_to_read = size;
    if (offset + size > node->size) {
        bytes_to_read = node->size - offset;
    }

    memcpy(buffer, node->data + offset, bytes_to_read);
    return (int32_t)bytes_to_read;
}

/* --------------------------------------------------------------------------
 * ramfs_write_fn: Write data to a ramfs file
 * -------------------------------------------------------------------------- */
static int32_t ramfs_write_fn(fs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (node == NULL) return -1;

    /* Ensure we have enough capacity */
    uint32_t needed = offset + size;
    if (needed > RAMFS_MAX_FILE_SIZE) {
        needed = RAMFS_MAX_FILE_SIZE;
        size = needed - offset;
    }

    if (node->data == NULL) {
        node->data = (uint8_t*)kmalloc(needed > 256 ? needed : 256);
        if (node->data == NULL) return -1;
        node->capacity = needed > 256 ? needed : 256;
        memset(node->data, 0, node->capacity);
    } else if (needed > node->capacity) {
        /* Reallocate: allocate new, copy, free old */
        uint32_t new_cap = needed * 2;
        if (new_cap > RAMFS_MAX_FILE_SIZE) new_cap = RAMFS_MAX_FILE_SIZE;
        uint8_t* new_data = (uint8_t*)kmalloc(new_cap);
        if (new_data == NULL) return -1;
        memcpy(new_data, node->data, node->size);
        memset(new_data + node->size, 0, new_cap - node->size);
        kfree(node->data);
        node->data = new_data;
        node->capacity = new_cap;
    }

    memcpy(node->data + offset, buffer, size);
    if (offset + size > node->size) {
        node->size = offset + size;
    }

    return (int32_t)size;
}

/* --------------------------------------------------------------------------
 * ramfs_readdir_fn: Get the nth child of root directory
 * -------------------------------------------------------------------------- */
static fs_node_t* ramfs_readdir_fn(fs_node_t* dir, uint32_t index) {
    if (dir == NULL || dir->type != FS_DIRECTORY) return NULL;

    fs_node_t* child = dir->children;
    uint32_t i = 0;
    while (child != NULL) {
        if (i == index) return child;
        child = child->next;
        i++;
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * ramfs_finddir_fn: Find a file by name in root directory
 * -------------------------------------------------------------------------- */
static fs_node_t* ramfs_finddir_fn(fs_node_t* dir, const char* name) {
    if (dir == NULL || dir->type != FS_DIRECTORY) return NULL;

    fs_node_t* child = dir->children;
    while (child != NULL) {
        if (strcmp(child->name, name) == 0) return child;
        child = child->next;
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * ramfs_create: Create a new file or directory
 * -------------------------------------------------------------------------- */
fs_node_t* ramfs_create(const char* name, uint8_t type) {
    if (file_count >= RAMFS_MAX_FILES) return NULL;
    if (ramfs_finddir_fn(&root_node, name) != NULL) return NULL; /* Already exists */

    fs_node_t* node = (fs_node_t*)kcalloc(1, sizeof(fs_node_t));
    if (node == NULL) return NULL;

    strncpy(node->name, name, FS_NAME_MAX - 1);
    node->name[FS_NAME_MAX - 1] = '\0';
    node->type = type;
    node->size = 0;
    node->inode = next_inode++;
    node->data = NULL;
    node->capacity = 0;
    node->parent = &root_node;
    node->children = NULL;

    /* Set operations */
    node->read = ramfs_read;
    node->write = ramfs_write_fn;
    node->readdir = (type == FS_DIRECTORY) ? ramfs_readdir_fn : NULL;
    node->finddir = (type == FS_DIRECTORY) ? ramfs_finddir_fn : NULL;

    /* Add to root's children (prepend) */
    node->next = root_node.children;
    root_node.children = node;
    file_count++;

    return node;
}

/* --------------------------------------------------------------------------
 * ramfs_delete: Remove a file by name
 * -------------------------------------------------------------------------- */
int ramfs_delete(const char* name) {
    fs_node_t* prev = NULL;
    fs_node_t* current = root_node.children;

    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            /* Unlink from list */
            if (prev == NULL) {
                root_node.children = current->next;
            } else {
                prev->next = current->next;
            }

            /* Free data and node */
            if (current->data != NULL) {
                kfree(current->data);
            }
            kfree(current);
            file_count--;
            return 0;
        }
        prev = current;
        current = current->next;
    }

    return -1; /* Not found */
}

/* --------------------------------------------------------------------------
 * ramfs_get_file_count
 * -------------------------------------------------------------------------- */
uint32_t ramfs_get_file_count(void) { return file_count; }

/* --------------------------------------------------------------------------
 * ramfs_init: Set up root directory and pre-populate files
 * -------------------------------------------------------------------------- */
void ramfs_init(void) {
    /* Initialize root directory */
    memset(&root_node, 0, sizeof(fs_node_t));
    strcpy(root_node.name, "/");
    root_node.type = FS_DIRECTORY;
    root_node.inode = 0;
    root_node.readdir = ramfs_readdir_fn;
    root_node.finddir = ramfs_finddir_fn;

    /* Set as VFS root */
    vfs_set_root(&root_node);

    /* Pre-populate with default files */
    fs_node_t* readme = ramfs_create("readme.txt", FS_FILE);
    if (readme) {
        const char* text = "Welcome to NexusOS!\n"
                           "A hybrid operating system combining the best of\n"
                           "Windows, macOS, and Linux.\n"
                           "\nVersion: 0.2.0 (Phase 2)\n"
                           "Type 'help' for available commands.\n";
        vfs_write(readme, 0, strlen(text), (const uint8_t*)text);
    }

    fs_node_t* version = ramfs_create("version", FS_FILE);
    if (version) {
        const char* ver = "NexusOS v0.2.0\n";
        vfs_write(version, 0, strlen(ver), (const uint8_t*)ver);
    }

    fs_node_t* motd = ramfs_create("motd", FS_FILE);
    if (motd) {
        const char* msg = "=== Message of the Day ===\n"
                          "NexusOS Phase 2: Now with filesystem and multitasking!\n";
        vfs_write(motd, 0, strlen(msg), (const uint8_t*)msg);
    }

    char buf[16];
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("RAM filesystem mounted (");
    int_to_str(file_count, buf);
    vga_print(buf);
    vga_print(" files)\n");
}
