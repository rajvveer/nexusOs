/* ============================================================================
 * NexusOS — IPv4 (Internet Protocol) — Phase 27
 * ============================================================================
 * IPv4 packet construction, validation, routing, and protocol demux.
 * Default configuration: QEMU user-mode networking (10.0.2.x).
 * ============================================================================ */

#include "ip.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "net.h"
#include "vga.h"
#include "string.h"

/* Network configuration (QEMU user-mode defaults) */
static net_config_t config = {
    .ip          = 0x0A00020F,  /* 10.0.2.15 */
    .gateway     = 0x0A000202,  /* 10.0.2.2 */
    .subnet_mask = 0xFFFFFF00,  /* 255.255.255.0 */
    .dns         = 0x0A000203   /* 10.0.2.3 */
};

/* Packet ID counter */
static uint16_t ip_id_counter = 0;

/* --------------------------------------------------------------------------
 * ip_init: Initialize IP subsystem
 * -------------------------------------------------------------------------- */
void ip_init(void) {
    ip_id_counter = 0;

    char ip_str[16];
    ip_to_string(config.ip, ip_str);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("IPv4 initialized: ");
    vga_print_color(ip_str, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("\n");
}

/* --------------------------------------------------------------------------
 * ip_checksum: One's complement checksum (RFC 1071)
 * -------------------------------------------------------------------------- */
uint16_t ip_checksum(const void* data, uint16_t len) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    /* Handle odd byte */
    if (len == 1) {
        sum += *(const uint8_t*)ptr;
    }
    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

/* --------------------------------------------------------------------------
 * ip_handle_packet: Process incoming IPv4 packet
 * -------------------------------------------------------------------------- */
void ip_handle_packet(const void* data, uint16_t len) {
    if (len < IP_HEADER_LEN) return;

    const ip_header_t* hdr = (const ip_header_t*)data;

    /* Validate version */
    uint8_t version = (hdr->ver_ihl >> 4) & 0xF;
    if (version != IP_VERSION_4) return;

    /* Get header length (in 32-bit words) */
    uint8_t ihl = hdr->ver_ihl & 0xF;
    uint16_t hdr_len = (uint16_t)(ihl * 4);
    if (hdr_len < IP_HEADER_LEN || hdr_len > len) return;

    /* Validate total length */
    uint16_t total_len = ntohs(hdr->total_len);
    if (total_len > len) return;

    /* Verify checksum */
    if (ip_checksum(data, hdr_len) != 0) return;

    /* Check destination: must be for us or broadcast */
    uint32_t dst = ntohl(hdr->dst_ip);
    if (dst != config.ip && dst != 0xFFFFFFFF &&
        dst != (config.ip | ~config.subnet_mask)) {
        return; /* Not for us */
    }

    /* Extract payload */
    const uint8_t* payload = (const uint8_t*)data + hdr_len;
    uint16_t payload_len = total_len - hdr_len;

    /* Learn sender's MAC→IP mapping via ARP */
    /* (The Ethernet header has already been stripped by net layer) */

    /* Protocol demux */
    switch (hdr->protocol) {
        case IP_PROTO_ICMP:
            icmp_handle_packet(ntohl(hdr->src_ip), payload, payload_len);
            break;
        case IP_PROTO_UDP:
            udp_handle_packet(ntohl(hdr->src_ip), payload, payload_len);
            break;
        case IP_PROTO_TCP:
            tcp_handle_packet(ntohl(hdr->src_ip), payload, payload_len);
            break;
        default:
            break; /* Unknown protocol — drop */
    }
}

/* --------------------------------------------------------------------------
 * ip_send_packet: Build and send an IPv4 packet
 * -------------------------------------------------------------------------- */
int ip_send_packet(uint32_t dst_ip, uint8_t protocol,
                   const void* payload, uint16_t payload_len) {
    net_device_t* dev = net_get_device(0);
    if (!dev) return -1;

    uint16_t total_len = IP_HEADER_LEN + payload_len;
    if (total_len > ETH_MTU) return -1; /* Too large (no fragmentation) */

    /* Build full frame: Ethernet + IP + payload */
    uint8_t frame[ETH_FRAME_MAX];
    eth_header_t* eth = (eth_header_t*)frame;
    ip_header_t* ip = (ip_header_t*)(frame + ETH_HEADER_LEN);
    uint8_t* pkt_payload = frame + ETH_HEADER_LEN + IP_HEADER_LEN;

    /* Determine next-hop IP (gateway if not on local subnet) */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & config.subnet_mask) != (config.ip & config.subnet_mask)) {
        next_hop = config.gateway; /* Route through gateway */
    }

    /* Resolve next-hop MAC via ARP */
    mac_addr_t dst_mac;
    if (dst_ip == 0xFFFFFFFF) {
        memcpy(dst_mac.addr, MAC_BROADCAST.addr, ETH_ALEN);
    } else if (!arp_resolve(next_hop, &dst_mac)) {
        /* ARP not yet resolved — request was sent, caller should retry */
        return -2;
    }

    /* Ethernet header */
    memcpy(eth->dest, dst_mac.addr, ETH_ALEN);
    memcpy(eth->src, dev->mac.addr, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_IPV4);

    /* IP header */
    ip->ver_ihl    = (IP_VERSION_4 << 4) | (IP_HEADER_LEN / 4);
    ip->tos        = 0;
    ip->total_len  = htons(total_len);
    ip->id         = htons(ip_id_counter++);
    ip->flags_frag = htons(0x4000); /* Don't Fragment flag */
    ip->ttl        = IP_DEFAULT_TTL;
    ip->protocol   = protocol;
    ip->checksum   = 0;
    ip->src_ip     = htonl(config.ip);
    ip->dst_ip     = htonl(dst_ip);

    /* Calculate IP header checksum */
    ip->checksum = ip_checksum(ip, IP_HEADER_LEN);

    /* Copy payload */
    if (payload && payload_len > 0) {
        memcpy(pkt_payload, payload, payload_len);
    }

    /* Send */
    return net_send_packet(dev, frame, ETH_HEADER_LEN + total_len);
}

