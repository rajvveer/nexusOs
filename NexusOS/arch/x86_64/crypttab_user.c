/* /sbin/init on an encrypted ext4 root with an /etc/crypttab (tests/test_x64_crypttab.py, static): the kernel unlocked
 * the root at boot, then the crypttab volumes (crypttab.c) before /etc/fstab mounted data1:
 *   data1  keyfile /etc/keys/data1.key (0400 root), luks,discard   -> open, the table has allow_discards, fstab mount
 *   data2  passphrase typed in the layout /etc/vconsole.conf names  -> open, mounted here
 *   data3  keyfile /etc/keys/data3.key with mode 0644                -> refused (not open)
 *   data4  a LUKS UUID no disk has, luks,nofail                       -> not open, the boot went on
 *   data5  keyfile data1.key, luks,readonly                           -> open read-only (writes fail)
 *   data6  luks,noauto                                                -> not open
 * Both volumes' files written by Debian's cryptsetup side are read back; new files are written for the host's
 * cryptsetup round trip. "[64] CRYPTTAB TEST PASS ..." lines, "[64] CRYPTTAB TEST ALL PASSED", then power-off. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(1, "[64] CRYPTTAB TEST FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); sync(); reboot(RB_POWER_OFF); _exit(99); } } while (0)
#define PASS(...) do { printf("[64] CRYPTTAB TEST PASS " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int slurp(const char* path, char* text, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, text, size - 1); close(fd); if (n < 0) return -1; text[n] = 0; return (int)n;
}
static int put(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text)); int ok = n == (ssize_t)strlen(text) && fsync(fd) == 0; close(fd); return ok ? 0 : -1;
}
/* the dm-crypt target's parameters (Linux DM_TABLE_STATUS with DM_STATUS_TABLE_FLAG) */
struct dm_ioctl { uint32_t version[3], data_size, data_start, target_count; int32_t open_count; uint32_t flags, event_nr, padding;
                  uint64_t dev; char name[128], uuid[129], data[7]; };
struct dm_target_spec { uint64_t sector_start, length; int32_t status; uint32_t next; char target_type[16]; };
static int table_of(const char* name, char* out, size_t size) {
    static union { struct dm_ioctl io; char raw[16384]; } b;
    memset(&b, 0, sizeof(b));
    b.io.version[0] = 4; b.io.data_size = sizeof(b); b.io.data_start = sizeof(struct dm_ioctl); b.io.flags = 1u << 4;
    snprintf(b.io.name, sizeof(b.io.name), "%s", name);
    int fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC); if (fd < 0) return -1;
    int r = ioctl(fd, _IOWR(0xfd, 12, struct dm_ioctl), &b); close(fd);
    if (r || b.io.target_count != 1) return -1;
    struct dm_target_spec* t = (void*)(b.raw + b.io.data_start);
    snprintf(out, size, "%s %s", t->target_type, (char*)(t + 1));
    return 0;
}
int main(void) {
    char text[4096];
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0);
    CHECK(strstr(text, "/dev/mapper/") != NULL);   /* the root is the unlocked mapping */
    PASS("encrypted root mounted");
    CHECK(access("/dev/mapper/data1", F_OK) == 0 && access("/dev/mapper/data2", F_OK) == 0 && access("/dev/mapper/data5", F_OK) == 0);
    PASS("data1 (keyfile), data2 (passphrase), data5 (read-only) open");
    CHECK(access("/dev/mapper/data3", F_OK) == -1 && errno == ENOENT);
    PASS("data3: keyfile with mode 0644 refused");
    CHECK(access("/dev/mapper/data4", F_OK) == -1 && access("/dev/mapper/data6", F_OK) == -1);
    PASS("data4 missing with nofail, data6 noauto: not open, boot went on");
    char table[512];
    CHECK(table_of("data1", table, sizeof(table)) == 0 && strstr(table, "crypt ") == table && strstr(table, "allow_discards"));
    CHECK(table_of("data2", table, sizeof(table)) == 0 && !strstr(table, "allow_discards"));
    PASS("discard: data1's dm-crypt table has allow_discards, data2's not");
    CHECK(strstr(text, "/dev/mapper/data1 /mnt/data1 vfat") != NULL);
    PASS("/etc/fstab mounted /dev/mapper/data1 after crypttab");
    mkdir("/mnt/data2", 0755);
    CHECK(mount("/dev/mapper/data2", "/mnt/data2", "vfat", 0, NULL) == 0);
    char back[128];
    CHECK(slurp("/mnt/data1/debian-wrote.txt", back, sizeof(back)) > 0 && !strcmp(back, "written by cryptsetup data1\n"));
    CHECK(slurp("/mnt/data2/debian-wrote.txt", back, sizeof(back)) > 0 && !strcmp(back, "written by cryptsetup data2\n"));
    PASS("files written through Debian's cryptsetup read back");
    static char block[4096];
    int fd = open("/dev/mapper/data5", O_RDWR | O_CLOEXEC);
    int refused = fd < 0 ? errno : pwrite(fd, block, sizeof(block), 1 << 20) < 0 ? errno : 0;
    if (fd >= 0) close(fd);
    CHECK(refused == EROFS || refused == EACCES || refused == EPERM);
    CHECK((fd = open("/dev/mapper/data5", O_RDONLY | O_CLOEXEC)) >= 0 && pread(fd, block, sizeof(block), 0) == (ssize_t)sizeof(block)); close(fd);
    PASS("data5 is read-only (errno %d) and readable", refused);
    char name[64]; snprintf(name, sizeof(name), "nexus wrote data1 cpus=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    CHECK(put("/mnt/data1/nexus-wrote.txt", name) == 0);
    snprintf(name, sizeof(name), "nexus wrote data2 cpus=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    CHECK(put("/mnt/data2/nexus-wrote.txt", name) == 0);
    CHECK(umount("/mnt/data2") == 0);
    sync();
    PASS("files written for the cryptsetup round trip");
    printf("[64] CRYPTTAB TEST ALL PASSED\n"); fflush(stdout);
    sync(); reboot(RB_POWER_OFF);
    return 99;
}
