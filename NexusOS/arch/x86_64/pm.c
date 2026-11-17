/* System sleep (power.h): /sys/power/state "freeze" (suspend-to-idle), "mem" (ACPI S3) and "disk" (hibernation,
 * hibernate.c), reboot(2) SW_SUSPEND, and the Aurora/policy requests.
 *
 * A transition runs on the boot CPU's scheduler loop (pm_poll, big kernel lock held, no task on that CPU) as a
 * staged state machine, so that the loop keeps running between stages:
 *   1. notify: uevent PM_EVENT=<state>-prepare; user space (Aurora's lock screen) gets sleep_delay_ms
 *      (/sys/power/policy, default 500 ms) unless it writes sleep_ack first;
 *   2. freeze: no task is dispatched any more (process.c: pm_freezing); running tasks are preempted at their next
 *      return to user mode (wake IPIs), tasks in the kernel finish their system call or block; application
 *      processors park (pm_cpu_park). Failing to freeze within 20 s aborts with EBUSY (as Linux's freezer);
 *   3. suspend: file systems written back and the disks flushed, then every registered driver's suspend in
 *      reverse registration order (a veto resumes the others and aborts), PCI configuration space saved;
 *   4. the platform state: s2idle halts the boot CPU until a wake event (power/sleep button, lid, RTC alarm,
 *      any ACPI event); S3 runs _TTS/_PTS/_SST, offlines the application processors, saves the CPU state
 *      (control registers, EFER, MTRRs, PAT, segment bases, GDT/IDT/TSS), points the FACS waking vector at
 *      the real-mode trampoline (pm_asm.asm) and writes SLP_EN; hibernation writes the image (hibernate.c);
 *   5. resume: CPU state, clocks (CLOCK_MONOTONIC continues where it stopped; REALTIME/BOOTTIME advance by the
 *      RTC-measured time asleep), PIT, local APIC/8259/IOAPIC routes (irq_resume), PCI configuration space
 *      (bridges first, command register last), MSI/MSI-X messages, _WAK/_SST/_TTS, ACPI event enables, the
 *      application processors restarted (smp_restart), every driver's resume in registration order;
 *   6. thaw and uevent PM_EVENT=post-<state>; the request returns 0 (or the error).
 * The waking press of a power/sleep button is reported as KEY_WAKEUP only (power.c pm_waking). */
#include "native.h"
#include "power.h"
#include "apic.h"
#include "boot_info.h"
#include "gpu.h"
#include "../../kernel/ata.h"
#include "../../kernel/port.h"
#include "../../kernel/pci.h"
#include "../../kernel/rtc.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

extern uint8_t pm_wake_trampoline[], pm_wake_trampoline_end[], pm_wake_gdtr[], pm_wake_cr3[], pm_wake_entry[], pm_wake_vbe_mode[];
extern void pm_wake_long(void);
extern int pm_suspend_cpu(int (*enter)(void* context), void* context);
extern uint64_t gdt64[], boot_pml4[];
extern uint8_t syscall_stack_top[], ist_stack_top[];
void power_resumed(void);
#define WAKE 0x7800ULL
#define TRAMPOLINE_PAGE 0x7000ULL
#define LAPIC_BASE 0xFEE00000ULL

static void say(const char* s) { vga_print(s); }
static void say_number(uint64_t value) { char t[24]; number64(value, t); vga_print(t); }
static inline uint64_t rdmsr(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr(uint32_t msr, uint64_t value) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory"); }
static inline void cpuid(uint32_t leaf, uint32_t* r) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0)); }

/* ---- the driver registry ---- */
#define PM_MAX_DEVICES 48
static const struct pm_ops* devices[PM_MAX_DEVICES];
static bool device_suspended[PM_MAX_DEVICES], device_storage[PM_MAX_DEVICES];
static unsigned device_count;
int pm_register(const struct pm_ops* ops) {
    if (!ops || device_count == PM_MAX_DEVICES) { say("[64] PM: driver registry full\n"); return -28; }
    for (unsigned i = 0; i < device_count; i++) if (devices[i] == ops) return 0;
    devices[device_count++] = ops; return 0;
}
int pm_register_storage(const struct pm_ops* ops) {
    int error = pm_register(ops);
    for (unsigned i = 0; !error && i < device_count; i++) if (devices[i] == ops) device_storage[i] = true;
    return error;
}
/* Plain hooks in one line, e.g. nic.c: pm_register_hooks("nic", native_nic_suspend, native_nic_resume). */
#define PM_MAX_HOOKS 8
static struct pm_hooks { const char* name; void (*suspend)(void); bool (*resume)(void); } hooks[PM_MAX_HOOKS];
static struct pm_ops hook_ops[PM_MAX_HOOKS];
static unsigned hook_count;
static int hooks_suspend(void* context, unsigned state) { (void)state; struct pm_hooks* h = context; if (h->suspend) h->suspend(); return 0; }
static void hooks_resume(void* context, unsigned state) {
    (void)state; struct pm_hooks* h = context;
    if (h->resume && !h->resume()) { say("[64] PM: "); say(h->name); say(" did not come back after the resume\n"); }
}
int pm_register_hooks(const char* name, void (*suspend)(void), bool (*resume)(void)) {
    for (unsigned i = 0; i < hook_count; i++) if (hooks[i].suspend == suspend && hooks[i].resume == resume) return 0;
    if (hook_count == PM_MAX_HOOKS) { say("[64] PM: driver registry full\n"); return -28; }
    hooks[hook_count] = (struct pm_hooks){name, suspend, resume};
    hook_ops[hook_count] = (struct pm_ops){name, hooks_suspend, hooks_resume, &hooks[hook_count]};
    return pm_register(&hook_ops[hook_count++]);
}
/* Hibernation suspends the storage drivers last, after the image went out through them (storage = true). */
static int devices_suspend(unsigned state, bool storage) {
    for (unsigned i = device_count; i--;) {
        const struct pm_ops* d = devices[i];
        if (device_suspended[i] || (state == PM_STATE_DISK && device_storage[i] != storage)) continue;
        int error = d->suspend ? d->suspend(d->context, state) : 0;
        if (error) { say("[64] PM: "); say(d->name); say(" refused to suspend\n"); return error; }
        device_suspended[i] = true;
    }
    return 0;
}
static void devices_resume(unsigned state) {
    for (unsigned i = 0; i < device_count; i++)
        if (device_suspended[i]) { device_suspended[i] = false; if (devices[i]->resume) devices[i]->resume(devices[i]->context, state); }
}
void pm_quiesce_all(void) { devices_suspend(PM_STATE_DISK, false); devices_suspend(PM_STATE_DISK, true); }

