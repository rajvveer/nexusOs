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
 *
 * Native x86-64 (NEXUS_NATIVE64) differs only where marked: TCBs carry
 * dynamically sized receive/send rings (default 256 KiB each), RFC 7323 window
 * scaling, 32-bit window/length fields, out-of-order reassembly straight into
 * the receive ring, ACK coalescing per receive batch plus a short delayed ACK,
 * prompt receiver window updates, RTT estimation and keepalive. The legacy
 * i386 build keeps its 16 KB / 8 KB in-TCB buffers and behaviour.
 * ============================================================================ */

#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "vga.h"
#include "string.h"
#include "irqflags.h"

/* Connection table (8 x ~24.7 KB -> .bss) */
#ifdef NEXUS_NATIVE64
#include "../arch/x86_64/native.h"
/* One allocation per connection: the slot, then the receive and send rings. */
struct tcp_slot { struct tcp_slot* next; unsigned references; uint64_t bytes; tcp_conn_t connection; };
static struct tcp_slot* connections;
static struct tcp_slot* slot_of(tcp_conn_t* connection) {
    return (void*)((uint8_t*)connection - __builtin_offsetof(struct tcp_slot, connection));
}
void tcp_retain(tcp_conn_t* connection) {
    struct tcp_slot* slot = slot_of(connection);
    if (slot->references == ~0U) panic("TCP reference overflow");
    slot->references++;
}
void tcp_release(tcp_conn_t* connection) {
    struct tcp_slot* slot = slot_of(connection);
    if (!slot->references) panic("TCP reference underflow");
    slot->references--;
}
void tcp_collect(void) {
    struct tcp_slot** link = &connections;
    while (*link) {
        struct tcp_slot* slot = *link;
        if (!slot->references && !slot->connection.active) { *link = slot->next; arena_free((uint64_t)slot, slot->bytes); }
        else link = &slot->next;
    }
}
uint64_t tcp_background_pages(void) {
    uint64_t pages = 0;
    for (struct tcp_slot* s = connections; s; s = s->next)
        if (!s->references && s->connection.active) pages += (s->bytes + 4095) / 4096;
    return pages;
}
bool tcp_port_in_use(uint16_t port, uint32_t ip) {
    for (struct tcp_slot* s = connections; s; s = s->next) {
        const tcp_conn_t* c = &s->connection;
        if (c->active && c->state != TCP_LISTEN && c->local_port == port && (!ip || c->local_ip == ip)) return true;
    }
    return false;
}
static tcp_conn_t* next_connection(tcp_conn_t* previous) {
    struct tcp_slot* slot = previous ? slot_of(previous)->next : connections;
    return slot ? &slot->connection : NULL;
}
static bool next_isn(uint32_t* value) { return native_random(value, sizeof(*value)); }
/* Ring arithmetic: base < cap and offset <= cap, so one subtraction wraps. */
static inline uint32_t ring_pos(uint32_t base, uint32_t offset, uint32_t cap) {
    uint32_t position = base + offset;
    return position >= cap ? position - cap : position;
}
static void ring_write(uint8_t* ring, uint32_t cap, uint32_t position, const uint8_t* data, uint32_t length) {
    uint32_t first = cap - position < length ? cap - position : length;
    memcpy(ring + position, data, first);
    if (length > first) memcpy(ring, data + first, length - first);
}
static void ring_read(const uint8_t* ring, uint32_t cap, uint32_t position, uint8_t* out, uint32_t length) {
    uint32_t first = cap - position < length ? cap - position : length;
    memcpy(out, ring + position, first);
    if (length > first) memcpy(out + first, ring, length - first);
}
/* Smallest RFC 7323 shift whose maximum window covers the receive ring. */
static uint8_t tcp_scale_for(uint32_t capacity) {
    uint8_t shift = 0;
    while (shift < 14 && (65535U << shift) < capacity) shift++;
    return shift;
}
uint32_t tcp_buffer_size(uint64_t requested, uint32_t fallback) {
    if (!requested) return fallback;
    if (requested < TCP_BUF_MIN) return TCP_BUF_MIN;
    if (requested > TCP_BUF_MAX) return TCP_BUF_MAX;
    return (uint32_t)((requested + 4095) & ~4095ULL);
}
#else
static tcp_conn_t connections[TCP_MAX_CONNECTIONS];

/* ISN (Initial Sequence Number) counter — incremented per connection */
static uint32_t tcp_isn_counter = 0x1000;
static tcp_conn_t* next_connection(tcp_conn_t* previous) {
    if (!previous) return connections;
    return previous + 1 < connections + TCP_MAX_CONNECTIONS ? previous + 1 : NULL;
}
static bool next_isn(uint32_t* value) { *value = tcp_isn_counter++; return true; }
#endif

static tcp_stats_t tcp_stats;

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Small helpers
 * -------------------------------------------------------------------------- */
#define SEQ_DIFF(a, b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)))
#define NOW()           (system_ticks ? system_ticks : 1)

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
#ifdef NEXUS_NATIVE64
    if (connections) panic("reset of owned TCP connections");
#else
    memset(connections, 0, sizeof(connections));
    tcp_isn_counter = system_ticks * 64 + 0x1000; /* Semi-random ISN */
#endif
    memset(&tcp_stats, 0, sizeof(tcp_stats));

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
#ifdef NEXUS_NATIVE64
    vga_print("TCP initialized (256 KiB rings, window scaling, reassembly)\n");
#else
    vga_print("TCP initialized (16 KB window, retransmission)\n");
#endif
}

const tcp_stats_t* tcp_get_stats(void) { return &tcp_stats; }

/* --------------------------------------------------------------------------
 * tcp_checksum: Compute TCP checksum with pseudo-header
 * -------------------------------------------------------------------------- */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void* tcp_data, uint16_t tcp_len) {
#ifdef NEXUS_NATIVE64
    if (tcp_handle6(src_ip) || tcp_handle6(dst_ip)) return tcp_checksum6(src_ip, dst_ip, tcp_data, tcp_len);   /* IPv6 */
#endif
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
                                   uint32_t local_ip, uint16_t local_port) {
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
        if (c->active && c->state != TCP_LISTEN && c->local_port == local_port &&
            c->remote_ip == remote_ip && c->remote_port == remote_port && c->local_ip == local_ip) {
            return c;
        }
    }
    /* A listener bound to this very address wins over a wildcard one; other addresses never match. */
    tcp_conn_t* wildcard = NULL;
#ifdef NEXUS_NATIVE64
    /* Several SO_REUSEPORT listeners of the winning kind share the clients by a hash of the client (Linux). */
    unsigned exact = 0, any = 0;
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
        if (!c->active || c->state != TCP_LISTEN || c->local_port != local_port) continue;
        if (c->listen_family && (c->listen_family == 6) != tcp_handle6(local_ip)) continue;   /* IPV6_V6ONLY / AF_INET */
        if (c->listener && c->listen_ip) exact += c->listen_ip == local_ip;
        else any++;
    }
    unsigned pick = (remote_ip * 2654435761U ^ remote_port * 40503U) % (exact ? exact : any ? any : 1);
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
        if (!c->active || c->state != TCP_LISTEN || c->local_port != local_port) continue;
        if (c->listen_family && (c->listen_family == 6) != tcp_handle6(local_ip)) continue;
        if ((c->listener && c->listen_ip) ? (!exact || c->listen_ip != local_ip) : exact != 0) continue;
        if (!pick--) return c;
    }
#else
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
        if (!c->active || c->state != TCP_LISTEN || c->local_port != local_port) continue;
        if (!wildcard) wildcard = c;
    }
#endif
    return wildcard;
}
#ifdef NEXUS_NATIVE64
bool tcp_tuple_in_use(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    tcp_conn_t* c = find_connection(remote_ip, remote_port, local_ip, local_port);
    return c && c->state != TCP_LISTEN;
}
#endif

/* --------------------------------------------------------------------------
 * alloc_connection: Get a free TCB slot
 * -------------------------------------------------------------------------- */
static uint32_t tcp_gen_counter = 0;   /* Phase 55: see tcp_conn_t.gen */

