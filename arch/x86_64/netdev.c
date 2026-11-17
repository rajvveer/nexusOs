/* Network interfaces of the native kernel: the registry (lo, eth0 on the NIC, tun/tap and WireGuard devices),
 * administrative and carrier state, IPv4 addresses, the link-layer receive filter (unicast/broadcast/joined
 * multicast, promiscuous and all-multicast counts), statistics, and the dispatch of received frames to ARP/IPv4
 * (kernel/net.c), IPv6 and packet sockets. netdev_poll() runs from the network poll and drives the timers of the
 * protocol modules. See netdev.h for the locking rule (big kernel lock everywhere). */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/arp.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

struct netdev* netdevs;
struct netdev* netdev_lo;
struct netdev* netdev_eth0;
struct netdev* netdev_rx_current;
static unsigned next_index = 1;
static bool mirror_valid;   /* netdev_sync_legacy: eth0's configuration has been mirrored */
const struct ip6a ip6a_all_nodes = {{0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
const struct ip6a ip6a_all_routers = {{0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}};
const struct ip6a ip6a_mld2_routers = {{0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16}};
const struct ip6a ip6a_unspecified = {{0}};
const struct ip6a ip6a_loopback_address = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};

/* ---- small utilities ---- */
uint64_t net_now(void) { return native_clock_ns(); }
uint32_t net_random32(void) {
    uint32_t value;
    if (!native_random(&value, sizeof(value))) value = (uint32_t)(native_tsc() * 0x9E3779B97F4A7C15ULL >> 32);
    return value;
}
void net_log(const char* text) { vga_print(text); }
void net_log_addr(const char* prefix, const struct ip6a* address, const char* suffix) {
    char text[64]; ip6a_text(address, text); vga_print(prefix); vga_print(text); vga_print(suffix);
}
uint32_t net_sum(uint32_t sum, const void* data, unsigned length) {
    const uint8_t* p = data;
    while (length >= 2) { sum += (uint32_t)p[0] << 8 | p[1]; p += 2; length -= 2; if (sum >> 31) sum = (sum & 0xFFFF) + (sum >> 16); }
    if (length) sum += (uint32_t)p[0] << 8;
    return sum;
}
uint16_t net_fold(uint32_t sum) { while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16); return (uint16_t)~sum; }
uint16_t net_checksum_add(uint32_t sum, const void* data, unsigned length, uint32_t* out) {
    sum = net_sum(sum, data, length); if (out) *out = sum; return net_fold(sum);
}
bool ip6a_prefix_eq(const struct ip6a* a, const struct ip6a* b, unsigned length) {
    if (length > 128) length = 128;
    unsigned bytes = length / 8, bits = length % 8;
    if (memcmp(a->b, b->b, bytes)) return false;
    if (!bits) return true;
    uint8_t mask = (uint8_t)(0xFF << (8 - bits));
    return (a->b[bytes] & mask) == (b->b[bytes] & mask);
}
void ip6a_mask(struct ip6a* a, unsigned length) {
    if (length >= 128) return;
    unsigned bytes = length / 8, bits = length % 8;
    if (bits) { a->b[bytes] &= (uint8_t)(0xFF << (8 - bits)); bytes++; }
    for (unsigned i = bytes; i < 16; i++) a->b[i] = 0;
}
void ip6a_solicited(const struct ip6a* target, struct ip6a* group) {
    static const uint8_t prefix[13] = {0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF};
    memcpy(group->b, prefix, 13); memcpy(group->b + 13, target->b + 13, 3);
}
static char* put_hex16(char* out, unsigned value) {
    static const char digits[] = "0123456789abcdef"; bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        unsigned digit = (value >> shift) & 15;
        if (digit || started || !shift) { *out++ = digits[digit]; started = true; }
    }
    return out;
}
static char* put_dec(char* out, unsigned value) {
    char digits[12]; unsigned n = 0; do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
    return out;
}
unsigned ip6a_text(const struct ip6a* a, char* out) {
    char* p = out;
    if (ip6a_is4(a)) {
        memcpy(p, "::ffff:", 7); p += 7;
        for (unsigned i = 12; i < 16; i++) { p = put_dec(p, a->b[i]); if (i < 15) *p++ = '.'; }
        *p = 0; return (unsigned)(p - out);
    }
    unsigned words[8]; for (unsigned i = 0; i < 8; i++) words[i] = (unsigned)a->b[2 * i] << 8 | a->b[2 * i + 1];
    int best = -1, best_length = 0;   /* RFC 5952: the longest run of >= 2 zero words, first on ties */
    for (int i = 0; i < 8;) {
        if (words[i]) { i++; continue; }
        int j = i; while (j < 8 && !words[j]) j++;
        if (j - i > best_length && j - i >= 2) { best = i; best_length = j - i; }
        i = j;
    }
    for (int i = 0; i < 8; i++) {
        if (i == best) { *p++ = ':'; *p++ = ':'; i += best_length - 1; continue; }
        if (i && i != best + best_length) *p++ = ':';
        p = put_hex16(p, words[i]);
    }
    *p = 0; return (unsigned)(p - out);
}
static int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool ip6a_parse(const char* text, struct ip6a* out) {
    uint16_t words[8]; int count = 0, gap = -1; const char* p = text;
    memset(out, 0, sizeof(*out));
    if (p[0] == ':' && p[1] == ':') { gap = 0; p += 2; if (!*p) return true; }
    else if (*p == ':') return false;
    while (*p) {
        /* An embedded IPv4 tail (::ffff:1.2.3.4) */
        const char* q = p; while (*q && *q != ':' && *q != '.') q++;
        if (*q == '.') {
            if (count > 6) return false;
            unsigned octets[4], n = 0;
            while (n < 4) {
                unsigned value = 0, digits = 0;
                while (*p >= '0' && *p <= '9') { value = value * 10 + (unsigned)(*p++ - '0'); if (++digits > 3 || value > 255) return false; }
                if (!digits) return false;
                octets[n++] = value;
                if (n < 4) { if (*p != '.') return false; p++; }
            }
            if (*p) return false;
            words[count++] = (uint16_t)(octets[0] << 8 | octets[1]); words[count++] = (uint16_t)(octets[2] << 8 | octets[3]);
            break;
        }
        unsigned value = 0, digits = 0;
        while (hex_value(*p) >= 0) { value = value << 4 | (unsigned)hex_value(*p++); if (++digits > 4) return false; }
        if (!digits || count >= 8) return false;
        words[count++] = (uint16_t)value;
        if (!*p) break;
        if (*p != ':') return false;
        p++;
        if (*p == ':') { if (gap >= 0) return false; gap = count; p++; if (!*p) break; }
        else if (!*p) return false;
    }
    if (gap < 0 ? count != 8 : count > 7) return false;
    int fill = gap < 0 ? 0 : 8 - count, w = 0;
    for (int i = 0; i < count; i++) {
        if (i == gap) w += fill;
        out->b[2 * w] = (uint8_t)(words[i] >> 8); out->b[2 * w + 1] = (uint8_t)words[i]; w++;
    }
    return true;
}

