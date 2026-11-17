/* upower-test: the UPower / power-profiles D-Bus APIs of nexus-upowerd as their clients use them
 * (tests/test_x64_upower.py runs it from the test hook on an installed system, root unless said otherwise).
 *
 *   upower-test check              objects, methods, properties and their agreement with sysfs; profiles (Set, holds,
 *                                  a holder that leaves the bus, the legacy name), hostile calls; prints UPTEST lines
 *   upower-test watch SECONDS      prints every PropertiesChanged / DeviceAdded / DeviceRemoved / ProfileReleased signal
 *                                  of the daemon as "UPTEST SIGNAL ..." until SECONDS pass or "stop" arrives on stdin
 *   upower-test state              one line with OnBattery and the display device (the harness compares it with what it
 *                                  set on the simulated hardware)
 *   upower-test set PROFILE        Set ActiveProfile (exit 0) - the harness runs it as an unprivileged user (denied: 3)
 * Failures print "UPTEST FAIL: ..."; check ends with "UPTEST DONE failures=N". */
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

#define UP "org.freedesktop.UPower"
#define UP_PATH "/org/freedesktop/UPower"
#define DEV "org.freedesktop.UPower.Device"
#define PP "org.freedesktop.UPower.PowerProfiles"
#define PP_PATH "/org/freedesktop/UPower/PowerProfiles"
#define PROPS "org.freedesktop.DBus.Properties"
static int failures;
static DBusConnection* bus;
static void fail(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("UPTEST FAIL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); failures++; }
static void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("UPTEST "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); }
static bool read_text(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY); out[0] = 0; if (fd < 0) return false;
    ssize_t n = read(fd, out, size - 1); close(fd); if (n < 0) return false;
    out[n] = 0; while (n && out[n - 1] == '\n') out[--n] = 0; return true;
}
static long long read_number(const char* path) { char t[64]; return read_text(path, t, sizeof(t)) ? atoll(t) : -1; }

