/* Native IPv4 datapath around kernel/ip.c: kernel/ip.c keeps input validation, the loopback ring and the protocol
 * demultiplexing; ip_send_packet_from() hands every packet to ip4_output() here, which routes it (policy rules,
 * tables, SO_BINDTODEVICE / multicast interface), picks the source, runs the netfilter OUTPUT/POSTROUTING hooks,
 * fragments to the path MTU unless DF is wanted, and transmits through eth0 (kernel/arp.c resolution), an ARP
 * neighbour of a TAP device, a point-to-point device (TUN, WireGuard) or the loopback ring. IPv6 address handles
 * (TCP over IPv6, see netdev.h) are passed on to ipv6.c. Input: reassembly of fragments (bounded, timed out) and
 * the local-delivery hook (filter INPUT, raw and ping sockets). */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/arp.h"
#include "../../kernel/string.h"

struct ip4_stats ip4_counters;
static uint16_t ip_id;

bool ip4_local_address(uint32_t address) {
    if (ip6h_is(address)) {
        struct ip6a a; uint32_t scope;
        return ip6h_address(address, &a, &scope) && (ip6a_loopback(&a) || addrconf_local(&a, scope, false));
    }
    return netdev_v4_local(address);
}
uint32_t ip4_source_for(uint32_t dst) {
    if (ip6h_is(dst)) {
        struct ip6a a, source; uint32_t scope;
        if (!ip6h_address(dst, &a, &scope)) return 0;
        struct route_result result;
        if (ip6a_loopback(&a)) source = ip6a_loopback_address;
        else if (addrconf_local(&a, scope, true)) source = a;
        else if (route_lookup(AF_INET6, &a, NULL, scope, 0, &result) || ip6a_any(&result.source)) return 0;
        else source = result.source;
        return ip6h_get(&source, ip6a_linklocal(&source) ? scope : 0);
    }
    if ((dst >> 24) == 127) return 0x7F000001U;
    if (dst == 0xFFFFFFFFU) return ip_get_config()->ip;   /* DHCP: 0.0.0.0 until a lease exists (dhcp64.c) */
    if (netdev_v4_local(dst)) return dst;
    struct ip6a a = ip6a_from4(dst); struct route_result result;
    if (route_lookup(AF_INET, &a, NULL, 0, 0, &result)) return ip_get_config()->ip;
    return ip6a_v4(&result.source);
}

static void header(uint8_t* p, uint32_t src, uint32_t dst, uint8_t protocol, unsigned total, uint16_t id, uint16_t frag, uint8_t ttl, uint8_t tos) {
    p[0] = 0x45; p[1] = tos; put16(p + 2, (uint16_t)total); put16(p + 4, id); put16(p + 6, frag); p[8] = ttl; p[9] = protocol;
    p[10] = p[11] = 0; put32(p + 12, src); put32(p + 16, dst);
    uint16_t sum = ip_checksum(p, 20); memcpy(p + 10, &sum, 2);   /* ip_checksum yields network-order bytes in place */
}
/* Packets the kernel sends once and never retries (nft TCP resets and rejects, ICMP errors, DHCPRELEASE) wait here
 * while eth0 resolves the next hop, as on Linux's neighbour queue (unres_qlen); a caller sets ip4_hold_unresolved
 * around its ip4_output(). Sockets keep -2: TCP retransmits, datagrams queue and retry themselves. 3 s, 8 packets. */
