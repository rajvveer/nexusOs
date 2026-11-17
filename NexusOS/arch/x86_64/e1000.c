/* Intel gigabit Ethernet: 82540EM (e1000), 82574L (e1000e), 82576 (igb), the PCH-integrated I217/I218/I219
 * (e1000e "ich8lan": Lynx Point .. Meteor Lake and later) and the I225/I226 2.5 GbE controllers (igc). All share
 * the 8254x register model; the differences handled here are:
 *  - the EERD layout (82540: done bit 4, address at bit 8; 82574/82576: done bit 1, address at bit 2). PCH parts
 *    have no EERD (the NVM is the shared SPI flash) and I225 keeps it at 0x12014; both take the MAC address from
 *    receive address 0, which the hardware loads from the NVM at reset (as Linux does);
 *  - the queue registers (igb/igc use the queue-0 block at 0xC000/0xE000 with SRRCTL and per-queue enable bits)
 *    and the descriptor formats (igb/igc advanced one-buffer receive and advanced data transmit descriptors);
 *  - interrupt registers (igc: ICR/IMS/IMC at 0x1500 with auto-mask);
 *  - bring-up: PCH parts share the PHY with the management engine (EXTCNF_CTRL software flag), may have it in
 *    ultra-low-power or SMBus mode after the firmware (ULP disable, SMBus unforce, LANPHYPC power cycle), need a
 *    combined MAC+PHY reset, LAN_INIT_DONE, a descriptor-ring flush before resets (I219 hang erratum) and
 *    link-up tuning (TIPG, EMI receive configuration, PLL clock gating, K1 clock request, pointer gap); I225 takes
 *    the SW/FW PHY semaphore, a PHY reset with completion polling and 2.5G advertisement through MMD 7.32.
 * Semantics follow Intel's datasheets as documented by the Linux e1000e/igc drivers; no code is copied.
 * The I217/I219/I225/I226 paths are NOT YET RUN ON REAL HARDWARE: they are verified against the simulator in
 * tests/e1000_sim_test.c; QEMU's e1000e (82574L) and igb (82576) models cover the shared data path.
 *
 * Like rtl8139.c the hot paths keep register traffic (VM exits under WHPX) low: the IRQ handler reads ICR once
 * (read-to-clear) and only flags receive or link work; the receive drain walks descriptors in memory and returns
 * them with one RDT write per batch; transmit copies into the slot's buffer and writes TDT once, reclaiming slots
 * lazily from their DD bits (no transmit interrupts). When the ring is full QEMU queues frames instead of dropping
 * them, and an RDT write may deliver more frames at once, so the drain looks at the ring again after returning
 * descriptors. Link changes are handled in the poll (PHY access is slow); a 1 s watchdog polls the link without an
 * interrupt line, resets the controller when a frame stays queued for 5 s with the link up, and notices a device
 * that reads all-ones. Suspend/resume: e1000_nic_suspend()/e1000_nic_resume() (nic.c native_nic_*). */
#include "netdrv.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define CTRL 0x0000
#define STATUS 0x0008
#define EECD 0x0010
#define EERD 0x0014
#define CTRL_EXT 0x0018
#define MDIC 0x0020
#define FEXTNVM6 0x0010
#define FEXTNVM4 0x0024
#define FEXTNVM3 0x003C
#define FEXTNVM7 0x00E4
#define ICR 0x00C0
#define ITR 0x00C4
#define IMS 0x00D0
#define IMC 0x00D8
#define RCTL 0x0100
#define TCTL 0x0400
#define TIPG 0x0410
#define EXTCNF_CTRL 0x0F00
#define PCIEANACFG 0x0F18
#define PBECCSTS 0x100C
#define RDTR 0x2820
#define RXDCTL0 0x2828
#define RADV 0x282C
#define KABGTXD 0x3004
#define TDLEN0 0x3808
#define TXDCTL0 0x3828
#define TARC0 0x3840
#define TXDCTL1 0x3928
#define TARC1 0x3940
#define RFCTL 0x5008
#define MTA 0x5200
#define RAL 0x5400
#define RAH 0x5404
#define H2ME 0x5B50
#define SWSM 0x5B50
#define FWSM 0x5B54
#define SW_FW_SYNC 0x5B5C
#define FEXTNVM11 0x5BBC
#define MANC 0x5820
#define FFLT_DBG 0x5F04
#define I225_PHPM 0x0E14
#define I225_ICR 0x1500
#define I225_IMS 0x1508
#define I225_IMC 0x150C
#define I225_IAM 0x1510
#define I225_EITR0 0x1680
#define I225_EERD 0x12014
/* Queue-0 register block offsets (from rx_base/tx_base). */
#define Q_BAL 0x00
#define Q_BAH 0x04
#define Q_LEN 0x08
#define Q_SRRCTL 0x0C
#define Q_HEAD 0x10
#define Q_TAIL 0x18
#define Q_DCTL 0x28

#define CTRL_GIO_MASTER_DISABLE (1U << 2)
#define CTRL_ASDE (1U << 5)
#define CTRL_SLU (1U << 6)
#define CTRL_FRCSPD (1U << 11)
#define CTRL_FRCDPX (1U << 12)
#define CTRL_LANPHYPC_OVERRIDE (1U << 16)
#define CTRL_LANPHYPC_VALUE (1U << 17)
#define CTRL_MEHE (1U << 19)
#define CTRL_RST (1U << 26)
#define CTRL_PHY_RST (1U << 31)
#define CTRL_EXT_LPCD (1U << 2)
#define CTRL_EXT_FORCE_SMBUS (1U << 11)
#define CTRL_EXT_RO_DIS (1U << 17)
#define CTRL_EXT_PHYPDEN (1U << 20)
#define CTRL_EXT_DRV_LOAD (1U << 28)
#define STATUS_FD (1U << 0)
#define STATUS_LU (1U << 1)
#define STATUS_SPEED_100 (1U << 6)
#define STATUS_SPEED_1000 (1U << 7)
#define STATUS_LAN_INIT_DONE (1U << 9)
#define STATUS_PHYRA (1U << 10)
#define STATUS_GIO_MASTER_ENABLE (1U << 19)
#define STATUS_SPEED_2500 (1U << 22)
#define EECD_AUTO_RD (1U << 9)
#define EXTCNF_SWFLAG (1U << 5)
#define EXTCNF_GATE_PHY_CFG (1U << 7)
#define FWSM_RSPCIPHY (1U << 6)
#define FWSM_ULP_CFG_DONE (1U << 10)
#define FWSM_FW_VALID (1U << 15)
#define H2ME_ULP (1U << 11)
#define H2ME_ENFORCE (1U << 12)
#define SWSM_SMBI 1U
#define SWSM_SWESMBI 2U
#define SWFW_PHY0 2U
#define MANC_BLK_PHY_RST (1U << 18)
#define PHPM_RST_COMPLETE (1U << 8)
#define RCTL_EN (1U << 1)
#define RCTL_BAM (1U << 15)
#define RCTL_SECRC (1U << 26)
#define TCTL_EN (1U << 1)
#define TCTL_PSP (1U << 3)
#define TCTL_MULR (1U << 28)
#define QUEUE_ENABLE (1U << 25)
#define INT_TXDW 0x01U
#define INT_LSC 0x04U
#define INT_RXSEQ 0x08U
#define INT_RXDMT0 0x10U
#define INT_RXO 0x40U
#define INT_RXT0 0x80U
#define RX_IRQ_BITS (INT_RXDMT0 | INT_RXO | INT_RXT0)
/* PHY registers: MII plus the PCH "HV" pages (PHY_REG(page, reg)) */
#define MII_BMCR 0
#define MII_BMSR 1
#define MII_ADVERTISE 4
#define MII_CTRL1000 9
#define PHY_REG(page, reg) ((page) << 5 | (reg))
#define HV_PORT_GEN_CFG PHY_REG(769, 17)
#define HV_CV_SMB_CTRL PHY_REG(769, 23)
#define HV_PM_CTRL PHY_REG(770, 17)
#define HV_PLL_CLOCK_GATE PHY_REG(772, 28)
#define HV_PTR_GAP PHY_REG(776, 20)
#define HV_ULP_CONFIG1 PHY_REG(779, 16)
#define EMI_ADDR 0x10
#define EMI_DATA 0x11
#define I217_RX_CONFIG 0xB20C

