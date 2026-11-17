/* Internet sockets of the native kernel: AF_INET and AF_INET6 stream (TCP, kernel/tcp.c), datagram (UDP), ICMP
 * echo datagram sockets ("ping" sockets: net.ipv4.ping_group_range) and raw sockets (CAP_NET_RAW).
 *
 * Addresses are kept IPv4-mapped for IPv4 (netdev.h), so an AF_INET6 socket can carry IPv4 traffic (dual stack,
 * unless IPV6_V6ONLY) and every comparison is one type. TCP names IPv6 endpoints with IPv6 address handles
 * (ipv6.c); a socket keeps the real address and scope. Datagrams wait in per-socket transmit/receive/error
 * queues charged against SO_SNDBUF/SO_RCVBUF; a transmit queue entry survives an unresolved next hop (ARP) or a
 * full ring for up to 5 s. Sends fragment to the path MTU unless IP_MTU_DISCOVER=IP_PMTUDISC_DO /
 * IPV6_DONTFRAG (EMSGSIZE). recvmsg() returns IP_PKTINFO/IP_TTL/IP_TOS/IP_ORIGDSTADDR, IPV6_PKTINFO/
 * IPV6_HOPLIMIT/IPV6_TCLASS/IPV6_ORIGDSTADDR and SO_TIMESTAMP(NS) when asked; sendmsg() takes IP_PKTINFO,
 * IP_TTL, IP_TOS, IPV6_PKTINFO, IPV6_HOPLIMIT, IPV6_TCLASS and IPV6_DONTFRAG. ICMP errors reach connected
 * sockets as SO_ERROR and, with IP_RECVERR/IPV6_RECVERR, the error queue (MSG_ERRQUEUE). Multicast (mcast.c holds
 * the rules): IP_ADD/DROP_MEMBERSHIP, IP_ADD/DROP_SOURCE_MEMBERSHIP, IP_BLOCK/UNBLOCK_SOURCE, IP_MSFILTER,
 * IPV6_JOIN/LEAVE_GROUP, and MCAST_JOIN/LEAVE_(SOURCE_)GROUP, MCAST_BLOCK/UNBLOCK_SOURCE, MCAST_MSFILTER at both levels;
 * received multicast passes each socket's filter (IP_MULTICAST_ALL / IPV6_MULTICAST_ALL without a membership). */
#include "netdev.h"
#include "../../kernel/tcp.h"
#include "../../kernel/udp.h"
#include "../../kernel/ip.h"
#include "../../kernel/string.h"

#define DONTWAIT 0x40U
#define NOSIGNAL 0x4000U
#define PEEK 2U
#define WAITALL 0x100U
#define TRUNCATE 0x20U
#define ERROR_QUEUE 0x2000U
#define CTRUNC 8U
#define CONFIRM 0x800U
#define MORE 0x8000U
enum { KIND_STREAM = 1, KIND_DGRAM = 2, KIND_PING = 3, KIND_RAW = 4 };
struct inet_address { uint16_t family, port; uint32_t ip; uint8_t zero[8]; };
struct inet6_address { uint16_t family, port; uint32_t flowinfo; uint8_t address[16]; uint32_t scope; };
struct extended_error { uint32_t number; uint8_t origin, type, code, pad; uint32_t info, data; };
struct datagram {
    struct datagram* next;
    uint64_t deadline, stamp_ns;
    struct ip6a peer, local;              /* receive: source, destination; transmit: destination, source (IPV6_PKTINFO) */
    uint32_t charge, length, peer_scope;
    uint16_t port;
    unsigned ifindex;                     /* receive: arrival interface; transmit: outgoing one (pktinfo), 0 none */
    uint8_t hop_limit, tclass;
    int16_t send_hops, send_tclass;       /* transmit: per-packet hop limit / traffic class (cmsg), -1 none */
    bool send_dontfrag;
    struct ip6a offender;
    struct extended_error error;
    uint8_t data[];
};
struct inet_socket {
    struct inet_socket* next;
    tcp_conn_t* connection;
    uint64_t receive_timeout, send_timeout;
    struct ip6a local, remote, bound;      /* bound: bind() address; local: the source in use */
    uint32_t local_scope, remote_scope, bound_scope;
    uint32_t observed_ready, observed_sequence;
    unsigned observed_space;
    struct datagram *received, *received_tail, *transmit, *transmit_tail, *errors, *errors_tail;
    uint64_t transmit_retry;
    unsigned receive_limit, send_limit, receive_used, send_used, datagram_sequence;
    int error;
    uint16_t local_port, remote_port;
    uint8_t family, kind, protocol;
    bool datagram;                         /* kind != KIND_STREAM */
    bool read_closed, write_closed, nodelay, reset_close, connecting, orphan, broadcast, bound_set, receive_errors;
    uint32_t stream_receive, stream_send, keep_idle, keep_interval, keep_count;
    bool keepalive, listening, reuse_address, reuse_port;
    bool isolated;                         /* created in an isolated network namespace (CLONE_NEWNET): loopback only */
    tcp_conn_t* listener;
    uint32_t owner;
    /* options */
    bool v6only, freebind, transparent, mc_loop4, mc_loop6, mc_all, mc_all6, hdrincl, pktinfo4, recv_ttl, recv_tos, origdst4,
         pktinfo6, recv_hoplimit, recv_tclass, origdst6, recv_pathmtu, dontfrag6, timestamp, timestamp_ns, bind_no_port, autoflowlabel;
    int16_t ttl, unicast_hops, mcast_hops;
    uint8_t mcast_ttl, tos, tclass;
    int8_t pmtudisc4, pmtudisc6;
    unsigned mcast_if4, mcast_if6, bound_dev;
    uint32_t mcast_addr4, mark, priority, addr_prefs, flowinfo;
    int checksum_offset;
    uint32_t icmp_filter, icmp6_filter[8];
    struct mc_membership* memberships;
    unsigned membership_count;
};
#define SOMAXCONN 128   /* backlog cap: each queued connection holds its rings (Linux's default before 5.4) */
_Static_assert(sizeof(struct inet_socket) <= 4096 && sizeof(struct inet_address) == 16 && sizeof(struct inet6_address) == 28 &&
               sizeof(struct message64) == 56, "INET ABI/allocations");
static struct inet_socket* sockets;
static bool is4(const struct ip6a* a) { return ip6a_is4(a); }
static uint32_t v4(const struct ip6a* a) { return ip6a_v4(a); }
static bool any4(const struct ip6a* a) { return is4(a) && !v4(a); }
/* An unspecified local address of either kind ("bound to nothing") */
static bool wildcard(const struct ip6a* a) { return ip6a_any(a) || any4(a); }
/* CLONE_NEWNET: the caller is in an isolated network namespace, or the socket was created in one (namespaces.c). */
static bool net_isolated(const struct inet_socket* s) { return s->isolated || task_net_namespace(); }

uint32_t inet_ready(struct inet_socket* s) {
    if (s->listening) return tcp_accept_ready(s->listener) ? 1 : 0;   /* readable while a connection waits for accept */
    if (s->datagram) return (s->received || s->read_closed ? 1 : 0) | (s->read_closed ? 0x2000 : 0) |
        ((s->send_used <= s->send_limit && s->send_limit - s->send_used >= 4096) || s->write_closed ? 4 : 0) | (s->error || s->errors ? 8 : 0) |
        (s->read_closed && s->write_closed ? 16 : 0);
    tcp_conn_t* c = s->connection;
    if (!c) return 16 | 4;
    uint32_t result = c->error ? 8 : 0;
    if (s->read_closed || c->closed) result |= 1 | 0x2000;
    if (c->rx_len) result |= 1;
    if (!c->active || (s->write_closed && (s->read_closed || c->closed))) result |= 16;
    if (s->write_closed || !c->active || tcp_send_space(c)) result |= 4;
    return result;
}
static void changed(struct inet_socket* s) {
    uint32_t ready = inet_ready(s), sequence = s->datagram ? s->datagram_sequence : s->listener ? s->listener->queue_seq :
                                                s->connection ? s->connection->rcv_nxt : 0;
    unsigned space = s->datagram ? (s->send_used < s->send_limit ? s->send_limit - s->send_used : 0) : s->connection ? tcp_send_space(s->connection) : 0;
    uint32_t events = (ready & ~s->observed_ready) | (sequence != s->observed_sequence ? ready & 1 : 0) |
                      (space > s->observed_space ? ready & 4 : 0);
    if (ready != s->observed_ready || sequence != s->observed_sequence || space != s->observed_space) {
        task_wake_channel(s); event_source_changed(s, events);
    }
    s->observed_ready = ready; s->observed_sequence = sequence; s->observed_space = space;
}

/* ---- TCP endpoint values: IPv4 addresses or IPv6 handles ---- */
static uint32_t tcp_value(const struct ip6a* a, uint32_t scope, bool create) {
    if (is4(a)) return v4(a);
    if (ip6a_any(a)) return 0;
    uint32_t effective = ip6a_linklocal(a) ? scope : 0;
    return create ? ip6h_get(a, effective) : ip6h_find(a, effective);
}
static bool from_tcp_value(uint32_t value, struct ip6a* a, uint32_t* scope) {
    *scope = 0;
    if (!ip6h_is(value)) { *a = ip6a_from4(value); return true; }
    return ip6h_address(value, a, scope);
}

/* ---- datagram queues ---- */
static unsigned datagram_charge(unsigned length) { return (sizeof(struct datagram) + length + 4095) & ~4095U; }
static void datagram_error(struct inet_socket* s, const struct ip6a* destination, uint16_t port, const struct ip6a* offender,
                           const void* data, unsigned size, struct extended_error error);
