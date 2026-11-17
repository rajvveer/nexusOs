/* IPv6 Neighbor Discovery (RFC 4861), stateless address autoconfiguration (RFC 4862), temporary addresses
 * (RFC 8981), stable-privacy interface identifiers (RFC 7217), DNS options (RFC 8106), router preferences
 * (RFC 4191) - and the neighbour table for every family: IPv6 entries, ARP entries of TAP-style Ethernet devices
 * (eth0's ARP stays in kernel/arp.c), and administrative entries (`ip neigh add`).
 *
 * Neighbour states follow RFC 4861 7.3 with Linux values: INCOMPLETE (3 multicast solicitations a retransmit
 * timer apart, up to 3 packets queued), REACHABLE (randomized 0.5-1.5 x base reachable time), STALE, DELAY (5 s),
 * PROBE (3 unicast solicitations), FAILED (queued packets are reported back as "address unreachable"), NOARP,
 * PERMANENT. Addresses: link-local from EUI-64, stable privacy or random identifiers (addr_gen_mode), DAD with
 * the RFC 7527 nonce (looped-back probes ignored), SLAAC from prefix information (RFC 4862 5.5.3 two-hour rule),
 * temporary addresses with desync factor and regeneration ahead of deprecation, lifetimes in seconds. */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/arp.h"
#include "../../kernel/string.h"

struct neighbour* neighbours;
static unsigned neighbour_count;
#define NEIGH_MAX 1024
#define QUEUE_MAX 3
#define MAX_MULTICAST_SOLICIT 3
#define MAX_UNICAST_SOLICIT 3
#define DELAY_FIRST_PROBE_NS 5000000000ULL
#define TEMP_IDGEN_RETRIES 3
#define DNS_SERVERS 8
struct rdnss { struct ip6a address; unsigned ifindex; uint64_t expires_ns; bool used; };
struct dnssl { char name[64]; unsigned ifindex; uint64_t expires_ns; bool used; };
static struct rdnss rdnss[DNS_SERVERS];
static struct dnssl dnssl[DNS_SERVERS];
static uint8_t boot_secret[16];
static bool boot_secret_ready;

uint64_t ndisc_random_ns(uint64_t span) { if (!span) return 0; uint64_t r = (uint64_t)net_random32() << 32 | net_random32(); return r % span; }
static uint32_t reachable_random(uint32_t base) { return base / 2 + (uint32_t)(ndisc_random_ns(base) & 0xFFFFFFFFU); }

