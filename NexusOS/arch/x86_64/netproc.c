/* /proc/sys/net: the network sysctls (core, ipv4, ipv6, per-interface ipv4/ipv6 conf directories with "all" and
 * "default", neighbour parameters), readable by everyone and writable with CAP_NET_ADMIN (in the initial user
 * namespace), with Linux value ranges; procext.c resolves paths below /proc and delegates nodes >= NETPROC_BASE
 * here. The /proc/net text files live in netlink.c (netif_proc_text). */
#include "netdev.h"
#include "../../kernel/string.h"

int32_t sysctl_ping_group_low = 0, sysctl_ping_group_high = 2147483647;   /* systemd's default (50-default.conf) */
int32_t sysctl_ip_forward = 0, sysctl_ip_default_ttl = 64, sysctl_local_port_low = 32768, sysctl_local_port_high = 60999;
int32_t sysctl_icmp_echo_ignore_all = 0, sysctl_icmp_ratelimit = 1000, sysctl_icmpv6_ratelimit = 1000, sysctl_ip_nonlocal_bind = 0;
int32_t sysctl_ipv6_nonlocal_bind = 0, sysctl_bindv6only = 0, sysctl_somaxconn = 4096, sysctl_icmpv6_echo_ignore_all = 0;
int32_t sysctl_ip6frag_time = 60, sysctl_ip6frag_high_thresh = 262144, sysctl_ip6frag_low_thresh = 196608;
int32_t sysctl_rmem_max = 212992, sysctl_wmem_max = 212992, sysctl_rmem_default = 212992, sysctl_wmem_default = 212992;
int32_t sysctl_tcp_syncookies = 1, sysctl_udp_rmem_min = 4096, sysctl_neigh_gc_thresh3 = 1024, sysctl_flowlabel_consistency = 1;
int32_t sysctl_auto_flowlabels = 1;
static int32_t ip_dynaddr, tcp_fin_timeout = 60, tcp_keepalive_time = 7200, tcp_keepalive_intvl = 75, tcp_keepalive_probes = 9,
               tcp_timestamps = 1, tcp_sack = 1, tcp_window_scaling = 1, tcp_ecn = 2, tcp_fastopen = 1, tcp_mtu_probing,
               ip_no_pmtu_disc, icmp_echo_ignore_broadcasts = 1, tcp_rmem[3] = {4096, 131072, 6291456},
               tcp_wmem[3] = {4096, 16384, 4194304}, udp_mem[3] = {93000, 124000, 186000}, netdev_max_backlog = 1000,
               optmem_max = 20480, ip6_route_max_size = 2147483647, ip6_gc_thresh = 1024, src_valid_mark_all;

/* Node ids: 1.. static directories and files (table below); per-interface entries
 * 0x100000 | family-bit(0x80000 ipv6) | interface slot << 7 | attribute (0: the directory itself). */
