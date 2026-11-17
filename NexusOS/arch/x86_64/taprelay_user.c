/* taprelay: bridges a TAP interface to UDP, so a host test (tests/netfix.py TunnelLink) can be a whole second Ethernet
 * segment behind this machine (tests/test_x64_nat.py: the LAN of a NAT router).
 *   taprelay IFNAME LOCAL_PORT PEER_IPV4 PEER_PORT
 * Frames read from the TAP device go to PEER as one UDP datagram each; datagrams from PEER are written to the TAP
 * device. Anything from another sender is ignored. Runs until killed. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 5) { fprintf(stderr, "usage: taprelay IFNAME LOCAL_PORT PEER_IPV4 PEER_PORT\n"); return 2; }
    int tap = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (tap < 0) { printf("[64] TAPRELAY open /dev/net/tun failed %d\n", errno); return 1; }
    struct ifreq request; memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, IFNAMSIZ, "%s", argv[1]); request.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ioctl(tap, TUNSETIFF, &request)) { printf("[64] TAPRELAY TUNSETIFF failed %d\n", errno); return 1; }
    int udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2]))}, peer = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[4]))};
    if (inet_pton(AF_INET, argv[3], &peer.sin_addr) != 1) return 2;
    int size = 1 << 20; setsockopt(udp, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)); setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    if (bind(udp, (struct sockaddr*)&local, sizeof(local))) { printf("[64] TAPRELAY bind failed %d\n", errno); return 1; }
    printf("[64] TAPRELAY READY %s\n", request.ifr_name); fflush(stdout);
    static unsigned char frame[65536];
    unsigned long long up = 0, down = 0;
    for (;;) {
        struct pollfd p[2] = {{tap, POLLIN, 0}, {udp, POLLIN, 0}};
        if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; return 1; }
        if (p[0].revents & POLLIN) {
            ssize_t n = read(tap, frame, sizeof(frame));
            if (n > 0 && sendto(udp, frame, (size_t)n, 0, (struct sockaddr*)&peer, sizeof(peer)) == n) up++;
        }
        if (p[1].revents & POLLIN) {
            struct sockaddr_in from; socklen_t length = sizeof(from);
            ssize_t n = recvfrom(udp, frame, sizeof(frame), 0, (struct sockaddr*)&from, &length);
            if (n > 0 && from.sin_addr.s_addr == peer.sin_addr.s_addr && from.sin_port == peer.sin_port && write(tap, frame, (size_t)n) == n) down++;
        }
        (void)up; (void)down;
    }
}
