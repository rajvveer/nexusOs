/* IPv6 (RFC 8200) for the native kernel.
 * Input: header validation (version, lengths, martian sources, multicast scope), destination check (our unicast
 * addresses on any interface - tentative ones excluded - and the groups the interface joined), the extension-header
 * chain (hop-by-hop only first, destination options, routing with segments left 0, fragment) with TLV option
 * processing (Pad1/PadN limits, Router Alert, the action bits of unknown options), bounds on the chain length and
 * count, reassembly (RFC 5722 overlap discard, RFC 6946 atomic fragments, RFC 7112 complete header chain, 60 s
 * timeout with ICMPv6 Time Exceeded, 64 queues / 256 KiB), the netfilter PREROUTING/INPUT hooks, then raw sockets
 * and ICMPv6, UDP (DHCPv6 client, sockets, port unreachable) or TCP through IPv6 address handles.
 * Output: routing and source selection (route.c), hop limit, the OUTPUT/POSTROUTING hooks, fragmentation to the
 * path MTU (unless IPV6_DONTFRAG), loopback through kernel/ip.c's loopback ring, neighbour resolution (ndisc.c)
 * or point-to-point transmission.
 * IPv6 address handles: TCP (kernel/tcp.c) keeps IPv4-sized endpoint values; for IPv6 they are handles from the
 * reserved block 240.0.0.0/4 naming (address, scope id). They are collected when no TCB names them (2 s grace). */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/tcp.h"
#include "../../kernel/string.h"

struct ipv6_stats ip6_stats;

/* ---- address handles ---- */
#define HANDLES_MAX 8192
#define HANDLE_BUCKETS 256
struct handle { struct handle* next_value; struct handle* next_address; uint32_t value, scope; struct ip6a address; uint64_t stamp_ns; bool marked; };
static struct handle* by_value[HANDLE_BUCKETS];
static struct handle* by_address[HANDLE_BUCKETS];
static unsigned handle_count;
static uint32_t next_handle = IP6H_FIRST;
static unsigned address_bucket(const struct ip6a* a, uint32_t scope) {
    uint32_t h = scope * 2654435761U;
    for (unsigned i = 0; i < 16; i++) h = (h ^ a->b[i]) * 16777619U;
    return h % HANDLE_BUCKETS;
}
static struct handle* handle_lookup(uint32_t value) {
    for (struct handle* h = by_value[value % HANDLE_BUCKETS]; h; h = h->next_value) if (h->value == value) return h;
    return NULL;
}
uint32_t ip6h_find(const struct ip6a* address, uint32_t scope) {
    for (struct handle* h = by_address[address_bucket(address, scope)]; h; h = h->next_address)
        if (h->scope == scope && ip6a_eq(&h->address, address)) { h->stamp_ns = native_clock_ns(); return h->value; }
    return 0;
}
uint32_t ip6h_get(const struct ip6a* address, uint32_t scope) {
    uint32_t found = ip6h_find(address, scope); if (found) return found;
    if (handle_count >= HANDLES_MAX) { ip6h_gc(native_clock_ns(), true); if (handle_count >= HANDLES_MAX) return 0; }
    struct handle* h = net_alloc(sizeof(*h)); if (!h) return 0;
    do { h->value = next_handle++; if (next_handle > IP6H_LAST) next_handle = IP6H_FIRST; } while (handle_lookup(h->value));
    h->address = *address; h->scope = scope; h->stamp_ns = native_clock_ns(); h->marked = false;
    h->next_value = by_value[h->value % HANDLE_BUCKETS]; by_value[h->value % HANDLE_BUCKETS] = h;
    unsigned b = address_bucket(address, scope); h->next_address = by_address[b]; by_address[b] = h;
    handle_count++;
    return h->value;
}
bool ip6h_address(uint32_t value, struct ip6a* address, uint32_t* scope) {
    struct handle* h = ip6h_is(value) ? handle_lookup(value) : NULL;
    if (!h) return false;
    *address = h->address; if (scope) *scope = h->scope; return true;
}
unsigned ip6h_count(void) { return handle_count; }
static void mark_handle(uint32_t value) { struct handle* h = ip6h_is(value) ? handle_lookup(value) : NULL; if (h) h->marked = true; }
static void mark_connection(const tcp_conn_t* c, void* context) {
    (void)context; mark_handle(c->local_ip); mark_handle(c->remote_ip); mark_handle(c->listen_ip);
}
void ip6h_gc(uint64_t now, bool force) {
    static uint64_t next; if (!force && now < next) return;
    next = now + 1000000000ULL;
    if (!handle_count) return;
    tcp_foreach(mark_connection, NULL);
    for (unsigned b = 0; b < HANDLE_BUCKETS; b++)
        for (struct handle** link = &by_value[b]; *link;) {
            struct handle* h = *link;
            if (h->marked || (!force && now - h->stamp_ns < 2000000000ULL)) { h->marked = false; link = &h->next_value; continue; }
            *link = h->next_value;
            unsigned a = address_bucket(&h->address, h->scope);
            for (struct handle** al = &by_address[a]; *al; al = &(*al)->next_address) if (*al == h) { *al = h->next_address; break; }
            net_free(h, sizeof(*h)); handle_count--;
        }
}

