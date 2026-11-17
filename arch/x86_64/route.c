/* Routing for both families: the routing tables (local 255, main 254, default 253 and any numbered table), the
 * policy rules that pick a table (priority, from/to prefixes, fwmark/mask, iif/oif, "not", suppress_prefixlength,
 * as `ip rule`), longest-prefix-match lookup with metrics and IPv6 router preference/reachability, RFC 6724 source
 * address selection, the path-MTU cache (ICMP "fragmentation needed" / "packet too big") and IPv6 redirect
 * exceptions. Routes are IPv4-mapped for AF_INET. */
#include "netdev.h"
#include "../../kernel/string.h"

struct route* routes;
struct rule* rules;
#define ROUTES_MAX 4096
#define RULES_MAX 256
#define PMTU_ENTRIES 128
#define EXCEPTIONS 64
static unsigned route_count, rule_count;
static struct { uint8_t family; struct ip6a dst; unsigned mtu; uint64_t expires_ns, used_ns; } pmtu[PMTU_ENTRIES];
static struct { struct ip6a dst, target; unsigned oif; uint64_t expires_ns; bool used; } redirects[EXCEPTIONS];

static bool route_same(const struct route* a, const struct route* b) {
    if (a->family != b->family || a->table != b->table || a->plen != b->plen || !ip6a_eq(&a->dst, &b->dst) || a->metric != b->metric) return false;
    if (a->src_len != b->src_len || !ip6a_eq(&a->src, &b->src)) return false;
    if (a->family == AF_INET6) return ip6a_eq(&a->gateway, &b->gateway) && a->oif == b->oif && a->type == b->type;
    return true;   /* IPv4: one route per (table, prefix, tos, priority) */
}
/* IPv4 routes keep their prefixes v4-mapped (::ffff:a.b.c.d, matched over 96 + plen bits) and "no gateway / no
 * preferred source" as ::, whatever spelling the caller used */
