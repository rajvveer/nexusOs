/* Symmetric multiprocessing: application-processor bring-up, the big kernel lock, IPIs and TLB
 * shootdown. Device IRQs stay on the BSP through the legacy PIC (virtual-wire LINT0); application
 * processors get a local-APIC timer for preemption and are woken by IPIs. */
#include "native.h"
#include "acpi.h"
#include "apic.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define LAPIC_BASE 0xFEE00000ULL
#define LAPIC_ID 0x20
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xB0
#define LAPIC_SVR 0xF0
#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE 0x3E0
#define TRAMPOLINE 0x7000ULL
#define AP_HZ 250
#define AP_STACK_BYTES (16 * 4096ULL)
#define AP_IST_BYTES (4 * 4096ULL)

struct cpu cpus[MAX_CPUS];
unsigned cpu_count = 1;
uint64_t smp_kernel_root;
extern uint8_t ap_trampoline[], ap_trampoline_end[], ap_gdtr[], ap_cr3[], ap_entry[];
extern uint64_t ap_stacks[MAX_CPUS], gdt64[];
extern void ap_start64(void);

static inline uint32_t lapic_read(unsigned reg) { return apic_read(reg); }              /* xAPIC or x2APIC (apic.c) */
static inline void lapic_write(unsigned reg, uint32_t value) { apic_write(reg, value); }
/* For power2's tick.c (LAPIC timer one-shot/TSC deadline): keep these over apic.c so they work in x2APIC mode. */
uint32_t lapic_reg_read(unsigned reg) { return apic_read(reg); }
void lapic_reg_write(unsigned reg, uint32_t value) { apic_write(reg, value); }
static inline void pause(void) { __asm__ volatile("pause" ::: "memory"); }
void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }
static uint32_t timer_period; /* LAPIC timer ticks per AP_HZ period (divide by 16) */

