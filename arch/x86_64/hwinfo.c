/* Hardware facts for the console and the diagnostics report (diag.c): fault-tolerant MSR reads,
 * the firmware's MTRR setup (Intel SDM vol. 3 ch. 12 "Memory Cache Control"; AMD APM vol. 2 ch. 7)
 * and the framebuffer's effective memory type.
 *
 * The kernel never reprograms MTRRs: the firmware owns them and sets them identically on every CPU.
 * Write-combining for the framebuffer comes from PAT entry 7 (cpu_local_init), which overrides an
 * MTRR UC/WT/WB type for that mapping (SDM table 12-7: MTRR UC + PAT WC = WC). */
#include "native.h"
#include "hwinfo.h"
#include "boot_info.h"
#include "acpi.h"
#include "pcidev.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

/* int hw_rdmsr_safe(uint32_t msr, uint64_t* value): 0, or -5 when the MSR does not exist (#GP at
 * hw_rdmsr_fault is turned into a return through hw_rdmsr_fixup by trap64). */
__asm__(".text\n"
        ".globl hw_rdmsr_safe, hw_rdmsr_fault, hw_rdmsr_fixup\n"
        "hw_rdmsr_safe:\n"
        "    movl %edi, %ecx\n"
        "hw_rdmsr_fault:\n"
        "    rdmsr\n"
        "    shlq $32, %rdx\n"
        "    orq %rdx, %rax\n"
        "    movq %rax, (%rsi)\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        "hw_rdmsr_fixup:\n"
        "    movl $-5, %eax\n"
        "    ret\n");

void hw_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static bool mtrr_present(void) { uint32_t r[4]; hw_cpuid(1, 0, r); return (r[3] >> 12) & 1; }
static unsigned physical_bits(void) {
    uint32_t r[4]; hw_cpuid(0x80000000U, 0, r);
    if (r[0] < 0x80000008U) return 36;
    hw_cpuid(0x80000008U, 0, r); unsigned bits = r[0] & 0xFF;
    return bits >= 32 && bits <= 52 ? bits : 36;
}
const char* hw_memtype_name(int type) {
    switch (type) {
    case MEMTYPE_UC: return "UC"; case MEMTYPE_WC: return "WC"; case MEMTYPE_WT: return "WT"; case MEMTYPE_WP: return "WP";
    case MEMTYPE_WB: return "WB"; case MEMTYPE_CONFLICT: return "conflict"; case MEMTYPE_UNKNOWN: return "unknown";
    default: return "reserved";
    }
}
bool hw_mtrr_read(struct hw_mtrr* m) {
    memset(m, 0, sizeof(*m));
    uint64_t cap, def;
    if (!mtrr_present() || hw_rdmsr_safe(0xFE, &cap) || hw_rdmsr_safe(0x2FF, &def)) return false;
    m->present = true; m->cap = cap; m->def_type = def;
    m->count = (unsigned)(cap & 0xFF); if (m->count > HW_MTRR_MAX) m->count = HW_MTRR_MAX;
    m->physical_mask = ((1ULL << physical_bits()) - 1) & ~0xFFFULL;
    for (unsigned i = 0; i < m->count; i++)
        if (hw_rdmsr_safe(0x200 + 2 * i, &m->base[i]) || hw_rdmsr_safe(0x201 + 2 * i, &m->mask[i])) { m->count = i; break; }
    return true;
}
/* Effective MTRR type of one physical address (fixed ranges are below 1 MiB and not consulted here). */
int hw_mtrr_type(const struct hw_mtrr* m, uint64_t physical) {
    if (!m->present) return MEMTYPE_UNKNOWN;
    if (!(m->def_type & (1U << 11))) return MEMTYPE_UC;                      /* MTRRs disabled: everything UC */
    int found = -1;
    for (unsigned i = 0; i < m->count; i++) {
        if (!(m->mask[i] & (1U << 11))) continue;
        uint64_t mask = m->mask[i] & m->physical_mask, base = m->base[i] & m->physical_mask;
        if ((physical & mask) != (base & mask)) continue;
        int type = (int)(m->base[i] & 0xFF);
        if (found < 0 || found == type) found = type;
        else if (found == MEMTYPE_UC || type == MEMTYPE_UC) found = MEMTYPE_UC;
        else if ((found == MEMTYPE_WT && type == MEMTYPE_WB) || (found == MEMTYPE_WB && type == MEMTYPE_WT)) found = MEMTYPE_WT;
        else return MEMTYPE_CONFLICT;
    }
    return found >= 0 ? found : (int)(m->def_type & 0xFF);
}
static void number(uint64_t value) { char text[24]; uint_to_str((uint32_t)value, text); vga_print(text); }
static void hex64(uint64_t value) {
    char text[19] = "0x"; for (int i = 0; i < 16; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (60 - 4 * i)) & 15]; text[18] = 0;
    vga_print(text);
}
void hwinfo_display_report(uint64_t physical, uint32_t width, uint32_t height, uint32_t stride, uint32_t format, bool write_combining) {
    struct hw_mtrr m; hw_mtrr_read(&m);
    vga_print("[64] DISPLAY "); number(width); vga_print("x"); number(height); vga_print(" stride="); number(stride);
    vga_print(format == 2 ? " XBGR8888" : " XRGB8888"); vga_print(" at "); hex64(physical);
    vga_print(" MTRR="); vga_print(hw_memtype_name(hw_mtrr_type(&m, physical)));
    vga_print(write_combining ? " console=WC (PAT)" : " console=direct map");
    const struct nexus_boot_display* d = &boot_info.display;
    static const char* const sources[] = {"none", "UEFI-active", "UEFI-discovered", "VBE-DDC"};
    static const char* const reasons[] = {"kept", "video=", "native", "default", "firmware", "largest"};
    vga_print(" EDID="); vga_print(sources[d->edid_source <= NEXUS_EDID_DDC ? d->edid_source : 0]);
    if (d->edid_size >= 128) {
        char vendor[4] = {(char)('@' + (d->edid[8] >> 2 & 31)), (char)('@' + ((d->edid[8] & 3) << 3 | d->edid[9] >> 5)), (char)('@' + (d->edid[9] & 31)), 0};
        vga_print(" "); vga_print(vendor);
        if (d->native_width) { vga_print(" preferred "); number(d->native_width); vga_print("x"); number(d->native_height); }
        if (d->edid_virtual) vga_print(" (virtual display)");
    }
    vga_print(" mode="); vga_print(reasons[d->reason <= NEXUS_MODE_LARGEST ? d->reason : 0]); vga_print("\n");
}

