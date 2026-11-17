/* nexus-notify-test - a Desktop Notifications client (what browsers and toolkits send): GetServerInformation,
 * GetCapabilities, then Notify(app, 0, "", summary, body, [], {"urgency": byte 1}, timeout) on the session bus,
 * waiting for NotificationClosed. Prints "NOTIFY-TEST server=<name> caps=<n> id=<id> closed=<reason>" (tests/test_x64_services.py).
 *   nexus-notify-test SUMMARY BODY [TIMEOUT_MS] */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: nexus-notify-test SUMMARY BODY [TIMEOUT_MS]\n"); return 2; }
    int32_t timeout = argc > 3 ? atoi(argv[3]) : 1500;
    DBusError e; dbus_error_init(&e);
    DBusConnection* bus = dbus_bus_get(DBUS_BUS_SESSION, &e);
    if (!bus) { printf("NOTIFY-TEST error=no-session-bus %s\n", e.message); return 1; }
    dbus_bus_add_match(bus, "type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'", &e);
    const char* dest = "org.freedesktop.Notifications"; const char* path = "/org/freedesktop/Notifications";
    DBusMessage* m = dbus_message_new_method_call(dest, path, dest, "GetServerInformation");
    DBusMessage* r = dbus_connection_send_with_reply_and_block(bus, m, 10000, &e); dbus_message_unref(m);
    if (!r) { printf("NOTIFY-TEST error=GetServerInformation %s\n", e.message); return 1; }
    const char *name = "", *vendor = "", *version = "", *spec = "";
    if (!dbus_message_get_args(r, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &vendor, DBUS_TYPE_STRING, &version, DBUS_TYPE_STRING, &spec, DBUS_TYPE_INVALID)) {
        printf("NOTIFY-TEST error=bad-server-information\n"); return 1;
    }
    char server[128]; snprintf(server, sizeof(server), "%s/%s/%s", name, vendor, spec); dbus_message_unref(r);
    m = dbus_message_new_method_call(dest, path, dest, "GetCapabilities");
    r = dbus_connection_send_with_reply_and_block(bus, m, 10000, &e); dbus_message_unref(m);
    if (!r) { printf("NOTIFY-TEST error=GetCapabilities %s\n", e.message); return 1; }
    char** caps = NULL; int ncaps = 0;
    if (!dbus_message_get_args(r, &e, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &caps, &ncaps, DBUS_TYPE_INVALID)) { printf("NOTIFY-TEST error=bad-capabilities\n"); return 1; }
    dbus_free_string_array(caps); dbus_message_unref(r);
    m = dbus_message_new_method_call(dest, path, dest, "Notify");
    DBusMessageIter it, sub, entry, var;
    const char *app = "nexus-notify-test", *icon = "", *summary = argv[1], *body = argv[2]; uint32_t replaces = 0;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app); dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &summary);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &body);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &sub); dbus_message_iter_close_container(&it, &sub);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &sub);
    dbus_message_iter_open_container(&sub, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char* key = "urgency"; unsigned char urgency = 1;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "y", &var); dbus_message_iter_append_basic(&var, DBUS_TYPE_BYTE, &urgency);
    dbus_message_iter_close_container(&entry, &var); dbus_message_iter_close_container(&sub, &entry); dbus_message_iter_close_container(&it, &sub);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout);
    r = dbus_connection_send_with_reply_and_block(bus, m, 10000, &e); dbus_message_unref(m);
    if (!r) { printf("NOTIFY-TEST error=Notify %s\n", e.message); return 1; }
    uint32_t id = 0;
    if (!dbus_message_get_args(r, &e, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID) || !id) { printf("NOTIFY-TEST error=bad-id\n"); return 1; }
    dbus_message_unref(r);
    uint32_t reason = 0;
    for (int waited = 0; waited < 20000 && !reason; waited += 100) {
        dbus_connection_read_write(bus, 100);
        DBusMessage* s;
        while ((s = dbus_connection_pop_message(bus))) {
            uint32_t sid = 0, why = 0;
            if (dbus_message_is_signal(s, dest, "NotificationClosed") &&
                dbus_message_get_args(s, NULL, DBUS_TYPE_UINT32, &sid, DBUS_TYPE_UINT32, &why, DBUS_TYPE_INVALID) && sid == id) reason = why;
            dbus_message_unref(s);
        }
    }
    printf("NOTIFY-TEST server=%s caps=%d id=%u closed=%u\n", server, ncaps, id, reason);
    return reason ? 0 : 1;
}
