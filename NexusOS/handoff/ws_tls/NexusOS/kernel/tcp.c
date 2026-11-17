/* ============================================================================
 * NexusOS — TCP (Transmission Control Protocol) — Phase 27 / 55 / 57
 * ============================================================================
 * Connection-oriented, reliable transport with 3-way handshake, sequence
 * number tracking and — since Phase 57 — a real retransmission queue,
 * window-limited sending, a 16 KB advertised receive window, in-order-only
 * receive with immediate duplicate ACKs, SYN/FIN retransmission and
 * zero-window probing.
 *
 * Concurrency model: tcp_handle_packet()/tcp_timer() run inside net_poll(),
 * which is called both from the timer IRQ (IF=0) and from task-context idle
 * pumps (guarded by net_busy). The application side (tcp_send/recv/close/
 * connect) runs in task context and disables interrupts around every TCB
 * mutation so an IRQ-time ACK or data segment cannot tear a TCB in the middle
 * of an update. Nothing here ever blocks with IF=0.
 * ============================================================================ */

#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "vga.h"
#include "string.h"

/* Connection table (8 x ~24.7 KB -> .bss) */
static tcp_conn_t connections[TCP_MAX_CONNECTIONS];

/* ISN (Initial Sequence Number) counter — incremented per connection */
static uint32_t tcp_isn_counter = 0x1000;

static tcp_stats_t tcp_stats;

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */
#define SEQ_DIFF(a, b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)))
#define NOW()           (system_ticks ? system_ticks : 1)

static inline uint32_t irq_save(void) {
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
    return eflags;
}
static inline void irq_restore(uint32_t eflags) {
    __asm__ volatile("push %0; popf" :: "r"(eflags) : "memory", "cc");
}
static inline bool irqs_enabled(void) {
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0" : "=r"(eflags));
    return (eflags & 0x200) != 0;
}

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
    memset(&tcp_stats, 0, sizeof(tcp_stats));
    tcp_isn_counter = system_ticks * 64 + 0x1000; /* Semi-random ISN */

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("TCP initialized (16 KB window, retransmission)\n");
}

const tcp_stats_t* tcp_get_stats(void) { return &tcp_stats; }

/* --------------------------------------------------------------------------
 * tcp_checksum: Compute TCP checksum with pseudo-header
 * -------------------------------------------------------------------------- */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void* tcp_data, uint16_t tcp_len) {
    tcp_pseudo_header_t pseudo;
    pseudo.src_ip     = htonl(src_ip);
    pseudo.dst_ip     = htonl(dst_ip);
    pseudo.zero       = 0;
    pseudo.protocol   = IP_PROTO_TCP;
    pseudo.tcp_length = htons(tcp_len);

    uint32_t sum = 0;
    const uint16_t* ptr = (const uint16_t*)&pseudo;
    for (int i = 0; i < 6; i++) sum += ptr[i];

    ptr = (const uint16_t*)tcp_data;
    uint16_t remaining = tcp_len;
    while (remaining > 1) { sum += *ptr++; remaining -= 2; }
    if (remaining == 1) sum += *(const uint8_t*)ptr;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* --------------------------------------------------------------------------
 * find_connection: Find TCB matching a packet
 * -------------------------------------------------------------------------- */
static tcp_conn_t* find_connection(uint32_t remote_ip, uint16_t remote_port,
                                   uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->state != TCP_LISTEN && c->local_port == local_port &&
            c->remote_ip == remote_ip && c->remote_port == remote_port) {
            return c;
        }
    }
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->state == TCP_LISTEN && c->local_port == local_port) {
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
            tcp_conn_t* c = &connections[i];
            memset(c, 0, sizeof(tcp_conn_t));
            c->active = true;
            c->gen = ++tcp_gen_counter;
            c->snd_mss = TCP_MSS;
            c->rto = TCP_RTO_INIT;
            c->rcv_wnd = TCP_WINDOW_SIZE;
            c->last_adv_wnd = TCP_WINDOW_SIZE;
            return c;
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
/* Phase 57: SYN retransmissions before an active open gives up */
#define TCP_SYN_RETRIES     4

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
        recent_ports[recent_idx].tick = NOW();
        recent_idx = (recent_idx + 1) % TCP_RECENT_PORTS;
    }
    c->state     = TCP_CLOSED;
    c->active    = false;
    c->rtx_timer = 0;
    c->probe_timer = 0;
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
 * tcp_back_to_listen: Return a passive half-open TCB to LISTEN
 * -------------------------------------------------------------------------- */
