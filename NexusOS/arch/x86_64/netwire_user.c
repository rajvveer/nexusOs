/* Guest agent for tests/test_x64_netwire.py: the host fixture is the whole network (router, DHCPv4/DHCPv6
 * server, attacker) and drives the campaign with commands over UDP to [::]:7001 / 0.0.0.0:7001; each command's
 * result goes back to the sender. Runs as /sbin/init (uid 1000, the desktop owner). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int sock6, events_fd = -1;
static char reply[60000];
static char events[16384]; static size_t events_used;
/* RTMGRP_LINK / IPV4_IFADDR / IPV6_IFADDR notifications, collected between commands (EVENTS returns them) */
static void event_line(const char* line) {
    size_t n = strlen(line); if (events_used + n + 1 >= sizeof(events)) return;
    memcpy(events + events_used, line, n + 1); events_used += n;
}
static void drain_events(void) {
    static char buffer[32768];
    for (;;) {
        ssize_t n = recv(events_fd, buffer, sizeof(buffer), MSG_DONTWAIT); if (n <= 0) return;
        for (struct nlmsghdr* h = (void*)buffer; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            char line[160];
            if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK) {
                struct ifinfomsg* m = NLMSG_DATA(h); int oper = -1, carrier = -1, length = (int)IFLA_PAYLOAD(h);
                for (struct rtattr* r = IFLA_RTA(m); RTA_OK(r, length); r = RTA_NEXT(r, length)) {
                    if (r->rta_type == IFLA_OPERSTATE) oper = *(unsigned char*)RTA_DATA(r);
                    if (r->rta_type == IFLA_CARRIER) carrier = *(unsigned char*)RTA_DATA(r);
                }
                snprintf(line, sizeof(line), "%s %d %x %d %d\n", h->nlmsg_type == RTM_NEWLINK ? "LINK" : "DELLINK", m->ifi_index, m->ifi_flags, oper, carrier);
                event_line(line);
            } else if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR) {
                struct ifaddrmsg* m = NLMSG_DATA(h); char text[64] = "?"; int length = (int)IFA_PAYLOAD(h);
                for (struct rtattr* r = IFA_RTA(m); RTA_OK(r, length); r = RTA_NEXT(r, length))
                    if (r->rta_type == IFA_ADDRESS || (r->rta_type == IFA_LOCAL && m->ifa_family == AF_INET)) inet_ntop(m->ifa_family, RTA_DATA(r), text, sizeof(text));
                snprintf(line, sizeof(line), "%s %s/%u %u\n", h->nlmsg_type == RTM_NEWADDR ? "NEWADDR" : "DELADDR", text, m->ifa_prefixlen, m->ifa_index);
                event_line(line);
            }
        }
    }
}
static void slurp(const char* path) {
    int fd = open(path, O_RDONLY); if (fd < 0) { snprintf(reply, sizeof(reply), "ERR %d", errno); return; }
    size_t used = 0; ssize_t n; while (used + 1 < sizeof(reply) && (n = read(fd, reply + used, sizeof(reply) - 1 - used)) > 0) used += (size_t)n;
    reply[used] = 0; close(fd);
}
static int parse(const char* text, struct sockaddr_in6* a) {
    memset(a, 0, sizeof(*a)); a->sin6_family = AF_INET6;
    char copy[96]; snprintf(copy, sizeof(copy), "%s", text); char* percent = strchr(copy, '%');
    if (percent) { *percent = 0; a->sin6_scope_id = if_nametoindex(percent + 1); }
    if (inet_pton(AF_INET6, copy, &a->sin6_addr) == 1) return 0;
    struct in_addr v4; if (inet_pton(AF_INET, copy, &v4) != 1) return -1;
    a->sin6_addr.s6_addr[10] = a->sin6_addr.s6_addr[11] = 0xFF; memcpy(a->sin6_addr.s6_addr + 12, &v4, 4); return 0;
}
static int netlink_addr(int add, const char* text, const char* device, unsigned flags) {
    char copy[96]; snprintf(copy, sizeof(copy), "%s", text); char* slash = strchr(copy, '/'); if (!slash) return -EINVAL; *slash = 0;
    struct in6_addr a; if (inet_pton(AF_INET6, copy, &a) != 1) return -EINVAL;
    int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE); if (nl < 0) return -errno;
    struct { struct nlmsghdr h; struct ifaddrmsg m; char attrs[64]; } q; memset(&q, 0, sizeof(q));
    q.h.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR; q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_EXCL : 0); q.h.nlmsg_seq = 1;
    q.m.ifa_family = AF_INET6; q.m.ifa_prefixlen = (unsigned char)atoi(slash + 1); q.m.ifa_index = if_nametoindex(device);
    struct rtattr* r = (void*)q.attrs; r->rta_type = IFA_LOCAL; r->rta_len = RTA_LENGTH(16); memcpy(RTA_DATA(r), &a, 16);
    unsigned used = RTA_ALIGN(r->rta_len);
    if (flags) { r = (void*)(q.attrs + used); r->rta_type = IFA_FLAGS; r->rta_len = RTA_LENGTH(4); memcpy(RTA_DATA(r), &flags, 4); used += RTA_ALIGN(r->rta_len); }
    q.h.nlmsg_len = NLMSG_LENGTH(sizeof(q.m)) + used;
    send(nl, &q, q.h.nlmsg_len, 0);
    char buffer[4096]; ssize_t n = recv(nl, buffer, sizeof(buffer), 0); close(nl);
    if (n < (ssize_t)sizeof(struct nlmsghdr)) return -EIO;
    struct nlmsghdr* h = (void*)buffer; if (h->nlmsg_type != NLMSG_ERROR) return -EIO;
    return ((struct nlmsgerr*)NLMSG_DATA(h))->error;
}
/* RUN <seconds> <program> <arguments...>: fork/exec with stdout+stderr captured ("%20" in an argument is a blank);
 * killed after <seconds>. Reply: "EXIT <status>\n<output>". */
