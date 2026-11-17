/* DHCPv4 client for eth0 (RFC 2131), driven from native_network_poll() so boot never waits for a server. Until a
 * lease arrives the static QEMU user-network configuration stays (10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3;
 * QEMU's own DHCP server offers exactly these).
 * States: SELECTING (DISCOVER, backoff 1-2-4-8 s then every 16 s), REQUESTING, BOUND, RENEWING at T1 (option 58,
 * else 0.5 x lease: REQUEST unicast to the server from the leased address), REBINDING at T2 (option 59, else 0.875 x
 * lease: broadcast), expiry (the address and its routes are removed, discovery restarts), INIT-REBOOT when the link
 * comes back with an unexpired lease (REQUEST for the old address), NAK handling in every requesting state.
 * Retransmissions in RENEWING/REBINDING wait half the time left to T2/expiry, at least 60 s (RFC 2131 4.4.5).
 * The client can be switched off for a static configuration (/proc/sys/net/nexus/dhcp, Aurora Settings): the lease
 * is released (DHCPRELEASE) and the address removed.
 * /etc/resolv.conf (proc.c) is served from the dynamic DNS data while any exists: static servers from Settings, else
 * the lease's servers, RDNSS (ndisc.c) and stateless DHCPv6 (dhcp6.c), at most 3 (glibc MAXNS), and the search
 * domains of all of them. */
#include "netdev.h"
#include "../../kernel/net.h"
#include "../../kernel/ip.h"
#include "../../kernel/udp.h"
#include "../../kernel/arp.h"
#include "../../kernel/dhcp.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

enum { STOPPED, SELECTING, REQUESTING, BOUND, RENEWING, REBINDING, INIT_REBOOT };
static unsigned state, attempts;
static bool enabled = true;
static uint32_t xid, offered, server;
static uint64_t next_send, renew_at, rebind_at, expire_at, started_ns;
static struct { uint32_t ip, mask, gateway, dns[3], server; unsigned dns_count; uint32_t seconds; char domain[64]; bool bound; } lease;
/* Static DNS (Settings): up to 3 servers of either family and a search list. */
static struct ip6a static_dns[3]; static unsigned static_dns_count; static char static_search[128];
static bool carrier_seen;

