/* Screen reader support of the Aurora shell (aurora_reader.h): keyboard access to the menu bar and the Dock, the
 * shell's state for aurora-a11y-bridge, and the screen reader process of each session. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include "aurora_reader.h"
#include "aurora_control.h"
#include "aurora_lock.h"
#include "aurora_session.h"
#include "aurora_wm.h"
#include "../../kernel/macui.h"
#include "../../kernel/menubar.h"
#include "../../kernel/dock.h"
#include "../../kernel/spotlight.h"

#define WATCHERS 8
#define READERS 8
#define GRAB_MODIFIERS 8
#define GRAB_KEYS 256
/* sent: hash of the last shell record. The keyboard monitor (aurora_reader.h): keys = the watcher wants key events,
 * grab_all, the screen reader's modifier keys (keysyms) and keystrokes (keysym + X modifier mask) it takes, how many
 * of its modifier keys are held now, and the keys whose press it took (their release is taken too). */
struct watcher {
    int fd; unsigned uid; uint64_t sent; bool was_active; struct wl_event_source* source;
    bool trusted, keys, grab_all; int modifier_count, stroke_count, held;   /* trusted: the bridge Aurora started (keys) */ uint32_t modifiers[GRAB_MODIFIERS]; uint32_t strokes[GRAB_KEYS][2];
    uint8_t taken[KEY_MAX / 8 + 1];
};
static struct wl_event_loop* loop;
struct reader { unsigned uid; pid_t pid; };
static struct watcher watchers[WATCHERS];
static struct reader readers[READERS];
static struct reader* reader_of(unsigned uid) { for (int i = 0; i < READERS; i++) if (readers[i].pid > 0 && readers[i].uid == uid) return &readers[i]; return NULL; }
static char record[AURORA_READER_RECORD]; static size_t used; static bool overflow;

static uint64_t hash(const char* text, size_t size) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < size; i++) { h ^= (unsigned char)text[i]; h *= 1099511628211ULL; } return h; }
static void put(const char* text) {   /* tabs and line breaks of names become spaces */
    for (; *text; text++) { if (used + 2 >= sizeof(record)) { overflow = true; return; } record[used++] = *text == '\t' || *text == '\n' || *text == '\r' ? ' ' : *text; }
}
/* text: an entry's content (the Text interface); never given for a password field */
static void node_text(const char* path, const char* parent, const char* role, const char* states, const char* name, const char* description, const char* text) {
    const char* fields[7] = {path, parent, role, states, name ? name : "", description ? description : "", text};
    if (used + 8 >= sizeof(record)) { overflow = true; return; }
    memcpy(record + used, "node", 4); used += 4;
    for (int i = 0; i < (text ? 7 : 6); i++) { record[used++] = '\t'; put(fields[i]); if (used + 4 >= sizeof(record)) { overflow = true; return; } }
    record[used++] = '\n';
}
static void node(const char* path, const char* parent, const char* role, const char* states, const char* name, const char* description) { node_text(path, parent, role, states, name, description, NULL); }
static void focus(const char* path) {
    char line[160]; snprintf(line, sizeof(line), "focus %s\n", path); size_t n = strlen(line);
    if (used + n < sizeof(record)) { memcpy(record + used, line, n); used += n; } else overflow = true;
}
static const char* strip_dots(const char* label, char* out, size_t size) {   /* "Log Out..." is read as "Log Out" */
    snprintf(out, size, "%s", label); size_t n = strlen(out); while (n && out[n - 1] == '.') out[--n] = 0; return out;
}