#define HOLD_MAX 8
#define HOLD_NS 3000000000ULL
int ip4_hold_unresolved;
static struct { bool used; uint32_t nexthop; uint64_t since; unsigned length; uint8_t* frame; } hold[HOLD_MAX];
static void hold_drop(unsigned i) { arena_free((uint64_t)hold[i].frame, hold[i].length); hold[i].used = false; }
static void hold_add(struct netdev* dev, uint32_t nexthop, const uint8_t* packet, unsigned length) {
    unsigned slot = HOLD_MAX, oldest = 0;
    for (unsigned i = 0; i < HOLD_MAX; i++) {
        if (!hold[i].used) { slot = i; break; }
        if (hold[i].since < hold[oldest].since) oldest = i;
    }
    if (slot == HOLD_MAX) { hold_drop(oldest); slot = oldest; dev->stats.tx_dropped++; }
    uint8_t* frame = (void*)arena_alloc(ETH_HEADER_LEN + length); if (!frame) return;
    memcpy(frame + 6, dev->addr, 6); put16(frame + 12, ETH_TYPE_IPV4); memcpy(frame + ETH_HEADER_LEN, packet, length);
    hold[slot].used = true; hold[slot].nexthop = nexthop; hold[slot].since = native_clock_ns();
    hold[slot].length = ETH_HEADER_LEN + length; hold[slot].frame = frame;
}
void ip4_hold_poll(uint64_t now) {
    for (unsigned i = 0; i < HOLD_MAX; i++) {
        if (!hold[i].used) continue;
        mac_addr_t mac;
        if (arp_lookup(hold[i].nexthop, &mac) && netdev_eth0 && netdev_running(netdev_eth0)) {
            memcpy(hold[i].frame, mac.addr, 6);
            net_send_packet(netdev_eth0->hw, hold[i].frame, (uint16_t)hold[i].length);
            hold_drop(i);
        } else if (now - hold[i].since > HOLD_NS) { hold_drop(i); if (netdev_eth0) netdev_eth0->stats.tx_dropped++; }
    }
}
/* Hand one complete IPv4 packet to its interface. */
static int transmit(struct netdev* dev, const struct ip6a* nexthop, bool broadcast, const uint8_t* packet, unsigned length) {
    uint32_t dst = be32(packet + 16);
    if (dev == netdev_lo) {
        int result = ip_loopback_queue(packet, (uint16_t)length);
        if (!result) { netdev_lo->stats.tx_packets++; netdev_lo->stats.tx_bytes += length; netdev_lo->stats.rx_packets++; netdev_lo->stats.rx_bytes += length; }
        return result;
    }
    if (dev->type == ARPHRD_NONE) return netdev_xmit(dev, packet, length, ETH_TYPE_IPV4);
    if (dev->type != ARPHRD_ETHER) return -101;
    if (dev != netdev_eth0) {
        if (broadcast || (dst >> 28) == 14) {
            uint8_t frame[ETH_HEADER_LEN + 1500];
            if (length > dev->mtu || length > 1500) return -90;
            if (broadcast) memset(frame, 0xFF, 6);
            else { frame[0] = 1; frame[1] = 0; frame[2] = 0x5E; frame[3] = (uint8_t)((dst >> 16) & 0x7F); frame[4] = (uint8_t)(dst >> 8); frame[5] = (uint8_t)dst; }
            memcpy(frame + 6, dev->addr, 6); put16(frame + 12, ETH_TYPE_IPV4); memcpy(frame + ETH_HEADER_LEN, packet, length);
            return netdev_xmit(dev, frame, ETH_HEADER_LEN + length, ETH_TYPE_IPV4);
        }
        return neigh_output(dev, AF_INET, nexthop, packet, length, ETH_TYPE_IPV4);
    }
    if (!netdev_running(dev)) return -100;
    uint8_t frame[ETH_FRAME_MAX];
    if (length > ETH_MTU) return -90;
    mac_addr_t mac;
    if (broadcast) memcpy(mac.addr, MAC_BROADCAST.addr, 6);
    else if ((dst >> 28) == 14) { mac.addr[0] = 1; mac.addr[1] = 0; mac.addr[2] = 0x5E; mac.addr[3] = (uint8_t)((dst >> 16) & 0x7F); mac.addr[4] = (uint8_t)(dst >> 8); mac.addr[5] = (uint8_t)dst; }
    else if (!arp_resolve(ip6a_v4(nexthop), &mac)) {   /* ARP pending: TCP retransmits, datagram queues retry */
        if (!ip4_hold_unresolved) return -2;
        hold_add(dev, ip6a_v4(nexthop), packet, length); return 0;   /* a one-shot kernel packet waits for the reply */
    }
    memcpy(frame, mac.addr, 6); memcpy(frame + 6, dev->addr, 6); put16(frame + 12, ETH_TYPE_IPV4);
    memcpy(frame + ETH_HEADER_LEN, packet, length);
    return net_send_packet(dev->hw, frame, (uint16_t)(ETH_HEADER_LEN + length)) == 0 ? 0 : -11;
}
/* LOCAL_OUT / POSTROUTING on the packet being built (writable: NAT may rewrite it) */
static int filter_out(uint8_t* packet, unsigned length, struct netdev* out, uint32_t mark, uint8_t hook) {
    if (!nf_active() && !nf_nat_active()) return NF_ACCEPT;
    struct nf_packet p = {.family = AF_INET, .hook = hook, .data = packet, .writable = packet, .length = length, .out = out, .mark = mark};
    unsigned ihl = (packet[0] & 15) * 4u;
    p.l4_proto = packet[9]; p.l4_offset = ihl; p.fragment = (be16(packet + 6) & 0x1FFF) != 0;
    return nf_hook(&p);
}
int ip4_transmit(struct netdev* dev, const struct ip6a* nexthop, const uint8_t* packet, unsigned length) {   /* forward.c */
    return transmit(dev, nexthop, false, packet, length);
}
int ip4_output(uint32_t src, uint32_t dst, uint8_t protocol, const void* payload, unsigned length, const struct ip6_send_options* options) {
    if (ip6h_is(dst)) {   /* TCP over IPv6 (tcp.c names the endpoints by handles) */
        struct ip6a d, s; uint32_t scope, source_scope;
        if (!ip6h_address(dst, &d, &scope)) return -113;
        struct ip6_send_options o; ip6_options_default(&o);
        if (src && ip6h_address(src, &s, &source_scope)) o.source = &s;
        o.oif = scope;
        return ip6_send(&d, protocol, payload, length, &o);
    }
    bool raw = options && options->raw_header;
    if ((length && !payload) || length > 65535 - (raw ? 0 : 20)) return -90;
    ip4_counters.out_requests++;
    uint8_t ttl = options && options->hop_limit >= 0 ? (uint8_t)options->hop_limit : (dst >> 28) == 14 ? 1 : (uint8_t)sysctl_ip_default_ttl;
    uint8_t tos = options && options->tclass >= 0 ? (uint8_t)options->tclass : 0;
    unsigned oif = options ? options->oif : 0; uint32_t mark = options ? options->mark : 0;
    bool dontfrag = !options || options->dontfrag;   /* the kernel's own TCP/UDP/ICMP: DF, as ip.c always set */
    bool df_when_fits = options && options->df_when_fits;
    if (raw) {   /* IP_HDRINCL: the caller's header (Linux fills the checksum, id and total length) */
        if (length < 20) return -22;
        const uint8_t* given = payload; unsigned ihl = (given[0] & 15) * 4u;
        if ((given[0] >> 4) != 4 || ihl < 20 || ihl > length) return -22;
        src = be32(given + 12); dst = be32(given + 16);
    }
    bool broadcast = dst == 0xFFFFFFFFU, multicast = (dst >> 28) == 14;
    struct netdev* dev = NULL; struct ip6a nexthop = ip6a_from4(dst);
    unsigned mtu;
    if (netdev_v4_local(dst) && !broadcast) {
        dev = netdev_lo; mtu = 65535;
        if (!src) src = (dst >> 24) == 127 ? 0x7F000001U : dst;
    } else if (broadcast || (multicast && oif)) {
        dev = oif ? netdev_by_index(oif) : netdev_eth0;
        if (!dev || !dev->present) { ip4_counters.out_no_routes++; return -101; }
        mtu = dev->mtu;
        if (!src && !broadcast) src = source_select4(dst, dev);
    } else {
        struct ip6a d = ip6a_from4(dst), s = ip6a_from4(src); struct route_result result;
        int error = route_lookup(AF_INET, &d, src ? &s : NULL, oif, mark, &result);
        if (error) { ip4_counters.out_no_routes++; return error; }
        if (oif && result.dev && result.dev->index != oif && result.type != RTN_LOCAL) {
            /* bound to another interface: send there directly when the destination is on-link for it (Linux
             * SO_BINDTODEVICE with a connected route), else the bound interface's own default */
            struct netdev* bound = netdev_by_index(oif);
            if (!bound) return -19;
            result.dev = bound; result.nexthop = d;
        }
        dev = result.dev; nexthop = result.nexthop; mtu = result.mtu;
        if (result.type == RTN_LOCAL) dev = netdev_lo;
        if (result.type == RTN_BROADCAST) broadcast = true;
        if (!src) src = ip6a_v4(&result.source);
        if (!src && dev != netdev_lo) { ip4_counters.out_no_routes++; return -99; }
    }
    if (dev == netdev_lo) mtu = 1500;   /* the loopback ring's slots */
    unsigned total = raw ? length : length + 20;
    uint8_t small[1600]; uint8_t* packet = total <= sizeof(small) ? small : (void*)arena_alloc(total); if (!packet) return -105;
    uint16_t id = ip_id++;
    if (raw) {
        memcpy(packet, payload, length); put16(packet + 2, (uint16_t)total);
        if (!be16(packet + 4)) put16(packet + 4, id);
        packet[10] = packet[11] = 0; unsigned ihl = (packet[0] & 15) * 4u;
        if (!be32(packet + 12)) put32(packet + 12, src);
        uint16_t sum = ip_checksum(packet, (uint16_t)ihl); memcpy(packet + 10, &sum, 2);
    } else {
        header(packet, src, dst, protocol, total, id, dontfrag || (df_when_fits && total <= mtu) ? 0x4000 : 0, ttl, tos);
        if (length) memcpy(packet + 20, payload, length);
    }
    int result = filter_out(packet, total, dev, mark, NF_INET_LOCAL_OUT);
    if (result == NF_ACCEPT && be32(packet + 16) != dst) {   /* destination NAT at OUTPUT: route the new destination */
        dst = be32(packet + 16); nexthop = ip6a_from4(dst); multicast = (dst >> 28) == 14; broadcast = dst == 0xFFFFFFFFU;
        if (netdev_v4_local(dst)) { dev = netdev_lo; mtu = 1500; }
        else {
            struct ip6a d = ip6a_from4(dst); struct route_result rr;
            if (route_lookup(AF_INET, &d, NULL, oif, mark, &rr)) result = NF_DROP;
            else { dev = rr.type == RTN_LOCAL ? netdev_lo : rr.dev; nexthop = rr.nexthop; mtu = dev == netdev_lo ? 1500 : rr.mtu; }
        }
    }
    if (result == NF_ACCEPT) result = filter_out(packet, total, dev, mark, NF_INET_POST_ROUTING);
    src = be32(packet + 12); dst = be32(packet + 16);   /* (source NAT): the fragments below carry the final addresses */
    if (result != NF_ACCEPT) { if (packet != small) arena_free((uint64_t)packet, total); ip4_counters.out_discards++; return -1; }
    /* IP_MULTICAST_LOOP (on by default for sockets): local members get a copy, as if it arrived on `dev` */
    if (multicast && options && options->mc_loop && dev != netdev_lo && total <= 1500) {
        struct ip6a group = ip6a_from4(dst);
        if (mc_joined(dev, &group)) { struct netdev* saved = netdev_rx_current; netdev_rx_current = dev; ip_mc_loop_deliver(packet, (uint16_t)total); netdev_rx_current = saved; }
    }
    if (total <= mtu) result = transmit(dev, &nexthop, broadcast, packet, total);
    else if ((be16(packet + 6) & 0x4000) || raw) { ip4_counters.frag_fails++; result = -90; }
    else {
        /* Fragment (RFC 791): 8-byte aligned pieces of the payload, each with the header, MF on all but the last. */
        unsigned piece = (mtu - 20) & ~7U, offset = 0; result = 0;
        while (offset < length && !result) {
            unsigned part = length - offset > piece ? piece : length - offset; bool more = offset + part < length;
            uint8_t fragment[20 + 1500];
            if (part + 20 > sizeof(fragment)) { result = -90; break; }
            header(fragment, src, dst, protocol, part + 20, id, (uint16_t)((more ? 0x2000 : 0) | (offset / 8)), ttl, tos);
            memcpy(fragment + 20, packet + 20 + offset, part);
            result = transmit(dev, &nexthop, broadcast, fragment, part + 20);
            offset += part;
        }
    }
    if (packet != small) arena_free((uint64_t)packet, total);
    if (result && result != -2 && result != -11) ip4_counters.out_discards++;
    return result;
}

