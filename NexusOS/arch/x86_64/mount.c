/* Mounted filesystems beside the root: see mount.h.
 *
 * Routing: vfs.h redirects the kernel's fat32_ and disk_ calls (outside vfs.c and this file) to the mnt_* functions
 * below. A path is matched against the attached mount points (longest prefix, the latest mount on a point wins);
 * a handle carries its mount id. The root filesystem keeps its own code (vfs.c: FAT32 or ext4) untouched.
 *
 * Policy: mount(2) needs CAP_SYS_ADMIN, except two desktop cases a udisks/fusermount setup would allow the
 * active session user: a hot-plugged USB block device mounted nosuid,nodev at /media/<user>/<name> (the
 * directory is created by the kernel when missing, as udisks does, and removed at unmount), and FUSE mounts
 * with nosuid,nodev, user_id = the caller, on a directory the caller may write. Such mounts record their owner,
 * who may unmount them. umount2 honours MNT_DETACH (lazy: unreachable at once, released when no file, cwd or
 * mapping uses it), MNT_FORCE (FUSE: the connection is aborted) and EBUSY otherwise. MS_REMOUNT switches
 * ro/rw and the per-mount flags, MS_BIND makes a directory visible elsewhere, MS_MOVE moves a mount,
 * propagation flags are accepted (one namespace, no propagation). */
#define MOUNT_IMPLEMENTATION
#include "mount.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

int kernel_user_path(uint64_t address, char* path);
int kernel_path_at(uint64_t directory, char* path, bool follow, struct open_file** bound);
struct open_file* kernel_file(uint64_t fd);
fat32_file_t* kernel_file_disk(struct open_file* file);
struct memory_file* kernel_file_memory(struct open_file* file);
int kernel_resolved_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* st);
unsigned filesystem_umask(void);
bool credentials_in_group(struct credentials* credentials, uint32_t gid, bool real);
bool credentials_access_capable(struct credentials* credentials, unsigned capability);
bool credentials_kernel_from_visible(uint32_t id, bool group, uint32_t* result);
void credentials_exec_setid(bool set_uid, uint32_t uid, bool set_gid, uint32_t gid);
bool kernel_mount_busy(unsigned id);
void memory_file_forget_mount(unsigned id);
struct block_partition* storage_partition(unsigned device);
bool boot_option(const char* word);
int fsprobe_resolve(const char* spec, char* device, unsigned capacity);   /* fsprobe.c: UUID=/LABEL=/PARTUUID= */
int fsprobe_type(struct block_partition* part, char* type, unsigned capacity);

#define CAP_CHOWN 0
#define CAP_DAC_OVERRIDE 1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER 3
#define CAP_FSETID 4
#define CAP_MKNOD 27
#define CAP_SYS_ADMIN 21
#define S_IFMT 0170000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFBLK 0060000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_IFSOCK 0140000
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define DESKTOP_UID 1000
#define DESKTOP_USER "nexus"
#define MAX_MOUNTS 64

static struct mount* mounts;
static unsigned mount_total, next_id = 2, anonymous_minor = 40;
unsigned mount_count(void) { return mount_total; }
uint32_t mount_current_uid(void) { return credentials_kernel_id(credentials_current(), false, 3); }
uint32_t mount_current_gid(void) { return credentials_kernel_id(credentials_current(), true, 3); }
/* A driver's inode number: ino, with generation as its upper 32 bits (FUSE node ids, large FAT slots). */
uint64_t mount_identity(const fat32_file_t* f) { uint64_t id = (uint64_t)f->generation << 32 | f->ino; return id ? id : 1; }
static bool capable(unsigned capability) { return credentials_capable(credentials_current(), NULL, capability); }

/* ---- table ---- */
static bool prefix_of(const struct mount* m, const char* path) {
    if (m->path_length == 1) return path[0] == '/';
    return !strncmp(path, m->path, m->path_length) && (!path[m->path_length] || path[m->path_length] == '/');
}
struct mount* mount_find(const char* path, const char** rest) {
    if (!mounts || !path || path[0] != '/') return NULL;
    struct mount* best = NULL;
    for (struct mount* m = mounts; m; m = m->next)
        if (!m->detached && prefix_of(m, path) && (!best || m->path_length >= best->path_length)) best = m;
    if (best && rest) { const char* r = path + best->path_length; while (*r == '/') r++; *rest = r; }
    return best;
}
struct mount* mount_by_id(unsigned id) {
    for (struct mount* m = mounts; m; m = m->next) if (m->id == id) return m;
    return NULL;
}
/* A mount whose device is a suspended dm device (dmsetup suspend, cryptsetup luksSuspend) is frozen: new operations
 * wait here, before any driver state is touched, until the device resumes -- the freeze Linux does before a dm
 * suspend. Operations cannot be half done at the suspend: the suspend ioctl runs under the big lock past the I/O
 * barrier. Contexts that cannot sleep go on (dm_rw then fails their I/O and write-back keeps its dirty data). */
static int thaw_channel;
static bool filesystem_of_frozen(const struct mount* m) { return m->frozen || (m->bound && m->bound->frozen); }
void kernel_io_sleep(void* channel, uint64_t deadline);
unsigned spin_held_count(void);
static bool may_wait(void) { return cpu_self()->task && kernel_lock_held() && spin_held_count() == (cpu_self()->vm_held ? 1u : 0u); }
static struct mount* thawed_by_id(unsigned id) {   /* mount_by_id after any freeze; looked up again after sleeping */
    struct mount* m = mount_by_id(id);
    while (m && filesystem_of_frozen(m) && may_wait()) { kernel_io_sleep(&thaw_channel, native_clock_ns() + 20000000ULL); m = mount_by_id(id); }
    return m;
}
void mount_freeze_device(struct block_device* dev, bool freeze) {
    for (struct mount* m = mounts; m; m = m->next) if (m->part && m->part->dev == dev) m->frozen = freeze;
    if (!freeze) task_wake_channel(&thaw_channel);
}
static struct mount* filesystem_of(struct mount* m) { return m && m->bound ? m->bound : m; }   /* bind mounts share the source's driver */
bool mount_path(const char* path) { struct mount* m = mount_find(path, NULL); return m && (m->ops || m->bound); }
static bool readonly(const struct mount* m) {
    return (m->flags & MS_RDONLY) || m->dead || (m->part && m->part->dev->read_only) || (m->bound && readonly(m->bound));
}
bool disk_file_readonly(const fat32_file_t* file) {
    if (!file->mount) return disk_is_readonly();
    struct mount* m = mount_by_id(file->mount);
    return !m || readonly(m);
}
uint64_t disk_file_max_size(const fat32_file_t* file) {
    if (!file->mount) return disk_max_file_size();
    struct mount* m = filesystem_of(mount_by_id(file->mount));
    return m && m->ops && m->ops->max_file_size ? m->ops->max_file_size : 0xFFFFFFFFULL;
}

/* ---- path translation for bind mounts ----
 * A mount resolves a path to (filesystem mount or NULL for the root fs, path below that filesystem's root). */
struct target { struct mount* m; struct mount* fs; char rest[FAT32_PATH_MAX]; char full[FAT32_PATH_MAX]; };
static bool translate(const char* path, struct target* t) {
    const char* rest = NULL; t->m = mount_find(path, &rest); t->fs = NULL; t->rest[0] = 0;
    while (t->m && filesystem_of_frozen(t->m) && may_wait()) {   /* a suspended dm device below: wait, then resolve again */
        kernel_io_sleep(&thaw_channel, native_clock_ns() + 20000000ULL); t->m = mount_find(path, &rest);
    }
    if (!t->m) return false;
    char tail[FAT32_PATH_MAX]; strcpy(tail, rest);
    struct mount* m = t->m;
    if (m->bound || !m->ops) {   /* bind: prefix the source directory */
        if (strlen(m->subdir) + 1 + strlen(tail) >= FAT32_PATH_MAX) return false;
        strcpy(t->rest, m->subdir);
        if (tail[0]) { if (t->rest[0]) strcat(t->rest, "/"); strcat(t->rest, tail); }
        t->fs = m->bound;
        if (!t->fs) { strcpy(t->full, "/"); strcat(t->full, t->rest); }   /* a root-filesystem directory */
        return true;
    }
    t->fs = m; strcpy(t->rest, tail);
    return true;
}
/* After a driver produced a handle: identity, mount id, root naming. */
static uint8_t kind_of(uint32_t mode) {   /* the kernel's native kinds (ext4 mode_kind): symlinks, sockets, fifos, devices */
    switch (mode & 0170000) { case 0140000: return 1; case 0120000: return 2; case 0010000: return 3; case 0020000: return 4; case 0060000: return 5; default: return 0; }
}
static void stamp(struct mount* view, struct mount* fs, fat32_file_t* f, bool root) {
    f->mount = fs->id; f->native_kind = kind_of(f->mode);
    if (!f->dir_lba) f->dir_lba = MOUNT_ROOT_LBA;
    f->native_ino = MOUNT_INO(fs, mount_identity(f));
    if (root && view) { strncpy(f->name, view->root_name, FAT32_LFN_MAX); f->name[FAT32_LFN_MAX] = 0; }
}
static int fs_lookup(struct mount* view, struct mount* fs, const char* rest, fat32_file_t* out) {
    if (fs->dead) return fs->ops && (fs->ops->flags & MOUNT_FUSE) ? -107 : -5;   /* a dead FUSE daemon: ENOTCONN */
    memset(out, 0, sizeof(*out));
    int error = fs->ops->lookup(fs, rest, out);
    if (error) return error;
    stamp(view, fs, out, !rest[0] || (view && view->bound && !strcmp(rest, view->subdir)));
    return 0;
}

