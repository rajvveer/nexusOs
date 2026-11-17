/* The new mount API (kabi, wave 2): fsopen(2), fsconfig(2), fsmount(2), move_mount(2), open_tree(2), fspick(2),
 * mount_setattr(2), enough for util-linux libmount (2.39+ uses them once fsopen exists) and systemd.
 *
 * Model: NexusOS mounts live only attached (mount.c), so the API keeps descriptors that describe what to do:
 *   fsopen(type)            a filesystem context (anon fd "[fscontext]"): fsconfig collects "source", the superblock
 *                           flags (ro, rw, sync, dirsync, lazytime, mand) and other options (key or key=value, as the
 *                           mount(2) data string); FSCONFIG_CMD_CREATE ends the parameter phase (EBUSY afterwards);
 *   fsmount(context)        a detached mount (fd "[fsmount]") with its MOUNT_ATTR_* flags; move_mount attaches it,
 *                           which is where mount.c creates the filesystem (errors of the device or the options appear
 *                           there, not at CMD_CREATE as on Linux);
 *   open_tree(path, CLONE)  a detached bind mount of path (AT_RECURSIVE as a plain bind: no submounts are copied);
 *   open_tree(path)         a reference to the mount at path (move_mount moves it, as MS_MOVE);
 *   fspick(path)            a context to reconfigure the mount at path (FSCONFIG_CMD_RECONFIGURE = remount with the
 *                           superblock flags and options, per-mount flags kept);
 *   mount_setattr(path)     per-mount flags (ro, nosuid, nodev, noexec, atime, nosymfollow) and propagation of an
 *                           attached mount (remount with the same flags) or of a detached one (kept until attached).
 * The filesystem types are mount.c's (vfat, exfat, iso9660, fuse, fuseblk): others are ENODEV, as mount(2) gives.
 * Legacy-style parameters only: FSCONFIG_SET_BINARY/SET_PATH/SET_PATH_EMPTY/SET_FD and CMD_CREATE_EXCL are
 * EOPNOTSUPP (Linux legacy_fs_context). MOUNT_ATTR_IDMAP is EINVAL (no idmapped mounts). Reading a context fd gives its
 * last error message ("e ..."), else ENODATA. Every call needs CAP_SYS_ADMIN (EPERM). Locking: the big kernel lock. */
#include "native.h"
#include "uaccess.h"
#include "mount.h"
#include "blockdev.h"
#include "../../kernel/string.h"

struct block_partition* storage_partition(unsigned device);

int kernel_user_path(uint64_t address, char* path);
int kernel_path_at(uint64_t directory, char* path, bool follow, struct open_file** bound);
int kernel_resolved_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* st);
bool mount_type_known(const char* type);
int mount_api_new(const char* source, const char* target, const char* type, uint64_t flags, char* data);
int mount_api_bind(const char* source, const char* target, uint64_t flags);
int mount_api_move(const char* source, const char* target);
int mount_api_remount(const char* target, uint64_t flags, char* data);
int mount_api_root(const char* path, uint64_t* flags, char* type, unsigned capacity);
void mount_api_propagation(const char* path, bool shared);

#define AT_FDCWD ((uint64_t)-100)
#define AT_SYMLINK_NOFOLLOW 0x100ULL
#define AT_NO_AUTOMOUNT 0x800ULL
#define AT_EMPTY_PATH 0x1000ULL
#define AT_RECURSIVE 0x8000ULL
#define OPEN_TREE_CLONE 1ULL
#define OPEN_TREE_CLOEXEC 0x80000ULL
#define MOUNT_ATTR_RDONLY 0x1ULL
#define MOUNT_ATTR_NOSUID 0x2ULL
#define MOUNT_ATTR_NODEV 0x4ULL
#define MOUNT_ATTR_NOEXEC 0x8ULL
#define MOUNT_ATTR__ATIME 0x70ULL
#define MOUNT_ATTR_RELATIME 0x0ULL
#define MOUNT_ATTR_NOATIME 0x10ULL
#define MOUNT_ATTR_STRICTATIME 0x20ULL
#define MOUNT_ATTR_NODIRATIME 0x80ULL
#define MOUNT_ATTR_IDMAP 0x100000ULL
#define MOUNT_ATTR_NOSYMFOLLOW 0x200000ULL
#define VALID_ATTRS (MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC | MOUNT_ATTR__ATIME | MOUNT_ATTR_NODIRATIME | MOUNT_ATTR_IDMAP | MOUNT_ATTR_NOSYMFOLLOW)
#define PER_MOUNT (MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_NODIRATIME | MS_RELATIME | MS_STRICTATIME | MS_NOSYMFOLLOW)
#define SB_FLAGS (MS_RDONLY | MS_SYNCHRONOUS | MS_DIRSYNC | MS_LAZYTIME | MS_MANDLOCK)

