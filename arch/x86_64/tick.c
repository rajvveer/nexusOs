/* Tickless idle (Linux NO_HZ_IDLE) for the native x86-64 kernel.
 *
 * The periodic ticks: the boot CPU (BSP) takes a 1 kHz tick that expires timed waiters and ITIMER_REAL alarms
 * (process.c trap64, device-vector path), preempts and drives the device polls of its scheduler pass; every
 * application processor (AP) takes a 250 Hz LAPIC tick (VEC_LAPIC_TIMER) for preemption only.
 *
 * Event devices of the boot CPU, chosen once in tick_init (boot option tick=tsc-deadline|lapic|pit forces one):
 *   TSC-deadline  local APIC timer in TSC-deadline mode (CPUID.01H:ECX[24]): periodic 1 kHz while busy, one deadline
 *                 (IA32_TSC_DEADLINE) at the next event while idle
 *   lapic         local APIC timer, divide 16, periodic / one-shot (calibrated against the TSC)
 *   pit           the 8254 channel 0 (legacy: no usable local APIC): mode 2 at 1 kHz, mode 0 one-shots of at most
 *                 54.9 ms while idle
 * With a local-APIC device the BSP's tick arrives on a dynamic IRQ_WAKE vector (irq.c): trap64 treats it as the
 * device interrupt the PIT was (timer expiry, preemption, signal delivery, restricted disk waits let it through) and
 * the PIT is shut down (IRQ0 masked). A self-test arms each local-APIC device once; a device that does not fire
 * within 20 ms is not used (TSC-deadline -> lapic -> pit).
 *
 * Idle (cpu_idle_enter, cpufreq.c):
 *   AP   its tick has no work while idle (wake-ups arrive as IPIs): it is stopped when the CPU expects to stay idle at
 *        least 500 us (recent idle periods, as Linux's menu governor) and started again only when the CPU leaves its
 *        idle loop to run a task.
 *   BSP  the device polls of its scheduler pass live on this CPU and other CPUs' tasks may hand it work (loopback
 *        packets, block completions, ...) that it used to pick up within 1 ms. So the boot CPU stops its tick only
 *        while every other online CPU is idle as well (a Dekker handshake: an AP leaving idle sets `busy` then reads
 *        `bsp_nohz` and kicks the boot CPU, the boot CPU sets `bsp_nohz` then reads every `busy`). It then programs
 *        one event at the earliest of: the next timed waiter / alarm (process.c next_deadline, which also carries
 *        every timer built on timed waits), what its last scheduler pass said its polls need next (tick_bsp_pass:
 *        network timers, sound DMA, USB, ACPI worker, power refresh, write-back, journal commit, adjtime slew, ...)
 *        and TICK_MAX_SLEEP_NS; only when that is at least 50 us away and recent idle periods were not tiny.
 *   Without ARAT (CPUID.06H:EAX[2]) the local APIC timer may stop in deep C-states: the boot CPU then stays in C1
 *        while its tick runs on the local APIC (Linux without a broadcast device does the same).
 * Suspend-to-idle and parked CPUs (pm.c) stop their ticks altogether.
 *
 * Observability: /proc/interrupts (LOC local timer, RES, TLB, PIT and device vectors per CPU), /proc/timer_list
 * (Linux's tick-sched fields: nohz_mode, tick_stopped, idle_calls, idle_sleeps, idle_sleeptime, ...),
 * /sys/devices/system/clockevents/clockevent<N>/current_device, /sys/devices/system/clocksource/clocksource0.
 * Boot options: nohz=off (periodic ticks always, the previous behaviour), tick=... (above). */
#include "native.h"
#include "power.h"
#include "boot_info.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define LVT_TIMER 0x320
#define TIMER_INITIAL 0x380
#define TIMER_CURRENT 0x390
#define TIMER_DIVIDE 0x3E0
#define MSR_TSC_DEADLINE 0x6E0
#define PIT_HZ 1193182ULL
#define TICK_HZ 1000ULL
#define TICK_STOP_MIN_NS 50000ULL         /* the boot CPU's next event this close: the periodic tick stays */
#define TICK_EARLY_SLACK_NS 2000ULL       /* an event this close to its time counts as on time */
#define TICK_ADVANCE_MAX_NS 1000000ULL    /* at most this much early arming for interrupt delivery latency */
#define TICK_SPIN_EXTRA_NS 100000ULL      /* an early event this close (beyond the advance) is waited out */
#define TICK_PREDICT_MIN_NS 500000ULL     /* recent idle periods shorter than this: keep the tick (IPI ping-pong) */
#define TICK_MAX_SLEEP_NS 1000000000ULL   /* safety net: the boot CPU passes its scheduler loop at least once a second */
#define TICK_POLLERS 16