/* ---- neighbour table ---- */
struct neighbour* neigh_lookup(struct netdev* dev, uint8_t family, const struct ip6a* key) {
    for (struct neighbour* n = neighbours; n; n = n->next) if (n->dev == dev && n->family == family && ip6a_eq(&n->key, key)) return n;
    return NULL;
}
static void queue_drop(struct neighbour* n, bool report) {
    while (n->queue) {
        struct neigh_pending* q = n->queue; n->queue = q->next;
        if (report && n->family == AF_INET6) icmp6_local_error(n->dev, 1, 3, 0, q->data, q->length);
        n->dev->stats.tx_dropped++;
        net_free(q, sizeof(*q) + q->length);
    }
    n->queued = 0;
}
static void neigh_free(struct neighbour** link) {
    struct neighbour* n = *link; *link = n->next; neighbour_count--;
    queue_drop(n, false); net_free(n, sizeof(*n));
}
static bool gc_one(void) {
    struct neighbour** victim = NULL;
    for (struct neighbour** link = &neighbours; *link; link = &(*link)->next) {
        struct neighbour* n = *link;
        if (n->state & (NUD_PERMANENT | NUD_NOARP)) continue;
        if (n->refcnt) continue;
        if (!victim || (n->state & NUD_FAILED) || n->used_ns < (*victim)->used_ns) victim = link;
    }
    if (!victim) return false;
    rtnl_notify_neigh(*victim, 29 /* RTM_DELNEIGH */);
    neigh_free(victim); return true;
}
static struct neighbour* neigh_create(struct netdev* dev, uint8_t family, const struct ip6a* key) {
    if (neighbour_count >= NEIGH_MAX && !gc_one()) return NULL;
    struct neighbour* n = net_alloc(sizeof(*n)); if (!n) return NULL;
    memset(n, 0, sizeof(*n)); n->dev = dev; n->family = family; n->key = *key; n->updated_ns = n->used_ns = native_clock_ns();
    n->next = neighbours; neighbours = n; neighbour_count++;
    return n;
}
static void send_ns(struct netdev* dev, const struct ip6a* target, const struct ip6a* dst, const struct ip6a* src, bool slla, const uint8_t* nonce) {
    uint8_t body[4 + 16 + 8 + 8] = {0}; unsigned length = 20;
    memcpy(body + 4, target->b, 16);
    if (slla && dev->type == ARPHRD_ETHER) { body[length] = 1; body[length + 1] = 1; memcpy(body + length + 2, dev->addr, 6); length += 8; }
    if (nonce) { body[length] = 14; body[length + 1] = 1; memcpy(body + length + 2, nonce, 6); length += 8; }
    icmp6_send(dst, src, dev, 135, 0, body, length, 255);
}
static bool own_source(struct netdev* dev, const struct ip6a* hint, struct ip6a* out) {
    if (hint && !ip6a_any(hint)) { struct inet6_ifaddr* a = addrconf_find(dev, hint); if (a && !(a->flags & IFA_F_TENTATIVE)) { *out = *hint; return true; } }
    for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next) if (ip6a_linklocal(&a->addr) && !(a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED))) { *out = a->addr; return true; }
    for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next) if (!(a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED))) { *out = a->addr; return true; }
    return false;
}
static void solicit(struct neighbour* n, bool unicast, const struct ip6a* source_hint) {
    struct netdev* dev = n->dev;
    if (n->family == AF_INET) {
        /* ARP request (TAP-style devices) */
        uint8_t frame[42]; memset(frame, 0, sizeof(frame));
        if (unicast && n->has_lladdr) memcpy(frame, n->lladdr, 6); else memset(frame, 0xFF, 6);
        memcpy(frame + 6, dev->addr, 6); put16(frame + 12, ETH_TYPE_ARP);
        uint8_t* a = frame + 14; put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, 1);
        memcpy(a + 8, dev->addr, 6); put32(a + 14, netdev_v4_primary(dev)); put32(a + 24, ip6a_v4(&n->key));
        netdev_xmit(dev, frame, sizeof(frame), ETH_TYPE_ARP);
        return;
    }
    struct ip6a src, dst;
    if (!own_source(dev, source_hint, &src)) return;
    if (unicast) dst = n->key; else ip6a_solicited(&n->key, &dst);
    send_ns(dev, &n->key, &dst, &src, true, NULL);
}
static void flush_queue(struct neighbour* n) {
    while (n->queue) {
        struct neigh_pending* q = n->queue; n->queue = q->next; n->queued--;
        uint8_t frame[ETH_HEADER_LEN + 1500];
        if (q->length <= 1500) {
            memcpy(frame, n->lladdr, 6); memcpy(frame + 6, n->dev->addr, 6); put16(frame + 12, q->protocol);
            memcpy(frame + ETH_HEADER_LEN, q->data, q->length);
            netdev_xmit(n->dev, frame, ETH_HEADER_LEN + q->length, q->protocol);
        }
        net_free(q, sizeof(*q) + q->length);
    }
}
static void neigh_set_state(struct neighbour* n, uint16_t state, uint64_t now) {
    bool changed = n->state != state;
    n->state = state; n->updated_ns = now;
    if (state == NUD_REACHABLE) { n->confirmed_ns = now; n->timer_ns = now + (uint64_t)n->dev->reachable_time_ms * 1000000ULL; }
    else if (state == NUD_DELAY) n->timer_ns = now + DELAY_FIRST_PROBE_NS;
    else if (state == NUD_PROBE || state == NUD_INCOMPLETE) n->timer_ns = now + (uint64_t)n->dev->retrans_ms * 1000000ULL;
    else n->timer_ns = 0;
    if (changed) rtnl_notify_neigh(n, 28 /* RTM_NEWNEIGH */);
}
int neigh_output(struct netdev* dev, uint8_t family, const struct ip6a* nexthop, const void* packet, unsigned length, uint16_t protocol) {
    if (length > 1500 || length > dev->mtu + 0u) return -90;
    uint64_t now = native_clock_ns();
    struct neighbour* n = neigh_lookup(dev, family, nexthop);
    if (!n) { n = neigh_create(dev, family, nexthop); if (!n) return -105; }
    n->used_ns = now;
    if (n->state & (NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE | NUD_PERMANENT | NUD_NOARP)) {
        if (n->state == NUD_STALE) neigh_set_state(n, NUD_DELAY, now);
        uint8_t frame[ETH_HEADER_LEN + 1500];
        memcpy(frame, n->lladdr, 6); memcpy(frame + 6, dev->addr, 6); put16(frame + 12, protocol);
        memcpy(frame + ETH_HEADER_LEN, packet, length);
        return netdev_xmit(dev, frame, ETH_HEADER_LEN + length, protocol);
    }
    /* INCOMPLETE (or FAILED / new): queue and solicit */
    struct neigh_pending* q = net_alloc(sizeof(*q) + length); if (!q) return -105;
    q->next = NULL; q->length = length; q->protocol = protocol; memcpy(q->data, packet, length);
    if (n->queued >= QUEUE_MAX) {   /* drop the oldest, as Linux's unres_qlen */
        struct neigh_pending* old = n->queue; n->queue = old->next; n->queued--; dev->stats.tx_dropped++;
        net_free(old, sizeof(*old) + old->length);
    }
    struct neigh_pending** tail = &n->queue; while (*tail) tail = &(*tail)->next; *tail = q; n->queued++;
    if (n->state != NUD_INCOMPLETE) {
        n->probes = 0; neigh_set_state(n, NUD_INCOMPLETE, now);
        const struct ip6a* hint = family == AF_INET6 && length >= 40 ? (const void*)((const uint8_t*)packet + 8) : NULL;
        solicit(n, false, hint); n->probes = 1;
    }
    return 0;
}
static void neigh_timers(uint64_t now) {
    for (struct neighbour** link = &neighbours; *link;) {
        struct neighbour* n = *link;
        if (n->timer_ns && now >= n->timer_ns) {
            switch (n->state) {
            case NUD_INCOMPLETE:
                if (n->probes < MAX_MULTICAST_SOLICIT) { solicit(n, false, NULL); n->probes++; n->timer_ns = now + (uint64_t)n->dev->retrans_ms * 1000000ULL; }
                else { queue_drop(n, true); neigh_set_state(n, NUD_FAILED, now); }
                break;
            case NUD_REACHABLE: neigh_set_state(n, NUD_STALE, now); break;
            case NUD_DELAY:
                if (n->confirmed_ns && now - n->confirmed_ns < (uint64_t)n->dev->reachable_time_ms * 1000000ULL) neigh_set_state(n, NUD_REACHABLE, now);
                else { n->probes = 0; neigh_set_state(n, NUD_PROBE, now); solicit(n, true, NULL); n->probes = 1; }
                break;
            case NUD_PROBE:
                if (n->probes < MAX_UNICAST_SOLICIT) { solicit(n, true, NULL); n->probes++; n->timer_ns = now + (uint64_t)n->dev->retrans_ms * 1000000ULL; }
                else { queue_drop(n, true); neigh_set_state(n, NUD_FAILED, now); }
                break;
            default: n->timer_ns = 0; break;
            }
        }
        /* garbage: failed or stale entries unused for a minute */
        if (!(n->state & (NUD_PERMANENT | NUD_NOARP)) && !n->refcnt && (n->state & (NUD_FAILED | NUD_STALE)) && now - n->used_ns > 60000000000ULL &&
            now - n->updated_ns > 60000000000ULL) {
            rtnl_notify_neigh(n, 29); neigh_free(link); continue;
        }
        link = &n->next;
    }
}
/* RFC 4861 7.2.5 (advertisement) / 7.2.3 (solicitation with a source link-layer address) / routers. */
static void neigh_learn(struct netdev* dev, uint8_t family, const struct ip6a* key, const uint8_t* lladdr, bool solicited, bool override,
                        bool advertisement, int is_router) {
    uint64_t now = native_clock_ns();
    struct neighbour* n = neigh_lookup(dev, family, key);
    if (n && (n->state & (NUD_PERMANENT | NUD_NOARP))) return;
    if (!n) {
        if (!lladdr) return;
        if (advertisement && family == AF_INET6) return;   /* unsolicited NA for an unknown entry: silently discarded */
        n = neigh_create(dev, family, key); if (!n) return;
        memcpy(n->lladdr, lladdr, 6); n->has_lladdr = true;
        if (is_router > 0) n->flags |= NTF_ROUTER;
        neigh_set_state(n, NUD_STALE, now);
        return;
    }
    bool differs = lladdr && (!n->has_lladdr || memcmp(lladdr, n->lladdr, 6));
    if (n->state == NUD_INCOMPLETE || n->state == NUD_FAILED || !n->has_lladdr) {
        if (!lladdr) return;
        memcpy(n->lladdr, lladdr, 6); n->has_lladdr = true;
        neigh_set_state(n, advertisement && solicited ? NUD_REACHABLE : NUD_STALE, now);
        flush_queue(n);
    } else if (!advertisement) {
        if (differs) { memcpy(n->lladdr, lladdr, 6); neigh_set_state(n, NUD_STALE, now); }
    } else if (!override && differs) {
        if (n->state == NUD_REACHABLE) neigh_set_state(n, NUD_STALE, now);
        return;
    } else {
        if (lladdr) memcpy(n->lladdr, lladdr, 6);
        if (solicited) { n->confirmed_ns = now; neigh_set_state(n, NUD_REACHABLE, now); }
        else if (differs) neigh_set_state(n, NUD_STALE, now);
    }
    if (is_router >= 0) {
        bool was = (n->flags & NTF_ROUTER) != 0;
        if (is_router) n->flags |= NTF_ROUTER; else n->flags &= (uint8_t)~NTF_ROUTER;
        if (was && !is_router) {   /* no longer a router: its default routes go (RFC 4861 7.2.5) */
            struct route m = {.family = AF_INET6, .plen = 0, .oif = dev->index, .protocol = RTPROT_RA}; m.gateway = *key;
            while (!route_del(&m, true)) {}
        }
    }
}
int neigh_update_admin(struct netdev* dev, uint8_t family, const struct ip6a* key, const uint8_t* lladdr, uint16_t state, uint8_t flags,
                       bool create, bool replace, bool notify) {
    struct neighbour* n = neigh_lookup(dev, family, key);
    if (n && !replace && create) return -17;
    if (!n) { if (!create) return -2; n = neigh_create(dev, family, key); if (!n) return -105; }
    if (lladdr) { memcpy(n->lladdr, lladdr, 6); n->has_lladdr = true; }
    n->flags = flags;
    uint64_t now = native_clock_ns();
    if (!state) state = NUD_PERMANENT;
    n->state = state; n->updated_ns = now; n->timer_ns = state == NUD_REACHABLE ? now + (uint64_t)dev->reachable_time_ms * 1000000ULL : 0;
    if (n->has_lladdr && (state & (NUD_REACHABLE | NUD_STALE | NUD_PERMANENT | NUD_NOARP | NUD_DELAY | NUD_PROBE))) flush_queue(n);
    if (notify) rtnl_notify_neigh(n, 28);
    return 0;
}
int neigh_delete(struct netdev* dev, uint8_t family, const struct ip6a* key, bool notify) {
    for (struct neighbour** link = &neighbours; *link; link = &(*link)->next) {
        struct neighbour* n = *link;
        if (n->dev == dev && n->family == family && ip6a_eq(&n->key, key)) { if (notify) rtnl_notify_neigh(n, 29); neigh_free(link); return 0; }
    }
    return -2;
}
void neigh_flush_dev(struct netdev* dev) {
    for (struct neighbour** link = &neighbours; *link;) {
        if ((*link)->dev == dev) { rtnl_notify_neigh(*link, 29); neigh_free(link); } else link = &(*link)->next;
    }
}

