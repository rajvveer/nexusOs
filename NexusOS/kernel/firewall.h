/* ============================================================================
 * NexusOS — Packet-Filter Firewall (Header) — Phase 44
 * ============================================================================
 * A small stateless rule list checked at the IP layer's ingress and egress
 * choke points (ip_handle_packet / ip_send_packet). Rules match on direction,
 * protocol, remote IP, and remote port; first match wins; a configurable
 * default policy applies when no rule matches.
 *
 * Wildcards: IP 0 (PROTO_ANY / port 0) means "any". IPs and ports in rules are
 * stored in HOST byte order (the hooks convert before calling).
 * ============================================================================ */

#ifndef FIREWALL_H
#define FIREWALL_H

#include "types.h"

#define FW_MAX_RULES 32

/* Verdicts / default policy */
#define FW_ACCEPT 0
#define FW_DROP   1

/* Direction */
#define FW_IN   0
#define FW_OUT  1
#define FW_BOTH 2

/* Protocol selectors (match the IP protocol numbers; 0 = any) */
#define FW_PROTO_ANY  0
#define FW_PROTO_ICMP 1
#define FW_PROTO_TCP  6
#define FW_PROTO_UDP  17

typedef struct {
    bool     used;
    uint8_t  action;     /* FW_ACCEPT / FW_DROP        */
    uint8_t  direction;  /* FW_IN / FW_OUT / FW_BOTH   */
    uint8_t  proto;      /* FW_PROTO_* (0 = any)       */
    uint32_t addr;       /* remote IP, host order (0 = any) */
    uint16_t port;       /* remote port, host order (0 = any) */
} fw_rule_t;

void firewall_init(void);

/* Master switch + default policy (applied when no rule matches). */
void firewall_set_enabled(bool on);
bool firewall_enabled(void);
void firewall_set_default(uint8_t action);   /* FW_ACCEPT / FW_DROP */
uint8_t firewall_default(void);

/* Rule management. add returns the rule index or -1 if full. */
int  firewall_add(uint8_t action, uint8_t direction, uint8_t proto,
                  uint32_t addr, uint16_t port);
bool firewall_del(int index);
void firewall_clear(void);
int  firewall_count(void);
const fw_rule_t* firewall_rule(int index);   /* NULL if out of range/unused */

/* Statistics */
uint32_t firewall_dropped(void);
uint32_t firewall_passed(void);

/* --- Hook entry points (called by ip.c) ---
 * Return true if the packet should be DROPPED. `remote` = the other end's IP
 * (source for ingress, dest for egress), `port` = remote port (0 if N/A),
 * both in host byte order. */
bool firewall_check(uint8_t direction, uint8_t proto, uint32_t remote, uint16_t port);

#endif /* FIREWALL_H */
