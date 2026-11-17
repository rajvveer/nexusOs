/* Aurora power UI (aurora_power.h).
 *
 * Kernel interfaces only (arch/x86_64/power.c, pm.c), as a Linux desktop would use them:
 *   - NETLINK_KOBJECT_UEVENT group 1: power_supply uevents (POWER_SUPPLY_*), backlight (BRIGHTNESS, SOURCE),
 *     thermal (EVENT=critical|hot) and SUBSYSTEM=power PM_EVENT=<state>-prepare|post-<state>|shutdown-prepare;
 *   - /sys/class/power_supply/<name>/..., /sys/class/backlight/<name>/{brightness,max_brightness};
 *   - /sys/power/state ("mem", "disk") written by a short-lived child (the write blocks until the resume while
 *     the compositor keeps drawing until the kernel freezes tasks), /sys/power/policy/{power_key,sleep_key}
 *     = "interactive" (the kernel acts on the keys itself again once this process is gone) and sleep_ack.
 * Power button: a dialog (Shut Down / Restart / Sleep / Cancel); a second press or 60 s without an answer shuts
 * down (graceful: windows are asked to close, then SIGTERM, then the desktop exits and the kernel syncs, unmounts
 * and powers off). Sleep key: sleep. Ctrl+Alt+Del: the dialog (the kernel has no VT keyboard; as on Linux with a
 * display server holding the keyboard, Ctrl-Alt-Del belongs to the desktop). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "aurora_power.h"
#include "../../kernel/macui.h"
#include "../../kernel/menubar.h"
#include "../../kernel/framebuffer.h"

#define BATTERIES 4
#define DIALOG_SECONDS 60
struct battery { char name[32]; bool present; int capacity; char status[20], level[16]; long long now, full, rate; };
static struct battery batteries[BATTERIES];
static int battery_count, ac_count;
static bool ac_online;
static char backlight[64]; static int brightness = -1, brightness_max;
static int uevent_fd = -1;
static bool dialog, panel, restart_requested, ack_pending, shutdown_requested;
static uint32_t dialog_until, ack_since;
static pid_t sleep_child; static char sleep_state[8];
static int pending_action;
static uint8_t keys_down[(KEY_MAX + 8) / 8], consumed[(KEY_MAX + 8) / 8];

static bool held(unsigned key) { return (keys_down[key / 8] & (1U << (key % 8))) != 0; }
static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000); }
static int read_text(const char* path, char* text, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, text, size - 1); close(fd);
    if (n < 0) return -1;
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' ')) n--;
    text[n] = 0; return (int)n;
}
static long long read_number(const char* directory, const char* name, long long fallback) {
    char path[256], text[32]; snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (read_text(path, text, sizeof(text)) <= 0) return fallback;
    char* end; long long v = strtoll(text, &end, 10); return *end ? fallback : v;
}
static int write_text(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC); if (fd < 0) return -errno;
    ssize_t n = write(fd, text, strlen(text)); int error = n < 0 ? -errno : 0; close(fd); return error;
}

/* ---- status ---- */
static struct battery* battery_named(const char* name, bool create) {
    for (int i = 0; i < battery_count; i++) if (!strcmp(batteries[i].name, name)) return &batteries[i];
    if (!create || battery_count == BATTERIES) return NULL;
    struct battery* b = &batteries[battery_count++]; memset(b, 0, sizeof(*b)); snprintf(b->name, sizeof(b->name), "%s", name); b->capacity = -1; return b;
}
static void read_supplies(void) {
    DIR* d = opendir("/sys/class/power_supply"); struct dirent* e; bool online = false; int mains = 0;
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char directory[300], text[32], path[340]; snprintf(directory, sizeof(directory), "/sys/class/power_supply/%s", e->d_name);
        snprintf(path, sizeof(path), "%s/type", directory);
        if (read_text(path, text, sizeof(text)) <= 0) continue;
        if (!strcmp(text, "Mains")) { mains++; if (read_number(directory, "online", 0)) online = true; continue; }
        if (strcmp(text, "Battery")) continue;
        struct battery* b = battery_named(e->d_name, true); if (!b) continue;
        b->present = read_number(directory, "present", 1) != 0;
        b->capacity = (int)read_number(directory, "capacity", -1);
        snprintf(path, sizeof(path), "%s/status", directory); if (read_text(path, b->status, sizeof(b->status)) < 0) b->status[0] = 0;
        snprintf(path, sizeof(path), "%s/capacity_level", directory); if (read_text(path, b->level, sizeof(b->level)) < 0) b->level[0] = 0;
        b->now = read_number(directory, "energy_now", read_number(directory, "charge_now", -1));
        b->full = read_number(directory, "energy_full", read_number(directory, "charge_full", -1));
        b->rate = read_number(directory, "power_now", read_number(directory, "current_now", -1));
    }
    closedir(d); ac_count = mains; ac_online = online;
}
static void read_backlight(void) {
    DIR* d = opendir("/sys/class/backlight"); struct dirent* e; backlight[0] = 0;
    if (!d) return;
    while ((e = readdir(d))) if (e->d_name[0] != '.' && strlen(e->d_name) < sizeof(backlight)) { strcpy(backlight, e->d_name); break; }
    closedir(d);
    if (!backlight[0]) return;
    char directory[96]; snprintf(directory, sizeof(directory), "/sys/class/backlight/%s", backlight);
    brightness_max = (int)read_number(directory, "max_brightness", 0); brightness = (int)read_number(directory, "brightness", -1);
    if (brightness_max <= 0 || brightness < 0) backlight[0] = 0;
}
/* Combined battery: percent, 1 charging / 2 discharging / 3 full or not charging, minutes left (-1 unknown). */
static int battery_summary(int* state, int* minutes) {
    long long now = 0, full = 0, rate = 0; int capacity = 0, counted = 0; bool charging = false, discharging = false;
    for (int i = 0; i < battery_count; i++) {
        const struct battery* b = &batteries[i]; if (!b->present || b->capacity < 0) continue;
        counted++; capacity += b->capacity;
        if (!strcmp(b->status, "Charging")) charging = true; else if (!strcmp(b->status, "Discharging")) discharging = true;
        if (b->now >= 0 && b->full > 0) { now += b->now; full += b->full; if (b->rate > 0) rate += b->rate; }
    }
    if (!counted) return -1;
    int percent = full > 0 ? (int)((now * 100 + full / 2) / full) : capacity / counted;
    if (percent > 100) percent = 100;
    *state = discharging ? 2 : charging ? 1 : 3; *minutes = -1;
    if (rate > 0 && full > 0) {
        long long left = discharging ? now * 60 / rate : charging ? (full - now) * 60 / rate : -1;
        if (left >= 0 && left < 100 * 60) *minutes = (int)left;
    }
    return percent;
}
/* ---- power mode (the kernel's /sys/power/policy/profile: cpufreq governors / EPP; nexus-upowerd's PowerProfiles API
 * reads the same file) and battery saver: on battery at the Low level a Balanced mode turns to Power Saver, connecting
 * power restores it (Windows' battery saver, GNOME's power-saver-on-low-battery). ---- */
