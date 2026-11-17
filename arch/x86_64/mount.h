/* Mounted filesystems beside the root (mount.c): a mount table keyed by canonical absolute paths, mount(2),
 * umount2(2), pivot_root(2), /proc/mounts and /proc/self/mountinfo, /etc/fstab at boot, and the routing of the
 * kernel's fat32_ and disk_ file API (vfs.h redirects it here) to the filesystem a path or handle belongs to.
 *
 * Handles: fat32_file_t.mount is 0 for the root filesystem, else the mount id. A mounted driver fills the
 * POSIX fields of the handle (ino, mode with type bits, uid, gid, nlink, rdev, size, blocks, times), keeps its
 * own location data in first/dir_lba/dir_off/parent/entry_index/cursor (dir_lba != 0), and the mount layer
 * sets mount, native_ino (MOUNT_INO) and the root entry's name. Paths given to drivers are relative to the
 * mount root without a leading '/' ("" = the root). Unlinking follows the ext4 driver's contract: the victim
 * is returned with detached set when its last link went, and release() frees it after its last user.
 * Errors are negative errno values; read/write return byte counts. */
#ifndef NEXUS_MOUNT_H
#define NEXUS_MOUNT_H
#include "native.h"
#include "ext4.h"

#define MS_RDONLY 1ULL
#define MS_NOSUID 2ULL
#define MS_NODEV 4ULL
#define MS_NOEXEC 8ULL
#define MS_SYNCHRONOUS 16ULL
#define MS_REMOUNT 32ULL
#define MS_MANDLOCK 64ULL
#define MS_DIRSYNC 128ULL
#define MS_NOSYMFOLLOW 256ULL
#define MS_NOATIME 1024ULL
#define MS_NODIRATIME 2048ULL
#define MS_BIND 4096ULL
#define MS_MOVE 8192ULL
#define MS_REC 16384ULL
#define MS_SILENT 32768ULL
#define MS_POSIXACL (1ULL << 16)
#define MS_UNBINDABLE (1ULL << 17)
#define MS_PRIVATE (1ULL << 18)
#define MS_SLAVE (1ULL << 19)
#define MS_SHARED (1ULL << 20)
#define MS_RELATIME (1ULL << 21)
#define MS_KERNMOUNT (1ULL << 22)
#define MS_I_VERSION (1ULL << 23)
#define MS_STRICTATIME (1ULL << 24)
#define MS_LAZYTIME (1ULL << 25)
#define MS_PROPAGATION (MS_SHARED | MS_SLAVE | MS_PRIVATE | MS_UNBINDABLE)
#define MNT_FORCE 1u
#define MNT_DETACH 2u
#define MNT_EXPIRE 4u
#define UMOUNT_NOFOLLOW 8u

#define MOUNT_INO(m, ino) (0x4000000000000000ULL | ((uint64_t)(m)->id << 44) | ((uint64_t)(ino) & ((1ULL << 44) - 1)))
#define MOUNT_ROOT_LBA 0xFFFFFFFFU

