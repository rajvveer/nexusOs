/* Connection tracking for the nf_tables filter (nft.c). Tracks TCP, UDP, ICMP and ICMPv6 flows by their original
 * and reply tuples and gives each packet an IP_CT_* state (NEW / ESTABLISHED / RELATED / INVALID, +REPLY) that the
 * `ct state` expression reads. ICMP/ICMPv6 error messages that quote a tracked packet are RELATED. TCP gets a small
 * state machine (SYN -> SYN/ACK -> established, FIN/RST shorten the timeout); UDP becomes assured after a reply.
 * Entries expire on a per-protocol timeout (conntrack_poll). Everything runs under the kernel lock, like the hooks.
 *
 * NAT (Linux nf_nat): a connection's first packet meets the nat chains of each NAT hook once (dst manip at
 * PREROUTING/OUTPUT: dnat, redirect; src manip at POSTROUTING/INPUT: snat, masquerade); a binding changes the reply
 * tuple, so replies match it as they arrive. Every packet of a bound connection is then rewritten at each NAT hook
 * toward the inverse of the other direction's tuple (addresses, ports or ICMP ids, IPv4 header and transport
 * checksums incrementally); ICMP errors quoting a NATed flow get their outer and quoted headers translated.
 * Source ports are kept when the reply tuple stays unique, else another port of the range is chosen. Masqueraded
 * connections are dropped when their interface loses the address they were bound to. */
#include "netdev.h"
#include "../../kernel/string.h"
#include "../../kernel/ip.h"

#define CT_MAX 4096
#define IP_CT_ESTABLISHED 0
#define IP_CT_RELATED 1
#define IP_CT_NEW 2
#define IP_CT_IS_REPLY 3            /* ESTABLISHED+REPLY = 3, RELATED+REPLY = 4 */
#define IP_CT_INVALID 255           /* our own marker (Linux returns NF_DROP instead) */

enum { TCP_NONE, TCP_SYN_SENT, TCP_SYN_RECV, TCP_ESTABLISHED, TCP_FIN, TCP_CLOSE };

struct tuple {
    uint8_t family, proto;
    struct ip6a src, dst;
    uint16_t src_port, dst_port;    /* ICMP: id in src_port, type in dst_port */
};
struct conntrack {
    struct conntrack* next;
    struct tuple orig, reply;       /* reply is the expected returning tuple (NAT bindings change it) */
    uint8_t state;                  /* TCP_* for TCP, else unused */
    bool assured, reply_seen, dying;
    uint64_t expires_ns, mark;
    uint32_t id, status;            /* status: Linux IPS_* bits (ct status) */
    unsigned masq_ifindex;          /* masqueraded out of this interface (0: not masquerade) */
    uint64_t packets[2], bytes[2];
};
#define IPS_SEEN_REPLY (1u << 1)
#define IPS_ASSURED (1u << 2)
#define IPS_CONFIRMED (1u << 3)
#define IPS_SRC_NAT (1u << 4)
#define IPS_DST_NAT (1u << 5)
#define IPS_SRC_NAT_DONE (1u << 7)
#define IPS_DST_NAT_DONE (1u << 8)
static struct conntrack* entries;
static unsigned count;
static uint32_t next_id = 1;

static uint32_t timeout_s(const struct conntrack* c) {
    if (c->orig.proto == IPPROTO_TCP)
        return c->state == TCP_ESTABLISHED ? (c->assured ? 432000 : 120) : c->state >= TCP_FIN ? 30 : 120;
    if (c->orig.proto == IPPROTO_UDP) return c->assured ? 120 : 30;
    return 30;                      /* ICMP */
}
static bool tuple_eq(const struct tuple* a, const struct tuple* b) {
    return a->family == b->family && a->proto == b->proto && ip6a_eq(&a->src, &b->src) && ip6a_eq(&a->dst, &b->dst) &&
           a->src_port == b->src_port && a->dst_port == b->dst_port;
}
static struct tuple invert(const struct tuple* t);
/* Which direction of `c` packet tuple `t` belongs to (0 original, 1 reply, -1 none). Between the NAT hooks a packet
 * may be half translated: each endpoint may be in its original or its translated form. */
