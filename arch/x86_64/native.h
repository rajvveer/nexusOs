#ifndef NEXUS_NATIVE64_H
#define NEXUS_NATIVE64_H
#include "../../kernel/types.h"
#include "../../kernel/fat32.h"
#include "blockdev.h" /* block devices, partitions, root volume (AHCI/NVMe/IDE) */
#include "vfs.h" /* routes the fat32_* file API to the mounted root filesystem (FAT32 or ext4) */
#define NATIVE_STACK_SIZE (8 * 1024 * 1024ULL)

/* ---- Symmetric multiprocessing (smp.c) ----
 * One big kernel lock: a CPU holds it whenever it runs kernel code (except while spinning for it or
 * halted idle), so kernel data needs no finer locking. It belongs to the CPU, not the task: a task
 * that blocks in the kernel may resume on another CPU. User code runs on every CPU. */
#define MAX_CPUS 32                /* keep in sync with entry.asm (GDT TSS slots, SYSCALL stubs) */
#define VEC_LAPIC_TIMER 48         /* application-processor preemption tick */
#define VEC_WAKE 49                /* IPI: leave hlt / re-enter the kernel (signals, exit) */
/* IPI: TLB shootdown (handled without the kernel lock). Highest priority class: a CPU spinning for
 * the kernel lock inside a device handler (LAPIC ISR set, e.g. the IOAPIC-routed tick) must still
 * acknowledge it, or the lock holder waiting in smp_tlb_shootdown deadlocks. */
#define VEC_TLB 0xFC
#define VEC_SPURIOUS 63
struct task;
struct address_space;
struct cpu {
    unsigned index; uint32_t apic_id;
    struct task* task;                  /* running task, NULL in this CPU's scheduler loop */
    uint64_t scheduler_rsp;             /* this CPU's scheduler context */
    uint64_t loaded_fs, loaded_gs;      /* FS/GS base in this CPU's MSRs */
    uint64_t loaded_root;               /* CR3 */
    struct address_space* loaded_space; /* space whose root is loaded (NULL: unknown/raw) */
    uint64_t fallback_top;              /* kernel stack when no task runs */
    uint64_t stack_top, ist_top;        /* application processor: scheduler and IST stacks */
    volatile bool arrived, online, idle, wake_pending;
    volatile bool resched;              /* wake IPI arrived in kernel mode: re-check preemption */
    volatile int go;
    uint64_t switches;                  /* tasks dispatched on this CPU */
    struct address_space* vm_held;      /* space lock taken with the big lock (NULL: none) */
    int64_t tsc_offset;                 /* added to this CPU's TSC to match the BSP's (0 when synchronized) */
    int64_t tsc_skew;                   /* offset measured at bring-up, before IA32_TSC_ADJUST */
    uint64_t tsc_round_trip;            /* shortest BSP<->AP ping-pong of that measurement (TSC ticks) */
    uint64_t tsc_warps, tsc_warp_max, tsc_warp_steps; /* bring-up warp test against the BSP */
};
extern bool smp_tsc_skewed;             /* some CPU needs tsc_offset: clocks read the TSC with interrupts off */
extern bool smp_rdtscp;                 /* RDTSCP present; TSC_AUX holds the CPU index */
extern bool smp_tsc_unstable;           /* warps remain after correction: clocks are clamped, vDSO clock off */
extern struct cpu cpus[MAX_CPUS];
extern unsigned cpu_count;
extern uint64_t smp_kernel_root;
/* GDT (entry.asm gdt64; per-CPU copies in segments.c) in the Linux x86-64 layout, which Wine's WoW64
 * relies on: 0x08 kernel code32 (AP trampoline, UEFI entry), 0x10 kernel code64, 0x18 kernel data,
 * 0x23 user code32 (compatibility mode), 0x2b user data, 0x33 user code64, 0x50 this CPU's LDT,
 * 0x63/0x6b/0x73 the running thread's TLS slots (set_thread_area), TSS of CPU n at 0x80 + 16 * n. */
#define KERNEL_CS 0x10
#define KERNEL_DS 0x18
#define USER32_CS 0x23
#define USER_DS 0x2B
#define USER_CS 0x33
#define TSS_SELECTOR 0x80
#define GDT_ENTRIES (16 + 2 * MAX_CPUS)
#define USER_MODE(r) (((r)->cs & 3) == 3)  /* trap frame from ring 3 (64-bit or compatibility mode) */
#define IA32_SYSCALL_VECTOR 0x180          /* trap-frame vector of int $0x80 (SYSCALL pushes 128) */
/* Each CPU loads its own TSS selector (TSS_SELECTOR + 16 * index): STR names the CPU without a VM exit. */
static inline struct cpu* cpu_self(void) {
    uint16_t selector; __asm__ volatile("str %0" : "=r"(selector));
    return &cpus[selector >= TSS_SELECTOR ? (unsigned)(selector - TSS_SELECTOR) >> 4 : 0];
}
/* User segment state of a thread (segments.c): TLS descriptors and selectors saved at a switch. */
struct user_segments { uint64_t tls[3]; uint16_t ds, es, fs, gs; };
struct registers;
void segments_cpu_init(unsigned index, uint64_t tss, uint32_t tss_limit);
void segments_switch_out(struct user_segments* s, uint64_t* fs_base, uint64_t* gs_base);
void segments_switch_in(const struct user_segments* s, struct address_space* space, uint64_t fs_base, uint64_t gs_base);
void segments_user_return(struct address_space* space); /* LDT/DS/ES refresh before ring 3 (any CPU, no lock) */
void segments_clone(struct user_segments* child, const struct user_segments* parent, bool new_tls);
void segments_exec(struct user_segments* s);
bool segments_space_clone(struct address_space* source, struct address_space* child);
void segments_space_release(struct address_space* space);
bool segments_fixup(struct registers* r);              /* trap64: kernel faults on user selectors */
struct registers* segments_int80(struct registers* r); /* int $0x80 (IA-32 system calls) */
int64_t segments_arch_prctl(uint64_t code, uint64_t argument);
int64_t segments_modify_ldt(uint64_t function, uint64_t address, uint64_t bytes);
bool segments_user_code(uint16_t selector);            /* sigreturn: valid ring-3 CS */
bool segments_user_stack(uint16_t selector);           /* sigreturn: valid ring-3 SS */
struct user_segments* task_segments(void);             /* process.c: the running thread's state */
void segments_selftest(void);
void smp_init(void);
bool kernel_lock_held(void);
void kernel_lock_acquire(void);
void kernel_lock_release(void);
void smp_send_ipi(unsigned cpu, unsigned vector);
bool smp_wake_idle(void); /* false: no CPU was idle */
void smp_tlb_shootdown(uint64_t root, bool drop);
void smp_tlb_ipi(void);
void smp_wake_ipi(void);
void lapic_eoi(void);
void cpu_local_init(unsigned index, uint64_t kernel_top, uint64_t ist_top);
void fpu_cpu_init(void);
void kernel_trampoline_page(uint64_t address, bool executable);
void space_activate_lazy(void);
void space_drop_loaded(uint64_t root);
void task_context_reload(void);
void task_scheduler_ap(void) __attribute__((noreturn));
/* Scheduler lock: runqueue, task states, wait/futex/timer queues, on_cpu, CPU idle flags.
 * Taken after the big kernel lock (never the reverse); interrupts stay off while held. */
void sched_lock(void);
void sched_unlock(void);
bool sched_lock_held(void);
void space_load_cr3(struct address_space* space); /* this CPU's CR3 only; no kernel globals */
/* Interrupts-off spinlocks (owner = CPU index + 1, 0 = free) that answer TLB shootdowns while spinning. */
void spin_acquire(volatile int* lock);
void spin_release(volatile int* lock);
bool spin_held(volatile int* lock);
void smp_tlb_poll(void); /* acknowledge a pending shootdown aimed at this CPU, if any */
void serial_drain(void);  /* write queued console output (no big lock needed) */
/* The big lock's holder owns its task's address space (page tables and VMAs) as well. */
void space_lock_task(void);
void space_unlock_held(void);
bool space_fault_fast(struct address_space* space, uint64_t address, uint64_t error); /* no big lock */
int user_read32(uint64_t address, uint32_t* out);  /* 0, or -14 if the page is not mapped (no BKL needed) */
struct signal_state;
bool signals_maybe_pending(struct signal_state* state); /* lockless hint; may be a false positive */
struct registers;
struct registers* native_syscall_finish(struct registers* r, uint64_t call);

