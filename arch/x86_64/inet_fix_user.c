/* Regression checks for the "net" fix group (listen/accept queue, SO_REUSEADDR, loopback backpressure, addresses). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] FIX FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define LOOP 0x7F000001U
#define SELF 0x0A00020FU
static bool nic, measure_only;   /* measure_only: report, do not judge (baseline kernels) */
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static struct sockaddr_in at(uint32_t ip, uint16_t port) { return (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip)}; }
static uint16_t port_of(int fd) { struct sockaddr_in a; socklen_t n = sizeof(a); CHECK(getsockname(fd, (void*)&a, &n) == 0); return ntohs(a.sin_port); }
static int tcp_socket(bool nonblock) { int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0); CHECK(fd >= 0); return fd; }
static int listener(uint32_t ip, uint16_t port, int backlog, bool reuse) {
    int fd = tcp_socket(false), one = 1;
    if (reuse) CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    struct sockaddr_in a = at(ip, port); CHECK(bind(fd, (void*)&a, sizeof(a)) == 0 && listen(fd, backlog) == 0); return fd;
}
static int sock_error(int fd) { int e = -1; socklen_t n = sizeof(e); CHECK(getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n) == 0); return e; }
/* Nonblocking connect; returns the SO_ERROR seen within `ms` (-1: still pending). */
static int connect_wait(int fd, uint32_t ip, uint16_t port, int ms) {
    struct sockaddr_in a = at(ip, port);
    int r = connect(fd, (void*)&a, sizeof(a)); if (r == 0) return 0;
    CHECK(errno == EINPROGRESS);
    struct pollfd p = {fd, POLLOUT, 0}; int n = poll(&p, 1, ms); CHECK(n >= 0);
    return n ? sock_error(fd) : -1;
}
static int connect_now(uint32_t ip, uint16_t port) {
    int fd = tcp_socket(false); struct sockaddr_in a = at(ip, port);
    CHECK(connect(fd, (void*)&a, sizeof(a)) == 0); return fd;
}
static int lowest_free_fd(void) { int fd = dup(1); CHECK(fd >= 0); close(fd); return fd; }

/* wine-9 / wine-34: accept re-checks addr/addrlen after sleeping, and never leaks the descriptor of a failed copy. */
static uint32_t* volatile shared_len;
static int race_server, race_result, race_errno;
static void* blocked_accept(void* unused) {
    (void)unused; struct sockaddr_in a;
    race_result = accept(race_server, (void*)&a, shared_len); race_errno = errno; return NULL;
}
static void accept_pointers(void) {
    int server = listener(LOOP, 0, 4, false); uint16_t port = port_of(server);
    int before = lowest_free_fd(), client = connect_now(LOOP, port); socklen_t len = 16;
    CHECK(accept4(server, (void*)8, &len, SOCK_CLOEXEC) == -1 && errno == EFAULT);
    CHECK(lowest_free_fd() == before + 1);   /* only the client descriptor exists */
    char c; CHECK(recv(client, &c, 1, 0) == -1 && errno == ECONNRESET); close(client);
    /* addrlen unmapped by another thread while accept() sleeps: EFAULT, not a kernel fault */
    uint32_t* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(page != MAP_FAILED);
    *page = 16; shared_len = page; race_server = server;
    pthread_t thread; CHECK(pthread_create(&thread, NULL, blocked_accept, NULL) == 0);
    CHECK(poll(NULL, 0, 150) == 0);
    CHECK(munmap(page, 4096) == 0);
    client = connect_now(LOOP, port);
    CHECK(pthread_join(thread, NULL) == 0 && race_result == -1 && race_errno == EFAULT);
    CHECK(lowest_free_fd() == before + 1);
    CHECK(recv(client, &c, 1, 0) == -1 && errno == ECONNRESET); close(client);
    /* a good accept still reports the peer */
    client = connect_now(LOOP, port); struct sockaddr_in peer; len = sizeof(peer);
    int served = accept(server, (void*)&peer, &len); CHECK(served >= 0 && len == 16 && peer.sin_addr.s_addr == htonl(LOOP) && ntohs(peer.sin_port) == port_of(client));
    close(served); close(client); close(server);
    puts("[64] FIX ACCEPT POINTERS PASSED");
}

