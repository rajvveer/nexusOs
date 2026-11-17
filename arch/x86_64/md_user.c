/* /sbin/init on a RAID1 root (tests/test_x64_md.py, static): the kernel assembled the arrays at boot (md.c md_boot,
 * before the root was looked for). The mode comes from the kernel command line (mdtest=...):
 *   root      / is /dev/md0, /proc/mdstat shows md0 raid1 [2/2] [UU], md1 raid0 and the one good degraded array
 *             beside the hostile ones, sysfs md/ and md/dev-<member>/, the vfat file on the RAID0 array; writes a proof.
 *   degraded  booted with one member: [2/1] [U_], the root writable.
 *   readd     both members again: the stale one was kicked; ADD_NEW_DISK puts it back, recovery rebuilds it ([UU]).
 *   unclean   writes to md0 past the filesystem (the array dirty) until the host cuts the power.
 *   resync    after that power cut: the kernel resynchronised md0; waits for [UU] with no resync running.
 * "[64] MD TEST ALL PASSED" and power-off (md_shutdown marks the arrays clean). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(1, "[64] MD TEST FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); dump(); sync(); reboot(RB_POWER_OFF); _exit(99); } } while (0)
#define PASS(...) do { printf("[64] MD TEST PASS " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static int slurp(const char* path, char* text, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, text, size - 1); close(fd); if (n < 0) return -1; text[n] = 0; return (int)n;
}
static void dump(void) { char t[4096]; if (slurp("/proc/mdstat", t, sizeof(t)) > 0) dprintf(1, "[64] MD TEST mdstat:\n%s", t); }
static int count(const char* text, const char* word) { int n = 0; for (const char* p = text; (p = strstr(p, word)); p++) n++; return n; }
static void attr(const char* path, const char* want) {
    char t[256]; CHECK(slurp(path, t, sizeof(t)) > 0); t[strcspn(t, "\n")] = 0;
    if (strcmp(t, want)) dprintf(1, "[64] MD TEST %s = '%s', expected '%s'\n", path, t, want);
    CHECK(!strcmp(t, want));
}
static void wait_synced(int seconds) {   /* no resync/recovery line and md0 [UU] */
    char t[4096];
    for (int i = 0; i < seconds * 10; i++) {
        CHECK(slurp("/proc/mdstat", t, sizeof(t)) > 0);
        if (!strstr(t, "resync =") && !strstr(t, "recovery =") && strstr(t, "[2/2] [UU]")) return;
        struct timespec d = {0, 100000000}; nanosleep(&d, NULL);
    }
    CHECK(!"the array did not get in sync");
}
static void write_file(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text) && fsync(fd) == 0 && close(fd) == 0);
}
int main(void) {
    char cmdline[1024], mode[32] = "", text[8192], back[256];
    CHECK(slurp("/proc/cmdline", cmdline, sizeof(cmdline)) > 0);
    const char* m = strstr(cmdline, "mdtest="); CHECK(m);
    sscanf(m + 7, "%31[a-z]", mode);
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0);
    CHECK(strstr(text, "/dev/md0 / "));
    PASS("%s: the root is /dev/md0", mode);
    CHECK(slurp("/proc/mdstat", text, sizeof(text)) > 0);
    CHECK(strstr(text, "Personalities : [raid0] [raid1]") && strstr(text, "md0 : active raid1"));
    if (!strcmp(mode, "root")) {
        CHECK(strstr(text, "[2/2] [UU]"));
        CHECK(strstr(text, "md1 : active raid0") && strstr(text, "64k chunks"));
        CHECK(count(text, " : active") == 3 && count(text, "[2/1] [U_]") == 1);   /* md0, md1 and the good degraded one */
        PASS("/proc/mdstat: md0 raid1 [UU], md1 raid0, one good degraded array (the hostile ones refused)");
        attr("/sys/block/md0/md/level", "raid1"); attr("/sys/block/md0/md/raid_disks", "2"); attr("/sys/block/md0/md/metadata_version", "1.2");
        attr("/sys/block/md0/md/degraded", "0"); attr("/sys/block/md0/md/sync_action", "idle"); attr("/sys/block/md0/md/consistency_policy", "resync");
        attr("/sys/block/md0/md/dev-sda/state", "in_sync"); attr("/sys/block/md0/md/dev-sdb/slot", "1"); attr("/sys/block/md0/md/dev-sdb/block/dev", "8:16");
        attr("/sys/block/md1/md/level", "raid0"); attr("/sys/block/md1/md/chunk_size", "65536");
        PASS("sysfs md/ and md/dev-<member>/");
        CHECK((mkdir("/mnt", 0755) == 0 || errno == EEXIST) && (mkdir("/mnt/r0", 0755) == 0 || errno == EEXIST));
        CHECK(mount("/dev/md1", "/mnt/r0", "vfat", MS_RDONLY, NULL) == 0);
        CHECK(slurp("/mnt/r0/hello.txt", back, sizeof(back)) > 0 && !strcmp(back, "raid0 data\n"));
        CHECK(umount("/mnt/r0") == 0);
        PASS("the file mdadm's vfat holds on the RAID0 array");
        write_file("/md-proof.txt", "written on a RAID1 root\n");
        PASS("root writable");
    } else if (!strcmp(mode, "degraded")) {
        CHECK(strstr(text, "[2/1] [U_]"));
        attr("/sys/block/md0/md/degraded", "1");
        write_file("/degraded.txt", "written while degraded\n");
        PASS("degraded: [2/1] [U_], root writable");
    } else if (!strcmp(mode, "readd")) {
        CHECK(strstr(text, "[2/1] [U_]") && !strstr(text, "sdb["));   /* the stale member is not bound */
        CHECK(slurp("/degraded.txt", back, sizeof(back)) > 0 && !strcmp(back, "written while degraded\n"));
        struct stat st; CHECK(stat("/dev/sdb", &st) == 0 && S_ISBLK(st.st_mode));
        int md = open("/dev/md0", O_RDONLY | O_CLOEXEC); CHECK(md >= 0);
        int info[5] = {0, (int)major(st.st_rdev), (int)minor(st.st_rdev), 0, 0};
        CHECK(ioctl(md, 0x40140921UL, info) == 0);   /* ADD_NEW_DISK */
        close(md);
        wait_synced(120);
        attr("/sys/block/md0/md/dev-sdb/state", "in_sync"); attr("/sys/block/md0/md/degraded", "0");
        PASS("readd: the stale member was kicked, ADD_NEW_DISK + recovery made it in_sync again");
    } else if (!strcmp(mode, "unclean")) {
        static char block[1 << 20];
        for (unsigned i = 0; i < sizeof(block); i++) block[i] = (char)(i * 7 + 1);
        int fd = open("/dev/md0", O_WRONLY | O_CLOEXEC); CHECK(fd >= 0);
        for (unsigned round = 0;; round++) {   /* 64..80 MiB: past the 48 MiB filesystem */
            CHECK(pwrite(fd, block, sizeof(block), (off_t)(64 + round % 16) << 20) == (ssize_t)sizeof(block) && fsync(fd) == 0);
            if (!round) { attr("/sys/block/md0/md/array_state", "active"); printf("[64] MD TEST WRITING\n"); fflush(stdout); }
        }
    } else if (!strcmp(mode, "resync")) {
        wait_synced(120);
        attr("/sys/block/md0/md/sync_action", "idle"); attr("/sys/block/md0/md/resync_start", "none");
        PASS("resync after the power cut finished");
    } else CHECK(!"unknown mode");
    printf("[64] MD TEST ALL PASSED\n"); fflush(stdout);
    sync(); reboot(RB_POWER_OFF);
    return 99;
}