/* ---- Platform: ACPI, interrupt routing, MSI/MSI-X, MMIO (acpi.c, irq.c) ----
 * Interrupt mode is chosen once at boot (irq_init): when the ACPI MADT reports an IOAPIC the 8259s
 * are masked and ISA lines are routed through the IOAPIC to the BSP, honouring MADT interrupt
 * source overrides (polarity/trigger); otherwise (no MADT/IOAPIC, or kernel option `noioapic`)
 * the legacy PIC stays in charge. EOI always goes through irq_eoi(vector).
 *
 * Vector map: 0x20-0x2F ISA lines (PIC mode: all; IOAPIC mode: ordinary lines), 0x30-0x3F local
 * (VEC_* above), 0x40-0x8F dynamic, 0x90-0x9F ISA lines that may wake a restricted wait (IOAPIC
 * mode: PIT, IDE), 0xA0-0xEF dynamic "wake" vectors, 0xF0-0xFF reserved (VEC_TLB), 0x80 is the
 * syscall pseudo-vector. A device vector stays in service (blocking its priority class and below
 * on that CPU) until trap64 calls irq_eoi after the handlers ran.
 *
 * Handlers run from trap64 on the CPU the interrupt was delivered to, with the big kernel lock
 * held (taken by trap64 if the CPU was in user mode or idle) and interrupts disabled. They must be
 * short: acknowledge the device, record work and wake tasks (task_wake_channel); the heavy lifting
 * belongs at scheduler/syscall boundaries, as for the NIC. Up to IRQ_SHARED_MAX handlers may share
 * a vector (shared INTx lines); each must tolerate being called when its device did not interrupt.
 *
 * MSI/MSI-X: allocate a vector (irq_alloc_vector), register the handler, then point the device at
 * it with pci_enable_msi / pci_enable_msix. Messages are edge-triggered, fixed delivery, physical
 * destination = cpus[cpu].apic_id (use cpu 0, the BSP, unless the device is serviced elsewhere).
 * These helpers also set PCI command bits for memory decoding, bus mastering (MSI writes are
 * upstream memory writes) and INTx-disable. */
#include "../../kernel/pci.h"
struct registers;
typedef void (*irq_handler_t)(unsigned vector, void* context);
#define IRQ_SHARED_MAX 4
#define IRQ_WAKE 1u        /* the vector may interrupt irq_wait_restricted() (disk completion, timer) */
#define IRQ_PCI_INTX 2u    /* irq_attach_isa: PCI INTx routed to an ISA line: level-triggered, PCI polarity */
void irq_init(void);
bool irq_ioapic_mode(void);
int irq_attach_isa(unsigned line, unsigned flags, irq_handler_t handler, void* context); /* -> vector, or -errno */
void irq_mask(unsigned line);           /* ISA line: PIC mask bit or IOAPIC redirection mask */
void irq_unmask(unsigned line);
int irq_alloc_vector(unsigned flags);   /* dynamic vector (IRQ_WAKE: wake class), or -28 */
void irq_free_vector(unsigned vector);  /* drops its handlers too */
int irq_register(unsigned vector, irq_handler_t handler, void* context);
int irq_unregister(unsigned vector, irq_handler_t handler, void* context);
bool irq_device_vector(unsigned vector);/* trap64: does this vector belong to a device/ISA line? */
void irq_dispatch(unsigned vector);     /* trap64: run the registered handlers */
void irq_eoi(unsigned vector);          /* trap64: PIC (ISA vector in PIC mode) or LAPIC EOI */
void irq_wait_restricted(void);         /* BSP: sti;hlt;cli letting only IRQ_WAKE vectors through */
uint8_t pci_find_capability(const pci_device_t* device, uint8_t id); /* config offset, 0 = absent */
uint64_t pci_bar_address(const pci_device_t* device, unsigned bar);   /* memory BAR (32/64-bit), 0 = I/O or unset */
int pci_enable_msi(pci_device_t* device, unsigned vector, unsigned cpu);
int pci_disable_msi(pci_device_t* device);
int pci_msix_count(const pci_device_t* device);                       /* table entries, or -95 */
int pci_enable_msix(pci_device_t* device, unsigned entry, unsigned vector, unsigned cpu);
int pci_disable_msix(pci_device_t* device);
volatile void* pci_ecam_address(unsigned segment, unsigned bus, unsigned device, unsigned function); /* MCFG, NULL if none */
#define MMIO_UNCACHED 0
#define MMIO_WRITE_COMBINED 1
/* Kernel virtual address for device memory: the direct map below 4 GiB; above 4 GiB (64-bit BARs, GOP
 * framebuffers on large-BAR systems) a kernel-only window in PML4 slot 510 shared by every address
 * space. Mappings are permanent; repeated calls for a covered range reuse them. */
volatile void* mmio_map(uint64_t physical, uint64_t size, unsigned cache);
void platform_init(void);               /* after memory_init: IRQ routing, MMIO window */
void platform_selftest(void);           /* MSI (QEMU edu) / MSI-X (virtio-rng) / MMIO-window checks when present */
void acpi_init(void);                   /* parse RSDP/XSDT/MADT/FADT/MCFG/DSDT (no allocation) */
void acpi_poweroff(void);               /* ACPI S5; returns only if it did not take effect */
void acpi_reset(void) __attribute__((noreturn)); /* FADT reset register, 8042 pulse, triple fault */
void platform_shutdown(unsigned code) __attribute__((noreturn)); /* final kernel exit (finish) */
int64_t platform_reboot_call(struct registers* registers);        /* reboot(2) */
/* ACPI interpreter and power management (power.h has the details): PCI INTx through the ACPI _PRT
 * (irq_attach_pci, else the firmware line), GSIs beyond the ISA lines, interrupt state after a power loss. */
int irq_attach_pci(const pci_device_t* device, unsigned flags, irq_handler_t handler, void* context);
int irq_attach_gsi(uint32_t gsi, unsigned flags, bool level, bool active_low, irq_handler_t handler, void* context);
void irq_resume(void);
void irq_resume_messages(void);
bool acpi_aml_init(void);   /* uACPI namespace, SCI, EC, ACPI devices; false: table-only mode */
void hibernate_resume(void);   /* hibernate.c: after block_probe, restore a valid hibernation image (no return) */
void power_init(void);      /* ACPI devices in /sys, power events, suspend/resume registry */
void power_poll(void);      /* boot CPU scheduler loop: ACPI worker, power events, pending sleep */
bool power_orderly_active(void);   /* power.c: an orderly shutdown/reboot is ending user space */
struct input_reader;
int rtc_device_open_check(uint64_t flags);   /* power.c: /dev/rtc0 (device.c) */
void rtc_device_open(struct input_reader* reader);
int64_t rtc_device_read(uint64_t buffer, uint64_t count, bool nonblock, struct input_reader* reader);
int rtc_device_ioctl(uint64_t command, uint64_t argument);
void* rtc_device_source(void);
uint32_t rtc_device_ready(const struct input_reader* reader);
void cpu_idle_enter(void);         /* cpufreq.c: the scheduler's idle wait (HLT, MWAIT or ACPI C-state) */
void cpufreq_tick(void);           /* cpufreq.c: apply this CPU's pending performance request (timer/wake interrupt) */
bool pm_freezing(void);     /* a system sleep transition holds every task */
bool pm_snapshotting(void); /* the hibernation image is being written: defer timeouts and alarms (pm.c) */
void pm_cpu_park(void);     /* application processor scheduler loop: park while pm_freezing() */
uint64_t pm_boottime_offset_ns(void); /* time spent suspended: CLOCK_BOOTTIME - CLOCK_MONOTONIC */
uint64_t native_suspended_ns(void);   /* kernel.c: the same offset, kept with the clocks */
void native_clock_resume(uint64_t monotonic, uint64_t slept_seconds); /* after a system sleep: monotonic continues */
void native_vdso_clock_update(void);  /* republish clock parameters (after APs re-synchronized their TSCs) */
void smp_restart(void);               /* smp.c: start the application processors again after a system sleep */
extern bool smp_restarting;           /* smp.c: set while smp_restart runs */
void pm_cpu_online(unsigned index);   /* ap_main: restore what a system sleep reset (pm.c) */

struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, flags, rsp, ss;
};
bool user_range(uint64_t address, uint64_t size);
bool accessible(uint64_t address, uint64_t size, bool write);
#include "uaccess.h"   /* SMAP: every user access goes through copy_to_user, copy_from_user, uget32, uput32... */
/* ---- Address layout (memory.c) ----
 * Lower half (PML4 entries 0-255): user space only, 0x10000 .. 0x7FFFFFFFF000 (each space's own tables).
 * Upper half (PML4 entries 256-511), shared by every address space:
 *   DIRECT_MAP_BASE     every physical page: the low 4 GiB (2 MiB pages, device memory included) and all RAM
 *   0xFFFFFF0000000000  MMIO window for device memory above 4 GiB (irq.c mmio_map)
 *   KERNEL_VMA          the kernel image (top 2 GiB), linked at KERNEL_VMA + its physical address
 * The boot identity map exists only until protect_kernel() and while application processors start.
 * page_alloc/arena_alloc/dma32_alloc return kernel virtual (direct-map) addresses; page tables and devices
 * take physical ones (virt_to_phys, frame_alloc). page_free/page_retain/page_references accept either form. */