/* ---- checksums ---- */
uint16_t ip6_checksum_parts(const struct ip6a* src, const struct ip6a* dst, uint8_t next, const void* header, unsigned header_length,
                            const void* data, unsigned length) {
    uint32_t total = header_length + length, sum = 0;
    sum = net_sum(sum, src->b, 16); sum = net_sum(sum, dst->b, 16);
    sum += total >> 16; sum += total & 0xFFFF; sum += next;
    if (header_length & 1) {
        /* odd header: combine through a bounce so the data continues on the right byte */
        uint8_t joined[64]; if (header_length > sizeof(joined) - 1) return 0;
        memcpy(joined, header, header_length);
        unsigned take = length ? 1 : 0; if (take) joined[header_length] = ((const uint8_t*)data)[0];
        sum = net_sum(sum, joined, header_length + take);
        return net_fold(net_sum(sum, (const uint8_t*)data + take, length - take));
    }
    sum = net_sum(sum, header, header_length);
    return net_fold(net_sum(sum, data, length));
}
uint16_t ip6_checksum(const struct ip6a* src, const struct ip6a* dst, uint8_t next, const void* data, unsigned length) {
    return ip6_checksum_parts(src, dst, next, NULL, 0, data, length);
}
/* tcp.c: the IPv6 pseudo-header checksum of a segment between two address handles (0xFFFF when one is unknown,
 * which never verifies). tcp.c stores the value as a native uint16_t (like ip_checksum), so it is byte-swapped. */
uint16_t tcp_checksum6(uint32_t src, uint32_t dst, const void* data, uint16_t length) {
    struct ip6a s, d;
    if (!ip6h_address(src, &s, NULL) || !ip6h_address(dst, &d, NULL)) return 0xFFFF;
    return __builtin_bswap16(ip6_checksum(&s, &d, IPPROTO_TCP, data, length));
}

/* ---- multicast membership ---- */
bool ipv6_mc_joined(struct netdev* dev, const struct ip6a* group) {
    if (ip6a_eq(group, &ip6a_all_nodes)) return true;
    for (struct mc6_group* g = dev->mc6; g; g = g->next) if (ip6a_eq(&g->group, group) && !g->leaving) return true;
    return false;
}