static const char* const mode_names[3] = {"power-saver", "balanced", "performance"};
static const char* const mode_labels[3] = {"Low Power", "Balanced", "Performance"};
static int mode = -1; static bool mode_choices[3], saver_auto;
static void read_mode(void) {
    char text[96]; mode = -1; mode_choices[0] = mode_choices[1] = mode_choices[2] = false;
    if (read_text("/sys/power/policy/profile_choices", text, sizeof(text)) > 0)
        for (int i = 0; i < 3; i++) { const char* at = strstr(text, mode_names[i]); size_t n = strlen(mode_names[i]); mode_choices[i] = at && (!at[n] || at[n] == ' '); }
    if (read_text("/sys/power/policy/profile", text, sizeof(text)) > 0) for (int i = 0; i < 3; i++) if (!strcmp(text, mode_names[i])) mode = i;
}
static int mode_count(void) { return mode_choices[0] + mode_choices[1] + mode_choices[2]; }
static int set_mode(int value, bool automatic) {
    if (value < 0 || value > 2 || !mode_choices[value]) return -EINVAL;
    int error = write_text("/sys/power/policy/profile", mode_names[value]);
    printf("[64] AURORA POWER profile %s%s error=%d\n", mode_names[value], automatic ? " (battery saver)" : "", error); fflush(stdout);
    if (!error) mode = value;
    if (!automatic) saver_auto = false;   /* the user's own choice is kept when power returns */
    aurora_shell_redraw(false); return error;
}
static void status_changed(void) {
    int state = 0, minutes, percent = battery_summary(&state, &minutes);
    /* the item also opens the power mode when there is neither battery nor backlight but a CPU driver to switch */
    menubar_set_power(percent, state, backlight[0] != 0 || mode_choices[2]);
    aurora_shell_redraw(false);
}

