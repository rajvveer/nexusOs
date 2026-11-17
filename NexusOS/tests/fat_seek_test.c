/* Real FAT seeks must avoid repeated prefix walks and invalidate stale aliases. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#include "../kernel/fat32.c"
#define SECTORS 8192
static uint8_t disk[SECTORS][512];
static unsigned read_sectors;
static uint32_t failed_lba = 0xFFFFFFFFu;
bool ata_disk_present(void) { return true; }
uint32_t ata_get_sectors(void) { return SECTORS; }
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    assert(lba < SECTORS && count <= SECTORS - lba); read_sectors += count;
    if (failed_lba >= lba && failed_lba - lba < count) return -1;
    memcpy(buffer, disk[lba], count * 512); return 0;
}
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    assert(lba < SECTORS && count <= SECTORS - lba);
    memcpy(disk[lba], buffer, count * 512); return 0;
}
void vga_print(const char* s) { (void)s; }
void vga_print_color(const char* s, uint8_t color) { (void)s; (void)color; }
void uint_to_str(uint32_t n, char* s) { sprintf(s, "%u", n); }
void int_to_str(int n, char* s) { sprintf(s, "%d", n); }
int main(void) {
    fat32_bpb_t valid = {0}; valid.bytes_per_sector = 512; valid.sectors_per_cluster = 1;
    valid.reserved_sectors = 2; valid.num_fats = 2; valid.fat_size_32 = 64;
    valid.total_sectors_32 = SECTORS; valid.root_cluster = 2;
    memcpy(disk[0], &valid, sizeof(valid)); disk[0][510] = 0x55; disk[0][511] = 0xAA;
    for (unsigned copy = 0; copy < 2; copy++) {
        uint32_t* fat = (void*)disk[2 + copy * 64]; fat[0] = fat[1] = fat[2] = FAT32_EOC;
        for (unsigned i = 0; i < 3000; i++) fat[10 + i] = i == 2999 ? FAT32_EOC : 11 + i;
    }
    fat32_dirent_t* entry = (void*)disk[130]; memcpy(entry->name, "LARGE   BIN", 11);
    entry->attr = ATTR_ARCHIVE; entry->first_cluster_lo = 10; entry->file_size = 3000 * 512;
    for (unsigned i = 0; i < 3000; i++) memset(disk[138 + i], i % 251, 512);
    assert(fat32_init()); fat32_file_t reader, writer, other;
    assert(fat32_lookup("large.bin", &reader));
    uint8_t byte; read_sectors = 0;
    assert(fat32_read_file(&reader, 2500 * 512, 1, &byte) == 1 && byte == 2500 % 251);
    unsigned cold = read_sectors; assert(cold > 2);
    for (unsigned i = 0; i < 100; i++) {
        unsigned block = (i * 113 + 91) % 2500; read_sectors = 0;
        assert(fat32_read_file(&reader, block * 512, 1, &byte) == 1 && byte == block % 251);
        assert(read_sectors <= 2);
    }
    printf("PASS FAT contiguous random seeks: cold=%u sectors, each of 100 warm seeks<=2 sectors\n", cold);
    fat32_file_t stale = reader;
    assert(fat32_read_file(&stale, 512, 1, &byte) == 1 && byte == 1);
    assert(fat32_lookup("large.bin", &writer) && fat32_truncate_file(&writer, 512) == 0);
    assert(fat32_create_path("other.bin", false, &other) == 0);
    uint8_t occupied[4096]; memset(occupied, 0xB7, sizeof(occupied));
    assert(fat32_write_file(&other, 0, sizeof(occupied), occupied) == sizeof(occupied));
    assert(fat32_truncate_file(&writer, 1536) == 0 && fat32_write_file(&writer, 512, 1, "R") == 1);
    assert(fat32_read_file(&reader, 512, 1, &byte) == 1 && byte == 'R');
    assert(fat32_write_file(&stale, 512, 1, "W") == 1);
    assert(fat32_read_file(&writer, 512, 1, &byte) == 1 && byte == 'W');
    uint8_t actual[4096]; assert(fat32_read_file(&other, 0, sizeof(actual), actual) == sizeof(actual));
    assert(!memcmp(actual, occupied, sizeof(actual)));
    assert(fat32_read_file(&reader, 1024, 1, &byte) == 1 && byte == 0);
    assert(fat32_init() && fat32_read_file(&reader, 512, 1, &byte) == 1 && byte == 'W');
    chain_epoch = 0xFFFFFFFFu;
    assert(fat32_read_file(&reader, 512, 1, &byte) == 1 && byte == 'W');
    assert(fat32_truncate_file(&writer, 512) == 0 && fat32_read_file(&reader, 512, 1, &byte) == 0);
    puts("PASS FAT seek cache: fragmented regrowth, independent stale read/write handles, remount and saturated epoch");

    /* A fresh volume with two fully interleaved large files reproduces installed-ELF page faults. */
    memset(disk, 0, sizeof(disk)); chain_epoch = 0;
    memcpy(disk[0], &valid, sizeof(valid)); disk[0][510] = 0x55; disk[0][511] = 0xAA;
    for (unsigned copy = 0; copy < 2; copy++) {
        uint32_t* fat = (void*)disk[2 + copy * 64]; fat[0] = fat[1] = fat[2] = FAT32_EOC;
        for (unsigned i = 0; i < 3000; i++) {
            fat[10 + i * 2] = i == 2999 ? FAT32_EOC : 12 + i * 2;
            fat[11 + i * 2] = i == 2999 ? FAT32_EOC : 13 + i * 2;
        }
    }
    entry = (void*)disk[130]; memcpy(entry->name, "FRAG    BIN", 11);
    entry->attr = ATTR_ARCHIVE; entry->first_cluster_lo = 10; entry->file_size = 3000 * 512;
    entry++; memcpy(entry->name, "GUARD   BIN", 11); entry->attr = ATTR_ARCHIVE; entry->first_cluster_lo = 11; entry->file_size = 3000 * 512;
    for (unsigned i = 0; i < 3000; i++) { memset(disk[138 + i * 2], i % 251, 512); memset(disk[139 + i * 2], 0xB7, 512); }
    assert(fat32_init() && fat32_lookup("frag.bin", &reader)); read_sectors = 0;
    assert(fat32_read_file(&reader, 2500 * 512, 1, &byte) == 1 && byte == 2500 % 251);
    cold = read_sectors; assert(cold > 10); unsigned worst = 0;
    for (unsigned i = 0; i < 100; i++) {
        unsigned block = (i * 113 + 91) % 2500; read_sectors = 0;
        assert(fat32_read_file(&reader, block * 512, 1, &byte) == 1 && byte == block % 251);
        if (read_sectors > worst) worst = read_sectors;
        assert(read_sectors <= 10);
    }
    fat32_file_t alias; assert(fat32_lookup("frag.bin", &alias)); read_sectors = 0;
    assert(fat32_read_file(&alias, 2300 * 512, 1, &byte) == 1 && byte == 2300 % 251 && read_sectors <= 10);
    printf("PASS FAT fragmented random seeks: cold=%u sectors, 100 warm/alias reads at most %u sectors\n", cold, worst);

    assert(fat32_lookup("frag.bin", &writer) && fat32_truncate_file(&writer, 512) == 0);
    assert(fat32_create_path("reuse.bin", false, &other) == 0 && fat32_truncate_file(&other, 1024 * 512) == 0);
    assert(fat32_truncate_file(&writer, 3000 * 512) == 0);
    assert(fat32_write_file(&writer, 2300 * 512, 4, "NEW!") == 4);
    assert(fat32_read_file(&alias, 2300 * 512, 1, &byte) == 1 && byte == 'N');
    assert(fat32_lookup("guard.bin", &other) && fat32_read_file(&other, 2300 * 512, 1, &byte) == 1 && byte == 0xB7);
    assert(fat32_init()); failed_lba = fat_start_lba + (fat_read_entry(writer.first) * 4) / 512;
    assert(failed_lba < fat_cache_start || failed_lba >= fat_cache_start + FAT_CACHE_SECTORS);
    volume_cache_invalidate(failed_lba, 1); /* Inject the failure below the read cache. */
    assert(fat32_read_file(&reader, 2300 * 512, 1, &byte) == -1); failed_lba = 0xFFFFFFFFu;
    assert(fat32_read_file(&reader, 2300 * 512, 1, &byte) == 1 && byte == 'N');
    failed_lba = cluster_to_lba(reader.cursor);
    volume_cache_invalidate(failed_lba, 1);
    assert(fat32_read_file(&reader, 2300 * 512, 1, &byte) == -1); failed_lba = 0xFFFFFFFFu;
    assert(fat32_read_file(&reader, 2300 * 512, 1, &byte) == 1 && byte == 'N');
    chain_epoch = 0xFFFFFFFFu;
    assert(fat32_read_file(&alias, 2300 * 512, 1, &byte) == 1 && byte == 'N');
    assert(fat32_truncate_file(&writer, 512) == 0 && fat32_read_file(&alias, 2300 * 512, 1, &byte) == 0);
    puts("PASS FAT sparse checkpoints: same-size chain replacement, guarded neighbor, remount, read faults and saturated epoch");
}
