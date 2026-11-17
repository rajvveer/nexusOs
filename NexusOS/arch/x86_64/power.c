/* Power policy and the Linux-compatible user-space interfaces of the ACPI devices (power.h).
 *
 * Events (power_event) arrive from any context - the SCI handler (fixed buttons, RTC alarm), the ACPI worker
 * (Notify() handlers), the periodic refresh - into a small queue that power_poll drains on the boot CPU's
 * scheduler loop with the big kernel lock held. There they become:
 *   - input events on the keyboard evdev node, as Linux's ACPI button/video drivers report them: KEY_POWER,
 *     KEY_SLEEP, KEY_BRIGHTNESSUP/DOWN/CYCLE/ZERO, KEY_DISPLAY_OFF (Aurora and logind-style consumers react);
 *   - kernel uevents (NETLINK_KOBJECT_UEVENT, "change@<devpath>") for power_supply, thermal and backlight
 *     devices with their POWER_SUPPLY_* / THERMAL_* properties, and the power-management notifications
 *     DEVPATH=/kernel/power SUBSYSTEM=power PM_EVENT=suspend-prepare|post-suspend|hibernation-prepare|
 *     post-hibernation|freeze-prepare|post-freeze|shutdown-prepare (Linux's PM notifier names);
 *   - policy (/sys/power/policy/<name>, root or the console session owner may change it): power key
 *     (poweroff|reboot|suspend|hibernate|ignore|interactive; Aurora sets "interactive" and shows its own dialog), sleep key,
 *     lid close (suspend), low/critical battery thresholds and the critical action (hibernate, else poweroff),
 *     thermal: _CRT -> orderly power-off, _HOT -> hibernate, _PSV -> CPU performance cap (cpufreq.c),
 *     _ACx -> fans of _ALx.
 * Orderly shutdown (power key, thermal, battery, reboot policy): SIGTERM to every process but init, SIGKILL
 * after 10 s, then SIGTERM to init (Aurora); when init exits the kernel's finish path syncs, unmounts and powers
 * off (or resets, power_finish_reboot). With no process at all the kernel syncs and powers off directly.
 *
 * sysfs (sysfs_add ids, power_sysfs_read/write): /sys/class/power_supply/<AC|BATn>, /sys/class/thermal/
 * thermal_zoneN + cooling_deviceN, /sys/class/backlight/acpi_video0, /sys/power/{state,mem_sleep,disk,
 * wakeup_count,image_size,policy/<name>}, /sys/class/rtc/rtc0/{name,date,time,since_epoch,wakealarm,...}. */
#include "native.h"
#include "power.h"
#include "power_devices.h"
#include "gpu.h"
#include "boot_info.h"
#include "../../kernel/port.h"
#include "../../kernel/rtc.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

void netlink_uevent(const char* message, unsigned length);
void cpufreq_thermal_cap(unsigned percent);      /* cpufreq.c: 100 = no cap */
void cpufreq_sysfs_init(void);
int64_t cpufreq_sysfs_read(unsigned id, char* text, unsigned capacity);
int64_t cpufreq_sysfs_write(unsigned id, const char* text, unsigned length);
void cpufreq_init(void);
bool pm_hibernation_available(void);
bool power_finish_reboot;                         /* acpi.c platform_shutdown: reset instead of power off */

