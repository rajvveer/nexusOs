/* CPU performance and idle states: Linux's cpufreq and cpuidle, with their sysfs files.
 *
 * Performance driver (the first that applies), one policy per CPU:
 *   intel_pstate    Intel HWP (CPUID.06H:EAX bit 7): IA32_PM_ENABLE 0x770, IA32_HWP_CAPABILITIES 0x771 (highest |
 *                   guaranteed << 8 | efficient << 16 | lowest << 24), IA32_HWP_REQUEST 0x774 (min | max << 8 | desired << 16
 *                   | EPP << 24); 100 MHz per performance unit. Active mode: governors performance and powersave.
 *   amd-pstate-epp  AMD CPPC in MSRs (CPUID 0x80000008 EBX bit 27: Zen 2 and later, e.g. the Ryzen 7 9700X): CPPC_CAP1
 *                   0xC00102B0 (lowest | lowest nonlinear << 8 | nominal << 16 | highest << 24), CPPC_ENABLE 0xC00102B1,
 *                   CPPC_REQ 0xC00102B3 (max | min << 8 | desired << 16 | EPP << 24); the nominal frequency from _PSS P0
 *                   or the P0 definition MSR 0xC0010064. Active mode: performance and powersave.
 *   acpi-cpufreq    _PSS/_PCT/_PPC of the processor objects; control through FFixedHW (Intel IA32_PERF_CTL 0x199, AMD
 *                   P-state control 0xC0010062) or SystemIO; performance, powersave and schedutil (every 10 ms:
 *                   target = 1.25 x max x busy fraction, the slowest state at or above it).
 * Limits: scaling_min/max_freq, _PPC (Notify 0x80 re-reads it), the thermal passive cap (power.c). EPP:
 * energy_performance_preference (default, performance, balance_performance, balance_power, power or 0-255).
 * Model-specific registers are written on their own CPU: a request is posted and that CPU applies it from its next
 * timer/wake interrupt or idle entry (cpufreq_tick).
 *
 * Idle (cpuidle, governor "menu"): state0 POLL, state1 C1 (HLT; MWAIT when _CST gives a FFixedHW C1), deeper states
 * from _CST: FFixedHW MWAIT hints (CPUID MONITOR required) or SystemIO P_LVLx reads followed by HLT (which returns at
 * once on hardware that entered the state, and halts on hypervisors that ignore the read). The deepest enabled
 * state whose target residency (2 x latency) fits the predicted idle time (average of recent idle periods) is
 * entered. Per state: usage, time, above, below; the disable files switch states off.
 *
 * Every CPUID, MSR, I/O and ACPI access goes through `struct cpufreq_platform`, so tests/cpufreq_host_test.c runs these
 * drivers on simulated processors. Without a performance driver no cpufreq directory exists (as on Linux). */
#ifndef CPUFREQ_HOST_TEST
#include "native.h"
#include "power.h"
#include "gpu.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#else
#include "cpufreq_host.h"
#endif

#define POLICIES 64
#define PSTATES 16
#define IDLE_STATES 6
#define ID(c, instance, attribute) ((unsigned)(c) << 12 | (unsigned)(instance) << 6 | (unsigned)(attribute))
#define CLASS_CPUFREQ 7U
#define CLASS_CPUIDLE 8U
enum { DRIVER_NONE, DRIVER_HWP, DRIVER_CPPC, DRIVER_ACPI };
enum { GOV_PERFORMANCE, GOV_POWERSAVE, GOV_SCHEDUTIL };
static const char* const driver_names[] = {"none", "intel_pstate", "amd-pstate-epp", "acpi-cpufreq"};
static const char* const governor_names[] = {"performance", "powersave", "schedutil"};
#define MSR_PERF_CTL 0x199U
#define MSR_PM_ENABLE 0x770U
#define MSR_HWP_CAPABILITIES 0x771U
#define MSR_HWP_REQUEST 0x774U
#define MSR_AMD_PSTATE_CTL 0xC0010062U
#define MSR_AMD_PSTATE_DEF 0xC0010064U
#define MSR_AMD_CPPC_CAP1 0xC00102B0U
#define MSR_AMD_CPPC_ENABLE 0xC00102B1U
#define MSR_AMD_CPPC_REQ 0xC00102B3U

struct cpufreq_platform {
    void (*cpuid)(uint32_t leaf, uint32_t r[4]);
    bool (*rdmsr)(uint32_t msr, uint64_t* value);            /* on the calling CPU */
    void (*wrmsr)(uint32_t msr, uint64_t value);
    uint32_t (*io_read)(uint16_t port, unsigned bits);
    void (*io_write)(uint16_t port, uint32_t value, unsigned bits);
    unsigned (*cpus)(void);
    unsigned (*self)(void);
    void (*kick)(unsigned cpu);                              /* make `cpu` run cpufreq_tick soon */
    uint64_t (*now_ns)(void);
    int (*pss)(unsigned cpu, struct acpi_pstate* out, unsigned capacity);
    int (*pct)(unsigned cpu, struct acpi_register* control, struct acpi_register* status);
    int (*ppc)(unsigned cpu);
    int (*cst)(unsigned cpu, struct acpi_cstate* out, unsigned capacity);
};
static const struct cpufreq_platform* platform;

