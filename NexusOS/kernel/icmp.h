/* ============================================================================
 * NexusOS — ICMP (Internet Control Message Protocol) Header — Phase 27
 * ============================================================================
 * ICMP echo request/reply (ping) and error messages.
 * ============================================================================ */

#ifndef ICMP_H
#define ICMP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * ICMP Types
 * -------------------------------------------------------------------------- */
#define ICMP_ECHO_REPLY         0
#define ICMP_DEST_UNREACHABLE   3
#define ICMP_ECHO_REQUEST       8
#define ICMP_TIME_EXCEEDED      11

/* --------------------------------------------------------------------------
 * ICMP Header
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  type;          /* Message type */
    uint8_t  code;          /* Type sub-code */
    uint16_t checksum;      /* Ones' complement checksum */
    uint16_t id;            /* Identifier (for echo) */
    uint16_t sequence;      /* Sequence number (for echo) */
} __attribute__((packed)) icmp_header_t;

/* --------------------------------------------------------------------------
 * Ping Result (for shell ping command)
 * -------------------------------------------------------------------------- */
#define PING_MAX_PENDING 8

typedef struct {
    bool     active;            /* Ping session active */
    uint32_t target_ip;         /* Target IP address */
    uint16_t id;                /* ICMP echo ID for this session */
    uint16_t seq_next;          /* Next sequence to send */
    uint16_t replies;           /* Number of replies received */
    uint16_t sent;              /* Number of requests sent */
    uint32_t last_send_tick;    /* Tick when last request sent */
    uint32_t last_rtt_ticks;    /* RTT of last reply in ticks */
    bool     got_reply;         /* Flag: new reply received */
} ping_state_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize ICMP subsystem */
void icmp_init(void);

/* Handle an incoming ICMP packet */
void icmp_handle_packet(uint32_t src_ip, const void* data, uint16_t len);

/* Send an ICMP echo request (ping) */
int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t sequence);

/* Get the global ping state (for shell command) */
ping_state_t* icmp_get_ping_state(void);

#endif /* ICMP_H */