#ifdef NEXUS_NATIVE64
static tcp_conn_t* alloc_connection_buffers(uint32_t rx_cap, uint32_t tx_cap) {
    uint64_t header = (sizeof(struct tcp_slot) + 63) & ~63ULL;
    struct tcp_slot* slot = (void*)arena_alloc(header + rx_cap + tx_cap);
    if (!slot) return NULL;
    slot->bytes = header + rx_cap + tx_cap;
    slot->next = connections; connections = slot;
    tcp_conn_t* c = &slot->connection;
    c->active = true; c->gen = ++tcp_gen_counter; c->snd_mss = TCP_MSS;
    c->rto = TCP_RTO_INIT;
    c->rx_buf = (uint8_t*)slot + header; c->rx_cap = rx_cap;
    c->tx_buf = c->rx_buf + rx_cap; c->tx_cap = tx_cap;
    c->rcv_wscale = tcp_scale_for(rx_cap);
    c->rcv_wnd = c->last_adv_wnd = rx_cap < 65535 ? rx_cap : 65535;
    c->keep_idle_s = 7200; c->keep_intvl_s = 75; c->keep_cnt = 9;   /* Linux defaults */
    c->last_rx_ns = native_clock_ns();
    return c;
}
#endif

static tcp_conn_t* alloc_connection(void) {
#ifdef NEXUS_NATIVE64
    return alloc_connection_buffers(TCP_RX_BUF_SIZE, TCP_TX_BUF_SIZE);
#else
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
#endif
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

static void tcp_send_rst(uint32_t local_ip, uint32_t remote_ip, uint16_t remote_port,
                         uint16_t local_port, uint32_t seq, uint32_t ack);
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
#ifdef NEXUS_NATIVE64
    c->delack_ns = 0; c->ack_now = c->ack_push = c->ack_flush = false; c->ack_bytes = 0; c->tx_blocked = false;
    if (c->parent) { c->parent->children--; c->parent = NULL; }   /* leaves its listener's queue */
    if (c->listener) {   /* a closed listener resets the connections still waiting for accept() (Linux) */
        for (tcp_conn_t* child = next_connection(NULL); child; child = next_connection(child)) {
            if (!child->active || child->parent != c) continue;
            child->parent = NULL;
            tcp_send_rst(child->local_ip, child->remote_ip, child->remote_port, child->local_port, child->snd_nxt, child->rcv_nxt);
            tcp_free_tcb(child, false);
        }
        c->children = 0;
    }
#endif
}

/* A half-open connection request (a Phase 55 orphan or a socket listener's child) is dropped when it fails: it
 * never turns into a listener itself. */
static bool tcp_request(const tcp_conn_t* c) {
#ifdef NEXUS_NATIVE64
    if (c->parent) return true;
#endif
    return c->orphan;
}

/* Is `port` a server port whose owner is momentarily without a listener? */
static bool tcp_port_served(uint16_t port) {
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
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
    c->closed = c->connected = c->discard_rx = false;
    c->error = 0;
#ifdef NEXUS_NATIVE64
    c->rx_head = c->tx_head = 0;
    c->ooo_count = 0; c->fin_pending = false;
    c->wscale_ok = false; c->snd_wscale = 0; c->rcv_wscale = tcp_scale_for(c->rx_cap);
    c->delack_ns = 0; c->ack_now = c->ack_push = c->ack_flush = false; c->ack_bytes = 0;
    c->rtt_timing = false;
#endif
}

#ifdef NEXUS_NATIVE64
/* A socket listener's child completed its handshake: move it from its small half-open rings to full-size ones
 * (smaller ones under fragmentation, as connect does; the small rings stay if nothing fits). Its rings are still
 * empty and nothing references it before tcp_accept(), so the TCB moves to the new slot and the old is collected. */
static tcp_conn_t* tcp_grow(tcp_conn_t* c) {
    uint32_t rx = TCP_RX_BUF_SIZE, tx = TCP_TX_BUF_SIZE;
    tcp_conn_t* n = alloc_connection_buffers(rx, tx);
    while (!n && (rx > 16384 || tx > 8192)) {
        if (rx > 16384) rx /= 2;
        if (tx > 8192) tx /= 2;
        n = alloc_connection_buffers(rx, tx);
    }
    if (!n) return c;
    uint8_t *rx_buf = n->rx_buf, *tx_buf = n->tx_buf;
    *n = *c;
    n->rx_buf = rx_buf; n->rx_cap = rx; n->tx_buf = tx_buf; n->tx_cap = tx;
    c->active = false; c->parent = NULL;
    return n;
}
#endif

/* --------------------------------------------------------------------------
 * tcp_xmit: Build and send one segment with explicit sequence number.
 * Does NOT touch snd_nxt (callers do). Advertises the current free window.
 * -------------------------------------------------------------------------- */
#ifdef NEXUS_NATIVE64
static bool tcp_tx_waiting;   /* some TCB has tx_blocked set */
/* Window field for an outgoing segment. SYN windows are never scaled; later
 * windows are scaled once both sides offered scaling. The right edge we
 * already offered (rcv_adv) is never retracted. */
static uint16_t tcp_window_field(tcp_conn_t* c, bool syn) {
    unsigned shift = syn || !c->wscale_ok ? 0 : c->rcv_wscale;
    uint32_t space = c->rx_cap - c->rx_len, limit = 65535U << shift;
    if (space > limit) space = limit;
    space &= ~((1U << shift) - 1);
    if (!syn && SEQ_DIFF(c->rcv_adv, c->rcv_nxt + space) > 0) space = c->rcv_adv - c->rcv_nxt;
    uint32_t field = (space + (1U << shift) - 1) >> shift;
    if (field > 65535) field = 65535;
    space = field << shift;
    c->rcv_wnd = c->last_adv_wnd = space;
    if (!syn && SEQ_DIFF(c->rcv_nxt + space, c->rcv_adv) > 0) c->rcv_adv = c->rcv_nxt + space;
    return (uint16_t)field;
}
#endif

static int tcp_xmit(tcp_conn_t* conn, uint32_t seq, uint8_t flags,
                    const void* data, uint16_t data_len) {
#ifdef NEXUS_NATIVE64
    uint8_t pkt[TCP_HEADER_LEN + 8 + TCP_MSS];
#else
    uint8_t pkt[TCP_HEADER_LEN + 4 + TCP_MSS];
#endif
    uint16_t hdr_len = TCP_HEADER_LEN;

    if (data_len > TCP_MSS) return -1;

    tcp_header_t* hdr = (tcp_header_t*)pkt;
    if (flags & TCP_FLAG_SYN) {
        /* MSS option: kind 2, len 4, 1460 (native: the route's MTU less the headers, 1440 over IPv6) */
#ifdef NEXUS_NATIVE64
        uint16_t mss = ip_route_mss(conn->local_ip, conn->remote_ip);
#else
        uint16_t mss = TCP_MSS;
#endif
        pkt[20] = 2; pkt[21] = 4; pkt[22] = (mss >> 8) & 0xFF; pkt[23] = mss & 0xFF;
        hdr_len += 4;
#ifdef NEXUS_NATIVE64
        /* RFC 7323: offer a window scale on our SYN; answer one on a SYN-ACK
         * only when the peer's SYN offered it. NOP + kind 3, len 3, shift. */
        if (!(flags & TCP_FLAG_ACK) || conn->wscale_ok) {
            pkt[24] = 1; pkt[25] = 3; pkt[26] = 3; pkt[27] = conn->rcv_wscale;
            hdr_len += 4;
        }
#endif
    }

#ifdef NEXUS_NATIVE64
    uint32_t old_adv = conn->rcv_adv, old_wnd = conn->rcv_wnd, old_adv_wnd = conn->last_adv_wnd;
    uint16_t wnd = tcp_window_field(conn, (flags & TCP_FLAG_SYN) != 0);
#else
    uint16_t wnd = (uint16_t)(TCP_RX_BUF_SIZE - conn->rx_len);
    conn->rcv_wnd = wnd;
    conn->last_adv_wnd = wnd;
#endif

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

#ifdef NEXUS_NATIVE64
    int result = ip_send_packet_from(conn->local_ip, conn->remote_ip, IP_PROTO_TCP, pkt, tcp_len);
    if (result == -11) {
        /* Transmit ring (loopback or NIC) full: nothing left, so nothing is counted or offered and the ACK state
         * is untouched; a refused plain ACK stays owed. tcp_resume sends again once the ring drains. */
        conn->rcv_adv = old_adv; conn->rcv_wnd = old_wnd; conn->last_adv_wnd = old_adv_wnd;
        conn->tx_blocked = tcp_tx_waiting = true;
        if (flags == TCP_FLAG_ACK && !data_len && seq == conn->snd_nxt) conn->ack_now = true;
        return result;
    }
    uint64_t now = native_clock_ns();
    conn->segs_out++;
    if (data_len) { conn->data_segs_out++; conn->bytes_sent += data_len; conn->last_data_tx_ns = now; }
    if (flags & TCP_FLAG_ACK) {
        /* Every ACK-bearing segment carries the cumulative ACK and window. */
        conn->ack_now = conn->ack_push = conn->ack_dup = false; conn->ack_bytes = 0; conn->delack_ns = 0;
        conn->last_ack_tx_ns = now;
    }
    conn->last_activity = system_ticks;
    tcp_stats.segs_sent++;
    tcp_stats.bytes_sent += data_len;
    return result;
#else
    conn->last_activity = system_ticks;
    tcp_stats.segs_sent++;
    tcp_stats.bytes_sent += data_len;
    return ip_send_packet_from(conn->local_ip, conn->remote_ip, IP_PROTO_TCP, pkt, tcp_len);
#endif
}