/* ---- the registry ---- */
struct netdev* netdev_by_index(unsigned index) {
    for (struct netdev* d = netdevs; d; d = d->next) if (d->index == index) return d;
    return NULL;
}
struct netdev* netdev_by_name(const char* name) {
    for (struct netdev* d = netdevs; d; d = d->next) if (!strncmp(d->name, name, IFNAMSIZ)) return d;
    return NULL;
}
unsigned netdev_count(void) { unsigned n = 0; for (struct netdev* d = netdevs; d; d = d->next) n++; return n; }
static bool name_valid(const char* name) {
    unsigned n = strlen(name);
    if (!n || n >= IFNAMSIZ || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (unsigned i = 0; i < n; i++) if (name[i] == '/' || name[i] == ':' || name[i] <= ' ' || name[i] == 127) return false;
    return true;
}
int netdev_name_template(const char* name, const char* fallback, char out[IFNAMSIZ]) {
    const char* pattern = name && name[0] ? name : fallback;
    if (strlen(pattern) >= IFNAMSIZ) return -22;
    const char* percent = strstr(pattern, "%d");
    if (!percent) { if (!name_valid(pattern)) return -22; strcpy(out, pattern); return 0; }
    if (strstr(percent + 1, "%")) return -22;
    for (unsigned n = 0; n < 32768; n++) {
        char candidate[40]; unsigned prefix = (unsigned)(percent - pattern);
        memcpy(candidate, pattern, prefix); char* p = put_dec(candidate + prefix, n); strcpy(p, percent + 2);
        if (strlen(candidate) >= IFNAMSIZ) return -23;
        if (!netdev_by_name(candidate)) { if (!name_valid(candidate)) return -22; strcpy(out, candidate); return 0; }
    }
    return -23;   /* ENFILE, as dev_alloc_name */
}
static void conf6_defaults(struct ipv6_conf* c, uint16_t type, unsigned mtu) {
    memset(c, 0, sizeof(*c));
    c->hop_limit = 64; c->mtu = (int32_t)(mtu < 1280 ? 1280 : mtu); c->accept_ra = 1; c->accept_redirects = 1; c->autoconf = 1;
    c->dad_transmits = 1; c->router_solicitations = -1; c->router_solicitation_interval = 4; c->router_solicitation_delay = 1;
    c->router_solicitation_max_interval = 3600;
    c->use_tempaddr = type == ARPHRD_ETHER ? 2 : 0;   /* privacy extensions preferred on Ethernet (RFC 8981) */
    c->temp_valid_lft = 604800; c->temp_prefered_lft = 86400; c->regen_max_retry = 3; c->max_desync_factor = 600;
    c->max_addresses = 16; c->accept_ra_defrtr = 1; c->accept_ra_pinfo = 1; c->accept_ra_mtu = 1; c->accept_ra_min_hop_limit = 1;
    c->accept_dad = 1; c->addr_gen_mode = 0; c->mldv1_unsolicited_report_interval = 10000; c->mldv2_unsolicited_report_interval = 1000;
    c->enhanced_dad = 1; c->suppress_frag_ndisc = 1; c->accept_source_route = 0;
    if (type == ARPHRD_LOOPBACK) { c->accept_ra = 0; c->autoconf = 0; c->dad_transmits = 0; c->accept_dad = -1; }
}
struct netdev* netdev_create(const char* name, uint16_t type, const struct netdev_ops* ops, void* priv) {
    if (!name_valid(name) || netdev_by_name(name)) return NULL;
    struct netdev* d = net_alloc(sizeof(struct netdev)); if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    strcpy(d->name, name); d->type = type; d->ops = ops; d->priv = priv; d->present = true; d->references = 1;
    d->index = next_index++;
    if (next_index > 0x7FFFFFF0U) next_index = 16;
    while (netdev_by_index(d->index)) d->index = next_index++;
    d->mtu = type == ARPHRD_LOOPBACK ? 65536 : 1500; d->min_mtu = type == ARPHRD_ETHER ? 68 : 68; d->max_mtu = type == ARPHRD_LOOPBACK ? 65536 : 65535;
    d->txqlen = type == ARPHRD_LOOPBACK ? 1000 : 1000;
    d->addr_len = type == ARPHRD_ETHER ? 6 : type == ARPHRD_LOOPBACK ? 6 : 0;
    if (type == ARPHRD_ETHER) memset(d->broadcast, 0xFF, 6);
    d->flags = type == ARPHRD_LOOPBACK ? IFF_LOOPBACK : type == ARPHRD_ETHER ? IFF_BROADCAST | IFF_MULTICAST : IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
    d->operstate = 2;
    conf6_defaults(&d->conf6, type, d->mtu); d->mtu6 = d->mtu < 1280 ? 1280 : d->mtu > 65535 ? 65535 : d->mtu;
    d->base_reachable_ms = d->reachable_time_ms = 30000; d->retrans_ms = 1000; d->ra_hop_limit = 64; d->mld_version = 2;
    d->mld_qrv = 2; d->mld_qi_s = 125; d->mld_qri_ms = 10000;
    d->rp_filter = 0; d->created_ns = native_clock_ns();
    struct netdev** link = &netdevs;
    while (*link && (*link)->index < d->index) link = &(*link)->next;
    d->next = *link; *link = d;
    return d;
}
void netdev_retain(struct netdev* dev) { dev->references++; }
void netdev_release(struct netdev* dev) {
    if (!dev->references) panic("netdev reference underflow");
    if (--dev->references) return;
    if (dev->present) panic("registered netdev freed");
    net_free(dev, sizeof(*dev));
}
int netdev_unregister(struct netdev* dev) {
    if (!dev->present) return -19;
    if (dev == netdev_lo || dev == netdev_eth0) return -95;
    if (dev->flags & IFF_UP) netdev_set_flags(dev, 0, IFF_UP);
    addrconf_flush_dev(dev, true);
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (dev->v4[i].used) netdev_v4_del(dev, dev->v4[i].address, dev->v4[i].plen);
    route_flush_dev(dev, 0); neigh_flush_dev(dev); mld_dev_down(dev); igmp_dev_down(dev); dhcp6_dev_down(dev);
    mc_free_all(dev);
    inet_dev_down(dev); packet_dev_down(dev);
    for (struct netdev** link = &netdevs; *link; link = &(*link)->next) if (*link == dev) { *link = dev->next; break; }
    dev->present = false;
    rtnl_notify_link(dev, 17 /* RTM_DELLINK */);
    if (dev->ops && dev->ops->destroy) dev->ops->destroy(dev);
    netdev_release(dev);
    return 0;
}
bool netdev_running(const struct netdev* dev) { return dev && dev->present && (dev->flags & IFF_UP) && dev->carrier; }
uint32_t netdev_flags(const struct netdev* dev) {
    uint32_t flags = dev->flags & ~(IFF_RUNNING | IFF_LOWER_UP | IFF_PROMISC | IFF_ALLMULTI);
    if (dev->carrier) flags |= IFF_LOWER_UP;
    if ((dev->flags & IFF_UP) && dev->carrier) flags |= IFF_RUNNING;
    if (dev->promiscuity) flags |= IFF_PROMISC;
    if (dev->allmulti) flags |= IFF_ALLMULTI;
    return flags;
}
static void operstate_update(struct netdev* dev) {
    uint8_t state = !(dev->flags & IFF_UP) ? 2 : dev->carrier ? 6 : 2;   /* IF_OPER_UP / IF_OPER_DOWN */
    if (dev->type == ARPHRD_LOOPBACK && (dev->flags & IFF_UP)) state = 0;  /* lo reports UNKNOWN, as Linux */
    dev->operstate = state;
}
static void link_changed(struct netdev* dev) { operstate_update(dev); rtnl_notify_link(dev, 16 /* RTM_NEWLINK */); }
int netdev_set_flags(struct netdev* dev, uint32_t flags, uint32_t mask) {
    mask &= IFF_UP | IFF_PROMISC | IFF_ALLMULTI | IFF_MULTICAST | IFF_NOARP;
    bool was_up = (dev->flags & IFF_UP) != 0, was_running = netdev_running(dev);
    if (mask & IFF_PROMISC) {
        bool on = (flags & IFF_PROMISC) != 0;
        if (on != dev->user_promisc) { dev->user_promisc = on; netdev_promiscuity(dev, on ? 1 : -1); }
    }
    if (mask & IFF_ALLMULTI) {
        bool on = (flags & IFF_ALLMULTI) != 0;
        if (on != dev->user_allmulti) { dev->user_allmulti = on; netdev_allmulti(dev, on ? 1 : -1); }
    }
    uint32_t keep = IFF_UP | IFF_MULTICAST | IFF_NOARP;
    dev->flags = (dev->flags & ~(mask & keep)) | (flags & mask & keep);
    bool up = (dev->flags & IFF_UP) != 0;
    if (up != was_up) {
        if (dev->ops && dev->ops->state) dev->ops->state(dev);
        if (up) {
            /* Local and connected routes of the IPv4 addresses come back with the link. */
            for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (dev->v4[i].used) {
                struct netdev_v4 a = dev->v4[i]; dev->v4[i].used = false;
                netdev_v4_add(dev, a.address, a.plen, a.peer, a.broadcast, a.label, false);
            }
            if (dev == netdev_eth0) { mirror_valid = false; netdev_sync_legacy(); }
        } else route_flush_dev(dev, AF_INET);   /* IPv4 addresses stay, every route through the interface goes (Linux) */
    }
    bool running = netdev_running(dev);
    if (running != was_running) { if (running) ndisc_dev_up(dev); else ndisc_dev_down(dev); }
    netdev_rx_filter(dev);
    link_changed(dev);
    return 0;
}
int netdev_set_mtu(struct netdev* dev, unsigned mtu) {
    if (mtu < dev->min_mtu || mtu > dev->max_mtu) return -22;
    if (dev == netdev_eth0 && mtu > 1500) return -22;   /* the NIC rings hold standard frames */
    dev->mtu = mtu;
    if (mtu < 1280) {
        /* IPv6 needs 1280: the interface loses its IPv6 configuration (Linux addrconf NETDEV_CHANGEMTU). */
        addrconf_flush_dev(dev, true); route_flush_dev(dev, AF_INET6);
    } else {
        dev->mtu6 = mtu; dev->conf6.mtu = (int32_t)mtu;
        if (netdev_running(dev) && !dev->addrs6) ndisc_dev_up(dev);
    }
    link_changed(dev);
    return 0;
}
int netdev_set_name(struct netdev* dev, const char* name) {
    if (!name_valid(name)) return -22;
    if (!strcmp(dev->name, name)) return 0;
    if (netdev_by_name(name)) return -17;
    if (dev->flags & IFF_UP) return -16;   /* EBUSY: Linux renames only interfaces that are down */
    strcpy(dev->name, name); link_changed(dev); return 0;
}
int netdev_set_address(struct netdev* dev, const uint8_t* address, unsigned length) {
    if (dev->type != ARPHRD_ETHER || length != 6) return -95;
    if ((address[0] & 1) || !(address[0] | address[1] | address[2] | address[3] | address[4] | address[5])) return -99;
    if (dev == netdev_eth0) return -95;    /* the NIC receive address register is programmed at boot only */
    memcpy(dev->addr, address, 6); link_changed(dev); return 0;
}
void netdev_set_carrier(struct netdev* dev, bool carrier) {
    if (dev->carrier == carrier) return;
    bool was_running = netdev_running(dev);
    dev->carrier = carrier; dev->carrier_changes++;
    if (carrier) dev->carrier_up_count++; else dev->carrier_down_count++;
    bool running = netdev_running(dev);
    if (running != was_running) { if (running) ndisc_dev_up(dev); else ndisc_dev_down(dev); }
    ndisc_dev_carrier(dev, carrier);
    link_changed(dev);
}
void netdev_promiscuity(struct netdev* dev, int delta) {
    if (delta < 0 && !dev->promiscuity) return;
    dev->promiscuity = (unsigned)((int)dev->promiscuity + delta); netdev_rx_filter(dev);
}
void netdev_allmulti(struct netdev* dev, int delta) {
    if (delta < 0 && !dev->allmulti) return;
    dev->allmulti = (unsigned)((int)dev->allmulti + delta); netdev_rx_filter(dev);
}

/* ---- link-layer receive filter ---- */
static void mc_mac6(const struct ip6a* group, uint8_t mac[6]) { mac[0] = 0x33; mac[1] = 0x33; memcpy(mac + 2, group->b + 12, 4); }
static void mc_mac4(uint32_t group, uint8_t mac[6]) {
    mac[0] = 1; mac[1] = 0; mac[2] = 0x5E; mac[3] = (uint8_t)((group >> 16) & 0x7F); mac[4] = (uint8_t)(group >> 8); mac[5] = (uint8_t)group;
}
bool netdev_l2_wanted(struct netdev* dev, const uint8_t* destination) {
    if (!(destination[0] & 1)) return !memcmp(destination, dev->addr, 6);
    if (!memcmp(destination, dev->broadcast, 6)) return true;
    if (dev->allmulti) return true;
    uint8_t mac[6];
    for (struct mc6_group* g = dev->mc6; g; g = g->next) { mc_mac6(&g->group, mac); if (!memcmp(mac, destination, 6)) return true; }
    for (struct mc4_group* g = dev->mc4; g; g = g->next) { mc_mac4(ip6a_v4(&g->group), mac); if (!memcmp(mac, destination, 6)) return true; }
    /* 224.0.0.1 (all hosts) is always joined, as on Linux */
    static const uint8_t all_hosts[6] = {1, 0, 0x5E, 0, 0, 1};
    return !memcmp(destination, all_hosts, 6);
}
void netdev_rx_filter(struct netdev* dev) {
    if (dev != netdev_eth0) { if (dev->ops && dev->ops->rx_mode) dev->ops->rx_mode(dev); return; }
    static uint8_t list[64][6]; unsigned count = 0; bool overflow = false;
    for (struct mc6_group* g = dev->mc6; g; g = g->next) { if (count < 64) mc_mac6(&g->group, list[count++]); else overflow = true; }
    for (struct mc4_group* g = dev->mc4; g; g = g->next) { if (count < 64) mc_mac4(ip6a_v4(&g->group), list[count++]); else overflow = true; }
    if (count < 64) { static const uint8_t all_hosts[6] = {1, 0, 0x5E, 0, 0, 1}; memcpy(list[count++], all_hosts, 6); }
    native_nic_rx_mode(dev->promiscuity != 0, dev->allmulti != 0 || overflow, (const uint8_t*)list, count);
}

/* ---- receive ---- */
#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
bool netdev_receive(struct netdev* dev, const void* data, unsigned length) {
    const uint8_t* frame = data;
    if (!dev || length < ETH_HEADER_LEN) return true;
    if (dev != netdev_eth0) { dev->stats.rx_packets++; dev->stats.rx_bytes += length; }
    uint8_t pkttype = !(frame[0] & 1) ? (!memcmp(frame, dev->addr, 6) ? PACKET_HOST : PACKET_OTHERHOST) :
                      !memcmp(frame, dev->broadcast, 6) ? PACKET_BROADCAST : PACKET_MULTICAST;
    uint16_t protocol = be16(frame + 12);
    if (pkttype == PACKET_MULTICAST && dev == netdev_eth0) dev->stats.multicast++;
    if (packet_socket_count) packet_tap(dev, frame, length, pkttype, protocol, false);
    if (!(dev->flags & IFF_UP)) return true;
    if (pkttype == PACKET_OTHERHOST) return true;                       /* promiscuous capture only */
    if (pkttype == PACKET_MULTICAST && !netdev_l2_wanted(dev, frame)) return true;   /* imperfect hash filter */
    if (protocol == ETH_TYPE_IPV6) {
        netdev_rx_current = dev; ipv6_receive(dev, frame + ETH_HEADER_LEN, length - ETH_HEADER_LEN, pkttype); netdev_rx_current = NULL;
        return true;
    }
    if (dev != netdev_eth0) {
        if (protocol == ETH_TYPE_ARP) ndisc_arp_receive(dev, frame, length);
        else if (protocol == ETH_TYPE_IPV4) ip4_input_device(dev, frame + ETH_HEADER_LEN, length - ETH_HEADER_LEN);
        else dev->stats.rx_nohandler++;
        return true;
    }
    if (protocol != ETH_TYPE_ARP && protocol != ETH_TYPE_IPV4) { dev->stats.rx_nohandler++; return true; }
    netdev_rx_current = dev;
    return false;   /* kernel/net.c hands it to ARP / IPv4 (and clears netdev_rx_current via netdev_receive_done) */
}
/* kernel/net.c hooks for eth0 (native_nic -> net_deliver_packet / net_send_packet) */
bool netdev_receive_eth0(const void* data, unsigned length) { return netdev_eth0 ? netdev_receive(netdev_eth0, data, length) : false; }
void netdev_receive_done(void) { netdev_rx_current = NULL; }
void netdev_transmit_tap(net_device_t* hw, const void* data, unsigned length) {
    if (packet_socket_count && netdev_eth0 && hw == netdev_eth0->hw && length >= ETH_HEADER_LEN)
        packet_tap(netdev_eth0, data, length, 4 /* PACKET_OUTGOING */, be16((const uint8_t*)data + 12), true);
}
void netdev_input_l3(struct netdev* dev, const void* packet, unsigned length, uint16_t protocol, uint8_t pkttype) {
    dev->stats.rx_packets++; dev->stats.rx_bytes += length;
    if (packet_socket_count) packet_tap(dev, packet, length, pkttype, protocol, false);
    if (!(dev->flags & IFF_UP)) { dev->stats.rx_dropped++; return; }
    if (protocol == ETH_TYPE_IPV6) { netdev_rx_current = dev; ipv6_receive(dev, packet, length, pkttype); netdev_rx_current = NULL; }
    else if (protocol == ETH_TYPE_IPV4) ip4_input_device(dev, packet, length);
    else dev->stats.rx_nohandler++;
}
int netdev_xmit(struct netdev* dev, const void* data, unsigned length, uint16_t protocol) {
    if (!dev->present || !(dev->flags & IFF_UP)) { dev->stats.tx_dropped++; return -100; }   /* ENETDOWN */
    if (!dev->carrier) { dev->stats.tx_dropped++; return -100; }
    if (dev != netdev_eth0 && packet_socket_count) {
        if (dev->type == ARPHRD_ETHER) packet_tap(dev, data, length, 4 /* PACKET_OUTGOING */, be16((const uint8_t*)data + 12), true);
        else packet_tap(dev, data, length, 4, protocol, true);
    }
    int result = dev->ops->xmit(dev, data, length);
    if (dev != netdev_eth0) {
        if (result == 0) { dev->stats.tx_packets++; dev->stats.tx_bytes += length; }
        else if (result == -11) dev->stats.tx_dropped++;
        else dev->stats.tx_errors++;
    }
    return result;
}

/* ---- IPv4 addresses ---- */
static unsigned plen_of(uint32_t mask) { unsigned n = 0; while (n < 32 && (mask & (0x80000000U >> n))) n++; return n; }
static uint32_t mask_of(unsigned plen) { return plen ? 0xFFFFFFFFU << (32 - plen) : 0; }
static void v4_routes(struct netdev* dev, const struct netdev_v4* a, bool add) {
    struct route local = {.family = AF_INET, .plen = 32, .protocol = RTPROT_KERNEL, .scope = RT_SCOPE_HOST, .type = RTN_LOCAL,
                          .table = RT_TABLE_LOCAL, .oif = dev->index};
    local.dst = ip6a_from4(a->address); local.prefsrc = local.dst;
    if (add) route_add(&local, true, false, true); else route_del(&local, true);
    if (a->plen < 32 && dev->type != ARPHRD_LOOPBACK) {
        struct route net = {.family = AF_INET, .plen = a->plen, .protocol = RTPROT_KERNEL, .scope = RT_SCOPE_LINK, .type = RTN_UNICAST,
                            .table = RT_TABLE_MAIN, .oif = dev->index};
        net.dst = ip6a_from4(a->address & mask_of(a->plen)); net.prefsrc = ip6a_from4(a->address);
        if (add) route_add(&net, true, false, true); else route_del(&net, true);
        if (a->broadcast && a->broadcast != a->address) {
            struct route bcast = {.family = AF_INET, .plen = 32, .protocol = RTPROT_KERNEL, .scope = RT_SCOPE_LINK, .type = RTN_BROADCAST,
                                  .table = RT_TABLE_LOCAL, .oif = dev->index};
            bcast.dst = ip6a_from4(a->broadcast); bcast.prefsrc = ip6a_from4(a->address);
            if (add) route_add(&bcast, true, false, true); else route_del(&bcast, true);
        }
    } else if (a->peer && a->peer != a->address) {
        struct route peer = {.family = AF_INET, .plen = 32, .protocol = RTPROT_KERNEL, .scope = RT_SCOPE_LINK, .type = RTN_UNICAST,
                             .table = RT_TABLE_MAIN, .oif = dev->index};
        peer.dst = ip6a_from4(a->peer); peer.prefsrc = ip6a_from4(a->address);
        if (add) route_add(&peer, true, false, true); else route_del(&peer, true);
    } else if (dev->type == ARPHRD_LOOPBACK) {
        struct route net = {.family = AF_INET, .plen = a->plen, .protocol = RTPROT_KERNEL, .scope = RT_SCOPE_HOST, .type = RTN_LOCAL,
                            .table = RT_TABLE_LOCAL, .oif = dev->index};
        net.dst = ip6a_from4(a->address & mask_of(a->plen)); net.prefsrc = ip6a_from4(a->address);
        if (add) route_add(&net, true, false, true); else route_del(&net, true);
    }
}
int netdev_v4_add(struct netdev* dev, uint32_t address, unsigned plen, uint32_t peer, uint32_t broadcast, const char* label, bool notify) {
    if (plen > 32 || !address || address >= 0xE0000000U) return -22;
    struct netdev_v4* free_slot = NULL; bool secondary = false;
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) {
        struct netdev_v4* a = &dev->v4[i];
        if (!a->used) { if (!free_slot) free_slot = a; continue; }
        if (a->address == address && a->plen == plen) return -17;
        if (plen == a->plen && (a->address & mask_of(plen)) == (address & mask_of(plen))) secondary = true;
    }
    if (!free_slot) return -28;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true; free_slot->address = address; free_slot->plen = (uint8_t)plen; free_slot->peer = peer;
    free_slot->broadcast = broadcast; free_slot->secondary = secondary;
    free_slot->scope = (address >> 24) == 127 ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
    free_slot->flags = 0x80; /* IFA_F_PERMANENT */
    strcpy(free_slot->label, label && label[0] && strlen(label) < IFNAMSIZ ? label : dev->name);
    if (dev->flags & IFF_UP) v4_routes(dev, free_slot, true);
    if (notify) rtnl_notify_addr4(dev, free_slot, 20 /* RTM_NEWADDR */);
    return 0;
}
int netdev_v4_del(struct netdev* dev, uint32_t address, unsigned plen) {
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) {
        struct netdev_v4* a = &dev->v4[i];
        if (!a->used || a->address != address || (plen <= 32 && a->plen != plen)) continue;
        v4_routes(dev, a, false);
        struct netdev_v4 copy = *a; a->used = false;
        rtnl_notify_addr4(dev, &copy, 21 /* RTM_DELADDR */);
        struct ip6a gone = ip6a_from4(address); conntrack_flush_masquerade(dev->index, &gone);   /* masquerade bound to it */
        return 0;
    }
    return -99;
}
bool netdev_v4_local(uint32_t address) {
    if ((address >> 24) == 127) return true;
    for (struct netdev* d = netdevs; d; d = d->next)
        for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && d->v4[i].address == address) return true;
    return false;
}
struct netdev* netdev_v4_owner(uint32_t address) {
    for (struct netdev* d = netdevs; d; d = d->next)
        for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && d->v4[i].address == address) return d;
    return (address >> 24) == 127 ? netdev_lo : NULL;
}
uint32_t netdev_v4_primary(const struct netdev* dev) {
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (dev->v4[i].used && !dev->v4[i].secondary) return dev->v4[i].address;
    return 0;
}
/* kernel/ip.c owns eth0's primary address (ARP answers for it, DHCP replaces it): mirror it into the interface and
 * its routes whenever it changes. A change made here (netlink, ioctl) writes it back with ip_set_config. */