static void tcp_back_to_listen(tcp_conn_t* c) {
    c->state       = TCP_LISTEN;
    c->remote_ip   = 0;
    c->remote_port = 0;
    c->rtx_timer   = 0;
    c->rx_len      = 0;
    c->tx_len      = 0;
    c->tx_unacked  = 0;
    c->fin_queued  = false;
    c->fin_sent    = false;
}

/* --------------------------------------------------------------------------
 * tcp_xmit: Build and send one segment with explicit sequence number.
 * Does NOT touch snd_nxt (callers do). Advertises the current free window.
 * -------------------------------------------------------------------------- */
static int tcp_xmit(tcp_conn_t* conn, uint32_t seq, uint8_t flags,
                    const void* data, uint16_t data_len) {
    uint8_t pkt[TCP_HEADER_LEN + 4 + TCP_MSS];
    uint16_t hdr_len = TCP_HEADER_LEN;

    if (data_len > TCP_MSS) return -1;

    tcp_header_t* hdr = (tcp_header_t*)pkt;
    if (flags & TCP_FLAG_SYN) {
        /* MSS option: kind 2, len 4, 1460 */
        pkt[20] = 2; pkt[21] = 4; pkt[22] = (TCP_MSS >> 8) & 0xFF; pkt[23] = TCP_MSS & 0xFF;
        hdr_len += 4;
    }

    uint16_t wnd = (uint16_t)(TCP_RX_BUF_SIZE - conn->rx_len);
    conn->rcv_wnd = wnd;
    conn->last_adv_wnd = wnd;

    hdr->src_port   = htons(conn->local_port);
    hdr->dst_port   = htons(conn->remote_port);
    hdr->seq_num    = htonl(seq);
    hdr->ack_num    = htonl(conn->rcv_nxt);
    hdr->data_off   = (uint8_t)((hdr_len / 4) << 4);
    hdr->flags      = flags;
    hdr->window     = htons(wnd);
    hdr->checksum   = 0;
    hdr->urgent_ptr = 0;

    if (data && data_len > 0) memcpy(pkt + hdr_len, data, data_len);

    uint16_t tcp_len = hdr_len + data_len;
    hdr->checksum = tcp_checksum(conn->local_ip, conn->remote_ip, pkt, tcp_len);

    conn->last_activity = system_ticks;
    tcp_stats.segs_sent++;
    tcp_stats.bytes_sent += data_len;
    return ip_send_packet(conn->remote_ip, IP_PROTO_TCP, pkt, tcp_len);
}

static inline void tcp_send_ack(tcp_conn_t* conn) {
    tcp_xmit(conn, conn->snd_nxt, TCP_FLAG_ACK, NULL, 0);
}

/* Arm the retransmission timer if it is idle */
static inline void tcp_arm_rtx(tcp_conn_t* c) {
    if (c->rtx_timer == 0) { c->rtx_timer = NOW(); c->rtx_first = c->rtx_timer; }
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

    hdr->checksum = tcp_checksum(ip_get_addr(), remote_ip, pkt, TCP_HEADER_LEN);
    tcp_stats.rst_sent++;
    ip_send_packet(remote_ip, IP_PROTO_TCP, pkt, TCP_HEADER_LEN);
}

/* --------------------------------------------------------------------------
 * tcp_output: transmit queued data as far as the peer window allows, then
 * the FIN if the app has closed. Called with interrupts off or from within
 * net_poll (never re-entered).
 * -------------------------------------------------------------------------- */
