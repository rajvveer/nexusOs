/* xdg-desktop-portal-aurora: the Aurora backend of xdg-desktop-portal (Debian's frontend, `apt install
 * xdg-desktop-portal`), /usr/libexec/xdg-desktop-portal-aurora, D-Bus activated on the user's session bus as
 * org.freedesktop.impl.portal.desktop.aurora (object /org/freedesktop/portal/desktop). It runs as the session's user.
 *   Settings 2     org.freedesktop.appearance color-scheme / accent-color / contrast / reduced-motion,
 *                  org.gnome.desktop.interface color-scheme / text-scaling-factor / enable-animations,
 *                  org.gnome.desktop.a11y.interface high-contrast: from ~/.config/aurora/appearance.conf (System Settings
 *                  > Appearance) and Aurora's accessibility settings; SettingChanged when they change (polled, 1 s).
 *   FileChooser 4  OpenFile (multiple, directory), SaveFile, SaveFiles: Aurora's file dialog (aurora-app filechooser).
 *   AppChooser 2   ChooseApplication: the last choice, else the frontend's first (default) handler; UpdateChoices.
 *   Access 1       AccessDialog: Aurora's permission prompt (aurora-app prompt), Deny focused.
 *   Screenshot 2   Screenshot: the active session's screen from Aurora (control socket "screenshot" with a memfd), a PNG
 *                  in ~/Pictures; interactive or not yet permitted requests ask first. PickColor: not supported (2).
 *   Inhibit 3      Inhibit: idle and suspend through logind inhibitors (Aurora's idle lock honours them), held until
 *                  the request is closed; logout / user-switch flags are not enforceable here. CreateMonitor: 2.
 *   Notification 1 AddNotification / RemoveNotification through org.freedesktop.Notifications.
 * ScreenCast is not offered: PipeWire needs timerfd/signalfd/POSIX timers (ENOSYS in this kernel; see HANDOFF).
 * Every dialog request exports its Request object (Close ends the dialog: response 2). Malformed calls get
 * InvalidArgs; nothing a client sends is passed to a shell. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.aurora"
#define PATH "/org/freedesktop/portal/desktop"
#define I_SETTINGS "org.freedesktop.impl.portal.Settings"
#define I_FILES "org.freedesktop.impl.portal.FileChooser"
#define I_APPS "org.freedesktop.impl.portal.AppChooser"
#define I_ACCESS "org.freedesktop.impl.portal.Access"
#define I_SHOT "org.freedesktop.impl.portal.Screenshot"
#define I_INHIBIT "org.freedesktop.impl.portal.Inhibit"
#define I_NOTIFY "org.freedesktop.impl.portal.Notification"
#define I_REQUEST "org.freedesktop.impl.portal.Request"
#define E_ARGS "org.freedesktop.DBus.Error.InvalidArgs"
#define MAX_REQUESTS 32
enum { R_FILE_OPEN, R_FILE_SAVE, R_FILE_SAVES, R_ACCESS, R_SHOT_PROMPT, R_INHIBIT };

static DBusConnection *bus, *system_bus;
struct request {
    bool used; int kind; char handle[256]; DBusMessage* call; pid_t pid; int pipe; char* out; size_t out_used;
    char app[256]; int inhibit_fds[2]; char save_files[4096];   /* SaveFiles: the names to put in the chosen folder */
};
static struct request requests[MAX_REQUESTS];
static int child_pipe[2] = {-1, -1};

static void note(const char* format, ...) {
    char line[700]; int n = snprintf(line, sizeof(line), "[64] AURORA PORTAL "); va_list list; va_start(list, format);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, list); va_end(list);
    size_t length = m < 0 ? (size_t)n : strlen(line); line[length++] = '\n'; if (write(2, line, length) < 0) {}
}
static DBusHandlerResult error(DBusMessage* m, const char* name, const char* text) {
    DBusMessage* r = dbus_message_new_error(m, name, text); if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static const char* runtime_dir(void) { const char* r = getenv("XDG_RUNTIME_DIR"); return r && r[0] == '/' ? r : NULL; }

/* ---------------------------------------------------------------- Aurora's control socket (the session's own) */
static bool aurora(const char* request, int fd, char* reply, size_t size) {
    const char* run = runtime_dir(); if (!run) return false;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (s < 0) return false;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "%s/aurora-control", run);
    struct timeval tv = {5, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    bool ok = false; if (reply && size) reply[0] = 0;
    if (!connect(s, (struct sockaddr*)&a, sizeof(a))) {
        struct iovec iov = {(void*)request, strlen(request)}; char control[CMSG_SPACE(sizeof(int))]; memset(control, 0, sizeof(control));
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
        if (fd >= 0) { m.msg_control = control; m.msg_controllen = sizeof(control); struct cmsghdr* c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &fd, sizeof(int)); }
        if (sendmsg(s, &m, MSG_NOSIGNAL) == (ssize_t)iov.iov_len) {
            char buffer[8192]; size_t used = 0; ssize_t n;
            while (used < sizeof(buffer) - 1 && (n = read(s, buffer + used, sizeof(buffer) - 1 - used)) > 0) {
                used += (size_t)n; buffer[used] = 0; char* last = buffer + used - 1; if (*last != '\n') continue;
                char* start = last; while (start > buffer && start[-1] != '\n') start--;
                if (!strncmp(start, "OK", 2)) { ok = true; break; } if (!strncmp(start, "ERROR", 5)) break;
            }
            buffer[used] = 0; if (reply && size) snprintf(reply, size, "%s", buffer);
        }
    }
    close(s); return ok;
}

