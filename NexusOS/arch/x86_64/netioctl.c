/* Interface ioctls on any socket (struct ifreq: 16-byte name + 24-byte union): SIOCGIF* queries, the SIOCSIF*
 * changes ifconfig and older tools use (flags, MTU, name, hardware address, queue length, IPv4 address / netmask /
 * broadcast / peer, IPv6 address add/delete through struct in6_ifreq on AF_INET6 sockets), SIOCADDRT/SIOCDELRT
 * (struct rtentry, struct in6_rtmsg) and the ethtool subset (driver info, link state, settings, permanent
 * address). Changes need CAP_NET_ADMIN (EPERM). */
#include "netdev.h"
#include "../../kernel/ip.h"
#include "../../kernel/string.h"

static int copy_out(uint64_t address, const void* data, unsigned size) {
    if (!accessible(address, size, true)) return -14;
    return copy_to_user((void*)address, data, size) ? -14 : 0;
}
static unsigned classful(uint32_t address) { return (address >> 31) == 0 ? 8 : (address >> 30) == 2 ? 16 : (address >> 29) == 6 ? 24 : 32; }
static struct netdev_v4* primary4(struct netdev* d) {
    for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (d->v4[i].used && !d->v4[i].secondary) return &d->v4[i];
    return NULL;
}
static void sync_eth0(struct netdev* d) {
    if (d != netdev_eth0) return;
    struct netdev_v4* a = primary4(d); const net_config_t* c = ip_get_config();
    uint32_t mask = a && a->plen ? 0xFFFFFFFFU << (32 - a->plen) : 0;
    ip_set_config(a ? a->address : 0, a ? c->gateway : 0, mask, c->dns); netdev_sync_legacy();
}
static int ethtool(struct netdev* d, uint64_t data) {
    if (!accessible(data, 4, false)) return -14;
    uint32_t command; copy_from_user(&command, (void*)data, 4);
    if (command == 0x3) {   /* ETHTOOL_GDRVINFO */
        uint8_t info[196] = {0}; memcpy(info, &command, 4);
        const char* driver = d == netdev_eth0 ? "e1000e" : d->type == ARPHRD_LOOPBACK ? "loopback" : d->kind[0] ? d->kind : "nexus";
        strcpy((char*)info + 4, driver); strcpy((char*)info + 36, "NexusOS"); strcpy((char*)info + 68, "N/A");
        strcpy((char*)info + 100, d == netdev_eth0 ? "0000:00:02.0" : d->kind[0] ? d->kind : "");
        return copy_out(data, info, sizeof(info));
    }
    if (command == 0xA) { uint32_t value[2] = {command, d->carrier ? 1U : 0}; return copy_out(data, value, 8); }   /* ETHTOOL_GLINK */
    if (command == 0x1 && d->type == ARPHRD_ETHER) {   /* ETHTOOL_GSET: 1000 Mb/s full duplex twisted pair, autonegotiated */
        uint8_t cmd[44] = {0}; memcpy(cmd, &command, 4);
        uint32_t supported = 0x2F | 0x40 | 0x80, advertising = 0x2F; memcpy(cmd + 4, &supported, 4); memcpy(cmd + 8, &advertising, 4);
        uint16_t speed = d->carrier ? 1000 : 0xFFFF; memcpy(cmd + 12, &speed, 2); cmd[14] = d->carrier ? 1 : 0xFF; cmd[15] = 0; cmd[18] = 1;
        uint16_t high = d->carrier ? 0 : 0xFFFF; memcpy(cmd + 30, &high, 2);
        return copy_out(data, cmd, sizeof(cmd));
    }
    if (command == 0x20 && d->addr_len) {   /* ETHTOOL_GPERMADDR */
        if (!accessible(data, 8, true)) return -14;
        uint32_t size; copy_from_user(&size, (void*)(data + 4), 4);
        if (size < d->addr_len) return -22;
        uint32_t length = d->addr_len; copy_to_user((void*)(data + 4), &length, 4);
        return copy_out(data + 8, d->type == ARPHRD_ETHER ? d->perm_addr : d->addr, d->addr_len);
    }
    return -95;
}
static int routes_ioctl(bool v6, bool add, uint64_t argument) {
    if (!netlink_admin()) return -1;
    struct route r; memset(&r, 0, sizeof(r));
    if (v6) {   /* struct in6_rtmsg */
        uint8_t m[80]; if (!accessible(argument, sizeof(m), false)) return -14;
        copy_from_user(m, (void*)argument, sizeof(m));
        r.family = AF_INET6; memcpy(r.dst.b, m, 16); memcpy(r.gateway.b, m + 32, 16);
        uint16_t dst_len; memcpy(&dst_len, m + 52, 2); uint32_t metric; memcpy(&metric, m + 56, 4); uint32_t flags; memcpy(&flags, m + 72, 4);
        int32_t index; memcpy(&index, m + 76, 4);
        if (dst_len > 128) return -22;
        r.plen = (uint8_t)dst_len; r.metric = metric ? metric : 1024; r.oif = (unsigned)index; ip6a_mask(&r.dst, r.plen);
        if (!(flags & 2)) r.gateway = ip6a_unspecified;
    } else {    /* struct rtentry */
        uint8_t e[120]; if (!accessible(argument, sizeof(e), false)) return -14;
        copy_from_user(e, (void*)argument, sizeof(e));
        uint16_t dst_family; memcpy(&dst_family, e + 8, 2); if (dst_family != AF_INET) return -97;
        uint32_t dst, gateway, mask; memcpy(&dst, e + 12, 4); memcpy(&gateway, e + 28, 4); memcpy(&mask, e + 44, 4);
        dst = __builtin_bswap32(dst); gateway = __builtin_bswap32(gateway); mask = __builtin_bswap32(mask);
        uint16_t flags; memcpy(&flags, e + 56, 2); int16_t metric; memcpy(&metric, e + 80, 2); uint64_t device; memcpy(&device, e + 88, 8);
        if (flags & 4) mask = 0xFFFFFFFFU;
        unsigned plen = 0; while (plen < 32 && (mask & (0x80000000U >> plen))) plen++;
        r.family = AF_INET; r.plen = (uint8_t)plen; r.dst = ip6a_from4(dst & mask); r.gateway = ip6a_from4((flags & 2) ? gateway : 0);
        r.prefsrc = ip6a_from4(0); r.src = ip6a_from4(0); r.metric = metric > 0 ? (uint32_t)metric - 1 : 0;
        if (device) {
            char name[IFNAMSIZ + 1] = {0};
            if (!accessible(device, 1, false)) return -14;
            for (unsigned i = 0; i < IFNAMSIZ; i++) { if (!accessible(device + i, 1, false)) return -14; name[i] = (char)uget8(device + i); if (!name[i]) break; }
            struct netdev* d = netdev_by_name(name); if (!d) return -19;
            r.oif = d->index;
        }
    }
    r.table = RT_TABLE_MAIN; r.type = RTN_UNICAST; r.protocol = RTPROT_BOOT;
    if (!add) { r.protocol = 0; r.type = 0; return route_del(&r, true); }
    bool gateway = r.family == AF_INET ? ip6a_v4(&r.gateway) != 0 : !ip6a_any(&r.gateway);
    if (gateway) {
        struct route_result via;
        if (route_lookup(r.family, &r.gateway, NULL, r.oif, 0, &via) || (via.gateway && !(r.family == AF_INET6 && ip6a_linklocal(&r.gateway)))) return -101;
        if (!r.oif) r.oif = via.dev ? via.dev->index : 0;
    } else if (!r.oif) return -19;
    if (r.family == AF_INET && !gateway) r.scope = RT_SCOPE_LINK;
    return route_add(&r, false, true, true);
}
static int ifreq_request(int family, uint32_t cmd, uint64_t argument, uint8_t* request);
int netif_ioctl_family(int family, uint64_t command, uint64_t argument) {
    uint32_t cmd = (uint32_t)command;
    if (cmd == 0x890B || cmd == 0x890C) return routes_ioctl(family == AF_INET6, cmd == 0x890B, argument);
    if (cmd == 0x8912) {   /* SIOCGIFCONF */
        struct { int32_t length, pad; uint64_t buffer; } conf;
        if (!accessible(argument, sizeof(conf), true)) return -14;
        copy_from_user(&conf, (void*)argument, sizeof(conf));
        int32_t used = 0;
        for (struct netdev* d = netdevs; d; d = d->next)
            for (unsigned i = 0; i < NETDEV_V4_MAX; i++) {
                if (!d->v4[i].used) continue;
                if (!conf.buffer) { used += 40; continue; }
                if (used + 40 > conf.length) goto done;
                uint8_t entry[40] = {0}; memcpy(entry, d->v4[i].label, strlen(d->v4[i].label));
                uint16_t f = AF_INET; uint32_t address = __builtin_bswap32(d->v4[i].address); memcpy(entry + 16, &f, 2); memcpy(entry + 20, &address, 4);
                if (copy_out(conf.buffer + (uint64_t)used, entry, 40)) return -14;
                used += 40;
            }
    done:
        return uput32(argument, (uint32_t)used);
    }
    if ((cmd & 0xFF00) == 0x8B00) return -95;   /* wireless extensions: not a wireless device */
    if (!accessible(argument, 40, true)) return -14;
    uint8_t request[40]; copy_from_user(request, (void*)argument, 40);   /* SMAP: the ifreq is handled as a kernel copy */
    int result = ifreq_request(family, cmd, argument, request);
    if (result >= 0 && copy_to_user((void*)argument, request, 40)) result = -14;
    return result;
}
static int ifreq_request(int family, uint32_t cmd, uint64_t argument, uint8_t* request) {
    if (cmd == 0x8910) {   /* SIOCGIFNAME: by index */
        int32_t index; memcpy(&index, request + 16, 4);
        struct netdev* d = index > 0 ? netdev_by_index((unsigned)index) : NULL; if (!d) return -19;
        memset(request, 0, 16); memcpy(request, d->name, strlen(d->name)); return 0;
    }
    if (family == AF_INET6 && (cmd == 0x8916 || cmd == 0x8936)) {   /* SIOCSIFADDR / SIOCDIFFADDR with struct in6_ifreq */
        if (!netlink_admin()) return -1;
        uint8_t q[24]; if (!accessible(argument, 24, false)) return -14; memcpy(q, request, 24);
        struct ip6a address; memcpy(address.b, q, 16); uint32_t plen; memcpy(&plen, q + 16, 4); int32_t index; memcpy(&index, q + 20, 4);
        struct netdev* d = index > 0 ? netdev_by_index((unsigned)index) : NULL; if (!d) return -19;
        if (plen > 128) return -22;
        if (cmd == 0x8936) return addrconf_del(d, &address, plen, true);
        return addrconf_add(d, &address, plen, 0, LIFETIME_INFINITE, LIFETIME_INFINITE, ORIGIN_MANUAL, NULL, 0, false, NULL);
    }
    char name[IFNAMSIZ + 1]; memcpy(name, request, IFNAMSIZ); name[IFNAMSIZ] = 0;
    char* colon = NULL; for (char* p = name; *p; p++) if (*p == ':') { colon = p; break; }   /* eth0:1 aliases name the device */
    if (colon) *colon = 0;
    struct netdev* d = netdev_by_name(name);
    if (!d) return -19;
    uint8_t* data = request + 16;
    struct netdev_v4* a4 = primary4(d);
    switch (cmd) {
    case 0x8913: { int16_t flags = (int16_t)(netdev_flags(d) & 0xFFFF); memset(data, 0, 24); memcpy(data, &flags, 2); return 0; }   /* SIOCGIFFLAGS */
    case 0x8921: { int32_t mtu = (int32_t)d->mtu; memset(data, 0, 24); memcpy(data, &mtu, 4); return 0; }                         /* SIOCGIFMTU */
    case 0x8933: { int32_t index = (int32_t)d->index; memset(data, 0, 24); memcpy(data, &index, 4); return 0; }                   /* SIOCGIFINDEX */
    case 0x8942: { int32_t queue = (int32_t)d->txqlen; memset(data, 0, 24); memcpy(data, &queue, 4); return 0; }                  /* SIOCGIFTXQLEN */
    case 0x891d: { int32_t metric = 0; memset(data, 0, 24); memcpy(data, &metric, 4); return 0; }                                  /* SIOCGIFMETRIC */
    case 0x8927: { uint16_t f = d->type; memset(data, 0, 24); memcpy(data, &f, 2); memcpy(data + 2, d->addr, d->addr_len); return 0; }   /* SIOCGIFHWADDR */
    case 0x8970: { memset(data, 0, 24); return 0; }                                                                                /* SIOCGIFMAP */
    case 0x8946: { uint64_t pointer; memcpy(&pointer, data, 8); return ethtool(d, pointer); }                                      /* SIOCETHTOOL */
    case 0x8915: case 0x891b: case 0x8919: case 0x8917: {   /* SIOCGIFADDR, NETMASK, BRDADDR, DSTADDR */
        if (!a4) return -99;
        uint32_t mask = a4->plen ? 0xFFFFFFFFU << (32 - a4->plen) : 0;
        uint32_t value = cmd == 0x8915 ? a4->address : cmd == 0x891b ? mask : cmd == 0x8919 ? a4->broadcast : (a4->peer ? a4->peer : a4->address);
        if (cmd == 0x8919 && !(d->flags & IFF_BROADCAST)) return -99;
        uint16_t f = AF_INET; uint32_t address = __builtin_bswap32(value); memset(data, 0, 24); memcpy(data, &f, 2); memcpy(data + 4, &address, 4); return 0;
    }
    }
    if ((cmd >= 0x8914 && cmd <= 0x8925 && (cmd & 1) == 0) || cmd == 0x8923 || cmd == 0x8943 || cmd == 0x8916 || cmd == 0x891a ||
        cmd == 0x891c || cmd == 0x8918 || cmd == 0x8924 || cmd == 0x8922 || cmd == 0x8914) {
        if (!netlink_admin()) return -1;
        switch (cmd) {
        case 0x8914: { int16_t flags; memcpy(&flags, data, 2); return netdev_set_flags(d, (uint16_t)flags, IFF_UP | IFF_PROMISC | IFF_ALLMULTI | IFF_MULTICAST | IFF_NOARP); }   /* SIOCSIFFLAGS */
        case 0x8922: { int32_t mtu; memcpy(&mtu, data, 4); return mtu < 0 ? -22 : netdev_set_mtu(d, (unsigned)mtu); }                                                 /* SIOCSIFMTU */
        case 0x8923: { char next[IFNAMSIZ + 1]; memcpy(next, data, IFNAMSIZ); next[IFNAMSIZ] = 0; return netdev_set_name(d, next); }                                 /* SIOCSIFNAME */
        case 0x8924: return netdev_set_address(d, data + 2, 6);                                                                                                        /* SIOCSIFHWADDR */
        case 0x8943: { int32_t queue; memcpy(&queue, data, 4); if (queue < 0) return -22; d->txqlen = (unsigned)queue; rtnl_notify_link(d, 16); return 0; }          /* SIOCSIFTXQLEN */
        case 0x8916: case 0x891c: case 0x891a: case 0x8918: {   /* SIOCSIFADDR, NETMASK, BRDADDR, DSTADDR (IPv4) */
            uint16_t f; memcpy(&f, data, 2); if (f != AF_INET) return -22;
            uint32_t value; memcpy(&value, data + 4, 4); value = __builtin_bswap32(value);
            if (cmd == 0x8916) {
                if (a4) { unsigned plen = a4->plen; uint32_t old = a4->address; netdev_v4_del(d, old, plen);
                          if (value) netdev_v4_add(d, value, plen, 0, plen < 31 ? (value | ~(plen ? 0xFFFFFFFFU << (32 - plen) : 0)) : 0, colon ? (char*)request : d->name, true); }
                else if (value) { unsigned plen = classful(value); int error = netdev_v4_add(d, value, plen, 0, value | ~(plen ? 0xFFFFFFFFU << (32 - plen) : 0), d->name, true); if (error) return error; }
                sync_eth0(d); return 0;
            }
            if (!a4) return -99;
            uint32_t address = a4->address; unsigned plen = a4->plen; uint32_t peer = a4->peer, broadcast = a4->broadcast;
            if (cmd == 0x891c) { plen = 0; while (plen < 32 && (value & (0x80000000U >> plen))) plen++; if (plen < 32 && (value << plen)) return -22; broadcast = plen < 31 ? (address | ~(plen ? 0xFFFFFFFFU << (32 - plen) : 0)) : 0; }
            else if (cmd == 0x891a) broadcast = value;
            else peer = value;
            netdev_v4_del(d, a4->address, a4->plen);
            int error = netdev_v4_add(d, address, plen, peer, broadcast, d->name, true);
            sync_eth0(d); return error;
        }
        }
    }
    if (cmd >= 0x8900 && cmd < 0x8A00 && (cmd & 0xFF) >= 0x10) return -95;
    return -25;
}
int netif_ioctl(uint64_t command, uint64_t argument) { return netif_ioctl_family(AF_INET, command, argument); }
