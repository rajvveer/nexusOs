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
