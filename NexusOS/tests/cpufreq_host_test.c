/* arch/x86_64/cpufreq.c on simulated processors (tests/test_cpufreq_host.py builds and runs this on the host).
 *
 * The processors are modelled on public specifications, not on dumps of real firmware: Intel HWP per the SDM (vol. 3B
 * 15.4, IA32_HWP_CAPABILITIES/REQUEST layouts), AMD CPPC per the PPR for family 1Ah (MSRC001_02B0/02B1/02B3, P0 definition
 * MSRC001_0064 CpuFid x 5 MHz) with figures of a Ryzen 7 9700X (3.8 GHz base, 5.5 GHz boost), and ACPI _PSS/_PCT/_PPC/
 * _CST per ACPI 6.5 sections 8.4. Every MSR write is recorded per logical CPU, so the tests also check that a request
 * is written by the CPU it belongs to (on its next tick), never by another one. */
#include "cpufreq_host.h"
#define CPUFREQ_HOST_TEST 1
#include "../arch/x86_64/cpufreq.c"
#include <stdlib.h>

#define CPUS 4
#define SLOTS 32
static int failures;
#define CHECK(test, ...) do { if (!(test)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the simulated machine ---- */
static struct {
    char vendor[13]; uint32_t signature; bool monitor, hwp, cppc;
    struct { uint32_t msr; uint64_t value; unsigned writes; } msr[CPUS][SLOTS];
    unsigned kicks[CPUS], self; uint64_t now;
    struct acpi_pstate pss[8]; unsigned pss_count; struct acpi_register control; int ppc;
    struct acpi_cstate cst[4]; int cst_count;
    uint32_t port_value[65536 / 4]; unsigned port_writes;
    char sysfs[1024][144]; unsigned sysfs_count;
} m;
static void machine(const char* vendor, uint32_t signature) { memset(&m, 0, sizeof(m)); strcpy(m.vendor, vendor); m.signature = signature; m.cst_count = -61; }
static uint64_t* msr_slot(unsigned cpu, uint32_t msr, bool create) {
    for (unsigned i = 0; i < SLOTS; i++) if (m.msr[cpu][i].msr == msr && (m.msr[cpu][i].writes || m.msr[cpu][i].value || !create)) return &m.msr[cpu][i].value;
    if (!create) return NULL;
    for (unsigned i = 0; i < SLOTS; i++) if (!m.msr[cpu][i].msr) { m.msr[cpu][i].msr = msr; return &m.msr[cpu][i].value; }
    abort();
}
static unsigned msr_writes(unsigned cpu, uint32_t msr) { for (unsigned i = 0; i < SLOTS; i++) if (m.msr[cpu][i].msr == msr) return m.msr[cpu][i].writes; return 0; }
static uint64_t msr_value(unsigned cpu, uint32_t msr) { uint64_t* v = msr_slot(cpu, msr, false); return v ? *v : 0; }
static void msr_set_all(uint32_t msr, uint64_t value) { for (unsigned c = 0; c < CPUS; c++) *msr_slot(c, msr, true) = value; }
static void s_cpuid(uint32_t leaf, uint32_t r[4]) {
    memset(r, 0, 16);
    if (leaf == 0) { r[0] = 0x16; memcpy(&r[1], m.vendor, 4); memcpy(&r[3], m.vendor + 4, 4); memcpy(&r[2], m.vendor + 8, 4); }
    else if (leaf == 1) { r[0] = m.signature; r[2] = m.monitor ? 1U << 3 : 0; }
    else if (leaf == 6) r[0] = m.hwp ? 1U << 7 : 0;
    else if (leaf == 0x80000000U) r[0] = 0x80000008U;
    else if (leaf == 0x80000008U) r[1] = m.cppc ? 1U << 27 : 0;
}
static bool s_rdmsr(uint32_t msr, uint64_t* value) { uint64_t* v = msr_slot(m.self, msr, false); if (!v) return false; *value = *v; return true; }
static void s_wrmsr(uint32_t msr, uint64_t value) {
    uint64_t* v = msr_slot(m.self, msr, true); *v = value;
    for (unsigned i = 0; i < SLOTS; i++) if (m.msr[m.self][i].msr == msr) { m.msr[m.self][i].writes++; break; }
}
static uint32_t s_io_read(uint16_t port, unsigned bits) { (void)bits; return m.port_value[port / 4]; }
static void s_io_write(uint16_t port, uint32_t value, unsigned bits) { (void)bits; m.port_value[port / 4] = value; m.port_writes++; }
static unsigned s_cpus(void) { return CPUS; }
static unsigned s_self(void) { return m.self; }
static void s_kick(unsigned cpu) { m.kicks[cpu]++; }
static uint64_t s_now(void) { return m.now; }
static int s_pss(unsigned cpu, struct acpi_pstate* out, unsigned capacity) {
    (void)cpu; if (!m.pss_count) return -61;
    unsigned n = m.pss_count < capacity ? m.pss_count : capacity; memcpy(out, m.pss, n * sizeof(*out)); return (int)n;
}
static int s_pct(unsigned cpu, struct acpi_register* control, struct acpi_register* status) { (void)cpu; if (!m.pss_count) return -61; *control = *status = m.control; return 0; }
static int s_ppc(unsigned cpu) { (void)cpu; return m.ppc; }
static int s_cst(unsigned cpu, struct acpi_cstate* out, unsigned capacity) {
    (void)cpu; if (m.cst_count < 0) return m.cst_count;
    unsigned n = (unsigned)m.cst_count < capacity ? (unsigned)m.cst_count : capacity; memcpy(out, m.cst, n * sizeof(*out)); return (int)n;
}
static const struct cpufreq_platform simulated = {s_cpuid, s_rdmsr, s_wrmsr, s_io_read, s_io_write, s_cpus, s_self, s_kick, s_now, s_pss, s_pct, s_ppc, s_cst};
int sysfs_add(const char* path, unsigned type, unsigned id, const char* target) {
    (void)type; (void)id; (void)target;
    if (m.sysfs_count < 1024) snprintf(m.sysfs[m.sysfs_count++], 144, "%s", path);
    return 0;
}
static bool has_node(const char* path) { for (unsigned i = 0; i < m.sysfs_count; i++) if (!strcmp(m.sysfs[i], path)) return true; return false; }
static void tick_all(void) { unsigned saved = m.self; for (unsigned c = 0; c < CPUS; c++) { m.self = c; cpufreq_tick(); } m.self = saved; }
static const char* read_attr(unsigned cpu, unsigned attribute) {
    static char text[256]; int64_t n = cpufreq_sysfs_read(ID(CLASS_CPUFREQ, cpu, attribute), text, sizeof(text));
    if (n < 0) return "<error>";
    if (n && text[n - 1] == '\n') text[n - 1] = 0;
    return text;
}
static int64_t write_attr(unsigned cpu, unsigned attribute, const char* value) { return cpufreq_sysfs_write(ID(CLASS_CPUFREQ, cpu, attribute), value, (unsigned)strlen(value)); }
static void boot(void) { m.self = 0; cpufreq_platform_init(&simulated); cpufreq_sysfs_init(); }

