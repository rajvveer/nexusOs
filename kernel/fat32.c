/* ============================================================================
 * NexusOS — FAT32 Filesystem Driver (Implementation) — Phase 19
 * ============================================================================
 * Reads/writes FAT32 on ATA disk. Parses BPB, follows cluster chains,
 * handles 8.3 short filenames and VFAT long filenames.
 * ============================================================================ */

#include "fat32.h"
#include "ata.h"
#include "vga.h"
#include "string.h"
#include "memory.h"
#ifdef NEXUS_NATIVE64
#include "../arch/x86_64/blockdev.h"
/* Native: the volume is a block-layer partition (IDE, AHCI or NVMe), addressed in 512-byte sectors. */
static const struct block_partition* volume_part;
#define VOLUME_DISK_READ(lba, count, buf) block_part_read(volume_part, lba, count, buf)
#define VOLUME_DISK_WRITE(lba, count, buf) block_part_write(volume_part, lba, count, buf)
#else
#define VOLUME_DISK_READ(lba, count, buf) ata_read_sectors(volume_lba + (lba), count, buf)
#define VOLUME_DISK_WRITE(lba, count, buf) ata_write_sectors(volume_lba + (lba), count, buf)
#endif

/* --------------------------------------------------------------------------
 * FAT32 On-Disk Structures
 * -------------------------------------------------------------------------- */

/* BIOS Parameter Block (first sector) */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} fat32_bpb_t;

/* Directory entry (32 bytes) */
typedef struct __attribute__((packed)) {
    char     name[11];          /* 8.3 format */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat32_dirent_t;

/* LFN entry (32 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;             /* Always 0x0F */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} fat32_lfn_t;

/* Directory entry attributes */
#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F

#define FAT32_EOC       0x0FFFFFF8  /* End of cluster chain */
#define FAT32_FREE      0x00000000
#define FAT32_BAD       0x0FFFFFF7
#define FAT32_CLEAN     0x08000000

/* --------------------------------------------------------------------------
 * Driver State
 * -------------------------------------------------------------------------- */

static bool mounted = false;
static fat32_bpb_t bpb;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;
static uint32_t root_cluster;
static uint32_t volume_lba, volume_sectors, cluster_limit, next_free;
static bool io_error;
static bool writes_blocked;
#ifdef NEXUS_NATIVE64
/* Session dirty flag and boot check (arch/x86_64/fatcheck.inc, included at the end of this file). */
static bool volume_dirty_tracking, volume_marked_dirty;
static int volume_mark_dirty(void);
static int fat32_boot_check(void);
/* Public entry points first wait while another task sleeps inside a disk wait (blockio.c owner): the driver's state
 * (sector buffers, FAT window, lookup filters) belongs to one operation at a time even where a caller missed the
 * system-call barrier. */
static void fat32_entry_barrier(const char* where);
#define FAT_ENTRY() fat32_entry_barrier(__func__)
#else
#define FAT_ENTRY() do { } while (0)
#endif
static bool free_known;
static uint32_t free_clusters;
static uint32_t chain_epoch;
#define SEEK_FILES 16
#define SEEK_MARKS 32
static struct seek_index { uint32_t first, size, epoch, shift, cluster[SEEK_MARKS]; } seek_indices[SEEK_FILES];
static unsigned seek_next;

/* Keep hot directory/FAT sectors without rereading them on every page fault.
 * Valid bits avoid speculative I/O outside the caller's requested sectors. */
#define VOLUME_CACHE_SLOTS 32
#define VOLUME_CACHE_SECTORS 8
#ifdef NEXUS_NATIVE64
struct volume_cache_entry {
    uint64_t used;
    uint32_t lba;
    uint8_t valid;
    uint8_t data[VOLUME_CACHE_SECTORS * 512];
};
/* Native: the kernel hands over a large set-associative cache at boot (fallback: one static set of VOLUME_CACHE_WAYS). */
#define VOLUME_CACHE_WAYS 4
static struct volume_cache_entry volume_cache_static[VOLUME_CACHE_WAYS]; /* one-set fallback: boot installs the arena cache */
static struct volume_cache_entry* volume_cache = volume_cache_static;
static uint32_t volume_cache_slots = VOLUME_CACHE_WAYS;
static uint64_t volume_cache_clock;
static void volume_cache_reset(void);
void fat32_set_volume_cache(void* memory, uint64_t bytes) {
    uint64_t slots = bytes / sizeof(struct volume_cache_entry);
    if (!memory || slots < VOLUME_CACHE_WAYS || slots > 0x100000) return;
    volume_cache = memory; volume_cache_slots = (uint32_t)(slots - slots % VOLUME_CACHE_WAYS); volume_cache_reset();
}
static struct volume_cache_entry* volume_cache_set(uint32_t base) {
    uint32_t sets = volume_cache_slots / VOLUME_CACHE_WAYS;
    return &volume_cache[((base / VOLUME_CACHE_SECTORS) % sets) * VOLUME_CACHE_WAYS];
}
static void volume_cache_reset(void) {
    for (uint32_t i = 0; i < volume_cache_slots; i++) volume_cache[i].valid = 0;
    volume_cache_clock = 0;
}
static void volume_cache_invalidate(uint32_t lba, uint32_t count) {
    if (!count) return;
    for (uint32_t base = lba - lba % VOLUME_CACHE_SECTORS; base < lba + count; base += VOLUME_CACHE_SECTORS) {
        struct volume_cache_entry* set = volume_cache_set(base);
        for (unsigned i = 0; i < VOLUME_CACHE_WAYS; i++) if (set[i].valid && set[i].lba == base) set[i].valid = 0;
        if (base > 0xFFFFFFFFu - VOLUME_CACHE_SECTORS) break;
    }
}
static struct volume_cache_entry* volume_cache_find(uint32_t lba) {
    struct volume_cache_entry* set = volume_cache_set(lba);
    for (unsigned i = 0; i < VOLUME_CACHE_WAYS; i++) if (set[i].valid && set[i].lba == lba) return &set[i];
    return NULL;
}
static struct volume_cache_entry* volume_cache_get(uint32_t lba) {
    volume_cache_clock++;
    struct volume_cache_entry* found = volume_cache_find(lba);
    if (found) { found->used = volume_cache_clock; return found; }
    struct volume_cache_entry *set = volume_cache_set(lba), *oldest = &set[0];
    for (unsigned i = 0; i < VOLUME_CACHE_WAYS; i++) {
        struct volume_cache_entry* entry = &set[i];
        if (!entry->valid || (oldest->valid && entry->used < oldest->used)) oldest = entry;
    }
    oldest->lba = lba; oldest->valid = 0; oldest->used = volume_cache_clock;
    return oldest;
}
/* Successful writes refresh any cached copy, so metadata read-modify-write stays in memory. */
static void volume_cache_update(uint32_t lba, uint32_t count, const uint8_t* buf) {
    while (count) {
        uint32_t offset = lba % VOLUME_CACHE_SECTORS, n = VOLUME_CACHE_SECTORS - offset;
        if (n > count) n = count;
        struct volume_cache_entry* entry = volume_cache_find(lba - offset);
        if (entry) { memcpy(entry->data + offset * 512, buf, n * 512); entry->valid |= (uint8_t)(((1u << n) - 1) << offset); }
        lba += n; count -= n; buf += n * 512;
    }
}
#else
static struct volume_cache_entry {
    uint64_t used;
    uint32_t lba;
    uint8_t valid;
    uint8_t data[VOLUME_CACHE_SECTORS * 512];
} volume_cache[VOLUME_CACHE_SLOTS];
static uint64_t volume_cache_clock;

static void volume_cache_reset(void) {
    for (unsigned i = 0; i < VOLUME_CACHE_SLOTS; i++) volume_cache[i].valid = 0;
    volume_cache_clock = 0;
}
static void volume_cache_invalidate(uint32_t lba, uint32_t count) {
    if (!count) return;
    for (unsigned i = 0; i < VOLUME_CACHE_SLOTS; i++) {
        struct volume_cache_entry* entry = &volume_cache[i];
        if (entry->valid && entry->lba < (uint64_t)lba + count &&
            (uint64_t)entry->lba + VOLUME_CACHE_SECTORS > lba) entry->valid = 0;
    }
}
static struct volume_cache_entry* volume_cache_find(uint32_t lba) {
    for (unsigned i = 0; i < VOLUME_CACHE_SLOTS; i++)
        if (volume_cache[i].valid && volume_cache[i].lba == lba) return &volume_cache[i];
    return NULL;
}
static struct volume_cache_entry* volume_cache_get(uint32_t lba) {
    volume_cache_clock++;
    struct volume_cache_entry* found = volume_cache_find(lba);
    if (found) { found->used = volume_cache_clock; return found; }
    struct volume_cache_entry* oldest = &volume_cache[0];
    for (unsigned i = 0; i < VOLUME_CACHE_SLOTS; i++) {
        struct volume_cache_entry* entry = &volume_cache[i];
        if (!entry->valid || (oldest->valid && entry->used < oldest->used)) oldest = entry;
    }
    oldest->lba = lba; oldest->valid = 0; oldest->used = volume_cache_clock;
    return oldest;
}
#endif
static int volume_read(uint32_t lba, uint32_t count, void* buf) {
    if ((count && !buf) || lba > volume_sectors || count > volume_sectors - lba) { io_error = true; return -1; }
    if (volume_cache_clock > ~(uint64_t)0 - count) volume_cache_reset();
    bool cached = true;
    for (uint32_t at = lba, left = count; left; ) {
        uint32_t offset = at % VOLUME_CACHE_SECTORS;
        uint32_t n = VOLUME_CACHE_SECTORS - offset;
        if (n > left) n = left;
        struct volume_cache_entry* entry = volume_cache_find(at - offset);
        uint8_t mask = (uint8_t)(((1u << n) - 1) << offset);
        if (!entry || (entry->valid & mask) != mask) { cached = false; break; }
        at += n; left -= n;
    }
    /* A cold unaligned cluster must still use one ATA command, not one per slot. */
    if (!cached && VOLUME_DISK_READ(lba, count, buf) < 0) { io_error = true; return -1; }
    uint8_t* dst = buf;
    while (count) {
        uint32_t offset = lba % VOLUME_CACHE_SECTORS;
        uint32_t n = VOLUME_CACHE_SECTORS - offset;
        if (n > count) n = count;
        struct volume_cache_entry* entry = volume_cache_get(lba - offset);
        uint8_t mask = (uint8_t)(((1u << n) - 1) << offset);
        if (cached) memcpy(dst, entry->data + offset * 512, n * 512);
        else { memcpy(entry->data + offset * 512, dst, n * 512); entry->valid |= mask; }
        lba += n; count -= n; dst += n * 512;
    }
    return 0;
}
/* Bulk file data: read straight into the caller's buffer without evicting cached metadata. */
static int volume_read_direct(uint32_t lba, uint32_t count, void* buf) {
    if ((count && !buf) || lba > volume_sectors || count > volume_sectors - lba) { io_error = true; return -1; }
    if (VOLUME_DISK_READ(lba, count, buf) < 0) { io_error = true; return -1; }
    return 0;
}
static int volume_write(uint32_t lba, uint32_t count, const void* buf) {
    if ((count && !buf) || lba > volume_sectors || count > volume_sectors - lba) { io_error = true; return -1; }
#ifdef NEXUS_NATIVE64
    /* The first write of a mounted session marks the volume dirty (FAT[1]) before it reaches the disk. */
    if (mounted && volume_dirty_tracking && !volume_marked_dirty && !writes_blocked && volume_mark_dirty() < 0) { io_error = true; return -1; }
    if (writes_blocked || VOLUME_DISK_WRITE(lba, count, buf) < 0) {
        /* Failed writes may already have changed part of the disk. */
        volume_cache_invalidate(lba, count); io_error = true; return -1;
    }
    volume_cache_update(lba, count, buf);
    return 0;
#else
    /* Failed writes may already have changed part of the disk. */
    volume_cache_invalidate(lba, count);
    if (writes_blocked || VOLUME_DISK_WRITE(lba, count, buf) < 0) { io_error = true; return -1; }
    return 0;
#endif
}