static int checksum_place(struct inet_socket* s, uint8_t* data, unsigned length, const struct ip6a* src, const struct ip6a* dst);
static int transmit_one(struct inet_socket* s, struct datagram* packet) {
    bool v6 = !is4(&packet->peer);
    struct ip6_send_options o; ip6_options_default(&o);
    o.oif = packet->ifindex ? packet->ifindex : s->bound_dev;
    o.mark = s->mark; o.socket = s;
    bool multicast = v6 ? ip6a_multicast(&packet->peer) : (v4(&packet->peer) >> 28) == 14;
    if (v6) {
        o.hop_limit = packet->send_hops >= 0 ? packet->send_hops : multicast ? s->mcast_hops : s->unicast_hops;
        o.tclass = packet->send_tclass >= 0 ? packet->send_tclass : s->tclass;
        o.dontfrag = packet->send_dontfrag || s->dontfrag6 || s->pmtudisc6 == 2 || s->pmtudisc6 == 3;
        o.mc_loop = s->mc_loop6;
        if (multicast && !o.oif) o.oif = s->mcast_if6;
        if (!o.oif && (ip6a_linklocal(&packet->peer) || (multicast && ip6a_mc_scope(&packet->peer) <= 2))) o.oif = packet->peer_scope;
        struct ip6a source = !ip6a_any(&packet->local) ? packet->local : s->bound;
        if (!ip6a_any(&source) && !is4(&source)) o.source = &source;
        o.flowlabel = s->flowinfo & 0xFFFFF;
    } else {
        o.hop_limit = packet->send_hops >= 0 ? packet->send_hops : multicast ? s->mcast_ttl : s->ttl;
        o.tclass = packet->send_tclass >= 0 ? packet->send_tclass : s->tos;
        o.dontfrag = s->pmtudisc4 == 2 || s->pmtudisc4 == 3;   /* DO / PROBE: EMSGSIZE beyond the path MTU */
        o.df_when_fits = s->pmtudisc4 == 1;                     /* WANT (the default): DF when it fits, else fragment */
        o.mc_loop = s->mc_loop4;
        if (multicast && !o.oif) o.oif = s->mcast_if4;
    }
    unsigned header = s->kind == KIND_DGRAM ? 8 : 0, total = header + packet->length;
    uint8_t small[1600]; uint8_t* buffer = total <= sizeof(small) ? small : (void*)arena_alloc(total); if (!buffer) return -11;
    if (header) { put16(buffer, s->local_port); put16(buffer + 2, packet->port); put16(buffer + 4, (uint16_t)total); buffer[6] = buffer[7] = 0; }
    if (packet->length) memcpy(buffer + header, packet->data, packet->length);
    int result;
    if (v6) {
        struct ip6a src;
        if (o.source) src = *o.source;
        else {
            struct route_result r;
            if (ip6a_loopback(&packet->peer)) src = ip6a_loopback_address;
            else if (addrconf_local(&packet->peer, 0, false)) src = packet->peer;
            else if (ip6a_multicast(&packet->peer) && o.oif) { struct netdev* d = netdev_by_index(o.oif); if (!d || !source_select6(&packet->peer, d, s->addr_prefs, &src)) { if (buffer != small) arena_free((uint64_t)buffer, total); return -99; } }
            else if (route_lookup(AF_INET6, &packet->peer, NULL, o.oif, o.mark, &r)) { if (buffer != small) arena_free((uint64_t)buffer, total); return -101; }
            else if (!source_select6(&packet->peer, r.dev, s->addr_prefs, &src)) src = r.source;
            o.source = &src;
        }
        if (s->kind == KIND_DGRAM) { uint16_t sum = ip6_checksum(&src, &packet->peer, IPPROTO_UDP, buffer, total); put16(buffer + 6, sum ? sum : 0xFFFF); }
        else if (s->kind == KIND_PING) { buffer[2] = buffer[3] = 0; put16(buffer + 2, ip6_checksum(&src, &packet->peer, IPPROTO_ICMPV6, buffer, total)); }
        else if (s->kind == KIND_RAW && !s->hdrincl) { int e = checksum_place(s, buffer, total, &src, &packet->peer); if (e) { if (buffer != small) arena_free((uint64_t)buffer, total); return e; } }
        o.raw_header = s->kind == KIND_RAW && s->hdrincl;
        uint8_t next = s->kind == KIND_DGRAM ? IPPROTO_UDP : s->kind == KIND_PING ? IPPROTO_ICMPV6 : s->protocol;
        result = ip6_send(&packet->peer, next, buffer, total, &o);
    } else {
        uint32_t dst = v4(&packet->peer), src = is4(&packet->local) && v4(&packet->local) ? v4(&packet->local) : v4(&s->bound);
        if (!src && dst != 0xFFFFFFFFU) src = ip_source_address(dst);
        if (s->kind == KIND_DGRAM) {
            uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + IPPROTO_UDP + total;
            uint16_t c = net_fold(net_sum(sum, buffer, total)); put16(buffer + 6, c ? c : 0xFFFF);
        } else if (s->kind == KIND_PING) { buffer[2] = buffer[3] = 0; uint16_t c = ip_checksum(buffer, (uint16_t)total); memcpy(buffer + 2, &c, 2); }
        o.raw_header = s->kind == KIND_RAW && (s->hdrincl || s->protocol == IPPROTO_RAW);
        uint8_t protocol = s->kind == KIND_DGRAM ? IPPROTO_UDP : s->kind == KIND_PING ? IPPROTO_ICMP : s->protocol;
        result = ip4_output(src, dst, protocol, buffer, total, &o);
    }
    if (buffer != small) arena_free((uint64_t)buffer, total);
    return result;
}
static void flush_datagrams(struct inet_socket* s) {
    uint64_t now = native_clock_ns();
    if (now < s->transmit_retry) return;
    unsigned budget = 16;
    while (s->transmit && budget--) {
        struct datagram* packet = s->transmit;
        int error = now >= packet->deadline ? -113 : transmit_one(s, packet);
        if (error == -2 || error == -11) {   /* ARP pending / full ring: kept; a full loopback ring drains by the next poll */
            if (is4(&packet->peer) ? !ip_is_local(v4(&packet->peer)) : !addrconf_local(&packet->peer, 0, true)) s->transmit_retry = now + 50000000;
            break;
        }
        if (error < 0 && !s->orphan) datagram_error(s, &packet->peer, packet->port, NULL, packet->data, packet->length,
            (struct extended_error){.number = error == -1 ? 1 : -error, .origin = 1});
        s->transmit = packet->next; s->send_used -= packet->charge;
        if (!s->transmit) s->transmit_tail = NULL;
        arena_free((uint64_t)packet, packet->charge);
    }
}
static void drop_memberships(struct inet_socket* s);
uint64_t inet_background_pages(void) {
    uint64_t pages = 0; struct inet_socket** link = &sockets;
    while (*link) {
        struct inet_socket* s = *link;
        if (s->orphan && !s->transmit) { *link = s->next; page_free((uint64_t)s); continue; }
        if (s->orphan) pages += 1 + s->send_used / 4096;
        link = &s->next;
    }
    return pages;
}
void inet_poll(void) {
    for (struct inet_socket* s = sockets; s; s = s->next) {
        if (s->datagram) flush_datagrams(s);
        if (!s->orphan) changed(s);
    }
    inet_background_pages();
}
static int take_error(struct inet_socket* s) {
    int error = s->datagram ? s->error : s->connection ? s->connection->error : 0;
    s->error = 0;
    if (s->connection) s->connection->error = 0;
    changed(s); return error;
}
/* close() or shutdown() of a listener: the connections not yet accepted are reset (tcp.c), the port stays bound. */
static void stop_listening(struct inet_socket* s) {
    tcp_reset(s->listener); tcp_release(s->listener); tcp_collect();
    s->listener = NULL; s->listening = false;
}
void inet_close(struct inet_socket* s) {
    while (s->received) { struct datagram* packet = s->received; s->received = packet->next; arena_free((uint64_t)packet, packet->charge); }
    while (s->errors) { struct datagram* packet = s->errors; s->errors = packet->next; arena_free((uint64_t)packet, packet->charge); }
    s->errors_tail = NULL;
    s->receive_used = 0; s->received_tail = NULL;
    drop_memberships(s);
    if (s->datagram && s->transmit) { s->orphan = true; return; }
    if (s->listener) stop_listening(s);
    struct inet_socket** link = &sockets;
    while (*link && *link != s) link = &(*link)->next;
    if (!*link) panic("unregistered INET socket");
    *link = s->next;
    if (s->connection) {
        if (s->reset_close || s->connection->rx_len) tcp_reset(s->connection);
        else { tcp_shutdown_read(s->connection); tcp_close(s->connection); }
        tcp_release(s->connection); tcp_collect();
    }
    page_free((uint64_t)s);
}
static bool ping_allowed(void) {
    uint32_t gid = credentials_kernel_id(credentials_current(), true, 1);
    return (int64_t)gid >= sysctl_ping_group_low && (int64_t)gid <= sysctl_ping_group_high;
}
static int create(uint64_t family, uint64_t type, uint64_t protocol) {
    if (type & ~(0x80000ULL | 0x800ULL | 15ULL)) return -22;
    unsigned socktype = type & 15; uint8_t kind;
    if (socktype == 1) { if (protocol && protocol != 6) return -93; kind = KIND_STREAM; }
    else if (socktype == 2) {
        if (!protocol || protocol == 17) kind = KIND_DGRAM;
        else if ((family == AF_INET && protocol == IPPROTO_ICMP) || (family == AF_INET6 && protocol == IPPROTO_ICMPV6)) {
            if (!ping_allowed()) return -13;
            kind = KIND_PING;
        } else return -93;
    } else if (socktype == 3) {
        if (protocol > 255 || !protocol) return -93;
        if (!net_raw_capable()) return -1;
        kind = KIND_RAW;
    } else return -94;
    struct inet_socket* s = (void*)page_alloc(); if (!s) return -12;
    memset(s, 0, sizeof(*s));
    s->family = (uint8_t)family; s->kind = kind; s->protocol = (uint8_t)(kind == KIND_STREAM ? 6 : kind == KIND_DGRAM ? 17 : protocol);
    s->datagram = kind != KIND_STREAM; s->receive_limit = s->send_limit = 262144;
    s->keep_idle = 7200; s->keep_interval = 75; s->keep_count = 9;   /* Linux defaults */
    s->owner = task_kernel_uid(); s->isolated = task_net_namespace() != 0;
    s->ttl = -1; s->unicast_hops = -1; s->mcast_hops = 1; s->mcast_ttl = 1; s->mc_loop4 = s->mc_loop6 = true; s->mc_all = s->mc_all6 = true;
    s->pmtudisc4 = 1; s->pmtudisc6 = 1; s->checksum_offset = kind == KIND_RAW && family == AF_INET6 && protocol == IPPROTO_ICMPV6 ? 2 : -1;
    s->v6only = family == AF_INET6 && sysctl_bindv6only; s->hdrincl = kind == KIND_RAW && protocol == IPPROTO_RAW;
    if (family == AF_INET) { s->local = s->bound = s->remote = ip6a_from4(0); }
    int fd = fd_create_inet(s, type); if (fd < 0) { page_free((uint64_t)s); return fd; }
    s->next = sockets; sockets = s; s->observed_ready = inet_ready(s);
    s->observed_space = s->datagram ? s->send_limit : 0; return fd;
}

/* ---- local ports ---- */
/* Linux inet_rcv_saddr_equal / ipv6_rcv_saddr_equal: may two sockets with these local addresses share a port? */
static bool addresses_overlap(const struct inet_socket* a, const struct inet_socket* b) {
    const struct ip6a *x = &a->local, *y = &b->local;
    bool xm = is4(x) || (a->family == AF_INET), ym = is4(y) || (b->family == AF_INET);
    bool xany6 = a->family == AF_INET6 && ip6a_any(x), yany6 = b->family == AF_INET6 && ip6a_any(y);
    if (xm && ym) return !v4(x) || !v4(y) || v4(x) == v4(y);
    if (xany6 && yany6) return true;
    if (xany6) return !(a->v6only && ym);
    if (yany6) return !(b->v6only && xm);
    if (xm || ym) return false;
    return ip6a_eq(x, y) && (!ip6a_linklocal(x) || !a->local_scope || !b->local_scope || a->local_scope == b->local_scope);
}
static bool same_kind(const struct inet_socket* a, const struct inet_socket* b) {
    if (a->kind != b->kind) return false;
    if (a->kind == KIND_PING) return a->family == b->family;   /* echo identifiers are per family */
    return true;
}
/* `port` is taken when a socket of the same kind holds it on an overlapping address (Linux inet_csk_bind_conflict /
 * udp_lib_lport_inuse). For bind() (reuse) that is waived when both set SO_REUSEPORT (same owner), or both set
 * SO_REUSEADDR and the other one is not listening; either flag also ignores leftover TCP connections. */
static bool port_used(uint16_t port, struct inet_socket* except, bool reuse) {
    if (except->kind == KIND_STREAM && !(reuse && (except->reuse_address || except->reuse_port))) {
        uint32_t ip = is4(&except->local) ? v4(&except->local) : 0;
        if (tcp_port_in_use(port, ip)) return true;
    }
    for (struct inet_socket* s = sockets; s; s = s->next) {
        if (s->orphan || s == except || !same_kind(s, except) || s->local_port != port) continue;
        if (!addresses_overlap(s, except)) continue;
        if (except->bound_dev && s->bound_dev && except->bound_dev != s->bound_dev) continue;
        if (reuse && except->reuse_port && s->reuse_port && except->owner == s->owner) continue;
        if (reuse && except->reuse_address && s->reuse_address && !s->listening) continue;
        return true;
    }
    return false;
}
static uint16_t unused_port(struct inet_socket* s) {
    uint32_t start; if (!native_random(&start, sizeof(start))) return 0;
    uint32_t low = sysctl_local_port_low > 0 ? (uint32_t)sysctl_local_port_low : 32768, high = sysctl_local_port_high > 0 ? (uint32_t)sysctl_local_port_high : 60999;
    if (s->kind == KIND_PING) { low = 1; high = 65535; }
    if (high < low) high = low;
    uint32_t span = high - low + 1;
    for (uint32_t i = 0; i < span && i < 65536; i++) {
        uint16_t port = (uint16_t)(low + (start + i) % span);
        if (!port_used(port, s, false)) return port;
    }
    return 0;
}