/* ---- Intel HWP: highest 45, guaranteed 30, efficient 12, lowest 8 (4.5 GHz turbo, 3.0 GHz base, 800 MHz) ---- */
static void test_hwp(void) {
    machine("GenuineIntel", 0x000B06A2); m.hwp = true;
    msr_set_all(MSR_HWP_CAPABILITIES, 45 | 30 << 8 | 12 << 16 | 8u << 24);
    *msr_slot(3, MSR_HWP_CAPABILITIES, true) = 32 | 20 << 8 | 10 << 16 | 6u << 24;   /* an efficiency core (hybrid) */
    boot();
    CHECK(driver == DRIVER_HWP, "driver %u", driver);
    CHECK(!strcmp(read_attr(0, FA_DRIVER), "intel_pstate") && !strcmp(read_attr(0, FA_GOVERNOR), "powersave"), "%s", read_attr(0, FA_GOVERNOR));
    CHECK(!strcmp(read_attr(0, FA_INFO_MAX), "4500000") && !strcmp(read_attr(0, FA_INFO_MIN), "800000"), "%s %s", read_attr(0, FA_INFO_MAX), read_attr(0, FA_INFO_MIN));
    CHECK(!strcmp(read_attr(0, FA_GOVERNORS), "performance powersave"), "%s", read_attr(0, FA_GOVERNORS));
    CHECK(m.kicks[1] && m.kicks[2] && m.kicks[3] && !msr_writes(1, MSR_HWP_REQUEST), "other CPUs must apply their own requests");
    tick_all();
    for (unsigned c = 0; c < CPUS; c++) CHECK(msr_value(c, MSR_PM_ENABLE) == 1 && msr_writes(c, MSR_HWP_REQUEST) >= 1, "cpu%u not enabled/requested", c);
    CHECK(msr_value(0, MSR_HWP_REQUEST) == (8 | 45 << 8 | 0x80ULL << 24), "powersave request %llx", (unsigned long long)msr_value(0, MSR_HWP_REQUEST));
    CHECK(msr_value(3, MSR_HWP_REQUEST) == (6 | 32 << 8 | 0x80ULL << 24), "hybrid E-core request %llx", (unsigned long long)msr_value(3, MSR_HWP_REQUEST));
    CHECK(!strcmp(read_attr(3, FA_INFO_MAX), "3200000"), "E-core max %s", read_attr(3, FA_INFO_MAX));
    CHECK(write_attr(2, FA_GOVERNOR, "performance\n") == 0, "governor");
    unsigned before = msr_writes(2, MSR_HWP_REQUEST); tick_all();
    CHECK(msr_writes(2, MSR_HWP_REQUEST) == before + 1 && msr_value(2, MSR_HWP_REQUEST) == (45 | 45 << 8), "performance %llx", (unsigned long long)msr_value(2, MSR_HWP_REQUEST));
    CHECK(write_attr(2, FA_EPP, "power") == -16, "EPP under performance must be EBUSY");
    CHECK(!strcmp(read_attr(2, FA_EPP), "performance"), "%s", read_attr(2, FA_EPP));
    CHECK(write_attr(2, FA_GOVERNOR, "schedutil") == -22 && write_attr(2, FA_GOVERNOR, "turbo") == -22, "governor validation");
    write_attr(2, FA_GOVERNOR, "powersave");
    CHECK(write_attr(2, FA_EPP, "balance_power") == 0 && !strcmp(read_attr(2, FA_EPP), "balance_power"), "%s", read_attr(2, FA_EPP));
    CHECK(write_attr(2, FA_EPP, "77") == 0 && !strcmp(read_attr(2, FA_EPP), "77"), "%s", read_attr(2, FA_EPP));
    CHECK(write_attr(2, FA_EPP, "300") == -22 && write_attr(2, FA_EPP, "eco") == -22, "EPP validation");
    write_attr(2, FA_EPP, "power"); tick_all();
    CHECK((msr_value(2, MSR_HWP_REQUEST) >> 24) == 0xFF, "EPP power %llx", (unsigned long long)msr_value(2, MSR_HWP_REQUEST));
    CHECK(write_attr(1, FA_MAX, "3000000") == 0 && write_attr(1, FA_MIN, "4000000") == 0, "limits");
    CHECK(!strcmp(read_attr(1, FA_MIN), "4000000") && !strcmp(read_attr(1, FA_MAX), "4000000"), "min above max lifts max: %s %s", read_attr(1, FA_MIN), read_attr(1, FA_MAX));
    write_attr(1, FA_MIN, "0"); tick_all();
    CHECK((msr_value(1, MSR_HWP_REQUEST) & 0xFFFF) == (8 | 40 << 8), "user max 4.0 GHz: %llx", (unsigned long long)msr_value(1, MSR_HWP_REQUEST));
    cpufreq_thermal_cap(50); tick_all();
    CHECK(((msr_value(1, MSR_HWP_REQUEST) >> 8) & 0xFF) == 22, "thermal cap 50%%: %llx", (unsigned long long)msr_value(1, MSR_HWP_REQUEST));
    cpufreq_thermal_cap(100); tick_all();
    CHECK(((msr_value(1, MSR_HWP_REQUEST) >> 8) & 0xFF) == 40, "cap released: %llx", (unsigned long long)msr_value(1, MSR_HWP_REQUEST));
    CHECK(has_node("/sys/devices/system/cpu/cpufreq/policy3/energy_performance_preference") && !has_node("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies"),
          "HWP sysfs files");
}

