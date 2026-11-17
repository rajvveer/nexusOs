/* Bare-metal diagnostics (boot option `diag`, or D held at boot: both loaders add the option).
 *
 * After every driver has probed, the kernel builds a plain-text report - boot hand-off, CPU and APIC
 * topology with MTRR/PAT, firmware memory map, ACPI tables, the PCI tree (IDs, BARs, IRQ, MSI/MSI-X,
 * PCIe link, power state) with the driver that took each function, display (GOP/VBE modes, EDID),
 * storage, network, USB, and the whole kernel log (every driver's probe messages and error codes) -
 * writes it as NEXUSDIAG.TXT to the NexusOS boot medium (the FAT volume holding EFI\NEXUS\KERNEL.BIN;
 * diagfat.c), prints a summary on screen, waits diag.wait=N seconds (default 15) and boots on.
 * The UEFI loader writes a firmware-only NEXUSDIAG.TXT first, so a boot medium the kernel cannot reach
 * (a USB stick without a USB storage driver) still carries a report. Nothing here changes hardware state
 * beyond config/MSR reads. */
#include "native.h"
#include "boot_info.h"
#include "acpi.h"
#include "hwinfo.h"
#include "pcidev.h"
#include "netdrv.h"
#include "usb.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "../../kernel/rtc.h"
#include "../../kernel/net.h"

/* ---- kernel log ring (fed by kernel.c put(); always on, so the report has the boot from the start) ---- */
#define LOG_BYTES 65536U
static char log_ring[LOG_BYTES];
static uint32_t log_head;
void diag_log_char(char c) { uint32_t at = __atomic_fetch_add(&log_head, 1, __ATOMIC_RELAXED); log_ring[at % LOG_BYTES] = c; }

/* ---- report text ---- */
struct out { char* p; uint64_t used, size; };
static __attribute__((noinline, cold)) void s(struct out* o, const char* t) { while (t && *t && o->used + 1 < o->size) o->p[o->used++] = *t++; }
static __attribute__((noinline, cold)) void ch(struct out* o, char c) { if (o->used + 1 < o->size) o->p[o->used++] = c; }
static __attribute__((noinline, cold)) void d(struct out* o, uint64_t v) { char b[24]; unsigned n = 0; do b[n++] = (char)('0' + v % 10); while (v /= 10); while (n) ch(o, b[--n]); }
static __attribute__((noinline, cold)) void x(struct out* o, uint64_t v, unsigned digits) { for (unsigned i = digits; i--; ) ch(o, "0123456789abcdef"[(v >> (4 * i)) & 15]); }
static __attribute__((noinline, cold)) void hx(struct out* o, uint64_t v) { s(o, "0x"); unsigned n = 1; while (n < 16 && (v >> (4 * n))) n++; x(o, v, n); }
static __attribute__((noinline, cold)) void pad(struct out* o, uint64_t start, unsigned column) { while (o->used - start < column) ch(o, ' '); }
static __attribute__((noinline, cold)) void title(struct out* o, const char* t) { s(o, "\r\n== "); s(o, t); s(o, " ==\r\n"); }
static __attribute__((noinline, cold)) void nl(struct out* o) { s(o, "\r\n"); }
static __attribute__((noinline, cold)) void bdf(struct out* o, const pci_device_t* p) { x(o, p->bus, 2); ch(o, ':'); x(o, p->device, 2); ch(o, '.'); x(o, p->function, 1); }

