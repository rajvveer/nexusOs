/* Multicast group membership of the native kernel, both families: the socket API's memberships and source filters
 * (RFC 3678: any-source joins, source-specific joins, block/unblock, full-state msfilter, as Linux), the interface
 * state merged from every socket (RFC 3376 3.2 / RFC 3810 4.2: EXCLUDE if any member excludes, sources = the
 * intersection of the exclude lists minus every include list; else INCLUDE with the union), and the protocols that
 * report it to routers:
 *   IGMPv3 (RFC 3376) with IGMPv1/v2 compatibility (RFC 2236, RFC 3376 7: older-version querier present timers,
 *     v1/v2 reports to the group, v2 Leave to 224.0.0.2, report suppression on another member's v1/v2 report);
 *   MLDv2 (RFC 3810) with MLDv1 compatibility (RFC 2710: reports, Done to ff02::2, suppression).
 * State changes go out as TO_IN/TO_EX (mode change) or ALLOW/BLOCK records (source list change), robustness-variable
 * times; queries are answered after a random delay up to their maximum response time: general queries with the
 * current state of every group (IS_IN/IS_EX), group-specific and group-and-source-specific ones per group.
 * Delivery: a socket without a membership for the group on the arrival interface takes the datagram only with
 * IP_MULTICAST_ALL / IPV6_MULTICAST_ALL (on by default); with one, its filter decides (Linux ip_mc_sf_allow).
 * Everything runs under the kernel lock (netdev.h); IPv4 groups and sources are kept IPv4-mapped. */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/string.h"

#define STATE_MAX 128             /* sources kept in an interface state (more: reported as EXCLUDE{}, a superset) */
#define MSF_MAX4 10               /* net.ipv4.igmp_max_msf */
#define MSF_MAX6 64               /* net.ipv6.mld_max_msf */
#define ALL_HOSTS4 0xE0000001U
#define ALL_ROUTERS4 0xE0000002U
#define IGMPV3_ROUTERS 0xE0000016U
enum { IS_IN = 1, IS_EX = 2, TO_IN = 3, TO_EX = 4, ALLOW = 5, BLOCK = 6 };

static bool v4(const struct ip6a* a) { return ip6a_is4(a); }
static uint64_t now_ns(void) { return native_clock_ns(); }
static bool reportable(const struct ip6a* g) {
    if (v4(g)) return ip6a_v4(g) != ALL_HOSTS4;
    return !ip6a_eq(g, &ip6a_all_nodes) && ip6a_mc_scope(g) >= 2;
}
static bool in_set(const struct ip6a* set, unsigned n, const struct ip6a* a) {
    for (unsigned i = 0; i < n; i++) if (ip6a_eq(&set[i], a)) return true;
    return false;
}
static void state_free(struct mc_state* s) { if (s->sources) net_free(s->sources, s->count * (unsigned)sizeof(struct ip6a)); s->sources = NULL; s->count = 0; }
static bool state_set(struct mc_state* s, uint8_t mode, const struct ip6a* sources, unsigned n) {
    struct ip6a* copy = NULL;
    if (n) { copy = net_alloc(n * (unsigned)sizeof(struct ip6a)); if (!copy) return false; memcpy(copy, sources, n * sizeof(struct ip6a)); }
    state_free(s); s->mode = mode; s->count = (uint16_t)n; s->sources = copy; return true;
}

/* ---- per-interface state ---- */
static struct mc_group** list_of(struct netdev* dev, const struct ip6a* group) { return v4(group) ? &dev->mc4 : &dev->mc6; }
static struct mc_group* find(struct netdev* dev, const struct ip6a* group) {
    for (struct mc_group* g = *list_of(dev, group); g; g = g->next) if (ip6a_eq(&g->group, group)) return g;
    return NULL;
}
bool mc_joined(struct netdev* dev, const struct ip6a* group) { struct mc_group* g = dev ? find(dev, group) : NULL; return g && !g->leaving; }
bool mc4_member_any(uint32_t group) {
    if (group == ALL_HOSTS4) return true;
    struct ip6a g = ip6a_from4(group);
    for (struct netdev* d = netdevs; d; d = d->next) if (mc_joined(d, &g)) return true;
    return false;
}