/* ---- PCI configuration space across a power loss ---- */
#define PCI_SAVE_MAX 64
static struct { uint8_t bus, device, function, header; uint32_t config[64]; } pci_saved[PCI_SAVE_MAX];
static unsigned pci_saved_count;
static void pci_save(void) {
    pci_device_t* list = pci_get_devices(); int count = pci_device_count(); pci_saved_count = 0;
    for (int i = 0; i < count && pci_saved_count < PCI_SAVE_MAX; i++) {
        unsigned n = pci_saved_count++;
        pci_saved[n].bus = list[i].bus; pci_saved[n].device = list[i].device; pci_saved[n].function = list[i].function;
        for (unsigned d = 0; d < 64; d++) pci_saved[n].config[d] = pci_read32(list[i].bus, list[i].device, list[i].function, (uint8_t)(d * 4));
        pci_saved[n].header = (uint8_t)(pci_saved[n].config[3] >> 16) & 0x7F;
    }
}
static uint8_t saved_capability(unsigned n, uint8_t id) {
    const uint8_t* c = (const uint8_t*)pci_saved[n].config;
    if (!(c[6] & 0x10)) return 0;
    uint8_t at = c[0x34] & 0xFC;
    for (unsigned guard = 0; at >= 0x40 && guard < 48; guard++) { if (c[at] == id) return at; at = c[at + 1] & 0xFC; }
    return 0;
}
static void config16(unsigned n, uint8_t offset, uint16_t value) {
    uint32_t word = pci_read32(pci_saved[n].bus, pci_saved[n].device, pci_saved[n].function, offset & 0xFC);
    unsigned shift = (offset & 2) * 8;
    word = (word & ~(0xFFFFU << shift)) | ((uint32_t)value << shift);
    if ((offset & 0xFC) == 0x04) word &= 0xFFFF;   /* never write status bits back (write-1-to-clear) */
    pci_write32(pci_saved[n].bus, pci_saved[n].device, pci_saved[n].function, offset & 0xFC, word);
}
/* Linux pci_restore_state order: bridges (bus numbers, windows) before what is behind them; BARs and the rest
 * of the header high to low, the command register last; PCIe and PM control registers; MSI/MSI-X are
 * reprogrammed by irq_resume_messages. */
static void pci_restore(void) {
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned n = 0; n < pci_saved_count; n++) {
            bool bridge = pci_saved[n].header == 1;
            if (bridge != (pass == 0)) continue;
            uint8_t b = pci_saved[n].bus, d = pci_saved[n].device, f = pci_saved[n].function;
            if (pci_read32(b, d, f, 0) != pci_saved[n].config[0]) continue;   /* gone or replaced */
            uint8_t pm = saved_capability(n, 0x01);
            if (pm) {   /* back to D0 first: config writes to a D3hot function's BARs are lost */
                uint16_t control = (uint16_t)(pci_read32(b, d, f, (uint8_t)(pm + 4)) & 0xFFFF);
                if (control & 3) { config16(n, (uint8_t)(pm + 4), control & ~3U); for (volatile unsigned i = 0; i < 100000; i++) port_byte_in(0x80); }
            }
            for (int dword = 15; dword >= 2; dword--)   /* 0x3C .. 0x08 (class/revision is read-only) */
                if (pci_read32(b, d, f, (uint8_t)(dword * 4)) != pci_saved[n].config[dword]) pci_write32(b, d, f, (uint8_t)(dword * 4), pci_saved[n].config[dword]);
            /* Host and ISA/LPC bridges keep chipset state in device-specific registers (PMBASE/ACPI decode, PIRQ
             * routing, MMCONFIG, RCBA...). Firmware with an S3 boot script restores them; SeaBIOS does not (QEMU
             * q35's ICH9 comes back with its ACPI I/O block undecoded). Rewrite those that differ. */
            uint8_t class_code = (uint8_t)(pci_saved[n].config[2] >> 24), subclass = (uint8_t)(pci_saved[n].config[2] >> 16);
            if (class_code == 0x06 && subclass <= 0x01)
                for (unsigned dword = 16; dword < 64; dword++)
                    if (pci_read32(b, d, f, (uint8_t)(dword * 4)) != pci_saved[n].config[dword]) pci_write32(b, d, f, (uint8_t)(dword * 4), pci_saved[n].config[dword]);
            uint8_t express = saved_capability(n, 0x10);
            if (express) {
                const uint8_t* c = (const uint8_t*)pci_saved[n].config;
                uint16_t flags = (uint16_t)(c[express + 2] | c[express + 3] << 8); unsigned type = (flags >> 4) & 15;
                config16(n, (uint8_t)(express + 0x08), (uint16_t)(c[express + 0x08] | c[express + 0x09] << 8));   /* device control */
                config16(n, (uint8_t)(express + 0x10), (uint16_t)(c[express + 0x10] | c[express + 0x11] << 8));   /* link control */
                if (flags & 0x100) config16(n, (uint8_t)(express + 0x18), (uint16_t)(c[express + 0x18] | c[express + 0x19] << 8)); /* slot */
                if (type == 4 || type == 6) config16(n, (uint8_t)(express + 0x1C), (uint16_t)(c[express + 0x1C] | c[express + 0x1D] << 8)); /* root control */
                if ((flags & 15) >= 2) {
                    config16(n, (uint8_t)(express + 0x28), (uint16_t)(c[express + 0x28] | c[express + 0x29] << 8));   /* device control 2 */
                    config16(n, (uint8_t)(express + 0x30), (uint16_t)(c[express + 0x30] | c[express + 0x31] << 8));   /* link control 2 */
                }
            }
            config16(n, 0x04, (uint16_t)pci_saved[n].config[1]);   /* command (decode, bus master, INTx disable) last */
        }
}

