/* AF_NETLINK sockets.
 *  - NETLINK_ROUTE (rtnetlink): links (RTM_GET/NEW/DEL/SETLINK, creation of tun, wireguard and dummy devices by
 *    IFLA_INFO_KIND), addresses of both families (RTM_GET/NEW/DELADDR with lifetimes and IFA_F_* flags), routes of
 *    every table (RTM_GET/NEW/DELROUTE, single "ip route get" lookups with RTM_F_FIB_MATCH), neighbours
 *    (RTM_GET/NEW/DELNEIGH, kernel/arp.c entries included), policy rules (RTM_GET/NEW/DELRULE), netconf, and the
 *    notifications of each change to the RTNLGRP_* multicast groups (link, IPv4/IPv6 addresses, routes, rules,
 *    neighbours, IPv6 prefixes, ND user options). Changes need CAP_NET_ADMIN.
 *  - NETLINK_GENERIC: the nlctrl family directory and families registered by other modules (wireguard.c).
 *  - NETLINK_NETFILTER: nf_tables (nft.c) with batches.
 *  - NETLINK_KOBJECT_UEVENT: udev monitors bind and receive the kernel uevents of power.c (netlink_uevent).
 * Replies are packed into datagrams of up to 4 KiB (dumps in NLM_F_MULTI parts ending with NLMSG_DONE); a socket's
 * queue is bounded by SO_RCVBUF for notifications (an overrun reports ENOBUFS once, as Linux). */
#include "nlmsg.h"
#include "../../kernel/ip.h"
#include "../../kernel/arp.h"
#include "../../kernel/string.h"

#define AF_NETLINK 16
#define NETLINK_ROUTE 0
#define NETLINK_SOCK_DIAG 4
#define NETLINK_NETFILTER 12
#define NETLINK_KOBJECT_UEVENT 15
#define NETLINK_GENERIC 16
#define DATAGRAM_BYTES 4096U
#define DUMP_LIMIT (4U * 1024U * 1024U)
enum { RTM_NEWLINK = 16, RTM_DELLINK, RTM_GETLINK, RTM_SETLINK, RTM_NEWADDR, RTM_DELADDR, RTM_GETADDR, RTM_NEWROUTE = 24, RTM_DELROUTE,
       RTM_GETROUTE, RTM_NEWNEIGH = 28, RTM_DELNEIGH, RTM_GETNEIGH, RTM_NEWRULE = 32, RTM_DELRULE, RTM_GETRULE, RTM_GETQDISC = 38,
       RTM_NEWPREFIX = 52, RTM_GETMULTICAST = 58, RTM_GETANYCAST = 62, RTM_NEWNDUSEROPT = 68, RTM_NEWNETCONF = 80, RTM_GETNETCONF = 82,
       RTM_GETADDRLABEL = 74, RTM_GETNSID = 90, RTM_GETSTATS = 94, RTM_GETCHAIN = 102, RTM_GETLINKPROP = 110, RTM_GETNEXTHOP = 106 };
struct nl_datagram { struct nl_datagram* next; unsigned size, charge; uint8_t data[]; };
struct netlink_socket {
    uint64_t id; int protocol; uint32_t portid; uint64_t groups; bool bound;
    struct nl_datagram *head, *tail; unsigned queued, count;
    uint32_t receive_buffer, send_buffer;
    bool pktinfo, cap_ack, ext_ack, strict, overrun, no_enobufs, listen_all, dumping, dump_first;
    bool passcred;                           /* SO_PASSCRED: every message carries the kernel's SCM_CREDENTIALS */
    bool batch_active; uint32_t batch_seq;
    struct netlink_socket* next;
};
struct sockaddr_nl { uint16_t family, pad; uint32_t pid, groups; };
static const struct object_operations netlink_operations;
static uint64_t next_id;
static uint32_t next_portid = 0xFFFFF000U;
static struct netlink_socket* sockets;   /* open sockets (RLIMIT_NOFILE bounds them): port ids are unique among them */

/* CAP_NET_ADMIN, or the desktop owner: uid 1000 in the initial user and PID namespaces (the local session user, as
 * logind/polkit let the active seat's user configure the network through a network manager - Aurora's Settings
 * configures it through these same interfaces). Sandboxed processes (other namespaces) never qualify. */
/* NexusOS has no root login: the desktop owner (uid 1000 in the initial user and PID namespaces) administers the
 * network, as acpi.c lets it reboot. It holds CAP_NET_ADMIN for configuration and, unless
 * /proc/sys/net/nexus/owner_raw is 0, CAP_NET_RAW for raw and packet sockets (ping, tcpdump, DHCP clients).
 * Processes in any other user or PID namespace (sandboxes, containers) and other users need the real capability in
 * the initial user namespace. */
static bool desktop_owner(struct credentials* who) {
    return task_pid_namespace() == pid_namespace_root() && !namespace_id(credentials_namespace(who)) && credentials_kernel_id(who, false, 1) == 1000;
}
/* CLONE_NEWNET (namespaces.c): an isolated network namespace has only loopback; every interface, route, firewall,
 * tunnel, sysctl, raw and packet socket belongs to the single initial stack, so nothing there qualifies. */
bool netlink_admin(void) {
    if (task_net_namespace()) return false;
    struct credentials* who = credentials_current();
    return credentials_capable(who, pid_namespace_root()->owner, 12) || desktop_owner(who);
}
bool net_raw_capable(void) {
    if (task_net_namespace()) return false;
    struct credentials* who = credentials_current();
    return credentials_capable(who, pid_namespace_root()->owner, 13) || (sysctl_owner_raw && desktop_owner(who));
}
static bool portid_used(uint32_t portid, int protocol) {
    for (struct netlink_socket* nl = sockets; nl; nl = nl->next) if (nl->bound && nl->protocol == protocol && nl->portid == portid) return true;
    return false;
}
static void autobind(struct netlink_socket* nl) {   /* the process id, else a free port id counting down from -4096 */
    if (nl->bound) return;
    uint32_t want = task_pid();
    if (portid_used(want, nl->protocol)) { while (portid_used(next_portid, nl->protocol)) next_portid--; want = next_portid--; }
    nl->portid = want; nl->bound = true;
}
uint32_t nl_portid(struct netlink_socket* nl) { return nl->portid; }
void nl_dump_begin(struct netlink_socket* nl) { nl->dumping = true; nl->dump_first = true; }   /* multi-part reply packing */
void nl_dump_end(struct netlink_socket* nl) { nl->dumping = false; }

/* ---- builder ---- */
void nl_put(struct nl_builder* b, const void* data, unsigned size) {
    unsigned padded = (size + 3) & ~3U;
    if (b->used + padded > sizeof(b->buffer)) { b->overflow = true; return; }
    if (size) memcpy(b->buffer + b->used, data, size);
    memset(b->buffer + b->used + size, 0, padded - size); b->used += padded;
}
void nl_begin(struct nl_builder* b, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
    b->used = 0; b->overflow = false; b->depth = 0;
    struct nlmsghdr h = {0, type, flags, seq, pid}; nl_put(b, &h, sizeof(h));
}
void nl_attr(struct nl_builder* b, uint16_t type, const void* data, unsigned size) {
    if (size > 0xFFFF - 4) { b->overflow = true; return; }
    uint16_t header[2] = {(uint16_t)(4 + size), type}; nl_put(b, header, 4);
    if (size) nl_put(b, data, size);
}
void nl_attr8(struct nl_builder* b, uint16_t type, uint8_t value) { nl_attr(b, type, &value, 1); }
void nl_attr16(struct nl_builder* b, uint16_t type, uint16_t value) { nl_attr(b, type, &value, 2); }
void nl_attr32(struct nl_builder* b, uint16_t type, uint32_t value) { nl_attr(b, type, &value, 4); }
void nl_attr64(struct nl_builder* b, uint16_t type, uint64_t value) { nl_attr(b, type, &value, 8); }
void nl_attr_be32(struct nl_builder* b, uint16_t type, uint32_t value) { uint32_t v = __builtin_bswap32(value); nl_attr(b, type, &v, 4); }
void nl_attr_be64(struct nl_builder* b, uint16_t type, uint64_t value) { uint64_t v = __builtin_bswap64(value); nl_attr(b, type, &v, 8); }
void nl_attr_string(struct nl_builder* b, uint16_t type, const char* text) { nl_attr(b, type, text, strlen(text) + 1); }
void nl_nest_begin(struct nl_builder* b, uint16_t type) {
    if (b->depth >= 8) { b->overflow = true; return; }
    b->nest[b->depth++] = b->used;
    uint16_t header[2] = {4, (uint16_t)(type | NLA_F_NESTED)}; nl_put(b, header, 4);
}
void nl_nest_end(struct nl_builder* b) {
    if (!b->depth) return;
    unsigned start = b->nest[--b->depth]; uint16_t length = (uint16_t)(b->used - start);
    if (!b->overflow) memcpy(b->buffer + start, &length, 2);
}
void nl_finish(struct nl_builder* b) { uint32_t length = b->used; memcpy(b->buffer, &length, 4); }
bool nl_parse(const uint8_t* data, unsigned length, struct nl_attr_view* table, unsigned max) {
    memset(table, 0, sizeof(*table) * (max + 1));
    for (unsigned at = 0; at + 4 <= length;) {
        uint16_t size, type; memcpy(&size, data + at, 2); memcpy(&type, data + at + 2, 2);
        if (size < 4 || size > length - at) return false;
        type &= NLA_TYPE_MASK;
        if (type <= max) { table[type].data = data + at + 4; table[type].length = (uint16_t)(size - 4); table[type].present = true; }
        at += (size + 3u) & ~3u;
    }
    return true;
}
uint32_t nl_u32(const struct nl_attr_view* a) { uint32_t v = 0; if (a->present && a->length >= 4) memcpy(&v, a->data, 4); return v; }
uint16_t nl_u16(const struct nl_attr_view* a) { uint16_t v = 0; if (a->present && a->length >= 2) memcpy(&v, a->data, 2); return v; }
uint8_t nl_u8(const struct nl_attr_view* a) { return a->present && a->length >= 1 ? a->data[0] : 0; }
uint32_t nl_be32(const struct nl_attr_view* a) { return __builtin_bswap32(nl_u32(a)); }
bool nl_string(const struct nl_attr_view* a, char* out, unsigned capacity) {
    if (!a->present || !capacity) return false;
    unsigned n = 0; while (n < a->length && a->data[n] && n + 1 < capacity) { out[n] = (char)a->data[n]; n++; }
    out[n] = 0; return n < a->length || n + 1 < capacity;
}

