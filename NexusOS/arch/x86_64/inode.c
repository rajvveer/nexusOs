/* FAT slots can move or be reused; live native inode identities must not. */
#include "native.h"
#include "../../kernel/string.h"

struct slot_identity { uint64_t slot, inode; struct slot_identity* chain; };
#define IDENTITIES_PER_PAGE 170
#define IDENTITY_BUCKETS 4096
struct identity_page { struct identity_page* next; unsigned used; struct slot_identity entries[IDENTITIES_PER_PAGE]; };
struct disk_orphan { struct disk_orphan* next; fat32_file_t file; };
_Static_assert(sizeof(struct identity_page) <= 4096 && sizeof(struct disk_orphan) <= 4096, "inode metadata allocation");
static struct identity_page* identities;
static struct slot_identity* identity_buckets[IDENTITY_BUCKETS];
static unsigned identity_bucket(uint64_t key) { return (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 52); }
static struct disk_orphan* orphans;
static uint64_t generation = 1ULL << 63;
static uint64_t slot(const fat32_file_t* file) { return (uint64_t)file->dir_lba << 32 | file->dir_off; }
static struct slot_identity* identity(uint64_t key, bool create) {
    unsigned bucket = identity_bucket(key);
    for (struct slot_identity* entry = identity_buckets[bucket]; entry; entry = entry->chain) if (entry->slot == key) return entry;
    if (!create) return NULL;
    if (!key || !identities || identities->used == IDENTITIES_PER_PAGE) panic("inode identity was not reserved");
    struct slot_identity* entry = &identities->entries[identities->used++]; entry->slot = key;
    entry->chain = identity_buckets[bucket]; identity_buckets[bucket] = entry; return entry;
}
uint64_t disk_inode_id(const fat32_file_t* file) {
    if (file->native_ino) return file->native_ino;
    struct slot_identity* entry = identity(slot(file), false);
    return entry ? entry->inode : (uint64_t)file->dir_lba * 16 + file->dir_off / 32 + 1;
}
bool disk_same_inode(const fat32_file_t* a, const fat32_file_t* b) { return disk_inode_id(a) == disk_inode_id(b); }
int disk_inode_reserve(unsigned count) {
    if (!count || count > IDENTITIES_PER_PAGE) return -22;
    if (identities && count <= IDENTITIES_PER_PAGE - identities->used) return 0;
    struct identity_page* page = (void*)page_alloc(); if (!page) return -12;
    page->next = identities; identities = page; return 0;
}
void disk_inode_rekey(const fat32_file_t* old, fat32_file_t* moved) {
    if (old->ino) { if (moved) moved->native_ino = old->native_ino; return; } /* ext4 inode numbers never move */
    uint64_t inode = disk_inode_id(old);
    if (moved && slot(old) == slot(moved)) { moved->native_ino = inode; return; }
    if (generation == ~0ULL) panic("inode generation overflow");
    identity(slot(old), true)->inode = ++generation;
    if (moved) { identity(slot(moved), true)->inode = inode; moved->native_ino = inode; }
}
struct disk_orphan* disk_orphan_prepare(void) { return (void*)page_alloc(); }
void disk_orphan_cancel(struct disk_orphan* orphan) { if (orphan) page_free((uint64_t)orphan); }
void disk_orphan_adopt(struct disk_orphan* orphan, const fat32_file_t* file) {
    if (!orphan || !file->detached || !file->native_ino) panic("invalid detached inode");
    orphan->file = *file; orphan->next = orphans; orphans = orphan;
}
static bool orphans_shut;   /* disk_orphan_shutdown ran: open orphans lost their chain owner on purpose */
void disk_orphan_refresh(const fat32_file_t* file) {
    if (!file->detached) return;
    for (struct disk_orphan* orphan = orphans; orphan; orphan = orphan->next)
        if (disk_same_inode(&orphan->file, file)) { orphan->file = *file; return; }
    if (orphans_shut) return;   /* another CPU/task still using one while the machine stops (the volume is write-blocked) */
    panic("detached inode has no chain owner");
}
void disk_orphan_collect(void) {
    struct disk_orphan** link = &orphans;
    while (*link) {
        struct disk_orphan* orphan = *link;
        if (disk_inode_live(&orphan->file) || fat32_dispose_file(&orphan->file)) { link = &orphan->next; continue; }
        *link = orphan->next; page_free((uint64_t)orphan);
    }
}
/* Orderly power-off/reboot (vfs.c disk_shutdown, FAT32 root): nothing uses an orphan any more, so every unlinked file
 * that was still open or mapped gives its clusters back now. Without this they stayed allocated with no directory
 * entry (lost clusters: Chrome's unlinked /tmp shared-memory files and BrowserMetrics .pma, Wine's anonymous
 * mappings). Orphans on other mounted volumes belong to their drivers. */
void disk_orphan_shutdown(void) {
    struct disk_orphan** link = &orphans; orphans_shut = true;
    while (*link) {
        struct disk_orphan* orphan = *link;
        if (orphan->file.mount || fat32_dispose_file(&orphan->file)) { link = &orphan->next; continue; }
        *link = orphan->next; page_free((uint64_t)orphan);
    }
}
uint64_t disk_inode_pages(void) {
    uint64_t count = 0;
    for (struct identity_page* page = identities; page; page = page->next) count++;
    for (struct disk_orphan* orphan = orphans; orphan; orphan = orphan->next) count++;
    return count;
}
