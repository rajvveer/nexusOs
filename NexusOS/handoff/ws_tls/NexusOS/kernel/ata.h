/* ============================================================================
 * NexusOS — ATA PIO Driver (Header) — Phase 19
 * ============================================================================
 * Simple ATA PIO mode driver for reading/writing disk sectors.
 * Uses I/O ports 0x1F0-0x1F7 (primary bus, master drive).
 * ============================================================================ */

#ifndef ATA_H
#define ATA_H

#include "types.h"

/* Sector size */
#define ATA_SECTOR_SIZE 512

/* Initialize ATA driver, detect primary master disk.
 * Returns true if a disk was found. */
bool ata_init(void);

/* Read 'count' sectors starting at LBA into buffer.
 * Returns 0 on success, -1 on error. */
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer);

/* Write 'count' sectors starting at LBA from buffer.
 * Returns 0 on success, -1 on error. */
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer);

/* Check if disk is present */
bool ata_disk_present(void);

/* Get disk size in 512-byte sectors (exact for any LBA28 disk) */
uint32_t ata_get_sectors(void);

/* Get disk size in bytes — saturates at 0xFFFFFFFF for disks >= 4 GiB, so
 * prefer ata_get_sectors() for geometry math. */
uint32_t ata_get_size(void);

/* Alias for block device layer */
#define ata_is_present ata_disk_present

#endif /* ATA_H */
