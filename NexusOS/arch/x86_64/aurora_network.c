/* Aurora "Network" settings (an aurora-app tool: `aurora-app network`), and the boot step that applies the saved
 * settings (`aurora-app network --apply`, started once by the desktop).
 *
 * Status: every interface (link state, MAC, IPv4 and IPv6 addresses with their kind), the default routes of both
 * families, the DNS servers in use (/etc/resolv.conf as the kernel generates it) and VPN interfaces (WireGuard, TUN).
 * Settings, saved in /etc/nexus/network.conf (key=value; where the session cannot write it, e.g. the FAT32 desktop
 * disk, in /home/nexus/.aurora/network.conf, which then overrides it) and applied at once and at every boot:
 *   dhcp=0|1            the kernel's DHCPv4 client (/proc/sys/net/nexus/dhcp)
 *   static4=A.B.C.D/N G a static IPv4 address and default gateway on eth0 when DHCP is off (rtnetlink)
 *   slaac=0|1           automatic IPv6 on eth0 (accept_ra + autoconf); 0 removes the RA-made addresses, default
 *                       routes and RA/DHCPv6 DNS servers
 *   static6=A/N G       a static IPv6 address (prefix 64 if omitted) and optional default gateway (link-local is fine)
 *                       on eth0, with or without slaac; a replaced static setting (IPv4 or IPv6) is removed
 *   dns=...             static DNS servers (IPv4 and/or IPv6) and "search=" domains (/proc/sys/net/nexus/dns); empty: automatic
 *   firewall=0|1        stateful inbound filter: ct established/related, loopback, ICMPv6 (ND, PMTU) and DHCPv6
 *                       accepted, invalid and everything else dropped; outbound allowed
 *   allow_tcp=22,80     services accepted inbound while the firewall is on
 * The firewall is an nf_tables table "inet nexus" built directly over nfnetlink (no nft binary needed); its text form
 * is written to /etc/nftables.conf when that file is absent or ours, so Debian's `nft -f /etc/nftables.conf` loads the
 * same rules. At boot, an /etc/nftables.conf of the user's own is loaded with /usr/sbin/nft when nft is installed
 * (Debian's nftables.service does that); otherwise firewall=1 builds the table. Changing settings needs the desktop
 * owner's network administration rights (netlink.c netlink_admin), which the session has.
 * Wi-Fi: the "Wi-Fi" segment shows aurora_wifi.c's page (networks, password, Connect/Forget/Scan over wpa_supplicant). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../kernel/window.h"
#include "../../kernel/appui.h"
#include "../../kernel/macui.h"

#define CONF_PATH "/etc/nexus/network.conf"
#define USER_CONF "/home/nexus/.aurora/network.conf"
#define NFT_CONF "/etc/nftables.conf"
#define MARKER "# Managed by NexusOS Aurora Network settings"

struct settings { bool dhcp, firewall; char static4[64], dns[200], allow_tcp[100]; bool slaac; char static6[100]; };
static struct settings conf = {true, false, "", "", "22", true, ""};
static char status_lines[48][120]; static int status_count;
static char message[200];
static int focus = -1;                          /* 0 static4, 1 dns, 2 allow_tcp */
static uint64_t refreshed_ms;
static int win = -1;
static bool wifi_page;                          /* the Wi-Fi segment (aurora_wifi.c) */
void aurora_wifi_refresh(void); void aurora_wifi_draw(int x, int y, int w, int cy); bool aurora_wifi_click(int px, int py);
bool aurora_wifi_key(char k); void aurora_wifi_boot(void);

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }

/* ---- settings file ---- */
static void load_file(struct settings* s, const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0; char* eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
        *eq = 0; const char* v = eq + 1;
        if (!strcmp(line, "dhcp")) s->dhcp = atoi(v) != 0;
        else if (!strcmp(line, "firewall")) s->firewall = atoi(v) != 0;
        else if (!strcmp(line, "static4")) snprintf(s->static4, sizeof(s->static4), "%s", v);
        else if (!strcmp(line, "dns")) snprintf(s->dns, sizeof(s->dns), "%s", v);
        else if (!strcmp(line, "allow_tcp")) snprintf(s->allow_tcp, sizeof(s->allow_tcp), "%s", v);
        else if (!strcmp(line, "slaac")) s->slaac = atoi(v) != 0;
        else if (!strcmp(line, "static6")) snprintf(s->static6, sizeof(s->static6), "%s", v);
    }
    fclose(f);
}
/* The system file, then the desktop owner's (Aurora keeps its settings in /home/nexus/.aurora): on the FAT32 desktop
 * disk only the home folder is writable for the session, so settings saved from the app land there and win. */