/* ---- the big kernel lock ---- */
static volatile int lock_owner = -1;
static volatile int lock_urgent = -1;   /* a CPU resuming the block I/O owner (blockio.c): the others let it in first */
bool kernel_lock_held(void) { return lock_owner == (int)cpu_self()->index; }
void kernel_lock_acquire(void) {
    int me = (int)cpu_self()->index, free = -1;
    bool urgent = block_io_owner_self();   /* its disk wait ended: syscall-heavy CPUs must not starve it */
    if (urgent) __atomic_store_n(&lock_urgent, me, __ATOMIC_RELAXED);
    for (;;) {
        int first = __atomic_load_n(&lock_urgent, __ATOMIC_RELAXED);
        if ((first < 0 || first == me) && __atomic_compare_exchange_n(&lock_owner, &free, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        free = -1;
        /* Spin with interrupts open: the holder may be waiting for this CPU's TLB acknowledgement. */
        do __asm__ volatile("sti; pause; cli" ::: "memory");
        while (__atomic_load_n(&lock_owner, __ATOMIC_RELAXED) != -1 ||
               ((first = __atomic_load_n(&lock_urgent, __ATOMIC_RELAXED)) >= 0 && first != me));
    }
    if (urgent || __atomic_load_n(&lock_urgent, __ATOMIC_RELAXED) == me) __atomic_store_n(&lock_urgent, -1, __ATOMIC_RELAXED);
    task_context_reload(); /* kernel globals now describe this CPU's task */
    space_lock_task();     /* and its address space may be changed by this CPU only */
}
void kernel_lock_release(void) {
    if (lock_owner != (int)cpu_self()->index) panic("kernel lock released by a non-owner");
    block_io_unlocking();   /* outside a disk wait: the task's block I/O sequence is over (blockio.c) */
    space_unlock_held();
    __atomic_store_n(&lock_owner, -1, __ATOMIC_RELEASE);
}

/* ---- small spinlocks ---- */
static unsigned spin_depth[MAX_CPUS];   /* spinlocks this CPU holds (blockio.c: a disk wait may sleep only without others) */
void spin_acquire(volatile int* lock) {
    int me = (int)cpu_self()->index + 1, free = 0;
    if (*lock == me) panic("spinlock recursion");
    while (!__atomic_compare_exchange_n(lock, &free, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        free = 0;
        /* The holder may be waiting for this CPU to acknowledge a shootdown: answer it here. */
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) { smp_tlb_poll(); pause(); }
    }
    spin_depth[me - 1]++;
}
bool spin_try_acquire(volatile int* lock) {
    int me = (int)cpu_self()->index + 1, free = 0;
    if (!__atomic_compare_exchange_n(lock, &free, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    spin_depth[me - 1]++; return true;
}
void spin_release(volatile int* lock) {
    if (*lock != (int)cpu_self()->index + 1) panic("spinlock released by a non-owner");
    spin_depth[cpu_self()->index]--;
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
unsigned spin_held_count(void) { return spin_depth[cpu_self()->index]; }
bool spin_held(volatile int* lock) { return *lock == (int)cpu_self()->index + 1; }

/* ---- scheduler lock ---- */
static volatile int sched_owner = -1;
bool sched_lock_held(void) { return sched_owner == (int)cpu_self()->index; }
void sched_lock(void) {
    int me = (int)cpu_self()->index, free = -1;
    if (sched_owner == me) panic("scheduler lock recursion");
    while (!__atomic_compare_exchange_n(&sched_owner, &free, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        free = -1; while (__atomic_load_n(&sched_owner, __ATOMIC_RELAXED) != -1) pause();
    }
}
void sched_unlock(void) {
    if (sched_owner != (int)cpu_self()->index) panic("scheduler lock released by a non-owner");
    __atomic_store_n(&sched_owner, -1, __ATOMIC_RELEASE);
}

/* ---- inter-processor interrupts ---- */
static void lapic_icr(uint32_t destination, uint32_t command) { apic_icr(destination, command); }
void smp_send_ipi(unsigned cpu, unsigned vector) {
    lapic_icr(cpus[cpu].apic_id, (1U << 14) | vector); /* fixed delivery, physical destination */
}
/* A task became runnable: wake one halted CPU (application processors first; the BSP also polls). */
bool smp_wake_idle(void) {
    if (cpu_count < 2) return false;
    struct cpu* me = cpu_self();
    for (unsigned k = 1; k <= MAX_CPUS; k++) {
        struct cpu* c = &cpus[k % MAX_CPUS];
        if (!c->online || !c->idle || c->wake_pending) continue;
        c->wake_pending = true;
        if (c != me) smp_send_ipi(c->index, VEC_WAKE);
        return true;
    }
    return false;
}
void smp_wake_ipi(void) { cpu_self()->wake_pending = true; lapic_eoi(); }

/* ---- TLB shootdown (one request at a time, under tlb_lock: initiators need not hold the big lock) ---- */
static volatile uint64_t tlb_root;
static volatile bool tlb_drop;
static volatile uint32_t tlb_pending;
static volatile int tlb_lock;
void smp_tlb_poll(void) {
    uint32_t bit = 1U << cpu_self()->index;
    if (!(__atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE) & bit)) return;
    if (tlb_drop) space_drop_loaded(tlb_root);
    else __asm__ volatile("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
    __atomic_and_fetch(&tlb_pending, ~bit, __ATOMIC_RELEASE);
}
static void tlb_request(uint64_t root, bool drop);
void smp_tlb_shootdown(uint64_t root, bool drop) {
    if (cpu_count < 2) return;
    spin_acquire(&tlb_lock); tlb_request(root, drop); spin_release(&tlb_lock);
}
static void tlb_request(uint64_t root, bool drop) {
    /* Our PTE stores must be visible before we read which CPUs have the root loaded: schedulers
     * load CR3 without the big lock, and a CPU that loads it afterwards walks the new tables. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct cpu* me = cpu_self(); uint32_t targets = 0;
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        struct cpu* c = &cpus[i];
        if (c != me && c->online && (!root || c->loaded_root == root)) targets |= 1U << i;
    }
    if (!targets) return;
    tlb_root = root; tlb_drop = drop;
    __atomic_store_n(&tlb_pending, targets, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < MAX_CPUS; i++) if (targets & (1U << i)) smp_send_ipi(i, VEC_TLB);
    while (__atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE)) pause();
}
void smp_tlb_ipi(void) {
    smp_tlb_poll(); /* nothing to do if a spin loop already answered this request */
    lapic_eoi();
}

/* ---- bring-up ---- */
static void delay_us(uint64_t us) {
    uint64_t end = native_clock_ns() + us * 1000;
    while (native_clock_ns() < end) pause();
}
static unsigned firmware_cpus(void) {
    /* QEMU fw_cfg FW_CFG_NB_CPUS (little-endian u16). */
    __asm__ volatile("outw %0,%1" :: "a"((uint16_t)0x05), "Nd"((uint16_t)0x510));
    uint8_t lo, hi;
    __asm__ volatile("inb %1,%0" : "=a"(lo) : "Nd"((uint16_t)0x511));
    __asm__ volatile("inb %1,%0" : "=a"(hi) : "Nd"((uint16_t)0x511));
    unsigned n = lo | (unsigned)hi << 8;
    return n == 0xFFFF ? 1 : n;
}
static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

/* ---- TSC synchronization ----
 * Firmware or a hypervisor may start each CPU's time-stamp counter at a different value (VMware does),
 * while the kernel clock, the vDSO and block timeouts read the TSC of whichever CPU they run on. While
 * the BSP waits for an AP to come online, the AP measures its offset from the BSP's TSC by ping-pong;
 * the sample with the shortest round trip wins (error at most half of it). An offset beyond that round
 * trip is cancelled with IA32_TSC_ADJUST when the CPU has it; what remains becomes the CPU's tsc_offset,
 * added by every clock read (native_tsc(); RDTSCP's TSC_AUX names the CPU in the vDSO). */
bool smp_tsc_skewed, smp_rdtscp, smp_tsc_unstable;
static volatile int tsc_ping;           /* 1: an AP asks for the BSP's TSC; 2: tsc_pong holds it */
static volatile uint64_t tsc_pong;
static inline uint64_t tsc_raw(void) {
    uint32_t lo, hi; __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}
static void tsc_warp_step(int64_t offset);
static void tsc_serve(void) {           /* BSP: answer a pending ping, or take part in the warp test (3) */
    int state = __atomic_load_n(&tsc_ping, __ATOMIC_ACQUIRE);
    if (state == 3) { while (__atomic_load_n(&tsc_ping, __ATOMIC_ACQUIRE) == 3) tsc_warp_step(0); return; }
    if (state != 1) return;
    tsc_pong = tsc_raw();
    __atomic_store_n(&tsc_ping, 2, __ATOMIC_RELEASE);
}
static int64_t tsc_measure(uint64_t* round_trip) {
    int64_t offset = 0; uint64_t best = ~0ULL;
    for (unsigned i = 0; i < 256; i++) {
        uint64_t before = tsc_raw();
        __atomic_store_n(&tsc_ping, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&tsc_ping, __ATOMIC_ACQUIRE) != 2) pause();
        uint64_t bsp = tsc_pong, after = tsc_raw();
        __atomic_store_n(&tsc_ping, 0, __ATOMIC_RELEASE);
        if (after - before < best) { best = after - before; offset = (int64_t)(bsp - (before + (after - before) / 2)); }
    }
    *round_trip = best;
    return offset;
}
static bool tsc_beyond(int64_t offset, uint64_t round_trip) { return (uint64_t)(offset < 0 ? -offset : offset) > round_trip; }
/* Warp test (as Linux's check_tsc_warp): both CPUs read their corrected TSC under one lock for 5 ms
 * and count reads that are older than the previous read of the other CPU. Any warp: smp_tsc_unstable. */
static volatile int tsc_warp_lock;
static volatile uint64_t tsc_warp_last, tsc_warp_count, tsc_warp_max, tsc_warp_steps;
static void tsc_warp_step(int64_t offset) {
    while (__atomic_exchange_n(&tsc_warp_lock, 1, __ATOMIC_ACQUIRE)) pause();
    uint64_t previous = tsc_warp_last, now = tsc_raw() + (uint64_t)offset;
    tsc_warp_last = now; tsc_warp_steps++;
    if (now < previous) { tsc_warp_count++; if (previous - now > tsc_warp_max) tsc_warp_max = previous - now; }
    __atomic_store_n(&tsc_warp_lock, 0, __ATOMIC_RELEASE);
}
static void tsc_warp_ap(struct cpu* c) {
    tsc_warp_last = 0; tsc_warp_count = tsc_warp_max = tsc_warp_steps = 0;
    __atomic_store_n(&tsc_ping, 3, __ATOMIC_RELEASE);
    uint64_t end = native_clock_ns() + 5000000ULL;
    while (native_clock_ns() < end) tsc_warp_step(c->tsc_offset);
    __atomic_store_n(&tsc_ping, 0, __ATOMIC_RELEASE);
    c->tsc_warps = tsc_warp_count; c->tsc_warp_max = tsc_warp_max; c->tsc_warp_steps = tsc_warp_steps;
}
static void tsc_sync_ap(struct cpu* c) {
    uint64_t round_trip; int64_t offset = tsc_measure(&round_trip);
    c->tsc_skew = offset; c->tsc_round_trip = round_trip;
    if (tsc_beyond(offset, round_trip)) {
        uint32_t a, b, cx, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cx), "=d"(d) : "a"(0), "c"(0));
        if (a >= 7) __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(cx), "=d"(d) : "a"(7), "c"(0)); else b = 0;
        if (b & 2) { /* IA32_TSC_ADJUST */
            uint64_t adjusted = rdmsr(0x3B) + (uint64_t)offset;
            __asm__ volatile("wrmsr" :: "c"(0x3B), "a"((uint32_t)adjusted), "d"((uint32_t)(adjusted >> 32)));
            offset = tsc_measure(&round_trip);
        }
        if (tsc_beyond(offset, round_trip)) c->tsc_offset = offset;
    }
    tsc_warp_ap(c);
}
/* An AP keeps x2APIC mode across INIT (firmware may have set it): enter the BSP's mode (apic.c). */
static void lapic_force_xapic(void) { (void)apic_enter_mode(); }
static void lapic_local_setup(bool ap) {
    lapic_write(LAPIC_SVR, (lapic_read(LAPIC_SVR) & ~0xFFU) | 0x100 | VEC_SPURIOUS);
    if (!ap) return; /* the BSP keeps the firmware's virtual-wire LINT0 for the PIC */
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_LINT0, 1U << 16); lapic_write(LAPIC_LVT_LINT1, 1U << 16); lapic_write(LAPIC_LVT_ERROR, 1U << 16);
    lapic_write(LAPIC_TIMER_DIVIDE, 3);
    lapic_write(LAPIC_LVT_TIMER, (1U << 17) | VEC_LAPIC_TIMER); /* periodic */
    lapic_write(LAPIC_TIMER_INITIAL, timer_period);
}
void ap_main(unsigned index) {
    struct cpu* c = &cpus[index];
    __atomic_store_n(&c->arrived, true, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&c->go, __ATOMIC_ACQUIRE)) pause();
    if (c->go != 1) for (;;) __asm__ volatile("cli; hlt");
    cpu_local_init(index, c->fallback_top, c->ist_top);
    fpu_cpu_init();
    c->loaded_root = smp_kernel_root; c->loaded_space = NULL;
    lapic_force_xapic();
    pm_cpu_online(index);   /* after a system sleep: MTRRs/PAT as before (pm.c) */
    lapic_local_setup(true);
    c->apic_id = apic_id_self();
    tsc_sync_ap(c);
    __atomic_store_n(&c->online, true, __ATOMIC_SEQ_CST);
    task_scheduler_ap();
}
void smp_init(void) {
    struct cpu* bsp = &cpus[0];
    bsp->online = true;
    __atomic_store_n(&lock_owner, 0, __ATOMIC_RELEASE); /* the boot CPU runs kernel code from here on */
    /* Processors: the MADT's enabled local APICs, started one by one (targeted INIT/SIPI); without
     * a MADT, QEMU's fw_cfg count and a broadcast INIT/SIPI. */
    bool targeted = acpi.madt && acpi.cpu_count > 0;
    unsigned wanted = targeted ? acpi.cpu_count : firmware_cpus();
    if ((boot_info.flags & NEXUS_BOOT_NO_TRAMPOLINE) || boot_option("nosmp")) wanted = 1;
    if (wanted > MAX_CPUS) wanted = MAX_CPUS;
    uint64_t apic_base = rdmsr(0x1B);
    if (wanted < 2 || !(apic_base & (1ULL << 11)) || (apic_base & 0xFFFFFF000ULL) != LAPIC_BASE) {
        vga_print("[64] SMP: 1 CPU\n"); return;
    }
    bsp->apic_id = apic_id_self();
    uint8_t targets[MAX_CPUS]; unsigned target_count = 0;
    if (targeted) {
        for (unsigned i = 0; i < acpi.cpu_count && 1 + target_count < wanted; i++)
            if (acpi.cpu_apic_ids[i] != bsp->apic_id) targets[target_count++] = acpi.cpu_apic_ids[i];
        wanted = 1 + target_count;
        if (wanted < 2) { vga_print("[64] SMP: 1 CPU\n"); return; }
    }
    lapic_local_setup(false);
    /* Calibrate the LAPIC timer against the TSC clock (one-shot, masked). */
    lapic_write(LAPIC_TIMER_DIVIDE, 3);
    lapic_write(LAPIC_LVT_TIMER, (1U << 16) | VEC_LAPIC_TIMER);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFFU);
    delay_us(10000);
    uint32_t elapsed = 0xFFFFFFFFU - lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0);
    timer_period = (uint32_t)((uint64_t)elapsed * 100 / AP_HZ);
    if (!timer_period) { vga_print("[64] SMP: LAPIC timer calibration failed; 1 CPU\n"); return; }
    for (unsigned i = 1; i < wanted; i++) {
        uint64_t stack = arena_alloc(AP_STACK_BYTES), fallback = arena_alloc(AP_IST_BYTES), ist = arena_alloc(AP_IST_BYTES);
        if (!stack || !fallback || !ist) panic("SMP stacks");
        cpus[i].stack_top = stack + AP_STACK_BYTES; cpus[i].fallback_top = fallback + AP_IST_BYTES;
        cpus[i].ist_top = ist + AP_IST_BYTES; ap_stacks[i] = cpus[i].stack_top;
    }
    uint64_t size = (uint64_t)(ap_trampoline_end - ap_trampoline);
    uint8_t* page = phys_to_virt(TRAMPOLINE);
    memcpy(page, ap_trampoline, size);
    *(uint16_t*)(page + (ap_gdtr - ap_trampoline)) = (uint16_t)(GDT_ENTRIES * 8 - 1); /* boot gdt64 */
    *(uint32_t*)(page + (ap_gdtr - ap_trampoline) + 2) = (uint32_t)virt_to_phys(gdt64);
    *(uint32_t*)(page + (ap_cr3 - ap_trampoline)) = (uint32_t)smp_kernel_root;
    *(uint64_t*)(page + (ap_entry - ap_trampoline)) = (uint64_t)ap_start64;
    kernel_trampoline_page(TRAMPOLINE, true);
    if (targeted) {
        for (unsigned i = 0; i < target_count; i++) lapic_icr(targets[i], 0x00004500);  /* INIT, physical destination */
        delay_us(10000);
        for (unsigned pass = 0; pass < 2; pass++) {
            for (unsigned i = 0; i < target_count; i++) lapic_icr(targets[i], 0x00004600 | (uint32_t)(TRAMPOLINE >> 12)); /* SIPI */
            delay_us(200);
        }
    } else {
        lapic_icr(0, 0x000C4500);                           /* INIT, all excluding self */
        delay_us(10000);
        lapic_icr(0, 0x000C4600 | (uint32_t)(TRAMPOLINE >> 12)); /* SIPI */
        delay_us(200);
        lapic_icr(0, 0x000C4600 | (uint32_t)(TRAMPOLINE >> 12));
    }
    uint64_t deadline = native_clock_ns() + 500000000ULL;
    for (unsigned i = 1; i < wanted; i++) while (!cpus[i].arrived && native_clock_ns() < deadline) pause();
    unsigned started = 1;
    for (unsigned i = 1; i < wanted; i++) {
        int verdict = cpus[i].arrived ? 1 : 2; /* a straggler that arrives later parks itself */
        __atomic_store_n(&cpus[i].go, verdict, __ATOMIC_RELEASE);
        if (verdict == 1) {
            uint64_t ready = native_clock_ns() + 500000000ULL;
            while (!cpus[i].online && native_clock_ns() < ready) { tsc_serve(); pause(); }
            if (!cpus[i].online) panic("application processor failed to initialize");
            if (cpus[i].tsc_offset) smp_tsc_skewed = true;
            if (cpus[i].tsc_warps) smp_tsc_unstable = true;
            started++;
        }
    }
    cpu_count = started;
    uint64_t worst = 0, warps = 0, warp_max = 0, warp_steps = 0; unsigned worst_cpu = 0;
    for (unsigned i = 1; i < started; i++) {
        uint64_t skew = (uint64_t)(cpus[i].tsc_skew < 0 ? -cpus[i].tsc_skew : cpus[i].tsc_skew);
        if (skew >= worst) { worst = skew; worst_cpu = i; }
        warps += cpus[i].tsc_warps; warp_steps += cpus[i].tsc_warp_steps;
        if (cpus[i].tsc_warp_max > warp_max) warp_max = cpus[i].tsc_warp_max;
    }
    if (started > 1) {
        char text[24];
        vga_print("[64] TSC: "); vga_print(smp_tsc_unstable ? "unstable (clamped clock)" : smp_tsc_skewed ? "per-CPU offsets" : "synchronized");
        vga_print(", largest skew "); vga_print(cpus[worst_cpu].tsc_skew < 0 ? "-" : ""); number64(worst, text); vga_print(text);
        vga_print(" ticks (cpu "); uint_to_str(worst_cpu, text); vga_print(text);
        vga_print(", round trip "); number64(cpus[worst_cpu].tsc_round_trip, text); vga_print(text);
        vga_print(", residual "); vga_print(cpus[worst_cpu].tsc_offset < 0 ? "-" : "");
        number64((uint64_t)(cpus[worst_cpu].tsc_offset < 0 ? -cpus[worst_cpu].tsc_offset : cpus[worst_cpu].tsc_offset), text); vga_print(text);
        vga_print(smp_rdtscp ? ", RDTSCP); warps " : ", no RDTSCP); warps "); number64(warps, text); vga_print(text);
        vga_print("/"); number64(warp_steps, text); vga_print(text);
        vga_print(" max "); number64(warp_max, text); vga_print(text); vga_print(" ticks\n");
    }
    kernel_trampoline_page(TRAMPOLINE, false);
    smp_tlb_shootdown(0, false);
    char number[12]; uint_to_str(started, number);
    vga_print("[64] SMP: "); vga_print(number);
    vga_print(apic_x2 ? " CPUs online (big kernel lock, x2APIC MSR IPIs, " : " CPUs online (big kernel lock, LAPIC IPIs, ");
    vga_print(targeted ? "MADT targeted INIT/SIPI)\n" : "broadcast INIT/SIPI)\n");
}

