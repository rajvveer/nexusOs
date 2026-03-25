/* ============================================================================
 * NexusOS — RTL8139 NIC Driver (Implementation) — Phase 26
 * ============================================================================
 * Realtek RTL8139 Fast Ethernet controller driver for QEMU.
 *
 * Features:
 *   - PCI auto-detection (vendor 0x10EC, device 0x8139)
 *   - Bus mastering enable
 *   - DMA receive buffer (8K ring) and 4 x TX descriptors
 *   - IRQ-driven receive with packet queuing
 *   - Ethernet frame send/receive
 *   - MAC address reading
 *
 * References:
 *   - RTL8139 Programming Guide
 *   - OSDev Wiki: RTL8139
 * ============================================================================ */

#include "rtl8139.h"
#include "net.h"
#include "pci.h"
#include "driver.h"
#include "port.h"
#include "paging.h"
#include "memory.h"
#include "heap.h"
#include "vga.h"
#include "string.h"

/* --------------------------------------------------------------------------
 * Driver State
 * -------------------------------------------------------------------------- */
static struct {
    bool        active;             /* Driver initialized and running */
    uint16_t    io_base;            /* PCI I/O base address */
    uint8_t     irq;                /* IRQ line */
    mac_addr_t  mac;                /* Hardware MAC address */

    /* RX buffer (8K + 16 + 1500 extra for wrap-around safety) */
    uint8_t*    rx_buffer;
    uint32_t    rx_offset;          /* Current read offset in RX ring */

    /* TX descriptors (4 slots, round-robin) */
    uint8_t*    tx_buffers[RTL_TX_DESC_COUNT];
    uint8_t     tx_cur;             /* Current TX descriptor index */

    /* Network device handle */
    net_device_t net_dev;
} rtl;

/* --------------------------------------------------------------------------
 * I/O helpers — read/write RTL8139 registers via I/O ports
 * -------------------------------------------------------------------------- */
static inline uint8_t rtl_inb(uint16_t reg) {
    return port_byte_in(rtl.io_base + reg);
}
static inline uint16_t rtl_inw(uint16_t reg) {
    return port_word_in(rtl.io_base + reg);
}
static inline uint32_t rtl_inl(uint16_t reg) {
    return port_dword_in(rtl.io_base + reg);
}
static inline void rtl_outb(uint16_t reg, uint8_t val) {
    port_byte_out(rtl.io_base + reg, val);
}
static inline void rtl_outw(uint16_t reg, uint16_t val) {
    port_word_out(rtl.io_base + reg, val);
}
static inline void rtl_outl(uint16_t reg, uint32_t val) {
    port_dword_out(rtl.io_base + reg, val);
}

/* --------------------------------------------------------------------------
 * rtl8139_read_mac: Read the 6-byte MAC address from registers
 * -------------------------------------------------------------------------- */
static void rtl8139_read_mac(void) {
    for (int i = 0; i < ETH_ALEN; i++) {
        rtl.mac.addr[i] = rtl_inb(RTL_MAC0 + i);
    }
}

/* --------------------------------------------------------------------------
 * rtl8139_reset: Software reset the NIC
 * -------------------------------------------------------------------------- */
static void rtl8139_reset(void) {
    rtl_outb(RTL_CMD, RTL_CMD_RESET);

    /* Wait for reset to complete (RST bit clears itself) */
    int timeout = 1000;
    while ((rtl_inb(RTL_CMD) & RTL_CMD_RESET) && timeout > 0) {
        io_wait();
        timeout--;
    }
}

/* --------------------------------------------------------------------------
 * rtl8139_handle_rx: Process received packets from RX ring buffer
 * -------------------------------------------------------------------------- */
static void rtl8139_handle_rx(void) {
    while (!(rtl_inb(RTL_CMD) & RTL_CMD_BUF_EMPTY)) {
        /* Read the 4-byte packet header at current offset */
        uint8_t* buf = rtl.rx_buffer + rtl.rx_offset;
        uint16_t status = (uint16_t)(buf[0] | (buf[1] << 8));
        uint16_t length = (uint16_t)(buf[2] | (buf[3] << 8));

        /* Validate packet */
        if (!(status & RTL_RX_ROK)) {
            /* Bad packet — skip and reset */
            rtl.net_dev.stats.rx_errors++;
            break;
        }

        /* Sanity check length */
        if (length < 8 || length > ETH_FRAME_MAX + 4) {
            /* Invalid length — skip */
            rtl.net_dev.stats.rx_errors++;
            break;
        }

        /* Strip 4-byte header and 4-byte CRC from length */
        uint16_t pkt_len = length - 4; /* subtract CRC */
        uint8_t* pkt_data = buf + 4;   /* skip 4-byte RX header */

        /* Queue the packet into the network subsystem */
        if (pkt_len >= ETH_HEADER_LEN && pkt_len <= ETH_FRAME_MAX) {
            net_receive_packet(pkt_data, pkt_len);
        }

        /* Advance offset: header(4) + length, aligned to 4 bytes, plus 4 */
        rtl.rx_offset = (rtl.rx_offset + length + 4 + 3) & ~3;
        rtl.rx_offset %= (8192);  /* Wrap within 8K buffer */

        /* Update CAPR (must subtract 0x10 per RTL8139 spec) */
        rtl_outw(RTL_CAPR, (uint16_t)(rtl.rx_offset - 16));
    }
}

