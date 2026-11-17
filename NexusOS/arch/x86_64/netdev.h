/* Native network core shared by the interface layer (netdev.c), routing (route.c), IPv4 output (net4.c), IPv6
 * (ipv6.c, icmp6.c, ndisc.c, dhcp6.c), sockets (inet.c, packet.c), TUN/TAP (tun.c), WireGuard (wireguard.c),
 * the packet filter (nft.c, conntrack.c), rtnetlink (netlink.c) and /proc (netproc.c).
 *
 * Concurrency: every function here runs with the big kernel lock held (system calls, the BSP network poll and
 * bsp_device_polls, which only runs for interrupts taken in user mode), so the network state needs no finer
 * locks. Nothing here sleeps except socket calls, which re-validate user memory after a wait.
 *
 * Addresses: struct ip6a holds an IPv6 address; IPv4 addresses are stored IPv4-mapped (::ffff:a.b.c.d), so one
 * type serves routes, neighbours, sockets and the filter. Host byte order applies only to the uint32_t IPv4
 * values the legacy kernel/ code uses. */
#ifndef NEXUS_NETDEV_H
#define NEXUS_NETDEV_H
#include "native.h"
#include "../../kernel/net.h"

struct ip6a { uint8_t b[16]; };
static inline bool ip6a_eq(const struct ip6a* a, const struct ip6a* b) { return !__builtin_memcmp(a, b, 16); }
static inline bool ip6a_any(const struct ip6a* a) { for (unsigned i = 0; i < 16; i++) if (a->b[i]) return false; return true; }
static inline bool ip6a_is4(const struct ip6a* a) {
    for (unsigned i = 0; i < 10; i++) if (a->b[i]) return false;
    return a->b[10] == 0xFF && a->b[11] == 0xFF;
}
static inline uint32_t ip6a_v4(const struct ip6a* a) {   /* host order */
    return (uint32_t)a->b[12] << 24 | (uint32_t)a->b[13] << 16 | (uint32_t)a->b[14] << 8 | a->b[15];
}
static inline struct ip6a ip6a_from4(uint32_t ip) {       /* host order -> ::ffff:ip */
    struct ip6a a = {{0}}; a.b[10] = a.b[11] = 0xFF;
    a.b[12] = (uint8_t)(ip >> 24); a.b[13] = (uint8_t)(ip >> 16); a.b[14] = (uint8_t)(ip >> 8); a.b[15] = (uint8_t)ip; return a;
}
static inline bool ip6a_loopback(const struct ip6a* a) { for (unsigned i = 0; i < 15; i++) if (a->b[i]) return false; return a->b[15] == 1; }
static inline bool ip6a_multicast(const struct ip6a* a) { return a->b[0] == 0xFF; }
static inline bool ip6a_linklocal(const struct ip6a* a) { return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80; }
static inline unsigned ip6a_mc_scope(const struct ip6a* a) { return a->b[1] & 15; }
bool ip6a_prefix_eq(const struct ip6a* a, const struct ip6a* b, unsigned length);
void ip6a_mask(struct ip6a* a, unsigned length);           /* clear the bits after `length` */
void ip6a_solicited(const struct ip6a* target, struct ip6a* group); /* ff02::1:ffXX:XXXX */
unsigned ip6a_text(const struct ip6a* a, char* out);         /* RFC 5952 text (v4-mapped as ::ffff:a.b.c.d); length */
bool ip6a_parse(const char* text, struct ip6a* out);
extern const struct ip6a ip6a_all_nodes, ip6a_all_routers, ip6a_mld2_routers, ip6a_unspecified, ip6a_loopback_address;

/* Linux constants used across the files. */
#define AF_INET 2
#define AF_INET6 10
#define AF_PACKET 17
#define IPPROTO_HOPOPTS 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_ESP 50
#define IPPROTO_AH 51
#define IPPROTO_ICMPV6 58
#define IPPROTO_NONE 59
#define IPPROTO_DSTOPTS 60
#define IPPROTO_RAW 255
#define IFF_UP 0x1U
#define IFF_BROADCAST 0x2U
#define IFF_LOOPBACK 0x8U
#define IFF_POINTOPOINT 0x10U
#define IFF_RUNNING 0x40U
#define IFF_NOARP 0x80U
#define IFF_PROMISC 0x100U
#define IFF_ALLMULTI 0x200U
#define IFF_MULTICAST 0x1000U
#define IFF_LOWER_UP 0x10000U
#define IFF_DORMANT 0x20000U
#define ARPHRD_ETHER 1
#define ARPHRD_LOOPBACK 772
#define ARPHRD_NONE 65534
#define IFNAMSIZ 16