/* ---- ACPI tables (read-only walk of the RSDT/XSDT acpi.c validated; below 4 GiB, checksummed) ---- */
#define LOW_LIMIT 0x100000000ULL
static bool table_ok(uint64_t address, uint32_t* length) {
    if (!address || address >= LOW_LIMIT - 36) return false;
    const uint8_t* t = phys_to_virt(address); memcpy(length, t + 4, 4);
    if (*length < 36 || *length > 0x1000000 || address + *length > LOW_LIMIT) return false;
    uint8_t sum = 0; for (uint32_t i = 0; i < *length; i++) sum = (uint8_t)(sum + t[i]);
    return sum == 0;
}
/* The nth table with this signature (NULL: the nth table of any kind); *physical gets its address. */
const uint8_t* hw_acpi_table(const char* signature, unsigned nth, uint64_t* physical) {
    if (!acpi.present || !acpi.rsdp || acpi.rsdp >= LOW_LIMIT - 36) return NULL;
    const uint8_t* rsdp = phys_to_virt(acpi.rsdp);
    uint64_t root = 0; unsigned width = acpi.xsdt ? 8 : 4; uint32_t length;
    if (acpi.xsdt) memcpy(&root, rsdp + 24, 8); else memcpy(&root, rsdp + 16, 4);
    if (!table_ok(root, &length)) return NULL;
    const uint8_t* list = phys_to_virt(root);
    for (uint32_t at = 36; at + width <= length; at += width) {
        uint64_t address = 0; uint32_t size; memcpy(&address, list + at, width);
        if (!table_ok(address, &size)) continue;
        const uint8_t* table = phys_to_virt(address);
        if (signature && memcmp(table, signature, 4)) continue;
        if (nth--) continue;
        if (physical) *physical = address;
        return table;
    }
    return NULL;
}