/* The shell's state now: one window of it (or none) and the focused element. */
static void describe(void) {
    used = 0; overflow = false; memcpy(record, "shell\n", 6); used = 6;
    int count, chosen; const char* status; int lock = aurora_lock_a11y(&count, &chosen, &status);
    char name[160], label[96];
    if (lock == 1) {
        bool signed_in; const char* user = aurora_lock_a11y_user(0, &signed_in);
        focus("lock/password");
        node("lock", "", "dialog", "active,modal", "Screen locked", "Type the password and press Enter. Tab: Switch User");
        snprintf(name, sizeof(name), "Password for %s", user); node("lock/password", "lock", "password_text", "focused,focusable,editable", name, status);
        return;
    }
    if (lock == 2) {
        char path[32]; snprintf(path, sizeof(path), "login/accounts/%d", chosen); focus(count > 1 ? path : "login/password");
        node("login", "", "dialog", "active,modal", "Log in", status[0] ? status : "Choose an account with Left and Right, type its password, press Enter");
        node("login/accounts", "login", "list", "", "Accounts", "");
        for (int i = 0; i < count && i < 16; i++) {
            bool signed_in; const char* user = aurora_lock_a11y_user(i, &signed_in); snprintf(path, sizeof(path), "login/accounts/%d", i);
            node(path, "login/accounts", "list_item", i == chosen ? (count > 1 ? "focused,focusable,selectable,selected" : "focusable,selectable,selected") : "focusable,selectable", user, signed_in ? "signed in" : "");
        }
        node("login/password", "login", "password_text", count > 1 ? "focusable,editable" : "focused,focusable,editable", "Password", status);
        return;
    }
    int results, selected; const char* query = spotlight_a11y(&results, &selected);
    if (query) {
        char path[32]; snprintf(path, sizeof(path), "spotlight/results/%d", selected); focus(selected >= 0 ? path : "spotlight/search");
        node("spotlight", "", "dialog", "active", "Spotlight Search", "Type to search applications and actions, Enter opens, Esc closes");
        node_text("spotlight/search", "spotlight", "entry", selected >= 0 ? "focusable,editable,single_line" : "focused,focusable,editable,single_line", "Search", "", query);
        node("spotlight/results", "spotlight", "list", "", "Results", "");
        for (int i = 0; i < results; i++) {
            const char* kind; const char* result = spotlight_a11y_result(i, &kind); snprintf(path, sizeof(path), "spotlight/results/%d", i);
            node(path, "spotlight/results", "list_item", i == selected ? "focused,focusable,selectable,selected" : "focusable,selectable", result, kind);
        }
        return;
    }
    int apps, app; const char* chosen_app = launchpad_a11y(&apps, &app);
    if (chosen_app) {
        focus("launchpad/apps/0"); snprintf(name, sizeof(name), "%d of %d", app + 1, apps);
        node("launchpad", "", "frame", "active", "Applications", "Arrows choose, Enter opens, Esc closes");
        node("launchpad/apps", "launchpad", "list", "", "Applications", "");
        node("launchpad/apps/0", "launchpad/apps", "list_item", "focused,focusable,selectable,selected", chosen_app, name);
        return;
    }
    int item, menu = menubar_a11y_state(&item);
    if (menu >= 0) {
        char path[48], parent[32]; snprintf(path, sizeof(path), item >= 0 ? "menubar/%d/%d" : "menubar/%d", menu, item); focus(path);
        node("menubar", "", "frame", "active", "Menu bar", "Left and Right: menus, Up and Down: items, Enter chooses, Esc closes");
        node("menubar/bar", "menubar", "menu_bar", "", "Menu bar", "");
        for (int m = 0; m < menubar_a11y_menus(); m++) {
            snprintf(path, sizeof(path), "menubar/%d", m);
            node(path, "menubar/bar", "menu", m == menu ? (item < 0 ? "focused,focusable,selectable,selected,expanded,expandable" : "focusable,selectable,selected,expanded,expandable") : "focusable,selectable,expandable,collapsed", menubar_a11y_title(m), "");
        }
        snprintf(parent, sizeof(parent), "menubar/%d", menu);
        for (int i = 0; i < menubar_a11y_items(menu); i++) {
            bool enabled = false; const char* text = menubar_a11y_item(menu, i, &enabled); snprintf(path, sizeof(path), "menubar/%d/%d", menu, i);
            if (!text) { node(path, parent, "separator", "", "", ""); continue; }
            char states[64]; snprintf(states, sizeof(states), "%s%s", i == item ? "focused,selected," : "", enabled ? "focusable,selectable" : "disabled");
            node(path, parent, "menu_item", states, strip_dots(text, label, sizeof(label)), "");
        }
        return;
    }
    int at = dock_a11y_focus();
    if (at >= 0) {
        char path[32]; snprintf(path, sizeof(path), "dock/items/%d", at); focus(path);
        node("dock", "", "frame", "active", "Dock", "Left and Right choose, Enter opens, Esc leaves");
        node("dock/items", "dock", "tool_bar", "", "Dock", "");
        for (int i = 0; i < dock_a11y_count(); i++) {
            bool running; const char* item_name = dock_a11y_name(i, &running); snprintf(path, sizeof(path), "dock/items/%d", i);
            node(path, "dock/items", "push_button", i == at ? "focused,focusable" : "focusable", item_name, running ? "running" : "");
        }
        return;
    }
    focus("-");
}
static void drop(struct watcher* w) {
    if (w->source) wl_event_source_remove(w->source);
    if (w->fd >= 0) close(w->fd);
    memset(w, 0, sizeof(*w)); w->fd = -1;
}
static bool deliver(struct watcher* w, const char* data, size_t size) {
    if (send(w->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)size) return true;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) { printf("[64] AURORA READER WATCHER GONE uid=%u\n", w->uid); fflush(stdout); drop(w); }
    return false;
}
static bool receives(const struct watcher* w) { return w->fd >= 0 && (!w->uid || w->uid == aurora_session_active_uid()); }
/* ---- the setting, kept across logins: READER_CONF, one "UID 0|1" line per user (Aurora's own state, next to
 * login.conf; the switches of the user - Super+Alt+S, System Settings, set-screen-reader - change it) ---- */
