/* nexus-upowerd - the UPower and power-profiles-daemon D-Bus APIs on the system bus (ext4 installs with D-Bus).
 *
 * Names: org.freedesktop.UPower, org.freedesktop.UPower.PowerProfiles and the legacy net.hadess.PowerProfiles.
 * Objects (UPower 1.90 / power-profiles-daemon 0.30 interfaces, what GNOME, KDE, libupower-glib and Chrome's battery
 * status code use):
 *   /org/freedesktop/UPower                           org.freedesktop.UPower: EnumerateDevices, GetDisplayDevice,
 *                                                     GetCriticalAction; DeviceAdded/DeviceRemoved; DaemonVersion,
 *                                                     OnBattery, LidIsClosed, LidIsPresent
 *   /org/freedesktop/UPower/devices/line_power_<N>    org.freedesktop.UPower.Device for every /sys/class/power_supply
 *   /org/freedesktop/UPower/devices/battery_<N>       entry (Mains / Battery), Refresh, GetHistory, GetStatistics
 *   /org/freedesktop/UPower/devices/DisplayDevice     the composite battery the desktop shows
 *   /org/freedesktop/UPower/PowerProfiles             org.freedesktop.UPower.PowerProfiles: ActiveProfile (read/write),
 *   /net/hadess/PowerProfiles                         Profiles, Actions, PerformanceDegraded, PerformanceInhibited,
 *                                                     ActiveProfileHolds, Version; HoldProfile, ReleaseProfile,
 *                                                     ProfileReleased (also as net.hadess.PowerProfiles)
 *   every object: org.freedesktop.DBus.Properties (Get, GetAll, Set, PropertiesChanged), Introspectable, Peer.
 * Data: the kernel's sysfs - /sys/class/power_supply/<N>/{type,online,status,present,capacity,energy_* or charge_*,
 * power_now or current_now,voltage_now,voltage_min_design,technology,cycle_count,manufacturer,model_name,serial_number},
 * /sys/power/policy/{profile,profile_choices,lid_state,low_battery_percent,critical_battery_percent,
 * critical_battery_action}, /sys/devices/system/cpu/cpufreq/policy0/scaling_driver, thermal cooling devices. Changes
 * come as kernel uevents (NETLINK_KOBJECT_UEVENT: power_supply, the POWER_PROFILE and PM_EVENT power events, the lid):
 * the daemon re-reads and emits PropertiesChanged for what changed; it has no timer of its own (no idle wake-ups).
 * The profile itself lives in the kernel (/sys/power/policy/profile maps it to cpufreq governors and EPP), so Aurora's
 * battery menu and this API always agree. Holds (HoldProfile) end with ReleaseProfile or when the holder leaves the bus;
 * a power-saver hold wins over a performance hold; setting ActiveProfile releases every hold (as ppd).
 * Writes (ActiveProfile, HoldProfile) are allowed to root and to the user of the local desktop session (the owner of
 * pid 1, Aurora; uid 1000 as the kernel's own sysfs rule) - power-profiles-daemon's polkit default (allow_active).
 * Options: --session (session bus, for tests), --sysfs PREFIX (a fake sysfs tree, for tests). */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define UP_PATH "/org/freedesktop/UPower"
#define UP_IFACE "org.freedesktop.UPower"
#define DEV_IFACE "org.freedesktop.UPower.Device"
#define DEV_PREFIX "/org/freedesktop/UPower/devices/"
#define DISPLAY_PATH DEV_PREFIX "DisplayDevice"
#define PP_PATH "/org/freedesktop/UPower/PowerProfiles"
#define PP_IFACE "org.freedesktop.UPower.PowerProfiles"
#define HADESS_PATH "/net/hadess/PowerProfiles"
#define HADESS_IFACE "net.hadess.PowerProfiles"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define MAX_DEVICES 16
#define MAX_HOLDS 32
#define DAEMON_VERSION "1.90.9"
#define PPD_VERSION "0.30"

static const char* sysfs = "";   /* --sysfs prefix */
static DBusConnection* bus;
static bool verbose;

/* ---- sysfs ---- */
static bool read_text(const char* path, char* out, size_t size) {
    char full[512]; snprintf(full, sizeof(full), "%s%s", sysfs, path);
    int fd = open(full, O_RDONLY | O_CLOEXEC); out[0] = 0; if (fd < 0) return false;
    ssize_t n = read(fd, out, size - 1); close(fd);
    if (n < 0) { out[0] = 0; return false; }
    out[n] = 0; while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
    return true;
}
static bool read_number(const char* path, long long* value) {
    char text[64]; if (!read_text(path, text, sizeof(text)) || !text[0]) return false;
    char* end; errno = 0; long long v = strtoll(text, &end, 10);
    if (errno || *end) return false;
    *value = v; return true;
}
static int write_text(const char* path, const char* text) {
    char full[512]; snprintf(full, sizeof(full), "%s%s", sysfs, path);
    int fd = open(full, O_WRONLY | O_CLOEXEC); if (fd < 0) return -errno;
    ssize_t n = write(fd, text, strlen(text)); int error = n < 0 ? -errno : 0; close(fd);
    return error;
}

/* ---- property values ---- */
enum { V_S = 's', V_O = 'o', V_B = 'b', V_U = 'u', V_I = 'i', V_X = 'x', V_T = 't', V_D = 'd' };
struct value { char type; bool b; uint32_t u; int32_t i; int64_t x; uint64_t t; double d; char s[128]; };
static struct value vs(char type, const char* s) { struct value v = {.type = type}; snprintf(v.s, sizeof(v.s), "%s", s); return v; }
static struct value vb(bool b) { struct value v = {.type = V_B}; v.b = b; return v; }
static struct value vu(uint32_t u) { struct value v = {.type = V_U}; v.u = u; return v; }
static struct value vi(int32_t i) { struct value v = {.type = V_I}; v.i = i; return v; }
static struct value vx(int64_t x) { struct value v = {.type = V_X}; v.x = x; return v; }
static struct value vt(uint64_t t) { struct value v = {.type = V_T}; v.t = t; return v; }
static struct value vd(double d) { struct value v = {.type = V_D}; v.d = d; return v; }
static bool value_equal(const struct value* a, const struct value* b) {
    if (a->type != b->type) return false;
    switch (a->type) {
    case V_S: case V_O: return !strcmp(a->s, b->s);
    case V_B: return a->b == b->b; case V_U: return a->u == b->u; case V_I: return a->i == b->i;
    case V_X: return a->x == b->x; case V_T: return a->t == b->t; default: return a->d == b->d;
    }
}
static bool append_basic(DBusMessageIter* it, const struct value* v) {
    dbus_bool_t b = v->b; const char* s = v->s;
    switch (v->type) {
    case V_S: return dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &s);
    case V_O: return dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &s);
    case V_B: return dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &b);
    case V_U: return dbus_message_iter_append_basic(it, DBUS_TYPE_UINT32, &v->u);
    case V_I: return dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &v->i);
    case V_X: return dbus_message_iter_append_basic(it, DBUS_TYPE_INT64, &v->x);
    case V_T: return dbus_message_iter_append_basic(it, DBUS_TYPE_UINT64, &v->t);
    default: return dbus_message_iter_append_basic(it, DBUS_TYPE_DOUBLE, &v->d);
    }
}
static bool append_variant(DBusMessageIter* it, const struct value* v) {
    char signature[2] = {v->type, 0}; DBusMessageIter variant;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, signature, &variant)) return false;
    bool ok = append_basic(&variant, v);
    return dbus_message_iter_close_container(it, &variant) && ok;
}
static bool dict_entry(DBusMessageIter* dict, const char* key, const struct value* v) {
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry)) return false;
    bool ok = dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) && append_variant(&entry, v);
    return dbus_message_iter_close_container(dict, &entry) && ok;
}