/* ---- ARP on TAP-style Ethernet devices ---- */
void ndisc_arp_receive(struct netdev* dev, const uint8_t* frame, unsigned length) {
    if (length < 42 || (dev->flags & IFF_NOARP)) return;
    const uint8_t* a = frame + 14;
    if (be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    uint16_t op = be16(a + 6); uint32_t sender = be32(a + 14), target = be32(a + 24);
    if (a[8] & 1) return;
    uint32_t mine = 0;
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (dev->v4[i].used && dev->v4[i].address == target) mine = target;
    struct ip6a key = ip6a_from4(sender);
    if (sender) neigh_learn(dev, AF_INET, &key, a + 8, op == 2, true, op == 2 && mine, -1);
    if (op == 1 && mine && sender != mine) {
        uint8_t reply[42];
        memcpy(reply, frame + 6, 6); memcpy(reply + 6, dev->addr, 6); put16(reply + 12, ETH_TYPE_ARP);
        uint8_t* r = reply + 14; put16(r, 1); put16(r + 2, 0x0800); r[4] = 6; r[5] = 4; put16(r + 6, 2);
        memcpy(r + 8, dev->addr, 6); put32(r + 14, mine); memcpy(r + 18, a + 8, 6); put32(r + 24, sender);
        netdev_xmit(dev, reply, sizeof(reply), ETH_TYPE_ARP);
    }
}

/* ---- addresses ---- */
struct inet6_ifaddr* addrconf_find(struct netdev* dev, const struct ip6a* addr) {
    for (struct netdev* d = dev ? dev : netdevs; d; d = dev ? NULL : d->next)
        for (struct inet6_ifaddr* a = d->addrs6; a; a = a->next) if (ip6a_eq(&a->addr, addr)) return a;
    return NULL;
}
bool addrconf_local(const struct ip6a* addr, unsigned scope, bool allow_tentative) {
    if (ip6a_loopback(addr)) return true;
    for (struct netdev* d = netdevs; d; d = d->next) {
        if (scope && ip6a_linklocal(addr) && d->index != scope) continue;
        for (struct inet6_ifaddr* a = d->addrs6; a; a = a->next)
            if (ip6a_eq(&a->addr, addr) && (allow_tentative || !(a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED)))) return true;
    }
    return false;
}
static void address_routes(struct netdev* dev, struct inet6_ifaddr* a, bool add) {
    if (dev->type == ARPHRD_LOOPBACK) {
        struct route local = {.family = AF_INET6, .plen = 128, .protocol = RTPROT_KERNEL, .type = RTN_LOCAL, .table = RT_TABLE_LOCAL, .oif = dev->index};
        local.dst = a->addr;
        if (add) route_add(&local, true, false, true); else route_del(&local, true);
        return;
    }
    struct route local = {.family = AF_INET6, .plen = 128, .protocol = RTPROT_KERNEL, .type = RTN_LOCAL, .table = RT_TABLE_LOCAL,
                          .oif = dev->index, .scope = RT_SCOPE_HOST};
    local.dst = a->addr;
    if (add) route_add(&local, true, false, true); else route_del(&local, true);
    if (a->plen < 128 && !(a->flags & IFA_F_NOPREFIXROUTE) && a->origin != ORIGIN_RA && a->origin != ORIGIN_TEMP) {
        struct route net = {.family = AF_INET6, .plen = a->plen, .protocol = RTPROT_KERNEL, .type = RTN_UNICAST, .table = RT_TABLE_MAIN,
                            .oif = dev->index, .metric = a->route_metric ? a->route_metric : 256};
        net.dst = a->addr; ip6a_mask(&net.dst, a->plen);
        if (add) route_add(&net, true, false, true);
        else {
            bool shared = false;   /* another address of the same prefix keeps the route */
            for (struct inet6_ifaddr* o = dev->addrs6; o; o = o->next)
                if (o != a && o->plen == a->plen && ip6a_prefix_eq(&o->addr, &a->addr, a->plen)) shared = true;
            if (!shared) route_del(&net, true);
        }
    }
}
static void addr_notify(struct netdev* dev, struct inet6_ifaddr* a, uint16_t type) { rtnl_notify_addr6(dev, a, type); }
static unsigned scope_of_addr(const struct ip6a* a) {
    if (ip6a_loopback(a)) return RT_SCOPE_HOST;
    if (ip6a_linklocal(a)) return RT_SCOPE_LINK;
    if (a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0xC0) return 200;
    return RT_SCOPE_UNIVERSE;
}
int addrconf_add(struct netdev* dev, const struct ip6a* addr, unsigned plen, uint32_t flags, uint32_t valid, uint32_t preferred,
                 uint8_t origin, const struct ip6a* peer, uint32_t metric, bool replace, struct inet6_ifaddr** out) {
    if (plen > 128 || ip6a_multicast(addr) || ip6a_any(addr) || ip6a_is4(addr)) return -22;
    if (dev->type != ARPHRD_LOOPBACK && ip6a_loopback(addr)) return -22;
    if (!netdev_ipv6_enabled(dev) && dev->type != ARPHRD_LOOPBACK) return -13;   /* EACCES: IPv6 disabled (Linux) */
    if (preferred > valid) return -22;
    uint64_t now = native_clock_ns();
    struct inet6_ifaddr* a = addrconf_find(dev, addr);
    if (a) {
        if (!replace) return -17;
        a->valid_lft = valid; a->prefered_lft = preferred; a->stamp_ns = now;
        a->flags = (a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED | IFA_F_TEMPORARY)) | (flags & ~(IFA_F_TENTATIVE | IFA_F_DADFAILED));
        if (preferred) a->flags &= ~IFA_F_DEPRECATED; else a->flags |= IFA_F_DEPRECATED;
        if (metric && metric != a->route_metric) { address_routes(dev, a, false); a->route_metric = metric; address_routes(dev, a, true); }
        addr_notify(dev, a, 20);
        if (out) *out = a;
        return 0;
    }
    unsigned count = 0; for (struct inet6_ifaddr* o = dev->addrs6; o; o = o->next) count++;
    if ((origin == ORIGIN_RA || origin == ORIGIN_TEMP) && count >= (unsigned)dev->conf6.max_addresses) return -28;
    if (count >= 64) return -28;
    a = net_alloc(sizeof(*a)); if (!a) return -12;
    memset(a, 0, sizeof(*a));
    a->addr = *addr; a->plen = (uint8_t)plen; a->origin = origin; a->valid_lft = valid; a->prefered_lft = preferred;
    a->created_ns = a->stamp_ns = now; a->scope = (uint8_t)scope_of_addr(addr); a->route_metric = metric;
    if (peer) a->peer = *peer;
    a->flags = flags & ~(IFA_F_TENTATIVE | IFA_F_DADFAILED | IFA_F_DEPRECATED);
    if (!preferred) a->flags |= IFA_F_DEPRECATED;
    if (valid == LIFETIME_INFINITE && preferred == LIFETIME_INFINITE) a->flags |= IFA_F_PERMANENT;
    else a->flags &= ~IFA_F_PERMANENT;
    if (origin == ORIGIN_MANUAL && valid == LIFETIME_INFINITE) a->flags |= IFA_F_PERMANENT;
    bool dad = dev->type == ARPHRD_ETHER && !(flags & IFA_F_NODAD) && dev->conf6.accept_dad > 0 && dev->conf6.dad_transmits > 0 &&
               !(dev->flags & IFF_NOARP);
    if (dad) a->flags |= IFA_F_TENTATIVE;
    strcpy(a->label, dev->name);
    struct inet6_ifaddr** tail = &dev->addrs6; while (*tail) tail = &(*tail)->next; *tail = a;
    if (dev->type != ARPHRD_LOOPBACK && (dev->flags & IFF_MULTICAST)) { struct ip6a group; ip6a_solicited(addr, &group); mc6_join(dev, &group, true); }
    if (dev->flags & IFF_UP) address_routes(dev, a, true);
    addr_notify(dev, a, 20);
    if (dad && netdev_running(dev)) addrconf_dad_start(dev, a);
    if (out) *out = a;
    return 0;
}
static void temporaries_of(struct netdev* dev, struct inet6_ifaddr* parent);
static void addr_remove(struct netdev* dev, struct inet6_ifaddr* a, bool notify) {
    for (struct inet6_ifaddr** link = &dev->addrs6; *link; link = &(*link)->next) if (*link == a) { *link = a->next; break; }
    for (struct netdev* d = netdevs; d; d = d->next)
        for (struct inet6_ifaddr* t = d->addrs6; t; t = t->next) if (t->public_parent == a) t->public_parent = NULL;
    address_routes(dev, a, false);
    if (dev->type != ARPHRD_LOOPBACK && (dev->flags & IFF_MULTICAST)) { struct ip6a group; ip6a_solicited(&a->addr, &group); mc6_leave(dev, &group, true); }
    if (notify) addr_notify(dev, a, 21);
    net_free(a, sizeof(*a));
}
int addrconf_del(struct netdev* dev, const struct ip6a* addr, unsigned plen, bool notify) {
    struct inet6_ifaddr* a = addrconf_find(dev, addr);
    if (!a || (plen <= 128 && a->plen != plen)) return -99;
    temporaries_of(dev, a);
    struct ip6a gone = a->addr;
    addr_remove(dev, a, notify);
    conntrack_flush_masquerade(dev->index, &gone);   /* masquerade bound to it */
    return 0;
}
void addrconf_flush_dev(struct netdev* dev, bool all) {
    for (struct inet6_ifaddr** link = &dev->addrs6; *link;) {
        struct inet6_ifaddr* a = *link;
        bool keep = !all && dev->conf6.keep_addr_on_down > 0 && a->origin == ORIGIN_MANUAL && (a->flags & IFA_F_PERMANENT) && !ip6a_linklocal(&a->addr);
        if (keep || dev->type == ARPHRD_LOOPBACK) { link = &a->next; continue; }
        addr_remove(dev, a, true);
    }
}
/* Delete the temporary addresses created for `parent` (its prefix went away). */
static void temporaries_of(struct netdev* dev, struct inet6_ifaddr* parent) {
    for (struct inet6_ifaddr** link = &dev->addrs6; *link;) {
        struct inet6_ifaddr* t = *link;
        if (t->public_parent == parent && (t->flags & IFA_F_TEMPORARY)) { addr_remove(dev, t, true); continue; }
        link = &t->next;
    }
}

