/* ============================================================================
 * NexusOS — USB Mass Storage (Implementation) — Phase 25 / 54 / 55
 * ============================================================================
 * USB Bulk-Only Transport (BBB) over any host controller through the USB
 * core's transfer API. Finds a mass-storage device, probes it with SCSI
 * TEST UNIT READY / READ CAPACITY, registers it as block device "usb0", and
 * implements block reads/writes via SCSI READ(10)/WRITE(10) wrapped in
 * CBW/CSW. Phase 55: hot-plug aware — usb_storage_rescan() rebinds when the
 * stick is unplugged / re-plugged (the blkdev entry stays registered and
 * reports errors while no device is present); a stalled bulk endpoint is
 * recovered with CLEAR_FEATURE(ENDPOINT_HALT).
 * ============================================================================ */

#include "usb_storage.h"
#include "usb.h"
#include "vga.h"
#include "string.h"
#include "blkdev.h"

static usb_storage_t storage;
static bool storage_present = false;

static usb_device_t*   msc_dev = 0;
static uint32_t        msc_gen = 0;
static usb_endpoint_t* ep_in = 0;
static usb_endpoint_t* ep_out = 0;
static uint32_t        cbw_tag = 1;
static bool            blk_registered = false;

/* Scratch DMA buffers (identity-mapped .bss is fine). */
static usb_cbw_t cbw;
static usb_csw_t csw;

static bool msc_alive(void) {
    return msc_dev && msc_dev->active && msc_dev->gen == msc_gen && storage.ready;
}

/* CLEAR_FEATURE(ENDPOINT_HALT) on a bulk endpoint + reset its data toggle. */
static void msc_clear_halt(usb_endpoint_t* ep) {
    usb_control(msc_dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                USB_REQ_CLEAR_FEATURE, 0 /*ENDPOINT_HALT*/, ep->address, 0, 0);
    ep->toggle = 0;
}

/* --------------------------------------------------------------------------
 * msc_transfer: run one Bulk-Only command (CBW -> optional data -> CSW).
 * data_in != 0 means the data stage flows device->host. Returns 0 on CSW pass.
 * -------------------------------------------------------------------------- */
static int msc_transfer(const uint8_t* cb, int cb_len, uint8_t* data,
                        uint32_t data_len, int data_in) {
    if (!msc_alive()) return -1;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = 0x43425355;        /* "USBC" */
    cbw.dCBWTag = cbw_tag++;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = data_in ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = (uint8_t)cb_len;
    memcpy(cbw.CBWCB, cb, cb_len);

    if (usb_bulk(msc_dev, ep_out, &cbw, sizeof(cbw), 0) < 0) return -1;

    if (data_len > 0 && data) {
        int n = usb_bulk(msc_dev, data_in ? ep_in : ep_out, data, (int)data_len, data_in);
        if (n < 0) msc_clear_halt(data_in ? ep_in : ep_out);   /* stall: recover, still read CSW */
    }

    memset(&csw, 0, sizeof(csw));
    int n = usb_bulk(msc_dev, ep_in, &csw, sizeof(csw), 1);
    if (n < 0) {
        msc_clear_halt(ep_in);
        memset(&csw, 0, sizeof(csw));
        n = usb_bulk(msc_dev, ep_in, &csw, sizeof(csw), 1);
        if (n < 0) return -1;
    }
    if (csw.dCSWSignature != 0x53425355) return -1;   /* "USBS" */
    if (csw.bCSWStatus != 0) return -1;
    return 0;
}

/* SCSI TEST UNIT READY. */
static int scsi_test_unit_ready(void) {
    uint8_t cb[6]; memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_TEST_UNIT_READY;
    return msc_transfer(cb, 6, 0, 0, 0);
}

/* SCSI READ CAPACITY(10) -> block_size, block_count. */
static int scsi_read_capacity(void) {
    uint8_t cb[10]; memset(cb, 0, sizeof(cb));
    uint8_t cap[8];
    cb[0] = SCSI_READ_CAPACITY;
    if (msc_transfer(cb, 10, cap, 8, 1) != 0) return -1;
    uint32_t last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                        ((uint32_t)cap[2] << 8) | cap[3];
    uint32_t bsize = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                     ((uint32_t)cap[6] << 8) | cap[7];
    if (bsize == 0) bsize = 512;
    storage.block_size = bsize;
    storage.block_count = last_lba + 1;
    storage.capacity_mb = (uint32_t)(((uint64_t)storage.block_count * bsize) >> 20);
    return 0;
}

/* --------------------------------------------------------------------------
 * usb_storage_read / write: SCSI READ(10) / WRITE(10) via BBB.
 * -------------------------------------------------------------------------- */
