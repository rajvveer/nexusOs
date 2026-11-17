/* ============================================================================
 * NexusOS — RTL8139 NIC Driver (Header) — Phase 26
 * ============================================================================
 * Realtek RTL8139 Fast Ethernet controller driver.
 * PCI vendor 0x10EC, device 0x8139.
 * QEMU's default emulated NIC — ideal for OS development.
 * ============================================================================ */

#ifndef RTL8139_H
#define RTL8139_H

#include "types.h"
#include "net.h"

/* --------------------------------------------------------------------------
 * RTL8139 PCI IDs
 * -------------------------------------------------------------------------- */
#define RTL8139_VENDOR_ID   0x10EC
#define RTL8139_DEVICE_ID   0x8139

/* --------------------------------------------------------------------------
 * RTL8139 Register Offsets (from I/O base)
 * -------------------------------------------------------------------------- */
#define RTL_MAC0            0x00    /* MAC address bytes 0-3 (32-bit) */
#define RTL_MAC4            0x04    /* MAC address bytes 4-5 (16-bit) */
#define RTL_MAR0            0x08    /* Multicast filter 0-3 */
#define RTL_MAR4            0x0C    /* Multicast filter 4-7 */

/* Transmit descriptors (4 descriptors) */
#define RTL_TSD0            0x10    /* TX status descriptor 0 */
#define RTL_TSD1            0x14
#define RTL_TSD2            0x18
#define RTL_TSD3            0x1C
#define RTL_TSAD0           0x20    /* TX start address descriptor 0 */
#define RTL_TSAD1           0x24
#define RTL_TSAD2           0x28
#define RTL_TSAD3           0x2C

/* Receive */
#define RTL_RBSTART         0x30    /* RX buffer start address */
#define RTL_ERBCR           0x34    /* Early RX byte count */
#define RTL_ERSR            0x36    /* Early RX status */

/* Command and status */
#define RTL_CMD             0x37    /* Command register */
#define RTL_CAPR            0x38    /* Current address of packet read */
#define RTL_CBR             0x3A    /* Current buffer address */

/* Interrupt */
#define RTL_IMR             0x3C    /* Interrupt mask register */
#define RTL_ISR             0x3E    /* Interrupt status register */

/* Configuration */
#define RTL_TCR             0x40    /* TX configuration */
#define RTL_RCR             0x44    /* RX configuration */

/* Misc */
#define RTL_CONFIG0         0x51    /* Config register 0 */
#define RTL_CONFIG1         0x52    /* Config register 1 */
#define RTL_MSR             0x58    /* Media status register */
#define RTL_BMCR            0x62    /* Basic mode control register */

/* --------------------------------------------------------------------------
 * Command Register Bits (RTL_CMD)
 * -------------------------------------------------------------------------- */
#define RTL_CMD_RESET       0x10    /* Software reset */
#define RTL_CMD_RX_ENABLE   0x08    /* Receiver enable */
#define RTL_CMD_TX_ENABLE   0x04    /* Transmitter enable */
#define RTL_CMD_BUF_EMPTY   0x01    /* RX buffer empty */

/* --------------------------------------------------------------------------
 * Interrupt Status/Mask Bits (RTL_ISR / RTL_IMR)
 * -------------------------------------------------------------------------- */
#define RTL_INT_ROK         0x0001  /* Receive OK */
#define RTL_INT_RER         0x0002  /* Receive error */
#define RTL_INT_TOK         0x0004  /* Transmit OK */
#define RTL_INT_TER         0x0008  /* Transmit error */
#define RTL_INT_RXOVW       0x0010  /* RX buffer overflow */
#define RTL_INT_PUN         0x0020  /* Packet underrun / link change */
#define RTL_INT_FOVW        0x0040  /* RX FIFO overflow */
#define RTL_INT_TIMEOUT     0x4000  /* Time out */
#define RTL_INT_SERR        0x8000  /* System error */

/* --------------------------------------------------------------------------
 * TX Status Register Bits
 * -------------------------------------------------------------------------- */
#define RTL_TX_OWN          0x2000  /* DMA operation completed (set by NIC) */
#define RTL_TX_TUN          0x4000  /* TX FIFO underrun */
#define RTL_TX_TOK          0x8000  /* TX completed OK */
#define RTL_TX_SIZE_MASK    0x1FFF  /* Packet size bits [12:0] */

/* --------------------------------------------------------------------------
 * RX Configuration Bits (RTL_RCR)
 * -------------------------------------------------------------------------- */
#define RTL_RCR_AAP         0x0001  /* Accept all packets */
#define RTL_RCR_APM         0x0002  /* Accept physical match */
#define RTL_RCR_AM          0x0004  /* Accept multicast */
#define RTL_RCR_AB          0x0008  /* Accept broadcast */
#define RTL_RCR_WRAP        0x0080  /* Wrap at end of RX buffer */
#define RTL_RCR_RBLEN_8K    0x0000  /* RX buffer length: 8K + 16 */
#define RTL_RCR_RBLEN_16K   0x0800  /* RX buffer length: 16K + 16 */
#define RTL_RCR_RBLEN_32K   0x1000  /* RX buffer length: 32K + 16 */
#define RTL_RCR_RBLEN_64K   0x1800  /* RX buffer length: 64K + 16 */

/* --------------------------------------------------------------------------
 * TX Configuration Bits (RTL_TCR)
 * -------------------------------------------------------------------------- */
#define RTL_TCR_IFG96       (3 << 24)   /* Interframe gap time = 9.6us */
#define RTL_TCR_MXDMA_2048  (7 << 8)    /* Max DMA burst = 2048 bytes */

/* --------------------------------------------------------------------------
 * RX Packet Header (prepended to each packet in RX buffer)
 * -------------------------------------------------------------------------- */
#define RTL_RX_ROK          0x0001  /* Receive OK */
#define RTL_RX_FAE          0x0002  /* Frame alignment error */
#define RTL_RX_CRC          0x0004  /* CRC error */
#define RTL_RX_LONG         0x0008  /* Long packet (> 4K) */
#define RTL_RX_RUNT         0x0010  /* Runt packet (< 64 bytes) */
#define RTL_RX_ISE          0x0020  /* Invalid symbol error */

/* --------------------------------------------------------------------------
 * Buffer sizes
 * -------------------------------------------------------------------------- */
#define RTL_RX_BUF_SIZE     (8192 + 16 + ETH_MTU)  /* 8K + header + padding */
#define RTL_TX_BUF_SIZE     ETH_FRAME_MAX
#define RTL_TX_DESC_COUNT   4       /* 4 TX descriptor slots */

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize RTL8139 driver (registers with driver framework) */
void rtl8139_init(void);

/* Send an Ethernet frame (called via net_device_t->send) */
int rtl8139_send(net_device_t* dev, const void* data, uint16_t len);

/* Get current MAC address */
void rtl8139_get_mac(mac_addr_t* mac);

/* Check if RTL8139 is detected and active */
bool rtl8139_is_active(void);

#endif /* RTL8139_H */
