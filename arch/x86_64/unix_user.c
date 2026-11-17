#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/vfs.h>
/* FAT32 roots refuse writes outside the user's areas with EROFS; ext4 roots enforce modes (EACCES). */
static int denied_errno(void) { struct statfs s; return statfs("/", &s) == 0 && s.f_type == 0xEF53 ? EACCES : EROFS; }

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] UNIX FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static struct sockaddr_un endpoint;
static socklen_t endpoint_size;
static int connecting;
static atomic_int started;
static void* blocked_connect(void* unused) {
    (void)unused; atomic_store(&started, 1);
    CHECK(connect(connecting, (struct sockaddr*)&endpoint, endpoint_size) == -1 && errno == ECONNREFUSED);
    return NULL;
}
static int new_socket(int flags) { int fd = socket(AF_UNIX, SOCK_STREAM | flags, 0); CHECK(fd >= 0); return fd; }
static void abstract_tests(void) {
    endpoint.sun_family = AF_UNIX;
    const char name[] = "\0nexus-unix\0suffix";
    memcpy(endpoint.sun_path, name, sizeof(name) - 1); endpoint_size = 2 + sizeof(name) - 1;
    int server = new_socket(SOCK_NONBLOCK | SOCK_CLOEXEC), client = new_socket(SOCK_CLOEXEC);
    char buffer[32]; struct sockaddr_un address; socklen_t size = sizeof(address);
    CHECK(getsockname(server, (struct sockaddr*)&address, &size) == 0 && size == 2 && address.sun_family == AF_UNIX);
    CHECK(getpeername(server, (struct sockaddr*)&address, &size) == -1 && errno == ENOTCONN);
    CHECK(read(server, buffer, 1) == -1 && errno == ENOTCONN && listen(server, 1) == -1 && errno == EINVAL);
    CHECK(bind(server, (struct sockaddr*)&endpoint, endpoint_size) == 0);
    CHECK(bind(client, (struct sockaddr*)&endpoint, endpoint_size) == -1 && errno == EADDRINUSE);
    int prefix = new_socket(0); CHECK(bind(prefix, (struct sockaddr*)&endpoint, 2 + 11) == 0 && close(prefix) == 0);
    int pass = 1; CHECK(setsockopt(server, SOL_SOCKET, SO_PASSCRED, &pass, sizeof(pass)) == 0 && listen(server, 1) == 0);
    CHECK(accept(server, NULL, NULL) == -1 && errno == EAGAIN);
    int ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0);
    struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 71}, event;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, server, &interest) == 0 && epoll_wait(ep, &event, 1, 0) == 0);
    CHECK(connect(client, (struct sockaddr*)&endpoint, endpoint_size) == 0 && epoll_wait(ep, &event, 1, 0) == 1 && event.data.u64 == 71);
    int file = open("/etc/native64.dat", O_RDONLY); CHECK(file >= 0);
    char controls[CMSG_SPACE(sizeof(int))] = {0}; struct iovec vector = {"ok", 2};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_control = controls, .msg_controllen = sizeof(controls)};
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &file, sizeof(file));
    CHECK(sendmsg(client, &message, 0) == 2 && close(file) == 0);
    size = sizeof(address);
    CHECK(syscall(SYS_accept4, server, (void*)1, &size, 0) == -1 && errno == EFAULT);
    size = 1; memset(&address, 0xA5, sizeof(address));
    int accepted = accept4(server, (struct sockaddr*)&address, &size, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(accepted >= 0 && size == 2 && ((unsigned char*)&address)[0] == 1 && ((unsigned char*)&address)[1] == 0xA5);
    CHECK(fcntl(accepted, F_GETFD) == FD_CLOEXEC && (fcntl(accepted, F_GETFL) & O_NONBLOCK));
    CHECK(epoll_wait(ep, &event, 1, 0) == 0);
    struct ucred peer; socklen_t peer_size = sizeof(peer);
    CHECK(getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0 && peer.pid == getpid() && peer.uid == getuid());
    size = sizeof(address); CHECK(getsockname(accepted, (struct sockaddr*)&address, &size) == 0 && size == endpoint_size && !memcmp(&address, &endpoint, size));
    size = sizeof(address); CHECK(getpeername(client, (struct sockaddr*)&address, &size) == 0 && size == endpoint_size && !memcmp(&address, &endpoint, size));
    char received_controls[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))] = {0};
    vector = (struct iovec){buffer, sizeof(buffer)};
    message = (struct msghdr){.msg_iov = &vector, .msg_iovlen = 1, .msg_control = received_controls, .msg_controllen = sizeof(received_controls)};
    CHECK(recvmsg(accepted, &message, MSG_CMSG_CLOEXEC) == 2 && !memcmp(buffer, "ok", 2));
    int received = -1, credentials = 0;
    for (c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_type == SCM_RIGHTS) memcpy(&received, CMSG_DATA(c), sizeof(received));
        if (c->cmsg_type == SCM_CREDENTIALS) { struct ucred value; memcpy(&value, CMSG_DATA(c), sizeof(value)); credentials = value.pid == getpid() && value.uid == getuid(); }
    }
    CHECK(received >= 0 && credentials && fcntl(received, F_GETFD) == FD_CLOEXEC);
    CHECK(read(received, buffer, 1) == 1 && (unsigned char)buffer[0] == 11 && close(received) == 0);
    CHECK(shutdown(client, SHUT_WR) == 0 && read(accepted, buffer, 1) == 0);
    CHECK(close(accepted) == 0 && close(client) == 0 && close(server) == 0 && close(ep) == 0);
    server = new_socket(0); CHECK(bind(server, (struct sockaddr*)&endpoint, endpoint_size) == 0 && listen(server, 0) == 0);
    client = new_socket(0); CHECK(connect(client, (struct sockaddr*)&endpoint, endpoint_size) == 0);
    int second = new_socket(SOCK_NONBLOCK);
    CHECK(connect(second, (struct sockaddr*)&endpoint, endpoint_size) == -1 && errno == EAGAIN);
    accepted = accept(server, NULL, NULL); CHECK(accepted >= 0 && close(accepted) == 0 && close(client) == 0);
    CHECK(connect(second, (struct sockaddr*)&endpoint, endpoint_size) == 0);
    connecting = new_socket(0); pthread_t worker;
    CHECK(pthread_create(&worker, NULL, blocked_connect, NULL) == 0);
    while (!atomic_load(&started)) CHECK(sched_yield() == 0);
    CHECK(sched_yield() == 0 && close(server) == 0 && pthread_join(worker, NULL) == 0 && close(connecting) == 0 && close(second) == 0);
    server = new_socket(0); struct sockaddr_un automatic = {.sun_family = AF_UNIX};
    CHECK(bind(server, (struct sockaddr*)&automatic, 2) == 0);
    size = sizeof(address); CHECK(getsockname(server, (struct sockaddr*)&address, &size) == 0 && size == 8 && address.sun_path[0] == 0 && close(server) == 0);
    for (int i = 0; i < 32; i++) {
        server = new_socket(0); client = new_socket(0);
        CHECK(bind(server, (struct sockaddr*)&endpoint, endpoint_size) == 0 && listen(server, 1) == 0 && connect(client, (struct sockaddr*)&endpoint, endpoint_size) == 0);
        vector = (struct iovec){"x", 1}; memset(controls, 0, sizeof(controls));
        message = (struct msghdr){.msg_iov = &vector, .msg_iovlen = 1, .msg_control = controls, .msg_controllen = sizeof(controls)};
        c = CMSG_FIRSTHDR(&message); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &server, sizeof(server));
        CHECK(sendmsg(client, &message, 0) == 1 && close(server) == 0 && close(client) == 0);
    }
    puts("[64] UNIX ALL PASSED: bind/listen/connect/accept, names, backlog wakeups, pending rights, peer close and accept-queue cycle cleanup");
}
/* SOCK_DGRAM: connectionless sendto/recvfrom with sender addresses, connect as default destination, boundaries and
 * MSG_TRUNC, SCM_CREDENTIALS, queue limit, readiness, errors (syslog's /dev/log, sd_notify, chronyc use these) */