/* The merged filter of every socket membership (and kernel join) for one group on one interface. */
static struct ip6a scratch_a[STATE_MAX], scratch_b[STATE_MAX];
struct merge { struct netdev* dev; const struct ip6a* group; unsigned excludes, includes, count; bool overflow, started; struct ip6a* set; struct ip6a* include_union; unsigned union_count; };
static void merge_exclude(struct mc_membership* m, void* context) {
    struct merge* x = context;
    if (m->ifindex != x->dev->index || !ip6a_eq(&m->group, x->group) || m->mode != MCAST_EXCLUDE) return;
    if (!x->started) { x->started = true; x->count = 0; for (unsigned i = 0; i < m->count && x->count < STATE_MAX; i++) x->set[x->count++] = m->sources[i]; }
    else { unsigned keep = 0; for (unsigned i = 0; i < x->count; i++) if (in_set(m->sources, m->count, &x->set[i])) x->set[keep++] = x->set[i]; x->count = keep; }
    x->excludes++;
}
static void merge_include(struct mc_membership* m, void* context) {
    struct merge* x = context;
    if (m->ifindex != x->dev->index || !ip6a_eq(&m->group, x->group) || m->mode != MCAST_INCLUDE) return;
    for (unsigned i = 0; i < m->count; i++) {
        if (in_set(x->include_union, x->union_count, &m->sources[i])) continue;
        if (x->union_count == STATE_MAX) { x->overflow = true; return; }
        x->include_union[x->union_count++] = m->sources[i];
    }
    x->includes++;
}
/* current interface state: *out_sources points at a static buffer valid until the next call */
static void current(struct netdev* dev, struct mc_group* g, uint8_t* mode, const struct ip6a** sources, unsigned* count) {
    *mode = MCAST_INCLUDE; *sources = scratch_a; *count = 0;
    if (!g || g->leaving) return;
    struct merge x = {dev, &g->group, 0, 0, 0, false, false, scratch_a, scratch_b, 0};
    inet_mc_visit(merge_exclude, &x);
    inet_mc_visit(merge_include, &x);
    if (g->kernel_users) { x.excludes++; x.count = 0; x.started = true; }    /* kernel joins: EXCLUDE{} */
    if (x.excludes) {
        unsigned keep = 0;
        for (unsigned i = 0; i < x.count; i++) if (!in_set(x.include_union, x.union_count, &x.set[i])) x.set[keep++] = x.set[i];
        *mode = MCAST_EXCLUDE; *count = keep; return;
    }
    if (x.overflow) { *mode = MCAST_EXCLUDE; *count = 0; return; }     /* too many sources to list: receive them all */
    memcpy(scratch_a, scratch_b, x.union_count * sizeof(struct ip6a)); *count = x.union_count;
}
static bool joined_state(uint8_t mode, unsigned count) { return mode == MCAST_EXCLUDE || count; }

/* ---- querier version, timers ---- */
static unsigned robustness(struct netdev* dev, bool four) { unsigned r = four ? dev->igmp_qrv : dev->mld_qrv; return r ? r : 2; }
static bool igmp_v1(struct netdev* dev) { return dev->igmp_v1_until_ns > now_ns(); }
static bool igmp_v2(struct netdev* dev) { return !igmp_v1(dev) && dev->igmp_v2_until_ns > now_ns(); }
static bool mld_v1(struct netdev* dev) {
    if (dev->conf6.force_mld_version == 1) return true;
    if (dev->conf6.force_mld_version == 2) return false;
    return dev->mld_v1_until_ns && dev->mld_v1_until_ns > now_ns();
}
static bool old_version(struct netdev* dev, bool four) { return four ? igmp_v1(dev) || igmp_v2(dev) : mld_v1(dev); }
static uint64_t unsolicited_interval(struct netdev* dev, bool four) {
    if (four) return old_version(dev, true) ? 10000000000ULL : 1000000000ULL;
    return (uint64_t)(mld_v1(dev) ? dev->conf6.mldv1_unsolicited_report_interval : dev->conf6.mldv2_unsolicited_report_interval) * 1000000ULL;
}
static bool usable(struct netdev* dev) { return netdev_running(dev) && dev->type != ARPHRD_LOOPBACK && dev != netdev_lo; }

