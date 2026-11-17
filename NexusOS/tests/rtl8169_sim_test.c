/* Register-level simulator for the Realtek PCIe Ethernet driver (arch/x86_64/rtl8169.c + netdrv.c, with the real
 * PCI BAR code of pcidev.c). The simulated chip models what the driver depends on:
 *  - MMIO registers with the RTL8168 (16-bit IMR/ISR at 0x3C/0x3E, TxPoll 0x38) and RTL8125 (32-bit IMR/ISR at
 *    0x38/0x3C, TxPoll 0x90) layouts, write-1-to-clear status, Cfg9346 write protection of the MAC address,
 *    a self-clearing chip reset (optionally stuck), TxConfig XID, MCU FIFO/link-list status, RXDV gate;
 *  - the MAC OCP window (0xB0), the PHY OCP window (0xB8) with busy/ready flags, ERI (EXGMAC + OOB, byte masks),
 *    the PCIe PHY window (5-bit addresses on 8168, 7-bit on 8125), CSI access to extended config space;
 *  - the internal PHY: MII registers at 0xA400, PHY ID, BMCR reset/power-down/autonegotiation, latched-low BMSR
 *    link, PHYSR speed/duplex (10..5000 Mb/s), the 0xA436/0xA438 and 0xB87C/0xB87E parameter RAMs, and the PHY
 *    MCU patch handshake (0xB820 bit 4 request -> 0xB800 bit 6 ready) that the linux-firmware programs poll;
 *  - DMA: transmit descriptors fetched on the doorbell (FS/LS/EOR checked, ring bounds checked), receive into
 *    owned descriptors (FCS appended, CRC/fragment/overflow cases), RDU when the ring is full;
 *  - PCI configuration space with capability list (PM, MSI, PCIe link control, MSI-X), BAR sizing, MSI/MSI-X
 *    edge semantics (a message only on a 0 -> non-zero transition of status & mask) and level INTx;
 *  - failures: all-ones reads (device gone), lost power (registers and config space cleared), transmit hang,
 *    MSI-X/MSI setup failures, missing/corrupted firmware files, hostile firmware programs.
 * Scenarios run the driver's real entry points against the chips of the user's PC (RTL8125B, XID 0x641) and every
 * other supported MAC version. */
#define RTL_TEST_MMIO 1
#define NEXUS_HOST_TEST 1
#include "../arch/x86_64/native.h"
#include "../kernel/string.h"
#include "../kernel/net.h"
#include "../kernel/vga.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>