static void normalize4(struct route* r) {
    if (r->family != AF_INET) return;
    r->dst = ip6a_from4(ip6a_v4(&r->dst)); r->src = ip6a_from4(ip6a_v4(&r->src));
    r->gateway = ip6a_v4(&r->gateway) ? ip6a_from4(ip6a_v4(&r->gateway)) : ip6a_unspecified;
    r->prefsrc = ip6a_v4(&r->prefsrc) ? ip6a_from4(ip6a_v4(&r->prefsrc)) : ip6a_unspecified;
}
int route_add(const struct route* route, bool replace, bool exclusive, bool notify) {
    if (route->plen > (route->family == AF_INET ? 32 : 128) || (route->family != AF_INET && route->family != AF_INET6)) return -22;
    struct route candidate = *route; normalize4(&candidate);
    if (route->family == AF_INET) {
        /* IPv4 prefixes are stored as ::ffff:a.b.c.d/96+plen internally but keep plen 0..32 */
        uint32_t v4 = ip6a_v4(&candidate.dst), mask = candidate.plen ? 0xFFFFFFFFU << (32 - candidate.plen) : 0;
        if (v4 & ~mask) return -22;    /* Linux: "Invalid prefix for given prefix length" */
    } else {
        struct ip6a masked = candidate.dst; ip6a_mask(&masked, candidate.plen);
        if (!ip6a_eq(&masked, &candidate.dst)) return -22;
    }
    if (!candidate.table) candidate.table = RT_TABLE_MAIN;
    if (!candidate.type) candidate.type = RTN_UNICAST;
    if (candidate.family == AF_INET6 && !candidate.metric && candidate.type == RTN_UNICAST) candidate.metric = 1024;
    for (struct route* r = routes; r; r = r->next) {
        if (!route_same(r, &candidate)) continue;
        if (exclusive && !replace) return -17;
        if (!replace) return -17;
        struct route* next = r->next; uint64_t created = r->created_ns;
        if (notify) rtnl_notify_route(r, 25 /* RTM_DELROUTE */);
        *r = candidate; r->next = next; r->created_ns = created;
        if (notify) rtnl_notify_route(r, 24 /* RTM_NEWROUTE */);
        return 0;
    }
    if (route_count >= ROUTES_MAX) return -105;
    struct route* r = net_alloc(sizeof(*r)); if (!r) return -12;
    *r = candidate; r->created_ns = native_clock_ns();
    /* Keep the list ordered by prefix length (longest first) so lookups can stop early per table. */
    struct route** link = &routes;
    while (*link && (*link)->plen >= r->plen) link = &(*link)->next;
    r->next = *link; *link = r; route_count++;
    if (notify) rtnl_notify_route(r, 24);
    return 0;
}
static bool route_matches(const struct route* r, const struct route* m) {
    if (r->family != m->family || r->plen != m->plen || !ip6a_eq(&r->dst, &m->dst)) return false;
    if (m->table && r->table != m->table) return false;
    if (!ip6a_any(&m->gateway) && !ip6a_eq(&r->gateway, &m->gateway)) return false;
    if (m->family == AF_INET && ip6a_is4(&m->gateway) && ip6a_v4(&m->gateway) && !ip6a_eq(&r->gateway, &m->gateway)) return false;
    if (m->oif && r->oif != m->oif) return false;
    if (m->metric && r->metric != m->metric) return false;
    if (m->protocol && r->protocol != m->protocol) return false;
    if (m->type && r->type != m->type) return false;
    if (m->src_len && (r->src_len != m->src_len || !ip6a_eq(&r->src, &m->src))) return false;
    return true;
}
static void route_unlink(struct route** link, bool notify) {
    struct route* r = *link; *link = r->next; route_count--;
    if (notify) rtnl_notify_route(r, 25);
    net_free(r, sizeof(*r));
}
int route_del(const struct route* match, bool notify) {
    struct route m = *match; normalize4(&m);
    for (struct route** link = &routes; *link; link = &(*link)->next)
        if (route_matches(*link, &m)) { route_unlink(link, notify); return 0; }
    return -3;   /* ESRCH */
}
void route_flush_dev(struct netdev* dev, uint8_t family) {
    for (struct route** link = &routes; *link;) {
        struct route* r = *link;
        if (r->oif == dev->index && (!family || r->family == family)) route_unlink(link, true); else link = &r->next;
    }
    for (unsigned i = 0; i < EXCEPTIONS; i++) if (redirects[i].used && redirects[i].oif == dev->index) redirects[i].used = false;
}
void route_flush_proto(struct netdev* dev, uint8_t family, uint8_t protocol) {
    for (struct route** link = &routes; *link;) {
        struct route* r = *link;
        if ((!dev || r->oif == dev->index) && r->family == family && r->protocol == protocol) route_unlink(link, true); else link = &r->next;
    }
}