static inline void tcp_send_ack(tcp_conn_t* conn) {
    tcp_xmit(conn, conn->snd_nxt, TCP_FLAG_ACK, NULL, 0);
}

#ifdef NEXUS_NATIVE64
/* Send `length` queued bytes starting `offset` bytes past snd_una's data. */
static int tcp_xmit_queued(tcp_conn_t* c, uint32_t seq, uint8_t flags, uint32_t offset, uint16_t length) {
    uint8_t data[TCP_MSS];
    if (length > TCP_MSS) return -1;
    ring_read(c->tx_buf, c->tx_cap, ring_pos(c->tx_head, offset, c->tx_cap), data, length);
    return tcp_xmit(c, seq, flags, data, length);
}

/* ---- ACK policy ----
 * A received in-order segment is acknowledged at once when two full segments
 * are unacknowledged, the peer pushed, or something unusual happened (gap
 * filled, FIN); otherwise after TCP_DELACK_NS. Inside a receive batch those
 * ACKs collapse to one per connection. Duplicate ACKs for out-of-order or old
 * data are never coalesced: the sender counts them for fast retransmit. */
static bool tcp_batching, tcp_flush_needed;
static void tcp_ack_policy(tcp_conn_t* c) {
    if (!c->active || (!c->ack_now && !c->ack_bytes && !c->ack_push && !c->ack_dup)) return;
    if (c->ack_dup) { tcp_send_ack(c); return; }
    if (c->ack_now || c->ack_push || c->ack_bytes >= 2U * c->snd_mss) {
        if (tcp_batching) { c->ack_flush = true; tcp_flush_needed = true; }
        else tcp_send_ack(c);
    } else if (!c->delack_ns) c->delack_ns = native_clock_ns() + TCP_DELACK_NS;
}
static void tcp_resume(void);
void tcp_batch_begin(void) { tcp_batching = true; }
void tcp_batch_end(void) {
    tcp_batching = false;
    if (tcp_flush_needed) {
        tcp_flush_needed = false;
        for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
            if (!c->ack_flush) continue;
            c->ack_flush = false;
            if (c->active && (c->ack_now || c->ack_push || c->ack_bytes)) tcp_send_ack(c);
        }
    }
    tcp_resume();   /* the receive batch drained the loopback ring: blocked senders go on */
}

/* Linux-style receiver window update: once the window still open to the peer
 * has fallen to half (or less) of what we could now offer, and the increase
 * is worth a segment (RFC 1122 SWS avoidance), advertise it. */
static bool tcp_window_update_due(const tcp_conn_t* c) {
    uint32_t left = SEQ_DIFF(c->rcv_adv, c->rcv_nxt) > 0 ? c->rcv_adv - c->rcv_nxt : 0;
    uint32_t space = c->rx_cap - c->rx_len, limit = 65535U << (c->wscale_ok ? c->rcv_wscale : 0);
    uint32_t threshold = c->rx_cap / 2 < TCP_MSS ? c->rx_cap / 2 : TCP_MSS;
    if (space > limit) space = limit;
    return space >= left + threshold && space >= 2 * left;
}

/* RFC 6298 smoothed RTT / variance in microseconds. */
static void tcp_rtt_sample(tcp_conn_t* c, uint64_t elapsed_ns) {
    uint32_t us = elapsed_ns / 1000 > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)(elapsed_ns / 1000);
    if (!us) us = 1;
    if (!c->srtt_us) { c->srtt_us = us; c->rttvar_us = us / 2; }
    else {
        uint32_t delta = c->srtt_us > us ? c->srtt_us - us : us - c->srtt_us;
        c->rttvar_us = (3 * (uint64_t)c->rttvar_us + delta) / 4;
        c->srtt_us = (7 * (uint64_t)c->srtt_us + us) / 8;
    }
    if (!c->min_rtt_us || us < c->min_rtt_us) c->min_rtt_us = us;
}
#endif

/* Arm the retransmission timer if it is idle */
static inline void tcp_arm_rtx(tcp_conn_t* c) {
    if (c->rtx_timer == 0) { c->rtx_timer = NOW(); c->rtx_first = c->rtx_timer; }
}

/* --------------------------------------------------------------------------
 * tcp_send_rst: Send a RST to reject unwanted segments
 * -------------------------------------------------------------------------- */
static void tcp_send_rst(uint32_t local_ip, uint32_t remote_ip, uint16_t remote_port,
                         uint16_t local_port, uint32_t seq, uint32_t ack) {
    /* From the address the segment was sent to (or the TCB's), so the peer's TCB matches it. */
    if (!local_ip) local_ip = ip_source_address(remote_ip);
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

    hdr->checksum = tcp_checksum(local_ip, remote_ip, pkt, TCP_HEADER_LEN);
    tcp_stats.rst_sent++;
    ip_send_packet_from(local_ip, remote_ip, IP_PROTO_TCP, pkt, TCP_HEADER_LEN);
}

/* --------------------------------------------------------------------------
 * tcp_output: transmit queued data as far as the peer window allows, then
 * the FIN if the app has closed. Called with interrupts off or from within
 * net_poll (never re-entered). CLOSING is included: the peer's FIN can arrive
 * (FIN_WAIT_1 -> CLOSING) before our queued data and FIN have left.
 * -------------------------------------------------------------------------- */
#ifdef NEXUS_NATIVE64
/* Loopback ring slots TCP data never takes: ACKs, handshakes, FINs and datagrams always find room. */
#define TCP_LOOP_RESERVE 16
#endif
static void tcp_output(tcp_conn_t* c) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT && c->state != TCP_FIN_WAIT_1 &&
        c->state != TCP_CLOSING && c->state != TCP_LAST_ACK) return;
#ifdef NEXUS_NATIVE64
    if (c->tx_blocked) return;   /* waiting for its turn in tcp_resume: no overtaking the other blocked TCBs */
#endif

    for (;;) {
        tcp_size_t unsent = c->tx_len - c->tx_unacked;
        if (unsent == 0) break;
        uint32_t usable = (c->snd_wnd > c->tx_unacked) ? (uint32_t)(c->snd_wnd - c->tx_unacked) : 0;
        if (usable == 0) {
            /* Zero (or exhausted) window: arm the persist timer when nothing
             * is in flight to carry the window update back to us. */
            if (c->tx_unacked == 0 && c->probe_timer == 0) c->probe_timer = NOW();
            break;
        }
        tcp_size_t n = unsent;
        if (n > usable)    n = (tcp_size_t)usable;
        if (n > c->snd_mss) n = c->snd_mss;
        if (c->nagle && c->tx_unacked && n < c->snd_mss && !c->fin_queued) break;
        uint8_t flags = TCP_FLAG_ACK;
        if (n == unsent) flags |= TCP_FLAG_PSH;
#ifdef NEXUS_NATIVE64
        uint64_t sent_ns = native_clock_ns();
        /* A full transmit ring keeps the data unsent (tcp_resume goes on) instead of losing it to the RTO. */
        if (ip_is_local(c->remote_ip) && ip_loopback_room() <= TCP_LOOP_RESERVE) { c->tx_blocked = tcp_tx_waiting = true; break; }
        if (tcp_xmit_queued(c, c->snd_nxt, flags, c->tx_unacked, (uint16_t)n) == -11) break;
        if (!c->rtt_timing) { c->rtt_timing = true; c->rtt_seq = c->snd_nxt; c->rtt_start_ns = sent_ns; }
#else
        tcp_xmit(c, c->snd_nxt, flags, c->tx_buf + c->tx_unacked, n);
#endif
        c->snd_nxt    += n;
        c->tx_unacked += n;
        c->probe_timer = 0;
        tcp_arm_rtx(c);
    }

    if (c->tx_len == c->tx_unacked && c->fin_queued && !c->fin_sent) {
#ifdef NEXUS_NATIVE64
        if (tcp_xmit(c, c->snd_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0) == -11) return;
#else
        tcp_xmit(c, c->snd_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
#endif
        c->snd_nxt++;
        c->fin_sent = true;
        tcp_arm_rtx(c);
    }
}