#define DIRECT_MAP_BASE 0xFFFF800000000000ULL
#define KERNEL_VMA 0xFFFFFFFF80000000ULL
#ifndef NEXUS_HOST_TEST
static inline void* phys_to_virt(uint64_t physical) { return (void*)(physical + DIRECT_MAP_BASE); }
static inline uint64_t virt_to_phys(const volatile void* address) {
    uint64_t a = (uint64_t)address; return a >= KERNEL_VMA ? a - KERNEL_VMA : a - DIRECT_MAP_BASE;
}
#else /* host-side driver tests (tests/hda_codec_test.c): simulated devices take pointers as bus addresses */
static inline void* phys_to_virt(uint64_t physical) { return (void*)physical; }
static inline uint64_t virt_to_phys(const volatile void* address) { return (uint64_t)address; }
#endif
uint64_t page_alloc(void);
uint64_t frame_alloc(void);                /* zeroed page by physical address (page tables, user frames); 0 = none */
void page_free(uint64_t address);
void page_retain(uint64_t address);
uint32_t page_references(uint64_t address);
uint64_t arena_alloc(uint64_t bytes);
uint64_t dma32_alloc(uint64_t bytes);      /* arena_alloc whose pages lie below 4 GiB (32-bit DMA devices) */
void arena_free(uint64_t address, uint64_t bytes);
void memory_init(void);
void memory_selftest(void);
uint64_t memory_used_pages(void);
uint64_t memory_total_pages(void);         /* managed RAM pages (all zones) */
unsigned memory_zone_view(unsigned index, uint64_t* begin, uint64_t* end, const uint32_t** references); /* hibernate.c */
bool memory_page_claim(uint64_t physical);  /* a free pool page becomes allocated (hibernation restore) */
bool memory_kernel_buffer(const void* buffer, uint64_t bytes); /* direct-mapped RAM or the kernel image: DMA-able as is */
/* Physical memory as user space sees it (sysinfo, /proc/meminfo), in bytes. */
void native_memory_info(uint64_t* total, uint64_t* free, uint64_t* cached);
unsigned task_count(void);
void panic(const char* message) __attribute__((noreturn));
struct address_space;
struct descriptor_table;
struct filesystem_state;
struct open_file;
struct lock_file_info { uint64_t device, inode, position, size, flags; };
int fd_lock_info(struct open_file* file, struct lock_file_info* info);
int64_t file_lock_call(struct open_file* file, uint32_t fd, uint32_t command, uint64_t argument, bool whole_file);
void file_locks_close(struct open_file* file, uint32_t pid);
void file_locks_release(struct open_file* file);
void file_locks_exit(uint32_t pid);
struct memory_block;
struct linux_stat;
struct memory_file {
    struct memory_file* next;
    struct memory_block* blocks;
    uint64_t size, inode, pages, changed;
    uint64_t index_pages, valid_size;
    unsigned references, seals, mode;
    uint32_t uid, gid;
    bool disk_backed, metadata_dirty;
    int io_error;
    fat32_file_t disk;
    uint64_t dirty_since, dirty_pages; /* write-back state (dirty_since: native_clock_ns()|1) */
    unsigned exec_maps;                /* live file-backed ELF image segments of this inode */
    unsigned writable_maps;            /* live shared mappings that may write */
    unsigned maps;                     /* live VMAs using this cache */
    struct open_file* views;           /* descriptions using this cache */
    struct memory_file* shmem_prev; bool swappable; /* shared memory whose pages may go to swap (shmem.c list via next) */
};
struct memory_file* memory_file_create(unsigned flags);
void memory_file_retain(struct memory_file* file);
void memory_file_release(struct memory_file* file);
uint64_t memory_file_page(struct memory_file* file, uint64_t index, bool create);
/* Page-fault path without the big lock: a retained resident page (shared memory: may create a zero
 * page), or 0. The caller's mapping keeps `file` alive. */