static void load_settings(struct settings* s) { load_file(s, CONF_PATH); load_file(s, USER_CONF); }
static int save_file(const struct settings* s, const char* path) {
    char temporary[96]; snprintf(temporary, sizeof(temporary), "%s.new", path);
    FILE* f = fopen(temporary, "w"); if (!f) return -errno;
    fprintf(f, "%s\ndhcp=%d\nstatic4=%s\ndns=%s\nfirewall=%d\nallow_tcp=%s\nslaac=%d\nstatic6=%s\n", MARKER, s->dhcp, s->static4, s->dns,
            s->firewall, s->allow_tcp, s->slaac, s->static6);
    bool good = fflush(f) == 0 && fsync(fileno(f)) == 0; int e = good ? 0 : -errno; good = fclose(f) == 0 && good;
    if (!good || rename(temporary, path)) { if (!e) e = -errno; unlink(temporary); return e ? e : -EIO; }
    return 0;
}
static bool save_settings(const struct settings* s) {
    mkdir("/etc", 0755); mkdir("/etc/nexus", 0755);
    int e = save_file(s, CONF_PATH);
    if (!e) { unlink(USER_CONF); return true; }   /* the system file holds everything again */
    if (e == -EROFS || e == -EACCES || e == -EPERM) {
        mkdir("/home/nexus/.aurora", 0755);
        if (!(e = save_file(s, USER_CONF))) return true;
    }
    snprintf(message, sizeof(message), "Saving settings failed: %s", strerror(-e)); return false;
}
static int write_text(const char* path, const char* text) {
    int fd = open(path, O_WRONLY); if (fd < 0) return -errno;
    ssize_t n = write(fd, text, strlen(text)); int e = n < 0 ? -errno : 0; close(fd); return e;
}

/* ---- rtnetlink helpers ---- */
struct request { struct nlmsghdr h; union { struct ifaddrmsg a; struct rtmsg r; } u; char attrs[256]; };
static void add_attr(struct nlmsghdr* h, unsigned max, unsigned short type, const void* data, unsigned length) {
    struct rtattr* r = (void*)((char*)h + NLMSG_ALIGN(h->nlmsg_len));
    if (NLMSG_ALIGN(h->nlmsg_len) + RTA_SPACE(length) > max) return;
    r->rta_type = type; r->rta_len = (unsigned short)RTA_LENGTH(length); memcpy(RTA_DATA(r), data, length);
    h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + RTA_SPACE(length);
}
static int talk(int protocol, void* message_data, unsigned length) {
    /* send, then collect every reply until the kernel goes quiet: the first negative NLMSG_ERROR is the result */
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol); if (fd < 0) return -errno;
    if (send(fd, message_data, length, 0) < 0) { int e = -errno; close(fd); return e; }
    int result = 0, wait = 2000; char buffer[8192];
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, wait) <= 0) break;
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0); if (n <= 0) break;
        for (struct nlmsghdr* h = (void*)buffer; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n))
            if (h->nlmsg_type == NLMSG_ERROR) { int e = ((struct nlmsgerr*)NLMSG_DATA(h))->error; if (e && !result) result = e; }
        wait = 150;
    }
    close(fd); return result;
}
static int set_static4(const char* spec) {
    char text[64]; snprintf(text, sizeof(text), "%s", spec);
    char* gw = strchr(text, ' '); if (gw) *gw++ = 0;
    char* slash = strchr(text, '/'); int plen = 24; if (slash) { *slash = 0; plen = atoi(slash + 1); }
    struct in_addr address, gateway; if (inet_pton(AF_INET, text, &address) != 1 || plen < 1 || plen > 32) return -EINVAL;
    unsigned index = if_nametoindex("eth0"); if (!index) return -ENODEV;
    struct request q; memset(&q, 0, sizeof(q));
    q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg)); q.h.nlmsg_type = RTM_NEWADDR; q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE; q.h.nlmsg_seq = 1;
    q.u.a.ifa_family = AF_INET; q.u.a.ifa_prefixlen = (unsigned char)plen; q.u.a.ifa_index = index;
    add_attr(&q.h, sizeof(q), IFA_LOCAL, &address, 4); add_attr(&q.h, sizeof(q), IFA_ADDRESS, &address, 4);
    int e = talk(NETLINK_ROUTE, &q, q.h.nlmsg_len); if (e && e != -EEXIST) return e;
    if (gw && *gw) {
        if (inet_pton(AF_INET, gw, &gateway) != 1) return -EINVAL;
        memset(&q, 0, sizeof(q));
        q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)); q.h.nlmsg_type = RTM_NEWROUTE; q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE; q.h.nlmsg_seq = 2;
        q.u.r.rtm_family = AF_INET; q.u.r.rtm_table = RT_TABLE_MAIN; q.u.r.rtm_protocol = RTPROT_STATIC; q.u.r.rtm_scope = RT_SCOPE_UNIVERSE; q.u.r.rtm_type = RTN_UNICAST;
        add_attr(&q.h, sizeof(q), RTA_GATEWAY, &gateway, 4); add_attr(&q.h, sizeof(q), RTA_OIF, &index, 4);
        e = talk(NETLINK_ROUTE, &q, q.h.nlmsg_len); if (e && e != -EEXIST) return e;
    }
    return 0;
}
static void remove_static4(const char* spec) {   /* a replaced static IPv4 setting: its address (and so its routes) goes */
    char text[64]; snprintf(text, sizeof(text), "%s", spec);
    char* gw = strchr(text, ' '); if (gw) *gw = 0;
    char* slash = strchr(text, '/'); int plen = 24; if (slash) { *slash = 0; plen = atoi(slash + 1); }
    struct in_addr address; unsigned index = if_nametoindex("eth0");
    if (inet_pton(AF_INET, text, &address) != 1 || plen < 1 || plen > 32 || !index) return;
    struct request q; memset(&q, 0, sizeof(q));
    q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg)); q.h.nlmsg_type = RTM_DELADDR; q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK; q.h.nlmsg_seq = 6;
    q.u.a.ifa_family = AF_INET; q.u.a.ifa_prefixlen = (unsigned char)plen; q.u.a.ifa_index = index;
    add_attr(&q.h, sizeof(q), IFA_LOCAL, &address, 4);
    talk(NETLINK_ROUTE, &q, q.h.nlmsg_len);
}