/* ---- receive queue ---- */
static void wake(struct netlink_socket* nl) { task_wake_channel(nl); event_source_changed(nl, 1); }
static bool queue_bytes(struct netlink_socket* nl, const uint8_t* data, unsigned size, bool new_datagram, bool reply) {
    if (!size) return true;
    if (!new_datagram && nl->tail && nl->tail->size + size <= DATAGRAM_BYTES) {
        memcpy(nl->tail->data + nl->tail->size, data, size); nl->tail->size += size; nl->queued += size; return true;
    }
    unsigned limit = reply ? DUMP_LIMIT : nl->receive_buffer;
    if (nl->queued + size > limit) { if (!reply && !nl->no_enobufs) nl->overrun = true; return false; }
    unsigned capacity = size > DATAGRAM_BYTES ? size : DATAGRAM_BYTES, charge = (unsigned)sizeof(struct nl_datagram) + capacity;
    struct nl_datagram* d = (void*)arena_alloc(charge); if (!d) { if (!reply) nl->overrun = true; return false; }
    d->next = NULL; d->size = size; d->charge = charge; memcpy(d->data, data, size);
    if (nl->tail) nl->tail->next = d; else nl->head = d;
    nl->tail = d; nl->count++; nl->queued += size;
    return true;
}
void nl_send(struct netlink_socket* nl, struct nl_builder* b) {
    if (b->overflow) return;
    nl_finish(b); queue_bytes(nl, b->buffer, b->used, !nl->dumping || nl->dump_first, true);   /* dump parts share datagrams */
    nl->dump_first = false;
}
static void send_raw(struct netlink_socket* nl, struct nl_builder* b) { nl_send(nl, b); }
void nl_send_error(struct netlink_socket* nl, const struct nlmsghdr* request, int error) {
    struct nl_builder* b = (void*)arena_alloc(sizeof(struct nl_builder)); if (!b) return;
    /* the request header, and for errors its payload too unless NETLINK_CAP_ACK (Linux netlink_ack: NLM_F_CAPPED) */
    unsigned echo = error && !nl->cap_ack && request->len <= 2048 ? request->len : sizeof(*request);
    nl_begin(b, NLMSG_ERROR, echo == sizeof(*request) ? 0x100 : 0, request->seq, nl->portid);
    int32_t value = error; nl_put(b, &value, 4);
    nl_put(b, request, echo);
    bool saved = nl->dumping; nl->dumping = false; nl_send(nl, b); nl->dumping = saved;
    arena_free((uint64_t)b, sizeof(struct nl_builder));
}
void nl_send_done(struct netlink_socket* nl, const struct nlmsghdr* request) {
    struct { struct nlmsghdr h; int32_t value; } done = {{20, NLMSG_DONE, NLM_F_MULTI, request->seq, nl->portid}, 0};
    queue_bytes(nl, (const uint8_t*)&done, sizeof(done), nl->dump_first, true);
    nl->dump_first = false;
}
void nl_multicast(int protocol, unsigned group, struct nl_builder* b, struct netlink_socket* except) {
    if (b->overflow || !group || group > 64) return;
    nl_finish(b);
    for (struct netlink_socket* nl = sockets; nl; nl = nl->next) {
        if (nl->protocol != protocol || nl == except || !(nl->groups & (1ULL << (group - 1)))) continue;
        if (queue_bytes(nl, b->buffer, b->used, true, false) || nl->overrun) wake(nl);
    }
}
void nl_unicast(struct netlink_socket* nl, struct nl_builder* b) {
    if (b->overflow) return;
    nl_finish(b);
    if (queue_bytes(nl, b->buffer, b->used, true, false) || nl->overrun) wake(nl);
}
static bool has_listeners(int protocol, unsigned group) {
    for (struct netlink_socket* nl = sockets; nl; nl = nl->next) if (nl->protocol == protocol && (nl->groups & (1ULL << (group - 1)))) return true;
    return false;
}

