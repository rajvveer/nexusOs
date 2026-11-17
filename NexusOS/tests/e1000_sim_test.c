/* Register-level simulator for the Intel Ethernet driver (arch/x86_64/e1000.c with netdrv.c and the real PCI code of
 * pcidev.c): the PCH-integrated I219 (Cannon Lake, Tiger Lake, Sunrise Point variants), the I225/I226 (igc) and the
 * 82574L whose QEMU model the boot tests use. The simulated controller models:
 *  - MAC registers: global reset (self-clearing, reloads RAR0 from the "NVM", releases the software flag), PHY
 *    reset (PCH: LAN_INIT_DONE/PHYRA; I225: PHPM reset-complete), PCIe master disable, STATUS speed/duplex/link incl.
 *    the I225 2.5G bit, ICR read-to-clear at 0x00C0 (8254x/PCH) or 0x1500 (I225), IMS/IMC, EECD auto-read;
 *  - the PCH PHY interface: EXTCNF_CTRL software flag (asserted held for every MDIC access; a firmware owner can keep
 *    it), ME present or not (FWSM.FW_VALID), ultra-low-power mode left by the firmware (MDIC errors until the ME
 *    clears it through H2ME/ULP_CFG_DONE or a LANPHYPC power cycle), SMBus vs PCIe interconnect (MDIC only works
 *    when CTRL_EXT.FORCE_SMBUS matches the PHY's mode), reset blocking (FWSM.RSPCIPHY), PHY page select (register
 *    31 = page << 5, address 2 for page 0, 1 for pages >= 768), the EMI window, I219 descriptor-ring hang flag
 *    (PCI config 0xE4 bit 8) cleared by the flush sequence;
 *  - the I225 SWSM (SMBI set by a read that finds it clear) + SW_FW_SYNC semaphores (asserted held for every MDIC
 *    access; firmware may own the PHY for a while), MMD access through registers 13/14;
 *  - DMA: legacy and advanced descriptors, TDH/TDT and RDH/RDT ownership windows, DD/EOP write-back;
 *  - MSI edge / INTx level delivery, all-ones reads (device gone), lost power (registers and config space cleared). */
#define E1000_TEST_MMIO 1
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

