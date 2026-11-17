/* Exercise the production block cache with real buffers and a bounded fake disk. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#include "../kernel/fat32.c"

#define SECTORS 1024
#define PARTITION 32768
static uint8_t disk[2][SECTORS][512];
static unsigned reads, writes, read_sectors;
static uint32_t last_lba, last_count;
static int fail_read_after = -1, fail_write_after = -1;
static bool present = true;
static struct {
    uint8_t before[32], data[(VOLUME_CACHE_SLOTS + 2) * 4096], after[32];
} buffer;

bool ata_disk_present(void) { return present; }
uint32_t ata_get_sectors(void) { return PARTITION + SECTORS; }
static uint8_t* backing(uint32_t lba, uint32_t count) {
    unsigned bank = lba >= PARTITION;
    if (bank) lba -= PARTITION;
    assert(lba < SECTORS && count && count <= SECTORS - lba);
    return disk[bank][lba];
}
int ata_read_sectors(uint32_t lba, uint32_t count, void* out) {
    uint8_t* src = backing(lba, count);
    reads++; read_sectors += count; last_lba = lba; last_count = count;
    uint32_t copied = fail_read_after < 0 ? count : (uint32_t)fail_read_after;
    if (copied > count) copied = count;
    memcpy(out, src, copied * 512);
    return fail_read_after < 0 ? 0 : -1;
}
int ata_write_sectors(uint32_t lba, uint32_t count, const void* data) {
    uint8_t* dst = backing(lba, count);
    /* Invalidation has to precede even the first partially successful sector. */
    for (unsigned i = 0; i < VOLUME_CACHE_SLOTS; i++) {
        struct volume_cache_entry* entry = &volume_cache[i];
        assert(!entry->valid || entry->lba >= (uint64_t)(lba - volume_lba) + count ||
               (uint64_t)entry->lba + VOLUME_CACHE_SECTORS <= lba - volume_lba);
    }
    writes++;
    uint32_t copied = fail_write_after < 0 ? count : (uint32_t)fail_write_after;
    if (copied > count) copied = count;
    memcpy(dst, data, copied * 512);
    return fail_write_after < 0 ? 0 : -1;
}
void vga_print(const char* s) { (void)s; }
void vga_print_color(const char* s, uint8_t color) { (void)s; (void)color; }
void uint_to_str(uint32_t n, char* s) { sprintf(s, "%u", n); }
void int_to_str(int n, char* s) { sprintf(s, "%d", n); }

