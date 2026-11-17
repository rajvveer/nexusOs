/* nexus-logind: the org.freedesktop.login1 subset NexusOS programs use, on the D-Bus system bus (ext4 installs; started
 * by nexusd as root: services/nexus-logind.service). The seat is Aurora's (pid 1): sessions, their users, which one
 * is active and whether it is locked come from Aurora's control socket ("sessions", aurora_session.c), re-read when
 * Aurora pokes /run/nexus/logind-notify and every 30 s; a leader whose real uid is not the session's is ignored.
 *
 * org.freedesktop.login1.Manager (/org/freedesktop/login1):
 *   GetSession GetSessionByPID GetUser GetUserByPID GetSeat ListSessions ListUsers ListSeats ListInhibitors
 *   ActivateSession ActivateSessionOnSeat LockSession UnlockSession LockSessions UnlockSessions TerminateSession
 *   TerminateUser KillSession KillUser PowerOff Reboot Suspend Hibernate (+ *WithFlags) HybridSleep
 *   SuspendThenHibernate (not supported) CanPowerOff CanReboot CanSuspend CanHibernate CanHybridSleep
 *   CanSuspendThenHibernate Inhibit; signals SessionNew SessionRemoved UserNew UserRemoved SeatNew PrepareForSleep
 *   PrepareForShutdown; properties IdleHint IdleSinceHint(Monotonic) BlockInhibited DelayInhibited InhibitDelayMaxUSec
 *   PreparingForShutdown PreparingForSleep NCurrentSessions NCurrentInhibitors InhibitorsMax KillUserProcesses NAutoVTs
 *   OnExternalPower Docked LidClosed HandlePowerKey HandleSuspendKey HandleHibernateKey HandleLidSwitch IdleAction.
 * org.freedesktop.login1.Session (/org/freedesktop/login1/session/_3N, also .../session/self and /auto = the caller's):
 *   Id User Name Timestamp(Monotonic) VTNr Seat TTY Display Remote RemoteHost RemoteUser Service Desktop Scope Leader
 *   Audit Type Class Active State IdleHint IdleSinceHint(Monotonic) LockedHint; Activate Lock Unlock Terminate Kill
 *   SetIdleHint SetLockedHint SetBrightness TakeControl ReleaseControl TakeDevice ReleaseDevice PauseDeviceComplete;
 *   signals Lock Unlock PauseDevice ResumeDevice.
 * org.freedesktop.login1.User (/org/freedesktop/login1/user/_UID, .../user/self) and .Seat (/org/freedesktop/login1/seat/seat0,
 *   .../seat/self, /auto); org.freedesktop.DBus.Properties (Get GetAll, PropertiesChanged), Introspectable, Peer.
 *
 * Authorization (polkit's defaults for logind, without an agent: "challenge" is refused with
 * InteractiveAuthorizationRequired): root may do everything; the active session's user may power off, reboot,
 * suspend and hibernate unless other users have sessions or another user's block inhibitor is held (then only root);
 * users with a session may hold inhibitors and lock/unlock/terminate their own sessions. authorize() is the seam for a
 * system authorization broker.
 * Inhibitors: a pipe whose write end the caller holds; closing it releases the inhibitor. "delay" (sleep, shutdown) makes
 * logind wait for its release up to InhibitDelayMaxUSec (5 s) after PrepareForSleep/PrepareForShutdown(true); "block"
 * refuses the operation for others than root. Idle inhibitors are passed to Aurora (no idle lock).
 * Sleep: logind writes /sys/power/state from a child (the write returns after the resume) and emits
 * PrepareForSleep(false) afterwards; sleeps started elsewhere (Aurora's menu, the sleep key) are seen as the kernel's
 * PM_EVENT uevents and announced as well. PowerOff/Reboot ask Aurora (control socket "power-off"/"reboot") for its
 * orderly shutdown. TakeDevice gives the active session's controller input (13:*) and DRM (226:*) nodes opened with the
 * session user's file-system identity, so the kernel's seat revokes input readers when the session goes inactive
 * (PauseDevice "force"; ResumeDevice with a new descriptor when it is active again). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.login1"
#define ROOT_PATH "/org/freedesktop/login1"
#define MANAGER "org.freedesktop.login1.Manager"
#define SESSION_IF "org.freedesktop.login1.Session"
#define USER_IF "org.freedesktop.login1.User"
#define SEAT_IF "org.freedesktop.login1.Seat"
#define PROPS "org.freedesktop.DBus.Properties"
#define AURORA_CONTROL "/run/user/1000/aurora-control"
#define NOTIFY_SOCKET "/run/nexus/logind-notify"
#define MAX_SESSIONS 16
#define MAX_INHIBITORS 512
#define MAX_PER_UID 128
#define MAX_DEVICES 16
#define DELAY_MAX_US 5000000ULL
#define E_ACCESS "org.freedesktop.DBus.Error.AccessDenied"
#define E_INTERACTIVE "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired"
#define E_ARGS "org.freedesktop.DBus.Error.InvalidArgs"
#define E_NOTSUP "org.freedesktop.DBus.Error.NotSupported"
#define E_NO_SESSION "org.freedesktop.login1.NoSuchSession"
#define E_NO_USER "org.freedesktop.login1.NoSuchUser"
#define E_NO_SEAT "org.freedesktop.login1.NoSuchSeat"
#define E_NO_PROCESS "org.freedesktop.login1.NoSessionForPID"
#define E_BUSY "org.freedesktop.login1.OperationInProgress"
#define E_INHIBITED "org.freedesktop.login1.OperationInhibited"
#define E_NOT_CONTROLLER "org.freedesktop.login1.NotInControl"
#define E_DEVICE_TAKEN "org.freedesktop.login1.DeviceIsTaken"
#define E_NO_DEVICE "org.freedesktop.login1.NoSuchDevice"

enum { W_SHUTDOWN = 1, W_SLEEP = 2, W_IDLE = 4, W_POWER_KEY = 8, W_SUSPEND_KEY = 16, W_HIBERNATE_KEY = 32, W_LID = 64, W_REBOOT_KEY = 128 };
static const char* const WHAT_NAMES[] = {"shutdown", "sleep", "idle", "handle-power-key", "handle-suspend-key", "handle-hibernate-key", "handle-lid-switch", "handle-reboot-key"};
enum { AUTH_YES, AUTH_CHALLENGE, AUTH_NO };

struct device { bool used; unsigned major, minor; int fd; bool paused; };
struct session {
    bool used, seen; char id[16]; unsigned uid, gid; char user[33], home[129], state[16]; long leader;
    bool active, locked, idle, locked_override, locked_hint; uint64_t since_us, since_mono, idle_since_us;
    char controller[256]; struct device devices[MAX_DEVICES];
};
struct inhibitor { bool used; unsigned what; bool delay; char who[256], why[256]; unsigned uid, pid; int fd; };

static DBusConnection* bus;
static struct session sessions[MAX_SESSIONS];
static struct inhibitor inhibitors[MAX_INHIBITORS];
static unsigned boot_uid = 1000;
static bool preparing_sleep, preparing_shutdown, aurora_seen;
static int pending_op;   /* what PrepareFor*(true) waits for: 0 none, 1 suspend, 2 hibernate, 3 power off, 4 reboot */
static uint64_t pending_deadline;
static pid_t sleep_child; static bool our_sleep; static uint64_t our_sleep_until;   /* the kernel's uevents of our own sleep come late */
static int child_pipe[2] = {-1, -1};
static unsigned idle_sent = ~0U;

static uint64_t now_us(clockid_t clock) { struct timespec t; clock_gettime(clock, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
static void logline(const char* format, ...) {
    char line[600]; int n = snprintf(line, sizeof(line), "nexus-logind: "); va_list list; va_start(list, format);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, list); va_end(list);
    size_t length = m < 0 ? (size_t)n : strlen(line); line[length++] = '\n'; if (write(2, line, length) < 0) {}
}

/* ---------------------------------------------------------------- Aurora */
static bool aurora_request(const char* request, char* reply, size_t size) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (fd < 0) return false;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "%s", AURORA_CONTROL);
    struct timeval tv = {3, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    bool ok = false; size_t used = 0; if (reply && size) reply[0] = 0;
    if (!connect(fd, (struct sockaddr*)&a, sizeof(a)) && write(fd, request, strlen(request)) == (ssize_t)strlen(request)) {
        char buffer[8192]; ssize_t n;
        while (used < sizeof(buffer) - 1 && (n = read(fd, buffer + used, sizeof(buffer) - 1 - used)) > 0) {
            used += (size_t)n; buffer[used] = 0;
            char* last = buffer + used - 1; if (*last != '\n') continue;
            char* start = last; while (start > buffer && start[-1] != '\n') start--;
            if (!strncmp(start, "OK", 2)) { ok = true; break; }
            if (!strncmp(start, "ERROR", 5)) break;
        }
        buffer[used] = 0; if (reply && size) snprintf(reply, size, "%s", buffer);
    }
    close(fd); return ok;
}
static long proc_uid(long pid) {   /* real uid of a live process, -1 if gone */
    char path[64], line[256]; snprintf(path, sizeof(path), "/proc/%ld/status", pid); FILE* f = fopen(path, "re"); if (!f) return -1;
    long uid = -1; char state = 0;
    while (fgets(line, sizeof(line), f)) { unsigned u; if (sscanf(line, "State: %c", &state) == 1 && state == 'Z') break; if (sscanf(line, "Uid: %u", &u) == 1) { uid = u; break; } }
    fclose(f); return uid;
}
static long proc_parent(long pid) {
    char path[64], text[512]; snprintf(path, sizeof(path), "/proc/%ld/stat", pid); FILE* f = fopen(path, "re"); if (!f) return -1;
    size_t n = fread(text, 1, sizeof(text) - 1, f); fclose(f); text[n] = 0; char* close = strrchr(text, ')'); long parent = -1;
    if (close && sscanf(close + 1, " %*c %ld", &parent) == 1) return parent;
    return -1;
}
static char* path_of(char* out, size_t size, const char* kind, const char* id) {   /* sd-bus label escaping */
    int n = snprintf(out, size, ROOT_PATH "/%s/", kind);
    for (const char* p = id; *p && (size_t)n + 4 < size; p++) {
        if (isalnum((unsigned char)*p) && !(p == id && isdigit((unsigned char)*p))) out[n++] = *p;
        else n += snprintf(out + n, size - (size_t)n, "_%02x", (unsigned char)*p);
    }
    out[n] = 0; return out;
}
static const char* session_path(const struct session* s) { static char p[2][96]; static int k; k ^= 1; return path_of(p[k], sizeof(p[k]), "session", s->id); }
static const char* user_path(unsigned uid) { static char p[2][96]; static int k; k ^= 1; snprintf(p[k], sizeof(p[k]), ROOT_PATH "/user/_%u", uid); return p[k]; }
static const char* seat_path(void) { return ROOT_PATH "/seat/seat0"; }

