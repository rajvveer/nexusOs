/* ============================================================================
 * NexusOS — ARP (Address Resolution Protocol) Header — Phase 27
 * ============================================================================
 * Maps IPv4 addresses to Ethernet MAC addresses.
 * RFC 826 implementation.
 * ============================================================================ */

#ifndef ARP_H
#define ARP_H

#include "types.h"
#include "net.h"

/* --------------------------------------------------------------------------
 * ARP Constants
 * -------------------------------------------------------------------------- */
#define ARP_HW_ETHERNET     1       /* Hardware type: Ethernet */
#define ARP_PROTO_IPV4      0x0800  /* Protocol type: IPv4 */

#define ARP_OP_REQUEST      1       /* ARP request */
#define ARP_OP_REPLY        2       /* ARP reply */

#define ARP_CACHE_SIZE      16      /* Max ARP cache entries */
#define ARP_CACHE_TTL       300     /* Cache entry lifetime in seconds */

/* --------------------------------------------------------------------------
 * ARP Packet (Ethernet + IPv4)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t hw_type;           /* Hardware type (1 = Ethernet) */
    uint16_t proto_type;        /* Protocol type (0x0800 = IPv4) */
    uint8_t  hw_len;            /* Hardware address length (6) */
    uint8_t  proto_len;         /* Protocol address length (4) */
    uint16_t opcode;            /* Operation (1=request, 2=reply) */
    uint8_t  sender_mac[6];     /* Sender hardware address */
    uint32_t sender_ip;         /* Sender protocol address */
    uint8_t  target_mac[6];     /* Target hardware address */
    uint32_t target_ip;         /* Target protocol address */
} __attribute__((packed)) arp_packet_t;

/* --------------------------------------------------------------------------
 * ARP Cache Entry
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t   ip;              /* IPv4 address */
    mac_addr_t mac;             /* Corresponding MAC address */
    uint32_t   timestamp;       /* When entry was created (ticks) */
    bool       valid;           /* Entry is active */
} arp_entry_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize ARP subsystem */
void arp_init(void);

/* Handle an incoming ARP packet (called from net layer) */
void arp_handle_packet(const void* data, uint16_t len);

/* Resolve an IP address to MAC address (may send ARP request) */
/* Returns true if resolved, false if pending */
bool arp_resolve(uint32_t ip, mac_addr_t* mac_out);

/* Look up IP in cache (no request sent) */
bool arp_lookup(uint32_t ip, mac_addr_t* mac_out);

/* Send a gratuitous ARP announcement */
void arp_announce(void);

/* Get the ARP cache for display */
const arp_entry_t* arp_get_cache(int* count);

#endif /* ARP_H */