/* ---- socket addresses ---- */
struct endpoint { struct ip6a address; uint32_t scope, flowinfo; uint16_t port, family; };
static int address_load(struct inet_socket* s, uint64_t pointer, uint64_t size, struct endpoint* out, bool sending) {
    if (size < 2) return -22;
    if (!accessible(pointer, size < 28 ? size : 28, false)) return -14;
    uint16_t family; copy_from_user(&family, (void*)pointer, 2);
    memset(out, 0, sizeof(*out)); out->family = family;
    if (s->family == AF_INET || (family == AF_INET && sending && !s->v6only)) {
        if (size < sizeof(struct inet_address)) return -22;
        struct inet_address a; copy_from_user(&a, (void*)pointer, sizeof(a));
        if (a.family != AF_INET) return s->family == AF_INET ? -97 : -97;
        out->address = ip6a_from4(ntohl(a.ip)); out->port = ntohs(a.port); return 0;
    }
    if (family == AF_INET && s->v6only) return sending ? -101 : -97;
    if (size < 24) return -22;
    struct inet6_address a; memset(&a, 0, sizeof(a)); copy_from_user(&a, (void*)pointer, size < 28 ? size : 28);
    if (a.family != AF_INET6) return -97;
    memcpy(out->address.b, a.address, 16); out->port = ntohs(a.port); out->flowinfo = ntohl(a.flowinfo);
    out->scope = size >= 28 ? a.scope : 0;
    return 0;
}
static int address_copy(struct inet_socket* s, uint64_t pointer, uint32_t capacity, const struct ip6a* address, uint32_t scope, uint16_t port, uint32_t* written) {
    if ((int32_t)capacity < 0) return -22;
    uint8_t bytes[28]; unsigned size;
    if (s->family == AF_INET) {
        struct inet_address a = {AF_INET, htons(port), htonl(is4(address) ? v4(address) : 0), {0}}; memcpy(bytes, &a, 16); size = 16;
    } else {
        struct inet6_address a = {AF_INET6, htons(port), 0, {0}, 0}; memcpy(a.address, address->b, 16);
        a.scope = ip6a_linklocal(address) || (ip6a_multicast(address) && ip6a_mc_scope(address) <= 2) ? scope : 0;
        memcpy(bytes, &a, 28); size = 28;
    }
    uint32_t amount = capacity < size ? capacity : size;
    if (amount && !accessible(pointer, amount, true)) return -14;
    if (amount && copy_to_user((void*)pointer, bytes, amount)) return -14;
    if (written) *written = size;
    return 0;
}
static int address_output(struct inet_socket* s, uint64_t pointer, uint64_t size_pointer, bool peer) {
    if (peer && (s->datagram ? !s->remote_port && !(s->kind == KIND_RAW && !wildcard(&s->remote)) : !s->connection || !s->connection->connected || !s->connection->active)) return -107;
    if (!accessible(size_pointer, 4, true)) return -14;
    uint32_t written;
    int error = address_copy(s, pointer, uget32(size_pointer), peer ? &s->remote : &s->local, peer ? s->remote_scope : s->local_scope,
                             s->kind == KIND_RAW ? 0 : peer ? s->remote_port : s->local_port, &written);
    if (error) return error;
    if (s->kind == KIND_RAW && s->family == AF_INET6 && !peer) {   /* raw sockets report the protocol as the port (Linux) */
        uint16_t proto = htons(s->protocol); if (uget32(size_pointer) >= 4) copy_to_user((uint8_t*)pointer + 2, &proto, 2);
    }
    return uput32(size_pointer, written);
}
static int bind_address(struct inet_socket* s, uint64_t pointer, uint64_t size) {
    if (s->connection || (s->local_port && s->kind != KIND_RAW) || s->listening || s->bound_set) return -22;
    struct endpoint e; int error = address_load(s, pointer, size, &e, false); if (error) return error;
    if (s->family == AF_INET6 && is4(&e.address) && s->v6only) return -22;
    /* CLONE_NEWNET (namespaces.c): an isolated network namespace has only loopback - no other address exists there. */
    if (net_isolated(s) && !(is4(&e.address) ? !v4(&e.address) || (v4(&e.address) >> 24) == 0x7F : ip6a_any(&e.address) || ip6a_loopback(&e.address)))
        return -99;   /* EADDRNOTAVAIL */
    bool local, multicast;
    if (is4(&e.address)) {
        uint32_t ip = v4(&e.address); multicast = (ip >> 28) == 14;
        local = !ip || netdev_v4_local(ip) || (ip == 0xFFFFFFFFU && s->datagram);
        if (multicast && !s->datagram) return -22;
    } else {
        multicast = ip6a_multicast(&e.address);
        if (multicast && !s->datagram) return -22;
        unsigned scope = ip6a_linklocal(&e.address) || multicast ? (e.scope ? e.scope : s->bound_dev) : 0;
        if ((ip6a_linklocal(&e.address) || (multicast && ip6a_mc_scope(&e.address) <= 2)) && !scope) return -22;
        if (scope && !netdev_by_index(scope)) return -19;
        if (scope && s->bound_dev && scope != s->bound_dev) return -22;
        local = ip6a_any(&e.address) || addrconf_local(&e.address, scope, true);
        e.scope = scope;
    }
    if (!local && !multicast && !s->freebind && !s->transparent && !(is4(&e.address) ? sysctl_ip_nonlocal_bind : sysctl_ipv6_nonlocal_bind)) return -99;
    uint16_t port = e.port;
    if (s->kind == KIND_RAW) {   /* raw sockets bind an address only */
        s->local = s->bound = e.address; s->local_scope = s->bound_scope = e.scope; s->bound_set = !wildcard(&e.address); return 0;
    }
    if (port && port < 1024 && s->kind != KIND_PING && !credentials_capable(credentials_current(), pid_namespace_root()->owner, 10)) return -13;
    struct ip6a saved = s->local; uint32_t saved_scope = s->local_scope;
    s->local = e.address; s->local_scope = e.scope;
    if (port ? port_used(port, s, true) : (s->bind_no_port && s->kind == KIND_STREAM ? false : !(port = unused_port(s)))) { s->local = saved; s->local_scope = saved_scope; return -98; }
    s->bound = e.address; s->bound_scope = e.scope; s->local_port = port; s->bound_set = true; return 0;
}
static uint64_t deadline_after(uint64_t timeout) {
    if (!timeout) return 0;
    uint64_t now = native_clock_ns(); return timeout > ~0ULL - now ? ~0ULL : now + timeout;
}
/* May `s` send to `e`? 0 or -errno (Linux ip_route_connect / ip6_dst_lookup errors). */
static int route_check(struct inet_socket* s, struct endpoint* e) {
    /* CLONE_NEWNET (process.c, namespaces.c): an isolated network namespace has only loopback - no route anywhere else.
     * Every outbound path (connect, sendto/sendmsg with or without a destination, ping and raw sockets) is routed here. */
    if (net_isolated(s) && !(is4(&e->address) ? (v4(&e->address) >> 24) == 0x7F : ip6a_loopback(&e->address) || ip6a_any(&e->address)))
        return -101;   /* ENETUNREACH */
    if (is4(&e->address)) {
        uint32_t ip = v4(&e->address);
        /* 127.0.0.0/8 and our own addresses go through the loopback ring, with or without a NIC */
        if (!native_network_available() && !ip_is_local(ip) && !netdev_v4_local(ip)) return -101;
        if (is4(&s->bound) && (v4(&s->bound) >> 24) == 127 && !ip_is_local(ip)) return -22;   /* a loopback source never leaves the host */
        bool broadcast = ip == 0xFFFFFFFFU;
        for (struct netdev* d = netdevs; d && !broadcast; d = d->next)
            for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && d->v4[i].broadcast == ip) broadcast = true;
        if (broadcast) return !s->datagram ? -101 : !s->broadcast ? -13 : !e->port && s->kind == KIND_DGRAM ? -22 : 0;
        bool multicast = (ip >> 28) == 14;
        if (multicast) return s->datagram && (ip >> 28) == 14 && ip < 0xF0000000U ? 0 : -101;
        if (!ip || (ip >> 28) >= 14 || (!e->port && s->kind != KIND_PING && s->kind != KIND_RAW)) return -101;
        if (ip_is_local(ip)) return 0;
        struct route_result r; struct ip6a src = s->bound;
        int error = route_lookup(AF_INET, &e->address, wildcard(&src) ? NULL : &src, s->bound_dev, s->mark, &r);
        return error;
    }
    if (ip6a_any(&e->address)) e->address = ip6a_loopback_address;   /* "connect to self" (Linux) */
    bool multicast = ip6a_multicast(&e->address);
    if (multicast && !s->datagram) return -101;
    if (!e->port && s->kind != KIND_PING && s->kind != KIND_RAW) return -101;
    if (ip6a_linklocal(&e->address) || (multicast && ip6a_mc_scope(&e->address) <= 2)) {
        unsigned scope = e->scope ? e->scope : s->bound_dev ? s->bound_dev : multicast && s->mcast_if6 ? s->mcast_if6 : s->bound_scope;
        if (!scope && multicast) {   /* Linux: the multicast route's device (ff00::/8), not EINVAL */
            struct route_result r;
            if (!route_lookup(AF_INET6, &e->address, NULL, 0, s->mark, &r) && r.dev && r.dev != netdev_lo) scope = r.dev->index;
            else if (netdev_eth0) scope = netdev_eth0->index;
        }
        if (!scope) return -22;
        if (s->bound_dev && scope != s->bound_dev) return -22;
        if (!netdev_by_index(scope)) return -19;
        e->scope = scope; return 0;
    }
    e->scope = 0;
    if (multicast) return 0;
    if (ip6a_loopback(&e->address) || addrconf_local(&e->address, 0, true)) return 0;
    if (ip6a_loopback(&s->bound)) return -22;
    struct route_result r; struct ip6a src = s->bound;
    return route_lookup(AF_INET6, &e->address, wildcard(&src) || is4(&src) ? NULL : &src, s->bound_dev, s->mark, &r);
}
/* The source address a connection to `e` uses (bound address, else the route's choice). */
static bool choose_source(struct inet_socket* s, const struct endpoint* e, struct ip6a* out, uint32_t* scope) {
    *scope = 0;
    if (!wildcard(&s->bound)) { *out = s->bound; *scope = s->bound_scope; return true; }
    if (is4(&e->address)) {
        uint32_t source = ip_source_address(v4(&e->address)); if (!source) return false;
        *out = ip6a_from4(source); return true;
    }
    if (ip6a_loopback(&e->address)) { *out = ip6a_loopback_address; return true; }
    if (addrconf_local(&e->address, e->scope, true)) { *out = e->address; *scope = e->scope; return true; }
    struct netdev* dev = e->scope ? netdev_by_index(e->scope) : NULL;
    if (!dev) {
        struct route_result r;
        if (route_lookup(AF_INET6, &e->address, NULL, s->bound_dev, s->mark, &r)) return false;
        dev = r.dev;
        if (!source_select6(&e->address, dev, s->addr_prefs, out)) { *out = r.source; if (ip6a_any(out)) return false; }
    } else if (!source_select6(&e->address, dev, s->addr_prefs, out)) return false;
    if (ip6a_linklocal(out)) *scope = dev ? dev->index : e->scope;
    return true;
}
static int connect_address(struct inet_socket* s, uint64_t pointer, uint64_t size, bool nonblock) {
    if (s->datagram && size >= 2) {
        if (!accessible(pointer, 2, false)) return -14;
        if (!uget16(pointer)) {   /* AF_UNSPEC: dissolve the association */
            s->remote = s->family == AF_INET ? ip6a_from4(0) : ip6a_unspecified; s->remote_port = 0; s->remote_scope = 0;
            s->local = s->bound; s->local_scope = s->bound_scope;
            if (!s->bound_set && !s->transmit) s->local_port = 0;
            s->read_closed = s->write_closed = false; changed(s); return 0;
        }
    }
    struct endpoint e; int error = address_load(s, pointer, size, &e, s->datagram); if (error) return error;
    if (s->listening) return -106;   /* EISCONN, as Linux for a socket in TCP_LISTEN */
    if (s->connection) {
        if (!s->connecting) return -106;
        if (s->connection->connected && s->connection->active) { s->connecting = false; return 0; }
        if (s->connection->active) return -114;
        error = take_error(s); tcp_release(s->connection); s->connection = NULL; tcp_collect(); s->connecting = false;
        changed(s); return error ? -error : -103;
    }
    if (s->family == AF_INET6 && is4(&e.address) && s->v6only) return -101;
    if (s->family == AF_INET6 && !is4(&e.address) && is4(&s->bound) && !any4(&s->bound)) return -22;   /* bound to an IPv4-mapped address */
    error = route_check(s, &e); if (error) return error;
    if (s->kind == KIND_RAW) { s->remote = e.address; s->remote_scope = e.scope; s->read_closed = s->write_closed = false; changed(s); return 0; }
    struct ip6a source; uint32_t source_scope;
    if (!choose_source(s, &e, &source, &source_scope)) return -99;
    if (!s->local_port) {
        struct ip6a saved = s->local; s->local = s->bound;
        uint16_t port = unused_port(s); s->local = saved;
        if (!port) return s->datagram ? -11 : -99;
        s->local_port = port;
    }
    if (!s->datagram) {
        /* SO_REUSEADDR/SO_REUSEPORT let sockets share a port: a second connection to the same peer would collide with
         * the first one's segments, so it is refused (Linux __inet_check_established) */
        uint32_t l = tcp_value(&source, source_scope, false), r = tcp_value(&e.address, e.scope, false);
        if (l && r && tcp_tuple_in_use(l, s->local_port, r, e.port)) return -99;
        if (!is4(&source) && (!(l = tcp_value(&source, source_scope, true)) || !(r = tcp_value(&e.address, e.scope, true)))) return -105;
        if (is4(&source)) { l = v4(&source); r = v4(&e.address); }
        s->local = source; s->local_scope = source_scope; s->remote = e.address; s->remote_scope = e.scope; s->remote_port = e.port;
        s->connection = tcp_connect_from(l, r, e.port, s->local_port, s->stream_receive, s->stream_send);
        if (!s->connection) { tcp_collect(); return -12; }
    } else {
        s->local = wildcard(&s->bound) ? source : s->bound; s->local_scope = source_scope;
        s->remote = e.address; s->remote_scope = e.scope; s->remote_port = e.port;
        s->read_closed = s->write_closed = false; changed(s); return 0;
    }
    tcp_retain(s->connection); tcp_set_nodelay(s->connection, s->nodelay);
    tcp_set_keepalive(s->connection, s->keepalive, s->keep_idle, s->keep_interval, s->keep_count);
    s->connecting = true; changed(s);
    if (nonblock) return -115;
    uint64_t deadline = deadline_after(s->send_timeout);
    while (s->connection->active && !s->connection->connected) {
        error = task_wait_until(s, deadline); if (error) return error == -110 ? -115 : error;
    }
    if (s->connection->connected && s->connection->active) { s->connecting = false; return 0; }
    error = take_error(s); return error ? -error : -103;
}
unsigned inet_available(struct inet_socket* s) { return s->datagram ? (s->received ? s->received->length : 0) : s->connection ? s->connection->rx_len : 0; }
static int pipe_error(uint64_t flags) {
    if (!(flags & NOSIGNAL)) signals_enqueue(signals_current(), 13, 0, task_visible_pid(task_pid()), task_kernel_uid(), 0, false);
    return -32;
}
static void datagram_error(struct inet_socket* s, const struct ip6a* destination, uint16_t port, const struct ip6a* offender,
                           const void* data, unsigned size, struct extended_error error) {
    if (s->orphan) return;
    s->error = (int)error.number;
    unsigned charge = datagram_charge(size);
    if (s->receive_errors && s->receive_used <= s->receive_limit && charge <= s->receive_limit - s->receive_used) {
        struct datagram* packet = (void*)arena_alloc(charge);
        if (packet) {
            memset(packet, 0, sizeof(*packet));
            packet->peer = *destination; packet->port = port; if (offender) packet->offender = *offender; packet->length = size;
            packet->charge = charge; packet->error = error; if (size) memcpy(packet->data, data, size);
            if (s->errors_tail) s->errors_tail->next = packet; else s->errors = packet;
            s->errors_tail = packet; s->receive_used += charge;
        }
    }
    task_wake_channel(s); event_source_changed(s, 8); changed(s);
}

/* ---- ICMP errors ---- */
static void deliver_error(uint8_t family, const struct ip6a* local, uint16_t lport, const struct ip6a* remote, uint16_t rport,
                          uint8_t kind, const struct ip6a* reporter, const uint8_t* payload, unsigned payload_length, struct extended_error report) {
    for (struct inet_socket* s = sockets; s; s = s->next) {   /* by address too: sockets may share a port (Linux) */
        if (s->kind != kind || s->orphan || !s->local_port || s->local_port != lport) continue;
        if (family == AF_INET6 ? (s->family != AF_INET6 || is4(&s->local)) : (s->family == AF_INET6 && s->v6only)) continue;
        if (!wildcard(&s->local) && !ip6a_eq(&s->local, local)) continue;
        if (!s->receive_errors && !s->remote_port && kind != KIND_PING) continue;
        if (s->remote_port && (!ip6a_eq(&s->remote, remote) || s->remote_port != rport)) continue;
        if (kind == KIND_PING && !s->receive_errors && !s->remote_port && !(s->kind == KIND_PING && !wildcard(&s->remote))) {
            /* ping sockets: errors only to those asking for them */
            if (!s->receive_errors) continue;
        }
        datagram_error(s, remote, rport, reporter, payload, payload_length, report); return;
    }
}
static int icmp4_errno(unsigned type, unsigned code) {
    if (type == 3 && code <= 15) return code == 3 ? 111 : code == 4 ? 90 : code == 2 ? 92 : code == 0 || code == 6 ? 101 : code == 9 || code == 10 || code == 13 ? 13 : 113;
    if (type == 11 && code <= 1) return 113;
    if (type == 12 && code <= 2) return 71;
    return 0;
}
void inet_icmp_error(uint32_t reporter, const void* data, uint16_t length) {
    if (length < 8 + IP_HEADER_LEN + 8) return;
    const uint8_t* bytes = data; unsigned type = bytes[0], code = bytes[1]; int error = icmp4_errno(type, code);
    if (!error) return;
    const ip_header_t* ip = (const void*)(bytes + 8); unsigned header = (ip->ver_ihl & 15) * 4;
    if (ip->ver_ihl >> 4 != 4 || header < IP_HEADER_LEN || header + 8 > length - 8U || !ip_is_local(ntohl(ip->src_ip)) ||
        (ntohs(ip->flags_frag) & 0x1FFFU) || ip_checksum(ip, (uint16_t)header)) return;
    uint32_t destination = ntohl(ip->dst_ip), source = ntohl(ip->src_ip);
    const uint8_t* l4 = bytes + 8 + header;
    if (type == 3 && code == 4) { struct ip6a d = ip6a_from4(destination); unsigned mtu = ((unsigned)bytes[6] << 8) | bytes[7]; if (mtu >= 68) pmtu_update(AF_INET, &d, mtu); }
    if (ip->protocol == IP_PROTO_TCP) {
        uint16_t lport = be16(l4), rport = be16(l4 + 2); uint32_t seq = be32(l4 + 4);
        if (type == 3 && code == 4) tcp_pmtu_update(source, lport, destination, rport, ((unsigned)bytes[6] << 8) | bytes[7]);
        else if (type == 3) tcp_icmp_error(source, lport, destination, rport, seq, error);
        return;
    }
    struct extended_error report = {.number = (uint32_t)error, .origin = 2, .type = (uint8_t)type, .code = (uint8_t)code};
    if (type == 3 && code == 4) report.info = ((unsigned)bytes[6] << 8) | bytes[7];
    if (type == 12) report.info = bytes[4];
    struct ip6a local = ip6a_from4(source), remote = ip6a_from4(destination), from = ip6a_from4(reporter);
    if (ip->protocol == IP_PROTO_UDP) {
        unsigned udp_length = be16(l4 + 4);
        if (udp_length < 8 || ntohs(ip->total_len) < header + udp_length) return;
        unsigned payload = length - 8 - header - 8; if (payload > udp_length - 8) payload = udp_length - 8;
        deliver_error(AF_INET, &local, be16(l4), &remote, be16(l4 + 2), KIND_DGRAM, &from, l4 + 8, payload, report);
    } else if (ip->protocol == IP_PROTO_ICMP && l4[0] == 8) {
        deliver_error(AF_INET, &local, be16(l4 + 4), &remote, 0, KIND_PING, &from, l4, length - 8 - header, report);
    }
}
void inet_icmp6_error(const struct ip6a* reporter, uint8_t type, uint8_t code, uint32_t info, const uint8_t* inner, unsigned length) {
    if (length < 48) return;
    const struct ip6a* src = (const void*)(inner + 8); const struct ip6a* dst = (const void*)(inner + 24);
    uint8_t next = inner[6]; unsigned at = 40, depth = 0;
    while ((next == IPPROTO_HOPOPTS || next == IPPROTO_DSTOPTS || next == IPPROTO_ROUTING || next == IPPROTO_FRAGMENT) && depth++ < 16) {
        if (length < at + 8) return;
        if (next == IPPROTO_FRAGMENT) { if (be16(inner + at + 2) & 0xFFF8) return; next = inner[at]; at += 8; continue; }
        next = inner[at]; at += (inner[at + 1] + 1u) * 8u;
    }
    if (length < at + 8) return;
    int error = type == 1 ? (code == 0 ? 101 : code == 1 || code == 5 || code == 6 ? 13 : code == 4 ? 111 : 113) : type == 2 ? 90 : type == 3 ? 113 : 71;
    struct extended_error report = {.number = (uint32_t)error, .origin = 3, .type = type, .code = code, .info = type == 2 || type == 4 ? info : 0};
    if (next == IPPROTO_UDP) {
        unsigned udp_length = be16(inner + at + 4); if (udp_length < 8) return;
        unsigned payload = length - at - 8; if (payload > udp_length - 8) payload = udp_length - 8;
        deliver_error(AF_INET6, src, be16(inner + at), dst, be16(inner + at + 2), KIND_DGRAM, reporter, inner + at + 8, payload, report);
    } else if (next == IPPROTO_ICMPV6 && inner[at] == 128) {
        deliver_error(AF_INET6, src, be16(inner + at + 4), dst, 0, KIND_PING, reporter, inner + at, length - at, report);
    }
}

