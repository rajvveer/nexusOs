/* ============================================================================
 * NexusOS — USB Core (Implementation) — Phase 25
 * ============================================================================
 * Detects UHCI host controller via PCI, enumerates connected USB devices.
 * UHCI uses I/O port-based register access from PCI BAR4.
 * ============================================================================ */

#include "usb.h"
#include "pci.h"
#include "port.h"
#include "vga.h"
#include "string.h"

/* UHCI register offsets */
#define UHCI_CMD        0x00    /* Command register */
#define UHCI_STS        0x02    /* Status register */
#define UHCI_INTR       0x04    /* Interrupt enable */
#define UHCI_FRNUM      0x06    /* Frame number */
#define UHCI_FRBASEADD  0x08    /* Frame list base address */
#define UHCI_SOFMOD     0x0C    /* Start of frame modify */
#define UHCI_PORTSC1    0x10    /* Port 1 status/control */
#define UHCI_PORTSC2    0x12    /* Port 2 status/control */

/* UHCI command bits */
#define UHCI_CMD_RUN    0x01
#define UHCI_CMD_HCRESET 0x02
#define UHCI_CMD_GRESET 0x04

/* UHCI port status bits */
#define UHCI_PORT_CONNECTED    0x01
#define UHCI_PORT_CONNECT_CHG  0x02
#define UHCI_PORT_ENABLED      0x04
#define UHCI_PORT_ENABLE_CHG   0x08
#define UHCI_PORT_LOW_SPEED    0x100
#define UHCI_PORT_RESET        0x200

/* USB devices */
static usb_device_t usb_devices[USB_MAX_DEVICES];
static int num_usb_devices = 0;

/* UHCI controller */
static uhci_controller_t uhci;

/* Next free USB address */
static uint8_t next_usb_address = 1;

/* --------------------------------------------------------------------------
 * USB class name lookup
 * -------------------------------------------------------------------------- */
const char* usb_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "Unspecified";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case USB_CLASS_HID: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case USB_CLASS_MASS_STORAGE: return "Storage";
        case USB_CLASS_HUB: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0E: return "Video";
        case 0xFE: return "App-Specific";
        case 0xFF: return "Vendor";
        default:   return "Unknown";
    }
}

/* --------------------------------------------------------------------------
 * uhci_reset_port: Reset a USB port on the UHCI controller
 * -------------------------------------------------------------------------- */
static void uhci_reset_port(uint16_t port_reg) {
    /* Assert port reset */
    port_word_out(uhci.io_base + port_reg, UHCI_PORT_RESET);

    /* Wait ~50ms (busy loop) */
    for (volatile int i = 0; i < 500000; i++);

    /* Deassert reset */
    port_word_out(uhci.io_base + port_reg, 0);

    /* Wait for device to recover */
    for (volatile int i = 0; i < 100000; i++);

    /* Enable the port */
    uint16_t status = port_word_in(uhci.io_base + port_reg);
    if (status & UHCI_PORT_CONNECTED) {
        port_word_out(uhci.io_base + port_reg, UHCI_PORT_ENABLED);
    }
}

/* --------------------------------------------------------------------------
 * uhci_detect_device: Check if a device is connected on a port
 * -------------------------------------------------------------------------- */
static void uhci_detect_device(uint16_t port_reg) {
    uint16_t status = port_word_in(uhci.io_base + port_reg);

    if (!(status & UHCI_PORT_CONNECTED)) return;
    if (num_usb_devices >= USB_MAX_DEVICES) return;

    /* Reset the port */
    uhci_reset_port(port_reg);

    /* Create device entry */
    usb_device_t* dev = &usb_devices[num_usb_devices];
    memset(dev, 0, sizeof(usb_device_t));

    dev->address = next_usb_address++;
    dev->speed = (status & UHCI_PORT_LOW_SPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
    dev->active = true;
    dev->configured = false;

    /* In a full implementation, we'd send GET_DESCRIPTOR control transfers
     * to read device/config descriptors. For now, detect device class
     * from PCI info and mark as enumerated. */
    dev->class_code = USB_CLASS_HID; /* Default assumption for basic devices */
    dev->vendor_id = 0x0000;
    dev->product_id = 0x0000;

    num_usb_devices++;
}

/* --------------------------------------------------------------------------
 * uhci_init: Initialize the UHCI host controller
 * -------------------------------------------------------------------------- */
static bool uhci_init(pci_device_t* pci_dev) {
    /* Get I/O base from BAR4 (UHCI uses I/O space) */
    uint32_t bar4 = pci_dev->bar[4];
    if (!(bar4 & 0x01)) return false; /* Must be I/O space */

    uhci.io_base = (uint16_t)(bar4 & 0xFFFC);
    uhci.pci_dev = pci_dev;
    uhci.active = true;

    /* Reset the controller */
    port_word_out(uhci.io_base + UHCI_CMD, UHCI_CMD_GRESET);
    for (volatile int i = 0; i < 500000; i++);
    port_word_out(uhci.io_base + UHCI_CMD, 0);

    /* Host controller reset */
    port_word_out(uhci.io_base + UHCI_CMD, UHCI_CMD_HCRESET);
    for (volatile int i = 0; i < 100000; i++);

    /* Wait for reset to complete */
    int timeout = 100;
    while ((port_word_in(uhci.io_base + UHCI_CMD) & UHCI_CMD_HCRESET) && --timeout > 0) {
        for (volatile int i = 0; i < 10000; i++);
    }

    /* Disable interrupts */
    port_word_out(uhci.io_base + UHCI_INTR, 0);

    /* Clear status */
    port_word_out(uhci.io_base + UHCI_STS, 0xFFFF);

    /* Detect devices on port 1 and port 2 */
    uhci_detect_device(UHCI_PORTSC1);
    uhci_detect_device(UHCI_PORTSC2);

    return true;
}

/* --------------------------------------------------------------------------
 * usb_init: Initialize USB subsystem
 * -------------------------------------------------------------------------- */
void usb_init(void) {
    memset(usb_devices, 0, sizeof(usb_devices));
    memset(&uhci, 0, sizeof(uhci));
    num_usb_devices = 0;
    next_usb_address = 1;

    /* Look for UHCI controller on PCI bus (class 0x0C, subclass 0x03, progif 0x00) */
    pci_device_t* devs = pci_get_devices();
    int dev_count = pci_device_count();
    bool found = false;

    for (int i = 0; i < dev_count; i++) {
        if (devs[i].active &&
            devs[i].class_code == 0x0C &&     /* Serial bus */
            devs[i].subclass == 0x03 &&        /* USB */
            devs[i].prog_if == 0x00) {         /* UHCI */
            if (uhci_init(&devs[i])) {
                found = true;
                break;
            }
        }
    }

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    if (found) {
        vga_print("USB: UHCI controller, ");
        char buf[8];
        int_to_str(num_usb_devices, buf);
        vga_print(buf);
        vga_print(" device(s)\n");
    } else {
        vga_print("USB: No host controller\n");
    }
}

/* --------------------------------------------------------------------------
 * Accessors
 * -------------------------------------------------------------------------- */
usb_device_t* usb_get_devices(void) { return usb_devices; }
int usb_device_count(void) { return num_usb_devices; }
