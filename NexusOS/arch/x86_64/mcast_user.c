/* Guest agent for tests/test_x64_multicast.py: a socket workbench driven over UDP [::]:7001 (each command's answer
 * goes back to its sender). The host builds every socket-option structure itself (hex), so the kernel's parsing of
 * well-formed and hostile ip_mreq/ip_mreqn/ip_mreq_source/ip_msfilter/group_req/group_source_req/group_filter values
 * is tested exactly as programs pass them. Runs as /sbin/init (uid 1000, the desktop owner).
 *   SOCK <4|6> <port>                    UDP socket, SO_REUSEADDR, bound to the wildcard address, packet info on: "OK <id>"
 *   SETOPT <id> <level> <name> <hex>     setsockopt: "RESULT <errno>"
 *   GETOPT <id> <level> <name> <size> [hex]   getsockopt with a buffer of <size> preloaded with hex: "RESULT <errno> <len> <hex>"
 *   SEND <id> <address> <port> <text>    sendto: "RESULT <errno> <bytes>"
 *   RECV <id> <ms>                       every datagram until <ms> pass without one: "RECV <n>" + lines
 *                                        "<source> <port> if=<ifindex> dst=<destination> <text>"
 *   CLOSE <id> | IFINDEX <name> | FILE <path> | EXIT */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static char reply[60000];
static int sockets[64];