static net_config_t mirrored;
void netdev_sync_legacy(void) {
    if (!netdev_eth0) return;
    const net_config_t* now = ip_get_config();
    if (mirror_valid && !memcmp(now, &mirrored, sizeof(mirrored))) return;
    net_config_t before = mirrored; bool had = mirror_valid;
    mirrored = *now; mirror_valid = true;
    struct netdev* dev = netdev_eth0;
    if (had && before.ip && (before.ip != now->ip || before.subnet_mask != now->subnet_mask))
        netdev_v4_del(dev, before.ip, plen_of(before.subnet_mask));
    if (had && before.gateway && before.gateway != now->gateway) {
        struct route old = {.family = AF_INET, .table = RT_TABLE_MAIN, .oif = dev->index, .protocol = RTPROT_DHCP};
        old.gateway = ip6a_from4(before.gateway); route_del(&old, true);
    }
    if (now->ip && (!had || before.ip != now->ip || before.subnet_mask != now->subnet_mask)) {
        unsigned plen = plen_of(now->subnet_mask);
        int error = netdev_v4_add(dev, now->ip, plen, 0, plen < 31 ? (now->ip | ~now->subnet_mask) : 0, dev->name, true);
        if (error == -17) {   /* re-add after link up: refresh routes only */
            for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (dev->v4[i].used && dev->v4[i].address == now->ip && (dev->flags & IFF_UP)) v4_routes(dev, &dev->v4[i], true);
        }
    }
    /* a DHCP lease makes the address dynamic (ip addr: "dynamic", valid_lft), as a network manager configures it */
    uint32_t leased; uint64_t expires;
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (dev->v4[i].used && dev->v4[i].address == now->ip) {
        bool dynamic = native_dhcp_lease(&leased, NULL, &expires) && leased == now->ip;
        uint64_t value = dynamic ? (expires == ~0ULL ? 0 : expires) : 0;
        if (dev->v4[i].expires_ns != value) { dev->v4[i].expires_ns = value; dev->v4[i].flags = value ? 0 : 0x80; rtnl_notify_addr4(dev, &dev->v4[i], 20); }
    }
    if (now->gateway && now->ip && (dev->flags & IFF_UP)) {
        struct route def = {.family = AF_INET, .plen = 0, .protocol = RTPROT_DHCP, .scope = RT_SCOPE_UNIVERSE, .type = RTN_UNICAST,
                            .table = RT_TABLE_MAIN, .oif = dev->index, .metric = 100};
        def.gateway = ip6a_from4(now->gateway); def.prefsrc = ip6a_from4(now->ip);
        route_add(&def, true, false, true);
    }
}

