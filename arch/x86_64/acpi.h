/* Parsed ACPI data shared by acpi.c (parser, power control), irq.c (IOAPIC routing) and smp.c
 * (AP start-up). Filled once by acpi_init(); read-only afterwards. */
#ifndef NEXUS_ACPI_H
#define NEXUS_ACPI_H
#include "../../kernel/types.h"

#define ACPI_MAX_CPUS 256
#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_NMIS 16
#define ACPI_MAX_MCFG 8
struct acpi_gas { uint8_t space, width, offset, access; uint64_t address; };  /* Generic Address Structure */
struct acpi_ioapic { uint32_t address, gsi_base; uint8_t id, pins; };
struct acpi_override { uint32_t gsi; uint16_t flags; bool present; };      /* MPS INTI flags */
struct acpi_nmi { uint32_t processor; uint16_t flags; uint8_t lint; };      /* processor 0xFF(FFFFFF) = all */
struct acpi_mcfg { uint64_t base; uint16_t segment; uint8_t start_bus, end_bus; };
struct acpi_info {
    bool present, madt, fadt, s5, xsdt;
    uint8_t revision;                      /* RSDP revision (0 = ACPI 1.0) */
    uint64_t rsdp;
    unsigned tables;                       /* valid tables listed by the RSDT/XSDT */
    /* MADT */
    uint32_t lapic_address, madt_flags;    /* flags bit 0: PC-AT dual 8259 present */
    unsigned cpu_count;                    /* enabled processors (ids below; x2APIC ids > 254 skipped) */
    uint8_t cpu_apic_ids[ACPI_MAX_CPUS];
    uint8_t cpu_uids[ACPI_MAX_CPUS];
    unsigned ioapic_count, override_count, nmi_count;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    struct acpi_override isa[16];          /* interrupt source overrides by ISA IRQ */
    struct acpi_nmi nmis[ACPI_MAX_NMIS];   /* local APIC NMI inputs (LINT0/LINT1) */
    /* FADT */
    uint32_t fadt_flags, smi_command;
    uint8_t fadt_revision, acpi_enable, reset_value, s5_a, s5_b;
    uint16_t boot_arch;                    /* IAPC_BOOT_ARCH */
    struct acpi_gas pm1a_control, pm1b_control, reset, sleep_control;
    uint64_t dsdt;
    /* MCFG */
    unsigned mcfg_count;
    struct acpi_mcfg mcfg[ACPI_MAX_MCFG];
};
extern struct acpi_info acpi;
#define ACPI_FADT_RESET_REG_SUP (1u << 10)
#define ACPI_FADT_HW_REDUCED (1u << 20)
#endif
