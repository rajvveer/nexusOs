/* IP forwarding (the host as a router between its interfaces) and the ingress step NAT needs, both families.
 *
 * IPv4 (net.ipv4.ip_forward, conf/<interface>/forwarding of the arrival interface) and IPv6
 * (net.ipv6.conf.all.forwarding + the arrival interface's): a packet for someone else is checked like Linux's
 * ip_forward / ip6_forward - never broadcast, multicast, loopback, link-local or unspecified addresses; strict
 * reverse-path filtering when rp_filter=1 - then its TTL / hop limit (ICMP Time Exceeded at 1), its route (ICMP
 * unreachable: network/host/administratively prohibited by route type), the FORWARD and POSTROUTING hooks (filter,
 * source NAT), the outgoing MTU (IPv4: fragmented, or ICMP Fragmentation Needed with DF; IPv6: Packet Too Big) and is
 * sent to the next hop. Errors quote the packet as received and are rate limited per destination.
 *
 * Ingress (kernel/ip.c, ipv6.c call it for every packet from the wire): when nat chains / NAT bindings exist or the
 * interface forwards, the packet is copied, PREROUTING runs on the copy (destination NAT may rewrite it), and the
 * routing decision is made again on the result: local delivery (PREROUTING already done) or forwarding. With
 * conntrack NAT active, IPv4 fragments are reassembled first (conntrack needs whole datagrams), as Linux's defrag.
 * Everything runs under the kernel lock. */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/string.h"

extern int32_t sysctl_ip_forward, sysctl_icmp_ratelimit;
uint64_t ip4_forwarded, ip6_forwarded;
int ip4_transmit(struct netdev* dev, const struct ip6a* nexthop, const uint8_t* packet, unsigned length);   /* net4.c */
int ip6_transmit(struct netdev* dev, const struct ip6a* nexthop, const uint8_t* packet, unsigned length);   /* ipv6.c */

static bool v4_never_routed(uint32_t a) {
    return !a || a == 0xFFFFFFFFU || (a >> 28) >= 14 || (a >> 24) == 127 || (a >> 24) == 0 || (a >> 16) == 0xA9FE;
}
static bool forwards4(struct netdev* in) { return sysctl_ip_forward && in && in->ipv4_forwarding && in != netdev_lo; }

