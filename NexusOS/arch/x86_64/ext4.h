/* Native ext4 driver: read/write, extents, htree directories, metadata_csum, jbd2 journal, orphans.
 * Paths are absolute and canonical ('/'-separated, no symlink following). Results are negative errno.
 * The fat32_file_t handle doubles as the ext4 inode snapshot (see the NEXUS_NATIVE64 fields). */
#ifndef NEXUS_EXT4_H
#define NEXUS_EXT4_H
#include "../../kernel/types.h"
#include "../../kernel/fat32.h"

#define EXT4_KIND_SOCKET 1   /* == FAT32_NATIVE_SOCKET */
#define EXT4_KIND_SYMLINK 2  /* == FAT32_NATIVE_SYMLINK */
#define EXT4_KIND_FIFO 3
#define EXT4_KIND_CHR 4
#define EXT4_KIND_BLK 5
#define EXT4_MAX_FILE_SIZE (((uint64_t)1 << 44) - 4096) /* 2^32 blocks of 4 KiB */

bool ext4_probe(void);                    /* an ext4 volume (MBR type 0x83 at LBA >= 32768, or whole disk) */
int  ext4_mount(void);
bool ext4_is_mounted(void);
bool ext4_is_readonly(void);
int  ext4_unmount_clean(void);            /* commit and mark the volume clean (needs_recovery cleared) */

int  ext4_lookup(const char* path, fat32_file_t* out);
int  ext4_refresh(fat32_file_t* file);
int  ext4_read_dir(const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out); /* 1 entry, 0 end */
int  ext4_create(const char* path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev,
                 const char* link_target, fat32_file_t* out);
int  ext4_link(fat32_file_t* file, const char* path);
int  ext4_unlink(const char* path, int directory /* 1 dir, 0 non-dir, -1 either */, fat32_file_t* victim);
/* An inode created on the orphan list without a name (staged downloads, O_TMPFILE-like), published
 * later under file->name in directory file->parent (optionally as a symbolic link). */
int  ext4_create_unlinked(const char* path, uint32_t mode, uint32_t uid, uint32_t gid, fat32_file_t* out);
int  ext4_link_unlinked(fat32_file_t* file, bool as_symlink);
int  ext4_rename(const char* from, const char* to, bool no_replace, fat32_file_t* moved, fat32_file_t* replaced);
int  ext4_release(fat32_file_t* file);   /* last reference to an unlinked inode is gone: free it */

int64_t ext4_read(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer);
int64_t ext4_write(fat32_file_t* file, uint64_t offset, uint64_t length, const void* buffer);
int  ext4_truncate(fat32_file_t* file, uint64_t size);
int  ext4_fallocate(fat32_file_t* file, unsigned mode, uint64_t offset, uint64_t length);
int  ext4_seek_data(fat32_file_t* file, uint64_t offset, bool hole, uint64_t* result);
int  ext4_file_extents(fat32_file_t* file, int (*emit)(void* context, uint64_t offset, uint64_t sector, uint64_t sectors), void* context);
struct block_partition;
const struct block_partition* ext4_dev_partition(void); /* the mounted volume (swap files), NULL without the block layer */

#define EXT4_ATTR_MODE 1
#define EXT4_ATTR_UID 2
#define EXT4_ATTR_GID 4
#define EXT4_ATTR_ATIME 8
#define EXT4_ATTR_MTIME 16
#define EXT4_ATTR_CTIME 32
struct ext4_attr { unsigned valid; uint32_t mode, uid, gid; int64_t atime, mtime, ctime; };
int  ext4_setattr(fat32_file_t* file, const struct ext4_attr* attr);

struct ext4_volume { uint64_t block_size, blocks, free_blocks, files, free_files; uint32_t fsid[2]; };
int  ext4_statfs(struct ext4_volume* volume);
int  ext4_sync(void);                     /* commit the running transaction: journal + device flush */
void ext4_poll(void);                     /* periodic commit of an aged transaction */
uint64_t ext4_mount_pages(void);
extern unsigned ext4_test_poweroff_after_commits; /* crash tests: power off after the Nth durable commit */
#endif