enum { DEV_NONE, DEV_PIT, DEV_LAPIC, DEV_TSC_DEADLINE };
static const char* const device_names[] = {"none", "pit", "lapic", "lapic-deadline"};
struct tick_cpu {
    volatile bool busy;                   /* outside the idle loop (APs): the boot CPU keeps its tick */
    bool stopped, parked;                 /* periodic tick off (idle / parked) */
    uint64_t idle_calls, idle_sleeps;     /* idle entries; of them with the tick stopped */
    uint64_t idle_entry, idle_exit, sleep_ns, expires, last_stop;
    uint64_t local, resched, tlb, spurious, pit;   /* interrupts taken on this CPU */
    uint64_t irqs, events, mark_irqs, mark_events, early;   /* all vectors / own events; at the last arm; early events */
    uint64_t target, spun; bool precise;   /* armed time (expires - advance); remainders waited out; a task timer */
};
static struct tick_cpu tick_cpus[MAX_CPUS];
static unsigned device;                   /* the boot CPU's event device */
static int event_vector = -1;             /* local APIC timer vector of the boot CPU (IRQ_WAKE class) */
static int pit_vector = -1;
static uint64_t lapic_hz, tsc_khz;        /* local APIC timer ticks/s at divide 16; TSC kHz */
static bool enabled, arat, tsc_deadline, tsc_invariant;
static volatile int bsp_nohz;             /* 1: the boot CPU stops (or stopped) its tick: APs leaving idle kick it */
static uint64_t bsp_poll_due = 0;         /* tick_bsp_pass: when the boot CPU's polls need it next (0: at once) */
static uint64_t test_events;              /* self-test: event interrupts seen */
static uint32_t* vector_counts;           /* [cpu][vector - 32] interrupts per CPU (arena, from tick_init) */
static unsigned counted_cpus;
struct poller { const char* name; uint64_t (*next)(uint64_t now); };
static struct poller pollers[TICK_POLLERS];
static unsigned poller_count;

uint32_t lapic_reg_read(unsigned reg);                 /* smp.c: local APIC registers (xAPIC MMIO or x2APIC MSRs) */
void lapic_reg_write(unsigned reg, uint32_t value);
void smp_ap_tick(bool on);                             /* smp.c: this AP's 250 Hz tick on/off */
void smp_self_ipi(unsigned vector);                    /* smp.c */
uint64_t task_next_deadline(void);                     /* process.c: earliest timed waiter / alarm */
/* Subsystems polled by the boot CPU's scheduler pass: when they need it next (absolute ns; ~0: only on an interrupt). */
uint64_t native_network_next_event(uint64_t now);      /* network.c */
uint64_t sound_next_event(uint64_t now);               /* sound.c */
uint64_t usb_next_event(uint64_t now);                 /* usb.c */
uint64_t native_polls_next_event(uint64_t now);        /* kernel.c: write-back, journal, descriptor GC, traces */
uint64_t timekeeping_next_event(void);                 /* timekeeping.c: end of an adjtime slew */
uint64_t power_next_event(uint64_t now);               /* power.c: ACPI worker, power events, refresh, pm */
bool virtio_gpu_busy(void);