/* ---- interface identifiers ---- */
static void ensure_secret(void) {
    if (boot_secret_ready) return;
    for (unsigned i = 0; i < 16; i += 4) { uint32_t r = net_random32(); memcpy(boot_secret + i, &r, 4); }
    boot_secret_ready = true;
}
/* RFC 7217: IID = F(prefix, interface, DAD counter, secret) with keyed BLAKE2s as the PRF. Reserved IIDs (RFC 5453)
 * are skipped by moving to the next counter value. */
static void stable_iid(struct netdev* dev, const struct ip6a* prefix, unsigned counter, struct ip6a* out) {
    ensure_secret();
    const uint8_t* secret = dev->conf6.stable_secret_set ? dev->conf6.stable_secret.b : boot_secret;
    for (unsigned attempt = 0; attempt < 8; attempt++, counter++) {
        uint8_t data[8 + IFNAMSIZ + 6 + 4] = {0}, digest[32];
        memcpy(data, prefix->b, 8); memcpy(data + 8, dev->name, strlen(dev->name)); memcpy(data + 8 + IFNAMSIZ, dev->perm_addr, 6);
        put32(data + 8 + IFNAMSIZ + 6, counter);
        blake2s(digest, 32, secret, 16, data, sizeof(data));
        *out = *prefix; memcpy(out->b + 8, digest, 8);
        bool zero = true, anycast = out->b[8] == 0xFD && out->b[9] == 0xFF && out->b[10] == 0xFF && out->b[11] == 0xFF && out->b[12] == 0xFF &&
                    out->b[13] == 0xFF && out->b[14] == 0xFF && out->b[15] >= 0x80;
        for (unsigned i = 8; i < 16; i++) if (out->b[i]) zero = false;
        if (!zero && !anycast) return;
    }
}
static bool eui64_iid(struct netdev* dev, const struct ip6a* prefix, struct ip6a* out) {
    if (dev->type != ARPHRD_ETHER) return false;
    *out = *prefix;
    out->b[8] = dev->addr[0] ^ 2; out->b[9] = dev->addr[1]; out->b[10] = dev->addr[2]; out->b[11] = 0xFF; out->b[12] = 0xFE;
    out->b[13] = dev->addr[3]; out->b[14] = dev->addr[4]; out->b[15] = dev->addr[5];
    return true;
}
static int gen_mode(struct netdev* dev) {
    int mode = dev->conf6.addr_gen_mode;
    if (dev->type == ARPHRD_NONE && mode == 0) mode = 3;   /* no hardware address: random (Linux) */
    return mode;
}
static bool make_address(struct netdev* dev, const struct ip6a* prefix, unsigned counter, struct ip6a* out, uint32_t* flags) {
    int mode = gen_mode(dev);
    *flags = 0;
    if (mode == 1) return false;
    if (mode == 2 || mode == 3) { stable_iid(dev, prefix, counter, out); *flags = IFA_F_STABLE_PRIVACY; return true; }
    return eui64_iid(dev, prefix, out);
}
void addrconf_ll_generate(struct netdev* dev) {
    if (!netdev_ipv6_enabled(dev) || dev->type == ARPHRD_LOOPBACK) return;
    if (!strcmp(dev->kind, "wireguard")) return;   /* WireGuard interfaces have no link-local address (Linux) */
    for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next) if (ip6a_linklocal(&a->addr)) return;
    struct ip6a prefix = {{0xFE, 0x80}}, address; uint32_t flags;
    if (!make_address(dev, &prefix, 0, &address, &flags)) return;
    struct inet6_ifaddr* ifa = NULL;
    addrconf_add(dev, &address, 64, flags | IFA_F_PERMANENT, LIFETIME_INFINITE, LIFETIME_INFINITE, ORIGIN_KERNEL_LL, NULL, 0, false, &ifa);
}

