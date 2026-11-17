/* Realtek PCIe Ethernet: RTL8111/8168 g, gu, h, ep, fp/RTL8117 (1 GbE) and RTL8125 A/B/D (2.5 GbE),
 * RTL8126A (5 GbE) -- the chips of the Linux r8169 driver from 2013 on (MAC versions 40, 42, 46, 51, 52,
 * 61, 63, 64, 70). Written from the RTL8169/8111 datasheet register model and the register semantics the
 * Linux r8169 driver and Realtek's BSD-licensed DPDK r8169 PMD document; no code is copied.
 * NOT YET RUN ON REAL HARDWARE: verified against the register-level simulator in tests/rtl8169_sim_test.c.
 *
 * Chip identification: TxConfig bits 30:20 (XID). Older RTL8169/8168b-f/8101 parts are recognised and
 * reported as unsupported (their PHY tuning tables are not implemented), so the next NIC driver gets a turn.
 * Register paths: MAC registers in the first memory BAR; MAC OCP words at 0xB0; the internal PHY over
 * GPHY_OCP 0xB8 (standard MII registers at OCP 0xA400 + 2*reg, page p register r at 16*p + 2*(r-16));
 * ERI at 0x70/0x74 (EXGMAC and OOB types); the PCIe PHY at 0x80 (5-bit address on 8168, 7-bit on 8125+);
 * PCI extended configuration through ECAM when the MCFG table maps it, else the chip's CSI window 0x64/0x68.
 *
 * PHY/MAC firmware: rtl_nic/<chip>.fw from linux-firmware, pinned by SHA-256 (netdrv.c). The root volume is
 * mounted after the network comes up, so the boot runs the PHY configuration without it and
 * rtl8169_nic_late_init() loads the file afterwards and restarts the PHY with it (one ~3 s link renegotiation
 * early in boot). A missing or mismatching file is logged and the NIC keeps running without it.
 *
 * Data path (like e1000.c): 256 receive / 64 transmit legacy descriptors with 2 KiB buffers in one DMA area,
 * placed anywhere in RAM (64-bit DMA; all supported chips have it). Interrupts: MSI-X, MSI, INTx or none
 * (netdrv.c). The IRQ handler reads and acknowledges the status register, masks the chip's interrupts and
 * flags work (interrupt mitigation as NAPI does); the network poll drains the ring, returns descriptors and
 * unmasks. Receive also works without an interrupt (the next descriptor is checked in memory). Transmit
 * never waits; slots are reclaimed from their OWN bits. A watchdog (1 s, run through the network poll)
 * polls the link when there is no interrupt, re-rings a lost doorbell, resets the chip when a frame has been
 * stuck in the transmit ring for 5 s with the link up, and notices a device that stopped answering (all-ones
 * reads) and brings it back when it answers again. No checksum offload: the shared protocol stack computes
 * and verifies every checksum in software and has no per-packet offload flags, and the receive VLAN
 * stripping is off. Suspend/resume: rtl8169_nic_suspend()/rtl8169_nic_resume() (nic.c native_nic_*). */
#include "netdrv.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

/* ---- registers ---- */
#define MAC0 0x00
#define MAC4 0x04
#define MAR0 0x08
#define TX_DESC_LOW 0x20
#define TX_DESC_HIGH 0x24
#define INT_CFG0_8125 0x34
#define CHIP_CMD 0x37
#define TX_POLL 0x38
#define INTR_MASK 0x3C
#define INTR_STATUS 0x3E
#define INTR_MASK_8125 0x38
#define INTR_STATUS_8125 0x3C
#define TX_CONFIG 0x40
#define RX_CONFIG 0x44
#define CFG9346 0x50
#define CONFIG1 0x52
#define CONFIG2 0x53
#define CONFIG3 0x54
#define CONFIG5 0x56
#define CSIDR 0x64
#define CSIAR 0x68
#define PHY_STATUS 0x6C
#define PMCH 0x6F
#define ERIDR 0x70
#define ERIAR 0x74
#define INT_CFG1_8125 0x7A
#define EPHYAR 0x80
#define TX_POLL_8125 0x90
#define MAC_OCP 0xB0
#define GPHY_OCP 0xB8
#define DLLPR 0xD0
#define MCU 0xD3
#define RX_MAX_SIZE 0xDA
#define CPLUS_CMD 0xE0
#define INTR_MITIGATE 0xE2
#define RX_DESC_LOW 0xE4
#define RX_DESC_HIGH 0xE8
#define MAX_TX_PACKET 0xEC
#define MISC 0xF0
#define MISC_1 0xF2
#define IBCR0 0xF8
#define IBCR2 0xF9
#define IBISR0 0xFB
#define EEE_LED 0x1B
#define MAC0_BKP 0x19E0
#define RSS_CTRL_8125 0x4500
#define Q_NUM_CTRL_8125 0x4800
#define EEE_TXIDLE_8125 0x6048
/* bits */
#define CMD_RESET 0x10
#define CMD_RX 0x08
#define CMD_TX 0x04
#define CMD_STOP 0x80
#define NPQ 0x40
#define INT_SYSERR 0x8000U
#define INT_LINK 0x0020U
#define INT_RX_FIFO 0x0040U
#define INT_RX_OVERFLOW 0x0010U
#define INT_TX_ERR 0x0008U
#define INT_TX_OK 0x0004U
#define INT_RX_ERR 0x0002U
#define INT_RX_OK 0x0001U
#define ACCEPT_ERR_MASK 0x30U
#define ACCEPT_BROADCAST 0x08U
#define ACCEPT_MULTICAST 0x04U
#define ACCEPT_MINE 0x02U
#define RX_VLAN_8125 (3U << 22)
#define TXCFG_AUTO_FIFO (1U << 7)
#define TXCFG_EMPTY (1U << 11)
#define RXDV_GATED (1U << 19)
#define NOW_IS_OOB 0x80
#define RXTX_EMPTY 0x30
#define LINK_LIST_READY 0x02
#define OCP_FLAG 0x80000000U
#define OCP_STD_PHY_BASE 0xA400U
#define CP_RX_VLAN 0x40
#define CP_RX_CSUM 0x20
#define CP_MASK (0x2000 | 0x40 | 0x20 | 0x3)
#define ERI_EXGMAC 0U
#define ERI_OOB (2U << 16)
#define ERI_MASK_0001 (1U << 12)
#define ERI_MASK_0011 (3U << 12)
#define ERI_MASK_1111 (15U << 12)
/* descriptors */
#define DESC_OWN (1U << 31)
#define DESC_EOR (1U << 30)
#define DESC_FS (1U << 29)
#define DESC_LS (1U << 28)
#define RX_RES (1U << 21)
#define RX_CRC (1U << 19)
/* MII */
#define BMCR 0x00
#define BMSR 0x01
#define BMCR_RESET 0x8000
#define BMCR_ANENABLE 0x1000
#define BMCR_PDOWN 0x0800
#define BMCR_ANRESTART 0x0200
#define ADVERTISE_ALL 0x0DE1   /* CSMA, 10/100 half+full, symmetric + asymmetric pause */
#define ADVERTISE_1000FULL 0x0200
#define PHY_AN_10GBT_CTRL 0xA5D4  /* 2.5G (bit 7) and 5G (bit 8) advertisement */
#define PHY_PHYSR 0xA434

#define RX_COUNT 256U
#define TX_COUNT 64U
#define BUFFER 2048U
#define RX_BATCH 256U
#define RX_RING_OFFSET 0U
#define TX_RING_OFFSET (RX_COUNT * 16U)
#define RX_BUFFER_OFFSET 8192U
#define TX_BUFFER_OFFSET (RX_BUFFER_OFFSET + RX_COUNT * BUFFER)
#define DMA_BYTES (TX_BUFFER_OFFSET + TX_COUNT * BUFFER)
#define WATCHDOG_NS 1000000000ULL
#define TX_TIMEOUT_NS 5000000000ULL
#define DEAD_RETRY_NS 2000000000ULL
#define FIRMWARE_STEPS 1000000U
_Static_assert(TX_RING_OFFSET + TX_COUNT * 16U <= RX_BUFFER_OFFSET, "descriptor rings overlap buffers");

struct rtl_desc { volatile uint32_t opts1, opts2; volatile uint64_t address; };
struct rtl_chip { uint16_t mask, xid; uint8_t version; const char* name; const char* firmware; };
static const struct rtl_chip chips[] = {
    {0x7cf, 0x64a, 70, "RTL8126A", "rtl_nic/rtl8126a-3.fw"}, {0x7cf, 0x649, 70, "RTL8126A", "rtl_nic/rtl8126a-2.fw"},
    {0x7cf, 0x689, 64, "RTL8125D", "rtl_nic/rtl8125d-2.fw"}, {0x7cf, 0x688, 64, "RTL8125D", "rtl_nic/rtl8125d-1.fw"},
    {0x7cf, 0x641, 63, "RTL8125B", "rtl_nic/rtl8125b-2.fw"}, {0x7cf, 0x609, 61, "RTL8125A", "rtl_nic/rtl8125a-3.fw"},
    {0x7cf, 0x54b, 52, "RTL8168fp/RTL8117", NULL}, {0x7cf, 0x54a, 52, "RTL8168fp/RTL8117", "rtl_nic/rtl8168fp-3.fw"},
    {0x7cf, 0x502, 51, "RTL8168ep/8111ep", NULL},
    {0x7cf, 0x541, 46, "RTL8168h/8111h", "rtl_nic/rtl8168h-2.fw"}, {0x7cf, 0x6c0, 46, "RTL8168M", "rtl_nic/rtl8168h-2.fw"},
    {0x7cf, 0x509, 42, "RTL8168gu/8111gu", "rtl_nic/rtl8168g-3.fw"}, {0x7cf, 0x4c0, 40, "RTL8168g/8111g", "rtl_nic/rtl8168g-2.fw"},
};
static const uint16_t vendor_ids[][2] = {
    {0x10EC, 0x8125}, {0x10EC, 0x3000}, {0x10EC, 0x8126}, {0x10EC, 0x8168}, {0x10EC, 0x8161}, {0x10EC, 0x8162},
    {0x10EC, 0x8167}, {0x10EC, 0x8169}, {0x10EC, 0x2502}, {0x10EC, 0x2600}, {0x10EC, 0x5000}, {0x10EC, 0x0E10},
    {0x10FF, 0x8168}, {0x1186, 0x4300}, {0x1186, 0x4302}, {0x1259, 0xC107}, {0x16EC, 0x0116}, {0x1737, 0x1032}, {0x0001, 0x8168},
};