enum { CONTEXT, DETACHED_NEW, DETACHED_BIND, ATTACHED };
struct mount_object {
    unsigned kind, phase;          /* CONTEXT: 0 parameters, 1 created, 2 mounted (fsmount done); reconfigure: fspick */
    bool reconfigure;
    char type[32], source[FAT32_PATH_MAX], path[FAT32_PATH_MAX], data[512], message[128];
    uint64_t sb_set, sb_clear;     /* superblock flags from fsconfig */
    uint64_t attrs;                /* MS_* per-mount flags of a detached mount */
    struct block_partition* held;  /* the source block device, held from CMD_CREATE (Linux opens it there): a loop
                                    * device with autoclear stays bound after libmount closes its descriptor */
};
static bool may_mount(void) { return credentials_capable(credentials_current(), NULL, 21); }
static void note(struct mount_object* o, const char* text) {
    unsigned n = 2; strcpy(o->message, "e ");
    while (*text && n < sizeof(o->message) - 1) o->message[n++] = *text++;
    o->message[n] = 0;   /* "e <text>", no newline (Linux fscontext_read) */
}

/* ---- the descriptors ---- */
static int64_t object_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    (void)flags; struct mount_object* o = object;
    if (write || o->kind != CONTEXT) return -22;   /* (fsmount/open_tree fds are O_PATH-like: EBADF from the handler too) */
    if (!o->message[0]) return -61;                 /* ENODATA: no message */
    uint64_t n = strlen(o->message); if (n > total) return -90;   /* EMSGSIZE */
    if (!iov_access(vectors, count, 0, n, true)) return -14;
    iov_copy(vectors, count, 0, (uint8_t*)o->message, n, false); o->message[0] = 0; return (int64_t)n;
}
static int object_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) { (void)object; (void)command; (void)argument; (void)flags; return -25; }
static uint32_t object_ready(void* object) { struct mount_object* o = object; return o->message[0] ? 1U : 0; }
static void* object_source(void* object) { return object; }
static void release_hold(struct mount_object* o) { if (o->held) { block_put(o->held->dev); o->held = NULL; } }
static void hold_source(struct mount_object* o) {
    if (!o->source[0] || o->held) return;
    char path[FAT32_PATH_MAX]; strcpy(path, o->source); struct open_file* bound = NULL;
    if (kernel_path_at(AT_FDCWD, path, true, &bound)) return;
    int device = device_lookup(path); if (device < 0) return;
    struct block_partition* part = storage_partition((unsigned)device); if (!part) return;
    block_get(part->dev); o->held = part;
}
static void object_release(void* object) { release_hold(object); kfree(object, sizeof(struct mount_object)); }
static void object_stat(void* object, struct linux_stat* stat) {
    struct mount_object* o = object;
    if (o->kind != CONTEXT && !kernel_resolved_stat(o->kind == DETACHED_BIND ? o->source : o->path, NULL, true, stat)) return;
    memset(stat, 0, sizeof(*stat)); stat->dev = 6; stat->ino = 1 + ((uint64_t)object >> 3 & 0xFFFFFFFULL); stat->nlink = 1; stat->mode = 0600;
    stat->block_size = 4096;
}
static const struct object_operations mount_operations = {object_io, object_ioctl, object_ready, object_source, object_release, object_stat};
static int install(struct mount_object* o, bool cloexec, const char* name) {
    int fd = fd_create_object(&mount_operations, o, (o->kind == CONTEXT ? 2 : 0x200000) | (cloexec ? 0x80000 : 0), name);
    if (fd < 0) object_release(o);
    return fd;
}
static struct mount_object* object_new(unsigned kind) {
    struct mount_object* o = kalloc(sizeof(*o)); if (!o) return NULL;
    memset(o, 0, sizeof(*o)); o->kind = kind; return o;
}
static struct mount_object* object_at(uint64_t fd) { struct open_file* f = fd_lookup(fd); return f ? fd_object(f, &mount_operations) : NULL; }
/* a path argument (dirfd + string, AT_EMPTY_PATH: the descriptor's own path) as a canonical path */
static int path_of(uint64_t directory, uint64_t address, bool empty, bool follow, char* path) {
    int error = kernel_user_path(address, path); if (error) return error;
    if (!path[0]) {
        if (!empty) return -2;
        struct open_file* f = fd_lookup(directory); if (!f) return -9;
        struct mount_object* o = fd_object(f, &mount_operations);
        const char* p = o ? (o->kind == DETACHED_BIND ? o->source : o->path) : fd_internal_path(f);
        if (!p || p[0] != '/') return -22;
        strcpy(path, p); return 0;
    }
    struct open_file* bound = NULL;
    if ((error = kernel_path_at(directory, path, follow, &bound))) return error;
    if (bound) { const char* p = fd_internal_path(bound); if (!p || p[0] != '/') return -22; strcpy(path, p); }
    struct linux_stat st; return kernel_resolved_stat(path, bound, follow, &st);
}
static uint64_t attrs_to_flags(uint64_t attrs) {
    return (attrs & MOUNT_ATTR_RDONLY ? MS_RDONLY : 0) | (attrs & MOUNT_ATTR_NOSUID ? MS_NOSUID : 0) | (attrs & MOUNT_ATTR_NODEV ? MS_NODEV : 0) |
           (attrs & MOUNT_ATTR_NOEXEC ? MS_NOEXEC : 0) | (attrs & MOUNT_ATTR_NODIRATIME ? MS_NODIRATIME : 0) |
           (attrs & MOUNT_ATTR_NOSYMFOLLOW ? MS_NOSYMFOLLOW : 0) |
           ((attrs & MOUNT_ATTR__ATIME) == MOUNT_ATTR_NOATIME ? MS_NOATIME : (attrs & MOUNT_ATTR__ATIME) == MOUNT_ATTR_STRICTATIME ? MS_STRICTATIME : MS_RELATIME);
}