/* ---- message bodies ---- */
static uint32_t clock_t_since(uint64_t ns) { uint64_t now = native_clock_ns(); return ns && now > ns ? (uint32_t)((now - ns) / 10000000ULL) : 0; }
static void link_fill(struct nl_builder* b, struct netdev* d, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
    nl_begin(b, type, flags, seq, pid);
    struct { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; } info = {0, 0, d->type, (int32_t)d->index, netdev_flags(d), type == RTM_DELLINK ? 0xFFFFFFFFU : 0};
    nl_put(b, &info, sizeof(info));
    nl_attr_string(b, 3, d->name);                                                   /* IFLA_IFNAME */
    nl_attr32(b, 13, d->txqlen);                                                      /* IFLA_TXQLEN */
    nl_attr8(b, 16, d->operstate);                                                    /* IFLA_OPERSTATE */
    nl_attr8(b, 17, 0);                                                               /* IFLA_LINKMODE */
    nl_attr32(b, 4, d->mtu);                                                          /* IFLA_MTU */
    nl_attr32(b, 50, d->min_mtu); nl_attr32(b, 51, d->max_mtu);                       /* IFLA_MIN_MTU, IFLA_MAX_MTU */
    nl_attr32(b, 27, d->group);                                                       /* IFLA_GROUP */
    nl_attr32(b, 30, d->promiscuity);                                                 /* IFLA_PROMISCUITY */
    nl_attr32(b, 31, 1); nl_attr32(b, 32, 1);                                         /* IFLA_NUM_TX/RX_QUEUES */
    nl_attr8(b, 33, d->carrier ? 1 : 0);                                              /* IFLA_CARRIER */
    nl_attr32(b, 35, (uint32_t)d->carrier_changes);                                   /* IFLA_CARRIER_CHANGES */
    nl_attr32(b, 47, (uint32_t)d->carrier_up_count); nl_attr32(b, 48, (uint32_t)d->carrier_down_count);
    nl_attr8(b, 39, 0);                                                               /* IFLA_PROTO_DOWN */
    nl_attr_string(b, 6, d->type == ARPHRD_LOOPBACK ? "noqueue" : "fq_codel");        /* IFLA_QDISC */
    if (d->alias[0]) nl_attr_string(b, 20, d->alias);                                 /* IFLA_IFALIAS */
    if (d->addr_len) {
        nl_attr(b, 1, d->addr, d->addr_len); nl_attr(b, 2, d->broadcast, d->addr_len);   /* IFLA_ADDRESS, IFLA_BROADCAST */
        if (d->type == ARPHRD_ETHER) nl_attr(b, 54, d->perm_addr, 6);                     /* IFLA_PERM_ADDRESS */
    }
    struct netdev_stats s = d->stats;
    if (d == netdev_eth0 && d->hw) {
        s.rx_packets = d->hw->stats.rx_packets; s.tx_packets = d->hw->stats.tx_packets; s.rx_bytes = d->hw->stats.rx_bytes; s.tx_bytes = d->hw->stats.tx_bytes;
        s.rx_errors = d->hw->stats.rx_errors; s.tx_errors = d->hw->stats.tx_errors; s.rx_dropped = d->hw->stats.rx_dropped;
    }
    uint32_t stats[24] = {(uint32_t)s.rx_packets, (uint32_t)s.tx_packets, (uint32_t)s.rx_bytes, (uint32_t)s.tx_bytes, (uint32_t)s.rx_errors,
                          (uint32_t)s.tx_errors, (uint32_t)s.rx_dropped, (uint32_t)s.tx_dropped, (uint32_t)s.multicast, (uint32_t)s.collisions};
    nl_attr(b, 7, stats, sizeof(stats));                                              /* IFLA_STATS */
    uint64_t stats64[24] = {s.rx_packets, s.tx_packets, s.rx_bytes, s.tx_bytes, s.rx_errors, s.tx_errors, s.rx_dropped, s.tx_dropped, s.multicast, s.collisions};
    stats64[23] = s.rx_nohandler;
    nl_attr(b, 23, stats64, sizeof(stats64));                                         /* IFLA_STATS64 */
    if (d->kind[0]) { nl_nest_begin(b, 18); nl_attr_string(b, 1, d->kind); nl_nest_end(b); }   /* IFLA_LINKINFO / IFLA_INFO_KIND */
    nl_nest_begin(b, 26);                                                             /* IFLA_AF_SPEC */
    nl_nest_begin(b, AF_INET); { int32_t conf[33] = {0}; conf[0] = d->ipv4_forwarding; conf[3] = 1; conf[6] = 1; conf[8] = d->rp_filter;
        nl_attr(b, 1, conf, sizeof(conf)); } nl_nest_end(b);                         /* IFLA_INET_CONF */
    nl_nest_begin(b, AF_INET6);
    uint32_t inet6_flags = d->ipv6_ready ? 0x80000000U : 0; if (d->ra_managed) inet6_flags |= 0x40; if (d->ra_other) inet6_flags |= 0x80;
    nl_attr32(b, 1, inet6_flags);                                                     /* IFLA_INET6_FLAGS */
    { int32_t conf[60] = {0}; const struct ipv6_conf* c = &d->conf6;   /* DEVCONF_* order (include/uapi/linux/ipv6.h) */
      conf[0] = c->forwarding; conf[1] = c->hop_limit; conf[2] = (int32_t)d->mtu6; conf[3] = c->accept_ra; conf[4] = c->accept_redirects; conf[5] = c->autoconf;
      conf[6] = c->dad_transmits; conf[7] = c->router_solicitations; conf[8] = c->router_solicitation_interval * 1000; conf[9] = c->router_solicitation_delay * 1000;
      conf[10] = c->use_tempaddr; conf[11] = c->temp_valid_lft; conf[12] = c->temp_prefered_lft; conf[13] = c->regen_max_retry; conf[14] = c->max_desync_factor;
      conf[15] = c->max_addresses; conf[16] = c->force_mld_version; conf[17] = c->accept_ra_defrtr; conf[18] = c->accept_ra_pinfo;
      conf[22] = c->accept_source_route; conf[25] = c->proxy_ndp; conf[26] = c->optimistic_dad; conf[27] = c->accept_dad; conf[30] = c->disable_ipv6;
      nl_attr(b, 2, conf, sizeof(conf)); }                                            /* IFLA_INET6_CONF */
    { struct { uint32_t max_reasm_len, tstamp, reachable_time, retrans_time; } cache = {65535, clock_t_since(d->created_ns), d->reachable_time_ms, d->retrans_ms};
      nl_attr(b, 5, &cache, sizeof(cache)); }                                         /* IFLA_INET6_CACHEINFO */
    { struct ip6a token = {{0}}; nl_attr(b, 7, &token, 16); }                         /* IFLA_INET6_TOKEN */
    nl_attr8(b, 8, (uint8_t)d->conf6.addr_gen_mode);                                  /* IFLA_INET6_ADDR_GEN_MODE */
    nl_nest_end(b); nl_nest_end(b);
}
static void addr4_fill(struct nl_builder* b, struct netdev* d, const struct netdev_v4* a, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
    nl_begin(b, type, flags, seq, pid);
    struct { uint8_t family, prefix, flags, scope; uint32_t index; } info = {AF_INET, a->plen, (uint8_t)(a->flags | (a->secondary ? 1 : 0)), a->scope, d->index};
    nl_put(b, &info, sizeof(info));
    nl_attr_be32(b, 1, a->peer ? a->peer : a->address); nl_attr_be32(b, 2, a->address);   /* IFA_ADDRESS, IFA_LOCAL */
    if (a->broadcast) nl_attr_be32(b, 4, a->broadcast);                                   /* IFA_BROADCAST */
    nl_attr_string(b, 3, a->label);                                                       /* IFA_LABEL */
    nl_attr32(b, 8, a->flags | (a->secondary ? 1 : 0));                                   /* IFA_FLAGS */
    uint64_t now = native_clock_ns(); uint32_t left = 0xFFFFFFFFU;
    if (a->expires_ns) left = a->expires_ns > now ? (uint32_t)((a->expires_ns - now) / 1000000000ULL) : 0;
    uint32_t cache[4] = {left, left, 0, 0}; nl_attr(b, 6, cache, sizeof(cache));          /* IFA_CACHEINFO: preferred, valid */
}
static void addr6_fill(struct nl_builder* b, struct netdev* d, const struct inet6_ifaddr* a, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
    nl_begin(b, type, flags, seq, pid);
    struct { uint8_t family, prefix, flags, scope; uint32_t index; } info = {AF_INET6, a->plen, (uint8_t)a->flags, a->scope, d->index};
    nl_put(b, &info, sizeof(info));
    if (!ip6a_any(&a->peer)) { nl_attr(b, 2, a->addr.b, 16); nl_attr(b, 1, a->peer.b, 16); }   /* IFA_LOCAL + IFA_ADDRESS (peer) */
    else nl_attr(b, 1, a->addr.b, 16);
    uint64_t now = native_clock_ns(), elapsed = (now - a->stamp_ns) / 1000000000ULL;
    uint32_t preferred = a->prefered_lft == LIFETIME_INFINITE ? LIFETIME_INFINITE : elapsed >= a->prefered_lft ? 0 : a->prefered_lft - (uint32_t)elapsed;
    uint32_t valid = a->valid_lft == LIFETIME_INFINITE ? LIFETIME_INFINITE : elapsed >= a->valid_lft ? 0 : a->valid_lft - (uint32_t)elapsed;
    struct { uint32_t preferred, valid, created, updated; } cache = {preferred, valid, (uint32_t)(a->created_ns / 10000000ULL), (uint32_t)(a->stamp_ns / 10000000ULL)};
    nl_attr(b, 6, &cache, sizeof(cache));                                                 /* IFA_CACHEINFO */
    nl_attr32(b, 8, a->flags);                                                            /* IFA_FLAGS */
    if (a->route_metric) nl_attr32(b, 9, a->route_metric);                                /* IFA_RT_PRIORITY */
}
static void route_fill(struct nl_builder* b, const struct route* r, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid, uint32_t rtm_flags) {
    nl_begin(b, type, flags, seq, pid);
    bool v6 = r->family == AF_INET6;
    struct { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; } info =
        {r->family, r->plen, r->src_len, 0, (uint8_t)(r->table < 256 ? r->table : 252), r->protocol, r->scope, r->type, rtm_flags};
    nl_put(b, &info, sizeof(info));
    nl_attr32(b, 15, r->table);                                                           /* RTA_TABLE */
    if (r->plen) { if (v6) nl_attr(b, 1, r->dst.b, 16); else nl_attr_be32(b, 1, ip6a_v4(&r->dst)); }   /* RTA_DST */
    if (r->src_len) { if (v6) nl_attr(b, 2, r->src.b, 16); else nl_attr_be32(b, 2, ip6a_v4(&r->src)); }
    if (v6 ? !ip6a_any(&r->gateway) : ip6a_v4(&r->gateway) != 0) { if (v6) nl_attr(b, 5, r->gateway.b, 16); else nl_attr_be32(b, 5, ip6a_v4(&r->gateway)); }
    if (v6 ? !ip6a_any(&r->prefsrc) : ip6a_is4(&r->prefsrc) && ip6a_v4(&r->prefsrc)) { if (v6) nl_attr(b, 7, r->prefsrc.b, 16); else nl_attr_be32(b, 7, ip6a_v4(&r->prefsrc)); }
    if (r->metric || v6) nl_attr32(b, 6, r->metric);                                      /* RTA_PRIORITY */
    if (r->oif) nl_attr32(b, 4, r->oif);                                                  /* RTA_OIF */
    if (r->mtu || r->hoplimit || r->advmss) {
        nl_nest_begin(b, 8);                                                              /* RTA_METRICS */
        if (r->mtu) nl_attr32(b, 2, r->mtu);
        if (r->advmss) nl_attr32(b, 8, r->advmss);
        if (r->hoplimit) nl_attr32(b, 10, r->hoplimit);
        nl_nest_end(b);
    }
    if (v6) {
        nl_attr8(b, 20, r->pref == 1 ? 1 : r->pref == 3 ? 3 : 0);                           /* RTA_PREF */
        uint64_t now = native_clock_ns();
        int32_t expires = r->expires_ns && r->expires_ns > now ? (int32_t)((r->expires_ns - now) / 10000000ULL) : 0;
        struct { uint32_t clntref, lastuse; int32_t expires; uint32_t error, used, id, ts, tsage; } cache = {0, 0, expires, 0, 0, 0, 0, 0};
        nl_attr(b, 12, &cache, sizeof(cache));                                            /* RTA_CACHEINFO */
    }
}
static void neigh_fill(struct nl_builder* b, struct netdev* d, uint8_t family, const struct ip6a* key, const uint8_t* lladdr, uint16_t state,
                       uint8_t flags, uint64_t confirmed, uint64_t used, uint64_t updated, uint8_t probes, uint16_t type, uint16_t msg_flags, uint32_t seq, uint32_t pid) {
    nl_begin(b, type, msg_flags, seq, pid);
    struct { uint8_t family, pad1; uint16_t pad2; int32_t index; uint16_t state; uint8_t flags, type; } info = {family, 0, 0, (int32_t)d->index, state, flags, 1};
    nl_put(b, &info, sizeof(info));
    if (family == AF_INET6) nl_attr(b, 1, key->b, 16); else nl_attr_be32(b, 1, ip6a_v4(key));
    if (lladdr) nl_attr(b, 2, lladdr, 6);
    struct { uint32_t confirmed, used, updated, refcnt; } cache = {clock_t_since(confirmed), clock_t_since(used), clock_t_since(updated), 0};
    nl_attr(b, 3, &cache, sizeof(cache));
    nl_attr32(b, 4, probes);
}
static void rule_fill(struct nl_builder* b, const struct rule* r, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
    nl_begin(b, type, flags, seq, pid);
    struct { uint8_t family, dst_len, src_len, tos, table, res1, res2, action; uint32_t flags; } info =
        {r->family, r->dst_len, r->src_len, r->tos, (uint8_t)(r->table < 256 ? r->table : 252), 0, 0, r->action, r->invert ? 2U : 0};
    nl_put(b, &info, sizeof(info));
    nl_attr32(b, 15, r->table); nl_attr32(b, 6, r->priority);                           /* FRA_TABLE, FRA_PRIORITY */
    bool v6 = r->family == AF_INET6;
    if (r->dst_len) { if (v6) nl_attr(b, 1, r->dst.b, 16); else nl_attr_be32(b, 1, ip6a_v4(&r->dst)); }
    if (r->src_len) { if (v6) nl_attr(b, 2, r->src.b, 16); else nl_attr_be32(b, 2, ip6a_v4(&r->src)); }
    if (r->iifname[0]) nl_attr_string(b, 3, r->iifname);
    if (r->oifname[0]) nl_attr_string(b, 17, r->oifname);
    if (r->fwmark || r->fwmask) { nl_attr32(b, 10, r->fwmark); nl_attr32(b, 16, r->fwmask ? r->fwmask : 0xFFFFFFFFU); }
    if (r->suppress_prefixlen >= 0) nl_attr32(b, 14, (uint32_t)r->suppress_prefixlen);
    if (r->goto_target) nl_attr32(b, 4, r->goto_target);
    nl_attr8(b, 21, r->protocol);                                                       /* FRA_PROTOCOL */
    if (r->ip_proto) nl_attr8(b, 22, r->ip_proto);
    if (r->sport_lo || r->sport_hi) { uint16_t range[2] = {r->sport_lo, r->sport_hi}; nl_attr(b, 23, range, 4); }
    if (r->dport_lo || r->dport_hi) { uint16_t range[2] = {r->dport_lo, r->dport_hi}; nl_attr(b, 24, range, 4); }
}

