#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int send_descriptor(int socket, int fd, const char* text) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {(void*)text, strlen(text)};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr* c = CMSG_FIRSTHDR(&message);
    c->cmsg_len = CMSG_LEN(sizeof(int)); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    return sendmsg(socket, &message, MSG_NOSIGNAL);
}
int main(void) {
    int pair[2], yes = 1;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return 1;
    if (!(fcntl(pair[0], F_GETFD) & FD_CLOEXEC)) return 2;
    if (setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &yes, sizeof(yes))) return 3;
    struct ucred peer; socklen_t size = sizeof(peer);
    if (getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.pid != getpid() || peer.uid != getuid()) return 4;
    int original = open("/etc/native64.dat", O_RDONLY);
    if (original < 0 || send_descriptor(pair[0], original, "hello nexus") != 11 || close(original)) return 5;
    char bytes[64] = {0}, control[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(sizeof(int))];
    struct iovec iov = {bytes, sizeof(bytes)};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    if (recvmsg(pair[1], &message, MSG_CMSG_CLOEXEC) != 11 || memcmp(bytes, "hello nexus", 11) || message.msg_flags) return 6;
    int received = -1, credentials = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET) return 7;
        if (c->cmsg_type == SCM_RIGHTS) memcpy(&received, CMSG_DATA(c), sizeof(received));
        if (c->cmsg_type == SCM_CREDENTIALS) {
            struct ucred value; memcpy(&value, CMSG_DATA(c), sizeof(value));
            credentials = value.pid == getpid() && value.uid == getuid() && value.gid == getgid();
        }
    }
    if (received < 0 || !credentials || !(fcntl(received, F_GETFD) & FD_CLOEXEC)) return 8;
    if (read(received, bytes, 1) != 1 || (unsigned char)bytes[0] != 11) return 9;
    int copy = dup(received); if (copy < 0 || fcntl(copy, F_GETFD)) return 10;
    if (lseek(copy, 42, SEEK_SET) != 42 || read(received, bytes, 1) != 1 || (unsigned char)bytes[0] != (unsigned char)(42 * 37 + 11)) return 11;
    if (fcntl(copy, F_SETFL, O_NONBLOCK) || !(fcntl(received, F_GETFL) & O_NONBLOCK)) return 12;
    int saved_stdin = dup(0);
    if (saved_stdin < 0 || dup2(copy, 0) != 0 || read(0, bytes, 1) != 1 || (unsigned char)bytes[0] != (unsigned char)(43 * 37 + 11)) return 13;
    if (dup2(saved_stdin, 0) != 0 || close(saved_stdin) || close(received) || read(copy, bytes, 1) != 1) return 14;
    if (close(copy)) return 15;
    unsigned char large[20000], received_large[20000];
    for (unsigned i = 0; i < sizeof(large); i++) large[i] = (unsigned char)(i * 23 + 5);
    if (send(pair[0], large, sizeof(large), MSG_NOSIGNAL) != sizeof(large) ||
        recv(pair[1], received_large, sizeof(received_large), 0) != sizeof(received_large) ||
        memcmp(large, received_large, sizeof(large))) return 29;
    if (send(pair[0], "peek", 4, MSG_NOSIGNAL) != 4 || recv(pair[1], bytes, sizeof(bytes), MSG_PEEK) != 4 ||
        memcmp(bytes, "peek", 4) || recv(pair[1], bytes, sizeof(bytes), 0) != 4 || memcmp(bytes, "peek", 4)) return 30;
    if (sendmsg(pair[0], (struct msghdr*)0x100000, 0) != -1 || errno != EFAULT) return 31;
    if (recvmsg(pair[1], (struct msghdr*)0x100000, MSG_DONTWAIT) != -1 || errno != EFAULT) return 32;
    char credentials_control[CMSG_SPACE(sizeof(struct ucred))] = {0};
    struct ucred forged = {.pid = getpid(), .uid = 0, .gid = 0};
    struct iovec forged_iov = {"bad", 3};
    struct msghdr forged_message = {.msg_iov = &forged_iov, .msg_iovlen = 1,
                                   .msg_control = credentials_control, .msg_controllen = sizeof(credentials_control)};
    struct cmsghdr* header = CMSG_FIRSTHDR(&forged_message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_CREDENTIALS; header->cmsg_len = CMSG_LEN(sizeof(forged));
    memcpy(CMSG_DATA(header), &forged, sizeof(forged));
    if (sendmsg(pair[0], &forged_message, MSG_NOSIGNAL) != -1 || errno != EPERM) return 33;
    if (send(pair[0], "abcdef", 6, MSG_NOSIGNAL) != 6) return 16;
    iov.iov_len = 3; message.msg_controllen = sizeof(control); message.msg_flags = 0;
    if (recvmsg(pair[1], &message, 0) != 3 || !(message.msg_flags & MSG_TRUNC) || memcmp(bytes, "abc", 3)) return 17;
    if (send(pair[0], "next", 4, MSG_NOSIGNAL) != 4 || recv(pair[1], bytes, sizeof(bytes), 0) != 4 || memcmp(bytes, "next", 4)) return 18;
    if (close(pair[0]) || recv(pair[1], bytes, sizeof(bytes), 0) != 0) return 19;
    if (send(pair[1], "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE || close(pair[1])) return 20;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair)) return 21;
    if (read(pair[0], bytes, 1) != -1 || errno != EAGAIN) return 22;
    if (write(pair[1], "ab", 2) != 2 || write(pair[1], "cd", 2) != 2) return 23;
    int queued = -1; /* FIONREAD counts every queued stream byte (Chrome's SyncSocket::Peek). */
    if (ioctl(pair[0], FIONREAD, &queued) || queued != 4) return 29;
    int count = 0;
    while (count < 4) { int got = read(pair[0], bytes + count, 4 - count); if (got <= 0) return 24; count += got; }
    if (memcmp(bytes, "abcd", 4)) return 25;
    if (ioctl(pair[0], FIONREAD, &queued) || queued != 0) return 30;
    if (shutdown(pair[1], SHUT_WR) || read(pair[0], bytes, 1) != 0 || close(pair[0]) || close(pair[1])) return 26;
    for (unsigned i = 0; i < 64; i++) {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair)) return 27;
        if (send_descriptor(pair[0], pair[1], "cycle") != 5 || close(pair[1]) || close(pair[0])) return 28;
    }
    puts("[64] IPC ALL PASSED: records/streams, credentials, rights, shared offsets, dup/fcntl, EOF and cycle cleanup");
    return 0;
}