static struct {
    net_device_t device;
    pci_device_t* pci;
    volatile uint8_t* mmio;
    const struct rtl_chip* chip;
    unsigned version;
    uint64_t dma, dma_physical;
    struct rtl_desc *rx, *tx;
    unsigned rx_next, tx_next, tx_dirty;
    uint64_t tx_time[TX_COUNT];
    uint32_t irq_mask, ocp_base;
    uint16_t cp_cmd, command;
    uint32_t bars[6];
    struct netdrv_irq irq;
    struct nic_link link;
    bool active, suspended, dead, dash, dash_enabled, discarding;
    volatile bool pending, link_event;
    uint64_t next_watchdog, dead_since;
    uint8_t* firmware; uint32_t firmware_size;
    unsigned resets, firmware_errors, ocp_errors;
} nic;

/* ---- register access (tests/rtl8169_sim_test.c supplies these with RTL_TEST_MMIO) ---- */
#ifndef RTL_TEST_MMIO
static inline uint8_t rd8(unsigned r) { return *(volatile uint8_t*)(nic.mmio + r); }
static inline uint16_t rd16(unsigned r) { return *(volatile uint16_t*)(nic.mmio + r); }
static inline uint32_t rd32(unsigned r) { return *(volatile uint32_t*)(nic.mmio + r); }
static inline void wr8(unsigned r, uint8_t v) { *(volatile uint8_t*)(nic.mmio + r) = v; }
static inline void wr16(unsigned r, uint16_t v) { *(volatile uint16_t*)(nic.mmio + r) = v; }
static inline void wr32(unsigned r, uint32_t v) { *(volatile uint32_t*)(nic.mmio + r) = v; }
#endif
static bool is8125(void) { return nic.version >= 61; }
static void mod8(unsigned r, uint8_t clear, uint8_t set) { wr8(r, (uint8_t)((rd8(r) & ~clear) | set)); }
static void commit(void) { (void)rd8(CHIP_CMD); }
static void unlock_config(void) { wr8(CFG9346, 0xC0); }
static void lock_config(void) { wr8(CFG9346, 0x00); }
/* Polls `done` every `step` microseconds up to `count` times. */
static bool wait_for(bool (*done)(void), unsigned step, unsigned count) {
    for (unsigned i = 0; i < count; i++) { if (done()) return true; netdrv_udelay(step); }
    return done();
}

/* ---- MAC OCP, PHY OCP, ERI, EPHY, CSI ---- */
static bool ocp_bad(uint32_t reg) { if (reg & 0xFFFF0001U) { nic.ocp_errors++; return true; } return false; }
static void mac_ocp_write(uint32_t reg, uint16_t value) { if (!ocp_bad(reg)) wr32(MAC_OCP, OCP_FLAG | reg << 15 | value); }
static uint16_t mac_ocp_read(uint32_t reg) { if (ocp_bad(reg)) return 0; wr32(MAC_OCP, reg << 15); return (uint16_t)rd32(MAC_OCP); }
static void mac_ocp_modify(uint32_t reg, uint16_t clear, uint16_t set) { mac_ocp_write(reg, (uint16_t)((mac_ocp_read(reg) & ~clear) | set)); }
static bool gphy_idle(void) { return !(rd32(GPHY_OCP) & OCP_FLAG); }
static bool gphy_ready(void) { return (rd32(GPHY_OCP) & OCP_FLAG) != 0; }
static void phy_ocp_write(uint32_t reg, uint16_t value) {
    if (ocp_bad(reg)) return;
    wr32(GPHY_OCP, OCP_FLAG | reg << 15 | value);
    if (!wait_for(gphy_idle, 25, 10)) nic.ocp_errors++;
}
static int phy_ocp_read(uint32_t reg) {
    if (ocp_bad(reg)) return -1;
    wr32(GPHY_OCP, reg << 15);
    if (!wait_for(gphy_ready, 25, 10)) { nic.ocp_errors++; return -1; }
    return (int)(rd32(GPHY_OCP) & 0xFFFF);
}
static bool eri_idle(void) { return !(rd32(ERIAR) & OCP_FLAG); }
static bool eri_ready(void) { return (rd32(ERIAR) & OCP_FLAG) != 0; }
static void eri_write_type(unsigned address, uint32_t mask, uint32_t value, uint32_t type) {
    uint32_t command = OCP_FLAG | type | mask | address;
    if (type == ERI_OOB && nic.version == 52) command |= 0xF70U << 18;
    wr32(ERIDR, value); wr32(ERIAR, command);
    if (!wait_for(eri_idle, 100, 100)) nic.ocp_errors++;
}
static uint32_t eri_read_type(unsigned address, uint32_t type) {
    uint32_t command = type | ERI_MASK_1111 | address;
    if (type == ERI_OOB && nic.version == 52) command |= 0xF70U << 18;
    wr32(ERIAR, command);
    if (!wait_for(eri_ready, 100, 100)) { nic.ocp_errors++; return ~0U; }
    return rd32(ERIDR);
}
static void eri_write(unsigned address, uint32_t mask, uint32_t value) { eri_write_type(address, mask, value, ERI_EXGMAC); }
static uint32_t eri_read(unsigned address) { return eri_read_type(address, ERI_EXGMAC); }
static void eri_modify(unsigned address, uint32_t clear, uint32_t set) { eri_write(address, ERI_MASK_1111, (eri_read(address) & ~clear) | set); }
static bool ephy_idle(void) { return !(rd32(EPHYAR) & OCP_FLAG); }
static bool ephy_ready(void) { return (rd32(EPHYAR) & OCP_FLAG) != 0; }
static unsigned ephy_mask(void) { return is8125() ? 0x7F : 0x1F; }
static void ephy_write(unsigned reg, uint16_t value) {
    wr32(EPHYAR, OCP_FLAG | value | (reg & ephy_mask()) << 16);
    if (!wait_for(ephy_idle, 10, 100)) nic.ocp_errors++;
    netdrv_udelay(10);
}
static uint16_t ephy_read(unsigned reg) {
    wr32(EPHYAR, (reg & ephy_mask()) << 16);
    if (!wait_for(ephy_ready, 10, 100)) { nic.ocp_errors++; return 0xFFFF; }
    return (uint16_t)rd32(EPHYAR);
}
struct ephy_change { uint8_t reg; uint16_t clear, set; };
static void ephy_apply(const struct ephy_change* list, unsigned count) {
    for (unsigned i = 0; i < count; i++) ephy_write(list[i].reg, (uint16_t)((ephy_read(list[i].reg) & ~list[i].clear) | list[i].set));
}
#define EPHY(list) ephy_apply(list, sizeof(list) / sizeof(*list))
static bool csi_idle(void) { return !(rd32(CSIAR) & OCP_FLAG); }
static bool csi_ready(void) { return (rd32(CSIAR) & OCP_FLAG) != 0; }
/* PCI configuration bytes beyond 0xFF (Realtek-specific registers): ECAM when mapped, else the CSI window. */
static void config_modify_extended(unsigned offset, uint8_t clear, uint8_t set) {
    volatile uint8_t* ecam = (volatile uint8_t*)pci_ecam_address(0, nic.pci->bus, nic.pci->device, nic.pci->function);
    if (ecam) { ecam[offset] = (uint8_t)((ecam[offset] & ~clear) | set); return; }
    uint32_t function = (uint32_t)nic.pci->function << 16, address = offset & 0xFFC, shift = (offset & 3) * 8;
    wr32(CSIAR, address | function | 0xF000U);
    if (!wait_for(csi_ready, 10, 100)) { nic.ocp_errors++; return; }
    uint32_t value = rd32(CSIDR);
    value = (value & ~((uint32_t)clear << shift)) | (uint32_t)set << shift;
    wr32(CSIDR, value); wr32(CSIAR, OCP_FLAG | address | function | 0xF000U);
    if (!wait_for(csi_idle, 10, 100)) nic.ocp_errors++;
}

/* ---- the internal PHY (MII registers emulated over OCP: register 0x1F selects the page) ---- */
static void phy_bmcr_quirk(uint16_t value) {  /* RTL8168g: PHY MCU interrupts off while the PHY is powered down */
    if (nic.version != 40) return;
    if ((value & BMCR_RESET) || !(value & BMCR_PDOWN)) eri_modify(0x1A8, 0, 0xFC000000U);
    else eri_modify(0x1A8, 0xFC000000U, 0);
}
static void phy_write(unsigned reg, uint16_t value) {
    if (reg == 0x1F) { nic.ocp_base = value ? (uint32_t)value << 4 : OCP_STD_PHY_BASE; return; }
    if (nic.ocp_base != OCP_STD_PHY_BASE) reg -= 0x10;
    else if (reg == BMCR) phy_bmcr_quirk(value);
    phy_ocp_write(nic.ocp_base + reg * 2, value);
}
static int phy_read(unsigned reg) {
    if (reg == 0x1F) return nic.ocp_base == OCP_STD_PHY_BASE ? 0 : (int)(nic.ocp_base >> 4);
    if (nic.ocp_base != OCP_STD_PHY_BASE) reg -= 0x10;
    return phy_ocp_read(nic.ocp_base + reg * 2);
}
static void phy_page(unsigned page) { phy_write(0x1F, (uint16_t)page); }
static void phy_modify(unsigned reg, uint16_t clear, uint16_t set) {
    int value = phy_read(reg); if (value < 0) return;
    phy_write(reg, (uint16_t)(((uint16_t)value & ~clear) | set));
}
static void phy_modify_paged(unsigned page, unsigned reg, uint16_t clear, uint16_t set) { phy_page(page); phy_modify(reg, clear, set); phy_page(0); }
static int phy_read_paged(unsigned page, unsigned reg) { phy_page(page); int value = phy_read(reg); phy_page(0); return value; }
static void phy_write_paged(unsigned page, unsigned reg, uint16_t value) { phy_page(page); phy_write(reg, value); phy_page(0); }
static void phy_param_g(uint16_t parameter, uint16_t clear, uint16_t set) { phy_page(0x0A43); phy_write(0x13, parameter); phy_modify(0x14, clear, set); phy_page(0); }
static void phy_param_8125(uint16_t parameter, uint16_t clear, uint16_t set) {
    phy_ocp_write(0xB87C, parameter);
    int value = phy_ocp_read(0xB87E); if (value >= 0) phy_ocp_write(0xB87E, (uint16_t)(((uint16_t)value & ~clear) | set));
}