/* ---- notifications ---- */
static struct nl_builder notify_builder;
void rtnl_notify_link(struct netdev* dev, uint16_t type) {
    if (!has_listeners(NETLINK_ROUTE, RTNLGRP_LINK)) return;
    link_fill(&notify_builder, dev, type, 0, 0, 0); nl_multicast(NETLINK_ROUTE, RTNLGRP_LINK, &notify_builder, NULL);
}
void rtnl_notify_addr4(struct netdev* dev, const struct netdev_v4* address, uint16_t type) {
    if (!has_listeners(NETLINK_ROUTE, RTNLGRP_IPV4_IFADDR)) return;
    addr4_fill(&notify_builder, dev, address, type, 0, 0, 0); nl_multicast(NETLINK_ROUTE, RTNLGRP_IPV4_IFADDR, &notify_builder, NULL);
}
void rtnl_notify_addr6(struct netdev* dev, const struct inet6_ifaddr* address, uint16_t type) {
    if (!has_listeners(NETLINK_ROUTE, RTNLGRP_IPV6_IFADDR)) return;
    addr6_fill(&notify_builder, dev, address, type, 0, 0, 0); nl_multicast(NETLINK_ROUTE, RTNLGRP_IPV6_IFADDR, &notify_builder, NULL);
}
void rtnl_notify_route(const struct route* route, uint16_t type) {
    unsigned group = route->family == AF_INET6 ? RTNLGRP_IPV6_ROUTE : RTNLGRP_IPV4_ROUTE;
    if (!has_listeners(NETLINK_ROUTE, group)) return;
    route_fill(&notify_builder, route, type, 0, 0, 0, 0); nl_multicast(NETLINK_ROUTE, group, &notify_builder, NULL);
}
void rtnl_notify_neigh(const struct neighbour* n, uint16_t type) {
    if (!has_listeners(NETLINK_ROUTE, RTNLGRP_NEIGH)) return;
    neigh_fill(&notify_builder, n->dev, n->family, &n->key, n->has_lladdr ? n->lladdr : NULL, n->state, n->flags, n->confirmed_ns, n->used_ns,
               n->updated_ns, n->probes, type, 0, 0, 0);
    nl_multicast(NETLINK_ROUTE, RTNLGRP_NEIGH, &notify_builder, NULL);
}
void rtnl_notify_rule(const struct rule* rule, uint16_t type) {
    unsigned group = rule->family == AF_INET6 ? RTNLGRP_IPV6_RULE : RTNLGRP_IPV4_RULE;
    if (!has_listeners(NETLINK_ROUTE, group)) return;
    rule_fill(&notify_builder, rule, type, 0, 0, 0); nl_multicast(NETLINK_ROUTE, group, &notify_builder, NULL);
}
void rtnl_notify_prefix(struct netdev* dev, const struct ip6a* prefix, unsigned plen, uint8_t flags, uint32_t valid, uint32_t preferred) {
    if (!has_listeners(NETLINK_ROUTE, RTNLGRP_IPV6_PREFIX)) return;
    struct nl_builder* b = &notify_builder; nl_begin(b, RTM_NEWPREFIX, 0, 0, 0);
    struct { uint8_t family, pad1; uint16_t pad2; int32_t index; uint8_t type, length, flags, pad3; } info = {AF_INET6, 0, 0, (int32_t)dev->index, 3, (uint8_t)plen, flags, 0};
    nl_put(b, &info, sizeof(info)); nl_attr(b, 1, prefix->b, 16);
    uint32_t cache[2] = {preferred, valid}; nl_attr(b, 2, cache, 8);
    nl_multicast(NETLINK_ROUTE, RTNLGRP_IPV6_PREFIX, b, NULL);
}
void rtnl_notify_useropt(struct netdev* dev, const uint8_t* option, unsigned length, const struct ip6a* router) {
    if (!has_listeners(NETLINK_ROUTE, RTNLGRP_ND_USEROPT) || length > 2048) return;
    struct nl_builder* b = &notify_builder; nl_begin(b, RTM_NEWNDUSEROPT, 0, 0, 0);
    struct { uint8_t family, pad1; uint16_t opts_len; int32_t index; uint8_t icmp_type, icmp_code; uint16_t pad2; uint32_t pad3; } info =
        {AF_INET6, 0, (uint16_t)length, (int32_t)dev->index, 134, 0, 0, 0};
    nl_put(b, &info, sizeof(info)); nl_put(b, option, length); nl_attr(b, 1, router->b, 16);   /* NDUSEROPT_SRCADDR */
    nl_multicast(NETLINK_ROUTE, RTNLGRP_ND_USEROPT, b, NULL);
}

/* ---- dumps ---- */
static void dump_links(struct netlink_socket* nl, const struct nlmsghdr* rq, struct nl_builder* b) {
    for (struct netdev* d = netdevs; d; d = d->next) { link_fill(b, d, RTM_NEWLINK, NLM_F_MULTI, rq->seq, nl->portid); send_raw(nl, b); }
}
static void dump_addrs(struct netlink_socket* nl, const struct nlmsghdr* rq, uint8_t family, unsigned index, struct nl_builder* b) {
    for (struct netdev* d = netdevs; d; d = d->next) {
        if (index && d->index != index) continue;
        if (!family || family == AF_INET)
            for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used) { addr4_fill(b, d, &d->v4[i], RTM_NEWADDR, NLM_F_MULTI, rq->seq, nl->portid); send_raw(nl, b); }
        if (!family || family == AF_INET6)
            for (struct inet6_ifaddr* a = d->addrs6; a; a = a->next) { addr6_fill(b, d, a, RTM_NEWADDR, NLM_F_MULTI, rq->seq, nl->portid); send_raw(nl, b); }
    }
}
static void dump_routes(struct netlink_socket* nl, const struct nlmsghdr* rq, uint8_t family, uint32_t table, struct nl_builder* b) {
    uint64_t now = native_clock_ns();
    for (const struct route* r = routes; r; r = r->next) {
        if ((family && r->family != family) || (table && r->table != table)) continue;
        if (r->expires_ns && r->expires_ns <= now) continue;
        route_fill(b, r, RTM_NEWROUTE, NLM_F_MULTI, rq->seq, nl->portid, 0); send_raw(nl, b);
    }
}
static void dump_neighbours(struct netlink_socket* nl, const struct nlmsghdr* rq, uint8_t family, unsigned index, struct nl_builder* b) {
    for (struct neighbour* n = neighbours; n; n = n->next) {
        if ((family && n->family != family) || (index && n->dev->index != index)) continue;
        neigh_fill(b, n->dev, n->family, &n->key, n->has_lladdr ? n->lladdr : NULL, n->state, n->flags, n->confirmed_ns, n->used_ns, n->updated_ns,
                   n->probes, RTM_NEWNEIGH, NLM_F_MULTI, rq->seq, nl->portid);
        send_raw(nl, b);
    }
    if ((!family || family == AF_INET) && netdev_eth0 && (!index || index == netdev_eth0->index)) {
        int count; const arp_entry_t* cache = arp_get_cache(&count);   /* eth0's IPv4 neighbours live in kernel/arp.c */
        for (int i = 0; i < count; i++) {
            if (!cache[i].valid) continue;
            struct ip6a key = ip6a_from4(cache[i].ip);
            if (neigh_lookup(netdev_eth0, AF_INET, &key)) continue;
            neigh_fill(b, netdev_eth0, AF_INET, &key, cache[i].mac.addr, NUD_REACHABLE, 0, 0, 0, 0, 0, RTM_NEWNEIGH, NLM_F_MULTI, rq->seq, nl->portid);
            send_raw(nl, b);
        }
    }
}
static void dump_rules(struct netlink_socket* nl, const struct nlmsghdr* rq, uint8_t family, struct nl_builder* b) {
    for (const struct rule* r = rules; r; r = r->next) {
        if (family && r->family != family) continue;
        rule_fill(b, r, RTM_NEWRULE, NLM_F_MULTI, rq->seq, nl->portid); send_raw(nl, b);
    }
}
static void netconf_fill(struct nl_builder* b, uint8_t family, struct netdev* d, uint16_t flags, uint32_t seq, uint32_t pid) {
    nl_begin(b, RTM_NEWNETCONF, flags, seq, pid);
    struct { uint8_t family, pad[3]; } info = {family, {0}}; nl_put(b, &info, sizeof(info));
    nl_attr32(b, 1, d ? d->index : 0xFFFFFFFFU);   /* NETCONFA_IFINDEX (-1: all) */
    nl_attr32(b, 2, family == AF_INET6 ? (uint32_t)(d ? d->conf6.forwarding : 0) : (uint32_t)(d ? d->ipv4_forwarding : sysctl_ip_forward));
    if (family == AF_INET) nl_attr32(b, 3, d ? (uint32_t)d->rp_filter : 0);
    nl_attr32(b, 4, 0); nl_attr32(b, 5, 0); nl_attr32(b, 6, d && family == AF_INET6 ? (uint32_t)d->conf6.ignore_routes_with_linkdown : 0);
}