/* ---- power supplies -> UPower devices ---- */
enum { KIND_UNKNOWN = 0, KIND_LINE_POWER = 1, KIND_BATTERY = 2 };
enum { ST_UNKNOWN = 0, ST_CHARGING = 1, ST_DISCHARGING = 2, ST_EMPTY = 3, ST_FULL = 4, ST_PENDING_CHARGE = 5, ST_PENDING_DISCHARGE = 6 };
enum { WARN_NONE = 1, WARN_LOW = 3, WARN_CRITICAL = 4, WARN_ACTION = 5 };
#define DEVICE_PROPS 36
static const char* const device_props[DEVICE_PROPS] = {
    "NativePath", "Vendor", "Model", "Serial", "UpdateTime", "Type", "PowerSupply", "HasHistory", "HasStatistics", "Online",
    "Energy", "EnergyEmpty", "EnergyFull", "EnergyFullDesign", "EnergyRate", "Voltage", "ChargeCycles", "Luminosity",
    "TimeToEmpty", "TimeToFull", "Percentage", "Temperature", "IsPresent", "State", "IsRechargeable", "Capacity",
    "Technology", "WarningLevel", "BatteryLevel", "IconName", "ChargeStartThreshold", "ChargeEndThreshold",
    "ChargeThresholdEnabled", "ChargeThresholdSupported", "ChargeThresholdSettingsSupported", "VoltageMinDesign"};
struct device {
    bool used; char name[64], path[160];
    struct value props[DEVICE_PROPS];
};
static struct device devices[MAX_DEVICES], display;
static unsigned low_percent = 10, critical_percent = 5;
static char critical_action[32] = "PowerOff";
static bool on_battery, lid_present, lid_closed;