/* ---- CPU ---- */
static void cpu_section(struct out* o) {
    title(o, "CPU and APIC topology");
    uint32_t r[4]; char vendor[13], brand[49];
    hw_cpuid(0, 0, r); uint32_t max = r[0]; memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4); vendor[12] = 0;
    hw_cpuid(0x80000000U, 0, r); uint32_t max_ext = r[0]; brand[0] = 0;
    if (max_ext >= 0x80000004U) { for (unsigned i = 0; i < 3; i++) { hw_cpuid(0x80000002U + i, 0, r); memcpy(brand + 16 * i, r, 16); } brand[48] = 0; }
    hw_cpuid(1, 0, r); uint32_t eax1 = r[0], ecx1 = r[2], edx1 = r[3];
    unsigned family = (eax1 >> 8) & 15, model = (eax1 >> 4) & 15;
    if (family == 15) family += (eax1 >> 20) & 0xFF;
    if (family >= 6) model |= ((eax1 >> 16) & 15) << 4;
    s(o, "vendor "); s(o, vendor); s(o, "  brand \""); const char* b = brand; while (*b == ' ') b++; s(o, b); s(o, "\"\r\n");
    s(o, "family "); hx(o, family); s(o, " model "); hx(o, model); s(o, " stepping "); d(o, eax1 & 15); s(o, " max leaf "); hx(o, max); s(o, "/"); hx(o, max_ext); nl(o);
    uint32_t ext1 = 0, power = 0, leaf7 = 0;
    if (max_ext >= 0x80000001U) { hw_cpuid(0x80000001U, 0, r); ext1 = r[3]; }
    if (max_ext >= 0x80000007U) { hw_cpuid(0x80000007U, 0, r); power = r[3]; }
    if (max >= 7) { hw_cpuid(7, 0, r); leaf7 = r[1]; }
    static const struct { const char* name; uint8_t reg, bit; } features[] = {
        {"apic", 3, 9}, {"mtrr", 3, 12}, {"pat", 3, 16}, {"sse2", 3, 26}, {"x2apic", 2, 21}, {"tsc-deadline", 2, 24}, {"xsave", 2, 26},
        {"avx", 2, 28}, {"rdrand", 2, 30}, {"hypervisor", 2, 31}};
    s(o, "features");
    for (unsigned i = 0; i < sizeof(features) / sizeof(*features); i++)
        if (((features[i].reg == 3 ? edx1 : ecx1) >> features[i].bit) & 1) { ch(o, ' '); s(o, features[i].name); }
    if ((ext1 >> 20) & 1) s(o, " nx");
    if ((power >> 8) & 1) s(o, " invariant-tsc");
    if ((leaf7 >> 5) & 1) s(o, " avx2");
    if ((leaf7 >> 16) & 1) s(o, " avx512f");
    nl(o);
    s(o, "TSC "); d(o, block_tsc_hz()); s(o, " Hz (calibration source: see the \"Calibrated TSC\" log line)\r\n");
    uint64_t apic_base = 0, pat = 0;
    if (!hw_rdmsr_safe(0x1B, &apic_base)) {
        s(o, "IA32_APIC_BASE "); hx(o, apic_base); s(o, apic_base & (1ULL << 11) ? " enabled" : " disabled");
        s(o, apic_base & (1ULL << 10) ? " x2APIC mode" : " xAPIC mode"); s(o, apic_base & (1ULL << 8) ? " (BSP)\r\n" : "\r\n");
    }
    if (!hw_rdmsr_safe(0x277, &pat)) { s(o, "IA32_PAT "); hx(o, pat); s(o, " (entry 7 = WC for framebuffers)\r\n"); }
    static const struct { uint32_t msr; const char* name; } msrs[] = {{0xC0010015, "AMD HWCR"}, {0xC0010058, "AMD MMIO_CFG_BASE (ECAM)"},
                                                                       {0xC0011029, "AMD DE_CFG"},
                                                                       {0x1A0, "IA32_MISC_ENABLE"}, {0x3A, "IA32_FEATURE_CONTROL"}, {0xCE, "Intel PLATFORM_INFO"}};
    for (unsigned i = 0; i < sizeof(msrs) / sizeof(*msrs); i++) {          /* absent ones fault: fixed up, reported */
        uint64_t value; s(o, "MSR "); hx(o, msrs[i].msr); ch(o, ' '); s(o, msrs[i].name);
        if (hw_rdmsr_safe(msrs[i].msr, &value)) s(o, ": absent\r\n"); else { s(o, " = "); hx(o, value); nl(o); }
    }
    s(o, "CPUs online "); d(o, cpu_count); s(o, " of MAX_CPUS "); d(o, MAX_CPUS); s(o, "; MADT enabled processors "); d(o, acpi.cpu_count);
    s(o, " (x2APIC entries with ids > 254 are skipped: the kernel runs the APIC in xAPIC mode)\r\n");
    s(o, "MADT APIC ids:"); for (unsigned i = 0; i < acpi.cpu_count && i < ACPI_MAX_CPUS; i++) { ch(o, ' '); d(o, acpi.cpu_apic_ids[i]); } nl(o);
    s(o, "online CPUs (index:apic id):"); for (unsigned i = 0; i < MAX_CPUS; i++) if (cpus[i].online) { ch(o, ' '); d(o, i); ch(o, ':'); d(o, cpus[i].apic_id); } nl(o);
    s(o, "IOAPICs "); d(o, acpi.ioapic_count);
    for (unsigned i = 0; i < acpi.ioapic_count; i++) { s(o, " [id "); d(o, acpi.ioapics[i].id); s(o, " at "); hx(o, acpi.ioapics[i].address); s(o, " gsi "); d(o, acpi.ioapics[i].gsi_base); s(o, "]"); }
    s(o, acpi.madt_flags & 1 ? "; dual 8259 present\r\n" : "; no 8259 (PCAT_COMPAT clear)\r\n");
    struct hw_mtrr m;
    if (hw_mtrr_read(&m)) {
        s(o, "MTRR cap "); hx(o, m.cap); s(o, " default "); s(o, hw_memtype_name((int)(m.def_type & 0xFF)));
        s(o, m.def_type & (1U << 11) ? " enabled" : " DISABLED"); s(o, m.def_type & (1U << 10) ? ", fixed ranges on\r\n" : ", fixed ranges off\r\n");
        for (unsigned i = 0; i < m.count; i++) {
            if (!(m.mask[i] & (1U << 11))) continue;
            uint64_t mask = m.mask[i] & m.physical_mask, size = (~mask & m.physical_mask) + 0x1000;
            s(o, "  var "); d(o, i); s(o, ": base "); hx(o, m.base[i] & m.physical_mask); s(o, " size "); hx(o, size); s(o, " "); s(o, hw_memtype_name((int)(m.base[i] & 0xFF))); nl(o);
        }
    } else s(o, "MTRR not available\r\n");
}

