/* ============================================================================
 * NexusOS — PCI Bus Driver (Implementation) — Phase 23
 * ============================================================================
 * Scans PCI buses, reads config space, detects hardware devices.
 * ============================================================================ */

#include "pci.h"
#include "port.h"
#include "vga.h"
#include "string.h"

/* Discovered devices */
static pci_device_t devices[PCI_MAX_DEVICES];
static int num_devices = 0;

/* --------------------------------------------------------------------------
 * PCI Config Space Access
 * Address format: [31]=enable, [23:16]=bus, [15:11]=dev, [10:8]=func, [7:2]=reg
 * -------------------------------------------------------------------------- */

static uint32_t pci_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (1U << 31) |
           ((uint32_t)bus << 16) |
           ((uint32_t)(dev & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) |
           (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    port_dword_out(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    return port_dword_in(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, dev, func, offset & 0xFC);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, dev, func, offset & 0xFC);
    return (uint8_t)(val >> ((offset & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    port_dword_out(PCI_CONFIG_ADDR, pci_address(bus, dev, func, offset));
    port_dword_out(PCI_CONFIG_DATA, val);
}

/* --------------------------------------------------------------------------
 * PCI Class Names
 * -------------------------------------------------------------------------- */
const char* pci_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "Legacy";
        case 0x01: return "Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Comms";
        case 0x08: return "System";
        case 0x09: return "Input";
        case 0x0A: return "Docking";
        case 0x0B: return "Processor";
        case 0x0C: return "Serial";
        case 0x0D: return "Wireless";
        default:   return "Other";
    }
}

/* --------------------------------------------------------------------------
 * pci_scan_device: Check one bus/dev/func for a device
 * -------------------------------------------------------------------------- */
static void pci_scan_device(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t vendor = pci_read16(bus, dev, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return; /* No device */

    if (num_devices >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &devices[num_devices];
    d->bus = bus;
    d->device = dev;
    d->function = func;
    d->vendor_id = vendor;
    d->device_id = pci_read16(bus, dev, func, PCI_DEVICE_ID);
    d->class_code = pci_read8(bus, dev, func, PCI_CLASS);
    d->subclass = pci_read8(bus, dev, func, PCI_SUBCLASS);
    d->prog_if = pci_read8(bus, dev, func, PCI_PROG_IF);
    d->revision = pci_read8(bus, dev, func, PCI_REVISION);
    d->irq_line = pci_read8(bus, dev, func, PCI_IRQ_LINE);

    /* Read BARs */
    for (int i = 0; i < 6; i++) {
        d->bar[i] = pci_read32(bus, dev, func, PCI_BAR0 + i * 4);
    }

    d->active = true;
    num_devices++;
}

/* --------------------------------------------------------------------------
 * pci_init: Scan all PCI buses and enumerate devices
 * -------------------------------------------------------------------------- */
void pci_init(void) {
    memset(devices, 0, sizeof(devices));
    num_devices = 0;

    /* Brute-force scan: bus 0–255, dev 0–31, func 0–7 */
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_read16(bus, dev, 0, PCI_VENDOR_ID);
            if (vendor == 0xFFFF) continue;

            pci_scan_device(bus, dev, 0);

            /* Check multi-function */
            uint8_t header = pci_read8(bus, dev, 0, PCI_HEADER_TYPE);
            if (header & 0x80) {
                for (int func = 1; func < 8; func++) {
                    pci_scan_device(bus, dev, func);
                }
            }
        }
    }

    /* Print results */
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("PCI: ");
    char buf[12];
    int_to_str(num_devices, buf);
    vga_print(buf);
    vga_print(" devices found\n");

    /* List devices */
    for (int i = 0; i < num_devices; i++) {
        pci_device_t* d = &devices[i];
        vga_print("     ");
        vga_print(pci_class_name(d->class_code));
        vga_print(" [");
        /* Print vendor:device in hex */
        char hex[8];
        hex[0] = "0123456789ABCDEF"[(d->vendor_id >> 12) & 0xF];
        hex[1] = "0123456789ABCDEF"[(d->vendor_id >> 8) & 0xF];
        hex[2] = "0123456789ABCDEF"[(d->vendor_id >> 4) & 0xF];
        hex[3] = "0123456789ABCDEF"[d->vendor_id & 0xF];
        hex[4] = '\0';
        vga_print(hex);
        vga_print(":");
        hex[0] = "0123456789ABCDEF"[(d->device_id >> 12) & 0xF];
        hex[1] = "0123456789ABCDEF"[(d->device_id >> 8) & 0xF];
        hex[2] = "0123456789ABCDEF"[(d->device_id >> 4) & 0xF];
        hex[3] = "0123456789ABCDEF"[d->device_id & 0xF];
        vga_print(hex);
        vga_print("]\n");
    }
}

/* --------------------------------------------------------------------------
 * Device Lookup
 * -------------------------------------------------------------------------- */

pci_device_t* pci_find_device(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < num_devices; i++) {
        if (devices[i].active &&
            devices[i].vendor_id == vendor &&
            devices[i].device_id == device)
            return &devices[i];
    }
    return NULL;
}

pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < num_devices; i++) {
        if (devices[i].active &&
            devices[i].class_code == class_code &&
            devices[i].subclass == subclass)
            return &devices[i];
    }
    return NULL;
}

pci_device_t* pci_get_devices(void) { return devices; }
int pci_device_count(void) { return num_devices; }