static const char* icon_level(double percentage) { return percentage < 10 ? "caution" : percentage < 30 ? "low" : percentage < 60 ? "good" : "full"; }
static void battery_icon(char* out, size_t size, bool present, uint32_t state, double percentage) {
    if (!present) { snprintf(out, size, "battery-missing-symbolic"); return; }
    if (state == ST_FULL) { snprintf(out, size, "battery-full-charged-symbolic"); return; }
    bool charging = state == ST_CHARGING || state == ST_PENDING_CHARGE;
    snprintf(out, size, "battery-%s%s-symbolic", percentage <= 0.5 && !charging ? "empty" : icon_level(percentage), charging ? "-charging" : "");
}
static uint32_t warning_level(bool battery_kind, uint32_t state, double percentage, bool system_on_battery) {
    if (!battery_kind || !system_on_battery || state != ST_DISCHARGING) return WARN_NONE;
    if (percentage <= 2) return WARN_ACTION;
    if (percentage <= critical_percent) return WARN_CRITICAL;
    if (percentage <= low_percent) return WARN_LOW;
    return WARN_NONE;
}
static uint32_t technology(const char* t) {
    if (!strcmp(t, "Li-ion")) return 1;
    if (!strcmp(t, "Li-poly")) return 2;
    if (!strcmp(t, "LiFe")) return 3;
    if (!strcmp(t, "NiCd")) return 5;
    if (!strcmp(t, "NiMH")) return 6;
    return 0;
}
/* Fill d->props from /sys/class/power_supply/<name>; returns the kind (0: not a UPower device). */
static unsigned device_read(struct device* d, const char* name, bool system_on_battery) {
    char dir[192], file[256], text[128]; long long n;
    snprintf(dir, sizeof(dir), "/sys/class/power_supply/%s", name);
    snprintf(file, sizeof(file), "%s/type", dir); if (!read_text(file, text, sizeof(text))) return 0;
    unsigned kind = !strcmp(text, "Mains") || !strcmp(text, "USB") ? KIND_LINE_POWER : !strcmp(text, "Battery") ? KIND_BATTERY : 0;
    if (!kind) return 0;
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->path, sizeof(d->path), DEV_PREFIX "%s_%s", kind == KIND_BATTERY ? "battery" : "line_power", name);
    for (char* p = d->path + strlen(DEV_PREFIX); *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) *p = '_';
    char vendor[64] = "", model[64] = "", serial[64] = "", status[32] = "", tech[32] = "";
    snprintf(file, sizeof(file), "%s/manufacturer", dir); read_text(file, vendor, sizeof(vendor));
    snprintf(file, sizeof(file), "%s/model_name", dir); read_text(file, model, sizeof(model));
    snprintf(file, sizeof(file), "%s/serial_number", dir); read_text(file, serial, sizeof(serial));
    snprintf(file, sizeof(file), "%s/status", dir); read_text(file, status, sizeof(status));
    snprintf(file, sizeof(file), "%s/technology", dir); read_text(file, tech, sizeof(tech));
    long long online = 0, present = 1, capacity = -1, cycles = -1, voltage = 0, design_voltage = 0;
    snprintf(file, sizeof(file), "%s/online", dir); read_number(file, &online);
    snprintf(file, sizeof(file), "%s/present", dir); read_number(file, &present);
    snprintf(file, sizeof(file), "%s/capacity", dir); read_number(file, &capacity);
    snprintf(file, sizeof(file), "%s/cycle_count", dir); if (read_number(file, &n)) cycles = n;
    snprintf(file, sizeof(file), "%s/voltage_now", dir); read_number(file, &voltage);
    snprintf(file, sizeof(file), "%s/voltage_min_design", dir); read_number(file, &design_voltage);
    if (!design_voltage) design_voltage = voltage;
    /* energy in Wh: energy_* (uWh) or charge_* (uAh) times the design voltage (uV) */
    double energy = 0, full = 0, full_design = 0, rate = 0;
    long long now_v, full_v, design_v, rate_v;
    snprintf(file, sizeof(file), "%s/energy_now", dir);
    if (read_number(file, &now_v)) {
        energy = now_v / 1e6;
        snprintf(file, sizeof(file), "%s/energy_full", dir); if (read_number(file, &full_v)) full = full_v / 1e6;
        snprintf(file, sizeof(file), "%s/energy_full_design", dir); if (read_number(file, &design_v)) full_design = design_v / 1e6;
        snprintf(file, sizeof(file), "%s/power_now", dir); if (read_number(file, &rate_v)) rate = (rate_v < 0 ? -rate_v : rate_v) / 1e6;
    } else {
        snprintf(file, sizeof(file), "%s/charge_now", dir);
        double volts = design_voltage / 1e6;
        if (read_number(file, &now_v)) energy = now_v / 1e6 * volts;
        snprintf(file, sizeof(file), "%s/charge_full", dir); if (read_number(file, &full_v)) full = full_v / 1e6 * volts;
        snprintf(file, sizeof(file), "%s/charge_full_design", dir); if (read_number(file, &design_v)) full_design = design_v / 1e6 * volts;
        snprintf(file, sizeof(file), "%s/current_now", dir); if (read_number(file, &rate_v)) rate = (rate_v < 0 ? -rate_v : rate_v) / 1e6 * volts;
    }
    if (full <= 0) full = full_design;
    double percentage = capacity >= 0 ? (double)(capacity > 100 ? 100 : capacity) : full > 0 ? energy * 100.0 / full : 0;
    if (percentage > 100) percentage = 100;
    uint32_t state = ST_UNKNOWN;
    if (kind == KIND_BATTERY && present) {
        if (!strcmp(status, "Charging")) state = ST_CHARGING;
        else if (!strcmp(status, "Discharging")) state = percentage <= 0.0 ? ST_EMPTY : ST_DISCHARGING;
        else if (!strcmp(status, "Full")) state = ST_FULL;
        else if (!strcmp(status, "Not charging")) state = ST_PENDING_CHARGE;
    }
    int64_t to_empty = 0, to_full = 0;
    if (state == ST_DISCHARGING && rate > 0.01) { double s = energy / rate * 3600; to_empty = s > 864000 ? 0 : (int64_t)s; }
    if (state == ST_CHARGING && rate > 0.01 && full > energy) { double s = (full - energy) / rate * 3600; to_full = s > 864000 ? 0 : (int64_t)s; }
    char icon[64];
    if (kind == KIND_BATTERY) battery_icon(icon, sizeof(icon), present != 0, state, percentage); else snprintf(icon, sizeof(icon), "ac-adapter-symbolic");
    struct timespec realtime; clock_gettime(CLOCK_REALTIME, &realtime);
    struct value* p = d->props; bool bat = kind == KIND_BATTERY;
    p[0] = vs(V_S, name); p[1] = vs(V_S, vendor); p[2] = vs(V_S, model); p[3] = vs(V_S, serial); p[4] = vt((uint64_t)realtime.tv_sec);
    p[5] = vu(kind); p[6] = vb(true); p[7] = vb(false); p[8] = vb(false); p[9] = vb(!bat && online);
    p[10] = vd(bat ? energy : 0); p[11] = vd(0); p[12] = vd(bat ? full : 0); p[13] = vd(bat ? full_design : 0); p[14] = vd(bat ? rate : 0);
    p[15] = vd(voltage / 1e6); p[16] = vi(bat ? (int32_t)cycles : 0); p[17] = vd(0);
    p[18] = vx(to_empty); p[19] = vx(to_full); p[20] = vd(bat ? percentage : 0); p[21] = vd(0);
    p[22] = vb(bat ? present != 0 : false); p[23] = vu(state); p[24] = vb(bat);
    p[25] = vd(bat && full_design > 0 ? (full / full_design * 100 > 100 ? 100 : full / full_design * 100) : 0);
    p[26] = vu(bat ? technology(tech) : 0); p[27] = vu(warning_level(bat, state, percentage, system_on_battery)); p[28] = vu(1);
    p[29] = vs(V_S, icon); p[30] = vu(0); p[31] = vu(100); p[32] = vb(false); p[33] = vb(false); p[34] = vu(0); p[35] = vd(design_voltage / 1e6);
    return kind;
}
/* The composite battery: energies summed, state combined (charging wins, then discharging), time from the sums. */
static void display_compute(void) {
    double energy = 0, full = 0, design = 0, rate = 0; unsigned batteries = 0; bool charging = false, discharging = false, all_full = true;
    for (unsigned i = 0; i < MAX_DEVICES; i++) {
        const struct device* d = &devices[i];
        if (!d->used || d->props[5].u != KIND_BATTERY || !d->props[22].b) continue;
        batteries++; energy += d->props[10].d; full += d->props[12].d; design += d->props[13].d; rate += d->props[14].d;
        uint32_t s = d->props[23].u;
        if (s == ST_CHARGING) charging = true; else if (s == ST_DISCHARGING || s == ST_EMPTY) discharging = true;
        if (s != ST_FULL) all_full = false;
    }
    double percentage = full > 0 ? energy * 100.0 / full : 0; if (percentage > 100) percentage = 100;
    uint32_t state = !batteries ? ST_UNKNOWN : charging ? ST_CHARGING : discharging ? (percentage <= 0 ? ST_EMPTY : ST_DISCHARGING) : all_full ? ST_FULL : ST_PENDING_CHARGE;
    int64_t to_empty = state == ST_DISCHARGING && rate > 0.01 ? (int64_t)(energy / rate * 3600) : 0;
    int64_t to_full = state == ST_CHARGING && rate > 0.01 && full > energy ? (int64_t)((full - energy) / rate * 3600) : 0;
    char icon[64] = "";
    if (batteries) battery_icon(icon, sizeof(icon), true, state, percentage);
    struct timespec realtime; clock_gettime(CLOCK_REALTIME, &realtime);
    struct value* p = display.props;
    p[0] = vs(V_S, ""); p[1] = vs(V_S, ""); p[2] = vs(V_S, ""); p[3] = vs(V_S, ""); p[4] = vt((uint64_t)realtime.tv_sec);
    p[5] = vu(batteries ? KIND_BATTERY : KIND_UNKNOWN); p[6] = vb(true); p[7] = vb(false); p[8] = vb(false); p[9] = vb(false);
    p[10] = vd(energy); p[11] = vd(0); p[12] = vd(full); p[13] = vd(design); p[14] = vd(rate); p[15] = vd(0); p[16] = vi(0); p[17] = vd(0);
    p[18] = vx(to_empty > 864000 ? 0 : to_empty); p[19] = vx(to_full > 864000 ? 0 : to_full); p[20] = vd(percentage); p[21] = vd(0);
    p[22] = vb(batteries > 0); p[23] = vu(state); p[24] = vb(false); p[25] = vd(0); p[26] = vu(0);
    p[27] = vu(warning_level(batteries > 0, state, percentage, on_battery)); p[28] = vu(1); p[29] = vs(V_S, icon);
    p[30] = vu(0); p[31] = vu(100); p[32] = vb(false); p[33] = vb(false); p[34] = vu(0); p[35] = vd(0);
    display.used = true; snprintf(display.path, sizeof(display.path), "%s", DISPLAY_PATH);
}