/* ---- memory ---- */
static const char* region_type(uint32_t source, uint32_t type) {
    if (source == NEXUS_BOOT_BIOS) {
        static const char* const e820[] = {"?", "usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad", "?", "persistent"};
        return type < 8 ? e820[type] : "other";
    }
    static const char* const efi[] = {"reserved", "loader code", "loader data", "boot code", "boot data", "runtime code", "runtime data",
                                      "conventional", "unusable", "ACPI reclaim", "ACPI NVS", "MMIO", "MMIO port", "PAL code", "persistent", "unaccepted"};
    return type < 16 ? efi[type] : "other";
}
static void memory_section(struct out* o) {
    title(o, "Memory map");
    const struct nexus_boot_info* b = &boot_info;
    s(o, "firmware map ("); s(o, b->source == NEXUS_BOOT_UEFI ? "UEFI GetMemoryMap at ExitBootServices" : "BIOS E820"); s(o, "), ");
    d(o, b->region_count); s(o, b->region_count >= NEXUS_BOOT_REGIONS_MAX ? " entries (list full, adjacent equal types merged)\r\n" : " entries\r\n");
    for (unsigned i = 0; i < b->region_count && i < NEXUS_BOOT_REGIONS_MAX; i++) {
        const struct nexus_boot_region* r = &b->regions[i]; uint64_t start = o->used;
        s(o, "  "); x(o, r->base, 16); s(o, "-"); x(o, r->base + r->size - 1, 16); pad(o, start, 38);
        s(o, region_type(b->source, r->type)); if (r->attributes) { s(o, " attr "); hx(o, r->attributes); } nl(o);
    }
    s(o, "usable RAM ranges used by the kernel ("); d(o, b->memory_total >> 20); s(o, " MiB):\r\n");
    for (unsigned i = 0; i < b->memory_count; i++) { s(o, "  "); x(o, b->memory[i].base, 16); s(o, " +"); hx(o, b->memory[i].size); nl(o); }
}

/* ---- ACPI ---- */
static void acpi_section(struct out* o) {
    title(o, "ACPI tables");
    if (!acpi.present) { s(o, "no ACPI (RSDP not found)\r\n"); return; }
    s(o, "RSDP "); hx(o, acpi.rsdp); s(o, " revision "); d(o, acpi.revision); s(o, acpi.xsdt ? " via XSDT\r\n" : " via RSDT\r\n");
    uint64_t physical;
    for (unsigned n = 0; n < 128; n++) {
        const uint8_t* t = hw_acpi_table(NULL, n, &physical);
        if (!t) break;
        uint32_t length; memcpy(&length, t + 4, 4);
        char sig[5] = {(char)t[0], (char)t[1], (char)t[2], (char)t[3], 0}, oem[7], id[9];
        memcpy(oem, t + 10, 6); oem[6] = 0; memcpy(id, t + 16, 8); id[8] = 0;
        uint64_t start = o->used;
        s(o, "  "); s(o, sig); s(o, " at "); x(o, physical, 8); s(o, " len "); d(o, length); pad(o, start, 30);
        s(o, " rev "); d(o, t[8]); s(o, " oem \""); s(o, oem); s(o, "\" \""); s(o, id); s(o, "\"\r\n");
    }
    s(o, "  DSDT at "); hx(o, acpi.dsdt); s(o, "; FADT flags "); hx(o, acpi.fadt_flags); s(o, acpi.fadt_flags & ACPI_FADT_HW_REDUCED ? " (hardware-reduced)" : "");
    s(o, "; IAPC_BOOT_ARCH "); hx(o, acpi.boot_arch); s(o, acpi.boot_arch & 2 ? " (8042)" : " (no 8042 declared)"); s(o, "; S5 "); s(o, acpi.s5 ? "found" : "missing"); nl(o);
    for (unsigned i = 0; i < acpi.mcfg_count; i++) { s(o, "  MCFG ECAM "); hx(o, acpi.mcfg[i].base); s(o, " buses "); d(o, acpi.mcfg[i].start_bus); s(o, "-"); d(o, acpi.mcfg[i].end_bus); nl(o); }
}