/* ---- IPv6 address flags (IFA_F_*) and per-interface configuration (/proc/sys/net/ipv6/conf) ---- */
#define IFA_F_TEMPORARY 0x01U
#define IFA_F_NODAD 0x02U
#define IFA_F_OPTIMISTIC 0x04U
#define IFA_F_DADFAILED 0x08U
#define IFA_F_HOMEADDRESS 0x10U
#define IFA_F_DEPRECATED 0x20U
#define IFA_F_TENTATIVE 0x40U
#define IFA_F_PERMANENT 0x80U
#define IFA_F_MANAGETEMPADDR 0x100U
#define IFA_F_NOPREFIXROUTE 0x200U
#define IFA_F_MCAUTOJOIN 0x400U
#define IFA_F_STABLE_PRIVACY 0x800U
#define LIFETIME_INFINITE 0xFFFFFFFFU
struct ipv6_conf {
    int32_t forwarding, hop_limit, mtu, accept_ra, accept_redirects, autoconf, dad_transmits, router_solicitations,
            router_solicitation_interval, router_solicitation_delay, use_tempaddr, temp_valid_lft, temp_prefered_lft,
            regen_max_retry, max_desync_factor, max_addresses, accept_ra_defrtr, accept_ra_pinfo, accept_ra_mtu,
            accept_ra_min_hop_limit, accept_source_route, accept_dad, disable_ipv6, addr_gen_mode, force_mld_version,
            mldv1_unsolicited_report_interval, mldv2_unsolicited_report_interval, keep_addr_on_down, ndisc_notify,
            drop_unsolicited_na, accept_ra_rt_info_max_plen, proxy_ndp, ignore_routes_with_linkdown, enhanced_dad,
            router_solicitation_max_interval, optimistic_dad, use_optimistic, suppress_frag_ndisc, accept_ra_from_local,
            drop_unicast_in_l2_multicast, ndisc_tclass, rpl_seg_enabled, accept_untracked_na;
    bool stable_secret_set;
    struct ip6a stable_secret;
};
struct inet6_ifaddr {
    struct inet6_ifaddr* next;
    struct ip6a addr, peer;
    uint8_t plen, scope;                 /* scope: RT_SCOPE_UNIVERSE 0 / LINK 253 / HOST 254 */
    uint32_t flags;                      /* IFA_F_* */
    uint8_t origin;                      /* ORIGIN_* */
    uint32_t valid_lft, prefered_lft;    /* seconds after `stamp_ns` (LIFETIME_INFINITE: never) */
    uint64_t created_ns, stamp_ns;
    uint8_t dad_probes, regen_count, dad_count;
    uint8_t dad_nonce[6];                /* RFC 7527 nonce of the last probe */
    uint64_t dad_next_ns;
    struct inet6_ifaddr* public_parent; /* temporary address: the SLAAC address it was made for */
    uint64_t regen_at_ns;                /* SLAAC address with temporaries: next temporary due */
    bool temp_regenerated;
    uint32_t route_metric;
    char label[IFNAMSIZ];
};
enum { ORIGIN_KERNEL_LL = 1, ORIGIN_RA = 2, ORIGIN_MANUAL = 3, ORIGIN_DHCP = 4, ORIGIN_TEMP = 5 };
/* ---- multicast (mcast.c): interface groups of both families (dev->mc4, dev->mc6), socket memberships ---- */
#define MCAST_EXCLUDE 0
#define MCAST_INCLUDE 1
struct mc_state { uint8_t mode; uint16_t count; struct ip6a* sources; };   /* a filter: mode + allocated source list */
struct mc_group {
    struct mc_group* next;
    struct ip6a group;                   /* IPv4 groups are IPv4-mapped */
    unsigned users;                      /* socket memberships + kernel joins */
    unsigned kernel_users;               /* kernel joins (IPv6 all-nodes, solicited-node): EXCLUDE{} */
    bool leaving;                        /* no member left: the leave is still being reported */
    bool full;                           /* this burst reports the full state (TO_IN/TO_EX) */
    bool sent;                           /* this burst has been transmitted at least once */
    bool answer_all;                     /* the pending query answer covers the whole group */
    uint8_t change_left;                 /* state-change reports still to send (robustness variable) */
    uint64_t change_ns;                  /* next state-change report, 0: none */
    uint64_t answer_ns;                  /* pending answer to a group(-and-source)-specific query, 0: none */
    struct mc_state reported;            /* the state routers last heard */
    struct mc_state before;              /* the state before the current burst of state-change reports */
    struct mc_state asked;               /* sources of pending group-and-source-specific queries */
};
#define mc4_group mc_group
#define mc6_group mc_group
/* A socket's membership (inet.c keeps the list): group, interface and source filter (RFC 3678). */
struct mc_membership { struct mc_membership* next; struct ip6a group; unsigned ifindex; uint8_t mode; uint16_t count, capacity; struct ip6a* sources; };
enum { MC_JOIN = 1, MC_LEAVE, MC_JOIN_SOURCE, MC_LEAVE_SOURCE, MC_BLOCK, MC_UNBLOCK };

