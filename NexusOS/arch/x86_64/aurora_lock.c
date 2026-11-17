/* Aurora lock screen and greeter (aurora_lock.h). */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "aurora_lock.h"
#include "aurora_auth.h"
#include "aurora_control.h"
#include "aurora_session.h"
#include "aurora_sak.h"
#include "../../kernel/macui.h"

enum { OFF, LOCKED, GREETER };
static int mode;
static struct aurora_login_conf conf;
static struct aurora_user users[16]; static int user_count, selected;   /* greeter: the accounts; locked: [0] = session user */
static char typed[256]; static int typed_len;
static char message[128];
static pid_t checking;          /* password check in progress */
static unsigned checking_uid;   /* whose: the session it unlocks or switches to */
static unsigned signing_in;     /* a new session of this uid is being started (nexus-session) */
static bool switching;          /* the greeter came from Switch User: Esc goes back to the lock screen */
static int failures; static uint64_t blocked_until, last_activity;
static uint64_t last_tick;

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void wipe(void) { memset(typed, 0, sizeof(typed)); typed_len = 0; }
static void say(const char* text) { snprintf(message, sizeof(message), "%s", text); aurora_host_lock_redraw(); }
static const struct aurora_user* current(void) { return user_count ? &users[selected < user_count ? selected : 0] : NULL; }
static void load_users(bool all) {   /* greeter: every account; locked: the active session's user */
    user_count = 0; selected = 0; unsigned me = aurora_session_active_uid();
    struct aurora_user list[16]; int n = aurora_users(list, 16, false);
    for (int i = 0; i < n && user_count < 16; i++) if (all || list[i].uid == me) users[user_count++] = list[i];
    if (!user_count) { memset(&users[0], 0, sizeof(users[0])); snprintf(users[0].name, sizeof(users[0].name), "nexus"); users[0].uid = me; users[0].password = AURORA_PW_NONE; user_count = 1; }
    for (int i = 0; i < user_count; i++) if (users[i].uid == me) selected = i;
}
/* Accounts added or removed while the greeter is shown (Settings, useradd) appear without a restart. */
static struct timespec passwd_seen;
static void reload_if_changed(void) {
    struct stat st; if (stat("/etc/passwd", &st)) return;
    bool same = st.st_mtim.tv_sec == passwd_seen.tv_sec && st.st_mtim.tv_nsec == passwd_seen.tv_nsec; passwd_seen = st.st_mtim;
    if (same || mode != GREETER || typed_len || checking || signing_in) return;
    unsigned keep = user_count ? users[selected < user_count ? selected : 0].uid : 0; load_users(true);
    for (int i = 0; i < user_count; i++) if (users[i].uid == keep) selected = i;
    printf("[64] AURORA GREETER USERS users=%d\n", user_count); fflush(stdout); aurora_host_lock_redraw();
}
static bool session_has_password(void) {
    struct aurora_user u; if (!aurora_user_by_uid(aurora_session_active_uid(), &u)) return false;
    return u.password == AURORA_PW_SET || u.password == AURORA_PW_UNKNOWN;   /* unknown: an ext4 root hides /etc/shadow */
}
/* The sleep path reads no file: the kernel freezes user space sleep_delay_ms (500 ms) after PM_EVENT=*-prepare, and a
 * read of login.conf, /etc/passwd or /etc/shadow can wait that long behind another process's disk I/O (Chrome right
 * after a resume), so the lock screen and sleep_ack came too late. The options and whether the session's account has a
 * password are kept here instead: read at start-up, on reload-login (System Settings), whenever the lock state or the
 * active session changes, and every 30 s while unlocked (a password set with passwd(1) counts within 30 s). */