static bool endpoint_eq(const struct ip6a* a, uint16_t ap, const struct ip6a* b, uint16_t bp) { return ap == bp && ip6a_eq(a, b); }
static int direction_of(const struct conntrack* c, const struct tuple* t) {
    if (c->orig.family != t->family || c->orig.proto != t->proto) return -1;
    for (int d = 0; d < 2; d++) {
        const struct tuple* own = d ? &c->reply : &c->orig; struct tuple target = invert(d ? &c->orig : &c->reply);
        bool src = endpoint_eq(&t->src, t->src_port, &own->src, own->src_port) || endpoint_eq(&t->src, t->src_port, &target.src, target.src_port);
        bool dst = endpoint_eq(&t->dst, t->dst_port, &own->dst, own->dst_port) || endpoint_eq(&t->dst, t->dst_port, &target.dst, target.dst_port);
        if (src && dst) return d;
    }
    return -1;
}
static void ct_free(struct conntrack** link) {
    struct conntrack* c = *link; *link = c->next; count--; net_free(c, sizeof(*c));
}
void conntrack_init(void) { entries = NULL; count = 0; next_id = 1; }
void conntrack_poll(uint64_t now) {
    for (struct conntrack** link = &entries; *link;) {
        if ((*link)->expires_ns <= now) ct_free(link); else link = &(*link)->next;
    }
}
unsigned conntrack_count(void) { return count; }
void conntrack_flush(void) { while (entries) ct_free(&entries); }

/* The L4 identifiers of `p` at l4_offset (ports, or ICMP id/type). False: not enough bytes (treated as INVALID). */
static bool l4_ports(const struct nf_packet* p, uint16_t* sport, uint16_t* dport, bool* icmp_error, struct tuple* embedded);
static bool read_tuple(const struct nf_packet* p, struct tuple* t, bool* icmp_error, struct tuple* embedded) {
    const uint8_t* d = p->data; t->family = p->family; t->proto = p->l4_proto; *icmp_error = false;
    if (p->family == AF_INET) {
        if (p->length < 20) return false;
        t->src = ip6a_from4(be32(d + 12)); t->dst = ip6a_from4(be32(d + 16));
    } else {
        if (p->length < 40) return false;
        memcpy(&t->src, d + 8, 16); memcpy(&t->dst, d + 24, 16);
    }
    return l4_ports(p, &t->src_port, &t->dst_port, icmp_error, embedded);
}
static bool l4_ports(const struct nf_packet* p, uint16_t* sport, uint16_t* dport, bool* icmp_error, struct tuple* embedded) {
    const uint8_t* d = p->data; unsigned off = p->l4_offset;
    if (p->fragment) { *sport = *dport = 0; return true; }   /* non-first fragment: match the tuple without ports */
    if (p->l4_proto == IPPROTO_TCP || p->l4_proto == IPPROTO_UDP) {
        if (off + 4 > p->length) return false;
        *sport = be16(d + off); *dport = be16(d + off + 2); return true;
    }
    if ((p->family == AF_INET && p->l4_proto == IPPROTO_ICMP) || (p->family == AF_INET6 && p->l4_proto == IPPROTO_ICMPV6)) {
        if (off + 8 > p->length) return false;
        uint8_t type = d[off];
        bool v6 = p->family == AF_INET6;
        bool echo = v6 ? (type == 128 || type == 129) : (type == 8 || type == 0);
        bool error = v6 ? (type < 128) : (type == 3 || type == 11 || type == 12 || type == 4);
        if (echo) { *sport = be16(d + off + 4); *dport = (uint16_t)(type == (v6 ? 129 : 0) ? (v6 ? 128 : 8) : type); return true; }
        if (error && embedded) {
            /* the quoted inner packet: build its tuple so the error is matched to the flow that caused it (RELATED) */
            unsigned inner = off + 8; *icmp_error = true;
            struct nf_packet q; memset(&q, 0, sizeof(q)); q.family = p->family; q.data = d + inner; q.length = p->length - inner;
            if (p->family == AF_INET) {
                if (q.length < 20) return false;
                q.l4_proto = q.data[9]; q.l4_offset = (q.data[0] & 15) * 4u;
            } else {
                if (q.length < 40) return false;
                q.l4_proto = q.data[6]; q.l4_offset = 40;
            }
            bool inner_err; return read_tuple(&q, embedded, &inner_err, NULL) && !inner_err;
        }
        *sport = *dport = 0; return true;
    }
    *sport = *dport = 0; return true;   /* other protocols: tracked by addresses only */
}
static struct tuple invert(const struct tuple* t) {
    struct tuple r = {t->family, t->proto, t->dst, t->src, t->dst_port, t->src_port};
    if ((t->family == AF_INET && t->proto == IPPROTO_ICMP) || (t->family == AF_INET6 && t->proto == IPPROTO_ICMPV6)) {
        r.src = t->dst; r.dst = t->src; r.src_port = t->src_port; r.dst_port = t->dst_port;   /* id kept; type already the reply type */
    }
    return r;
}
static void tcp_transition(struct conntrack* c, const struct nf_packet* p, bool reply) {
    if (p->fragment || p->l4_offset + 14 > p->length) return;
    uint8_t flags = p->data[p->l4_offset + 13];
    if (flags & 0x04) { c->state = TCP_CLOSE; return; }                       /* RST */
    if (flags & 0x01) { if (c->state < TCP_FIN) c->state = TCP_FIN; return; } /* FIN */
    if ((flags & 0x12) == 0x12) { if (!reply) ; else c->state = TCP_SYN_RECV; }   /* SYN|ACK from the responder */
    else if (flags & 0x02) c->state = reply ? c->state : TCP_SYN_SENT;        /* SYN */
    else if ((flags & 0x10) && c->state == TCP_SYN_RECV) c->state = TCP_ESTABLISHED;   /* ACK completes the handshake */
}