/* ---- MTRRs (Intel SDM 11.11.8) ---- */
static struct { bool valid; unsigned variable; uint64_t def, fixed[11], base[16], mask[16], pat; } mtrr;
static const uint32_t fixed_msrs[11] = {0x250, 0x258, 0x259, 0x268, 0x269, 0x26A, 0x26B, 0x26C, 0x26D, 0x26E, 0x26F};
static void mtrr_save(void) {
    uint32_t r[4]; cpuid(1, r); mtrr.valid = false;
    if (r[3] & (1U << 16)) mtrr.pat = rdmsr(0x277);
    if (!(r[3] & (1U << 12))) return;
    uint64_t cap = rdmsr(0xFE); mtrr.variable = (unsigned)(cap & 0xFF); if (mtrr.variable > 16) mtrr.variable = 16;
    mtrr.def = rdmsr(0x2FF);
    if (cap & 0x100) for (unsigned i = 0; i < 11; i++) mtrr.fixed[i] = rdmsr(fixed_msrs[i]);
    for (unsigned i = 0; i < mtrr.variable; i++) { mtrr.base[i] = rdmsr(0x200 + 2 * i); mtrr.mask[i] = rdmsr(0x201 + 2 * i); }
    mtrr.valid = true;
}
static void mtrr_restore(void) {   /* interrupts off */
    if (!mtrr.valid) return;
    uint64_t cr0, cr4; __asm__ volatile("mov %%cr0,%0; mov %%cr4,%1" : "=r"(cr0), "=r"(cr4));
    if ((rdmsr(0x2FF) == mtrr.def) && (!mtrr.variable || rdmsr(0x200) == mtrr.base[0])) { if (mtrr.pat) wrmsr(0x277, mtrr.pat); return; }   /* firmware restored them */
    __asm__ volatile("mov %0,%%cr0; wbinvd" :: "r"((cr0 | (1ULL << 30)) & ~(1ULL << 29)) : "memory");   /* CD=1, NW=0 */
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4 & ~(1ULL << 7)) : "memory");                            /* PGE off: flush */
    wrmsr(0x2FF, mtrr.def & ~0xC00ULL);
    if (rdmsr(0xFE) & 0x100) for (unsigned i = 0; i < 11; i++) wrmsr(fixed_msrs[i], mtrr.fixed[i]);
    for (unsigned i = 0; i < mtrr.variable; i++) { wrmsr(0x200 + 2 * i, mtrr.base[i]); wrmsr(0x201 + 2 * i, mtrr.mask[i]); }
    wrmsr(0x2FF, mtrr.def);
    if (mtrr.pat) wrmsr(0x277, mtrr.pat);
    __asm__ volatile("wbinvd; mov %0,%%cr0; mov %1,%%cr4" :: "r"(cr0), "r"(cr4) : "memory");
}