/* ---- power profiles ---- */
static const char* const profile_names[3] = {"power-saver", "balanced", "performance"};
struct hold { bool used; uint32_t cookie; unsigned profile; char reason[96], application[96], owner[96]; };
static struct hold holds[MAX_HOLDS];
static uint32_t next_cookie = 1;
static unsigned base_profile = 1, active_profile = 1;   /* the user's choice, and what is in effect (holds) */
static bool choices[3] = {true, true, false};
static char cpu_driver[32] = "placeholder", degraded[64] = "";
static int profile_index(const char* name) { for (int i = 0; i < 3; i++) if (!strcmp(name, profile_names[i])) return i; return -1; }
static void profiles_read(void) {
    char text[128];
    if (read_text("/sys/power/policy/profile_choices", text, sizeof(text))) {
        choices[0] = choices[1] = choices[2] = false;
        for (char* word = strtok(text, " "); word; word = strtok(NULL, " ")) { int p = profile_index(word); if (p >= 0) choices[p] = true; }
    }
    if (read_text("/sys/power/policy/profile", text, sizeof(text)) && profile_index(text) >= 0) active_profile = (unsigned)profile_index(text);
    if (read_text("/sys/devices/system/cpu/cpufreq/policy0/scaling_driver", text, sizeof(text)) && text[0])
        snprintf(cpu_driver, sizeof(cpu_driver), "%s", !strcmp(text, "amd-pstate-epp") ? "amd_pstate" : text);
    else snprintf(cpu_driver, sizeof(cpu_driver), "placeholder");
    /* the thermal policy caps the CPUs (a processor cooling device in use): the performance profile is degraded */
    degraded[0] = 0;
    for (unsigned i = 0; i < 16; i++) {
        char path[128]; long long state; snprintf(path, sizeof(path), "/sys/class/thermal/cooling_device%u/type", i);
        if (!read_text(path, text, sizeof(text))) continue;
        snprintf(path, sizeof(path), "/sys/class/thermal/cooling_device%u/cur_state", i);
        if (!strcmp(text, "Processor") && read_number(path, &state) && state > 0) snprintf(degraded, sizeof(degraded), "high-operating-temperature");
    }
}
static unsigned held_profile(unsigned fallback) {
    bool saver = false, performance = false;
    for (unsigned i = 0; i < MAX_HOLDS; i++) if (holds[i].used) { if (holds[i].profile == 0) saver = true; else if (holds[i].profile == 2) performance = true; }
    return saver ? 0 : performance ? 2 : fallback;
}
static int profile_apply(unsigned profile) {
    int error = write_text("/sys/power/policy/profile", profile_names[profile]);
    if (error) { fprintf(stderr, "nexus-upowerd: profile %s: %s\n", profile_names[profile], strerror(-error)); return error; }
    active_profile = profile; return 0;
}

/* ---- object properties ---- */
struct changed { const char* name; struct value v; };
static bool manager_prop(unsigned i, const char** name, struct value* v) {
    switch (i) {
    case 0: *name = "DaemonVersion"; *v = vs(V_S, DAEMON_VERSION); return true;
    case 1: *name = "OnBattery"; *v = vb(on_battery); return true;
    case 2: *name = "LidIsClosed"; *v = vb(lid_closed); return true;
    case 3: *name = "LidIsPresent"; *v = vb(lid_present); return true;
    }
    return false;
}
static bool pp_basic_prop(unsigned i, const char** name, struct value* v) {
    switch (i) {
    case 0: *name = "ActiveProfile"; *v = vs(V_S, profile_names[active_profile]); return true;
    case 1: *name = "PerformanceInhibited"; *v = vs(V_S, ""); return true;
    case 2: *name = "PerformanceDegraded"; *v = vs(V_S, degraded); return true;
    case 3: *name = "Version"; *v = vs(V_S, PPD_VERSION); return true;
    }
    return false;
}
/* Profiles aa{sv}, Actions as, ActiveProfileHolds aa{sv} */
static bool append_profiles(DBusMessageIter* variant_parent) {
    DBusMessageIter variant, array;
    if (!dbus_message_iter_open_container(variant_parent, DBUS_TYPE_VARIANT, "aa{sv}", &variant)) return false;
    if (!dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "a{sv}", &array)) return false;
    for (unsigned p = 0; p < 3; p++) {
        if (!choices[p]) continue;
        DBusMessageIter dict; if (!dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY, "{sv}", &dict)) return false;
        bool cpu = strcmp(cpu_driver, "placeholder") != 0;
        struct value v = vs(V_S, profile_names[p]); dict_entry(&dict, "Profile", &v);
        v = vs(V_S, cpu ? cpu_driver : "placeholder"); dict_entry(&dict, "Driver", &v);
        if (cpu) { v = vs(V_S, cpu_driver); dict_entry(&dict, "CpuDriver", &v); }
        v = vs(V_S, "placeholder"); dict_entry(&dict, "PlatformDriver", &v);
        if (!dbus_message_iter_close_container(&array, &dict)) return false;
    }
    return dbus_message_iter_close_container(&variant, &array) && dbus_message_iter_close_container(variant_parent, &variant);
}
static bool append_actions(DBusMessageIter* variant_parent) {
    DBusMessageIter variant, array;
    if (!dbus_message_iter_open_container(variant_parent, DBUS_TYPE_VARIANT, "as", &variant)) return false;
    if (!dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array)) return false;
    return dbus_message_iter_close_container(&variant, &array) && dbus_message_iter_close_container(variant_parent, &variant);
}
static bool append_holds(DBusMessageIter* variant_parent) {
    DBusMessageIter variant, array;
    if (!dbus_message_iter_open_container(variant_parent, DBUS_TYPE_VARIANT, "aa{sv}", &variant)) return false;
    if (!dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "a{sv}", &array)) return false;
    for (unsigned i = 0; i < MAX_HOLDS; i++) {
        if (!holds[i].used) continue;
        DBusMessageIter dict; if (!dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY, "{sv}", &dict)) return false;
        struct value v = vs(V_S, holds[i].application); dict_entry(&dict, "ApplicationId", &v);
        v = vs(V_S, profile_names[holds[i].profile]); dict_entry(&dict, "Profile", &v);
        v = vs(V_S, holds[i].reason); dict_entry(&dict, "Reason", &v);
        if (!dbus_message_iter_close_container(&array, &dict)) return false;
    }
    return dbus_message_iter_close_container(&variant, &array) && dbus_message_iter_close_container(variant_parent, &variant);
}
static bool append_pp_complex(DBusMessageIter* parent, const char* name) {
    if (!strcmp(name, "Profiles")) return append_profiles(parent);
    if (!strcmp(name, "Actions")) return append_actions(parent);
    if (!strcmp(name, "ActiveProfileHolds")) return append_holds(parent);
    return false;
}
static const char* const pp_complex[] = {"Profiles", "Actions", "ActiveProfileHolds"};

/* ---- signals ---- */
static void send_and_unref(DBusMessage* m) { if (m) { dbus_connection_send(bus, m, NULL); dbus_message_unref(m); } }
/* PropertiesChanged(interface, {name: value}, []) for the changed basic properties and the named complex ones. */
static void properties_changed(const char* path, const char* iface, const struct changed* list, unsigned count, const char* const* complex, unsigned complex_count) {
    if (!count && !complex_count) return;
    DBusMessage* m = dbus_message_new_signal(path, PROPS_IFACE, "PropertiesChanged"); if (!m) return;
    DBusMessageIter it, dict, empty; dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    for (unsigned i = 0; i < count; i++) dict_entry(&dict, list[i].name, &list[i].v);
    for (unsigned i = 0; i < complex_count; i++) {
        DBusMessageIter entry; dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &complex[i]); append_pp_complex(&entry, complex[i]);
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_close_container(&it, &dict);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &empty); dbus_message_iter_close_container(&it, &empty);
    send_and_unref(m);
}
static void device_signal(const char* member, const char* path) {
    DBusMessage* m = dbus_message_new_signal(UP_PATH, UP_IFACE, member); if (!m) return;
    dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID); send_and_unref(m);
}
static void device_changed(const struct device* now, const struct device* before) {
    struct changed list[DEVICE_PROPS]; unsigned count = 0;
    for (unsigned i = 0; i < DEVICE_PROPS; i++)
        if (i != 4 && (!before || !value_equal(&now->props[i], &before->props[i]))) list[count++] = (struct changed){device_props[i], now->props[i]};
    if (count) { list[count++] = (struct changed){"UpdateTime", now->props[4]}; properties_changed(now->path, DEV_IFACE, list, count, NULL, 0); }
}