/* ---- report messages ---- */
struct report { struct netdev* dev; bool four; uint8_t body[1400]; unsigned used, records, limit; };
static void report_flush(struct report* r) {
    if (!r->records) return;
    if (r->four) {
        uint8_t packet[24 + sizeof(r->body)];
        uint32_t src = netdev_v4_primary(r->dev); unsigned length = 24 + r->used;
        memset(packet, 0, 24);
        packet[0] = 0x46; packet[1] = 0xC0; put16(packet + 2, (uint16_t)length); packet[8] = 1; packet[9] = 2;   /* IHL 6, TTL 1, IGMP */
        put32(packet + 12, src); put32(packet + 16, IGMPV3_ROUTERS); packet[20] = 0x94; packet[21] = 4;      /* Router Alert */
        r->body[0] = 0x22; r->body[1] = 0; r->body[2] = r->body[3] = 0; r->body[4] = r->body[5] = 0; put16(r->body + 6, (uint16_t)r->records);
        uint16_t sum = ip_checksum(r->body, (uint16_t)r->used); memcpy(r->body + 2, &sum, 2);
        memcpy(packet + 24, r->body, r->used);
        struct ip6_send_options o; ip6_options_default(&o); o.raw_header = true; o.oif = r->dev->index; o.mc_loop = false;
        ip4_output(src, IGMPV3_ROUTERS, 2, packet, length, &o);
    } else {
        r->body[0] = r->body[1] = 0; put16(r->body + 2, (uint16_t)r->records);
        struct ip6a source; mld_report_source(r->dev, &source);
        icmp6_send(&ip6a_mld2_routers, &source, r->dev, 143, 0, r->body, r->used, 1);
    }
    r->used = r->four ? 8 : 4; r->records = 0;
}
static void report_begin(struct report* r, struct netdev* dev, bool four) {
    r->dev = dev; r->four = four; r->used = four ? 8 : 4; r->records = 0;
    r->limit = four ? (dev->mtu > 24 + 8 && dev->mtu - 24 < sizeof(r->body) ? dev->mtu - 24 : sizeof(r->body)) : 1280 - 48 - 4;
}
/* One record; a list that does not fit is split (IS_IN/ALLOW/BLOCK) or truncated (EXCLUDE kinds, RFC 3376 4.2.16). */
static void record(struct report* r, uint8_t type, const struct ip6a* group, const struct ip6a* sources, unsigned count) {
    unsigned per = r->four ? 4 : 16, header = r->four ? 8 : 20, done = 0;
    do {
        if (r->used + header + per > r->limit) report_flush(r);
        unsigned room = (r->limit - r->used - header) / per, n = count - done < room ? count - done : room;
        uint8_t* p = r->body + r->used; p[0] = type; p[1] = 0; put16(p + 2, (uint16_t)n);
        if (r->four) put32(p + 4, ip6a_v4(group)); else memcpy(p + 4, group->b, 16);
        for (unsigned i = 0; i < n; i++) { if (r->four) put32(p + header + 4 * i, ip6a_v4(&sources[done + i])); else memcpy(p + header + 16 * i, sources[done + i].b, 16); }
        r->used += header + per * n; r->records++; done += n;
        if (type == IS_EX || type == TO_EX || type == TO_IN) break;
    } while (done < count);
}
static void old_message(struct netdev* dev, const struct ip6a* group, bool leave) {
    if (v4(group)) {
        if (leave && igmp_v1(dev)) return;                               /* IGMPv1 has no leave */
        uint8_t packet[24 + 8]; uint32_t src = netdev_v4_primary(dev), g = ip6a_v4(group), dst = leave ? ALL_ROUTERS4 : g;
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x46; packet[1] = 0xC0; put16(packet + 2, sizeof(packet)); packet[8] = 1; packet[9] = 2;
        put32(packet + 12, src); put32(packet + 16, dst); packet[20] = 0x94; packet[21] = 4;
        uint8_t* m = packet + 24; m[0] = leave ? 0x17 : igmp_v1(dev) ? 0x12 : 0x16; put32(m + 4, g);
        uint16_t sum = ip_checksum(m, 8); memcpy(m + 2, &sum, 2);
        struct ip6_send_options o; ip6_options_default(&o); o.raw_header = true; o.oif = dev->index; o.mc_loop = false;
        ip4_output(src, dst, 2, packet, sizeof(packet), &o);
    } else {
        uint8_t body[20] = {0}; memcpy(body + 4, group->b, 16);
        struct ip6a source; mld_report_source(dev, &source);
        icmp6_send(leave ? &ip6a_all_routers : group, &source, dev, leave ? 132 : 131, 0, body, 20, 1);
    }
}
static void set_difference(const struct ip6a* a, unsigned na, const struct ip6a* b, unsigned nb, struct ip6a* out, unsigned* n) {
    *n = 0; for (unsigned i = 0; i < na; i++) if (!in_set(b, nb, &a[i])) out[(*n)++] = a[i];
}
static struct ip6a diff_a[STATE_MAX], diff_b[STATE_MAX];
/* state-change records of a burst: the state before it (g->before) -> the current state. A second change during a
 * burst's retransmissions (routers may have heard the intermediate state) sends the full state instead (TO_IN/TO_EX),
 * which is correct whatever routers last heard (RFC 3376 6.1 allows merged reports; this is the safe merge). */
static void change_records(struct report* r, struct mc_group* g, uint8_t mode, const struct ip6a* sources, unsigned count) {
    const struct mc_state* was = &g->before;
    if (g->full || was->mode != mode) { record(r, mode == MCAST_EXCLUDE ? TO_EX : TO_IN, &g->group, sources, count); return; }
    unsigned added, removed;
    set_difference(sources, count, was->sources, was->count, diff_a, &added);
    set_difference(was->sources, was->count, sources, count, diff_b, &removed);
    if (mode == MCAST_INCLUDE) { if (added) record(r, ALLOW, &g->group, diff_a, added); if (removed) record(r, BLOCK, &g->group, diff_b, removed); }
    else { if (removed) record(r, ALLOW, &g->group, diff_b, removed); if (added) record(r, BLOCK, &g->group, diff_a, added); }
}
static void current_record(struct report* r, struct mc_group* g, uint8_t mode, const struct ip6a* sources, unsigned count) {
    if (joined_state(mode, count)) record(r, mode == MCAST_EXCLUDE ? IS_EX : IS_IN, &g->group, sources, count);
}
/* answer to a group-and-source-specific query for the sources in g->asked (RFC 3376 5.2) */
static void asked_record(struct report* r, struct mc_group* g, uint8_t mode, const struct ip6a* sources, unsigned count) {
    unsigned n = 0;
    for (unsigned i = 0; i < g->asked.count; i++) {
        bool listed = in_set(sources, count, &g->asked.sources[i]);
        if (mode == MCAST_INCLUDE ? listed : !listed) diff_a[n++] = g->asked.sources[i];
    }
    if (n) record(r, IS_IN, &g->group, diff_a, n);
}