/* ---- firmware (rtl_nic/ files): a program of 32-bit actions over PHY or MAC-MCU register writes ---- */
enum { FW_READ = 0, FW_OR = 1, FW_AND = 2, FW_BJMPN = 3, FW_MDIO_CHG = 4, FW_CLEAR_COUNT = 7, FW_WRITE = 8,
       FW_COUNT_EQ_SKIP = 9, FW_EQ_SKIPN = 10, FW_NEQ_SKIPN = 11, FW_WRITE_PREVIOUS = 12, FW_SKIPN = 13, FW_DELAY_MS = 14 };
struct rtl_firmware { const uint8_t* code; uint32_t count; char version[33]; };
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* The two file formats: a header (magic 0, version, start, length, byte sum 0) or a bare action list. */
static bool firmware_parse(const uint8_t* data, uint32_t size, const char* name, struct rtl_firmware* fw) {
    memset(fw, 0, sizeof(*fw));
    if (size < 4) return false;
    if (!le32(data)) {
        if (size < 45) return false;
        uint8_t sum = 0; for (uint32_t i = 0; i < size; i++) sum = (uint8_t)(sum + data[i]);
        if (sum) return false;
        uint32_t start = le32(data + 36), length = le32(data + 40);
        if (start > size || length > (size - start) / 4) return false;
        memcpy(fw->version, data + 4, 32); fw->version[32] = 0;
        fw->code = data + start; fw->count = length;
    } else {
        if (size % 4) return false;
        unsigned n = 0; while (name[n] && n < 32) { fw->version[n] = name[n]; n++; } fw->version[n] = 0;
        fw->code = data; fw->count = size / 4;
    }
    for (uint32_t i = 0; i < fw->count; i++) {
        uint32_t action = le32(fw->code + 4 * i), value = action & 0xFFFF, reg = (action >> 16) & 0xFFF;
        switch (action >> 28) {
        case FW_READ: case FW_OR: case FW_AND: case FW_CLEAR_COUNT: case FW_WRITE: case FW_WRITE_PREVIOUS: case FW_DELAY_MS: break;
        case FW_MDIO_CHG: if (value > 1) return false; break;
        case FW_BJMPN: if (reg > i || reg == 0) return false; break;   /* a zero-distance jump would never end */
        case FW_COUNT_EQ_SKIP: if (i + 2 >= fw->count) return false; break;
        case FW_EQ_SKIPN: case FW_NEQ_SKIPN: case FW_SKIPN: if (i + 1 + reg >= fw->count) return false; break;
        default: return false;
        }
    }
    return true;
}
static void mcu_write(unsigned reg, uint16_t value) {
    if (reg == 0x1F) { nic.ocp_base = (uint32_t)value << 4; return; }
    mac_ocp_write(nic.ocp_base + reg, value);
}
static int mcu_read(unsigned reg) { return mac_ocp_read(nic.ocp_base + reg); }
static bool bmcr_reset_done(void) { int v = phy_read(BMCR); return v >= 0 && !(v & BMCR_RESET); }
/* Runs a validated program; a step budget bounds even a program whose backward jumps loop forever. */
static bool firmware_run(const struct rtl_firmware* fw) {
    bool mac = false; int previous = 0; unsigned count = 0, steps = 0;
    for (uint32_t i = 0; i < fw->count; i++) {
        if (++steps > FIRMWARE_STEPS) { nic.ocp_base = OCP_STD_PHY_BASE; return false; }
        uint32_t action = le32(fw->code + 4 * i), value = action & 0xFFFF, reg = (action >> 16) & 0xFFF;
        switch (action >> 28) {
        case FW_READ: previous = mac ? mcu_read(reg) : phy_read(reg); count++; break;
        case FW_OR: previous |= (int)value; break;
        case FW_AND: previous &= (int)value; break;
        case FW_BJMPN: i -= reg + 1; break;
        case FW_MDIO_CHG: mac = value != 0; break;
        case FW_CLEAR_COUNT: count = 0; break;
        case FW_WRITE: if (mac) mcu_write(reg, (uint16_t)value); else phy_write(reg, (uint16_t)value); break;
        case FW_COUNT_EQ_SKIP: if (count == value) i++; break;
        case FW_EQ_SKIPN: if (previous == (int)value) i += reg; break;
        case FW_NEQ_SKIPN: if (previous != (int)value) i += reg; break;
        case FW_WRITE_PREVIOUS: if (mac) mcu_write(reg, (uint16_t)previous); else phy_write(reg, (uint16_t)previous); break;
        case FW_SKIPN: i += reg; break;
        case FW_DELAY_MS: netdrv_mdelay(value); break;
        }
    }
    nic.ocp_base = OCP_STD_PHY_BASE;            /* at least one firmware leaves a page selected */
    for (unsigned i = 0; i < 12 && !bmcr_reset_done(); i++) netdrv_mdelay(50);   /* PHY soft reset may still run */
    return true;
}
static void firmware_apply(void) {
    if (!nic.firmware) return;
    struct rtl_firmware fw;
    if (!firmware_parse(nic.firmware, nic.firmware_size, nic.chip->firmware, &fw) || !firmware_run(&fw)) {
        nic.firmware_errors++;
        vga_print("[64] Realtek: firmware "); vga_print(nic.chip->firmware); vga_print(" rejected (malformed program)\n");
        netdrv_firmware_free(nic.firmware, nic.firmware_size); nic.firmware = NULL; nic.firmware_size = 0;
    }
}

/* ---- chip-level helpers ---- */
static bool txcfg_empty(void) { return (rd32(TX_CONFIG) & TXCFG_EMPTY) != 0; }
static bool rxtx_empty(void) { return (rd8(MCU) & RXTX_EMPTY) == RXTX_EMPTY; }
static bool rxtx_empty_2(void) { return (rd16(INTR_MITIGATE) & 0x0103) == 0x0103; }
static bool link_list_ready(void) { return (rd8(MCU) & LINK_LIST_READY) != 0; }
static bool reset_done(void) { return !(rd8(CHIP_CMD) & CMD_RESET); }
static bool e00e_clear(void) { return !(mac_ocp_read(0xE00E) & (1U << 13)); }
static bool cmac_done(void) { return (rd8(IBISR0) & 0x20) != 0; }
static void wait_fifo_empty(void) {
    if (nic.version <= 52) { wait_for(txcfg_empty, 100, 42); wait_for(rxtx_empty, 100, 42); }
    else if (nic.version == 61) wait_for(rxtx_empty, 100, 42);
    else { wr8(CHIP_CMD, rd8(CHIP_CMD) | CMD_STOP); wait_for(rxtx_empty, 100, 42); wait_for(rxtx_empty_2, 100, 42); }
}
static void rxdv_gate(bool on) {
    if (!on) { wr32(MISC, rd32(MISC) & ~RXDV_GATED); return; }
    wr32(MISC, rd32(MISC) | RXDV_GATED); netdrv_mdelay(2); wait_fifo_empty();
}
static void irq_off(void) { if (is8125()) wr32(INTR_MASK_8125, 0); else wr16(INTR_MASK, 0); }
static void irq_on(void) { if (!nic.irq.vector) return; if (is8125()) wr32(INTR_MASK_8125, nic.irq_mask); else wr16(INTR_MASK, (uint16_t)nic.irq_mask); }
static uint32_t irq_status(void) { return is8125() ? rd32(INTR_STATUS_8125) : rd16(INTR_STATUS); }
static void irq_ack(uint32_t bits) { if (is8125()) wr32(INTR_STATUS_8125, bits); else wr16(INTR_STATUS, (uint16_t)bits); }
static void stop_cmac(void) {   /* RTL8168ep/fp: stop the out-of-band management CPU's MAC access */
    wr8(IBCR2, rd8(IBCR2) & ~1U);
    wait_for(cmac_done, 1000, 1000);   /* the chip answers within milliseconds; give up after 1 s */
    wr8(IBISR0, rd8(IBISR0) | 0x20); wr8(IBCR0, rd8(IBCR0) & ~1U);
}
static bool dash_ack(void) { return (eri_read_type(0x124, ERI_OOB) & 1) != 0; }
static void dash_driver(bool start) {   /* RTL8168ep/fp: tell the management firmware that the OS driver runs */
    if (!nic.dash) return;
    if (!start) stop_cmac();
    eri_write_type(0x180, ERI_MASK_0001, start ? 0x05 : 0x06, ERI_OOB);
    eri_write_type(0x30, ERI_MASK_0001, eri_read_type(0x30, ERI_OOB) | 1, ERI_OOB);
    if (nic.dash_enabled) { for (unsigned i = 0; i < 30 && dash_ack() != start; i++) netdrv_mdelay(10); }
}
static void hw_initialize(void) {   /* probe-time: leave out-of-band mode, link the shared FIFO */
    if (nic.version == 51 || nic.version == 52) stop_cmac();
    rxdv_gate(true);
    wr8(CHIP_CMD, rd8(CHIP_CMD) & ~(CMD_TX | CMD_RX));
    netdrv_mdelay(1);
    wr8(MCU, rd8(MCU) & ~NOW_IS_OOB);
    mac_ocp_modify(0xE8DE, 1U << 14, 0);
    wait_for(link_list_ready, 100, 42);
    if (is8125()) { mac_ocp_write(0xC0AA, 0x07D0); mac_ocp_write(0xC0A6, 0x0150); mac_ocp_write(0xC01E, 0x5555); }
    else mac_ocp_modify(0xE8DE, 0, 1U << 15);
    wait_for(link_list_ready, 100, 42);
}
static bool hw_reset(void) { wr8(CHIP_CMD, CMD_RESET); return wait_for(reset_done, 100, 100); }
static void aspm_clkreq_off(void) {
    if (nic.version == 46 || is8125()) mac_ocp_modify(0xE092, 0x00FF, 0);
    if (nic.version == 70) wr8(INT_CFG0_8125, rd8(INT_CFG0_8125) & ~0x08);
    else mod8(CONFIG2, 0x80, 0);
    mod8(CONFIG5, 0x01, 0);
}
static void aspm_entry_latency(void) { config_modify_extended(0x070F, 0xFF, 0x27); }   /* L0s 7 us, L1 16 us */
static void fifo_and_pause(uint8_t low, uint8_t high) {
    eri_write(0xC8, ERI_MASK_1111, 0x08U << 16 | 0x02); eri_write(0xE8, ERI_MASK_1111, 0x10U << 16 | 0x06);
    eri_write(0xCC, ERI_MASK_0001, low); eri_write(0xD0, ERI_MASK_0001, high);
}
static void packet_filter_reset(void) { eri_modify(0xDC, 1, 0); eri_modify(0xDC, 0, 1); }
static void eee_mac_8168(void) { wr8(EEE_LED, rd8(EEE_LED) & ~0x07); eri_modify(0x1B0, 0, 0x0003); }
static void l2l3_off(void) { wr8(CONFIG3, rd8(CONFIG3) & ~0x02); }
static void sw_count_1ms(void) {
    int saw = phy_read_paged(0x0C42, 0x13);
    if (saw > 0 && (saw & 0x3FFF)) mac_ocp_modify(0xD412, 0x0FFF, (uint16_t)((16000000 / (saw & 0x3FFF)) & 0x0FFF));
}

