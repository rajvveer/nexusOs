/* Aurora removable media: what udisks2 + the GNOME volume monitor do on Linux, inside the desktop process.
 *
 * The kernel broadcasts block uevents on NETLINK_KOBJECT_UEVENT (group 1) with udev's blkid properties already
 * filled in (ID_FS_TYPE/USAGE/UUID/LABEL, ID_BUS=usb; blkdev_node.c). For each USB filesystem that appears, and for
 * those attached before the desktop started, this mounts it as the desktop user at /media/nexus/<label> with
 * nosuid,nodev (the kernel's user-mount policy, mount.c, creates and later removes the directory) and shows a notice.
 * A drive pulled out while still mounted is lazily unmounted, with a warning. Files (aurora_places.c) lists the
 * mounts below /media and unmounts/ejects them. NTFS and ext4 drives go through their FUSE drivers when installed
 * (ntfs-3g, fuse2fs); otherwise the notice says which program is missing. Disks holding a system mount (the root
 * when booted from USB) are never touched. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <wayland-server.h>
#include "aurora_media.h"

#define MEDIA_UID 1000
static void (*notify)(const char* text);
static int uevents = -1;
static struct wl_event_source* source;

static const char* property(const char* data, size_t size, const char* key) {
    size_t n = strlen(key);
    for (size_t at = 0; at < size; ) {
        const char* item = data + at; size_t length = strnlen(item, size - at);
        if (length > n && !strncmp(item, key, n) && item[n] == '=') return item + n + 1;
        at += length + 1;
    }
    return NULL;
}
/* /proc/self/mounts fields are octal-escaped (\040 for a space). */
static void unescape(char* text) {
    char* out = text;
    for (char* in = text; *in; ) {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '3' && in[2] >= '0' && in[2] <= '7' && in[3] >= '0' && in[3] <= '7') {
            *out++ = (char)((in[1] - '0') << 6 | (in[2] - '0') << 3 | (in[3] - '0')); in += 4;
        } else *out++ = *in++;
    }
    *out = 0;
}
/* Calls visit(source, point, type) for each mount; stops when it returns true. */
static bool each_mount(bool (*visit)(const char*, const char*, const char*, void*), void* context) {
    FILE* f = fopen("/proc/self/mounts", "re"); if (!f) return false;
    char line[2048]; bool stopped = false;
    while (!stopped && fgets(line, sizeof(line), f)) {
        char* save = NULL; char* device = strtok_r(line, " ", &save); char* point = strtok_r(NULL, " ", &save); char* type = strtok_r(NULL, " ", &save);
        if (!device || !point || !type) continue;
        unescape(device); unescape(point);
        stopped = visit(device, point, type, context);
    }
    fclose(f); return stopped;
}
struct find { const char* device; char* point; size_t capacity; };
static bool find_device(const char* device, const char* point, const char* type, void* context) {
    (void)type; struct find* f = context;
    if (strcmp(device, f->device)) return false;
    if (f->point) snprintf(f->point, f->capacity, "%s", point);
    return true;
}
static bool mounted_at(const char* device, char* point, size_t capacity) { struct find f = {device, point, capacity}; return each_mount(find_device, &f); }
static bool point_used(const char* device, const char* point, const char* type, void* context) { (void)device; (void)type; return !strcmp(point, context); }
/* sdb1 -> sdb, nvme0n1p2 -> nvme0n1, loop3p1 -> loop3, mmcblk0p1 -> mmcblk0 (the sysfs link names the parent). */
static void disk_of(const char* name, char* disk, size_t capacity) {
    char link[256], target[512]; snprintf(link, sizeof(link), "/sys/class/block/%s", name);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = 0; char* last = strrchr(target, '/');
        if (last && !strcmp(last + 1, name)) { *last = 0; char* parent = strrchr(target, '/');
            char partition[300]; snprintf(partition, sizeof(partition), "/sys/class/block/%s/partition", name);
            if (parent && !access(partition, F_OK)) { snprintf(disk, capacity, "%s", parent + 1); return; } }
    }
    snprintf(disk, capacity, "%s", name);
}
struct system { const char* disk; bool found; };
static bool system_mount(const char* device, const char* point, const char* type, void* context) {
    (void)type; struct system* s = context;
    if (strncmp(device, "/dev/", 5) || !strncmp(point, "/media/", 7) || !strncmp(point, "/run/media/", 11)) return false;
    char disk[64]; disk_of(device + 5, disk, sizeof(disk));
    if (!strcmp(disk, s->disk)) { s->found = true; return true; }
    return false;
}
static bool system_disk(const char* name) {
    char disk[64]; disk_of(name, disk, sizeof(disk));
    struct system s = {disk, false}; each_mount(system_mount, &s); return s.found;
}
/* A directory name from the label (udisks: '/' becomes '_'), else the UUID. */
static void volume_name(const char* label, const char* uuid, char* out, size_t capacity) {
    const char* from = label && label[0] ? label : uuid && uuid[0] ? uuid : "disk";
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)from; *p && n + 1 < capacity && n < 64; p++)
        out[n++] = *p == '/' ? '_' : *p < 32 || *p == 127 ? '_' : (char)*p;
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    if (!n || !strcmp(out, ".") || !strcmp(out, "..")) snprintf(out, capacity, "disk");
}
static void say(const char* format, const char* name, const char* detail) {
    char text[256]; snprintf(text, sizeof(text), format, name, detail ? detail : "");
    if (notify) notify(text);
}
/* A FUSE driver run as the desktop user: helper SOURCE POINT -o OPTIONS (it mounts through /dev/fuse itself). */
static int fuse_helper(const char* const* candidates, const char* device, const char* point, const char* options) {
    const char* helper = NULL;
    for (unsigned i = 0; candidates[i] && !helper; i++) if (!access(candidates[i], X_OK)) helper = candidates[i];
    if (!helper) return -ENOENT;
    if (mkdir(point, 0700) && errno != EEXIST) return -errno;
    pid_t child = fork();
    if (child < 0) return -errno;
    if (!child) { execl(helper, helper, device, point, "-o", options, (char*)NULL); _exit(127); }
    int status = 0; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { rmdir(point); return -EIO; }
    return 0;
}
static void automount(const char* name, const char* type, const char* label, const char* uuid) {
    char device[64], point[256], title[80]; snprintf(device, sizeof(device), "/dev/%s", name);
    volume_name(label, uuid, title, sizeof(title));
    if (mounted_at(device, NULL, 0)) return;
    if (system_disk(name)) { printf("[64] AURORA MEDIA SKIP %s (system disk)\n", device); fflush(stdout); return; }
    struct stat st;
    if (stat("/media/nexus", &st) && errno == ENOENT) { mkdir("/media", 0755); }
    for (unsigned i = 0; i < 10; i++) {
        if (i) snprintf(point, sizeof(point), "/media/nexus/%s%u", title, i); else snprintf(point, sizeof(point), "/media/nexus/%s", title);
        if (!each_mount(point_used, point)) break;
    }
    unsigned long flags = MS_NOSUID | MS_NODEV; const char* fs = type; char options[160]; int error = 0;
    if (!strcmp(type, "vfat")) snprintf(options, sizeof(options), "uid=%d,gid=%d,shortname=mixed,utf8,showexec,flush,errors=remount-ro", MEDIA_UID, MEDIA_UID);
    else if (!strcmp(type, "exfat")) snprintf(options, sizeof(options), "uid=%d,gid=%d,iocharset=utf8,errors=remount-ro", MEDIA_UID, MEDIA_UID);
    else if (!strcmp(type, "iso9660") || !strcmp(type, "udf")) { flags |= MS_RDONLY; snprintf(options, sizeof(options), "uid=%d,gid=%d,iocharset=utf8", MEDIA_UID, MEDIA_UID); }
    else if (!strcmp(type, "ntfs") || !strcmp(type, "ext4") || !strcmp(type, "ext3") || !strcmp(type, "ext2")) {
        static const char* const ntfs[] = {"/usr/bin/ntfs-3g", "/bin/ntfs-3g", NULL};
        static const char* const ext[] = {"/usr/bin/fuse2fs", "/usr/sbin/fuse2fs", "/sbin/fuse2fs", NULL};
        bool windows = !strcmp(type, "ntfs");
        if (windows) snprintf(options, sizeof(options), "uid=%d,gid=%d,windows_names,nosuid,nodev", MEDIA_UID, MEDIA_UID);
        else snprintf(options, sizeof(options), "fakeroot,nosuid,nodev");
        error = fuse_helper(windows ? ntfs : ext, device, point, options);
        if (error == -ENOENT) {
            printf("[64] AURORA MEDIA UNSUPPORTED %s type=%s (no FUSE driver)\n", device, type); fflush(stdout);
            say("\"%s\" uses %s: install it to open this drive.", title, windows ? "NTFS (ntfs-3g)" : "ext4 (fuse2fs)"); return;
        }
        fs = windows ? "fuseblk" : "fuse";
        goto done;
    } else {
        printf("[64] AURORA MEDIA UNSUPPORTED %s type=%s\n", device, type); fflush(stdout);
        say("\"%s\" has a filesystem NexusOS cannot open (%s).", title, type); return;
    }
    if (mount(device, point, fs, flags, options)) error = -errno;
done:
    if (error) {
        printf("[64] AURORA MEDIA MOUNT FAILED %s type=%s errno=%d\n", device, type, -error); fflush(stdout);
        say("\"%s\" could not be opened: %s.", title, strerror(-error)); return;
    }
    printf("[64] AURORA MEDIA MOUNTED source=%s point=%s type=%s\n", device, point, fs); fflush(stdout);
    say("\"%s\" is ready. Open it from the sidebar in Files.%s", title, "");
}
static void removed(const char* name) {
    char device[64], point[256]; snprintf(device, sizeof(device), "/dev/%s", name);
    if (!mounted_at(device, point, sizeof(point))) return;
    const char* leaf = strrchr(point, '/'); leaf = leaf ? leaf + 1 : point;
    int result = umount2(point, MNT_DETACH);
    printf("[64] AURORA MEDIA UNSAFE REMOVAL source=%s point=%s detach=%d\n", device, point, result ? -errno : 0); fflush(stdout);
    say("\"%s\" was removed without ejecting it first. Recent changes may be lost.%s", leaf, "");
}
static void handle(const char* data, size_t size) {
    const char* action = property(data, size, "ACTION"), *subsystem = property(data, size, "SUBSYSTEM"), *name = property(data, size, "DEVNAME");
    if (!action || !subsystem || !name || strcmp(subsystem, "block") || strchr(name, '/') || strlen(name) > 40) return;
    if (!strcmp(action, "remove")) { removed(name); return; }
    if (strcmp(action, "add")) return;
    const char* bus = property(data, size, "ID_BUS"), *usage = property(data, size, "ID_FS_USAGE"), *type = property(data, size, "ID_FS_TYPE");
    if (!bus || strcmp(bus, "usb") || !usage || strcmp(usage, "filesystem") || !type) return;
    automount(name, type, property(data, size, "ID_FS_LABEL"), property(data, size, "ID_FS_UUID"));
}
static int readable(int fd, uint32_t mask, void* data) {
    (void)mask; (void)data;
    for (int i = 0; i < 32; i++) {
        char buffer[8192]; struct sockaddr_nl from; socklen_t length = sizeof(from);
        ssize_t n = recvfrom(fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT, (struct sockaddr*)&from, &length);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (length != sizeof(from) || from.nl_pid != 0) continue;   /* only the kernel's broadcasts */
        buffer[n] = 0;
        const char* at = memchr(buffer, '@', (size_t)n);
        if (!at || !memchr(buffer, 0, (size_t)n)) continue;
        size_t header = strlen(buffer) + 1;
        if (header < (size_t)n) handle(buffer + header, (size_t)n - header);
    }
    return 0;
}
/* Drives attached before the desktop started: the same identity the kernel puts into its uevents is in
 * /dev/disk/by-uuid, by-label; the type comes from a superblock look (blkid's most common cases). */