/* ---- calls ---- */
static DBusMessage* call(DBusConnection* c, const char* dest, const char* path, const char* iface, const char* member, char* error_name, int first, ...) {
    DBusMessage* m = dbus_message_new_method_call(dest, path, iface, member); if (!m) return NULL;
    va_list ap; va_start(ap, first);
    if (first != DBUS_TYPE_INVALID) dbus_message_append_args_valist(m, first, ap);
    va_end(ap);
    DBusError e; dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 10000, &e);
    dbus_message_unref(m);
    if (error_name) error_name[0] = 0;
    if (!r) { if (error_name) snprintf(error_name, 128, "%s", e.name ? e.name : "?"); dbus_error_free(&e); }
    return r;
}
/* Get a property; the variant's basic value copied into out (string types into text). */
struct value { int type; dbus_bool_t b; uint32_t u; int32_t i; int64_t x; uint64_t t; double d; char s[256]; };
static bool get_property(DBusConnection* c, const char* dest, const char* path, const char* iface, const char* name, struct value* v, char* error_name) {
    DBusMessage* r = call(c, dest, path, PROPS, "Get", error_name, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    if (!r) return false;
    DBusMessageIter it, variant; memset(v, 0, sizeof(*v));
    bool ok = dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT;
    if (ok) {
        dbus_message_iter_recurse(&it, &variant); v->type = dbus_message_iter_get_arg_type(&variant);
        switch (v->type) {
        case DBUS_TYPE_STRING: case DBUS_TYPE_OBJECT_PATH: { const char* s; dbus_message_iter_get_basic(&variant, &s); snprintf(v->s, sizeof(v->s), "%s", s); break; }
        case DBUS_TYPE_BOOLEAN: dbus_message_iter_get_basic(&variant, &v->b); break;
        case DBUS_TYPE_UINT32: dbus_message_iter_get_basic(&variant, &v->u); break;
        case DBUS_TYPE_INT32: dbus_message_iter_get_basic(&variant, &v->i); break;
        case DBUS_TYPE_INT64: dbus_message_iter_get_basic(&variant, &v->x); break;
        case DBUS_TYPE_UINT64: dbus_message_iter_get_basic(&variant, &v->t); break;
        case DBUS_TYPE_DOUBLE: dbus_message_iter_get_basic(&variant, &v->d); break;
        default: break;   /* containers: the type is enough */
        }
    }
    dbus_message_unref(r); return ok;
}
static bool set_profile(DBusConnection* c, const char* dest, const char* path, const char* iface, const char* profile, char* error_name) {
    DBusMessage* m = dbus_message_new_method_call(dest, path, PROPS, "Set"); if (!m) return false;
    const char* name = "ActiveProfile"; DBusMessageIter it, variant;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &variant); dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &profile);
    dbus_message_iter_close_container(&it, &variant);
    DBusError e; dbus_error_init(&e); DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 10000, &e); dbus_message_unref(m);
    if (error_name) error_name[0] = 0;
    if (!r) { if (error_name) snprintf(error_name, 128, "%s", e.name ? e.name : "?"); dbus_error_free(&e); return false; }
    dbus_message_unref(r); return true;
}
static DBusConnection* connect_bus(void) {
    DBusError e; dbus_error_init(&e);
    DBusConnection* c = dbus_bus_get_private(getenv("UPTEST_SESSION") ? DBUS_BUS_SESSION : DBUS_BUS_SYSTEM, &e);
    if (!c) { fail("no system bus: %s", e.message); dbus_error_free(&e); }
    else dbus_connection_set_exit_on_disconnect(c, FALSE);
    return c;
}
static int paths(DBusConnection* c, const char* member, char out[][160], int max) {
    char err[128]; DBusMessage* r = call(c, UP, UP_PATH, UP, member, err, DBUS_TYPE_INVALID);
    if (!r) { fail("%s: %s", member, err); return -1; }
    DBusMessageIter it, array; int n = 0;
    dbus_message_iter_init(r, &it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) { const char* p; dbus_message_iter_get_basic(&it, &p); snprintf(out[0], 160, "%s", p); n = 1; }
    else if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_OBJECT_PATH && n < max) {
            const char* p; dbus_message_iter_get_basic(&array, &p); snprintf(out[n++], 160, "%s", p); dbus_message_iter_next(&array);
        }
    }
    dbus_message_unref(r); return n;
}
/* Wait up to ms for a PropertiesChanged on `path` naming `property`. */
static bool wait_changed(DBusConnection* c, const char* path, const char* property, int ms) {
    uint64_t end; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); end = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000 + (uint64_t)ms;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t); uint64_t now = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
        if (now >= end) return false;
        dbus_connection_read_write(c, (int)(end - now));
        DBusMessage* m;
        while ((m = dbus_connection_pop_message(c))) {
            bool hit = false;
            if (dbus_message_is_signal(m, PROPS, "PropertiesChanged") && !strcmp(dbus_message_get_path(m), path)) {
                DBusMessageIter it, dict, entry; dbus_message_iter_init(m, &it); dbus_message_iter_next(&it);
                dbus_message_iter_recurse(&it, &dict);
                while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                    const char* key; dbus_message_iter_recurse(&dict, &entry); dbus_message_iter_get_basic(&entry, &key);
                    if (!strcmp(key, property)) hit = true;
                    dbus_message_iter_next(&dict);
                }
            }
            dbus_message_unref(m);
            if (hit) return true;
        }
    }
}

