/* Bluetooth HCI test (tests/test_x64_bluetooth.py), run as /bin/hw64 (root: /nexus/init-as-root) by /nexus/test-hw.
   Two virtual controllers on /dev/vhci; a forked child plays both controllers (a tiny btvirt): it answers the init
   commands, Read Local Name, Inquiry (each controller "sees" the other one: Inquiry Result with the peer's BD_ADDR)
   and relays ACL data between them (an HCI-level echo link). The parent is the host: HCIGETDEVLIST/HCIGETDEVINFO,
   HCIDEVUP/DOWN, the mgmt subset (READ_VERSION, READ_INDEX_LIST, READ_INFO, SET_POWERED + New Settings), raw sockets
   with HCI_FILTER (inquiry = scan finds the peer, ACL across), the user channel (exclusive, device down), the monitor
   channel (btmon framing), hostile requests, unprivileged denials and unplug (ENODEV/POLLHUP, DEL_INDEX, Index Removed). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;
#define CHECK(test, ...) do { if (!(test)) { printf("[64] BT FAIL line=%d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fflush(stdout); failures++; } } while (0)
#define AF_BT 31
#define BTPROTO_HCI 1
#define HCIDEVUP 0x400448C9
#define HCIDEVDOWN 0x400448CA
#define HCIGETDEVLIST 0x800448D2
#define HCIGETDEVINFO 0x800448D3
struct sockaddr_hci { uint16_t family, dev, channel; };
struct hci_filter { uint32_t type_mask, event_mask[2]; uint16_t opcode; };
static const uint8_t ADDR[2][6] = {{0x01, 0x00, 0x00, 0x01, 0xAA, 0x00}, {0x02, 0x00, 0x00, 0x01, 0xAA, 0x00}};   /* 00:AA:01:00:00:0N */
static unsigned get16(const uint8_t* p) { return p[0] | (unsigned)p[1] << 8; }

/* ---------- the controllers (child) ---------- */
static int vhci[2];
static void event(int c, const uint8_t* data, unsigned length) { uint8_t p[300]; p[0] = 4; memcpy(p + 1, data, length); if (write(vhci[c], p, length + 1) < 0) _exit(90); }
static void complete(int c, unsigned opcode, const uint8_t* params, unsigned length) {
    uint8_t e[260] = {0x0E, (uint8_t)(3 + length), 1, (uint8_t)opcode, (uint8_t)(opcode >> 8)}; memcpy(e + 5, params, length); event(c, e, 5 + length);
}
static void controllers(void) {
    for (;;) {
        struct pollfd p[2] = {{vhci[0], POLLIN, 0}, {vhci[1], POLLIN, 0}};
        if (poll(p, 2, 1000) < 0 && errno != EINTR) _exit(91);
        for (int c = 0; c < 2; c++) {
            if (p[c].revents & (POLLHUP | POLLERR | POLLNVAL)) _exit(0);
            if (!(p[c].revents & POLLIN)) continue;
            uint8_t b[1100]; ssize_t n = read(vhci[c], b, sizeof(b));
            if (n <= 0) { if (n < 0 && errno == EAGAIN) continue; _exit(0); }
            if (b[0] == 2) { if (write(vhci[!c], b, (size_t)n) != n) _exit(92); continue; }   /* ACL relayed to the peer */
            if (b[0] != 1 || n < 4) continue;
            unsigned opcode = get16(b + 1); uint8_t r[260] = {0};
            switch (opcode) {
            case 0x0C03: case 0x0C01: complete(c, opcode, r, 1); break;                         /* Reset, Set Event Mask */
            case 0x1001: { uint8_t v[9] = {0, 0x0C, 0x34, 0x12, 0x0C, 0x02, 0x00, 0x78, 0x56}; complete(c, opcode, v, 9); break; }
            case 0x1003: { uint8_t f[9] = {0, 0xFF, 0xFF, 0x8F, 0xFE, 0xDB, 0xFF, 0x5B, 0x87}; complete(c, opcode, f, 9); break; }
            case 0x1005: { uint8_t s[8] = {0, 0xFD, 0x03, 0x40, 8, 0, 8, 0}; complete(c, opcode, s, 8); break; }
            case 0x1009: r[0] = 0; memcpy(r + 1, ADDR[c], 6); complete(c, opcode, r, 7); break;
            case 0x0C14: r[0] = 0; strcpy((char*)r + 1, c ? "NexusOS Peer" : "NexusOS Host"); complete(c, opcode, r, 249); break;
            case 0x0401: {                                                                      /* Inquiry: the peer answers */
                uint8_t status[4] = {0x0F, 4, 0, 1}; uint8_t s2[6]; memcpy(s2, status, 4); s2[4] = (uint8_t)opcode; s2[5] = (uint8_t)(opcode >> 8);
                event(c, s2, 6);
                uint8_t result[17] = {0x02, 15, 1}; memcpy(result + 3, ADDR[!c], 6); result[9] = 1; result[12] = 0x0C; result[13] = 0x02; result[14] = 0x5A;
                event(c, result, 17);
                uint8_t done[3] = {0x01, 1, 0}; event(c, done, 3); break;
            }
            default: r[0] = 0x01; complete(c, opcode, r, 1);                                     /* unknown command */
            }
        }
    }
}