/* ---------------------------------------------------------------- signals */
static void emit(const char* path, const char* iface, const char* name, int first, ...) {
    DBusMessage* m = dbus_message_new_signal(path, iface, name); if (!m) return;
    va_list list; va_start(list, first);
    if (first != DBUS_TYPE_INVALID) dbus_message_append_args_valist(m, first, list);
    va_end(list); dbus_connection_send(bus, m, NULL); dbus_message_unref(m);
}
static bool append_property(DBusMessageIter* it, const char* iface, const char* name, struct session* s, unsigned uid);
static void properties_changed(const char* path, const char* iface, struct session* s, unsigned uid, const char* const* names) {
    DBusMessage* m = dbus_message_new_signal(path, PROPS, "PropertiesChanged"); if (!m) return;
    DBusMessageIter it, dict, entry, empty; dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    for (const char* const* n = names; *n; n++) {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, n); append_property(&entry, iface, *n, s, uid);
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_close_container(&it, &dict);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &empty); dbus_message_iter_close_container(&it, &empty);
    dbus_connection_send(bus, m, NULL); dbus_message_unref(m);
}

/* ---------------------------------------------------------------- the seat from Aurora */
static struct session* by_id(const char* id) { for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && !strcmp(sessions[i].id, id)) return &sessions[i]; return NULL; }
static struct session* active_session(void) { for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].active) return &sessions[i]; return NULL; }
static bool user_has_sessions(unsigned uid, const struct session* except) { for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && &sessions[i] != except && sessions[i].uid == uid) return true; return false; }
static unsigned session_count(void) { unsigned n = 0; for (int i = 0; i < MAX_SESSIONS; i++) n += sessions[i].used; return n; }
static void device_pause(struct session* s, bool resume);
static void refresh(void) {
    char reply[8192];
    if (!aurora_request("sessions\n", reply, sizeof(reply))) {
        if (aurora_seen) logline("Aurora does not answer on %s", AURORA_CONTROL);
        return;
    }
    aurora_seen = true;
    for (int i = 0; i < MAX_SESSIONS; i++) sessions[i].seen = false;
    for (char* save = NULL, *line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        struct session n; memset(&n, 0, sizeof(n)); int active = 0, locked = 0; unsigned long long since = 0; char id[16] = "", user[33] = "", state[16] = "", home[129] = "";
        if (sscanf(line, "id=%15s uid=%u gid=%u user=%32s leader=%ld state=%15s active=%d locked=%d since=%llu home=%128s", id, &n.uid, &n.gid, user, &n.leader, state, &active, &locked, &since, home) != 10) {
            unsigned a; if (sscanf(line, "seat active_uid=%u", &a) == 1) {} continue; }
        bool digits = id[0] != 0; for (char* p = id; *p; p++) if (!isdigit((unsigned char)*p)) digits = false;
        if (!digits || n.leader <= 0 || proc_uid(n.leader) != (long)n.uid) { logline("ignored session %s: leader %ld is not uid %u", id, n.leader, n.uid); continue; }
        if (!strcmp(id, "1")) boot_uid = n.uid;
        struct session* s = by_id(id); bool fresh = !s;
        if (!s) { for (int i = 0; i < MAX_SESSIONS && !s; i++) if (!sessions[i].used) s = &sessions[i]; if (!s) continue;
            memset(s, 0, sizeof(*s)); s->used = true; snprintf(s->id, sizeof(s->id), "%s", id); s->uid = n.uid; s->gid = n.gid;
            snprintf(s->user, sizeof(s->user), "%s", user); snprintf(s->home, sizeof(s->home), "%s", home); s->leader = n.leader;
            s->since_us = since ? since : now_us(CLOCK_REALTIME); s->since_mono = now_us(CLOCK_MONOTONIC); for (int d = 0; d < MAX_DEVICES; d++) s->devices[d].fd = -1; }
        s->seen = true;
        bool was_active = s->active, was_locked = s->locked;
        s->active = active != 0; s->locked = s->locked_override ? s->locked_hint : locked != 0; snprintf(s->state, sizeof(s->state), "%s", state);
        if (fresh) {
            bool new_user = !user_has_sessions(s->uid, s);
            const char* p = session_path(s); const char* idp = s->id; emit(ROOT_PATH, MANAGER, "SessionNew", DBUS_TYPE_STRING, &idp, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
            if (new_user) { dbus_uint32_t u = s->uid; const char* up = user_path(s->uid); emit(ROOT_PATH, MANAGER, "UserNew", DBUS_TYPE_UINT32, &u, DBUS_TYPE_OBJECT_PATH, &up, DBUS_TYPE_INVALID); }
            logline("session %s uid %u (%s) leader %ld", s->id, s->uid, s->user, s->leader);
        } else {
            if (was_active != s->active) { static const char* const n2[] = {"Active", "State", NULL}; properties_changed(session_path(s), SESSION_IF, s, s->uid, n2); device_pause(s, s->active); }
            if (was_locked != s->locked) { static const char* const n3[] = {"LockedHint", NULL}; properties_changed(session_path(s), SESSION_IF, s, s->uid, n3); }
        }
        if (fresh || was_active != s->active) { static const char* const seat[] = {"ActiveSession", "Sessions", NULL}; properties_changed(seat_path(), SEAT_IF, NULL, 0, seat);
            static const char* const user_props[] = {"State", "Display", "Sessions", NULL}; properties_changed(user_path(s->uid), USER_IF, NULL, s->uid, user_props); }
    }
    for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && !sessions[i].seen) {
        struct session* s = &sessions[i]; const char* p = session_path(s); const char* idp = s->id;
        for (int d = 0; d < MAX_DEVICES; d++) if (s->devices[d].used && s->devices[d].fd >= 0) close(s->devices[d].fd);
        unsigned uid = s->uid; char id[16]; snprintf(id, sizeof(id), "%s", s->id);
        emit(ROOT_PATH, MANAGER, "SessionRemoved", DBUS_TYPE_STRING, &idp, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
        s->used = false;
        if (!user_has_sessions(uid, NULL)) { dbus_uint32_t u = uid; const char* up = user_path(uid); emit(ROOT_PATH, MANAGER, "UserRemoved", DBUS_TYPE_UINT32, &u, DBUS_TYPE_OBJECT_PATH, &up, DBUS_TYPE_INVALID); }
        static const char* const seat[] = {"ActiveSession", "Sessions", NULL}; properties_changed(seat_path(), SEAT_IF, NULL, 0, seat);
        logline("session %s of uid %u ended", id, uid);
    }
}
/* The session a process belongs to: its ancestry up to a session leader (the agent; pid 1 = the desktop user's
 * session), else XDG_SESSION_ID in its environment; always only a session of the process's own user. */
static struct session* session_of_pid(long pid) {
    long uid = proc_uid(pid); if (uid < 0) return NULL;
    long p = pid;
    for (int hops = 0; hops < 64 && p > 0; hops++) {
        for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].leader == p && sessions[i].uid == (unsigned)uid) return &sessions[i];
        if (p == 1) break;
        p = proc_parent(p);
    }
    char path[64]; snprintf(path, sizeof(path), "/proc/%ld/environ", pid); FILE* f = fopen(path, "re");
    if (f) { char env[16384]; size_t n = fread(env, 1, sizeof(env) - 1, f); fclose(f); env[n] = 0;
        for (size_t at = 0; at < n; at += strlen(env + at) + 1) if (!strncmp(env + at, "XDG_SESSION_ID=", 15)) {
            struct session* s = by_id(env + at + 15); if (s && s->uid == (unsigned)uid) return s; } }
    return NULL;
}

/* ---------------------------------------------------------------- callers and authorization */
struct caller { const char* sender; unsigned uid; long pid; struct session* session; };
static bool identify(DBusMessage* m, struct caller* c) {
    c->sender = dbus_message_get_sender(m); c->pid = -1; c->session = NULL; if (!c->sender) return false;
    DBusError e; dbus_error_init(&e);
    unsigned long uid = dbus_bus_get_unix_user(bus, c->sender, &e); if (dbus_error_is_set(&e)) { dbus_error_free(&e); return false; }
    c->uid = (unsigned)uid;
    DBusMessage* q = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID");
    if (q && dbus_message_append_args(q, DBUS_TYPE_STRING, &c->sender, DBUS_TYPE_INVALID)) {
        DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, q, 2000, &e); dbus_uint32_t pid = 0;
        if (r && dbus_message_get_args(r, &e, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID)) c->pid = pid;
        if (r) dbus_message_unref(r);
        dbus_error_free(&e);
    }
    if (q) dbus_message_unref(q);
    if (c->pid > 0) { c->session = session_of_pid(c->pid); if (c->session && c->session->uid != c->uid) c->session = NULL; }
    return true;
}
/* polkit-like defaults (see the header); "challenge" means an administrator's authentication would be needed. */
static int authorize(const char* action, const struct caller* c) {
    if (c->uid == 0) return AUTH_YES;
    bool in_active = c->session && c->session->active;
    if (!strcmp(action, "power") || !strcmp(action, "sleep")) return in_active ? AUTH_YES : AUTH_CHALLENGE;
    if (!strcmp(action, "multiple-sessions") || !strcmp(action, "ignore-inhibit") || !strcmp(action, "manage")) return AUTH_CHALLENGE;
    if (!strcmp(action, "inhibit")) return c->session || user_has_sessions(c->uid, NULL) ? AUTH_YES : AUTH_CHALLENGE;
    if (!strcmp(action, "chvt")) return in_active ? AUTH_YES : AUTH_CHALLENGE;
    return AUTH_NO;
}

/* ---------------------------------------------------------------- inhibitors */
static unsigned inhibited(bool delay, unsigned except_uid, bool use_except) {
    unsigned mask = 0;
    for (int i = 0; i < MAX_INHIBITORS; i++) if (inhibitors[i].used && inhibitors[i].delay == delay && (!use_except || inhibitors[i].uid != except_uid)) mask |= inhibitors[i].what;
    return mask;
}
static void what_text(unsigned mask, char* out, size_t size) {
    out[0] = 0; for (unsigned i = 0; i < 8; i++) if (mask & (1U << i)) { if (out[0]) strncat(out, ":", size - strlen(out) - 1); strncat(out, WHAT_NAMES[i], size - strlen(out) - 1); }
}
static void inhibitors_changed(void) {
    static const char* const names[] = {"BlockInhibited", "DelayInhibited", "NCurrentInhibitors", NULL}; properties_changed(ROOT_PATH, MANAGER, NULL, 0, names);
    unsigned idle = 0; for (int i = 0; i < MAX_INHIBITORS; i++) idle += inhibitors[i].used && !inhibitors[i].delay && (inhibitors[i].what & W_IDLE);
    if (idle != idle_sent) { char line[48]; snprintf(line, sizeof(line), "inhibit idle=%u\n", idle); if (aurora_request(line, NULL, 0)) idle_sent = idle; }
}
static void release_inhibitor(struct inhibitor* in) {
    char what[160]; what_text(in->what, what, sizeof(what));
    logline("inhibitor released: %s by %s (uid %u, %s)", what, in->who, in->uid, in->delay ? "delay" : "block");
    close(in->fd); memset(in, 0, sizeof(*in)); in->fd = -1; inhibitors_changed();
}