/* ---- uevents ---- */
static const char* field(const char* message, size_t size, const char* key) {
    size_t k = strlen(key);
    for (size_t at = 0; at < size;) { const char* f = message + at; size_t n = strnlen(f, size - at); if (n > k && !memcmp(f, key, k) && f[k] == '=') return f + k + 1; at += n + 1; }
    return NULL;
}
static void supply_event(const char* m, size_t size) {
    const char* name = field(m, size, "POWER_SUPPLY_NAME"), *type = field(m, size, "POWER_SUPPLY_TYPE");
    if (!name || !type) return;
    if (!strcmp(type, "Mains")) {
        const char* online = field(m, size, "POWER_SUPPLY_ONLINE"); if (online) ac_online = *online == '1';
        if (ac_online && saver_auto) {   /* battery saver ends with external power */
            saver_auto = false; read_mode();
            if (mode == 0 && !set_mode(1, true)) aurora_shell_notice("Power connected: Balanced power mode is back.");
        }
        status_changed(); return;
    }
    if (strcmp(type, "Battery")) return;
    struct battery* b = battery_named(name, true); if (!b) return;
    char old_level[16]; snprintf(old_level, sizeof(old_level), "%s", b->level);
    const char* v;
    if ((v = field(m, size, "POWER_SUPPLY_PRESENT"))) b->present = *v == '1';
    if ((v = field(m, size, "POWER_SUPPLY_CAPACITY"))) b->capacity = atoi(v);
    if ((v = field(m, size, "POWER_SUPPLY_STATUS"))) snprintf(b->status, sizeof(b->status), "%s", v);
    if ((v = field(m, size, "POWER_SUPPLY_CAPACITY_LEVEL"))) snprintf(b->level, sizeof(b->level), "%s", v);
    if ((v = field(m, size, "POWER_SUPPLY_ENERGY_NOW")) || (v = field(m, size, "POWER_SUPPLY_CHARGE_NOW"))) b->now = atoll(v);
    if ((v = field(m, size, "POWER_SUPPLY_ENERGY_FULL")) || (v = field(m, size, "POWER_SUPPLY_CHARGE_FULL"))) b->full = atoll(v);
    if ((v = field(m, size, "POWER_SUPPLY_POWER_NOW")) || (v = field(m, size, "POWER_SUPPLY_CURRENT_NOW"))) b->rate = atoll(v);
    status_changed();
    if (strcmp(b->status, "Discharging") || !strcmp(old_level, b->level)) return;
    int state, minutes, percent = battery_summary(&state, &minutes); char text[160];
    if (!strcmp(b->level, "Critical")) {
        char action[24] = "hibernate"; if (read_text("/sys/power/policy/critical_battery_action", action, sizeof(action)) < 0) strcpy(action, "hibernate");
        snprintf(text, sizeof(text), "Battery critically low (%d%%). The computer will %s in 10 seconds unless you connect power.", percent,
                 !strcmp(action, "poweroff") ? "shut down" : action);
        aurora_shell_notice(text); printf("[64] AURORA POWER battery critical %d%%\n", percent); fflush(stdout);
    } else if (!strcmp(b->level, "Low")) {
        read_mode();
        bool saver = mode == 1 && mode_choices[0] && !ac_online && !set_mode(0, true);   /* battery saver */
        if (saver) saver_auto = true;
        if (minutes >= 0) snprintf(text, sizeof(text), "Battery low: %d%% remaining (about %d:%02d).%s", percent, minutes / 60, minutes % 60,
                                   saver ? " Low Power mode is on until you connect power." : " Connect power soon.");
        else snprintf(text, sizeof(text), "Battery low: %d%% remaining.%s", percent, saver ? " Low Power mode is on until you connect power." : " Connect power soon.");
        aurora_shell_notice(text); printf("[64] AURORA POWER battery low %d%%%s\n", percent, saver ? " battery-saver" : ""); fflush(stdout);
    }
}
static void pm_event(const char* event) {
    printf("[64] AURORA POWER pm-event %s\n", event); fflush(stdout);
    if (!strcmp(event, "suspend-prepare") || !strcmp(event, "hibernation-prepare") || !strcmp(event, "freeze-prepare")) {
        dialog = panel = false; menubar_close();
        if (aurora_lock_for_sleep) aurora_lock_for_sleep();
        aurora_shell_redraw(true); ack_pending = true; ack_since = now_ms();
    } else if (!strncmp(event, "post-", 5)) {
        ack_pending = false; read_supplies(); read_backlight(); status_changed();
        if (aurora_lock_after_sleep) aurora_lock_after_sleep();
        aurora_shell_redraw(true);   /* video memory may not have survived the sleep */
    } else if (!strcmp(event, "shutdown-prepare")) {
        dialog = panel = false; shutdown_requested = true;   /* critical battery/temperature or the kernel's power-key policy */
        aurora_shell_notice("The computer is shutting down."); aurora_shell_redraw(true);
    }
}
static int uevent_ready(int fd, uint32_t mask, void* data) {
    (void)mask; (void)data; char message[4096];
    for (;;) {
        ssize_t n = recv(fd, message, sizeof(message) - 1, MSG_DONTWAIT);
        if (n <= 0) break;
        message[n] = 0; size_t size = (size_t)n;
        const char* subsystem = field(message, size, "SUBSYSTEM"); if (!subsystem) continue;
        if (!strcmp(subsystem, "power_supply")) supply_event(message, size);
        else if (!strcmp(subsystem, "power")) {
            const char* e = field(message, size, "PM_EVENT"); if (e) pm_event(e);
            if (field(message, size, "POWER_PROFILE")) { read_mode(); aurora_shell_redraw(false); }   /* another program (PowerProfiles API) */
        }
        else if (!strcmp(subsystem, "backlight")) {
            const char* v = field(message, size, "BRIGHTNESS"); const char* source = field(message, size, "SOURCE");
            if (v) brightness = atoi(v);
            if (source && !strcmp(source, "hotkey") && brightness_max > 0) {
                char text[64]; snprintf(text, sizeof(text), "Display brightness %d%%", brightness * 100 / brightness_max); aurora_shell_notice(text);
            }
            aurora_shell_redraw(false);
        } else if (!strcmp(subsystem, "thermal")) {
            const char* e = field(message, size, "EVENT");
            if (e && !strcmp(e, "critical")) aurora_shell_notice("The computer is too hot and is shutting down.");
            else if (e && !strcmp(e, "hot")) aurora_shell_notice("The computer is too hot; saving your session and powering off.");
        }
    }
    return 0;
}