static void print_ip(uint32_t ip) { char text[16]; ip_to_string(ip, text); vga_print(text); }
static uint32_t big32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned option(uint8_t* options, unsigned at, uint8_t code, uint8_t length, const void* value) {
    if (at + 2U + length >= sizeof(((dhcp_msg_t*)0)->options)) return at; /* keep room for END */
    options[at] = code; options[at + 1] = length; memcpy(options + at + 2, value, length);
    return at + 2U + length;
}
static void send_message(uint8_t type) {
    net_device_t* device = net_get_device(0); if (!device) return;
    bool from_lease = state == RENEWING || state == REBINDING || type == DHCP_RELEASE;
    dhcp_msg_t m; memset(&m, 0, sizeof(m));
    m.op = 1; m.htype = 1; m.hlen = 6; m.xid = htonl(xid);
    uint64_t elapsed = (native_clock_ns() - started_ns) / 1000000000ULL; m.secs = htons((uint16_t)(elapsed > 0xFFFF ? 0xFFFF : elapsed));
    if (!from_lease) m.flags = htons(0x8000);   /* replies by broadcast until we own an address */
    if (from_lease) m.ciaddr = htonl(lease.ip);
    memcpy(m.chaddr, device->mac.addr, 6);
    m.cookie = htonl(DHCP_MAGIC_COOKIE);
    unsigned at = option(m.options, 0, DHCP_OPT_MSGTYPE, 1, &type);
    uint8_t client[7] = {1}; memcpy(client + 1, device->mac.addr, 6);
    at = option(m.options, at, 61, sizeof(client), client);                   /* client identifier */
    if (type == DHCP_REQUEST && (state == REQUESTING || state == INIT_REBOOT)) {
        uint32_t requested = htonl(state == INIT_REBOOT ? lease.ip : offered);
        at = option(m.options, at, DHCP_OPT_REQIP, 4, &requested);
        if (state == REQUESTING && server) { uint32_t id = htonl(server); at = option(m.options, at, DHCP_OPT_SERVERID, 4, &id); }
    }
    if (type == DHCP_RELEASE) { uint32_t id = htonl(lease.server); at = option(m.options, at, DHCP_OPT_SERVERID, 4, &id); }
    if (type != DHCP_RELEASE) {
        static const uint8_t wanted[] = {DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS, DHCP_OPT_DOMAIN, DHCP_OPT_LEASE, DHCP_OPT_SERVERID, 58, 59, 26, 119};
        at = option(m.options, at, DHCP_OPT_PARAMLIST, sizeof(wanted), wanted);
        uint16_t size = htons(1500); at = option(m.options, at, 57, 2, &size);   /* maximum message size */
        at = option(m.options, at, DHCP_OPT_HOSTNAME, 5, "nexus");
    }
    m.options[at++] = DHCP_OPT_END;
    unsigned length = (unsigned)(sizeof(m) - sizeof(m.options)) + at;
    if (length < 300) length = 300; /* BOOTP minimum; some relays drop shorter messages */
    if (state == RENEWING || type == DHCP_RELEASE) {
        udp_send_from(lease.ip, lease.server, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &m, (uint16_t)length);   /* unicast to the server */
        return;
    }
    if (state == REBINDING) { udp_send_from(lease.ip, 0xFFFFFFFFU, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &m, (uint16_t)length); return; }
    /* Before a lease the source address is 0.0.0.0 (RFC 2131 4.1). */
    net_config_t saved = *ip_get_config();
    ip_set_config(0, saved.gateway, saved.subnet_mask, saved.dns);
    udp_send(0xFFFFFFFFU, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &m, (uint16_t)length);
    ip_set_config(saved.ip, saved.gateway, saved.subnet_mask, saved.dns);
}
static void restart(uint64_t delay_ns) {
    if (!native_random(&xid, sizeof(xid))) xid = (uint32_t)native_tsc() ^ 0x4E455855U;
    state = SELECTING; attempts = 0; started_ns = native_clock_ns(); next_send = started_ns + delay_ns;
}
static void drop_lease(const char* why) {
    if (!lease.bound) return;
    vga_print("[64] DHCP "); vga_print(why); vga_print(": "); print_ip(lease.ip); vga_print(" removed\n");
    const net_config_t* c = ip_get_config();
    if (c->ip == lease.ip) ip_set_config(0, 0, 0, c->dns);
    lease.bound = false; netdev_sync_legacy();
}
void native_dhcp_start(void) { if (enabled) restart(0); else state = STOPPED; }
bool native_dhcp_enabled(void) { return enabled; }
void native_dhcp_enable(bool on) {
    if (on == enabled) return;
    enabled = on;
    if (!on) {
        if (lease.bound && (state == BOUND || state == RENEWING || state == REBINDING)) {
            extern int ip4_hold_unresolved;   /* net4.c: the release waits for the server's ARP reply */
            ip4_hold_unresolved++; send_message(DHCP_RELEASE); ip4_hold_unresolved--;
        }
        drop_lease("disabled"); state = STOPPED;
    } else restart(0);
}
void native_dhcp_poll(void) {
    if (state == STOPPED) return;
    uint64_t now = native_clock_ns();
    bool carrier = netdev_eth0 && netdev_running(netdev_eth0);
    if (!carrier) { carrier_seen = false; return; }   /* nothing goes out without a link */
    if (!carrier_seen) {
        bool first = !started_ns; carrier_seen = true;
        /* link back up: verify an unexpired lease with INIT-REBOOT (RFC 2131 3.2), else discover */
        if (!first && lease.bound && now < expire_at && (state == BOUND || state == RENEWING || state == REBINDING)) {
            state = INIT_REBOOT; attempts = 0; next_send = now; xid++; started_ns = now;
        }
    }
    if (lease.bound && now >= expire_at && (state == BOUND || state == RENEWING || state == REBINDING || state == INIT_REBOOT)) {
        drop_lease("lease expired"); restart(0);
    }
    if (state == BOUND) {
        if (now < renew_at) return;
        state = RENEWING; attempts = 0; next_send = 0; xid++; started_ns = now;
    }
    if (state == RENEWING && now >= rebind_at) { state = REBINDING; attempts = 0; next_send = 0; xid++; }
    if (now < next_send) return;
    if (state == REQUESTING && attempts >= 4) restart(0);
    if (state == INIT_REBOOT && attempts >= 3) { drop_lease("not confirmed after the link came back"); restart(0); }
    send_message(state == SELECTING ? DHCP_DISCOVER : DHCP_REQUEST);
    if (state == SELECTING && attempts == 3 && !lease.bound) {
        /* The QEMU user-network address the kernel boots with is only a guess: a network without a DHCP server
         * (IPv6-only, or a static setup) must not see a fake IPv4 address and route (glibc's AI_ADDRCONFIG would
         * report IPv4 as usable and programs would try it first). Discovery continues; a lease brings IPv4 back. */
        const net_config_t* c = ip_get_config();
        if (c->ip == 0x0A00020FU && c->gateway == 0x0A000202U) {
            vga_print("[64] DHCP: no server answered; provisional 10.0.2.15 withdrawn, still discovering\n");
            ip_set_config(0, 0, 0, c->dns); netdev_sync_legacy();
        } else { vga_print("[64] DHCP: no server answered yet; keeping "); print_ip(c->ip); vga_print(" and retrying\n"); }
    }
    if (state == RENEWING || state == REBINDING) {
        uint64_t until = state == RENEWING ? rebind_at : expire_at, left = until > now ? until - now : 0, wait = left / 2;
        if (wait < 60000000000ULL) wait = left < 60000000000ULL ? left : 60000000000ULL;
        if (wait < 1000000000ULL) wait = 1000000000ULL;
        next_send = now + wait;
    } else {
        unsigned shift = attempts < 4 ? attempts : 4; /* 1, 2, 4, 8, then every 16 seconds (randomized +/- 0.5 s) */
        next_send = now + (1000000000ULL << shift) + ndisc_random_ns(1000000000ULL) - 500000000ULL;
    }
    attempts++;
}
static void apply(const dhcp_msg_t* m, uint32_t mask, uint32_t router, const uint32_t* dns, unsigned dns_count,
                  const char* domain, uint32_t seconds, uint32_t t1, uint32_t t2) {
    uint32_t ip = ntohl(m->yiaddr);
    if (!ip || ip >= 0xE0000000U || (ip >> 24) == 127 || (ip >> 24) == 0) return;   /* never multicast/class E/loopback */
    if (!mask) mask = 0xFFFFFF00U;
    if (router >= 0xE0000000U || (router && ((router ^ ip) & mask))) router = 0;   /* the router must be on-link */
    bool changed = ip != ip_get_config()->ip || !lease.bound;
    ip_set_config(ip, router, mask, dns_count ? dns[0] : 0);
    lease.ip = ip; lease.mask = mask; lease.gateway = router; lease.dns_count = dns_count; lease.seconds = seconds; lease.server = server;
    for (unsigned i = 0; i < dns_count; i++) lease.dns[i] = dns[i];
    strcpy(lease.domain, domain); lease.bound = true;
    uint64_t now = native_clock_ns();
    if (seconds == 0xFFFFFFFFU) renew_at = rebind_at = expire_at = ~0ULL;
    else {
        if (seconds < 10) seconds = 10;
        uint64_t lease_ns = (uint64_t)seconds * 1000000000ULL;
        uint64_t t1_ns = t1 && t1 < seconds ? (uint64_t)t1 * 1000000000ULL : lease_ns / 2;
        uint64_t t2_ns = t2 && t2 < seconds && t2 > t1 ? (uint64_t)t2 * 1000000000ULL : lease_ns / 8 * 7;
        if (t2_ns <= t1_ns) t2_ns = t1_ns + (lease_ns - t1_ns) / 2;
        renew_at = now + t1_ns; rebind_at = now + t2_ns; expire_at = now + lease_ns;
    }
    bool renewal = state == RENEWING || state == REBINDING || state == INIT_REBOOT;
    state = BOUND; attempts = 0;
    netdev_sync_legacy();
    if (changed) arp_announce();
    if (renewal && !changed) { vga_print("[64] DHCP lease renewed\n"); return; }
    vga_print("[64] DHCP lease "); print_ip(ip);
    unsigned prefix = 0; for (uint32_t bits = mask; bits & 0x80000000U; bits <<= 1) prefix++;
    char number[12]; uint_to_str(prefix, number); vga_print("/"); vga_print(number);
    vga_print(" gateway "); print_ip(router);
    for (unsigned i = 0; i < dns_count; i++) { vga_print(i ? "," : " dns "); print_ip(dns[i]); }
    if (seconds != 0xFFFFFFFFU) { uint_to_str(seconds, number); vga_print(" lease "); vga_print(number); vga_print(" s"); }
    vga_print(" from "); print_ip(server); vga_print("\n");
}
void native_dhcp_receive(const void* data, uint16_t length) {
    if (state == STOPPED || state == BOUND || length < 240) return;
    dhcp_msg_t m; memset(&m, 0, sizeof(m));
    memcpy(&m, data, length < sizeof(m) ? length : sizeof(m));
    net_device_t* device = net_get_device(0);
    if (!device || m.op != 2 || ntohl(m.xid) != xid || memcmp(m.chaddr, device->mac.addr, 6) || ntohl(m.cookie) != DHCP_MAGIC_COOKIE) return;
    uint8_t type = 0; uint32_t mask = 0, router = 0, id = 0, seconds = 0xFFFFFFFFU, t1 = 0, t2 = 0, dns[3]; unsigned dns_count = 0; char domain[64] = "";
    unsigned end = length < sizeof(m) ? length - (unsigned)(sizeof(m) - sizeof(m.options)) : sizeof(m.options);
    for (unsigned at = 0; at < end; ) {
        uint8_t code = m.options[at++];
        if (code == DHCP_OPT_END) break;
        if (!code) continue;
        if (at >= end) break;
        unsigned size = m.options[at++];
        if (at + size > end) break;
        const uint8_t* value = m.options + at;
        if (code == DHCP_OPT_MSGTYPE && size >= 1) type = value[0];
        else if (code == DHCP_OPT_SUBNET && size >= 4) mask = big32(value);
        else if (code == DHCP_OPT_ROUTER && size >= 4) router = big32(value);
        else if (code == DHCP_OPT_SERVERID && size >= 4) id = big32(value);
        else if (code == DHCP_OPT_LEASE && size >= 4) seconds = big32(value);
        else if (code == 58 && size >= 4) t1 = big32(value);
        else if (code == 59 && size >= 4) t2 = big32(value);
        else if (code == DHCP_OPT_DNS) for (unsigned i = 0; i + 4 <= size && dns_count < 3; i += 4) { uint32_t s = big32(value + i); if (s && s < 0xE0000000U) dns[dns_count++] = s; }
        else if (code == DHCP_OPT_DOMAIN && size && size < sizeof(domain)) {
            unsigned n = 0;
            for (unsigned i = 0; i < size; i++) if (value[i] > ' ' && value[i] < 127) domain[n++] = (char)value[i];
            domain[n] = 0;
        }
        at += size;
    }
    if (mask) { uint32_t inverted = ~mask; if (inverted & (inverted + 1)) mask = 0; }   /* non-contiguous masks are rejected */
    if (type == DHCP_OFFER && state == SELECTING) {
        offered = ntohl(m.yiaddr); server = id ? id : ntohl(m.siaddr);
        if (!offered || offered >= 0xE0000000U) return;
        state = REQUESTING; attempts = 0; next_send = 0;
    } else if (type == DHCP_ACK && (state == REQUESTING || state == RENEWING || state == REBINDING || state == INIT_REBOOT)) {
        if (id) server = id;
        apply(&m, mask, router, dns, dns_count, domain, seconds, t1, t2);
    } else if (type == DHCP_NAK && (state == REQUESTING || state == RENEWING || state == REBINDING || state == INIT_REBOOT)) {
        vga_print("[64] DHCP NAK; discovering again\n");
        drop_lease("NAK"); restart(1000000000ULL);
    }
}
bool native_dhcp_lease(uint32_t* ip, uint32_t* server_out, uint64_t* expires_ns) {
    if (!lease.bound) return false;
    if (ip) *ip = lease.ip;
    if (server_out) *server_out = lease.server;
    if (expires_ns) *expires_ns = expire_at;
    return true;
}
unsigned native_dhcp_state(void) { return state; }