static void tcp_output(tcp_conn_t* c) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT &&
        c->state != TCP_FIN_WAIT_1 && c->state != TCP_LAST_ACK) return;

    for (;;) {
        uint16_t unsent = c->tx_len - c->tx_unacked;
        if (unsent == 0) break;
        uint32_t usable = (c->snd_wnd > c->tx_unacked) ? (uint32_t)(c->snd_wnd - c->tx_unacked) : 0;
        if (usable == 0) {
            /* Zero (or exhausted) window: arm the persist timer when nothing
             * is in flight to carry the window update back to us. */
            if (c->tx_unacked == 0 && c->probe_timer == 0) c->probe_timer = NOW();
            break;
        }
        uint16_t n = unsent;
        if (n > usable)    n = (uint16_t)usable;
        if (n > c->snd_mss) n = c->snd_mss;
        uint8_t flags = TCP_FLAG_ACK;
        if (n == unsent) flags |= TCP_FLAG_PSH;
        tcp_xmit(c, c->snd_nxt, flags, c->tx_buf + c->tx_unacked, n);
        c->snd_nxt    += n;
        c->tx_unacked += n;
        c->probe_timer = 0;
        tcp_arm_rtx(c);
    }

    if (c->tx_len == c->tx_unacked && c->fin_queued && !c->fin_sent) {
        tcp_xmit(c, c->snd_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        c->snd_nxt++;
        c->fin_sent = true;
        tcp_arm_rtx(c);
    }
}

/* --------------------------------------------------------------------------
 * tcp_retransmit: resend the oldest unacknowledged segment (SYN, data or FIN)
 * -------------------------------------------------------------------------- */
