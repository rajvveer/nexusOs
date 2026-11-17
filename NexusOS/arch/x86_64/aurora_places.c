/* Files sidebar: Home, Computer and the removable drives the desktop mounted below /media (aurora_media.c),
 * each with Unmount and Eject. Eject unmounts every volume of that drive, then asks the kernel to stop it
 * (CDROMEJECT: SCSI START STOP UNIT; a USB stick then leaves /dev like udisks' power-off). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/cdrom.h>
#include "aurora_places.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

#define DRIVES 8
struct drive { char source[64], point[256], name[64]; };
static struct drive drives[DRIVES];
static unsigned drive_count;
static char signature[4096];
static uint64_t checked;

static void unescape(char* text) {
    char* out = text;
    for (char* in = text; *in; ) {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '3' && in[2] >= '0' && in[2] <= '7' && in[3] >= '0' && in[3] <= '7') {
            *out++ = (char)((in[1] - '0') << 6 | (in[2] - '0') << 3 | (in[3] - '0')); in += 4;
        } else *out++ = *in++;
    }
    *out = 0;
}
static bool media_point(const char* point) { return !strncmp(point, "/media/", 7) || !strncmp(point, "/run/media/", 11); }
/* Re-reads the mounts; true when the list of drives changed. */
static bool load(void) {
    FILE* f = fopen("/proc/self/mounts", "re"); if (!f) return false;
    char line[1024], next[sizeof(signature)] = ""; struct drive found[DRIVES]; unsigned count = 0;
    while (fgets(line, sizeof(line), f)) {
        char* save = NULL; char* source = strtok_r(line, " ", &save); char* point = strtok_r(NULL, " ", &save);
        if (!source || !point) continue;
        unescape(source); unescape(point);
        if (!media_point(point) || count >= DRIVES || strlen(source) >= sizeof(found[0].source) || strlen(point) >= sizeof(found[0].point)) continue;
        strcpy(found[count].source, source); strcpy(found[count].point, point);
        const char* leaf = strrchr(point, '/'); snprintf(found[count].name, sizeof(found[count].name), "%s", leaf ? leaf + 1 : point);
        if (strlen(next) + strlen(point) + 2 < sizeof(next)) { strcat(next, point); strcat(next, "\n"); }
        count++;
    }
    fclose(f);
    bool changed = strcmp(next, signature) != 0;
    memcpy(drives, found, sizeof(found)); drive_count = count; strcpy(signature, next);
    return changed;
}
bool places_tick(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    if (checked && ms - checked < 500) return false;
    checked = ms; return load();
}
static void eject_glyph(int x, int y, uint32_t color) {   /* a triangle over a bar */
    for (int row = 0; row < 6; row++) appui_px_fill(x + 6 - row, y + row, 2 * row + 1, 1, color);
    appui_px_fill(x, y + 8, 13, 2, color);
}
static const char* home_dir(void) { const char* h = getenv("HOME"); return h && h[0] == '/' && strlen(h) < 200 ? h : "/home/nexus"; }   /* the session user's */
void places_draw(int x, int y, int h, const char* current) {
    if (!checked) places_tick();
    appui_px_fill(x, y, PLACES_WIDTH, h, APPUI_SIDEBAR); appui_px_vsep(x + PLACES_WIDTH - 1, y, h);
    appui_px_text(x + 12, y + 8, "Places", APPUI_F_SMALL, APPUI_FAINT);
    static const char* const names[2] = {"Home", "Computer"}; const char* const paths[2] = {home_dir(), "/"};
    for (int i = 0; i < 2; i++) {
        bool here = !strcmp(current, paths[i]);
        appui_px_row(x + 4, y + 26 + i * 28, PLACES_WIDTH - 10, 26, here);
        ui_icon(ICON_FOLDER, x + 10, y + 29 + i * 28, 18); appui_px_text(x + 34, y + 30 + i * 28, names[i], APPUI_F_BODY, APPUI_TEXT);
    }
    appui_px_text(x + 12, y + 92, "Drives", APPUI_F_SMALL, APPUI_FAINT);
    if (!drive_count) appui_px_text(x + 12, y + 112, "No drives", APPUI_F_SMALL, APPUI_SUBTLE);
    for (unsigned i = 0; i < drive_count; i++) {
        int top = y + 110 + (int)i * 48; size_t n = strlen(drives[i].point);
        bool here = !strncmp(current, drives[i].point, n) && (current[n] == 0 || current[n] == '/');
        appui_px_row(x + 4, top, PLACES_WIDTH - 10, 46, here);
        char shown[20]; snprintf(shown, sizeof(shown), "%s", drives[i].name); if (strlen(drives[i].name) > 15) strcpy(shown + 13, "..");
        ui_icon(ICON_GENERIC, x + 10, top + 3, 18); appui_px_text(x + 34, top + 4, shown, APPUI_F_BODY, APPUI_TEXT);
        appui_px_text(x + 34, top + 26, "Unmount", APPUI_F_SMALL, APPUI_ACCENT);
        eject_glyph(x + PLACES_WIDTH - 34, top + 27, APPUI_ACCENT);
    }
}
/* Every mounted volume of the drive that holds `source` (sdb1, sdb2 -> sdb). */
static void disk_of(const char* source, char* disk, size_t capacity) {
    snprintf(disk, capacity, "%s", source);
    size_t n = strlen(disk);
    if (n > 5 && !strncmp(disk, "/dev/sd", 7)) { while (n > 7 && disk[n - 1] >= '0' && disk[n - 1] <= '9') disk[--n] = 0; return; }
    char* p = strrchr(disk, 'p');   /* nvme0n1p1, loop0p1, mmcblk0p1 */
    if (p && p > disk + 5 && p[-1] >= '0' && p[-1] <= '9' && p[1] >= '0' && p[1] <= '9') *p = 0;
}
static int unmount_point(const char* point) {
    if (umount2(point, 0)) return -errno;
    rmdir(point);   /* a FUSE mount's directory (the kernel removes the ones it created) */
    return 0;
}
int places_click(int x, int y, char* path, size_t capacity, char* message, size_t message_capacity) {
    if (x < 0 || x >= PLACES_WIDTH) return 0;
    for (int i = 0; i < 2; i++) if (y >= 26 + i * 28 && y < 52 + i * 28) { snprintf(path, capacity, "%s", i ? "/" : home_dir()); return 1; }
    for (unsigned i = 0; i < drive_count; i++) {
        int top = 110 + (int)i * 48;
        if (y < top || y >= top + 46) continue;
        struct drive d = drives[i];
        if (y < top + 24) { snprintf(path, capacity, "%s", d.point); return 1; }
        bool eject = x >= PLACES_WIDTH - 44;
        sync();
        if (!eject) {
            int error = unmount_point(d.point);
            if (error) snprintf(message, message_capacity, "\"%s\" is in use: close what uses it and try again (%s).", d.name, strerror(-error));
            else snprintf(message, message_capacity, "\"%s\" was unmounted.", d.name);
            printf("[64] AURORA FILES UNMOUNT source=%s point=%s result=%d\n", d.source, d.point, error); fflush(stdout);
        } else {
            char disk[64]; disk_of(d.source, disk, sizeof(disk)); int error = 0;
            for (unsigned k = 0; k < drive_count && !error; k++) {
                char other[64]; disk_of(drives[k].source, other, sizeof(other));
                if (!strcmp(other, disk)) error = unmount_point(drives[k].point);
            }
            if (!error) {
                int fd = open(disk, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0 || ioctl(fd, CDROMEJECT, 0)) error = -errno;
                if (fd >= 0) close(fd);
                if (error) snprintf(message, message_capacity, "\"%s\" was unmounted, but the drive could not be stopped (%s). It is safe to remove.", d.name, strerror(-error));
                else snprintf(message, message_capacity, "\"%s\" can be removed safely now.", d.name);
            } else snprintf(message, message_capacity, "\"%s\" is in use: close what uses it and try again (%s).", d.name, strerror(-error));
            printf("[64] AURORA FILES EJECT source=%s disk=%s result=%d\n", d.source, disk, error); fflush(stdout);
        }
        load();
        size_t n = strlen(d.point);   /* the folder shown was on that drive: back to Home */
        if (!strncmp(path, d.point, n) && (path[n] == 0 || path[n] == '/')) { snprintf(path, capacity, "%s", home_dir()); return 3; }
        return 2;
    }
    return 0;
}