/* ---- DAD ---- */
void addrconf_dad_start(struct netdev* dev, struct inet6_ifaddr* a) {
    if (!(a->flags & IFA_F_TENTATIVE)) return;
    a->dad_probes = (uint8_t)(dev->conf6.dad_transmits > 0 ? dev->conf6.dad_transmits : 1);
    /* RFC 4862 5.4.2: a random delay before the first probe */
    a->dad_next_ns = native_clock_ns() + ndisc_random_ns((uint64_t)dev->conf6.router_solicitation_delay * 1000000000ULL / 4) + 1;
}
static void dad_succeeded(struct netdev* dev, struct inet6_ifaddr* a) {
    a->flags &= ~IFA_F_TENTATIVE; a->dad_next_ns = 0;
    addr_notify(dev, a, 20);
    if (ip6a_linklocal(&a->addr)) {
        /* link-local usable: solicit routers, re-report memberships with a real source */
        if (dev->conf6.accept_ra > 0 && !dev->conf6.forwarding) { dev->rs_sent = 0; dev->rs_next_ns = native_clock_ns() + ndisc_random_ns(1000000000ULL); }
        mld_dev_up(dev);
    }
    if (dev->conf6.ndisc_notify > 0) {   /* unsolicited NA (ndisc_notify) */
        uint8_t body[4 + 16 + 8] = {0}; body[0] = 0x20; memcpy(body + 4, a->addr.b, 16);
        body[20] = 2; body[21] = 1; memcpy(body + 22, dev->addr, 6);
        icmp6_send(&ip6a_all_nodes, &a->addr, dev, 136, 0, body, sizeof(body), 255);
    }
}
static void create_temporary(struct netdev* dev, struct inet6_ifaddr* parent, uint64_t now);
static void dad_failed(struct netdev* dev, struct inet6_ifaddr* a) {
    net_log_addr("[64] IPv6: duplicate address ", &a->addr, " detected\n");
    a->dad_next_ns = 0;
    if (a->flags & IFA_F_STABLE_PRIVACY && a->dad_count < TEMP_IDGEN_RETRIES && (a->origin == ORIGIN_RA || a->origin == ORIGIN_KERNEL_LL)) {
        /* RFC 7217 5.1: try the next DAD counter */
        struct ip6a prefix = a->addr; ip6a_mask(&prefix, 64); struct ip6a next; unsigned counter = a->dad_count + 1u;
        stable_iid(dev, &prefix, counter, &next);
        uint8_t origin = a->origin; uint32_t valid = a->valid_lft, preferred = a->prefered_lft, flags = a->flags & ~(IFA_F_TENTATIVE | IFA_F_DADFAILED);
        unsigned plen = a->plen;
        addr_remove(dev, a, true);
        struct inet6_ifaddr* fresh = NULL;
        if (!addrconf_add(dev, &next, plen, flags, valid, preferred, origin, NULL, 0, false, &fresh) && fresh) fresh->dad_count = (uint8_t)counter;
        return;
    }
    if (a->flags & IFA_F_TEMPORARY) {
        struct inet6_ifaddr* parent = a->public_parent; uint8_t tries = a->regen_count;
        addr_remove(dev, a, true);
        if (parent && tries < TEMP_IDGEN_RETRIES) { dev->temp_attempts = tries + 1u; create_temporary(dev, parent, native_clock_ns()); }
        else net_log("[64] IPv6: giving up on temporary addresses after DAD failures\n");
        return;
    }
    a->flags |= IFA_F_DADFAILED;
    if (ip6a_linklocal(&a->addr)) dev->dad_failed_ll = true;
    addr_notify(dev, a, 20);
}
static void dad_timers(struct netdev* dev, uint64_t now) {
    for (struct inet6_ifaddr* a = dev->addrs6; a;) {
        struct inet6_ifaddr* next = a->next;
        if ((a->flags & IFA_F_TENTATIVE) && !(a->flags & IFA_F_DADFAILED) && a->dad_next_ns && now >= a->dad_next_ns) {
            if (a->dad_probes) {
                if (dev->conf6.enhanced_dad > 0) for (unsigned i = 0; i < 6; i += 3) { uint32_t r = net_random32(); memcpy(a->dad_nonce + i, &r, 3); }
                struct ip6a group; ip6a_solicited(&a->addr, &group);
                send_ns(dev, &a->addr, &group, &ip6a_unspecified, false, dev->conf6.enhanced_dad > 0 ? a->dad_nonce : NULL);
                a->dad_probes--;
                a->dad_next_ns = now + (uint64_t)dev->retrans_ms * 1000000ULL;
            } else dad_succeeded(dev, a);
        }
        a = next;
    }
}

/* ---- SLAAC and temporary addresses ---- */
static uint32_t remaining(const struct inet6_ifaddr* a, uint32_t lifetime, uint64_t now) {
    if (lifetime == LIFETIME_INFINITE) return LIFETIME_INFINITE;
    uint64_t elapsed = (now - a->stamp_ns) / 1000000000ULL;
    return elapsed >= lifetime ? 0 : lifetime - (uint32_t)elapsed;
}
static uint32_t desync_factor(struct netdev* dev) {
    uint32_t max = (uint32_t)(dev->conf6.max_desync_factor > 0 ? dev->conf6.max_desync_factor : 0);
    uint32_t limit = dev->conf6.temp_prefered_lft > 0 ? (uint32_t)dev->conf6.temp_prefered_lft : 0;
    uint32_t regen = 2 + TEMP_IDGEN_RETRIES * (uint32_t)dev->conf6.dad_transmits * dev->retrans_ms / 1000;
    if (limit <= regen) return 0;
    if (max > limit - regen) max = limit - regen;
    return max ? net_random32() % (max + 1) : 0;
}
static void create_temporary(struct netdev* dev, struct inet6_ifaddr* parent, uint64_t now) {
    if (dev->conf6.use_tempaddr <= 0 || !parent) return;
    uint32_t parent_valid = remaining(parent, parent->valid_lft, now), parent_pref = remaining(parent, parent->prefered_lft, now);
    if (!parent_valid || !parent_pref) return;
    uint32_t desync = desync_factor(dev);
    uint32_t valid = (uint32_t)dev->conf6.temp_valid_lft, preferred = (uint32_t)dev->conf6.temp_prefered_lft;
    preferred = preferred > desync ? preferred - desync : 0;
    if (parent_valid < valid) valid = parent_valid;
    if (parent_pref < preferred) preferred = parent_pref;
    uint32_t regen = 2 + TEMP_IDGEN_RETRIES * (uint32_t)dev->conf6.dad_transmits * dev->retrans_ms / 1000;
    if (preferred <= regen) return;   /* RFC 8981 3.4: too short to be useful */
    struct ip6a address = parent->addr;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        uint32_t r1 = net_random32(), r2 = net_random32();
        memcpy(address.b + 8, &r1, 4); memcpy(address.b + 12, &r2, 4);
        address.b[8] &= (uint8_t)~2;   /* RFC 8981: never the "universal" bit pattern of an EUI-64 */
        bool reserved = address.b[8] == 0xFD && address.b[9] == 0xFF && address.b[10] == 0xFF && address.b[11] == 0xFF;
        if (!reserved && !addrconf_find(NULL, &address)) break;
    }
    struct inet6_ifaddr* t = NULL;
    if (addrconf_add(dev, &address, parent->plen, IFA_F_TEMPORARY, valid, preferred, ORIGIN_TEMP, NULL, 0, false, &t) || !t) return;
    t->public_parent = parent; t->regen_count = (uint8_t)dev->temp_attempts; dev->temp_attempts = 0;
    parent->regen_at_ns = now + (uint64_t)(preferred - regen) * 1000000000ULL;
}
static bool has_live_temporary(struct netdev* dev, struct inet6_ifaddr* parent, uint64_t now) {
    for (struct inet6_ifaddr* t = dev->addrs6; t; t = t->next)
        if (t->public_parent == parent && (t->flags & IFA_F_TEMPORARY) && !(t->flags & (IFA_F_DEPRECATED | IFA_F_DADFAILED)) &&
            remaining(t, t->prefered_lft, now) > 0) return true;
    return false;
}
static void slaac(struct netdev* dev, const struct ip6a* prefix, unsigned plen, uint32_t valid, uint32_t preferred, uint64_t now) {
    if (plen != 64 || !dev->conf6.autoconf) return;   /* the interface identifier is 64 bits on Ethernet (RFC 2464) */
    struct inet6_ifaddr* existing = NULL;
    for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next)
        if (a->origin == ORIGIN_RA && a->plen == plen && ip6a_prefix_eq(&a->addr, prefix, plen)) { existing = a; break; }
    if (!existing) {
        if (!valid) return;
        struct ip6a address; uint32_t flags;
        if (!make_address(dev, prefix, 0, &address, &flags)) return;
        if (dev->conf6.use_tempaddr > 0) flags |= IFA_F_MANAGETEMPADDR;
        struct inet6_ifaddr* a = NULL;
        /* (no prefix route of its own: the PIO's on-link flag manages it, see address_routes) */
        if (addrconf_add(dev, &address, plen, flags, valid, preferred, ORIGIN_RA, NULL, 0, false, &a) || !a) return;
        if (dev->conf6.use_tempaddr > 0 && !(a->flags & IFA_F_TENTATIVE)) create_temporary(dev, a, now);
        return;
    }
    /* RFC 4862 5.5.3(e): the two-hour rule protects the valid lifetime against spoofed small values */
    uint32_t left = remaining(existing, existing->valid_lft, now), new_valid;
    if (valid > 7200 || valid > left) new_valid = valid;
    else if (left <= 7200) new_valid = left;
    else new_valid = 7200;
    existing->valid_lft = new_valid; existing->prefered_lft = preferred > new_valid ? new_valid : preferred; existing->stamp_ns = now;
    if (existing->prefered_lft) existing->flags &= ~IFA_F_DEPRECATED; else existing->flags |= IFA_F_DEPRECATED;
    addr_notify(dev, existing, 20);
    /* temporaries: never outlive the prefix (RFC 8981 3.4) */
    for (struct inet6_ifaddr* t = dev->addrs6; t; t = t->next) {
        if (t->public_parent != existing) continue;
        uint32_t tv = remaining(t, t->valid_lft, now), tp = remaining(t, t->prefered_lft, now);
        if (tv > new_valid || tp > existing->prefered_lft) {
            t->valid_lft = tv > new_valid ? new_valid : tv; t->prefered_lft = tp > existing->prefered_lft ? existing->prefered_lft : tp;
            if (t->prefered_lft > t->valid_lft) t->prefered_lft = t->valid_lft;
            t->stamp_ns = now; if (!t->prefered_lft) t->flags |= IFA_F_DEPRECATED;
            addr_notify(dev, t, 20);
        }
    }
    if (dev->conf6.use_tempaddr > 0 && existing->prefered_lft && !(existing->flags & IFA_F_TENTATIVE) && !has_live_temporary(dev, existing, now))
        create_temporary(dev, existing, now);
}
static void lifetimes(struct netdev* dev, uint64_t now) {
    for (struct inet6_ifaddr* a = dev->addrs6; a;) {
        struct inet6_ifaddr* next = a->next;
        if (a->valid_lft != LIFETIME_INFINITE && !remaining(a, a->valid_lft, now)) {
            temporaries_of(dev, a); addr_remove(dev, a, true); a = dev->addrs6; continue;   /* restart: the list changed */
        }
        if (a->prefered_lft != LIFETIME_INFINITE && !(a->flags & IFA_F_DEPRECATED) && !remaining(a, a->prefered_lft, now)) {
            a->flags |= IFA_F_DEPRECATED; addr_notify(dev, a, 20);
        }
        /* a public SLAAC address whose temporary nears deprecation gets a fresh one */
        if ((a->flags & IFA_F_MANAGETEMPADDR) && dev->conf6.use_tempaddr > 0 && a->regen_at_ns && now >= a->regen_at_ns &&
            !(a->flags & (IFA_F_DEPRECATED | IFA_F_TENTATIVE))) {
            a->regen_at_ns = 0; create_temporary(dev, a, now);
        }
        if ((a->flags & IFA_F_MANAGETEMPADDR) && dev->conf6.use_tempaddr > 0 && !(a->flags & (IFA_F_DEPRECATED | IFA_F_TENTATIVE)) &&
            !a->regen_at_ns && !has_live_temporary(dev, a, now)) create_temporary(dev, a, now);
        a = next;
    }
}