/* ---- refresh everything from sysfs, signalling the differences ---- */
static void refresh(bool quiet) {
    long long value; char text[64];
    if (read_number("/sys/power/policy/low_battery_percent", &value) && value > 0 && value < 100) low_percent = (unsigned)value;
    if (read_number("/sys/power/policy/critical_battery_percent", &value) && value > 0 && value < 100) critical_percent = (unsigned)value;
    if (read_text("/sys/power/policy/critical_battery_action", text, sizeof(text))) {
        /* what the kernel will really do: hibernation only with an area for the image (else it powers off) */
        char states[64] = ""; read_text("/sys/power/state", states, sizeof(states));
        bool disk = strstr(states, "disk") != NULL;
        snprintf(critical_action, sizeof(critical_action), "%s", !strcmp(text, "hibernate") && disk ? "Hibernate" : !strcmp(text, "suspend") ? "Suspend" : "PowerOff");
    }
    bool old_battery = on_battery, old_present = lid_present, old_closed = lid_closed;
    if (read_text("/sys/power/policy/lid_state", text, sizeof(text))) { lid_present = strcmp(text, "absent") != 0; lid_closed = !strcmp(text, "closed"); }
    /* pass 1: which supplies exist and whether the system runs on battery */
    char names[MAX_DEVICES][64]; unsigned count = 0; bool any_ac_online = false, any_discharging = false;
    char dirpath[256]; snprintf(dirpath, sizeof(dirpath), "%s/sys/class/power_supply", sysfs);
    DIR* dir = opendir(dirpath);
    if (dir) {
        struct dirent* e;
        while ((e = readdir(dir)) && count < MAX_DEVICES) {
            if (e->d_name[0] == '.') continue;
            struct device probe = {0};
            unsigned kind = device_read(&probe, e->d_name, false);
            if (!kind) continue;
            snprintf(names[count++], sizeof(names[0]), "%s", e->d_name);
            if (kind == KIND_LINE_POWER && probe.props[9].b) any_ac_online = true;
            if (kind == KIND_BATTERY && (probe.props[23].u == ST_DISCHARGING || probe.props[23].u == ST_EMPTY)) any_discharging = true;
        }
        closedir(dir);
    }
    on_battery = !any_ac_online && any_discharging;
    /* pass 2: devices with their warning levels (they depend on OnBattery) */
    bool seen[MAX_DEVICES] = {false};
    for (unsigned n = 0; n < count; n++) {
        struct device fresh = {0}; if (!device_read(&fresh, names[n], on_battery)) continue;
        fresh.used = true;
        int slot = -1;
        for (unsigned i = 0; i < MAX_DEVICES; i++) if (devices[i].used && !strcmp(devices[i].name, fresh.name)) slot = (int)i;
        if (slot >= 0) {
            struct device before = devices[slot]; devices[slot] = fresh; seen[slot] = true;
            if (!quiet) device_changed(&devices[slot], &before);
        } else {
            for (unsigned i = 0; i < MAX_DEVICES; i++) if (!devices[i].used) { slot = (int)i; break; }
            if (slot < 0) continue;
            devices[slot] = fresh; seen[slot] = true;
            if (!quiet) device_signal("DeviceAdded", devices[slot].path);
            if (verbose) fprintf(stderr, "nexus-upowerd: device %s\n", devices[slot].path);
        }
    }
    for (unsigned i = 0; i < MAX_DEVICES; i++) if (devices[i].used && !seen[i]) {
        if (!quiet) device_signal("DeviceRemoved", devices[i].path);
        devices[i].used = false;
    }
    struct device before = display; display_compute();
    if (!quiet) device_changed(&display, before.used ? &before : NULL);
    if (!quiet) {
        struct changed list[3]; unsigned n = 0;
        if (old_battery != on_battery) list[n++] = (struct changed){"OnBattery", vb(on_battery)};
        if (old_present != lid_present) list[n++] = (struct changed){"LidIsPresent", vb(lid_present)};
        if (old_closed != lid_closed) list[n++] = (struct changed){"LidIsClosed", vb(lid_closed)};
        properties_changed(UP_PATH, UP_IFACE, list, n, NULL, 0);
    }
    /* profiles: the kernel's value (Aurora may have changed it); a hold in force keeps its profile */
    unsigned old_active = active_profile; bool old_choices[3] = {choices[0], choices[1], choices[2]}; char old_degraded[64]; snprintf(old_degraded, sizeof(old_degraded), "%s", degraded);
    profiles_read();
    if (held_profile(3) == 3) base_profile = active_profile;
    if (!quiet) {
        struct changed list[2]; unsigned n = 0; const char* complex[1]; unsigned c = 0;
        if (old_active != active_profile) list[n++] = (struct changed){"ActiveProfile", vs(V_S, profile_names[active_profile])};
        if (strcmp(old_degraded, degraded)) list[n++] = (struct changed){"PerformanceDegraded", vs(V_S, degraded)};
        if (memcmp(old_choices, choices, sizeof(choices))) complex[c++] = "Profiles";
        properties_changed(PP_PATH, PP_IFACE, list, n, complex, c); properties_changed(HADESS_PATH, HADESS_IFACE, list, n, complex, c);
    }
}

/* ---- authorization: root, or the local desktop session's user ---- */
static bool caller_allowed(DBusMessage* m) {
    const char* sender = dbus_message_get_sender(m); if (!sender) return false;
    DBusError e; dbus_error_init(&e);
    unsigned long uid = dbus_bus_get_unix_user(bus, sender, &e);
    if (dbus_error_is_set(&e)) { dbus_error_free(&e); return false; }
    if (uid == 0) return true;
    struct stat init; if (stat("/proc/1", &init) == 0 && init.st_uid != 0 && uid == init.st_uid) return true;   /* Aurora's user */
    return uid == 1000;   /* the kernel's session-owner rule for /sys/power */
}

