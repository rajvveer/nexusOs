/* ============================================================================
 * NexusOS — UDP (User Datagram Protocol) — Phase 27
 * ============================================================================
 * Connectionless datagram transport with port-based demuxing.
 * ============================================================================ */

#include "udp.h"
#include "ip.h"
#include "net.h"
#include "vga.h"
#include "string.h"

/* Port binding table */
static udp_binding_t bindings[UDP_MAX_BINDINGS];

/* --------------------------------------------------------------------------
 * udp_init: Initialize UDP subsystem
 * -------------------------------------------------------------------------- */
void udp_init(void) {
    memset(bindings, 0, sizeof(bindings));

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("UDP initialized\n");
}

/* --------------------------------------------------------------------------
 * find_binding: Find a port binding
 * -------------------------------------------------------------------------- */
static udp_binding_t* find_binding(uint16_t port) {
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (bindings[i].active && bindings[i].port == port) {
            return &bindings[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * udp_handle_packet: Process incoming UDP datagram
 * -------------------------------------------------------------------------- */
void udp_handle_packet(uint32_t src_ip, const void* data, uint16_t len) {
    if (len < UDP_HEADER_LEN) return;

    const udp_header_t* hdr = (const udp_header_t*)data;
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t udp_len  = ntohs(hdr->length);

    if (udp_len < UDP_HEADER_LEN || udp_len > len) return;

    uint16_t payload_len = udp_len - UDP_HEADER_LEN;
    const uint8_t* payload = (const uint8_t*)data + UDP_HEADER_LEN;

    /* Find binding for this port */
    udp_binding_t* bind = find_binding(dst_port);
    if (!bind) return; /* No listener — drop */

    /* Queue the datagram */
    int next_head = (bind->rx_head + 1) % UDP_RX_BUF_SIZE;
    if (next_head == bind->rx_tail) {
        return; /* RX buffer full — drop */
    }

    udp_datagram_t* dgram = &bind->rx_buf[bind->rx_head];
    dgram->src_ip   = src_ip;
    dgram->src_port = src_port;
    dgram->length   = payload_len > 512 ? 512 : payload_len;
    memcpy(dgram->data, payload, dgram->length);
    dgram->valid    = true;

    bind->rx_head = next_head;
}

/* --------------------------------------------------------------------------
 * udp_send: Send a UDP datagram
 * -------------------------------------------------------------------------- */
int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void* data, uint16_t len) {
    if (len > 512) return -1; /* Too large for buffer */

    /* Build UDP packet: header + payload */
    uint8_t pkt[UDP_HEADER_LEN + 1472];
    udp_header_t* hdr = (udp_header_t*)pkt;

    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons(UDP_HEADER_LEN + len);
    hdr->checksum = 0; /* Optional for UDP over IPv4 */

    if (data && len > 0) {
        memcpy(pkt + UDP_HEADER_LEN, data, len);
    }

    return ip_send_packet(dst_ip, IP_PROTO_UDP, pkt, UDP_HEADER_LEN + len);
}

/* --------------------------------------------------------------------------
 * udp_bind: Bind a local port
 * -------------------------------------------------------------------------- */
int udp_bind(uint16_t port) {
    /* Check if already bound */
    if (find_binding(port)) return -1;

    /* Find free slot */
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (!bindings[i].active) {
            bindings[i].port    = port;
            bindings[i].active  = true;
            bindings[i].rx_head = 0;
            bindings[i].rx_tail = 0;
            memset(bindings[i].rx_buf, 0, sizeof(bindings[i].rx_buf));
            return 0;
        }
    }
    return -1; /* No free slots */
}

/* --------------------------------------------------------------------------
 * udp_unbind: Release a port binding
 * -------------------------------------------------------------------------- */
void udp_unbind(uint16_t port) {
    udp_binding_t* bind = find_binding(port);
    if (bind) {
        bind->active = false;
    }
}

/* --------------------------------------------------------------------------
 * udp_recv: Dequeue a received datagram
 * -------------------------------------------------------------------------- */
udp_datagram_t* udp_recv(uint16_t port) {
    udp_binding_t* bind = find_binding(port);
    if (!bind) return NULL;
    if (bind->rx_tail == bind->rx_head) return NULL;

    udp_datagram_t* dgram = &bind->rx_buf[bind->rx_tail];
    if (!dgram->valid) return NULL;

    bind->rx_tail = (bind->rx_tail + 1) % UDP_RX_BUF_SIZE;
    dgram->valid = false;
    return dgram;
}

/* --------------------------------------------------------------------------
 * udp_get_bindings: Return binding table for display
 * -------------------------------------------------------------------------- */
const udp_binding_t* udp_get_bindings(int* count) {
    if (count) *count = UDP_MAX_BINDINGS;
    return bindings;
}
