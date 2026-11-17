/* File-system calls desktop applications need beyond the core VFS: statx, hard links, permission and
 * ownership changes with Linux vfat semantics on the FAT root, fallocate (FAT and shared memory),
 * sendfile/copy_file_range and name_to_handle_at. The ext4 driver supplies its own semantics at merge. */
#include "native.h"
#include "../../kernel/string.h"

#define AT_FDCWD ((uint64_t)-100)
#define AT_SYMLINK_NOFOLLOW 0x100ULL
#define AT_SYMLINK_FOLLOW 0x400ULL
#define AT_NO_AUTOMOUNT 0x800ULL
#define AT_EMPTY_PATH 0x1000ULL
#define AT_STATX_SYNC_TYPE 0x6000ULL
#define CHUNK (1024ULL * 1024)
fat32_file_t* kernel_file_disk(struct open_file* file); /* kernel.c: a regular file description's own record */

static int read_path(uint64_t address, char* path) {
    int64_t length = strncpy_from_user(path, address, FAT32_PATH_MAX);   /* validated page by page (uaccess.c) */
    return length < 0 ? (int)length : 0;
}
/* Resolve (dirfd, path) honoring AT_EMPTY_PATH: *description is set for an empty path. */
static int resolve(uint64_t directory, uint64_t address, uint64_t flags, bool follow, char* path, struct open_file** bound, struct open_file** description) {
    *description = NULL; *bound = NULL;
    int error = read_path(address, path); if (error) return error;
    if (!path[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -2;
        if (directory == AT_FDCWD) { strcpy(path, "."); return native_path_resolve(AT_FDCWD, (uint64_t)".", true, path, bound); }
        *description = fd_lookup(directory); return *description ? 0 : -9;
    }
    return native_path_resolve(directory, address, follow, path, bound);
}
static uint32_t dev_major(uint64_t dev) { return (uint32_t)(((dev >> 8) & 0xFFF) | ((dev >> 32) & ~0xFFFULL)); }
static uint32_t dev_minor(uint64_t dev) { return (uint32_t)((dev & 0xFF) | ((dev >> 12) & ~0xFFULL)); }
static uint64_t mount_id(uint64_t dev) { return 20 + dev; }
/* ---- statx(2): generic, from the stat data ---- */
struct statx_time { int64_t seconds; uint32_t nanoseconds; int32_t reserved; };
struct statx64 {
    uint32_t mask, block_size; uint64_t attributes; uint32_t nlink, uid, gid; uint16_t mode, spare0;
    uint64_t ino, size, blocks, attributes_mask; struct statx_time atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor; uint64_t mnt_id; uint32_t dio_mem_align, dio_offset_align; uint64_t spare[12];
};
_Static_assert(sizeof(struct statx64) == 256 && __builtin_offsetof(struct statx64, mnt_id) == 144, "statx ABI");
static int statx_call(uint64_t directory, uint64_t address, uint64_t flags, uint64_t mask, uint64_t buffer) {
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE)) || (flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE) return -22;
    if (mask & 0x80000000U) return -22;
    char path[FAT32_PATH_MAX]; struct open_file *bound, *description; struct linux_stat st;
    int error = resolve(directory, address, flags, !(flags & AT_SYMLINK_NOFOLLOW), path, &bound, &description);
    if (!error) error = description ? fd_description_stat(description, &st) : native_path_stat(path, bound, !(flags & AT_SYMLINK_NOFOLLOW), &st);
    if (error) return error;
    if (!accessible(buffer, sizeof(struct statx64), true)) return -14;
    struct statx64 x = {0};
    x.mask = 0x7FF | 0x1000; /* STATX_BASIC_STATS | STATX_MNT_ID */
    x.block_size = (uint32_t)st.block_size; x.nlink = (uint32_t)st.nlink; x.uid = st.uid; x.gid = st.gid; x.mode = (uint16_t)st.mode;
    x.ino = st.ino; x.size = (uint64_t)st.size; x.blocks = (uint64_t)st.blocks;
    x.atime = (struct statx_time){st.atime, (uint32_t)st.atime_ns, 0};
    x.mtime = (struct statx_time){st.mtime, (uint32_t)st.mtime_ns, 0};
    x.ctime = (struct statx_time){st.ctime, (uint32_t)st.ctime_ns, 0};
    x.rdev_major = dev_major(st.rdev); x.rdev_minor = dev_minor(st.rdev);
    x.dev_major = dev_major(st.dev); x.dev_minor = dev_minor(st.dev); x.mnt_id = mount_id(st.dev);
    return copy_to_user((void*)buffer, &x, sizeof(x)) ? -14 : 0;
}
/* ---- link(2): the FAT root has no hard links (Linux vfat has no ->link: EPERM) ---- */
static int link_call(uint64_t old_directory, uint64_t old_address, uint64_t new_directory, uint64_t new_address, uint64_t flags) {
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) return -22;
    char from[FAT32_PATH_MAX], to[FAT32_PATH_MAX]; struct open_file *bound, *description; struct linux_stat source, target;
    if ((flags & AT_EMPTY_PATH) && !credentials_capable(credentials_current(), NULL, 2)) flags &= ~AT_EMPTY_PATH;
    int error = resolve(old_directory, old_address, flags, (flags & AT_SYMLINK_FOLLOW) != 0, from, &bound, &description);
    if (!error) error = description ? fd_description_stat(description, &source) : native_path_stat(from, bound, (flags & AT_SYMLINK_FOLLOW) != 0, &source);
    if (error) return error;
    if ((error = native_path_resolve(new_directory, new_address, false, to, &bound))) return error;
    if (shm_path(from) || shm_path(to)) return shm_link_path(from, to);   /* /dev/shm names (sem_open) */
    error = native_path_stat(to, bound, false, &target);
    if (!error) return -17;
    if (error != -2) return error;
    if (!native_parent_writable(to)) return -30;
    if (source.dev != 1) return -18;
    return -1;
}
/* ---- chmod/chown with vfat semantics (fat_setattr, fat_sanitize_mode_change) ---- */
static uint32_t fat_owner(const char* path) { return native_path_private(path) ? 1000 : 0; }
static bool owner_or(const char* path, unsigned capability) {
    struct credentials* caller = credentials_current();
    return credentials_kernel_id(caller, false, 1) == fat_owner(path) || credentials_capable(caller, NULL, capability);
}
/* path names the file for the (path-based) vfat policy; file is the inode itself, which may be unlinked. */
static int fat_chmod(const char* path, fat32_file_t* file, uint64_t mode) {
    if (!owner_or(path, 3)) return -1;                       /* CAP_FOWNER */
    if (fat32_is_readonly() || !native_path_private(path)) return -30;
    if (mode & 07000) return -1;                             /* outside FAT_VALID_MODE */
    /* Read/execute bits must stay complete (mask 077); write bits are all or none, kept as ATTR_RO. */
    bool representable = (mode & 0500) == 0500 && !file->is_dir;
    if (file->is_dir && (mode & 0700) != 0700) representable = false;
    if (representable && file->read_only != !(mode & 0200)) {
        /* An unlinked file has no directory entry (its old slot may hold another file): only its records change. */
        if (file->detached) file->read_only = !(mode & 0200);
        else if (fat32_set_read_only(file, !(mode & 0200))) return -5;
        struct memory_file* cache = memory_file_disk_find(file);
        if (cache) cache->disk.read_only = file->read_only;  /* views are reset from it after a writeback */
        file_refresh_views(file);
    }
    if (inotify_watches) inotify_event(path, file, IN_ATTRIB, 0);
    return 0;
}
static int chmod_target(const char* path, struct open_file* bound, struct open_file* description, uint64_t mode) {
    struct regular_view view; struct open_file* target = description ? description : bound;
    if (target) {
        int kind = fd_regular(target, &view);
        if (kind == 2) return memory_file_chmod(view.memory, mode & 07777);
        /* The descriptor's own inode, never its name looked up again: after an unlink that name may be reused. */
        if (kind == 1 || kind == 3) { fat32_file_t file = *kernel_file_disk(target); return fat_chmod(view.path, &file, mode); }
        return -1;
    }
    struct linux_stat st; int error = native_path_stat(path, NULL, true, &st); if (error) return error;
    if (shm_path(path)) return shm_chmod_path(path, (uint32_t)mode);
    if (st.dev != 1) return credentials_capable(credentials_current(), NULL, 3) ? -30 : -1;
    fat32_file_t file; if (!fat32_lookup(path, &file)) return -2;
    return fat_chmod(path, &file, mode);
}
static int chmod_call(uint64_t directory, uint64_t address, uint64_t mode, uint64_t flags, bool extended) {
    if (flags & ~(extended ? AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH : 0)) return -22;
    char path[FAT32_PATH_MAX]; struct open_file *bound, *description;
    int error = resolve(directory, address, flags, !(flags & AT_SYMLINK_NOFOLLOW), path, &bound, &description);
    if (error) return error;
    if ((flags & AT_SYMLINK_NOFOLLOW) && !description) {
        struct linux_stat st; error = native_path_stat(path, bound, false, &st); if (error) return error;
        if ((st.mode & 0170000) == 0120000) return -95;     /* Linux cannot chmod a symlink itself */
    }
    return chmod_target(path, bound, description, mode);
}
static int chown_target(const char* path, struct open_file* target, bool follow, uint64_t user, uint64_t group) {
    struct linux_stat st; int error = target ? fd_description_stat(target, &st) : native_path_stat(path, NULL, follow, &st);
    if (error) return error;
    struct regular_view view;
    if (target && fd_regular(target, &view) == 2) {
        /* Shared memory files keep their creator; only a same-owner change is accepted without CAP_CHOWN. */
        if (((uint32_t)user != ~0U && (uint32_t)user != st.uid) || ((uint32_t)group != ~0U && (uint32_t)group != st.gid))
            return credentials_capable(credentials_current(), NULL, 0) ? -95 : -1;
        return 0;
    }
    if (st.dev != 1) return credentials_capable(credentials_current(), NULL, 0) ? -30 : -1;
    const char* name = target ? view.path : path;           /* fchown(path NULL) of a console fd: its own name */
    /* vfat: ownership is a mount option, so only a no-op change can succeed. */
    if (((uint32_t)user != ~0U && (uint32_t)user != st.uid) || ((uint32_t)group != ~0U && (uint32_t)group != st.gid)) return -1;
    if (!owner_or(name, 0)) return -1;
    if (inotify_watches) inotify_event(name, target ? kernel_file_disk(target) : NULL, IN_ATTRIB, 0); /* the fd's inode, not the name's */
    return 0;
}
static int chown_call(uint64_t directory, uint64_t address, uint64_t user, uint64_t group, uint64_t flags) {
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -22;
    char path[FAT32_PATH_MAX]; struct open_file *bound, *description;
    int error = resolve(directory, address, flags, !(flags & AT_SYMLINK_NOFOLLOW), path, &bound, &description);
    if (error) return error;
    return chown_target(path, description ? description : bound, !(flags & AT_SYMLINK_NOFOLLOW), user, group);
}
/* ---- fallocate(2) ---- */
static int fallocate_call(uint64_t fd, uint64_t mode, int64_t offset, int64_t length) {
    if (offset < 0 || length <= 0) return -22;
    if (mode & ~(1ULL | 2ULL | 8ULL | 0x10ULL | 0x20ULL | 0x40ULL)) return -95;
    if ((mode & 2) && !(mode & 1)) return -22;              /* PUNCH_HOLE needs KEEP_SIZE */
    struct open_file* f = fd_lookup(fd); if (!f) return -9;
    struct regular_view view; int kind = fd_regular(f, &view);
    if (!(view.flags & 3)) return -9;
    if (kind == 3) return -21;
    if (kind == 0) { struct linux_stat st; fd_description_stat(f, &st); return (st.mode & 0170000) == 0010000 ? -29 : -19; }
    if ((uint64_t)offset > 0x7FFFFFFFFFFFFFFFULL - (uint64_t)length) return -27;
    uint64_t end = (uint64_t)offset + (uint64_t)length;
    if (view.flags & 0x400 && (mode & ~1ULL)) return -9;    /* append-only files only grow */
    if (kind == 1) {
        /* Linux vfat: mode 0 is an expanding truncate; no hole punching or ranges. */
        if (mode & ~1ULL) return -95;
        /* shortcut: FALLOC_FL_KEEP_SIZE would need clusters past EOF, which this FAT layer does not track. */
        if (mode & 1) return -95;
        if (end <= view.memory->size) return 0;
        if (end > 0xFFFFFFFFULL) return -27;
        fat32_space_t space; if (fat32_stat_volume(&space)) return -5;
        uint64_t need = (end - view.memory->size + space.cluster_bytes - 1) / space.cluster_bytes;
        if (need > space.free_clusters) return -28;
        return fd_resize(f, end);
    }
    struct memory_file* m = view.memory;
    if (mode & ~3ULL) return -95;                            /* tmpfs: KEEP_SIZE and PUNCH_HOLE */
    if (m->seals & 24) return -1;                            /* F_SEAL_WRITE / F_SEAL_FUTURE_WRITE */
    if (mode & 2) return memory_file_punch(m, (uint64_t)offset, (uint64_t)length);
    if (!(mode & 1) && end > m->size && (m->seals & 4)) return -1; /* F_SEAL_GROW */
    for (uint64_t index = (uint64_t)offset / 4096; index < (end + 4095) / 4096; index++)
        if (!memory_file_page(m, index, true)) return -28;
    if (!(mode & 1) && end > m->size) return memory_file_truncate(m, end);
    return 0;
}
/* ---- faccessat(2)/faccessat2(2): the rules of access(2) relative to a directory descriptor ---- */
static int access_call(uint64_t directory, uint64_t address, uint64_t mode, uint64_t flags) {
    if ((mode & ~7ULL) || (flags & ~(0x200ULL | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))) return -22;
    char path[FAT32_PATH_MAX]; struct open_file *bound, *description; struct linux_stat st;
    int error = resolve(directory, address, flags, !(flags & AT_SYMLINK_NOFOLLOW), path, &bound, &description);
    if (!error) error = description ? fd_description_stat(description, &st) : native_path_stat(path, bound, !(flags & AT_SYMLINK_NOFOLLOW), &st);
    if (error || !mode) return error;
    if (description || bound || st.dev != 1) {
        /* devices, procfs, memory files: the node's permission bits */
        uint32_t needed = (mode & 4 ? 0444 : 0) | (mode & 2 ? 0222 : 0) | (mode & 1 ? 0111 : 0);
        for (unsigned bit = 0; bit < 3; bit++) {
            uint32_t mask = 0444U >> bit; if ((needed & mask) && !(st.mode & mask)) return -13;
        }
        return 0;
    }
    if ((mode & 1) && !(st.mode & 0111)) return -13;
    if (mode & 2) {
        if (!native_path_writable(path)) return -30;
        if (!(st.mode & 0200) && !credentials_capable(credentials_current(), NULL, 1)) return -13;
    }
    return 0;
}
/* ---- fadvise64(2): validated; the page cache already reads ahead on its own ---- */
static int advise_call(uint64_t fd, int64_t offset, int64_t length, uint64_t advice) {
    struct open_file* f = fd_lookup(fd); if (!f) return -9;
    struct linux_stat st; int error = fd_description_stat(f, &st); if (error) return error;
    if ((st.mode & 0170000) == 0010000) return -29;
    if (advice > 5 || length < 0) return -22;
    (void)offset; return 0;
}
/* ---- readahead(2): pull the range into the page cache ---- */
static int readahead_call(uint64_t fd, int64_t offset, uint64_t count) {
    struct open_file* f = fd_lookup(fd); if (!f) return -9;
    struct regular_view view; int kind = fd_regular(f, &view);
    if ((view.flags & 3) == 1 || (view.flags & 0x200000)) return -9;   /* write-only or O_PATH */
    if (kind != 1 && kind != 2) return -22;
    if (offset < 0) return -22;
    struct memory_file* m = view.memory; uint64_t end = (uint64_t)offset + (count > (32ULL << 20) ? 32ULL << 20 : count);
    if (end > m->size) end = m->size;
    for (uint64_t index = (uint64_t)offset / 4096; index * 4096 < end; index++)
        if (!memory_file_page(m, index, m->disk_backed)) break;
    return 0;
}
/* ---- truncate(2) on a path ---- */
static int truncate_call(uint64_t address, int64_t length) {
    if (length < 0) return -22;
    char path[FAT32_PATH_MAX]; struct open_file* bound; struct regular_view view;
    int error = native_path_resolve(AT_FDCWD, address, true, path, &bound); if (error) return error;
    fat32_file_t file; struct memory_file* cache = NULL;
    if (bound) {
        int kind = fd_regular(bound, &view);
        if (kind == 2) return memory_file_access(view.memory, true) ? memory_file_truncate(view.memory, (uint64_t)length) : -13;
        if (kind == 3) return -21;
        if (kind != 1) return -22;
        /* The open file itself, never its name looked up again: after an unlink that name may be reused. */
        strcpy(path, view.path); file = *kernel_file_disk(bound); cache = view.memory;
    } else {
        if (shm_path(path)) return shm_truncate_path(path, (uint64_t)length);
        if (!fat32_lookup(path, &file)) return -2;
    }
    if (file.is_dir) return -21;
    if (file.native_kind) return -22;
    if (!native_path_writable(path)) return -30;
    if (file.read_only && !credentials_capable(credentials_current(), NULL, 1)) return -13;
    if ((uint64_t)length > 0xFFFFFFFFULL) return -27;
    if (cache) memory_file_retain(cache);
    else if (!(cache = memory_file_disk_get(&file))) return -12;
    error = cache->exec_maps ? -26 : memory_file_truncate(cache, (uint64_t)length);
    memory_file_release(cache);
    if (!error && inotify_watches) inotify_event(path, &file, IN_MODIFY, 0);
    return error;
}
/* ---- sendfile(2) / copy_file_range(2): chunked through a temporary private user buffer ---- */
static int64_t copy_data(struct open_file* in, uint64_t* in_position, struct open_file* out, uint64_t* out_position, uint64_t count) {
    if (count > 0x7FFFF000) count = 0x7FFFF000;
    if (!count) return 0;
    uint64_t size = count < CHUNK ? (count + 4095) & ~4095ULL : CHUNK;
    uint64_t buffer = native_scratch_map(size); if ((int64_t)buffer < 0) return (int64_t)buffer;
    int64_t done = 0;
    while ((uint64_t)done < count) {
        uint64_t part = count - (uint64_t)done < size ? count - (uint64_t)done : size;
        int64_t got = fd_transfer(in, buffer, part, *in_position, true, false);
        if (got <= 0) { if (!done) done = got; break; }
        int64_t put = out_position ? fd_transfer(out, buffer, (uint64_t)got, *out_position, true, true) : fd_transfer(out, buffer, (uint64_t)got, 0, false, true);
        if (put <= 0) { if (!done) done = put; break; }
        *in_position += (uint64_t)put; if (out_position) *out_position += (uint64_t)put;
        done += put;
        if (put < got) break;
    }
    native_scratch_unmap(buffer, size); return done;
}
static int64_t sendfile_call(uint64_t out_fd, uint64_t in_fd, uint64_t offset_address, uint64_t count) {
    struct open_file *in = fd_lookup(in_fd), *out = fd_lookup(out_fd);
    if (!in || !out) return -9;
    struct regular_view in_view, out_view; int kind = fd_regular(in, &in_view); fd_regular(out, &out_view);
    if ((in_view.flags & 3) == 1 || (in_view.flags & 0x200000) || !(out_view.flags & 3)) return -9;
    if (kind != 1 && kind != 2) return -22;
    if (out_view.flags & 0x400) return -22;
    uint64_t position;
    if (offset_address) {
        if (!accessible(offset_address, 8, true)) return -14;
        int64_t start = (int64_t)uget64(offset_address); if (start < 0) return -22;
        position = (uint64_t)start;
    } else position = *in_view.position;
    fd_pin(in); fd_pin(out);
    int64_t result = copy_data(in, &position, out, NULL, count);
    fd_unpin(out); fd_unpin(in);
    if (result >= 0 || position) {
        if (offset_address) { if (accessible(offset_address, 8, true)) uput64(offset_address, position); }
        else *in_view.position = position;
    }
    return result;
}
static int64_t copy_range_call(uint64_t in_fd, uint64_t in_offset, uint64_t out_fd, uint64_t out_offset, uint64_t length, uint64_t flags) {
    if (flags) return -22;
    struct open_file *in = fd_lookup(in_fd), *out = fd_lookup(out_fd);
    if (!in || !out) return -9;
    struct regular_view in_view, out_view; int in_kind = fd_regular(in, &in_view), out_kind = fd_regular(out, &out_view);
    if ((in_view.flags & 3) == 1 || (in_view.flags & 0x200000) || !(out_view.flags & 3) || (out_view.flags & 0x400)) return -9;
    if (in_kind == 3 || out_kind == 3) return -21;
    if ((in_kind != 1 && in_kind != 2) || (out_kind != 1 && out_kind != 2)) return -22;
    uint64_t from = *in_view.position, to = *out_view.position;
    if (in_offset) { if (!accessible(in_offset, 8, true)) return -14; from = uget64(in_offset); if ((int64_t)from < 0) return -22; }
    if (out_offset) { if (!accessible(out_offset, 8, true)) return -14; to = uget64(out_offset); if ((int64_t)to < 0) return -22; }
    if (from > ~0ULL - length || to > ~0ULL - length) return -27;
    if (in_view.memory == out_view.memory && from < to + length && to < from + length) return -22;
    fd_pin(in); fd_pin(out);
    int64_t result = copy_data(in, &from, out, &to, length);
    fd_unpin(out); fd_unpin(in);
    if (in_offset) { if (accessible(in_offset, 8, true)) uput64(in_offset, from); } else *in_view.position = from;
    if (out_offset) { if (accessible(out_offset, 8, true)) uput64(out_offset, to); } else *out_view.position = to;
    return result;
}
/* ---- name_to_handle_at(2) ---- */
static int handle_call(uint64_t directory, uint64_t address, uint64_t handle, uint64_t mount, uint64_t flags) {
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH | 0x200ULL | 0x1ULL)) return -22;
    char path[FAT32_PATH_MAX]; struct open_file *bound, *description; struct linux_stat st;
    int error = resolve(directory, address, flags, (flags & AT_SYMLINK_FOLLOW) != 0, path, &bound, &description);
    if (!error) error = description ? fd_description_stat(description, &st) : native_path_stat(path, bound, (flags & AT_SYMLINK_FOLLOW) != 0, &st);
    if (error) return error;
    if (st.dev == 7 || st.dev == 8) return -95;             /* procfs and nsfs cannot be exported */
    if (!accessible(handle, 8, true) || !accessible(mount, (flags & 1) ? 8 : 4, true)) return -14;
    uint32_t capacity = uget32(handle);
    if (capacity > 128) return -22;                          /* MAX_HANDLE_SZ */
    struct { uint64_t inode; uint32_t device; } __attribute__((packed)) payload = {st.ino, (uint32_t)st.dev};
    uint32_t header[2] = {sizeof(payload), st.dev == 1 ? 0x71U : 0x1U}; /* FILEID_FAT_WITHOUT_PARENT / FILEID_INO32_GEN */
    if (flags & 1) uput64(mount, mount_id(st.dev)); else uput32(mount, (uint32_t)mount_id(st.dev));
    if (capacity < sizeof(payload)) { uput32(handle, sizeof(payload)); return -75; }
    if (!accessible(handle, 8 + sizeof(payload), true)) return -14;
    copy_to_user((void*)handle, header, 8); return copy_to_user((void*)(handle + 8), &payload, sizeof(payload)) ? -14 : 0;
}
/* ---- extended attributes: neither root filesystem stores them (Linux vfat: no xattr handlers). The target is
 * resolved first (ENOENT/EBADF as on Linux); get/set/remove then fail with EOPNOTSUPP and list returns an empty
 * list. Wine keeps DOS attributes in user.DOSATTRIB and treats EOPNOTSUPP as "not supported here". ---- */