/* ---- state changes ---- */
static void schedule_change(struct netdev* dev, struct mc_group* g) {
    if (!usable(dev) || !reportable(&g->group)) { g->change_left = 0; g->change_ns = 0; return; }
    if (g->change_left && g->sent) g->full = true;                        /* a change during a burst already heard */
    else if (!g->change_left) { g->full = !state_set(&g->before, g->reported.mode, g->reported.sources, g->reported.count); g->sent = false; }
    g->change_left = (uint8_t)robustness(dev, v4(&g->group)); g->change_ns = now_ns();
}
static void group_free(struct netdev* dev, struct mc_group** link) {
    struct mc_group* g = *link; *link = g->next;
    state_free(&g->reported); state_free(&g->before); state_free(&g->asked); net_free(g, sizeof(*g));
    netdev_rx_filter(dev);
}
int mc_join_group(struct netdev* dev, const struct ip6a* group, bool kernel) {
    if (v4(group) ? (ip6a_v4(group) >> 28) != 14 : !ip6a_multicast(group)) return -22;
    struct mc_group* g = find(dev, group);
    if (g) {
        if (g->leaving) { g->leaving = false; g->users = 0; g->kernel_users = 0; }
        g->users++; if (kernel) g->kernel_users++;
        schedule_change(dev, g); return 0;
    }
    unsigned count = 0; for (struct mc_group* x = *list_of(dev, group); x; x = x->next) count++;
    if (count >= 256) return -105;
    g = net_alloc(sizeof(*g)); if (!g) return -12;
    memset(g, 0, sizeof(*g)); g->group = *group; g->users = 1; g->kernel_users = kernel ? 1 : 0;
    g->reported.mode = MCAST_INCLUDE;                                    /* not a member: INCLUDE{} */
    struct mc_group** list = list_of(dev, group); g->next = *list; *list = g;
    netdev_rx_filter(dev);
    schedule_change(dev, g);
    return 0;
}
int mc_leave_group(struct netdev* dev, const struct ip6a* group, bool kernel) {
    for (struct mc_group** link = list_of(dev, group); *link; link = &(*link)->next) {
        struct mc_group* g = *link;
        if (!ip6a_eq(&g->group, group) || g->leaving) continue;
        if (kernel && g->kernel_users) g->kernel_users--;
        if (--g->users) { schedule_change(dev, g); return 0; }
        g->leaving = true; g->answer_ns = 0;
        if (!usable(dev) || !reportable(group)) { group_free(dev, link); return 0; }
        schedule_change(dev, g);
        netdev_rx_filter(dev);
        return 0;
    }
    return -99;
}
void mc_filter_changed(struct netdev* dev, const struct ip6a* group) { struct mc_group* g = find(dev, group); if (g && !g->leaving) schedule_change(dev, g); }
/* legacy names used by ndisc.c, inet.c and others */
int mc4_join(struct netdev* dev, uint32_t group) { struct ip6a g = ip6a_from4(group); return mc_join_group(dev, &g, false); }
int mc4_leave(struct netdev* dev, uint32_t group) { struct ip6a g = ip6a_from4(group); return mc_leave_group(dev, &g, false); }
int mc6_join(struct netdev* dev, const struct ip6a* group, bool kernel) { return mc_join_group(dev, group, kernel); }
int mc6_leave(struct netdev* dev, const struct ip6a* group, bool kernel) { return mc_leave_group(dev, group, kernel); }

/* ---- the timers (network poll) ---- */
static void send_change(struct netdev* dev, struct mc_group* g, struct report* r) {
    uint8_t mode; const struct ip6a* sources; unsigned count; current(dev, g, &mode, &sources, &count);
    if (old_version(dev, v4(&g->group))) {                                /* v1/v2: report while a member, else leave */
        if (joined_state(mode, count)) old_message(dev, &g->group, false);
        else if (joined_state(g->before.mode, g->before.count) || joined_state(g->reported.mode, g->reported.count)) old_message(dev, &g->group, true);
    } else change_records(r, g, mode, sources, count);
    if (!state_set(&g->reported, mode, sources, count)) g->full = true;   /* what routers heard now */
}
static void poll_family(struct netdev* dev, bool four, uint64_t now) {
    struct mc_group** list = four ? &dev->mc4 : &dev->mc6;
    uint64_t* general = four ? &dev->igmp_general_ns : &dev->mld_general_ns;
    static struct report r; report_begin(&r, dev, four);
    if (*general && now >= *general) {
        *general = 0;
        for (struct mc_group* g = *list; g; g = g->next) {
            if (g->leaving || !reportable(&g->group)) continue;
            uint8_t mode; const struct ip6a* sources; unsigned count; current(dev, g, &mode, &sources, &count);
            if (old_version(dev, four)) { if (joined_state(mode, count)) old_message(dev, &g->group, false); }
            else current_record(&r, g, mode, sources, count);
        }
        report_flush(&r);
    }
    for (struct mc_group** link = list; *link;) {
        struct mc_group* g = *link;
        if (g->answer_ns && now >= g->answer_ns) {
            g->answer_ns = 0;
            uint8_t mode; const struct ip6a* sources; unsigned count; current(dev, g, &mode, &sources, &count);
            if (!g->leaving) {
                if (old_version(dev, four)) { if (joined_state(mode, count)) old_message(dev, &g->group, false); }
                else if (g->answer_all) current_record(&r, g, mode, sources, count);
                else asked_record(&r, g, mode, sources, count);
            }
            state_free(&g->asked); g->answer_all = false;
        }
        if (g->change_ns && now >= g->change_ns) {
            send_change(dev, g, &r); report_flush(&r); g->sent = true;
            if (g->change_left) g->change_left--;
            if (g->change_left) g->change_ns = now + ndisc_random_ns(unsolicited_interval(dev, four)) + 1;
            else { g->change_ns = 0; g->full = false; }
        }
        if (g->leaving && !g->change_left && !g->change_ns) { group_free(dev, link); continue; }
        link = &g->next;
    }
    report_flush(&r);
}
void mcast_poll(uint64_t now) {
    for (struct netdev* d = netdevs; d; d = d->next) {
        if (!usable(d)) continue;
        poll_family(d, true, now);
        poll_family(d, false, now);
    }
}
void mld_poll(struct netdev* dev, uint64_t now) { (void)dev; (void)now; }   /* (mcast_poll runs both families) */
/* The earliest pending report/answer/general-query deadline (tickless core); ~0 when none. Groups without a pending
 * timer cost one compare each; nothing else is scanned. */
