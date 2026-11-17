/* ============================================================================
 * NexusOS — Socket API — Phase 27
 * ============================================================================
 * BSD-style socket layer translating to UDP/TCP calls.
 * ============================================================================ */

#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "ip.h"
#include "vga.h"
#include "string.h"

/* Socket table */
static socket_t sockets[MAX_SOCKETS];

/* Ephemeral port counter */
static uint16_t next_ephemeral = 49152;

/* --------------------------------------------------------------------------
 * socket_init: Initialize socket subsystem
 * -------------------------------------------------------------------------- */
void socket_init(void) {
    memset(sockets, 0, sizeof(sockets));
    next_ephemeral = 49152;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Socket API initialized\n");
}

/* --------------------------------------------------------------------------
 * alloc_ephemeral: Get next ephemeral port
 * -------------------------------------------------------------------------- */
static uint16_t alloc_ephemeral(void) {
    uint16_t port = next_ephemeral++;
    if (next_ephemeral > 65530) next_ephemeral = 49152;
    return port;
}

/* --------------------------------------------------------------------------
 * get_socket: Validate and return socket by fd
 * -------------------------------------------------------------------------- */
static socket_t* get_socket(int fd) {
    if (fd < 0 || fd >= MAX_SOCKETS) return NULL;
    if (!sockets[fd].active) return NULL;
    return &sockets[fd];
}

/* --------------------------------------------------------------------------
 * socket_create: Create a new socket
 * -------------------------------------------------------------------------- */
int socket_create(int domain, int type, int protocol) {
    (void)domain;
    (void)protocol;

    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -1;

    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            memset(&sockets[i], 0, sizeof(socket_t));
            sockets[i].active = true;
            sockets[i].type   = type;
            return i;
        }
    }
    return -1; /* No free sockets */
}

/* --------------------------------------------------------------------------
 * socket_bind: Bind to local port
 * -------------------------------------------------------------------------- */