/* --------------------------------------------------------------------------
 * rtl8139_irq_handler: Handle RTL8139 interrupts
 * -------------------------------------------------------------------------- */
static void rtl8139_irq_handler(struct registers* regs) {
    (void)regs;

    if (!rtl.active) return;

    uint16_t status = rtl_inw(RTL_ISR);
    if (status == 0) return; /* Not our interrupt */

    /* Acknowledge all pending interrupts */
    rtl_outw(RTL_ISR, status);

    /* Receive OK */
    if (status & RTL_INT_ROK) {
        rtl8139_handle_rx();
    }

    /* Transmit OK */
    if (status & RTL_INT_TOK) {
        /* TX completed — nothing special to do, stats updated in send */
    }

    /* Receive error */
    if (status & RTL_INT_RER) {
        rtl.net_dev.stats.rx_errors++;
    }

    /* Transmit error */
    if (status & RTL_INT_TER) {
        rtl.net_dev.stats.tx_errors++;
    }

    /* RX buffer overflow — reset RX */
    if (status & RTL_INT_RXOVW) {
        rtl.net_dev.stats.rx_errors++;
        /* Re-enable receiver */
        rtl_outb(RTL_CMD, RTL_CMD_RX_ENABLE | RTL_CMD_TX_ENABLE);
    }
}

/* --------------------------------------------------------------------------
 * rtl8139_send: Transmit an Ethernet frame
 * -------------------------------------------------------------------------- */
int rtl8139_send(net_device_t* dev, const void* data, uint16_t len) {
    (void)dev;

    if (!rtl.active || !data || len == 0) return -1;
    if (len > RTL_TX_BUF_SIZE) return -1;

    /* Wait for current TX descriptor to be available */
    uint16_t tsd_reg = RTL_TSD0 + (rtl.tx_cur * 4);
    uint16_t tsad_reg = RTL_TSAD0 + (rtl.tx_cur * 4);

    /* Check if the descriptor is owned by us (OWN bit set = DMA done) */
    int timeout = 1000;
    while (timeout > 0) {
        uint32_t tsd = rtl_inl(tsd_reg);
        if (tsd & RTL_TX_OWN) break; /* NIC finished with this descriptor */
        if (tsd & RTL_TX_TOK) break; /* TX completed OK */
        io_wait();
        timeout--;
    }

    /* Copy data to TX buffer */
    memcpy(rtl.tx_buffers[rtl.tx_cur], data, len);

    /* Pad to minimum Ethernet frame size (60 bytes, NIC adds 4-byte CRC) */
    if (len < 60) {
        memset(rtl.tx_buffers[rtl.tx_cur] + len, 0, 60 - len);
        len = 60;
    }

    /* Write TX buffer address */
    rtl_outl(tsad_reg, (uint32_t)(rtl.tx_buffers[rtl.tx_cur]));

    /* Write TX status: clear OWN bit, set size — starts DMA transfer */
    rtl_outl(tsd_reg, (uint32_t)len & RTL_TX_SIZE_MASK);

    /* Advance to next descriptor (round-robin) */
    rtl.tx_cur = (rtl.tx_cur + 1) % RTL_TX_DESC_COUNT;

    return 0;
}

/* --------------------------------------------------------------------------
 * rtl8139_probe: PCI probe callback — initialize the NIC hardware
 * -------------------------------------------------------------------------- */