#ifdef NEXUS_NATIVE64
/* Send what met a full transmit ring (tcp_xmit / tcp_output), from the network poll once the rings have drained:
 * the SYN or SYN-ACK of a handshake, queued data / FIN and an owed ACK. Every blocked TCB gets its turn, round
 * robin from the one after the TCB that filled a ring first last time, so a bulk sender cannot starve the rest. */
static uint32_t tcp_resume_after;   /* gen of that TCB (0: start at the head) */
static void tcp_resume(void) {
    if (!tcp_tx_waiting) return;
    tcp_tx_waiting = false;
    tcp_conn_t* first = next_connection(NULL);
    for (tcp_conn_t* c = first; c && tcp_resume_after; c = next_connection(c))
        if (c->gen == tcp_resume_after) { if (next_connection(c)) first = next_connection(c); break; }
    tcp_resume_after = 0;
    tcp_conn_t* c = first;
    if (c) do {
        if (c->tx_blocked) {
            c->tx_blocked = false;
            if (c->active) {
                if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED)
                    tcp_xmit(c, c->snd_una, c->state == TCP_SYN_SENT ? TCP_FLAG_SYN : TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                else {
                    tcp_output(c);
                    if (c->active && !c->tx_blocked && c->ack_now) tcp_send_ack(c);
                }
                if (c->tx_blocked && !tcp_resume_after) tcp_resume_after = c->gen;
            }
        }
        c = next_connection(c);
        if (!c) c = next_connection(NULL);
    } while (c != first);
}
#endif

/* --------------------------------------------------------------------------
 * tcp_retransmit: resend the oldest unacknowledged segment (SYN, data or FIN)
 * -------------------------------------------------------------------------- */
