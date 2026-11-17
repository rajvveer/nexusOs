/* CLOCK_REALTIME that can be set and slewed, for NTP clients (chrony) and timedatectl-style tools:
 * clock_settime(2), settimeofday(2), adjtimex(2), clock_adjtime(2) on CLOCK_REALTIME.
 *
 * realtime(mono) = real_base + (mono - mono_base) + (mono - mono_base) * mult / 2^32, with mono the kernel's monotonic
 * nanoseconds (TSC). mult carries the NTP frequency correction (ADJ_FREQUENCY, ppm << 16, +-500 ppm), the tick
 * correction (ADJ_TICK, 9000..11000 us per 1/100 s) and an active adjtime-style slew (ADJ_OFFSET_SINGLESHOT, and
 * ADJ_OFFSET, applied the same way at 500 ppm: there is no kernel PLL). Every change re-bases the line at the current
 * instant, so time never jumps except by an explicit step (settimeofday, clock_settime, ADJ_SETOFFSET). The three
 * parameters are published under a sequence counter to the kernel's readers and to the vDSO data page (vdso.c), so
 * user clock reads stay system-call free. CLOCK_MONOTONIC is not slewed. Steps also set the CMOS RTC (UTC).
 * Locking: tk_lock (leaf) orders writers; readers retry on an odd or changed sequence. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/rtc.h"

#define CAP_SYS_TIME 25
#define STA_UNSYNC 0x0040
#define STA_NANO 0x2000
#define STA_RONLY 0xFF00   /* PPSSIGNAL..CLK: read only */
#define MAXFREQ_SCALED (500LL << 16)
#define SLEW_MULT 2147484LL         /* 500 ppm in 2^-32 units */
#define NSEC 1000000000LL

struct clock_line { volatile uint64_t seq; int64_t real_base; uint64_t mono_base; int64_t mult; };
static struct clock_line line;
static volatile uint64_t* vdso_line;           /* seq, real_base, mono_base, mult in the vDSO data page */
static volatile int tk_lock;
static int64_t freq = 0, tick = 10000, maxerror = 16000000, esterror = 16000000, constant = 2;
static int32_t status = STA_UNSYNC, tai = 0;
static int64_t slew_total;                     /* ns still to slew when the slew began (signed) */
static uint64_t slew_start, slew_end;          /* monotonic ns; slew_end == 0: no slew */

static int64_t evaluate(uint64_t mono, int64_t real_base, uint64_t mono_base, int64_t mult) {
    int64_t delta = (int64_t)(mono - mono_base);
    __int128 product = (__int128)delta * mult;
    return real_base + delta + (int64_t)(product >> 32);
}
uint64_t timekeeping_realtime(uint64_t mono) {
    for (;;) {
        uint64_t s = __atomic_load_n(&line.seq, __ATOMIC_ACQUIRE);
        if (s & 1) { __asm__ volatile("pause"); continue; }
        int64_t rb = line.real_base, m = line.mult; uint64_t mb = line.mono_base;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&line.seq, __ATOMIC_RELAXED) != s) continue;
        int64_t value = evaluate(mono, rb, mb, m);
        return value < 0 ? 0 : (uint64_t)value;
    }
}
static int64_t base_mult(void) {
    int64_t ppm16 = freq + (tick - 10000) * 100 * 65536;   /* total frequency, ppm << 16 */
    int64_t mult = ppm16 * 65536 / 1000000;                 /* 2^-32 units */
    if (slew_end) mult += slew_total >= 0 ? SLEW_MULT : -SLEW_MULT;
    return mult;
}
/* tk_lock held: publish a new line through `at` (monotonic) with value `real` */
static void publish(int64_t real, uint64_t at, int64_t mult) {
    __atomic_store_n(&line.seq, line.seq + 1, __ATOMIC_RELEASE); __atomic_thread_fence(__ATOMIC_SEQ_CST);
    line.real_base = real; line.mono_base = at; line.mult = mult;
    __atomic_store_n(&line.seq, line.seq + 1, __ATOMIC_RELEASE);
    if (vdso_line) {
        __atomic_store_n(&vdso_line[0], vdso_line[0] + 1, __ATOMIC_RELEASE); __atomic_thread_fence(__ATOMIC_SEQ_CST);
        vdso_line[1] = (uint64_t)real; vdso_line[2] = at; vdso_line[3] = (uint64_t)mult;
        __atomic_store_n(&vdso_line[0], vdso_line[0] + 1, __ATOMIC_RELEASE);
    }
}
/* tk_lock held: keep the current time, apply the current rate */
static void rebase(void) {
    uint64_t now = native_clock_ns();
    publish(evaluate(now, line.real_base, line.mono_base, line.mult), now, base_mult());
}
void timekeeping_init(uint64_t epoch_seconds) {
    spin_acquire(&tk_lock); publish((int64_t)epoch_seconds * NSEC, 0, 0); spin_release(&tk_lock);
}
/* System sleep (kernel.c native_clock_resume): CLOCK_MONOTONIC stood still while the machine slept; CLOCK_REALTIME
 * steps forward by the time asleep (as the RTC measured it), keeping its rate and any slew in progress. */
