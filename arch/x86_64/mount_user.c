/* mount(2)/vfat campaign (tests/test_x64_mount.py). Root: ext4 with /bin/mount64-root set-user-ID root, or FAT
 * (desktop policy only). Disks: sda = stick (p1 FAT16 made by the host with HELLO.TXT, SUB/DATA.BIN; p2 pattern),
 * sdb = fuzz stick (p1 scratch, p2 the pristine FAT template).
 *   mount64 [root|user|fuzz|persist|removal]   (no argument: /nexus/mount-mode, default "root" via the setuid copy)
 * Phases print "[64] MOUNT <phase> PASSED"; any failure "[64] MOUNTTEST FAIL line=.. errno=..". */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { printf("[64] MOUNTTEST FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); fflush(stdout); _exit(99); } } while (0)
static void say(const char* text) { printf("[64] MOUNT %s\n", text); fflush(stdout); }
static long slurp(const char* path, char* out, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    long total = 0; for (;;) { ssize_t n = read(fd, out + total, capacity - 1 - total); if (n <= 0) break; total += n; }
    close(fd); out[total] = 0; return total;
}
static void put_file(const char* path, const void* data, size_t length) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    size_t done = 0; while (done < length) { ssize_t n = write(fd, (const char*)data + done, length - done); CHECK(n > 0); done += n; }
    CHECK(fsync(fd) == 0 && close(fd) == 0);
}
static uint8_t pat(uint64_t i, unsigned seed) { return (uint8_t)((i * 131 + seed * 17 + (i >> 11)) & 255); }
static void check_pattern(const char* path, size_t length, unsigned seed) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    struct stat st; CHECK(fstat(fd, &st) == 0 && (size_t)st.st_size == length);
    static uint8_t buffer[65536]; size_t at = 0;
    while (at < length) { ssize_t n = read(fd, buffer, sizeof(buffer)); CHECK(n > 0); for (ssize_t i = 0; i < n; i++) CHECK(buffer[i] == pat(at + i, seed)); at += n; }
    CHECK(read(fd, buffer, 1) == 0 && close(fd) == 0);
}
static void write_pattern(const char* path, size_t length, unsigned seed) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    static uint8_t buffer[65536]; size_t at = 0;
    while (at < length) { size_t n = length - at < sizeof(buffer) ? length - at : sizeof(buffer); for (size_t i = 0; i < n; i++) buffer[i] = pat(at + i, seed); CHECK(write(fd, buffer, n) == (ssize_t)n); at += n; }
    CHECK(fsync(fd) == 0 && close(fd) == 0);
}
static bool listed(const char* dir, const char* name, unsigned char type) {
    DIR* d = opendir(dir); CHECK(d); struct dirent* e; bool found = false;
    while ((e = readdir(d))) if (!strcmp(e->d_name, name)) { found = !type || e->d_type == type; }
    closedir(d); return found;
}
static long long free_blocks(const char* path) { struct statfs s; CHECK(statfs(path, &s) == 0); return (long long)s.f_bfree; }

