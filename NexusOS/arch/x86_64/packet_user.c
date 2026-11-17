/* AF_PACKET campaign (tests/test_x64_packet.py, kernel campaign /nexus/test-packet): raw and cooked capture of
 * real traffic to QEMU's user-mode network (ping to the gateway, ARP), sending ARP requests from a packet socket,
 * classic BPF filters (accept/truncate/ancillary loads) and the checker's rejections, PACKET_AUXDATA, sockaddr_ll,
 * promiscuous membership bookkeeping, TPACKET_V2 and TPACKET_V3 mmap rings, statistics, /proc/net/packet and
 * error cases. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sched.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c, ...) do { if (!(c)) { printf("[64] FAIL: " __VA_ARGS__); printf(" (errno %d)\n", errno); exit(1); } } while (0)
static int eth0;
static unsigned char mac[6];
static const unsigned char gateway_ip[4] = {10, 0, 2, 2}, our_ip[4] = {10, 0, 2, 15};

static void ping_gateway(int sequence) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP); CHECK(fd >= 0, "ping socket");
    struct sockaddr_in to = {.sin_family = AF_INET}; memcpy(&to.sin_addr, gateway_ip, 4);
    unsigned char echo[64] = {8, 0, 0, 0, 0, 0, 0, (unsigned char)sequence};
    for (int i = 8; i < 64; i++) echo[i] = (unsigned char)(0xA0 + i);
    CHECK(sendto(fd, echo, sizeof(echo), 0, (void*)&to, sizeof(to)) == sizeof(echo), "ping send");
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 3000) == 1, "ping reply");
    unsigned char back[256]; CHECK(recv(fd, back, sizeof(back), 0) == 64 && back[0] == 0, "echo reply");
    close(fd);
}
static int packet_socket(int type, int protocol) {
    int fd = socket(AF_PACKET, type, htons(protocol)); CHECK(fd >= 0, "socket(AF_PACKET, %d, %#x)", type, protocol);
    struct sockaddr_ll a = {.sll_family = AF_PACKET, .sll_protocol = htons(protocol), .sll_ifindex = eth0};
    CHECK(bind(fd, (void*)&a, sizeof(a)) == 0, "bind eth0");
    return fd;
}
static void drain(int fd) { char b[2048]; while (recv(fd, b, sizeof(b), MSG_DONTWAIT) > 0) {} }
/* the next frame accepted by `want` within 3 s */
static ssize_t next_frame(int fd, unsigned char* buffer, size_t size, struct sockaddr_ll* from, int (*want)(const unsigned char*, ssize_t, const struct sockaddr_ll*)) {
    for (int tries = 0; tries < 200; tries++) {
        struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, 3000) != 1) return -1;
        socklen_t n = sizeof(*from); ssize_t got = recvfrom(fd, buffer, size, 0, (void*)from, &n);
        if (got < 0) return -1;
        CHECK(n == sizeof(struct sockaddr_ll), "sockaddr_ll length %u", n);
        if (want(buffer, got, from)) return got;
    }
    return -1;
}
static int raw_echo_request(const unsigned char* f, ssize_t n, const struct sockaddr_ll* a) {
    return n >= 42 && f[12] == 8 && f[13] == 0 && f[23] == 1 && f[34] == 8 && a->sll_pkttype == PACKET_OUTGOING;
}
static int raw_echo_reply(const unsigned char* f, ssize_t n, const struct sockaddr_ll* a) {
    return n >= 42 && f[12] == 8 && f[13] == 0 && f[23] == 1 && f[34] == 0 && a->sll_pkttype == PACKET_HOST;
}
static int cooked_reply(const unsigned char* f, ssize_t n, const struct sockaddr_ll* a) { return n >= 28 && f[0] == 0x45 && f[9] == 1 && f[20] == 0 && a->sll_pkttype == PACKET_HOST; }
static int arp_reply(const unsigned char* f, ssize_t n, const struct sockaddr_ll* a) { (void)a; return n >= 28 && f[7] == 2 && !memcmp(f + 14, gateway_ip, 4); }
static unsigned flags_of(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); struct ifreq q; memset(&q, 0, sizeof(q)); strcpy(q.ifr_name, "eth0");
    CHECK(ioctl(fd, SIOCGIFFLAGS, &q) == 0, "SIOCGIFFLAGS"); close(fd); return (unsigned short)q.ifr_flags;
}
static int attach(int fd, struct sock_filter* code, unsigned n) {
    struct sock_fprog program = {(unsigned short)n, code}; return setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program));
}

