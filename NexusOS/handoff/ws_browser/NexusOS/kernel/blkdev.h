/* ============================================================================
 * NexusOS — Block Device Layer (Header) — Phase 24
 * ============================================================================
 * Abstraction over raw disk I/O for the storage stack.
 * ============================================================================ */

#ifndef BLKDEV_H
#define BLKDEV_H

#include "types.h"

/* Block device limits */
#define BLKDEV_MAX       4
#define BLKDEV_NAME_MAX  16
#define BLKDEV_SECTOR_SIZE 512

/* Read/write callbacks */
typedef int (*blkdev_read_fn)(uint32_t lba, uint32_t count, uint8_t* buf);
typedef int (*blkdev_write_fn)(uint32_t lba, uint32_t count, const uint8_t* buf);

/* Block device */
typedef struct {
    char     name[BLKDEV_NAME_MAX];
    uint32_t sector_size;
    uint32_t sector_count;    /* Total sectors */
    uint32_t size_mb;         /* Size in MB */
    blkdev_read_fn  read;
    blkdev_write_fn write;
    bool     active;
} blkdev_t;

/* Initialize block device layer */
void blkdev_init(void);

/* Register a block device, returns device index or -1 */
int blkdev_register(const char* name, uint32_t sectors,
                    blkdev_read_fn read_fn, blkdev_write_fn write_fn);

/* Read/write sectors via block device */
int blkdev_read(int dev, uint32_t lba, uint32_t count, uint8_t* buf);
int blkdev_write(int dev, uint32_t lba, uint32_t count, const uint8_t* buf);

/* Lookup */
blkdev_t* blkdev_get(int dev);
int blkdev_count(void);

#endif /* BLKDEV_H */
