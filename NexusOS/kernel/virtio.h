/* ============================================================================
 * NexusOS — VirtIO PCI Transport (Header) — Phase 41
 * ============================================================================
 * Modern (VirtIO 1.0) PCI transport: locates the virtio vendor capabilities
 * in PCI config space, maps the MMIO config windows, negotiates features and
 * runs split virtqueues in polled (interrupt-free) mode. Device drivers
 * (e.g. the VirtIO-GPU in gpu.c) sit on top of this.
 * ============================================================================ */

#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"
#include "pci.h"

/* VirtIO PCI vendor IDs: modern device IDs are 0x1040 + device type */
#define VIRTIO_VENDOR        0x1AF4
#define VIRTIO_DEV_GPU       0x1050   /* type 16 (virtio-gpu / virtio-vga) */

/* Vendor-specific capability config types */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

/* Device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FEATURES_OK  0x08
#define VIRTIO_STATUS_FAILED       0x80

/* Feature bits (dword 1 = bits 32..63) */
#define VIRTIO_F_VERSION_1_HI      0x00000001  /* bit 32: modern device */

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

/* --------------------------------------------------------------------------
 * MMIO register layouts (little-endian, packed). 64-bit fields are split
 * into lo/hi dwords — the kernel links without libgcc, so no 64-bit math.
 * -------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;   /* 0x00 RW */
    uint32_t device_feature;          /* 0x04 RO */
    uint32_t driver_feature_select;   /* 0x08 RW */
    uint32_t driver_feature;          /* 0x0C RW */
    uint16_t msix_config;             /* 0x10 */
    uint16_t num_queues;              /* 0x12 RO */
    uint8_t  device_status;           /* 0x14 RW */
    uint8_t  config_generation;       /* 0x15 RO */
    uint16_t queue_select;            /* 0x16 RW */
    uint16_t queue_size;              /* 0x18 RW */
    uint16_t queue_msix_vector;       /* 0x1A */
    uint16_t queue_enable;            /* 0x1C RW */
    uint16_t queue_notify_off;        /* 0x1E RO */
    uint32_t queue_desc_lo,   queue_desc_hi;    /* 0x20 */
    uint32_t queue_driver_lo, queue_driver_hi;  /* 0x28 avail ring */
    uint32_t queue_device_lo, queue_device_hi;  /* 0x30 used ring  */
} virtio_common_cfg_t;

typedef struct __attribute__((packed)) {
    uint32_t addr_lo, addr_hi;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];                  /* [queue size] */
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];         /* [queue size] */
} virtq_used_t;

/* --------------------------------------------------------------------------
 * Driver-side state
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t                index;       /* queue number               */
    uint16_t                size;        /* negotiated entry count     */
    virtq_desc_t*           desc;
    volatile virtq_avail_t* avail;
    volatile virtq_used_t*  used;
    uint16_t                last_used;   /* our used-ring read cursor  */
    volatile uint16_t*      notify;      /* doorbell for this queue    */
} virtq_t;

typedef struct {
    bool                          present;
    pci_device_t*                 pci;
    volatile virtio_common_cfg_t* common;
    volatile uint8_t*             isr;
    volatile uint8_t*             device_cfg;
    uint8_t*                      notify_base;
    uint32_t                      notify_off_multiplier;
    uint32_t                      dev_features_lo, dev_features_hi;  /* offered    */
    uint32_t                      features_lo,     features_hi;      /* negotiated */
} virtio_dev_t;

/* Map the vendor capabilities of a virtio PCI function (enables MMIO + bus
 * mastering). Returns false if the modern config windows are missing. */
bool virtio_pci_setup(virtio_dev_t* vd, pci_device_t* pci);

/* Reset, ACKNOWLEDGE/DRIVER, negotiate (want & offered) features, FEATURES_OK.
 * VERSION_1 is required and added automatically. */
bool virtio_negotiate(virtio_dev_t* vd, uint32_t want_lo, uint32_t want_hi);

/* Allocate and register split-ring storage for queue `index` (entry count
 * clamped to `max_size`). Call between negotiate and driver_ok. */
bool virtio_queue_init(virtio_dev_t* vd, virtq_t* q, uint16_t index, uint16_t max_size);

/* Final handshake step: tell the device the driver is ready. */
void virtio_driver_ok(virtio_dev_t* vd);

/* Synchronous request: chain one driver→device buffer and (optionally) one
 * device→driver buffer, kick the doorbell and poll the used ring to
 * completion (tick timeout + spin cap — never hangs). Both buffers must be
 * identity-mapped (kmalloc/static memory <16MB). Returns bytes the device
 * wrote into `in`, or -1 on timeout. */
int virtio_run(virtio_dev_t* vd, virtq_t* q,
               const void* out, uint32_t out_len, void* in, uint32_t in_len);

#endif /* VIRTIO_H */