/* ---- check ---- */
static void check_device(DBusConnection* c, const char* path) {
    struct value v; char err[128], native[128] = "";
    if (!get_property(c, UP, path, DEV, "NativePath", &v, err)) { fail("%s NativePath: %s", path, err); return; }
    snprintf(native, sizeof(native), "%s", v.s);
    char sys[256], type[64] = "", status[64] = ""; snprintf(sys, sizeof(sys), "/sys/class/power_supply/%s/type", native); read_text(sys, type, sizeof(type));
    get_property(c, UP, path, DEV, "Type", &v, err); uint32_t kind = v.u;
    if (!strcmp(type, "Battery") ? kind != 2 : !strcmp(type, "Mains") ? kind != 1 : true) fail("%s: Type %u for sysfs type '%s'", path, kind, type);
    if (kind == 2) {
        snprintf(sys, sizeof(sys), "/sys/class/power_supply/%s/capacity", native); long long capacity = read_number(sys);
        get_property(c, UP, path, DEV, "Percentage", &v, err);
        if (capacity >= 0 && (v.d < capacity - 0.5 || v.d > capacity + 0.5)) fail("%s: Percentage %.1f, capacity %lld", path, v.d, capacity);
        double percentage = v.d;
        snprintf(sys, sizeof(sys), "/sys/class/power_supply/%s/status", native); read_text(sys, status, sizeof(status));
        get_property(c, UP, path, DEV, "State", &v, err);
        uint32_t want = !strcmp(status, "Charging") ? 1 : !strcmp(status, "Discharging") ? 2 : !strcmp(status, "Full") ? 4 : !strcmp(status, "Not charging") ? 5 : 0;
        if (want == 2 && percentage <= 0) want = 3;
        if (v.u != want) fail("%s: State %u for status '%s'", path, v.u, status);
        snprintf(sys, sizeof(sys), "/sys/class/power_supply/%s/energy_now", native); long long energy = read_number(sys);
        if (energy < 0) { snprintf(sys, sizeof(sys), "/sys/class/power_supply/%s/charge_now", native); energy = read_number(sys); }
        get_property(c, UP, path, DEV, "Energy", &v, err);
        if (energy > 0 && v.d <= 0) fail("%s: Energy %.3f Wh with sysfs %lld", path, v.d, energy);
        get_property(c, UP, path, DEV, "IsPresent", &v, err); if (v.type != DBUS_TYPE_BOOLEAN) fail("%s: IsPresent not a boolean", path);
        get_property(c, UP, path, DEV, "IconName", &v, err); if (!strstr(v.s, "battery")) fail("%s: IconName '%s'", path, v.s);
        get_property(c, UP, path, DEV, "TimeToEmpty", &v, err); if (v.type != DBUS_TYPE_INT64) fail("%s: TimeToEmpty type %c", path, v.type);
        say("DEVICE %s battery %s percentage=%.0f state=%u", path, native, percentage, want);
    } else {
        snprintf(sys, sizeof(sys), "/sys/class/power_supply/%s/online", native); long long online = read_number(sys);
        get_property(c, UP, path, DEV, "Online", &v, err); if ((bool)v.b != (online == 1)) fail("%s: Online %d, sysfs %lld", path, v.b, online);
        say("DEVICE %s line-power %s online=%d", path, native, v.b);
    }
    /* GetAll answers every property UPower clients read */
    const char* iface = DEV; DBusMessage* r = call(c, UP, path, PROPS, "GetAll", err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    if (!r) { fail("%s GetAll: %s", path, err); return; }
    DBusMessageIter it, dict; int count = 0; dbus_message_iter_init(r, &it); dbus_message_iter_recurse(&it, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) { count++; dbus_message_iter_next(&dict); }
    dbus_message_unref(r);
    if (count < 30) fail("%s GetAll: %d properties", path, count);
}
static int check(void) {
    char err[128]; struct value v; char list[16][160];
    /* Introspection: the manager object and the tree */
    DBusMessage* r = call(bus, UP, UP_PATH, "org.freedesktop.DBus.Introspectable", "Introspect", err, DBUS_TYPE_INVALID);
    if (!r) fail("Introspect: %s", err);
    else { const char* xml = ""; dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID); if (!strstr(xml, "EnumerateDevices") || !strstr(xml, "devices")) fail("Introspect lacks EnumerateDevices"); dbus_message_unref(r); }
    if (!get_property(bus, UP, UP_PATH, UP, "DaemonVersion", &v, err)) fail("DaemonVersion: %s", err); else say("DaemonVersion %s", v.s);
    int n = paths(bus, "EnumerateDevices", list, 16);
    int supplies = 0; { FILE* p = popen("ls /sys/class/power_supply 2>/dev/null | wc -l", "r"); if (p) { if (fscanf(p, "%d", &supplies) != 1) supplies = 0; pclose(p); } }
    say("EnumerateDevices %d (sysfs power_supply entries %d)", n, supplies);
    if (n != supplies) fail("EnumerateDevices returned %d devices, sysfs has %d", n, supplies);
    bool any_ac = false, discharging = false;
    for (int i = 0; i < n; i++) {
        check_device(bus, list[i]);
        get_property(bus, UP, list[i], DEV, "Type", &v, err); uint32_t kind = v.u;
        if (kind == 1) { get_property(bus, UP, list[i], DEV, "Online", &v, err); if (v.b) any_ac = true; }
        if (kind == 2) { get_property(bus, UP, list[i], DEV, "State", &v, err); if (v.u == 2 || v.u == 3) discharging = true; }
    }
    if (paths(bus, "GetDisplayDevice", list, 1) != 1 || strcmp(list[0], "/org/freedesktop/UPower/devices/DisplayDevice")) fail("GetDisplayDevice");
    get_property(bus, UP, "/org/freedesktop/UPower/devices/DisplayDevice", DEV, "IsPresent", &v, err); say("DisplayDevice IsPresent=%d", v.b);
    get_property(bus, UP, UP_PATH, UP, "OnBattery", &v, err);
    if ((bool)v.b != (!any_ac && discharging)) fail("OnBattery %d (AC online %d, discharging %d)", v.b, any_ac, discharging);
    get_property(bus, UP, UP_PATH, UP, "LidIsPresent", &v, err); bool lid = v.b;
    char lid_state[32] = ""; read_text("/sys/power/policy/lid_state", lid_state, sizeof(lid_state));
    if (lid != (strcmp(lid_state, "absent") != 0)) fail("LidIsPresent %d, kernel lid_state %s", lid, lid_state);
    r = call(bus, UP, UP_PATH, UP, "GetCriticalAction", err, DBUS_TYPE_INVALID);
    if (!r) fail("GetCriticalAction: %s", err); else { const char* a = ""; dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &a, DBUS_TYPE_INVALID); say("GetCriticalAction %s", a); dbus_message_unref(r); }

    /* hostile calls */
    if (call(bus, UP, "/org/freedesktop/UPower/devices/nothing_here", PROPS, "GetAll", err, DBUS_TYPE_INVALID) || !strstr(err, "Unknown") ) fail("GetAll on a missing object: %s", err);
    const char* wrong = "org.example.Nope", *prop = "OnBattery";
    if (call(bus, UP, UP_PATH, PROPS, "Get", err, DBUS_TYPE_STRING, &wrong, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID) || !strstr(err, "UnknownInterface")) fail("Get on a wrong interface: %s", err);
    const char* up = UP, *nope = "NoSuchProperty";
    if (call(bus, UP, UP_PATH, PROPS, "Get", err, DBUS_TYPE_STRING, &up, DBUS_TYPE_STRING, &nope, DBUS_TYPE_INVALID) || !strstr(err, "UnknownProperty")) fail("Get of an unknown property: %s", err);
    uint32_t bogus = 5;
    if (call(bus, UP, UP_PATH, PROPS, "Get", err, DBUS_TYPE_UINT32, &bogus, DBUS_TYPE_INVALID) || !strstr(err, "InvalidArgs")) fail("Get(u): %s", err);
    if (call(bus, UP, UP_PATH, UP, "NoSuchMethod", err, DBUS_TYPE_INVALID) || !strstr(err, "UnknownMethod")) fail("unknown method: %s", err);
    for (int i = 0; i < 200; i++) if (!get_property(bus, UP, UP_PATH, UP, "OnBattery", &v, err)) { fail("burst Get %d: %s", i, err); break; }

    /* power profiles */
    char kernel[64] = "", choices[128] = "";
    read_text("/sys/power/policy/profile", kernel, sizeof(kernel)); read_text("/sys/power/policy/profile_choices", choices, sizeof(choices));
    if (!get_property(bus, PP, PP_PATH, PP, "ActiveProfile", &v, err)) fail("ActiveProfile: %s", err);
    else if (strcmp(v.s, kernel)) fail("ActiveProfile %s, kernel %s", v.s, kernel);
    say("PROFILE active=%s choices=%s", v.s, choices);
    get_property(bus, PP, PP_PATH, PP, "Profiles", &v, err); if (v.type != DBUS_TYPE_ARRAY) fail("Profiles is not an array");
    get_property(bus, PP, PP_PATH, PP, "Version", &v, err); say("PowerProfiles Version %s", v.s);
    bool performance = strstr(choices, "performance") != NULL;
    const char* order[] = {"power-saver", "performance", "balanced"};
    for (int i = 0; i < 3; i++) {
        if (!strcmp(order[i], "performance") && !performance) {
            if (set_profile(bus, PP, PP_PATH, PP, "performance", err)) fail("performance accepted without a CPU driver");
            continue;
        }
        if (!set_profile(bus, PP, PP_PATH, PP, order[i], err)) { fail("Set ActiveProfile %s: %s", order[i], err); continue; }
        read_text("/sys/power/policy/profile", kernel, sizeof(kernel));
        if (strcmp(kernel, order[i])) fail("after Set %s the kernel profile is %s", order[i], kernel);
        char governor[64] = ""; read_text("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", governor, sizeof(governor));
        say("PROFILE set %s -> kernel %s governor %s", order[i], kernel, governor[0] ? governor : "(no cpufreq)");
        if (governor[0]) {
            const char* want = !strcmp(order[i], "performance") ? "performance" : !strcmp(order[i], "power-saver") ? "powersave" : NULL;
            if (want && strcmp(governor, want)) fail("profile %s: governor %s", order[i], governor);
        }
    }
    if (set_profile(bus, PP, PP_PATH, PP, "turbo", err) || !strstr(err, "InvalidArgs")) fail("Set ActiveProfile turbo: %s", err);
    const char* ppi = PP, *version = "Version";
    { DBusMessage* m = dbus_message_new_method_call(PP, PP_PATH, PROPS, "Set"); DBusMessageIter it, var; dbus_message_iter_init_append(m, &it);
      dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &ppi); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &version);
      const char* x = "9"; dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var); dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &x); dbus_message_iter_close_container(&it, &var);
      DBusError e; dbus_error_init(&e); DBusMessage* rr = dbus_connection_send_with_reply_and_block(bus, m, 5000, &e); dbus_message_unref(m);
      if (rr || !e.name || !strstr(e.name, "PropertyReadOnly")) fail("Set Version: %s", e.name ? e.name : "accepted"); if (rr) dbus_message_unref(rr); dbus_error_free(&e); }
    { DBusMessage* m = dbus_message_new_method_call(PP, PP_PATH, PROPS, "Set"); DBusMessageIter it, var; dbus_message_iter_init_append(m, &it);
      const char* active = "ActiveProfile"; dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &ppi); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &active);
      uint32_t number = 2; dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "u", &var); dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &number); dbus_message_iter_close_container(&it, &var);
      DBusError e; dbus_error_init(&e); DBusMessage* rr = dbus_connection_send_with_reply_and_block(bus, m, 5000, &e); dbus_message_unref(m);
      if (rr || !e.name || !strstr(e.name, "InvalidArgs")) fail("Set ActiveProfile with a number: %s", e.name ? e.name : "accepted"); if (rr) dbus_message_unref(rr); dbus_error_free(&e); }

    /* holds: performance (if available) and power-saver from a second connection that then leaves */
    const char* hold = performance ? "performance" : "power-saver", *reason = "upower-test", *app = "org.nexusos.UpowerTest";
    r = call(bus, PP, PP_PATH, PP, "HoldProfile", err, DBUS_TYPE_STRING, &hold, DBUS_TYPE_STRING, &reason, DBUS_TYPE_STRING, &app, DBUS_TYPE_INVALID);
    uint32_t cookie = 0;
    if (!r) fail("HoldProfile %s: %s", hold, err);
    else { dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID); dbus_message_unref(r); }
    get_property(bus, PP, PP_PATH, PP, "ActiveProfile", &v, err); if (strcmp(v.s, hold)) fail("with a %s hold ActiveProfile is %s", hold, v.s);
    say("HOLD %s cookie=%u active=%s", hold, cookie, v.s);
    DBusConnection* other = connect_bus();
    if (other && performance) {
        const char* saver = "power-saver";
        r = call(other, PP, PP_PATH, PP, "HoldProfile", err, DBUS_TYPE_STRING, &saver, DBUS_TYPE_STRING, &reason, DBUS_TYPE_STRING, &app, DBUS_TYPE_INVALID);
        if (!r) fail("second HoldProfile: %s", err); else dbus_message_unref(r);
        get_property(bus, PP, PP_PATH, PP, "ActiveProfile", &v, err); if (strcmp(v.s, "power-saver")) fail("power-saver hold does not win: %s", v.s);
        dbus_connection_close(other); dbus_connection_unref(other); other = NULL;
        usleep(500000);
        get_property(bus, PP, PP_PATH, PP, "ActiveProfile", &v, err); if (strcmp(v.s, "performance")) fail("after the holder left, ActiveProfile %s", v.s);
        say("HOLD second holder left -> %s", v.s);
    } else if (other) { dbus_connection_close(other); dbus_connection_unref(other); }
    const char* balanced = "balanced", *saver = "power-saver";
    if (call(bus, PP, PP_PATH, PP, "HoldProfile", err, DBUS_TYPE_STRING, &balanced, DBUS_TYPE_STRING, &reason, DBUS_TYPE_STRING, &app, DBUS_TYPE_INVALID) || !strstr(err, "InvalidArgs")) fail("HoldProfile balanced: %s", err);
    uint32_t nonexistent = 0x7FFFFFF0U;
    if (call(bus, PP, PP_PATH, PP, "ReleaseProfile", err, DBUS_TYPE_UINT32, &nonexistent, DBUS_TYPE_INVALID) || !strstr(err, "InvalidArgs")) fail("ReleaseProfile of no hold: %s", err);
    r = call(bus, PP, PP_PATH, PP, "ReleaseProfile", err, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID);
    if (!r) fail("ReleaseProfile: %s", err); else dbus_message_unref(r);
    get_property(bus, PP, PP_PATH, PP, "ActiveProfile", &v, err); if (strcmp(v.s, "balanced")) fail("after ReleaseProfile ActiveProfile %s", v.s);
    /* the legacy name and path */
    if (!get_property(bus, "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles", "ActiveProfile", &v, err)) fail("net.hadess.PowerProfiles: %s", err);
    else say("LEGACY net.hadess.PowerProfiles active=%s", v.s);
    /* Set fires PropertiesChanged */
    dbus_bus_add_match(bus, "type='signal',sender='" PP "',interface='" PROPS "',member='PropertiesChanged'", NULL); dbus_connection_flush(bus);
    if (!set_profile(bus, PP, PP_PATH, PP, saver, err)) fail("Set power-saver: %s", err);
    if (!wait_changed(bus, PP_PATH, "ActiveProfile", 5000)) fail("no PropertiesChanged(ActiveProfile) after Set");
    set_profile(bus, PP, PP_PATH, PP, balanced, err);
    say("DONE failures=%d", failures);
    return failures ? 1 : 0;
}