enum { SIM_82574, SIM_PCH, SIM_I225 };
static struct {
    int kind; unsigned pch; uint16_t device;
    uint32_t regs[0x20000 / 4];
    uint16_t phy[2][1024][32];      /* [address 1/2 -> 0/1][page][register] */
    unsigned page[2];
    uint16_t emi[0x10000];
    bool me, ulp, phy_smbus, reset_blocked, phy_dead, flag_owned_by_fw, dead, tx_hang, gate_seen, ulp_commit_seen, k1_on_seen;
    unsigned fw_phy_busy;           /* I225: SW_FW_SYNC firmware PHY ownership for this many polls */
    unsigned reset_reads, lpcd_reads, ulp_reads, phy_reset_reads;
    bool link, full; unsigned speed;
    unsigned rx_head, tx_head;
    /* observations */
    unsigned mac_resets, phy_resets, lanphypc_toggles, mdic_errors, tx_frames, rx_missed, flush_tx_seen;
    uint8_t tx_last[2048]; unsigned tx_last_length;
    bool prev_level; unsigned messages;
} sim;
static uint8_t cfg[256];
static bool bar_sizing[6];
static const uint32_t bar_mask[6] = {0xFFFE0000, 0, 0xFFFFFFE1, 0xFFFFC000, 0, 0};
static const uint8_t nvm_mac[6] = {0x3C, 0x7C, 0x3F, 0x11, 0x22, 0x33};
static uint32_t R(unsigned r) { return sim.regs[r / 4]; }
static void S(unsigned r, uint32_t v) { sim.regs[r / 4] = v; }
static unsigned ICR_REG(void) { return sim.kind == SIM_I225 ? 0x1500 : 0x00C0; }
static unsigned IMS_REG(void) { return sim.kind == SIM_I225 ? 0x1508 : 0x00D0; }
static unsigned IMC_REG(void) { return sim.kind == SIM_I225 ? 0x150C : 0x00D8; }
static bool advanced_sim(void) { return sim.kind == SIM_I225; }
static unsigned RXB(void) { return advanced_sim() ? 0xC000 : 0x2800; }
static unsigned TXB(void) { return advanced_sim() ? 0xE000 : 0x3800; }
static bool msi_on(void) { return (cfg[0x52] & 1) != 0; }
static void irq_check(void) {
    bool level = (R(ICR_REG()) & R(IMS_REG())) != 0;
    if (msi_on() && level && !sim.prev_level) sim.messages++;
    sim.prev_level = level;
}
static void raise_cause(uint32_t bits) { S(ICR_REG(), R(ICR_REG()) | bits); irq_check(); }
static void status_update(void) {
    uint32_t s = R(0x08) & ~(0x1U | 0x2U | 0xC0U | (1U << 22));
    if (sim.link) {
        s |= 0x2 | (sim.full ? 1 : 0);
        s |= sim.speed >= 1000 ? 0x80 : sim.speed == 100 ? 0x40 : 0;
        if (sim.speed == 2500) s |= 1U << 22;
    }
    S(0x08, s);
}
static void load_rar0(void) {
    S(0x5400, nvm_mac[0] | (uint32_t)nvm_mac[1] << 8 | (uint32_t)nvm_mac[2] << 16 | (uint32_t)nvm_mac[3] << 24);
    S(0x5404, nvm_mac[4] | (uint32_t)nvm_mac[5] << 8 | 1U << 31);
}
static void phy_defaults(void) {
    memset(sim.phy, 0, sizeof(sim.phy)); sim.page[0] = sim.page[1] = 0;
    for (unsigned a = 0; a < 2; a++) { sim.phy[a][0][0] = 0x1140; sim.phy[a][0][1] = 0x7949; sim.phy[a][0][2] = 0x0141; sim.phy[a][0][3] = 0x0CC0;
                                       sim.phy[a][0][4] = 0x01E1; sim.phy[a][0][9] = 0x0200; }
    sim.phy[0][776][20] = 0x10 << 2;                         /* pointer gap below 0x18 */
    sim.phy[0][770][17] = 0x0000;
}
static void power_on(int kind, unsigned pch_generation, uint16_t device) {
    memset(&sim, 0, sizeof(sim));
    sim.kind = kind; sim.pch = pch_generation; sim.device = device;
    load_rar0(); phy_defaults();
    S(0x08, 1U << 19);                                       /* GIO master enabled */
    S(0x5B54, 1U << 6);                                      /* FWSM: PHY reset allowed */
    S(0x0010, 1U << 9);                                      /* EECD: auto-read done */
    S(0x0E14, 1U << 8);
    if (kind == SIM_PCH) S(0x14, 0x12340002);                /* FEXTNVM5 (no EERD on PCH): must not be read as NVM words */
    memset(cfg, 0, sizeof(cfg));
    cfg[0] = 0x86; cfg[1] = 0x80; cfg[2] = (uint8_t)device; cfg[3] = (uint8_t)(device >> 8); cfg[6] = 0x10; cfg[11] = 2; cfg[0x3C] = 11;
    cfg[0x10] = 0x00; cfg[0x11] = 0x00; cfg[0x12] = 0x40; cfg[0x13] = 0xFE;   /* BAR0 0xFE400000, 128 KiB, 32-bit */
    cfg[0x34] = 0x50; cfg[0x50] = 0x05; cfg[0x51] = 0x00; cfg[0x52] = 0x80;   /* MSI, 64-bit */
    memset(bar_sizing, 0, sizeof(bar_sizing));
}
/* PHY access through MDIC: who may talk to the PHY, and the PHY's registers */
static bool phy_reachable(unsigned address) {
    if (sim.kind == SIM_PCH) {
        assert(R(0x0F00) & (1U << 5));                       /* software flag held for every PHY access */
        if (sim.phy_dead || sim.ulp) return false;
        bool mac_smbus = (R(0x18) & (1U << 11)) != 0;
        if (mac_smbus != sim.phy_smbus) return false;
        return address == 1 || address == 2;
    }
    if (sim.kind == SIM_I225) { assert(R(0x5B5C) & 2); return address == 0; }   /* SW_FW_SYNC PHY0 held */
    return address == 1;
}
static uint16_t* phy_cell(unsigned address, unsigned reg) {
    unsigned a = sim.kind == SIM_PCH ? (address == 1 ? 0 : 1) : 1;
    unsigned page = reg >= 16 ? sim.page[a] : 0;
    return &sim.phy[a][page & 1023][reg];
}
static uint32_t mmd_device, mmd_address; static uint16_t mmd[8][256];
static void mdic_command(uint32_t v) {
    unsigned reg = (v >> 16) & 31, address = (v >> 21) & 31, op = (v >> 26) & 3; uint16_t data = (uint16_t)v;
    uint32_t result = (v & 0x0FFF0000U) | 1U << 28;
    if (!phy_reachable(address)) { sim.mdic_errors++; S(0x20, result | 1U << 30); return; }
    if (op == 1) {
        if (reg == 31) { sim.page[sim.kind == SIM_PCH ? (address == 1 ? 0 : 1) : 1] = data >> 5; }
        else if (sim.kind == SIM_PCH && address == 2 && reg == 0x11 && *phy_cell(address, 0x10) == 0xB20C) sim.emi[0xB20C] = data;
        else if (sim.kind == SIM_I225 && reg == 13) { if (data & 0x4000) mmd_device = data & 7; else { mmd_device = data & 7; } if (!data) mmd_device = 0; }
        else if (sim.kind == SIM_I225 && reg == 14 && !(*phy_cell(address, 13) & 0x4000)) mmd_address = data;
        else if (sim.kind == SIM_I225 && reg == 14 && (*phy_cell(address, 13) & 0x4000)) mmd[mmd_device][mmd_address & 255] = data;
        if (sim.kind == SIM_PCH && address == 1 && reg == 16 && sim.page[0] == 779 && (data & 1) && !(data & 0x1D74)) sim.ulp_commit_seen = true;
        if (sim.kind == SIM_PCH && address == 1 && reg == 17 && sim.page[0] == 770 && (data & 0x4000)) sim.k1_on_seen = true;
        if (sim.kind == SIM_I225 && reg == 13) *phy_cell(address, 13) = data;
        else if (!(reg == 14 && sim.kind == SIM_I225)) *phy_cell(address, reg) = data;
        if (reg == 0 && (data & 0x8000)) *phy_cell(address, 0) &= (uint16_t)~0x8000;
        S(0x20, result | data);
    } else {
        uint16_t value = *phy_cell(address, reg);
        if (sim.kind == SIM_I225 && reg == 14 && (*phy_cell(address, 13) & 0x4000)) value = mmd[mmd_device][mmd_address & 255];
        if (reg == 1) value = (uint16_t)((value & ~0x0024) | (sim.link ? 0x0024 : 0));
        S(0x20, result | value);
    }
}
static void phy_power_cycle(void) {   /* LANPHYPC: the PHY restarts in PCIe mode, ULP ends */
    sim.lanphypc_toggles++; sim.ulp = false; sim.phy_smbus = false; sim.lpcd_reads = 3;
}
static void mac_reset(bool with_phy) {
    sim.mac_resets++;
    uint32_t keep_fwsm = R(0x5B54), keep_status = R(0x08) & (1U << 19);
    uint32_t smbus = R(0x18) & (1U << 11);
    memset(sim.regs, 0, sizeof(sim.regs));
    S(0x5B54, keep_fwsm); S(0x08, keep_status | (1U << 19)); S(0x0010, 1U << 9); S(0x0E14, 1U << 8); S(0x18, smbus);
    if (sim.flag_owned_by_fw) S(0x0F00, 1U << 5);
    load_rar0(); sim.rx_head = sim.tx_head = 0;
    S(0x00, 1U << 26); sim.reset_reads = 3;
    if (with_phy) { sim.phy_resets++; phy_defaults(); S(0x08, R(0x08) | (1U << 10)); sim.phy_reset_reads = 3; }
    status_update(); sim.prev_level = false;
}
static void tx_tail(uint32_t tail) {
    uint64_t base = R(TXB()) | (uint64_t)R(TXB() + 4) << 32; unsigned count = R(TXB() + 8) / 16;
    assert(base && count == 64 && tail < count);
    if (sim.tx_hang) return;
    for (unsigned guard = 0; sim.tx_head != tail && guard < 128; guard++) {
        uint32_t* d = (uint32_t*)(uintptr_t)(base + 16ULL * sim.tx_head);
        uint64_t address = d[0] | (uint64_t)d[1] << 32; unsigned length = d[2] & 0xFFFF;
        if (address == base && length == 512 && !advanced_sim()) sim.flush_tx_seen++;   /* the I219 flush descriptor (IFCS only) */
        else if (advanced_sim()) assert(((d[2] >> 20) & 15) == 3 && (d[2] & (1U << 24)) && (d[2] & (1U << 29)) && (d[3] >> 14) == length);
        else assert((d[2] & (1U << 24)) && (d[2] & (1U << 27)));
        if (address == base && length == 512 && !advanced_sim()) {}
        else { assert(length >= 60 && length <= 1514); memcpy(sim.tx_last, (const void*)(uintptr_t)address, length); sim.tx_last_length = length; sim.tx_frames++; }
        d[3] |= 1;                                           /* DD */
        sim.tx_head = (sim.tx_head + 1) % count;
        if (sim.flush_tx_seen) cfg[0xE5] &= (uint8_t)~1;     /* hang state cleared by the flush */
    }
    S(TXB() + 0x10, sim.tx_head);
    raise_cause(0x01);
}
static bool rx_frame(const uint8_t* frame, unsigned length) {
    if (!(R(0x100) & 2)) return false;
    uint64_t base = R(RXB()) | (uint64_t)R(RXB() + 4) << 32; unsigned count = R(RXB() + 8) / 16;
    assert(base && count == 256);
    if (sim.rx_head == R(RXB() + 0x18)) { sim.rx_missed++; raise_cause(0x40); return false; }
    uint32_t* d = (uint32_t*)(uintptr_t)(base + 16ULL * sim.rx_head);
    uint8_t* buffer = (uint8_t*)(uintptr_t)(d[0] | (uint64_t)d[1] << 32); assert(buffer);
    memcpy(buffer, frame, length);
    if (advanced_sim()) { d[2] = 0x3; d[3] = length; } else { d[2] = length; d[3] = 0x3; }
    sim.rx_head = (sim.rx_head + 1) % count; S(RXB() + 0x10, sim.rx_head);
    raise_cause(0x80);
    return true;
}
static void sim_link(bool up, unsigned speed, bool full) { sim.link = up; sim.speed = speed; sim.full = full; status_update(); raise_cause(0x04); }