/* ---- TSC calibration without the PIT ---- */
static inline uint64_t tsc_now(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
/* HPET (ACPI "HPET" table, IA-PC HPET 1.0a): main counter at +0xF0, period in femtoseconds in the
 * capabilities register; the counter is enabled if the firmware left it stopped. 10 ms window. */
static uint64_t hpet_calibrate(void) {
    const uint8_t* table = hw_acpi_table("HPET", 0, NULL);
    if (!table) return 0;
    uint32_t length; memcpy(&length, table + 4, 4);
    uint64_t base; memcpy(&base, table + 44, 8);
    if (length < 56 || table[40] != 0 || !base || (base & 0x3FF)) return 0;          /* system-memory GAS */
    volatile uint8_t* h = pci_mmio_map(base, 0x400);
    if (!h) return 0;
    uint64_t caps = *(volatile uint64_t*)h; uint32_t period = (uint32_t)(caps >> 32);
    if (!period || period > 100000000U || caps == ~0ULL) return 0;                     /* spec: <= 100 ns */
    volatile uint64_t* config = (volatile uint64_t*)(h + 0x10);
    if (!(*config & 1)) *config |= 1;                                                  /* ENABLE_CNF */
    volatile uint32_t* counter = (volatile uint32_t*)(h + 0xF0);
    uint32_t want = (uint32_t)(10000000000000ULL / period), first = *counter, now = first;
    for (unsigned spins = 0; now == first; spins++) { if (spins > 1000000) return 0; now = *counter; }   /* not counting */
    uint32_t start = now; uint64_t tsc_start = tsc_now();
    for (unsigned spins = 0; (uint32_t)(now - start) < want; spins++) { if (spins > 50000000) return 0; now = *counter; }
    uint64_t tsc = tsc_now() - tsc_start, ns = (uint64_t)(now - start) * period / 1000000;
    return ns ? tsc * 1000000000ULL / ns : 0;
}
/* ACPI PM timer (FADT PM_TMR_BLK / X_PM_TMR_BLK): 3.579545 MHz, 24 or 32 bits (TMR_VAL_EXT). */
static uint64_t pmtimer_calibrate(void) {
    const uint8_t* fadt = hw_acpi_table("FACP", 0, NULL);
    if (!fadt) return 0;
    uint32_t length, port = 0, flags = 0; memcpy(&length, fadt + 4, 4);
    if (length >= 80) memcpy(&port, fadt + 76, 4);
    if (length >= 116) memcpy(&flags, fadt + 112, 4);
    if (length >= 220) { uint64_t x; memcpy(&x, fadt + 212, 8); if (x && fadt[208] == 1) port = (uint32_t)x; }
    if (!port || port > 0xFFFC || (flags & (1U << 20))) return 0;                      /* none, or hardware-reduced ACPI */
    uint32_t mask = flags & (1U << 8) ? 0xFFFFFFFFU : 0xFFFFFFU, want = 35795;          /* 10 ms */
    uint32_t first = port_dword_in((uint16_t)port) & mask, now = first;
    for (unsigned spins = 0; now == first; spins++) { if (spins > 100000) return 0; now = port_dword_in((uint16_t)port) & mask; }
    uint32_t start = now; uint64_t tsc_start = tsc_now();
    for (unsigned spins = 0; ((now - start) & mask) < want; spins++) { if (spins > 20000000) return 0; now = port_dword_in((uint16_t)port) & mask; }
    uint64_t tsc = tsc_now() - tsc_start, ticks = (now - start) & mask;
    return tsc * 3579545ULL / ticks;
}
/* No usable PIT (gated or absent on legacy-free boards): HPET, then the ACPI PM timer (pm_first: the
 * other way round). 0 when neither works. */
uint64_t hw_tsc_calibrate(bool pm_first, const char** source) {
    for (unsigned pass = 0; pass < 2; pass++) {
        bool pm = (pass == 0) == pm_first;
        uint64_t hz = pm ? pmtimer_calibrate() : hpet_calibrate();
        if (hz >= 100000000ULL && hz <= 10000000000ULL) { *source = pm ? "ACPI-PM-timer" : "HPET"; return hz; }
    }
    return 0;
}