/* ---------------------------------------------------------------- settings */
struct appearance { unsigned scheme, contrast, motion; double accent[3]; bool has_accent; double text_scale; };
static struct appearance current; static bool have_current;
static void load_appearance(struct appearance* a) {
    memset(a, 0, sizeof(*a)); a->accent[0] = a->accent[1] = a->accent[2] = -1; a->text_scale = 1.0;
    const char* home = getenv("HOME"); char path[512];
    if (home && home[0] == '/' && (size_t)snprintf(path, sizeof(path), "%s/.config/aurora/appearance.conf", home) < sizeof(path)) {
        FILE* f = fopen(path, "re");
        if (f) { char line[128]; unsigned r, g, b;
            while (fgets(line, sizeof(line), f)) {
                if (!strncmp(line, "color_scheme=dark", 17)) a->scheme = 1; else if (!strncmp(line, "color_scheme=light", 18)) a->scheme = 2;
                else if (sscanf(line, "accent=#%02x%02x%02x", &r, &g, &b) == 3) { a->accent[0] = r / 255.0; a->accent[1] = g / 255.0; a->accent[2] = b / 255.0; a->has_accent = true; }
            }
            fclose(f); }
    }
    char reply[512]; int contrast = 0, larger = 0, motion = 0;
    if (aurora("a11y\n", -1, reply, sizeof(reply))) {
        char* p = strstr(reply, "contrast="); if (p) contrast = atoi(p + 9);
        p = strstr(reply, "larger_text="); if (p) larger = atoi(p + 12);
        p = strstr(reply, "reduce_motion="); if (p) motion = atoi(p + 14);
    } else if (have_current) { a->contrast = current.contrast; a->motion = current.motion; a->text_scale = current.text_scale; return; }   /* not the active session now */
    a->contrast = contrast != 0; a->motion = motion != 0; a->text_scale = larger ? 1.25 : 1.0;
}
static void append_value(DBusMessageIter* it, const char* ns, const char* key, const struct appearance* a) {
    DBusMessageIter v, st;
    if (!strcmp(ns, "org.freedesktop.appearance")) {
        if (!strcmp(key, "accent-color")) {
            dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "(ddd)", &v); dbus_message_iter_open_container(&v, DBUS_TYPE_STRUCT, NULL, &st);
            for (int i = 0; i < 3; i++) { double d = a->has_accent ? a->accent[i] : -1; dbus_message_iter_append_basic(&st, DBUS_TYPE_DOUBLE, &d); }
            dbus_message_iter_close_container(&v, &st); dbus_message_iter_close_container(it, &v); return;
        }
        dbus_uint32_t u = !strcmp(key, "color-scheme") ? a->scheme : !strcmp(key, "contrast") ? a->contrast : a->motion;
        dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "u", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &u); dbus_message_iter_close_container(it, &v); return;
    }
    if (!strcmp(key, "color-scheme")) { const char* s = a->scheme == 1 ? "prefer-dark" : a->scheme == 2 ? "prefer-light" : "default";
        dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s); dbus_message_iter_close_container(it, &v); return; }
    if (!strcmp(key, "text-scaling-factor")) { double d = a->text_scale; dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "d", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &d); dbus_message_iter_close_container(it, &v); return; }
    dbus_bool_t b = !strcmp(key, "enable-animations") ? !a->motion : a->contrast;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "b", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b); dbus_message_iter_close_container(it, &v);
}
static const char* const SETTINGS[][2] = {{"org.freedesktop.appearance", "color-scheme"}, {"org.freedesktop.appearance", "accent-color"},
    {"org.freedesktop.appearance", "contrast"}, {"org.freedesktop.appearance", "reduced-motion"}, {"org.gnome.desktop.interface", "color-scheme"},
    {"org.gnome.desktop.interface", "text-scaling-factor"}, {"org.gnome.desktop.interface", "enable-animations"}, {"org.gnome.desktop.a11y.interface", "high-contrast"}};
#define NSETTINGS (sizeof(SETTINGS) / sizeof(*SETTINGS))
static bool namespace_match(const char* pattern, const char* ns) {   /* "a.b" exact or "a.*" prefix */
    size_t n = strlen(pattern); if (n && pattern[n - 1] == '*') return !strncmp(pattern, ns, n - 1);
    return !strcmp(pattern, ns);
}
static DBusHandlerResult settings_read_all(DBusMessage* m) {
    DBusMessageIter in, arr; const char* patterns[64]; int np = 0;
    if (strcmp(dbus_message_get_signature(m), "as") || !dbus_message_iter_init(m, &in)) return error(m, E_ARGS, "ReadAll(as)");
    dbus_message_iter_recurse(&in, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING && np < 64) { dbus_message_iter_get_basic(&arr, &patterns[np]); np++; dbus_message_iter_next(&arr); }
    struct appearance a; load_appearance(&a); current = a; have_current = true;
    DBusMessage* r = dbus_message_new_method_return(m); DBusMessageIter it, outer, nsentry, inner, entry; dbus_message_iter_init_append(r, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sa{sv}}", &outer);
    static const char* const NS[] = {"org.freedesktop.appearance", "org.gnome.desktop.interface", "org.gnome.desktop.a11y.interface"};
    for (unsigned k = 0; k < 3; k++) {
        bool wanted = !np; for (int i = 0; i < np; i++) if (namespace_match(patterns[i], NS[k])) wanted = true;
        if (!wanted) continue;
        dbus_message_iter_open_container(&outer, DBUS_TYPE_DICT_ENTRY, NULL, &nsentry); dbus_message_iter_append_basic(&nsentry, DBUS_TYPE_STRING, &NS[k]);
        dbus_message_iter_open_container(&nsentry, DBUS_TYPE_ARRAY, "{sv}", &inner);
        for (unsigned s = 0; s < NSETTINGS; s++) if (!strcmp(SETTINGS[s][0], NS[k])) {
            dbus_message_iter_open_container(&inner, DBUS_TYPE_DICT_ENTRY, NULL, &entry); dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &SETTINGS[s][1]);
            append_value(&entry, NS[k], SETTINGS[s][1], &a); dbus_message_iter_close_container(&inner, &entry);
        }
        dbus_message_iter_close_container(&nsentry, &inner); dbus_message_iter_close_container(&outer, &nsentry);
    }
    dbus_message_iter_close_container(&it, &outer); dbus_connection_send(bus, r, NULL); dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult settings_read(DBusMessage* m) {
    const char *ns, *key; DBusError e; dbus_error_init(&e);
    if (strcmp(dbus_message_get_signature(m), "ss") || !dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID)) { dbus_error_free(&e); return error(m, E_ARGS, "Read(ss)"); }
    for (unsigned s = 0; s < NSETTINGS; s++) if (!strcmp(SETTINGS[s][0], ns) && !strcmp(SETTINGS[s][1], key)) {
        struct appearance a; load_appearance(&a); current = a; have_current = true;
        DBusMessage* r = dbus_message_new_method_return(m); DBusMessageIter it; dbus_message_iter_init_append(r, &it);
        append_value(&it, ns, key, &a); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    return error(m, "org.freedesktop.portal.Error.NotFound", "Requested setting not found");
}
static void settings_poll(void) {
    struct appearance a; load_appearance(&a);
    if (!have_current) { current = a; have_current = true; return; }
    for (unsigned s = 0; s < NSETTINGS; s++) {
        const char *ns = SETTINGS[s][0], *key = SETTINGS[s][1]; bool changed;
        if (!strcmp(key, "color-scheme")) changed = a.scheme != current.scheme;
        else if (!strcmp(key, "accent-color")) changed = a.has_accent != current.has_accent || memcmp(a.accent, current.accent, sizeof(a.accent));
        else if (!strcmp(key, "contrast") || !strcmp(key, "high-contrast")) changed = a.contrast != current.contrast;
        else if (!strcmp(key, "text-scaling-factor")) changed = a.text_scale != current.text_scale;
        else changed = a.motion != current.motion;
        if (!changed) continue;
        DBusMessage* sig = dbus_message_new_signal(PATH, I_SETTINGS, "SettingChanged"); DBusMessageIter it; dbus_message_iter_init_append(sig, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &ns); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key); append_value(&it, ns, key, &a);
        dbus_connection_send(bus, sig, NULL); dbus_message_unref(sig);
        note("SettingChanged %s %s", ns, key);
    }
    current = a;
}