/* ---------------------------------------------------------------- sleep and shutdown */
static bool state_supports(const char* word) {
    char text[128] = ""; int fd = open("/sys/power/state", O_RDONLY | O_CLOEXEC); if (fd < 0) return false;
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd); if (n <= 0) return false; text[n] = 0;
    for (char* save = NULL, *w = strtok_r(text, " \n", &save); w; w = strtok_r(NULL, " \n", &save)) if (!strcmp(w, word)) return true;
    return false;
}
static void announce_sleep(bool start) {
    preparing_sleep = start; dbus_bool_t b = start; emit(ROOT_PATH, MANAGER, "PrepareForSleep", DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID);
    static const char* const names[] = {"PreparingForSleep", NULL}; properties_changed(ROOT_PATH, MANAGER, NULL, 0, names);
    dbus_connection_flush(bus); logline("PrepareForSleep(%s)", start ? "true" : "false");
}
static void announce_shutdown(bool start) {
    preparing_shutdown = start; dbus_bool_t b = start; emit(ROOT_PATH, MANAGER, "PrepareForShutdown", DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID);
    static const char* const names[] = {"PreparingForShutdown", NULL}; properties_changed(ROOT_PATH, MANAGER, NULL, 0, names);
    dbus_connection_flush(bus); logline("PrepareForShutdown(%s)", start ? "true" : "false");
}
static void execute_pending(void) {
    int op = pending_op; pending_op = 0;
    if (op == 1 || op == 2) {
        const char* state = op == 1 ? (state_supports("mem") ? "mem" : "freeze") : "disk";
        our_sleep = true; pid_t pid = fork();
        if (!pid) { int fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC); if (fd < 0) _exit(errno & 127);
            ssize_t n = write(fd, state, strlen(state)); _exit(n == (ssize_t)strlen(state) ? 0 : (errno & 127)); }
        if (pid < 0) { our_sleep = false; announce_sleep(false); return; }
        sleep_child = pid; logline("sleeping: %s", state);
    } else if (op == 3 || op == 4) {
        logline("%s: asking Aurora for its orderly shutdown", op == 3 ? "power off" : "reboot");
        if (!aurora_request(op == 3 ? "power-off\n" : "reboot\n", NULL, 0)) { logline("Aurora did not answer: the kernel shuts down"); sync(); reboot(op == 3 ? RB_POWER_OFF : RB_AUTOBOOT); }
    }
}
static void delay_check(void) {   /* the pending operation goes once its delay inhibitors are gone or the time is up */
    if (!pending_op) return;
    unsigned w = pending_op <= 2 ? W_SLEEP : W_SHUTDOWN;
    if (!(inhibited(true, 0, false) & w) || now_us(CLOCK_MONOTONIC) >= pending_deadline) execute_pending();
}
static void sleep_finished(int status) {
    sleep_child = 0;
    logline("sleep ended (%s %d)", WIFEXITED(status) && !WEXITSTATUS(status) ? "resumed" : "failed: errno", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    announce_sleep(false); our_sleep = false; our_sleep_until = now_us(CLOCK_MONOTONIC) + 3000000;
}
/* the kernel's PM notifications: sleeps that did not go through us (Aurora's menu, keys, the lid) */
static void uevent_input(int fd) {
    for (;;) {
        char buf[2048]; ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT); if (n <= 0) return; buf[n] = 0;
        bool power = false; const char* event = NULL;
        for (ssize_t at = 0; at < n; at += (ssize_t)strlen(buf + at) + 1) { if (!strcmp(buf + at, "SUBSYSTEM=power")) power = true; if (!strncmp(buf + at, "PM_EVENT=", 9)) event = buf + at + 9; }
        if (!power || !event) continue;
        if (!strcmp(event, "shutdown-prepare")) { if (!preparing_shutdown) announce_shutdown(true); continue; }
        bool prepare = strstr(event, "-prepare") != NULL, post = !strncmp(event, "post-", 5);
        if (our_sleep || now_us(CLOCK_MONOTONIC) < our_sleep_until) continue;   /* announced by us */
        if (prepare && !preparing_sleep) announce_sleep(true);
        else if (post && preparing_sleep) announce_sleep(false);
    }
}

/* ---------------------------------------------------------------- properties */
static void variant(DBusMessageIter* it, int type, const char* sig, const void* value) {
    DBusMessageIter v; dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &v); dbus_message_iter_append_basic(&v, type, value); dbus_message_iter_close_container(it, &v);
}
static void v_str(DBusMessageIter* it, const char* s) { variant(it, DBUS_TYPE_STRING, "s", &s); }
static void v_bool(DBusMessageIter* it, bool b) { dbus_bool_t x = b; variant(it, DBUS_TYPE_BOOLEAN, "b", &x); }
static void v_u32(DBusMessageIter* it, uint32_t u) { variant(it, DBUS_TYPE_UINT32, "u", &u); }
static void v_u64(DBusMessageIter* it, uint64_t u) { dbus_uint64_t x = u; variant(it, DBUS_TYPE_UINT64, "t", &x); }
static void v_so(DBusMessageIter* it, const char* id, const char* path) {
    DBusMessageIter v, st; dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "(so)", &v); dbus_message_iter_open_container(&v, DBUS_TYPE_STRUCT, NULL, &st);
    dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &id); dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_close_container(&v, &st); dbus_message_iter_close_container(it, &v);
}
static void v_uo(DBusMessageIter* it, uint32_t uid, const char* path) {
    DBusMessageIter v, st; dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "(uo)", &v); dbus_message_iter_open_container(&v, DBUS_TYPE_STRUCT, NULL, &st);
    dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &uid); dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_close_container(&v, &st); dbus_message_iter_close_container(it, &v);
}
static void v_sessions(DBusMessageIter* it, unsigned uid, bool all) {   /* a(so) */
    DBusMessageIter v, arr, st; dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a(so)", &v); dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(so)", &arr);
    for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && (all || sessions[i].uid == uid)) {
        const char* id = sessions[i].id; const char* p = session_path(&sessions[i]);
        dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st); dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &id);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &p); dbus_message_iter_close_container(&arr, &st);
    }
    dbus_message_iter_close_container(&v, &arr); dbus_message_iter_close_container(it, &v);
}
static bool on_external_power(void) {
    DIR* d = opendir("/sys/class/power_supply"); if (!d) return true;
    struct dirent* e; bool mains = false, online = false, battery = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue; char path[300], text[32] = ""; FILE* f;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name); f = fopen(path, "re"); if (!f) continue;
        if (fgets(text, sizeof(text), f)) {} fclose(f);
        if (!strncmp(text, "Battery", 7)) battery = true;
        if (!strncmp(text, "Mains", 5)) { mains = true; snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", e->d_name); f = fopen(path, "re");
            if (f) { if (fgets(text, sizeof(text), f) && text[0] == '1') online = true; fclose(f); } }
    }
    closedir(d); return online || !battery || !mains;
}
static const char* const MANAGER_PROPS[] = {"IdleHint", "IdleSinceHint", "IdleSinceHintMonotonic", "BlockInhibited", "DelayInhibited", "InhibitDelayMaxUSec",
    "PreparingForShutdown", "PreparingForSleep", "NCurrentSessions", "NCurrentInhibitors", "InhibitorsMax", "SessionsMax", "KillUserProcesses", "NAutoVTs",
    "OnExternalPower", "Docked", "LidClosed", "HandlePowerKey", "HandleSuspendKey", "HandleHibernateKey", "HandleLidSwitch", "IdleAction", "RemoveIPC", NULL};
static const char* const SESSION_PROPS[] = {"Id", "User", "Name", "Timestamp", "TimestampMonotonic", "VTNr", "Seat", "TTY", "Display", "Remote", "RemoteHost",
    "RemoteUser", "Service", "Desktop", "Scope", "Leader", "Audit", "Type", "Class", "Active", "State", "IdleHint", "IdleSinceHint", "IdleSinceHintMonotonic", "LockedHint", NULL};
