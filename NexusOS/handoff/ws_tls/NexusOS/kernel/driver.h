/* ============================================================================
 * NexusOS — Driver Framework (Header) — Phase 23
 * ============================================================================
 * Pluggable driver model with PCI matching and shared IRQ routing.
 * ============================================================================ */

#ifndef DRIVER_H
#define DRIVER_H

#include "types.h"
#include "pci.h"
#include "idt.h"

/* Driver limits */
#define MAX_DRIVERS       16
#define MAX_IRQ_HANDLERS  4   /* Max handlers sharing one IRQ */
#define NUM_IRQS          16

/* IRQ handler callback */
typedef void (*irq_handler_fn)(struct registers* regs);

/* Driver structure */
typedef struct {
    const char* name;

    /* PCI match criteria (0 = match any) */
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint8_t  pci_class;
    uint8_t  pci_subclass;

    /* Callbacks */
    int  (*init)(void);                    /* One-time initialization */
    int  (*probe)(pci_device_t* dev);      /* Called when PCI device matches */
    void (*remove)(void);                  /* Cleanup/shutdown */

    /* State */
    bool active;
    bool probed;
} driver_t;

/* Initialize driver framework */
void driver_init(void);

/* Register/unregister a driver */
int  driver_register(driver_t* drv);
void driver_unregister(const char* name);

/* Probe all registered drivers against PCI devices */
void driver_probe_all(void);

/* IRQ routing with shared handler support */
int  irq_register_handler(uint8_t irq, irq_handler_fn handler);
void irq_unregister_handler(uint8_t irq, irq_handler_fn handler);

/* Get driver count */
int driver_count(void);

#endif /* DRIVER_H */