static int has_password = -1; static unsigned password_uid; static uint64_t password_read;
static void refresh_password(void) { has_password = session_has_password(); password_uid = aurora_session_active_uid(); password_read = now_ms(); }
static bool cached_password(void) { if (has_password < 0 || password_uid != aurora_session_active_uid()) refresh_password(); return has_password > 0; }
bool aurora_lock_active(void) { return mode != OFF; }
static void enter(int how, const char* reason) {
    bool was = mode != OFF;
    mode = how; wipe(); message[0] = 0; load_users(how == GREETER); if (how != GREETER) switching = false;
    { struct stat st; if (!stat("/etc/passwd", &st)) passwd_seen = st.st_mtim; }
    const struct aurora_user* u = current();
    if (u && u->password == AURORA_PW_NONE) say("No password is set: press Enter. (System Settings > Users)");
    if (!was) aurora_host_lock_changed(true);
    printf("[64] AURORA %s reason=%s user=%s users=%d\n", how == GREETER ? "GREETER" : "LOCKED", reason, u ? u->name : "-", user_count); fflush(stdout);
    aurora_host_lock_redraw();
}
void aurora_lock_now(const char* reason) { if (mode == OFF) enter(LOCKED, reason); else aurora_host_lock_redraw(); }
static void unlock(void) {
    const struct aurora_user* u = current();
    printf("[64] AURORA UNLOCKED user=%s mode=%s\n", u ? u->name : "-", mode == GREETER ? "greeter" : "lock"); fflush(stdout);
    mode = OFF; wipe(); message[0] = 0; failures = 0; last_activity = now_ms(); switching = false; refresh_password();
    aurora_host_lock_changed(false);
}
/* The password of uid was right (or it has none): back to its session, signing the desktop user in when needed. */
static void finish_unlock(unsigned uid) {
    if (uid == getuid() && !aurora_session_running(uid)) aurora_session_boot_login(true);
    else aurora_session_activate_uid(uid);
    unlock();
}
static void refused(int code) {
    printf("[64] AURORA LOCK FAILED code=%d (%s)\n", code, aurora_auth_message(code)); fflush(stdout);
    if (code == AURORA_AUTH_WRONG && ++failures >= 5) { blocked_until = now_ms() + 30000; failures = 0; say("Incorrect password. Too many attempts: wait 30 s."); }
    else if (code == AURORA_AUTH_THROTTLED) { blocked_until = now_ms() + 60000; say(aurora_auth_message(code)); }
    else if (code == AURORA_AUTH_UNAVAILABLE && checking_uid != getuid()) say("Other accounts can sign in once the disk has an ext4 (POSIX) root.");
    else if (code == AURORA_AUTH_DENIED) say("This account cannot sign in here.");
    else say(aurora_auth_message(code));
}
static void submit(void) {
    const struct aurora_user* u = current(); if (!u || checking) return;
    uint64_t now = now_ms();
    if (now < blocked_until) { char text[96]; snprintf(text, sizeof(text), "Too many attempts. Try again in %d s.", (int)((blocked_until - now + 999) / 1000)); say(text); return; }
    if (signing_in) return;
    if (u->password == AURORA_PW_LOCKED) { say("This account is locked."); wipe(); return; }
    checking_uid = u->uid;
    if (u->uid != getuid() && !aurora_session_running(u->uid)) {   /* a new session: the launcher checks the password */
        signing_in = u->uid; say("Signing in..."); aurora_session_start(u->name, typed); wipe(); return;
    }
    if (u->password == AURORA_PW_NONE) { finish_unlock(u->uid); return; }
    pid_t pid = fork();
    if (pid < 0) { say("Could not check the password (process limit)."); wipe(); return; }
    if (!pid) { int code = aurora_auth_verify(u->name, typed); memset(typed, 0, sizeof(typed)); _exit(code); }
    checking = pid; wipe(); say("Checking...");
}
bool aurora_lock_reaped(pid_t pid, int status) {
    if (!checking || pid != checking) return false;
    checking = 0;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : AURORA_AUTH_UNAVAILABLE;
    if (mode == OFF) return true;
    if (code == AURORA_AUTH_OK || code == AURORA_AUTH_NO_PASSWORD) { finish_unlock(checking_uid); return true; }
    refused(code);
    return true;
}
/* aurora_session.h services */
void aurora_lock_session_result(int code, unsigned uid) {
    if (!signing_in || (uid && uid != signing_in)) return;
    signing_in = 0;
    if (mode == OFF) return;
    if (code == AURORA_AUTH_OK) { unlock(); return; }   /* the new session is already the active one */
    refused(code);
}
void aurora_lock_session_ended(unsigned uid, bool was_active) {
    (void)uid; if (!was_active) return;
    if (mode == OFF) enter(GREETER, "logout");
    else { mode = GREETER; switching = false; wipe(); message[0] = 0; load_users(true); aurora_host_lock_redraw(); }
}
void aurora_lock_switch_user(void) {
    if (mode == GREETER) return;
    bool was = mode != OFF; enter(GREETER, "switch-user"); switching = was || aurora_session_running(aurora_session_active_uid());
}
void aurora_lock_key(unsigned key, const char* text) {
    if (mode == OFF) return;
    if (key == KEY_ENTER || key == KEY_KPENTER) { submit(); return; }
    if (key == KEY_ESC) {   /* clear; an empty field in a Switch User greeter goes back to the lock screen */
        if (mode == GREETER && switching && !typed_len && !checking && !signing_in) { enter(LOCKED, "switch-cancel"); return; }
        wipe(); message[0] = 0; aurora_host_lock_redraw(); return; }
    if (key == KEY_TAB && mode == LOCKED && !checking) { aurora_lock_switch_user(); return; }   /* Switch User from the lock screen */
    if (key == KEY_TAB && mode == GREETER && user_count > 1) key = KEY_RIGHT;
    if (key == KEY_BACKSPACE) { while (typed_len > 0 && ((unsigned char)typed[--typed_len] & 0xC0) == 0x80) {} typed[typed_len] = 0; aurora_host_lock_redraw(); return; }
    if (mode == GREETER && (key == KEY_LEFT || key == KEY_RIGHT)) reload_if_changed();
    if (mode == GREETER && (key == KEY_LEFT || key == KEY_RIGHT) && user_count > 1) {
        selected = (selected + (key == KEY_RIGHT ? 1 : user_count - 1)) % user_count; wipe(); message[0] = 0; aurora_host_lock_redraw();
        printf("[64] AURORA GREETER SELECT user=%s signed_in=%d\n", users[selected].name, aurora_session_running(users[selected].uid)); fflush(stdout); return; }
    size_t n = text ? strlen(text) : 0;
    if (n && (unsigned char)text[0] >= 32 && text[0] != 127 && typed_len + (int)n < (int)sizeof(typed) - 1) {
        memcpy(typed + typed_len, text, n); typed_len += (int)n; typed[typed_len] = 0; if (message[0] && !checking) message[0] = 0; aurora_host_lock_redraw(); }
}
/* Primary display layout (logical, display-relative); the greeter's account row sits above the password field. */
static int layout_w, layout_h, layout_x, layout_y;
void aurora_lock_pointer(int x, int y) {
    if (mode != GREETER || user_count < 2 || !layout_w) return;
    int cx = layout_x + layout_w / 2, row = layout_y + layout_h * 50 / 100, step = 120, first = cx - (user_count - 1) * step / 2;
    for (int i = 0; i < user_count; i++) if (abs(x - (first + i * step)) < 50 && abs(y - row) < 60) { selected = i; wipe(); message[0] = 0; aurora_host_lock_redraw(); }
}
void aurora_lock_draw(int dx, int dy, int dw, int dh, bool primary) {
    if (mode == OFF) return;
    ui_blend_rect(0, 0, dw, dh, 0x06070C, 150);
    time_t t = time(NULL); struct tm local; localtime_r(&t, &local);
    char clock[16], date[64]; strftime(clock, sizeof(clock), "%H:%M", &local); strftime(date, sizeof(date), "%A %d %B", &local);
    ui_text_center(dw / 2, dh * 10 / 100, clock, UF_HUGE, 0xF4F4F8);
    ui_text_center(dw / 2, dh * 10 / 100 + ui_font_height(UF_HUGE) + 6, date, UF_TITLE, 0xE0E0E8);
    if (!primary) return;
    layout_x = dx; layout_y = dy; layout_w = dw; layout_h = dh;
    int cx = dw / 2, row = dh * 50 / 100;
    if (mode == GREETER && user_count > 1) {
        int step = 120, first = cx - (user_count - 1) * step / 2;
        for (int i = 0; i < user_count; i++) {
            int ax = first + i * step;
            if (i == selected) ui_circle(ax, row, 46, 0x5A9CFF, 200);
            ui_avatar(ax, row, i == selected ? 42 : 36);
            ui_text_center(ax, row + 50, users[i].gecos[0] ? users[i].gecos : users[i].name, UF_UI, i == selected ? 0xFFFFFF : 0xB8B8C4);
            if (aurora_session_running(users[i].uid)) ui_text_center(ax, row + 50 + ui_font_height(UF_UI) + 2, "signed in", UF_SMALL, 0x9FD39F);
        }
    } else {
        const struct aurora_user* u = current();
        ui_avatar(cx, row, 48);
        ui_text_center(cx, row + 58, u ? (u->gecos[0] ? u->gecos : u->name) : "", UF_TITLE, 0xFFFFFF);
    }
    int fw = 280, fh = 36, fx = cx - fw / 2, fy = row + 100;
    ui_rrect(fx, fy, fw, fh, fh / 2, 0xFFFFFF, 46); ui_rrect_stroke(fx, fy, fw, fh, fh / 2, 0xFFFFFF, 90);
    int chars = 0; for (int i = 0; i < typed_len; i++) if (((unsigned char)typed[i] & 0xC0) != 0x80) chars++;
    if (!chars) ui_text(fx + 18, fy + (fh - ui_font_height(UF_UI)) / 2, checking || signing_in ? "Checking..." : "Enter Password", UF_UI, 0xC8C8D2);
    for (int i = 0; i < chars && i < 22; i++) ui_circle(fx + 22 + i * 11, fy + fh / 2, 4, 0xFFFFFF, 255);
    if (message[0]) ui_text_center(cx, fy + fh + 14, message, UF_UI, strncmp(message, "Checking", 8) ? 0xFF8A80 : 0xE0E0E8);
    ui_text_center(cx, dh - 40, mode == GREETER ? (switching ? "NexusOS - Esc: back" : "NexusOS") : "Locked - Tab: Switch User", UF_SMALL, 0xA0A0AC);
}
void aurora_lock_activity(void) { last_activity = now_ms(); }
void aurora_lock_sak(const char* text) { if (mode != OFF) say(text); }   /* aurora_sak.h */
/* The screen reader's view (aurora_reader.c): never the typed text or its length. */
int aurora_lock_a11y(int* count, int* chosen, const char** status) {
    *count = mode == GREETER ? user_count : 0; *chosen = selected; *status = checking || signing_in ? "Checking..." : message;
    return mode;
}
const char* aurora_lock_a11y_user(int i, bool* signed_in) {
    const struct aurora_user* u = mode == GREETER ? (i >= 0 && i < user_count ? &users[i] : NULL) : current();
    *signed_in = u && aurora_session_running(u->uid); return u ? (u->gecos[0] ? u->gecos : u->name) : "";
}
/* (wl_event_loop timers need timerfd, which NexusOS does not have: the desktop's main loop calls this instead.) */
void aurora_lock_tick(void) {
    uint64_t now = now_ms(); if (now - last_tick < 5000) return;
    last_tick = now;
    if (mode == OFF && conf.lock_after > 0 && now - last_activity >= (uint64_t)conf.lock_after * 1000 && session_has_password()) {
        if (aurora_session_idle_inhibited()) last_activity = now;   /* an idle inhibitor (video, presentation): no idle lock */
        else aurora_lock_now("idle");
    }
    if (mode != OFF) aurora_host_lock_redraw();   /* the clock */
    if (mode == GREETER) reload_if_changed();
    if (mode == OFF && now - password_read >= 30000) { aurora_login_conf(&conf); refresh_password(); }
}
bool aurora_session_prepare_sleep(void) {   /* no file access here (above) */
    uint64_t began = now_ms();
    if (mode == OFF && conf.lock_on_sleep && cached_password()) aurora_lock_now("sleep");
    if (mode != OFF) aurora_host_present_now();
    printf("[64] AURORA PREPARE SLEEP locked=%d in %u ms\n", mode != OFF, (unsigned)(now_ms() - began)); fflush(stdout);
    return mode != OFF;
}
void aurora_session_resumed(void) { last_activity = now_ms(); printf("[64] AURORA RESUMED locked=%d\n", mode != OFF); fflush(stdout); if (mode != OFF) aurora_host_lock_redraw(); }
/* aurora_power.c (weak hooks there): the kernel announced a sleep (PM_EVENT=*-prepare) / the resume (post-*). */
void aurora_lock_for_sleep(void) { (void)aurora_session_prepare_sleep(); }
void aurora_lock_after_sleep(void) { aurora_session_resumed(); }
/* control socket: lock, lock-state, prepare-sleep, resumed */
static bool control_lock(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; aurora_lock_now("control"); return true;
}
static bool control_state(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size;
    aurora_control_print(reply, "mode=%s user=%s password=%d autologin=%d lock_after=%d lock_on_sleep=%d\n", mode == OFF ? "unlocked" : mode == GREETER ? "greeter" : "locked",
                         current() ? current()->name : "-", session_has_password(), conf.autologin, conf.lock_after, conf.lock_on_sleep);
    return true;
}
static bool control_sleep(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; aurora_control_print(reply, "locked=%d\n", aurora_session_prepare_sleep()); return true;
}
static bool control_resumed(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; aurora_session_resumed(); return true;
}
static bool control_reload(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; aurora_login_conf(&conf); refresh_password(); return true;
}
void aurora_lock_init(struct wl_event_loop* loop) {
    (void)loop; aurora_login_conf(&conf); refresh_password(); last_activity = now_ms();
    aurora_control_register("lock", false, control_lock);
    aurora_control_register("lock-state", false, control_state);
    aurora_control_register("prepare-sleep", false, control_sleep);
    aurora_control_register("resumed", false, control_resumed);
    aurora_control_register("reload-login", false, control_reload);
    if (!conf.autologin && session_has_password()) { aurora_session_boot_greeter(); enter(GREETER, "boot"); }
}