/* ---- interfaces ---- */
struct netdev;
struct netdev_ops {
    /* Transmit one frame (ARPHRD_ETHER: Ethernet header included) or one L3 packet (ARPHRD_NONE / loopback). 0 or
     * -errno (-11: ring full, the packet is dropped like a full qdisc would). */
    int (*xmit)(struct netdev* dev, const void* data, unsigned length);
    void (*rx_mode)(struct netdev* dev); /* promiscuous / multicast filter changed */
    void (*destroy)(struct netdev* dev); /* the interface is going away (RTM_DELLINK, tun close) */
    void (*state)(struct netdev* dev);   /* administrative up/down changed */
};
struct netdev_stats {
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors, rx_dropped, tx_dropped, multicast,
             rx_frame_errors, collisions, rx_nohandler;
};
#define NETDEV_V4_MAX 8
struct netdev_v4 { uint32_t address, peer, broadcast; uint8_t plen, scope, flags; char label[IFNAMSIZ]; bool used, secondary; uint64_t expires_ns; /* DHCP lease end, 0: permanent */ };
struct netdev {
    struct netdev* next;
    unsigned index;
    char name[IFNAMSIZ];
    char kind[16];                        /* IFLA_INFO_KIND: "" (hardware/loopback), "tun", "wireguard", "dummy" */
    uint16_t type;                        /* ARPHRD_* */
    uint32_t flags;                       /* administrative IFF_* (RUNNING/LOWER_UP are computed) */
    bool carrier, present;                /* present: false once unregistered (the object lives on while referenced) */
    unsigned mtu, min_mtu, max_mtu, txqlen, references;
    uint8_t addr[6], broadcast[6], perm_addr[6];
    unsigned addr_len;
    uint8_t operstate;                    /* IF_OPER_* (2 down, 6 up) */
    uint64_t carrier_changes, carrier_up_count, carrier_down_count;
    net_device_t* hw;                     /* the NIC behind eth0 */
    const struct netdev_ops* ops;
    void* priv;
    struct netdev_stats stats;
    unsigned promiscuity, allmulti;       /* counts (packet sockets, `ip link set promisc on` adds one) */
    bool user_promisc, user_allmulti;
    /* IPv4 */
    struct netdev_v4 v4[NETDEV_V4_MAX];
    struct mc4_group* mc4;
    uint64_t igmp_v1_until_ns, igmp_v2_until_ns, igmp_general_ns;   /* older-version querier present; general answer */
    uint32_t igmp_qrv, igmp_qi_s;                                   /* querier robustness / interval (IGMPv3 queries) */
    int32_t ipv4_forwarding, rp_filter, src_valid_mark, accept_local, arp_ignore;
    /* IPv6 */
    struct ipv6_conf conf6;
    struct inet6_ifaddr* addrs6;
    struct mc6_group* mc6;
    bool ipv6_ready;                      /* link-local configured and DAD started */
    unsigned rs_sent;
    uint64_t rs_next_ns;
    uint32_t reachable_time_ms, base_reachable_ms, retrans_ms;
    uint8_t ra_hop_limit;
    unsigned ra_mtu;                      /* MTU option of the last RA (0: none) */
    bool ra_managed, ra_other;            /* M and O flags of the last RA */
    uint8_t mld_version;                  /* 2, or 1 while an MLDv1 querier is present */
    uint64_t mld_v1_until_ns, mld_general_ns;
    uint32_t mld_qrv, mld_qi_s, mld_qri_ms;
    uint64_t token_ns;
    uint32_t mtu6;                        /* IPv6 link MTU (conf6.mtu mirror) */
    bool dad_failed_ll;                   /* the EUI-64 link-local address was a duplicate */
    unsigned temp_attempts;
    uint32_t group;                       /* IFLA_GROUP */
    char alias[64];
    uint32_t link_netnsid;
    uint64_t created_ns;
    uint32_t tun_owner, tun_group;        /* tun.c */
    bool tun_persist;
};
extern struct netdev* netdevs;            /* registry, by index */
extern struct netdev* netdev_lo;
extern struct netdev* netdev_eth0;        /* NULL without a NIC */
struct netdev* netdev_by_index(unsigned index);
struct netdev* netdev_by_name(const char* name);
struct netdev* netdev_create(const char* name, uint16_t type, const struct netdev_ops* ops, void* priv);
int netdev_unregister(struct netdev* dev);           /* removes addresses/routes/neighbours, RTM_DELLINK */
void netdev_retain(struct netdev* dev);
void netdev_release(struct netdev* dev);
int netdev_name_template(const char* name, const char* fallback, char out[IFNAMSIZ]); /* "tun%d" -> first free */
bool netdev_running(const struct netdev* dev);        /* IFF_UP and carrier */
uint32_t netdev_flags(const struct netdev* dev);      /* IFF_* as reported (RUNNING, LOWER_UP, PROMISC, ALLMULTI) */
int netdev_set_flags(struct netdev* dev, uint32_t flags, uint32_t mask); /* IFF_UP/PROMISC/ALLMULTI/MULTICAST/NOARP */
int netdev_set_mtu(struct netdev* dev, unsigned mtu);
int netdev_set_name(struct netdev* dev, const char* name);
int netdev_set_address(struct netdev* dev, const uint8_t* address, unsigned length);
void netdev_set_carrier(struct netdev* dev, bool carrier);
void netdev_promiscuity(struct netdev* dev, int delta);
void netdev_allmulti(struct netdev* dev, int delta);
bool netdev_l2_wanted(struct netdev* dev, const uint8_t* destination); /* unicast to us / broadcast / joined group */
int netdev_xmit(struct netdev* dev, const void* data, unsigned length, uint16_t protocol); /* taps + stats */
/* A frame (ARPHRD_ETHER) or packet arrived on `dev`; consumed by the native stack (true) or left to the
 * legacy ARP/IPv4 path in kernel/net.c (false, eth0 only). */