uint64_t memory_file_fault_page(struct memory_file* file, uint64_t index, bool create);
int64_t memory_file_io(struct memory_file* file, uint64_t address, uint64_t count, uint64_t offset, bool write);
int memory_file_truncate(struct memory_file* file, uint64_t size);
int memory_file_seal(struct memory_file* file, uint64_t seals);
int memory_file_chmod(struct memory_file* file, uint64_t mode);
bool memory_file_access(struct memory_file* file, bool write);
void memory_file_stat(struct memory_file* file, struct linux_stat* stat);
bool memory_file_writable_maps(struct memory_file* file);
void memory_file_invalidate(struct memory_file* file, uint64_t size);
struct memory_file* memory_file_disk_find(const fat32_file_t* disk);
struct memory_file* memory_file_disk_get(const fat32_file_t* disk);
uint64_t memory_file_disk_pages(void);
void memory_file_dirty(struct memory_file* file, uint64_t index);
void memory_file_writeprotect(struct memory_file* file, uint64_t index);
void memory_file_writeprotect_range(struct memory_file* file, uint64_t first, uint64_t count);
uint64_t memory_file_dirty_total(void);
uint64_t memory_file_dirty_files(void);
int memory_file_writeback(bool all, uint64_t min_age_ns, uint64_t budget_bytes);
void native_writeback_poll(bool idle);
void memory_file_refresh_views(struct memory_file* file);
void native_gc_poll(void);
bool ipc_rights_inflight(void);
/* Extended FPU state: xsave_mask is XCR0 when XSAVE is enabled, else 0 (FXSAVE only). */
#define FPU_AREA_BYTES 1024
extern uint64_t xsave_mask; extern uint32_t xsave_size;
static inline void fpu_save(void* area) {
    if (xsave_mask) __asm__ volatile(".byte 0x48,0x0f,0xae,0x27" :: "D"(area), "a"((uint32_t)xsave_mask), "d"((uint32_t)(xsave_mask >> 32)) : "memory");
    else __asm__ volatile("fxsave64 (%0)" :: "r"(area) : "memory");
}
static inline void fpu_restore(const void* area) {
    if (xsave_mask) __asm__ volatile(".byte 0x48,0x0f,0xae,0x2f" :: "D"(area), "a"((uint32_t)xsave_mask), "d"((uint32_t)(xsave_mask >> 32)) : "memory");
    else __asm__ volatile("fxrstor64 (%0)" :: "r"(area) : "memory");
}
int64_t task_sched_call(struct registers* r);
void task_sched_yield(void);
void task_set_name(const char* name);
void task_get_name(char* name);
void memory_file_writeback_init(void);
void memory_file_cache_stats(uint64_t* misses, uint64_t* readahead);
void native_io_poll(void);
struct kernel_counters { uint64_t switches, syscalls, faults, irqs, syncs, sync_pages, flush_passes, opens, closes, forks; };
extern struct kernel_counters kstat;
int memory_file_sync(struct memory_file* file, uint64_t begin, uint64_t end);
int disk_file_touch(fat32_file_t* file);
void file_refresh_views(const fat32_file_t* file);
uint64_t disk_inode_id(const fat32_file_t* file);
bool disk_same_inode(const fat32_file_t* a, const fat32_file_t* b);
int disk_inode_reserve(unsigned count);
void disk_inode_rekey(const fat32_file_t* old, fat32_file_t* moved);
uint64_t disk_inode_pages(void);
struct disk_orphan;
struct disk_orphan* disk_orphan_prepare(void);
void disk_orphan_cancel(struct disk_orphan* orphan);
void disk_orphan_adopt(struct disk_orphan* orphan, const fat32_file_t* file);
void disk_orphan_refresh(const fat32_file_t* file);
void disk_orphan_collect(void);
bool disk_inode_live(const fat32_file_t* file);
void memory_file_disk_moved(const fat32_file_t* old, const fat32_file_t* moved);
struct filesystem_state* filesystem_current(void);
struct filesystem_state* filesystem_clone(struct filesystem_state* source, bool share);
void filesystem_activate(struct filesystem_state* state);
void filesystem_release(struct filesystem_state* state);
unsigned filesystem_users(struct filesystem_state* state);   /* tasks sharing it (CLONE_FS); setns(CLONE_NEWUSER) */
bool filesystem_chrooted(void);
struct open_file* filesystem_directory(struct filesystem_state* state, bool root);
const char* filesystem_path(struct filesystem_state* state, bool root);
const char* filesystem_visible_path(const char* path);
struct signal_state;
struct credentials;
struct user_namespace;
struct credentials* credentials_current(void);
void credentials_boot_root(bool root);  /* the next boot session runs as root (test disks) */
void credentials_activate(struct credentials* credentials);
struct credentials* credentials_clone(struct credentials* source, bool new_user_namespace, int* error);
struct credentials* credentials_exec(struct credentials* source);
bool credentials_exec_secure(bool file_setid);   /* AT_SECURE for the execve being prepared */
void credentials_destroy(struct credentials* credentials);
uint32_t credentials_id(struct credentials* credentials, bool group, unsigned which);
uint32_t credentials_kernel_id(struct credentials* credentials, bool group, unsigned which);
uint32_t credentials_visible_id(uint32_t id, bool group);
struct user_namespace* credentials_namespace(struct credentials* credentials);
bool credentials_capable(struct credentials* credentials, struct user_namespace* space, unsigned capability);
bool credentials_signal_allowed(struct credentials* target);
bool credentials_inspect_allowed(struct credentials* target, bool dumpable);
bool credentials_limits_allowed(struct credentials* target);
int64_t credentials_call(struct registers* registers);
void credentials_selftest(void);
void namespace_retain(struct user_namespace* space);
void namespace_release(struct user_namespace* space);
uint64_t namespace_id(struct user_namespace* space);
bool namespace_mapping(struct user_namespace* space, struct user_namespace* reader, bool group, unsigned index, uint32_t values[3]);
bool namespace_groups_allowed(struct user_namespace* space);
int namespace_write_map(struct user_namespace* space, struct credentials* opener, bool group, const char* text, unsigned size, uint64_t offset);
int namespace_write_groups(struct user_namespace* space, struct credentials* opener, const char* text, unsigned size, uint64_t offset);
struct pid_namespace {
    struct pid_namespace* parent;
    struct user_namespace* owner;
    uint64_t id;
    unsigned references, depth;
    uint32_t last, init;
    bool dead;
};
struct pid_identity {
    struct pid_identity* next;
    struct pid_namespace* space;
    unsigned references;
    bool published;
    uint32_t numbers[33];
};
struct pid_namespace* pid_namespace_root(void);
struct pid_namespace* pid_namespace_create(struct pid_namespace* parent, struct credentials* owner, int* error);
void pid_namespace_retain(struct pid_namespace* space);
void pid_namespace_release(struct pid_namespace* space);
bool pid_namespace_contains(struct pid_namespace* ancestor, struct pid_namespace* space);
struct pid_identity* pid_allocate(struct pid_namespace* space);
void pid_publish(struct pid_identity* pid);
void pid_retain(struct pid_identity* pid);
void pid_release(struct pid_identity* pid);
struct pid_identity* pid_lookup(uint32_t number, struct pid_namespace* space);
uint32_t pid_number(struct pid_identity* pid, struct pid_namespace* space);
void pid_reset(void);
void pid_selftest(void);
struct address_space* space_current(void);
bool space_dumpable(struct address_space* space);
void space_set_dumpable(struct address_space* space, bool dumpable);
struct address_space* space_clone(struct address_space* source);
void space_activate(struct address_space* space);
void space_retain(struct address_space* space);
void space_release(struct address_space* space);
void kernel_guard_page(uint64_t address, bool guard);
void cpu_task_stack(uint64_t top);
uint64_t native_clock_ns(void);
uint64_t native_realtime_ns(void);
uint64_t native_tsc(void); /* this CPU's TSC corrected to the boot CPU's (see smp_tsc_skewed) */
bool native_random_available(void);
bool native_random(void* output, uint64_t length);
struct framebuffer_info { uint64_t physical; uint32_t width, height, stride, size, format; };   /* format: NEXUS_FB_XRGB8888/XBGR8888 */
const struct framebuffer_info* framebuffer_get(void);
void hwinfo_display_report(uint64_t physical, uint32_t width, uint32_t height, uint32_t stride, uint32_t format, bool write_combining);
uint64_t hw_tsc_calibrate(bool pm_first, const char** source);   /* hwinfo.c: HPET, ACPI PM timer (no PIT) */
void pci_bar_fixup(void);   /* pcifix.c: memory BARs the firmware left unassigned (behind bridges) */
void diag_log_char(char c); /* diag.c: kernel log ring for the diagnostics report */
void diag_run(void);        /* diag.c: boot option `diag` -> NEXUSDIAG.TXT on the boot medium + summary */
int diagfat_write(const uint8_t* data, uint64_t bytes, char* where, unsigned size);   /* diagfat.c */
const pci_device_t* hda_pci(void);   /* hda.c: the controller driving the PCM, NULL if none */
void framebuffer_retain(void);
void framebuffer_release(void);
struct input_reader { uint64_t sequence; int clock; bool report; uint32_t seat; };
/* seat.c: the console user (logind-style device access for the one seat; pid 1 sets it on session switches) */
uint32_t seat_console_uid(void);
bool seat_device_access(void);                 /* root, pid 1 or the console user may open console devices */
uint32_t seat_open_token(void);                /* stored in an input reader at open */
bool seat_revoked(uint32_t token);             /* the console user changed since (not for root/pid 1 readers) */
int64_t seat_prctl(uint64_t operation, uint64_t value);   /* prctl(PR_NEXUS_SEAT, 0 get | 1 set, uid) */
#define PR_NEXUS_SEAT 0x4E585301
void input_init(void);
void input_irq(void);
bool native_nic_init(void);
void native_nic_poll(void);
void native_nic_irq(unsigned vector);
bool native_nic_rx_pending(void);
void native_nic_rx_mode(bool promiscuous, bool all_multicast, const uint8_t* groups, unsigned count);   /* netdev.c filter */
void native_nic_late_init(void);   /* after the root mount: NIC firmware from /lib/firmware (nic.c) */
void native_nic_suspend(void);     /* system sleep hooks for the active NIC (nic.c) */
bool native_nic_resume(void);
/* USB host controllers and devices (usb.c): init at boot, poll from the BSP loop, IRQ from trap64. */
void hwnode_init(void);                /* hwnode.c: /dev/uinput, virtual test camera (boot option vcam) */
void hwnode_poll(uint64_t now);        /* hwnode.c: cameras, force-feedback durations */
bool bluetooth_descriptor(struct open_file* description);   /* bluetooth.c: AF_BLUETOOTH sockets, AF_ALG */
int64_t bluetooth_call(struct registers* registers);
struct iovec64;
bool hwnode_write_vectors(unsigned device, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags, struct input_reader* reader, int64_t* result);   /* hwnode.c: writev = one message */
void usb_init(void);
void usb_poll(void);
void usb_irq(unsigned vector);
/* Extra evdev sources (USB HID) merge into the keyboard (index 0) and pointer (index 1) nodes. */
void input_source_attach(unsigned index, const uint8_t* key_bits, bool absolute);
void input_source_key(unsigned index, unsigned code, int value);
void input_source_motion(unsigned type, unsigned code, int value);
void input_source_report(unsigned index);
void native_network_init(void);
void native_network_poll(void);
int ip_loopback_pending(void);    /* kernel/ip.c: loopback packets waiting for the network poll */
/* ptrace (process.c) and cross-process memory (kernel.c) */
int64_t task_ptrace(uint64_t request, int64_t pid, uint64_t address, uint64_t data);
int64_t task_itimer(uint64_t call, uint64_t which, uint64_t value, uint64_t old);   /* alarm, getitimer, setitimer */
int task_ptrace_stop(struct registers* r, int number);
struct address_space* task_space(uint32_t tid);
int task_set_name_tid(uint32_t tid, const char* name);
int64_t space_access(struct address_space* space, uint64_t address, void* buffer, uint64_t count, bool write);
bool native_network_available(void);
uint64_t native_network_background_pages(void);
/* DHCP client (dhcp64.c): started by native_network_init, driven by native_network_poll. */
void native_dhcp_start(void);
void native_dhcp_poll(void);
void native_dhcp_receive(const void* data, uint16_t length);
bool native_dhcp_bound(void);
unsigned native_dhcp_resolv(char* text, unsigned capacity);
uint64_t native_dhcp_resolv_stamp(unsigned* length);
bool native_network_selftest(uint16_t port);
bool input_available(unsigned index);
void input_open(unsigned index, struct input_reader* reader);
void* input_source(unsigned index);
uint32_t input_ready(unsigned index, const struct input_reader* reader);
int64_t input_read(unsigned index, struct input_reader* reader, uint64_t buffer, uint64_t count, bool nonblock);
int input_ioctl(unsigned index, struct input_reader* reader, uint64_t command, uint64_t argument);
int device_lookup(const char* path);
int device_open_check(unsigned device, uint64_t flags);
uint64_t device_open(unsigned device, uint64_t flags, struct input_reader* reader);
void device_close(unsigned device, uint64_t flags, struct input_reader* reader);
int64_t device_io(unsigned device, uint64_t buffer, uint64_t count, bool write, uint64_t* position, uint64_t flags, struct input_reader* reader);
int64_t device_seek(unsigned device, uint64_t* position, int64_t offset, uint64_t whence, struct input_reader* reader);
int device_ioctl(unsigned device, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags);
bool device_framebuffer(unsigned device);
struct memory_file* device_mapping(unsigned device, uint64_t* offset, uint64_t length, struct input_reader* reader);
bool sound_init(void);
bool sound_available(void);
bool sound_capture_available(void);   /* pcmC0D0c: the engine has an input path */
void sound_poll(void);
int sound_open_check(unsigned node, uint64_t flags);
void sound_open(unsigned node, struct input_reader* reader);
void sound_close(unsigned node, struct input_reader* reader);
int64_t sound_ioctl(unsigned node, uint64_t command, uint64_t argument, uint64_t flags, struct input_reader* reader);
int64_t sound_io(unsigned node, uint64_t buffer, uint64_t count, bool write, uint64_t flags, struct input_reader* reader);
uint32_t sound_ready(unsigned node, const struct input_reader* reader);
void* sound_source(unsigned node);
struct memory_file* sound_mapping(unsigned node, uint64_t offset, uint64_t length, const struct input_reader* reader);
void* device_event_source(unsigned device, const struct input_reader* reader);
uint32_t device_ready(unsigned device, const struct input_reader* reader);
bool device_pollable(unsigned device);
struct futex_key { uint64_t domain, word; unsigned kind; };
int memory_futex_key(uint64_t address, bool private, struct futex_key* key);
/* seccomp (seccomp.c): the syscall-entry hook, prctl/seccomp(2) entry, and the per-task state helpers it
 * reaches in process.c. seccomp_enforce returns true when the syscall must not run (*out is the frame to
 * return, NULL kills the thread), false to let native_dispatch run it. arch = AUDIT_ARCH_X86_64 or _I386. */
