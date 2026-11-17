/* Root filesystem dispatch (FAT32 or ext4) and POSIX file attributes for the native kernel.
 * See vfs.h. FAT32 keeps its existing semantics (synthesized owners/modes, path-based write policy,
 * no hard links/devices, write-time only) and reports EPERM where Linux vfat cannot represent a
 * change; ext4 stores everything and enforces permissions against the caller's credentials. */
#define VFS_IMPLEMENTATION
#include "native.h"
#include "vfs.h"
#include "ext4.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

/* kernel.c services */
struct open_file;
int kernel_user_path(uint64_t address, char* path);
int kernel_path_at(uint64_t directory, char* path, bool follow, struct open_file** bound);
struct open_file* kernel_file(uint64_t fd);
fat32_file_t* kernel_file_disk(struct open_file* file);
struct memory_file* kernel_file_memory(struct open_file* file);
int kernel_resolved_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* st);
int kernel_truncate_path(const char* path, uint64_t size);
bool kernel_private_path(const char* path);
unsigned filesystem_umask(void);
unsigned filesystem_exchange_umask(unsigned mask);
bool credentials_in_group(struct credentials* credentials, uint32_t gid, bool real);
bool credentials_access_capable(struct credentials* credentials, unsigned capability);
bool credentials_kernel_from_visible(uint32_t id, bool group, uint32_t* result);
void credentials_exec_setid(bool set_uid, uint32_t uid, bool set_gid, uint32_t gid);

#define CAP_CHOWN 0
#define CAP_DAC_OVERRIDE 1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER 3
#define CAP_FSETID 4
#define CAP_MKNOD 27
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

static bool posix;
bool disk_posix(void) { return posix; }
bool disk_root_present(void) { return ext4_probe(); }
bool disk_mount_root(void) {
    if (ext4_probe()) {
        int error = ext4_mount();
        if (!error) { posix = true; return true; }
        char n[24]; number64((uint64_t)-error, n);
        vga_print("[VFS] ext4 root found but not mountable, errno="); vga_print(n); vga_print("\n");
        return false;
    }
    return fat32_init();
}
bool disk_is_readonly(void) { return posix ? ext4_is_readonly() : fat32_is_readonly(); }

/* ------------------------------------------------------------------ fat32_* dispatch */
bool disk_lookup(const char* path, fat32_file_t* out) { return posix ? ext4_lookup(path, out) == 0 : fat32_lookup(path, out); }
int disk_lookup_status(const char* path, fat32_file_t* out) {
    if (!posix) return fat32_lookup_status(path, out);
    int error = ext4_lookup(path, out);
    return !error ? 0 : error == -2 || error == -20 || error == -36 ? -2 : -5;
}
int disk_refresh_file(fat32_file_t* file) {
    if (!posix) return fat32_refresh_file(file);
    return ext4_refresh(file) ? -1 : 0;
}
int32_t disk_read_file(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer) {
    if (posix) { int64_t r = ext4_read(file, offset, length, buffer); return r < 0 ? -1 : (int32_t)r; }
    if (offset > 0xFFFFFFFFULL) return 0;
    if (length > 0x7FFFFFFFULL) length = 0x7FFFFFFFULL;
    return fat32_read_file(file, (uint32_t)offset, (uint32_t)length, buffer);
}
/* Sequential readdir cursors: index-based FAT positions map to ext4 byte cookies. */
static struct { uint32_t dir, next; uint64_t cookie, used; } cursors[16];
static uint64_t cursor_clock;
int disk_read_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out) {
    if (!posix) return fat32_read_dir_entry(dir, index, out);
    unsigned pick = 0; bool found = false;
    for (unsigned i = 0; i < 16; i++) if (cursors[i].dir == dir->ino && cursors[i].next <= index &&
                                          (!found || cursors[i].next > cursors[pick].next)) { pick = i; found = true; }
    if (!found) {
        for (unsigned i = 1; i < 16; i++) if (cursors[i].used < cursors[pick].used) pick = i;
        cursors[pick].dir = dir->ino; cursors[pick].next = 0; cursors[pick].cookie = 0;
    }
    cursors[pick].used = ++cursor_clock;
    fat32_file_t skip;
    while (cursors[pick].next < index) {
        int r = ext4_read_dir(dir, &cursors[pick].cookie, &skip);
        if (r <= 0) { cursors[pick].dir = 0; return r < 0 ? -1 : 0; }
        cursors[pick].next++;
    }
    int r = ext4_read_dir(dir, &cursors[pick].cookie, out);
    if (r == 1) cursors[pick].next++; else cursors[pick].dir = 0;
    return r < 0 ? -1 : r;
}
int disk_read_dir_at(const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) {
    if (posix) { int r = ext4_read_dir(dir, cookie, out); return r < 0 ? -1 : r; }
    if (*cookie > 0xFFFFFFFFULL) return 0;
    int r = fat32_read_dir_entry(dir, (uint32_t)*cookie, out);
    if (r > 0) (*cookie)++;
    return r;
}
static int fat_create_error(int error) { return error == -2 ? -17 : error == -3 ? -2 : error == -4 ? -28 : -5; }
static int ext4_to_fat_create(int error) { return !error ? 0 : error == -17 ? -2 : error == -2 || error == -20 ? -3 : error == -28 ? -4 : -1; }
int disk_create_path(const char* path, bool is_dir, fat32_file_t* out) {
    if (!posix) return fat32_create_path(path, is_dir, out);
    return ext4_to_fat_create(disk_create_node(path, (is_dir ? S_IFDIR | 0777 : S_IFREG | 0666), 0, NULL, out));
}
int disk_create_socket_path(const char* path, fat32_file_t* out) {
    if (!posix) return fat32_create_socket_path(path, out);
    return ext4_to_fat_create(disk_create_node(path, S_IFSOCK | 0777, 0, NULL, out));
}
int disk_write_file(fat32_file_t* file, uint64_t offset, uint64_t length, const void* buffer) {
    if (posix) { int64_t r = ext4_write(file, offset, length, buffer); return r == -28 ? -2 : r < 0 ? -1 : (int)r; }
    if (offset > 0xFFFFFFFFULL || length > 0xFFFFFFFFULL - offset) return -5;
    return fat32_write_file(file, (uint32_t)offset, (uint32_t)length, buffer);
}
int disk_truncate_file(fat32_file_t* file, uint64_t size) {
    if (posix) { int r = ext4_truncate(file, size); return r == -28 ? -2 : r ? -1 : 0; }
    if (size > 0xFFFFFFFFULL) return -1;
    return fat32_truncate_file(file, (uint32_t)size);
}
int disk_remove_path(const char* path) {
    if (!posix) return fat32_remove_path(path);
    fat32_file_t victim; int error = ext4_unlink(path, -1, &victim);
    if (error) return -1;
    if (victim.detached) ext4_release(&victim);
    return 0;
}
int disk_rename_path(const char* from, const char* to, bool no_replace, fat32_file_t* moved, fat32_file_t* replaced) {
    if (!posix) return fat32_rename_path(from, to, no_replace, moved, replaced);
    return ext4_rename(from, to, no_replace, moved, replaced);
}
int disk_detach_path(const char* path, fat32_file_t* detached) {
    if (!posix) return fat32_detach_path(path, detached);
    return ext4_unlink(path, -1, detached);
}
int disk_dispose_file(fat32_file_t* file) {
    if (!posix) return fat32_dispose_file(file);
    return ext4_release(file);
}
int disk_begin_download(const char* path, fat32_file_t* out) {
    if (!posix) return fat32_begin_download(path, out);
    struct credentials* c = credentials_current();
    int error = ext4_create_unlinked(path, S_IFREG | (0666 & ~filesystem_umask()), credentials_kernel_id(c, false, 3), credentials_kernel_id(c, true, 3), out);
    if (!error) out->pending = true;
    return ext4_to_fat_create(error);
}
static int publish(fat32_file_t* file, bool symlink) {
    if (!file || !file->pending) return -1;
    int error = ext4_link_unlinked(file, symlink);
    if (!error) file->pending = false;
    return error == -17 ? -2 : error ? -1 : 0;
}
int disk_finish_download(fat32_file_t* file) { return posix ? publish(file, false) : fat32_finish_download(file); }
int disk_finish_symlink(fat32_file_t* file) { return posix ? publish(file, true) : fat32_finish_symlink(file); }
int disk_abort_download(fat32_file_t* file) {
    if (!posix) return fat32_abort_download(file);
    if (!file || !file->pending) return 0;
    file->pending = false; file->detached = true;
    return ext4_release(file) ? -1 : 0;
}
int disk_set_times(fat32_file_t* file, uint16_t date, uint16_t time) {
    if (!posix) return fat32_set_times(file, date, time);
    struct ext4_attr attr = {.valid = EXT4_ATTR_MTIME | EXT4_ATTR_CTIME, .mtime = (int64_t)native_realtime_ns(), .ctime = (int64_t)native_realtime_ns()};
    return ext4_setattr(file, &attr) ? -1 : 0;
}
int disk_stat_volume(fat32_space_t* space) {
    if (!posix) return fat32_stat_volume(space);
    struct ext4_volume v; if (ext4_statfs(&v)) return -1;
    space->cluster_bytes = (uint32_t)v.block_size; space->total_clusters = (uint32_t)(v.blocks > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : v.blocks);
    space->free_clusters = (uint32_t)(v.free_blocks > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : v.free_blocks);
    space->serial = v.fsid[0]; space->start_lba = v.fsid[1];
    return 0;
}