static uint64_t rdmsr64(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return (uint64_t)hi << 32 | lo; }
static void wrmsr64(uint32_t msr, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory"); }
static void cpuid(uint32_t leaf, uint32_t r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0)); }
static uint64_t raw_tsc(void) { uint32_t lo, hi; __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory"); return (uint64_t)hi << 32 | lo; }
static void say(const char* s) { vga_print(s); }
static void say_number(uint64_t v) { char t[24]; number64(v, t); vga_print(t); }

bool tick_nohz_enabled(void) { return enabled; }
void tick_register_poller(const char* name, uint64_t (*next)(uint64_t now)) {
    if (poller_count < TICK_POLLERS && next) pollers[poller_count++] = (struct poller){name, next};
}

/* ---- the boot CPU's event device ---- */
static void pit_periodic(void) { port_byte_out(0x43, 0x34); port_byte_out(0x40, (uint8_t)(PIT_HZ / TICK_HZ)); port_byte_out(0x40, (uint8_t)((PIT_HZ / TICK_HZ) >> 8)); }
static void pit_oneshot(uint64_t delta_ns) {
    uint64_t count = delta_ns / 1000 * PIT_HZ / 1000000; if (count < 2) count = 2; if (count > 0xFFFF) count = 0xFFFF;
    port_byte_out(0x43, 0x30); port_byte_out(0x40, (uint8_t)count); port_byte_out(0x40, (uint8_t)(count >> 8));
}
static void pit_shutdown(void) { port_byte_out(0x43, 0x30); port_byte_out(0x40, 0); port_byte_out(0x40, 0); }   /* as Linux: one last 55 ms count */
static uint32_t lapic_count(uint64_t delta_ns) {
    uint64_t count = delta_ns * (lapic_hz / 1000) / 1000000ULL;   /* delta <= TICK_MAX_SLEEP_NS: no overflow */
    return count < 1 ? 1 : count > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)count;
}
/* TSC-deadline mode keeps its LVT; its 1 kHz tick is a deadline re-armed by each tick interrupt (event_irq). */
static volatile bool deadline_ticking; static uint64_t deadline_next;
static bool deadline_lvt;   /* the LVT holds TSC-deadline mode (cleared when anything else writes it or power was lost) */
static void deadline_mode(void) {
    if (deadline_lvt) return;
    lapic_reg_write(LVT_TIMER, (2U << 17) | (uint32_t)event_vector); deadline_lvt = true;
    __asm__ volatile("mfence" ::: "memory");   /* the mode switch before any deadline write (SDM 10.5.4.1) */
}
static void bsp_periodic(void) {
    switch (device) {
    case DEV_TSC_DEADLINE:
        deadline_mode(); deadline_next = raw_tsc() + tsc_khz; deadline_ticking = true;
        wrmsr64(MSR_TSC_DEADLINE, deadline_next); break;
    case DEV_LAPIC:
        lapic_reg_write(LVT_TIMER, (1U << 17) | (uint32_t)event_vector); lapic_reg_write(TIMER_DIVIDE, 3);
        lapic_reg_write(TIMER_INITIAL, (uint32_t)(lapic_hz / TICK_HZ)); break;
    case DEV_PIT: pit_periodic(); break;
    }
}
static void bsp_arm(uint64_t delta_ns) {
    if (delta_ns > TICK_MAX_SLEEP_NS) delta_ns = TICK_MAX_SLEEP_NS;
    switch (device) {
    case DEV_TSC_DEADLINE:
        deadline_mode(); deadline_ticking = false;
        wrmsr64(MSR_TSC_DEADLINE, raw_tsc() + delta_ns * tsc_khz / 1000000ULL); break;
    case DEV_LAPIC:
        lapic_reg_write(LVT_TIMER, (uint32_t)event_vector); lapic_reg_write(TIMER_DIVIDE, 3); lapic_reg_write(TIMER_INITIAL, lapic_count(delta_ns)); break;
    case DEV_PIT: pit_oneshot(delta_ns); break;
    }
}
/* One-shot events: how late they arrived after their timer's time (late_*), and the interrupt delivery latency
 * after the armed time that the next one-shots compensate (advance_ns: 3/4 of its running average, at most 1 ms -
 * microseconds on hardware, about a millisecond for QEMU on a Windows host that rounds its timers to milliseconds). */
static uint64_t late_sum, late_count, late_max, advance_ns;
static bool self_event;   /* the next event is the CPU's own (tick_idle_exit): not a delivery-latency sample */
static void event_irq(unsigned vector, void* context) {
    (void)vector; (void)context; test_events++;
    struct tick_cpu* t = &tick_cpus[0];
    if (t->stopped && t->expires != ~0ULL) {
        uint64_t now = native_clock_ns();
        if (now >= t->expires) { uint64_t late = now - t->expires; late_sum += late; late_count++; if (late > late_max) late_max = late; }
        if (!self_event && now >= t->target) {
            uint64_t raw = now - t->target, scaled = raw * 3 / 4;
            advance_ns = (advance_ns * 7 + (scaled > TICK_ADVANCE_MAX_NS ? TICK_ADVANCE_MAX_NS : scaled)) / 8;
        }
        self_event = false;
    }
    if (device == DEV_TSC_DEADLINE && deadline_ticking) {   /* the next 1 ms period, skipping missed ones */
        uint64_t now = raw_tsc(); deadline_next += tsc_khz;
        if (deadline_next <= now) deadline_next = now + tsc_khz;
        wrmsr64(MSR_TSC_DEADLINE, deadline_next);
    }
}
static void pit_irq(unsigned vector, void* context) { (void)vector; (void)context; }
/* Arm one event 400 us ahead with interrupts open; it must arrive within 20 ms. */
static bool device_works(unsigned candidate) {
    unsigned saved = device; device = candidate;
    uint64_t before = test_events, start = native_clock_ns();
    bsp_arm(400000ULL);
    while (test_events == before && native_clock_ns() - start < 20000000ULL) __asm__ volatile("sti; hlt; cli" ::: "memory");
    bool fired = test_events != before; uint64_t took = native_clock_ns() - start;
    if (candidate == DEV_TSC_DEADLINE) wrmsr64(MSR_TSC_DEADLINE, 0); else lapic_reg_write(TIMER_INITIAL, 0);
    if (!fired || took < 350000ULL) {
        say("[64] TICK: "); say(device_names[candidate]); say(fired ? " fired early" : " never fired"); say(" in the self-test; not used\n");
        device = saved; return false;
    }
    return true;
}
/* The local APIC timer's rate at divide 16: the bus clock a known hypervisor reports (CPUID 0x40000010 EBX, kHz), else
 * measured against the TSC over 25 ms with each counter read bracketed by TSC reads (a read pair the host interrupted
 * is taken again). One-shots that still end early are re-armed (tick_idle_exit); a rate error makes them late by
 * that fraction of the sleep (25 ms with ~1 us brackets: ~0.01%). */