/* ---- IPv6: automatic (router advertisements: SLAAC, default router, RDNSS) and/or one static address ----
 * static6 = "ADDRESS[/PREFIX] [GATEWAY]" (prefix 64 by default; a link-local gateway is fine: eth0 is the device). */
static int parse_static6(const char* spec, struct in6_addr* address, int* plen, struct in6_addr* gateway, bool* has_gateway) {
    char text[100]; snprintf(text, sizeof(text), "%s", spec);
    char* gw = strchr(text, ' '); if (gw) { *gw++ = 0; while (*gw == ' ') gw++; }
    char* slash = strchr(text, '/'); *plen = 64;
    if (slash) { *slash = 0; char* end; long n = strtol(slash + 1, &end, 10); if (*end || n < 1 || n > 128) return -EINVAL; *plen = (int)n; }
    if (inet_pton(AF_INET6, text, address) != 1 || IN6_IS_ADDR_MULTICAST(address) || IN6_IS_ADDR_UNSPECIFIED(address) ||
        IN6_IS_ADDR_LOOPBACK(address) || IN6_IS_ADDR_V4MAPPED(address) || IN6_IS_ADDR_LINKLOCAL(address)) return -EINVAL;
    *has_gateway = gw && *gw;
    if (*has_gateway && (inet_pton(AF_INET6, gw, gateway) != 1 || IN6_IS_ADDR_MULTICAST(gateway) || IN6_IS_ADDR_UNSPECIFIED(gateway))) return -EINVAL;
    return 0;
}
static int address6(bool add, unsigned index, const struct in6_addr* a, int plen) {
    struct request q; memset(&q, 0, sizeof(q));
    q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg)); q.h.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
    q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_REPLACE : 0); q.h.nlmsg_seq = 3;
    q.u.a.ifa_family = AF_INET6; q.u.a.ifa_prefixlen = (unsigned char)plen; q.u.a.ifa_index = index;
    add_attr(&q.h, sizeof(q), IFA_LOCAL, a, 16); add_attr(&q.h, sizeof(q), IFA_ADDRESS, a, 16);
    return talk(NETLINK_ROUTE, &q, q.h.nlmsg_len);
}
static int default6(bool add, unsigned index, const struct in6_addr* gateway, unsigned char protocol) {
    struct request q; memset(&q, 0, sizeof(q));
    q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)); q.h.nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
    q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_REPLACE : 0); q.h.nlmsg_seq = 4;
    q.u.r.rtm_family = AF_INET6; q.u.r.rtm_table = RT_TABLE_MAIN; q.u.r.rtm_protocol = protocol;
    q.u.r.rtm_scope = RT_SCOPE_UNIVERSE; q.u.r.rtm_type = RTN_UNICAST;
    if (gateway) add_attr(&q.h, sizeof(q), RTA_GATEWAY, gateway, 16);
    add_attr(&q.h, sizeof(q), RTA_OIF, &index, 4);
    return talk(NETLINK_ROUTE, &q, q.h.nlmsg_len);
}
/* Manual IPv6: the addresses router advertisements configured on eth0 (global, not permanent) are removed */
static void flush_autoconf6(unsigned index) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE); if (fd < 0) return;
    struct { struct nlmsghdr h; struct ifaddrmsg m; } q = {{sizeof(q), RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 5, 0}, {AF_INET6, 0, 0, 0, 0}};
    struct { struct in6_addr a; int plen; } victims[16]; int count = 0;
    if (send(fd, &q, sizeof(q), 0) == (ssize_t)sizeof(q)) {
        static char buffer[16384]; bool done = false;
        while (!done) {
            struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, 2000) <= 0) break;
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0); if (n <= 0) break;
            for (struct nlmsghdr* h = (void*)buffer; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
                if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = true; break; }
                if (h->nlmsg_type != RTM_NEWADDR) continue;
                struct ifaddrmsg* m = NLMSG_DATA(h); uint32_t flags = m->ifa_flags; struct in6_addr a; bool have = false;
                int length = (int)IFA_PAYLOAD(h);
                for (struct rtattr* r = IFA_RTA(m); RTA_OK(r, length); r = RTA_NEXT(r, length)) {
                    if (r->rta_type == IFA_ADDRESS && RTA_PAYLOAD(r) == 16) { memcpy(&a, RTA_DATA(r), 16); have = true; }
                    if (r->rta_type == IFA_FLAGS && RTA_PAYLOAD(r) == 4) memcpy(&flags, RTA_DATA(r), 4);
                }
                if (have && m->ifa_index == index && m->ifa_scope == RT_SCOPE_UNIVERSE && !(flags & IFA_F_PERMANENT) && count < 16)
                    { victims[count].a = a; victims[count].plen = m->ifa_prefixlen; count++; }
            }
        }
    }
    close(fd);
    for (int i = 0; i < count; i++) address6(false, index, &victims[i].a, victims[i].plen);
    for (int i = 0; i < 8 && default6(false, index, NULL, RTPROT_RA) == 0; i++) {}
}
static int apply_ipv6(const struct settings* s, const struct settings* old) {
    unsigned index = if_nametoindex("eth0"); if (!index) return s->static6[0] || !s->slaac ? -ENODEV : 0;
    const char* value = s->slaac ? "1\n" : "0\n";
    int e = write_text("/proc/sys/net/ipv6/conf/eth0/accept_ra", value); if (e < 0) return e;
    if ((e = write_text("/proc/sys/net/ipv6/conf/eth0/autoconf", value)) < 0) return e;
    if (!s->slaac) flush_autoconf6(index);
    struct in6_addr address, gateway, old_address, old_gateway; int plen = 64, old_plen = 64; bool has_gateway = false, old_has_gateway = false;
    if (s->static6[0] && (e = parse_static6(s->static6, &address, &plen, &gateway, &has_gateway)) < 0) return e;
    if (old && old->static6[0] && strcmp(old->static6, s->static6) &&
        !parse_static6(old->static6, &old_address, &old_plen, &old_gateway, &old_has_gateway)) {   /* the replaced setting goes */
        if (old_has_gateway) default6(false, index, &old_gateway, RTPROT_STATIC);
        if (!s->static6[0] || memcmp(&old_address, &address, 16)) address6(false, index, &old_address, old_plen);
    }
    if (!s->static6[0]) return 0;
    if ((e = address6(true, index, &address, plen)) < 0 && e != -EEXIST) return e;
    if (has_gateway && (e = default6(true, index, &gateway, RTPROT_STATIC)) < 0 && e != -EEXIST) return e;
    return 0;
}

