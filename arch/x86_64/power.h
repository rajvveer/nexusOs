/* ACPI (uACPI) and power management: the internal contract between acpi_os.c (uACPI kernel API),
 * acpi_aml.c (namespace bring-up, ACPI devices, events), power.c (policy, sysfs, user-space events),
 * pm.c (suspend/resume, hibernation) and cpufreq.c (CPU idle and performance states).
 *
 * Drivers take part in system sleep with one registration (pm_register) of a struct pm_ops:
 *   suspend  - quiesce: stop DMA and interrupts, save what the device loses in D3cold. Called with the
 *              big kernel lock held, user tasks frozen, other CPUs parked, interrupts still enabled.
 *   resume   - after power returns (S3 wake or a hibernation image restore): the device was reset by the
 *              platform; reprogram it from the saved/driver state and restart its queues/streams.
 * Order: suspend in reverse registration order, resume in registration order (dependencies register first). */
#ifndef NEXUS_POWER_H
#define NEXUS_POWER_H
#include "../../kernel/types.h"

struct pm_ops {
    const char* name;
    int (*suspend)(void* context, unsigned state);   /* 0, or -errno to veto the transition */
    void (*resume)(void* context, unsigned state);
    void* context;
};
#define PM_STATE_FREEZE 1u   /* suspend-to-idle ("freeze") */
#define PM_STATE_MEM 3u      /* ACPI S3 */
#define PM_STATE_DISK 4u     /* ACPI S4 / hibernation image */
int pm_register(const struct pm_ops* ops);          /* boot-time: registry is static (PM_MAX_DEVICES) */
int pm_register_storage(const struct pm_ops* ops);  /* disk controllers: up while a hibernation image is written */
int pm_register_hooks(const char* name, void (*suspend)(void), bool (*resume)(void));   /* plain hooks in one line */

/* ---- acpi_os.c ---- */
bool acpi_os_init(uint64_t aml_bytes);               /* heap pool sized for the AML; false: no memory */
void acpi_worker_poll(void);                          /* BSP scheduler loop: run deferred ACPI work */
bool acpi_worker_busy(void);                          /* queued or running deferred work */
void acpi_os_drain(uint64_t timeout_ms);              /* any BSP context: run the worker until idle */
unsigned acpi_os_heap_pages(void);
void* acpi_os_memmove(void* dest, const void* src, __SIZE_TYPE__ n);
void* acpi_os_alloc(uint64_t bytes);                  /* the uACPI pool (zeroed); acpi_os_free with the same size */
void acpi_os_free(void* memory, uint64_t bytes);
bool acpi_os_irq_context(void);
int acpi_os_queue(void (*work)(void* context), void* context);  /* deferred work on the ACPI worker: 0 or -28 */

/* ---- acpi_aml.c ---- */
bool acpi_aml_init(void);                             /* after time_init/platform_init; false: table-only fallback */
bool acpi_aml_ready(void);
int acpi_pci_route(unsigned segment, unsigned bus, unsigned device, unsigned pin,
                   uint32_t* gsi, bool* level, bool* active_low);   /* _PRT: 0, or -errno (no route) */
bool acpi_sleep_state_supported(unsigned state);      /* \_Sx present */
int acpi_prepare_sleep(unsigned state);               /* _TTS/_PTS/_SST, SLP_TYP values */
int acpi_enter_sleep(unsigned state);                 /* writes SLP_EN; S3 returns only on failure */
void acpi_leave_sleep(unsigned state);                /* _WAK/_SST after resume or a failed entry */
void acpi_set_waking_vector(uint32_t real_mode, uint64_t protected_mode);
uint64_t acpi_facs_hardware_signature(void);
void acpi_poweroff_aml(void);                         /* _PTS(5) + S5 through uACPI; returns on failure */
void acpi_reboot_aml(void);                           /* FADT reset register through uACPI; returns on failure */
void acpi_events_resume(void);                        /* re-arm SCI/GPE/fixed events after a power loss */
void acpi_rtc_wake_enable(bool enable);              /* PM1 RTC_EN: the CMOS alarm wakes the system */
/* Processor objects (ACPI0007 devices or Processor()), in namespace order = CPU index: performance and idle states. */
struct acpi_register { uint8_t space, bit_width, bit_offset, access; uint64_t address; };   /* GAS: 0 memory, 1 I/O, 0x7F FFixedHW */
struct acpi_pstate { uint32_t mhz, power_mw, latency_us, bus_latency_us; uint64_t control, status; };
struct acpi_cstate { struct acpi_register reg; uint32_t type, latency_us, power_mw; };
unsigned acpi_processor_count(void);
int acpi_processor_pss(unsigned cpu, struct acpi_pstate* out, unsigned capacity);       /* _PSS: count or -errno */
int acpi_processor_pct(unsigned cpu, struct acpi_register* control, struct acpi_register* status);
int acpi_processor_ppc(unsigned cpu);                 /* _PPC: first usable P-state index; 0 without _PPC */
int acpi_processor_cst(unsigned cpu, struct acpi_cstate* out, unsigned capacity);       /* _CST: count or -errno */
void cpufreq_limits_changed(unsigned cpu);            /* cpufreq.c: Notify(processor, 0x80/0x81) from the ACPI worker */
void cpufreq_cstates_changed(unsigned cpu);
void cpufreq_tick(void);                              /* every CPU's timer/wake interrupt: apply its pending request */
void cpu_idle_enter(void);                            /* the scheduler's idle wait (cpuidle: HLT, MWAIT, I/O C-states) */