/* ---- fsopen / fsconfig / fsmount / fspick ---- */
static int64_t fsopen_call(uint64_t name_address, uint64_t flags) {
    if (!may_mount()) return -1;
    if (flags & ~1ULL) return -22;
    char name[64]; int error = kernel_user_path(name_address, name);
    if (error) return error == -36 ? -19 : error;
    if (!name[0] || !mount_type_known(name)) return -19;   /* ENODEV */
    struct mount_object* o = object_new(CONTEXT); if (!o) return -12;
    strncpy(o->type, name, sizeof(o->type) - 1);
    return install(o, flags & 1, "anon_inode:[fscontext]");
}
static int64_t fspick_call(uint64_t directory, uint64_t address, uint64_t flags) {
    if (flags & ~0xFULL) return -22;
    if (!may_mount()) return -1;
    char path[FAT32_PATH_MAX]; int error = path_of(directory, address, flags & 8, !(flags & 2), path); if (error) return error;
    uint64_t current; char type[32]; int root = mount_api_root(path, &current, type, sizeof(type)); if (root < 0) return root;
    struct mount_object* o = object_new(CONTEXT); if (!o) return -12;
    o->reconfigure = true; strcpy(o->path, path); strcpy(o->type, type);
    return install(o, flags & 1, "anon_inode:[fscontext]");
}
static int add_option(struct mount_object* o, const char* key, const char* value) {
    unsigned used = strlen(o->data), need = strlen(key) + (value ? strlen(value) + 1 : 0) + (used ? 1 : 0);
    if (used + need >= sizeof(o->data)) return -12;
    if (used) strcat(o->data, ",");
    strcat(o->data, key); if (value) { strcat(o->data, "="); strcat(o->data, value); }
    return 0;
}
static int64_t fsconfig_call(uint64_t fd, uint64_t command, uint64_t key_address, uint64_t value_address, int64_t aux) {
    if ((int64_t)(int32_t)fd < 0) return -22;
    switch (command) {
    case 0: if (!key_address || value_address || aux) return -22; break;                                   /* SET_FLAG */
    case 1: if (!key_address || !value_address || aux) return -22; break;                                  /* SET_STRING */
    case 2: if (!key_address || !value_address || aux <= 0 || aux > 1024 * 1024) return -22; break;        /* SET_BINARY */
    case 3: case 4: if (!key_address || !value_address || ((int32_t)aux != -100 && aux < 0)) return -22; break;   /* SET_PATH(_EMPTY) */
    case 5: if (!key_address || value_address || aux < 0) return -22; break;                               /* SET_FD */
    case 6: case 7: case 8: if (key_address || value_address || aux) return -22; break;                    /* CMD_CREATE/RECONFIGURE/CREATE_EXCL */
    default: return -95;
    }
    struct open_file* f = fd_lookup(fd); if (!f) return -9;
    struct mount_object* o = fd_object(f, &mount_operations); if (!o || o->kind != CONTEXT) return -22;
    if (command == 2 || command == 3 || command == 4 || command == 5 || command == 8) return -95;   /* legacy contexts (Linux) */
    if (command == 6 || command == 7) {
        if (!may_mount()) return -1;
        if ((command == 6) == o->reconfigure) { note(o, "operation does not match the context"); return -16; }
        if (command == 6) {
            if (o->phase) return -16;
            hold_source(o); o->phase = 1; return 0;
        }
        uint64_t current; char type[32]; int root = mount_api_root(o->path, &current, type, sizeof(type)); if (root < 0) return root;
        uint64_t flags = ((current & ~o->sb_clear) | o->sb_set) & (SB_FLAGS | PER_MOUNT);
        int error = mount_api_remount(o->path, flags | MS_REMOUNT, o->data[0] ? o->data : NULL);
        if (error) note(o, "reconfiguration failed");
        return error;
    }
    if (o->phase) return -16;   /* parameters after CMD_CREATE */
    char key[64], value[256] = "";
    int error = kernel_user_path(key_address, key); if (error) return error == -36 ? -22 : error;
    if (command == 1 && (error = kernel_user_path(value_address, value))) return error == -36 ? -22 : error;
    if (!strcmp(key, "source")) {
        if (command != 1) return -22;
        if (o->source[0]) { note(o, "VFS: Multiple sources"); return -22; }
        if (o->reconfigure) return 0;   /* ignored on reconfiguration */
        strncpy(o->source, value, sizeof(o->source) - 1); return 0;
    }
    static const struct { const char* name; uint64_t flag; bool set; } sb[] = {
        {"ro", MS_RDONLY, true}, {"rw", MS_RDONLY, false}, {"sync", MS_SYNCHRONOUS, true}, {"async", MS_SYNCHRONOUS, false},
        {"dirsync", MS_DIRSYNC, true}, {"lazytime", MS_LAZYTIME, true}, {"nolazytime", MS_LAZYTIME, false}, {"mand", MS_MANDLOCK, true}, {"nomand", MS_MANDLOCK, false}};
    for (unsigned i = 0; i < sizeof(sb) / sizeof(sb[0]); i++) if (!strcmp(key, sb[i].name)) {
        if (command != 0) return -22;
        if (sb[i].set) { o->sb_set |= sb[i].flag; o->sb_clear &= ~sb[i].flag; } else { o->sb_clear |= sb[i].flag; o->sb_set &= ~sb[i].flag; }
        return 0;
    }
    return add_option(o, key, command == 1 ? value : NULL);
}
static int64_t fsmount_call(uint64_t fd, uint64_t flags, uint64_t attrs) {
    if (!may_mount()) return -1;
    if (flags & ~1ULL) return -22;
    if (attrs & ~(MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC | MOUNT_ATTR__ATIME | MOUNT_ATTR_NODIRATIME | MOUNT_ATTR_NOSYMFOLLOW)) return -22;
    uint64_t atime = attrs & MOUNT_ATTR__ATIME;
    if (atime != MOUNT_ATTR_RELATIME && atime != MOUNT_ATTR_NOATIME && atime != MOUNT_ATTR_STRICTATIME) return -22;
    struct open_file* f = fd_lookup(fd); if (!f) return -9;
    struct mount_object* c = fd_object(f, &mount_operations); if (!c || c->kind != CONTEXT) return -22;
    if (c->reconfigure || c->phase != 1) return -16;   /* EBUSY: not created, or already mounted */
    struct mount_object* m = object_new(DETACHED_NEW); if (!m) return -12;
    strcpy(m->type, c->type); strcpy(m->source, c->source); strcpy(m->data, c->data);
    m->sb_set = c->sb_set; m->attrs = attrs_to_flags(attrs); c->phase = 2;
    m->held = c->held; c->held = NULL;   /* the device hold moves to the mount */
    return install(m, flags & 1, "anon_inode:[fsmount]");
}