/* ------------------------------------------------------------------ permissions */
static bool capable(unsigned cap) { return credentials_capable(credentials_current(), NULL, cap); }
static uint32_t fsuid(void) { return credentials_kernel_id(credentials_current(), false, 3); }
static uint32_t fsgid(void) { return credentials_kernel_id(credentials_current(), true, 3); }
static bool owner_or_capable(const fat32_file_t* f) { return fsuid() == f->uid || capable(CAP_FOWNER); }
/* generic_permission + capability overrides (initial user namespace). */
static int check_mode(const fat32_file_t* f, unsigned mask, bool real_ids) {
    struct credentials* c = credentials_current();
    uint32_t uid = credentials_kernel_id(c, false, real_ids ? 0 : 3);
    unsigned mode = f->mode, perm;
    if (uid == f->uid) perm = (mode >> 6) & 7;
    else if (credentials_in_group(c, f->gid, real_ids)) perm = (mode >> 3) & 7;
    else perm = mode & 7;
    if ((perm & mask) == mask) return 0;
    bool dir = (mode & S_IFMT) == S_IFDIR;
    bool override = real_ids ? credentials_access_capable(c, CAP_DAC_OVERRIDE) : capable(CAP_DAC_OVERRIDE);
    bool search = real_ids ? credentials_access_capable(c, CAP_DAC_READ_SEARCH) : capable(CAP_DAC_READ_SEARCH);
    if (override && (!(mask & DISK_MAY_EXEC) || dir || (mode & 0111))) return 0;
    if (search && !(mask & DISK_MAY_WRITE) && (!(mask & DISK_MAY_EXEC) || dir)) return 0;
    return -13;
}
int disk_permission(const fat32_file_t* file, unsigned mask) {
    if (!posix || !file) return 0;
    if ((mask & DISK_MAY_WRITE) && ext4_is_readonly()) return -30;
    return check_mode(file, mask, false);
}
int disk_access(const fat32_file_t* file, unsigned mask, bool effective_ids) {
    if (!posix) return 0;
    if ((mask & DISK_MAY_WRITE) && ext4_is_readonly()) {
        uint32_t type = file->mode & S_IFMT;
        if (type == S_IFREG || type == S_IFDIR || type == S_IFLNK) return -30;
    }
    if (!mask) return 0;
    return check_mode(file, mask, !effective_ids);
}
int disk_may_search(const char* directory) {
    if (!posix) return 0;
    fat32_file_t dir;
    if (ext4_lookup(directory, &dir)) return 0; /* not a disk directory (/proc, /dev) */
    if (!dir.is_dir) return 0;
    return check_mode(&dir, DISK_MAY_EXEC, false);
}
static void parent_of(const char* path, char* parent) {
    strcpy(parent, path);
    unsigned end = strlen(parent);
    while (end > 1 && parent[end - 1] == '/') end--;
    while (end > 1 && parent[end - 1] != '/') end--;
    if (end > 1) end--;
    parent[end] = 0;
}
int disk_parent_access(const char* path) {
    if (!posix) return 0;
    char parent[FAT32_PATH_MAX]; parent_of(path, parent);
    fat32_file_t dir;
    int error = ext4_lookup(parent, &dir); if (error) return error == -20 ? -20 : -2;
    if (!dir.is_dir) return -20;
    if (ext4_is_readonly()) return -30;
    return check_mode(&dir, DISK_MAY_WRITE | DISK_MAY_EXEC, false);
}
int disk_delete_access(const char* path, const fat32_file_t* victim) {
    if (!posix) return 0;
    int error = disk_parent_access(path); if (error) return error;
    char parent[FAT32_PATH_MAX]; parent_of(path, parent);
    fat32_file_t dir; if (ext4_lookup(parent, &dir)) return -2;
    if ((dir.mode & S_ISVTX) && victim && fsuid() != victim->uid && fsuid() != dir.uid && !capable(CAP_FOWNER)) return -1;
    return 0;
}
int disk_open_access(const fat32_file_t* file, uint64_t flags) {
    if (!posix) return 0;
    unsigned mask = 0, access = flags & 3;
    if (access != 1) mask |= DISK_MAY_READ;
    if (access) mask |= DISK_MAY_WRITE;
    if (flags & 0x200) mask |= DISK_MAY_WRITE;
    if ((flags & 0x200000) && !(flags & 0x200)) mask = 0; /* O_PATH */
    return disk_permission(file, mask);
}
static bool pending_setid;
int disk_exec_access(const fat32_file_t* file) {
    if (!posix) return 0;
    if ((file->mode & S_IFMT) != S_IFREG) return -13;
    return check_mode(file, DISK_MAY_EXEC, false);
}
void disk_exec_arm(const fat32_file_t* file) {
    if (!posix || (file->mode & S_IFMT) != S_IFREG) return;
    bool set_uid = file->mode & S_ISUID, set_gid = (file->mode & S_ISGID) && (file->mode & 010);
    if (set_uid || set_gid) { credentials_exec_setid(set_uid, file->uid, set_gid, file->gid); pending_setid = true; }
}
void disk_exec_finished(void) { if (pending_setid) { credentials_exec_setid(false, 0, false, 0); pending_setid = false; } }
bool disk_exec_secure(const fat32_file_t* file) {   /* the set-ID bits of file would change the caller's real IDs */
    if (!posix || (file->mode & S_IFMT) != S_IFREG) return false;
    struct credentials* c = credentials_current();
    return ((file->mode & S_ISUID) && file->uid != credentials_kernel_id(c, false, 0)) ||
           ((file->mode & S_ISGID) && (file->mode & 010) && file->gid != credentials_kernel_id(c, true, 0));
}
uint64_t disk_max_file_size(void) { return posix ? EXT4_MAX_FILE_SIZE : 0xFFFFFFFFULL; }
int64_t disk_mtime_ns(const fat32_file_t* file) {
    return posix ? file->mtime : (int64_t)fat32_time_to_unix(file->wdate, file->wtime) * 1000000000LL;
}
int disk_file_stamp(fat32_file_t* file, uint64_t changed_ns) {
    if (!posix) return disk_file_touch(file);
    if (ext4_refresh(file)) return -5;
    struct ext4_attr attr = {.valid = EXT4_ATTR_MTIME | EXT4_ATTR_CTIME, .mtime = (int64_t)changed_ns,
                             .ctime = file->ctime > (int64_t)changed_ns ? file->ctime : (int64_t)changed_ns};
    return ext4_setattr(file, &attr) ? -5 : 0;
}
void disk_write_privs(fat32_file_t* file) {
    if (!posix || !(file->mode & (S_ISUID | S_ISGID)) || (file->mode & S_IFMT) != S_IFREG || capable(CAP_FSETID)) return;
    if (ext4_refresh(file)) return;
    unsigned mode = file->mode & ~S_ISUID;
    if ((file->mode & S_ISGID) && (file->mode & 010)) mode &= ~S_ISGID;
    if (mode == file->mode) return;
    struct ext4_attr attr = {.valid = EXT4_ATTR_MODE, .mode = mode & 07777};
    if (!ext4_setattr(file, &attr)) file_refresh_views(file);
}
int disk_sync(void) { return posix ? ext4_sync() : vfs_flush_devices() < 0 ? -5 : 0; }
void disk_writeback_complete(bool all, bool wrote) {
    if (posix) { if (all) ext4_sync(); else if (wrote) ext4_poll(); return; }
    if (wrote) vfs_flush_devices();
}
void disk_commit_poll(void) { if (posix) ext4_poll(); }
uint64_t disk_commit_next_event(uint64_t now) { uint64_t ext4_next_event(uint64_t now); return posix ? ext4_next_event(now) : ~0ULL; }   /* tick.c */
void disk_orphan_shutdown(void);   /* inode.c */
void boot_keymap_sync(void);   /* osinstall.c: an encrypted root's boot prompt layout */
void md_shutdown(void);   /* md.c: arrays clean on disk */
void disk_shutdown(void) { boot_keymap_sync(); if (posix) ext4_unmount_clean(); else if (fat32_is_mounted()) { disk_orphan_shutdown(); fat32_unmount_clean(); } md_shutdown(); }   /* FAT32: orphans freed, clean flag (fatcheck.inc) */
void disk_test_poweroff_after(unsigned commits) { if (posix) ext4_test_poweroff_after_commits = commits; }

