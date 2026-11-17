/* ============================================================================
 * NexusOS — IPv4 (Internet Protocol) Header — Phase 27
 * ============================================================================
 * IPv4 packet handling, routing, and network configuration.
 * ============================================================================ */

#ifndef IP_H
#define IP_H

#include "types.h"
#include "net.h"

/* --------------------------------------------------------------------------
 * IP Protocol Numbers
 * -------------------------------------------------------------------------- */
#define IP_PROTO_ICMP       1
#define IP_PROTO_TCP        6
#define IP_PROTO_UDP        17

/* IP version and header length */
#define IP_VERSION_4        4
#define IP_HEADER_LEN       20      /* Minimum IP header (no options) */
#define IP_DEFAULT_TTL      64

/* --------------------------------------------------------------------------
 * IPv4 Header (20 bytes, no options)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  ver_ihl;       /* Version (4 bits) + IHL (4 bits) */
    uint8_t  tos;           /* Type of service */
    uint16_t total_len;     /* Total length */
    uint16_t id;            /* Identification */
    uint16_t flags_frag;    /* Flags (3 bits) + Fragment offset (13 bits) */
    uint8_t  ttl;           /* Time to live */
    uint8_t  protocol;      /* Protocol (ICMP=1, TCP=6, UDP=17) */
    uint16_t checksum;      /* Header checksum */
    uint32_t src_ip;        /* Source IP (network byte order) */
    uint32_t dst_ip;        /* Destination IP (network byte order) */
} __attribute__((packed)) ip_header_t;

/* --------------------------------------------------------------------------
 * Network Configuration
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t ip;            /* Our IP address (host byte order) */
    uint32_t gateway;       /* Default gateway (host byte order) */
    uint32_t subnet_mask;   /* Subnet mask (host byte order) */
    uint32_t dns;           /* DNS server (host byte order) */
} net_config_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize IP subsystem with default config */
void ip_init(void);

/* Handle an incoming IP packet (called from net layer) */
void ip_handle_packet(const void* data, uint16_t len);

/* Send an IP packet: builds IPv4 header, resolves next-hop MAC, sends */
int ip_send_packet(uint32_t dst_ip, uint8_t protocol,
                   const void* payload, uint16_t payload_len);
/* Same with an explicit source address (0: ip_source_address(dst_ip)) */
int ip_send_packet_from(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                        const void* payload, uint16_t payload_len);
/* Source address for a destination: 127.0.0.1 for 127.0.0.0/8, else our address */
uint32_t ip_source_address(uint32_t dst_ip);
/* UDP datagram from an explicit source address (udp.c; 0: ip_source_address(dst_ip)), e.g. a bound socket's */
int udp_send_from(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void* data, uint16_t len);
#ifdef NEXUS_NATIVE64
/* Loopback: 127.0.0.0/8 and our own address never leave the host (queued, delivered by ip_loopback_poll) */
int ip_is_local(uint32_t ip);
int ip_loopback_pending(void);
int ip_loopback_room(void);   /* free loopback ring slots */
void ip_loopback_poll(void);
int ip_loopback_queue(const void* packet, uint16_t len);         /* a complete IPv4/IPv6 packet: 0, -11 full, -90 too big */
void ip_deliver_reassembled(const void* data, uint16_t len, int loopback);
/* arch/x86_64 native datapath (net4.c, ipv6.c) */
struct ip6_send_options;
int ip4_output(uint32_t src, uint32_t dst, uint8_t protocol, const void* payload, unsigned length, const struct ip6_send_options* options);
uint32_t ip4_source_for(uint32_t dst);
bool ip4_local_address(uint32_t address);
bool ip4_accept_destination(uint32_t address);
void ip4_fragment_input(const void* data, unsigned length, bool loopback);
bool ip4_local_deliver_hook(const void* data, unsigned length, int loopback);   /* loopback 2: multicast loop copy */
void ip_mc_loop_deliver(const void* data, uint16_t len);   /* IP_MULTICAST_LOOP copy of an outgoing datagram (net4.c) */
bool ip4_ingress(const void* data, unsigned length, bool local);   /* forwarding and NAT before local delivery (forward.c) */
void ipv6_loopback_input(const void* packet, unsigned length);
#endif

/* Get our IP address (host byte order) */
uint32_t ip_get_addr(void);

/* Get network configuration */
const net_config_t* ip_get_config(void);

/* Set network configuration */
void ip_set_config(uint32_t ip, uint32_t gateway, uint32_t mask, uint32_t dns);

/* Compute IP header checksum */
uint16_t ip_checksum(const void* data, uint16_t len);

/* Helper: parse dotted-decimal IP string to uint32_t (host byte order) */
uint32_t ip_parse(const char* str);

/* Helper: format IP as dotted-decimal string */
void ip_to_string(uint32_t ip, char* buf);

#endif /* IP_H */