/* ---- the eth0 / lo devices ---- */
static int eth0_xmit(struct netdev* dev, const void* data, unsigned length) {
    if (length > ETH_FRAME_MAX) return -90;
    return net_send_packet(dev->hw, data, (uint16_t)length) == 0 ? 0 : -11;
}
static const struct netdev_ops eth0_ops = {eth0_xmit, NULL, NULL, NULL};
static int lo_xmit(struct netdev* dev, const void* data, unsigned length) { (void)dev; (void)data; (void)length; return -95; }
static const struct netdev_ops lo_ops = {lo_xmit, NULL, NULL, NULL};
bool netdev_ipv6_enabled(const struct netdev* dev) { return dev && !dev->conf6.disable_ipv6 && dev->mtu >= 1280; }
void netdev_init(void) {
    netmem_init(); route_init(); ndisc_init(); ipv6_init(); nf_init(); conntrack_init();
    if (tick_register_poller) { tick_register_poller("mcast", mc_next_event); tick_register_poller("wifi", wifi_next_event); }
    netdev_lo = netdev_create("lo", ARPHRD_LOOPBACK, &lo_ops, NULL);
    if (!netdev_lo) panic("loopback interface");
    netdev_lo->carrier = true; netdev_lo->flags |= IFF_UP; netdev_lo->operstate = 0;
    netdev_v4_add(netdev_lo, 0x7F000001U, 8, 0, 0, "lo", false);
    struct ip6a one = ip6a_loopback_address; struct inet6_ifaddr* ifa;
    addrconf_add(netdev_lo, &one, 128, IFA_F_PERMANENT, LIFETIME_INFINITE, LIFETIME_INFINITE, ORIGIN_MANUAL, NULL, 0, false, &ifa);
    net_device_t* hw = native_network_available() ? net_get_device(0) : NULL;
    if (hw) {
        struct netdev* d = netdev_create(hw->name && hw->name[0] ? hw->name : "eth0", ARPHRD_ETHER, &eth0_ops, NULL);
        if (!d) panic("eth0 interface");
        d->hw = hw; memcpy(d->addr, hw->mac.addr, 6); memcpy(d->perm_addr, hw->mac.addr, 6); d->max_mtu = 1500;
        d->carrier = hw->link_up; d->flags |= IFF_UP;
        netdev_eth0 = d;
        operstate_update(d);
        netdev_sync_legacy();
        netdev_rx_filter(d);
        if (netdev_running(d)) ndisc_dev_up(d);
    }
}
void netdev_poll(uint64_t now) {
    if (netdev_eth0) {
        netdev_sync_legacy();
        bool link = netdev_eth0->hw->link_up;
        if (link != netdev_eth0->carrier) netdev_set_carrier(netdev_eth0, link);
    }
    ndisc_poll(now); ipv6_poll(now); route_poll(now); dhcp6_poll(now); nf_poll(now); conntrack_poll(now); packet_poll(now); wireguard_poll(now); wifi_poll(now);
    for (struct netdev* d = netdevs; d; d = d->next) mld_poll(d, now);
    ip6h_gc(now, false);
}
