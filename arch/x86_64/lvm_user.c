/* /sbin/init on a root logical volume (tests/test_x64_lvm.py, static): the kernel activated the volume groups at boot
 * (lvm_boot.c) -- the root's VG before the root was looked for (also inside an unlocked LUKS volume), the others before
 * /etc/fstab. Checks: / is /dev/mapper/<vg>-root, /dev/<vg>/{root,data,datasnap} link to their dm devices, the
 * snapshot shows the data volume as it was when the snapshot was taken while /mnt/data (fstab) has the later file,
 * hostile VGs (/nexus/lvm-hostile) were refused, the root is writable; "[64] LVM TEST ALL PASSED" and power-off. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(1, "[64] LVM TEST FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); sync(); reboot(RB_POWER_OFF); _exit(99); } } while (0)
#define PASS(...) do { printf("[64] LVM TEST PASS " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static int slurp(const char* path, char* text, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, text, size - 1); close(fd); if (n < 0) return -1; text[n] = 0; return (int)n;
}
static void same_device(const char* link, const char* mapper) {   /* /dev/<vg>/<lv> -> ../dm-N, the same block device as /dev/mapper/... */
    struct stat a, b, l; char target[64];
    CHECK(lstat(link, &l) == 0 && S_ISLNK(l.st_mode));
    ssize_t n = readlink(link, target, sizeof(target) - 1); CHECK(n > 0); target[n] = 0;
    CHECK(!strncmp(target, "../dm-", 6));
    CHECK(stat(link, &a) == 0 && stat(mapper, &b) == 0 && S_ISBLK(a.st_mode) && a.st_rdev == b.st_rdev && major(a.st_rdev) == 253);
}
int main(void) {
    char vg[64] = "vgroot", text[8192];
    if (slurp("/nexus/lvm-vg", text, sizeof(text)) > 0) { text[strcspn(text, "\n")] = 0; snprintf(vg, sizeof(vg), "%s", text); }
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0);
    char want[128]; snprintf(want, sizeof(want), "/dev/mapper/%s-root / ", vg);
    CHECK(strstr(text, want) == text || strstr(text, want));
    PASS("root is %s-root", vg);
    char link[128], mapper[128];
    snprintf(link, sizeof(link), "/dev/%s/root", vg); snprintf(mapper, sizeof(mapper), "/dev/mapper/%s-root", vg); same_device(link, mapper);
    if (access("/nexus/lvm-luks", F_OK) == 0) {   /* LVM on LUKS: the PV is the unlocked mapping */
        CHECK(access("/dev/mapper/cryptroot", F_OK) == 0);
        char name[64]; DIR* d = opendir("/sys/block"); CHECK(d); struct dirent* e; int crypt = 0;
        while ((e = readdir(d))) if (!strncmp(e->d_name, "dm-", 3)) {
            char path[160]; snprintf(path, sizeof(path), "/sys/block/%s/dm/name", e->d_name);
            if (slurp(path, name, sizeof(name)) > 0 && !strncmp(name, "cryptroot", 9)) {
                snprintf(path, sizeof(path), "/sys/block/%s/holders", e->d_name);   /* the root LV holds it */
                DIR* h = opendir(path); struct dirent* x; if (h) { while ((x = readdir(h))) if (!strncmp(x->d_name, "dm-", 3)) crypt = 1; closedir(h); }
            }
        }
        closedir(d);
        CHECK(crypt);
        PASS("LVM on LUKS: /dev/mapper/cryptroot holds the root LV");
        printf("[64] LVM TEST ALL PASSED\n"); fflush(stdout); sync(); reboot(RB_POWER_OFF); return 99;
    }
    snprintf(link, sizeof(link), "/dev/%s/data", vg); snprintf(mapper, sizeof(mapper), "/dev/mapper/%s-data", vg); same_device(link, mapper);
    snprintf(link, sizeof(link), "/dev/%s/datasnap", vg); snprintf(mapper, sizeof(mapper), "/dev/mapper/%s-datasnap", vg); same_device(link, mapper);
    snprintf(link, sizeof(link), "/dev/%s", vg);
    DIR* d = opendir(link); CHECK(d); struct dirent* e; int seen = 0;
    while ((e = readdir(d))) seen += !strcmp(e->d_name, "root") + !strcmp(e->d_name, "data") + !strcmp(e->d_name, "datasnap") + 10 * !strcmp(e->d_name, "data-real");
    closedir(d);
    CHECK(seen == 3);   /* the visible LVs, no -real/-cow layers */
    PASS("/dev/%s lists root, data, datasnap linking to their dm devices", vg);
    char other[128];   /* /proc/mounts names the source as fstab gave it */
    snprintf(want, sizeof(want), "/dev/mapper/%s-data /mnt/data vfat", vg); snprintf(other, sizeof(other), "/dev/%s/data /mnt/data vfat", vg);
    CHECK(strstr(text, want) || strstr(text, other));
    char back[128];
    CHECK(slurp("/mnt/data/before.txt", back, sizeof(back)) > 0 && !strcmp(back, "before the snapshot\n"));
    CHECK(slurp("/mnt/data/after.txt", back, sizeof(back)) > 0 && !strcmp(back, "after the snapshot\n"));
    mkdir("/mnt/snap", 0755);
    snprintf(mapper, sizeof(mapper), "/dev/%s/datasnap", vg);
    CHECK(mount(mapper, "/mnt/snap", "vfat", MS_RDONLY, NULL) == 0);
    CHECK(slurp("/mnt/snap/before.txt", back, sizeof(back)) > 0 && !strcmp(back, "before the snapshot\n"));
    CHECK(access("/mnt/snap/after.txt", F_OK) == -1 && errno == ENOENT);
    CHECK(umount("/mnt/snap") == 0);
    PASS("fstab mounted /dev/%s/data (late activation); the snapshot shows it as it was", vg);
    if (access("/nexus/lvm-hostile", F_OK) == 0) {
        for (int i = 1; i <= 5; i++) { char bad[32]; snprintf(bad, sizeof(bad), "/dev/vgbad%d", i); CHECK(access(bad, F_OK) == -1); }
        CHECK(access("/dev/mapper/vgok-lv", F_OK) == 0);
        PASS("hostile volume groups refused, the good one beside them active");
    }
    int fd = open("/home/nexus/lvm-proof.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(fd >= 0 && write(fd, "written on a logical volume\n", 28) == 28 && fsync(fd) == 0 && close(fd) == 0);
    PASS("root writable");
    printf("[64] LVM TEST ALL PASSED\n"); fflush(stdout);
    sync(); reboot(RB_POWER_OFF);
    return 99;
}
