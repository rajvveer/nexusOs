/* Interrupt routing (8259 PIC or IOAPIC from the ACPI MADT), local-APIC EOI/TPR, the dynamic
 * vector allocator and handler table, PCI MSI/MSI-X, and the kernel MMIO window. The contract for
 * drivers is documented in native.h ("Platform"). */
#include "native.h"
#include "acpi.h"
#include "apic.h"
#include "boot_info.h"
#include "power.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define LAPIC_BASE 0xFEE00000ULL
#define LAPIC_ID 0x20
#define LAPIC_TPR 0x80
#define LAPIC_SVR 0xF0
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define VEC_ISA 0x20
#define VEC_ISA_WAKE 0x90
#define VEC_DYNAMIC_FIRST 0x40
#define VEC_DYNAMIC_LAST 0x8F
#define VEC_WAKE_FIRST 0xA0
#define VEC_WAKE_LAST 0xEF
#define VEC_SYSCALL 0x80
#define TPR_RESTRICTED 8ULL            /* CR8 class 8: only vectors >= 0x90 get through */
enum { SLOT_FREE, SLOT_FIXED, SLOT_DYNAMIC, SLOT_ISA };

struct irq_action { irq_handler_t handler; void* context; };
struct vector_entry { struct irq_action actions[IRQ_SHARED_MAX]; };
static struct vector_entry* vectors;   /* 256 entries (16 KiB, arena memory from irq_init) */
static uint8_t vector_state[256];
static bool ioapic_mode;
static uint16_t pic_mask = 0xFFFF;     /* PIC mode: cached IMR, bit set = masked */
static uint8_t isa_vector[16];
static uint32_t isa_gsi[16], isa_low[16];
static uint8_t bsp_apic;

static inline uint32_t lapic_read(unsigned reg) { return apic_read(reg); }              /* xAPIC or x2APIC (apic.c) */
static inline void lapic_write(unsigned reg, uint32_t value) { apic_write(reg, value); }
static void cpuid1(uint32_t* ecx, uint32_t* edx) { uint32_t a, b; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(*ecx), "=d"(*edx) : "a"(1), "c"(0)); }
static void print_hex(uint64_t value) {
    char text[19]; text[0] = '0'; text[1] = 'x';
    unsigned digits = 1; while (digits < 16 && (value >> (4 * digits))) digits++;
    for (unsigned i = 0; i < digits; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (4 * (digits - 1 - i))) & 15];
    text[2 + digits] = 0; vga_print(text);
}
static void print_number(uint64_t value) { char text[12]; uint_to_str((uint32_t)value, text); vga_print(text); }

/* ---- IOAPIC ---- */
static uint32_t ioapic_read(const struct acpi_ioapic* io, unsigned reg) {
    *(volatile uint32_t*)phys_to_virt(io->address) = reg; return *(volatile uint32_t*)phys_to_virt((uint64_t)io->address + 0x10);
}
static void ioapic_write(const struct acpi_ioapic* io, unsigned reg, uint32_t value) {
    *(volatile uint32_t*)phys_to_virt(io->address) = reg; *(volatile uint32_t*)phys_to_virt((uint64_t)io->address + 0x10) = value;
}
static const struct acpi_ioapic* ioapic_for(uint32_t gsi, unsigned* pin) {
    for (unsigned i = 0; i < acpi.ioapic_count; i++) {
        const struct acpi_ioapic* io = &acpi.ioapics[i];
        if (gsi >= io->gsi_base && gsi < io->gsi_base + io->pins) { *pin = gsi - io->gsi_base; return io; }
    }
    return NULL;
}
static void isa_redirect(unsigned line) {
    unsigned pin; const struct acpi_ioapic* io = ioapic_for(isa_gsi[line], &pin);
    if (io) ioapic_write(io, 0x10 + 2 * pin, isa_low[line]);
}
static void pic_apply(void) { port_byte_out(0x21, (uint8_t)pic_mask); port_byte_out(0xA1, (uint8_t)(pic_mask >> 8)); }

/* The mode is chosen once (apic_select: x2APIC kept from the firmware or asked for with "x2apic", else xAPIC) and
 * entered again after a system sleep, from whatever state the firmware left. */
