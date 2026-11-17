/* nexus-os-install: install the NexusOS system running from a USB drive onto another disk.
 *
 *   nexus-os-install                 list the disks it can install to
 *   nexus-os-install DISK [--encrypt] [--yes] [--reboot]
 *
 * --encrypt: the installed root becomes a LUKS2 volume (the kernel asks for its passphrase twice on the console during
 * the copy; at every start cryptroot.c asks for it before the root is mounted).
 * DISK is a name from the list (nvme0n1, sda, /dev/nvme0n1). The tool checks that the system runs from a
 * NexusOS USB image (build.py usb-image), that DISK is another disk, large enough and not in use, asks for YES
 * (unless --yes), and records the request in /home/nexus/.nexus-os-install. The copy itself happens in the
 * kernel at the next start, before any program runs (arch/x86_64/osinstall.c), so the copied system is
 * consistent; the kernel asks once more on the console. --reboot restarts right away (reboot(2) is allowed for
 * the console session's user). Works in the Aurora Terminal (answers come from stdin) and on a text console. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUEST "/home/nexus/.nexus-os-install"

static bool read_text(const char* path, char* out, size_t capacity) {
    FILE* f = fopen(path, "re"); if (!f) return false;
    size_t n = fread(out, 1, capacity - 1, f); fclose(f);
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) n--;
    out[n] = 0; return true;
}
static unsigned long long number(const char* path) { char t[64]; return read_text(path, t, sizeof(t)) ? strtoull(t, NULL, 10) : 0; }
/* The root's partition and disk names from /proc/self/mounts and /sys/class/block. */
static bool root_device(char* part, size_t pcap, char* disk, size_t dcap) {
    FILE* f = fopen("/proc/self/mounts", "re"); if (!f) return false;
    char line[1024]; bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char source[256], point[512];
        if (sscanf(line, "%255s %511s", source, point) == 2 && !strcmp(point, "/") && !strncmp(source, "/dev/", 5)) { snprintf(part, pcap, "%s", source + 5); found = true; }
    }
    fclose(f);
    if (!found) return false;
    if (!strncmp(part, "mapper/", 7)) {   /* an encrypted root: the partition below /dev/mapper/NAME (sysfs slaves/) */
        DIR* d = opendir("/sys/block"); struct dirent* e; bool resolved = false;
        while (d && !resolved && (e = readdir(d))) {
            if (strncmp(e->d_name, "dm-", 3)) continue;
            char path[300], name[128]; snprintf(path, sizeof(path), "/sys/block/%s/dm/name", e->d_name);
            if (!read_text(path, name, sizeof(name)) || strcmp(name, part + 7)) continue;
            snprintf(path, sizeof(path), "/sys/block/%s/slaves", e->d_name);
            DIR* s = opendir(path); struct dirent* below;
            while (s && (below = readdir(s))) if (below->d_name[0] != '.') { snprintf(part, pcap, "%s", below->d_name); resolved = true; break; }
            if (s) closedir(s);
        }
        if (d) closedir(d);
        if (!resolved) return false;
        char path[300]; snprintf(path, sizeof(path), "/sys/class/block/%s/partition", part);
        if (access(path, F_OK)) { snprintf(disk, dcap, "%s", part); return true; }   /* slaves/ names the whole disk */
    }
    char link[300], target[512]; snprintf(link, sizeof(link), "/sys/class/block/%s", part);
    ssize_t n = readlink(link, target, sizeof(target) - 1); if (n <= 0) return false;
    target[n] = 0; char* last = strrchr(target, '/'); if (!last) return false;
    *last = 0; char* parent = strrchr(target, '/'); if (!parent) return false;
    snprintf(disk, dcap, "%s", parent + 1); return true;
}
static void describe(const char* name, char* out, size_t capacity) {
    char path[300], vendor[64] = "", model[64] = "";
    snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", name); read_text(path, vendor, sizeof(vendor));
    snprintf(path, sizeof(path), "/sys/block/%s/device/model", name); read_text(path, model, sizeof(model));
    snprintf(path, sizeof(path), "/sys/block/%s/size", name); unsigned long long sectors = number(path);
    snprintf(out, capacity, "%-10s %8.1f GiB  %s %s", name, sectors / 2097152.0, vendor, model);
}
static bool candidate(const char* name, const char* root_disk) {
    if (!strcmp(name, root_disk) || !strncmp(name, "loop", 4) || !strncmp(name, "dm-", 3) || !strncmp(name, "ram", 3)) return false;
    char path[300]; snprintf(path, sizeof(path), "/sys/block/%s/size", name);
    return number(path) > 0;
}
static int list(const char* root_disk) {
    DIR* d = opendir("/sys/block"); if (!d) { perror("nexus-os-install: /sys/block"); return 1; }
    struct dirent* e; unsigned count = 0;
    printf("Disks NexusOS can be installed on (the system runs from %s):\n", root_disk);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !candidate(e->d_name, root_disk)) continue;
        char text[256]; describe(e->d_name, text, sizeof(text)); printf("  %s\n", text); count++;
    }
    closedir(d);
    if (!count) puts("  (none)");
    puts("Usage: nexus-os-install DISK [--encrypt] [--yes] [--reboot]   -- erases DISK and installs NexusOS on it");
    return 0;
}
int main(int argc, char** argv) {
    const char* target = NULL; bool yes = false, restart = false, encrypt = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) yes = true;
        else if (!strcmp(argv[i], "--reboot")) restart = true;
        else if (!strcmp(argv[i], "--encrypt")) encrypt = true;   /* LUKS2 root: the passphrase is asked on the console at the copy */
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { puts("Usage: nexus-os-install [DISK [--encrypt] [--yes] [--reboot]]"); return 0; }
        else if (!target) target = !strncmp(argv[i], "/dev/", 5) ? argv[i] + 5 : argv[i];
        else { fprintf(stderr, "nexus-os-install: unexpected argument %s\n", argv[i]); return 2; }
    }
    char root_part[64], root_disk[64];
    if (!root_device(root_part, sizeof(root_part), root_disk, sizeof(root_disk))) { fputs("nexus-os-install: cannot tell which disk the system runs from\n", stderr); return 1; }
    if (!target) return list(root_disk);
    char path[300];
    if (strncmp(root_disk, "sd", 2)) { fprintf(stderr, "nexus-os-install: the system runs from %s, not from a USB drive\n", root_disk); return 1; }
    if (strchr(target, '/') || !candidate(target, root_disk)) { fprintf(stderr, "nexus-os-install: %s is not a disk NexusOS can be installed on (run without arguments for the list)\n", target); return 1; }
    snprintf(path, sizeof(path), "/sys/class/block/%s/start", root_part); unsigned long long start = number(path);
    snprintf(path, sizeof(path), "/sys/class/block/%s/size", root_part); unsigned long long size = number(path);
    snprintf(path, sizeof(path), "/sys/block/%s/size", target); unsigned long long have = number(path);
    snprintf(path, sizeof(path), "/sys/block/%s/queue/logical_block_size", target); unsigned long long block = number(path);
    unsigned long long need = start + size + 34 + (encrypt ? 32768 : 0);   /* the LUKS2 header takes 16 MiB more */
    if (!start || !size || have < need) { fprintf(stderr, "nexus-os-install: %s is too small (needs %.1f GiB)\n", target, need / 2097152.0); return 1; }
    if (block && block != 512) { fprintf(stderr, "nexus-os-install: %s uses %llu-byte sectors; only 512-byte sector disks are supported\n", target, block); return 1; }
    char text[256]; describe(target, text, sizeof(text));
    printf("Install NexusOS on:\n  %s\nALL DATA ON %s WILL BE ERASED. The copy happens when the computer restarts.\n", text, target);
    if (encrypt) puts("The disk will be encrypted (LUKS2, AES-XTS 512-bit, Argon2id): choose its passphrase on the console then;\n"
                      "it is asked at every start, and without it the data cannot be recovered.");
    if (!yes) {
        printf("Type YES to continue: "); fflush(stdout);
        char answer[32]; if (!fgets(answer, sizeof(answer), stdin)) return 1;
        answer[strcspn(answer, "\r\n")] = 0;
        if (strcmp(answer, "YES")) { puts("Cancelled."); return 1; }
    }
    char serial[64] = ""; snprintf(path, sizeof(path), "/sys/block/%s/device/serial", target); read_text(path, serial, sizeof(serial));
    char temporary[] = "/home/nexus/.nexus-os-install.XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0) { perror("nexus-os-install: request"); return 1; }
    char request[256]; int n = snprintf(request, sizeof(request), "target=%s\nserial=%s\n%s", target, serial, encrypt ? "encrypt=1\n" : "");
    if (write(fd, request, (size_t)n) != n || fsync(fd) || close(fd) || rename(temporary, REQUEST)) { perror("nexus-os-install: request"); unlink(temporary); return 1; }
    sync();
    printf("[64] OSINSTALL REQUEST target=%s serial=%s\n", target, serial);
    puts("Restart the computer (keep the USB drive in) to install; it asks once more before erasing.");
    fflush(stdout);
    if (restart) { sync(); if (reboot(RB_AUTOBOOT)) { perror("nexus-os-install: reboot"); return 1; } }
    return 0;
}
