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

/* Optional calibrated clock, advancing even with interrupts disabled. */
void ata_set_clock(uint64_t (*read_ticks)(void), uint64_t ticks_per_second);

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

#ifdef NEXUS_NATIVE64
/* Optional wait primitive while a command is in flight (e.g. sti;hlt for IRQ14). */
void ata_set_wait_hook(void (*hook)(void));
/* 64 KiB DMA bounce buffer below 4 GiB; without one the driver uses PIO. */
void ata_set_dma_buffer(void* buffer);
/* Kernel pointer -> physical address for the bus master (the native kernel is not identity-mapped);
 * without a translator pointers are used as physical addresses. */
void ata_set_dma_translate(uint64_t (*translate)(const volatile void* pointer));
/* Writes are not individually flushed: callers issue a barrier when durability matters. */
int ata_flush(void);
struct ata_stats { uint64_t read_cmds, read_sectors, write_cmds, write_sectors, flushes, busy_ticks; };
void ata_get_stats(struct ata_stats* out);
#endif
#endif /* ATA_H */