enum { D_NET = 1, D_CORE, D_IPV4, D_IPV6, D_IPV4_CONF, D_IPV6_CONF, D_IPV6_ICMP, D_IPV4_NEIGH, D_IPV6_NEIGH, D_NEXUS, D_STATIC_END };
static const struct { const char* path; int parent; int32_t* value; unsigned count; int32_t low, high; } files[] = {
    {"core/somaxconn", D_CORE, &sysctl_somaxconn, 1, 0, 2147483647},
    {"core/rmem_max", D_CORE, &sysctl_rmem_max, 1, 4096, 2147483647}, {"core/wmem_max", D_CORE, &sysctl_wmem_max, 1, 4096, 2147483647},
    {"core/rmem_default", D_CORE, &sysctl_rmem_default, 1, 4096, 2147483647}, {"core/wmem_default", D_CORE, &sysctl_wmem_default, 1, 4096, 2147483647},
    {"core/netdev_max_backlog", D_CORE, &netdev_max_backlog, 1, 0, 2147483647}, {"core/optmem_max", D_CORE, &optmem_max, 1, 0, 2147483647},
    {"ipv4/ping_group_range", D_IPV4, &sysctl_ping_group_low, 2, 0, 2147483647},
    {"ipv4/ip_forward", D_IPV4, &sysctl_ip_forward, 1, 0, 1},   /* routing between interfaces (forward.c) */
    {"ipv4/ip_default_ttl", D_IPV4, &sysctl_ip_default_ttl, 1, 1, 255},
    {"ipv4/ip_local_port_range", D_IPV4, &sysctl_local_port_low, 2, 1, 65535},
    {"ipv4/ip_nonlocal_bind", D_IPV4, &sysctl_ip_nonlocal_bind, 1, 0, 1}, {"ipv4/ip_dynaddr", D_IPV4, &ip_dynaddr, 1, 0, 2},
    {"ipv4/ip_no_pmtu_disc", D_IPV4, &ip_no_pmtu_disc, 1, 0, 3},
    {"ipv4/icmp_echo_ignore_all", D_IPV4, &sysctl_icmp_echo_ignore_all, 1, 0, 1},
    {"ipv4/icmp_echo_ignore_broadcasts", D_IPV4, &icmp_echo_ignore_broadcasts, 1, 0, 1},
    {"ipv4/icmp_ratelimit", D_IPV4, &sysctl_icmp_ratelimit, 1, 0, 2147483647},
    {"ipv4/tcp_syncookies", D_IPV4, &sysctl_tcp_syncookies, 1, 0, 2}, {"ipv4/tcp_fin_timeout", D_IPV4, &tcp_fin_timeout, 1, 1, 2147483647},
    {"ipv4/tcp_keepalive_time", D_IPV4, &tcp_keepalive_time, 1, 1, 32767}, {"ipv4/tcp_keepalive_intvl", D_IPV4, &tcp_keepalive_intvl, 1, 1, 32767},
    {"ipv4/tcp_keepalive_probes", D_IPV4, &tcp_keepalive_probes, 1, 1, 127}, {"ipv4/tcp_timestamps", D_IPV4, &tcp_timestamps, 1, 0, 0},
    {"ipv4/tcp_sack", D_IPV4, &tcp_sack, 1, 0, 0}, {"ipv4/tcp_window_scaling", D_IPV4, &tcp_window_scaling, 1, 1, 1},
    {"ipv4/tcp_ecn", D_IPV4, &tcp_ecn, 1, 0, 2}, {"ipv4/tcp_fastopen", D_IPV4, &tcp_fastopen, 1, 0, 1023},
    {"ipv4/tcp_mtu_probing", D_IPV4, &tcp_mtu_probing, 1, 0, 2},
    {"ipv4/tcp_rmem", D_IPV4, tcp_rmem, 3, 1, 2147483647}, {"ipv4/tcp_wmem", D_IPV4, tcp_wmem, 3, 1, 2147483647},
    {"ipv4/udp_mem", D_IPV4, udp_mem, 3, 1, 2147483647}, {"ipv4/udp_rmem_min", D_IPV4, &sysctl_udp_rmem_min, 1, 1, 2147483647},
    {"ipv6/bindv6only", D_IPV6, &sysctl_bindv6only, 1, 0, 1}, {"ipv6/ip_nonlocal_bind", D_IPV6, &sysctl_ipv6_nonlocal_bind, 1, 0, 1},
    {"ipv6/ip6frag_time", D_IPV6, &sysctl_ip6frag_time, 1, 1, 3600}, {"ipv6/ip6frag_high_thresh", D_IPV6, &sysctl_ip6frag_high_thresh, 1, 0, 262144},
    {"ipv6/ip6frag_low_thresh", D_IPV6, &sysctl_ip6frag_low_thresh, 1, 0, 262144},
    {"ipv6/flowlabel_consistency", D_IPV6, &sysctl_flowlabel_consistency, 1, 0, 1}, {"ipv6/auto_flowlabels", D_IPV6, &sysctl_auto_flowlabels, 1, 0, 3},
    {"ipv6/icmp/ratelimit", D_IPV6_ICMP, &sysctl_icmpv6_ratelimit, 1, 0, 2147483647},
    {"ipv6/icmp/echo_ignore_all", D_IPV6_ICMP, &sysctl_icmpv6_echo_ignore_all, 1, 0, 1},
    {"ipv6/route/max_size", D_IPV6, &ip6_route_max_size, 1, 0, 2147483647}, {"ipv6/route/gc_thresh", D_IPV6, &ip6_gc_thresh, 1, 0, 2147483647},
    {"ipv4/neigh/default/gc_thresh3", D_IPV4_NEIGH, &sysctl_neigh_gc_thresh3, 1, 0, 2147483647},
    {"ipv6/neigh/default/gc_thresh3", D_IPV6_NEIGH, &sysctl_neigh_gc_thresh3, 1, 0, 2147483647},
};
#define FILES (sizeof(files) / sizeof(files[0]))
static const char* const directories[] = {"", "", "core", "ipv4", "ipv6", "ipv4/conf", "ipv6/conf", "ipv6/icmp", "ipv4/neigh", "ipv6/neigh", "nexus"};
static const int parents[] = {0, 0, D_NET, D_NET, D_NET, D_IPV4, D_IPV6, D_IPV6, D_IPV4, D_IPV6, D_NET};
#define FILE_BASE 0x1000U
/* NexusOS network-manager knobs (Aurora Settings): nexus/dhcp (eth0's DHCPv4 client 0/1), nexus/dns (static DNS
 * servers and "search=" domains, empty: the dynamic ones) */