/* ---------------- simulated chip ---------------- */
struct sim_desc { volatile uint32_t opts1, opts2; volatile uint64_t address; };
static struct {
    unsigned version; uint16_t xid; bool is8125;
    uint8_t regs[0x10000];
    uint16_t mac_ocp[0x8000], phy_ocp[0x8000], sram_g[0x10000], sram_8125[0x10000];
    uint32_t mac_ocp_addr, phy_ocp_addr, phy_flag_reads; bool phy_read_pending;
    uint32_t eri[0x400], eri_oob[0x400]; bool eri_read_pending;
    uint16_t ephy[0x80];
    unsigned reset_reads; bool reset_stuck;
    bool dead, tx_hang;
    unsigned rx_index, tx_index;
    bool link, full, link_latch_down; unsigned speed;
    unsigned patch_ready_reads;
    uint32_t sram_g_addr, sram_8125_addr;
    /* observations */
    unsigned resets, doorbells, tx_frames, rx_missed, rx_filtered, locked_mac_writes, ephy_writes, mac_ocp_writes, phy_ocp_writes;
    unsigned patch_requests, firmware_mac_writes;
    uint8_t tx_last[2048]; unsigned tx_last_length;
    bool prev_level; unsigned messages;
} sim;
static uint8_t cfg[4096];          /* the NIC's PCI configuration space */
static bool bar_sizing[6];
static const uint32_t bar_mask[6] = {0xFFFFFF01, 0, 0xFFFF0004, 0xFFFFFFFF, 0xFFFFC004, 0xFFFFFFFF};
static bool ecam_present = true;
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32v(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16v(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32v(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static unsigned R_IMR(void) { return sim.is8125 ? 0x38 : 0x3C; }
static unsigned R_ISR(void) { return sim.is8125 ? 0x3C : 0x3E; }
static uint32_t imr(void) { return sim.is8125 ? le32v(sim.regs + 0x38) : le16(sim.regs + 0x3C); }
static uint32_t isr(void) { return sim.is8125 ? le32v(sim.regs + 0x3C) : le16(sim.regs + 0x3E); }
static void set_isr(uint32_t v) { if (sim.is8125) put32v(sim.regs + 0x3C, v); else put16v(sim.regs + 0x3E, (uint16_t)v); }
static bool msix_on(void) { return (le16(cfg + 0xB2) & 0x8000) != 0; }
static bool msi_on(void) { return (le16(cfg + 0x52) & 1) != 0; }
static void irq_check(void) {
    bool level = (isr() & imr()) != 0;
    if ((msix_on() || msi_on()) && level && !sim.prev_level) sim.messages++;
    sim.prev_level = level;
}
static void raise(uint32_t bits) { set_isr(isr() | bits); irq_check(); }

/* PHY OCP: the internal PHY */
#define PHY_PHYSR_SIM 0xA434
static uint16_t phy_physr(void) {
    unsigned s = sim.speed == 10 ? 0 : sim.speed == 100 ? 0x10 : sim.speed == 1000 ? 0x20 : sim.speed == 2500 ? 0x210 : sim.speed == 5000 ? 0x220 : 0;
    return (uint16_t)(s | (sim.full ? 8 : 0) | (sim.link ? 4 : 0));
}
static bool phy_powered(void) { return !(sim.phy_ocp[0xA400 / 2] & 0x0800); }
static uint16_t phy_read_reg(uint32_t a) {
    if (a == 0xA402) {
        bool up = sim.link && phy_powered() && !sim.link_latch_down; sim.link_latch_down = false;
        return (uint16_t)(0x7949 | (up ? 0x0024 : 0));
    }
    if (a == 0xA400) { uint16_t v = sim.phy_ocp[a / 2]; sim.phy_ocp[a / 2] &= (uint16_t)~(0x8000 | 0x0200); return v; }  /* reset, restart self-clear */
    if (a == 0xA404) return 0x001C;
    if (a == 0xA406) return sim.is8125 ? 0xC840 : 0xC800;
    if (a == PHY_PHYSR_SIM) return phy_physr();
    if (a == 0xB800) {                                       /* patch ready follows the request after two reads */
        if (sim.phy_ocp[0xB820 / 2] & 0x10) { if (sim.patch_ready_reads && --sim.patch_ready_reads == 0) sim.phy_ocp[0xB800 / 2] |= 0x40; }
        else sim.phy_ocp[0xB800 / 2] &= (uint16_t)~0x40;
        return sim.phy_ocp[a / 2];
    }
    if (a == 0xA438) return sim.sram_g[sim.sram_g_addr & 0xFFFF];
    if (a == 0xB87E) return sim.sram_8125[sim.sram_8125_addr & 0xFFFF];
    return sim.phy_ocp[a / 2];
}
static void phy_write_reg(uint32_t a, uint16_t v) {
    sim.phy_ocp_writes++;
    if (a == 0xA436) { sim.sram_g_addr = v; return; }
    if (a == 0xA438) { sim.sram_g[sim.sram_g_addr & 0xFFFF] = v; sim.sram_g_addr++; return; }   /* auto-increment */
    if (a == 0xB87C) { sim.sram_8125_addr = v; return; }
    if (a == 0xB87E) { sim.sram_8125[sim.sram_8125_addr & 0xFFFF] = v; return; }
    if (a == 0xB820) { if ((v & 0x10) && !(sim.phy_ocp[a / 2] & 0x10)) { sim.patch_requests++; sim.patch_ready_reads = 2; } }
    sim.phy_ocp[a / 2] = v;
}
static void chip_reset(void) {
    sim.resets++;
    if (sim.is8125) { put32v(sim.regs + 0x38, 0); put32v(sim.regs + 0x3C, 0); } else { put16v(sim.regs + 0x3C, 0); put16v(sim.regs + 0x3E, 0); }
    sim.regs[0x37] = 0x10; sim.reset_reads = 3;
    sim.rx_index = sim.tx_index = 0;
    sim.mac_ocp[0xE00E / 2] |= 1U << 13;
    sim.prev_level = false;
}
static void tx_kick(void) {
    sim.doorbells++;
    if (!(sim.regs[0x37] & 0x04) || sim.tx_hang) return;
    uint64_t base = le32v(sim.regs + 0x20) | (uint64_t)le32v(sim.regs + 0x24) << 32;
    assert(base && !(base & 255));
    for (unsigned n = 0; n < 256; n++) {
        assert(sim.tx_index < 64);                          /* the driver's ring has 64 descriptors: EOR must wrap it */
        struct sim_desc* d = (struct sim_desc*)(uintptr_t)(base + 16ULL * sim.tx_index);
        uint32_t o = d->opts1;
        if (!(o & 0x80000000U)) break;
        assert((o & 0x30000000U) == 0x30000000U);           /* one buffer per frame: FS and LS */
        unsigned length = o & 0xFFFF; assert(length >= 60 && length <= 1518);
        assert(d->address); memcpy(sim.tx_last, (const void*)(uintptr_t)d->address, length); sim.tx_last_length = length; sim.tx_frames++;
        d->opts1 = o & ~0x80000000U;
        sim.tx_index = (o & 0x40000000U) ? 0 : sim.tx_index + 1;
        raise(0x0004);
    }
}
/* Deliver a frame (FCS appended). error: 0, 1 = CRC error, 2 = split across two descriptors. */
static bool rx_frame(const uint8_t* frame, unsigned length, int error) {
    if (!(sim.regs[0x37] & 0x08) || (le32v(sim.regs + 0xF0) & (1U << 19)) || !(le32v(sim.regs + 0x44) & 0x0A)) { sim.rx_filtered++; return false; }
    uint64_t base = le32v(sim.regs + 0xE4) | (uint64_t)le32v(sim.regs + 0xE8) << 32;
    assert(base && !(base & 255));
    unsigned parts = error == 2 ? 2 : 1, done = 0;
    for (unsigned p = 0; p < parts; p++) {
        assert(sim.rx_index < 256);
        struct sim_desc* d = (struct sim_desc*)(uintptr_t)(base + 16ULL * sim.rx_index);
        uint32_t o = d->opts1;
        if (!(o & 0x80000000U)) { sim.rx_missed++; raise(0x0010); return false; }   /* RDU */
        unsigned capacity = o & 0x3FFF, total = length + 4, chunk = parts == 2 ? (p ? total - total / 2 : total / 2) : total;
        assert(capacity >= chunk);
        uint8_t* buffer = (uint8_t*)(uintptr_t)d->address; assert(buffer);
        for (unsigned i = 0; i < chunk; i++) { unsigned at = done + i; buffer[i] = at < length ? frame[at] : 0xCC; }
        done += chunk;
        uint32_t flags = (parts == 1 ? 0x30000000U : p ? 0x10000000U : 0x20000000U) | (error == 1 ? (1U << 21) | (1U << 19) : 0);
        d->opts2 = 0; d->opts1 = (o & 0x40000000U) | flags | (parts == 2 ? chunk : total);
        sim.rx_index = (o & 0x40000000U) ? 0 : sim.rx_index + 1;
    }
    raise(error == 1 ? 0x0002 : 0x0001);
    return true;
}
static void sim_link(bool up, unsigned speed, bool full) {
    if (!up && sim.link) sim.link_latch_down = true;
    sim.link = up; sim.speed = speed; sim.full = full;
    raise(0x0020);
}
static void power_on(unsigned version, uint16_t xid) {
    memset(&sim, 0, sizeof(sim));
    sim.version = version; sim.xid = xid; sim.is8125 = version >= 61;
    sim.regs[0xD3] = 0x80 | 0x30 | 0x02;                     /* NOW_IS_OOB set by firmware, FIFOs empty, link list ready */
    put16v(sim.regs + 0xE0, 0x2060);                          /* CPlusCmd: firmware left VLAN + checksum on */
    put32v(sim.regs + 0xF0, 1U << 19);                        /* RXDV gated */
    put32v(sim.regs + 0x44, 0x0000000E);
    static const uint8_t mac[6] = {0x00, 0xE0, 0x4C, 0x68, 0x00, 0x25};
    if (sim.is8125) memcpy(sim.regs + 0x19E0, mac, 6);
    else { sim.eri[0xE0 / 4] = le32v(mac); sim.eri[0xE4 / 4] = le16(mac + 4); }
    sim.phy_ocp[0xA400 / 2] = 0x0800;                         /* PHY powered down */
    sim.mac_ocp[0xE00E / 2] = 1U << 13;
    sim.regs[0x56] = 0x01; sim.regs[0x53] = 0x80;             /* ASPM + CLKREQ left on by the BIOS */
    memset(cfg, 0, sizeof(cfg));
    put16v(cfg + 0, 0x10EC); put16v(cfg + 2, version >= 70 ? 0x8126 : sim.is8125 ? 0x8125 : 0x8168);
    put16v(cfg + 6, 0x0010); cfg[8] = 5; cfg[9] = 0; cfg[10] = 0; cfg[11] = 2; cfg[0x3C] = 11; cfg[0x3D] = 1;
    put32v(cfg + 0x10, 0xD001); put32v(cfg + 0x18, 0xFC600004); put32v(cfg + 0x1C, 0); put32v(cfg + 0x20, 0xFC610004);
    cfg[0x34] = 0x40;
    cfg[0x40] = 0x01; cfg[0x41] = 0x50;                       /* PM */
    cfg[0x50] = 0x05; cfg[0x51] = 0x70; put16v(cfg + 0x52, 0x0080);   /* MSI, 64-bit */
    cfg[0x70] = 0x10; cfg[0x71] = 0xB0; put16v(cfg + 0x80, 0x0003);   /* PCIe, link control: ASPM L0s+L1 on */
    cfg[0xB0] = 0x11; cfg[0xB1] = 0x00; put16v(cfg + 0xB2, 0x0003); put32v(cfg + 0xB4, 0x00000004);   /* MSI-X: 4 entries, BAR4 */
    memset(bar_sizing, 0, sizeof(bar_sizing));
}

/* ---------------- register access for the driver ---------------- */
static uint32_t reg_read(unsigned r, unsigned width) {
    uint32_t ones = width == 4 ? 0xFFFFFFFFU : width == 2 ? 0xFFFF : 0xFF;
    if (sim.dead) return ones;
    assert(r + width <= sizeof(sim.regs));
    switch (r) {
    case 0x40: assert(width == 4); return (le32v(sim.regs + 0x40) & 0x000FF7FFU) | (uint32_t)sim.xid << 20 | (1U << 11);
    case 0x37: assert(width == 1);
        if ((sim.regs[0x37] & 0x10) && !sim.reset_stuck && sim.reset_reads && --sim.reset_reads == 0) sim.regs[0x37] &= (uint8_t)~0x10;
        return sim.regs[0x37];
    case 0xB0: assert(width == 4); return sim.mac_ocp[(sim.mac_ocp_addr & 0xFFFF) / 2];
    case 0xB8: assert(width == 4);   /* a read is busy for one poll, then the flag and the data stay until the next command */
        if (sim.phy_read_pending) {
            if (sim.phy_flag_reads && --sim.phy_flag_reads) return sim.phy_ocp_addr << 15;
            sim.phy_read_pending = false; put32v(sim.regs + 0xB8, 0x80000000U | sim.phy_ocp_addr << 15 | phy_read_reg(sim.phy_ocp_addr));
        }
        return le32v(sim.regs + 0xB8);
    case 0xE2: assert(width == 2); return le16(sim.regs + 0xE2) | (sim.version >= 63 ? 0x0103 : 0);
    }
    if (r == R_ISR()) assert(width == (sim.is8125 ? 4U : 2U));
    uint32_t v = 0; for (unsigned i = 0; i < width; i++) v |= (uint32_t)sim.regs[r + i] << (8 * i);
    return v;
}
static void reg_write(unsigned r, uint32_t v, unsigned width) {
    if (sim.dead) return;
    assert(r + width <= sizeof(sim.regs));
    if (r < 6 && sim.regs[0x50] != 0xC0) { sim.locked_mac_writes++; return; }
    if (r == R_ISR()) { assert(width == (sim.is8125 ? 4U : 2U)); set_isr(isr() & ~v); irq_check(); return; }
    if (r == R_IMR()) { assert(width == (sim.is8125 ? 4U : 2U)); for (unsigned i = 0; i < width; i++) sim.regs[r + i] = (uint8_t)(v >> (8 * i)); irq_check(); return; }
    switch (r) {
    case 0x37: assert(width == 1);
        if (v & 0x10) { chip_reset(); return; }
        sim.regs[0x37] = (uint8_t)v; return;
    case 0x38: assert(!sim.is8125 && width == 1); if (v & 0x40) tx_kick(); return;
    case 0x90: if (sim.is8125) { assert(width == 2); if (v & 1) tx_kick(); return; } break;
    case 0xB0: assert(width == 4); {
        uint32_t address = (v >> 15) & 0xFFFE;
        if (v & 0x80000000U) { sim.mac_ocp[address / 2] = (uint16_t)v; sim.mac_ocp_writes++;
                               if (address == 0xE098 && (uint16_t)v == 0xC302) sim.mac_ocp[0xE00E / 2] &= (uint16_t)~(1U << 13);
                               if (address >= 0xF800 || (address >= 0xFC20 && address < 0xFC50)) sim.firmware_mac_writes++; }
        sim.mac_ocp_addr = address; return; }
    case 0xB8: assert(width == 4); {
        uint32_t address = (v >> 15) & 0xFFFE;
        if (v & 0x80000000U) { phy_write_reg(address, (uint16_t)v); put32v(sim.regs + 0xB8, v & 0x7FFFFFFFU); sim.phy_read_pending = false; return; }
        sim.phy_ocp_addr = address; sim.phy_read_pending = true; sim.phy_flag_reads = 2; return; }
    case 0x74: assert(width == 4); {   /* ERI: commands complete at once (read: flag set, data in ERIDR) */
        unsigned address = v & 0xFFF, mask = (v >> 12) & 15, type = (v >> 16) & 3;
        uint32_t* space = type == 2 ? sim.eri_oob : sim.eri; assert(!(address & 3) && address < 0x1000);
        if (v & 0x80000000U) {
            assert(mask); uint32_t data = le32v(sim.regs + 0x70), old = space[address / 4], m = 0;
            for (unsigned i = 0; i < 4; i++) if (mask & (1U << i)) m |= 0xFFU << (8 * i);
            space[address / 4] = (old & ~m) | (data & m);
            if (type == 2 && address == 0x180) space[0x124 / 4] = (data & 0xFF) == 5 ? 1 : 0;   /* DASH: driver start/stop acknowledged */
            put32v(sim.regs + 0x74, v & 0x7FFFFFFFU);
        } else { put32v(sim.regs + 0x70, space[address / 4]); put32v(sim.regs + 0x74, v | 0x80000000U); }
        return; }
    case 0x80: assert(width == 4); {   /* EPHY: commands complete at once */
        unsigned reg = (v >> 16) & (sim.is8125 ? 0x7F : 0x1F);
        if (v & 0x80000000U) { sim.ephy[reg] = (uint16_t)v; sim.ephy_writes++; put32v(sim.regs + 0x80, v & 0x7FFFFFFFU); }
        else put32v(sim.regs + 0x80, 0x80000000U | reg << 16 | sim.ephy[reg]);
        return; }
    case 0x68: assert(width == 4); {
        unsigned address = v & 0xFFC;
        if (v & 0x80000000U) { put32v(cfg + address, le32v(sim.regs + 0x64)); put32v(sim.regs + 0x68, v & 0x7FFFFFFFU); }
        else { put32v(sim.regs + 0x64, le32v(cfg + address)); put32v(sim.regs + 0x68, v | 0x80000000U); }
        return; }
    case 0xF9: assert(width == 1); sim.regs[0xF9] = (uint8_t)v; if (!(v & 1)) sim.regs[0xFB] |= 0x20; return;
    case 0xFB: assert(width == 1); sim.regs[0xFB] &= (uint8_t)~(v & 0x20); return;
    }
    for (unsigned i = 0; i < width; i++) sim.regs[r + i] = (uint8_t)(v >> (8 * i));
}
static uint8_t rd8(unsigned r) { return (uint8_t)reg_read(r, 1); }
static uint16_t rd16(unsigned r) { return (uint16_t)reg_read(r, 2); }
static uint32_t rd32(unsigned r) { return reg_read(r, 4); }
static void wr8(unsigned r, uint8_t v) { reg_write(r, v, 1); }
static void wr16(unsigned r, uint16_t v) { reg_write(r, v, 2); }
static void wr32(unsigned r, uint32_t v) { reg_write(r, v, 4); }

/* ---------------- PCI, IRQ, memory, clock, files: kernel services ---------------- */
static pci_device_t devices[2];
static int device_count = 1;
static bool is_nic(uint8_t b, uint8_t d, uint8_t f) { return b == 3 && d == 0 && f == 0; }
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    if (!is_nic(b, d, f) || sim.dead) return 0xFFFFFFFFU;
    off &= 0xFC;
    if (off >= 0x10 && off < 0x28 && bar_sizing[(off - 0x10) / 4]) return bar_mask[(off - 0x10) / 4];
    return le32v(cfg + off);
}
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { return (uint16_t)(pci_read32(b, d, f, off & 0xFC) >> ((off & 2) * 8)); }
uint8_t pci_read8(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { return (uint8_t)(pci_read32(b, d, f, off & 0xFC) >> ((off & 3) * 8)); }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) {
    assert(is_nic(b, d, f)); if (sim.dead) return; off &= 0xFC;
    if (off >= 0x10 && off < 0x28) { unsigned i = (off - 0x10) / 4; bar_sizing[i] = v == 0xFFFFFFFFU; if (!bar_sizing[i]) put32v(cfg + off, bar_mask[i] ? (v & bar_mask[i]) | (le32v(cfg + off) & 0xF & bar_mask[i]) : 0); return; }
    if (off == 0x04) { put16v(cfg + 4, (uint16_t)v); return; }
    put32v(cfg + off, v);
}
pci_device_t* pci_get_devices(void) { return devices; }
int pci_device_count(void) { return device_count; }
volatile void* pci_ecam_address(unsigned segment, unsigned bus, unsigned device, unsigned function) {
    (void)segment; assert(is_nic((uint8_t)bus, (uint8_t)device, (uint8_t)function)); return ecam_present ? cfg : NULL;
}
uint8_t pci_find_capability(const pci_device_t* d, uint8_t id) {
    if (!(pci_read16(d->bus, d->device, d->function, 6) & 0x10)) return 0;
    for (uint8_t at = cfg[0x34]; at; at = cfg[at + 1]) if (cfg[at] == id) return at;
    return 0;
}
static bool fail_msix, fail_msi, fail_vectors;
static unsigned vectors_live, msi_vector;
static irq_handler_t handlers[256]; static unsigned intx_vector;
int pci_msix_count(const pci_device_t* d) { uint8_t c = pci_find_capability(d, 0x11); return c ? (le16(cfg + c + 2) & 0x7FF) + 1 : -95; }
int pci_enable_msix(pci_device_t* d, unsigned entry, unsigned vector, unsigned cpu) {
    (void)d; (void)cpu; if (fail_msix) return -6; assert(entry == 0 && handlers[vector]);
    put16v(cfg + 0xB2, (uint16_t)(le16(cfg + 0xB2) | 0x8000)); put16v(cfg + 4, (uint16_t)(le16(cfg + 4) | 0x406)); msi_vector = vector; return 0;
}
int pci_enable_msi(pci_device_t* d, unsigned vector, unsigned cpu) {
    (void)d; (void)cpu; if (fail_msi) return -22; assert(handlers[vector]);
    put16v(cfg + 0x52, (uint16_t)(le16(cfg + 0x52) | 1)); put16v(cfg + 4, (uint16_t)(le16(cfg + 4) | 0x406)); msi_vector = vector; return 0;
}
int pci_disable_msi(pci_device_t* d) { (void)d; put16v(cfg + 0x52, (uint16_t)(le16(cfg + 0x52) & ~1)); return 0; }
int pci_disable_msix(pci_device_t* d) { (void)d; put16v(cfg + 0xB2, (uint16_t)(le16(cfg + 0xB2) & ~0x8000)); return 0; }
static bool vector_taken[256];
int irq_alloc_vector(unsigned flags) { (void)flags; if (fail_vectors) return -28; for (unsigned v = 0x40; v < 0x90; v++) if (!vector_taken[v]) { vector_taken[v] = true; vectors_live++; return (int)v; } return -28; }
int irq_register(unsigned vector, irq_handler_t handler, void* context) { (void)context; assert(!handlers[vector]); handlers[vector] = handler; return 0; }
void irq_free_vector(unsigned vector) { assert(vector_taken[vector] && vectors_live); vector_taken[vector] = false; handlers[vector] = NULL; vectors_live--; }
int irq_attach_isa(unsigned line, unsigned flags, irq_handler_t handler, void* context) { (void)context; assert(flags == IRQ_PCI_INTX && line == 11); intx_vector = 0x20 + line; handlers[intx_vector] = handler; return (int)intx_vector; }
int irq_attach_pci(const pci_device_t* device, unsigned flags, irq_handler_t handler, void* context) {   /* pcidev.c (power: _PRT) */
    (void)flags; unsigned line = device->irq_line;
    if (line < 3 || line >= 16 || line == 8 || line == 13) return 0;
    return irq_attach_isa(line, IRQ_PCI_INTX, handler, context);
}
int irq_unregister(unsigned vector, irq_handler_t handler, void* context) { (void)context; assert(handlers[vector] == handler); handlers[vector] = NULL; return 0; }
void irq_mask(unsigned line) { (void)line; }
volatile void* mmio_map(uint64_t physical, uint64_t size, unsigned cache) { (void)physical; (void)size; (void)cache; return NULL; }
uint64_t boot_pd[2048];
void usb_irq(unsigned vector) { (void)vector; }
static uint64_t clock_ns;
uint64_t native_clock_ns(void) { return clock_ns += 200; }
void irq_wait_restricted(void) { clock_ns += 1000000; }
static unsigned allocations; static uint64_t allocated_bytes;
uint64_t arena_alloc(uint64_t bytes) { uint8_t* block = calloc(1, bytes + 4096); assert(block); allocations++; allocated_bytes += bytes; return ((uint64_t)(uintptr_t)block + 4095) & ~4095ULL; }
void arena_free(uint64_t address, uint64_t bytes) { assert(address && allocations); allocations--; allocated_bytes -= bytes; }   /* blocks are not reused */
uint64_t dma32_alloc(uint64_t bytes) { (void)bytes; assert(!"64-bit DMA chips must not use the 32-bit zone"); return 0; }
bool native_random(void* output, uint64_t length) { memset(output, 0x5A, length); return true; }
static net_device_t* registered; static int register_result;
int net_register_device(net_device_t* device) { if (register_result) return register_result; registered = device; return 0; }
net_device_t* net_get_device(int index) { return index == 0 ? registered : NULL; }
bool native_network_available(void) { return registered != NULL; }
static unsigned delivered, delivered_bad; static uint8_t delivered_last[2048]; static unsigned delivered_length;
void net_deliver_packet(const void* data, uint16_t length) {
    const uint8_t* p = data; delivered++; delivered_length = length; memcpy(delivered_last, p, length);
    for (unsigned i = 14; i < length; i++) if (p[i] != (uint8_t)(i * 7 + p[13])) { delivered_bad++; break; }
}
static unsigned link_events;
void netlink_link_changed(void) { link_events++; }
static char log_text[1 << 17]; static unsigned log_used;
void vga_print(const char* text) { size_t n = strlen(text); if (log_used + n < sizeof(log_text)) { memcpy(log_text + log_used, text, n); log_used += (unsigned)n; log_text[log_used] = 0; } fputs(text, stdout); }
static bool logged(const char* text) { return strstr(log_text, text) != NULL; }
/* firmware files */
static struct { const char* path; uint8_t* data; uint32_t size; } files[4]; static unsigned file_count;
bool disk_lookup(const char* path, fat32_file_t* out) {
    for (unsigned i = 0; i < file_count; i++) if (!strcmp(files[i].path, path)) { memset(out, 0, sizeof(*out)); out->size = files[i].size; out->first = i; return true; }
    return false;
}
int32_t disk_read_file(fat32_file_t* f, uint64_t offset, uint64_t length, void* buffer) {
    assert(f->first < file_count && offset + length <= files[f->first].size); memcpy(buffer, files[f->first].data + offset, length); return (int32_t)length;
}
/* storage: vfs.h routes fat32_lookup / fat32_read_file through the mount table (mnt_*) */
int32_t mnt_read_file(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer) { return disk_read_file(file, offset, length, buffer); }
/* the driver under test */
#include "../arch/x86_64/pcidev.c"
#include "../arch/x86_64/netdrv.c"
#include "../arch/x86_64/rtl8169.c"
void native_nic_irq(unsigned vector) { rtl8169_nic_irq(vector); }
bool native_nic_link(struct nic_link* link) { return rtl8169_nic_link(link); }

/* ---------------- harness ---------------- */
static void deliver_interrupts(void) {
    for (unsigned guard = 0; guard < 16; guard++) {
        if (sim.messages && msi_vector && handlers[msi_vector]) { sim.messages--; handlers[msi_vector](msi_vector, NULL); continue; }
        bool intx = !msix_on() && !msi_on() && !(le16(cfg + 4) & 0x400) && intx_vector && handlers[intx_vector];
        if (intx && (isr() & imr())) { handlers[intx_vector](intx_vector, NULL); continue; }
        return;
    }
    assert(!"interrupt storm: the line stays asserted after the handler ran");
}
static void service(void) {   /* the network poll loop with interrupts open between calls */
    for (unsigned i = 0; i < 64; i++) {
        deliver_interrupts();
        if (!rtl8169_nic_rx_pending()) break;
        rtl8169_nic_poll();
    }
    deliver_interrupts();
}
static void advance(uint64_t ns) { clock_ns += ns; service(); }
static uint8_t* load(const char* directory, const char* name, uint32_t* size) {
    char path[512]; snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE* f = fopen(path, "rb"); if (!f) { fprintf(stderr, "missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc((size_t)n); assert(data && fread(data, 1, (size_t)n, f) == (size_t)n); fclose(f);
    *size = (uint32_t)n; return data;
}
static const char* firmware_dir;
static void add_firmware(const char* name) {
    static char paths[4][96]; assert(file_count < 4);
    snprintf(paths[file_count], sizeof(paths[0]), "/lib/firmware/%s", name);
    files[file_count].path = paths[file_count]; files[file_count].data = load(firmware_dir, name + 8 /* "rtl_nic/" */, &files[file_count].size); file_count++;
}
static void frame(uint8_t* f, unsigned length, uint8_t seed) {
    memset(f, 0xFF, 6); f[6] = 0x52; f[7] = 0x54; f[8] = 0; f[9] = 0x12; f[10] = 0x34; f[11] = 0x56; f[12] = 0x08; f[13] = seed;
    for (unsigned i = 14; i < length; i++) f[i] = (uint8_t)(i * 7 + seed);
}
static void scenario(unsigned version, uint16_t xid) {
    memset(&nic, 0, sizeof(nic)); power_on(version, xid);
    devices[0] = (pci_device_t){.bus = 3, .device = 0, .function = 0, .vendor_id = 0x10EC, .device_id = le16(cfg + 2),
                                .class_code = PCI_CLASS_NETWORK, .irq_line = 11};
    device_count = 1; registered = NULL; register_result = 0; fail_msix = fail_msi = fail_vectors = false;
    memset(handlers, 0, sizeof(handlers)); memset(vector_taken, 0, sizeof(vector_taken)); vectors_live = 0; msi_vector = 0; intx_vector = 0;
    delivered = delivered_bad = 0; link_events = 0; log_used = 0; log_text[0] = 0; file_count = 0; allocations = 0; ecam_present = true;
}
static int transmit_frame(unsigned length, uint8_t seed) { uint8_t f[2048]; frame(f, length, seed); return nic.device.send(&nic.device, f, (uint16_t)length); }
static void traffic(unsigned frames) {
    uint8_t f[1600]; unsigned before = delivered;
    for (unsigned i = 0; i < frames; i++) { frame(f, 60 + (i * 37) % 1400, (uint8_t)i); assert(rx_frame(f, 60 + (i * 37) % 1400, 0)); if (i % 32 == 31) service(); }
    service();
    assert(delivered == before + frames && !delivered_bad);
    unsigned sent = sim.tx_frames;
    for (unsigned i = 0; i < 20; i++) { assert(transmit_frame(100 + i * 50, (uint8_t)(0x40 + i)) == 0); service(); }
    assert(sim.tx_frames == sent + 20 && sim.tx_last_length == 1050);
    uint8_t expect[2048]; frame(expect, 1050, 0x53); assert(!memcmp(sim.tx_last, expect, 1050));
    /* sustained transmit: the 64-entry ring wraps (EOR) several times, slots are reclaimed from OWN */
    for (unsigned i = 0; i < 150; i++) { assert(transmit_frame(64 + i, (uint8_t)i) == 0); if (i % 40 == 39) service(); }
    service(); assert(sim.tx_frames == sent + 170 && sim.tx_last_length == 64 + 149);
}

/* hostile firmware programs: the validator and the step budget */
static void firmware_programs(void) {
    struct rtl_firmware fw; uint8_t buf[256];
    #define ACT(i, op, reg, val) put32v(buf + 4 * (i), (uint32_t)(op) << 28 | (uint32_t)(reg) << 16 | (val))
    memset(buf, 0, sizeof(buf));
    ACT(0, FW_WRITE, 0x1F, 0x0A43); ACT(1, 15, 0, 0); assert(!firmware_parse(buf, 8, "x", &fw));            /* unknown opcode */
    ACT(1, FW_BJMPN, 2, 0); assert(!firmware_parse(buf, 8, "x", &fw));                                         /* jump before the start */
    ACT(1, FW_BJMPN, 0, 0); assert(!firmware_parse(buf, 8, "x", &fw));                                         /* zero-distance loop */
    ACT(1, FW_SKIPN, 1, 0); assert(!firmware_parse(buf, 8, "x", &fw));                                         /* skip past the end */
    ACT(1, FW_MDIO_CHG, 0, 2); assert(!firmware_parse(buf, 8, "x", &fw));
    ACT(1, FW_COUNT_EQ_SKIP, 0, 1); assert(!firmware_parse(buf, 8, "x", &fw));
    assert(!firmware_parse(buf, 6, "x", &fw) && !firmware_parse(buf, 2, "x", &fw));                            /* not whole actions */
    ACT(1, FW_WRITE, 0x10, 0x1234); assert(firmware_parse(buf, 8, "bare.fw", &fw) && fw.count == 2 && !strcmp(fw.version, "bare.fw"));
    /* header format: byte sum must be zero, start/length inside the file */
    uint8_t h[128]; memset(h, 0, sizeof(h)); memcpy(h + 4, "test_1.0", 8); put32v(h + 36, 48); put32v(h + 40, 2);
    put32v(h + 48, (uint32_t)FW_WRITE << 28 | 0x1F << 16 | 0x0A43); put32v(h + 52, (uint32_t)FW_WRITE << 28 | 0x10 << 16 | 0x0001);
    uint8_t sum = 0; for (unsigned i = 0; i < 56; i++) sum = (uint8_t)(sum + h[i]); h[44] = (uint8_t)(0x100 - sum);
    assert(firmware_parse(h, 56, "y", &fw) && fw.count == 2 && !strcmp(fw.version, "test_1.0"));
    h[50] ^= 1; assert(!firmware_parse(h, 56, "y", &fw)); h[50] ^= 1;                                          /* checksum */
    put32v(h + 40, 3); h[44] = (uint8_t)(h[44] - 1); assert(!firmware_parse(h, 56, "y", &fw));                 /* length past the end */
    put32v(h + 40, 2); h[44] = (uint8_t)(h[44] + 1); put32v(h + 36, 60); h[44] = (uint8_t)(h[44] - 12); assert(!firmware_parse(h, 56, "y", &fw)); /* start past the end */
    /* a valid program that loops forever (the condition never holds): stopped by the step budget */
    memset(buf, 0, sizeof(buf));
    ACT(0, FW_WRITE, 0x1F, 0x0B80); ACT(1, FW_READ, 0x10, 0); ACT(2, FW_AND, 0, 0x8000); ACT(3, FW_EQ_SKIPN, 1, 0x8000); ACT(4, FW_BJMPN, 3, 0); ACT(5, FW_WRITE, 0x1F, 0);
    assert(firmware_parse(buf, 24, "loop", &fw));
    uint64_t t = clock_ns; assert(!firmware_run(&fw) && nic.ocp_base == OCP_STD_PHY_BASE); assert(clock_ns - t < 60ULL * 1000000000ULL);
    #undef ACT
}

int main(int argc, char** argv) {
    assert(argc == 2); firmware_dir = argv[1];
    clock_ns = 1000000000ULL;

    /* ===== RTL8125B (the user's PC: 10EC:8125 rev 05, XID 0x641) ===== */
    scenario(63, 0x641); add_firmware("rtl_nic/rtl8125b-2.fw");
    assert(rtl8169_nic_init());
    assert(logged("Realtek RTL8125B (XID 641, MAC version 63) native DMA ready, IRQ MSI-X"));
    assert(msix_on() && !msi_on() && (le16(cfg + 4) & 0x406) == 0x406 && vectors_live == 1);
    assert(!(le16(cfg + 0x80) & 3));                                   /* ASPM off on the link */
    assert(!memcmp(nic.device.mac.addr, "\x00\xE0\x4C\x68\x00\x25", 6) && !memcmp(sim.regs, nic.device.mac.addr, 6) && !sim.locked_mac_writes);
    assert(sim.regs[0x50] == 0x00);                                    /* config registers locked again */
    assert(!(sim.regs[0xD3] & 0x80) && !(le32v(sim.regs + 0xF0) & (1U << 19)));   /* left OOB mode, RXDV gate open */
    assert(sim.mac_ocp[0xC0AA / 2] == 0x07D0 && sim.mac_ocp[0xC01E / 2] == 0x5555);
    assert((sim.mac_ocp[0xE614 / 2] & 0x0700) == 0x0200 && !(sim.mac_ocp[0xEB58 / 2] & 1) && (sim.mac_ocp[0xD3E2 / 2] & 0x0FFF) == 0x03A9);
    assert((sim.mac_ocp[0xE040 / 2] & 3) == 3 && !(sim.mac_ocp[0xE00E / 2] & (1U << 13)) && sim.mac_ocp[0xE098 / 2] == 0xC302);
    assert(sim.ephy[0x0B] == 0xA908 && sim.ephy[0x4B] == 0xA908 && sim.ephy[0x5E] == 0x20EB && (sim.ephy[0x62] & 0x30) == 0x20);   /* 7-bit EPHY addresses */
    for (unsigned r = 0xA00; r < 0xA80; r += 4) assert(!le32v(sim.regs + r));
    assert(cfg[0x70F] == 0x27);                                        /* ASPM entry latency through ECAM */
    assert(!(sim.regs[0x56] & 1) && !(sim.regs[0x53] & 0x80) && !(sim.mac_ocp[0xE092 / 2] & 0xFF));   /* chip ASPM/CLKREQ off */
    assert(!(le16(sim.regs + 0xE0) & 0x60));                           /* no VLAN stripping, no checksum offload */
    uint32_t rxcfg = le32v(sim.regs + 0x44);
    assert((rxcfg & 0x3F) == 0x0E && !(rxcfg & (3U << 22)) && (rxcfg & (8U << 27)) && (rxcfg & (1U << 11)));
    assert(le32v(sim.regs + 0x08) == 0xFFFFFFFFU && le32v(sim.regs + 0x0C) == 0xFFFFFFFFU && le16(sim.regs + 0xDA) == 2049);
    assert((sim.regs[0x37] & 0x0C) == 0x0C && imr() == 0x002B);
    uint64_t rx_ring = le32v(sim.regs + 0xE4) | (uint64_t)le32v(sim.regs + 0xE8) << 32, tx_ring = le32v(sim.regs + 0x20) | (uint64_t)le32v(sim.regs + 0x24) << 32;
    assert(rx_ring == nic.dma_physical && tx_ring == nic.dma_physical + 4096 && allocations == 1);
    for (unsigned i = 0; i < 256; i++) assert(nic.rx[i].opts1 == (0x80000000U | (i == 255 ? 0x40000000U : 0) | 2048));
    /* PHY: powered, configured, every speed advertised, autonegotiation running */
    assert(phy_powered() && (sim.phy_ocp[0xA400 / 2] & 0x1000) && sim.phy_ocp[0xA408 / 2] == 0x0DE1 && (sim.phy_ocp[0xA412 / 2] & 0x0300) == 0x0200);
    assert((sim.phy_ocp[0xA5D4 / 2] & 0x0180) == 0x0080);
    assert(sim.sram_8125[0x80F5] == 0x760E && sim.sram_8125[0x8107] == 0x360E && sim.sram_g[0x8044] == 0x2417 && sim.sram_g[0x807A] == 0x2417);
    assert(!(sim.phy_ocp[0xA5B4 / 2] & 0x8000) && !(sim.phy_ocp[0xA430 / 2] & 4));   /* legacy force mode off, ALDPS off */
    assert(!sim.patch_requests);                                       /* no firmware before the root volume is mounted */
    /* link up at 2.5 Gb/s */
    assert(!nic.device.link_up && transmit_frame(100, 1) == -100);
    service(); assert(link_events == 0);
    sim_link(true, 2500, true); service();
    assert(nic.device.link_up && nic.link.speed == 2500 && nic.link.duplex == NIC_DUPLEX_FULL && link_events == 1);
    assert(logged("eth0: link up 2500 Mb/s full duplex"));
    char text[128]; assert(netdrv_sysfs_text(10, 2, text) && !strcmp(text, "2500\n"));   /* /sys/class/net/eth0/speed */
    assert(netdrv_sysfs_text(3, 2, text) && !strcmp(text, "1\n") && netdrv_sysfs_text(4, 2, text) && !strcmp(text, "full\n"));
    assert(netdrv_sysfs_text(9, 2, text) && !strcmp(text, "up\n") && netdrv_sysfs_text(0, 2, text) && !strcmp(text, "00:e0:4c:68:00:25\n"));
    assert(netdrv_sysfs_text(9, 1, text) && !strcmp(text, "unknown\n") && netdrv_sysfs_text(12, 1, text) && !strcmp(text, "772\n"));
    /* data path: receive through MSI-X with interrupt masking, transmit through the 8125 doorbell */
    traffic(300);
    assert(imr() == 0x002B && sim.doorbells >= 20);
    /* a frame arriving right after the unmask is not lost (edge-triggered MSI-X) */
    uint8_t f[1600]; frame(f, 200, 9); unsigned before = delivered; assert(rx_frame(f, 200, 0)); service(); assert(delivered == before + 1);
    /* receive errors: CRC, split frame, runt, then good frames again */
    unsigned errors = nic.device.stats.rx_errors, dropped = nic.device.stats.rx_dropped; before = delivered;
    assert(rx_frame(f, 200, 1)); assert(rx_frame(f, 1500, 2)); assert(rx_frame(f, 12, 0)); assert(rx_frame(f, 200, 0)); service();
    assert(delivered == before + 1 && nic.device.stats.rx_errors == errors + 3 && nic.device.stats.rx_dropped == dropped + 1);
    /* ring overflow while nobody polls: 256 accepted, the rest missed (RDU), then the ring recovers */
    before = delivered;
    for (unsigned i = 0; i < 300; i++) { frame(f, 100, (uint8_t)i); rx_frame(f, 100, 0); }
    assert(sim.rx_missed == 44); service(); assert(delivered == before + 256 && !delivered_bad);
    frame(f, 64, 3); assert(rx_frame(f, 64, 0)); service(); assert(delivered == before + 257);
    /* transmit argument checks, padding */
    assert(nic.device.send(&nic.device, NULL, 10) == -22 && nic.device.send(&nic.device, f, 0) == -22 && nic.device.send(&nic.device, f, 1515) == -90);
    assert(transmit_frame(20, 7) == 0 && sim.tx_last_length == 60); for (unsigned i = 20; i < 60; i++) assert(sim.tx_last[i] == 0);
    /* RTL8125B PTP erratum: a UDP frame to port 319 with a short payload is padded to 47 transport bytes */
    uint8_t ptp[64]; memset(ptp, 0, sizeof(ptp)); memset(ptp, 0xFF, 6); ptp[12] = 0x08; ptp[14] = 0x45; ptp[23] = 17; ptp[36] = 319 >> 8; ptp[37] = 319 & 0xFF;
    assert(nic.device.send(&nic.device, ptp, 14 + 20 + 10) == 0 && sim.tx_last_length == 14 + 20 + 47);
    /* full ring with a hung transmitter: 63 queued, then EAGAIN, then the 5 s watchdog resets the chip */
    sim.tx_hang = true; unsigned queued = 0;
    while (transmit_frame(80, 1) == 0) queued++;
    assert(queued == 63 && transmit_frame(80, 1) == -11);
    unsigned resets = sim.resets; advance(2000000000ULL); assert(sim.resets == resets);
    advance(4000000000ULL); assert(sim.resets == resets + 1 && logged("transmit timeout, resetting the chip"));
    sim.tx_hang = false; service();
    assert(transmit_frame(90, 2) == 0 && sim.tx_last_length == 90); traffic(10);
    /* link down and back at 1 Gb/s */
    sim_link(false, 0, false); service(); assert(!nic.device.link_up && transmit_frame(100, 1) == -100 && logged("eth0: link down"));
    sim_link(true, 1000, true); service(); assert(nic.link.speed == 1000 && nic.device.link_up && link_events == 3);
    /* firmware after the root mount: missing first (clean fallback), then a corrupted copy, then the real file */
    char* saved_path = (char*)files[0].path; files[0].path = "/nonexistent";
    rtl8169_nic_late_init(); assert(logged("rtl8125b-2.fw not found; running without it") && !nic.firmware && !sim.patch_requests);
    files[0].path = saved_path; files[0].data[100] ^= 0x40;
    rtl8169_nic_late_init(); assert(logged("SHA-256 or size mismatch; running without it") && !nic.firmware && allocations == 1);
    files[0].data[100] ^= 0x40;
    unsigned mac_writes = sim.firmware_mac_writes; resets = sim.resets;
    rtl8169_nic_late_init(); assert(logged("applying firmware rtl_nic/rtl8125b-2.fw") && nic.firmware && allocations == 2);
    assert(sim.patch_requests == 1 && !(sim.phy_ocp[0xB820 / 2] & 0x10) && sim.firmware_mac_writes > mac_writes + 90);
    assert(sim.mac_ocp[0xF800 / 2] == 0xE008 && sim.mac_ocp[0xF802 / 2] == 0xE013 && sim.mac_ocp[0xF810 / 2] == 0x740A);   /* MAC MCU patch */
    assert(sim.sram_g[0x801E] == 0x0016 && sim.resets == resets + 1 && !nic.firmware_errors && nic.ocp_base == OCP_STD_PHY_BASE);
    assert(sim.sram_8125[0x80F5] == 0x760E);                          /* PHY parameters applied after the firmware */
    service(); sim_link(true, 2500, true); service(); assert(nic.link.speed == 2500); traffic(20);
    /* the device stops answering (all-ones), then comes back */
    sim.dead = true; deliver_interrupts(); advance(1100000000ULL);
    assert(nic.dead && !nic.device.link_up && logged("device not responding") && transmit_frame(100, 1) == -100);
    advance(1100000000ULL); assert(nic.dead);
    sim.dead = false; advance(1100000000ULL); advance(1100000000ULL);
    assert(!nic.dead && logged("device answers again, reinitialising"));
    sim_link(true, 2500, true); service(); assert(nic.device.link_up); traffic(20);
    /* suspend, lose power (registers and configuration space), resume */
    unsigned requests = sim.patch_requests;
    rtl8169_nic_suspend();
    assert(imr() == 0 && !(sim.regs[0x37] & 0x0C) && !phy_powered() && !(le16(cfg + 4) & 4) && !nic.device.link_up);
    assert(!rtl8169_nic_rx_pending()); deliver_interrupts();
    power_on(63, 0x641);                                               /* power lost: BARs, command, MSI-X gone */
    put32v(cfg + 0x18, 0); put32v(cfg + 0x20, 0); put16v(cfg + 4, 0);
    assert(rtl8169_nic_resume());
    assert(le32v(cfg + 0x18) == 0xFC600004 && le32v(cfg + 0x20) == 0xFC610004 && msix_on() && (le16(cfg + 4) & 6) == 6);
    assert(requests >= 2 && sim.patch_requests == 1);                   /* firmware re-applied on the fresh chip */
    assert(sim.mac_ocp[0xF800 / 2] == 0xE008 && !memcmp(sim.regs, "\x00\xE0\x4C\x68\x00\x25", 6) && (sim.regs[0x37] & 0x0C) == 0x0C);
    sim_link(true, 2500, true); service(); assert(nic.device.link_up && nic.link.speed == 2500); traffic(40);
    rtl8169_nic_suspend(); assert(rtl8169_nic_resume()); sim_link(true, 2500, true); service(); traffic(5);   /* without power loss */
    firmware_programs();
    puts("RTL8125B scenario passed");

    /* ===== RTL8168h (VER 46): MSI-X and MSI fail -> INTx; 16-bit status, TxPoll doorbell, MAC from ERI, 5-bit EPHY ===== */
    scenario(46, 0x541); add_firmware("rtl_nic/rtl8168h-2.fw"); fail_msix = true; fail_msi = true;
    assert(rtl8169_nic_init() && logged("RTL8168h/8111h (XID 541, MAC version 46) native DMA ready, IRQ INTx") && vectors_live == 0 && intx_vector == 0x2B);
    assert(!msix_on() && !msi_on() && !(le16(cfg + 4) & 0x400));
    assert(!memcmp(nic.device.mac.addr, "\x00\xE0\x4C\x68\x00\x25", 6) && sim.ephy[0x05] == 0x2089 && sim.ephy[0x01] == 0x068B);
    assert(sim.regs[0xEC] == 0x27 && !le16(sim.regs + 0xE2) && sim.mac_ocp[0xE048 / 2] == 1546 && (le32v(sim.regs + 0x40) & (1U << 7)));
    assert((sim.eri[0xDC / 4] & 0x1D) == 0x1D && (sim.eri[0x5F0 / 4] & 0xFFFF) == 0x4F87 && imr() == 0x002B);
    assert(sim.sram_g[0x808A] == 0x000A);
    sim_link(true, 1000, true); service(); assert(nic.link.speed == 1000 && link_events == 1);
    traffic(100);
    rtl8169_nic_late_init(); assert(nic.firmware && sim.patch_requests == 1 && !(sim.phy_ocp[0xB820 / 2] & 0x10));
    sim_link(true, 100, false); service(); assert(nic.link.speed == 100 && nic.link.duplex == NIC_DUPLEX_HALF); traffic(30);
    puts("RTL8168h INTx scenario passed");

    /* ===== polled: no MSI, no usable line; link and receive work from the poll alone ===== */
    scenario(63, 0x641); fail_msix = true; fail_msi = true; devices[0].irq_line = 0;
    assert(rtl8169_nic_init() && logged("IRQ polled") && imr() == 0);
    sim_link(true, 2500, true); advance(1100000000ULL); assert(nic.device.link_up);
    traffic(50); assert(imr() == 0);
    puts("polled scenario passed");

    /* ===== MSI when MSI-X fails; RTL8125A (VER 61) ===== */
    scenario(61, 0x609); fail_msix = true;
    assert(rtl8169_nic_init() && logged("RTL8125A (XID 609, MAC version 61) native DMA ready, IRQ MSI") && msi_on() && vectors_live == 1);
    assert(sim.ephy[0x44] == 0xD000 && sim.ephy[0x69] == 0xFE04 && (sim.mac_ocp[0xE614 / 2] & 0x700) == 0x300 && (sim.mac_ocp[0xEB62 / 2] & 6) == 6);
    for (unsigned r = 0xA00; r < 0xB00; r += 4) assert(!le32v(sim.regs + r));
    assert(sim.sram_g[0x81B3] == 0x0043 && sim.sram_g[0x81B3 + 14] == 0x0002 && sim.sram_8125[0x80A2] == 0x0153);
    sim_link(true, 2500, true); service(); traffic(40);
    puts("RTL8125A MSI scenario passed");

    /* ===== the other supported MAC versions: probe, link, traffic ===== */
    static const struct { unsigned version; uint16_t xid; const char* name; } others[] = {
        {40, 0x4C0, "RTL8168g/8111g"}, {42, 0x509, "RTL8168gu/8111gu"}, {51, 0x502, "RTL8168ep/8111ep"}, {52, 0x54A, "RTL8168fp/RTL8117"},
        {64, 0x688, "RTL8125D"}, {64, 0x689, "RTL8125D"}, {70, 0x649, "RTL8126A"}, {70, 0x64A, "RTL8126A"}, {46, 0x6C0, "RTL8168M"}};
    for (unsigned i = 0; i < sizeof(others) / sizeof(*others); i++) {
        scenario(others[i].version, others[i].xid);
        if (others[i].version == 70) { ecam_present = false; cfg[0x890] = 0x03; }   /* extended config through the CSI window */
        if (others[i].version == 51) sim.eri_oob[0x128 / 4] = 1;   /* DASH firmware active */
        assert(rtl8169_nic_init()); assert(logged(others[i].name));
        if (others[i].version == 70) assert(cfg[0x70F] == 0x27 && cfg[0x890] == 0x02 && (sim.phy_ocp[0xA5D4 / 2] & 0x0180) == 0x0180);
        if (others[i].version == 51) assert(logged("DASH enabled") && sim.eri_oob[0x180 / 4] == 5 && (sim.regs[0xFB] & 0x20) == 0);
        if (others[i].version == 40) assert((sim.eri[0x1A8 / 4] & 0xFC000000U) == 0xFC000000U);   /* PHY MCU interrupts on (powered) */
        sim_link(true, others[i].version >= 70 ? 5000 : others[i].version >= 61 ? 2500 : 1000, true); service();
        assert(nic.device.link_up && nic.link.speed == (others[i].version >= 70 ? 5000U : others[i].version >= 61 ? 2500U : 1000U));
        traffic(40);
        rtl8169_nic_suspend(); assert(rtl8169_nic_resume()); sim_link(true, 1000, true); service(); traffic(5);
        if (others[i].version == 51) { rtl8169_nic_suspend(); assert(sim.eri_oob[0x180 / 4] == 6); assert(rtl8169_nic_resume()); }
    }
    puts("other MAC versions passed");

    /* ===== refusals: unsupported chips, no BAR, stuck reset, dead device, registration failure ===== */
    static const uint16_t unsupported[] = {0x2C8 /* 8168evl */, 0x481 /* 8168f */, 0x380 /* 8168b */, 0x008 /* 8169s */, 0x448 /* 8106e */, 0x7CF};
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(*unsupported); i++) {
        scenario(34, unsupported[i]);
        assert(!rtl8169_nic_init() && logged("is not supported by this driver") && !(le16(cfg + 4) & 4) && !allocations && !vectors_live && !registered);
    }
    scenario(63, 0x641); put32v(cfg + 0x18, 0); put32v(cfg + 0x20, 0);
    assert(!rtl8169_nic_init() && logged("no memory BAR assigned by the firmware") && !allocations);
    scenario(63, 0x641); sim.reset_stuck = true; uint64_t t0 = clock_ns;
    assert(!rtl8169_nic_init() && logged("chip reset timed out") && !allocations && !(le16(cfg + 4) & 4) && clock_ns - t0 < 2000000000ULL);
    scenario(63, 0x641); sim.dead = true;
    assert(!rtl8169_nic_init() && !allocations);
    scenario(63, 0x641); register_result = -1;
    assert(!rtl8169_nic_init() && !allocations && !vectors_live && !(le16(cfg + 4) & 4));
    scenario(63, 0x641); devices[0].device_id = 0x8136;                 /* RTL810x fast Ethernet: not ours */
    assert(!rtl8169_nic_init() && !log_used);
    puts("refusal scenarios passed");

    /* ===== SHA-256 known answers ===== */
    uint8_t digest[32];
    netdrv_sha256("abc", 3, digest); assert(digest[0] == 0xBA && digest[1] == 0x78 && digest[31] == 0xAD);
    netdrv_sha256("", 0, digest); assert(digest[0] == 0xE3 && digest[31] == 0x55);
    static const char million[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    netdrv_sha256(million, 56, digest); assert(digest[0] == 0x24 && digest[1] == 0x8D && digest[31] == 0xC1);
    puts("RTL8169 SIMULATOR TESTS PASSED");
    return 0;
}
