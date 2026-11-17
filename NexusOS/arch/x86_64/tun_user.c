/* TUN/TAP campaign (tests/test_x64_tun.py, kernel campaign /nexus/test-tun): open /dev/net/tun, create an IFF_TUN
 * interface, address it, and prove the data path both ways -- a packet the kernel routes out tun0 is read from the
 * device fd (the VPN "encapsulate" direction), and a packet written into the device fd is delivered to a socket (the
 * "decapsulate" direction). Also covers IFF_NO_PI vs the 4-byte packet-information header, TUNGETIFF, persistence,
 * owner checks and a TAP interface carrying an ARP/IPv6 frame. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(c, ...) do { if (!(c)) { printf("[64] FAIL: " __VA_ARGS__); printf(" (errno %d)\n", errno); exit(1); } } while (0)

static uint16_t ip_checksum(const void* data, unsigned length) {
    const uint8_t* p = data; uint32_t sum = 0;
    for (unsigned i = 0; i + 1 < length; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (length & 1) sum += (uint32_t)p[length - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
static int tun_open_iface(const char* name, int flags) {
    int fd = open("/dev/net/tun", O_RDWR); CHECK(fd >= 0, "open /dev/net/tun");
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr)); ifr.ifr_flags = (short)flags; strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    CHECK(ioctl(fd, TUNSETIFF, &ifr) == 0, "TUNSETIFF %s", name);
    return fd;
}
static void set_addr(const char* name, const char* addr, const char* mask) {
    int s = socket(AF_INET, SOCK_DGRAM, 0); struct ifreq ifr; memset(&ifr, 0, sizeof(ifr)); strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    struct sockaddr_in* sin = (void*)&ifr.ifr_addr; sin->sin_family = AF_INET;
    inet_pton(AF_INET, addr, &sin->sin_addr); CHECK(ioctl(s, SIOCSIFADDR, &ifr) == 0, "SIOCSIFADDR %s", addr);
    inet_pton(AF_INET, mask, &sin->sin_addr); CHECK(ioctl(s, SIOCSIFNETMASK, &ifr) == 0, "SIOCSIFNETMASK");
    CHECK(ioctl(s, SIOCGIFFLAGS, &ifr) == 0, "SIOCGIFFLAGS");
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING; CHECK(ioctl(s, SIOCSIFFLAGS, &ifr) == 0, "SIOCSIFFLAGS up");
    close(s);
}
static unsigned build_udp(uint8_t* p, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const char* payload) {
    unsigned plen = (unsigned)strlen(payload), udp = 8 + plen, total = 20 + udp;
    memset(p, 0, total);
    p[0] = 0x45; p[3] = (uint8_t)total; p[2] = (uint8_t)(total >> 8); p[8] = 64; p[9] = 17;   /* IPv4, UDP, ttl 64 */
    uint32_t s = htonl(src), d = htonl(dst); memcpy(p + 12, &s, 4); memcpy(p + 16, &d, 4);
    uint16_t c = htons(ip_checksum(p, 20)); memcpy(p + 10, &c, 2);
    p[20] = (uint8_t)(sport >> 8); p[21] = (uint8_t)sport; p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    p[24] = (uint8_t)(udp >> 8); p[25] = (uint8_t)udp; memcpy(p + 28, payload, plen);   /* checksum 0 (optional for IPv4 UDP) */
    return total;
}

