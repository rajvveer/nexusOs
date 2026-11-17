/* /dev/shm: a tmpfs namespace of named shared-memory files (POSIX shm_open/sem_open, Chromium/Electron
 * shared memory). Files are the same memory files that back memfd descriptions; an entry holds one
 * reference, so data lives until unlink and the last descriptor or mapping is gone. */
#include "native.h"
#include "swap.h"
#include "../../kernel/string.h"

#define NAME_MAX 255U
#define MAX_ENTRIES 4096U
struct shm_entry { struct shm_entry* next; struct memory_file* file; uint32_t id; char name[NAME_MAX + 1]; };
_Static_assert(sizeof(struct shm_entry) <= 4096, "shm entry page");
static struct shm_entry* entries;
static uint32_t next_id, count;

bool shm_path(const char* path) { return !strncmp(path, "/dev/shm/", 9) && path[9]; }
static struct shm_entry* find_name(const char* name) {
    for (struct shm_entry* e = entries; e; e = e->next) if (!strcmp(e->name, name)) return e;
    return NULL;
}
static struct shm_entry* find_id(uint32_t id) {
    for (struct shm_entry* e = entries; e; e = e->next) if (e->id == id) return e;
    return NULL;
}
static int valid_name(const char* name) {
    unsigned size = strlen(name);
    if (!size) return -2;
    for (unsigned i = 0; i < size; i++) if (name[i] == '/') return -2; /* no subdirectories in this tmpfs */
    if (size > NAME_MAX) return -36;
    return (size == 1 && name[0] == '.') || (size == 2 && name[0] == '.' && name[1] == '.') ? -21 : 0;
}
int shm_lookup(const char* name) {
    struct shm_entry* e = valid_name(name) ? NULL : find_name(name);
    return e ? (int)(DEVICE_SHM + e->id) : -1;
}
void shm_device_stat(unsigned device, struct linux_stat* stat) {
    struct shm_entry* e = find_id(device - DEVICE_SHM);
    if (e) memory_file_stat(e->file, stat);
    else { memset(stat, 0, sizeof(*stat)); stat->dev = 10; stat->mode = 0100000; }
}
int shm_directory(uint64_t position, struct directory_item* item) {
    uint64_t index = 0;
    for (struct shm_entry* e = entries; e; e = e->next) if (index++ == position) {
        strcpy(item->name, e->name); item->inode = e->file->inode; item->type = 8; return 1;
    }
    return 0;
}
static bool owner_or_capable(struct memory_file* file) {
    struct credentials* caller = credentials_current();
    return credentials_kernel_id(caller, false, 1) == file->uid || credentials_capable(caller, NULL, 3);
}
/* open(2) below /dev/shm: returns a descriptor, or a negative errno. */
int shm_open_path(const char* path, uint64_t flags, uint32_t mode, uint32_t umask) {
    const char* name = path + 9; int error = valid_name(name); if (error) return error;
    if ((flags & 3) == 3) return -22;
    if (flags & 0x10000) return -20;                      /* O_DIRECTORY on a regular file */
    struct shm_entry* e = find_name(name);
    if (e && (flags & 0xC0) == 0xC0) return -17;
    if (!e) {
        if (!(flags & 0x40)) return -2;
        if (count >= MAX_ENTRIES) return -28;
        e = (void*)page_alloc(); if (!e) return -12;
        e->file = memory_file_create(0);
        if (!e->file) { page_free((uint64_t)e); return -12; }
        memory_file_swappable(e->file);   /* tmpfs pages may go to swap (swap.h) */
        e->file->mode = mode & ~umask & 07777; e->id = ++next_id; strcpy(e->name, name);
        e->next = entries; entries = e; count++;
    } else {
        if (((flags & 3) != 1 && !memory_file_access(e->file, false)) || ((flags & (3 | 0x200)) && !memory_file_access(e->file, true))) return -13;
    }
    if ((flags & 0x200) && (flags & 3)) { error = memory_file_truncate(e->file, 0); if (error) return error; }
    return fd_create_memory(e->file, path, flags);
}
int shm_unlink_path(const char* path) {
    const char* name = path + 9; int error = valid_name(name); if (error) return error == -21 ? -1 : error;
    struct shm_entry** link = &entries; while (*link && strcmp((*link)->name, name)) link = &(*link)->next;
    if (!*link) return -2;
    struct shm_entry* e = *link;
    /* sticky directory (mode 1777): only the owner (or CAP_FOWNER) removes an entry */
    if (!owner_or_capable(e->file)) return -1;
    *link = e->next; count--;
    memory_file_release(e->file); page_free((uint64_t)e); return 0;
}
int shm_rename_path(const char* from, const char* to, bool no_replace) {
    if (!shm_path(from) || !shm_path(to)) return -18;    /* EXDEV across file systems */
    int error = valid_name(to + 9); if (error) return error;
    struct shm_entry* source = find_name(from + 9); if (!source) return -2;
    if (!owner_or_capable(source->file)) return -1;
    struct shm_entry* target = find_name(to + 9);
    if (target == source) return 0;
    if (target && no_replace) return -17;
    if (target) {
        if (!owner_or_capable(target->file)) return -1;
        shm_unlink_path(to);
    }
    strcpy(source->name, to + 9); return 0;
}
/* link(2) inside /dev/shm: a second name for the same memory file (glibc's sem_open creates "sem.XXXXXX", maps it
 * and links it to the semaphore's name, so that the name never shows a half-initialised semaphore). */
int shm_link_path(const char* from, const char* to) {
    if (!shm_path(from) || !shm_path(to)) return -18;    /* EXDEV across file systems */
    int error = valid_name(to + 9); if (error) return error;
    struct shm_entry* source = find_name(from + 9); if (!source) return -2;
    if (!owner_or_capable(source->file) && !(memory_file_access(source->file, false) && memory_file_access(source->file, true))) return -1;   /* protected_hardlinks */
    if (find_name(to + 9)) return -17;
    if (count >= MAX_ENTRIES) return -28;
    struct shm_entry* e = (void*)page_alloc(); if (!e) return -12;
    memory_file_retain(source->file); e->file = source->file; e->id = ++next_id; strcpy(e->name, to + 9);
    e->next = entries; entries = e; count++;
    return 0;
}
int shm_truncate_path(const char* path, uint64_t size) {
    struct shm_entry* e = find_name(path + 9); if (!e) return -2;
    return memory_file_access(e->file, true) ? memory_file_truncate(e->file, size) : -13;
}
int shm_chmod_path(const char* path, uint32_t mode) {
    struct shm_entry* e = find_name(path + 9); if (!e) return -2;
    return memory_file_chmod(e->file, mode & 07777);
}
