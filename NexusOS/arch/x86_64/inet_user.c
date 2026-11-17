#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] INET FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static struct sockaddr_in peer;
static int ep;
static volatile sig_atomic_t pipe_signals, user_signals;
static void signal_handler(int number) { if (number == SIGPIPE) pipe_signals++; else user_signals++; }
static uint64_t now(void) { struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static int socket_error(int fd) { int value = -1; socklen_t size = 4; CHECK(getsockopt(fd, SOL_SOCKET, SO_ERROR, &value, &size) == 0 && size == 4); return value; }
static void interest(int fd, unsigned events, bool add) {
    struct epoll_event item = {.events = events, .data.fd = fd}; CHECK(epoll_ctl(ep, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &item) == 0);
}
static unsigned wait_event(int fd) { struct epoll_event event; CHECK(epoll_wait(ep, &event, 1, 45000) == 1 && event.data.fd == fd); return event.events; }
static int connect_to(bool nonblock) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), IPPROTO_TCP); CHECK(fd >= 0);
    if (nonblock) {
        CHECK(connect(fd, (void*)&peer, sizeof(peer)) == -1 && errno == EINPROGRESS);
        interest(fd, EPOLLOUT, true); CHECK(wait_event(fd) & EPOLLOUT); CHECK(socket_error(fd) == 0);
    } else CHECK(connect(fd, (void*)&peer, sizeof(peer)) == 0);
    return fd;
}
static void close_fd(int fd) { CHECK(close(fd) == 0); }
static void validations(void) {
    CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 && errno == EPROTONOSUPPORT);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); CHECK(fd >= 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK(syscall(SYS_connect, fd, (void*)1, sizeof(peer)) == -1 && errno == EFAULT);
    struct sockaddr_in address = {.sin_family = AF_INET};
    CHECK(bind(fd, (void*)&address, sizeof(address)) == 0);
    socklen_t size = sizeof(address); CHECK(getsockname(fd, (void*)&address, &size) == 0 && size == 16 && address.sin_port != 0);
    int other = socket(AF_INET, SOCK_STREAM, 0); CHECK(other >= 0);
    CHECK(bind(other, (void*)&address, sizeof(address)) == -1 && errno == EADDRINUSE);
    address.sin_port = htons(80); CHECK(bind(other, (void*)&address, sizeof(address)) == -1 && errno == EACCES);
    char buffer[32]; CHECK(recv(fd, buffer, 1, MSG_DONTWAIT) == -1 && errno == ENOTCONN);
    CHECK(getpeername(fd, (void*)&address, &size) == -1 && errno == ENOTCONN);
    {   /* TCP servers (inet.c listen/accept): a loopback listener accepts a local connection */
        struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; socklen_t length = sizeof(local);
        int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); CHECK(server >= 0);
        CHECK(bind(server, (void*)&local, sizeof(local)) == 0 && listen(server, 1) == 0);
        CHECK(getsockname(server, (void*)&local, &length) == 0 && local.sin_port != 0);
        int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); CHECK(client >= 0 && connect(client, (void*)&local, sizeof(local)) == 0);
        int served = accept4(server, NULL, NULL, SOCK_CLOEXEC); CHECK(served >= 0);
        CHECK(write(client, "L", 1) == 1 && read(served, buffer, 1) == 1 && buffer[0] == 'L');
        close_fd(served); close_fd(client); close_fd(server);
    }
    struct stat st; CHECK(fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode));
    snprintf(buffer, sizeof(buffer), "/proc/self/fd/%d", fd); char path[64]; ssize_t n = readlink(buffer, path, sizeof(path));
    CHECK(n > 8 && !memcmp(path, "socket:[", 8));
    int value = 1; CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) == 0);
    value = 0; size = sizeof(value); CHECK(getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0 && value == 1);
    struct timeval timeout = {.tv_usec = 1000000}; CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 && errno == EDOM);
    CHECK(syscall(SYS_getsockopt, fd, SOL_SOCKET, SO_ERROR, &value, (void*)1) == -1 && errno == EFAULT);
    /* Keepalive knobs, buffer sizing and TCP_INFO as Chrome's network service uses them. */
    value = 1; CHECK(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value)) == 0);
    value = 0; size = sizeof(value); CHECK(getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, &size) == 0 && value == 1);
    value = 45; CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value)) == 0);
    value = 45; CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value)) == 0);
    value = 0; CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value)) == -1 && errno == EINVAL);
    value = 128; CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value)) == -1 && errno == EINVAL);
    value = 0; size = sizeof(value); CHECK(getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, &size) == 0 && value == 45);
    value = 0; CHECK(getsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, &size) == 0 && value == 9);
    value = 100000; CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) == 0);
    value = 0; CHECK(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, &size) == 0 && value == 200704);
    value = 0; CHECK(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, &size) == 0 && value == 262144);
    struct tcp_info info; memset(&info, 0xFF, sizeof(info)); size = sizeof(info);
    CHECK(getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &size) == 0 && size == sizeof(info) && info.tcpi_state == 7 && info.tcpi_rtt == 0);
    close_fd(other); close_fd(fd);
}
/* Linux UAPI tcp_info (248 bytes): read fields by offset, as glibc's struct is shorter. */
static uint64_t info_field(int fd, unsigned offset, unsigned bytes) {
    uint8_t info[256]; socklen_t size = sizeof(info); memset(info, 0, sizeof(info));
    CHECK(getsockopt(fd, IPPROTO_TCP, TCP_INFO, info, &size) == 0 && size == 248);
    uint64_t value = 0; memcpy(&value, info + offset, bytes); return value;
}
static void stream(void) {
    int fd = connect_to(true), alias = dup(fd); CHECK(alias >= 0);
    int value = 1; CHECK(ioctl(alias, FIONBIO, &value) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    struct sockaddr_in address; socklen_t size = sizeof(address);
    CHECK(getsockname(fd, (void*)&address, &size) == 0 && address.sin_addr.s_addr == htonl(0x0a00020f) && address.sin_port);
    size = sizeof(address); CHECK(getpeername(fd, (void*)&address, &size) == 0 && address.sin_port == peer.sin_port && address.sin_addr.s_addr == peer.sin_addr.s_addr);
    size = 2; memset(&address, 0xAA, sizeof(address)); CHECK(getpeername(fd, (void*)&address, &size) == 0 && size == 16 && address.sin_family == AF_INET && address.sin_port == 0xAAAA);
    CHECK(info_field(fd, 0, 1) == 1 && info_field(fd, 16, 4) >= 536 && info_field(fd, 228, 4) > 0);   /* ESTABLISHED, MSS, peer window */
    size = sizeof(value); CHECK(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, &size) == 0 && value == 262144);
    value = 1; CHECK(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value)) == 0);
    CHECK(send(fd, "S", 1, MSG_NOSIGNAL) == 1); interest(fd, EPOLLIN | EPOLLRDHUP | EPOLLET, false);
    CHECK(wait_event(fd) & EPOLLIN);
    char tiny[16] = {0}; CHECK(recv(fd, tiny, 9, MSG_PEEK) == 9 && !memcmp(tiny, "peek-data", 9));
    CHECK(ioctl(fd, FIONREAD, &value) == 0 && value == 9);
    CHECK(syscall(SYS_recvfrom, fd, (void*)1, 9, 0, 0, 0) == -1 && errno == EFAULT);
    struct iovec pieces[2] = {{tiny, 3}, {tiny + 3, 6}};
    CHECK(readv(fd, pieces, 2) == 9 && !memcmp(tiny, "peek-data", 9));
    CHECK(recv(fd, tiny, sizeof(tiny), 0) == -1 && errno == EAGAIN);
    value = 0; CHECK(ioctl(alias, FIONBIO, &value) == 0 && !(fcntl(fd, F_GETFL) & O_NONBLOCK));
    struct timeval timeout = {.tv_usec = 120000}; CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint64_t before = now(); CHECK(recv(fd, tiny, 1, 0) == -1 && errno == EAGAIN && now() - before >= 100000000);
    timeout.tv_usec = 0; CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    value = 1; CHECK(ioctl(alias, FIONBIO, &value) == 0);
    CHECK(sendmsg(fd, &(struct msghdr){.msg_iov = &(struct iovec){"GO", 2}, .msg_iovlen = 1}, MSG_NOSIGNAL) == 2);
    uint8_t data[32768]; uint64_t total = 0;
    interest(fd, EPOLLOUT, false);
    while (total < 2 * 1024 * 1024) {
        unsigned bytes = sizeof(data); if (bytes > 2 * 1024 * 1024 - total) bytes = 2 * 1024 * 1024 - total;
        for (unsigned i = 0; i < bytes; i++) data[i] = ((total + i) * 37 + 11) & 255;
        struct iovec vectors[2] = {{data, bytes / 3}, {data + bytes / 3, bytes - bytes / 3}};
        ssize_t sent = writev(fd, vectors, 2);
        if (sent < 0) { CHECK(errno == EAGAIN); CHECK(wait_event(fd) & EPOLLOUT); }
        else { CHECK(sent > 0 && sent <= bytes); total += sent; }
    }
    total = 0; interest(fd, EPOLLIN | EPOLLRDHUP | EPOLLET, false);
    for (;;) {
        struct iovec vectors[3] = {{data, 13}, {data + 13, 4000}, {data + 4013, sizeof(data) - 4013}};
        struct sockaddr_in from = {0}; char ancillary[64];
        struct msghdr message = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = vectors, .msg_iovlen = 3, .msg_control = ancillary, .msg_controllen = sizeof(ancillary)};
        ssize_t got = recvmsg(fd, &message, 0);
        if (got < 0) { CHECK(errno == EAGAIN); CHECK(wait_event(fd) & (EPOLLIN | EPOLLRDHUP)); continue; }
        CHECK(message.msg_namelen == 0 && message.msg_controllen == 0 && message.msg_flags == 0);
        if (!got) break;
        for (ssize_t i = 0; i < got; i++) CHECK(data[i] == (uint8_t)(((total + i) * 37 + 11) & 255));
        total += got; CHECK(total <= 2 * 1024 * 1024);
    }
    CHECK(total == 2 * 1024 * 1024);
    CHECK(info_field(fd, 120, 8) >= 2 * 1024 * 1024 + 3 && info_field(fd, 128, 8) >= 2 * 1024 * 1024 + 9);   /* bytes_acked / bytes_received */
    CHECK(info_field(fd, 68, 4) > 0 && info_field(fd, 148, 4) > 0);   /* data RTT samples: tcpi_rtt, tcpi_min_rtt */
    CHECK(send(fd, "TAIL", 4, MSG_NOSIGNAL) == 4 && shutdown(alias, SHUT_WR) == 0);
    CHECK(send(fd, "X", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE && !pipe_signals);
    CHECK(write(fd, "X", 1) == -1 && errno == EPIPE && pipe_signals == 1);
    close_fd(fd); CHECK(recv(alias, tiny, 1, 0) == 0); close_fd(alias);
    puts("[64] INET 2 MIB UPLOAD/DOWNLOAD, EPOLL, ALIASES AND HALF-CLOSE PASSED");
}
static pthread_t main_thread;
static int worker_fd;
static void* interrupt_reader(void* shutdown_read) {
    CHECK(poll(NULL, 0, 120) == 0);
    if (shutdown_read) CHECK(shutdown(worker_fd, SHUT_RD) == 0);
    else CHECK(pthread_kill(main_thread, SIGUSR1) == 0);
    return NULL;
}
static void blocking(void) {
    int fd = connect_to(false); CHECK(send(fd, "W", 1, 0) == 1);
    char data[16] = {0}; CHECK(recv(fd, data, 8, MSG_PEEK | MSG_WAITALL) == 8 && !memcmp(data, "wait-all", 8));
    CHECK(shutdown(fd, SHUT_RD) == 0);
    CHECK(recv(fd, data, 8, MSG_WAITALL) == 8 && !memcmp(data, "wait-all", 8));
    CHECK(recv(fd, data, 1, 0) == 0); close_fd(fd);
    fd = connect_to(false); CHECK(send(fd, "I", 1, 0) == 1); main_thread = pthread_self(); pthread_t worker;
    CHECK(pthread_create(&worker, NULL, interrupt_reader, NULL) == 0);
    CHECK(recv(fd, data, 1, 0) == -1 && errno == EINTR && user_signals == 1); CHECK(pthread_join(worker, NULL) == 0);
    worker_fd = dup(fd); CHECK(worker_fd >= 0); CHECK(pthread_create(&worker, NULL, interrupt_reader, (void*)1) == 0);
    CHECK(recv(fd, data, 1, 0) == 0); CHECK(pthread_join(worker, NULL) == 0);
    CHECK(send(fd, "D", 1, MSG_NOSIGNAL) == 1 && shutdown(fd, SHUT_WR) == 0);
    close_fd(worker_fd); close_fd(fd);
    puts("[64] INET WAITALL, TIMEOUT, SIGNAL AND SHUTDOWN WAKE PASSED");
}
static void refusal(unsigned port) {
    struct sockaddr_in address = peer; address.sin_port = htons(port);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); CHECK(fd >= 0);
    CHECK(connect(fd, (void*)&address, sizeof(address)) == -1 && errno == EINPROGRESS);
    interest(fd, EPOLLOUT, true); CHECK(wait_event(fd) & EPOLLERR);
    socklen_t size = 4; CHECK(syscall(SYS_getsockopt, fd, SOL_SOCKET, SO_ERROR, (void*)1, &size) == -1 && errno == EFAULT);
    int error = socket_error(fd); printf("[64] INET CONNECT ERROR %d\n", error);
    CHECK((error == ECONNREFUSED || error == ETIMEDOUT) && socket_error(fd) == 0); close_fd(fd);
    fd = connect_to(false); CHECK(send(fd, "R", 1, 0) == 1);
    char buffer[16]; CHECK(recv(fd, buffer, 5, MSG_WAITALL) == 5 && !memcmp(buffer, "reset", 5));
    CHECK(recv(fd, buffer, 1, 0) == -1 && errno == ECONNRESET);
    size = sizeof(address); CHECK(getpeername(fd, (void*)&address, &size) == -1 && errno == ENOTCONN);
    CHECK(socket_error(fd) == 0 && recv(fd, buffer, 1, 0) == 0); close_fd(fd);
    puts("[64] INET CONNECT AND RESET ERRORS PASSED");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned short ports[2]; int config = open("/nexus/inet-ports", O_RDONLY); CHECK(config >= 0 && read(config, ports, sizeof(ports)) == sizeof(ports)); close_fd(config);
    peer = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(ports[0]), .sin_addr.s_addr = htonl(0x0a000202)};
    struct sigaction action = {.sa_handler = signal_handler}; CHECK(sigaction(SIGPIPE, &action, NULL) == 0 && sigaction(SIGUSR1, &action, NULL) == 0);
    ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0);
    validations(); stream(); blocking(); refusal(ports[1]); close_fd(ep);
    CHECK(poll(NULL, 0, 250) == 0);
    puts("[64] INET SOCKET CAMPAIGN PASSED"); return 0;
}
