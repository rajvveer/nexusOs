/* ============================================================================
 * NexusOS — TCP (Transmission Control Protocol) Header — Phase 27 / 57
 * ============================================================================
 * Connection-oriented, reliable byte stream protocol.
 *
 * Phase 57 added reliability: per-TCB retransmission queue (RTO with
 * exponential backoff, fast retransmit on 3 dup ACKs, 30 s give-up -> RST),
 * send window / MSS honoured, 16 KB receive window that shrinks as rx_buf
 * fills and is re-advertised when the app reads, in-order-only receive
 * (out-of-order segments are dropped + dup-ACKed), SYN/FIN retransmission,
 * zero-window probes. tcp_send() blocks (bounded, net_poll+hlt) while the
 * send buffer is full; tcp_send_nb() never blocks.
 * ============================================================================ */

#ifndef TCP_H
#define TCP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define TCP_HEADER_LEN      20      /* Minimum TCP header (no options) */
#define TCP_MAX_CONNECTIONS  8       /* Legacy table; native TCBs are dynamically allocated. */
#ifdef NEXUS_NATIVE64
/* Native x86-64: per-connection ring buffers are allocated with the TCB, so
 * these are defaults (SO_RCVBUF/SO_SNDBUF before connect pick other sizes). */
#define TCP_RX_BUF_SIZE     (256U * 1024U)  /* default receive ring (window up to this) */
#define TCP_TX_BUF_SIZE     (256U * 1024U)  /* default send / retransmission ring */
#define TCP_BUF_MIN         4096U
#define TCP_BUF_MAX         (4U * 1024U * 1024U)
#define TCP_OOO_MAX         8               /* out-of-order ranges remembered per TCB */
#define TCP_DELACK_NS       2000000ULL      /* delayed ACK for a lone sub-2-MSS segment */
typedef uint32_t tcp_size_t;
#else
#define TCP_RX_BUF_SIZE     16384   /* Per-connection RX buffer = receive window */
#define TCP_TX_BUF_SIZE     8192    /* Per-connection TX / retransmission queue */
typedef uint16_t tcp_size_t;
#endif
#define TCP_WINDOW_SIZE     TCP_RX_BUF_SIZE
#define TCP_MSS             1460    /* Max segment size (MTU - IP - TCP) */
#define TCP_RTO_INIT        18      /* initial RTO ~1 s at 18.2 Hz */
#define TCP_RTO_MAX         (16 * 18)
#define TCP_GIVEUP_TICKS    (30 * 18)   /* abort after ~30 s without progress */
#define TCP_RETRANSMIT_TICKS TCP_RTO_INIT   /* legacy name */

/* --------------------------------------------------------------------------
 * TCP Flags
 * -------------------------------------------------------------------------- */
#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20

/* --------------------------------------------------------------------------
 * TCP Header (20 bytes minimum)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t src_port;      /* Source port */
    uint16_t dst_port;      /* Destination port */
    uint32_t seq_num;       /* Sequence number */
    uint32_t ack_num;       /* Acknowledgment number */
    uint8_t  data_off;      /* Data offset (upper 4 bits) + reserved */
    uint8_t  flags;         /* TCP flags */
    uint16_t window;        /* Receive window size */
    uint16_t checksum;      /* Checksum */
    uint16_t urgent_ptr;    /* Urgent pointer */
} __attribute__((packed)) tcp_header_t;

/* --------------------------------------------------------------------------
 * TCP Pseudo-Header (for checksum calculation)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_length;
} __attribute__((packed)) tcp_pseudo_header_t;

/* --------------------------------------------------------------------------
 * TCP States
 * -------------------------------------------------------------------------- */
typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

/* --------------------------------------------------------------------------
 * TCP Control Block (TCB)
 * -------------------------------------------------------------------------- */