static int unhex(const char* hex, unsigned char* out, size_t capacity) {
    size_t n = 0;
    for (const char* h = hex; h[0] && h[1] && n < capacity; h += 2) { unsigned v; if (sscanf(h, "%2x", &v) != 1) return -1; out[n++] = (unsigned char)v; }
    return (int)n;
}
static size_t tohex(const unsigned char* data, size_t n, char* out, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < n && used + 3 < capacity; i++) used += (size_t)snprintf(out + used, capacity - used, "%02x", data[i]);
    return used;
}
static int parse(const char* text, struct sockaddr_storage* a, socklen_t* length, int port) {
    memset(a, 0, sizeof(*a));
    struct sockaddr_in6* s6 = (void*)a; struct sockaddr_in* s4 = (void*)a;
    if (inet_pton(AF_INET6, text, &s6->sin6_addr) == 1) { s6->sin6_family = AF_INET6; s6->sin6_port = htons((uint16_t)port); *length = sizeof(*s6); return 0; }
    if (inet_pton(AF_INET, text, &s4->sin_addr) == 1) { s4->sin_family = AF_INET; s4->sin_port = htons((uint16_t)port); *length = sizeof(*s4); return 0; }
    return -1;
}
static void slurp(const char* path) {
    int fd = open(path, O_RDONLY); if (fd < 0) { snprintf(reply, sizeof(reply), "ERR %d", errno); return; }
    size_t used = 0; ssize_t n; while (used + 1 < sizeof(reply) && (n = read(fd, reply + used, sizeof(reply) - 1 - used)) > 0) used += (size_t)n;
    reply[used] = 0; close(fd);
}
static int lookup(const char* text) { int id = atoi(text); return id >= 0 && id < 64 && sockets[id] > 0 ? sockets[id] : -1; }
static void receive(int fd, int ms) {
    size_t used = (size_t)snprintf(reply, sizeof(reply), "RECV "); size_t count_at = used; used += 8; int count = 0;
    memset(reply + count_at, ' ', 8); reply[used++] = '\n';
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, ms) <= 0) break;
        char data[2048], control[256]; struct sockaddr_storage from; struct iovec v = {data, sizeof(data) - 1};
        struct msghdr m = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        ssize_t n = recvmsg(fd, &m, MSG_DONTWAIT); if (n < 0) break;
        data[n] = 0; for (ssize_t i = 0; i < n; i++) if (data[i] == '\n' || data[i] == ' ') data[i] = '_';
        char source[64] = "?", destination[64] = "?"; int port = 0, ifindex = 0;
        if (from.ss_family == AF_INET6) { inet_ntop(AF_INET6, &((struct sockaddr_in6*)&from)->sin6_addr, source, sizeof(source)); port = ntohs(((struct sockaddr_in6*)&from)->sin6_port); }
        else if (from.ss_family == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in*)&from)->sin_addr, source, sizeof(source)); port = ntohs(((struct sockaddr_in*)&from)->sin_port); }
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo i; memcpy(&i, CMSG_DATA(c), sizeof(i)); ifindex = i.ipi_ifindex; inet_ntop(AF_INET, &i.ipi_addr, destination, sizeof(destination));
            } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
                struct in6_pktinfo i; memcpy(&i, CMSG_DATA(c), sizeof(i)); ifindex = (int)i.ipi6_ifindex; inet_ntop(AF_INET6, &i.ipi6_addr, destination, sizeof(destination));
            }
        }
        used += (size_t)snprintf(reply + used, sizeof(reply) - used, "%s %d if=%d dst=%s %.200s\n", source, port, ifindex, destination, data);
        count++;
        if (used + 400 >= sizeof(reply)) break;
    }
    char number[9]; snprintf(number, sizeof(number), "%d", count); memcpy(reply + count_at, number, strlen(number));
    reply[used] = 0;
}
static void command(char* text) {
    char verb[32] = {0}, a1[256] = {0}, a2[256] = {0}, a3[256] = {0}, a4[20000] = {0};
    sscanf(text, "%31s %255s %255s %255s %19999s", verb, a1, a2, a3, a4);
    reply[0] = 0;
    if (!strcmp(verb, "SOCK")) {
        int family = atoi(a1) == 6 ? AF_INET6 : AF_INET, id = -1;
        for (int i = 1; i < 64; i++) if (sockets[i] <= 0) { id = i; break; }
        int fd = socket(family, SOCK_DGRAM, 0), one = 1;
        if (fd < 0 || id < 0) { snprintf(reply, sizeof(reply), "ERR %d", errno); if (fd >= 0) close(fd); return; }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (family == AF_INET6) { setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)); setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)); }
        else setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
        struct sockaddr_storage any; memset(&any, 0, sizeof(any)); socklen_t length;
        if (family == AF_INET6) { struct sockaddr_in6* s = (void*)&any; s->sin6_family = AF_INET6; s->sin6_port = htons((uint16_t)atoi(a2)); length = sizeof(*s); }
        else { struct sockaddr_in* s = (void*)&any; s->sin_family = AF_INET; s->sin_port = htons((uint16_t)atoi(a2)); length = sizeof(*s); }
        if (bind(fd, (void*)&any, length)) { snprintf(reply, sizeof(reply), "ERR bind %d", errno); close(fd); return; }
        sockets[id] = fd; snprintf(reply, sizeof(reply), "OK %d", id);
    } else if (!strcmp(verb, "SETOPT")) {
        int fd = lookup(a1); if (fd < 0) { strcpy(reply, "ERR socket"); return; }
        static unsigned char value[10000]; int n = strcmp(a4, "-") ? unhex(a4, value, sizeof(value)) : 0;
        if (n < 0) { strcpy(reply, "ERR hex"); return; }
        int r = setsockopt(fd, atoi(a2), atoi(a3), value, (socklen_t)n);
        snprintf(reply, sizeof(reply), "RESULT %d", r ? errno : 0);
    } else if (!strcmp(verb, "GETOPT")) {
        int fd = lookup(a1); if (fd < 0) { strcpy(reply, "ERR socket"); return; }
        char size_text[16] = {0}; static char hex[20000]; memset(hex, 0, sizeof(hex)); sscanf(a4, "%15[^:]:%19999s", size_text, hex);   /* <size>:<hex> */
        static unsigned char value[10000]; memset(value, 0, sizeof(value));
        if (hex[0]) unhex(hex, value, sizeof(value));
        socklen_t size = (socklen_t)atoi(size_text); if (size > sizeof(value)) size = sizeof(value);
        int r = getsockopt(fd, atoi(a2), atoi(a3), value, &size);
        size_t used = (size_t)snprintf(reply, sizeof(reply), "RESULT %d %u ", r ? errno : 0, (unsigned)size);
        if (!r) tohex(value, size, reply + used, sizeof(reply) - used);
    } else if (!strcmp(verb, "SEND")) {
        int fd = lookup(a1); if (fd < 0) { strcpy(reply, "ERR socket"); return; }
        struct sockaddr_storage to; socklen_t length; if (parse(a2, &to, &length, atoi(a3))) { strcpy(reply, "ERR address"); return; }
        ssize_t n = sendto(fd, a4, strlen(a4), 0, (void*)&to, length);
        snprintf(reply, sizeof(reply), "RESULT %d %zd", n < 0 ? errno : 0, n);
    } else if (!strcmp(verb, "RECV")) {
        int fd = lookup(a1); if (fd < 0) { strcpy(reply, "ERR socket"); return; }
        receive(fd, atoi(a2));
    } else if (!strcmp(verb, "CLOSE")) {
        int id = atoi(a1), fd = lookup(a1); if (fd < 0) { strcpy(reply, "ERR socket"); return; }
        close(fd); sockets[id] = 0; strcpy(reply, "OK");
    } else if (!strcmp(verb, "IFINDEX")) snprintf(reply, sizeof(reply), "%u", if_nametoindex(a1));
    else if (!strcmp(verb, "FILE")) slurp(a1);
    else if (!strcmp(verb, "EXIT")) strcpy(reply, "BYE");
    else snprintf(reply, sizeof(reply), "ERR unknown %s", verb);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int control = socket(AF_INET6, SOCK_DGRAM, 0), zero = 0; setsockopt(control, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    struct sockaddr_in6 any = {.sin6_family = AF_INET6, .sin6_port = htons(7001)};
    if (bind(control, (void*)&any, sizeof(any))) { printf("[64] MCAST bind failed %d\n", errno); return 1; }
    puts("[64] MCAST AGENT READY");
    static char buffer[40000];
    for (;;) {
        struct sockaddr_in6 from; socklen_t n = sizeof(from);
        ssize_t got = recvfrom(control, buffer, sizeof(buffer) - 1, 0, (void*)&from, &n);
        if (got <= 0) continue;
        buffer[got] = 0;
        printf("[64] MCAST command: %.100s\n", buffer);
        command(buffer);
        sendto(control, reply, strlen(reply), 0, (void*)&from, n);
        if (!strcmp(reply, "BYE")) break;
    }
    puts("[64] MCAST AGENT DONE");
    return 0;
}
