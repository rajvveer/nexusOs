/* ============================================================================
 * NexusOS — Driver Framework (Implementation) — Phase 23
 * ============================================================================
 * Pluggable driver model with PCI matching and shared IRQ routing.
 * ============================================================================ */

#include "driver.h"
#include "pci.h"
#include "vga.h"
#include "string.h"

/* Registered drivers */
static driver_t* drivers[MAX_DRIVERS];
static int num_drivers = 0;

/* Shared IRQ handler chains */
static struct {
    irq_handler_fn handlers[MAX_IRQ_HANDLERS];
    int count;
} irq_chains[NUM_IRQS];

/* --------------------------------------------------------------------------
 * driver_init: Initialize driver framework
 * -------------------------------------------------------------------------- */
void driver_init(void) {
    memset(drivers, 0, sizeof(drivers));
    memset(irq_chains, 0, sizeof(irq_chains));
    num_drivers = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Driver framework initialized\n");
}

/* --------------------------------------------------------------------------
 * driver_register: Register a driver
 * -------------------------------------------------------------------------- */
int driver_register(driver_t* drv) {
    if (!drv || num_drivers >= MAX_DRIVERS) return -1;

    drivers[num_drivers++] = drv;
    drv->active = true;
    drv->probed = false;

    /* Call init if provided */
    if (drv->init) {
        drv->init();
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * driver_unregister: Unregister a driver by name
 * -------------------------------------------------------------------------- */
void driver_unregister(const char* name) {
    for (int i = 0; i < num_drivers; i++) {
        if (drivers[i] && drivers[i]->name && strcmp(drivers[i]->name, name) == 0) {
            if (drivers[i]->remove) {
                drivers[i]->remove();
            }
            drivers[i]->active = false;
            return;
        }
    }
}

/* --------------------------------------------------------------------------
 * driver_match: Check if a driver matches a PCI device
 * -------------------------------------------------------------------------- */
static bool driver_match(driver_t* drv, pci_device_t* dev) {
    if (drv->pci_vendor != 0 && drv->pci_vendor != dev->vendor_id) return false;
    if (drv->pci_device != 0 && drv->pci_device != dev->device_id) return false;
    if (drv->pci_class != 0 && drv->pci_class != dev->class_code) return false;
    if (drv->pci_subclass != 0 && drv->pci_subclass != dev->subclass) return false;
    return true;
}

/* --------------------------------------------------------------------------
 * driver_probe_all: Match PCI devices to drivers and probe
 * -------------------------------------------------------------------------- */
void driver_probe_all(void) {
    int probed = 0;
    pci_device_t* devs = pci_get_devices();
    int dev_count = pci_device_count();

    for (int d = 0; d < num_drivers; d++) {
        if (!drivers[d] || !drivers[d]->active || drivers[d]->probed) continue;
        if (!drivers[d]->probe) continue;

        for (int i = 0; i < dev_count; i++) {
            if (!devs[i].active) continue;
            if (driver_match(drivers[d], &devs[i])) {
                int ret = drivers[d]->probe(&devs[i]);
                if (ret == 0) {
                    drivers[d]->probed = true;
                    probed++;
                }
                break; /* One device per driver for now */
            }
        }
    }

    if (probed > 0) {
        vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("Probed ");
        char buf[8];
        int_to_str(probed, buf);
        vga_print(buf);
        vga_print(" driver(s)\n");
    }
}

/* --------------------------------------------------------------------------
 * IRQ Routing (shared handlers)
 * -------------------------------------------------------------------------- */

/* Master IRQ dispatcher — called from IDT for IRQs */
static void irq_chain_handler(struct registers* regs) {
    uint8_t irq = (uint8_t)(regs->int_no - 32); /* IRQs start at IDT 32 */
    if (irq >= NUM_IRQS) return;

    for (int i = 0; i < irq_chains[irq].count; i++) {
        if (irq_chains[irq].handlers[i]) {
            irq_chains[irq].handlers[i](regs);
        }
    }
}

int irq_register_handler(uint8_t irq, irq_handler_fn handler) {
    if (irq >= NUM_IRQS || !handler) return -1;
    if (irq_chains[irq].count >= MAX_IRQ_HANDLERS) return -1;

    /* Register chain handler on first use */
    if (irq_chains[irq].count == 0) {
        register_interrupt_handler(32 + irq, irq_chain_handler);
    }

    irq_chains[irq].handlers[irq_chains[irq].count++] = handler;
    return 0;
}

void irq_unregister_handler(uint8_t irq, irq_handler_fn handler) {
    if (irq >= NUM_IRQS) return;

    for (int i = 0; i < irq_chains[irq].count; i++) {
        if (irq_chains[irq].handlers[i] == handler) {
            /* Shift remaining handlers down */
            for (int j = i; j < irq_chains[irq].count - 1; j++) {
                irq_chains[irq].handlers[j] = irq_chains[irq].handlers[j + 1];
            }
            irq_chains[irq].count--;
            return;
        }
    }
}

/* --------------------------------------------------------------------------
 * driver_count
 * -------------------------------------------------------------------------- */
int driver_count(void) { return num_drivers; }
