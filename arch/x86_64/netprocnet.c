/* /proc/net text files in Linux's formats: dev, route, arp, if_inet6, ipv6_route, tcp/tcp6/udp/udp6/raw/raw6/
 * icmp/icmp6 (one line per socket, inet.c), snmp, snmp6, netstat, sockstat(6), igmp, igmp6, dev_mcast, packet,
 * nf_conntrack (conntrack.c), protocols, unix (header) and wireless (header: no wireless devices). */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/arp.h"
#include "../../kernel/tcp.h"
#include "../../kernel/string.h"

struct text { char* out; unsigned capacity, used; };
static void put(struct text* t, const char* s) { while (*s && t->used + 1 < t->capacity) t->out[t->used++] = *s++; t->out[t->used] = 0; }
static void dec(struct text* t, uint64_t v, unsigned width) {
    char digits[24]; unsigned n = 0; do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (width > n && t->used + 1 < t->capacity) { t->out[t->used++] = ' '; width--; }
    while (n && t->used + 1 < t->capacity) t->out[t->used++] = digits[--n];
    t->out[t->used] = 0;
}
static void left(struct text* t, const char* s, unsigned width) { unsigned n = strlen(s); put(t, s); while (n++ < width) put(t, " "); }
static void hex(struct text* t, uint64_t v, unsigned digits, bool upper) {
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (unsigned i = 0; i < digits && t->used + 1 < t->capacity; i++) t->out[t->used++] = set[(v >> ((digits - 1 - i) * 4)) & 15];
    t->out[t->used] = 0;
}
static void hex6(struct text* t, const struct ip6a* a) { for (unsigned i = 0; i < 16; i++) hex(t, a->b[i], 2, false); }
static void mac(struct text* t, const uint8_t* m) { for (unsigned i = 0; i < 6; i++) { if (i) put(t, ":"); hex(t, m[i], 2, false); } }
static void dev_file(struct text* t) {
    put(t, "Inter-|   Receive                                                |  Transmit\n");
    put(t, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    for (struct netdev* d = netdevs; d; d = d->next) {
        struct netdev_stats s = d->stats;
        if (d == netdev_eth0 && d->hw) {
            s.rx_packets = d->hw->stats.rx_packets; s.tx_packets = d->hw->stats.tx_packets; s.rx_bytes = d->hw->stats.rx_bytes;
            s.tx_bytes = d->hw->stats.tx_bytes; s.rx_errors = d->hw->stats.rx_errors; s.tx_errors = d->hw->stats.tx_errors; s.rx_dropped = d->hw->stats.rx_dropped;
        }
        unsigned pad = strlen(d->name); while (pad++ < 6) put(t, " ");
        put(t, d->name); put(t, ":");
        dec(t, s.rx_bytes, 8); put(t, " "); dec(t, s.rx_packets, 7); put(t, " "); dec(t, s.rx_errors, 4); put(t, " "); dec(t, s.rx_dropped, 4);
        put(t, "    0     0          0 "); dec(t, s.multicast, 9); put(t, " ");
        dec(t, s.tx_bytes, 8); put(t, " "); dec(t, s.tx_packets, 7); put(t, " "); dec(t, s.tx_errors, 4); put(t, " "); dec(t, s.tx_dropped, 4);
        put(t, "    0 "); dec(t, s.collisions, 5); put(t, "       0          0\n");
    }
}
static void route_file(struct text* t) {
    put(t, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT                                                       \n");
    for (const struct route* r = routes; r; r = r->next) {
        if (r->family != AF_INET || r->table != RT_TABLE_MAIN || r->type != RTN_UNICAST) continue;
        struct netdev* d = netdev_by_index(r->oif);
        uint32_t mask = r->plen ? 0xFFFFFFFFU << (32 - r->plen) : 0, gateway = ip6a_v4(&r->gateway);
        unsigned flags = 1 | (gateway ? 2 : 0) | (r->plen == 32 ? 4 : 0);
        put(t, d ? d->name : "*"); put(t, "\t"); hex(t, __builtin_bswap32(ip6a_v4(&r->dst)), 8, true); put(t, "\t"); hex(t, __builtin_bswap32(gateway), 8, true);
        put(t, "\t"); hex(t, flags, 4, true); put(t, "\t0\t0\t"); dec(t, r->metric, 0); put(t, "\t"); hex(t, __builtin_bswap32(mask), 8, true);
        put(t, "\t"); dec(t, r->mtu, 0); put(t, "\t0\t0                                                                               \n");
    }
}
static void arp_file(struct text* t) {
    put(t, "IP address       HW type     Flags       HW address            Mask     Device\n");
    if (netdev_eth0) {
        int count; const arp_entry_t* cache = arp_get_cache(&count);
        for (int i = 0; i < count; i++) {
            if (!cache[i].valid) continue;
            char ip[16]; ip_to_string(cache[i].ip, ip); left(t, ip, 17); put(t, "0x1         0x2         "); mac(t, cache[i].mac.addr);
            put(t, "     *        "); put(t, netdev_eth0->name); put(t, "\n");
        }
    }
    for (struct neighbour* n = neighbours; n; n = n->next) {
        if (n->family != AF_INET) continue;
        char ip[16]; ip_to_string(ip6a_v4(&n->key), ip); left(t, ip, 17); put(t, "0x1         ");
        put(t, n->state & (NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE) ? "0x2         " : n->state & NUD_PERMANENT ? "0x6         " : "0x0         ");
        if (n->has_lladdr) mac(t, n->lladdr); else put(t, "00:00:00:00:00:00");
        put(t, "     *        "); put(t, n->dev->name); put(t, "\n");
    }
}
static unsigned scope6(const struct ip6a* a) { return ip6a_loopback(a) ? 0x10 : ip6a_linklocal(a) ? 0x20 : (a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0xC0) ? 0x40 : 0; }
static void if_inet6_file(struct text* t) {
    for (struct netdev* d = netdevs; d; d = d->next)
        for (struct inet6_ifaddr* a = d->addrs6; a; a = a->next) {
            hex6(t, &a->addr); put(t, " "); hex(t, d->index, 2, false); put(t, " "); hex(t, a->plen, 2, false); put(t, " ");
            hex(t, scope6(&a->addr), 2, false); put(t, " "); hex(t, a->flags & 0xFF, 2, false); put(t, " ");
            unsigned pad = strlen(d->name); while (pad++ < 8) put(t, " ");
            put(t, d->name); put(t, "\n");
        }
}
static void ipv6_route_file(struct text* t) {
    uint64_t now = native_clock_ns();
    for (const struct route* r = routes; r; r = r->next) {
        if (r->family != AF_INET6 || (r->expires_ns && r->expires_ns <= now)) continue;
        struct netdev* d = netdev_by_index(r->oif);
        unsigned flags = 1 | (ip6a_any(&r->gateway) ? 0 : 2) | (r->plen == 128 ? 4 : 0) | (r->expires_ns ? 0x400000 : 0) |
                         (r->protocol == RTPROT_RA ? 0x40000 | (r->plen == 0 ? 0x10000 : 0) : 0) | (r->type == RTN_UNREACHABLE ? 0x200 : 0);
        hex6(t, &r->dst); put(t, " "); hex(t, r->plen, 2, false); put(t, " "); hex6(t, &r->src); put(t, " "); hex(t, r->src_len, 2, false); put(t, " ");
        hex6(t, &r->gateway); put(t, " "); hex(t, r->metric, 8, false); put(t, " 00000001 00000000 "); hex(t, flags, 8, false); put(t, " ");
        const char* name = r->type == RTN_LOCAL ? "lo" : d ? d->name : "lo";
        unsigned pad = strlen(name); while (pad++ < 8) put(t, " ");
        put(t, name); put(t, "\n");
    }
}
static void sockets_file(struct text* t, int family, int kind) {
    if (kind == 1) put(t, family == AF_INET ? "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n" :
                                             "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    else put(t, family == AF_INET ? "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n" :
                                   "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    if (t->used + 1 < t->capacity) t->used += inet_proc_lines(family, kind, t->out + t->used, t->capacity - t->used);
    t->out[t->used] = 0;
}
static void counters(struct text* t, const char* const* names, const uint64_t* values, unsigned count, const char* prefix) {
    put(t, prefix); for (unsigned i = 0; i < count; i++) { put(t, " "); put(t, names[i]); } put(t, "\n");
    put(t, prefix); for (unsigned i = 0; i < count; i++) { put(t, " "); dec(t, values[i], 0); } put(t, "\n");
}
static void snmp_file(struct text* t) {
    static const char* const ip[] = {"Forwarding", "DefaultTTL", "InReceives", "InHdrErrors", "InAddrErrors", "ForwDatagrams", "InUnknownProtos",
        "InDiscards", "InDelivers", "OutRequests", "OutDiscards", "OutNoRoutes", "ReasmTimeout", "ReasmReqds", "ReasmOKs", "ReasmFails", "FragOKs", "FragFails", "FragCreates"};
    uint64_t ipv[19] = {sysctl_ip_forward ? 1 : 2, (uint64_t)sysctl_ip_default_ttl, ip4_counters.in_receives, 0, 0, ip4_forwarded, 0, 0, ip4_counters.in_delivers, ip4_counters.out_requests,
                        ip4_counters.out_discards, ip4_counters.out_no_routes, 0, 0, 0, 0, 0, ip4_counters.frag_fails, 0};
    counters(t, ip, ipv, 19, "Ip:");
    put(t, "Icmp: InMsgs InErrors InCsumErrors InDestUnreachs InTimeExcds InParmProbs InSrcQuenchs InRedirects InEchos InEchoReps InTimestamps InTimestampReps InAddrMasks InAddrMaskReps OutMsgs OutErrors OutRateLimitGlobal OutRateLimitHost OutDestUnreachs OutTimeExcds OutParmProbs OutSrcQuenchs OutRedirects OutEchos OutEchoReps OutTimestamps OutTimestampReps OutAddrMasks OutAddrMaskReps\n");
    put(t, "Icmp: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    put(t, "IcmpMsg:\n");
    const tcp_stats_t* tcp = tcp_get_stats();
    static const char* const tn[] = {"RtoAlgorithm", "RtoMin", "RtoMax", "MaxConn", "ActiveOpens", "PassiveOpens", "AttemptFails", "EstabResets",
        "CurrEstab", "InSegs", "OutSegs", "RetransSegs", "InErrs", "OutRsts", "InCsumErrors"};
    uint64_t tv[15] = {1, 200, 120000, 0, 0, 0, 0, 0, 0, tcp->segs_rcvd, tcp->segs_sent, tcp->retransmits + tcp->fast_retransmits, 0, tcp->rst_sent, 0};
    put(t, "Tcp:"); for (unsigned i = 0; i < 15; i++) { put(t, " "); put(t, tn[i]); } put(t, "\nTcp:");
    for (unsigned i = 0; i < 15; i++) { put(t, " "); if (i == 3) put(t, "-1"); else dec(t, tv[i], 0); } put(t, "\n");
    put(t, "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors\nUdp: 0 0 0 0 0 0 0 0 0\n");
    put(t, "UdpLite: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors\nUdpLite: 0 0 0 0 0 0 0 0 0\n");
}
static void snmp6_file(struct text* t) {
    const struct ipv6_stats* s = &ip6_stats;
    static const char* const names[] = {"Ip6InReceives", "Ip6InHdrErrors", "Ip6InTooBigErrors", "Ip6InNoRoutes", "Ip6InAddrErrors", "Ip6InUnknownProtos",
        "Ip6InTruncatedPkts", "Ip6InDiscards", "Ip6InDelivers", "Ip6OutForwDatagrams", "Ip6OutRequests", "Ip6OutDiscards", "Ip6OutNoRoutes",
        "Ip6ReasmTimeout", "Ip6ReasmReqds", "Ip6ReasmOKs", "Ip6ReasmFails", "Ip6FragOKs", "Ip6FragFails", "Ip6FragCreates", "Ip6InMcastPkts",
        "Ip6OutMcastPkts", "Ip6InOctets", "Ip6OutOctets"};
    uint64_t values[] = {s->in_receives, s->in_hdr_errors, s->in_too_big, s->in_no_routes, s->in_addr_errors, s->in_unknown_protos, s->in_truncated,
        s->in_discards, s->in_delivers, s->out_forw, s->out_requests, s->out_discards, s->out_no_routes, s->reasm_timeout, s->reasm_reqds, s->reasm_oks,
        s->reasm_fails, s->frag_oks, s->frag_fails, s->frag_creates, s->in_mcast, s->out_mcast, s->in_octets, s->out_octets};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) { left(t, names[i], 32); dec(t, values[i], 0); put(t, "\n"); }
    left(t, "Icmp6InMsgs", 32); dec(t, icmp6_counters.in_msgs, 0); put(t, "\n");
    left(t, "Icmp6InErrors", 32); dec(t, icmp6_counters.in_errors, 0); put(t, "\n");
    left(t, "Icmp6OutMsgs", 32); dec(t, icmp6_counters.out_msgs, 0); put(t, "\n");
    left(t, "Icmp6OutErrors", 32); dec(t, icmp6_counters.out_errors, 0); put(t, "\n");
    left(t, "Icmp6InCsumErrors", 32); dec(t, icmp6_counters.in_csum_errors, 0); put(t, "\n");
    static const struct { uint8_t type; const char* name; } types[] = {{1, "DestUnreachs"}, {2, "PktTooBigs"}, {3, "TimeExcds"}, {4, "ParmProblems"},
        {128, "Echos"}, {129, "EchoReplies"}, {130, "GroupMembQueries"}, {131, "GroupMembResponses"}, {132, "GroupMembReductions"},
        {133, "RouterSolicits"}, {134, "RouterAdvertisements"}, {135, "NeighborSolicits"}, {136, "NeighborAdvertisements"}, {137, "Redirects"}, {143, "MLDv2Reports"}};
    for (unsigned dir = 0; dir < 2; dir++)
        for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            char name[48] = "Icmp6"; strcat(name, dir ? "Out" : "In"); strcat(name, types[i].name);
            left(t, name, 32); dec(t, dir ? icmp6_counters.out_type[types[i].type] : icmp6_counters.in_type[types[i].type], 0); put(t, "\n");
        }
    static const char* const udp[] = {"Udp6InDatagrams", "Udp6NoPorts", "Udp6InErrors", "Udp6OutDatagrams", "Udp6RcvbufErrors", "Udp6SndbufErrors", "Udp6InCsumErrors", "Udp6IgnoredMulti", "Udp6MemErrors"};
    for (unsigned i = 0; i < 9; i++) { left(t, udp[i], 32); put(t, "0\n"); }
}
static void igmp6_file(struct text* t) {
    for (struct netdev* d = netdevs; d; d = d->next)
        for (struct mc6_group* g = d->mc6; g; g = g->next) {
            if (g->leaving) continue;
            dec(t, d->index, 0); put(t, "    "); left(t, d->name, 16); hex6(t, &g->group); put(t, " "); dec(t, g->users, 5); put(t, " ");
            hex(t, g->change_ns || g->answer_ns ? 0 : 4, 8, true); put(t, " 0\n");
        }
}
static void igmp_file(struct text* t) {
    put(t, "Idx\tDevice    : Count Querier\tGroup    Users Timer\tReporter\n");
    for (struct netdev* d = netdevs; d; d = d->next) {
        unsigned count = 1; for (struct mc4_group* g = d->mc4; g; g = g->next) if (!g->leaving) count++;
        dec(t, d->index, 0); put(t, "\t"); left(t, d->name, 10); put(t, ": "); dec(t, count, 5); put(t, " "); put(t, mc_igmp_version(d)); put(t, "\n");
        put(t, "\t\t\t\t010000E0     1 0:00000000\t\t0\n");
        for (struct mc4_group* g = d->mc4; g; g = g->next) {
            if (g->leaving) continue;
            put(t, "\t\t\t\t"); hex(t, __builtin_bswap32(ip6a_v4(&g->group)), 8, true); put(t, " "); dec(t, g->users, 5); put(t, " 0:00000000\t\t0\n");
        }
    }
}
static void dev_mcast_file(struct text* t) {
    for (struct netdev* d = netdevs; d; d = d->next) {
        for (struct mc6_group* g = d->mc6; g; g = g->next) {
            dec(t, d->index, 0); put(t, "    "); left(t, d->name, 16); put(t, "1     0     33");
            put(t, "33"); for (unsigned i = 12; i < 16; i++) hex(t, g->group.b[i], 2, false); put(t, "\n");
        }
    }
}
unsigned packet_proc_lines(char* out, unsigned capacity);
int conntrack_proc_text(char* out, unsigned capacity);
bool netif_proc_text(const char* name, char* out, unsigned capacity) {
    struct text t = {out, capacity, 0}; out[0] = 0;
    if (!strcmp(name, "dev")) dev_file(&t);
    else if (!strcmp(name, "route")) route_file(&t);
    else if (!strcmp(name, "arp")) arp_file(&t);
    else if (!strcmp(name, "if_inet6")) if_inet6_file(&t);
    else if (!strcmp(name, "ipv6_route")) ipv6_route_file(&t);
    else if (!strcmp(name, "tcp")) sockets_file(&t, AF_INET, 1);
    else if (!strcmp(name, "tcp6")) sockets_file(&t, AF_INET6, 1);
    else if (!strcmp(name, "udp")) sockets_file(&t, AF_INET, 2);
    else if (!strcmp(name, "udp6")) sockets_file(&t, AF_INET6, 2);
    else if (!strcmp(name, "icmp")) sockets_file(&t, AF_INET, 3);
    else if (!strcmp(name, "icmp6")) sockets_file(&t, AF_INET6, 3);
    else if (!strcmp(name, "raw")) sockets_file(&t, AF_INET, 4);
    else if (!strcmp(name, "raw6")) sockets_file(&t, AF_INET6, 4);
    else if (!strcmp(name, "snmp")) snmp_file(&t);
    else if (!strcmp(name, "snmp6")) snmp6_file(&t);
    else if (!strcmp(name, "netstat")) put(&t, "TcpExt: SyncookiesSent SyncookiesRecv SyncookiesFailed\nTcpExt: 0 0 0\nIpExt: InNoRoutes InTruncatedPkts InMcastPkts OutMcastPkts\nIpExt: 0 0 0 0\n");
    else if (!strcmp(name, "sockstat")) put(&t, "sockets: used 0\nTCP: inuse 0 orphan 0 tw 0 alloc 0 mem 0\nUDP: inuse 0 mem 0\nUDPLITE: inuse 0\nRAW: inuse 0\nFRAG: inuse 0 memory 0\n");
    else if (!strcmp(name, "sockstat6")) put(&t, "TCP6: inuse 0\nUDP6: inuse 0\nUDPLITE6: inuse 0\nRAW6: inuse 0\nFRAG6: inuse 0 memory 0\n");
    else if (!strcmp(name, "igmp")) igmp_file(&t);
    else if (!strcmp(name, "igmp6")) igmp6_file(&t);
    else if (!strcmp(name, "dev_mcast")) dev_mcast_file(&t);
    else if (!strcmp(name, "mcfilter") || !strcmp(name, "mcfilter6")) mc_proc_filters(name[8] == '6', out, capacity);   /* mcast.c */
    else if (!strcmp(name, "packet")) { put(&t, "sk               RefCnt Type Proto  Iface R Rmem   User   Inode\n"); if (t.used + 1 < capacity) t.used += packet_proc_lines(out + t.used, capacity - t.used); }
    else if (!strcmp(name, "nf_conntrack")) { int n = conntrack_proc_text(out, capacity); if (n < 0) return false; }
    else if (!strcmp(name, "unix")) put(&t, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    else if (!strcmp(name, "protocols")) put(&t, "protocol  size sockets  memory press maxhdr  slab module     cl co di ac io in de sh ss gs se re sp bi br ha uh gp em\n"
                                                  "TCPv6     2400      0       0   no     320   yes  kernel      y  y  y  y  y  y  y  y  y  y  y  y  y  n  y  y  y  y  y\n"
                                                  "UDPv6     1536      0       0   NI       0   yes  kernel      y  y  y  n  y  y  y  n  y  y  y  y  y  n  y  y  y  y  n\n"
                                                  "RAWv6     1152      0      -1   NI       0   yes  kernel      y  y  y  n  y  y  y  n  y  y  y  y  n  y  y  y  y  n  n\n"
                                                  "TCP       2240      0       0   no     320   yes  kernel      y  y  y  y  y  y  y  y  y  y  y  y  y  n  y  y  y  y  y\n"
                                                  "UDP       1344      0       0   NI       0   yes  kernel      y  y  y  n  y  y  y  n  y  y  y  y  y  n  y  y  y  y  n\n"
                                                  "RAW       1152      0      -1   NI       0   yes  kernel      y  y  y  n  y  y  y  n  y  y  y  y  n  y  y  y  y  n  n\n"
                                                  "PACKET    1472      0      -1   NI       0   no   kernel      n  n  n  n  n  n  n  n  n  n  n  n  n  n  n  n  n  n  n\n");
    else if (!strcmp(name, "wireless")) put(&t, "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n");
    else return false;
    return true;
}