/* wine-11 / wine-13 / wine-51: the accept queue holds backlog+1 connections, extra SYNs wait (client retries). */
static void accept_queue(void) {
    int server = listener(LOOP, 0, 2, false); uint16_t port = port_of(server);
    int clients[6], done = 0;
    for (int i = 0; i < 6; i++) { clients[i] = tcp_socket(true); struct sockaddr_in a = at(LOOP, port);
        CHECK(connect(clients[i], (void*)&a, sizeof(a)) == -1 && errno == EINPROGRESS); }
    CHECK(poll(NULL, 0, 400) == 0);
    for (int i = 0; i < 6; i++) { struct pollfd p = {clients[i], POLLOUT, 0}; CHECK(poll(&p, 1, 0) >= 0); if (p.revents & POLLOUT) { CHECK(sock_error(clients[i]) == 0); done++; } }
    printf("[64] FIX QUEUE: %d of 6 handshakes completed with backlog 2\n", done);
    CHECK(done == 3);
    struct pollfd p = {server, POLLIN, 0}; CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    int value = 0; socklen_t n = sizeof(value); CHECK(getsockopt(server, SOL_SOCKET, SO_ACCEPTCONN, &value, &n) == 0 && value == 1);
    struct timeval timeout = {.tv_sec = 6}; CHECK(setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint64_t start = now_ms();
    for (int i = 0; i < 6; i++) {
        int s = accept4(server, NULL, NULL, SOCK_CLOEXEC); CHECK(s >= 0);
        CHECK(write(s, "q", 1) == 1); close(s);
    }
    printf("[64] FIX QUEUE: all 6 accepted in %llu ms\n", (unsigned long long)(now_ms() - start));
    for (int i = 0; i < 6; i++) { char c = 0; struct pollfd q = {clients[i], POLLIN, 0}; CHECK(poll(&q, 1, 3000) == 1); CHECK(read(clients[i], &c, 1) == 1 && c == 'q'); close(clients[i]); }
    close(server);
    puts("[64] FIX ACCEPT QUEUE PASSED");
}

/* wine-15 / wine-49 / wine-52 / wine-53 / wine-54 / wine-21: options and listener life cycle. */
static int shut_server, shut_result, shut_errno;
static void* accept_until_shutdown(void* unused) { (void)unused; shut_result = accept(shut_server, NULL, NULL); shut_errno = errno; return NULL; }
static void listener_lifecycle(void) {
    int fd = tcp_socket(false), value = 1; socklen_t n = sizeof(value);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0);
    value = 0; CHECK(getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, &n) == 0 && value == 1);
    value = 1; CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value)) == 0);
    value = 0; CHECK(getsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &value, &n) == 0 && value == 1);
    value = 1; CHECK(getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &n) == 0 && value == 0);
    struct sockaddr_in a = at(LOOP, 0); CHECK(bind(fd, (void*)&a, sizeof(a)) == 0);
    CHECK(listen(fd, -1) == 0);   /* negative backlog: maximum, as Linux */
    uint16_t port = port_of(fd);
    value = 0; CHECK(getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &n) == 0 && value == 1);
    struct sockaddr_in self = at(LOOP, port); CHECK(connect(fd, (void*)&self, sizeof(self)) == -1 && errno == EISCONN);
    int client = connect_now(LOOP, port), served = accept(fd, NULL, NULL); CHECK(served >= 0);
    close(fd);   /* the listener goes, the accepted connection stays */
    int probe = tcp_socket(true); uint64_t start = now_ms();
    CHECK(connect_wait(probe, LOOP, port, 3000) == ECONNREFUSED && now_ms() - start < 1500); close(probe);
    int plain = tcp_socket(false); a = at(LOOP, port);
    CHECK(bind(plain, (void*)&a, sizeof(a)) == -1 && errno == EADDRINUSE); close(plain);
    int again = listener(LOOP, port, 4, true);   /* restart with SO_REUSEADDR beside the live accepted connection */
    int other = listener(LOOP, 0, 4, true); a = at(LOOP, port_of(other));
    int clash = tcp_socket(false); value = 1; CHECK(setsockopt(clash, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0);
    CHECK(bind(clash, (void*)&a, sizeof(a)) == -1 && errno == EADDRINUSE);   /* never beside a live listener */
    close(clash); close(other);
    int second = connect_now(LOOP, port), s2 = accept(again, NULL, NULL); CHECK(s2 >= 0);
    CHECK(write(served, "a", 1) == 1 && write(s2, "b", 1) == 1);
    char c; CHECK(read(client, &c, 1) == 1 && c == 'a' && read(second, &c, 1) == 1 && c == 'b');
    close(s2); close(second); close(served); close(client);
    /* shutdown(SHUT_WR) leaves a listener alone; SHUT_RDWR wakes a blocked accept with EINVAL */
    CHECK(shutdown(again, SHUT_WR) == 0);
    value = 0; CHECK(getsockopt(again, SOL_SOCKET, SO_ACCEPTCONN, &value, &n) == 0 && value == 1);
    shut_server = again; pthread_t thread; CHECK(pthread_create(&thread, NULL, accept_until_shutdown, NULL) == 0);
    CHECK(poll(NULL, 0, 150) == 0);
    CHECK(shutdown(again, SHUT_RDWR) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && shut_result == -1 && shut_errno == EINVAL);
    value = 1; CHECK(getsockopt(again, SOL_SOCKET, SO_ACCEPTCONN, &value, &n) == 0 && value == 0);
    probe = tcp_socket(true); CHECK(connect_wait(probe, LOOP, port, 3000) == ECONNREFUSED); close(probe);
    CHECK(listen(again, 1) == 0);   /* listening again on the same bound port works */
    client = connect_now(LOOP, port); served = accept(again, NULL, NULL); CHECK(served >= 0);
    close(served); close(client); close(again);
    puts("[64] FIX LISTENER LIFE CYCLE PASSED");
}