/* SIGTERM (the kernel's orderly shutdown ends init last; or any service manager): shut the session down cleanly. */
static int term_pipe[2] = {-1, -1};
static void term_signal(int number) { (void)number; int saved = errno; if (term_pipe[1] >= 0) { ssize_t n = write(term_pipe[1], "", 1); (void)n; } errno = saved; }
static int term_ready(int fd, uint32_t mask, void* data) {
    (void)mask; (void)data; char drain[16]; while (read(fd, drain, sizeof(drain)) > 0) {}
    puts("[64] AURORA POWER SIGTERM: shutting down"); fflush(stdout);
    dialog = panel = false; shutdown_requested = true; aurora_shell_redraw(true); return 0;
}
bool aurora_power_init(struct wl_event_loop* loop) {
    read_supplies(); read_backlight(); read_mode();
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (fd >= 0 && (bind(fd, (struct sockaddr*)&address, sizeof(address)) || !wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, uevent_ready, NULL))) { close(fd); fd = -1; }
    uevent_fd = fd;
    if (!pipe2(term_pipe, O_NONBLOCK | O_CLOEXEC) && wl_event_loop_add_fd(loop, term_pipe[0], WL_EVENT_READABLE, term_ready, NULL)) {
        struct sigaction action = {.sa_handler = term_signal, .sa_flags = SA_RESTART}; sigemptyset(&action.sa_mask); sigaction(SIGTERM, &action, NULL);
    }
    /* The desktop answers the buttons while it runs; the kernel's own policy returns when this process ends. */
    int key_error = write_text("/sys/power/policy/power_key", "interactive"); write_text("/sys/power/policy/sleep_key", "interactive");
    int state = 0, minutes; int percent = battery_summary(&state, &minutes);
    menubar_set_power(percent, state, backlight[0] != 0 || mode_choices[2]);
    printf("[64] AURORA POWER ready uevents=%s batteries=%d ac=%d backlight=%s keys=%s profile=%s modes=%d\n", fd >= 0 ? "yes" : "no", battery_count, ac_count,
           backlight[0] ? backlight : "none", key_error ? "kernel" : "interactive", mode >= 0 ? mode_names[mode] : "none", mode_count());
    fflush(stdout);
    return fd >= 0;
}

/* ---- requests ---- */
/* The request goes out once every key is up and SLEEP_SETTLE_MS passed: the release of the key or button that chose
 * Sleep would otherwise be the first wake event (keyboards and mice are wake sources). */
