/* Stateless DHCPv6 (RFC 8415 section 18.2.6): when a router advertisement sets the M or O flag, or carries no
 * RDNSS option, the interface sends INFORMATION-REQUEST to ff02::1:2 from its link-local address (randomized
 * retransmission from 1 s up to INF_MAX_RT, a random initial delay up to 1 s) asking for DNS servers (23), the
 * domain search list (24) and the information refresh time (32). The REPLY's servers feed /etc/resolv.conf
 * (dhcp64.c) next to RDNSS; they are refreshed after the refresh time (default 86400 s, at least 600 s).
 * Addresses from a DHCPv6 server (IA_NA, the M flag's stateful mode) are not requested. */
#include "netdev.h"
#include "../../kernel/string.h"

#define SERVERS 4
#define DOMAINS 4
struct dhcp6 {
    bool used, active, done;
    unsigned ifindex;
    uint8_t xid[3];
    unsigned attempts;
    uint64_t start_ns, next_ns, refresh_ns, rt_ns;
    struct ip6a servers[SERVERS]; unsigned server_count;
    char domains[DOMAINS][64]; unsigned domain_count;
};
static struct dhcp6 clients[4];
static struct dhcp6* client_for(struct netdev* dev, bool create) {
    for (unsigned i = 0; i < 4; i++) if (clients[i].used && clients[i].ifindex == dev->index) return &clients[i];
    if (!create) return NULL;
    for (unsigned i = 0; i < 4; i++) if (!clients[i].used) { memset(&clients[i], 0, sizeof(clients[i])); clients[i].used = true; clients[i].ifindex = dev->index; return &clients[i]; }
    return NULL;
}
static void new_xid(struct dhcp6* c) { uint32_t r = net_random32(); c->xid[0] = (uint8_t)r; c->xid[1] = (uint8_t)(r >> 8); c->xid[2] = (uint8_t)(r >> 16); }
void dhcp6_start(struct netdev* dev, bool stateful) {
    (void)stateful;
    struct dhcp6* c = client_for(dev, true); if (!c) return;
    if (c->active || (c->done && c->refresh_ns > native_clock_ns())) return;
    c->active = true; c->attempts = 0; new_xid(c);
    uint64_t now = native_clock_ns();
    c->start_ns = now; c->next_ns = now + ndisc_random_ns(1000000000ULL) + 1; c->rt_ns = 0;
}
static void send_request(struct netdev* dev, struct dhcp6* c, uint64_t now) {
    struct ip6a src = ip6a_unspecified; bool have = false;
    for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next)
        if (ip6a_linklocal(&a->addr) && !(a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED))) { src = a->addr; have = true; break; }
    if (!have) return;   /* RFC 8415: a link-local source is required; tried again on the next timer */
    uint8_t m[8 + 4 + 18 + 4 + 2 + 4 + 6]; unsigned n = 8;
    m[n++] = 11; memcpy(m + n, c->xid, 3); n += 3;                                     /* INFORMATION-REQUEST */
    put16(m + n, 1); put16(m + n + 2, 10); n += 4;                                        /* client identifier: DUID-LL */
    put16(m + n, 3); put16(m + n + 2, 1); memcpy(m + n + 4, dev->addr, 6); n += 10;
    uint64_t elapsed = (now - c->start_ns) / 10000000ULL; if (elapsed > 0xFFFF) elapsed = 0xFFFF;
    put16(m + n, 8); put16(m + n + 2, 2); put16(m + n + 4, (uint16_t)elapsed); n += 6; /* elapsed time (1/100 s) */
    put16(m + n, 6); put16(m + n + 2, 6); put16(m + n + 4, 23); put16(m + n + 6, 24); put16(m + n + 8, 32); n += 10;   /* ORO */
    put16(m, 546); put16(m + 2, 547); put16(m + 4, (uint16_t)n); put16(m + 6, 0);
    static const struct ip6a servers = {{0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2}};
    uint16_t sum = ip6_checksum(&src, &servers, IPPROTO_UDP, m, n); put16(m + 6, sum ? sum : 0xFFFF);
    struct ip6_send_options o; ip6_options_default(&o); o.source = &src; o.oif = dev->index; o.hop_limit = 1; o.mc_loop = false;
    ip6_send(&servers, IPPROTO_UDP, m, n, &o);
}
void dhcp6_poll(uint64_t now) {
    for (unsigned i = 0; i < 4; i++) {
        struct dhcp6* c = &clients[i]; if (!c->used) continue;
        struct netdev* dev = netdev_by_index(c->ifindex);
        if (!dev || !netdev_running(dev)) { c->active = false; continue; }
        if (c->done && !c->active && c->refresh_ns && now >= c->refresh_ns) dhcp6_start(dev, false);
        if (!c->active || now < c->next_ns) continue;
        send_request(dev, c, now);
        /* RT = 2 RT + RAND * RT, RAND in [-0.1, 0.1]; IRT 1 s, MRT 3600 s */
        uint64_t rt = c->rt_ns ? 2 * c->rt_ns : 1000000000ULL;
        if (rt > 3600000000000ULL) rt = 3600000000000ULL;
        uint64_t jitter = ndisc_random_ns(rt / 5 + 1);
        rt = rt - rt / 10 + jitter;
        c->rt_ns = rt; c->next_ns = now + rt; c->attempts++;
    }
}
static void label_text(const uint8_t* p, unsigned length, unsigned* at, char* out) {
    unsigned n = 0; out[0] = 0;
    while (*at < length) {
        unsigned label = p[(*at)++];
        if (!label) break;
        if (label > 63 || *at + label > length || n + label + 2 > 63) { out[0] = 0; *at = length; return; }
        if (n) out[n++] = '.';
        for (unsigned i = 0; i < label; i++) { char ch = (char)p[*at + i]; out[n++] = ch > ' ' && ch < 127 ? ch : '_'; }
        *at += label;
    }
    out[n] = 0;
}
void dhcp6_receive(struct netdev* dev, const struct ip6a* src, const uint8_t* data, unsigned length) {
    (void)src;
    struct dhcp6* c = client_for(dev, false);
    if (!c || !c->active || length < 4 || data[0] != 7 || memcmp(data + 1, c->xid, 3)) return;   /* REPLY with our id */
    struct ip6a servers[SERVERS]; unsigned count = 0, domain_count = 0; char domains[DOMAINS][64];
    uint32_t refresh = 86400; bool status_ok = true;
    for (unsigned at = 4; at + 4 <= length;) {
        unsigned code = be16(data + at), size = be16(data + at + 2);
        if (size > length - at - 4) return;   /* malformed: ignored entirely */
        const uint8_t* value = data + at + 4;
        if (code == 23) for (unsigned i = 0; i + 16 <= size && count < SERVERS; i += 16) { memcpy(servers[count].b, value + i, 16); if (!ip6a_multicast(&servers[count]) && !ip6a_any(&servers[count])) count++; }
        else if (code == 24) { unsigned p = 0; while (p < size && domain_count < DOMAINS) { label_text(value, size, &p, domains[domain_count]); if (domains[domain_count][0]) domain_count++; } }
        else if (code == 32 && size == 4) refresh = be32(value);
        else if (code == 13 && size >= 2 && be16(value)) status_ok = false;   /* status code: failure */
        at += 4 + size;
    }
    if (!status_ok) return;
    if (refresh < 600) refresh = 600;
    c->server_count = count; memcpy(c->servers, servers, sizeof(servers));
    c->domain_count = domain_count; memcpy(c->domains, domains, sizeof(domains));
    c->active = false; c->done = true;
    c->refresh_ns = refresh == LIFETIME_INFINITE ? ~0ULL : native_clock_ns() + (uint64_t)refresh * 1000000000ULL;
}
unsigned dhcp6_dns(struct ip6a* out, unsigned capacity, char domains[][64], unsigned* domain_count) {
    unsigned n = 0, d = 0, limit = domain_count ? *domain_count : 0;
    for (unsigned i = 0; i < 4; i++) {
        if (!clients[i].used || !clients[i].done) continue;
        for (unsigned j = 0; j < clients[i].server_count && n < capacity; j++) out[n++] = clients[i].servers[j];
        for (unsigned j = 0; j < clients[i].domain_count && d < limit; j++) strcpy(domains[d++], clients[i].domains[j]);
    }
    if (domain_count) *domain_count = d;
    return n;
}
void dhcp6_dev_down(struct netdev* dev) { struct dhcp6* c = client_for(dev, false); if (c) memset(c, 0, sizeof(*c)); }
