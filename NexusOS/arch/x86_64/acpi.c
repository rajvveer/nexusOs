/* ACPI table discovery and power control, without an AML interpreter.
 *
 * RSDP: boot_info.rsdp (UEFI configuration table) or, on BIOS boots, a scan of the first KiB of
 * the EBDA and 0xE0000-0xFFFFF. Tables come from the XSDT (revision >= 2) or the RSDT; every
 * table's checksum is verified before use. Parsed: MADT (local APICs, IOAPICs, interrupt source
 * overrides, NMI inputs), FADT (PM1a/PM1b control, reset register, SMI command), MCFG (PCIe ECAM),
 * and the DSDT/SSDT \_S5 package, located by a bounded AML byte scan for
 * NameOp "_S5_" PackageOp PkgLength NumElements <SLP_TYPa> <SLP_TYPb>.
 *
 * Tables are read through the kernel's direct map of the low 4 GiB (device memory included): tables
 * placed above 4 GiB are reported and skipped. */
#include "native.h"
#include "acpi.h"
#include "boot_info.h"
#include "power.h"
#include "../../kernel/ata.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

struct acpi_info acpi;
static uint64_t aml_bytes;   /* DSDT + SSDTs: sizes the interpreter heap (acpi_os.c) */
static uint8_t rtc_day_alarm;
uint8_t acpi_rtc_day_alarm(void) { return rtc_day_alarm >= 0x0E && rtc_day_alarm < 0x80 ? rtc_day_alarm : 0; }
#define IDENTITY_LIMIT 0x100000000ULL
struct sdt { char signature[4]; uint32_t length; uint8_t revision, checksum; char oem[6], table[8]; uint32_t oem_revision, creator, creator_revision; } __attribute__((packed));

static void print_hex(uint64_t value) {
    char text[19]; text[0] = '0'; text[1] = 'x';
    unsigned digits = 1; while (digits < 16 && (value >> (4 * digits))) digits++;
    for (unsigned i = 0; i < digits; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (4 * (digits - 1 - i))) & 15];
    text[2 + digits] = 0; vga_print(text);
}
static void print_number(uint64_t value) { char text[12]; uint_to_str((uint32_t)value, text); vga_print(text); }
static bool checksum_ok(const void* data, uint64_t length) {
    const uint8_t* p = data; uint8_t sum = 0;
    for (uint64_t i = 0; i < length; i++) sum += p[i];
    return sum == 0;
}
static const struct sdt* table_at(uint64_t address) {
    if (!address || address >= IDENTITY_LIMIT - sizeof(struct sdt)) {
        if (address) { vga_print("[64] ACPI table above 4 GiB skipped at "); print_hex(address); vga_print("\n"); }
        return NULL;
    }
    const struct sdt* table = phys_to_virt(address);
    if (table->length < sizeof(struct sdt) || table->length > 0x1000000 || address + table->length > IDENTITY_LIMIT) return NULL;
    if (!checksum_ok(table, table->length)) {
        vga_print("[64] ACPI checksum mismatch: "); char name[5] = {table->signature[0], table->signature[1], table->signature[2], table->signature[3], 0};
        vga_print(name); vga_print("\n"); return NULL;
    }
    return table;
}
static const uint8_t* rsdp_scan(uint64_t begin, uint64_t end) {
    for (uint64_t at = begin & ~15ULL; at + 20 <= end; at += 16) {
        const uint8_t* p = phys_to_virt(at);
        if (!memcmp(p, "RSD PTR ", 8) && checksum_ok(p, 20)) return p;
    }
    return NULL;
}
static const uint8_t* rsdp_find(void) {
    if (boot_info.rsdp && boot_info.rsdp < IDENTITY_LIMIT - 36) {
        const uint8_t* p = phys_to_virt(boot_info.rsdp);
        if (!memcmp(p, "RSD PTR ", 8) && checksum_ok(p, 20)) return p;
        vga_print("[64] ACPI: boot-info RSDP is invalid\n");
    }
    uint64_t ebda = (uint64_t)*(volatile uint16_t*)phys_to_virt(0x40E) << 4;
    const uint8_t* found = ebda >= 0x80000 && ebda < 0xA0000 ? rsdp_scan(ebda, ebda + 1024) : NULL;
    return found ? found : rsdp_scan(0xE0000, 0x100000);
}