bool netdev_receive(struct netdev* dev, const void* data, unsigned length);
void netdev_input_l3(struct netdev* dev, const void* packet, unsigned length, uint16_t protocol, uint8_t pkttype);
extern struct netdev* netdev_rx_current; /* interface of the packet being delivered (NULL: loopback/local) */
void netdev_poll(uint64_t now);           /* carrier, timers (from native_network_poll) */
void netdev_init(void);
int netdev_v4_add(struct netdev* dev, uint32_t address, unsigned plen, uint32_t peer, uint32_t broadcast, const char* label, bool notify);
int netdev_v4_del(struct netdev* dev, uint32_t address, unsigned plen);
bool netdev_v4_local(uint32_t address);                /* some interface holds it (127/8 included) */
struct netdev* netdev_v4_owner(uint32_t address);
uint32_t netdev_v4_primary(const struct netdev* dev);
void netdev_sync_legacy(void);                         /* mirror kernel/ip.c's eth0 configuration (DHCP) */
bool netdev_ipv6_enabled(const struct netdev* dev);
void netdev_rx_filter(struct netdev* dev);              /* recompute and push the multicast filter */
unsigned netdev_count(void);

/* ---- routing (route.c) ---- */
#define RT_TABLE_UNSPEC 0
#define RT_TABLE_DEFAULT 253
#define RT_TABLE_MAIN 254
#define RT_TABLE_LOCAL 255
#define RTPROT_KERNEL 2
#define RTPROT_BOOT 3
#define RTPROT_STATIC 4
#define RTPROT_RA 9
#define RTPROT_DHCP 16
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK 253
#define RT_SCOPE_HOST 254
#define RT_SCOPE_NOWHERE 255
#define RTN_UNICAST 1
#define RTN_LOCAL 2
#define RTN_BROADCAST 3
#define RTN_ANYCAST 4
#define RTN_MULTICAST 5
#define RTN_BLACKHOLE 6
#define RTN_UNREACHABLE 7
#define RTN_PROHIBIT 8
#define RTN_THROW 9
struct route {
    struct route* next;
    uint8_t family, plen, protocol, scope, type, pref, src_len;
    struct ip6a dst, gateway, prefsrc, src;
    unsigned oif;
    uint32_t table, metric, mtu, flags, advmss, hoplimit;
    uint64_t expires_ns;                  /* 0: permanent */
    uint64_t created_ns;
};
struct rule {
    struct rule* next;
    uint8_t family, action, src_len, dst_len, protocol, tos;
    bool invert;
    uint32_t priority, table, fwmark, fwmask, flags, goto_target;
    int32_t suppress_prefixlen, suppress_ifgroup;
    struct ip6a src, dst;
    char iifname[IFNAMSIZ], oifname[IFNAMSIZ];
    uint8_t l3mdev;
    uint16_t sport_lo, sport_hi, dport_lo, dport_hi;
    uint8_t ip_proto;
};
struct route_result {
    struct netdev* dev;
    struct ip6a nexthop, source;
    const struct route* route;
    uint32_t table, mtu;
    uint8_t type;
    bool gateway;
};
extern struct route* routes;
extern struct rule* rules;
int route_add(const struct route* route, bool replace, bool exclusive, bool notify);
int route_del(const struct route* match, bool notify);   /* fields left 0 match anything */
void route_flush_dev(struct netdev* dev, uint8_t family);
void route_flush_proto(struct netdev* dev, uint8_t family, uint8_t protocol);
/* Route `dst` (oif: SO_BINDTODEVICE/IPV6_PKTINFO/scope id, 0 none; src: a bound source or NULL; mark: SO_MARK).
 * 0 or -ENETUNREACH (-101) / -EHOSTUNREACH (-113) / -EACCES (prohibit) / -EINVAL. */
int route_lookup(uint8_t family, const struct ip6a* dst, const struct ip6a* src, unsigned oif, uint32_t mark,
                 struct route_result* out);
int route_lookup_input(uint8_t family, const struct ip6a* dst, const struct ip6a* src, unsigned iif, uint32_t mark,
                       struct route_result* out);    /* fib saddr/daddr type, rp_filter */
bool source_select6(const struct ip6a* dst, struct netdev* dev, uint32_t preferences, struct ip6a* out);
uint32_t source_select4(uint32_t dst, struct netdev* dev);
int rule_add(const struct rule* rule, bool notify);
int rule_del(const struct rule* rule, bool notify);
void route_init(void);
void route_poll(uint64_t now);           /* expiry of RA routes / redirect / PMTU entries */
unsigned route_mtu(uint8_t family, const struct ip6a* dst, struct netdev* dev);
void pmtu_update(uint8_t family, const struct ip6a* dst, unsigned mtu);
unsigned pmtu_get(uint8_t family, const struct ip6a* dst);
void route_redirect6(const struct ip6a* dst, const struct ip6a* target, struct netdev* dev);
uint16_t ip_route_mss(uint32_t local, uint32_t remote);   /* tcp.c: MSS for an IPv4 address or IPv6 handle pair */

/* ---- neighbours (ndisc.c): IPv6 ND, ARP for non-eth0 Ethernet interfaces, /proc/net/arp view of kernel/arp.c ---- */
#define NUD_INCOMPLETE 0x01
#define NUD_REACHABLE 0x02
#define NUD_STALE 0x04
#define NUD_DELAY 0x08
#define NUD_PROBE 0x10
#define NUD_FAILED 0x20
#define NUD_NOARP 0x40
#define NUD_PERMANENT 0x80
#define NTF_ROUTER 0x80
struct neigh_pending { struct neigh_pending* next; unsigned length; uint16_t protocol; uint8_t data[]; };
struct neighbour {
    struct neighbour* next;
    struct netdev* dev;
    uint8_t family;
    struct ip6a key;
    uint8_t lladdr[6];
    bool has_lladdr;
    uint16_t state;
    uint8_t flags, probes;
    uint64_t confirmed_ns, updated_ns, used_ns, timer_ns;
    struct neigh_pending* queue;
    unsigned queued;
    uint32_t refcnt;
};
extern struct neighbour* neighbours;
struct neighbour* neigh_lookup(struct netdev* dev, uint8_t family, const struct ip6a* key);
int neigh_update_admin(struct netdev* dev, uint8_t family, const struct ip6a* key, const uint8_t* lladdr, uint16_t state,
                       uint8_t flags, bool create, bool replace, bool notify);