/* ---- method calls ---- */
static DBusHandlerResult reply_error(DBusMessage* m, const char* name, const char* text) {
    DBusMessage* r = dbus_message_new_error(m, name, text); send_and_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult reply_empty(DBusMessage* m) { send_and_unref(dbus_message_new_method_return(m)); return DBUS_HANDLER_RESULT_HANDLED; }
static struct device* device_at(const char* path) {
    if (!strcmp(path, DISPLAY_PATH)) return &display;
    for (unsigned i = 0; i < MAX_DEVICES; i++) if (devices[i].used && !strcmp(devices[i].path, path)) return &devices[i];
    return NULL;
}
static void introspect_xml(char* out, size_t size, const char* path) {
    const char* common =
        "<interface name=\"org.freedesktop.DBus.Properties\"><method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
        "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
        "<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
        "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal></interface>"
        "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface>"
        "<interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/><method name=\"GetMachineId\"><arg type=\"s\" direction=\"out\"/></method></interface>";
    int n = snprintf(out, size, "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>");
    if (!strcmp(path, UP_PATH)) {
        n += snprintf(out + n, size - n, "<interface name=\"" UP_IFACE "\"><method name=\"EnumerateDevices\"><arg type=\"ao\" direction=\"out\"/></method>"
            "<method name=\"GetDisplayDevice\"><arg type=\"o\" direction=\"out\"/></method><method name=\"GetCriticalAction\"><arg type=\"s\" direction=\"out\"/></method>"
            "<signal name=\"DeviceAdded\"><arg type=\"o\"/></signal><signal name=\"DeviceRemoved\"><arg type=\"o\"/></signal>"
            "<property name=\"DaemonVersion\" type=\"s\" access=\"read\"/><property name=\"OnBattery\" type=\"b\" access=\"read\"/>"
            "<property name=\"LidIsClosed\" type=\"b\" access=\"read\"/><property name=\"LidIsPresent\" type=\"b\" access=\"read\"/></interface>%s"
            "<node name=\"devices\"/><node name=\"PowerProfiles\"/>", common);
    } else if (!strcmp(path, DEV_PREFIX "") || !strcmp(path, "/org/freedesktop/UPower/devices")) {
        for (unsigned i = 0; i < MAX_DEVICES; i++) if (devices[i].used) n += snprintf(out + n, size - n, "<node name=\"%s\"/>", devices[i].path + strlen(DEV_PREFIX));
        n += snprintf(out + n, size - n, "<node name=\"DisplayDevice\"/>");
    } else if (device_at(path)) {
        static const char* const types[DEVICE_PROPS] = {"s","s","s","s","t","u","b","b","b","b","d","d","d","d","d","d","i","d","x","x","d","d","b","u","b","d","u","u","u","s","u","u","b","b","u","d"};
        n += snprintf(out + n, size - n, "<interface name=\"" DEV_IFACE "\"><method name=\"Refresh\"/>"
            "<method name=\"GetHistory\"><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"a(udu)\" direction=\"out\"/></method>"
            "<method name=\"GetStatistics\"><arg type=\"s\" direction=\"in\"/><arg type=\"a(dd)\" direction=\"out\"/></method>");
        for (unsigned i = 0; i < DEVICE_PROPS && n < (int)size - 128; i++) n += snprintf(out + n, size - n, "<property name=\"%s\" type=\"%s\" access=\"read\"/>", device_props[i], types[i]);
        n += snprintf(out + n, size - n, "</interface>%s", common);
    } else if (!strcmp(path, PP_PATH) || !strcmp(path, HADESS_PATH)) {
        const char* iface = !strcmp(path, PP_PATH) ? PP_IFACE : HADESS_IFACE;
        n += snprintf(out + n, size - n, "<interface name=\"%s\"><method name=\"HoldProfile\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/></method><method name=\"ReleaseProfile\"><arg type=\"u\" direction=\"in\"/></method>"
            "<signal name=\"ProfileReleased\"><arg type=\"u\"/></signal><property name=\"ActiveProfile\" type=\"s\" access=\"readwrite\"/>"
            "<property name=\"PerformanceInhibited\" type=\"s\" access=\"read\"/><property name=\"PerformanceDegraded\" type=\"s\" access=\"read\"/>"
            "<property name=\"Profiles\" type=\"aa{sv}\" access=\"read\"/><property name=\"Actions\" type=\"as\" access=\"read\"/>"
            "<property name=\"ActiveProfileHolds\" type=\"aa{sv}\" access=\"read\"/><property name=\"Version\" type=\"s\" access=\"read\"/></interface>%s", iface, common);
    } else if (!strcmp(path, "/") || !strcmp(path, "/org") || !strcmp(path, "/org/freedesktop")) {
        n += snprintf(out + n, size - n, "<node name=\"%s\"/>", !strcmp(path, "/") ? "org" : !strcmp(path, "/org") ? "freedesktop" : "UPower");
    } else if (!strcmp(path, "/net") || !strcmp(path, "/net/hadess")) {
        n += snprintf(out + n, size - n, "<node name=\"%s\"/>", !strcmp(path, "/net") ? "hadess" : "PowerProfiles");
    }
    if (n < (int)size) snprintf(out + n, size - n, "</node>\n");
}
static void hold_release(unsigned index, bool signal) {
    uint32_t cookie = holds[index].cookie; holds[index].used = false;
    if (signal) {
        DBusMessage* m = dbus_message_new_signal(PP_PATH, PP_IFACE, "ProfileReleased");
        if (m) { dbus_message_append_args(m, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID); send_and_unref(m); }
        m = dbus_message_new_signal(HADESS_PATH, HADESS_IFACE, "ProfileReleased");
        if (m) { dbus_message_append_args(m, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID); send_and_unref(m); }
    }
}
/* After holds changed: the profile in force, and the signals for it. */
static void holds_changed(void) {
    unsigned wanted = held_profile(base_profile), before = active_profile;
    if (wanted != active_profile) profile_apply(wanted);
    struct changed list[1]; unsigned n = 0; const char* complex[1] = {"ActiveProfileHolds"};
    if (before != active_profile) list[n++] = (struct changed){"ActiveProfile", vs(V_S, profile_names[active_profile])};
    properties_changed(PP_PATH, PP_IFACE, list, n, complex, 1); properties_changed(HADESS_PATH, HADESS_IFACE, list, n, complex, 1);
}
static DBusHandlerResult properties_call(DBusMessage* m, const char* path, const char* member) {
    DBusMessageIter it; const char* iface = NULL, *name = NULL;
    if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return reply_error(m, DBUS_ERROR_INVALID_ARGS, "interface name expected");
    dbus_message_iter_get_basic(&it, &iface); dbus_message_iter_next(&it);
    if (strcmp(member, "GetAll")) {
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return reply_error(m, DBUS_ERROR_INVALID_ARGS, "property name expected");
        dbus_message_iter_get_basic(&it, &name); dbus_message_iter_next(&it);
    }
    bool pp = !strcmp(path, PP_PATH) || !strcmp(path, HADESS_PATH);
    const char* own = !strcmp(path, UP_PATH) ? UP_IFACE : pp ? (!strcmp(path, PP_PATH) ? PP_IFACE : HADESS_IFACE) : DEV_IFACE;
    struct device* d = pp || !strcmp(path, UP_PATH) ? NULL : device_at(path);
    if (!pp && strcmp(path, UP_PATH) && !d) return reply_error(m, DBUS_ERROR_UNKNOWN_OBJECT, "no such object");
    if (iface[0] && strcmp(iface, own)) return reply_error(m, DBUS_ERROR_UNKNOWN_INTERFACE, "no such interface");
    if (!strcmp(member, "Set")) {
        if (!pp || strcmp(name, "ActiveProfile")) {
            bool known = false; const char* n2; struct value v;
            for (unsigned i = 0; (pp ? pp_basic_prop(i, &n2, &v) : d ? i < DEVICE_PROPS && (n2 = device_props[i]) : manager_prop(i, &n2, &v)); i++) if (!strcmp(n2, name)) known = true;
            if (pp) for (unsigned i = 0; i < 3; i++) if (!strcmp(pp_complex[i], name)) known = true;
            return reply_error(m, known ? DBUS_ERROR_PROPERTY_READ_ONLY : DBUS_ERROR_UNKNOWN_PROPERTY, known ? "property is read-only" : "no such property");
        }
        DBusMessageIter variant; const char* profile = NULL;
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) return reply_error(m, DBUS_ERROR_INVALID_ARGS, "variant expected");
        dbus_message_iter_recurse(&it, &variant);
        if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_STRING) return reply_error(m, DBUS_ERROR_INVALID_ARGS, "ActiveProfile is a string");
        dbus_message_iter_get_basic(&variant, &profile);
        int p = profile_index(profile);
        if (p < 0 || !choices[p]) return reply_error(m, DBUS_ERROR_INVALID_ARGS, "Invalid profile name or profile not available");
        if (!caller_allowed(m)) return reply_error(m, DBUS_ERROR_ACCESS_DENIED, "Not authorized to switch the power profile");
        for (unsigned i = 0; i < MAX_HOLDS; i++) if (holds[i].used) hold_release(i, true);   /* a user choice ends every hold */
        base_profile = (unsigned)p; unsigned before = active_profile;
        int error = profile_apply((unsigned)p);
        if (error) return reply_error(m, DBUS_ERROR_FAILED, strerror(-error));
        DBusHandlerResult result = reply_empty(m);
        struct changed list[1]; unsigned n = 0; const char* complex[1] = {"ActiveProfileHolds"};
        if (before != active_profile) list[n++] = (struct changed){"ActiveProfile", vs(V_S, profile_names[active_profile])};
        properties_changed(PP_PATH, PP_IFACE, list, n, complex, 1); properties_changed(HADESS_PATH, HADESS_IFACE, list, n, complex, 1);
        return result;
    }
    DBusMessage* r = dbus_message_new_method_return(m); if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    DBusMessageIter out; dbus_message_iter_init_append(r, &out);
    if (!strcmp(member, "Get")) {
        struct value v; const char* n2; bool found = false;
        if (d) { for (unsigned i = 0; i < DEVICE_PROPS; i++) if (!strcmp(device_props[i], name)) { append_variant(&out, &d->props[i]); found = true; break; } }
        else if (pp) {
            for (unsigned i = 0; pp_basic_prop(i, &n2, &v); i++) if (!strcmp(n2, name)) { append_variant(&out, &v); found = true; break; }
            if (!found) for (unsigned i = 0; i < 3; i++) if (!strcmp(pp_complex[i], name)) { append_pp_complex(&out, name); found = true; break; }
        } else for (unsigned i = 0; manager_prop(i, &n2, &v); i++) if (!strcmp(n2, name)) { append_variant(&out, &v); found = true; break; }
        if (!found) { dbus_message_unref(r); return reply_error(m, DBUS_ERROR_UNKNOWN_PROPERTY, "no such property"); }
    } else {
        DBusMessageIter dict; dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &dict);
        struct value v; const char* n2;
        if (d) for (unsigned i = 0; i < DEVICE_PROPS; i++) dict_entry(&dict, device_props[i], &d->props[i]);
        else if (pp) {
            for (unsigned i = 0; pp_basic_prop(i, &n2, &v); i++) dict_entry(&dict, n2, &v);
            for (unsigned i = 0; i < 3; i++) {
                DBusMessageIter entry; dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &pp_complex[i]); append_pp_complex(&entry, pp_complex[i]);
                dbus_message_iter_close_container(&dict, &entry);
            }
        } else for (unsigned i = 0; manager_prop(i, &n2, &v); i++) dict_entry(&dict, n2, &v);
        dbus_message_iter_close_container(&out, &dict);
    }
    send_and_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult handle(DBusConnection* connection, DBusMessage* m, void* data) {
    (void)connection; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char* path = dbus_message_get_path(m), *iface = dbus_message_get_interface(m), *member = dbus_message_get_member(m);
    if (!path || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (verbose) fprintf(stderr, "nexus-upowerd: %s %s.%s\n", path, iface ? iface : "", member);
    if ((!iface || !strcmp(iface, "org.freedesktop.DBus.Introspectable")) && !strcmp(member, "Introspect")) {
        static char xml[16384]; introspect_xml(xml, sizeof(xml), path); const char* p = xml;
        DBusMessage* r = dbus_message_new_method_return(m); if (r) dbus_message_append_args(r, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
        send_and_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    if ((!iface || !strcmp(iface, "org.freedesktop.DBus.Peer")) && !strcmp(member, "Ping")) return reply_empty(m);
    if ((!iface || !strcmp(iface, "org.freedesktop.DBus.Peer")) && !strcmp(member, "GetMachineId")) {
        char id[64] = ""; if (!read_text("/etc/machine-id", id, sizeof(id))) snprintf(id, sizeof(id), "00000000000000000000000000000000");
        const char* p = id; DBusMessage* r = dbus_message_new_method_return(m); if (r) dbus_message_append_args(r, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
        send_and_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
    }
    bool known_object = !strcmp(path, UP_PATH) || !strcmp(path, PP_PATH) || !strcmp(path, HADESS_PATH) || device_at(path);
    if (iface && !strcmp(iface, PROPS_IFACE)) {
        if (strcmp(member, "Get") && strcmp(member, "GetAll") && strcmp(member, "Set")) return reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
        if (!known_object) return reply_error(m, DBUS_ERROR_UNKNOWN_OBJECT, "no such object");
        return properties_call(m, path, member);
    }
    if (!strcmp(path, UP_PATH) && (!iface || !strcmp(iface, UP_IFACE))) {
        if (!strcmp(member, "EnumerateDevices")) {
            DBusMessage* r = dbus_message_new_method_return(m); if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
            DBusMessageIter it, array; dbus_message_iter_init_append(r, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o", &array);
            for (unsigned i = 0; i < MAX_DEVICES; i++) if (devices[i].used) { const char* p = devices[i].path; dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, &p); }
            dbus_message_iter_close_container(&it, &array); send_and_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (!strcmp(member, "GetDisplayDevice")) {
            const char* p = DISPLAY_PATH; DBusMessage* r = dbus_message_new_method_return(m);
            if (r) dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
            send_and_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (!strcmp(member, "GetCriticalAction")) {
            const char* p = critical_action; DBusMessage* r = dbus_message_new_method_return(m);
            if (r) dbus_message_append_args(r, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
            send_and_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
        }
        return reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
    }
    if (device_at(path) && (!iface || !strcmp(iface, DEV_IFACE))) {
        if (!strcmp(member, "Refresh")) { refresh(false); return reply_empty(m); }
        if (!strcmp(member, "GetHistory") || !strcmp(member, "GetStatistics")) {
            DBusError e; dbus_error_init(&e); const char* kind = NULL; uint32_t timespan = 0, resolution = 0;
            bool ok = !strcmp(member, "GetHistory") ? dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &kind, DBUS_TYPE_UINT32, &timespan, DBUS_TYPE_UINT32, &resolution, DBUS_TYPE_INVALID)
                                                    : dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &kind, DBUS_TYPE_INVALID);
            if (!ok) { dbus_error_free(&e); return reply_error(m, DBUS_ERROR_INVALID_ARGS, "bad arguments"); }
            /* no history is kept (HasHistory/HasStatistics are false): UPower answers an error for that */
            return reply_error(m, "org.freedesktop.UPower.GeneralError", "history and statistics are not kept for this device");
        }
        /* charge limits need a vendor interface (EC/WMI); none is driven, so ChargeThresholdSupported is false */
        if (!strcmp(member, "EnableChargeThreshold")) return reply_error(m, "org.freedesktop.UPower.GeneralError", "setting battery charge thresholds is not supported");
        return reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
    }
    if ((!strcmp(path, PP_PATH) || !strcmp(path, HADESS_PATH)) && (!iface || !strcmp(iface, PP_IFACE) || !strcmp(iface, HADESS_IFACE))) {
        if (!strcmp(member, "HoldProfile")) {
            DBusError e; dbus_error_init(&e); const char *profile = NULL, *reason = NULL, *application = NULL;
            if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &profile, DBUS_TYPE_STRING, &reason, DBUS_TYPE_STRING, &application, DBUS_TYPE_INVALID)) {
                dbus_error_free(&e); return reply_error(m, DBUS_ERROR_INVALID_ARGS, "HoldProfile(sss)");
            }
            int p = profile_index(profile);
            if (p != 0 && p != 2) return reply_error(m, DBUS_ERROR_INVALID_ARGS, "Only profiles 'performance' and 'power-saver' can be a hold");
            if (!choices[p]) return reply_error(m, DBUS_ERROR_INVALID_ARGS, "profile not available");
            if (!caller_allowed(m)) return reply_error(m, DBUS_ERROR_ACCESS_DENIED, "Not authorized to hold a power profile");
            int slot = -1; for (unsigned i = 0; i < MAX_HOLDS; i++) if (!holds[i].used) { slot = (int)i; break; }
            if (slot < 0) return reply_error(m, DBUS_ERROR_LIMITS_EXCEEDED, "too many profile holds");
            struct hold* h = &holds[slot]; memset(h, 0, sizeof(*h)); h->used = true; h->cookie = next_cookie++; if (!next_cookie) next_cookie = 1;
            h->profile = (unsigned)p; snprintf(h->reason, sizeof(h->reason), "%s", reason); snprintf(h->application, sizeof(h->application), "%s", application);
            snprintf(h->owner, sizeof(h->owner), "%s", dbus_message_get_sender(m) ? dbus_message_get_sender(m) : "");
            DBusMessage* r = dbus_message_new_method_return(m);
            if (r) dbus_message_append_args(r, DBUS_TYPE_UINT32, &h->cookie, DBUS_TYPE_INVALID);
            send_and_unref(r); holds_changed(); return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (!strcmp(member, "ReleaseProfile")) {
            DBusError e; dbus_error_init(&e); uint32_t cookie = 0;
            if (!dbus_message_get_args(m, &e, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID)) { dbus_error_free(&e); return reply_error(m, DBUS_ERROR_INVALID_ARGS, "ReleaseProfile(u)"); }
            const char* sender = dbus_message_get_sender(m);
            for (unsigned i = 0; i < MAX_HOLDS; i++) if (holds[i].used && holds[i].cookie == cookie) {
                if (sender && strcmp(sender, holds[i].owner) && !caller_allowed(m)) return reply_error(m, DBUS_ERROR_ACCESS_DENIED, "not the holder");
                hold_release(i, true); DBusHandlerResult result = reply_empty(m); holds_changed(); return result;
            }
            return reply_error(m, DBUS_ERROR_INVALID_ARGS, "No hold with that cookie");
        }
        return reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
    }
    return reply_error(m, known_object ? DBUS_ERROR_UNKNOWN_INTERFACE : DBUS_ERROR_UNKNOWN_OBJECT, known_object ? "no such interface" : "no such object");
}
/* A hold ends when its holder leaves the bus. */
static DBusHandlerResult bus_filter(DBusConnection* connection, DBusMessage* m, void* data) {
    (void)connection; (void)data;
    if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        DBusError e; dbus_error_init(&e); const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
        if (dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) && !new_owner[0] && name[0] == ':') {
            bool any = false;
            for (unsigned i = 0; i < MAX_HOLDS; i++) if (holds[i].used && !strcmp(holds[i].owner, name)) { hold_release(i, true); any = true; }
            if (any) holds_changed();
        }
        dbus_error_free(&e);
    }
    if (dbus_message_is_signal(m, "org.freedesktop.DBus.Local", "Disconnected")) exit(0);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---- uevents ---- */
static int uevent_open(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    struct sockaddr_nl a = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (bind(fd, (struct sockaddr*)&a, sizeof(a))) { close(fd); return -1; }
    return fd;
}
/* Drain the socket; true when an event concerns power supplies, the profile, the lid or a system sleep. */
static bool uevent_relevant(int fd) {
    bool relevant = false; char buffer[4096];
    for (;;) {
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) break;
        buffer[n] = 0;
        for (ssize_t at = 0; at < n; at += (ssize_t)strlen(buffer + at) + 1) {
            const char* field = buffer + at;
            if (!strcmp(field, "SUBSYSTEM=power_supply") || !strncmp(field, "POWER_PROFILE=", 14) || !strncmp(field, "LID_STATE=", 10) ||
                !strncmp(field, "PM_EVENT=post-", 14) || !strcmp(field, "SUBSYSTEM=thermal")) relevant = true;
        }
    }
    return relevant;
}

int main(int argc, char** argv) {
    bool session = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--session")) session = true;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "--sysfs") && i + 1 < argc) sysfs = argv[++i];
        else if (!strcmp(argv[i], "--version")) { printf("nexus-upowerd (UPower %s, power-profiles-daemon %s API)\n", DAEMON_VERSION, PPD_VERSION); return 0; }
        else { fprintf(stderr, "usage: nexus-upowerd [--session] [--sysfs PREFIX] [--verbose]\n"); return 2; }
    }
    DBusError e; dbus_error_init(&e);
    bus = dbus_bus_get_private(session ? DBUS_BUS_SESSION : DBUS_BUS_SYSTEM, &e);
    if (!bus) { fprintf(stderr, "nexus-upowerd: no %s bus: %s\n", session ? "session" : "system", e.message); return 1; }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    refresh(true);
    static const DBusObjectPathVTable vtable = {.message_function = handle};
    if (!dbus_connection_register_fallback(bus, "/", &vtable, NULL) || !dbus_connection_add_filter(bus, bus_filter, NULL, NULL)) {
        fprintf(stderr, "nexus-upowerd: out of memory\n"); return 1;
    }
    dbus_bus_add_match(bus, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'", NULL);
    static const char* const names[] = {UP_IFACE, PP_IFACE, HADESS_IFACE};
    for (unsigned i = 0; i < 3; i++) {
        int got = dbus_bus_request_name(bus, names[i], DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
        if (got != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            fprintf(stderr, "nexus-upowerd: cannot own %s: %s\n", names[i], dbus_error_is_set(&e) ? e.message : "already owned (another UPower?)");
            if (!i) return 1;
            dbus_error_free(&e); dbus_error_init(&e);
        }
    }
    int dbus_fd = -1; dbus_connection_get_unix_fd(bus, &dbus_fd);
    int events = uevent_open();
    fprintf(stderr, "nexus-upowerd: ready (%u devices, profile %s%s)\n", (unsigned)({ unsigned c = 0; for (unsigned i = 0; i < MAX_DEVICES; i++) c += devices[i].used; c; }),
            profile_names[active_profile], events < 0 ? ", no uevents: changes are seen on Refresh only" : "");
    for (;;) {
        dbus_connection_flush(bus);
        while (dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS) dbus_connection_dispatch(bus);
        struct pollfd p[2] = {{dbus_fd, POLLIN, 0}, {events, POLLIN, 0}};
        int ready = poll(p, events >= 0 ? 2 : 1, -1);   /* no timeout: this daemon causes no idle wake-ups */
        if (ready < 0 && errno != EINTR) { perror("nexus-upowerd: poll"); return 1; }
        if (events >= 0 && (p[1].revents & POLLIN) && uevent_relevant(events)) refresh(false);
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (!dbus_connection_read_write(bus, 0)) break;   /* disconnected */
        }
        while (dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS) dbus_connection_dispatch(bus);
    }
    return 0;
}
