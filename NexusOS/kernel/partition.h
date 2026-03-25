/* ============================================================================
 * NexusOS — Disk Partition (Header) — Phase 24
 * ============================================================================
 * MBR partition table parsing.
 * ============================================================================ */

#ifndef PARTITION_H
#define PARTITION_H

#include "types.h"

/* Partition limits */
#define MAX_PARTITIONS 4

/* MBR partition entry (16 bytes each, at offset 0x1BE in sector 0) */
typedef struct __attribute__((packed)) {
    uint8_t  boot_flag;     /* 0x80 = active/bootable */
    uint8_t  chs_start[3];
    uint8_t  type;          /* Partition type (0x0B/0x0C = FAT32) */
    uint8_t  chs_end[3];
    uint32_t lba_start;     /* Starting LBA */
    uint32_t sector_count;  /* Number of sectors */
} mbr_partition_entry_t;

/* Parsed partition info */
typedef struct {
    uint8_t  type;
    uint32_t lba_start;
    uint32_t sector_count;
    uint32_t size_mb;
    bool     bootable;
    bool     active;
    int      blkdev;        /* Parent block device index */
} partition_t;

/* Common partition types */
#define PART_TYPE_EMPTY   0x00
#define PART_TYPE_FAT12   0x01
#define PART_TYPE_FAT16   0x06
#define PART_TYPE_FAT32   0x0B
#define PART_TYPE_FAT32L  0x0C
#define PART_TYPE_NTFS    0x07
#define PART_TYPE_LINUX   0x83
#define PART_TYPE_SWAP    0x82
#define PART_TYPE_EXT     0x05

/* Initialize and scan partitions on all block devices */
void partition_init(void);

/* Get partition info */
partition_t* partition_get(int index);
int partition_count(void);

/* Get partition type name */
const char* partition_type_name(uint8_t type);

#endif /* PARTITION_H */