static void tcp_retransmit(tcp_conn_t* c, bool backoff) {
    int result;
    if (c->state == TCP_SYN_SENT) {
        result = tcp_xmit(c, c->snd_una, TCP_FLAG_SYN, NULL, 0);
    } else if (c->state == TCP_SYN_RECEIVED) {
        result = tcp_xmit(c, c->snd_una, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
    } else if (c->tx_unacked > 0) {
        tcp_size_t n = c->tx_unacked;
        if (n > c->snd_mss) n = c->snd_mss;
        uint8_t flags = TCP_FLAG_ACK | TCP_FLAG_PSH;
        if (c->fin_sent && n == c->tx_unacked) flags |= TCP_FLAG_FIN;
#ifdef NEXUS_NATIVE64
        result = tcp_xmit_queued(c, c->snd_una, flags, 0, (uint16_t)n);
        if (result != -11) c->bytes_retrans += n;
#else
        result = tcp_xmit(c, c->snd_una, flags, c->tx_buf, n);
#endif
    } else if (c->fin_sent) {
        result = tcp_xmit(c, c->snd_una, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    } else {
        c->rtx_timer = 0;               /* nothing outstanding after all */
        return;
    }
#ifdef NEXUS_NATIVE64
    /* The ring took nothing: no backoff and no count; the expired timer (or the next duplicate ACK) tries again. */
    if (result == -11) { if (!backoff && c->dupacks) c->dupacks--; return; }
    c->rtt_timing = false;              /* Karn: never time a retransmitted segment */
    c->total_retrans++;
#else
    (void)result;
#endif
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
        tcp_send_rst(c->local_ip, c->remote_ip, c->remote_port, c->local_port, c->snd_nxt, c->rcv_nxt);
    c->closed = true;
    c->error = 110;
    tcp_stats.giveups++;
    tcp_free_tcb(c, true);
}

/* --------------------------------------------------------------------------
 * tcp_reap: expire stale half-open / half-closed / orphan TCBs
 * -------------------------------------------------------------------------- */
static void tcp_reap(void) {
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
        if (!c->active) continue;
        if (c->state == TCP_SYN_RECEIVED &&
            system_ticks - c->last_activity > TCP_SYN_RCVD_TICKS) {
            if (tcp_request(c)) tcp_free_tcb(c, false);   /* the request is dropped (Linux) */
            else tcp_back_to_listen(c);
        }
        /* Phase 55: a peer that vanishes mid-close (FIN_WAIT_1/2, LAST_ACK,
         * TIME_WAIT) would pin one of the 8 TCBs forever; reap after ~30 s. */
        if (c->active && (c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2 ||
                          c->state == TCP_LAST_ACK || c->state == TCP_TIME_WAIT ||
                          c->state == TCP_CLOSING) &&
            system_ticks - c->last_activity > TCP_HALF_CLOSED_TICKS) {
            c->closed = true; c->error = 110;
            tcp_free_tcb(c, true);
        }
        /* Phase 55: an orphan nobody adopted (server stopped / busy) is reset
         * so the client does not hang on a half-served connection; one left in
         * LISTEN (its SYN could not be answered) is simply freed. */
        if (c->active && c->orphan && (c->state == TCP_LISTEN ||
            system_ticks - c->last_activity > TCP_ORPHAN_TICKS)) {
            if (c->state != TCP_LISTEN)
                tcp_send_rst(c->local_ip, c->remote_ip, c->remote_port, c->local_port, c->snd_nxt, c->rcv_nxt);
            tcp_free_tcb(c, false);
        }
    }
}

/* --------------------------------------------------------------------------
 * tcp_timer: retransmission / persist / window-update timers (every tick)
 * -------------------------------------------------------------------------- */
void tcp_timer(void) {
    uint32_t now = NOW();
#ifdef NEXUS_NATIVE64
    uint64_t now_ns = native_clock_ns();
    tcp_resume();   /* a NIC transmit ring drains without a receive batch */
#endif
    tcp_reap();
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
        if (!c->active || c->state == TCP_LISTEN) continue;

        /* Retransmission timeout */
        if (c->rtx_timer && (now - c->rtx_timer) >= c->rto) {
            bool give_up = (now - c->rtx_first) > TCP_GIVEUP_TICKS ||
                           (c->state == TCP_SYN_SENT && c->rtx_count >= TCP_SYN_RETRIES);
            if (give_up) { tcp_abort(c); continue; }
            /* tcp_reap drops a half-open request (or returns a kernel listener's TCB to LISTEN) */
            if (c->state == TCP_SYN_RECEIVED && c->rtx_count >= 3) continue;
            tcp_retransmit(c, true);
            continue;
        }

        /* Zero-window probe: one byte of new data after RTO of silence */
        if (c->probe_timer && c->tx_unacked == 0 && c->tx_len > 0 &&
            (now - c->probe_timer) >= c->rto &&
            (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT || c->state == TCP_FIN_WAIT_1 ||
             c->state == TCP_CLOSING || c->state == TCP_LAST_ACK)) {
#ifdef NEXUS_NATIVE64
            /* a full ring took nothing: the probe is due again on the next tick */
            if (tcp_xmit_queued(c, c->snd_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH, 0, 1) == -11) continue;
#else
            tcp_xmit(c, c->snd_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH, c->tx_buf, 1);
#endif
            c->snd_nxt++;
            c->tx_unacked = 1;
            c->probe_timer = 0;
            tcp_stats.zero_wnd_probes++;
            tcp_arm_rtx(c);
            continue;
        }

#ifdef NEXUS_NATIVE64
        /* Delayed ACK expiry, or a window update the reader made due. */
        if ((c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
             c->state == TCP_FIN_WAIT_2) &&
            ((c->delack_ns && now_ns >= c->delack_ns) || tcp_window_update_due(c))) {
            tcp_send_ack(c);
            continue;
        }
        /* Keepalive: after keep_idle_s of silence with nothing in flight,
         * probe with SEG.SEQ = SND.NXT - 1 every keep_intvl_s; give up
         * (ETIMEDOUT) after keep_cnt unanswered probes. */
        if (c->keepalive && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) &&
            c->snd_una == c->snd_nxt &&
            now_ns - c->last_rx_ns >= (uint64_t)c->keep_idle_s * 1000000000ULL &&
            now_ns >= c->keep_next_ns) {
            if (c->keep_probes >= c->keep_cnt) { tcp_abort(c); continue; }
            if (tcp_xmit(c, c->snd_nxt - 1, TCP_FLAG_ACK, NULL, 0) == -11) continue;   /* not sent: due again */
            c->keep_probes++;
            c->keep_next_ns = now_ns + (uint64_t)c->keep_intvl_s * 1000000000ULL;
        }
#else
        /* Window update: the app drained a buffer we had advertised as
         * (nearly) full — tell the peer it may send again. Covers vnc.c,
         * which empties rx_buf directly instead of through tcp_recv(). */
        if ((c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
             c->state == TCP_FIN_WAIT_2) &&
            c->last_adv_wnd < 2 * TCP_MSS &&
            (TCP_RX_BUF_SIZE - c->rx_len) >= TCP_RX_BUF_SIZE / 2) {
            tcp_send_ack(c);
        }
#endif
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

#ifdef NEXUS_NATIVE64
/* RFC 7323 window scale option (kind 3, len 3): the shift, or -1 if absent. */
static int tcp_parse_wscale(const uint8_t* opts, int len) {
    int i = 0;
    while (i < len) {
        uint8_t kind = opts[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t olen = opts[i + 1];
        if (olen < 2 || i + olen > len) break;
        if (kind == 3 && olen == 3) return opts[i + 2];
        i += olen;
    }
    return -1;
}
#endif

static void tcp_learn_mss(tcp_conn_t* c, const uint8_t* seg, uint8_t data_off) {
    uint16_t mss = tcp_parse_mss(seg + TCP_HEADER_LEN, data_off - TCP_HEADER_LEN);
#ifdef NEXUS_NATIVE64
    uint16_t limit = ip_route_mss(c->local_ip, c->remote_ip);   /* path MTU of the route (IPv6: 1440 on Ethernet) */
    if (limit > TCP_MSS) limit = TCP_MSS;
    if (mss >= 536 && mss < limit) c->snd_mss = mss;
    else c->snd_mss = limit;
#else
    if (mss >= 536 && mss < TCP_MSS) c->snd_mss = mss;
    else c->snd_mss = TCP_MSS;
#endif
#ifdef NEXUS_NATIVE64
    /* Scaling is in force only if both SYNs carried the option (we always
     * offer it); a shift above 14 is treated as 14. */
    int shift = tcp_parse_wscale(seg + TCP_HEADER_LEN, data_off - TCP_HEADER_LEN);
    c->wscale_ok = shift >= 0;
    c->snd_wscale = shift > 14 ? 14 : shift > 0 ? (uint8_t)shift : 0;
    if (!c->wscale_ok) c->rcv_wscale = 0;
#endif
}

/* Peer window from a non-SYN segment, in bytes. */
#ifdef NEXUS_NATIVE64
static inline uint32_t tcp_peer_window(const tcp_conn_t* c, uint32_t window) {
    return c->wscale_ok ? window << c->snd_wscale : window;
}
#else
#define tcp_peer_window(c, window) (window)
#endif

#ifdef NEXUS_NATIVE64
/* ---- Out-of-order reassembly ----
 * Segments beyond rcv_nxt are written straight to their final place in the
 * receive ring; ooo[] remembers which sequence ranges are present (sorted,
 * disjoint, non-adjacent). The receive ring always has room for them since we
 * never accept bytes past rx_cap - rx_len beyond rcv_nxt. */
static bool tcp_ooo_insert(tcp_conn_t* c, uint32_t start, uint32_t end) {
    unsigned i = 0, count = c->ooo_count;
    while (i < count && SEQ_DIFF(c->ooo[i].end, start) < 0) i++;
    if (i < count && SEQ_DIFF(c->ooo[i].start, end) <= 0) {
        if (SEQ_DIFF(start, c->ooo[i].start) < 0) c->ooo[i].start = start;
        if (SEQ_DIFF(end, c->ooo[i].end) > 0) c->ooo[i].end = end;
        unsigned j = i + 1;
        while (j < count && SEQ_DIFF(c->ooo[j].start, c->ooo[i].end) <= 0) {
            if (SEQ_DIFF(c->ooo[j].end, c->ooo[i].end) > 0) c->ooo[i].end = c->ooo[j].end;
            j++;
        }
        unsigned removed = j - i - 1;
        for (unsigned k = i + 1; k + removed < count; k++) c->ooo[k] = c->ooo[k + removed];
        c->ooo_count = (uint8_t)(count - removed);
        return true;
    }
    if (count == TCP_OOO_MAX) return false;
    for (unsigned k = count; k > i; k--) c->ooo[k] = c->ooo[k - 1];
    c->ooo[i].start = start; c->ooo[i].end = end;
    c->ooo_count = (uint8_t)(count + 1);
    return true;
}

/* rcv_nxt advanced: absorb every stored range it now reaches. */
static void tcp_ooo_advance(tcp_conn_t* c) {
    while (c->ooo_count && SEQ_DIFF(c->ooo[0].start, c->rcv_nxt) <= 0) {
        if (SEQ_DIFF(c->ooo[0].end, c->rcv_nxt) > 0) {
            uint32_t extra = c->ooo[0].end - c->rcv_nxt;
            c->rcv_nxt += extra; c->rx_len += extra;
            c->bytes_received += extra; tcp_stats.bytes_rcvd += extra;
        }
        for (unsigned k = 1; k < c->ooo_count; k++) c->ooo[k - 1] = c->ooo[k];
        c->ooo_count--;
    }
}

/* Accept a data segment (state ESTABLISHED / FIN_WAIT_x). Returns true when the
 * whole segment is now held (in order or queued), so a FIN on it can count. */
static bool tcp_receive_data(tcp_conn_t* c, uint32_t seq, const uint8_t* data, uint32_t length, bool push) {
    int32_t off = SEQ_DIFF(c->rcv_nxt, seq);
    if (off >= (int32_t)length) {                 /* entirely old: duplicate ACK */
        tcp_stats.dupacks_sent++;
        c->ack_dup = true;
        return true;
    }
    if (off > 0) { data += off; length -= (uint32_t)off; seq = c->rcv_nxt; }
    uint32_t gap = seq - c->rcv_nxt, space = c->rx_cap - c->rx_len;
    bool whole = true;
    if (gap >= space) {                            /* nothing fits in the ring */
        tcp_stats.rx_full_drops += length;
        c->ack_dup = true;
        return false;
    }
    if (length > space - gap) {
        tcp_stats.rx_full_drops += length - (space - gap);
        length = space - gap;
        whole = false;
    }
    if (c->discard_rx) {                           /* reader shut down: sink in-order bytes */
        if (gap) { c->ack_dup = true; return false; }
        c->rcv_nxt += length;
        c->bytes_received += length; tcp_stats.bytes_rcvd += length;
        c->ack_now = true;
        return whole;
    }
    ring_write(c->rx_buf, c->rx_cap, ring_pos(c->rx_head, c->rx_len + gap, c->rx_cap), data, length);
    if (gap == 0) {
        c->rcv_nxt += length; c->rx_len += length;
        c->bytes_received += length; tcp_stats.bytes_rcvd += length;
        c->ack_bytes += length;
        if (push) c->ack_push = true;
        if (c->ooo_count) { tcp_ooo_advance(c); c->ack_now = true; }   /* gap filled: ACK at once */
    } else {
        c->ooo_packets++;
        tcp_stats.dupacks_sent++;
        c->ack_dup = true;                         /* duplicate ACK triggers fast retransmit */
        if (!tcp_ooo_insert(c, seq, seq + length)) { tcp_stats.ooo_dropped++; whole = false; }
    }
    c->data_available = c->rx_len != 0;
    return whole;
}
#endif

/* --------------------------------------------------------------------------
 * tcp_handle_packet: Process incoming TCP segment
 * -------------------------------------------------------------------------- */
void tcp_handle_packet(uint32_t src_ip, const void* data, uint16_t len) { tcp_handle_packet_to(src_ip, ip_get_addr(), data, len); }
void tcp_handle_packet_to(uint32_t src_ip, uint32_t dst_ip, const void* data, uint16_t len) {
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
    if (tcp_checksum(src_ip, dst_ip, data, len) != 0) return;

    const uint8_t* payload = (const uint8_t*)data + data_off;
    uint16_t payload_len = len - data_off;
    tcp_stats.segs_rcvd++;

    tcp_reap();

    /* Find matching TCB */
    tcp_conn_t* conn = find_connection(src_ip, src_port, dst_ip, dst_port);

#ifdef NEXUS_NATIVE64
    if (conn && conn->listener) {
        /* A socket listener: a bare SYN gets a child TCB while the backlog has room; with the queue full the SYN is
         * dropped and the client retries (Linux). An ACK is answered with a RST, anything else dropped (RFC 9293). */
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK | TCP_FLAG_RST)) != TCP_FLAG_SYN) {
            if ((flags & (TCP_FLAG_ACK | TCP_FLAG_RST)) == TCP_FLAG_ACK)
                tcp_send_rst(dst_ip, src_ip, src_port, dst_port, ack, seq + payload_len + ((flags & TCP_FLAG_FIN) ? 1 : 0));
            return;
        }
        if (conn->children >= conn->backlog) return;
        tcp_conn_t* child = alloc_connection_buffers(TCP_BUF_MIN, TCP_BUF_MIN);   /* tcp_grow on completion */
        if (!child) return;
        child->rcv_wscale = tcp_scale_for(TCP_RX_BUF_SIZE);   /* the SYN-ACK offers the scale of the grown ring */
        child->local_port = dst_port;
        child->state      = TCP_LISTEN;   /* handled by the LISTEN case below */
        child->parent     = conn; conn->children++;
        conn = child;
    }
