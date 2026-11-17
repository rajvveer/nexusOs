/* Root-filesystem dispatch for the native kernel (vfs.c).
 *
 * The kernel's file layer is written against the fat32_* API with fat32_file_t handles. The boot
 * mounts whichever root volume the disk carries -- ext4 (MBR type 0x83 partition at LBA >= 32768,
 * or a whole-disk ext4) or FAT32 -- and the macros below route every fat32_* call made by
 * arch/x86_64 code to the disk_* functions in vfs.c, which call the mounted driver. Offsets and
 * sizes are 64-bit here (ext4 files may exceed 4 GiB). vfs.c also provides the POSIX semantics FAT32
 * cannot express (ownership, modes, hard links, device/fifo nodes, timestamps, permission checks) and
 * the attribute syscalls; on FAT32 those keep the Linux vfat behaviour. */
#ifndef NEXUS_VFS_H
#define NEXUS_VFS_H
#include "../../kernel/types.h"
#include "../../kernel/fat32.h"

bool     disk_mount_root(void);
bool     disk_root_present(void);   /* an ext4 root exists (FAT32 roots come from block_probe) */
bool     disk_posix(void);               /* ext4 root: real owners, modes and links */
bool     disk_is_readonly(void);
bool     disk_lookup(const char* path, fat32_file_t* out);
int      disk_lookup_status(const char* path, fat32_file_t* out);
int      disk_refresh_file(fat32_file_t* file);
int32_t  disk_read_file(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer);
int      disk_read_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out);
int      disk_read_dir_at(const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out); /* 1 entry (cookie advanced), 0 end */
int      disk_create_path(const char* path, bool is_dir, fat32_file_t* out);
int      disk_create_socket_path(const char* path, fat32_file_t* out);
int      disk_write_file(fat32_file_t* file, uint64_t offset, uint64_t length, const void* buffer);
int      disk_truncate_file(fat32_file_t* file, uint64_t size);
int      disk_remove_path(const char* path);
int      disk_rename_path(const char* from, const char* to, bool no_replace, fat32_file_t* moved, fat32_file_t* replaced);
int      disk_detach_path(const char* path, fat32_file_t* detached);
int      disk_dispose_file(fat32_file_t* file);
int      disk_begin_download(const char* path, fat32_file_t* out);
int      disk_finish_download(fat32_file_t* file);
int      disk_finish_symlink(fat32_file_t* file);
int      disk_abort_download(fat32_file_t* file);
int      disk_set_times(fat32_file_t* file, uint16_t date, uint16_t time);
int      disk_stat_volume(fat32_space_t* space);

#ifndef VFS_IMPLEMENTATION
#define fat32_init disk_mount_root
#define fat32_is_readonly disk_is_readonly
#define fat32_lookup disk_lookup
#define fat32_lookup_status disk_lookup_status
#define fat32_refresh_file disk_refresh_file
#define fat32_read_file disk_read_file
#define fat32_read_dir_entry disk_read_dir_entry
#define fat32_create_path disk_create_path
#define fat32_create_socket_path disk_create_socket_path
#define fat32_write_file disk_write_file
#define fat32_truncate_file disk_truncate_file
#define fat32_remove_path disk_remove_path
#define fat32_rename_path disk_rename_path
#define fat32_detach_path disk_detach_path
#define fat32_dispose_file disk_dispose_file
#define fat32_begin_download disk_begin_download
#define fat32_finish_download disk_finish_download
#define fat32_finish_symlink disk_finish_symlink
#define fat32_abort_download disk_abort_download
#define fat32_set_times disk_set_times
#define fat32_stat_volume disk_stat_volume
#endif