static void guarded_read(uint32_t lba, uint32_t count) {
    assert(count * 512 < sizeof(buffer.data));
    memset(&buffer, 0xA5, sizeof(buffer));
    assert(volume_read(lba, count, buffer.data) == 0);
    assert(!memcmp(buffer.data, backing(volume_lba + lba, count), count * 512));
    for (unsigned i = 0; i < sizeof(buffer.before); i++) assert(buffer.before[i] == 0xA5);
    for (unsigned i = count * 512; i < sizeof(buffer.data); i++) assert(buffer.data[i] == 0xA5);
    for (unsigned i = 0; i < sizeof(buffer.after); i++) assert(buffer.after[i] == 0xA5);
}
static void format(unsigned bank, unsigned total) {
    memset(disk[bank], 0, sizeof(disk[bank]));
    fat32_bpb_t valid = {0};
    valid.bytes_per_sector = 512; valid.sectors_per_cluster = 1;
    valid.reserved_sectors = 2; valid.num_fats = 1; valid.fat_size_32 = 8;
    valid.total_sectors_32 = total; valid.root_cluster = 2;
    memcpy(disk[bank][0], &valid, sizeof(valid));
    disk[bank][0][510] = 0x55; disk[bank][0][511] = 0xAA;
    uint32_t* fat = (void*)disk[bank][2]; fat[0] = fat[1] = fat[2] = FAT32_EOC;
    memset(disk[bank][64], 0x30 + bank, 512);
}
int main(void) {
    for (unsigned bank = 0; bank < 2; bank++) for (unsigned lba = 0; lba < SECTORS; lba++)
        for (unsigned byte = 0; byte < 512; byte++) disk[bank][lba][byte] = (uint8_t)(bank * 91 + lba * 7 + byte);
    volume_lba = 0; volume_sectors = SECTORS - 3;
    volume_cache_reset();
    guarded_read(19, 1); assert(reads == 1 && read_sectors == 1);
    for (unsigned i = 0; i < 1000; i++) guarded_read(19, 1);
    assert(reads == 1 && read_sectors == 1);
    guarded_read(20, 1); assert(reads == 2 && last_lba == 20 && last_count == 1);
    guarded_read(19, 3); assert(reads == 3 && last_lba == 19 && last_count == 3);
    guarded_read(19, 3); assert(reads == 3);
    guarded_read(7, 9); assert(reads == 4 && last_lba == 7 && last_count == 9);
    guarded_read(7, 9); assert(reads == 4);
    guarded_read(131, 8); assert(reads == 5 && last_lba == 131 && last_count == 8);
    guarded_read(131, 8); assert(reads == 5);
    guarded_read(volume_sectors - 3, 3); assert(last_count == 3);
    unsigned before = reads;
    guarded_read(volume_sectors - 3, 3); assert(reads == before);
    memset(&buffer, 0xA5, sizeof(buffer));
    assert(volume_read(volume_sectors - 1, 2, buffer.data) == -1 && io_error && reads == before);
    assert(volume_read(~0u, 1, buffer.data) == -1 && reads == before);
    assert(volume_read(0, ~0u, buffer.data) == -1 && reads == before);
    assert(volume_read(0, 1, NULL) == -1 && reads == before);
    assert(volume_read(volume_sectors, 0, NULL) == 0 && reads == before);
    for (unsigned i = 0; i < sizeof(buffer); i++) assert(((uint8_t*)&buffer)[i] == 0xA5);
    puts("PASS FAT block cache: 1001 repeated sector reads use one ATA read; partial/cross-block hits, exact cold request and tail bounds");

    volume_cache_reset(); reads = 0;
    for (unsigned i = 0; i < VOLUME_CACHE_SLOTS; i++) guarded_read(i * 8, 1);
    guarded_read(0, 1); assert(reads == VOLUME_CACHE_SLOTS);
    guarded_read(VOLUME_CACHE_SLOTS * 8, 1);
    guarded_read(0, 1); assert(reads == VOLUME_CACHE_SLOTS + 1);
    guarded_read(8, 1); assert(reads == VOLUME_CACHE_SLOTS + 2);
    volume_cache_reset(); before = reads;
    guarded_read(200, (VOLUME_CACHE_SLOTS + 1) * 8);
    assert(reads == before + 1 && last_lba == 200 && last_count == (VOLUME_CACHE_SLOTS + 1) * 8);
    guarded_read(200 + VOLUME_CACHE_SLOTS * 8, 8); assert(reads == before + 1);
    guarded_read(200, 1); assert(reads == before + 2);
    volume_cache_clock = ~(uint64_t)0;
    guarded_read(200, 1); assert(reads == before + 3);
    puts("PASS FAT block cache: bounded LRU eviction, oversized requests stay one ATA read, clock wrap discards cache safely");

    uint8_t replacement[4 * 512]; memset(replacement, 0x71, sizeof(replacement));
    guarded_read(64, 16); guarded_read(88, 1); before = reads;
    assert(volume_write(70, 4, replacement) == 0 && writes == 1);
    guarded_read(88, 1); assert(reads == before);
    guarded_read(64, 16); assert(reads == before + 1 && !memcmp(disk[0][70], replacement, sizeof(replacement)));
    for (unsigned partial = 0; partial <= 4; partial += 2) {
        memset(replacement, 0x80 + partial, sizeof(replacement));
        fail_write_after = partial; before = reads; io_error = false;
        assert(volume_write(70, 4, replacement) == -1 && io_error);
        fail_write_after = -1;
        guarded_read(64, 16); assert(reads == before + 1);
        assert(!memcmp(disk[0][70], replacement, partial * 512));
    }
    writes_blocked = true; before = writes;
    assert(volume_write(70, 4, replacement) == -1 && writes == before);
    writes_blocked = false; guarded_read(64, 16);
    before = writes;
    assert(volume_write(volume_sectors, 1, replacement) == -1 && writes == before);
    puts("PASS FAT block cache: overlap invalidated before successful, rejected, partial and fully-written failed writes; neighbors preserved");

    volume_cache_reset(); guarded_read(64, 1); before = reads;
    fail_read_after = 2; io_error = false;
    assert(volume_read(64, 4, buffer.data) == -1 && io_error && reads == before + 1);
    struct volume_cache_entry* entry = volume_cache_find(64);
    assert(entry && entry->valid == 1);
    fail_read_after = -1; disk[0][65][0] ^= 0xFF;
    guarded_read(64, 4); assert(reads == before + 2);
    guarded_read(64, 4); assert(reads == before + 2);
    puts("PASS FAT block cache: failed reads publish no partial bytes into cache; retry reads fresh disk data");

    format(0, SECTORS); assert(fat32_init()); guarded_read(64, 1);
    memset(disk[0][64], 0x5C, 512); assert(fat32_init()); guarded_read(64, 1);
    assert(buffer.data[0] == 0x5C);
    present = false; assert(!fat32_init());
    for (unsigned i = 0; i < VOLUME_CACHE_SLOTS; i++) assert(!volume_cache[i].valid);
    present = true; disk[0][0][510] = 0; assert(!fat32_init());
    format(0, SECTORS); assert(fat32_init()); guarded_read(64, 1);
    format(1, SECTORS - 3); memset(disk[0][0], 0, 512);
    disk[0][0][510] = 0x55; disk[0][0][511] = 0xAA; disk[0][0][450] = 0x0C;
    uint32_t start = PARTITION, total = SECTORS;
    memcpy(disk[0][0] + 454, &start, 4); memcpy(disk[0][0] + 458, &total, 4);
    assert(fat32_init() && volume_lba == PARTITION && volume_sectors == SECTORS - 3);
    entry = volume_cache_find(0);
    assert(!entry || !(entry->valid & 1)); /* Mount-time BPB was discarded on final geometry. */
    guarded_read(64, 1); assert(buffer.data[0] == 0x31 && last_lba == PARTITION + 64);
    guarded_read(volume_sectors - 1, 1);
    puts("PASS FAT block cache: successful/failed reinit, partition switch and final BPB geometry discard stale cache");
}
