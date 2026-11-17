/* ACPI device / power-management campaign program (/bin/power64, tests/test_x64_acpi.py and test_x64_suspend.py).
 *
 * Mode 'a': the Linux interfaces of the ACPI devices described by tests/acpi_test_ssdt.py: /sys/class/power_supply,
 *   /sys/class/thermal, /sys/class/backlight, /sys/power, /sys/class/rtc/rtc0, hostile stores, then a scripted
 *   sequence: for every step it prints "[64] POWER WAIT <step>", the harness changes the simulated hardware
 *   (vmgenid GUID over QMP, or system_powerdown) and the program waits for the kernel uevent / input event and the
 *   sysfs state that must follow, printing "[64] POWER STEP <step> PASS".
 * Mode 'w': ready, then wait for SIGTERM (critical battery / thermal shutdown runs: the kernel ends user space).
 * Mode 's': system sleep: writes /sys/power/state (argv[1] or "mem") N times (argv[2]) checking clocks, the uevent
 *   PM notifications, timers, files written before/after and the RTC wake alarm. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void fail(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("[64] POWER FAIL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); failures++;
}
static void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("[64] POWER "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); }
static char* read_text(const char* path) {
    static char buffer[4][4096]; static unsigned slot; char* out = buffer[slot++ % 4];
    int fd = open(path, O_RDONLY); if (fd < 0) { snprintf(out, 4096, "<open %s: %s>", path, strerror(errno)); return out; }
    ssize_t n = read(fd, out, 4095); close(fd);
    if (n < 0) { snprintf(out, 4096, "<read %s: %s>", path, strerror(errno)); return out; }
    out[n] = 0; while (n && out[n - 1] == '\n') out[--n] = 0;
    return out;
}
static int write_text(const char* path, const char* text) {
    int fd = open(path, O_WRONLY); if (fd < 0) return -errno;
    ssize_t n = write(fd, text, strlen(text)); int error = n < 0 ? -errno : 0; close(fd);
    return error;
}
static void expect(const char* path, const char* want) {
    const char* got = read_text(path);
    if (strcmp(got, want)) fail("%s = '%s', expected '%s'", path, got, want);
}
static long long number(const char* path) { return atoll(read_text(path)); }
static void expect_number(const char* path, long long want) { long long got = number(path); if (got != want) fail("%s = %lld, expected %lld", path, got, want); }
static void expect_store(const char* path, const char* text, int error) {
    int got = write_text(path, text); if (got != error) fail("store '%s' > %s: %d, expected %d", text, path, got, error);
}
static double seconds(clockid_t clock) { struct timespec t; clock_gettime(clock, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---- event sources ---- */
static int uevent_fd = -1, keys_fd = -1;
static void open_sources(void) {
    uevent_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (uevent_fd < 0 || bind(uevent_fd, (struct sockaddr*)&address, sizeof(address))) fail("uevent socket: %s", strerror(errno));
    keys_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (keys_fd < 0) fail("open /dev/input/event0: %s", strerror(errno));
    struct input_event e; while (keys_fd >= 0 && read(keys_fd, &e, sizeof(e)) > 0) {}
}
/* A uevent whose text contains every needle ("KEY=VALUE" or devpath fragments), within `timeout` seconds. */
static bool wait_uevent(double timeout, const char* first, const char* second) {
    double end = seconds(CLOCK_MONOTONIC) + timeout;
    while (seconds(CLOCK_MONOTONIC) < end) {
        struct pollfd p = {uevent_fd, POLLIN, 0};
        int ready = poll(&p, 1, (int)((end - seconds(CLOCK_MONOTONIC)) * 1000) + 1);
        if (ready <= 0) continue;
        char message[4096]; ssize_t n = recv(uevent_fd, message, sizeof(message) - 1, 0);
        if (n <= 0) continue;
        message[n] = 0;
        bool a = !first, b = !second;
        for (ssize_t at = 0; at < n; at += (ssize_t)strlen(message + at) + 1) {
            if (first && strstr(message + at, first)) a = true;
            if (second && strstr(message + at, second)) b = true;
        }
        if (a && b) return true;
    }
    return false;
}
static bool wait_key(double timeout, unsigned code) {
    double end = seconds(CLOCK_MONOTONIC) + timeout; bool down = false;
    while (seconds(CLOCK_MONOTONIC) < end) {
        struct pollfd p = {keys_fd, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        struct input_event e;
        while (read(keys_fd, &e, sizeof(e)) == sizeof(e)) {
            if (e.type == EV_KEY && e.code == code && e.value == 1) down = true;
            if (e.type == EV_KEY && e.code == code && e.value == 0 && down) return true;
        }
    }
    return false;
}
static bool wait_text(double timeout, const char* path, const char* want) {
    double end = seconds(CLOCK_MONOTONIC) + timeout;
    do { if (!strcmp(read_text(path), want)) return true; usleep(50000); } while (seconds(CLOCK_MONOTONIC) < end);
    return false;
}
static void step(const char* name) { say("WAIT %s", name); }
static void passed(const char* name) { say("STEP %s PASS", name); }

#define AC "/sys/class/power_supply/AC"
#define BAT "/sys/class/power_supply/BAT0"
#define TZ "/sys/class/thermal/thermal_zone0"
#define BL "/sys/class/backlight/acpi_video0"
#define CPUFREQ "/sys/devices/system/cpu/cpufreq/policy0"
#define CPUIDLE0 "/sys/devices/system/cpu/cpu0/cpuidle"
static void set_governor(const char* governor) {   /* every policy, as cpupower frequency-set does (one _PCT register here) */
    for (unsigned cpu = 0; cpu < 64; cpu++) {
        char path[96]; snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%u/scaling_governor", cpu);
        if (access(path, F_OK)) break;
        expect_store(path, governor, 0);
    }
}
static long long idle_usage(unsigned state) { char path[96]; snprintf(path, sizeof(path), CPUIDLE0 "/state%u/usage", state); return number(path); }
/* acpi-cpufreq on the test SSDT's _PSS (2400/1800/1200 MHz) and cpuidle on its _CST (C1 HLT, C2 SystemIO). */
static void cpu_states(void) {
    expect(CPUFREQ "/scaling_driver", "acpi-cpufreq"); expect("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver", "acpi-cpufreq");
    expect_number(CPUFREQ "/cpuinfo_max_freq", 2400000); expect_number(CPUFREQ "/cpuinfo_min_freq", 1200000);
    expect(CPUFREQ "/scaling_available_frequencies", "2400000 1800000 1200000");
    expect(CPUFREQ "/scaling_available_governors", "performance powersave schedutil"); expect(CPUFREQ "/scaling_governor", "schedutil");
    expect_number(CPUFREQ "/cpuinfo_transition_latency", 10000); expect_number(CPUFREQ "/bios_limit", 2400000);
    expect("/sys/devices/system/cpu/cpuidle/current_driver", "acpi_idle"); expect("/sys/devices/system/cpu/cpuidle/current_governor", "menu");
    expect(CPUIDLE0 "/state0/name", "POLL"); expect(CPUIDLE0 "/state1/name", "C1"); expect(CPUIDLE0 "/state1/desc", "ACPI HLT");
    expect(CPUIDLE0 "/state2/name", "C2"); expect(CPUIDLE0 "/state2/desc", "ACPI IOPORT"); expect_number(CPUIDLE0 "/state2/latency", 50);
    expect_number(CPUIDLE0 "/state2/residency", 100); expect_number(CPUIDLE0 "/state2/power", 300);
    long long before = idle_usage(2); usleep(300000);
    if (idle_usage(2) <= before) fail("C2 never entered (usage %lld)", idle_usage(2));
    expect_store(CPUIDLE0 "/state2/disable", "1", 0); expect(CPUIDLE0 "/state2/disable", "1");
    usleep(50000); before = idle_usage(2); long long c1 = idle_usage(1); usleep(300000);
    if (idle_usage(2) != before || idle_usage(1) <= c1) fail("disabled C2 still entered (%lld -> %lld), C1 %lld -> %lld", before, idle_usage(2), c1, idle_usage(1));
    expect_store(CPUIDLE0 "/state2/disable", "0", 0);
    expect_store(CPUFREQ "/scaling_max_freq", "1900000", 0); expect_number(CPUFREQ "/scaling_max_freq", 1900000);
    expect_store(CPUFREQ "/scaling_min_freq", "5000000", 0); expect_number(CPUFREQ "/scaling_min_freq", 2400000); expect_number(CPUFREQ "/scaling_max_freq", 2400000);
    expect_store(CPUFREQ "/scaling_min_freq", "0", 0); expect_number(CPUFREQ "/scaling_min_freq", 1200000);
    say("cpufreq/cpuidle sysfs checked");
}
static void initial_state(void) {
    expect(AC "/type", "Mains"); expect(AC "/online", "1");
    expect(BAT "/type", "Battery"); expect(BAT "/present", "1"); expect(BAT "/status", "Charging");
    expect(BAT "/technology", "Li-ion"); expect(BAT "/model_name", "NX-BAT-01"); expect(BAT "/serial_number", "SN0001");
    expect(BAT "/manufacturer", "NexusOS Test Cells"); expect_number(BAT "/cycle_count", 42);
    expect_number(BAT "/energy_full_design", 50000000); expect_number(BAT "/energy_full", 48000000); expect_number(BAT "/energy_now", 40000000);
    expect_number(BAT "/voltage_min_design", 11100000); expect_number(BAT "/voltage_now", 11800000); expect_number(BAT "/power_now", 5000000);
    expect_number(BAT "/capacity", 83); expect(BAT "/capacity_level", "Normal");
    if (!strstr(read_text(BAT "/uevent"), "POWER_SUPPLY_NAME=BAT0\nPOWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_STATUS=Charging")) fail("BAT0 uevent: %s", read_text(BAT "/uevent"));
    if (!strstr(read_text(AC "/uevent"), "POWER_SUPPLY_ONLINE=1")) fail("AC uevent: %s", read_text(AC "/uevent"));
    char link[256] = {0}; if (readlink(BAT, link, sizeof(link) - 1) < 0 || !strstr(link, "PNP0C0A")) fail("BAT0 class link '%s'", link);
    expect(TZ "/type", "acpitz"); expect_number(TZ "/temp", 35000); expect(TZ "/mode", "enabled");
    expect(TZ "/trip_point_0_type", "critical"); expect_number(TZ "/trip_point_0_temp", 100000);
    expect(TZ "/trip_point_1_type", "hot"); expect_number(TZ "/trip_point_1_temp", 90000);
    expect(TZ "/trip_point_2_type", "passive"); expect_number(TZ "/trip_point_2_temp", 80000);
    expect(TZ "/trip_point_3_type", "active"); expect_number(TZ "/trip_point_3_temp", 70000);
    expect("/sys/class/thermal/cooling_device0/type", "Processor"); expect("/sys/class/thermal/cooling_device1/type", "Fan");
    expect("/sys/class/thermal/cooling_device1/cur_state", "0"); expect("/sys/class/thermal/cooling_device1/max_state", "1");
    expect(BL "/max_brightness", "9"); expect(BL "/brightness", "6"); expect(BL "/actual_brightness", "6"); expect(BL "/type", "firmware");
    if (!strstr(read_text("/sys/power/state"), "freeze")) fail("/sys/power/state: %s", read_text("/sys/power/state"));
    expect("/sys/class/rtc/rtc0/name", "rtc_cmos");
    long long rtc = number("/sys/class/rtc/rtc0/since_epoch"); time_t now = time(NULL);
    if (llabs(rtc - (long long)now) > 2) fail("rtc0 since_epoch %lld vs time %lld", rtc, (long long)now);
    say("initial sysfs state checked");
}
static void hostile(void) {
    expect_store("/sys/power/state", "bogus", -EINVAL); expect_store("/sys/power/state", "standby", -EINVAL);
    expect_store("/sys/power/mem_sleep", "nonsense", -EINVAL); expect_store("/sys/power/disk", "\n", -EINVAL);
    expect_store("/sys/power/disk", "", 0);   /* a zero-length write never reaches the store, as on Linux */
    expect_store(BL "/brightness", "99", -EINVAL); expect_store(BL "/brightness", "-1", -EINVAL); expect_store(BL "/brightness", "3x", -EINVAL);
    expect_store(BL "/brightness", "18446744073709551616", -EINVAL);
    char big[5000]; memset(big, '7', sizeof(big) - 1); big[sizeof(big) - 1] = 0; expect_store(BL "/brightness", big, -EINVAL);
    expect_store("/sys/class/rtc/rtc0/wakealarm", "1", -EINVAL);           /* in the past */
    expect_store("/sys/class/rtc/rtc0/wakealarm", "+99999999", -EINVAL);   /* beyond the CMOS alarm range */
    char stale[32]; snprintf(stale, sizeof(stale), "%lld", number("/sys/power/wakeup_count") + 1);
    expect_store("/sys/power/wakeup_count", stale, -EINVAL);   /* a count that does not match: events came in */
    int rtc = open("/dev/rtc0", O_RDONLY);
    if (rtc < 0) fail("/dev/rtc0: errno %d", errno);
    struct { unsigned char enabled, pending, pad[2]; int tm[9]; } alarm = {1, 0, {0}, {0, 0, 12, 30, 1, 130, 0, 0, 0}};   /* 2030-02-30 */
    if (ioctl(rtc, 0x4028700F, &alarm) != -1 || errno != EINVAL) fail("RTC_WKALM_SET accepted February 30 (errno %d)", errno);
    close(rtc);
    expect_store(CPUFREQ "/scaling_governor", "turbo", -EINVAL); expect_store(CPUFREQ "/scaling_max_freq", "fast", -EINVAL);
    expect_store(CPUFREQ "/scaling_min_freq", "4294967296000", -EINVAL);
    expect_store(CPUFREQ "/energy_performance_preference", "power", -ENOENT);   /* acpi-cpufreq has no EPP file */
    expect_store(CPUIDLE0 "/state2/disable", "2", -EINVAL);
    if (open(CPUFREQ "/cpuinfo_max_freq", O_WRONLY) >= 0 || errno != EACCES) fail("cpuinfo_max_freq opened for writing (errno %d)", errno);
    expect_store("/sys/power/policy/power_key", "explode", -EINVAL);
    expect_store("/sys/power/policy/low_battery_percent", "90", -EINVAL);
    expect_store("/sys/power/policy/critical_battery_action", "suspend", -EINVAL);
    if (open(BAT "/capacity", O_WRONLY) >= 0 || errno != EACCES) fail("read-only attribute opened for writing (errno %d)", errno);
    if (open("/sys/class/thermal/cooling_device0/cur_state", O_WRONLY) >= 0) fail("processor cooling state writable");
    say("hostile: running as uid %u euid %u", (unsigned)getuid(), (unsigned)geteuid());
    pid_t child = fork();
    if (!child) {   /* an unprivileged process may not change power state or policy */
        /* root drops to nobody; the console user (uid 1000, allowed like logind's active seat) enters a user namespace */
        if (geteuid() == 0) { if (setgroups(0, NULL) || setresgid(65534, 65534, 65534) || setresuid(65534, 65534, 65534)) _exit(2); }
        else if (unshare(CLONE_NEWUSER)) _exit(3);
        int a = write_text("/sys/power/policy/power_key", "ignore"), b = write_text(BL "/brightness", "1"), c = write_text("/sys/power/state", "mem");
        int d = write_text(CPUFREQ "/scaling_governor", "performance"), e = write_text(CPUIDLE0 "/state1/disable", "1");
        _exit(a == -EACCES && b == -EACCES && c == -EACCES && d == -EACCES && e == -EACCES ? 0 : 1);
    }
    int status; waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) fail("unprivileged stores were not refused (status %x)", status);
    expect(BL "/brightness", "6");
    say("hostile stores refused");
}
static void scripted(void) {
    expect_store("/sys/power/policy/power_key", "ignore", 0); expect_store("/sys/power/policy/sleep_key", "ignore", 0);
    expect_store("/sys/power/policy/lid_switch", "ignore\n", 0); expect("/sys/power/policy/power_key", "ignore");
    expect_store(BL "/brightness", "3", 0); expect(BL "/brightness", "3"); expect(BL "/actual_brightness", "3");
    if (!wait_uevent(5, "SUBSYSTEM=backlight", "BRIGHTNESS=3")) fail("backlight uevent after a store");
    step("ac-offline");
    if (!wait_uevent(15, "ACPI0003", "POWER_SUPPLY_ONLINE=0")) fail("no AC offline uevent");
    if (!wait_text(5, AC "/online", "0") || !wait_text(5, BAT "/status", "Discharging")) fail("AC online %s, battery %s", read_text(AC "/online"), read_text(BAT "/status"));
    passed("ac-offline");
    step("battery-low");
    if (!wait_uevent(15, "POWER_SUPPLY_CAPACITY_LEVEL=Low", "POWER_SUPPLY_NAME=BAT0")) fail("no low-battery uevent");
    expect_number(BAT "/capacity", 8); expect(BAT "/capacity_level", "Low");
    passed("battery-low");
    step("battery-critical");
    if (!wait_uevent(15, "POWER_SUPPLY_CAPACITY_LEVEL=Critical", NULL)) fail("no critical-battery uevent");
    expect(BAT "/capacity_level", "Critical");
    passed("battery-critical");
    step("ac-online");
    if (!wait_uevent(15, "ACPI0003", "POWER_SUPPLY_ONLINE=1")) fail("no AC online uevent");
    if (!wait_text(5, BAT "/status", "Charging")) fail("battery not charging after AC returned: %s", read_text(BAT "/status"));
    passed("ac-online");
    step("lid-close");
    if (!wait_uevent(15, "LID_STATE=closed", NULL)) fail("no lid-closed uevent");
    passed("lid-close");
    step("lid-open");
    if (!wait_uevent(15, "LID_STATE=open", NULL)) fail("no lid-open uevent");
    passed("lid-open");
    step("sleep-button");
    if (!wait_key(15, KEY_SLEEP)) fail("no KEY_SLEEP");
    passed("sleep-button");
    step("power-button-method");
    if (!wait_key(15, KEY_POWER)) fail("no KEY_POWER (control method button)");
    passed("power-button-method");
    step("power-button-fixed");
    if (!wait_key(15, KEY_POWER)) fail("no KEY_POWER (fixed button)");
    passed("power-button-fixed");
    step("brightness-up");
    if (!wait_key(15, KEY_BRIGHTNESSUP)) fail("no KEY_BRIGHTNESSUP");
    if (!wait_text(5, BL "/actual_brightness", "4")) fail("brightness after hotkey up: %s", read_text(BL "/actual_brightness"));
    passed("brightness-up");
    step("brightness-down");
    if (!wait_key(15, KEY_BRIGHTNESSDOWN)) fail("no KEY_BRIGHTNESSDOWN");
    if (!wait_text(5, BL "/actual_brightness", "3")) fail("brightness after hotkey down: %s", read_text(BL "/actual_brightness"));
    passed("brightness-down");
    step("thermal-active");
    if (!wait_uevent(15, "SUBSYSTEM=thermal", "TEMP=72000")) fail("no 72 C thermal uevent");
    if (!wait_text(5, "/sys/class/thermal/cooling_device1/cur_state", "1")) fail("fan not on at 72 C");
    passed("thermal-active");
    step("thermal-passive");
    if (!wait_uevent(15, "SUBSYSTEM=thermal", "TEMP=82000")) fail("no 82 C thermal uevent");
    double end = seconds(CLOCK_MONOTONIC) + 10; while (number("/sys/class/thermal/cooling_device0/cur_state") == 0 && seconds(CLOCK_MONOTONIC) < end) usleep(100000);
    if (number("/sys/class/thermal/cooling_device0/cur_state") == 0) fail("no passive cooling at 82 C");
    passed("thermal-passive");
    step("thermal-normal");
    if (!wait_uevent(15, "SUBSYSTEM=thermal", "TEMP=40000")) fail("no 40 C thermal uevent");
    if (!wait_text(5, "/sys/class/thermal/cooling_device1/cur_state", "0")) fail("fan still on at 40 C");
    if (!wait_text(30, "/sys/class/thermal/cooling_device0/cur_state", "0")) fail("passive cap not released at 40 C");
    passed("thermal-normal");
    step("battery-removed");
    if (!wait_uevent(15, "POWER_SUPPLY_NAME=BAT0", "POWER_SUPPLY_PRESENT=0")) fail("no battery-removed uevent");
    expect(BAT "/present", "0"); expect(BAT "/status", "Unknown");
    passed("battery-removed");
    step("battery-inserted");
    if (!wait_uevent(15, "POWER_SUPPLY_NAME=BAT0", "POWER_SUPPLY_PRESENT=1")) fail("no battery-inserted uevent");
    expect(BAT "/present", "1"); expect(BAT "/model_name", "NX-BAT-01");
    passed("battery-inserted");
    /* acpi-cpufreq: the harness reads the _PCT register (0 = 2400, 1 = 1800, 2 = 1200 MHz) around each step */
    set_governor("performance");
    step("cpufreq-performance");   /* register 0; then _PPC = 1 and Notify(0x80) */
    if (!wait_text(15, CPUFREQ "/bios_limit", "1800000")) fail("_PPC change not applied: bios_limit %s", read_text(CPUFREQ "/bios_limit"));
    expect_number(CPUFREQ "/scaling_cur_freq", 1800000);
    passed("cpufreq-performance");
    usleep(1500000);   /* the harness reads the register (1800 MHz) after this PASS */
    set_governor("powersave"); expect_number(CPUFREQ "/scaling_cur_freq", 1200000);
    step("cpufreq-powersave");     /* register 2; then _PPC = 0 */
    if (!wait_text(15, CPUFREQ "/bios_limit", "2400000")) fail("_PPC back to 0 not applied: bios_limit %s", read_text(CPUFREQ "/bios_limit"));
    passed("cpufreq-powersave");
    set_governor("schedutil");
    usleep(500000);
    step("cpufreq-schedutil-busy");   /* every CPU busy for 2 s: schedutil raises them all to 2400 MHz (the _PCT port is shared) */
    long cpus = sysconf(_SC_NPROCESSORS_ONLN); pid_t spinners[16]; long started = 0;
    for (long i = 0; i < (cpus < 1 ? 1 : cpus > 16 ? 16 : cpus); i++) {
        pid_t pid = fork();
        if (!pid) { double end = seconds(CLOCK_MONOTONIC) + 2; volatile unsigned long spins = 0; while (seconds(CLOCK_MONOTONIC) < end) spins++; _exit(0); }
        if (pid > 0) spinners[started++] = pid;
    }
    for (long i = 0; i < started; i++) waitpid(spinners[i], NULL, 0);
    passed("cpufreq-schedutil-busy");
    step("cpufreq-schedutil-idle");   /* idle: back to 1200 MHz */
    sleep(3);
    passed("cpufreq-schedutil-idle");
}
static volatile sig_atomic_t terminated;
static void on_term(int number) { (void)number; terminated = 1; }