/* ---------------------------------------------------------------- requests and dialogs */
static DBusHandlerResult request_handler(DBusConnection* c, DBusMessage* m, void* data);
static const DBusObjectPathVTable request_vtable = {.message_function = request_handler};
static void response(struct request* r, dbus_uint32_t code, void (*results)(struct request*, DBusMessageIter*)) {
    if (r->call) {
        DBusMessage* reply = dbus_message_new_method_return(r->call); DBusMessageIter it, dict; dbus_message_iter_init_append(reply, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &code); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        if (code == 0 && results) results(r, &dict);
        dbus_message_iter_close_container(&it, &dict); dbus_connection_send(bus, reply, NULL); dbus_message_unref(reply);
        dbus_message_unref(r->call); r->call = NULL;
    }
}
static void release(struct request* r) {
    if (r->pid > 0) kill(r->pid, SIGTERM);
    if (r->pipe >= 0) close(r->pipe);
    for (int i = 0; i < 2; i++) if (r->inhibit_fds[i] >= 0) close(r->inhibit_fds[i]);
    if (r->call) { dbus_message_unref(r->call); r->call = NULL; }
    if (r->handle[0]) dbus_connection_unregister_object_path(bus, r->handle);
    free(r->out); memset(r, 0, sizeof(*r)); r->pipe = -1; r->inhibit_fds[0] = r->inhibit_fds[1] = -1;
}
static struct request* new_request(DBusMessage* m, const char* handle, int kind) {
    if (!dbus_validate_path(handle, NULL)) return NULL;
    struct request* r = NULL; for (int i = 0; i < MAX_REQUESTS && !r; i++) if (!requests[i].used) r = &requests[i];
    if (!r) return NULL;
    memset(r, 0, sizeof(*r)); r->used = true; r->kind = kind; r->pipe = -1; r->inhibit_fds[0] = r->inhibit_fds[1] = -1;
    snprintf(r->handle, sizeof(r->handle), "%s", handle);
    if (!dbus_connection_register_object_path(bus, r->handle, &request_vtable, r)) { r->handle[0] = 0; r->used = false; return NULL; }
    r->call = m ? dbus_message_ref(m) : NULL; return r;
}
static DBusHandlerResult request_handler(DBusConnection* c, DBusMessage* m, void* data) {
    (void)c; struct request* r = data;
    if (dbus_message_is_method_call(m, I_REQUEST, "Close")) {
        note("request %s closed by the frontend", r->handle);
        DBusMessage* reply = dbus_message_new_method_return(m); if (reply) { dbus_connection_send(bus, reply, NULL); dbus_message_unref(reply); }
        response(r, 2, NULL); release(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
/* the dialog program, as this user, with its parameters in the environment and the result on a pipe */
static bool spawn_dialog(struct request* r, const char* kind, const char* const* env, int envc) {
    int p[2]; if (pipe2(p, O_CLOEXEC)) return false;
    pid_t pid = fork();
    if (!pid) {
        int fd = fcntl(p[1], F_DUPFD, 10); if (fd < 0) _exit(127);   /* not close-on-exec */
        char text[32]; snprintf(text, sizeof(text), "AURORA_DIALOG_FD=%d", fd); putenv(text);
        for (int i = 0; i < envc; i++) putenv((char*)env[i]);
        if (!getenv("WAYLAND_DISPLAY")) putenv("WAYLAND_DISPLAY=wayland-0");
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL); signal(SIGCHLD, SIG_DFL);
        execl("/bin/aurora-app", "aurora-app", kind, (char*)NULL); _exit(127);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return false; }
    fcntl(p[0], F_SETFL, O_NONBLOCK); r->pid = pid; r->pipe = p[0]; return true;
}
static char* env_entry(const char* key, const char* value) {   /* KEY=VALUE without control characters (except newlines in bodies) */
    size_t n = strlen(key) + strlen(value) + 2; char* s = malloc(n); if (!s) return NULL;
    snprintf(s, n, "%s=%s", key, value); for (char* p = s; *p; p++) if ((unsigned char)*p < 32 && *p != '\n') *p = ' ';
    return s;
}
/* a{sv} option lookups */
static bool option(DBusMessageIter* options, const char* name, DBusMessageIter* value) {
    DBusMessageIter dict, entry; dbus_message_iter_recurse(options, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(&dict, &entry); const char* key; dbus_message_iter_get_basic(&entry, &key); dbus_message_iter_next(&entry);
        if (!strcmp(key, name) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) { dbus_message_iter_recurse(&entry, value); return true; }
        dbus_message_iter_next(&dict);
    }
    return false;
}
static bool option_bool(DBusMessageIter* options, const char* name) {
    DBusMessageIter v; dbus_bool_t b = 0; if (option(options, name, &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(&v, &b); return b;
}
static const char* option_string(DBusMessageIter* options, const char* name) {
    DBusMessageIter v; const char* s = NULL; if (option(options, name, &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&v, &s); return s;
}
static bool option_bytes(DBusMessageIter* options, const char* name, char* out, size_t size) {   /* ay, NUL-terminated path */
    DBusMessageIter v, arr; out[0] = 0; if (!option(options, name, &v) || dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_ARRAY) return false;
    dbus_message_iter_recurse(&v, &arr); const char* bytes; int n = 0;
    if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_BYTE) return false;
    dbus_message_iter_get_fixed_array(&arr, &bytes, &n); if (n <= 0 || (size_t)n > size) return false;
    if (memchr(bytes, 0, (size_t)n) != bytes + n - 1) return false;   /* exactly one NUL, at the end */
    memcpy(out, bytes, (size_t)n); return true;
}
static void mime_globs(const char* mime, char* out, size_t size) {   /* the common types the frontend may send as MIME filters */
    static const char* const map[][2] = {{"text/plain", "*.txt;*.text;*.log"}, {"image/png", "*.png"}, {"image/jpeg", "*.jpg;*.jpeg"}, {"image/gif", "*.gif"},
        {"image/webp", "*.webp"}, {"image/svg+xml", "*.svg"}, {"image/*", "*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.svg"}, {"application/pdf", "*.pdf"},
        {"text/html", "*.html;*.htm"}, {"audio/*", "*.mp3;*.ogg;*.opus;*.wav;*.flac"}, {"video/*", "*.mp4;*.webm;*.mkv;*.ogv"}, {"application/zip", "*.zip"}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(*map); i++) if (!strcmp(map[i][0], mime)) { size_t l = strlen(out); snprintf(out + l, size - l, "%s%s", l ? ";" : "", map[i][1]); return; }
}
static void filter_patterns(DBusMessageIter* filter, char* out, size_t size) {   /* (sa(us)) */
    DBusMessageIter st, arr, item; dbus_message_iter_recurse(filter, &st); dbus_message_iter_next(&st);
    if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&st, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(&arr, &item); dbus_uint32_t kind = 0; const char* pattern = "";
        if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_UINT32) { dbus_message_iter_get_basic(&item, &kind); dbus_message_iter_next(&item); }
        if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&item, &pattern);
        if (kind == 0 && pattern[0] && !strchr(pattern, ';') && !strchr(pattern, '/')) { size_t l = strlen(out); snprintf(out + l, size - l, "%s%s", l ? ";" : "", pattern); }
        else if (kind == 1) mime_globs(pattern, out, size);
        dbus_message_iter_next(&arr);
    }
}
static DBusHandlerResult file_chooser(DBusMessage* m, int kind) {
    DBusMessageIter it, options; const char *handle, *app, *parent, *title;
    if (strcmp(dbus_message_get_signature(m), "osssa{sv}") || !dbus_message_iter_init(m, &it)) return error(m, E_ARGS, "(osssa{sv})");
    dbus_message_iter_get_basic(&it, &handle); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &parent); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &title); dbus_message_iter_next(&it);
    dbus_message_iter_recurse(&it, &options); DBusMessageIter options_iter = it; (void)parent;
    struct request* r = new_request(m, handle, kind); if (!r) return error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many requests or a bad handle");
    snprintf(r->app, sizeof(r->app), "%s", app);
    char folder[4096] = "", name[256] = "", patterns[512] = "", mode[32];
    option_bytes(&options_iter, "current_folder", folder, sizeof(folder));
    const char* current_name = option_string(&options_iter, "current_name"); if (current_name && !strchr(current_name, '/')) snprintf(name, sizeof(name), "%s", current_name);
    DBusMessageIter v;
    if (option(&options_iter, "current_filter", &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRUCT) filter_patterns(&v, patterns, sizeof(patterns));
    else if (option(&options_iter, "filters", &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr; dbus_message_iter_recurse(&v, &arr); if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) filter_patterns(&arr, patterns, sizeof(patterns)); }
    if (kind == R_FILE_SAVES) {   /* the files go into a chosen folder */
        if (option(&options_iter, "files", &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_ARRAY) {
            DBusMessageIter arr; dbus_message_iter_recurse(&v, &arr); size_t used = 0;
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_ARRAY) {
                DBusMessageIter bytes; dbus_message_iter_recurse(&arr, &bytes); const char* b; int n = 0;
                if (dbus_message_iter_get_arg_type(&bytes) == DBUS_TYPE_BYTE) dbus_message_iter_get_fixed_array(&bytes, &b, &n);
                if (n > 1 && memchr(b, 0, (size_t)n) == b + n - 1 && !memchr(b, '/', (size_t)n) && used + (size_t)n + 1 < sizeof(r->save_files)) { memcpy(r->save_files + used, b, (size_t)n - 1); used += (size_t)n - 1; r->save_files[used++] = '\n'; }
                dbus_message_iter_next(&arr);
            }
            r->save_files[used] = 0;
        }
        snprintf(mode, sizeof(mode), "folder");
    } else if (kind == R_FILE_SAVE) snprintf(mode, sizeof(mode), "save");
    else snprintf(mode, sizeof(mode), "%s", option_bool(&options_iter, "directory") ? "folder" : option_bool(&options_iter, "multiple") ? "open-multiple" : "open");
    const char* accept = option_string(&options_iter, "accept_label");
    char* env[8]; int envc = 0;
    env[envc++] = env_entry("AURORA_DIALOG_TITLE", title[0] ? title : kind == R_FILE_OPEN ? "Open" : "Save");
    env[envc++] = env_entry("AURORA_DIALOG_MODE", mode);
    if (folder[0]) env[envc++] = env_entry("AURORA_DIALOG_FOLDER", folder);
    if (name[0]) env[envc++] = env_entry("AURORA_DIALOG_NAME", name);
    if (patterns[0]) env[envc++] = env_entry("AURORA_DIALOG_PATTERNS", patterns);
    if (accept && accept[0] && strlen(accept) < 48) { char clean[64]; size_t k = 0; for (const char* p = accept; *p && k + 1 < sizeof(clean); p++) if (*p != '_') clean[k++] = *p; clean[k] = 0; env[envc++] = env_entry("AURORA_DIALOG_ACCEPT", clean); }
    bool ok = true; for (int i = 0; i < envc; i++) ok &= env[i] != NULL;
    ok = ok && spawn_dialog(r, "filechooser", (const char* const*)env, envc);
    for (int i = 0; i < envc; i++) free(env[i]);
    if (!ok) { response(r, 2, NULL); release(r); return DBUS_HANDLER_RESULT_HANDLED; }
    note("FileChooser %s mode=%s app=%s", dbus_message_get_member(m), mode, app);
    return DBUS_HANDLER_RESULT_HANDLED;
}
static void uri_of(const char* path, char* out, size_t size) {   /* file:// + percent-encoding */
    size_t k = (size_t)snprintf(out, size, "file://");
    for (const unsigned char* p = (const unsigned char*)path; *p && k + 4 < size; p++) {
        if (isalnum(*p) || strchr("/-._~", *p)) out[k++] = (char)*p; else k += (size_t)snprintf(out + k, size - k, "%%%02X", *p);
    }
    out[k] = 0;
}
static void file_results(struct request* r, DBusMessageIter* dict) {
    DBusMessageIter entry, v, arr; const char* key = "uris";
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry); dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &v); dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &arr);
    char* save = NULL;
    for (char* line = strtok_r(r->out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] != '/') continue;
        if (r->kind == R_FILE_SAVES) {   /* folder + each proposed name */
            char names[4096]; snprintf(names, sizeof(names), "%s", r->save_files); char* s2 = NULL;
            for (char* n = strtok_r(names, "\n", &s2); n; n = strtok_r(NULL, "\n", &s2)) { char full[8192], uri[16384]; snprintf(full, sizeof(full), "%s/%s", line, n); uri_of(full, uri, sizeof(uri)); const char* u = uri; dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &u); }
            break;
        }
        char uri[16384]; uri_of(line, uri, sizeof(uri)); const char* u = uri; dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &u);
    }
    dbus_message_iter_close_container(&v, &arr); dbus_message_iter_close_container(&entry, &v); dbus_message_iter_close_container(dict, &entry);
    if (r->kind == R_FILE_OPEN) { const char* w = "writable"; dbus_bool_t yes = 1; dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &w); dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &yes);
        dbus_message_iter_close_container(&entry, &v); dbus_message_iter_close_container(dict, &entry); }
}
static DBusHandlerResult access_dialog(DBusMessage* m) {
    DBusMessageIter it, options; const char *handle, *app, *parent, *title, *subtitle, *body;
    if (strcmp(dbus_message_get_signature(m), "osssssa{sv}") || !dbus_message_iter_init(m, &it)) return error(m, E_ARGS, "AccessDialog(osssssa{sv})");
    dbus_message_iter_get_basic(&it, &handle); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &parent); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &title); dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &subtitle); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &body); dbus_message_iter_next(&it);
    options = it; (void)parent;
    struct request* r = new_request(m, handle, R_ACCESS); if (!r) return error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many requests or a bad handle");
    char text[1200]; snprintf(text, sizeof(text), "%.300s\n%.300s\n%.500s", subtitle, body, app[0] ? app : "");
    const char* grant = option_string(&options, "grant_label");
    char* env[3] = {env_entry("AURORA_DIALOG_TITLE", title[0] ? title : "Permission"), env_entry("AURORA_DIALOG_BODY", text), grant && strlen(grant) < 48 ? env_entry("AURORA_DIALOG_ACCEPT", grant) : NULL};
    int envc = env[2] ? 3 : 2; bool ok = env[0] && env[1] && spawn_dialog(r, "prompt", (const char* const*)env, envc);
    for (int i = 0; i < 3; i++) free(env[i]);
    if (!ok) { response(r, 2, NULL); release(r); }
    else note("AccessDialog app=%s title=%s", app, title);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------------------------------------------------------------- screenshot */
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320U ^ (v >> 1) : v >> 1; crc_table[i] = v; }
    c ^= 0xFFFFFFFFU; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFU;
}
static bool chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
    uint8_t head[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3]};
    uint32_t c = crc(crc(0, head + 4, 4), data, n); uint8_t tail[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    return fwrite(head, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(tail, 1, 4, f) == 4;
}
/* PNG, RGB 8-bit, zlib with stored (uncompressed) deflate blocks: simple and exact */
static bool write_png(const char* path, const uint32_t* pixels, int w, int h, int stride) {
    size_t raw = (size_t)h * ((size_t)w * 3 + 1), blocks = (raw + 65534) / 65535, size = 2 + raw + blocks * 5 + 4;
    uint8_t* z = malloc(size); if (!z) return false;
    size_t k = 0; z[k++] = 0x78; z[k++] = 0x01; uint32_t a = 1, b = 0; size_t left = raw, row_byte = 0; int y = 0, x = 0;
    while (left) {
        size_t n = left > 65535 ? 65535 : left; left -= n;
        z[k++] = left ? 0 : 1; z[k++] = (uint8_t)n; z[k++] = (uint8_t)(n >> 8); z[k++] = (uint8_t)~n; z[k++] = (uint8_t)(~n >> 8);
        for (size_t i = 0; i < n; i++) {
            uint8_t byte;
            if (row_byte == 0) byte = 0;   /* filter: none */
            else { uint32_t p = pixels[(size_t)y * (size_t)stride + (size_t)x]; int c = (int)((row_byte - 1) % 3); byte = (uint8_t)(c == 0 ? p >> 16 : c == 1 ? p >> 8 : p); if (c == 2) x++; }
            z[k++] = byte; a = (a + byte) % 65521; b = (b + a) % 65521;
            if (++row_byte == (size_t)w * 3 + 1) { row_byte = 0; x = 0; y++; }
        }
    }
    uint32_t adler = b << 16 | a; z[k++] = (uint8_t)(adler >> 24); z[k++] = (uint8_t)(adler >> 16); z[k++] = (uint8_t)(adler >> 8); z[k++] = (uint8_t)adler;
    char tmp[4200]; snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE* f = fopen(tmp, "wbx"); if (!f) { free(z); return false; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    bool ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, (uint32_t)k) && chunk(f, "IEND", NULL, 0);
    ok = !fflush(f) && !fsync(fileno(f)) && ok; fclose(f); free(z);
    if (!ok || rename(tmp, path)) { unlink(tmp); return false; }
    return true;
}
static bool take_screenshot(char* path, size_t size) {
    int fd = memfd_create("aurora-screenshot", MFD_CLOEXEC); if (fd < 0) return false;
    char reply[256]; bool ok = aurora("screenshot\n", fd, reply, sizeof(reply));
    if (!ok) { note("screenshot refused: %s", reply); close(fd); return false; }
    struct stat st; if (fstat(fd, &st) || st.st_size < 32) { close(fd); return false; }
    uint8_t* data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd); if (data == MAP_FAILED) return false;
    int w = 0, h = 0, stride = 0, x0, y0, header = 0;
    ok = sscanf((const char*)data, "NXFRAME 1 %d %d %d %d %d\n%n", &w, &h, &stride, &x0, &y0, &header) == 5 && header > 0 && w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
         stride >= w * 4 && (size_t)header + (size_t)stride * (size_t)h <= (size_t)st.st_size;
    if (ok) {
        const char* home = getenv("HOME"); char dir[1024]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        snprintf(dir, sizeof(dir), "%s/Pictures", home && home[0] == '/' ? home : "/tmp"); mkdir(dir, 0755);
        char stamp[64]; strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tm);
        for (int n = 0; n < 100; n++) { if (n) snprintf(path, size, "%s/Screenshot from %s (%d).png", dir, stamp, n); else snprintf(path, size, "%s/Screenshot from %s.png", dir, stamp); if (access(path, F_OK)) break; }
        ok = write_png(path, (const uint32_t*)(data + header), w, h, stride / 4);
        if (ok) note("screenshot %dx%d -> %s", w, h, path);
    }
    munmap(data, (size_t)st.st_size); return ok;
}
static char shot_path[4200];
static void shot_results(struct request* r, DBusMessageIter* dict) {
    (void)r; DBusMessageIter entry, v; const char* key = "uri"; char uri[16384]; uri_of(shot_path, uri, sizeof(uri)); const char* u = uri;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry); dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &u);
    dbus_message_iter_close_container(&entry, &v); dbus_message_iter_close_container(dict, &entry);
}
static DBusHandlerResult screenshot(DBusMessage* m) {
    DBusMessageIter it, options; const char *handle, *app, *parent;
    if (strcmp(dbus_message_get_signature(m), "ossa{sv}") || !dbus_message_iter_init(m, &it)) return error(m, E_ARGS, "Screenshot(ossa{sv})");
    dbus_message_iter_get_basic(&it, &handle); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &parent); dbus_message_iter_next(&it); options = it; (void)parent;
    bool interactive = option_bool(&options, "interactive"), checked = option_bool(&options, "permission_store_checked");
    struct request* r = new_request(m, handle, R_SHOT_PROMPT); if (!r) return error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many requests or a bad handle");
    snprintf(r->app, sizeof(r->app), "%s", app);
    if (!interactive && checked) { bool ok = take_screenshot(shot_path, sizeof(shot_path)); response(r, ok ? 0 : 2, ok ? shot_results : NULL); release(r); return DBUS_HANDLER_RESULT_HANDLED; }
    char text[600]; snprintf(text, sizeof(text), "%.200s wants to take a screenshot of your screen.\nEverything visible will be in the picture (saved in Pictures).", app[0] ? app : "An application");
    char* env[3] = {env_entry("AURORA_DIALOG_TITLE", "Screenshot"), env_entry("AURORA_DIALOG_BODY", text), env_entry("AURORA_DIALOG_ACCEPT", "Take")};
    bool ok = env[0] && env[1] && env[2] && spawn_dialog(r, "prompt", (const char* const*)env, 3);
    for (int i = 0; i < 3; i++) free(env[i]);
    if (!ok) { response(r, 2, NULL); release(r); }
    note("Screenshot app=%s interactive=%d checked=%d", app, interactive, checked);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------------------------------------------------------------- inhibit (logind), notifications */
