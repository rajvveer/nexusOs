/* Native NIC selection behind the native_nic_* interface that network.c uses. Boot order: the first
 * supported Intel controller in PCI scan order (82574L e1000e, 82576 igb, 8254x e1000, I217/I218/I219, I225/I226; see
 * e1000.c), then Realtek PCIe chips (RTL8111/8168 g and later, RTL8125/8126; rtl8169.c), then the RTL8139.
 * Only one NIC is driven; the protocol stack is unchanged.
 * rtl8139.c keeps its own source: build.py compiles it with native_nic_* renamed to rtl8139_nic_*.
 *
 * Power management: native_nic_suspend()/native_nic_resume() quiesce and restore the active NIC (drivers
 * without hooks are left alone; rtl8139.c registers its own pm_ops); they are registered with pm.c as the "nic"
 * hooks once a driver is chosen. native_nic_rx_mode() pushes netdev.c's receive filter. native_nic_late_init()
 * runs once the root volume is mounted (firmware files). native_nic_link() reports carrier, speed and duplex. */
#include "netdrv.h"
#include "power.h"

static const struct nic_driver {
    const char* name;
    const pci_device_t* (*pci)(void);
    bool (*init)(void);
    void (*poll)(void);
    void (*irq)(unsigned vector);
    bool (*rx_pending)(void);
    void (*rx_mode)(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count);
    void (*late_init)(void);
    void (*suspend)(void);
    bool (*resume)(void);
    bool (*link)(struct nic_link* link);
} drivers[] = {
    {.name = "e1000", .pci = e1000_nic_pci, .init = e1000_nic_init, .poll = e1000_nic_poll, .irq = e1000_nic_irq, .rx_pending = e1000_nic_rx_pending, .rx_mode = e1000_nic_rx_mode,
     .suspend = e1000_nic_suspend, .resume = e1000_nic_resume, .link = e1000_nic_link},
    {.name = "rtl8169", .pci = rtl8169_nic_pci, .init = rtl8169_nic_init, .poll = rtl8169_nic_poll, .irq = rtl8169_nic_irq, .rx_pending = rtl8169_nic_rx_pending, .rx_mode = rtl8169_nic_rx_mode,
     .late_init = rtl8169_nic_late_init, .suspend = rtl8169_nic_suspend, .resume = rtl8169_nic_resume, .link = rtl8169_nic_link},
    {.name = "rtl8139", .init = rtl8139_nic_init, .poll = rtl8139_nic_poll, .irq = rtl8139_nic_irq, .rx_pending = rtl8139_nic_rx_pending, .rx_mode = rtl8139_nic_rx_mode},
};
static const struct nic_driver* active;

bool native_nic_init(void) {
    for (unsigned i = 0; i < sizeof(drivers) / sizeof(*drivers); i++)
        if (drivers[i].init()) {
            active = &drivers[i];
            if (active->suspend) pm_register_hooks("nic", native_nic_suspend, native_nic_resume);   /* system sleep (pm.c) */
            return true;
        }
    return false;
}
void native_nic_poll(void) { if (active) active->poll(); }
const char* native_nic_driver(void) { return active ? active->name : "none"; }
const pci_device_t* native_nic_pci(void) { return active && active->pci ? active->pci() : NULL; }
void native_nic_irq(unsigned vector) { if (active) active->irq(vector); }
bool native_nic_rx_pending(void) { return active && active->rx_pending(); }
void native_nic_rx_mode(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count) {
    if (active && active->rx_mode) active->rx_mode(promiscuous, all_multicast, groups, count);
}
void native_nic_late_init(void) { if (active && active->late_init) active->late_init(); }
void native_nic_suspend(void) { if (active && active->suspend) active->suspend(); }
bool native_nic_resume(void) { return !active || !active->resume || active->resume(); }
/* Carrier for netlink/sysfs: drivers without a link hook report the device's link_up flag, speed unknown. */
bool native_nic_link(struct nic_link* link) {
    if (!active) return false;
    if (active->link) return active->link(link);
    net_device_t* device = net_get_device(0);
    link->up = device && device->link_up; link->speed = 0; link->duplex = NIC_DUPLEX_UNKNOWN;
    return device != NULL;
}
