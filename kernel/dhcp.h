/* ============================================================================
 * NexusOS — DHCP Client Header — Phase 30
 * ============================================================================
 * Dynamic Host Configuration Protocol client for auto IP configuration.
 * ============================================================================ */

#ifndef DHCP_H
#define DHCP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68
#define DHCP_MAGIC_COOKIE   0x63825363

/* DHCP message types */
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_RELEASE        7

/* DHCP option codes */
#define DHCP_OPT_SUBNET     1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_DNS        6
#define DHCP_OPT_HOSTNAME   12
#define DHCP_OPT_DOMAIN     15
#define DHCP_OPT_REQIP      50
#define DHCP_OPT_LEASE      51
#define DHCP_OPT_MSGTYPE    53
#define DHCP_OPT_SERVERID   54
#define DHCP_OPT_PARAMLIST  55
#define DHCP_OPT_END        255

/* DHCP timeout */
#define DHCP_TIMEOUT_TICKS  90  /* ~5 seconds at 18.2 Hz */

/* DHCP status */
typedef enum {
    DHCP_IDLE = 0,
    DHCP_DISCOVERING,
    DHCP_REQUESTING,
    DHCP_BOUND,
    DHCP_FAILED
} dhcp_state_t;

/* --------------------------------------------------------------------------
 * DHCP Message (simplified, 548 bytes)
 * -------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint8_t  op;            /* 1=BOOTREQUEST, 2=BOOTREPLY */
    uint8_t  htype;         /* Hardware type: 1=Ethernet */
    uint8_t  hlen;          /* Hardware address length: 6 */
    uint8_t  hops;          /* Hops: 0 */
    uint32_t xid;           /* Transaction ID */
    uint16_t secs;          /* Seconds elapsed */
    uint16_t flags;         /* Flags (0x8000 = broadcast) */
    uint32_t ciaddr;        /* Client IP (if already known) */
    uint32_t yiaddr;        /* 'Your' IP (offered by server) */
    uint32_t siaddr;        /* Server IP */
    uint32_t giaddr;        /* Relay agent IP */
    uint8_t  chaddr[16];    /* Client hardware address */
    uint8_t  sname[64];     /* Server host name */
    uint8_t  file[128];     /* Boot file name */
    uint32_t cookie;        /* Magic cookie: 0x63825363 */
    uint8_t  options[312];  /* DHCP options */
} dhcp_msg_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize DHCP client */
void dhcp_init(void);

/* Perform DHCP request (blocking, ~5s timeout) */
bool dhcp_request(void);

/* Get current DHCP state */
dhcp_state_t dhcp_get_state(void);

#endif /* DHCP_H */