#define NEXUS_DHCP (FILE_BASE + 0x800U)
#define NEXUS_DNS (FILE_BASE + 0x801U)
#define NEXUS_OWNER_RAW (FILE_BASE + 0x802U)   /* the desktop owner's CAP_NET_RAW (netlink.c) */
#define NEXUS_WIFI_SIM (FILE_BASE + 0x803U)    /* virtual Wi-Fi radios (wifi.c): "add N", "signal wlanN DBM", "range wlanN on|off", "del phyN" */
int32_t sysctl_owner_raw = 1;

/* ---- per-interface conf ---- */
struct conf_entry { const char* name; unsigned offset; int32_t low, high; bool readonly; };
#define C6(field, lo, hi) {#field, __builtin_offsetof(struct ipv6_conf, field), lo, hi, false}
static const struct conf_entry conf6_entries[] = {
    C6(forwarding, 0, 1), C6(hop_limit, 1, 255), C6(mtu, 1280, 65535), C6(accept_ra, 0, 2), C6(accept_redirects, 0, 1), C6(autoconf, 0, 1),
    C6(dad_transmits, 0, 64), C6(router_solicitations, -1, 2147483647), C6(router_solicitation_interval, 1, 2147483647),
    C6(router_solicitation_delay, 0, 2147483647), C6(router_solicitation_max_interval, 1, 2147483647), C6(use_tempaddr, -1, 2),
    C6(temp_valid_lft, 0, 2147483647), C6(temp_prefered_lft, 0, 2147483647), C6(regen_max_retry, 0, 2147483647),
    C6(max_desync_factor, 0, 2147483647), C6(max_addresses, 0, 64), C6(accept_ra_defrtr, 0, 1), C6(accept_ra_pinfo, 0, 1),
    C6(accept_ra_mtu, 0, 1), C6(accept_ra_min_hop_limit, 0, 255), C6(accept_source_route, -1, 0), C6(accept_dad, -1, 2),
    C6(disable_ipv6, 0, 1), C6(addr_gen_mode, 0, 3), C6(force_mld_version, 0, 2), C6(mldv1_unsolicited_report_interval, 0, 2147483647),
    C6(mldv2_unsolicited_report_interval, 0, 2147483647), C6(keep_addr_on_down, -1, 1), C6(ndisc_notify, 0, 1),
    C6(drop_unsolicited_na, 0, 1), C6(accept_ra_rt_info_max_plen, 0, 0), C6(proxy_ndp, 0, 0), C6(ignore_routes_with_linkdown, 0, 1),
    C6(enhanced_dad, 0, 1), C6(optimistic_dad, 0, 0), C6(use_optimistic, 0, 0), C6(suppress_frag_ndisc, 0, 1),
    C6(accept_ra_from_local, 0, 1), C6(drop_unicast_in_l2_multicast, 0, 1), C6(ndisc_tclass, 0, 255), C6(accept_untracked_na, 0, 2),
    {"stable_secret", 0, 0, 0, false},
};
#define CONF6 (sizeof(conf6_entries) / sizeof(conf6_entries[0]))
#define C4(name, field, lo, hi) {name, __builtin_offsetof(struct netdev, field), lo, hi, false}
static const struct conf_entry conf4_entries[] = {
    C4("forwarding", ipv4_forwarding, 0, 1), C4("rp_filter", rp_filter, 0, 2), C4("src_valid_mark", src_valid_mark, 0, 1),
    C4("accept_local", accept_local, 0, 1), C4("arp_ignore", arp_ignore, 0, 8),
};
#define CONF4 (sizeof(conf4_entries) / sizeof(conf4_entries[0]))
static struct ipv6_conf conf6_all, conf6_default;
static bool conf_ready;
static struct { int32_t ipv4_forwarding, rp_filter, src_valid_mark, accept_local, arp_ignore; } conf4_all, conf4_default;
static void conf_init(void) {
    if (conf_ready) return;
    conf_ready = true;
    if (netdev_eth0) conf6_default = netdev_eth0->conf6; else if (netdev_lo) { conf6_default = netdev_lo->conf6; conf6_default.accept_ra = 1; conf6_default.autoconf = 1; conf6_default.dad_transmits = 1; conf6_default.accept_dad = 1; }
    conf6_all = conf6_default; conf6_all.use_tempaddr = 0;
}
/* Interface slots: 1 = all, 2 = default, ifindex + 2 for interfaces (up to 4093). */
static unsigned slot_of(const char* name) {
    if (!strcmp(name, "all")) return 1;
    if (!strcmp(name, "default")) return 2;
    struct netdev* d = netdev_by_name(name);
    return d && d->index + 2 < 4096 ? d->index + 2 : 0;
}
static bool slot_name(unsigned slot, char* out) {
    if (slot == 1) { strcpy(out, "all"); return true; }
    if (slot == 2) { strcpy(out, "default"); return true; }
    struct netdev* d = slot > 2 ? netdev_by_index(slot - 2) : NULL; if (!d) return false;
    strcpy(out, d->name); return true;
}
static unsigned conf_id(bool v6, unsigned slot, unsigned attribute) { return 0x100000U | (v6 ? 0x80000U : 0) | slot << 7 | attribute; }
static bool conf_decode(unsigned id, bool* v6, unsigned* slot, unsigned* attribute) {
    if (!(id & 0x100000U)) return false;
    *v6 = (id & 0x80000U) != 0; *slot = (id >> 7) & 0xFFF; *attribute = id & 0x7F;
    if (*slot == 0) return false;
    if (*attribute > (*v6 ? CONF6 : CONF4)) return false;
    char name[IFNAMSIZ]; return slot_name(*slot, name);
}
int netproc_lookup(const char* path) {
    if (strncmp(path, "sys/net", 7) || (path[7] && path[7] != '/')) return -1;
    const char* p = path[7] ? path + 8 : path + 7;
    if (!*p) return D_NET;
    for (unsigned i = D_CORE; i < D_STATIC_END; i++) if (!strcmp(p, directories[i])) return (int)i;
    for (unsigned i = 0; i < FILES; i++) if (!strcmp(p, files[i].path)) return (int)(FILE_BASE + i);
    if (!strcmp(p, "nexus/dhcp")) return (int)NEXUS_DHCP;
    if (!strcmp(p, "nexus/dns")) return (int)NEXUS_DNS;
    if (!strcmp(p, "nexus/owner_raw")) return (int)NEXUS_OWNER_RAW;
    if (!strcmp(p, "nexus/wifi_sim")) return (int)NEXUS_WIFI_SIM;
    bool v6 = !strncmp(p, "ipv6/conf/", 10);
    if (!v6 && strncmp(p, "ipv4/conf/", 10)) return -1;
    p += 10;
    char name[IFNAMSIZ + 1]; unsigned n = 0;
    while (*p && *p != '/' && n < IFNAMSIZ) name[n++] = *p++;
    name[n] = 0; if (*p && *p != '/') return -1;
    unsigned slot = slot_of(name); if (!slot) return -1;
    if (!*p) return (int)conf_id(v6, slot, 0);
    p++;
    for (unsigned i = 0; i < (v6 ? CONF6 : CONF4); i++)
        if (!strcmp(p, v6 ? conf6_entries[i].name : conf4_entries[i].name)) return (int)conf_id(v6, slot, i + 1);
    return -1;
}
bool netproc_directory(unsigned id) {
    if (id < D_STATIC_END) return true;
    bool v6; unsigned slot, attribute;
    return conf_decode(id, &v6, &slot, &attribute) && !attribute;
}
bool netproc_path(unsigned id, char* out) {
    strcpy(out, "sys/net");
    if (id == D_NET) return true;
    if (id < D_STATIC_END) { strcat(out, "/"); strcat(out, directories[id]); return true; }
    if (id >= FILE_BASE && id < FILE_BASE + FILES) { strcat(out, "/"); strcat(out, files[id - FILE_BASE].path); return true; }
    if (id == NEXUS_WIFI_SIM) { strcat(out, "/nexus/wifi_sim"); return true; }
    if (id == NEXUS_DHCP || id == NEXUS_DNS || id == NEXUS_OWNER_RAW) { strcat(out, id == NEXUS_DHCP ? "/nexus/dhcp" : id == NEXUS_DNS ? "/nexus/dns" : "/nexus/owner_raw"); return true; }
    bool v6; unsigned slot, attribute; char name[IFNAMSIZ];
    if (!conf_decode(id, &v6, &slot, &attribute) || !slot_name(slot, name)) return false;
    strcat(out, v6 ? "/ipv6/conf/" : "/ipv4/conf/"); strcat(out, name);
    if (attribute) { strcat(out, "/"); strcat(out, v6 ? conf6_entries[attribute - 1].name : conf4_entries[attribute - 1].name); }
    return true;
}
/* Children of directory `id` (0: none - /proc/sys gets "net" from procext.c). */
int netproc_child(unsigned id, uint64_t position, char* name, bool* directory, unsigned* child) {
    uint64_t seen = 0;
    if (id < D_STATIC_END) {
        for (unsigned i = D_CORE; i < D_STATIC_END; i++) {
            if ((unsigned)parents[i] != id) continue;
            if (seen++ == position) { const char* leaf = directories[i]; const char* slash = strstr(leaf, "/"); strcpy(name, slash ? slash + 1 : leaf); *directory = true; *child = i; return 1; }
        }
        for (unsigned i = 0; i < FILES; i++) {
            if ((unsigned)files[i].parent != id) continue;
            if (seen++ == position) { const char* leaf = files[i].path; const char* last = leaf; for (const char* q = leaf; *q; q++) if (*q == '/') last = q + 1; strcpy(name, last); *directory = false; *child = FILE_BASE + i; return 1; }
        }
        if (id == D_NEXUS) {
            if (seen++ == position) { strcpy(name, "dhcp"); *directory = false; *child = NEXUS_DHCP; return 1; }
            if (seen++ == position) { strcpy(name, "dns"); *directory = false; *child = NEXUS_DNS; return 1; }
            if (seen++ == position) { strcpy(name, "owner_raw"); *directory = false; *child = NEXUS_OWNER_RAW; return 1; }
            if (seen++ == position) { strcpy(name, "wifi_sim"); *directory = false; *child = NEXUS_WIFI_SIM; return 1; }
        }
        if (id == D_IPV4_CONF || id == D_IPV6_CONF) {
            bool v6 = id == D_IPV6_CONF;
            for (unsigned slot = 1; slot < 3; slot++) if (seen++ == position) { slot_name(slot, name); *directory = true; *child = conf_id(v6, slot, 0); return 1; }
            for (struct netdev* d = netdevs; d; d = d->next) if (d->index + 2 < 4096 && seen++ == position) { strcpy(name, d->name); *directory = true; *child = conf_id(v6, d->index + 2, 0); return 1; }
        }
        return 0;
    }
    bool v6; unsigned slot, attribute;
    if (!conf_decode(id, &v6, &slot, &attribute) || attribute) return 0;
    if (position >= (v6 ? CONF6 : CONF4)) return 0;
    strcpy(name, v6 ? conf6_entries[position].name : conf4_entries[position].name); *directory = false;
    *child = conf_id(v6, slot, (unsigned)position + 1); return 1;
}
uint32_t netproc_mode(unsigned id) {
    if (netproc_directory(id)) return 040555;
    bool v6; unsigned slot, attribute;
    if (conf_decode(id, &v6, &slot, &attribute) && v6 && attribute && !strcmp(conf6_entries[attribute - 1].name, "stable_secret")) return 0100600;
    return 0100644;
}
static struct ipv6_conf* conf6_for(unsigned slot) {
    conf_init();
    if (slot == 1) return &conf6_all;
    if (slot == 2) return &conf6_default;
    struct netdev* d = netdev_by_index(slot - 2); return d ? &d->conf6 : NULL;
}
static int32_t* conf4_for(unsigned slot, unsigned attribute) {
    conf_init();
    const struct conf_entry* e = &conf4_entries[attribute - 1];
    if (slot <= 2) {
        int32_t* base = slot == 1 ? &conf4_all.ipv4_forwarding : &conf4_default.ipv4_forwarding;
        return base + (e->offset - __builtin_offsetof(struct netdev, ipv4_forwarding)) / 4;
    }
    struct netdev* d = netdev_by_index(slot - 2); return d ? (int32_t*)((uint8_t*)d + e->offset) : NULL;
}
static unsigned format(int32_t* values, unsigned count, char* out) {
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++) {
        int32_t v = values[i]; if (i) out[n++] = '\t';
        if (v < 0) { out[n++] = '-'; v = -v; }
        char digits[12]; uint_to_str((uint32_t)v, digits); strcpy(out + n, digits); n += strlen(digits);
    }
    out[n++] = '\n'; out[n] = 0; return n;
}
int netproc_text(unsigned id, char* out, unsigned capacity) {
    if (id == NEXUS_DHCP) { int32_t on = native_dhcp_enabled() ? 1 : 0; return (int)format(&on, 1, out); }
    if (id == NEXUS_DNS) return (int)native_dns_static_text(out, capacity);
    if (id == NEXUS_OWNER_RAW) return (int)format(&sysctl_owner_raw, 1, out);
    if (id == NEXUS_WIFI_SIM) return (int)wifi_sim_text(out, capacity);
    if (id >= FILE_BASE && id < FILE_BASE + FILES) return (int)format(files[id - FILE_BASE].value, files[id - FILE_BASE].count, out);
    bool v6; unsigned slot, attribute;
    if (!conf_decode(id, &v6, &slot, &attribute) || !attribute) return -21;
    if (!v6) { int32_t* value = conf4_for(slot, attribute); if (!value) return -2; return (int)format(value, 1, out); }
    struct ipv6_conf* c = conf6_for(slot); if (!c) return -2;
    const struct conf_entry* e = &conf6_entries[attribute - 1];
    if (!strcmp(e->name, "stable_secret")) {
        if (!c->stable_secret_set) return -5;   /* EIO until set, as Linux */
        unsigned n = ip6a_text(&c->stable_secret, out); out[n++] = '\n'; out[n] = 0; return (int)n;
    }
    return (int)format((int32_t*)((uint8_t*)c + e->offset), 1, out);
}
int64_t netproc_read(unsigned id, uint64_t address, uint64_t count, uint64_t offset) {
    static char text[2048]; int length = netproc_text(id, text, sizeof(text));   /* (BKL; nexus/wifi_sim lists radios) */
    if (length < 0) return length;
    if (offset >= (uint64_t)length) return 0;
    uint64_t n = (uint64_t)length - offset; if (n > count) n = count;
    if (n && !accessible(address, n, true)) return -14;
    if (n && copy_to_user((void*)address, text + offset, n)) return -14;
    return (int64_t)n;
}
static bool parse_numbers(const char* text, int32_t* out, unsigned count) {
    const char* p = text;
    for (unsigned i = 0; i < count; i++) {
        while (*p == ' ' || *p == '\t') p++;
        bool negative = *p == '-'; if (negative) p++;
        if (*p < '0' || *p > '9') return false;
        int64_t v = 0; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); if (v > 2147483648LL) return false; }
        if (negative) v = -v;
        if (v > 2147483647LL || v < -2147483647LL - 1) return false;
        out[i] = (int32_t)v;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return *p == 0;
}
static void apply6(struct netdev* d, const char* name, struct ipv6_conf* c) {
    if (!strcmp(name, "disable_ipv6")) {
        if (c->disable_ipv6) { addrconf_flush_dev(d, true); route_flush_dev(d, AF_INET6); }
        else if (netdev_running(d)) ndisc_dev_up(d);
    } else if (!strcmp(name, "mtu")) { if ((unsigned)c->mtu <= d->mtu) d->mtu6 = (unsigned)c->mtu; else c->mtu = (int32_t)d->mtu6; }
    else if (!strcmp(name, "accept_ra") && c->accept_ra <= 0) ndisc_forget_ra(d);
}
int64_t netproc_write(unsigned id, uint64_t address, uint64_t count) {
    if (!netlink_admin()) return -1;
    if (!count) return 0;
    if (count > 240) return -22;
    if (!accessible(address, count, false)) return -14;
    char text[256]; copy_from_user(text, (void*)address, count); text[count] = 0;
    for (unsigned i = 0; i < count; i++) if (!text[i]) return -22;
    if (id == NEXUS_DHCP) { int32_t on; if (!parse_numbers(text, &on, 1) || on < 0 || on > 1) return -22; native_dhcp_enable(on != 0); return (int64_t)count; }
    if (id == NEXUS_DNS) { int error = native_dns_static(text); return error ? error : (int64_t)count; }
    if (id == NEXUS_OWNER_RAW) { int32_t on; if (!parse_numbers(text, &on, 1) || on < 0 || on > 1) return -22; sysctl_owner_raw = on; return (int64_t)count; }
    if (id == NEXUS_WIFI_SIM) { int error = wifi_sim_command(text); return error ? error : (int64_t)count; }
    if (id >= FILE_BASE && id < FILE_BASE + FILES) {
        unsigned i = id - FILE_BASE; int32_t values[3];
        if (!parse_numbers(text, values, files[i].count)) return -22;
        for (unsigned k = 0; k < files[i].count; k++) if (values[k] < files[i].low || values[k] > files[i].high) return -22;
        if (files[i].count == 2 && values[0] > values[1] && files[i].value != &sysctl_ping_group_low) return -22;
        memcpy(files[i].value, values, files[i].count * 4);
        if (files[i].value == &sysctl_ip_forward) {   /* Linux: ip_forward sets conf/all, conf/default and every interface */
            conf4_all.ipv4_forwarding = conf4_default.ipv4_forwarding = values[0];
            for (struct netdev* d = netdevs; d; d = d->next) if (d->type != ARPHRD_LOOPBACK) d->ipv4_forwarding = values[0];
        }
        return (int64_t)count;
    }
    bool v6; unsigned slot, attribute;
    if (!conf_decode(id, &v6, &slot, &attribute) || !attribute) return -21;
    if (!v6) {
        int32_t value; const struct conf_entry* e = &conf4_entries[attribute - 1];
        if (!parse_numbers(text, &value, 1) || value < e->low || value > e->high) return -22;
        if (slot == 1) {
            if (!strcmp(e->name, "src_valid_mark")) src_valid_mark_all = value;
            if (!strcmp(e->name, "forwarding")) sysctl_ip_forward = value;   /* conf/all/forwarding is ip_forward */
            for (struct netdev* d = netdevs; d; d = d->next) if (d->type != ARPHRD_LOOPBACK || strcmp(e->name, "forwarding")) *(int32_t*)((uint8_t*)d + e->offset) = value;
        }
        int32_t* target = conf4_for(slot, attribute); if (!target) return -2;
        *target = value; return (int64_t)count;
    }
    const struct conf_entry* e = &conf6_entries[attribute - 1];
    struct ipv6_conf* c = conf6_for(slot); if (!c) return -2;
    if (!strcmp(e->name, "stable_secret")) {
        if (slot == 1) return -5;
        for (unsigned i = 0; text[i]; i++) if (text[i] == '\n') text[i] = 0;
        struct ip6a secret; if (!ip6a_parse(text, &secret)) return -22;
        c->stable_secret = secret; c->stable_secret_set = true;
        if (slot > 2) c->addr_gen_mode = 2;   /* Linux switches the interface to stable privacy */
        return (int64_t)count;
    }
    int32_t value;
    if (!parse_numbers(text, &value, 1) || value < e->low || value > e->high) return -22;
    if (slot == 1) {   /* "all": forwarding and disable_ipv6 apply to every interface (Linux) */
        *(int32_t*)((uint8_t*)c + e->offset) = value;
        if (!strcmp(e->name, "disable_ipv6") || !strcmp(e->name, "forwarding") || !strcmp(e->name, "accept_dad") || !strcmp(e->name, "accept_ra"))
            for (struct netdev* d = netdevs; d; d = d->next) { *(int32_t*)((uint8_t*)&d->conf6 + e->offset) = value; apply6(d, e->name, &d->conf6); }
        return (int64_t)count;
    }
    *(int32_t*)((uint8_t*)c + e->offset) = value;
    if (slot > 2) apply6(netdev_by_index(slot - 2), e->name, c);
    return (int64_t)count;
}
struct ipv6_conf* netproc_conf6_all(void) { conf_init(); return &conf6_all; }   /* forward.c: net.ipv6.conf.all.forwarding */
/* New interfaces start from "default" (netdev_create keeps its own type-specific defaults for loopback). */
void netproc_conf_defaults(struct netdev* dev) {
    if (!conf_ready || dev->type == ARPHRD_LOOPBACK) return;
    int32_t hop = dev->conf6.hop_limit; (void)hop;
    struct ipv6_conf c = conf6_default; c.mtu = dev->conf6.mtu;
    if (dev->type == ARPHRD_NONE) c.use_tempaddr = 0;
    dev->conf6 = c;
    dev->rp_filter = conf4_default.rp_filter; dev->src_valid_mark = conf4_default.src_valid_mark | src_valid_mark_all;
    dev->ipv4_forwarding = conf4_default.ipv4_forwarding;
}