/* ------------------------------------------------------------------ node creation */
int disk_create_node(const char* path, uint32_t mode, uint32_t rdev, const char* target, fat32_file_t* out) {
    uint32_t type = mode & S_IFMT;
    if (!posix) {
        int error;
        switch (type) {
        case S_IFREG: return (error = fat32_create_path(path, false, out)) ? fat_create_error(error) : 0;
        case S_IFDIR: return (error = fat32_create_path(path, true, out)) ? fat_create_error(error) : 0;
        case S_IFSOCK: return (error = fat32_create_socket_path(path, out)) ? fat_create_error(error) : 0;
        case S_IFLNK: {
            if ((error = fat32_begin_download(path, out))) return fat_create_error(error);
            unsigned bytes = strlen(target);
            int written = fat32_write_file(out, 0, bytes, target);
            if (written != (int)bytes) error = written == -2 ? -28 : -5;
            else if (disk_file_touch(out)) error = -5;
            else { int published = fat32_finish_symlink(out); error = published == -2 ? -17 : published ? -5 : 0; }
            if (error && fat32_abort_download(out)) return -5;
            return error;
        }
        default: return -1; /* vfat has no fifo or device nodes */
        }
    }
    if (type != S_IFLNK) mode = (mode & S_IFMT) | (mode & 07777 & ~filesystem_umask());
    uint32_t uid = fsuid(), gid = fsgid();
    char parent[FAT32_PATH_MAX]; parent_of(path, parent);
    fat32_file_t dir;
    if (!ext4_lookup(parent, &dir) && (dir.mode & S_ISGID)) {
        gid = dir.gid;
        if (type == S_IFDIR) mode |= S_ISGID;
    }
    if (type != S_IFDIR && (mode & S_ISGID) && !credentials_in_group(credentials_current(), gid, false) && !capable(CAP_FSETID)) mode &= ~S_ISGID;
    return ext4_create(path, mode, uid, gid, rdev, target, out);
}