/* ---- local destinations ---- */
bool ip4_accept_destination(uint32_t address) {
    if (netdev_v4_local(address)) return true;
    if ((address >> 28) == 14) return mc4_member_any(address);   /* mcast.c */
    for (struct netdev* d = netdevs; d; d = d->next)
        for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && d->v4[i].broadcast == address) return true;
    return false;
}

/* ---- ICMP port unreachable for UDP without a socket (RFC 1122 4.1.3.1), rate limited per destination ---- */
static struct { uint32_t dst; uint64_t next_ns; bool used; } unreachable_limit[16];
void icmp4_port_unreachable(uint32_t source, uint32_t destination, uint16_t source_port, uint16_t destination_port, const void* data, unsigned length) {
    if (!source || (source >> 28) >= 14 || ip_is_local(source) || (destination >> 28) >= 14) return;
    uint64_t now = native_clock_ns(); unsigned slot = source % 16;   /* one error per icmp_ratelimit ms per destination */
    if (unreachable_limit[slot].used && unreachable_limit[slot].dst == source && unreachable_limit[slot].next_ns > now) return;
    unreachable_limit[slot].used = true; unreachable_limit[slot].dst = source;
    unreachable_limit[slot].next_ns = now + (uint64_t)(sysctl_icmp_ratelimit > 0 ? sysctl_icmp_ratelimit : 0) * 1000000ULL;
    unsigned quoted = length > 520 ? 520 : length;
    uint8_t message[8 + 28 + 520] = {0};
    message[0] = 3; message[1] = 3;
    uint8_t* ip = message + 8; ip[0] = 0x45; put16(ip + 2, (uint16_t)(28 + length)); ip[8] = 64; ip[9] = 17; put32(ip + 12, source); put32(ip + 16, destination);
    uint16_t hsum = ip_checksum(ip, 20); memcpy(ip + 10, &hsum, 2);
    uint8_t* udp = ip + 20; put16(udp, source_port); put16(udp + 2, destination_port); put16(udp + 4, (uint16_t)(8 + length));
    if (quoted) memcpy(udp + 8, data, quoted);
    uint16_t sum = ip_checksum(message, (uint16_t)(8 + 28 + quoted)); memcpy(message + 2, &sum, 2);
    ip4_hold_unresolved++; ip4_output(destination, source, IPPROTO_ICMP, message, 8 + 28 + quoted, NULL); ip4_hold_unresolved--;
}