/* ---- PCI ---- */
static const char* class_name(const pci_device_t* p) {
    switch (p->class_code) {
    case 0x01: return p->subclass == 6 ? "SATA (AHCI)" : p->subclass == 8 ? "NVMe" : p->subclass == 4 ? "RAID" : p->subclass == 1 ? "IDE" : "storage";
    case 0x02: return p->subclass == 0 ? "Ethernet" : p->subclass == 0x80 ? "network (Wi-Fi?)" : "network";
    case 0x03: return p->subclass == 0 ? "VGA display" : "display";
    case 0x04: return p->subclass == 3 ? "HD Audio" : p->subclass == 1 ? "AC97 audio" : "multimedia";
    case 0x06: return p->subclass == 4 ? "PCI bridge" : p->subclass == 0 ? "host bridge" : p->subclass == 1 ? "ISA bridge" : "bridge";
    case 0x0C: return p->subclass == 3 ? (p->prog_if == 0x30 ? "USB xHCI" : p->prog_if == 0x20 ? "USB EHCI" : p->prog_if == 0x10 ? "USB OHCI" :
                                          p->prog_if == 0 ? "USB UHCI" : p->prog_if == 0x40 ? "USB4" : "USB") : p->subclass == 5 ? "SMBus" : "serial bus";
    case 0x08: return "system peripheral";
    case 0x0D: return "wireless";
    case 0x10: return "encryption";
    case 0x11: return "signal processing";
    case 0x13: return "non-essential instrumentation";
    default: return "other";
    }
}
static bool same(const pci_device_t* a, const pci_device_t* b) { return a && b && a->bus == b->bus && a->device == b->device && a->function == b->function; }
static void driver_status(struct out* o, const pci_device_t* p) {
    const pci_device_t* nic = native_nic_pci();
    if (p->class_code == 0x02 && p->subclass == 0) {
        if (same(p, nic)) {
            struct nic_link link = {0}; native_nic_link(&link);
            s(o, native_nic_driver()); s(o, ": driving eth0, link "); s(o, link.up ? "up" : "down");
            if (link.up && link.speed) { ch(o, ' '); d(o, link.speed); s(o, " Mb/s"); }
        } else s(o, nic ? "not driven (one NIC at a time)" : "no driver for this NIC");
        return;
    }
    if (p->class_code == 0x02 || p->class_code == 0x0D) {
        s(o, p->vendor_id == 0x14C3 ? "no driver (MediaTek Wi-Fi: not supported yet; see the MT7922 note in HANDOFF)" : "no driver"); return;
    }
    if (p->class_code == 0x01) {
        const char* kind = p->subclass == 6 ? "AHCI" : p->subclass == 8 ? "NVMe" : p->subclass == 1 ? "IDE" : NULL;
        if (p->subclass == 4) { s(o, "RAID mode: disks invisible - set the SATA/NVMe mode to AHCI in the firmware setup"); return; }
        if (!kind) { s(o, "no driver"); return; }
        unsigned disks = 0; for (unsigned i = 0; i < block_device_count(); i++) disks += !strcmp(block_device_get(i)->kind, kind);
        s(o, p->subclass == 6 ? "ahci" : p->subclass == 8 ? "nvme" : "ide"); s(o, " ("); d(o, disks); s(o, " "); s(o, kind); s(o, " block devices in total)"); return;
    }
    if (p->class_code == 0x03) {
        s(o, p->vendor_id == 0x1AF4 ? "virtio-gpu (2D/virgl) + firmware framebuffer" : "firmware framebuffer only (GOP/VBE): no native GPU driver, no 3D"); return;
    }
    if (p->class_code == 0x04 && p->subclass == 3) { s(o, same(p, hda_pci()) ? "hda: driving the PCM" : "hda: probed, not chosen (see HDA log lines)"); return; }
    if (p->class_code == 0x04 && p->subclass == 1) { s(o, "ac97"); return; }
    if (p->class_code == 0x0C && p->subclass == 3) {
        for (unsigned i = 0; i < xhci_count(); i++) if (same(p, xhci_pci(xhci_get(i)))) { s(o, "xhci: "); s(o, xhci_name(xhci_get(i))); s(o, " ("); d(o, xhci_port_count(xhci_get(i))); s(o, " ports)"); return; }
        s(o, p->prog_if == 0x30 ? "xHCI not started (see USB log lines)" : "no driver (USB 1/2 companion; devices are served by xHCI)"); return;
    }
    if (p->class_code == 0x06) { s(o, "-"); return; }
    s(o, "no driver");
}
static void pci_section(struct out* o, unsigned* undriven) {
    title(o, "PCI devices");
    pci_device_t* list = pci_get_devices(); int count = pci_device_count();
    s(o, "functions "); d(o, (uint64_t)count); s(o, " (bus:dev.fn vendor:device class/sub/prog-if rev  type  driver)\r\n");
    for (int i = 0; i < count; i++) {
        const pci_device_t* p = &list[i];
        uint64_t start = o->used;
        bdf(o, p); ch(o, ' '); x(o, p->vendor_id, 4); ch(o, ':'); x(o, p->device_id, 4); ch(o, ' ');
        x(o, p->class_code, 2); ch(o, '/'); x(o, p->subclass, 2); ch(o, '/'); x(o, p->prog_if, 2); s(o, " rev "); x(o, p->revision, 2);
        s(o, "  "); s(o, class_name(p)); pad(o, start, 58); s(o, "  "); driver_status(o, p); nl(o);
        uint8_t header = pci_read8(p->bus, p->device, p->function, 0x0E) & 0x7F;
        uint16_t command = pci_read16(p->bus, p->device, p->function, PCI_COMMAND), status = pci_read16(p->bus, p->device, p->function, 0x06);
        s(o, "    command "); x(o, command, 4); s(o, " status "); x(o, status, 4); s(o, " irq line "); d(o, p->irq_line);
        s(o, " pin "); d(o, pci_read8(p->bus, p->device, p->function, 0x3D));
        if (header == 1) {
            uint32_t buses = pci_read32(p->bus, p->device, p->function, 0x18), mem = pci_read32(p->bus, p->device, p->function, 0x20), pref = pci_read32(p->bus, p->device, p->function, 0x24);
            s(o, " buses "); d(o, (buses >> 8) & 0xFF); ch(o, '-'); d(o, (buses >> 16) & 0xFF);
            s(o, " mem "); hx(o, (uint64_t)(mem & 0xFFF0) << 16); ch(o, '-'); hx(o, ((uint64_t)(mem >> 16 & 0xFFF0) << 16) | 0xFFFFF);
            uint64_t pb = (uint64_t)(pref & 0xFFF0) << 16, pl = ((uint64_t)(pref >> 16 & 0xFFF0) << 16) | 0xFFFFF;
            if ((pref & 0xF) == 1) { pb |= (uint64_t)pci_read32(p->bus, p->device, p->function, 0x28) << 32; pl |= (uint64_t)pci_read32(p->bus, p->device, p->function, 0x2C) << 32; }
            s(o, " pref "); hx(o, pb); ch(o, '-'); hx(o, pl);
        }
        nl(o);
        unsigned bars = header == 0 ? 6 : header == 1 ? 2 : 0;
        for (unsigned b = 0; b < bars; b++) {
            uint32_t low = pci_read32(p->bus, p->device, p->function, (uint8_t)(0x10 + 4 * b));
            if (!low) continue;
            s(o, "    BAR"); d(o, b); ch(o, ' ');
            if (low & 1) { s(o, "I/O "); hx(o, low & ~3U); nl(o); continue; }
            uint64_t base = low & ~0xFULL; bool wide = ((low >> 1) & 3) == 2;
            if (wide && b < 5) base |= (uint64_t)pci_read32(p->bus, p->device, p->function, (uint8_t)(0x14 + 4 * b)) << 32;
            s(o, wide ? "mem64 " : "mem32 "); hx(o, base); s(o, low & 8 ? " prefetchable" : ""); s(o, base ? "\r\n" : " UNASSIGNED\r\n");
            if (wide) b++;
        }
        if (status & 0x10) {
            s(o, "    caps:");
            uint8_t at = pci_read8(p->bus, p->device, p->function, 0x34) & 0xFC;
            for (unsigned guard = 0; at >= 0x40 && guard < 48; guard++) {
                uint8_t id = pci_read8(p->bus, p->device, p->function, at);
                uint16_t control = pci_read16(p->bus, p->device, p->function, (uint8_t)(at + 2));
                switch (id) {
                case 0x01: s(o, " PM(D"); d(o, pci_read16(p->bus, p->device, p->function, (uint8_t)(at + 4)) & 3); s(o, ")"); break;
                case 0x05: s(o, " MSI("); s(o, control & 1 ? "on" : "off"); s(o, control & 0x80 ? ",64-bit" : ""); s(o, ",vectors "); d(o, 1U << ((control >> 1) & 7)); s(o, ")"); break;
                case 0x10: {
                    uint32_t link_cap = pci_read32(p->bus, p->device, p->function, (uint8_t)(at + 0x0C)); uint16_t link_status = pci_read16(p->bus, p->device, p->function, (uint8_t)(at + 0x12));
                    s(o, " PCIe(gen "); d(o, link_status & 15); s(o, " x"); d(o, (link_status >> 4) & 63); s(o, " of gen "); d(o, link_cap & 15); s(o, " x"); d(o, (link_cap >> 4) & 63); s(o, ")");
                    break;
                }
                case 0x11: s(o, " MSI-X("); s(o, control & 0x8000 ? "on" : "off"); s(o, ",table "); d(o, (control & 0x7FF) + 1U); s(o, ")"); break;
                case 0x09: s(o, " vendor"); break;
                case 0x0D: s(o, " SSVID"); break;
                case 0x12: s(o, " SATA"); break;
                case 0x13: s(o, " AF"); break;
                default: s(o, " cap"); hx(o, id); break;
                }
                at = pci_read8(p->bus, p->device, p->function, (uint8_t)(at + 1)) & 0xFC;
            }
            nl(o);
        }
        bool none = false;
        if ((p->class_code == 0x02 && !same(p, native_nic_pci())) || p->class_code == 0x0D || (p->class_code == 0x03 && p->vendor_id != 0x1AF4) ||
            (p->class_code == 0x01 && p->subclass == 4)) none = true;
        if (none) (*undriven)++;
    }
}