static void gas_from(struct acpi_gas* gas, const uint8_t* p) {
    gas->space = p[0]; gas->width = p[1]; gas->offset = p[2]; gas->access = p[3];
    memcpy(&gas->address, p + 4, 8);
}
static void parse_madt(const struct sdt* table) {
    const uint8_t* p = (const void*)table; uint32_t length = table->length;
    if (length < 44) return;
    acpi.madt = true;
    memcpy(&acpi.lapic_address, p + 36, 4); memcpy(&acpi.madt_flags, p + 40, 4);
    for (uint32_t at = 44; at + 2 <= length; ) {
        uint8_t type = p[at], size = p[at + 1];
        if (size < 2 || at + size > length) break;
        const uint8_t* e = p + at;
        if (type == 0 && size >= 8) {                       /* processor local APIC */
            uint32_t flags; memcpy(&flags, e + 4, 4);
            if ((flags & 1) && acpi.cpu_count < ACPI_MAX_CPUS) { acpi.cpu_uids[acpi.cpu_count] = e[2]; acpi.cpu_apic_ids[acpi.cpu_count++] = e[3]; }
        } else if (type == 9 && size >= 16) {               /* processor local x2APIC */
            uint32_t id, flags, uid; memcpy(&id, e + 4, 4); memcpy(&flags, e + 8, 4); memcpy(&uid, e + 12, 4);
            bool duplicate = false;
            for (unsigned i = 0; i < acpi.cpu_count; i++) if (acpi.cpu_apic_ids[i] == id) duplicate = true;
            if ((flags & 1) && id < 0xFF && !duplicate && acpi.cpu_count < ACPI_MAX_CPUS) { acpi.cpu_uids[acpi.cpu_count] = (uint8_t)uid; acpi.cpu_apic_ids[acpi.cpu_count++] = (uint8_t)id; }
        } else if (type == 1 && size >= 12 && acpi.ioapic_count < ACPI_MAX_IOAPICS) {
            struct acpi_ioapic* io = &acpi.ioapics[acpi.ioapic_count++];
            io->id = e[2]; memcpy(&io->address, e + 4, 4); memcpy(&io->gsi_base, e + 8, 4);
        } else if (type == 2 && size >= 10) {               /* interrupt source override (bus 0 = ISA) */
            if (e[2] == 0 && e[3] < 16) {
                struct acpi_override* o = &acpi.isa[e[3]];
                memcpy(&o->gsi, e + 4, 4); memcpy(&o->flags, e + 8, 2); o->present = true; acpi.override_count++;
            }
        } else if (type == 4 && size >= 6 && acpi.nmi_count < ACPI_MAX_NMIS) { /* local APIC NMI */
            struct acpi_nmi* n = &acpi.nmis[acpi.nmi_count++];
            n->processor = e[2] == 0xFF ? 0xFFFFFFFFU : e[2]; memcpy(&n->flags, e + 3, 2); n->lint = e[5];
        } else if (type == 0xA && size >= 12 && acpi.nmi_count < ACPI_MAX_NMIS) { /* local x2APIC NMI */
            struct acpi_nmi* n = &acpi.nmis[acpi.nmi_count++];
            memcpy(&n->flags, e + 2, 2); memcpy(&n->processor, e + 4, 4); n->lint = e[8];
        } else if (type == 5 && size >= 12) {               /* 64-bit local APIC address override */
            uint64_t address; memcpy(&address, e + 4, 8);
            if (address < IDENTITY_LIMIT) acpi.lapic_address = (uint32_t)address;
        }
        at += size;
    }
}
static void parse_fadt(const struct sdt* table) {
    const uint8_t* p = (const void*)table; uint32_t length = table->length;
    if (length < 116) return;
    acpi.fadt = true; acpi.fadt_revision = table->revision;
    uint32_t dsdt32, pm1a, pm1b; memcpy(&dsdt32, p + 40, 4); memcpy(&acpi.smi_command, p + 48, 4);
    acpi.acpi_enable = p[52]; memcpy(&pm1a, p + 64, 4); memcpy(&pm1b, p + 68, 4);
    memcpy(&acpi.boot_arch, p + 109, 2); memcpy(&acpi.fadt_flags, p + 112, 4);
    acpi.dsdt = dsdt32;
    rtc_day_alarm = p[106];   /* DAY_ALRM: CMOS index of the day-of-month alarm (0 = none) */
    acpi.pm1a_control = (struct acpi_gas){1, 16, 0, 2, pm1a};
    acpi.pm1b_control = (struct acpi_gas){1, 16, 0, 2, pm1b};
    if (length >= 129) { gas_from(&acpi.reset, p + 116); acpi.reset_value = p[128]; }
    if (length >= 148) { uint64_t x; memcpy(&x, p + 140, 8); if (x) acpi.dsdt = x; }
    struct acpi_gas x;
    if (length >= 184) { gas_from(&x, p + 172); if (x.address && x.space <= 1) acpi.pm1a_control = x; }
    if (length >= 196) { gas_from(&x, p + 184); if (x.address && x.space <= 1) acpi.pm1b_control = x; }
    if (length >= 256) gas_from(&acpi.sleep_control, p + 244);
}
static void parse_mcfg(const struct sdt* table) {
    const uint8_t* p = (const void*)table;
    for (uint32_t at = 44; at + 16 <= table->length && acpi.mcfg_count < ACPI_MAX_MCFG; at += 16) {
        struct acpi_mcfg* m = &acpi.mcfg[acpi.mcfg_count++];
        memcpy(&m->base, p + at, 8); memcpy(&m->segment, p + at + 8, 2); m->start_bus = p[at + 10]; m->end_bus = p[at + 11];
    }
}
/* \_S5 is normally a static package: Name (_S5, Package (n) { SLP_TYPa, SLP_TYPb, ... }). */
static bool aml_constant(const uint8_t* aml, uint32_t length, uint32_t* at, uint8_t* value) {
    uint32_t p = *at; if (p >= length) return false;
    uint8_t op = aml[p];
    if (op == 0x00 || op == 0x01) { *value = op; *at = p + 1; return true; }            /* ZeroOp, OneOp */
    if (op == 0x0A && p + 1 < length) { *value = aml[p + 1]; *at = p + 2; return true; } /* BytePrefix */
    if (op == 0x0B && p + 2 < length) { *value = aml[p + 1]; *at = p + 3; return true; } /* WordPrefix */
    if (op == 0x0C && p + 4 < length) { *value = aml[p + 1]; *at = p + 5; return true; } /* DWordPrefix */
    return false;
}
static bool aml_find_s5(const struct sdt* table) {
    const uint8_t* aml = (const uint8_t*)table + sizeof(struct sdt);
    uint32_t length = table->length - sizeof(struct sdt);
    for (uint32_t i = 1; i + 4 < length; i++) {
        if (aml[i] != '_' || aml[i + 1] != 'S' || aml[i + 2] != '5' || aml[i + 3] != '_') continue;
        if (!(aml[i - 1] == 0x08 || (i >= 2 && aml[i - 1] == '\\' && aml[i - 2] == 0x08))) continue;
        uint32_t p = i + 4;
        if (p >= length || aml[p] != 0x12) continue;                                     /* PackageOp */
        p++; if (p >= length) continue;
        p += (uint32_t)(aml[p] >> 6) + 1;                                                 /* PkgLength */
        if (p >= length) continue;
        uint8_t count = aml[p++], a, b;
        if (!count || !aml_constant(aml, length, &p, &a)) continue;
        if (count < 2 || !aml_constant(aml, length, &p, &b)) b = a;
        acpi.s5 = true; acpi.s5_a = a & 7; acpi.s5_b = b & 7;
        return true;
    }
    return false;
}