uint64_t mc_next_event(uint64_t now) {
    uint64_t next = ~0ULL;
    for (struct netdev* d = netdevs; d; d = d->next) {
        if (d->igmp_general_ns && d->igmp_general_ns < next) next = d->igmp_general_ns;
        if (d->mld_general_ns && d->mld_general_ns < next) next = d->mld_general_ns;
        for (int six = 0; six < 2; six++)
            for (struct mc_group* g = six ? d->mc6 : d->mc4; g; g = g->next) {
                if (g->answer_ns && g->answer_ns < next) next = g->answer_ns;
                if (g->change_ns && g->change_ns < next) next = g->change_ns;
                if (g->leaving && !g->change_left && !g->change_ns && now < next) next = now;   /* to be freed */
            }
    }
    return next;
}
void igmp_poll(uint64_t now) { mcast_poll(now); }

/* Interface down: pending reports are dropped (leaving groups go); up: every membership is reported again. */
static void dev_down_family(struct netdev* dev, struct mc_group** list) {
    for (struct mc_group** link = list; *link;) {
        struct mc_group* g = *link;
        if (g->leaving) { group_free(dev, link); continue; }
        g->change_left = 0; g->change_ns = 0; g->answer_ns = 0; g->full = false; g->sent = false; state_free(&g->asked);
        state_free(&g->reported); g->reported.mode = MCAST_INCLUDE; state_free(&g->before); g->before.mode = MCAST_INCLUDE;
        link = &g->next;
    }
}
void mld_dev_down(struct netdev* dev) { dev_down_family(dev, &dev->mc6); dev->mld_general_ns = 0; }
void igmp_dev_down(struct netdev* dev) { dev_down_family(dev, &dev->mc4); dev->igmp_general_ns = 0; }
static void dev_up_family(struct netdev* dev, struct mc_group* list) {   /* routers are told everything again */
    uint64_t now = now_ns();
    for (struct mc_group* g = list; g; g = g->next) {
        if (g->leaving || !reportable(&g->group)) continue;
        state_free(&g->reported); g->reported.mode = MCAST_INCLUDE; state_free(&g->before); g->before.mode = MCAST_INCLUDE; g->full = false; g->sent = false;
        g->change_left = (uint8_t)robustness(dev, v4(&g->group)); g->change_ns = now + ndisc_random_ns(1000000000ULL) + 1;
    }
}
void mld_dev_up(struct netdev* dev) { dev_up_family(dev, dev->mc6); }
void igmp_dev_up(struct netdev* dev) { dev_up_family(dev, dev->mc4); }
void mc_free_all(struct netdev* dev) {
    while (dev->mc6) group_free(dev, &dev->mc6);
    while (dev->mc4) group_free(dev, &dev->mc4);
}