/* ---- display ---- */
static void edid_section(struct out* o, const struct nexus_boot_display* dsp) {
    const uint8_t* e = dsp->edid;
    static const char* const sources[] = {"none", "UEFI EDID Active", "UEFI EDID Discovered", "VBE/DDC"};
    s(o, "EDID source: "); s(o, sources[dsp->edid_source <= 3 ? dsp->edid_source : 0]); s(o, ", "); d(o, dsp->edid_size); s(o, " bytes");
    if (dsp->edid_size < 128) { nl(o); return; }
    char vendor[4] = {(char)('@' + (e[8] >> 2 & 31)), (char)('@' + ((e[8] & 3) << 3 | e[9] >> 5)), (char)('@' + (e[9] & 31)), 0};
    s(o, ", manufacturer "); s(o, vendor); s(o, " product "); x(o, e[10] | e[11] << 8, 4); s(o, " week "); d(o, e[16]); s(o, " year "); d(o, 1990U + e[17]);
    s(o, " EDID "); d(o, e[18]); ch(o, '.'); d(o, e[19]); s(o, " extensions "); d(o, e[126]); s(o, dsp->edid_virtual ? " (virtual-machine display)" : ""); nl(o);
    for (unsigned k = 0; k < 4; k++) {
        const uint8_t* desc = e + 54 + 18 * k;
        if (desc[0] || desc[1]) {
            unsigned w = desc[2] | (desc[4] & 0xF0) << 4, h = desc[5] | (desc[7] & 0xF0) << 4, clock = desc[0] | desc[1] << 8;
            unsigned htotal = w + (desc[3] | (desc[4] & 15) << 8), vtotal = h + (desc[6] | (desc[7] & 15) << 8);
            s(o, "  timing "); d(o, w); ch(o, 'x'); d(o, h); s(o, " pixel clock "); d(o, clock * 10ULL); s(o, " kHz");
            if (htotal && vtotal) { s(o, " ~"); d(o, (uint64_t)clock * 10000ULL / htotal * 1000ULL / vtotal / 1000ULL); s(o, " Hz"); }
            s(o, k == 0 ? " (preferred)\r\n" : "\r\n");
        } else if (desc[3] == 0xFC || desc[3] == 0xFF) {
            s(o, desc[3] == 0xFC ? "  monitor name \"" : "  serial \"");
            for (unsigned i = 5; i < 18 && desc[i] != 0x0A; i++) ch(o, desc[i] >= 0x20 && desc[i] < 0x7F ? (char)desc[i] : '?');
            s(o, "\"\r\n");
        }
    }
    for (unsigned i = 0; i < dsp->edid_size; i += 16) { s(o, "  "); for (unsigned j = 0; j < 16 && i + j < dsp->edid_size; j++) { x(o, e[i + j], 2); ch(o, ' '); } nl(o); }
}
static void display_section(struct out* o) {
    title(o, "Display");
    const struct nexus_boot_framebuffer* fb = &boot_info.framebuffer; const struct nexus_boot_display* dsp = &boot_info.display;
    static const char* const formats[] = {"none", "XRGB8888 (BGRX bytes)", "XBGR8888 (RGBX bytes)", "other/unsupported"};
    static const char* const reasons[] = {"kept current (switch failed or BIOS fallback)", "video= option", "EDID native", "1920x1080 default", "firmware's current mode", "largest mode"};
    s(o, "framebuffer "); d(o, fb->width); ch(o, 'x'); d(o, fb->height); s(o, " pitch "); d(o, fb->pitch); s(o, " bpp "); d(o, fb->bpp); s(o, " at "); hx(o, fb->base);
    s(o, " "); s(o, formats[fb->format <= 3 ? fb->format : 3]); s(o, "; chosen by: "); s(o, reasons[dsp->reason <= 5 ? dsp->reason : 0]); nl(o);
    struct hw_mtrr m; if (hw_mtrr_read(&m) && fb->base) { s(o, "framebuffer MTRR type "); s(o, hw_memtype_name(hw_mtrr_type(&m, fb->base))); s(o, "; kernel console and /dev/fb0 map it write-combining through PAT\r\n"); }
    s(o, "no native GPU driver: AMD (RDNA2 iGPU) and NVIDIA (Blackwell) run on this firmware framebuffer only - no 3D acceleration, no mode switching, no multi-monitor\r\n");
    if (boot_info.source == NEXUS_BOOT_UEFI) {
        s(o, "GOP modes ("); d(o, dsp->mode_total); s(o, " reported):");
        for (unsigned i = 0; i < dsp->mode_count; i++) {
            if (i % 6 == 0) s(o, "\r\n "); ch(o, ' '); d(o, dsp->modes[i].width); ch(o, 'x'); d(o, dsp->modes[i].height);
            if (dsp->modes[i].pitch_pixels != dsp->modes[i].width) { s(o, "/p"); d(o, dsp->modes[i].pitch_pixels); }
            s(o, dsp->modes[i].format == 1 ? "" : dsp->modes[i].format == 2 ? "(RGB)" : "(other)"); if (dsp->modes[i].flags & 1) ch(o, '*');
        }
        nl(o);
    }
    edid_section(o, dsp);
}