void acpi_init(void) {
    const uint8_t* rsdp = rsdp_find();
    if (!rsdp) { vga_print("[64] ACPI: no RSDP (legacy PIC, fw_cfg CPU count, isa-debug-exit only)\n"); return; }
    acpi.present = true; acpi.rsdp = virt_to_phys(rsdp); acpi.revision = rsdp[15];
    uint32_t rsdt; memcpy(&rsdt, rsdp + 16, 4);
    uint64_t xsdt = 0;
    if (acpi.revision >= 2) {
        uint32_t length; memcpy(&length, rsdp + 20, 4);
        if (length >= 36 && length < 4096 && checksum_ok(rsdp, length)) memcpy(&xsdt, rsdp + 24, 8);
        else vga_print("[64] ACPI: extended RSDP checksum mismatch; using the RSDT\n");
    }
    const struct sdt* root = xsdt ? table_at(xsdt) : NULL;
    if (root && memcmp(root->signature, "XSDT", 4)) root = NULL;
    acpi.xsdt = root != NULL;
    if (!root) { root = table_at(rsdt); if (root && memcmp(root->signature, "RSDT", 4)) root = NULL; }
    if (!root) { vga_print("[64] ACPI: no valid RSDT/XSDT\n"); acpi.present = false; return; }
    unsigned width = acpi.xsdt ? 8 : 4, entries = (root->length - sizeof(struct sdt)) / width;
    const uint8_t* list = (const uint8_t*)root + sizeof(struct sdt);
    const struct sdt* ssdts[16]; unsigned ssdt_count = 0;
    for (unsigned i = 0; i < entries; i++) {
        uint64_t address = 0; memcpy(&address, list + i * width, width);
        const struct sdt* table = table_at(address);
        if (!table) continue;
        acpi.tables++;
        if (!memcmp(table->signature, "APIC", 4) && !acpi.madt) parse_madt(table);
        else if (!memcmp(table->signature, "FACP", 4) && !acpi.fadt) parse_fadt(table);
        else if (!memcmp(table->signature, "MCFG", 4) && !acpi.mcfg_count) parse_mcfg(table);
        else if (!memcmp(table->signature, "SSDT", 4) && ssdt_count < 16) ssdts[ssdt_count++] = table;
    }
    for (unsigned i = 0; i < ssdt_count; i++) aml_bytes += ssdts[i]->length;
    if (acpi.fadt) {
        const struct sdt* dsdt = table_at(acpi.dsdt);
        if (dsdt && !memcmp(dsdt->signature, "DSDT", 4)) aml_bytes += dsdt->length;
        if (dsdt && !memcmp(dsdt->signature, "DSDT", 4)) aml_find_s5(dsdt);
        for (unsigned i = 0; i < ssdt_count && !acpi.s5; i++) aml_find_s5(ssdts[i]);
    }
    vga_print("[64] ACPI rev="); print_number(acpi.revision); vga_print(acpi.xsdt ? " XSDT" : " RSDT");
    vga_print(" tables="); print_number(acpi.tables); vga_print(" rsdp="); print_hex(acpi.rsdp); vga_print("\n");
    if (acpi.madt) {
        vga_print("[64] ACPI MADT cpus="); print_number(acpi.cpu_count); vga_print(" ioapics="); print_number(acpi.ioapic_count);
        for (unsigned i = 0; i < acpi.ioapic_count; i++) { vga_print(" ["); print_hex(acpi.ioapics[i].address); vga_print(" gsi "); print_number(acpi.ioapics[i].gsi_base); vga_print("]"); }
        vga_print(" overrides="); print_number(acpi.override_count);
        for (unsigned irq = 0; irq < 16; irq++) if (acpi.isa[irq].present) {
            vga_print(" "); print_number(irq); vga_print("->"); print_number(acpi.isa[irq].gsi); vga_print("/"); print_hex(acpi.isa[irq].flags);
        }
        vga_print(" nmi="); print_number(acpi.nmi_count); vga_print(" lapic="); print_hex(acpi.lapic_address);
        vga_print(acpi.madt_flags & 1 ? " pcat\n" : "\n");
    }
    if (acpi.fadt) {
        vga_print("[64] ACPI FADT rev="); print_number(acpi.fadt_revision);
        vga_print(" pm1a="); print_hex(acpi.pm1a_control.address); vga_print(" pm1b="); print_hex(acpi.pm1b_control.address);
        vga_print(" reset="); print_hex(acpi.reset.address); vga_print("/"); print_hex(acpi.reset_value);
        vga_print(acpi.fadt_flags & ACPI_FADT_RESET_REG_SUP ? " (supported)" : " (unsupported)");
        vga_print(" S5="); if (acpi.s5) { print_number(acpi.s5_a); vga_print("/"); print_number(acpi.s5_b); } else vga_print("none");
        vga_print("\n");
    }
    for (unsigned i = 0; i < acpi.mcfg_count; i++) {
        vga_print("[64] ACPI MCFG ecam="); print_hex(acpi.mcfg[i].base); vga_print(" segment="); print_number(acpi.mcfg[i].segment);
        vga_print(" buses="); print_number(acpi.mcfg[i].start_bus); vga_print("-"); print_number(acpi.mcfg[i].end_bus); vga_print("\n");
    }
}

