/* ============================================================================
 * NexusOS — TCP (Transmission Control Protocol) Header — Phase 27 / 57
 * ============================================================================
 * Connection-oriented, reliable byte stream protocol.
 *
 * Phase 57 made it internet-grade: per-TCB retransmission queue (RTO with
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
#define TCP_MAX_CONNECTIONS  8       /* Max simultaneous TCBs */
#define TCP_RX_BUF_SIZE     16384   /* Per-connection RX buffer = receive window */
#define TCP_TX_BUF_SIZE     8192    /* Per-connection TX / retransmission queue */
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
typedef struct {
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

    /* Timing */
    uint32_t    last_activity;  /* Last packet sent/received (ticks) */

    /* Flags for applications */
    bool        data_available; /* New data in RX buffer */
    bool        connected;      /* Connection fully established */
    bool        closed;         /* Connection closed by remote (FIN/RST/abort) */

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
    uint16_t    last_adv_wnd;   /* window value in the last segment we sent */
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
int tcp_recv(tcp_conn_t* conn, void* buf, uint16_t max_len);

/* Close a connection (FIN after the queued data) */
void tcp_close(tcp_conn_t* conn);

/* Phase 57: pick a free ephemeral local port (49152..65535) */
uint16_t tcp_ephemeral_port(void);

/* Get connection table for netstat display */
const tcp_conn_t* tcp_get_connections(int* count);

/* Phase 57: global counters */
const tcp_stats_t* tcp_get_stats(void);

/* Get state name string */
const char* tcp_state_name(tcp_state_t state);

#endif /* TCP_H */