struct seccomp_filter;
bool seccomp_enforce(struct registers* r, struct registers** out);
int64_t seccomp_syscall(struct registers* r);
int64_t seccomp_set_mode_prctl(uint64_t mode, uint64_t argument);
int64_t seccomp_get_mode(void);
void seccomp_filter_retain(struct seccomp_filter* f);
void seccomp_filter_release(struct seccomp_filter* f);
void seccomp_filter_chain(struct seccomp_filter* f, struct seccomp_filter* prev);
unsigned seccomp_filter_count(struct seccomp_filter* f);
bool seccomp_filter_is_ancestor(struct seccomp_filter* maybe_ancestor, struct seccomp_filter* chain);
unsigned seccomp_actions_text(bool logged, char* out, unsigned capacity);   /* /proc/sys/kernel/seccomp/actions_* */
int seccomp_actions_logged_write(const char* text);
uint8_t task_seccomp_mode(void);
struct seccomp_filter* task_seccomp_head(void);
void task_seccomp_set(uint8_t mode, struct seccomp_filter* head);
int task_seccomp_tsync(struct seccomp_filter* added);
void task_seccomp_kill(int signal, bool group);
void task_seccomp_info(uint32_t tid, int* mode, unsigned* filters);
bool task_alive(uint32_t tid);
bool credentials_no_new_privs(void);
void credentials_mark_no_new_privs(struct credentials* c);
/* ASLR: randomize_va_space sysctl (kernel.c) and per-task personality (process.c). */
int native_get_randomize(void);
void native_set_randomize(int value);
int native_get_kptr_restrict(void);
void native_set_kptr_restrict(int value);
int native_get_dmesg_restrict(void);
void native_set_dmesg_restrict(int value);
uint32_t task_personality(void);
int64_t task_personality_call(uint64_t persona);
void task_set_ptracer(int64_t pid);   /* Yama PR_SET_PTRACER */
int native_yama_scope(void);          /* /proc/sys/kernel/yama/ptrace_scope */
uint64_t task_net_namespace(void);    /* 0 = root network namespace; non-zero = isolated (CLONE_NEWNET) */
/* network / IPC namespaces (namespaces.c): reference-counted slots, 0 = the initial namespace */
enum { NSOBJ_NET, NSOBJ_IPC };
int nsobj_create(unsigned kind, struct user_namespace* owner);   /* new slot or -ENOSPC */
void nsobj_retain(unsigned kind, unsigned slot);
void nsobj_release(unsigned kind, unsigned slot);
uint64_t nsobj_inode(unsigned kind, unsigned slot);
int task_nsobj_of(uint32_t tid, unsigned kind);   /* /proc/PID/ns/{net,ipc}: the task's slot, or -ENOENT */
uint32_t* task_nsobj_slot(unsigned kind);          /* the current process's slot (setns) */
int task_set_children_namespace(struct pid_namespace* space);
int task_enter_user_namespace(struct user_namespace* space);
struct credentials* credentials_enter(struct credentials* source, struct user_namespace* target, int* error);
int namespace_setns(unsigned long type, unsigned slot, struct user_namespace* user, struct pid_namespace* pid, uint64_t nstype);
struct proc_handle;
int proc_setns(struct proc_handle* handle, uint64_t nstype);   /* setns(2) */
extern bool sandbox_trace;   /* `sbxtrace` boot option (seccomp.c) */
/* Kernel hardening (hardening.c): SMEP/SMAP/UMIP per CPU, W^X audit, stack canary, boot report. */
void security_cpu_init(void);
bool security_verify_wx(void);
void hardening_report(void);
int umip_emulate(struct registers* r);   /* ring-3 #GP: emulate SGDT/SIDT/SLDT/SMSW/STR (umip.c; 1 if handled) */
/* Kernel lockdown (lockdown.c): integrity/confidentiality, from Secure Boot or the `lockdown=` boot option. */
void lockdown_init(const char* option);
void lockdown_selftest(void);
int kernel_lockdown_level(void);
bool kernel_lockdown_active(void);
bool kernel_lockdown_confidential(void);
bool kernel_lockdown_denied(const char* reason, bool confidential);
bool kernel_lockdown_allows_kernel_image(bool authenticated);
int kernel_lockdown_set(int level);
unsigned kernel_lockdown_text(char* out, unsigned capacity);
int kernel_lockdown_write(const char* text);
extern bool umip_enabled;
extern bool smap_enabled;
int fd_install_into(uint32_t tid, uint32_t srcfd, uint32_t newfd, bool setfd, bool close_exec);
struct registers* signal_seccomp_sigsys(struct registers* r, int nr, uint32_t arch, uint64_t call_addr, int errno_data);
struct registers* native_dispatch(struct registers* registers);

int task_run(uint64_t entry, uint64_t stack, struct address_space* space,
             struct descriptor_table* descriptors, struct signal_state* signals, const char* executable);
int64_t task_clone(struct registers* registers);
int task_unshare(uint64_t flags);
int task_exec(struct registers* registers, uint64_t entry, uint64_t stack, struct address_space* space, const char* executable, uint64_t inode, bool deleted);
int64_t task_wait4(int64_t pid, uint64_t status, uint64_t options, uint64_t usage);
int task_waitid(uint64_t kind, uint64_t id, uint64_t info, uint64_t options, uint64_t usage);
void task_yield(void);
int task_wait_channel(void* channel);
int task_wait_until(void* channel, uint64_t deadline);
void task_wake_channel(void* channel);
/* Internal IDs serve the scheduler and the initial procfs mount. */
uint32_t task_pid(void);
uint32_t task_tid(void);
uint32_t task_parent_pid(void);
struct pid_namespace* task_pid_namespace(void);
struct pid_namespace* task_children_namespace(uint32_t tid);
uint32_t task_visible_pid(uint32_t internal);
uint32_t task_internal_pid(uint32_t visible);
uint32_t task_uid(void);
uint32_t task_gid(void);
uint32_t task_kernel_uid(void);
struct credentials* task_credentials(uint32_t tid);
uint32_t task_process_id(uint32_t tid);
uint32_t task_next_id(uint32_t group, uint32_t after);
unsigned task_thread_count(uint32_t group);
struct descriptor_table* task_files(uint32_t tid);
struct filesystem_state* task_filesystem(uint32_t tid);
const char* task_executable(uint32_t tid);
bool task_executable_deleted(uint32_t tid);
int task_files_moved(const char* from, const char* to, uint64_t inode, bool directory, bool apply);
int path_moved(char* path, const char* from, const char* to, bool apply);
bool task_inspect_allowed(uint32_t tid);
bool task_is_dumpable(uint32_t tid);
bool task_active(void);
uint64_t task_cpu_time(bool thread);
uint64_t task_resource_soft(unsigned resource);
uint64_t task_file_size_limit(void);      /* RLIMIT_FSIZE soft limit of the caller */
int task_resource_limit(uint32_t pid, uint32_t resource, uint64_t next, uint64_t previous);
bool task_exiting(void);
void task_exit_group(int status);
bool task_kguard_containable(void);               /* kguard: a task is to blame and its locks can be unwound */
void task_kguard_kill(void) __attribute__((noreturn));  /* kguard: SIGKILL the current task and leave to the scheduler */
bool task_kernel_stack_overflow(uint64_t fault_address, uint64_t rsp);
int task_signal(int64_t pid, uint64_t number, int code);
int task_thread_signal(int32_t group, int32_t tid, uint64_t number);
int64_t task_futex(struct registers* registers);
int64_t task_session(uint64_t call, int64_t pid, int64_t group);
void task_set_clear_tid(uint64_t address);
void native_set_executable(const char* path);
void native_exit_trace(struct registers* registers, unsigned status);
extern unsigned user_termination_signal;
extern int user_result;
void reset_user_fpu(void);