/* ---- nf_tables over nfnetlink: the "inet nexus" table ---- */
#define NFT_BUF 16384
struct nbuf { uint8_t data[NFT_BUF]; unsigned used, nest[8], depth; uint32_t seq; };
static void put(struct nbuf* b, const void* d, unsigned n) { if (b->used + ((n + 3) & ~3U) > NFT_BUF) return; memcpy(b->data + b->used, d, n); memset(b->data + b->used + n, 0, ((n + 3) & ~3U) - n); b->used += (n + 3) & ~3U; }
static unsigned msg_begin(struct nbuf* b, uint16_t type, uint16_t flags, uint8_t family, uint16_t res) {
    unsigned at = b->used; struct nlmsghdr h = {0, type, (uint16_t)(NLM_F_REQUEST | flags), ++b->seq, 0}; put(b, &h, sizeof(h));
    uint8_t g[4] = {family, 0, (uint8_t)(res >> 8), (uint8_t)res}; put(b, g, 4); return at;
}
static void msg_end(struct nbuf* b, unsigned at) { uint32_t length = b->used - at; memcpy(b->data + at, &length, 4); }
static void attr(struct nbuf* b, uint16_t type, const void* d, unsigned n) { uint16_t h[2] = {(uint16_t)(4 + n), type}; put(b, h, 4); if (n) put(b, d, n); }
static void attr_str(struct nbuf* b, uint16_t type, const char* s) { attr(b, type, s, (unsigned)strlen(s) + 1); }
static void attr_be32(struct nbuf* b, uint16_t type, uint32_t v) { v = htonl(v); attr(b, type, &v, 4); }
static void nest(struct nbuf* b, uint16_t type) { b->nest[b->depth++] = b->used; uint16_t h[2] = {4, (uint16_t)(type | 0x8000)}; put(b, h, 4); }
static void unnest(struct nbuf* b) { unsigned at = b->nest[--b->depth]; uint16_t n = (uint16_t)(b->used - at); memcpy(b->data + at, &n, 2); }
/* expressions */
static void expr_begin(struct nbuf* b, const char* name) { nest(b, 1 /* LIST_ELEM */); attr_str(b, 1, name); nest(b, 2 /* EXPR_DATA */); }
static void expr_end(struct nbuf* b) { unnest(b); unnest(b); }
static void value(struct nbuf* b, uint16_t type, const void* d, unsigned n) { nest(b, type); attr(b, 1 /* DATA_VALUE */, d, n); unnest(b); }
static void e_meta(struct nbuf* b, uint32_t key) { expr_begin(b, "meta"); attr_be32(b, 1, 1); attr_be32(b, 2, key); expr_end(b); }   /* DREG 1, KEY */
static void e_ct_state(struct nbuf* b) { expr_begin(b, "ct"); attr_be32(b, 1, 1); attr_be32(b, 2, 0); expr_end(b); }
static void e_bitwise(struct nbuf* b, uint32_t mask) { uint32_t zero = 0; expr_begin(b, "bitwise"); attr_be32(b, 1, 1); attr_be32(b, 2, 1); attr_be32(b, 3, 4); value(b, 4, &mask, 4); value(b, 5, &zero, 4); expr_end(b); }
static void e_cmp(struct nbuf* b, uint32_t op, const void* d, unsigned n) { expr_begin(b, "cmp"); attr_be32(b, 1, 1); attr_be32(b, 2, op); value(b, 3, d, n); expr_end(b); }
static void e_payload(struct nbuf* b, uint32_t base, uint32_t offset, uint32_t length) { expr_begin(b, "payload"); attr_be32(b, 1, 1); attr_be32(b, 2, base); attr_be32(b, 3, offset); attr_be32(b, 4, length); expr_end(b); }
static void e_verdict(struct nbuf* b, int32_t code) {
    expr_begin(b, "immediate"); attr_be32(b, 1, 0); nest(b, 2); nest(b, 2 /* DATA_VERDICT */); attr_be32(b, 1, (uint32_t)code); unnest(b); unnest(b); expr_end(b);
}
static void e_counter(struct nbuf* b) { uint64_t zero = 0; expr_begin(b, "counter"); attr(b, 1, &zero, 8); attr(b, 2, &zero, 8); expr_end(b); }
static unsigned rule_begin(struct nbuf* b) { unsigned at = msg_begin(b, (10 << 8) | 6, NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK, 1, 0); attr_str(b, 1, "nexus"); attr_str(b, 2, "input"); nest(b, 4); return at; }
static void rule_end(struct nbuf* b, unsigned at) { unnest(b); msg_end(b, at); }
static int run_batch(struct nbuf* b) {
    unsigned at = msg_begin(b, 17 /* BATCH_END */, 0, 0, 10); msg_end(b, at);
    return talk(NETLINK_NETFILTER, b->data, b->used);
}
static void batch_begin(struct nbuf* b) { b->used = 0; b->depth = 0; b->seq = (uint32_t)time(NULL); unsigned at = msg_begin(b, 16 /* BATCH_BEGIN */, 0, 0, 10); msg_end(b, at); }
static int firewall_off(void) {
    static struct nbuf b; batch_begin(&b);
    unsigned at = msg_begin(&b, (10 << 8) | 2 /* DELTABLE */, NLM_F_ACK, 1, 0); attr_str(&b, 1, "nexus"); msg_end(&b, at);
    int e = run_batch(&b); return e == -ENOENT ? 0 : e;
}
static int firewall_on(const char* allow_tcp) {
    firewall_off();
    static struct nbuf b; batch_begin(&b);
    unsigned at = msg_begin(&b, (10 << 8) | 0 /* NEWTABLE */, NLM_F_CREATE | NLM_F_ACK, 1, 0); attr_str(&b, 1, "nexus"); msg_end(&b, at);
    at = msg_begin(&b, (10 << 8) | 3 /* NEWCHAIN */, NLM_F_CREATE | NLM_F_ACK, 1, 0);
    attr_str(&b, 1, "nexus"); attr_str(&b, 3, "input");
    nest(&b, 4); attr_be32(&b, 1, 1 /* NF_INET_LOCAL_IN */); attr_be32(&b, 2, 0); unnest(&b);
    attr_be32(&b, 5, 0 /* NF_DROP */); attr_str(&b, 7, "filter"); msg_end(&b, at);
    uint32_t zero = 0; uint8_t icmp6 = 58, tcp = 6, udp = 17; uint32_t lo = if_nametoindex("lo");
    at = rule_begin(&b); e_ct_state(&b); e_bitwise(&b, 1u /* invalid */); e_cmp(&b, 1 /* NEQ */, &zero, 4); e_verdict(&b, 0 /* drop */); rule_end(&b, at);
    at = rule_begin(&b); e_ct_state(&b); e_bitwise(&b, 2u | 4u /* established, related */); e_cmp(&b, 1, &zero, 4); e_verdict(&b, 1 /* accept */); rule_end(&b, at);
    at = rule_begin(&b); e_meta(&b, 4 /* IIF */); e_cmp(&b, 0, &lo, 4); e_verdict(&b, 1); rule_end(&b, at);
    at = rule_begin(&b); e_meta(&b, 16 /* L4PROTO */); e_cmp(&b, 0, &icmp6, 1); e_verdict(&b, 1); rule_end(&b, at);
    uint16_t dhcp6 = htons(546);
    at = rule_begin(&b); e_meta(&b, 16); e_cmp(&b, 0, &udp, 1); e_payload(&b, 2, 2, 2); e_cmp(&b, 0, &dhcp6, 2); e_verdict(&b, 1); rule_end(&b, at);
    char list[100]; snprintf(list, sizeof(list), "%s", allow_tcp);
    for (char* save = NULL, *t = strtok_r(list, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        long port = strtol(t, NULL, 10); if (port <= 0 || port > 65535) continue;
        uint16_t p = htons((uint16_t)port);
        at = rule_begin(&b); e_meta(&b, 16); e_cmp(&b, 0, &tcp, 1); e_payload(&b, 2, 2, 2); e_cmp(&b, 0, &p, 2); e_verdict(&b, 1); rule_end(&b, at);
    }
    at = rule_begin(&b); e_counter(&b); rule_end(&b, at);   /* what the policy drops */
    return run_batch(&b);
}
static void write_nftables_conf(const struct settings* s) {
    FILE* f = fopen(NFT_CONF, "r");
    if (f) { char head[128] = ""; if (!fgets(head, sizeof(head), f)) head[0] = 0; if (!fgets(head, sizeof(head), f)) head[0] = 0; fclose(f); if (!strstr(head, "NexusOS")) return; }   /* the user's own file */
    f = fopen(NFT_CONF, "w"); if (!f) return;
    fprintf(f, "#!/usr/sbin/nft -f\n%s\n", MARKER);
    if (!s->firewall) { fprintf(f, "# firewall off\n"); fclose(f); return; }
    fprintf(f, "table inet nexus {\n  chain input {\n    type filter hook input priority 0; policy drop;\n"
               "    ct state invalid drop\n    ct state established,related accept\n    iif \"lo\" accept\n"
               "    meta l4proto ipv6-icmp accept\n    udp dport 546 accept\n");
    char list[100]; snprintf(list, sizeof(list), "%s", s->allow_tcp);
    for (char* save = NULL, *t = strtok_r(list, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) { long port = strtol(t, NULL, 10); if (port > 0 && port <= 65535) fprintf(f, "    tcp dport %ld accept\n", port); }
    fprintf(f, "    counter\n  }\n}\n"); fclose(f);
}
static bool user_nftables_conf(void) {
    FILE* f = fopen(NFT_CONF, "r"); if (!f) return false;
    char head[128] = "", second[128] = ""; if (!fgets(head, sizeof(head), f)) head[0] = 0; if (!fgets(second, sizeof(second), f)) second[0] = 0; fclose(f);
    return !strstr(second, "NexusOS") && !strstr(head, "NexusOS");
}
static int run_nft_file(void) {
    if (access("/usr/sbin/nft", X_OK)) return -ENOENT;
    pid_t child = fork(); if (child < 0) return -errno;
    if (!child) { int null = open("/dev/null", O_RDWR); if (null >= 0) { dup2(null, 1); dup2(null, 2); } execl("/usr/sbin/nft", "nft", "-f", NFT_CONF, (char*)NULL); _exit(127); }
    int status = 0; waitpid(child, &status, 0); return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
}

/* ---- apply ---- */
static int apply(const struct settings* s, bool boot, const struct settings* old) {
    int errors = 0, e;
    if ((e = write_text("/proc/sys/net/nexus/dhcp", s->dhcp ? "1" : "0")) < 0) { errors++; snprintf(message, sizeof(message), "DHCP: %s", strerror(-e)); }
    if (!s->dhcp && s->static4[0] && (e = set_static4(s->static4)) < 0) { errors++; snprintf(message, sizeof(message), "Static IPv4 %s: %s", s->static4, strerror(-e)); }
    if (old && !old->dhcp && old->static4[0] && (s->dhcp || strcmp(old->static4, s->static4))) remove_static4(old->static4);
    if ((e = apply_ipv6(s, old)) < 0) { errors++; snprintf(message, sizeof(message), "IPv6 %s: %s", s->static6[0] ? s->static6 : "automatic", strerror(-e)); }
    if ((e = write_text("/proc/sys/net/nexus/dns", s->dns[0] ? s->dns : "\n")) < 0) { errors++; snprintf(message, sizeof(message), "DNS: %s", strerror(-e)); }
    if (boot && user_nftables_conf() && !access("/usr/sbin/nft", X_OK)) {
        if ((e = run_nft_file()) < 0) { errors++; snprintf(message, sizeof(message), "%s: nft failed", NFT_CONF); }
    } else if (s->firewall) {
        if ((e = firewall_on(s->allow_tcp)) < 0) { errors++; snprintf(message, sizeof(message), "Firewall: %s", strerror(-e)); }
    } else if (!boot) firewall_off();
    if (!boot) write_nftables_conf(s);
    if (!errors) snprintf(message, sizeof(message), "Settings applied.");
    return errors;
}
int aurora_network_apply(void) {
    struct settings s = conf; load_settings(&s);
    int errors = apply(&s, true, NULL);
    aurora_wifi_boot();                          /* saved Wi-Fi networks reconnect */
    printf("[64] NETWORK SETTINGS APPLIED dhcp=%d static4=%s dns=%s firewall=%d allow_tcp=%s slaac=%d static6=%s errors=%d %s\n",
           s.dhcp, s.static4[0] ? s.static4 : "-", s.dns[0] ? s.dns : "-", s.firewall, s.allow_tcp, s.slaac, s.static6[0] ? s.static6 : "-",
           errors, errors ? message : "");
    fflush(stdout); return errors ? 1 : 0;
}

/* ---- status ---- */
static void line(const char* format, const char* a, const char* b, const char* c) {
    if (status_count >= 48) return;
    snprintf(status_lines[status_count++], sizeof(status_lines[0]), format, a ? a : "", b ? b : "", c ? c : "");
}
static const char* kind_of(const char* name) {
    if (!strncmp(name, "wg", 2)) return "WireGuard VPN"; if (!strncmp(name, "tun", 3) || !strncmp(name, "tap", 3)) return "tunnel"; return NULL;
}
static void refresh(void) {
    status_count = 0;
    struct ifaddrs* list = NULL; char names[16][IFNAMSIZ]; int name_count = 0;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs* i = list; i; i = i->ifa_next) {
            bool seen = false; for (int k = 0; k < name_count; k++) if (!strcmp(names[k], i->ifa_name)) seen = true;
            if (!seen && name_count < 16) strcpy(names[name_count++], i->ifa_name);
        }
        for (int k = 0; k < name_count; k++) {
            if (!strcmp(names[k], "lo")) continue;
            unsigned flags = 0; for (struct ifaddrs* i = list; i; i = i->ifa_next) if (!strcmp(i->ifa_name, names[k])) flags = i->ifa_flags;
            const char* state = !(flags & IFF_UP) ? "off" : (flags & IFF_RUNNING) ? "connected" : "no link";
            const char* kind = kind_of(names[k]);
            char head[64]; snprintf(head, sizeof(head), "%s%s%s", names[k], kind ? "  " : "", kind ? kind : "");
            line("%s: %s", head, state, NULL);
            for (struct ifaddrs* i = list; i; i = i->ifa_next) {
                if (strcmp(i->ifa_name, names[k]) || !i->ifa_addr) continue;
                char text[INET6_ADDRSTRLEN] = ""; int plen = 0;
                if (i->ifa_addr->sa_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in*)i->ifa_addr)->sin_addr, text, sizeof(text));
                    if (i->ifa_netmask) { uint32_t m = ntohl(((struct sockaddr_in*)i->ifa_netmask)->sin_addr.s_addr); while (m & 0x80000000U) { plen++; m <<= 1; } }
                    char t[64]; snprintf(t, sizeof(t), "%s/%d", text, plen); line("    IPv4  %s%s", t, conf.dhcp && !strcmp(names[k], "eth0") ? "  (DHCP)" : "", NULL);
                } else if (i->ifa_addr->sa_family == AF_INET6) {
                    struct in6_addr* a = &((struct sockaddr_in6*)i->ifa_addr)->sin6_addr;
                    inet_ntop(AF_INET6, a, text, sizeof(text));
                    const char* scope = IN6_IS_ADDR_LINKLOCAL(a) ? "  link-local" : (a->s6_addr[0] & 0xFE) == 0xFC ? "  unique-local" : "  global";
                    line("    IPv6  %s%s", text, scope, NULL);
                }
            }
        }
        freeifaddrs(list);
    }
    FILE* f = fopen("/proc/net/route", "r");
    if (f) {
        char l[256]; while (fgets(l, sizeof(l), f)) { char ifn[32]; unsigned dst, gw; if (sscanf(l, "%31s %x %x", ifn, &dst, &gw) == 3 && dst == 0) {
            struct in_addr g = {gw}; char t[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &g, t, sizeof(t)); line("Default IPv4 route: via %s dev %s", t, ifn, NULL); } }
        fclose(f);
    }
    f = fopen("/proc/net/ipv6_route", "r");
    if (f) {
        char l[512]; while (fgets(l, sizeof(l), f)) {
            char dst[40], dplen[4], src[40], splen[4], next[40], ifn[32]; unsigned metric, refs, use, flags;
            if (sscanf(l, "%32s %2s %32s %2s %32s %x %x %x %x %31s", dst, dplen, src, splen, next, &metric, &refs, &use, &flags, ifn) == 10 &&
                !strcmp(dst, "00000000000000000000000000000000") && !strcmp(dplen, "00") && strcmp(ifn, "lo")) {
                struct in6_addr a; for (int i = 0; i < 16; i++) { unsigned v; sscanf(next + 2 * i, "%2x", &v); a.s6_addr[i] = (uint8_t)v; }
                char t[INET6_ADDRSTRLEN]; inet_ntop(AF_INET6, &a, t, sizeof(t)); line("Default IPv6 route: via %s dev %s", t, ifn, NULL); break;
            }
        }
        fclose(f);
    }
    f = fopen("/etc/resolv.conf", "r");
    if (f) { char l[256]; while (fgets(l, sizeof(l), f)) { l[strcspn(l, "\r\n")] = 0; if (!strncmp(l, "nameserver ", 11)) line("DNS server: %s", l + 11, NULL, NULL); else if (!strncmp(l, "search ", 7)) line("Search domains: %s", l + 7, NULL, NULL); } fclose(f); }
    refreshed_ms = now_ms();
}