/* ---- storage / network / USB ---- */
static void devices_section(struct out* o) {
    title(o, "Storage");
    for (unsigned i = 0; i < block_device_count(); i++) {
        const struct block_device* b = block_device_get(i);
        s(o, b->name); s(o, " "); s(o, b->kind); s(o, " \""); s(o, b->model); s(o, "\" "); d(o, b->blocks * b->block_size >> 20); s(o, " MiB, ");
        d(o, b->block_size); s(o, "-byte blocks, irq "); d(o, b->irq); s(o, ", errors "); d(o, b->stats.errors); s(o, ", resets "); d(o, b->stats.resets); nl(o);
    }
    if (!block_device_count()) s(o, "no block devices (see AHCI/NVMe/IDE log lines)\r\n");
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        s(o, "  "); s(o, p->label); s(o, p->scheme == BLOCK_SCHEME_GPT ? " GPT type " : p->scheme == BLOCK_SCHEME_MBR ? " MBR type " : " whole disk");
        if (p->scheme == BLOCK_SCHEME_GPT) for (unsigned k = 0; k < 16; k++) x(o, p->type_guid[k], 2);
        else if (p->scheme == BLOCK_SCHEME_MBR) hx(o, p->mbr_type);
        s(o, " start "); d(o, p->start); s(o, " blocks "); d(o, p->blocks); if (p->name[0]) { s(o, " \""); s(o, p->name); s(o, "\""); }
        s(o, block_root() == p ? " (root)\r\n" : "\r\n");
    }
    title(o, "Network");
    net_device_t* n = net_get_device(0);
    if (n) {
        s(o, n->name); s(o, " MAC "); for (unsigned i = 0; i < 6; i++) { if (i) ch(o, ':'); x(o, ((const uint8_t*)&n->mac)[i], 2); }
        struct nic_link link = {0}; bool known = native_nic_link(&link);
        s(o, " driver "); s(o, native_nic_driver()); s(o, known && link.up ? " link up" : " link down");
        if (known && link.up && link.speed) { ch(o, ' '); d(o, link.speed); s(o, link.duplex == NIC_DUPLEX_FULL ? " Mb/s full duplex" : " Mb/s"); }
        nl(o);
    } else s(o, "no Ethernet interface (no supported NIC; Wi-Fi is not supported)\r\n");
    title(o, "USB");
    for (unsigned i = 0; i < xhci_count(); i++) {
        struct xhci* hc = xhci_get(i); const pci_device_t* p = xhci_pci(hc);
        s(o, xhci_name(hc)); s(o, " "); if (p) { bdf(o, p); ch(o, ' '); x(o, p->vendor_id, 4); ch(o, ':'); x(o, p->device_id, 4); } s(o, " ports "); d(o, xhci_port_count(hc)); nl(o);
    }
    if (!xhci_count()) s(o, "no xHCI controller started\r\n");
    s(o, "(devices and hubs: see the USB log lines)\r\n");
}
static void log_section(struct out* o) {
    title(o, "Kernel log");
    uint32_t head = __atomic_load_n(&log_head, __ATOMIC_RELAXED), first = head > LOG_BYTES ? head - LOG_BYTES : 0;
    if (first) s(o, "(older lines dropped: 64 KiB ring)\r\n");
    for (uint32_t i = first; i < head; i++) { char c = log_ring[i % LOG_BYTES]; if (c == '\n') s(o, "\r\n"); else if (c != '\r') ch(o, c); }
}