struct open_file;
struct linux_stat {
    uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev;
    int64_t size, block_size, blocks, atime; uint64_t atime_ns;
    int64_t mtime; uint64_t mtime_ns; int64_t ctime; uint64_t ctime_ns, reserved[3];
};
struct directory_item { uint64_t inode, next; uint8_t type; char name[256]; };
void device_stat(unsigned device, struct linux_stat* stat);
void device_description_stat(unsigned device, const struct input_reader* reader, struct linux_stat* stat);
int device_directory(unsigned device, uint64_t position, struct directory_item* item);
struct proc_handle;
bool proc_path(const char* path);
struct proc_handle* proc_open(const char* path, uint64_t flags, int* error);
struct proc_handle* proc_clone(struct proc_handle* source, uint64_t flags, int* error);
void proc_close(struct proc_handle* handle);
void proc_release_dead_images(void);   /* after a task left its address space: unpin images no task runs */
int proc_stat(struct proc_handle* handle, struct linux_stat* stat);
int proc_path_stat(const char* path, bool follow, struct linux_stat* stat);
int64_t proc_read(struct proc_handle* handle, uint64_t address, uint64_t count, uint64_t offset);
int64_t proc_write(struct proc_handle* handle, uint64_t address, uint64_t count, uint64_t offset);
int proc_directory(struct proc_handle* handle, uint64_t position, struct directory_item* item);
int proc_readlink(const char* path, char* result, unsigned capacity);
int proc_magic_target(const char* path, char* result, unsigned capacity, struct open_file** bound);
int proc_descriptor_path(struct proc_handle* handle, char* result, unsigned capacity);
struct open_file* fd_lookup_table(struct descriptor_table* table, uint64_t fd);
int fd_next_number(struct descriptor_table* table, uint32_t minimum);
int fd_description_stat(struct open_file* description, struct linux_stat* stat);
int fd_information(struct descriptor_table* table, uint32_t fd, uint64_t values[4]);
unsigned fd_information_text(struct descriptor_table* table, uint32_t fd, char* out, unsigned capacity);   /* fdinfo.c lines */
unsigned kfmt(char* out, unsigned capacity, unsigned used, const char* format, ...);   /* fdinfo.c: small formatter */
uint32_t fdinfo_sdev(uint64_t dev);
const char* fd_internal_path(struct open_file* description);
int disk_path_stat(const char* path, struct linux_stat* stat);
int fd_description_path(struct open_file* description, char* result, unsigned capacity);
void number64(uint64_t value, char* text);
struct iovec64 { uint64_t address, size; };
struct message64 { uint64_t name; uint32_t name_size, pad; uint64_t vectors, count, control, control_size; uint32_t flags, pad2; };
struct inet_socket;
int fd_create_inet(struct inet_socket* socket, uint64_t flags);
struct inet_socket* fd_inet(struct open_file* file, uint64_t* flags);
int64_t inet_call(struct registers* registers);
/* netlink.c: AF_NETLINK sockets (rtnetlink reads, uevent), SIOCGIF* ioctls, /proc/net text */
int64_t netlink_call(struct registers* registers);
bool netlink_descriptor(struct open_file* description);
int64_t packet_call(struct registers* registers);                 /* AF_PACKET (packet.c) */
extern unsigned packet_socket_count;
bool packet_descriptor(struct open_file* description);
struct memory_file* packet_mapping(struct open_file* description, uint64_t* offset, uint64_t length);
int tun_open(uint64_t flags);                                      /* /dev/net/tun (tun.c) */
int netif_ioctl(uint64_t command, uint64_t argument);
int64_t inet_ioctl(struct inet_socket* socket, uint64_t command, uint64_t argument);   /* inet.c */
bool netif_proc_text(const char* name, char* out, unsigned capacity);
uint64_t fd_status_flags(struct open_file* description);
int64_t inet_socket_call(struct inet_socket* socket, uint64_t status_flags, struct registers* registers);
int64_t inet_vectors(struct inet_socket* socket, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags);
void inet_close(struct inet_socket* socket);
void inet_poll(void);
uint64_t inet_background_pages(void);
void inet_datagram_receive(uint32_t source, uint32_t destination, uint16_t source_port, uint16_t destination_port, const void* data, uint16_t length);
void inet_icmp_error(uint32_t reporter, const void* data, uint16_t length);
uint32_t inet_ready(struct inet_socket* socket);
unsigned inet_available(struct inet_socket* socket);
bool iov_access(const struct iovec64* vectors, unsigned count, uint64_t skip, uint64_t length, bool write);
void iov_copy(const struct iovec64* vectors, unsigned count, uint64_t skip, uint8_t* data, unsigned length, bool write);
struct pipe_end;
int fd_create_pipe(struct pipe_end* end, bool write, uint64_t flags);
int pipe_create(uint64_t descriptors, uint64_t flags);
int64_t pipe_io(struct pipe_end* end, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, bool nonblock);
void pipe_close(struct pipe_end* end);
struct pipe_end* pipe_reopen(struct pipe_end* any, bool write);   /* open through /proc/PID/fd/N */
struct open_file;
struct open_file* proc_fd_object(const char* path);              /* /proc/PID/fd/N -> the open file, else NULL */
int64_t pipe_control(struct pipe_end* end, uint64_t command, uint64_t argument);
uint64_t pipe_identity(struct pipe_end* end);
unsigned pipe_available(struct pipe_end* end);
uint32_t pipe_ready(struct pipe_end* end);
struct pipe_end* pipe_create_named(void);                          /* named pipes (fifo.c) */
bool pipe_end_open(struct pipe_end* any, bool write);
unsigned pipe_writers_seen(struct pipe_end* any);
int pipe_wait(struct pipe_end* any);
int fifo_open(uint64_t identity, uint64_t flags, const char* path); /* open(2) of a FIFO inode */
bool fifo_description(struct open_file* description);
struct event_poll;
int fd_create_epoll(struct event_poll* poll, bool close_exec);
struct event_poll* fd_epoll(struct open_file* description);
bool fd_pollable(struct open_file* description);
void* fd_event_source(struct open_file* description);
uint32_t fd_ready(struct open_file* description);
int64_t event_call(struct registers* registers);
void event_source_changed(void* source, uint32_t events);
void event_forget(struct open_file* description);
void event_destroy(struct event_poll* poll);
uint32_t event_ready(struct event_poll* poll);
struct event_counter;
int fd_create_counter(struct event_counter* counter, uint64_t flags);
void counter_destroy(struct event_counter* counter);
uint32_t counter_ready(struct event_counter* counter);
int64_t counter_io(struct event_counter* counter, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, bool nonblock);
struct descriptor_table;
struct descriptor_table* fd_table_current(void);
struct descriptor_table* fd_table_new(bool standard);
struct descriptor_table* fd_table_clone(struct descriptor_table* source, bool share);
void fd_table_activate(struct descriptor_table* table);
void fd_table_release(struct descriptor_table* table);
void fd_close_exec(void);
struct unix_socket;
struct open_file* fd_lookup(uint64_t fd);
void fd_reference(struct open_file* description);
void fd_release(struct open_file* description);
void fd_pin(struct open_file* description);
void fd_unpin(struct open_file* description);
void fd_mark(struct open_file* description);
int fd_install(struct open_file* description, int minimum, bool close_exec);
int fd_close(uint64_t fd);
int fd_create_socket(struct unix_socket* socket, uint64_t flags);
struct unix_socket* fd_socket(uint64_t fd, uint64_t* flags);
int64_t ipc_call(struct registers* r);
int64_t ipc_socket_call(struct unix_socket* socket, uint64_t status_flags, struct registers* registers);
int64_t ipc_read(struct unix_socket* socket, uint64_t address, uint64_t count, uint64_t flags);
int64_t ipc_write(struct unix_socket* socket, uint64_t address, uint64_t count, uint64_t flags);
int64_t ipc_vectors(struct unix_socket* socket, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags);
void ipc_close(struct unix_socket* socket);
void ipc_mark(struct unix_socket* socket);
void ipc_discard(struct unix_socket* socket);
unsigned ipc_live_pairs(void);
void ipc_path_unlinked(uint64_t inode);
int unix_path_inode(const char* path, bool create, uint64_t* inode);
uint32_t ipc_ready(struct unix_socket* socket);
int64_t ipc_available(struct unix_socket* socket);