/* ---- rules ---- */
#define FR_ACT_TO_TBL 1
#define FR_ACT_GOTO 2
#define FR_ACT_NOP 3
#define FR_ACT_BLACKHOLE 6
#define FR_ACT_UNREACHABLE 7
#define FR_ACT_PROHIBIT 8
static bool rule_same(const struct rule* a, const struct rule* b) {
    return a->family == b->family && a->priority == b->priority && a->table == b->table && a->action == b->action &&
           a->invert == b->invert && a->src_len == b->src_len && a->dst_len == b->dst_len && ip6a_eq(&a->src, &b->src) &&
           ip6a_eq(&a->dst, &b->dst) && a->fwmark == b->fwmark && a->fwmask == b->fwmask && !strcmp(a->iifname, b->iifname) &&
           !strcmp(a->oifname, b->oifname) && a->suppress_prefixlen == b->suppress_prefixlen && a->ip_proto == b->ip_proto &&
           a->sport_lo == b->sport_lo && a->sport_hi == b->sport_hi && a->dport_lo == b->dport_lo && a->dport_hi == b->dport_hi;
}
int rule_add(const struct rule* rule, bool notify) {
    if (rule->family != AF_INET && rule->family != AF_INET6) return -97;
    for (struct rule* r = rules; r; r = r->next) if (rule_same(r, rule)) return -17;
    if (rule_count >= RULES_MAX) return -105;
    struct rule* r = net_alloc(sizeof(*r)); if (!r) return -12;
    *r = *rule;
    if (!r->priority) {   /* Linux: no priority -> one below the first non-zero rule of the family */
        uint32_t lowest = 32766;
        for (struct rule* o = rules; o; o = o->next) if (o->family == r->family && o->priority && o->priority < lowest) lowest = o->priority;
        r->priority = lowest ? lowest - 1 : 0;
    }
    struct rule** link = &rules;
    while (*link && (*link)->priority <= r->priority) link = &(*link)->next;
    r->next = *link; *link = r; rule_count++;
    if (notify) rtnl_notify_rule(r, 32 /* RTM_NEWRULE */);
    return 0;
}
int rule_del(const struct rule* match, bool notify) {
    for (struct rule** link = &rules; *link; link = &(*link)->next) {
        struct rule* r = *link;
        if (r->family != match->family) continue;
        if (match->priority && r->priority != match->priority) continue;
        if (match->table && r->table != match->table) continue;
        if (match->src_len && (r->src_len != match->src_len || !ip6a_eq(&r->src, &match->src))) continue;
        if (match->dst_len && (r->dst_len != match->dst_len || !ip6a_eq(&r->dst, &match->dst))) continue;
        if (match->fwmark && r->fwmark != match->fwmark) continue;
        if (match->iifname[0] && strcmp(r->iifname, match->iifname)) continue;
        if (match->oifname[0] && strcmp(r->oifname, match->oifname)) continue;
        if (match->invert && !r->invert) continue;
        if (match->suppress_prefixlen >= 0 && match->suppress_prefixlen != r->suppress_prefixlen && match->suppress_prefixlen != -2) continue;
        *link = r->next; rule_count--;
        if (notify) rtnl_notify_rule(r, 33 /* RTM_DELRULE */);
        net_free(r, sizeof(*r));
        return 0;
    }
    return -2;
}