/* ---- per-chip MAC configuration ---- */
static void start_8168g(void) {
    fifo_and_pause(0x38, 0x48); aspm_entry_latency(); packet_filter_reset();
    eri_write(0x2F8, ERI_MASK_0011, 0x1D8F); rxdv_gate(false);
    eri_write(0xC0, ERI_MASK_0011, 0); eri_write(0xB8, ERI_MASK_0011, 0);
    eee_mac_8168(); eri_modify(0x2FC, 0x06, 0x01); eri_modify(0x1B0, 1U << 12, 0); l2l3_off();
}
static void start_8168h(void) {
    static const struct ephy_change list[] = {{0x1E, 0x0800, 0x0001}, {0x1D, 0, 0x0800}, {0x05, 0xFFFF, 0x2089}, {0x06, 0xFFFF, 0x5881},
                                              {0x04, 0xFFFF, 0x854A}, {0x01, 0xFFFF, 0x068B}};
    EPHY(list);
    fifo_and_pause(0x38, 0x48); aspm_entry_latency(); packet_filter_reset();
    eri_modify(0xDC, 0, 0x001C); eri_write(0x5F0, ERI_MASK_0011, 0x4F87); rxdv_gate(false);
    eri_write(0xC0, ERI_MASK_0011, 0); eri_write(0xB8, ERI_MASK_0011, 0); eee_mac_8168();
    wr8(DLLPR, rd8(DLLPR) & ~0x40); wr8(MISC_1, rd8(MISC_1) & ~0x40); wr8(DLLPR, rd8(DLLPR) & ~0x80);
    eri_modify(0x1B0, 1U << 12, 0); l2l3_off(); sw_count_1ms();
    mac_ocp_modify(0xE056, 0x00F0, 0x0070); mac_ocp_modify(0xE052, 0x6000, 0x8008);
    mac_ocp_modify(0xE0D6, 0x01FF, 0x017F); mac_ocp_modify(0xD420, 0x0FFF, 0x047F);
    mac_ocp_write(0xE63E, 0x0001); mac_ocp_write(0xE63E, 0x0000); mac_ocp_write(0xC094, 0); mac_ocp_write(0xC09E, 0);
}
static void start_8168ep(void) {
    static const struct ephy_change list[] = {{0x00, 0, 0x0080}, {0x0D, 0x0100, 0x0200}, {0x19, 0x8021, 0}, {0x1E, 0, 0x2000}};
    EPHY(list);
    stop_cmac(); fifo_and_pause(0x2F, 0x5F); aspm_entry_latency(); packet_filter_reset();
    eri_write(0x5F0, ERI_MASK_0011, 0x4F87); rxdv_gate(false);
    eri_write(0xC0, ERI_MASK_0011, 0); eri_write(0xB8, ERI_MASK_0011, 0); eee_mac_8168();
    eri_modify(0x2FC, 0x06, 0x01); wr8(DLLPR, rd8(DLLPR) & ~0x80); l2l3_off();
    wr8(DLLPR, rd8(DLLPR) & ~0x40); wr8(MISC_1, rd8(MISC_1) & ~0x40);
    mac_ocp_modify(0xD3E2, 0x0FFF, 0x0271); mac_ocp_modify(0xD3E4, 0x00FF, 0); mac_ocp_modify(0xE860, 0, 0x0080);
}
static void start_8117(void) {
    static const struct ephy_change list[] = {{0x19, 0x0040, 0x1100}, {0x59, 0x0040, 0x1100}};
    stop_cmac(); EPHY(list);
    fifo_and_pause(0x2F, 0x5F); aspm_entry_latency(); packet_filter_reset();
    eri_modify(0xD4, 0, 0x0010); eri_write(0x5F0, ERI_MASK_0011, 0x4F87); rxdv_gate(false);
    eri_write(0xC0, ERI_MASK_0011, 0); eri_write(0xB8, ERI_MASK_0011, 0); eee_mac_8168();
    wr8(DLLPR, rd8(DLLPR) & ~0x40); wr8(MISC_1, rd8(MISC_1) & ~0x40); wr8(DLLPR, rd8(DLLPR) & ~0x80);
    eri_modify(0x1B0, 1U << 12, 0); l2l3_off(); sw_count_1ms();
    mac_ocp_modify(0xE056, 0x00F0, 0x0070); mac_ocp_write(0xEA80, 0x0003); mac_ocp_modify(0xE052, 0, 0x0009);
    mac_ocp_modify(0xD420, 0x0FFF, 0x047F);
    mac_ocp_write(0xE63E, 0x0001); mac_ocp_write(0xE63E, 0x0000); mac_ocp_write(0xC094, 0); mac_ocp_write(0xC09E, 0);
    firmware_apply();   /* RTL8117: the firmware patches the MAC, not the PHY */
}
static void start_8125_common(void) {
    l2l3_off();
    wr16(0x382, 0x221B); wr32(RSS_CTRL_8125, 0); wr16(Q_NUM_CTRL_8125, 0);
    mac_ocp_modify(0xD40A, 0x0010, 0);                       /* UPS off */
    wr8(CONFIG1, rd8(CONFIG1) & ~0x10);
    mac_ocp_write(0xC140, 0xFFFF); mac_ocp_write(0xC142, 0xFFFF);
    mac_ocp_modify(0xD3E2, 0x0FFF, 0x03A9); mac_ocp_modify(0xD3E4, 0x00FF, 0); mac_ocp_modify(0xE860, 0, 0x0080);
    mac_ocp_modify(0xEB58, 0x0001, 0);                       /* legacy (v1) descriptors */
    if (nic.version == 70) wr8(0xD8, rd8(0xD8) & ~0x02);
    mac_ocp_modify(0xE614, 0x0700, nic.version == 70 ? 0x0400 : nic.version == 63 ? 0x0200 : 0x0300);
    mac_ocp_modify(0xE63E, 0x0C30, nic.version == 63 ? 0 : 0x0020);
    mac_ocp_modify(0xC0B4, 0, 0x000C); mac_ocp_modify(0xEB6A, 0x00FF, 0x0033); mac_ocp_modify(0xEB50, 0x03E0, 0x0040);
    mac_ocp_modify(0xE056, 0x00F0, 0x0030); mac_ocp_modify(0xE040, 0x1000, 0); mac_ocp_modify(0xEA1C, 0x0003, 0x0001);
    if (nic.version == 70) mac_ocp_modify(0xEA1C, 0x0300, 0); else mac_ocp_modify(0xEA1C, 0x0004, 0);
    mac_ocp_modify(0xE0C0, 0x4F0F, 0x4403); mac_ocp_modify(0xE052, 0x0080, 0x0068); mac_ocp_modify(0xD430, 0x0FFF, 0x047F);
    mac_ocp_modify(0xEA1C, 0x0004, 0); mac_ocp_modify(0xEB54, 0, 0x0001); netdrv_udelay(1); mac_ocp_modify(0xEB54, 0x0001, 0);
    wr16(0x1880, rd16(0x1880) & ~0x0030);
    mac_ocp_write(0xE098, 0xC302);
    wait_for(e00e_clear, 1000, 10);
    mac_ocp_modify(0xE040, 0, 0x0003);                       /* EEE in the MAC */
    if (nic.version == 61) mac_ocp_modify(0xEB62, 0, 0x0006);
    rxdv_gate(false);
}
static void start_mac(void) {
    static const struct ephy_change g1[] = {{0x00, 0x0008, 0}, {0x0C, 0x3FF0, 0x0820}, {0x1E, 0, 0x0001}, {0x19, 0x8000, 0}};
    static const struct ephy_change g2[] = {{0x00, 0x0008, 0}, {0x0C, 0x3FF0, 0x0820}, {0x19, 0xFFFF, 0x7C00}, {0x1E, 0xFFFF, 0x20EB},
                                            {0x0D, 0xFFFF, 0x1666}, {0x00, 0xFFFF, 0x10A3}, {0x06, 0xFFFF, 0xF050}, {0x04, 0, 0x0010}, {0x1D, 0x4000, 0}};
    static const struct ephy_change a2[] = {{0x04, 0xFFFF, 0xD000}, {0x0A, 0xFFFF, 0x8653}, {0x23, 0xFFFF, 0xAB66}, {0x20, 0xFFFF, 0x9455},
                                            {0x21, 0xFFFF, 0x99FF}, {0x29, 0xFFFF, 0xFE04}, {0x44, 0xFFFF, 0xD000}, {0x4A, 0xFFFF, 0x8653},
                                            {0x63, 0xFFFF, 0xAB66}, {0x60, 0xFFFF, 0x9455}, {0x61, 0xFFFF, 0x99FF}, {0x69, 0xFFFF, 0xFE04}};
    static const struct ephy_change b[] = {{0x0B, 0xFFFF, 0xA908}, {0x1E, 0xFFFF, 0x20EB}, {0x4B, 0xFFFF, 0xA908}, {0x5E, 0xFFFF, 0x20EB},
                                           {0x22, 0x0030, 0x0020}, {0x62, 0x0030, 0x0020}};
    if (is8125()) {
        wr8(INT_CFG0_8125, 0);
        unsigned end = nic.version == 63 || nic.version == 70 ? 0xA80 : 0xB00;   /* interrupt coalescing off */
        for (unsigned r = 0xA00; r < end; r += 4) wr32(r, 0);
        if (nic.version == 63 || nic.version == 70) wr16(INT_CFG1_8125, 0);
        mac_ocp_modify(0xEA84, 0, 0x0003);                   /* extended tally counters */
        if (nic.version == 70) config_modify_extended(0x0890, 0x01, 0);   /* zrxdc non-compliance timeout off */
        aspm_entry_latency();
        if (nic.version == 61) EPHY(a2);
        if (nic.version == 63) EPHY(b);
        start_8125_common();
        return;
    }
    wr8(MAX_TX_PACKET, 0x27);                                /* early transmit size (8168e-vl and later) */
    switch (nic.version) {
    case 40: start_8168g(); EPHY(g1); break;
    case 42: start_8168g(); EPHY(g2); break;
    case 46: start_8168h(); break;
    case 51: start_8168ep(); break;
    case 52: start_8117(); break;
    }
    wr16(INTR_MITIGATE, 0);                                  /* hardware coalescing off: the IRQ masking mitigates */
}