/* ICMP errors from the router, per destination rate limit (net.ipv4.icmp_ratelimit) */
static struct { uint32_t dst; uint64_t next_ns; bool used; } limit4[16];
static void icmp4_error(struct netdev* in, uint8_t type, uint8_t code, uint32_t info, const uint8_t* packet, unsigned length) {
    if (length < 20) return;
    uint32_t src = be32(packet + 12), dst = be32(packet + 16);
    if (v4_never_routed(src) || (be16(packet + 6) & 0x1FFF)) return;                 /* never about these, or non-first fragments */
    unsigned ihl = (packet[0] & 15) * 4u;
    if (packet[9] == IPPROTO_ICMP && length >= ihl + 1 && (packet[ihl] == 3 || packet[ihl] == 11 || packet[ihl] == 12 || packet[ihl] == 4 || packet[ihl] == 5)) return;   /* errors about errors */
    uint64_t now = native_clock_ns(); unsigned slot = src % 16;
    if (limit4[slot].used && limit4[slot].dst == src && limit4[slot].next_ns > now) return;
    limit4[slot].used = true; limit4[slot].dst = src; limit4[slot].next_ns = now + (uint64_t)(sysctl_icmp_ratelimit > 0 ? sysctl_icmp_ratelimit : 0) * 1000000ULL;
    unsigned quote = length < 548 ? length : 548;                                    /* within 576 bytes (RFC 1812 4.3.2.3) */
    uint8_t body[8 + 548]; memset(body, 0, 8); body[0] = type; body[1] = code; put32(body + 4, info); memcpy(body + 8, packet, quote);
    uint32_t from = in ? netdev_v4_primary(in) : 0; if (!from) from = source_select4(src, NULL);
    (void)dst;
    extern int ip4_hold_unresolved; ip4_hold_unresolved++;
    ip4_output(from, src, IPPROTO_ICMP, body, 8 + quote, NULL);
    ip4_hold_unresolved--;
}
static void nf_packet4(struct nf_packet* p, uint8_t hook, uint8_t* packet, unsigned length, struct netdev* in, struct netdev* out) {
    unsigned ihl = (packet[0] & 15) * 4u;
    p->family = AF_INET; p->hook = hook; p->data = packet; p->writable = packet; p->length = length; p->in = in; p->out = out;
    p->l4_proto = packet[9]; p->l4_offset = ihl; p->fragment = (be16(packet + 6) & 0x1FFF) != 0;
}
/* send `packet` (a complete datagram, TTL already lowered) out of `dev` toward nexthop, fragmenting to `mtu` */
static void send4(struct netdev* dev, const struct ip6a* nexthop, uint8_t* packet, unsigned length, unsigned mtu, struct netdev* in) {
    if (length <= mtu) { ip4_transmit(dev, nexthop, packet, length); return; }
    if (be16(packet + 6) & 0x4000) { icmp4_error(in, 3, 4, mtu & 0xFFFF, packet, length); return; }   /* DF: fragmentation needed */
    unsigned ihl = (packet[0] & 15) * 4u, data = length - ihl, piece = (mtu - ihl) & ~7U, offset = 0;
    uint16_t field = be16(packet + 6); unsigned base = (field & 0x1FFF) * 8u; bool more_after = (field & 0x2000) != 0;
    if (piece < 8) return;
    uint8_t fragment[60 + 1500];
    while (offset < data) {
        unsigned part = data - offset > piece ? piece : data - offset; bool more = offset + part < data || more_after;
        if (ihl + part > sizeof(fragment)) return;
        memcpy(fragment, packet, ihl); memcpy(fragment + ihl, packet + ihl + offset, part);
        put16(fragment + 2, (uint16_t)(ihl + part)); put16(fragment + 6, (uint16_t)((field & 0x4000) | (more ? 0x2000 : 0) | ((base + offset) / 8)));
        fragment[10] = fragment[11] = 0; uint16_t sum = ip_checksum(fragment, (uint16_t)ihl); memcpy(fragment + 10, &sum, 2);
        ip4_transmit(dev, nexthop, fragment, ihl + part); offset += part;
    }
}
static void forward4(struct netdev* in, uint8_t* packet, unsigned length, struct nf_packet* pre) {
    uint32_t src = be32(packet + 12), dst = be32(packet + 16);
    if (v4_never_routed(dst) || v4_never_routed(src)) return;
    for (struct netdev* d = netdevs; d; d = d->next)
        for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && d->v4[i].broadcast == dst) return;   /* directed broadcast */
    struct ip6a s6 = ip6a_from4(src), d6 = ip6a_from4(dst); struct route_result r;
    if (in->rp_filter == 1) {   /* strict reverse path: the route back to the source must use the arrival interface */
        if (route_lookup(AF_INET, &s6, NULL, 0, pre->mark, &r) || r.dev != in) return;
    }
    if (packet[8] <= 1) { icmp4_error(in, 11, 0, 0, packet, length); return; }   /* TTL exceeded in transit */
    if (route_lookup(AF_INET, &d6, NULL, 0, pre->mark, &r) || !r.dev) { icmp4_error(in, 3, 0, 0, packet, length); return; }
    if (r.type == RTN_BLACKHOLE) return;
    if (r.type == RTN_UNREACHABLE) { icmp4_error(in, 3, 1, 0, packet, length); return; }
    if (r.type == RTN_PROHIBIT) { icmp4_error(in, 3, 13, 0, packet, length); return; }
    if (r.type == RTN_LOCAL || r.dev == netdev_lo) return;
    struct netdev* out = r.dev;
    if (!netdev_running(out)) { icmp4_error(in, 3, 1, 0, packet, length); return; }
    /* TTL - 1 with the checksum fixed incrementally (RFC 1624) */
    packet[8]--; uint32_t c = (uint32_t)be16(packet + 10) + 0x0100; c = (c & 0xFFFF) + (c >> 16); put16(packet + 10, (uint16_t)c);
    if (nf_active() || nf_nat_active()) {
        struct nf_packet f; memset(&f, 0, sizeof(f)); nf_packet4(&f, NF_INET_FORWARD, packet, length, in, out);
        f.ct = pre->ct; f.ct_info = pre->ct_info; f.ct_related_dir = pre->ct_related_dir; f.mark = pre->mark; f.untracked = pre->untracked;
        if (nf_hook(&f) != NF_ACCEPT) return;
        f.hook = NF_INET_POST_ROUTING; f.in = NULL; f.nat_changed = false;
        if (nf_hook(&f) != NF_ACCEPT) return;
    }
    unsigned mtu = r.mtu && r.mtu < out->mtu ? r.mtu : out->mtu;
    ip4_forwarded++;
    send4(out, &r.nexthop, packet, length, mtu, in);
}

