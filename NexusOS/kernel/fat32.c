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
 * Helper: Read a FAT entry for a given cluster
 * -------------------------------------------------------------------------- */
static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    /* Check if this sector is cached */
    if (fat_sector < fat_cache_start ||
        fat_sector >= fat_cache_start + FAT_CACHE_SECTORS) {
        /* Load FAT sectors into cache */
        fat_cache_start = fat_sector;
        ata_read_sectors(fat_cache_start, FAT_CACHE_SECTORS, fat_cache);
    }

    uint32_t cached_offset = (fat_sector - fat_cache_start) * 512 + entry_offset;
    uint32_t val = *(uint32_t*)&fat_cache[cached_offset];
    return val & 0x0FFFFFFF;
}

/* --------------------------------------------------------------------------
 * Helper: Write a FAT entry
 * -------------------------------------------------------------------------- */
static void fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    /* Read sector */
    ata_read_sectors(fat_sector, 1, sector_buf);

    /* Modify entry (preserve top 4 bits) */
    uint32_t* entry = (uint32_t*)&sector_buf[entry_offset];
    *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);

    /* Write back */
    ata_write_sectors(fat_sector, 1, sector_buf);

    /* Invalidate FAT cache */
    fat_cache_start = 0xFFFFFFFF;
}

/* --------------------------------------------------------------------------
 * Helper: Find a free cluster in the FAT
 * -------------------------------------------------------------------------- */
static uint32_t fat_alloc_cluster(void) {
    uint32_t total = bpb.total_sectors_32 / sectors_per_cluster;
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
    if (offset + size > max_write) size = max_write - offset;

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

    while (cluster >= 2 && cluster < FAT32_EOC && dir_node_count < MAX_DIR_ENTRIES) {
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

    /* Read first sector (BPB) */
    if (ata_read_sectors(0, 1, (void*)&bpb) < 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("FAT32: cannot read BPB\n");
        return false;
    }

    /* Validate FAT32 signature */
    if (bpb.bytes_per_sector != 512) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("FAT32: invalid sector size\n");
        return false;
    }

    if (bpb.fat_size_32 == 0) {
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
    while (cluster >= 2 && cluster < FAT32_EOC) {
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
    while (cluster >= 2 && cluster < FAT32_EOC) {
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
                    while (fc >= 2 && fc < FAT32_EOC) {
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