/* ---- router discovery ---- */
static void send_rs(struct netdev* dev) {
    struct ip6a src; uint8_t body[4 + 8] = {0}; unsigned length = 4;
    bool have = own_source(dev, NULL, &src);
    if (have && dev->type == ARPHRD_ETHER) { body[4] = 1; body[5] = 1; memcpy(body + 6, dev->addr, 6); length = 12; }
    icmp6_send(&ip6a_all_routers, have ? &src : &ip6a_unspecified, dev, 133, 0, body, length, 255);
}
static void rs_timer(struct netdev* dev, uint64_t now) {
    if (!dev->rs_next_ns || now < dev->rs_next_ns) return;
    int limit = dev->conf6.router_solicitations;
    if (limit >= 0 && dev->rs_sent >= (unsigned)limit) { dev->rs_next_ns = 0; return; }
    send_rs(dev); dev->rs_sent++;
    /* RFC 7559 backoff up to router_solicitation_max_interval */
    uint64_t interval = (uint64_t)dev->conf6.router_solicitation_interval * 1000000000ULL;
    if (dev->rs_sent > 3) {
        uint64_t max = (uint64_t)dev->conf6.router_solicitation_max_interval * 1000000000ULL; interval <<= (dev->rs_sent - 3 > 10 ? 10 : dev->rs_sent - 3);
        if (interval > max) interval = max;
    }
    dev->rs_next_ns = now + interval;
}
static void dns_label_text(const uint8_t* p, unsigned length, unsigned* at, char* out) {
    unsigned n = 0; out[0] = 0;
    while (*at < length) {
        unsigned label = p[(*at)++];
        if (!label) break;
        if (label > 63 || *at + label > length || n + label + 2 > 63) { out[0] = 0; *at = length; return; }
        if (n) out[n++] = '.';
        for (unsigned i = 0; i < label; i++) { char c = (char)p[*at + i]; out[n++] = (c > ' ' && c < 127) ? c : '_'; }
        *at += label;
    }
    out[n] = 0;
}
static void store_rdnss(struct netdev* dev, const struct ip6a* address, uint32_t lifetime, uint64_t now) {
    int slot = -1;
    for (int i = 0; i < DNS_SERVERS; i++) if (rdnss[i].used && rdnss[i].ifindex == dev->index && ip6a_eq(&rdnss[i].address, address)) slot = i;
    if (slot < 0) for (int i = 0; i < DNS_SERVERS; i++) if (!rdnss[i].used || rdnss[i].expires_ns <= now) { slot = i; break; }
    if (slot < 0) return;
    if (!lifetime) { rdnss[slot].used = false; return; }
    rdnss[slot].used = true; rdnss[slot].address = *address; rdnss[slot].ifindex = dev->index;
    rdnss[slot].expires_ns = lifetime == LIFETIME_INFINITE ? ~0ULL : now + (uint64_t)lifetime * 1000000000ULL;
}
static void store_dnssl(struct netdev* dev, const char* name, uint32_t lifetime, uint64_t now) {
    if (!name[0]) return;
    int slot = -1;
    for (int i = 0; i < DNS_SERVERS; i++) if (dnssl[i].used && dnssl[i].ifindex == dev->index && !strcmp(dnssl[i].name, name)) slot = i;
    if (slot < 0) for (int i = 0; i < DNS_SERVERS; i++) if (!dnssl[i].used || dnssl[i].expires_ns <= now) { slot = i; break; }
    if (slot < 0) return;
    if (!lifetime) { dnssl[slot].used = false; return; }
    dnssl[slot].used = true; strcpy(dnssl[slot].name, name); dnssl[slot].ifindex = dev->index;
    dnssl[slot].expires_ns = lifetime == LIFETIME_INFINITE ? ~0ULL : now + (uint64_t)lifetime * 1000000000ULL;
}
unsigned dns_servers6(struct ip6a* out, unsigned capacity, char domains[][64], unsigned* domain_count) {
    uint64_t now = native_clock_ns(); unsigned n = 0, d = 0;
    for (unsigned i = 0; i < DNS_SERVERS && n < capacity; i++) if (rdnss[i].used && rdnss[i].expires_ns > now) out[n++] = rdnss[i].address;
    if (domains && domain_count) {
        for (unsigned i = 0; i < DNS_SERVERS && d < *domain_count; i++) if (dnssl[i].used && dnssl[i].expires_ns > now) strcpy(domains[d++], dnssl[i].name);
        *domain_count = d;
    }
    return n;
}
/* Validate the option area: every option has a non-zero length and fits (RFC 4861 4.6). */
static bool options_valid(const uint8_t* p, unsigned length) {
    unsigned at = 0;
    while (at < length) {
        if (length - at < 2 || !p[at + 1] || (unsigned)p[at + 1] * 8 > length - at) return false;
        at += (unsigned)p[at + 1] * 8;
    }
    return true;
}
static const uint8_t* option_find(const uint8_t* p, unsigned length, uint8_t type) {
    for (unsigned at = 0; at + 2 <= length && p[at + 1]; at += (unsigned)p[at + 1] * 8)
        if (p[at] == type && (unsigned)p[at + 1] * 8 <= length - at) return p + at;
    return NULL;
}
static void receive_ra(struct netdev* dev, const struct ip6a* src, const uint8_t* m, unsigned length) {
    if (length < 16 || !ip6a_linklocal(src)) return;
    const uint8_t* opts = m + 16; unsigned opts_length = length - 16;
    if (!options_valid(opts, opts_length)) return;
    if (dev->conf6.accept_ra <= 0 || (dev->conf6.forwarding > 0 && dev->conf6.accept_ra < 2)) return;
    if (addrconf_local(src, dev->index, true) && !dev->conf6.accept_ra_from_local) return;
    uint64_t now = native_clock_ns();
    dev->rs_next_ns = 0;   /* a router answered: stop soliciting */
    uint8_t hop = m[4], flags = m[5]; uint16_t lifetime = be16(m + 6); uint32_t reachable = be32(m + 8), retrans = be32(m + 12);
    if (hop && hop >= dev->conf6.accept_ra_min_hop_limit) { dev->ra_hop_limit = hop; dev->conf6.hop_limit = hop; }
    bool managed = (flags & 0x80) != 0, other = (flags & 0x40) != 0;
    uint8_t pref = (flags >> 3) & 3; if (pref == 2) pref = 0;   /* reserved -> medium */
    if (reachable && reachable <= 3600000U) { dev->base_reachable_ms = reachable; dev->reachable_time_ms = reachable_random(reachable); }
    if (retrans) dev->retrans_ms = retrans < 100 ? 100 : retrans > 60000 ? 60000 : retrans;
    const uint8_t* slla = option_find(opts, opts_length, 1);
    if (slla && slla[1] == 1) neigh_learn(dev, AF_INET6, src, slla + 2, false, true, false, 1);
    else { struct neighbour* n = neigh_lookup(dev, AF_INET6, src); if (n) n->flags |= NTF_ROUTER; }
    if (dev->conf6.accept_ra_defrtr > 0) {
        struct route def = {.family = AF_INET6, .plen = 0, .protocol = RTPROT_RA, .type = RTN_UNICAST, .table = RT_TABLE_MAIN, .oif = dev->index,
                            .metric = 1024, .pref = pref, .hoplimit = hop};
        def.gateway = *src;
        if (!lifetime) route_del(&def, true);
        else { def.expires_ns = now + (uint64_t)lifetime * 1000000000ULL; route_add(&def, true, false, true); }
    }
    bool rdnss_seen = false;
    for (unsigned at = 0; at < opts_length; at += (unsigned)opts[at + 1] * 8) {
        const uint8_t* o = opts + at; unsigned size = (unsigned)o[1] * 8;
        if (o[0] == 5 && size == 8 && dev->conf6.accept_ra_mtu > 0) {
            uint32_t mtu = be32(o + 4);
            if (mtu >= 1280 && mtu <= dev->mtu) { dev->ra_mtu = mtu; dev->mtu6 = mtu; dev->conf6.mtu = (int32_t)mtu; }
        } else if (o[0] == 3 && size == 32 && dev->conf6.accept_ra_pinfo > 0) {
            unsigned plen = o[2]; uint8_t pflags = o[3]; uint32_t valid = be32(o + 4), preferred = be32(o + 8);
            struct ip6a prefix; memcpy(prefix.b, o + 16, 16);
            if (plen > 128 || ip6a_linklocal(&prefix) || ip6a_multicast(&prefix) || preferred > valid) continue;
            ip6a_mask(&prefix, plen);
            if (pflags & 0x80) {   /* on-link */
                struct route r = {.family = AF_INET6, .plen = (uint8_t)plen, .protocol = RTPROT_KERNEL, .type = RTN_UNICAST, .table = RT_TABLE_MAIN,
                                  .oif = dev->index, .metric = 256};
                r.dst = prefix;
                if (!valid) route_del(&r, true);
                else { r.expires_ns = valid == LIFETIME_INFINITE ? 0 : now + (uint64_t)valid * 1000000000ULL; route_add(&r, true, false, true); }
            }
            if (pflags & 0x40) slaac(dev, &prefix, plen, valid, preferred, now);
            rtnl_notify_prefix(dev, &prefix, plen, pflags, valid, preferred);
        } else if (o[0] == 25 && size >= 24) {
            uint32_t life = be32(o + 4); unsigned count = (size - 8) / 16;
            for (unsigned i = 0; i < count; i++) { struct ip6a a; memcpy(a.b, o + 8 + 16 * i, 16); if (!ip6a_multicast(&a)) store_rdnss(dev, &a, life, now); }
            rdnss_seen = true;
            rtnl_notify_useropt(dev, o, size, src);
        } else if (o[0] == 31 && size >= 16) {
            uint32_t life = be32(o + 4); unsigned p = 8;
            while (p < size && o[p]) { char name[64]; dns_label_text(o, size, &p, name); store_dnssl(dev, name, life, now); }
            rtnl_notify_useropt(dev, o, size, src);
        }
    }
    dev->ra_managed = managed; dev->ra_other = other;
    if (managed || other || !rdnss_seen) dhcp6_start(dev, managed);
}
static void receive_ns(struct netdev* dev, const struct ip6a* src, const struct ip6a* dst, const uint8_t* m, unsigned length) {
    if (length < 24) return;
    struct ip6a target; memcpy(target.b, m + 8, 16);
    if (ip6a_multicast(&target)) return;
    const uint8_t* opts = m + 24; unsigned opts_length = length - 24;
    if (!options_valid(opts, opts_length)) return;
    const uint8_t* slla = option_find(opts, opts_length, 1);
    bool dad = ip6a_any(src);
    if (dad) {
        struct ip6a solicited; ip6a_solicited(&target, &solicited);
        if (!ip6a_eq(dst, &solicited) || slla) return;   /* RFC 4861 7.1.1 */
    }
    struct inet6_ifaddr* a = addrconf_find(dev, &target);
    if (!a) return;
    if (a->flags & IFA_F_TENTATIVE) {
        if (!dad) return;   /* a resolution request for a tentative address is ignored */
        const uint8_t* nonce = option_find(opts, opts_length, 14);
        if (nonce && nonce[1] == 1 && !memcmp(nonce + 2, a->dad_nonce, 6)) return;   /* our own probe looped back (RFC 7527) */
        dad_failed(dev, a); return;
    }
    if (a->flags & IFA_F_DADFAILED) return;
    if (!dad && slla && slla[1] == 1) neigh_learn(dev, AF_INET6, src, slla + 2, false, true, false, -1);
    uint8_t body[4 + 16 + 8] = {0};
    body[0] = (uint8_t)((dad ? 0 : 0x40) | 0x20);   /* Solicited (unless DAD), Override */
    memcpy(body + 4, target.b, 16);
    unsigned body_length = 20;
    if (dev->type == ARPHRD_ETHER) { body[20] = 2; body[21] = 1; memcpy(body + 22, dev->addr, 6); body_length = 28; }
    icmp6_send(dad ? &ip6a_all_nodes : src, &target, dev, 136, 0, body, body_length, 255);
}
static void receive_na(struct netdev* dev, const struct ip6a* dst, const uint8_t* m, unsigned length) {
    if (length < 24) return;
    uint8_t flags = m[4]; bool router = (flags & 0x80) != 0, solicited = (flags & 0x40) != 0, override = (flags & 0x20) != 0;
    struct ip6a target; memcpy(target.b, m + 8, 16);
    if (ip6a_multicast(&target) || (solicited && ip6a_multicast(dst))) return;
    const uint8_t* opts = m + 24; unsigned opts_length = length - 24;
    if (!options_valid(opts, opts_length)) return;
    struct inet6_ifaddr* a = addrconf_find(dev, &target);
    if (a) {
        if (a->flags & IFA_F_TENTATIVE) dad_failed(dev, a);   /* someone else holds the address */
        else net_log_addr("[64] IPv6: neighbour advertisement for our address ", &target, " (duplicate?)\n");
        return;
    }
    const uint8_t* tlla = option_find(opts, opts_length, 2);
    neigh_learn(dev, AF_INET6, &target, tlla && tlla[1] == 1 ? tlla + 2 : NULL, solicited, override, true, router ? 1 : 0);
}
static void receive_redirect(struct netdev* dev, const struct ip6a* src, const uint8_t* m, unsigned length) {
    if (length < 40 || !ip6a_linklocal(src) || dev->conf6.accept_redirects <= 0 || dev->conf6.forwarding > 0) return;
    struct ip6a target, destination; memcpy(target.b, m + 8, 16); memcpy(destination.b, m + 24, 16);
    if (ip6a_multicast(&destination) || (!ip6a_linklocal(&target) && !ip6a_eq(&target, &destination))) return;
    const uint8_t* opts = m + 40; unsigned opts_length = length - 40;
    if (!options_valid(opts, opts_length)) return;
    struct route_result current;   /* only the current first hop may redirect (RFC 4861 8.1) */
    if (route_lookup(AF_INET6, &destination, NULL, 0, 0, &current) || current.dev != dev || !current.gateway || !ip6a_eq(&current.nexthop, src)) return;
    const uint8_t* tlla = option_find(opts, opts_length, 2);
    if (tlla && tlla[1] == 1) neigh_learn(dev, AF_INET6, &target, tlla + 2, false, true, false, ip6a_eq(&target, &destination) ? 0 : 1);
    route_redirect6(&destination, &target, dev);
}
void ndisc_receive(struct netdev* dev, const struct ip6a* src, const struct ip6a* dst, uint8_t hop_limit, const uint8_t* m, unsigned length, bool has_hbh) {
    (void)has_hbh;
    if (hop_limit != 255 || m[1] != 0) return;   /* RFC 4861: hop limit 255 and code 0 on every ND message */
    if (dev->type != ARPHRD_ETHER && dev->type != ARPHRD_NONE) return;
    switch (m[0]) {
    case 134: receive_ra(dev, src, m, length); break;
    case 135: receive_ns(dev, src, dst, m, length); break;
    case 136: receive_na(dev, dst, m, length); break;
    case 137: receive_redirect(dev, src, m, length); break;
    default: break;
    }
}

