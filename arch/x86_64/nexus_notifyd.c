/* nexus-notifyd - org.freedesktop.Notifications (Desktop Notifications Specification 1.2) on the session bus, shown by
 * Aurora. Started on demand by the session dbus-daemon (/usr/share/dbus-1/services/org.freedesktop.Notifications.service)
 * or by hand. Each Notify call is forwarded as one datagram "<app>\x1f<summary>\x1f<body>" to Aurora's socket
 * $XDG_RUNTIME_DIR/aurora-notify (aurora_services.c shows it in the desktop notice area); after the expiry timeout
 * (default 6 s, Aurora's notice time) NotificationClosed(id, 1) is emitted, CloseNotification emits reason 3.
 * Methods: GetCapabilities -> ["body", "persistence"=no], Notify, CloseNotification, GetServerInformation; standard
 * Introspectable/Peer interfaces. Exits after 10 idle minutes (D-Bus activation restarts it). */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PATH "/org/freedesktop/Notifications"
#define IFACE "org.freedesktop.Notifications"
#define MAX_PENDING 64
static const char* const INTROSPECTION =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node><interface name=\"org.freedesktop.Notifications\">"
    "<method name=\"GetCapabilities\"><arg direction=\"out\" type=\"as\"/></method>"
    "<method name=\"Notify\"><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"u\"/><arg direction=\"in\" type=\"s\"/>"
    "<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"as\"/><arg direction=\"in\" type=\"a{sv}\"/>"
    "<arg direction=\"in\" type=\"i\"/><arg direction=\"out\" type=\"u\"/></method>"
    "<method name=\"CloseNotification\"><arg direction=\"in\" type=\"u\"/></method>"
    "<method name=\"GetServerInformation\"><arg direction=\"out\" type=\"s\"/><arg direction=\"out\" type=\"s\"/><arg direction=\"out\" type=\"s\"/><arg direction=\"out\" type=\"s\"/></method>"
    "<signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
    "<signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal>"
    "</interface><interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg direction=\"out\" type=\"s\"/></method></interface>"
    "<interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/></interface></node>\n";

struct pending { uint32_t id; uint64_t expires; };
static struct pending pending[MAX_PENDING]; static unsigned npending;
static uint32_t next_id = 1;
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }

static void forward(const char* app, const char* summary, const char* body) {
    const char* dir = getenv("XDG_RUNTIME_DIR"); if (!dir || dir[0] != '/') return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if ((size_t)snprintf(a.sun_path, sizeof(a.sun_path), "%s/aurora-notify", dir) >= sizeof(a.sun_path)) return;
    char text[1024]; int n = snprintf(text, sizeof(text), "%s\x1f%s\x1f%s", app, summary, body);
    if (n < 0) return; if ((size_t)n >= sizeof(text)) n = sizeof(text) - 1;
    for (int i = 0; i < n; i++) if ((unsigned char)text[i] < 0x20 && text[i] != 0x1f) text[i] = ' ';
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return;
    if (sendto(fd, text, (size_t)n, MSG_NOSIGNAL, (struct sockaddr*)&a, sizeof(a)) < 0)
        fprintf(stderr, "nexus-notifyd: Aurora is not listening on %s: %s\n", a.sun_path, strerror(errno));
    close(fd);
}
static void closed_signal(DBusConnection* bus, uint32_t id, uint32_t reason) {
    DBusMessage* m = dbus_message_new_signal(PATH, IFACE, "NotificationClosed"); if (!m) return;
    dbus_message_append_args(m, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
    dbus_connection_send(bus, m, NULL); dbus_message_unref(m);
}
static void forget(uint32_t id) {
    for (unsigned i = 0; i < npending; i++) if (pending[i].id == id) { pending[i] = pending[--npending]; return; }
}
static DBusHandlerResult reply_error(DBusConnection* bus, DBusMessage* m, const char* name, const char* text) {
    DBusMessage* r = dbus_message_new_error(m, name, text); if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult handle(DBusConnection* bus, DBusMessage* m, void* data) {
    (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char* member = dbus_message_get_member(m); const char* iface = dbus_message_get_interface(m);
    DBusMessage* r = NULL;
    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        r = dbus_message_new_method_return(m); if (r) dbus_message_append_args(r, DBUS_TYPE_STRING, &INTROSPECTION, DBUS_TYPE_INVALID);
    } else if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Peer", "Ping")) {
        r = dbus_message_new_method_return(m);
    } else if (iface && strcmp(iface, IFACE)) {
        return reply_error(bus, m, DBUS_ERROR_UNKNOWN_INTERFACE, "unknown interface");
    } else if (member && !strcmp(member, "GetCapabilities")) {
        r = dbus_message_new_method_return(m);
        if (r) { const char* caps[] = {"body"}; const char** p = caps;
            dbus_message_append_args(r, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &p, 1, DBUS_TYPE_INVALID); }
    } else if (member && !strcmp(member, "GetServerInformation")) {
        r = dbus_message_new_method_return(m);
        const char *name = "Aurora", *vendor = "NexusOS", *version = "1.0", *spec = "1.2";
        if (r) dbus_message_append_args(r, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &vendor, DBUS_TYPE_STRING, &version, DBUS_TYPE_STRING, &spec, DBUS_TYPE_INVALID);
    } else if (member && !strcmp(member, "Notify")) {
        DBusMessageIter it; const char *app = "", *icon = "", *summary = "", *body = ""; uint32_t replaces = 0; int32_t timeout = -1;
        if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "Notify(susssasa{sv}i)");
        dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "replaces_id");
        dbus_message_iter_get_basic(&it, &replaces); dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "app_icon");
        dbus_message_iter_get_basic(&it, &icon); dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "summary");
        dbus_message_iter_get_basic(&it, &summary); dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "body");
        dbus_message_iter_get_basic(&it, &body); dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "actions");
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "hints");
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INT32) return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "expire_timeout");
        dbus_message_iter_get_basic(&it, &timeout);
        uint32_t id = replaces ? replaces : next_id++;
        if (!next_id) next_id = 1;
        forget(id);
        forward(app, summary, body);
        printf("nexus-notifyd: #%u from %s: %s\n", id, app, summary); fflush(stdout);
        if (npending < MAX_PENDING) { pending[npending].id = id; pending[npending].expires = now_ms() + (uint64_t)(timeout > 0 ? timeout : 6000); npending++; }
        r = dbus_message_new_method_return(m); if (r) dbus_message_append_args(r, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
    } else if (member && !strcmp(member, "CloseNotification")) {
        uint32_t id = 0; DBusError e; dbus_error_init(&e);
        if (!dbus_message_get_args(m, &e, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) { dbus_error_free(&e); return reply_error(bus, m, DBUS_ERROR_INVALID_ARGS, "CloseNotification(u)"); }
        forget(id); closed_signal(bus, id, 3);
        r = dbus_message_new_method_return(m);
    } else return reply_error(bus, m, DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
    if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
int main(void) {
    DBusError e; dbus_error_init(&e);
    DBusConnection* bus = dbus_bus_get(DBUS_BUS_SESSION, &e);
    if (!bus) { fprintf(stderr, "nexus-notifyd: no session bus: %s\n", e.message); return 1; }
    int got = dbus_bus_request_name(bus, IFACE, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (got != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { fprintf(stderr, "nexus-notifyd: cannot own %s: %s\n", IFACE, dbus_error_is_set(&e) ? e.message : "taken"); return 1; }
    static const DBusObjectPathVTable vtable = {.message_function = handle};
    if (!dbus_connection_register_object_path(bus, PATH, &vtable, NULL)) { fprintf(stderr, "nexus-notifyd: out of memory\n"); return 1; }
    uint64_t idle_since = now_ms();
    for (;;) {
        uint64_t t = now_ms(); int wait = 1000;
        for (unsigned i = 0; i < npending;) {
            if (pending[i].expires <= t) { uint32_t id = pending[i].id; pending[i] = pending[--npending]; closed_signal(bus, id, 1); continue; }
            if ((int64_t)(pending[i].expires - t) < wait) wait = (int)(pending[i].expires - t);
            i++;
        }
        dbus_connection_flush(bus);
        if (!dbus_connection_read_write_dispatch(bus, wait)) break;   /* disconnected */
        if (npending) idle_since = now_ms();
        else if (now_ms() - idle_since > 600000) break;
        while (dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS) { dbus_connection_dispatch(bus); idle_since = now_ms(); }
    }
    return 0;
}