static void say(const char* s) { vga_print(s); }
static void say_number(uint64_t value) { char t[24]; number64(value, t); vga_print(t); }
static inline uint64_t irq_save(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_restore(uint64_t f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }

/* ---- text helpers ---- */
struct text { char* data; unsigned used, capacity; };
static void add(struct text* t, const char* s) { while (*s && t->used + 1 < t->capacity) t->data[t->used++] = *s++; t->data[t->used] = 0; }
static void add_number(struct text* t, uint64_t value) { char n[24]; number64(value, n); add(t, n); }
static void add_signed(struct text* t, int64_t value) { if (value < 0) { add(t, "-"); value = -value; } add_number(t, (uint64_t)value); }
static void add_two(struct text* t, unsigned value) { char n[3] = {(char)('0' + value / 10 % 10), (char)('0' + value % 10), 0}; add(t, n); }
static bool parse_number(const char* s, uint64_t* value) {
    uint64_t v = 0; bool any = false;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { if (v > (~0ULL - 9) / 10) return false; v = v * 10 + (uint64_t)(*s++ - '0'); any = true; }
    while (*s == ' ' || *s == '\n') s++;
    if (!any || *s) return false;
    *value = v; return true;
}
static bool word_is(const char* text, const char* word) {   /* sysfs store: trailing newline allowed */
    unsigned n = strlen(word);
    return !strncmp(text, word, n) && (!text[n] || (text[n] == '\n' && !text[n + 1]));
}

/* ---- event queue ---- */
#define EVENTS 64U
struct power_queued { uint16_t kind, device; uint32_t code, data; };
static struct power_queued events[EVENTS];
static unsigned event_head, event_count, event_dropped;
static volatile int event_lock;
void pm_wakeup_event(void);
void pm_sleep_ack(void);
uint64_t* pm_sleep_delay(void);
void power_event(unsigned kind, unsigned device_index, uint32_t code, uint32_t data) {
    /* Wakeup sources for suspend-to-idle: buttons, opening the lid, the RTC alarm, AC changes (as Linux's ACPI wakeup devices). */
    if (kind == POWER_EV_BUTTON_POWER || kind == POWER_EV_BUTTON_SLEEP || kind == POWER_EV_RTC || kind == POWER_EV_AC || (kind == POWER_EV_LID && data)) pm_wakeup_event();
    uint64_t f = irq_save(); spin_acquire(&event_lock);
    if (event_count == EVENTS) event_dropped++;
    else { events[(event_head + event_count) % EVENTS] = (struct power_queued){(uint16_t)kind, (uint16_t)device_index, code, data}; event_count++; }
    spin_release(&event_lock); irq_restore(f);
}
static bool event_pop(struct power_queued* e) {
    uint64_t f = irq_save(); spin_acquire(&event_lock);
    bool any = event_count != 0;
    if (any) { *e = events[event_head]; event_head = (event_head + 1) % EVENTS; event_count--; }
    spin_release(&event_lock); irq_restore(f);
    return any;
}

/* ---- policy ---- */
enum action { ACT_IGNORE, ACT_POWEROFF, ACT_REBOOT, ACT_SUSPEND, ACT_HIBERNATE, ACT_INTERACTIVE };
static const char* const action_names[] = {"ignore", "poweroff", "reboot", "suspend", "hibernate", "interactive"};
static unsigned policy_power_key = ACT_POWEROFF, policy_sleep_key = ACT_SUSPEND, policy_lid = ACT_SUSPEND, policy_critical = ACT_HIBERNATE;
static unsigned battery_low_percent = 10, battery_critical_percent = 5;
/* Lid closed on external power / with an external display (logind HandleLidSwitchExternalPower / HandleLidSwitchDocked,
 * macOS clamshell): until lid_switch_external_power is written it follows lid_switch; docked defaults to ignore. */
static unsigned policy_lid_ac = ACT_SUSPEND, policy_lid_docked = ACT_IGNORE; static bool lid_ac_set;
unsigned kms_connected_heads(void) __attribute__((weak));   /* kms.c: virtio-gpu heads the host shows */
int kms_lid(bool closed) __attribute__((weak));             /* kms.c: the built-in panel (head 0) off / back on */
static void lid_panel(void* closed) { if (kms_lid && kms_lid(closed != NULL) == -11) acpi_os_queue(lid_panel, closed); }   /* -11: retried next pass */
/* "interactive": the process that chose it (Aurora) answers KEY_POWER/KEY_SLEEP itself, like a logind inhibitor;
 * once it has exited the kernel's default action (poweroff / suspend) applies again. */
static uint32_t interactive_owner;
static unsigned key_policy(unsigned policy, unsigned fallback) {
    if (policy != ACT_INTERACTIVE) return policy;
    return interactive_owner && task_thread_count(interactive_owner) ? ACT_IGNORE : fallback;
}
static int parse_action(const char* text, unsigned* out, bool allow_interactive);
static int key_policy_store(const char* text, unsigned* policy) {
    int error = parse_action(text, policy, true);
    if (!error && *policy == ACT_INTERACTIVE) interactive_owner = task_pid();
    return error;
}
static bool session_owner_or_root(void) {
    struct credentials* who = credentials_current();
    if (!who) return true;   /* kernel context */
    if (credentials_capable(who, NULL, 21)) return true;   /* CAP_SYS_ADMIN */
    return task_pid_namespace() == pid_namespace_root() && !namespace_id(credentials_namespace(who)) && credentials_kernel_id(who, false, 1) == 1000;
}

/* ---- uevents ---- */
static uint64_t uevent_sequence;
struct uevent { char data[1536]; unsigned used; };
static void uevent_field(struct uevent* u, const char* key, const char* value) {
    unsigned k = strlen(key), v = strlen(value);
    if (u->used + k + v + 2 > sizeof(u->data)) return;
    memcpy(u->data + u->used, key, k); u->used += k;
    if (value[0] || key[k - 1] == '=') { memcpy(u->data + u->used, value, v); u->used += v; }
    u->data[u->used++] = 0;
}
static void uevent_number(struct uevent* u, const char* key, uint64_t value) { char n[24]; number64(value, n); uevent_field(u, key, n); }
static void uevent_begin(struct uevent* u, const char* devpath, const char* subsystem) {
    u->used = 0;
    char head[160] = "change@"; strcat(head, devpath); uevent_field(u, head, "");
    uevent_field(u, "ACTION=", "change"); uevent_field(u, "DEVPATH=", devpath); uevent_field(u, "SUBSYSTEM=", subsystem);
}
static void uevent_send(struct uevent* u) { uevent_number(u, "SEQNUM=", ++uevent_sequence); netlink_uevent(u->data, u->used); }
void power_notify_pm(const char* event) {
    struct uevent u; uevent_begin(&u, "/kernel/power", "power"); uevent_field(&u, "PM_EVENT=", event); uevent_send(&u);
    say("[64] PM EVENT "); say(event); say("\n");
}

/* ---- power supplies ---- */
static void supply_path(unsigned index, char* path, bool sys) {
    const struct supply_state* s = &power_devices.supplies[index].state;
    strcpy(path, sys ? "/sys" : ""); strcat(path, "/devices/LNXSYSTM:00/LNXSYBUS:00/");
    strcat(path, s->battery ? "PNP0C0A:0" : "ACPI0003:0"); char n[4]; uint_to_str(index, n); strcat(path, n);
    strcat(path, "/power_supply/"); strcat(path, s->name);
}
static unsigned battery_capacity(const struct supply_state* s) {
    if (!s->present || !s->full_capacity || s->full_capacity == 0xFFFFFFFFU || s->remaining == 0xFFFFFFFFU) return 0;
    uint64_t percent = (uint64_t)s->remaining * 100 / s->full_capacity;
    return percent > 100 ? 100 : (unsigned)percent;
}
static const char* battery_status(const struct supply_state* s) {
    static const char* const names[] = {"Unknown", "Charging", "Discharging", "Not charging", "Full"};
    return s->status < 5 ? names[s->status] : "Unknown";
}
static const char* battery_level(const struct supply_state* s) {
    if (!s->present || !s->state_valid) return "Unknown";
    if (s->critical) return "Critical";
    unsigned capacity = battery_capacity(s);
    if (s->status == SUPPLY_STATUS_FULL || capacity >= 100) return "Full";
    if ((s->low && s->remaining <= s->low) || capacity <= battery_critical_percent) return "Critical";
    if ((s->warning && s->remaining <= s->warning) || capacity <= battery_low_percent) return "Low";
    return "Normal";
}
static const char* battery_technology(const struct supply_state* s) {
    const char* k = s->kind;
    if (!strcmp(k, "NiCd")) return "NiCd";
    if (!strcmp(k, "NiMH")) return "NiMH";
    if (!strcmp(k, "LION") || !strcmp(k, "Li-ion") || !strcmp(k, "LI-ION") || !strcmp(k, "LiIon") || !strcmp(k, "Lion")) return "Li-ion";
    if (!strcmp(k, "LiP") || !strcmp(k, "LIP") || !strcmp(k, "LiPo")) return "Li-poly";
    return "Unknown";
}
static uint64_t micro(uint32_t value) { return value == 0xFFFFFFFFU ? 0 : (uint64_t)value * 1000; }
/* Battery attributes (Linux drivers/acpi/battery.c): energy_* (mWh units) or charge_* (mAh units). */
enum { SA_TYPE, SA_ONLINE, SA_UEVENT, SA_STATUS, SA_PRESENT, SA_TECHNOLOGY, SA_CYCLE_COUNT, SA_VOLTAGE_MIN_DESIGN, SA_VOLTAGE_NOW,
       SA_POWER_NOW, SA_FULL_DESIGN, SA_FULL, SA_NOW, SA_CAPACITY, SA_CAPACITY_LEVEL, SA_MODEL, SA_MANUFACTURER, SA_SERIAL, SA_ALARM,
       SA_SCOPE, SA_COUNT };
static const char* const battery_files[SA_COUNT] = {"type", "online", "uevent", "status", "present", "technology", "cycle_count",
    "voltage_min_design", "voltage_now", "power_now", "energy_full_design", "energy_full", "energy_now", "capacity", "capacity_level",
    "model_name", "manufacturer", "serial_number", "alarm", "scope"};
static const char* supply_file(const struct supply_state* s, unsigned a) {
    if (!s->mah) return battery_files[a];
    switch (a) {
    case SA_POWER_NOW: return "current_now";
    case SA_FULL_DESIGN: return "charge_full_design";
    case SA_FULL: return "charge_full";
    case SA_NOW: return "charge_now";
    default: return battery_files[a];
    }
}
static void supply_refresh(unsigned index) {   /* task or worker context: AML may run (Linux cache_time 1 s) */
    struct acpi_supply* supply = &power_devices.supplies[index];
    if (native_clock_ns() - supply->state.updated_ns < 1000000000ULL) return;
    if (supply->state.battery) acpi_battery_refresh(index, false); else acpi_ac_refresh(index);
}
static void supply_value(unsigned index, unsigned a, struct text* t);
static void supply_properties(unsigned index, struct text* t, bool uevent_form, struct uevent* u) {
    const struct supply_state* s = &power_devices.supplies[index].state;
    static const unsigned battery_props[] = {SA_STATUS, SA_PRESENT, SA_TECHNOLOGY, SA_CYCLE_COUNT, SA_VOLTAGE_MIN_DESIGN, SA_VOLTAGE_NOW, SA_POWER_NOW,
                                             SA_FULL_DESIGN, SA_FULL, SA_NOW, SA_CAPACITY, SA_CAPACITY_LEVEL, SA_MODEL, SA_MANUFACTURER, SA_SERIAL};
    char key[64], value[96];
    unsigned count = s->battery ? sizeof(battery_props) / sizeof(battery_props[0]) : 1;
    for (int i = -2; i < (int)count; i++) {
        unsigned a = i == -2 ? SA_TYPE : i == -1 ? SA_TYPE : s->battery ? battery_props[i] : SA_ONLINE;
        strcpy(key, "POWER_SUPPLY_");
        if (i == -2) { strcat(key, "NAME="); strcpy(value, s->name); }
        else {
            const char* file = supply_file(s, a); unsigned at = strlen(key);
            for (unsigned j = 0; file[j] && at + 2 < sizeof(key); j++) key[at++] = file[j] >= 'a' && file[j] <= 'z' ? (char)(file[j] - 32) : file[j];
            key[at++] = '='; key[at] = 0;
            struct text v = {value, 0, sizeof(value)}; value[0] = 0; supply_value(index, a, &v);
            if (v.used && value[v.used - 1] == '\n') value[--v.used] = 0;
        }
        if (uevent_form) uevent_field(u, key, value); else { add(t, key); add(t, value); add(t, "\n"); }
    }
}
static void supply_value(unsigned index, unsigned a, struct text* t) {
    const struct supply_state* s = &power_devices.supplies[index].state;
    switch (a) {
    case SA_TYPE: add(t, s->battery ? "Battery" : "Mains"); break;
    case SA_ONLINE: add(t, s->online ? "1" : "0"); break;
    case SA_UEVENT: supply_properties(index, t, false, NULL); return;
    case SA_STATUS: add(t, battery_status(s)); break;
    case SA_PRESENT: add(t, s->present ? "1" : "0"); break;
    case SA_TECHNOLOGY: add(t, battery_technology(s)); break;
    case SA_CYCLE_COUNT: add_number(t, s->cycle_count == 0xFFFFFFFFU ? 0 : s->cycle_count); break;
    case SA_VOLTAGE_MIN_DESIGN: add_number(t, micro(s->design_voltage)); break;
    case SA_VOLTAGE_NOW: add_number(t, micro(s->voltage)); break;
    case SA_POWER_NOW: add_number(t, micro(s->rate)); break;
    case SA_FULL_DESIGN: add_number(t, micro(s->design_capacity)); break;
    case SA_FULL: add_number(t, micro(s->full_capacity)); break;
    case SA_NOW: add_number(t, micro(s->remaining)); break;
    case SA_CAPACITY: add_number(t, battery_capacity(s)); break;
    case SA_CAPACITY_LEVEL: add(t, battery_level(s)); break;
    case SA_MODEL: add(t, s->model); break;
    case SA_MANUFACTURER: add(t, s->manufacturer); break;
    case SA_SERIAL: add(t, s->serial); break;
    case SA_ALARM: add_number(t, micro(s->warning)); break;
    case SA_SCOPE: add(t, "System"); break;
    }
    add(t, "\n");
}
static void supply_uevent(unsigned index) {
    char path[112]; supply_path(index, path, false);
    struct uevent u; uevent_begin(&u, path, "power_supply"); supply_properties(index, NULL, true, &u); uevent_send(&u);
}

/* ---- thermal ---- */
static int64_t millicelsius(uint32_t decikelvin) { return ((int64_t)decikelvin - 2732) * 100; }
struct trip { const char* type; uint32_t temperature; };
static unsigned zone_trips(unsigned index, struct trip* trips) {
    const struct thermal_state* z = &power_devices.zones[index].state; unsigned n = 0;
    if (z->critical) trips[n++] = (struct trip){"critical", z->critical};
    if (z->hot) trips[n++] = (struct trip){"hot", z->hot};
    if (z->passive) trips[n++] = (struct trip){"passive", z->passive};
    for (unsigned i = 0; i < z->active_count; i++) trips[n++] = (struct trip){"active", z->active[i]};
    return n;
}
enum { TA_TYPE, TA_TEMP, TA_MODE, TA_POLICY, TA_POLICIES, TA_UEVENT, TA_TRIP_TEMP, TA_TRIP_TYPE, TA_TRIP_HYST };
static void zone_value(unsigned index, unsigned a, unsigned trip, struct text* t) {
    const struct thermal_state* z = &power_devices.zones[index].state; struct trip trips[3 + THERMAL_ACTIVE_MAX]; unsigned count = zone_trips(index, trips);
    switch (a) {
    case TA_TYPE: add(t, "acpitz"); break;
    case TA_TEMP: add_signed(t, z->valid ? millicelsius(z->temperature) : 0); break;
    case TA_MODE: add(t, "enabled"); break;
    case TA_POLICY: add(t, "step_wise"); break;
    case TA_POLICIES: add(t, "step_wise"); break;
    case TA_UEVENT: add(t, "NAME=acpitz"); break;
    case TA_TRIP_TEMP: add_signed(t, trip < count ? millicelsius(trips[trip].temperature) : 0); break;
    case TA_TRIP_TYPE: add(t, trip < count ? trips[trip].type : "unknown"); break;
    case TA_TRIP_HYST: add(t, "0"); break;
    }
    add(t, "\n");
}
static void zone_uevent(unsigned index, const char* why) {
    char path[64] = "/devices/virtual/thermal/thermal_zone"; char n[4]; uint_to_str(index, n); strcat(path, n);
    struct uevent u; uevent_begin(&u, path, "thermal"); uevent_field(&u, "NAME=", "acpitz");
    char temp[24]; int64_t mc = millicelsius(power_devices.zones[index].state.temperature);
    if (mc < 0) { temp[0] = '-'; number64((uint64_t)-mc, temp + 1); } else number64((uint64_t)mc, temp);
    uevent_field(&u, "TEMP=", temp); uevent_field(&u, "EVENT=", why); uevent_send(&u);
}
/* Passive cooling (ACPI 11.1.5): every _TSP, dP[%] = _TC1 * (Tn - Tn-1) + _TC2 * (Tn - Tpsv); the CPU cap moves by dP. */
static unsigned passive_cap = 100;
static uint32_t passive_last[THERMAL_ZONES_MAX];
static uint64_t passive_due[THERMAL_ZONES_MAX];
static bool thermal_shutdown_started;

/* ---- backlight ---- */
enum { BA_BRIGHTNESS, BA_ACTUAL, BA_MAX, BA_TYPE, BA_POWER, BA_SCALE, BA_UEVENT };
static void backlight_uevent(unsigned index, const char* source) {
    char path[48] = "/devices/virtual/backlight/acpi_video"; char n[4]; uint_to_str(index, n); strcat(path, n);
    struct uevent u; uevent_begin(&u, path, "backlight");
    uevent_field(&u, "SOURCE=", source); uevent_number(&u, "BRIGHTNESS=", power_devices.backlights[index].current); uevent_send(&u);
}

/* ---- RTC alarm (MC146818: alarm registers 1/3/5, register B AIE, FADT day-of-month alarm) ---- */
static uint8_t cmos_get(uint8_t reg) { uint64_t f = irq_save(); port_byte_out(0x70, reg); uint8_t v = port_byte_in(0x71); irq_restore(f); return v; }
static void cmos_put(uint8_t reg, uint8_t value) { uint64_t f = irq_save(); port_byte_out(0x70, reg); port_byte_out(0x71, value); irq_restore(f); }
static uint64_t wakealarm_seconds;            /* armed alarm (seconds since the epoch on the RTC's clock), 0 = none */
static uint64_t wakealarm_system;             /* the same moment on the system clock (CLOCK_REALTIME seconds) */
uint8_t acpi_rtc_day_alarm(void);             /* acpi.c: FADT DAY_ALRM CMOS index (0 = none) */
static uint64_t epoch_now(void) { return native_realtime_ns() / 1000000000ULL; }
static void civil(uint64_t seconds, unsigned* year, unsigned* month, unsigned* day, unsigned* hour, unsigned* minute, unsigned* second) {
    uint64_t days = seconds / 86400, rest = seconds % 86400;
    *hour = (unsigned)(rest / 3600); *minute = (unsigned)(rest % 3600 / 60); *second = (unsigned)(rest % 60);
    /* days since 1970-01-01 -> civil date (Howard Hinnant's algorithm) */
    int64_t z = (int64_t)days + 719468, era = z / 146097; uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *day = (unsigned)(doy - (153 * mp + 2) / 5 + 1); *month = (unsigned)(mp < 10 ? mp + 3 : mp - 9); *year = (unsigned)(y + (*month <= 2));
}
static uint8_t to_cmos(unsigned value, bool binary) { return binary ? (uint8_t)value : (uint8_t)((value / 10) << 4 | value % 10); }
void acpi_rtc_wake_enable(bool enable);        /* acpi_aml.c: PM1 RTC_EN (wake from S3/S4) */
static bool alarm_posted;                     /* the due alarm's RTC event is queued (power_rtc_alarm_check) */
static uint64_t rtc_hardware_now(void);
static int wakealarm_set(uint64_t when) {
    uint8_t b = cmos_get(0x0B); alarm_posted = false;
    if (!when) { cmos_put(0x0B, b & ~0x20); wakealarm_seconds = 0; acpi_rtc_wake_enable(false); return 0; }
    /* `when` counts on the RTC's clock, as Linux's wakealarm / RTC_WKALM_SET ("+N" = RTC now + N): the system clock may
       have drifted from it since boot (TSC calibration, TCG), and an alarm the RTC has already passed never fires. */
    uint64_t now = rtc_hardware_now();
    if (when <= now) return -22;
    if (when - now > 31ULL * 86400) return -22;   /* the CMOS alarm reaches one month ahead at most */
    /* The RTC keeps local time = UTC here (time_init reads it as UTC). */
    unsigned year, month, day, hour, minute, second; civil(when, &year, &month, &day, &hour, &minute, &second);
    bool binary = (b & 0x04) != 0;
    if (!(b & 0x02)) {   /* 12-hour mode */
        unsigned h12 = hour % 12 ? hour % 12 : 12; uint8_t v = to_cmos(h12, binary); if (hour >= 12) v |= 0x80;
        cmos_put(0x05, v);
    } else cmos_put(0x05, to_cmos(hour, binary));
    cmos_put(0x01, to_cmos(second, binary)); cmos_put(0x03, to_cmos(minute, binary));
    uint8_t day_index = acpi_rtc_day_alarm();
    if (day_index) cmos_put(day_index, to_cmos(day, binary));
    else if (when - now >= 86400) return -22;    /* without a day alarm the alarm fires within 24 h */
    cmos_get(0x0C);                               /* clear pending flags */
    cmos_put(0x0B, b | 0x20);                     /* AIE */
    wakealarm_seconds = when; wakealarm_system = epoch_now() + (when - now);
    acpi_rtc_wake_enable(true);   /* the fixed RTC event: the alarm is seen while running and wakes S3/S5 */
    (void)year; (void)month;
    return 0;
}
static uint64_t rtc_alarms;                      /* alarms delivered: /dev/rtc0 read() returns when this moves */
void power_rtc_alarm_fired(void) {             /* SCI RTC_STS or the resume path: a fired alarm disarms (Linux rtc-cmos) */
    if (!wakealarm_seconds) return;
    bool fired = (cmos_get(0x0C) & 0x20) != 0;   /* register C AF (reading clears it) */
    if (!fired && epoch_now() + 2 < wakealarm_system) return;
    cmos_put(0x0B, cmos_get(0x0B) & ~0x20); wakealarm_seconds = 0; acpi_rtc_wake_enable(false);
    rtc_alarms++; task_wake_channel(&rtc_alarms); event_source_changed(&rtc_alarms, 1);
}
void power_resumed(void) { power_rtc_alarm_fired(); }
/* QEMU raises RTC_STS only for a guest in S3 and nothing services IRQ 8, so an armed wake alarm is also timed here:
 * power_poll (running) and pm.c's suspend-to-idle loop post the RTC event once it is due (rtcwake -m freeze,
 * suspend-then-hibernate on machines with suspend-to-idle only). */
uint64_t power_rtc_alarm_ns(void) {             /* CLOCK_MONOTONIC ns of the armed alarm, ~0 = none */
    uint64_t when = wakealarm_system;
    if (!wakealarm_seconds || alarm_posted) return ~0ULL;
    uint64_t at = when * 1000000000ULL, real = native_realtime_ns(), now = native_clock_ns();
    return at > real ? now + (at - real) : now;
}
void power_rtc_alarm_check(void) {
    if (wakealarm_seconds && !alarm_posted && epoch_now() >= wakealarm_system) { alarm_posted = true; power_event(POWER_EV_RTC, 0, 0, 0); }
}
/* Seam for posix_timers.c (the _ALARM clocks, Linux alarmtimer): at suspend the earliest alarm-clock timer is
 * programmed into the RTC unless the user's wakealarm comes first; at resume the user's alarm is set again.
 * `when` is CLOCK_REALTIME seconds; wakealarm_set() counts on the RTC's clock (the two may drift apart). */
static uint64_t user_alarm_saved; static bool kernel_alarm;
int power_alarm_suspend(uint64_t when) {
    if (wakealarm_seconds && wakealarm_system <= when) return 0;
    uint64_t real = epoch_now(), rtc = rtc_hardware_now();
    uint64_t saved = wakealarm_seconds; int error = wakealarm_set(when > real ? rtc + (when - real) : rtc + 1);
    if (!error) { user_alarm_saved = saved; kernel_alarm = true; }
    return error;
}
void power_alarm_resume(void) {
    if (!kernel_alarm) return;
    kernel_alarm = false; cmos_get(0x0C); wakealarm_set(0);   /* our alarm (fired or not) goes, its flag cleared */
    if (user_alarm_saved > rtc_hardware_now()) wakealarm_set(user_alarm_saved);
    user_alarm_saved = 0;
}

/* ---- /dev/rtc0: Linux's rtc-cmos character device (rtcwake, hwclock) ----
 * RTC_RD_TIME/RTC_SET_TIME (CAP_SYS_TIME), RTC_ALM_READ/RTC_ALM_SET (time of day, next occurrence), RTC_AIE_ON/OFF,
 * RTC_WKALM_RD/RTC_WKALM_SET (the same alarm as /sys/class/rtc/rtc0/wakealarm); update and periodic interrupts are not
 * offered (EINVAL: hwclock then polls the seconds). read() blocks until an alarm fires and returns
 * (count << 8) | RTC_IRQF | RTC_AF. Opened by root or the console session owner. */
struct linux_rtc_time { int32_t sec, min, hour, mday, mon, year, wday, yday, isdst; };
struct linux_rtc_wkalrm { uint8_t enabled, pending, pad[2]; struct linux_rtc_time time; };
_Static_assert(sizeof(struct linux_rtc_time) == 36 && sizeof(struct linux_rtc_wkalrm) == 40, "rtc ioctl ABI");
static uint64_t alarm_time;                      /* RTC_ALM_SET / RTC_WKALM_SET request (seconds since the epoch) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {   /* Howard Hinnant's inverse */
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void to_rtc_time(uint64_t epoch, struct linux_rtc_time* t) {
    unsigned y, mo, d, h, mi, s; civil(epoch, &y, &mo, &d, &h, &mi, &s);
    int64_t days = (int64_t)(epoch / 86400);
    *t = (struct linux_rtc_time){(int32_t)s, (int32_t)mi, (int32_t)h, (int32_t)d, (int32_t)mo - 1, (int32_t)y - 1900, (int32_t)((days + 4) % 7),
                                 (int32_t)(days - days_from_civil(y, 1, 1)), 0};
}
static int from_rtc_time(const struct linux_rtc_time* t, uint64_t* epoch) {
    if (t->sec < 0 || t->sec > 59 || t->min < 0 || t->min > 59 || t->hour < 0 || t->hour > 23 || t->mday < 1 || t->mday > 31 ||
        t->mon < 0 || t->mon > 11 || t->year < 70 || t->year > 1100) return -22;
    static const uint8_t month_days[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};   /* Linux rtc_valid_tm */
    unsigned year = (unsigned)t->year + 1900; bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (t->mday > month_days[t->mon] || (t->mon == 1 && t->mday == 29 && !leap)) return -22;
    int64_t days = days_from_civil(t->year + 1900, (unsigned)t->mon + 1, (unsigned)t->mday);
    *epoch = (uint64_t)days * 86400 + (uint64_t)t->hour * 3600 + (uint64_t)t->min * 60 + (uint64_t)t->sec; return 0;
}
static uint64_t rtc_hardware_now(void) {
    rtc_time_t t; rtc_read(&t);
    if (!t.month || t.month > 12 || !t.day || t.day > 31 || t.year < 1970) return epoch_now();
    return (uint64_t)days_from_civil(t.year, t.month, t.day) * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
}
int rtc_device_open_check(uint64_t flags) { (void)flags; return session_owner_or_root() ? 0 : -13; }
void rtc_device_open(struct input_reader* reader) { reader->sequence = rtc_alarms; }
void* rtc_device_source(void) { return &rtc_alarms; }
uint32_t rtc_device_ready(const struct input_reader* reader) { return reader->sequence != rtc_alarms ? 1U : 4U; }   /* POLLIN / POLLOUT */
int64_t rtc_device_read(uint64_t buffer, uint64_t count, bool nonblock, struct input_reader* reader) {
    if (count < 4) return -22;
    while (reader->sequence == __atomic_load_n(&rtc_alarms, __ATOMIC_ACQUIRE)) {
        if (nonblock) return -11;   /* the 1 s deadline re-checks: a wake between the check and the sleep is not lost */
        if (task_wait_until(&rtc_alarms, native_clock_ns() + 1000000000ULL) == -4) return -4;
    }
    uint64_t seen = __atomic_load_n(&rtc_alarms, __ATOMIC_ACQUIRE);   /* one snapshot: an alarm may fire meanwhile */
    uint64_t value = (seen - reader->sequence) << 8 | 0x80 | 0x20; reader->sequence = seen;
    unsigned size = count >= 8 ? 8 : 4;
    if (!accessible(buffer, size, true)) return -14;
    return copy_to_user((void*)buffer, &value, size) ? -14 : (int64_t)size;
}
int rtc_device_ioctl(uint64_t command, uint64_t argument) {
    struct linux_rtc_time t; struct linux_rtc_wkalrm w; uint64_t when; int error;
    switch ((uint32_t)command) {
    case 0x80247009:   /* RTC_RD_TIME */
        to_rtc_time(rtc_hardware_now(), &t);
        if (!accessible(argument, sizeof(t), true)) return -14;
        return copy_to_user((void*)argument, &t, sizeof(t)) ? -14 : 0;
    case 0x4024700A: {   /* RTC_SET_TIME */
        if (!credentials_capable(credentials_current(), NULL, 25)) return -1;   /* CAP_SYS_TIME */
        if (!accessible(argument, sizeof(t), false)) return -14;
        copy_from_user(&t, (void*)argument, sizeof(t));
        if ((error = from_rtc_time(&t, &when))) return error;
        rtc_time_t hw = {(uint8_t)t.sec, (uint8_t)t.min, (uint8_t)t.hour, (uint8_t)t.mday, (uint8_t)(t.mon + 1), (uint16_t)(t.year + 1900)};
        rtc_set(&hw); return 0; }
    case 0x80247008:   /* RTC_ALM_READ: time of day */
        to_rtc_time(wakealarm_seconds ? wakealarm_seconds : alarm_time, &t);
        if (!accessible(argument, sizeof(t), true)) return -14;
        return copy_to_user((void*)argument, &t, sizeof(t)) ? -14 : 0;
    case 0x40247007: {   /* RTC_ALM_SET: the next time this time of day comes */
        if (!accessible(argument, sizeof(t), false)) return -14;
        copy_from_user(&t, (void*)argument, sizeof(t));
        if (t.sec < 0 || t.sec > 59 || t.min < 0 || t.min > 59 || t.hour < 0 || t.hour > 23) return -22;
        uint64_t now = rtc_hardware_now(), day = now - now % 86400;
        when = day + (uint64_t)t.hour * 3600 + (uint64_t)t.min * 60 + (uint64_t)t.sec; if (when <= now) when += 86400;
        alarm_time = when;
        return wakealarm_seconds ? wakealarm_set(alarm_time) : 0; }
    case 0x7001: return alarm_time ? wakealarm_set(alarm_time) : -22;   /* RTC_AIE_ON */
    case 0x7002: return wakealarm_set(0);                               /* RTC_AIE_OFF */
    case 0x80287010:   /* RTC_WKALM_RD */
        w = (struct linux_rtc_wkalrm){wakealarm_seconds != 0, 0, {0}, {0}}; to_rtc_time(wakealarm_seconds ? wakealarm_seconds : alarm_time, &w.time);
        if (!accessible(argument, sizeof(w), true)) return -14;
        return copy_to_user((void*)argument, &w, sizeof(w)) ? -14 : 0;
    case 0x4028700F:   /* RTC_WKALM_SET */
        if (!accessible(argument, sizeof(w), false)) return -14;
        copy_from_user(&w, (void*)argument, sizeof(w));
        if ((error = from_rtc_time(&w.time, &when))) return error;
        alarm_time = when;
        if (!w.enabled) return wakealarm_set(0);
        say("[64] POWER: RTC wake alarm set through /dev/rtc0 in "); say_number(when > rtc_hardware_now() ? when - rtc_hardware_now() : 0); say(" s\n");
        return wakealarm_set(when);
    case 0x7003: case 0x7004: case 0x7005: case 0x7006: case 0x8008700B: case 0x4008700C: case 0x8008700D: case 0x4008700E:
        return -22;   /* update/periodic interrupts, IRQ rate, epoch: not provided */
    }
    return -25;
}

/* ---- orderly shutdown ---- */
static int orderly_stage; static uint64_t orderly_at; static const char* orderly_reason;
static bool orderly_only_init(void) {
    for (uint32_t pid = task_next_id(0, 0); pid; pid = task_next_id(0, pid)) if (pid != 1 && task_thread_count(pid)) return false;
    return true;
}
static void orderly_signal(unsigned number, bool init) {
    for (uint32_t pid = task_next_id(0, 0); pid; pid = task_next_id(0, pid))
        if ((pid == 1) == init && task_thread_count(pid)) task_signal_process(pid, number);
}
bool power_orderly_active(void) { return orderly_stage != 0; }
void power_orderly(bool reboot, const char* reason) {
    if (orderly_stage) return;
    power_finish_reboot = reboot; orderly_reason = reason; orderly_stage = 1; orderly_at = native_clock_ns();
    say("[64] POWER ORDERLY "); say(reboot ? "REBOOT" : "SHUTDOWN"); say(": "); say(reason); say("\n");
    power_notify_pm("shutdown-prepare");
    if (!task_next_id(0, 0)) {   /* no user space at all: sync and stop here */
        memory_file_writeback(true, 0, ~0ULL); disk_shutdown(); block_flush_all(); nvme_shutdown_all();
        if (reboot) acpi_reset();
        platform_shutdown(0x10);
    }
    orderly_signal(15, false);
}
static void orderly_step(void) {
    if (!orderly_stage) return;
    uint64_t elapsed = native_clock_ns() - orderly_at;
    bool urgent = thermal_shutdown_started;      /* thermal: Linux's emergency power-off delay is short */
    if (orderly_stage == 1 && (orderly_only_init() || elapsed > (urgent ? 3000000000ULL : 10000000000ULL))) {
        orderly_signal(9, false); memory_file_writeback(true, 0, ~0ULL);
        orderly_stage = 2; orderly_at = native_clock_ns(); orderly_signal(15, true);
        say("[64] POWER ORDERLY: applications ended, stopping init\n");
    } else if (orderly_stage == 2 && elapsed > (urgent ? 3000000000ULL : 10000000000ULL)) {
        orderly_stage = 3; orderly_signal(9, true);
    }
}
static void act(unsigned action, const char* reason) {
    switch (action) {
    case ACT_POWEROFF: power_orderly(false, reason); break;
    case ACT_REBOOT: power_orderly(true, reason); break;
    case ACT_SUSPEND: if (pm_request_async(PM_STATE_MEM)) say("[64] POWER: suspend unavailable\n"); break;
    case ACT_HIBERNATE: if (pm_request_async(PM_STATE_DISK)) { say("[64] POWER: hibernation unavailable; powering off\n"); power_orderly(false, reason); } break;
    default: break;
    }
}

/* ---- keys ---- */
static bool keys_attached;
static void key_tap(unsigned code) {
    if (!keys_attached) {
        uint8_t bits[96] = {0}; static const unsigned codes[] = {116, 142, 143, 224, 225, 243, 244, 245};
        for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) bits[codes[i] / 8] |= (uint8_t)(1U << (codes[i] % 8));
        input_source_attach(0, bits, false); keys_attached = true;
    }
    input_source_key(0, code, 1); input_source_report(0); input_source_key(0, code, 0); input_source_report(0);
}

