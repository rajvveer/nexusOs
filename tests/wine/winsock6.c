/* NexusOS IPv6 through Wine's winsock (tests/test_x64_ipv6_apps.py), built for x86_64-windows-gnu (zig cc).
 *
 *   winsock6.exe MODE SERVER [PORT]
 *     MODE   dual   (IPv4 + IPv6 network, names from the fixture's DNS)
 *            v6only (no IPv4 at all)
 *            slirp  (QEMU user network, IPv6 only: SERVER is a literal, no test names)
 *     SERVER the test server name or literal (dual.nexus.test / fec0::2), PORT its HTTP port (80)
 *
 * Checks: GetAdaptersAddresses (a global IPv6 unicast address, an IPv6 DNS server from RA/DHCPv6), getaddrinfo order and
 * families (AI_ADDRCONFIG), HTTP over IPv6 (and IPv4 when present), the connect loop's fallback over a dead IPv6 path
 * (non-blocking connect + select, as Windows programs do), IPV6_V6ONLY default (Windows: on), a dual-stack listener
 * that the test server connects back to over IPv6 and IPv4 (v4-mapped peer), UDP over IPv6 (echo).
 * Prints "WS6 PASS <what>" / "WS6 FAIL <what> (<detail>)" and "WS6 SUMMARY pass=N fail=M"; exit code = failures. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passes, failures;
static const char* mode = "dual";
static const char* server = "dual.nexus.test";
static const char* port = "80";

static void check(int ok, const char* what, const char* format, ...) {
    char detail[700] = "";
    if (format) { va_list list; va_start(list, format); vsnprintf(detail, sizeof(detail), format, list); va_end(list); }
    if (ok) { passes++; printf("WS6 PASS %s%s%s%s\n", what, detail[0] ? " (" : "", detail, detail[0] ? ")" : ""); }
    else { failures++; printf("WS6 FAIL %s (%s; WSAGetLastError=%d)\n", what, detail, WSAGetLastError()); }
    fflush(stdout);
}
static void info(const char* format, ...) {
    char text[1024]; va_list list; va_start(list, format); vsnprintf(text, sizeof(text), format, list); va_end(list);
    printf("WS6 INFO %s\n", text); fflush(stdout);
}
static const char* text_of(const struct sockaddr* a, char* out, size_t size) {
    char host[INET6_ADDRSTRLEN] = "?";
    if (a->sa_family == AF_INET6) inet_ntop(AF_INET6, &((const struct sockaddr_in6*)a)->sin6_addr, host, sizeof(host));
    else if (a->sa_family == AF_INET) inet_ntop(AF_INET, &((const struct sockaddr_in*)a)->sin_addr, host, sizeof(host));
    snprintf(out, size, "%s", host); return out;
}
static int is_global6(const struct in6_addr* a) { return (a->s6_addr[0] & 0xE0) == 0x20 || (a->s6_addr[0] == 0xFE && (a->s6_addr[1] & 0xC0) == 0xC0); }

/* connect with a timeout (non-blocking connect + select); 0 = connected */
static int connect_timeout(SOCKET s, const struct sockaddr* a, int length, int ms) {
    u_long on = 1; ioctlsocket(s, FIONBIO, &on);
    int r = connect(s, a, length);
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) return WSAGetLastError();
    if (r == SOCKET_ERROR) {
        fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
        struct timeval t = {ms / 1000, (ms % 1000) * 1000};
        int ready = select(0, NULL, &w, &e, &t);
        if (ready == 0) return WSAETIMEDOUT;
        if (FD_ISSET(s, &e) || !FD_ISSET(s, &w)) { int error = 0, n = sizeof(error); getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&error, &n); return error ? error : WSAECONNREFUSED; }
    }
    u_long off = 0; ioctlsocket(s, FIONBIO, &off); return 0;
}
/* GET path over a connected socket; returns the HTTP status (body in out) or -1 */
static int http_get(SOCKET s, const char* host, const char* path, char* out, int size) {
    char request[512]; int n = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: winsock6\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, request, n, 0) != n) return -1;
    DWORD timeout = 10000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    int used = 0, got;
    while (used < size - 1 && (got = recv(s, out + used, size - 1 - used, 0)) > 0) used += got;
    out[used] = 0; int status = -1; sscanf(out, "HTTP/%*s %d", &status); return status;
}