/* ---- receive ---- */
static bool socket_takes(const struct inet_socket* o, const struct inet_rx_info* info, bool v6) {
    if (o->orphan) return false;
    if (o->isolated && info->ifindex != 1) return false;   /* CLONE_NEWNET: loopback only */
    if (v6 ? (o->family != AF_INET6 || (!wildcard(&o->local) && is4(&o->local))) : (o->family == AF_INET6 && o->v6only)) return false;
    if (o->bound_dev && o->bound_dev != info->ifindex && info->ifindex != 1) return false;
    if (!wildcard(&o->local) && !ip6a_eq(&o->local, &info->dst)) {
        if (!(info->multicast && wildcard(&o->bound))) return false;
    }
    if (ip6a_linklocal(&o->local) && o->local_scope && info->ifindex != o->local_scope && info->ifindex != 1) return false;
    return true;
}
/* A multicast datagram (not a broadcast) passes the socket's source filter for the group on the arrival interface. */
static bool multicast_allowed(const struct inet_socket* s, const struct inet_rx_info* info) {
    if (is4(&info->dst) ? (v4(&info->dst) >> 28) != 14 : !ip6a_multicast(&info->dst)) return true;
    return mc_sock_allows(s->memberships, is4(&info->dst) ? s->mc_all : s->mc_all6, &info->dst, info->ifindex, &info->src);
}
void inet_mc_visit(void (*visit)(struct mc_membership* membership, void* context), void* context) {
    for (struct inet_socket* s = sockets; s; s = s->next)
        for (struct mc_membership* m = s->memberships; m; m = m->next) visit(m, context);
}
static void enqueue(struct inet_socket* s, const struct inet_rx_info* info, uint16_t port, const void* data, unsigned length) {
    unsigned charge = datagram_charge(length);
    if (s->receive_used > s->receive_limit || charge > s->receive_limit - s->receive_used) return;
    struct datagram* packet = (void*)arena_alloc(charge); if (!packet) return;
    memset(packet, 0, sizeof(*packet));
    packet->peer = info->src; packet->local = info->dst; packet->port = port; packet->length = length; packet->charge = charge;
    packet->ifindex = info->ifindex; packet->hop_limit = info->hop_limit; packet->tclass = info->tclass;
    packet->peer_scope = ip6a_linklocal(&info->src) ? info->ifindex : 0; packet->stamp_ns = native_realtime_ns();
    if (length) memcpy(packet->data, data, length);
    if (s->received_tail) s->received_tail->next = packet; else s->received = packet;
    s->received_tail = packet; s->receive_used += charge; s->datagram_sequence++;
    changed(s);
}
bool inet_udp_receive(const struct inet_rx_info* info, const void* data, unsigned length) {
    bool v6 = !is4(&info->src);
    if (inet_wireguard_port(info->dst_port, v6 ? AF_INET6 : AF_INET) && wireguard_receive(info, data, length)) return true;
    struct inet_socket* s = NULL; bool any = false;   /* a connected or address-bound socket wins over a wildcard one (Linux) */
    for (struct inet_socket* o = sockets; o; o = o->next) {
        if (o->kind != KIND_DGRAM || !o->local_port || o->local_port != info->dst_port || !socket_takes(o, info, v6)) continue;
        if (o->remote_port && (o->remote_port != info->src_port || !ip6a_eq(&o->remote, &info->src))) continue;
        if (info->multicast && !multicast_allowed(o, info)) continue;
        any = true;
        if (info->multicast) { enqueue(o, info, info->src_port, data, length); continue; }   /* every member gets a copy */
        if (!s || !wildcard(&o->local) || o->remote_port) s = o;
        if (!wildcard(&s->local) || s->remote_port) break;
    }
    if (info->multicast) return any;
    if (!s) return false;
    enqueue(s, info, info->src_port, data, length);
    return true;
}
void inet_datagram_receive(uint32_t source, uint32_t destination, uint16_t source_port, uint16_t destination_port, const void* data, uint16_t length) {
    struct inet_rx_info info; memset(&info, 0, sizeof(info));
    info.src = ip6a_from4(source); info.dst = ip6a_from4(destination); info.src_port = source_port; info.dst_port = destination_port;
    struct netdev* in = netdev_rx_current; info.ifindex = in ? in->index : (ip_is_local(source) ? 1 : netdev_eth0 ? netdev_eth0->index : 1);
    info.hop_limit = 64; info.multicast = (destination >> 28) == 14 || destination == 0xFFFFFFFFU;
    for (struct netdev* d = netdevs; d && !info.multicast; d = d->next)
        for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && d->v4[i].broadcast == destination) info.multicast = true;
    if (!inet_udp_receive(&info, data, length) && !info.multicast) icmp4_port_unreachable(source, destination, source_port, destination_port, data, length);
}
bool inet_ping_receive(uint8_t family, const struct inet_rx_info* info, const uint8_t* message, unsigned length) {
    if (length < 8) return false;
    uint16_t id = be16(message + 4); bool v6 = family == AF_INET6, taken = false;
    for (struct inet_socket* s = sockets; s; s = s->next) {
        if (s->kind != KIND_PING || s->family != family || s->local_port != id || !socket_takes(s, info, v6)) continue;
        if (s->remote_port || !wildcard(&s->remote)) { if (!ip6a_eq(&s->remote, &info->src)) continue; }
        enqueue(s, info, 0, message, length); taken = true;
    }
    return taken;
}
bool inet_raw_protocol(uint8_t family, uint8_t protocol) {
    for (struct inet_socket* s = sockets; s; s = s->next) if (s->kind == KIND_RAW && s->family == family && s->protocol == protocol) return true;
    return false;
}
void inet_raw_receive(uint8_t family, const struct inet_rx_info* info, uint8_t protocol, const uint8_t* packet, unsigned length,
                      const uint8_t* payload, unsigned payload_length) {
    for (struct inet_socket* s = sockets; s; s = s->next) {
        if (s->kind != KIND_RAW || s->family != family || s->orphan || s->protocol != protocol) continue;
        if (s->isolated && info->ifindex != 1) continue;   /* CLONE_NEWNET: loopback only */
        if (s->bound_dev && s->bound_dev != info->ifindex) continue;
        if (s->bound_set && !ip6a_eq(&s->local, &info->dst)) continue;
        if (!wildcard(&s->remote) && !ip6a_eq(&s->remote, &info->src)) continue;
        if (info->multicast && !multicast_allowed(s, info)) continue;
        if (family == AF_INET && protocol == IPPROTO_ICMP && payload_length >= 1 && payload[0] < 32 && (s->icmp_filter & (1U << payload[0]))) continue;
        if (family == AF_INET6 && protocol == IPPROTO_ICMPV6 && payload_length >= 1 && (s->icmp6_filter[payload[0] >> 5] & (1U << (payload[0] & 31)))) continue;
        if (family == AF_INET6 && s->checksum_offset >= 0 && protocol != IPPROTO_ICMPV6) {
            /* IPV6_CHECKSUM: verify the checksum at the offset (RFC 3542 3.1) */
            if ((unsigned)s->checksum_offset + 2 > payload_length ||
                ip6_checksum(&info->src, &info->dst, protocol, payload, payload_length)) continue;
        }
        if (family == AF_INET) enqueue(s, info, 0, packet, length);   /* IPv4 raw sockets see the IP header */
        else enqueue(s, info, 0, payload, payload_length);
    }
}
void inet_dev_down(struct netdev* dev) {
    for (struct inet_socket* s = sockets; s; s = s->next) mc_sock_dev_gone(&s->memberships, &s->membership_count, dev->index);
}