static void tcp_retransmit(tcp_conn_t* c, bool backoff) {
    if (c->state == TCP_SYN_SENT) {
        tcp_xmit(c, c->snd_una, TCP_FLAG_SYN, NULL, 0);
    } else if (c->state == TCP_SYN_RECEIVED) {
        tcp_xmit(c, c->snd_una, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
    } else if (c->tx_unacked > 0) {
        uint16_t n = c->tx_unacked;
        if (n > c->snd_mss) n = c->snd_mss;
        uint8_t flags = TCP_FLAG_ACK | TCP_FLAG_PSH;
        if (c->fin_sent && n == c->tx_unacked) flags |= TCP_FLAG_FIN;
        tcp_xmit(c, c->snd_una, flags, c->tx_buf, n);
    } else if (c->fin_sent) {
        tcp_xmit(c, c->snd_una, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    } else {
        c->rtx_timer = 0;               /* nothing outstanding after all */
        return;
    }
    if (backoff) {
        tcp_stats.retransmits++;
        c->rtx_count++;
        c->rto *= 2;
        if (c->rto > TCP_RTO_MAX) c->rto = TCP_RTO_MAX;
    } else {
        tcp_stats.fast_retransmits++;
    }
    c->rtx_timer = NOW();
}

/* Abort a connection that made no progress: RST the peer, free the slot. */
static void tcp_abort(tcp_conn_t* c) {
    if (c->state != TCP_SYN_SENT && c->state != TCP_LISTEN)
        tcp_send_rst(c->remote_ip, c->remote_port, c->local_port, c->snd_nxt, c->rcv_nxt);
    c->closed = true;
    tcp_stats.giveups++;
    tcp_free_tcb(c, true);
}

/* --------------------------------------------------------------------------
 * tcp_reap: expire stale half-open / half-closed / orphan TCBs
 * -------------------------------------------------------------------------- */
static void tcp_reap(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (!c->active) continue;
        if (c->state == TCP_SYN_RECEIVED &&
            system_ticks - c->last_activity > TCP_SYN_RCVD_TICKS) {
            tcp_back_to_listen(c);
        }
        /* Phase 55: a peer that vanishes mid-close (FIN_WAIT_1/2, LAST_ACK,
         * TIME_WAIT) would pin one of the 8 TCBs forever; reap after ~30 s. */
        if (c->active && (c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2 ||
                          c->state == TCP_LAST_ACK || c->state == TCP_TIME_WAIT ||
                          c->state == TCP_CLOSING) &&
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
}

/* --------------------------------------------------------------------------
 * tcp_timer: retransmission / persist / window-update timers (every tick)
 * -------------------------------------------------------------------------- */
void tcp_timer(void) {
    uint32_t now = NOW();
    tcp_reap();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (!c->active || c->state == TCP_LISTEN) continue;

        /* Retransmission timeout */
        if (c->rtx_timer && (now - c->rtx_timer) >= c->rto) {
            bool give_up = (now - c->rtx_first) > TCP_GIVEUP_TICKS ||
                           (c->state == TCP_SYN_SENT && c->rtx_count >= TCP_SYN_RETRIES);
            if (give_up) { tcp_abort(c); continue; }
            if (c->state == TCP_SYN_RECEIVED && c->rtx_count >= 3) continue; /* LISTEN reverts it */
            tcp_retransmit(c, true);
            continue;
        }

        /* Zero-window probe: one byte of new data after RTO of silence */
        if (c->probe_timer && c->tx_unacked == 0 && c->tx_len > 0 &&
            (now - c->probe_timer) >= c->rto &&
            (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT ||
             c->state == TCP_FIN_WAIT_1 || c->state == TCP_LAST_ACK)) {
            tcp_xmit(c, c->snd_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH, c->tx_buf, 1);
            c->snd_nxt++;
            c->tx_unacked = 1;
            c->probe_timer = 0;
            tcp_stats.zero_wnd_probes++;
            tcp_arm_rtx(c);
            continue;
        }

        /* Window update: the app drained a buffer we had advertised as
         * (nearly) full — tell the peer it may send again. Covers vnc.c,
         * which empties rx_buf directly instead of through tcp_recv(). */
        if ((c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
             c->state == TCP_FIN_WAIT_2) &&
            c->last_adv_wnd < 2 * TCP_MSS &&
            (TCP_RX_BUF_SIZE - c->rx_len) >= TCP_RX_BUF_SIZE / 2) {
            tcp_send_ack(c);
        }
    }
}

/* --------------------------------------------------------------------------
 * tcp_parse_mss: read the MSS option out of a SYN / SYN-ACK
 * -------------------------------------------------------------------------- */
static uint16_t tcp_parse_mss(const uint8_t* opts, int len) {
    int i = 0;
    while (i < len) {
        uint8_t kind = opts[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t olen = opts[i + 1];
        if (olen < 2 || i + olen > len) break;
        if (kind == 2 && olen == 4) return (uint16_t)((opts[i + 2] << 8) | opts[i + 3]);
        i += olen;
    }
    return 0;
}

static void tcp_learn_mss(tcp_conn_t* c, const uint8_t* seg, uint8_t data_off) {
    uint16_t mss = tcp_parse_mss(seg + TCP_HEADER_LEN, data_off - TCP_HEADER_LEN);
    if (mss >= 536 && mss < TCP_MSS) c->snd_mss = mss;
    else c->snd_mss = TCP_MSS;
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
    uint16_t window   = ntohs(hdr->window);
    uint8_t  flags    = hdr->flags;
    uint8_t  data_off = (uint8_t)((hdr->data_off >> 4) * 4);

    /* Data offset must lie in [20, segment length] (else payload_len wraps) */
    if (data_off < TCP_HEADER_LEN || data_off > len) return;

    /* Verify checksum */
    if (tcp_checksum(src_ip, ip_get_addr(), data, len) != 0) return;

    const uint8_t* payload = (const uint8_t*)data + data_off;
    uint16_t payload_len = len - data_off;
    tcp_stats.segs_rcvd++;

    tcp_reap();

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
                conn->state      = TCP_LISTEN;      /* handled by the LISTEN case below */
                conn->passive    = true;
                conn->orphan     = true;
            }
        }
        if (!conn) {
            if (!(flags & TCP_FLAG_RST)) {
                tcp_send_rst(src_ip, src_port, dst_port,
                             (flags & TCP_FLAG_ACK) ? ack : 0, seq + payload_len +
                             ((flags & TCP_FLAG_SYN) ? 1 : 0) + ((flags & TCP_FLAG_FIN) ? 1 : 0));
            }
            return;
        }
    }

    /* ---- Non-synchronized states ---- */
    switch (conn->state) {
        case TCP_LISTEN:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK | TCP_FLAG_RST)) == TCP_FLAG_SYN) {
                conn->remote_ip   = src_ip;
                conn->remote_port = src_port;
                conn->rcv_nxt     = seq + 1;
                conn->snd_nxt     = tcp_isn_counter++;
                conn->snd_una     = conn->snd_nxt;
                conn->snd_wnd     = window;
                conn->rx_len      = 0;
                conn->tx_len      = 0;
                conn->tx_unacked  = 0;
                conn->fin_queued  = false;
                conn->fin_sent    = false;
                conn->closed      = false;
                conn->connected   = false;
                conn->data_available = false;
                conn->rto         = TCP_RTO_INIT;
                conn->rtx_count   = 0;
                conn->dupacks     = 0;
                conn->state       = TCP_SYN_RECEIVED;
                tcp_learn_mss(conn, (const uint8_t*)data, data_off);

                tcp_xmit(conn, conn->snd_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                conn->snd_nxt++;
                conn->rtx_timer = NOW(); conn->rtx_first = conn->rtx_timer;
            }
            conn->last_activity = system_ticks;
            return;

        case TCP_SYN_SENT:
            if (flags & TCP_FLAG_RST) {
                if ((flags & TCP_FLAG_ACK) && ack == conn->snd_nxt) {
                    /* connection refused */
                    conn->closed = true;
                    tcp_stats.rst_rcvd++;
                    tcp_free_tcb(conn, false);
                }
                return;
            }
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                if (ack != conn->snd_nxt) {
                    tcp_send_rst(src_ip, src_port, dst_port, ack, 0);
                    return;
                }
                conn->snd_una   = ack;
                conn->rcv_nxt   = seq + 1;
                conn->snd_wnd   = window;
                conn->rtx_timer = 0;
                conn->rtx_count = 0;
                conn->rto       = TCP_RTO_INIT;
                conn->state     = TCP_ESTABLISHED;
                conn->connected = true;
                tcp_learn_mss(conn, (const uint8_t*)data, data_off);
                tcp_send_ack(conn);
                conn->last_activity = system_ticks;
                /* Data riding on the SYN-ACK is unusual; let the peer resend it. */
                tcp_output(conn);
            }
            return;

        case TCP_SYN_RECEIVED:
            if (flags & TCP_FLAG_RST) { tcp_back_to_listen(conn); return; }
            /* Peer re-sent its SYN: our SYN-ACK was lost — resend it. */
            if ((flags & TCP_FLAG_SYN) && seq + 1 == conn->rcv_nxt) {
                tcp_xmit(conn, conn->snd_una, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                return;
            }
            /* Only the ACK of our SYN-ACK completes the handshake */
            if (!(flags & TCP_FLAG_ACK) || ack != conn->snd_nxt) return;
            conn->snd_una   = ack;
            conn->snd_wnd   = window;
            conn->rtx_timer = 0;
            conn->rtx_count = 0;
            conn->state     = TCP_ESTABLISHED;
            conn->connected = true;
            break;      /* fall into synchronized processing (piggybacked data) */

        default:
            break;
    }

    /* ---- Synchronized states ---- */

    /* RST: abort */
    if (flags & TCP_FLAG_RST) {
        conn->closed = true;
        tcp_stats.rst_rcvd++;
        tcp_free_tcb(conn, true);
        return;
    }
    /* A SYN in a synchronized state is ignored */
    if (flags & TCP_FLAG_SYN) return;
    /* Every segment in a synchronized state carries ACK */
    if (!(flags & TCP_FLAG_ACK)) return;

    bool need_ack = false;

    /* ACK processing */
    int32_t acked = SEQ_DIFF(ack, conn->snd_una);
    if (acked > 0) {
        if (SEQ_DIFF(ack, conn->snd_nxt) > 0) {
            /* ACK for something not yet sent: ack back, drop */
            tcp_send_ack(conn);
            return;
        }
        uint32_t n = (uint32_t)acked;
        uint16_t data_acked = (n > conn->tx_unacked) ? conn->tx_unacked : (uint16_t)n;
        if (data_acked > 0) {
            uint16_t rest = conn->tx_len - data_acked;
            if (rest > 0) memcpy(conn->tx_buf, conn->tx_buf + data_acked, rest);
            conn->tx_len      = rest;
            conn->tx_unacked -= data_acked;
        }
        bool fin_acked = conn->fin_sent && ack == conn->snd_nxt;
        conn->snd_una  = ack;
        conn->snd_wnd  = window;
        conn->dupacks  = 0;
        conn->rtx_count = 0;
        conn->rto      = TCP_RTO_INIT;
        if (conn->snd_una == conn->snd_nxt) conn->rtx_timer = 0;
        else { conn->rtx_timer = NOW(); conn->rtx_first = conn->rtx_timer; }

        if (fin_acked) {
            if (conn->state == TCP_FIN_WAIT_1) conn->state = TCP_FIN_WAIT_2;
            else if (conn->state == TCP_CLOSING || conn->state == TCP_LAST_ACK) {
                tcp_free_tcb(conn, true);
                return;
            }
        }
    } else if (acked == 0) {
        /* Duplicate ACK (same ack, no data, window unchanged) while data is
         * in flight -> after three, retransmit the head segment at once. */
        if (payload_len == 0 && !(flags & TCP_FLAG_FIN) && conn->tx_unacked > 0 &&
            window == conn->snd_wnd) {
            if (++conn->dupacks == 3) tcp_retransmit(conn, false);
        }
        conn->snd_wnd = window;
    }
    /* acked < 0: old ACK, ignore the ACK part */

    /* Data processing */
    uint32_t fin_seq = seq + payload_len;           /* sequence number of a FIN */
    if (payload_len > 0) {
        if (conn->state == TCP_ESTABLISHED || conn->state == TCP_FIN_WAIT_1 ||
            conn->state == TCP_FIN_WAIT_2) {
            int32_t off = SEQ_DIFF(conn->rcv_nxt, seq);
            if (off < 0) {
                /* Out of order (a gap before it): drop, duplicate ACK */
                tcp_stats.ooo_dropped++;
                tcp_stats.dupacks_sent++;
                need_ack = true;
            } else if (off >= (int32_t)payload_len) {
                /* Entirely old data: duplicate ACK */
                tcp_stats.dupacks_sent++;
                need_ack = true;
            } else {
                payload     += off;
                payload_len -= (uint16_t)off;
                uint16_t space = (uint16_t)(TCP_RX_BUF_SIZE - conn->rx_len);
                uint16_t copy  = payload_len < space ? payload_len : space;
                if (copy > 0) {
                    memcpy(conn->rx_buf + conn->rx_len, payload, copy);
                    conn->rx_len  += copy;
                    conn->rcv_nxt += copy;
                    conn->data_available = true;
                    tcp_stats.bytes_rcvd += copy;
                }
                if (copy < payload_len) tcp_stats.rx_full_drops += payload_len - copy;
                need_ack = true;
            }
        } else {
            need_ack = true;       /* data after the peer's FIN / in closing states */
        }
    }

    /* FIN processing (only when it is the next expected byte) */
    if (flags & TCP_FLAG_FIN) {
        int32_t d = SEQ_DIFF(fin_seq, conn->rcv_nxt);
        if (d == 0 && (conn->state == TCP_ESTABLISHED || conn->state == TCP_FIN_WAIT_1 ||
                       conn->state == TCP_FIN_WAIT_2)) {
            conn->rcv_nxt++;
            conn->closed = true;
            need_ack = true;
            if (conn->state == TCP_ESTABLISHED) {
                conn->state = TCP_CLOSE_WAIT;
            } else if (conn->state == TCP_FIN_WAIT_1) {
                /* our FIN not yet acked -> CLOSING; acked -> TIME_WAIT (freed) */
                if (conn->fin_sent && conn->snd_una == conn->snd_nxt) {
                    tcp_send_ack(conn);
                    tcp_free_tcb(conn, true);   /* Phase 55: no 2*MSL timer */
                    return;
                }
                conn->state = TCP_CLOSING;
            } else { /* FIN_WAIT_2 */
                tcp_send_ack(conn);
                tcp_free_tcb(conn, true);       /* Phase 55: see above */
                return;
            }
        } else if (d < 0) {
            need_ack = true;                    /* retransmitted FIN: re-ACK */
        }
    }

    if (conn->state == TCP_TIME_WAIT) {
        tcp_send_ack(conn);
        tcp_free_tcb(conn, true);
        return;
    }

    if (need_ack) tcp_send_ack(conn);

    /* The window may have opened / data may have been acked: push more */
    tcp_output(conn);
    conn->last_activity = system_ticks;
}

