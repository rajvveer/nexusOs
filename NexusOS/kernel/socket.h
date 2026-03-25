/* ============================================================================
 * NexusOS — Socket API Header — Phase 27
 * ============================================================================
 * BSD-style socket interface over UDP and TCP.
 * ============================================================================ */

#ifndef SOCKET_H
#define SOCKET_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define SOCK_STREAM     1       /* TCP */
#define SOCK_DGRAM      2       /* UDP */
#define AF_INET         2       /* IPv4 */

#define MAX_SOCKETS     8

/* --------------------------------------------------------------------------
 * Socket Address
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t sin_family;    /* Address family (AF_INET) */
    uint16_t sin_port;      /* Port (host byte order) */
    uint32_t sin_addr;      /* IP address (host byte order) */
} sockaddr_in_t;

/* --------------------------------------------------------------------------
 * Socket Descriptor
 * -------------------------------------------------------------------------- */
typedef struct {
    bool         active;
    int          type;          /* SOCK_STREAM or SOCK_DGRAM */
    uint16_t     local_port;
    uint32_t     remote_ip;
    uint16_t     remote_port;
    bool         bound;
    bool         listening;
    bool         connected;
    void*        proto_data;    /* tcp_conn_t* or NULL */
} socket_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize socket subsystem */
void socket_init(void);

/* Create a socket (returns socket fd, or -1 on error) */
int socket_create(int domain, int type, int protocol);

/* Bind socket to local address */
int socket_bind(int fd, const sockaddr_in_t* addr);

/* Listen for incoming connections (TCP only) */
int socket_listen(int fd, int backlog);

/* Accept a connection (TCP only, returns new fd) */
int socket_accept(int fd, sockaddr_in_t* addr);

/* Connect to remote address */
int socket_connect(int fd, const sockaddr_in_t* addr);

/* Send data */
int socket_send(int fd, const void* data, uint16_t len);

/* Send datagram to specific address (UDP) */
int socket_sendto(int fd, const void* data, uint16_t len,
                  const sockaddr_in_t* addr);

/* Receive data */
int socket_recv(int fd, void* buf, uint16_t max_len);

/* Receive datagram with sender info (UDP) */
int socket_recvfrom(int fd, void* buf, uint16_t max_len,
                    sockaddr_in_t* from);

/* Close socket */
void socket_close(int fd);

#endif /* SOCKET_H */
