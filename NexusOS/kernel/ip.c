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
#include "firewall.h"

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

#ifdef NEXUS_NATIVE64
/* Loopback device: packets for 127.0.0.0/8 or our own address wait in this ring and are delivered from the
 * network poll (ip_loopback_poll), in order; a full ring refuses the packet with -11 (EAGAIN): TCP and the
 * datagram queues keep it and send it again once the ring drains. */
#define LOOP_SLOTS 128
#define LOOP_BYTES 1500
static uint8_t loop_ring[LOOP_SLOTS][LOOP_BYTES];
static uint16_t loop_size[LOOP_SLOTS];
static unsigned loop_head, loop_count;
/* Native: every address of every interface (and IPv6 handles of local addresses) is local too (net4.c). */
int ip_is_local(uint32_t ip) { return (ip >> 24) == 127 || (config.ip && ip == config.ip) || ip4_local_address(ip); }
int ip_loopback_pending(void) { return loop_count != 0; }
int ip_loopback_room(void) { return (int)(LOOP_SLOTS - loop_count); }
static void ip_deliver(const void* data, uint16_t len, int loopback);
extern void native_network_kick(void);
void ip_loopback_poll(void) {
    unsigned n = loop_count;   /* packets queued while these are delivered wait for the next poll */
    while (n-- && loop_count) {
        uint8_t packet[LOOP_BYTES]; uint16_t size = loop_size[loop_head];
        memcpy(packet, loop_ring[loop_head], size);
        loop_head = (loop_head + 1) % LOOP_SLOTS; loop_count--;
        if ((packet[0] >> 4) == 6) ipv6_loopback_input(packet, size); else ip_deliver(packet, size, 1);   /* IPv6 shares the ring */
    }
}
/* Queue one complete IPv4 or IPv6 packet on the loopback device (arch/x86_64 net4.c / ipv6.c output). */
int ip_loopback_queue(const void* packet, uint16_t len) {
    if (len > LOOP_BYTES) return -90;
    if (loop_count == LOOP_SLOTS) return -11;   /* full: the sender retries after ip_loopback_poll */
    unsigned slot = (loop_head + loop_count) % LOOP_SLOTS;
    memcpy(loop_ring[slot], packet, len); loop_size[slot] = len; loop_count++;
    native_network_kick();
    return 0;
}
void ip_deliver_reassembled(const void* data, uint16_t len, int loopback) { ip_deliver(data, len, loopback); }
uint32_t ip_source_address(uint32_t dst_ip) { return ip4_source_for(dst_ip); }   /* routing (net4.c) */
#else
uint32_t ip_source_address(uint32_t dst_ip) { return (dst_ip >> 24) == 127 ? 0x7F000001U : config.ip; }
#endif

/* --------------------------------------------------------------------------
 * ip_handle_packet: Process incoming IPv4 packet
 * -------------------------------------------------------------------------- */