/* Sector buffer */
static uint8_t sector_buf[512];

/* Cached FAT sectors */
#define FAT_CACHE_SECTORS 4
static uint8_t fat_cache[FAT_CACHE_SECTORS * 512];
static uint32_t fat_cache_start = 0xFFFFFFFF;
/* Batched FAT updates: inside a deferred operation, modified FAT sectors stay in the
 * cache window and are written once (all FAT copies) before the window moves, before
 * any directory entry is written, and when the outermost operation ends. */
static uint8_t fat_dirty_mask;
static unsigned fat_defer_depth;

/* Root directory cache */
#define MAX_DIR_ENTRIES 64
static fs_node_t dir_nodes[MAX_DIR_ENTRIES];
static int dir_node_count = 0;

/* VFS root node for FAT32 */
static fs_node_t fat32_root_node;

/* --------------------------------------------------------------------------
 * Helper: Convert cluster number to LBA
 * -------------------------------------------------------------------------- */
static uint32_t cluster_to_lba(uint32_t cluster) {
    if (cluster < 2 || cluster >= cluster_limit) { io_error = true; return volume_sectors; }
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

/* --------------------------------------------------------------------------
 * Helper: point at a cluster's FAT entry inside the FAT cache (loading the
 * sector run if needed). NULL on a disk error.
 * -------------------------------------------------------------------------- */
static int fat_flush_dirty(void);
static uint32_t* fat_entry_ptr(uint32_t cluster, uint32_t* sector_out) {
    if (cluster >= cluster_limit) { io_error = true; return NULL; }
    uint32_t fat_sector = fat_start_lba + (cluster * 4) / 512;
    if (fat_sector < fat_cache_start ||
        fat_sector >= fat_cache_start + FAT_CACHE_SECTORS) {
        if (fat_dirty_mask && fat_flush_dirty() < 0) return NULL;
        fat_cache_start = 0xFFFFFFFF;
        uint32_t count = fat_start_lba + bpb.fat_size_32 - fat_sector;
        if (count > FAT_CACHE_SECTORS) count = FAT_CACHE_SECTORS;
        if (volume_read(fat_sector, count, fat_cache) < 0) return NULL;
        fat_cache_start = fat_sector;
    }
    if (sector_out) *sector_out = fat_sector;
    return (uint32_t*)&fat_cache[(fat_sector - fat_cache_start) * 512 + (cluster * 4) % 512];
}

/* Read a FAT entry. A disk error reads as end-of-chain so walks terminate. */
static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t* e = fat_entry_ptr(cluster, NULL);
    return e ? (*e & 0x0FFFFFFF) : 0x0FFFFFFF;
}

/* Write a FAT entry through the FAT cache. Uses its own buffer (the cache),
 * NOT sector_buf, which callers such as fat32_delete_file are still using for
 * the directory sector they are about to write back. */
static void fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_sector;
    uint32_t* e = fat_entry_ptr(cluster, &fat_sector);
    if (!e) return;
    uint32_t old = *e & 0x0FFFFFFF, next = value & 0x0FFFFFFF;
    /* Appending to a chain leaves every existing prefix valid; frees and relinks do not. */
    bool append = (old == FAT32_FREE && next >= 0x0FFFFFF8) || (old >= 0x0FFFFFF8 && next >= 2 && next < 0x0FFFFFF0);
    if (cluster >= 2 && old != next && !append && chain_epoch != 0xFFFFFFFFu) chain_epoch++;
    *e = (*e & 0xF0000000) | (value & 0x0FFFFFFF);   /* keep top 4 bits */
    uint32_t cache_off = (fat_sector - fat_cache_start) * 512;
    if (fat_defer_depth) fat_dirty_mask |= (uint8_t)(1u << (fat_sector - fat_cache_start));
    else for (uint32_t copy = 0; copy < bpb.num_fats; copy++)
        if (volume_write(fat_sector + copy * bpb.fat_size_32, 1, fat_cache + cache_off) < 0) {
            fat_cache_start = 0xFFFFFFFF; free_known = false; return;
        }
    if (free_known && cluster >= 2 && ((old == FAT32_FREE) != (next == FAT32_FREE))) {
        if (next == FAT32_FREE) free_clusters++;
        else if (free_clusters) free_clusters--;
        else free_known = false;
    }
    if (value == FAT32_FREE && cluster < next_free) next_free = cluster;
}

static int fat_flush_dirty(void) {
    if (!fat_dirty_mask) return 0;
    if (fat_cache_start == 0xFFFFFFFF) { fat_dirty_mask = 0; io_error = true; free_known = false; return -1; }
    for (uint32_t i = 0; i < FAT_CACHE_SECTORS; i++) {
        if (!(fat_dirty_mask & (1u << i))) continue;
        for (uint32_t copy = 0; copy < bpb.num_fats; copy++)
            if (volume_write(fat_cache_start + i + copy * bpb.fat_size_32, 1, fat_cache + i * 512) < 0) {
                fat_dirty_mask = 0; fat_cache_start = 0xFFFFFFFF; free_known = false; io_error = true; return -1;
            }
    }
    fat_dirty_mask = 0; return 0;
}
static void fat_defer_begin(void) { fat_defer_depth++; }
static int fat_defer_end(void) {
    if (fat_defer_depth && --fat_defer_depth) return 0;
    return fat_flush_dirty();
}

/* Max clusters a directory chain may span: FAT caps a directory at 65536
 * 32-byte entries (2 MiB). Bounds chain walks so a corrupt/cyclic FAT can't
 * hang the kernel. */
static uint32_t dir_max_clusters(void) { return 0x200000u / bytes_per_cluster; }

/* --------------------------------------------------------------------------
 * Helper: Find a free cluster in the FAT
 * -------------------------------------------------------------------------- */
/* Free-slot hints: for directory `dir`, no run of >= w free slots starts before first[w-1].
 * Only ever a lower bound: deleting slot x lowers it to x+1-w, allocating a cluster forgets it. */
#define F57_HINT_DIRS 16
#define F57_HINT_RUNS 21
static struct { uint32_t dir; uint32_t first[F57_HINT_RUNS]; } f57_hints[F57_HINT_DIRS];
static unsigned f57_hint_victim;
static int f57_hint_find(uint32_t dir) {
    for (int i = 0; i < F57_HINT_DIRS; i++) if (f57_hints[i].dir == dir) return i;
    return -1;
}
static void f57_hint_drop(uint32_t dir) {
    int i = f57_hint_find(dir); if (i >= 0) f57_hints[i].dir = 0;
}
static void f57_hint_freed(uint32_t dir, uint32_t index) {
    int i = f57_hint_find(dir); if (i < 0) return;
    for (uint32_t w = 1; w <= F57_HINT_RUNS; w++) {
        uint32_t bound = index + 1 >= w ? index + 1 - w : 0;
        if (f57_hints[i].first[w - 1] > bound) f57_hints[i].first[w - 1] = bound;
    }
}
static uint32_t f57_hint_get(uint32_t dir, uint32_t wanted) {
    int i = f57_hint_find(dir);
    return i < 0 || !wanted ? 0 : f57_hints[i].first[(wanted > F57_HINT_RUNS ? F57_HINT_RUNS : wanted) - 1];
}
static void f57_hint_found(uint32_t dir, uint32_t wanted, uint32_t start) {
    /* The earliest run of `wanted` starts at `start`, so no longer run starts earlier either. */
    if (!wanted || wanted > F57_HINT_RUNS) return;
    int i = f57_hint_find(dir);
    if (i < 0) {
        i = (int)(f57_hint_victim++ % F57_HINT_DIRS);
        f57_hints[i].dir = dir; memset(f57_hints[i].first, 0, sizeof(f57_hints[i].first));
    }
    for (uint32_t w = wanted; w <= F57_HINT_RUNS; w++)
        if (f57_hints[i].first[w - 1] < start) f57_hints[i].first[w - 1] = start;
}

#ifdef NEXUS_NATIVE64
static void f57_lc_drop_bloom(uint32_t dir);
static void f57_lc_drop_dir(uint32_t dir);
static void f57_lc_added(uint32_t dir, const char* name);
static void f57_lc_reset(void);
#else
static inline void f57_lc_drop_bloom(uint32_t dir) { (void)dir; }
static inline void f57_lc_drop_dir(uint32_t dir) { (void)dir; }
static inline void f57_lc_added(uint32_t dir, const char* name) { (void)dir; (void)name; }
static inline void f57_lc_reset(void) {}
#endif

static uint32_t fat_alloc_cluster(void) {
    /* Highest valid cluster + 1: data clusters only (the BPB total includes
     * the reserved + FAT sectors), capped by the FAT's own entry count. The
     * old total_sectors/spc bound let allocation run past the volume end. */
    if (next_free < 2 || next_free >= cluster_limit) next_free = 2;
    uint32_t start = next_free;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t end = pass ? start : cluster_limit;
        for (uint32_t c = pass ? 2 : start; c < end; c++) {
            uint32_t entry = fat_read_entry(c);
            if (io_error) return 0;
            if (entry == FAT32_FREE) {
                f57_hint_drop(c); f57_lc_drop_bloom(c);
                fat_write_entry(c, 0x0FFFFFFF);
                if (io_error) return 0;
                next_free = c + 1;
                return c;
            }
        }
    }
    return 0; /* No free clusters */
}

/* --------------------------------------------------------------------------
 * Helper: Convert 8.3 name to readable string
 * -------------------------------------------------------------------------- */
static void fat_name_to_string(const char* fat_name, char* out) {
    int i;
    /* Copy filename part (first 8 chars, trim spaces) */
    for (i = 7; i >= 0 && fat_name[i] == ' '; i--);
    int len = i + 1;
    for (int j = 0; j < len; j++) {
        out[j] = fat_name[j];
        if (out[j] >= 'A' && out[j] <= 'Z') out[j] += 32; /* lowercase */
    }

    /* Extension */
    if (fat_name[8] != ' ') {
        out[len++] = '.';
        for (int j = 8; j < 11 && fat_name[j] != ' '; j++) {
            out[len] = fat_name[j];
            if (out[len] >= 'A' && out[len] <= 'Z') out[len] += 32;
            len++;
        }
    }
    out[len] = '\0';
}

/* --------------------------------------------------------------------------
 * Helper: Convert string to 8.3 FAT name
 * -------------------------------------------------------------------------- */
static void string_to_fat_name(const char* str, char* fat_name) {
    memset(fat_name, ' ', 11);
    int i = 0, j = 0;
    /* Filename part */
    while (str[i] && str[i] != '.' && j < 8) {
        fat_name[j++] = (str[i] >= 'a' && str[i] <= 'z') ? str[i] - 32 : str[i];
        i++;
    }
    /* Skip to extension */
    if (str[i] == '.') {
        i++;
        j = 8;
        while (str[i] && j < 11) {
            fat_name[j++] = (str[i] >= 'a' && str[i] <= 'z') ? str[i] - 32 : str[i];
            i++;
        }
    }
}

/* --------------------------------------------------------------------------
 * VFS callbacks for FAT32 files
 * -------------------------------------------------------------------------- */

/* Read file data */
static int32_t fat32_vfs_read(fs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !buffer) return -1;
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    uint32_t cluster = node->inode;
    uint32_t bytes_read = 0;
    uint32_t skip = offset;

    /* Skip clusters for offset */
    while (skip >= bytes_per_cluster && cluster < FAT32_EOC) {
        skip -= bytes_per_cluster;
        cluster = fat_read_entry(cluster);
    }

    /* Read data cluster by cluster */
    while (bytes_read < size && cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < sectors_per_cluster && bytes_read < size; s++) {
            volume_read(lba + s, 1, sector_buf);

            uint32_t start = 0;
            if (skip > 0) {
                start = skip > 512 ? 512 : skip;
                skip -= start;
            }

            uint32_t to_copy = 512 - start;
            if (to_copy > size - bytes_read) to_copy = size - bytes_read;

            memcpy(buffer + bytes_read, sector_buf + start, to_copy);
            bytes_read += to_copy;
        }

        cluster = fat_read_entry(cluster);
    }

    return (int32_t)bytes_read;
}

