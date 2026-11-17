#ifndef NEXUS_BLOCKDEV_H
#define NEXUS_BLOCKDEV_H
/* ---- Block devices, partitions and the root volume (blockdev.c, partition.c, ahci.c, nvme.c) ----
 * Drivers: IDE (kernel/ata.c), AHCI SATA ports (ahci.c), NVMe namespaces (nvme.c). All optional;
 * block_probe() finds them at boot, parses MBR/GPT partition tables and selects the root volume.
 *
 * Units: block_read/block_write address the device in its own logical blocks (dev->block_size
 * bytes, 512 or 4096). block_part_read/block_part_write address a partition in 512-byte sectors
 * relative to its start on any device (unaligned 4Kn access reads the covering blocks with one
 * command; writes read only the partial head/tail blocks, then write the range once).
 * Buffers: any kernel address and alignment. Identity-mapped RAM is used for DMA directly; other
 * buffers (user virtual addresses, odd alignment) go through a bounce buffer.
 * Calls are synchronous and run under the big kernel lock; they return 0 or -1 (I/O error).
 * Completion is interrupt driven on the bootstrap CPU (sti;hlt until the device IRQ, with the
 * 1 kHz tick as a fallback) and polled, ~4 us apart, on application processors (see block_wait).
 * Writes may sit in the device's volatile cache: block_flush (or block_flush_all from fsync)
 * issues FLUSH CACHE (EXT) on SATA/IDE and Flush on NVMe when writes are pending.
 * Other filesystems (ext4) find their volume in the partition list (type_guid / mbr_type).
 *
 * Root volume (block_root), first match in this order, disks in probe order (IDE, AHCI by
 * controller/port, NVMe by controller/namespace), each candidate must hold a FAT32 boot sector:
 *   1. GPT partition with the NexusOS root type GUID BLOCK_GUID_NEXUS_ROOT
 *   2. GPT Microsoft basic-data partition (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7)
 *   3. MBR partition of type 0x0B/0x0C (FAT32), primary or logical
 *   4. a whole disk without a partition table whose sector 0 is a FAT32 boot sector
 * The EFI system partition is never selected. */
#include "../../kernel/types.h"
#include "../../kernel/pci.h"
#include "../../kernel/pci.h"

/* 7D1B4F2C-3A86-4E59-9C0D-4E45585553F1 ("NEXUS" in the last bytes), on-disk byte order. */
#define BLOCK_GUID_NEXUS_ROOT {0x2C,0x4F,0x1B,0x7D,0x86,0x3A,0x59,0x4E,0x9C,0x0D,0x4E,0x45,0x58,0x55,0x53,0xF1}
#define BLOCK_GUID_BASIC_DATA {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7}
#define BLOCK_GUID_EFI_SYSTEM {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B}
#define BLOCK_GUID_LINUX_DATA {0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4}