/* ---- per-chip PHY configuration ---- */
static void phy_gphy_10m(void) { phy_modify_paged(0x0A44, 0x11, 0, 1U << 11); }
static void phy_aldps_off(void) { phy_modify_paged(0x0A43, 0x10, 1U << 2, 0); }
static void phy_eee_g(void) { phy_modify_paged(0x0A43, 0x11, 0, 1U << 4); }
static void phy_eee_8125_common(void) { phy_modify_paged(0x0A6D, 0x14, 0x0010, 0); phy_modify_paged(0x0A42, 0x14, 0x0080, 0); phy_modify_paged(0x0A4A, 0x11, 0x0200, 0); }
static void phy_10m_aldps(void) {
    phy_modify_paged(0x0BCC, 0x14, 1U << 8, 0); phy_modify_paged(0x0A44, 0x11, 0, (1U << 7) | (1U << 6));
    phy_param_g(0x8084, 0x6000, 0); phy_modify_paged(0x0A43, 0x10, 0, 0x1003);
}
static void phy_swr(uint16_t reg, uint16_t value) {   /* "Improve SWR efficiency" / "force PWM mode" sequence */
    phy_write(0x1F, 0x0BCD); phy_write(0x14, 0x5065); phy_write(0x14, 0xD065);
    phy_write(0x1F, 0x0BC8); phy_write(reg, value);
    phy_write(0x1F, 0x0BCD); phy_write(0x14, 0x1065); phy_write(0x14, 0x9065); phy_write(0x14, 0x1065); phy_write(0x1F, 0);
}
static void phy_config(void) {
    switch (nic.version) {
    case 40: {
        firmware_apply();
        int v = phy_read_paged(0x0A46, 0x10);
        if (v >= 0 && (v & 0x100)) phy_modify_paged(0x0BCC, 0x12, 0x8000, 0); else phy_modify_paged(0x0BCC, 0x12, 0, 0x8000);
        v = phy_read_paged(0x0A46, 0x13);
        if (v >= 0 && (v & 0x100)) phy_modify_paged(0x0C41, 0x15, 0, 0x0002); else phy_modify_paged(0x0C41, 0x15, 0x0002, 0);
        phy_modify_paged(0x0A44, 0x11, 0, 0x000C); phy_10m_aldps(); phy_modify_paged(0x0A4B, 0x11, 0, 0x0004);
        phy_param_g(0x8012, 0, 0x8000); phy_modify_paged(0x0C42, 0x11, 0x2000, 0x4000); phy_swr(0x11, 0x5655);
        phy_aldps_off(); phy_eee_g(); break;
    }
    case 42: firmware_apply(); phy_eee_g(); break;
    case 46: {
        firmware_apply();
        phy_param_g(0x808A, 0x003F, 0x000A); phy_param_g(0x0811, 0, 0x0800); phy_modify_paged(0x0A42, 0x16, 0, 0x0002); phy_gphy_10m();
        mac_ocp_write(0xDD02, 0x807D);
        uint16_t d1 = mac_ocp_read(0xDD02), d2 = mac_ocp_read(0xDD00), ioffset = (uint16_t)(((d2 >> 1) & 0x7FF8) | (d2 & 7) | (d1 & 0x80 ? 0x8000 : 0));
        if (ioffset != 0xFFFF) phy_write_paged(0x0BCF, 0x16, ioffset);
        int data = phy_read_paged(0x0BCD, 0x16); unsigned rlen = data >= 0 && (data & 15) > 3 ? (unsigned)(data & 15) - 3 : 0;
        phy_write_paged(0x0BCD, 0x17, (uint16_t)(rlen | rlen << 4 | rlen << 8 | rlen << 12));
        phy_modify_paged(0x0A44, 0x11, 1U << 7, 0); phy_modify_paged(0x0A43, 0x10, 1U << 0, 0);
        phy_aldps_off(); phy_eee_g(); break;
    }
    case 51: {
        static const uint16_t ce[][2] = {{0x80F3, 0x8B00}, {0x80F0, 0x3A00}, {0x80EF, 0x0500}, {0x80F6, 0x6E00}, {0x80EC, 0x6800},
            {0x80ED, 0x7C00}, {0x80F2, 0xF400}, {0x80F4, 0x8500}, {0x8110, 0xA800}, {0x810F, 0x1D00}, {0x8111, 0xF500},
            {0x8113, 0x6100}, {0x8115, 0x9200}, {0x810E, 0x0400}, {0x810C, 0x7C00}, {0x810B, 0x5A00}, {0x80D1, 0xFF00},
            {0x80CD, 0x9E00}, {0x80D3, 0x0E00}, {0x80D5, 0xCA00}, {0x80D7, 0x8400}};
        phy_10m_aldps(); phy_param_g(0x8012, 0, 0x8000); phy_modify_paged(0x0C42, 0x11, 0x2000, 0x4000);
        for (unsigned i = 0; i < sizeof(ce) / sizeof(*ce); i++) phy_param_g(ce[i][0], 0xFF00, ce[i][1]);
        phy_swr(0x12, 0x00ED); phy_aldps_off(); phy_eee_g(); break;
    }
    case 52: {
        static const uint16_t ce[][3] = {{0x808E, 0xFF00, 0x4800}, {0x8090, 0xFF00, 0xCC00}, {0x8092, 0xFF00, 0xB000},
            {0x8088, 0xFF00, 0x6000}, {0x808B, 0x3F00, 0x0B00}, {0x808D, 0x1F00, 0x0600}, {0x808C, 0xFF00, 0xB000},
            {0x80A0, 0xFF00, 0x2800}, {0x80A2, 0xFF00, 0x5000}, {0x809B, 0xF800, 0xB000}, {0x809A, 0xFF00, 0x4B00},
            {0x809D, 0x3F00, 0x0800}, {0x80A1, 0xFF00, 0x7000}, {0x809F, 0x1F00, 0x0300}, {0x809E, 0xFF00, 0x8800},
            {0x80B2, 0xFF00, 0x2200}, {0x80AD, 0xF800, 0x9800}, {0x80AF, 0x3F00, 0x0800}, {0x80B3, 0xFF00, 0x6F00},
            {0x80B1, 0x1F00, 0x0300}, {0x80B0, 0xFF00, 0x9300}};
        for (unsigned i = 0; i < sizeof(ce) / sizeof(*ce); i++) phy_param_g(ce[i][0], ce[i][1], ce[i][2]);
        phy_param_g(0x8011, 0, 0x0800); phy_gphy_10m(); phy_param_g(0x8016, 0, 0x0400); phy_aldps_off();
        phy_eee_g(); phy_modify_paged(0x0A4A, 0x11, 0, 0x0200); phy_modify_paged(0x0A42, 0x14, 0, 0x0080); break;
    }
    case 61: {
        static const uint16_t pre[][4] = {{0x0AD4, 0x17, 0x0000, 0x0010}, {0x0AD1, 0x13, 0x03FF, 0x03FF}, {0x0AD3, 0x11, 0x003F, 0x0006},
            {0x0AC0, 0x14, 0x1100, 0x0000}, {0x0ACC, 0x10, 0x0003, 0x0002}, {0x0AD4, 0x10, 0x00E7, 0x0044}, {0x0AC1, 0x12, 0x0080, 0x0000},
            {0x0AC8, 0x10, 0x0300, 0x0000}, {0x0AC5, 0x17, 0x0007, 0x0002}};
        static const uint16_t table[] = {0x0043, 0x00A7, 0x00D6, 0x00EC, 0x00F6, 0x00FB, 0x00FD, 0x00FF, 0x00BB, 0x0058, 0x0029, 0x0013, 0x0009, 0x0004, 0x0002};
        for (unsigned i = 0; i < sizeof(pre) / sizeof(*pre); i++) phy_modify_paged(pre[i][0], pre[i][1], pre[i][2], pre[i][3]);
        phy_write_paged(0x0AD4, 0x16, 0x00A8); phy_write_paged(0x0AC5, 0x16, 0x01FF); phy_modify_paged(0x0AC8, 0x15, 0x00F0, 0x0030);
        phy_param_8125(0x80A2, 0xFFFF, 0x0153); phy_param_8125(0x809C, 0xFFFF, 0x0153);
        phy_write(0x1F, 0x0A43); phy_write(0x13, 0x81B3);
        for (unsigned i = 0; i < sizeof(table) / sizeof(*table); i++) phy_write(0x14, table[i]);
        for (unsigned i = 0; i < 25; i++) phy_write(0x14, 0);
        phy_write(0x1F, 0);
        phy_param_g(0x8257, 0xFFFF, 0x020F); phy_param_g(0x80EA, 0xFFFF, 0x7843);
        firmware_apply();
        phy_modify_paged(0x0D06, 0x14, 0, 0x2000); phy_param_g(0x81A2, 0, 0x0100);
        phy_modify_paged(0x0B54, 0x16, 0xFF00, 0xDB00); phy_modify_paged(0x0A45, 0x12, 0x0001, 0); phy_modify_paged(0x0A5D, 0x12, 0, 0x0020);
        phy_modify_paged(0x0AD4, 0x17, 0x0010, 0); phy_modify_paged(0x0A86, 0x15, 0x0001, 0); phy_gphy_10m();
        phy_aldps_off(); phy_eee_g(); phy_eee_8125_common(); break;
    }
    case 63: {
        firmware_apply(); phy_gphy_10m();
        phy_modify_paged(0x0AC4, 0x13, 0x00F0, 0x0090); phy_modify_paged(0x0AD3, 0x10, 0x0003, 0x0001);
        phy_param_8125(0x80F5, 0xFFFF, 0x760E); phy_param_8125(0x8107, 0xFFFF, 0x360E); phy_param_8125(0x8551, 0xFF00, 0x0800);
        phy_modify_paged(0x0BF0, 0x10, 0xE000, 0xA000); phy_modify_paged(0x0BF4, 0x13, 0x0F00, 0x0300);
        for (uint16_t p = 0x8044; p <= 0x807A; p = (uint16_t)(p + 6)) phy_param_g(p, 0xFFFF, 0x2417);
        phy_modify_paged(0x0A4C, 0x15, 0, 0x0040); phy_modify_paged(0x0BF8, 0x12, 0xE000, 0xA000);
        phy_modify_paged(0x0A5B, 0x12, 0x8000, 0);           /* legacy force mode off */
        phy_aldps_off(); phy_eee_g(); phy_eee_8125_common(); break;
    }
    case 64: firmware_apply(); phy_gphy_10m(); phy_modify_paged(0x0A5B, 0x12, 0x8000, 0); phy_aldps_off(); phy_eee_g(); phy_eee_8125_common(); break;
    case 70: firmware_apply(); phy_gphy_10m(); phy_modify_paged(0x0A5B, 0x12, 0x8000, 0); phy_aldps_off(); phy_eee_8125_common(); break;
    }
}
/* Power the PHY up, configure it (firmware first), soft-reset it and start autonegotiation of every speed. */
static bool phy_start(void) {
    nic.ocp_base = OCP_STD_PHY_BASE;
    int id1 = phy_read(0x02), id2 = phy_read(0x03);
    if (id1 < 0 || id2 < 0 || id1 == 0xFFFF) { vga_print("[64] Realtek: internal PHY does not answer\n"); return false; }
    int bmcr = phy_read(BMCR); if (bmcr < 0) return false;
    phy_write(BMCR, (uint16_t)(bmcr & ~BMCR_PDOWN));
    phy_config();
    phy_write(0x04, ADVERTISE_ALL);
    phy_modify(0x09, 0x0300, ADVERTISE_1000FULL);
    if (is8125()) {
        int adv = phy_ocp_read(PHY_AN_10GBT_CTRL);
        if (adv >= 0) phy_ocp_write(PHY_AN_10GBT_CTRL, (uint16_t)((adv & ~0x0180) | 0x0080 | (nic.version == 70 ? 0x0100 : 0)));
    }
    bmcr = phy_read(BMCR); if (bmcr < 0) return false;
    phy_write(BMCR, (uint16_t)((bmcr & ~BMCR_PDOWN) | BMCR_RESET | BMCR_ANENABLE | BMCR_ANRESTART));
    for (unsigned i = 0; i < 12 && !bmcr_reset_done(); i++) netdrv_mdelay(50);
    return true;
}
static void phy_power_down(void) {
    nic.ocp_base = OCP_STD_PHY_BASE;
    int bmcr = phy_read(BMCR); if (bmcr >= 0) phy_write(BMCR, (uint16_t)(bmcr | BMCR_PDOWN));
}
/* Link from the PHY: BMSR link bit (latched low: read twice), speed and duplex from PHYSR. */
static void link_read(struct nic_link* link) {
    memset(link, 0, sizeof(*link));
    nic.ocp_base = OCP_STD_PHY_BASE;
    (void)phy_read(BMSR); int status = phy_read(BMSR);
    if (status < 0 || !(status & 0x0004)) return;
    int physr = phy_ocp_read(PHY_PHYSR); if (physr < 0) return;
    link->up = true;
    switch (physr & 0x0630) {
    case 0x0000: link->speed = 10; break; case 0x0010: link->speed = 100; break; case 0x0020: link->speed = 1000; break;
    case 0x0210: link->speed = 2500; break; case 0x0220: link->speed = 5000; break; case 0x0200: link->speed = 10000; break;
    default: link->speed = 0; break;
    }
    link->duplex = physr & 0x0008 ? NIC_DUPLEX_FULL : NIC_DUPLEX_HALF;
}
static void link_update(void) {
    struct nic_link now; link_read(&now);
    if (now.up == nic.link.up && now.speed == nic.link.speed && now.duplex == nic.link.duplex) return;
    nic.link = now;
    netdrv_link_report(&nic.device, &now);
}

