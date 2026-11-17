/* Rename must preserve both chains, including live replaced files and I/O faults. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#include "../kernel/fat32.c"
#define SECTORS 1024
static uint8_t disk[SECTORS][512], baseline[SECTORS][512], interrupted[SECTORS][512];
static unsigned reads, writes, fail_read, fail_write;
static unsigned interrupt_after;
static bool fail_after_write, fail_persistent;
bool ata_disk_present(void) { return true; }
uint32_t ata_get_sectors(void) { return SECTORS; }
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    assert(lba < SECTORS && count <= SECTORS - lba);
    if (++reads == fail_read) return -1;
    memcpy(buffer, disk[lba], count * 512); return 0;
}
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    assert(lba < SECTORS && count <= SECTORS - lba);
    writes++; bool fail = writes == fail_write || (fail_persistent && writes > fail_write);
    if (!fail || fail_after_write) memcpy(disk[lba], buffer, count * 512);
    if (writes == interrupt_after) memcpy(interrupted, disk, sizeof(disk));
    return fail ? -1 : 0;
}
void vga_print(const char* s) { (void)s; }
void vga_print_color(const char* s, uint8_t color) { (void)s; (void)color; }
void uint_to_str(uint32_t n, char* s) { sprintf(s, "%u", n); }
void int_to_str(int n, char* s) { sprintf(s, "%d", n); }
static void format(void) {
    memset(disk, 0, sizeof(disk));
    fat32_bpb_t valid = {0}; valid.bytes_per_sector = 512; valid.sectors_per_cluster = 1;
    valid.reserved_sectors = 2; valid.num_fats = 2; valid.fat_size_32 = 8;
    valid.total_sectors_32 = SECTORS; valid.root_cluster = 2;
    memcpy(disk[0], &valid, sizeof(valid)); disk[0][510] = 0x55; disk[0][511] = 0xAA;
    for (unsigned f = 0; f < 2; f++) {
        uint32_t* fat = (void*)disk[2 + f * 8]; fat[0] = fat[1] = fat[2] = FAT32_EOC;
    }
    reads = writes = fail_read = fail_write = 0; fail_after_write = fail_persistent = false;
    assert(fat32_init());
}
static fat32_file_t create(const char* path, const char* contents) {
    fat32_file_t file; assert(fat32_create_path(path, !contents, &file) == 0);
    if (contents) assert(fat32_write_file(&file, 0, strlen(contents), contents) == (int)strlen(contents));
    return file;
}
static void content(fat32_file_t file, const char* expected) {
    char text[128]; unsigned size = strlen(expected);
    assert(file.size == size && size < sizeof(text));
    assert(fat32_read_file(&file, 0, size, text) == (int)size && !memcmp(text, expected, size));
}
static void at(const char* path, const char* expected) {
    fat32_file_t file; assert(fat32_lookup(path, &file)); content(file, expected);
}
static void absent(const char* path) { fat32_file_t file; assert(!fat32_lookup(path, &file)); }
static void restore(void) {
    memcpy(disk, baseline, sizeof(disk));
    fail_read = fail_write = reads = writes = 0; fail_after_write = fail_persistent = false;
    assert(fat32_init()); reads = writes = 0;
    volume_cache_reset(); fat_cache_start = 0xFFFFFFFF;
}
int main(void) {
    format(); create("left", NULL); create("right", NULL);
    fat32_file_t old = create("left/source document.txt", "source"), moved, replaced;
    create("right/destination document.txt", "target");
    fat32_space_t before, during, after; assert(fat32_stat_volume(&before) == 0);
    assert(fat32_rename_path("left/source document.txt", "right/destination document.txt", true, &moved, &replaced) == -17);
    at("left/source document.txt", "source"); at("right/destination document.txt", "target");
    assert(fat32_rename_path("left/source document.txt", "right/destination document.txt", false, &moved, &replaced) == 0);
    absent("left/source document.txt"); at("right/destination document.txt", "source");
    assert(moved.first == old.first && replaced.detached && !moved.detached);
    content(replaced, "target");
    assert(fat32_stat_volume(&during) == 0 && during.free_clusters == before.free_clusters);
    assert(fat32_write_file(&replaced, 0, 3, "old") == 3);
    content(replaced, "oldget"); at("right/destination document.txt", "source");
    assert(fat32_truncate_file(&replaced, 1025) == 0);
    assert(fat32_stat_volume(&during) == 0 && during.free_clusters + 2 == before.free_clusters);
    assert(fat32_dispose_file(&replaced) == 0 && fat32_stat_volume(&after) == 0 && after.free_clusters == before.free_clusters + 1);
    assert(fat32_dispose_file(&moved) == -22);
    char longname[262] = "left/"; memset(longname + 5, 'x', 255); longname[260] = 0;
    assert(fat32_rename_path("right/destination document.txt", longname, false, &moved, &replaced) == 0 && !replaced.detached);
    assert(fat32_init()); at(longname, "source");
    assert(fat32_rename_path(longname, "right/caf\xc3\xa9.txt", false, &moved, &replaced) == 0);
    assert(fat32_detach_path("right/caf\xc3\xa9.txt", &old) == 0 && old.detached);
    absent("right/caf\xc3\xa9.txt"); content(old, "source"); assert(fat32_dispose_file(&old) == 0);
    puts("PASS FAT rename: replacement chains stay independent, detached growth/free, UTF-8/255-byte names and remount");

    create("left/tree", NULL); create("left/tree/child", NULL); create("left/tree/child/data", "nested");
    assert(fat32_rename_path("left/tree", "left/tree/child/cycle", false, &moved, &replaced) == -22);
    assert(fat32_rename_path("left/tree", "right/tree", false, &moved, &replaced) == 0);
    at("right/tree/child/../../tree/child/data", "nested");
    assert(fat32_rename_path("left", "right/tree", false, &moved, &replaced) == -39);
    create("left/empty", NULL);
    assert(fat32_rename_path("right/tree", "left/empty", false, &moved, &replaced) == 0 && replaced.detached && replaced.is_dir);
    assert(fat32_dispose_file(&replaced) == 0); at("left/empty/child/data", "nested");
    create("plain", "plain");
    assert(fat32_rename_path("plain", "left/empty", false, &moved, &replaced) == -21);
    assert(fat32_rename_path("left/empty", "plain", false, &moved, &replaced) == -20);
    assert(fat32_rename_path("left/empty", "left/empty/.", false, &moved, &replaced) == -16);
    assert(fat32_rename_path("/", "root", false, &moved, &replaced) == -16);
    assert(fat32_detach_path("left/empty", &old) == -39);
    assert(fat32_rename_path("plain", "PLAIN", false, &moved, &replaced) == 0 && !replaced.detached);
    fat32_file_t special;
    assert(fat32_create_socket_path("left/socket", &special) == 0);
    assert(fat32_rename_path("left/socket", "right/socket", false, &moved, &replaced) == 0 && moved.native_kind == FAT32_NATIVE_SOCKET);
    assert(fat32_begin_download("left/link", &special) == 0 && fat32_write_file(&special, 0, 5, "plain") == 5 && fat32_finish_symlink(&special) == 0);
    assert(fat32_rename_path("left/link", "right/link", false, &moved, &replaced) == 0 && moved.native_kind == FAT32_NATIVE_SYMLINK);
    content(moved, "plain");
    puts("PASS FAT rename: directory parent updates, cycle/type/nonempty refusal, socket/symlink metadata");

    format(); create("left", NULL); create("right", NULL);
    create("left/source document.txt", "source"); create("right/destination document.txt", "target");
    volume_cache_reset(); fail_read = reads + 1;
    assert(fat32_lookup_status("right/destination document.txt", &moved) == -5);
    fail_read = 0;
    assert(fat32_lookup_status("right/missing", &moved) == -2);
    assert(fat32_lookup_status("right/destination document.txt", &moved) == 0); content(moved, "target");
    memcpy(baseline, disk, sizeof(disk)); reads = writes = 0;
    volume_cache_reset(); fat_cache_start = 0xFFFFFFFF;
    assert(fat32_rename_path("left/source document.txt", "right/destination document.txt", false, &moved, &replaced) == 0);
    unsigned write_total = writes, read_total = reads;
    for (unsigned after_write = 0; after_write < 2; after_write++) for (unsigned fail = 1; fail <= write_total; fail++) {
        restore(); fail_write = fail; fail_after_write = after_write;
        memset(&moved, 0x5A, sizeof(moved)); replaced = moved;
        int result = fat32_rename_path("left/source document.txt", "right/destination document.txt", false, &moved, &replaced);
        if (fail > write_total - 2) {
            assert(result == 0 && writes_blocked && replaced.detached);
            absent("left/source document.txt"); at("right/destination document.txt", "source"); content(replaced, "target");
        } else {
            assert(result == -5 && !writes_blocked && !memcmp(disk, baseline, sizeof(disk)));
            assert(moved.size == 0x5A5A5A5Au && replaced.size == 0x5A5A5A5Au);
            at("left/source document.txt", "source"); at("right/destination document.txt", "target");
        }
    }
    for (unsigned fail = 1; fail <= read_total; fail++) {
        restore(); fail_read = fail;
        assert(fat32_rename_path("left/source document.txt", "right/destination document.txt", false, &moved, &replaced) == -5);
        assert(writes == 0 && !memcmp(disk, baseline, sizeof(disk)));
    }
    restore(); fail_write = 4; fail_persistent = true;
    assert(fat32_rename_path("left/source document.txt", "right/destination document.txt", false, &moved, &replaced) == -5 && writes_blocked);
    fail_write = 0; fail_persistent = false; unsigned written = writes;
    assert(fat32_create_path("unsafe", false, &old) != 0 && writes == written);
    assert(fat32_init() && fat32_is_readonly());
    for (unsigned cut = 1; cut <= write_total; cut++) {
        restore(); interrupt_after = cut;
        assert(fat32_rename_path("left/source document.txt", "right/destination document.txt", false, &moved, &replaced) == 0);
        memcpy(disk, interrupted, sizeof(disk)); interrupt_after = 0;
        assert(fat32_init() && fat32_is_readonly() == (cut < write_total));
        if (cut == write_total) { absent("left/source document.txt"); at("right/destination document.txt", "source"); }
        else { written = writes; assert(fat32_create_path("unsafe", false, &old) != 0 && writes == written); }
    }
    puts("PASS FAT rename: read/write and after-write faults, rollback, persistent quarantine and every power-cut boundary");
    restore(); assert(fat32_remove_path("right/destination document.txt") == 0);
    memcpy(baseline, disk, sizeof(disk)); writes = 0;
    assert(fat32_rename_path("left/source document.txt", "right/new document.txt", false, &moved, &replaced) == 0);
    write_total = writes;
    for (unsigned fail = 1; fail <= write_total; fail++) {
        restore(); fail_write = fail;
        int result = fat32_rename_path("left/source document.txt", "right/new document.txt", false, &moved, &replaced);
        if (result == 0) { absent("left/source document.txt"); at("right/new document.txt", "source"); }
        else { at("left/source document.txt", "source"); absent("right/new document.txt"); }
    }
    puts("PASS FAT rename: staged absent-destination publication never exposes an empty/partial replacement on write failure");
}