/* kernel/ip.c, for every IPv4 packet from the wire after header checks: true = handled here. `local`: the plain
 * destination check of ip.c (one of our addresses, a joined group, a broadcast, a DHCP reply). */
bool ip4_ingress(const void* data, unsigned length, bool local) {
    struct netdev* in = netdev_rx_current ? netdev_rx_current : netdev_eth0;
    if (!in || length < 20 || length > 65535) return false;
    bool nat = nf_nat_active(), forwarding = forwards4(in);
    if (!nat && (local || !forwarding)) return false;   /* the host path, unchanged */
    const uint8_t* p = data;
    if (nat && (be16(p + 6) & 0x3FFF)) {                  /* conntrack needs the whole datagram: reassemble first */
        if (local || forwarding) ip4_fragment_input(data, length, false);
        return true;
    }
    uint8_t small[1600]; uint8_t* packet = length <= sizeof(small) ? small : (void*)arena_alloc(length);
    if (!packet) return true;
    memcpy(packet, data, length);
    struct nf_packet pre; memset(&pre, 0, sizeof(pre)); nf_packet4(&pre, NF_INET_PRE_ROUTING, packet, length, in, NULL);
    bool dhcp = p[9] == IPPROTO_UDP && length >= (p[0] & 15) * 4u + 8 && be16(p + (p[0] & 15) * 4u) == 67;
    bool handled = true;
    if (nf_active() && !dhcp && nf_hook(&pre) != NF_ACCEPT) goto done;   /* dropped (rejects were answered) */
    uint32_t dst = be32(packet + 16);
    if (pre.nat_changed) local = ip4_accept_destination(dst) || dst == 0xFFFFFFFFU;
    if (local) ip_deliver_reassembled(packet, (uint16_t)length, 3);      /* PREROUTING done: INPUT and delivery */
    else if (forwarding) forward4(in, packet, length, &pre);
done:
    if (packet != small) arena_free((uint64_t)packet, length);
    return handled;
}