/* ---- interface state ---- */
void ndisc_dev_up(struct netdev* dev) {
    if (!netdev_ipv6_enabled(dev) || dev->type == ARPHRD_LOOPBACK) return;
    dev->reachable_time_ms = reachable_random(dev->base_reachable_ms);
    if (dev->flags & IFF_MULTICAST) { struct ip6a all = ip6a_all_nodes; mc6_join(dev, &all, true); }
    dev->ipv6_ready = true;
    /* Addresses kept over a down/up (manual ones) redo DAD; the link-local one is generated if absent. */
    for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next) {
        address_routes(dev, a, true);
        if (dev->type == ARPHRD_ETHER && !(a->flags & IFA_F_NODAD) && dev->conf6.accept_dad > 0 && dev->conf6.dad_transmits > 0) {
            a->flags |= IFA_F_TENTATIVE; a->flags &= ~IFA_F_DADFAILED; addrconf_dad_start(dev, a);
        }
    }
    struct route mc = {.family = AF_INET6, .plen = 8, .protocol = RTPROT_KERNEL, .type = RTN_MULTICAST, .table = RT_TABLE_LOCAL, .oif = dev->index, .metric = 256};
    mc.dst.b[0] = 0xFF; route_add(&mc, true, false, true);
    struct route ll = {.family = AF_INET6, .plen = 64, .protocol = RTPROT_KERNEL, .type = RTN_UNICAST, .table = RT_TABLE_MAIN, .oif = dev->index, .metric = 256};
    ll.dst.b[0] = 0xFE; ll.dst.b[1] = 0x80;
    if (strcmp(dev->kind, "wireguard")) route_add(&ll, true, false, true);
    addrconf_ll_generate(dev);
    mld_dev_up(dev); igmp_dev_up(dev);
    if (dev->type == ARPHRD_NONE && dev->conf6.accept_ra > 0 && !dev->conf6.forwarding) { dev->rs_sent = 0; dev->rs_next_ns = native_clock_ns() + ndisc_random_ns(1000000000ULL); }
}
void ndisc_dev_down(struct netdev* dev) {
    dev->ipv6_ready = false; dev->rs_next_ns = 0;
    if (dev->flags & IFF_UP) {
        /* carrier lost (Linux NETDEV_CHANGE): addresses, routes learnt from routers and DNS data stay (the routes
         * are unusable while the link is down); DAD reruns and a router solicitation goes out when it returns */
        for (struct inet6_ifaddr* a = dev->addrs6; a; a = a->next) a->dad_next_ns = 0;
    } else {
        dhcp6_dev_down(dev);
        for (unsigned i = 0; i < DNS_SERVERS; i++) { if (rdnss[i].ifindex == dev->index) rdnss[i].used = false; if (dnssl[i].ifindex == dev->index) dnssl[i].used = false; }
        addrconf_flush_dev(dev, false);
        route_flush_dev(dev, AF_INET6);
    }
    for (struct neighbour** link = &neighbours; *link;) {
        struct neighbour* n = *link;
        if (n->dev == dev && n->family == AF_INET6 && !(n->state & (NUD_PERMANENT | NUD_NOARP))) { rtnl_notify_neigh(n, 29); neigh_free(link); }
        else link = &n->next;
    }
    mld_dev_down(dev); igmp_dev_down(dev);
}
/* accept_ra switched off (netproc.c): the DNS data learnt from this interface's router advertisements (RDNSS,
 * DNSSL) and from the stateless DHCPv6 they started is forgotten, as a network manager does when an interface is
 * configured by hand; addresses and routes stay until the manager (or their lifetime) removes them, as in Linux */
void ndisc_forget_ra(struct netdev* dev) {
    dhcp6_dev_down(dev);
    for (unsigned i = 0; i < DNS_SERVERS; i++) { if (rdnss[i].ifindex == dev->index) rdnss[i].used = false; if (dnssl[i].ifindex == dev->index) dnssl[i].used = false; }
}
void ndisc_dev_carrier(struct netdev* dev, bool on) { (void)dev; (void)on; }
void ndisc_poll(uint64_t now) {
    static uint64_t next_second;
    neigh_timers(now);
    for (struct netdev* d = netdevs; d; d = d->next) {
        if (!netdev_running(d) || d->type == ARPHRD_LOOPBACK) continue;
        dad_timers(d, now); rs_timer(d, now);
    }
    if (now >= next_second) {
        next_second = now + 1000000000ULL;
        for (struct netdev* d = netdevs; d; d = d->next) lifetimes(d, now);
        for (unsigned i = 0; i < DNS_SERVERS; i++) { if (rdnss[i].used && rdnss[i].expires_ns <= now) rdnss[i].used = false; if (dnssl[i].used && dnssl[i].expires_ns <= now) dnssl[i].used = false; }
    }
}
void ndisc_init(void) { neighbours = NULL; neighbour_count = 0; }