/* Write file data (simple: single cluster files only for now) */
static int32_t fat32_vfs_write(fs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (!node || !buffer) return -1;

    uint32_t cluster = node->inode;
    if (cluster < 2) return -1;

    /* Simple write: just write to first cluster, limited to cluster size */
    uint32_t max_write = bytes_per_cluster;
    if (offset >= max_write) return 0;                  /* no wrap below */
    if (size > max_write - offset) size = max_write - offset;

    uint32_t lba = cluster_to_lba(cluster);
    uint32_t sector_offset = offset / 512;
    uint32_t byte_offset = offset % 512;
    uint32_t written = 0;

    while (written < size && sector_offset < sectors_per_cluster) {
        volume_read(lba + sector_offset, 1, sector_buf);

        uint32_t to_write = 512 - byte_offset;
        if (to_write > size - written) to_write = size - written;

        memcpy(sector_buf + byte_offset, buffer + written, to_write);
        volume_write(lba + sector_offset, 1, sector_buf);

        written += to_write;
        sector_offset++;
        byte_offset = 0;
    }

    /* Update file size if extended */
    if (offset + size > node->size) {
        node->size = offset + size;
    }

    return (int32_t)written;
}

/* Read directory child by index */
static fs_node_t* fat32_vfs_readdir(fs_node_t* dir, uint32_t index) {
    (void)dir;
    if ((int)index >= dir_node_count) return NULL;
    return &dir_nodes[index];
}

/* Find child by name */
static fs_node_t* fat32_vfs_finddir(fs_node_t* dir, const char* name) {
    (void)dir;
    for (int i = 0; i < dir_node_count; i++) {
        if (strcmp(dir_nodes[i].name, name) == 0)
            return &dir_nodes[i];
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Read root directory entries from disk
 * -------------------------------------------------------------------------- */
static void fat32_read_root_dir(void) {
    f57_lc_drop_bloom(root_cluster);
    dir_node_count = 0;
    uint32_t cluster = root_cluster;

    for (uint32_t hops = dir_max_clusters();
         hops-- && cluster >= 2 && cluster < FAT32_EOC && dir_node_count < MAX_DIR_ENTRIES; ) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (volume_read(lba + s, 1, sector_buf) < 0) goto failed;

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            int entries_per_sector = 512 / sizeof(fat32_dirent_t);

            for (int e = 0; e < entries_per_sector && dir_node_count < MAX_DIR_ENTRIES; e++) {
                if (entries[e].name[0] == 0x00) goto done; /* No more entries */
                if ((uint8_t)entries[e].name[0] == 0xE5) continue; /* Deleted */
                if (entries[e].attr == ATTR_LFN) continue; /* Skip LFN for now */
                if (entries[e].attr & ATTR_VOLUME_ID) continue;

                fs_node_t* node = &dir_nodes[dir_node_count];
                memset(node, 0, sizeof(fs_node_t));

                fat_name_to_string(entries[e].name, node->name);

                uint32_t first_cluster = ((uint32_t)entries[e].first_cluster_hi << 16) |
                                          entries[e].first_cluster_lo;
                node->inode = first_cluster;
                node->size = entries[e].file_size;
                node->type = (entries[e].attr & ATTR_DIRECTORY) ? FS_DIRECTORY : FS_FILE;
                node->read = fat32_vfs_read;
                node->write = fat32_vfs_write;

                /* Skip . and .. entries */
                if (node->name[0] == '.') continue;

                dir_node_count++;
            }
        }
        cluster = fat_read_entry(cluster);
        if (io_error) goto failed;
    }
done:
    return;
failed:
    dir_node_count = 0;
}

/* --------------------------------------------------------------------------
 * fat32_init: Initialize FAT32 from ATA disk
 * -------------------------------------------------------------------------- */
bool fat32_init(void) {
    volume_cache_reset();
    if (chain_epoch != 0xFFFFFFFFu) chain_epoch++;
    memset(seek_indices, 0, sizeof(seek_indices)); seek_next = 0;
    mounted = false;
    dir_node_count = 0;
    writes_blocked = false;
    free_known = false;
    io_error = false;
    volume_lba = 0;
    volume_sectors = 20480;                /* legacy FAT must not enter NPFS */

#ifdef NEXUS_NATIVE64
    /* block_probe chose the root volume (see blockdev.h for the order); read its boot sector. */
    volume_part = block_root();
    if (!volume_part) return false;
    { uint64_t start = volume_part->start * (volume_part->dev->block_size / 512), sectors = block_part_sectors(volume_part);
      volume_lba = (uint32_t)start; volume_sectors = sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sectors; }
    if (volume_read(0, 1, sector_buf) < 0) {
        vga_print("[!!] FAT32: cannot read BPB\n");
        return false;
    }
    memcpy(&bpb, sector_buf, sizeof(bpb));
#else
    if (!ata_disk_present()) {
        return false;
    }

    /* Read first sector (BPB). The read is a full 512-byte sector, so land it
     * in sector_buf and copy out the 90-byte struct (reading straight into
     * &bpb overran it into the neighbouring .bss). */
    if (ata_read_sectors(0, 1, sector_buf) < 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("FAT32: cannot read BPB\n");
        return false;
    }
    memcpy(&bpb, sector_buf, sizeof(bpb));
    if (bpb.bytes_per_sector != 512 || !bpb.fat_size_32) {
        if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) return false;
        for (int off = 446; off < 510; off += 16) {
            uint32_t start, count;
            memcpy(&start, sector_buf + off + 8, 4); memcpy(&count, sector_buf + off + 12, 4);
            if ((sector_buf[off + 4] == 0x0B || sector_buf[off + 4] == 0x0C) && start >= 32768 &&
                start < ata_get_sectors() && count && count <= ata_get_sectors() - start) {
                volume_lba = start; volume_sectors = count; break;
            }
        }
        if (!volume_lba || volume_read(0, 1, sector_buf) < 0) return false;
        memcpy(&bpb, sector_buf, sizeof(bpb));
    }
#endif

    /* Validate FAT32 signature */
    if (bpb.bytes_per_sector != 512) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("FAT32: invalid sector size\n");
        return false;
    }

    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA || bpb.fat_size_32 == 0 ||
        bpb.sectors_per_cluster == 0 || bpb.sectors_per_cluster > 128 ||
        (bpb.sectors_per_cluster & (bpb.sectors_per_cluster - 1)) ||
        bpb.num_fats < 1 || bpb.num_fats > 2 || bpb.reserved_sectors < 2 || bpb.ext_flags ||
        !bpb.total_sectors_32 || bpb.total_sectors_32 > volume_sectors ||
        bpb.reserved_sectors >= bpb.total_sectors_32 ||
        bpb.fat_size_32 > (bpb.total_sectors_32 - bpb.reserved_sectors) / bpb.num_fats) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("FAT32: not a FAT32 volume\n");
        return false;
    }

    /* Calculate layout */
    sectors_per_cluster = bpb.sectors_per_cluster;
    bytes_per_cluster = sectors_per_cluster * 512;
    fat_start_lba = bpb.reserved_sectors;
    data_start_lba = fat_start_lba + (bpb.num_fats * bpb.fat_size_32);
    root_cluster = bpb.root_cluster;
    volume_sectors = bpb.total_sectors_32;
    volume_cache_reset();
    if (data_start_lba >= volume_sectors) return false;
    cluster_limit = (volume_sectors - data_start_lba) / sectors_per_cluster + 2;
    if ((cluster_limit + 127) / 128 > bpb.fat_size_32 || root_cluster < 2 || root_cluster >= cluster_limit) return false;
    next_free = 2;

    /* Initialize FAT cache */
    fat_cache_start = 0xFFFFFFFF;

#ifdef NEXUS_NATIVE64
    /* A volume left dirty (crash, power loss, killed VM) is checked and repaired before the mount (fatcheck.inc). */
    if (fat32_boot_check() < 0) writes_blocked = true;
#else
    for (uint32_t copy = 0; copy < bpb.num_fats; copy++) {
        if (volume_read(fat_start_lba + copy * bpb.fat_size_32, 1, sector_buf) < 0) return false;
        uint32_t state; memcpy(&state, sector_buf + 4, 4);
        if ((state & 0x03FFFFFF) >= 0x03FFFFF8 && !(state & FAT32_CLEAN)) writes_blocked = true;
    }
    if (writes_blocked) vga_print("[FAT32] unclean metadata; mounted read-only until filesystem repair\n");
#endif

    /* Read root directory */
    fat32_read_root_dir();
    if (io_error) { vga_print("[FAT32] cannot read root directory\n"); return false; }

    /* Setup VFS root node */
    memset(&fat32_root_node, 0, sizeof(fs_node_t));
    strncpy(fat32_root_node.name, "disk", FS_NAME_MAX);
    fat32_root_node.type = FS_DIRECTORY;
    fat32_root_node.inode = root_cluster;
    fat32_root_node.readdir = fat32_vfs_readdir;
    fat32_root_node.finddir = fat32_vfs_finddir;

    mounted = true;
    memset(f57_hints, 0, sizeof(f57_hints)); f57_lc_reset();

    /* Print volume label */
    char label[12];
    memcpy(label, bpb.volume_label, 11);
    label[11] = '\0';
    /* Trim trailing spaces */
    for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = '\0';

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("FAT32: \"");
    vga_print(label);
    vga_print("\" ");
    char nbuf[12];
    int_to_str(dir_node_count, nbuf);
    vga_print(nbuf);
    vga_print(" files\n");

    return true;
}

fs_node_t* fat32_get_root(void) {
    return mounted ? &fat32_root_node : NULL;
}

bool fat32_is_mounted(void) { return mounted; }
bool fat32_is_readonly(void) { return writes_blocked; }
int fat32_stat_volume(fat32_space_t* space) {
    FAT_ENTRY();
    if (!mounted || !space) return -1;
    if (!free_known) {
        uint32_t count = 0; io_error = false;
        for (uint32_t cluster = 2; cluster < cluster_limit; cluster++) {
            uint32_t value = fat_read_entry(cluster); if (io_error) return -1;
            if (value == FAT32_FREE) count++;
        }
        free_clusters = count; free_known = true;
    }
    *space = (fat32_space_t){bytes_per_cluster, cluster_limit - 2, free_clusters, bpb.volume_id, volume_lba};
    return 0;
}

/* --------------------------------------------------------------------------
 * fat32_create_file: Create a new file in root directory
 * -------------------------------------------------------------------------- */
int fat32_create_file(const char* name, uint8_t type) {
    if (!mounted) return -1;
    if (dir_node_count >= MAX_DIR_ENTRIES) return -1;

    /* Allocate a cluster for the new file */
    uint32_t new_cluster = fat_alloc_cluster();
    if (new_cluster == 0) return -1;

    /* Zero out the cluster */
    memset(sector_buf, 0, 512);
    uint32_t lba = cluster_to_lba(new_cluster);
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        volume_write(lba + s, 1, sector_buf);
    }

    /* Find empty slot in root directory */
    uint32_t cluster = root_cluster;
    for (uint32_t hops = dir_max_clusters(); hops-- && cluster >= 2 && cluster < FAT32_EOC; ) {
        uint32_t dir_lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            volume_read(dir_lba + s, 1, sector_buf);
            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            int ecount = 512 / sizeof(fat32_dirent_t);

            for (int e = 0; e < ecount; e++) {
                if (entries[e].name[0] == 0x00 || (uint8_t)entries[e].name[0] == 0xE5) {
                    /* Found empty slot */
                    memset(&entries[e], 0, sizeof(fat32_dirent_t));
                    string_to_fat_name(name, entries[e].name);
                    entries[e].attr = (type == FS_DIRECTORY) ? ATTR_DIRECTORY : ATTR_ARCHIVE;
                    entries[e].first_cluster_hi = (new_cluster >> 16) & 0xFFFF;
                    entries[e].first_cluster_lo = new_cluster & 0xFFFF;
                    entries[e].file_size = 0;

                    volume_write(dir_lba + s, 1, sector_buf);

                    /* Refresh directory cache */
                    fat32_read_root_dir();
                    return io_error ? -1 : 0;
                }
            }
        }
        cluster = fat_read_entry(cluster);
    }

    return -1;
}