/* --------------------------------------------------------------------------
 * Getters / Setters
 * -------------------------------------------------------------------------- */
uint32_t ip_get_addr(void) {
    return config.ip;
}

const net_config_t* ip_get_config(void) {
    return &config;
}

void ip_set_config(uint32_t ip, uint32_t gateway, uint32_t mask, uint32_t dns) {
    config.ip          = ip;
    config.gateway     = gateway;
    config.subnet_mask = mask;
    config.dns         = dns;
}

/* --------------------------------------------------------------------------
 * ip_parse: Parse "A.B.C.D" to uint32_t (host byte order)
 * -------------------------------------------------------------------------- */
uint32_t ip_parse(const char* str) {
    uint32_t result = 0;
    uint32_t octet = 0;
    int shift = 24;

    while (*str) {
        if (*str == '.') {
            result |= (octet & 0xFF) << shift;
            shift -= 8;
            octet = 0;
        } else if (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (*str - '0');
        } else {
            break;
        }
        str++;
    }
    result |= (octet & 0xFF) << shift;
    return result;
}

/* --------------------------------------------------------------------------
 * ip_to_string: Format uint32_t (host byte order) as "A.B.C.D"
 * -------------------------------------------------------------------------- */
void ip_to_string(uint32_t ip, char* buf) {
    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        uint8_t octet = (ip >> (i * 8)) & 0xFF;
        /* Convert octet to string */
        if (octet >= 100) {
            buf[pos++] = '0' + (octet / 100);
            buf[pos++] = '0' + ((octet / 10) % 10);
            buf[pos++] = '0' + (octet % 10);
        } else if (octet >= 10) {
            buf[pos++] = '0' + (octet / 10);
            buf[pos++] = '0' + (octet % 10);
        } else {
            buf[pos++] = '0' + octet;
        }
        if (i > 0) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}