/* Assign p->ct and p->ct_info. `update` (the first hook a packet meets: PRE_ROUTING / LOCAL_OUT) advances the
 * connection's state and creates an entry for a new flow; a later hook classifies against the existing entry only. */
static void classify(struct nf_packet* p, bool update) {
    uint64_t now = native_clock_ns();
    struct tuple t, embedded; bool icmp_error = false;
    if (!read_tuple(p, &t, &icmp_error, &embedded)) { p->ct = NULL; p->ct_info = IP_CT_INVALID; return; }
    if (icmp_error) {
        for (struct conntrack* c = entries; c; c = c->next) {
            if (c->expires_ns <= now) continue;                                 /* expired: GC pending */
            int d = direction_of(c, &embedded);
            if (d < 0) continue;
            /* the error travels opposite to the packet it quotes */
            p->ct = c; p->ct_info = (uint8_t)(IP_CT_RELATED + (d == 0 ? IP_CT_IS_REPLY : 0)); p->ct_related_dir = (uint8_t)d;
            if (update) { c->packets[!d]++; c->bytes[!d] += p->length; }
            return;
        }
        p->ct = NULL; p->ct_info = IP_CT_INVALID; return;
    }
    for (struct conntrack* c = entries; c; c = c->next) {
        if (c->expires_ns <= now) continue;                                     /* expired: GC pending */
        int d = direction_of(c, &t); bool orig = d == 0, reply = d == 1;
        if (!orig && !reply) continue;
        if (update) {
            if (reply && !c->reply_seen) { c->reply_seen = true; c->status |= IPS_SEEN_REPLY; if (c->orig.proto != IPPROTO_TCP) { c->assured = true; c->status |= IPS_ASSURED; } }
            if (c->orig.proto == IPPROTO_TCP) tcp_transition(c, p, reply);
            c->packets[reply ? 1 : 0]++; c->bytes[reply ? 1 : 0] += p->length;
            c->expires_ns = now + (uint64_t)timeout_s(c) * 1000000000ULL;
        }
        bool established = c->reply_seen && (c->orig.proto != IPPROTO_TCP || c->state >= TCP_ESTABLISHED || c->state == TCP_SYN_RECV);
        p->ct = c; p->ct_info = (uint8_t)((established ? IP_CT_ESTABLISHED : IP_CT_NEW) + (reply ? IP_CT_IS_REPLY : 0));
        return;
    }
    if (!update) { p->ct = NULL; p->ct_info = IP_CT_NEW; return; }   /* not seen at the first hook: treat as new */
    if (count >= CT_MAX) { p->ct = NULL; p->ct_info = IP_CT_NEW; return; }
    struct conntrack* c = net_alloc(sizeof(*c)); if (!c) { p->ct = NULL; p->ct_info = IP_CT_NEW; return; }
    memset(c, 0, sizeof(*c)); c->orig = t; c->reply = invert(&t); c->id = next_id++; c->status = IPS_CONFIRMED;
    if (t.proto == IPPROTO_TCP) tcp_transition(c, p, false);
    c->packets[0] = 1; c->bytes[0] = p->length; c->expires_ns = now + (uint64_t)timeout_s(c) * 1000000000ULL;
    c->next = entries; entries = c; count++;
    p->ct = c; p->ct_info = IP_CT_NEW;
}
void conntrack_track(struct nf_packet* p) { classify(p, true); }
void conntrack_lookup(struct nf_packet* p) { classify(p, false); }
/* The `ct state` bitmask (nft_ct) for p->ct_info. */
uint32_t conntrack_state_bits(const struct nf_packet* p) {
    switch (p->ct_info) {
    case IP_CT_ESTABLISHED: case IP_CT_ESTABLISHED + IP_CT_IS_REPLY: return 1u << 1;
    case IP_CT_RELATED: case IP_CT_RELATED + IP_CT_IS_REPLY: return 1u << 2;
    case IP_CT_NEW: case IP_CT_NEW + IP_CT_IS_REPLY: return 1u << 3;
    case IP_CT_INVALID: return 1u << 0;
    default: return 1u << 0;
    }
}
uint32_t conntrack_direction(const struct nf_packet* p) { return p->ct_info >= IP_CT_IS_REPLY ? 1 : 0; }
uint32_t conntrack_status(const struct nf_packet* p) {
    if (!p->ct) return 0;
    struct conntrack* c = p->ct; uint32_t s = c->status;
    if (c->assured) s |= IPS_ASSURED;
    if (c->reply_seen) s |= IPS_SEEN_REPLY;
    return s;
}
uint32_t conntrack_id(const struct nf_packet* p) { return p->ct ? p->ct->id : 0; }
uint64_t conntrack_counter(const struct nf_packet* p, bool bytes, int direction) {   /* direction -1: both */
    if (!p->ct) return 0;
    const uint64_t* v = bytes ? p->ct->bytes : p->ct->packets;
    return direction < 0 ? v[0] + v[1] : v[direction ? 1 : 0];
}
uint32_t conntrack_expiration_ms(const struct nf_packet* p) {
    uint64_t now = native_clock_ns();
    return p->ct && p->ct->expires_ns > now ? (uint32_t)((p->ct->expires_ns - now) / 1000000ULL) : 0;
}
/* ct original|reply saddr/daddr/proto-src/proto-dst (nft ct keys) */
bool conntrack_tuple_field(const struct nf_packet* p, int direction, unsigned field, uint8_t* out, unsigned* length) {
    if (!p->ct) return false;
    const struct tuple* t = direction ? &p->ct->reply : &p->ct->orig;
    if (field == 0 || field == 1) {
        const struct ip6a* a = field ? &t->dst : &t->src;
        if (t->family == AF_INET) { uint32_t v = __builtin_bswap32(ip6a_v4(a)); memcpy(out, &v, 4); *length = 4; } else { memcpy(out, a->b, 16); *length = 16; }
        return true;
    }
    if (field == 2 || field == 3) { uint16_t v = __builtin_bswap16(field == 2 ? t->src_port : t->dst_port); memcpy(out, &v, 2); *length = 2; return true; }
    if (field == 4) { out[0] = t->proto; *length = 1; return true; }
    if (field == 5) { out[0] = t->family; *length = 1; return true; }
    return false;
}