static const char* probe_type(int fd) {
    static unsigned char block[4096];
    if (pread(fd, block, sizeof(block), 0) != (ssize_t)sizeof(block)) return NULL;
    if (!memcmp(block + 3, "EXFAT   ", 8)) return "exfat";
    if (!memcmp(block + 3, "NTFS    ", 8)) return "ntfs";
    if (block[510] == 0x55 && block[511] == 0xAA && (!memcmp(block + 54, "FAT", 3) || !memcmp(block + 82, "FAT32", 5))) return "vfat";
    if (block[1024 + 56] == 0x53 && block[1024 + 57] == 0xEF) return "ext4";
    static unsigned char volume[2048];
    if (pread(fd, volume, sizeof(volume), 32768) == (ssize_t)sizeof(volume) && volume[0] == 1 && !memcmp(volume + 1, "CD001", 5)) return "iso9660";
    return NULL;
}
static void link_value(const char* directory, const char* name, char* out, size_t capacity) {
    out[0] = 0; DIR* d = opendir(directory); if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char link[512], target[256]; snprintf(link, sizeof(link), "%s/%s", directory, e->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1); if (n <= 0) continue; target[n] = 0;
        const char* leaf = strrchr(target, '/'); leaf = leaf ? leaf + 1 : target;
        if (!strcmp(leaf, name)) {   /* by-label names escape '/' and spaces as \x2f, \x20 (udev) */
            size_t k = 0; for (const char* p = e->d_name; *p && k + 1 < capacity; p++) {
                if (p[0] == '\\' && p[1] == 'x' && p[2] && p[3]) { char hex[3] = {p[2], p[3], 0}; out[k++] = (char)strtol(hex, NULL, 16); p += 3; }
                else out[k++] = *p;
            }
            out[k] = 0; break;
        }
    }
    closedir(d);
}
static void coldplug(void) {
    DIR* d = opendir("/sys/class/block"); if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        const char* name = e->d_name;
        if (name[0] == '.' || strlen(name) > 40 || strncmp(name, "sd", 2)) continue;   /* USB disks are SCSI disks */
        char disk[64], removable[256], text[8] = ""; disk_of(name, disk, sizeof(disk));
        snprintf(removable, sizeof(removable), "/sys/class/block/%s/device/type", disk);
        char device[64]; snprintf(device, sizeof(device), "/dev/%s", name);
        int fd = open(device, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;   /* not the desktop user's drive (hot-plugged USB nodes are) */
        const char* type = probe_type(fd); close(fd);
        (void)text;
        if (!type) continue;
        char label[80], uuid[80];
        link_value("/dev/disk/by-label", name, label, sizeof(label)); link_value("/dev/disk/by-uuid", name, uuid, sizeof(uuid));
        /* whole-disk filesystem or a partition; a partitioned disk itself has no superblock and was skipped */
        automount(name, type, label, uuid);
    }
    closedir(d);
}
bool aurora_media_init(struct wl_event_loop* loop, void (*notice)(const char* text)) {
    notify = notice;
    uevents = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (uevents < 0) { perror("aurora media: uevent socket"); return false; }
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (bind(uevents, (struct sockaddr*)&address, sizeof(address)) || !(source = wl_event_loop_add_fd(loop, uevents, WL_EVENT_READABLE, readable, NULL))) {
        perror("aurora media: uevent listener"); close(uevents); uevents = -1; return false;
    }
    printf("[64] AURORA MEDIA listening\n"); fflush(stdout);
    coldplug();
    return true;
}
