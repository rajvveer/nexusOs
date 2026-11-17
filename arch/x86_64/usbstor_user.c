/* USB mass storage campaign (tests/test_x64_usbstorage.py): runs as /sbin/init on an ext4 root, the stick is sda.
 *   storage64 [phase]       unprivileged phases, then execs /bin/storage64-root (set-user-ID root) for "root"
 * Phases: basic (nodes, stat, ioctls, sysfs, /proc/partitions, pattern reads, unaligned/O_DIRECT writes, errors,
 * O_EXCL), root (BLKRRPART busy/ok, BLKPG delete/add/resize + uevents), hotplug (uevent add of sdb, I/O, surprise
 * removal while reading: EIO, remove uevent, node gone). The host coordinates hot-plug through QMP on markers. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { printf("[64] USBSTOR FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); fflush(stdout); _exit(99); } } while (0)
#define P2_START 34816ULL
#define P2_BYTES (64ULL * 1024 * 1024 - P2_START * 512)
#define WRITE_AT (1048576ULL + 333)
#define WRITE_LEN 200000U
static uint8_t pattern(uint64_t o) { return (uint8_t)(((o >> 9) * 7 + (o & 511) * 13 + 1) & 255); }
static uint8_t pattern2(uint64_t o) { return (uint8_t)((o * 29 + 101) & 255); }
static void say(const char* text) { printf("[64] USBSTOR %s\n", text); fflush(stdout); }
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static long slurp(const char* path, char* out, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    long total = 0; for (;;) { ssize_t n = read(fd, out + total, capacity - 1 - total); if (n <= 0) break; total += n; }
    close(fd); out[total] = 0; return total;
}
static uint64_t sys_number(const char* path) { char text[64]; CHECK(slurp(path, text, sizeof(text)) > 0); return strtoull(text, NULL, 10); }
static void full_pread(int fd, void* buffer, size_t length, off_t offset) {
    size_t done = 0; while (done < length) { ssize_t n = pread(fd, (uint8_t*)buffer + done, length - done, offset + done); CHECK(n > 0); done += n; }
}
static int uevent_socket(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT); CHECK(fd >= 0);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1}; CHECK(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
    return fd;
}
/* Wait for an uevent "<action>@..." whose DEVNAME is `name` (and SUBSYSTEM=block); 1 found, 0 timeout. */
static int wait_uevent(int fd, const char* action, const char* name, unsigned seconds) {
    uint64_t end = now_ms() + seconds * 1000ULL;
    while (now_ms() < end) {
        struct pollfd p = {fd, POLLIN, 0}; int ready = poll(&p, 1, 500); if (ready <= 0) continue;
        char buffer[4096]; struct sockaddr_nl from; struct iovec v = {buffer, sizeof(buffer) - 1};
        char control[64]; struct msghdr m = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        ssize_t n = recvmsg(fd, &m, 0); CHECK(n > 0); buffer[n] = 0;
        CHECK(from.nl_groups == 1 && from.nl_pid == 0);
        struct cmsghdr* c = CMSG_FIRSTHDR(&m); CHECK(c && c->cmsg_type == SCM_CREDENTIALS && ((struct ucred*)CMSG_DATA(c))->uid == 0);
        size_t alen = strlen(action);
        if (strncmp(buffer, action, alen) || buffer[alen] != '@') continue;
        bool block = false, match = false, has_seq = false, has_action = false;
        for (char* field = buffer + strlen(buffer) + 1; field < buffer + n; field += strlen(field) + 1) {
            if (!strcmp(field, "SUBSYSTEM=block")) block = true;
            if (!strncmp(field, "DEVNAME=", 8) && !strcmp(field + 8, name)) match = true;
            if (!strncmp(field, "SEQNUM=", 7)) has_seq = true;
            if (!strncmp(field, "ACTION=", 7) && !strcmp(field + 7, action)) has_action = true;
        }
        if (block && match) { CHECK(has_seq && has_action); return 1; }
    }
    return 0;
}