int neigh_delete(struct netdev* dev, uint8_t family, const struct ip6a* key, bool notify);
void neigh_flush_dev(struct netdev* dev);
/* Resolve and send an L3 packet on an Ethernet interface: queued while resolution runs (-11 never: queued == 0). */
int neigh_output(struct netdev* dev, uint8_t family, const struct ip6a* nexthop, const void* packet, unsigned length, uint16_t protocol);
void ndisc_init(void);
void ndisc_poll(uint64_t now);
void ndisc_receive(struct netdev* dev, const struct ip6a* src, const struct ip6a* dst, uint8_t hop_limit,
                   const uint8_t* message, unsigned length, bool has_hbh);
void ndisc_dev_up(struct netdev* dev);
void ndisc_dev_down(struct netdev* dev);
void ndisc_forget_ra(struct netdev* dev);   /* accept_ra -> 0: drop the interface's RA/DHCPv6 DNS data */
void ndisc_dev_carrier(struct netdev* dev, bool on);
void addrconf_dad_start(struct netdev* dev, struct inet6_ifaddr* ifa);
int addrconf_add(struct netdev* dev, const struct ip6a* addr, unsigned plen, uint32_t flags, uint32_t valid, uint32_t preferred,
                 uint8_t origin, const struct ip6a* peer, uint32_t metric, bool replace, struct inet6_ifaddr** out);
int addrconf_del(struct netdev* dev, const struct ip6a* addr, unsigned plen, bool notify);
struct inet6_ifaddr* addrconf_find(struct netdev* dev, const struct ip6a* addr);  /* dev NULL: any interface */
bool addrconf_local(const struct ip6a* addr, unsigned scope_ifindex, bool allow_tentative);
void addrconf_ll_generate(struct netdev* dev);
void addrconf_flush_dev(struct netdev* dev, bool all);
void ndisc_arp_receive(struct netdev* dev, const uint8_t* frame, unsigned length);  /* tap ARP */
unsigned dns_servers6(struct ip6a* out, unsigned capacity, char domains[][64], unsigned* domain_count);
uint64_t ndisc_random_ns(uint64_t span_ns);

/* ---- IPv6 (ipv6.c) ---- */
struct ip6_send_options {
    int hop_limit;                        /* -1: route / interface default */
    int tclass;                           /* -1: 0 */
    uint32_t flowlabel;
    unsigned oif;                         /* fixed outgoing interface (scope id, pktinfo, bind to device) */
    bool dontfrag;                        /* IP_PMTUDISC_DO / IPV6_DONTFRAG: EMSGSIZE instead of fragmenting */
    bool df_when_fits;                    /* IPv4 IP_PMTUDISC_WANT: DF on packets that fit, fragments otherwise */
    bool mc_loop;                         /* multicast: deliver a copy locally */
    bool no_route_mc;                     /* NDP/MLD: on-link multicast without a route */
    bool raw_header;                      /* IPV6_HDRINCL: payload is a complete IPv6 packet */
    uint32_t mark;
    const struct ip6a* source;            /* NULL: source selection */
    const uint8_t* ext;                   /* extension headers to insert (hop-by-hop router alert for MLD) */
    unsigned ext_length;
    uint8_t ext_first;                    /* the extension header type the chain starts with */
    void* socket;                         /* sender (EMSGSIZE / PMTU notifications) */
};
void ip6_options_default(struct ip6_send_options* options);
/* Send `payload` as protocol `next` to `dst`; 0 or -errno (-90 EMSGSIZE with dontfrag, -101/-113 no route). */
int ip6_send(const struct ip6a* dst, uint8_t next, const void* payload, unsigned length, const struct ip6_send_options* options);
void ipv6_receive(struct netdev* dev, const void* packet, unsigned length, uint8_t pkttype);
void ipv6_loopback_input(const void* packet, unsigned length);       /* from kernel/ip.c's loopback ring */
uint16_t ip6_checksum(const struct ip6a* src, const struct ip6a* dst, uint8_t next, const void* data, unsigned length);
uint16_t ip6_checksum_parts(const struct ip6a* src, const struct ip6a* dst, uint8_t next, const void* header, unsigned header_length,
                            const void* data, unsigned length);
