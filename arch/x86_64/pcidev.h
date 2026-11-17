/* PCI device helpers for the native xHCI (xhci.c) and Intel NIC (e1000.c) drivers. */
#ifndef NEXUS_PCIDEV_H
#define NEXUS_PCIDEV_H
#include "native.h"
#include "../../kernel/pci.h"

/* Physical base and size of memory BAR `index` (32- or 64-bit); 0 when it is an I/O BAR or unassigned. */
uint64_t pci_bar_memory(const pci_device_t* device, unsigned index, uint64_t* size);
/* Enable memory decoding and bus mastering (DMA); keeps legacy INTx enabled. Returns the old command word. */
uint16_t pci_enable_dma(const pci_device_t* device);
void pci_restore_command(const pci_device_t* device, uint16_t command);
/* Uncached kernel mapping of device registers: the direct map below 4 GiB, a boot-time window above.
 * Must run at boot (before user address spaces copy the kernel's top-level table). NULL on failure. */
volatile void* pci_mmio_map(uint64_t physical, uint64_t size);
/* Interrupt hookup for these drivers, in one place: MSI to the BSP when the device has it, else its
 * legacy INTx line (level-triggered) through the platform's PIC/IOAPIC layer. Returns the vector, or
 * 0 when there is none (the driver then runs polled). */
unsigned pci_intx_route(const pci_device_t* device);

static inline uint32_t mmio_read32(volatile void* base, unsigned offset) { return *(volatile uint32_t*)((volatile uint8_t*)base + offset); }
static inline void mmio_write32(volatile void* base, unsigned offset, uint32_t value) { *(volatile uint32_t*)((volatile uint8_t*)base + offset) = value; }
static inline void mmio_write64(volatile void* base, unsigned offset, uint64_t value) {
    mmio_write32(base, offset, (uint32_t)value); mmio_write32(base, offset + 4, (uint32_t)(value >> 32));
}
static inline void cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }
/* Boot-time device waits on the BSP: halt until the next 1 ms tick instead of spinning. A spinning vCPU
 * starves device emulation and virtual time on some hypervisors (VMware on a Hyper-V host took ~6 s per
 * USB enumeration step); real hardware is unaffected. Never use it in syscall or interrupt context. */
static inline void boot_wait(void) { irq_wait_restricted(); }

/* NIC back ends behind native_nic_* (nic.c). rtl8139.c is compiled with its native_nic_* names mapped to rtl8139_nic_*. */
bool e1000_nic_init(void);
void e1000_nic_poll(void);
void e1000_nic_irq(unsigned vector);
bool e1000_nic_rx_pending(void);
void e1000_nic_rx_mode(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count);
bool rtl8139_nic_init(void);
void rtl8139_nic_poll(void);
void rtl8139_nic_irq(unsigned vector);
bool rtl8139_nic_rx_pending(void);
void rtl8139_nic_rx_mode(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count);
/* Realtek RTL8111/8168 g+ and RTL8125/8126 (rtl8169.c); carrier/speed/duplex of the active NIC (nic.c). */
struct nic_link;
bool rtl8169_nic_init(void);
void rtl8169_nic_poll(void);
void rtl8169_nic_irq(unsigned vector);
bool rtl8169_nic_rx_pending(void);
void rtl8169_nic_rx_mode(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count);
void rtl8169_nic_late_init(void);
void rtl8169_nic_suspend(void);
bool rtl8169_nic_resume(void);
bool rtl8169_nic_link(struct nic_link* link);
void e1000_nic_suspend(void);
bool e1000_nic_resume(void);
bool e1000_nic_link(struct nic_link* link);
bool native_nic_link(struct nic_link* link);
const char* native_nic_driver(void);          /* "e1000", "rtl8169", "rtl8139" or "none" (diag.c) */
const pci_device_t* native_nic_pci(void);     /* the driven NIC's PCI function, NULL if unknown */
const pci_device_t* e1000_nic_pci(void);
const pci_device_t* rtl8169_nic_pci(void);
#endif