#define SLEEP_SETTLE_MS 600
static uint32_t sleep_due; static bool sleep_queued;
static bool any_key_held(void) { for (unsigned i = 0; i < sizeof(keys_down); i++) if (keys_down[i]) return true; return false; }
static void request_sleep(const char* state) {
    if (sleep_child > 0 || sleep_queued) return;
    snprintf(sleep_state, sizeof(sleep_state), "%s", state);
    sleep_queued = true; sleep_due = now_ms() + SLEEP_SETTLE_MS;
}
static void sleep_start(void) {
    sleep_queued = false;
    printf("[64] AURORA POWER request %s\n", sleep_state); fflush(stdout);
    const char* state = sleep_state;
    pid_t child = fork();
    if (!child) {
        int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC); if (fd < 0) _exit(errno & 127);
        ssize_t n = write(fd, state, strlen(state)); _exit(n < 0 ? (errno & 127 ? errno & 127 : 1) : 0);
    }
    if (child < 0) { aurora_shell_notice("Sleep is not possible right now: no process could be started."); return; }
    sleep_child = child;
}
bool aurora_power_reaped(pid_t pid, int status) {
    if (pid != sleep_child || pid <= 0) return false;
    sleep_child = 0; int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1; bool disk = !strcmp(sleep_state, "disk");
    printf("[64] AURORA POWER %s request returned %d\n", sleep_state, code); fflush(stdout);
    if (!code) return true;
    const char* text = code == EBUSY ? (disk ? "Hibernation was cancelled: an application or device did not stop in time." : "Sleep was cancelled: an application or device did not stop in time.") :
                       code == EACCES || code == EPERM ? "You are not allowed to put this computer to sleep." :
                       disk ? "Hibernation is not available: no valid hibernation area (swap partition) was found." : "This computer cannot sleep.";
    aurora_shell_notice(text); return true;
}
void aurora_power_restart(bool restart) { restart_requested = restart; }
void aurora_power_finish(void) {
    if (!restart_requested) return;
    printf("[64] AURORA POWER restarting\n"); fflush(stdout); sync();
    if (reboot(RB_AUTOBOOT)) perror("Aurora restart");   /* not allowed: plain exit (power-off) */
}

/* ---- dialog + panel geometry ---- */
#define DW 520
#define DH 184
#define PW 300
static void dialog_box(int* x, int* y) { int sw = (int)fb_get_width(), sh = (int)fb_get_height(); *x = sw / 2 - DW / 2; *y = sh / 3 - DH / 2; if (*y < MENUBAR_H + 8) *y = MENUBAR_H + 8; }
static const char* const button_label[] = {"Cancel", "Sleep", "Restart", "Shut Down"};
static void button_box(int i, int* bx, int* by, int* bw) { int x, y; dialog_box(&x, &y); *bw = i == 3 ? 104 : 88; *by = y + DH - 46; *bx = x + DW - 20 - 104 - (3 - i) * 98 + (i == 3 ? 0 : 0); }
static int panel_height(void) {
    int state, minutes; int h = 20; if (battery_summary(&state, &minutes) >= 0) h += 92;
    if (mode_count() >= 2) h += 66;
    if (backlight[0]) h += 62;
    return h;
}
static void panel_box(int* x, int* y, int* w, int* h) {
    int sw = (int)fb_get_width(), at = menubar_power_x();
    *w = PW; *h = panel_height(); *x = at + 24 - PW; if (*x > sw - PW - 8) *x = sw - PW - 8; if (*x < 8) *x = 8; *y = MENUBAR_H + 6;
}
static void slider_box(int* sx, int* sy, int* sw) { int x, y, w, h; panel_box(&x, &y, &w, &h); *sx = x + 20; *sw = w - 40; *sy = y + h - 30; }
/* Power mode buttons (the shown choices side by side) below the battery rows. */
static bool mode_box(int i, int* bx, int* by, int* bw) {
    if (!mode_choices[i] || mode_count() < 2) return false;
    int x, y, w, h, state, minutes; panel_box(&x, &y, &w, &h);
    int slot = 0; for (int k = 0; k < i; k++) slot += mode_choices[k];
    int n = mode_count(), gap = 6, width = (w - 40 - gap * (n - 1)) / n;
    *bw = width; *bx = x + 20 + slot * (width + gap);
    *by = y + 14 + (battery_summary(&state, &minutes) >= 0 ? 92 : 0) + 10 + 24;
    return true;
}