static bool lapic_usable(void) {
    static bool selected;
    if (!selected) { selected = true; return apic_select(); }
    return apic_enter_mode();
}
static void lapic_nmi_inputs(void) {
    unsigned uid = 0xFFFFFFFFU;
    for (unsigned i = 0; i < acpi.cpu_count; i++) if (acpi.cpu_apic_ids[i] == bsp_apic) uid = acpi.cpu_uids[i];
    for (unsigned i = 0; i < acpi.nmi_count; i++) {
        const struct acpi_nmi* n = &acpi.nmis[i];
        if ((n->processor != 0xFFFFFFFFU && n->processor != uid) || n->lint > 1) continue;
        uint32_t value = 0x400 | ((n->flags & 3) == 3 ? 1U << 13 : 0);   /* NMI, active-low if flagged */
        if (n->lint == 0 && !ioapic_mode) continue;  /* PIC mode needs LINT0 as ExtINT */
        lapic_write(n->lint ? LAPIC_LVT_LINT1 : LAPIC_LVT_LINT0, value);
    }
}

static int add_action(unsigned vector, irq_handler_t handler, void* context) {
    struct irq_action* a = vectors[vector].actions;
    for (unsigned i = 0; i < IRQ_SHARED_MAX; i++) if (a[i].handler == handler && a[i].context == context) return 0;
    for (unsigned i = 0; i < IRQ_SHARED_MAX; i++) if (!a[i].handler) { a[i].context = context; a[i].handler = handler; return 0; }
    return -16;
}

void irq_init(void) {
    vectors = (void*)arena_alloc(256 * sizeof(struct vector_entry));
    if (!vectors) panic("IRQ vector table");
    for (unsigned v = 0; v < 0x40; v++) vector_state[v] = SLOT_FIXED;          /* exceptions, ISA, local */
    for (unsigned v = VEC_ISA_WAKE; v < VEC_ISA_WAKE + 16; v++) vector_state[v] = SLOT_FIXED;
    for (unsigned v = 0xF0; v < 256; v++) vector_state[v] = SLOT_FIXED;
    vector_state[VEC_SYSCALL] = SLOT_FIXED;
    bool lapic = lapic_usable();
    const char* why = !lapic ? "no usable local APIC" : !acpi.madt ? "no ACPI MADT" : !acpi.ioapic_count ? "MADT lists no IOAPIC" :
                      boot_option("noioapic") || boot_option("noapic") ? "disabled on the command line" : NULL;
    ioapic_mode = why == NULL;
    if (ioapic_mode) {
        for (unsigned i = 0; i < acpi.ioapic_count; i++) {
            struct acpi_ioapic* io = &acpi.ioapics[i];
            io->pins = (uint8_t)(((ioapic_read(io, 1) >> 16) & 0xFF) + 1);
            for (unsigned pin = 0; pin < io->pins; pin++) { ioapic_write(io, 0x10 + 2 * pin, 1U << 16); ioapic_write(io, 0x11 + 2 * pin, 0); }
        }
        port_byte_out(0x21, 0xFF); port_byte_out(0xA1, 0xFF); pic_mask = 0xFFFF;  /* remapped by cpu_tables, now silent */
    } else pic_mask = (uint16_t)(port_byte_in(0x21) | port_byte_in(0xA1) << 8);
    if (lapic) {
        bsp_apic = (uint8_t)apic_id_self();                             /* < 255: IOAPIC/MSI destinations are 8 bits */
        lapic_write(LAPIC_SVR, (lapic_read(LAPIC_SVR) & ~0xFFU) | 0x100 | VEC_SPURIOUS);
        lapic_write(LAPIC_TPR, 0);
        lapic_write(LAPIC_LVT_LINT0, ioapic_mode ? 1U << 16 : 0x700);   /* PIC mode: 8259 through virtual-wire ExtINT */
        lapic_write(LAPIC_LVT_LINT1, 0x400);                            /* NMI */
        lapic_nmi_inputs();
    }
    if (ioapic_mode) {
        vga_print("[64] IRQ mode: IOAPIC ("); print_number(acpi.ioapic_count); vga_print(" IOAPIC, ");
        print_number(acpi.ioapics[0].pins); vga_print(" pins at "); print_hex(acpi.ioapics[0].address);
        vga_print("), 8259 PIC masked, BSP APIC id "); print_number(bsp_apic); vga_print("\n");
    } else { vga_print("[64] IRQ mode: legacy 8259 PIC ("); vga_print(why); vga_print(")\n"); }
    if (irq_attach_isa(0, IRQ_WAKE, NULL, NULL) < 0) panic("cannot route the PIT tick");   /* 1 kHz, cpu_tables */
}
bool irq_ioapic_mode(void) { return ioapic_mode; }