static int64_t xattr_call(uint64_t call, struct registers* r) {
    bool by_fd = call == 190 || call == 193 || call == 196 || call == 199;
    if (by_fd) { if (!fd_lookup(r->rdi)) return -9; }
    else {
        char path[FAT32_PATH_MAX]; struct open_file* bound; struct linux_stat st;
        bool follow = !(call == 189 || call == 192 || call == 195 || call == 198);   /* l*xattr do not follow */
        int error = native_path_resolve(AT_FDCWD, r->rdi, follow, path, &bound);
        if (!error) error = native_path_stat(path, bound, follow, &st);
        if (error) return error;
    }
    if (call == 188 || call == 189 || call == 190 || call == 197 || call == 198 || call == 199) {   /* set/remove: name must be readable */
        char name[256];                                     /* XATTR_NAME_MAX 255 + NUL: empty or longer is ERANGE */
        for (unsigned i = 0; ; i++) {
            if (i == sizeof(name)) return -34;
            if (r->rsi > ~0ULL - i || !accessible(r->rsi + i, 1, false)) return -14;
            if (!(name[i] = (char)uget8(r->rsi + i))) break;
        }
        if (!name[0]) return -34;
    }
    if (call >= 194 && call <= 196) return 0;                                      /* listxattr: no attributes */
    return -95;
}
int64_t fsext_call(struct registers* r) {
    switch (r->rax) {
    case 188: case 189: case 190: case 191: case 192: case 193: case 194: case 195: case 196: case 197: case 198: case 199:
        return xattr_call(r->rax, r);
    case 332: return statx_call(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case 86: return link_call(AT_FDCWD, r->rdi, AT_FDCWD, r->rsi, 0);
    case 265: return link_call(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case 90: return chmod_call(AT_FDCWD, r->rdi, r->rsi, 0, false);
    case 268: return chmod_call(r->rdi, r->rsi, r->rdx, 0, false);
    case 452: return chmod_call(r->rdi, r->rsi, r->rdx, r->r10, true);
    case 91: { struct open_file* f = fd_lookup(r->rdi); return f ? chmod_target(NULL, NULL, f, r->rsi) : -9; }
    case 92: case 94: return chown_call(AT_FDCWD, r->rdi, r->rsi, r->rdx, r->rax == 94 ? AT_SYMLINK_NOFOLLOW : 0);
    case 93: { struct open_file* f = fd_lookup(r->rdi); return f ? chown_target(NULL, f, true, r->rsi, r->rdx) : -9; }
    case 260: return chown_call(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case 76: return truncate_call(r->rdi, (int64_t)r->rsi);
    case 269: return access_call(r->rdi, r->rsi, r->rdx, 0);
    case 439: return access_call(r->rdi, r->rsi, r->rdx, r->r10);
    case 187: return readahead_call(r->rdi, (int64_t)r->rsi, r->rdx);
    case 221: return advise_call(r->rdi, (int64_t)r->rsi, (int64_t)r->rdx, r->r10);
    case 285: return fallocate_call(r->rdi, r->rsi, (int64_t)r->rdx, (int64_t)r->r10);
    case 40: return sendfile_call(r->rdi, r->rsi, r->rdx, r->r10);
    case 326: return copy_range_call(r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9);
    case 303: return handle_call(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    }
    return -38;
}
