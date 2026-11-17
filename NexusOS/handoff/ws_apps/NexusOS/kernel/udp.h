/* ============================================================================
 * NexusOS — UDP (User Datagram Protocol) Header — Phase 27
 * ============================================================================
 * Connectionless datagram transport protocol.
 * ============================================================================ */

#ifndef UDP_H
#define UDP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define UDP_HEADER_LEN      8
#define UDP_MAX_BINDINGS    8       /* Max bound ports */
#define UDP_RX_BUF_SIZE     2       /* RX queue depth per binding */

/* --------------------------------------------------------------------------
 * UDP Header
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t src_port;      /* Source port */
    uint16_t dst_port;      /* Destination port */
    uint16_t length;        /* Length (header + data) */
    uint16_t checksum;      /* Checksum (optional, 0 = none) */
} __attribute__((packed)) udp_header_t;

/* --------------------------------------------------------------------------
 * UDP Received Datagram
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t length;
    uint8_t  data[512];     /* Reduced buffer (saves ~8KB per slot vs 1472) */
    bool     valid;
} udp_datagram_t;

/* --------------------------------------------------------------------------
 * UDP Port Binding
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t       port;
    bool           active;
    udp_datagram_t rx_buf[UDP_RX_BUF_SIZE];
    int            rx_head;
    int            rx_tail;
} udp_binding_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize UDP subsystem */
void udp_init(void);

/* Handle incoming UDP packet (called from IP layer) */
void udp_handle_packet(uint32_t src_ip, const void* data, uint16_t len);

/* Send a UDP datagram */
int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void* data, uint16_t len);

/* Bind a local port for receiving */
int udp_bind(uint16_t port);

/* Unbind a local port */
void udp_unbind(uint16_t port);

/* Receive a datagram from a bound port (returns NULL if none) */
udp_datagram_t* udp_recv(uint16_t port);

/* Get binding table for netstat display */
const udp_binding_t* udp_get_bindings(int* count);

#endif /* UDP_H */
