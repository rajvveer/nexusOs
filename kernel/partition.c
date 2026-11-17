/* ============================================================================
 * NexusOS — Disk Partition (Implementation) — Phase 24
 * ============================================================================
 * Reads MBR partition table and catalogs partitions.
 * ============================================================================ */

#include "partition.h"
#include "blkdev.h"
#include "string.h"
#include "vga.h"

/* MBR signature */
#define MBR_SIGNATURE 0xAA55

/* Partition table */
static partition_t partitions[MAX_PARTITIONS * BLKDEV_MAX];
static int num_partitions = 0;

const char* partition_type_name(uint8_t type) {
    switch (type) {
        case PART_TYPE_EMPTY:  return "Empty";
        case PART_TYPE_FAT12:  return "FAT12";
        case PART_TYPE_FAT16:  return "FAT16";
        case PART_TYPE_FAT32:  return "FAT32";
        case PART_TYPE_FAT32L: return "FAT32-LBA";
        case PART_TYPE_NTFS:   return "NTFS";
        case PART_TYPE_LINUX:  return "Linux";
        case PART_TYPE_SWAP:   return "Swap";
        case PART_TYPE_EXT:    return "Extended";
        default:               return "Unknown";
    }
}

/* --------------------------------------------------------------------------
 * partition_init: Read MBR from each block device and parse partitions
 * -------------------------------------------------------------------------- */
void partition_init(void) {
    num_partitions = 0;
    uint8_t mbr[512];

    int dev_count = blkdev_count();
    for (int d = 0; d < dev_count; d++) {
        blkdev_t* dev = blkdev_get(d);
        if (!dev) continue;

        /* Read MBR (sector 0) */
        if (blkdev_read(d, 0, 1, mbr) != 0) continue;

        /* Check MBR signature */
        uint16_t sig = *(uint16_t*)(mbr + 510);
        if (sig != MBR_SIGNATURE) continue;

        /* Parse 4 partition entries at offset 0x1BE */
        mbr_partition_entry_t* entries = (mbr_partition_entry_t*)(mbr + 0x1BE);

        for (int p = 0; p < 4; p++) {
            if (entries[p].type == PART_TYPE_EMPTY) continue;
            if (entries[p].sector_count == 0) continue;

            partition_t* part = &partitions[num_partitions];
            part->type = entries[p].type;
            part->lba_start = entries[p].lba_start;
            part->sector_count = entries[p].sector_count;
            part->size_mb = (entries[p].sector_count * 512) / (1024 * 1024);
            part->bootable = (entries[p].boot_flag == 0x80);
            part->blkdev = d;
            part->active = true;
            num_partitions++;
        }
    }

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Partitions: ");
    char buf[8];
    int_to_str(num_partitions, buf);
    vga_print(buf);
    vga_print(" found\n");

    /* List partitions */
    for (int i = 0; i < num_partitions; i++) {
        partition_t* p = &partitions[i];
        vga_print("     ");
        blkdev_t* dev = blkdev_get(p->blkdev);
        if (dev) { vga_print(dev->name); vga_print("p"); }
        char idx[4];
        int_to_str(i + 1, idx);
        vga_print(idx);
        vga_print(": ");
        vga_print(partition_type_name(p->type));
        vga_print(" ");
        int_to_str(p->size_mb, buf);
        vga_print(buf);
        vga_print("MB");
        if (p->bootable) vga_print(" *");
        vga_print("\n");
    }
}

partition_t* partition_get(int index) {
    if (index < 0 || index >= num_partitions) return NULL;
    return partitions[index].active ? &partitions[index] : NULL;
}

int partition_count(void) { return num_partitions; }