struct block_device;
struct block_ops {
    int (*read)(struct block_device* dev, uint64_t lba, uint32_t count, void* buffer);
    int (*write)(struct block_device* dev, uint64_t lba, uint32_t count, const void* buffer);
    int (*flush)(struct block_device* dev);
};
/* Same first fields as the old ata_stats; sectors are 512-byte units, busy_ticks are TSC ticks. */
struct block_stats { uint64_t read_cmds, read_sectors, write_cmds, write_sectors, flushes, busy_ticks, irqs, errors, resets; };
struct block_device {
    char name[16];            /* "ide0", "sata0", "nvme0n1" */
    char model[41];
    const char* kind;         /* "IDE", "AHCI", "NVMe" */
    uint32_t block_size;      /* logical block size in bytes (512 or 4096) */
    uint64_t blocks;          /* capacity in logical blocks */
    uint32_t max_blocks;      /* largest single device command the driver issues */
    bool flush_pending;       /* writes completed since the last successful flush */
    struct block_stats stats; /* updated by the driver */
    const struct block_ops* ops;
    void* driver;
    unsigned irq;             /* PIC line (0 = polled only) */
    /* Linux view, hot-plug and lifetime (blockdev.c block_hot_*, blkdev_node.c). block_register fills the name
     * and number of boot disks too: SATA/IDE/USB/SCSI -> sdX (8:16n), NVMe -> nvme0n1 (259:n), loop/dm by driver. */
    char linux_name[16];
    uint32_t major, minor;
    uint32_t physical_block_size;   /* 0: = block_size */
    bool removable, read_only, removed, hotplug, rotational;
    unsigned references;            /* block_get/block_put; the driver holds one until block_hot_remove */
    unsigned holders;               /* whole-disk opens (exclusive checks, BLKRRPART) */
    char vendor[9], serial[21];
    const struct block_extra_ops* extra;  /* optional: eject, discard, partition rescans, ioctls */
    uint64_t discard_granularity;   /* bytes, 0: no discard */
    unsigned generation;            /* bumped on media change / rescan */
};
struct block_extra_ops {
    int (*eject)(struct block_device* dev);                      /* stop + eject (SCSI START STOP UNIT), 0 or -errno */
    int (*discard)(struct block_device* dev, uint64_t lba, uint64_t count);
    void (*release)(struct block_device* dev);                   /* references reached 0 after removal */
    int (*ioctl)(struct block_device* dev, uint64_t command, uint64_t argument); /* -25: not handled */
    void (*idle)(struct block_device* dev);                      /* only the driver's reference is left (loop autoclear) */
};
struct block_partition {
    struct block_device* dev;
    unsigned number;          /* 1-based table index (MBR logicals from 5), 0 = whole disk */
    uint8_t scheme;           /* BLOCK_SCHEME_* */
    uint8_t mbr_type;         /* MBR system id, 0 for GPT */
    uint64_t start, blocks;   /* in device logical blocks */
    uint8_t type_guid[16], unique_guid[16];
    uint64_t attributes;      /* GPT attribute bits */
    char name[37];            /* GPT partition name (ASCII), "" for MBR */
    char label[20];           /* "nvme0n1p2" */
    char linux_name[20];      /* "sda1", "nvme0n1p2"; whole disk: the disk's name */
    uint32_t major, minor;
    unsigned holders;         /* mounts, exclusive opens, loop/dm users of this partition */
    unsigned opens;           /* open /dev nodes (BLKRRPART/BLKPG refuse to drop an open partition) */
    bool listed;              /* in the global partition table (block_partition_get) */
    bool allocated;           /* arena_alloc'd (hot-plug, rescans); boot partitions live in the boot array */
};
#define BLOCK_SCHEME_WHOLE 0
#define BLOCK_SCHEME_MBR 1
#define BLOCK_SCHEME_GPT 2

bool block_probe(uint64_t tsc_hz);                 /* boot: find disks, partitions and the root */
unsigned block_device_count(void);
struct block_device* block_device_get(unsigned index);
unsigned block_partition_count(void);
const struct block_partition* block_partition_get(unsigned index);
const struct block_partition* block_root(void);    /* selected FAT32 root volume, NULL if none */
extern const struct block_partition* block_unlocked_root;   /* cryptroot.c: the opened LUKS root, tried first as root */
int block_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buffer);
int block_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buffer);
int block_flush(struct block_device* dev);
int block_flush_all(void);                         /* every device with pending writes */
int block_part_read(const struct block_partition* part, uint64_t sector, uint32_t count, void* buffer);
int block_part_write(const struct block_partition* part, uint64_t sector, uint32_t count, const void* buffer);
uint64_t block_part_sectors(const struct block_partition* part); /* size in 512-byte sectors */
void block_get_stats(struct block_stats* total);   /* sum over all devices */
void block_report(void);                           /* "[64] BLOCKSTAT ..." per device */

/* Driver interface (ahci.c, nvme.c). */
int block_register(struct block_device* dev);
/* Platform glue, one function each (swap for irq_attach_isa/irq_register + irq_wait_restricted + mmio_map):
 * block_pci_irq: MSI, MSI-X or PCI INTx (irq.c) to its handler; returns a token (0 = none: poll);
 * block_pci_bar: uncached kernel mapping of memory BAR `index` (32- or 64-bit), NULL if unusable;
 * block_wait:    one wait step inside a completion loop (line 0: paced polling). */
