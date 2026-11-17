/* ============================================================================
 * NexusOS — RAM Filesystem (Implementation)
 * ============================================================================
 * In-memory filesystem. All files live in heap-allocated buffers.
 * Single flat directory (root) containing file nodes.
 * ============================================================================ */

#include "ramfs.h"
#include "version.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "users.h"
#include "persist.h"   /* Phase 55: mutation hooks for the disk snapshot */

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
    if (node == NULL) return -1;
    /* An empty file (never written: data == NULL) is a valid 0-byte read. */
    if (node->data == NULL || offset >= node->size) return 0;

    uint32_t bytes_to_read = size;
    if (size > node->size - offset) {           /* overflow-safe clamp */
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

    /* Ensure we have enough capacity. Clamp without forming offset+size first
     * (it can wrap to a small value and under-allocate before the memcpy). */
    if (offset >= RAMFS_MAX_FILE_SIZE) return 0;
    if (size > RAMFS_MAX_FILE_SIZE - offset) size = RAMFS_MAX_FILE_SIZE - offset;
    uint32_t needed = offset + size;

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

    persist_note_write(node);                  /* Phase 55 */
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
 * ramfs_mknode: Create a node inside an arbitrary directory (Phase 54)
 * -------------------------------------------------------------------------- */
fs_node_t* ramfs_mknode(fs_node_t* parent, const char* name, uint8_t type) {
    if (parent == NULL || parent->type != FS_DIRECTORY) return NULL;
    if (!name || !name[0] || strlen(name) >= FS_NAME_MAX) return NULL;
    if (file_count >= RAMFS_MAX_FILES) return NULL;
    if (ramfs_finddir_fn(parent, name) != NULL) return NULL; /* Already exists */

    fs_node_t* node = (fs_node_t*)kcalloc(1, sizeof(fs_node_t));
    if (node == NULL) return NULL;

    strncpy(node->name, name, FS_NAME_MAX - 1);
    node->name[FS_NAME_MAX - 1] = '\0';
    node->type = type;
    node->size = 0;
    node->inode = next_inode++;
    node->data = NULL;
    node->capacity = 0;
    node->parent = parent;
    node->children = NULL;

    /* Set operations */
    node->read = ramfs_read;
    node->write = ramfs_write_fn;
    node->readdir = (type == FS_DIRECTORY) ? ramfs_readdir_fn : NULL;
    node->finddir = (type == FS_DIRECTORY) ? ramfs_finddir_fn : NULL;

    /* Phase 44: default ownership (creator) + mode (0644 file / 0755 dir) */
    vfs_init_perms(node);

    /* Add to parent's children (prepend) */
    node->next = parent->children;
    parent->children = node;
    file_count++;

    persist_note_create(node);                 /* Phase 55 */
    return node;
}

/* --------------------------------------------------------------------------
 * ramfs_create: Create a new file or directory in the root (legacy)
 * -------------------------------------------------------------------------- */
fs_node_t* ramfs_create(const char* name, uint8_t type) {
    return ramfs_mknode(&root_node, name, type);
}

/* --------------------------------------------------------------------------
 * ramfs_delete: Remove a file by name
 * -------------------------------------------------------------------------- */
int ramfs_delete(const char* name) {
    fs_node_t* prev = NULL;
    fs_node_t* current = root_node.children;

    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            /* Phase 44: only the owner or root may delete a file (delete is a
             * change to the file's existence — gate on ownership, not mode). */
            uint32_t cu = users_current_uid();
            if (cu != UID_ROOT && current->uid != cu)
                return -2;  /* permission denied */

            persist_note_unlink(current);      /* Phase 55 (before free) */
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
 * ramfs_rename: Rename in place (no data copy, so any size is safe and the
 * file is never lost). Fails if the target exists. Same gate as delete.
 * -------------------------------------------------------------------------- */
int ramfs_rename(const char* old_name, const char* new_name) {
    if (!new_name || !*new_name || strlen(new_name) >= FS_NAME_MAX) return -1;
    fs_node_t* n = ramfs_finddir_fn(&root_node, old_name);
    if (!n || ramfs_finddir_fn(&root_node, new_name)) return -1;
    uint32_t cu = users_current_uid();
    if (cu != UID_ROOT && n->uid != cu) return -2;  /* permission denied */
    persist_note_rename(n);                    /* Phase 55 (old name still set) */
    strcpy(n->name, new_name);
    return 0;
}

/* ============================================================================
 * Phase 54: path resolution + real directories
 * ============================================================================ */

/* Unlink `node` from its parent's child list (does not free). */
static void ramfs_unlink_child(fs_node_t* node) {
    if (!node || !node->parent) return;
    fs_node_t* parent = node->parent;
    fs_node_t* prev = NULL;
    fs_node_t* cur = parent->children;
    while (cur) {
        if (cur == node) {
            if (prev) prev->next = cur->next;
            else parent->children = cur->next;
            node->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

fs_node_t* ramfs_resolve(const char* path, fs_node_t* cwd) {
    if (!path) return NULL;
    fs_node_t* cur = (path[0] == '/') ? &root_node : (cwd ? cwd : &root_node);
    const char* p = path;
    char comp[FS_NAME_MAX];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int ci = 0;
        while (*p && *p != '/' && ci < FS_NAME_MAX - 1) comp[ci++] = *p++;
        comp[ci] = '\0';
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) { if (cur->parent) cur = cur->parent; continue; }
        if (cur->type != FS_DIRECTORY) return NULL;
        fs_node_t* next = ramfs_finddir_fn(cur, comp);
        if (!next) return NULL;
        cur = next;
    }
    return cur;
}

fs_node_t* ramfs_resolve_parent(const char* path, fs_node_t* cwd, char* leaf) {
    if (!path || !leaf) return NULL;
    char tmp[FS_PATH_MAX];
    strncpy(tmp, path, FS_PATH_MAX - 1);
    tmp[FS_PATH_MAX - 1] = '\0';

    int len = (int)strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';  /* strip trailing / */

    int slash = -1;
    for (int i = 0; i < len; i++) if (tmp[i] == '/') slash = i;

    if (slash < 0) {                        /* bare name, relative to cwd */
        strncpy(leaf, tmp, FS_NAME_MAX - 1);
        leaf[FS_NAME_MAX - 1] = '\0';
        return cwd ? cwd : &root_node;
    }

    strncpy(leaf, tmp + slash + 1, FS_NAME_MAX - 1);
    leaf[FS_NAME_MAX - 1] = '\0';

    if (slash == 0) return &root_node;      /* parent is "/" */
    tmp[slash] = '\0';
    return ramfs_resolve(tmp, cwd);
}

int ramfs_mkdir(const char* path, fs_node_t* cwd) {
    char leaf[FS_NAME_MAX];
    fs_node_t* parent = ramfs_resolve_parent(path, cwd, leaf);
    if (!parent || parent->type != FS_DIRECTORY) return -3;
    if (!leaf[0] || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) return -1;
    if (ramfs_finddir_fn(parent, leaf)) return -1;             /* exists */
    if (!vfs_check_perm(parent, users_current_uid(), ACC_WRITE)) return -2;
    return ramfs_mknode(parent, leaf, FS_DIRECTORY) ? 0 : -1;
}

int ramfs_rmdir(const char* path, fs_node_t* cwd) {
    char leaf[FS_NAME_MAX];
    fs_node_t* parent = ramfs_resolve_parent(path, cwd, leaf);
    if (!parent) return -1;
    fs_node_t* n = ramfs_finddir_fn(parent, leaf);
    if (!n) return -1;
    if (n->type != FS_DIRECTORY) return -4;
    if (n == &root_node) return -1;
    if (n->children) return -3;                                /* not empty */
    uint32_t cu = users_current_uid();
    if (cu != UID_ROOT && n->uid != cu) return -2;
    persist_note_unlink(n);                    /* Phase 55 */
    ramfs_unlink_child(n);
    kfree(n);
    file_count--;
    return 0;
}

int ramfs_rm_path(const char* path, fs_node_t* cwd) {
    char leaf[FS_NAME_MAX];
    fs_node_t* parent = ramfs_resolve_parent(path, cwd, leaf);
    if (!parent) return -1;
    fs_node_t* n = ramfs_finddir_fn(parent, leaf);
    if (!n) return -1;
    if (n->type == FS_DIRECTORY) return -4;                    /* use rmdir */
    uint32_t cu = users_current_uid();
    if (cu != UID_ROOT && n->uid != cu) return -2;
    persist_note_unlink(n);                    /* Phase 55 */
    ramfs_unlink_child(n);
    if (n->data) kfree(n->data);
    kfree(n);
    file_count--;
    return 0;
}

void ramfs_abspath(fs_node_t* node, char* out, int sz) {
    if (sz <= 0) return;
    if (!node || !node->parent) { strncpy(out, "/", sz); out[sz - 1] = '\0'; return; }
    const char* parts[32];
    int np = 0;
    fs_node_t* c = node;
    while (c && c->parent && np < 32) { parts[np++] = c->name; c = c->parent; }
    int pos = 0;
    for (int i = np - 1; i >= 0; i--) {
        if (pos < sz - 1) out[pos++] = '/';
        const char* s = parts[i];
        while (*s && pos < sz - 1) out[pos++] = *s++;
    }
    out[pos] = '\0';
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
        const char* ver = NEXUS_VERSION_STRING "\n";
        vfs_write(version, 0, strlen(ver), (const uint8_t*)ver);
    }

    fs_node_t* motd = ramfs_create("motd", FS_FILE);
    if (motd) {
        const char* msg = "=== Message of the Day ===\n"
                          "NexusOS Phase 2: Now with filesystem and multitasking!\n";
        vfs_write(motd, 0, strlen(msg), (const uint8_t*)msg);
    }

    /* Phase 54: a few default directories so the tree looks real. These are
     * created at boot while the current user is root (login runs later). */
    fs_node_t* home = ramfs_mknode(&root_node, "home", FS_DIRECTORY);
    if (home) {
        ramfs_mknode(home, "root", FS_DIRECTORY);
        ramfs_mknode(home, "guest", FS_DIRECTORY);
    }
    fs_node_t* tmp_dir = ramfs_mknode(&root_node, "tmp", FS_DIRECTORY);
    if (tmp_dir) tmp_dir->mode = 0777;      /* world-writable scratch, like /tmp */

    char buf[16];
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("RAM filesystem mounted (");
    int_to_str(file_count, buf);
    vga_print(buf);
    vga_print(" files)\n");
}