/* ==================================================================================================== NAT */
/* RFC 1624 incremental update of a 16-bit ones' complement checksum for replacing `from` by `to` (both even length) */
static void csum_replace(uint8_t* sum, const uint8_t* from, const uint8_t* to, unsigned length, bool zero_means_none) {
    uint16_t old = be16(sum);
    if (zero_means_none && !old) return;
    uint32_t c = (uint16_t)~old;
    for (unsigned i = 0; i + 1 < length; i += 2) { c += (uint16_t)~be16(from + i); c += be16(to + i); }
    while (c >> 16) c = (c & 0xFFFF) + (c >> 16);
    uint16_t value = (uint16_t)~c;
    if (zero_means_none && !value) value = 0xFFFF;   /* UDP: 0 would mean "no checksum" */
    put16(sum, value);
}
/* Offsets of the L4 checksum and port/id fields of a packet (IPv4 header length or 40 + extension headers). */
static int l4_layout(uint8_t family, uint8_t proto, const uint8_t* packet, unsigned l4, unsigned length,
                     unsigned* sum_at, unsigned* sport_at, unsigned* dport_at, bool* pseudo, bool* udp) {
    *udp = false; *pseudo = true; *sport_at = *dport_at = 0;
    if (proto == IPPROTO_TCP) { if (l4 + 18 > length) return -1; *sum_at = l4 + 16; *sport_at = l4; *dport_at = l4 + 2; return 0; }
    if (proto == IPPROTO_UDP) { if (l4 + 8 > length) return -1; *sum_at = l4 + 6; *sport_at = l4; *dport_at = l4 + 2; *udp = family == AF_INET; return 0; }
    if ((family == AF_INET && proto == IPPROTO_ICMP) || (family == AF_INET6 && proto == IPPROTO_ICMPV6)) {
        if (l4 + 8 > length) return -1;
        uint8_t type = packet[l4]; bool echo = family == AF_INET ? (type == 8 || type == 0) : (type == 128 || type == 129);
        *sum_at = l4 + 2; *pseudo = family == AF_INET6;
        if (echo) *sport_at = l4 + 4;   /* the identifier plays the port (both directions) */
        return 0;
    }
    *sum_at = 0; return 0;
}
/* Rewrite one endpoint (source or destination) of the IP packet at `packet` (l4 = transport offset). */
static void rewrite_endpoint(uint8_t family, uint8_t* packet, unsigned length, unsigned l4, uint8_t proto, bool source,
                             const struct ip6a* address, uint16_t port, bool fragment) {
    unsigned sum_at = 0, sport_at = 0, dport_at = 0; bool pseudo = true, udp = false;
    bool have_l4 = !fragment && !l4_layout(family, proto, packet, l4, length, &sum_at, &sport_at, &dport_at, &pseudo, &udp);
    if (family == AF_INET) {
        uint8_t* field = packet + (source ? 12 : 16); uint8_t value[4]; put32(value, ip6a_v4(address));
        if (memcmp(field, value, 4)) {
            if (have_l4 && sum_at && pseudo) csum_replace(packet + sum_at, field, value, 4, udp);
            csum_replace(packet + 10, field, value, 4, false);
            memcpy(field, value, 4);
        }
    } else {
        uint8_t* field = packet + (source ? 8 : 24);
        if (memcmp(field, address->b, 16)) {
            if (have_l4 && sum_at && pseudo) csum_replace(packet + sum_at, field, address->b, 16, false);
            memcpy(field, address->b, 16);
        }
    }
    unsigned port_at = source ? sport_at : dport_at;
    if (have_l4 && port_at && be16(packet + port_at) != port) {
        uint8_t value[2]; put16(value, port);
        if (sum_at) csum_replace(packet + sum_at, packet + port_at, value, 2, udp);
        memcpy(packet + port_at, value, 2);
    }
}
/* nat chains must still run for this packet at a hook of this manip type (0 src, 1 dst): a new flow's first packet */
bool conntrack_nat_pending(const struct nf_packet* p, int manip) {
    struct conntrack* c = p->ct;
    if (!c || p->ct_info != IP_CT_NEW) return false;
    return !(c->status & (manip ? IPS_DST_NAT_DONE : IPS_SRC_NAT_DONE));
}
void conntrack_nat_done(struct nf_packet* p, int manip) { if (p->ct) p->ct->status |= manip ? IPS_DST_NAT_DONE : IPS_SRC_NAT_DONE; }
static bool reply_taken(const struct conntrack* self, const struct tuple* reply) {
    for (struct conntrack* c = entries; c; c = c->next) {
        if (c == self) continue;
        if (tuple_eq(&c->reply, reply) || tuple_eq(&c->orig, reply)) return true;
        struct tuple a = invert(&c->orig), b = invert(&c->reply);
        if (tuple_eq(&a, reply) || tuple_eq(&b, reply)) return true;
    }
    return false;
}
static bool ported(const struct tuple* t) {
    return t->proto == IPPROTO_TCP || t->proto == IPPROTO_UDP || (t->family == AF_INET && t->proto == IPPROTO_ICMP) ||
           (t->family == AF_INET6 && t->proto == IPPROTO_ICMPV6);
}
/* Bind the connection of `p`: manip 0 (source) or 1 (destination) to an address in [low, high] (NULL: unchanged) and a
 * port in [port_low, port_high] (0: kept when unique). masq_ifindex: masquerade's interface. 0 or -errno. */