/* --------------------------------------------------------------------------
 * tcp_connect: Active open (initiate connection)
 * -------------------------------------------------------------------------- */
tcp_conn_t* tcp_connect(uint32_t remote_ip, uint16_t remote_port,
                        uint16_t local_port) {
    uint32_t ef = irq_save();
    tcp_conn_t* conn = alloc_connection();
    if (!conn) { irq_restore(ef); return NULL; }

    conn->local_ip    = ip_get_addr();
    conn->local_port  = local_port;
    conn->remote_ip   = remote_ip;
    conn->remote_port = remote_port;
    conn->snd_una     = tcp_isn_counter++;
    conn->snd_nxt     = conn->snd_una + 1;   /* the SYN occupies one sequence number */
    conn->snd_wnd     = TCP_MSS;        /* until the SYN-ACK tells us */
    conn->state       = TCP_SYN_SENT;
    conn->rtx_timer   = NOW(); conn->rtx_first = conn->rtx_timer;
    irq_restore(ef);

    /* Send SYN with interrupts enabled: the first packet to a new peer may
     * have to wait for ARP (arp_resolve blocks ~1 s only in task context).
     * If it is lost, tcp_timer() retransmits it. */
    tcp_xmit(conn, conn->snd_una, TCP_FLAG_SYN, NULL, 0);
    return conn;
}