static void phase_basic(void) {
    char text[8192];
    CHECK(slurp("/proc/partitions", text, sizeof(text)) > 0);
    CHECK(strstr(text, " sda\n") && strstr(text, " sda1\n") && strstr(text, " sda2\n"));
    struct stat st; CHECK(stat("/dev/sda", &st) == 0 && S_ISBLK(st.st_mode) && major(st.st_rdev) == 8 && minor(st.st_rdev) == 0);
    CHECK(stat("/dev/sda2", &st) == 0 && S_ISBLK(st.st_mode) && major(st.st_rdev) == 8 && minor(st.st_rdev) == 2);
    CHECK(st.st_uid == 1000 && st.st_gid == 6 && (st.st_mode & 0777) == 0660);
    DIR* dev = opendir("/dev"); CHECK(dev); int seen = 0; struct dirent* e;
    while ((e = readdir(dev))) { if (!strcmp(e->d_name, "sda") || !strcmp(e->d_name, "sda2")) { CHECK(e->d_type == DT_BLK); seen++; } }
    closedir(dev); CHECK(seen == 2);
    int fd = open("/dev/sda", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    uint64_t bytes = 0; CHECK(ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes == 64ULL * 1024 * 1024);
    unsigned long sectors = 0; CHECK(ioctl(fd, BLKGETSIZE, &sectors) == 0 && sectors == bytes / 512);
    int logical = 0; unsigned physical = 0; CHECK(ioctl(fd, BLKSSZGET, &logical) == 0 && (logical == 512 || logical == 4096));
    CHECK(ioctl(fd, BLKPBSZGET, &physical) == 0 && physical >= (unsigned)logical);
    int ro = -1; CHECK(ioctl(fd, BLKROGET, &ro) == 0 && ro == 0);
    unsigned short rotational = 9; CHECK(ioctl(fd, BLKROTATIONAL, &rotational) == 0 && rotational == 0);
    struct hd_geometry g; CHECK(ioctl(fd, HDIO_GETGEO, &g) == 0 && g.heads == 255 && g.sectors == 63 && g.start == 0);
    CHECK(ioctl(fd, BLKRRPART) == -1 && errno == EACCES);              /* needs CAP_SYS_ADMIN */
    CHECK(ioctl(fd, 0x12345678) == -1 && errno == ENOTTY);
    CHECK(lseek(fd, 0, SEEK_END) == (off_t)bytes);
    uint8_t mbr[512]; CHECK(pread(fd, mbr, 512, 0) == 512 && mbr[510] == 0x55 && mbr[511] == 0xAA);
    close(fd);
    snprintf(text, sizeof(text), "logical=%d physical=%u", logical, physical); say(text);
    /* sysfs */
    CHECK(sys_number("/sys/block/sda/size") == bytes / 512);
    CHECK(sys_number("/sys/block/sda/queue/logical_block_size") == (uint64_t)logical);
    CHECK(sys_number("/sys/class/block/sda2/start") == P2_START);
    CHECK(sys_number("/sys/class/block/sda2/partition") == 2);
    CHECK(slurp("/sys/block/sda/dev", text, sizeof(text)) > 0 && !strcmp(text, "8:0\n"));
    CHECK(slurp("/sys/block/sda/sda2/uevent", text, sizeof(text)) > 0 && strstr(text, "DEVNAME=sda2") && strstr(text, "DEVTYPE=partition"));
    CHECK(slurp("/sys/block/sda/device/vendor", text, sizeof(text)) > 0 && !strncmp(text, "QEMU", 4));
    char link[256]; ssize_t n = readlink("/sys/dev/block/8:2", link, sizeof(link) - 1); CHECK(n > 0); link[n] = 0;
    CHECK(!strcmp(link, "/sys/devices/virtual/block/sda/sda2"));
    n = readlink("/sys/block/sda", link, sizeof(link) - 1); CHECK(n > 0); link[n] = 0; CHECK(!strcmp(link, "/sys/devices/virtual/block/sda"));
    DIR* sys = opendir("/sys/block/sda"); CHECK(sys); int found = 0;
    while ((e = readdir(sys))) if (!strcmp(e->d_name, "sda1") || !strcmp(e->d_name, "queue") || !strcmp(e->d_name, "size")) found++;
    closedir(sys); CHECK(found == 3);
    /* pattern reads: aligned, unaligned, large, across the end */
    fd = open("/dev/sda2", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    CHECK(ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes == P2_BYTES);
    CHECK(ioctl(fd, HDIO_GETGEO, &g) == 0 && g.start == P2_START);
    static uint8_t buffer[1 << 20];
    full_pread(fd, buffer, sizeof(buffer), 3 << 20);
    for (unsigned i = 0; i < sizeof(buffer); i++) CHECK(buffer[i] == pattern((3 << 20) + i));
    full_pread(fd, buffer, 70001, 12345);
    for (unsigned i = 0; i < 70001; i++) CHECK(buffer[i] == pattern(12345 + i));
    CHECK(pread(fd, buffer, 4096, bytes - 1000) == 1000);
    for (unsigned i = 0; i < 1000; i++) CHECK(buffer[i] == pattern(bytes - 1000 + i));
    CHECK(pread(fd, buffer, 4096, bytes) == 0);
    CHECK(lseek(fd, 777, SEEK_SET) == 777 && read(fd, buffer, 100) == 100 && buffer[0] == pattern(777) && lseek(fd, 0, SEEK_CUR) == 877);
    uint64_t range[2] = {0, 4096}; CHECK(ioctl(fd, BLKDISCARD, range) == -1 && errno == EBADF);
    close(fd);
    /* writes: unaligned pwrite + fsync, read back; O_DIRECT alignment rules; ENOSPC at the end */
    fd = open("/dev/sda2", O_RDWR | O_CLOEXEC); CHECK(fd >= 0);
    for (unsigned i = 0; i < WRITE_LEN; i++) buffer[i] = pattern2(WRITE_AT + i);
    CHECK(pwrite(fd, buffer, WRITE_LEN, WRITE_AT) == WRITE_LEN);
    CHECK(fsync(fd) == 0);
    memset(buffer, 0, WRITE_LEN + 2);
    full_pread(fd, buffer, WRITE_LEN + 2, WRITE_AT - 1);
    CHECK(buffer[0] == pattern(WRITE_AT - 1) && buffer[WRITE_LEN + 1] == pattern(WRITE_AT + WRITE_LEN));
    for (unsigned i = 0; i < WRITE_LEN; i++) CHECK(buffer[i + 1] == pattern2(WRITE_AT + i));
    CHECK(pwrite(fd, buffer, 10, bytes) == -1 && errno == ENOSPC);
    CHECK(pwrite(fd, buffer, 100, bytes - 50) == 50);   /* truncated at the end: restore the pattern there */
    for (unsigned i = 0; i < 50; i++) buffer[i] = pattern(bytes - 50 + i);
    CHECK(pwrite(fd, buffer, 50, bytes - 50) == 50);
    close(fd);
    fd = open("/dev/sda2", O_RDONLY | O_DIRECT | O_CLOEXEC); CHECK(fd >= 0);
    uint8_t* aligned = aligned_alloc(4096, 65536); CHECK(aligned);
    CHECK(pread(fd, aligned, 65536, 8 << 20) == 65536 && aligned[1] == pattern((8 << 20) + 1));
    CHECK(pread(fd, aligned + 1, 4096, 0) == -1 && errno == EINVAL);
    CHECK(pread(fd, aligned, 4096, 100) == -1 && errno == EINVAL);
    CHECK(pread(fd, aligned, 1000, 0) == -1 && errno == EINVAL);
    close(fd); free(aligned);
    /* exclusive opens */
    int a = open("/dev/sda1", O_RDONLY | O_EXCL | O_CLOEXEC); CHECK(a >= 0);
    CHECK(open("/dev/sda1", O_RDONLY | O_EXCL | O_CLOEXEC) == -1 && errno == EBUSY);
    int shared = open("/dev/sda1", O_RDONLY | O_CLOEXEC); CHECK(shared >= 0); close(shared);
    close(a);
    a = open("/dev/sda1", O_RDONLY | O_EXCL | O_CLOEXEC); CHECK(a >= 0); close(a);
    CHECK(open("/dev/sda9", O_RDONLY) == -1 && errno == ENOENT);
    say("BASIC PASSED: nodes, stat, block ioctls, sysfs, /proc/partitions, pattern I/O, O_DIRECT, ENOSPC, O_EXCL");
}
static void phase_root(void) {
    CHECK(geteuid() == 0);
    int events = uevent_socket();
    int part = open("/dev/sda2", O_RDONLY | O_CLOEXEC); CHECK(part >= 0);
    int disk = open("/dev/sda", O_RDONLY | O_CLOEXEC); CHECK(disk >= 0);
    CHECK(ioctl(disk, BLKRRPART) == -1 && errno == EBUSY);                  /* a partition is open */
    CHECK(ioctl(part, BLKRRPART) == -1 && errno == EINVAL);                 /* not on a partition */
    close(part);
    CHECK(ioctl(disk, BLKRRPART) == 0);
    CHECK(wait_uevent(events, "remove", "sda2", 10) && wait_uevent(events, "add", "sda2", 10) && wait_uevent(events, "change", "sda", 10));
    struct stat st; CHECK(stat("/dev/sda2", &st) == 0 && S_ISBLK(st.st_mode));
    /* BLKPG: delete partition 2, re-add it smaller, resize it back */
    struct blkpg_partition bp = {.pno = 2}; struct blkpg_ioctl_arg arg = {.op = BLKPG_DEL_PARTITION, .datalen = sizeof(bp), .data = &bp};
    CHECK(ioctl(disk, BLKPG, &arg) == 0 && wait_uevent(events, "remove", "sda2", 10));
    CHECK(stat("/dev/sda2", &st) == -1 && errno == ENOENT);
    CHECK(ioctl(disk, BLKPG, &arg) == -1 && errno == ENXIO);
    bp.start = P2_START * 512; bp.length = 8ULL << 20; arg.op = BLKPG_ADD_PARTITION;
    CHECK(ioctl(disk, BLKPG, &arg) == 0 && wait_uevent(events, "add", "sda2", 10));
    CHECK(ioctl(disk, BLKPG, &arg) == -1 && errno == EBUSY);                /* exists */
    struct blkpg_partition overlap = {.start = 2048 * 512, .length = 1 << 20, .pno = 3}; struct blkpg_ioctl_arg o = {.op = BLKPG_ADD_PARTITION, .datalen = sizeof(overlap), .data = &overlap};
    CHECK(ioctl(disk, BLKPG, &o) == -1 && errno == EBUSY);                  /* overlaps sda1 */
    char text[64]; CHECK(slurp("/sys/class/block/sda2/size", text, sizeof(text)) > 0 && strtoull(text, NULL, 10) == (8ULL << 20) / 512);
    bp.length = P2_BYTES; arg.op = BLKPG_RESIZE_PARTITION;
    CHECK(ioctl(disk, BLKPG, &arg) == 0);
    part = open("/dev/sda2", O_RDONLY | O_CLOEXEC); CHECK(part >= 0);
    uint64_t bytes = 0; CHECK(ioctl(part, BLKGETSIZE64, &bytes) == 0 && bytes == P2_BYTES);
    uint8_t b; CHECK(pread(part, &b, 1, 4096) == 1 && b == pattern(4096));
    close(part);
    int ro = 1; CHECK(ioctl(disk, BLKROSET, &ro) == 0);
    int w = open("/dev/sda2", O_WRONLY | O_CLOEXEC); CHECK(w >= 0);
    CHECK(write(w, "x", 1) == -1 && errno == EPERM);
    close(w); ro = 0; CHECK(ioctl(disk, BLKROSET, &ro) == 0);
    CHECK(ioctl(disk, BLKFLSBUF) == 0);
    close(disk); close(events);
    say("ROOT PASSED: BLKRRPART busy/ok + uevents, BLKPG delete/add/overlap/resize, BLKROSET/EPERM, BLKFLSBUF");
}
static void phase_hotplug(void) {
    int events = uevent_socket();
    say("WAITING ADD");
    CHECK(wait_uevent(events, "add", "sdb", 90));
    CHECK(wait_uevent(events, "add", "sdb2", 10));
    struct stat st; CHECK(stat("/dev/sdb2", &st) == 0 && S_ISBLK(st.st_mode) && minor(st.st_rdev) == 18);
    int fd = open("/dev/sdb2", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    static uint8_t buffer[256 * 1024];
    full_pread(fd, buffer, sizeof(buffer), 0);
    for (unsigned i = 0; i < sizeof(buffer); i++) CHECK(buffer[i] == pattern(i));
    say("HOTPLUG ADD PASSED");
    say("WAITING REMOVE");
    /* Keep reading until the device disappears underneath: every read either succeeds with the pattern or fails EIO. */
    uint64_t end = now_ms() + 90000; int failed = 0; uint64_t reads = 0;
    while (now_ms() < end) {
        off_t at = (off_t)((reads * 131072) % (P2_BYTES - sizeof(buffer)));
        ssize_t n = pread(fd, buffer, sizeof(buffer), at);
        if (n < 0) { CHECK(errno == EIO); failed = 1; break; }
        CHECK(n > 0 && n % 512 == 0);   /* a read cut short by the removal returns what arrived */
        for (unsigned i = 0; i < 4096 && i < (unsigned)n; i++) CHECK(buffer[i] == pattern(at + i));
        reads++;
    }
    CHECK(failed);
    CHECK(wait_uevent(events, "remove", "sdb", 30));
    CHECK(pread(fd, buffer, 512, 0) == -1 && errno == EIO);
    uint64_t bytes; CHECK(ioctl(fd, BLKGETSIZE64, &bytes) == -1 && errno == ENODEV);
    CHECK(close(fd) == 0);
    CHECK(stat("/dev/sdb", &st) == -1 && errno == ENOENT && stat("/dev/sdb2", &st) == -1);
    char text[4096]; CHECK(slurp("/proc/partitions", text, sizeof(text)) > 0 && !strstr(text, "sdb"));
    CHECK(access("/sys/block/sdb", F_OK) == -1);
    printf("[64] USBSTOR HOTPLUG REMOVE PASSED reads=%llu\n", (unsigned long long)reads); fflush(stdout);
    close(events);
}
int main(int argc, char** argv) {
    char mode[32] = "all";
    if (argc > 1) snprintf(mode, sizeof(mode), "%s", argv[1]);
    else if (slurp("/nexus/usbstor-mode", mode, sizeof(mode)) <= 0) strcpy(mode, "all");
    const char* phase = mode;
    if (!strcmp(phase, "root")) { phase_root(); return 0; }
    phase_basic();
    if (access("/bin/storage64-root", X_OK) == 0) {
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { execl("/bin/storage64-root", "storage64-root", "root", (char*)NULL); _exit(98); }
        int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    if (strcmp(phase, "nohotplug")) phase_hotplug();
    say("ALL PASSED");
    return 0;
}