bool aurora_power_open(void) { return dialog || panel; }
bool aurora_power_bounds(int* x, int* y, int* w, int* h) {
    if (dialog) { dialog_box(x, y); *x -= 30; *y -= 20; *w = DW + 60; *h = DH + 60; return true; }
    if (panel) { panel_box(x, y, w, h); *x -= 20; *y -= 14; *w += 40; *h += 40; return true; }
    return false;
}
static void icon_power(int cx, int cy, int r, uint32_t color) {
    ui_arc_up(cx, cy, r, 4, color, 255); ui_ring(cx, cy, r, 4, color, 120); ui_line(cx, cy - r - 3, cx, cy - 2, 4, color, 255);
}
void aurora_power_draw(void) {
    if (dialog) {
        int x, y; dialog_box(&x, &y);
        ui_shadow(x, y, DW, DH, 12, 24, 120, 8); ui_rrect(x, y, DW, DH, 12, UI_RGB(44, 44, 50), 250); ui_rrect_stroke(x, y, DW, DH, 12, UI_RGB(110, 110, 118), 140);
        icon_power(x + 52, y + 56, 20, 0xFFFFFF);
        ui_text(x + 96, y + 28, "Are you sure you want to shut down your computer now?", UF_UIB, 0xFFFFFF);
        int left = (int)(dialog_until - now_ms() + 999) / 1000; if (left < 0) left = 0; if (left > DIALOG_SECONDS) left = DIALOG_SECONDS;
        char line[96]; snprintf(line, sizeof(line), "automatically in %d second%s.", left, left == 1 ? "" : "s");
        ui_text(x + 96, y + 56, "If you do nothing, the computer will shut down", UF_UI, UI_RGB(210, 210, 216));
        ui_text(x + 96, y + 76, line, UF_UI, UI_RGB(210, 210, 216));
        ui_text(x + 96, y + 102, "Press the power button again to shut down now.", UF_SMALL, UI_RGB(150, 150, 158));
        for (int i = 0; i < 4; i++) {
            int bx, by, bw; button_box(i, &bx, &by, &bw); bool main_button = i == 3;
            ui_rrect(bx, by, bw, 28, 7, main_button ? UI_RGB(10, 100, 230) : UI_RGB(86, 86, 94), 255);
            ui_text_center(bx + bw / 2, by + (28 - ui_font_height(UF_UI)) / 2 + 1, button_label[i], UF_UI, 0xFFFFFF);
        }
        return;
    }
    if (!panel) return;
    int x, y, w, h; panel_box(&x, &y, &w, &h);
    ui_shadow(x, y, w, h, 10, 18, 110, 6); ui_rrect(x, y, w, h, 10, UI_RGB(42, 42, 46), 246); ui_rrect_stroke(x, y, w, h, 10, UI_RGB(110, 110, 116), 120);
    int row = y + 14, state = 0, minutes = -1, percent = battery_summary(&state, &minutes); char text[96];
    if (percent >= 0) {
        ui_text(x + 20, row, "Battery", UF_UIB, 0xFFFFFF);
        snprintf(text, sizeof(text), "%d%%", percent); ui_text(x + w - 20 - ui_text_width(text, UF_UIB), row, text, UF_UIB, 0xFFFFFF); row += 26;
        snprintf(text, sizeof(text), "Power Source: %s", ac_online ? "Power Adapter" : "Battery"); ui_text(x + 20, row, text, UF_UI, UI_RGB(210, 210, 216)); row += 22;
        if (state == 2) { if (minutes >= 0) snprintf(text, sizeof(text), "%d:%02d remaining", minutes / 60, minutes % 60); else snprintf(text, sizeof(text), "Calculating time remaining..."); }
        else if (state == 1) { if (minutes >= 0) snprintf(text, sizeof(text), "Charging, %d:%02d until full", minutes / 60, minutes % 60); else snprintf(text, sizeof(text), "Charging"); }
        else snprintf(text, sizeof(text), percent >= 95 ? "Fully charged" : "Not charging");
        ui_text(x + 20, row, text, UF_UI, UI_RGB(210, 210, 216)); row += 22;
        bool low = false; for (int i = 0; i < battery_count; i++) if (!strcmp(batteries[i].level, "Low") || !strcmp(batteries[i].level, "Critical")) low = true;
        ui_text(x + 20, row, low ? "Battery level is low" : "Battery condition: Normal", UF_SMALL, low ? UI_RGB(255, 120, 110) : UI_RGB(150, 150, 158)); row += 22;
    }
    if (mode_count() >= 2) {
        ui_blend_rect(x + 12, row, w - 24, 1, 0xFFFFFF, 40); row += 10;
        ui_text(x + 20, row, "Power Mode", UF_UIB, 0xFFFFFF);
        if (saver_auto) ui_text(x + w - 20 - ui_text_width("Battery saver", UF_SMALL), row + 2, "Battery saver", UF_SMALL, UI_RGB(255, 200, 90));
        for (int i = 0; i < 3; i++) {
            int bx, by, bw; if (!mode_box(i, &bx, &by, &bw)) continue;
            bool on = mode == i;
            ui_rrect(bx, by, bw, 26, 6, on ? UI_RGB(10, 100, 230) : UI_RGB(70, 70, 78), 255);
            ui_text_center(bx + bw / 2, by + (26 - ui_font_height(UF_SMALL)) / 2, mode_labels[i], UF_SMALL, 0xFFFFFF);
        }
        row += 56;
    }
    if (backlight[0]) {
        ui_blend_rect(x + 12, row, w - 24, 1, 0xFFFFFF, 40); row += 10;
        ui_text(x + 20, row, "Display Brightness", UF_UIB, 0xFFFFFF);
        int sx, sy, sw; slider_box(&sx, &sy, &sw);
        int fill = brightness_max > 0 && brightness >= 0 ? sw * brightness / brightness_max : 0;
        ui_rrect(sx, sy, sw, 8, 4, UI_RGB(90, 90, 98), 255); ui_rrect(sx, sy, fill < 8 ? 8 : fill, 8, 4, 0xFFFFFF, 255); ui_circle(sx + fill, sy + 4, 9, 0xFFFFFF, 255);
    }
}