#endif
    if (!conn) {
        /* Phase 55: a bare SYN for a server port that is between listeners is
         * accepted into an orphan TCB (adopted by the next tcp_listen on that
         * port) instead of being reset — see tcp_port_served(). */
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK | TCP_FLAG_RST)) == TCP_FLAG_SYN &&
            tcp_port_served(dst_port)) {
            conn = alloc_connection();
            if (conn) {
                conn->local_ip   = dst_ip;
                conn->local_port = dst_port;
                conn->state      = TCP_LISTEN;      /* handled by the LISTEN case below */
                conn->passive    = true;
                conn->orphan     = true;
            }
        }
        if (!conn) {
            if (!(flags & TCP_FLAG_RST)) {
                tcp_send_rst(dst_ip, src_ip, src_port, dst_port,
                             (flags & TCP_FLAG_ACK) ? ack : 0, seq + payload_len +
                             ((flags & TCP_FLAG_SYN) ? 1 : 0) + ((flags & TCP_FLAG_FIN) ? 1 : 0));
            }
            return;
        }
    }

    /* A blind reset must not destroy a live stream (RFC 9293 / RFC 5961). */
#ifdef NEXUS_NATIVE64
    if ((flags & TCP_FLAG_RST) && conn->state != TCP_LISTEN && conn->state != TCP_SYN_SENT && seq != conn->rcv_nxt) {
        uint32_t window_left = SEQ_DIFF(conn->rcv_adv, conn->rcv_nxt) > 0 ? conn->rcv_adv - conn->rcv_nxt : 0;
        if (SEQ_DIFF(seq, conn->rcv_nxt) > 0 && (uint32_t)(seq - conn->rcv_nxt) < window_left) tcp_send_ack(conn);
        return;
    }
    conn->segs_in++;
    conn->last_rx_ns = native_clock_ns();
    if (payload_len) { conn->data_segs_in++; conn->last_data_rx_ns = conn->last_rx_ns; }
    conn->keep_probes = 0; conn->keep_next_ns = 0;
#else
    if ((flags & TCP_FLAG_RST) && conn->state != TCP_LISTEN && conn->state != TCP_SYN_SENT && seq != conn->rcv_nxt) {
        if (SEQ_DIFF(seq, conn->rcv_nxt) > 0 && (uint32_t)(seq - conn->rcv_nxt) < (uint32_t)(TCP_RX_BUF_SIZE - conn->rx_len)) tcp_send_ack(conn);
        return;
    }
#endif

    /* ---- Non-synchronized states ---- */
    switch (conn->state) {
        case TCP_LISTEN:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK | TCP_FLAG_RST)) == TCP_FLAG_SYN) {
                conn->remote_ip   = src_ip;
                conn->remote_port = src_port;
                conn->local_ip    = dst_ip;      /* the address the client connected to */
                conn->rcv_nxt     = seq + 1;
#ifdef NEXUS_NATIVE64
                conn->rcv_adv     = conn->rcv_nxt;
#endif
                if (!next_isn(&conn->snd_nxt)) {   /* no entropy: refuse; a request TCB must not stay a listener */
                    if (tcp_request(conn)) tcp_free_tcb(conn, false);
                    return;
                }
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
                    conn->error = 111;
                    tcp_stats.rst_rcvd++;
                    tcp_free_tcb(conn, false);
                }
                return;
            }
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                if (ack != conn->snd_nxt) {
                    tcp_send_rst(dst_ip, src_ip, src_port, dst_port, ack, 0);
                    return;
                }
                conn->snd_una   = ack;
                conn->rcv_nxt   = seq + 1;
                conn->snd_wnd   = window;
#ifdef NEXUS_NATIVE64
                conn->rcv_adv   = conn->rcv_nxt;
                if (conn->rtt_timing) { tcp_rtt_sample(conn, native_clock_ns() - conn->rtt_start_ns); conn->rtt_timing = false; }
#endif
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
            if (flags & TCP_FLAG_RST) {
                if (tcp_request(conn)) tcp_free_tcb(conn, false);
                else tcp_back_to_listen(conn);
                return;
            }
            /* Peer re-sent its SYN: our SYN-ACK was lost — resend it. */
            if ((flags & TCP_FLAG_SYN) && seq + 1 == conn->rcv_nxt) {
                tcp_xmit(conn, conn->snd_una, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                return;
            }
            /* Only the ACK of our SYN-ACK completes the handshake */
            if (!(flags & TCP_FLAG_ACK) || ack != conn->snd_nxt) return;
            conn->snd_una   = ack;
            conn->snd_wnd   = tcp_peer_window(conn, window);
            conn->rtx_timer = 0;
            conn->rtx_count = 0;
            conn->state     = TCP_ESTABLISHED;
            conn->connected = true;
#ifdef NEXUS_NATIVE64
            if (conn->parent) { conn = tcp_grow(conn); conn->queue_seq = ++conn->parent->queue_seq; }   /* acceptable now */
#endif
            break;      /* fall into synchronized processing (piggybacked data) */

        default:
            break;
    }

    /* ---- Synchronized states ---- */

    /* RST: abort */
    if (flags & TCP_FLAG_RST) {
        conn->closed = true;
        conn->error = 104;
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
        tcp_size_t data_acked = (n > conn->tx_unacked) ? conn->tx_unacked : (tcp_size_t)n;
        if (data_acked > 0) {
#ifdef NEXUS_NATIVE64
            conn->tx_head     = ring_pos(conn->tx_head, data_acked, conn->tx_cap);
            conn->tx_len     -= data_acked;
            conn->tx_unacked -= data_acked;
            conn->bytes_acked += data_acked;
#else
            uint16_t rest = conn->tx_len - data_acked;
            if (rest > 0) memcpy(conn->tx_buf, conn->tx_buf + data_acked, rest);
            conn->tx_len      = rest;
            conn->tx_unacked -= data_acked;
#endif
        }
#ifdef NEXUS_NATIVE64
        if (conn->rtt_timing && SEQ_DIFF(ack, conn->rtt_seq) > 0) {
            tcp_rtt_sample(conn, native_clock_ns() - conn->rtt_start_ns);
            conn->rtt_timing = false;
        }
#endif
        bool fin_acked = conn->fin_sent && ack == conn->snd_nxt;
        conn->snd_una  = ack;
        conn->snd_wnd  = tcp_peer_window(conn, window);
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
            tcp_peer_window(conn, window) == conn->snd_wnd) {
            if (++conn->dupacks == 3) tcp_retransmit(conn, false);
        }
        conn->snd_wnd = tcp_peer_window(conn, window);
    }
    /* acked < 0: old ACK, ignore the ACK part */

    /* Data processing */
    uint32_t fin_seq = seq + payload_len;           /* sequence number of a FIN */