/* --------------------------------------------------------------------------
 * fat32_delete_file: Delete a file from root directory
 * -------------------------------------------------------------------------- */
int fat32_delete_file(const char* name) {
    if (!mounted) return -1;

    char fat_name[11];
    string_to_fat_name(name, fat_name);

    uint32_t cluster = root_cluster;
    for (uint32_t hops = dir_max_clusters(); hops-- && cluster >= 2 && cluster < FAT32_EOC; ) {
        uint32_t dir_lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            volume_read(dir_lba + s, 1, sector_buf);
            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            int ecount = 512 / sizeof(fat32_dirent_t);

            for (int e = 0; e < ecount; e++) {
                if (entries[e].name[0] == 0x00) goto not_found;
                if ((uint8_t)entries[e].name[0] == 0xE5) continue;
                if (entries[e].attr == ATTR_LFN) continue;

                if (memcmp(entries[e].name, fat_name, 11) == 0) {
                    /* Free cluster chain */
                    uint32_t fc = ((uint32_t)entries[e].first_cluster_hi << 16) |
                                   entries[e].first_cluster_lo;
                    for (uint32_t lim = bpb.fat_size_32 * 128;
                         lim-- && fc >= 2 && fc < FAT32_EOC; ) {
                        uint32_t next = fat_read_entry(fc);
                        fat_write_entry(fc, FAT32_FREE);
                        fc = next;
                    }

                    /* Mark entry as deleted */
                    entries[e].name[0] = (char)0xE5;
                    f57_hint_drop(root_cluster);
                    volume_write(dir_lba + s, 1, sector_buf);

                    fat32_read_root_dir();
                    return io_error ? -1 : 0;
                }
            }
        }
        cluster = fat_read_entry(cluster);
    }
not_found:
    return -1;
}

uint32_t fat32_get_file_count(void) {
    return (uint32_t)dir_node_count;
}

/* ============================================================================
 * ---- Phase 57 apps: sub-directories, streamed reads, writes ----
 * ============================================================================
 * The original driver only caches the root directory. User programs live in
 * /disk/bin and /disk/share, so this block adds path lookup through any depth
 * of 8.3-named sub-directories, offset/length reads straight into a caller
 * buffer (the ELF loader reads segments directly into user pages), and simple
 * writes (append/extend, truncate, create, remove). Own buffers: the legacy
 * functions above keep using sector_buf.
 * ============================================================================ */
static uint8_t  f57_clu[65536];                 /* maximum FAT cluster       */
static uint8_t  f57_sec[512];                   /* directory sector R/M/W     */

static bool f57_valid_cluster(uint32_t c) { return c >= 2 && c < cluster_limit; }

static void f57_fill(fat32_file_t* out, const fat32_dirent_t* e, uint32_t lba, uint32_t off) {
    memset(out, 0, sizeof(*out));
    out->first  = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
    out->size   = e->file_size;
    out->is_dir = (e->attr & ATTR_DIRECTORY) != 0;
    if (!out->is_dir && (e->nt_reserved & 0xE0) == 0xA0) out->native_kind = e->nt_reserved & 7;
    out->read_only = !out->is_dir && (e->attr & ATTR_READ_ONLY) != 0;
    out->dir_lba = lba;
    out->dir_off = off;
    out->wdate  = e->write_date;
    out->wtime  = e->write_time;
    fat_name_to_string(e->name, out->name);
}

static void f57_root(fat32_file_t* out) {
    memset(out, 0, sizeof(*out));
    out->first = root_cluster; out->is_dir = true;
    out->name[0] = '/'; out->name[1] = 0;
}

static bool name_equal(const char* a, const char* b) {
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}
static uint8_t lfn_checksum(const char* name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)name[i]);
    return sum;
}
static const uint8_t lfn_offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
#ifdef NEXUS_NATIVE64
/* Characters FAT long names reserve are stored as U+F000+c (the Cygwin/WSL convention), so POSIX names such as
 * Wine's dosdevices/c: and com1: work on FAT; lfn_decode maps them back. */
