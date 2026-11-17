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

/* Sector buffer */
static uint8_t sector_buf[512];

/* Cached FAT sectors */
#define FAT_CACHE_SECTORS 4
static uint8_t fat_cache[FAT_CACHE_SECTORS * 512];
static uint32_t fat_cache_start = 0xFFFFFFFF;

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
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

/* --------------------------------------------------------------------------
 * Helper: point at a cluster's FAT entry inside the FAT cache (loading the
 * sector run if needed). NULL on a disk error.
 * -------------------------------------------------------------------------- */
static uint32_t* fat_entry_ptr(uint32_t cluster, uint32_t* sector_out) {
    uint32_t fat_sector = fat_start_lba + (cluster * 4) / 512;
    if (fat_sector < fat_cache_start ||
        fat_sector >= fat_cache_start + FAT_CACHE_SECTORS) {
        fat_cache_start = 0xFFFFFFFF;
        if (ata_read_sectors(fat_sector, FAT_CACHE_SECTORS, fat_cache) < 0) return NULL;
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
    *e = (*e & 0xF0000000) | (value & 0x0FFFFFFF);   /* keep top 4 bits */
    if (ata_write_sectors(fat_sector, 1,
                          &fat_cache[(fat_sector - fat_cache_start) * 512]) < 0)
        fat_cache_start = 0xFFFFFFFF;                  /* cache != disk now */
}

/* Max clusters a directory chain may span: FAT caps a directory at 65536
 * 32-byte entries (2 MiB). Bounds chain walks so a corrupt/cyclic FAT can't
 * hang the kernel. */
static uint32_t dir_max_clusters(void) { return 0x200000u / bytes_per_cluster; }

/* --------------------------------------------------------------------------
 * Helper: Find a free cluster in the FAT
 * -------------------------------------------------------------------------- */
static uint32_t fat_alloc_cluster(void) {
    /* Highest valid cluster + 1: data clusters only (the BPB total includes
     * the reserved + FAT sectors), capped by the FAT's own entry count. The
     * old total_sectors/spc bound let allocation run past the volume end. */
    if (bpb.total_sectors_32 <= data_start_lba) return 0;
    uint32_t total = (bpb.total_sectors_32 - data_start_lba) / sectors_per_cluster + 2;
    if (total > bpb.fat_size_32 * 128) total = bpb.fat_size_32 * 128;
    for (uint32_t c = 2; c < total; c++) {
        if (fat_read_entry(c) == FAT32_FREE) {
            fat_write_entry(c, 0x0FFFFFFF); /* Mark as EOC */
            return c;
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
            ata_read_sectors(lba + s, 1, sector_buf);

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
        ata_read_sectors(lba + sector_offset, 1, sector_buf);

        uint32_t to_write = 512 - byte_offset;
        if (to_write > size - written) to_write = size - written;

        memcpy(sector_buf + byte_offset, buffer + written, to_write);
        ata_write_sectors(lba + sector_offset, 1, sector_buf);

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
    dir_node_count = 0;
    uint32_t cluster = root_cluster;

    for (uint32_t hops = dir_max_clusters();
         hops-- && cluster >= 2 && cluster < FAT32_EOC && dir_node_count < MAX_DIR_ENTRIES; ) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);

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
    }
done:
    return;
}

/* --------------------------------------------------------------------------
 * fat32_init: Initialize FAT32 from ATA disk
 * -------------------------------------------------------------------------- */
bool fat32_init(void) {
    mounted = false;

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

    /* Validate FAT32 signature */
    if (bpb.bytes_per_sector != 512) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("FAT32: invalid sector size\n");
        return false;
    }

    if (bpb.fat_size_32 == 0 || bpb.sectors_per_cluster == 0) {
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

    /* Initialize FAT cache */
    fat_cache_start = 0xFFFFFFFF;

    /* Read root directory */
    fat32_read_root_dir();

    /* Setup VFS root node */
    memset(&fat32_root_node, 0, sizeof(fs_node_t));
    strncpy(fat32_root_node.name, "disk", FS_NAME_MAX);
    fat32_root_node.type = FS_DIRECTORY;
    fat32_root_node.inode = root_cluster;
    fat32_root_node.readdir = fat32_vfs_readdir;
    fat32_root_node.finddir = fat32_vfs_finddir;

    mounted = true;

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
        ata_write_sectors(lba + s, 1, sector_buf);
    }

    /* Find empty slot in root directory */
    uint32_t cluster = root_cluster;
    for (uint32_t hops = dir_max_clusters(); hops-- && cluster >= 2 && cluster < FAT32_EOC; ) {
        uint32_t dir_lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            ata_read_sectors(dir_lba + s, 1, sector_buf);
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

                    ata_write_sectors(dir_lba + s, 1, sector_buf);

                    /* Refresh directory cache */
                    fat32_read_root_dir();
                    return 0;
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
            ata_read_sectors(dir_lba + s, 1, sector_buf);
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
                    ata_write_sectors(dir_lba + s, 1, sector_buf);

                    fat32_read_root_dir();
                    return 0;
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
static uint8_t  f57_clu[4096];                  /* one cluster (spc <= 8)     */
static uint8_t  f57_sec[512];                   /* directory sector R/M/W     */

static bool f57_valid_cluster(uint32_t c) { return c >= 2 && c < FAT32_EOC; }

static void f57_fill(fat32_file_t* out, const fat32_dirent_t* e, uint32_t lba, uint32_t off) {
    out->first  = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
    out->size   = e->file_size;
    out->is_dir = (e->attr & ATTR_DIRECTORY) != 0;
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

/* Visit every live entry of directory `dirc`. cb returns true to stop (hit). */
typedef bool (*f57_dir_cb)(const fat32_dirent_t* e, uint32_t lba, uint32_t off, void* ctx);
static bool f57_scan(uint32_t dirc, f57_dir_cb cb, void* ctx) {
    uint32_t c = dirc;
    for (uint32_t hops = dir_max_clusters(); hops-- && f57_valid_cluster(c); c = fat_read_entry(c)) {
        uint32_t lba = cluster_to_lba(c);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (ata_read_sectors(lba + s, 1, f57_sec) < 0) return false;
            const fat32_dirent_t* e = (const fat32_dirent_t*)f57_sec;
            for (uint32_t i = 0; i < 512 / sizeof(fat32_dirent_t); i++) {
                if (e[i].name[0] == 0x00) return false;
                if ((uint8_t)e[i].name[0] == 0xE5 || e[i].attr == ATTR_LFN || (e[i].attr & ATTR_VOLUME_ID)) continue;
                if (cb(&e[i], lba + s, i * sizeof(fat32_dirent_t), ctx)) return true;
            }
        }
    }
    return false;
}

typedef struct { char want[11]; fat32_file_t* out; } f57_find_t;
static bool f57_find_cb(const fat32_dirent_t* e, uint32_t lba, uint32_t off, void* ctx) {
    f57_find_t* f = (f57_find_t*)ctx;
    if (memcmp(e->name, f->want, 11) != 0) return false;
    f57_fill(f->out, e, lba, off);
    return true;
}

static bool f57_find_in(uint32_t dirc, const char* comp, int clen, fat32_file_t* out) {
    char tmp[FS_NAME_MAX];
    if (clen <= 0 || clen >= FS_NAME_MAX) return false;
    memcpy(tmp, comp, clen); tmp[clen] = 0;
    f57_find_t f; f.out = out;
    string_to_fat_name(tmp, f.want);
    return f57_scan(dirc, f57_find_cb, &f);
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
    char dir[FS_PATH_MAX];
    int dl = (int)(last - path - 1);
    if (dl >= FS_PATH_MAX) return false;
    memcpy(dir, path, dl); dir[dl] = 0;
    return fat32_lookup(dir, parent) && parent->is_dir;
}

bool fat32_lookup(const char* path, fat32_file_t* out) {
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

int32_t fat32_read_file(const fat32_file_t* f, uint32_t off, uint32_t len, void* buf) {
    if (!mounted || !f || !buf) return -1;
    if (f->is_dir) return -1;
    if (off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;
    uint32_t c = f->first, skip = off / bytes_per_cluster, done = 0;
    while (skip-- && f57_valid_cluster(c)) c = fat_read_entry(c);
    uint32_t inoff = off % bytes_per_cluster;
    uint8_t* dst = (uint8_t*)buf;
    while (done < len && f57_valid_cluster(c)) {
        if (ata_read_sectors(cluster_to_lba(c), sectors_per_cluster, f57_clu) < 0) return (int32_t)done;
        uint32_t n = bytes_per_cluster - inoff;
        if (n > len - done) n = len - done;
        memcpy(dst + done, f57_clu + inoff, n);
        done += n; inoff = 0;
        c = fat_read_entry(c);
    }
    return (int32_t)done;
}

typedef struct { uint32_t want, seen; fat32_file_t* out; } f57_idx_t;
static bool f57_idx_cb(const fat32_dirent_t* e, uint32_t lba, uint32_t off, void* ctx) {
    f57_idx_t* x = (f57_idx_t*)ctx;
    if (e->name[0] == '.') return false;             /* skip . and ..          */
    if (x->seen++ != x->want) return false;
    f57_fill(x->out, e, lba, off);
    return true;
}

bool fat32_dir_entry(const fat32_file_t* dir, uint32_t index, fat32_file_t* out) {
    if (!mounted || !dir || !dir->is_dir || !out) return false;
    f57_idx_t x; x.want = index; x.seen = 0; x.out = out;
    return f57_scan(dir->first, f57_idx_cb, &x);
}

/* Write the on-disk entry of f (size / first cluster / time) back. */
static int f57_update_dirent(const fat32_file_t* f) {
    if (f->dir_lba == 0) return 0;                   /* root pseudo entry      */
    if (ata_read_sectors(f->dir_lba, 1, f57_sec) < 0) return -1;
    fat32_dirent_t* e = (fat32_dirent_t*)(f57_sec + f->dir_off);
    e->file_size = f->size;
    e->first_cluster_hi = (uint16_t)(f->first >> 16);
    e->first_cluster_lo = (uint16_t)(f->first & 0xFFFF);
    e->write_date = f->wdate; e->write_time = f->wtime;
    return ata_write_sectors(f->dir_lba, 1, f57_sec) < 0 ? -1 : 0;
}

/* Find (or create by extending the chain) a free 32-byte slot in dir `dirc`. */
static bool f57_free_slot(uint32_t dirc, uint32_t* lba_out, uint32_t* off_out) {
    uint32_t c = dirc, last = dirc;
    for (uint32_t hops = dir_max_clusters(); hops-- && f57_valid_cluster(c); c = fat_read_entry(c)) {
        last = c;
        uint32_t lba = cluster_to_lba(c);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (ata_read_sectors(lba + s, 1, f57_sec) < 0) return false;
            const fat32_dirent_t* e = (const fat32_dirent_t*)f57_sec;
            for (uint32_t i = 0; i < 512 / sizeof(fat32_dirent_t); i++)
                if (e[i].name[0] == 0x00 || (uint8_t)e[i].name[0] == 0xE5) {
                    *lba_out = lba + s; *off_out = i * sizeof(fat32_dirent_t); return true;
                }
        }
    }
    uint32_t nc = fat_alloc_cluster();
    if (!nc) return false;
    fat_write_entry(last, nc);
    memset(f57_sec, 0, 512);
    for (uint32_t s = 0; s < sectors_per_cluster; s++) ata_write_sectors(cluster_to_lba(nc) + s, 1, f57_sec);
    *lba_out = cluster_to_lba(nc); *off_out = 0;
    return true;
}

int fat32_create_path(const char* path, bool is_dir, fat32_file_t* out) {
    if (!mounted) return -1;
    fat32_file_t parent, dummy;
    const char* leaf; int ll;
    if (!f57_parent(path, &parent, &leaf, &ll)) return -3;         /* no parent dir */
    if (ll <= 0 || ll >= FS_NAME_MAX) return -1;
    if (f57_find_in(parent.first, leaf, ll, &dummy)) return -2;    /* exists */
    uint32_t lba, off;
    if (!f57_free_slot(parent.first, &lba, &off)) return -4;       /* disk full */
    uint32_t first = 0;
    if (is_dir) {
        first = fat_alloc_cluster();
        if (!first) return -4;
        memset(f57_clu, 0, sizeof(f57_clu));
        fat32_dirent_t* d = (fat32_dirent_t*)f57_clu;
        memset(d[0].name, ' ', 11); d[0].name[0] = '.'; d[0].attr = ATTR_DIRECTORY;
        d[0].first_cluster_hi = (uint16_t)(first >> 16); d[0].first_cluster_lo = (uint16_t)first;
        memset(d[1].name, ' ', 11); d[1].name[0] = '.'; d[1].name[1] = '.'; d[1].attr = ATTR_DIRECTORY;
        uint32_t pc = (parent.first == root_cluster) ? 0 : parent.first;
        d[1].first_cluster_hi = (uint16_t)(pc >> 16); d[1].first_cluster_lo = (uint16_t)pc;
        for (uint32_t s = 0; s < sectors_per_cluster; s++)
            ata_write_sectors(cluster_to_lba(first) + s, 1, f57_clu + s * 512);
    }
    char tmp[FS_NAME_MAX]; memcpy(tmp, leaf, ll); tmp[ll] = 0;
    if (ata_read_sectors(lba, 1, f57_sec) < 0) return -1;
    fat32_dirent_t* e = (fat32_dirent_t*)(f57_sec + off);
    memset(e, 0, sizeof(*e));
    string_to_fat_name(tmp, e->name);
    e->attr = is_dir ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    e->first_cluster_hi = (uint16_t)(first >> 16); e->first_cluster_lo = (uint16_t)first;
    e->file_size = 0;
    if (ata_write_sectors(lba, 1, f57_sec) < 0) return -1;
    if (out) f57_fill(out, e, lba, off);
    if (parent.first == root_cluster) fat32_read_root_dir();       /* legacy root cache */
    return 0;
}

int fat32_write_file(fat32_file_t* f, uint32_t off, uint32_t len, const void* buf) {
    if (!mounted || !f || f->is_dir) return -1;
    if (len == 0) return 0;
    if (off > f->size) {                             /* zero-fill the gap first */
        static const uint8_t zeros[512];
        while (f->size < off) {
            uint32_t n = off - f->size; if (n > 512) n = 512;
            if (fat32_write_file(f, f->size, n, zeros) < 0) return -1;
        }
    }
    uint32_t c = f->first;
    if (!f57_valid_cluster(c)) {
        c = fat_alloc_cluster();
        if (!c) return -2;
        f->first = c;
    }
    uint32_t want = off / bytes_per_cluster;
    for (uint32_t i = 0; i < want; i++) {
        uint32_t nx = fat_read_entry(c);
        if (!f57_valid_cluster(nx)) { nx = fat_alloc_cluster(); if (!nx) return -2; fat_write_entry(c, nx); }
        c = nx;
    }
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t done = 0, inoff = off % bytes_per_cluster;
    while (done < len) {
        uint32_t lba = cluster_to_lba(c) + inoff / 512;
        uint32_t so = inoff % 512;
        uint32_t n = 512 - so; if (n > len - done) n = len - done;
        if (n == 512) {
            if (ata_write_sectors(lba, 1, src + done) < 0) return -1;
        } else {
            if (ata_read_sectors(lba, 1, f57_sec) < 0) return -1;
            memcpy(f57_sec + so, src + done, n);
            if (ata_write_sectors(lba, 1, f57_sec) < 0) return -1;
        }
        done += n; inoff += n;
        if (inoff >= bytes_per_cluster && done < len) {
            inoff = 0;
            uint32_t nx = fat_read_entry(c);
            if (!f57_valid_cluster(nx)) { nx = fat_alloc_cluster(); if (!nx) break; fat_write_entry(c, nx); }
            c = nx;
        }
    }
    if (off + done > f->size) f->size = off + done;
    f57_update_dirent(f);
    return (int)done;
}

int fat32_truncate_file(fat32_file_t* f, uint32_t newsize) {
    if (!mounted || !f || f->is_dir) return -1;
    if (newsize > f->size) {
        static const uint8_t zeros[512];
        while (f->size < newsize) {
            uint32_t n = newsize - f->size; if (n > 512) n = 512;
            if (fat32_write_file(f, f->size, n, zeros) < 0) return -1;
        }
        return 0;
    }
    uint32_t keep = (newsize + bytes_per_cluster - 1) / bytes_per_cluster;
    uint32_t c = f->first;
    if (keep == 0) {
        for (uint32_t lim = bpb.fat_size_32 * 128; lim-- && f57_valid_cluster(c); ) {
            uint32_t nx = fat_read_entry(c); fat_write_entry(c, FAT32_FREE); c = nx;
        }
        f->first = 0;
    } else {
        for (uint32_t i = 1; i < keep && f57_valid_cluster(c); i++) c = fat_read_entry(c);
        if (f57_valid_cluster(c)) {
            uint32_t nx = fat_read_entry(c);
            fat_write_entry(c, 0x0FFFFFFF);
            c = nx;
            for (uint32_t lim = bpb.fat_size_32 * 128; lim-- && f57_valid_cluster(c); ) {
                uint32_t n2 = fat_read_entry(c); fat_write_entry(c, FAT32_FREE); c = n2;
            }
        }
    }
    f->size = newsize;
    return f57_update_dirent(f);
}

typedef struct { int n; } f57_cnt_t;
static bool f57_cnt_cb(const fat32_dirent_t* e, uint32_t lba, uint32_t off, void* ctx) {
    (void)lba; (void)off;
    if (e->name[0] != '.') ((f57_cnt_t*)ctx)->n++;
    return false;
}

int fat32_remove_path(const char* path) {
    if (!mounted) return -1;
    fat32_file_t f;
    if (!fat32_lookup(path, &f)) return -1;
    if (f.dir_lba == 0) return -2;                   /* root */
    if (f.is_dir) {
        f57_cnt_t cnt; cnt.n = 0;
        f57_scan(f.first, f57_cnt_cb, &cnt);
        if (cnt.n) return -3;                        /* not empty */
    }
    uint32_t c = f.first;
    for (uint32_t lim = bpb.fat_size_32 * 128; lim-- && f57_valid_cluster(c); ) {
        uint32_t nx = fat_read_entry(c); fat_write_entry(c, FAT32_FREE); c = nx;
    }
    if (ata_read_sectors(f.dir_lba, 1, f57_sec) < 0) return -1;
    f57_sec[f.dir_off] = 0xE5;
    if (ata_write_sectors(f.dir_lba, 1, f57_sec) < 0) return -1;
    fat32_read_root_dir();
    return 0;
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