typedef struct tcp_conn {
    bool        active;
    tcp_state_t state;

    /* Connection identity */
    uint32_t    local_ip;
    uint16_t    local_port;
    uint32_t    remote_ip;
    uint16_t    remote_port;

    /* Sequence tracking */
    uint32_t    snd_una;        /* Send unacknowledged */
    uint32_t    snd_nxt;        /* Send next */
    uint32_t    rcv_nxt;        /* Receive next expected */
#ifdef NEXUS_NATIVE64
    uint32_t    snd_wnd;        /* Peer's advertised window in bytes (already scaled) */
    uint32_t    rcv_wnd;        /* Window we last advertised, in bytes */

    /* Receive ring: rx_len in-order bytes start at rx_buf[rx_head]; bytes
     * beyond them (up to rx_cap) may hold out-of-order data (see ooo[]). */
    uint8_t*    rx_buf;
    uint32_t    rx_len, rx_head, rx_cap;

    /* Send ring starting at tx_buf[tx_head]: the first tx_unacked bytes are in
     * flight (snd_una .. snd_nxt), the rest of tx_len is queued unsent. */
    uint8_t*    tx_buf;
    uint32_t    tx_len, tx_unacked, tx_head, tx_cap;
#else
    uint16_t    snd_wnd;        /* Peer's advertised window */
    uint16_t    rcv_wnd;        /* Window we last advertised */

    /* Receive buffer: in-order data, always starting at rx_buf[0]
     * (tcp_recv shifts; vnc.c reads it directly and sets rx_len = 0). */
    uint8_t     rx_buf[TCP_RX_BUF_SIZE];
    uint16_t    rx_len;         /* Bytes available to read */

    /* Transmit / retransmission queue: tx_buf[0 .. tx_unacked) is in flight
     * (snd_una .. snd_nxt), tx_buf[tx_unacked .. tx_len) is queued unsent. */
    uint8_t     tx_buf[TCP_TX_BUF_SIZE];
    uint16_t    tx_len;         /* Bytes in the queue (unacked + unsent) */
    uint16_t    tx_unacked;     /* Leading bytes already transmitted */
#endif

    /* Timing */
    uint32_t    last_activity;  /* Last packet sent/received (ticks) */

    /* Flags for applications */
    bool        data_available; /* New data in RX buffer */
    bool        connected;      /* Connection fully established */
    bool        closed;         /* Connection closed by remote (FIN/RST/abort) */
    int         error;          /* Terminal transport errno, separate from an orderly FIN. */
    bool        discard_rx, nagle;

    /* Phase 55: passive = created by tcp_listen (a server-side TCB);
     * orphan = accepted by the TCP layer while the server port had no
     * LISTEN TCB (the app was between clients) — handed to the next
     * tcp_listen() on that port instead of RST-ing the client. */
    bool        passive;
    bool        orphan;
    uint32_t    gen;            /* allocation generation: apps that keep a
                                   TCB index compare it to detect slot reuse */

    /* Phase 57: reliability */
    uint16_t    snd_mss;        /* min(TCP_MSS, peer MSS option) */
    bool        fin_queued;     /* app closed: FIN follows the queued data */
    bool        fin_sent;       /* FIN transmitted (snd_nxt counts it) */
    uint32_t    rto;            /* current retransmission timeout (ticks) */
    uint32_t    rtx_timer;      /* tick of the transmission being timed (0 = none) */
    uint32_t    rtx_first;      /* tick the oldest unacked byte was first sent */
    uint32_t    probe_timer;    /* zero-window persist timer start (0 = none) */
    uint8_t     rtx_count;      /* consecutive retransmissions */
    uint8_t     dupacks;        /* duplicate ACKs seen for snd_una */
#ifdef NEXUS_NATIVE64
    uint32_t    last_adv_wnd;   /* window (bytes) in the last segment we sent */

    /* RFC 7323 window scaling: rcv_wscale is the shift we offer/apply to our
     * window, snd_wscale the peer's; both apply only once wscale_ok. */
    bool        wscale_ok;
    uint8_t     rcv_wscale, snd_wscale;
    uint32_t    rcv_adv;        /* right edge (rcv_nxt + window) last advertised */

    /* Out-of-order reassembly: ranges [start, end) already stored in the ring. */
    struct { uint32_t start, end; } ooo[TCP_OOO_MAX];
    uint8_t     ooo_count;
    bool        fin_pending;    /* FIN seen beyond a gap at rx_fin_seq */
    uint32_t    rx_fin_seq;

    /* ACK policy: coalesce per receive batch, delay a lone small segment;
     * duplicate ACKs (out-of-order / old data) always go out at once. */
    bool        ack_now, ack_push, ack_flush, ack_dup;
    uint32_t    ack_bytes;      /* in-order bytes received since our last ACK */
    uint64_t    delack_ns;      /* delayed-ACK deadline (0 = none) */

    /* RTT estimation (RFC 6298, Karn): one timed segment at a time. */
    bool        rtt_timing;
    uint32_t    rtt_seq;
    uint64_t    rtt_start_ns;
    uint32_t    srtt_us, rttvar_us, min_rtt_us;

    /* Keepalive (SO_KEEPALIVE / TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT). */
    bool        keepalive;
    uint8_t     keep_probes;
    uint32_t    keep_idle_s, keep_intvl_s, keep_cnt;
    uint64_t    last_rx_ns, last_data_rx_ns, last_data_tx_ns, last_ack_tx_ns, keep_next_ns;

    /* TCP_INFO counters */
    uint32_t    segs_in, segs_out, data_segs_in, data_segs_out, total_retrans, ooo_packets;
    uint64_t    bytes_acked, bytes_received, bytes_sent, bytes_retrans;

    /* Socket listeners (tcp_listen_socket): the listener TCB stays in LISTEN; each SYN within its backlog gets a
     * child TCB (small rings until the handshake completes) that waits on it until tcp_accept() takes it. */
    bool        listener;
    bool        tx_blocked;     /* a segment met a full transmit ring: resent once it drains */
    uint16_t    backlog, children;  /* listener: queue limit, half-open + unaccepted children */
    uint32_t    listen_ip;      /* listener: bound address (0 = any) */
    uint8_t     listen_family;  /* listener: 0 both, 4 IPv4 only, 6 IPv6 only (IPv6 endpoints are address handles) */
    uint32_t    queue_seq;      /* listener: handshakes completed; child: its place in the queue */
    struct tcp_conn* parent;    /* child: its listener until accepted */
#else
    uint16_t    last_adv_wnd;   /* window value in the last segment we sent */
#endif
} tcp_conn_t;