/* ---- send ---- */
static int checksum_place(struct inet_socket* s, uint8_t* data, unsigned length, const struct ip6a* src, const struct ip6a* dst) {
    if (s->checksum_offset < 0) return 0;
    unsigned offset = (unsigned)s->checksum_offset;
    if (offset + 2 > length) return -22;
    data[offset] = data[offset + 1] = 0;
    uint16_t sum = ip6_checksum(src, dst, s->protocol, data, length);
    put16(data + offset, sum);
    return 0;
}
struct send_control { bool has_source; struct ip6a source; unsigned ifindex; int hops, tclass; bool dontfrag; };
/* Ancillary data of sendmsg (struct cmsghdr: 8-byte length, level, type). */
static int parse_control(struct inet_socket* s, uint64_t pointer, uint64_t size, struct send_control* c) {
    c->has_source = false; c->ifindex = 0; c->hops = -1; c->tclass = -1; c->dontfrag = false;
    if (!size) return 0;
    if (size > 4096) return -105;
    if (!accessible(pointer, size, false)) return -14;
    uint8_t buffer[4096]; copy_from_user(buffer, (void*)pointer, size);
    for (uint64_t at = 0; at + 16 <= size;) {
        uint64_t length; int32_t level, type; memcpy(&length, buffer + at, 8); memcpy(&level, buffer + at + 8, 4); memcpy(&type, buffer + at + 12, 4);
        if (length < 16 || length > size - at) return -22;
        const uint8_t* data = buffer + at + 16; uint64_t n = length - 16;
        if (level == 0 && s->datagram) {   /* SOL_IP */
            if (type == 8 && n >= 12) {   /* IP_PKTINFO: ifindex, spec_dst (source), addr */
                int32_t index; memcpy(&index, data, 4); uint32_t spec; memcpy(&spec, data + 4, 4);
                if (index) c->ifindex = (unsigned)index;
                if (spec) { c->source = ip6a_from4(ntohl(spec)); c->has_source = true; }
            } else if (type == 2 && n >= 4) { int32_t v; memcpy(&v, data, 4); if (v < 0 || v > 255) return -22; c->hops = v; }   /* IP_TTL */
            else if (type == 1 && n >= 1) c->tclass = data[0];                                                                    /* IP_TOS */
            else return -22;
        } else if (level == IPPROTO_IPV6 && s->datagram && s->family == AF_INET6) {
            if (type == 50 && n >= 20) {   /* IPV6_PKTINFO */
                memcpy(c->source.b, data, 16); int32_t index; memcpy(&index, data + 16, 4);
                if (!ip6a_any(&c->source)) { if (!addrconf_local(&c->source, (unsigned)index, false) && !ip6a_loopback(&c->source) && !s->freebind && !s->transparent) return -99; c->has_source = true; }
                if (index) { if (!netdev_by_index((unsigned)index)) return -19; c->ifindex = (unsigned)index; }
            } else if (type == 52 && n >= 4) { int32_t v; memcpy(&v, data, 4); if (v < -1 || v > 255) return -22; c->hops = v; }   /* IPV6_HOPLIMIT */
            else if (type == 67 && n >= 4) { int32_t v; memcpy(&v, data, 4); if (v < -1 || v > 255) return -22; c->tclass = v; }  /* IPV6_TCLASS */
            else if (type == 62 && n >= 4) { int32_t v; memcpy(&v, data, 4); c->dontfrag = v != 0; }                              /* IPV6_DONTFRAG */
            else return -22;
        } else if (level == 1) {
            if (type != 36 && type != 37 && type != 61 && type != 1) return -22;   /* SO_MARK, SO_TIMESTAMPING, SCM_TXTIME, SCM_RIGHTS(ignored by TCP) */
        } else if (s->datagram) return -22;
        at += (length + 7) & ~7ULL;
    }
    return 0;
}
static int64_t datagram_send(struct inet_socket* s, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags,
                             const struct endpoint* address, const struct send_control* control) {
    if (flags & ~(DONTWAIT | NOSIGNAL | CONFIRM | MORE)) return -95;
    unsigned limit = s->kind == KIND_DGRAM ? (address ? (is4(&address->address) ? 65507 : 65527) : (is4(&s->remote) ? 65507 : 65527)) : 65535 - 20;
    if (total > limit) return -90;
    if (s->kind == KIND_PING && total < 8) return -22;
    if (s->error) return -take_error(s);
    if (s->write_closed) return -32;
    struct endpoint e;
    if (address) e = *address;
    else {
        if (!s->remote_port && !(s->kind != KIND_DGRAM && !wildcard(&s->remote))) return s->kind == KIND_DGRAM ? -89 : -89;
        e.address = s->remote; e.scope = s->remote_scope; e.port = s->remote_port;
    }
    if (s->family == AF_INET6 && is4(&e.address) && s->v6only) return -101;
    if (control && control->ifindex && !e.scope && !is4(&e.address) &&
        (ip6a_linklocal(&e.address) || (ip6a_multicast(&e.address) && ip6a_mc_scope(&e.address) <= 2)))
        e.scope = control->ifindex;   /* IPV6_PKTINFO names the interface (avahi sends to ff02::fb this way) */
    int error = route_check(s, &e); if (error) return error;
    bool v6 = !is4(&e.address);
    bool dontfrag = v6 ? s->dontfrag6 || s->pmtudisc6 == 2 || s->pmtudisc6 == 3 || (control && control->dontfrag) : s->pmtudisc4 == 2 || s->pmtudisc4 == 3;
    if (dontfrag && !(s->kind == KIND_RAW && s->hdrincl)) {   /* EMSGSIZE at once, as Linux's ip_append_data / ip6_append_data */
        unsigned mtu = (v6 ? ip6a_loopback(&e.address) || addrconf_local(&e.address, 0, true) : ip_is_local(v4(&e.address))) ? 1500 :
                       route_mtu(v6 ? AF_INET6 : AF_INET, &e.address, e.scope ? netdev_by_index(e.scope) : NULL);
        unsigned headers = (v6 ? 40 : 20) + (s->kind == KIND_DGRAM ? 8 : 0);
        if (total + headers > mtu) return -90;
    }
    if (s->kind != KIND_RAW && !s->local_port && !(s->local_port = unused_port(s))) return -11;
    unsigned charge = datagram_charge((unsigned)total); uint64_t deadline = deadline_after(s->send_timeout);
    while (s->send_used > s->send_limit || charge > s->send_limit - s->send_used) {
        if (flags & DONTWAIT) return -11;
        error = task_wait_until(s, deadline); if (error) return error == -110 ? -11 : error;
        if (s->error) return -take_error(s);
        if (s->write_closed) return -32;
    }
    if (!iov_access(vectors, count, 0, total, false)) return -14;
    struct datagram* packet = (void*)arena_alloc(charge); if (!packet) return -12;
    memset(packet, 0, sizeof(*packet));
    packet->peer = e.address; packet->peer_scope = e.scope; packet->port = e.port; packet->length = (uint32_t)total; packet->charge = charge;
    packet->deadline = deadline_after(5000000000ULL); packet->send_hops = -1; packet->send_tclass = -1;
    if (control) {
        if (control->has_source) packet->local = control->source;
        packet->ifindex = control->ifindex; packet->send_hops = (int16_t)control->hops; packet->send_tclass = (int16_t)control->tclass;
        packet->send_dontfrag = control->dontfrag;
    }
    iov_copy(vectors, count, 0, packet->data, (unsigned)total, true);
    if (s->kind == KIND_PING) {
        uint8_t expected = s->family == AF_INET ? 8 : 128;
        if (packet->data[0] != expected || packet->data[1]) { arena_free((uint64_t)packet, charge); return -22; }
        put16(packet->data + 4, s->local_port);   /* the kernel owns the identifier */
    }
    if (s->transmit_tail) s->transmit_tail->next = packet; else s->transmit = packet;
    s->transmit_tail = packet; s->send_used += charge;
    flush_datagrams(s); changed(s); return (int64_t)total;
}
/* ---- receive and ancillary data ---- */
struct control_writer { uint8_t* buffer; unsigned capacity, used; bool truncated; };
static void control_put(struct control_writer* w, int32_t level, int32_t type, const void* data, unsigned length) {
    unsigned space = 16 + ((length + 7) & ~7U), needed = 16 + length;
    if (w->used + needed > w->capacity) { w->truncated = true; return; }
    uint64_t header = 16 + length; memcpy(w->buffer + w->used, &header, 8); memcpy(w->buffer + w->used + 8, &level, 4); memcpy(w->buffer + w->used + 12, &type, 4);
    memcpy(w->buffer + w->used + 16, data, length);
    if (w->used + space <= w->capacity) { memset(w->buffer + w->used + needed, 0, space - needed); w->used += space; } else w->used = w->capacity;
}
static void receive_controls(struct inet_socket* s, const struct datagram* packet, struct control_writer* w) {
    bool v4packet = is4(&packet->peer);
    if (s->timestamp || s->timestamp_ns) {
        int64_t t[2] = {(int64_t)(packet->stamp_ns / 1000000000ULL), (int64_t)(packet->stamp_ns % 1000000000ULL)};
        if (!s->timestamp_ns) t[1] /= 1000;
        control_put(w, 1, s->timestamp_ns ? 35 : 29, t, 16);
    }
    if (v4packet) {
        if (s->pktinfo4) {
            uint8_t info[12]; int32_t index = (int32_t)packet->ifindex; uint32_t a = htonl(v4(&packet->local));
            memcpy(info, &index, 4); memcpy(info + 4, &a, 4); memcpy(info + 8, &a, 4);
            control_put(w, 0, 8, info, 12);
        }
        if (s->recv_ttl) { int32_t ttl = packet->hop_limit; control_put(w, 0, 2, &ttl, 4); }
        if (s->recv_tos) control_put(w, 0, 1, &packet->tclass, 1);
        if (s->origdst4) { struct inet_address a = {AF_INET, htons(s->local_port), htonl(v4(&packet->local)), {0}}; control_put(w, 0, 20, &a, 16); }
    }
    if (s->family == AF_INET6) {
        if (s->pktinfo6) { uint8_t info[20]; memcpy(info, packet->local.b, 16); int32_t index = (int32_t)packet->ifindex; memcpy(info + 16, &index, 4); control_put(w, IPPROTO_IPV6, 50, info, 20); }
        if (s->recv_hoplimit) { int32_t hops = packet->hop_limit; control_put(w, IPPROTO_IPV6, 52, &hops, 4); }
        if (s->recv_tclass) { int32_t tclass = packet->tclass; control_put(w, IPPROTO_IPV6, 67, &tclass, 4); }
        if (s->origdst6) {
            struct inet6_address a = {AF_INET6, htons(s->local_port), 0, {0}, 0}; memcpy(a.address, packet->local.b, 16);
            if (ip6a_linklocal(&packet->local)) a.scope = packet->ifindex;
            control_put(w, IPPROTO_IPV6, 74, &a, 28);
        }
    }
}
static int64_t datagram_read(struct inet_socket* s, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags, struct message64* message) {
    if (flags & ~(DONTWAIT | NOSIGNAL | PEEK | WAITALL | TRUNCATE | ERROR_QUEUE | 0x40000000U)) return -95;
    bool errors = (flags & ERROR_QUEUE) != 0;
    if (errors && !s->errors) return -11;
    uint64_t deadline = deadline_after(s->receive_timeout);
    while (!errors && !s->received) {
        if (s->error) return -take_error(s);
        if (s->read_closed) { if (message) message->name_size = message->flags = message->control_size = 0; return 0; }
        if (flags & DONTWAIT) return -11;
        int error = task_wait_until(s, deadline); if (error) return error == -110 ? -11 : error;
    }
    struct datagram* packet = errors ? s->errors : s->received;
    unsigned length = packet->length, amount = total < length ? (unsigned)total : length;
    if (!iov_access(vectors, count, 0, amount, true)) return -14;
    if (message) {
        if (message->name) {
            uint32_t written; int error = address_copy(s, message->name, message->name_size, &packet->peer, packet->peer_scope, packet->port, &written);
            if (error) return error;
            message->name_size = written;
        } else message->name_size = 0;
        message->flags = (amount < length ? TRUNCATE : 0) | (errors ? ERROR_QUEUE : 0);
        unsigned capacity = message->control_size > 4096 ? 4096 : (unsigned)message->control_size;
        uint8_t control[4096]; struct control_writer w = {control, capacity, 0, false};
        if (errors) {
            bool v6 = s->family == AF_INET6 && !is4(&packet->peer);
            if (v6) {
                struct { struct extended_error error; struct inet6_address offender; } e = {packet->error, {ip6a_any(&packet->offender) ? 0 : AF_INET6, 0, 0, {0}, 0}};
                memcpy(e.offender.address, packet->offender.b, 16);
                control_put(&w, IPPROTO_IPV6, 25, &e, sizeof(e));
            } else {
                struct { struct extended_error error; struct inet_address offender; } e =
                    {packet->error, {is4(&packet->offender) && v4(&packet->offender) ? AF_INET : 0, 0, htonl(is4(&packet->offender) ? v4(&packet->offender) : 0), {0}}};
                control_put(&w, 0, 11, &e, sizeof(e));
            }
        } else receive_controls(s, packet, &w);
        if (w.truncated) message->flags |= CTRUNC;
        if (w.used && !accessible(message->control, w.used, true)) return -14;
        if (w.used && copy_to_user((void*)message->control, control, w.used)) return -14;
        message->control_size = w.used;
    }
    iov_copy(vectors, count, 0, packet->data, amount, false);
    if (errors || !(flags & PEEK)) {
        if (errors) {
            s->errors = packet->next; if (!s->errors) s->errors_tail = NULL;
            s->error = s->errors ? (int)s->errors->error.number : 0;
        } else { s->received = packet->next; if (!s->received) s->received_tail = NULL; }
        s->receive_used -= packet->charge;
        arena_free((uint64_t)packet, packet->charge); changed(s);
    }
    return !errors && (flags & TRUNCATE) ? length : amount;
}
int64_t inet_vectors(struct inet_socket* s, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    if (s->datagram) return write ? datagram_send(s, vectors, count, total, flags, NULL, NULL) : total ? datagram_read(s, vectors, count, total, flags, NULL) : 0;
    if (flags & ~(DONTWAIT | NOSIGNAL | (write ? MORE | CONFIRM : PEEK | WAITALL | 0x40000000U))) return -95;
    if (total > 0x7FFFF000) total = 0x7FFFF000;
    if (!total) return 0;
    uint64_t done = 0, deadline = deadline_after(write ? s->send_timeout : s->receive_timeout);
    for (;;) {
        tcp_conn_t* c = s->connection;
        unsigned available = c ? (write ? (unsigned)tcp_send_space(c) : c->rx_len) : 0;
        if (!write && (flags & PEEK)) available = done < available ? available - (unsigned)done : 0;
        if (write && (s->write_closed || (c && c->error))) {
            if (done) return (int64_t)done;
            int error = take_error(s); return error ? -error : pipe_error(flags);
        }
        if (available) {
            unsigned bytes = total - done < available ? (unsigned)(total - done) : available;
            if (!iov_access(vectors, count, done, bytes, !write)) return done ? (int64_t)done : -14;
            /* Copy straight between the user vectors and the connection's rings. */
            unsigned moved = 0;
            if (write) {
                while (moved < bytes) {
                    uint8_t* span; unsigned part = tcp_tx_span(c, moved, &span);
                    if (!part) panic("INET send readiness mismatch");
                    if (part > bytes - moved) part = bytes - moved;
                    iov_copy(vectors, count, done + moved, span, part, true); moved += part;
                }
                tcp_tx_commit(c, bytes);
            } else {
                unsigned base = (flags & PEEK) ? (unsigned)done : 0;
                while (moved < bytes) {
                    const uint8_t* span; unsigned part = tcp_rx_span(c, base + moved, &span);
                    if (!part) panic("INET receive readiness mismatch");
                    if (part > bytes - moved) part = bytes - moved;
                    iov_copy(vectors, count, done + moved, (uint8_t*)span, part, false); moved += part;
                }
                if (!(flags & PEEK)) tcp_rx_consume(c, bytes);
            }
            done += bytes; changed(s);
            if (done == total) return (int64_t)done;
            continue;
        }
        if (done && !write && !(flags & WAITALL)) return (int64_t)done;
        if (!c || !c->active || (!write && (c->closed || s->read_closed))) {
            if (done) return (int64_t)done;
            int error = take_error(s); if (error) return -error;
            return write ? pipe_error(flags) : c ? 0 : -107;
        }
        if (flags & DONTWAIT) return done ? (int64_t)done : -11;
        int error = task_wait_until(s, deadline);
        if (error) return done ? (int64_t)done : error == -110 ? -11 : error;
    }
}

/* ---- options ---- */
/* Linux UAPI struct tcp_info (include/uapi/linux/tcp.h), 248 bytes. */
struct linux_tcp_info {
    uint8_t state, ca_state, retransmits, probes, backoff, options, wscale, flags;
    uint32_t rto, ato, snd_mss, rcv_mss;
    uint32_t unacked, sacked, lost, retrans, fackets;
    uint32_t last_data_sent, last_ack_sent, last_data_recv, last_ack_recv;
    uint32_t pmtu, rcv_ssthresh, rtt, rttvar, snd_ssthresh, snd_cwnd, advmss, reordering;
    uint32_t rcv_rtt, rcv_space, total_retrans;
    uint64_t pacing_rate, max_pacing_rate, bytes_acked, bytes_received;
    uint32_t segs_out, segs_in, notsent_bytes, min_rtt, data_segs_in, data_segs_out;
    uint64_t delivery_rate, busy_time, rwnd_limited, sndbuf_limited;
    uint32_t delivered, delivered_ce;
    uint64_t bytes_sent, bytes_retrans;
    uint32_t dsack_dups, reord_seen, rcv_ooopack, snd_wnd, rcv_wnd, rehash;
    uint16_t total_rto, total_rto_recoveries;
    uint32_t total_rto_time;
};
_Static_assert(sizeof(struct linux_tcp_info) == 248 && __builtin_offsetof(struct linux_tcp_info, rtt) == 68 &&
               __builtin_offsetof(struct linux_tcp_info, pacing_rate) == 104 && __builtin_offsetof(struct linux_tcp_info, segs_out) == 136 &&
               __builtin_offsetof(struct linux_tcp_info, delivery_rate) == 160 && __builtin_offsetof(struct linux_tcp_info, bytes_sent) == 200 &&
               __builtin_offsetof(struct linux_tcp_info, snd_wnd) == 228 && __builtin_offsetof(struct linux_tcp_info, total_rto_time) == 244,
               "Linux tcp_info ABI");