/* wine-47 / wine-20: RSTs leave from the contacted address, so 127.0.x.y gets ECONNREFUSED at once. */
static void refused_alias(void) {
    int fd = tcp_socket(true); uint64_t start = now_ms();
    int error = connect_wait(fd, 0x7F000101U, 9, 5000);
    printf("[64] FIX 127.0.1.1 CONNECT ERROR %d after %llu ms\n", error, (unsigned long long)(now_ms() - start));
    if (!measure_only) CHECK(error == ECONNREFUSED);
    close(fd);
    puts("[64] FIX RST SOURCE PASSED");
}

/* wine-48: a burst of UDP datagrams to localhost never fails with ENETUNREACH (full loopback ring). */
static void udp_burst(void) {
    int receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(receiver >= 0 && sender >= 0);
    int size = 4 << 20; CHECK(setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0);
    struct sockaddr_in a = at(LOOP, 0); CHECK(bind(receiver, (void*)&a, sizeof(a)) == 0); a = at(LOOP, port_of(receiver));
    char data[100]; memset(data, 'u', sizeof(data));
    for (int i = 0; i < 400; i++) { data[0] = (char)i; ssize_t r = sendto(sender, data, sizeof(data), 0, (void*)&a, sizeof(a)); if (r != (ssize_t)sizeof(data)) { printf("[64] FIX UDP send %d -> %zd errno %d\n", i, r, errno); CHECK(measure_only); break; } }
    if (!measure_only) CHECK(sock_error(sender) == 0);
    int got = 0; struct pollfd p = {receiver, POLLIN, 0};
    while (got < 400 && poll(&p, 1, 2000) == 1) { CHECK(recv(receiver, data, sizeof(data), 0) == (ssize_t)sizeof(data)); got++; }
    printf("[64] FIX UDP BURST: 400 sent, %d received\n", got);
    if (!measure_only) CHECK(got == 400);
    close(sender); close(receiver);
    puts("[64] FIX UDP LOOPBACK BURST PASSED");
}