/* ------------------------------------------------------------------ stat */
bool disk_stat(const fat32_file_t* file, struct memory_file* cache, struct linux_stat* st) {
    if (!posix || !file) return false;
    fat32_file_t now = *file;
    if (ext4_refresh(&now)) now = *file;
    memset(st, 0, sizeof(*st));
    st->dev = 1; st->ino = now.ino; st->nlink = now.nlink;
    st->mode = now.mode; st->uid = credentials_visible_id(now.uid, false); st->gid = credentials_visible_id(now.gid, true);
    st->rdev = now.rdev; st->size = (int64_t)now.size; st->block_size = 4096; st->blocks = (int64_t)now.blocks;
    int64_t mtime = now.mtime, ctime = now.ctime;
    if (cache) {
        st->size = (int64_t)cache->size;
        /* An orphan is never written back: its cache alone holds the current data and write time. */
        bool orphan = cache->disk.detached;
        if (cache->dirty_pages || cache->metadata_dirty || orphan) { mtime = (int64_t)cache->changed; if ((int64_t)cache->changed > ctime) ctime = (int64_t)cache->changed; }
        if (orphan && (int64_t)(cache->pages * 8) > st->blocks) st->blocks = (int64_t)(cache->pages * 8);
    }
    st->atime = now.atime / 1000000000LL; st->atime_ns = (uint64_t)(now.atime % 1000000000LL);
    st->mtime = mtime / 1000000000LL; st->mtime_ns = (uint64_t)(mtime % 1000000000LL);
    st->ctime = ctime / 1000000000LL; st->ctime_ns = (uint64_t)(ctime % 1000000000LL);
    return true;
}

int disk_seek_data(fat32_file_t* file, struct memory_file* cache, uint64_t offset, bool hole, uint64_t* result) {
    uint64_t size = cache ? cache->size : file->size;
    bool orphan = cache && cache->disk.detached;   /* never written back: the disk inode is stale */
    if (cache && !orphan && (cache->dirty_pages || cache->metadata_dirty)) {
        int error = memory_file_sync(cache, 0, ~0ULL); if (error) return error;
        *file = cache->disk;
    }
    if (offset >= size) return -6;
    if (!posix || orphan) { *result = hole ? size : offset; return 0; } /* FAT, orphans: the whole file is data */
    int error = ext4_seek_data(file, offset, hole, result);
    if (!error && *result > size) *result = size;
    return error;
}
/* swapon: runs of the file's blocks on the root volume's partition (holes refused on ext4; FAT has none). */
int disk_file_extents(fat32_file_t* file, const struct block_partition** part, int (*emit)(void* context, uint64_t offset, uint64_t sector, uint64_t sectors), void* context) {
    if (!posix) return fat32_file_extents(file, part, emit, context);
    *part = ext4_dev_partition(); if (!*part) return -22;
    return ext4_file_extents(file, emit, context);
}
void disk_statfs(uint64_t* type, uint64_t* files, uint64_t* free_files) {
    struct ext4_volume v; if (!posix || ext4_statfs(&v)) return;
    *type = 0xEF53; *files = v.files; *free_files = v.free_files;
}