static bool lfn_reserved(uint32_t c) { return c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|'; }
#endif

static bool lfn_decode(const uint16_t* text, char* name) {
    int n = 0;
    for (int i = 0; i < 260 && text[i] && text[i] != 0xFFFF; i++) {
        uint32_t cp = text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (++i >= 260 || text[i] < 0xDC00 || text[i] > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + text[i] - 0xDC00;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
#ifdef NEXUS_NATIVE64
        if (cp >= 0xF000 && cp < 0xF080 && lfn_reserved(cp - 0xF000)) cp -= 0xF000;
#endif
        int bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (n + bytes > FAT32_LFN_MAX) return false;
        if (bytes == 1) name[n++] = (char)cp;
        else {
            name[n++] = (char)((bytes == 2 ? 0xC0 : bytes == 3 ? 0xE0 : 0xF0) | (cp >> (6 * (bytes - 1))));
            for (int j = bytes - 2; j >= 0; j--) name[n++] = (char)(0x80 | ((cp >> (6 * j)) & 63));
        }
    }
    name[n] = 0;
    return n > 0;
}
static int lfn_encode(const char* name, uint16_t* text) {
    int n = 0;
    const uint8_t* p = (const uint8_t*)name;
    while (*p) {
        uint32_t cp = *p++;
#ifdef NEXUS_NATIVE64
        if (cp < 32 || cp == 127 || cp == '/') return -1;
        if (lfn_reserved(cp)) cp |= 0xF000;
        else
#else
        if (cp < 32 || cp == 127 || cp == '"' || cp == '*' || cp == '/' || cp == ':' ||
            cp == '<' || cp == '>' || cp == '?' || cp == '\\' || cp == '|') return -1;
#endif
        if (cp >= 128) {
            int more = cp >= 0xC2 && cp <= 0xDF ? 1 : cp >= 0xE0 && cp <= 0xEF ? 2 : cp >= 0xF0 && cp <= 0xF4 ? 3 : -1;
            if (more < 0) return -1;
            cp &= (1u << (6 - more)) - 1;
            for (int j = 0; j < more; j++) { if ((*p & 0xC0) != 0x80) return -1; cp = (cp << 6) | (*p++ & 63); }
            if (cp < (more == 1 ? 0x80u : more == 2 ? 0x800u : 0x10000u) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
#ifdef NEXUS_NATIVE64
            if (cp >= 0xF000 && cp < 0xF080 && lfn_reserved(cp - 0xF000)) return -1;   /* keep the mapping one-to-one */
#endif
        }
        if (n + (cp >= 0x10000 ? 2 : 1) > FAT32_LFN_MAX) return -1;
        if (cp >= 0x10000) { cp -= 0x10000; text[n++] = (uint16_t)(0xD800 | (cp >> 10)); text[n++] = (uint16_t)(0xDC00 | (cp & 1023)); }
        else text[n++] = (uint16_t)cp;
    }
    if (!n || text[n - 1] == '.' || text[n - 1] == ' ') return -1;
    return n;
}

typedef bool (*f57_dir_cb)(const fat32_file_t* file, void* ctx);
static bool f57_scan_complete;   /* the last f57_scan that found nothing reached the directory's real end */
static bool f57_scan(uint32_t dirc, f57_dir_cb cb, void* ctx) {
    uint16_t longname[260];
    uint8_t expected = 0, count = 0, checksum = 0;
    uint32_t c = dirc, index = 0;
    f57_scan_complete = false;
    for (uint32_t hops = dir_max_clusters(); hops-- && f57_valid_cluster(c); c = fat_read_entry(c)) {
        uint32_t lba = cluster_to_lba(c);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (volume_read(lba + s, 1, f57_sec) < 0) return false;
            const fat32_dirent_t* entries = (const fat32_dirent_t*)f57_sec;
            for (uint32_t i = 0; i < 16; i++, index++) {
                const fat32_dirent_t* e = &entries[i];
                const uint8_t* raw = (const uint8_t*)e;
                if (raw[0] == 0) { f57_scan_complete = !io_error; return false; }
                if (raw[0] == 0xE5) { count = expected = 0; continue; }
                if (e->attr == ATTR_LFN) {
                    uint8_t ord = raw[0] & 0x1F;
                    if ((raw[0] & 0x40) && ord && ord <= 20) {
                        memset(longname, 0xFF, sizeof(longname)); count = expected = ord; checksum = raw[13];
                    }
                    if (!expected || ord != expected || raw[13] != checksum || raw[12] || raw[26] || raw[27]) { count = expected = 0; continue; }
                    for (int k = 0; k < 13; k++) longname[(ord - 1) * 13 + k] = raw[lfn_offsets[k]] | ((uint16_t)raw[lfn_offsets[k] + 1] << 8);
                    expected--; continue;
                }
                if (!(e->attr & ATTR_VOLUME_ID)) {
                    fat32_file_t file;
                    f57_fill(&file, e, lba + s, i * 32);
                    strcpy(file.short_name, file.name);
                    file.parent = dirc; file.entry_index = index;
                    file.lfn_count = expected == 0 ? count : 0;
                    if (file.lfn_count && checksum == lfn_checksum(e->name)) {
                        if (!lfn_decode(longname, file.name)) strcpy(file.name, file.short_name);
                    }
                    if (file.is_dir && !file.first) file.first = root_cluster;
                    if (cb(&file, ctx)) return true;
                }
                count = expected = 0;
            }
        }
    }
    /* complete only at a proper end of chain: not after a link into a free/bad cluster or the hop limit */
    f57_scan_complete = !io_error && c >= FAT32_EOC;
    return false;
}

#ifdef NEXUS_NATIVE64
/* Lookup cache. Long-name slots of a live entry are never rewritten in place, deletion only writes
 * 0xE5 (never an end marker), and generated aliases are unique, so a slot that still holds the same
 * short name is the entry a scan would find. Names only enter a directory through f57_create and
 * f57 rename (both call f57_lc_added) or the legacy root writers (which drop the root's filter). */
#define F57_LC_SLOTS 512
#define F57_BLOOM_DIRS 32
#define F57_BLOOM_BITS 16384
#define F57_BLOOM_REFRESH 4096 /* names added since the build before it is rebuilt */
typedef struct {
    uint32_t dir, lba, entry_index; uint16_t off; uint8_t lfn_count, live;
    uint8_t short11[11];
    char name[FAT32_LFN_MAX + 1];
} f57_lc_t;
typedef struct { uint32_t dir, added; uint64_t bits[F57_BLOOM_BITS / 64]; } f57_bloom_t;
/* ~216 KiB: allocated at boot by fat32_lookup_cache_init (the bootstrap kernel image + BSS must stay
 * under 2 MiB); without it lookups simply scan. */
struct f57_lc_tables { f57_lc_t lc[F57_LC_SLOTS]; f57_bloom_t bloom[F57_BLOOM_DIRS]; uint64_t scratch[F57_BLOOM_BITS / 64]; };
uint64_t arena_alloc(uint64_t bytes);
static struct f57_lc_tables* f57_tab;
static unsigned f57_bloom_victim;
#define f57_lc f57_tab->lc
#define f57_bloom f57_tab->bloom
#define f57_bloom_scratch f57_tab->scratch
static bool f57_lc_ready(void) { return f57_tab != NULL; }
void fat32_lookup_cache_init(void) {
    if (!f57_tab) f57_tab = (struct f57_lc_tables*)(uintptr_t)arena_alloc(sizeof(struct f57_lc_tables));
}
static uint32_t f57_name_hash(const char* name) {
    uint32_t h = 2166136261u; /* folds exactly like name_equal */
    for (; *name; name++) { uint8_t c = (uint8_t)*name; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 16777619u; }
    return h;
}
static uint32_t f57_bloom_bit(uint32_t h, unsigned k) {
    uint32_t h2 = ((h >> 15) | (h << 17)) * 0x9E3779B1u | 1;
    return (h + k * h2) % F57_BLOOM_BITS;
}
static void f57_bloom_set(uint64_t* bits, uint32_t h) {
    for (unsigned k = 0; k < 3; k++) { uint32_t b = f57_bloom_bit(h, k); bits[b / 64] |= 1ULL << (b % 64); }
}
static bool f57_bloom_test(const uint64_t* bits, uint32_t h) {
    for (unsigned k = 0; k < 3; k++) { uint32_t b = f57_bloom_bit(h, k); if (!(bits[b / 64] & (1ULL << (b % 64)))) return false; }
    return true;
}
static int f57_bloom_find(uint32_t dir) {
    if (!f57_tab) return -1;
    for (int i = 0; i < F57_BLOOM_DIRS; i++) if (f57_bloom[i].dir == dir) return i;
    return -1;
}
static void f57_lc_drop_bloom(uint32_t dir) {
    int i = f57_bloom_find(dir); if (i >= 0) f57_bloom[i].dir = 0;
}
static void f57_lc_drop_dir(uint32_t dir) {
    if (!f57_tab) return;
    f57_lc_drop_bloom(dir);
    for (unsigned i = 0; i < F57_LC_SLOTS; i++) if (f57_lc[i].dir == dir) f57_lc[i].live = 0;
}
static void f57_lc_reset(void) {
    if (!f57_tab) return;
    for (unsigned i = 0; i < F57_LC_SLOTS; i++) f57_lc[i].live = 0;
    for (unsigned i = 0; i < F57_BLOOM_DIRS; i++) f57_bloom[i].dir = 0;
}
static void f57_lc_added(uint32_t dir, const char* name) {
    int i = f57_bloom_find(dir); if (i < 0) return;
    if (++f57_bloom[i].added > F57_BLOOM_REFRESH) { f57_bloom[i].dir = 0; return; }
    f57_bloom_set(f57_bloom[i].bits, f57_name_hash(name));
}
static void f57_lc_added_alias(uint32_t dir, const char alias11[11]) {
    char alias[13]; fat_name_to_string(alias11, alias); f57_lc_added(dir, alias);
}
static f57_lc_t* f57_lc_slot(uint32_t dir, uint32_t h) { return &f57_lc[(h ^ dir * 0x9E3779B1u) % F57_LC_SLOTS]; }
static bool f57_lc_get(uint32_t dir, const char* want, uint32_t h, fat32_file_t* out) {
    f57_lc_t* e = f57_lc_slot(dir, h);
    if (!e->live || e->dir != dir) return false;
    char alias[13]; fat_name_to_string((const char*)e->short11, alias);
    if (!name_equal(e->name, want) && !name_equal(alias, want)) return false;
    if (volume_read(e->lba, 1, f57_sec) < 0) return false;
    const fat32_dirent_t* d = (const fat32_dirent_t*)(f57_sec + e->off);
    const uint8_t* raw = (const uint8_t*)d;
    if (!raw[0] || raw[0] == 0xE5 || d->attr == ATTR_LFN || (d->attr & ATTR_VOLUME_ID) || memcmp(d->name, e->short11, 11)) {
        e->live = 0; return false;
    }
    f57_fill(out, d, e->lba, e->off);
    strcpy(out->short_name, out->name); strcpy(out->name, e->name);
    out->parent = dir; out->entry_index = e->entry_index; out->lfn_count = e->lfn_count;
    if (out->is_dir && !out->first) out->first = root_cluster;
    return true;
}
static void f57_lc_put(uint32_t dir, uint32_t h, const fat32_file_t* file, const uint8_t short11[11]) {
    f57_lc_t* e = f57_lc_slot(dir, h);
    e->dir = dir; e->lba = file->dir_lba; e->off = (uint16_t)file->dir_off;
    e->entry_index = file->entry_index; e->lfn_count = file->lfn_count;
    memcpy(e->short11, short11, 11); strcpy(e->name, file->name); e->live = 1;
}
#endif
typedef struct { const char* want; fat32_file_t* out; uint64_t* bloom; uint8_t short11[11]; } f57_find_t;
static bool f57_find_cb(const fat32_file_t* file, void* ctx) {
    f57_find_t* f = (f57_find_t*)ctx;
#ifdef NEXUS_NATIVE64
    if (f->bloom) { f57_bloom_set(f->bloom, f57_name_hash(file->name)); f57_bloom_set(f->bloom, f57_name_hash(file->short_name)); }
#endif
    if (!name_equal(file->name, f->want) && !name_equal(file->short_name, f->want)) return false;
    *f->out = *file;
    memcpy(f->short11, f57_sec + file->dir_off, 11); /* f57_scan calls back while the sector is loaded */
    return true;
}
static bool f57_find_in(uint32_t dirc, const char* comp, int clen, fat32_file_t* out) {
    char tmp[FAT32_LFN_MAX + 1];
    if (clen <= 0 || clen > FAT32_LFN_MAX) return false;
    memcpy(tmp, comp, clen); tmp[clen] = 0;
    f57_find_t f = {tmp, out, NULL, {0}};
#ifdef NEXUS_NATIVE64
    if (!f57_lc_ready()) return f57_scan(dirc, f57_find_cb, &f);
    uint32_t h = f57_name_hash(tmp);
    int b = f57_bloom_find(dirc);
    if (b >= 0 && !f57_bloom_test(f57_bloom[b].bits, h)) return false;
    if (f57_lc_get(dirc, tmp, h, out)) return true;
    bool had_error = io_error;
    if (b < 0) { memset(f57_bloom_scratch, 0, sizeof(f57_bloom_scratch)); f.bloom = f57_bloom_scratch; }
    bool found = f57_scan(dirc, f57_find_cb, &f);
    if (found) f57_lc_put(dirc, h, out, f.short11);
    else if (f.bloom && !io_error && !had_error && f57_scan_complete) {
        /* The scan saw every entry a lookup can match: install its name set. */
        int i = (int)(f57_bloom_victim++ % F57_BLOOM_DIRS);
        f57_bloom[i].dir = dirc; f57_bloom[i].added = 0;
        memcpy(f57_bloom[i].bits, f57_bloom_scratch, sizeof(f57_bloom_scratch));
    }
    return found;
#else
    return f57_scan(dirc, f57_find_cb, &f);
#endif
}

/* Split "a/b/c" into the parent dir entry + leaf component. */
static bool f57_parent(const char* path, fat32_file_t* parent, const char** leaf, int* leaflen) {
    while (*path == '/') path++;
    const char* last = path; const char* p = path;
    for (; *p; p++) if (*p == '/' && p[1]) last = p + 1;
    *leaf = last;
    int ll = 0; while (last[ll] && last[ll] != '/') ll++;
    *leaflen = ll;
    if (last == path) { f57_root(parent); return true; }
    char dir[FAT32_PATH_MAX];
    int dl = (int)(last - path - 1);
    if (dl >= FAT32_PATH_MAX) return false;
    memcpy(dir, path, dl); dir[dl] = 0;
    return fat32_lookup(dir, parent) && parent->is_dir;
}

bool fat32_lookup(const char* path, fat32_file_t* out) {
    FAT_ENTRY();
    if (!mounted || !path || !out) return false;
    fat32_file_t cur; f57_root(&cur);
    const char* p = path;
    while (*p == '/') p++;
    while (*p) {
        const char* s = p;
        while (*p && *p != '/') p++;
        int clen = (int)(p - s);
        while (*p == '/') p++;
        if (clen == 1 && s[0] == '.') continue;
        if (!cur.is_dir) return false;
        fat32_file_t next;
        if (!f57_find_in(cur.first, s, clen, &next)) return false;
        cur = next;
    }
    *out = cur;
    return true;
}

int fat32_lookup_status(const char* path, fat32_file_t* out) {
    FAT_ENTRY();
    if (!mounted || !path || !out) return -5;
    io_error = false;
    bool found = fat32_lookup(path, out);
    return io_error ? -5 : found ? 0 : -2;
}
static int f57_refresh(fat32_file_t* f) {
    if (f->chain_epoch != chain_epoch || chain_epoch == 0xFFFFFFFFu) {
        f->cursor = f->cursor_index = f->linear = 0; f->chain_epoch = chain_epoch;
    }
    if (f->detached) return 0;
    if (!f->dir_lba) return 0;
    if (volume_read(f->dir_lba, 1, f57_sec) < 0) return -1;
    const fat32_dirent_t* entry = (const fat32_dirent_t*)(f57_sec + f->dir_off);
    char name[13]; fat_name_to_string(entry->name, name);
    if (!name_equal(name, f->short_name) || ((entry->attr & ATTR_DIRECTORY) != 0) != f->is_dir) {
        vga_print("[FAT32] stale entry: expected "); vga_print(f->short_name); vga_print(" read "); vga_print(name); vga_print("\n"); return -1;
    }
    uint32_t first = ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
    if (first != f->first) f->cursor = f->cursor_index = f->linear = 0;
    f->first = first; f->size = entry->file_size;
    return 0;
}

int fat32_refresh_file(fat32_file_t* file) {
    FAT_ENTRY();
    if (!mounted || !file) return -1;
    io_error = false; return f57_refresh(file);
}
static uint32_t f57_next_file_cluster(fat32_file_t* f, uint32_t cluster, uint32_t index) {
    if (index + 1 < f->linear && cluster == f->first + index) return cluster + 1;
    uint32_t next = fat_read_entry(cluster);
    if (!io_error && index + 1 == f->linear && next == cluster + 1 && f57_valid_cluster(next)) f->linear++;
    return next;
}
/* Sparse chain checkpoints also cover fragmented executables and are shared by read aliases. */
static struct seek_index* f57_seek_index(const fat32_file_t* file) {
    uint32_t clusters = (file->size - 1) / bytes_per_cluster + 1;
    if (clusters <= SEEK_MARKS * 2 || chain_epoch == 0xFFFFFFFFu) return NULL;
    struct seek_index* index = NULL;
    for (unsigned i = 0; i < SEEK_FILES; i++) if (seek_indices[i].first == file->first) {
        index = &seek_indices[i];
        if (index->size == file->size && index->epoch == chain_epoch) return index;
        break;
    }
    if (!index) index = &seek_indices[seek_next++ % SEEK_FILES];
    memset(index, 0, sizeof(*index)); index->first = file->first; index->size = file->size; index->epoch = chain_epoch;
    while (((clusters - 1) >> index->shift) >= SEEK_MARKS) index->shift++;
    index->cluster[0] = file->first; return index;
}
static void f57_seek_record(struct seek_index* cache, uint32_t index, uint32_t cluster) {
    if (cache && !io_error && f57_valid_cluster(cluster) && !(index & ((1u << cache->shift) - 1)) &&
        (index >> cache->shift) < SEEK_MARKS) cache->cluster[index >> cache->shift] = cluster;
}
int32_t fat32_read_file(fat32_file_t* f, uint32_t off, uint32_t len, void* buf) {
    FAT_ENTRY();
    if (!mounted || !f || !buf) return -1;
    if (f->is_dir) return -1;
    if (f57_refresh(f) < 0) return -1;
    if (off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;
    io_error = false;
    uint32_t index = off / bytes_per_cluster, at = 0, c = f->first, done = 0;
    if (!f->linear && f57_valid_cluster(c)) f->linear = 1;
    if (f->linear) { at = index < f->linear ? index : f->linear - 1; c = f->first + at; }
    if (f57_valid_cluster(f->cursor) && f->cursor_index <= index && f->cursor_index > at) {
        c = f->cursor; at = f->cursor_index;
    }
    struct seek_index* cache = f57_seek_index(f);
    if (cache) for (uint32_t mark = index >> cache->shift; ; mark--) {
        if ((mark << cache->shift) <= at) break;
        if (f57_valid_cluster(cache->cluster[mark])) { c = cache->cluster[mark]; at = mark << cache->shift; break; }
        if (!mark) break;
    }
    while (at < index && f57_valid_cluster(c)) { c = f57_next_file_cluster(f, c, at); at++; f57_seek_record(cache, at, c); }
    uint32_t inoff = off % bytes_per_cluster;
    uint8_t* dst = (uint8_t*)buf;
    while (done < len && f57_valid_cluster(c)) {
        f->cursor = c; f->cursor_index = index;
        if (!inoff && len - done >= bytes_per_cluster) {
            /* Whole clusters: one command for each physically contiguous run. */
            uint32_t run_first = c, clusters = 1, follow = 0; bool have_follow = false;
            while (done + (clusters + 1) * bytes_per_cluster <= len) {
                uint32_t nx = f57_next_file_cluster(f, c, index);
                if (io_error) return -1;
                index++; f57_seek_record(cache, index, nx);
                if (nx != c + 1 || !f57_valid_cluster(nx)) { follow = nx; have_follow = true; break; }
                c = nx; clusters++; f->cursor = c; f->cursor_index = index;
            }
            if (volume_read_direct(cluster_to_lba(run_first), clusters * sectors_per_cluster, dst + done) < 0) return -1;
            done += clusters * bytes_per_cluster;
            if (done < len) {
                if (have_follow) c = follow;
                else { c = f57_next_file_cluster(f, c, index); index++; f57_seek_record(cache, index, c); }
            }
            continue;
        }
        if (volume_read(cluster_to_lba(c), sectors_per_cluster, f57_clu) < 0) return -1;
        uint32_t n = bytes_per_cluster - inoff;
        if (n > len - done) n = len - done;
        memcpy(dst + done, f57_clu + inoff, n);
        done += n; inoff = 0;
        if (done < len) { c = f57_next_file_cluster(f, c, index); index++; f57_seek_record(cache, index, c); }
    }
    return io_error ? -1 : (int32_t)done;
}

typedef struct { uint32_t want, seen; fat32_file_t* out; } f57_idx_t;
static bool f57_idx_cb(const fat32_file_t* file, void* ctx) {
    f57_idx_t* x = (f57_idx_t*)ctx;
    if (!strcmp(file->name, ".") || !strcmp(file->name, "..")) return false;
    if (x->seen++ != x->want) return false;
    *x->out = *file; return true;
}

bool fat32_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out) {
    FAT_ENTRY();
    if (!mounted || !dir || !dir->is_dir || !out) return false;
    f57_idx_t x; x.want = index; x.seen = 0; x.out = out;
    return f57_scan(dir->first, f57_idx_cb, &x);
}
int fat32_read_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out) {
    FAT_ENTRY();
    if (!mounted || !dir || !dir->is_dir || !out) return -1;
    io_error = false;
    bool found = fat32_dir_entry(dir, index, out);
    return io_error ? -1 : found ? 1 : 0;
}

/* Write the on-disk entry of f (size / first cluster / time) back. */
static int f57_update_dirent(const fat32_file_t* f) {
    if (fat_flush_dirty() < 0) return -1;
    if (f->detached) return 0;
    if (f->dir_lba == 0) return 0;                   /* root pseudo entry      */
    if (volume_read(f->dir_lba, 1, f57_sec) < 0) return -1;
    fat32_dirent_t* e = (fat32_dirent_t*)(f57_sec + f->dir_off);
    char current[13]; fat_name_to_string(e->name, current);
    if (!name_equal(current, f->short_name)) return -1;
    e->file_size = f->size;
    e->first_cluster_hi = (uint16_t)(f->first >> 16);
    e->first_cluster_lo = (uint16_t)(f->first & 0xFFFF);
    e->write_date = f->wdate; e->write_time = f->wtime;
    return volume_write(f->dir_lba, 1, f57_sec) < 0 ? -1 : 0;
}
int fat32_set_read_only(fat32_file_t* file, bool read_only) {
    FAT_ENTRY();
    if (!mounted || !file || !file->dir_lba || file->is_dir) return -1;
    io_error = false;
    if (f57_refresh(file) < 0 || fat_flush_dirty() < 0 || volume_read(file->dir_lba, 1, f57_sec) < 0) return -1;
    fat32_dirent_t* e = (fat32_dirent_t*)(f57_sec + file->dir_off);
    e->attr = (uint8_t)((e->attr & ~ATTR_READ_ONLY) | (read_only ? ATTR_READ_ONLY : 0));
    if (volume_write(file->dir_lba, 1, f57_sec) < 0) return -1;
    file->read_only = read_only; return 0;
}
int fat32_set_times(fat32_file_t* file, uint16_t date, uint16_t time) {
    FAT_ENTRY();
    if (!mounted || !file || !file->dir_lba) return -1;
    io_error = false;
    if (f57_refresh(file) < 0) return -1;
    file->wdate = date; file->wtime = time;
    return f57_update_dirent(file);
}

static bool f57_slot(uint32_t dir, uint32_t index, bool extend, uint32_t* lba, uint32_t* off) {
    uint32_t per_cluster = sectors_per_cluster * 16, hops = index / per_cluster;
    if (hops >= dir_max_clusters()) return false;
    uint32_t c = dir;
    while (hops--) {
        uint32_t next = fat_read_entry(c);
        if (io_error) return false;
        if (!f57_valid_cluster(next)) {
            if (next < FAT32_EOC || !extend) return false;
            next = fat_alloc_cluster(); if (!next) return false;
            memset(f57_clu, 0, bytes_per_cluster);
            if (volume_write(cluster_to_lba(next), sectors_per_cluster, f57_clu) < 0) return false;
            fat_write_entry(c, next); if (io_error) return false;
        }
        c = next;
    }
    uint32_t within = index % per_cluster;
    *lba = cluster_to_lba(c) + within / 16; *off = (within % 16) * 32;
    return f57_valid_cluster(c);
}
static int f57_put_slot(uint32_t dir, uint32_t index, const void* entry) {
    uint32_t lba, off;
    if (!f57_slot(dir, index, false, &lba, &off) || volume_read(lba, 1, f57_sec) < 0) return -1;
    memcpy(f57_sec + off, entry, 32);
    return volume_write(lba, 1, f57_sec);
}
static uint32_t f57_next_cluster(uint32_t c, bool extend) {
    uint32_t next = fat_read_entry(c);
    if (io_error) return 0;
    if (f57_valid_cluster(next)) return next;
    if (next < FAT32_EOC || !extend) return 0;
    next = fat_alloc_cluster(); if (!next) return 0;
    memset(f57_clu, 0, bytes_per_cluster);
    if (volume_write(cluster_to_lba(next), sectors_per_cluster, f57_clu) < 0) return 0;
    fat_write_entry(c, next); if (io_error) return 0;
    return next;
}
static bool f57_free_slots(uint32_t dir, uint32_t wanted, uint32_t* start) {
    uint32_t per_cluster = sectors_per_cluster * 16, limit = dir_max_clusters();
    uint32_t i = f57_hint_get(dir, wanted), c = dir, cluster = 0;
    if (i >= 65536 || i / per_cluster >= limit) i = 0;
    i -= i % 16; /* whole sectors; slots before the hint are part of no long-enough run */
    for (uint32_t hops = i / per_cluster; cluster < hops; cluster++) {
        /* The hinted cluster may be one past the chain's end (directory full): extend it then. */
        uint32_t next = f57_next_cluster(c, cluster + 1 == hops);
        if (!next) { if (io_error) return false; i = 0; c = dir; cluster = 0; break; } /* stale */
        c = next;
    }
    if (!f57_valid_cluster(c)) return false;
    uint32_t run = 0;
    bool after_end = false;
    for (uint32_t s = (i % per_cluster) / 16; ; s = 0) {
        uint32_t base = cluster_to_lba(c);
        for (; s < sectors_per_cluster; s++) {
            if (volume_read(base + s, 1, f57_sec) < 0) return false;
            for (uint32_t k = 0; k < 16; k++, i++) {
                uint8_t first = f57_sec[k * 32];
                if (!first) after_end = true;
                if (after_end || first == 0xE5) run++; else run = 0;
                if (run != wanted) continue;
                *start = i + 1 - run;
                f57_hint_found(dir, wanted, *start);
                if (after_end) { /* keep an explicit end marker after the new entries */
                    uint32_t lba, off;
                    if (k + 1 < 16) { lba = base + s; off = (k + 1) * 32; }
                    else if (!f57_slot(dir, i + 1, false, &lba, &off)) return !io_error;
                    else if (volume_read(lba, 1, f57_sec) < 0) return false;
                    f57_sec[off] = 0;
                    if (volume_write(lba, 1, f57_sec) < 0) return false;
                }
                return !io_error;
            }
        }
        if (i >= 65536 || ++cluster >= limit) return false;
        c = f57_next_cluster(c, true); if (!c) return false;
    }
}
static bool f57_short(uint32_t dir, const char* name, bool temporary, char out[11]) {
    char candidate[13]; fat32_file_t found;
    string_to_fat_name(name, out); fat_name_to_string(out, candidate);
    bool short_ok = name[0] != '.';
    int dots = 0;
    for (const char* p = name; *p; p++) {
        char one[2] = {*p, 0};
        if (*p == '.' && ++dots > 1) short_ok = false;
        if ((uint8_t)*p <= 32 || (uint8_t)*p >= 127 || strstr("\"*+,/:;<=>?[\\]|", one)) short_ok = false;
    }
    if (!temporary && short_ok && name_equal(name, candidate)) return true;
    static uint32_t sequence = 0;
    const char* ext = NULL;
    for (const char* p = name; *p; p++) if (*p == '.') ext = p + 1;
    for (uint32_t trial = 0; trial < 65536; trial++) {
        if (++sequence > 9999999) sequence = 1;
        memset(out, ' ', 11); out[0] = temporary ? 'T' : 'N';
        uint32_t n = sequence;
        for (int i = 7; i >= 1; i--) { out[i] = (char)('0' + n % 10); n /= 10; }
        if (temporary) memcpy(out + 8, "TMP", 3);
        else if (ext) for (int i = 0; i < 3 && ext[i]; i++) {
            char c = ext[i]; if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + i] = ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
        }
        fat_name_to_string(out, candidate);
        if (!f57_find_in(dir, candidate, (int)strlen(candidate), &found)) return !io_error;
    }
    return false;
}
static void f57_long_entry(const uint16_t* wide, unsigned slots, unsigned index, uint8_t checksum, uint8_t entry[32]) {
    unsigned ord = slots - index;
    memset(entry, 0, 32);
    entry[0] = (uint8_t)(ord | (index == 0 ? 0x40 : 0)); entry[11] = ATTR_LFN; entry[13] = checksum;
    for (int k = 0; k < 13; k++) {
        uint16_t v = wide[(ord - 1) * 13 + k]; entry[lfn_offsets[k]] = (uint8_t)v; entry[lfn_offsets[k] + 1] = (uint8_t)(v >> 8);
    }
}
static int f57_create(const char* path, bool is_dir, bool pending, uint8_t native_kind, fat32_file_t* out) {
    FAT_ENTRY();
    if (!mounted || !path) return -1;
    io_error = false;
    fat32_file_t parent, existing;
    const char* leaf; int len;
    if (!f57_parent(path, &parent, &leaf, &len)) return -3;
    if (len <= 0 || len > FAT32_LFN_MAX || leaf[len]) return -1;
    char name[FAT32_LFN_MAX + 1]; memcpy(name, leaf, len); name[len] = 0;
    uint16_t wide[260]; memset(wide, 0xFF, sizeof(wide));
    int units = lfn_encode(name, wide);
    if (units < 0) return -1;
    if (f57_find_in(parent.first, leaf, len, &existing)) return -2;
    if (io_error) return -1;
    char final_short[11], shortname[11];
    if (!f57_short(parent.first, name, false, final_short)) return -1;
    memcpy(shortname, final_short, 11);
    if (pending) do {
        if (!f57_short(parent.first, name, true, shortname)) return -1;
    } while (lfn_checksum(shortname) == lfn_checksum(final_short));
    char decoded[13]; fat_name_to_string(final_short, decoded);
    uint32_t long_slots = strcmp(name, decoded) ? (uint32_t)(units + 12) / 13 : 0;
    uint32_t start;
    if (!f57_free_slots(parent.first, long_slots + 1, &start)) return io_error ? -1 : -4;
    f57_lc_added(parent.first, name); f57_lc_added(parent.first, decoded);
#ifdef NEXUS_NATIVE64
    f57_lc_added_alias(parent.first, shortname);
#endif
    uint32_t first = 0;
    if (is_dir) {
        first = fat_alloc_cluster(); if (!first) return io_error ? -1 : -4;
        memset(f57_clu, 0, bytes_per_cluster);
        fat32_dirent_t* dot = (fat32_dirent_t*)f57_clu;
        memset(dot[0].name, ' ', 11); dot[0].name[0] = '.'; dot[0].attr = ATTR_DIRECTORY;
        dot[0].first_cluster_hi = (uint16_t)(first >> 16); dot[0].first_cluster_lo = (uint16_t)first;
        memset(dot[1].name, ' ', 11); dot[1].name[0] = dot[1].name[1] = '.'; dot[1].attr = ATTR_DIRECTORY;
        uint32_t pc = parent.first == root_cluster ? 0 : parent.first;
        dot[1].first_cluster_hi = (uint16_t)(pc >> 16); dot[1].first_cluster_lo = (uint16_t)pc;
        if (volume_write(cluster_to_lba(first), sectors_per_cluster, f57_clu) < 0) goto unpublished;
    }
    if (units < 260) wide[units] = 0;
    for (uint32_t i = 0; i < long_slots; i++) {
        uint8_t entry[32]; f57_long_entry(wide, long_slots, i, lfn_checksum(final_short), entry);
        if (f57_put_slot(parent.first, start + i, entry) < 0) goto unpublished;
    }
    fat32_dirent_t entry; memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, shortname, 11); entry.attr = is_dir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    if (native_kind) entry.nt_reserved = 0xA0 | native_kind;
    entry.first_cluster_hi = (uint16_t)(first >> 16); entry.first_cluster_lo = (uint16_t)first;
    if (f57_put_slot(parent.first, start + long_slots, &entry) < 0) goto unpublished;
    if (out) {
        uint32_t lba, off;
        if (!f57_slot(parent.first, start + long_slots, false, &lba, &off)) return -1;
        f57_fill(out, &entry, lba, off);
        strcpy(out->short_name, out->name); strcpy(out->name, name);
        out->parent = parent.first; out->entry_index = start + long_slots;
        out->lfn_count = (uint8_t)long_slots; out->pending = pending;
        memcpy(out->final_short, final_short, 11);
    }
    if (parent.first == root_cluster) fat32_read_root_dir();
    return io_error ? -1 : 0;