/* wine-14: a bulk TCP transfer over loopback is not throttled to one segment per RTO. */
static int bulk_fd;
static void* bulk_reader(void* unused) {
    (void)unused; static char buffer[65536]; size_t total = 0; ssize_t n;
    while ((n = read(bulk_fd, buffer, sizeof(buffer))) > 0) total += n;
    return (void*)total;
}
static void tcp_bulk(void) {
    int server = listener(LOOP, 0, 1, false), client = connect_now(LOOP, port_of(server)), served = accept(server, NULL, NULL);
    CHECK(served >= 0); bulk_fd = served; pthread_t thread; CHECK(pthread_create(&thread, NULL, bulk_reader, NULL) == 0);
    static char block[1 << 20]; memset(block, 'b', sizeof(block)); uint64_t start = now_ms();
    for (int i = 0; i < 8; i++) { size_t off = 0; while (off < sizeof(block)) { ssize_t n = write(client, block + off, sizeof(block) - off); CHECK(n > 0); off += n; } }
    CHECK(shutdown(client, SHUT_WR) == 0);
    void* total; CHECK(pthread_join(thread, &total) == 0);
    uint64_t ms = now_ms() - start;
    printf("[64] FIX TCP LOOPBACK 8 MiB in %llu ms\n", (unsigned long long)ms);
    CHECK((size_t)total == 8u << 20); if (!measure_only) CHECK(ms < 30000);
    close(served); close(client); close(server);
    puts("[64] FIX TCP LOOPBACK BULK PASSED");
}

