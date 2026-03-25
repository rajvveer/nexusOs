/* ============================================================================
 * NexusOS — PCI Bus Driver (Header) — Phase 23
 * ============================================================================
 * PCI configuration space access and bus enumeration.
 * ============================================================================ */

#ifndef PCI_H
#define PCI_H

#include "types.h"

/* PCI I/O ports */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* PCI config space offsets */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_REVISION     0x08
#define PCI_PROG_IF      0x09
#define PCI_SUBCLASS     0x0A
#define PCI_CLASS        0x0B
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24
#define PCI_IRQ_LINE     0x3C

/* PCI class codes */
#define PCI_CLASS_STORAGE    0x01
#define PCI_CLASS_NETWORK    0x02
#define PCI_CLASS_DISPLAY    0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MEMORY     0x05
#define PCI_CLASS_BRIDGE     0x06
#define PCI_CLASS_SERIAL     0x0C

/* Max discovered devices */
#define PCI_MAX_DEVICES  32

/* PCI device info */
typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  irq_line;
    uint32_t bar[6];
    bool     active;
} pci_device_t;

/* Initialize PCI and scan bus */
void pci_init(void);

/* Config space access */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);

/* Device lookup */
pci_device_t* pci_find_device(uint16_t vendor, uint16_t device);
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);
pci_device_t* pci_get_devices(void);
int           pci_device_count(void);

/* Get class name string */
const char* pci_class_name(uint8_t class_code);

#endif /* PCI_H */