static int logind_inhibit(const char* what, const char* who, const char* why) {
    if (!system_bus) { DBusError e; dbus_error_init(&e); system_bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e); dbus_error_free(&e); if (!system_bus) return -1; dbus_connection_set_exit_on_disconnect(system_bus, FALSE); }
    const char* mode = "block"; DBusMessage* q = dbus_message_new_method_call("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit");
    if (!q) return -1;
    dbus_message_append_args(q, DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who, DBUS_TYPE_STRING, &why, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID);
    DBusError e; dbus_error_init(&e); DBusMessage* r = dbus_connection_send_with_reply_and_block(system_bus, q, 5000, &e); dbus_message_unref(q);
    int fd = -1; if (r) { if (!dbus_message_get_args(r, &e, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) fd = -1; dbus_message_unref(r); }
    if (dbus_error_is_set(&e)) { note("logind Inhibit(%s): %s", what, e.message); dbus_error_free(&e); }
    return fd;
}
static DBusHandlerResult inhibit(DBusMessage* m) {
    DBusMessageIter it, options; const char *handle, *app, *window; dbus_uint32_t flags;
    if (strcmp(dbus_message_get_signature(m), "ossua{sv}") || !dbus_message_iter_init(m, &it)) return error(m, E_ARGS, "Inhibit(ossua{sv})");
    dbus_message_iter_get_basic(&it, &handle); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &window); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &flags); dbus_message_iter_next(&it); options = it; (void)window;
    if (flags & ~15U) return error(m, E_ARGS, "unknown inhibit flags");
    struct request* r = new_request(NULL, handle, R_INHIBIT); if (!r) return error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many requests or a bad handle");
    const char* reason = option_string(&options, "reason"); char why[256]; snprintf(why, sizeof(why), "%s", reason && reason[0] ? reason : "requested through the desktop portal");
    for (char* p = why; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    char who[256]; snprintf(who, sizeof(who), "%s", app[0] ? app : "portal client"); for (char* p = who; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    if (flags & 8) r->inhibit_fds[0] = logind_inhibit("idle", who, why);
    if (flags & 4) r->inhibit_fds[1] = logind_inhibit("sleep", who, why);
    note("Inhibit app=%s flags=%u idle=%d sleep=%d", app, flags, r->inhibit_fds[0] >= 0, r->inhibit_fds[1] >= 0);
    DBusMessage* reply = dbus_message_new_method_return(m); if (reply) { dbus_connection_send(bus, reply, NULL); dbus_message_unref(reply); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
struct notification { char app[128], id[128]; dbus_uint32_t server; };
static struct notification notifications[64];
static DBusHandlerResult add_notification(DBusMessage* m) {
    DBusMessageIter it, n; const char *app, *id;
    if (strcmp(dbus_message_get_signature(m), "ssa{sv}") || !dbus_message_iter_init(m, &it)) return error(m, E_ARGS, "AddNotification(ssa{sv})");
    dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &id); dbus_message_iter_next(&it); n = it;
    const char* title = option_string(&n, "title"); const char* body = option_string(&n, "body");
    struct notification* slot = NULL; for (int i = 0; i < 64; i++) if (!strcmp(notifications[i].app, app) && !strcmp(notifications[i].id, id)) slot = &notifications[i];
    for (int i = 0; i < 64 && !slot; i++) if (!notifications[i].app[0] && !notifications[i].id[0]) slot = &notifications[i];
    if (!slot) slot = &notifications[0];
    DBusMessage* q = dbus_message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
    const char *name = app[0] ? app : "portal", *icon = "", *summary = title ? title : "", *text = body ? body : ""; dbus_uint32_t replaces = slot->server; dbus_int32_t timeout = -1;
    DBusMessageIter qi, arr, dict; dbus_message_iter_init_append(q, &qi);
    dbus_message_iter_append_basic(&qi, DBUS_TYPE_STRING, &name); dbus_message_iter_append_basic(&qi, DBUS_TYPE_UINT32, &replaces); dbus_message_iter_append_basic(&qi, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&qi, DBUS_TYPE_STRING, &summary); dbus_message_iter_append_basic(&qi, DBUS_TYPE_STRING, &text);
    dbus_message_iter_open_container(&qi, DBUS_TYPE_ARRAY, "s", &arr); dbus_message_iter_close_container(&qi, &arr);
    dbus_message_iter_open_container(&qi, DBUS_TYPE_ARRAY, "{sv}", &dict); dbus_message_iter_close_container(&qi, &dict);
    dbus_message_iter_append_basic(&qi, DBUS_TYPE_INT32, &timeout);
    DBusError e; dbus_error_init(&e); DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, q, 5000, &e); dbus_message_unref(q);
    if (r) { dbus_message_get_args(r, &e, DBUS_TYPE_UINT32, &slot->server, DBUS_TYPE_INVALID); dbus_message_unref(r); }
    bool ok = !dbus_error_is_set(&e); if (!ok) note("Notify: %s", e.message); dbus_error_free(&e);
    snprintf(slot->app, sizeof(slot->app), "%s", app); snprintf(slot->id, sizeof(slot->id), "%s", id);
    if (!ok) return error(m, "org.freedesktop.DBus.Error.Failed", "the notification server is unavailable");
    DBusMessage* reply = dbus_message_new_method_return(m); if (reply) { dbus_connection_send(bus, reply, NULL); dbus_message_unref(reply); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult remove_notification(DBusMessage* m) {
    const char *app, *id; DBusError e; dbus_error_init(&e);
    if (strcmp(dbus_message_get_signature(m), "ss") || !dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &app, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID)) { dbus_error_free(&e); return error(m, E_ARGS, "RemoveNotification(ss)"); }
    for (int i = 0; i < 64; i++) if (!strcmp(notifications[i].app, app) && !strcmp(notifications[i].id, id)) {
        DBusMessage* q = dbus_message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "CloseNotification");
        if (q) { dbus_message_append_args(q, DBUS_TYPE_UINT32, &notifications[i].server, DBUS_TYPE_INVALID); dbus_connection_send(bus, q, NULL); dbus_message_unref(q); }
        memset(&notifications[i], 0, sizeof(notifications[i]));
    }
    DBusMessage* reply = dbus_message_new_method_return(m); if (reply) { dbus_connection_send(bus, reply, NULL); dbus_message_unref(reply); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult choose_application(DBusMessage* m) {
    DBusMessageIter it, choices, options; const char *handle, *app, *parent;
    if (strcmp(dbus_message_get_signature(m), "ossasa{sv}") || !dbus_message_iter_init(m, &it)) return error(m, E_ARGS, "ChooseApplication(ossasa{sv})");
    dbus_message_iter_get_basic(&it, &handle); dbus_message_iter_next(&it); dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &parent); dbus_message_iter_next(&it); dbus_message_iter_recurse(&it, &choices); dbus_message_iter_next(&it); options = it; (void)parent;
    const char* last = option_string(&options, "last_choice"); const char* first = NULL, *pick = NULL;
    while (dbus_message_iter_get_arg_type(&choices) == DBUS_TYPE_STRING) { const char* c; dbus_message_iter_get_basic(&choices, &c); if (!first) first = c; if (last && !strcmp(last, c)) pick = c; dbus_message_iter_next(&choices); }
    if (!pick) pick = first;
    DBusMessage* r = dbus_message_new_method_return(m); dbus_uint32_t code = pick ? 0 : 2; DBusMessageIter ri, dict, entry, v; dbus_message_iter_init_append(r, &ri);
    dbus_message_iter_append_basic(&ri, DBUS_TYPE_UINT32, &code); dbus_message_iter_open_container(&ri, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (pick) { const char* key = "choice"; dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry); dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &pick); dbus_message_iter_close_container(&entry, &v); dbus_message_iter_close_container(&dict, &entry); }
    dbus_message_iter_close_container(&ri, &dict); dbus_connection_send(bus, r, NULL); dbus_message_unref(r);
    note("ChooseApplication app=%s choice=%s", app, pick ? pick : "-");
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------------------------------------------------------------- dispatch */
static const char* const INTROSPECTION = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>"
    "<interface name=\"org.freedesktop.impl.portal.Settings\"><method name=\"ReadAll\"><arg type=\"as\" direction=\"in\"/><arg type=\"a{sa{sv}}\" direction=\"out\"/></method>"
    "<method name=\"Read\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
    "<signal name=\"SettingChanged\"><arg type=\"s\"/><arg type=\"s\"/><arg type=\"v\"/></signal><property name=\"version\" type=\"u\" access=\"read\"/></interface>"
    "<interface name=\"org.freedesktop.impl.portal.FileChooser\">"
    "<method name=\"OpenFile\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "<method name=\"SaveFile\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "<method name=\"SaveFiles\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "<property name=\"version\" type=\"u\" access=\"read\"/></interface>"
    "<interface name=\"org.freedesktop.impl.portal.AppChooser\"><method name=\"ChooseApplication\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "<method name=\"UpdateChoices\"><arg type=\"o\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/></method><property name=\"version\" type=\"u\" access=\"read\"/></interface>"
    "<interface name=\"org.freedesktop.impl.portal.Access\"><method name=\"AccessDialog\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method><property name=\"version\" type=\"u\" access=\"read\"/></interface>"
    "<interface name=\"org.freedesktop.impl.portal.Screenshot\"><method name=\"Screenshot\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "<method name=\"PickColor\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method><property name=\"version\" type=\"u\" access=\"read\"/></interface>"
    "<interface name=\"org.freedesktop.impl.portal.Inhibit\"><method name=\"Inhibit\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/></method>"
    "<method name=\"CreateMonitor\"><arg type=\"o\" direction=\"in\"/><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/></method>"
    "<method name=\"QueryEndResponse\"><arg type=\"o\" direction=\"in\"/></method><property name=\"version\" type=\"u\" access=\"read\"/></interface>"
    "<interface name=\"org.freedesktop.impl.portal.Notification\"><method name=\"AddNotification\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/></method>"
    "<method name=\"RemoveNotification\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method><signal name=\"ActionInvoked\"><arg type=\"s\"/><arg type=\"s\"/><arg type=\"s\"/><arg type=\"av\"/></signal><property name=\"version\" type=\"u\" access=\"read\"/></interface>"
    "</node>\n";
static DBusHandlerResult handle(DBusConnection* c, DBusMessage* m, void* data) {
    (void)c; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m), *member = dbus_message_get_member(m); if (!iface || !member) return error(m, E_ARGS, "interface required");
    if (!strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
        DBusMessage* r = dbus_message_new_method_return(m); dbus_message_append_args(r, DBUS_TYPE_STRING, &INTROSPECTION, DBUS_TYPE_INVALID); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(iface, "org.freedesktop.DBus.Properties") && (!strcmp(member, "Get") || !strcmp(member, "GetAll"))) {
        const char* which = NULL; DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &which, DBUS_TYPE_INVALID) && strcmp(member, "Get")) { dbus_error_free(&e); return error(m, E_ARGS, "GetAll(s)"); }
        dbus_error_free(&e);
        dbus_uint32_t version = !which ? 1 : !strcmp(which, I_SETTINGS) || !strcmp(which, I_SHOT) || !strcmp(which, I_APPS) ? 2 : !strcmp(which, I_FILES) ? 4 : !strcmp(which, I_INHIBIT) ? 3 : 1;
        DBusMessage* r = dbus_message_new_method_return(m); DBusMessageIter it, dict, entry, v; dbus_message_iter_init_append(r, &it); const char* key = "version";
        if (!strcmp(member, "Get")) { dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "u", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &version); dbus_message_iter_close_container(&it, &v); }
        else { dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict); dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key); dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &version);
            dbus_message_iter_close_container(&entry, &v); dbus_message_iter_close_container(&dict, &entry); dbus_message_iter_close_container(&it, &dict); }
        dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(iface, I_SETTINGS)) { if (!strcmp(member, "ReadAll")) return settings_read_all(m); if (!strcmp(member, "Read")) return settings_read(m); }
    if (!strcmp(iface, I_FILES)) { if (!strcmp(member, "OpenFile")) return file_chooser(m, R_FILE_OPEN); if (!strcmp(member, "SaveFile")) return file_chooser(m, R_FILE_SAVE); if (!strcmp(member, "SaveFiles")) return file_chooser(m, R_FILE_SAVES); }
    if (!strcmp(iface, I_APPS)) {
        if (!strcmp(member, "ChooseApplication")) return choose_application(m);
        if (!strcmp(member, "UpdateChoices")) { DBusMessage* r = dbus_message_new_method_return(m); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED; }
    }
    if (!strcmp(iface, I_ACCESS) && !strcmp(member, "AccessDialog")) return access_dialog(m);
    if (!strcmp(iface, I_SHOT)) {
        if (!strcmp(member, "Screenshot")) return screenshot(m);
        if (!strcmp(member, "PickColor")) { DBusMessage* r = dbus_message_new_method_return(m); dbus_uint32_t code = 2; DBusMessageIter it, dict; dbus_message_iter_init_append(r, &it);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &code); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict); dbus_message_iter_close_container(&it, &dict);
            dbus_connection_send(bus, r, NULL); dbus_message_unref(r); note("PickColor: not supported"); return DBUS_HANDLER_RESULT_HANDLED; }
    }
    if (!strcmp(iface, I_INHIBIT)) {
        if (!strcmp(member, "Inhibit")) return inhibit(m);
        if (!strcmp(member, "CreateMonitor")) { DBusMessage* r = dbus_message_new_method_return(m); dbus_uint32_t code = 2; dbus_message_append_args(r, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED; }
        if (!strcmp(member, "QueryEndResponse")) { DBusMessage* r = dbus_message_new_method_return(m); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED; }
    }
    if (!strcmp(iface, I_NOTIFY)) { if (!strcmp(member, "AddNotification")) return add_notification(m); if (!strcmp(member, "RemoveNotification")) return remove_notification(m); }
    return error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}