static const char* lapic_source = "measured";
/* The tightest of 32 bracketed reads (each read of the counter is a VM exit under a hypervisor): its bracket in TSC ticks. */
static uint64_t lapic_sample(uint64_t* tsc, uint32_t* count) {
    uint64_t best = ~0ULL;
    for (unsigned tries = 0; tries < 32; tries++) {
        uint64_t before = raw_tsc(); uint32_t value = lapic_reg_read(TIMER_CURRENT); uint64_t after = raw_tsc();
        if (after - before < best) { best = after - before; *tsc = before + best / 2; *count = value; }
    }
    return best;
}
static bool lapic_calibrate(void) {
    uint32_t r[4]; cpuid(1, r);
    if (r[2] & (1U << 31)) {   /* VMware / KVM report the virtual bus clock that drives the local APIC timer */
        cpuid(0x40000000U, r);
        bool known = (r[1] == 0x61774D56 && r[2] == 0x4D566572 && r[3] == 0x65726177) || (r[1] == 0x4B4D564B && r[2] == 0x564B4D56 && r[3] == 0x0000004D);
        if (known && r[0] >= 0x40000010U) { cpuid(0x40000010U, r); if (r[1] >= 1000 && r[1] <= 10000000) { lapic_hz = (uint64_t)r[1] * 1000 / 16; lapic_source = "hypervisor"; return true; } }
    }
    if (!tsc_khz) return false;
    deadline_lvt = false;
    lapic_reg_write(TIMER_DIVIDE, 3); lapic_reg_write(LVT_TIMER, 1U << 16); lapic_reg_write(TIMER_INITIAL, 0xFFFFFFFFU);
    uint64_t t0 = 0, t1 = 0; uint32_t c0 = 0, c1 = 0;
    uint64_t bracket = lapic_sample(&t0, &c0);
    /* 25 ms when a read pair takes <= 2 us (bare metal), else long enough for a 0.01% error, at most 200 ms */
    uint64_t window = bracket <= tsc_khz / 500 ? 25000000ULL : bracket * 1000000ULL / tsc_khz * 10000ULL;
    if (window > 200000000ULL) window = 200000000ULL;
    uint64_t start = native_clock_ns(); while (native_clock_ns() - start < window) __asm__ volatile("pause");
    bracket += lapic_sample(&t1, &c1);
    lapic_reg_write(TIMER_INITIAL, 0);
    if (c1 >= c0 || t1 <= t0 || bracket > (t1 - t0) / 1000) return false;   /* reads too slow for 0.1% */
    lapic_hz = (uint64_t)(c0 - c1) * (tsc_khz * 1000) / (t1 - t0);
    return lapic_hz >= 1000000ULL && lapic_hz <= 10000000000ULL;
}

void tick_init(void) {
    enabled = !boot_option("nohz=off");
    uint32_t r[4]; cpuid(0, r); uint32_t max = r[0];
    cpuid(1, r); bool lapic = (r[3] >> 9) & 1; tsc_deadline = (r[2] >> 24) & 1;
    if (max >= 6) { cpuid(6, r); arat = (r[0] >> 2) & 1; }
    cpuid(0x80000000U, r); if (r[0] >= 0x80000007U) { cpuid(0x80000007U, r); tsc_invariant = (r[3] >> 8) & 1; }
    uint64_t apic_base = lapic ? rdmsr64(0x1B) : 0;
    lapic = lapic && (apic_base & (1ULL << 11));
    {   /* the TSC rate behind native_clock_ns (kernel.c's calibration), read back exactly over 2 ms */
        uint64_t n0 = native_clock_ns(), t0 = raw_tsc(), n1, t1;
        do { n1 = native_clock_ns(); t1 = raw_tsc(); } while (n1 - n0 < 2000000ULL);
        tsc_khz = (t1 - t0) * 1000000ULL / (n1 - n0);
    }
    counted_cpus = cpu_count < MAX_CPUS ? cpu_count : MAX_CPUS;
    uint64_t table = arena_alloc(((uint64_t)counted_cpus * 224 * sizeof(uint32_t) + 4095) & ~4095ULL);
    if (table) { memset((void*)table, 0, (uint64_t)counted_cpus * 224 * sizeof(uint32_t)); vector_counts = (uint32_t*)table; }
    for (unsigned i = 1; i < MAX_CPUS; i++) tick_cpus[i].busy = true;
    pit_vector = irq_attach_isa(0, IRQ_WAKE, pit_irq, NULL);
    bool want_pit = boot_option("tick=pit"), want_lapic = boot_option("tick=lapic");
    device = DEV_PIT;
    if (lapic && !want_pit && (event_vector = irq_alloc_vector(IRQ_WAKE)) >= 0 && !irq_register((unsigned)event_vector, event_irq, NULL)) {
        uint64_t flags; __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        if (tsc_deadline && !want_lapic && tsc_khz && device_works(DEV_TSC_DEADLINE)) device = DEV_TSC_DEADLINE;   /* needs no LAPIC rate */
        else if (!lapic_calibrate()) say("[64] TICK: local APIC timer rate could not be measured (counter reads too slow); PIT used\n");
        else if (device_works(DEV_LAPIC)) device = DEV_LAPIC;
        if (device != DEV_PIT) {
            bsp_periodic();                       /* the boot CPU's 1 kHz tick moves to the local APIC timer */
            irq_mask(0); pit_shutdown();          /* the PIT is quiet from here on */
        }
        if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
    }
    if (device == DEV_PIT && event_vector >= 0) { irq_free_vector((unsigned)event_vector); event_vector = -1; }
    say("[64] TICK: "); say(enabled ? "nohz idle" : "periodic (nohz=off)"); say(", boot CPU event device "); say(device_names[device]);
    if (device == DEV_LAPIC) { say(" (vector "); say_number((uint64_t)event_vector); say(", LAPIC "); say_number(lapic_hz / 1000); say(" kHz "); say(lapic_source); say(")"); }
    if (device == DEV_TSC_DEADLINE) { say(" (vector "); say_number((uint64_t)event_vector); say(", TSC "); say_number(tsc_khz); say(" kHz)"); }
    say(arat ? ", ARAT" : ", no ARAT"); say(tsc_invariant ? ", invariant TSC" : "");
    say(cpu_count > 1 ? "; application processors stop their 250 Hz tick when idle\n" : "\n");
}
/* After S3 / a hibernation restore: the platform lost the timers' state (pm.c platform_resume). */
void tick_resume(void) {
    for (unsigned i = 0; i < MAX_CPUS; i++) { tick_cpus[i].stopped = tick_cpus[i].parked = false; tick_cpus[i].busy = i != 0; }
    __atomic_store_n(&bsp_nohz, 0, __ATOMIC_SEQ_CST);
    if (device == DEV_PIT) { pit_periodic(); return; }
    deadline_lvt = false;   /* the local APIC was reset */
    pit_shutdown(); bsp_periodic();
}