static uint32_t elapsed_ms(uint64_t now, uint64_t then) {
    if (!then || then > now) return 0;
    uint64_t ms = (now - then) / 1000000; return ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)ms;
}
static int tcp_info_option(struct inet_socket* s, struct registers* r) {
    static const uint8_t linux_state[] = {7, 10, 2, 3, 1, 4, 5, 8, 11, 9, 6};   /* tcp_state_t -> TCP_* */
    if (!accessible(r->r8, 4, true)) return -14;
    int32_t requested; copy_from_user(&requested, (void*)r->r8, 4); if (requested < 0) return -22;
    struct linux_tcp_info info; memset(&info, 0, sizeof(info));
    tcp_conn_t* c = s->connection;
    info.state = s->listening ? 10 : 7;
    if (c && c->active) {
        uint64_t now = native_clock_ns();
        uint32_t mss = c->snd_mss ? c->snd_mss : TCP_MSS;
        info.state = c->state <= TCP_TIME_WAIT ? linux_state[c->state] : 7;
        info.retransmits = info.backoff = c->rtx_count; info.probes = c->keep_probes;
        info.options = c->wscale_ok ? 4 : 0;                 /* TCPI_OPT_WSCALE */
        info.wscale = (uint8_t)((c->snd_wscale & 15) | (c->rcv_wscale << 4));
        info.rto = c->rto * 54925;                            /* 18.2 Hz ticks -> microseconds */
        info.ato = TCP_DELACK_NS / 1000; info.snd_mss = mss; info.rcv_mss = TCP_MSS;
        info.unacked = (c->tx_unacked + mss - 1) / mss; info.retrans = c->rtx_count ? 1 : 0;
        info.last_data_sent = elapsed_ms(now, c->last_data_tx_ns);
        info.last_data_recv = elapsed_ms(now, c->last_data_rx_ns ? c->last_data_rx_ns : c->last_rx_ns);
        info.last_ack_recv = elapsed_ms(now, c->last_rx_ns);
        info.pmtu = s->family == AF_INET6 && !is4(&s->remote) ? mss + 60 : mss + 40;
        info.rcv_ssthresh = c->rcv_wnd; info.rtt = c->srtt_us; info.rttvar = c->rttvar_us;
        info.snd_ssthresh = 0x7FFFFFFF;                       /* no slow-start threshold (no congestion control) */
        info.snd_cwnd = c->snd_wnd / mss ? c->snd_wnd / mss : 1;   /* sending is bounded only by the peer window */
        info.advmss = TCP_MSS; info.reordering = 3;
        info.rcv_space = c->rx_cap; info.total_retrans = c->total_retrans;
        info.max_pacing_rate = ~0ULL;
        if (c->srtt_us) info.pacing_rate = (uint64_t)info.snd_cwnd * mss * 1000000ULL / c->srtt_us;
        info.bytes_acked = c->bytes_acked; info.bytes_received = c->bytes_received;
        info.segs_out = c->segs_out; info.segs_in = c->segs_in;
        info.notsent_bytes = c->tx_len - c->tx_unacked; info.min_rtt = c->min_rtt_us;
        info.data_segs_in = c->data_segs_in; info.data_segs_out = c->data_segs_out;
        info.delivered = c->data_segs_out > info.unacked ? c->data_segs_out - info.unacked : 0;
        info.bytes_sent = c->bytes_sent; info.bytes_retrans = c->bytes_retrans;
        info.rcv_ooopack = c->ooo_packets; info.snd_wnd = c->snd_wnd; info.rcv_wnd = c->rcv_wnd;
    }
    uint32_t amount = (uint32_t)requested < sizeof(info) ? (uint32_t)requested : sizeof(info);
    if (amount && !accessible(r->r10, amount, true)) return -14;
    if (amount) copy_to_user((void*)r->r10, &info, amount);
    return uput32(r->r8, amount);
}
static void drop_memberships(struct inet_socket* s) { mc_sock_drop_all(&s->memberships, &s->membership_count); }
static struct netdev* multicast_device(const struct ip6a* group, unsigned ifindex, uint32_t interface_address) {
    if (ifindex) return netdev_by_index(ifindex);
    if (interface_address) return netdev_v4_owner(interface_address);
    struct route_result r;
    if (!route_lookup(is4(group) ? AF_INET : AF_INET6, group, NULL, 0, 0, &r) && r.dev && r.dev != netdev_lo) return r.dev;
    return netdev_eth0 ? netdev_eth0 : netdev_lo;
}
static int membership_device(struct inet_socket* s, const struct ip6a* group, unsigned ifindex, uint32_t interface_address, struct netdev** out) {
    if (!s->datagram) return -92;   /* ENOPROTOOPT on stream sockets (Linux: EINVAL for some; glibc users test datagram) */
    if (is4(group) ? (v4(group) >> 28) != 14 : !ip6a_multicast(group)) return -22;
    struct netdev* dev = multicast_device(group, ifindex, interface_address);
    if (!dev || !dev->present) return -19;
    if (!(dev->flags & IFF_MULTICAST) && dev != netdev_lo) return -99;
    *out = dev; return 0;
}
static int membership(struct inet_socket* s, int op, const struct ip6a* group, unsigned ifindex, uint32_t interface_address,
                      const struct ip6a* source) {
    struct netdev* dev; int error = membership_device(s, group, ifindex, interface_address, &dev); if (error) return error;
    return mc_sock_change(&s->memberships, &s->membership_count, op, dev, group, source);
}
static bool storage_address(const uint8_t* storage, uint16_t want_family, struct ip6a* out) {   /* sockaddr_storage */
    uint16_t family; memcpy(&family, storage, 2);
    if (family != want_family) return false;
    if (family == AF_INET) { uint32_t a; memcpy(&a, storage + 4, 4); *out = ip6a_from4(ntohl(a)); return true; }
    if (family == AF_INET6) { memcpy(out->b, storage + 8, 16); return true; }
    return false;
}
static int group_request(struct inet_socket* s, unsigned option, const uint8_t* value, unsigned size) {
    /* struct group_req {uint32 interface; sockaddr_storage group} / group_source_req (+ source): storage at offset 8 */
    bool with_source = option == 46 || option == 47 || option == 43 || option == 44;
    if (size < (with_source ? 264u : 136u)) return -22;
    uint32_t interface; memcpy(&interface, value, 4);
    struct ip6a group, source; uint16_t family; memcpy(&family, value + 8, 2);
    if (family != AF_INET && family != AF_INET6) return -97;
    if (!storage_address(value + 8, family, &group)) return -22;
    if (with_source && !storage_address(value + 136, family, &source)) return -22;
    if (family == AF_INET6 && s->family != AF_INET6) return -97;
    int op = option == 42 ? MC_JOIN : option == 45 ? MC_LEAVE : option == 46 ? MC_JOIN_SOURCE : option == 47 ? MC_LEAVE_SOURCE :
             option == 43 ? MC_BLOCK : MC_UNBLOCK;
    return membership(s, op, &group, interface, 0, with_source ? &source : NULL);
}
/* IP_MSFILTER (struct ip_msfilter) and MCAST_MSFILTER (struct group_filter): whole filters in both directions.
 * They can exceed option()'s 264-byte buffer: copied here. mc_user_in/out are this file's only new user-memory
 * accessors (SMAP: through copy_from_user/copy_to_user, a fault after the check is -EFAULT). */