static void test_adapters(void) {
    ULONG size = 0; GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size);
    IP_ADAPTER_ADDRESSES* list = size ? malloc(size) : NULL;
    ULONG r = list ? GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, list, &size) : ERROR_NOT_ENOUGH_MEMORY;
    check(r == NO_ERROR, "GetAdaptersAddresses", "error %lu", r);
    int global6 = 0, dns6 = 0, v4 = 0; char seen[1024] = ""; size_t used = 0;
    for (IP_ADAPTER_ADDRESSES* a = r == NO_ERROR ? list : NULL; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            char t[64]; text_of(u->Address.lpSockaddr, t, sizeof(t));
            used += (size_t)snprintf(seen + used, used < sizeof(seen) ? sizeof(seen) - used : 0, "%s ", t);
            if (u->Address.lpSockaddr->sa_family == AF_INET6 && is_global6(&((struct sockaddr_in6*)u->Address.lpSockaddr)->sin6_addr)) global6++;
            if (u->Address.lpSockaddr->sa_family == AF_INET) v4++;
        }
        for (IP_ADAPTER_DNS_SERVER_ADDRESS* d = a->FirstDnsServerAddress; d; d = d->Next) {
            char t[64]; text_of(d->Address.lpSockaddr, t, sizeof(t));
            used += (size_t)snprintf(seen + used, used < sizeof(seen) ? sizeof(seen) - used : 0, "dns=%s ", t);
            if (d->Address.lpSockaddr->sa_family == AF_INET6) dns6++;
        }
    }
    info("adapters: %s", seen);
    check(global6 > 0, "an adapter has a global IPv6 address (SLAAC)", "%d", global6);
    check(dns6 > 0, "an adapter lists an IPv6 DNS server (RA RDNSS / DHCPv6)", "%d", dns6);
    if (!strcmp(mode, "v6only") || !strcmp(mode, "slirp")) check(v4 == 0, "no IPv4 address on an IPv6-only network", "%d", v4);
    free(list);
}

static void test_resolve(void) {
    struct addrinfo hints = {0}, *res = NULL; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_ADDRCONFIG;
    int r = getaddrinfo(server, port, &hints, &res);
    check(r == 0 && res, "getaddrinfo(AF_UNSPEC, AI_ADDRCONFIG) of the server", "%s: %d", server, r);
    int n6 = 0, n4 = 0, first = res ? res->ai_family : 0; char order[512] = ""; size_t used = 0;
    for (struct addrinfo* i = res; i; i = i->ai_next) {
        char t[64]; used += (size_t)snprintf(order + used, used < sizeof(order) ? sizeof(order) - used : 0, "%s ", text_of(i->ai_addr, t, sizeof(t)));
        if (i->ai_family == AF_INET6) n6++; else if (i->ai_family == AF_INET) n4++;
    }
    info("getaddrinfo %s -> %s", server, order);
    check(n6 > 0 && first == AF_INET6, "IPv6 result first (RFC 6724 order)", "%s", order);
    if (!strcmp(mode, "dual")) check(n4 > 0, "IPv4 result present on a dual-stack network", "%s", order);
    else check(n4 == 0, "no IPv4 result with AI_ADDRCONFIG on an IPv6-only network", "%s", order);
    /* HTTP over the IPv6 result */
    for (struct addrinfo* i = res; i; i = i->ai_next) {
        if (i->ai_family != AF_INET6) continue;
        SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); char body[4096] = "";
        int e = connect_timeout(s, i->ai_addr, (int)i->ai_addrlen, 8000);
        int status = e ? -1 : http_get(s, server, "/ws6/v6", body, sizeof(body));
        check(status == 200 && strstr(body, "WS6-OK family=6"), "HTTP GET over IPv6 (winsock connect/send/recv)", "connect %d status %d", e, status);
        closesocket(s); break;
    }
    if (!strcmp(mode, "dual"))
        for (struct addrinfo* i = res; i; i = i->ai_next) {
            if (i->ai_family != AF_INET) continue;
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); char body[4096] = "";
            int e = connect_timeout(s, i->ai_addr, (int)i->ai_addrlen, 8000);
            int status = e ? -1 : http_get(s, server, "/ws6/v4", body, sizeof(body));
            check(status == 200 && strstr(body, "WS6-OK family=4"), "HTTP GET over IPv4 on the same name", "connect %d status %d", e, status);
            closesocket(s); break;
        }
    if (res) freeaddrinfo(res);
}

/* brokenv6.nexus.test: AAAA = a dead path (blackhole), A works. Programs walk getaddrinfo's list: the IPv6 attempt
 * times out, the IPv4 one connects. brokenv4.nexus.test: the reverse, IPv6 first and working. */
static void test_fallback(const char* name, int want_family) {
    struct addrinfo hints = {0}, *res = NULL; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_ADDRCONFIG;
    int r = getaddrinfo(name, port, &hints, &res); char what[160];
    snprintf(what, sizeof(what), "connect loop over %s reaches it over IPv%d", name, want_family == AF_INET6 ? 6 : 4);
    if (r || !res) { check(0, what, "getaddrinfo %d", r); return; }
    int family = 0, tried = 0; DWORD t0 = GetTickCount(); char trace[512] = ""; size_t used = 0;
    for (struct addrinfo* i = res; i && !family; i = i->ai_next) {
        SOCKET s = socket(i->ai_family, SOCK_STREAM, IPPROTO_TCP); tried++;
        int e = connect_timeout(s, i->ai_addr, (int)i->ai_addrlen, 2500); char t[64];
        used += (size_t)snprintf(trace + used, used < sizeof(trace) ? sizeof(trace) - used : 0, "%s:%d ", text_of(i->ai_addr, t, sizeof(t)), e);
        if (!e) {
            char body[4096]; int status = http_get(s, name, want_family == AF_INET6 ? "/ws6/fallback6" : "/ws6/fallback4", body, sizeof(body));
            if (status == 200) family = i->ai_family;
        }
        closesocket(s);
    }
    check(family == want_family, what, "%s tried=%d %lu ms", trace, tried, (unsigned long)(GetTickCount() - t0));
    freeaddrinfo(res);
}