unpublished:   /* no entry names the new directory's cluster: give it back (a failed write may leave it to fsck) */
    if (first) { io_error = false; fat_write_entry(first, FAT32_FREE); }
    return -1;
}
int fat32_create_path(const char* path, bool is_dir, fat32_file_t* out) { return f57_create(path, is_dir, false, 0, out); }
int fat32_create_socket_path(const char* path, fat32_file_t* out) { return f57_create(path, false, false, FAT32_NATIVE_SOCKET, out); }
int fat32_begin_download(const char* path, fat32_file_t* out) { return f57_create(path, false, true, 0, out); }
static int f57_publish(fat32_file_t* file, uint8_t native_kind) {
    FAT_ENTRY();
    if (!file || !file->pending) return -1;
    io_error = false;
    fat32_file_t other;
    if (f57_find_in(file->parent, file->name, (int)strlen(file->name), &other)) return -2;
    char alias[13]; fat_name_to_string(file->final_short, alias);
    if (f57_find_in(file->parent, alias, (int)strlen(alias), &other)) return -2;
    if (io_error || volume_read(file->dir_lba, 1, f57_sec) < 0) return -1;
    fat32_dirent_t* entry = (fat32_dirent_t*)(f57_sec + file->dir_off);
    char current[13]; fat_name_to_string(entry->name, current);
    if (!name_equal(current, file->short_name)) return -1;
    memcpy(entry->name, file->final_short, 11);
    entry->nt_reserved = native_kind ? 0xA0 | native_kind : 0;
    if (volume_write(file->dir_lba, 1, f57_sec) < 0) return -1;
    fat_name_to_string(file->final_short, file->short_name); file->pending = false; file->native_kind = native_kind;
    return 0;
}
int fat32_finish_download(fat32_file_t* file) { return f57_publish(file, 0); }
int fat32_finish_symlink(fat32_file_t* file) { return f57_publish(file, FAT32_NATIVE_SYMLINK); }

