/* ============================================================================
 * NexusOS — ICMP (Internet Control Message Protocol) — Phase 27
 * ============================================================================
 * Handles ICMP echo request/reply (ping).
 * Responds to incoming pings and tracks outgoing ping sessions.
 * ============================================================================ */

#include "icmp.h"
#include "ip.h"
#include "net.h"
#include "vga.h"
#include "string.h"

/* Global ping tracking state */
static ping_state_t ping_state;

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * icmp_init: Initialize ICMP subsystem
 * -------------------------------------------------------------------------- */
void icmp_init(void) {
    memset(&ping_state, 0, sizeof(ping_state));

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("ICMP initialized (ping ready)\n");
}

/* --------------------------------------------------------------------------
 * icmp_handle_packet: Process incoming ICMP packet
 * -------------------------------------------------------------------------- */
void icmp_handle_packet(uint32_t src_ip, const void* data, uint16_t len) {
    if (len < sizeof(icmp_header_t)) return;

    const icmp_header_t* hdr = (const icmp_header_t*)data;

    /* Verify checksum */
    if (ip_checksum(data, len) != 0) return;

    switch (hdr->type) {
        case ICMP_ECHO_REQUEST: {
            /* Someone is pinging us — send echo reply */
            /* Build reply: same data but type = ECHO_REPLY */
            uint8_t reply[ETH_MTU];
            if (len > ETH_MTU) return;

            memcpy(reply, data, len);
            icmp_header_t* reply_hdr = (icmp_header_t*)reply;
            reply_hdr->type = ICMP_ECHO_REPLY;
            reply_hdr->code = 0;
            reply_hdr->checksum = 0;
            reply_hdr->checksum = ip_checksum(reply, len);

            ip_send_packet(src_ip, IP_PROTO_ICMP, reply, len);
            break;
        }

        case ICMP_ECHO_REPLY: {
            /* We got a ping reply — check if it matches our ping session */
            if (ping_state.active &&
                ntohs(hdr->id) == ping_state.id) {
                ping_state.replies++;
                ping_state.last_rtt_ticks = system_ticks - ping_state.last_send_tick;
                ping_state.got_reply = true;
            }
            break;
        }

        default:
            break; /* Ignore other ICMP types */
    }
}

/* --------------------------------------------------------------------------
 * icmp_send_echo: Send ICMP echo request
 * -------------------------------------------------------------------------- */
int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t sequence) {
    /* Build ICMP echo request */
    uint8_t pkt[sizeof(icmp_header_t) + 32]; /* header + 32 bytes payload */
    icmp_header_t* hdr = (icmp_header_t*)pkt;
    uint8_t* payload = pkt + sizeof(icmp_header_t);

    hdr->type     = ICMP_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = htons(id);
    hdr->sequence = htons(sequence);

    /* Fill payload with pattern */
    for (int i = 0; i < 32; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    /* Calculate checksum */
    uint16_t total_len = sizeof(icmp_header_t) + 32;
    hdr->checksum = ip_checksum(pkt, total_len);

    /* Track in ping state */
    ping_state.last_send_tick = system_ticks;
    ping_state.sent++;

    return ip_send_packet(dst_ip, IP_PROTO_ICMP, pkt, total_len);
}

/* --------------------------------------------------------------------------
 * icmp_get_ping_state: Return ping state for shell
 * -------------------------------------------------------------------------- */
ping_state_t* icmp_get_ping_state(void) {
    return &ping_state;
}