struct signal_state;
struct signal_state* signals_current(void);
struct signal_state* signals_create(struct signal_state* source, bool share_handlers);
struct signal_state* signals_exec(struct signal_state* source);
bool signals_share_pending(struct signal_state* state, struct signal_state* source);
void signals_disable_altstack(struct signal_state* state);
bool signals_masked(struct signal_state* state, unsigned number);
bool signals_has_handler(struct signal_state* state, unsigned number);
void signals_activate(struct signal_state* state);
void signals_destroy(struct signal_state* state);
void signals_selftest(void);
int signals_enqueue(struct signal_state* state, uint64_t number, int code, uint32_t sender, uint32_t uid, int status, bool group);
bool signals_interrupt_pending(void);
bool signals_reap_children(struct signal_state* state);
bool signals_should_restart(void);
int signals_wait_mask(uint64_t address, uint64_t size);
int signal_action(uint64_t number, uint64_t action, uint64_t old, uint64_t size);
int signal_mask_change(uint64_t how, uint64_t set, uint64_t old, uint64_t size);
int signal_pending_mask(uint64_t address, uint64_t size);
int signal_altstack(uint64_t address, uint64_t old, uint64_t rsp);
int signal_send(int64_t pid, uint64_t number, int code);
struct registers* signal_dispatch(struct registers* r);
struct registers* signal_fault(struct registers* r, unsigned number, int code, uint64_t address);
struct registers* signal_return(struct registers* r);