#define RX_COUNT 256U
#define TX_COUNT 64U
#define BUFFER 2048U
#define RX_BATCH_FRAMES 256U
/* DMA layout: RX descriptors, TX descriptors, RX buffers, TX buffers. */
#define RX_RING_OFFSET 0U
#define TX_RING_OFFSET (RX_COUNT * 16U)
#define RX_BUFFER_OFFSET 8192U
#define TX_BUFFER_OFFSET (RX_BUFFER_OFFSET + RX_COUNT * BUFFER)
#define DMA_ALLOCATION (TX_BUFFER_OFFSET + TX_COUNT * BUFFER)
#define WATCHDOG_NS 1000000000ULL
#define TX_TIMEOUT_NS 5000000000ULL
_Static_assert(TX_RING_OFFSET + TX_COUNT * 16U <= RX_BUFFER_OFFSET, "descriptor rings overlap buffers");

enum model { MODEL_82540, MODEL_82574, MODEL_82576, MODEL_PCH, MODEL_I225 };
/* PCH generations (Linux board_pch_*): only the behaviour differences used here. */
enum { PCH_LPT = 1, PCH_SPT, PCH_CNP, PCH_TGP, PCH_ADP, PCH_MTP };
struct descriptor { volatile uint64_t address; volatile uint32_t word2, word3; };
static const struct e1000_id { uint16_t device; uint8_t model, pch; const char* name; } supported[] = {
    {0x10D3, MODEL_82574, 0, "82574L (e1000e)"}, {0x10C9, MODEL_82576, 0, "82576 (igb)"}, {0x10E6, MODEL_82576, 0, "82576 (igb)"},
    {0x10E7, MODEL_82576, 0, "82576 (igb)"}, {0x10E8, MODEL_82576, 0, "82576 (igb)"}, {0x100E, MODEL_82540, 0, "82540EM (e1000)"},
    {0x100F, MODEL_82540, 0, "82545EM (e1000)"}, {0x1004, MODEL_82540, 0, "82543GC (e1000)"}, {0x1011, MODEL_82540, 0, "82545EM (e1000)"},
    {0x1026, MODEL_82540, 0, "82545GM (e1000)"}, {0x1010, MODEL_82540, 0, "82546EB (e1000)"}, {0x100C, MODEL_82540, 0, "82544GC (e1000)"},
    /* I217 / I218 (Lynx Point, Wildcat Point) */
    {0x153A, MODEL_PCH, PCH_LPT, "I217-LM"}, {0x153B, MODEL_PCH, PCH_LPT, "I217-V"}, {0x155A, MODEL_PCH, PCH_LPT, "I218-LM"},
    {0x1559, MODEL_PCH, PCH_LPT, "I218-V"}, {0x15A0, MODEL_PCH, PCH_LPT, "I218-LM"}, {0x15A1, MODEL_PCH, PCH_LPT, "I218-V"},
    {0x15A2, MODEL_PCH, PCH_LPT, "I218-LM"}, {0x15A3, MODEL_PCH, PCH_LPT, "I218-V"},
    /* I219 */
    {0x156F, MODEL_PCH, PCH_SPT, "I219-LM"}, {0x1570, MODEL_PCH, PCH_SPT, "I219-V"}, {0x15B7, MODEL_PCH, PCH_SPT, "I219-LM"},
    {0x15B8, MODEL_PCH, PCH_SPT, "I219-V"}, {0x15B9, MODEL_PCH, PCH_SPT, "I219-LM"}, {0x15D7, MODEL_PCH, PCH_SPT, "I219-LM"},
    {0x15D8, MODEL_PCH, PCH_SPT, "I219-V"}, {0x15E3, MODEL_PCH, PCH_SPT, "I219-LM"}, {0x15D6, MODEL_PCH, PCH_SPT, "I219-V"},
    {0x0D53, MODEL_PCH, PCH_SPT, "I219-LM"}, {0x0D55, MODEL_PCH, PCH_SPT, "I219-V"},
    {0x15BD, MODEL_PCH, PCH_CNP, "I219-LM"}, {0x15BE, MODEL_PCH, PCH_CNP, "I219-V"}, {0x15BB, MODEL_PCH, PCH_CNP, "I219-LM"},
    {0x15BC, MODEL_PCH, PCH_CNP, "I219-V"}, {0x15DF, MODEL_PCH, PCH_CNP, "I219-LM"}, {0x15E0, MODEL_PCH, PCH_CNP, "I219-V"},
    {0x15E1, MODEL_PCH, PCH_CNP, "I219-LM"}, {0x15E2, MODEL_PCH, PCH_CNP, "I219-V"}, {0x0D4E, MODEL_PCH, PCH_CNP, "I219-LM"},
    {0x0D4F, MODEL_PCH, PCH_CNP, "I219-V"}, {0x0D4C, MODEL_PCH, PCH_CNP, "I219-LM"}, {0x0D4D, MODEL_PCH, PCH_CNP, "I219-V"},
    {0x15FB, MODEL_PCH, PCH_TGP, "I219-LM"}, {0x15FC, MODEL_PCH, PCH_TGP, "I219-V"}, {0x15F9, MODEL_PCH, PCH_TGP, "I219-LM"},
    {0x15FA, MODEL_PCH, PCH_TGP, "I219-V"}, {0x15F4, MODEL_PCH, PCH_TGP, "I219-LM"}, {0x15F5, MODEL_PCH, PCH_TGP, "I219-V"},
    {0x0DC5, MODEL_PCH, PCH_ADP, "I219-LM"}, {0x0DC6, MODEL_PCH, PCH_ADP, "I219-V"}, {0x1A1E, MODEL_PCH, PCH_ADP, "I219-LM"},
    {0x1A1F, MODEL_PCH, PCH_ADP, "I219-V"}, {0x1A1C, MODEL_PCH, PCH_ADP, "I219-LM"}, {0x1A1D, MODEL_PCH, PCH_ADP, "I219-V"},
    {0x0DC7, MODEL_PCH, PCH_ADP, "I219-LM"}, {0x0DC8, MODEL_PCH, PCH_ADP, "I219-V"}, {0x550C, MODEL_PCH, PCH_ADP, "I219-LM"},
    {0x550D, MODEL_PCH, PCH_ADP, "I219-V"},
    {0x550A, MODEL_PCH, PCH_MTP, "I219-LM"}, {0x550B, MODEL_PCH, PCH_MTP, "I219-V"}, {0x550E, MODEL_PCH, PCH_MTP, "I219-LM"},
    {0x550F, MODEL_PCH, PCH_MTP, "I219-V"}, {0x5510, MODEL_PCH, PCH_MTP, "I219-LM"}, {0x5511, MODEL_PCH, PCH_MTP, "I219-V"},
    {0x57A0, MODEL_PCH, PCH_MTP, "I219-LM"}, {0x57A1, MODEL_PCH, PCH_MTP, "I219-V"}, {0x57B3, MODEL_PCH, PCH_MTP, "I219-LM"},
    {0x57B4, MODEL_PCH, PCH_MTP, "I219-V"}, {0x57B5, MODEL_PCH, PCH_MTP, "I219-LM"}, {0x57B6, MODEL_PCH, PCH_MTP, "I219-V"},
    {0x57B7, MODEL_PCH, PCH_MTP, "I219-LM"}, {0x57B8, MODEL_PCH, PCH_MTP, "I219-V"}, {0x57B9, MODEL_PCH, PCH_MTP, "I219-LM"},
    {0x57BA, MODEL_PCH, PCH_MTP, "I219-V"},
    /* I225 / I226 / I221 (igc) */
    {0x15F2, MODEL_I225, 0, "I225-LM"}, {0x15F3, MODEL_I225, 0, "I225-V"}, {0x15F8, MODEL_I225, 0, "I225-I"}, {0x15F7, MODEL_I225, 0, "I220-V"},
    {0x3100, MODEL_I225, 0, "I225-K"}, {0x3101, MODEL_I225, 0, "I225-K2"}, {0x3102, MODEL_I225, 0, "I226-K"}, {0x5502, MODEL_I225, 0, "I225-LMvP"},
    {0x5503, MODEL_I225, 0, "I226-LMvP"}, {0x0D9F, MODEL_I225, 0, "I225-IT"}, {0x125B, MODEL_I225, 0, "I226-LM"}, {0x125C, MODEL_I225, 0, "I226-V"},
    {0x125D, MODEL_I225, 0, "I226-IT"}, {0x125E, MODEL_I225, 0, "I221-V"}, {0x125F, MODEL_I225, 0, "I226 (blank NVM)"}, {0x15FD, MODEL_I225, 0, "I225 (blank NVM)"},
};
static struct {
    net_device_t device;
    pci_device_t* pci;
    volatile void* registers;
    const char* name;
    uint64_t dma, dma_physical;
    struct descriptor *rx, *tx;
    enum model model;
    unsigned pch, vector, rx_next, tx_next, tx_dirty, rx_base, tx_base, reg_icr, reg_ims, reg_imc;
    uint64_t tx_time[TX_COUNT], next_watchdog;
    uint32_t bars[6]; uint16_t command;
    struct nic_link link;
    bool active, discarding, suspended, dead, me_valid;
    volatile bool rx_pending, link_event;
    unsigned resets, phy_errors;
} nic;