/* ---- power.c: events and policy ---- */
enum power_event_kind { POWER_EV_BUTTON_POWER = 1, POWER_EV_BUTTON_SLEEP, POWER_EV_LID, POWER_EV_AC, POWER_EV_BATTERY,
                        POWER_EV_THERMAL, POWER_EV_BACKLIGHT, POWER_EV_PM, POWER_EV_RTC };
void power_event(unsigned kind, unsigned device_index, uint32_t code, uint32_t data); /* any context */
void power_poll(void);                                /* BSP scheduler loop (process.c) */
void power_init(void);                                /* after acpi_aml_init and sysfs_init */
int64_t power_sysfs_read(unsigned id, char* text, unsigned capacity);
int64_t power_sysfs_write(unsigned id, const char* text, unsigned length);
void power_notify_pm(const char* event);              /* uevent PM_EVENT=<event> (see power.c) */
void power_orderly(bool reboot, const char* reason);  /* end user space gracefully, then power off / reset */
void power_rtc_alarm_fired(void);
extern bool power_finish_reboot;
unsigned pm_disk_mode(void);                          /* /sys/power/disk: 0 platform, 1 shutdown, 2 reboot */
bool pm_mem_is_deep(void);

/* ---- pm.c ---- */
int pm_suspend(unsigned state);                       /* full system sleep transition (BSP scheduler loop) */
int pm_request(unsigned state);                       /* any task: schedule a transition and wait for it */
bool pm_freezing(void);                               /* scheduler: no task may be dispatched */
void pm_cpu_park(void);                               /* AP scheduler loop: park while a transition runs */
uint64_t pm_boottime_offset_ns(void);                 /* CLOCK_BOOTTIME - CLOCK_MONOTONIC */
unsigned pm_resume_count(void);
int pm_request_async(unsigned state);                 /* schedule a transition (power_poll context): 0 or -errno */
bool pm_waking(void);                                 /* a resume finished in the last few seconds (wake press) */
void pm_poll(void);                                   /* power_poll: run a pending transition */
bool pm_transition_pending(void);                     /* a requested or running transition (pm_poll has work) */
const char* pm_last_event(void);
void pm_init(void);
bool pm_hibernation_available(void);
void pm_quiesce_all(void);                            /* hibernation restore: every driver suspended before the jump */
void pm_stop_other_cpus(void);

/* ---- tick.c: tickless idle (NO_HZ idle) ----
 * Work the boot CPU's scheduler pass polls must say when it needs that CPU next, or it only runs when the boot CPU
 * wakes for something else (at least once a second): either a next-event function called by tick_bsp_pass, or
 * tick_register_poller(name, next) at init. next(now) returns an absolute CLOCK_MONOTONIC ns (now: at once; ~0: only
 * on an interrupt). Timed waits (task_wait_until, kernel_io_sleep, nanosleep, poll timeouts, alarms) need nothing. */
void tick_init(void);                                 /* power_init: event device, PIT -> local APIC tick */
void tick_resume(void);                               /* platform_resume: timers lost their state */
uint64_t tick_idle_enter(uint64_t predicted_ns);      /* cpu_idle_enter: maybe stop the tick; ns to the next event */
bool tick_idle_exit(void);                            /* true: an early own event, re-armed: halt again */
bool tick_deep_idle_allowed(void);                    /* no ARAT: the boot CPU's local APIC tick needs C1 */
void tick_bsp_pass(bool work_pending);                /* process.c: end of the boot CPU's scheduler pass */
void tick_cpu_park(bool parked);                      /* pm.c: an AP parks for a system sleep */
void tick_s2idle(bool entering);                      /* pm.c: no tick during suspend-to-idle */
void tick_s2idle_timer(uint64_t delta_ns);            /* pm.c: during suspend-to-idle, one event delta_ns ahead */
uint64_t power_rtc_alarm_ns(void);                    /* power.c: CLOCK_MONOTONIC ns of the armed RTC wake alarm, ~0 none */
void power_rtc_alarm_check(void);                     /* power.c: post the RTC event of a due alarm (QEMU: no SCI while running) */
void tick_trap_count(unsigned vector);                /* process.c trap64: /proc/interrupts */
void tick_register_poller(const char* name, uint64_t (*next)(uint64_t now));
bool tick_nohz_enabled(void);
unsigned tick_proc_interrupts(char* out, unsigned capacity);
unsigned tick_proc_timer_list(char* out, unsigned capacity);
int64_t tick_sysfs_read(unsigned instance, unsigned attribute, char* out, unsigned capacity);
uint64_t acpi_worker_next_event(uint64_t now);        /* acpi_os.c */

/* ---- hibernate.c ---- */
bool hibernate_available(void);                       /* an area exists (partition type 0xA0 / GUID, or /nexus/hiber.img) */
int hibernate_prepare(void);                          /* before the snapshot: area, buffers, disk identity, room */
int hibernate_write(void);                            /* after the snapshot: the image, then the valid header */
void hibernate_invalidate(void);                      /* the system runs on: the image must not be restored */
void hibernate_release(void);
void hibernate_resume(void);                          /* kernel.c after block_probe: restore (no return) or discard */
#endif