/* ---- AMD CPPC, Ryzen 7 9700X (family 1Ah): highest 174, nominal 120, lowest nonlinear 61, lowest 18; P0 3.8 GHz ---- */
static void test_cppc(bool with_pss) {
    machine("AuthenticAMD", 0x00B40F40); m.cppc = true;
    msr_set_all(MSR_AMD_CPPC_CAP1, 18 | 61 << 8 | 120 << 16 | 174u << 24);
    msr_set_all(MSR_AMD_PSTATE_DEF, 1ULL << 63 | 760);   /* CpuFid 760 x 5 MHz = 3800 MHz */
    if (with_pss) { m.pss_count = 1; m.pss[0] = (struct acpi_pstate){3800, 65000, 1, 1, 0, 0}; m.control = (struct acpi_register){0x7F, 64, 0, 0, 0}; }
    boot(); tick_all();
    CHECK(driver == DRIVER_CPPC && !strcmp(read_attr(0, FA_DRIVER), "amd-pstate-epp"), "driver %u", driver);
    CHECK(!strcmp(read_attr(0, FA_INFO_MAX), "5509884") && !strcmp(read_attr(0, FA_INFO_MIN), "569988"), "%s %s", read_attr(0, FA_INFO_MAX), read_attr(0, FA_INFO_MIN));
    for (unsigned c = 0; c < CPUS; c++) CHECK(msr_value(c, MSR_AMD_CPPC_ENABLE) == 1, "cpu%u CPPC not enabled", c);
    CHECK(msr_value(0, MSR_AMD_CPPC_REQ) == (174 | 18 << 8 | 0x80ULL << 24), "powersave request %llx", (unsigned long long)msr_value(0, MSR_AMD_CPPC_REQ));
    write_attr(0, FA_GOVERNOR, "performance"); tick_all();
    CHECK(msr_value(0, MSR_AMD_CPPC_REQ) == (174 | 174 << 8), "performance request %llx", (unsigned long long)msr_value(0, MSR_AMD_CPPC_REQ));
    write_attr(0, FA_GOVERNOR, "powersave"); write_attr(0, FA_MAX, "3800000"); tick_all();
    CHECK((msr_value(0, MSR_AMD_CPPC_REQ) & 0xFF) == 120, "base clock limit %llx", (unsigned long long)msr_value(0, MSR_AMD_CPPC_REQ));
}
static void test_cppc_broken(void) {   /* inconsistent capabilities: CPPC refused, _PSS (acpi-cpufreq on the P-state MSRs) used */
    machine("AuthenticAMD", 0x00A60F12); m.cppc = true;
    msr_set_all(MSR_AMD_CPPC_CAP1, 200 | 61 << 8 | 120 << 16 | 174u << 24);
    m.pss_count = 3; m.control = (struct acpi_register){0x7F, 64, 0, 0, 0};
    m.pss[0] = (struct acpi_pstate){4700, 88000, 1, 1, 0, 0}; m.pss[1] = (struct acpi_pstate){3600, 50000, 1, 1, 1, 1}; m.pss[2] = (struct acpi_pstate){2200, 25000, 1, 1, 2, 2};
    boot(); tick_all();
    CHECK(driver == DRIVER_ACPI && fixed_hw_amd, "driver %u amd %d", driver, fixed_hw_amd);
    write_attr(0, FA_GOVERNOR, "performance"); tick_all();
    CHECK(msr_writes(0, MSR_AMD_PSTATE_CTL) && msr_value(0, MSR_AMD_PSTATE_CTL) == 0 && !msr_writes(0, MSR_PERF_CTL), "AMD FFH P0");
    write_attr(0, FA_GOVERNOR, "powersave"); tick_all();
    CHECK(msr_value(0, MSR_AMD_PSTATE_CTL) == 2, "AMD FFH P2 %llu", (unsigned long long)msr_value(0, MSR_AMD_PSTATE_CTL));
}