static void datagram_tests(void) {
    struct sockaddr_un rx = {.sun_family = AF_UNIX}, tx = {.sun_family = AF_UNIX}, from;
    memcpy(rx.sun_path, "\0nexus-dgram-rx", 15); memcpy(tx.sun_path, "\0nexus-dgram-tx", 15);
    socklen_t rx_size = 2 + 15, tx_size = 2 + 15, size;
    int receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0), sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(receiver >= 0 && sender >= 0 && bind(receiver, (struct sockaddr*)&rx, rx_size) == 0);
    CHECK(listen(receiver, 1) == -1 && errno == EOPNOTSUPP);
    char buffer[64];
    CHECK(recv(receiver, buffer, sizeof(buffer), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(sendto(sender, "hello", 5, 0, (struct sockaddr*)&rx, rx_size) == 5);
    struct pollfd p = {receiver, POLLIN, 0}; CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    size = sizeof(from); CHECK(recvfrom(receiver, buffer, sizeof(buffer), 0, (struct sockaddr*)&from, &size) == 5 && !memcmp(buffer, "hello", 5) && size == 2);
    CHECK(bind(sender, (struct sockaddr*)&tx, tx_size) == 0 && sendto(sender, "ping", 4, 0, (struct sockaddr*)&rx, rx_size) == 4);
    size = sizeof(from); CHECK(recvfrom(receiver, buffer, sizeof(buffer), 0, (struct sockaddr*)&from, &size) == 4 && size == tx_size && !memcmp(&from, &tx, tx_size));
    CHECK(sendto(receiver, "pong", 4, 0, (struct sockaddr*)&from, size) == 4 && recv(sender, buffer, sizeof(buffer), 0) == 4 && !memcmp(buffer, "pong", 4));
    /* connect: default destination; AF_UNSPEC dissolves it */
    CHECK(send(sender, "x", 1, 0) == -1 && errno == ENOTCONN);
    CHECK(connect(sender, (struct sockaddr*)&rx, rx_size) == 0 && send(sender, "0123456789", 10, 0) == 10);
    size = sizeof(from); CHECK(getpeername(sender, (struct sockaddr*)&from, &size) == 0 && size == rx_size && !memcmp(&from, &rx, rx_size));
    struct iovec vector = {buffer, 4}; struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    CHECK(recvmsg(receiver, &message, 0) == 4 && (message.msg_flags & MSG_TRUNC));            /* the rest of the datagram is gone */
    CHECK(send(sender, "abcdefgh", 8, 0) == 8 && recv(receiver, buffer, 2, MSG_TRUNC) == 8);  /* MSG_TRUNC: the real length */
    struct sockaddr unspec = {.sa_family = AF_UNSPEC};
    CHECK(connect(sender, &unspec, sizeof(unspec)) == 0 && send(sender, "x", 1, 0) == -1 && errno == ENOTCONN);
    /* SCM_CREDENTIALS */
    int on = 1; CHECK(setsockopt(receiver, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) == 0);
    CHECK(sendto(sender, "cred", 4, 0, (struct sockaddr*)&rx, rx_size) == 4);
    char control[CMSG_SPACE(sizeof(struct ucred))]; vector = (struct iovec){buffer, sizeof(buffer)};
    message = (struct msghdr){.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &vector, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(receiver, &message, 0) == 4 && message.msg_namelen == tx_size);
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); CHECK(c && c->cmsg_type == SCM_CREDENTIALS);
    struct ucred cred; memcpy(&cred, CMSG_DATA(c), sizeof(cred)); CHECK(cred.pid == getpid() && cred.uid == getuid());
    /* queue limit, then room again */
    int sent = 0; while (sendto(sender, "q", 1, MSG_DONTWAIT, (struct sockaddr*)&rx, rx_size) == 1) sent++;
    CHECK(errno == EAGAIN && sent == 512 && recv(receiver, buffer, sizeof(buffer), 0) == 1 && sendto(sender, "q", 1, MSG_DONTWAIT, (struct sockaddr*)&rx, rx_size) == 1);
    /* errors: nobody bound, a stream socket bound */
    struct sockaddr_un nobody = {.sun_family = AF_UNIX}; memcpy(nobody.sun_path, "\0nexus-dgram-none", 17);
    CHECK(sendto(sender, "x", 1, 0, (struct sockaddr*)&nobody, 2 + 17) == -1 && errno == ECONNREFUSED);
    int stream = new_socket(0); CHECK(bind(stream, (struct sockaddr*)&nobody, 2 + 17) == 0 && listen(stream, 1) == 0);
    CHECK(sendto(sender, "x", 1, 0, (struct sockaddr*)&nobody, 2 + 17) == -1 && errno == EPROTOTYPE && close(stream) == 0);
    CHECK(close(receiver) == 0);
    CHECK(sendto(sender, "x", 1, 0, (struct sockaddr*)&rx, rx_size) == -1 && errno == ECONNREFUSED && close(sender) == 0);
    /* socketpair: connected, boundaries kept */
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    CHECK(write(pair[0], "one", 3) == 3 && write(pair[0], "two!", 4) == 4 && read(pair[1], buffer, sizeof(buffer)) == 3 && read(pair[1], buffer, sizeof(buffer)) == 4);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    puts("[64] UNIX DATAGRAM PASSED: sendto/recvfrom addresses, connect/AF_UNSPEC, MSG_TRUNC, SCM_CREDENTIALS, queue limit, errors, socketpair");
}
static void named_tests(void) {
    const char* root = "/tmp/native64-unix"; CHECK(mkdir(root, 0700) == 0);
    int directory = open(root, O_RDONLY | O_DIRECTORY); CHECK(directory >= 0);
    struct stat removed;
    CHECK(rmdir(root) == 0 && fstat(directory, &removed) == 0 && removed.st_nlink == 0);
    CHECK(mkdir(root, 0700) == 0 && openat(directory, "new", O_CREAT | O_WRONLY, 0600) == -1 && errno == ENOENT);
    CHECK(close(directory) == 0);
    CHECK(symlink("file", "/tmp/native64-unix/link") == 0);
    struct stat st; CHECK(lstat("/tmp/native64-unix/link", &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(stat("/tmp/native64-unix/link", &st) == -1 && errno == ENOENT);
    int file = open("/tmp/native64-unix/link", O_CREAT | O_EXCL | O_WRONLY, 0600); CHECK(file == -1 && errno == EEXIST);
    CHECK(open("/tmp/native64-unix/link", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    file = open("/tmp/native64-unix/link", O_CREAT | O_RDWR, 0600); CHECK(file >= 0 && write(file, "data", 4) == 4);
    CHECK(unlink("/tmp/native64-unix/file") == 0 && fstat(file, &removed) == 0 && removed.st_nlink == 0);
    char deleted_data[4]; CHECK(pread(file, deleted_data, 4, 0) == 4 && !memcmp(deleted_data, "data", 4));
    int new_file = open("/tmp/native64-unix/link", O_CREAT | O_RDWR, 0600);
    CHECK(new_file >= 0 && fstat(new_file, &st) == 0 && st.st_ino != removed.st_ino);
    CHECK(write(new_file, "data", 4) == 4 && close(new_file) == 0 && close(file) == 0);
    CHECK(stat("/tmp/native64-unix/link", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 4);
    CHECK(symlink("loop", "/tmp/native64-unix/loop") == 0 && open("/tmp/native64-unix/loop", O_RDONLY) == -1 && errno == ELOOP);
    CHECK(symlink("/etc", "/tmp/native64-unix/out") == 0);
    CHECK(open("/tmp/native64-unix/out/forbidden", O_CREAT | O_WRONLY, 0600) == -1 && errno == denied_errno());
    char target[64] = {0}; CHECK(readlink("/tmp/native64-unix/link", target, sizeof(target)) == 4 && !strcmp(target, "file"));
    CHECK(unlink("/tmp/native64-unix/out") == 0 && unlink("/tmp/native64-unix/loop") == 0 && unlink("/tmp/native64-unix/link") == 0 && unlink("/tmp/native64-unix/file") == 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX}; strcpy(address.sun_path, "/tmp/native64-unix/server.sock");
    socklen_t size = 2 + strlen(address.sun_path) + 1;
    int server = new_socket(0); CHECK(bind(server, (struct sockaddr*)&address, size) == 0 && listen(server, 1) == 0);
    CHECK(stat(address.sun_path, &st) == 0 && S_ISSOCK(st.st_mode));
    CHECK(open(address.sun_path, O_RDWR) == -1 && errno == ENXIO);
    CHECK(symlink("server.sock", "/tmp/native64-unix/alias") == 0);
    struct sockaddr_un alias = {.sun_family = AF_UNIX}; strcpy(alias.sun_path, "/tmp/native64-unix/alias");
    int client = new_socket(0); CHECK(connect(client, (struct sockaddr*)&alias, 2 + strlen(alias.sun_path) + 1) == 0);
    int accepted = accept(server, NULL, NULL); CHECK(accepted >= 0);
    CHECK(unlink(address.sun_path) == 0);
    int replacement = new_socket(0); CHECK(bind(replacement, (struct sockaddr*)&address, size) == 0 && listen(replacement, 1) == 0);
    CHECK(write(client, "old", 3) == 3); char data[8]; CHECK(read(accepted, data, sizeof(data)) == 3 && !memcmp(data, "old", 3));
    int next_client = new_socket(0); CHECK(connect(next_client, (struct sockaddr*)&alias, 2 + strlen(alias.sun_path) + 1) == 0);
    int next_accepted = accept(replacement, NULL, NULL); CHECK(next_accepted >= 0);
    CHECK(close(server) == 0 && close(client) == 0 && close(accepted) == 0 && close(replacement) == 0 && close(next_client) == 0 && close(next_accepted) == 0);
    CHECK(stat(address.sun_path, &st) == 0 && S_ISSOCK(st.st_mode));
    client = new_socket(0); CHECK(connect(client, (struct sockaddr*)&address, size) == -1 && errno == ECONNREFUSED && close(client) == 0);
    CHECK(unlink(address.sun_path) == 0 && unlink("/tmp/native64-unix/alias") == 0 && rmdir(root) == 0);
    strcpy(address.sun_path, "/home/nexus/native64-persist/stale.sock"); size = 2 + strlen(address.sun_path) + 1;
    if (lstat(address.sun_path, &st) == 0) {
        CHECK(S_ISSOCK(st.st_mode)); client = new_socket(0);
        CHECK(connect(client, (struct sockaddr*)&address, size) == -1 && errno == ECONNREFUSED && close(client) == 0 && unlink(address.sun_path) == 0);
        puts("[64] UNIX persistent socket type survived reboot PASS");
    } else CHECK(errno == ENOENT);
    server = new_socket(0); CHECK(bind(server, (struct sockaddr*)&address, size) == 0 && close(server) == 0);
    const char* persisted_link = "/home/nexus/native64-persist/data-link";
    if (lstat(persisted_link, &st) == 0) {
        CHECK(S_ISLNK(st.st_mode));
        memset(target, 0, sizeof(target)); CHECK(readlink(persisted_link, target, sizeof(target)) == 8 && !strcmp(target, "data.bin"));
        file = open(persisted_link, O_RDONLY); CHECK(file >= 0 && read(file, data, 2) == 2 && data[0] == 7 && data[1] == 'Z' && close(file) == 0);
        puts("[64] UNIX persistent symlink survived reboot PASS");
    } else { CHECK(errno == ENOENT); CHECK(symlink("data.bin", persisted_link) == 0); }
    puts("[64] UNIX NAMED PASSED: persisted inode type, symlinks, permission resolution, unlink/rebind and live connection identity");
}
int main(int argc, char** argv) { CHECK(argc == 2); if (argv[1][0] == 'd') named_tests(); else { abstract_tests(); datagram_tests(); } return 0; }