void ipv6_init(void);
void ipv6_poll(uint64_t now);
unsigned ip6_scope_of(struct netdev* dev, const struct ip6a* address);
bool ipv6_mc_joined(struct netdev* dev, const struct ip6a* group);
struct ipv6_stats {
    uint64_t in_receives, in_hdr_errors, in_too_big, in_no_routes, in_addr_errors, in_unknown_protos, in_truncated, in_discards,
             in_delivers, out_forw, out_requests, out_discards, out_no_routes, reasm_timeout, reasm_reqds, reasm_oks, reasm_fails,
             frag_oks, frag_fails, frag_creates, in_mcast, out_mcast, in_octets, out_octets;
};
extern struct ipv6_stats ip6_stats;
/* IPv6 address handles (see netdev.h head): TCP names IPv6 endpoints by 32-bit values in 240.0.0.0/4. */
#define IP6H_FIRST 0xF0000001U
#define IP6H_LAST 0xFEFFFFFFU
static inline bool ip6h_is(uint32_t value) { return value >= 0xF0000000U && value <= IP6H_LAST; }
uint32_t ip6h_get(const struct ip6a* address, uint32_t scope);       /* find or create; 0: table full */
uint32_t ip6h_find(const struct ip6a* address, uint32_t scope);      /* 0: no such handle */
bool ip6h_address(uint32_t handle, struct ip6a* address, uint32_t* scope);
void ip6h_gc(uint64_t now, bool force);
unsigned ip6h_count(void);
/* Multicast membership (mcast.c): reference counted interface groups of both families (sockets and the kernel),
 * IGMP/MLD reports follow the merged source filters. */
int mc_join_group(struct netdev* dev, const struct ip6a* group, bool kernel);
int mc_leave_group(struct netdev* dev, const struct ip6a* group, bool kernel);
void mc_filter_changed(struct netdev* dev, const struct ip6a* group);
bool mc_joined(struct netdev* dev, const struct ip6a* group);
bool mc4_member_any(uint32_t group);                         /* host order: joined on any interface (or 224.0.0.1) */
int mc6_join(struct netdev* dev, const struct ip6a* group, bool kernel);
int mc6_leave(struct netdev* dev, const struct ip6a* group, bool kernel);
int mc4_join(struct netdev* dev, uint32_t group);
int mc4_leave(struct netdev* dev, uint32_t group);
void mld_query(struct netdev* dev, const struct ip6a* src, const struct ip6a* dst, const uint8_t* message, unsigned length);
void mld_heard_report(struct netdev* dev, const uint8_t* message, unsigned length);
void mld_report_source(struct netdev* dev, struct ip6a* source);   /* icmp6.c: a valid link-local address or :: */
void igmp_receive(struct netdev* dev, const uint8_t* message, unsigned length, uint32_t dst);
void mld_poll(struct netdev* dev, uint64_t now);
void mcast_poll(uint64_t now);
void mld_dev_down(struct netdev* dev);
void mld_dev_up(struct netdev* dev);
void igmp_dev_down(struct netdev* dev);
void igmp_dev_up(struct netdev* dev);
void mc_free_all(struct netdev* dev);
const char* mc_igmp_version(struct netdev* dev);
unsigned mc_proc_filters(bool six, char* out, unsigned capacity);   /* /proc/net/mcfilter[6] */
/* socket side (inet.c owns each socket's list; *count = its length) */
int mc_sock_change(struct mc_membership** list, unsigned* count, int op, struct netdev* dev, const struct ip6a* group, const struct ip6a* source);
int mc_sock_set_filter(struct mc_membership** list, unsigned* count, struct netdev* dev, const struct ip6a* group, uint8_t mode,
                       const struct ip6a* sources, unsigned n);
int mc_sock_get_filter(struct mc_membership* list, unsigned ifindex, const struct ip6a* group, uint8_t* mode, struct ip6a* out,
                       unsigned capacity, unsigned* total);
void mc_sock_drop_all(struct mc_membership** list, unsigned* count);
void mc_sock_dev_gone(struct mc_membership** list, unsigned* count, unsigned ifindex);
bool mc_sock_allows(struct mc_membership* list, bool all, const struct ip6a* group, unsigned ifindex, const struct ip6a* source);
void inet_mc_visit(void (*visit)(struct mc_membership* membership, void* context), void* context);   /* inet.c: every socket's */
void icmp6_local_error(struct netdev* dev, uint8_t type, uint8_t code, uint32_t info, const uint8_t* packet, unsigned length);
/* ---- crypto shared with WireGuard (wgcrypto.c) ---- */
void blake2s(uint8_t* out, unsigned out_length, const void* key, unsigned key_length, const void* data, unsigned length);

/* ---- ICMPv6 (icmp6.c) ---- */
void icmp6_receive(struct netdev* dev, const struct ip6a* src, const struct ip6a* dst, uint8_t hop_limit, const uint8_t* message,
                   unsigned length, const uint8_t* packet, unsigned packet_length, bool has_hbh);
/* Report an error about `packet` (the offending IPv6 packet as received) to its source, RFC 4443 rules. */
void icmp6_error(struct netdev* dev, uint8_t type, uint8_t code, uint32_t info, const uint8_t* packet, unsigned length);
int icmp6_send(const struct ip6a* dst, const struct ip6a* src, struct netdev* dev, uint8_t type, uint8_t code,
               const void* body, unsigned body_length, int hop_limit);
struct icmp6_stats { uint64_t in_msgs, in_errors, in_csum_errors, out_msgs, out_errors, out_rate_limited, in_type[256], out_type[256]; };
extern struct icmp6_stats icmp6_counters;