struct mount;
struct mount_statfs { uint64_t type, block_size, blocks, free, available, files, free_files, name_max; uint32_t fsid[2]; };
struct mount_attr { unsigned valid; uint32_t mode, uid, gid; int64_t atime, mtime, ctime; uint64_t size; };   /* EXT4_ATTR_* + SIZE */
#define MOUNT_ATTR_SIZE 64u
struct mount_ops {
    const char* name;
    unsigned flags;                    /* MOUNT_NEEDS_BLOCK, MOUNT_POSIX (real owners/modes), MOUNT_USER_OK */
    int (*mount)(struct mount* m, char* options);
    int (*unmount)(struct mount* m, bool force);
    int (*sync)(struct mount* m);
    int (*statfs)(struct mount* m, struct mount_statfs* out);
    int (*remount)(struct mount* m, uint64_t flags, char* options);
    int (*lookup)(struct mount* m, const char* path, fat32_file_t* out);
    int (*refresh)(struct mount* m, fat32_file_t* file);
    int64_t (*read)(struct mount* m, fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer);
    int64_t (*write)(struct mount* m, fat32_file_t* file, uint64_t offset, uint64_t length, const void* buffer);
    int (*truncate)(struct mount* m, fat32_file_t* file, uint64_t size);
    int (*readdir)(struct mount* m, const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out);  /* 1, 0 end */
    int (*create)(struct mount* m, const char* path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, const char* target, fat32_file_t* out);
    int (*unlink)(struct mount* m, const char* path, int directory, fat32_file_t* victim);
    int (*release)(struct mount* m, fat32_file_t* file);
    int (*rename)(struct mount* m, const char* from, const char* to, unsigned flags, fat32_file_t* moved, fat32_file_t* replaced);
    int (*link)(struct mount* m, fat32_file_t* file, const char* path);
    int (*setattr)(struct mount* m, fat32_file_t* file, const struct mount_attr* attr);
    int (*fsync)(struct mount* m, fat32_file_t* file);
    int (*fallocate)(struct mount* m, fat32_file_t* file, unsigned mode, uint64_t offset, uint64_t length);
    int64_t (*xattr)(struct mount* m, fat32_file_t* file, unsigned op, const char* name, void* value, uint64_t size, unsigned flags);
    uint64_t max_file_size;
};
#define MOUNT_NEEDS_BLOCK 1u
#define MOUNT_POSIX 2u
#define MOUNT_USER_OK 4u      /* the desktop user may mount it from removable media */
#define MOUNT_FUSE 8u
#define MOUNT_F_DEFAULT_PERMISSIONS 1u   /* FUSE: the kernel checks modes */
#define MOUNT_F_ALLOW_OTHER 2u           /* FUSE: users other than the mounter may enter */
struct mount {
    unsigned id, parent;
    char path[FAT32_PATH_MAX];         /* canonical absolute mount point */
    unsigned path_length;
    char root_name[256];               /* last component of the mount point: the root entry's name */
    char type[32], source[160], options[256];
    uint64_t flags;                    /* MS_* */
    const struct mount_ops* ops;       /* NULL: a bind mount of a root-filesystem directory */
    void* sb;                          /* driver instance */
    struct mount* bound;               /* bind mounts of mounted filesystems: the source mount */
    char subdir[FAT32_PATH_MAX];       /* bind mounts: the source directory below its filesystem's root */
    struct block_partition* part;      /* block-backed mounts (holder + device reference) */
    uint64_t dev_number;               /* st_dev, Linux encoding */
    uint32_t owner;                    /* user mounts: the mounting uid (may unmount); ~0: none */
    unsigned fsflags;                  /* MOUNT_F_*: FUSE access policy */
    bool detached, dead, created_dir, propagation_shared;
    bool frozen;                       /* its device is a suspended dm device: operations wait at entry (dm.c) */
    struct mount* next;
};
void mount_freeze_device(struct block_device* dev, bool freeze);   /* dm suspend/resume: mounts on dev wait / go on */
/* Lookup helpers for the kernel's policy code (kernel.c hunks). */
struct mount* mount_find(const char* path, const char** rest);   /* NULL: the root filesystem */
struct mount* mount_by_id(unsigned id);
bool mount_path(const char* path);                  /* path is inside a mounted filesystem (not the root's) */
bool disk_file_readonly(const fat32_file_t* file);  /* EROFS for writes to this handle (root or mount) */
uint64_t disk_file_max_size(const fat32_file_t* file);
bool mount_write_through(const fat32_file_t* file);   /* FUSE and MS_SYNCHRONOUS mounts: write(2) writes the data out */
bool mount_statfs_dev(uint64_t dev, uint64_t values[12]); /* statfs for files whose st_dev belongs to a mount */
bool mount_syscall_targets_mount(struct registers* r);    /* attribute syscalls: route to vfs_call (mnt_vfs_call) */
int64_t mount_call(struct registers* r);            /* mount, umount2, pivot_root */
unsigned mount_proc_text(char* out, unsigned capacity, bool mountinfo);
void mount_fstab(void);                             /* boot: mount /etc/fstab entries */
void mount_device_removed(struct block_device* dev); /* block layer: the device left (mounts go dead) */
unsigned mount_count(void);
extern const struct mount_ops vfat_mount_ops, exfat_mount_ops, iso9660_mount_ops, fuse_mount_ops, fuseblk_mount_ops;
/* Shared helpers for the drivers. */
void mount_fill_times(fat32_file_t* file, int64_t atime, int64_t mtime, int64_t ctime);
bool mount_option(char** cursor, char* key, unsigned key_size, char* value, unsigned value_size); /* "k=v,k2" */
uint64_t mount_identity(const fat32_file_t* file);   /* (generation << 32) | ino of a mounted file */
uint32_t mount_current_uid(void);
uint32_t mount_current_gid(void);
unsigned filesystem_umask(void);                                     /* kernel.c */
bool credentials_kernel_from_visible(uint32_t id, bool group, uint32_t* result); /* credentials.c */
#endif