/* ---- the boot CPU's context ---- */
static struct { uint64_t cr0, cr3, cr4, efer, fs_base, gs_base, kernel_gs_base, xcr0; bool xsave; } cpu;
static void cpu_save(void) {
    __asm__ volatile("mov %%cr0,%0; mov %%cr3,%1; mov %%cr4,%2" : "=r"(cpu.cr0), "=r"(cpu.cr3), "=r"(cpu.cr4));
    cpu.efer = rdmsr(0xC0000080); cpu.fs_base = rdmsr(0xC0000100); cpu.gs_base = rdmsr(0xC0000101); cpu.kernel_gs_base = rdmsr(0xC0000102);
    mtrr_save();
}
void pm_resume_cpu(void);
/* Entered from pm_wake_long on the saved stack: long mode on the kernel's tables, interrupts off. */
void pm_resume_cpu(void) {
    __asm__ volatile("mov %0,%%cr4" :: "r"(cpu.cr4 & ~(1ULL << 18)) : "memory");   /* OSXSAVE with XCR0 below */
    wrmsr(0xC0000080, cpu.efer);
    __asm__ volatile("mov %0,%%cr0" :: "r"(cpu.cr0) : "memory");
    mtrr_restore();
    cpu_local_init(0, (uint64_t)syscall_stack_top, (uint64_t)ist_stack_top);   /* GDT copy, TSS (ltr), LDT, IDT, SYSCALL MSRs, PAT, TSC_AUX */
    fpu_cpu_init();                                                                /* CR4.OSXSAVE + XCR0 */
    wrmsr(0xC0000100, cpu.fs_base); wrmsr(0xC0000101, cpu.gs_base); wrmsr(0xC0000102, cpu.kernel_gs_base);
    __asm__ volatile("mov %0,%%cr3" :: "r"(cpu.cr3) : "memory");
}
/* Application processors coming back (smp.c ap_main): the MTRRs and PAT the boot CPU had. */
static volatile bool mtrr_resume_aps;
void pm_cpu_online(unsigned index) {
    (void)index;
    if (!mtrr_resume_aps) return;
    uint64_t flags; __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    mtrr_restore();
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* ---- display: QEMU's VGA (bochs DISPI) loses its mode with the device reset ---- */
static uint16_t dispi_saved[10]; static bool dispi_valid;
static uint16_t dispi_read(unsigned index) { port_word_out(0x1CE, (uint16_t)index); return port_word_in(0x1CF); }
static void dispi_write(unsigned index, uint16_t value) { port_word_out(0x1CE, (uint16_t)index); port_word_out(0x1CF, value); }
static bool dispi_present(void) {
    pci_device_t* list = pci_get_devices(); bool vga = false;
    for (int i = 0; i < pci_device_count(); i++) if (list[i].class_code == 0x03 && (list[i].vendor_id == 0x1234 || (list[i].vendor_id == 0x1AF4 && list[i].device_id == 0x1050))) vga = true;
    if (!vga) return false;
    uint16_t id = dispi_read(0); return id >= 0xB0C0 && id <= 0xB0C5;
}
static void display_suspend(void) {   /* registered through pm_register_hooks (plain hooks) */
    dispi_valid = framebuffer_get() && dispi_present() && (dispi_read(4) & 1);
    if (dispi_valid) for (unsigned i = 1; i < 10; i++) dispi_saved[i] = dispi_read(i);
}
static bool display_resume(void) {
    if (!dispi_valid) return true;
    dispi_write(4, 0);
    for (unsigned i = 1; i < 4; i++) dispi_write(i, dispi_saved[i]);
    for (unsigned i = 5; i < 10; i++) dispi_write(i, dispi_saved[i]);
    dispi_write(4, (uint16_t)(dispi_saved[4] | 0x80));   /* enabled, LFB, keep video memory */
    port_byte_in(0x3DA); port_byte_out(0x3C0, 0x20);     /* attribute controller: palette source = screen on */
    return (dispi_read(4) & 1) != 0;
}

/* ---- request/state machine ---- */
enum { IDLE, NOTIFY, DELAY, FREEZE };
static unsigned stage, target, pending;
static uint64_t stage_at, request_generation, done_generation, wake_until, sleep_delay_ms = 500;
static uint64_t io_wait_since;   /* blockio.c: the freeze waits for a task that owns the block path (0: not waiting) */
/* sleep_ack and the I/O barrier: the acknowledging process (Aurora, pid 1) cannot even open sleep_ack while another
 * task owns the block path (blockio.c), so while pid 1 sleeps at that barrier the delay is extended - each time it is
 * seen there, by sleep_delay_ms after that - up to ACK_GRACE_NS after the notification. */
#define ACK_GRACE_NS 5000000000ULL
bool task_at_channel(uint32_t pid, const void* channel);   /* process.c */
const void* block_io_barrier_channel(void);                 /* blockio.c */
static uint64_t ack_extended_until, notify_barrier_waits; static bool ack_late_reported;
static int result;
static volatile bool freezing, offline_request, ack, wake_event;
static volatile bool parked[MAX_CPUS], offlined[MAX_CPUS];
static unsigned resumes;
static const char* last_event = "none";
bool pm_freezing(void) { return freezing; }
/* While the hibernation image is written from live memory (interrupts on), expiring timeouts must not move tasks
 * between the scheduler's lists: pages saved before and after such a wake-up would disagree and the restored
 * task would be lost. process.c defers expiries while this is true; they run at the first interrupt after it. */
static volatile bool snapshotting;
bool pm_snapshotting(void) { return snapshotting; }
uint64_t pm_boottime_offset_ns(void) { return native_suspended_ns(); }
unsigned pm_resume_count(void) { return resumes; }
const char* pm_last_event(void) { return last_event; }
bool pm_waking(void) { return stage != IDLE || native_clock_ns() < wake_until; }
void pm_wakeup_event(void) { wake_event = true; }
void pm_sleep_ack(void) { ack = true; }
uint64_t* pm_sleep_delay(void) { return &sleep_delay_ms; }
static int lockdown(void);
bool pm_hibernation_available(void) { return !lockdown() && hibernate_available(); }   /* "disk" in /sys/power/state */
static bool state_supported(unsigned state) {
    if (state == PM_STATE_FREEZE) return true;
    if (state == PM_STATE_MEM) return acpi_sleep_state_supported(3) && !(boot_info.flags & NEXUS_BOOT_NO_TRAMPOLINE);
    if (state == PM_STATE_DISK) return !lockdown() && hibernate_available();
    return false;
}
static int locked_down(unsigned state) {   /* Linux: hibernation_available() false -> EPERM */
    if (state != PM_STATE_DISK || !lockdown()) return 0;
    say("[64] PM: hibernation refused: the kernel is locked down and the image cannot be authenticated (no TPM)\n");
    return -1;
}
static const char* prepare_name(unsigned state) { return state == PM_STATE_DISK ? "hibernation-prepare" : state == PM_STATE_FREEZE ? "freeze-prepare" : "suspend-prepare"; }
static const char* post_name(unsigned state) { return state == PM_STATE_DISK ? "post-hibernation" : state == PM_STATE_FREEZE ? "post-freeze" : "post-suspend"; }
int pm_request_async(unsigned state) {
    if (locked_down(state)) return -1;
    if (!state_supported(state)) return -22;
    if (stage != IDLE || pending) return -16;
    pending = state; request_generation++;
    return 0;
}
int pm_request(unsigned state) {
    if (!cpu_self()->task) return pm_request_async(state);
    if (locked_down(state)) return -1;
    if (!state_supported(state)) return -22;
    if (stage != IDLE || pending) return -16;
    pending = state; uint64_t generation = ++request_generation;
    /* A signal does not cancel a transition: with one pending the task cannot sleep, so it yields instead (frozen
     * tasks are not dispatched, so this does not spin once the freeze began). No wake can be lost between the check
     * and the sleep: the transition only completes after the freeze, which needs this task asleep first. */
    while (__atomic_load_n(&done_generation, __ATOMIC_ACQUIRE) < generation) if (task_wait_channel(&done_generation)) task_yield();
    return result;
}

/* Every CPU but this one parked with no task, and none running a task. */
static bool all_frozen(void) {
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        if (!cpus[i].online) continue;
        if (cpus[i].task) return false;
        if (i && !parked[i]) return false;
    }
    return true;
}
static void kick_cpus(void) {
    for (unsigned i = 1; i < MAX_CPUS; i++) if (cpus[i].online) smp_send_ipi(i, VEC_WAKE);
}
void pm_cpu_park(void) {
    struct cpu* c = cpu_self(); unsigned i = c->index;
    parked[i] = true;
    tick_cpu_park(true);   /* no 250 Hz tick while parked (tick.c) */
    while (freezing) {
        if (offline_request) {   /* the platform loses this CPU's state: halt until the INIT of smp_restart */
            apic_write(0x320, 1U << 16);                                    /* LAPIC timer masked */
            __atomic_store_n(&c->online, false, __ATOMIC_SEQ_CST);
            __atomic_store_n(&offlined[i], true, __ATOMIC_SEQ_CST);
            for (;;) __asm__ volatile("cli; hlt" ::: "memory");
        }
        __asm__ volatile("sti; hlt; cli" ::: "memory");
        smp_tlb_poll();
    }
    tick_cpu_park(false);
    parked[i] = false;
}
/* Application processors offline before the platform sleeps; back online afterwards (or after a failure). */
static bool aps_offline(void) {
    if (cpu_count < 2) return true;
    for (unsigned i = 0; i < MAX_CPUS; i++) offlined[i] = false;
    offline_request = true; kick_cpus();
    uint64_t deadline = native_clock_ns() + 2000000000ULL;
    for (;;) {
        bool all = true;
        for (unsigned i = 1; i < cpu_count; i++) if (!offlined[i]) all = false;
        if (all) return true;
        if (native_clock_ns() > deadline) { say("[64] PM: an application processor did not go offline\n"); return false; }
        __asm__ volatile("pause");
    }
}
static void aps_online(void) {
    if (cpu_count < 2) return;
    offline_request = false;
    mtrr_resume_aps = true;
    for (unsigned i = 1; i < cpu_count; i++) parked[i] = false;
    smp_restart();
    mtrr_resume_aps = false;
    native_vdso_clock_update();   /* TSC offsets measured again */
}
void pm_stop_other_cpus(void) {   /* reboot(2) halt/power-off/restart: the others stop where they are */
    if (cpu_count < 2 || cpu_self()->index) return;
    for (unsigned i = 1; i < MAX_CPUS; i++) if (cpus[i].online) {
        apic_icr(cpus[i].apic_id, 0x00004500);   /* INIT: the processor waits for a SIPI that never comes */
        cpus[i].online = false;
    }
}

