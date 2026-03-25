/* ============================================================================
 * NexusOS — TCP (Transmission Control Protocol) Header — Phase 27
 * ============================================================================
 * Connection-oriented, reliable byte stream protocol.
 * ============================================================================ */

#ifndef TCP_H
#define TCP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define TCP_HEADER_LEN      20      /* Minimum TCP header (no options) */
#define TCP_MAX_CONNECTIONS  8       /* Max simultaneous TCBs */
#define TCP_WINDOW_SIZE     1024    /* Default receive window */
#define TCP_RX_BUF_SIZE     1024    /* Per-connection RX buffer */
#define TCP_TX_BUF_SIZE     1024    /* Per-connection TX buffer */
#define TCP_MSS             1460    /* Max segment size (MTU - IP - TCP) */
#define TCP_RETRANSMIT_TICKS 36     /* ~2 seconds at 18.2 Hz */

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
    uint16_t    snd_wnd;        /* Send window */
    uint16_t    rcv_wnd;        /* Receive window */

    /* Receive buffer */
    uint8_t     rx_buf[TCP_RX_BUF_SIZE];
    uint16_t    rx_len;         /* Bytes available to read */

    /* Transmit buffer */
    uint8_t     tx_buf[TCP_TX_BUF_SIZE];
    uint16_t    tx_len;         /* Bytes pending to send */

    /* Timing */
    uint32_t    last_activity;  /* Last packet sent/received (ticks) */

    /* Flags for applications */
    bool        data_available; /* New data in RX buffer */
    bool        connected;      /* Connection fully established */
    bool        closed;         /* Connection closed by remote */
} tcp_conn_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize TCP subsystem */
void tcp_init(void);

/* Handle incoming TCP segment (called from IP layer) */
void tcp_handle_packet(uint32_t src_ip, const void* data, uint16_t len);

/* Active open: connect to remote host */
tcp_conn_t* tcp_connect(uint32_t remote_ip, uint16_t remote_port,
                        uint16_t local_port);

/* Passive open: listen on a port */
tcp_conn_t* tcp_listen(uint16_t port);

/* Send data on a connection */
int tcp_send(tcp_conn_t* conn, const void* data, uint16_t len);

/* Receive data from a connection */
int tcp_recv(tcp_conn_t* conn, void* buf, uint16_t max_len);

/* Close a connection */
void tcp_close(tcp_conn_t* conn);

/* Get connection table for netstat display */
const tcp_conn_t* tcp_get_connections(int* count);

/* Get state name string */
const char* tcp_state_name(tcp_state_t state);

#endif /* TCP_H */