/* ---- queries and other members' reports ---- */
static void schedule_answer(struct mc_group* g, uint64_t when, const struct ip6a* sources, unsigned count, bool whole) {
    if (g->leaving) return;
    if (whole || g->answer_all) { state_free(&g->asked); g->answer_all = true; }
    else {   /* union of the asked sources of pending group-and-source-specific queries */
        unsigned n = 0; static struct ip6a merged[STATE_MAX];
        for (unsigned i = 0; i < g->asked.count && n < STATE_MAX; i++) merged[n++] = g->asked.sources[i];
        for (unsigned i = 0; i < count && n < STATE_MAX; i++) if (!in_set(merged, n, &sources[i])) merged[n++] = sources[i];
        if (!state_set(&g->asked, MCAST_INCLUDE, merged, n)) { state_free(&g->asked); g->answer_all = true; }
    }
    if (!g->answer_ns || when < g->answer_ns) g->answer_ns = when;
}
void igmp_receive(struct netdev* dev, const uint8_t* m, unsigned length, uint32_t dst) {
    if (!dev || !usable(dev) || length < 8) return;
    uint64_t now = now_ns();
    if (m[0] == 0x12 || m[0] == 0x16) {                                  /* another member's v1/v2 report: suppress ours */
        struct ip6a group = ip6a_from4(be32(m + 4)); struct mc_group* g = find(dev, &group);
        if (g && old_version(dev, true)) { g->answer_ns = 0; state_free(&g->asked); g->answer_all = false; }
        return;
    }
    if (m[0] != 0x11) return;
    uint32_t group = be32(m + 4); uint64_t max_ns; static struct ip6a asked[STATE_MAX]; unsigned sources = 0;
    if (length == 8) {
        unsigned qrv = dev->igmp_qrv ? dev->igmp_qrv : 2, qi = dev->igmp_qi_s ? dev->igmp_qi_s : 125;
        uint64_t present = (uint64_t)qrv * qi * 1000000000ULL + 10000000000ULL;   /* older version querier present timeout */
        if (!m[1]) { dev->igmp_v1_until_ns = now + present; max_ns = 10000000000ULL; }   /* IGMPv1 query: 10 s */
        else { dev->igmp_v2_until_ns = now + present; max_ns = (uint64_t)m[1] * 100000000ULL; }
    } else if (length >= 12) {
        unsigned code = m[1]; max_ns = (code < 128 ? code : (uint64_t)((code & 15) | 16) << (((code >> 4) & 7) + 3)) * 100000000ULL;
        unsigned qrv = m[8] & 7, qqic = m[9]; sources = be16(m + 10);
        if (12 + 4u * sources > length) return;                           /* truncated source list: ignored */
        if (qrv) dev->igmp_qrv = qrv;
        dev->igmp_qi_s = qqic < 128 ? qqic : (uint32_t)((qqic & 15) | 16) << (((qqic >> 4) & 7) + 3);
        if (sources > STATE_MAX) sources = STATE_MAX;
        for (unsigned i = 0; i < sources; i++) asked[i] = ip6a_from4(be32(m + 12 + 4 * i));
    } else return;                                                        /* 9..11 bytes: not a valid query */
    if (!max_ns) max_ns = 100000000ULL;
    if (!group) {                                                         /* general query (to 224.0.0.1) */
        if (dst != ALL_HOSTS4) return;
        uint64_t when = now + ndisc_random_ns(max_ns);
        if (old_version(dev, true)) {                                     /* v1/v2: one timer per group */
            for (struct mc_group* g = dev->mc4; g; g = g->next) if (reportable(&g->group)) schedule_answer(g, now + ndisc_random_ns(max_ns), NULL, 0, true);
        } else if (!dev->igmp_general_ns || when < dev->igmp_general_ns) dev->igmp_general_ns = when;
        return;
    }
    if ((group >> 28) != 14) return;
    struct ip6a g6 = ip6a_from4(group); struct mc_group* g = find(dev, &g6);
    if (!g || !reportable(&g6)) return;
    schedule_answer(g, now + ndisc_random_ns(max_ns), asked, sources, !sources || old_version(dev, true));
}
static uint64_t mld_max_ns(uint16_t code) {
    if (code < 32768) return (uint64_t)code * 1000000ULL;   /* RFC 3810 5.1.3: exponential above 32768 */
    unsigned mant = code & 0xFFF, exp = (code >> 12) & 7;
    return (uint64_t)((mant | 0x1000) << (exp + 3)) * 1000000ULL;
}
void mld_query(struct netdev* dev, const struct ip6a* src, const struct ip6a* dst, const uint8_t* m, unsigned length) {
    if (!ip6a_linklocal(src) || length < 24 || !usable(dev)) return;
    uint64_t now = now_ns(), max_ns;
    const struct ip6a* group = (const void*)(m + 8);
    static struct ip6a asked[STATE_MAX]; unsigned sources = 0;
    if (length == 24) {   /* MLDv1 query */
        max_ns = (uint64_t)be16(m + 4) * 1000000ULL;
        dev->mld_v1_until_ns = now + (uint64_t)dev->mld_qrv * dev->mld_qi_s * 1000000000ULL + dev->mld_qri_ms * 1000000ULL;
    } else if (length >= 28) {
        max_ns = mld_max_ns(be16(m + 4));
        uint8_t qrv = m[24] & 7, qqic = m[25];
        sources = be16(m + 26);
        if (28 + 16u * sources > length) return;
        if (qrv) dev->mld_qrv = qrv;
        dev->mld_qi_s = qqic < 128 ? qqic : (uint32_t)((qqic & 15) | 16) << (((qqic >> 4) & 7) + 3);
        dev->mld_qri_ms = (uint32_t)(max_ns / 1000000ULL);
        if (sources > STATE_MAX) sources = STATE_MAX;
        memcpy(asked, m + 28, 16 * sources);
    } else return;
    if (!ip6a_any(group) && !ip6a_multicast(group)) return;
    if (ip6a_any(group)) {
        if (!ip6a_eq(dst, &ip6a_all_nodes)) return;   /* a general query goes to ff02::1 */
        if (mld_v1(dev)) { for (struct mc_group* g = dev->mc6; g; g = g->next) if (reportable(&g->group)) schedule_answer(g, now + ndisc_random_ns(max_ns), NULL, 0, true); return; }
        uint64_t when = now + ndisc_random_ns(max_ns);
        if (!dev->mld_general_ns || when < dev->mld_general_ns) dev->mld_general_ns = when;
        return;
    }
    struct mc_group* g = find(dev, group);
    if (!g || !reportable(group)) return;
    schedule_answer(g, now + ndisc_random_ns(max_ns), asked, sources, !sources || mld_v1(dev));
}
void mld_heard_report(struct netdev* dev, const uint8_t* m, unsigned length) {   /* MLDv1 report of another listener */
    if (length < 24 || !mld_v1(dev)) return;
    struct mc_group* g = find(dev, (const void*)(m + 8));
    if (g) { g->answer_ns = 0; state_free(&g->asked); g->answer_all = false; }
}