/* ---- requests ---- */
static int parse_inet(uint8_t family, const struct nl_attr_view* a, struct ip6a* out) {
    if (!a->present) return 0;
    if (family == AF_INET) { if (a->length < 4) return -22; uint32_t v; memcpy(&v, a->data, 4); *out = ip6a_from4(__builtin_bswap32(v)); return 1; }
    if (family == AF_INET6) { if (a->length < 16) return -22; memcpy(out->b, a->data, 16); return 1; }
    return -97;
}
int tun_create_netlink(const char* name, const struct nl_attr_view* data, unsigned data_length, struct netdev** out);
int wireguard_create(const char* name, struct netdev** out);
int dummy_create(const char* name, struct netdev** out);
static int new_link(struct netlink_socket* nl, const struct nlmsghdr* rq, const uint8_t* p, unsigned n, bool create_allowed) {
    (void)nl;
    if (n < 16) return -22;
    int32_t index; memcpy(&index, p + 4, 4); uint32_t flags, change; memcpy(&flags, p + 8, 4); memcpy(&change, p + 12, 4);
    struct nl_attr_view a[64]; if (!nl_parse(p + 16, n - 16, a, 63)) return -22;
    char name[IFNAMSIZ + 1] = {0}; bool named = nl_string(&a[3], name, sizeof(name));
    if (named && strlen(name) >= IFNAMSIZ) return -22;
    struct netdev* d = index > 0 ? netdev_by_index((unsigned)index) : named ? netdev_by_name(name) : NULL;
    if (index > 0 && !d) return -19;
    if (!d) {
        if (!(rq->flags & NLM_F_CREATE) || !create_allowed) return -19;
        struct nl_attr_view info[8];
        if (!a[18].present || !nl_parse(a[18].data, a[18].length, info, 7)) return -22;
        char kind[16] = {0}; if (!nl_string(&info[1], kind, sizeof(kind))) return -22;
        char final[IFNAMSIZ]; int error;
        if (!strcmp(kind, "tun")) error = tun_create_netlink(named ? name : "", info, 8, &d);
        else if (!strcmp(kind, "wireguard")) { error = netdev_name_template(named ? name : "", "wg%d", final); if (!error) error = wireguard_create(final, &d); }
        else if (!strcmp(kind, "dummy")) { error = netdev_name_template(named ? name : "", "dummy%d", final); if (!error) error = dummy_create(final, &d); }
        else return -95;
        if (error) return error;
        rtnl_notify_link(d, RTM_NEWLINK);
    } else if ((rq->flags & NLM_F_EXCL) && (rq->flags & NLM_F_CREATE)) return -17;
    if (index > 0 && named && strcmp(name, d->name)) { int error = netdev_set_name(d, name); if (error) return error; }
    if (a[4].present) { int error = netdev_set_mtu(d, nl_u32(&a[4])); if (error) return error; }
    if (a[1].present) { int error = netdev_set_address(d, a[1].data, a[1].length); if (error) return error; }
    if (a[13].present) { d->txqlen = nl_u32(&a[13]); rtnl_notify_link(d, RTM_NEWLINK); }
    if (a[20].present) { nl_string(&a[20], d->alias, sizeof(d->alias)); }
    if (a[27].present) d->group = nl_u32(&a[27]);
    if (a[33].present && !strcmp(d->kind, "dummy")) netdev_set_carrier(d, nl_u8(&a[33]) != 0);
    if (a[26].present) {   /* IFLA_AF_SPEC: AF_INET6 address generation mode / token */
        struct nl_attr_view af[16];
        if (nl_parse(a[26].data, a[26].length, af, 15) && af[AF_INET6].present) {
            struct nl_attr_view six[16];
            if (!nl_parse(af[AF_INET6].data, af[AF_INET6].length, six, 15)) return -22;
            if (six[8].present) { uint8_t mode = nl_u8(&six[8]); if (mode > 3) return -22; d->conf6.addr_gen_mode = mode; }
        }
    }
    if (change || flags) {
        uint32_t mask = change ? change : 0;
        if (mask) { int error = netdev_set_flags(d, flags, mask); if (error) return error; }
    }
    return 0;
}
static int del_link(const uint8_t* p, unsigned n) {
    if (n < 16) return -22;
    int32_t index; memcpy(&index, p + 4, 4);
    struct nl_attr_view a[64]; if (!nl_parse(p + 16, n - 16, a, 63)) return -22;
    char name[IFNAMSIZ + 1] = {0}; bool named = nl_string(&a[3], name, sizeof(name));
    struct netdev* d = index > 0 ? netdev_by_index((unsigned)index) : named ? netdev_by_name(name) : NULL;
    if (!d) return -19;
    if (!d->kind[0]) return -95;   /* hardware and loopback interfaces cannot be deleted */
    return netdev_unregister(d);
}
static void legacy_eth0_config(void) {
    /* kernel/ip.c answers ARP and accepts packets for eth0's primary address: keep it in step with netlink changes */
    if (!netdev_eth0) return;
    const net_config_t* c = ip_get_config();
    uint32_t primary = 0; unsigned plen = 24;
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (netdev_eth0->v4[i].used && !netdev_eth0->v4[i].secondary) { primary = netdev_eth0->v4[i].address; plen = netdev_eth0->v4[i].plen; break; }
    uint32_t mask = plen ? 0xFFFFFFFFU << (32 - plen) : 0;
    if (primary != c->ip || (primary && mask != c->subnet_mask)) { ip_set_config(primary, primary ? c->gateway : 0, primary ? mask : 0, c->dns); netdev_sync_legacy(); }
}
static int addr_change(const uint8_t* p, unsigned n, bool add, uint16_t flags) {
    if (n < 8) return -22;
    uint8_t family = p[0], plen = p[1], ifa_flags = p[2]; uint32_t index; memcpy(&index, p + 4, 4);
    struct netdev* d = netdev_by_index(index); if (!d) return -19;
    struct nl_attr_view a[16]; if (!nl_parse(p + 8, n - 8, a, 15)) return -22;
    struct ip6a local = {{0}}, address = {{0}}, broadcast = {{0}};
    int has_local = parse_inet(family, &a[2], &local), has_address = parse_inet(family, &a[1], &address), has_broadcast = parse_inet(family, &a[4], &broadcast);
    if (has_local < 0 || has_address < 0 || has_broadcast < 0) return has_local < 0 ? has_local : has_address < 0 ? has_address : has_broadcast;
    uint32_t extended = a[8].present ? nl_u32(&a[8]) : ifa_flags;
    char label[IFNAMSIZ + 1] = {0}; if (a[3].present) nl_string(&a[3], label, sizeof(label));
    if (family == AF_INET) {
        if (plen > 32) return -22;
        struct ip6a self = has_local ? local : address;
        if (!has_local && !has_address) return -22;
        uint32_t ip = ip6a_v4(&self), peer = has_local && has_address ? ip6a_v4(&address) : 0;
        if (!add) { int error = netdev_v4_del(d, ip, plen); if (!error && d == netdev_eth0) legacy_eth0_config(); return error; }
        if (flags & NLM_F_REPLACE) netdev_v4_del(d, ip, plen);
        int error = netdev_v4_add(d, ip, plen, peer == ip ? 0 : peer, has_broadcast ? ip6a_v4(&broadcast) : 0, label, true);
        if (!error && d == netdev_eth0) legacy_eth0_config();
        return error;
    }
    if (family != AF_INET6) return -97;
    if (plen > 128) return -22;
    struct ip6a self = has_local ? local : address, peer = has_local && has_address && !ip6a_eq(&local, &address) ? address : ip6a_unspecified;
    if (!has_local && !has_address) return -22;
    if (!add) return addrconf_del(d, &self, plen, true);
    uint32_t valid = LIFETIME_INFINITE, preferred = LIFETIME_INFINITE;
    if (a[6].present && a[6].length >= 8) { memcpy(&preferred, a[6].data, 4); memcpy(&valid, a[6].data + 4, 4); if (!valid || preferred > valid) return -22; }
    uint32_t metric = a[9].present ? nl_u32(&a[9]) : 0;
    uint32_t allowed = IFA_F_NODAD | IFA_F_HOMEADDRESS | IFA_F_MANAGETEMPADDR | IFA_F_NOPREFIXROUTE | IFA_F_MCAUTOJOIN | IFA_F_OPTIMISTIC;
    if (addrconf_find(d, &self) && !(flags & NLM_F_REPLACE)) return -17;
    return addrconf_add(d, &self, plen, extended & allowed, valid, preferred, ORIGIN_MANUAL, ip6a_any(&peer) ? NULL : &peer, metric,
                        (flags & NLM_F_REPLACE) != 0, NULL);
}
static int route_parse(const uint8_t* p, unsigned n, struct route* r, struct nl_attr_view* a) {
    if (n < 12) return -22;
    memset(r, 0, sizeof(*r));
    r->family = p[0]; r->plen = p[1]; r->src_len = p[2]; r->table = p[4]; r->protocol = p[5]; r->scope = p[6]; r->type = p[7];
    if (r->family != AF_INET && r->family != AF_INET6) return -97;
    if (r->plen > (r->family == AF_INET ? 32 : 128) || r->src_len > (r->family == AF_INET ? 32 : 128)) return -22;
    if (!nl_parse(p + 12, n - 12, a, 31)) return -22;
    int e;
    if (r->family == AF_INET) r->dst = r->gateway = r->prefsrc = r->src = ip6a_from4(0);
    if ((e = parse_inet(r->family, &a[1], &r->dst)) < 0) return e;
    if ((e = parse_inet(r->family, &a[2], &r->src)) < 0) return e;
    if ((e = parse_inet(r->family, &a[5], &r->gateway)) < 0) return e;
    if ((e = parse_inet(r->family, &a[7], &r->prefsrc)) < 0) return e;
    if (a[15].present) r->table = nl_u32(&a[15]);
    if (a[4].present) r->oif = nl_u32(&a[4]);
    if (a[6].present) r->metric = nl_u32(&a[6]);
    if (a[20].present) { uint8_t pref = nl_u8(&a[20]); r->pref = pref == 1 ? 1 : pref == 3 ? 3 : 0; }
    if (a[8].present) {
        struct nl_attr_view m[20]; if (!nl_parse(a[8].data, a[8].length, m, 19)) return -22;
        if (m[2].present) r->mtu = nl_u32(&m[2]);
        if (m[8].present) r->advmss = nl_u32(&m[8]);
        if (m[10].present) r->hoplimit = nl_u32(&m[10]);
    }
    if (a[23].present && r->family == AF_INET6) { uint32_t seconds = nl_u32(&a[23]); if (seconds) r->expires_ns = native_clock_ns() + (uint64_t)seconds * 1000000000ULL; }
    if (r->family == AF_INET) {
        uint32_t mask = r->plen ? 0xFFFFFFFFU << (32 - r->plen) : 0; r->dst = ip6a_from4(ip6a_v4(&r->dst) & mask);
    } else ip6a_mask(&r->dst, r->plen);
    return 0;
}
static int route_change(const uint8_t* p, unsigned n, bool add, uint16_t flags) {
    struct route r; struct nl_attr_view a[32]; int error = route_parse(p, n, &r, a); if (error) return error;
    if (a[9].present) return -95;   /* RTA_MULTIPATH */
    if (!add) {
        if (!r.table) r.table = RT_TABLE_MAIN;
        if (r.type == RTN_UNICAST && !a[5].present) r.type = 0;   /* `ip route del` sends unicast for any type */
        if (!r.protocol || r.protocol == RTPROT_BOOT) r.protocol = 0;
        if (r.scope == RT_SCOPE_NOWHERE) r.scope = 0;
        return route_del(&r, true);
    }
    if (!r.type) r.type = RTN_UNICAST;
    if (!r.protocol) r.protocol = RTPROT_BOOT;
    bool gateway = r.family == AF_INET ? ip6a_v4(&r.gateway) != 0 : !ip6a_any(&r.gateway);
    if (r.oif) { struct netdev* d = netdev_by_index(r.oif); if (!d) return -19; if (!(d->flags & IFF_UP) && r.type == RTN_UNICAST) return -100; }
    if (r.type == RTN_UNICAST && gateway) {
        /* the gateway must be on-link: a connected route reaches it (Linux fib_check_nh) */
        struct route_result via;
        if (route_lookup(r.family, &r.gateway, NULL, r.oif, 0, &via)) return -101;
        if (via.gateway && !(r.family == AF_INET6 && ip6a_linklocal(&r.gateway))) return -101;
        if (!r.oif) r.oif = via.dev ? via.dev->index : 0;
    } else if (r.type == RTN_UNICAST && !r.oif) return -19;
    if (r.type == RTN_UNICAST && !r.scope && !gateway && r.family == AF_INET) r.scope = RT_SCOPE_LINK;
    if (r.family == AF_INET6 && !r.metric) r.metric = 1024;
    return route_add(&r, (flags & NLM_F_REPLACE) != 0, (flags & NLM_F_EXCL) != 0, true);
}
static int route_get(struct netlink_socket* nl, const struct nlmsghdr* rq, const uint8_t* p, unsigned n, struct nl_builder* b) {
    struct route r; struct nl_attr_view a[32]; int error = route_parse(p, n, &r, a); if (error) return error;
    uint32_t rtm_flags; memcpy(&rtm_flags, p + 8, 4);
    uint32_t mark = a[16].present ? nl_u32(&a[16]) : 0;
    struct route_result result;
    error = a[3].present ? route_lookup_input(r.family, &r.dst, a[2].present ? &r.src : NULL, nl_u32(&a[3]), mark, &result)
                         : route_lookup(r.family, &r.dst, a[2].present ? &r.src : NULL, r.oif, mark, &result);
    if (error) return error;
    if (rtm_flags & 0x2000) {   /* RTM_F_FIB_MATCH: the route itself */
        if (!result.route) return -101;
        route_fill(b, result.route, RTM_NEWROUTE, 0, rq->seq, nl->portid, 0); send_raw(nl, b); return 1;
    }
    struct route answer; memset(&answer, 0, sizeof(answer));
    answer.family = r.family; answer.plen = r.family == AF_INET ? 32 : 128; answer.dst = r.dst; answer.table = result.table ? result.table : RT_TABLE_MAIN;
    answer.type = result.type ? result.type : RTN_UNICAST; answer.protocol = result.route ? result.route->protocol : RTPROT_KERNEL;
    answer.oif = result.dev ? result.dev->index : 0; answer.prefsrc = result.source;
    if (result.gateway) answer.gateway = result.nexthop; else if (r.family == AF_INET) answer.gateway = ip6a_from4(0);
    if (result.route) answer.metric = result.route->metric;
    if (a[2].present) { answer.src = r.src; answer.src_len = r.family == AF_INET ? 32 : 128; }
    route_fill(b, &answer, RTM_NEWROUTE, 0, rq->seq, nl->portid, 0x200 /* RTM_F_CLONED */);
    if (result.mtu) { nl_nest_begin(b, 8); nl_attr32(b, 2, result.mtu); nl_nest_end(b); }
    if (a[16].present) nl_attr32(b, 16, mark);
    nl_attr32(b, 25, credentials_visible_id(task_kernel_uid(), false));   /* RTA_UID */
    send_raw(nl, b);
    return 1;
}
static int neigh_change(const uint8_t* p, unsigned n, bool add, uint16_t flags) {
    if (n < 12) return -22;
    uint8_t family = p[0]; int32_t index; memcpy(&index, p + 4, 4); uint16_t state; memcpy(&state, p + 8, 2); uint8_t ntf = p[10];
    struct netdev* d = netdev_by_index((unsigned)index); if (!d) return -19;
    struct nl_attr_view a[16]; if (!nl_parse(p + 12, n - 12, a, 15)) return -22;
    struct ip6a key; int e = parse_inet(family, &a[1], &key); if (e <= 0) return e < 0 ? e : -22;
    if (!add) return neigh_delete(d, family, &key, true);
    if (a[2].present && a[2].length != 6) return -22;
    if (d->type != ARPHRD_ETHER) return -95;
    return neigh_update_admin(d, family, &key, a[2].present ? a[2].data : NULL, state, ntf & NTF_ROUTER, (flags & NLM_F_CREATE) != 0,
                              (flags & NLM_F_REPLACE) != 0, true);
}
static int rule_change(const uint8_t* p, unsigned n, bool add) {
    if (n < 12) return -22;
    struct rule r; memset(&r, 0, sizeof(r));
    r.family = p[0]; r.dst_len = p[1]; r.src_len = p[2]; r.tos = p[3]; r.table = p[4]; r.action = p[7];
    uint32_t flags; memcpy(&flags, p + 8, 4); r.invert = (flags & 2) != 0;
    r.suppress_prefixlen = -1; r.suppress_ifgroup = -1;
    if (r.family != AF_INET && r.family != AF_INET6) return -97;
    struct nl_attr_view a[32]; if (!nl_parse(p + 12, n - 12, a, 31)) return -22;
    int e;
    if ((e = parse_inet(r.family, &a[1], &r.dst)) < 0 || (e = parse_inet(r.family, &a[2], &r.src)) < 0) return e;
    if (a[3].present) nl_string(&a[3], r.iifname, sizeof(r.iifname));
    if (a[17].present) nl_string(&a[17], r.oifname, sizeof(r.oifname));
    if (a[6].present) r.priority = nl_u32(&a[6]);
    if (a[15].present) r.table = nl_u32(&a[15]);
    if (a[10].present) r.fwmark = nl_u32(&a[10]);
    if (a[16].present) r.fwmask = nl_u32(&a[16]);
    if (a[14].present) r.suppress_prefixlen = (int32_t)nl_u32(&a[14]);
    if (a[4].present) r.goto_target = nl_u32(&a[4]);
    r.protocol = a[21].present ? nl_u8(&a[21]) : RTPROT_BOOT;
    if (a[22].present) r.ip_proto = nl_u8(&a[22]);
    if (a[23].present && a[23].length >= 4) { memcpy(&r.sport_lo, a[23].data, 2); memcpy(&r.sport_hi, a[23].data + 2, 2); }
    if (a[24].present && a[24].length >= 4) { memcpy(&r.dport_lo, a[24].data, 2); memcpy(&r.dport_hi, a[24].data + 2, 2); }
    if (r.dst_len > (r.family == AF_INET ? 32 : 128) || r.src_len > (r.family == AF_INET ? 32 : 128)) return -22;
    if (!add) { if (!a[14].present) r.suppress_prefixlen = -2; return rule_del(&r, true); }
    if (!r.action) r.action = 1;
    if (r.action == 1 && !r.table) return -22;
    return rule_add(&r, true);
}
static void route_request(struct netlink_socket* nl, const struct nlmsghdr* rq, const uint8_t* p, unsigned n) {
    if (!(rq->flags & NLM_F_REQUEST)) return;
    struct nl_builder* b = (void*)arena_alloc(sizeof(struct nl_builder)); if (!b) { nl_send_error(nl, rq, -105); return; }
    int error = 0; uint16_t type = rq->type; uint8_t family = n >= 1 ? p[0] : 0;
    bool dump = (rq->flags & NLM_F_DUMP) != 0 && (type & 3) == 2;
    if (type < 16) { if (rq->flags & NLM_F_ACK) nl_send_error(nl, rq, 0); arena_free((uint64_t)b, sizeof(*b)); return; }
    if (dump) {
        nl->dumping = true; nl->dump_first = true;
        unsigned index = 0;
        if (type == RTM_GETADDR && n >= 8) memcpy(&index, p + 4, 4);
        if (type == RTM_GETNEIGH && n >= 8) memcpy(&index, p + 4, 4);
        if (type == RTM_GETLINK) dump_links(nl, rq, b);
        else if (type == RTM_GETADDR) dump_addrs(nl, rq, family, nl->strict ? index : 0, b);
        else if (type == RTM_GETROUTE) {
            uint32_t table = 0;
            if (nl->strict && n >= 12) { table = p[4]; struct nl_attr_view a[32]; if (nl_parse(p + 12, n - 12, a, 31) && a[15].present) table = nl_u32(&a[15]); }
            dump_routes(nl, rq, family, table, b);
        }
        else if (type == RTM_GETNEIGH) dump_neighbours(nl, rq, family == 7 /* AF_BRIDGE */ ? 0xFF : family, nl->strict ? index : 0, b);
        else if (type == RTM_GETRULE) dump_rules(nl, rq, family, b);
        else if (type == RTM_GETNETCONF) {
            for (uint8_t f = AF_INET; f <= AF_INET6; f = (uint8_t)(f == AF_INET ? AF_INET6 : 0xFF)) {
                if (family && family != f) continue;
                for (struct netdev* d = netdevs; d; d = d->next) { netconf_fill(b, f, d, NLM_F_MULTI, rq->seq, nl->portid); send_raw(nl, b); }
                if (f == AF_INET6) break;
            }
        }
        /* other dumps (qdiscs, multicast, nsids, nexthops, stats...): empty */
        nl->dumping = false;
        nl_send_done(nl, rq);
        arena_free((uint64_t)b, sizeof(*b)); return;
    }
    bool change = type == RTM_NEWLINK || type == RTM_DELLINK || type == RTM_SETLINK || type == RTM_NEWADDR || type == RTM_DELADDR ||
                  type == RTM_NEWROUTE || type == RTM_DELROUTE || type == RTM_NEWNEIGH || type == RTM_DELNEIGH || type == RTM_NEWRULE || type == RTM_DELRULE;
    if (change && !netlink_admin()) error = -1;
    else switch (type) {
    case RTM_GETLINK: {
        if (n < 16) { error = -22; break; }
        int32_t index; memcpy(&index, p + 4, 4); struct nl_attr_view a[64]; char name[IFNAMSIZ + 1] = {0};
        if (!nl_parse(p + 16, n - 16, a, 63)) { error = -22; break; }
        nl_string(&a[3], name, sizeof(name));
        struct netdev* d = index > 0 ? netdev_by_index((unsigned)index) : name[0] ? netdev_by_name(name) : NULL;
        if (!d) { error = -19; break; }
        link_fill(b, d, RTM_NEWLINK, 0, rq->seq, nl->portid); send_raw(nl, b); error = 1; break;
    }
    case RTM_NEWLINK: case RTM_SETLINK: error = new_link(nl, rq, p, n, type == RTM_NEWLINK); break;
    case RTM_DELLINK: error = del_link(p, n); break;
    case RTM_NEWADDR: case RTM_DELADDR: error = addr_change(p, n, type == RTM_NEWADDR, rq->flags); break;
    case RTM_NEWROUTE: case RTM_DELROUTE: error = route_change(p, n, type == RTM_NEWROUTE, rq->flags); break;
    case RTM_GETROUTE: error = route_get(nl, rq, p, n, b); break;
    case RTM_NEWNEIGH: case RTM_DELNEIGH: error = neigh_change(p, n, type == RTM_NEWNEIGH, rq->flags); break;
    case RTM_NEWRULE: case RTM_DELRULE: error = rule_change(p, n, type == RTM_NEWRULE); break;
    case RTM_GETNETCONF: {
        if (n < 4) { error = -22; break; }
        struct nl_attr_view a[8]; if (!nl_parse(p + 4, n - 4, a, 7)) { error = -22; break; }
        int32_t index = (int32_t)nl_u32(&a[1]); struct netdev* d = index > 0 ? netdev_by_index((unsigned)index) : NULL;
        if (index > 0 && !d) { error = -19; break; }
        netconf_fill(b, family ? family : AF_INET, d, 0, rq->seq, nl->portid); send_raw(nl, b); error = 1; break;
    }
    default: error = (type & 3) == 2 ? -95 : -95; break;
    }
    if (error < 0) nl_send_error(nl, rq, error);
    else if (rq->flags & NLM_F_ACK) nl_send_error(nl, rq, 0);
    arena_free((uint64_t)b, sizeof(*b));
}