/* ---- after a system sleep (pm.c) ----
 * S3 and a hibernation restore leave the application processors reset (they were offlined first: no task ran on
 * them). Start them again through the trampoline as smp_init did; their scheduler loops begin afresh and the
 * TSC offsets are measured again. Indices follow arrival order, as at boot (every per-CPU structure is
 * re-initialized by cpu_local_init/ap_main for the index a processor takes). */
extern int ap_next_index;
bool smp_restarting;
void smp_restart(void) {
    if (cpu_count < 2) return;
    unsigned wanted = cpu_count, started = 1;
    smp_restarting = true;
    lapic_local_setup(false);
    uint64_t size = (uint64_t)(ap_trampoline_end - ap_trampoline);
    uint8_t* page = phys_to_virt(TRAMPOLINE);
    memcpy(page, ap_trampoline, size);
    *(uint16_t*)(page + (ap_gdtr - ap_trampoline)) = (uint16_t)(GDT_ENTRIES * 8 - 1);
    *(uint32_t*)(page + (ap_gdtr - ap_trampoline) + 2) = (uint32_t)virt_to_phys(gdt64);
    *(uint32_t*)(page + (ap_cr3 - ap_trampoline)) = (uint32_t)smp_kernel_root;
    *(uint64_t*)(page + (ap_entry - ap_trampoline)) = (uint64_t)ap_start64;
    kernel_trampoline_page(TRAMPOLINE, true);
    __atomic_store_n(&ap_next_index, 1, __ATOMIC_SEQ_CST);
    smp_tsc_skewed = smp_tsc_unstable = false;
    for (unsigned i = 1; i < wanted; i++) {
        cpus[i].arrived = cpus[i].online = cpus[i].idle = cpus[i].wake_pending = cpus[i].resched = false;
        cpus[i].go = 0; cpus[i].task = NULL; cpus[i].tsc_offset = 0; cpus[i].vm_held = NULL;
        cpus[i].loaded_fs = cpus[i].loaded_gs = 0;   /* cpu_local_init zeroes the AP's FS/GS base MSRs */
        ap_stacks[i] = cpus[i].stack_top;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    bool targeted = acpi.madt && acpi.cpu_count > 0;
    if (targeted) {
        for (unsigned i = 0; i < acpi.cpu_count; i++) if (acpi.cpu_apic_ids[i] != cpus[0].apic_id) lapic_icr(acpi.cpu_apic_ids[i], 0x00004500);
        delay_us(10000);
        for (unsigned pass = 0; pass < 2; pass++) {
            for (unsigned i = 0; i < acpi.cpu_count; i++) if (acpi.cpu_apic_ids[i] != cpus[0].apic_id) lapic_icr(acpi.cpu_apic_ids[i], 0x00004600 | (uint32_t)(TRAMPOLINE >> 12));
            delay_us(200);
        }
    } else {
        lapic_icr(0, 0x000C4500); delay_us(10000);
        lapic_icr(0, 0x000C4600 | (uint32_t)(TRAMPOLINE >> 12)); delay_us(200);
        lapic_icr(0, 0x000C4600 | (uint32_t)(TRAMPOLINE >> 12));
    }
    uint64_t deadline = native_clock_ns() + 500000000ULL;
    for (unsigned i = 1; i < wanted; i++) while (!cpus[i].arrived && native_clock_ns() < deadline) pause();
    for (unsigned i = 1; i < wanted; i++) {
        int verdict = cpus[i].arrived ? 1 : 2;
        __atomic_store_n(&cpus[i].go, verdict, __ATOMIC_RELEASE);
        if (verdict != 1) { vga_print("[64] SMP: an application processor did not come back after resume\n"); continue; }
        uint64_t ready = native_clock_ns() + 500000000ULL;
        while (!cpus[i].online && native_clock_ns() < ready) { tsc_serve(); pause(); }
        if (!cpus[i].online) { vga_print("[64] SMP: application processor failed to initialize after resume\n"); continue; }
        if (cpus[i].tsc_offset) smp_tsc_skewed = true;
        if (cpus[i].tsc_warps) smp_tsc_unstable = true;
        started++;
    }
    kernel_trampoline_page(TRAMPOLINE, false);
    smp_tlb_shootdown(0, false);
    smp_restarting = false;
    char number[12]; uint_to_str(started, number);
    vga_print("[64] SMP: "); vga_print(number); vga_print(" CPUs online after resume\n");
}

/* ---- local APIC timer for tick.c (tickless idle): lapic_reg_read/write (above) and these go through apic.c
 * (xAPIC MMIO or x2APIC MSRs), so an x2APIC switch covers them ---- */
void smp_self_ipi(unsigned vector) { lapic_icr(0, (1U << 18) | vector); }   /* fixed delivery, "self" shorthand */
void smp_ap_tick(bool on) {   /* this application processor's 250 Hz preemption tick (stopped while it idles) */
    if (!on) { lapic_write(LAPIC_TIMER_INITIAL, 0); return; }
    lapic_write(LAPIC_LVT_TIMER, (1U << 17) | VEC_LAPIC_TIMER); lapic_write(LAPIC_TIMER_INITIAL, timer_period);
}
