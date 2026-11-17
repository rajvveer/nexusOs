/* TCP throughput benchmark against the host (tests/net_bench.py): glibc sockets,
 * no browser and no disk traffic, so it measures the network path itself. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { printf("[64] NETBENCH FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static uint8_t buffer[1 << 18];
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int open_peer(const struct sockaddr_in* peer) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
    int one = 1; CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
    CHECK(connect(fd, (const void*)peer, sizeof(*peer)) == 0);
    return fd;
}
static void command(int fd, char kind, uint32_t mib) {
    uint8_t request[5] = {(uint8_t)kind, (uint8_t)mib, (uint8_t)(mib >> 8), (uint8_t)(mib >> 16), (uint8_t)(mib >> 24)};
    CHECK(write(fd, request, sizeof(request)) == sizeof(request));
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint16_t config[3]; int file = open("/nexus/netbench", O_RDONLY);
    CHECK(file >= 0 && read(file, config, sizeof(config)) == sizeof(config)); close(file);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(config[0]), .sin_addr.s_addr = htonl(0x0a000202)};
    uint32_t mib = config[1], rounds = config[2];
    for (uint32_t round = 0; round < rounds; round++) {
        int fd = open_peer(&peer); command(fd, 'D', mib);
        uint64_t total = 0, want = (uint64_t)mib << 20; unsigned bad = 0; double start = seconds();
        for (;;) {
            ssize_t got = read(fd, buffer, sizeof(buffer)); CHECK(got >= 0);
            if (!got) break;
            for (ssize_t i = 0; i < got; i += 4093) if (buffer[i] != (uint8_t)((total + i) * 37 + 11)) bad++;
            total += got;
        }
        double elapsed = seconds() - start; close(fd);
        CHECK(total == want && !bad);
        printf("[64] NETBENCH download round=%u MiB=%u seconds=%.3f MBps=%.2f\n", round, mib, elapsed, total / 1e6 / elapsed);
        fd = open_peer(&peer); command(fd, 'U', mib);
        for (size_t i = 0; i < sizeof(buffer); i++) buffer[i] = (uint8_t)(i * 37 + 11);
        start = seconds(); total = 0;
        while (total < want) {
            size_t part = want - total < sizeof(buffer) ? want - total : sizeof(buffer);
            ssize_t sent = write(fd, buffer, part); CHECK(sent > 0); total += sent;
        }
        CHECK(shutdown(fd, SHUT_WR) == 0);
        char ack[8]; ssize_t n = read(fd, ack, sizeof(ack)); CHECK(n == 2 && !memcmp(ack, "OK", 2));
        elapsed = seconds() - start; close(fd);
        printf("[64] NETBENCH upload round=%u MiB=%u seconds=%.3f MBps=%.2f\n", round, mib, elapsed, total / 1e6 / elapsed);
    }
    puts("[64] NETBENCH DONE");
    return 0;
}