/* ---- the event loop ---- */
static bool on_external_power(void) {   /* an online AC adapter, or no battery at all (systemd on_ac_power) */
    bool battery = false;
    for (unsigned i = 0; i < power_devices.supply_count; i++) {
        const struct supply_state* s = &power_devices.supplies[i].state;
        if (!s->battery && s->online) return true;
        if (s->battery && s->present) battery = true;
    }
    return !battery;
}
static bool battery_alerted_low[SUPPLIES_MAX], battery_alerted_critical[SUPPLIES_MAX];
static uint64_t critical_deadline;
static void battery_policy(void) {
    bool on_ac = false, any_battery = false, critical = false;
    for (unsigned i = 0; i < power_devices.supply_count; i++) {
        const struct supply_state* s = &power_devices.supplies[i].state;
        if (!s->battery) { if (s->online) on_ac = true; continue; }
        if (!s->present || !s->state_valid) continue;
        any_battery = true;
        const char* level = battery_level(s);
        bool discharging = s->status == SUPPLY_STATUS_DISCHARGING;
        if (discharging && !strcmp(level, "Low") && !battery_alerted_low[i]) {
            battery_alerted_low[i] = true; say("[64] POWER: battery "); say(s->name); say(" low ("); say_number(battery_capacity(s)); say("%)\n"); supply_uevent(i);
        }
        if (!discharging || strcmp(level, "Low")) { if (strcmp(level, "Critical")) battery_alerted_low[i] = false; }
        if (discharging && !strcmp(level, "Critical")) critical = true;
        if (!discharging) battery_alerted_critical[i] = false;
    }
    if (!any_battery) return;
    if (critical && !on_ac) {
        if (!critical_deadline) {
            critical_deadline = native_clock_ns() + 10000000000ULL;   /* 10 s for the desktop to warn the user */
            for (unsigned i = 0; i < power_devices.supply_count; i++) if (power_devices.supplies[i].state.battery) { battery_alerted_critical[i] = true; supply_uevent(i); }
            say("[64] POWER: battery critical; "); say(action_names[policy_critical]); say(" in 10 s unless AC returns\n");
        } else if (native_clock_ns() >= critical_deadline) {
            critical_deadline = ~0ULL;   /* once */
            act(policy_critical == ACT_HIBERNATE && !pm_hibernation_available() ? ACT_POWEROFF : policy_critical, "critical battery");
        }
    } else if (critical_deadline && critical_deadline != ~0ULL) { critical_deadline = 0; say("[64] POWER: critical battery action cancelled\n"); }
    else if (!critical) critical_deadline = 0;
}
static void thermal_policy(unsigned index) {
    struct thermal_state* z = &power_devices.zones[index].state;
    if (!z->valid) return;
    if (z->critical && z->temperature >= z->critical && !thermal_shutdown_started) {
        thermal_shutdown_started = true;
        say("[64] POWER: critical temperature "); say_number((uint64_t)millicelsius(z->temperature) / 1000); say(" C reached, shutting down\n");
        zone_uevent(index, "critical"); power_orderly(false, "critical temperature"); return;
    }
    static bool hot_done[THERMAL_ZONES_MAX];
    if (z->hot && z->temperature >= z->hot && !hot_done[index]) {
        hot_done[index] = true; zone_uevent(index, "hot"); say("[64] POWER: hot temperature trip, hibernating\n");
        act(pm_hibernation_available() ? ACT_HIBERNATE : ACT_POWEROFF, "hot temperature");
    } else if (!z->hot || z->temperature + 50 < z->hot) hot_done[index] = false;
    /* active cooling: the hottest _ACx at or below the temperature selects the fans (1 K hysteresis down) */
    int level = -1;
    for (unsigned i = 0; i < z->active_count; i++) if (z->temperature >= z->active[i]) { level = (int)i; break; }
    if (level < 0 && z->active_level >= 0 && z->temperature + 10 >= z->active[z->active_level]) level = z->active_level;
    if (level != z->active_level) {
        acpi_thermal_set_active(index, level);
        say("[64] POWER: thermal zone "); say_number(index); say(level < 0 ? " fans off\n" : " active cooling level "); if (level >= 0) { say_number((uint64_t)level); say("\n"); }
    }
    /* passive cooling */
    if (z->passive && (z->temperature >= z->passive || passive_cap < 100) && native_clock_ns() >= passive_due[index]) {
        int64_t delta = (int64_t)z->tc1 * ((int64_t)z->temperature - (int64_t)(passive_last[index] ? passive_last[index] : z->temperature)) +
                        (int64_t)z->tc2 * ((int64_t)z->temperature - (int64_t)z->passive);
        delta /= 10;   /* temperatures are tenths of a degree */
        int64_t cap = (int64_t)passive_cap - delta;
        if (z->temperature < z->passive && cap <= (int64_t)passive_cap) cap = passive_cap + 10;   /* below the trip: release */
        if (cap > 100) cap = 100;
        if (cap < 10) cap = 10;
        if ((unsigned)cap != passive_cap) { passive_cap = (unsigned)cap; cpufreq_thermal_cap(passive_cap); say("[64] POWER: passive cooling cap "); say_number(passive_cap); say("%\n"); }
        passive_due[index] = native_clock_ns() + (uint64_t)(z->tsp ? z->tsp : 10) * 100000000ULL;
    }
    passive_last[index] = z->temperature;
}
static void handle(const struct power_queued* e) {
    switch (e->kind) {
    case POWER_EV_BUTTON_POWER:   /* the press that woke the system is KEY_WAKEUP only (Linux acpi_button) */
        if (pm_waking()) { say("[64] POWER BUTTON pressed (wake)\n"); key_tap(143); break; }
        say("[64] POWER BUTTON pressed\n"); key_tap(116); act(key_policy(policy_power_key, ACT_POWEROFF), "power button");
        break;
    case POWER_EV_BUTTON_SLEEP:
        if (pm_waking()) { say("[64] SLEEP BUTTON pressed (wake)\n"); key_tap(143); break; }
        say("[64] SLEEP BUTTON pressed\n"); key_tap(142); act(key_policy(policy_sleep_key, ACT_SUSPEND), "sleep button");
        break;
    case POWER_EV_LID:
        say(e->data ? "[64] LID opened\n" : "[64] LID closed\n");
        { struct uevent u; uevent_begin(&u, "/devices/LNXSYSTM:00/LNXSYBUS:00/PNP0C0D:00", "acpi"); uevent_field(&u, "LID_STATE=", e->data ? "open" : "closed"); uevent_send(&u); }
        if (!e->data) {
            bool docked = kms_connected_heads && kms_connected_heads() > 1, ac = lid_ac_set && on_external_power();
            unsigned action = docked ? policy_lid_docked : ac ? policy_lid_ac : policy_lid;
            if (docked || ac) { say(docked ? "[64] POWER: lid closed with an external display: " : "[64] POWER: lid closed on external power: "); say(action_names[action]); say("\n"); }
            if (action == ACT_IGNORE) lid_panel((void*)1);   /* the machine stays up: the built-in panel goes dark */
            act(action, "lid closed");
        } else lid_panel(NULL);
        break;
    case POWER_EV_AC:
        say("[64] POWER AC "); say(power_devices.supplies[e->device].state.name); say(e->data ? " online\n" : " offline\n");
        supply_uevent(e->device);
        for (unsigned i = 0; i < power_devices.supply_count; i++) if (power_devices.supplies[i].state.battery) supply_uevent(i);
        battery_policy(); break;
    case POWER_EV_BATTERY: {
        const struct supply_state* s = &power_devices.supplies[e->device].state;
        say("[64] POWER BATTERY "); say(s->name); say(" "); say(battery_status(s)); say(" "); say_number(battery_capacity(s)); say("% ");
        say(battery_level(s)); say(e->code == 0x81 ? " (information changed)\n" : "\n");
        supply_uevent(e->device); battery_policy(); break;
    }
    case POWER_EV_THERMAL:
        say("[64] POWER THERMAL zone "); say_number(e->device); say(" temp "); say_number((uint64_t)(millicelsius(power_devices.zones[e->device].state.temperature) / 100)); say(" dC\n");
        zone_uevent(e->device, e->code == 0x81 ? "trips" : "temperature"); thermal_policy(e->device); break;
    case POWER_EV_BACKLIGHT: {
        struct acpi_backlight* b = &power_devices.backlights[e->device]; unsigned key = 0;
        if (e->code == 0x86 && b->current + 1 < b->count) { acpi_backlight_set(e->device, b->current + 1); key = 225; }
        else if (e->code == 0x86) key = 225;
        else if (e->code == 0x87) { if (b->current) acpi_backlight_set(e->device, b->current - 1); key = 224; }
        else if (e->code == 0x88) { acpi_backlight_set(e->device, b->current + 1 < b->count ? b->current + 1 : 0); key = 243; }
        else if (e->code == 0x89) { acpi_backlight_set(e->device, 0); key = 244; }
        else if (e->code == 0x8A) key = 245;
        say("[64] POWER BACKLIGHT hotkey "); say_number(e->code); say(" level "); say_number(b->current); say("\n");
        if (key) key_tap(key);
        backlight_uevent(e->device, "hotkey"); break;
    }
    case POWER_EV_RTC: say("[64] POWER RTC alarm\n"); power_rtc_alarm_fired(); break;
    default: break;
    }
}
/* Worker context (AML may sleep): periodic refresh of batteries and thermal zones. */
static volatile bool refresh_queued;
static uint64_t thermal_next[THERMAL_ZONES_MAX], battery_next;
static void refresh_work(void* context) {
    (void)context; uint64_t now = native_clock_ns();
    if (now >= battery_next) {
        battery_next = now + 5000000000ULL;   /* 5 s: batteries drain slowly; notifications cover the rest */
        for (unsigned i = 0; i < power_devices.supply_count; i++) {
            struct supply_state before = power_devices.supplies[i].state;
            if (power_devices.supplies[i].state.battery) acpi_battery_refresh(i, false); else acpi_ac_refresh(i);
            const struct supply_state* after = &power_devices.supplies[i].state;
            if (before.online != after->online || before.status != after->status || before.remaining != after->remaining || before.present != after->present)
                power_event(after->battery ? POWER_EV_BATTERY : POWER_EV_AC, i, 0, after->online);
        }
    }
    for (unsigned i = 0; i < power_devices.zone_count; i++) {
        struct thermal_state* z = &power_devices.zones[i].state;
        if (now < thermal_next[i]) continue;
        uint64_t period = z->polling ? (uint64_t)z->polling * 100000000ULL : 10000000000ULL;
        if (z->passive && passive_cap < 100) period = (uint64_t)(z->tsp ? z->tsp : 10) * 100000000ULL;
        thermal_next[i] = now + period;
        uint32_t before = z->temperature; acpi_thermal_refresh(i, false);
        if (z->temperature != before || (z->passive && passive_cap < 100)) power_event(POWER_EV_THERMAL, i, 0, z->temperature);
    }
    refresh_queued = false;
}
static uint64_t next_refresh;
void power_poll(void) {
    power_rtc_alarm_check();
    acpi_worker_poll();
    struct power_queued e;
    for (unsigned i = 0; i < EVENTS && event_pop(&e); i++) handle(&e);
    orderly_step();
    pm_poll();
    uint64_t now = native_clock_ns();
    if (acpi_aml_ready() && now >= next_refresh && !refresh_queued && (power_devices.supply_count || power_devices.zone_count)) {
        next_refresh = now + 1000000000ULL;
        refresh_queued = true;
        if (acpi_os_queue(refresh_work, NULL)) refresh_queued = false;
    }
    if (critical_deadline && critical_deadline != ~0ULL && now >= critical_deadline) battery_policy();
}
/* tick.c: when power_poll needs the boot CPU next while the system idles. */
uint64_t power_next_event(uint64_t now) {
    if (__atomic_load_n(&event_count, __ATOMIC_RELAXED) || pm_transition_pending()) return now;
    uint64_t due = acpi_worker_next_event(now);
    if (orderly_stage && now + 10000000ULL < due) due = now + 10000000ULL;   /* orderly shutdown: its timeouts */
    if (critical_deadline && critical_deadline != ~0ULL && critical_deadline < due) due = critical_deadline;
    { uint64_t alarm = power_rtc_alarm_ns(); if (alarm < due) due = alarm; }
    if (acpi_aml_ready() && !refresh_queued && (power_devices.supply_count || power_devices.zone_count)) {
        uint64_t refresh = power_devices.supply_count ? battery_next : ~0ULL;   /* refresh_work's own due times */
        for (unsigned i = 0; i < power_devices.zone_count; i++) if (thermal_next[i] < refresh) refresh = thermal_next[i];
        if (refresh < next_refresh) refresh = next_refresh;
        if (refresh < due) due = refresh;
    }
    return due;
}

