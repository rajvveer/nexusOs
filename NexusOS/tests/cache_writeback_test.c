/* Production cache with deterministic partial-write and metadata failures. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#undef NDEBUG
#include <assert.h>
#include "../arch/x86_64/shmem.c"
#include "../arch/x86_64/inode.c"

struct allocation { uint64_t address; unsigned refs; };
static struct allocation allocations[256];
static unsigned allocated, protects, writes, fail_write, grows;
static bool fail_touch;
static uint8_t bytes[16384];
static fat32_file_t entry;
static unsigned disposed;
bool disk_inode_live(const fat32_file_t* file) { return memory_file_disk_find(file) != NULL; }
int fat32_dispose_file(fat32_file_t* file) { assert(file->detached); disposed++; file->first = file->size = 0; return 0; }
uint64_t page_alloc(void) {
    for (unsigned i = 0; i < 256; i++) if (!allocations[i].refs) {
        void* p = _aligned_malloc(4096, 4096); assert(p); memset(p, 0, 4096);
        allocations[i] = (struct allocation){(uint64_t)p, 1}; allocated++; return (uint64_t)p;
    }
    abort();
}
static struct allocation* allocation(uint64_t address) {
    for (unsigned i = 0; i < 256; i++) if (allocations[i].refs && allocations[i].address == address) return &allocations[i];
    abort();
}
void page_free(uint64_t address) { struct allocation* a = allocation(address); if (!--a->refs) { _aligned_free((void*)address); allocated--; } }
void page_retain(uint64_t address) { allocation(address)->refs++; }
uint32_t page_references(uint64_t address) { return allocation(address)->refs; }
bool accessible(uint64_t address, uint64_t size, bool write) { (void)size; (void)write; return address != 0; }
void panic(const char* message) { fprintf(stderr, "%s\n", message); abort(); }
uint64_t native_realtime_ns(void) { static uint64_t time; return time += 2000000000ULL; }
struct credentials* credentials_current(void) { return (void*)1; }
uint32_t credentials_kernel_id(struct credentials* c, bool group, unsigned which) { (void)c; (void)group; (void)which; return 1000; }
uint32_t credentials_visible_id(uint32_t id, bool group) { (void)group; return id; }
bool credentials_capable(struct credentials* c, struct user_namespace* ns, unsigned cap) { (void)c; (void)ns; (void)cap; return false; }
bool memory_file_writable_maps(struct memory_file* file) { (void)file; return false; }
void memory_file_invalidate(struct memory_file* file, uint64_t size) { (void)file; (void)size; }
void memory_file_writeprotect(struct memory_file* file, uint64_t index) { (void)file; (void)index; protects++; }
int fat32_refresh_file(fat32_file_t* file) { *file = entry; return 0; }
int32_t fat32_read_file(fat32_file_t* file, uint32_t offset, uint32_t count, void* output) {
    *file = entry; if (offset >= entry.size) return 0;
    if (count > entry.size - offset) count = entry.size - offset;
    memcpy(output, bytes + offset, count); return count;
}
int fat32_write_file(fat32_file_t* file, uint32_t offset, uint32_t count, const void* data) {
    writes++; if (fail_write && writes == fail_write) return -1;
    assert(offset <= sizeof(bytes) && count <= sizeof(bytes) - offset);
    if (offset > entry.size) memset(bytes + entry.size, 0, offset - entry.size);
    memcpy(bytes + offset, data, count); if (offset + count > entry.size) entry.size = offset + count;
    *file = entry; return count;
}
int fat32_truncate_file(fat32_file_t* file, uint32_t size) {
    if (size > sizeof(bytes)) return -2;
    if (size > entry.size) { grows++; memset(bytes + entry.size, 0, size - entry.size); }
    entry.size = size; *file = entry; return 0;
}
int disk_file_touch(fat32_file_t* file) { if (fail_touch) return -1; entry.wtime++; *file = entry; return 0; }
void file_refresh_views(const fat32_file_t* file) { disk_orphan_refresh(file); assert(file->size == entry.size); }
uint32_t fat32_time_to_unix(uint16_t date, uint16_t time) { (void)date; return time; }
void vga_print(const char* text) { (void)text; }
int main(void) {
    entry.dir_lba = 100; strcpy(entry.name, "cache.dat");
    struct memory_file* file = memory_file_disk_get(&entry); assert(file);
    memset(bytes, 0xC1, sizeof(bytes));
    uint8_t input[8192]; memset(input, 0x42, 4096); memset(input + 4096, 0x73, 4096);
    assert(memory_file_io(file, (uint64_t)input, sizeof(input), 0, true) == sizeof(input));
    fail_write = 2;
    assert(memory_file_sync(file, 0, ~0ULL) == -5 && bytes[0] == 0x42 && entry.size == 4096 && !grows);
    memory_file_release(file); assert(memory_file_disk_find(&entry) && memory_file_disk_pages() == allocated);
    file = memory_file_disk_get(&entry); uint8_t result[8192];
    assert(memory_file_io(file, (uint64_t)result, sizeof(result), 0, false) == sizeof(result) && !memcmp(input, result, sizeof(result)));
    fail_write = 0; writes = 0; fail_touch = true;
    assert(memory_file_sync(file, 0, ~0ULL) == -5 && !memcmp(bytes, input, sizeof(input)));
    fail_touch = false;
    assert(memory_file_sync(file, 0, ~0ULL) == 0 && !memcmp(bytes, input, sizeof(input)) && protects >= 2);
    assert(!grows); /* Appends write data directly, without a duplicate zero-extension pass. */
    assert(memory_file_truncate(file, 17) == 0 && memory_file_truncate(file, 8192) == 0);
    assert(memory_file_sync(file, 0, ~0ULL) == 0);
    for (unsigned i = 0; i < 8192; i++) assert(bytes[i] == (i < 17 ? 0x42 : 0));
    memory_file_release(file); assert(allocated == 0 && memory_file_disk_pages() == 0);
    entry.size = 0; memset(bytes, 0xC1, sizeof(bytes)); file = memory_file_disk_get(&entry);
    assert(memory_file_io(file, (uint64_t)"A", 1, 0, true) == 1);
    assert(memory_file_io(file, (uint64_t)"Z", 1, 12288, true) == 1);
    assert(memory_file_sync(file, 0, 4096) == 0 && entry.size == 12289 && bytes[0] == 'A');
    for (unsigned i = 1; i < 12289; i++) assert(bytes[i] == 0);
    uint8_t deferred; assert(memory_file_io(file, (uint64_t)&deferred, 1, 12288, false) == 1 && deferred == 'Z');
    assert(memory_file_sync(file, 0, ~0ULL) == 0 && bytes[12288] == 'Z');
    memory_file_release(file); assert(!allocated && !memory_file_disk_pages());
    puts("PASS cache retries partial data/metadata failures without losing dirty bytes; shrink/regrow and ownership cleanup");
    for (unsigned i = 1; i <= 700; i++) {
        fat32_file_t source = {.dir_lba = 100 + i}, moved = {.dir_lba = 2000 + i};
        source.native_ino = disk_inode_id(&source);
        assert(disk_inode_reserve(2) == 0); disk_inode_rekey(&source, &moved);
        fat32_file_t reused = {.dir_lba = source.dir_lba};
        assert(disk_same_inode(&source, &moved) && !disk_same_inode(&source, &reused));
        assert(disk_inode_reserve(1) == 0); disk_inode_rekey(&moved, NULL);
        reused.dir_lba = moved.dir_lba; assert(!disk_same_inode(&moved, &reused));
    }
    memset(&entry, 0, sizeof(entry)); entry.dir_lba = 1000000;
    fat32_file_t old = entry; old.native_ino = disk_inode_id(&old);
    struct memory_file* original = memory_file_disk_get(&old); assert(original);
    struct disk_orphan* orphan = disk_orphan_prepare(); assert(orphan);
    fat32_file_t removed = old; removed.detached = true;
    disk_orphan_adopt(orphan, &removed); memory_file_disk_moved(&old, &removed);
    assert(disk_inode_reserve(1) == 0); disk_inode_rekey(&old, NULL);
    struct memory_file* replacement = memory_file_disk_get(&entry); assert(replacement && replacement != original);
    disk_orphan_collect(); assert(disposed == 0);
    memory_file_release(original); disk_orphan_collect(); assert(disposed == 1 && memory_file_disk_find(&entry) == replacement);
    memory_file_release(replacement); assert(allocated == disk_inode_pages() && memory_file_disk_pages() == 0);
    puts("PASS stable inode identities across 700 moves/unlinks, slot reuse, orphan/cache separation and last-reference reclamation");
}