/* ---- rings ---- */
static void rings_init(void) {
    for (unsigned i = 0; i < RX_COUNT; i++) {
        struct rtl_desc* d = &nic.rx[i];
        d->address = nic.dma_physical + RX_BUFFER_OFFSET + (uint64_t)i * BUFFER; d->opts2 = 0;
        __asm__ volatile("" ::: "memory");
        d->opts1 = DESC_OWN | (i == RX_COUNT - 1 ? DESC_EOR : 0) | BUFFER;
    }
    for (unsigned i = 0; i < TX_COUNT; i++) { nic.tx[i].opts1 = 0; nic.tx[i].opts2 = 0; nic.tx[i].address = 0; }
    nic.rx_next = nic.tx_next = nic.tx_dirty = 0; nic.discarding = false;
}
static void rx_config_init(void) {
    wr32(RX_CONFIG, nic.version <= 52 ? (1U << 15) | (1U << 14) | (7U << 8) | (1U << 11) :
                    nic.version == 61 ? (8U << 27) | (7U << 8) : (8U << 27) | (7U << 8) | (1U << 11));
}
static bool rx_promiscuous;   /* netdev.c filter: AcceptAllPhys; multicast is always accepted (MAR all ones) */
/* MAC setup after a reset (Linux rtl_hw_start): chip registers, rings, receive filter, interrupts. */
static void hw_start(void) {
    unlock_config();
    aspm_clkreq_off();
    wr16(CPLUS_CMD, nic.cp_cmd);
    if (nic.version == 46) mac_ocp_write(0xE048, 1546); else if (is8125()) wr16(EEE_TXIDLE_8125, 1546);   /* MTU + 14 + 32 */
    start_mac();
    mac_ocp_modify(0xC0AC, 0, 0x1F80);                       /* events that leave ASPM L1 */
    aspm_clkreq_off();
    wr16(RX_MAX_SIZE, BUFFER + 1);
    uint64_t tx = nic.dma_physical + TX_RING_OFFSET, rx = nic.dma_physical + RX_RING_OFFSET;
    wr32(TX_DESC_HIGH, (uint32_t)(tx >> 32)); wr32(TX_DESC_LOW, (uint32_t)tx);
    wr32(RX_DESC_HIGH, (uint32_t)(rx >> 32)); wr32(RX_DESC_LOW, (uint32_t)rx);
    lock_config();
    commit();
    wr8(CHIP_CMD, CMD_TX | CMD_RX);
    rx_config_init();
    wr32(TX_CONFIG, (7U << 8) | (3U << 24) | (nic.version <= 52 ? TXCFG_AUTO_FIFO : 0));
    wr32(RX_CONFIG, rd32(RX_CONFIG) & ~(ACCEPT_ERR_MASK | (is8125() ? RX_VLAN_8125 : 0)));
    wr32(MAR0 + 4, 0xFFFFFFFFU); wr32(MAR0, 0xFFFFFFFFU);
    wr32(RX_CONFIG, (rd32(RX_CONFIG) & ~0x0FU) | ACCEPT_BROADCAST | ACCEPT_MULTICAST | ACCEPT_MINE | (rx_promiscuous ? 0x01U : 0));
    irq_on();
}
/* Receive filter (netdev.c): every multicast frame is accepted in hardware and netdev.c filters exactly in software;
 * promiscuous mode (packet sockets, IFF_PROMISC) sets AcceptAllPhys. Kept across resets and resume (hw_start). */
