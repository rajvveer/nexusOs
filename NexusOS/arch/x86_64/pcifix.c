/* Memory BARs the firmware left unassigned (base 0, nonzero size): a device behind a PCI-PCI bridge gets
 * space inside that bridge's memory window (the prefetchable window for prefetchable BARs when there is
 * one), first fit, aligned to the BAR size, avoiding every assigned BAR and child-bridge window on the same
 * bus; then memory decoding is enabled. Bus 0 devices are only reported: without ACPI _CRS the host
 * bridge's apertures are unknown, and guessing could overlap chipset MMIO. Only BAR registers that read a
 * zero base are sized (and, when one turns out unassigned, the BARs of its bus), so a fully assigned
 * machine is not touched beyond config reads. Runs once at boot, before the drivers' pci_init() scan (which
 * then reads the new BARs). Boot option pci.test-unassign first clears BAR0 of the first device behind a
 * bridge (tests/test_x64_hwready.py exercises the path on QEMU). */
#include "native.h"
#include "boot_info.h"
#include "../../kernel/pci.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define MAX_BRIDGES 64
#define MAX_RANGES 256
#define MAX_MISSING 32
struct bridge { uint8_t bus, device, function, secondary; uint64_t mem_base, mem_limit, pref_base, pref_limit; };
struct missing { uint8_t bus, device, function, index; bool wide, prefetch; uint64_t size; };
static struct bridge bridges[MAX_BRIDGES]; static unsigned bridge_count;
static struct missing missing[MAX_MISSING]; static unsigned missing_count;
static struct { uint64_t base, end; } used[MAX_RANGES]; static unsigned used_count;
static bool bus_scanned[256];