/* ---- DNS configuration ---- */
int native_dns_static(const char* text) {
    struct ip6a servers[3]; unsigned count = 0; char search[128] = "";
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n') p++;
        if (!*p) break;
        char word[96]; unsigned n = 0; while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '\n' && n < sizeof(word) - 1) word[n++] = *p++;
        word[n] = 0;
        if (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '\n') return -22;
        if (!strncmp(word, "search=", 7)) {
            if (strlen(word + 7) + strlen(search) + 2 > sizeof(search)) return -22;
            for (const char* q = word + 7; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '.' || *q == '-')) return -22;
            if (search[0]) strcat(search, " "); strcat(search, word + 7); continue;
        }
        struct ip6a address; uint32_t v4 = 0, octet = 0; unsigned dots = 0, digits = 0; bool dotted = word[0] != 0;
        for (unsigned i = 0; word[i]; i++) {
            if (word[i] == '.') { if (!digits || octet > 255) dotted = false; v4 = v4 << 8 | octet; octet = 0; digits = 0; dots++; }
            else if (word[i] >= '0' && word[i] <= '9') { octet = octet * 10 + (uint32_t)(word[i] - '0'); if (++digits > 3) dotted = false; }
            else dotted = false;
        }
        if (dotted && dots == 3 && digits && octet <= 255) {
            v4 = v4 << 8 | octet;
            if (!v4 || v4 >= 0xE0000000U) return -22;   /* 0.0.0.0, multicast, class E and broadcast are no servers */
            address = ip6a_from4(v4);
        }
        else if (!ip6a_parse(word, &address) || ip6a_multicast(&address) || ip6a_any(&address)) return -22;
        if (count == 3) return -22;
        servers[count++] = address;
    }
    memcpy(static_dns, servers, sizeof(servers)); static_dns_count = count; strcpy(static_search, search);
    return 0;
}
unsigned native_dns_static_text(char* out, unsigned capacity) {
    unsigned n = 0; out[0] = 0;
    for (unsigned i = 0; i < static_dns_count; i++) {
        char address[64]; if (ip6a_is4(&static_dns[i])) ip_to_string(ip6a_v4(&static_dns[i]), address); else ip6a_text(&static_dns[i], address);
        if (n + strlen(address) + 2 >= capacity) break;
        if (n) out[n++] = ' ';
        strcpy(out + n, address); n += strlen(address);
    }
    if (static_search[0] && n + strlen(static_search) + 10 < capacity) { if (n) out[n++] = ' '; strcpy(out + n, "search="); n += 7; strcpy(out + n, static_search); n += strlen(static_search); }
    if (n + 2 <= capacity) { out[n++] = '\n'; out[n] = 0; }
    return n;
}
static bool have_dynamic_dns(void) {
    struct ip6a v6[4]; char domains[2][64]; unsigned dc = 2;
    return static_dns_count || lease.bound || dns_servers6(v6, 4, domains, &dc) || dhcp6_dns(v6, 4, NULL, NULL);
}
/* proc.c: /etc/resolv.conf is served from the kernel's DNS data while any exists */
bool native_dhcp_bound(void) { return have_dynamic_dns(); }
static bool append(char* text, unsigned capacity, unsigned* used, const char* line) {
    unsigned n = strlen(line); if (*used + n + 1 > capacity) return false;
    memcpy(text + *used, line, n + 1); *used += n; return true;
}
/* glibc reloads resolv.conf only when its size, inode, mtime or ctime change, so the generated file carries the
 * wall-clock time its contents last changed (as a network manager rewriting it would) */