/* ---- lookup ---- */
static bool prefix_match(uint8_t family, const struct ip6a* address, const struct ip6a* prefix, unsigned plen) {
    return family == AF_INET ? ip6a_prefix_eq(address, prefix, 96 + plen) : ip6a_prefix_eq(address, prefix, plen);
}
static int neigh_score(const struct route* r) {
    if (r->family != AF_INET6 || ip6a_any(&r->gateway)) return 2;
    struct netdev* dev = netdev_by_index(r->oif); if (!dev) return 0;
    struct neighbour* n = neigh_lookup(dev, AF_INET6, &r->gateway);
    if (!n) return 1;                                   /* unknown: try it (round robin would probe) */
    if (n->state & (NUD_FAILED)) return 0;
    if (n->state & (NUD_INCOMPLETE)) return 1;
    return 2;
}
static int pref_value(uint8_t pref) { return pref == 1 ? 3 : pref == 3 ? 1 : 2; }  /* RFC 4191: 01 high, 00 medium, 11 low */
static const struct route* table_lookup(uint32_t table, uint8_t family, const struct ip6a* dst, const struct ip6a* src, unsigned oif, uint64_t now) {
    const struct route* best = NULL; int best_score = -1;
    for (const struct route* r = routes; r; r = r->next) {
        if (best && r->plen < best->plen) break;       /* sorted by prefix length */
        if (r->family != family || r->table != table) continue;
        if (r->expires_ns && r->expires_ns <= now) continue;
        if (!prefix_match(family, dst, &r->dst, r->plen)) continue;
        if (r->src_len && (!src || !prefix_match(family, src, &r->src, r->src_len))) continue;
        if (oif && r->oif && r->oif != oif && r->type != RTN_LOCAL) continue;
        struct netdev* dev = r->oif ? netdev_by_index(r->oif) : NULL;
        if (r->oif && (!dev || !(dev->flags & IFF_UP))) continue;
        int score = neigh_score(r) * 4 + pref_value(r->pref);
        if (best && r->plen == best->plen && (r->metric > best->metric || (r->metric == best->metric && score <= best_score))) continue;
        best = r; best_score = score;
    }
    return best;
}
static bool rule_selects(const struct rule* r, uint8_t family, const struct ip6a* dst, const struct ip6a* src, const char* iif,
                         unsigned oif, uint32_t mark) {
    bool match = true;
    if (r->src_len && (!src || !prefix_match(family, src, &r->src, r->src_len))) match = false;
    if (match && r->dst_len && !prefix_match(family, dst, &r->dst, r->dst_len)) match = false;
    if (match && (r->fwmark || r->fwmask) && ((mark ^ r->fwmark) & (r->fwmask ? r->fwmask : 0xFFFFFFFFU))) match = false;
    if (match && r->iifname[0] && (!iif || strcmp(iif, r->iifname))) match = false;
    if (match && r->oifname[0]) { struct netdev* d = oif ? netdev_by_index(oif) : NULL; if (!d || strcmp(d->name, r->oifname)) match = false; }
    return r->invert ? !match : match;
}
static int resolve(uint8_t family, const struct ip6a* dst, const struct ip6a* src, unsigned oif, uint32_t mark, const char* iif,
                   const struct route** found, uint32_t* found_table) {
    uint64_t now = native_clock_ns();
    for (const struct rule* r = rules; r; r = r->next) {
        if (r->family != family || !rule_selects(r, family, dst, src, iif, oif, mark)) continue;
        if (r->action == FR_ACT_BLACKHOLE) return -22;
        if (r->action == FR_ACT_UNREACHABLE) return -101;
        if (r->action == FR_ACT_PROHIBIT) return -13;
        if (r->action != FR_ACT_TO_TBL) continue;
        const struct route* route = table_lookup(r->table, family, dst, src, oif, now);
        if (!route) continue;
        if (route->type == RTN_THROW) continue;
        if (r->suppress_prefixlen >= 0 && route->plen <= (unsigned)r->suppress_prefixlen) continue;
        if (route->type == RTN_BLACKHOLE) return -22;
        if (route->type == RTN_UNREACHABLE) return -113;
        if (route->type == RTN_PROHIBIT) return -13;
        *found = route; *found_table = r->table; return 0;
    }
    return -101;
}
static unsigned redirect_oif(const struct ip6a* dst, struct ip6a* target) {
    uint64_t now = native_clock_ns();
    for (unsigned i = 0; i < EXCEPTIONS; i++)
        if (redirects[i].used && redirects[i].expires_ns > now && ip6a_eq(&redirects[i].dst, dst)) { *target = redirects[i].target; return redirects[i].oif; }
    return 0;
}
int route_lookup(uint8_t family, const struct ip6a* dst, const struct ip6a* src, unsigned oif, uint32_t mark, struct route_result* out) {
    memset(out, 0, sizeof(*out));
    if (family == AF_INET6 && (ip6a_multicast(dst) || ip6a_linklocal(dst)) && oif) {
        /* Link-scope and multicast destinations go out the given interface (scope id / IPV6_MULTICAST_IF). */
        struct netdev* dev = netdev_by_index(oif);
        if (!dev || !dev->present) return -19;
        out->dev = dev; out->nexthop = *dst; out->type = ip6a_multicast(dst) ? RTN_MULTICAST : RTN_UNICAST; out->table = RT_TABLE_MAIN;
        out->mtu = dev->mtu6;
        if (src && !ip6a_any(src)) out->source = *src; else if (!source_select6(dst, dev, 0, &out->source)) out->source = ip6a_unspecified;
        return 0;
    }
    const struct route* route = NULL; uint32_t table = 0;
    struct ip6a redirect_target; unsigned redirected = family == AF_INET6 ? redirect_oif(dst, &redirect_target) : 0;
    int error = resolve(family, dst, src, oif, mark, NULL, &route, &table);
    if (error) return error;
    struct netdev* dev = route->oif ? netdev_by_index(route->oif) : NULL;
    if (route->type == RTN_LOCAL) dev = netdev_lo;
    if (!dev) return -101;
    out->dev = dev; out->route = route; out->table = table; out->type = route->type;
    out->gateway = family == AF_INET ? ip6a_v4(&route->gateway) != 0 : !ip6a_any(&route->gateway);
    out->nexthop = out->gateway ? route->gateway : *dst;
    if (redirected && redirected == dev->index) { out->nexthop = redirect_target; out->gateway = !ip6a_eq(&redirect_target, dst); }
    if (src && !ip6a_any(src) && !(family == AF_INET && !ip6a_v4(src))) out->source = *src;
    else if (family == AF_INET) {
        uint32_t chosen = ip6a_is4(&route->prefsrc) && ip6a_v4(&route->prefsrc) ? ip6a_v4(&route->prefsrc) : source_select4(ip6a_v4(dst), dev);
        out->source = ip6a_from4(chosen);
    } else if (!ip6a_any(&route->prefsrc)) out->source = route->prefsrc;
    else if (!source_select6(dst, route->type == RTN_LOCAL ? NULL : dev, 0, &out->source)) out->source = ip6a_unspecified;
    unsigned mtu = route->mtu ? route->mtu : family == AF_INET6 ? dev->mtu6 : dev->mtu;
    unsigned cached = pmtu_get(family, dst); if (cached && cached < mtu) mtu = cached;
    out->mtu = mtu;
    return 0;
}
int route_lookup_input(uint8_t family, const struct ip6a* dst, const struct ip6a* src, unsigned iif, uint32_t mark, struct route_result* out) {
    memset(out, 0, sizeof(*out));
    struct netdev* in = iif ? netdev_by_index(iif) : NULL;
    const struct route* route = NULL; uint32_t table = 0;
    int error = resolve(family, dst, src, 0, mark, in ? in->name : NULL, &route, &table);
    if (error) return error;
    out->route = route; out->table = table; out->type = route->type;
    out->dev = route->type == RTN_LOCAL ? netdev_lo : netdev_by_index(route->oif);
    return 0;
}