static bool mc_user_in(void* to, uint64_t from, unsigned n) { if (!accessible(from, n, false)) return false; return copy_from_user(to, (const void*)from, n) == 0; }
static bool mc_user_out(uint64_t to, const void* from, unsigned n) { if (!accessible(to, n, true)) return false; return copy_to_user((void*)to, from, n) == 0; }
#define MSF_BUFFER (144 + 128 * 64)
static int msfilter_option(struct inet_socket* s, struct registers* r, bool get, bool group_filter) {
    static uint8_t buffer[MSF_BUFFER]; static struct ip6a sources[64];
    unsigned head = group_filter ? 144 : 16, per = group_filter ? 128 : 4;
    uint32_t size;
    if (get) { if (!mc_user_in(&size, r->r8, 4)) return -14; } else size = r->r8 > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)r->r8;
    if ((int32_t)size < 0 || size < head) return -22;
    if (size > 20480) return -105;                                         /* net.core.optmem_max */
    unsigned copy = size > MSF_BUFFER ? MSF_BUFFER : size;
    if (!mc_user_in(buffer, r->r10, get ? head : copy)) return -14;
    uint32_t interface, mode, count; struct ip6a group; uint16_t family = AF_INET;
    memcpy(&interface, buffer, 4);
    if (group_filter) {
        memcpy(&family, buffer + 8, 2);
        if (family != AF_INET && family != AF_INET6) return -97;
        if (!storage_address(buffer + 8, family, &group)) return -22;
        if (family == AF_INET6 && s->family != AF_INET6) return -97;
        memcpy(&mode, buffer + 136, 4); memcpy(&count, buffer + 140, 4);
    } else {
        uint32_t g; memcpy(&g, buffer, 4); group = ip6a_from4(ntohl(g)); memcpy(&interface, buffer + 4, 4);
        memcpy(&mode, buffer + 8, 4); memcpy(&count, buffer + 12, 4);
    }
    struct netdev* dev; uint32_t ifindex = group_filter ? interface : 0, address = group_filter ? 0 : ntohl(interface);
    int error = membership_device(s, &group, ifindex, address, &dev); if (error) return error;
    if (get) {
        uint8_t got_mode; unsigned total;
        error = mc_sock_get_filter(s->memberships, dev->index, &group, &got_mode, sources, 64, &total); if (error) return error;
        unsigned copied = count < total ? count : total; if (copied > 64) copied = 64;
        if (head + per * copied > size) copied = (size - head) / per;
        mode = got_mode;
        if (group_filter) { memcpy(buffer + 136, &mode, 4); memcpy(buffer + 140, &total, 4); }
        else { memcpy(buffer + 8, &mode, 4); memcpy(buffer + 12, &total, 4); }
        for (unsigned i = 0; i < copied; i++) {
            uint8_t* slot = buffer + head + per * i;
            if (group_filter) {
                memset(slot, 0, per); memcpy(slot, &family, 2);
                if (family == AF_INET) { uint32_t a = htonl(ip6a_v4(&sources[i])); memcpy(slot + 4, &a, 4); } else memcpy(slot + 8, sources[i].b, 16);
            } else { uint32_t a = htonl(ip6a_v4(&sources[i])); memcpy(slot, &a, 4); }
        }
        uint32_t length = head + per * copied;
        if (!mc_user_out(r->r10, buffer, length) || !mc_user_out(r->r8, &length, 4)) return -14;
        return 0;
    }
    if (mode != MCAST_INCLUDE && mode != MCAST_EXCLUDE) return -22;
    if (count >= 0x3FFFFFFCU || count > (family == AF_INET ? 10u : 64u)) return -105;   /* ENOBUFS */
    if ((uint64_t)head + (uint64_t)per * count > size) return -22;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t* slot = buffer + head + per * i;
        if (group_filter) { if (!storage_address(slot, family, &sources[i])) return -22; }
        else { uint32_t a; memcpy(&a, slot, 4); sources[i] = ip6a_from4(ntohl(a)); }
    }
    return mc_sock_set_filter(&s->memberships, &s->membership_count, dev, &group, (uint8_t)mode, sources, count);
}
static int device_by_name(const uint8_t* value, unsigned size, unsigned* index) {
    char name[IFNAMSIZ + 1] = {0}; memcpy(name, value, size < IFNAMSIZ ? size : IFNAMSIZ);
    if (!name[0]) { *index = 0; return 0; }
    struct netdev* d = netdev_by_name(name); if (!d) return -19;
    *index = d->index; return 0;
}
static int option(struct inet_socket* s, struct registers* r, bool get) {
    uint64_t level = r->rsi, name = r->rdx;
    if (get && level == 6 && name == 11 && !s->datagram) return tcp_info_option(s, r);
    if ((level == 0 && (name == 41 || name == 48)) || (level == IPPROTO_IPV6 && name == 48)) {   /* IP_MSFILTER, MCAST_MSFILTER */
        if (!s->datagram) return -92;
        return msfilter_option(s, r, get, name == 48);
    }
    uint8_t value[264] = {0}; uint32_t size = 4, given = 0;
    bool timeout = level == 1 && (name == 20 || name == 21);
    bool linger = level == 1 && name == 13;
    bool bytes_value = (level == 1 && name == 25) || (level == IPPROTO_ICMPV6 && name == 1) ||
                       ((level == 0 || level == IPPROTO_IPV6) && (name == 42 || name == 43 || name == 44 || name == 45 || name == 46 || name == 47)) ||
                       (level == 0 && (name == 35 || name == 36 || name == 32 || (name >= 37 && name <= 40))) || (level == IPPROTO_IPV6 && (name == 20 || name == 21));
    if (timeout) size = 16;
    if (linger) size = 8;
    if (!get) {
        given = r->r8 > sizeof(value) ? sizeof(value) : (uint32_t)r->r8;
        if (!bytes_value && r->r8 < size && !(level == 0 && r->r8 >= 1 && (name == 1 || name == 2 || name == 33 || name == 34))) return -22;
        if (bytes_value) size = given;
        if (size && !accessible(r->r10, size, false)) return -14;
        copy_from_user(value, (void*)r->r10, size);
    } else if (!accessible(r->r8, 4, true)) return -14;
    int32_t number; memcpy(&number, value, 4);
    if (!get && level == 0 && r->r8 < 4 && (name == 1 || name == 2 || name == 33 || name == 34)) number = value[0];   /* byte-sized IP options */
    if (level == 1 && name == 25) {   /* SO_BINDTODEVICE */
        if (get) {
            struct netdev* d = s->bound_dev ? netdev_by_index(s->bound_dev) : NULL;
            unsigned length = d ? strlen(d->name) + 1 : 0, capacity = uget32(r->r8);
            if ((int32_t)capacity < 0) return -22;
            if (length > capacity) return -22;
            if (length && !accessible(r->r10, length, true)) return -14;
            if (length) copy_to_user((void*)r->r10, d->name, length);
            return uput32(r->r8, length);
        }
        if (!net_raw_capable()) return -1;
        unsigned index; int error = device_by_name(value, given, &index); if (error) return error;
        s->bound_dev = index; return 0;
    }
    if (level == 1 && name == 62) {   /* SO_BINDTOIFINDEX */
        if (get) { number = (int32_t)s->bound_dev; goto out; }
        if (!net_raw_capable()) return -1;
        if (number < 0 || (number && !netdev_by_index((unsigned)number))) return -19;
        s->bound_dev = (unsigned)number; return 0;
    }
    if (level == 0 && name == 11 && s->datagram) {   /* IP_RECVERR */
        if (get) number = s->receive_errors;
        else {
            s->receive_errors = number != 0;
            if (!s->receive_errors) {
                while (s->errors) { struct datagram* p = s->errors; s->errors = p->next; s->receive_used -= p->charge; arena_free((uint64_t)p, p->charge); }
                s->errors_tail = NULL; changed(s);
            }
        }
    } else if (level == IPPROTO_IPV6 && s->family != AF_INET6) return -92;
    else if (level == IPPROTO_IPV6) {
        bool* flag = NULL;
        switch (name) {
        case 26:   /* IPV6_V6ONLY */
            if (get) { number = s->v6only; break; }
            if (s->local_port || s->connection || s->listening) return -22;
            s->v6only = number != 0; break;
        case 16:   /* IPV6_UNICAST_HOPS */
            if (get) { number = s->unicast_hops >= 0 ? s->unicast_hops : 64; break; }
            if (number < -1 || number > 255) return -22;
            s->unicast_hops = (int16_t)number; break;
        case 18:   /* IPV6_MULTICAST_HOPS */
            if (get) { number = s->mcast_hops; break; }
            if (number < -1 || number > 255) return -22;
            s->mcast_hops = (int16_t)(number < 0 ? 1 : number); break;
        case 19: flag = &s->mc_loop6; if (!get && number != 0 && number != 1) return -22; break;   /* IPV6_MULTICAST_LOOP */
        case 17:   /* IPV6_MULTICAST_IF */
            if (get) { number = (int32_t)s->mcast_if6; break; }
            if (number && !netdev_by_index((unsigned)number)) return -19;
            if (s->bound_dev && number && (unsigned)number != s->bound_dev) return -22;
            s->mcast_if6 = (unsigned)number; break;
        case 20: case 21: {   /* IPV6_JOIN_GROUP / IPV6_LEAVE_GROUP: struct ipv6_mreq */
            if (get) return -92;
            if (given < 20) return -22;
            struct ip6a group; memcpy(group.b, value, 16); int32_t index; memcpy(&index, value + 16, 4);
            if (is4(&group)) return -22;
            return membership(s, name == 20 ? MC_JOIN : MC_LEAVE, &group, (unsigned)index, 0, NULL);
        }
        case 42: case 43: case 44: case 45: case 46: case 47: if (get) return -92; return group_request(s, (unsigned)name, value, given);
        case 29: flag = &s->mc_all6; break;           /* IPV6_MULTICAST_ALL */
        case 23:   /* IPV6_MTU_DISCOVER */
            if (get) { number = s->pmtudisc6; break; }
            if (number < 0 || number > 5) return -22;
            s->pmtudisc6 = (int8_t)number; break;
        case 24:   /* IPV6_MTU */
            if (get) {
                if (!s->remote_port && !s->connection) return -107;
                struct ip6a dst = s->remote; unsigned mtu = route_mtu(is4(&dst) ? AF_INET : AF_INET6, &dst, NULL); number = (int32_t)mtu; break;
            }
            if (number && number < 1280) return -22;
            break;
        case 25: flag = &s->receive_errors; if (!s->datagram) { if (get) { number = 0; break; } return 0; } break;   /* IPV6_RECVERR */
        case 49: flag = &s->pktinfo6; break;          /* IPV6_RECVPKTINFO */
        case 51: flag = &s->recv_hoplimit; break;     /* IPV6_RECVHOPLIMIT */
        case 66: flag = &s->recv_tclass; break;       /* IPV6_RECVTCLASS */
        case 74: flag = &s->origdst6; break;          /* IPV6_RECVORIGDSTADDR */
        case 60: flag = &s->recv_pathmtu; break;      /* IPV6_RECVPATHMTU */
        case 62: flag = &s->dontfrag6; break;         /* IPV6_DONTFRAG */
        case 78: flag = &s->freebind; break;          /* IPV6_FREEBIND */
        case 70: flag = &s->autoflowlabel; break;     /* IPV6_AUTOFLOWLABEL */
        case 75:                                      /* IPV6_TRANSPARENT */
            if (!get && !netlink_admin() && !net_raw_capable()) return -1;
            flag = &s->transparent; break;
        case 67:   /* IPV6_TCLASS */
            if (get) { number = s->tclass; break; }
            if (number < -1 || number > 255) return -22;
            s->tclass = (uint8_t)(number < 0 ? 0 : number); break;
        case 72:   /* IPV6_ADDR_PREFERENCES */
            if (get) { number = (int32_t)s->addr_prefs; break; }
            if ((number & 3) == 3 || (number & 0x30) == 0x30 || (number & 0xC) == 0xC) return -22;
            s->addr_prefs = (uint32_t)number; break;
        case 7:    /* IPV6_CHECKSUM */
            if (s->kind != KIND_RAW) return -92;
            if (get) { number = s->checksum_offset; break; }
            if (s->protocol == IPPROTO_ICMPV6) return -22;   /* always computed for ICMPv6 */
            if (number >= 0 && (number & 1)) return -22;
            s->checksum_offset = number < 0 ? -1 : number; break;
        case 36:   /* IPV6_HDRINCL */
            if (s->kind != KIND_RAW) return -92;
            flag = &s->hdrincl; break;
        case 11: case 33:   /* IPV6_FLOWINFO / IPV6_FLOWINFO_SEND */
            if (get) { number = 0; break; }
            break;
        case 76:   /* IPV6_UNICAST_IF */
            if (get) { number = 0; break; }
            break;
        case 61:   /* IPV6_PATHMTU */
            if (!get) return -92;
            { struct ip6a dst = s->remote; uint8_t info[32] = {0}; struct inet6_address a = {AF_INET6, htons(s->remote_port), 0, {0}, s->remote_scope};
              memcpy(a.address, dst.b, 16); memcpy(info, &a, 28); uint32_t mtu = route_mtu(AF_INET6, &dst, NULL); memcpy(info + 28, &mtu, 4);
              uint32_t capacity = uget32(r->r8); if (capacity > 32) capacity = 32;
              if (capacity && !accessible(r->r10, capacity, true)) return -14;
              copy_to_user((void*)r->r10, info, capacity); return uput32(r->r8, capacity); }
        default: return -92;
        }
        if (flag) { if (get) number = *flag; else *flag = number != 0; }
    } else if (level == 0) {   /* SOL_IP (AF_INET and the IPv4 side of dual-stack sockets) */
        bool* flag = NULL;
        switch (name) {
        case 1:  if (get) number = s->tos; else s->tos = (uint8_t)(number & 0xFC); break;   /* IP_TOS (ECN bits are the stack's) */
        case 2:  if (get) number = s->ttl >= 0 ? s->ttl : sysctl_ip_default_ttl; else { if (number < -1 || number == 0 || number > 255) return -22; s->ttl = (int16_t)number; } break;
        case 3:  if (s->kind != KIND_RAW) return -92; flag = &s->hdrincl; break;          /* IP_HDRINCL */
        case 4:  if (get) { return uput32(r->r8, 0); } return 0;                  /* IP_OPTIONS: none */
        case 8:  flag = &s->pktinfo4; break;                                              /* IP_PKTINFO */
        case 10: if (get) number = s->pmtudisc4; else { if (number < 0 || number > 5) return -22; s->pmtudisc4 = (int8_t)number; } break;
        case 12: flag = &s->recv_ttl; break;
        case 13: flag = &s->recv_tos; break;
        case 14: {   /* IP_MTU */
            if (!get) return -92;
            if (!s->remote_port && !s->connection) return -107;
            struct ip6a dst = s->remote; number = (int32_t)route_mtu(AF_INET, &dst, NULL); break;
        }
        case 15: flag = &s->freebind; break;
        case 19: if (!get && !netlink_admin() && !net_raw_capable()) return -1; flag = &s->transparent; break;
        case 20: flag = &s->origdst4; break;
        case 24: flag = &s->bind_no_port; break;
        case 32: {   /* IP_MULTICAST_IF: struct in_addr or ip_mreqn */
            if (get) { uint32_t a = htonl(s->mcast_addr4); memcpy(value, &a, 4); number = (int32_t)a; break; }
            if (given >= 12) { int32_t index; memcpy(&index, value + 8, 4); uint32_t a; memcpy(&a, value + 4, 4); s->mcast_if4 = (unsigned)index; s->mcast_addr4 = ntohl(a);
                               if (!index && a) { struct netdev* d = netdev_v4_owner(ntohl(a)); if (!d) return -99; s->mcast_if4 = d->index; } }
            else if (given >= 4) { uint32_t a; memcpy(&a, value, 4); s->mcast_addr4 = ntohl(a); if (a) { struct netdev* d = netdev_v4_owner(ntohl(a)); if (!d) return -99; s->mcast_if4 = d->index; } else s->mcast_if4 = 0; }
            else return -22;
            break;
        }
        case 33: if (get) number = s->mcast_ttl; else { if (number < -1 || number > 255) return -22; s->mcast_ttl = (uint8_t)(number < 0 ? 1 : number); } break;
        case 34: flag = &s->mc_loop4; break;
        case 35: case 36: {   /* IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP: ip_mreq or ip_mreqn */
            if (get) return -92;
            if (given < 8) return -22;
            uint32_t group, address; memcpy(&group, value, 4); memcpy(&address, value + 4, 4); int32_t index = 0;
            if (given >= 12) memcpy(&index, value + 8, 4);
            struct ip6a g = ip6a_from4(ntohl(group));
            return membership(s, name == 35 ? MC_JOIN : MC_LEAVE, &g, (unsigned)index, ntohl(address), NULL);
        }
        case 37: case 38: case 39: case 40: {   /* IP_UNBLOCK/BLOCK_SOURCE, IP_ADD/DROP_SOURCE_MEMBERSHIP: ip_mreq_source */
            if (get) return -92;
            if (given < 12) return -22;
            uint32_t group, address, source; memcpy(&group, value, 4); memcpy(&address, value + 4, 4); memcpy(&source, value + 8, 4);
            struct ip6a g = ip6a_from4(ntohl(group)), from = ip6a_from4(ntohl(source));
            int op = name == 39 ? MC_JOIN_SOURCE : name == 40 ? MC_LEAVE_SOURCE : name == 38 ? MC_BLOCK : MC_UNBLOCK;
            return membership(s, op, &g, 0, ntohl(address), &from);
        }
        case 42: case 43: case 44: case 45: case 46: case 47: if (get) return -92; return group_request(s, (unsigned)name, value, given);
        case 49: flag = &s->mc_all; break;   /* IP_MULTICAST_ALL */
        default: return -92;
        }
        if (flag) { if (get) number = *flag; else *flag = number != 0; }
    } else if (level == 255 && s->kind == KIND_RAW && s->family == AF_INET && s->protocol == IPPROTO_ICMP && name == 1) {   /* ICMP_FILTER */
        if (get) number = (int32_t)s->icmp_filter; else s->icmp_filter = (uint32_t)number;
    } else if (level == IPPROTO_ICMPV6 && name == 1 && s->kind == KIND_RAW && s->family == AF_INET6 && s->protocol == IPPROTO_ICMPV6) {   /* ICMP6_FILTER */
        if (get) {
            uint32_t capacity = uget32(r->r8); if ((int32_t)capacity < 0) return -22; if (capacity > 32) capacity = 32;
            if (capacity && !accessible(r->r10, capacity, true)) return -14;
            copy_to_user((void*)r->r10, s->icmp6_filter, capacity); return uput32(r->r8, capacity);
        }
        if (given > 32) given = 32;
        memset(s->icmp6_filter, 0, 32); memcpy(s->icmp6_filter, value, given); return 0;
    } else if (level == 6 && name == 1 && !s->datagram) {
        if (get) number = s->nodelay;
        else { s->nodelay = number != 0; if (s->connection) tcp_set_nodelay(s->connection, s->nodelay); changed(s); }
    } else if (level == 6 && name >= 4 && name <= 6 && !s->datagram) {
        /* TCP_KEEPIDLE / TCP_KEEPINTVL (1..32767 s) and TCP_KEEPCNT (1..127), as Linux. */
        uint32_t* target = name == 4 ? &s->keep_idle : name == 5 ? &s->keep_interval : &s->keep_count;
        if (get) number = (int32_t)*target;
        else {
            if (number < 1 || number > (name == 6 ? 127 : 32767)) return -22;
            *target = (uint32_t)number;
            if (s->connection) tcp_set_keepalive(s->connection, s->keepalive, s->keep_idle, s->keep_interval, s->keep_count);
        }
    } else if (level == 1 && name == 9 && !s->datagram) {
        if (get) number = s->keepalive;
        else {
            s->keepalive = number != 0;
            if (s->connection) tcp_set_keepalive(s->connection, s->keepalive, s->keep_idle, s->keep_interval, s->keep_count);
        }
    } else if (level == 1 && (name == 7 || name == 8) && !s->datagram) {
        /* SO_SNDBUF / SO_RCVBUF size the rings of the next connect (Linux doubles the
         * request); reads report the ring the connection has or will get. */
        uint32_t* target = name == 7 ? &s->stream_send : &s->stream_receive;
        if (get) {
            tcp_conn_t* c = s->connection;
            number = (int32_t)(c && c->active ? (name == 7 ? c->tx_cap : c->rx_cap) :
                               tcp_buffer_size(*target, name == 7 ? TCP_TX_BUF_SIZE : TCP_RX_BUF_SIZE));
        } else {
            if (number < 0) return -22;
            *target = tcp_buffer_size((uint64_t)number * 2, name == 7 ? TCP_TX_BUF_SIZE : TCP_RX_BUF_SIZE);
        }
    } else if (level == 1) {
        if (timeout) {
            uint64_t* target = name == 20 ? &s->receive_timeout : &s->send_timeout;
            int64_t time[2]; memcpy(time, value, 16);
            if (get) { time[0] = (int64_t)(*target / 1000000000); time[1] = (int64_t)((*target % 1000000000) / 1000); memcpy(value, time, 16); }
            else {
                if (time[0] < 0 || time[1] < 0 || time[1] >= 1000000) return -33;
                uint64_t micros = (uint64_t)time[1] * 1000;
                if ((uint64_t)time[0] > (~0ULL - micros) / 1000000000) return -22;
                *target = (uint64_t)time[0] * 1000000000 + micros;
            }
        } else if (s->datagram && name == 6) {
            if (get) number = s->broadcast; else s->broadcast = number != 0;
        } else if (s->datagram && (name == 7 || name == 8)) {
            unsigned* target = name == 7 ? &s->send_limit : &s->receive_limit;
            if (get) number = (int32_t)*target;
            else {
                if (number < 0) return -22;
                *target = number > 0x3FFFFFFF ? 0x7FFFFFFF : (unsigned)number * 2;
                if (*target < 4096) *target = 4096;
                changed(s);
            }
        } else if (linger && !s->datagram) {
            int32_t seconds; memcpy(&seconds, value + 4, 4);
            if (get) { number = s->reset_close; memcpy(value, &number, 4); }
            else { if (number && seconds != 0) return -95; s->reset_close = number != 0; }
        } else if (name == 2 || name == 15) {   /* SO_REUSEADDR / SO_REUSEPORT: honoured by bind() (port_used) */
            bool* target = name == 2 ? &s->reuse_address : &s->reuse_port;
            if (get) number = *target; else *target = number != 0;
        } else if (name == 36) {   /* SO_MARK */
            if (get) number = (int32_t)s->mark;
            else { if (!netlink_admin()) return -1; s->mark = (uint32_t)number; }
        } else if (name == 12) {   /* SO_PRIORITY */
            if (get) number = (int32_t)s->priority; else { if ((number < 0 || number > 6) && !netlink_admin()) return -1; s->priority = (uint32_t)number; }
        } else if (name == 29 || name == 35) {   /* SO_TIMESTAMP / SO_TIMESTAMPNS */
            bool* target = name == 29 ? &s->timestamp : &s->timestamp_ns;
            if (get) number = *target; else { *target = number != 0; if (*target) { if (name == 29) s->timestamp_ns = false; else s->timestamp = false; } }
        } else if (get) {
            switch (name) {
            case 3: number = s->kind == KIND_STREAM ? 1 : s->kind == KIND_RAW ? 3 : 2; break;
            case 4: number = s->datagram ? s->error : s->connection ? s->connection->error : 0; break;
            case 7: number = TCP_TX_BUF_SIZE; break;
            case 8: number = TCP_RX_BUF_SIZE; break;
            case 30: number = s->listening; break;   /* SO_ACCEPTCONN */
            case 38: number = s->protocol; break;    /* SO_PROTOCOL */
            case 39: number = s->family; break;      /* SO_DOMAIN */
            default: return -92;
            }
        } else return -92;
    } else return -92;
out:
    if (!get) return 0;
    if (size == 4) memcpy(value, &number, 4);
    uint32_t amount = uget32(r->r8); if ((int32_t)amount < 0) return -22;
    if (amount > size) amount = size;
    if (amount && !accessible(r->r10, amount, true)) return -14;
    if (amount) copy_to_user((void*)r->r10, value, amount);
    uput32(r->r8, amount);
    if (level == 1 && name == 4) take_error(s);
    return 0;
}
static int listen_socket(struct inet_socket* s, int64_t backlog) {
    if (s->datagram) return -95;
    if (s->connection) return -22;
    if ((uint64_t)backlog > SOMAXCONN) backlog = SOMAXCONN;   /* Linux: (unsigned) backlog, so a negative one means the maximum */
    if (s->listener) { s->listener->backlog = (uint16_t)backlog + 1; return 0; }   /* listen() again: new backlog */
    for (struct inet_socket* o = sockets; o; o = o->next)   /* SO_REUSEADDR lets sockets share a bound port; one listens */
        if (o != s && o->listening && s->local_port && o->local_port == s->local_port && addresses_overlap(o, s) &&
            !(o->reuse_port && s->reuse_port && o->owner == s->owner)) return -98;
    bool autobind = !s->local_port;
    if (autobind && !(s->local_port = unused_port(s))) return -98;
    uint32_t ip; uint8_t family;
    if (s->family == AF_INET) { ip = v4(&s->bound); family = 4; }
    else if (ip6a_any(&s->bound)) { ip = 0; family = s->v6only ? 6 : 0; }
    else if (is4(&s->bound)) { ip = v4(&s->bound); family = 4; }
    else { ip = tcp_value(&s->bound, s->bound_scope, true); family = 6; if (!ip) { if (autobind) s->local_port = 0; return -105; } }
    /* Linux allows backlog + 1 queued connections; the queue also bounds half-open ones (no SYN flood growth). */
    tcp_conn_t* c = tcp_listen_socket(ip, s->local_port, (unsigned)backlog + 1);
    if (!c) { if (autobind) s->local_port = 0; return -105; }
    tcp_set_listen_family(c, family);
    tcp_retain(c); s->listener = c; s->listening = s->bound_set = true;
    changed(s); return 0;
}
static int64_t accept_socket(struct inet_socket* s, uint64_t address, uint64_t size_address, uint64_t flags, uint64_t status) {
    if (!s->listening) return -22;
    if (flags & ~(0x800ULL | 0x80000ULL)) return -22;
    if (address && !accessible(size_address, 4, true)) return -14;
    uint64_t deadline = deadline_after(s->receive_timeout);
    tcp_conn_t* c;
    while (!(c = tcp_accept(s->listener))) {
        if (status & 0x800) return -11;
        int error = task_wait_until(s, deadline); if (error) return error == -110 ? -11 : error;
        if (!s->listening) return -22;   /* shutdown() stopped the listener meanwhile (Linux: EINVAL) */
    }
    tcp_retain(c); changed(s);
    struct ip6a remote, local; uint32_t remote_scope, local_scope;
    if (!from_tcp_value(c->remote_ip, &remote, &remote_scope) || !from_tcp_value(c->local_ip, &local, &local_scope)) {
        tcp_reset(c); tcp_release(c); tcp_collect(); return -103;
    }
    if (s->isolated && !(is4(&remote) ? (v4(&remote) >> 24) == 0x7F : ip6a_loopback(&remote))) {   /* CLONE_NEWNET: loopback only */
        tcp_reset(c); tcp_release(c); tcp_collect(); return -103;   /* ECONNABORTED, as for a connection reset before accept */
    }
    /* The wait let other threads unmap the user buffers: check them again. Nothing sleeps from here on, and a failed
     * copy drops the connection before a descriptor exists (Linux). */
    if (address) {
        uint32_t written;
        int error = accessible(size_address, 4, true) ? address_copy(s, address, uget32(size_address), &remote, remote_scope, c->remote_port, &written) : -14;
        if (error) { tcp_reset(c); tcp_release(c); tcp_collect(); return error; }
        uput32(size_address, written);
    }
    struct inet_socket* n = (void*)page_alloc(); if (!n) { tcp_reset(c); tcp_release(c); tcp_collect(); return -12; }
    memset(n, 0, sizeof(*n));
    n->family = s->family; n->kind = KIND_STREAM; n->protocol = 6; n->v6only = s->v6only;
    n->receive_limit = n->send_limit = 262144; n->keep_idle = s->keep_idle; n->keep_interval = s->keep_interval; n->keep_count = s->keep_count;
    n->connection = c; n->local = n->bound = local; n->local_scope = n->bound_scope = local_scope; n->local_port = s->local_port;
    n->remote = remote; n->remote_scope = remote_scope; n->remote_port = c->remote_port;
    n->bound_set = true; n->nodelay = s->nodelay; n->keepalive = s->keepalive; n->bound_dev = s->bound_dev; n->mark = s->mark;
    n->reuse_address = s->reuse_address; n->reuse_port = s->reuse_port; n->owner = s->owner; n->isolated = s->isolated;
    n->ttl = s->ttl; n->unicast_hops = s->unicast_hops; n->mcast_hops = s->mcast_hops; n->mcast_ttl = s->mcast_ttl; n->tos = s->tos; n->tclass = s->tclass;
    n->pmtudisc4 = s->pmtudisc4; n->pmtudisc6 = s->pmtudisc6; n->checksum_offset = -1; n->mc_loop4 = n->mc_loop6 = true;
    tcp_set_nodelay(c, n->nodelay); tcp_set_keepalive(c, n->keepalive, n->keep_idle, n->keep_interval, n->keep_count);
    int fd = fd_create_inet(n, flags); if (fd < 0) { tcp_reset(c); tcp_release(c); page_free((uint64_t)n); tcp_collect(); return fd; }
    n->next = sockets; sockets = n; n->observed_ready = inet_ready(n); n->observed_space = 0; changed(n);
    return fd;
}
int64_t inet_socket_call(struct inet_socket* s, uint64_t status, struct registers* r) {
    if (r->rax == 50) return listen_socket(s, (int64_t)(int32_t)r->rsi);
    if (r->rax == 43 || r->rax == 288) return accept_socket(s, r->rsi, r->rdx, r->rax == 288 ? r->r10 : 0, status);
    if (r->rax == 49) return bind_address(s, r->rsi, r->rdx);
    if (r->rax == 42) return connect_address(s, r->rsi, r->rdx, (status & 0x800) != 0);
    if (r->rax == 51 || r->rax == 52) return address_output(s, r->rsi, r->rdx, r->rax == 52);
    if (r->rax == 54 || r->rax == 55) return option(s, r, r->rax == 55);
    if (r->rax == 48) {
        if (r->rsi > 2) return -22;
        if (s->datagram) {
            if (r->rsi != 1) s->read_closed = true;
            if (r->rsi != 0) s->write_closed = true;
            changed(s); return s->remote_port || (s->kind == KIND_RAW && !wildcard(&s->remote)) ? 0 : -107;
        }
        if (s->listening) {   /* Linux: SHUT_RD / SHUT_RDWR stop a listener and blocked accept() calls fail (EINVAL) */
            if (r->rsi != 1) { stop_listening(s); task_wake_channel(s); changed(s); }
            return 0;
        }
        if (!s->connection || !s->connection->connected) return -107;
        if (r->rsi != 1) { s->read_closed = true; tcp_shutdown_read(s->connection); }
        if (r->rsi != 0) { s->write_closed = true; tcp_close(s->connection); }
        changed(s); return 0;
    }
    if (r->rax != 44 && r->rax != 45 && r->rax != 46 && r->rax != 47) return -95;
    bool write = r->rax == 44 || r->rax == 46;
    uint64_t flags = r->rax >= 46 ? r->rdx : r->r10;
    if (status & 0x800) flags |= DONTWAIT;
    if (r->rax == 44 || r->rax == 45) {
        if (s->datagram) {
            struct endpoint address; struct iovec64 vector = {r->rsi, r->rdx};
            if (write) {
                if (r->r8) { int error = address_load(s, r->r8, r->r9, &address, true); if (error) return error; }
                return datagram_send(s, &vector, 1, r->rdx, flags, r->r8 ? &address : NULL, NULL);
            }
            struct message64 message = {.name = r->r8};
            if (r->r8) {
                if (!accessible(r->r9, 4, true)) return -14;
                message.name_size = uget32(r->r9); if ((int32_t)message.name_size < 0) return -22;
            }
            int64_t result = datagram_read(s, &vector, 1, r->rdx, flags, &message);
            if (result >= 0 && r->r8) {
                if (!accessible(r->r9, 4, true)) return -14;
                uput32(r->r9, message.name_size);
            }
            return result;
        }
        if (write && r->r8) {
            struct endpoint address; int error = address_load(s, r->r8, r->r9, &address, true); if (error) return error;
        }
        struct iovec64 vector = {r->rsi, r->rdx};
        int64_t result = inet_vectors(s, &vector, 1, r->rdx, write, flags);
        if (!write && result >= 0 && r->r8) {
            if (!accessible(r->r9, 4, true)) return -14;
            uput32(r->r9, 0);
        }
        return result;
    }
    if (!accessible(r->rsi, sizeof(struct message64), !write)) return -14;
    struct message64 message; copy_from_user(&message, (void*)r->rsi, sizeof(message));
    if (message.count > 1024) return -22;
    struct endpoint address; struct send_control control;
    if (write) {
        int error = parse_control(s, message.control, message.control_size, &control); if (error) return error;
        if (message.name && s->datagram) { error = address_load(s, message.name, message.name_size, &address, true); if (error) return error; }
    }
    struct iovec64 vectors[1024]; uint64_t total = 0;
    if (message.count && !accessible(message.vectors, message.count * sizeof(*vectors), false)) return -14;
    if (message.count && copy_from_user(vectors, (void*)message.vectors, message.count * sizeof(*vectors))) return -14;
    for (unsigned i = 0; i < message.count; i++) {
        if (vectors[i].size > 0x7FFFFFFFFFFFFFFFULL - total) return -22;
        total += vectors[i].size;
    }
    int64_t result = s->datagram ? (write ? datagram_send(s, vectors, (unsigned)message.count, total, flags, message.name ? &address : NULL, &control) :
        datagram_read(s, vectors, (unsigned)message.count, total, flags, &message)) : inet_vectors(s, vectors, (unsigned)message.count, total, write, flags);
    if (!write && result >= 0) {
        if (!accessible(r->rsi, sizeof(message), true)) return -14;
        if (!s->datagram) message.name_size = message.flags = 0, message.control_size = 0;
        copy_to_user((void*)r->rsi, &message, sizeof(message));
    }
    return result;
}
int netif_ioctl_family(int family, uint64_t command, uint64_t argument);
int64_t inet_ioctl(struct inet_socket* s, uint64_t command, uint64_t argument) {
    if (command == 0x5411) {   /* SIOCOUTQ: bytes not yet acknowledged (TCP) or queued (datagrams) */
        if (!accessible(argument, 4, true)) return -14;
        int value = s->datagram ? (int)(s->transmit ? s->transmit->length : 0) : s->connection ? (int)s->connection->tx_len : 0;
        return uput32(argument, (uint32_t)value);
    }
    return netif_ioctl_family(s->family, command, argument);
}
int64_t inet_call(struct registers* r) {
    if (r->rax == 41) return create(r->rdi, r->rsi, r->rdx);
    struct open_file* f = fd_lookup(r->rdi); if (!f) return -9;
    uint64_t flags; struct inet_socket* s = fd_inet(f, &flags); if (!s) return -88;
    fd_pin(f); int64_t result = inet_socket_call(s, flags, r); fd_unpin(f); return result;
}

