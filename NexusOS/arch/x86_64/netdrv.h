/* Shared helpers for the native PCIe Ethernet drivers (rtl8169.c, e1000.c): interrupt setup with an
 * MSI-X -> MSI -> INTx -> polled fallback chain, boot-aware delays, firmware files from /lib/firmware
 * pinned by SHA-256, and link-state reporting (console line, NETLINK_ROUTE RTM_NEWLINK events,
 * /sys/class/net attributes through native_nic_link()). */
#ifndef NEXUS_NETDRV_H
#define NEXUS_NETDRV_H
#include "pcidev.h"
#include "../../kernel/net.h"

/* ---- interrupts ---- */
enum netdrv_irq_kind { NETDRV_POLLED = 0, NETDRV_INTX = 1, NETDRV_MSI = 2, NETDRV_MSIX = 3 };
struct netdrv_irq { unsigned vector, kind, line; };
/* Tries MSI-X entry 0 (when allowed), then MSI, then the legacy INTx line (level-triggered through irq.c);
 * kind NETDRV_POLLED with vector 0 when none works. The handler calls native_nic_irq(vector). */
void netdrv_irq_setup(pci_device_t* device, struct netdrv_irq* irq, bool allow_msix, bool allow_msi);
void netdrv_irq_release(pci_device_t* device, struct netdrv_irq* irq);
/* After a power transition that lost config space: point MSI/MSI-X at the same vector again. */
bool netdrv_irq_restore(pci_device_t* device, const struct netdrv_irq* irq);
const char* netdrv_irq_name(unsigned kind);

/* ---- delays: spin with pause; at boot (netdrv_booting, BSP only) waits of a millisecond or more halt
 * until the next tick instead (boot_wait) so a hypervisor's device model keeps running. ---- */
extern bool netdrv_booting;
void netdrv_udelay(uint64_t microseconds);
void netdrv_mdelay(uint64_t milliseconds);

/* ---- firmware ---- */
struct netdrv_firmware_pin { const char* name; uint32_t size; const char* sha256; };
extern const struct netdrv_firmware_pin netdrv_firmware_pins[];
/* Reads /lib/firmware/<name> (or /usr/lib/firmware/<name>), checks size and SHA-256 against the pin table.
 * 0 and an arena buffer the caller frees with netdrv_firmware_free; -2 missing (no root volume or no file),
 * -1 no pin for this name, -27 too large, -74 size/hash mismatch, -12 no memory, -5 read error. */
int netdrv_firmware_load(const char* name, uint8_t** data, uint32_t* size);
void netdrv_firmware_free(uint8_t* data, uint32_t size);
void netdrv_sha256(const void* data, uint64_t size, uint8_t out[32]);
const char* netdrv_error_name(int error);

/* ---- link ---- */
struct nic_link { bool up; uint32_t speed; uint8_t duplex; /* NIC_DUPLEX_* */ };
#define NIC_DUPLEX_UNKNOWN 0
#define NIC_DUPLEX_HALF 1
#define NIC_DUPLEX_FULL 2
/* Prints "[64] eth0: link up 2500 Mb/s full duplex" / "link down", updates device->link_up, and queues an
 * RTM_NEWLINK notification for netlink listeners (netlink_link_changed). Poll context only. */
void netdrv_link_report(net_device_t* device, const struct nic_link* now);
void netlink_link_changed(void);   /* netlink.c */
/* /sys/class/net/<if>/<name> text (sysfs.c): attribute index into netdrv_sysfs_names, interface 1 = lo, 2 = eth0. */
extern const char* const netdrv_sysfs_names[];
unsigned netdrv_sysfs_text(unsigned attribute, unsigned index, char* text);
void netdrv_print_hex(uint64_t value, unsigned digits);
void netdrv_print_dec(uint64_t value);
#endif