/* ---- input ---- */
void ip4_input_device(struct netdev* dev, const void* packet, unsigned length) {
    if (length < 20 || length > 65535) { dev->stats.rx_errors++; return; }
    if (!(dev->flags & IFF_UP)) { dev->stats.rx_dropped++; return; }
    struct netdev* saved = netdev_rx_current; netdev_rx_current = dev;
    ip_handle_packet(packet, (uint16_t)length);
    netdev_rx_current = saved;
}
/* Fragment reassembly: queues keyed by (source, destination, id, protocol); RFC 5722-style rejection of overlaps;
 * 64 queues and 256 KiB at most; a queue lives 30 s. */
#define REASM_QUEUES 64
#define REASM_BYTES (256U * 1024U)
struct fragment { struct fragment* next; unsigned offset, length; uint8_t data[]; };
struct reasm {
    bool used, last_seen, bad;
    uint32_t src, dst; uint16_t id; uint8_t protocol;
    unsigned total, received, bytes;
    uint8_t header[60]; unsigned header_length;
    struct fragment* fragments;
    uint64_t expires_ns;
    struct netdev* dev;
};
static struct reasm queues[REASM_QUEUES];
static unsigned reasm_bytes;
static void reasm_free(struct reasm* q) {
    while (q->fragments) { struct fragment* f = q->fragments; q->fragments = f->next; reasm_bytes -= sizeof(*f) + f->length; net_free(f, sizeof(*f) + f->length); }
    q->used = false;
}
void ip4_fragment_input(const void* data, unsigned length, bool loopback) {
    const uint8_t* p = data; unsigned ihl = (p[0] & 15) * 4u;
    uint16_t field = be16(p + 6); unsigned offset = (field & 0x1FFF) * 8u, part = length - ihl; bool more = (field & 0x2000) != 0;
    if (field & 0x8000) return;                                   /* reserved bit set */
    if (more && (part & 7)) return;                                /* every fragment but the last is 8-byte aligned */
    if (offset + part > 65535 - ihl || (!part && more)) return;
    uint32_t src = be32(p + 12), dst = be32(p + 16); uint16_t id = be16(p + 4); uint8_t protocol = p[9];
    uint64_t now = native_clock_ns();
    struct reasm* q = NULL, *spare = NULL, *oldest = NULL;
    for (unsigned i = 0; i < REASM_QUEUES; i++) {
        struct reasm* c = &queues[i];
        if (c->used && c->expires_ns <= now) reasm_free(c);
        if (!c->used) { if (!spare) spare = c; continue; }
        if (c->src == src && c->dst == dst && c->id == id && c->protocol == protocol) { q = c; break; }
        if (!oldest || c->expires_ns < oldest->expires_ns) oldest = c;
    }
    if (!q) {
        if (!spare) { reasm_free(oldest); spare = oldest; }   /* evict the oldest incomplete datagram */
        q = spare; memset(q, 0, sizeof(*q)); q->used = true; q->src = src; q->dst = dst; q->id = id; q->protocol = protocol;
        q->expires_ns = now + 30000000000ULL; q->dev = netdev_rx_current;
    }
    if (q->bad) return;
    if (!more) { if (q->last_seen && q->total != offset + part) { q->bad = true; return; } q->last_seen = true; q->total = offset + part; }
    if (q->last_seen && offset + part > q->total) { q->bad = true; return; }
    for (struct fragment* f = q->fragments; f; f = f->next)
        if (offset < f->offset + f->length && f->offset < offset + part) {
            if (f->offset == offset && f->length == part) return;   /* exact duplicate */
            q->bad = true; return;                                   /* overlap: the datagram is discarded */
        }
    if (reasm_bytes + sizeof(struct fragment) + part > REASM_BYTES) { reasm_free(q); return; }
    struct fragment* f = net_alloc(sizeof(*f) + part); if (!f) return;
    f->offset = offset; f->length = part; memcpy(f->data, p + ihl, part);
    struct fragment** link = &q->fragments; while (*link && (*link)->offset < offset) link = &(*link)->next;
    f->next = *link; *link = f; q->received += part; reasm_bytes += sizeof(*f) + part;
    if (!offset) { memcpy(q->header, p, ihl); q->header_length = ihl; }
    if (!q->last_seen || !q->header_length || q->received != q->total) return;
    unsigned whole = q->header_length + q->total;
    uint8_t* packet = (void*)arena_alloc(whole); if (!packet) { reasm_free(q); return; }
    memcpy(packet, q->header, q->header_length);
    for (struct fragment* g = q->fragments; g; g = g->next) memcpy(packet + q->header_length + g->offset, g->data, g->length);
    put16(packet + 2, (uint16_t)whole); put16(packet + 6, 0); packet[10] = packet[11] = 0;
    uint16_t sum = ip_checksum(packet, (uint16_t)q->header_length); memcpy(packet + 10, &sum, 2);
    struct netdev* saved = netdev_rx_current; netdev_rx_current = q->dev;
    reasm_free(q);
    if (loopback) ip_loopback_queue(packet, (uint16_t)whole); else ip_handle_packet(packet, (uint16_t)whole);
    netdev_rx_current = saved;
    arena_free((uint64_t)packet, whole);
}
/* Local delivery hook (kernel/ip.c, after validation): filter INPUT, then copies for raw and ping sockets.
 * True: the packet was dropped. The kernel DHCP client's replies bypass the filter as packet sockets do. */