/* ---- IPv4 (net4.c): native output for kernel/ip.c, local delivery helpers ---- */
int ip4_output(uint32_t src, uint32_t dst, uint8_t protocol, const void* payload, unsigned length, const struct ip6_send_options* options);
bool ip4_local_address(uint32_t address);
void ip4_receive_hook(const void* packet, unsigned length, bool loopback);   /* raw/ping sockets, filter input */
void ip4_input_device(struct netdev* dev, const void* packet, unsigned length);
struct ip4_stats { uint64_t out_requests, out_no_routes, out_discards, frag_fails, in_receives, in_delivers; };
extern struct ip4_stats ip4_counters;

extern int32_t sysctl_owner_raw;   /* netproc.c: /proc/sys/net/nexus/owner_raw */

/* ---- DHCPv4 (dhcp64.c) ---- */
void native_dhcp_enable(bool on);
bool native_dhcp_enabled(void);
bool native_dhcp_lease(uint32_t* ip, uint32_t* server, uint64_t* expires_ns);
unsigned native_dhcp_state(void);
int native_dns_static(const char* text);
unsigned native_dns_static_text(char* out, unsigned capacity);

/* ---- DHCPv6 (dhcp6.c) ---- */
void dhcp6_start(struct netdev* dev, bool stateful);
void dhcp6_receive(struct netdev* dev, const struct ip6a* src, const uint8_t* data, unsigned length);
void dhcp6_poll(uint64_t now);
unsigned dhcp6_dns(struct ip6a* out, unsigned capacity, char domains[][64], unsigned* domain_count);
void dhcp6_dev_down(struct netdev* dev);

/* ---- sockets (inet.c) ---- */
/* Datagram delivery from the IPv4/IPv6 input paths and ICMP error routing. */
struct inet_rx_info {
    struct ip6a src, dst;
    unsigned ifindex;
    uint8_t hop_limit, tclass;
    uint16_t src_port, dst_port;
    bool multicast;
};
bool inet_udp_receive(const struct inet_rx_info* info, const void* data, unsigned length); /* false: no socket (port unreachable) */
void inet_icmp6_error(const struct ip6a* reporter, uint8_t type, uint8_t code, uint32_t info, const uint8_t* inner, unsigned length);
void inet_raw_receive(uint8_t family, const struct inet_rx_info* info, uint8_t protocol, const uint8_t* packet, unsigned length,
                      const uint8_t* payload, unsigned payload_length);
bool inet_ping_receive(uint8_t family, const struct inet_rx_info* info, const uint8_t* message, unsigned length);
bool inet_raw_protocol(uint8_t family, uint8_t protocol);   /* a raw socket takes this protocol (no "unknown" error) */
void inet_dev_down(struct netdev* dev);
unsigned inet_proc_lines(int family, int kind, char* out, unsigned capacity);   /* /proc/net/{tcp,udp,raw}[6]; kind 1..4 */
bool inet_wireguard_port(uint16_t port, uint8_t family);                        /* wireguard.c */
bool wireguard_receive(const struct inet_rx_info* info, const void* data, unsigned length);
void wifi_poll(uint64_t now);                                                    /* wifi.c: MLME timers, beacons, scans */
bool wifi_rx_pending(void);                                                      /* frames on the virtual radio medium */
void wifi_rx_flush(void);
unsigned wifi_sim_text(char* out, unsigned capacity);                           /* /proc/sys/net/nexus/wifi_sim */
int wifi_sim_command(const char* text);
/* power2's tickless core (absent here: the 1 ms network timer polls): each poller reports its next deadline, an absolute
 * CLOCK_MONOTONIC ns, or ~0 when nothing is pending. */
void tick_register_poller(const char* name, uint64_t (*next)(uint64_t now)) __attribute__((weak));
uint64_t mc_next_event(uint64_t now);                                           /* mcast.c: IGMP/MLD report and query timers */
uint64_t wifi_next_event(uint64_t now);                                         /* wifi.c: scan dwell, auth/assoc retry, beacons, beacon loss */
void wireguard_poll(uint64_t now);                                               /* timers (netdev_poll) */
void icmp4_port_unreachable(uint32_t source, uint32_t destination, uint16_t source_port, uint16_t destination_port, const void* data, unsigned length);

/* ---- AF_PACKET (packet.c) ---- */
void packet_tap(struct netdev* dev, const uint8_t* frame, unsigned length, uint8_t pkttype, uint16_t protocol, bool outgoing);
extern unsigned packet_socket_count;
int64_t packet_call(struct registers* registers);
bool packet_descriptor(struct open_file* description);
struct memory_file* packet_mapping(struct open_file* description, uint64_t* offset, uint64_t length);
void packet_dev_down(struct netdev* dev);
void packet_poll(uint64_t now);
unsigned packet_proc_lines(char* out, unsigned capacity);