/* Phase 57: global counters for netstat */
typedef struct {
    uint32_t retransmits;       /* RTO-driven retransmissions */
    uint32_t fast_retransmits;  /* 3-dup-ACK retransmissions */
    uint32_t giveups;           /* connections aborted after ~30 s */
    uint32_t ooo_dropped;       /* out-of-order segments dropped */
    uint32_t rx_full_drops;     /* in-window bytes dropped (rx buffer full) */
    uint32_t dupacks_sent;      /* duplicate ACKs we sent */
    uint32_t zero_wnd_probes;   /* persist probes sent */
    uint32_t rst_sent;
    uint32_t rst_rcvd;
    uint32_t segs_sent;
    uint32_t segs_rcvd;
    uint32_t bytes_sent;
    uint32_t bytes_rcvd;
} tcp_stats_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize TCP subsystem */
void tcp_init(void);

/* Handle incoming TCP segment (called from IP layer) */
void tcp_handle_packet(uint32_t src_ip, const void* data, uint16_t len);
/* Same, for a segment sent to dst_ip (one of our addresses: the TCB local address and checksum use it) */
void tcp_handle_packet_to(uint32_t src_ip, uint32_t dst_ip, const void* data, uint16_t len);

/* Phase 57: timers (retransmission, persist, reapers). Called from net_poll
 * on every timer tick and from the idle pumps; cheap when nothing is due. */
void tcp_timer(void);

/* Active open: connect to remote host */
tcp_conn_t* tcp_connect(uint32_t remote_ip, uint16_t remote_port,
                        uint16_t local_port);

/* Passive open: listen on a port */
tcp_conn_t* tcp_listen(uint16_t port);

/* Send data on a connection. Queues the bytes for (re)transmission; when the
 * send queue is full it pumps net_poll()+hlt (bounded) until space frees up.
 * Returns the number of bytes queued (may be < len on timeout), -1 if the
 * connection cannot send. Never call from IRQ context. */
int tcp_send(tcp_conn_t* conn, const void* data, uint16_t len);

/* Phase 57: non-blocking variant — queues what fits, returns that count
 * (0 = queue full, try again after net_poll()), -1 on error. */
int tcp_send_nb(tcp_conn_t* conn, const void* data, uint16_t len);

/* Phase 57: free bytes in the send queue (0 if the connection cannot send). */
int tcp_send_space(const tcp_conn_t* conn);

/* Receive data from a connection. >0 bytes, 0 none yet, -1 closed & drained */
int tcp_recv(tcp_conn_t* conn, void* buf, uint32_t max_len);