int irq_attach_isa(unsigned line, unsigned flags, irq_handler_t handler, void* context) {
    if (!vectors || line >= 16 || line == 2) return -22;
    unsigned vector = ioapic_mode && (flags & IRQ_WAKE) ? VEC_ISA_WAKE + line : VEC_ISA + line;
    if (isa_vector[line] && isa_vector[line] != vector) return -16;
    if (handler) { int error = add_action(vector, handler, context); if (error) return error; }
    if (!isa_vector[line]) {
        if (ioapic_mode) {
            const struct acpi_override* o = &acpi.isa[line];
            unsigned polarity = o->present ? o->flags & 3 : 0, trigger = o->present ? (o->flags >> 2) & 3 : 0, pin;
            bool pci = (flags & IRQ_PCI_INTX) != 0;
            bool level = trigger == 3 || (trigger == 0 && pci), low = polarity == 3 || (polarity == 0 && pci);
            uint32_t gsi = o->present ? o->gsi : line;
            const struct acpi_ioapic* io = ioapic_for(gsi, &pin);
            if (!io) return -19;
            isa_gsi[line] = gsi;
            isa_low[line] = vector | (low ? 1U << 13 : 0) | (level ? 1U << 15 : 0) | (1U << 16); /* fixed, physical */
            ioapic_write(io, 0x11 + 2 * pin, (uint32_t)bsp_apic << 24);
            ioapic_write(io, 0x10 + 2 * pin, isa_low[line]);
        }
        isa_vector[line] = (uint8_t)vector; vector_state[vector] = SLOT_ISA;
    }
    irq_unmask(line);
    return (int)vector;
}
void irq_unmask(unsigned line) {
    if (line >= 16) return;
    if (ioapic_mode) { if (isa_vector[line]) { isa_low[line] &= ~(1U << 16); isa_redirect(line); } return; }
    pic_mask &= (uint16_t)~(1U << line);
    if (line >= 8) pic_mask &= (uint16_t)~(1U << 2);
    pic_apply();
}
void irq_mask(unsigned line) {
    if (line >= 16) return;
    if (ioapic_mode) { if (isa_vector[line]) { isa_low[line] |= 1U << 16; isa_redirect(line); } return; }
    pic_mask |= (uint16_t)(1U << line);
    pic_apply();
}
/* GSIs beyond the ISA lines (ACPI _PRT routes, GPE-block interrupts): explicit trigger/polarity, a dynamic
 * vector shared by every handler of that GSI. A GSI that is an ISA line (identity or MADT override) keeps
 * the ISA vector. */
#define GSI_ROUTES 32
static struct { uint32_t gsi, low; uint8_t vector; } gsi_routes[GSI_ROUTES];
static unsigned gsi_route_count;
int irq_attach_gsi(uint32_t gsi, unsigned flags, bool level, bool active_low, irq_handler_t handler, void* context) {
    if (!vectors || !handler) return -22;
    for (unsigned line = 0; line < 16; line++)
        if (line != 2 && (acpi.isa[line].present ? acpi.isa[line].gsi : line) == gsi) return irq_attach_isa(line, flags | IRQ_PCI_INTX, handler, context);
    if (!ioapic_mode) return -19;
    for (unsigned i = 0; i < gsi_route_count; i++) if (gsi_routes[i].gsi == gsi) {
        int error = add_action(gsi_routes[i].vector, handler, context); return error ? error : gsi_routes[i].vector;
    }
    unsigned pin; const struct acpi_ioapic* io = ioapic_for(gsi, &pin);
    if (!io || gsi_route_count == GSI_ROUTES) return -19;
    int vector = irq_alloc_vector(flags & IRQ_WAKE); if (vector < 0) return vector;
    if (add_action((unsigned)vector, handler, context)) { irq_free_vector((unsigned)vector); return -16; }
    uint32_t low = (uint32_t)vector | (active_low ? 1U << 13 : 0) | (level ? 1U << 15 : 0);  /* fixed, physical, unmasked */
    gsi_routes[gsi_route_count].gsi = gsi; gsi_routes[gsi_route_count].low = low; gsi_routes[gsi_route_count].vector = (uint8_t)vector; gsi_route_count++;
    ioapic_write(io, 0x11 + 2 * pin, (uint32_t)bsp_apic << 24); ioapic_write(io, 0x10 + 2 * pin, low);
    return vector;
}
/* PCI INTx: the ACPI _PRT route in IOAPIC mode (power.h acpi_pci_route), else the firmware's interrupt line. */
int irq_attach_pci(const pci_device_t* d, unsigned flags, irq_handler_t handler, void* context) {
    uint8_t pin = pci_read8(d->bus, d->device, d->function, 0x3D);
    uint32_t gsi; bool level, low;
    if (ioapic_mode && pin >= 1 && pin <= 4 && !acpi_pci_route(0, d->bus, d->device, pin - 1U, &gsi, &level, &low)) {
        int vector = irq_attach_gsi(gsi, flags, level, low, handler, context);
        if (vector >= 0) return vector;
    }
    unsigned line = d->irq_line;
    if (line < 3 || line >= 16 || line == 8 || line == 13) return -19;
    return irq_attach_isa(line, flags | IRQ_PCI_INTX, handler, context);
}
/* After a power loss (S3 wake, hibernation restore): local APIC, 8259s and every routed IOAPIC pin as
 * irq_init and the attach calls left them; MSI/MSI-X messages as recorded by pci_enable_msi(x). */