/* wine-55 / wine-18 / wine-20: per-address binds, listener address filtering, bound source addresses. */
static void addresses(void) {
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0); CHECK(u1 >= 0 && u2 >= 0);
    struct sockaddr_in a = at(LOOP, 0); CHECK(bind(u1, (void*)&a, sizeof(a)) == 0); uint16_t port = port_of(u1);
    a = at(SELF, port); CHECK(bind(u2, (void*)&a, sizeof(a)) == 0);   /* same port, other address */
    int u3 = socket(AF_INET, SOCK_DGRAM, 0); a = at(0, port); CHECK(bind(u3, (void*)&a, sizeof(a)) == -1 && errno == EADDRINUSE); close(u3);
    /* a datagram from a socket bound to our address arrives from that address; a 127-bound one cannot leave the host */
    int from = socket(AF_INET, SOCK_DGRAM, 0); a = at(SELF, 0); CHECK(bind(from, (void*)&a, sizeof(a)) == 0);
    a = at(LOOP, port); CHECK(sendto(from, "s", 1, 0, (void*)&a, sizeof(a)) == 1);
    struct sockaddr_in src; socklen_t n = sizeof(src); char c; struct pollfd p = {u1, POLLIN, 0};
    CHECK(poll(&p, 1, 2000) == 1 && recvfrom(u1, &c, 1, 0, (void*)&src, &n) == 1 && src.sin_addr.s_addr == htonl(SELF) && ntohs(src.sin_port) == port_of(from));
    CHECK(sendto(u1, "r", 1, 0, (void*)&src, n) == 1);   /* the reply reaches the bound socket */
    p.fd = from; CHECK(poll(&p, 1, 2000) == 1 && recv(from, &c, 1, 0) == 1 && c == 'r');
    a = at(0x0A000202U, 9); CHECK(sendto(u1, "x", 1, 0, (void*)&a, sizeof(a)) == -1 && errno == EINVAL);
    close(from); close(u1); close(u2);
    /* TCP: one listener per address on the same port; each only takes its own address */
    int t1 = listener(LOOP, 0, 4, false); port = port_of(t1); int t2 = listener(SELF, port, 4, false);
    int t3 = tcp_socket(false); a = at(0, port); CHECK(bind(t3, (void*)&a, sizeof(a)) == -1 && errno == EADDRINUSE); close(t3);
    int c1 = connect_now(LOOP, port), c2 = tcp_socket(false);
    a = at(SELF, 0); CHECK(bind(c2, (void*)&a, sizeof(a)) == 0); a = at(SELF, port); CHECK(connect(c2, (void*)&a, sizeof(a)) == 0);
    int s1 = accept(t1, NULL, NULL), s2 = accept(t2, (void*)&src, &n); CHECK(s1 >= 0 && s2 >= 0);
    n = sizeof(src); CHECK(getpeername(s2, (void*)&src, &n) == 0 && src.sin_addr.s_addr == htonl(SELF));
    n = sizeof(src); CHECK(getsockname(s1, (void*)&src, &n) == 0 && src.sin_addr.s_addr == htonl(LOOP));
    close(s1); close(s2); close(c1); close(c2); close(t2);
    /* a 127.0.0.1-bound listener refuses a connection to our external address (RST at the SYN) */
    int probe = tcp_socket(true); CHECK(connect_wait(probe, SELF, port, 3000) == ECONNREFUSED); close(probe);
    /* a TCP socket bound to our address connecting to 127.0.0.1 uses that source */
    int c3 = tcp_socket(false); a = at(SELF, 0); CHECK(bind(c3, (void*)&a, sizeof(a)) == 0);
    a = at(LOOP, port); CHECK(connect(c3, (void*)&a, sizeof(a)) == 0);
    int s3 = accept(t1, NULL, NULL); CHECK(s3 >= 0); n = sizeof(src);
    CHECK(getpeername(s3, (void*)&src, &n) == 0 && src.sin_addr.s_addr == htonl(SELF) && ntohs(src.sin_port) == port_of(c3));
    CHECK(write(c3, "z", 1) == 1 && read(s3, &c, 1) == 1 && c == 'z');
    int c4 = tcp_socket(false); a = at(LOOP, 0); CHECK(bind(c4, (void*)&a, sizeof(a)) == 0);
    a = at(0x0A000202U, 80); CHECK(connect(c4, (void*)&a, sizeof(a)) == -1 && errno == EINVAL); close(c4);
    close(s3); close(c3); close(t1);
    puts("[64] FIX ADDRESSES PASSED");
}

/* wine-12: closing a listener resets the connections still queued for accept(), and frees the port at once. */
static void close_with_queue(void) {
    int server = listener(LOOP, 0, 4, false); uint16_t port = port_of(server);
    int c1 = connect_now(LOOP, port), c2 = connect_now(LOOP, port);
    struct pollfd p = {server, POLLIN, 0}; CHECK(poll(&p, 1, 2000) == 1);
    close(server);
    char c; struct pollfd q[2] = {{c1, POLLIN, 0}, {c2, POLLIN, 0}}; CHECK(poll(q, 2, 2000) >= 1);
    CHECK(read(c1, &c, 1) == -1 && errno == ECONNRESET && read(c2, &c, 1) == -1 && errno == ECONNRESET);
    int again = tcp_socket(false); struct sockaddr_in a = at(LOOP, port);
    CHECK(bind(again, (void*)&a, sizeof(a)) == 0 && listen(again, 1) == 0);   /* no SO_REUSEADDR needed */
    close(again); close(c1); close(c2);
    puts("[64] FIX CLOSE WITH QUEUE PASSED");
}

/* wine-14: the peer's FIN arrives while the server's response and FIN still wait for the loopback ring
 * (FIN_WAIT_1 -> CLOSING): the client still gets every byte and a clean EOF. */