#ifdef NEXUS_NATIVE64
    bool segment_held = true;                       /* every byte before the FIN is stored */
    if (payload_len > 0) {
        if (conn->state == TCP_ESTABLISHED || conn->state == TCP_FIN_WAIT_1 ||
            conn->state == TCP_FIN_WAIT_2) {
            segment_held = tcp_receive_data(conn, seq, payload, payload_len, (flags & TCP_FLAG_PSH) != 0);
        } else {
            need_ack = true;       /* data after the peer's FIN / in closing states */
        }
    }
#else
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
                    if (!conn->discard_rx) {
                        memcpy(conn->rx_buf + conn->rx_len, payload, copy);
                        conn->rx_len += copy;
                    }
                    conn->rcv_nxt += copy;
                    conn->data_available = conn->rx_len != 0;
                    tcp_stats.bytes_rcvd += copy;
                }
                if (copy < payload_len) tcp_stats.rx_full_drops += payload_len - copy;
                need_ack = true;
            }
        } else {
            need_ack = true;       /* data after the peer's FIN / in closing states */
        }
    }
#endif

    /* FIN processing (only when it is the next expected byte) */
#ifdef NEXUS_NATIVE64
    /* A FIN beyond a gap is remembered and takes effect once the gap fills. */
    bool fin = (flags & TCP_FLAG_FIN) != 0;
    bool synchronized_rx = conn->state == TCP_ESTABLISHED || conn->state == TCP_FIN_WAIT_1 ||
                           conn->state == TCP_FIN_WAIT_2;
    if (fin && synchronized_rx && segment_held && SEQ_DIFF(fin_seq, conn->rcv_nxt) > 0 &&
        fin_seq - conn->rcv_nxt <= conn->rx_cap - conn->rx_len) {
        conn->fin_pending = true; conn->rx_fin_seq = fin_seq;
        need_ack = true;                            /* duplicate ACK for the gap */
    }
    if (!fin && conn->fin_pending && conn->rcv_nxt == conn->rx_fin_seq) { fin = true; fin_seq = conn->rx_fin_seq; }
    if (fin) {
        int32_t d = SEQ_DIFF(fin_seq, conn->rcv_nxt);
        if (d == 0) conn->fin_pending = false;
#else
    if (flags & TCP_FLAG_FIN) {
        int32_t d = SEQ_DIFF(fin_seq, conn->rcv_nxt);
#endif
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

#ifdef NEXUS_NATIVE64
    /* Data segments sent now piggyback the ACK; otherwise the ACK policy
     * sends it at once, at the end of this receive batch, or delayed. */
    if (need_ack) conn->ack_now = true;
    tcp_output(conn);
    tcp_ack_policy(conn);
#else
    if (need_ack) tcp_send_ack(conn);

    /* The window may have opened / data may have been acked: push more */
    tcp_output(conn);
#endif
    conn->last_activity = system_ticks;
}

/* --------------------------------------------------------------------------
 * tcp_connect: Active open (initiate connection)
 * -------------------------------------------------------------------------- */
#ifdef NEXUS_NATIVE64
tcp_conn_t* tcp_connect(uint32_t remote_ip, uint16_t remote_port,
                        uint16_t local_port) {
    return tcp_connect_buffers(remote_ip, remote_port, local_port, 0, 0);
}
tcp_conn_t* tcp_connect_buffers(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                                uint32_t receive_bytes, uint32_t send_bytes) {
    return tcp_connect_from(0, remote_ip, remote_port, local_port, receive_bytes, send_bytes);
}
tcp_conn_t* tcp_connect_from(uint32_t local_ip, uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                             uint32_t receive_bytes, uint32_t send_bytes) {
    uintptr_t ef = irq_save();
    /* Rings are physically contiguous: under fragmentation fall back to smaller
     * ones (down to the legacy 16 KB / 8 KB) rather than failing the connect. */
    uint32_t rx = tcp_buffer_size(receive_bytes, TCP_RX_BUF_SIZE), tx = tcp_buffer_size(send_bytes, TCP_TX_BUF_SIZE);
    tcp_conn_t* conn = alloc_connection_buffers(rx, tx);
    while (!conn && (rx > 16384 || tx > 8192)) {
        if (rx > 16384) rx /= 2;
        if (tx > 8192) tx /= 2;
        conn = alloc_connection_buffers(rx, tx);
    }
#else
tcp_conn_t* tcp_connect(uint32_t remote_ip, uint16_t remote_port,
                        uint16_t local_port) {
    uintptr_t ef = irq_save();
    tcp_conn_t* conn = alloc_connection();
    uint32_t local_ip = 0;
#endif
    if (!conn) { irq_restore(ef); return NULL; }

    conn->local_ip    = local_ip ? local_ip : ip_source_address(remote_ip);
    conn->local_port  = local_port;
    conn->remote_ip   = remote_ip;
    conn->remote_port = remote_port;
    if (!next_isn(&conn->snd_una)) { tcp_free_tcb(conn, false); irq_restore(ef); return NULL; }
    conn->snd_nxt     = conn->snd_una + 1;   /* the SYN occupies one sequence number */
    conn->snd_wnd     = TCP_MSS;        /* until the SYN-ACK tells us */
    conn->state       = TCP_SYN_SENT;
    conn->rtx_timer   = NOW(); conn->rtx_first = conn->rtx_timer;
#ifdef NEXUS_NATIVE64
    conn->rtt_timing = true; conn->rtt_seq = conn->snd_una; conn->rtt_start_ns = native_clock_ns();
#endif
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
    uintptr_t ef = irq_save();
    /* Phase 55: adopt a client that connected while this port was between
     * listeners (oldest first) — the caller sees it as its new connection. */
    tcp_conn_t* best = NULL;
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
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

#ifdef NEXUS_NATIVE64
/* Socket listener (listen(2)): it never leaves LISTEN and owns no rings; SYNs create its children (see
 * tcp_handle_packet_to) and the Phase 55 orphan machinery never applies to it. */
tcp_conn_t* tcp_listen_socket(uint32_t ip, uint16_t port, unsigned backlog) {
    uintptr_t ef = irq_save();
    tcp_conn_t* conn = alloc_connection_buffers(0, 0);
    if (conn) {
        conn->local_ip = conn->listen_ip = ip;
        conn->local_port = port;
        conn->state = TCP_LISTEN;
        conn->listener = true;
        conn->backlog = backlog > 0xFFFF ? 0xFFFF : (uint16_t)backlog;
    }
    irq_restore(ef);
    return conn;
}
bool tcp_accept_ready(const tcp_conn_t* listener) {
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c))
        if (c->active && c->parent == listener && c->connected) return true;
    return false;
}
tcp_conn_t* tcp_accept(tcp_conn_t* listener) {
    uintptr_t ef = irq_save();
    tcp_conn_t* best = NULL;
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c))
        if (c->active && c->parent == listener && c->connected &&
            (!best || SEQ_DIFF(c->queue_seq, best->queue_seq) < 0)) best = c;
    if (best) { best->parent = NULL; listener->children--; }
    irq_restore(ef);
    return best;
}
#endif

/* --------------------------------------------------------------------------
 * tcp_ephemeral_port: next free local port in 49152..65535
 * -------------------------------------------------------------------------- */
uint16_t tcp_ephemeral_port(void) {
    static uint16_t next = 49152;
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next++;
        if (next < 49152) next = 49152;
        bool used = false;
        for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c))
            if (c->active && c->local_port == p) { used = true; break; }
        if (!used) return p;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * tcp_send / tcp_send_nb: queue data for transmission
 * -------------------------------------------------------------------------- */
static bool tcp_can_send(const tcp_conn_t* c) {
    return c && c->active && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)
           && !c->fin_queued;
}

#ifdef NEXUS_NATIVE64
int tcp_send_space(const tcp_conn_t* conn) {
    if (!tcp_can_send(conn)) return 0;
    return (int)(conn->tx_cap - conn->tx_len);
}

