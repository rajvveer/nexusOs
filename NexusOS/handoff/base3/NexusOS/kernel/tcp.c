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
static uint32_t tcp_gen_counter = 0;   /* Phase 55: see tcp_conn_t.gen */

static tcp_conn_t* alloc_connection(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(tcp_conn_t));
            connections[i].active = true;
            connections[i].gen = ++tcp_gen_counter;
            return &connections[i];
        }
    }
    return NULL;
}

/* Phase 55: how long a freed server port stays "hot" (tcp_port_served). */
#define TCP_RECENT_TICKS   36        /* ~2 s */

/* Half-open (SYN_RECEIVED) lifetime before reverting to LISTEN: ~5 s */
#define TCP_SYN_RCVD_TICKS  (5 * 18)
/* Phase 55: half-closed (FIN_WAIT_x / LAST_ACK / TIME_WAIT) lifetime: ~30 s */
#define TCP_HALF_CLOSED_TICKS (30 * 18)

/* ---- Phase 55: server ports between listeners ----
 * The polled servers (httpd/vnc/rshell/sync) re-arm their listener on the
 * poll AFTER a client connection ends, but the next client's SYN is handled
 * at IRQ time and could land in that gap and be RST'd (seen with back-to-back
 * HTTP requests). Remember which server ports were just in use, accept such
 * a SYN into an "orphan" TCB, and let tcp_listen() adopt it. */
#define TCP_RECENT_PORTS   4
#define TCP_ORPHAN_TICKS   90        /* un-adopted orphan is reset after ~5 s */
static struct { uint16_t port; uint32_t tick; } recent_ports[TCP_RECENT_PORTS];
static int recent_idx = 0;

/* Release a TCB; a finished server-side connection marks its port hot. */
static void tcp_free_tcb(tcp_conn_t* c, bool record_port) {
    if (record_port && c->passive && c->state != TCP_LISTEN) {
        recent_ports[recent_idx].port = c->local_port;
        recent_ports[recent_idx].tick = system_ticks ? system_ticks : 1;
        recent_idx = (recent_idx + 1) % TCP_RECENT_PORTS;
    }
    c->state  = TCP_CLOSED;
    c->active = false;
}

