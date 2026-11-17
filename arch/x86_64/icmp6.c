/* ICMPv6 (RFC 4443) and MLDv2 (RFC 3810, with MLDv1 compatibility, RFC 2710).
 * Receive: checksum, echo (requests answered from a unicast address, also for multicast destinations, unless
 * echo_ignore_all), errors routed to the transport (UDP/raw/ping sockets in inet.c, TCP path MTU and hard errors
 * in tcp.c) and to the path-MTU cache, NDP to ndisc.c, MLD queries (validated: link-local source, hop limit 1,
 * Router Alert) and other listeners' MLDv1 reports to mcast.c. Send: errors follow RFC 4443 2.4(e) (never about
 * errors, redirects, unspecified/multicast/anycast sources, or multicast destinations except Packet Too Big and
 * code-2 parameter problems) and 2.4(f) (a token bucket per destination plus a global one,
 * net.ipv6.icmp.ratelimit). */
#include "netdev.h"
#include "../../kernel/tcp.h"
#include "../../kernel/string.h"

struct icmp6_stats icmp6_counters;

/* ---- sending ---- */
int icmp6_send(const struct ip6a* dst, const struct ip6a* src, struct netdev* dev, uint8_t type, uint8_t code,
               const void* body, unsigned body_length, int hop_limit) {
    if (body_length > 65535 - 4) return -90;
    uint8_t small[1280]; uint8_t* message = 4 + body_length <= sizeof(small) ? small : (void*)arena_alloc(4 + body_length); if (!message) return -105;
    message[0] = type; message[1] = code; message[2] = message[3] = 0;
    if (body_length) memcpy(message + 4, body, body_length);
    struct ip6a source;
    if (src) source = *src;
    else if (!source_select6(dst, dev, 0, &source)) source = ip6a_unspecified;
    uint16_t sum = ip6_checksum(&source, dst, IPPROTO_ICMPV6, message, 4 + body_length); put16(message + 2, sum);
    struct ip6_send_options o; ip6_options_default(&o);
    o.hop_limit = hop_limit; o.source = &source; o.oif = dev ? dev->index : 0;
    o.no_route_mc = ip6a_multicast(dst) && dev != NULL; o.mc_loop = false;
    if (type == 130 || type == 131 || type == 132 || type == 143) {
        /* MLD: hop-by-hop Router Alert (value 0) */
        static const uint8_t alert[8] = {0, 0, 5, 2, 0, 0, 1, 0};
        o.ext = alert; o.ext_length = 8; o.ext_first = IPPROTO_HOPOPTS;
    }
    int result = ip6_send(dst, IPPROTO_ICMPV6, message, 4 + body_length, &o);
    if (message != small) arena_free((uint64_t)message, 4 + body_length);
    if (!result) { icmp6_counters.out_msgs++; icmp6_counters.out_type[type]++; } else icmp6_counters.out_errors++;
    return result;
}
/* Rate limiting (RFC 4443 2.4(f)): token buckets in nanoseconds of credit - per destination one error per
 * net.ipv6.icmp.ratelimit ms with a burst of 6, and globally 1000 per second with a burst of 50. */