static void pit_program(void) { port_byte_out(0x43, 0x34); port_byte_out(0x40, 1193 & 0xFF); port_byte_out(0x40, 1193 >> 8); }   /* 1 kHz, as cpu_tables */

/* ---- ACPI NVS memory across S3 and S4 (Linux suspend_nvs_save/restore) ----
 * Firmware memory of type ACPI NVS (E820 type 4, UEFI EfiACPIMemoryNVS 10) holds AML's own state (OperationRegions in
 * SystemMemory: OS flags from _OSI, brightness tables, EC shadows). A hibernation restore boots the firmware afresh,
 * which re-initialises it, and some firmware disturbs it across S3; the copy taken before the transition (kernel
 * memory: part of the hibernation image) is put back before _WAK runs. Test fixture (boot option
 * pm.test-nvs=0xADDR): 64 bytes of the firmware-reserved EBDA tail at ADDR act as an NVS region whose content each
 * boot sets to a new marker; after a restore it must hold the hibernated marker again. */
#define NVS_MAX 16
#define NVS_LIMIT (16ULL << 20)
struct nvs_region { uint64_t base, size; uint8_t* copy; };
static struct nvs_region nvs[NVS_MAX];
static unsigned nvs_count;
static uint64_t nvs_test_base, nvs_test_marker;
static void* nvs_view(uint64_t physical, uint64_t size) {
    if (physical + size <= 0x100000000ULL && physical + size > physical) return phys_to_virt(physical);
    return (void*)mmio_map(physical, size, MMIO_UNCACHED);
}
static unsigned nvs_regions(uint64_t base[NVS_MAX], uint64_t size[NVS_MAX]) {
    unsigned n = 0; uint32_t type = boot_info.source == NEXUS_BOOT_UEFI ? 10 : 4;
    for (unsigned i = 0; i < boot_info.region_count && i < NEXUS_BOOT_REGIONS_MAX && n < NVS_MAX; i++)
        if (boot_info.regions[i].type == type && boot_info.regions[i].size) { base[n] = boot_info.regions[i].base; size[n] = boot_info.regions[i].size; n++; }
    if (nvs_test_base && n < NVS_MAX) { base[n] = nvs_test_base; size[n] = 64; n++; }
    return n;
}
static void nvs_release(void) {
    for (unsigned i = 0; i < nvs_count; i++) if (nvs[i].copy) arena_free((uint64_t)nvs[i].copy, nvs[i].size);
    memset(nvs, 0, sizeof(nvs)); nvs_count = 0;
}
static int nvs_save(void) {
    uint64_t base[NVS_MAX], size[NVS_MAX], total = 0; unsigned n = nvs_regions(base, size);
    nvs_release();
    for (unsigned i = 0; i < n; i++) {
        if (size[i] > NVS_LIMIT || total + size[i] > NVS_LIMIT) { say("[64] PM: ACPI NVS larger than 16 MiB, not saved\n"); nvs_release(); return -12; }
        uint8_t* copy = (uint8_t*)arena_alloc(size[i]); void* view = nvs_view(base[i], size[i]);
        if (!copy || !view) { if (copy) arena_free((uint64_t)copy, size[i]); nvs_release(); return -12; }
        memcpy(copy, view, size[i]); nvs[nvs_count++] = (struct nvs_region){base[i], size[i], copy}; total += size[i];
    }
    if (nvs_test_base) { memcpy(&nvs_test_marker, phys_to_virt(nvs_test_base), 8); }
    if (nvs_count) { say("[64] PM: ACPI NVS saved: "); say_number(nvs_count); say(" region(s), "); say_number(total / 1024); say(" KiB\n"); }
    return 0;
}
static void nvs_restore(bool power_lost) {
    unsigned changed = 0;
    for (unsigned i = 0; i < nvs_count; i++) {
        uint8_t* view = nvs_view(nvs[i].base, nvs[i].size); if (!view) continue;
        if (memcmp(view, nvs[i].copy, nvs[i].size)) { changed++; memcpy(view, nvs[i].copy, nvs[i].size); }
    }
    if (nvs_count) {
        say("[64] PM: ACPI NVS restored: "); say_number(nvs_count); say(" region(s), "); say_number(changed); say(power_lost ? " changed by the firmware\n" : " changed\n");
        if (nvs_test_base) {
            uint64_t now; memcpy(&now, phys_to_virt(nvs_test_base), 8);
            say(now == nvs_test_marker ? "[64] PM: NVS test marker restored\n" : "[64] PM: NVS test marker LOST\n");
        }
    }
    nvs_release();
}
void pm_nvs_test_boot(void) {   /* kernel.c is not touched: pm_init runs before the image restore (hibernate_resume) */
    char value[24]; if (!boot_value("pm.test-nvs", value, sizeof(value))) return;
    uint64_t address = 0; for (const char* p = value[0] == '0' && (value[1] | 32) == 'x' ? value + 2 : value; *p; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
        if (d < 0) return; address = address << 4 | (uint64_t)d;
    }
    if (address < 0x80000 || address + 64 > 0xA0000) { say("[64] PM: pm.test-nvs must lie in the EBDA (0x80000-0x9FFC0)\n"); return; }
    for (unsigned i = 0; i < boot_info.memory_count && i < NEXUS_BOOT_MEMORY_MAX; i++)   /* never usable RAM the kernel owns */
        if (address < boot_info.memory[i].base + boot_info.memory[i].size && address + 64 > boot_info.memory[i].base) { say("[64] PM: pm.test-nvs lies in usable RAM; ignored\n"); return; }
    nvs_test_base = address;
    uint64_t marker = native_tsc() ^ 0x4E56535445535421ULL; memcpy(phys_to_virt(address), &marker, 8);   /* this boot's "firmware" value */
    say("[64] PM: NVS test region at "); say_number(address); say("\n");
}