/* ---- idle ---- */
static bool others_idle(void) {
    for (unsigned i = 1; i < MAX_CPUS; i++)
        if (cpus[i].online && __atomic_load_n(&tick_cpus[i].busy, __ATOMIC_SEQ_CST)) return false;
    return true;
}
/* cpu_idle_enter, interrupts off: maybe stop this CPU's tick. Returns the time to this CPU's next timer event
 * (~0 when it has none) for the C-state choice. */
uint64_t tick_idle_enter(uint64_t predicted_ns) {
    struct cpu* c = cpu_self(); unsigned i = c->index; struct tick_cpu* t = &tick_cpus[i];
    uint64_t now = native_clock_ns();
    t->idle_calls++; t->idle_entry = now;
    if (i) {
        __atomic_store_n(&t->busy, false, __ATOMIC_SEQ_CST);
        if (!enabled || t->stopped) return t->stopped ? ~0ULL : 4000000ULL;
        if (predicted_ns < TICK_PREDICT_MIN_NS) return 4000000ULL;
        smp_ap_tick(false); t->stopped = true; t->idle_sleeps++; t->last_stop = now; t->expires = ~0ULL;
        return ~0ULL;
    }
    uint64_t due = task_next_deadline();
    if (bsp_poll_due < due) due = bsp_poll_due;
    uint64_t until_tick = due > now && due - now < 1000000ULL ? due - now : 1000000ULL;
    if (!enabled || device == DEV_NONE || predicted_ns < TICK_PREDICT_MIN_NS || due < now + TICK_STOP_MIN_NS) return until_tick;
    __atomic_store_n(&bsp_nohz, 1, __ATOMIC_SEQ_CST);
    if (!others_idle()) { __atomic_store_n(&bsp_nohz, 0, __ATOMIC_SEQ_CST); return until_tick; }
    uint64_t delta = due - now; if (delta > TICK_MAX_SLEEP_NS) delta = TICK_MAX_SLEEP_NS;
    /* a task's timer (not a housekeeping poll): armed `advance` early, the rest waited out (tick_idle_exit) */
    t->precise = due == task_next_deadline() && device != DEV_PIT && delta == due - now;
    uint64_t lead = t->precise && delta > advance_ns + TICK_STOP_MIN_NS ? advance_ns : 0;
    bsp_arm(delta - lead);
    t->stopped = true; t->idle_sleeps++; t->last_stop = now; t->expires = now + delta; t->target = t->expires - lead;
    t->mark_irqs = t->irqs; t->mark_events = t->events;
    return delta;
}
/* cpu_idle_enter after the halt, interrupts off. True (boot CPU): nothing but its own event interrupted the halt and
 * the event came before its time - the local APIC timer counts at a rate calibrated against the TSC (TSC-deadline
 * does not), the PIT counts at most 54.9 ms: the rest is armed again and the CPU halts on without a scheduler pass. */