struct policy {
    bool online;
    uint32_t lowest, nonlinear, nominal, highest;            /* HWP/CPPC performance levels */
    uint32_t unit_khz;                                       /* kHz per performance level */
    struct acpi_pstate states[PSTATES]; unsigned state_count; int ppc;
    struct acpi_register control, status;
    uint32_t min_khz, max_khz;                               /* scaling_min/max_freq */
    unsigned governor; int epp;                              /* -1: the driver's default */
    uint32_t cur_khz; unsigned cur_state;
    uint64_t posted; uint32_t applied;                      /* generation << 32 | value for this CPU's own MSR/I-O write */
    bool enabled;                                            /* HWP/CPPC switched on by this CPU itself */
    uint64_t idle_ns, busy, window_at;                       /* schedutil: idle time, busy fraction (0..1024) */
};
static struct policy policies[POLICIES];
static unsigned driver, cpu_total, thermal_percent = 100;
static bool fixed_hw_amd;                                   /* FFixedHW _PCT on AMD: P-state MSRs (else IA32_PERF_CTL) */

static void say(const char* s) { vga_print(s); }
static void say_number(uint64_t value) { char t[24]; number64(value, t); vga_print(t); }

/* ---- frequencies ---- */
static uint32_t level_khz(const struct policy* p, uint32_t level) { return level * p->unit_khz; }
static uint32_t info_min(const struct policy* p) { return driver == DRIVER_ACPI ? p->states[p->state_count - 1].mhz * 1000 : level_khz(p, p->lowest); }
static uint32_t info_max(const struct policy* p) { return driver == DRIVER_ACPI ? p->states[0].mhz * 1000 : level_khz(p, p->highest); }
/* The ceiling right now: the user's maximum, _PPC and the thermal cap (never under the hardware or user minimum). */
static uint32_t ceiling_khz(const struct policy* p) {
    uint32_t top = p->max_khz;
    if (driver == DRIVER_ACPI && p->ppc > 0 && (unsigned)p->ppc < p->state_count && p->states[p->ppc].mhz * 1000 < top) top = p->states[p->ppc].mhz * 1000;
    uint64_t capped = (uint64_t)info_max(p) * thermal_percent / 100;
    if (capped < top) top = (uint32_t)capped;
    if (top < info_min(p)) top = info_min(p);
    return top;
}
static uint32_t khz_level(const struct policy* p, uint32_t khz) {   /* rounded down, within the hardware range */
    uint32_t level = p->unit_khz ? khz / p->unit_khz : p->highest;
    if (level < p->lowest) level = p->lowest;
    if (level > p->highest) level = p->highest;
    return level;
}