/* ---- input ---- */
static void dialog_open(void) {
    dialog = true; panel = false; menubar_close(); dialog_until = now_ms() + DIALOG_SECONDS * 1000;
    printf("[64] AURORA POWER dialog open\n"); fflush(stdout); aurora_shell_redraw(true);
}
static int dialog_choice(int choice) {
    dialog = false; aurora_shell_redraw(true);
    printf("[64] AURORA POWER dialog choice %s\n", button_label[choice]); fflush(stdout);
    switch (choice) {
    case 1: request_sleep("mem"); return 0;
    case 2: return UI_ACT_RESTART;
    case 3: return UI_ACT_SHUTDOWN_NOW;
    default: return 0;
    }
}
static void set_brightness(int level) {
    if (!backlight[0] || brightness_max <= 0) return;
    if (level < 0) level = 0;
    if (level > brightness_max) level = brightness_max;
    char path[128], text[16]; snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", backlight); snprintf(text, sizeof(text), "%d", level);
    int error = write_text(path, text);
    if (!error) brightness = level; else aurora_shell_notice("The display brightness could not be changed.");
    printf("[64] AURORA POWER brightness %d/%d error=%d\n", level, brightness_max, error); fflush(stdout);
    aurora_shell_redraw(false);
}
static bool key_press(unsigned key) {
    if (key == KEY_POWER) {
        if (dialog) pending_action = dialog_choice(3);   /* second press: shut down now */
        else dialog_open();
        return true;
    }
    if (key == KEY_SLEEP) { dialog = panel = false; request_sleep("mem"); return true; }
    bool control = held(KEY_LEFTCTRL) || held(KEY_RIGHTCTRL), alt = held(KEY_LEFTALT) || held(KEY_RIGHTALT);
    if (key == KEY_DELETE && control && alt) { if (!dialog) dialog_open(); return true; }
    if (dialog) {
        if (key == KEY_ESC) dialog_choice(0);
        else if (key == KEY_ENTER || key == KEY_KPENTER) pending_action = dialog_choice(3);
        else if (key == KEY_S) dialog_choice(1);
        else if (key == KEY_R) pending_action = dialog_choice(2);
        return true;   /* modal */
    }
    if (panel) {
        if (key == KEY_ESC) { panel = false; puts("[64] AURORA POWER panel closed"); fflush(stdout); aurora_shell_redraw(true); }
        else if (key == KEY_LEFT || key == KEY_DOWN) set_brightness(brightness - (brightness_max + 9) / 10);
        else if (key == KEY_RIGHT || key == KEY_UP) set_brightness(brightness + (brightness_max + 9) / 10);
        else if (key >= KEY_1 && key <= KEY_3 && mode_count() >= 2) {   /* 1 Low Power, 2 Balanced, 3 Performance */
            if (set_mode((int)(key - KEY_1), false)) aurora_shell_notice("This power mode is not available.");
        }
        return true;
    }
    return false;
}
bool aurora_power_key(unsigned key, int value) {
    if (key > KEY_MAX || value == 2) return false;
    uint8_t mask = (uint8_t)(1U << (key % 8));
    if (!value) {   /* a release belongs to whoever took the press */
        keys_down[key / 8] &= (uint8_t)~mask;
        if (consumed[key / 8] & mask) { consumed[key / 8] &= (uint8_t)~mask; return true; }
        return false;
    }
    keys_down[key / 8] |= mask;
    if (!key_press(key)) return false;
    consumed[key / 8] |= mask; return true;
}
int aurora_power_click(int x, int y) {
    if (dialog) {
        for (int i = 0; i < 4; i++) { int bx, by, bw; button_box(i, &bx, &by, &bw); if (x >= bx && x < bx + bw && y >= by && y < by + 28) return dialog_choice(i); }
        return 0;   /* modal */
    }
    if (panel) {
        int px, py, pw, ph; panel_box(&px, &py, &pw, &ph);
        if (x < px || y < py || x >= px + pw || y >= py + ph) { panel = false; puts("[64] AURORA POWER panel closed"); fflush(stdout); aurora_shell_redraw(true); return 0; }
        for (int i = 0; i < 3; i++) {
            int bx, by, bw;
            if (mode_box(i, &bx, &by, &bw) && x >= bx && x < bx + bw && y >= by && y < by + 26) {
                if (set_mode(i, false)) aurora_shell_notice("The power mode could not be changed.");
                return 0;
            }
        }
        int sx, sy, sw; slider_box(&sx, &sy, &sw);
        if (backlight[0] && y >= sy - 12 && y < sy + 20 && x >= sx - 10 && x < sx + sw + 10) set_brightness(((x - sx) * brightness_max + sw / 2) / sw);
    }
    return 0;
}
bool aurora_power_action(int action) {
    if (action == UI_ACT_SLEEP) { request_sleep("mem"); return true; }
    if (action == UI_ACT_HIBERNATE) { request_sleep("disk"); return true; }
    if (action == UI_ACT_POWERDIALOG) { dialog_open(); return true; }
    if (action == UI_ACT_BATTERY) {
        panel = !panel; dialog = false; puts(panel ? "[64] AURORA POWER panel open" : "[64] AURORA POWER panel closed"); fflush(stdout);
        if (panel) {
            read_supplies(); read_backlight(); read_mode(); status_changed();
            int px, py, pw, ph, sx, sy, sw, mx[3] = {-1, -1, -1}, my = -1, mw = 0; panel_box(&px, &py, &pw, &ph); slider_box(&sx, &sy, &sw);
            for (int i = 0; i < 3; i++) { int bx, by, bw; if (mode_box(i, &bx, &by, &bw)) { mx[i] = bx + bw / 2; my = by + 13; mw = bw; } }
            printf("[64] AURORA POWER panel geometry x=%d y=%d w=%d h=%d slider_x=%d slider_y=%d slider_w=%d modes=%d,%d,%d mode_y=%d mode_w=%d mode=%s\n",
                   px, py, pw, ph, sx, sy, sw, mx[0], mx[1], mx[2], my, mw, mode >= 0 ? mode_names[mode] : "none");
            fflush(stdout);
        }
        aurora_shell_redraw(true); return true;
    }
    return false;
}
int aurora_power_timeout(uint32_t now) {
    int timeout = -1;
    if (dialog) { int32_t left = (int32_t)(dialog_until - now); timeout = left > 0 ? left % 1000 + 1 : 0; }   /* the countdown, once a second */
    if (sleep_queued) { int32_t left = (int32_t)(sleep_due - now); if (left < 0) left = 50; if (timeout < 0 || left < timeout) timeout = left; }
    return timeout;
}
int aurora_power_tick(uint32_t now, bool painted) {
    static int reported = -2; int item = menubar_power_x();
    if (painted && item != reported) { reported = item; printf("[64] AURORA POWER status item x=%d\n", item); fflush(stdout); }
    /* Sleep announced: acknowledge once the frame drawn after it is on screen (the lock screen shows on wake). */
    if (ack_pending && painted) {   /* the line before the write: the kernel freezes this process as soon as it sees the ack */
        ack_pending = false; printf("[64] AURORA POWER sleep acknowledged after %u ms\n", now - ack_since); fflush(stdout);
        int error = write_text("/sys/power/policy/sleep_ack", "1");
        if (error) { printf("[64] AURORA POWER sleep_ack failed error=%d\n", error); fflush(stdout); }
    }
    if (dialog) {
        static int shown = -1; int left = (int)((int32_t)(dialog_until - now) + 999) / 1000;
        if ((int32_t)(dialog_until - now) <= 0) { printf("[64] AURORA POWER dialog timed out\n"); fflush(stdout); return dialog_choice(3); }
        if (left != shown) { shown = left; aurora_shell_redraw(false); }
    }
    if (sleep_queued && (int32_t)(now - sleep_due) >= 0 && (!any_key_held() || (int32_t)(now - sleep_due) > 5000)) sleep_start();
    if (shutdown_requested) { shutdown_requested = false; sleep_queued = false; return UI_ACT_SHUTDOWN_NOW; }
    int action = pending_action; pending_action = 0; return action;
}