struct bucket { struct ip6a dst; uint64_t credit_ns, last_ns; bool used; };
static struct bucket buckets[32];
static struct bucket global_bucket;
static bool take(struct bucket* b, uint64_t now, uint64_t period, uint64_t burst) {
    if (!b->last_ns) { b->credit_ns = burst; b->last_ns = now; }
    uint64_t gained = now - b->last_ns; b->last_ns = now;
    b->credit_ns = gained > burst - b->credit_ns ? burst : b->credit_ns + gained;
    if (b->credit_ns < period) return false;
    b->credit_ns -= period; return true;
}
static bool rate_allow(const struct ip6a* dst) {
    uint64_t now = native_clock_ns(), period = (uint64_t)(sysctl_icmpv6_ratelimit > 0 ? sysctl_icmpv6_ratelimit : 0) * 1000000ULL;
    if (!period) return true;
    unsigned slot = 0; bool found = false;
    for (unsigned i = 0; i < 32; i++) {
        if (buckets[i].used && ip6a_eq(&buckets[i].dst, dst)) { slot = i; found = true; break; }
        if (!buckets[i].used || (buckets[slot].used && buckets[i].last_ns < buckets[slot].last_ns)) slot = i;
    }
    if (!found) { memset(&buckets[slot], 0, sizeof(buckets[slot])); buckets[slot].used = true; buckets[slot].dst = *dst; }
    if (!take(&global_bucket, now, 1000000ULL, 50000000ULL)) return false;
    return take(&buckets[slot], now, period, 6 * period);
}
static bool upper_is_error(const uint8_t* packet, unsigned length) {
    uint8_t next = packet[6]; unsigned at = 40, depth = 0;
    while ((next == IPPROTO_HOPOPTS || next == IPPROTO_DSTOPTS || next == IPPROTO_ROUTING || next == IPPROTO_FRAGMENT) && depth++ < 16) {
        if (length < at + 8) return false;
        if (next == IPPROTO_FRAGMENT) { if (be16(packet + at + 2) & 0xFFF8) return false; next = packet[at]; at += 8; continue; }
        next = packet[at]; at += (packet[at + 1] + 1u) * 8u;
    }
    if (next != IPPROTO_ICMPV6 || length < at + 1) return false;
    return packet[at] < 128 || packet[at] == 137;   /* errors and redirects */
}
void icmp6_error(struct netdev* dev, uint8_t type, uint8_t code, uint32_t info, const uint8_t* packet, unsigned length) {
    if (!packet || length < 40 || !dev) return;
    const struct ip6a* src = (const void*)(packet + 8); const struct ip6a* dst = (const void*)(packet + 24);
    if (ip6a_any(src) || ip6a_multicast(src)) return;
    bool multicast_ok = type == 2 || (type == 4 && code == 2);
    if (ip6a_multicast(dst) && !multicast_ok) return;
    if (upper_is_error(packet, length)) return;
    if (!rate_allow(src)) { icmp6_counters.out_rate_limited++; return; }
    struct ip6a source; const struct ip6a* from = NULL;
    if (!ip6a_multicast(dst) && addrconf_local(dst, ip6a_linklocal(dst) ? dev->index : 0, false)) { source = *dst; from = &source; }
    unsigned quoted = length > 1232 ? 1232 : length;
    uint8_t body[4 + 1232]; put32(body, info); memcpy(body + 4, packet, quoted);
    icmp6_send(src, from, ip6a_linklocal(src) ? dev : NULL, type, code, body, 4 + quoted, -1);
}

/* ---- errors received: tell the transport ---- */
static void transport_error(struct netdev* dev, const struct ip6a* reporter, uint8_t type, uint8_t code, uint32_t info,
                            const uint8_t* inner, unsigned length) {
    if (length < 40 || (inner[0] >> 4) != 6) return;
    const struct ip6a* isrc = (const void*)(inner + 8); const struct ip6a* idst = (const void*)(inner + 24);
    if (!ip6a_loopback(isrc) && !addrconf_local(isrc, ip6a_linklocal(isrc) ? dev->index : 0, true)) return;   /* not a packet of ours */
    if (type == 2) {
        unsigned mtu = info < 1280 ? 1280 : info;
        pmtu_update(AF_INET6, idst, mtu);
    }
    uint8_t next = inner[6]; unsigned at = 40, depth = 0;
    while ((next == IPPROTO_HOPOPTS || next == IPPROTO_DSTOPTS || next == IPPROTO_ROUTING || next == IPPROTO_FRAGMENT) && depth++ < 16) {
        if (length < at + 8) break;
        if (next == IPPROTO_FRAGMENT) { if (be16(inner + at + 2) & 0xFFF8) return; next = inner[at]; at += 8; continue; }
        next = inner[at]; at += (inner[at + 1] + 1u) * 8u;
    }
    if (next == IPPROTO_TCP && length >= at + 8) {
        uint32_t local = ip6h_find(isrc, ip6_scope_of(dev, isrc)), remote = ip6h_find(idst, ip6_scope_of(dev, idst));
        if (!local || !remote) return;
        uint16_t lport = be16(inner + at), rport = be16(inner + at + 2); uint32_t seq = be32(inner + at + 4);
        if (type == 2) tcp_pmtu_update(local, lport, remote, rport, info < 1280 ? 1280 : info);
        else if (type == 1) {
            int error = code == 0 ? 101 : code == 1 ? 13 : code == 3 ? 113 : code == 4 ? 111 : code == 5 || code == 6 ? 13 : 113;
            tcp_icmp_error(local, lport, remote, rport, seq, error);
        } else if (type == 4) tcp_icmp_error(local, lport, remote, rport, seq, 71);   /* EPROTO */
        /* Time Exceeded is a soft error for TCP (Linux keeps retrying) */
        return;
    }
    inet_icmp6_error(reporter, type, code, info, inner, length);
}