void rtl8169_nic_rx_mode(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count) {
    (void)all_multicast; (void)groups; (void)count;
    rx_promiscuous = promiscuous;
    if (!nic.active || nic.suspended || nic.dead) return;
    wr32(RX_CONFIG, (rd32(RX_CONFIG) & ~0x01U) | (promiscuous ? 0x01U : 0));
}
static void mac_program(const uint8_t* mac) {
    unlock_config();
    wr32(MAC4, mac[4] | (uint32_t)mac[5] << 8); commit();
    wr32(MAC0, mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24); commit();
    lock_config();
}
static bool mac_valid(const uint8_t* m) { return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]); }
static void mac_read(void) {
    uint8_t* mac = nic.device.mac.addr; memset(mac, 0, ETH_ALEN);
    if (is8125()) for (unsigned i = 0; i < ETH_ALEN; i++) mac[i] = rd8(MAC0_BKP + i);
    else { uint32_t low = eri_read(0xE0), high = eri_read(0xE4); for (unsigned i = 0; i < 4; i++) mac[i] = (uint8_t)(low >> (8 * i)); mac[4] = (uint8_t)high; mac[5] = (uint8_t)(high >> 8); }
    if (!mac_valid(mac)) for (unsigned i = 0; i < ETH_ALEN; i++) mac[i] = rd8(MAC0 + i);
    if (!mac_valid(mac)) {
        if (!native_random(mac, ETH_ALEN)) { uint64_t t = native_clock_ns(); for (unsigned i = 0; i < ETH_ALEN; i++) mac[i] = (uint8_t)(t >> (8 * i)); }
        mac[0] = (uint8_t)((mac[0] & 0xFE) | 0x02);          /* locally administered unicast */
        vga_print("[64] Realtek: no valid MAC address in the chip, using a random one\n");
    }
    mac_program(mac);
}
/* Stop DMA and reset the chip (Linux rtl8169_cleanup). */
static void quiesce(void) {
    irq_off(); irq_ack(0xFFFFFFFFU); commit();
    wr32(RX_CONFIG, rd32(RX_CONFIG) & ~0x3FU);
    rxdv_gate(true); netdrv_mdelay(2);
    if (!hw_reset()) vga_print("[64] Realtek: chip reset did not complete\n");
}
/* Full restart: reset, PHY again when asked (firmware, resume), rings re-armed, MAC started. */
static void restart(bool phy) {
    quiesce();
    if (phy) phy_start();
    rings_init();
    hw_start();
    nic.pending = true; nic.link_event = true;
}

/* ---- data path ---- */
static void tx_reclaim(void) {
    while (nic.tx_dirty != nic.tx_next && !(nic.tx[nic.tx_dirty].opts1 & DESC_OWN)) nic.tx_dirty = (nic.tx_dirty + 1) % TX_COUNT;
}
static void doorbell(void) { if (is8125()) wr16(TX_POLL_8125, 1); else wr8(TX_POLL, NPQ); }
/* RTL8125A/B: short UDP frames to the PTP ports need extra padding (Realtek erratum, r8125 vendor driver). */
static uint16_t ptp_padding(const uint8_t* frame, uint16_t length) {
    if ((nic.version != 61 && nic.version != 63) || length >= 128 + 47 || length < 14 + 20) return length;
    if (frame[12] != 0x08 || frame[13] != 0x00 || (frame[14] >> 4) != 4 || frame[14 + 9] != 17) return length;
    unsigned offset = 14 + (frame[14] & 15U) * 4; if (offset > length) return length;
    unsigned data = length - offset, padded = length;
    if (data >= 4 && data < 47) { unsigned port = (unsigned)frame[offset + 2] << 8 | frame[offset + 3]; if (port == 319 || port == 320) padded = length + 47 - data; }
    if (data < 8 && padded < length + 8 - data) padded = length + 8 - data;
    return (uint16_t)padded;
}
static int transmit(net_device_t* device, const void* packet, uint16_t length) {
    (void)device;
    if (!nic.active || nic.suspended || nic.dead) return -100;
    if (!nic.link.up) return -100;
    if (!packet || !length) return -22;
    if (length > ETH_FRAME_MAX) return -90;
    unsigned slot = nic.tx_next;
    if ((slot + 1) % TX_COUNT == nic.tx_dirty) { tx_reclaim(); if ((slot + 1) % TX_COUNT == nic.tx_dirty) { doorbell(); return -11; } }
    struct rtl_desc* d = &nic.tx[slot];
    uint8_t* buffer = (uint8_t*)(nic.dma + TX_BUFFER_OFFSET + (uint64_t)slot * BUFFER);
    memcpy(buffer, packet, length);
    uint16_t wire = ptp_padding(buffer, length); if (wire < 60) wire = 60;
    if (wire > length) memset(buffer + length, 0, wire - length);
    d->address = nic.dma_physical + TX_BUFFER_OFFSET + (uint64_t)slot * BUFFER; d->opts2 = 0;
    __asm__ volatile("sfence" ::: "memory");
    d->opts1 = DESC_OWN | DESC_FS | DESC_LS | (slot == TX_COUNT - 1 ? DESC_EOR : 0) | wire;
    nic.tx_time[slot] = native_clock_ns();
    nic.tx_next = (slot + 1) % TX_COUNT;
    __asm__ volatile("sfence" ::: "memory");
    doorbell();
    nic.device.stats.tx_packets++; nic.device.stats.tx_bytes += wire;
    return 0;
}
static bool rx_ready(void) { return !(nic.rx[nic.rx_next].opts1 & DESC_OWN); }
static void receive(void) {
    for (unsigned budget = RX_BATCH; budget; budget--) {
        struct rtl_desc* d = &nic.rx[nic.rx_next];
        uint32_t status = d->opts1;
        if (status & DESC_OWN) return;
        __asm__ volatile("lfence" ::: "memory");
        bool first = status & DESC_FS, last = status & DESC_LS;
        uint32_t length = status & 0x3FFF;
        if (nic.discarding) { if (last) nic.discarding = false; nic.device.stats.rx_errors++; }
        else if (status & RX_RES) nic.device.stats.rx_errors++;
        else if (!first || !last) { if (first) nic.discarding = true; nic.device.stats.rx_dropped++; }
        else if (length < ETH_HEADER_LEN + 4 || length - 4 > ETH_FRAME_MAX + 4) nic.device.stats.rx_errors++;
        else {
            nic.device.stats.rx_packets++; nic.device.stats.rx_bytes += length - 4;
            net_deliver_packet((const void*)(nic.dma + RX_BUFFER_OFFSET + (uint64_t)nic.rx_next * BUFFER), (uint16_t)(length - 4));
        }
        d->opts2 = 0;
        __asm__ volatile("sfence" ::: "memory");
        d->opts1 = DESC_OWN | (nic.rx_next == RX_COUNT - 1 ? DESC_EOR : 0) | BUFFER;
        nic.rx_next = (nic.rx_next + 1) % RX_COUNT;
    }
    nic.pending = true;   /* budget used up: come back on the next poll */
}
bool rtl8169_nic_rx_pending(void) {
    if (!nic.active || nic.suspended) return false;
    return nic.pending || (!nic.dead && rx_ready()) || native_clock_ns() >= nic.next_watchdog;
}
/* IRQ context: one status read; mask, acknowledge and flag (the poll drains and unmasks). */
void rtl8169_nic_irq(unsigned vector) {
    if (!nic.active || nic.suspended || vector != nic.irq.vector) return;
    uint32_t status = irq_status();
    if ((status & 0xFFFF) == 0xFFFF || !(status & (nic.irq_mask | INT_RX_OVERFLOW | INT_RX_FIFO))) return;
    if (status & INT_LINK) nic.link_event = true;
    if (status & (INT_RX_OVERFLOW | INT_RX_FIFO)) nic.device.stats.rx_dropped++;
    if (status & INT_TX_ERR) nic.device.stats.tx_errors++;
    irq_off(); nic.pending = true;
    irq_ack(status);
}
static void aspm_link_off(const pci_device_t* d) {   /* ASPM L0s/L1 off on the endpoint (Linux: L1 unless the vendor tested it) */
    uint8_t cap = pci_find_capability(d, 0x10);
    if (!cap) return;
    uint32_t control = pci_read32(d->bus, d->device, d->function, (uint8_t)(cap + 0x10));
    if (control & 3) pci_write32(d->bus, d->device, d->function, (uint8_t)(cap + 0x10), control & ~3U);
}
/* Configuration space after a power transition or a link loss: BARs, command (memory, bus master), ASPM, MSI. */
static void config_restore(void) {
    pci_device_t* d = nic.pci;
    for (unsigned i = 0; i < 6; i++)
        if (pci_read32(d->bus, d->device, d->function, (uint8_t)(PCI_BAR0 + 4 * i)) != nic.bars[i])
            pci_write32(d->bus, d->device, d->function, (uint8_t)(PCI_BAR0 + 4 * i), nic.bars[i]);
    pci_write32(d->bus, d->device, d->function, PCI_COMMAND, (nic.command | 6U) & ~0x400U);
    aspm_link_off(d);
    if (!netdrv_irq_restore(d, &nic.irq)) vga_print("[64] Realtek: interrupt restore failed; polling\n");
}
static void watchdog(uint64_t now) {
    nic.next_watchdog = now + WATCHDOG_NS;
    if (rd32(TX_CONFIG) == 0xFFFFFFFFU) {                    /* the device stopped answering (surprise removal, link loss) */
        if (!nic.dead) { nic.dead = true; nic.dead_since = now; vga_print("[64] Realtek: device not responding (reads all-ones)\n");
                         struct nic_link down = {false, 0, 0}; nic.link = down; netdrv_link_report(&nic.device, &down); }
        return;
    }
    if (nic.dead) {                                          /* it answers again: bring it back up */
        if (now - nic.dead_since < DEAD_RETRY_NS) return;
        nic.dead = false; nic.resets++;
        vga_print("[64] Realtek: device answers again, reinitialising\n");
        config_restore();
        hw_initialize(); mac_program(nic.device.mac.addr); restart(true);
        return;
    }
    if (!nic.irq.vector) nic.link_event = true;             /* polled: look at the PHY every second */
    tx_reclaim();
    if (nic.tx_dirty != nic.tx_next) {
        if (nic.link.up && now - nic.tx_time[nic.tx_dirty] > TX_TIMEOUT_NS) {
            nic.resets++; nic.device.stats.tx_errors++;
            vga_print("[64] Realtek: transmit timeout, resetting the chip\n");
            restart(false);
            return;
        }
        doorbell();                                          /* 8168: a doorbell can be lost when frames follow closely */
    }
}
void rtl8169_nic_poll(void) {
    if (!nic.active || nic.suspended) return;
    nic.pending = false;
    uint64_t now = native_clock_ns();
    if (now >= nic.next_watchdog) watchdog(now);
    if (nic.dead) return;
    /* Acknowledge what is latched (also without an interrupt line): with MSI the chip only signals again once
     * the unmasked status goes from zero to non-zero, so nothing may stay pending across the unmask below. */
    uint32_t status = irq_status();
    if ((status & 0xFFFF) != 0xFFFF && status) { if (status & INT_LINK) nic.link_event = true; irq_ack(status); }
    tx_reclaim();
    receive();
    if (nic.link_event) { nic.link_event = false; link_update(); }
    if (!nic.pending) {                                      /* drained: unmask, then catch a frame that raced the unmask */
        irq_on();
        if (rx_ready()) nic.pending = true;
    }
}