/* ---- sockets: memberships and filters (Linux ip_mc_join_group / ip_mc_source / ip_mc_msfilter semantics) ---- */
static struct mc_membership* membership_find(struct mc_membership* list, const struct ip6a* group, unsigned ifindex) {
    for (struct mc_membership* m = list; m; m = m->next) if (m->ifindex == ifindex && ip6a_eq(&m->group, group)) return m;
    return NULL;
}
static void membership_free(struct mc_membership* m) {
    if (m->sources) net_free(m->sources, m->capacity * (unsigned)sizeof(struct ip6a));
    net_free(m, sizeof(*m));
}
static int membership_reserve(struct mc_membership* m, unsigned n) {
    if (n <= m->capacity) return 0;
    unsigned capacity = m->capacity ? m->capacity : 4; while (capacity < n) capacity *= 2;
    struct ip6a* bigger = net_alloc(capacity * (unsigned)sizeof(struct ip6a)); if (!bigger) return -105;
    if (m->count) memcpy(bigger, m->sources, m->count * sizeof(struct ip6a));
    if (m->sources) net_free(m->sources, m->capacity * (unsigned)sizeof(struct ip6a));
    m->sources = bigger; m->capacity = (uint16_t)capacity; return 0;
}
static int membership_create(struct mc_membership** list, unsigned* count, struct netdev* dev, const struct ip6a* group, uint8_t mode,
                             struct mc_membership** out) {
    if (*count >= 20) return -105;                                        /* IP_MAX_MEMBERSHIPS */
    struct mc_membership* m = net_alloc(sizeof(*m)); if (!m) return -12;
    memset(m, 0, sizeof(*m)); m->group = *group; m->ifindex = dev->index; m->mode = mode;
    int error = mc_join_group(dev, group, false);
    if (error) { net_free(m, sizeof(*m)); return error; }
    m->next = *list; *list = m; (*count)++; *out = m;
    return 0;
}
static void membership_remove(struct mc_membership** list, unsigned* count, struct mc_membership* victim) {
    for (struct mc_membership** link = list; *link; link = &(*link)->next)
        if (*link == victim) {
            *link = victim->next; (*count)--;
            struct netdev* dev = netdev_by_index(victim->ifindex); struct ip6a group = victim->group;
            membership_free(victim);
            if (dev) mc_leave_group(dev, &group, false);
            return;
        }
}
int mc_sock_change(struct mc_membership** list, unsigned* count, int op, struct netdev* dev, const struct ip6a* group, const struct ip6a* source) {
    if (v4(group) ? (ip6a_v4(group) >> 28) != 14 : !ip6a_multicast(group)) return -22;
    if (source && (v4(source) != v4(group) || (v4(source) ? (ip6a_v4(source) >> 28) >= 14 || !ip6a_v4(source) : ip6a_multicast(source) || ip6a_any(source)))) return -22;
    unsigned limit = v4(group) ? MSF_MAX4 : MSF_MAX6;
    struct mc_membership* m = membership_find(*list, group, dev->index);
    switch (op) {
    case MC_JOIN:
        if (m) return -98;                                                /* EADDRINUSE */
        return membership_create(list, count, dev, group, MCAST_EXCLUDE, &m);
    case MC_LEAVE:
        if (!m) return -99;                                               /* EADDRNOTAVAIL */
        membership_remove(list, count, m); return 0;
    case MC_JOIN_SOURCE: case MC_BLOCK: {
        uint8_t mode = op == MC_JOIN_SOURCE ? MCAST_INCLUDE : MCAST_EXCLUDE;
        if (!m) {
            if (op == MC_BLOCK) return -22;                               /* a block needs a prior any-source join */
            int error = membership_create(list, count, dev, group, MCAST_INCLUDE, &m); if (error) return error;
        } else if (m->mode != mode) {
            if (m->count) return -22;                                     /* a filter of the other mode exists */
            m->mode = mode;                                               /* empty filters may switch mode */
        }
        if (in_set(m->sources, m->count, source)) { mc_filter_changed(dev, group); return 0; }
        if (m->count >= limit) return -105;                               /* ENOBUFS: igmp_max_msf / mld_max_msf */
        int error = membership_reserve(m, m->count + 1u); if (error) return error;
        m->sources[m->count++] = *source;
        mc_filter_changed(dev, group); return 0;
    }
    case MC_LEAVE_SOURCE: case MC_UNBLOCK: {
        uint8_t mode = op == MC_LEAVE_SOURCE ? MCAST_INCLUDE : MCAST_EXCLUDE;
        if (!m) return -99;
        if (m->mode != mode) return -22;
        unsigned i = 0; while (i < m->count && !ip6a_eq(&m->sources[i], source)) i++;
        if (i == m->count) return -99;
        if (mode == MCAST_INCLUDE && m->count == 1) { membership_remove(list, count, m); return 0; }   /* INCLUDE{} == leave */
        m->sources[i] = m->sources[--m->count];
        mc_filter_changed(dev, group); return 0;
    }
    }
    return -22;
}
int mc_sock_set_filter(struct mc_membership** list, unsigned* count, struct netdev* dev, const struct ip6a* group, uint8_t mode,
                       const struct ip6a* sources, unsigned n) {
    if (v4(group) ? (ip6a_v4(group) >> 28) != 14 : !ip6a_multicast(group)) return -22;
    if (mode != MCAST_INCLUDE && mode != MCAST_EXCLUDE) return -22;
    if (n > (v4(group) ? MSF_MAX4 : MSF_MAX6)) return -105;
    for (unsigned i = 0; i < n; i++) if (v4(&sources[i]) != v4(group)) return -22;
    struct mc_membership* m = membership_find(*list, group, dev->index);
    if (!m) return -22;                                                   /* Linux: a prior join is required */
    if (mode == MCAST_INCLUDE && !n) { membership_remove(list, count, m); return 0; }
    int error = membership_reserve(m, n); if (error) return error;
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) if (!in_set(sources, i, &sources[i])) m->sources[k++] = sources[i];   /* duplicates once */
    m->count = (uint16_t)k; m->mode = mode;
    mc_filter_changed(dev, group); return 0;
}
int mc_sock_get_filter(struct mc_membership* list, unsigned ifindex, const struct ip6a* group, uint8_t* mode, struct ip6a* out,
                       unsigned capacity, unsigned* total) {
    struct mc_membership* m = membership_find(list, group, ifindex);
    if (!m) return -99;
    *mode = m->mode; *total = m->count;
    for (unsigned i = 0; i < m->count && i < capacity; i++) out[i] = m->sources[i];
    return 0;
}
void mc_sock_drop_all(struct mc_membership** list, unsigned* count) {
    while (*list) membership_remove(list, count, *list);
}
void mc_sock_dev_gone(struct mc_membership** list, unsigned* count, unsigned ifindex) {
    for (struct mc_membership** link = list; *link;) {
        struct mc_membership* m = *link;
        if (m->ifindex == ifindex) { *link = m->next; (*count)--; membership_free(m); } else link = &m->next;
    }
}
bool mc_sock_allows(struct mc_membership* list, bool all, const struct ip6a* group, unsigned ifindex, const struct ip6a* source) {
    struct mc_membership* m = membership_find(list, group, ifindex);
    if (!m) return all;
    bool listed = in_set(m->sources, m->count, source);
    return m->mode == MCAST_INCLUDE ? listed : !listed;
}