static struct { pci_device_t d; uint16_t entry; uint8_t vector, cpu, msix; } message_routes[64];
static unsigned message_count;
static void message_record(const pci_device_t* d, unsigned entry, unsigned vector, unsigned cpu, bool msix) {
    unsigned i = 0;
    while (i < message_count && !(message_routes[i].d.bus == d->bus && message_routes[i].d.device == d->device &&
           message_routes[i].d.function == d->function && message_routes[i].entry == entry && message_routes[i].msix == msix)) i++;
    if (i == 64) return;
    if (i == message_count) message_count++;
    message_routes[i].d = *d; message_routes[i].entry = (uint16_t)entry; message_routes[i].vector = (uint8_t)vector;
    message_routes[i].cpu = (uint8_t)cpu; message_routes[i].msix = msix;
}
static void pic_remap(void) {   /* the 8259 pair at vectors 0x20/0x28 (as cpu_tables), masks from pic_mask */
    port_byte_out(0x20, 0x11); port_byte_out(0xA0, 0x11); port_byte_out(0x21, VEC_ISA); port_byte_out(0xA1, VEC_ISA + 8);
    port_byte_out(0x21, 4); port_byte_out(0xA1, 2); port_byte_out(0x21, 1); port_byte_out(0xA1, 1);
    pic_apply();
}
void irq_resume(void) {
    pic_remap();
    if (lapic_usable()) {
        lapic_write(LAPIC_SVR, (lapic_read(LAPIC_SVR) & ~0xFFU) | 0x100 | VEC_SPURIOUS);
        lapic_write(LAPIC_TPR, 0);
        lapic_write(LAPIC_LVT_LINT0, ioapic_mode ? 1U << 16 : 0x700);
        lapic_write(LAPIC_LVT_LINT1, 0x400);
        lapic_nmi_inputs();
    }
    if (ioapic_mode) {
        for (unsigned i = 0; i < acpi.ioapic_count; i++)
            for (unsigned pin = 0; pin < acpi.ioapics[i].pins; pin++) { ioapic_write(&acpi.ioapics[i], 0x10 + 2 * pin, 1U << 16); ioapic_write(&acpi.ioapics[i], 0x11 + 2 * pin, 0); }
        for (unsigned line = 0; line < 16; line++) if (isa_vector[line]) {
            unsigned pin; const struct acpi_ioapic* io = ioapic_for(isa_gsi[line], &pin);
            if (io) { ioapic_write(io, 0x11 + 2 * pin, (uint32_t)bsp_apic << 24); ioapic_write(io, 0x10 + 2 * pin, isa_low[line]); }
        }
        for (unsigned i = 0; i < gsi_route_count; i++) {
            unsigned pin; const struct acpi_ioapic* io = ioapic_for(gsi_routes[i].gsi, &pin);
            if (io) { ioapic_write(io, 0x11 + 2 * pin, (uint32_t)bsp_apic << 24); ioapic_write(io, 0x10 + 2 * pin, gsi_routes[i].low); }
        }
    } else pic_apply();
}
void irq_resume_messages(void) {
    for (unsigned i = 0; i < message_count; i++) {
        if (message_routes[i].msix) pci_enable_msix(&message_routes[i].d, message_routes[i].entry, message_routes[i].vector, message_routes[i].cpu);
        else pci_enable_msi(&message_routes[i].d, message_routes[i].vector, message_routes[i].cpu);
    }
}
int irq_alloc_vector(unsigned flags) {
    if (!vectors) return -22;
    unsigned first = flags & IRQ_WAKE ? VEC_WAKE_FIRST : VEC_DYNAMIC_FIRST, last = flags & IRQ_WAKE ? VEC_WAKE_LAST : VEC_DYNAMIC_LAST;
    for (unsigned v = first; v <= last; v++) if (vector_state[v] == SLOT_FREE) {
        vector_state[v] = SLOT_DYNAMIC; memset(&vectors[v], 0, sizeof(vectors[v])); return (int)v;
    }
    return -28;
}
void irq_free_vector(unsigned vector) {
    if (!vectors || vector >= 256 || vector_state[vector] != SLOT_DYNAMIC) return;
    memset(&vectors[vector], 0, sizeof(vectors[vector])); vector_state[vector] = SLOT_FREE;
}
int irq_register(unsigned vector, irq_handler_t handler, void* context) {
    if (!vectors || vector >= 256 || !handler || (vector_state[vector] != SLOT_DYNAMIC && vector_state[vector] != SLOT_ISA)) return -22;
    return add_action(vector, handler, context);
}
int irq_unregister(unsigned vector, irq_handler_t handler, void* context) {
    if (!vectors || vector >= 256) return -22;
    struct irq_action* a = vectors[vector].actions;
    for (unsigned i = 0; i < IRQ_SHARED_MAX; i++) if (a[i].handler == handler && a[i].context == context) { a[i].handler = NULL; a[i].context = NULL; return 0; }
    return -2;
}
bool irq_device_vector(unsigned vector) {
    return (vector >= VEC_ISA && vector < VEC_ISA + 16) || (vector >= VEC_DYNAMIC_FIRST && vector <= VEC_WAKE_LAST && vector != VEC_SYSCALL);
}
void irq_dispatch(unsigned vector) {
    if (!vectors || vector >= 256) return;
    const struct irq_action* a = vectors[vector].actions;
    for (unsigned i = 0; i < IRQ_SHARED_MAX; i++) if (a[i].handler) a[i].handler(vector, a[i].context);
}
void irq_eoi(unsigned vector) {
    if (!ioapic_mode && vector >= VEC_ISA && vector < VEC_ISA + 16) {
        if (vector >= VEC_ISA + 8) port_byte_out(0xA0, 0x20);
        port_byte_out(0x20, 0x20);
    } else lapic_eoi();
}
/* Disk waits on the BSP: only the tick, IDE and IRQ_WAKE vectors may interrupt (NIC/input handlers
 * must not run in the middle of a syscall). TPR blocks every vector below 0x90 (the 8259's ExtINT
 * delivery ignores TPR, so PIC mode additionally masks all lines but 0, 2 and 14). */