/* ---- descriptor operations ---- */
static int64_t netlink_send(struct netlink_socket* nl, const struct iovec64* vectors, unsigned count, uint64_t total) {
    if (total > 1048576 || total > nl->send_buffer) return -90;
    if (!iov_access(vectors, count, 0, total, false)) return -14;
    if (nl->protocol == NETLINK_KOBJECT_UEVENT) return (int64_t)total;   /* uevent: nothing listens to user messages */
    autobind(nl);
    if (!total) return 0;
    /* The whole datagram in one buffer, parsed as netlink_rcv_skb does: every message must fit in the bytes left
     * (64-bit arithmetic: no length can wrap the walk back), a truncated or malformed tail ends the walk. */
    uint8_t* request = (void*)arena_alloc(total); if (!request) return -105;   /* ENOBUFS, as a failed skb allocation */
    iov_copy(vectors, count, 0, request, (unsigned)total, true);
    for (uint64_t offset = 0; offset + sizeof(struct nlmsghdr) <= total;) {
        struct nlmsghdr header; memcpy(&header, request + offset, sizeof(header));
        if (header.len < sizeof(header) || header.len > total - offset) break;
        const uint8_t* payload = request + offset + sizeof(header); unsigned length = header.len - (unsigned)sizeof(header);
        if (nl->protocol == NETLINK_ROUTE) route_request(nl, &header, payload, length);
        else if (nl->protocol == NETLINK_GENERIC) {
            int error = genl_request(nl, &header, payload, length);
            if (error < 0) nl_send_error(nl, &header, error); else if (!error && (header.flags & NLM_F_ACK)) nl_send_error(nl, &header, 0);
        } else if (nl->protocol == NETLINK_NETFILTER) {
            int error = nfnl_request(nl, &header, payload, length, &nl->batch_active, &nl->batch_seq);
            if (error < 0) nl_send_error(nl, &header, error); else if (!error && (header.flags & NLM_F_ACK)) nl_send_error(nl, &header, 0);
        } else if (nl->protocol == NETLINK_SOCK_DIAG) nl_send_error(nl, &header, -95);
        offset += ((uint64_t)header.len + 3) & ~3ULL;
    }
    arena_free((uint64_t)request, total);
    if (nl->count) wake(nl);
    return (int64_t)total;
}
/* Receive one datagram; source address (sockaddr_nl of the kernel) and MSG_TRUNC as Linux reports them. */
static int64_t netlink_receive(struct netlink_socket* nl, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags,
                               uint64_t descriptor_flags, uint64_t name, uint32_t* name_size, uint32_t* message_flags) {
    if (nl->overrun) { nl->overrun = false; return -105; }
    while (!nl->head) {
        if ((flags & 0x40) || (descriptor_flags & 0x800)) return -11;
        int error = task_wait_channel(nl); if (error) return error;
        if (nl->overrun) { nl->overrun = false; return -105; }
    }
    struct nl_datagram* d = nl->head;
    unsigned size = d->size, amount = total < size ? (unsigned)total : size;
    if (amount && !iov_access(vectors, count, 0, amount, true)) return -14;
    if (amount) iov_copy(vectors, count, 0, d->data, amount, false);
    if (name && name_size) {
        struct sockaddr_nl source = {AF_NETLINK, 0, 0, nl->protocol == NETLINK_KOBJECT_UEVENT ? 1U : 0U};   /* uevents: kernel multicast group 1 */
        unsigned n = *name_size < sizeof(source) ? *name_size : sizeof(source);
        if (n && !accessible(name, n, true)) return -14;
        if (n) copy_to_user((void*)name, &source, n);
        *name_size = sizeof(source);
    } else if (name_size) *name_size = 0;
    if (message_flags) *message_flags = size > total ? 0x20 : 0;               /* MSG_TRUNC */
    if (!(flags & 2)) {                                                       /* MSG_PEEK keeps it */
        nl->head = d->next; if (!nl->head) nl->tail = NULL;
        nl->count--; nl->queued -= d->size; arena_free((uint64_t)d, d->charge);
    }
    return (flags & 0x20) ? size : amount;                                    /* MSG_TRUNC: the real length */
}
static int64_t netlink_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    return write ? netlink_send(object, vectors, count, total) : netlink_receive(object, vectors, count, total, 0, flags, 0, NULL, NULL);
}
static int netlink_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct netlink_socket* nl = object;
    if ((uint32_t)command == 0x541B) {                                          /* FIONREAD: bytes of the next datagram */
        if (!accessible(argument, 4, true)) return -14;
        return uput32(argument, (uint32_t)(nl->head ? (int)nl->head->size : 0));
    }
    if (((uint32_t)command & 0xFF00) == 0x8900) return netif_ioctl(command, argument);
    return -25;
}
static uint32_t netlink_ready(void* object) { struct netlink_socket* nl = object; return (nl->head || nl->overrun ? 1U : 0) | (nl->overrun ? 8U : 0) | 4U; }
static void* netlink_source(void* object) { return object; }
static void netlink_release(void* object) {
    struct netlink_socket* nl = object;
    for (struct netlink_socket** link = &sockets; *link; link = &(*link)->next) if (*link == nl) { *link = nl->next; break; }
    if (nl->protocol == NETLINK_GENERIC) nl80211_socket_closed(nl);          /* Wi-Fi frame registrations, socket owners */
    while (nl->head) { struct nl_datagram* d = nl->head; nl->head = d->next; arena_free((uint64_t)d, d->charge); }
    page_free((uint64_t)nl);
}
static void netlink_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 4; stat->ino = ((struct netlink_socket*)object)->id; stat->nlink = 1; stat->mode = 0140777;
    stat->uid = credentials_visible_id(credentials_kernel_id(credentials_current(), false, 1), false);
    stat->gid = credentials_visible_id(credentials_kernel_id(credentials_current(), true, 1), true); stat->block_size = 4096;
}
static const struct object_operations netlink_operations = {netlink_io, netlink_ioctl, netlink_ready, netlink_source, netlink_release, netlink_stat};
bool netlink_descriptor(struct open_file* description) { return fd_object(description, &netlink_operations) != NULL; }