/* ---- /proc/net/{tcp,tcp6,udp,udp6,raw,raw6,icmp,icmp6} ---- */
static void hex_put(char* out, unsigned* n, unsigned capacity, uint32_t value, unsigned digits) {
    for (unsigned i = 0; i < digits && *n + 1 < capacity; i++) out[(*n)++] = "0123456789ABCDEF"[(value >> ((digits - 1 - i) * 4)) & 15];
}
static void text_put(char* out, unsigned* n, unsigned capacity, const char* text) { while (*text && *n + 1 < capacity) out[(*n)++] = *text++; }
static void address_put(char* out, unsigned* n, unsigned capacity, const struct ip6a* a, bool v6, uint16_t port) {
    if (!v6) { uint32_t x = v4(a); hex_put(out, n, capacity, __builtin_bswap32(x), 8); }
    else for (unsigned w = 0; w < 4; w++) { uint32_t x; memcpy(&x, a->b + 4 * w, 4); hex_put(out, n, capacity, x, 8); }   /* host-order words, as Linux */
    text_put(out, n, capacity, ":"); hex_put(out, n, capacity, port, 4);
}
unsigned inet_proc_lines(int family, int kind, char* out, unsigned capacity) {
    unsigned n = 0, slot = 0; bool v6 = family == AF_INET6;
    for (struct inet_socket* s = sockets; s && n + 200 < capacity; s = s->next) {
        if (s->orphan || s->kind != kind || (kind != KIND_STREAM && s->family != family)) continue;
        if (kind == KIND_STREAM && (v6 ? s->family != AF_INET6 : s->family != AF_INET)) continue;
        char number[16]; uint_to_str(slot++, number);
        unsigned pad = strlen(number); while (pad++ < 4) text_put(out, &n, capacity, " ");
        text_put(out, &n, capacity, number); text_put(out, &n, capacity, ": ");
        address_put(out, &n, capacity, &s->local, v6, s->kind == KIND_RAW ? s->protocol : s->local_port); text_put(out, &n, capacity, " ");
        address_put(out, &n, capacity, &s->remote, v6, s->remote_port); text_put(out, &n, capacity, " ");
        uint32_t state = s->listening ? 10 : s->datagram ? (s->remote_port ? 1 : 7) : s->connection && s->connection->active ? (uint32_t)(s->connection->state == TCP_ESTABLISHED ? 1 : s->connection->state == TCP_SYN_SENT ? 2 : s->connection->state == TCP_CLOSE_WAIT ? 8 : 6) : 7;
        hex_put(out, &n, capacity, state, 2); text_put(out, &n, capacity, " ");
        uint32_t tx = s->datagram ? s->send_used : s->connection ? s->connection->tx_len : 0, rx = s->datagram ? s->receive_used : s->connection ? s->connection->rx_len : 0;
        hex_put(out, &n, capacity, tx, 8); text_put(out, &n, capacity, ":"); hex_put(out, &n, capacity, rx, 8);
        text_put(out, &n, capacity, " 00:00000000 00000000 ");
        char uid[12]; uint_to_str(credentials_visible_id(s->owner, false), uid); pad = strlen(uid); while (pad++ < 5) text_put(out, &n, capacity, " ");
        text_put(out, &n, capacity, uid); text_put(out, &n, capacity, "        0 0 2 0000000000000000\n");
    }
    out[n] = 0; return n;
}
bool inet_socket_tcp(struct inet_socket* s, int* family) { *family = s->family; return s->kind == KIND_STREAM; }   /* landlock.c */