/* ---- source address selection ---- */
uint32_t source_select4(uint32_t dst, struct netdev* dev) {
    if ((dst >> 24) == 127) return 0x7F000001U;
    if (dev) {
        uint32_t first = 0;
        for (unsigned i = 0; i < NETDEV_V4_MAX; i++) {
            const struct netdev_v4* a = &dev->v4[i]; if (!a->used) continue;
            uint32_t mask = a->plen ? 0xFFFFFFFFU << (32 - a->plen) : 0;
            if (a->plen && ((a->address ^ dst) & mask) == 0) return a->address;
            if ((a->peer && a->peer == dst)) return a->address;
            if (!first && !a->secondary) first = a->address;
        }
        if (first) return first;
    }
    for (struct netdev* d = netdevs; d; d = d->next)   /* inet_select_addr: any global address of another interface */
        for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && d->v4[i].scope == RT_SCOPE_UNIVERSE) return d->v4[i].address;
    return 0;
}
static unsigned address_scope(const struct ip6a* a) {
    if (ip6a_multicast(a)) return ip6a_mc_scope(a);
    if (ip6a_linklocal(a) || ip6a_loopback(a)) return 2;
    if (a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0xC0) return 5;
    return 14;
}
/* RFC 6724 default policy table: (prefix, length, precedence, label) */
static const struct { uint8_t prefix[16]; uint8_t length, precedence, label; } policy[] = {
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 128, 50, 0},
    {{0}, 0, 40, 1},
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}, 96, 35, 4},
    {{0x20, 0x02}, 16, 30, 2},
    {{0x20, 0x01, 0, 0}, 32, 5, 5},
    {{0xFC}, 7, 3, 13},
    {{0}, 96, 1, 3},
    {{0xFE, 0xC0}, 10, 1, 11},
    {{0x3F, 0xFE}, 16, 1, 12},
};
static unsigned policy_label(const struct ip6a* a) {
    int best = -1; unsigned best_length = 0;
    for (unsigned i = 0; i < sizeof(policy) / sizeof(policy[0]); i++) {
        struct ip6a p; memcpy(p.b, policy[i].prefix, 16);
        if (ip6a_prefix_eq(a, &p, policy[i].length) && (best < 0 || policy[i].length > best_length)) { best = (int)i; best_length = policy[i].length; }
    }
    return best < 0 ? 1 : policy[best].label;
}
static unsigned common_prefix(const struct ip6a* a, const struct ip6a* b) {
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) {
        uint8_t x = a->b[i] ^ b->b[i];
        if (!x) { n += 8; continue; }
        while (!(x & 0x80)) { n++; x <<= 1; }
        break;
    }
    return n > 64 ? 64 : n;   /* RFC 6724 rule 8 only compares up to the prefix length (64) */
}
#define IPV6_PREFER_SRC_TMP 0x0001U
#define IPV6_PREFER_SRC_PUBLIC 0x0002U
/* Is candidate `a` better than `b` for `dst` (RFC 6724 section 5)? */
static bool source_better(const struct inet6_ifaddr* a, struct netdev* adev, const struct inet6_ifaddr* b, struct netdev* bdev,
                          const struct ip6a* dst, struct netdev* out, uint32_t preferences) {
    if (ip6a_eq(&a->addr, dst) != ip6a_eq(&b->addr, dst)) return ip6a_eq(&a->addr, dst);           /* rule 1 */
    unsigned sa = address_scope(&a->addr), sb = address_scope(&b->addr), sd = address_scope(dst);
    if (sa != sb) return sa < sb ? sa >= sd : sb < sd;                                                 /* rule 2 */
    bool da = (a->flags & IFA_F_DEPRECATED) != 0, db = (b->flags & IFA_F_DEPRECATED) != 0;
    if (da != db) return !da;                                                                          /* rule 3 */
    if (out && (adev == out) != (bdev == out)) return adev == out;                                     /* rule 5 */
    unsigned ld = policy_label(dst); bool la = policy_label(&a->addr) == ld, lb = policy_label(&b->addr) == ld;
    if (la != lb) return la;                                                                           /* rule 6 */
    bool prefer_temp = preferences & IPV6_PREFER_SRC_PUBLIC ? false : preferences & IPV6_PREFER_SRC_TMP ? true :
                       (adev->conf6.use_tempaddr >= 2);
    bool ta = (a->flags & IFA_F_TEMPORARY) != 0, tb = (b->flags & IFA_F_TEMPORARY) != 0;
    if (ta != tb) return prefer_temp ? ta : tb;                                                        /* rule 7 */
    return common_prefix(&a->addr, dst) > common_prefix(&b->addr, dst);                                /* rule 8 */
}
bool source_select6(const struct ip6a* dst, struct netdev* dev, uint32_t preferences, struct ip6a* out) {
    const struct inet6_ifaddr* best = NULL; struct netdev* best_dev = NULL;
    bool link_scope = ip6a_linklocal(dst) || (ip6a_multicast(dst) && ip6a_mc_scope(dst) <= 2);
    if (ip6a_loopback(dst)) { *out = ip6a_loopback_address; return true; }
    for (struct netdev* d = netdevs; d; d = d->next) {
        if (link_scope && dev && d != dev) continue;    /* link-scope destinations: only the outgoing interface */
        if (!netdev_ipv6_enabled(d)) continue;
        for (const struct inet6_ifaddr* a = d->addrs6; a; a = a->next) {
            if (a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED)) continue;
            if (ip6a_loopback(&a->addr) && !ip6a_loopback(dst)) continue;
            if (a->prefered_lft == 0 && a->valid_lft == 0) continue;
            if (!best || source_better(a, d, best, best_dev, dst, dev, preferences)) { best = a; best_dev = d; }
        }
    }
    if (!best) return false;
    *out = best->addr; return true;
}