/* Is `port` a server port whose owner is momentarily without a listener? */
static bool tcp_port_served(uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->passive && c->local_port == port) return true;
    }
    for (int i = 0; i < TCP_RECENT_PORTS; i++) {
        if (recent_ports[i].tick && recent_ports[i].port == port &&
            system_ticks - recent_ports[i].tick <= TCP_RECENT_TICKS) return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * tcp_back_to_listen: Return a passive half-open TCB to LISTEN (only passive
 * opens ever reach SYN_RECEIVED; LISTEN re-inits the rest on the next SYN)
 * -------------------------------------------------------------------------- */
static void tcp_back_to_listen(tcp_conn_t* c) {
    c->state       = TCP_LISTEN;
    c->remote_ip   = 0;
    c->remote_port = 0;
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

    /* Data offset must lie in [20, segment length] (else payload_len wraps) */
    if (data_off < TCP_HEADER_LEN || data_off > len) return;

    /* Verify checksum */
    if (tcp_checksum(src_ip, ip_get_addr(), data, len) != 0) return;

    /* Payload */
    const uint8_t* payload = (const uint8_t*)data + data_off;
    uint16_t payload_len = len - data_off;

    /* Expire stale half-open passive connections back to LISTEN first, so a
     * stray/spoofed SYN can't wedge a listener (checked lazily: it only
     * matters when a new segment arrives). */
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->state == TCP_SYN_RECEIVED &&
            system_ticks - c->last_activity > TCP_SYN_RCVD_TICKS) {
            tcp_back_to_listen(c);
        }
        /* Phase 55: a peer that vanishes mid-close (FIN_WAIT_1/2, LAST_ACK,
         * TIME_WAIT) would pin one of the 8 TCBs forever; reap after ~30 s. */
        if (c->active && (c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2 ||
                          c->state == TCP_LAST_ACK || c->state == TCP_TIME_WAIT) &&
            system_ticks - c->last_activity > TCP_HALF_CLOSED_TICKS) {
            tcp_free_tcb(c, true);
        }
        /* Phase 55: an orphan nobody adopted (server stopped / busy) is reset
         * so the client does not hang on a half-served connection. */
        if (c->active && c->orphan && c->state != TCP_LISTEN &&
            system_ticks - c->last_activity > TCP_ORPHAN_TICKS) {
            tcp_send_rst(c->remote_ip, c->remote_port, c->local_port, c->snd_nxt, c->rcv_nxt);
            tcp_free_tcb(c, false);
        }
    }

    /* Find matching TCB */
    tcp_conn_t* conn = find_connection(src_ip, src_port, dst_port);

    if (!conn) {
        /* Phase 55: a bare SYN for a server port that is between listeners is
         * accepted into an orphan TCB (adopted by the next tcp_listen on that
         * port) instead of being reset — see tcp_port_served(). */
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK | TCP_FLAG_RST)) == TCP_FLAG_SYN &&
            tcp_port_served(dst_port)) {
            conn = alloc_connection();
            if (conn) {
                conn->local_ip   = ip_get_addr();
                conn->local_port = dst_port;
                conn->rcv_wnd    = TCP_WINDOW_SIZE;
                conn->state      = TCP_LISTEN;      /* handled by the LISTEN case below */
                conn->passive    = true;
                conn->orphan     = true;
            }
        }
        if (!conn) {
            /* No connection — send RST if not a RST */
            if (!(flags & TCP_FLAG_RST)) {
                tcp_send_rst(src_ip, src_port, dst_port, 0, seq + 1);
            }
            return;
        }
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
            /* RST aborts the half-open connection back to LISTEN */
            if (flags & TCP_FLAG_RST) { tcp_back_to_listen(conn); return; }
            /* Peer re-sent its SYN: our SYN-ACK was lost (or dropped on an
             * ARP miss) — resend it with the same ISN. */
            if ((flags & TCP_FLAG_SYN) && seq + 1 == conn->rcv_nxt) {
                conn->snd_nxt = conn->snd_una;
                tcp_send_segment(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                return;
            }
            /* Only the ACK of our SYN-ACK completes the handshake; anything
             * else is ignored and does not refresh the timeout. */
            if (!(flags & TCP_FLAG_ACK) || ack != conn->snd_nxt) return;
            conn->snd_una   = ack;
            conn->state     = TCP_ESTABLISHED;
            conn->connected = true;
            break;

        case TCP_ESTABLISHED:
            /* Handle RST */
            if (flags & TCP_FLAG_RST) {
                conn->closed = true;
                tcp_free_tcb(conn, true);
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
                    /* Phase 55: there is no 2*MSL timer, so a TCB left in
                     * TIME_WAIT was only freed by a *further* packet that
                     * never comes -> every server-side close leaked a slot
                     * (httpd closes after each response). Free it now; a
                     * retransmitted FIN just gets a harmless RST. */
                    tcp_free_tcb(conn, true);
                    return;
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
                tcp_free_tcb(conn, true);       /* Phase 55: see FIN_WAIT_1 */
                return;
            }
            break;

        case TCP_CLOSE_WAIT:
            /* Waiting for application to close */
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_FLAG_ACK) {
                tcp_free_tcb(conn, true);
                return;
            }
            break;

        case TCP_TIME_WAIT:
            /* Wait 2*MSL then close — simplified: close immediately */
            tcp_free_tcb(conn, true);
            return;

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
    /* Phase 55: adopt a client that connected while this port was between
     * listeners (oldest first) — the caller sees it as its new connection. */
    tcp_conn_t* best = NULL;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->orphan && c->local_port == port &&
            (!best || (int32_t)(c->last_activity - best->last_activity) < 0))
            best = c;
    }
    if (best) { best->orphan = false; return best; }

    tcp_conn_t* conn = alloc_connection();
    if (!conn) return NULL;

    conn->local_ip   = ip_get_addr();
    conn->local_port = port;
    conn->rcv_wnd    = TCP_WINDOW_SIZE;
    conn->state      = TCP_LISTEN;
    conn->passive    = true;            /* Phase 55: server-side TCB */

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

    /* The timer-IRQ net_poll appends to rx_buf/rx_len: do the copy-out and
     * shift with interrupts off (save/restore EFLAGS, as keyboard_inject_char
     * does) so an arriving segment can't be lost or torn. */
    uint32_t eflags;
    int ret;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
    if (conn->rx_len == 0) {
        ret = conn->closed ? -1 : 0;    /* closed : no data yet */
    } else {
        uint16_t copy_len = conn->rx_len < max_len ? conn->rx_len : max_len;
        memcpy(buf, conn->rx_buf, copy_len);

        /* Shift remaining data forward */
        if (copy_len < conn->rx_len) {
            memcpy(conn->rx_buf, conn->rx_buf + copy_len,
                   conn->rx_len - copy_len);
        }
        conn->rx_len -= copy_len;
        conn->data_available = (conn->rx_len > 0);
        ret = (int)copy_len;
    }
    __asm__ volatile("push %0; popf" :: "r"(eflags) : "memory", "cc");
    return ret;
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
            tcp_free_tcb(conn, false);   /* server stop / aborted connect: port not "hot" */
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