/* ---- sysfs ---- */
enum { CLASS_SUPPLY = 1, CLASS_ZONE, CLASS_COOLING, CLASS_BACKLIGHT, CLASS_POWER, CLASS_RTC, CLASS_CPUFREQ, CLASS_CPUIDLE, CLASS_TICK };   /* 7, 8: cpufreq.c; 9: tick.c */
#define ID(c, instance, attribute) ((unsigned)(c) << 12 | (unsigned)(instance) << 6 | (unsigned)(attribute))
enum { PA_STATE, PA_MEM_SLEEP, PA_DISK, PA_WAKEUP_COUNT, PA_IMAGE_SIZE, PA_POWER_KEY, PA_SLEEP_KEY, PA_LID, PA_CRITICAL, PA_LOW_PERCENT, PA_CRITICAL_PERCENT,
       PA_SLEEP_DELAY, PA_SLEEP_ACK, PA_RESUMES, PA_LAST, PA_PROFILE, PA_PROFILE_CHOICES, PA_LID_STATE, PA_RESUME, PA_RESUME_OFFSET,
       PA_LID_AC, PA_LID_DOCKED };
int hibernate_resume_set(const char* spec); int hibernate_resume_offset_set(uint64_t page);   /* hibernate.c */
void hibernate_resume_get(unsigned* major, unsigned* minor, uint64_t* page);
static const char* const profile_names[] = {"power-saver", "balanced", "performance"};
unsigned cpufreq_profile(void); bool cpufreq_profile_supported(unsigned value); int cpufreq_set_profile(unsigned value);
enum { RA_NAME, RA_DATE, RA_TIME, RA_SINCE, RA_WAKEALARM, RA_MAX_FREQ, RA_HCTOSYS, RA_UEVENT, RA_DEV, RA_WAKEUP };
enum { CA_TYPE, CA_MAX, CA_CUR };
static uint64_t wakeup_count;
static unsigned mem_sleep_deep = 1;   /* "mem" means S3 ("deep") when the firmware has \_S3, else s2idle */
static unsigned disk_mode;            /* 0 platform (S4), 1 shutdown, 2 reboot */
static uint64_t image_size_bytes;
unsigned pm_disk_mode(void) { return disk_mode; }
/* /sys/power/image_size: the image the hibernation tries to shrink to (Linux: 2/5 of RAM by default) */
uint64_t pm_image_size(void) { return image_size_bytes ? image_size_bytes : memory_total_pages() * 4096 * 2 / 5; }
bool pm_mem_is_deep(void) { return mem_sleep_deep && acpi_sleep_state_supported(3); }
int64_t power_sysfs_read(unsigned id, char* buffer, unsigned capacity) {
    struct text t = {buffer, 0, capacity}; buffer[0] = 0;
    unsigned c = id >> 12, instance = (id >> 6) & 63, a = id & 63;
    switch (c) {
    case CLASS_SUPPLY:
        if (instance >= power_devices.supply_count) return -2;
        if (cpu_self()->task) supply_refresh(instance);
        supply_value(instance, a, &t); break;
    case CLASS_ZONE:
        if ((instance & 31) >= power_devices.zone_count) return -2;
        if (a == TA_TEMP && cpu_self()->task && native_clock_ns() - power_devices.zones[instance & 31].state.updated_ns > 500000000ULL) acpi_thermal_refresh(instance & 31, false);
        zone_value(instance & 31, a >= 16 ? TA_TRIP_TEMP + (a - 16) % 3 : a, a >= 16 ? (a - 16) / 3 : 0, &t); break;
    case CLASS_COOLING: {
        unsigned zone = instance >> 1; bool fan = instance & 1;
        if (zone >= power_devices.zone_count) return -2;
        const struct thermal_state* z = &power_devices.zones[zone].state;
        if (a == CA_TYPE) add(&t, fan ? "Fan" : "Processor");
        else if (a == CA_MAX) add_number(&t, fan ? z->active_count : 9);
        else add_number(&t, fan ? (z->active_level < 0 ? 0 : z->active_count - (unsigned)z->active_level) : (100 - passive_cap) / 10);
        add(&t, "\n"); break;
    }
    case CLASS_BACKLIGHT: {
        if (instance >= power_devices.backlight_count) return -2;
        struct acpi_backlight* b = &power_devices.backlights[instance];
        if (a == BA_ACTUAL && cpu_self()->task) acpi_backlight_refresh(instance);
        switch (a) {
        case BA_BRIGHTNESS: case BA_ACTUAL: add_number(&t, b->current); break;
        case BA_MAX: add_number(&t, b->count - 1); break;
        case BA_TYPE: add(&t, "firmware"); break;
        case BA_POWER: add(&t, "0"); break;
        case BA_SCALE: add(&t, "unknown"); break;
        case BA_UEVENT: break;
        }
        if (a != BA_UEVENT) add(&t, "\n");
        break;
    }
    case CLASS_POWER:
        switch (a) {
        case PA_STATE: { bool any = false;
            add(&t, "freeze"); any = true;
            if (acpi_sleep_state_supported(3)) { add(&t, any ? " mem" : "mem"); }
            if (pm_hibernation_available()) add(&t, " disk");
            add(&t, "\n"); break; }
        case PA_MEM_SLEEP: add(&t, pm_mem_is_deep() ? "s2idle [deep]\n" : acpi_sleep_state_supported(3) ? "[s2idle] deep\n" : "[s2idle]\n"); break;
        case PA_DISK: add(&t, disk_mode == 0 ? "[platform] shutdown reboot\n" : disk_mode == 1 ? "platform [shutdown] reboot\n" : "platform shutdown [reboot]\n"); break;
        case PA_WAKEUP_COUNT: add_number(&t, wakeup_count); add(&t, "\n"); break;
        case PA_IMAGE_SIZE: add_number(&t, image_size_bytes ? image_size_bytes : memory_total_pages() * 4096 * 2 / 5); add(&t, "\n"); break;
        case PA_POWER_KEY: add(&t, action_names[policy_power_key]); add(&t, "\n"); break;
        case PA_SLEEP_KEY: add(&t, action_names[policy_sleep_key]); add(&t, "\n"); break;
        case PA_LID: add(&t, action_names[policy_lid]); add(&t, "\n"); break;
        case PA_LID_AC: add(&t, action_names[lid_ac_set ? policy_lid_ac : policy_lid]); add(&t, "\n"); break;
        case PA_LID_DOCKED: add(&t, action_names[policy_lid_docked]); add(&t, "\n"); break;
        case PA_CRITICAL: add(&t, action_names[policy_critical]); add(&t, "\n"); break;
        case PA_LOW_PERCENT: add_number(&t, battery_low_percent); add(&t, "\n"); break;
        case PA_CRITICAL_PERCENT: add_number(&t, battery_critical_percent); add(&t, "\n"); break;
        case PA_SLEEP_DELAY: add_number(&t, *pm_sleep_delay()); add(&t, "\n"); break;
        case PA_SLEEP_ACK: add(&t, "0\n"); break;
        case PA_RESUMES: add_number(&t, pm_resume_count()); add(&t, "\n"); break;
        case PA_LAST: add(&t, pm_last_event()); add(&t, "\n"); break;
        case PA_PROFILE: add(&t, profile_names[cpufreq_profile() % 3]); add(&t, "\n"); break;
        case PA_PROFILE_CHOICES:
            for (unsigned p = 0; p < 3; p++) if (cpufreq_profile_supported(p)) { if (t.used) add(&t, " "); add(&t, profile_names[p]); }
            add(&t, "\n"); break;
        case PA_LID_STATE: add(&t, !power_devices.lid ? "absent\n" : (cpu_self()->task ? acpi_lid_open() : power_devices.lid_open) ? "open\n" : "closed\n"); break;
        case PA_RESUME: case PA_RESUME_OFFSET: {
            unsigned major, minor; uint64_t page; hibernate_resume_get(&major, &minor, &page);
            if (a == PA_RESUME) { add_number(&t, major); add(&t, ":"); add_number(&t, minor); } else add_number(&t, page);
            add(&t, "\n"); break;
        }
        }
        break;
    case CLASS_RTC: {
        uint64_t now = epoch_now(); unsigned y, mo, d, h, mi, s; civil(now, &y, &mo, &d, &h, &mi, &s);
        switch (a) {
        case RA_NAME: add(&t, "rtc_cmos"); break;
        case RA_DATE: add_number(&t, y); add(&t, "-"); add_two(&t, mo); add(&t, "-"); add_two(&t, d); break;
        case RA_TIME: add_two(&t, h); add(&t, ":"); add_two(&t, mi); add(&t, ":"); add_two(&t, s); break;
        case RA_SINCE: add_number(&t, now); break;
        case RA_WAKEALARM: if (wakealarm_seconds) add_number(&t, wakealarm_seconds); break;
        case RA_MAX_FREQ: add(&t, "64"); break;
        case RA_HCTOSYS: add(&t, "1"); break;
        case RA_UEVENT: add(&t, "MAJOR=253\nMINOR=0\nDEVNAME=rtc0"); break;
        case RA_DEV: add(&t, "253:0"); break;
        case RA_WAKEUP: add(&t, "enabled"); break;   /* the CMOS alarm is a wakeup source (rtcwake checks this) */
        }
        if (a != RA_WAKEALARM || wakealarm_seconds) add(&t, "\n");
        break;
    }
    case CLASS_CPUFREQ: case CLASS_CPUIDLE: return cpufreq_sysfs_read(id, buffer, capacity);
    case CLASS_TICK: return tick_sysfs_read(instance, a, buffer, capacity);
    default: return -2;
    }
    return t.used;
}
static int parse_action(const char* text, unsigned* out, bool allow_interactive) {
    for (unsigned i = 0; i < 6; i++) if (word_is(text, action_names[i]) && (i != ACT_INTERACTIVE || allow_interactive)) { *out = i; return 0; }
    return -22;
}
int64_t power_sysfs_write(unsigned id, const char* text, unsigned length) {
    (void)length;
    unsigned c = id >> 12, instance = (id >> 6) & 63, a = id & 63;
    if (!session_owner_or_root()) return -13;
    if (c == CLASS_CPUFREQ || c == CLASS_CPUIDLE) return cpufreq_sysfs_write(id, text, length);
    uint64_t value;
    switch (c) {
    case CLASS_BACKLIGHT:
        if (instance >= power_devices.backlight_count || a != BA_BRIGHTNESS) return -22;
        if (!parse_number(text, &value) || value >= power_devices.backlights[instance].count) return -22;
        { int error = acpi_backlight_set(instance, (unsigned)value); if (error) return error; }
        backlight_uevent(instance, "sysfs"); return 0;
    case CLASS_COOLING: {
        unsigned zone = instance >> 1; if (zone >= power_devices.zone_count || a != CA_CUR || !(instance & 1)) return -22;
        unsigned count = power_devices.zones[zone].state.active_count;
        if (!parse_number(text, &value) || value > count) return -22;
        acpi_thermal_set_active(zone, value ? (int)(count - value) : -1); return 0;
    }
    case CLASS_RTC:
        if (a == RA_WAKEUP) return word_is(text, "enabled") ? 0 : word_is(text, "disabled") ? -95 : -22;   /* it always may wake */
        if (a != RA_WAKEALARM) return -22;
        if (text[0] == '+') { if (!parse_number(text + 1, &value)) return -22; if (wakealarm_seconds) return -16; value += rtc_hardware_now(); }
        else { if (!parse_number(text, &value)) return -22; if (value && wakealarm_seconds) return -16; }   /* Linux: EBUSY while armed */
        return wakealarm_set(value);
    case CLASS_POWER:
        switch (a) {
        case PA_STATE: {
            unsigned state = word_is(text, "mem") ? (pm_mem_is_deep() ? PM_STATE_MEM : PM_STATE_FREEZE) : word_is(text, "freeze") ? PM_STATE_FREEZE :
                             word_is(text, "disk") ? PM_STATE_DISK : word_is(text, "standby") ? 0 : 99;
            if (state == 99) return -22;
            if (!state) return -22;   /* standby (S1) is not offered */
            wakeup_count++;
            return pm_request(state);
        }
        case PA_MEM_SLEEP:
            if (word_is(text, "deep")) { if (!acpi_sleep_state_supported(3)) return -22; mem_sleep_deep = 1; return 0; }
            if (word_is(text, "s2idle")) { mem_sleep_deep = 0; return 0; }
            return -22;
        case PA_DISK:
            if (word_is(text, "platform")) disk_mode = 0; else if (word_is(text, "shutdown")) disk_mode = 1; else if (word_is(text, "reboot")) disk_mode = 2;
            else return -22;
            return 0;
        case PA_WAKEUP_COUNT: if (!parse_number(text, &value)) return -22; return value == wakeup_count ? 0 : -22;   /* Linux: EINVAL when events came in */
        case PA_IMAGE_SIZE: if (!parse_number(text, &value)) return -22; image_size_bytes = value; return 0;
        case PA_POWER_KEY: return key_policy_store(text, &policy_power_key);
        case PA_SLEEP_KEY: return key_policy_store(text, &policy_sleep_key);
        case PA_LID: return parse_action(text, &policy_lid, false);
        case PA_LID_AC: { int e = parse_action(text, &policy_lid_ac, false); if (!e) lid_ac_set = true; return e; }
        case PA_LID_DOCKED: return parse_action(text, &policy_lid_docked, false);
        case PA_CRITICAL: { unsigned v; int e = parse_action(text, &v, false); if (e) return e; if (v == ACT_SUSPEND || v == ACT_IGNORE) return -22; policy_critical = v; return 0; }
        case PA_LOW_PERCENT: if (!parse_number(text, &value) || value > 50 || value <= battery_critical_percent) return -22; battery_low_percent = (unsigned)value; return 0;
        case PA_CRITICAL_PERCENT: if (!parse_number(text, &value) || !value || value >= battery_low_percent) return -22; battery_critical_percent = (unsigned)value; return 0;
        case PA_SLEEP_DELAY: if (!parse_number(text, &value) || value > 10000) return -22; *pm_sleep_delay() = value; return 0;
        case PA_SLEEP_ACK: pm_sleep_ack(); return 0;   /* user space is ready (lock screen drawn): sleep now */
        case PA_RESUME: case PA_RESUME_OFFSET: {   /* where the next hibernation image goes: root only (Linux 0644 root) */
            struct credentials* who = credentials_current();
            if (who && !credentials_capable(who, NULL, 21)) return -1;
            if (a == PA_RESUME) return hibernate_resume_set(text);
            if (!parse_number(text, &value)) return -22;
            return hibernate_resume_offset_set(value);
        }
        case PA_PROFILE:
            for (unsigned p = 0; p < 3; p++) if (word_is(text, profile_names[p])) {
                int error = cpufreq_set_profile(p); if (error) return error;
                say("[64] POWER PROFILE "); say(profile_names[p]); say("\n");
                struct uevent u; uevent_begin(&u, "/kernel/power", "power"); uevent_field(&u, "POWER_PROFILE=", profile_names[p]); uevent_send(&u);
                return 0;
            }
            return -22;
        }
        return -22;
    }
    return -22;
}
static void node(const char* path, unsigned type, unsigned id, const char* target) {
    if (sysfs_add(path, type, id, target) < 0) { say("[64] power sysfs: cannot add "); say(path); say("\n"); }
}
static void directories(const char* path) {   /* create every missing parent directory of `path` (and itself) */
    char partial[112]; unsigned n = strlen(path); if (n >= sizeof(partial)) return;
    for (unsigned i = 5; i <= n; i++) if (path[i] == '/' || !path[i]) {
        memcpy(partial, path, i); partial[i] = 0;
        uint32_t found; if (sysfs_resolve(partial, false, &found)) node(partial, 0, 0, NULL);
    }
}
static void attribute(const char* directory, const char* name, unsigned id, bool writable) {
    char path[112]; strcpy(path, directory); strcat(path, "/"); strcat(path, name); node(path, writable ? 2 : 1, id, NULL);
}
void power_init(void) {
    char path[112], link[112];
    directories("/sys/class/power_supply");
    for (unsigned i = 0; i < power_devices.supply_count; i++) {
        const struct supply_state* s = &power_devices.supplies[i].state;
        supply_path(i, path, true); directories(path);
        static const unsigned ac_attributes[] = {SA_TYPE, SA_ONLINE, SA_UEVENT, SA_SCOPE};
        if (!s->battery) for (unsigned j = 0; j < 4; j++) attribute(path, battery_files[ac_attributes[j]], ID(CLASS_SUPPLY, i, ac_attributes[j]), false);
        else for (unsigned a = 0; a < SA_COUNT; a++) if (a != SA_ONLINE) attribute(path, supply_file(s, a), ID(CLASS_SUPPLY, i, a), false);
        strcpy(link, "/sys/class/power_supply/"); strcat(link, s->name); node(link, 3, 0, path);
    }
    directories("/sys/class/thermal"); directories("/sys/devices/virtual/thermal");
    for (unsigned i = 0; i < power_devices.zone_count; i++) {
        char number[4]; uint_to_str(i, number);
        strcpy(path, "/sys/devices/virtual/thermal/thermal_zone"); strcat(path, number); directories(path);
        static const char* const files[] = {"type", "temp", "mode", "policy", "available_policies", "uevent"};
        for (unsigned a = 0; a < 6; a++) attribute(path, files[a], ID(CLASS_ZONE, i, a), false);
        struct trip trips[3 + THERMAL_ACTIVE_MAX]; unsigned count = zone_trips(i, trips);
        for (unsigned trip = 0; trip < count && trip < 15; trip++) {
            static const char* const suffix[] = {"_temp", "_type", "_hyst"};
            for (unsigned k = 0; k < 3; k++) {
                char name[40] = "trip_point_", tn[4]; uint_to_str(trip, tn); strcat(name, tn); strcat(name, suffix[k]);
                attribute(path, name, ID(CLASS_ZONE, i, 16 + trip * 3 + k), false);
            }
        }
        strcpy(link, "/sys/class/thermal/thermal_zone"); strcat(link, number); node(link, 3, 0, path);
        for (unsigned fan = 0; fan < 2; fan++) {
            if (fan ? !power_devices.zones[i].state.active_count : !power_devices.zones[i].state.passive) continue;
            char cd[8]; uint_to_str(i * 2 + fan, cd);
            strcpy(path, "/sys/devices/virtual/thermal/cooling_device"); strcat(path, cd); directories(path);
            attribute(path, "type", ID(CLASS_COOLING, i * 2 + fan, CA_TYPE), false);
            attribute(path, "max_state", ID(CLASS_COOLING, i * 2 + fan, CA_MAX), false);
            attribute(path, "cur_state", ID(CLASS_COOLING, i * 2 + fan, CA_CUR), fan != 0);
            strcpy(link, "/sys/class/thermal/cooling_device"); strcat(link, cd); node(link, 3, 0, path);
        }
    }
    for (unsigned i = 0; i < power_devices.backlight_count; i++) {   /* Linux's ACPI video: acpi_video0, acpi_video1, ... */
        char number[4]; uint_to_str(i, number);
        directories("/sys/class/backlight"); strcpy(path, "/sys/devices/virtual/backlight/acpi_video"); strcat(path, number); directories(path);
        static const char* const files[] = {"brightness", "actual_brightness", "max_brightness", "type", "bl_power", "scale", "uevent"};
        for (unsigned a = 0; a < 7; a++) attribute(path, files[a], ID(CLASS_BACKLIGHT, i, a), a == BA_BRIGHTNESS);
        strcpy(link, "/sys/class/backlight/acpi_video"); strcat(link, number); node(link, 3, 0, path);
    }
    directories("/sys/power/policy");
    static const char* const power_files[] = {"state", "mem_sleep", "disk", "wakeup_count", "image_size"};
    for (unsigned a = 0; a < 5; a++) attribute("/sys/power", power_files[a], ID(CLASS_POWER, 0, a), true);
    attribute("/sys/power", "resume", ID(CLASS_POWER, 0, PA_RESUME), true);
    attribute("/sys/power", "resume_offset", ID(CLASS_POWER, 0, PA_RESUME_OFFSET), true);
    static const char* const policy_files[] = {"power_key", "sleep_key", "lid_switch", "critical_battery_action", "low_battery_percent", "critical_battery_percent",
                                               "sleep_delay_ms", "sleep_ack", "resume_count", "last_event", "profile", "profile_choices", "lid_state"};
    for (unsigned a = 0; a < 13; a++) attribute("/sys/power/policy", policy_files[a], ID(CLASS_POWER, 0, PA_POWER_KEY + a), a < 8 || a == 10);
    attribute("/sys/power/policy", "lid_switch_external_power", ID(CLASS_POWER, 0, PA_LID_AC), true);
    attribute("/sys/power/policy", "lid_switch_docked", ID(CLASS_POWER, 0, PA_LID_DOCKED), true);
    strcpy(path, "/sys/devices/pnp0/00:01/rtc/rtc0"); directories(path); directories("/sys/class/rtc");
    static const char* const rtc_files[] = {"name", "date", "time", "since_epoch", "wakealarm", "max_user_freq", "hctosys", "uevent", "dev"};
    for (unsigned a = 0; a < 9; a++) attribute(path, rtc_files[a], ID(CLASS_RTC, 0, a), a == RA_WAKEALARM);
    node("/sys/class/rtc/rtc0", 3, 0, path);
    directories("/sys/devices/pnp0/00:01/power"); attribute("/sys/devices/pnp0/00:01/power", "wakeup", ID(CLASS_RTC, 0, RA_WAKEUP), true);
    node("/sys/devices/pnp0/00:01/rtc/rtc0/device", 3, 0, "/sys/devices/pnp0/00:01");
    tick_init();   /* tickless idle: the boot CPU's event device (tick.c) */
    for (unsigned i = 0; i < cpu_count && i < 63; i++) {
        char number[4]; uint_to_str(i, number);
        strcpy(path, "/sys/devices/system/clockevents/clockevent"); strcat(path, number); directories(path);
        attribute(path, "current_device", ID(CLASS_TICK, i, 0), false);
    }
    directories("/sys/devices/system/clocksource/clocksource0");
    attribute("/sys/devices/system/clocksource/clocksource0", "current_clocksource", ID(CLASS_TICK, 0, 1), false);
    attribute("/sys/devices/system/clocksource/clocksource0", "available_clocksource", ID(CLASS_TICK, 0, 2), false);
    cpufreq_init();
    cpufreq_sysfs_init();
    pm_init();
    say("[64] POWER: ");
    say_number(power_devices.supply_count); say(" power supplies, "); say_number(power_devices.zone_count); say(" thermal zones, ");
    say_number(power_devices.backlight_count); say(" backlights; /sys/power/state:");
    if (acpi_sleep_state_supported(3)) say(" mem(S3)");
    say(" freeze"); if (pm_hibernation_available()) say(" disk");
    say("\n");
}
