/* A failed directory read must not publish stale sector bytes as a mount. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#include "../kernel/fat32.c"
static uint8_t disk[128][512];
static uint32_t fail_lba = ~0u;
static uint32_t fail_write_lba = ~0u, writes[32];
static unsigned write_count;
static unsigned read_count;
static bool allow_writes;
bool ata_disk_present(void) { return true; }
uint32_t ata_get_sectors(void) { return 128; }
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    read_count++;
    if (lba >= 128 || count > 128 - lba || (fail_lba >= lba && fail_lba - lba < count)) return -1;
    memcpy(buffer, disk[lba], count * 512); return 0;
}
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    assert(allow_writes && lba < 128 && count <= 128 - lba && write_count < 32);
    writes[write_count++] = lba;
    if (fail_write_lba >= lba && fail_write_lba - lba < count) return -1;
    memcpy(disk[lba], buffer, count * 512); return 0;
}
void vga_print(const char* s) { (void)s; }
void vga_print_color(const char* s, uint8_t color) { (void)s; (void)color; }
void uint_to_str(uint32_t n, char* s) { sprintf(s, "%u", n); }
void int_to_str(int n, char* s) { sprintf(s, "%d", n); }
int main(void) {
    fat32_bpb_t valid = {0};
    valid.bytes_per_sector = 512; valid.sectors_per_cluster = 1; valid.reserved_sectors = 2;
    valid.num_fats = 1; valid.fat_size_32 = 1; valid.total_sectors_32 = 128; valid.root_cluster = 2;
    memcpy(disk[0], &valid, sizeof(valid)); disk[0][510] = 0x55; disk[0][511] = 0xAA;
    assert(fat32_init() && fat32_is_mounted() && dir_node_count == 0);
    /* A previous successful mount must not survive either failure. */
    fail_lba = 3;
    assert(!fat32_init() && !fat32_is_mounted() && dir_node_count == 0 && io_error);
    for (unsigned i = 0; i < 16; i++) disk[3][i * 32] = 0xE5;
    memcpy(disk[3], "EXAMPLE TXT", 11); disk[3][11] = ATTR_ARCHIVE;
    fail_lba = 2;
    assert(!fat32_init() && !fat32_is_mounted() && dir_node_count == 0 && io_error);
    uint32_t end = FAT32_EOC; memcpy(disk[2] + 8, &end, 4); fail_lba = ~0u;
    assert(fat32_init() && fat32_is_mounted() && dir_node_count == 1 && !io_error);
    puts("PASS FAT mount rejects root-sector/chain read errors and discards partial directory cache");
    fat32_dirent_t* file_entry = (void*)disk[3];
    file_entry->first_cluster_lo = 3; file_entry->file_size = 1536;
    uint32_t* fat = (void*)disk[2]; fat[3] = 4; fat[4] = 5; fat[5] = FAT32_EOC;
    assert(fat32_init()); fat32_file_t file; assert(fat32_lookup("example.txt", &file)); allow_writes = true;
    fat32_space_t space; assert(fat32_stat_volume(&space) == 0 && space.cluster_bytes == 512 && space.total_clusters == 125 && space.free_clusters == 121);
    unsigned reads = read_count; assert(fat32_stat_volume(&space) == 0 && read_count == reads);
    fail_write_lba = 3;
    assert(fat32_truncate_file(&file, 512) == -1 && file_entry->file_size == 1536 && fat[3] == 4 && fat[4] == 5);
    fail_write_lba = 2; write_count = 0;
    assert(fat32_truncate_file(&file, 512) == -1 && file_entry->file_size == 512 && fat[3] == 4 && fat[4] == 5);
    assert(writes[0] == 3 && writes[1] == 2);
    assert(fat32_stat_volume(&space) == 0 && space.free_clusters == 121);
    file_entry->file_size = 1536; fail_write_lba = ~0u; write_count = 0;
    assert(fat32_init() && fat32_lookup("example.txt", &file));
    assert(fat32_stat_volume(&space) == 0 && space.free_clusters == 121);
    assert(fat32_truncate_file(&file, 512) == 0 && file_entry->file_size == 512 && fat[3] >= FAT32_EOC && fat[4] == 0 && fat[5] == 0);
    assert(fat32_stat_volume(&space) == 0 && space.free_clusters == 123);
    puts("PASS FAT truncate commits the smaller directory entry before freeing/reusing tail clusters");
    disk[3][32] = 0; assert(fat32_init()); fail_lba = 2; space.free_clusters = 999;
    volume_cache_reset(); /* The injected device failure must reach ATA, not a warm cache. */
    assert(fat32_stat_volume(&space) == -1 && space.free_clusters == 999);
    fail_lba = ~0u; assert(fat32_stat_volume(&space) == 0 && space.free_clusters == 123);
    puts("PASS FAT free-space scan/cache, mutation deltas and read/write failure recovery");
}
