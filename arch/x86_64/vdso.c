/* NexusOS x86-64 vDSO: clock_gettime/gettimeofday/time/getcpu without a kernel entry.
 * Time is derived exactly like the kernel's monotonic_ns(): TSC ticks since boot scaled by the
 * calibrated TSC frequency; the kernel fills vdso_data at boot. Unsupported clocks fall back to
 * the real system call. When the CPUs' TSCs are not synchronized (per_cpu), RDTSCP reads the TSC
 * together with the CPU index (TSC_AUX) and that CPU's offset to the boot CPU's TSC is added. */
#include <stdint.h>

struct nexus_timespec { int64_t tv_sec, tv_nsec; };
struct nexus_timeval { int64_t tv_sec, tv_usec; };
/* realtime line (timekeeping.c): real = real_base + d + d * mult / 2^32, d = monotonic - mono_base; seq odd = updating;
 * boottime_offset: time spent in system sleep (pm.c) */
struct vdso_data { char magic[8]; uint64_t tsc_frequency, boot_tsc, realtime_epoch, enabled, per_cpu; int64_t tsc_offset[32];
                   uint64_t seq; int64_t real_base; uint64_t mono_base; int64_t mult; uint64_t boottime_offset;
                   uint64_t rdtscp; };   /* rdtscp: TSC_AUX holds the CPU index (getcpu) */

__attribute__((visibility("hidden"), aligned(64)))
const volatile struct vdso_data vdso_data = { {'N','X','V','D','S','O','0','4'}, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0 };

static inline long nexus_syscall2(long number, long a, long b) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return result;
}
static inline uint64_t nexus_rdtsc(void) {
    uint32_t lo, hi, cpu;
    if (vdso_data.per_cpu) {
        __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(cpu) :: "memory");
        return (((uint64_t)hi << 32) | lo) + (uint64_t)vdso_data.tsc_offset[cpu & 31];
    }
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t nexus_monotonic(void) {
    uint64_t f = vdso_data.tsc_frequency, ticks = nexus_rdtsc() - vdso_data.boot_tsc;
    return (ticks / f) * 1000000000ULL + (ticks % f) * 1000000000ULL / f;
}
/* Mirrors the kernel's clock_ns(): realtime/coarse/boottime ids handled, others go to the kernel. */
static inline int nexus_clock(int id, uint64_t* value) {
    if (!vdso_data.enabled || !vdso_data.tsc_frequency) return 0;
    if (id != 0 && id != 1 && id != 4 && id != 5 && id != 6 && id != 7) return 0;
    uint64_t ns = nexus_monotonic();
    if (id == 7) ns += vdso_data.boottime_offset;   /* CLOCK_BOOTTIME: + time spent in system sleep */
    if (id == 0 || id == 5) {
        for (;;) {
            uint64_t seq = vdso_data.seq;
            if (seq & 1) { __asm__ volatile("pause"); continue; }
            int64_t real_base = vdso_data.real_base, mult = vdso_data.mult; uint64_t mono_base = vdso_data.mono_base;
            __asm__ volatile("" ::: "memory");   /* x86 keeps loads in order; only the compiler must not move them */
            if (vdso_data.seq != seq) continue;
            int64_t delta = (int64_t)(ns - mono_base);
            int64_t real = real_base + delta + (int64_t)(((__int128)delta * mult) >> 32);
            ns = real < 0 ? 0 : (uint64_t)real;
            break;
        }
    }
    if (id == 5 || id == 6) ns = ns / 1000000 * 1000000;
    *value = ns; return 1;
}

int __vdso_clock_gettime(int id, struct nexus_timespec* ts) {
    uint64_t ns;
    if (!ts || !nexus_clock(id, &ns)) return (int)nexus_syscall2(228, id, (long)ts);
    ts->tv_sec = (int64_t)(ns / 1000000000ULL); ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
    return 0;
}
int __vdso_gettimeofday(struct nexus_timeval* tv, void* tz) {
    uint64_t ns;
    if (!nexus_clock(0, &ns)) return (int)nexus_syscall2(96, (long)tv, (long)tz);
    if (tv) { tv->tv_sec = (int64_t)(ns / 1000000000ULL); tv->tv_usec = (int64_t)(ns % 1000000000ULL / 1000); }
    if (tz) *(volatile uint64_t*)tz = 0; /* struct timezone {minuteswest, dsttime}: UTC, as the syscall reports */
    return 0;
}
int64_t __vdso_time(int64_t* out) {
    uint64_t ns;
    if (!nexus_clock(0, &ns)) return nexus_syscall2(201, (long)out, 0);
    int64_t seconds = (int64_t)(ns / 1000000000ULL);
    if (out) *out = seconds;
    return seconds;
}
int __vdso_clock_getres(int id, struct nexus_timespec* res) { return (int)nexus_syscall2(229, id, (long)res); }
int __vdso_getcpu(unsigned* cpu, unsigned* node, void* unused) {
    (void)unused;
    if (!vdso_data.rdtscp) return (int)nexus_syscall2(309, (long)cpu, (long)node);   /* the kernel knows the CPU */
    uint32_t lo, hi, index;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(index));
    (void)lo; (void)hi;
    if (cpu) *cpu = index & 0xFFF;
    if (node) *node = 0;
    return 0;
}
int clock_gettime(int, struct nexus_timespec*) __attribute__((weak, alias("__vdso_clock_gettime")));
int gettimeofday(struct nexus_timeval*, void*) __attribute__((weak, alias("__vdso_gettimeofday")));
int64_t time(int64_t*) __attribute__((weak, alias("__vdso_time")));
int clock_getres(int, struct nexus_timespec*) __attribute__((weak, alias("__vdso_clock_getres")));
int getcpu(unsigned*, unsigned*, void*) __attribute__((weak, alias("__vdso_getcpu")));