static const char* const USER_PROPS[] = {"UID", "GID", "Name", "Timestamp", "TimestampMonotonic", "RuntimePath", "Service", "Slice", "Display", "State", "Sessions", "IdleHint", "IdleSinceHint", "IdleSinceHintMonotonic", "Linger", NULL};
static const char* const SEAT_PROPS[] = {"Id", "ActiveSession", "CanMultiSession", "CanTTY", "CanGraphical", "Sessions", "IdleHint", "IdleSinceHint", "IdleSinceHintMonotonic", NULL};
static bool append_property(DBusMessageIter* it, const char* iface, const char* n, struct session* s, unsigned uid) {
    char text[160]; struct session* a = active_session();
    if (!strcmp(iface, MANAGER)) {
        if (!strcmp(n, "IdleHint")) v_bool(it, a && a->idle);
        else if (!strcmp(n, "IdleSinceHint")) v_u64(it, a ? a->idle_since_us : 0);
        else if (!strcmp(n, "IdleSinceHintMonotonic")) v_u64(it, 0);
        else if (!strcmp(n, "BlockInhibited")) { what_text(inhibited(false, 0, false), text, sizeof(text)); v_str(it, text); }
        else if (!strcmp(n, "DelayInhibited")) { what_text(inhibited(true, 0, false), text, sizeof(text)); v_str(it, text); }
        else if (!strcmp(n, "InhibitDelayMaxUSec")) v_u64(it, DELAY_MAX_US);
        else if (!strcmp(n, "PreparingForShutdown")) v_bool(it, preparing_shutdown);
        else if (!strcmp(n, "PreparingForSleep")) v_bool(it, preparing_sleep);
        else if (!strcmp(n, "NCurrentSessions")) v_u64(it, session_count());
        else if (!strcmp(n, "NCurrentInhibitors")) { uint64_t c = 0; for (int i = 0; i < MAX_INHIBITORS; i++) c += inhibitors[i].used; v_u64(it, c); }
        else if (!strcmp(n, "InhibitorsMax")) v_u64(it, MAX_INHIBITORS);
        else if (!strcmp(n, "SessionsMax")) v_u64(it, MAX_SESSIONS);
        else if (!strcmp(n, "KillUserProcesses")) v_bool(it, true);
        else if (!strcmp(n, "NAutoVTs")) v_u32(it, 0);
        else if (!strcmp(n, "OnExternalPower")) v_bool(it, on_external_power());
        else if (!strcmp(n, "Docked") || !strcmp(n, "LidClosed") || !strcmp(n, "RemoveIPC")) v_bool(it, !strcmp(n, "RemoveIPC"));
        else if (!strcmp(n, "HandlePowerKey")) v_str(it, "poweroff");
        else if (!strcmp(n, "HandleSuspendKey") || !strcmp(n, "HandleLidSwitch")) v_str(it, "suspend");
        else if (!strcmp(n, "HandleHibernateKey")) v_str(it, "hibernate");
        else if (!strcmp(n, "IdleAction")) v_str(it, "ignore");
        else return false;
        return true;
    }
    if (!strcmp(iface, SESSION_IF) && s) {
        if (!strcmp(n, "Id")) v_str(it, s->id);
        else if (!strcmp(n, "User")) v_uo(it, s->uid, user_path(s->uid));
        else if (!strcmp(n, "Name")) v_str(it, s->user);
        else if (!strcmp(n, "Timestamp")) v_u64(it, s->since_us);
        else if (!strcmp(n, "TimestampMonotonic")) v_u64(it, s->since_mono);
        else if (!strcmp(n, "VTNr")) v_u32(it, 0);
        else if (!strcmp(n, "Seat")) v_so(it, "seat0", seat_path());
        else if (!strcmp(n, "TTY") || !strcmp(n, "RemoteHost") || !strcmp(n, "RemoteUser") || !strcmp(n, "Display")) v_str(it, "");
        else if (!strcmp(n, "Remote")) v_bool(it, false);
        else if (!strcmp(n, "Service")) v_str(it, "aurora");
        else if (!strcmp(n, "Desktop")) v_str(it, "aurora");
        else if (!strcmp(n, "Scope")) { snprintf(text, sizeof(text), "session-%s.scope", s->id); v_str(it, text); }
        else if (!strcmp(n, "Leader")) v_u32(it, (uint32_t)s->leader);
        else if (!strcmp(n, "Audit")) v_u32(it, 0);
        else if (!strcmp(n, "Type")) v_str(it, "wayland");
        else if (!strcmp(n, "Class")) v_str(it, "user");
        else if (!strcmp(n, "Active")) v_bool(it, s->active);
        else if (!strcmp(n, "State")) v_str(it, s->state[0] ? s->state : "online");
        else if (!strcmp(n, "IdleHint")) v_bool(it, s->idle);
        else if (!strcmp(n, "IdleSinceHint")) v_u64(it, s->idle_since_us);
        else if (!strcmp(n, "IdleSinceHintMonotonic")) v_u64(it, 0);
        else if (!strcmp(n, "LockedHint")) v_bool(it, s->locked);
        else return false;
        return true;
    }
    if (!strcmp(iface, USER_IF)) {
        struct session* first = NULL; for (int i = 0; i < MAX_SESSIONS && !first; i++) if (sessions[i].used && sessions[i].uid == uid) first = &sessions[i];
        if (!first) return false;
        if (!strcmp(n, "UID")) v_u32(it, uid);
        else if (!strcmp(n, "GID")) v_u32(it, first->gid);
        else if (!strcmp(n, "Name")) v_str(it, first->user);
        else if (!strcmp(n, "Timestamp")) v_u64(it, first->since_us);
        else if (!strcmp(n, "TimestampMonotonic")) v_u64(it, first->since_mono);
        else if (!strcmp(n, "RuntimePath")) { snprintf(text, sizeof(text), "/run/user/%u", uid); v_str(it, text); }
        else if (!strcmp(n, "Service")) { snprintf(text, sizeof(text), "user@%u.service", uid); v_str(it, text); }
        else if (!strcmp(n, "Slice")) { snprintf(text, sizeof(text), "user-%u.slice", uid); v_str(it, text); }
        else if (!strcmp(n, "Display")) v_so(it, first->id, session_path(first));
        else if (!strcmp(n, "State")) v_str(it, a && a->uid == uid ? "active" : "online");
        else if (!strcmp(n, "Sessions")) v_sessions(it, uid, false);
        else if (!strcmp(n, "IdleHint")) v_bool(it, first->idle);
        else if (!strcmp(n, "IdleSinceHint") || !strcmp(n, "IdleSinceHintMonotonic")) v_u64(it, 0);
        else if (!strcmp(n, "Linger")) v_bool(it, false);
        else return false;
        return true;
    }
    if (!strcmp(iface, SEAT_IF)) {
        if (!strcmp(n, "Id")) v_str(it, "seat0");
        else if (!strcmp(n, "ActiveSession")) v_so(it, a ? a->id : "", a ? session_path(a) : "/");
        else if (!strcmp(n, "CanMultiSession") || !strcmp(n, "CanGraphical")) v_bool(it, true);
        else if (!strcmp(n, "CanTTY")) v_bool(it, false);
        else if (!strcmp(n, "Sessions")) v_sessions(it, 0, true);
        else if (!strcmp(n, "IdleHint")) v_bool(it, a && a->idle);
        else if (!strcmp(n, "IdleSinceHint") || !strcmp(n, "IdleSinceHintMonotonic")) v_u64(it, 0);
        else return false;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- method helpers */
static DBusHandlerResult reply_error(DBusMessage* m, const char* name, const char* format, ...) {
    char text[300]; va_list list; va_start(list, format); vsnprintf(text, sizeof(text), format, list); va_end(list);
    DBusMessage* r = dbus_message_new_error(m, name, text); if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult reply_empty(DBusMessage* m) { DBusMessage* r = dbus_message_new_method_return(m); if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); } return DBUS_HANDLER_RESULT_HANDLED; }
static DBusHandlerResult reply_path(DBusMessage* m, const char* path) {
    DBusMessage* r = dbus_message_new_method_return(m); if (r) { dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult reply_string(DBusMessage* m, const char* text) {
    DBusMessage* r = dbus_message_new_method_return(m); if (r) { dbus_message_append_args(r, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static bool args(DBusMessage* m, int first, ...) {   /* exact signature match */
    DBusError e; dbus_error_init(&e); va_list list; va_start(list, first);
    bool ok = dbus_message_get_args_valist(m, &e, first, list); va_end(list); dbus_error_free(&e); return ok;
}
static bool exact(DBusMessage* m, const char* signature) { return !strcmp(dbus_message_get_signature(m), signature); }
static struct session* session_arg(const char* id, const struct caller* c) {   /* "", "self", "auto": the caller's */
    if (!id[0] || !strcmp(id, "self")) return c->session;
    if (!strcmp(id, "auto")) {   /* the caller's session, else its user's graphical one (the active one first) */
        if (c->session) return c->session;
        struct session* a = active_session(); if (a && a->uid == c->uid) return a;
        for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].uid == c->uid) return &sessions[i];
        return NULL;
    }
    return by_id(id);
}
static DBusHandlerResult auth_error(DBusMessage* m, int verdict, const char* what) {
    return verdict == AUTH_CHALLENGE ? reply_error(m, E_INTERACTIVE, "Interactive authentication required to %s (no authentication agent).", what)
                                     : reply_error(m, E_ACCESS, "Access denied: %s", what);
}
static bool own_or_root(const struct caller* c, const struct session* s) { return c->uid == 0 || (s && s->uid == c->uid); }
static int sleep_or_shutdown_verdict(const struct caller* c, unsigned w, const char* base) {
    int v = authorize(base, c);
    bool others = false; for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].uid != c->uid) others = true;
    if (v == AUTH_YES && others) v = authorize("multiple-sessions", c);
    if (v == AUTH_YES && (inhibited(false, c->uid, true) & w)) v = authorize("ignore-inhibit", c);
    return v;
}
static const char* can_text(const struct caller* c, int op) {
    if ((op == 1 && !state_supports("mem") && !state_supports("freeze")) || (op == 2 && !state_supports("disk")) || op >= 5) return "na";
    int v = sleep_or_shutdown_verdict(c, op <= 2 ? W_SLEEP : W_SHUTDOWN, op <= 2 ? "sleep" : "power");
    return v == AUTH_YES ? "yes" : v == AUTH_CHALLENGE ? "challenge" : "no";
}
static DBusHandlerResult start_operation(DBusMessage* m, const struct caller* c, int op, const char* name) {
    if ((op == 1 && !state_supports("mem") && !state_supports("freeze")) || (op == 2 && !state_supports("disk"))) return reply_error(m, E_NOTSUP, "%s is not supported on this machine", name);
    if (pending_op || sleep_child) return reply_error(m, E_BUSY, "There is already a shutdown or sleep operation in progress");
    unsigned w = op <= 2 ? W_SLEEP : W_SHUTDOWN;
    int v = sleep_or_shutdown_verdict(c, w, op <= 2 ? "sleep" : "power");
    if (v != AUTH_YES) {
        if (inhibited(false, c->uid, true) & w) return reply_error(m, E_INHIBITED, "Operation inhibited by a block inhibitor of another user");
        return auth_error(m, v, name);
    }
    logline("%s requested by uid %u pid %ld", name, c->uid, c->pid);
    reply_empty(m); dbus_connection_flush(bus);
    pending_op = op; pending_deadline = now_us(CLOCK_MONOTONIC) + DELAY_MAX_US;
    if (op <= 2) { our_sleep = true; announce_sleep(true); } else announce_shutdown(true);
    delay_check();
    return DBUS_HANDLER_RESULT_HANDLED;
}
static bool parse_what(const char* text, unsigned* mask) {
    *mask = 0; if (!text[0] || strlen(text) > 200) return false;
    char copy[256]; snprintf(copy, sizeof(copy), "%s", text);
    for (char* save = NULL, *w = strtok_r(copy, ":", &save); w; w = strtok_r(NULL, ":", &save)) {
        bool known = false; for (unsigned i = 0; i < 8; i++) if (!strcmp(w, WHAT_NAMES[i])) { *mask |= 1U << i; known = true; }
        if (!known) return false;
    }
    return *mask != 0;
}
static bool printable(const char* text, size_t max) { size_t n = 0; for (const unsigned char* p = (const unsigned char*)text; *p; p++, n++) if (*p < 0x20 || n >= max) return false; return true; }
static DBusHandlerResult method_inhibit(DBusMessage* m, const struct caller* c) {
    const char *what, *who, *why, *mode;
    if (!exact(m, "ssss") || !args(m, DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who, DBUS_TYPE_STRING, &why, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "Inhibit(ssss)");
    unsigned mask; if (!parse_what(what, &mask)) return reply_error(m, E_ARGS, "Invalid what specification %.80s", what);
    bool delay = !strcmp(mode, "delay"); if (!delay && strcmp(mode, "block") && strcmp(mode, "block-weak")) return reply_error(m, E_ARGS, "Unknown mode %.40s", mode);
    if (delay && (mask & ~(W_SLEEP | W_SHUTDOWN))) return reply_error(m, E_ARGS, "Delay inhibitors only supported for shutdown and sleep");
    if (!printable(who, 255) || !printable(why, 255)) return reply_error(m, E_ARGS, "who/why: printable text up to 255 bytes");
    int v = authorize("inhibit", c); if (v != AUTH_YES) return auth_error(m, v, "inhibit");
    unsigned total = 0, mine = 0; struct inhibitor* slot = NULL;
    for (int i = 0; i < MAX_INHIBITORS; i++) { if (inhibitors[i].used) { total++; mine += inhibitors[i].uid == c->uid; } else if (!slot) slot = &inhibitors[i]; }
    if (!slot || mine >= MAX_PER_UID) return reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "Maximum number of inhibitors (%u per user) reached", (unsigned)MAX_PER_UID);
    if (!dbus_connection_can_send_type(bus, DBUS_TYPE_UNIX_FD)) return reply_error(m, E_NOTSUP, "the bus connection cannot pass file descriptors");
    int p[2]; if (pipe2(p, O_CLOEXEC)) return reply_error(m, "org.freedesktop.DBus.Error.Failed", "pipe: %s", strerror(errno));
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    DBusMessage* r = dbus_message_new_method_return(m);
    if (!r || !dbus_message_append_args(r, DBUS_TYPE_UNIX_FD, &p[1], DBUS_TYPE_INVALID)) { if (r) dbus_message_unref(r); close(p[0]); close(p[1]); return reply_error(m, "org.freedesktop.DBus.Error.NoMemory", "no memory"); }
    close(p[1]);   /* libdbus duplicated it into the message */
    memset(slot, 0, sizeof(*slot)); slot->used = true; slot->what = mask; slot->delay = delay; slot->uid = c->uid; slot->pid = (unsigned)(c->pid > 0 ? c->pid : 0); slot->fd = p[0];
    snprintf(slot->who, sizeof(slot->who), "%s", who); snprintf(slot->why, sizeof(slot->why), "%s", why);
    dbus_connection_send(bus, r, NULL); dbus_message_unref(r);
    logline("inhibitor taken: %s by %s (uid %u pid %u, %s): %s", what, who, c->uid, slot->pid, mode, why);
    inhibitors_changed(); (void)total;
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult list_reply(DBusMessage* m, int kind) {   /* 0 sessions a(susso), 1 users a(uso), 2 seats a(so), 3 inhibitors a(ssssuu) */
    DBusMessage* r = dbus_message_new_method_return(m); if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    static const char* const sigs[] = {"(susso)", "(uso)", "(so)", "(ssssuu)"};
    DBusMessageIter it, arr, st; dbus_message_iter_init_append(r, &it); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, sigs[kind], &arr);
    if (kind == 0) for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used) {
        struct session* s = &sessions[i]; const char *id = s->id, *user = s->user, *seat = "seat0", *p = session_path(s); dbus_uint32_t uid = s->uid;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st); dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &id); dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &uid);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &user); dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &seat); dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &p);
        dbus_message_iter_close_container(&arr, &st);
    }
    if (kind == 1) for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used) {
        bool first = true; for (int k = 0; k < i; k++) if (sessions[k].used && sessions[k].uid == sessions[i].uid) first = false;
        if (!first) continue;
        dbus_uint32_t uid = sessions[i].uid; const char *user = sessions[i].user, *p = user_path(uid);
        dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st); dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &uid);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &user); dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &p); dbus_message_iter_close_container(&arr, &st);
    }
    if (kind == 2) { const char *seat = "seat0", *p = seat_path(); dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &seat); dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &p); dbus_message_iter_close_container(&arr, &st); }
    if (kind == 3) for (int i = 0; i < MAX_INHIBITORS; i++) if (inhibitors[i].used) {
        char what[160]; what_text(inhibitors[i].what, what, sizeof(what)); const char *w = what, *who = inhibitors[i].who, *why = inhibitors[i].why, *mode = inhibitors[i].delay ? "delay" : "block";
        dbus_uint32_t uid = inhibitors[i].uid, pid = inhibitors[i].pid;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st); dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &w); dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &who);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &why); dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &mode); dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &uid);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &pid); dbus_message_iter_close_container(&arr, &st);
    }
    dbus_message_iter_close_container(&it, &arr); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
}
static void kill_session(struct session* s, bool leader_only, int number) {   /* only processes of the session's user */
    if (leader_only) { if (s->leader > 1 && proc_uid(s->leader) == (long)s->uid) kill((pid_t)s->leader, number); return; }
    DIR* d = opendir("/proc"); if (!d) return; struct dirent* e;
    while ((e = readdir(d))) { char* end; long pid = strtol(e->d_name, &end, 10); if (*end || pid <= 1) continue;
        if (proc_uid(pid) == (long)s->uid && session_of_pid(pid) == s) kill((pid_t)pid, number); }
    closedir(d);
}
static bool session_command(const char* verb, const struct session* s) { char line[64]; snprintf(line, sizeof(line), "%s %s\n", verb, s->id); return aurora_request(line, NULL, 0); }