int conntrack_nat_bind(struct nf_packet* p, int manip, const struct ip6a* low, const struct ip6a* high, uint16_t port_low, uint16_t port_high,
                       bool random, unsigned masq_ifindex) {
    struct conntrack* c = p->ct;
    if (!c || p->ct_info != IP_CT_NEW) return -22;
    struct tuple reply = c->reply;
    struct ip6a* address = manip ? &reply.src : &reply.dst; uint16_t* port = manip ? &reply.src_port : &reply.dst_port;
    if (low) {
        *address = *low;
        if (high && !ip6a_eq(low, high)) {   /* a range: spread connections over it (by the original source) */
            unsigned span = ip6a_is4(low) ? ip6a_v4(high) - ip6a_v4(low) + 1 : (unsigned)(high->b[15] - low->b[15] + 1);
            uint32_t h = c->orig.src.b[12] * 131u + c->orig.src.b[13] * 31u + c->orig.src.b[14] * 7u + c->orig.src.b[15] + c->orig.src.b[11];
            unsigned k = span ? h % span : 0;
            if (ip6a_is4(low)) *address = ip6a_from4(ip6a_v4(low) + k); else address->b[15] = (uint8_t)(low->b[15] + k);
        }
    }
    bool is_icmp = (reply.proto == IPPROTO_ICMP && reply.family == AF_INET) || (reply.proto == IPPROTO_ICMPV6 && reply.family == AF_INET6);
    if (ported(&reply) && (port_low || (!manip && reply_taken(c, &reply)))) {
        /* the port (ICMP: the echo id) to use: the requested range, else keep it if unique, else search like Linux */
        uint16_t original = is_icmp ? reply.src_port : *port;
        uint16_t lo = port_low, hi = port_high ? port_high : port_low;
        if (!lo) { lo = is_icmp ? 0 : original < 1024 ? 600 : 1024; hi = is_icmp ? 65535 : original < 1024 ? 1023 : 65535; }
        if (hi < lo) { uint16_t x = lo; lo = hi; hi = x; }
        uint32_t span = (uint32_t)hi - lo + 1, start = random ? (uint32_t)native_tsc() % span : (original >= lo && original <= hi ? original - lo : (uint32_t)(c->id * 2654435761u) % span);
        bool found = false;
        for (uint32_t i = 0; i < span && i < 65536; i++) {
            uint16_t candidate = (uint16_t)(lo + (start + i) % span);
            if (is_icmp) reply.src_port = candidate; else *port = candidate;
            if (!manip && !reply_taken(c, &reply)) { found = true; break; }
            if (manip) { found = true; break; }
        }
        if (!found) return -28;   /* every port of the range is in use */
    }
    c->reply = reply;
    if (manip) c->status |= IPS_DST_NAT | IPS_DST_NAT_DONE; else c->status |= IPS_SRC_NAT | IPS_SRC_NAT_DONE;
    if (masq_ifindex) c->masq_ifindex = masq_ifindex;
    return 0;
}
/* Translate the packet (its header in `packet`) of a tracked connection at a hook of manip type `manip`. */
static bool translate_header(const struct tuple* to, int direction, int manip, uint8_t family, uint8_t* packet, unsigned length,
                             unsigned l4, uint8_t proto, bool fragment) {
    struct tuple target = *to, now;
    now.family = family; now.proto = proto;
    if (family == AF_INET) { now.src = ip6a_from4(be32(packet + 12)); now.dst = ip6a_from4(be32(packet + 16)); }
    else { memcpy(now.src.b, packet + 8, 16); memcpy(now.dst.b, packet + 24, 16); }
    unsigned sum_at, sport_at, dport_at; bool pseudo, udp;
    now.src_port = now.dst_port = 0;
    if (!fragment && !l4_layout(family, proto, packet, l4, length, &sum_at, &sport_at, &dport_at, &pseudo, &udp)) {
        if (sport_at) now.src_port = be16(packet + sport_at);
        if (dport_at) now.dst_port = be16(packet + dport_at);
    }
    bool is_icmp = (family == AF_INET && proto == IPPROTO_ICMP) || (family == AF_INET6 && proto == IPPROTO_ICMPV6);
    if (manip) {
        uint16_t want = is_icmp ? target.src_port : target.dst_port;   /* ICMP: the id lives in "src_port" both ways */
        bool port_change = is_icmp ? false : now.dst_port != want;
        if (ip6a_eq(&now.dst, &target.dst) && !port_change) return false;
        rewrite_endpoint(family, packet, length, l4, proto, false, &target.dst, is_icmp ? 0 : want, fragment);
        if (is_icmp && sport_at && now.src_port != target.src_port && direction == 1) rewrite_endpoint(family, packet, length, l4, proto, true, &now.src, target.src_port, fragment);
    } else {
        bool port_change = now.src_port != target.src_port;
        if (ip6a_eq(&now.src, &target.src) && !port_change) return false;
        rewrite_endpoint(family, packet, length, l4, proto, true, &target.src, target.src_port, fragment);
    }
    return true;
}
static unsigned header_l4(uint8_t family, const uint8_t* packet, unsigned length, uint8_t* proto) {
    if (family == AF_INET) { if (length < 20) return 0; *proto = packet[9]; return (packet[0] & 15) * 4u; }
    if (length < 40) return 0;
    uint8_t next = packet[6]; unsigned at = 40;
    for (unsigned n = 0; n < 8 && (next == 0 || next == 43 || next == 60); n++) {
        if (at + 8 > length) return 0;
        next = packet[at]; at += (packet[at + 1] + 1u) * 8u;
    }
    *proto = next; return at <= length ? at : 0;
}
/* All of `p`'s translation at this hook. `packet` is the writable copy of p->data. True: something changed. */
bool conntrack_nat_translate(struct nf_packet* p, uint8_t* packet, unsigned length, int manip) {
    struct conntrack* c = p->ct;
    if (!c || !(c->status & (IPS_SRC_NAT | IPS_DST_NAT))) return false;
    uint8_t info = p->ct_info;
    if (info == IP_CT_RELATED || info == IP_CT_RELATED + IP_CT_IS_REPLY) {
        /* an ICMP error about a NATed flow: the outer header goes like the error's own direction, the quoted packet
         * (which travelled the other way) gets the opposite manip */
        int quoted = p->ct_related_dir, outer = !quoted;
        struct tuple outer_target = invert(outer ? &c->orig : &c->reply);   /* the quoted packet goes back to its sender form */
        bool changed = translate_header(&outer_target, outer, manip, p->family, packet, length, p->l4_offset, p->l4_proto, false);
        unsigned inner = p->l4_offset + 8; if (inner >= length) return changed;
        uint8_t inner_proto = 0; unsigned inner_l4 = header_l4(p->family, packet + inner, length - inner, &inner_proto);
        if (!inner_l4) return changed;
        uint8_t* q = packet + inner; unsigned qlen = length - inner;
        uint8_t before[128]; unsigned span = qlen < 128 ? qlen : 128; memcpy(before, q, span);
        bool inner_changed = translate_header(quoted ? &c->reply : &c->orig, quoted, !manip, p->family, q, qlen, inner_l4, inner_proto, false);
        if (inner_changed) {
            /* the ICMP checksum covers the quoted packet: fold the quoted header change in */
            unsigned even = span & ~1u;
            csum_replace(packet + p->l4_offset + 2, before, q, even, false);
            changed = true;
        }
        return changed;
    }
    int direction = info >= IP_CT_IS_REPLY ? 1 : 0;
    struct tuple target = invert(direction ? &c->orig : &c->reply);
    return translate_header(&target, direction, manip, p->family, packet, length, p->l4_offset, p->l4_proto, p->fragment);
}
/* An interface lost the address masquerade bound to: its connections go (Linux masquerade notifier). */
void conntrack_flush_masquerade(unsigned ifindex, const struct ip6a* address) {
    for (struct conntrack** link = &entries; *link;) {
        struct conntrack* c = *link;
        if (c->masq_ifindex == ifindex && (!address || ip6a_eq(&c->reply.dst, address))) ct_free(link); else link = &c->next;
    }
}
bool conntrack_any_nat(void) {
    for (struct conntrack* c = entries; c; c = c->next) if (c->status & (IPS_SRC_NAT | IPS_DST_NAT)) return true;
    return false;
}
uint64_t conntrack_mark(const struct nf_packet* p) { return p->ct ? p->ct->mark : 0; }
void conntrack_set_mark(struct nf_packet* p, uint64_t value) { if (p->ct) p->ct->mark = value; }