static void tun_roundtrip(void) {
    int fd = tun_open_iface("tun0", IFF_TUN | IFF_NO_PI);
    struct ifreq got; memset(&got, 0, sizeof(got)); CHECK(ioctl(fd, TUNGETIFF, &got) == 0, "TUNGETIFF");
    CHECK(!strcmp(got.ifr_name, "tun0") && (got.ifr_flags & IFF_TUN) && (got.ifr_flags & IFF_NO_PI), "TUNGETIFF reports tun0/IFF_TUN/NO_PI (%s %x)", got.ifr_name, got.ifr_flags);
    set_addr("tun0", "10.9.0.1", "255.255.255.0");

    /* encapsulate direction: a datagram the kernel routes to 10.9.0.2 (on tun0's subnet) is read raw from the fd */
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(4242)}; inet_pton(AF_INET, "10.9.0.2", &to.sin_addr);
    CHECK(sendto(udp, "hello-tun", 9, 0, (void*)&to, sizeof(to)) == 9, "send to the tun subnet");
    /* the interface also emits IPv6 control traffic (RS/MLD) when it comes up; read past it to our IPv4 UDP packet */
    uint8_t frame[2048]; long n = 0; int found = 0;
    for (int tries = 0; tries < 50 && !found; tries++) {
        struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 3000) == 1, "tun fd readable after routing");
        n = read(fd, frame, sizeof(frame));
        if (n >= 28 && (frame[0] >> 4) == 4 && frame[9] == 17) found = 1;
    }
    CHECK(found, "read the IPv4 UDP packet from tun0 (last n=%ld, b0=%02x)", n, frame[0]);
    uint32_t dst; memcpy(&dst, frame + 16, 4); unsigned ihl = (frame[0] & 15) * 4;
    CHECK(ntohl(dst) == 0x0A090002 && (frame[ihl + 2] << 8 | frame[ihl + 3]) == 4242 && !memcmp(frame + ihl + 8, "hello-tun", 9),
          "the packet carries the datagram to 10.9.0.2:4242");

    /* decapsulate direction: a packet written into the fd is delivered to a bound socket */
    int listen = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in bind_addr = {.sin_family = AF_INET, .sin_port = htons(5000)}; inet_pton(AF_INET, "10.9.0.1", &bind_addr.sin_addr);
    CHECK(bind(listen, (void*)&bind_addr, sizeof(bind_addr)) == 0, "bind 10.9.0.1:5000");
    uint8_t packet[128]; unsigned len = build_udp(packet, 0x0A090002, 0x0A090001, 4242, 5000, "world-in");
    CHECK(write(fd, packet, len) == (long)len, "write a packet into tun0");
    struct pollfd p = {listen, POLLIN, 0}; CHECK(poll(&p, 1, 3000) == 1, "the injected packet reaches the socket");
    uint8_t back[128]; struct sockaddr_in from; socklen_t fl = sizeof(from);
    n = recvfrom(listen, back, sizeof(back), 0, (void*)&from, &fl);
    CHECK(n == 8 && !memcmp(back, "world-in", 8) && from.sin_addr.s_addr == htonl(0x0A090002) && from.sin_port == htons(4242),
          "socket received the decapsulated datagram from 10.9.0.2:4242");
    close(udp); close(listen); close(fd);
    puts("[64] TUN ROUNDTRIP: route-out read from the fd, write-in delivered to a socket, IFF_NO_PI, TUNGETIFF");
}
static void tun_pi(void) {
    int fd = tun_open_iface("tun0", IFF_TUN);   /* with the 4-byte packet-information header */
    set_addr("tun0", "10.9.0.1", "255.255.255.0");
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(99)}; inet_pton(AF_INET, "10.9.0.5", &to.sin_addr);
    CHECK(sendto(udp, "pi", 2, 0, (void*)&to, sizeof(to)) == 2, "send");
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 3000) == 1, "readable");
    uint8_t frame[2048]; long n = read(fd, frame, sizeof(frame));
    CHECK(n >= 24 && frame[0] == 0 && frame[1] == 0 && (frame[2] << 8 | frame[3]) == 0x0800 && (frame[4] >> 4) == 4,
          "the frame starts with the PI header (flags 0, proto 0x0800) then the IPv4 packet");
    close(udp); close(fd);
    puts("[64] TUN PI: the packet-information header prefixes the frame without IFF_NO_PI");
}
static void tap_frame(void) {
    int fd = tun_open_iface("tap0", IFF_TAP | IFF_NO_PI);
    set_addr("tap0", "10.9.1.1", "255.255.255.0");
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(7)}; inet_pton(AF_INET, "10.9.1.9", &to.sin_addr);
    /* 10.9.1.9 is unknown: the kernel emits an ARP request on tap0, which we read as an Ethernet frame */
    sendto(udp, "x", 1, 0, (void*)&to, sizeof(to));
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 3000) == 1, "tap readable");
    uint8_t frame[2048]; long n = read(fd, frame, sizeof(frame));
    CHECK(n >= 14, "read an Ethernet frame from tap0 (%ld)", n);
    uint16_t ethertype = (uint16_t)(frame[12] << 8 | frame[13]);
    CHECK(ethertype == 0x0806 || ethertype == 0x86DD || ethertype == 0x0800, "tap frame has an ethertype (%04x)", ethertype);
    close(udp); close(fd);
    puts("[64] TAP: an IFF_TAP interface carries whole Ethernet frames");
}
static void tun_persist_owner(void) {
    int fd = tun_open_iface("tunp", IFF_TUN | IFF_NO_PI);
    CHECK(ioctl(fd, TUNSETPERSIST, (void*)1) == 0, "TUNSETPERSIST on");
    CHECK(ioctl(fd, TUNSETOWNER, (void*)1000) == 0, "TUNSETOWNER");
    close(fd);                                              /* persistent: the interface stays */
    int again = open("/dev/net/tun", O_RDWR); struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI; strncpy(ifr.ifr_name, "tunp", IFNAMSIZ - 1);
    CHECK(ioctl(again, TUNSETIFF, &ifr) == 0, "re-attach to the persistent tunp");
    CHECK(ioctl(again, TUNSETPERSIST, (void*)0) == 0, "TUNSETPERSIST off");
    close(again);                                          /* no longer persistent and no fd: it goes away */
    int s = socket(AF_INET, SOCK_DGRAM, 0); memset(&ifr, 0, sizeof(ifr)); strncpy(ifr.ifr_name, "tunp", IFNAMSIZ - 1);
    CHECK(ioctl(s, SIOCGIFINDEX, &ifr) < 0 && errno == ENODEV, "tunp was removed after the last fd closed");
    close(s);
    puts("[64] TUN PERSIST: TUNSETPERSIST keeps the interface across close; clearing it removes the interface");
}
static void features(void) {
    int fd = open("/dev/net/tun", O_RDWR); CHECK(fd >= 0, "open /dev/net/tun in features"); unsigned f = 0;
    CHECK(ioctl(fd, TUNGETFEATURES, &f) == 0 && (f & IFF_TUN) && (f & IFF_TAP) && (f & IFF_NO_PI), "TUNGETFEATURES %x", f);
    uint8_t junk[4]; CHECK(read(fd, junk, sizeof(junk)) < 0 && errno == EBADFD, "read before TUNSETIFF is EBADFD");
    close(fd);
    puts("[64] TUN FEATURES: TUNGETFEATURES advertises TUN/TAP/NO_PI; I/O before TUNSETIFF fails");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    features(); tun_roundtrip(); tun_pi(); tap_frame(); tun_persist_owner();
    puts("[64] TUN CAMPAIGN PASSED");
    return 0;
}