/* ---- /proc/net/mcfilter and /proc/net/mcfilter6 (Linux format: interface, group, source, INC/EXC counts) ---- */
static unsigned hex_out(char* out, unsigned at, unsigned capacity, const uint8_t* bytes, unsigned n) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < n && at + 2 < capacity; i++) { out[at++] = digits[bytes[i] >> 4]; out[at++] = digits[bytes[i] & 15]; }
    return at;
}
static unsigned text_out(char* out, unsigned at, unsigned capacity, const char* text) { while (*text && at + 1 < capacity) out[at++] = *text++; return at; }
static unsigned number_out(char* out, unsigned at, unsigned capacity, unsigned value, unsigned width) {
    char digits[12]; unsigned n = 0; do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (width > n && at + 1 < capacity) { out[at++] = ' '; width--; }
    while (n && at + 1 < capacity) out[at++] = digits[--n];
    return at;
}
unsigned mc_proc_filters(bool six, char* out, unsigned capacity) {
    unsigned at = 0;
    if (!six) at = text_out(out, at, capacity, "Idx Device        MCA        SRC    INC    EXC\n");
    for (struct netdev* d = netdevs; d; d = d->next)
        for (struct mc_group* g = six ? d->mc6 : d->mc4; g; g = g->next) {
            if (g->leaving) continue;
            uint8_t mode; const struct ip6a* sources; unsigned count; current(d, g, &mode, &sources, &count);
            for (unsigned i = 0; i < count; i++) {
                at = number_out(out, at, capacity, d->index, six ? 0 : 3); at = text_out(out, at, capacity, " ");
                for (unsigned pad = (unsigned)strlen(d->name); !six && pad < 6; pad++) at = text_out(out, at, capacity, " ");
                at = text_out(out, at, capacity, d->name); at = text_out(out, at, capacity, " ");
                if (six) at = hex_out(out, at, capacity, g->group.b, 16); else { at = text_out(out, at, capacity, "0x"); at = hex_out(out, at, capacity, g->group.b + 12, 4); }
                at = text_out(out, at, capacity, " ");
                if (six) at = hex_out(out, at, capacity, sources[i].b, 16); else { at = text_out(out, at, capacity, "0x"); at = hex_out(out, at, capacity, sources[i].b + 12, 4); }
                at = number_out(out, at, capacity, mode == MCAST_INCLUDE ? 1 : 0, 7); at = number_out(out, at, capacity, mode == MCAST_EXCLUDE ? 1 : 0, 7);
                at = text_out(out, at, capacity, "\n");
            }
        }
    if (capacity) out[at < capacity ? at : capacity - 1] = 0;
    return at;
}
const char* mc_igmp_version(struct netdev* dev) { return igmp_v1(dev) ? "V1" : igmp_v2(dev) ? "V2" : "V3"; }