static const uint8_t f57_zeros[4096];
static int fat32_write_file_body(fat32_file_t* f, uint32_t off, uint32_t len, const void* buf);
int fat32_write_file(fat32_file_t* f, uint32_t off, uint32_t len, const void* buf) {
    FAT_ENTRY();
    fat_defer_begin();
    int result = fat32_write_file_body(f, off, len, buf);
    if (fat_defer_end() < 0) return -1;
    return result;
}
static int fat32_write_file_body(fat32_file_t* f, uint32_t off, uint32_t len, const void* buf) {
    if (!mounted || !f || f->is_dir) return -1;
    if (len == 0) return 0;
    if (!buf || len > 0x7FFFFFFFu || off > 0xFFFFFFFFu - len) return -5;
    io_error = false;
    if (f57_refresh(f) < 0) return -1;
    if (off > f->size) {                             /* zero-fill the gap first */
        while (f->size < off) {
            uint32_t n = off - f->size; if (n > sizeof(f57_zeros)) n = sizeof(f57_zeros);
            if (fat32_write_file(f, f->size, n, f57_zeros) != (int)n) return -2;
        }
    }
    uint32_t c = f->first;
    if (!f57_valid_cluster(c)) {
        c = fat_alloc_cluster();
        if (!c) return -2;
        f->first = c;
        if (f57_update_dirent(f) < 0) {   /* nothing names the cluster: free it instead of leaking it */
            f->first = 0; io_error = false; fat_write_entry(c, FAT32_FREE); return -1;
        }
    }
    uint32_t want = off / bytes_per_cluster;
    uint32_t index = 0;
    if (f57_valid_cluster(f->cursor) && f->cursor_index <= want) { c = f->cursor; index = f->cursor_index; }
    for (; index < want; index++) {
        uint32_t nx = fat_read_entry(c);
        if (!f57_valid_cluster(nx)) { nx = fat_alloc_cluster(); if (!nx) return -2; fat_write_entry(c, nx); }
        c = nx;
        if (io_error) return -1;
    }
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t done = 0, inoff = off % bytes_per_cluster;
    /* Physically contiguous full-sector spans are merged into one volume write. */
    uint32_t run_lba = 0, run_count = 0; const uint8_t* run_src = NULL;
    uint32_t old_size = f->size, old_first = f->first;
    while (done < len) {
        f->cursor = c; f->cursor_index = index;
        uint32_t lba = cluster_to_lba(c) + inoff / 512;
        uint32_t so = inoff % 512;
        uint32_t n = 512 - so; if (n > len - done) n = len - done;
        if (n == 512) {
            n = len - done;
            if (n > bytes_per_cluster - inoff) n = bytes_per_cluster - inoff;
            n &= ~511u;
            if (run_count && run_lba + run_count == lba && run_src + run_count * 512 == src + done) run_count += n / 512;
            else {
                if (run_count && volume_write(run_lba, run_count, run_src) < 0) return -1;
                run_lba = lba; run_count = n / 512; run_src = src + done;
            }
        } else {
            if (run_count) { if (volume_write(run_lba, run_count, run_src) < 0) return -1; run_count = 0; }
            if (volume_read(lba, 1, f57_sec) < 0) return -1;
            memcpy(f57_sec + so, src + done, n);
            if (volume_write(lba, 1, f57_sec) < 0) return -1;
        }
        done += n; inoff += n;
        if (inoff >= bytes_per_cluster && done < len) {
            inoff = 0;
            uint32_t nx = fat_read_entry(c);
            if (!f57_valid_cluster(nx)) { nx = fat_alloc_cluster(); if (!nx) break; fat_write_entry(c, nx); }
            c = nx;
            index++;
        }
    }
    if (run_count && volume_write(run_lba, run_count, run_src) < 0) return -1;
    if (off + done > f->size) f->size = off + done;
    /* The entry only changes when the size or first cluster did. */
    if (io_error || ((f->size != old_size || f->first != old_first) && f57_update_dirent(f) < 0)) return -1;
    f->chain_epoch = chain_epoch;
    return (int)done;
}

static int fat32_truncate_file_body(fat32_file_t* f, uint32_t newsize);
int fat32_truncate_file(fat32_file_t* f, uint32_t newsize) {
    FAT_ENTRY();
    fat_defer_begin();
    int result = fat32_truncate_file_body(f, newsize);
    if (fat_defer_end() < 0) return -1;
    return result;
}
static int fat32_truncate_file_body(fat32_file_t* f, uint32_t newsize) {
    if (!mounted || !f || f->is_dir) return -1;
    io_error = false;
    if (f57_refresh(f) < 0) return -1;
    if (newsize > f->size) {
        while (f->size < newsize) {
            uint32_t n = newsize - f->size; if (n > sizeof(f57_zeros)) n = sizeof(f57_zeros);
            if (fat32_write_file(f, f->size, n, f57_zeros) != (int)n) return -2;
        }
        return 0;
    }
    if (newsize == f->size) return 0;
    uint32_t keep = newsize / bytes_per_cluster + (newsize % bytes_per_cluster != 0), last = f->first, tail;
    for (uint32_t i = 1; i < keep; i++) {
        if (!f57_valid_cluster(last)) return -1;
        last = fat_read_entry(last); if (io_error) return -1;
    }
    if (keep && !f57_valid_cluster(last)) return -1;
    tail = keep ? fat_read_entry(last) : f->first;
    if (io_error) return -1;
    fat32_file_t old = *f;
    f->size = newsize; if (!keep) f->first = 0;
    f->cursor = f->cursor_index = f->linear = 0;
    /* Publish the smaller file before freeing clusters that another file could reuse. */
    if (f57_update_dirent(f) < 0) { *f = old; return -1; }
    if (keep) { fat_write_entry(last, 0x0FFFFFFF); if (io_error) return -1; }
    for (uint32_t hops = cluster_limit; hops-- && f57_valid_cluster(tail); ) {
        uint32_t next = fat_read_entry(tail); if (io_error) return -1;
        fat_write_entry(tail, FAT32_FREE); if (io_error) return -1;
        tail = next;
    }
    return 0;
}

typedef struct { int n; } f57_cnt_t;
static bool f57_cnt_cb(const fat32_file_t* file, void* ctx) {
    if (strcmp(file->name, ".") && strcmp(file->name, "..")) ((f57_cnt_t*)ctx)->n++;
    return false;
}