#ifndef E1000_TEST_MMIO
static uint32_t rd(unsigned reg) { return mmio_read32(nic.registers, reg); }
static void wr(unsigned reg, uint32_t value) { mmio_write32(nic.registers, reg, value); }
#endif
static bool advanced(void) { return nic.model == MODEL_82576 || nic.model == MODEL_I225; }
static bool pch(void) { return nic.model == MODEL_PCH; }
static bool modern(void) { return nic.model == MODEL_PCH || nic.model == MODEL_I225; }
static void flush(void) { (void)rd(STATUS); }

static bool wait_clear(unsigned reg, uint32_t bits, uint64_t nanoseconds) {
    uint64_t deadline = native_clock_ns() + nanoseconds;
    while (rd(reg) & bits) { if (native_clock_ns() >= deadline) return false; cpu_pause(); }
    return true;
}
static bool wait_set(unsigned reg, uint32_t bits, uint64_t nanoseconds) {
    uint64_t deadline = native_clock_ns() + nanoseconds;
    while ((rd(reg) & bits) != bits) { if (native_clock_ns() >= deadline) return false; cpu_pause(); }
    return true;
}
/* PHY registers through MDIC: PHY address 1 on the 8254x models, 2 (page 0) or 1 (pages >= 768) on PCH, 0 on I225. */
static bool mdic(unsigned address, unsigned reg, bool write, uint16_t* value) {
    wr(MDIC, (write ? *value : 0) | (reg & 31) << 16 | address << 21 | (write ? 1U << 26 : 2U << 26));
    uint64_t deadline = native_clock_ns() + 10000000ULL;
    for (;;) {
        uint32_t value32 = rd(MDIC);
        if (value32 & (1U << 28)) { if (value32 & (1U << 30)) return false; if (!write) *value = (uint16_t)value32; return true; }
        if (native_clock_ns() >= deadline) return false;
        cpu_pause();
    }
}
static bool phy_access(unsigned reg, bool write, uint16_t* value) {
    if (!pch()) return mdic(nic.model == MODEL_I225 ? 0 : 1, reg, write, value);
    unsigned page = reg >> 5, number = reg & 31, address = page >= 768 ? 1 : 2;
    if (number > 15) { uint16_t select = (uint16_t)(page << 5); if (!mdic(address, 0x1F, true, &select)) return false; }
    return mdic(address, number, write, value);
}
static bool phy_read(unsigned reg, uint16_t* value) { if (phy_access(reg, false, value)) return true; nic.phy_errors++; return false; }
static bool phy_write(unsigned reg, uint16_t value) { if (phy_access(reg, true, &value)) return true; nic.phy_errors++; return false; }
static bool phy_modify(unsigned reg, uint16_t clear, uint16_t set) {
    uint16_t value; if (!phy_read(reg, &value)) return false;
    return phy_write(reg, (uint16_t)((value & ~clear) | set));
}

/* ---- semaphores: PCH software flag (shared with the management engine), I225 SWSM + SW_FW_SYNC ---- */
static bool pch_acquire(void) {
    if (!wait_clear(EXTCNF_CTRL, EXTCNF_SWFLAG, 1000000000ULL)) { vga_print("[64] Intel PCH LAN: software flag stuck\n"); return false; }
    wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) | EXTCNF_SWFLAG);
    if (wait_set(EXTCNF_CTRL, EXTCNF_SWFLAG, 1000000000ULL)) return true;
    wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
    vga_print("[64] Intel PCH LAN: firmware holds the PHY semaphore\n");
    return false;
}
static void pch_release(void) { uint32_t v = rd(EXTCNF_CTRL); if (v & EXTCNF_SWFLAG) wr(EXTCNF_CTRL, v & ~EXTCNF_SWFLAG); }
static bool i225_semaphore(void) {
    if (!wait_clear(SWSM, SWSM_SMBI, 50000000ULL)) {
        wr(SWSM, rd(SWSM) & ~(SWSM_SMBI | SWSM_SWESMBI));      /* clear a stale semaphore once, as Linux does */
        if (!wait_clear(SWSM, SWSM_SMBI, 50000000ULL)) return false;
    }
    for (unsigned i = 0; i < 100; i++) {
        wr(SWSM, rd(SWSM) | SWSM_SWESMBI);
        if (rd(SWSM) & SWSM_SWESMBI) return true;
        netdrv_udelay(500);
    }
    wr(SWSM, rd(SWSM) & ~(SWSM_SMBI | SWSM_SWESMBI));
    return false;
}
static void i225_semaphore_put(void) { wr(SWSM, rd(SWSM) & ~(SWSM_SMBI | SWSM_SWESMBI)); }
static bool i225_acquire(void) {
    for (unsigned i = 0; i < 200; i++) {
        if (!i225_semaphore()) break;
        uint32_t sync = rd(SW_FW_SYNC);
        if (!(sync & (SWFW_PHY0 | SWFW_PHY0 << 16))) { wr(SW_FW_SYNC, sync | SWFW_PHY0); i225_semaphore_put(); return true; }
        i225_semaphore_put(); netdrv_mdelay(5);
    }
    vga_print("[64] Intel I225: PHY semaphore timeout\n");
    return false;
}
static void i225_release(void) { if (!i225_semaphore()) return; wr(SW_FW_SYNC, rd(SW_FW_SYNC) & ~SWFW_PHY0); i225_semaphore_put(); }
static bool phy_lock(void) { return pch() ? pch_acquire() : nic.model == MODEL_I225 ? i225_acquire() : true; }
static void phy_unlock(void) { if (pch()) pch_release(); else if (nic.model == MODEL_I225) i225_release(); }

/* Link still down after reset (the 82576 model, some boards): restart PHY autonegotiation. The link comes
 * up later and raises LSC; transmit re-reads STATUS while the link is down. */