#ifdef NEXUS_NATIVE64
void ip_handle_packet(const void* data, uint16_t len) { ip_deliver(data, len, 0); }
void ip_mc_loop_deliver(const void* data, uint16_t len) { ip_deliver(data, len, 2); }   /* our own multicast, looped (net4.c) */
static void ip_deliver(const void* data, uint16_t len, int loopback) {
#else
void ip_handle_packet(const void* data, uint16_t len) {
#endif
    if (len < IP_HEADER_LEN) return;

    const ip_header_t* hdr = (const ip_header_t*)data;

    /* Validate version */
    uint8_t version = (hdr->ver_ihl >> 4) & 0xF;
    if (version != IP_VERSION_4) return;

    /* Get header length (in 32-bit words) */
    uint8_t ihl = hdr->ver_ihl & 0xF;
    uint16_t hdr_len = (uint16_t)(ihl * 4);
    if (hdr_len < IP_HEADER_LEN || hdr_len > len) return;

    /* Validate total length: must cover the header (else payload_len below
     * underflows) and fit in the received frame. */
    uint16_t total_len = ntohs(hdr->total_len);
    if (total_len < hdr_len || total_len > len) return;

    /* Verify checksum */
    if (ip_checksum(data, hdr_len) != 0) return;
    /* Fragment reassembly is required before transport headers can be trusted. */
#ifdef NEXUS_NATIVE64
    bool fragment = (ntohs(hdr->flags_frag) & ~0x4000U) != 0;   /* reassembled once the destination is ours (net4.c) */
#else
    if (ntohs(hdr->flags_frag) & ~0x4000U) return;
#endif

    /* Check destination: must be for us or broadcast */
    uint32_t dst = ntohl(hdr->dst_ip);
#ifdef NEXUS_NATIVE64
    /* DHCP replies may be unicast to the offered address before it is configured. */
    bool dhcp_reply = hdr->protocol == IP_PROTO_UDP && total_len >= hdr_len + 8U &&
                      ((const uint8_t*)data)[hdr_len + 2] == 0 && ((const uint8_t*)data)[hdr_len + 3] == 68;
#else
    bool dhcp_reply = false;
#endif
#ifdef NEXUS_NATIVE64
    /* Martian sources: loopback, our own and broadcast/multicast addresses never come from the wire (Linux). */
    uint32_t src = ntohl(hdr->src_ip);
    if (!loopback && ((src >> 24) == 127 || (config.ip && src == config.ip) || src >= 0xE0000000U)) return;
    if (loopback == 1 ? !ip_is_local(dst) : (dst >> 24) == 127) return; /* 127/8 only on the loopback device */
    if (!loopback && ip4_ingress(data, total_len, dst == config.ip || dst == 0xFFFFFFFF || ip4_accept_destination(dst) ||
                                 dst == (config.ip | ~config.subnet_mask) || dhcp_reply)) return;   /* forwarding, NAT (forward.c) */
    if (!loopback && dst != config.ip && dst != 0xFFFFFFFF && !ip4_accept_destination(dst) &&
#else
    if (dst != config.ip && dst != 0xFFFFFFFF &&
#endif
        dst != (config.ip | ~config.subnet_mask) && !dhcp_reply) {
        return; /* Not for us */
    }
#ifdef NEXUS_NATIVE64
    if (fragment) { ip4_fragment_input(data, total_len, loopback); return; }
#endif

    /* Extract payload */
    const uint8_t* payload = (const uint8_t*)data + hdr_len;
    uint16_t payload_len = total_len - hdr_len;

    /* Phase 44: firewall ingress filter. For TCP/UDP the remote port we filter
     * on is the packet's DESTINATION port (the local service being reached);
     * the source IP is the remote end. */
    {
        uint16_t dport = 0;
        if ((hdr->protocol == IP_PROTO_TCP || hdr->protocol == IP_PROTO_UDP)
            && payload_len >= 4) {
            dport = ntohs(*(const uint16_t*)(payload + 2)); /* dst_port */
        }
        if (firewall_check(FW_IN, hdr->protocol, ntohl(hdr->src_ip), dport)) {
            return; /* dropped by firewall */
        }
    }
#ifdef NEXUS_NATIVE64
    if (ip4_local_deliver_hook(data, total_len, loopback)) return;   /* netfilter INPUT, raw and ping sockets (net4.c) */
#endif

    /* Learn sender's MAC→IP mapping via ARP */
    /* (The Ethernet header has already been stripped by net layer) */

    /* Protocol demux */
    switch (hdr->protocol) {
        case IP_PROTO_ICMP:
            icmp_handle_packet(ntohl(hdr->src_ip), payload, payload_len);
            break;
        case IP_PROTO_UDP:
            udp_handle_packet(ntohl(hdr->src_ip), dst, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            /* TCP is unicast only: segments for a broadcast address are dropped (no RST) */
#ifdef NEXUS_NATIVE64
            if (dst == config.ip || (dst >> 24) == 127 || ip_is_local(dst))   /* every interface's addresses (net4.c) */
#else
            if (dst == config.ip || (dst >> 24) == 127)
#endif
                tcp_handle_packet_to(ntohl(hdr->src_ip), dst, payload, payload_len);
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
    return ip_send_packet_from(0, dst_ip, protocol, payload, payload_len);
}
int ip_send_packet_from(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                        const void* payload, uint16_t payload_len) {
#ifdef NEXUS_NATIVE64
    return ip4_output(src_ip, dst_ip, protocol, payload, payload_len, NULL);   /* routing, filter, fragments (net4.c) */
#endif
    if (payload_len > ETH_MTU - IP_HEADER_LEN || (payload_len && !payload)) return -1;
    if (!src_ip) src_ip = ip_source_address(dst_ip);
#ifdef NEXUS_NATIVE64
    if (ip_is_local(dst_ip)) {
        if (loop_count == LOOP_SLOTS) return -11;   /* full: the sender retries after ip_loopback_poll */
        uint8_t* slot = loop_ring[(loop_head + loop_count) % LOOP_SLOTS];
        ip_header_t* lip = (ip_header_t*)slot; uint16_t loop_total = IP_HEADER_LEN + payload_len;
        lip->ver_ihl = (IP_VERSION_4 << 4) | (IP_HEADER_LEN / 4); lip->tos = 0; lip->total_len = htons(loop_total);
        lip->id = htons(ip_id_counter++); lip->flags_frag = htons(0x4000); lip->ttl = IP_DEFAULT_TTL; lip->protocol = protocol;
        lip->checksum = 0; lip->src_ip = htonl(src_ip); lip->dst_ip = htonl(dst_ip);
        lip->checksum = ip_checksum(lip, IP_HEADER_LEN);
        if (payload_len) memcpy(slot + IP_HEADER_LEN, payload, payload_len);
        loop_size[(loop_head + loop_count) % LOOP_SLOTS] = loop_total; loop_count++;
        native_network_kick();
        return 0;
    }
#endif
    net_device_t* dev = net_get_device(0);
    if (!dev) return -1;

    /* Phase 44: firewall egress filter (remote = dst_ip, port = dst port). */
    {
        uint16_t dport = 0;
        if ((protocol == IP_PROTO_TCP || protocol == IP_PROTO_UDP)
            && payload && payload_len >= 4) {
            dport = ntohs(*(const uint16_t*)((const uint8_t*)payload + 2));
        }
        if (firewall_check(FW_OUT, protocol, dst_ip, dport)) {
            return -1; /* blocked by firewall */
        }
    }

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
    if (dst_ip == 0xFFFFFFFF || dst_ip == (config.ip | ~config.subnet_mask)) {
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
    ip->src_ip     = htonl(src_ip);
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