static void run(char* line) {
    char* words[64]; int count = 0;
    for (char* save = NULL, *w = strtok_r(line, " ", &save); w && count < 63; w = strtok_r(NULL, " ", &save)) {
        char* out = w; for (char* in = w; *in; ) { if (!strncmp(in, "%20", 3)) { *out++ = ' '; in += 3; } else *out++ = *in++; } *out = 0;
        words[count++] = w;
    }
    words[count] = NULL;
    if (count < 2) { strcpy(reply, "ERR usage"); return; }
    int seconds = atoi(words[0]), pipefd[2]; if (pipe(pipefd)) { snprintf(reply, sizeof(reply), "ERR pipe %d", errno); return; }
    pid_t child = fork();
    if (child == 0) {
        dup2(pipefd[1], 1); dup2(pipefd[1], 2); close(pipefd[0]); close(pipefd[1]);
        int null = open("/dev/null", O_RDONLY); if (null >= 0) { dup2(null, 0); close(null); }
        char* environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/home/nexus", "LANG=C", "TERM=dumb", NULL};
        execve(words[1], words + 1, environment); printf("exec %s: errno %d\n", words[1], errno); _exit(127);
    }
    close(pipefd[1]);
    size_t used = (size_t)snprintf(reply, sizeof(reply), "EXIT ");
    char output[50000]; size_t got = 0; int status = -1;
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct pollfd p = {pipefd[0], POLLIN, 0}; int ready = poll(&p, 1, 200);
        if (ready > 0) { ssize_t n = read(pipefd[0], output + got, sizeof(output) - 1 - got); if (n > 0) { got += (size_t)n; if (got >= sizeof(output) - 1) got = sizeof(output) - 2; continue; } }
        if (waitpid(child, &status, WNOHANG) == child) {
            for (;;) { ssize_t n = read(pipefd[0], output + got, sizeof(output) - 1 - got); if (n <= 0 || got + 1 >= sizeof(output)) break; got += (size_t)n; }
            break;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= seconds) { kill(child, SIGTERM); poll(NULL, 0, 300); kill(child, SIGKILL); waitpid(child, &status, 0); break; }
    }
    close(pipefd[0]); output[got] = 0;
    used += (size_t)snprintf(reply + used, sizeof(reply) - used, "%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    snprintf(reply + used, sizeof(reply) - used, "%s", output);
}
/* SPAWN <program> <arguments...>: start it in the background, output to the console ("%20" = a blank); "PID <pid>" */
static void spawn(char* line) {
    char* words[64]; int count = 0;
    for (char* save = NULL, *w = strtok_r(line, " ", &save); w && count < 63; w = strtok_r(NULL, " ", &save)) {
        char* out = w; for (char* in = w; *in; ) { if (!strncmp(in, "%20", 3)) { *out++ = ' '; in += 3; } else *out++ = *in++; } *out = 0;
        words[count++] = w;
    }
    words[count] = NULL;
    if (count < 1) { strcpy(reply, "ERR usage"); return; }
    pid_t child = fork();
    if (child == 0) {
        setsid();
        char* environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/home/nexus", "LANG=C", NULL};
        execve(words[0], words, environment); printf("exec %s: errno %d\n", words[0], errno); _exit(127);
    }
    snprintf(reply, sizeof(reply), child > 0 ? "PID %d" : "ERR %d", child > 0 ? child : errno);
}
static void command(char* text) {
    if (!strncmp(text, "RUN ", 4)) { run(text + 4); return; }
    if (!strncmp(text, "SPAWN ", 6)) { spawn(text + 6); return; }
    if (!strncmp(text, "PUT ", 4) || !strncmp(text, "PUTA ", 5)) {   /* PUT|PUTA <path> <hex bytes>: write / append */
        int append = text[3] == 'A'; text += append ? 5 : 4;
        char path[256]; const char* hex = NULL; if (sscanf(text, "%255s", path) != 1 || !(hex = strchr(text, ' '))) { strcpy(reply, "ERR usage"); return; }
        hex++; int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644); if (fd < 0) { snprintf(reply, sizeof(reply), "ERR %d", errno); return; }
        size_t n = 0; static unsigned char data[2048];
        for (const char* h = hex; h[0] && h[1] && n < sizeof(data); h += 2) { unsigned v; if (sscanf(h, "%2x", &v) != 1) break; data[n++] = (unsigned char)v; }
        ssize_t w = write(fd, data, n); close(fd); snprintf(reply, sizeof(reply), w == (ssize_t)n ? "OK" : "ERR %d", errno); return;
    }
    char verb[32] = {0}, a1[200] = {0}, a2[200] = {0}, a3[200] = {0};
    sscanf(text, "%31s %199s %199s %199s", verb, a1, a2, a3);
    reply[0] = 0;
    if (!strcmp(verb, "FILE")) slurp(a1);
    else if (!strcmp(verb, "MONCAP")) {   /* MONCAP <ifname> <seconds>: capture one frame on a SOCK_RAW packet socket, report sll_hatype + hex */
        int ifindex = (int)if_nametoindex(a1); int seconds = atoi(a2[0] ? a2 : "5");
        if (!ifindex) { strcpy(reply, "ERR noif"); return; }
        int fd = socket(AF_PACKET, SOCK_RAW, htons(0x0003 /* ETH_P_ALL */)); if (fd < 0) { snprintf(reply, sizeof(reply), "ERR socket %d", errno); return; }
        struct sockaddr_ll ll; memset(&ll, 0, sizeof(ll)); ll.sll_family = AF_PACKET; ll.sll_protocol = htons(0x0003); ll.sll_ifindex = ifindex;
        if (bind(fd, (void*)&ll, sizeof(ll))) { snprintf(reply, sizeof(reply), "ERR bind %d", errno); close(fd); return; }
        unsigned char buf[2048]; struct sockaddr_ll from; socklen_t fl = sizeof(from);
        struct pollfd pf = {fd, POLLIN, 0}; int frames = 0; uint16_t hatype = 0; size_t got = 0;
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); long left = seconds * 1000L - ((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
            if (left <= 0) break;
            if (poll(&pf, 1, (int)left) <= 0) break;
            fl = sizeof(from); ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (void*)&from, &fl);
            if (n < 0) break;
            if (!frames) { hatype = from.sll_hatype; got = (size_t)n; }   /* sll_hatype is host order (ARPHRD_*) */
            frames++;
        }
        close(fd);
        size_t r = (size_t)snprintf(reply, sizeof(reply), "MONCAP frames %d hatype %u first ", frames, hatype);
        for (size_t i = 0; i < got && r + 2 < sizeof(reply); i++) r += (size_t)snprintf(reply + r, sizeof(reply) - r, "%02x", buf[i]);
    }
    else if (!strcmp(verb, "ARPING")) {   /* ARPING <ifname> <source> <target> [count]: ARP requests on a packet socket, replies counted */
        int ifindex = (int)if_nametoindex(a1); struct in_addr src, dst; int count = a3[0] ? 3 : 0;
        char a4[16] = {0}; sscanf(text, "%*s %*s %*s %*s %15s", a4); if (a4[0]) count = atoi(a4);
        if (!ifindex || inet_pton(AF_INET, a2, &src) != 1 || inet_pton(AF_INET, a3, &dst) != 1 || count < 1 || count > 20) { strcpy(reply, "ERR usage"); return; }
        int fd = socket(AF_PACKET, SOCK_DGRAM, htons(0x0806)); if (fd < 0) { snprintf(reply, sizeof(reply), "ERR socket %d", errno); return; }
        struct ifreq ifr; memset(&ifr, 0, sizeof(ifr)); snprintf(ifr.ifr_name, IFNAMSIZ, "%s", a1);
        if (ioctl(fd, SIOCGIFHWADDR, &ifr)) { snprintf(reply, sizeof(reply), "ERR hwaddr %d", errno); close(fd); return; }
        struct sockaddr_ll ll; memset(&ll, 0, sizeof(ll)); ll.sll_family = AF_PACKET; ll.sll_protocol = htons(0x0806); ll.sll_ifindex = ifindex;
        if (bind(fd, (void*)&ll, sizeof(ll))) { snprintf(reply, sizeof(reply), "ERR bind %d", errno); close(fd); return; }
        int replies = 0; char from[32] = "-";
        for (int i = 0; i < count; i++) {
            unsigned char arp[28] = {0, 1, 8, 0, 6, 4, 0, 1};
            memcpy(arp + 8, ifr.ifr_hwaddr.sa_data, 6); memcpy(arp + 14, &src, 4); memcpy(arp + 24, &dst, 4);
            struct sockaddr_ll to = ll; to.sll_halen = 6; memset(to.sll_addr, 0xFF, 6);
            if (sendto(fd, arp, sizeof(arp), 0, (void*)&to, sizeof(to)) != (ssize_t)sizeof(arp)) { snprintf(reply, sizeof(reply), "ERR send %d", errno); close(fd); return; }
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            for (;;) {
                clock_gettime(CLOCK_MONOTONIC, &t1); long left = 1000 - ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
                if (left <= 0) break;
                struct pollfd pf = {fd, POLLIN, 0}; if (poll(&pf, 1, (int)left) <= 0) break;
                unsigned char in[64]; ssize_t n = recv(fd, in, sizeof(in), 0);
                if (n >= 28 && in[7] == 2 && !memcmp(in + 14, &dst, 4) && !memcmp(in + 24, &src, 4)) {
                    replies++; snprintf(from, sizeof(from), "%02x:%02x:%02x:%02x:%02x:%02x", in[8], in[9], in[10], in[11], in[12], in[13]); break;
                }
            }
        }
        close(fd); snprintf(reply, sizeof(reply), "ARPING sent %d replies %d from %s", count, replies, from);
    }
    else if (!strcmp(verb, "KILL")) { int r = kill(atoi(a1), a2[0] ? atoi(a2) : SIGTERM); snprintf(reply, sizeof(reply), r ? "ERR %d" : "OK", errno); }   /* KILL <pid> [signal] */
    else if (!strcmp(verb, "TAIL") || !strcmp(verb, "GREP")) {   /* TAIL <path> [bytes] / GREP <path> <text>: the end of a log, its matching lines */
        int fd = open(a1, O_RDONLY); if (fd < 0) { snprintf(reply, sizeof(reply), "ERR %d", errno); return; }
        static char text[1 << 20]; size_t used = 0; ssize_t n;
        while (used + 1 < sizeof(text) && (n = read(fd, text + used, sizeof(text) - 1 - used)) > 0) used += (size_t)n;
        text[used] = 0; close(fd);
        if (verb[0] == 'T') { size_t want = a2[0] ? (size_t)atol(a2) : sizeof(reply) - 1; if (want > sizeof(reply) - 1) want = sizeof(reply) - 1;
                              snprintf(reply, sizeof(reply), "%s", text + (used > want ? used - want : 0)); }
        else {
            char pattern[200]; char* o = pattern; for (char* i = a2; *i && o < pattern + sizeof(pattern) - 1; ) { if (!strncmp(i, "%20", 3)) { *o++ = ' '; i += 3; } else *o++ = *i++; } *o = 0;
            size_t r = 0; reply[0] = 0;
            for (char* line = text; *line; ) {
                char* end = strchr(line, '\n'); size_t len = end ? (size_t)(end - line) : strlen(line);
                char saved = line[len]; line[len] = 0;
                if (strstr(line, pattern) && r + len + 2 < sizeof(reply)) { memcpy(reply + r, line, len); r += len; reply[r++] = '\n'; reply[r] = 0; }
                line[len] = saved; line += len + (end ? 1 : 0);
            }
        }
    }
    else if (!strcmp(verb, "WRITE")) {
        int fd = open(a1, O_WRONLY); if (fd < 0) { snprintf(reply, sizeof(reply), "ERR %d", errno); return; }
        const char* value = !strcmp(a2, "EMPTY") ? "\n" : a2;   /* (commands carry no blanks) */
        ssize_t n = write(fd, value, strlen(value)); snprintf(reply, sizeof(reply), n < 0 ? "ERR %d" : "OK", errno); close(fd);
    } else if (!strcmp(verb, "ADDR") || !strcmp(verb, "DELADDR")) {
        int error = netlink_addr(!strcmp(verb, "ADDR"), a1, a2, (unsigned)strtoul(a3[0] ? a3 : "0", NULL, 0)); snprintf(reply, sizeof(reply), "RESULT %d", error);
    } else if (!strcmp(verb, "UDP")) {   /* UDP <address> <port> <size> */
        struct sockaddr_in6 to; if (parse(a1, &to)) { strcpy(reply, "ERR bad address"); return; }
        to.sin6_port = htons((uint16_t)atoi(a2)); static char data[65536]; size_t size = (size_t)atoi(a3); for (size_t i = 0; i < size; i++) data[i] = (char)(i * 3 + 1);
        int fd = socket(AF_INET6, SOCK_DGRAM, 0); ssize_t n = sendto(fd, data, size, 0, (void*)&to, sizeof(to));
        snprintf(reply, sizeof(reply), "RESULT %zd %d", n, n < 0 ? errno : 0); close(fd);
    } else if (!strcmp(verb, "UDP4")) {   /* UDP4 <address> <port> <size>: an AF_INET socket, connected */
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(a2))};
        if (inet_pton(AF_INET, a1, &to.sin_addr) != 1) { strcpy(reply, "ERR bad address"); return; }
        static char data[65536]; size_t size = (size_t)atoi(a3); memset(data, 'u', size);
        int fd = socket(AF_INET, SOCK_DGRAM, 0), r = connect(fd, (void*)&to, sizeof(to)), error = r ? errno : 0;
        ssize_t n = r ? -1 : send(fd, data, size, 0);
        snprintf(reply, sizeof(reply), "RESULT %d %zd %d", error, n, n < 0 ? errno : 0); close(fd);
    } else if (!strcmp(verb, "UDPERR")) {   /* connected socket with IPV6_RECVERR: send, wait for an error, report it and IPV6_MTU */
        struct sockaddr_in6 to; if (parse(a1, &to)) { strcpy(reply, "ERR bad address"); return; }
        to.sin6_port = htons((uint16_t)atoi(a2)); int fd = socket(AF_INET6, SOCK_DGRAM, 0), one = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one)); connect(fd, (void*)&to, sizeof(to));
        static char data[1400]; send(fd, data, (size_t)atoi(a3), 0);
        struct pollfd p = {fd, POLLERR, 0}; int ready = poll(&p, 1, 7000);
        char control[256], payload[64]; struct iovec v = {payload, sizeof(payload)}; struct sockaddr_in6 from;
        struct msghdr m = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        ssize_t n = ready > 0 ? recvmsg(fd, &m, MSG_ERRQUEUE) : -1; int number = -1, type = -1, code = -1; unsigned info = 0;
        for (struct cmsghdr* c = n >= 0 ? CMSG_FIRSTHDR(&m) : NULL; c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_RECVERR) {
                struct { uint32_t errno_; uint8_t origin, type, code, pad; uint32_t info, data; } e; memcpy(&e, CMSG_DATA(c), sizeof(e));
                number = (int)e.errno_; type = e.type; code = e.code; info = e.info;
            }
        int mtu = 0; socklen_t size = sizeof(mtu); getsockopt(fd, IPPROTO_IPV6, IPV6_MTU, &mtu, &size);
        snprintf(reply, sizeof(reply), "RESULT %d %d %d %u %d", number, type, code, info, mtu); close(fd);
    } else if (!strcmp(verb, "TCP")) {   /* TCP <address> <port>: connect with a 12 s timeout */
        struct sockaddr_in6 to; if (parse(a1, &to)) { strcpy(reply, "ERR bad address"); return; }
        to.sin6_port = htons((uint16_t)atoi(a2)); int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
        int r = connect(fd, (void*)&to, sizeof(to)), error = r ? errno : 0;
        if (r && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0}; poll(&p, 1, 12000); socklen_t n = sizeof(error); getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n);
            if (!(p.revents & (POLLOUT | POLLERR))) error = ETIMEDOUT;
        }
        snprintf(reply, sizeof(reply), "RESULT %d", error); close(fd);
    } else if (!strcmp(verb, "HTTP")) {   /* HTTP <address> <port> <path>: GET over TCP, reply "RESULT <error> <bytes>\n<head of the response>" */
        struct sockaddr_in6 to; if (parse(a1, &to)) { strcpy(reply, "ERR bad address"); return; }
        to.sin6_port = htons((uint16_t)atoi(a2)); int fd = socket(AF_INET6, SOCK_STREAM, 0), error = 0; ssize_t total = 0;
        struct timeval tv = {10, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        static char body[16384];
        if (connect(fd, (void*)&to, sizeof(to))) error = errno;
        else {
            char request[300]; int n = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", a3[0] ? a3 : "/", a1);
            if (send(fd, request, (size_t)n, 0) != n) error = errno;
            for (ssize_t got; !error && total + 1 < (ssize_t)sizeof(body) && (got = recv(fd, body + total, sizeof(body) - 1 - (size_t)total, 0)) > 0;) total += got;
        }
        body[total > 0 ? total : 0] = 0;
        snprintf(reply, sizeof(reply), "RESULT %d %zd\n%.4000s", error, total, body); close(fd);
    } else if (!strcmp(verb, "PING")) {
        struct sockaddr_in6 to; if (parse(a1, &to)) { strcpy(reply, "ERR bad address"); return; }
        int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6); unsigned char q[64] = {128, 0, 0, 0, 0, 0, 0, 7};
        sendto(fd, q, (size_t)(a2[0] ? atoi(a2) : 64), 0, (void*)&to, sizeof(to));
        struct pollfd p = {fd, POLLIN, 0}; int ready = poll(&p, 1, 3000); unsigned char back[2048];
        ssize_t n = ready > 0 ? recv(fd, back, sizeof(back), 0) : -1;
        snprintf(reply, sizeof(reply), "RESULT %zd %d", n, n > 0 ? back[0] : -1); close(fd);
    } else if (!strcmp(verb, "JOIN")) {
        static int joined = -1; if (joined < 0) joined = socket(AF_INET6, SOCK_DGRAM, 0);
        struct ipv6_mreq m; inet_pton(AF_INET6, a1, &m.ipv6mr_multiaddr); m.ipv6mr_interface = if_nametoindex("eth0");
        int r = setsockopt(joined, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m, sizeof(m)); snprintf(reply, sizeof(reply), "RESULT %d", r ? errno : 0);
    } else if (!strcmp(verb, "RESOLVE")) {
        /* getaddrinfo through the kernel's resolv.conf */
        struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *result = NULL;
        int r = getaddrinfo(a1, "80", &hints, &result); size_t used = (size_t)snprintf(reply, sizeof(reply), "RESULT %d", r);
        for (struct addrinfo* i = result; i && used + 64 < sizeof(reply); i = i->ai_next) {
            char text[64]; void* address = i->ai_family == AF_INET6 ? (void*)&((struct sockaddr_in6*)i->ai_addr)->sin6_addr : (void*)&((struct sockaddr_in*)i->ai_addr)->sin_addr;
            inet_ntop(i->ai_family, address, text, sizeof(text)); used += (size_t)snprintf(reply + used, sizeof(reply) - used, " %s", text);
        }
        if (result) freeaddrinfo(result);
    } else if (!strcmp(verb, "ADDRS")) {   /* RTM_GETADDR dump: address/prefix flags preferred valid, one per line */
        int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        int family = a1[0] == '4' ? AF_INET : AF_INET6;
        struct { struct nlmsghdr h; struct ifaddrmsg m; } q = {{sizeof(q), RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP, 9, 0}, {(unsigned char)family, 0, 0, 0, 0}};
        send(nl, &q, sizeof(q), 0); size_t used = 0; int done = 0; static char buffer[32768];
        while (!done) {
            ssize_t n = recv(nl, buffer, sizeof(buffer), 0); if (n <= 0) break;
            for (struct nlmsghdr* h = (void*)buffer; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
                if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
                struct ifaddrmsg* m = NLMSG_DATA(h); char text[64] = "?"; unsigned flags = m->ifa_flags; struct ifa_cacheinfo ci = {0, 0, 0, 0};
                int length = (int)IFA_PAYLOAD(h);
                for (struct rtattr* r = IFA_RTA(m); RTA_OK(r, length); r = RTA_NEXT(r, length)) {
                    if (r->rta_type == IFA_ADDRESS) inet_ntop(family, RTA_DATA(r), text, sizeof(text));
                    if (r->rta_type == IFA_FLAGS) memcpy(&flags, RTA_DATA(r), 4);
                    if (r->rta_type == IFA_CACHEINFO) memcpy(&ci, RTA_DATA(r), sizeof(ci));
                }
                used += (size_t)snprintf(reply + used, sizeof(reply) - used, "%s/%u %u %x %u %u\n", text, m->ifa_prefixlen, m->ifa_index, flags, ci.ifa_prefered, ci.ifa_valid);
            }
        }
        close(nl);
    } else if (!strcmp(verb, "NEIGH")) {   /* RTM_GETNEIGH dump (IPv6): address state, one per line */
        int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        struct { struct nlmsghdr h; struct ndmsg m; } q = {{sizeof(q), RTM_GETNEIGH, NLM_F_REQUEST | NLM_F_DUMP, 10, 0}, {AF_INET6, 0, 0, 0, 0, 0, 0}};
        send(nl, &q, sizeof(q), 0); size_t used = 0; int done = 0; static char buffer[32768];
        while (!done) {
            ssize_t n = recv(nl, buffer, sizeof(buffer), 0); if (n <= 0) break;
            for (struct nlmsghdr* h = (void*)buffer; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
                if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
                struct ndmsg* m = NLMSG_DATA(h); char text[64] = "?"; int length = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*m)));
                for (struct rtattr* r = (void*)((char*)m + NLMSG_ALIGN(sizeof(*m))); RTA_OK(r, length); r = RTA_NEXT(r, length))
                    if (r->rta_type == NDA_DST) inet_ntop(AF_INET6, RTA_DATA(r), text, sizeof(text));
                used += (size_t)snprintf(reply + used, sizeof(reply) - used, "%s %x %x\n", text, m->ndm_state, m->ndm_flags);
            }
        }
        close(nl);
    } else if (!strcmp(verb, "EVENTS")) { drain_events(); snprintf(reply, sizeof(reply), "EVENTS\n%s", events); events_used = 0; events[0] = 0; }
    else if (!strcmp(verb, "SLEEP")) { poll(NULL, 0, atoi(a1)); strcpy(reply, "OK"); }
    else if (!strcmp(verb, "EXIT")) { strcpy(reply, "BYE"); }
    else snprintf(reply, sizeof(reply), "ERR unknown %s", verb);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sock6 = socket(AF_INET6, SOCK_DGRAM, 0); int zero = 0; setsockopt(sock6, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    struct sockaddr_in6 any = {.sin6_family = AF_INET6, .sin6_port = htons(7001)};
    if (bind(sock6, (void*)&any, sizeof(any))) { printf("[64] NETWIRE bind failed %d\n", errno); return 1; }
    events_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    struct sockaddr_nl groups = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR};
    if (events_fd < 0 || bind(events_fd, (void*)&groups, sizeof(groups))) { printf("[64] NETWIRE netlink monitor failed %d\n", errno); return 1; }
    puts("[64] NETWIRE AGENT READY");
    static char buffer[4096];
    for (;;) {
        struct pollfd ready[2] = {{sock6, POLLIN, 0}, {events_fd, POLLIN, 0}};
        if (poll(ready, 2, -1) <= 0) continue;
        if (ready[1].revents) drain_events();
        if (!(ready[0].revents & POLLIN)) continue;
        struct sockaddr_in6 from; socklen_t n = sizeof(from);
        ssize_t got = recvfrom(sock6, buffer, sizeof(buffer) - 1, 0, (void*)&from, &n);
        if (got <= 0) continue;
        buffer[got] = 0;
        printf("[64] NETWIRE command: %.80s\n", buffer);
        command(buffer);
        sendto(sock6, reply, strlen(reply), 0, (void*)&from, n);
        if (!strcmp(reply, "BYE")) break;
    }
    puts("[64] NETWIRE AGENT DONE");
    return 0;
}