bool ip4_local_deliver_hook(const void* data, unsigned length, int loopback) {
    const uint8_t* p = data; unsigned ihl = (p[0] & 15) * 4u;
    ip4_counters.in_receives++;
    /* loopback: 1 the loopback device, 2 a multicast loop copy, 3 PREROUTING already done (forward.c ingress),
     * 4 every hook done (a packet INPUT NAT rewrote, delivered again) */
    struct netdev* in = loopback == 1 ? netdev_lo : netdev_rx_current ? netdev_rx_current : netdev_eth0;
    bool dhcp = p[9] == IPPROTO_UDP && length >= ihl + 8 && be16(p + ihl) == 67 && be16(p + ihl + 2) == 68;
    bool nat = nf_nat_active();
    if ((nf_active() || nat) && !dhcp && loopback != 4) {
        uint8_t* copy = NULL;
        if (nat) { copy = (void*)arena_alloc(length); if (!copy) return true; memcpy(copy, p, length); }
        struct nf_packet packet = {.family = AF_INET, .hook = NF_INET_PRE_ROUTING, .data = copy ? copy : p, .writable = copy, .length = length, .in = in,
                                   .l4_proto = p[9], .l4_offset = ihl, .fragment = (be16(p + 6) & 0x1FFF) != 0};
        bool drop = loopback != 3 && nf_hook(&packet) != NF_ACCEPT;
        if (!drop) { packet.hook = NF_INET_LOCAL_IN; drop = nf_hook(&packet) != NF_ACCEPT; }
        if (!drop && copy && packet.nat_changed) { ip_deliver_reassembled(copy, (uint16_t)length, 4); drop = true; }   /* delivered as rewritten */
        if (copy) arena_free((uint64_t)copy, length);
        if (drop) return true;
    }
    struct inet_rx_info info; memset(&info, 0, sizeof(info));
    info.src = ip6a_from4(be32(p + 12)); info.dst = ip6a_from4(be32(p + 16)); info.ifindex = in ? in->index : 1;
    info.hop_limit = p[8]; info.tclass = p[1]; info.multicast = (be32(p + 16) >> 28) == 14 || be32(p + 16) == 0xFFFFFFFFU;
    inet_raw_receive(AF_INET, &info, p[9], p, length, p + ihl, length - ihl);
    if (p[9] == IPPROTO_ICMP && length >= ihl + 8 && p[ihl] == 0 && !ip_checksum(p + ihl, (uint16_t)(length - ihl))) inet_ping_receive(AF_INET, &info, p + ihl, length - ihl);
    if (p[9] == 2 && !loopback && length >= ihl + 8 && !ip_checksum(p + ihl, (uint16_t)(length - ihl))) igmp_receive(in, p + ihl, length - ihl, be32(p + 16));
    ip4_counters.in_delivers++;
    return false;
}
