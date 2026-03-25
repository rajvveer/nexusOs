/* ============================================================================
 * NexusOS — USB Mass Storage (Header) — Phase 25
 * ============================================================================
 * USB mass storage driver for flash drives (bulk-only transport).
 * ============================================================================ */

#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include "types.h"
#include "usb.h"

/* USB mass storage subclass */
#define USB_MSC_SUBCLASS_SCSI  0x06

/* USB mass storage protocol */
#define USB_MSC_PROTO_BBB      0x50  /* Bulk-Only (BBB) */

/* SCSI commands */
#define SCSI_TEST_UNIT_READY   0x00
#define SCSI_INQUIRY           0x12
#define SCSI_READ_CAPACITY     0x25
#define SCSI_READ_10           0x28
#define SCSI_WRITE_10          0x2A

/* Command Block Wrapper (31 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;      /* 0x43425355 "USBC" */
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;         /* 0x80 = data-in */
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];          /* SCSI command block */
} usb_cbw_t;

/* Command Status Wrapper (13 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;      /* 0x53425355 "USBS" */
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;         /* 0=pass, 1=fail, 2=phase error */
} usb_csw_t;

/* USB storage device info */
typedef struct {
    uint32_t block_size;
    uint32_t block_count;
    uint32_t capacity_mb;
    bool     ready;
    int      usb_dev_index;     /* Index into USB device table */
} usb_storage_t;

/* Initialize USB mass storage driver */
void usb_storage_init(void);

/* Check if USB storage is present */
bool usb_storage_present(void);

/* Get USB storage device info */
usb_storage_t* usb_storage_get(void);

/* Read/write blocks (stub — needs full USB transfer implementation) */
int usb_storage_read(uint32_t lba, uint32_t count, uint8_t* buf);
int usb_storage_write(uint32_t lba, uint32_t count, const uint8_t* buf);

#endif /* USB_STORAGE_H */