/* /proc/net/nf_conntrack */
static void add(char* line, unsigned* n, const char* text) { strcpy(line + *n, text); *n += strlen(text); }
static void add_num(char* line, unsigned* n, uint32_t v) { char t[16]; uint_to_str(v, t); add(line, n, t); }
static void add_addr(char* line, unsigned* n, const struct ip6a* a) {
    char text[64]; if (ip6a_is4(a)) ip_to_string(ip6a_v4(a), text); else ip6a_text(a, text); add(line, n, text);
}
int conntrack_proc_text(char* out, unsigned capacity) {
    unsigned used = 0; uint64_t now = native_clock_ns();
    for (struct conntrack* c = entries; c; c = c->next) {
        char line[320]; unsigned n = 0;
        const char* proto = c->orig.proto == IPPROTO_TCP ? "tcp" : c->orig.proto == IPPROTO_UDP ? "udp" :
                            c->orig.family == AF_INET6 ? "icmpv6" : "icmp";
        add(line, &n, c->orig.family == AF_INET6 ? "ipv6 10 " : "ipv4 2 "); add(line, &n, proto); add(line, &n, " ");
        add_num(line, &n, c->orig.proto); add(line, &n, " ");
        add_num(line, &n, c->expires_ns > now ? (uint32_t)((c->expires_ns - now) / 1000000000ULL) : 0);
        for (int d = 0; d < 2; d++) {
            const struct tuple* t = d ? &c->reply : &c->orig;
            add(line, &n, " src="); add_addr(line, &n, &t->src); add(line, &n, " dst="); add_addr(line, &n, &t->dst);
            if (t->proto == IPPROTO_TCP || t->proto == IPPROTO_UDP) {
                add(line, &n, " sport="); add_num(line, &n, t->src_port); add(line, &n, " dport="); add_num(line, &n, t->dst_port);
            } else if (t->proto == IPPROTO_ICMP || t->proto == IPPROTO_ICMPV6) { add(line, &n, " id="); add_num(line, &n, t->src_port); }
            if (!d && !c->reply_seen) add(line, &n, " [UNREPLIED]");
        }
        if (c->assured) add(line, &n, " [ASSURED]");
        add(line, &n, " mark="); add_num(line, &n, (uint32_t)c->mark); add(line, &n, "\n");
        if (used + n + 1 > capacity) break;
        memcpy(out + used, line, n + 1); used += n;
    }
    return (int)used;
}