/* ---- kernel lockdown (sec2's kernel_lockdown_level(); without it, Linux's lockdown= boot option) ----
 * The hibernation image is not authenticated (no TPM here to seal a key across the power-off): a locked-down kernel
 * refuses to hibernate, as Linux does without secure hibernation. */
int kernel_lockdown_level(void) __attribute__((weak));
static int lockdown(void) {
    if (kernel_lockdown_level) return kernel_lockdown_level();
    return boot_option("lockdown=confidentiality") ? 2 : boot_option("lockdown=integrity") ? 1 : 0;
}
static uint64_t rtc_seconds(void) {
    rtc_time_t t; rtc_read(&t);
    uint64_t days = 0;
    for (unsigned year = 1970; year < t.year; year++) days += 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    static const unsigned months[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (unsigned month = 1; month < t.month && month <= 12; month++) days += months[month - 1];
    if (t.month > 2 && t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0)) days++;
    return (days + t.day - 1) * 86400 + t.hour * 3600U + t.minute * 60U + t.second;
}

/* ---- S3 ---- */
static int enter_s3(void* context) {
    (void)context;
    int error = acpi_enter_sleep(3);   /* SLP_TYP/SLP_EN: with power gone this never returns */
    return error ? error : 2;          /* WAK_STS without a power loss */
}
static uint64_t sleep_monotonic, sleep_rtc;
/* After the CPU context is back (or the entry failed): clocks, timers, interrupt controllers, PCI. */
static void platform_resume(bool power_lost) {
    uint64_t rtc = rtc_seconds(), slept = rtc > sleep_rtc ? rtc - sleep_rtc : 0;
    native_clock_resume(sleep_monotonic, slept);
    pit_program();
    irq_resume();
    tick_resume();   /* the boot CPU's tick device (local APIC timer or PIT) as tick_init chose it */
    if (power_lost) pci_restore();
    irq_resume_messages();
    nvs_restore(power_lost);   /* before _WAK (acpi_leave_sleep) runs AML that reads it */
    say("[64] PM: resumed after "); say_number(slept); say(" s asleep\n");
}
static int s3(void) {
    int error = acpi_prepare_sleep(3);
    if (error) { say("[64] PM: _PTS(3) failed\n"); acpi_leave_sleep(3); return error; }
    if ((error = nvs_save())) { acpi_leave_sleep(3); return error; }
    if (!aps_offline()) { nvs_release(); acpi_leave_sleep(3); aps_online(); return -16; }
    uint64_t flags; __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    pci_save(); cpu_save();
    sleep_monotonic = native_clock_ns(); sleep_rtc = rtc_seconds();
    uint8_t* page = phys_to_virt(WAKE);
    uint64_t size = (uint64_t)(pm_wake_trampoline_end - pm_wake_trampoline);
    memcpy(page, pm_wake_trampoline, size);
    *(uint16_t*)(page + (pm_wake_gdtr - pm_wake_trampoline)) = (uint16_t)(GDT_ENTRIES * 8 - 1);
    *(uint32_t*)(page + (pm_wake_gdtr - pm_wake_trampoline) + 2) = (uint32_t)virt_to_phys(gdt64);
    *(uint32_t*)(page + (pm_wake_cr3 - pm_wake_trampoline)) = (uint32_t)virt_to_phys(boot_pml4);
    *(uint64_t*)(page + (pm_wake_entry - pm_wake_trampoline)) = (uint64_t)pm_wake_long;
    *(uint16_t*)(page + (pm_wake_vbe_mode - pm_wake_trampoline)) = 0;
    kernel_trampoline_page(TRAMPOLINE_PAGE, true);
    acpi_set_waking_vector((uint32_t)WAKE, 0);
    say("[64] PM: entering S3\n"); serial_drain();
    int entered = pm_suspend_cpu(enter_s3, NULL);
    bool power_lost = entered == 1;
    if (power_lost) say("[64] PM: woke from S3\n");
    else if (entered == 2) say("[64] PM: S3 entry returned without a power loss\n");
    else say("[64] PM: S3 entry failed\n");
    acpi_set_waking_vector(0, 0);
    kernel_trampoline_page(TRAMPOLINE_PAGE, false);
    platform_resume(power_lost);
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
    acpi_leave_sleep(3);
    acpi_events_resume();
    power_resumed();   /* the RTC alarm that woke us disarms */
    aps_online();
    return entered == 1 || entered == 2 ? 0 : -5;
}
/* ---- suspend-to-idle: devices quiesced, the boot CPU halts until a wake event (SCI, RTC alarm) ---- */
static int s2idle(void) {
    wake_event = false;
    say("[64] PM: entering suspend-to-idle\n"); serial_drain();
    uint64_t start = native_clock_ns();
    tick_s2idle(true);   /* no periodic tick while suspended to idle: only wake events interrupt (tick.c) */
    while (!wake_event) {
        uint64_t alarm = power_rtc_alarm_ns(), now = native_clock_ns();   /* an armed RTC wake alarm: one timer event */
        if (alarm != ~0ULL) tick_s2idle_timer(alarm > now ? alarm - now : 0);
        __asm__ volatile("sti; hlt; cli" ::: "memory");
        acpi_worker_poll();   /* GPE methods and Notify() handlers (lid open, buttons) still run */
        power_rtc_alarm_check();   /* a due alarm posts the RTC wake event */
    }
    tick_s2idle(false);
    __asm__ volatile("sti" ::: "memory");
    say("[64] PM: woke from suspend-to-idle after "); say_number((native_clock_ns() - start) / 1000000); say(" ms\n");
    return 0;
}