/* ---------------------------------------------------------------- devices (TakeDevice) */
static int open_device(const struct session* s, unsigned major, unsigned minor) {
    static const char* const nodes[] = {"/dev/input/event0", "/dev/input/event1", "/dev/input/event2", "/dev/input/event3", "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/renderD128"};
    for (unsigned i = 0; i < sizeof(nodes) / sizeof(*nodes); i++) {
        struct stat st; if (stat(nodes[i], &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != major || minor(st.st_rdev) != minor) continue;
        /* opened with the session user's file-system identity: the kernel's seat revokes input readers on a switch */
        setfsuid(s->uid); setfsgid(s->gid);
        int fd = open(nodes[i], (major == 13 ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
        int error = errno; setfsuid(0); setfsgid(0); errno = error; return fd;
    }
    errno = ENODEV; return -1;
}
static void device_pause(struct session* s, bool resume) {
    if (!s->controller[0]) return;
    for (int d = 0; d < MAX_DEVICES; d++) {
        struct device* dev = &s->devices[d]; if (!dev->used) continue;
        dbus_uint32_t major = dev->major, minor = dev->minor;
        if (!resume && !dev->paused) {
            const char* type = "force"; dev->paused = true; if (dev->fd >= 0 && major == 13) { close(dev->fd); dev->fd = -1; }
            DBusMessage* m = dbus_message_new_signal(session_path(s), SESSION_IF, "PauseDevice"); if (!m) continue;
            dbus_message_set_destination(m, s->controller);
            dbus_message_append_args(m, DBUS_TYPE_UINT32, &major, DBUS_TYPE_UINT32, &minor, DBUS_TYPE_STRING, &type, DBUS_TYPE_INVALID);
            dbus_connection_send(bus, m, NULL); dbus_message_unref(m);
        } else if (resume && dev->paused) {
            int fd = open_device(s, dev->major, dev->minor); if (fd < 0) continue;
            dev->paused = false; if (dev->fd >= 0) close(dev->fd); dev->fd = fd;
            DBusMessage* m = dbus_message_new_signal(session_path(s), SESSION_IF, "ResumeDevice"); if (!m) continue;
            dbus_message_set_destination(m, s->controller);
            dbus_message_append_args(m, DBUS_TYPE_UINT32, &major, DBUS_TYPE_UINT32, &minor, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
            dbus_connection_send(bus, m, NULL); dbus_message_unref(m);
        }
    }
}
static void controller_gone(const char* name) {
    for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].controller[0] && !strcmp(sessions[i].controller, name)) {
        for (int d = 0; d < MAX_DEVICES; d++) { if (sessions[i].devices[d].used && sessions[i].devices[d].fd >= 0) close(sessions[i].devices[d].fd); memset(&sessions[i].devices[d], 0, sizeof(sessions[i].devices[d])); sessions[i].devices[d].fd = -1; }
        sessions[i].controller[0] = 0; logline("session %s: controller %s left", sessions[i].id, name);
    }
}

/* ---------------------------------------------------------------- dispatch */
static const char* const INTROSPECT_HEAD = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>"
    "<interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/><method name=\"GetMachineId\"><arg type=\"s\" direction=\"out\"/></method></interface>"
    "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface>"
    "<interface name=\"org.freedesktop.DBus.Properties\"><method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
    "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method><method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
    "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal></interface>";
static const char* const INTROSPECT_MANAGER = "<interface name=\"org.freedesktop.login1.Manager\">"
    "<method name=\"GetSession\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
    "<method name=\"GetSessionByPID\"><arg type=\"u\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
    "<method name=\"GetUser\"><arg type=\"u\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
    "<method name=\"GetUserByPID\"><arg type=\"u\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
    "<method name=\"GetSeat\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
    "<method name=\"ListSessions\"><arg type=\"a(susso)\" direction=\"out\"/></method><method name=\"ListUsers\"><arg type=\"a(uso)\" direction=\"out\"/></method>"
    "<method name=\"ListSeats\"><arg type=\"a(so)\" direction=\"out\"/></method><method name=\"ListInhibitors\"><arg type=\"a(ssssuu)\" direction=\"out\"/></method>"
    "<method name=\"ActivateSession\"><arg type=\"s\" direction=\"in\"/></method><method name=\"ActivateSessionOnSeat\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
    "<method name=\"LockSession\"><arg type=\"s\" direction=\"in\"/></method><method name=\"UnlockSession\"><arg type=\"s\" direction=\"in\"/></method>"
    "<method name=\"LockSessions\"/><method name=\"UnlockSessions\"/><method name=\"TerminateSession\"><arg type=\"s\" direction=\"in\"/></method>"
    "<method name=\"TerminateUser\"><arg type=\"u\" direction=\"in\"/></method><method name=\"KillSession\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
    "<method name=\"KillUser\"><arg type=\"u\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
    "<method name=\"PowerOff\"><arg type=\"b\" direction=\"in\"/></method><method name=\"Reboot\"><arg type=\"b\" direction=\"in\"/></method>"
    "<method name=\"Suspend\"><arg type=\"b\" direction=\"in\"/></method><method name=\"Hibernate\"><arg type=\"b\" direction=\"in\"/></method>"
    "<method name=\"HybridSleep\"><arg type=\"b\" direction=\"in\"/></method><method name=\"SuspendThenHibernate\"><arg type=\"b\" direction=\"in\"/></method>"
    "<method name=\"PowerOffWithFlags\"><arg type=\"t\" direction=\"in\"/></method><method name=\"RebootWithFlags\"><arg type=\"t\" direction=\"in\"/></method>"
    "<method name=\"SuspendWithFlags\"><arg type=\"t\" direction=\"in\"/></method><method name=\"HibernateWithFlags\"><arg type=\"t\" direction=\"in\"/></method>"
    "<method name=\"CanPowerOff\"><arg type=\"s\" direction=\"out\"/></method><method name=\"CanReboot\"><arg type=\"s\" direction=\"out\"/></method>"
    "<method name=\"CanSuspend\"><arg type=\"s\" direction=\"out\"/></method><method name=\"CanHibernate\"><arg type=\"s\" direction=\"out\"/></method>"
    "<method name=\"CanHybridSleep\"><arg type=\"s\" direction=\"out\"/></method><method name=\"CanSuspendThenHibernate\"><arg type=\"s\" direction=\"out\"/></method>"
    "<method name=\"Inhibit\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"h\" direction=\"out\"/></method>"
    "<signal name=\"SessionNew\"><arg type=\"s\"/><arg type=\"o\"/></signal><signal name=\"SessionRemoved\"><arg type=\"s\"/><arg type=\"o\"/></signal>"
    "<signal name=\"UserNew\"><arg type=\"u\"/><arg type=\"o\"/></signal><signal name=\"UserRemoved\"><arg type=\"u\"/><arg type=\"o\"/></signal>"
    "<signal name=\"SeatNew\"><arg type=\"s\"/><arg type=\"o\"/></signal><signal name=\"PrepareForShutdown\"><arg type=\"b\"/></signal><signal name=\"PrepareForSleep\"><arg type=\"b\"/></signal>"
    "<property name=\"IdleHint\" type=\"b\" access=\"read\"/><property name=\"IdleSinceHint\" type=\"t\" access=\"read\"/><property name=\"BlockInhibited\" type=\"s\" access=\"read\"/>"
    "<property name=\"DelayInhibited\" type=\"s\" access=\"read\"/><property name=\"InhibitDelayMaxUSec\" type=\"t\" access=\"read\"/><property name=\"PreparingForShutdown\" type=\"b\" access=\"read\"/>"
    "<property name=\"PreparingForSleep\" type=\"b\" access=\"read\"/><property name=\"NCurrentSessions\" type=\"t\" access=\"read\"/><property name=\"OnExternalPower\" type=\"b\" access=\"read\"/>"
    "</interface>";
static const char* const INTROSPECT_SESSION = "<interface name=\"org.freedesktop.login1.Session\"><method name=\"Terminate\"/><method name=\"Activate\"/><method name=\"Lock\"/><method name=\"Unlock\"/>"
    "<method name=\"SetIdleHint\"><arg type=\"b\" direction=\"in\"/></method><method name=\"SetLockedHint\"><arg type=\"b\" direction=\"in\"/></method>"
    "<method name=\"Kill\"><arg type=\"s\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
    "<method name=\"TakeControl\"><arg type=\"b\" direction=\"in\"/></method><method name=\"ReleaseControl\"/>"
    "<method name=\"TakeDevice\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"h\" direction=\"out\"/><arg type=\"b\" direction=\"out\"/></method>"
    "<method name=\"ReleaseDevice\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method><method name=\"PauseDeviceComplete\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>"
    "<method name=\"SetBrightness\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>"
    "<signal name=\"PauseDevice\"><arg type=\"u\"/><arg type=\"u\"/><arg type=\"s\"/></signal><signal name=\"ResumeDevice\"><arg type=\"u\"/><arg type=\"u\"/><arg type=\"h\"/></signal>"
    "<signal name=\"Lock\"/><signal name=\"Unlock\"/>"
    "<property name=\"Id\" type=\"s\" access=\"read\"/><property name=\"User\" type=\"(uo)\" access=\"read\"/><property name=\"Name\" type=\"s\" access=\"read\"/><property name=\"Seat\" type=\"(so)\" access=\"read\"/>"
    "<property name=\"Type\" type=\"s\" access=\"read\"/><property name=\"Class\" type=\"s\" access=\"read\"/><property name=\"Active\" type=\"b\" access=\"read\"/><property name=\"State\" type=\"s\" access=\"read\"/>"
    "<property name=\"Leader\" type=\"u\" access=\"read\"/><property name=\"LockedHint\" type=\"b\" access=\"read\"/><property name=\"IdleHint\" type=\"b\" access=\"read\"/></interface>";
static const char* const INTROSPECT_USER = "<interface name=\"org.freedesktop.login1.User\"><method name=\"Terminate\"/><method name=\"Kill\"><arg type=\"i\" direction=\"in\"/></method>"
    "<property name=\"UID\" type=\"u\" access=\"read\"/><property name=\"GID\" type=\"u\" access=\"read\"/><property name=\"Name\" type=\"s\" access=\"read\"/><property name=\"RuntimePath\" type=\"s\" access=\"read\"/>"
    "<property name=\"Display\" type=\"(so)\" access=\"read\"/><property name=\"State\" type=\"s\" access=\"read\"/><property name=\"Sessions\" type=\"a(so)\" access=\"read\"/></interface>";
static const char* const INTROSPECT_SEAT = "<interface name=\"org.freedesktop.login1.Seat\"><method name=\"Terminate\"/><method name=\"ActivateSession\"><arg type=\"s\" direction=\"in\"/></method>"
    "<method name=\"SwitchTo\"><arg type=\"u\" direction=\"in\"/></method><method name=\"SwitchToNext\"/><method name=\"SwitchToPrevious\"/>"
    "<property name=\"Id\" type=\"s\" access=\"read\"/><property name=\"ActiveSession\" type=\"(so)\" access=\"read\"/><property name=\"CanMultiSession\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanGraphical\" type=\"b\" access=\"read\"/><property name=\"Sessions\" type=\"a(so)\" access=\"read\"/></interface>";

/* what object a path names: 0 manager, 1 session, 2 user, 3 seat, 4 inner node, -1 none */
static int object_of(const char* path, const struct caller* c, struct session** s, unsigned* uid) {
    *s = NULL; *uid = 0;
    if (!strcmp(path, ROOT_PATH)) return 0;
    if (!strcmp(path, ROOT_PATH "/session") || !strcmp(path, ROOT_PATH "/user") || !strcmp(path, ROOT_PATH "/seat")) return 4;
    if (!strncmp(path, ROOT_PATH "/session/", sizeof(ROOT_PATH "/session/") - 1)) {
        const char* label = path + sizeof(ROOT_PATH "/session/") - 1;
        if (!strcmp(label, "self") || !strcmp(label, "auto")) { *s = c->session; return *s ? 1 : -1; }
        for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && !strcmp(session_path(&sessions[i]), path)) { *s = &sessions[i]; return 1; }
        return -1;
    }
    if (!strncmp(path, ROOT_PATH "/user/", sizeof(ROOT_PATH "/user/") - 1)) {
        const char* label = path + sizeof(ROOT_PATH "/user/") - 1;
        if (!strcmp(label, "self")) { *uid = c->uid; return user_has_sessions(c->uid, NULL) ? 2 : -1; }
        char* end; if (label[0] != '_') return -1; unsigned long u = strtoul(label + 1, &end, 10);
        if (*end || end == label + 1 || u > 0xFFFFFFFEUL || !user_has_sessions((unsigned)u, NULL)) return -1;
        *uid = (unsigned)u; return 2;
    }
    if (!strcmp(path, ROOT_PATH "/seat/seat0") || !strcmp(path, ROOT_PATH "/seat/self") || !strcmp(path, ROOT_PATH "/seat/auto")) return 3;
    return -1;
}
static DBusHandlerResult properties(DBusMessage* m, int kind, struct session* s, unsigned uid) {
    static const char* const ifaces[] = {MANAGER, SESSION_IF, USER_IF, SEAT_IF};
    static const char* const* const lists[] = {MANAGER_PROPS, SESSION_PROPS, USER_PROPS, SEAT_PROPS};
    const char* member = dbus_message_get_member(m);
    if (!strcmp(member, "Set")) return reply_error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "Properties are read-only");
    if (!strcmp(member, "GetAll")) {
        const char* iface; if (!exact(m, "s") || !args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "GetAll(s)");
        DBusMessage* r = dbus_message_new_method_return(m); if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter it, dict, entry; dbus_message_iter_init_append(r, &it); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        if (!iface[0] || !strcmp(iface, ifaces[kind]))
            for (const char* const* n = lists[kind]; *n; n++) {
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry); dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, n);
                if (!append_property(&entry, ifaces[kind], *n, s, uid)) v_str(&entry, "");
                dbus_message_iter_close_container(&dict, &entry);
            }
        dbus_message_iter_close_container(&it, &dict); dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(member, "Get")) {
        const char *iface, *name; if (!exact(m, "ss") || !args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "Get(ss)");
        if (strcmp(iface, ifaces[kind])) return reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "Unknown interface %.80s", iface);
        DBusMessage* r = dbus_message_new_method_return(m); if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        DBusMessageIter it; dbus_message_iter_init_append(r, &it);
        if (!append_property(&it, iface, name, s, uid)) { dbus_message_unref(r); return reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", "Unknown property %.80s", name); }
        dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method %.60s", member);
}
static DBusHandlerResult manager_method(DBusMessage* m, struct caller* c, const char* member) {
    const char* id; dbus_uint32_t u; dbus_bool_t interactive; dbus_uint64_t flags; dbus_int32_t number; const char* who;
    if (!strcmp(member, "GetSession")) {
        if (!exact(m, "s") || !args(m, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "GetSession(s)");
        struct session* s = session_arg(id, c); if (!s) return reply_error(m, E_NO_SESSION, "No session '%.40s' known", id); return reply_path(m, session_path(s));
    }
    if (!strcmp(member, "GetSessionByPID") || !strcmp(member, "GetUserByPID")) {
        if (!exact(m, "u") || !args(m, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "%s(u)", member);
        long pid = u ? (long)u : c->pid; struct session* s = pid > 0 ? session_of_pid(pid) : NULL;
        if (!s) return reply_error(m, E_NO_PROCESS, "PID %ld does not belong to any known session", pid);
        return reply_path(m, member[3] == 'S' ? session_path(s) : user_path(s->uid));
    }
    if (!strcmp(member, "GetUser")) {
        if (!exact(m, "u") || !args(m, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "GetUser(u)");
        if (!user_has_sessions(u, NULL)) return reply_error(m, E_NO_USER, "User %u is not logged in", (unsigned)u); return reply_path(m, user_path(u));
    }
    if (!strcmp(member, "GetSeat")) {
        if (!exact(m, "s") || !args(m, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "GetSeat(s)");
        if (strcmp(id, "seat0") && strcmp(id, "self") && strcmp(id, "auto") && id[0]) return reply_error(m, E_NO_SEAT, "No seat '%.40s' known", id);
        return reply_path(m, seat_path());
    }
    if (!strcmp(member, "ListSessions")) return list_reply(m, 0);
    if (!strcmp(member, "ListUsers")) return list_reply(m, 1);
    if (!strcmp(member, "ListSeats")) return list_reply(m, 2);
    if (!strcmp(member, "ListInhibitors")) return list_reply(m, 3);
    if (!strcmp(member, "Inhibit")) return method_inhibit(m, c);
    if (!strcmp(member, "ActivateSession") || !strcmp(member, "ActivateSessionOnSeat")) {
        const char* seat = "seat0";
        if (!strcmp(member, "ActivateSession") ? !exact(m, "s") || !args(m, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID) : !exact(m, "ss") || !args(m, DBUS_TYPE_STRING, &id, DBUS_TYPE_STRING, &seat, DBUS_TYPE_INVALID))
            return reply_error(m, E_ARGS, "%s", member);
        if (strcmp(seat, "seat0") && strcmp(seat, "self") && strcmp(seat, "auto") && seat[0]) return reply_error(m, E_NO_SEAT, "No seat '%.40s' known", seat);
        struct session* s = session_arg(id, c); if (!s) return reply_error(m, E_NO_SESSION, "No session '%.40s' known", id);
        int v = authorize("chvt", c); if (v != AUTH_YES && c->uid != s->uid) return auth_error(m, v, "switch sessions");
        if (!s->active && !session_command("session-activate", s)) return reply_error(m, "org.freedesktop.DBus.Error.Failed", "the display server refused");
        refresh(); return reply_empty(m);
    }
    if (!strcmp(member, "LockSession") || !strcmp(member, "UnlockSession") || !strcmp(member, "TerminateSession")) {
        if (!exact(m, "s") || !args(m, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "%s(s)", member);
        struct session* s = session_arg(id, c); if (!s) return reply_error(m, E_NO_SESSION, "No session '%.40s' known", id);
        if (!own_or_root(c, s)) return auth_error(m, authorize(member[0] == 'T' ? "manage" : "lock-sessions", c), member);
        if (member[0] == 'T') { if (!session_command("session-terminate", s)) return reply_error(m, "org.freedesktop.DBus.Error.Failed", "the display server refused"); }
        else { emit(session_path(s), SESSION_IF, member[0] == 'L' ? "Lock" : "Unlock", DBUS_TYPE_INVALID);
            if (member[0] == 'L' && s->active) aurora_request("lock\n", NULL, 0); }
        return reply_empty(m);
    }
    if (!strcmp(member, "LockSessions") || !strcmp(member, "UnlockSessions")) {
        if (!exact(m, "")) return reply_error(m, E_ARGS, "%s()", member);
        if (c->uid) return auth_error(m, authorize("lock-sessions", c), member);
        for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used) emit(session_path(&sessions[i]), SESSION_IF, member[0] == 'L' ? "Lock" : "Unlock", DBUS_TYPE_INVALID);
        if (member[0] == 'L') aurora_request("lock\n", NULL, 0);
        return reply_empty(m);
    }
    if (!strcmp(member, "TerminateUser") || !strcmp(member, "KillUser")) {
        number = SIGTERM;
        if (member[0] == 'T' ? !exact(m, "u") || !args(m, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INVALID) : !exact(m, "ui") || !args(m, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INT32, &number, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "%s", member);
        if (number < 1 || number > 64) return reply_error(m, E_ARGS, "Invalid signal %d", number);
        if (!user_has_sessions(u, NULL)) return reply_error(m, E_NO_USER, "User %u is not logged in", (unsigned)u);
        if (c->uid && c->uid != u) return auth_error(m, authorize("manage", c), member);
        for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].uid == u) { if (member[0] == 'T') session_command("session-terminate", &sessions[i]); else kill_session(&sessions[i], false, number); }
        return reply_empty(m);
    }
    if (!strcmp(member, "KillSession")) {
        if (!exact(m, "ssi") || !args(m, DBUS_TYPE_STRING, &id, DBUS_TYPE_STRING, &who, DBUS_TYPE_INT32, &number, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "KillSession(ssi)");
        if (number < 1 || number > 64 || (strcmp(who, "leader") && strcmp(who, "all"))) return reply_error(m, E_ARGS, "Invalid signal or whom");
        struct session* s = session_arg(id, c); if (!s) return reply_error(m, E_NO_SESSION, "No session '%.40s' known", id);
        if (!own_or_root(c, s)) return auth_error(m, authorize("manage", c), member);
        if (s->leader == 1) return reply_error(m, E_ACCESS, "The display server's session leader is not signalled");
        kill_session(s, !strcmp(who, "leader"), number); return reply_empty(m);
    }
    static const struct { const char* name; int op; bool with_flags; } ops[] = {{"PowerOff", 3, false}, {"Reboot", 4, false}, {"Suspend", 1, false}, {"Hibernate", 2, false},
        {"PowerOffWithFlags", 3, true}, {"RebootWithFlags", 4, true}, {"SuspendWithFlags", 1, true}, {"HibernateWithFlags", 2, true},
        {"HybridSleep", 5, false}, {"SuspendThenHibernate", 6, false}, {"HybridSleepWithFlags", 5, true}, {"SuspendThenHibernateWithFlags", 6, true}, {"Halt", 7, false}};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(*ops); i++) if (!strcmp(member, ops[i].name)) {
        if (ops[i].with_flags ? !exact(m, "t") || !args(m, DBUS_TYPE_UINT64, &flags, DBUS_TYPE_INVALID) : !exact(m, "b") || !args(m, DBUS_TYPE_BOOLEAN, &interactive, DBUS_TYPE_INVALID))
            return reply_error(m, E_ARGS, "%s(%s)", member, ops[i].with_flags ? "t" : "b");
        if (ops[i].with_flags && (flags & ~1ULL)) return reply_error(m, E_ARGS, "Invalid flags parameter");
        if (ops[i].op >= 5) return reply_error(m, E_NOTSUP, "%s is not supported on this machine", member);
        return start_operation(m, c, ops[i].op, member);
    }
    static const struct { const char* name; int op; } cans[] = {{"CanPowerOff", 3}, {"CanReboot", 4}, {"CanSuspend", 1}, {"CanHibernate", 2}, {"CanHybridSleep", 5}, {"CanSuspendThenHibernate", 6}, {"CanHalt", 7}};
    for (unsigned i = 0; i < sizeof(cans) / sizeof(*cans); i++) if (!strcmp(member, cans[i].name)) {
        if (!exact(m, "")) return reply_error(m, E_ARGS, "%s()", member);
        return reply_string(m, cans[i].op == 7 ? "na" : can_text(c, cans[i].op));
    }
    if (!strcmp(member, "SetUserLinger") || !strcmp(member, "ScheduleShutdown") || !strcmp(member, "CancelScheduledShutdown") || !strcmp(member, "AttachDevice") || !strcmp(member, "FlushDevices"))
        return reply_error(m, E_NOTSUP, "%s is not supported by NexusOS", member);
    return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method %.60s", member);
}
static DBusHandlerResult session_method(DBusMessage* m, struct caller* c, struct session* s, const char* member) {
    dbus_bool_t b; dbus_uint32_t major, minor, value; const char *who, *subsystem, *name; dbus_int32_t number;
    bool own = own_or_root(c, s);
    if (!strcmp(member, "Activate")) { if (!exact(m, "")) return reply_error(m, E_ARGS, "Activate()");
        if (!own && authorize("chvt", c) != AUTH_YES) return auth_error(m, AUTH_CHALLENGE, "switch sessions");
        if (!s->active && !session_command("session-activate", s)) return reply_error(m, "org.freedesktop.DBus.Error.Failed", "the display server refused"); refresh(); return reply_empty(m); }
    if (!strcmp(member, "Lock") || !strcmp(member, "Unlock")) { if (!exact(m, "")) return reply_error(m, E_ARGS, "%s()", member);
        if (!own) return auth_error(m, authorize("lock-sessions", c), member);
        emit(session_path(s), SESSION_IF, member, DBUS_TYPE_INVALID); if (member[0] == 'L' && s->active) aurora_request("lock\n", NULL, 0); return reply_empty(m); }
    if (!strcmp(member, "Terminate")) { if (!exact(m, "")) return reply_error(m, E_ARGS, "Terminate()");
        if (!own) return auth_error(m, authorize("manage", c), member);
        if (!session_command("session-terminate", s)) return reply_error(m, "org.freedesktop.DBus.Error.Failed", "the display server refused"); return reply_empty(m); }
    if (!strcmp(member, "Kill")) {
        if (!exact(m, "si") || !args(m, DBUS_TYPE_STRING, &who, DBUS_TYPE_INT32, &number, DBUS_TYPE_INVALID) || number < 1 || number > 64 || (strcmp(who, "leader") && strcmp(who, "all"))) return reply_error(m, E_ARGS, "Kill(si)");
        if (!own) return auth_error(m, authorize("manage", c), member);
        if (s->leader == 1) return reply_error(m, E_ACCESS, "The display server's session leader is not signalled");
        kill_session(s, !strcmp(who, "leader"), number); return reply_empty(m);
    }
    if (!strcmp(member, "SetIdleHint") || !strcmp(member, "SetLockedHint")) {
        if (!exact(m, "b") || !args(m, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "%s(b)", member);
        if (c->uid != 0 && c->uid != s->uid) return reply_error(m, E_ACCESS, "Only the session's user may set its hints");
        if (member[3] == 'I') { s->idle = b; s->idle_since_us = now_us(CLOCK_REALTIME); static const char* const n[] = {"IdleHint", "IdleSinceHint", NULL}; properties_changed(session_path(s), SESSION_IF, s, s->uid, n); }
        else { s->locked_override = true; s->locked_hint = b; s->locked = b; static const char* const n[] = {"LockedHint", NULL}; properties_changed(session_path(s), SESSION_IF, s, s->uid, n); }
        return reply_empty(m);
    }
    if (!strcmp(member, "SetBrightness")) {
        if (!exact(m, "ssu") || !args(m, DBUS_TYPE_STRING, &subsystem, DBUS_TYPE_STRING, &name, DBUS_TYPE_UINT32, &value, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "SetBrightness(ssu)");
        if (strcmp(subsystem, "backlight") && strcmp(subsystem, "leds")) return reply_error(m, E_ARGS, "Invalid subsystem %.40s", subsystem);
        if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > 64) return reply_error(m, E_ARGS, "Invalid device name");
        if (!(c->uid == 0 || (c->uid == s->uid && s->active))) return reply_error(m, E_ACCESS, "Not the active session");
        char path[160], text[24]; snprintf(path, sizeof(path), "/sys/class/%s/%s/brightness", subsystem, name);
        int fd = open(path, O_WRONLY | O_CLOEXEC); if (fd < 0) return reply_error(m, E_NO_DEVICE, "No such device %.64s", name);
        int n = snprintf(text, sizeof(text), "%u", (unsigned)value); bool ok = write(fd, text, (size_t)n) == n; int error = errno; close(fd);
        if (!ok) return reply_error(m, E_ARGS, "brightness %u: %s", (unsigned)value, strerror(error));
        return reply_empty(m);
    }
    if (!strcmp(member, "TakeControl")) {
        if (!exact(m, "b") || !args(m, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "TakeControl(b)");
        if (c->uid != s->uid && c->uid != 0) return reply_error(m, E_ACCESS, "Only the session's user may control it");
        if (s->controller[0] && strcmp(s->controller, c->sender) && !(b && c->uid == 0)) return reply_error(m, E_BUSY, "Device or resource busy: the session has a controller");
        if (s->controller[0] && strcmp(s->controller, c->sender)) controller_gone(s->controller);
        snprintf(s->controller, sizeof(s->controller), "%s", c->sender); logline("session %s: controller %s", s->id, c->sender); return reply_empty(m);
    }
    if (!strcmp(member, "ReleaseControl")) {
        if (!exact(m, "")) return reply_error(m, E_ARGS, "ReleaseControl()");
        if (strcmp(s->controller, c->sender)) return reply_error(m, E_NOT_CONTROLLER, "You are not in control of this session");
        controller_gone(c->sender); return reply_empty(m);
    }
    if (!strcmp(member, "TakeDevice") || !strcmp(member, "ReleaseDevice") || !strcmp(member, "PauseDeviceComplete")) {
        if (!exact(m, "uu") || !args(m, DBUS_TYPE_UINT32, &major, DBUS_TYPE_UINT32, &minor, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "%s(uu)", member);
        if (!s->controller[0] || strcmp(s->controller, c->sender)) return reply_error(m, E_NOT_CONTROLLER, "You are not in control of this session");
        struct device* found = NULL, *slot = NULL;
        for (int d = 0; d < MAX_DEVICES; d++) { if (s->devices[d].used && s->devices[d].major == major && s->devices[d].minor == minor) found = &s->devices[d]; else if (!s->devices[d].used && !slot) slot = &s->devices[d]; }
        if (member[0] == 'P') return reply_empty(m);
        if (member[0] == 'R') { if (!found) return reply_error(m, E_NO_DEVICE, "Device not taken"); if (found->fd >= 0) close(found->fd); memset(found, 0, sizeof(*found)); found->fd = -1; return reply_empty(m); }
        if (found) return reply_error(m, E_DEVICE_TAKEN, "Device already taken");
        if (major != 13 && major != 226) return reply_error(m, E_ARGS, "Only input (13) and DRM (226) devices");
        if (!slot) return reply_error(m, "org.freedesktop.DBus.Error.LimitsExceeded", "too many devices");
        if (!s->active) return reply_error(m, E_ACCESS, "The session is not active: devices belong to the active session");
        int fd = open_device(s, major, minor); if (fd < 0) return reply_error(m, E_NO_DEVICE, "%u:%u: %s", (unsigned)major, (unsigned)minor, strerror(errno));
        DBusMessage* r = dbus_message_new_method_return(m); dbus_bool_t inactive = false;
        if (!r || !dbus_message_append_args(r, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_BOOLEAN, &inactive, DBUS_TYPE_INVALID)) { if (r) dbus_message_unref(r); close(fd); return DBUS_HANDLER_RESULT_NEED_MEMORY; }
        slot->used = true; slot->major = major; slot->minor = minor; slot->fd = fd; slot->paused = false;
        dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(member, "SetType") || !strcmp(member, "SetDisplay") || !strcmp(member, "SetTTY") || !strcmp(member, "SetClass")) return reply_error(m, E_NOTSUP, "%s is not supported by NexusOS", member);
    return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method %.60s", member);
}
static DBusHandlerResult handle(DBusConnection* connection, DBusMessage* m, void* data) {
    (void)connection; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *path = dbus_message_get_path(m), *iface = dbus_message_get_interface(m), *member = dbus_message_get_member(m);
    if (!path || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    struct caller c; if (!identify(m, &c)) return reply_error(m, E_ACCESS, "Cannot identify the caller");
    struct session* s; unsigned uid; int kind = object_of(path, &c, &s, &uid);
    if (kind < 0) return reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", "Unknown object %.120s", path);
    if (iface && !strcmp(iface, "org.freedesktop.DBus.Peer")) {
        if (!strcmp(member, "Ping")) return reply_empty(m);
        if (!strcmp(member, "GetMachineId")) { char id[64] = ""; FILE* f = fopen("/etc/machine-id", "re"); if (f) { if (!fgets(id, sizeof(id), f)) id[0] = 0; fclose(f); } id[strcspn(id, "\n")] = 0; return reply_string(m, id); }
    }
    if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
        char* xml = malloc(32768); if (!xml) return DBUS_HANDLER_RESULT_NEED_MEMORY;
        snprintf(xml, 32768, "%s%s", INTROSPECT_HEAD, kind == 0 ? INTROSPECT_MANAGER : kind == 1 ? INTROSPECT_SESSION : kind == 2 ? INTROSPECT_USER : kind == 3 ? INTROSPECT_SEAT : "");
        if (kind == 0) strcat(xml, "<node name=\"session\"/><node name=\"user\"/><node name=\"seat\"/>");
        if (kind == 4 && strstr(path, "/session")) for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used) { const char* p = session_path(&sessions[i]); size_t l = strlen(xml); snprintf(xml + l, 32768 - l, "<node name=\"%s\"/>", strrchr(p, '/') + 1); }
        if (kind == 4 && strstr(path, "/seat")) strcat(xml, "<node name=\"seat0\"/>");
        strcat(xml, "</node>\n"); DBusHandlerResult r = reply_string(m, xml); free(xml); return r;
    }
    if (kind == 4) return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method %.60s", member);
    if (iface && !strcmp(iface, PROPS)) return properties(m, kind, s, uid);
    static const char* const ifaces[] = {MANAGER, SESSION_IF, USER_IF, SEAT_IF};
    if (iface && strcmp(iface, ifaces[kind])) return reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "Unknown interface %.80s", iface);
    if (kind == 0) return manager_method(m, &c, member);
    if (kind == 1) return session_method(m, &c, s, member);
    if (kind == 2) {
        if (!strcmp(member, "Terminate") || !strcmp(member, "Kill")) {
            dbus_int32_t number = SIGTERM;
            if (member[0] == 'K' ? !exact(m, "i") || !args(m, DBUS_TYPE_INT32, &number, DBUS_TYPE_INVALID) || number < 1 || number > 64 : !exact(m, "")) return reply_error(m, E_ARGS, "%s", member);
            if (c.uid && c.uid != uid) return auth_error(m, authorize("manage", &c), member);
            for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].uid == uid) { if (member[0] == 'T') session_command("session-terminate", &sessions[i]); else kill_session(&sessions[i], false, number); }
            return reply_empty(m);
        }
        return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method %.60s", member);
    }
    if (!strcmp(member, "ActivateSession")) {
        const char* id; if (!exact(m, "s") || !args(m, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID)) return reply_error(m, E_ARGS, "ActivateSession(s)");
        struct session* target = session_arg(id, &c); if (!target) return reply_error(m, E_NO_SESSION, "No session '%.40s' known", id);
        if (authorize("chvt", &c) != AUTH_YES && c.uid != target->uid) return auth_error(m, AUTH_CHALLENGE, "switch sessions");
        if (!target->active && !session_command("session-activate", target)) return reply_error(m, "org.freedesktop.DBus.Error.Failed", "the display server refused");
        refresh(); return reply_empty(m);
    }
    if (!strcmp(member, "SwitchTo") || !strcmp(member, "SwitchToNext") || !strcmp(member, "SwitchToPrevious") || !strcmp(member, "Terminate"))
        return reply_error(m, E_NOTSUP, "%s: NexusOS has no virtual terminals", member);
    return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method %.60s", member);
}
static DBusHandlerResult bus_filter(DBusConnection* connection, DBusMessage* m, void* data) {   /* a controller left the bus */
    (void)connection; (void)data;
    if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old, *new_owner;
        if (args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old, DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) && name[0] == ':' && !new_owner[0]) controller_gone(name);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void child_signal(int sig) { (void)sig; int saved = errno; if (write(child_pipe[1], "c", 1) < 0) {} errno = saved; }
static volatile sig_atomic_t stop;
static void stop_signal(int sig) { (void)sig; stop = 1; if (write(child_pipe[1], "t", 1) < 0) {} }
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (pipe2(child_pipe, O_CLOEXEC | O_NONBLOCK)) return 1;
    struct sigaction action = {.sa_handler = child_signal, .sa_flags = SA_RESTART | SA_NOCLDSTOP}; sigemptyset(&action.sa_mask); sigaction(SIGCHLD, &action, NULL);
    struct sigaction term = {.sa_handler = stop_signal}; sigemptyset(&term.sa_mask); sigaction(SIGTERM, &term, NULL); sigaction(SIGINT, &term, NULL);
    for (int i = 0; i < MAX_INHIBITORS; i++) inhibitors[i].fd = -1;
    DBusError e; dbus_error_init(&e);
    bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
    if (!bus) { logline("no system bus: %s", e.message); return 1; }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    int got = dbus_bus_request_name(bus, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (got != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { logline("cannot own %s: %s", BUS_NAME, dbus_error_is_set(&e) ? e.message : "taken"); return 1; }
    static const DBusObjectPathVTable vtable = {.message_function = handle};
    if (!dbus_connection_register_fallback(bus, ROOT_PATH, &vtable, NULL)) return 1;
    dbus_bus_add_match(bus, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'", NULL);
    dbus_connection_add_filter(bus, bus_filter, NULL, NULL);
    int notify = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "%s", NOTIFY_SOCKET); mkdir("/run/nexus", 0755); unlink(NOTIFY_SOCKET);
    if (notify < 0 || bind(notify, (struct sockaddr*)&a, sizeof(a)) || chmod(NOTIFY_SOCKET, 0622)) logline("%s: %s", NOTIFY_SOCKET, strerror(errno));
    int uevent = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl nl = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (uevent >= 0 && bind(uevent, (struct sockaddr*)&nl, sizeof(nl))) { close(uevent); uevent = -1; }
    const char* seat_id = "seat0"; const char* seat = seat_path(); emit(ROOT_PATH, MANAGER, "SeatNew", DBUS_TYPE_STRING, &seat_id, DBUS_TYPE_OBJECT_PATH, &seat, DBUS_TYPE_INVALID);
    refresh();
    logline("ready on the system bus as %s (%u sessions)", BUS_NAME, session_count());
    int bus_fd = -1; dbus_connection_get_unix_fd(bus, &bus_fd);
    uint64_t next_refresh = now_us(CLOCK_MONOTONIC) + 30000000;
    while (!stop) {
        dbus_connection_flush(bus);
        struct pollfd fds[4 + MAX_INHIBITORS]; struct inhibitor* owner[4 + MAX_INHIBITORS]; unsigned nf = 0;
        fds[nf] = (struct pollfd){bus_fd, POLLIN, 0}; owner[nf++] = NULL;
        fds[nf] = (struct pollfd){child_pipe[0], POLLIN, 0}; owner[nf++] = NULL;
        fds[nf] = (struct pollfd){notify, POLLIN, 0}; owner[nf++] = NULL;
        fds[nf] = (struct pollfd){uevent, POLLIN, 0}; owner[nf++] = NULL;
        for (int i = 0; i < MAX_INHIBITORS; i++) if (inhibitors[i].used) { fds[nf] = (struct pollfd){inhibitors[i].fd, POLLIN, 0}; owner[nf++] = &inhibitors[i]; }
        uint64_t now = now_us(CLOCK_MONOTONIC); int timeout = (int)((next_refresh > now ? next_refresh - now : 0) / 1000);
        if (pending_op) { int until = pending_deadline > now ? (int)((pending_deadline - now) / 1000) + 1 : 0; if (until < timeout) timeout = until; }
        if (dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS) timeout = 0;
        int ready = poll(fds, nf, timeout);
        if (ready < 0 && errno != EINTR) { logline("poll: %s", strerror(errno)); break; }
        if (fds[0].revents & (POLLHUP | POLLERR)) { logline("the system bus went away"); break; }
        if (!dbus_connection_read_write(bus, 0)) { logline("disconnected from the system bus"); break; }
        while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
        if (ready > 0 && fds[1].revents) { char b[64]; while (read(child_pipe[0], b, sizeof(b)) > 0) {} int status; pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) if (pid == sleep_child) sleep_finished(status); }
        if (ready > 0 && notify >= 0 && fds[2].revents) { char b[64]; while (recv(notify, b, sizeof(b), MSG_DONTWAIT) > 0) {} refresh(); }
        if (ready > 0 && uevent >= 0 && fds[3].revents) uevent_input(uevent);
        for (unsigned i = 4; ready > 0 && i < nf; i++) if (fds[i].revents && owner[i]->used) {   /* the holder closed its end (or wrote: ignored) */
            char b[64]; ssize_t n = read(owner[i]->fd, b, sizeof(b));
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR) || (fds[i].revents & (POLLHUP | POLLERR))) release_inhibitor(owner[i]);
        }
        delay_check();
        if (now_us(CLOCK_MONOTONIC) >= next_refresh) { refresh(); next_refresh = now_us(CLOCK_MONOTONIC) + 30000000; }
    }
    logline("stopping");
    for (int i = 0; i < MAX_INHIBITORS; i++) if (inhibitors[i].used) close(inhibitors[i].fd);
    dbus_connection_close(bus); dbus_connection_unref(bus);
    return 0;
}