static void closing_response(void) {
    int server = listener(LOOP, 0, 1, false), client = connect_now(LOOP, port_of(server)), served = accept(server, NULL, NULL);
    CHECK(served >= 0); char request[4];
    CHECK(write(client, "req", 3) == 3 && read(served, request, 3) == 3);
    static unsigned char response[256 << 10]; for (size_t i = 0; i < sizeof(response); i++) response[i] = (unsigned char)(i * 7 + 3);
    CHECK(write(served, response, sizeof(response)) == (ssize_t)sizeof(response));
    close(served);
    CHECK(shutdown(client, SHUT_WR) == 0);   /* our FIN races the unsent part of the response */
    struct timeval timeout = {.tv_sec = 10}; CHECK(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    static unsigned char got[256 << 10]; size_t total = 0; ssize_t n; uint64_t start = now_ms();
    while ((n = read(client, got + total, sizeof(got) - total < 65536 ? sizeof(got) - total : 65536)) > 0) { total += n; if (total == sizeof(got)) break; }
    CHECK(n >= 0 && total == sizeof(got) && !memcmp(got, response, sizeof(got)));
    char extra; CHECK(read(client, &extra, 1) == 0);   /* clean EOF */
    printf("[64] FIX CLOSING RESPONSE: %zu bytes + EOF in %llu ms\n", total, (unsigned long long)(now_ms() - start));
    close(client); close(server);
    puts("[64] FIX CLOSING RESPONSE PASSED");
}

/* wine-14: a bulk loopback transfer does not starve an older connection or local datagrams. */
static volatile int bulk_stop;
static int fair_writer_fd;
static void* fair_writer(void* unused) {
    (void)unused; static char block[64 << 10]; memset(block, 'w', sizeof(block)); size_t total = 0;
    while (!bulk_stop) { ssize_t n = write(fair_writer_fd, block, sizeof(block)); CHECK(n > 0); total += n; }
    CHECK(shutdown(fair_writer_fd, SHUT_WR) == 0); return (void*)total;
}
static void fairness(void) {
    int server = listener(LOOP, 0, 4, false); uint16_t port = port_of(server);
    int old_client = connect_now(LOOP, port), old_served = accept(server, NULL, NULL);   /* the older connection */
    int bulk_client = connect_now(LOOP, port), bulk_served = accept(server, NULL, NULL); CHECK(old_served >= 0 && bulk_served >= 0);
    bulk_stop = 0; fair_writer_fd = bulk_client; bulk_fd = bulk_served;
    pthread_t writer, reader; CHECK(pthread_create(&reader, NULL, bulk_reader, NULL) == 0 && pthread_create(&writer, NULL, fair_writer, NULL) == 0);
    CHECK(poll(NULL, 0, 200) == 0);   /* the bulk transfer is running */
    struct timeval timeout = {.tv_sec = 5}; CHECK(setsockopt(old_served, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                                                 setsockopt(old_client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint64_t start = now_ms(), worst = 0;
    for (int i = 0; i < 100; i++) {
        uint64_t t = now_ms(); char c = (char)i, r = 0;
        CHECK(write(old_client, &c, 1) == 1 && read(old_served, &r, 1) == 1 && r == c && write(old_served, &r, 1) == 1 && read(old_client, &c, 1) == 1 && c == (char)i);
        if (now_ms() - t > worst) worst = now_ms() - t;
    }
    uint64_t ping_ms = now_ms() - start;
    int receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); CHECK(receiver >= 0 && sender >= 0);
    int size = 4 << 20; CHECK(setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0);
    struct sockaddr_in a = at(LOOP, 0); CHECK(bind(receiver, (void*)&a, sizeof(a)) == 0); a = at(LOOP, port_of(receiver));
    int got = 0; char data[200]; start = now_ms();
    for (int i = 0; i < 200; i++) CHECK(sendto(sender, data, sizeof(data), 0, (void*)&a, sizeof(a)) == (ssize_t)sizeof(data));
    struct pollfd p = {receiver, POLLIN, 0};
    while (got < 200 && poll(&p, 1, 3000) == 1) { CHECK(recv(receiver, data, sizeof(data), 0) == (ssize_t)sizeof(data)); got++; }
    uint64_t udp_ms = now_ms() - start;
    bulk_stop = 1; void* written; void* read_total; CHECK(pthread_join(writer, &written) == 0 && pthread_join(reader, &read_total) == 0);
    printf("[64] FIX FAIRNESS: 100 round trips in %llu ms (worst %llu ms), %d/200 datagrams in %llu ms beside %zu MiB bulk\n",
           (unsigned long long)ping_ms, (unsigned long long)worst, got, (unsigned long long)udp_ms, (size_t)written >> 20);
    CHECK(got == 200 && sock_error(sender) == 0 && worst < 2000 && (size_t)read_total == (size_t)written);
    close(receiver); close(sender); close(old_client); close(old_served); close(bulk_client); close(bulk_served); close(server);
    puts("[64] FIX LOOPBACK FAIRNESS PASSED");
}

/* wine-15 / wine-51: a duplicate address pair is refused at connect(); SO_REUSEPORT alone shares a port in use. */
static void reuse_rules(void) {
    int server = listener(LOOP, 0, 8, false); uint16_t port = port_of(server), other_port;
    int one = 1, a = tcp_socket(false), b = tcp_socket(false);
    CHECK(setsockopt(a, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 && setsockopt(b, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    struct sockaddr_in self = at(LOOP, 0); CHECK(bind(a, (void*)&self, sizeof(self)) == 0); self = at(LOOP, port_of(a));
    CHECK(bind(b, (void*)&self, sizeof(self)) == 0);   /* both SO_REUSEADDR, neither listening */
    struct sockaddr_in to = at(LOOP, port); CHECK(connect(a, (void*)&to, sizeof(to)) == 0);
    CHECK(connect(b, (void*)&to, sizeof(to)) == -1 && errno == EADDRNOTAVAIL);   /* same four-tuple */
    int second = listener(LOOP, 0, 8, false); other_port = port_of(second); to = at(LOOP, other_port);
    int c = tcp_socket(false); CHECK(setsockopt(c, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 && bind(c, (void*)&self, sizeof(self)) == 0);
    CHECK(connect(c, (void*)&to, sizeof(to)) == 0);   /* another peer: fine */
    int s1 = accept(server, NULL, NULL), s2 = accept(second, NULL, NULL); CHECK(s1 >= 0 && s2 >= 0);
    CHECK(write(a, "1", 1) == 1 && write(c, "2", 1) == 1); char ch;
    CHECK(read(s1, &ch, 1) == 1 && ch == '1' && read(s2, &ch, 1) == 1 && ch == '2');
    close(s1); close(s2); close(a); close(b); close(c); close(second); close(server);
    /* SO_REUSEPORT only: a second listener of the same user joins a port with live connections; clients are shared */
    int l1 = tcp_socket(false); CHECK(setsockopt(l1, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == 0);
    self = at(LOOP, 0); CHECK(bind(l1, (void*)&self, sizeof(self)) == 0 && listen(l1, 16) == 0); port = port_of(l1);
    int live = connect_now(LOOP, port), live_served = accept(l1, NULL, NULL); CHECK(live_served >= 0);
    int l2 = tcp_socket(false); CHECK(setsockopt(l2, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == 0);
    self = at(LOOP, port); CHECK(bind(l2, (void*)&self, sizeof(self)) == 0 && listen(l2, 16) == 0);
    int plain = tcp_socket(false); CHECK(bind(plain, (void*)&self, sizeof(self)) == -1 && errno == EADDRINUSE); close(plain);
    int clients[8], counts[2] = {0, 0}, accepted = 0;
    for (int i = 0; i < 8; i++) {   /* consecutive client ports: the client hash alternates between the listeners */
        clients[i] = tcp_socket(false); struct sockaddr_in from = at(LOOP, (uint16_t)(41000 + i)), to2 = at(LOOP, port);
        CHECK(bind(clients[i], (void*)&from, sizeof(from)) == 0 && connect(clients[i], (void*)&to2, sizeof(to2)) == 0);
    }
    while (accepted < 8) {
        struct pollfd p[2] = {{l1, POLLIN, 0}, {l2, POLLIN, 0}}; CHECK(poll(p, 2, 2000) >= 1);
        for (int k = 0; k < 2; k++) if (p[k].revents & POLLIN) { int s = accept(k ? l2 : l1, NULL, NULL); CHECK(s >= 0); close(s); counts[k]++; accepted++; }
    }
    printf("[64] FIX REUSEPORT: %d + %d connections\n", counts[0], counts[1]);
    CHECK(counts[0] == 4 && counts[1] == 4);
    for (int i = 0; i < 8; i++) close(clients[i]);
    close(live); close(live_served); close(l1); close(l2);
    puts("[64] FIX REUSE RULES PASSED");
}

/* wine-55: a connected, never-bound socket holds only its source address. */
static void connected_source_address(void) {
    int server = listener(SELF, 0, 1, false), client = connect_now(SELF, port_of(server)), served = accept(server, NULL, NULL);
    CHECK(served >= 0); uint16_t ephemeral = port_of(client);
    int other = tcp_socket(false); struct sockaddr_in a = at(LOOP, ephemeral);
    CHECK(bind(other, (void*)&a, sizeof(a)) == 0);   /* 127.0.0.1:E beside 10.0.2.15:E */
    int any = tcp_socket(false); a = at(0, ephemeral); CHECK(bind(any, (void*)&a, sizeof(a)) == -1 && errno == EADDRINUSE);
    close(any); close(other); close(served); close(client); close(server);
    puts("[64] FIX CONNECTED SOURCE ADDRESS PASSED");
}

/* wine-17: without a NIC, loopback still works and other destinations are unreachable. */
static void no_nic(void) {
    int server = listener(LOOP, 0, 1, false), client = connect_now(LOOP, port_of(server)), served = accept(server, NULL, NULL);
    char c; CHECK(served >= 0 && write(client, "n", 1) == 1 && read(served, &c, 1) == 1 && c == 'n');
    close(served); close(client); close(server);
    int u = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a = at(LOOP, 0); CHECK(bind(u, (void*)&a, sizeof(a)) == 0);
    a = at(LOOP, port_of(u)); CHECK(sendto(u, "d", 1, 0, (void*)&a, sizeof(a)) == 1);
    struct pollfd p = {u, POLLIN, 0}; CHECK(poll(&p, 1, 2000) == 1 && recv(u, &c, 1, 0) == 1 && c == 'd'); close(u);
    int far = tcp_socket(false); a = at(0x0A000202U, 80); CHECK(connect(far, (void*)&a, sizeof(a)) == -1 && errno == ENETUNREACH); close(far);
    int self = tcp_socket(false); a = at(SELF, 0); CHECK(bind(self, (void*)&a, sizeof(a)) == -1 && errno == EADDRNOTAVAIL); close(self);
    puts("[64] FIX LOOPBACK WITHOUT NIC PASSED");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char mode[8] = {0}; int config = open("/nexus/inet-fix-mode", O_RDONLY); CHECK(config >= 0 && read(config, mode, sizeof(mode) - 1) > 0); close(config);
    nic = mode[0] == 'n'; measure_only = mode[0] == 'b';
    if (measure_only) { if (mode[1] != 'r') tcp_bulk(); udp_burst(); refused_alias(); puts("[64] INET FIX CAMPAIGN PASSED"); return 0; }
    if (!nic) { no_nic(); refused_alias(); accept_pointers(); accept_queue(); listener_lifecycle(); udp_burst(); tcp_bulk();
                close_with_queue(); closing_response(); fairness(); reuse_rules(); }
    else { accept_pointers(); accept_queue(); listener_lifecycle(); refused_alias(); udp_burst(); tcp_bulk(); addresses();
           close_with_queue(); closing_response(); fairness(); reuse_rules(); connected_source_address(); }
    puts("[64] INET FIX CAMPAIGN PASSED"); return 0;
}