/* Close a connection (FIN after the queued data) */
void tcp_close(tcp_conn_t* conn);

/* Phase 57: pick a free ephemeral local port (49152..65535) */
uint16_t tcp_ephemeral_port(void);

/* Get connection table for netstat display */
#ifdef NEXUS_NATIVE64
/* Retain before yielding/polling; closed native TCBs live until their last owner releases them. */
void tcp_retain(tcp_conn_t* connection);
void tcp_release(tcp_conn_t* connection);
void tcp_collect(void);
uint64_t tcp_background_pages(void);
/* A connection (not a listener) on local `port`, at address `ip` (0 = any address). */
bool tcp_port_in_use(uint16_t port, uint32_t ip);
/* A live connection already uses this local/remote address pair (connect() fails with EADDRNOTAVAIL). */
bool tcp_tuple_in_use(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port);
/* Socket listener on ip:port (ip 0 = any address) holding up to `backlog` half-open + unaccepted connections. */
tcp_conn_t* tcp_listen_socket(uint32_t ip, uint16_t port, unsigned backlog);
/* The oldest connected child of a socket listener, detached from it (the caller retains it), or NULL. */
tcp_conn_t* tcp_accept(tcp_conn_t* listener);
bool tcp_accept_ready(const tcp_conn_t* listener);
void tcp_reset(tcp_conn_t* connection);
void tcp_shutdown_read(tcp_conn_t* connection);
void tcp_set_nodelay(tcp_conn_t* connection, bool enabled);
/* Active open with explicit receive/send ring sizes (clamped to TCP_BUF_MIN..TCP_BUF_MAX). */
tcp_conn_t* tcp_connect_buffers(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                                uint32_t receive_bytes, uint32_t send_bytes);
/* Same from source address local_ip (a bound socket; 0 = ip_source_address(remote_ip)). */
tcp_conn_t* tcp_connect_from(uint32_t local_ip, uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                             uint32_t receive_bytes, uint32_t send_bytes);
uint32_t tcp_buffer_size(uint64_t requested, uint32_t fallback);
/* Zero-copy access to the rings: contiguous readable bytes at `offset` into the
 * received data / writable bytes at `offset` past the queued data. */
uint32_t tcp_rx_span(const tcp_conn_t* connection, uint32_t offset, const uint8_t** data);
void tcp_rx_consume(tcp_conn_t* connection, uint32_t bytes);
uint32_t tcp_tx_span(const tcp_conn_t* connection, uint32_t offset, uint8_t** data);
void tcp_tx_commit(tcp_conn_t* connection, uint32_t bytes);
/* Receive batches: ACKs owed by segments handled between begin and end are
 * coalesced into one ACK per connection at tcp_batch_end(). */
void tcp_batch_begin(void);
void tcp_batch_end(void);
/* Keepalive parameters (seconds / probe count); takes effect immediately. */
void tcp_set_keepalive(tcp_conn_t* connection, bool enabled, uint32_t idle, uint32_t interval, uint32_t count);
/* IPv6 (arch/x86_64): endpoints are 32-bit address handles from 240.0.0.0/4 (ipv6.c); the IPv6 pseudo-header
 * checksum and the route MSS come from there and route.c. */
static inline bool tcp_handle6(uint32_t value) { return value >= 0xF0000000U && value <= 0xFEFFFFFFU; }
uint16_t tcp_checksum6(uint32_t src, uint32_t dst, const void* data, uint16_t length);
uint16_t ip_route_mss(uint32_t local, uint32_t remote);
void tcp_set_listen_family(tcp_conn_t* listener, uint8_t family);
/* Every TCB, including closed ones still referenced (handle collection, /proc/net/tcp). */
void tcp_foreach(void (*visit)(const tcp_conn_t* connection, void* context), void* context);
/* ICMP feedback for the connection local:lport -> remote:rport: a smaller path MTU, or a hard error (errno) that
 * aborts a connection still in SYN_SENT whose initial sequence number `seq` the error quotes. */
void tcp_pmtu_update(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport, unsigned mtu);
void tcp_icmp_error(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport, uint32_t seq, int error);
#else
const tcp_conn_t* tcp_get_connections(int* count);
#endif

/* Phase 57: global counters */
const tcp_stats_t* tcp_get_stats(void);

/* Get state name string */
const char* tcp_state_name(tcp_state_t state);

#endif /* TCP_H */