unsigned block_pci_irq(pci_device_t* pci, void (*handler)(void* context), void* context);
unsigned block_pci_irq_msix(pci_device_t* pci, void (*handler)(void* context), void* context);   /* MSI-X, MSI, INTx */
void block_irq_detach(pci_device_t* pci, unsigned irq, void* context);
volatile void* block_pci_bar(pci_device_t* pci, unsigned index, uint64_t bytes);
void block_wait(unsigned line);
bool block_irq_attach(unsigned line, void (*handler)(void* context), void* context); /* ISA/INTx line (irq.c) */
volatile void* block_mmio(uint64_t physical, uint64_t bytes); /* uncached mapping (identity, or the MMIO window above 4 GiB) */
bool block_dma_direct(const void* buffer, uint64_t bytes, uint64_t limit); /* kernel RAM ending below limit: DMA-able as is */
uint8_t* block_bounce(uint64_t* bytes);            /* shared DMA32 bounce buffer (BKL serialises use) */
uint64_t block_ticks(void);                        /* TSC */
uint64_t block_tsc_hz(void);
void block_name_number(char* name, const char* prefix, unsigned number, const char* suffix);
bool ahci_probe_all(void);
bool nvme_probe_all(void);
void nvme_shutdown_all(void);   /* normal shutdown notification (CC.SHN) after the final flush */
unsigned partition_scan(struct block_device* dev, struct block_partition* out, unsigned capacity, uint8_t* scratch, uint64_t scratch_bytes);
uint32_t block_crc32(const void* data, uint64_t length);

/* Hot-plug devices (USB storage, loop, device mapper), references and Linux names (blockdev.c).
 * block_hot_add: register a device at run time (Linux name/number, partitions scanned, /dev + /sys + uevent
 * "add"); block_hot_remove: the device left (I/O fails with ENODEV, partitions unlisted, uevent "remove"), its
 * structures live until the last block_put (extra->release). block_rescan: re-read the partition table (EBUSY
 * while a partition is held). All under the kernel lock. */
int  block_hot_add(struct block_device* dev);
void block_hot_remove(struct block_device* dev);
int  block_rescan(struct block_device* dev);
void block_get(struct block_device* dev);
void block_put(struct block_device* dev);
struct block_device* block_find(const char* linux_name);           /* whole disk by Linux name */
struct block_partition* block_find_partition(const char* linux_name); /* partition or whole disk (number 0) */
struct block_partition* block_whole(struct block_device* dev);    /* the number-0 view of a disk */
unsigned block_device_partitions(struct block_device* dev, struct block_partition** out, unsigned capacity);
bool block_root_late(uint64_t wait_ns);                           /* boot: root on a late (USB) disk */
uint32_t block_sd_index(const struct block_device* dev);
void block_assign_name(struct block_device* dev, const char* prefix, unsigned number, uint32_t major); /* loop/dm: "loop"+n (7:n) */
extern void (*block_event_hook)(struct block_device* dev, struct block_partition* part, const char* action); /* uevents */

/* Storage device nodes (blkdev_node.c): /dev/sdX[N], /dev/nvmeXnY[pN], /dev/loopN, /dev/dm-N, /dev/mapper/{control,
 * NAME}, /dev/loop-control, /dev/fuse, /dev/disk/by-{uuid,label,partuuid}; FD_DEVICE numbers from DEVICE_STORAGE
 * (device.c and kernel.c delegate every operation in that range). */