/* ---- output ---- */
void ip6_options_default(struct ip6_send_options* o) { memset(o, 0, sizeof(*o)); o->hop_limit = -1; o->tclass = -1; o->mc_loop = true; }
static uint32_t fragment_id(void) { uint32_t id; do id = net_random32(); while (!id); return id; }
static int transmit6(struct netdev* dev, const struct ip6a* nexthop, const uint8_t* packet, unsigned length) {
    if (dev == netdev_lo) {
        int result = ip_loopback_queue(packet, (uint16_t)length);
        if (!result) { dev->stats.tx_packets++; dev->stats.tx_bytes += length; dev->stats.rx_packets++; dev->stats.rx_bytes += length; }
        return result;
    }
    if (dev->type == ARPHRD_NONE) return netdev_xmit(dev, packet, length, ETH_TYPE_IPV6);
    if (dev->type != ARPHRD_ETHER) return -101;
    const struct ip6a* dst = (const void*)(packet + 24);
    if (ip6a_multicast(dst)) {
        if (length > 1500) return -90;
        uint8_t frame[ETH_HEADER_LEN + 1500];
        frame[0] = 0x33; frame[1] = 0x33; memcpy(frame + 2, dst->b + 12, 4); memcpy(frame + 6, dev->addr, 6); put16(frame + 12, ETH_TYPE_IPV6);
        memcpy(frame + ETH_HEADER_LEN, packet, length);
        ip6_stats.out_mcast++;
        return netdev_xmit(dev, frame, ETH_HEADER_LEN + length, ETH_TYPE_IPV6);
    }
    return neigh_output(dev, AF_INET6, nexthop, packet, length, ETH_TYPE_IPV6);
}
static int filter6(const uint8_t* packet, unsigned length, struct netdev* in, struct netdev* out, uint8_t hook, unsigned l4_offset,
                   uint8_t l4_proto, bool fragment, uint32_t mark) {
    if (!nf_active()) return NF_ACCEPT;
    struct nf_packet p = {.family = AF_INET6, .hook = hook, .data = packet, .length = length, .in = in, .out = out, .mark = mark,
                          .l4_offset = l4_offset, .l4_proto = l4_proto, .fragment = fragment};
    return nf_hook(&p);
}
/* the output hooks on the packet being built: NAT may rewrite it */
static int filter6_out(uint8_t* packet, unsigned length, struct netdev* out, uint8_t hook, unsigned l4_offset, uint8_t l4_proto, uint32_t mark) {
    if (!nf_active() && !nf_nat_active()) return NF_ACCEPT;
    struct nf_packet p = {.family = AF_INET6, .hook = hook, .data = packet, .writable = packet, .length = length, .out = out, .mark = mark,
                          .l4_offset = l4_offset, .l4_proto = l4_proto};
    return nf_hook(&p);
}
int ip6_transmit(struct netdev* dev, const struct ip6a* nexthop, const uint8_t* packet, unsigned length) { return transmit6(dev, nexthop, packet, length); }   /* forward.c */
static bool prerouted;   /* forward.c ingress ran PREROUTING on the packet being processed */
static void process(struct netdev* dev, const uint8_t* p, unsigned length, uint8_t pkttype, bool loopback, bool reassembled);
static void deliver_prerouted(struct netdev* dev, uint8_t* p, unsigned length) {
    struct netdev* saved = netdev_rx_current; netdev_rx_current = dev;
    prerouted = true; process(dev, p, length, 0 /* PACKET_HOST */, false, false); prerouted = false;
    netdev_rx_current = saved;
}
static int send_fragmented(struct netdev* dev, const struct ip6a* nexthop, uint8_t* packet, unsigned unfragmentable, unsigned total,
                           unsigned mtu, uint8_t* next_field) {
    /* RFC 8200 4.5: the unfragmentable part (IPv6 header + hop-by-hop) repeats in every fragment */
    unsigned piece = (mtu - unfragmentable - 8) & ~7U;
    if (piece < 8) { ip6_stats.frag_fails++; return -90; }
    uint8_t original_next = *next_field; uint32_t id = fragment_id();
    unsigned data = total - unfragmentable, offset = 0; int result = 0;
    uint8_t small[1600]; uint8_t* buffer = mtu <= sizeof(small) ? small : (void*)arena_alloc(mtu); if (!buffer) return -105;
    *next_field = IPPROTO_FRAGMENT;
    while (offset < data && !result) {
        unsigned part = data - offset > piece ? piece : data - offset; bool more = offset + part < data;
        memcpy(buffer, packet, unfragmentable);
        uint8_t* fh = buffer + unfragmentable;
        fh[0] = original_next; fh[1] = 0; put16(fh + 2, (uint16_t)(offset | (more ? 1 : 0))); put32(fh + 4, id);
        memcpy(fh + 8, packet + unfragmentable + offset, part);
        put16(buffer + 4, (uint16_t)(unfragmentable - 40 + 8 + part));
        result = transmit6(dev, nexthop, buffer, unfragmentable + 8 + part);
        if (!result) ip6_stats.frag_creates++;
        offset += part;
    }
    *next_field = original_next;
    if (buffer != small) arena_free((uint64_t)buffer, mtu);
    if (result) ip6_stats.frag_fails++; else ip6_stats.frag_oks++;
    return result;
}
int ip6_send(const struct ip6a* dst, uint8_t next, const void* payload, unsigned length, const struct ip6_send_options* o) {
    struct ip6_send_options defaults; if (!o) { ip6_options_default(&defaults); o = &defaults; }
    if (o->raw_header) {
        if (length < 40 || (((const uint8_t*)payload)[0] >> 4) != 6) return -22;
        dst = (const void*)((const uint8_t*)payload + 24);
    }
    ip6_stats.out_requests++;
    unsigned ext = o->raw_header ? 0 : o->ext_length;
    if (length > 65535 - ext) return -90;
    struct netdev* dev; struct ip6a nexthop = *dst, source;
    unsigned mtu; bool local = ip6a_loopback(dst) || addrconf_local(dst, 0, true);
    struct ip6a given_source = o->source ? *o->source : ip6a_unspecified;
    if (o->raw_header) given_source = *(const struct ip6a*)((const uint8_t*)payload + 8);
    if (local && !ip6a_multicast(dst)) {
        dev = netdev_lo; mtu = 1500;
        source = !ip6a_any(&given_source) ? given_source : ip6a_loopback(dst) ? ip6a_loopback_address : *dst;
    } else if (ip6a_multicast(dst) && (o->oif || o->no_route_mc)) {
        dev = netdev_by_index(o->oif); if (!dev || !dev->present) return -19;
        mtu = dev->mtu6;
        if (!ip6a_any(&given_source) || o->no_route_mc) source = given_source;
        else if (!source_select6(dst, dev, 0, &source)) return -99;
    } else {
        struct route_result result;
        int error = route_lookup(AF_INET6, dst, ip6a_any(&given_source) ? NULL : &given_source, o->oif, o->mark, &result);
        if (error) { ip6_stats.out_no_routes++; return error; }
        dev = result.dev; nexthop = result.nexthop; mtu = result.mtu;
        if (result.type == RTN_LOCAL) { dev = netdev_lo; mtu = 1500; }
        source = !ip6a_any(&given_source) ? given_source : result.source;
        if (ip6a_any(&source)) { ip6_stats.out_no_routes++; return -99; }
    }
    if (!netdev_ipv6_enabled(dev) && dev != netdev_lo) return -101;
    if (dev != netdev_lo && mtu > dev->mtu6) mtu = dev->mtu6;
    if (mtu < 1280) mtu = 1280;
    unsigned total = o->raw_header ? length : 40 + ext + length;
    uint8_t small[1600]; uint8_t* packet = total <= sizeof(small) ? small : (void*)arena_alloc(total); if (!packet) return -105;
    uint8_t* next_field;
    if (o->raw_header) {
        memcpy(packet, payload, length); put16(packet + 4, (uint16_t)(length - 40));
        if (ip6a_any((const struct ip6a*)(packet + 8))) memcpy(packet + 8, source.b, 16);
        next_field = packet + 6;
    } else {
        uint8_t tclass = o->tclass >= 0 ? (uint8_t)o->tclass : 0;
        uint8_t hops = o->hop_limit >= 0 ? (uint8_t)o->hop_limit :
                       ip6a_multicast(dst) ? 1 : (uint8_t)(dev->conf6.hop_limit > 0 ? dev->conf6.hop_limit : 64);
        put32(packet, 0x60000000U | (uint32_t)tclass << 20 | (o->flowlabel & 0xFFFFF));
        put16(packet + 4, (uint16_t)(ext + length)); packet[6] = ext ? o->ext_first : next; packet[7] = hops;
        memcpy(packet + 8, source.b, 16); memcpy(packet + 24, dst->b, 16);
        if (ext) { memcpy(packet + 40, o->ext, ext); packet[40] = next; }   /* the chain's first header names `next` */
        if (length) memcpy(packet + 40 + ext, payload, length);
        next_field = ext ? packet + 40 : packet + 6;
    }
    unsigned l4 = o->raw_header ? 40 : 40 + ext;
    int result = filter6_out(packet, total, dev, NF_INET_LOCAL_OUT, l4, next, o->mark);
    if (result == NF_ACCEPT && memcmp(packet + 24, dst->b, 16)) {   /* destination NAT at OUTPUT: the new destination's route */
        struct ip6a moved; memcpy(moved.b, packet + 24, 16); struct route_result rr;
        if (ip6a_loopback(&moved) || addrconf_local(&moved, 0, true)) { dev = netdev_lo; nexthop = moved; mtu = 1500; }
        else if (route_lookup(AF_INET6, &moved, NULL, o->oif, o->mark, &rr)) result = NF_DROP;
        else { dev = rr.dev; nexthop = rr.gateway ? rr.nexthop : moved; mtu = rr.mtu ? rr.mtu : dev->mtu6; }
    }
    if (result == NF_ACCEPT) result = filter6_out(packet, total, dev, NF_INET_POST_ROUTING, l4, next, o->mark);
    if (result != NF_ACCEPT) { if (packet != small) arena_free((uint64_t)packet, total); ip6_stats.out_discards++; return -1; }
    /* A multicast datagram is looped back to local members unless IPV6_MULTICAST_LOOP is off. */
    if (ip6a_multicast(dst) && o->mc_loop && dev != netdev_lo && ipv6_mc_joined(dev, dst) && total <= 1500 && !o->no_route_mc) {
        uint8_t copy_dev = 0; (void)copy_dev;
        struct netdev* saved = netdev_rx_current; netdev_rx_current = dev;
        ipv6_receive(dev, packet, total, 2 /* PACKET_MULTICAST */);
        netdev_rx_current = saved;
    }
    if (total <= mtu) result = transmit6(dev, &nexthop, packet, total);
    else if (o->dontfrag) { result = -90; ip6_stats.frag_fails++; }
    else {
        /* Our own extension headers (MLD router alert) are hop-by-hop: unfragmentable. */
        unsigned unfragmentable = o->raw_header ? 40 : 40 + (o->ext_first == IPPROTO_HOPOPTS ? ext : 0);
        if (o->raw_header) next_field = packet + 6;
        result = send_fragmented(dev, &nexthop, packet, unfragmentable, total, mtu, next_field);
    }
    if (result && result != -11) ip6_stats.out_discards++;
    ip6_stats.out_octets += total;
    if (packet != small) arena_free((uint64_t)packet, total);
    return result;
}