/* ------------------------------------------------------------------ syscalls */
struct path_target { char path[FAT32_PATH_MAX]; struct open_file* bound; fat32_file_t file; bool disk; };
/* Resolve (dirfd, path) to a disk inode. AT_EMPTY_PATH (flags 0x1000) uses dirfd itself. */
static int target_at(uint64_t dirfd, uint64_t address, bool follow, bool empty_ok, struct path_target* t) {
    t->bound = NULL; t->disk = false;
    int error;
    if (!address) { if (!empty_ok) return -14; t->path[0] = 0; }
    else if ((error = kernel_user_path(address, t->path))) return error;
    if (!t->path[0]) {
        if (!empty_ok) return -2;
        struct open_file* f = kernel_file(dirfd); if (!f) return -9;
        fat32_file_t* disk = kernel_file_disk(f);
        t->bound = f;
        if (disk) { t->file = *disk; t->disk = true; if (posix) ext4_refresh(&t->file); }
        strcpy(t->path, fd_internal_path(f));
        return 0;
    }
    if ((error = kernel_path_at(dirfd, t->path, follow, &t->bound))) return error;
    if (t->bound) {
        fat32_file_t* disk = kernel_file_disk(t->bound);
        if (disk) { t->file = *disk; t->disk = true; if (posix) ext4_refresh(&t->file); }
        return 0;
    }
    if (proc_path(t->path) || device_lookup(t->path) >= 0) return 0;
    if (!disk_lookup(t->path, &t->file)) return -2;
    t->disk = true; return 0;
}
static int refresh_everywhere(fat32_file_t* file) { file_refresh_views(file); return 0; }
static int do_chmod(struct path_target* t, uint64_t mode) {
    if (!t->disk) return t->bound || proc_path(t->path) || device_lookup(t->path) >= 0 ? -1 : -2;
    if (!posix) {
        /* vfat: the synthesized permission bits cannot change */
        struct linux_stat st; int error = kernel_resolved_stat(t->path, NULL, false, &st);
        if (error) return error;
        if (!kernel_private_path(t->path) || fat32_is_readonly()) return -30;
        return (mode & 07777) == (st.mode & 07777) ? 0 : -1;
    }
    if (ext4_is_readonly()) return -30;
    if (!owner_or_capable(&t->file)) return -1;
    if ((t->file.mode & S_IFMT) == S_IFLNK) return -95;
    mode &= 07777;
    if ((mode & S_ISGID) && (t->file.mode & S_IFMT) != S_IFDIR && !credentials_in_group(credentials_current(), t->file.gid, false) && !capable(CAP_FSETID)) mode &= ~S_ISGID;
    struct ext4_attr attr = {.valid = EXT4_ATTR_MODE, .mode = (uint32_t)mode};
    int error = ext4_setattr(&t->file, &attr); if (error) return error;
    return refresh_everywhere(&t->file);
}
int vfs_fchmod(void* description, uint64_t mode) {
    struct open_file* f = description; fat32_file_t* disk = kernel_file_disk(f);
    if (!disk) return -1;
    struct path_target t; t.bound = f; t.disk = true; t.file = *disk; strcpy(t.path, fd_internal_path(f));
    if (posix) ext4_refresh(&t.file);
    return do_chmod(&t, mode);
}
static int do_chown(struct path_target* t, uint32_t user_uid, uint32_t user_gid) {
    if (!t->disk) return t->bound || proc_path(t->path) || device_lookup(t->path) >= 0 ? -1 : -2;
    uint32_t uid = 0xFFFFFFFFu, gid = 0xFFFFFFFFu;
    if (user_uid != 0xFFFFFFFFu && !credentials_kernel_from_visible(user_uid, false, &uid)) return -22;
    if (user_gid != 0xFFFFFFFFu && !credentials_kernel_from_visible(user_gid, true, &gid)) return -22;
    if (!posix) {
        struct linux_stat st; int error = kernel_resolved_stat(t->path, NULL, false, &st);
        if (error) return error;
        uint32_t cur_uid = 0, cur_gid = 0;
        credentials_kernel_from_visible(st.uid, false, &cur_uid); credentials_kernel_from_visible(st.gid, true, &cur_gid);
        if (!kernel_private_path(t->path) || fat32_is_readonly()) return -30;
        return (uid == 0xFFFFFFFFu || uid == cur_uid) && (gid == 0xFFFFFFFFu || gid == cur_gid) ? 0 : -1;
    }
    if (ext4_is_readonly()) return -30;
    fat32_file_t* f = &t->file;
    if (uid != 0xFFFFFFFFu && uid != f->uid && !capable(CAP_CHOWN)) return -1;
    if (gid != 0xFFFFFFFFu && gid != f->gid && !capable(CAP_CHOWN) &&
        (fsuid() != f->uid || !credentials_in_group(credentials_current(), gid, false))) return -1;
    if (uid == 0xFFFFFFFFu && gid == 0xFFFFFFFFu) {
        struct ext4_attr touch = {.valid = 0}; int error = ext4_setattr(f, &touch); if (error) return error;
        return refresh_everywhere(f);
    }
    struct ext4_attr attr = {0};
    if (uid != 0xFFFFFFFFu) { attr.valid |= EXT4_ATTR_UID; attr.uid = uid; }
    if (gid != 0xFFFFFFFFu) { attr.valid |= EXT4_ATTR_GID; attr.gid = gid; }
    if ((f->mode & S_IFMT) != S_IFDIR && (f->mode & (S_ISUID | S_ISGID))) {
        unsigned mode = f->mode & ~S_ISUID;
        if ((f->mode & S_ISGID) && (f->mode & 010)) mode &= ~S_ISGID;
        attr.valid |= EXT4_ATTR_MODE; attr.mode = mode & 07777;
    }
    int error = ext4_setattr(f, &attr); if (error) return error;
    return refresh_everywhere(f);
}
static int do_link(uint64_t olddir, uint64_t oldaddr, uint64_t newdir, uint64_t newaddr, uint64_t flags) {
    if (flags & ~0x1400ULL) return -22;
    if ((flags & 0x1000) && !capable(2)) return -2; /* AT_EMPTY_PATH needs CAP_DAC_READ_SEARCH */
    struct path_target source;
    int error = target_at(olddir, oldaddr, (flags & 0x400) != 0, (flags & 0x1000) != 0, &source); if (error) return error;
    char path[FAT32_PATH_MAX];
    if ((error = kernel_user_path(newaddr, path))) return error;
    if (!path[0]) return -2;
    struct open_file* bound;
    if ((error = kernel_path_at(newdir, path, false, &bound))) return error;
    if (!bound && (shm_path(source.path) || shm_path(path))) return source.disk ? -18 : shm_link_path(source.path, path);   /* sem_open */
    if (bound || proc_path(path) || device_lookup(path) >= 0) return -17;
    if (!source.disk) return proc_path(source.path) ? -18 : -1;
    if (!posix) return -1; /* vfat has no hard links */
    if (source.file.is_dir) return -1;
    fat32_file_t existing;
    if (!ext4_lookup(path, &existing)) return -17;
    if ((error = disk_parent_access(path))) return error;
    if ((error = ext4_link(&source.file, path))) return error;
    return refresh_everywhere(&source.file);
}
static int do_mknod(uint64_t dirfd, uint64_t address, uint64_t mode, uint64_t dev) {
    uint32_t type = (uint32_t)mode & S_IFMT;
    if (!type) type = S_IFREG;
    if (type != S_IFREG && type != S_IFCHR && type != S_IFBLK && type != S_IFIFO && type != S_IFSOCK) return -22;
    if ((type == S_IFCHR || type == S_IFBLK) && !capable(CAP_MKNOD)) return -1;
    char path[FAT32_PATH_MAX]; int error = kernel_user_path(address, path); if (error) return error;
    if (!path[0]) return -2;
    struct open_file* bound;
    if ((error = kernel_path_at(dirfd, path, false, &bound))) return error;
    if (type == S_IFBLK && !strncmp(path, "/dev/", 5) && device_lookup(path) < 0) { int storage_mknod(const char* path, uint32_t rdev); return storage_mknod(path, (uint32_t)dev); }   /* /dev is devtmpfs */
    if (bound || proc_path(path) || device_lookup(path) >= 0) return -17;
    fat32_file_t file;
    if (disk_lookup(path, &file)) return -17;
    if (!posix) {
        if (type == S_IFCHR || type == S_IFBLK || type == S_IFIFO) return -1;
        char parent[FAT32_PATH_MAX]; parent_of(path, parent);
        if (fat32_is_readonly() || !kernel_private_path(parent)) return -30;
    } else if ((error = disk_parent_access(path))) return error;
    error = disk_create_node(path, type | ((uint32_t)mode & 07777), (uint32_t)dev, NULL, &file);
    if (!error && !posix && disk_file_touch(&file)) error = -5;
    return error;
}
/* times[0] = atime, times[1] = mtime in nanoseconds; UTIME_NOW / UTIME_OMIT markers. */
#define UTIME_NOW_NS (-1LL)
#define UTIME_OMIT_NS (-2LL)
/* FAT keeps the write time as a UTC date/time with 2-second resolution (1980..2107); stamps are read back
 * through the 32-bit fat32_time_to_unix, so the newest one stored is 2106-02-07 06:28:14 (2^32 - 2). */