/* ---- socket calls ---- */
static int name_load(uint64_t address, uint64_t size, struct sockaddr_nl* name) {
    if (size < sizeof(*name)) return -22;
    if (!accessible(address, sizeof(*name), false)) return -14;
    copy_from_user(name, (void*)address, sizeof(*name)); return name->family == AF_NETLINK ? 0 : -22;
}
static int name_store(struct netlink_socket* nl, uint64_t address, uint64_t size_address) {
    if (!accessible(size_address, 4, true)) return -14;
    struct sockaddr_nl name = {AF_NETLINK, 0, nl->portid, (uint32_t)nl->groups};
    uint32_t capacity = uget32(size_address); if ((int32_t)capacity < 0) return -22;
    unsigned n = capacity < sizeof(name) ? capacity : sizeof(name);
    if (n && !accessible(address, n, true)) return -14;
    if (n) copy_to_user((void*)address, &name, n);
    return uput32(size_address, sizeof(name));
}
static int create(uint64_t type, uint64_t protocol) {
    unsigned kind = type & 0xF;
    if ((type & ~(0xFULL | 0x800ULL | 0x80000ULL)) || (kind != 2 && kind != 3)) return -94;   /* SOCK_DGRAM / SOCK_RAW */
    if (protocol != NETLINK_ROUTE && protocol != NETLINK_KOBJECT_UEVENT && protocol != NETLINK_GENERIC && protocol != NETLINK_NETFILTER &&
        protocol != NETLINK_SOCK_DIAG) return -93;
    struct netlink_socket* nl = (void*)page_alloc(); if (!nl) return -12;
    _Static_assert(sizeof(struct netlink_socket) <= 4096, "netlink socket page");
    memset(nl, 0, sizeof(*nl)); nl->id = ++next_id; nl->protocol = (int)protocol; nl->receive_buffer = nl->send_buffer = 212992;
    int fd = fd_create_object(&netlink_operations, nl, (type & 0x800) | (type & 0x80000) | 2, "socket:[netlink]");
    if (fd < 0) page_free((uint64_t)nl); else { nl->next = sockets; sockets = nl; }
    return fd;
}
static bool group_allowed(struct netlink_socket* nl, unsigned group) {
    (void)group;
    if (nl->protocol == NETLINK_KOBJECT_UEVENT || nl->protocol == NETLINK_ROUTE || nl->protocol == NETLINK_GENERIC || nl->protocol == NETLINK_NETFILTER) return true;
    return false;
}
int64_t netlink_call(struct registers* r) {
    if (r->rax == 41) return create(r->rsi, r->rdx);
    struct open_file* description = fd_lookup(r->rdi); if (!description) return -9;
    struct netlink_socket* nl = fd_object(description, &netlink_operations); if (!nl) return -88;
    uint64_t descriptor_flags = fd_status_flags(description);
    int64_t result;
    fd_pin(description);
    switch (r->rax) {
    case 49: {                                                                  /* bind */
        struct sockaddr_nl name; result = name_load(r->rsi, r->rdx, &name); if (result) break;
        if (name.pid && nl->bound && name.pid != nl->portid) { result = -22; break; }
        if (!nl->bound && name.pid) {
            if (portid_used(name.pid, nl->protocol)) { result = -98; break; }
            nl->portid = name.pid; nl->bound = true;
        }
        autobind(nl);
        nl->groups = (nl->groups & ~0xFFFFFFFFULL) | name.groups; result = 0; break;
    }
    case 42: {                                                                  /* connect: only the kernel (pid 0) is a peer */
        struct sockaddr_nl name; result = name_load(r->rsi, r->rdx, &name); if (result) break;
        autobind(nl);
        result = name.pid ? -111 : 0; break;
    }
    case 51: autobind(nl); result = name_store(nl, r->rsi, r->rdx); break;
    case 52: { if (!accessible(r->rdx, 4, true)) { result = -14; break; }
               struct sockaddr_nl peer = {AF_NETLINK, 0, 0, 0}; uint32_t capacity = uget32(r->rdx); unsigned n = capacity < sizeof(peer) ? capacity : sizeof(peer);
               if (n && !accessible(r->rsi, n, true)) { result = -14; break; }
               if (n) copy_to_user((void*)r->rsi, &peer, n); uput32(r->rdx, sizeof(peer)); result = 0; break; }
    case 44: {                                                                  /* sendto */
        if (r->r8) { struct sockaddr_nl name; result = name_load(r->r8, r->r9, &name); if (result) break; if (name.pid) { result = -111; break; } }
        struct iovec64 vector = {r->rsi, r->rdx}; result = netlink_send(nl, &vector, 1, r->rdx); break;
    }
    case 45: {                                                                  /* recvfrom */
        struct iovec64 vector = {r->rsi, r->rdx}; uint32_t size = 0;
        if (r->r8) {
            if (!accessible(r->r9, 4, true)) { result = -14; break; }
            size = uget32(r->r9); if ((int32_t)size < 0) { result = -22; break; }
        }
        result = netlink_receive(nl, &vector, 1, r->rdx, r->r10, descriptor_flags, r->r8, r->r8 ? &size : NULL, NULL);
        if (result >= 0 && r->r8) {                                             /* the receive may have slept: check again */
            if (!accessible(r->r9, 4, true)) { result = -14; break; }
            uput32(r->r9, size);
        }
        break;
    }
    case 46: case 47: {                                                         /* sendmsg / recvmsg */
        if (!accessible(r->rsi, sizeof(struct message64), r->rax == 47)) { result = -14; break; }
        struct message64 message; copy_from_user(&message, (void*)r->rsi, sizeof(message));
        if (message.count > 64) { result = -22; break; }
        struct iovec64 vectors[64]; uint64_t total = 0;
        if (message.count && !accessible(message.vectors, message.count * sizeof(struct iovec64), false)) { result = -14; break; }
        if (message.count && copy_from_user(vectors, (void*)message.vectors, message.count * sizeof(struct iovec64))) { result = -14; break; }
        bool overflow = false;
        for (uint64_t i = 0; i < message.count; i++) { if (vectors[i].size > 0x7FFFFFFF - total) overflow = true; else total += vectors[i].size; }
        if (overflow) { result = -22; break; }
        if (r->rax == 46) {
            if (message.name) { struct sockaddr_nl name; result = name_load(message.name, message.name_size, &name); if (result) break; if (name.pid) { result = -111; break; } }
            result = netlink_send(nl, vectors, (unsigned)message.count, total); break;
        }
        uint32_t name_size = message.name ? message.name_size : 0, out_flags = 0; uint64_t control_size_in = message.control_size;
        result = netlink_receive(nl, vectors, (unsigned)message.count, total, r->rdx, descriptor_flags, message.name, message.name ? &name_size : NULL, &out_flags);
        if (result >= 0) {
            message.name_size = name_size; message.flags = out_flags;
            if ((nl->protocol == NETLINK_KOBJECT_UEVENT || nl->passcred) && message.control && control_size_in >= 32) {
                /* uevents and, with SO_PASSCRED, every netlink message come from the kernel: SCM_CREDENTIALS {pid 0, uid 0,
                 * gid 0} (libudev drops uevents otherwise, avahi-daemon its rtnetlink data) */
                if (!accessible(message.control, 32, true)) { result = -14; break; }
                uint32_t credentials[8] = {28, 0, 1, 2, 0, 0, 0, 0};   /* cmsghdr {len 28, SOL_SOCKET, SCM_CREDENTIALS} + ucred */
                copy_to_user((void*)message.control, credentials, 32); message.control_size = 32;
            } else if (nl->pktinfo && message.control && message.control_size >= 20) {   /* NETLINK_PKTINFO: destination group */
                if (!accessible(message.control, 20, true)) { result = -14; break; }
                uint8_t cmsg[24] = {0}; uint64_t length = 20; int32_t level = 270, type = 3; uint32_t group = 0;
                memcpy(cmsg, &length, 8); memcpy(cmsg + 8, &level, 4); memcpy(cmsg + 12, &type, 4); memcpy(cmsg + 16, &group, 4);
                copy_to_user((void*)message.control, cmsg, 20); message.control_size = message.control_size >= 24 ? 24 : 20;
            } else message.control_size = 0;
            if (!accessible(r->rsi, sizeof(message), true)) { result = -14; break; }
            copy_to_user((void*)r->rsi, &message, sizeof(message));
        }
        break;
    }
    case 48: result = 0; break;                                                 /* shutdown */
    case 54: {                                                                  /* setsockopt: SOL_SOCKET buffers/credentials/filters, SOL_NETLINK */
        uint64_t level = r->rsi, option = r->rdx;
        if (level == 1 && (option == 8 || option == 7 || option == 32 || option == 33)) {   /* SO_RCVBUF(FORCE), SO_SNDBUF(FORCE) */
            if (r->r8 < 4 || !accessible(r->r10, 4, false)) { result = r->r8 < 4 ? -22 : -14; break; }
            uint32_t value = uget32(r->r10);
            if (value > 0x3FFFFFFF) value = 0x3FFFFFFF;
            if (option == 8 || option == 33) nl->receive_buffer = value * 2 < 2304 ? 2304 : value * 2; else nl->send_buffer = value * 2 < 4608 ? 4608 : value * 2;
            result = 0;
        } else if (level == 1 && option == 16) {                               /* SO_PASSCRED */
            if (r->r8 < 4 || !accessible(r->r10, 4, false)) { result = r->r8 < 4 ? -22 : -14; break; }
            nl->passcred = uget32(r->r10) != 0; result = 0;
        } else if (level == 1 && (option == 26 || option == 27 || option == 44 || option == 2 || option == 9 || option == 1)) result = 0; /* (DE)ATTACH_FILTER, LOCK_FILTER, REUSEADDR, KEEPALIVE */
        else if (level == 270) {                                                /* SOL_NETLINK */
            if (r->r8 < 4 || !accessible(r->r10, 4, false)) { result = r->r8 < 4 ? -22 : -14; break; }
            uint32_t value = uget32(r->r10); result = 0;
            if (option == 1 || option == 2) {                                   /* NETLINK_ADD/DROP_MEMBERSHIP */
                if (!value || value > 64) { result = -22; break; }
                if (!group_allowed(nl, value)) { result = -1; break; }
                autobind(nl);
                if (option == 1) nl->groups |= 1ULL << (value - 1); else nl->groups &= ~(1ULL << (value - 1));
            }
            else if (option == 3) nl->pktinfo = value != 0;
            else if (option == 5) nl->no_enobufs = value != 0;
            else if (option == 8) nl->listen_all = value != 0;
            else if (option == 10) nl->cap_ack = value != 0;
            else if (option == 11) nl->ext_ack = value != 0;
            else if (option == 12) nl->strict = value != 0;
            else if (option == 4) result = 0;                                   /* NETLINK_BROADCAST_ERROR */
            else result = -92;
        } else result = -92;
        break;
    }
    case 55: {                                                                  /* getsockopt */
        uint64_t level = r->rsi, option = r->rdx; uint32_t value;
        if (level == 270 && option == 9) {                                      /* NETLINK_LIST_MEMBERSHIPS */
            if (!accessible(r->r8, 4, true)) { result = -14; break; }
            uint32_t capacity = uget32(r->r8), groups[2] = {(uint32_t)nl->groups, (uint32_t)(nl->groups >> 32)};
            unsigned n = capacity < 8 ? capacity & ~3U : 8;
            if (n && !accessible(r->r10, n, true)) { result = -14; break; }
            if (n) copy_to_user((void*)r->r10, groups, n);
            uput32(r->r8, 8); result = 0; break;
        }
        if (level == 1 && option == 8) value = nl->receive_buffer;
        else if (level == 1 && option == 7) value = nl->send_buffer;
        else if (level == 1 && option == 3) value = 3;                          /* SO_TYPE: SOCK_RAW */
        else if (level == 1 && option == 38) value = (uint32_t)nl->protocol;    /* SO_PROTOCOL */
        else if (level == 1 && option == 39) value = AF_NETLINK;               /* SO_DOMAIN */
        else if (level == 1 && option == 4) value = 0;                          /* SO_ERROR */
        else if (level == 270 && option == 3) value = nl->pktinfo;
        else if (level == 270 && option == 5) value = nl->no_enobufs;
        else if (level == 270 && option == 10) value = nl->cap_ack;
        else if (level == 270 && option == 11) value = nl->ext_ack;
        else if (level == 270 && option == 12) value = nl->strict;
        else { result = -92; break; }
        if (!accessible(r->r8, 4, true) || !accessible(r->r10, 4, true)) { result = -14; break; }
        if ((int32_t)uget32(r->r8) < 4) { result = -22; break; }
        uput32(r->r10, value); uput32(r->r8, 4); result = 0; break;
    }
    case 43: case 288: case 50: result = -95; break;                            /* accept, listen */
    default: result = -95;
    }
    fd_unpin(description);
    return result;
}
/* Kernel uevents (NETLINK_KOBJECT_UEVENT, multicast group 1 - what udev monitors and upower bind to): one datagram
 * "ACTION@DEVPATH\0ACTION=...\0DEVPATH=...\0SUBSYSTEM=...\0..." per listener, bounded by its SO_RCVBUF (an overrun
 * reports ENOBUFS once, like the other notifications) (power.c). */
void netlink_uevent(const char* message, unsigned length) {
    if (!length || length > DATAGRAM_BYTES) return;
    for (struct netlink_socket* nl = sockets; nl; nl = nl->next) {
        if (nl->protocol != NETLINK_KOBJECT_UEVENT || !(nl->groups & 1)) continue;
        if (queue_bytes(nl, (const uint8_t*)message, length, true, false) || nl->overrun) wake(nl);
    }
}
/* The active NIC's carrier changed (netdrv.c netdrv_link_report, driver poll or suspend/resume context; it already set
 * the device's link_up): update eth0 now - netdev_set_carrier sends RTM_NEWLINK (RTNLGRP_LINK, IFLA_CARRIER) and
 * reruns IPv6 DAD/router solicitation when the link returns (netdev_poll would notice on its next round anyway). */
void netlink_link_changed(void) {
    if (netdev_eth0 && netdev_eth0->hw && netdev_eth0->hw->link_up != netdev_eth0->carrier) netdev_set_carrier(netdev_eth0, netdev_eth0->hw->link_up);
}
