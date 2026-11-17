/* IPv6 campaign (tests/test_x64_ipv6.py): SLAAC from QEMU's router, /proc and netlink views, AF_INET6 TCP/UDP over
 * loopback and to the host through slirp, dual stack (v4-mapped, IPV6_V6ONLY), ancillary data, fragmentation,
 * link-local scope rules, ICMPv6 ping and raw sockets, glibc resolution and getifaddrs. Runs as /sbin/init. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] NET6 FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static uint16_t host_tcp, host_udp;
static struct in6_addr host6;   /* QEMU's host alias (prefix::2) */
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void slurp(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY); CHECK(fd >= 0); size_t used = 0; ssize_t n;
    while (used + 1 < size && (n = read(fd, out + used, size - 1 - used)) > 0) used += (size_t)n;
    out[used] = 0; close(fd);
}
static void dump(const char* path) { char text[8192]; slurp(path, text, sizeof(text)); printf("[64] %s:\n%s", path, text); }
static uint16_t port6(int fd) { struct sockaddr_in6 a; socklen_t n = sizeof(a); CHECK(getsockname(fd, (void*)&a, &n) == 0 && n == sizeof(a)); return ntohs(a.sin6_port); }
static int tcp6(void) { int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0); CHECK(fd >= 0); return fd; }
static struct sockaddr_in6 at6(const struct in6_addr* a, uint16_t port, uint32_t scope) {
    struct sockaddr_in6 s = {.sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_scope_id = scope}; s.sin6_addr = *a; return s;
}
static void exact_io(int fd, void* data, size_t size, int write_side) {
    size_t done = 0; while (done < size) { ssize_t n = write_side ? write(fd, (char*)data + done, size - done) : read(fd, (char*)data + done, size - done); CHECK(n > 0); done += (size_t)n; }
}
/* ---- 1. autoconfiguration ---- */
static unsigned ifindex;
static struct in6_addr global, linklocal;
static int find_addresses(void) {
    char text[4096]; slurp("/proc/net/if_inet6", text, sizeof(text));
    int found = 0; char* line = text;
    while (*line) {
        char hexaddr[33] = {0}; unsigned index, plen, scope, flags; char name[17];
        if (sscanf(line, "%32s %x %x %x %x %16s", hexaddr, &index, &plen, &scope, &flags, name) == 6 && strcmp(name, "lo")) {
            struct in6_addr a; for (int i = 0; i < 16; i++) { unsigned byte; sscanf(hexaddr + 2 * i, "%2x", &byte); a.s6_addr[i] = (uint8_t)byte; }
            ifindex = index;
            if (scope == 0x20 && !(flags & 0x40)) { linklocal = a; found |= 1; }
            if (scope == 0 && !(flags & (0x40 | 0x01))) { global = a; found |= 2; }
            if (scope == 0 && (flags & 0x01)) found |= 4;   /* a temporary (privacy) address */
        }
        char* next = strchr(line, '\n'); if (!next) break; line = next + 1;
    }
    return found;
}
static void autoconf(void) {
    uint64_t deadline = now_ms() + 20000; int found = 0;
    while (now_ms() < deadline && (found = find_addresses()) != 7) poll(NULL, 0, 200);
    dump("/proc/net/if_inet6"); dump("/proc/net/ipv6_route");
    CHECK(found & 1); CHECK(found & 2); CHECK(found & 4);
    char text[64]; inet_ntop(AF_INET6, &global, text, sizeof(text)); printf("[64] NET6 global %s ifindex %u\n", text, ifindex);
    /* EUI-64 from the MAC (Linux addr_gen_mode 0): 52:54:00:12:34:56 -> 5054:ff:fe12:3456 */
    CHECK(linklocal.s6_addr[8] == 0x50 && linklocal.s6_addr[11] == 0xFF && linklocal.s6_addr[12] == 0xFE && linklocal.s6_addr[15] == 0x56);
    host6 = global; memset(host6.s6_addr + 8, 0, 8); host6.s6_addr[15] = 2;
    char route[8192]; slurp("/proc/net/ipv6_route", route, sizeof(route));
    CHECK(strstr(route, "00000000000000000000000000000000 00 00000000000000000000000000000000 00 fe80"));   /* default via a link-local router */
    char value[64]; slurp("/proc/sys/net/ipv6/conf/eth0/hop_limit", value, sizeof(value)); CHECK(atoi(value) == 64);
    slurp("/proc/sys/net/ipv6/conf/eth0/use_tempaddr", value, sizeof(value)); CHECK(atoi(value) == 2);
    slurp("/proc/sys/net/ipv6/conf/all/forwarding", value, sizeof(value)); CHECK(atoi(value) == 0);
    puts("[64] NET6 SLAAC, TEMPORARY ADDRESS, EUI-64 LINK-LOCAL, DEFAULT ROUTE AND SYSCTLS PASSED");
}
/* ---- 2. loopback TCP/UDP ---- */
static void* echo_server(void* argument) {
    int server = (int)(intptr_t)argument, fd = accept4(server, NULL, NULL, SOCK_CLOEXEC); CHECK(fd >= 0);
    static char buffer[65536]; ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) exact_io(fd, buffer, (size_t)n, 1);
    CHECK(n == 0); close(fd); return NULL;
}
static void loopback_tcp(void) {
    int server = tcp6(); struct sockaddr_in6 a = at6(&in6addr_loopback, 0, 0);
    CHECK(bind(server, (void*)&a, sizeof(a)) == 0 && listen(server, 4) == 0);
    uint16_t port = port6(server); pthread_t thread; CHECK(pthread_create(&thread, NULL, echo_server, (void*)(intptr_t)server) == 0);
    int client = tcp6(); a = at6(&in6addr_loopback, port, 0); CHECK(connect(client, (void*)&a, sizeof(a)) == 0);
    struct sockaddr_in6 peer; socklen_t n = sizeof(peer);
    CHECK(getpeername(client, (void*)&peer, &n) == 0 && n == 28 && peer.sin6_family == AF_INET6 && ntohs(peer.sin6_port) == port && IN6_IS_ADDR_LOOPBACK(&peer.sin6_addr));
    static unsigned char block[262144], back[262144];
    for (size_t i = 0; i < sizeof(block); i++) block[i] = (unsigned char)(i * 131 + 7);
    for (size_t off = 0; off < sizeof(block); off += 16384) { exact_io(client, block + off, 16384, 1); exact_io(client, back + off, 16384, 0); }
    CHECK(!memcmp(block, back, sizeof(block)));
    struct tcp_info info; n = sizeof(info); CHECK(getsockopt(client, IPPROTO_TCP, TCP_INFO, &info, &n) == 0 && info.tcpi_state == TCP_ESTABLISHED && info.tcpi_snd_mss <= 1440);
    CHECK(shutdown(client, SHUT_WR) == 0); char c; CHECK(read(client, &c, 1) == 0); close(client);
    CHECK(pthread_join(thread, NULL) == 0); close(server);
    puts("[64] NET6 LOOPBACK TCP 256 KIB ECHO, NAMES AND MSS PASSED");
}
static void dual_stack(void) {
    int server = tcp6(); int value = -1, one = 1; socklen_t n = sizeof(value);
    CHECK(getsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &value, &n) == 0 && value == 0);
    struct sockaddr_in6 any = at6(&in6addr_any, 0, 0);
    CHECK(bind(server, (void*)&any, sizeof(any)) == 0 && listen(server, 4) == 0);
    uint16_t port = port6(server);
    CHECK(setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) == -1 && errno == EINVAL);   /* too late */
    int v4 = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a4 = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(v4, (void*)&a4, sizeof(a4)) == 0);
    struct sockaddr_in6 peer; n = sizeof(peer); int served = accept(server, (void*)&peer, &n); CHECK(served >= 0 && n == 28);
    CHECK(IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr) && peer.sin6_addr.s6_addr[15] == 1 && peer.sin6_addr.s6_addr[12] == 127);
    CHECK(write(v4, "x", 1) == 1); char c; CHECK(read(served, &c, 1) == 1 && c == 'x');
    close(served); close(v4);
    /* an AF_INET socket on the same port conflicts with the dual-stack listener... */
    int clash = socket(AF_INET, SOCK_STREAM, 0); a4.sin_addr.s_addr = 0; CHECK(bind(clash, (void*)&a4, sizeof(a4)) == -1 && errno == EADDRINUSE); close(clash);
    close(server);
    /* ...but not with an IPV6_V6ONLY one */
    int only = tcp6(); CHECK(setsockopt(only, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) == 0);
    any.sin6_port = 0; CHECK(bind(only, (void*)&any, sizeof(any)) == 0 && listen(only, 4) == 0); port = port6(only);
    int four = socket(AF_INET, SOCK_STREAM, 0); a4.sin_port = htons(port); a4.sin_addr.s_addr = 0;
    CHECK(bind(four, (void*)&a4, sizeof(a4)) == 0 && listen(four, 4) == 0);
    int c4 = socket(AF_INET, SOCK_STREAM, 0); a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK); CHECK(connect(c4, (void*)&a4, sizeof(a4)) == 0);
    int s4 = accept(four, NULL, NULL); CHECK(s4 >= 0);
    struct pollfd p = {only, POLLIN, 0}; CHECK(poll(&p, 1, 100) == 0);   /* the v4 client went to the AF_INET listener */
    int c6 = tcp6(); struct sockaddr_in6 to = at6(&in6addr_loopback, port, 0); CHECK(connect(c6, (void*)&to, sizeof(to)) == 0);
    int s6 = accept(only, NULL, NULL); CHECK(s6 >= 0);
    int mapped = tcp6(); CHECK(setsockopt(mapped, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) == 0);
    struct in6_addr m = {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 127, 0, 0, 1}}}; to = at6(&m, port, 0);
    CHECK(connect(mapped, (void*)&to, sizeof(to)) == -1 && errno == ENETUNREACH);
    close(mapped); close(c4); close(s4); close(c6); close(s6); close(four); close(only);
    puts("[64] NET6 DUAL STACK: V4-MAPPED ACCEPT, V6ONLY, PORT SHARING RULES PASSED");
}
static void loopback_udp(void) {
    int a = socket(AF_INET6, SOCK_DGRAM, 0), b = socket(AF_INET6, SOCK_DGRAM, 0); CHECK(a >= 0 && b >= 0);
    struct sockaddr_in6 to = at6(&in6addr_loopback, 0, 0); CHECK(bind(a, (void*)&to, sizeof(to)) == 0); to.sin6_port = htons(port6(a));
    int one = 1; CHECK(setsockopt(a, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)) == 0 && setsockopt(a, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one)) == 0);
    CHECK(setsockopt(a, IPPROTO_IPV6, IPV6_RECVTCLASS, &one, sizeof(one)) == 0);
    int hops = 17, tclass = 0x28; CHECK(setsockopt(b, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops)) == 0 && setsockopt(b, IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass)) == 0);
    CHECK(sendto(b, "hello6", 6, 0, (void*)&to, sizeof(to)) == 6);
    char data[16], control[256]; struct sockaddr_in6 from; struct iovec v = {data, sizeof(data)};
    struct msghdr m = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct pollfd p = {a, POLLIN, 0}; CHECK(poll(&p, 1, 2000) == 1);
    CHECK(recvmsg(a, &m, 0) == 6 && !memcmp(data, "hello6", 6) && m.msg_namelen == 28 && ntohs(from.sin6_port) == port6(b));
    int got = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) { struct in6_pktinfo* pi = (void*)CMSG_DATA(c); CHECK(IN6_IS_ADDR_LOOPBACK(&pi->ipi6_addr) && pi->ipi6_ifindex == 1); got |= 1; }
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT) { CHECK(*(int*)CMSG_DATA(c) == 17); got |= 2; }
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS) { CHECK(*(int*)CMSG_DATA(c) == 0x28); got |= 4; }
    }
    CHECK(got == 7);
    /* 9000 bytes: fragmented to the 1500-byte loopback slots and reassembled */
    static char big[9000], back[9100]; for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i * 7);
    CHECK(sendto(b, big, sizeof(big), 0, (void*)&to, sizeof(to)) == (ssize_t)sizeof(big));
    CHECK(poll(&p, 1, 2000) == 1 && recv(a, back, sizeof(back), 0) == (ssize_t)sizeof(big) && !memcmp(big, back, sizeof(big)));
    /* IPV6_DONTFRAG: EMSGSIZE instead */
    CHECK(setsockopt(b, IPPROTO_IPV6, IPV6_DONTFRAG, &one, sizeof(one)) == 0);
    CHECK(sendto(b, big, sizeof(big), 0, (void*)&to, sizeof(to)) == -1 && errno == EMSGSIZE);
    /* a dual-stack UDP socket receives IPv4 datagrams as v4-mapped sources */
    int dual = socket(AF_INET6, SOCK_DGRAM, 0); struct sockaddr_in6 any = at6(&in6addr_any, 0, 0); CHECK(bind(dual, (void*)&any, sizeof(any)) == 0);
    int v4 = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a4 = {.sin_family = AF_INET, .sin_port = htons(port6(dual)), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(sendto(v4, "four", 4, 0, (void*)&a4, sizeof(a4)) == 4);
    p.fd = dual; CHECK(poll(&p, 1, 2000) == 1); socklen_t n = sizeof(from);
    CHECK(recvfrom(dual, data, sizeof(data), 0, (void*)&from, &n) == 4 && n == 28 && IN6_IS_ADDR_V4MAPPED(&from.sin6_addr));
    CHECK(sendto(dual, "back", 4, 0, (void*)&from, n) == 4);   /* and answers through IPv4 */
    p.fd = v4; CHECK(poll(&p, 1, 2000) == 1 && recv(v4, data, sizeof(data), 0) == 4 && !memcmp(data, "back", 4));
    close(a); close(b); close(dual); close(v4);
    /* port unreachable reaches a connected socket as ECONNREFUSED */
    int lonely = socket(AF_INET6, SOCK_DGRAM, 0), closed = socket(AF_INET6, SOCK_DGRAM, 0); struct sockaddr_in6 c6 = at6(&in6addr_loopback, 0, 0);
    CHECK(bind(closed, (void*)&c6, sizeof(c6)) == 0); c6.sin6_port = htons(port6(closed)); close(closed);
    CHECK(connect(lonely, (void*)&c6, sizeof(c6)) == 0 && send(lonely, "?", 1, 0) == 1);
    p.fd = lonely; p.events = POLLIN; poll(&p, 1, 2000);
    CHECK(recv(lonely, data, 1, MSG_DONTWAIT) == -1 && errno == ECONNREFUSED); close(lonely);
    puts("[64] NET6 UDP: PKTINFO/HOPLIMIT/TCLASS, 9000-BYTE FRAGMENTS, DONTFRAG, V4-MAPPED, PORT UNREACHABLE PASSED");
}
static void link_local(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0); struct sockaddr_in6 a = at6(&linklocal, 0, 0);
    CHECK(bind(fd, (void*)&a, sizeof(a)) == -1 && errno == EINVAL);           /* scope id required */
    a.sin6_scope_id = ifindex; CHECK(bind(fd, (void*)&a, sizeof(a)) == 0);
    struct sockaddr_in6 got; socklen_t n = sizeof(got); CHECK(getsockname(fd, (void*)&got, &n) == 0 && got.sin6_scope_id == ifindex);
    int tcp = tcp6(); struct sockaddr_in6 to = at6(&linklocal, 9, 0);
    CHECK(connect(tcp, (void*)&to, sizeof(to)) == -1 && errno == EINVAL); close(tcp);
    /* our own link-local address with its scope: through loopback */
    int server = tcp6(); a.sin6_port = 0; CHECK(bind(server, (void*)&a, sizeof(a)) == 0 && listen(server, 1) == 0);
    int client = tcp6(); to = at6(&linklocal, port6(server), ifindex); CHECK(connect(client, (void*)&to, sizeof(to)) == 0);
    int served = accept(server, (void*)&got, &n); CHECK(served >= 0 && got.sin6_scope_id == ifindex);
    CHECK(write(client, "L", 1) == 1); char c; CHECK(read(served, &c, 1) == 1);
    close(client); close(served); close(server); close(fd);
    puts("[64] NET6 LINK-LOCAL SCOPE RULES PASSED");
}
/* ---- 3. through QEMU to the host ---- */
static void host_tcp_stream(void) {
    int fd = tcp6(); struct sockaddr_in6 to = at6(&host6, host_tcp, 0);
    CHECK(connect(fd, (void*)&to, sizeof(to)) == 0);
    struct sockaddr_in6 local; socklen_t n = sizeof(local); CHECK(getsockname(fd, (void*)&local, &n) == 0);
    char text[64]; inet_ntop(AF_INET6, &local.sin6_addr, text, sizeof(text)); printf("[64] NET6 host connection from %s\n", text);
    CHECK(!IN6_IS_ADDR_LINKLOCAL(&local.sin6_addr));
    static unsigned char block[1 << 20]; size_t total = 0;
    CHECK(write(fd, "GET6", 4) == 4);
    ssize_t r; while ((r = read(fd, block + total, sizeof(block) - total)) > 0) total += (size_t)r;
    CHECK(r == 0 && total == sizeof(block));
    for (size_t i = 0; i < total; i++) CHECK(block[i] == (unsigned char)(i * 29 + 3));
    close(fd);
    int u = socket(AF_INET6, SOCK_DGRAM, 0); to = at6(&host6, host_udp, 0);
    CHECK(sendto(u, "udp6-echo", 9, 0, (void*)&to, sizeof(to)) == 9);
    struct pollfd p = {u, POLLIN, 0}; char back[32]; CHECK(poll(&p, 1, 5000) == 1 && recv(u, back, sizeof(back), 0) == 9 && !memcmp(back, "udp6-echo", 9));
    /* 4000 bytes leave as a fragment train (slirp cannot reassemble IPv6: the harness checks the frames) */
    static char big[4000]; for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i * 5 + 1);
    CHECK(sendto(u, big, sizeof(big), 0, (void*)&to, sizeof(to)) == (ssize_t)sizeof(big));
    close(u);
    puts("[64] NET6 HOST TCP 1 MIB AND UDP THROUGH QEMU PASSED");
}
static void ping6(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6); CHECK(fd >= 0);
    struct sockaddr_in6 to = at6(&host6, 0, 0);
    unsigned char request[64] = {ICMP6_ECHO_REQUEST, 0, 0, 0, 0x12, 0x34, 0, 1}; for (int i = 8; i < 64; i++) request[i] = (unsigned char)i;
    CHECK(sendto(fd, request, sizeof(request), 0, (void*)&to, sizeof(to)) == sizeof(request));
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 5000) == 1);
    unsigned char reply[128]; struct sockaddr_in6 from; socklen_t n = sizeof(from);
    CHECK(recvfrom(fd, reply, sizeof(reply), 0, (void*)&from, &n) == sizeof(request) && reply[0] == ICMP6_ECHO_REPLY && !memcmp(reply + 8, request + 8, 56));
    CHECK(!memcmp(&from.sin6_addr, &host6, 16));
    close(fd);
    /* raw ICMPv6 with a filter that passes only echo replies: CAP_NET_RAW, which the desktop owner holds in the
     * initial namespaces (netlink.c net_raw_capable); a process in a user namespace of its own does not */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { if (unshare(CLONE_NEWUSER)) _exit(2); int r = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6); _exit(r == -1 && errno == EPERM ? 0 : 3); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    int raw = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    CHECK(raw >= 0);
    struct icmp6_filter filter; ICMP6_FILTER_SETBLOCKALL(&filter); ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
    CHECK(setsockopt(raw, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) == 0);
    request[4] = 0x56; to = at6(&in6addr_loopback, 0, 0);
    CHECK(sendto(raw, request, sizeof(request), 0, (void*)&to, sizeof(to)) == sizeof(request));
    p.fd = raw; CHECK(poll(&p, 1, 2000) == 1);
    CHECK(recv(raw, reply, sizeof(reply), 0) == sizeof(request) && reply[0] == ICMP6_ECHO_REPLY && reply[4] == 0x56);
    close(raw);
    puts("[64] NET6 PING SOCKET TO THE ROUTER AND RAW ICMPV6 WITH FILTER PASSED (raw refused inside a user namespace)");
}
/* ---- 4. libc and netlink views ---- */
static void views(void) {
    struct ifaddrs* list; CHECK(getifaddrs(&list) == 0); int v6 = 0;
    for (struct ifaddrs* i = list; i; i = i->ifa_next) if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET6) v6++;
    freeifaddrs(list); CHECK(v6 >= 3);
    CHECK(if_nametoindex("eth0") == ifindex && if_nametoindex("lo") == 1);
    struct addrinfo hints = {.ai_family = AF_INET6, .ai_socktype = SOCK_STREAM}, *result;
    CHECK(getaddrinfo("::1", "80", &hints, &result) == 0 && result->ai_family == AF_INET6); freeaddrinfo(result);
    hints.ai_flags = AI_ADDRCONFIG; hints.ai_family = AF_UNSPEC;
    CHECK(getaddrinfo("localhost", "80", &hints, &result) == 0); freeaddrinfo(result);
    int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE); CHECK(nl >= 0);
    struct { struct nlmsghdr h; struct ifaddrmsg m; } request = {{sizeof(request), RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 7, 0}, {AF_INET6, 0, 0, 0, 0}};
    CHECK(send(nl, &request, sizeof(request), 0) == sizeof(request));
    char buffer[16384]; int addresses = 0, done = 0;
    while (!done) {
        ssize_t n = recv(nl, buffer, sizeof(buffer), 0); CHECK(n > 0);
        for (struct nlmsghdr* h = (void*)buffer; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            CHECK(h->nlmsg_type == RTM_NEWADDR && h->nlmsg_seq == 7);
            struct ifaddrmsg* m = NLMSG_DATA(h); CHECK(m->ifa_family == AF_INET6); addresses++;
        }
    }
    CHECK(addresses >= 3); close(nl);
    dump("/proc/net/snmp6");
    puts("[64] NET6 GETIFADDRS, GETADDRINFO, RTM_GETADDR DUMP PASSED");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int config = open("/nexus/net6-ports", O_RDONLY); CHECK(config >= 0);
    uint16_t ports[2]; CHECK(read(config, ports, 4) == 4); close(config); host_tcp = ports[0]; host_udp = ports[1];
    autoconf(); loopback_tcp(); dual_stack(); loopback_udp(); link_local(); host_tcp_stream(); ping6(); views();
    puts("[64] NET6 CAMPAIGN PASSED");
    return 0;
}
