/* ============================================================================
 * NexusOS — ARP (Address Resolution Protocol) — Phase 27
 * ============================================================================
 * Resolves IPv4 addresses to Ethernet MAC addresses.
 * Maintains a simple cache with TTL-based aging.
 * ============================================================================ */

#include "arp.h"
#include "ip.h"
#include "net.h"
#include "vga.h"
#include "string.h"

/* ARP cache */
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * arp_init: Clear ARP cache
 * -------------------------------------------------------------------------- */
void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("ARP subsystem initialized\n");
}

/* --------------------------------------------------------------------------
 * arp_cache_add: Add or update an entry in the ARP cache
 * -------------------------------------------------------------------------- */
static void arp_cache_add(uint32_t ip, const uint8_t* mac) {
    /* Look for existing entry or empty slot */
    int free_slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            /* Update existing entry */
            memcpy(arp_cache[i].mac.addr, mac, ETH_ALEN);
            arp_cache[i].timestamp = system_ticks;
            return;
        }
        if (!arp_cache[i].valid && free_slot == -1) {
            free_slot = i;
        }
    }

    /* Use free slot or overwrite oldest */
    if (free_slot == -1) {
        uint32_t oldest_time = 0xFFFFFFFF;
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp_cache[i].timestamp < oldest_time) {
                oldest_time = arp_cache[i].timestamp;
                free_slot = i;
            }
        }
    }

    if (free_slot >= 0) {
        arp_cache[free_slot].ip = ip;
        memcpy(arp_cache[free_slot].mac.addr, mac, ETH_ALEN);
        arp_cache[free_slot].timestamp = system_ticks;
        arp_cache[free_slot].valid = true;
    }
}

/* --------------------------------------------------------------------------
 * arp_send: Send an ARP packet (request or reply)
 * -------------------------------------------------------------------------- */
static void arp_send(uint16_t opcode, const uint8_t* target_mac,
                     uint32_t target_ip) {
    net_device_t* dev = net_get_device(0);
    if (!dev) return;

    /* Build full frame: Ethernet header + ARP packet */
    uint8_t frame[ETH_HEADER_LEN + sizeof(arp_packet_t)];
    eth_header_t* eth = (eth_header_t*)frame;
    arp_packet_t* arp = (arp_packet_t*)(frame + ETH_HEADER_LEN);

    /* Ethernet header */
    memcpy(eth->dest, target_mac, ETH_ALEN);
    memcpy(eth->src, dev->mac.addr, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_ARP);

    /* ARP packet */
    arp->hw_type    = htons(ARP_HW_ETHERNET);
    arp->proto_type = htons(ARP_PROTO_IPV4);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->opcode     = htons(opcode);
    memcpy(arp->sender_mac, dev->mac.addr, ETH_ALEN);
    arp->sender_ip  = htonl(ip_get_addr());
    memcpy(arp->target_mac, target_mac, ETH_ALEN);
    arp->target_ip  = htonl(target_ip);

    net_send_packet(dev, frame, ETH_HEADER_LEN + sizeof(arp_packet_t));
}

/* --------------------------------------------------------------------------
 * arp_handle_packet: Process an incoming ARP packet
 * -------------------------------------------------------------------------- */
void arp_handle_packet(const void* data, uint16_t len) {
    if (len < sizeof(arp_packet_t)) return;

    const arp_packet_t* arp = (const arp_packet_t*)data;

    /* Validate: must be Ethernet/IPv4 */
    if (ntohs(arp->hw_type) != ARP_HW_ETHERNET) return;
    if (ntohs(arp->proto_type) != ARP_PROTO_IPV4) return;

    uint32_t sender_ip = ntohl(arp->sender_ip);
    uint32_t target_ip = ntohl(arp->target_ip);

    /* Always learn the sender's MAC */
    arp_cache_add(sender_ip, arp->sender_mac);

    uint16_t opcode = ntohs(arp->opcode);

    if (opcode == ARP_OP_REQUEST) {
        /* If they're asking for our IP, send a reply */
        if (target_ip == ip_get_addr()) {
            arp_send(ARP_OP_REPLY, arp->sender_mac, sender_ip);
        }
    }
    /* ARP_OP_REPLY: sender already cached above */
}

/* --------------------------------------------------------------------------
 * arp_resolve: Resolve IP to MAC (sends request if not cached)
 * -------------------------------------------------------------------------- */
bool arp_resolve(uint32_t ip, mac_addr_t* mac_out) {
    /* Broadcast address → broadcast MAC */
    if (ip == 0xFFFFFFFF) {
        memcpy(mac_out->addr, MAC_BROADCAST.addr, ETH_ALEN);
        return true;
    }

    /* Check cache first */
    if (arp_lookup(ip, mac_out)) {
        return true;
    }

    /* Not in cache — send ARP request and wait for reply */
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    arp_send(ARP_OP_REQUEST, bcast, ip);

    /* Wait up to ~1 second for ARP reply (18 ticks at 18.2 Hz) */
    uint32_t start = system_ticks;
    while ((system_ticks - start) < 18) {
        net_poll();
        __asm__ volatile("hlt"); /* Process packets then wait for next tick */
        if (arp_lookup(ip, mac_out)) {
            return true;
        }
    }

    return false; /* Timeout — ARP resolution failed */
}

/* --------------------------------------------------------------------------
 * arp_lookup: Check ARP cache (no request sent)
 * -------------------------------------------------------------------------- */
bool arp_lookup(uint32_t ip, mac_addr_t* mac_out) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            /* Check TTL (300 seconds ≈ 5460 ticks at 18.2 Hz) */
            uint32_t age = (system_ticks - arp_cache[i].timestamp) / 18;
            if (age > ARP_CACHE_TTL) {
                arp_cache[i].valid = false;
                continue;
            }
            if (mac_out) {
                memcpy(mac_out->addr, arp_cache[i].mac.addr, ETH_ALEN);
            }
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * arp_announce: Send gratuitous ARP
 * -------------------------------------------------------------------------- */
void arp_announce(void) {
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    arp_send(ARP_OP_REQUEST, bcast, ip_get_addr());
}

/* --------------------------------------------------------------------------
 * arp_get_cache: Return cache for display
 * -------------------------------------------------------------------------- */
const arp_entry_t* arp_get_cache(int* count) {
    if (count) *count = ARP_CACHE_SIZE;
    return arp_cache;
}