void irq_wait_restricted(void) {
    if (!ioapic_mode) { port_byte_out(0x21, (uint8_t)(pic_mask | 0xFA)); port_byte_out(0xA1, (uint8_t)((pic_mask >> 8) | 0xBF)); }
    __asm__ volatile("mov %0,%%cr8; sti; hlt; cli; mov %1,%%cr8" :: "r"(TPR_RESTRICTED), "r"(0ULL) : "memory");
    if (!ioapic_mode) pic_apply();
}

/* ---- PCI capabilities, MSI, MSI-X ---- */
static void config_write16(const pci_device_t* d, uint8_t offset, uint16_t value) {
    uint32_t word = pci_read32(d->bus, d->device, d->function, offset & 0xFC);
    unsigned shift = (offset & 2) * 8;
    word = (word & ~(0xFFFFU << shift)) | ((uint32_t)value << shift);
    pci_write32(d->bus, d->device, d->function, offset & 0xFC, word);
}
/* Memory decoding, bus mastering, INTx disable. The status half is written as zero (its bits are
 * write-1-to-clear). */
static void command_enable(const pci_device_t* d, uint16_t bits) {
    uint16_t command = pci_read16(d->bus, d->device, d->function, PCI_COMMAND);
    pci_write32(d->bus, d->device, d->function, PCI_COMMAND, (uint32_t)(command | bits));
}
uint8_t pci_find_capability(const pci_device_t* d, uint8_t id) {
    if (!d || !(pci_read16(d->bus, d->device, d->function, PCI_STATUS) & 0x10)) return 0;
    uint8_t at = pci_read8(d->bus, d->device, d->function, 0x34) & 0xFC;
    for (unsigned guard = 0; at >= 0x40 && guard < 48; guard++) {
        if (pci_read8(d->bus, d->device, d->function, at) == id) return at;
        at = pci_read8(d->bus, d->device, d->function, at + 1) & 0xFC;
    }
    return 0;
}
uint64_t pci_bar_address(const pci_device_t* d, unsigned bar) {
    if (!d || bar > 5) return 0;
    uint32_t low = pci_read32(d->bus, d->device, d->function, PCI_BAR0 + bar * 4);
    if (low & 1) return 0;
    uint64_t address = low & ~0xFULL;
    if (((low >> 1) & 3) == 2 && bar < 5) address |= (uint64_t)pci_read32(d->bus, d->device, d->function, PCI_BAR0 + (bar + 1) * 4) << 32;
    return address;
}
static bool message_target(unsigned vector, unsigned cpu, uint32_t* address) {
    if (vector < VEC_DYNAMIC_FIRST || vector > VEC_WAKE_LAST || vector == VEC_SYSCALL || cpu >= MAX_CPUS || (cpu && !cpus[cpu].online)) return false;
    if (cpu && cpus[cpu].apic_id > 0xFE) cpu = 0;                       /* 8-bit MSI destination (no interrupt remapping) */
    *address = 0xFEE00000U | ((uint32_t)(cpu ? cpus[cpu].apic_id : bsp_apic) << 12);   /* physical, no redirection */
    return true;
}
int pci_enable_msi(pci_device_t* d, unsigned vector, unsigned cpu) {
    uint32_t address;
    if (!d || !message_target(vector, cpu, &address)) return -22;
    uint8_t cap = pci_find_capability(d, 0x05);
    if (!cap) return -95;
    uint16_t control = pci_read16(d->bus, d->device, d->function, cap + 2);
    config_write16(d, cap + 2, control & ~0x71U);                       /* off, one message */
    pci_write32(d->bus, d->device, d->function, cap + 4, address);
    if (control & 0x80) {                                                /* 64-bit address */
        pci_write32(d->bus, d->device, d->function, cap + 8, 0);
        config_write16(d, cap + 12, (uint16_t)vector);                   /* edge, fixed delivery */
        if (control & 0x100) pci_write32(d->bus, d->device, d->function, cap + 16, 0);
    } else {
        config_write16(d, cap + 8, (uint16_t)vector);
        if (control & 0x100) pci_write32(d->bus, d->device, d->function, cap + 12, 0);
    }
    command_enable(d, 0x0406);
    config_write16(d, cap + 2, (uint16_t)((control & ~0x70U) | 1));
    message_record(d, 0, vector, cpu, false);
    return 0;
}
int pci_disable_msi(pci_device_t* d) {
    uint8_t cap = pci_find_capability(d, 0x05);
    if (!cap) return -95;
    config_write16(d, cap + 2, pci_read16(d->bus, d->device, d->function, cap + 2) & ~1U);
    return 0;
}
int pci_msix_count(const pci_device_t* d) {
    uint8_t cap = pci_find_capability(d, 0x11);
    return cap ? (int)(pci_read16(d->bus, d->device, d->function, cap + 2) & 0x7FF) + 1 : -95;
}
int pci_enable_msix(pci_device_t* d, unsigned entry, unsigned vector, unsigned cpu) {
    uint32_t address;
    if (!d || !message_target(vector, cpu, &address)) return -22;
    uint8_t cap = pci_find_capability(d, 0x11);
    if (!cap) return -95;
    uint16_t control = pci_read16(d->bus, d->device, d->function, cap + 2);
    unsigned entries = (control & 0x7FF) + 1;
    if (entry >= entries) return -22;
    uint32_t table = pci_read32(d->bus, d->device, d->function, cap + 4);
    uint64_t bar = pci_bar_address(d, table & 7);
    if (!bar) return -6;
    command_enable(d, 0x0406);                                           /* the table lives in memory space */
    volatile uint32_t* slot = (volatile uint32_t*)mmio_map(bar + (table & ~7U), (uint64_t)entries * 16, MMIO_UNCACHED);
    if (!slot) return -12;
    slot += entry * 4;
    config_write16(d, cap + 2, (uint16_t)(control | 0xC000));            /* enabled, function masked */
    slot[3] |= 1;
    slot[0] = address; slot[1] = 0; slot[2] = vector;
    slot[3] &= ~1U;
    config_write16(d, cap + 2, (uint16_t)((control | 0x8000) & ~0x4000));
    message_record(d, entry, vector, cpu, true);
    return 0;
}
int pci_disable_msix(pci_device_t* d) {
    uint8_t cap = pci_find_capability(d, 0x11);
    if (!cap) return -95;
    config_write16(d, cap + 2, pci_read16(d->bus, d->device, d->function, cap + 2) & ~0x8000U);
    return 0;
}