/* ---- system sleep cycles ---- */
static volatile sig_atomic_t alarmed;
static void on_alarm(int number) { (void)number; alarmed = 1; }
static long long meminfo(const char* key) {
    const char* text = read_text("/proc/meminfo"); const char* at = strstr(text, key);
    return at ? atoll(at + strlen(key)) : -1;
}
static bool write_marker(const char* path, unsigned cycle) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return false;
    char text[64]; int n = snprintf(text, sizeof(text), "cycle %u\n", cycle);
    bool ok = write(fd, text, (size_t)n) == n && fsync(fd) == 0; close(fd); return ok;
}
/* A TCP connection to the harness's echo server (10.0.2.2:8930 through QEMU user networking) that must survive the
 * sleeps: one line out, the same line back. */
static int echo_fd = -1;
static bool echo_line(const char* line, double timeout) {
    char want[64]; snprintf(want, sizeof(want), "%s\n", line);
    if (send(echo_fd, want, strlen(want), MSG_NOSIGNAL) != (ssize_t)strlen(want)) return false;
    char got[64]; size_t used = 0; double end = seconds(CLOCK_MONOTONIC) + timeout;
    while (used < strlen(want) && seconds(CLOCK_MONOTONIC) < end) {
        struct pollfd p = {echo_fd, POLLIN, 0};
        if (poll(&p, 1, 200) != 1) continue;
        ssize_t n = recv(echo_fd, got + used, strlen(want) - used, 0); if (n <= 0) return false;
        used += (size_t)n;
    }
    return used == strlen(want) && !memcmp(got, want, used);
}
static bool echo_connect(void) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(8930)};
    inet_pton(AF_INET, "10.0.2.2", &address.sin_addr);
    double end = seconds(CLOCK_MONOTONIC) + 40;
    while (seconds(CLOCK_MONOTONIC) < end) {   /* DHCP may still be running */
        echo_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (echo_fd >= 0 && !connect(echo_fd, (struct sockaddr*)&address, sizeof(address))) return echo_line("hello", 10);
        if (echo_fd >= 0) close(echo_fd);
        echo_fd = -1; usleep(500000);
    }
    return false;
}
static int run_audio(void) {   /* /bin/audio64 (tests/test_x64_audio.py's ALSA client) after the sleeps */
    pid_t child = fork();
    if (!child) { execl("/bin/audio64", "audio64", "1", (char*)NULL); _exit(127); }
    int status = 0; waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
/* virgl: S3 must be refused while a GPU resource exists (the host-side state cannot be saved); after the sleeps
 * /bin/gpu64 renders again, so the virtio-gpu came back. */
struct virtgpu_resource_create { uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, bo_handle, res_handle, size, stride; };
#define VIRTGPU_RESOURCE_CREATE (0xC0000000U | (uint32_t)sizeof(struct virtgpu_resource_create) << 16 | 'd' << 8 | 0x44)
static void gpu_veto(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { fail("open /dev/dri/renderD128: %s", strerror(errno)); return; }
    struct virtgpu_resource_create rc = {2, 1, 2, 64, 64, 1, 1, 0, 0, 0, 0, 0, 64 * 64 * 4, 64 * 4};
    if (ioctl(fd, VIRTGPU_RESOURCE_CREATE, &rc) || !rc.bo_handle) { fail("virtgpu RESOURCE_CREATE: %s", strerror(errno)); close(fd); return; }
    int error = write_text("/sys/power/state", "mem");
    if (error != -EBUSY) fail("S3 with a live virgl resource: %d (EBUSY expected)", error);
    else say("S3 refused while a virgl resource exists (EBUSY)");
    close(fd);
}
static int run_gpu(void) {
    pid_t child = fork();
    if (!child) { execl("/bin/gpu64", "gpu64", (char*)NULL); _exit(127); }
    int status = 0; waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
/* util-linux rtcwake (Debian's binary, tests/test_x64_suspend.py --only rtcwake): alarm through /dev/rtc0, then "mem". */
static int run_rtcwake(unsigned seconds) {
    char text[16]; snprintf(text, sizeof(text), "%u", seconds ? seconds : 3);
    pid_t child = fork();
    if (!child) { execl("/usr/sbin/rtcwake", "rtcwake", "-v", "-m", "mem", "-s", text, (char*)NULL); _exit(127); }
    if (child < 0) return -errno;
    int status; if (waitpid(child, &status, 0) < 0) return -errno;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { say("rtcwake exited with status %x", status); return -EIO; }
    return 0;
}
/* Memory under pressure across a hibernation into swap (" pressure=MB"): MB of pages with a per-page pattern, more
 * than RAM, so part of them is in swap when the image is written; all must read back intact after the restore. */
static uint64_t* pressure; static size_t pressure_pages; static bool pressure_random;   /* random: every word, incompressible */
static uint64_t pressure_word(size_t i, size_t k) {
    if (!pressure_random) return i * 0x9E3779B97F4A7C15ULL ^ k;
    uint64_t x = (i * 512 + k + 1) * 0x9E3779B97F4A7C15ULL; x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ULL; x ^= x >> 27; return x;
}
static void pressure_fill(unsigned megabytes) {
    pressure_pages = (size_t)megabytes * 256; pressure = mmap(NULL, pressure_pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pressure == MAP_FAILED) { fail("pressure mmap %u MiB: %s", megabytes, strerror(errno)); pressure = NULL; return; }
    size_t step = pressure_random ? 1 : 64;
    for (size_t i = 0; i < pressure_pages; i++) for (size_t k = 0; k < 512; k += step) pressure[i * 512 + k] = pressure_word(i, k);
    say("pressure: %u MiB written%s; swap: %s", megabytes, pressure_random ? " (random)" : "", read_text("/proc/swaps"));
}
static void pressure_check(const char* when) {
    if (!pressure) return;
    size_t bad = 0, step = pressure_random ? 1 : 64;
    for (size_t i = 0; i < pressure_pages; i++) for (size_t k = 0; k < 512; k += step) if (pressure[i * 512 + k] != pressure_word(i, k)) bad++;
    if (bad) fail("pressure: %zu words wrong %s", bad, when); else say("pressure: %zu pages intact %s", pressure_pages, when);
}
static int sleep_cycles(const char* arguments) {
    char state[16] = "mem"; unsigned count = 1, wake_seconds = 0;
    sscanf(arguments, "%15s %u %u", state, &count, &wake_seconds);
    bool net = strstr(arguments, " net") != NULL, usb = strstr(arguments, " usb") != NULL, audio = strstr(arguments, " audio") != NULL, gpu = strstr(arguments, " gpu") != NULL;
    bool button = strstr(arguments, " button") != NULL;   /* the host presses the power button as the system resumes */
    /* " swapon=PATH": activate that swap area first; " pressure=MB": see pressure_fill; " expect-error=N": the sleep
     * request must fail with errno N (and the system runs on) - lockdown (1), a swap area without room (28) */
    const char* swap = strstr(arguments, " swapon="), *load = strstr(arguments, " pressure="), *expected = strstr(arguments, " expect-error=");
    int expect_error = expected ? atoi(expected + 14) : 0;
    open_sources();
    if (swap) {
        char path[64]; sscanf(swap + 8, "%63s", path);
        if (syscall(SYS_swapon, path, 0)) fail("swapon %s: %s", path, strerror(errno)); else say("swapon %s: %s", path, read_text("/proc/swaps"));
    }
    if (strstr(arguments, " pressure-random=")) { pressure_random = true; pressure_fill((unsigned)atoi(strstr(arguments, " pressure-random=") + 17)); }
    else if (load) pressure_fill((unsigned)atoi(load + 10));
    say("resume device %s offset %s", read_text("/sys/power/resume"), read_text("/sys/power/resume_offset"));
    if (net) { if (!echo_connect()) fail("no TCP connection to the host echo server"); else say("TCP connection to the host established"); }
    say("sleep test: state=%s cycles=%u rtc=%u; /sys/power/state='%s' mem_sleep='%s'", state, count, wake_seconds, read_text("/sys/power/state"), read_text("/sys/power/mem_sleep"));
    expect_store("/sys/power/policy/power_key", "ignore", 0);
    if (!strcmp(state, "swsusp")) expect_store("/sys/power/disk", "reboot", 0);
    if (gpu) gpu_veto();
    long long free_before = -1;
    for (unsigned cycle = 1; cycle <= count && !failures; cycle++) {
        if (cycle == (count + 1) / 2 + 1) free_before = meminfo("MemAvailable:");   /* past the one-time allocations (netlink ring slots, caches) */
        char path[96]; snprintf(path, sizeof(path), "/home/nexus/pm-before-%u", cycle);
        if (!write_marker(path, cycle)) fail("write %s: %s", path, strerror(errno));
        alarmed = 0; signal(SIGALRM, on_alarm);
        struct itimerval interval = {{0, 0}, {0, 300000}}; setitimer(ITIMER_REAL, &interval, NULL);   /* fires 300 ms of uptime later */
        if (wake_seconds && strcmp(state, "rtcwake")) { char when[32]; snprintf(when, sizeof(when), "+%u", wake_seconds); expect_store("/sys/class/rtc/rtc0/wakealarm", when, 0); }
        double mono = seconds(CLOCK_MONOTONIC), boot = seconds(CLOCK_BOOTTIME), real = seconds(CLOCK_REALTIME);
        say("WAIT sleep-%u", cycle);
        /* "swsusp": reboot(2) LINUX_REBOOT_CMD_SW_SUSPEND with /sys/power/disk "reboot" (image saved, machine reset);
         * "rtcwake": util-linux rtcwake -m mem -s N (/dev/rtc0 RTC_WKALM_SET, then /sys/power/state) */
        int error = !strcmp(state, "rtcwake") ? run_rtcwake(wake_seconds)
                  : strcmp(state, "swsusp") ? write_text("/sys/power/state", state)
                  : syscall(SYS_reboot, 0xfee1deadL, 672274793L, 0xD000FCE2L, 0L) ? -errno : 0;
        double mono2 = seconds(CLOCK_MONOTONIC), boot2 = seconds(CLOCK_BOOTTIME), real2 = seconds(CLOCK_REALTIME);
        if (expect_error) {
            if (error != -expect_error) fail("cycle %u: %s gave %d, expected -%d", cycle, state, error, expect_error);
            else say("cycle %u: %s refused as expected (%s); the system runs on", cycle, state, strerror(expect_error));
            setitimer(ITIMER_REAL, &(struct itimerval){{0, 0}, {0, 0}}, NULL);
            pressure_check("after the refused hibernation");
            say("SLEEP CYCLE %u PASS", cycle); continue;
        }
        if (error) { fail("cycle %u: write %s to /sys/power/state: %d (%s)", cycle, state, error, strerror(-error)); break; }
        pressure_check("after the resume");
        double slept = (boot2 - boot) - (mono2 - mono);
        say("cycle %u resumed: monotonic +%.3f s, boottime +%.3f s, realtime +%.3f s, suspended %.3f s", cycle, mono2 - mono, boot2 - boot, real2 - real, slept);
        if (mono2 < mono || boot2 < boot) fail("cycle %u: clock went backwards", cycle);
        if (strcmp(state, "freeze") && (slept < 0.5 || real2 - real < slept - 1.5)) fail("cycle %u: suspended time not accounted (boottime %.3f, realtime %.3f)", cycle, boot2 - boot, real2 - real);
        for (unsigned i = 0; i < 200 && !alarmed; i++) usleep(10000);
        if (!alarmed) fail("cycle %u: the ITIMER_REAL armed before the sleep did not fire after resume", cycle);
        if (!wait_uevent(5, "PM_EVENT=post-", NULL)) fail("cycle %u: no post-suspend uevent", cycle);
        if (button && !wait_key(10, 143)) fail("cycle %u: the waking power-button press did not come as KEY_WAKEUP", cycle);
        if (strcmp(read_text(path), "cycle") == 0) {}
        char text[32]; snprintf(text, sizeof(text), "cycle %u", cycle);
        if (strcmp(read_text(path), text)) fail("cycle %u: %s lost: '%s'", cycle, path, read_text(path));
        snprintf(path, sizeof(path), "/home/nexus/pm-after-%u", cycle);
        if (!write_marker(path, cycle)) fail("write %s after resume: %s", path, strerror(errno));
        if (wake_seconds && read_text("/sys/class/rtc/rtc0/wakealarm")[0]) fail("cycle %u: wake alarm still armed after the wake", cycle);
        if (net && echo_fd >= 0) {
            char line[32]; snprintf(line, sizeof(line), "cycle %u", cycle);
            if (!echo_line(line, 20)) fail("cycle %u: the TCP connection to the host did not survive the sleep", cycle);
            else say("cycle %u: TCP connection to the host still works", cycle);
        }
        if (usb) {
            say("WAIT usb-key-%u", cycle);
            if (!wait_key(20, KEY_A)) fail("cycle %u: no key from the USB keyboard after resume", cycle);
            else say("cycle %u: USB keyboard works", cycle);
        }
        say("SLEEP CYCLE %u PASS", cycle);
    }
    if (count > 2 && !failures) {   /* no kernel memory may stay behind per cycle */
        long long free_after = meminfo("MemAvailable:");
        say("memory: MemAvailable %lld kB after cycle %u, %lld kB after cycle %u", free_before, (count + 1) / 2, free_after, count);
        if (free_before - free_after > 16) fail("%lld kB less available memory after %u more cycles", free_before - free_after, count - (count + 1) / 2);
    }
    if (audio && !failures) {
        int status = run_audio();
        if (status) fail("audio after resume: /bin/audio64 exited %d", status); else say("audio played after resume");
    }
    if (gpu && !failures) {
        int status = run_gpu();
        if (status) fail("virgl after resume: /bin/gpu64 exited %d", status); else say("virgl rendering works after resume");
    }
    if (failures) { say("SLEEP FAILED (%d)", failures); return 1; }
    say("SLEEP ALL PASSED %u cycles", count);
    return 0;
}
static char config[256];
int main(int argc, char** argv) {
    /* The campaign file /nexus/test-power: "<mode>[ <state> <count>]" (the kernel passes the mode as argv[1]). */
    int fd = open("/nexus/test-power", O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, config, sizeof(config) - 1); if (n > 0) config[n] = 0; close(fd); }
    char mode = config[0] ? config[0] : argc > 1 ? argv[1][0] : 'a';
    signal(SIGTERM, on_term);
    if (mode == 's') return sleep_cycles(config[1] ? config + 2 : "mem");
    if (mode == 'L') {   /* lid policies (tests/test_x64_acpi.py lid-policy): closing on AC is ignored, then like 'w' */
        expect("/sys/power/policy/lid_switch_external_power", "suspend"); expect("/sys/power/policy/lid_switch_docked", "ignore");
        expect_store("/sys/power/policy/lid_switch_docked", "bogus\n", -EINVAL); expect_store("/sys/power/policy/lid_switch_external_power", "interactive\n", -EINVAL);
        expect_store("/sys/power/policy/lid_switch_external_power", "ignore\n", 0); expect("/sys/power/policy/lid_switch_external_power", "ignore");
        expect("/sys/power/policy/lid_switch", "suspend");
        if (failures) say("LID POLICY FAILED (%d)", failures); else say("LID POLICY READY");
        while (!terminated) pause();
        return failures ? 1 : 143;
    }
    if (mode == 'w') {
        say("READY waiting for SIGTERM");
        while (!terminated) pause();
        say("SIGTERM received, exiting");
        return 143;
    }
    open_sources();
    initial_state();
    cpu_states();
    hostile();
    scripted();
    if (failures) { say("ACPI DEVICES FAILED (%d)", failures); return 1; }
    say("ACPI DEVICES ALL PASSED");
    expect_store("/sys/power/policy/power_key", "poweroff", 0);
    step("final-power-button");
    while (!terminated) pause();
    say("SIGTERM received after the power button, exiting");
    return 143;
}