#define DEVICE_STORAGE 0x2000000U
struct input_reader;
struct linux_stat;
struct directory_item;
int  storage_lookup(const char* path);
int  storage_open_check(unsigned device, uint64_t flags);
void storage_open(unsigned device, uint64_t flags, struct input_reader* reader);
void storage_close(unsigned device, uint64_t flags, struct input_reader* reader);
int64_t storage_io(unsigned device, uint64_t buffer, uint64_t count, bool write, uint64_t* position, uint64_t flags, struct input_reader* reader);
int64_t storage_seek(unsigned device, uint64_t* position, int64_t offset, uint64_t whence);
int  storage_ioctl(unsigned device, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags);
void storage_stat(unsigned device, struct linux_stat* stat);
int  storage_directory(unsigned device, uint64_t position, struct directory_item* item);
int  storage_root_entry(uint64_t index, struct directory_item* item);   /* extra children of /dev */
int  storage_fsync(unsigned device);
int  storage_readlink(unsigned device, char* target, unsigned capacity); /* /dev/disk/by-* links */
uint32_t storage_ready(unsigned device, const struct input_reader* reader);
void* storage_event_source(unsigned device, const struct input_reader* reader);
bool storage_pollable(unsigned device);
struct block_partition* storage_partition(unsigned device);  /* block node -> partition view (mount sources) */
/* Character-node drivers behind storage numbers (loop.c, dm.c, fuse.c): */
struct storage_char_ops {
    int (*open)(uint64_t flags, struct input_reader* reader);
    void (*close)(struct input_reader* reader);
    int64_t (*io)(struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags);
    int (*ioctl)(struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags);
    uint32_t (*ready)(const struct input_reader* reader);
    void* (*source)(const struct input_reader* reader);
    int64_t (*write_message)(struct input_reader* reader, const uint8_t* data, uint64_t count);   /* optional: writev as one message */
};
struct iovec64;
bool storage_message_device(unsigned device);   /* its char driver takes a writev as one message (/dev/fuse) */
int64_t storage_write_vectors(unsigned device, const struct iovec64* vectors, unsigned count, uint64_t total, struct input_reader* reader);
extern const struct storage_char_ops* storage_loop_control_ops;
extern const struct storage_char_ops* storage_mapper_control_ops;
extern const struct storage_char_ops* storage_fuse_ops;
int  storage_node_alias(struct block_device* dev, const char* name);  /* "mapper/NAME" for dm devices */
void storage_node_unalias(struct block_device* dev);
void storage_uevent(const char* action, const char* devpath, const char* const* pairs, unsigned count); /* other subsystems */
uint64_t storage_linux_dev(uint32_t major, uint32_t minor);         /* new_encode_dev for st_rdev */
void netlink_uevent(const char* data, unsigned size);               /* netlink.c: broadcast to group 1 */
bool blk_sysfs_path(const char* path);
int  blk_sysfs_resolve(const char* path, bool follow, uint32_t* node);
#define BLK_SYSFS_BASE 0x40000000U
/* Disk waits without the big lock (blockio.c): a task in a completion wait sleeps and owns the block/filesystem path
 * until it next releases the big lock outside a disk wait; others wait at block_io_barrier() before filesystem or
 * block work, contexts without a task skip it while block_io_defer() says so. */
bool block_io_sleep(void);          /* block_wait/ATA wait step in task context: true = slept until an IRQ or a tick */
void block_io_completed(void);      /* storage interrupt: wake the sleeper */
void block_io_barrier(void);        /* task with the big lock: wait until no other task owns the I/O path */
bool block_io_busy(void);           /* another task owns the I/O path */
bool block_io_owner_self(void);     /* this CPU's task owns it (smp.c: it gets the big lock first) */
void block_io_request_start(void);  /* blockdev.c: a request starts (a page fault spins its first millisecond) */
void block_io_fault(bool on);       /* kernel.c: the running task enters/leaves a page fault */
bool block_io_defer(void);          /* busy, counted: a task-less context must not start filesystem/block work now */
void block_io_unlocking(void);      /* kernel_lock_release: ends the caller's ownership outside a disk wait */
struct registers;
bool syscall_io_free(const struct registers* r); /* the call never touches a filesystem, the page cache or a disk */
void block_io_stats(uint64_t out[3]);            /* disk-wait sleeps, barrier waits, deferred task-less work */
#endif
