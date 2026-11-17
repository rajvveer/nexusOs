/* Settings > System Upgrade: upgrade this FAT32 disk to the full NexusOS system (an ext4 root with the Debian base:
 * apt and the Software app, system services, printing, core updates) keeping every file - documents, browser
 * profiles, Wine programs, settings - and afterwards keep the result or go back to the old disk.
 *
 * The work is done while NexusOS is shut down, on the PC that runs it: the page writes a request on this disk and the
 * launcher (arch/x86_64/build.py desktop -> arch/x86_64/migrate_disk.py) carries it out before the next start, with
 * its progress in the launcher's window. Files (below ~/.local/state/nexus/upgrade of the desktop account /home/nexus:
 * on a FAT32 root the desktop user can write only there and in /tmp, kernel.c private_path):
 *   FAT disk   request.json   {"version": 1, "user": ..., "password_hash": ..., "hostname": ..., "timezone": ...,
 *                             "locale": ...}: the account's password on the full system (sudo, Software and printing
 *                             need one; hashed here, never stored in clear), written atomically
 *              failed.json    the launcher's reason when an attempt failed (the old disk is unchanged)
 *   new disk   result.json    the launcher's report: files compared, differences, names whose case was not known
 *              rollback-request / finish-request   go back to the old disk / delete it, at the next start
 * Serial lines for tests: "[64] UPGRADE STATE ...", "[64] UPGRADE CONTROLS ...", "[64] UPGRADE REQUEST ...". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "aurora_settings.h"
#include "aurora_auth.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

#define DIR_PATH "/home/nexus/.local/state/nexus/upgrade"   /* = migrate_disk.py UPDIR */
#define REQUEST_PATH DIR_PATH "/request.json"
#define FAILED_PATH DIR_PATH "/failed.json"
#define RESULT_PATH DIR_PATH "/result.json"
#define ROLLBACK_PATH DIR_PATH "/rollback-request"
#define FINISH_PATH DIR_PATH "/finish-request"
enum { ROOT_OTHER, ROOT_FAT, ROOT_EXT4 };
static int root_kind;
static bool requested, rollback_requested, finish_requested, firstboot_running, firstboot_failed;
static char failure[420], result_text[3][200], hostname[64], timezone_name[64], locale_name[48], user_name[33], old_disk[80];
static unsigned long long used_bytes, total_bytes; static unsigned packages;
static char inputs[2][64]; static int field;   /* password, confirm */
struct rect { int x, y, w, h; };
static struct rect hit_fields[2], hit_go, hit_cancel, hit_shutdown, hit_keep, hit_back;
static int page_x, page_y;