/* ---- IPv6 ---- */
extern struct ipv6_conf* netproc_conf6_all(void);
static bool forwards6(struct netdev* in) {
    struct ipv6_conf* all = netproc_conf6_all();
    return in && in != netdev_lo && in->conf6.forwarding > 0 && (!all || all->forwarding > 0);
}
static unsigned l4_offset6(const uint8_t* p, unsigned length, uint8_t* proto, bool* fragment) {
    uint8_t next = p[6]; unsigned at = 40; *fragment = false;
    for (unsigned n = 0; n < 8; n++) {
        if (next == IPPROTO_HOPOPTS || next == IPPROTO_ROUTING || next == IPPROTO_DSTOPTS) {
            if (at + 8 > length) break;
            next = p[at]; at += (p[at + 1] + 1u) * 8u; continue;
        }
        if (next == IPPROTO_FRAGMENT) { if (at + 8 > length) break; *fragment = (be16(p + at + 2) & 0xFFF8) != 0; next = p[at]; at += 8; continue; }
        break;
    }
    *proto = next; return at <= length ? at : length;
}
static void nf_packet6(struct nf_packet* p, uint8_t hook, uint8_t* packet, unsigned length, struct netdev* in, struct netdev* out) {
    p->family = AF_INET6; p->hook = hook; p->data = packet; p->writable = packet; p->length = length; p->in = in; p->out = out;
    p->l4_offset = l4_offset6(packet, length, &p->l4_proto, &p->fragment);
}
static void forward6(struct netdev* in, uint8_t* packet, unsigned length, struct nf_packet* pre) {
    const struct ip6a* src = (const void*)(packet + 8); struct ip6a dst; memcpy(dst.b, packet + 24, 16);
    if (ip6a_multicast(&dst) || ip6a_any(src) || ip6a_loopback(&dst) || ip6a_multicast(src)) return;
    if (ip6a_linklocal(&dst)) return;
    if (ip6a_linklocal(src)) { icmp6_error(in, 1, 2, 0, packet, length); return; }   /* beyond scope of source address */
    if (packet[7] <= 1) { icmp6_error(in, 3, 0, 0, packet, length); return; }     /* hop limit exceeded in transit */
    struct route_result r;
    if (route_lookup(AF_INET6, &dst, NULL, 0, pre->mark, &r) || !r.dev) { icmp6_error(in, 1, 0, 0, packet, length); return; }
    if (r.type == RTN_BLACKHOLE) return;
    if (r.type == RTN_UNREACHABLE) { icmp6_error(in, 1, 0, 0, packet, length); return; }
    if (r.type == RTN_PROHIBIT) { icmp6_error(in, 1, 1, 0, packet, length); return; }
    if (r.type == RTN_LOCAL || r.dev == netdev_lo) return;
    struct netdev* out = r.dev;
    if (!netdev_running(out)) { icmp6_error(in, 1, 3, 0, packet, length); return; }
    unsigned mtu = r.mtu && r.mtu < out->mtu6 ? r.mtu : out->mtu6;
    if (length > mtu) { icmp6_error(in, 2, 0, mtu, packet, length); return; }   /* routers never fragment IPv6 */
    packet[7]--;
    if (nf_active() || nf_nat_active()) {
        struct nf_packet f; memset(&f, 0, sizeof(f)); nf_packet6(&f, NF_INET_FORWARD, packet, length, in, out);
        f.ct = pre->ct; f.ct_info = pre->ct_info; f.ct_related_dir = pre->ct_related_dir; f.mark = pre->mark; f.untracked = pre->untracked;
        if (nf_hook(&f) != NF_ACCEPT) return;
        f.hook = NF_INET_POST_ROUTING; f.in = NULL;
        if (nf_hook(&f) != NF_ACCEPT) return;
    }
    ip6_forwarded++;
    ip6_transmit(out, r.gateway ? &r.nexthop : &dst, packet, length);
}
/* ipv6.c, for every IPv6 packet from the wire after header checks: true = handled here. */
bool ip6_ingress(struct netdev* in, const uint8_t* data, unsigned length, bool local, void (*deliver)(struct netdev*, uint8_t*, unsigned)) {
    if (!in || length < 40) return false;
    bool nat = nf_nat_active(), forwarding = forwards6(in);
    if (!nat && (local || !forwarding)) return false;
    uint8_t small[1600]; uint8_t* packet = length <= sizeof(small) ? small : (void*)arena_alloc(length);
    if (!packet) return true;
    memcpy(packet, data, length);
    struct nf_packet pre; memset(&pre, 0, sizeof(pre)); nf_packet6(&pre, NF_INET_PRE_ROUTING, packet, length, in, NULL);
    if (!nf_active() || nf_hook(&pre) == NF_ACCEPT) {
        struct ip6a dst; memcpy(dst.b, packet + 24, 16);
        if (pre.nat_changed) local = ip6a_multicast(&dst) ? ipv6_mc_joined(in, &dst) : ip6a_loopback(&dst) || addrconf_local(&dst, ip6a_linklocal(&dst) ? in->index : 0, false);
        if (local) deliver(in, packet, length);                          /* PREROUTING done */
        else if (forwarding) forward6(in, packet, length, &pre);
    }
    if (packet != small) arena_free((uint64_t)packet, length);
    return true;
}