/* Queue what fits (interrupts off) and kick the output engine. */
static uint16_t tcp_enqueue(tcp_conn_t* c, const uint8_t* data, uint16_t len) {
    uintptr_t ef = irq_save();
    uint16_t n = 0;
    if (tcp_can_send(c)) {
        uint32_t space = c->tx_cap - c->tx_len;
        n = len < space ? len : (uint16_t)space;
        if (n > 0) {
            ring_write(c->tx_buf, c->tx_cap, ring_pos(c->tx_head, c->tx_len, c->tx_cap), data, n);
            c->tx_len += n;
            tcp_output(c);
        }
    }
    irq_restore(ef);
    return n;
}

uint32_t tcp_tx_span(const tcp_conn_t* c, uint32_t offset, uint8_t** data) {
    uint32_t used = c->tx_len + offset;
    if (used >= c->tx_cap) return 0;
    uint32_t position = ring_pos(c->tx_head, used, c->tx_cap);
    uint32_t free = c->tx_cap - used, contiguous = c->tx_cap - position;
    *data = c->tx_buf + position;
    return free < contiguous ? free : contiguous;
}
void tcp_tx_commit(tcp_conn_t* c, uint32_t bytes) {
    uintptr_t ef = irq_save();
    if (bytes && tcp_can_send(c) && bytes <= c->tx_cap - c->tx_len) {
        c->tx_len += bytes;
        tcp_output(c);
    }
    irq_restore(ef);
}
#else
int tcp_send_space(const tcp_conn_t* conn) {
    if (!tcp_can_send(conn)) return 0;
    return TCP_TX_BUF_SIZE - conn->tx_len;
}

/* Queue what fits (interrupts off) and kick the output engine. */
static uint16_t tcp_enqueue(tcp_conn_t* c, const uint8_t* data, uint16_t len) {
    uintptr_t ef = irq_save();
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
#endif

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
int tcp_recv(tcp_conn_t* conn, void* buf, uint32_t max_len) {
    if (!conn) return -1;

    /* The timer-IRQ net_poll appends to rx_buf/rx_len: do the copy-out and
     * shift with interrupts off so an arriving segment can't be lost or torn. */
    uintptr_t eflags = irq_save();
    int ret;
    if (conn->rx_len == 0) {
        ret = conn->closed ? -1 : 0;    /* closed : no data yet */
    } else {
#ifdef NEXUS_NATIVE64
        uint32_t copy_len = conn->rx_len < max_len ? conn->rx_len : max_len;
        ring_read(conn->rx_buf, conn->rx_cap, conn->rx_head, buf, copy_len);
        tcp_rx_consume(conn, copy_len);
        ret = (int)copy_len;
#else
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
#endif
    }
    irq_restore(eflags);
    return ret;
}

#ifdef NEXUS_NATIVE64
uint32_t tcp_rx_span(const tcp_conn_t* c, uint32_t offset, const uint8_t** data) {
    if (offset >= c->rx_len) return 0;
    uint32_t position = ring_pos(c->rx_head, offset, c->rx_cap);
    uint32_t available = c->rx_len - offset, contiguous = c->rx_cap - position;
    *data = c->rx_buf + position;
    return available < contiguous ? available : contiguous;
}
/* The reader took `bytes`: free them and tell the peer promptly if the window
 * it sees has become small compared with what we can now offer. */
void tcp_rx_consume(tcp_conn_t* c, uint32_t bytes) {
    uintptr_t ef = irq_save();
    if (bytes > c->rx_len) bytes = c->rx_len;
    c->rx_head = ring_pos(c->rx_head, bytes, c->rx_cap);
    c->rx_len -= bytes;
    c->data_available = c->rx_len > 0;
    if (bytes && c->active && (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
                               c->state == TCP_FIN_WAIT_2) && tcp_window_update_due(c))
        tcp_send_ack(c);
    irq_restore(ef);
}
#endif

/* --------------------------------------------------------------------------
 * tcp_close: Initiate connection close (FIN follows any queued data)
 * -------------------------------------------------------------------------- */
void tcp_close(tcp_conn_t* conn) {
    if (!conn || !conn->active) return;
    uintptr_t ef = irq_save();

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
            tcp_send_rst(conn->local_ip, conn->remote_ip, conn->remote_port, conn->local_port,
                         conn->snd_nxt, conn->rcv_nxt);
            tcp_free_tcb(conn, false);
            break;

        default:
            break;
    }
    irq_restore(ef);
}

#ifdef NEXUS_NATIVE64
void tcp_reset(tcp_conn_t* conn) {
    if (!conn || !conn->active) return;
    uintptr_t ef = irq_save();
    if (conn->state != TCP_SYN_SENT && conn->state != TCP_LISTEN)
        tcp_send_rst(conn->local_ip, conn->remote_ip, conn->remote_port, conn->local_port, conn->snd_nxt, conn->rcv_nxt);
    conn->closed = true; conn->error = 104; tcp_free_tcb(conn, false);
    irq_restore(ef);
}
void tcp_shutdown_read(tcp_conn_t* conn) {
    uintptr_t ef = irq_save();
    conn->discard_rx = true;
    if (conn->active && conn->connected) tcp_send_ack(conn);
    irq_restore(ef);
}
void tcp_set_nodelay(tcp_conn_t* conn, bool enabled) {
    uintptr_t ef = irq_save(); conn->nagle = !enabled;
    if (enabled) tcp_output(conn);
    irq_restore(ef);
}
void tcp_set_keepalive(tcp_conn_t* conn, bool enabled, uint32_t idle, uint32_t interval, uint32_t count) {
    uintptr_t ef = irq_save();
    conn->keepalive = enabled;
    conn->keep_idle_s = idle ? idle : 1;
    conn->keep_intvl_s = interval ? interval : 1;
    conn->keep_cnt = count ? count : 1;
    conn->keep_probes = 0; conn->keep_next_ns = 0;
    irq_restore(ef);
}
void tcp_set_listen_family(tcp_conn_t* listener, uint8_t family) { listener->listen_family = family; }
void tcp_foreach(void (*visit)(const tcp_conn_t* connection, void* context), void* context) {
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) visit(c, context);
}
static tcp_conn_t* tcp_exact(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport) {
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c))
        if (c->active && c->state != TCP_LISTEN && c->local_port == lport && c->remote_port == rport && c->local_ip == local && c->remote_ip == remote) return c;
    return NULL;
}
/* RFC 1191 / 8201: a smaller path MTU shrinks the segment size and resends what is in flight at once. */
void tcp_pmtu_update(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport, unsigned mtu) {
    tcp_conn_t* c = tcp_exact(local, lport, remote, rport); if (!c) return;
    unsigned headers = tcp_handle6(remote) ? 60 : 40, floor = tcp_handle6(remote) ? 1220 : 536;
    unsigned mss = mtu > headers ? mtu - headers : floor;
    if (mss < floor) mss = floor;
    if (mss >= c->snd_mss) return;
    c->snd_mss = (uint16_t)mss;
    if (c->tx_unacked) tcp_retransmit(c, false);
}
/* RFC 1122 4.2.3.9 / Linux tcp_v4_err: only a connection still in SYN_SENT is aborted by a hard ICMP error. */
void tcp_icmp_error(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport, uint32_t seq, int error) {
    tcp_conn_t* c = tcp_exact(local, lport, remote, rport); if (!c) return;
    if (c->state != TCP_SYN_SENT || seq != c->snd_una) return;
    c->closed = true; c->error = error; tcp_free_tcb(c, false);
}
/* Tickless idle (arch/x86_64/tick.c via network.c): no connection needs the 1 ms timer now (no retransmission,
 * zero-window probe, delayed ACK, owed window update or ACK, nor a segment waiting for ring space). Keepalives and
 * the half-closed / orphan reaping work in seconds and run with the slow network timers. */
bool tcp_timers_idle(void) {
    if (tcp_tx_waiting) return false;
    for (tcp_conn_t* c = next_connection(NULL); c; c = next_connection(c)) {
        if (!c->active || c->state == TCP_LISTEN) continue;
        if (c->rtx_timer || c->probe_timer || c->delack_ns || c->tx_blocked || c->ack_now || c->ack_flush) return false;
        if ((c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2) && tcp_window_update_due(c)) return false;
    }
    return true;
}
#endif

/* --------------------------------------------------------------------------
 * tcp_get_connections: Return connection table for display
 * -------------------------------------------------------------------------- */
#ifndef NEXUS_NATIVE64
const tcp_conn_t* tcp_get_connections(int* count) {
    if (count) *count = TCP_MAX_CONNECTIONS;
    return connections;
}
#endif