/* --------------------------------------------------------------------------
 * tcp_listen: Passive open (wait for connection)
 * -------------------------------------------------------------------------- */
tcp_conn_t* tcp_listen(uint16_t port) {
    uint32_t ef = irq_save();
    /* Phase 55: adopt a client that connected while this port was between
     * listeners (oldest first) — the caller sees it as its new connection. */
    tcp_conn_t* best = NULL;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &connections[i];
        if (c->active && c->orphan && c->local_port == port &&
            (!best || (int32_t)(c->last_activity - best->last_activity) < 0))
            best = c;
    }
    if (best) { best->orphan = false; irq_restore(ef); return best; }

    tcp_conn_t* conn = alloc_connection();
    if (!conn) { irq_restore(ef); return NULL; }

    conn->local_ip   = ip_get_addr();
    conn->local_port = port;
    conn->state      = TCP_LISTEN;
    conn->passive    = true;            /* Phase 55: server-side TCB */
    irq_restore(ef);
    return conn;
}

/* --------------------------------------------------------------------------
 * tcp_ephemeral_port: next free local port in 49152..65535
 * -------------------------------------------------------------------------- */
uint16_t tcp_ephemeral_port(void) {
    static uint16_t next = 49152;
    for (int tries = 0; tries < 64; tries++) {
        uint16_t p = next++;
        if (next < 49152) next = 49152;
        bool used = false;
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++)
            if (connections[i].active && connections[i].local_port == p) { used = true; break; }
        if (!used) return p;
    }
    return next++;
}