static void restart_autonegotiation(void) {
    uint16_t control;
    if (!phy_access(0, false, &control)) { vga_print("[64] Intel NIC PHY not responding\n"); return; }
    control |= (1U << 12) | (1U << 9); /* auto-negotiation enable + restart */
    if (!phy_access(0, true, &control)) vga_print("[64] Intel NIC PHY autonegotiation restart failed\n");
}
static bool eeprom_word(unsigned address, uint16_t* word) {
    bool old = nic.model == MODEL_82540;
    unsigned reg = nic.model == MODEL_I225 ? I225_EERD : EERD;
    uint32_t done = old ? 1U << 4 : 1U << 1;
    wr(reg, 1U | (address << (old ? 8 : 2)));
    uint64_t deadline = native_clock_ns() + 10000000ULL;
    for (;;) {
        uint32_t value = rd(reg);
        if (value & done) { *word = (uint16_t)(value >> 16); return true; }
        if (native_clock_ns() >= deadline) return false;
        cpu_pause();
    }
}
static bool mac_valid(const uint8_t* mac) { return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]); }
/* MAC address: NVM words 0-2 (8254x); receive address 0 that the NVM load filled (fallback, and the only
 * source on PCH and I225, as in Linux). */
static bool read_mac(void) {
    uint16_t words[3]; bool nvm = !modern();
    for (unsigned i = 0; i < 3 && nvm; i++) nvm = eeprom_word(i, &words[i]);
    uint8_t mac[ETH_ALEN];
    if (nvm) for (unsigned i = 0; i < 3; i++) { mac[2 * i] = (uint8_t)words[i]; mac[2 * i + 1] = (uint8_t)(words[i] >> 8); }
    if (!nvm || !mac_valid(mac)) {
        uint32_t low = rd(RAL), high = rd(RAH);
        if (!(high & (1U << 31))) return false;
        for (unsigned i = 0; i < 4; i++) mac[i] = (uint8_t)(low >> (8 * i));
        mac[4] = (uint8_t)high; mac[5] = (uint8_t)(high >> 8);
        if (!mac_valid(mac)) return false;
    }
    memcpy(nic.device.mac.addr, mac, ETH_ALEN);
    return true;
}

