/* Block-device seam for the root filesystems: the only file in the ext4/VFS code that talks to a disk
 * driver. ext4_dev_find() locates the ext4 volume, ext4_dev_read/write address it in 4 KiB filesystem
 * blocks, ext4_dev_flush() empties the device's volatile write cache, vfs_flush_devices() is the FAT32
 * root's flush.
 * Two backends, chosen at compile time:
 *  - the storage layer (blockdev.h: IDE/AHCI/NVMe, MBR/GPT): the first partition, in the block layer's
 *    probe order, that holds an ext4 superblock and is typed GPT NexusOS-root, GPT Linux filesystem data
 *    (0FC63DAF-8483-4772-8E79-3D69D8477DE4) or MBR 0x83, else an unpartitioned disk holding ext4;
 *  - the legacy IDE driver (kernel/ata.c) when blockdev.h is absent: an MBR 0x83 partition starting at or
 *    beyond LBA 32768 (clear of the legacy raw-LBA windows), else an unpartitioned disk. */
#include "ext4_internal.h"
#include "../../kernel/string.h"

static uint8_t probe_buffer[1024];
static uint64_t dev_sectors;
uint64_t ext4_dev_sectors(void) { return dev_sectors; }
static bool range_ok(uint64_t block, uint32_t count, uint64_t* sector, uint64_t* n) {
    *sector = block * EXT4_SECTORS_PER_BLOCK; *n = (uint64_t)count * EXT4_SECTORS_PER_BLOCK;
    return *sector < dev_sectors && *n <= dev_sectors - *sector && *n <= 0xFFFFFFFFULL;
}

#if __has_include("blockdev.h")
#include "blockdev.h"
static const struct block_partition* volume;
int block_root_wanted(struct block_partition* p);
static bool has_ext4(const struct block_partition* part) {
    return block_part_sectors(part) > 2048 && block_part_read(part, 2, 2, probe_buffer) == 0 && le16(probe_buffer + SB_MAGIC) == 0xEF53;
}
bool ext4_dev_find(void) {
    static const uint8_t nexus_root[16] = BLOCK_GUID_NEXUS_ROOT, linux_data[16] = BLOCK_GUID_LINUX_DATA;
    if (block_unlocked_root && has_ext4(block_unlocked_root)) { volume = block_unlocked_root; dev_sectors = block_part_sectors(volume); return true; }   /* LUKS root */
    for (int pass = 0; pass < 4; pass++)
        for (unsigned i = 0; i < block_partition_count(); i++) {
            const struct block_partition* part = block_partition_get(i);
            if (!part) continue;
            bool gpt = part->scheme == BLOCK_SCHEME_GPT;
            int wanted = block_root_wanted((struct block_partition*)part);   /* root= (fsprobe.c) */
            if (!wanted) continue;
            bool match = wanted > 0 ? pass == 0 : pass == 0 ? gpt && !memcmp(part->type_guid, nexus_root, 16)
                       : pass == 1 ? gpt && !memcmp(part->type_guid, linux_data, 16)
                       : pass == 2 ? part->scheme == BLOCK_SCHEME_MBR && part->mbr_type == 0x83
                       : part->scheme == BLOCK_SCHEME_WHOLE;
            if (match && has_ext4(part)) { volume = part; dev_sectors = block_part_sectors(part); return true; }
        }
    return false;
}
int ext4_dev_read(uint64_t block, uint32_t count, void* buffer) {
    uint64_t sector, n; if (!count) return 0;
    if (!volume || !range_ok(block, count, &sector, &n)) return -E_IO;
    return block_part_read(volume, sector, (uint32_t)n, buffer) < 0 ? -E_IO : 0;
}
int ext4_dev_write(uint64_t block, uint32_t count, const void* buffer) {
    uint64_t sector, n; if (!count) return 0;
    if (!volume || !range_ok(block, count, &sector, &n)) return -E_IO;
    return block_part_write(volume, sector, (uint32_t)n, buffer) < 0 ? -E_IO : 0;
}
int ext4_dev_flush(void) { return volume && block_flush(volume->dev) < 0 ? -E_IO : 0; }
const struct block_partition* ext4_dev_partition(void) { return volume; }
bool ext4_dev_readonly(void) { return volume && volume->dev->read_only; }
int vfs_flush_devices(void) { return block_flush_all() < 0 ? -E_IO : 0; }

#else
#include "../../kernel/ata.h"
static uint32_t first_lba;
static bool magic_at(uint32_t lba) {
    if (ata_read_sectors(lba + 2, 2, probe_buffer) < 0) return false;
    return le16(probe_buffer + SB_MAGIC) == 0xEF53;
}
bool ext4_dev_find(void) {
    if (!ata_disk_present()) return false;
    uint64_t disk = ata_get_sectors();
    static uint8_t mbr[512];
    if (ata_read_sectors(0, 1, mbr) < 0) return false;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA)
        for (int off = 446; off < 510; off += 16) {
            uint32_t start = le32(mbr + off + 8), count = le32(mbr + off + 12);
            if (mbr[off + 4] == 0x83 && start >= 32768 && count && start < disk && count <= disk - start && magic_at(start)) {
                first_lba = start; dev_sectors = count; return true;
            }
        }
    if (disk > 2048 && magic_at(0)) { first_lba = 0; dev_sectors = disk; return true; }
    return false;
}
int ext4_dev_read(uint64_t block, uint32_t count, void* buffer) {
    uint64_t sector, n; if (!count) return 0;
    if (!range_ok(block, count, &sector, &n) || first_lba + sector + n > 0xFFFFFFFFULL) return -E_IO;
    return ata_read_sectors((uint32_t)(first_lba + sector), (uint32_t)n, buffer) < 0 ? -E_IO : 0;
}
int ext4_dev_write(uint64_t block, uint32_t count, const void* buffer) {
    uint64_t sector, n; if (!count) return 0;
    if (!range_ok(block, count, &sector, &n) || first_lba + sector + n > 0xFFFFFFFFULL) return -E_IO;
    return ata_write_sectors((uint32_t)(first_lba + sector), (uint32_t)n, buffer) < 0 ? -E_IO : 0;
}
int ext4_dev_flush(void) { return ata_flush() < 0 ? -E_IO : 0; }
const struct block_partition* ext4_dev_partition(void) { return NULL; }
bool ext4_dev_readonly(void) { return false; }
int vfs_flush_devices(void) { return ata_flush() < 0 ? -E_IO : 0; }
#endif