/* Linux-errno operations used directly by the kernel. */
#define DISK_MAY_EXEC 1
#define DISK_MAY_WRITE 2
#define DISK_MAY_READ 4
#define DISK_KIND_FIFO 3
#define DISK_KIND_CHR 4
#define DISK_KIND_BLK 5
int      disk_create_node(const char* path, uint32_t mode, uint32_t rdev, const char* target, fat32_file_t* out);
int      disk_permission(const fat32_file_t* file, unsigned mask);
int      disk_may_search(const char* directory);      /* x permission on a disk directory path */
int      disk_parent_access(const char* path);         /* create/remove in the parent of path */
int      disk_delete_access(const char* path, const fat32_file_t* victim); /* + sticky directories */
int      disk_open_access(const fat32_file_t* file, uint64_t flags);
int      disk_access(const fat32_file_t* file, unsigned mask, bool effective_ids);
int      disk_exec_access(const fat32_file_t* file);  /* execute permission (ext4) */
void     disk_exec_arm(const fat32_file_t* file);      /* set-user/group-ID for the following execve commit */
void     disk_exec_finished(void);
bool     disk_exec_secure(const fat32_file_t* file);   /* AT_SECURE preview: its set-ID bits change the real IDs */
uint64_t disk_max_file_size(void);
int64_t  disk_mtime_ns(const fat32_file_t* file);
int      disk_file_stamp(fat32_file_t* file, uint64_t changed_ns);
void     disk_write_privs(fat32_file_t* file);         /* writes drop set-ID bits (no CAP_FSETID) */
int      vfs_flush_devices(void);                       /* FAT32 root: flush the disk write cache (ext4_dev.c) */
int      disk_sync(void);                               /* fsync/syncfs: journal commit + device flush */
void     disk_writeback_complete(bool all, bool wrote);
void     disk_commit_poll(void);
void     disk_shutdown(void);                           /* clean unmount before power-off */
void     disk_test_poweroff_after(unsigned commits);    /* crash tests: stop after the Nth durable journal commit */
struct linux_stat;
struct memory_file;
bool     disk_stat(const fat32_file_t* file, struct memory_file* cache, struct linux_stat* st);
struct registers;
int64_t  vfs_call(struct registers* r);                 /* attribute/link/node/time/statx syscalls */
int      disk_seek_data(fat32_file_t* file, struct memory_file* cache, uint64_t offset, bool hole, uint64_t* result);
void     disk_statfs(uint64_t* type, uint64_t* files, uint64_t* free_files);
struct block_partition;
int      disk_file_extents(fat32_file_t* file, const struct block_partition** part, int (*emit)(void* context, uint64_t offset, uint64_t sector, uint64_t sectors), void* context);
int      vfs_fchmod(void* description, uint64_t mode);

/* Mounted filesystems (mount.c): everywhere but vfs.c and mount.c the root API above is reached through the
 * mount table first; a path inside a mount point or a handle with fat32_file_t.mount != 0 goes to its driver. */
