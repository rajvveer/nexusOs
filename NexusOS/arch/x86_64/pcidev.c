/* PCI BAR decoding, DMA enable, uncached MMIO mappings and interrupt hookup for the native xHCI and
 * Intel NIC drivers. The kernel's direct map covers the low 4 GiB with 2 MiB pages; device registers there
 * get PCD|PWT (PAT entry 3 = UC) on their 2 MiB entries. BARs above 4 GiB (UEFI firmware places
 * 64-bit BARs high) go through the platform's kernel MMIO window (irq.c mmio_map). Interrupts use
 * the platform layer: MSI when the device has it, else its INTx line through the PIC or IOAPIC. */
#include "pcidev.h"

extern uint64_t boot_pd[];
#define UNCACHED 24ULL
#define HUGE (2ULL << 20)

uint64_t pci_bar_memory(const pci_device_t* device, unsigned index, uint64_t* size) {
    uint8_t bus = device->bus, slot = device->device, function = device->function, reg = (uint8_t)(PCI_BAR0 + index * 4);
    *size = 0;
    if (index > 5) return 0;
    uint32_t low = pci_read32(bus, slot, function, reg);
    if (low & 1) return 0;
    bool wide = ((low >> 1) & 3) == 2;
    if (wide && index == 5) return 0;
    uint32_t high = wide ? pci_read32(bus, slot, function, (uint8_t)(reg + 4)) : 0;
    uint16_t command = pci_read16(bus, slot, function, PCI_COMMAND);
    pci_write32(bus, slot, function, PCI_COMMAND, command & ~3U); /* no decoding while the BAR reads back its mask */
    pci_write32(bus, slot, function, reg, 0xFFFFFFFFU);
    uint32_t mask_low = pci_read32(bus, slot, function, reg), mask_high = 0xFFFFFFFFU;
    pci_write32(bus, slot, function, reg, low);
    if (wide) {
        pci_write32(bus, slot, function, (uint8_t)(reg + 4), 0xFFFFFFFFU);
        mask_high = pci_read32(bus, slot, function, (uint8_t)(reg + 4));
        pci_write32(bus, slot, function, (uint8_t)(reg + 4), high);
    }
    pci_write32(bus, slot, function, PCI_COMMAND, command);
    uint64_t mask = ((uint64_t)mask_high << 32) | (mask_low & ~0xFULL), base = ((uint64_t)high << 32) | (low & ~0xFULL);
    if (!(mask_low & ~0xFULL)) return 0;
    *size = ~mask + 1;
    return base;
}
uint16_t pci_enable_dma(const pci_device_t* device) {
    uint16_t command = pci_read16(device->bus, device->device, device->function, PCI_COMMAND);
    pci_write32(device->bus, device->device, device->function, PCI_COMMAND, (command | 6U) & ~0x400U);
    return command;
}
void pci_restore_command(const pci_device_t* device, uint16_t command) {
    pci_write32(device->bus, device->device, device->function, PCI_COMMAND, command);
}
volatile void* pci_mmio_map(uint64_t physical, uint64_t size) {
    if (!physical || !size || physical + size < physical || physical + size > (1ULL << 46)) return NULL;
    uint64_t first = physical & ~(HUGE - 1), end = (physical + size + HUGE - 1) & ~(HUGE - 1);
    if (end <= 0x100000000ULL) {
        for (uint64_t address = first; address < end; address += HUGE) {
            boot_pd[address >> 21] |= UNCACHED;
            __asm__ volatile("invlpg (%0)" :: "r"(phys_to_virt(address)) : "memory");
        }
        return phys_to_virt(physical);
    }
    return mmio_map(physical, size, MMIO_UNCACHED);
}
/* Both drivers check that the vector is theirs and read their own cause registers. */
static void pci_device_interrupt(unsigned vector, void* context) {
    (void)context;
    native_nic_irq(vector);
    usb_irq(vector);
}
unsigned pci_intx_route(const pci_device_t* device) {
    pci_device_t* writable = (pci_device_t*)device;
    if (pci_find_capability(device, 0x05)) { /* MSI: edge-triggered, delivered to the BSP */
        int vector = irq_alloc_vector(0);
        if (vector >= 0) {
            if (!irq_register((unsigned)vector, pci_device_interrupt, NULL) && !pci_enable_msi(writable, (unsigned)vector, 0)) return (unsigned)vector;
            irq_free_vector((unsigned)vector);
        }
    }
    /* INTx: the ACPI _PRT route in IOAPIC mode, else the firmware-assigned line (level-triggered, PIC or IOAPIC). */
    int vector = irq_attach_pci(device, 0, pci_device_interrupt, NULL);
    return vector < 0 ? 0 : (unsigned)vector;
}
