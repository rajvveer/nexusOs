/* SCSI block commands over any transport (usbstorage.c: USB Bulk-Only and UAS). scsi.c turns a logical unit
 * into a block device (blockdev.h): INQUIRY (+ unit serial VPD), TEST UNIT READY with UNIT ATTENTION /
 * becoming-ready handling, READ CAPACITY(10)/(16), MODE SENSE(6/10) for write protection and the write cache,
 * READ/WRITE(10)/(16), SYNCHRONIZE CACHE(10), START STOP UNIT, PREVENT ALLOW MEDIUM REMOVAL, UNMAP, and
 * fixed/descriptor sense decoding with a retry policy per sense key. Commands run synchronously under the
 * kernel lock, one at a time per transport (the transport serialises its logical units). */
#ifndef NEXUS_SCSI_H
#define NEXUS_SCSI_H
#include "native.h"

#define SCSI_GOOD 0
#define SCSI_CHECK 1          /* CHECK CONDITION: sense data in the command */
#define SCSI_BUSY 2           /* BUSY / TASK SET FULL: retry later */
#define SCSI_SENSE_MAX 96
struct scsi_command {
    uint8_t cdb[16]; unsigned cdb_length;
    void* data; uint32_t length; bool in;        /* data phase (length 0: none) */
    uint32_t transferred;
    uint8_t sense[SCSI_SENSE_MAX]; unsigned sense_length;
    uint64_t timeout_ns;
};
struct scsi_transport {
    /* One command: SCSI_GOOD/CHECK/BUSY, or -errno for a transport failure (-19 gone, -110 timeout, -5/-71 protocol). */
    int (*execute)(void* context, unsigned lun, struct scsi_command* command);
    int (*reset)(void* context, unsigned lun);   /* transport error recovery: 0 or -errno */
    bool (*gone)(void* context);
    void (*sleep)(void* context, uint64_t ns);   /* wait while letting the transport make progress */
    void (*retain)(void* context);
    void (*release)(void* context);
    uint32_t max_transfer;                       /* bytes per command */
    void (*changed)(void* context);              /* optional: a unit was ejected (the transport acts on it) */
};
struct scsi_sense { uint8_t key, asc, ascq; bool valid, current; };
bool scsi_decode_sense(const uint8_t* sense, unsigned length, struct scsi_sense* out);

struct scsi_disk {
    struct block_device block;                   /* first: dev->driver == the disk */
    const struct scsi_transport* transport; void* context; unsigned lun;
    uint8_t peripheral, version;
    bool removable, write_protect, write_cache, cache_known, use16, no_sync, lbpme, media, offline, prevented;
    char vendor[9], product[17], revision[5], serial[21];
    uint64_t blocks; uint32_t block_size, physical_exponent;
    unsigned resets, failures;
    char kind[8];
    bool ejected;                                /* stopped by CDROMEJECT: the transport detaches it (power-off) */
};
/* Probe the logical unit (INQUIRY .. MODE SENSE) and fill disk->block (ops, size, names). 0, -6 (no unit
 * at this LUN), -123 (no medium: registered later on insertion), -95 (not a direct-access device), -errno. */
int scsi_disk_probe(struct scsi_disk* disk);
int scsi_disk_io(struct scsi_disk* disk, uint64_t lba, uint32_t count, void* buffer, bool write);
int scsi_disk_flush(struct scsi_disk* disk);
int scsi_disk_eject(struct scsi_disk* disk);
int scsi_disk_prevent(struct scsi_disk* disk, bool prevent);
int scsi_disk_ready(struct scsi_disk* disk);    /* TEST UNIT READY: 0 ready, -123 no medium, -errno */
int scsi_report_luns(const struct scsi_transport* transport, void* context, uint8_t* luns, unsigned capacity);
extern const struct block_ops scsi_disk_ops;
#endif