/* ---- kernel MMIO window (PML4 slot 510; slot 511 holds the kernel image) ---- */
#define WINDOW_BASE 0xFFFFFF0000000000ULL
#define WINDOW_END 0xFFFFFF7FFFFFF000ULL
#define WINDOW_MAPS 32
extern uint64_t boot_pml4[];
static uint64_t* window_pdpt;
static uint64_t window_next;
static struct { uint64_t physical, end, virt; } window_maps[WINDOW_MAPS];
static unsigned window_count;
static bool pat_write_combining;
static uint64_t* window_table(uint64_t* table, unsigned index) {
    if (!(table[index] & 1)) { uint64_t frame = frame_alloc(); if (!frame) return NULL; table[index] = frame | 3; }
    return phys_to_virt(table[index] & 0x000FFFFFFFFFF000ULL);
}
volatile void* mmio_map(uint64_t physical, uint64_t size, unsigned cache) {
    if (!size || physical + size < physical || physical + size > (1ULL << 52)) return NULL;
    if (physical + size <= 0x100000000ULL && (cache != MMIO_WRITE_COMBINED || !pat_write_combining || !window_pdpt))
        return phys_to_virt(physical);                                        /* direct map of the low 4 GiB (WB PAT; MTRR decides) */
    if (!window_pdpt) return NULL;
    for (unsigned i = 0; i < window_count; i++)
        if (physical >= window_maps[i].physical && physical + size <= window_maps[i].end) return (volatile void*)(window_maps[i].virt + physical - window_maps[i].physical);
    uint64_t first = physical & ~0xFFFULL, last = (physical + size + 0xFFF) & ~0xFFFULL, pages = (last - first) >> 12;
    if (window_count == WINDOW_MAPS || pages > (WINDOW_END - window_next) / 4096 - 1) return NULL;
    uint64_t flags = 3 | (1ULL << 63) | (cache == MMIO_WRITE_COMBINED && pat_write_combining ? 0x98 : 0x18);
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t va = window_next + p * 4096;
        uint64_t* directory = window_table(window_pdpt, (va >> 30) & 511);
        uint64_t* table = directory ? window_table(directory, (va >> 21) & 511) : NULL;
        if (!table) return NULL;
        table[(va >> 12) & 511] = (first + p * 4096) | flags;
    }
    window_maps[window_count].physical = first; window_maps[window_count].end = last; window_maps[window_count].virt = window_next;
    window_count++;
    uint64_t virt = window_next;
    window_next += (pages + 1) * 4096;                                        /* unmapped guard page between mappings */
    return (volatile void*)(virt + (physical - first));
}

/* After memory_init: the window's top-level table must exist before the first address space is
 * cloned from boot_pml4 (space_create copies the kernel's PML4 entries 256-511). */
void platform_init(void) {
    window_pdpt = (void*)page_alloc();
    if (!window_pdpt) panic("MMIO window page table");
    boot_pml4[510] = virt_to_phys(window_pdpt) | 3;
    window_next = WINDOW_BASE;
    uint32_t ecx, edx; cpuid1(&ecx, &edx); pat_write_combining = (edx & (1U << 16)) != 0;
    irq_init();
}
