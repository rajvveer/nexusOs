/* ============================================================================
 * NexusOS — Network Subsystem (Implementation) — Phase 27
 * ============================================================================
 * Manages network devices, packet buffering, and protocol dispatch.
 * Protocol dispatch is done via net_poll() from timer context,
 * NOT from IRQ context, to avoid blocking mouse/keyboard IRQs.
 * ============================================================================ */

#include "net.h"
#include "arp.h"
#include "ip.h"
#include "vga.h"
#include "string.h"

/* Broadcast MAC address */
const mac_addr_t MAC_BROADCAST = {{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }};

/* Registered network devices */
static net_device_t* devices[NET_MAX_DEVICES];
static int num_devices = 0;

/* RX packet ring buffer */
static net_packet_t rx_ring[NET_RX_RING_SIZE];
static int rx_head = 0;    /* Next slot to write */
static int rx_tail = 0;    /* Next slot to read */

/* --------------------------------------------------------------------------
 * net_init: Initialize network subsystem
 * -------------------------------------------------------------------------- */
void net_init(void) {
    memset(devices, 0, sizeof(devices));
    memset(rx_ring, 0, sizeof(rx_ring));
    num_devices = 0;
    rx_head = 0;
    rx_tail = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Network subsystem initialized\n");
}

/* --------------------------------------------------------------------------
 * net_register_device: Register a NIC
 * -------------------------------------------------------------------------- */
int net_register_device(net_device_t* dev) {
    if (!dev || num_devices >= NET_MAX_DEVICES) return -1;

    devices[num_devices++] = dev;
    dev->active = true;

    /* Print device info */
    vga_print("     ");
    vga_print(dev->name);
    vga_print(" MAC=");

    char mac_str[18];
    net_mac_to_string(&dev->mac, mac_str);
    vga_print_color(mac_str, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("\n");

    return 0;
}

/* --------------------------------------------------------------------------
 * net_get_device / net_device_count
 * -------------------------------------------------------------------------- */
net_device_t* net_get_device(int index) {
    if (index < 0 || index >= num_devices) return NULL;
    return devices[index];
}

int net_device_count(void) {
    return num_devices;
}

/* --------------------------------------------------------------------------
 * net_send_packet: Send raw Ethernet frame through device
 * -------------------------------------------------------------------------- */
int net_send_packet(net_device_t* dev, const void* data, uint16_t len) {
    if (!dev || !dev->active || !dev->send) return -1;
    if (len > ETH_FRAME_MAX) return -1;

    int ret = dev->send(dev, data, len);
    if (ret == 0) {
        dev->stats.tx_packets++;
        dev->stats.tx_bytes += len;
    } else {
        dev->stats.tx_errors++;
    }
    return ret;
}

/* --------------------------------------------------------------------------
 * net_receive_packet: Queue an incoming packet from NIC IRQ handler
 * -------------------------------------------------------------------------- */
void net_receive_packet(const void* data, uint16_t len) {
    if (!data || len == 0 || len > ETH_FRAME_MAX) return;

    /* Check if ring is full */
    int next_head = (rx_head + 1) % NET_RX_RING_SIZE;
    if (next_head == rx_tail) {
        /* Ring full — drop packet */
        if (num_devices > 0 && devices[0]) {
            devices[0]->stats.rx_dropped++;
        }
        return;
    }

    memcpy(rx_ring[rx_head].data, data, len);
    rx_ring[rx_head].length = len;
    rx_ring[rx_head].valid = true;
    rx_head = next_head;

    /* Update stats on first device */
    if (num_devices > 0 && devices[0]) {
        devices[0]->stats.rx_packets++;
        devices[0]->stats.rx_bytes += len;
    }

    /* NOTE: Protocol dispatch is NOT done here (IRQ context).
     * It is done in net_poll() called from the timer tick,
     * so we don't block mouse/keyboard IRQs. */
}

/* --------------------------------------------------------------------------
 * net_poll: Process queued packets (called from timer tick, NOT IRQ context)
 * This is the safe place to do heavy protocol processing.
 * -------------------------------------------------------------------------- */
void net_poll(void) {
    /* Process up to 4 packets per tick to avoid hogging the CPU */
    int processed = 0;
    while (processed < 4) {
        net_packet_t* pkt = net_get_rx_packet();
        if (!pkt) break;

        if (pkt->length >= ETH_HEADER_LEN) {
            const eth_header_t* eth = (const eth_header_t*)pkt->data;
            uint16_t ethertype = ntohs(eth->ethertype);
            const uint8_t* payload = pkt->data + ETH_HEADER_LEN;
            uint16_t payload_len = pkt->length - ETH_HEADER_LEN;

            switch (ethertype) {
                case ETH_TYPE_ARP:
                    arp_handle_packet(payload, payload_len);
                    break;
                case ETH_TYPE_IPV4:
                    ip_handle_packet(payload, payload_len);
                    break;
                default:
                    break;
            }
        }
        processed++;
    }
}

/* --------------------------------------------------------------------------
 * net_get_rx_packet: Dequeue a received packet (returns NULL if empty)
 * -------------------------------------------------------------------------- */
net_packet_t* net_get_rx_packet(void) {
    if (rx_tail == rx_head) return NULL;

    net_packet_t* pkt = &rx_ring[rx_tail];
    if (!pkt->valid) return NULL;

    rx_tail = (rx_tail + 1) % NET_RX_RING_SIZE;
    pkt->valid = false;
    return pkt;
}

/* --------------------------------------------------------------------------
 * net_mac_to_string: Format MAC address as "XX:XX:XX:XX:XX:XX"
 * -------------------------------------------------------------------------- */
void net_mac_to_string(const mac_addr_t* mac, char* buf) {
    static const char hex[] = "0123456789ABCDEF";

    for (int i = 0; i < ETH_ALEN; i++) {
        buf[i * 3]     = hex[(mac->addr[i] >> 4) & 0xF];
        buf[i * 3 + 1] = hex[mac->addr[i] & 0xF];
        buf[i * 3 + 2] = (i < ETH_ALEN - 1) ? ':' : '\0';
    }
}