uint64_t acpi_table_rsdp(void) { return acpi.present ? acpi.rsdp : 0; }
uint64_t acpi_table_aml_bytes(void) { return aml_bytes; }

volatile void* pci_ecam_address(unsigned segment, unsigned bus, unsigned device, unsigned function) {
    for (unsigned i = 0; i < acpi.mcfg_count; i++) {
        const struct acpi_mcfg* m = &acpi.mcfg[i];
        if (m->segment != segment || bus < m->start_bus || bus > m->end_bus || device > 31 || function > 7) continue;
        uint64_t offset = ((uint64_t)(bus - m->start_bus) << 20) | ((uint64_t)device << 15) | ((uint64_t)function << 12);
        return mmio_map(m->base + offset, 4096, MMIO_UNCACHED);
    }
    return NULL;
}

/* ---- power control ---- */
static uint64_t gas_read(const struct acpi_gas* gas) {
    unsigned bits = gas->width ? gas->width : 16;
    if (gas->space == 1) {
        uint16_t port = (uint16_t)gas->address;
        return bits <= 8 ? port_byte_in(port) : bits <= 16 ? port_word_in(port) : port_dword_in(port);
    }
    if (gas->space == 0 && gas->address < IDENTITY_LIMIT) {
        volatile void* r = phys_to_virt(gas->address);
        if (bits <= 8) return *(volatile uint8_t*)r;
        if (bits <= 16) return *(volatile uint16_t*)r;
        if (bits <= 32) return *(volatile uint32_t*)r;
        return *(volatile uint64_t*)r;
    }
    return 0;
}
static void gas_write(const struct acpi_gas* gas, uint64_t value) {
    unsigned bits = gas->width ? gas->width : 8;
    if (gas->space == 1) {
        uint16_t port = (uint16_t)gas->address;
        if (bits <= 8) port_byte_out(port, (uint8_t)value);
        else if (bits <= 16) port_word_out(port, (uint16_t)value);
        else port_dword_out(port, (uint32_t)value);
    } else if (gas->space == 0 && gas->address < IDENTITY_LIMIT) {
        volatile void* r = phys_to_virt(gas->address);
        if (bits <= 8) *(volatile uint8_t*)r = (uint8_t)value;
        else if (bits <= 16) *(volatile uint16_t*)r = (uint16_t)value;
        else if (bits <= 32) *(volatile uint32_t*)r = (uint32_t)value;
        else *(volatile uint64_t*)r = value;
    } else if (gas->space == 2) {                       /* PCI configuration space, segment 0 bus 0 */
        uint8_t device = (uint8_t)(gas->address >> 32), function = (uint8_t)(gas->address >> 16), offset = (uint8_t)gas->address;
        uint32_t word = pci_read32(0, device, function, offset & 0xFC);
        unsigned shift = (offset & 3) * 8;
        word = (word & ~(0xFFU << shift)) | ((uint32_t)(uint8_t)value << shift);
        pci_write32(0, device, function, offset & 0xFC, word);
    }
}
static void settle(unsigned spins) { for (unsigned i = 0; i < spins; i++) port_byte_in(0x80); } /* ~1 us per port read */
void acpi_poweroff(void) {
    acpi_poweroff_aml();   /* _PTS(5), _SST and \_S5 through the interpreter; returns only if that failed */
    if (!acpi.fadt || !acpi.s5) { vga_print("[64] ACPI power-off unavailable (no FADT or \\_S5)\n"); return; }
    bool reduced = (acpi.fadt_flags & ACPI_FADT_HW_REDUCED) != 0;
    if (reduced && acpi.sleep_control.address) {
        vga_print("[64] ACPI S5 power-off via sleep control register\n");
        gas_write(&acpi.sleep_control, ((uint64_t)(acpi.s5_a & 7) << 2) | (1u << 5));
        settle(3000000); return;
    }
    if (!acpi.pm1a_control.address) { vga_print("[64] ACPI power-off unavailable (no PM1a control block)\n"); return; }
    /* Leave legacy (SMM) mode first if the firmware has not handed the hardware to the OS. */
    if (!(gas_read(&acpi.pm1a_control) & 1) && acpi.smi_command && acpi.acpi_enable) {
        port_byte_out((uint16_t)acpi.smi_command, acpi.acpi_enable);
        for (unsigned i = 0; i < 3000 && !(gas_read(&acpi.pm1a_control) & 1); i++) settle(1000);
    }
    vga_print("[64] ACPI S5 power-off: SLP_TYPa="); print_number(acpi.s5_a); vga_print(" SLP_TYPb="); print_number(acpi.s5_b); vga_print("\n");
    __asm__ volatile("cli; wbinvd" ::: "memory");
    uint64_t a = gas_read(&acpi.pm1a_control) & ~((7ULL << 10) | (1ULL << 13)), b = 0;
    if (acpi.pm1b_control.address) b = gas_read(&acpi.pm1b_control) & ~((7ULL << 10) | (1ULL << 13));
    gas_write(&acpi.pm1a_control, a | ((uint64_t)acpi.s5_a << 10));
    if (acpi.pm1b_control.address) gas_write(&acpi.pm1b_control, b | ((uint64_t)acpi.s5_b << 10));
    gas_write(&acpi.pm1a_control, a | ((uint64_t)acpi.s5_a << 10) | (1ULL << 13));
    if (acpi.pm1b_control.address) gas_write(&acpi.pm1b_control, b | ((uint64_t)acpi.s5_b << 10) | (1ULL << 13));
    settle(3000000);
    vga_print("[64] ACPI S5 did not power off\n");
}
void acpi_reset(void) {
    acpi_reboot_aml();     /* _TTS(5) and the FADT reset register through uACPI (I/O, memory or PCI space) */
    __asm__ volatile("cli" ::: "memory");
    if (acpi.fadt && (acpi.fadt_flags & ACPI_FADT_RESET_REG_SUP) && acpi.reset.address && acpi.reset.space <= 2) {
        vga_print("[64] ACPI reset via FADT reset register "); print_hex(acpi.reset.address); vga_print("="); print_hex(acpi.reset_value); vga_print("\n");
        gas_write(&acpi.reset, acpi.reset_value);
        settle(500000);
    }
    vga_print("[64] reset via keyboard controller\n");
    for (unsigned i = 0; i < 100000 && (port_byte_in(0x64) & 2); i++) settle(1);
    port_byte_out(0x64, 0xFE);
    settle(500000);
    vga_print("[64] reset via triple fault\n");
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) empty = {0, 0};
    __asm__ volatile("lidt %0; int3" :: "m"(empty));
    for (;;) __asm__ volatile("hlt");
}