static int rtl8139_probe(pci_device_t* dev) {
    /* Extract I/O base address from PCI BARs */
    rtl.io_base = 0;
    for (int i = 0; i < 6; i++) {
        if (dev->bar[i] && (dev->bar[i] & 1)) { /* I/O space BAR */
            rtl.io_base = (uint16_t)(dev->bar[i] & 0xFFFC);
            break;
        }
    }
    
    if (rtl.io_base == 0) {
        vga_print_color("[RTL8139] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("Failed to find I/O base address in BARs\n");
        return -1;
    }
    rtl.irq = dev->irq_line;

    if (rtl.io_base == 0) {
        vga_print_color("[FAIL] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("RTL8139: Invalid I/O base\n");
        return -1;
    }

    /* Enable PCI bus mastering (required for DMA) */
    uint32_t cmd = pci_read32(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= (1 << 2);  /* Bus Master Enable */
    cmd |= (1 << 0);  /* I/O Space Enable */
    pci_write32(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);

    /* Power on: write 0x00 to Config1 register */
    rtl_outb(RTL_CONFIG1, 0x00);

    /* Software reset */
    rtl8139_reset();

    /* Read MAC address */
    rtl8139_read_mac();

    /* --- Allocate DMA buffers --- */

    /* RX buffer: 8K + 16 + 1500 (for wrap safety) — must be physically contiguous */
    /* Allocate 3 pages (12KB) which covers 8K+16+1500 = ~9716 bytes */
    uint32_t rx_phys = pmm_alloc_page();
    uint32_t rx_phys2 = pmm_alloc_page();
    uint32_t rx_phys3 = pmm_alloc_page();
    if (!rx_phys || !rx_phys2 || !rx_phys3) {
        vga_print_color("[FAIL] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("RTL8139: Cannot allocate RX buffer\n");
        return -1;
    }

    /* Use identity-mapped physical addresses (first 16MB is identity-mapped) */
    rtl.rx_buffer = (uint8_t*)rx_phys;
    memset(rtl.rx_buffer, 0, RTL_RX_BUF_SIZE);
    rtl.rx_offset = 0;

    /* TX buffers: 4 x 2K each — allocate from pages */
    uint32_t tx_phys = pmm_alloc_page();
    uint32_t tx_phys2 = pmm_alloc_page();
    if (!tx_phys || !tx_phys2) {
        vga_print_color("[FAIL] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("RTL8139: Cannot allocate TX buffers\n");
        return -1;
    }
    rtl.tx_buffers[0] = (uint8_t*)tx_phys;
    rtl.tx_buffers[1] = (uint8_t*)(tx_phys + 2048);
    rtl.tx_buffers[2] = (uint8_t*)tx_phys2;
    rtl.tx_buffers[3] = (uint8_t*)(tx_phys2 + 2048);
    rtl.tx_cur = 0;

    for (int i = 0; i < RTL_TX_DESC_COUNT; i++) {
        memset(rtl.tx_buffers[i], 0, RTL_TX_BUF_SIZE);
    }

    /* --- Configure the NIC --- */

    /* Set RX buffer address */
    rtl_outl(RTL_RBSTART, (uint32_t)rtl.rx_buffer);

    /* Set interrupt mask: enable ROK, TOK, RER, TER, RXOVW */
    rtl_outw(RTL_IMR, RTL_INT_ROK | RTL_INT_TOK | RTL_INT_RER |
                       RTL_INT_TER | RTL_INT_RXOVW);

    /* Configure RX: accept broadcast + physical match + multicast, 8K buffer, wrap */
    rtl_outl(RTL_RCR, RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB |
                       RTL_RCR_WRAP | RTL_RCR_RBLEN_8K);

    /* Configure TX: IFG = standard, Max DMA burst = 2048 */
    rtl_outl(RTL_TCR, RTL_TCR_IFG96 | RTL_TCR_MXDMA_2048);

    /* Enable RX and TX */
    rtl_outb(RTL_CMD, RTL_CMD_RX_ENABLE | RTL_CMD_TX_ENABLE);

    /* Clear any pending interrupts */
    rtl_outw(RTL_ISR, 0xFFFF);

    /* Register IRQ handler */
    if (rtl.irq > 0 && rtl.irq < 16) {
        irq_register_handler(rtl.irq, rtl8139_irq_handler);
    }

    /* --- Register with network subsystem --- */
    memset(&rtl.net_dev, 0, sizeof(net_device_t));
    rtl.net_dev.name = "eth0";
    memcpy(&rtl.net_dev.mac, &rtl.mac, sizeof(mac_addr_t));
    rtl.net_dev.link_up = true;
    rtl.net_dev.send = rtl8139_send;
    rtl.net_dev.priv = &rtl;

    net_register_device(&rtl.net_dev);

    rtl.active = true;

    /* Print success */
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("RTL8139: IRQ=");
    char buf[8];
    int_to_str(rtl.irq, buf);
    vga_print(buf);
    vga_print(" IO=0x");
    hex_to_str(rtl.io_base, buf);
    vga_print(buf);
    vga_print("\n");

    return 0;
}

/* --------------------------------------------------------------------------
 * RTL8139 Driver Descriptor (for driver framework)
 * -------------------------------------------------------------------------- */
static driver_t rtl8139_driver = {
    .name        = "rtl8139",
    .pci_vendor  = RTL8139_VENDOR_ID,
    .pci_device  = RTL8139_DEVICE_ID,
    .pci_class   = PCI_CLASS_NETWORK,
    .pci_subclass = 0,
    .init        = NULL,
    .probe       = rtl8139_probe,
    .remove      = NULL,
    .active      = false,
    .probed      = false,
};

/* --------------------------------------------------------------------------
 * rtl8139_init: Register the RTL8139 driver with the framework
 * -------------------------------------------------------------------------- */
void rtl8139_init(void) {
    memset(&rtl, 0, sizeof(rtl));
    driver_register(&rtl8139_driver);
}

/* --------------------------------------------------------------------------
 * rtl8139_get_mac: Copy the current MAC address
 * -------------------------------------------------------------------------- */
void rtl8139_get_mac(mac_addr_t* mac) {
    if (mac) {
        memcpy(mac, &rtl.mac, sizeof(mac_addr_t));
    }
}

/* --------------------------------------------------------------------------
 * rtl8139_is_active: Check if NIC is up
 * -------------------------------------------------------------------------- */
bool rtl8139_is_active(void) {
    return rtl.active;
}