/* ---- reassembly ---- */
#define REASM_QUEUES 64
#define REASM_BYTES (256U * 1024U)
#define REASM_TIMEOUT_NS 60000000000ULL
struct fragment6 { struct fragment6* next; unsigned offset, length; uint8_t data[]; };
struct reasm6 {
    bool used, last_seen, first_seen;
    struct ip6a src, dst; uint32_t id; struct netdev* dev;
    unsigned total, received;
    uint8_t* unfragmentable; unsigned unfragmentable_length, next_offset;   /* the first fragment's headers before the fragment header */
    uint8_t next_header;
    uint8_t* first_packet; unsigned first_length;                          /* for ICMPv6 Time Exceeded */
    struct fragment6* fragments;
    uint64_t expires_ns;
};
static struct reasm6 queues[REASM_QUEUES];
static unsigned reasm_bytes;
static void reasm6_free(struct reasm6* q) {
    while (q->fragments) {
        struct fragment6* f = q->fragments; q->fragments = f->next;
        reasm_bytes -= sizeof(*f) + f->length; net_free(f, sizeof(*f) + f->length);
    }
    if (q->unfragmentable) { net_free(q->unfragmentable, q->unfragmentable_length); reasm_bytes -= q->unfragmentable_length; }
    if (q->first_packet) { net_free(q->first_packet, q->first_length); reasm_bytes -= q->first_length; }
    if (q->dev) netdev_release(q->dev);
    memset(q, 0, sizeof(*q));
}
static void process(struct netdev* dev, const uint8_t* p, unsigned length, uint8_t pkttype, bool loopback, bool reassembled);
/* One fragment of `p` whose fragment header is at `offset` (the next-header byte naming it at `next_offset`). */
static void fragment_input(struct netdev* dev, const uint8_t* p, unsigned length, unsigned offset, unsigned next_offset, uint8_t pkttype, bool loopback) {
    const uint8_t* fh = p + offset;
    unsigned fragment_offset = be16(fh + 2) & 0xFFF8, part = length - offset - 8; bool more = (fh[3] & 1) != 0;
    uint32_t id = be32(fh + 4);
    if (!fragment_offset && !more) {   /* RFC 6946 atomic fragment: process as an unfragmented packet */
        uint8_t* copy = (void*)arena_alloc(length - 8); if (!copy) return;
        memcpy(copy, p, offset); memcpy(copy + offset, p + offset + 8, length - offset - 8);
        copy[next_offset] = fh[0]; put16(copy + 4, (uint16_t)(length - 8 - 40));
        process(dev, copy, length - 8, pkttype, loopback, true);
        arena_free((uint64_t)copy, length - 8); return;
    }
    ip6_stats.reasm_reqds++;
    if (more && (part & 7)) { icmp6_error(dev, 4, 0, 4, p, length); ip6_stats.reasm_fails++; return; }   /* pointer: payload length */
    if (fragment_offset + part > 65535) { icmp6_error(dev, 4, 0, offset + 2, p, length); ip6_stats.reasm_fails++; return; }
    if (!part && more) { ip6_stats.reasm_fails++; return; }
    const struct ip6a* src = (const void*)(p + 8); const struct ip6a* dst = (const void*)(p + 24);
    uint64_t now = native_clock_ns();
    struct reasm6* q = NULL, *spare = NULL, *oldest = NULL;
    for (unsigned i = 0; i < REASM_QUEUES; i++) {
        struct reasm6* c = &queues[i];
        if (!c->used) { if (!spare) spare = c; continue; }
        if (c->id == id && ip6a_eq(&c->src, src) && ip6a_eq(&c->dst, dst)) { q = c; break; }
        if (!oldest || c->expires_ns < oldest->expires_ns) oldest = c;
    }
    if (!q) {
        /* every queue busy: the oldest incomplete datagram is evicted (as Linux under memory pressure), so a flood
         * of lone first fragments cannot block reassembly */
        if (!spare) { reasm6_free(oldest); ip6_stats.reasm_fails++; spare = oldest; }
        q = spare; memset(q, 0, sizeof(*q)); q->used = true; q->src = *src; q->dst = *dst; q->id = id;
        q->dev = dev; netdev_retain(dev); q->expires_ns = now + (uint64_t)sysctl_ip6frag_time * 1000000000ULL;
    }
    if (!more) {
        if (q->last_seen && q->total != fragment_offset + part) goto discard;
        q->last_seen = true; q->total = fragment_offset + part;
    }
    if (q->last_seen && fragment_offset + part > q->total) goto discard;
    for (struct fragment6* f = q->fragments; f; f = f->next)
        if (fragment_offset < f->offset + f->length && f->offset < fragment_offset + part) {
            if (f->offset == fragment_offset && f->length == part) return;   /* duplicate */
            goto discard;                                                    /* RFC 5722 overlap */
        }
    if (reasm_bytes + sizeof(struct fragment6) + part + (fragment_offset ? 0 : offset + length) > REASM_BYTES) goto discard;
    struct fragment6* f = net_alloc(sizeof(*f) + part); if (!f) goto discard;
    f->offset = fragment_offset; f->length = part; memcpy(f->data, fh + 8, part);
    struct fragment6** link = &q->fragments; while (*link && (*link)->offset < fragment_offset) link = &(*link)->next;
    f->next = *link; *link = f; q->received += part; reasm_bytes += sizeof(*f) + part;
    if (!fragment_offset && !q->first_seen) {
        q->unfragmentable = net_alloc(offset); q->first_packet = net_alloc(length);
        if (!q->unfragmentable || !q->first_packet) goto discard;
        memcpy(q->unfragmentable, p, offset); q->unfragmentable_length = offset; q->next_offset = next_offset; q->next_header = fh[0];
        memcpy(q->first_packet, p, length); q->first_length = length; reasm_bytes += offset + length;
        q->first_seen = true;
    }
    if (!q->last_seen || !q->first_seen || q->received != q->total) return;
    unsigned whole = q->unfragmentable_length + q->total;
    if (whole - 40 > 65535) goto discard;
    uint8_t* packet = (void*)arena_alloc(whole); if (!packet) goto discard;
    memcpy(packet, q->unfragmentable, q->unfragmentable_length);
    for (struct fragment6* g = q->fragments; g; g = g->next) memcpy(packet + q->unfragmentable_length + g->offset, g->data, g->length);
    packet[q->next_offset] = q->next_header; put16(packet + 4, (uint16_t)(whole - 40));
    struct netdev* arrival = q->dev; netdev_retain(arrival);
    reasm6_free(q); ip6_stats.reasm_oks++;
    process(arrival, packet, whole, pkttype, loopback, true);
    netdev_release(arrival);
    arena_free((uint64_t)packet, whole);
    return;
discard:
    ip6_stats.reasm_fails++; reasm6_free(q);
}
static void reasm_expire(uint64_t now) {
    for (unsigned i = 0; i < REASM_QUEUES; i++) {
        struct reasm6* q = &queues[i];
        if (!q->used || q->expires_ns > now) continue;
        ip6_stats.reasm_timeout++; ip6_stats.reasm_fails++;
        /* RFC 8200: Time Exceeded (fragment reassembly) only if the first fragment arrived */
        if (q->first_seen && q->dev && q->dev->present) icmp6_error(q->dev, 3, 1, 0, q->first_packet, q->first_length);
        reasm6_free(q);
    }
}