/* ---- netlink notifications (netlink.c) ---- */
#define RTNLGRP_LINK 1
#define RTNLGRP_NEIGH 3
#define RTNLGRP_IPV4_IFADDR 5
#define RTNLGRP_IPV4_ROUTE 7
#define RTNLGRP_IPV4_RULE 8
#define RTNLGRP_IPV6_IFADDR 9
#define RTNLGRP_IPV6_ROUTE 11
#define RTNLGRP_IPV6_IFINFO 12
#define RTNLGRP_IPV6_PREFIX 18
#define RTNLGRP_IPV6_RULE 19
#define RTNLGRP_ND_USEROPT 20
void rtnl_notify_link(struct netdev* dev, uint16_t type);              /* RTM_NEWLINK / RTM_DELLINK */
void rtnl_notify_addr4(struct netdev* dev, const struct netdev_v4* address, uint16_t type);
void rtnl_notify_addr6(struct netdev* dev, const struct inet6_ifaddr* address, uint16_t type);
void rtnl_notify_route(const struct route* route, uint16_t type);
void rtnl_notify_neigh(const struct neighbour* neighbour, uint16_t type);
void rtnl_notify_rule(const struct rule* rule, uint16_t type);
void rtnl_notify_prefix(struct netdev* dev, const struct ip6a* prefix, unsigned plen, uint8_t flags, uint32_t valid, uint32_t preferred);
void rtnl_notify_useropt(struct netdev* dev, const uint8_t* option, unsigned length, const struct ip6a* router);
bool netlink_admin(void);                 /* CAP_NET_ADMIN of the caller */
bool net_raw_capable(void);               /* CAP_NET_RAW */

/* ---- /proc and /sys (netproc.c) ---- */
/* /proc/sys/net nodes: native.h (procext.c delegates ids >= NETPROC_BASE to netproc.c). */
void netproc_conf_defaults(struct netdev* dev);
extern int32_t sysctl_ping_group_low, sysctl_ping_group_high, sysctl_ip_forward, sysctl_ip_default_ttl, sysctl_local_port_low,
               sysctl_local_port_high, sysctl_icmp_echo_ignore_all, sysctl_icmp_ratelimit, sysctl_icmpv6_ratelimit,
               sysctl_ip_nonlocal_bind, sysctl_ipv6_nonlocal_bind, sysctl_bindv6only, sysctl_somaxconn,
               sysctl_icmpv6_echo_ignore_all, sysctl_ip6frag_time, sysctl_ip6frag_high_thresh, sysctl_ip6frag_low_thresh,
               sysctl_rmem_max, sysctl_wmem_max, sysctl_rmem_default, sysctl_wmem_default, sysctl_tcp_syncookies,
               sysctl_udp_rmem_min, sysctl_neigh_gc_thresh3, sysctl_flowlabel_consistency, sysctl_auto_flowlabels;

/* ---- packet filter (nft.c / conntrack.c) ---- */
#define NF_INET_PRE_ROUTING 0
#define NF_INET_LOCAL_IN 1
#define NF_INET_FORWARD 2
#define NF_INET_LOCAL_OUT 3
#define NF_INET_POST_ROUTING 4
#define NF_INET_INGRESS 5
#define NF_DROP 0
#define NF_ACCEPT 1
/* Run the hook's chains (and connection tracking) over an L3 packet; NF_ACCEPT or NF_DROP. `in`/`out` are the
 * interfaces (NULL when not applicable); a reject verdict sends its ICMP/TCP reset itself. */
struct nf_packet {
    uint8_t family;                       /* NFPROTO_IPV4 2 / NFPROTO_IPV6 10 */
    uint8_t hook;
    const uint8_t* data;
    unsigned length;
    unsigned l4_offset;                   /* transport header (after extension headers), length if none */
    uint8_t l4_proto;
    bool fragment;                        /* non-first fragment: no transport header */
    struct netdev *in, *out;
    uint32_t mark;
    uint32_t socket_uid, socket_gid;
    bool has_socket;
    struct conntrack* ct;
    uint8_t ct_info;                      /* IP_CT_* */
    uint8_t pkttype;
    uint8_t ct_related_dir;               /* an ICMP error: the direction of the packet it quotes */
    bool untracked;                       /* notrack */
    bool nat_changed;                     /* NAT rewrote writable */
    uint32_t priority;                    /* meta priority */
    uint8_t* writable;                    /* the packet bytes may be rewritten here (NAT, mangling); NULL: read only */
};
bool nf_nat_active(void);                 /* nat chains or NAT bindings exist: ingress copies packets for nf_hook */
bool ip6_ingress(struct netdev* in, const uint8_t* data, unsigned length, bool local, void (*deliver)(struct netdev*, uint8_t*, unsigned));   /* forward.c */
extern uint64_t ip4_forwarded, ip6_forwarded;
void conntrack_flush_masquerade(unsigned ifindex, const struct ip6a* address);
int nf_hook(struct nf_packet* packet);
bool nf_active(void);                     /* any base chain registered: the hooks parse packets */
void nf_init(void);
void nf_poll(uint64_t now);
int64_t nfnetlink_request(void* socket, const uint8_t* message, unsigned length);
void conntrack_init(void);
void conntrack_poll(uint64_t now);
unsigned conntrack_count(void);
int conntrack_proc_text(char* out, unsigned capacity);

/* ---- memory (netmem.c): small objects from a boot-time reserve ---- */
void netmem_init(void);
void* net_alloc(unsigned size);           /* zeroed; NULL when exhausted */
void net_free(void* object, unsigned size);
uint64_t net_background_pages(void);      /* pages taken beyond the reserve */
uint64_t net_objects_live(void);

/* ---- utilities ---- */
uint16_t net_checksum_add(uint32_t sum, const void* data, unsigned length, uint32_t* out); /* raw one's-complement sum */
uint32_t net_sum(uint32_t sum, const void* data, unsigned length);
uint16_t net_fold(uint32_t sum);
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void net_log(const char* text);
void net_log_addr(const char* prefix, const struct ip6a* address, const char* suffix);
uint64_t net_now(void);
uint32_t net_random32(void);
#endif