/* ---- path MTU and redirects ---- */
unsigned pmtu_get(uint8_t family, const struct ip6a* dst) {
    uint64_t now = native_clock_ns();
    for (unsigned i = 0; i < PMTU_ENTRIES; i++)
        if (pmtu[i].mtu && pmtu[i].family == family && pmtu[i].expires_ns > now && ip6a_eq(&pmtu[i].dst, dst)) { pmtu[i].used_ns = now; return pmtu[i].mtu; }
    return 0;
}
void pmtu_update(uint8_t family, const struct ip6a* dst, unsigned mtu) {
    unsigned floor = family == AF_INET6 ? 1280 : 552;   /* IPv6 minimum link MTU; Linux min_pmtu for IPv4 */
    if (mtu < floor) mtu = floor;
    uint64_t now = native_clock_ns(); unsigned victim = 0;
    for (unsigned i = 0; i < PMTU_ENTRIES; i++) {
        if (pmtu[i].mtu && pmtu[i].family == family && ip6a_eq(&pmtu[i].dst, dst)) { victim = i; goto store; }
        if (!pmtu[i].mtu || pmtu[i].expires_ns <= now) { victim = i; continue; }
        if (pmtu[victim].mtu && pmtu[victim].expires_ns > now && pmtu[i].used_ns < pmtu[victim].used_ns) victim = i;
    }
store:
    if (pmtu[victim].mtu && pmtu[victim].family == family && ip6a_eq(&pmtu[victim].dst, dst) && pmtu[victim].expires_ns > now && mtu >= pmtu[victim].mtu) return;
    pmtu[victim].family = family; pmtu[victim].dst = *dst; pmtu[victim].mtu = mtu;
    pmtu[victim].expires_ns = now + 600000000000ULL; pmtu[victim].used_ns = now;   /* mtu_expires: 10 minutes */
}
unsigned route_mtu(uint8_t family, const struct ip6a* dst, struct netdev* dev) {
    struct route_result result;
    if (route_lookup(family, dst, NULL, dev ? dev->index : 0, 0, &result)) return family == AF_INET6 ? 1280 : 576;
    return result.mtu;
}
void route_redirect6(const struct ip6a* dst, const struct ip6a* target, struct netdev* dev) {
    unsigned slot = 0; uint64_t now = native_clock_ns();
    for (unsigned i = 0; i < EXCEPTIONS; i++) {
        if (redirects[i].used && ip6a_eq(&redirects[i].dst, dst)) { slot = i; break; }
        if (!redirects[i].used || redirects[i].expires_ns <= now) slot = i;
    }
    redirects[slot].used = true; redirects[slot].dst = *dst; redirects[slot].target = *target; redirects[slot].oif = dev->index;
    redirects[slot].expires_ns = now + 600000000000ULL;
}
uint16_t ip_route_mss(uint32_t local, uint32_t remote) {
    (void)local;
    if (ip6h_is(remote)) {
        struct ip6a address; uint32_t scope;
        if (!ip6h_address(remote, &address, &scope)) return 1220;
        unsigned mtu = ip6a_loopback(&address) || addrconf_local(&address, scope, true) ? 1500 : route_mtu(AF_INET6, &address, scope ? netdev_by_index(scope) : NULL);
        if (mtu > 1500) mtu = 1500;
        return (uint16_t)(mtu < 1280 ? 1220 : mtu - 60);
    }
    struct ip6a address = ip6a_from4(remote);
    unsigned mtu = (remote >> 24) == 127 || netdev_v4_local(remote) ? 1500 : route_mtu(AF_INET, &address, NULL);
    if (mtu > 1500) mtu = 1500;
    return (uint16_t)(mtu < 576 ? 536 : mtu - 40);
}
void route_poll(uint64_t now) {
    static uint64_t next; if (now < next) return; next = now + 1000000000ULL;
    for (struct route** link = &routes; *link;) {
        struct route* r = *link;
        if (r->expires_ns && r->expires_ns <= now) route_unlink(link, true); else link = &r->next;
    }
    for (unsigned i = 0; i < EXCEPTIONS; i++) if (redirects[i].used && redirects[i].expires_ns <= now) redirects[i].used = false;
}
void route_init(void) {
    static const struct { uint8_t family; uint32_t priority, table; } defaults[] = {
        {AF_INET, 0, RT_TABLE_LOCAL}, {AF_INET, 32766, RT_TABLE_MAIN}, {AF_INET, 32767, RT_TABLE_DEFAULT},
        {AF_INET6, 0, RT_TABLE_LOCAL}, {AF_INET6, 32766, RT_TABLE_MAIN},
    };
    for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        struct rule* r = net_alloc(sizeof(*r)); if (!r) panic("routing rules");
        memset(r, 0, sizeof(*r));
        r->family = defaults[i].family; r->priority = defaults[i].priority; r->table = defaults[i].table; r->action = FR_ACT_TO_TBL;
        r->suppress_prefixlen = -1; r->suppress_ifgroup = -1; r->protocol = RTPROT_KERNEL;
        struct rule** link = &rules;
        while (*link && (*link)->priority <= r->priority) link = &(*link)->next;
        r->next = *link; *link = r; rule_count++;
    }
}