/* ---- fat32_* / disk_* routing ---- */
bool mnt_lookup(const char* path, fat32_file_t* out) {
    struct target t;
    if (!translate(path, &t)) { bool found = disk_lookup(path, out); if (found) out->mount = 0; return found; }
    if (!t.fs) {
        bool found = disk_lookup(t.full, out); if (!found) return false;
        out->mount = 0;
        if (!t.rest[0] || !strcmp(t.rest, t.m->subdir)) { strncpy(out->name, t.m->root_name, FAT32_LFN_MAX); out->name[FAT32_LFN_MAX] = 0; }
        return true;
    }
    return fs_lookup(t.m, t.fs, t.rest, out) == 0;
}
int mnt_lookup_status(const char* path, fat32_file_t* out) {
    struct target t;
    if (!translate(path, &t)) { int r = disk_lookup_status(path, out); if (!r) out->mount = 0; return r; }
    if (!t.fs) { int r = disk_lookup_status(t.full, out); if (!r) { out->mount = 0; if (!t.rest[0]) strcpy(out->name, t.m->root_name); } return r; }
    int error = fs_lookup(t.m, t.fs, t.rest, out);
    return !error ? 0 : error == -2 || error == -20 || error == -36 ? -2 : error == -107 ? -107 : -5;
}
int mnt_refresh_file(fat32_file_t* file) {
    if (!file->mount) return disk_refresh_file(file);
    struct mount* m = thawed_by_id(file->mount);
    if (!m || m->dead) return -1;
    if (file->detached || !m->ops->refresh) return 0;
    char name[FAT32_LFN_MAX + 1]; strcpy(name, file->name);
    int error = m->ops->refresh(m, file);
    if (!error) { file->mount = m->id; file->native_kind = kind_of(file->mode); file->native_ino = MOUNT_INO(m, mount_identity(file)); if (file->dir_lba == MOUNT_ROOT_LBA) strcpy(file->name, name); }
    return error ? -1 : 0;
}
int32_t mnt_read_file(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer) {
    if (!file->mount) return disk_read_file(file, offset, length, buffer);
    struct mount* m = thawed_by_id(file->mount);
    if (!m || m->dead) return -1;
    if (length > 0x7FFFFFFFULL) length = 0x7FFFFFFFULL;
    int64_t r = m->ops->read(m, file, offset, length, buffer);
    file->mount = m->id;
    return r < 0 ? -1 : (int32_t)r;
}
static struct { unsigned mount; uint64_t ino, next_index, cookie, used; } dir_cursors[16];
static uint64_t dir_clock;
int mnt_read_dir_at(const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) {
    if (!dir->mount) { int r = disk_read_dir_at(dir, cookie, out); if (r > 0) out->mount = 0; return r; }
    struct mount* m = thawed_by_id(dir->mount);
    if (!m || m->dead) return -1;
    memset(out, 0, sizeof(*out));
    int r = m->ops->readdir(m, dir, cookie, out);
    if (r > 0) stamp(NULL, m, out, false);
    return r < 0 ? -1 : r;
}
int mnt_read_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out) {
    if (!dir->mount) { int r = disk_read_dir_entry(dir, index, out); if (r > 0) out->mount = 0; return r; }
    unsigned pick = 0; bool found = false;
    for (unsigned i = 0; i < 16; i++)
        if (dir_cursors[i].mount == dir->mount && dir_cursors[i].ino == dir->ino && dir_cursors[i].next_index <= index &&
            (!found || dir_cursors[i].next_index > dir_cursors[pick].next_index)) { pick = i; found = true; }
    if (!found) {
        for (unsigned i = 1; i < 16; i++) if (dir_cursors[i].used < dir_cursors[pick].used) pick = i;
        dir_cursors[pick].mount = dir->mount; dir_cursors[pick].ino = dir->ino; dir_cursors[pick].next_index = 0; dir_cursors[pick].cookie = 0;
    }
    dir_cursors[pick].used = ++dir_clock;
    fat32_file_t skip;
    while (dir_cursors[pick].next_index < index) {
        int r = mnt_read_dir_at(dir, &dir_cursors[pick].cookie, &skip);
        if (r <= 0) { dir_cursors[pick].mount = 0; return r; }
        dir_cursors[pick].next_index++;
    }
    int r = mnt_read_dir_at(dir, &dir_cursors[pick].cookie, out);
    if (r == 1) dir_cursors[pick].next_index++; else dir_cursors[pick].mount = 0;
    return r;
}
static int create_in(const char* path, uint32_t mode, uint32_t rdev, const char* link_target, fat32_file_t* out, bool posix_ids) {
    struct target t;
    if (!translate(path, &t)) return 1;   /* not mine */
    if (!t.fs) return 2;                  /* bind of a root directory: the caller uses the root with t.full */
    if (readonly(t.fs) || readonly(t.m)) return -30;
    if (!t.rest[0]) return -17;
    uint32_t uid = mount_current_uid(), gid = mount_current_gid();
    if (posix_ids && (mode & S_IFMT) != S_IFLNK) mode = (mode & S_IFMT) | (mode & 07777 & ~filesystem_umask());
    /* setgid directories pass their group on (POSIX filesystems) */
    if (t.fs->ops->flags & MOUNT_POSIX) {
        char parent[FAT32_PATH_MAX]; strcpy(parent, t.rest); char* slash = s_strrchr(parent, '/');
        if (slash) *slash = 0; else parent[0] = 0;
        fat32_file_t dir;
        if (!fs_lookup(NULL, t.fs, parent, &dir) && (dir.mode & S_ISGID)) { gid = dir.gid; if ((mode & S_IFMT) == S_IFDIR) mode |= S_ISGID; }
    }
    if (t.fs->dead) return -5;
    memset(out, 0, sizeof(*out));
    int error = t.fs->ops->create(t.fs, t.rest, mode, uid, gid, rdev, link_target, out);
    if (!error) stamp(t.m, t.fs, out, false);
    return error;
}
static void root_target(const char* path, char* full) { struct target t; if (translate(path, &t) && !t.fs) strcpy(full, t.full); else strcpy(full, path); }
static int fat_create_code(int error) { return !error ? 0 : error == -17 ? -2 : error == -2 || error == -20 ? -3 : error == -28 ? -4 : -1; }
int mnt_create_path(const char* path, bool is_dir, fat32_file_t* out) {
    int r = create_in(path, is_dir ? S_IFDIR | 0777 : S_IFREG | 0666, 0, NULL, out, true);
    if (r == 1 || r == 2) { char full[FAT32_PATH_MAX]; root_target(path, full); int e = disk_create_path(full, is_dir, out); if (!e) out->mount = 0; return e; }
    return fat_create_code(r);
}
int mnt_create_socket_path(const char* path, fat32_file_t* out) {
    int r = create_in(path, S_IFSOCK | 0777, 0, NULL, out, true);
    if (r == 1 || r == 2) { char full[FAT32_PATH_MAX]; root_target(path, full); int e = disk_create_socket_path(full, out); if (!e) out->mount = 0; return e; }
    return fat_create_code(r);
}
int mnt_create_node(const char* path, uint32_t mode, uint32_t rdev, const char* target, fat32_file_t* out) {
    struct target t;
    if (translate(path, &t) && t.fs && (t.m->flags & MS_NODEV) && ((mode & S_IFMT) == S_IFCHR || (mode & S_IFMT) == S_IFBLK)) {
        /* nodev: nodes may be created (Linux allows mknod), they just cannot be opened as devices */
    }
    int r = create_in(path, mode, rdev, target, out, true);
    if (r == 1 || r == 2) { char full[FAT32_PATH_MAX]; root_target(path, full); int e = disk_create_node(full, mode, rdev, target, out); if (!e) out->mount = 0; return e; }
    return r;
}
int mnt_write_file(fat32_file_t* file, uint64_t offset, uint64_t length, const void* buffer) {
    if (!file->mount) return disk_write_file(file, offset, length, buffer);
    struct mount* m = thawed_by_id(file->mount);
    if (!m || m->dead) return -1;
    if (readonly(m)) return -1;
    if (length > 0x7FFFFFFFULL) return -1;
    int64_t r = m->ops->write(m, file, offset, length, buffer);
    file->mount = m->id;
    return r == -28 ? -2 : r < 0 ? -1 : (int)r;
}
int mnt_truncate_file(fat32_file_t* file, uint64_t size) {
    if (!file->mount) return disk_truncate_file(file, size);
    struct mount* m = thawed_by_id(file->mount);
    if (!m || m->dead || readonly(m)) return -1;
    int r = m->ops->truncate(m, file, size);
    file->mount = m->id;
    return r == -28 ? -2 : r ? -1 : 0;
}
static int unlink_in(const char* path, int directory, fat32_file_t* victim) {
    struct target t;
    if (!translate(path, &t)) return 1;
    if (!t.fs) return 2;
    if (!t.rest[0] || (t.m->bound && !strcmp(t.rest, t.m->subdir))) return -16;   /* the mount root itself */
    if (readonly(t.fs) || readonly(t.m)) return -30;
    if (t.fs->dead) return -5;
    memset(victim, 0, sizeof(*victim));
    int error = t.fs->ops->unlink(t.fs, t.rest, directory, victim);
    if (!error) stamp(NULL, t.fs, victim, false);
    return error;
}
int mnt_remove_path(const char* path) {
    fat32_file_t victim;
    int r = unlink_in(path, -1, &victim);
    if (r == 1 || r == 2) { char full[FAT32_PATH_MAX]; root_target(path, full); return disk_remove_path(full); }
    if (r) return r == -2 ? -1 : r == -39 ? -3 : r == -16 ? -2 : -1;
    if (victim.detached) { struct mount* m = mount_by_id(victim.mount); if (m && m->ops->release) m->ops->release(m, &victim); }
    return 0;
}
int mnt_detach_path(const char* path, fat32_file_t* detached) {
    int r = unlink_in(path, -1, detached);
    if (r == 1 || r == 2) { char full[FAT32_PATH_MAX]; root_target(path, full); int e = disk_detach_path(full, detached); if (!e) detached->mount = 0; return e; }
    return r;
}
int mnt_dispose_file(fat32_file_t* file) {
    if (!file->mount) return disk_dispose_file(file);
    struct mount* m = thawed_by_id(file->mount);
    if (!m) return 0;                 /* the filesystem is gone: nothing left to free */
    if (m->dead || !m->ops->release) return 0;
    return m->ops->release(m, file) < 0 ? -1 : 0;
}
int mnt_rename_path(const char* from, const char* to, bool no_replace, fat32_file_t* moved, fat32_file_t* replaced) {
    struct target a, b; bool am = translate(from, &a), bm = translate(to, &b);
    if (!am && !bm) { int e = disk_rename_path(from, to, no_replace, moved, replaced); if (!e) { moved->mount = 0; replaced->mount = 0; } return e; }
    struct mount* fa = am ? a.fs : NULL; struct mount* fb = bm ? b.fs : NULL;
    if (fa != fb || (am && !bm) || (!am && bm)) {
        if (am && bm && !fa && !fb) {   /* both inside root-directory binds */
            int e = disk_rename_path(a.full, b.full, no_replace, moved, replaced);
            if (!e) { moved->mount = 0; replaced->mount = 0; }
            return e;
        }
        return -18;                   /* EXDEV across filesystems */
    }
    if (!fa) {
        int e = disk_rename_path(a.full, b.full, no_replace, moved, replaced);
        if (!e) { moved->mount = 0; replaced->mount = 0; }
        return e;
    }
    if (!a.rest[0] || !b.rest[0]) return -16;
    if (readonly(fa) || readonly(a.m) || readonly(b.m)) return -30;
    if (fa->dead) return -5;
    memset(moved, 0, sizeof(*moved)); memset(replaced, 0, sizeof(*replaced));
    int e = fa->ops->rename(fa, a.rest, b.rest, no_replace ? 1u : 0u, moved, replaced);
    if (!e) { stamp(NULL, fa, moved, false); if (replaced->ino || replaced->detached) stamp(NULL, fa, replaced, false); else replaced->native_ino = 0; }
    return e;
}
int mnt_begin_download(const char* path, fat32_file_t* out) {
    if (mount_path(path)) return -1;    /* staged downloads target the root filesystem */
    int e = disk_begin_download(path, out); if (!e) out->mount = 0; return e;
}
int mnt_finish_download(fat32_file_t* file) { return file->mount ? -1 : disk_finish_download(file); }
int mnt_finish_symlink(fat32_file_t* file) { return file->mount ? -1 : disk_finish_symlink(file); }
int mnt_abort_download(fat32_file_t* file) { return file->mount ? -1 : disk_abort_download(file); }
static int setattr_file(fat32_file_t* file, const struct mount_attr* attr) {
    struct mount* m = thawed_by_id(file->mount);
    if (!m || m->dead) return -5;
    if (readonly(m)) return -30;
    if (!m->ops->setattr) return -1;
    int e = m->ops->setattr(m, file, attr);
    file->mount = m->id;   /* drivers rebuild handles (refresh): the mount id is the mount layer's */
    if (!e) file->native_ino = MOUNT_INO(m, mount_identity(file));
    return e;
}
static int64_t fat_time_ns(uint16_t date, uint16_t time) { return (int64_t)fat32_time_to_unix(date, time) * 1000000000LL; }
int mnt_set_times(fat32_file_t* file, uint16_t date, uint16_t time) {
    if (!file->mount) return disk_set_times(file, date, time);
    struct mount_attr attr = {.valid = EXT4_ATTR_MTIME | EXT4_ATTR_CTIME, .mtime = fat_time_ns(date, time), .ctime = (int64_t)native_realtime_ns()};
    return setattr_file(file, &attr) ? -1 : 0;
}
int64_t mnt_mtime_ns(const fat32_file_t* file) { return file->mount ? file->mtime : disk_mtime_ns(file); }
int mnt_file_stamp(fat32_file_t* file, uint64_t changed_ns) {
    if (!file->mount) return disk_file_stamp(file, changed_ns);
    if (file->detached) return 0;
    struct mount_attr attr = {.valid = EXT4_ATTR_MTIME | EXT4_ATTR_CTIME, .mtime = (int64_t)changed_ns,
                              .ctime = file->ctime > (int64_t)changed_ns ? file->ctime : (int64_t)changed_ns};
    return setattr_file(file, &attr) ? -5 : 0;
}
void mnt_write_privs(fat32_file_t* file) {
    if (!file->mount) { disk_write_privs(file); return; }
    struct mount* m = mount_by_id(file->mount);
    if (!m || !(m->ops->flags & MOUNT_POSIX) || !(file->mode & (S_ISUID | S_ISGID)) || (file->mode & S_IFMT) != S_IFREG || capable(CAP_FSETID)) return;
    unsigned mode = file->mode & ~S_ISUID;
    if ((file->mode & S_ISGID) && (file->mode & 010)) mode &= ~S_ISGID;
    if (mode == file->mode) return;
    struct mount_attr attr = {.valid = EXT4_ATTR_MODE, .mode = mode & 07777};
    if (!setattr_file(file, &attr)) file_refresh_views(file);
}