bool tick_idle_exit(void) {
    struct cpu* c = cpu_self(); unsigned i = c->index; struct tick_cpu* t = &tick_cpus[i];
    uint64_t now = native_clock_ns(); t->idle_exit = now;
    if (i) {
        if (!__atomic_load_n(&c->wake_pending, __ATOMIC_ACQUIRE)) return false;   /* still idle: the loop halts again */
        if (t->stopped) { smp_ap_tick(true); t->stopped = false; t->sleep_ns += now - t->last_stop; }
        __atomic_store_n(&t->busy, true, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&bsp_nohz, __ATOMIC_SEQ_CST) && cpus[0].online) smp_send_ipi(0, VEC_WAKE);
        return false;
    }
    if (!t->stopped) return false;
    uint64_t others = (t->irqs - t->mark_irqs) - (t->events - t->mark_events);
    if (!others && t->events != t->mark_events && now + TICK_EARLY_SLACK_NS < t->expires && !__atomic_load_n(&c->wake_pending, __ATOMIC_ACQUIRE)) {
        if (t->precise && t->expires - now <= advance_ns + TICK_SPIN_EXTRA_NS) {
            /* the event came within the delivery allowance: wait the rest out with interrupts open, then deliver the
             * event to this CPU itself, so trap64 expires the timer on time (KVM's lapic_timer_advance, guest side) */
            uint64_t irqs = t->irqs;
            while (native_clock_ns() < t->expires && t->irqs == irqs) __asm__ volatile("sti; pause; cli" ::: "memory");
            if (t->irqs == irqs) {
                self_event = true; smp_self_ipi((unsigned)event_vector); t->spun++;
                t->mark_irqs = t->irqs; t->mark_events = t->events; t->target = t->expires;
                return true;   /* the halt takes the event at once */
            }
            /* another interrupt arrived meanwhile: a normal exit */
        } else {
            uint64_t left = t->expires - now, lead = t->precise && left > advance_ns + TICK_STOP_MIN_NS ? advance_ns : 0;
            bsp_arm(left - lead); t->early++; t->target = t->expires - lead;
            t->mark_irqs = t->irqs; t->mark_events = t->events;
            return true;
        }
    }
    bsp_periodic();
    t->stopped = false; t->sleep_ns += now - t->last_stop;
    __atomic_store_n(&bsp_nohz, 0, __ATOMIC_SEQ_CST);
    return false;
}
/* The local APIC timer may stop in C-states deeper than C1 without ARAT. */
bool tick_deep_idle_allowed(void) { return arat || device == DEV_PIT || cpu_self()->index != 0; }   /* APs: no timer duty while idle */
/* Boot CPU scheduler pass (big lock held), after its polls: when they need the boot CPU again. */
void tick_bsp_pass(bool work_pending) {
    uint64_t now = native_clock_ns(), due = work_pending ? now : ~0ULL, at;
#define TAKE(x) do { at = (x); if (at < due) due = at; } while (0)
    TAKE(native_network_next_event(now));
    TAKE(sound_next_event(now));
    TAKE(usb_next_event(now));
    TAKE(native_polls_next_event(now));
    TAKE(timekeeping_next_event());
    TAKE(power_next_event(now));
    if (virtio_gpu_busy()) TAKE(now + 1000000ULL);
    for (unsigned k = 0; k < poller_count; k++) TAKE(pollers[k].next(now));
#undef TAKE
    bsp_poll_due = due;
}
/* pm.c: an application processor parks for a system sleep (its tick off), or leaves the park. */
void tick_cpu_park(bool parked) {
    unsigned i = cpu_self()->index; struct tick_cpu* t = &tick_cpus[i];
    if (!i) return;
    if (parked && !t->parked) {
        __atomic_store_n(&t->busy, false, __ATOMIC_SEQ_CST);
        if (!t->stopped) { smp_ap_tick(false); t->stopped = true; t->last_stop = native_clock_ns(); }
        t->parked = true;
    } else if (!parked && t->parked) {
        t->parked = false;
        if (t->stopped) { smp_ap_tick(true); t->stopped = false; t->sleep_ns += native_clock_ns() - t->last_stop; }
        __atomic_store_n(&t->busy, true, __ATOMIC_SEQ_CST);
    }
}
/* pm.c suspend-to-idle: the boot CPU halts until a wake event with no tick at all (tasks are frozen). */
void tick_s2idle(bool entering) {
    struct tick_cpu* t = &tick_cpus[0];
    if (entering) {
        if (device == DEV_PIT) { pit_shutdown(); irq_mask(0); }
        else if (device == DEV_TSC_DEADLINE) { deadline_ticking = false; wrmsr64(MSR_TSC_DEADLINE, 0); }
        else { lapic_reg_write(TIMER_INITIAL, 0); lapic_reg_write(LVT_TIMER, (1U << 16) | (uint32_t)event_vector); }
        t->stopped = true; t->last_stop = native_clock_ns(); t->expires = ~0ULL;   /* tick_s2idle_timer events: no latency samples */
    } else {
        if (device == DEV_PIT) { pit_periodic(); irq_unmask(0); }
        bsp_periodic();
        t->stopped = false; t->sleep_ns += native_clock_ns() - t->last_stop;
    }
}
/* pm.c suspend-to-idle: one event delta_ns ahead (an armed RTC wake alarm; at most 1 s, the loop re-arms). */
void tick_s2idle_timer(uint64_t delta_ns) {
    if (device == DEV_PIT) irq_unmask(0);   /* tick_s2idle masked it; a mode-0 one-shot interrupts once */
    bsp_arm(delta_ns < TICK_STOP_MIN_NS ? TICK_STOP_MIN_NS : delta_ns);
}