#if !defined(VFS_IMPLEMENTATION) && !defined(MOUNT_IMPLEMENTATION)
bool     mnt_lookup(const char* path, fat32_file_t* out);
int      mnt_lookup_status(const char* path, fat32_file_t* out);
int      mnt_refresh_file(fat32_file_t* file);
int32_t  mnt_read_file(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer);
int      mnt_read_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out);
int      mnt_read_dir_at(const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out);
int      mnt_create_path(const char* path, bool is_dir, fat32_file_t* out);
int      mnt_create_socket_path(const char* path, fat32_file_t* out);
int      mnt_write_file(fat32_file_t* file, uint64_t offset, uint64_t length, const void* buffer);
int      mnt_truncate_file(fat32_file_t* file, uint64_t size);
int      mnt_remove_path(const char* path);
int      mnt_rename_path(const char* from, const char* to, bool no_replace, fat32_file_t* moved, fat32_file_t* replaced);
int      mnt_detach_path(const char* path, fat32_file_t* detached);
int      mnt_dispose_file(fat32_file_t* file);
int      mnt_begin_download(const char* path, fat32_file_t* out);
int      mnt_finish_download(fat32_file_t* file);
int      mnt_finish_symlink(fat32_file_t* file);
int      mnt_abort_download(fat32_file_t* file);
int      mnt_set_times(fat32_file_t* file, uint16_t date, uint16_t time);
int      mnt_create_node(const char* path, uint32_t mode, uint32_t rdev, const char* target, fat32_file_t* out);
int      mnt_permission(const fat32_file_t* file, unsigned mask);
int      mnt_may_search(const char* directory);
int      mnt_parent_access(const char* path);
int      mnt_delete_access(const char* path, const fat32_file_t* victim);
int      mnt_open_access(const fat32_file_t* file, uint64_t flags);
int      mnt_access(const fat32_file_t* file, unsigned mask, bool effective_ids);
int      mnt_exec_access(const fat32_file_t* file);
void     mnt_exec_arm(const fat32_file_t* file);
void     mnt_exec_finished(void);
int64_t  mnt_mtime_ns(const fat32_file_t* file);
int      mnt_file_stamp(fat32_file_t* file, uint64_t changed_ns);
void     mnt_write_privs(fat32_file_t* file);
int      mnt_sync(void);
void     mnt_writeback_complete(bool all, bool wrote);
void     mnt_commit_poll(void);
void     mnt_shutdown(void);
bool     mnt_stat(const fat32_file_t* file, struct memory_file* cache, struct linux_stat* st);
int      mnt_seek_data(fat32_file_t* file, struct memory_file* cache, uint64_t offset, bool hole, uint64_t* result);
int64_t  mnt_vfs_call(struct registers* r);
int      mnt_vfs_fchmod(void* description, uint64_t mode);
#undef fat32_lookup
#define fat32_lookup mnt_lookup
#undef fat32_lookup_status
#define fat32_lookup_status mnt_lookup_status
#undef fat32_refresh_file
#define fat32_refresh_file mnt_refresh_file
#undef fat32_read_file
#define fat32_read_file mnt_read_file
#undef fat32_read_dir_entry
#define fat32_read_dir_entry mnt_read_dir_entry
#undef fat32_create_path
#define fat32_create_path mnt_create_path
#undef fat32_create_socket_path
#define fat32_create_socket_path mnt_create_socket_path
#undef fat32_write_file
#define fat32_write_file mnt_write_file
#undef fat32_truncate_file
#define fat32_truncate_file mnt_truncate_file
#undef fat32_remove_path
#define fat32_remove_path mnt_remove_path
#undef fat32_rename_path
#define fat32_rename_path mnt_rename_path
#undef fat32_detach_path
#define fat32_detach_path mnt_detach_path
#undef fat32_dispose_file
#define fat32_dispose_file mnt_dispose_file
#undef fat32_begin_download
#define fat32_begin_download mnt_begin_download
#undef fat32_finish_download
#define fat32_finish_download mnt_finish_download
#undef fat32_finish_symlink
#define fat32_finish_symlink mnt_finish_symlink
#undef fat32_abort_download
#define fat32_abort_download mnt_abort_download
#undef fat32_set_times
#define fat32_set_times mnt_set_times
#define disk_lookup mnt_lookup
#define disk_read_dir_at mnt_read_dir_at
#define disk_create_node mnt_create_node
#define disk_permission mnt_permission
#define disk_may_search mnt_may_search
#define disk_parent_access mnt_parent_access
#define disk_delete_access mnt_delete_access
#define disk_open_access mnt_open_access
#define disk_access mnt_access
#define disk_exec_access mnt_exec_access
#define disk_exec_arm mnt_exec_arm
#define disk_exec_finished mnt_exec_finished
#define disk_mtime_ns mnt_mtime_ns
#define disk_file_stamp mnt_file_stamp
#define disk_write_privs mnt_write_privs
#define disk_sync mnt_sync
#define disk_writeback_complete mnt_writeback_complete
#define disk_commit_poll mnt_commit_poll
#define disk_shutdown mnt_shutdown
#define disk_stat mnt_stat
#define disk_seek_data mnt_seek_data
#define vfs_call mnt_vfs_call
#define vfs_fchmod mnt_vfs_fchmod
#endif
#endif