/* ---- PCH (I217/I218/I219) bring-up ---- */
static bool pch_reset_blocked(void) {   /* the management engine may forbid PHY resets (FWSM.RSPCIPHY clear) */
    for (unsigned i = 0; i < 30; i++) { if (rd(FWSM) & FWSM_RSPCIPHY) return false; netdrv_mdelay(10); }
    return true;
}
static bool pch_phy_accessible(void) {   /* PHY ID readable (software flag held) */
    for (unsigned i = 0; i < 2; i++) {
        uint16_t id1, id2;
        if (mdic(2, 2, false, &id1) && id1 != 0xFFFF && mdic(2, 3, false, &id2) && id2 != 0xFFFF && (id1 | id2)) {
            if (!nic.me_valid) {                              /* only unforce SMBus without an active ME */
                uint16_t smb; if (phy_access(HV_CV_SMB_CTRL, false, &smb)) { smb &= ~1U; phy_access(HV_CV_SMB_CTRL, true, &smb); }
                wr(CTRL_EXT, rd(CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
            }
            return true;
        }
    }
    return false;
}
static void pch_toggle_lanphypc(void) {   /* fully power-cycles the PHY */
    wr(FEXTNVM3, (rd(FEXTNVM3) & ~0x0C000000U) | 0x08000000U);   /* PHY config counter 50 ms */
    uint32_t ctrl = rd(CTRL);
    wr(CTRL, (ctrl | CTRL_LANPHYPC_OVERRIDE) & ~CTRL_LANPHYPC_VALUE); flush();
    netdrv_udelay(20);
    wr(CTRL, ctrl & ~(CTRL_LANPHYPC_OVERRIDE | CTRL_LANPHYPC_VALUE)); flush();
    for (unsigned i = 0; i < 20 && !(rd(CTRL_EXT) & CTRL_EXT_LPCD); i++) netdrv_mdelay(5);
    netdrv_mdelay(30);
}
/* Ultra-low-power mode may be left on by the firmware (S0ix, cable unplugged at boot): leave it. */
static bool pch_ulp_disable(void) {
    if (nic.me_valid) {
        wr(H2ME, (rd(H2ME) & ~H2ME_ULP) | H2ME_ENFORCE);
        bool done = false;
        for (unsigned i = 0; i < 250 && !done; i++) { if (!(rd(FWSM) & FWSM_ULP_CFG_DONE)) done = true; else netdrv_mdelay(10); }
        wr(H2ME, rd(H2ME) & ~H2ME_ENFORCE);
        if (!done) vga_print("[64] Intel PCH LAN: management engine did not leave ULP\n");
        return done;
    }
    if (!pch_acquire()) return false;
    pch_toggle_lanphypc();
    uint16_t value; bool ok = true;
    if (!phy_access(HV_CV_SMB_CTRL, false, &value)) {        /* MAC in PCIe mode: reach the PHY through SMBus */
        wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_FORCE_SMBUS); netdrv_mdelay(50);
        ok = phy_access(HV_CV_SMB_CTRL, false, &value);
    }
    if (ok) {
        value &= ~1U; phy_access(HV_CV_SMB_CTRL, true, &value);
        wr(CTRL_EXT, rd(CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
        if (phy_access(HV_PM_CTRL, false, &value)) { value |= 0x4000; phy_access(HV_PM_CTRL, true, &value); }   /* K1 back on */
        if (phy_access(HV_ULP_CONFIG1, false, &value)) {
            value &= (uint16_t)~(0x0004 | 0x0010 | 0x0100 | 0x0040 | 0x0020 | 0x0400 | 0x0800 | 0x1000);
            phy_access(HV_ULP_CONFIG1, true, &value);
            value |= 0x0001; phy_access(HV_ULP_CONFIG1, true, &value);   /* commit: start auto ULP configuration */
        }
        wr(FEXTNVM7, rd(FEXTNVM7) & ~(1U << 5));             /* SMBus release on PERST# allowed again */
    }
    pch_release();
    return ok;
}
static void pch_lan_init_done(void) {
    for (unsigned i = 0; i < 1500 && !(rd(STATUS) & STATUS_LAN_INIT_DONE); i++) netdrv_udelay(100);
    wr(STATUS, rd(STATUS) & ~STATUS_LAN_INIT_DONE);
    uint32_t status = rd(STATUS); if (status & STATUS_PHYRA) wr(STATUS, status & ~STATUS_PHYRA);
}
static void pch_post_phy_reset(void) {
    if (pch_reset_blocked()) return;
    netdrv_mdelay(10);
    if (pch_acquire()) { uint16_t v; if (phy_access(HV_PORT_GEN_CFG, false, &v)) { v &= ~0x10U; phy_access(HV_PORT_GEN_CFG, true, &v); } pch_release(); }
}
static void pch_phy_reset(void) {
    if (pch_reset_blocked() || !pch_acquire()) return;
    uint32_t ctrl = rd(CTRL);
    wr(CTRL, ctrl | CTRL_PHY_RST); flush(); netdrv_udelay(100);
    wr(CTRL, ctrl); flush(); netdrv_udelay(150);
    pch_release();
    pch_lan_init_done(); pch_post_phy_reset();
}
/* Gate automatic PHY configuration, leave ULP, make the PHY reachable (SMBus / LANPHYPC), reset it. */
static bool pch_phy_init(void) {
    nic.me_valid = (rd(FWSM) & FWSM_FW_VALID) != 0;
    wr(EXTCNF_CTRL, rd(EXTCNF_CTRL) | EXTCNF_GATE_PHY_CFG);
    if (!pch_ulp_disable()) vga_print("[64] Intel PCH LAN: ULP disable failed, continuing\n");
    if (!nic.me_valid) { pch_phy_reset(); netdrv_mdelay(50); }
    if (!pch_acquire()) return false;
    bool ok = pch_phy_accessible();
    if (!ok) { wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_FORCE_SMBUS); netdrv_mdelay(50); ok = pch_phy_accessible(); }
    if (!ok && !pch_reset_blocked()) {
        pch_toggle_lanphypc(); ok = pch_phy_accessible();
        if (!ok) { wr(CTRL_EXT, rd(CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS); ok = pch_phy_accessible(); }
    }
    pch_release();
    if (!ok) { vga_print("[64] Intel PCH LAN: PHY not accessible (SMBus/ULP), giving up\n"); return false; }
    if (!pch_reset_blocked()) pch_phy_reset();
    else vga_print("[64] Intel PCH LAN: PHY reset blocked by the management engine\n");
    return true;
}
/* I219 erratum: descriptor rings must be empty before a reset or D3, else the unit hangs until a PCI reset. */
static void pch_flush_rings(void) {
    if (nic.pch < PCH_SPT) return;
    wr(FEXTNVM11, rd(FEXTNVM11) | (1U << 13));                /* disable the MULR fix */
    uint16_t state = pci_read16(nic.pci->bus, nic.pci->device, nic.pci->function, 0xE4);
    if (!(state & 0x100) || !rd(TDLEN0) || !nic.tx) return;
    uint32_t tctl = rd(TCTL); wr(TCTL, tctl | TCTL_EN);
    unsigned slot = rd(nic.tx_base + Q_TAIL) % TX_COUNT;      /* one dummy 512-byte descriptor pointing at the ring */
    struct descriptor* d = &nic.tx[slot];
    d->address = nic.dma_physical + TX_RING_OFFSET; d->word2 = 512 | (1U << 25); d->word3 = 0;
    __asm__ volatile("sfence" ::: "memory");
    wr(nic.tx_base + Q_TAIL, (slot + 1) % TX_COUNT); netdrv_udelay(250);
    state = pci_read16(nic.pci->bus, nic.pci->device, nic.pci->function, 0xE4);
    if (state & 0x100) {
        uint32_t rctl = rd(RCTL); wr(RCTL, rctl & ~RCTL_EN); flush(); netdrv_udelay(150);
        wr(RXDCTL0, (rd(RXDCTL0) & 0xFFFFC000U) | 0x1F | (1U << 8) | (1U << 24));
        wr(RCTL, rctl | RCTL_EN); flush(); netdrv_udelay(150); wr(RCTL, rctl & ~RCTL_EN);
    }
}
static void master_disable(void) {
    wr(CTRL, rd(CTRL) | CTRL_GIO_MASTER_DISABLE);
    if (!wait_clear(STATUS, STATUS_GIO_MASTER_ENABLE, 80000000ULL)) vga_print("[64] Intel NIC: PCIe master disable timed out\n");
}
/* MAC+PHY reset (Linux e1000_reset_hw_ich8lan) and the required bits (e1000_initialize_hw_bits_ich8lan). */
static bool pch_reset(void) {
    pch_flush_rings();
    master_disable();
    wr(IMC, 0xFFFFFFFFU); wr(RCTL, 0); wr(TCTL, TCTL_PSP); flush();
    netdrv_mdelay(10);
    uint32_t ctrl = rd(CTRL);
    bool phy_reset = !pch_reset_blocked();
    if (phy_reset) ctrl |= CTRL_PHY_RST;
    bool flag = pch_acquire();
    wr(CTRL, ctrl | CTRL_RST);                               /* the reset also releases the software flag */
    netdrv_mdelay(20);
    if (!wait_clear(CTRL, CTRL_RST, 100000000ULL)) { if (flag) pch_release(); return false; }
    if (phy_reset) { pch_lan_init_done(); pch_post_phy_reset(); }
    wr(IMC, 0xFFFFFFFFU); (void)rd(ICR);
    wr(KABGTXD, rd(KABGTXD) | 0x00050000U);
    wr(CTRL_EXT, rd(CTRL_EXT) | (1U << 22) | CTRL_EXT_PHYPDEN);
    wr(TXDCTL0, rd(TXDCTL0) | (1U << 22)); wr(TXDCTL1, rd(TXDCTL1) | (1U << 22));
    wr(TARC0, rd(TARC0) | (1U << 23) | (1U << 24) | (1U << 26) | (1U << 27));
    uint32_t tarc1 = rd(TARC1) | (1U << 24) | (1U << 26) | (1U << 30);
    wr(TARC1, rd(TCTL) & TCTL_MULR ? tarc1 & ~(1U << 28) : tarc1 | (1U << 28));
    wr(RFCTL, rd(RFCTL) | (1U << 6) | (1U << 7));            /* NFS filtering off (descriptor corruption erratum) */
    wr(PBECCSTS, rd(PBECCSTS) | (1U << 16)); wr(CTRL, rd(CTRL) | CTRL_MEHE);   /* packet buffer ECC (Lynx Point+) */
    uint32_t txdctl = rd(TXDCTL0);                           /* descriptor write-back: GRAN=1, WTHRESH=1, PTHRESH=31 */
    txdctl = (txdctl & ~0x003F0000U) | 0x01010000U; txdctl = (txdctl & ~0x3FU) | 0x0100001FU; wr(TXDCTL0, txdctl);
    if (nic.pch >= PCH_TGP) wr(FFLT_DBG, rd(FFLT_DBG) | 0x1000);   /* do not gate the DMA clock (TGP packet loss) */
    wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_RO_DIS);
    return true;
}
/* Link-up tuning (Linux e1000_check_for_copper_link_ich8lan) once speed and duplex are known. */
static void pch_link_tuning(const struct nic_link* link) {
    wr(FEXTNVM4, (rd(FEXTNVM4) & ~7U) | 7U);                 /* beacon duration 8 us (I217 packet loss) */
    if (nic.pch == PCH_SPT) {                                /* K1-off follows the PCIe analog configuration */
        uint32_t fextnvm6 = rd(FEXTNVM6);
        wr(FEXTNVM6, rd(PCIEANACFG) & 0x80000000U ? fextnvm6 | 0x80000000U : fextnvm6 & ~0x80000000U);
    }
    if (!link->up) return;
    uint32_t tipg = rd(TIPG) & ~0x3FFU; uint16_t emi;
    if (link->duplex == NIC_DUPLEX_HALF && link->speed == 10) { tipg |= 0xFF; emi = 0; }
    else if (nic.pch >= PCH_SPT && link->duplex == NIC_DUPLEX_FULL && link->speed != 1000) { tipg |= 0x0C; emi = 1; }
    else { tipg |= 0x08; emi = 1; }
    wr(TIPG, tipg);
    if (!pch_acquire()) return;
    uint16_t address = I217_RX_CONFIG; phy_write(EMI_ADDR, address); phy_write(EMI_DATA, emi);
    phy_modify(HV_PLL_CLOCK_GATE, 0x07FF, link->speed == 1000 ? 0x00FA : 0x03E8);
    if (link->speed == 1000) phy_modify(HV_PM_CTRL, 0, 0x0200);
    if (nic.pch >= PCH_SPT) {
        if (link->speed == 1000) { uint16_t data; if (phy_read(HV_PTR_GAP, &data) && ((data >> 2) & 0x3FF) < 0x18) phy_write(HV_PTR_GAP, (uint16_t)((data & ~(0x3FFU << 2)) | 0x18U << 2)); }
        else phy_write(HV_PTR_GAP, 0xC023);
    }
    pch_release();
}

/* ---- I225/I226 bring-up ---- */
static bool i225_reset(void) {
    master_disable();
    wr(I225_IMC, 0xFFFFFFFFU); wr(RCTL, 0); wr(TCTL, TCTL_PSP); flush();
    netdrv_mdelay(10);
    wr(CTRL, rd(CTRL) | CTRL_RST);
    netdrv_mdelay(1);
    if (!wait_clear(CTRL, CTRL_RST, 100000000ULL)) return false;
    if (!wait_set(EECD, EECD_AUTO_RD, 10000000ULL)) vga_print("[64] Intel I225: NVM auto-read did not complete\n");
    wr(I225_IMC, 0xFFFFFFFFU); (void)rd(I225_ICR);
    if (!(rd(MANC) & MANC_BLK_PHY_RST) && i225_acquire()) {  /* PHY reset with completion (PHPM) */
        uint32_t ctrl = rd(CTRL);
        wr(CTRL, ctrl | CTRL_PHY_RST); flush(); netdrv_udelay(100);
        wr(CTRL, ctrl); flush(); netdrv_udelay(150);
        if (!wait_set(I225_PHPM, PHPM_RST_COMPLETE, 10000000ULL)) vga_print("[64] Intel I225: PHY reset did not complete\n");
        netdrv_udelay(150);
        i225_release();
    }
    return true;
}
/* MMD register through the Clause-22 indirect access (registers 13/14). */
static bool i225_mmd(unsigned device, unsigned address, bool write, uint16_t* value) {
    uint16_t control = (uint16_t)device, location = (uint16_t)address, data = (uint16_t)(0x4000 | device), zero = 0;
    return mdic(0, 13, true, &control) && mdic(0, 14, true, &location) && mdic(0, 13, true, &data) &&
           mdic(0, 14, write, value) && mdic(0, 13, true, &zero);
}
/* Advertise 10/100 half+full, 1000 full (and 2.5G on I225), symmetric+asymmetric pause; restart autonegotiation. */
static bool phy_autonegotiate(void) {
    if (!phy_lock()) return false;
    bool ok = phy_modify(MII_ADVERTISE, 0x0DE0, 0x0DE0) && phy_modify(MII_CTRL1000, 0x0300, 0x0200);
    if (ok && nic.model == MODEL_I225) {
        uint16_t an; ok = i225_mmd(7, 0x20, false, &an);
        if (ok) { an |= 0x0080; ok = i225_mmd(7, 0x20, true, &an); }
    }
    ok = ok && phy_modify(MII_BMCR, 0x0800, (1U << 12) | (1U << 9));   /* power up, AN enable + restart */
    phy_unlock();
    return ok;
}
static void link_read(struct nic_link* link) {
    uint32_t status = rd(STATUS);
    memset(link, 0, sizeof(*link));
    if (status == 0xFFFFFFFFU || !(status & STATUS_LU)) return;
    link->up = true;
    link->speed = status & STATUS_SPEED_1000 ? (nic.model == MODEL_I225 && (status & STATUS_SPEED_2500) ? 2500 : 1000) : status & STATUS_SPEED_100 ? 100 : 10;
    link->duplex = status & STATUS_FD ? NIC_DUPLEX_FULL : NIC_DUPLEX_HALF;
}
static void link_update(void) {
    struct nic_link now; link_read(&now);
    if (now.up == nic.link.up && now.speed == nic.link.speed && now.duplex == nic.link.duplex) return;
    if (pch()) pch_link_tuning(&now);
    nic.link = now;
    netdrv_link_report(&nic.device, &now);
}

static void rx_arm(unsigned index) {
    struct descriptor* d = &nic.rx[index];
    d->address = nic.dma_physical + RX_BUFFER_OFFSET + (uint64_t)index * BUFFER;
    d->word2 = 0; d->word3 = 0; /* legacy: length/status; advanced: header address (0) */
}
static void tx_reclaim(void) {
    while (nic.tx_dirty != nic.tx_next && (nic.tx[nic.tx_dirty].word3 & 1)) nic.tx_dirty = (nic.tx_dirty + 1) % TX_COUNT;
}
/* Never waits: a slot whose previous frame the NIC has not finished drops the frame; TCP/ARP retransmit. */
static int transmit(net_device_t* device, const void* packet, uint16_t length) {
    (void)device;
    if (!nic.active || nic.suspended || nic.dead) return -100;
    if (!nic.device.link_up && !(nic.device.link_up = (rd(STATUS) & STATUS_LU) != 0)) return -100;
    if (!packet || !length) return -22;
    if (length > ETH_FRAME_MAX) return -90;
    unsigned slot = nic.tx_next;
    /* One slot stays free: TDT == TDH means an empty ring to the hardware. Completed slots (DD, status byte bit 0
     * in both formats) are reclaimed only when the ring looks full. */
    if ((slot + 1) % TX_COUNT == nic.tx_dirty) { tx_reclaim(); if ((slot + 1) % TX_COUNT == nic.tx_dirty) return -11; }
    struct descriptor* d = &nic.tx[slot];
    uint64_t buffer = nic.dma + TX_BUFFER_OFFSET + (uint64_t)slot * BUFFER;
    memcpy((void*)buffer, packet, length);
    if (length < 60) { memset((void*)(buffer + length), 0, 60 - length); length = 60; }
    d->address = nic.dma_physical + TX_BUFFER_OFFSET + (uint64_t)slot * BUFFER;
    if (advanced()) { d->word2 = length | (3U << 20) | (1U << 24) | (1U << 25) | (1U << 27) | (1U << 29); d->word3 = (uint32_t)length << 14; }
    else { d->word2 = length | (1U << 24) | (1U << 25) | (1U << 27); d->word3 = 0; } /* EOP | IFCS | RS */
    nic.tx_time[slot] = native_clock_ns();
    nic.tx_next = (slot + 1) % TX_COUNT;
    __asm__ volatile("sfence" ::: "memory");
    wr(nic.tx_base + Q_TAIL, nic.tx_next);
    nic.device.stats.tx_packets++; nic.device.stats.tx_bytes += length;
    return 0;
}
static void receive(void) {
    unsigned budget = RX_BATCH_FRAMES;
    for (;;) {
        unsigned last = RX_COUNT;
        while (budget) {
            struct descriptor* d = &nic.rx[nic.rx_next];
            uint32_t status, length;
            if (advanced()) { status = d->word2; length = d->word3 & 0xFFFF; }
            else { status = (d->word3 & 0xFF) | ((d->word3 >> 8 & 0xFF) << 24); length = d->word2 & 0xFFFF; }
            if (!(status & 1)) break;
            __asm__ volatile("lfence" ::: "memory");
            bool end = (status & 2) != 0, errors = (status & 0x87000000U) != 0; /* CE | SE | SEQ | RXE */
            if (nic.discarding) { if (end) nic.discarding = false; nic.device.stats.rx_errors++; }
            else if (!end) { nic.discarding = true; nic.device.stats.rx_errors++; }
            else if (errors || length < ETH_HEADER_LEN || length > ETH_FRAME_MAX + 4) nic.device.stats.rx_errors++;
            else {
                nic.device.stats.rx_packets++; nic.device.stats.rx_bytes += length;
                net_deliver_packet((const void*)(nic.dma + RX_BUFFER_OFFSET + (uint64_t)nic.rx_next * BUFFER), (uint16_t)length);
            }
            rx_arm(nic.rx_next);
            last = nic.rx_next; nic.rx_next = (nic.rx_next + 1) % RX_COUNT; budget--;
        }
        if (last == RX_COUNT) return;
        __asm__ volatile("sfence" ::: "memory");
        wr(nic.rx_base + Q_TAIL, last); /* hand the drained descriptors back; QEMU may deliver queued frames now */
        if (!budget) { nic.rx_pending = true; return; }
    }
}
/* Also looks at the next descriptor in memory (no register access), so receive works without an IRQ line. */
bool e1000_nic_rx_pending(void) {
    if (!nic.active || nic.suspended) return false;
    struct descriptor* d = &nic.rx[nic.rx_next];
    return nic.rx_pending || nic.link_event || ((advanced() ? d->word2 : d->word3) & 1) || native_clock_ns() >= nic.next_watchdog;
}
static bool hw_init(void);
static void watchdog(uint64_t now) {
    nic.next_watchdog = now + WATCHDOG_NS;
    if (rd(STATUS) == 0xFFFFFFFFU) {
        if (!nic.dead) { nic.dead = true; vga_print("[64] Intel NIC: device not responding (reads all-ones)\n");
                         struct nic_link down = {false, 0, 0}; nic.link = down; netdrv_link_report(&nic.device, &down); }
        return;
    }
    if (nic.dead) {
        nic.dead = false; nic.resets++; vga_print("[64] Intel NIC: device answers again, reinitialising\n");
        pci_write32(nic.pci->bus, nic.pci->device, nic.pci->function, PCI_COMMAND, (nic.command | 6U) & ~0x400U);
        if (nic.vector >= 0x40) pci_enable_msi(nic.pci, nic.vector, 0);
        hw_init(); return;
    }
    if (!nic.vector) nic.link_event = true;
    tx_reclaim();
    if (nic.tx_dirty != nic.tx_next && nic.link.up && now - nic.tx_time[nic.tx_dirty] > TX_TIMEOUT_NS) {
        nic.resets++; nic.device.stats.tx_errors++;
        vga_print("[64] Intel NIC: transmit timeout, resetting the controller\n");
        hw_init();
    }
}
void e1000_nic_poll(void) {
    if (!nic.active || nic.suspended) return;
    nic.rx_pending = false;
    uint64_t now = native_clock_ns();
    if (now >= nic.next_watchdog) watchdog(now);
    if (nic.dead) return;
    receive();
    if (nic.link_event) { nic.link_event = false; link_update(); }
}
/* IRQ context: ICR is read-to-clear (deasserts the level-triggered line); only flag work here. */
void e1000_nic_irq(unsigned vector) {
    if (!nic.active || nic.suspended || vector != nic.vector) return;
    uint32_t cause = rd(nic.reg_icr);
    if (!cause || cause == 0xFFFFFFFFU) return;
    if (cause & INT_LSC) { nic.device.link_up = (rd(STATUS) & STATUS_LU) != 0; nic.link_event = true; }
    if (cause & INT_RXO) nic.device.stats.rx_dropped++;
    if (cause & RX_IRQ_BITS) nic.rx_pending = true;
}
static bool enable_queue(unsigned reg) {
    wr(reg, rd(reg) | QUEUE_ENABLE);
    uint64_t deadline = native_clock_ns() + 10000000ULL;
    while (!(rd(reg) & QUEUE_ENABLE)) { if (native_clock_ns() >= deadline) return false; cpu_pause(); }
    return true;
}
/* Receive filter (netdev.c): RCTL.UPE/MPE for promiscuous / all-multicast, otherwise the 4096-bit multicast table
 * (MO = 00: hash = destination bits 47:36) holds the joined groups; netdev.c filters exactly in software. The filter
 * is kept here and programmed again by hw_init() (probe, watchdog/timeout reset, resume). */
static uint32_t rx_filter_table[128], rx_filter_rctl;
void e1000_nic_rx_mode(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count) {
    memset(rx_filter_table, 0, sizeof(rx_filter_table));
    for (unsigned i = 0; i < count; i++) {
        const uint8_t* mac = groups + 6 * i;
        unsigned hash = ((unsigned)mac[4] >> 4 | (unsigned)mac[5] << 4) & 0xFFF;
        rx_filter_table[hash >> 5] |= 1U << (hash & 31);
    }
    rx_filter_rctl = promiscuous ? (1U << 3 | 1U << 4) : all_multicast ? 1U << 4 : 0;
    if (!nic.active || nic.suspended || nic.dead) return;
    for (unsigned i = 0; i < 128; i++) wr(MTA + i * 4, rx_filter_table[i]);
    wr(RCTL, (rd(RCTL) & ~((1U << 3) | (1U << 4))) | rx_filter_rctl);
}
/* Reset and program the controller around the existing DMA area (probe, timeout recovery, resume). */
static bool hw_init(void) {
    if (pch()) { if (!pch_reset()) return false; }
    else if (nic.model == MODEL_I225) { if (!i225_reset()) return false; }
    else {
        wr(IMC, 0xFFFFFFFFU);
        wr(CTRL, rd(CTRL) | CTRL_RST);
        for (volatile unsigned i = 0; i < 1000; i++) cpu_pause(); /* the reset bit is not readable for ~1 us after the write */
        if (!wait_clear(CTRL, CTRL_RST, 100000000ULL)) return false;
        wr(IMC, 0xFFFFFFFFU); (void)rd(ICR);
        if (nic.model == MODEL_82576) wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    }
    if (modern()) wr(CTRL_EXT, rd(CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    wr(CTRL, (rd(CTRL) | CTRL_SLU | CTRL_ASDE) & ~((1U << 3) | (1U << 7) | (1U << 30) | (1U << 31) | (modern() ? CTRL_FRCSPD | CTRL_FRCDPX : 0)));
    const uint8_t* mac = nic.device.mac.addr;
    wr(RAL, mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    wr(RAH, mac[4] | (uint32_t)mac[5] << 8 | (1U << 31));
    for (unsigned i = 0; i < 128; i++) wr(MTA + i * 4, rx_filter_table[i]);   /* netdev.c filter (zero at probe) */
    if (modern()) { if (!phy_autonegotiate()) vga_print("[64] Intel NIC: PHY autonegotiation setup failed\n"); }
    else if (!(rd(STATUS) & STATUS_LU)) restart_autonegotiation();
    for (unsigned i = 0; i < RX_COUNT; i++) rx_arm(i);
    for (unsigned i = 0; i < TX_COUNT; i++) { nic.tx[i].address = 0; nic.tx[i].word2 = 0; nic.tx[i].word3 = 0; }
    nic.rx_next = nic.tx_next = nic.tx_dirty = 0; nic.discarding = false;
    uint64_t rx = nic.dma_physical + RX_RING_OFFSET, tx = nic.dma_physical + TX_RING_OFFSET;
    if (advanced()) { wr(nic.rx_base + Q_DCTL, 0); wr(nic.tx_base + Q_DCTL, 0); }
    wr(nic.rx_base + Q_BAL, (uint32_t)rx); wr(nic.rx_base + Q_BAH, (uint32_t)(rx >> 32));
    wr(nic.rx_base + Q_LEN, RX_COUNT * 16); wr(nic.rx_base + Q_HEAD, 0);
    wr(nic.tx_base + Q_BAL, (uint32_t)tx); wr(nic.tx_base + Q_BAH, (uint32_t)(tx >> 32));
    wr(nic.tx_base + Q_LEN, TX_COUNT * 16); wr(nic.tx_base + Q_HEAD, 0); wr(nic.tx_base + Q_TAIL, 0);
    if (advanced()) {
        wr(nic.rx_base + Q_SRRCTL, (BUFFER / 1024) | (1U << 25)); /* 2 KiB buffers, advanced one-buffer descriptors */
        if (!enable_queue(nic.rx_base + Q_DCTL) || !enable_queue(nic.tx_base + Q_DCTL)) return false;
    } else { wr(RDTR, 0); wr(RADV, 0); wr(TIPG, 8U | (8U << 10) | (6U << 20)); }
    wr(nic.rx_base + Q_TAIL, RX_COUNT - 1);
    wr(RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | rx_filter_rctl); /* 2 KiB buffers (BSIZE 0), broadcast, strip CRC, no bad frames */
    wr(TCTL, TCTL_EN | TCTL_PSP | (0x0FU << 4) | (0x40U << 12) | (1U << 24));
    /* Interrupt mitigation: the 8254x models keep ITR 0 (QEMU paces them); PCH at most ~20000/s; I225 EITR 50 us. */
    if (nic.model == MODEL_I225) { wr(I225_EITR0, 50U << 2); wr(I225_IAM, 0); }   /* no auto-mask on ICR read */
    else wr(ITR, pch() ? 195 : 0);
    (void)rd(nic.reg_icr);
    wr(nic.reg_ims, INT_LSC | INT_RXSEQ | RX_IRQ_BITS);
    nic.link_event = true; nic.rx_pending = true;
    return true;
}
static bool start(pci_device_t* device, const char* name) {
    nic.pci = device; nic.name = name;
    uint64_t size, base = pci_bar_memory(device, 0, &size);
    if (!base || size < 0x10000) { if (modern()) vga_print("[64] Intel NIC: BAR0 not assigned by the firmware\n"); return false; }
#ifndef E1000_TEST_MMIO
    nic.registers = pci_mmio_map(base, 0x20000 < size ? 0x20000 : size);
    if (!nic.registers) return false;
#endif
    for (unsigned i = 0; i < 6; i++) nic.bars[i] = pci_read32(device->bus, device->device, device->function, (uint8_t)(PCI_BAR0 + 4 * i));
    nic.command = pci_enable_dma(device);
    nic.rx_base = advanced() ? 0xC000 : 0x2800; nic.tx_base = advanced() ? 0xE000 : 0x3800;
    nic.reg_icr = nic.model == MODEL_I225 ? I225_ICR : ICR; nic.reg_ims = nic.model == MODEL_I225 ? I225_IMS : IMS;
    nic.reg_imc = nic.model == MODEL_I225 ? I225_IMC : IMC;
    nic.tx = NULL;
    if (rd(STATUS) == 0xFFFFFFFFU) { vga_print("[64] Intel NIC: register reads fail (all-ones)\n"); goto failed; }
    wr(nic.reg_imc, 0xFFFFFFFFU);
    if (pch() && !pch_phy_init()) goto failed;
    if (modern()) {                                           /* reset first: receive address 0 is reloaded from the NVM */
        if (pch() ? !pch_reset() : !i225_reset()) { vga_print("[64] Intel NIC: controller reset timed out\n"); goto failed; }
    } else {
        wr(CTRL, rd(CTRL) | CTRL_RST);
        for (volatile unsigned i = 0; i < 1000; i++) cpu_pause();
        if (!wait_clear(CTRL, CTRL_RST, 100000000ULL)) goto failed;
    }
    if (!read_mac()) { vga_print("[64] Intel NIC: no valid MAC address in the NVM or RAR0\n"); goto failed; }
    nic.dma = modern() ? arena_alloc(DMA_ALLOCATION) : dma32_alloc(DMA_ALLOCATION);   /* PCH/I225: 64-bit DMA */
    if (!nic.dma) goto failed;
    nic.dma_physical = virt_to_phys((void*)nic.dma);
    nic.rx = (struct descriptor*)(nic.dma + RX_RING_OFFSET); nic.tx = (struct descriptor*)(nic.dma + TX_RING_OFFSET);
    if (!hw_init()) goto failed;
    nic.device.name = "eth0"; nic.device.send = transmit;
    nic.device.link_up = (rd(STATUS) & STATUS_LU) != 0;
    memset(&nic.link, 0, sizeof(nic.link));
    if (net_register_device(&nic.device) < 0) goto failed;
    nic.active = true;
    nic.vector = pci_intx_route(device);
    (void)rd(nic.reg_icr);
    wr(nic.reg_ims, INT_LSC | INT_RXSEQ | RX_IRQ_BITS);
    nic.next_watchdog = native_clock_ns() + WATCHDOG_NS;
    if (!nic.vector) vga_print("[64] Intel NIC has no legacy IRQ line; receive is polled\n");
    vga_print("[64] Intel "); vga_print(name); vga_print(nic.device.link_up ? " native DMA/IRQ ready, link up\n" : " native DMA/IRQ ready, link down\n");
    return true;
failed:
    wr(nic.reg_imc, 0xFFFFFFFFU); wr(RCTL, 0); wr(TCTL, 0);
    pci_restore_command(device, nic.command & ~4U);
    if (nic.dma) arena_free(nic.dma, DMA_ALLOCATION);
    nic.dma = 0; nic.registers = NULL; nic.rx = nic.tx = NULL;
    vga_print("[64] Intel "); vga_print(name); vga_print(" initialization failed\n");
    return false;
}
bool e1000_nic_init(void) {
    pci_device_t* devices = pci_get_devices(); int count = pci_device_count();
    for (int i = 0; i < count; i++) {
        if (devices[i].vendor_id != 0x8086 || devices[i].class_code != PCI_CLASS_NETWORK) continue;
        for (unsigned j = 0; j < sizeof(supported) / sizeof(*supported); j++) if (supported[j].device == devices[i].device_id) {
            nic.model = (enum model)supported[j].model; nic.pch = supported[j].pch;
            netdrv_booting = true;
            bool ok = start(&devices[i], supported[j].name);
            netdrv_booting = false;
            if (ok) return true;
        }
    }
    return false;
}
bool e1000_nic_link(struct nic_link* link) { if (!nic.active) return false; *link = nic.link; return true; }
const pci_device_t* e1000_nic_pci(void) { return nic.active ? nic.pci : NULL; }
/* System sleep: stop receive/transmit and interrupts (I219: empty the rings first), no DMA while asleep. */
void e1000_nic_suspend(void) {
    if (!nic.active || nic.suspended) return;
    nic.suspended = true;
    if (!nic.dead) {
        wr(nic.reg_imc, 0xFFFFFFFFU);
        if (pch()) pch_flush_rings();
        wr(RCTL, rd(RCTL) & ~RCTL_EN); wr(TCTL, rd(TCTL) & ~TCTL_EN); flush();
    }
    pci_write32(nic.pci->bus, nic.pci->device, nic.pci->function, PCI_COMMAND, pci_read16(nic.pci->bus, nic.pci->device, nic.pci->function, PCI_COMMAND) & ~4U);
    struct nic_link down = {false, 0, 0}; bool was_up = nic.link.up; nic.link = down;
    if (was_up) netdrv_link_report(&nic.device, &down); else nic.device.link_up = false;
}
/* Resume: restore configuration space (BARs, command, MSI) and reinitialise around the same rings. */
bool e1000_nic_resume(void) {
    if (!nic.active || !nic.suspended) return nic.active;
    pci_device_t* d = nic.pci;
    for (unsigned i = 0; i < 6; i++)
        if (pci_read32(d->bus, d->device, d->function, (uint8_t)(PCI_BAR0 + 4 * i)) != nic.bars[i])
            pci_write32(d->bus, d->device, d->function, (uint8_t)(PCI_BAR0 + 4 * i), nic.bars[i]);
    pci_write32(d->bus, d->device, d->function, PCI_COMMAND, (nic.command | 6U) & ~0x400U);
    if (nic.vector >= 0x40 && pci_enable_msi(d, nic.vector, 0)) vga_print("[64] Intel NIC: MSI restore failed after resume\n");
    nic.suspended = false; nic.dead = false;
    if (rd(STATUS) == 0xFFFFFFFFU) { nic.dead = true; vga_print("[64] Intel NIC: device absent after resume\n"); return false; }
    netdrv_booting = false;
    if (pch() && !pch_phy_init()) return false;
    if (!hw_init()) { vga_print("[64] Intel NIC: reinitialisation after resume failed\n"); return false; }
    nic.next_watchdog = native_clock_ns() + WATCHDOG_NS;
    return true;
}