static void phase_root(void) {
    CHECK(geteuid() == 0);
    mkdir("/mnt", 0755); CHECK(mkdir("/mnt/usb", 0755) == 0 || errno == EEXIST);
    CHECK(mount("/dev/sda1", "/mnt/usb", "vfat", MS_NOSUID | MS_NODEV, "uid=1000,gid=1000,umask=022,shortname=mixed,utf8") == 0);
    char text[16384];
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0 && strstr(text, "/dev/sda1 /mnt/usb vfat rw,nosuid,nodev,relatime,uid=1000"));
    CHECK(slurp("/proc/self/mountinfo", text, sizeof(text)) > 0 && strstr(text, " 8:1 / /mnt/usb rw,nosuid,nodev,relatime - vfat /dev/sda1 rw,uid=1000"));
    struct statfs fs; CHECK(statfs("/mnt/usb", &fs) == 0 && fs.f_type == 0x4d44 && fs.f_blocks > 1000 && fs.f_bfree > 0 && (fs.f_flags & ST_NOSUID) && (fs.f_flags & ST_NODEV));
    struct stat st; CHECK(stat("/mnt/usb", &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev == makedev(8, 1) && st.st_uid == 1000 && (st.st_mode & 0777) == 0755);
    struct stat root; CHECK(stat("/", &root) == 0 && root.st_dev != st.st_dev);
    /* files from the host */
    CHECK(slurp("/mnt/usb/HELLO.TXT", text, sizeof(text)) == 13 && !strcmp(text, "hello, stick\n"));
    CHECK(slurp("/mnt/usb/hello.txt", text, sizeof(text)) == 13);                      /* case-insensitive */
    check_pattern("/mnt/usb/SUB/DATA.BIN", 300000, 1);
    CHECK(stat("/mnt/usb/SUB/DATA.BIN", &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 1000 && st.st_nlink == 1 && (st.st_mode & 0777) == 0755);
    /* long names, nested directories, rename, unlink */
    const char* long_name = "/mnt/usb/Long File Name with spaces and \xc3\xa9t\xc3\xa9.txt";
    write_pattern(long_name, 100000, 2); check_pattern(long_name, 100000, 2);
    CHECK(listed("/mnt/usb", "Long File Name with spaces and \xc3\xa9t\xc3\xa9.txt", DT_REG));
    CHECK(mkdir("/mnt/usb/New Folder", 0755) == 0 && mkdir("/mnt/usb/New Folder", 0755) == -1 && errno == EEXIST);
    CHECK(listed("/mnt/usb", "New Folder", DT_DIR));
    write_pattern("/mnt/usb/New Folder/inner.bin", 5000, 3);
    CHECK(rename("/mnt/usb/New Folder/inner.bin", "/mnt/usb/SUB/moved.bin") == 0);
    CHECK(access("/mnt/usb/New Folder/inner.bin", F_OK) == -1 && errno == ENOENT); check_pattern("/mnt/usb/SUB/moved.bin", 5000, 3);
    write_pattern("/mnt/usb/SUB/victim.bin", 7000, 4);
    CHECK(rename("/mnt/usb/SUB/moved.bin", "/mnt/usb/SUB/victim.bin") == 0); check_pattern("/mnt/usb/SUB/victim.bin", 5000, 3);
    CHECK(rename("/mnt/usb/SUB/victim.bin", "/mnt/usb/SUB/Victim.BIN") == 0 && listed("/mnt/usb/SUB", "Victim.BIN", DT_REG));   /* case-only rename */
    CHECK(rename("/mnt/usb/New Folder", "/mnt/usb/SUB/Folder2") == 0 && listed("/mnt/usb/SUB", "Folder2", DT_DIR));          /* directory move */
    CHECK(rename("/mnt/usb/SUB", "/mnt/usb/SUB/Folder2/x") == -1 && errno == EINVAL);                                      /* into itself */
    CHECK(stat("/mnt/usb/SUB/Folder2/..", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(rmdir("/mnt/usb/SUB") == -1 && errno == ENOTEMPTY);
    CHECK(rmdir("/mnt/usb/SUB/Folder2") == 0 && unlink("/mnt/usb/SUB/Victim.BIN") == 0);
    CHECK(unlink("/mnt/usb/SUB") == -1 && errno == EISDIR);
    /* vfat semantics */
    CHECK(chmod("/mnt/usb/HELLO.TXT", 0555) == 0 && stat("/mnt/usb/HELLO.TXT", &st) == 0 && (st.st_mode & 0777) == 0555);
    CHECK(chmod("/mnt/usb/HELLO.TXT", 0755) == 0 && stat("/mnt/usb/HELLO.TXT", &st) == 0 && (st.st_mode & 0777) == 0755);
    CHECK(chmod("/mnt/usb/HELLO.TXT", 0700) == -1 && errno == EPERM);
    CHECK(chown("/mnt/usb/HELLO.TXT", 1234, 1234) == -1 && errno == EPERM);
    CHECK(chown("/mnt/usb/HELLO.TXT", 1000, 1000) == 0);
    CHECK(link("/mnt/usb/HELLO.TXT", "/mnt/usb/hard") == -1 && errno == EPERM);
    CHECK(symlink("HELLO.TXT", "/mnt/usb/soft") == -1 && errno == EPERM);
    CHECK(mknod("/mnt/usb/fifo", S_IFIFO | 0644, 0) == -1 && errno == EPERM);
    struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, {.tv_sec = 1700000000, .tv_nsec = 0}};
    CHECK(utimensat(AT_FDCWD, "/mnt/usb/HELLO.TXT", times, 0) == 0 && stat("/mnt/usb/HELLO.TXT", &st) == 0 && st.st_mtime == 1700000000);
    CHECK(rename("/mnt/usb/HELLO.TXT", "/tmp/hello") == -1 && errno == EXDEV);
    CHECK(open("/mnt/usb/bad:name", O_WRONLY | O_CREAT, 0644) == -1 && errno == EINVAL);
    /* large file: sequential write, truncate, extend with zeros */
    write_pattern("/mnt/usb/big.bin", 5 << 20, 5);
    long long before = free_blocks("/mnt/usb");
    CHECK(truncate("/mnt/usb/big.bin", 1 << 20) == 0);
    long long after_truncate = free_blocks("/mnt/usb"); if (after_truncate <= before) printf("[64] MOUNT free before=%lld after=%lld\n", before, after_truncate);
    CHECK(after_truncate > before);
    CHECK(truncate("/mnt/usb/big.bin", 3 << 20) == 0);
    int fd = open("/mnt/usb/big.bin", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    static uint8_t buffer[65536];
    CHECK(pread(fd, buffer, 4096, (1 << 20) - 4096) == 4096); for (int i = 0; i < 4096; i++) CHECK(buffer[i] == pat((1 << 20) - 4096 + i, 5));
    CHECK(pread(fd, buffer, 65536, (2 << 20)) == 65536); for (int i = 0; i < 65536; i++) CHECK(buffer[i] == 0);
    close(fd);
    /* a write past the end leaves a zero-filled gap */
    fd = open("/mnt/usb/gap.bin", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    CHECK(pwrite(fd, "end", 3, 200000) == 3 && fsync(fd) == 0);
    CHECK(pread(fd, buffer, 1000, 150000) == 1000); for (int i = 0; i < 1000; i++) CHECK(buffer[i] == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 200003); close(fd);
    /* shared mapping */
    fd = open("/mnt/usb/map.bin", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0 && ftruncate(fd, 65536) == 0);
    uint8_t* map = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(map != MAP_FAILED);
    for (int i = 0; i < 65536; i++) map[i] = pat(i, 6);
    CHECK(msync(map, 65536, MS_SYNC) == 0 && munmap(map, 65536) == 0 && close(fd) == 0);
    check_pattern("/mnt/usb/map.bin", 65536, 6);
    /* unlinked while open: the data stays readable, the space returns at close */
    write_pattern("/mnt/usb/orphan.bin", 1 << 20, 7);
    fd = open("/mnt/usb/orphan.bin", O_RDWR | O_CLOEXEC); CHECK(fd >= 0);
    long long held = free_blocks("/mnt/usb");
    CHECK(unlink("/mnt/usb/orphan.bin") == 0 && access("/mnt/usb/orphan.bin", F_OK) == -1);
    CHECK(pread(fd, buffer, 4096, 4096) == 4096 && buffer[5] == pat(4096 + 5, 7));
    CHECK(umount("/mnt/usb") == -1 && errno == EBUSY);
    close(fd);
    CHECK(free_blocks("/mnt/usb") > held);
    /* busy, remount ro/rw */
    fd = open("/mnt/usb/HELLO.TXT", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    CHECK(umount("/mnt/usb") == -1 && errno == EBUSY); close(fd);
    CHECK(chdir("/mnt/usb/SUB") == 0 && umount("/mnt/usb") == -1 && errno == EBUSY && chdir("/") == 0);
    CHECK(mount(NULL, "/mnt/usb", NULL, MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) == 0);
    CHECK(open("/mnt/usb/ro.txt", O_WRONLY | O_CREAT, 0644) == -1 && errno == EROFS);
    CHECK(statfs("/mnt/usb", &fs) == 0 && (fs.f_flags & ST_RDONLY));
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0 && strstr(text, "/mnt/usb vfat ro,"));
    CHECK(mount(NULL, "/mnt/usb", NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV, NULL) == 0);
    put_file("/mnt/usb/after-remount.txt", "rw again\n", 9);
    /* unmount, mount again: everything persisted */
    CHECK(umount("/mnt/usb") == 0 && umount("/mnt/usb") == -1 && errno == EINVAL);
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0 && !strstr(text, "/mnt/usb"));
    CHECK(access("/mnt/usb/HELLO.TXT", F_OK) == -1);
    CHECK(mount("/dev/sda1", "/mnt/usb", "vfat", 0, "") == 0);
    check_pattern(long_name, 100000, 2); check_pattern("/mnt/usb/map.bin", 65536, 6);
    CHECK(slurp("/mnt/usb/after-remount.txt", text, sizeof(text)) == 9 && stat("/mnt/usb/HELLO.TXT", &st) == 0 && st.st_mtime == 1700000000);
    CHECK(stat("/mnt/usb", &st) == 0 && st.st_uid == 0);                 /* default uid: the mounter */
    /* lazy unmount with an open file */
    fd = open("/mnt/usb/HELLO.TXT", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    CHECK(umount2("/mnt/usb", MNT_DETACH) == 0 && access("/mnt/usb/HELLO.TXT", F_OK) == -1);
    CHECK(pread(fd, text, 5, 0) == 5 && !memcmp(text, "hello", 5)); close(fd);
    /* errors */
    CHECK(mount("/dev/sda1", "/mnt/usb", "nosuchfs", 0, "") == -1 && errno == ENODEV);
    CHECK(mount("/dev/sda2", "/mnt/usb", "vfat", MS_RDONLY, "") == -1 && errno == EINVAL);   /* not FAT */
    CHECK(mount("/etc/hostname", "/mnt/usb", "vfat", 0, "") == -1 && (errno == ENOTBLK || errno == ENOENT));
    CHECK(mount("/dev/sda1", "/mnt/nonexistent", "vfat", 0, "") == -1 && errno == ENOENT);
    CHECK(mount("/dev/sda1", "/mnt/usb", "vfat", 0, "bogus=1") == -1 && errno == EINVAL);
    CHECK(mount("/dev/sda1", "/mnt/usb", "vfat", 0, "") == 0 && mount("/dev/sda1", "/mnt", "vfat", 0, "") == -1 && errno == EBUSY);
    CHECK(umount("/mnt/usb") == 0);
    say("ROOT PASSED: vfat mount/umount, /proc/mounts+mountinfo, statfs, LFN, rename/replace/move, vfat attribute rules, EXDEV, truncate/extend, holes, mmap, orphans, EBUSY, remount ro/rw, lazy unmount, errors");
}
static void phase_user(void) {
    /* the desktop user mounts removable media like udisks would */
    CHECK(getuid() == 1000);
    CHECK(mount("/dev/sda1", "/mnt", "vfat", MS_NOSUID | MS_NODEV, "") == -1 && errno == EPERM);
    CHECK(mount("/dev/sda1", "/media/nexus/STICK", "vfat", MS_NODEV, "") == -1 && errno == EPERM);   /* nosuid missing */
    CHECK(mount("/dev/sda1", "/media/nexus/STICK", "vfat", MS_NOSUID | MS_NODEV, "uid=1000,gid=1000,shortname=mixed,utf8,showexec,flush") == 0);
    struct stat st; CHECK(stat("/media/nexus/STICK/HELLO.TXT", &st) == 0 && (st.st_mode & 0111) == 0);   /* showexec */
    put_file("/media/nexus/STICK/from-user.txt", "desktop\n", 8);
    char text[4096]; CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0 && strstr(text, "/media/nexus/STICK vfat"));
    CHECK(umount("/media/nexus/STICK") == 0);
    CHECK(access("/media/nexus/STICK", F_OK) == -1 && errno == ENOENT);   /* the kernel-made mount point went with it */
    say("USER PASSED: session-user mount of a USB stick under /media/nexus (created/removed), nosuid/nodev required, EPERM elsewhere");
}
/* Corrupted FAT images must never fault the kernel: mount fails or files read with errors. */
static uint32_t rng = 12345;
static uint32_t next_random(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void walk_tree(const char* path, int depth) {
    if (depth > 6) return;
    DIR* d = opendir(path); if (!d) return;
    struct dirent* e; int count = 0;
    while ((e = readdir(d)) && count++ < 200) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st; if (lstat(child, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk_tree(child, depth + 1);
        else { int fd = open(child, O_RDONLY); if (fd >= 0) { static uint8_t b[65536]; while (read(fd, b, sizeof(b)) > 0) {} close(fd); } }
    }
    closedir(d);
}
static void phase_fuzz(void) {
    CHECK(geteuid() == 0);
    int template = open("/dev/sdb2", O_RDONLY | O_CLOEXEC); CHECK(template >= 0);
    size_t size = 2 << 20; uint8_t* image = malloc(size), *work = malloc(size); CHECK(image && work);
    size_t done = 0; while (done < size) { ssize_t n = pread(template, image + done, size - done, done); CHECK(n > 0); done += n; }
    close(template);
    mkdir("/mnt", 0755); CHECK(mkdir("/mnt/fuzz", 0755) == 0 || errno == EEXIST);
    int target = open("/dev/sdb1", O_RDWR | O_CLOEXEC); CHECK(target >= 0);
    unsigned mounted = 0, failed = 0;
    for (unsigned round = 0; round < 120; round++) {
        memcpy(work, image, size);
        unsigned mutations = 1 + next_random() % 24;
        for (unsigned k = 0; k < mutations; k++) {
            uint32_t r = next_random(); size_t at;
            switch (r % 4) {
            case 0: at = next_random() % 512; break;                     /* boot sector / BPB */
            case 1: at = 512 + next_random() % 65536; break;              /* FATs */
            case 2: at = next_random() % 262144; break;                   /* root directory and early data */
            default: at = next_random() % size; break;
            }
            work[at] = (uint8_t)(r >> 8);
            if (r & 0x10000) work[at ^ 1] = (uint8_t)(r >> 24);
        }
        CHECK(pwrite(target, work, size, 0) == (ssize_t)size && fsync(target) == 0);
        if (mount("/dev/sdb1", "/mnt/fuzz", "vfat", 0, "") != 0) { failed++; continue; }
        mounted++;
        walk_tree("/mnt/fuzz", 0);
        int fd = open("/mnt/fuzz/fuzz-new.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { (void)!write(fd, "x", 1); close(fd); }
        (void)!mkdir("/mnt/fuzz/fuzzdir", 0755); (void)!rename("/mnt/fuzz/fuzz-new.txt", "/mnt/fuzz/fuzzdir/n"); (void)!unlink("/mnt/fuzz/fuzzdir/n");
        if (umount("/mnt/fuzz") != 0) CHECK(umount2("/mnt/fuzz", MNT_DETACH) == 0);
    }
    close(target);
    printf("[64] MOUNT FUZZ PASSED rounds=120 mounted=%u refused=%u\n", mounted, failed); fflush(stdout);
}
static void phase_removal(void) {
    CHECK(geteuid() == 0);
    mkdir("/mnt", 0755); CHECK(mkdir("/mnt/gone", 0755) == 0 || errno == EEXIST);
    CHECK(mount("/dev/sdb2", "/mnt/gone", "vfat", 0, "") == 0);
    write_pattern("/mnt/gone/r.bin", 1 << 20, 9);
    int fd = open("/mnt/gone/r.bin", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    say("WAITING REMOVE");
    static uint8_t buffer[65536]; bool failed = false;
    for (unsigned i = 0; i < 2000000 && !failed; i++) {
        int f2 = open("/mnt/gone/r.bin", O_RDONLY | O_CLOEXEC);
        if (f2 < 0) { failed = true; break; }
        ssize_t n = pread(f2, buffer, sizeof(buffer), (i * 65536) % (1 << 20));
        close(f2);
        if (n < 0) { CHECK(errno == EIO); failed = true; }
        /* cached pages may keep answering: force fresh I/O */
        int w = open("/mnt/gone/probe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (w < 0) { failed = true; break; }
        if (write(w, "p", 1) != 1 || fsync(w) != 0) failed = true;
        close(w);
        usleep(20000);
    }
    CHECK(failed);
    close(fd);
    CHECK(umount2("/mnt/gone", MNT_DETACH) == 0);
    char text[8192]; CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0 && !strstr(text, "/mnt/gone"));
    say("REMOVAL PASSED: surprise removal of a mounted stick: EIO to applications, lazy unmount, no fault");
}
int main(int argc, char** argv) {
    char mode[32] = "root";
    if (argc > 1) snprintf(mode, sizeof(mode), "%s", argv[1]);
    else if (slurp("/nexus/mount-mode", mode, sizeof(mode)) <= 0) strcpy(mode, "root");
    for (char* p = mode; *p; p++) if (*p == '\n') *p = 0;
    if (getuid() != 0 && geteuid() != 0 && strcmp(mode, "user") && access("/bin/mount64-root", X_OK) == 0) {
        /* init runs as the desktop user: the privileged phases run through the set-user-ID copy, then the user phase */
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { execl("/bin/mount64-root", "mount64-root", mode, (char*)NULL); _exit(98); }
        int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (!strcmp(mode, "root")) phase_user();
        say("ALL PASSED");
        return 0;
    }
    if (!strcmp(mode, "root")) phase_root();
    else if (!strcmp(mode, "user")) phase_user();
    else if (!strcmp(mode, "fuzz")) phase_fuzz();
    else if (!strcmp(mode, "removal")) phase_removal();
    else CHECK(!"unknown mode");
    if (getuid() == geteuid()) say("ALL PASSED");
    return 0;
}