/* ---- input ---- */
#define MAX_EXTENSIONS 16
#define PACKET_HOST 0
/* Hop-by-hop / destination options. 0: continue, -1: drop silently, >0: ICMP parameter problem code 2 at offset-1. */
static int options(struct netdev* dev, const uint8_t* p, unsigned length, unsigned start, unsigned size, bool hbh, bool* router_alert) {
    const struct ip6a* dst = (const void*)(p + 24);
    unsigned at = start + 2, end = start + size, padding = 0;
    while (at < end) {
        uint8_t type = p[at];
        if (type == 0) { at++; if (++padding > 7) return -1; continue; }
        if (at + 2 > end) return -1;
        unsigned option_length = p[at + 1];
        if (at + 2 + option_length > end) return -1;
        if (type == 1) {
            padding += option_length + 2; if (padding > 7) return -1;
            for (unsigned i = 0; i < option_length; i++) if (p[at + 2 + i]) return -1;   /* PadN carries zeros (Linux) */
        } else {
            padding = 0;
            if (hbh && type == 5 && option_length == 2) *router_alert = true;
            else if (hbh && type == 0xC2) return -1;                                  /* jumbo payload: unsupported */
            else {
                unsigned action = type >> 6;   /* RFC 8200 4.2: what to do with an unrecognized option */
                if (action == 1 || (action == 3 && ip6a_multicast(dst))) return -1;
                if (action >= 2) return (int)at + 1;
            }
        }
        at += 2 + option_length;
    }
    (void)dev; (void)length;
    return 0;
}
static void deliver_udp(struct netdev* dev, const uint8_t* p, unsigned length, unsigned offset, const struct ip6a* src, const struct ip6a* dst) {
    if (length - offset < 8) { ip6_stats.in_hdr_errors++; return; }
    const uint8_t* u = p + offset; unsigned udp_length = be16(u + 4);
    if (udp_length < 8 || udp_length > length - offset) { ip6_stats.in_hdr_errors++; return; }
    if (!be16(u + 6) || ip6_checksum(src, dst, IPPROTO_UDP, u, udp_length)) { ip6_stats.in_hdr_errors++; return; }   /* mandatory over IPv6 */
    uint16_t sport = be16(u), dport = be16(u + 2);
    if (dport == 546) dhcp6_receive(dev, src, u + 8, udp_length - 8);   /* the kernel's DHCPv6 client (any server port: slirp) */
    struct inet_rx_info info; memset(&info, 0, sizeof(info));
    info.src = *src; info.dst = *dst; info.ifindex = dev->index; info.hop_limit = p[7];
    info.tclass = (uint8_t)((be16(p) >> 4) & 0xFF); info.src_port = sport; info.dst_port = dport; info.multicast = ip6a_multicast(dst);
    if (!inet_udp_receive(&info, u + 8, udp_length - 8) && !info.multicast && dport != 546) icmp6_error(dev, 1, 4, 0, p, length);
}
/* The scope id an address carries on arrival: its interface for link-local addresses (over loopback: the interface
 * owning our own address), 0 otherwise. Sockets name their TCP handles by the same rule. */