/* ---- watch / state / set ---- */
static int watch(int seconds) {
    dbus_bus_add_match(bus, "type='signal',sender='" UP "'", NULL);
    dbus_bus_add_match(bus, "type='signal',sender='" PP "'", NULL);
    dbus_connection_flush(bus);
    say("WATCHING %d", seconds); fflush(stdout);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); time_t end = t.tv_sec + seconds;
    bool input = true;   /* a background job's stdin is /dev/null: its end of file is no "stop" */
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t); if (t.tv_sec >= end) break;
        struct pollfd in = {0, POLLIN, 0};
        if (input && poll(&in, 1, 0) > 0) { char line[32]; if (!fgets(line, sizeof(line), stdin)) input = false; else if (!strncmp(line, "stop", 4)) break; }
        dbus_connection_read_write(bus, 500);
        DBusMessage* m;
        while ((m = dbus_connection_pop_message(bus))) {
            if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_SIGNAL) {
                char detail[512] = ""; size_t used = 0;
                DBusMessageIter it; if (dbus_message_iter_init(m, &it)) {
                    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING || dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) { const char* s; dbus_message_iter_get_basic(&it, &s); used += (size_t)snprintf(detail + used, sizeof(detail) - used, "%s", s); }
                    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) { uint32_t u; dbus_message_iter_get_basic(&it, &u); used += (size_t)snprintf(detail + used, sizeof(detail) - used, "%u", u); }
                    if (dbus_message_is_signal(m, PROPS, "PropertiesChanged") && dbus_message_iter_next(&it)) {
                        DBusMessageIter dict, entry; dbus_message_iter_recurse(&it, &dict);
                        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY && used < sizeof(detail) - 40) {
                            const char* key; dbus_message_iter_recurse(&dict, &entry); dbus_message_iter_get_basic(&entry, &key);
                            used += (size_t)snprintf(detail + used, sizeof(detail) - used, " %s", key);
                            DBusMessageIter var; dbus_message_iter_next(&entry); dbus_message_iter_recurse(&entry, &var);
                            int type = dbus_message_iter_get_arg_type(&var);
                            if (type == DBUS_TYPE_BOOLEAN) { dbus_bool_t b; dbus_message_iter_get_basic(&var, &b); used += (size_t)snprintf(detail + used, sizeof(detail) - used, "=%d", b); }
                            else if (type == DBUS_TYPE_DOUBLE) { double d; dbus_message_iter_get_basic(&var, &d); used += (size_t)snprintf(detail + used, sizeof(detail) - used, "=%.1f", d); }
                            else if (type == DBUS_TYPE_UINT32) { uint32_t u; dbus_message_iter_get_basic(&var, &u); used += (size_t)snprintf(detail + used, sizeof(detail) - used, "=%u", u); }
                            else if (type == DBUS_TYPE_STRING) { const char* s; dbus_message_iter_get_basic(&var, &s); used += (size_t)snprintf(detail + used, sizeof(detail) - used, "=%s", s); }
                            dbus_message_iter_next(&dict);
                        }
                    }
                }
                say("SIGNAL %s %s %s", dbus_message_get_path(m), dbus_message_get_member(m), detail);
            }
            dbus_message_unref(m);
        }
    }
    say("WATCH END"); return 0;
}
static int state(void) {
    struct value v; char err[128];
    get_property(bus, UP, UP_PATH, UP, "OnBattery", &v, err); bool battery = v.b;
    const char* d = "/org/freedesktop/UPower/devices/DisplayDevice";
    get_property(bus, UP, d, DEV, "Percentage", &v, err); double percentage = v.d;
    get_property(bus, UP, d, DEV, "State", &v, err); uint32_t st = v.u;
    get_property(bus, UP, d, DEV, "IsPresent", &v, err); bool present = v.b;
    get_property(bus, UP, d, DEV, "WarningLevel", &v, err); uint32_t warning = v.u;
    get_property(bus, UP, d, DEV, "TimeToEmpty", &v, err); int64_t empty = v.x;
    get_property(bus, PP, PP_PATH, PP, "ActiveProfile", &v, err);
    say("STATE on_battery=%d present=%d percentage=%.0f state=%u warning=%u time_to_empty=%lld profile=%s", battery, present, percentage, st, warning, (long long)empty, v.s);
    return 0;
}
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: upower-test check|watch SECONDS|state|set PROFILE\n"); return 2; }
    bus = connect_bus(); if (!bus) return 1;
    if (!strcmp(argv[1], "check")) return check();
    if (!strcmp(argv[1], "watch")) return watch(argc > 2 ? atoi(argv[2]) : 30);
    if (!strcmp(argv[1], "state")) return state();
    if (!strcmp(argv[1], "set") && argc > 2) {
        char err[128];
        if (set_profile(bus, PP, PP_PATH, PP, argv[2], err)) { say("SET %s ok", argv[2]); return 0; }
        say("SET %s denied=%s", argv[2], err); return strstr(err, "AccessDenied") ? 3 : 1;
    }
    return 2;
}