int usb_storage_read(uint32_t lba, uint32_t count, uint8_t* buf) {
    if (!storage_present || !msc_alive() || count == 0) return -1;
    uint8_t cb[10]; memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_READ_10;
    cb[2] = (lba >> 24) & 0xFF; cb[3] = (lba >> 16) & 0xFF;
    cb[4] = (lba >> 8) & 0xFF;  cb[5] = lba & 0xFF;
    cb[7] = (count >> 8) & 0xFF; cb[8] = count & 0xFF;
    uint32_t len = count * storage.block_size;
    if (msc_transfer(cb, 10, buf, len, 1) != 0) return -1;
    return (int)count;
}

int usb_storage_write(uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (!storage_present || !msc_alive() || count == 0) return -1;
    uint8_t cb[10]; memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_WRITE_10;
    cb[2] = (lba >> 24) & 0xFF; cb[3] = (lba >> 16) & 0xFF;
    cb[4] = (lba >> 8) & 0xFF;  cb[5] = lba & 0xFF;
    cb[7] = (count >> 8) & 0xFF; cb[8] = count & 0xFF;
    uint32_t len = count * storage.block_size;
    if (msc_transfer(cb, 10, (uint8_t*)buf, len, 0) != 0) return -1;
    return (int)count;
}

/* Block-device callbacks (fixed 512-byte sectors). */
static int msc_blk_read(uint32_t lba, uint32_t count, uint8_t* buf) {
    return usb_storage_read(lba, count, buf) < 0 ? -1 : 0;
}
static int msc_blk_write(uint32_t lba, uint32_t count, const uint8_t* buf) {
    return usb_storage_write(lba, count, buf) < 0 ? -1 : 0;
}

/* --------------------------------------------------------------------------
 * usb_storage_rescan: unbind a vanished stick, bind a new one.
 * Called at init and by the USB core after hot-plug changes.
 * -------------------------------------------------------------------------- */
void usb_storage_rescan(void) {
    if (msc_dev && (!msc_dev->active || msc_dev->gen != msc_gen)) {
        usb_log("USB-MSC: device removed\n");
        msc_dev = 0; ep_in = 0; ep_out = 0;
        storage.ready = false;
        storage_present = false;
    }
    if (msc_dev) return;

    usb_device_t* devs = usb_get_devices();
    int count = usb_device_count();
    for (int i = 0; i < count; i++) {
        usb_device_t* d = &devs[i];
        if (!d->active || !d->configured || d->class_code != USB_CLASS_MASS_STORAGE) continue;
        if (d->protocol != USB_MSC_PROTO_BBB) continue;    /* bulk-only only */

        usb_endpoint_t* in  = usb_find_endpoint(d, USB_EP_XFER_BULK, 1);
        usb_endpoint_t* out = usb_find_endpoint(d, USB_EP_XFER_BULK, 0);
        if (!in || !out) continue;

        msc_dev = d;
        msc_gen = d->gen;
        ep_in = in;
        ep_out = out;
        storage.usb_dev_index = i;
        storage.block_size = 512;
        storage.block_count = 0;
        storage.capacity_mb = 0;
        storage.ready = true;
        storage_present = true;

        /* Real sticks answer "not ready" for a while after power-up. */
        for (int t = 0; t < 5; t++) {
            if (scsi_test_unit_ready() == 0) break;
            usb_delay_ms(50);
        }
        int ok = -1;
        for (int t = 0; t < 3 && ok != 0; t++) ok = scsi_read_capacity();
        if (ok != 0) { storage.block_count = 0; storage.capacity_mb = 0; }

        {
            char path[24];
            usb_port_path(d, path, sizeof(path));
            usb_log("USB-MSC: bound "); usb_log(path); usb_log(" ");
            usb_log_dec(storage.capacity_mb); usb_log(" MB, ");
            usb_log_dec(storage.block_count); usb_log(" blocks\n");
        }

        /* Expose through the block layer (once; there is no unregister). */
        if (!blk_registered && storage.block_count > 0 && storage.block_size == 512) {
            blkdev_register("usb0", storage.block_count, msc_blk_read, msc_blk_write);
            blk_registered = true;
        }
        break;
    }
}

/* --------------------------------------------------------------------------
 * usb_storage_init: find a mass-storage device, probe it, register a blkdev.
 * -------------------------------------------------------------------------- */
void usb_storage_init(void) {
    memset(&storage, 0, sizeof(storage));
    storage_present = false;
    msc_dev = 0; ep_in = 0; ep_out = 0;
    blk_registered = false;

    usb_storage_rescan();

    if (storage_present) {
        vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("USB Storage: ");
        char b[12]; int_to_str(storage.capacity_mb, b); vga_print(b);
        vga_print(" MB, ");
        int_to_str(storage.block_count, b); vga_print(b);
        vga_print(" blocks\n");
    }
}

bool usb_storage_present(void) { return storage_present && msc_alive(); }
usb_storage_t* usb_storage_get(void) { return usb_storage_present() ? &storage : NULL; }