static void hex(uint64_t value, unsigned digits) {
    char text[17]; for (unsigned i = 0; i < digits; i++) text[i] = "0123456789abcdef"[(value >> (4 * (digits - 1 - i))) & 15];
    text[digits] = 0; vga_print(text);
}
static void location(uint8_t bus, uint8_t device, uint8_t function) { hex(bus, 2); vga_print(":"); hex(device, 2); vga_print("."); hex(function, 1); }
static void add_used(uint64_t base, uint64_t size) {
    if (size && used_count < MAX_RANGES) { used[used_count].base = base; used[used_count++].end = base + size; }
}
/* Memory BAR `index`: base, 64-bit/prefetchable flags, and (size_it) its size; 0 size for I/O or absent BARs. */
static uint64_t bar_info(uint8_t bus, uint8_t device, uint8_t function, unsigned index, bool size_it, uint64_t* base, bool* wide, bool* prefetch) {
    uint8_t reg = (uint8_t)(0x10 + 4 * index);
    uint32_t low = pci_read32(bus, device, function, reg);
    *wide = false; *prefetch = false; *base = 0;
    if (low & 1) return 0;
    *wide = ((low >> 1) & 3) == 2; *prefetch = (low & 8) != 0;
    if (*wide && index == 5) { *wide = false; return 0; }
    uint32_t high = *wide ? pci_read32(bus, device, function, (uint8_t)(reg + 4)) : 0;
    *base = ((uint64_t)high << 32) | (low & ~0xFULL);
    if (!size_it) return 0;
    uint16_t command = pci_read16(bus, device, function, PCI_COMMAND);
    pci_write32(bus, device, function, PCI_COMMAND, command & ~2U);                   /* no decoding while sizing */
    pci_write32(bus, device, function, reg, 0xFFFFFFFFU);
    uint32_t mask_low = pci_read32(bus, device, function, reg), mask_high = 0xFFFFFFFFU;
    pci_write32(bus, device, function, reg, low);
    if (*wide) {
        pci_write32(bus, device, function, (uint8_t)(reg + 4), 0xFFFFFFFFU);
        mask_high = pci_read32(bus, device, function, (uint8_t)(reg + 4));
        pci_write32(bus, device, function, (uint8_t)(reg + 4), high);
    }
    pci_write32(bus, device, function, PCI_COMMAND, command);
    if (!(mask_low & ~0xFU)) return 0;
    uint64_t mask = ((uint64_t)mask_high << 32) | (mask_low & ~0xFULL);
    if (!*wide) mask |= 0xFFFFFFFF00000000ULL;
    return ~mask + 1;
}
typedef void (*visitor)(uint8_t bus, uint8_t device, uint8_t function);
/* only_bus < 0: bus 0 and every bus behind a bridge (breadth first; no brute-force 256-bus scan). */
static void each_function(visitor visit, int only_bus) {
    uint8_t queue[256]; bool seen[256] = {false}; unsigned head = 0, tail = 0;
    queue[tail++] = (uint8_t)(only_bus < 0 ? 0 : only_bus); seen[queue[0]] = true;
    while (head < tail) {
        uint8_t bus = queue[head++];
        for (unsigned device = 0; device < 32; device++) {
            if (pci_read16(bus, (uint8_t)device, 0, PCI_VENDOR_ID) == 0xFFFF) continue;
            unsigned functions = pci_read8(bus, (uint8_t)device, 0, 0x0E) & 0x80 ? 8 : 1;
            for (unsigned function = 0; function < functions; function++) {
                if (pci_read16(bus, (uint8_t)device, (uint8_t)function, PCI_VENDOR_ID) == 0xFFFF) continue;
                visit(bus, (uint8_t)device, (uint8_t)function);
                uint8_t secondary = pci_read8(bus, (uint8_t)device, (uint8_t)function, 0x19);
                if (only_bus < 0 && (pci_read8(bus, (uint8_t)device, (uint8_t)function, 0x0E) & 0x7F) == 1 && secondary && !seen[secondary]) {
                    seen[secondary] = true; queue[tail++] = secondary;
                }
            }
        }
    }
}
/* Pass 1: bridge windows (config reads only) and BARs that read a zero base and size as nonzero. */
static void survey(uint8_t bus, uint8_t device, uint8_t function) {
    uint8_t type = pci_read8(bus, device, function, 0x0E) & 0x7F;
    if (type == 1 && bridge_count < MAX_BRIDGES) {
        struct bridge* b = &bridges[bridge_count++];
        uint32_t memory = pci_read32(bus, device, function, 0x20), pref = pci_read32(bus, device, function, 0x24);
        b->bus = bus; b->device = device; b->function = function; b->secondary = pci_read8(bus, device, function, 0x19);
        b->mem_base = (uint64_t)(memory & 0xFFF0) << 16; b->mem_limit = ((uint64_t)(memory >> 16 & 0xFFF0) << 16) | 0xFFFFF;
        b->pref_base = (uint64_t)(pref & 0xFFF0) << 16; b->pref_limit = ((uint64_t)(pref >> 16 & 0xFFF0) << 16) | 0xFFFFF;
        if ((pref & 0xF) == 1) {
            b->pref_base |= (uint64_t)pci_read32(bus, device, function, 0x28) << 32;
            b->pref_limit |= (uint64_t)pci_read32(bus, device, function, 0x2C) << 32;
        }
        return;
    }
    if (type != 0) return;
    for (unsigned i = 0; i < 6; i++) {
        uint64_t base; bool wide, prefetch;
        bar_info(bus, device, function, i, false, &base, &wide, &prefetch);
        unsigned index = i; if (wide) i++;
        if (base || (pci_read32(bus, device, function, (uint8_t)(0x10 + 4 * index)) & 1)) continue;
        uint64_t size = bar_info(bus, device, function, index, true, &base, &wide, &prefetch);
        if (size && !base && missing_count < MAX_MISSING)
            missing[missing_count++] = (struct missing){bus, device, function, (uint8_t)index, wide, prefetch, size};
    }
}
/* Pass 2 (only for a bus with a missing BAR): what is taken on it - assigned BARs and child-bridge windows. */
static void taken(uint8_t bus, uint8_t device, uint8_t function) {
    uint8_t type = pci_read8(bus, device, function, 0x0E) & 0x7F;
    unsigned bars = type == 0 ? 6 : type == 1 ? 2 : 0;
    for (unsigned i = 0; i < bars; i++) {
        uint64_t base; bool wide, prefetch;
        bar_info(bus, device, function, i, false, &base, &wide, &prefetch);
        if (base) add_used(base, bar_info(bus, device, function, i, true, &base, &wide, &prefetch));
        if (wide) i++;
    }
    for (unsigned k = 0; k < bridge_count && type == 1; k++)
        if (bridges[k].bus == bus && bridges[k].device == device && bridges[k].function == function) {
            if (bridges[k].mem_base <= bridges[k].mem_limit) add_used(bridges[k].mem_base, bridges[k].mem_limit - bridges[k].mem_base + 1);
            if (bridges[k].pref_base <= bridges[k].pref_limit) add_used(bridges[k].pref_base, bridges[k].pref_limit - bridges[k].pref_base + 1);
        }
}
static uint64_t first_fit(uint64_t window_base, uint64_t window_limit, uint64_t size) {
    if (!window_base || window_base > window_limit || size > window_limit - window_base + 1) return 0;
    for (uint64_t at = (window_base + size - 1) & ~(size - 1); at >= window_base && at + size - 1 <= window_limit; at += size) {
        bool clash = false;
        for (unsigned i = 0; i < used_count && !clash; i++) clash = at < used[i].end && used[i].base < at + size;
        if (!clash) return at;
    }
    return 0;
}
static bool test_cleared;
static void test_unassign(uint8_t bus, uint8_t device, uint8_t function) {
    if (test_cleared || !bus || (pci_read8(bus, device, function, 0x0E) & 0x7F) != 0) return;
    uint32_t bar = pci_read32(bus, device, function, 0x10);
    if ((bar & 1) || !(bar & ~0xFU)) return;
    pci_write32(bus, device, function, PCI_COMMAND, pci_read16(bus, device, function, PCI_COMMAND) & ~2U);
    pci_write32(bus, device, function, 0x10, bar & 0xFU);
    if (((bar >> 1) & 3) == 2) pci_write32(bus, device, function, 0x14, 0);
    test_cleared = true;
    vga_print("[64] PCI test: BAR0 of "); location(bus, device, function); vga_print(" cleared\n");
}
void pci_bar_fixup(void) {
    if (boot_option("pci.test-unassign")) each_function(test_unassign, -1);
    bridge_count = missing_count = 0;
    each_function(survey, -1);
    unsigned assigned = 0;
    for (unsigned m = 0; m < missing_count; m++) {
        struct missing* x = &missing[m];
        const struct bridge* parent = NULL;
        for (unsigned k = 0; k < bridge_count && x->bus; k++) if (bridges[k].secondary == x->bus) parent = &bridges[k];
        if (parent && !bus_scanned[x->bus]) {
            used_count = 0; each_function(taken, x->bus);
            bus_scanned[x->bus] = true;
        }
        uint64_t at = 0;
        if (parent && x->prefetch && (x->wide || parent->pref_limit < 0x100000000ULL)) at = first_fit(parent->pref_base, parent->pref_limit, x->size);
        if (parent && !at && parent->mem_limit < 0x100000000ULL) at = first_fit(parent->mem_base, parent->mem_limit, x->size);
        vga_print("[64] PCI "); location(x->bus, x->device, x->function); vga_print(" BAR"); hex(x->index, 1); vga_print(" size 0x"); hex(x->size, 8);
        if (!at) {
            vga_print(parent ? " unassigned by firmware: no room in the bridge window; device left without it\n" :
                               " unassigned by firmware on the root bus; device left without it\n");
            continue;
        }
        uint8_t reg = (uint8_t)(0x10 + 4 * x->index);
        uint16_t command = pci_read16(x->bus, x->device, x->function, PCI_COMMAND);
        pci_write32(x->bus, x->device, x->function, PCI_COMMAND, command & ~2U);
        pci_write32(x->bus, x->device, x->function, reg, (uint32_t)at | (pci_read32(x->bus, x->device, x->function, reg) & 0xFU));
        if (x->wide) pci_write32(x->bus, x->device, x->function, (uint8_t)(reg + 4), (uint32_t)(at >> 32));
        pci_write32(x->bus, x->device, x->function, PCI_COMMAND, command | 2U);
        add_used(at, x->size); assigned++;
        vga_print(" assigned 0x"); hex(at, at >> 32 ? 16 : 8); vga_print(" in the window of bridge ");
        location(parent->bus, parent->device, parent->function); vga_print("\n");
    }
    if (missing_count) {
        vga_print("[64] PCI BAR fixup: "); char text[12]; uint_to_str(assigned, text); vga_print(text);
        vga_print(" assigned, "); uint_to_str(missing_count - assigned, text); vga_print(text); vga_print(" left unassigned\n");
    }
}