static uint32_t rd(unsigned r) {
    if (sim.dead) return 0xFFFFFFFFU;
    assert(r < 0x20000 && !(r & 3));
    switch (r) {
    case 0x00: if ((R(0) & (1U << 26)) && sim.reset_reads && --sim.reset_reads == 0) S(0, R(0) & ~(1U << 26)); return R(0);
    case 0x08:
        if (sim.phy_reset_reads && --sim.phy_reset_reads == 0 && sim.kind == SIM_PCH) S(0x08, R(0x08) | (1U << 9));   /* LAN_INIT_DONE */
        if (R(0) & (1U << 2)) S(0x08, R(0x08) & ~(1U << 19)); else S(0x08, R(0x08) | (1U << 19));
        status_update(); return R(0x08);
    case 0x18: if (sim.lpcd_reads && --sim.lpcd_reads == 0) S(0x18, R(0x18) | (1U << 2)); return R(0x18);
    case 0x5B54:
        if ((R(0x5B54) & (1U << 10)) && sim.ulp_reads && --sim.ulp_reads == 0) { S(0x5B54, R(0x5B54) & ~(1U << 10)); sim.ulp = false; }
        return sim.reset_blocked ? R(0x5B54) & ~(1U << 6) : R(0x5B54) | (1U << 6);
    case 0x5B50:
        if (sim.kind == SIM_I225) { uint32_t v = R(0x5B50); if (!(v & 1)) S(0x5B50, v | 1); return v; }   /* SMBI: set by the read */
        return R(0x5B50);
    case 0x5B5C: if (sim.fw_phy_busy) { sim.fw_phy_busy--; return R(0x5B5C) | (2U << 16); } return R(0x5B5C);
    }
    if (r == ICR_REG()) { uint32_t v = R(r); S(r, 0); irq_check(); return v; }
    return R(r);
}
static void wr(unsigned r, uint32_t v) {
    if (sim.dead) return;
    assert(r < 0x20000 && !(r & 3));
    assert(!(sim.kind == SIM_PCH && r == 0x14));            /* PCH has no EERD: 0x14 is FEXTNVM5, never written */
    switch (r) {
    case 0x00:
        if (sim.kind == SIM_PCH) {
            if ((v & (1U << 16)) && !(v & (1U << 17))) sim.lpcd_reads = 0;
            if (!(v & (1U << 16)) && (R(0) & (1U << 16))) phy_power_cycle();
        }
        if (v & (1U << 26)) { mac_reset((v & (1U << 31)) != 0); return; }
        if ((v & (1U << 31)) && !(R(0) & (1U << 31))) { sim.phy_resets++; phy_defaults(); if (sim.kind == SIM_PCH) { S(0x08, R(0x08) | (1U << 10)); sim.phy_reset_reads = 3; } else S(0x0E14, R(0x0E14) & ~(1U << 8)); }
        if (!(v & (1U << 31)) && (R(0) & (1U << 31)) && sim.kind == SIM_I225) S(0x0E14, R(0x0E14) | (1U << 8));
        S(0, v); return;
    case 0x08: S(0x08, (R(0x08) & ~((1U << 9) | (1U << 10))) | (v & ((1U << 9) | (1U << 10)))); return;   /* W0C of init-done/PHYRA */
    case 0x20: mdic_command(v); return;
    case 0x0F00:
        if (v & (1U << 7)) sim.gate_seen = true;
        if (sim.kind == SIM_PCH && (v & (1U << 5)) && !(R(0x0F00) & (1U << 5)) && sim.flag_owned_by_fw) return;
        if (sim.flag_owned_by_fw) { S(0x0F00, (v & ~(1U << 5)) | (1U << 5)); return; }
        S(0x0F00, v); return;
    case 0x5B50:
        if (sim.kind == SIM_PCH && sim.me) {                  /* H2ME: enforce settings with ULP clear -> ME exits ULP */
            if ((v & (1U << 12)) && !(v & (1U << 11)) && (R(0x5B54) & (1U << 10))) sim.ulp_reads = 4;
            S(0x5B50, v); return;
        }
        S(0x5B50, v); return;
    }
    if (r == ICR_REG()) { S(r, R(r) & ~v); irq_check(); return; }
    if (r == IMS_REG()) { S(r, R(r) | v); irq_check(); return; }
    if (r == IMC_REG()) { S(IMS_REG(), R(IMS_REG()) & ~v); irq_check(); return; }
    if (r == TXB() + 0x18) { S(r, v); tx_tail(v); return; }
    if (advanced_sim() && (r == RXB() + 0x28 || r == TXB() + 0x28)) { S(r, v); return; }
    S(r, v);
}