unsigned ip6_scope_of(struct netdev* dev, const struct ip6a* a) {
    if (!ip6a_linklocal(a)) return 0;
    if (dev != netdev_lo) return dev->index;
    for (struct netdev* d = netdevs; d; d = d->next) if (d != netdev_lo && addrconf_find(d, a)) return d->index;
    return dev->index;
}
static void deliver_tcp(struct netdev* dev, const uint8_t* p, unsigned length, unsigned offset, const struct ip6a* src, const struct ip6a* dst) {
    if (ip6a_multicast(dst) || length - offset < 20 || length - offset > 65535) { ip6_stats.in_discards++; return; }
    uint32_t source = ip6h_get(src, ip6_scope_of(dev, src)), destination = ip6h_get(dst, ip6_scope_of(dev, dst));
    if (!source || !destination) { ip6_stats.in_discards++; return; }
    /* (the NIC and loopback polls already run inside a TCP receive batch) */
    tcp_handle_packet_to(source, destination, p + offset, (uint16_t)(length - offset));
}
static void process(struct netdev* dev, const uint8_t* p, unsigned length, uint8_t pkttype, bool loopback, bool reassembled) {
    const struct ip6a* src = (const void*)(p + 8); const struct ip6a* dst = (const void*)(p + 24);
    uint8_t next = p[6]; unsigned offset = 40, next_offset = 6, count = 0; bool router_alert = false, has_hbh = false;
    unsigned fragment_header = 0;
    while (true) {
        if (next == IPPROTO_HOPOPTS || next == IPPROTO_DSTOPTS || next == IPPROTO_ROUTING || next == IPPROTO_FRAGMENT) {
            if (++count > MAX_EXTENSIONS) { ip6_stats.in_hdr_errors++; return; }
            if (next == IPPROTO_HOPOPTS && offset != 40) { icmp6_error(dev, 4, 1, next_offset, p, length); ip6_stats.in_hdr_errors++; return; }
            if (length - offset < 8) {
                /* RFC 7112: the first fragment carries the whole chain */
                if (fragment_header) icmp6_error(dev, 4, 3, 0, p, length);
                ip6_stats.in_truncated++; return;
            }
            unsigned size = next == IPPROTO_FRAGMENT ? 8 : (p[offset + 1] + 1u) * 8u;
            if (size > length - offset) { if (fragment_header) icmp6_error(dev, 4, 3, 0, p, length); ip6_stats.in_truncated++; return; }
            if (next == IPPROTO_HOPOPTS || next == IPPROTO_DSTOPTS) {
                if (next == IPPROTO_HOPOPTS) has_hbh = true;
                int verdict = options(dev, p, length, offset, size, next == IPPROTO_HOPOPTS, &router_alert);
                if (verdict < 0) { ip6_stats.in_hdr_errors++; return; }
                if (verdict > 0) { icmp6_error(dev, 4, 2, (uint32_t)verdict - 1, p, length); ip6_stats.in_hdr_errors++; return; }
            } else if (next == IPPROTO_ROUTING) {
                if (p[offset + 3]) {   /* segments left: we are not a router of any routing type (RFC 5095 for type 0) */
                    icmp6_error(dev, 4, 0, offset + 2, p, length); ip6_stats.in_hdr_errors++; return;
                }
            } else {
                if (reassembled) { ip6_stats.in_hdr_errors++; return; }   /* a second fragment header after reassembly */
                fragment_header = offset;
                unsigned frag_offset = be16(p + offset + 2) & 0xFFF8; bool more = (p[offset + 3] & 1) != 0;
                if (frag_offset || more) {
                    if (frag_offset == 0) {
                        /* the first fragment must hold the complete header chain up to the upper-layer header (RFC 7112) */
                        uint8_t n = p[offset]; unsigned at = offset + 8, depth = 0;
                        while (n == IPPROTO_HOPOPTS || n == IPPROTO_DSTOPTS || n == IPPROTO_ROUTING) {
                            if (++depth > MAX_EXTENSIONS || length - at < 8 || (p[at + 1] + 1u) * 8u > length - at) { icmp6_error(dev, 4, 3, 0, p, length); return; }
                            n = p[at]; at += (p[at + 1] + 1u) * 8u;
                        }
                        unsigned need = n == IPPROTO_TCP ? 20 : n == IPPROTO_UDP ? 8 : n == IPPROTO_ICMPV6 ? 4 : 0;
                        if (length - at < need) { icmp6_error(dev, 4, 3, 0, p, length); return; }
                    }
                }
                fragment_input(dev, p, length, offset, next_offset, pkttype, loopback); return;   /* (atomic ones too) */
            }
            next_offset = offset; next = p[offset]; offset += size;
            continue;
        }
        break;
    }
    if (next == IPPROTO_NONE) { ip6_stats.in_delivers++; return; }
    bool fragment = false;
    if ((!prerouted && filter6(p, length, dev, NULL, NF_INET_PRE_ROUTING, offset, next, fragment, 0) != NF_ACCEPT) ||
        filter6(p, length, dev, NULL, NF_INET_LOCAL_IN, offset, next, fragment, 0) != NF_ACCEPT) { ip6_stats.in_discards++; return; }
    struct inet_rx_info info; memset(&info, 0, sizeof(info));
    info.src = *src; info.dst = *dst; info.ifindex = dev->index; info.hop_limit = p[7];
    info.tclass = (uint8_t)((be16(p) >> 4) & 0xFF); info.multicast = ip6a_multicast(dst);
    inet_raw_receive(AF_INET6, &info, next, p, length, p + offset, length - offset);
    ip6_stats.in_delivers++;
    switch (next) {
    case IPPROTO_ICMPV6:
        icmp6_receive(dev, src, dst, p[7], p + offset, length - offset, p, length, has_hbh && router_alert); break;
    case IPPROTO_UDP: deliver_udp(dev, p, length, offset, src, dst); break;
    case IPPROTO_TCP: deliver_tcp(dev, p, length, offset, src, dst); break;
    default:
        if (!inet_raw_protocol(AF_INET6, next)) { icmp6_error(dev, 4, 1, next_offset, p, length); ip6_stats.in_unknown_protos++; }
        break;
    }
    (void)pkttype;
}
void ipv6_receive(struct netdev* dev, const void* data, unsigned length, uint8_t pkttype) {
    const uint8_t* p = data; bool loopback = dev == netdev_lo;
    ip6_stats.in_receives++; ip6_stats.in_octets += length;
    if (!netdev_ipv6_enabled(dev) && !loopback) { ip6_stats.in_discards++; return; }
    if (length < 40 || (p[0] >> 4) != 6) { ip6_stats.in_hdr_errors++; return; }
    unsigned payload = be16(p + 4);
    if (!payload && p[6] == IPPROTO_HOPOPTS) { ip6_stats.in_hdr_errors++; return; }   /* jumbogram */
    if (40 + payload > length) { ip6_stats.in_truncated++; return; }
    length = 40 + payload;                                                             /* L2 padding */
    const struct ip6a* src = (const void*)(p + 8); const struct ip6a* dst = (const void*)(p + 24);
    if (ip6a_multicast(src)) { ip6_stats.in_addr_errors++; return; }
    if (!loopback && (ip6a_loopback(src) || ip6a_loopback(dst))) { ip6_stats.in_addr_errors++; return; }
    if (ip6a_is4(src) || ip6a_is4(dst)) { ip6_stats.in_addr_errors++; return; }     /* RFC 4291 2.5.5.2: never on the wire */
    bool local = ip6a_multicast(dst) ? ipv6_mc_joined(dev, dst) :
                 ip6a_loopback(dst) || addrconf_local(dst, ip6a_linklocal(dst) && !loopback ? dev->index : 0, false);
    if (ip6a_multicast(dst)) {
        unsigned scope = ip6a_mc_scope(dst);
        if (!scope || (scope == 1 && !loopback)) { ip6_stats.in_addr_errors++; return; }
    }
    if (!loopback && ip6_ingress(dev, p, length, local, deliver_prerouted)) return;   /* forwarding, NAT (forward.c) */
    if (ip6a_multicast(dst)) {
        if (!local) { ip6_stats.in_discards++; return; }
        ip6_stats.in_mcast++;
    } else if (!local) {
        /* (over loopback: any of our addresses, link-local ones of every interface included) */
        /* Not ours (or still tentative), and this interface does not forward. */
        ip6_stats.in_addr_errors++; return;
    }
    process(dev, p, length, pkttype, loopback, false);
}
void ipv6_loopback_input(const void* packet, unsigned length) {
    struct netdev* saved = netdev_rx_current; netdev_rx_current = netdev_lo;
    ipv6_receive(netdev_lo, packet, length, PACKET_HOST);
    netdev_rx_current = saved;
}
void ipv6_poll(uint64_t now) {
    static uint64_t next; if (now < next) return; next = now + 1000000000ULL;
    reasm_expire(now);
}
void ipv6_init(void) { memset(queues, 0, sizeof(queues)); }