/* Two 21-slot names span at most six sectors, plus a directory's '..' sector. */
static struct { uint32_t lba; uint8_t before[512], after[512]; } rename_sectors[7];
static unsigned rename_count;
static uint8_t* f57_edit_sector(uint32_t lba) {
    for (unsigned i = 0; i < rename_count; i++) if (rename_sectors[i].lba == lba) return rename_sectors[i].after;
    if (rename_count == sizeof(rename_sectors) / sizeof(rename_sectors[0])) return NULL;
    unsigned i = rename_count;
    if (volume_read(lba, 1, rename_sectors[i].before) < 0) return NULL;
    memcpy(rename_sectors[i].after, rename_sectors[i].before, 512);
    rename_sectors[i].lba = lba; rename_count++; return rename_sectors[i].after;
}
static int f57_erase_name(const fat32_file_t* file) {
    for (uint32_t i = 0; i <= file->lfn_count; i++) {
        uint32_t lba, off;
        if (!f57_slot(file->parent, file->entry_index - i, false, &lba, &off)) return -5;
        uint8_t* sector = f57_edit_sector(lba); if (!sector) return -5;
        sector[off] = 0xE5;
        f57_hint_freed(file->parent, file->entry_index - i);
    }
    return 0;
}
static int f57_commit_name(void) {
    bool toggle = true;   /* FAT[1] dirty while the sectors are written */
#ifdef NEXUS_NATIVE64
    toggle = !volume_dirty_tracking;   /* the session's dirty flag (fatcheck.inc) already covers an interruption */
#endif
    uint32_t state = 0, clean = 0;
    if (toggle) {
        state = fat_read_entry(1); if (io_error) return -5;
        clean = state ? state : 0x0FFFFFFF;
        fat_write_entry(1, clean & ~FAT32_CLEAN);
    }
    bool failed = false;
    if (io_error) goto restore_state;
    for (unsigned i = 0; i < rename_count; i++) {
        if (volume_write(rename_sectors[i].lba, 1, rename_sectors[i].after) == 0) continue;
        /* Include the failing write: a device may report failure after writing. */
        for (unsigned n = i + 1; n; n--)
            if (volume_write(rename_sectors[n - 1].lba, 1, rename_sectors[n - 1].before) < 0) failed = true;
        goto restore_state;
    }
    if (!toggle) return 0;
    fat_write_entry(1, clean);
    /* The namespace is committed; a failed clean marker must not undo it. */
    if (io_error) { writes_blocked = true; vga_print("[FAT32] clean marker failed; writes blocked\n"); }
    return 0;
restore_state:
    if (!failed && toggle) {
        io_error = false; fat_write_entry(1, state);
        failed = io_error;
    }
    if (failed) {
        writes_blocked = true;
        vga_print("[FAT32] metadata rollback failed; writes blocked until filesystem repair\n");
    }
    return -5;
}
static int f57_empty_directory(const fat32_file_t* file) {
    if (!file->is_dir) return 0;
    f57_cnt_t count = {0}; f57_scan(file->first, f57_cnt_cb, &count);
    return io_error ? -5 : count.n ? -39 : 0;
}
int fat32_detach_path(const char* path, fat32_file_t* detached) {
    FAT_ENTRY();
    if (!mounted || !path || !detached || writes_blocked) return -5;
    io_error = false; fat32_file_t file;
    if (!fat32_lookup(path, &file)) return io_error ? -5 : -2;
    if (!file.dir_lba || !strcmp(file.name, ".") || !strcmp(file.name, "..")) return -16;
    int error = f57_empty_directory(&file); if (error) return error;
    if (file.is_dir) f57_lc_drop_dir(file.first);
    rename_count = 0;
    if ((error = f57_erase_name(&file)) || (error = f57_commit_name())) return error;
    file.detached = true; *detached = file;
    if (file.parent == root_cluster) fat32_read_root_dir();
    return 0;
}
static int fat32_dispose_file_body(fat32_file_t* file);
int fat32_dispose_file(fat32_file_t* file) {
    FAT_ENTRY();
    fat_defer_begin();
    int result = fat32_dispose_file_body(file);
    if (fat_defer_end() < 0) return -5;
    return result;
}
static int fat32_dispose_file_body(fat32_file_t* file) {
    if (!mounted || !file || !file->detached) return -22;
    if (writes_blocked) return -5;
    io_error = false;
    if (file->is_dir) f57_lc_drop_dir(file->first);
    for (uint32_t hops = cluster_limit; hops-- && f57_valid_cluster(file->first); ) {
        uint32_t cluster = file->first, next = fat_read_entry(cluster); if (io_error) return -5;
        fat_write_entry(cluster, FAT32_FREE);
        /* Never retry freeing a cluster whose write may already have succeeded. */
        file->first = next;
        if (io_error) return -5;
    }
    file->first = file->size = file->cursor = file->cursor_index = 0;
    return 0;
}
int fat32_rename_path(const char* source, const char* destination, bool no_replace,
                      fat32_file_t* moved, fat32_file_t* replaced) {
    FAT_ENTRY();
    if (!mounted || !source || !destination || !moved || !replaced || writes_blocked) return -5;
    io_error = false;
    fat32_file_t from, to, parent;
    if (!fat32_lookup(source, &from)) return io_error ? -5 : -2;
    if (!from.dir_lba || !strcmp(from.name, ".") || !strcmp(from.name, "..")) return -16;
    const char* leaf; int length;
    if (!f57_parent(destination, &parent, &leaf, &length)) return io_error ? -5 : -2;
    if (length <= 0 || length > FAT32_LFN_MAX || leaf[length]) return -22;
    char name[FAT32_LFN_MAX + 1]; memcpy(name, leaf, length); name[length] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -16;
    uint16_t wide[260]; memset(wide, 0xFF, sizeof(wide));
    int units = lfn_encode(name, wide); if (units < 0) return -22;
    wide[units] = 0;
    bool exists = f57_find_in(parent.first, leaf, length, &to);
    if (io_error) return -5;
    if (exists && no_replace) return -17;
    if (exists && to.dir_lba == from.dir_lba && to.dir_off == from.dir_off) {
        *moved = from; memset(replaced, 0, sizeof(*replaced)); return 0;
    }
    if (exists && from.is_dir != to.is_dir) return from.is_dir ? -20 : -21;
    int error = exists ? f57_empty_directory(&to) : 0; if (error) return error;
    if (from.is_dir) {
        uint32_t ancestor = parent.first;
        for (uint32_t hops = cluster_limit; ; ) {
            if (ancestor == from.first) return -22;
            if (ancestor == root_cluster) break;
            fat32_file_t up;
            if (!hops-- || !f57_find_in(ancestor, "..", 2, &up) || !up.is_dir || !f57_valid_cluster(up.first)) return -5;
            ancestor = up.first;
        }
    }
    rename_count = 0;
    if (!exists) {
        memset(&to, 0, sizeof(to)); to.parent = parent.first; strcpy(to.name, name);
        if (!f57_short(parent.first, name, false, to.final_short)) return -5;
        fat_name_to_string(to.final_short, to.short_name);
        f57_lc_added(parent.first, to.name); f57_lc_added(parent.first, to.short_name);
        to.lfn_count = strcmp(name, to.short_name) ? (unsigned)(units + 12) / 13 : 0;
        uint32_t start;
        if (!f57_free_slots(parent.first, to.lfn_count + 1, &start)) return io_error ? -5 : -28;
        to.entry_index = start + to.lfn_count;
        if (!f57_slot(parent.first, to.entry_index, false, &to.dir_lba, &to.dir_off)) return -5;
        for (unsigned i = 0; i < to.lfn_count; i++) {
            uint32_t lba, off;
            if (!f57_slot(parent.first, start + i, false, &lba, &off)) return -5;
            uint8_t* sector = f57_edit_sector(lba); if (!sector) return -5;
            f57_long_entry(wide, to.lfn_count, i, lfn_checksum(to.final_short), sector + off);
        }
    }
    fat32_file_t old_to = to;
    uint8_t* target = f57_edit_sector(to.dir_lba);
    if (!target) { error = -5; goto rollback; }
    if (volume_read(from.dir_lba, 1, f57_sec) < 0) { error = -5; goto rollback; }
    fat32_dirent_t entry; memcpy(&entry, f57_sec + from.dir_off, sizeof(entry));
    if (exists) memcpy(entry.name, target + to.dir_off, 11);
    else memcpy(entry.name, to.final_short, 11);
    memcpy(target + to.dir_off, &entry, sizeof(entry));
    if ((error = f57_erase_name(&from))) goto rollback;
    if (from.is_dir && from.parent != parent.first) {
        uint8_t* dot = f57_edit_sector(cluster_to_lba(from.first));
        if (!dot || memcmp(dot + 32, "..         ", 11) || !(dot[43] & ATTR_DIRECTORY)) { error = -5; goto rollback; }
        fat32_dirent_t* up = (void*)(dot + 32);
        uint32_t cluster = parent.first == root_cluster ? 0 : parent.first;
        up->first_cluster_hi = (uint16_t)(cluster >> 16); up->first_cluster_lo = (uint16_t)cluster;
    }
    if ((error = f57_commit_name())) goto rollback;
    f57_fill(moved, &entry, to.dir_lba, to.dir_off);
    strcpy(moved->short_name, moved->name); strcpy(moved->name, to.name);
    moved->parent = to.parent; moved->entry_index = to.entry_index; moved->lfn_count = to.lfn_count;
    memset(replaced, 0, sizeof(*replaced));
    if (exists) { *replaced = old_to; replaced->detached = true; if (old_to.is_dir) f57_lc_drop_dir(old_to.first); }
    if (from.parent == root_cluster || to.parent == root_cluster) fat32_read_root_dir();
    /* shortcut: FAT has no power-loss journal; interrupted multi-sector updates need filesystem repair. */
    return 0;
rollback:
    return error;
}

static int f57_remove(fat32_file_t* file) {
    if (file->dir_lba == 0) return -2;
    if (f57_refresh(file) < 0) return -1;
    if (file->is_dir) {
        f57_cnt_t count = {0}; f57_scan(file->first, f57_cnt_cb, &count);
        if (count.n || io_error) return -3;
    }
    /* Unpublish first so an I/O failure while freeing cannot expose reused data. */
    if (volume_read(file->dir_lba, 1, f57_sec) < 0) return -1;
    f57_sec[file->dir_off] = 0xE5;
    if (volume_write(file->dir_lba, 1, f57_sec) < 0) return -1;
    for (uint32_t i = 0; i <= file->lfn_count; i++) f57_hint_freed(file->parent, file->entry_index - i);
    if (file->is_dir) { f57_hint_drop(file->first); f57_lc_drop_dir(file->first); }
    uint8_t deleted[32]; memset(deleted, 0, sizeof(deleted)); deleted[0] = 0xE5;
    for (uint32_t i = 0; i < file->lfn_count; i++)
        if (f57_put_slot(file->parent, file->entry_index - file->lfn_count + i, deleted) < 0) return -1;
    uint32_t c = file->first;
    for (uint32_t lim = cluster_limit; lim-- && f57_valid_cluster(c); ) {
        uint32_t next = fat_read_entry(c); if (io_error) return -1;
        fat_write_entry(c, FAT32_FREE); if (io_error) return -1;
        c = next;
    }
    file->pending = false; file->first = file->size = file->cursor = 0;
    if (file->parent == root_cluster) fat32_read_root_dir();
    return io_error ? -1 : 0;
}
int fat32_abort_download(fat32_file_t* file) {
    FAT_ENTRY();
    if (!file || !file->pending) return 0;
    io_error = false;
    return f57_remove(file);
}
int fat32_remove_path(const char* path) {
    FAT_ENTRY();
    if (!mounted) return -1;
    io_error = false;
    fat32_file_t file;
    if (!fat32_lookup(path, &file)) return -1;
    return f57_remove(&file);
}
/* FAT date/time -> seconds since 1970 (days-from-civil, integer only). */
uint32_t fat32_time_to_unix(uint16_t date, uint16_t time) {
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12) m = 1;
    if (d < 1) d = 1;
    y -= (m <= 2);
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = (uint32_t)(era * 146097 + doe - 719468);
    return days * 86400u + (uint32_t)(time >> 11) * 3600u + (uint32_t)((time >> 5) & 63) * 60u +
           (uint32_t)(time & 31) * 2u;
}
/* ---- end Phase 57 apps ---- */
#ifdef NEXUS_NATIVE64
int fat32_file_extents(fat32_file_t* file, const struct block_partition** part, fat32_extent_fn emit, void* context) {
    FAT_ENTRY();
    if (!mounted || !file || !part || !emit || file->is_dir) return -22;
    io_error = false;
    if (f57_refresh(file) < 0) return -5;
    *part = volume_part;
    uint64_t clusters = file->size ? (file->size - 1) / bytes_per_cluster + 1 : 0, index = 0;
    uint32_t c = file->first;
    while (index < clusters) {   /* index grows every pass: a looping chain ends at the file size */
        if (!f57_valid_cluster(c)) return -22;
        uint32_t first = c; uint64_t run = 1, next = fat_read_entry(c);
        while (index + run < clusters && next == (uint64_t)c + 1 && f57_valid_cluster((uint32_t)next)) { c = (uint32_t)next; run++; next = fat_read_entry(c); }
        if (io_error) return -5;
        int error = emit(context, index * bytes_per_cluster, cluster_to_lba(first), run * sectors_per_cluster);
        if (error) return error;
        index += run; c = (uint32_t)next;
    }
    return io_error ? -5 : 0;
}
#include "../arch/x86_64/fatcheck.inc"   /* dirty flag, boot-time check/repair, clean unmount */
#endif