static void summary_line(const char* text) { vga_print("[64] DIAG "); vga_print(text); vga_print("\n"); }
static void summary(struct out* o, unsigned undriven, int written, const char* where) {
    (void)o;
    vga_print("\n[64] DIAG ===== NexusOS hardware diagnostics =====\n");
    char line[400]; struct out l = {line, 0, sizeof(line)};
    uint32_t r[4]; char brand[49] = {0}; hw_cpuid(0x80000000U, 0, r);
    if (r[0] >= 0x80000004U) for (unsigned i = 0; i < 3; i++) { hw_cpuid(0x80000002U + i, 0, r); memcpy(brand + 16 * i, r, 16); }
    const char* b = brand; while (*b == ' ') b++;
    s(&l, "CPU: "); s(&l, b); s(&l, ", "); d(&l, cpu_count); s(&l, " CPUs online"); line[l.used] = 0; summary_line(line);
    l.used = 0; const struct nexus_boot_framebuffer* fb = &boot_info.framebuffer;
    s(&l, "display: "); if (fb->format) { d(&l, fb->width); ch(&l, 'x'); d(&l, fb->height); s(&l, " firmware framebuffer, no native GPU driver"); } else s(&l, "no linear framebuffer (serial console only)");
    line[l.used] = 0; summary_line(line);
    l.used = 0; s(&l, "storage: "); d(&l, block_device_count()); s(&l, " disks");
    for (unsigned i = 0; i < block_device_count() && i < 4; i++) { s(&l, ", "); s(&l, block_device_get(i)->name); }
    s(&l, block_root() ? "; root found" : "; no root volume"); line[l.used] = 0; summary_line(line);
    l.used = 0; struct nic_link link = {0};
    s(&l, "network: "); if (native_nic_pci() || net_get_device(0)) { s(&l, native_nic_driver()); s(&l, native_nic_link(&link) && link.up ? " link up" : " link down"); } else s(&l, "no supported NIC");
    line[l.used] = 0; summary_line(line);
    l.used = 0; s(&l, "audio: "); s(&l, hda_pci() ? "HD Audio driving" : "no HD Audio output chosen"); s(&l, "; USB: "); d(&l, xhci_count()); s(&l, " xHCI controllers");
    line[l.used] = 0; summary_line(line);
    l.used = 0; s(&l, "PCI: "); d(&l, (uint64_t)pci_device_count()); s(&l, " functions, "); d(&l, undriven); s(&l, " NIC/Wi-Fi/GPU/RAID functions without a driver (details in the report)");
    line[l.used] = 0; summary_line(line);
    l.used = 0; s(&l, written > 0 ? "report: NEXUSDIAG.TXT written to " : "report: NEXUSDIAG.TXT NOT written - "); s(&l, where); line[l.used] = 0; summary_line(line);
}