/* ---- S4: hibernation (hibernate.c writes and restores the image) ---- */
static int enter_s4(void* context) {
    (void)context;
    __asm__ volatile("sti" ::: "memory");   /* the image goes out through the running disk driver (IRQ completions) */
    snapshotting = true;                     /* process.c: no timed wake-ups or alarms while memory is being saved */
    int error = hibernate_write();
    snapshotting = false;
    if (!error) error = devices_suspend(PM_STATE_DISK, true);   /* NVMe shutdown notification, AHCI ports stopped */
    __asm__ volatile("cli" ::: "memory");
    if (error) return error;
    unsigned mode = pm_disk_mode();
    say(mode == 2 ? "[64] PM: hibernation image saved, restarting\n" : "[64] PM: hibernation image saved, powering off\n"); serial_drain();
    if (mode == 2) acpi_reset();
    if (mode == 0 && acpi_sleep_state_supported(4)) acpi_enter_sleep(4);
    platform_shutdown(0x10);   /* S5 (QEMU: isa-debug-exit): no S4, "shutdown" mode, or the S4 entry failed */
    return -5;
}
static int s4(void) {
    if (lockdown()) return locked_down(PM_STATE_DISK);   /* lockdown may have been raised since the request */
    int error = hibernate_prepare();
    if (error) { say("[64] PM: hibernation not possible (error "); say_number((uint64_t)-error); say(")\n"); return error; }
    error = acpi_prepare_sleep(4);
    if (error) { acpi_leave_sleep(4); hibernate_release(); return error; }
    if ((error = nvs_save())) { acpi_leave_sleep(4); hibernate_release(); return error; }   /* in the image: put back after the restore */
    if (!aps_offline()) { nvs_release(); acpi_leave_sleep(4); aps_online(); hibernate_release(); return -16; }
    uint64_t flags; __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    pci_save(); cpu_save();
    sleep_monotonic = native_clock_ns(); sleep_rtc = rtc_seconds();
    say("[64] PM: writing the hibernation image\n"); serial_drain();
    int entered = pm_suspend_cpu(enter_s4, NULL);
    snapshotting = false;   /* the restored image was saved with it set: timeouts and alarms run again */
    bool restored = entered == 1;
    if (restored) {   /* booted again: every driver, storage included, reprograms its device */
        say("[64] PM: restored from the hibernation image\n");
        for (unsigned i = 0; i < device_count; i++) if (device_storage[i]) device_suspended[i] = true;
    } else { say("[64] PM: hibernation failed (error "); say_number((uint64_t)-entered); say(")\n"); hibernate_invalidate(); }
    platform_resume(restored);
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
    acpi_leave_sleep(4);
    acpi_events_resume();
    power_resumed();
    aps_online();
    hibernate_release();
    return restored ? 0 : entered;
}

