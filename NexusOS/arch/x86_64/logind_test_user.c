/* logind-test: a libdbus client of org.freedesktop.login1 for tests/test_aurora_sessions.py (and by hand).
 *   logind-test [--log FILE] list | manager | session ID | user UID | seat | by-pid PID | can
 *   logind-test [--log FILE] inhibit WHAT MODE SECONDS        hold an inhibitor (released when it exits)
 *   logind-test [--log FILE] watch SECONDS [--delay MS]       print PrepareForSleep/Shutdown and Session signals; with
 *                                                             --delay a sleep delay inhibitor, closed MS after PrepareForSleep(true)
 *   logind-test [--log FILE] call METHOD [b|t|s:VALUE...]     a Manager method; prints the reply or the error name
 *   logind-test [--log FILE] hostile                          malformed and unauthorised requests (each must fail cleanly)
 *   logind-test [--log FILE] take-device MAJOR MINOR SECONDS  TakeControl + TakeDevice on the caller's session; reads it
 * Output lines start with "LT ". Exit status 0 unless the bus could not be reached. */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEST "org.freedesktop.login1"
#define ROOT "/org/freedesktop/login1"
#define MANAGER "org.freedesktop.login1.Manager"
static DBusConnection* bus; static FILE* logfile;
static void out(const char* format, ...) {
    char line[4096]; va_list list; va_start(list, format); vsnprintf(line, sizeof(line), format, list); va_end(list);
    printf("LT %s\n", line); fflush(stdout); if (logfile) { fprintf(logfile, "LT %s\n", line); fflush(logfile); }
}
static uint64_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static DBusMessage* call(const char* path, const char* iface, const char* method, char* error, size_t size, int first, ...) {
    DBusMessage* m = dbus_message_new_method_call(DEST, path, iface, method); if (!m) { snprintf(error, size, "oom"); return NULL; }
    va_list list; va_start(list, first); if (first != DBUS_TYPE_INVALID) dbus_message_append_args_valist(m, first, list); va_end(list);
    DBusError e; dbus_error_init(&e); DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, m, 15000, &e); dbus_message_unref(m);
    if (!r) { snprintf(error, size, "%s: %s", e.name ? e.name : "?", e.message ? e.message : ""); dbus_error_free(&e); }
    return r;
}
static void print_value(DBusMessageIter* it, char* text, size_t size) {
    int type = dbus_message_iter_get_arg_type(it); size_t used = strlen(text);
    if (type == DBUS_TYPE_VARIANT) { DBusMessageIter v; dbus_message_iter_recurse(it, &v); print_value(&v, text, size); return; }
    if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) { const char* s; dbus_message_iter_get_basic(it, &s); snprintf(text + used, size - used, "%s", s); return; }
    if (type == DBUS_TYPE_BOOLEAN) { dbus_bool_t b; dbus_message_iter_get_basic(it, &b); snprintf(text + used, size - used, "%s", b ? "true" : "false"); return; }
    if (type == DBUS_TYPE_UINT32) { dbus_uint32_t u; dbus_message_iter_get_basic(it, &u); snprintf(text + used, size - used, "%u", u); return; }
    if (type == DBUS_TYPE_INT32) { dbus_int32_t u; dbus_message_iter_get_basic(it, &u); snprintf(text + used, size - used, "%d", u); return; }
    if (type == DBUS_TYPE_UINT64) { dbus_uint64_t u; dbus_message_iter_get_basic(it, &u); snprintf(text + used, size - used, "%llu", (unsigned long long)u); return; }
    if (type == DBUS_TYPE_UNIX_FD) { int fd; dbus_message_iter_get_basic(it, &fd); snprintf(text + used, size - used, "fd:%d", fd); return; }
    if (type == DBUS_TYPE_STRUCT || type == DBUS_TYPE_ARRAY) {
        DBusMessageIter sub; dbus_message_iter_recurse(it, &sub); snprintf(text + used, size - used, "%c", type == DBUS_TYPE_STRUCT ? '(' : '[');
        bool first = true; while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
            if (!first) { used = strlen(text); snprintf(text + used, size - used, ","); } first = false;
            print_value(&sub, text, size); dbus_message_iter_next(&sub); }
        used = strlen(text); snprintf(text + used, size - used, "%c", type == DBUS_TYPE_STRUCT ? ')' : ']'); return;
    }
    snprintf(text + used, size - used, "<%c>", type);
}
static void dump_reply(const char* label, DBusMessage* r) {
    char text[4096] = ""; DBusMessageIter it;
    if (dbus_message_iter_init(r, &it)) do { if (text[0]) strncat(text, " ", sizeof(text) - strlen(text) - 1); print_value(&it, text, sizeof(text)); } while (dbus_message_iter_next(&it));
    out("%s %s", label, text);
}
static void get_all(const char* path, const char* iface, const char* label) {
    char error[512]; DBusMessage* r = call(path, "org.freedesktop.DBus.Properties", "GetAll", error, sizeof(error), DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    if (!r) { out("%s ERROR %s", label, error); return; }
    DBusMessageIter it, dict, entry; dbus_message_iter_init(r, &it); dbus_message_iter_recurse(&it, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(&dict, &entry); const char* name; dbus_message_iter_get_basic(&entry, &name); dbus_message_iter_next(&entry);
        char text[2048] = ""; print_value(&entry, text, sizeof(text)); out("%s %s=%s", label, name, text); dbus_message_iter_next(&dict);
    }
    dbus_message_unref(r);
}
static const char* object(const char* method, const char* arg_s, dbus_uint32_t arg_u, bool use_u, char* error, size_t size) {
    static char path[256]; DBusMessage* r = use_u ? call(ROOT, MANAGER, method, error, size, DBUS_TYPE_UINT32, &arg_u, DBUS_TYPE_INVALID) : call(ROOT, MANAGER, method, error, size, DBUS_TYPE_STRING, &arg_s, DBUS_TYPE_INVALID);
    if (!r) return NULL; const char* p = NULL; DBusError e; dbus_error_init(&e);
    if (!dbus_message_get_args(r, &e, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID)) { dbus_error_free(&e); dbus_message_unref(r); return NULL; }
    snprintf(path, sizeof(path), "%s", p); dbus_message_unref(r); return path;
}
static int inhibit(const char* what, const char* who, const char* why, const char* mode, char* error, size_t size) {
    DBusMessage* r = call(ROOT, MANAGER, "Inhibit", error, size, DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who, DBUS_TYPE_STRING, &why, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID);
    if (!r) return -1; int fd = -1; DBusError e; dbus_error_init(&e);
    if (!dbus_message_get_args(r, &e, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) { snprintf(error, size, "no fd: %s", e.message); dbus_error_free(&e); fd = -1; }
    dbus_message_unref(r); return fd;
}
static void expect_error(const char* name, DBusMessage* r, const char* error, const char* want) {
    if (r) { out("HOSTILE %s FAIL accepted", name); dbus_message_unref(r); return; }
    bool ok = !want || strstr(error, want); out("HOSTILE %s %s %s", name, ok ? "PASS" : "FAIL", error);
}
static void hostile(void) {
    char error[512]; const char* s; dbus_uint32_t u; dbus_bool_t b = 0;
    s = "nonsense"; expect_error("inhibit-bad-what", NULL, (inhibit(s, "t", "t", "block", error, sizeof(error)) < 0) ? error : "accepted", "InvalidArgs");
    expect_error("inhibit-bad-mode", NULL, (inhibit("sleep", "t", "t", "sometimes", error, sizeof(error)) < 0) ? error : "accepted", "InvalidArgs");
    expect_error("inhibit-delay-idle", NULL, (inhibit("idle", "t", "t", "delay", error, sizeof(error)) < 0) ? error : "accepted", "InvalidArgs");
    expect_error("inhibit-empty-what", NULL, (inhibit("", "t", "t", "block", error, sizeof(error)) < 0) ? error : "accepted", "InvalidArgs");
    char longwho[600]; memset(longwho, 'w', sizeof(longwho) - 1); longwho[sizeof(longwho) - 1] = 0;
    expect_error("inhibit-long-who", NULL, (inhibit("sleep", longwho, "t", "block", error, sizeof(error)) < 0) ? error : "accepted", "InvalidArgs");
    expect_error("inhibit-control-chars", NULL, (inhibit("sleep", "a\tb", "t", "block", error, sizeof(error)) < 0) ? error : "accepted", "InvalidArgs");
    int fds[200], taken = 0; bool limited = false;
    for (int i = 0; i < 200; i++) { fds[i] = inhibit("handle-lid-switch", "flood", "flood", "block", error, sizeof(error)); if (fds[i] < 0) { limited = strstr(error, "LimitsExceeded") != NULL; break; } taken++; }
    out("HOSTILE inhibitor-flood %s taken=%d %s", limited && taken <= 128 ? "PASS" : "FAIL", taken, limited ? "" : error);
    for (int i = 0; i < taken; i++) close(fds[i]);
    s = "string-not-bool"; expect_error("suspend-wrong-signature", call(ROOT, MANAGER, "Suspend", error, sizeof(error), DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID), error, "InvalidArgs");
    dbus_uint64_t flags = 0xFF00; expect_error("poweroff-bad-flags", call(ROOT, MANAGER, "PowerOffWithFlags", error, sizeof(error), DBUS_TYPE_UINT64, &flags, DBUS_TYPE_INVALID), error, "InvalidArgs");
    expect_error("unknown-method", call(ROOT, MANAGER, "MakeCoffee", error, sizeof(error), DBUS_TYPE_INVALID), error, "UnknownMethod");
    expect_error("unknown-object", call(ROOT "/session/_39999", "org.freedesktop.login1.Session", "Lock", error, sizeof(error), DBUS_TYPE_INVALID), error, "UnknownObject");
    expect_error("bad-user-path", call(ROOT "/user/_x1", "org.freedesktop.login1.User", "Terminate", error, sizeof(error), DBUS_TYPE_INVALID), error, "UnknownObject");
    s = "9999"; expect_error("get-unknown-session", call(ROOT, MANAGER, "GetSession", error, sizeof(error), DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID), error, "NoSuchSession");
    u = 0x7FFFFFF0; expect_error("get-session-by-bogus-pid", call(ROOT, MANAGER, "GetSessionByPID", error, sizeof(error), DBUS_TYPE_UINT32, &u, DBUS_TYPE_INVALID), error, "NoSessionForPID");
    const char *iface = MANAGER, *prop = "NoSuchProperty";
    expect_error("get-unknown-property", call(ROOT, "org.freedesktop.DBus.Properties", "Get", error, sizeof(error), DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID), error, "UnknownProperty");
    s = "../../etc"; const char* who = "all"; dbus_int32_t sig = 9;
    expect_error("kill-session-bad-id", call(ROOT, MANAGER, "KillSession", error, sizeof(error), DBUS_TYPE_STRING, &s, DBUS_TYPE_STRING, &who, DBUS_TYPE_INT32, &sig, DBUS_TYPE_INVALID), error, "NoSuchSession");
    s = "1"; sig = 9; expect_error("kill-display-server-session-leader", call(ROOT, MANAGER, "KillSession", error, sizeof(error), DBUS_TYPE_STRING, &s, DBUS_TYPE_STRING, (const char*[]){"leader"}, DBUS_TYPE_INT32, &sig, DBUS_TYPE_INVALID), error, NULL);
    sig = 200; s = "1"; expect_error("kill-bad-signal", call(ROOT, MANAGER, "KillSession", error, sizeof(error), DBUS_TYPE_STRING, &s, DBUS_TYPE_STRING, &who, DBUS_TYPE_INT32, &sig, DBUS_TYPE_INVALID), error, "InvalidArgs");
    const char *subsystem = "backlight", *dev = "../../../etc/passwd"; u = 1;
    expect_error("brightness-path-escape", call(ROOT "/session/auto", "org.freedesktop.login1.Session", "SetBrightness", error, sizeof(error), DBUS_TYPE_STRING, &subsystem, DBUS_TYPE_STRING, &dev, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INVALID), error, NULL);
    iface = MANAGER; prop = "IdleHint"; DBusMessage* m = dbus_message_new_method_call(DEST, ROOT, "org.freedesktop.DBus.Properties", "Set");
    DBusMessageIter it, v; dbus_message_iter_init_append(m, &it); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b); dbus_message_iter_close_container(&it, &v);
    DBusError e; dbus_error_init(&e); DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, m, 5000, &e); dbus_message_unref(m);
    if (!r) snprintf(error, sizeof(error), "%s", e.name ? e.name : "?"); dbus_error_free(&e);
    expect_error("set-property", r, error, "PropertyReadOnly");
    out("HOSTILE DONE");
}
static DBusHandlerResult watch_filter(DBusConnection* c, DBusMessage* m, void* data) {
    (void)c; int* delay_fd = data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char* member = dbus_message_get_member(m); const char* path = dbus_message_get_path(m);
    if (!strcmp(member, "PrepareForSleep") || !strcmp(member, "PrepareForShutdown")) {
        dbus_bool_t b = 0; DBusError e; dbus_error_init(&e); dbus_message_get_args(m, &e, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID); dbus_error_free(&e);
        out("SIGNAL %s %s at=%llu", member, b ? "true" : "false", (unsigned long long)ms());
        if (b && delay_fd && *delay_fd >= 0 && !strcmp(member, "PrepareForSleep")) { int wait = delay_fd[1]; struct timespec t = {wait / 1000, (long)(wait % 1000) * 1000000}; nanosleep(&t, NULL);
            out("DELAY released after %d ms at=%llu", wait, (unsigned long long)ms()); close(*delay_fd); *delay_fd = -1; }
    } else if (!strcmp(member, "SessionNew") || !strcmp(member, "SessionRemoved") || !strcmp(member, "UserNew") || !strcmp(member, "UserRemoved") || !strcmp(member, "Lock") || !strcmp(member, "Unlock") || !strcmp(member, "PauseDevice") || !strcmp(member, "ResumeDevice")) {
        char text[512] = ""; DBusMessageIter it; if (dbus_message_iter_init(m, &it)) do { if (text[0]) strncat(text, " ", sizeof(text) - strlen(text) - 1); print_value(&it, text, sizeof(text)); } while (dbus_message_iter_next(&it));
        out("SIGNAL %s %s %s", member, path, text);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}
int main(int argc, char** argv) {
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "--log")) { logfile = fopen(argv[2], "ae"); first = 3; }
    if (argc <= first) { fprintf(stderr, "usage: logind-test [--log FILE] list|manager|session ID|user UID|seat|by-pid PID|can|inhibit W M S|watch S [--delay MS]|call M ...|hostile|take-device MA MI S\n"); return 2; }
    DBusError e; dbus_error_init(&e); bus = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    if (!bus) { out("ERROR no system bus: %s", e.message); return 1; }
    const char* cmd = argv[first]; char error[512];
    if (!strcmp(cmd, "list")) {
        DBusMessage* r = call(ROOT, MANAGER, "ListSessions", error, sizeof(error), DBUS_TYPE_INVALID); if (r) { dump_reply("SESSIONS", r); dbus_message_unref(r); } else out("SESSIONS ERROR %s", error);
        r = call(ROOT, MANAGER, "ListUsers", error, sizeof(error), DBUS_TYPE_INVALID); if (r) { dump_reply("USERS", r); dbus_message_unref(r); } else out("USERS ERROR %s", error);
        r = call(ROOT, MANAGER, "ListSeats", error, sizeof(error), DBUS_TYPE_INVALID); if (r) { dump_reply("SEATS", r); dbus_message_unref(r); } else out("SEATS ERROR %s", error);
        r = call(ROOT, MANAGER, "ListInhibitors", error, sizeof(error), DBUS_TYPE_INVALID); if (r) { dump_reply("INHIBITORS", r); dbus_message_unref(r); } else out("INHIBITORS ERROR %s", error);
        return 0;
    }
    if (!strcmp(cmd, "manager")) { get_all(ROOT, MANAGER, "MANAGER"); return 0; }
    if (!strcmp(cmd, "seat")) { get_all(ROOT "/seat/seat0", "org.freedesktop.login1.Seat", "SEAT"); return 0; }
    if (!strcmp(cmd, "session") && argc > first + 1) {
        const char* p = object("GetSession", argv[first + 1], 0, false, error, sizeof(error)); if (!p) { out("SESSION ERROR %s", error); return 0; }
        out("SESSION PATH %s", p); get_all(p, "org.freedesktop.login1.Session", "SESSION"); return 0;
    }
    if (!strcmp(cmd, "user") && argc > first + 1) {
        const char* p = object("GetUser", NULL, (dbus_uint32_t)strtoul(argv[first + 1], NULL, 10), true, error, sizeof(error)); if (!p) { out("USER ERROR %s", error); return 0; }
        out("USER PATH %s", p); get_all(p, "org.freedesktop.login1.User", "USER"); return 0;
    }
    if (!strcmp(cmd, "by-pid") && argc > first + 1) {
        const char* p = object("GetSessionByPID", NULL, (dbus_uint32_t)strtoul(argv[first + 1], NULL, 10), true, error, sizeof(error));
        if (!p) out("BYPID ERROR %s", error); else { out("BYPID %s", p); get_all(p, "org.freedesktop.login1.Session", "BYPID"); } return 0;
    }
    if (!strcmp(cmd, "can")) {
        static const char* const methods[] = {"CanPowerOff", "CanReboot", "CanSuspend", "CanHibernate", "CanHybridSleep", "CanSuspendThenHibernate"};
        for (unsigned i = 0; i < 6; i++) { DBusMessage* r = call(ROOT, MANAGER, methods[i], error, sizeof(error), DBUS_TYPE_INVALID); if (r) { char label[64]; snprintf(label, sizeof(label), "CAN %s", methods[i]); dump_reply(label, r); dbus_message_unref(r); } else out("CAN %s ERROR %s", methods[i], error); }
        return 0;
    }
    if (!strcmp(cmd, "inhibit") && argc > first + 3) {
        int fd = inhibit(argv[first + 1], "logind-test", "test inhibitor", argv[first + 2], error, sizeof(error));
        if (fd < 0) { out("INHIBIT ERROR %s", error); return 0; }
        out("INHIBIT TAKEN %s %s fd=%d", argv[first + 1], argv[first + 2], fd); sleep((unsigned)atoi(argv[first + 3])); close(fd); out("INHIBIT RELEASED %s", argv[first + 1]); return 0;
    }
    if (!strcmp(cmd, "watch") && argc > first + 1) {
        int delay[2] = {-1, 0};
        if (argc > first + 3 && !strcmp(argv[first + 2], "--delay")) { delay[1] = atoi(argv[first + 3]); delay[0] = inhibit("sleep", "logind-test", "delays sleep", "delay", error, sizeof(error)); out("DELAY %s", delay[0] >= 0 ? "TAKEN" : error); }
        dbus_bus_add_match(bus, "type='signal',sender='org.freedesktop.login1'", &e); dbus_error_free(&e);
        dbus_connection_add_filter(bus, watch_filter, delay, NULL); out("WATCHING");
        uint64_t end = ms() + (uint64_t)atoi(argv[first + 1]) * 1000;
        while (ms() < end && dbus_connection_read_write_dispatch(bus, 200)) {}
        out("WATCH END"); return 0;
    }
    if (!strcmp(cmd, "call") && argc > first + 1) {
        DBusMessage* m = dbus_message_new_method_call(DEST, ROOT, MANAGER, argv[first + 1]); DBusMessageIter it; dbus_message_iter_init_append(m, &it);
        for (int i = first + 2; i < argc; i++) {
            const char* a = argv[i];
            if (!strncmp(a, "b:", 2)) { dbus_bool_t b = a[2] == '1' || a[2] == 't'; dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &b); }
            else if (!strncmp(a, "t:", 2)) { dbus_uint64_t t = strtoull(a + 2, NULL, 0); dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT64, &t); }
            else if (!strncmp(a, "u:", 2)) { dbus_uint32_t u = (dbus_uint32_t)strtoul(a + 2, NULL, 0); dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &u); }
            else if (!strncmp(a, "s:", 2)) { const char* s = a + 2; dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s); }
        }
        DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, m, 15000, &e); dbus_message_unref(m);
        if (r) { char label[96]; snprintf(label, sizeof(label), "CALL %s OK", argv[first + 1]); dump_reply(label, r); dbus_message_unref(r); }
        else { out("CALL %s ERROR %s %s", argv[first + 1], e.name ? e.name : "?", e.message ? e.message : ""); dbus_error_free(&e); }
        return 0;
    }
    if (!strcmp(cmd, "hostile")) { hostile(); return 0; }
    if (!strcmp(cmd, "take-device") && argc > first + 3) {
        const char* p = object("GetSession", "auto", 0, false, error, sizeof(error)); if (!p) { out("DEVICE ERROR no session: %s", error); return 0; }
        dbus_bool_t force = 0; DBusMessage* r = call(p, "org.freedesktop.login1.Session", "TakeControl", error, sizeof(error), DBUS_TYPE_BOOLEAN, &force, DBUS_TYPE_INVALID);
        if (!r) { out("DEVICE ERROR TakeControl %s", error); return 0; } dbus_message_unref(r);
        r = call(p, "org.freedesktop.login1.Session", "TakeControl", error, sizeof(error), DBUS_TYPE_BOOLEAN, &force, DBUS_TYPE_INVALID);
        out("DEVICE TakeControl again: %s", r ? "accepted (same controller)" : error); if (r) dbus_message_unref(r);
        dbus_uint32_t major = (dbus_uint32_t)atoi(argv[first + 1]), minor = (dbus_uint32_t)atoi(argv[first + 2]);
        r = call(p, "org.freedesktop.login1.Session", "TakeDevice", error, sizeof(error), DBUS_TYPE_UINT32, &major, DBUS_TYPE_UINT32, &minor, DBUS_TYPE_INVALID);
        if (!r) { out("DEVICE ERROR TakeDevice %s", error); return 0; }
        int fd = -1; dbus_bool_t inactive = 1; DBusError e2; dbus_error_init(&e2);
        dbus_message_get_args(r, &e2, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_BOOLEAN, &inactive, DBUS_TYPE_INVALID); dbus_error_free(&e2); dbus_message_unref(r);
        char buf[64]; ssize_t n = read(fd, buf, sizeof(buf)); out("DEVICE TAKEN %u:%u fd=%d inactive=%d read=%zd errno=%d", major, minor, fd, inactive, n, n < 0 ? errno : 0);
        dbus_bus_add_match(bus, "type='signal',sender='org.freedesktop.login1'", &e); dbus_error_free(&e);
        dbus_connection_add_filter(bus, watch_filter, NULL, NULL);
        uint64_t end = ms() + (uint64_t)atoi(argv[first + 3]) * 1000; bool revoked = false;
        while (ms() < end && dbus_connection_read_write_dispatch(bus, 200)) {
            n = read(fd, buf, sizeof(buf)); if (n < 0 && errno == ENODEV && !revoked) { revoked = true; out("DEVICE REVOKED at=%llu", (unsigned long long)ms()); }
        }
        out("DEVICE END revoked=%d", revoked); return 0;
    }
    fprintf(stderr, "logind-test: unknown command %s\n", cmd); return 2;
}