/* ---- kernel services ---- */
static pci_device_t devices[2];
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    assert(b == 0 && d == 31 && f == 6); if (sim.dead) return 0xFFFFFFFFU; off &= 0xFC;
    if (off >= 0x10 && off < 0x28 && bar_sizing[(off - 0x10) / 4]) return bar_mask[(off - 0x10) / 4];
    uint32_t v; memcpy(&v, cfg + off, 4); return v;
}
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { return (uint16_t)(pci_read32(b, d, f, off & 0xFC) >> ((off & 2) * 8)); }
uint8_t pci_read8(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { return (uint8_t)(pci_read32(b, d, f, off & 0xFC) >> ((off & 3) * 8)); }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) {
    assert(b == 0 && d == 31 && f == 6); if (sim.dead) return; off &= 0xFC;
    if (off >= 0x10 && off < 0x28) { unsigned i = (off - 0x10) / 4; bar_sizing[i] = v == 0xFFFFFFFFU; if (!bar_sizing[i]) { uint32_t m = bar_mask[i]; uint32_t w = m ? (v & m) | (v & 0xF & m) : 0; memcpy(cfg + off, &w, 4); } return; }
    if (off == 4) { cfg[4] = (uint8_t)v; cfg[5] = (uint8_t)(v >> 8); return; }
    memcpy(cfg + off, &v, 4);
}
pci_device_t* pci_get_devices(void) { return devices; }
int pci_device_count(void) { return 1; }
uint8_t pci_find_capability(const pci_device_t* d, uint8_t id) { (void)d; for (uint8_t at = cfg[0x34]; at; at = cfg[at + 1]) if (cfg[at] == id) return at; return 0; }
static bool fail_msi; static unsigned vectors_live, msi_vector, intx_vector; static irq_handler_t handlers[256]; static bool taken[256];
int pci_enable_msi(pci_device_t* d, unsigned vector, unsigned cpu) { (void)d; (void)cpu; if (fail_msi) return -22; cfg[0x52] |= 1; cfg[5] |= 4; msi_vector = vector; return 0; }
int pci_disable_msi(pci_device_t* d) { (void)d; cfg[0x52] &= (uint8_t)~1; return 0; }
int pci_msix_count(const pci_device_t* d) { (void)d; return -95; }
int pci_enable_msix(pci_device_t* d, unsigned e, unsigned v, unsigned c) { (void)d; (void)e; (void)v; (void)c; return -95; }
int pci_disable_msix(pci_device_t* d) { (void)d; return -95; }
volatile void* pci_ecam_address(unsigned s, unsigned b, unsigned d, unsigned f) { (void)s; (void)b; (void)d; (void)f; return NULL; }
int irq_alloc_vector(unsigned flags) { (void)flags; for (unsigned v = 0x40; v < 0x90; v++) if (!taken[v]) { taken[v] = true; vectors_live++; return (int)v; } return -28; }
int irq_register(unsigned vector, irq_handler_t handler, void* context) { (void)context; handlers[vector] = handler; return 0; }
void irq_free_vector(unsigned vector) { assert(taken[vector]); taken[vector] = false; handlers[vector] = NULL; vectors_live--; }
int irq_attach_isa(unsigned line, unsigned flags, irq_handler_t handler, void* context) { (void)flags; (void)context; if (line != 11) return -22; intx_vector = 0x20 + line; handlers[intx_vector] = handler; return (int)intx_vector; }
int irq_attach_pci(const pci_device_t* device, unsigned flags, irq_handler_t handler, void* context) {   /* pcidev.c (power: _PRT) */
    (void)flags; unsigned line = device->irq_line;
    if (line < 3 || line >= 16 || line == 8 || line == 13) return 0;
    return irq_attach_isa(line, IRQ_PCI_INTX, handler, context);
}
int irq_unregister(unsigned vector, irq_handler_t handler, void* context) { (void)context; (void)handler; handlers[vector] = NULL; return 0; }
void irq_mask(unsigned line) { (void)line; }
volatile void* mmio_map(uint64_t p, uint64_t s, unsigned c) { (void)p; (void)s; (void)c; return NULL; }
uint64_t boot_pd[2048];
void usb_irq(unsigned vector) { (void)vector; }
static uint64_t clock_ns;
uint64_t native_clock_ns(void) { return clock_ns += 200; }
void irq_wait_restricted(void) { clock_ns += 1000000; }
static unsigned allocations, dma32_allocations;
uint64_t arena_alloc(uint64_t bytes) { uint8_t* b = calloc(1, bytes + 4096); assert(b); allocations++; return ((uint64_t)(uintptr_t)b + 4095) & ~4095ULL; }
uint64_t dma32_alloc(uint64_t bytes) { dma32_allocations++; return arena_alloc(bytes); }
void arena_free(uint64_t address, uint64_t bytes) { (void)address; (void)bytes; assert(allocations); allocations--; }
bool native_random(void* o, uint64_t n) { memset(o, 0x5A, n); return true; }
static net_device_t* registered;
int net_register_device(net_device_t* device) { registered = device; return 0; }
net_device_t* net_get_device(int index) { return index == 0 ? registered : NULL; }
bool native_network_available(void) { return registered != NULL; }
static unsigned delivered, delivered_bad;
void net_deliver_packet(const void* data, uint16_t length) { const uint8_t* p = data; delivered++; for (unsigned i = 14; i < length; i++) if (p[i] != (uint8_t)(i * 3 + p[13])) { delivered_bad++; break; } }
static unsigned link_events;
void netlink_link_changed(void) { link_events++; }
static char log_text[1 << 16]; static unsigned log_used;
void vga_print(const char* text) { size_t n = strlen(text); if (log_used + n < sizeof(log_text)) { memcpy(log_text + log_used, text, n); log_used += (unsigned)n; log_text[log_used] = 0; } fputs(text, stdout); }
static bool logged(const char* text) { return strstr(log_text, text) != NULL; }
bool disk_lookup(const char* path, fat32_file_t* out) { (void)path; (void)out; return false; }
int32_t disk_read_file(fat32_file_t* f, uint64_t o, uint64_t l, void* b) { (void)f; (void)o; (void)l; (void)b; return -5; }
/* storage: vfs.h routes fat32_lookup / fat32_read_file through the mount table (mnt_*) */
int32_t mnt_read_file(fat32_file_t* file, uint64_t offset, uint64_t length, void* buffer) { return disk_read_file(file, offset, length, buffer); }
#include "../arch/x86_64/pcidev.c"
#include "../arch/x86_64/netdrv.c"
#include "../arch/x86_64/e1000.c"
void native_nic_irq(unsigned vector) { e1000_nic_irq(vector); }
bool native_nic_link(struct nic_link* link) { return e1000_nic_link(link); }