#define READER_CONF "/home/nexus/.aurora/screen-reader.conf"
static int saved_setting(unsigned uid) {   /* 1 on, 0 off, -1 never set */
    FILE* f = fopen(READER_CONF, "re"); if (!f) return -1;
    unsigned u; int on, result = -1; while (fscanf(f, "%u %d", &u, &on) == 2) if (u == uid) result = on != 0;
    fclose(f); return result;
}
static void save_setting(unsigned uid, bool on) {
    if (saved_setting(uid) == (int)on) return;
    char lines[2048]; size_t n = 0; FILE* f = fopen(READER_CONF, "re");
    if (f) { unsigned u; int v; while (fscanf(f, "%u %d", &u, &v) == 2 && n + 32 < sizeof(lines)) if (u != uid) n += (size_t)snprintf(lines + n, sizeof(lines) - n, "%u %d\n", u, v != 0); fclose(f); }
    n += (size_t)snprintf(lines + n, sizeof(lines) - n, "%u %d\n", uid, on);
    mkdir("/home/nexus/.aurora", 0755);
    FILE* out = fopen(READER_CONF ".tmp", "we"); bool ok = out && fwrite(lines, 1, n, out) == n;
    if (out && fclose(out)) ok = false;
    if (!ok || rename(READER_CONF ".tmp", READER_CONF)) { unlink(READER_CONF ".tmp"); printf("[64] AURORA READER SAVE FAILED uid=%u\n", uid); fflush(stdout); return; }
    printf("[64] AURORA READER SAVED uid=%u screen_reader=%d\n", uid, on); fflush(stdout);
}
static bool start(bool debug);
static unsigned shown = ~0u;   /* the session autostart() last saw on screen (~0: none since a log-out) */
static void autostart(void) {   /* a session comes on screen (unlocked): its screen reader if the user left it on */
    unsigned uid = aurora_session_active_uid();
    if (uid == shown || aurora_lock_active()) return;
    shown = uid;
    if (!uid || reader_of(uid) || saved_setting(uid) != 1) return;
    printf("[64] AURORA READER AUTOSTART uid=%u\n", uid); fflush(stdout); start(false);
}
void aurora_reader_tick(void) {
    autostart();
    bool any = false; for (int i = 0; i < WATCHERS; i++) if (watchers[i].fd >= 0) any = true;
    if (!any) return;
    bool described = false; uint64_t h = 0;
    for (int i = 0; i < WATCHERS; i++) {
        struct watcher* w = &watchers[i]; if (w->fd < 0) continue;
        if (!receives(w)) {   /* its session left the screen: the shell is not its to read */
            if (w->was_active) { static const char gone[] = "shell\nfocus -\n"; if (deliver(w, gone, sizeof(gone) - 1)) { w->was_active = false; w->sent = 0; } }
            continue;
        }
        if (!described) { describe(); described = true; h = hash(record, used); if (overflow) { printf("[64] AURORA READER RECORD TOO LONG\n"); fflush(stdout); } }
        if (h != w->sent && deliver(w, record, used)) { w->sent = h; w->was_active = true; }
    }
}
void aurora_reader_notice(const char* text) {
    char line[600]; snprintf(line, sizeof(line), "notice\t%s", text ? text : ""); for (char* p = line + 7; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
    size_t n = strlen(line); line[n++] = '\n';
    for (int i = 0; i < WATCHERS; i++) if (receives(&watchers[i])) deliver(&watchers[i], line, n);
}

/* ---- the screen reader processes (aurora-a11y-bridge --orca), one per session user ---- */
static void stop(unsigned uid) {
    struct reader* r = reader_of(uid); if (!r) return;
    for (int i = 0; i < WATCHERS; i++) if (watchers[i].fd >= 0 && watchers[i].uid == uid) deliver(&watchers[i], "quit\n", 5);   /* any user: it exits */
    if (uid == getuid()) kill(r->pid, SIGTERM);   /* our own child (another user's is its agent's: it exits on "quit") */
    printf("[64] AURORA READER STOP uid=%u pid=%ld\n", uid, (long)r->pid); fflush(stdout);
    r->pid = -r->pid;   /* stopping: forgotten when reaped */
}
static bool start(bool debug) {   /* debug: the bridge traces, Orca writes $XDG_RUNTIME_DIR/orca-debug.txt */
    unsigned uid = aurora_session_active_uid(); if (reader_of(uid)) return true;
    struct reader* r = NULL; for (int i = 0; i < READERS; i++) if (readers[i].pid == 0) { r = &readers[i]; break; }
    if (!r) return false;
    char debug_file[64]; snprintf(debug_file, sizeof(debug_file), "--debug-file=/run/user/%u/orca-debug.txt", uid);
    char* plain[] = {(char*)AURORA_READER_PROGRAM, (char*)"--orca", NULL};
    char* traced[] = {(char*)AURORA_READER_PROGRAM, (char*)"--orca", (char*)"--trace", (char*)"--", debug_file, NULL};
    char** command = debug ? traced : plain;
    pid_t pid = aurora_session_foreign() ? aurora_session_run(command, "screen-reader") : fork();
    if (!pid) { execv(command[0], command); _exit(127); }
    if (pid < 0) { printf("[64] AURORA READER START FAILED uid=%u\n", uid); fflush(stdout); return false; }
    r->uid = uid; r->pid = pid; printf("[64] AURORA READER START uid=%u pid=%ld\n", uid, (long)pid); fflush(stdout);
    return true;
}
bool aurora_reader_reaped(pid_t pid) {
    for (int i = 0; i < READERS; i++) if (pid > 0 && (readers[i].pid == pid || readers[i].pid == -pid)) {
        printf("[64] AURORA READER EXIT uid=%u pid=%ld\n", readers[i].uid, (long)pid); fflush(stdout);
        readers[i].pid = 0; return true;
    }
    return false;
}
void aurora_reader_session_end(unsigned uid) {
    if (uid == shown) shown = ~0u;   /* its next login starts the screen reader again */
    stop(uid); for (int i = 0; i < WATCHERS; i++) if (watchers[i].fd >= 0 && watchers[i].uid == uid) drop(&watchers[i]);
    if (uid != getuid()) for (int i = 0; i < READERS; i++) if (readers[i].pid && readers[i].uid == uid) readers[i].pid = 0;   /* its agent ends it */
}
static void toggle(void) {
    unsigned uid = aurora_session_active_uid();
    if (reader_of(uid)) { stop(uid); save_setting(uid, false); aurora_shell_notice("Screen reader off"); }
    else if (start(false)) { save_setting(uid, true); aurora_shell_notice("Screen reader on"); }
    else aurora_shell_notice("The screen reader could not be started");
}

/* ---- keyboard access ---- */
bool aurora_reader_dock_focus(void) { return dock_a11y_focus() >= 0; }
bool aurora_reader_key(unsigned key, int value, unsigned mods, int* action) {
    *action = 0; if (!value) return false;
    if (value == 1 && key == KEY_S && mods == (AURORA_MOD_SUPER | AURORA_MOD_ALT)) { toggle(); return true; }
    if (value == 1 && key == KEY_F2 && mods == AURORA_MOD_CTRL) {   /* the menu bar */
        if (dock_a11y_focus() >= 0) dock_key(27);
        menubar_open_first(); printf("[64] AURORA KEYNAV menubar\n"); fflush(stdout); return true;
    }
    if (value == 1 && key == KEY_F3 && mods == AURORA_MOD_CTRL) {   /* the Dock */
        menubar_close(); dock_key(0); printf("[64] AURORA KEYNAV dock focus=%d\n", dock_a11y_focus()); fflush(stdout); return true;
    }
    if (dock_a11y_focus() < 0) return false;
    char k = key == KEY_LEFT ? (char)0x82 : key == KEY_RIGHT || key == KEY_TAB ? (char)0x83 : key == KEY_HOME ? (char)0x9D : key == KEY_END ? (char)0x9E :
             key == KEY_ENTER || key == KEY_KPENTER ? '\n' : key == KEY_SPACE ? ' ' : 0;
    if (key == KEY_TAB && (mods & AURORA_MOD_SHIFT)) k = (char)0x82;
    if (!k) { if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) return true; k = 27; }   /* Esc (or any other key) leaves the Dock */
    *action = dock_key(k);
    printf("[64] AURORA KEYNAV dock focus=%d action=%d\n", dock_a11y_focus(), *action); fflush(stdout);
    return true;
}