static bool qemu_fw_cfg(void) {
    port_word_out(0x510, 0);
    return port_byte_in(0x511) == 'Q' && port_byte_in(0x511) == 'E' && port_byte_in(0x511) == 'M' && port_byte_in(0x511) == 'U';
}
/* Final kernel exit (kernel.c finish): QEMU's isa-debug-exit when present keeps the test exit codes
 * (status = code * 2 + 1); otherwise a successful run powers the machine off through ACPI. Failures
 * halt so the message stays on screen. */
void platform_shutdown(unsigned code) {
    if (qemu_fw_cfg()) __asm__ volatile("outl %0,%1" :: "a"(code), "Nd"((uint16_t)0xF4));
    if (code == 0x10) {
        ata_flush();
        if (power_finish_reboot) { vga_print("[64] SHUTDOWN: restart requested\n"); acpi_reset(); }
        vga_print("[64] SHUTDOWN: no isa-debug-exit device; ACPI power-off\n");
        acpi_poweroff();
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* reboot(2) with Linux's semantics (kernel/reboot.c). Allowed with CAP_SYS_BOOT in the initial user namespace,
 * or for the console session owner (uid 1000 in the initial user and PID namespaces, like logind's active-seat
 * policy). Inside a child PID namespace (Linux pid_ns_reboot) RESTART/RESTART2 send SIGHUP and POWER_OFF/HALT
 * SIGINT to that namespace's init, given CAP_SYS_BOOT over its owner namespace; CAD_ON/CAD_OFF there: EINVAL.
 * CAD_ON/CAD_OFF choose what Ctrl-Alt-Del does (immediate restart / SIGINT to init: power_ctrl_alt_del).
 * SW_SUSPEND hibernates and returns 0 after the resume. Data reaches the disk before the machine stops; the
 * other processors are stopped first (pm_stop_other_cpus). */
#define REBOOT_MAGIC1 0xFEE1DEADULL
#define CMD_RESTART 0x01234567ULL
#define CMD_HALT 0xCDEF0123ULL
#define CMD_CAD_ON 0x89ABCDEFULL
#define CMD_CAD_OFF 0x00000000ULL
#define CMD_POWER_OFF 0x4321FEDCULL
#define CMD_RESTART2 0xA1B2C3D4ULL
#define CMD_SW_SUSPEND 0xD000FCE2ULL
#define CMD_KEXEC 0x45584543ULL
bool reboot_cad_enabled = true;   /* Linux's default C_A_D = 1 */
void pm_stop_other_cpus(void);
int64_t platform_reboot_call(struct registers* r) {
    uint32_t magic2 = (uint32_t)r->rsi, command = (uint32_t)r->rdx;
    if ((uint32_t)r->rdi != REBOOT_MAGIC1 || (magic2 != 672274793 && magic2 != 85072278 && magic2 != 369367448 && magic2 != 537993216)) return -22;
    struct credentials* who = credentials_current();
    struct pid_namespace* space = task_pid_namespace();
    if (space != pid_namespace_root()) {
        if (!credentials_capable(who, space->owner, 22)) return -1;
        if (command == CMD_RESTART || command == CMD_RESTART2) return space->init ? task_signal_process(space->init, 1) < 0 ? -3 : 0 : -3;
        if (command == CMD_POWER_OFF || command == CMD_HALT) return space->init ? task_signal_process(space->init, 2) < 0 ? -3 : 0 : -3;
        return -22;
    }
    bool owner = !namespace_id(credentials_namespace(who)) && credentials_kernel_id(who, false, 1) == 1000;
    if (!credentials_capable(who, NULL, 22) && !owner) return -1;
    if (command == CMD_CAD_ON || command == CMD_CAD_OFF) { reboot_cad_enabled = command == CMD_CAD_ON; return 0; }
    if (command == CMD_SW_SUSPEND) return pm_request(PM_STATE_DISK);
    if (command == CMD_KEXEC) return -22;   /* no kexec image can be loaded */
    if (command != CMD_RESTART && command != CMD_RESTART2 && command != CMD_HALT && command != CMD_POWER_OFF) return -22;
    char argument[257] = {0};
    if (command == CMD_RESTART2) {   /* the restart command string (at most 256 bytes, NUL-terminated) */
        for (unsigned i = 0; i < 256; i++) {
            if (!accessible(r->r10 + i, 1, false)) return -14;
            argument[i] = (char)uget8(r->r10 + i); if (!argument[i]) break;
        }
    }
    vga_print("[64] REBOOT syscall: ");
    vga_print(command == CMD_POWER_OFF ? "power-off" : command == CMD_HALT ? "halt" : "restart");
    if (argument[0]) { vga_print(" with command '"); vga_print(argument); vga_print("'"); }
    vga_print(" requested by pid "); print_number(task_pid()); vga_print("; syncing\n");
    memory_file_writeback(true, 0, ~0ULL);
    disk_shutdown();
    ata_flush();
    block_flush_all(); nvme_shutdown_all();   /* device caches, then NVMe CC.SHN after the last flush */
    pm_stop_other_cpus();
    if (command == CMD_RESTART || command == CMD_RESTART2) acpi_reset();
    if (command == CMD_POWER_OFF) {
        acpi_poweroff();
        if (qemu_fw_cfg()) __asm__ volatile("outl %0,%1" :: "a"(0x10), "Nd"((uint16_t)0xF4));
    }
    vga_print("[64] System halted\n");
    for (;;) __asm__ volatile("cli; hlt");
}