static void child_finished(pid_t pid, int status) {
    for (int i = 0; i < MAX_REQUESTS; i++) if (requests[i].used && requests[i].pid == pid) {
        struct request* r = &requests[i]; r->pid = 0;
        for (;;) {   /* whatever is left in the pipe */
            char buf[4096]; ssize_t n = r->pipe >= 0 ? read(r->pipe, buf, sizeof(buf)) : 0; if (n <= 0) break;
            if (r->out_used + (size_t)n > 1 << 20) break;
            char* grown = realloc(r->out, r->out_used + (size_t)n + 1); if (!grown) break; r->out = grown; memcpy(r->out + r->out_used, buf, (size_t)n); r->out_used += (size_t)n; r->out[r->out_used] = 0;
        }
        bool chosen = WIFEXITED(status) && WEXITSTATUS(status) == 0 && r->out_used > 0;
        note("dialog %s exit=%d chosen=%d", r->handle, WIFEXITED(status) ? WEXITSTATUS(status) : -1, chosen);
        if (r->kind == R_ACCESS) response(r, chosen && !strncmp(r->out, "allow", 5) ? 0 : 1, NULL);
        else if (r->kind == R_SHOT_PROMPT) { bool ok = chosen && !strncmp(r->out, "allow", 5) && take_screenshot(shot_path, sizeof(shot_path)); response(r, ok ? 0 : chosen ? 2 : 1, ok ? shot_results : NULL); }
        else response(r, chosen ? 0 : 1, chosen ? file_results : NULL);
        release(r);
    }
}
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_REQUESTS; i++) { requests[i].pipe = -1; requests[i].inhibit_fds[0] = requests[i].inhibit_fds[1] = -1; }
    if (pipe2(child_pipe, O_CLOEXEC | O_NONBLOCK)) return 1;
    DBusError e; dbus_error_init(&e); bus = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
    if (!bus) { fprintf(stderr, "xdg-desktop-portal-aurora: no session bus: %s\n", e.message); return 1; }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    int got = dbus_bus_request_name(bus, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (got != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { fprintf(stderr, "xdg-desktop-portal-aurora: cannot own %s\n", BUS_NAME); return 1; }
    static const DBusObjectPathVTable vtable = {.message_function = handle};
    if (!dbus_connection_register_object_path(bus, PATH, &vtable, NULL)) return 1;
    note("ready uid=%u runtime=%s", (unsigned)getuid(), runtime_dir() ? runtime_dir() : "-");
    int fd = -1; dbus_connection_get_unix_fd(bus, &fd);
    uint64_t last_poll = 0, idle_since = 0;
    for (;;) {
        dbus_connection_flush(bus);
        struct pollfd fds[1 + MAX_REQUESTS]; struct request* owner[1 + MAX_REQUESTS]; unsigned nf = 0;
        fds[nf] = (struct pollfd){fd, POLLIN, 0}; owner[nf++] = NULL;
        for (int i = 0; i < MAX_REQUESTS; i++) if (requests[i].used && requests[i].pipe >= 0) { fds[nf] = (struct pollfd){requests[i].pipe, POLLIN, 0}; owner[nf++] = &requests[i]; }
        int timeout = dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS ? 0 : 500;
        int ready = poll(fds, nf, timeout);
        if (ready < 0 && errno != EINTR) break;
        if (!dbus_connection_read_write(bus, 0)) break;
        while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
        for (unsigned i = 1; ready > 0 && i < nf; i++) if (fds[i].revents && owner[i]->used && owner[i]->pipe >= 0) {
            struct request* r = owner[i]; char buf[4096]; ssize_t n = read(r->pipe, buf, sizeof(buf));
            if (n > 0 && r->out_used + (size_t)n <= 1 << 20) { char* grown = realloc(r->out, r->out_used + (size_t)n + 1); if (grown) { r->out = grown; memcpy(r->out + r->out_used, buf, (size_t)n); r->out_used += (size_t)n; r->out[r->out_used] = 0; } }
            else if (n == 0) { close(r->pipe); r->pipe = -1; }
        }
        int status; pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) child_finished(pid, status);
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); uint64_t now = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
        if (now - last_poll >= 1000) { last_poll = now; settings_poll(); }
        bool busy = false; for (int i = 0; i < MAX_REQUESTS; i++) busy |= requests[i].used;
        if (busy || !idle_since) idle_since = now;
        (void)idle_since;   /* the frontend keeps a connection; it stays as long as the session bus */
    }
    return 0;
}