/* A packet of ours died before leaving (neighbour resolution failed): the sender learns it as from an ICMPv6 error
 * of the local node (Linux ndisc_error_report sends "address unreachable" back to the local socket). */
void icmp6_local_error(struct netdev* dev, uint8_t type, uint8_t code, uint32_t info, const uint8_t* packet, unsigned length) {
    if (!packet || length < 40) return;
    const struct ip6a* src = (const void*)(packet + 8);
    transport_error(dev, src, type, code, info, packet, length);
}

/* ---- MLD: the protocol lives in mcast.c (with IGMP); reports leave from a valid link-local address ---- */
void mld_report_source(struct netdev* dev, struct ip6a* source) {
    for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next)
        if (ip6a_linklocal(&a->addr) && !(a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED))) { *source = a->addr; return; }
    *source = ip6a_unspecified;   /* RFC 3590: no valid link-local address yet */
}

/* ---- receive ---- */
void icmp6_receive(struct netdev* dev, const struct ip6a* src, const struct ip6a* dst, uint8_t hop_limit, const uint8_t* m,
                   unsigned length, const uint8_t* packet, unsigned packet_length, bool router_alert) {
    icmp6_counters.in_msgs++;
    if (length < 4) { icmp6_counters.in_errors++; return; }
    if (ip6_checksum(src, dst, IPPROTO_ICMPV6, m, length)) { icmp6_counters.in_csum_errors++; icmp6_counters.in_errors++; return; }
    uint8_t type = m[0], code = m[1];
    icmp6_counters.in_type[type]++;
    struct inet_rx_info info; memset(&info, 0, sizeof(info));
    info.src = *src; info.dst = *dst; info.ifindex = dev->index; info.hop_limit = hop_limit; info.multicast = ip6a_multicast(dst);
    switch (type) {
    case 128: {
        if (sysctl_icmpv6_echo_ignore_all || length < 8 || code) return;
        struct ip6a source; const struct ip6a* from = NULL;
        if (!ip6a_multicast(dst)) { source = *dst; from = &source; }
        icmp6_send(src, from, ip6a_linklocal(src) || ip6a_multicast(dst) ? dev : NULL, 129, 0, m + 4, length - 4, -1);
        return;
    }
    case 129: if (length >= 8) inet_ping_receive(AF_INET6, &info, m, length); return;
    case 1: case 2: case 3: case 4:
        if (length < 8 + 40) return;
        transport_error(dev, src, type, code, be32(m + 4), m + 8, length - 8);
        return;
    case 130:
        if (hop_limit != 1 || !router_alert) return;
        mld_query(dev, src, dst, m, length);
        return;
    case 131: if (hop_limit == 1) mld_heard_report(dev, m, length); return;   /* another listener's MLDv1 report */
    case 132: case 143: return;   /* other listeners' Done and MLDv2 reports */
    case 133: case 134: case 135: case 136: case 137:
        ndisc_receive(dev, src, dst, hop_limit, m, length, false);
        return;
    default: return;
    }
    (void)packet; (void)packet_length;
}