static void raw_capture(void) {
    int fd = packet_socket(SOCK_RAW, ETH_P_ALL); drain(fd);
    ping_gateway(1);
    unsigned char f[2048]; struct sockaddr_ll a;
    CHECK(next_frame(fd, f, sizeof(f), &a, raw_echo_request) > 0, "outgoing echo request captured");
    CHECK(a.sll_ifindex == eth0 && a.sll_hatype == 1 && a.sll_halen == 6 && !memcmp(a.sll_addr, mac, 6) && a.sll_protocol == htons(ETH_P_IP) &&
          !memcmp(f + 6, mac, 6) && !memcmp(f + 26, our_ip, 4) && !memcmp(f + 30, gateway_ip, 4) && f[42] == 0xA0 + 8, "request frame / sockaddr_ll");
    CHECK(next_frame(fd, f, sizeof(f), &a, raw_echo_reply) > 0, "incoming echo reply captured");
    CHECK(!memcmp(f, mac, 6) && !memcmp(f + 26, gateway_ip, 4), "reply frame addresses");
    struct tpacket_stats st; socklen_t n = sizeof(st);
    CHECK(getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &st, &n) == 0 && n == 8 && st.tp_packets >= 2, "statistics %u", st.tp_packets);
    n = sizeof(st); CHECK(getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &st, &n) == 0 && st.tp_packets < 3, "statistics reset on read");
    char text[4096] = {0}; int proc = open("/proc/net/packet", O_RDONLY); CHECK(proc >= 0, "/proc/net/packet");
    CHECK(read(proc, text, sizeof(text) - 1) > 0 && strstr(text, "0003   2") && strstr(text, "Iface"), "/proc/net/packet lists the socket:\n%s", text); close(proc);
    close(fd);
    puts("[64] PACKET RAW CAPTURE: outgoing + incoming frames, sockaddr_ll, statistics, /proc/net/packet");
}
static void cooked_and_auxdata(void) {
    int fd = packet_socket(SOCK_DGRAM, ETH_P_IP), on = 1; drain(fd);
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &on, sizeof(on)) == 0, "PACKET_AUXDATA");
    ping_gateway(2);
    unsigned char f[2048]; struct sockaddr_ll a;
    for (int tries = 0;; tries++) {
        CHECK(tries < 50, "cooked echo reply");
        struct iovec v = {f, sizeof(f)}; char control[128];
        struct msghdr m = {.msg_name = &a, .msg_namelen = sizeof(a), .msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 3000) == 1, "cooked frame");
        ssize_t got = recvmsg(fd, &m, 0); CHECK(got > 0, "recvmsg");
        if (!cooked_reply(f, got, &a)) continue;
        struct tpacket_auxdata* aux = NULL;
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_PACKET && c->cmsg_type == PACKET_AUXDATA) aux = (void*)CMSG_DATA(c);
        CHECK(aux && aux->tp_len == (unsigned)got && aux->tp_snaplen == (unsigned)got && (aux->tp_status & TP_STATUS_USER), "auxdata");
        CHECK(a.sll_protocol == htons(ETH_P_IP) && a.sll_halen == 6 && a.sll_ifindex == eth0, "cooked sockaddr_ll");
        break;
    }
    close(fd);
    puts("[64] PACKET COOKED: SOCK_DGRAM ETH_P_IP network-layer packets with PACKET_AUXDATA");
}
static void arp_exchange(void) {
    int tx = packet_socket(SOCK_RAW, ETH_P_ARP), rx = packet_socket(SOCK_DGRAM, ETH_P_ARP); drain(rx);
    unsigned char frame[42] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(frame + 6, mac, 6); frame[12] = 8; frame[13] = 6;
    unsigned char arp[28] = {0, 1, 8, 0, 6, 4, 0, 1}; memcpy(arp + 8, mac, 6); memcpy(arp + 14, our_ip, 4); memcpy(arp + 24, gateway_ip, 4);
    memcpy(frame + 14, arp, 28);
    CHECK(send(tx, frame, sizeof(frame), 0) == sizeof(frame), "raw ARP request");
    unsigned char f[256]; struct sockaddr_ll a;
    CHECK(next_frame(rx, f, sizeof(f), &a, arp_reply) > 0, "ARP reply from the gateway");
    CHECK(a.sll_protocol == htons(ETH_P_ARP) && a.sll_halen == 6 && !memcmp(f + 8, a.sll_addr, 6), "ARP sockaddr_ll");
    /* SOCK_DGRAM send: the kernel builds the Ethernet header from sll_addr / sll_protocol */
    struct sockaddr_ll to = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ARP), .sll_ifindex = eth0, .sll_halen = 6};
    memset(to.sll_addr, 0xFF, 6); drain(rx);
    int dgram = socket(AF_PACKET, SOCK_DGRAM, 0); CHECK(dgram >= 0, "cooked sender");
    CHECK(sendto(dgram, arp, sizeof(arp), 0, (void*)&to, sizeof(to)) == sizeof(arp), "cooked ARP request");
    CHECK(next_frame(rx, f, sizeof(f), &a, arp_reply) > 0, "ARP reply to the cooked request");
    /* errors */
    CHECK(send(dgram, arp, sizeof(arp), 0) < 0 && errno == ENXIO, "send without an interface: ENXIO");
    static unsigned char huge[4000];
    CHECK(send(tx, huge, sizeof(huge), 0) < 0 && errno == EMSGSIZE, "oversized frame: EMSGSIZE");
    CHECK(send(tx, huge, 6, 0) < 0 && errno == EINVAL, "frame shorter than its header: EINVAL");
    struct sockaddr_ll bad = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = 999};
    CHECK(bind(dgram, (void*)&bad, sizeof(bad)) < 0 && errno == ENODEV, "bind to a missing interface: ENODEV");
    close(dgram); close(tx); close(rx);
    puts("[64] PACKET ARP: raw and cooked sends answered by the gateway; ENXIO/EMSGSIZE/EINVAL/ENODEV");
}
static void filters(void) {
    int fd = packet_socket(SOCK_RAW, ETH_P_ALL);
    struct sock_filter icmp_only[] = {
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_IP, 0, 3),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 23), BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_ICMP, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 60), BPF_STMT(BPF_RET | BPF_K, 0)};
    CHECK(attach(fd, icmp_only, 6) == 0, "attach ICMP filter"); drain(fd);
    int udp = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9)}; memcpy(&to.sin_addr, gateway_ip, 4);
    CHECK(sendto(udp, "filtered", 8, 0, (void*)&to, sizeof(to)) == 8, "UDP datagram"); close(udp);
    ping_gateway(3);
    unsigned char f[2048]; struct sockaddr_ll a; int seen = 0;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, 500) != 1) break;
        struct iovec v = {f, sizeof(f)}; struct msghdr m = {.msg_name = &a, .msg_namelen = sizeof(a), .msg_iov = &v, .msg_iovlen = 1};
        ssize_t got = recvmsg(fd, &m, MSG_TRUNC);
        CHECK(got == 60 && f[23] == 1, "only ICMP, truncated to the filter's 60 bytes (got %zd proto %d)", got, f[23]); seen++;
    }
    CHECK(seen >= 2, "filtered frames %d", seen);
    /* ancillary loads: only outgoing frames */
    struct sock_filter outgoing[] = {BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SKF_AD_OFF + SKF_AD_PKTTYPE),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PACKET_OUTGOING, 0, 1), BPF_STMT(BPF_RET | BPF_K, 0xFFFF), BPF_STMT(BPF_RET | BPF_K, 0)};
    CHECK(attach(fd, outgoing, 4) == 0, "attach pkttype filter"); drain(fd);
    ping_gateway(4);
    int out = 0;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, 500) != 1) break;
        socklen_t n = sizeof(a); CHECK(recvfrom(fd, f, sizeof(f), 0, (void*)&a, &n) > 0 && a.sll_pkttype == PACKET_OUTGOING, "pkttype filter"); out++;
    }
    CHECK(out >= 1, "outgoing frames %d", out);
    /* the checker */
    struct sock_filter no_return[] = {BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0)};
    struct sock_filter wild_jump[] = {BPF_JUMP(BPF_JMP | BPF_JA, 5, 0, 0), BPF_STMT(BPF_RET | BPF_K, 0)};
    struct sock_filter wild_branch[] = {BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 7, 0), BPF_STMT(BPF_RET | BPF_K, 0)};
    struct sock_filter divide[] = {BPF_STMT(BPF_ALU | BPF_DIV | BPF_K, 0), BPF_STMT(BPF_RET | BPF_K, 0)};
    struct sock_filter memory[] = {BPF_STMT(BPF_ST, 16), BPF_STMT(BPF_RET | BPF_K, 0)};
    struct sock_filter opcode[] = {{0xFF, 0, 0, 0}, BPF_STMT(BPF_RET | BPF_K, 0)};
    CHECK(attach(fd, no_return, 1) < 0 && errno == EINVAL, "no final return rejected");
    CHECK(attach(fd, wild_jump, 2) < 0 && errno == EINVAL, "jump past the end rejected");
    CHECK(attach(fd, wild_branch, 2) < 0 && errno == EINVAL, "branch past the end rejected");
    CHECK(attach(fd, divide, 2) < 0 && errno == EINVAL, "division by constant zero rejected");
    CHECK(attach(fd, memory, 2) < 0 && errno == EINVAL, "scratch memory index 16 rejected");
    CHECK(attach(fd, opcode, 2) < 0 && errno == EINVAL, "unknown opcode rejected");
    CHECK(attach(fd, icmp_only, 0) < 0 && errno == EINVAL, "empty program rejected");
    struct sock_filter division_x[] = {BPF_STMT(BPF_LDX | BPF_IMM, 0), BPF_STMT(BPF_ALU | BPF_DIV | BPF_X, 0), BPF_STMT(BPF_RET | BPF_K, 0xFFFF)};
    CHECK(attach(fd, division_x, 3) == 0, "runtime division by X"); drain(fd); ping_gateway(5);
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 700) == 0, "division by zero at run time drops the frame");
    int one = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LOCK_FILTER, &one, sizeof(one)) == 0, "SO_LOCK_FILTER");
    CHECK(setsockopt(fd, SOL_SOCKET, SO_DETACH_FILTER, &one, sizeof(one)) < 0 && errno == EPERM, "locked filter cannot be detached");
    CHECK(attach(fd, outgoing, 4) < 0 && errno == EPERM, "locked filter cannot be replaced");
    close(fd);
    puts("[64] PACKET BPF: protocol filter + snap length, SKF_AD_PKTTYPE, checker rejections, runtime division, SO_LOCK_FILTER");
}
static void promiscuous(void) {
    unsigned before = flags_of();
    CHECK(!(before & IFF_PROMISC), "eth0 starts non-promiscuous");
    int a = packet_socket(SOCK_RAW, ETH_P_ALL), b = packet_socket(SOCK_RAW, ETH_P_ALL);
    struct packet_mreq m = {.mr_ifindex = eth0, .mr_type = PACKET_MR_PROMISC};
    CHECK(setsockopt(a, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &m, sizeof(m)) == 0 && setsockopt(b, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &m, sizeof(m)) == 0, "promiscuous membership");
    CHECK(flags_of() & IFF_PROMISC, "IFF_PROMISC set");
    CHECK(setsockopt(a, SOL_PACKET, PACKET_DROP_MEMBERSHIP, &m, sizeof(m)) == 0, "drop membership");
    CHECK(setsockopt(a, SOL_PACKET, PACKET_DROP_MEMBERSHIP, &m, sizeof(m)) < 0 && errno == EADDRNOTAVAIL, "dropping twice: EADDRNOTAVAIL");
    CHECK(flags_of() & IFF_PROMISC, "still promiscuous for the other socket");
    close(b);
    CHECK(!(flags_of() & IFF_PROMISC), "closing the socket undoes its membership");
    struct packet_mreq bad = {.mr_ifindex = 999, .mr_type = PACKET_MR_PROMISC};
    CHECK(setsockopt(a, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &bad, sizeof(bad)) < 0 && errno == ENODEV, "membership on a missing interface");
    close(a);
    puts("[64] PACKET PROMISC: membership counting, IFF_PROMISC, undone on close");
}
static void ring_v2(void) {
    int fd = packet_socket(SOCK_RAW, ETH_P_ALL), version = TPACKET_V2;
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) == 0, "TPACKET_V2");
    int hdrlen = TPACKET_V2; socklen_t n = sizeof(hdrlen);
    CHECK(getsockopt(fd, SOL_PACKET, PACKET_HDRLEN, &hdrlen, &n) == 0 && hdrlen == TPACKET2_HDRLEN, "PACKET_HDRLEN %d", hdrlen);
    struct tpacket_req bad = {4096, 4, 2008, 8};   /* not a multiple of TPACKET_ALIGNMENT */
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &bad, sizeof(bad)) < 0 && errno == EINVAL, "unaligned frame size rejected");
    struct tpacket_req mismatch = {4096, 4, 2048, 9};
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &mismatch, sizeof(mismatch)) < 0 && errno == EINVAL, "frame count mismatch rejected");
    struct tpacket_req huge = {1 << 22, 4096, 1 << 22, 4096};
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &huge, sizeof(huge)) < 0, "a 16 GiB ring is refused");
    struct tpacket_req req = {4096, 8, 2048, 16};
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) == 0, "V2 ring");
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0 && errno == EBUSY, "version change with a ring: EBUSY");
    unsigned char* ring = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(ring != MAP_FAILED, "mmap V2 ring");
    CHECK(mmap(NULL, 16 * 4096, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED, "mapping beyond the ring refused");
    ping_gateway(6);
    unsigned frame = 0, requests = 0, replies = 0;
    for (int rounds = 0; rounds < 40 && !(requests && replies); rounds++) {
        struct tpacket2_hdr* h = (void*)(ring + frame * 2048);
        if (!(h->tp_status & TP_STATUS_USER)) { struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 2000) == 1, "V2 poll"); continue; }
        unsigned char* f = (unsigned char*)h + h->tp_mac; struct sockaddr_ll* a = (void*)((unsigned char*)h + TPACKET_ALIGN(sizeof(*h)));
        CHECK(h->tp_snaplen == h->tp_len && h->tp_len >= 42 && h->tp_net == h->tp_mac + 14 && a->sll_ifindex == eth0 && h->tp_sec > 0, "V2 frame header");
        if (f[12] == 8 && f[13] == 0 && f[23] == 1) { if (f[34] == 8) requests++; else if (f[34] == 0) replies++; }
        h->tp_status = TP_STATUS_KERNEL; __sync_synchronize(); frame = (frame + 1) % 16;
    }
    CHECK(requests && replies, "V2 ring captured the ping (%u/%u)", requests, replies);
    struct tpacket_req teardown = {0, 0, 0, 0};
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &teardown, sizeof(teardown)) < 0 && errno == EBUSY, "ring busy while mapped");
    munmap(ring, 8 * 4096); close(fd);
    puts("[64] PACKET TPACKET_V2: mmap ring frames, header offsets, parameter checks");
}
static void ring_v3(void) {
    int fd = packet_socket(SOCK_RAW, ETH_P_ALL), version = TPACKET_V3;
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) == 0, "TPACKET_V3");
    int reserve = 4; CHECK(setsockopt(fd, SOL_PACKET, PACKET_RESERVE, &reserve, sizeof(reserve)) == 0, "PACKET_RESERVE");
    struct tpacket_req3 req = {.tp_block_size = 16384, .tp_block_nr = 4, .tp_frame_size = 2048, .tp_frame_nr = 32, .tp_retire_blk_tov = 20};
    CHECK(setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) == 0, "V3 ring");
    unsigned char* ring = mmap(NULL, 4 * 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(ring != MAP_FAILED, "mmap V3 ring");
    ping_gateway(7); ping_gateway(8);
    unsigned block = 0, packets = 0, echo = 0, blocks = 0;
    for (int rounds = 0; rounds < 40 && echo < 4; rounds++) {
        struct tpacket_block_desc* d = (void*)(ring + block * 16384);
        if (!(d->hdr.bh1.block_status & TP_STATUS_USER)) { struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 2000) == 1, "V3 poll"); continue; }
        CHECK(d->version == TPACKET_V3 && d->hdr.bh1.num_pkts > 0 && d->hdr.bh1.offset_to_first_pkt >= 48 && d->hdr.bh1.blk_len <= 16384, "V3 block header");
        struct tpacket3_hdr* h = (void*)((unsigned char*)d + d->hdr.bh1.offset_to_first_pkt);
        for (unsigned i = 0; i < d->hdr.bh1.num_pkts; i++) {
            unsigned char* f = (unsigned char*)h + h->tp_mac;
            CHECK(h->tp_snaplen == h->tp_len && h->tp_net == h->tp_mac + 14 && h->tp_mac >= TPACKET3_HDRLEN + 4 && (h->tp_status & TP_STATUS_USER), "V3 packet header");
            if (f[12] == 8 && f[13] == 0 && f[23] == 1) echo++;
            packets++;
            if (i + 1 < d->hdr.bh1.num_pkts) { CHECK(h->tp_next_offset, "next offset"); h = (void*)((unsigned char*)h + h->tp_next_offset); }
            else CHECK(!h->tp_next_offset, "last packet's next offset is 0");
        }
        blocks++; d->hdr.bh1.block_status = TP_STATUS_KERNEL; __sync_synchronize(); block = (block + 1) % 4;
    }
    CHECK(echo >= 4, "V3 ring captured both pings (%u echo frames in %u packets / %u blocks)", echo, packets, blocks);
    struct tpacket_stats_v3 st; socklen_t n = sizeof(st);
    CHECK(getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &st, &n) == 0 && n == sizeof(st) && st.tp_packets >= 4, "V3 statistics");
    munmap(ring, 4 * 16384); close(fd);
    printf("[64] PACKET TPACKET_V3: %u packets in %u retired blocks, PACKET_RESERVE, tpacket_stats_v3\n", packets, blocks);
}
int main(int argc, char** argv) {
    (void)argc; (void)argv; setvbuf(stdout, NULL, _IONBF, 0);
    eth0 = (int)if_nametoindex("eth0"); CHECK(eth0 > 0, "eth0");
    int fd = socket(AF_INET, SOCK_DGRAM, 0); struct ifreq q; memset(&q, 0, sizeof(q)); strcpy(q.ifr_name, "eth0");
    CHECK(ioctl(fd, SIOCGIFHWADDR, &q) == 0, "SIOCGIFHWADDR"); memcpy(mac, q.ifr_hwaddr.sa_data, 6); close(fd);
    CHECK(socket(AF_PACKET, SOCK_PACKET, htons(ETH_P_ALL)) < 0, "SOCK_PACKET is not offered");
    pid_t child = fork(); CHECK(child >= 0, "fork");
    if (!child) { if (unshare(CLONE_NEWUSER)) _exit(2); int r = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)); _exit(r == -1 && errno == EPERM ? 0 : 3); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "AF_PACKET refused inside a user namespace (%#x)", status);
    int knob = open("/proc/sys/net/nexus/owner_raw", O_RDWR); CHECK(knob >= 0, "owner_raw sysctl");
    CHECK(write(knob, "0", 1) == 1, "owner_raw off");
    CHECK(socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)) < 0 && errno == EPERM, "owner_raw 0 withdraws CAP_NET_RAW");
    CHECK(lseek(knob, 0, SEEK_SET) == 0 && write(knob, "1", 1) == 1, "owner_raw on"); close(knob);
    raw_capture(); cooked_and_auxdata(); arp_exchange(); filters(); promiscuous(); ring_v2(); ring_v3();
    puts("[64] PACKET CAMPAIGN PASSED");
    return 0;
}