/* ---- acpi-cpufreq, Intel FFixedHW (IA32_PERF_CTL), schedutil and _PPC ---- */
static void test_acpi_intel(void) {
    machine("GenuineIntel", 0x000306A9);
    m.pss_count = 3; m.control = (struct acpi_register){0x7F, 16, 0, 0, 0};
    m.pss[0] = (struct acpi_pstate){1000, 10000, 10, 10, 0x0A00, 0x0A00}; m.pss[1] = (struct acpi_pstate){3000, 35000, 10, 10, 0x1E00, 0x1E00};
    m.pss[2] = (struct acpi_pstate){2000, 20000, 10, 10, 0x1400, 0x1400};   /* out of order: sorted fastest first */
    boot(); tick_all();
    CHECK(driver == DRIVER_ACPI && !strcmp(read_attr(0, FA_GOVERNOR), "schedutil"), "driver %u", driver);
    CHECK(!strcmp(read_attr(0, FA_FREQUENCIES), "3000000 2000000 1000000"), "%s", read_attr(0, FA_FREQUENCIES));
    CHECK(!strcmp(read_attr(0, FA_LATENCY), "10000"), "%s", read_attr(0, FA_LATENCY));
    CHECK(msr_value(0, MSR_PERF_CTL) == 0x1E00, "schedutil starts at full speed %llx", (unsigned long long)msr_value(0, MSR_PERF_CTL));
    /* 10 ms windows: CPU0 9 ms idle (10% busy), CPU1 1 ms idle (90% busy) */
    m.now += 10000000; policies[0].idle_ns = 9000000; policies[1].idle_ns = 1000000; policies[2].idle_ns = 10000000; policies[3].idle_ns = 5000000;
    cpufreq_poll(); tick_all();
    CHECK(msr_value(0, MSR_PERF_CTL) == 0x0A00, "10%% busy -> 1 GHz: %llx", (unsigned long long)msr_value(0, MSR_PERF_CTL));
    CHECK(msr_value(1, MSR_PERF_CTL) == 0x1E00, "90%% busy -> 3 GHz: %llx", (unsigned long long)msr_value(1, MSR_PERF_CTL));
    CHECK(msr_value(3, MSR_PERF_CTL) == 0x1400, "50%% busy -> 1.25 x 3 GHz x 0.5 = 1.875 -> 2 GHz: %llx", (unsigned long long)msr_value(3, MSR_PERF_CTL));
    CHECK(msr_value(2, MSR_PERF_CTL) == 0x0A00, "idle -> 1 GHz: %llx", (unsigned long long)msr_value(2, MSR_PERF_CTL));
    m.ppc = 1; cpufreq_limits_changed(1); tick_all();
    CHECK(msr_value(1, MSR_PERF_CTL) == 0x1400 && !strcmp(read_attr(1, FA_BIOS_LIMIT), "2000000"), "_PPC=1 limits to 2 GHz: %llx %s",
          (unsigned long long)msr_value(1, MSR_PERF_CTL), read_attr(1, FA_BIOS_LIMIT));
    m.ppc = 0; cpufreq_limits_changed(1); write_attr(1, FA_GOVERNOR, "performance"); tick_all();
    CHECK(msr_value(1, MSR_PERF_CTL) == 0x1E00, "_PPC=0 and performance: %llx", (unsigned long long)msr_value(1, MSR_PERF_CTL));
    cpufreq_thermal_cap(40); tick_all();   /* 40% of 3 GHz = 1.2 GHz: the fastest state within is 1 GHz */
    CHECK(msr_value(1, MSR_PERF_CTL) == 0x0A00, "thermal cap: %llx", (unsigned long long)msr_value(1, MSR_PERF_CTL));
    cpufreq_thermal_cap(100);
    CHECK(write_attr(0, FA_EPP, "power") == -22, "acpi-cpufreq has no EPP");
    CHECK(!has_node("/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference") && has_node("/sys/devices/system/cpu/cpufreq/policy0/bios_limit"), "acpi sysfs");
}
/* ---- no driver: no HWP, no CPPC, no _PSS ---- */
static void test_none(void) {
    machine("GenuineIntel", 0x00050654);
    boot();
    CHECK(driver == DRIVER_NONE && !has_node("/sys/devices/system/cpu/cpufreq") && has_node("/sys/devices/system/cpu/cpuidle/current_driver"), "no-driver layout");
    CHECK(cpufreq_sysfs_read(ID(CLASS_CPUFREQ, 0, FA_DRIVER), (char[64]){0}, 64) == -2, "no policy");
    tick_all(); CHECK(!m.msr[0][0].writes, "no MSR writes without a driver");
}
/* ---- _CST: FFixedHW MWAIT states need MONITOR/MWAIT; C3 through I/O is not used ---- */
static void test_cst(void) {
    machine("GenuineIntel", 0x00050654); m.monitor = true;
    m.cst_count = 4;
    m.cst[0] = (struct acpi_cstate){{0x7F, 1, 2, 1, 0x00}, 1, 1, 1000};
    m.cst[1] = (struct acpi_cstate){{0x7F, 1, 2, 3, 0x10}, 2, 64, 500};
    m.cst[2] = (struct acpi_cstate){{0x7F, 1, 2, 3, 0x20}, 3, 128, 100};
    m.cst[3] = (struct acpi_cstate){{1, 8, 0, 0, 0x415}, 3, 400, 50};
    boot();
    CHECK(idle_count == 4 && idle[1].kind == IDLE_MWAIT && idle[1].hint == 0 && idle[2].kind == IDLE_MWAIT && idle[2].hint == 0x10 &&
          idle[3].hint == 0x20 && idle[3].residency_us == 256, "MWAIT states: %u", idle_count);
    char text[64]; cpufreq_sysfs_read(ID(CLASS_CPUIDLE, 2, 2 * 10 + IA_NAME), text, sizeof(text));
    CHECK(!strcmp(text, "C2\n"), "%s", text);
    CHECK(cpufreq_sysfs_write(ID(CLASS_CPUIDLE, 1, 3 * 10 + IA_DISABLE), "1", 1) == 0 && idle_cpus[1].disabled[3], "disable");
    CHECK(cpufreq_sysfs_write(ID(CLASS_CPUIDLE, 1, 3 * 10 + IA_NAME), "x", 1) == -22 && cpufreq_sysfs_write(ID(CLASS_CPUIDLE, 1, 9 * 10 + IA_DISABLE), "1", 1) == -22, "cpuidle validation");
    m.monitor = false; cpufreq_cstates_changed(0);
    CHECK(idle_count == 2 && idle[1].kind == IDLE_HLT, "without MONITOR: HLT only (%u states)", idle_count);
}

int main(void) {
    test_hwp(); test_cppc(false); test_cppc(true); test_cppc_broken(); test_acpi_intel(); test_none(); test_cst();
    if (failures) { printf("CPUFREQ HOST TESTS FAILED: %d\n", failures); return 1; }
    printf("CPUFREQ HOST TESTS PASSED\n");
    return 0;
}