uint64_t native_dhcp_resolv_stamp(unsigned* length) {
    static uint32_t last; static uint64_t stamp;
    char text[512]; unsigned n = native_dhcp_resolv(text, sizeof(text));
    uint32_t hash = 2166136261U; for (unsigned i = 0; i < n; i++) hash = (hash ^ (uint8_t)text[i]) * 16777619U;
    if (!stamp || hash != last) { last = hash; stamp = native_realtime_ns(); if (!stamp) stamp = 1; }
    if (length) *length = n;
    return stamp;
}
unsigned native_dhcp_resolv(char* text, unsigned capacity) {
    unsigned used = 0; if (capacity) text[0] = 0;
    append(text, capacity, &used, "# Generated by the NexusOS kernel (DHCP, router advertisements, DHCPv6, Settings)\n");
    struct ip6a servers[3]; unsigned count = 0; char search[256] = "";
    if (static_dns_count) { memcpy(servers, static_dns, sizeof(static_dns[0]) * static_dns_count); count = static_dns_count; strcpy(search, static_search); }
    else {
        for (unsigned i = 0; lease.bound && i < lease.dns_count && count < 3; i++) servers[count++] = ip6a_from4(lease.dns[i]);
        if (lease.bound && lease.domain[0]) strcpy(search, lease.domain);
        struct ip6a v6[4]; char domains[4][64]; unsigned dc = 4;
        unsigned n6 = dns_servers6(v6, 4, domains, &dc);
        for (unsigned i = 0; i < n6 && count < 3; i++) servers[count++] = v6[i];
        for (unsigned i = 0; i < dc; i++) if (strlen(search) + strlen(domains[i]) + 2 < sizeof(search)) { if (search[0]) strcat(search, " "); strcat(search, domains[i]); }
        dc = 4; n6 = dhcp6_dns(v6, 4, domains, &dc);
        for (unsigned i = 0; i < n6 && count < 3; i++) {
            bool duplicate = false; for (unsigned k = 0; k < count; k++) if (ip6a_eq(&servers[k], &v6[i])) duplicate = true;
            if (!duplicate) servers[count++] = v6[i];
        }
        for (unsigned i = 0; i < dc; i++) if (!strstr(search, domains[i]) && strlen(search) + strlen(domains[i]) + 2 < sizeof(search)) { if (search[0]) strcat(search, " "); strcat(search, domains[i]); }
    }
    for (unsigned i = 0; i < count; i++) {
        char line[96] = "nameserver ", address[64];
        if (ip6a_is4(&servers[i])) ip_to_string(ip6a_v4(&servers[i]), address);
        else {
            ip6a_text(&servers[i], address);
            if (ip6a_linklocal(&servers[i]) && netdev_eth0) { strcat(address, "%"); strcat(address, netdev_eth0->name); }
        }
        strcat(line, address); strcat(line, "\n");
        if (!append(text, capacity, &used, line)) break;
    }
    if (search[0]) { char line[300] = "search "; strcat(line, search); strcat(line, "\n"); append(text, capacity, &used, line); }
    append(text, capacity, &used, "options timeout:2 attempts:2\n");
    return used;
}
