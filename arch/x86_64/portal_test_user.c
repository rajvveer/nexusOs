/* portal-test: a client of the desktop portal (org.freedesktop.portal.Desktop on the session bus) for
 * tests/test_aurora_portal.py and by hand. Output lines start with "PT ".
 *   portal-test settings                     Settings ReadAll + ReadOne color-scheme
 *   portal-test watch SECONDS                print SettingChanged signals
 *   portal-test screenshot [interactive]     Screenshot -> response + uri
 *   portal-test open [multiple|directory] [/FOLDER]   FileChooser.OpenFile -> response + uris
 *   portal-test save NAME FOLDER             FileChooser.SaveFile
 *   portal-test inhibit FLAGS SECONDS        Inhibit (8 idle, 4 suspend), released by Request.Close
 *   portal-test openuri URI                  OpenURI.OpenURI
 *   portal-test openfile PATH                OpenURI.OpenFile (a descriptor of PATH: its content type picks the handler)
 *   portal-test notify TITLE BODY            Notification.AddNotification
 *   portal-test hostile                      malformed calls to the frontend and straight to the Aurora backend */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEST "org.freedesktop.portal.Desktop"
#define OBJ "/org/freedesktop/portal/desktop"
static DBusConnection* bus;
static void out(const char* format, ...) { va_list l; va_start(l, format); printf("PT "); vprintf(format, l); printf("\n"); fflush(stdout); va_end(l); }
static uint64_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void print_value(DBusMessageIter* it, char* text, size_t size) {
    int type = dbus_message_iter_get_arg_type(it); size_t used = strlen(text);
    if (type == DBUS_TYPE_VARIANT) { DBusMessageIter v; dbus_message_iter_recurse(it, &v); print_value(&v, text, size); return; }
    if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) { const char* s; dbus_message_iter_get_basic(it, &s); snprintf(text + used, size - used, "%s", s); return; }
    if (type == DBUS_TYPE_BOOLEAN) { dbus_bool_t b; dbus_message_iter_get_basic(it, &b); snprintf(text + used, size - used, "%s", b ? "true" : "false"); return; }
    if (type == DBUS_TYPE_UINT32) { dbus_uint32_t u; dbus_message_iter_get_basic(it, &u); snprintf(text + used, size - used, "%u", u); return; }
    if (type == DBUS_TYPE_DOUBLE) { double d; dbus_message_iter_get_basic(it, &d); snprintf(text + used, size - used, "%.3f", d); return; }
    if (type == DBUS_TYPE_STRUCT || type == DBUS_TYPE_ARRAY || type == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter sub; dbus_message_iter_recurse(it, &sub); snprintf(text + used, size - used, "%c", type == DBUS_TYPE_ARRAY ? '[' : '(');
        bool first = true; while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) { if (!first) { used = strlen(text); snprintf(text + used, size - used, type == DBUS_TYPE_DICT_ENTRY ? "=" : ","); } first = false; print_value(&sub, text, size); dbus_message_iter_next(&sub); }
        used = strlen(text); snprintf(text + used, size - used, "%c", type == DBUS_TYPE_ARRAY ? ']' : ')'); return;
    }
    snprintf(text + used, size - used, "<%c>", type);
}
static void dump(const char* label, DBusMessage* m) {
    char text[8192] = ""; DBusMessageIter it;
    if (dbus_message_iter_init(m, &it)) do { if (text[0]) strncat(text, " ", sizeof(text) - strlen(text) - 1); print_value(&it, text, sizeof(text)); } while (dbus_message_iter_next(&it));
    out("%s %s", label, text);
}
static DBusMessage* call(DBusMessage* m, char* error, size_t size) {
    DBusError e; dbus_error_init(&e); DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, m, 30000, &e); dbus_message_unref(m);
    if (!r) { snprintf(error, size, "%s: %s", e.name ? e.name : "?", e.message ? e.message : ""); dbus_error_free(&e); }
    return r;
}
static char request_path[256]; static bool responded; static dbus_uint32_t response_code;
static DBusHandlerResult filter(DBusConnection* c, DBusMessage* m, void* data) {
    (void)c; (void)data;
    if (dbus_message_is_signal(m, "org.freedesktop.portal.Request", "Response") && !strcmp(dbus_message_get_path(m), request_path)) {
        DBusMessageIter it; dbus_message_iter_init(m, &it); dbus_message_iter_get_basic(&it, &response_code); responded = true; dump("RESPONSE", m);
    } else if (dbus_message_is_signal(m, "org.freedesktop.portal.Settings", "SettingChanged")) dump("SETTING-CHANGED", m);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
static const char* token(void) { static char t[32]; snprintf(t, sizeof(t), "pt%ld_%llu", (long)getpid(), (unsigned long long)ms()); return t; }
static void expect_request(const char* tok) {   /* /org/freedesktop/portal/desktop/request/SENDER/TOKEN */
    char sender[128]; snprintf(sender, sizeof(sender), "%s", dbus_bus_get_unique_name(bus) + 1); for (char* p = sender; *p; p++) if (*p == '.') *p = '_';
    snprintf(request_path, sizeof(request_path), "/org/freedesktop/portal/desktop/request/%s/%s", sender, tok);
    char match[400]; snprintf(match, sizeof(match), "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='%s'", request_path);
    dbus_bus_add_match(bus, match, NULL); responded = false;
}
static void wait_response(int seconds) {
    uint64_t end = ms() + (uint64_t)seconds * 1000;
    while (!responded && ms() < end && dbus_connection_read_write_dispatch(bus, 200)) {}
    if (!responded) out("RESPONSE TIMEOUT");
}
static void dict_entry_s(DBusMessageIter* dict, const char* key, const char* value) {
    DBusMessageIter e, v; dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e); dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &value); dbus_message_iter_close_container(&e, &v); dbus_message_iter_close_container(dict, &e);
}
static void dict_entry_b(DBusMessageIter* dict, const char* key, bool value) {
    DBusMessageIter e, v; dbus_bool_t b = value; dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e); dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &v); dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b); dbus_message_iter_close_container(&e, &v); dbus_message_iter_close_container(dict, &e);
}
static void dict_entry_ay(DBusMessageIter* dict, const char* key, const char* path) {
    DBusMessageIter e, v, a; dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e); dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "ay", &v); dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "y", &a);
    const char* bytes = path; dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_BYTE, &bytes, (int)strlen(path) + 1);
    dbus_message_iter_close_container(&v, &a); dbus_message_iter_close_container(&e, &v); dbus_message_iter_close_container(dict, &e);
}
static void hostile(void) {
    char error[600]; const char* s = "x"; dbus_uint32_t u = 99;
    struct { const char* dest; const char* path; const char* iface; const char* member; } calls[] = {
        {"org.freedesktop.impl.portal.desktop.aurora", OBJ, "org.freedesktop.impl.portal.Screenshot", "Screenshot"},
        {"org.freedesktop.impl.portal.desktop.aurora", OBJ, "org.freedesktop.impl.portal.FileChooser", "OpenFile"},
        {"org.freedesktop.impl.portal.desktop.aurora", OBJ, "org.freedesktop.impl.portal.Settings", "Read"},
        {"org.freedesktop.impl.portal.desktop.aurora", OBJ, "org.freedesktop.impl.portal.Inhibit", "Inhibit"},
        {"org.freedesktop.impl.portal.desktop.aurora", OBJ, "org.freedesktop.impl.portal.Access", "AccessDialog"},
        {"org.freedesktop.impl.portal.desktop.aurora", OBJ, "org.freedesktop.impl.portal.Notification", "AddNotification"}};
    for (unsigned i = 0; i < sizeof(calls) / sizeof(*calls); i++) {   /* wrong signatures straight to the backend */
        DBusMessage* m = dbus_message_new_method_call(calls[i].dest, calls[i].path, calls[i].iface, calls[i].member);
        dbus_message_append_args(m, DBUS_TYPE_STRING, &s, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INVALID);
        DBusMessage* r = call(m, error, sizeof(error));
        out("HOSTILE %s.%s %s %s", strrchr(calls[i].iface, '.') + 1, calls[i].member, !r && strstr(error, "InvalidArgs") ? "PASS" : "FAIL", r ? "accepted" : error); if (r) dbus_message_unref(r);
    }
    /* a FileChooser call with a bad handle path and a current_folder without its NUL */
    DBusMessage* m = dbus_message_new_method_call("org.freedesktop.impl.portal.desktop.aurora", OBJ, "org.freedesktop.impl.portal.FileChooser", "OpenFile");
    const char *handle = "/org/freedesktop/portal/desktop/request/x/hostile1", *app = "", *parent = "", *title = "hostile";
    DBusMessageIter it, dict; dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &handle); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    { DBusMessageIter e, v, a; const char* key = "current_folder"; dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e); dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
      dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "ay", &v); dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "y", &a);
      const char* bytes = "/etc/../root"; dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_BYTE, &bytes, 12); dbus_message_iter_close_container(&v, &a);
      dbus_message_iter_close_container(&e, &v); dbus_message_iter_close_container(&dict, &e); }
    dict_entry_s(&dict, "accept_label", "_Op\x01en"); dbus_message_iter_close_container(&it, &dict);
    DBusPendingCall* pending = NULL; dbus_connection_send_with_reply(bus, m, &pending, 30000); dbus_message_unref(m); dbus_connection_flush(bus);
    usleep(1500000);   /* the dialog is up: close the request from outside, as the frontend does */
    DBusMessage* close_call = dbus_message_new_method_call("org.freedesktop.impl.portal.desktop.aurora", handle, "org.freedesktop.impl.portal.Request", "Close");
    DBusMessage* cr = call(close_call, error, sizeof(error)); if (cr) dbus_message_unref(cr);
    dbus_pending_call_block(pending); DBusMessage* r = dbus_pending_call_steal_reply(pending); dbus_pending_call_unref(pending);
    dbus_uint32_t code = 77; if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_METHOD_RETURN) { DBusMessageIter ri; dbus_message_iter_init(r, &ri); dbus_message_iter_get_basic(&ri, &code); }
    out("HOSTILE request-closed-mid-dialog %s response=%u", code == 2 ? "PASS" : "FAIL", code); if (r) dbus_message_unref(r);
    out("HOSTILE DONE");
}
int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: portal-test settings|watch S|screenshot [interactive]|open [multiple|directory]|save NAME FOLDER|inhibit FLAGS S|openuri URI|openfile PATH|notify T B|hostile\n"); return 2; }
    DBusError e; dbus_error_init(&e); bus = dbus_bus_get(DBUS_BUS_SESSION, &e);
    if (!bus) { out("ERROR no session bus: %s", e.message); return 1; }
    dbus_connection_add_filter(bus, filter, NULL, NULL);
    char error[600]; const char* cmd = argv[1];
    if (!strcmp(cmd, "settings")) {
        DBusMessage* m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.Settings", "ReadAll");
        DBusMessageIter it, arr; dbus_message_iter_init_append(m, &it); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
        const char* ns = "org.freedesktop.appearance"; dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &ns); ns = "org.gnome.desktop.*"; dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &ns);
        dbus_message_iter_close_container(&it, &arr);
        DBusMessage* r = call(m, error, sizeof(error)); if (r) { dump("READALL", r); dbus_message_unref(r); } else out("READALL ERROR %s", error);
        const char *n2 = "org.freedesktop.appearance", *key = "color-scheme";
        m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.Settings", "ReadOne"); dbus_message_append_args(m, DBUS_TYPE_STRING, &n2, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
        r = call(m, error, sizeof(error)); if (r) { dump("READONE color-scheme", r); dbus_message_unref(r); } else out("READONE ERROR %s", error);
        key = "no-such-key"; m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.Settings", "ReadOne"); dbus_message_append_args(m, DBUS_TYPE_STRING, &n2, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
        r = call(m, error, sizeof(error)); out("READONE unknown %s", r ? "accepted" : error); if (r) dbus_message_unref(r);
        return 0;
    }
    if (!strcmp(cmd, "watch") && argc > 2) {
        dbus_bus_add_match(bus, "type='signal',interface='org.freedesktop.portal.Settings',member='SettingChanged'", NULL); out("WATCHING");
        uint64_t end = ms() + (uint64_t)atoi(argv[2]) * 1000; while (ms() < end && dbus_connection_read_write_dispatch(bus, 200)) {}
        out("WATCH END"); return 0;
    }
    if (!strcmp(cmd, "screenshot") || !strcmp(cmd, "open") || !strcmp(cmd, "save") || !strcmp(cmd, "openuri") || (!strcmp(cmd, "openfile") && argc > 2)) {
        const char* tok = token(); expect_request(tok); const char* parent = "";
        DBusMessage* m; DBusMessageIter it, dict;
        if (!strcmp(cmd, "screenshot")) { m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.Screenshot", "Screenshot"); dbus_message_iter_init_append(m, &it); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent); }
        else if (!strcmp(cmd, "openuri")) { m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.OpenURI", "OpenURI"); dbus_message_iter_init_append(m, &it); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent);
            const char* uri = argc > 2 ? argv[2] : "file:///etc/hostname"; dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &uri); }
        else if (!strcmp(cmd, "openfile")) {   /* OpenURI with a file URI asks for x-scheme-handler/file; files go by descriptor */
            int fd = open(argv[2], O_RDONLY | O_CLOEXEC); if (fd < 0) { out("openfile ERROR %s", strerror(errno)); return 0; }
            m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.OpenURI", "OpenFile"); dbus_message_iter_init_append(m, &it);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent); dbus_message_iter_append_basic(&it, DBUS_TYPE_UNIX_FD, &fd); }
        else { m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.FileChooser", !strcmp(cmd, "save") ? "SaveFile" : "OpenFile"); dbus_message_iter_init_append(m, &it);
            const char* title = !strcmp(cmd, "save") ? "Save Report" : "Open Report"; dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title); }
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict); dict_entry_s(&dict, "handle_token", tok);
        if (!strcmp(cmd, "screenshot")) dict_entry_b(&dict, "interactive", argc > 2 && !strcmp(argv[2], "interactive"));
        for (int i = 2; !strcmp(cmd, "open") && i < argc; i++) {   /* multiple | directory | /first/folder */
            if (argv[i][0] == '/') dict_entry_ay(&dict, "current_folder", argv[i]); else dict_entry_b(&dict, !strcmp(argv[i], "multiple") ? "multiple" : "directory", true); }
        if (!strcmp(cmd, "save") && argc > 3) { dict_entry_s(&dict, "current_name", argv[2]); dict_entry_ay(&dict, "current_folder", argv[3]); }
        dbus_message_iter_close_container(&it, &dict);
        DBusMessage* r = call(m, error, sizeof(error)); if (!r) { out("%s ERROR %s", cmd, error); return 0; }
        dump("HANDLE", r); dbus_message_unref(r);
        wait_response(180); return 0;
    }
    if (!strcmp(cmd, "inhibit") && argc > 3) {
        const char* tok = token(); const char* window = ""; dbus_uint32_t flags = (dbus_uint32_t)atoi(argv[2]);
        DBusMessage* m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.Inhibit", "Inhibit"); DBusMessageIter it, dict; dbus_message_iter_init_append(m, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &window); dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &flags);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict); dict_entry_s(&dict, "handle_token", tok); dict_entry_s(&dict, "reason", "portal-test video"); dbus_message_iter_close_container(&it, &dict);
        DBusMessage* r = call(m, error, sizeof(error)); if (!r) { out("INHIBIT ERROR %s", error); return 0; }
        const char* path = NULL; DBusError e2; dbus_error_init(&e2); dbus_message_get_args(r, &e2, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID); dbus_error_free(&e2);
        char handle[256]; snprintf(handle, sizeof(handle), "%s", path ? path : ""); dbus_message_unref(r); out("INHIBIT HANDLE %s", handle);
        sleep((unsigned)atoi(argv[3]));
        DBusMessage* c = dbus_message_new_method_call(DEST, handle, "org.freedesktop.portal.Request", "Close"); r = call(c, error, sizeof(error));
        out("INHIBIT CLOSED %s", r ? "ok" : error); if (r) dbus_message_unref(r); return 0;
    }
    if (!strcmp(cmd, "notify") && argc > 3) {
        const char* id = "portal-test-1"; DBusMessage* m = dbus_message_new_method_call(DEST, OBJ, "org.freedesktop.portal.Notification", "AddNotification"); DBusMessageIter it, dict;
        dbus_message_iter_init_append(m, &it); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &id); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_entry_s(&dict, "title", argv[2]); dict_entry_s(&dict, "body", argv[3]); dbus_message_iter_close_container(&it, &dict);
        DBusMessage* r = call(m, error, sizeof(error)); out("NOTIFY %s", r ? "ok" : error); if (r) dbus_message_unref(r); return 0;
    }
    if (!strcmp(cmd, "hostile")) { hostile(); return 0; }
    fprintf(stderr, "portal-test: unknown command\n"); return 2;
}