/* ---- open_tree / move_mount / mount_setattr ---- */
static int64_t open_tree_call(uint64_t directory, uint64_t address, uint64_t flags) {
    if (flags & ~(AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_RECURSIVE | AT_SYMLINK_NOFOLLOW | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)) return -22;
    if ((flags & (AT_RECURSIVE | OPEN_TREE_CLONE)) == AT_RECURSIVE) return -22;
    if ((flags & OPEN_TREE_CLONE) && !may_mount()) return -1;
    char path[FAT32_PATH_MAX]; int error = path_of(directory, address, flags & AT_EMPTY_PATH, !(flags & AT_SYMLINK_NOFOLLOW), path); if (error) return error;
    struct mount_object* o = object_new(flags & OPEN_TREE_CLONE ? DETACHED_BIND : ATTACHED); if (!o) return -12;
    if (flags & OPEN_TREE_CLONE) strcpy(o->source, path); else strcpy(o->path, path);
    return install(o, flags & OPEN_TREE_CLOEXEC, flags & OPEN_TREE_CLONE ? "anon_inode:[open_tree]" : path);
}
static int64_t move_mount_call(uint64_t from_dir, uint64_t from_address, uint64_t to_dir, uint64_t to_address, uint64_t flags) {
    if (flags & ~0x377ULL) return -22;   /* F_SYMLINKS F_AUTOMOUNTS F_EMPTY_PATH T_SYMLINKS T_AUTOMOUNTS T_EMPTY_PATH SET_GROUP BENEATH */
    if (!may_mount()) return -1;
    if (flags & 0x300) return -22;   /* MOVE_MOUNT_SET_GROUP, MOVE_MOUNT_BENEATH: peer groups and stacking are not kept */
    char target[FAT32_PATH_MAX]; int error = path_of(to_dir, to_address, flags & 0x40, (flags & 0x10) != 0, target); if (error) return error;
    char first = 1; if (accessible(from_address, 1, false)) first = (char)uget8(from_address);
    struct mount_object* o = (flags & 4) && !first ? object_at(from_dir) : NULL;
    if (o && o->kind == CONTEXT) return -22;
    if (o && o->kind == DETACHED_NEW) {
        error = mount_api_new(o->source[0] ? o->source : NULL, target, o->type, o->attrs | o->sb_set, o->data[0] ? o->data : NULL);
        if (!error) { o->kind = ATTACHED; strcpy(o->path, target); release_hold(o); }   /* the mount holds the device now */
        return error;
    }
    if (o && o->kind == DETACHED_BIND) {
        error = mount_api_bind(o->source, target, o->attrs);
        if (!error) { o->kind = ATTACHED; strcpy(o->path, target); }
        return error;
    }
    char source[FAT32_PATH_MAX];
    if (o) strcpy(source, o->path);
    else if ((error = path_of(from_dir, from_address, flags & 4, (flags & 1) != 0, source))) return error;
    if ((error = mount_api_move(source, target))) return error;
    if (o) strcpy(o->path, target);
    return 0;
}
static int64_t mount_setattr_call(uint64_t directory, uint64_t address, uint64_t flags, uint64_t attr_address, uint64_t size) {
    if (flags & ~(AT_EMPTY_PATH | AT_RECURSIVE | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT)) return -22;
    if (size > 4096) return -7;
    if (size < 32) return -22;
    if (!may_mount()) return -1;
    uint64_t attr[4] = {0, 0, 0, 0};   /* attr_set, attr_clr, propagation, userns_fd */
    if (!accessible(attr_address, size, false)) return -14;
    copy_from_user(attr, (const void*)attr_address, 32);
    for (uint64_t at = 32; at < size; at++) { uint8_t byte = uget8(attr_address + at); if (byte) return -7; }
    if (!attr[0] && !attr[1] && !attr[2]) return 0;
    if (attr[2] && attr[2] != MS_SHARED && attr[2] != MS_SLAVE && attr[2] != MS_PRIVATE && attr[2] != MS_UNBINDABLE) return -22;
    if ((attr[0] | attr[1]) & ~VALID_ATTRS) return -22;
    if ((attr[0] | attr[1]) & MOUNT_ATTR_IDMAP) return -22;   /* no idmapped mounts */
    uint64_t set = attrs_to_flags(attr[0] & ~MOUNT_ATTR__ATIME) & ~MS_RELATIME, clear = attrs_to_flags(attr[1] & ~MOUNT_ATTR__ATIME) & ~MS_RELATIME;
    if (attr[1] & MOUNT_ATTR__ATIME) {
        if ((attr[1] & MOUNT_ATTR__ATIME) != MOUNT_ATTR__ATIME) return -22;
        clear |= MS_RELATIME | MS_NOATIME | MS_STRICTATIME;
        uint64_t atime = attr[0] & MOUNT_ATTR__ATIME;
        if (atime == MOUNT_ATTR_RELATIME) set |= MS_RELATIME; else if (atime == MOUNT_ATTR_NOATIME) set |= MS_NOATIME;
        else if (atime == MOUNT_ATTR_STRICTATIME) set |= MS_STRICTATIME; else return -22;
    } else if (attr[0] & MOUNT_ATTR__ATIME) return -22;
    char first = 1; if (accessible(address, 1, false)) first = (char)uget8(address);
    struct mount_object* o = (flags & AT_EMPTY_PATH) && !first ? object_at(directory) : NULL;
    if (o && o->kind == CONTEXT) return -22;
    if (o && o->kind != ATTACHED) { o->attrs = (o->attrs & ~clear) | set; return 0; }   /* a detached mount */
    char path[FAT32_PATH_MAX]; int error;
    if (o) strcpy(path, o->path);
    else if ((error = path_of(directory, address, flags & AT_EMPTY_PATH, !(flags & AT_SYMLINK_NOFOLLOW), path))) return error;
    uint64_t current; char type[32]; int root = mount_api_root(path, &current, type, sizeof(type)); if (root < 0) return root;
    if (attr[2]) mount_api_propagation(path, attr[2] == MS_SHARED);
    if (!set && !clear) return 0;
    uint64_t next = ((current & ~clear) | set) & (PER_MOUNT | SB_FLAGS);
    if (!root && (next & ~MS_RDONLY) == (current & ~MS_RDONLY & (PER_MOUNT | SB_FLAGS)) && (next & MS_RDONLY) == (current & MS_RDONLY)) return 0;
    return mount_api_remount(path, next | MS_REMOUNT, NULL);
}

/* 428 open_tree, 429 move_mount, 430 fsopen, 431 fsconfig, 432 fsmount, 433 fspick, 442 mount_setattr */
int64_t mountapi_call(struct registers* r) {
    switch (r->rax) {
    case 428: return open_tree_call(r->rdi, r->rsi, (uint32_t)r->rdx);
    case 429: return move_mount_call(r->rdi, r->rsi, r->rdx, r->r10, (uint32_t)r->r8);
    case 430: return fsopen_call(r->rdi, (uint32_t)r->rsi);
    case 431: return fsconfig_call((uint32_t)r->rdi, (uint32_t)r->rsi, r->rdx, r->r10, (int64_t)(int32_t)r->r8);
    case 432: return fsmount_call((uint32_t)r->rdi, (uint32_t)r->rsi, (uint32_t)r->rdx);
    case 433: return fspick_call(r->rdi, r->rsi, (uint32_t)r->rdx);
    case 442: return mount_setattr_call(r->rdi, r->rsi, (uint32_t)r->rdx, r->r10, r->r8);
    }
    return -38;
}