/* ---- the keyboard monitor: requests of the bridge on its watch descriptor ----
 *   "keys 0|1"            key events for org.freedesktop.a11y.KeyboardMonitor (WatchKeyboard / UnwatchKeyboard)
 *   "grab-all 0|1"        every key goes to the screen reader only (GrabKeyboard / UngrabKeyboard)
 *   "grabs M,M.. K:S,K:S.."  its modifier keysyms and keystrokes (keysym:X modifier mask), "-" for none (SetKeyGrabs)
 * Aurora answers with "key\tPRESSED\tSTATE\tKEYSYM\tUNICHAR\tKEYCODE\n" for every key while its session is on
 * screen and nothing is locked (the greeter's and the lock screen's keys never leave Aurora). */
static void request(struct watcher* w, char* line) {
    if (!strncmp(line, "keys ", 5) && !w->trusted) { printf("[64] AURORA READER KEYS REFUSED uid=%u (not the screen reader Aurora started)\n", w->uid); fflush(stdout); return; }
    if (!strncmp(line, "keys ", 5)) { w->keys = line[5] == '1'; if (!w->keys) { w->grab_all = false; w->held = 0; } printf("[64] AURORA READER KEYS uid=%u watch=%d\n", w->uid, w->keys); fflush(stdout); }
    else if (!strncmp(line, "grab-all ", 9)) w->grab_all = line[9] == '1';
    else if (!strncmp(line, "grabs ", 6)) {
        char* modifiers = line + 6; char* strokes = strchr(modifiers, ' '); if (!strokes) return;
        *strokes++ = 0; w->modifier_count = w->stroke_count = 0;
        for (char* save = NULL, *m = strtok_r(modifiers, ",", &save); m && strcmp(m, "-") && w->modifier_count < GRAB_MODIFIERS; m = strtok_r(NULL, ",", &save))
            w->modifiers[w->modifier_count++] = (uint32_t)strtoul(m, NULL, 0);
        for (char* save = NULL, *k = strtok_r(strokes, ",", &save); k && strcmp(k, "-") && w->stroke_count < GRAB_KEYS; k = strtok_r(NULL, ",", &save)) {
            char* colon = strchr(k, ':'); if (!colon) continue;
            w->strokes[w->stroke_count][0] = (uint32_t)strtoul(k, NULL, 0); w->strokes[w->stroke_count][1] = (uint32_t)strtoul(colon + 1, NULL, 0); w->stroke_count++;
        }
        printf("[64] AURORA READER GRABS uid=%u modifiers=%d keystrokes=%d\n", w->uid, w->modifier_count, w->stroke_count); fflush(stdout);
    }
}
static int readable(int fd, uint32_t mask, void* data) {
    struct watcher* w = data; (void)fd;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { printf("[64] AURORA READER WATCHER GONE uid=%u\n", w->uid); fflush(stdout); drop(w); return 0; }
    char buffer[8192]; ssize_t n;
    while ((n = recv(w->fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT)) > 0) {
        buffer[n] = 0;
        for (char* save = NULL, *line = strtok_r(buffer, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) request(w, line);
    }
    if (n == 0) { printf("[64] AURORA READER WATCHER GONE uid=%u\n", w->uid); fflush(stdout); drop(w); }
    return 0;
}
static bool modifier_key(const struct watcher* w, uint32_t keysym) { for (int i = 0; i < w->modifier_count; i++) if (w->modifiers[i] == keysym) return true; return false; }
bool aurora_reader_key_event(unsigned key, bool pressed, uint32_t keysym, uint32_t unichar, uint32_t state) {
    if (key > KEY_MAX || aurora_lock_active()) return false;
    struct watcher* w = NULL; for (int i = 0; i < WATCHERS; i++) if (receives(&watchers[i]) && watchers[i].keys) { w = &watchers[i]; break; }
    if (!w) return false;
    uint8_t bit = (uint8_t)(1U << (key % 8)); bool taken, modifier = modifier_key(w, keysym);
    if (pressed) {
        taken = w->grab_all || modifier || w->held > 0;
        for (int i = 0; !taken && i < w->stroke_count; i++) {   /* Shift, Control, Alt (Mod1), Super (Mod4); virtual bits need a held modifier key */
            uint32_t want = w->strokes[i][1];
            if (w->strokes[i][0] == keysym && (state & 0x4D) == (want & 0x4D) && (!(want & ~0xFFu) || w->held)) taken = true;
        }
        if (modifier) w->held++;
        if (taken) w->taken[key / 8] |= bit; else w->taken[key / 8] &= (uint8_t)~bit;
    } else {
        if (modifier && w->held > 0) w->held--;
        taken = (w->taken[key / 8] & bit) != 0; w->taken[key / 8] &= (uint8_t)~bit;
    }
    char line[96]; int n = snprintf(line, sizeof(line), "key\t%d\t%u\t%u\t%u\t%u\n", pressed, state, keysym, unichar, key + 8);
    deliver(w, line, (size_t)n);
    return taken;
}

/* ---- control commands ---- */
static bool control_watch(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; int fd = aurora_control_peer_fd(), type = 0; socklen_t length = sizeof(type);
    if (fd < 0) { snprintf(error, size, "a11y-watch needs a SOCK_SEQPACKET descriptor"); return false; }
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_SEQPACKET) { close(fd); snprintf(error, size, "a11y-watch needs a SOCK_SEQPACKET descriptor"); return false; }
    unsigned uid = (unsigned)aurora_control_peer_uid(); struct watcher* w = NULL;
    for (int i = 0; i < WATCHERS; i++) if (watchers[i].fd >= 0 && watchers[i].uid == uid) { w = &watchers[i]; drop(w); break; }   /* one per user: the newest */
    for (int i = 0; !w && i < WATCHERS; i++) if (watchers[i].fd < 0) w = &watchers[i];
    if (!w) { close(fd); snprintf(error, size, "too many screen reader watchers"); return false; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(w, 0, sizeof(*w)); w->fd = fd; w->uid = uid;
    for (int i = 0; i < READERS; i++) if (readers[i].pid > 0 && readers[i].uid == uid && readers[i].pid == aurora_control_peer_pid()) w->trusted = true;
    w->source = loop ? wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, readable, w) : NULL;
    printf("[64] AURORA READER WATCH uid=%u screen_reader=%d\n", uid, w->trusted); fflush(stdout); return true;
}
static bool control_get(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; unsigned uid = (unsigned)aurora_control_peer_uid();
    unsigned who = uid ? uid : aurora_session_active_uid(); bool keys = false;
    for (int i = 0; i < WATCHERS; i++) if (watchers[i].fd >= 0 && watchers[i].uid == who && watchers[i].keys) keys = true;
    aurora_control_print(reply, "screen_reader=%d keys=%d\n", reader_of(who) != NULL, keys); return true;
}
static bool control_set(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply;
    if (strcmp(args, "0") && strcmp(args, "1") && strcmp(args, "1 debug")) { snprintf(error, size, "usage: set-screen-reader 0|1 [debug]"); return false; }
    unsigned uid = aurora_session_active_uid();   /* the requester is root or the active session's user */
    if (args[0] == '1') { if (!start(args[1] != 0)) { snprintf(error, size, "the screen reader could not be started"); return false; } }
    else stop(uid);
    save_setting(uid, args[0] == '1');   /* kept across logins (autostart) */
    return true;
}
void aurora_reader_init(struct wl_event_loop* event_loop) {
    loop = event_loop; for (int i = 0; i < WATCHERS; i++) watchers[i].fd = -1;
    aurora_control_register("a11y-watch", false, control_watch);
    aurora_control_register("screen-reader", false, control_get);
    aurora_control_register("set-screen-reader", false, control_set);
}