/* ---------- host side ---------- */
static int hci_socket(unsigned dev, unsigned channel) {
    int fd = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI); if (fd < 0) return -1;
    struct sockaddr_hci a = {AF_BT, (uint16_t)dev, (uint16_t)channel};
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}
static int read_timeout(int fd, uint8_t* out, unsigned size, int ms) {
    struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, ms) <= 0) return -2;
    return (int)read(fd, out, size);
}
static int mgmt(int fd, unsigned opcode, unsigned index, const uint8_t* params, unsigned length, uint8_t* reply, unsigned size) {
    uint8_t m[64] = {(uint8_t)opcode, (uint8_t)(opcode >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, 0}; memcpy(m + 6, params, length);
    if (write(fd, m, 6 + length) != (ssize_t)(6 + length)) return -1;
    for (int i = 0; i < 20; i++) {
        int n = read_timeout(fd, reply, size, 2000); if (n < 0) return -1;
        if (n < 9) continue;                                            /* an event (Index Added/Removed ...) */
        if ((get16(reply) == 1 || get16(reply) == 2) && get16(reply + 6) == opcode) return n;   /* Command Complete / Status */
    }
    return -1;
}
static bool devinfo(unsigned dev, uint8_t* info) { int fd = hci_socket(0xFFFF, 0); memset(info, 0, 92); info[0] = (uint8_t)dev; int r = fd >= 0 ? ioctl(fd, HCIGETDEVINFO, info) : -1; if (fd >= 0) close(fd); return r == 0; }

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[64] BT TEST START uid=%d\n", (int)getuid());
    CHECK(getuid() == 0, "runs as root (test disk /nexus/init-as-root)");
    errno = 0; CHECK(socket(AF_BT, SOCK_SEQPACKET, 2) < 0 && errno == EPROTONOSUPPORT, "SCO sockets are not offered");
    int monitor = hci_socket(0xFFFF, 2); CHECK(monitor >= 0, "monitor channel");
    int control = hci_socket(0xFFFF, 3); CHECK(control >= 0, "control channel");
    for (int c = 0; c < 2; c++) {
        vhci[c] = open("/dev/vhci", O_RDWR); CHECK(vhci[c] >= 0, "open /dev/vhci");
        uint8_t create[2] = {0xFF, 0x00}; CHECK(write(vhci[c], create, 2) == 2, "create controller");
        uint8_t reply[4] = {0}; CHECK(read(vhci[c], reply, 4) == 4 && reply[0] == 0xFF && get16(reply + 2) == (unsigned)c, "vendor reply index %u", get16(reply + 2));
    }
    errno = 0; uint8_t again[2] = {0xFF, 0}; CHECK(write(vhci[0], again, 2) < 0 && errno == EINVAL, "second create");
    uint8_t bad_event[5] = {4, 0x0E, 9, 1, 2}; errno = 0; CHECK(write(vhci[0], bad_event, 5) < 0 && errno == EINVAL, "event length must match");
    uint8_t bad_type[4] = {9, 1, 2, 3}; errno = 0; CHECK(write(vhci[0], bad_type, 4) < 0 && errno == EINVAL, "unknown packet type");
    pid_t child = fork();
    if (!child) controllers();
    for (int c = 0; c < 2; c++) fcntl(vhci[c], F_SETFL, O_NONBLOCK);
    usleep(300000);                                                       /* the controllers' setup (identity read at registration) */
    /* device list, mgmt */
    int raw = hci_socket(0xFFFF, 0); CHECK(raw >= 0, "raw socket without device");
    uint8_t list[4 + 8 * 4] = {4, 0};
    CHECK(raw >= 0 && !ioctl(raw, HCIGETDEVLIST, list) && get16(list) == 2 && get16(list + 4) == 0 && get16(list + 12) == 1, "HCIGETDEVLIST %u", get16(list));
    uint8_t zero[4] = {0}; errno = 0; CHECK(ioctl(raw, HCIGETDEVLIST, zero) < 0 && errno == EINVAL, "dev_num 0");
    errno = 0; CHECK(ioctl(raw, HCIGETDEVINFO, (void*)8) < 0 && errno == EFAULT, "HCIGETDEVINFO bad pointer");
    uint8_t r[300];
    CHECK(mgmt(control, 0x0001, 0xFFFF, NULL, 0, r, sizeof(r)) >= 12 && r[8] == 0 && r[9] == 1, "mgmt READ_VERSION %u.%u", r[9], get16(r + 10));
    CHECK(mgmt(control, 0x0003, 0xFFFF, NULL, 0, r, sizeof(r)) >= 13 && get16(r + 9) == 2, "mgmt READ_INDEX_LIST %u", get16(r + 9));
    CHECK(mgmt(control, 0x0004, 1, NULL, 0, r, sizeof(r)) > 20 && r[8] == 0 && !(r[22] & 1), "mgmt READ_INFO hci1 (down)");
    uint8_t on = 1; CHECK(mgmt(control, 0x0005, 1, &on, 1, r, sizeof(r)) >= 13 && r[8] == 0 && (r[9] & 1), "mgmt SET_POWERED hci1");
    CHECK(mgmt(control, 0x0004, 1, NULL, 0, r, sizeof(r)) > 20 && !memcmp(r + 9, ADDR[1], 6) && (r[22] & 1), "READ_INFO after power: address learned");
    CHECK(mgmt(control, 0x0099, 0xFFFF, NULL, 0, r, sizeof(r)) >= 9 && r[8] == 0x01, "unknown mgmt command");
    CHECK(mgmt(control, 0x0004, 7, NULL, 0, r, sizeof(r)) >= 9 && r[8] == 0x11, "invalid index");
    /* HCIDEVUP hci0: the init sequence through the emulator */
    CHECK(!ioctl(raw, HCIDEVUP, 0), "HCIDEVUP hci0");
    errno = 0; CHECK(ioctl(raw, HCIDEVUP, 0) < 0 && errno == EALREADY, "second HCIDEVUP");
    uint8_t info[92];
    CHECK(devinfo(0, info) && !strcmp((char*)info + 2, "hci0") && !memcmp(info + 10, ADDR[0], 6) && (info[16] & 1) && get16(info + 44) == 1021, "HCIGETDEVINFO hci0");
    /* scan: inquiry on hci0 finds hci1 */
    int scan = hci_socket(0, 0); CHECK(scan >= 0, "raw socket on hci0");
    struct hci_filter f = {1U << 4, {0xFFFFFFFFU, 0xFFFFFFFFU}, 0}; CHECK(!setsockopt(scan, 0, 2, &f, 14), "HCI_FILTER");
    uint8_t inquiry[9] = {1, 0x01, 0x04, 5, 0x33, 0x8B, 0x9E, 4, 0}; CHECK(write(scan, inquiry, 9) == 9, "Inquiry");
    bool status = false, found = false, complete_seen = false;
    for (int i = 0; i < 6 && !complete_seen; i++) {
        int n = read_timeout(scan, r, sizeof(r), 2000); if (n < 3) break;
        if (r[1] == 0x0F) status = true;
        if (r[1] == 0x02 && n >= 9 && !memcmp(r + 4, ADDR[1], 6)) found = true;
        if (r[1] == 0x01) complete_seen = true;
    }
    CHECK(status && found && complete_seen, "inquiry: status=%d peer found=%d complete=%d", status, found, complete_seen);
    printf("[64] BT scan: hci0 inquiry found 00:AA:01:00:00:02 (hci1)\n");
    /* ACL across the emulated link */
    int peer = hci_socket(1, 0); struct hci_filter acl = {1U << 2, {0, 0}, 0}; CHECK(peer >= 0 && !setsockopt(peer, 0, 2, &acl, 14), "raw socket on hci1 (ACL)");
    uint8_t data[13] = {2, 0x01, 0x20, 8, 0, 4, 0, 0x40, 0, 'e', 'c', 'h', 'o'}; CHECK(write(scan, data, 13) == 13, "ACL write on hci0");
    int n = read_timeout(peer, r, sizeof(r), 2000); CHECK(n == 13 && !memcmp(r, data, 13), "ACL arrived on hci1 (%d)", n);
    uint8_t shortp[2] = {1, 3}; errno = 0; CHECK(write(scan, shortp, 2) < 0 && errno == EINVAL, "short packet");
    uint8_t wrongcmd[6] = {1, 0x09, 0x10, 5, 0, 0}; errno = 0; CHECK(write(scan, wrongcmd, 6) < 0 && errno == EINVAL, "command length mismatch");
    /* user channel */
    errno = 0; CHECK(hci_socket(0, 1) < 0 && errno == EBUSY, "user channel needs the device down");
    CHECK(!ioctl(raw, HCIDEVDOWN, 0), "HCIDEVDOWN hci0");
    int user = hci_socket(0, 1); CHECK(user >= 0, "user channel on hci0");
    errno = 0; CHECK(hci_socket(0, 1) < 0 && errno == EBUSY, "user channel is exclusive");
    errno = 0; CHECK(ioctl(raw, HCIDEVUP, 0) < 0 && errno == EBUSY, "no HCIDEVUP while user-owned");
    uint8_t bdaddr_cmd[4] = {1, 0x09, 0x10, 0}; CHECK(write(user, bdaddr_cmd, 4) == 4, "user channel command");
    n = read_timeout(user, r, sizeof(r), 2000); CHECK(n >= 13 && r[0] == 4 && r[1] == 0x0E && !memcmp(r + 7, ADDR[0], 6), "user channel gets the reply (%d)", n);
    CHECK(mgmt(control, 0x0003, 0xFFFF, NULL, 0, r, sizeof(r)) >= 11 && get16(r + 9) == 1, "mgmt hides the user-owned controller");
    close(user);
    CHECK(mgmt(control, 0x0003, 0xFFFF, NULL, 0, r, sizeof(r)) >= 13 && get16(r + 9) == 2, "back after the user channel closed");
    /* hostile binds and unprivileged users */
    int s = socket(AF_BT, SOCK_RAW, BTPROTO_HCI);
    struct sockaddr_hci bad = {AF_BT, 0, 9}; errno = 0; CHECK(bind(s, (struct sockaddr*)&bad, sizeof(bad)) < 0 && errno == EINVAL, "unknown channel");
    bad = (struct sockaddr_hci){AF_BT, 9, 0}; errno = 0; CHECK(bind(s, (struct sockaddr*)&bad, sizeof(bad)) < 0 && errno == ENODEV, "unknown device");
    bad = (struct sockaddr_hci){2, 0, 0}; errno = 0; CHECK(bind(s, (struct sockaddr*)&bad, sizeof(bad)) < 0 && errno == EINVAL, "wrong family");
    errno = 0; CHECK(bind(s, (struct sockaddr*)&bad, 2) < 0 && errno == EINVAL, "short address");
    close(s);
    pid_t nobody = fork();
    if (!nobody) {
        if (setgroups(0, NULL) || setresgid(65534, 65534, 65534) || setresuid(65534, 65534, 65534)) _exit(20);
        if (open("/dev/vhci", O_RDWR) >= 0 || errno != EACCES) _exit(21);
        if (hci_socket(0xFFFF, 2) >= 0 || errno != EPERM) _exit(22);
        int fd = hci_socket(0xFFFF, 0); if (fd < 0 || ioctl(fd, HCIDEVUP, 1) == 0 || errno != EPERM) _exit(23);
        int on1 = hci_socket(1, 0); uint8_t reset[4] = {1, 0x03, 0x0C, 0}; if (on1 < 0 || write(on1, reset, 4) >= 0 || errno != EPERM) _exit(24);   /* raw commands need CAP_NET_RAW */
        _exit(0);
    }
    int status_code = -1; waitpid(nobody, &status_code, 0);
    CHECK(WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0, "unprivileged denials: status %x", status_code);
    /* monitor: indexes and the init traffic */
    int news = 0, opens = 0, commands = 0, events = 0, bdaddr_seen = 0;
    for (;;) {
        int m = read_timeout(monitor, r, sizeof(r), 100); if (m < 6) break;
        unsigned op = get16(r);
        if (op == 0) news++; if (op == 8) opens++;
        if (op == 2) { commands++; if (m >= 8 && get16(r + 6) == 0x1009) bdaddr_seen++; }
        if (op == 3) events++;
    }
    CHECK(news == 2 && opens >= 2 && commands >= 10 && events >= 10 && bdaddr_seen >= 2, "monitor: new %d open %d cmd %d evt %d bdaddr %d", news, opens, commands, events, bdaddr_seen);
    /* unplug hci0 */
    kill(child, SIGTERM); waitpid(child, NULL, 0);
    close(vhci[0]);
    errno = 0; CHECK(read(scan, r, sizeof(r)) < 0 && errno == ENODEV, "raw socket after unplug: ENODEV");
    struct pollfd p = {scan, POLLIN, 0}; CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLHUP), "POLLHUP after unplug");
    int m = read_timeout(monitor, r, sizeof(r), 500); CHECK(m >= 6 && get16(r) == 1 && get16(r + 2) == 0, "monitor DEL_INDEX");
    bool removed = false; for (int i = 0; i < 4 && !removed; i++) { m = read_timeout(control, r, sizeof(r), 500); removed = m >= 6 && get16(r) == 5 && get16(r + 2) == 0; }
    CHECK(removed, "mgmt Index Removed");
    list[0] = 4; list[1] = 0; CHECK(!ioctl(raw, HCIGETDEVLIST, list) && get16(list) == 1, "one controller left");
    close(vhci[1]); close(scan); close(peer); close(raw); close(monitor); close(control);
    printf(failures ? "[64] BT TEST FAILED failures=%d\n" : "[64] BT TEST PASSED failures=%d\n", failures);
    return failures ? 1 : 0;
}
