/* ============================================================================
 * NexusOS — Block Device Layer (Implementation) — Phase 24
 * ============================================================================ */

#include "blkdev.h"
#include "ata.h"
#include "string.h"
#include "vga.h"

static blkdev_t devices[BLKDEV_MAX];
static int num_devices = 0;

/* ATA read/write wrappers for block device interface */
static int ata_blk_read(uint32_t lba, uint32_t count, uint8_t* buf) {
    return ata_read_sectors(lba, count, buf);
}

static int ata_blk_write(uint32_t lba, uint32_t count, const uint8_t* buf) {
    return ata_write_sectors(lba, count, (uint8_t*)buf);
}

void blkdev_init(void) {
    memset(devices, 0, sizeof(devices));
    num_devices = 0;

    /* Auto-register ATA disk if present */
    if (ata_is_present()) {
        uint32_t sectors = ata_get_size() / BLKDEV_SECTOR_SIZE;
        blkdev_register("hda", sectors, ata_blk_read, ata_blk_write);
    }

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Block devices: ");
    char buf[8];
    int_to_str(num_devices, buf);
    vga_print(buf);
    vga_print(" registered\n");
}

int blkdev_register(const char* name, uint32_t sectors,
                    blkdev_read_fn read_fn, blkdev_write_fn write_fn) {
    if (num_devices >= BLKDEV_MAX) return -1;

    blkdev_t* d = &devices[num_devices];
    strncpy(d->name, name, BLKDEV_NAME_MAX - 1);
    d->sector_size = BLKDEV_SECTOR_SIZE;
    d->sector_count = sectors;
    d->size_mb = (sectors * BLKDEV_SECTOR_SIZE) / (1024 * 1024);
    d->read = read_fn;
    d->write = write_fn;
    d->active = true;

    return num_devices++;
}

int blkdev_read(int dev, uint32_t lba, uint32_t count, uint8_t* buf) {
    if (dev < 0 || dev >= num_devices || !devices[dev].active) return -1;
    if (!devices[dev].read) return -1;
    return devices[dev].read(lba, count, buf);
}

int blkdev_write(int dev, uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (dev < 0 || dev >= num_devices || !devices[dev].active) return -1;
    if (!devices[dev].write) return -1;
    return devices[dev].write(lba, count, buf);
}

blkdev_t* blkdev_get(int dev) {
    if (dev < 0 || dev >= num_devices) return NULL;
    return devices[dev].active ? &devices[dev] : NULL;
}

int blkdev_count(void) { return num_devices; }