/* ---- harness ---- */
static void deliver(void) {
    for (unsigned guard = 0; guard < 16; guard++) {
        if (sim.messages && msi_vector && handlers[msi_vector]) { sim.messages--; handlers[msi_vector](msi_vector, NULL); continue; }
        if (!msi_on() && intx_vector && handlers[intx_vector] && (R(ICR_REG()) & R(IMS_REG()))) { handlers[intx_vector](intx_vector, NULL); continue; }
        return;
    }
    assert(!"interrupt storm");
}
static void service(void) { for (unsigned i = 0; i < 64; i++) { deliver(); if (!e1000_nic_rx_pending()) break; e1000_nic_poll(); } deliver(); }
static void advance(uint64_t ns) { clock_ns += ns; service(); }
static void frame(uint8_t* f, unsigned length, uint8_t seed) { memset(f, 0xFF, 6); memset(f + 6, 0x02, 6); f[12] = 0x08; f[13] = seed; for (unsigned i = 14; i < length; i++) f[i] = (uint8_t)(i * 3 + seed); }
static void scenario(int kind, unsigned pch_generation, uint16_t device) {
    memset(&nic, 0, sizeof(nic)); power_on(kind, pch_generation, device);
    devices[0] = (pci_device_t){.bus = 0, .device = 31, .function = 6, .vendor_id = 0x8086, .device_id = device, .class_code = PCI_CLASS_NETWORK, .irq_line = 11};
    registered = NULL; fail_msi = false; memset(handlers, 0, sizeof(handlers)); memset(taken, 0, sizeof(taken)); vectors_live = 0; msi_vector = intx_vector = 0;
    delivered = delivered_bad = 0; link_events = 0; log_used = 0; log_text[0] = 0; allocations = dma32_allocations = 0;
}
static void traffic(unsigned frames) {
    uint8_t f[1600]; unsigned before = delivered;
    for (unsigned i = 0; i < frames; i++) { frame(f, 64 + (i * 41) % 1400, (uint8_t)i); assert(rx_frame(f, 64 + (i * 41) % 1400)); if (i % 40 == 39) service(); }
    service(); assert(delivered == before + frames && !delivered_bad);
    unsigned sent = sim.tx_frames;
    for (unsigned i = 0; i < 150; i++) { frame(f, 60 + i * 9, (uint8_t)i); assert(nic.device.send(&nic.device, f, (uint16_t)(60 + i * 9)) == 0); if (i % 30 == 29) service(); }
    service(); assert(sim.tx_frames == sent + 150 && sim.tx_last_length == 60 + 149 * 9);
    uint8_t expect[1600]; frame(expect, 60 + 149 * 9, 149); assert(!memcmp(sim.tx_last, expect, sim.tx_last_length));
}
static void common_failures(void) {
    /* transmitter hang: 63 queued (one slot stays free: TDT == TDH would mean "empty"), then EAGAIN, then the 5 s
     * watchdog resets the controller */
    uint8_t f[100]; frame(f, 100, 1);
    unsigned resets = sim.mac_resets, queued = 0; int r;
    sim.tx_hang = true;
    while ((r = nic.device.send(&nic.device, f, 100)) == 0) queued++;
    assert(r == -11 && queued == 63);
    advance(2000000000ULL); assert(sim.mac_resets == resets);
    advance(4000000000ULL); assert(sim.mac_resets == resets + 1 && logged("transmit timeout, resetting the controller"));
    sim.tx_hang = false;
    sim_link(true, sim.speed ? sim.speed : 1000, true); service(); traffic(20);
    /* device gone, then back */
    sim.dead = true; advance(1100000000ULL); assert(nic.dead && !nic.device.link_up && logged("device not responding"));
    sim.dead = false; advance(1100000000ULL); assert(!nic.dead && logged("device answers again"));
    sim_link(true, 1000, true); service(); traffic(10);
}