/* ---- the window ---- */
#define ROW 22
#define FIELDS 4
#define TOGGLES 3
static int field_y[FIELDS], toggle_y[TOGGLES], apply_y, apply_x;
static char* field_text(int i) { return i == 0 ? conf.static4 : i == 1 ? conf.static6 : i == 2 ? conf.dns : conf.allow_tcp; }
static unsigned field_size(int i) { return i == 0 ? sizeof(conf.static4) : i == 1 ? sizeof(conf.static6) : i == 2 ? sizeof(conf.dns) : sizeof(conf.allow_tcp); }
static void draw(int id, int ccx, int ccy, int ccw, int cch) {
    (void)ccx; (void)ccy; (void)ccw; (void)cch;
    int x, y, w, h; if (!appui_content(id, &x, &y, &w, &h)) return;
    appui_px_fill(x, y, w, h, APPUI_BG);
    int cx = x + 18, cy = y + 14;
    appui_px_text(cx, cy, "Network", APPUI_F_TITLE, APPUI_TEXT);
    static const char* const segments[2] = {"Wired", "Wi-Fi"}; appui_px_segment(x + w - 200, cy, 180, 26, segments, 2, wifi_page ? 1 : 0);
    cy += 34;
    if (wifi_page) { aurora_wifi_draw(x, y, w, cy); return; }
    appui_px_card(cx - 8, cy - 6, w - 20, ROW * (status_count > 12 ? 12 : status_count) + 16, 10, APPUI_PANEL);
    for (int i = 0; i < status_count && i < 12; i++) { appui_px_text(cx, cy, status_lines[i], i && status_lines[i][0] == ' ' ? APPUI_F_SMALL : APPUI_F_BODY, status_lines[i][0] == ' ' ? APPUI_SUBTLE : APPUI_TEXT); cy += ROW; }
    cy += 24;
    const char* toggles[TOGGLES] = {"Automatic IPv4 (DHCP)", "Automatic IPv6 (router advertisements)", "Firewall (block unsolicited inbound traffic)"};
    bool values[TOGGLES] = {conf.dhcp, conf.slaac, conf.firewall};
    for (int i = 0; i < TOGGLES; i++) {
        toggle_y[i] = cy - y; appui_px_text(cx, cy + 3, toggles[i], APPUI_F_BODY, APPUI_TEXT); appui_px_toggle(x + w - 70, cy, values[i]); cy += 32;
    }
    const char* labels[FIELDS] = {"Static IPv4 (address/prefix gateway)", "Static IPv6 (address/prefix gateway)", "DNS servers, IPv4 or IPv6 (empty: automatic)",
                                  "Allowed inbound TCP ports"};
    for (int i = 0; i < FIELDS; i++) {
        appui_px_text(cx, cy, labels[i], APPUI_F_SMALL, i == 0 && conf.dhcp ? APPUI_FAINT : APPUI_SUBTLE); cy += 18;
        field_y[i] = cy - y; appui_px_field(cx, cy, w - 40, 28, field_text(i), focus == i); cy += 38;
    }
    apply_x = w - 140; apply_y = cy - y; appui_px_button(x + apply_x, cy, 110, 30, "Apply", true);
    if (message[0]) appui_px_text(cx, cy + 8, message, APPUI_F_SMALL, strstr(message, "fail") || strstr(message, ":") ? APPUI_ORANGE : APPUI_GREEN);
}
static void apply_now(void) {
    message[0] = 0;
    struct settings old = {true, false, "", "", "22", true, ""}; load_settings(&old);   /* what is replaced */
    if (save_settings(&conf)) apply(&conf, false, &old);
    refresh();
}
static void click(int id, int px, int py) {
    (void)id; int w = 0, h = 0, x, y; appui_content(win, &x, &y, &w, &h);
    if (py >= 14 && py < 40 && px >= w - 200 && px < w - 20) { wifi_page = px >= w - 110; if (wifi_page) aurora_wifi_refresh(); return; }
    if (wifi_page) { aurora_wifi_click(px, py); aurora_wifi_refresh(); return; }
    for (int i = 0; i < TOGGLES; i++) if (py >= toggle_y[i] && py < toggle_y[i] + 22 && px >= w - 70 && px < w - 30) {
        if (i == 0) conf.dhcp = !conf.dhcp; else if (i == 1) conf.slaac = !conf.slaac; else conf.firewall = !conf.firewall;
        apply_now(); return;
    }
    focus = -1;
    for (int i = 0; i < FIELDS; i++) if (py >= field_y[i] && py < field_y[i] + 28) focus = i;
    if (py >= apply_y && py < apply_y + 30 && px >= apply_x && px < apply_x + 110) apply_now();
}
static void key(int id, char k) {
    (void)id;
    if (wifi_page) { aurora_wifi_key(k); return; }
    if (k == '\t') { focus = (focus + 1) % FIELDS; return; }
    if (focus < 0) return;
    char* t = field_text(focus); size_t n = strlen(t);
    if (k == '\n') { apply_now(); return; }
    if (k == '\b' || k == 127) { if (n) t[n - 1] = 0; return; }
    if ((unsigned char)k >= 32 && (unsigned char)k < 127 && n + 1 < field_size(focus)) { t[n] = k; t[n + 1] = 0; }
}
int aurora_network_open(void) {
    load_settings(&conf); refresh();
    win = window_create("Network", 40, 2, 76, 50, draw, key);
    if (win >= 0) window_set_on_click(win, click);
    return win;
}
bool aurora_network_tick(void) { if (win < 0 || now_ms() - refreshed_ms < 3000) return false; refresh(); if (wifi_page) aurora_wifi_refresh(); return true; }