/* ---- permissions ---- */
static int mode_check(const struct mount* m, const fat32_file_t* f, unsigned mask, bool real_ids) {
    struct credentials* c = credentials_current();
    uint32_t uid = credentials_kernel_id(c, false, real_ids ? 0 : 3);
    if ((m->ops->flags & MOUNT_FUSE) && !(m->fsflags & MOUNT_F_ALLOW_OTHER) && m->owner != ~0U && uid != m->owner) return -13;   /* fuse: user_id only, root included (Linux) */
    if ((m->ops->flags & MOUNT_FUSE) && !(m->fsflags & MOUNT_F_DEFAULT_PERMISSIONS)) return 0;   /* the daemon decides */
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
int mnt_permission(const fat32_file_t* file, unsigned mask) {
    if (!file || !file->mount) return disk_permission(file, mask);
    struct mount* m = mount_by_id(file->mount);
    if (!m) return -5;
    if ((mask & DISK_MAY_WRITE) && readonly(m)) return -30;
    return mode_check(m, file, mask, false);
}
int mnt_access(const fat32_file_t* file, unsigned mask, bool effective_ids) {
    if (!file->mount) return disk_access(file, mask, effective_ids);
    struct mount* m = mount_by_id(file->mount);
    if (!m) return -5;
    if ((mask & DISK_MAY_WRITE) && readonly(m)) {
        uint32_t type = file->mode & S_IFMT;
        if (type == S_IFREG || type == S_IFDIR || type == S_IFLNK) return -30;
    }
    if ((mask & DISK_MAY_EXEC) && (m->flags & MS_NOEXEC) && (file->mode & S_IFMT) == S_IFREG) return -13;
    return mask ? mode_check(m, file, mask, !effective_ids) : 0;
}
int mnt_may_search(const char* directory) {
    if (!mount_path(directory)) return disk_may_search(directory);
    fat32_file_t dir;
    if (!mnt_lookup(directory, &dir) || !dir.is_dir) return 0;
    if (!dir.mount) return disk_may_search(directory);
    return mnt_permission(&dir, DISK_MAY_EXEC);
}
static void parent_of(const char* path, char* parent) {
    strcpy(parent, path);
    unsigned end = strlen(parent);
    while (end > 1 && parent[end - 1] == '/') end--;
    while (end > 1 && parent[end - 1] != '/') end--;
    if (end > 1) end--;
    parent[end] = 0;
}
int mnt_parent_access(const char* path) {
    if (!mount_path(path)) return disk_parent_access(path);
    struct target t; translate(path, &t);
    if (!t.fs) return disk_parent_access(t.full);
    if (!t.rest[0]) return -16;
    char parent[FAT32_PATH_MAX]; parent_of(path, parent);
    fat32_file_t dir;
    if (!mnt_lookup(parent, &dir)) return -2;
    if (!dir.is_dir) return -20;
    if (readonly(t.fs) || readonly(t.m)) return -30;
    return mode_check(t.fs, &dir, DISK_MAY_WRITE | DISK_MAY_EXEC, false);
}
int mnt_delete_access(const char* path, const fat32_file_t* victim) {
    if (!mount_path(path)) return disk_delete_access(path, victim);
    int error = mnt_parent_access(path); if (error) return error;
    struct target t; translate(path, &t);
    if (!t.fs) return disk_delete_access(t.full, victim);
    char parent[FAT32_PATH_MAX]; parent_of(path, parent);
    fat32_file_t dir; if (!mnt_lookup(parent, &dir)) return -2;
    uint32_t uid = mount_current_uid();
    if ((t.fs->ops->flags & MOUNT_POSIX) && (dir.mode & S_ISVTX) && victim && uid != victim->uid && uid != dir.uid && !capable(CAP_FOWNER)) return -1;
    return 0;
}
int mnt_open_access(const fat32_file_t* file, uint64_t flags) {
    if (!file->mount) return disk_open_access(file, flags);
    unsigned mask = 0, access = flags & 3;
    if (access != 1) mask |= DISK_MAY_READ;
    if (access) mask |= DISK_MAY_WRITE;
    if (flags & 0x200) mask |= DISK_MAY_WRITE;
    if ((flags & 0x200000) && !(flags & 0x200)) mask = 0;
    return mnt_permission(file, mask);
}
static bool mount_setid_pending;
int mnt_exec_access(const fat32_file_t* file) {
    if (!file->mount) return disk_exec_access(file);
    struct mount* m = mount_by_id(file->mount);
    if (!m) return -5;
    if (m->flags & MS_NOEXEC) return -13;
    if ((file->mode & S_IFMT) != S_IFREG) return -13;
    return mode_check(m, file, DISK_MAY_EXEC, false);
}
void mnt_exec_arm(const fat32_file_t* file) {
    if (!file->mount) { disk_exec_arm(file); return; }
    struct mount* m = mount_by_id(file->mount);
    if (!m || (m->flags & MS_NOSUID) || !(m->ops->flags & MOUNT_POSIX) || (file->mode & S_IFMT) != S_IFREG) return;
    bool set_uid = file->mode & S_ISUID, set_gid = (file->mode & S_ISGID) && (file->mode & 010);
    if (set_uid || set_gid) { credentials_exec_setid(set_uid, file->uid, set_gid, file->gid); mount_setid_pending = true; }
}
void mnt_exec_finished(void) {
    if (mount_setid_pending) { credentials_exec_setid(false, 0, false, 0); mount_setid_pending = false; }
    disk_exec_finished();
}

/* ---- stat, sync ---- */
bool mnt_stat(const fat32_file_t* file, struct memory_file* cache, struct linux_stat* st) {
    if (!file || !file->mount) return disk_stat(file, cache, st);
    struct mount* m = mount_by_id(file->mount);
    fat32_file_t now = *file;
    if (m && !m->dead && !now.detached) mnt_refresh_file(&now);
    memset(st, 0, sizeof(*st));
    st->dev = m ? m->dev_number : 0; st->ino = mount_identity(&now); st->nlink = now.detached ? 0 : now.nlink ? now.nlink : 1;
    st->mode = now.mode; st->uid = credentials_visible_id(now.uid, false); st->gid = credentials_visible_id(now.gid, true);
    st->rdev = now.rdev; st->size = (int64_t)now.size; st->block_size = 4096; st->blocks = (int64_t)now.blocks;
    int64_t mtime = now.mtime, ctime = now.ctime;
    if (cache) {
        st->size = (int64_t)cache->size;
        if (cache->dirty_pages || cache->metadata_dirty || cache->disk.detached) { mtime = (int64_t)cache->changed; if ((int64_t)cache->changed > ctime) ctime = (int64_t)cache->changed; }
        if ((int64_t)(cache->pages * 8) > st->blocks && cache->disk.detached) st->blocks = (int64_t)(cache->pages * 8);
    }
    st->atime = now.atime / 1000000000LL; st->atime_ns = (uint64_t)(now.atime % 1000000000LL);
    st->mtime = mtime / 1000000000LL; st->mtime_ns = (uint64_t)(mtime % 1000000000LL);
    st->ctime = ctime / 1000000000LL; st->ctime_ns = (uint64_t)(ctime % 1000000000LL);
    return true;
}
int mnt_seek_data(fat32_file_t* file, struct memory_file* cache, uint64_t offset, bool hole, uint64_t* result) {
    if (!file->mount) return disk_seek_data(file, cache, offset, hole, result);
    uint64_t size = cache ? cache->size : file->size;
    if (offset >= size) return -6;
    *result = hole ? size : offset;
    return 0;
}
static int sync_mount(struct mount* m) {
    if (m->dead || m->bound || !m->ops || !m->ops->sync || readonly(m)) return 0;
    return m->ops->sync(m);
}
int mnt_sync(void) {
    int result = disk_sync();
    for (struct mount* m = mounts; m; m = m->next) if (sync_mount(m) < 0) result = -5;
    return result;
}
void mnt_writeback_complete(bool all, bool wrote) {
    disk_writeback_complete(all, wrote);
    if (all) for (struct mount* m = mounts; m; m = m->next) sync_mount(m);
}
void mnt_commit_poll(void) { disk_commit_poll(); }
static int unmount_one(struct mount* m, bool force);
bool mount_write_through(const fat32_file_t* file) {
    struct mount* m = file->mount ? mount_by_id(file->mount) : NULL;
    return m && !m->dead && m->ops && ((m->ops->flags & MOUNT_FUSE) || (m->flags & MS_SYNCHRONOUS));
}
void mnt_shutdown(void) {
    /* FUSE daemons cannot be waited for while the machine stops: their mounts go dead first (no requests) */
    for (struct mount* m = mounts; m; m = m->next) if (m->ops && (m->ops->flags & MOUNT_FUSE)) m->dead = true;
    memory_file_writeback(true, 0, ~0ULL);
    for (;;) {   /* newest first: children before their parents */
        struct mount* last = NULL;
        for (struct mount* m = mounts; m; m = m->next) last = m;
        if (!last) break;
        unmount_one(last, true);
    }
    disk_shutdown();
}
int mnt_vfs_fchmod(void* description, uint64_t mode);
int64_t mnt_vfs_call(struct registers* r);

/* ---- per-mount helpers for drivers ---- */
void mount_fill_times(fat32_file_t* file, int64_t atime, int64_t mtime, int64_t ctime) { file->atime = atime; file->mtime = mtime; file->ctime = ctime; }
bool mount_option(char** cursor, char* key, unsigned key_size, char* value, unsigned value_size) {
    char* p = *cursor;
    while (*p == ',') p++;
    if (!*p) return false;
    unsigned k = 0, v = 0; key[0] = value[0] = 0;
    while (*p && *p != ',' && *p != '=') { if (k + 1 < key_size) key[k++] = *p; p++; }
    key[k] = 0;
    if (*p == '=') { p++; while (*p && *p != ',') { if (v + 1 < value_size) value[v++] = *p; p++; } }
    value[v] = 0;
    *cursor = p;
    return true;
}

/* ---- mount(2) ---- */
static int copy_user_string(uint64_t address, char* out, unsigned capacity) {
    for (unsigned i = 0; i < capacity; i++) {
        if (!accessible(address + i, 1, false)) return -14;
        out[i] = (char)uget8(address + i);
        if (!out[i]) return 0;
    }
    return -36;
}
static const struct mount_ops* const types[] = {&vfat_mount_ops, &exfat_mount_ops, &iso9660_mount_ops, &fuse_mount_ops, &fuseblk_mount_ops};
static const struct mount_ops* type_named(const char* name) {
    if (!strcmp(name, "msdos") || !strcmp(name, "fat")) name = "vfat";
    if (!strncmp(name, "fuse.", 5)) name = "fuse";
    if (!strncmp(name, "fuseblk.", 8)) name = "fuseblk";
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) if (types[i] && types[i]->name && !strcmp(types[i]->name, name)) return types[i];
    return NULL;
}
__attribute__((weak)) const struct mount_ops vfat_mount_ops = {0};
__attribute__((weak)) const struct mount_ops exfat_mount_ops = {0};
__attribute__((weak)) const struct mount_ops iso9660_mount_ops = {0};
__attribute__((weak)) const struct mount_ops fuse_mount_ops = {0};
__attribute__((weak)) const struct mount_ops fuseblk_mount_ops = {0};
static void basename_of(const char* path, char* out) {
    const char* base = path + strlen(path);
    while (base > path && base[-1] != '/') base--;
    strncpy(out, *base ? base : "/", 255); out[255] = 0;
}
static bool user_media_target(const char* path, char* parent_out) {
    static const char* const roots[2] = {"/media/" DESKTOP_USER "/", "/run/media/" DESKTOP_USER "/"};
    for (unsigned i = 0; i < 2; i++) {
        unsigned n = strlen(roots[i]);
        if (!strncmp(path, roots[i], n) && path[n] && !s_strchr(path + n, '/') && strcmp(path + n, ".") && strcmp(path + n, "..")) { strncpy(parent_out, roots[i], FAT32_PATH_MAX); parent_out[n - 1] = 0; return true; }
    }
    return false;
}
static int ensure_directory(const char* path) {   /* kernel-created mount points (udisks style) */
    fat32_file_t f;
    if (mnt_lookup(path, &f)) return f.is_dir ? 0 : -20;
    char parent[FAT32_PATH_MAX]; parent_of(path, parent);
    if (strcmp(parent, "/")) { int e = ensure_directory(parent); if (e) return e; }
    int e = mnt_create_node(path, S_IFDIR | 0755, 0, NULL, &f);
    if (!e && !f.mount) disk_file_touch(&f);
    return e;
}
static uint64_t new_anonymous_dev(void) { return storage_linux_dev(0, anonymous_minor++); }
static void options_text(struct mount* m, char* out, unsigned capacity, bool super) {
    out[0] = 0;
    strcat(out, readonly(m) ? "ro" : "rw");
    if (!super) {
        if (m->flags & MS_NOSUID) strcat(out, ",nosuid");
        if (m->flags & MS_NODEV) strcat(out, ",nodev");
        if (m->flags & MS_NOEXEC) strcat(out, ",noexec");
        if (m->flags & MS_SYNCHRONOUS) strcat(out, ",sync");
        if (m->flags & MS_DIRSYNC) strcat(out, ",dirsync");
        strcat(out, (m->flags & MS_NOATIME) ? ",noatime" : (m->flags & MS_STRICTATIME) ? "" : ",relatime");
        return;
    }
    if (m->options[0] && strlen(out) + 1 + strlen(m->options) < capacity) { strcat(out, ","); strcat(out, m->options); }
}
static int attach(struct mount* m, const char* target) {
    if (mount_total >= MAX_MOUNTS) return -12;
    strcpy(m->path, target); m->path_length = strlen(target);
    basename_of(target, m->root_name);
    m->id = next_id++; if (next_id >= (1U << 19)) next_id = 2;
    struct mount* parent = mount_find(target, NULL); m->parent = parent ? parent->id : 1;
    m->next = NULL;
    struct mount** link = &mounts; while (*link) link = &(*link)->next;
    *link = m; mount_total++;
    return 0;
}
static void unlink_mount(struct mount* m) {
    for (struct mount** link = &mounts; *link; link = &(*link)->next) if (*link == m) { *link = m->next; mount_total--; break; }
}
static void free_mount(struct mount* m) {
    if (m->part) { if (m->part->holders) m->part->holders--; block_put(m->part->dev); m->part = NULL; }
    arena_free((uint64_t)m, sizeof(*m));
}
/* Release a detached or forced mount: write back, driver unmount, holders. */
static int unmount_one(struct mount* m, bool force) {
    for (struct mount* c = mounts; c; c = c->next) if (c->bound == m && c != m) { c->bound = NULL; c->dead = true; }   /* binds of it go dead */
    if (!m->dead) memory_file_writeback(true, 0, ~0ULL);
    memory_file_forget_mount(m->id);
    if (m->ops && !m->bound && m->ops->unmount) m->ops->unmount(m, force || m->dead);
    if (m->created_dir) {   /* udisks-style mount point under /media/<user> */
        fat32_file_t gone; char path[FAT32_PATH_MAX]; strcpy(path, m->path);
        unlink_mount(m);
        if (!mount_find(path, NULL) || !mount_path(path)) { if (!mnt_detach_path(path, &gone) && gone.detached) mnt_dispose_file(&gone); }
    } else unlink_mount(m);
    vga_print("[64] MOUNT unmounted "); vga_print(m->path); vga_print("\n");
    free_mount(m);
    return 0;
}
static void reap_detached(void) {
    for (struct mount* m = mounts; m;) {
        struct mount* next = m->next;
        if (m->detached && !kernel_mount_busy(m->id)) {
            bool children = false;
            for (struct mount* c = mounts; c; c = c->next) if (c != m && c->parent == m->id && !c->detached) children = true;
            if (!children) unmount_one(m, false);
        }
        m = next;
    }
}
static bool user_may_mount_block(struct block_partition* part, const char* target, uint64_t flags, char* parent) {
    if (mount_current_uid() != DESKTOP_UID) return false;
    struct block_device* dev = part->dev;
    if (!dev->hotplug || !dev->kind || (strcmp(dev->kind, "USB") && strcmp(dev->kind, "UAS") && strcmp(dev->kind, "loop"))) return false;
    if ((flags & (MS_NOSUID | MS_NODEV)) != (MS_NOSUID | MS_NODEV)) return false;
    return user_media_target(target, parent);
}
static int mount_new(const char* source, const char* target, const char* type, uint64_t flags, char* data, bool kernel) {
    const struct mount_ops* ops = type_named(type);
    if (!ops || !ops->mount) return -19;              /* ENODEV: unknown filesystem type */
    struct block_partition* part = NULL;
    if (ops->flags & MOUNT_NEEDS_BLOCK) {
        if (!source || !source[0]) return -15;        /* ENOTBLK */
        char path[FAT32_PATH_MAX]; strcpy(path, source); struct open_file* bound = NULL;
        int error = kernel ? 0 : kernel_path_at((uint64_t)-100, path, true, &bound);
        if (error) return error;
        int device = device_lookup(path);
        {   /* a kernel link (/dev/<vg>/<lv>, /dev/disk/by-*: fstab at boot is not resolved above): its device */
            int device_readlink(unsigned device, char* target, unsigned capacity); char target[96];
            if (device >= 0 && device_readlink((unsigned)device, target, sizeof(target)) > 0) {
                const char* p = target; while (!strncmp(p, "../", 3)) p += 3;
                char again[112] = "/dev/"; strcat(again, p); device = device_lookup(again);
            }
        }
        if (device < 0) { struct linux_stat st; return kernel_resolved_stat(path, bound, true, &st) ? -2 : -15; }
        part = storage_partition((unsigned)device);
        if (!part) return -15;
        if (part->dev->removed) return -6;
        if (part->holders) return -16;               /* mounted elsewhere or opened exclusively */
        if (part->dev->read_only) flags |= MS_RDONLY;
    }
    char parent[FAT32_PATH_MAX]; bool created = false;
    if (!kernel && !capable(CAP_SYS_ADMIN)) {
        if (part && user_may_mount_block(part, target, flags, parent) && (ops->flags & MOUNT_USER_OK)) {
            fat32_file_t f;
            if (!mnt_lookup(target, &f)) {
                fat32_file_t pdir;
                if (!mnt_lookup(parent, &pdir)) { int e = ensure_directory(parent); if (e) return e; }
                int e = ensure_directory(target); if (e) return e;
                created = true;
            }
        } else if (!(ops->flags & MOUNT_FUSE)) return -1;   /* EPERM */
    }
    fat32_file_t point;
    if (!mnt_lookup(target, &point)) return -2;
    if (!point.is_dir) return -20;
    struct mount* m = (void*)arena_alloc(sizeof(*m));
    if (!m) return -12;
    memset(m, 0, sizeof(*m));
    m->ops = ops; m->part = part; m->flags = flags & ~(MS_REMOUNT | MS_BIND | MS_MOVE | MS_REC | MS_SILENT | MS_PROPAGATION | MS_KERNMOUNT);
    m->owner = kernel || capable(CAP_SYS_ADMIN) ? ~0U : mount_current_uid();
    m->created_dir = created;
    strncpy(m->type, type, sizeof(m->type) - 1); strncpy(m->source, source ? source : "none", sizeof(m->source) - 1);
    m->dev_number = part ? storage_linux_dev(part->major, part->minor) : new_anonymous_dev();
    strcpy(m->path, target);   /* drivers may log it */
    m->id = next_id;           /* MOUNT_INO during the driver's mount (attach assigns the same) */
    if (part) { part->holders++; block_get(part->dev); }
    char options[256]; strncpy(options, data ? data : "", sizeof(options) - 1); options[sizeof(options) - 1] = 0;
    int error = ops->mount(m, options);
    if (error) {
        if (part) { part->holders--; block_put(part->dev); }
        arena_free((uint64_t)m, sizeof(*m));
        if (created) { fat32_file_t gone; if (!mnt_detach_path(target, &gone) && gone.detached) mnt_dispose_file(&gone); }
        return error;
    }
    if ((error = attach(m, target))) { ops->unmount(m, true); free_mount(m); return error; }
    vga_print("[64] MOUNT "); vga_print(m->source); vga_print(" on "); vga_print(m->path); vga_print(" type "); vga_print(m->type);
    vga_print(readonly(m) ? " (ro)\n" : " (rw)\n");
    return 0;
}
static int mount_bind(const char* source, const char* target, uint64_t flags) {
    if (!capable(CAP_SYS_ADMIN)) return -1;
    fat32_file_t s, t;
    if (!mnt_lookup(source, &s)) return -2;
    if (!mnt_lookup(target, &t)) return -2;
    if (s.is_dir != t.is_dir) return -20;
    if (!s.is_dir) return -22;   /* file bind mounts are not supported: directories only */
    struct target st; bool inside = translate(source, &st);
    struct mount* m = (void*)arena_alloc(sizeof(*m)); if (!m) return -12;
    memset(m, 0, sizeof(*m));
    m->flags = flags & (MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC);
    m->owner = ~0U;
    if (inside) { m->bound = st.fs; strcpy(m->subdir, st.rest); m->ops = st.fs ? st.fs->ops : NULL; m->dev_number = st.fs ? st.fs->dev_number : 1; }
    else { m->bound = NULL; m->ops = NULL; strcpy(m->subdir, source[0] == '/' ? source + 1 : source); m->dev_number = 1; }
    strcpy(m->type, inside && st.fs ? st.fs->type : disk_posix() ? "ext4" : "vfat");
    strncpy(m->source, source, sizeof(m->source) - 1);
    int error = attach(m, target);
    if (error) { arena_free((uint64_t)m, sizeof(*m)); return error; }
    return 0;
}
static int mount_move(const char* source, const char* target) {
    if (!capable(CAP_SYS_ADMIN)) return -1;
    struct mount* m = NULL;
    for (struct mount* c = mounts; c; c = c->next) if (!c->detached && !strcmp(c->path, source)) m = c;
    if (!m) return -22;
    fat32_file_t t; if (!mnt_lookup(target, &t)) return -2;
    if (!t.is_dir) return -20;
    if (prefix_of(m, target)) return -40;          /* into itself */
    unsigned old_length = m->path_length;
    for (struct mount* c = mounts; c; c = c->next)  /* children move along */
        if (c != m && !strncmp(c->path, m->path, old_length) && c->path[old_length] == '/') {
            char tail[FAT32_PATH_MAX]; strcpy(tail, c->path + old_length);
            if (strlen(target) + strlen(tail) >= FAT32_PATH_MAX) return -36;
            strcpy(c->path, target); strcat(c->path, tail); c->path_length = strlen(c->path);
        }
    strcpy(m->path, target); m->path_length = strlen(target); basename_of(target, m->root_name);
    return 0;
}
static int remount(const char* target, uint64_t flags, char* data) {
    if (!capable(CAP_SYS_ADMIN)) return -1;
    if (!strcmp(target, "/") && !mount_find("/", NULL)) {    /* the root filesystem: read-only switch only through the driver */
        if ((flags & MS_RDONLY) == (disk_is_readonly() ? MS_RDONLY : 0)) return 0;
        return flags & MS_RDONLY ? (mnt_sync() ? -5 : 0) : -30;
    }
    struct mount* m = NULL;
    for (struct mount* c = mounts; c; c = c->next) if (!c->detached && !strcmp(c->path, target)) m = c;
    if (!m) return -22;
    uint64_t keep = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_SYNCHRONOUS | MS_DIRSYNC | MS_NOATIME | MS_NODIRATIME | MS_RELATIME | MS_STRICTATIME | MS_LAZYTIME;
    bool to_ro = (flags & MS_RDONLY) && !(m->flags & MS_RDONLY), to_rw = !(flags & MS_RDONLY) && (m->flags & MS_RDONLY);
    if (to_rw && m->part && m->part->dev->read_only) return -13;
    if (to_ro) { memory_file_writeback(true, 0, ~0ULL); if (sync_mount(m) < 0) return -5; }
    if ((to_ro || to_rw) && m->ops && !m->bound && m->ops->remount) { int e = m->ops->remount(m, flags, data); if (e) return e; }
    m->flags = (m->flags & ~keep) | (flags & keep);
    return 0;
}
static int resolve_target(uint64_t address, char* path, bool follow) {
    int error = kernel_user_path(address, path); if (error) return error;
    if (!path[0]) return -2;
    struct open_file* bound = NULL;
    return kernel_path_at((uint64_t)-100, path, follow, &bound);
}
static int do_umount(const char* path, unsigned flags) {
    reap_detached();
    if (flags & ~(MNT_FORCE | MNT_DETACH | MNT_EXPIRE | UMOUNT_NOFOLLOW)) return -22;
    if ((flags & MNT_EXPIRE) && (flags & (MNT_FORCE | MNT_DETACH))) return -22;
    struct mount* m = NULL;
    for (struct mount* c = mounts; c; c = c->next) if (!c->detached && !strcmp(c->path, path)) m = c;
    if (!m) return -22;                                    /* not a mount point */
    if (!capable(CAP_SYS_ADMIN) && (m->owner == ~0U || m->owner != mount_current_uid())) return -1;
    if (flags & MNT_EXPIRE) return -11;
    bool children = false;
    for (struct mount* c = mounts; c; c = c->next) if (c != m && !c->detached && prefix_of(m, c->path) && c->path_length > m->path_length) children = true;
    if (flags & MNT_DETACH) {
        for (struct mount* c = mounts; c; c = c->next) if (c != m && prefix_of(m, c->path) && c->path_length > m->path_length) c->detached = true;
        m->detached = true;
        if ((flags & MNT_FORCE) && m->ops && (m->ops->flags & MOUNT_FUSE) && m->ops->unmount) m->dead = true;
        reap_detached();
        return 0;
    }
    if (children) return -16;
    if ((flags & MNT_FORCE) && m->ops && (m->ops->flags & MOUNT_FUSE)) m->dead = true;   /* abort the connection */
    memory_file_writeback(true, 0, ~0ULL);
    if (kernel_mount_busy(m->id)) { if (m->dead && (flags & MNT_FORCE)) { m->detached = true; return 0; } return -16; }
    if (!m->dead && sync_mount(m) < 0 && !(flags & MNT_FORCE)) return -5;
    return unmount_one(m, (flags & MNT_FORCE) != 0);
}
int64_t mount_call(struct registers* r) {
    char target[FAT32_PATH_MAX]; int error;
    if (r->rax == 166) {                                    /* umount2(target, flags) */
        if ((error = resolve_target(r->rdi, target, !(r->rsi & UMOUNT_NOFOLLOW)))) return error;
        return do_umount(target, (unsigned)r->rsi);
    }
    if (r->rax == 155) return -22;                          /* pivot_root: the boot hand-over replaces the root (mount_switch_root) */
    /* mount(source, target, fstype, flags, data) */
    uint64_t flags = r->r10;
    if ((flags & 0xFFFF0000ULL) == 0xC0ED0000ULL) flags &= 0xFFFF;   /* MS_MGC_VAL */
    char source[FAT32_PATH_MAX] = "", type[64] = "", data[512] = "";
    if (r->rdi && (error = copy_user_string(r->rdi, source, sizeof(source)))) return error;
    if ((error = resolve_target(r->rsi, target, true))) {
        /* udisks-style user mounts: /media/<user>/<name> may not exist yet, mount_new creates it */
        char parent[FAT32_PATH_MAX];
        if (error != -2 || kernel_user_path(r->rsi, target) || !user_media_target(target, parent)) return error;
    }
    if (r->rdx && (error = copy_user_string(r->rdx, type, sizeof(type)))) return error;
    if (r->r8 && (error = copy_user_string(r->r8, data, sizeof(data)))) return error == -36 ? -22 : error;
    reap_detached();
    if (flags & MS_REMOUNT) return remount(target, flags, data);
    if (flags & MS_BIND) {
        char path[FAT32_PATH_MAX]; strcpy(path, source); struct open_file* bound = NULL;
        if ((error = kernel_path_at((uint64_t)-100, path, true, &bound))) return error;
        return mount_bind(path, target, flags);
    }
    if (flags & MS_MOVE) {
        char path[FAT32_PATH_MAX]; strcpy(path, source); struct open_file* bound = NULL;
        if ((error = kernel_path_at((uint64_t)-100, path, true, &bound))) return error;
        return mount_move(path, target);
    }
    if (flags & MS_PROPAGATION) {                           /* --make-private/--make-shared: one namespace, nothing to propagate */
        if (!capable(CAP_SYS_ADMIN)) return -1;
        fat32_file_t f; return mnt_lookup(target, &f) || !strcmp(target, "/") ? 0 : -2;
    }
    if (!type[0]) return -22;
    return mount_new(source, target, type, flags, data, false);
}
/* statfs on a file of a mount: values = type, bsize, blocks, bfree, bavail, files, ffree, fsid0, fsid1, namemax, frsize, flags */
bool mount_statfs_dev(uint64_t dev, uint64_t values[12]) {
    struct mount* m = NULL;
    for (struct mount* c = mounts; c; c = c->next) if (c->dev_number == dev && !c->bound && c->ops) m = c;
    if (!m) return false;
    if (!readonly(m)) memory_file_writeback(false, 0, ~0ULL);   /* allocations happen at write-back (as the FAT root's statfs) */
    struct mount_statfs s; memset(&s, 0, sizeof(s)); s.block_size = 4096; s.name_max = 255;
    if (!m->dead && m->ops->statfs) m->ops->statfs(m, &s);
    values[0] = s.type; values[1] = s.block_size; values[2] = s.blocks; values[3] = s.free; values[4] = s.available;
    values[5] = s.files; values[6] = s.free_files; values[7] = s.fsid[0] ? s.fsid[0] : (uint32_t)dev; values[8] = s.fsid[1]; values[9] = s.name_max;
    values[10] = s.block_size;
    uint64_t f = 0x20;   /* ST_VALID */
    if (readonly(m)) f |= 1; if (m->flags & MS_NOSUID) f |= 2; if (m->flags & MS_NODEV) f |= 4; if (m->flags & MS_NOEXEC) f |= 8;
    if (m->flags & MS_SYNCHRONOUS) f |= 16; if (m->flags & MS_NOATIME) f |= 1024; if (!(m->flags & (MS_NOATIME | MS_STRICTATIME))) f |= 4096;
    values[11] = f;
    return true;
}
/* Block device left (blockdev.c hot-remove path via mount_device_removed): its mounts return EIO from now on. */
void mount_device_removed(struct block_device* dev) {
    for (struct mount* m = mounts; m; m = m->next) if (m->part && m->part->dev == dev) m->dead = true;
}