/* ---- requests: written by the CPU they belong to ---- */
static void post(unsigned cpu, uint64_t value) {
    struct policy* p = &policies[cpu];
    /* one 64-bit compare-exchange: racing posters (sysfs on one CPU, a tick on another) each get their own generation,
     * so the owner never skips the later value as already applied */
    uint64_t old = __atomic_load_n(&p->posted, __ATOMIC_ACQUIRE), next;
    do next = ((old >> 32) + 1) << 32 | (uint32_t)value;
    while (!__atomic_compare_exchange_n(&p->posted, &old, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    if (cpu == platform->self()) {   /* not interrupted by this CPU's own tick applying a newer request meanwhile */
#ifndef CPUFREQ_HOST_TEST
        uint64_t flags; __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        cpufreq_tick();
        if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
#else
        cpufreq_tick();
#endif
    } else platform->kick(cpu);
}
static void write_register(const struct acpi_register* r, uint64_t value) {
    if (r->space == 1) platform->io_write((uint16_t)r->address, (uint32_t)value, r->bit_width ? r->bit_width : 8);
    else if (r->space == 0x7F) platform->wrmsr(fixed_hw_amd ? MSR_AMD_PSTATE_CTL : MSR_PERF_CTL, value);
}
static void schedutil_sample(unsigned cpu, uint64_t now);
static void apply(unsigned cpu);
void cpufreq_tick(void) {
    if (!platform || driver == DRIVER_NONE) return;
    unsigned cpu = platform->self(); if (cpu >= POLICIES) return;
    struct policy* p = &policies[cpu];
    if (driver == DRIVER_ACPI) schedutil_sample(cpu, platform->now_ns());   /* every 10 ms, on the CPU itself (its tick) */
    if ((driver == DRIVER_HWP || driver == DRIVER_CPPC) && p->online && !p->enabled) {
        /* first tick here: HWP/CPPC are enabled per logical CPU, and hybrid parts report their own capabilities */
        p->enabled = true;
        platform->wrmsr(driver == DRIVER_HWP ? MSR_PM_ENABLE : MSR_AMD_CPPC_ENABLE, 1);
        uint64_t caps;
        if (platform->rdmsr(driver == DRIVER_HWP ? MSR_HWP_CAPABILITIES : MSR_AMD_CPPC_CAP1, &caps)) {
            uint32_t highest = driver == DRIVER_HWP ? caps & 0xFF : (caps >> 24) & 0xFF, lowest = driver == DRIVER_HWP ? (caps >> 24) & 0xFF : caps & 0xFF;
            if (highest && lowest && lowest <= highest && (highest != p->highest || lowest != p->lowest)) {
                p->highest = highest; p->lowest = lowest;   /* this CPU's own range (before any user limit could apply) */
                p->max_khz = level_khz(p, highest); p->min_khz = level_khz(p, lowest);
                apply(cpu);
            }
        }
    }
    uint64_t posted = __atomic_load_n(&p->posted, __ATOMIC_ACQUIRE); uint32_t request = (uint32_t)posted;
    if ((uint32_t)(posted >> 32) == p->applied) return;
    p->applied = (uint32_t)(posted >> 32);
    if (driver == DRIVER_HWP) platform->wrmsr(MSR_HWP_REQUEST, request);
    else if (driver == DRIVER_CPPC) platform->wrmsr(MSR_AMD_CPPC_REQ, request);
    else write_register(&p->control, request);
}

/* ---- governors ---- */
static void apply(unsigned cpu) {
    struct policy* p = &policies[cpu];
    if (!p->online) return;
    uint32_t top = ceiling_khz(p), bottom = p->min_khz > top ? top : p->min_khz;
    if (driver == DRIVER_HWP || driver == DRIVER_CPPC) {
        uint32_t max = khz_level(p, top), min = p->governor == GOV_PERFORMANCE ? max : khz_level(p, bottom);
        if (min > max) min = max;
        uint32_t epp = (uint32_t)(p->governor == GOV_PERFORMANCE ? 0 : p->epp >= 0 ? p->epp : 0x80);   /* 0x80: balance_performance */
        uint64_t value = driver == DRIVER_HWP ? (uint64_t)min | (uint64_t)max << 8 | (uint64_t)epp << 24
                                              : (uint64_t)max | (uint64_t)min << 8 | (uint64_t)epp << 24;
        p->cur_khz = level_khz(p, p->governor == GOV_PERFORMANCE ? max : (min + max) / 2);   /* autonomous: an estimate */
        post(cpu, value);
        return;
    }
    /* acpi-cpufreq: states run from the fastest (0) to the slowest */
    uint32_t wanted = p->governor == GOV_PERFORMANCE ? top : bottom;
    if (p->governor == GOV_SCHEDUTIL) {
        uint64_t target = (uint64_t)info_max(p) * 5 / 4 * p->busy / 1024;
        wanted = target > top ? top : target < bottom ? bottom : (uint32_t)target;
    }
    unsigned state = 0;
    for (unsigned i = 0; i < p->state_count; i++) if (p->states[i].mhz * 1000 >= wanted) state = i;   /* the slowest at or above */
    while (state + 1 < p->state_count && p->states[state].mhz * 1000 > top) state++;
    if (state == p->cur_state) return;
    p->cur_state = state; p->cur_khz = p->states[state].mhz * 1000;
    post(cpu, p->states[state].control);
}
void cpufreq_thermal_cap(unsigned percent) {
    thermal_percent = percent > 100 ? 100 : percent < 1 ? 1 : percent;
    for (unsigned i = 0; i < cpu_total; i++) apply(i);
}
void cpufreq_limits_changed(unsigned cpu) {   /* Notify 0x80 (ACPI worker): _PPC moved */
    if (cpu >= cpu_total || driver != DRIVER_ACPI || !policies[cpu].online) return;
    policies[cpu].ppc = platform->ppc(cpu);
    say("[64] CPUFREQ: cpu"); say_number(cpu); say(" _PPC="); say_number((uint64_t)policies[cpu].ppc); say("\n");
    apply(cpu);
}

/* ---- idle (cpuidle) ---- */
enum { IDLE_POLL, IDLE_HLT, IDLE_MWAIT, IDLE_IO };
struct idle_state { char name[8], desc[24]; unsigned kind; uint32_t hint, latency_us, residency_us, power_mw; uint16_t port; };
static struct idle_state idle[IDLE_STATES];
static unsigned idle_count;
struct idle_cpu { uint64_t usage[IDLE_STATES], time_us[IDLE_STATES], above[IDLE_STATES], below[IDLE_STATES], predicted_ns; bool disabled[IDLE_STATES]; };
static struct idle_cpu idle_cpus[POLICIES];
static bool has_mwait(void) { uint32_t r[4]; platform->cpuid(1, r); return (r[2] >> 3) & 1; }
static void idle_setup(void) {
    struct idle_state states[IDLE_STATES]; unsigned count = 2;
    states[0] = (struct idle_state){"POLL", "CPUIDLE CORE POLL IDLE", IDLE_POLL, 0, 0, 0, 0, 0};
    states[1] = (struct idle_state){"C1", "ACPI HLT", IDLE_HLT, 0, 1, 1, 0, 0};
    struct acpi_cstate c[8]; int n = platform->cst ? platform->cst(0, c, 8) : -1;
    bool mwait = has_mwait();
    for (int i = 0; i < n && count < IDLE_STATES; i++) {
        bool ffh = c[i].reg.space == 0x7F && c[i].reg.bit_offset == 2;   /* FFixedHW class 2: native C-state, address = MWAIT hint */
        if (c[i].type <= 1) {   /* C1 replaces the default */
            if (ffh && mwait) { states[1].kind = IDLE_MWAIT; states[1].hint = (uint32_t)c[i].reg.address; strcpy(states[1].desc, "ACPI FFH MWAIT C1"); }
            states[1].latency_us = c[i].latency_us ? c[i].latency_us : 1; states[1].residency_us = states[1].latency_us; states[1].power_mw = c[i].power_mw;
            continue;
        }
        struct idle_state s = {"C", "", IDLE_HLT, 0, c[i].latency_us, c[i].latency_us * 2, c[i].power_mw, 0};
        if (ffh && mwait) { s.kind = IDLE_MWAIT; s.hint = (uint32_t)c[i].reg.address; strcpy(s.desc, "ACPI FFH MWAIT"); }
        else if (c[i].reg.space == 1 && c[i].type == 2) { s.kind = IDLE_IO; s.port = (uint16_t)c[i].reg.address; strcpy(s.desc, "ACPI IOPORT"); }
        else continue;   /* C3 through I/O needs bus-master arbitration control: not used */
        s.name[1] = (char)('0' + count); s.name[2] = 0;
        states[count++] = s;
    }
    memcpy(idle, states, sizeof(states)); __atomic_store_n(&idle_count, count, __ATOMIC_RELEASE);
}
void cpufreq_cstates_changed(unsigned cpu) { if (!cpu && platform) idle_setup(); }
#ifndef CPUFREQ_HOST_TEST
void cpu_idle_enter(void) {
    struct cpu* c = cpu_self(); unsigned index = c->index < POLICIES ? c->index : POLICIES - 1;
    cpufreq_tick();
    struct idle_cpu* d = &idle_cpus[index];
    unsigned count = __atomic_load_n(&idle_count, __ATOMIC_ACQUIRE), state = 0;
    if (count < 2) { __asm__ volatile("sti; hlt; cli" ::: "memory"); return; }   /* before cpufreq_init */
    /* tickless idle (tick.c): maybe stop this CPU's tick; the expected sleep is the earlier of the next timer event
     * and the recent idle periods (Linux's menu governor) */
    uint64_t next_event = tick_idle_enter(d->predicted_ns), expected = next_event < d->predicted_ns ? next_event : d->predicted_ns;
    for (unsigned i = count; i-- > 1;)
        if (!d->disabled[i] && (i == 1 || (uint64_t)idle[i].residency_us * 1000 <= expected)) { state = i; break; }
    if (state > 1 && !tick_deep_idle_allowed()) state = 1;
    uint64_t start = native_clock_ns();
    do switch (idle[state].kind) {   /* again while tick.c re-armed an early timer event */
    case IDLE_POLL: __asm__ volatile("sti; pause; pause; pause; pause; cli" ::: "memory"); break;
    case IDLE_MWAIT:
        __asm__ volatile("monitor" :: "a"(&c->wake_pending), "c"(0), "d"(0) : "memory");
        if (!c->wake_pending) __asm__ volatile("sti; mwait; cli" :: "a"(idle[state].hint), "c"(0) : "memory");
        else __asm__ volatile("sti; nop; cli" ::: "memory");
        break;
    case IDLE_IO: port_byte_in(idle[state].port); __asm__ volatile("sti; hlt; cli" ::: "memory"); break;
    default: __asm__ volatile("sti; hlt; cli" ::: "memory"); break;
    } while (tick_idle_exit());
    uint64_t slept = native_clock_ns() - start;
    d->usage[state]++; d->time_us[state] += slept / 1000;
    if (state > 1 && slept < (uint64_t)idle[state].residency_us * 1000) d->above[state]++;
    for (unsigned i = state + 1; i < count; i++) if (!d->disabled[i] && slept >= (uint64_t)idle[i].residency_us * 1000) { d->below[state]++; break; }
    d->predicted_ns = (d->predicted_ns * 7 + slept) / 8;
    policies[index].idle_ns += slept;
}
#endif

/* ---- schedutil: each CPU samples its own busy fraction from its timer tick (cpufreq_tick) ---- */
static void schedutil_sample(unsigned cpu, uint64_t now) {
    struct policy* p = &policies[cpu];
    if (!p->online || p->governor != GOV_SCHEDUTIL || now - p->window_at < 10000000ULL) return;
    uint64_t window = now - p->window_at, idle_ns = p->idle_ns; p->idle_ns = 0; p->window_at = now;
    if (idle_ns > window) idle_ns = window;
    p->busy = (window - idle_ns) * 1024 / window;
    apply(cpu);
}
void cpufreq_poll(void) {   /* every policy at once (tests/cpufreq_host_test.c drives the samples this way) */
    if (!platform || driver != DRIVER_ACPI) return;
    uint64_t now = platform->now_ns();
    for (unsigned i = 0; i < cpu_total; i++) schedutil_sample(i, now);
}

/* ---- power profiles (/sys/power/policy/profile, power.c; nexus-upowerd's power-profiles-daemon API) ----
 * power-profiles-daemon's CPU drivers: intel_pstate / amd-pstate-epp: performance -> governor performance (EPP 0),
 * balanced -> powersave + EPP balance_performance, power-saver -> powersave + EPP power; acpi-cpufreq (ppd has no
 * driver for it): performance / schedutil / powersave. Without a performance-state driver only power-saver and
 * balanced exist (ppd's "placeholder") and nothing changes on the CPUs. A profile change overrides the governor and
 * EPP files of every policy, as ppd does; later writes to those files stay possible. */
static unsigned profile = 1;   /* 0 power-saver, 1 balanced, 2 performance */
unsigned cpufreq_profile(void) { return profile; }
bool cpufreq_profile_supported(unsigned value) { return value < 2 || (value == 2 && driver != DRIVER_NONE); }
const char* cpufreq_driver_name(void) { return driver_names[driver]; }
int cpufreq_set_profile(unsigned value) {
    if (!cpufreq_profile_supported(value)) return -22;
    profile = value;
    for (unsigned i = 0; i < cpu_total; i++) {
        struct policy* p = &policies[i];
        if (!p->online) continue;
        if (driver == DRIVER_HWP || driver == DRIVER_CPPC) {
            p->governor = value == 2 ? GOV_PERFORMANCE : GOV_POWERSAVE; p->epp = value == 0 ? 0xFF : -1;
        } else {
            p->governor = value == 2 ? GOV_PERFORMANCE : value == 1 ? GOV_SCHEDUTIL : GOV_POWERSAVE;
            p->window_at = platform->now_ns(); p->idle_ns = 0; p->busy = 1024;
        }
        apply(i);
    }
    return 0;
}

/* ---- detection ---- */
static bool vendor(const char* name) {   /* "GenuineIntel" / "AuthenticAMD" */
    uint32_t r[4]; platform->cpuid(0, r); char v[13];
    memcpy(v, &r[1], 4); memcpy(v + 4, &r[3], 4); memcpy(v + 8, &r[2], 4); v[12] = 0;
    return !strcmp(v, name);
}
static bool detect_hwp(void) {
    uint32_t r[4]; platform->cpuid(0, r);
    if (r[0] < 6 || !vendor("GenuineIntel")) return false;
    platform->cpuid(6, r); if (!((r[0] >> 7) & 1)) return false;
    platform->wrmsr(MSR_PM_ENABLE, 1);
    uint64_t caps; if (!platform->rdmsr(MSR_HWP_CAPABILITIES, &caps)) return false;
    uint32_t highest = caps & 0xFF, guaranteed = (caps >> 8) & 0xFF, efficient = (caps >> 16) & 0xFF, lowest = (caps >> 24) & 0xFF;
    if (!highest || !lowest || lowest > highest) return false;
    for (unsigned i = 0; i < cpu_total; i++)
        policies[i] = (struct policy){.lowest = lowest, .nonlinear = efficient, .nominal = guaranteed ? guaranteed : highest, .highest = highest, .unit_khz = 100000};
    return true;
}
static bool detect_cppc(void) {
    uint32_t r[4];
    if (!vendor("AuthenticAMD")) return false;
    platform->cpuid(0x80000000U, r); if (r[0] < 0x80000008U) return false;
    platform->cpuid(0x80000008U, r); if (!((r[1] >> 27) & 1)) return false;
    platform->wrmsr(MSR_AMD_CPPC_ENABLE, 1);
    uint64_t caps; if (!platform->rdmsr(MSR_AMD_CPPC_CAP1, &caps)) return false;
    uint32_t lowest = caps & 0xFF, nonlinear = (caps >> 8) & 0xFF, nominal = (caps >> 16) & 0xFF, highest = (caps >> 24) & 0xFF;
    if (!lowest || !nominal || lowest > nominal || nominal > highest) return false;
    /* the nominal frequency: _PSS P0, else the P0 definition (family 1Ah: CpuFid[11:0] x 5 MHz; 17h/19h: 200 x FID / DFS) */
    uint32_t nominal_mhz = 0; struct acpi_pstate s[PSTATES];
    if (platform->pss && platform->pss(0, s, PSTATES) > 0) nominal_mhz = s[0].mhz;
    uint64_t definition;
    if (!nominal_mhz && platform->rdmsr(MSR_AMD_PSTATE_DEF, &definition) && (definition >> 63)) {
        platform->cpuid(1, r); unsigned family = ((r[0] >> 8) & 15) + ((r[0] >> 20) & 255);
        if (family >= 0x1A) nominal_mhz = (uint32_t)(definition & 0xFFF) * 5;
        else if ((definition >> 8) & 0x3F) nominal_mhz = (uint32_t)(200 * (definition & 0xFF) / ((definition >> 8) & 0x3F));
    }
    if (!nominal_mhz) return false;
    for (unsigned i = 0; i < cpu_total; i++)
        policies[i] = (struct policy){.lowest = lowest, .nonlinear = nonlinear, .nominal = nominal, .highest = highest, .unit_khz = nominal_mhz * 1000 / nominal};
    return true;
}
static bool detect_acpi(void) {
    if (!platform->pss || !platform->pct) return false;
    for (unsigned i = 0; i < cpu_total; i++) {
        struct policy* p = &policies[i];
        int n = platform->pss(i, p->states, PSTATES);
        if (n <= 0 || platform->pct(i, &p->control, &p->status) || (p->control.space != 1 && p->control.space != 0x7F)) { p->state_count = 0; continue; }
        p->state_count = (unsigned)n; p->ppc = platform->ppc(i);
        for (unsigned a = 0; a < p->state_count; a++)   /* fastest first (ACPI 8.4.5.2) */
            for (unsigned b = a + 1; b < p->state_count; b++) if (p->states[b].mhz > p->states[a].mhz) { struct acpi_pstate t = p->states[a]; p->states[a] = p->states[b]; p->states[b] = t; }
        if (p->control.space == 0x7F) fixed_hw_amd = vendor("AuthenticAMD");
    }
    return policies[0].state_count > 0;
}
void cpufreq_platform_init(const struct cpufreq_platform* with) {
    platform = with; cpu_total = with->cpus(); if (cpu_total > POLICIES) cpu_total = POLICIES;
    memset(policies, 0, sizeof(policies)); memset(idle_cpus, 0, sizeof(idle_cpus)); driver = DRIVER_NONE; thermal_percent = 100; fixed_hw_amd = false;
    if (detect_hwp()) driver = DRIVER_HWP;
    else if (detect_cppc()) driver = DRIVER_CPPC;
    else if (memset(policies, 0, sizeof(policies)), detect_acpi()) driver = DRIVER_ACPI;
    idle_setup();
    if (driver == DRIVER_NONE) { say("[64] CPUFREQ: no performance-state driver (no HWP, CPPC or _PSS); cpuidle "); say_number(idle_count); say(" states\n"); return; }
    for (unsigned i = 0; i < cpu_total; i++) {
        struct policy* p = &policies[i];
        if (driver == DRIVER_ACPI && !p->state_count) continue;
        p->online = true; p->epp = -1; p->min_khz = info_min(p); p->max_khz = info_max(p); p->cur_state = ~0U;
        p->governor = driver == DRIVER_ACPI ? GOV_SCHEDUTIL : GOV_POWERSAVE;   /* Linux's defaults */
        p->busy = 1024; p->window_at = platform->now_ns();
        apply(i);
    }
    struct policy* p = &policies[0];
    say("[64] CPUFREQ: "); say(driver_names[driver]); say(" "); say_number(info_min(p) / 1000); say("-"); say_number(info_max(p) / 1000);
    say(" MHz, governor "); say(governor_names[p->governor]);
    if (driver == DRIVER_ACPI) { say(", "); say_number(p->state_count); say(" P-states, _PPC="); say_number((uint64_t)p->ppc); }
    say("; cpuidle "); say_number(idle_count); say(" states\n");
}

/* ---- sysfs ---- */
enum { FA_AFFECTED, FA_RELATED, FA_INFO_MIN, FA_INFO_MAX, FA_LATENCY, FA_CUR, FA_DRIVER, FA_GOVERNOR, FA_GOVERNORS, FA_MIN, FA_MAX,
       FA_FREQUENCIES, FA_BIOS_LIMIT, FA_EPP, FA_EPPS, FA_CUR_INFO, FA_COUNT };
static const char* const freq_files[FA_COUNT] = {"affected_cpus", "related_cpus", "cpuinfo_min_freq", "cpuinfo_max_freq",
    "cpuinfo_transition_latency", "scaling_cur_freq", "scaling_driver", "scaling_governor", "scaling_available_governors",
    "scaling_min_freq", "scaling_max_freq", "scaling_available_frequencies", "bios_limit", "energy_performance_preference",
    "energy_performance_available_preferences", "cpuinfo_cur_freq"};
enum { IA_NAME, IA_DESC, IA_LATENCY, IA_RESIDENCY, IA_POWER, IA_USAGE, IA_TIME, IA_DISABLE, IA_ABOVE, IA_BELOW, IA_COUNT };
static const char* const idle_files[IA_COUNT] = {"name", "desc", "latency", "residency", "power", "usage", "time", "disable", "above", "below"};
enum { GA_DRIVER, GA_GOVERNOR, GA_GOVERNOR_RO, GA_GOVERNORS };
static const char* const epp_names[] = {"default", "performance", "balance_performance", "balance_power", "power"};
static const int epp_values[] = {-1, 0, 0x80, 0xC0, 0xFF};
struct out { char* data; unsigned used, capacity; };
static void put(struct out* o, const char* s) { while (*s && o->used + 1 < o->capacity) o->data[o->used++] = *s++; if (o->capacity) o->data[o->used] = 0; }
static void put_number(struct out* o, uint64_t value) { char t[24]; number64(value, t); put(o, t); }
static bool word(const char* text, const char* w) { unsigned n = strlen(w); return !strncmp(text, w, n) && (!text[n] || (text[n] == '\n' && !text[n + 1])); }
static bool number(const char* s, uint64_t* value) {
    uint64_t v = 0; bool any = false;
    while (*s >= '0' && *s <= '9') { if (v > 0xFFFFFFFFULL) return false; v = v * 10 + (uint64_t)(*s++ - '0'); any = true; }
    if (*s == '\n') s++;
    if (!any || *s) return false;
    *value = v; return true;
}
int64_t cpufreq_sysfs_read(unsigned id, char* text, unsigned capacity) {
    struct out o = {text, 0, capacity}; unsigned c = id >> 12, instance = (id >> 6) & 63, a = id & 63;
    if (capacity) text[0] = 0;
    if (c == CLASS_CPUIDLE) {
        if (instance == 63) { put(&o, a == GA_DRIVER ? "acpi_idle" : "menu"); put(&o, "\n"); return o.used; }
        unsigned state = a / 10, f = a % 10; if (instance >= cpu_total || state >= idle_count) return -2;
        const struct idle_cpu* d = &idle_cpus[instance];
        switch (f) {
        case IA_NAME: put(&o, idle[state].name); break;
        case IA_DESC: put(&o, idle[state].desc); break;
        case IA_LATENCY: put_number(&o, idle[state].latency_us); break;
        case IA_RESIDENCY: put_number(&o, idle[state].residency_us); break;
        case IA_POWER: put_number(&o, idle[state].power_mw); break;
        case IA_USAGE: put_number(&o, d->usage[state]); break;
        case IA_TIME: put_number(&o, d->time_us[state]); break;
        case IA_DISABLE: put(&o, d->disabled[state] ? "1" : "0"); break;
        case IA_ABOVE: put_number(&o, d->above[state]); break;
        case IA_BELOW: put_number(&o, d->below[state]); break;
        }
        put(&o, "\n"); return o.used;
    }
    if (c != CLASS_CPUFREQ || instance >= cpu_total || !policies[instance].online || a >= FA_COUNT) return -2;
    const struct policy* p = &policies[instance];
    switch (a) {
    case FA_AFFECTED: case FA_RELATED: put_number(&o, instance); break;
    case FA_INFO_MIN: put_number(&o, info_min(p)); break;
    case FA_INFO_MAX: put_number(&o, info_max(p)); break;
    case FA_LATENCY: put_number(&o, driver == DRIVER_ACPI ? (uint64_t)p->states[0].latency_us * 1000 : 0); break;
    case FA_CUR: case FA_CUR_INFO: put_number(&o, p->cur_khz); break;
    case FA_DRIVER: put(&o, driver_names[driver]); break;
    case FA_GOVERNOR: put(&o, governor_names[p->governor]); break;
    case FA_GOVERNORS: put(&o, driver == DRIVER_ACPI ? "performance powersave schedutil" : "performance powersave"); break;
    case FA_MIN: put_number(&o, p->min_khz); break;
    case FA_MAX: put_number(&o, p->max_khz); break;
    case FA_FREQUENCIES: for (unsigned i = 0; i < p->state_count; i++) { if (i) put(&o, " "); put_number(&o, (uint64_t)p->states[i].mhz * 1000); } break;
    case FA_BIOS_LIMIT: put_number(&o, p->ppc > 0 && (unsigned)p->ppc < p->state_count ? (uint64_t)p->states[p->ppc].mhz * 1000 : info_max(p)); break;
    case FA_EPP: {
        int value = p->governor == GOV_PERFORMANCE ? 0 : p->epp < 0 ? 0x80 : p->epp; const char* name = NULL;
        for (unsigned i = 1; i < 5; i++) if (epp_values[i] == value) name = epp_names[i];
        if (name) put(&o, name); else put_number(&o, (uint64_t)value);
        break; }
    case FA_EPPS: put(&o, "default performance balance_performance balance_power power"); break;
    }
    put(&o, "\n"); return o.used;
}
int64_t cpufreq_sysfs_write(unsigned id, const char* text, unsigned length) {
    (void)length;
    unsigned c = id >> 12, instance = (id >> 6) & 63, a = id & 63; uint64_t value;
    if (c == CLASS_CPUIDLE) {
        unsigned state = a / 10, f = a % 10;
        if (instance >= cpu_total || state >= idle_count || f != IA_DISABLE || !number(text, &value) || value > 1) return -22;
        idle_cpus[instance].disabled[state] = value != 0; return 0;
    }
    if (c != CLASS_CPUFREQ || instance >= cpu_total || !policies[instance].online) return -22;
    struct policy* p = &policies[instance];
    switch (a) {
    case FA_GOVERNOR:
        for (unsigned g = 0; g < 3; g++) if (word(text, governor_names[g])) {
            if (g == GOV_SCHEDUTIL && driver != DRIVER_ACPI) return -22;
            p->governor = g; p->window_at = platform->now_ns(); p->idle_ns = 0; p->busy = 1024; apply(instance); return 0;
        }
        return -22;
    case FA_MIN: case FA_MAX:
        if (!number(text, &value)) return -22;
        if (value < info_min(p)) value = info_min(p);
        if (value > info_max(p)) value = info_max(p);
        if (a == FA_MIN) { p->min_khz = (uint32_t)value; if (p->max_khz < p->min_khz) p->max_khz = p->min_khz; }
        else { p->max_khz = (uint32_t)value; if (p->min_khz > p->max_khz) p->min_khz = p->max_khz; }
        apply(instance); return 0;
    case FA_EPP:
        if (driver != DRIVER_HWP && driver != DRIVER_CPPC) return -22;
        if (p->governor == GOV_PERFORMANCE) return -16;   /* Linux: EBUSY, the performance policy pins EPP 0 */
        for (unsigned i = 0; i < 5; i++) if (word(text, epp_names[i])) { p->epp = epp_values[i]; apply(instance); return 0; }
        if (!number(text, &value) || value > 255) return -22;
        p->epp = (int)value; apply(instance); return 0;
    }
    return -22;
}
void cpufreq_sysfs_init(void) {
    if (!platform) return;
    char path[112], link[112], number_text[8];
    if (driver != DRIVER_NONE) {
        sysfs_add("/sys/devices/system/cpu/cpufreq", 0, 0, NULL);
        for (unsigned i = 0; i < cpu_total && i < 63; i++) {
            if (!policies[i].online) continue;
            uint_to_str(i, number_text);
            strcpy(path, "/sys/devices/system/cpu/cpufreq/policy"); strcat(path, number_text); sysfs_add(path, 0, 0, NULL);
            for (unsigned a = 0; a < FA_COUNT; a++) {
                if ((a == FA_FREQUENCIES || a == FA_BIOS_LIMIT) && driver != DRIVER_ACPI) continue;
                if ((a == FA_EPP || a == FA_EPPS) && driver == DRIVER_ACPI) continue;
                char file[144]; strcpy(file, path); strcat(file, "/"); strcat(file, freq_files[a]);
                bool writable = a == FA_GOVERNOR || a == FA_MIN || a == FA_MAX || a == FA_EPP;
                sysfs_add(file, writable ? 2 : 1, ID(CLASS_CPUFREQ, i, a), NULL);
            }
            strcpy(link, "/sys/devices/system/cpu/cpu"); strcat(link, number_text); strcat(link, "/cpufreq"); sysfs_add(link, 3, 0, path);
        }
    }
    sysfs_add("/sys/devices/system/cpu/cpuidle", 0, 0, NULL);
    static const char* const global[] = {"current_driver", "current_governor", "current_governor_ro", "available_governors"};
    for (unsigned a = 0; a < 4; a++) { strcpy(path, "/sys/devices/system/cpu/cpuidle/"); strcat(path, global[a]); sysfs_add(path, 1, ID(CLASS_CPUIDLE, 63, a), NULL); }
    for (unsigned i = 0; i < cpu_total && i < 63; i++) {
        uint_to_str(i, number_text);
        strcpy(path, "/sys/devices/system/cpu/cpu"); strcat(path, number_text); strcat(path, "/cpuidle"); sysfs_add(path, 0, 0, NULL);
        for (unsigned s = 0; s < idle_count; s++) {
            char state[128], n[4]; strcpy(state, path); strcat(state, "/state"); uint_to_str(s, n); strcat(state, n); sysfs_add(state, 0, 0, NULL);
            for (unsigned f = 0; f < IA_COUNT; f++) {
                char file[144]; strcpy(file, state); strcat(file, "/"); strcat(file, idle_files[f]);
                sysfs_add(file, f == IA_DISABLE ? 2 : 1, ID(CLASS_CPUIDLE, i, s * 10 + f), NULL);
            }
        }
    }
}

#ifndef CPUFREQ_HOST_TEST
/* ---- the kernel's platform ---- */
static void k_cpuid(uint32_t leaf, uint32_t r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0)); }
static bool k_rdmsr(uint32_t msr, uint64_t* value) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); *value = (uint64_t)hi << 32 | lo; return true; }
static void k_wrmsr(uint32_t msr, uint64_t value) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory"); }
static uint32_t k_io_read(uint16_t port, unsigned bits) { return bits > 16 ? port_dword_in(port) : bits > 8 ? port_word_in(port) : port_byte_in(port); }
static void k_io_write(uint16_t port, uint32_t value, unsigned bits) {
    if (bits > 16) port_dword_out(port, value); else if (bits > 8) port_word_out(port, (uint16_t)value); else port_byte_out(port, (uint8_t)value);
}
static unsigned k_cpus(void) { return cpu_count; }
static unsigned k_self(void) { return cpu_self()->index; }
static void k_kick(unsigned cpu) { if (cpu < MAX_CPUS && cpus[cpu].online) smp_send_ipi(cpu, VEC_WAKE); }
static uint64_t k_now(void) { return native_clock_ns(); }
static const struct cpufreq_platform kernel_platform = {k_cpuid, k_rdmsr, k_wrmsr, k_io_read, k_io_write, k_cpus, k_self, k_kick, k_now,
                                                        acpi_processor_pss, acpi_processor_pct, acpi_processor_ppc, acpi_processor_cst};
void cpufreq_init(void) { cpufreq_platform_init(&kernel_platform); }
#endif