/* --------------------------------------------------------------------------
 * tcp_send / tcp_send_nb: queue data for transmission
 * -------------------------------------------------------------------------- */
static bool tcp_can_send(const tcp_conn_t* c) {
    return c && c->active && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)
           && !c->fin_queued;
}

int tcp_send_space(const tcp_conn_t* conn) {
    if (!tcp_can_send(conn)) return 0;
    return TCP_TX_BUF_SIZE - conn->tx_len;
}

/* Queue what fits (interrupts off) and kick the output engine. */
static uint16_t tcp_enqueue(tcp_conn_t* c, const uint8_t* data, uint16_t len) {
    uint32_t ef = irq_save();
    uint16_t n = 0;
    if (tcp_can_send(c)) {
        uint16_t space = (uint16_t)(TCP_TX_BUF_SIZE - c->tx_len);
        n = len < space ? len : space;
        if (n > 0) {
            memcpy(c->tx_buf + c->tx_len, data, n);
            c->tx_len += n;
            tcp_output(c);
        }
    }
    irq_restore(ef);
    return n;
}

int tcp_send_nb(tcp_conn_t* conn, const void* data, uint16_t len) {
    if (!tcp_can_send(conn)) return -1;
    if (len == 0) return 0;
    return (int)tcp_enqueue(conn, (const uint8_t*)data, len);
}