int main(void) {
    clock_ns = 1000000000ULL;
    uint8_t f[1600];

    /* ===== I219-V (Cannon Lake PCH, 8086:15BC) with an active ME and the PHY left in ULP by the firmware ===== */
    scenario(SIM_PCH, PCH_CNP, 0x15BC); sim.me = true; S(0x5B54, R(0x5B54) | (1U << 15) | (1U << 10)); sim.ulp = true;
    assert(e1000_nic_init()); assert(logged("Intel I219-V native DMA/IRQ ready"));
    assert(!sim.ulp && !(R(0x5B50) & (1U << 12)) && !sim.lanphypc_toggles);            /* ME cleared ULP, enforce bit dropped */
    assert(sim.gate_seen);                                                              /* automatic PHY config was gated */
    assert(!memcmp(nic.device.mac.addr, nvm_mac, 6) && msi_on() && allocations == 1 && !dma32_allocations);   /* RAR0 MAC, 64-bit DMA */
    assert((R(0x18) & (1U << 22)) && (R(0x18) & (1U << 20)) && (R(0x18) & (1U << 17)) && (R(0x18) & (1U << 28)));
    assert((R(0x5008) & 0xC0) == 0xC0 && (R(0x100C) & (1U << 16)) && (R(0) & (1U << 19)) && (R(0x3004) & 0x50000) == 0x50000);
    assert((R(0x3840) & 0x0D800000U) == 0x0D800000U && (R(0x3828) & 0x3F) == 0x1F && !(R(0x5F04) & 0x1000));   /* FFLT_DBG only TGP+ */
    assert(R(0xC4) == 195 && (R(0x100) & 2) && (R(0x400) & 2) && sim.phy[1][0][4] == 0x0DE1 && (sim.phy[1][0][9] & 0x300) == 0x200);
    assert(sim.phy[1][0][0] & 0x1000);
    service(); sim_link(true, 1000, true); service();
    assert(nic.device.link_up && nic.link.speed == 1000 && nic.link.duplex == NIC_DUPLEX_FULL && link_events == 1 && logged("eth0: link up 1000 Mb/s full duplex"));
    assert((R(0x410) & 0x3FF) == 0x08 && sim.emi[0xB20C] == 1 && (sim.phy[0][772][28] & 0x7FF) == 0xFA && (sim.phy[0][770][17] & 0x200));
    assert(((sim.phy[0][776][20] >> 2) & 0x3FF) == 0x18 && (R(0x24) & 7) == 7);
    traffic(300);
    sim_link(true, 100, true); service();
    assert(nic.link.speed == 100 && (R(0x410) & 0x3FF) == 0x0C && (sim.phy[0][772][28] & 0x7FF) == 0x3E8 && sim.phy[0][776][20] == 0xC023);
    sim_link(true, 10, false); service(); assert((R(0x410) & 0x3FF) == 0xFF && sim.emi[0xB20C] == 0 && nic.link.duplex == NIC_DUPLEX_HALF);
    sim_link(false, 0, false); service(); assert(!nic.device.link_up && link_events == 4);
    uint8_t g[60]; frame(g, 60, 1); assert(nic.device.send(&nic.device, g, 60) == -100);
    sim_link(true, 1000, true); service(); traffic(30);
    common_failures();
    /* suspend with the I219 ring-hang flag set: the flush descriptor runs, then power is lost and restored */
    cfg[0xE5] |= 1; unsigned flushes = sim.flush_tx_seen;
    e1000_nic_suspend();
    assert(sim.flush_tx_seen == flushes + 1 && !(cfg[0xE5] & 1) && !(R(0x100) & 2) && !(cfg[4] & 4) && !nic.device.link_up && (R(0x5BBC) & (1U << 13)));
    sim.me = true; power_on(SIM_PCH, PCH_CNP, 0x15BC); sim.me = true; S(0x5B54, R(0x5B54) | (1U << 15));
    memset(cfg + 0x10, 0, 4); cfg[4] = 0;
    assert(e1000_nic_resume());
    assert(cfg[0x12] == 0x40 && cfg[0x13] == 0xFE && msi_on() && (cfg[4] & 6) == 6 && (R(0x100) & 2) && R(0xC4) == 195);
    sim_link(true, 1000, true); service(); assert(nic.device.link_up); traffic(40);
    puts("I219 (CNP, ME) scenario passed");

    /* ===== I219-LM (Tiger Lake, 8086:15FB) without ME, PHY stuck in SMBus mode: LANPHYPC power cycle ===== */
    scenario(SIM_PCH, PCH_TGP, 0x15FB); sim.phy_smbus = true; sim.ulp = true; S(0xE4, 1U << 5);
    assert(e1000_nic_init() && logged("Intel I219-LM native DMA/IRQ ready"));
    assert(sim.lanphypc_toggles >= 1 && !sim.ulp && !sim.phy_smbus && !(R(0x18) & (1U << 11)) && !(R(0xE4) & (1U << 5)));
    assert(sim.ulp_commit_seen && sim.k1_on_seen);                                     /* ULP config committed, K1 back on */
    assert(R(0x5F04) & 0x1000);                                                         /* TGP: DMA clock not gated */
    sim_link(true, 1000, true); service(); traffic(60);
    puts("I219 (TGP, no ME, SMBus) scenario passed");

    /* ===== I219 (Sunrise Point): PHY never answers -> clean failure; firmware keeps the software flag -> clean failure ===== */
    scenario(SIM_PCH, PCH_SPT, 0x15B8); sim.phy_dead = true;
    uint64_t t0 = clock_ns;
    assert(!e1000_nic_init() && logged("PHY not accessible") && !allocations && !(cfg[4] & 4) && !registered && clock_ns - t0 < 5000000000ULL);
    scenario(SIM_PCH, PCH_SPT, 0x15B8); sim.flag_owned_by_fw = true; S(0x0F00, 1U << 5);
    t0 = clock_ns;
    assert(!e1000_nic_init() && logged("software flag stuck") && !allocations && !(cfg[4] & 4) && clock_ns - t0 < 20000000000ULL);
    /* reset blocked by the ME: no PHY reset, the MAC still comes up */
    scenario(SIM_PCH, PCH_SPT, 0x156F); sim.me = true; S(0x5B54, R(0x5B54) | (1U << 15)); sim.reset_blocked = true;
    assert(e1000_nic_init() && logged("PHY reset blocked by the management engine") && !sim.phy_resets);
    sim_link(true, 1000, true); service(); traffic(20);
    puts("I219 failure scenarios passed");

    /* ===== I226-V (8086:125C): SW/FW semaphores, PHY reset completion, 2.5G advertisement, 0x1500 interrupts ===== */
    scenario(SIM_I225, 0, 0x125C); sim.fw_phy_busy = 5;
    assert(e1000_nic_init() && logged("Intel I226-V native DMA/IRQ ready"));
    assert(!(R(0x5B5C) & 2) && !(R(0x5B50) & 3));                                      /* semaphores released */
    assert(mmd[7][0x20] & 0x0080);                                                      /* 2.5G advertised (MMD 7.32) */
    assert(sim.phy_resets >= 1 && (R(0x0E14) & (1U << 8)) && R(0x1680) == 200 && !R(0x1510) && (R(0x1508) & 0xDC) == 0xDC);
    assert(!memcmp(nic.device.mac.addr, nvm_mac, 6) && (R(0xC00C) & (1U << 25)) && (R(0xC028) & (1U << 25)) && (R(0xE028) & (1U << 25)));
    sim_link(true, 2500, true); service(); assert(nic.link.speed == 2500 && logged("eth0: link up 2500 Mb/s full duplex"));
    traffic(300);
    sim_link(true, 1000, true); service(); assert(nic.link.speed == 1000);
    common_failures();
    e1000_nic_suspend(); power_on(SIM_I225, 0, 0x125C); memset(cfg + 0x10, 0, 4); cfg[4] = 0;
    assert(e1000_nic_resume() && msi_on() && (mmd[7][0x20] & 0x80)); sim_link(true, 2500, true); service(); traffic(30);
    puts("I226 scenario passed");

    /* ===== 82574L (the QEMU e1000e model) keeps its path: EERD MAC, 32-bit DMA zone, ITR 0 ===== */
    scenario(SIM_82574, 0, 0x10D3);
    assert(e1000_nic_init() && logged("Intel 82574L (e1000e) native DMA/IRQ ready") && dma32_allocations == 1 && R(0xC4) == 0);
    sim_link(true, 1000, true); service(); assert(nic.link.speed == 1000); traffic(100);
    /* INTx when MSI fails, polled without a line */
    scenario(SIM_82574, 0, 0x10D3); fail_msi = true;
    assert(e1000_nic_init() && intx_vector == 0x2B); sim_link(true, 1000, true); service(); traffic(40);
    scenario(SIM_82574, 0, 0x10D3); fail_msi = true; devices[0].irq_line = 0;
    assert(e1000_nic_init() && logged("receive is polled")); sim_link(true, 1000, true); advance(1100000000ULL); assert(nic.device.link_up); traffic(40);
    frame(f, 100, 1); (void)f;
    puts("82574L scenarios passed");
    puts("E1000 SIMULATOR TESTS PASSED");
    return 0;
}