static bool inside(int x, int y, struct rect r) { return r.w && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static bool exists(const char* path) { struct stat st; return !stat(path, &st); }
static size_t read_text(const char* path, char* out, size_t size) {
    out[0] = 0; int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t n = read(fd, out, size - 1); close(fd); if (n < 0) n = 0; out[n] = 0; return (size_t)n;
}
/* the first line of a file, only characters of `allowed` (else empty) */
static void read_word(const char* path, char* out, size_t size, const char* allowed) {
    char text[256]; read_text(path, text, sizeof(text)); text[strcspn(text, "\r\n")] = 0;
    size_t n = strlen(text); if (!n || n >= size || strspn(text, allowed) != n || strstr(text, "..")) { out[0] = 0; return; }
    memcpy(out, text, n + 1);
}
/* "key": number or "key": "string" of a small flat JSON object (the launcher's own files) */
static long long json_number(const char* json, const char* key) {
    char pattern[64]; snprintf(pattern, sizeof(pattern), "\"%s\":", key); const char* p = strstr(json, pattern);
    return p ? strtoll(p + strlen(pattern), NULL, 10) : -1;
}
static void json_string(const char* json, const char* key, char* out, size_t size) {
    char pattern[64]; snprintf(pattern, sizeof(pattern), "\"%s\": \"", key); const char* p = strstr(json, pattern); out[0] = 0;
    if (!p) return; p += strlen(pattern); size_t n = 0;
    while (*p && *p != '"' && n + 1 < size) { char c = *p++; if (c == '\\' && *p) c = *p++; out[n++] = (unsigned char)c < 32 ? ' ' : c; }
    out[n] = 0;
}
static void load(void) {
    struct statfs fs; root_kind = ROOT_OTHER; used_bytes = total_bytes = 0;
    if (!statfs("/", &fs)) {
        root_kind = fs.f_type == 0x4d44 ? ROOT_FAT : fs.f_type == 0xEF53 ? ROOT_EXT4 : ROOT_OTHER;
        total_bytes = (unsigned long long)fs.f_blocks * (unsigned long long)fs.f_bsize; used_bytes = total_bytes - (unsigned long long)fs.f_bfree * (unsigned long long)fs.f_bsize;
    }
    requested = exists(REQUEST_PATH); rollback_requested = exists(ROLLBACK_PATH); finish_requested = exists(FINISH_PATH);
    read_word("/etc/hostname", hostname, sizeof(hostname), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-");
    if (!hostname[0]) snprintf(hostname, sizeof(hostname), "nexus");
    read_word("/etc/timezone", timezone_name, sizeof(timezone_name), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-/");
    const char* lang = getenv("LANG"); locale_name[0] = 0;
    if (lang && strlen(lang) < sizeof(locale_name) && strspn(lang, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@-") == strlen(lang)) snprintf(locale_name, sizeof(locale_name), "%s", lang);
    struct aurora_user me; if (aurora_user_by_uid(getuid(), &me)) snprintf(user_name, sizeof(user_name), "%s", me.name); else snprintf(user_name, sizeof(user_name), "nexus");
    char json[4096]; failure[0] = 0;
    if (read_text(FAILED_PATH, json, sizeof(json))) json_string(json, "reason", failure, sizeof(failure));
    for (int i = 0; i < 3; i++) result_text[i][0] = 0;
    old_disk[0] = 0; firstboot_running = firstboot_failed = false; packages = 0;
    if (read_text(RESULT_PATH, json, sizeof(json))) {
        char when[40]; json_string(json, "upgraded", when, sizeof(when)); json_string(json, "old_disk", old_disk, sizeof(old_disk));
        snprintf(result_text[0], sizeof(result_text[0]), "Upgraded on %s: %lld files (%.2f GB) copied and compared, %lld differences.",
                 when, json_number(json, "files"), (double)json_number(json, "bytes") / 1e9, json_number(json, "differences"));
        snprintf(result_text[1], sizeof(result_text[1]), "%lld file names from the old disk had no known capitalisation and are kept in lower case.",
                 json_number(json, "names_kept_lower_case"));
        snprintf(result_text[2], sizeof(result_text[2]), "The old disk is kept as %s on the PC until you choose.", old_disk[0] ? old_disk : "desktop64-fat.img");
    }
    if (root_kind == ROOT_EXT4) {   /* the first start installs the Debian base: is it still running, how far */
        static char status[1 << 20]; size_t n = read_text("/var/lib/dpkg/status", status, sizeof(status));
        for (const char* p = status; n && (p = strstr(p, "Status: install ok installed")); p++) packages++;
        int out[2];
        if (!pipe2(out, O_CLOEXEC)) {
            pid_t pid = fork();
            if (!pid) { dup2(out[1], 1); int null = open("/dev/null", O_WRONLY); if (null >= 0) dup2(null, 2); execl("/usr/bin/nexusctl", "nexusctl", "is-active", "nexus-firstboot", (char*)NULL); _exit(127); }
            close(out[1]); char text[64] = ""; ssize_t r = pid > 0 ? read(out[0], text, sizeof(text) - 1) : -1; close(out[0]);
            if (pid > 0) waitpid(pid, NULL, 0);
            if (r > 0) { text[r] = 0; firstboot_running = !strncmp(text, "activating", 10); firstboot_failed = !strncmp(text, "failed", 6); }
        }
    }
    const char* state = root_kind == ROOT_FAT ? (requested ? "scheduled" : failure[0] ? "failed" : "fat") :
                        root_kind == ROOT_EXT4 ? (result_text[0][0] ? (rollback_requested ? "rollback-scheduled" : finish_requested ? "finish-scheduled" : "upgraded") : "full") : "other";
    printf("[64] UPGRADE STATE %s used=%llu firstboot=%d packages=%u%s%s\n", state, used_bytes, firstboot_running ? 1 : firstboot_failed ? 2 : 0, packages, failure[0] ? " reason=" : "", failure); fflush(stdout);
    settings_redraw();
}
static bool write_file(const char* path, const char* text) {
    char dir[] = DIR_PATH; for (char* p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    mkdir(dir, 0755);
    char part[128]; snprintf(part, sizeof(part), "%s.part", path);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) return false;
    size_t n = strlen(text); bool ok = write(fd, text, n) == (ssize_t)n && !fsync(fd);
    if (close(fd)) ok = false;
    if (!ok || rename(part, path)) { unlink(part); return false; }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);   /* the rename itself, committed before "Shut Down Now" */
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return true;
}
static void schedule(void) {
    if (strlen(inputs[0]) < 6) { settings_message("The password needs at least 6 characters."); return; }
    if (strcmp(inputs[0], inputs[1])) { settings_message("The passwords do not match."); return; }
    char hash[256];
    if (!aurora_make_hash(inputs[0], hash, sizeof(hash))) { settings_message("Could not make the password hash (no random source)."); return; }
    memset(inputs, 0, sizeof(inputs)); field = 0;
    char json[1024]; time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm); char when[32]; strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
    snprintf(json, sizeof(json), "{\"version\": 1, \"requested\": \"%s\", \"user\": \"%s\", \"password_hash\": \"%s\", \"hostname\": \"%s\"%s%s%s%s%s%s}\n",
             when, user_name, hash, hostname, timezone_name[0] ? ", \"timezone\": \"" : "", timezone_name, timezone_name[0] ? "\"" : "",
             locale_name[0] ? ", \"locale\": \"" : "", locale_name, locale_name[0] ? "\"" : "");
    if (!write_file(REQUEST_PATH, json)) { char text[160]; snprintf(text, sizeof(text), "Could not write the request: %s", strerror(errno)); settings_message(text); return; }
    unlink(FAILED_PATH);
    printf("[64] UPGRADE REQUEST written user=%s host=%s\n", user_name, hostname); fflush(stdout);
    settings_message("The upgrade is scheduled: it starts the next time NexusOS starts.");
    load();
}
static void cancel(void) {
    const char* path = root_kind == ROOT_FAT ? REQUEST_PATH : rollback_requested ? ROLLBACK_PATH : FINISH_PATH;
    if (unlink(path) && errno != ENOENT) { settings_message("Could not cancel it."); return; }
    printf("[64] UPGRADE CANCELLED %s\n", path); fflush(stdout); settings_message("Cancelled."); load();
}
static void ask(const char* path, const char* what) {
    if (!write_file(path, "requested\n")) { char text[160]; snprintf(text, sizeof(text), "Could not ask for it: %s", strerror(errno)); settings_message(text); return; }
    printf("[64] UPGRADE %s requested\n", what); fflush(stdout); load();
}
static void shutdown_now(void) {
    printf("[64] UPGRADE SHUTDOWN requested\n"); fflush(stdout);
    if (kill(1, SIGTERM)) settings_message("Could not shut down: use the menu (Shut Down).");
    else settings_message("Shutting down...");
}
static void enter(void) { memset(inputs, 0, sizeof(inputs)); field = 0; load(); }
static int line(int x, int y, const char* text, int font, unsigned color) { appui_px_text(x, y, text, font, color); return y + (font == APPUI_F_SMALL ? 20 : 26); }
static void draw(int x, int y, int w, int h) {
    (void)w; (void)h; page_x = x; page_y = y;
    memset(hit_fields, 0, sizeof(hit_fields)); hit_go = hit_cancel = hit_shutdown = hit_keep = hit_back = (struct rect){0, 0, 0, 0};
    appui_px_text(x + 20, y + 14, "System Upgrade", APPUI_F_TITLE, APPUI_TEXT);
    int ty = y + 56; char text[240];
    if (root_kind == ROOT_FAT) {
        ty = line(x + 20, ty, "This disk runs the basic system (FAT32). The full system adds apt and the Software app, system", APPUI_F_BODY, APPUI_TEXT);
        ty = line(x + 20, ty, "services, printing and signed core updates, on an ext4 disk with permissions for every file.", APPUI_F_BODY, APPUI_TEXT);
        ty = line(x + 20, ty + 6, "Everything is kept: your documents, browser profiles, Windows programs (Wine) and settings. Every", APPUI_F_SMALL, APPUI_SUBTLE);
        ty = line(x + 20, ty, "file is compared with the original before the new disk is used, and the old disk stays on the PC", APPUI_F_SMALL, APPUI_SUBTLE);
        ty = line(x + 20, ty, "until you keep the upgrade or go back to it.", APPUI_F_SMALL, APPUI_SUBTLE);
        snprintf(text, sizeof(text), "In use on this disk: %.1f GB. The PC needs about %.1f GB free beside the disk file. It is done while NexusOS is off.",
                 (double)used_bytes / 1e9, (double)used_bytes / 1e9 + 1.6);
        ty = line(x + 20, ty + 6, text, APPUI_F_SMALL, APPUI_TEXT);
        if (failure[0]) { snprintf(text, sizeof(text), "The last attempt did not finish (the disk is unchanged): %.180s", failure); ty = line(x + 20, ty + 6, text, APPUI_F_SMALL, 0xE0675B); }
        ty += 16;
        if (requested) {
            ty = line(x + 20, ty, "The upgrade is scheduled: it starts the next time NexusOS starts.", APPUI_F_BOLD, APPUI_TEXT);
            ty = line(x + 20, ty, "Shut down now; the launcher shows the progress, then starts the upgraded system.", APPUI_F_SMALL, APPUI_SUBTLE);
            appui_px_button(x + 20, ty + 10, 170, 30, "Shut Down Now", true); hit_shutdown = (struct rect){20, ty + 10 - y, 170, 30};
            appui_px_button(x + 200, ty + 10, 170, 30, "Cancel the Upgrade", false); hit_cancel = (struct rect){200, ty + 10 - y, 170, 30};
        } else {
            snprintf(text, sizeof(text), "Account %s on the full system: a password is needed (administrator tasks).", user_name);
            ty = line(x + 20, ty, text, APPUI_F_BODY, APPUI_TEXT);
            static const char* const labels[2] = {"Password", "Confirm"};
            for (int i = 0; i < 2; i++) {
                char shown[64]; size_t n = strlen(inputs[i]); memset(shown, '*', n); shown[n] = 0;
                appui_px_text(x + 20, ty + 10, labels[i], APPUI_F_BODY, APPUI_SUBTLE); appui_px_field(x + 180, ty + 4, 280, 28, shown, i == field);
                hit_fields[i] = (struct rect){180, ty + 4 - y, 280, 28}; ty += 38;
            }
            snprintf(text, sizeof(text), "Computer name %s; time zone %s.", hostname, timezone_name[0] ? timezone_name : "Etc/UTC");
            ty = line(x + 20, ty + 4, text, APPUI_F_SMALL, APPUI_SUBTLE);
            appui_px_button(x + 180, ty + 8, 220, 30, "Upgrade at Next Start", true); hit_go = (struct rect){180, ty + 8 - y, 220, 30};
        }
    } else if (root_kind == ROOT_EXT4 && result_text[0][0]) {
        for (int i = 0; i < 3; i++) ty = line(x + 20, ty, result_text[i], i ? APPUI_F_SMALL : APPUI_F_BODY, i ? APPUI_SUBTLE : APPUI_TEXT);
        if (firstboot_running) { snprintf(text, sizeof(text), "Finishing: installing the system packages (%u installed so far). Keep NexusOS running.", packages); ty = line(x + 20, ty + 6, text, APPUI_F_BOLD, APPUI_TEXT); }
        if (firstboot_failed) {   /* a broken full system is never kept: only the way back is offered */
            ty = line(x + 20, ty + 6, "The first start could not install the system packages (nexusctl logs nexus-firstboot).", APPUI_F_BOLD, APPUI_TEXT);
            ty = line(x + 20, ty + 2, "Nothing of yours is lost: go back to the old disk, it is unchanged.", APPUI_F_SMALL, APPUI_SUBTLE);
        }
        ty += 16;
        if (rollback_requested || finish_requested) {
            ty = line(x + 20, ty, rollback_requested ? "Going back to the old disk the next time NexusOS starts (what changed here is kept beside it)."
                                                     : "The old disk is deleted on the PC the next time NexusOS starts.", APPUI_F_BOLD, APPUI_TEXT);
            appui_px_button(x + 20, ty + 10, 170, 30, "Shut Down Now", true); hit_shutdown = (struct rect){20, ty + 10 - y, 170, 30};
            appui_px_button(x + 200, ty + 10, 130, 30, "Cancel", false); hit_cancel = (struct rect){200, ty + 10 - y, 130, 30};
        } else {
            if (!firstboot_failed) { appui_px_button(x + 20, ty + 10, 230, 30, "Keep the Full System", true); hit_keep = (struct rect){20, ty + 10 - y, 230, 30}; }
            appui_px_button(x + 260, ty + 10, 230, 30, "Go Back to the Old Disk", false); hit_back = (struct rect){260, ty + 10 - y, 230, 30};
        }
    } else if (root_kind == ROOT_EXT4) {
        line(x + 20, ty, "This disk runs the full system.", APPUI_F_BODY, APPUI_TEXT);
    } else line(x + 20, ty, "Upgrades are made from a FAT32 desktop disk.", APPUI_F_BODY, APPUI_TEXT);
    static char reported[400]; char controls[400];   /* window-local control centres (tests drive the page by them) */
    #define C(r) (r).w ? page_x + (r).x + (r).w / 2 : 0, (r).w ? page_y + (r).y + (r).h / 2 : 0
    snprintf(controls, sizeof(controls), "[64] UPGRADE CONTROLS field0=%d,%d field1=%d,%d go=%d,%d cancel=%d,%d shutdown=%d,%d keep=%d,%d back=%d,%d\n",
             C(hit_fields[0]), C(hit_fields[1]), C(hit_go), C(hit_cancel), C(hit_shutdown), C(hit_keep), C(hit_back));
    #undef C
    if (strcmp(controls, reported)) { strcpy(reported, controls); fputs(controls, stdout); fflush(stdout); }
}
static void click(int x, int y) {
    for (int i = 0; i < 2; i++) if (inside(x, y, hit_fields[i])) { field = i; settings_redraw(); return; }
    if (inside(x, y, hit_go)) { schedule(); return; }
    if (inside(x, y, hit_cancel)) { cancel(); return; }
    if (inside(x, y, hit_shutdown)) { shutdown_now(); return; }
    if (inside(x, y, hit_keep)) { ask(FINISH_PATH, "FINISH"); return; }
    if (inside(x, y, hit_back)) { ask(ROLLBACK_PATH, "ROLLBACK"); return; }
}
static void key(char k) {
    if (root_kind != ROOT_FAT || requested) return;
    if (k == '\t') { field = (field + 1) % 2; settings_redraw(); return; }
    if (k == '\n') { if (field == 0) field = 1; else schedule(); settings_redraw(); return; }
    char* text = inputs[field]; size_t n = strlen(text);
    if (k == '\b') { if (n) text[n - 1] = 0; settings_redraw(); return; }
    if ((unsigned char)k >= 32 && (unsigned char)k < 127 && n < sizeof(inputs[0]) - 1) { text[n] = k; text[n + 1] = 0; settings_redraw(); }
}
static bool tick(void) {   /* the first start's progress: refresh every 5 s until the Debian base is installed */
    static int count;
    if (root_kind != ROOT_EXT4 || !result_text[0][0] || (!firstboot_running && packages >= 100) || ++count < 100) return false;
    count = 0; load(); return true;
}
const struct settings_page settings_upgrade = {"upgrade", "System Upgrade", ICON_SETTINGS, enter, draw, click, NULL, key, tick};
