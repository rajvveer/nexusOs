/* ============================================================================
 * NexusOS — TCP (Transmission Control Protocol) — Phase 27
 * ============================================================================
 * Connection-oriented, reliable transport with 3-way handshake,
 * sequence number tracking, and simple sliding window.
 * ============================================================================ */

#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "vga.h"
#include "string.h"

/* Connection table */
static tcp_conn_t connections[TCP_MAX_CONNECTIONS];

/* ISN (Initial Sequence Number) counter — incremented per connection */
static uint32_t tcp_isn_counter = 0x1000;

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * State name lookup
 * -------------------------------------------------------------------------- */
static const char* state_names[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED",
    "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
    "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"
};

const char* tcp_state_name(tcp_state_t state) {
    if (state <= TCP_TIME_WAIT) return state_names[state];
    return "UNKNOWN";
}

/* --------------------------------------------------------------------------
 * tcp_init: Initialize TCP subsystem
 * -------------------------------------------------------------------------- */
void tcp_init(void) {
    memset(connections, 0, sizeof(connections));
    tcp_isn_counter = system_ticks * 64 + 0x1000; /* Semi-random ISN */

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("TCP initialized\n");
}

/* --------------------------------------------------------------------------
 * tcp_checksum: Compute TCP checksum with pseudo-header
 * -------------------------------------------------------------------------- */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void* tcp_data, uint16_t tcp_len) {
    /* Build pseudo-header + TCP segment for checksumming */
    tcp_pseudo_header_t pseudo;
    pseudo.src_ip     = htonl(src_ip);
    pseudo.dst_ip     = htonl(dst_ip);
    pseudo.zero       = 0;
    pseudo.protocol   = IP_PROTO_TCP;
    pseudo.tcp_length = htons(tcp_len);

    /* Sum pseudo-header */
    uint32_t sum = 0;
    const uint16_t* ptr = (const uint16_t*)&pseudo;
    for (int i = 0; i < 6; i++) {
        sum += ptr[i];
    }

    /* Sum TCP data */
    ptr = (const uint16_t*)tcp_data;
    uint16_t remaining = tcp_len;
    while (remaining > 1) {
        sum += *ptr++;
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += *(const uint8_t*)ptr;
    }

    /* Fold */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

/* --------------------------------------------------------------------------
 * find_connection: Find TCB matching a packet
 * -------------------------------------------------------------------------- */
static tcp_conn_t* find_connection(uint32_t remote_ip, uint16_t remote_port,
                                   uint16_t local_port) {
    /* First: exact match */
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->local_port == local_port &&
            c->remote_ip == remote_ip && c->remote_port == remote_port) {
            return c;
        }
    }
    /* Second: LISTEN match (wildcard remote) */
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->state == TCP_LISTEN &&
            c->local_port == local_port) {
            return c;
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * alloc_connection: Get a free TCB slot
 * -------------------------------------------------------------------------- */
static tcp_conn_t* alloc_connection(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(tcp_conn_t));
            connections[i].active = true;
            return &connections[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * tcp_send_segment: Send a TCP segment with specified flags
 * -------------------------------------------------------------------------- */
static int tcp_send_segment(tcp_conn_t* conn, uint8_t flags,
                            const void* data, uint16_t data_len) {
    uint16_t tcp_len = TCP_HEADER_LEN + data_len;
    uint8_t pkt[TCP_HEADER_LEN + TCP_MSS];

    if (tcp_len > sizeof(pkt)) return -1;

    tcp_header_t* hdr = (tcp_header_t*)pkt;
    hdr->src_port   = htons(conn->local_port);
    hdr->dst_port   = htons(conn->remote_port);
    hdr->seq_num    = htonl(conn->snd_nxt);
    hdr->ack_num    = htonl(conn->rcv_nxt);
    hdr->data_off   = (TCP_HEADER_LEN / 4) << 4; /* Data offset in 32-bit words */
    hdr->flags      = flags;
    hdr->window     = htons(conn->rcv_wnd);
    hdr->checksum   = 0;
    hdr->urgent_ptr = 0;

    /* Copy payload */
    if (data && data_len > 0) {
        memcpy(pkt + TCP_HEADER_LEN, data, data_len);
    }

    /* Compute checksum */
    hdr->checksum = tcp_checksum(conn->local_ip, conn->remote_ip,
                                 pkt, tcp_len);

    /* Advance SND.NXT */
    if (flags & TCP_FLAG_SYN) conn->snd_nxt++;
    if (flags & TCP_FLAG_FIN) conn->snd_nxt++;
    conn->snd_nxt += data_len;
    conn->last_activity = system_ticks;

    return ip_send_packet(conn->remote_ip, IP_PROTO_TCP, pkt, tcp_len);
}

/* --------------------------------------------------------------------------
 * tcp_send_rst: Send a RST to reject unwanted segments
 * -------------------------------------------------------------------------- */
static void tcp_send_rst(uint32_t remote_ip, uint16_t remote_port,
                         uint16_t local_port, uint32_t seq, uint32_t ack) {
    uint8_t pkt[TCP_HEADER_LEN];
    tcp_header_t* hdr = (tcp_header_t*)pkt;

    hdr->src_port   = htons(local_port);
    hdr->dst_port   = htons(remote_port);
    hdr->seq_num    = htonl(seq);
    hdr->ack_num    = htonl(ack);
    hdr->data_off   = (TCP_HEADER_LEN / 4) << 4;
    hdr->flags      = TCP_FLAG_RST | TCP_FLAG_ACK;
    hdr->window     = 0;
    hdr->checksum   = 0;
    hdr->urgent_ptr = 0;

    hdr->checksum = tcp_checksum(ip_get_addr(), remote_ip,
                                  pkt, TCP_HEADER_LEN);

    ip_send_packet(remote_ip, IP_PROTO_TCP, pkt, TCP_HEADER_LEN);
}

/* --------------------------------------------------------------------------
 * tcp_handle_packet: Process incoming TCP segment
 * -------------------------------------------------------------------------- */
void tcp_handle_packet(uint32_t src_ip, const void* data, uint16_t len) {
    if (len < TCP_HEADER_LEN) return;

    const tcp_header_t* hdr = (const tcp_header_t*)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq      = ntohl(hdr->seq_num);
    uint32_t ack      = ntohl(hdr->ack_num);
    uint8_t  flags    = hdr->flags;
    uint8_t  data_off = (hdr->data_off >> 4) * 4;

    /* Verify checksum */
    if (tcp_checksum(src_ip, ip_get_addr(), data, len) != 0) return;

    /* Payload */
    const uint8_t* payload = (const uint8_t*)data + data_off;
    uint16_t payload_len = len - data_off;

    /* Find matching TCB */
    tcp_conn_t* conn = find_connection(src_ip, src_port, dst_port);

    if (!conn) {
        /* No connection — send RST if not a RST */
        if (!(flags & TCP_FLAG_RST)) {
            tcp_send_rst(src_ip, src_port, dst_port, 0, seq + 1);
        }
        return;
    }

    /* State machine */
    switch (conn->state) {
        case TCP_LISTEN:
            if (flags & TCP_FLAG_SYN) {
                /* Incoming connection */
                conn->remote_ip   = src_ip;
                conn->remote_port = src_port;
                conn->rcv_nxt     = seq + 1;
                conn->snd_nxt     = tcp_isn_counter++;
                conn->snd_una     = conn->snd_nxt;
                conn->rcv_wnd     = TCP_WINDOW_SIZE;
                conn->state       = TCP_SYN_RECEIVED;

                /* Send SYN-ACK */
                tcp_send_segment(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_SYN_SENT:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
                (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                /* SYN-ACK received — complete handshake */
                conn->snd_una = ack;
                conn->rcv_nxt = seq + 1;
                conn->state   = TCP_ESTABLISHED;
                conn->connected = true;

                /* Send ACK */
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_SYN_RECEIVED:
            if (flags & TCP_FLAG_ACK) {
                conn->snd_una   = ack;
                conn->state     = TCP_ESTABLISHED;
                conn->connected = true;
            }
            break;

        case TCP_ESTABLISHED:
            /* Handle RST */
            if (flags & TCP_FLAG_RST) {
                conn->state  = TCP_CLOSED;
                conn->closed = true;
                conn->active = false;
                return;
            }

            /* Handle incoming data */
            if (payload_len > 0 && seq == conn->rcv_nxt) {
                /* Copy data to RX buffer */
                uint16_t space = TCP_RX_BUF_SIZE - conn->rx_len;
                uint16_t copy_len = payload_len < space ? payload_len : space;
                if (copy_len > 0) {
                    memcpy(conn->rx_buf + conn->rx_len, payload, copy_len);
                    conn->rx_len += copy_len;
                    conn->rcv_nxt += copy_len;
                    conn->data_available = true;
                }
            }

            /* Handle ACK */
            if (flags & TCP_FLAG_ACK) {
                conn->snd_una = ack;
                conn->snd_wnd = ntohs(hdr->window);
            }

            /* Handle FIN */
            if (flags & TCP_FLAG_FIN) {
                conn->rcv_nxt = seq + payload_len + 1;
                conn->state   = TCP_CLOSE_WAIT;
                conn->closed  = true;

                /* ACK the FIN */
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            } else if (payload_len > 0) {
                /* ACK the data */
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_FIN_WAIT_1:
            if (flags & TCP_FLAG_ACK) {
                conn->snd_una = ack;
                if (flags & TCP_FLAG_FIN) {
                    /* Simultaneous close */
                    conn->rcv_nxt = seq + 1;
                    conn->state = TCP_TIME_WAIT;
                    tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                } else {
                    conn->state = TCP_FIN_WAIT_2;
                }
            }
            break;

        case TCP_FIN_WAIT_2:
            if (flags & TCP_FLAG_FIN) {
                conn->rcv_nxt = seq + 1;
                conn->state   = TCP_TIME_WAIT;
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_CLOSE_WAIT:
            /* Waiting for application to close */
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_FLAG_ACK) {
                conn->state  = TCP_CLOSED;
                conn->active = false;
            }
            break;

        case TCP_TIME_WAIT:
            /* Wait 2*MSL then close — simplified: close immediately */
            conn->state  = TCP_CLOSED;
            conn->active = false;
            break;

        default:
            break;
    }

    conn->last_activity = system_ticks;
}

/* --------------------------------------------------------------------------
 * tcp_connect: Active open (initiate connection)
 * -------------------------------------------------------------------------- */
tcp_conn_t* tcp_connect(uint32_t remote_ip, uint16_t remote_port,
                        uint16_t local_port) {
    tcp_conn_t* conn = alloc_connection();
    if (!conn) return NULL;

    conn->local_ip    = ip_get_addr();
    conn->local_port  = local_port;
    conn->remote_ip   = remote_ip;
    conn->remote_port = remote_port;
    conn->snd_nxt     = tcp_isn_counter++;
    conn->snd_una     = conn->snd_nxt;
    conn->rcv_wnd     = TCP_WINDOW_SIZE;
    conn->state       = TCP_SYN_SENT;

    /* Send SYN */
    tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0);

    return conn;
}

/* --------------------------------------------------------------------------
 * tcp_listen: Passive open (wait for connection)
 * -------------------------------------------------------------------------- */
tcp_conn_t* tcp_listen(uint16_t port) {
    tcp_conn_t* conn = alloc_connection();
    if (!conn) return NULL;

    conn->local_ip   = ip_get_addr();
    conn->local_port = port;
    conn->rcv_wnd    = TCP_WINDOW_SIZE;
    conn->state      = TCP_LISTEN;

    return conn;
}

/* --------------------------------------------------------------------------
 * tcp_send: Send data on an established connection
 * -------------------------------------------------------------------------- */
int tcp_send(tcp_conn_t* conn, const void* data, uint16_t len) {
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
    if (len == 0) return 0;

    /* Send in MSS-sized chunks */
    const uint8_t* ptr = (const uint8_t*)data;
    uint16_t remaining = len;

    while (remaining > 0) {
        uint16_t chunk = remaining < TCP_MSS ? remaining : TCP_MSS;
        int ret = tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH,
                                    ptr, chunk);
        if (ret < 0) return ret;
        ptr += chunk;
        remaining -= chunk;
    }

    return (int)len;
}

/* --------------------------------------------------------------------------
 * tcp_recv: Read data from RX buffer
 * -------------------------------------------------------------------------- */
int tcp_recv(tcp_conn_t* conn, void* buf, uint16_t max_len) {
    if (!conn) return -1;
    if (conn->rx_len == 0) {
        if (conn->closed) return -1; /* Connection closed */
        return 0; /* No data yet */
    }

    uint16_t copy_len = conn->rx_len < max_len ? conn->rx_len : max_len;
    memcpy(buf, conn->rx_buf, copy_len);

    /* Shift remaining data forward */
    if (copy_len < conn->rx_len) {
        memcpy(conn->rx_buf, conn->rx_buf + copy_len,
               conn->rx_len - copy_len);
    }
    conn->rx_len -= copy_len;
    conn->data_available = (conn->rx_len > 0);

    return (int)copy_len;
}

/* --------------------------------------------------------------------------
 * tcp_close: Initiate connection close
 * -------------------------------------------------------------------------- */
void tcp_close(tcp_conn_t* conn) {
    if (!conn || !conn->active) return;

    switch (conn->state) {
        case TCP_ESTABLISHED:
            conn->state = TCP_FIN_WAIT_1;
            tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            break;

        case TCP_CLOSE_WAIT:
            conn->state = TCP_LAST_ACK;
            tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            break;

        case TCP_LISTEN:
        case TCP_SYN_SENT:
            conn->state  = TCP_CLOSED;
            conn->active = false;
            break;

        default:
            break;
    }
}

/* --------------------------------------------------------------------------
 * tcp_get_connections: Return connection table for display
 * -------------------------------------------------------------------------- */
const tcp_conn_t* tcp_get_connections(int* count) {
    if (count) *count = TCP_MAX_CONNECTIONS;
    return connections;
}