void timekeeping_resume(uint64_t monotonic_ns, uint64_t slept_ns) {
    spin_acquire(&tk_lock);
    publish(evaluate(monotonic_ns, line.real_base, line.mono_base, line.mult) + (int64_t)slept_ns, monotonic_ns, line.mult);
    spin_release(&tk_lock);
    posix_clock_was_set();   /* Linux clock_was_set at resume: realtime moved, boottime timers too */
}
void timekeeping_vdso(uint64_t* fields) {
    spin_acquire(&tk_lock);
    vdso_line = fields; fields[0] = 0; fields[1] = (uint64_t)line.real_base; fields[2] = line.mono_base; fields[3] = (uint64_t)line.mult;
    spin_release(&tk_lock);
}
/* remaining slew in ns at monotonic `now` (tk_lock held) */
static int64_t slew_left(uint64_t now) {
    if (!slew_end) return 0;
    if (now >= slew_end) return 0;
    int64_t done = (int64_t)((__int128)(int64_t)(now - slew_start) * SLEW_MULT >> 32);
    return slew_total >= 0 ? slew_total - done : slew_total + done;
}
/* BSP device poll: end a finished slew exactly at its end time */
void timekeeping_poll(void) {
    if (!__atomic_load_n(&slew_end, __ATOMIC_RELAXED)) return;
    uint64_t now = native_clock_ns();
    if (now < slew_end) return;
    spin_acquire(&tk_lock);
    if (slew_end && now >= slew_end) {   /* the line continues from the slew's end point at the plain rate */
        uint64_t end = slew_end;
        int64_t at_end = evaluate(end, line.real_base, line.mono_base, line.mult);
        slew_end = 0; slew_total = 0;
        publish(at_end, end, base_mult());
    }
    spin_release(&tk_lock);
}
uint64_t timekeeping_next_event(void) { uint64_t end = __atomic_load_n(&slew_end, __ATOMIC_RELAXED); return end ? end : ~0ULL; }   /* tick.c */
/* tk_lock held: replace any slew in progress by one of ns (adjtime semantics) */
static void start_slew(int64_t ns) {
    if (ns > 2145LL * NSEC) ns = 2145LL * NSEC;
    if (ns < -2145LL * NSEC) ns = -2145LL * NSEC;
    uint64_t current = native_clock_ns();
    int64_t real = evaluate(current, line.real_base, line.mono_base, line.mult);
    slew_total = ns; slew_start = current; slew_end = 0;
    if (ns) {
        uint64_t magnitude = (uint64_t)(ns < 0 ? -ns : ns);
        uint64_t duration = magnitude / (uint64_t)SLEW_MULT << 32 | ((magnitude % (uint64_t)SLEW_MULT) << 32) / (uint64_t)SLEW_MULT;
        slew_end = current + (duration ? duration : 1);
    }
    publish(real, current, base_mult());
}
static void set_rtc(int64_t real) {
    int64_t seconds = real / NSEC, days = seconds / 86400, rest = seconds % 86400;
    rtc_time_t t; t.hour = (uint8_t)(rest / 3600); t.minute = (uint8_t)(rest % 3600 / 60); t.second = (uint8_t)(rest % 60);
    unsigned year = 1970;
    for (;;) { unsigned length = 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)); if (days < length) break; days -= length; year++; }
    static const unsigned months[] = {31,28,31,30,31,30,31,31,30,31,30,31}; unsigned month = 0;
    for (; month < 12; month++) {
        unsigned length = months[month] + (month == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (days < length) break;
        days -= length;
    }
    t.year = (uint16_t)year; t.month = (uint8_t)(month + 1); t.day = (uint8_t)(days + 1);
    rtc_set(&t);
}
static bool may_set(void) { return credentials_capable(credentials_current(), NULL, CAP_SYS_TIME); }
/* step CLOCK_REALTIME to `real` ns (validated by the caller) */
static void step_to(int64_t real) {
    spin_acquire(&tk_lock);
    uint64_t now = native_clock_ns();
    slew_end = 0; slew_total = 0; status |= STA_UNSYNC;   /* Linux ntp_clear() on a settime */
    publish(real, now, base_mult());
    spin_release(&tk_lock);
    posix_clock_was_set();   /* absolute CLOCK_REALTIME timers, TFD_TIMER_CANCEL_ON_SET (posix_timers.c) */
    set_rtc(real);
}
struct timex64 {
    uint32_t modes; int32_t pad0; int64_t offset, freq, maxerror, esterror; int32_t status, pad1;
    int64_t constant, precision, tolerance, time_sec, time_usec, tick, ppsfreq, jitter; int32_t shift, pad2;
    int64_t stabil, jitcnt, calcnt, errcnt, stbcnt; int32_t tai; int32_t reserved[11];
};
_Static_assert(sizeof(struct timex64) == 208, "struct timex ABI");
#define ADJ_OFFSET 0x0001
#define ADJ_FREQUENCY 0x0002
#define ADJ_MAXERROR 0x0004
#define ADJ_ESTERROR 0x0008
#define ADJ_STATUS 0x0010
#define ADJ_TIMECONST 0x0020
#define ADJ_TAI 0x0080
#define ADJ_SETOFFSET 0x0100
#define ADJ_MICRO 0x1000
#define ADJ_NANO 0x2000
#define ADJ_TICK 0x4000
#define ADJ_OFFSET_SINGLESHOT 0x8001
#define ADJ_OFFSET_SS_READ 0xA001
static int64_t adjtimex_call(uint64_t address) {
    if (!accessible(address, sizeof(struct timex64), true)) return -14;
    struct timex64 tx; copy_from_user(&tx, (void*)address, sizeof(tx));
    uint32_t modes = tx.modes;
    bool singleshot = modes == ADJ_OFFSET_SINGLESHOT || modes == ADJ_OFFSET_SS_READ;
    if (!singleshot && (modes & ~(uint32_t)(ADJ_OFFSET | ADJ_FREQUENCY | ADJ_MAXERROR | ADJ_ESTERROR | ADJ_STATUS | ADJ_TIMECONST |
                                            ADJ_TAI | ADJ_SETOFFSET | ADJ_MICRO | ADJ_NANO | ADJ_TICK))) return -22;
    if (modes && modes != ADJ_OFFSET_SS_READ && !may_set()) return -1;
    if ((modes & ADJ_TICK) && !singleshot && (tx.tick < 9000 || tx.tick > 11000)) return -22;
    if ((modes & ADJ_SETOFFSET) && !singleshot) {
        int64_t sub = tx.time_usec, scale = (modes & ADJ_NANO) ? 1 : 1000;
        if (sub < 0 || sub >= NSEC / scale) return -22;
        if (tx.time_sec > 100LL * 365 * 86400 || tx.time_sec < -100LL * 365 * 86400) return -22;
    }
    int64_t old_slew = 0;
    spin_acquire(&tk_lock);
    uint64_t now = native_clock_ns();
    old_slew = slew_left(now);
    if (singleshot) {
        if (modes == ADJ_OFFSET_SINGLESHOT) { int64_t us = tx.offset; if (us > 2145000000LL) us = 2145000000LL; if (us < -2145000000LL) us = -2145000000LL; start_slew(us * 1000); }
    } else {
        if (modes & ADJ_STATUS) status = (status & STA_RONLY) | (tx.status & ~STA_RONLY);
        if (modes & ADJ_NANO) status |= STA_NANO;
        if (modes & ADJ_MICRO) status &= ~STA_NANO;
        if (modes & ADJ_FREQUENCY) freq = tx.freq > MAXFREQ_SCALED ? MAXFREQ_SCALED : tx.freq < -MAXFREQ_SCALED ? -MAXFREQ_SCALED : tx.freq;
        if (modes & ADJ_MAXERROR) maxerror = tx.maxerror < 0 ? 0 : tx.maxerror > 16000000 ? 16000000 : tx.maxerror;
        if (modes & ADJ_ESTERROR) esterror = tx.esterror < 0 ? 0 : tx.esterror > 16000000 ? 16000000 : tx.esterror;
        if (modes & ADJ_TIMECONST) constant = tx.constant < 0 ? 0 : tx.constant > 10 ? 10 : tx.constant;
        if ((modes & ADJ_TAI) && tx.constant >= 0) tai = (int32_t)tx.constant;
        if (modes & ADJ_TICK) tick = tx.tick;
        if (modes & ADJ_OFFSET) {   /* PLL offset: slewed at 500 ppm, at most +-0.5 s (MAXPHASE) */
            int64_t ns = (status & STA_NANO) ? tx.offset : tx.offset * 1000;
            if (ns > 500000000LL) ns = 500000000LL;
            if (ns < -500000000LL) ns = -500000000LL;
            start_slew(ns);
        }
        if (modes & ADJ_SETOFFSET) {
            int64_t delta = tx.time_sec * NSEC + tx.time_usec * ((modes & ADJ_NANO) ? 1 : 1000);
            uint64_t at = native_clock_ns();
            publish(evaluate(at, line.real_base, line.mono_base, line.mult) + delta, at, line.mult);
        }
        if (modes & (ADJ_FREQUENCY | ADJ_TICK)) rebase();
    }
    now = native_clock_ns();
    int64_t real = evaluate(now, line.real_base, line.mono_base, line.mult), left = slew_left(now);
    tx.offset = singleshot ? old_slew / 1000 : (status & STA_NANO) ? left : left / 1000;
    tx.freq = freq; tx.maxerror = maxerror; tx.esterror = esterror; tx.status = status; tx.constant = constant;
    tx.precision = 1; tx.tolerance = MAXFREQ_SCALED; tx.time_sec = real / NSEC;
    tx.time_usec = (status & STA_NANO) ? real % NSEC : real % NSEC / 1000; tx.tick = tick;
    tx.ppsfreq = tx.jitter = tx.stabil = tx.jitcnt = tx.calcnt = tx.errcnt = tx.stbcnt = 0; tx.shift = 0; tx.tai = tai;
    int32_t state = (status & STA_UNSYNC) ? 5 : 0;   /* TIME_ERROR / TIME_OK */
    spin_release(&tk_lock);
    if ((modes & ADJ_SETOFFSET) && !singleshot) { posix_clock_was_set(); set_rtc(real); }
    if (!accessible(address, sizeof(tx), true)) return -14;
    if (copy_to_user((void*)address, &tx, sizeof(tx))) return -14;
    return state;
}
int32_t timekeeping_tai_seconds(void) { return tai; }   /* CLOCK_TAI - CLOCK_REALTIME (adjtimex ADJ_TAI) */
int64_t timekeeping_call(struct registers* r) {
    switch (r->rax) {
    case 227: {   /* clock_settime */
        if ((uint32_t)r->rdi != 0) return (uint32_t)r->rdi <= 11 ? -22 : -22;
        if (!accessible(r->rsi, 16, false)) return -14;
        int64_t ts[2]; copy_from_user(ts, (void*)r->rsi, sizeof(ts));
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= NSEC || ts[0] > 0x7FFFFFFFFFLL) return -22;
        if (!may_set()) return -1;
        step_to(ts[0] * NSEC + ts[1]); return 0;
    }
    case 164: {   /* settimeofday(tv, tz): the time zone is accepted and ignored (the kernel keeps UTC) */
        if (r->rsi && !accessible(r->rsi, 8, false)) return -14;
        if (!r->rdi) return r->rsi ? (may_set() ? 0 : -1) : 0;
        if (!accessible(r->rdi, 16, false)) return -14;
        int64_t tv[2]; copy_from_user(tv, (void*)r->rdi, sizeof(tv));
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000 || tv[0] > 0x7FFFFFFFFFLL) return -22;
        if (!may_set()) return -1;
        step_to(tv[0] * NSEC + tv[1] * 1000); return 0;
    }
    case 159: return adjtimex_call(r->rdi);
    case 305:     /* clock_adjtime: CLOCK_REALTIME only */
        if ((uint32_t)r->rdi != 0) return -95;
        return adjtimex_call(r->rsi);
    }
    return -38;
}