static void unix_to_fat(int64_t seconds, uint16_t* date, uint16_t* time) {
    if (seconds < 315532800) seconds = 315532800;
    if (seconds > 4294967294LL) seconds = 4294967294LL;
    int64_t days = seconds / 86400 + 719468, rest = seconds % 86400, era = days / 146097;   /* civil-from-days */
    unsigned doe = (unsigned)(days - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9; int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
    *date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    *time = (uint16_t)(((rest / 3600) << 11) | (((rest / 60) % 60) << 5) | ((rest % 60) / 2));
}
static int do_utimes(struct path_target* t, int64_t atime, int64_t mtime) {
    if (!t->disk) return t->bound ? 0 : proc_path(t->path) || device_lookup(t->path) >= 0 ? 0 : -2;
    if (!posix) {
        /* vfat: the write time is stored (Windows programs and archivers set it); FAT has no access time of day. */
        if (mtime == UTIME_OMIT_NS) return 0;
        if (!kernel_private_path(t->path) || fat32_is_readonly()) return -30;
        fat32_file_t* f = &t->file;
        struct memory_file* cache = f->is_dir ? NULL : memory_file_disk_find(f);
        if (cache) { int error = memory_file_sync(cache, 0, ~0ULL); if (error) return error; } /* written data no longer restamps */
        int64_t value = mtime == UTIME_NOW_NS ? (int64_t)native_realtime_ns() : mtime;
        uint16_t date, time; unix_to_fat(value < 0 ? 0 : value / 1000000000LL, &date, &time);
        if (fat32_set_times(f, date, time)) return -5;
        if (cache) { cache->disk.wdate = date; cache->disk.wtime = time; cache->changed = (uint64_t)fat32_time_to_unix(date, time) * 1000000000ULL; }
        return refresh_everywhere(f);
    }
    if (atime == UTIME_OMIT_NS && mtime == UTIME_OMIT_NS) return 0;
    if (ext4_is_readonly()) return -30;
    fat32_file_t* f = &t->file;
    bool explicit = (atime != UTIME_NOW_NS && atime != UTIME_OMIT_NS) || (mtime != UTIME_NOW_NS && mtime != UTIME_OMIT_NS);
    if (!owner_or_capable(f)) {
        if (explicit) return -1;
        if (check_mode(f, DISK_MAY_WRITE, false)) return -13;
    }
    struct memory_file* cache = f->is_dir ? NULL : memory_file_disk_find(f);
    if (cache) { int error = memory_file_sync(cache, 0, ~0ULL); if (error) return error; ext4_refresh(f); }
    int64_t now = (int64_t)native_realtime_ns();
    struct ext4_attr attr = {0};
    if (atime != UTIME_OMIT_NS) { attr.valid |= EXT4_ATTR_ATIME; attr.atime = atime == UTIME_NOW_NS ? now : atime; }
    if (mtime != UTIME_OMIT_NS) { attr.valid |= EXT4_ATTR_MTIME; attr.mtime = mtime == UTIME_NOW_NS ? now : mtime; }
    int error = ext4_setattr(f, &attr); if (error) return error;
    /* an orphan is never synced: with mtime omitted its disk mtime is stale and the cache keeps the write time */
    if (cache && (mtime != UTIME_OMIT_NS || !cache->disk.detached)) cache->changed = (uint64_t)f->mtime;
    return refresh_everywhere(f);
}
/* Seconds beyond the int64 nanosecond range (before 1678, after 2262) saturate instead of wrapping around;
 * FAT then clamps them to its own 1980..2106 range. */
static int64_t sec_to_ns(int64_t sec, int64_t ns) {
    const int64_t limit = 9223372035LL;   /* INT64_MAX / 1e9 - 1 */
    if (sec > limit) sec = limit;
    if (sec < -limit) sec = -limit;
    return sec * 1000000000LL + ns;
}
static int read_timespec(uint64_t address, int64_t* out) {
    if (!accessible(address, 32, false)) return -14;
    int64_t ts[4]; copy_from_user(ts, (const void*)address, sizeof(ts));
    for (int i = 0; i < 2; i++) {
        int64_t sec = ts[2 * i], ns = ts[2 * i + 1];
        if (ns == (1LL << 30) - 1) out[i] = UTIME_NOW_NS;
        else if (ns == (1LL << 30) - 2) out[i] = UTIME_OMIT_NS;
        else if (ns < 0 || ns > 999999999) return -22;
        else out[i] = sec_to_ns(sec, ns);
    }
    return 0;
}
static int read_timeval(uint64_t address, int64_t* out) {
    if (!accessible(address, 32, false)) return -14;
    int64_t tv[4]; copy_from_user(tv, (const void*)address, sizeof(tv));
    for (int i = 0; i < 2; i++) {
        if (tv[2 * i + 1] < 0 || tv[2 * i + 1] >= 1000000) return -22;
        out[i] = sec_to_ns(tv[2 * i], tv[2 * i + 1] * 1000);
    }
    return 0;
}
struct statx_ts { int64_t sec; uint32_t nsec; int32_t reserved; };
struct statx64 {
    uint32_t mask, blksize; uint64_t attributes; uint32_t nlink, uid, gid; uint16_t mode, spare0;
    uint64_t ino, size, blocks, attributes_mask;
    struct statx_ts atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t mnt_id; uint32_t dio_mem_align, dio_offset_align; uint64_t spare[12];
};
_Static_assert(sizeof(struct statx64) == 256 && __builtin_offsetof(struct statx64, atime) == 0x40 &&
               __builtin_offsetof(struct statx64, rdev_major) == 0x80 && __builtin_offsetof(struct statx64, mnt_id) == 0x90, "statx ABI");
static void ts_set(struct statx_ts* ts, int64_t sec, uint64_t ns) { ts->sec = sec; ts->nsec = (uint32_t)ns; ts->reserved = 0; }
static int do_statx(uint64_t dirfd, uint64_t address, uint64_t flags, uint64_t mask, uint64_t output) {
    if (flags & ~(0x100ULL | 0x800ULL | 0x1000ULL | 0x6000ULL) || (flags & 0x6000) == 0x6000 || (mask & 0x80000000ULL)) return -22;
    if (!accessible(output, sizeof(struct statx64), true)) return -14;
    struct path_target t;
    int error = target_at(dirfd, address, !(flags & 0x100), (flags & 0x1000) != 0, &t); if (error) return error;
    struct linux_stat st;
    if (t.bound) { if ((error = fd_description_stat(t.bound, &st))) return error; }
    else if ((error = kernel_resolved_stat(t.path, NULL, !(flags & 0x100), &st))) return error;
    struct statx64 x; memset(&x, 0, sizeof(x));
    x.mask = 0x7FF; x.blksize = (uint32_t)st.block_size; x.nlink = (uint32_t)st.nlink; x.uid = st.uid; x.gid = st.gid;
    x.mode = (uint16_t)st.mode; x.ino = st.ino; x.size = (uint64_t)st.size; x.blocks = (uint64_t)st.blocks;
    ts_set(&x.atime, st.atime, st.atime_ns); ts_set(&x.mtime, st.mtime, st.mtime_ns); ts_set(&x.ctime, st.ctime, st.ctime_ns);
    x.rdev_major = (uint32_t)(((st.rdev >> 8) & 0xFFF) | ((st.rdev >> 32) & ~0xFFFULL));
    x.rdev_minor = (uint32_t)((st.rdev & 0xFF) | ((st.rdev >> 12) & ~0xFFULL));
    x.dev_major = (uint32_t)(((st.dev >> 8) & 0xFFF) | ((st.dev >> 32) & ~0xFFFULL));
    x.dev_minor = (uint32_t)((st.dev & 0xFF) | ((st.dev >> 12) & ~0xFFULL));
    x.mnt_id = st.dev; x.mask |= 0x1000;
    if (t.disk && posix) {
        if (t.file.btime) { ts_set(&x.btime, t.file.btime / 1000000000LL, (uint64_t)(t.file.btime % 1000000000LL)); x.mask |= 0x800; }
        x.attributes_mask = 0x10 | 0x20 | 0x40 | 0x4 | 0x800;
        if (t.file.iflags & 0x10) x.attributes |= 0x10;   /* immutable */
        if (t.file.iflags & 0x20) x.attributes |= 0x20;   /* append */
        if (t.file.iflags & 0x40) x.attributes |= 0x40;   /* nodump */
        if (t.file.iflags & 0x4) x.attributes |= 0x4;     /* compressed */
        if (t.file.iflags & 0x800) x.attributes |= 0x800; /* encrypted */
    }
    return copy_to_user((void*)output, &x, sizeof(x)) ? -14 : 0;
}
static int do_access(uint64_t dirfd, uint64_t address, uint64_t mode, uint64_t flags) {
    if (mode & ~7ULL) return -22;
    if (flags & ~(0x100ULL | 0x200ULL | 0x1000ULL)) return -22;
    struct path_target t;
    int error = target_at(dirfd, address, !(flags & 0x100), (flags & 0x1000) != 0, &t); if (error) return error;
    if (!t.disk || !posix) {
        struct linux_stat st;
        if (t.bound) error = fd_description_stat(t.bound, &st); else error = kernel_resolved_stat(t.path, NULL, !(flags & 0x100), &st);
        if (error) return error;
        if (!posix && t.disk) {
            if ((mode & 2) && (fat32_is_readonly() || !kernel_private_path(t.path))) return -30;
            if ((mode & 1) && !(st.mode & 0111)) return -13;
            return 0;
        }
        if (((mode & 2) && !(st.mode & 0222)) || ((mode & 1) && !(st.mode & 0111)) || ((mode & 4) && !(st.mode & 0444))) return -13;
        return 0;
    }
    unsigned mask = (mode & 4 ? DISK_MAY_READ : 0) | (mode & 2 ? DISK_MAY_WRITE : 0) | (mode & 1 ? DISK_MAY_EXEC : 0);
    return disk_access(&t.file, mask, (flags & 0x200) != 0);
}
/* fallocate on a disk file: the page cache is written back first, then refreshed. */
static int do_fallocate(uint64_t fd, uint64_t mode, int64_t offset, int64_t length) {
    struct open_file* f = kernel_file(fd); if (!f) return -9;
    fat32_file_t* disk = kernel_file_disk(f);
    if (!disk) return kernel_file_memory(f) ? -95 : -19;
    if (offset < 0 || length <= 0) return -22;
    if (disk->is_dir) return -21;
    if (!posix) return -95;
    struct memory_file* cache = kernel_file_memory(f);
    if (cache) { int error = memory_file_sync(cache, 0, ~0ULL); if (error) return error; *disk = cache->disk; }
    /* An orphan is never written back, so a cut made in its cache alone (O_TRUNC, ftruncate after unlink) still
     * leaves the old size and data on disk: drop them first, or growing the file below would expose them. */
    if (cache && disk->detached && cache->valid_size < disk->size) {
        int error = ext4_truncate(disk, cache->valid_size); if (error) return error;
        cache->disk = *disk;
    }
    int error = ext4_fallocate(disk, (unsigned)mode, (uint64_t)offset, (uint64_t)length);
    if (error) return error;
    if (cache) {
        cache->disk = *disk;
        if (mode & 0x12) { /* punched/zeroed range reads as zeros through every mapping */
            cache->changed = (uint64_t)disk->mtime;   /* ext4 stamped the modification; an orphan's stat reads it here */
            uint64_t end = (uint64_t)offset + (uint64_t)length;
            for (uint64_t at = (uint64_t)offset & ~4095ULL; at < end && at < cache->size; at += 4096) {
                uint64_t frame = memory_file_page(cache, at / 4096, false); if (!frame) continue;
                uint64_t from = at < (uint64_t)offset ? (uint64_t)offset - at : 0, to = end - at < 4096 ? end - at : 4096;
                memset((uint8_t*)phys_to_virt(frame) + from, 0, to - from);
            }
        }
        if (disk->size > cache->size) { cache->size = disk->size; cache->valid_size = disk->size; }
    }
    file_refresh_views(disk);
    return 0;
}
int64_t vfs_call(struct registers* r) {
    struct path_target t; int error; int64_t times[2];
    switch (r->rax) {
    case 90: case 268: case 452: {
        uint64_t dirfd = r->rax == 90 ? (uint64_t)-100 : r->rdi, path = r->rax == 90 ? r->rdi : r->rsi, mode = r->rax == 90 ? r->rsi : r->rdx;
        uint64_t flags = r->rax == 452 ? r->r10 : 0;
        if (flags & ~(0x100ULL | 0x1000ULL)) return -22;
        if ((error = target_at(dirfd, path, !(flags & 0x100), (flags & 0x1000) != 0, &t))) return error;
        if ((flags & 0x100) && t.disk && t.file.native_kind == 2) return -95;
        return do_chmod(&t, mode);
    }
    case 92: case 94: case 260: {
        uint64_t dirfd = r->rax == 260 ? r->rdi : (uint64_t)-100, path = r->rax == 260 ? r->rsi : r->rdi;
        uint64_t uid = r->rax == 260 ? r->rdx : r->rsi, gid = r->rax == 260 ? r->r10 : r->rdx;
        uint64_t flags = r->rax == 260 ? r->r8 : r->rax == 94 ? 0x100 : 0;
        if (flags & ~(0x100ULL | 0x1000ULL)) return -22;
        if ((error = target_at(dirfd, path, !(flags & 0x100), (flags & 0x1000) != 0, &t))) return error;
        return do_chown(&t, (uint32_t)uid, (uint32_t)gid);
    }
    case 93: {
        struct open_file* f = kernel_file(r->rdi); if (!f) return -9;
        fat32_file_t* disk = kernel_file_disk(f);
        if (!disk) return kernel_file_memory(f) ? 0 : -1;
        t.bound = f; t.disk = true; t.file = *disk; strcpy(t.path, fd_internal_path(f));
        if (posix) ext4_refresh(&t.file);
        return do_chown(&t, (uint32_t)r->rsi, (uint32_t)r->rdx);
    }
    case 86: return do_link((uint64_t)-100, r->rdi, (uint64_t)-100, r->rsi, 0);
    case 265: return do_link(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case 133: return do_mknod((uint64_t)-100, r->rdi, r->rsi, r->rdx);
    case 259: return do_mknod(r->rdi, r->rsi, r->rdx, r->r10);
    case 132: case 235: case 261: {
        uint64_t dirfd = r->rax == 261 ? r->rdi : (uint64_t)-100, path = r->rax == 261 ? r->rsi : r->rdi, value = r->rax == 261 ? r->rdx : r->rsi;
        if (!value) times[0] = times[1] = UTIME_NOW_NS;
        else if (r->rax == 132) {
            if (!accessible(value, 16, false)) return -14;
            times[0] = sec_to_ns((int64_t)uget64(value), 0); times[1] = sec_to_ns((int64_t)uget64(value + 8), 0);
        } else if ((error = read_timeval(value, times))) return error;
        if ((error = target_at(dirfd, path, true, false, &t))) return error;
        return do_utimes(&t, times[0], times[1]);
    }
    case 280: {
        uint64_t flags = r->r10;
        if (flags & ~(0x100ULL | 0x1000ULL)) return -22;
        if (r->rdx) { if ((error = read_timespec(r->rdx, times))) return error; }
        else times[0] = times[1] = UTIME_NOW_NS;
        if ((error = target_at(r->rdi, r->rsi, !(flags & 0x100), !r->rsi || (flags & 0x1000), &t))) return error;
        return do_utimes(&t, times[0], times[1]);
    }
    case 332: return do_statx(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case 269: return do_access(r->rdi, r->rsi, r->rdx, 0);
    case 439: return do_access(r->rdi, r->rsi, r->rdx, r->r10);
    case 76: {
        char path[FAT32_PATH_MAX];
        if ((int64_t)r->rsi < 0) return -22;
        if ((error = kernel_user_path(r->rdi, path))) return error;
        return kernel_truncate_path(path, r->rsi);
    }
    case 95: return filesystem_exchange_umask((unsigned)r->rdi & 0777);
    case 285: return do_fallocate(r->rdi, r->rsi, (int64_t)r->rdx, (int64_t)r->r10);
    default: return -38;
    }
}
