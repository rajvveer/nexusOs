/* ============================================================================
 * NexusOS — Network Subsystem (Header) — Phase 26
 * ============================================================================
 * Network device abstraction, Ethernet frame types, and packet buffers.
 * ============================================================================ */

#ifndef NET_H
#define NET_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define ETH_ALEN        6       /* Ethernet address (MAC) length */
#define ETH_MTU         1500    /* Maximum transmission unit */
#define ETH_HEADER_LEN  14      /* dest(6) + src(6) + ethertype(2) */
#define ETH_FRAME_MAX   (ETH_HEADER_LEN + ETH_MTU)

#define NET_RX_RING_SIZE 8      /* RX packet ring buffer slots */
#define NET_MAX_DEVICES  2      /* Max registered network devices */

/* Ethernet types (big-endian on wire) */
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IPV6   0x86DD

/* --------------------------------------------------------------------------
 * MAC address
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t addr[ETH_ALEN];
} mac_addr_t;

/* Broadcast MAC */
extern const mac_addr_t MAC_BROADCAST;

/* --------------------------------------------------------------------------
 * Ethernet frame header
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  dest[ETH_ALEN];    /* Destination MAC */
    uint8_t  src[ETH_ALEN];     /* Source MAC */
    uint16_t ethertype;         /* Protocol type (big-endian) */
} __attribute__((packed)) eth_header_t;

/* --------------------------------------------------------------------------
 * Packet buffer
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  data[ETH_FRAME_MAX];
    uint16_t length;
    bool     valid;
} net_packet_t;

/* --------------------------------------------------------------------------
 * Network device statistics
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t rx_dropped;
} net_stats_t;

/* --------------------------------------------------------------------------
 * Network device abstraction
 * -------------------------------------------------------------------------- */
typedef struct net_device {
    const char* name;               /* Device name (e.g. "eth0") */
    mac_addr_t  mac;                /* Hardware MAC address */
    bool        link_up;            /* Link status */
    bool        active;             /* Device registered and active */
    net_stats_t stats;              /* Traffic statistics */

    /* Driver callbacks */
    int  (*send)(struct net_device* dev, const void* data, uint16_t len);
    void (*get_mac)(struct net_device* dev, mac_addr_t* mac);

    /* Private driver data */
    void* priv;
} net_device_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize the network subsystem */
void net_init(void);

/* Register/get network devices */
int           net_register_device(net_device_t* dev);
net_device_t* net_get_device(int index);
int           net_device_count(void);

/* Send a raw Ethernet frame */
int net_send_packet(net_device_t* dev, const void* data, uint16_t len);

/* Called by NIC driver IRQ to queue a received packet */
void net_receive_packet(const void* data, uint16_t len);

/* Get a received packet (returns NULL if none available) */
net_packet_t* net_get_rx_packet(void);

/* Poll: process queued RX packets (call from timer tick, NOT IRQ context) */
void net_poll(void);

/* Utility: format MAC address as string "XX:XX:XX:XX:XX:XX" */
void net_mac_to_string(const mac_addr_t* mac, char* buf);

/* Byte order helpers (x86 is little-endian, network is big-endian) */
static inline uint16_t htons(uint16_t val) {
    return (val >> 8) | (val << 8);
}
static inline uint16_t ntohs(uint16_t val) {
    return htons(val);
}
static inline uint32_t htonl(uint32_t val) {
    return ((val >> 24) & 0xFF) |
           ((val >> 8)  & 0xFF00) |
           ((val << 8)  & 0xFF0000) |
           ((val << 24) & 0xFF000000);
}
static inline uint32_t ntohl(uint32_t val) {
    return htonl(val);
}

#endif /* NET_H */