/* ---- interrupt accounting: trap64 calls this for every vector >= 32 except system calls ---- */
void tick_trap_count(unsigned vector) {
    unsigned i = cpu_self()->index;
    struct tick_cpu* t = &tick_cpus[i < MAX_CPUS ? i : 0];
    t->irqs++;
    if ((event_vector >= 0 && vector == (unsigned)event_vector) || (device == DEV_PIT && pit_vector >= 0 && vector == (unsigned)pit_vector)) t->events++;
    if (vector == VEC_LAPIC_TIMER || (event_vector >= 0 && vector == (unsigned)event_vector)) t->local++;
    else if (vector == VEC_WAKE) t->resched++;
    else if (vector == VEC_TLB) t->tlb++;
    else if (vector == VEC_SPURIOUS) t->spurious++;
    else if (pit_vector >= 0 && vector == (unsigned)pit_vector) t->pit++;
    if (vector_counts && i < counted_cpus && vector >= 32 && vector < 256) vector_counts[i * 224 + vector - 32]++;
}

/* ---- /proc/interrupts, /proc/timer_list ---- */
struct text { char* data; unsigned used, capacity; };
static void put(struct text* o, const char* s) { while (*s && o->used + 1 < o->capacity) o->data[o->used++] = *s++; if (o->capacity) o->data[o->used] = 0; }
static void put_number(struct text* o, uint64_t v) { char t[24]; number64(v, t); put(o, t); }
static void put_padded(struct text* o, uint64_t v, unsigned width) {
    char t[24]; number64(v, t); for (unsigned n = strlen(t); n < width; n++) put(o, " ");
    put(o, t);
}
static unsigned shown_cpus(void) { unsigned n = 1; for (unsigned i = 0; i < MAX_CPUS; i++) if (cpus[i].online) n = i + 1; return n; }
static void row(struct text* o, const char* label, const uint64_t* values, unsigned cpus_shown, const char* name) {
    unsigned n = strlen(label); for (; n < 4; n++) put(o, " ");
    put(o, label);
    for (unsigned c = 0; c < cpus_shown; c++) { put(o, " "); put_padded(o, values[c], 10); }
    put(o, "   "); put(o, name); put(o, "\n");
}
unsigned tick_proc_interrupts(char* out, unsigned capacity) {
    struct text o = {out, 0, capacity}; unsigned n = shown_cpus(); uint64_t v[MAX_CPUS];
    if (capacity) out[0] = 0;
    put(&o, "    "); for (unsigned c = 0; c < n; c++) { put(&o, "       CPU"); put_number(&o, c); if (c < 10) put(&o, " "); }
    put(&o, "\n");
    for (unsigned c = 0; c < n; c++) v[c] = tick_cpus[c].pit;
    row(&o, "0:", v, n, irq_ioapic_mode() ? "IO-APIC    2-edge      timer" : "XT-PIC     0-edge      timer");
    if (vector_counts)   /* device vectors (ISA lines, IOAPIC GSIs, MSI/MSI-X): numbered by vector */
        for (unsigned vector = 32; vector < 256; vector++) {
            if (vector == VEC_LAPIC_TIMER || vector == VEC_WAKE || vector == VEC_TLB || vector == VEC_SPURIOUS || vector == 128 ||
                (event_vector >= 0 && vector == (unsigned)event_vector) || (pit_vector >= 0 && vector == (unsigned)pit_vector)) continue;
            uint64_t total = 0;
            for (unsigned c = 0; c < n && c < counted_cpus; c++) total += v[c] = vector_counts[c * 224 + vector - 32];
            if (!total) continue;
            for (unsigned c = counted_cpus; c < n; c++) v[c] = 0;
            char label[8]; number64(vector, label); strcat(label, ":");
            row(&o, label, v, n, vector >= 0x90 && vector < 0xA0 ? "IO-APIC    edge        isa" : vector >= 0x20 && vector < 0x30 ? "XT-PIC     edge        isa" : "PCI-MSI/IO-APIC         device");
        }
    for (unsigned c = 0; c < n; c++) v[c] = tick_cpus[c].local;
    row(&o, "LOC:", v, n, "Local timer interrupts");
    for (unsigned c = 0; c < n; c++) v[c] = tick_cpus[c].spurious;
    row(&o, "SPU:", v, n, "Spurious interrupts");
    for (unsigned c = 0; c < n; c++) v[c] = tick_cpus[c].resched;
    row(&o, "RES:", v, n, "Rescheduling interrupts");
    for (unsigned c = 0; c < n; c++) v[c] = tick_cpus[c].tlb;
    row(&o, "TLB:", v, n, "TLB shootdowns");
    put(&o, "ERR:          0\nMIS:          0\n");
    return o.used;
}
static void field(struct text* o, const char* name, uint64_t value, const char* unit) {
    put(o, "  ."); put(o, name); for (unsigned n = strlen(name); n < 15; n++) put(o, " ");
    put(o, ": "); put_number(o, value); if (unit) { put(o, " "); put(o, unit); } put(o, "\n");
}
unsigned tick_proc_timer_list(char* out, unsigned capacity) {
    struct text o = {out, 0, capacity}; uint64_t now = native_clock_ns(); unsigned n = shown_cpus();
    if (capacity) out[0] = 0;
    put(&o, "Timer List Version: v0.9\nHRTIMER_MAX_CLOCK_BASES: 8\nnow at "); put_number(&o, now); put(&o, " nsecs\n\n");
    uint64_t next = task_next_deadline();
    for (unsigned c = 0; c < n; c++) {
        const struct tick_cpu* t = &tick_cpus[c];
        put(&o, "cpu: "); put_number(&o, c); put(&o, "\n");
        field(&o, "nohz_mode", !enabled ? 0 : device == DEV_PIT && !c ? 1 : 2, NULL);
        field(&o, "tick_stopped", t->stopped, NULL);
        field(&o, "idle_active", c ? !t->busy : cpus[c].idle, NULL);
        field(&o, "idle_calls", t->idle_calls, NULL);
        field(&o, "idle_sleeps", t->idle_sleeps, NULL);
        field(&o, "idle_entrytime", t->idle_entry, "nsecs");
        field(&o, "idle_exittime", t->idle_exit, "nsecs");
        field(&o, "idle_sleeptime", t->sleep_ns + (t->stopped && now > t->last_stop ? now - t->last_stop : 0), "nsecs");
        field(&o, "iowait_sleeptime", 0, "nsecs");
        field(&o, "last_tick", t->last_stop, "nsecs");
        field(&o, "idle_expires", t->stopped ? t->expires : 0, "nsecs");
        if (!c) { field(&o, "next_timer", next == ~0ULL ? 0x7FFFFFFFFFFFFFFFULL : next, "nsecs"); field(&o, "next_poll", bsp_poll_due == ~0ULL ? 0x7FFFFFFFFFFFFFFFULL : bsp_poll_due, "nsecs"); }
        field(&o, "wakeups", t->idle_calls, NULL);
        if (!c) {
            field(&o, "early_events", t->early, NULL);
            field(&o, "event_late_avg", late_count ? late_sum / late_count : 0, "nsecs");
            field(&o, "event_late_max", late_max, "nsecs");
            field(&o, "event_advance", advance_ns, "nsecs");
            field(&o, "events_waited_out", t->spun, NULL);
        }
        field(&o, "local_timer_irqs", t->local + (c ? 0 : t->pit), NULL);
        put(&o, "\n");
    }
    for (unsigned c = 0; c < n; c++) {
        put(&o, "Tick Device: mode:     1\nPer CPU device: "); put_number(&o, c); put(&o, "\nClock Event Device: ");
        put(&o, c ? "lapic" : device_names[device]); put(&o, "\n max_delta_ns:   ");
        put_number(&o, c ? 0 : device == DEV_PIT ? 54925438ULL : TICK_MAX_SLEEP_NS); put(&o, "\n min_delta_ns:   ");
        put_number(&o, c ? 0 : device == DEV_PIT ? 1676ULL : 1000ULL);
        put(&o, "\n mode:           "); put_number(&o, tick_cpus[c].stopped ? (c ? 1 : 3) : 2);   /* 1 shutdown, 2 periodic, 3 oneshot */
        put(&o, "\n event_handler:  "); put(&o, enabled ? "hrtimer_interrupt" : "tick_handle_periodic"); put(&o, "\n\n");
    }
    return o.used;
}
/* /sys/devices/system/clockevents, /sys/devices/system/clocksource (power.c CLASS_TICK) */
int64_t tick_sysfs_read(unsigned instance, unsigned attribute, char* out, unsigned capacity) {
    struct text o = {out, 0, capacity}; if (capacity) out[0] = 0;
    if (attribute == 0) { if (instance >= MAX_CPUS || (instance && !cpus[instance].online)) return -2; put(&o, instance ? "lapic" : device_names[device]); }
    else if (attribute == 1) put(&o, "tsc");
    else if (attribute == 2) put(&o, "tsc hpet acpi_pm");
    else if (attribute == 3) put(&o, enabled ? "1" : "0");
    else return -2;
    put(&o, "\n"); return o.used;
}