int tcp_send(tcp_conn_t* conn, const void* data, uint16_t len) {
    if (!tcp_can_send(conn)) return -1;
    if (len == 0) return 0;

    const uint8_t* ptr = (const uint8_t*)data;
    uint16_t sent = 0;
    /* We may only wait when interrupts are on and we are not already inside
     * net_poll (the nested poll would be a no-op and hlt would never wake). */
    bool can_block = irqs_enabled() && !net_is_busy();
    uint32_t start = system_ticks;

    while (sent < len) {
        sent += tcp_enqueue(conn, ptr + sent, (uint16_t)(len - sent));
        if (sent >= len) break;
        if (!can_block) break;
        if (!tcp_can_send(conn)) break;
        if (system_ticks - start > TCP_GIVEUP_TICKS) break;
        net_poll();
        __asm__ volatile("hlt");
    }
    if (sent == 0 && !tcp_can_send(conn)) return -1;
    return (int)sent;
}

/* --------------------------------------------------------------------------
 * tcp_recv: Read data from RX buffer
 * -------------------------------------------------------------------------- */
int tcp_recv(tcp_conn_t* conn, void* buf, uint16_t max_len) {
    if (!conn) return -1;

    /* The timer-IRQ net_poll appends to rx_buf/rx_len: do the copy-out and
     * shift with interrupts off so an arriving segment can't be lost or torn. */
    uint32_t eflags = irq_save();
    int ret;
    if (conn->rx_len == 0) {
        ret = conn->closed ? -1 : 0;    /* closed : no data yet */
    } else {
        uint16_t copy_len = conn->rx_len < max_len ? conn->rx_len : max_len;
        memcpy(buf, conn->rx_buf, copy_len);
        if (copy_len < conn->rx_len) {
            memcpy(conn->rx_buf, conn->rx_buf + copy_len, conn->rx_len - copy_len);
        }
        conn->rx_len -= copy_len;
        conn->data_available = (conn->rx_len > 0);
        ret = (int)copy_len;

        /* Window update: we had advertised a (nearly) closed window and the
         * app just made room — tell the peer right away. */
        if (conn->active && conn->last_adv_wnd < 2 * TCP_MSS &&
            (TCP_RX_BUF_SIZE - conn->rx_len) >= TCP_RX_BUF_SIZE / 2 &&
            (conn->state == TCP_ESTABLISHED || conn->state == TCP_FIN_WAIT_1 ||
             conn->state == TCP_FIN_WAIT_2)) {
            tcp_send_ack(conn);
        }
    }
    irq_restore(eflags);
    return ret;
}

/* --------------------------------------------------------------------------
 * tcp_close: Initiate connection close (FIN follows any queued data)
 * -------------------------------------------------------------------------- */
void tcp_close(tcp_conn_t* conn) {
    if (!conn || !conn->active) return;
    uint32_t ef = irq_save();

    switch (conn->state) {
        case TCP_ESTABLISHED:
            conn->state = TCP_FIN_WAIT_1;
            conn->fin_queued = true;
            tcp_output(conn);
            break;

        case TCP_CLOSE_WAIT:
            conn->state = TCP_LAST_ACK;
            conn->fin_queued = true;
            tcp_output(conn);
            break;

        case TCP_LISTEN:
        case TCP_SYN_SENT:
            tcp_free_tcb(conn, false);   /* server stop / aborted connect: port not "hot" */
            break;

        case TCP_SYN_RECEIVED:
            tcp_send_rst(conn->remote_ip, conn->remote_port, conn->local_port,
                         conn->snd_nxt, conn->rcv_nxt);
            tcp_free_tcb(conn, false);
            break;

        default:
            break;
    }
    irq_restore(ef);
}

/* --------------------------------------------------------------------------
 * tcp_get_connections: Return connection table for display
 * -------------------------------------------------------------------------- */
const tcp_conn_t* tcp_get_connections(int* count) {
    if (count) *count = TCP_MAX_CONNECTIONS;
    return connections;
}