void diag_run(void) {
    if (!boot_option("diag")) return;
    uint64_t size = 1ULL << 20, buffer = arena_alloc(size);
    if (!buffer) { vga_print("[64] DIAG no memory for the report\n"); return; }
    struct out o = {(char*)(uintptr_t)buffer, 0, size};
    rtc_time_t now; rtc_read(&now);
    s(&o, "NexusOS hardware diagnostics report (NEXUSDIAG.TXT, written by the kernel)\r\n");
    s(&o, "time (RTC) "); d(&o, now.year); ch(&o, '-'); d(&o, now.month / 10); d(&o, now.month % 10); ch(&o, '-'); d(&o, now.day / 10); d(&o, now.day % 10);
    ch(&o, ' '); d(&o, now.hour / 10); d(&o, now.hour % 10); ch(&o, ':'); d(&o, now.minute / 10); d(&o, now.minute % 10); ch(&o, ':'); d(&o, now.second / 10); d(&o, now.second % 10);
    s(&o, "\r\nboot "); s(&o, boot_info.source == NEXUS_BOOT_UEFI ? "UEFI (BOOTX64.EFI)" : "BIOS (VBE/E820)"); s(&o, "; command line \""); s(&o, boot_info.cmdline); s(&o, "\"\r\n");
    s(&o, "Send this file with bug reports. It contains hardware IDs, firmware tables and the kernel log; no user files.\r\n");
    unsigned undriven = 0;
    cpu_section(&o); memory_section(&o); acpi_section(&o); pci_section(&o, &undriven); display_section(&o); devices_section(&o); log_section(&o);
    char where[320]; int written = diagfat_write((const uint8_t*)o.p, o.used, where, sizeof(where));
    summary(&o, undriven, written, where);
    arena_free(buffer, size);
    unsigned wait = boot_option_number("diag.wait=");
    if (!wait && !boot_option("diag.wait=0")) wait = 15;
    if (wait) {
        char text[12]; uint_to_str(wait, text);
        vga_print("[64] DIAG booting on in "); vga_print(text); vga_print(" s (diag.wait=N changes this; power off to stop)\n");
        for (uint64_t end = native_clock_ns() + wait * 1000000000ULL; native_clock_ns() < end; ) __asm__ volatile("pause");
    }
}