/* ---- Application platform (apps agent): generic descriptor objects, terminals, job control, inotify ---- */
/* FD_OBJECT descriptions delegate to an operations table (pty.c, inotify.c). */
struct object_operations {
    int64_t (*io)(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags);
    int (*ioctl)(void* object, uint64_t command, uint64_t argument, uint64_t flags);
    uint32_t (*ready)(void* object);
    void* (*source)(void* object);
    void (*release)(void* object);
    void (*stat)(void* object, struct linux_stat* stat);
};
int fd_create_object(const struct object_operations* operations, void* object, uint64_t flags, const char* path);
void* fd_object(struct open_file* description, const struct object_operations* operations);
struct terminal;
#define DEVICE_PTMX 0x10000U   /* /dev/ptmx */
#define DEVICE_TTY 0x10001U    /* /dev/tty: the caller's controlling terminal */
#define DEVICE_PTS 0x10100U    /* /dev/pts/N = DEVICE_PTS + N */
int pty_lookup(const char* name);
void pty_device_stat(unsigned device, struct linux_stat* stat);
int pty_directory(uint64_t position, struct directory_item* item);
int pty_open_device(unsigned device, uint64_t flags);
/* Kernel log: /dev/kmsg and syslog(2) (kmsg.c) */
void kmsg_putc(char c);
void kmsg_init(void);
void kmsg_clock_start(void);
void kmsg_poll(void);
bool kmsg_path(const char* path);
int kmsg_open(uint64_t flags);
int64_t kmsg_syslog(uint64_t type, uint64_t buffer, uint64_t length);
/* Settable/slewed CLOCK_REALTIME (timekeeping.c) */
uint64_t timekeeping_realtime(uint64_t monotonic_ns);
void timekeeping_init(uint64_t epoch_seconds);
void timekeeping_resume(uint64_t monotonic_ns, uint64_t slept_ns);   /* after system sleep (pm.c) */
void timekeeping_vdso(uint64_t* fields);
void timekeeping_poll(void);
int64_t timekeeping_call(struct registers* r);
/* Host/domain name (uts.c) */
const char* uts_hostname(void);
const char* uts_domainname(void);
int64_t uts_call(struct registers* r);
/* A/B core update slots: the booted slot's init, /proc/cmdline (bootslot.c) */
const char* boot_cmdline(void);
const char* boot_slot_init(char* out, unsigned size);
#define DEVICE_SHM 0x1000000U  /* /dev/shm/<name> = DEVICE_SHM + entry id (shmfs.c) */
bool shm_path(const char* path);
int shm_lookup(const char* name);
void shm_device_stat(unsigned device, struct linux_stat* stat);
int shm_directory(uint64_t position, struct directory_item* item);
int shm_open_path(const char* path, uint64_t flags, uint32_t mode, uint32_t umask);
int shm_unlink_path(const char* path);
int shm_rename_path(const char* from, const char* to, bool no_replace);
int shm_link_path(const char* from, const char* to);   /* link(2) within /dev/shm (sem_open) */
int shm_truncate_path(const char* path, uint64_t size);
int shm_chmod_path(const char* path, uint32_t mode);
int fd_create_memory(struct memory_file* memory, const char* path, uint64_t flags);
void terminal_session_exit(struct terminal* terminal);
struct terminal* task_terminal(void);
void task_set_terminal(struct terminal* terminal);
void task_terminal_forget(struct terminal* terminal, uint32_t session);
bool task_session_leader(void);
uint32_t task_session_id(void);
uint32_t task_group_id(void);
bool task_group_exists(uint32_t group, uint32_t session);
bool task_group_orphaned(uint32_t group);
int task_signal_group(uint32_t group, unsigned number);
int task_signal_process(uint32_t pid, unsigned number);
void task_stop(unsigned number);
void task_stop_point(void);
int64_t task_rusage(int64_t who, uint64_t address);
int64_t task_times(uint64_t address);
void signals_discard(struct signal_state* state, uint64_t mask);
bool signals_ignored(struct signal_state* state, unsigned number);
bool signals_nocldstop(struct signal_state* state);
bool signals_stop_pending(void);
uint64_t space_resident_pages(struct address_space* space);
uint64_t space_read(struct address_space* space, uint64_t address, void* output, uint64_t count);
void space_arguments(struct address_space* space, uint64_t range[4]);
uint64_t space_virtual_bytes(struct address_space* space, uint64_t* stack, uint64_t* data);
uint64_t space_locked_bytes(struct address_space* space);   /* mlocked bytes (VmLck) */
int space_map_line(struct address_space* space, uint64_t index, char* line, unsigned capacity);
struct task_info {
    uint32_t pid, tid, ppid, group, session, threads, uid[4], gid[4];
    char state, name[16]; int nice, oom_score_adj; unsigned processor;
    uint64_t cpu_ns, start_ns, faults, child_faults, child_cpu_ns, voluntary, involuntary;
    struct address_space* space; struct terminal* terminal;
};
bool task_info(uint32_t tid, bool thread, struct task_info* info);
int task_set_oom_score(uint32_t tid, int value);
void task_cpu_usage(uint64_t busy[MAX_CPUS], unsigned* running, unsigned* total);
int procext_global(const char* path);
bool procext_global_directory(unsigned index);
const char* procext_global_path(unsigned index, unsigned* length);
int procext_global_child(int parent, uint64_t position, char* name, bool* directory, unsigned* index);
int procext_task(const char* name);
const char* procext_task_name(unsigned index);
uint32_t procext_mode(bool task, unsigned index);
int64_t procext_read(bool task, unsigned index, uint32_t tid, bool thread, uint64_t address, uint64_t count, uint64_t offset);
int64_t procext_write(unsigned index, uint32_t tid, uint64_t address, uint64_t count);
/* /proc/sys/net (netproc.c): procext node ids from NETPROC_BASE */
#define NETPROC_BASE 0x01000000U
int netproc_lookup(const char* path);
bool netproc_directory(unsigned id);
bool netproc_path(unsigned id, char* out);
int netproc_child(unsigned id, uint64_t position, char* name, bool* directory, unsigned* child);
uint32_t netproc_mode(unsigned id);
int64_t netproc_read(unsigned id, uint64_t address, uint64_t count, uint64_t offset);
int64_t netproc_write(unsigned id, uint64_t address, uint64_t count);
int64_t procext_write_global(unsigned index, uint64_t address, uint64_t count); /* /proc/sys/vm (reclaim.c) */
uint32_t terminal_device(struct terminal* terminal, uint32_t* foreground);
/* inotify.c: kernel file operations report events on canonical paths. */
#define IN_ACCESS 0x1U
#define IN_MODIFY 0x2U
#define IN_ATTRIB 0x4U
#define IN_CLOSE_WRITE 0x8U
#define IN_CLOSE_NOWRITE 0x10U
#define IN_OPEN 0x20U
#define IN_MOVED_FROM 0x40U
#define IN_MOVED_TO 0x80U
#define IN_CREATE 0x100U
#define IN_DELETE 0x200U
#define IN_DELETE_SELF 0x400U
#define IN_MOVE_SELF 0x800U
extern unsigned inotify_watches;
void inotify_event(const char* path, const fat32_file_t* file, uint32_t mask, uint32_t cookie);
uint32_t inotify_cookie(void);
int64_t inotify_call(struct registers* registers);
/* fsext.c: statx, links, permissions, ownership, preallocation, in-kernel copies, file handles. */
int64_t fsext_call(struct registers* registers);
int native_path_resolve(uint64_t directory, uint64_t address, bool follow, char* path, struct open_file** bound);
int native_path_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* stat);
bool native_path_private(const char* path);
int64_t fd_transfer(struct open_file* description, uint64_t buffer, uint64_t count, uint64_t offset, bool positional, bool write);
int native_path_canonical(uint64_t directory, char* path, bool follow, struct open_file** bound);
bool native_parent_writable(const char* path);
bool native_path_writable(const char* path);
struct regular_view { struct memory_file* memory; const char* path; uint64_t* position; uint64_t flags; };
int fd_regular(struct open_file* description, struct regular_view* view); /* 1 FAT file, 2 memory file, 3 directory, 0 other */
int fd_resize(struct open_file* description, uint64_t size);
uint64_t native_scratch_map(uint64_t bytes);
void native_scratch_unmap(uint64_t address, uint64_t bytes);
int memory_file_punch(struct memory_file* file, uint64_t offset, uint64_t length);
void memory_file_unmap_page(struct memory_file* file, uint64_t index, uint64_t frame);
void memory_file_invalidate_range(struct memory_file* file, uint64_t first, uint64_t last); /* unmap pages [first, last) */
/* ---- Linux ABI completeness (kabi, wave 2): small-object slab, POSIX timers + timerfd, signalfd, pidfd/clone3 ---- */
struct kslab_page;
struct kslab { unsigned size, pages; uint64_t objects; struct kslab_page* partial; };   /* kslab.c: size-byte slots in pages */
void* kslab_alloc(struct kslab* slab);          /* zeroed object, NULL without memory */
void kslab_free(struct kslab* slab, void* object);
void* kalloc(uint64_t bytes);                    /* zeroed; size classes up to 2016 bytes, else the arena */
void kfree(void* object, uint64_t bytes);       /* the size given to kalloc */
/* signal.c additions */
int signals_enqueue_info(struct signal_state* state, const void* siginfo, bool group, void* timer);   /* 0, 1 ignored, -errno */
int signals_dequeue(uint64_t mask, void* siginfo);           /* the caller's lowest pending signal of mask (0: none) */
bool signals_pending_set(struct signal_state* state, uint64_t mask);
bool signals_unqueue_timer(struct signal_state* state, void* timer);
void signals_reset_handlers(struct signal_state* state);     /* CLONE_CLEAR_SIGHAND */
/* posix_timers.c */
bool posix_timer_unqueued(void* timer, void* siginfo);       /* signal.c: a timer's queued signal left its queue */
void posix_timers_expire(uint64_t now);                      /* process.c trap64: due timers (big lock) */
void posix_timers_exit(uint32_t pid);
void posix_timers_exec(uint32_t pid);
void posix_clock_was_set(void);                              /* timekeeping.c: CLOCK_REALTIME stepped / resume */
int64_t posix_timer_call(struct registers* r);               /* timer_create .. timer_delete, timerfd_* */
int64_t posix_itimer_cpu(uint64_t call, uint64_t which, uint64_t value, uint64_t old);   /* ITIMER_VIRTUAL/PROF */
bool posix_cpu_clock(int32_t clock, uint64_t* value);        /* clock ids < 0: CPU clocks of threads/processes */
unsigned posix_timer_objects(void);
/* signalfd.c */
void signalfd_queued(struct signal_state* state, unsigned number);   /* signal.c: a signal was queued for `state` */
int64_t signalfd_syscall(struct registers* r);               /* signalfd(4), rt_sigtimedwait, rt_sig(tg)queueinfo */
int sigpending_charge(struct signal_state* state, unsigned number, int code, uint32_t* charged);   /* 0, 1 no info, -11 */
bool sigpending_charge_current(uint32_t* charged);           /* a POSIX timer's preallocated signal */
void sigpending_uncharge(uint32_t charged);
uint32_t sigpending_count(uint32_t uid);
/* pidfd.c */
struct clone3_request { struct registers* frame; uint64_t pidfd; bool clear_sighand; };
int64_t pidfd_call(struct registers* r);                     /* pidfd_open/send_signal/getfd, clone3, waitid(P_PIDFD) */
void pidfd_process_changed(struct pid_identity* identity);   /* process.c: exited / reaped */
int pidfd_clone_install(struct pid_identity* identity, uint64_t address);   /* process.c: CLONE_PIDFD */
/* kabi.c */
int64_t kabi_call(struct registers* r);                      /* quotactl, quotactl_fd, getcpu, rseq */
uint16_t kabi_narrow(struct registers* r, uint64_t saved[6]);   /* native_dispatch: the 32-bit arguments of call r->rax */
void kabi_restore(struct registers* r, uint16_t mask, const uint64_t saved[6]);
/* timekeeping.c */
int32_t timekeeping_tai_seconds(void);
/* process.c helpers (big lock held) */
void task_timer_deadline(uint64_t deadline);                 /* the timer interrupt's expiry pass runs by then */
uint64_t task_rlimit_soft(unsigned resource);
uint32_t task_live_thread(uint32_t pid);
struct signal_state* task_signal_target(uint32_t pid, uint32_t tid);
int task_queue_info(uint32_t pid, uint32_t tid, const void* siginfo, void* cookie);
bool task_cpu_clock(uint32_t id, bool thread, uint64_t* ns);
int task_pidfd_state(uint32_t pid);                          /* 0 running, 1 exited, 2 reaped */
bool task_attach_allowed(uint32_t tid);
int64_t task_clone3(const struct clone3_request* request, struct registers* arguments);
int task_signal_info(int64_t pid, int32_t tid, const void* siginfo);
int task_waitid_found(void);
bool task_signal_owner(struct signal_state* state, uint32_t* uid, uint64_t* limit);   /* RLIMIT_SIGPENDING */
/* sysvipc.c: System V semaphores, message queues, shared memory */
int64_t sysvipc_call(struct registers* r);
void sysvipc_exit(uint32_t pid);                            /* process.c: SEM_UNDO, shm_rmid_forced */
void sysvipc_namespace_gone(uint32_t slot);                 /* namespaces.c: an IPC namespace went away */
uint32_t sysvipc_proc_mode(const char* name);               /* procext.c: /proc/sysvipc, /proc/sys/kernel IPC sysctls (0: not ours) */
int sysvipc_proc_text(const char* name, char* out, unsigned capacity);
int64_t sysvipc_sysctl_write(const char* name, const char* value);
unsigned sysvipc_objects(void);
/* landlock.c */
extern unsigned landlock_users;                             /* threads with a domain (native_dispatch fast path) */
int64_t landlock_call(struct registers* r);                 /* landlock_create_ruleset, _add_rule, _restrict_self */
int64_t landlock_check(struct registers* r);                /* native_dispatch: 0 or -EACCES/-EXDEV/... */
void landlock_fork(uint32_t parent, uint32_t child);
void landlock_exit(uint32_t tid);
bool landlock_ptrace_allowed(uint32_t tracer, uint32_t tracee);
int64_t mountapi_call(struct registers* r);                 /* mountapi.c: fsopen .. mount_setattr */
void space_exit_mappings(void);                             /* kernel.c: exit_mm (process.c terminate) */
int64_t kabi_pdeath_set(uint64_t number);                   /* kabi.c: PR_SET_PDEATHSIG / PR_GET_PDEATHSIG */
int64_t kabi_pdeath_get(uint64_t address);
unsigned kabi_pdeath_signal(uint32_t tid);
void kabi_pdeath_forget(uint32_t tid);
#endif