static void test_v6only_default(void) {
    SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); DWORD value = 7; int n = sizeof(value);
    int r = getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&value, &n);
    check(r == 0 && value == 1, "IPV6_V6ONLY defaults to on (Windows semantics)", "r=%d value=%lu", r, (unsigned long)value);
    closesocket(s);
}

/* [::]:7090 with IPV6_V6ONLY off; the server connects back over IPv6 (and IPv4 in dual mode) */
static void test_dual_listener(void) {
    SOCKET l = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); DWORD off = 0;
    int r = setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
    struct sockaddr_in6 any = {0}; any.sin6_family = AF_INET6; any.sin6_port = htons(7090);
    if (!r) r = bind(l, (struct sockaddr*)&any, sizeof(any));
    if (!r) r = listen(l, 4);
    check(r == 0, "dual-stack listener [::]:7090 (IPV6_V6ONLY=0)", "r=%d", r);
    if (r) { closesocket(l); return; }
    /* ask the test server to connect back */
    struct addrinfo hints = {0}, *res = NULL; hints.ai_family = AF_INET6; hints.ai_socktype = SOCK_STREAM;
    int want = !strcmp(mode, "dual") ? 2 : 1, got6 = 0, got4 = 0;
    if (!getaddrinfo(server, port, &hints, &res) && res) {
        SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); char body[2048];
        int e = connect_timeout(s, res->ai_addr, (int)res->ai_addrlen, 8000);
        int status = e ? -1 : http_get(s, server, "/ws6/connect-back?port=7090", body, sizeof(body));
        check(status == 200, "asked the server to connect back", "connect %d status %d", e, status);
        closesocket(s); freeaddrinfo(res);
    }
    for (int i = 0; i < want; i++) {
        fd_set rd; FD_ZERO(&rd); FD_SET(l, &rd); struct timeval t = {15, 0};
        if (select(0, &rd, NULL, NULL, &t) <= 0) break;
        struct sockaddr_in6 peer; int n = sizeof(peer); SOCKET c = accept(l, (struct sockaddr*)&peer, &n);
        if (c == INVALID_SOCKET) break;
        char t6[64]; text_of((struct sockaddr*)&peer, t6, sizeof(t6));
        int mapped = IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr);
        char hello[64] = ""; DWORD timeout = 5000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        int k = recv(c, hello, sizeof(hello) - 1, 0); if (k > 0) hello[k] = 0;
        send(c, hello, k > 0 ? k : 0, 0);
        info("accepted %s (v4-mapped %d) said %s", t6, mapped, hello);
        if (mapped && !strncmp(hello, "HELLO4", 6)) got4++;
        if (!mapped && !strncmp(hello, "HELLO6", 6)) got6++;
        closesocket(c);
    }
    check(got6 == 1, "accepted the server's IPv6 connection (native IPv6 peer)", "%d", got6);
    if (want == 2) check(got4 == 1, "accepted the server's IPv4 connection as a v4-mapped peer", "%d", got4);
    closesocket(l);
}

static void test_udp6(void) {
    struct addrinfo hints = {0}, *res = NULL; hints.ai_family = AF_INET6; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server, "7", &hints, &res) || !res) { check(0, "UDP echo over IPv6", "getaddrinfo"); return; }
    SOCKET s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP); const char* message = "nexus-udp6-echo";
    DWORD timeout = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        sendto(s, message, (int)strlen(message), 0, res->ai_addr, (int)res->ai_addrlen);
        char back[128]; struct sockaddr_in6 from; int n = sizeof(from);
        int k = recvfrom(s, back, sizeof(back) - 1, 0, (struct sockaddr*)&from, &n);
        if (k > 0) { back[k] = 0; ok = !strcmp(back, message) && from.sin6_family == AF_INET6; }
    }
    check(ok, "UDP echo over IPv6 (sendto/recvfrom)", "%s", server);
    closesocket(s); freeaddrinfo(res);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) mode = argv[1];
    if (argc > 2) server = argv[2];
    if (argc > 3) port = argv[3];
    WSADATA data; check(!WSAStartup(MAKEWORD(2, 2), &data), "WSAStartup 2.2", NULL);
    test_adapters();
    test_resolve();
    test_v6only_default();
    if (!strcmp(mode, "dual")) { test_fallback("brokenv6.nexus.test", AF_INET); test_fallback("brokenv4.nexus.test", AF_INET6); }
    if (!strcmp(mode, "v6only")) test_fallback("brokenv4.nexus.test", AF_INET6);
    if (strcmp(mode, "slirp")) { test_dual_listener(); test_udp6(); }
    printf("WS6 SUMMARY pass=%d fail=%d\n", passes, failures);
    WSACleanup();
    return failures;
}