static int transition(unsigned state) {
    memory_file_writeback(true, 0, ~0ULL);
    ata_flush();
    int error = devices_suspend(state, false);
    if (!error) {
        if (state == PM_STATE_FREEZE) error = s2idle();
        else if (state == PM_STATE_MEM) error = s3();
        else error = s4();
    }
    devices_resume(state);
    return error;
}
static void thaw(void) {
    sched_lock(); freezing = false; sched_unlock();
    for (unsigned i = 0; i < MAX_CPUS; i++) parked[i] = false;
    kick_cpus();
}
static void complete(int value) {
    result = value; done_generation = request_generation; task_wake_channel(&done_generation);
    stage = IDLE; wake_until = native_clock_ns() + 1500000000ULL;
}
void pm_poll(void) {
    uint64_t now = native_clock_ns();
    switch (stage) {
    case IDLE:
        if (!pending) return;
        target = pending; pending = 0; stage = NOTIFY;
        /* fall through */
    case NOTIFY:
        say(target == PM_STATE_MEM ? "[64] PM: suspend (S3) requested\n" : target == PM_STATE_FREEZE ? "[64] PM: suspend-to-idle requested\n" : "[64] PM: hibernation requested\n");
        ack = false; last_event = prepare_name(target); power_notify_pm(last_event);
        { uint64_t io[3]; block_io_stats(io); notify_barrier_waits = io[1]; } ack_extended_until = 0; ack_late_reported = false;
        stage = DELAY; stage_at = now; return;
    case DELAY:
        if (!ack && now - stage_at < sleep_delay_ms * 1000000ULL) return;
        if (!ack && now - stage_at < ACK_GRACE_NS) {
            bool held = task_at_channel(1, block_io_barrier_channel());
            if (!ack_late_reported) {
                uint64_t io[3]; block_io_stats(io); ack_late_reported = true;
                say("[64] PM: no sleep_ack after "); say_number((now - stage_at) / 1000000); say(" ms (pid 1 ");
                say(held ? "held at the I/O barrier" : "not at the I/O barrier"); say(", barrier waits +"); say_number(io[1] - notify_barrier_waits); say(")\n");
            }
            if (held) ack_extended_until = now + sleep_delay_ms * 1000000ULL;
            if (now < ack_extended_until) return;
        }
        if (block_io_busy()) {   /* a task sleeps in a disk wait and owns the block path: let it finish its request */
            if (!io_wait_since) io_wait_since = now;
            if (now - io_wait_since < 20000000000ULL) return;
            say("[64] PM: a disk request did not finish within 20 s\n"); io_wait_since = 0;
            last_event = post_name(target); power_notify_pm(last_event); complete(-16); return;
        }
        sched_lock(); freezing = true; sched_unlock();
        kick_cpus();
        stage = FREEZE; stage_at = now; return;
    case FREEZE:
        if (!all_frozen()) {
            if (now - stage_at < 20000000000ULL) return;
            say("[64] PM: freezing of tasks failed after 20 s\n");
            thaw(); last_event = post_name(target); power_notify_pm(last_event); complete(-16); return;
        }
        if (block_io_busy()) {   /* a disk wait began as the freeze started (counted frozen while it sleeps): thaw, retry */
            if (!io_wait_since) io_wait_since = now;
            thaw(); stage = DELAY; ack = true; return;
        }
        io_wait_since = 0;
        say("[64] PM: tasks frozen\n");
        int error = transition(target);
        if (!error) resumes++;
        thaw();
        last_event = post_name(target); power_notify_pm(last_event);
        complete(error);
        return;
    }
}
bool pm_transition_pending(void) { return pending || stage != IDLE; }   /* power_next_event: pm_poll has work */
extern const struct pm_ops input_pm;   /* input.c: the PS/2 controller is programmed again after a power loss */
void pm_init(void) {
    pm_nvs_test_boot();
    pm_register_hooks("vga-dispi", display_suspend, display_resume);
    if (input_available(0) || input_available(1)) pm_register(&input_pm);
    extern const struct pm_ops virtio_gpu_pm; if (virtio_gpu_ready()) pm_register(&virtio_gpu_pm);   /* virgl: veto while in use; 2D KMS: rebuilt */
}
