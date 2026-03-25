/* ============================================================================
 * NexusOS — USB Mass Storage (Implementation) — Phase 25
 * ============================================================================
 * Scans for USB mass storage devices. Provides block I/O stubs.
 * Full bulk transfers require complete UHCI transfer descriptor support.
 * ============================================================================ */

#include "usb_storage.h"
#include "usb.h"
#include "vga.h"
#include "string.h"

static usb_storage_t storage;
static bool storage_present = false;

/* --------------------------------------------------------------------------
 * usb_storage_init: Scan for mass storage devices
 * -------------------------------------------------------------------------- */
void usb_storage_init(void) {
    memset(&storage, 0, sizeof(storage));
    storage_present = false;

    usb_device_t* devs = usb_get_devices();
    int count = usb_device_count();

    for (int i = 0; i < count; i++) {
        if (!devs[i].active) continue;
        if (devs[i].class_code != USB_CLASS_MASS_STORAGE) continue;

        storage.usb_dev_index = i;
        storage.block_size = 512;
        storage.block_count = 0;  /* Would be read via SCSI READ_CAPACITY */
        storage.capacity_mb = 0;
        storage.ready = true;
        storage_present = true;

        vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("USB Storage: device found\n");
        break;
    }
}

/* --------------------------------------------------------------------------
 * Block I/O stubs
 * Full implementation requires UHCI transfer descriptors for bulk transfers.
 * Currently returns error; ATA disk is the primary storage path.
 * -------------------------------------------------------------------------- */
int usb_storage_read(uint32_t lba, uint32_t count, uint8_t* buf) {
    (void)lba; (void)count; (void)buf;
    if (!storage_present || !storage.ready) return -1;
    /* TODO: Implement bulk-only transport with SCSI READ(10) */
    return -1;
}

int usb_storage_write(uint32_t lba, uint32_t count, const uint8_t* buf) {
    (void)lba; (void)count; (void)buf;
    if (!storage_present || !storage.ready) return -1;
    /* TODO: Implement bulk-only transport with SCSI WRITE(10) */
    return -1;
}

bool usb_storage_present(void) { return storage_present; }
usb_storage_t* usb_storage_get(void) { return storage_present ? &storage : NULL; }