/* ---- probe ---- */
static bool supported_id(const pci_device_t* d) {
    if (d->class_code != PCI_CLASS_NETWORK || d->device_id == 0x8136) return false;   /* 8136: RTL810x fast Ethernet */
    for (unsigned i = 0; i < sizeof(vendor_ids) / sizeof(*vendor_ids); i++) if (d->vendor_id == vendor_ids[i][0] && d->device_id == vendor_ids[i][1]) return true;
    return false;
}
static bool start(pci_device_t* d) {
    nic.pci = d;
    uint64_t size = 0, base = 0; unsigned bar = 0;
    for (; bar < 6 && !base; bar++) { base = pci_bar_memory(d, bar, &size); if (base) break; }
    if (!base || size < 0x100) { vga_print("[64] Realtek: no memory BAR assigned by the firmware\n"); return false; }
#ifndef RTL_TEST_MMIO
    nic.mmio = (volatile uint8_t*)pci_mmio_map(base, size < 0x10000 ? size : 0x10000);
    if (!nic.mmio) return false;
#endif
    for (unsigned i = 0; i < 6; i++) nic.bars[i] = pci_read32(d->bus, d->device, d->function, (uint8_t)(PCI_BAR0 + 4 * i));
    nic.command = pci_enable_dma(d);
    uint32_t txconfig = rd32(TX_CONFIG);
    if (txconfig == 0xFFFFFFFFU) { vga_print("[64] Realtek: register reads fail (all-ones)\n"); goto refused; }
    uint16_t xid = (uint16_t)((txconfig >> 20) & 0xFCF);
    nic.chip = NULL;
    for (unsigned i = 0; i < sizeof(chips) / sizeof(*chips) && !nic.chip; i++) if ((xid & chips[i].mask) == chips[i].xid) nic.chip = &chips[i];
    if (!nic.chip) {
        vga_print("[64] Realtek XID "); netdrv_print_hex(xid, 3);
        vga_print(" (RTL8169/8168b-f/8101 or newer) is not supported by this driver\n");
        goto refused;
    }
    nic.version = nic.chip->version;
    aspm_link_off(d);
    nic.dash = nic.version == 51 || nic.version == 52;
    nic.dash_enabled = nic.dash && (eri_read_type(0x128, ERI_OOB) & 1) != 0;
    nic.cp_cmd = (uint16_t)(rd16(CPLUS_CMD) & CP_MASK & ~(CP_RX_VLAN | CP_RX_CSUM));
    rx_config_init();
    irq_off(); irq_ack(0xFFFFFFFFU); commit();
    hw_initialize();
    if (!hw_reset()) { vga_print("[64] Realtek: chip reset timed out\n"); goto refused; }
    mac_read();
    nic.dma = arena_alloc(DMA_BYTES);                        /* 64-bit DMA: any RAM, above 4 GiB included */
    if (!nic.dma) { vga_print("[64] Realtek: no memory for the descriptor rings\n"); goto refused; }
    nic.dma_physical = virt_to_phys((void*)nic.dma);
    nic.rx = (struct rtl_desc*)(nic.dma + RX_RING_OFFSET); nic.tx = (struct rtl_desc*)(nic.dma + TX_RING_OFFSET);
    if (!phy_start()) goto failed;
    dash_driver(true);
    nic.irq_mask = INT_RX_OK | INT_RX_ERR | INT_TX_ERR | INT_LINK;   /* transmit slots are reclaimed lazily: no TxOK */
    nic.device.name = "eth0"; nic.device.send = transmit; nic.device.link_up = false;
    memset(&nic.link, 0, sizeof(nic.link));
    if (net_register_device(&nic.device) < 0) goto failed;
    netdrv_irq_setup(d, &nic.irq, true, true);
    rings_init();
    hw_start();
    nic.active = true; nic.pending = true; nic.link_event = true; nic.next_watchdog = native_clock_ns() + WATCHDOG_NS;
    vga_print("[64] Realtek "); vga_print(nic.chip->name); vga_print(" (XID "); netdrv_print_hex(xid, 3); vga_print(", MAC version ");
    netdrv_print_dec(nic.version); vga_print(") native DMA ready, IRQ "); vga_print(netdrv_irq_name(nic.irq.kind));
    vga_print(nic.dash ? (nic.dash_enabled ? ", DASH enabled\n" : ", DASH disabled\n") : "\n");
    return true;
failed:
    irq_off(); wr8(CHIP_CMD, 0);
    if (nic.dma) arena_free(nic.dma, DMA_BYTES);
    nic.dma = 0; nic.rx = nic.tx = NULL;
refused:
    pci_restore_command(d, nic.command & ~4U);
    nic.chip = NULL; nic.version = 0;
    return false;
}
bool rtl8169_nic_init(void) {
    pci_device_t* devices = pci_get_devices(); int count = pci_device_count();
    netdrv_booting = true;
    bool ok = false;
    for (int i = 0; i < count && !ok; i++) if (supported_id(&devices[i])) ok = start(&devices[i]);
    netdrv_booting = false;
    return ok;
}
/* After the root volume is mounted (still at boot): the pinned PHY/MAC firmware, applied with a PHY restart. */
void rtl8169_nic_late_init(void) {
    if (!nic.active || !nic.chip->firmware || nic.firmware) return;
    netdrv_booting = true;
    int error = netdrv_firmware_load(nic.chip->firmware, &nic.firmware, &nic.firmware_size);
    if (error) {
        vga_print("[64] Realtek: firmware /lib/firmware/"); vga_print(nic.chip->firmware); vga_print(" "); vga_print(netdrv_error_name(error));
        vga_print("; running without it\n");
    } else {
        vga_print("[64] Realtek: applying firmware "); vga_print(nic.chip->firmware); vga_print("\n");
        if (nic.version == 52) { quiesce(); rings_init(); hw_start(); nic.pending = true; }   /* MAC firmware: applied by hw_start */
        else restart(true);
    }
    netdrv_booting = false;
}
bool rtl8169_nic_link(struct nic_link* link) { if (!nic.active) return false; *link = nic.link; return true; }
const pci_device_t* rtl8169_nic_pci(void) { return nic.active ? nic.pci : NULL; }
/* System sleep: stop DMA and interrupts, power the PHY down, tell DASH firmware; rings and firmware stay. */
void rtl8169_nic_suspend(void) {
    if (!nic.active || nic.suspended) return;
    nic.suspended = true;
    if (!nic.dead) { quiesce(); dash_driver(false); phy_power_down(); }
    pci_device_t* d = nic.pci;                               /* no DMA while asleep */
    pci_write32(d->bus, d->device, d->function, PCI_COMMAND, pci_read16(d->bus, d->device, d->function, PCI_COMMAND) & ~4U);
    struct nic_link down = {false, 0, 0}; bool was_up = nic.link.up; nic.link = down;
    if (was_up) netdrv_link_report(&nic.device, &down); else nic.device.link_up = false;
}
/* Resume: configuration space may have been lost (BARs, command, MSI); restore it, then reinitialise the
 * chip, the MAC address, the PHY (with firmware) and the rings. */
bool rtl8169_nic_resume(void) {
    if (!nic.active || !nic.suspended) return nic.active;
    config_restore();
    nic.suspended = false; nic.dead = false;
    if (rd32(TX_CONFIG) == 0xFFFFFFFFU) { nic.dead = true; nic.dead_since = native_clock_ns(); vga_print("[64] Realtek: device absent after resume\n"); return false; }
    irq_off(); irq_ack(0xFFFFFFFFU);
    hw_initialize();
    if (!hw_reset()) vga_print("[64] Realtek: chip reset timed out after resume\n");
    mac_program(nic.device.mac.addr);
    dash_driver(true);
    phy_start(); rings_init(); hw_start();
    nic.pending = true; nic.link_event = true; nic.next_watchdog = native_clock_ns() + WATCHDOG_NS;
    return true;
}