/* ---- attribute syscalls on mounted files ----
 * kernel.c sends chmod/chown/link/access/statx/fallocate/truncate (and through vfs_call: mknod, utimes) here
 * when the target lies on a mounted filesystem; root files keep vfs.c (ext4) or fsext.c (FAT) semantics. */
#define AT_FDCWD ((uint64_t)-100)
#define AT_NOFOLLOW 0x100ULL
#define AT_EMPTY 0x1000ULL
#define AT_FOLLOW 0x400ULL
struct resolved { char path[FAT32_PATH_MAX]; struct open_file* bound; fat32_file_t file; bool disk, mounted; };
static int resolve_at(uint64_t directory, uint64_t address, bool follow, bool empty_ok, struct resolved* t) {
    t->bound = NULL; t->disk = false; t->mounted = false;
    int error;
    if (!address) { if (!empty_ok) return -14; t->path[0] = 0; }
    else if ((error = kernel_user_path(address, t->path))) return error;
    if (!t->path[0]) {
        if (!empty_ok) return -2;
        struct open_file* f = kernel_file(directory); if (!f) return -9;
        fat32_file_t* disk = kernel_file_disk(f);
        t->bound = f;
        if (disk) { t->file = *disk; t->disk = true; t->mounted = disk->mount != 0; if (t->mounted) mnt_refresh_file(&t->file); }
        return 0;
    }
    if ((error = kernel_path_at(directory, t->path, follow, &t->bound))) return error;
    if (t->bound) {
        fat32_file_t* disk = kernel_file_disk(t->bound);
        if (disk) { t->file = *disk; t->disk = true; t->mounted = disk->mount != 0; if (t->mounted) mnt_refresh_file(&t->file); }
        return 0;
    }
    t->mounted = mount_path(t->path);
    if (t->mounted && mnt_lookup(t->path, &t->file)) t->disk = true;
    if (t->disk && !t->file.mount) t->mounted = false;    /* a bind of a root directory */
    return 0;
}
static bool path_argument_on_mount(uint64_t directory, uint64_t address, bool follow, bool empty_ok) {
    struct resolved t;
    if (resolve_at(directory, address, follow, empty_ok, &t)) return false;
    if (t.disk) return t.mounted;
    return mount_path(t.path);    /* not existing yet: the parent's filesystem decides (mknod) */
}
bool mount_syscall_targets_mount(struct registers* r) {
    if (!mounts) return false;
    switch (r->rax) {
    case 90: case 92: case 132: case 235: case 133: case 76: return path_argument_on_mount(AT_FDCWD, r->rdi, true, false);
    case 94: return path_argument_on_mount(AT_FDCWD, r->rdi, false, false);
    case 93: case 91: case 285: { struct open_file* f = kernel_file(r->rdi); fat32_file_t* d = f ? kernel_file_disk(f) : NULL; return d && d->mount; }
    case 260: return path_argument_on_mount(r->rdi, r->rsi, !(r->r8 & AT_NOFOLLOW), (r->r8 & AT_EMPTY) != 0);
    case 268: case 269: return path_argument_on_mount(r->rdi, r->rsi, true, false);
    case 439: case 452: return path_argument_on_mount(r->rdi, r->rsi, !(r->r10 & AT_NOFOLLOW), (r->r10 & AT_EMPTY) != 0);
    case 332: return path_argument_on_mount(r->rdi, r->rsi, !(r->rdx & AT_NOFOLLOW), (r->rdx & AT_EMPTY) != 0 || !r->rsi);
    case 86: return path_argument_on_mount(AT_FDCWD, r->rdi, false, false) || path_argument_on_mount(AT_FDCWD, r->rsi, false, false);
    case 265: return path_argument_on_mount(r->rdi, r->rsi, (r->r8 & AT_FOLLOW) != 0, (r->r8 & AT_EMPTY) != 0) || path_argument_on_mount(r->rdx, r->r10, false, false);
    case 259: return path_argument_on_mount(r->rdi, r->rsi, false, false);
    case 261: return path_argument_on_mount(r->rdi, r->rsi, true, !r->rsi);
    case 280: return path_argument_on_mount(r->rdi, r->rsi, !(r->r10 & AT_NOFOLLOW), !r->rsi || (r->r10 & AT_EMPTY));
    }
    return false;
}
static int chmod_file(struct resolved* t, uint64_t mode) {
    if (!t->disk) return -2;
    struct mount* m = mount_by_id(t->file.mount); if (!m) return -5;
    if (readonly(m)) return -30;
    if (mount_current_uid() != t->file.uid && !capable(CAP_FOWNER)) return -1;
    if ((t->file.mode & S_IFMT) == S_IFLNK) return -95;
    mode &= 07777;
    if ((mode & S_ISGID) && (t->file.mode & S_IFMT) != S_IFDIR && !credentials_in_group(credentials_current(), t->file.gid, false) && !capable(CAP_FSETID)) mode &= ~S_ISGID;
    struct mount_attr attr = {.valid = EXT4_ATTR_MODE, .mode = (uint32_t)mode};
    int error = setattr_file(&t->file, &attr); if (error) return error;
    file_refresh_views(&t->file); return 0;
}
int mnt_vfs_fchmod(void* description, uint64_t mode) {
    struct open_file* f = description; fat32_file_t* disk = kernel_file_disk(f);
    if (!disk || !disk->mount) return vfs_fchmod(description, mode);
    struct resolved t; t.bound = f; t.disk = true; t.mounted = true; t.file = *disk; mnt_refresh_file(&t.file);
    return chmod_file(&t, mode);
}
static int chown_file(struct resolved* t, uint32_t user_uid, uint32_t user_gid) {
    if (!t->disk) return -2;
    struct mount* m = mount_by_id(t->file.mount); if (!m) return -5;
    uint32_t uid = 0xFFFFFFFFu, gid = 0xFFFFFFFFu;
    if (user_uid != 0xFFFFFFFFu && !credentials_kernel_from_visible(user_uid, false, &uid)) return -22;
    if (user_gid != 0xFFFFFFFFu && !credentials_kernel_from_visible(user_gid, true, &gid)) return -22;
    if (readonly(m)) return -30;
    fat32_file_t* f = &t->file;
    if (uid != 0xFFFFFFFFu && uid != f->uid && !capable(CAP_CHOWN)) return -1;
    if (gid != 0xFFFFFFFFu && gid != f->gid && !capable(CAP_CHOWN) && (mount_current_uid() != f->uid || !credentials_in_group(credentials_current(), gid, false))) return -1;
    struct mount_attr attr = {0};
    if (uid != 0xFFFFFFFFu) { attr.valid |= EXT4_ATTR_UID; attr.uid = uid; }
    if (gid != 0xFFFFFFFFu) { attr.valid |= EXT4_ATTR_GID; attr.gid = gid; }
    if (attr.valid && (f->mode & S_IFMT) != S_IFDIR && (f->mode & (S_ISUID | S_ISGID))) {
        unsigned mode = f->mode & ~S_ISUID;
        if ((f->mode & S_ISGID) && (f->mode & 010)) mode &= ~S_ISGID;
        attr.valid |= EXT4_ATTR_MODE; attr.mode = mode & 07777;
    }
    if (!attr.valid) { attr.valid = EXT4_ATTR_CTIME; attr.ctime = (int64_t)native_realtime_ns(); }
    int error = setattr_file(f, &attr); if (error) return error;
    file_refresh_views(f); return 0;
}
#define TIME_NOW (-1LL)
#define TIME_OMIT (-2LL)
static int64_t to_ns(int64_t sec, int64_t ns) {
    const int64_t limit = 9223372035LL;
    if (sec > limit) sec = limit; if (sec < -limit) sec = -limit;
    return sec * 1000000000LL + ns;
}
static int utimes_file(struct resolved* t, int64_t atime, int64_t mtime) {
    if (!t->disk) return -2;
    if (atime == TIME_OMIT && mtime == TIME_OMIT) return 0;
    struct mount* m = mount_by_id(t->file.mount); if (!m) return -5;
    if (readonly(m)) return -30;
    fat32_file_t* f = &t->file;
    bool explicit = (atime != TIME_NOW && atime != TIME_OMIT) || (mtime != TIME_NOW && mtime != TIME_OMIT);
    if (mount_current_uid() != f->uid && !capable(CAP_FOWNER)) {
        if (explicit) return -1;
        if (mode_check(m, f, DISK_MAY_WRITE, false)) return -13;
    }
    struct memory_file* cache = f->is_dir ? NULL : memory_file_disk_find(f);
    if (cache) { int error = memory_file_sync(cache, 0, ~0ULL); if (error) return error; mnt_refresh_file(f); }
    int64_t now = (int64_t)native_realtime_ns();
    struct mount_attr attr = {.valid = EXT4_ATTR_CTIME, .ctime = now};
    if (atime != TIME_OMIT) { attr.valid |= EXT4_ATTR_ATIME; attr.atime = atime == TIME_NOW ? now : atime; }
    if (mtime != TIME_OMIT) { attr.valid |= EXT4_ATTR_MTIME; attr.mtime = mtime == TIME_NOW ? now : mtime; }
    int error = setattr_file(f, &attr); if (error) return error;
    if (cache && mtime != TIME_OMIT) cache->changed = (uint64_t)f->mtime;
    file_refresh_views(f); return 0;
}
static int read_times(uint64_t call, uint64_t value, int64_t* times) {
    if (!value) { times[0] = times[1] = TIME_NOW; return 0; }
    if (call == 132) {
        if (!accessible(value, 16, false)) return -14;
        times[0] = to_ns((int64_t)uget64(value), 0); times[1] = to_ns((int64_t)uget64(value + 8), 0); return 0;
    }
    if (!accessible(value, 32, false)) return -14;
    int64_t v[4]; copy_from_user(v, (const void*)value, sizeof(v));
    for (int i = 0; i < 2; i++) {
        int64_t sec = v[2 * i], fraction = v[2 * i + 1];
        if (call == 280) {
            if (fraction == (1LL << 30) - 1) times[i] = TIME_NOW;
            else if (fraction == (1LL << 30) - 2) times[i] = TIME_OMIT;
            else if (fraction < 0 || fraction > 999999999) return -22;
            else times[i] = to_ns(sec, fraction);
        } else {
            if (fraction < 0 || fraction >= 1000000) return -22;
            times[i] = to_ns(sec, fraction * 1000);
        }
    }
    return 0;
}
static int link_paths(uint64_t olddir, uint64_t oldaddr, uint64_t newdir, uint64_t newaddr, uint64_t flags) {
    if (flags & ~(AT_FOLLOW | AT_EMPTY)) return -22;
    if ((flags & AT_EMPTY) && !capable(CAP_DAC_READ_SEARCH)) return -2;
    struct resolved source;
    int error = resolve_at(olddir, oldaddr, (flags & AT_FOLLOW) != 0, (flags & AT_EMPTY) != 0, &source); if (error) return error;
    char path[FAT32_PATH_MAX]; struct open_file* bound;
    if ((error = kernel_user_path(newaddr, path))) return error;
    if (!path[0]) return -2;
    if ((error = kernel_path_at(newdir, path, false, &bound))) return error;
    fat32_file_t existing;
    if (bound || mnt_lookup(path, &existing)) return -17;
    if (!source.disk) return -2;
    struct target a, b; bool am = translate(source.path[0] ? source.path : path, &a), bm = translate(path, &b);
    if (!source.file.mount || !bm || !b.fs || b.fs->id != source.file.mount || (am && a.fs != b.fs)) return -18;
    if (source.file.is_dir) return -1;
    if ((error = mnt_parent_access(path))) return error;
    struct mount* m = b.fs;
    if (!m->ops->link) return -1;                         /* vfat, exfat: no hard links (EPERM) */
    if ((error = m->ops->link(m, &source.file, b.rest))) return error;
    mnt_refresh_file(&source.file); file_refresh_views(&source.file);
    return 0;
}
static int mknod_path(uint64_t directory, uint64_t address, uint64_t mode, uint64_t dev) {
    uint32_t type = (uint32_t)mode & S_IFMT;
    if (!type) type = S_IFREG;
    if (type != S_IFREG && type != S_IFCHR && type != S_IFBLK && type != S_IFIFO && type != S_IFSOCK) return -22;
    if ((type == S_IFCHR || type == S_IFBLK) && !capable(CAP_MKNOD)) return -1;
    char path[FAT32_PATH_MAX]; int error = kernel_user_path(address, path); if (error) return error;
    if (!path[0]) return -2;
    struct open_file* bound;
    if ((error = kernel_path_at(directory, path, false, &bound))) return error;
    fat32_file_t file;
    if (bound || mnt_lookup(path, &file)) return -17;
    if ((error = mnt_parent_access(path))) return error;
    return mnt_create_node(path, type | ((uint32_t)mode & 07777), (uint32_t)dev, NULL, &file);
}
int64_t fsext_call(struct registers* registers);
int64_t mnt_vfs_call(struct registers* r) {
    if (!mount_syscall_targets_mount(r)) return vfs_call(r);
    struct resolved t; int error; int64_t times[2];
    switch (r->rax) {
    case 76: return vfs_call(r);       /* generic: truncate through an open description and the page cache */
    case 332: return fsext_call(r);    /* generic statx from stat */
    case 90: case 268: case 452: {
        uint64_t dirfd = r->rax == 90 ? AT_FDCWD : r->rdi, path = r->rax == 90 ? r->rdi : r->rsi, mode = r->rax == 90 ? r->rsi : r->rdx;
        uint64_t flags = r->rax == 452 ? r->r10 : 0;
        if (flags & ~(AT_NOFOLLOW | AT_EMPTY)) return -22;
        if ((error = resolve_at(dirfd, path, !(flags & AT_NOFOLLOW), (flags & AT_EMPTY) != 0, &t))) return error;
        return chmod_file(&t, mode);
    }
    case 91: { struct open_file* f = kernel_file(r->rdi); return f ? mnt_vfs_fchmod(f, r->rsi) : -9; }
    case 92: case 94: case 260: {
        uint64_t dirfd = r->rax == 260 ? r->rdi : AT_FDCWD, path = r->rax == 260 ? r->rsi : r->rdi;
        uint64_t uid = r->rax == 260 ? r->rdx : r->rsi, gid = r->rax == 260 ? r->r10 : r->rdx;
        uint64_t flags = r->rax == 260 ? r->r8 : r->rax == 94 ? AT_NOFOLLOW : 0;
        if (flags & ~(AT_NOFOLLOW | AT_EMPTY)) return -22;
        if ((error = resolve_at(dirfd, path, !(flags & AT_NOFOLLOW), (flags & AT_EMPTY) != 0, &t))) return error;
        return chown_file(&t, (uint32_t)uid, (uint32_t)gid);
    }
    case 93: {
        struct open_file* f = kernel_file(r->rdi); if (!f) return -9;
        fat32_file_t* disk = kernel_file_disk(f); if (!disk) return -1;
        t.bound = f; t.disk = true; t.file = *disk; mnt_refresh_file(&t.file);
        return chown_file(&t, (uint32_t)r->rsi, (uint32_t)r->rdx);
    }
    case 86: return link_paths(AT_FDCWD, r->rdi, AT_FDCWD, r->rsi, 0);
    case 265: return link_paths(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case 133: return mknod_path(AT_FDCWD, r->rdi, r->rsi, r->rdx);
    case 259: return mknod_path(r->rdi, r->rsi, r->rdx, r->r10);
    case 132: case 235: case 261: case 280: {
        uint64_t dirfd = r->rax == 261 || r->rax == 280 ? r->rdi : AT_FDCWD, path = r->rax == 261 || r->rax == 280 ? r->rsi : r->rdi;
        uint64_t value = r->rax == 261 || r->rax == 280 ? r->rdx : r->rsi, flags = r->rax == 280 ? r->r10 : 0;
        if (flags & ~(AT_NOFOLLOW | AT_EMPTY)) return -22;
        if ((error = read_times(r->rax, value, times))) return error;
        if ((error = resolve_at(dirfd, path, !(flags & AT_NOFOLLOW), !path || (flags & AT_EMPTY), &t))) return error;
        return utimes_file(&t, times[0], times[1]);
    }
    case 269: case 439: {
        uint64_t mode = r->rdx, flags = r->rax == 439 ? r->r10 : 0;
        if (mode & ~7ULL) return -22;
        if (flags & ~(AT_NOFOLLOW | 0x200ULL | AT_EMPTY)) return -22;
        if ((error = resolve_at(r->rdi, r->rsi, !(flags & AT_NOFOLLOW), (flags & AT_EMPTY) != 0, &t))) return error;
        if (!t.disk) return -2;
        unsigned mask = (mode & 4 ? DISK_MAY_READ : 0) | (mode & 2 ? DISK_MAY_WRITE : 0) | (mode & 1 ? DISK_MAY_EXEC : 0);
        return mnt_access(&t.file, mask, (flags & 0x200) != 0);
    }
    case 285: {
        struct open_file* f = kernel_file(r->rdi); if (!f) return -9;
        fat32_file_t* disk = kernel_file_disk(f); if (!disk) return -19;
        if ((int64_t)r->rdx < 0 || (int64_t)r->r10 <= 0) return -22;
        if (disk->is_dir) return -21;
        struct mount* m = mount_by_id(disk->mount); if (!m) return -5;
        if (readonly(m)) return -30;
        if (!m->ops->fallocate) return -95;
        struct memory_file* cache = kernel_file_memory(f);
        if (cache) { int e = memory_file_sync(cache, 0, ~0ULL); if (e) return e; *disk = cache->disk; }
        int e = m->ops->fallocate(m, disk, (unsigned)r->rsi, r->rdx, r->r10); disk->mount = m->id;
        if (e) return e;
        if (cache) { cache->disk = *disk; if (disk->size > cache->size) { cache->size = disk->size; cache->valid_size = disk->size; } }
        file_refresh_views(disk); return 0;
    }
    }
    return vfs_call(r);
}

/* ---- /proc/mounts, /proc/self/mountinfo ---- */
static unsigned add(char* out, unsigned used, unsigned capacity, const char* text) {
    unsigned n = strlen(text);
    if (used + n >= capacity) return used;
    memcpy(out + used, text, n + 1); return used + n;
}
static void escape(const char* in, char* out, unsigned capacity) {   /* spaces, tabs, newlines and backslashes as octal (seq_file mangle) */
    unsigned n = 0;
    for (; *in && n + 5 < capacity; in++) {
        if (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\\') { out[n++] = '\\'; out[n++] = (char)('0' + ((*in >> 6) & 7)); out[n++] = (char)('0' + ((*in >> 3) & 7)); out[n++] = (char)('0' + (*in & 7)); }
        else out[n++] = *in;
    }
    out[n] = 0;
}
const struct block_partition* ext4_dev_partition(void);
const char* dm_device_name(const struct block_device* dev);
static void root_source(char* out) {
    const struct block_partition* p = disk_posix() ? ext4_dev_partition() : block_root();
    const char* mapped = p ? dm_device_name(p->dev) : NULL;   /* a LUKS root (cryptroot.c): /dev/mapper/NAME, as Linux shows it */
    if (mapped && strlen(mapped) < 48) { strcpy(out, "/dev/mapper/"); strcat(out, mapped); return; }
    if (p) { strcpy(out, "/dev/"); strcat(out, p->linux_name); return; }   /* the FAT or ext4 root volume (an md root: /dev/mdN) */
    for (unsigned i = 0; i < block_partition_count(); i++) {   /* ext4 root: the listed partition the driver holds */
        const struct block_partition* q = block_partition_get(i);
        if (q && q->holders && q->number) { strcpy(out, "/dev/"); strcat(out, q->linux_name); return; }
    }
    strcpy(out, "/dev/root");
}
unsigned mount_proc_text(char* out, unsigned capacity, bool info) {
    unsigned used = 0; out[0] = 0;
    char line[1024], root[64], number[24];
    root_source(root);
    const char* root_type = disk_posix() ? "ext4" : "vfat";
    const char* root_rw = disk_is_readonly() ? "ro" : "rw";
    if (!info) {
        strcpy(line, root); strcat(line, " / "); strcat(line, root_type); strcat(line, " "); strcat(line, root_rw);
        strcat(line, disk_posix() ? ",relatime 0 0\n" : ",relatime,fmask=0077,dmask=0077,codepage=437,iocharset=utf8 0 0\n");
        used = add(out, used, capacity, line);
        used = add(out, used, capacity, "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
                                        "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
                                        "devtmpfs /dev devtmpfs rw,nosuid 0 0\n"
                                        "devpts /dev/pts devpts rw,nosuid,noexec,relatime,gid=5,mode=620,ptmxmode=666 0 0\n"
                                        "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n");
    } else {
        strcpy(line, "21 1 0:1 / / "); strcat(line, root_rw); strcat(line, ",relatime shared:1 - "); strcat(line, root_type); strcat(line, " ");
        strcat(line, root); strcat(line, " "); strcat(line, root_rw); strcat(line, disk_posix() ? "\n" : ",fmask=0077,dmask=0077\n");
        used = add(out, used, capacity, line);
        used = add(out, used, capacity, "27 21 0:7 / /proc rw,nosuid,nodev,noexec,relatime shared:2 - proc proc rw\n"
                                        "32 21 0:12 / /sys rw,nosuid,nodev,noexec,relatime shared:6 - sysfs sysfs rw\n"
                                        "29 21 0:9 / /dev rw,nosuid shared:3 - devtmpfs devtmpfs rw\n"
                                        "31 29 0:11 / /dev/pts rw,nosuid,noexec,relatime shared:4 - devpts devpts rw,gid=5,mode=620,ptmxmode=666\n"
                                        "30 29 0:10 / /dev/shm rw,nosuid,nodev shared:5 - tmpfs tmpfs rw\n");
    }
    for (struct mount* m = mounts; m; m = m->next) {
        if (m->detached) continue;
        char point[600], source[400], per[160], super[300];
        escape(m->path, point, sizeof(point)); escape(m->source, source, sizeof(source));
        options_text(m, per, sizeof(per), false); options_text(m, super, sizeof(super), true);
        line[0] = 0;
        if (!info) {
            strcat(line, source); strcat(line, " "); strcat(line, point); strcat(line, " "); strcat(line, m->type); strcat(line, " ");
            /* /proc/mounts: per-mount and superblock options in one list */
            strcat(line, per); if (m->options[0]) { strcat(line, ","); strcat(line, m->options); } strcat(line, " 0 0\n");
        } else {
            struct mount* parent = mount_by_id(m->parent);
            number64(20 + m->dev_number, number); strcat(line, number); strcat(line, " ");
            number64(parent ? 20 + parent->dev_number : 21, number); strcat(line, number); strcat(line, " ");
            uint64_t dev = m->dev_number; uint32_t major = (uint32_t)(((dev >> 8) & 0xFFF) | ((dev >> 32) & ~0xFFFULL)), minor = (uint32_t)((dev & 0xFF) | ((dev >> 12) & ~0xFFULL));
            number64(major, number); strcat(line, number); strcat(line, ":"); number64(minor, number); strcat(line, number);
            strcat(line, " /"); if (m->subdir[0]) { char sub[600]; escape(m->subdir, sub, sizeof(sub)); strcat(line, sub); }
            strcat(line, " "); strcat(line, point); strcat(line, " "); strcat(line, per); strcat(line, " - "); strcat(line, m->type);
            strcat(line, " "); strcat(line, source); strcat(line, " "); strcat(line, super); strcat(line, "\n");
        }
        used = add(out, used, capacity, line);
    }
    return used;
}

/* ---- /etc/fstab at boot ----
 * fs_spec (device path, UUID=, LABEL=, PARTUUID=, PARTLABEL=) fs_file fs_vfstype fs_mntops; "noauto", swap and the
 * root ("/") are skipped; options defaults/rw/ro/nosuid/nodev/noexec/sync/noatime/relatime/user/users/nofail, the
 * rest go to the filesystem. FUSE types need their userspace daemon and are left to the session. */
static uint64_t flag_option(const char* key, bool* consumed) {
    static const struct { const char* name; uint64_t set, clear; } table[] = {
        {"defaults", 0, 0}, {"rw", 0, MS_RDONLY}, {"ro", MS_RDONLY, 0}, {"nosuid", MS_NOSUID, 0}, {"suid", 0, MS_NOSUID},
        {"nodev", MS_NODEV, 0}, {"dev", 0, MS_NODEV}, {"noexec", MS_NOEXEC, 0}, {"exec", 0, MS_NOEXEC}, {"sync", MS_SYNCHRONOUS, 0},
        {"async", 0, MS_SYNCHRONOUS}, {"noatime", MS_NOATIME, 0}, {"relatime", MS_RELATIME, 0}, {"strictatime", MS_STRICTATIME, 0},
        {"nodiratime", MS_NODIRATIME, 0}, {"dirsync", MS_DIRSYNC, 0}, {"auto", 0, 0}, {"nofail", 0, 0}, {"user", MS_NOSUID | MS_NODEV | MS_NOEXEC, 0},
        {"users", MS_NOSUID | MS_NODEV | MS_NOEXEC, 0}, {"nouser", 0, 0}, {"lazytime", MS_LAZYTIME, 0}, {"x-systemd.automount", 0, 0}};
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) if (!strcmp(key, table[i].name)) { *consumed = true; return table[i].set | (table[i].clear << 32); }
    *consumed = !strncmp(key, "x-", 2) || !strncmp(key, "comment=", 8);
    return 0;
}
void mount_fstab(void) {
    fat32_file_t f;
    if (!mnt_lookup("/etc/fstab", &f) || f.is_dir || !f.size || f.size > 65536) return;
    char* text = (void*)arena_alloc(f.size + 1); if (!text) return;
    if (mnt_read_file(&f, 0, f.size, text) != (int32_t)f.size) { arena_free((uint64_t)text, f.size + 1); return; }
    text[f.size] = 0;
    for (char* line = text; line && *line;) {
        char* end = s_strchr(line, '\n'); if (end) *end = 0;
        char* fields[6] = {0}; unsigned n = 0; char* p = line;
        while (*p && n < 6) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '#') break;
            fields[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
        line = end ? end + 1 : NULL;
        if (n < 3 || !strcmp(fields[1], "/") || !strcmp(fields[2], "swap") || !strcmp(fields[1], "none")) continue;
        char options[256] = "", raw[256]; strncpy(raw, n > 3 ? fields[3] : "defaults", sizeof(raw) - 1); raw[sizeof(raw) - 1] = 0;
        uint64_t flags = 0; bool noauto = false, nofail = false;
        char* cursor = raw; char key[64], value[160];
        while (mount_option(&cursor, key, sizeof(key), value, sizeof(value))) {
            if (!strcmp(key, "noauto")) { noauto = true; continue; }
            if (!strcmp(key, "nofail")) nofail = true;
            bool consumed; uint64_t bits = flag_option(key, &consumed);
            if (consumed) { flags = (flags | (bits & 0xFFFFFFFFULL)) & ~(bits >> 32); continue; }
            if (strlen(options) + strlen(key) + strlen(value) + 3 < sizeof(options)) { if (options[0]) strcat(options, ","); strcat(options, key); if (value[0]) { strcat(options, "="); strcat(options, value); } }
        }
        if (noauto || !strncmp(fields[2], "fuse", 4)) continue;
        char device[FAT32_PATH_MAX];
        if (!strncmp(fields[0], "UUID=", 5) || !strncmp(fields[0], "LABEL=", 6) || !strncmp(fields[0], "PARTUUID=", 9) || !strncmp(fields[0], "PARTLABEL=", 10)) {
            if (fsprobe_resolve(fields[0], device, sizeof(device))) { if (!nofail) { vga_print("[64] FSTAB no device for "); vga_print(fields[0]); vga_print("\n"); } continue; }
        } else strncpy(device, fields[0], sizeof(device) - 1), device[sizeof(device) - 1] = 0;
        int error;
        bool bind = false; { char* c = raw; while (mount_option(&c, key, sizeof(key), value, sizeof(value))) if (!strcmp(key, "bind")) bind = true; }
        if (bind) error = mount_bind(device, fields[1], flags);
        else error = mount_new(device, fields[1], fields[2], flags, options, true);
        if (error) { vga_print("[64] FSTAB mount failed: "); vga_print(fields[1]); char e[24]; number64((uint64_t)-error, e); vga_print(" errno="); vga_print(e); vga_print("\n"); }
    }
    arena_free((uint64_t)text, f.size + 1);
}
__attribute__((weak)) int fsprobe_resolve(const char* spec, char* device, unsigned capacity) { (void)spec; (void)device; (void)capacity; return -2; }
/* ---- mountapi.c: fsopen/fsconfig/fsmount/move_mount/open_tree/fspick/mount_setattr do the same operations ---- */
bool mount_path_readonly(const char* path) { struct mount* m = mount_find(path, NULL); return m && readonly(m); }   /* kernel.c */
bool mount_type_known(const char* type) { const struct mount_ops* ops = type_named(type); return ops && ops->mount; }
int mount_api_new(const char* source, const char* target, const char* type, uint64_t flags, char* data) { reap_detached(); return mount_new(source, target, type, flags, data, false); }
int mount_api_bind(const char* source, const char* target, uint64_t flags) { reap_detached(); return mount_bind(source, target, flags); }
int mount_api_move(const char* source, const char* target) { reap_detached(); return mount_move(source, target); }
int mount_api_remount(const char* target, uint64_t flags, char* data) { reap_detached(); return remount(target, flags, data); }
/* the mount whose root `path` is: 1 (*flags, *type), 0 the root filesystem ("/"), -EINVAL not a mount root */
int mount_api_root(const char* path, uint64_t* flags, char* type, unsigned capacity) {
    struct mount* m = NULL;
    for (struct mount* c = mounts; c; c = c->next) if (!c->detached && !strcmp(c->path, path)) m = c;
    if (m) { *flags = m->flags | (m->propagation_shared ? MS_SHARED : 0); strncpy(type, m->type, capacity - 1); type[capacity - 1] = 0; return 1; }
    if (strcmp(path, "/")) return -22;
    *flags = disk_is_readonly() ? MS_RDONLY : 0; strncpy(type, disk_posix() ? "ext4" : "vfat", capacity - 1); type[capacity - 1] = 0; return 0;
}
void mount_api_propagation(const char* path, bool shared) {
    for (struct mount* c = mounts; c; c = c->next) if (!c->detached && !strcmp(c->path, path)) c->propagation_shared = shared;
}