int socket_bind(int fd, const sockaddr_in_t* addr) {
    socket_t* s = get_socket(fd);
    if (!s || !addr) return -1;
    if (s->bound) return -1;

    s->local_port = addr->sin_port;
    s->bound = true;

    if (s->type == SOCK_DGRAM) {
        return udp_bind(addr->sin_port);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * socket_listen: Put TCP socket in LISTEN state
 * -------------------------------------------------------------------------- */
int socket_listen(int fd, int backlog) {
    (void)backlog;
    socket_t* s = get_socket(fd);
    if (!s || s->type != SOCK_STREAM) return -1;
    if (!s->bound) return -1;

    tcp_conn_t* conn = tcp_listen(s->local_port);
    if (!conn) return -1;

    s->proto_data = conn;
    s->listening  = true;
    return 0;
}

/* --------------------------------------------------------------------------
 * socket_accept: Accept incoming TCP connection
 * -------------------------------------------------------------------------- */
int socket_accept(int fd, sockaddr_in_t* addr) {
    socket_t* s = get_socket(fd);
    if (!s || !s->listening) return -1;

    tcp_conn_t* listen_conn = (tcp_conn_t*)s->proto_data;
    if (!listen_conn || !listen_conn->connected) return -1; /* No pending */

    /* Create new socket for the accepted connection */
    int new_fd = socket_create(AF_INET, SOCK_STREAM, 0);
    if (new_fd < 0) return -1;

    socket_t* ns = &sockets[new_fd];
    ns->local_port  = s->local_port;
    ns->remote_ip   = listen_conn->remote_ip;
    ns->remote_port = listen_conn->remote_port;
    ns->connected   = true;
    ns->proto_data  = listen_conn;

    if (addr) {
        addr->sin_family = AF_INET;
        addr->sin_port   = listen_conn->remote_port;
        addr->sin_addr   = listen_conn->remote_ip;
    }

    /* Re-create listen socket for next connection */
    s->proto_data = tcp_listen(s->local_port);

    return new_fd;
}

/* --------------------------------------------------------------------------
 * socket_connect: Connect to remote (TCP or set UDP peer)
 * -------------------------------------------------------------------------- */
int socket_connect(int fd, const sockaddr_in_t* addr) {
    socket_t* s = get_socket(fd);
    if (!s || !addr) return -1;

    s->remote_ip   = addr->sin_addr;
    s->remote_port = addr->sin_port;

    if (!s->bound) {
        s->local_port = alloc_ephemeral();
        s->bound = true;
    }

    if (s->type == SOCK_STREAM) {
        tcp_conn_t* conn = tcp_connect(addr->sin_addr, addr->sin_port,
                                       s->local_port);
        if (!conn) return -1;
        s->proto_data = conn;
        s->connected = true;
    } else {
        /* UDP "connect" just sets default peer */
        udp_bind(s->local_port);
        s->connected = true;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * socket_send: Send data on connected socket
 * -------------------------------------------------------------------------- */
int socket_send(int fd, const void* data, uint16_t len) {
    socket_t* s = get_socket(fd);
    if (!s || !s->connected) return -1;

    if (s->type == SOCK_STREAM) {
        return tcp_send((tcp_conn_t*)s->proto_data, data, len);
    } else {
        return udp_send(s->remote_ip, s->local_port, s->remote_port,
                        data, len);
    }
}

/* --------------------------------------------------------------------------
 * socket_sendto: Send datagram to address (UDP)
 * -------------------------------------------------------------------------- */
int socket_sendto(int fd, const void* data, uint16_t len,
                  const sockaddr_in_t* addr) {
    socket_t* s = get_socket(fd);
    if (!s || s->type != SOCK_DGRAM) return -1;
    if (!addr) return -1;

    if (!s->bound) {
        s->local_port = alloc_ephemeral();
        s->bound = true;
        udp_bind(s->local_port);
    }

    return udp_send(addr->sin_addr, s->local_port, addr->sin_port,
                    data, len);
}

/* --------------------------------------------------------------------------
 * socket_recv: Receive data from connected socket
 * -------------------------------------------------------------------------- */
int socket_recv(int fd, void* buf, uint16_t max_len) {
    socket_t* s = get_socket(fd);
    if (!s) return -1;

    if (s->type == SOCK_STREAM) {
        return tcp_recv((tcp_conn_t*)s->proto_data, buf, max_len);
    } else {
        udp_datagram_t* dgram = udp_recv(s->local_port);
        if (!dgram) return 0;
        uint16_t copy = dgram->length < max_len ? dgram->length : max_len;
        memcpy(buf, dgram->data, copy);
        return (int)copy;
    }
}

/* --------------------------------------------------------------------------
 * socket_recvfrom: Receive datagram with sender info (UDP)
 * -------------------------------------------------------------------------- */
int socket_recvfrom(int fd, void* buf, uint16_t max_len,
                    sockaddr_in_t* from) {
    socket_t* s = get_socket(fd);
    if (!s || s->type != SOCK_DGRAM) return -1;

    udp_datagram_t* dgram = udp_recv(s->local_port);
    if (!dgram) return 0;

    uint16_t copy = dgram->length < max_len ? dgram->length : max_len;
    memcpy(buf, dgram->data, copy);

    if (from) {
        from->sin_family = AF_INET;
        from->sin_port   = dgram->src_port;
        from->sin_addr   = dgram->src_ip;
    }
    return (int)copy;
}

/* --------------------------------------------------------------------------
 * socket_close: Close and free a socket
 * -------------------------------------------------------------------------- */
void socket_close(int fd) {
    socket_t* s = get_socket(fd);
    if (!s) return;

    if (s->type == SOCK_STREAM && s->proto_data) {
        tcp_close((tcp_conn_t*)s->proto_data);
    } else if (s->type == SOCK_DGRAM && s->bound) {
        udp_unbind(s->local_port);
    }

    s->active = false;
}
