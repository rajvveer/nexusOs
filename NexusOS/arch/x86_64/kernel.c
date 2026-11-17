/* Native NexusOS x86-64 bring-up. No foreign kernel or hosted runtime. */
#include "../../kernel/types.h"
#include "../../kernel/font8x8.h"
#include "../../kernel/string.h"
#include "../../kernel/ata.h"
#include "../../kernel/fat32.h"
#include "../../kernel/rtc.h"
#include "native.h"
#include "fw_session.h"
#include "boot_info.h"
#include "gpu.h"
#include "swap.h"

#define PAGE 4096ULL
#define PRESENT 1ULL
#define WRITE 2ULL
#define USER 4ULL
#define LARGE 128ULL
#define COW 512ULL
#define FILE_WRITE 1024ULL
#define MMIO 2048ULL
#define UNCACHED 24ULL
#define WRITE_COMBINED (128ULL | UNCACHED) /* 4 KiB PTE PAT|PCD|PWT selects PAT entry 7 = WC */
static uint64_t framebuffer_cache = UNCACHED; /* user /dev/fb0 mapping type: WRITE_COMBINED once PAT is set */
#define NX (1ULL << 63)
#define ADDRESS 0x000FFFFFFFFFF000ULL
#define USER_START 0x10000ULL        /* Linux mmap_min_addr: page 0 and the low 64 KiB never map */
#define USER_END 0x7FFFFFFFF000ULL   /* Linux TASK_SIZE: the lower half belongs to user space */
#define LOW_4G 0x100000000ULL
#define STACK_TOP 0x200000000ULL
#define STACK_SIZE NATIVE_STACK_SIZE

extern uint64_t boot_pml4[], boot_pdpt[], boot_pd[], gdt64[], isr_table[];
extern uint8_t __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern uint8_t kernel_stack_guard[], syscall_stack_top[], ist_stack_top[];
extern uint8_t test_elf[], test_elf_end[];
extern int user_result;
extern uint64_t syscall_entries[], cpu_kstack_top[];

static inline void out8(uint16_t p, uint8_t x) { __asm__ volatile("outb %0,%1" :: "a"(x), "Nd"(p)); }
static inline uint8_t in8(uint16_t p) { uint8_t x; __asm__ volatile("inb %1,%0" : "=a"(x) : "Nd"(p)); return x; }
static void session_select(uint16_t item) { __asm__ volatile("outw %0,%1" :: "a"(item), "Nd"((uint16_t)0x510)); }
static uint8_t session_read(void) { return in8(0x511); }
static inline void set_cr3(uint64_t x) {
    struct cpu* c = cpu_self(); c->loaded_root = x; c->loaded_space = NULL;
    __asm__ volatile("mov %0,%%cr3" :: "r"(x) : "memory");
}
static inline void wrmsr(uint32_t n, uint64_t x) { __asm__ volatile("wrmsr" :: "c"(n), "a"((uint32_t)x), "d"((uint32_t)(x >> 32)) : "memory"); }

static volatile uint32_t* framebuffer;
static struct framebuffer_info display;
static unsigned display_references;
const struct framebuffer_info* framebuffer_get(void) { return framebuffer ? &display : NULL; }
volatile uint32_t* framebuffer_pixels(void) { return framebuffer; }   /* boot-time dialogs (cryptroot.c), with framebuffer_retain */
void framebuffer_retain(void) { if (!framebuffer || display_references == ~0U) panic("framebuffer ownership"); display_references++; }
void framebuffer_release(void) { if (!display_references) panic("framebuffer reference underflow"); display_references--; }
static uint32_t screen_width, screen_height, screen_stride, col, row;
static bool screen_bgr;   /* XBGR8888 (bytes R,G,B,X): console colours swapped; /dev/fb0 reports the layout */
static uint32_t screen_pixel(uint32_t rgb) { return screen_bgr ? (rgb & 0xFF00FF00U) | (rgb >> 16 & 0xFF) | (rgb & 0xFF) << 16 : rgb; }
/* Linear framebuffer from the boot hand-off (BIOS VBE or UEFI GOP): 32-bit XRGB or XBGR (any pitch).
 * One above 4 GiB (large-BAR GOP) is attached by screen_map_high(), which also moves a low one from the
 * direct map (write-back PAT, so the firmware's UC MTRR applies) to a write-combining mapping.
 * fb.test-bgr treats an XRGB framebuffer as XBGR (exercises the RGB path on QEMU: colours come out swapped);
 * fb.test-width=N uses only the left N pixels of each line (a pitch wider than the visible width). */
static void screen_init(void) {
    const struct nexus_boot_framebuffer* fb = &boot_info.framebuffer;
    uint64_t base = fb->base, size = ((uint64_t)fb->pitch * fb->height + PAGE - 1) & ~(PAGE - 1);   /* 1366x768 etc.: not a page multiple */
    screen_stride = fb->pitch / 4; screen_width = fb->width; screen_height = fb->height;
    uint32_t format = fb->format == NEXUS_FB_XRGB8888 && boot_option("fb.test-bgr") ? NEXUS_FB_XBGR8888 : fb->format;
    uint32_t narrow = boot_option_number("fb.test-width=");   /* stride > width test: only the left part is the screen */
    if (narrow >= 640 && narrow < screen_width) screen_width = narrow;
    screen_bgr = format == NEXUS_FB_XBGR8888;
    if ((format != NEXUS_FB_XRGB8888 && format != NEXUS_FB_XBGR8888) || fb->bpp != 32 || screen_width < 640 || screen_height < 480 ||
        screen_stride < screen_width || base < 0x1000000 || base + size < base || size > 0xFFFFFFFFULL ||
        ((base | size) & (PAGE - 1)) || (fb->pitch & 3)) return;
    for (unsigned i = 0; i < boot_info.memory_count; i++)
        if (base < boot_info.memory[i].base + boot_info.memory[i].size && base + size > boot_info.memory[i].base) return;
    display = (struct framebuffer_info){base, screen_width, screen_height, screen_stride * 4, (uint32_t)size, format};
    if (base + size > LOW_4G) return;
    framebuffer = phys_to_virt(base);
    for (uint32_t y = 0; y < screen_height; y++)
        for (uint32_t x = 0; x < screen_width; x++) framebuffer[y * screen_stride + x] = screen_pixel(0x101522);
}
/* COM1 output ring (serial_lock): bytes leave in 16-byte FIFO bursts, one LSR check per burst. */
#define SERIAL_RING 16384U
static uint8_t* serial_ring;
static uint32_t serial_head, serial_tail;
static volatile int serial_lock;
static bool serial_direct; /* panic: bypass the ring */
/* A UART that never reports THR empty (unprogrammed divisor, broken Super I/O decode) is given up instead of
 * hanging the console: ~200k status reads cover a 16-byte FIFO at 9600 baud many times over. */
static bool serial_dead;
static bool serial_ready(void) {
    for (unsigned spins = 0; !serial_dead && spins < 200000; spins++) if (in8(0x3FD) & 0x20) return true;
    serial_dead = true; return false;
}
static void serial_emit(uint8_t c) { if (serial_ready()) out8(0x3F8, c); }
static void serial_flush_locked(void) {
    while (serial_tail != serial_head) {
        if (!serial_ready()) { serial_tail = serial_head; return; } /* THR and FIFO empty */
        for (unsigned n = 0; n < 16 && serial_tail != serial_head; n++, serial_tail++) out8(0x3F8, serial_ring[serial_tail % SERIAL_RING]);
    }
}
static void serial_write(const char* s, uint64_t n, bool flush) {
    if (!serial_ring || serial_direct || spin_held(&serial_lock)) { while (n--) serial_emit((uint8_t)*s++); return; }
    spin_acquire(&serial_lock);
    while (n--) {
        if (serial_head - serial_tail == SERIAL_RING) serial_flush_locked();
        serial_ring[serial_head++ % SERIAL_RING] = (uint8_t)*s++;
    }
    if (flush) serial_flush_locked();
    spin_release(&serial_lock);
}
void serial_drain(void) {
    if (!serial_ring || __atomic_load_n(&serial_head, __ATOMIC_RELAXED) == __atomic_load_n(&serial_tail, __ATOMIC_RELAXED)) return;
    if (spin_held(&serial_lock)) return;
    spin_acquire(&serial_lock); serial_flush_locked(); spin_release(&serial_lock);
}
static void serial_ring_init(void) {
    out8(0x3FA, 0x07); /* enable and clear the 16550 FIFOs */
    serial_ring = (uint8_t*)arena_alloc(SERIAL_RING);
}
static void screen_map_high(void) {
    if (!display.size) return;
    if (framebuffer) {   /* low framebuffer: write-combining window, direct-map 2 MiB pages without RAM made UC (no WB alias) */
        volatile uint32_t* wc = (volatile uint32_t*)mmio_map(display.physical, display.size, MMIO_WRITE_COMBINED);
        if (wc && wc != framebuffer) {
            for (uint64_t page = display.physical & ~0x1FFFFFULL; page < display.physical + display.size; page += 0x200000) {
                bool ram = false;
                for (unsigned i = 0; i < boot_info.memory_count; i++)
                    ram |= page < boot_info.memory[i].base + boot_info.memory[i].size && page + 0x200000 > boot_info.memory[i].base;
                if (!ram) { boot_pd[page >> 21] |= 24; __asm__ volatile("invlpg (%0)" :: "r"(phys_to_virt(page)) : "memory"); }
            }
            framebuffer = wc;
        }
        hwinfo_display_report(display.physical, display.width, display.height, display.stride, display.format, wc == framebuffer);
        return;
    }
    framebuffer = (volatile uint32_t*)mmio_map(display.physical, display.size, MMIO_WRITE_COMBINED);
    if (!framebuffer) { display.size = 0; return; }
    for (uint32_t y = 0; y < screen_height; y++)
        for (uint32_t x = 0; x < screen_width; x++) framebuffer[y * screen_stride + x] = screen_pixel(0x101522);
    hwinfo_display_report(display.physical, display.width, display.height, display.stride, display.format, true);
}
static void put_raw(char c);
static void put(char c) { if (!serial_direct) kmsg_putc(c); put_raw(c); }   /* kernel messages also go to /dev/kmsg (kmsg.c); not while panicking */
static void put_raw(char c) {
    serial_write(&c, 1, c == '\n'); diag_log_char(c);
    if (!framebuffer || display_references) return;
    if (c == '\n') { col = 0; row++; return; }
    if (c == '\r') { col = 0; return; }
    if (col * 8 + 40 >= screen_width) { col = 0; row++; }
    if (row * 18 + 40 >= screen_height) row = 0;
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 8; x++)
            framebuffer[(24 + row * 18 + y) * screen_stride + 24 + col * 8 + x] =
                screen_pixel(font8x8_data[(uint8_t)c][y / 2] & (0x80 >> x) ? 0xD8E8FF : 0x101522);
    col++;
}
static void say(const char* s) { while (*s) put(*s++); serial_drain(); }
void vga_print(const char* s) { say(s); }
void vga_print_color(const char* s, uint8_t color) { (void)color; say(s); }
static void hex(uint64_t n) { say("0x"); for (int i = 60; i >= 0; i -= 4) put("0123456789ABCDEF"[(n >> i) & 15]); }
static void finish(unsigned code) __attribute__((noreturn));
static void finish(unsigned code) {
    if (code == 0x10) { memory_file_writeback(true, 0, ~0ULL); disk_shutdown(); block_flush_all(); nvme_shutdown_all(); }
    block_report();
    serial_drain();
    platform_shutdown(code); /* isa-debug-exit under QEMU, else ACPI S5 on success (acpi.c) */
}
void panic(const char* why) { display_references = 0; serial_drain(); serial_direct = true; say("[64] FAIL: "); say(why); put('\n'); finish(0x12); }
static void check(bool ok, const char* what) { if (!ok) panic(what); say("[64] PASS "); say(what); put('\n'); }

static uint64_t tsc_frequency, boot_tsc, realtime_epoch;
static struct memory_file* vdso_file; static uint64_t vdso_bytes; /* shared vDSO image */
#define VDSO_ADDRESS (STACK_TOP + 0x200000ULL)
/* /proc/sys/kernel/randomize_va_space: 0 off, 1 (stack/mmap/vdso), 2 (+brk/data). Default 2, like a modern distro. */
int native_randomize_va_space = 2;
/* SMAP (CR4.SMAP; every user access goes through uaccess.h copies with fault fixups) is ON by default when the CPU
 * has it; the `nosmap` boot option turns it off. */
bool smap_wanted;   /* set in kernel64_main (not `nosmap`), before cpu_tables() */
int native_get_randomize(void) { return native_randomize_va_space; }
void native_set_randomize(int value) { if (value >= 0 && value <= 2) native_randomize_va_space = value; }
/* /proc/sys/kernel/kptr_restrict (0 show, 1 hide from non-CAP_SYSLOG, 2 hide from all) and dmesg_restrict (0/1):
 * root-writable (procext.c); kptr governs /proc/kallsyms addresses and %pK-style prints, dmesg governs syslog(2)/dmesg. */
int native_kptr_restrict = 1, native_dmesg_restrict = 1;
int native_get_kptr_restrict(void) { return native_kptr_restrict; }
void native_set_kptr_restrict(int value) { if (value >= 0 && value <= 2) native_kptr_restrict = value; }
int native_get_dmesg_restrict(void) { return native_dmesg_restrict; }
void native_set_dmesg_restrict(int value) { if (value == 0 || value == 1) native_dmesg_restrict = value; }
static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    if (!smp_tsc_skewed) {
        __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
        return ((uint64_t)hi << 32) | lo;
    }
    /* Unsynchronized TSCs (smp.c): add this CPU's offset; no migration between the read and the lookup. */
    uint64_t flags; __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    uint64_t value = (((uint64_t)hi << 32) | lo) + (uint64_t)cpu_self()->tsc_offset;
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
    return value;
}
uint64_t native_tsc(void) { return read_tsc(); }
static void cpuid_leaf(uint32_t leaf, uint32_t* r) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}
/* TSC frequency reported by the platform: the VMware/KVM hypervisor leaf 0x40000010 (kHz), else Intel's
 * CPUID 0x15 crystal ratio, else the 0x16 base frequency (MHz). 0 when none is available. */
static uint64_t tsc_reported_hz(bool include_estimates, const char** source) {
    uint32_t r[4]; cpuid_leaf(1, r);
    if (r[2] & (1U << 31)) { /* hypervisor present */
        uint32_t h[4]; cpuid_leaf(0x40000000U, h);
        bool known = (h[1] == 0x61774D56 && h[2] == 0x4D566572 && h[3] == 0x65726177) ||  /* "VMwareVMware" */
                     (h[1] == 0x4B4D564B && h[2] == 0x564B4D56 && h[3] == 0x0000004D);    /* "KVMKVMKVM" */
        if (known && h[0] >= 0x40000010U) {
            uint32_t f[4]; cpuid_leaf(0x40000010U, f);
            if (f[0] >= 1000 && f[0] <= 10000000) { *source = "hypervisor"; return (uint64_t)f[0] * 1000; }
        }
    }
    cpuid_leaf(0, r); uint32_t max = r[0];
    if (max >= 0x15) {
        cpuid_leaf(0x15, r);
        if (r[0] && r[1] && r[2]) { *source = "cpuid 0x15"; return (uint64_t)r[2] * r[1] / r[0]; }
    }
    if (include_estimates && max >= 0x16) {
        cpuid_leaf(0x16, r);
        if (r[0] >= 100 && r[0] <= 10000) { *source = "cpuid 0x16"; return (uint64_t)r[0] * 1000000; }
    }
    return 0;
}
static void time_init(void) {
    /* An exact platform value wins (a 10 ms PIT window was 0.16% off under VMware, and polling the PIT
     * with interrupts off stalled VMware's virtual time for ~10 s); else measure against PIT channel 2;
     * CPUID estimates are the last resort when the PIT is unusable. */
    const char* source = "PIT";
    uint64_t reported = tsc_reported_hz(false, &source);
    bool pit = !boot_option("tsc=hpet") && !boot_option("tsc=pmtimer");   /* tests: calibrate as if there were no PIT */
    if (reported) tsc_frequency = reported;
    else if (pit) {
        /* Calibrate channel 2 only; the BIOS/legacy IRQ0 PIT frequency is untouched. */
        uint8_t speaker = in8(0x61);
        out8(0x61, (speaker & ~2U) | 1U); out8(0x43, 0xB0);
        out8(0x42, 11932 & 255); out8(0x42, 11932 >> 8);
        uint64_t start = read_tsc(); unsigned spins = 20000000;
        while (!(in8(0x61) & 0x20) && --spins) __asm__ volatile("pause");
        uint64_t elapsed = read_tsc() - start;
        out8(0x61, speaker);
        if (spins && elapsed <= ~0ULL / 1193182) tsc_frequency = elapsed * 1193182 / 11932;
    }
    /* PIT gated or absent (legacy-free boards; AMD has no CPUID 0x15/0x16): HPET, then the ACPI PM timer. */
    if (!reported && (tsc_frequency < 100000000ULL || tsc_frequency > 10000000000ULL)) tsc_frequency = hw_tsc_calibrate(boot_option("tsc=pmtimer"), &source);
    if (tsc_frequency < 1000000 || tsc_frequency > 10000000000ULL) tsc_frequency = tsc_reported_hz(true, &source);
    if (tsc_frequency < 1000000 || tsc_frequency > 10000000000ULL) panic("invalid calibrated TSC frequency");
    rtc_time_t now; rtc_read(&now);
    if (now.month < 1 || now.month > 12 || now.day < 1 || now.day > 31) panic("invalid RTC calendar");
    uint64_t days = 0;
    for (unsigned year = 1970; year < now.year; year++) days += 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    const unsigned months[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (unsigned month = 1; month < now.month; month++) days += months[month - 1];
    if (now.month > 2 && now.year % 4 == 0 && (now.year % 100 != 0 || now.year % 400 == 0)) days++;
    realtime_epoch = (days + now.day - 1) * 86400 + now.hour * 3600 + now.minute * 60 + now.second;
    boot_tsc = read_tsc();
    say("[64] Calibrated TSC Hz="); hex(tsc_frequency); say(" source="); say(source); put('\n');
}
static uint64_t clock_last_ns; /* smp_tsc_unstable: largest value handed out on any CPU */
static uint64_t monotonic_ns(void) {
    uint64_t ticks = read_tsc() - boot_tsc;
    if ((int64_t)ticks < 0) ticks = 0;
    uint64_t ns = (ticks / tsc_frequency) * 1000000000ULL + (ticks % tsc_frequency) * 1000000000ULL / tsc_frequency;
    if (smp_tsc_unstable) { /* the CPUs' TSCs disagree (smp.c warp test): never go below another CPU's reading */
        uint64_t last = __atomic_load_n(&clock_last_ns, __ATOMIC_RELAXED);
        while (ns > last && !__atomic_compare_exchange_n(&clock_last_ns, &last, ns, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if (ns < last) ns = last;
    }
    return ns;
}
uint64_t native_clock_ns(void) { return monotonic_ns(); }
/* System sleep (pm.c): CLOCK_MONOTONIC stands still while the machine sleeps (the TSC may restart from zero);
 * CLOCK_REALTIME and CLOCK_BOOTTIME advance by the time asleep as the RTC measured it (Linux's persistent clock). */
static uint64_t suspended_ns, *vdso_clock;
uint64_t native_suspended_ns(void) { return suspended_ns; }
void native_vdso_clock_update(void) {
    if (!vdso_clock) return;
    vdso_clock[1] = boot_tsc; vdso_clock[2] = realtime_epoch;
    vdso_clock[3] = !smp_tsc_unstable && (!smp_tsc_skewed || smp_rdtscp); vdso_clock[4] = smp_tsc_skewed && smp_rdtscp;
    for (unsigned i = 0; i < MAX_CPUS; i++) vdso_clock[5 + i] = (uint64_t)cpus[i].tsc_offset;
    vdso_clock[5 + 32 + 4] = suspended_ns;   /* after offsets[32] and timekeeping.c's line (vdso.c NXVDSO04) */
}
void native_clock_resume(uint64_t monotonic, uint64_t slept_seconds) {
    uint64_t ticks = monotonic / 1000000000ULL * tsc_frequency + monotonic % 1000000000ULL * tsc_frequency / 1000000000ULL;
    boot_tsc = read_tsc() - ticks;   /* modulo 2^64: the clock resumes exactly where it stopped */
    realtime_epoch += slept_seconds; suspended_ns += slept_seconds * 1000000000ULL;
    timekeeping_resume(monotonic, slept_seconds * 1000000000ULL);   /* CLOCK_REALTIME line steps over the sleep */
    __atomic_store_n(&clock_last_ns, monotonic, __ATOMIC_RELAXED);
    native_vdso_clock_update();
}
void native_memory_info(uint64_t* total, uint64_t* free, uint64_t* cached) {
    uint64_t pages = memory_total_pages(), used = memory_used_pages();
    *total = pages * PAGE; *free = used < pages ? (pages - used) * PAGE : 0; *cached = memory_file_disk_pages() * PAGE;
}
uint64_t native_realtime_ns(void) { return timekeeping_realtime(monotonic_ns()); } /* settable/slewed (timekeeping.c) */
static bool clock_ns(uint32_t id, uint64_t* value) {
    if (id == 2 || id == 3) { *value = task_cpu_time(id == 3); return true; }
    if ((int32_t)id < 0) return posix_cpu_clock((int32_t)id, value);   /* clock_getcpuclockid/pthread_getcpuclockid (posix_timers.c) */
    if (id == 11) { *value = timekeeping_realtime(monotonic_ns()) + (uint64_t)timekeeping_tai_seconds() * 1000000000ULL; return true; }   /* CLOCK_TAI */
    if (id != 0 && id != 1 && id != 4 && id != 5 && id != 6 && id != 7) return false;
    *value = monotonic_ns();
    if (id == 7) *value += suspended_ns;   /* CLOCK_BOOTTIME includes time asleep */
    if (id == 0 || id == 5) *value = timekeeping_realtime(*value);
    if (id == 5 || id == 6) *value = *value / 1000000 * 1000000;
    return true;
}
static int clock_sleep(uint32_t clock, uint32_t flags, uint64_t request, uint64_t remaining) {
    if (clock == 2) return -95;
    if ((clock != 0 && clock != 1 && clock != 7) || (flags & ~1u)) return -22;
    if (!accessible(request, 16, false)) return -14;
    int64_t input[2]; copy_from_user(input, (void*)request, sizeof(input));
    if (input[0] < 0 || input[1] < 0 || input[1] >= 1000000000) return -22;
    uint64_t seconds = input[0], ns = input[1];
    uint64_t duration = seconds > (~0ULL - ns) / 1000000000 ? ~0ULL : seconds * 1000000000 + ns;
    uint64_t now = native_clock_ns();
    if (flags & 1) {
        uint64_t reference = clock == 0 ? native_realtime_ns() : clock == 7 ? now + suspended_ns : now;   /* BOOTTIME: + time asleep */
        duration = duration > reference ? duration - reference : 0;
    }
    if (!duration) return 0;
    uint64_t deadline = duration > ~0ULL - now ? ~0ULL : now + duration;
    int channel = 0;
    while (native_clock_ns() < deadline) {
        int error = task_wait_until(&channel, deadline);
        if (error == -110) return 0;
        if (!error) continue;
        if (error == -4 && !(flags & 1) && remaining) {
            uint64_t time = native_clock_ns(), left = deadline > time ? deadline - time : 0;
            if (!accessible(remaining, 16, true)) return -14;
            uput64(remaining, left / 1000000000); uput64(remaining + 8, left % 1000000000);
        }
        return error;
    }
    return 0;
}

struct vm_area;
struct address_space {
    struct address_space* next; struct vm_area* maps;
    uint64_t root, map_hint, brk, brk_floor; unsigned references; bool dumpable;
    struct vm_area* lookup_hint; /* last VMA found (cleared when that VMA is released) */
    volatile int vm_lock; /* maps, lookup_hint and the user page tables (see space_lock_task) */
    uint64_t arg_start, arg_end, env_start, env_end; /* argv/envp strings on the initial stack (/proc/pid/cmdline) */
    bool lock_future, lock_onfault; /* mlockall MCL_FUTURE [MCL_ONFAULT]: new mappings are locked (Linux def_flags) */
};
static struct address_space kernel_space, *spaces, *active_space = &kernel_space;
#define process_root (active_space->root)
static void space_flush(struct address_space* space);
static void space_flush_page(struct address_space* space, uint64_t va);
#define areas (active_space->maps)
#define mmap_base (active_space->map_hint)
#define program_break (active_space->brk)
#define break_floor (active_space->brk_floor)
static uint64_t first_pt[512] __attribute__((aligned(4096))), second_pt[512] __attribute__((aligned(4096)));
static void protect_kernel(void) {
    /* Keep the executing 4 MiB (kernel image) mapping executable until its replacement is ready. */
    for (unsigned i = 2; i < 2048; i++) boot_pd[i] |= NX;
    if (framebuffer) {
        /* Firmware mappings may have cached writes before both aliases become UC. */
        __asm__ volatile("wbinvd" ::: "memory");
        for (uint64_t i = display.physical >> 21; i <= (display.physical + display.size - 1) >> 21; i++) boot_pd[i] |= UNCACHED;
    }
    for (unsigned i = 1; i < 1024; i++) { /* physical 0-4 MiB (the kernel image): through the direct map and the kernel image map */
        uint64_t a = (uint64_t)i * PAGE, v = KERNEL_VMA + a, flags = PRESENT | WRITE | NX;
        if (v >= (uint64_t)__text_start && v < (uint64_t)__text_end) flags = PRESENT;
        if (v >= (uint64_t)__rodata_start && v < (uint64_t)__rodata_end) flags = PRESENT | NX;
        if (v == (uint64_t)kernel_stack_guard) flags = 0;
        (i < 512 ? first_pt : second_pt)[i & 511] = a | flags;
    }
    boot_pd[0] = virt_to_phys(first_pt) | PRESENT | WRITE;
    boot_pd[1] = virt_to_phys(second_pt) | PRESENT | WRITE;
    boot_pml4[0] = 0; /* the boot identity map ends: the lower half belongs to user space */
    kernel_space.root = virt_to_phys(boot_pml4);
    set_cr3(kernel_space.root);
}
/* The AP trampoline page (below 1 MiB) is executable, and the boot identity map is back in the kernel's
 * tables, only while application processors start: they enable paging at physical addresses. */
void kernel_trampoline_page(uint64_t address, bool executable) {
    first_pt[address >> 12] = address | PRESENT | WRITE | (executable ? 0 : NX);
    boot_pml4[0] = executable ? virt_to_phys(boot_pdpt) | PRESENT | WRITE : 0;
    __asm__ volatile("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
}
bool user_range(uint64_t a, uint64_t n) { return a >= USER_START && a < USER_END && n <= USER_END - a; }
static uint64_t* pte(uint64_t root, uint64_t va, bool create) {
    uint64_t* table = phys_to_virt(root);
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t* slot = &table[(va >> shift) & 511];
        if (!(*slot & PRESENT)) {
            if (!create) return NULL;
            uint64_t frame = frame_alloc(); if (!frame) return NULL;
            *slot = frame | PRESENT | WRITE | USER;
        }
        if ((*slot & LARGE) || !(*slot & USER)) return NULL;
        table = phys_to_virt(*slot & ADDRESS);
    }
    return &table[(va >> 12) & 511];
}
static bool map_page(uint64_t root, uint64_t va, uint64_t flags) {
    uint64_t* slot = pte(root, va, true);
    if (!slot || (*slot & PRESENT)) return false;
    uint64_t frame = frame_alloc(); if (!frame) return false;
    *slot = frame | flags | PRESENT | USER; return true;
}
struct vm_area {
    struct vm_area* next; uint64_t start, end, offset; unsigned prot;
    struct memory_file* memory; bool may_write, stack;
    bool file_backed, image_segment, dontdump, shared, framebuffer, device_memory; fat32_file_t file;
    bool locked;               /* mlock/mlockall/MAP_LOCKED (Linux VM_LOCKED): counts against RLIMIT_MEMLOCK */
    const uint8_t* image_memory; uint64_t image_begin, image_end;
    char path[FAT32_PATH_MAX]; /* mapped file name for /proc/pid/maps */
};
static void area_release(struct vm_area* area) {
    if (active_space->lookup_hint == area) active_space->lookup_hint = NULL;
    if (area->framebuffer) framebuffer_release();
    if (area->memory && area->image_segment && area->file_backed && area->memory->exec_maps) area->memory->exec_maps--;
    if (area->memory && area->shared && area->may_write && area->memory->writable_maps) area->memory->writable_maps--;
    if (area->memory && area->memory->maps) area->memory->maps--;
    if (area->memory) memory_file_release(area->memory);
    page_free((uint64_t)area);
    disk_orphan_collect();
}
static int sync_mappings(uint64_t start, uint64_t end) {
    for (struct vm_area* a = areas; a && a->start < end; a = a->next) {
        if (!a->shared || !a->memory || !a->memory->disk_backed || a->end <= start || a->start >= end) continue;
        uint64_t begin = a->start > start ? a->start : start, stop = a->end < end ? a->end : end;
        int error = memory_file_sync(a->memory, a->offset + begin - a->start, a->offset + stop - a->start);
        if (error) return error;
    }
    return 0;
}
static struct vm_area* space_area(struct address_space* space, uint64_t address) {
    struct vm_area* hint = space->lookup_hint;
    if (hint && address >= hint->start && address < hint->end) return hint;
    for (struct vm_area* a = space->maps; a && a->start <= address; a = a->next)
        if (address < a->end) { space->lookup_hint = a; return a; }
    return NULL;
}
static struct vm_area* area_find(uint64_t address) { return space_area(active_space, address); }
static bool data_mapping(unsigned prot, bool shared, bool stack) { return (prot & 2) && !shared && !stack; }
static bool memory_growth(uint64_t total_removed, uint64_t total_added, uint64_t data_removed, uint64_t data_added) {
    bool grows = total_added > total_removed, data_grows = data_added > data_removed;
    uint64_t span = USER_END - USER_START;
    if ((!grows || (task_resource_soft(9) & ~(PAGE - 1)) >= span) && (!data_grows || (task_resource_soft(2) & ~(PAGE - 1)) >= span))
        return true; /* nothing grows, or growth cannot pass a limit larger than the address space */
    uint64_t total = 0, data = 0;
    for (struct vm_area* a = areas; a; a = a->next) {
        uint64_t bytes = a->end - a->start; total += bytes;
        if (data_mapping(a->prot, a->shared, a->stack)) data += bytes;
    }
    if (total_removed > total || data_removed > data) panic("invalid VM resource accounting");
    if (total_added > total_removed && total - total_removed + total_added > (task_resource_soft(9) & ~(PAGE - 1))) return false;
    if (data_added > data_removed && data - data_removed + data_added > (task_resource_soft(2) & ~(PAGE - 1))) return false;
    return true;
}
int memory_futex_key(uint64_t address, bool private, struct futex_key* key) {
    if (address & 3) return -22;
    if (!accessible(address, 4, false)) return -14;
    *key = (struct futex_key){(uint64_t)active_space, address, private ? 0 : 1};
    if (private) return 0;
    struct vm_area* area = area_find(address);
    if (!private && area && area->shared && area->file_backed)
        *key = (struct futex_key){disk_inode_id(&area->file), address - area->start + area->offset, 2};
    if (!private && area && area->shared && area->memory)
        *key = (struct futex_key){area->memory->inode, address - area->start + area->offset, 3};
    return 0;
}
static bool area_free(uint64_t start, uint64_t end) {
    for (struct vm_area* a = areas; a; a = a->next) if (start < a->end && end > a->start) return false;
    return true;
}
static void area_insert(struct vm_area* area) {
    struct vm_area** link = &areas;
    while (*link && (*link)->start < area->start) link = &(*link)->next;
    area->next = *link; *link = area;
}
static bool area_add(uint64_t start, uint64_t end, unsigned prot, bool stack) {
    if (!area_free(start, end)) return false;
    if (!memory_growth(0, end - start, 0, data_mapping(prot, false, stack) ? end - start : 0)) return false;
    struct vm_area* a = (void*)page_alloc(); if (!a) return false;
    a->start = start; a->end = end; a->prot = prot; a->stack = stack; area_insert(a); return true;
}
static bool area_split(uint64_t at) {
    struct vm_area* a = area_find(at);
    if (!a || a->start == at) return true;
    struct vm_area* b = (void*)page_alloc(); if (!b) return false;
    *b = *a; b->start = at; b->offset += at - a->start;
    if (b->memory) memory_file_retain(b->memory);
    if (b->memory && b->image_segment && b->file_backed) b->memory->exec_maps++;
    if (b->memory && b->shared && b->may_write) b->memory->writable_maps++;
    if (b->memory) b->memory->maps++;
    if (b->framebuffer) framebuffer_retain();
    a->end = at; a->next = b; return true;
}
static uint64_t page_flags(unsigned prot) {
    return PRESENT | (prot ? USER : 0) | (prot & 2 ? WRITE : 0) | (prot & 4 ? 0 : NX);
}
/* Visit allocated page-table branches, so a sparse terabyte reservation stays cheap. */
static void page_change(uint64_t physical, unsigned level, uint64_t base,
                        uint64_t start, uint64_t end, int prot) {
    uint64_t* table = phys_to_virt(physical); unsigned shift = 12 + 9 * level;
    unsigned first = start > base ? (unsigned)((start - base) >> shift) : 0;
    uint64_t last = (end - 1 - base) >> shift; if (last > 511) last = 511;
    for (unsigned i = first; i <= last; i++) {
        if (!(table[i] & PRESENT)) { if (!level && prot < 0 && swap_entry_is(table[i])) { swap_free(table[i]); table[i] = 0; } continue; }
        if (!level) {
            if (prot < 0) { if (!(table[i] & MMIO)) page_free(table[i] & ADDRESS); table[i] = 0; }
            else {
                uint64_t frame = table[i] & ADDRESS, flags = page_flags((unsigned)prot);
                flags |= table[i] & (MMIO | WRITE_COMBINED);
                struct vm_area* a = area_find(base + (uint64_t)i * PAGE);
                if ((prot & 2) && a && !a->shared && page_references(frame) > 1) flags = (flags & ~WRITE) | COW;
                if ((prot & 2) && a && a->shared && a->memory && a->memory->disk_backed) flags = (flags & ~WRITE) | FILE_WRITE;
                table[i] = frame | flags;
            }
        } else {
            if (table[i] & LARGE) panic("user mapping overlaps kernel");
            uint64_t child = table[i] & ADDRESS;
            page_change(child, level - 1, base + ((uint64_t)i << shift), start, end, prot);
            if (prot < 0) {
                uint64_t* entries = phys_to_virt(child); bool empty = true;
                for (unsigned j = 0; j < 512; j++) if (entries[j]) { empty = false; break; }
                if (empty) { table[i] = 0; page_free(child); }
            }
        }
    }
}
static int area_unmap(uint64_t start, uint64_t end);
/* process.c terminate (Linux exit_mm): the last user of an address space drops its mappings before the parent hears of
 * the exit (System V shm_nattch, mapped files); the page-table root goes when the zombie is released. */
void space_exit_mappings(void) { if (active_space != &kernel_space && active_space->references == 1) area_unmap(USER_START, USER_END); }
static int area_unmap(uint64_t start, uint64_t end) {
    int error = sync_mappings(start, end); if (error) return error;
    if (!area_split(start) || !area_split(end)) return -12;
    page_change(process_root, 3, 0, start, end, -1);
    space_flush(active_space); /* before the freed frames can be reused */
    struct vm_area** link = &areas;
    while (*link) {
        struct vm_area* a = *link;
        if (a->start >= start && a->end <= end) { *link = a->next; area_release(a); }
        else link = &a->next;
    }
    return 0;
}
static bool page_file_read(fat32_file_t* file, uint32_t offset, uint32_t count, void* output) {
    int32_t result = fat32_read_file(file, offset, count, output);
    if (result == (int32_t)count) return true;
    say("[64] PAGE_IO file="); say(file->name); say(" short="); say(file->short_name);
    say(" offset="); hex(offset); say(" expected="); hex(count); say(" result="); hex((uint64_t)(int64_t)result);
    say(" size="); hex(file->size); say(" cluster="); hex(file->first); say(" dir="); hex(file->dir_lba); say(" slot="); hex(file->dir_off); put('\n');
    return false;
}
#include "vm_reclaim.inc"
static bool page_resolve(uint64_t address, bool write, bool execute) {
    if (!user_range(address, 1)) return false;
    task_set_io_free(false);   /* blockio.c: a fault may reach file, swap and reclaim code, so every sleep below (direct
                                * reclaim, the OOM wait) resumes behind a sleeping I/O owner, whatever the last syscall was */
    struct vm_area* a = area_find(address);
    if (!a || !a->prot || (write && !(a->prot & 2)) || (execute && !(a->prot & 4))) return false;
    uint64_t va = address & ~(PAGE - 1);
    uint64_t* slot = pte(process_root, va, true);
    if (!slot) { if (task_exiting()) return false; reclaim_direct(1); return true; }   /* no page-table page: reclaim, retry */
    if (block_io_busy() && (a->memory || a->file_backed || a->image_segment || swap_entry_is(*slot))) {
        block_io_barrier(); return true;   /* may read a file or swap: after the sleeping owner, with fresh state */
    }
    if (memory_pressure()) reclaim_wake();
    if (swap_entry_is(*slot)) return vm_swap_fault(active_space, slot, va);
    if (*slot & PRESENT) {
        /* Another CPU already resolved it (or this TLB entry is stale): the PTE allows the access. */
        if ((*slot & USER) && (!write || (*slot & WRITE)) && (!execute || !(*slot & NX))) {
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); return true;
        }
        if (write && (*slot & FILE_WRITE) && a->memory && a->shared) {
            memory_file_dirty(a->memory, (a->offset + va - a->start) / PAGE);
            *slot = (*slot | WRITE) & ~FILE_WRITE;
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); return true;
        }
        if (!write || !(*slot & COW)) return false;
        uint64_t frame = *slot & ADDRESS, old = 0;
        if (page_references(frame) > 1) {
            bool retry = false; uint64_t replacement = vm_user_frame(&retry); if (!replacement) return retry;
            memcpy(phys_to_virt(replacement), phys_to_virt(frame), PAGE); old = frame; frame = replacement;
        }
        *slot = frame | page_flags(a->prot);
        /* Other threads must stop reading the old frame before our reference to it goes. */
        if (old) { space_flush_page(active_space, va); page_free(old); }
        else __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
        return true;
    }
    uint64_t offset = a->offset + va - a->start;
    if (a->framebuffer) {
        if (!framebuffer || offset >= display.size) return false;
        *slot = (display.physical + offset) | page_flags(a->prot) | MMIO | framebuffer_cache; return true;
    }
    bool partial_image_page = a->image_segment && (offset < a->image_begin || offset + PAGE > a->image_end);
    if (a->memory && !partial_image_page) {
        if (offset >= a->memory->size) return false;
        uint64_t yields = task_yields();
        uint64_t backing = memory_file_page(a->memory, offset / PAGE, true);
        /* No memory for the cache page: reclaim and retry (also when another CPU freed memory meanwhile); a read error is
         * SIGBUS. Failing a valid fault under pressure killed processes (SIGSEGV) instead of waiting for reclaim. */
        if (!backing) { if (memory_file_read_failed()) return false; if (reclaim_low()) reclaim_direct(1); return !task_exiting(); }
        if (task_yields() != yields) return true;   /* it slept (swapped shared memory): the tables may have changed */
        uint64_t frame = backing, flags = page_flags(a->prot);
        if (!a->shared && write) {
            bool retry = false; frame = vm_user_frame(&retry); if (!frame) return retry;
            memcpy(phys_to_virt(frame), phys_to_virt(backing), PAGE);
        } else {
            page_retain(frame);
            if (!a->shared && (a->prot & 2)) flags = (flags & ~WRITE) | COW;
            if (a->shared && a->memory->disk_backed && (a->prot & 2)) {
                if (write) memory_file_dirty(a->memory, offset / PAGE);
                else flags = (flags & ~WRITE) | FILE_WRITE;
            }
        }
        *slot = frame | flags; __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); return true;
    }
    if (a->file_backed && !a->image_segment && offset >= a->file.size) return false;
    bool retry = false; uint64_t frame = vm_user_frame(&retry); if (!frame) return retry;
    uint8_t* data = phys_to_virt(frame);
    if (a->image_segment) {
        uint64_t begin = offset > a->image_begin ? offset : a->image_begin;
        uint64_t end = offset + PAGE < a->image_end ? offset + PAGE : a->image_end;
        if (end > begin) {
            if (a->image_memory) memcpy(data + begin - offset, a->image_memory + begin, end - begin);
            else if (a->memory) {
                uint64_t backing = memory_file_page(a->memory, offset / PAGE, true);
                if (!backing) { page_free(frame); if (memory_file_read_failed()) return false; if (reclaim_low()) reclaim_direct(1); return !task_exiting(); }
                memcpy(data + begin - offset, (uint8_t*)phys_to_virt(backing) + begin - offset, end - begin);
            }
            else if (!page_file_read(&a->file, (uint32_t)begin, (uint32_t)(end - begin), data + begin - offset)) {
                page_free(frame); return false;
            }
        }
    } else if (a->file_backed) {
        uint32_t count = a->file.size - offset < PAGE ? a->file.size - offset : PAGE;
        if (!page_file_read(&a->file, (uint32_t)offset, count, data)) {
            page_free(frame); return false;
        }
    }
    *slot = frame | page_flags(a->prot);
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); return true;
}
/* A resident page-cache page for a not-present fault (the space lock is held): mapped the way
 * page_resolve maps it. False: the locked path decides (dirty tracking, disk I/O, SIGBUS). */
static bool map_cached(struct vm_area* a, uint64_t* slot, uint64_t offset, bool write) {
    struct memory_file* file = a->memory;
    bool tracked = a->shared && file->disk_backed && (a->prot & 2); /* writes are recorded */
    if (tracked && write) return false;
    uint64_t frame = memory_file_fault_page(file, offset / PAGE, a->shared); if (!frame) return false;
    uint64_t flags = page_flags(a->prot);
    if (!a->shared && write) {
        uint64_t copy = frame_alloc_user();
        if (copy) memcpy(phys_to_virt(copy), phys_to_virt(frame), PAGE);
        page_free(frame); if (!copy) return false;
        *slot = copy | flags; return true;
    }
    if (!a->shared && (a->prot & 2)) flags = (flags & ~WRITE) | COW;
    if (tracked) flags = (flags & ~WRITE) | FILE_WRITE;
    *slot = frame | flags; return true;
}
/* User page faults without the big lock: zero-fill and copy-on-write on private memory, resident
 * page-cache pages (images, shared memory, file mappings), or a fault another CPU already resolved.
 * Only this space's lock (then the cache and allocator locks) is taken; anything else (dirty tracking,
 * disk I/O, partial image pages, the framebuffer, real access violations) returns false. */
bool space_fault_fast(struct address_space* space, uint64_t address, uint64_t error) {
    bool write = (error & 2) != 0, execute = (error & 16) != 0, handled = false;
    if (!space || space == &kernel_space || (error & 8) || !user_range(address, 1)) return false;
    spin_acquire(&space->vm_lock);
    struct vm_area* a = space_area(space, address);
    if (a && a->prot && (!write || (a->prot & 2)) && (!execute || (a->prot & 4)) && !a->framebuffer && !a->image_memory) {
        uint64_t va = address & ~(PAGE - 1), offset = a->offset + va - a->start;
        bool anonymous = !a->memory && !a->file_backed && !a->image_segment;
        bool cached = a->memory && !(a->image_segment && (offset < a->image_begin || offset + PAGE > a->image_end));
        uint64_t* slot = anonymous || cached ? pte(space->root, va, true) : NULL;
        uint64_t entry = slot ? *slot : 0;
        if (!slot) {
        } else if (swap_entry_is(entry)) {
            handled = vm_swap_fault_fast(a, slot, va);   /* still in the swap cache */
        } else if (!(entry & PRESENT)) {
            if (anonymous) {
                uint64_t frame = frame_alloc_user();
                if (frame) { *slot = frame | page_flags(a->prot); handled = true; }
            } else handled = map_cached(a, slot, offset, write);
        } else if ((entry & USER) && (!write || (entry & WRITE)) && (!execute || !(entry & NX))) {
            handled = true;
        } else if (write && (entry & COW) && !(entry & (MMIO | FILE_WRITE)) && !a->shared) {
            uint64_t frame = entry & ADDRESS;
            if (page_references(frame) == 1) { *slot = frame | page_flags(a->prot); handled = true; }
            else {
                uint64_t copy = frame_alloc_user();
                if (copy) {
                    memcpy(phys_to_virt(copy), phys_to_virt(frame), PAGE);
                    *slot = copy | page_flags(a->prot);
                    space_flush_page(space, va); page_free(frame); handled = true;
                }
            }
        }
        if (handled) __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    }
    spin_release(&space->vm_lock);
    if (memory_pressure()) reclaim_wake();   /* below a low watermark: kswapd (swap.h) */
    return handled;
}
void space_lock_task(void) {
    struct cpu* c = cpu_self();
    if (!c->task) return;
    if (c->vm_held) panic("address space lock already held");
    spin_acquire(&active_space->vm_lock); c->vm_held = active_space;
}
void space_unlock_held(void) {
    struct cpu* c = cpu_self();
    if (c->vm_held) { spin_release(&c->vm_held->vm_lock); c->vm_held = NULL; }
}
/* Another space's tables (shared file mappings): lock it unless this CPU already does. */
static bool space_lock_other(struct address_space* space) {
    if (spin_held(&space->vm_lock)) return false;
    spin_acquire(&space->vm_lock); return true;
}
static bool copy_into(uint64_t root, uint64_t va, const void* data, uint64_t n) {
    const uint8_t* source = data;
    while (n) {
        uint64_t* slot = pte(root, va, false);
        if (!slot || !(*slot & PRESENT) || (*slot & MMIO)) return false;
        uint64_t off = va & (PAGE - 1), part = PAGE - off;
        if (part > n) part = n;
        memcpy((uint8_t*)phys_to_virt(*slot & ADDRESS) + off, source, part);
        va += part; source += part; n -= part;
    }
    return true;
}
bool accessible(uint64_t a, uint64_t n, bool writable) {
    if (!user_range(a, n)) return false;
    uint64_t end = a + n;
    for (uint64_t page = a & ~(PAGE - 1); page < end; page += PAGE) {
        uint64_t need = PRESENT | USER | (writable ? WRITE : 0);
        for (unsigned attempt = 0; ; attempt++) {   /* swap-in, then COW; direct reclaim asks for a retry (each one slept) */
            uint64_t* slot = pte(process_root, page, false);
            if (slot && (*slot & need) == need) break;
            if (attempt == 4096 || task_exiting() || !page_resolve(page, writable, false)) return false;
        }
    }
    return true;
}
static uint64_t present_pages(uint64_t physical, unsigned level, uint64_t base) {
    uint64_t* table = phys_to_virt(physical), count = 0, span = 1ULL << (12 + 9 * level);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t entry = table[i], address = base + i * span;
        if (!(entry & PRESENT) || !(entry & USER) || address >= USER_END || address + span <= USER_START) continue;
        if (!level) count++; else if (!(entry & LARGE)) count += present_pages(entry & ADDRESS, level - 1, address);
    }
    return count;
}
/* Copy between a kernel buffer and an address space (ptrace PEEK/POKE, /proc/PID/mem). Pages are faulted in there
 * (zero fill, copy-on-write, file and image pages) as if that process had touched them; copying stops at the first
 * page that is not mapped with the needed access, and at device memory (a /dev/fb0 page; Linux refuses VM_IO).
 * Writes are forced like Linux FOLL_FORCE: a private mapping without PROT_WRITE (a breakpoint in code) gets its own
 * copy of the page, which stays read-only; shared read-only and PROT_NONE mappings refuse. Returns the bytes copied. */
int64_t space_access(struct address_space* space, uint64_t address, void* buffer, uint64_t count, bool write) {
    if (!space || space == &kernel_space) return 0;
    block_io_barrier();   /* faults below may read files: no other task may be inside a disk wait (blockio.c) */
    bool locked = space_lock_other(space); struct address_space* saved = active_space; uint64_t done = 0;
    while (done < count) {
        uint64_t va = address + done, page = va & ~(PAGE - 1), offset = va & (PAGE - 1), part = PAGE - offset; if (part > count - done) part = count - done;
        if (va < address || !user_range(va, part)) break;
        struct vm_area* a = write ? space_area(space, va) : NULL;
        bool force = a && a->prot && !(a->prot & 2) && !a->shared && !a->framebuffer;
        uint64_t need = PRESENT | USER | (write && !force ? WRITE : 0), *slot = pte(space->root, page, false);
        for (unsigned attempt = 0; (!slot || (*slot & need) != need) && attempt < 4096 && !task_exiting(); attempt++) {
            active_space = space; bool ok = page_resolve(page, write && !force, false); active_space = saved;   /* fault in there */
            slot = pte(space->root, page, false);
            if (!ok) break;
        }
        if (!slot || (*slot & need) != need) break;
        if (*slot & MMIO) break;   /* possibly outside the direct map (a framebuffer above 4 GiB) */
        if (force && page_references(*slot & ADDRESS) != 1) {   /* page cache or fork sharing: break it like a COW fault */
            uint64_t old = *slot & ADDRESS, copy = frame_alloc(); if (!copy) break;
            memcpy(phys_to_virt(copy), phys_to_virt(old), PAGE);
            *slot = copy | page_flags(a->prot); space_flush_page(space, page); page_free(old);
        }
        uint8_t* frame = (uint8_t*)phys_to_virt(*slot & ADDRESS) + offset;
        if (write) memcpy(frame, (uint8_t*)buffer + done, part); else memcpy((uint8_t*)buffer + done, frame, part);
        done += part;
    }
    if (locked) spin_release(&space->vm_lock);
    return (int64_t)done;
}
/* Read another address space's memory through its page tables (present RAM pages only: device memory such as a
 * /dev/fb0 page, which may lie outside the direct map, ends the copy like Linux access_remote_vm on VM_IO). */
uint64_t space_read(struct address_space* space, uint64_t address, void* output, uint64_t count) {
    uint64_t done = 0;
    while (done < count) {
        uint64_t* slot = pte(space->root, (address + done) & ~(PAGE - 1), false);
        uint64_t offset = (address + done) & (PAGE - 1), part = PAGE - offset; if (part > count - done) part = count - done;
        if (slot && swap_entry_is(*slot)) {   /* swapped out: fault it in there */
            if (space_access(space, address + done, (uint8_t*)output + done, part, false) != (int64_t)part) break;
            done += part; continue;
        }
        if (!slot || !(*slot & PRESENT) || !(*slot & USER) || (*slot & MMIO)) break;
        memcpy((uint8_t*)output + done, (uint8_t*)phys_to_virt(*slot & ADDRESS) + offset, part); done += part;
    }
    return done;
}
void space_arguments(struct address_space* space, uint64_t range[4]) {
    range[0] = space->arg_start; range[1] = space->arg_end; range[2] = space->env_start; range[3] = space->env_end;
}
uint64_t space_locked_bytes(struct address_space* space) {   /* /proc/PID/status VmLck */
    uint64_t total = 0; for (struct vm_area* a = space->maps; a; a = a->next) if (a->locked) total += a->end - a->start;
    return total;
}
uint64_t space_virtual_bytes(struct address_space* space, uint64_t* stack, uint64_t* data) {
    uint64_t total = 0; *stack = *data = 0;
    for (struct vm_area* a = space->maps; a; a = a->next) {
        total += a->end - a->start;
        if (a->stack) *stack += a->end - a->start; else if (data_mapping(a->prot, a->shared, a->stack)) *data += a->end - a->start;
    }
    return total;
}
/* One /proc/pid/maps line for the VMA at `index`; 0 past the last VMA. */
int space_map_line(struct address_space* space, uint64_t index, char* line, unsigned capacity) {
    struct vm_area* a = space->maps; while (a && index--) a = a->next;
    if (!a) return 0;
    static const char digits[] = "0123456789abcdef";
    char text[FAT32_PATH_MAX + 128]; unsigned n = 0;
    uint64_t values[3] = {a->start, a->end, a->offset};
    for (unsigned k = 0; k < 3; k++) {
        unsigned width = 8; while (width < 16 && (values[k] >> (4 * width))) width++;
        for (unsigned d = width; d--;) text[n++] = digits[(values[k] >> (4 * d)) & 15];
        text[n++] = k == 0 ? '-' : ' ';
        if (k == 1) {
            text[n++] = a->prot & 1 ? 'r' : '-'; text[n++] = a->prot & 2 ? 'w' : '-'; text[n++] = a->prot & 4 ? 'x' : '-';
            text[n++] = a->shared ? 's' : 'p'; text[n++] = ' ';
        }
    }
    uint64_t inode = a->file_backed ? disk_inode_id(&a->file) : a->memory && a->path[0] ? a->memory->inode : 0;
    const char* device = a->file_backed ? "00:01 " : a->memory && a->path[0] ? "00:0a " : "00:00 ";
    memcpy(text + n, device, 6); n += 6;
    char number[24]; number64(inode, number); memcpy(text + n, number, strlen(number)); n += strlen(number);
    const char* name = a->path[0] ? filesystem_visible_path(a->path) : a->stack ? "[stack]" : a->start == VDSO_ADDRESS ? "[vdso]" :
                       a->start >= space->brk_floor && a->end <= ((space->brk + PAGE - 1) & ~(PAGE - 1)) && a->end > space->brk_floor ? "[heap]" : "";
    if (name[0]) { while (n < 73) text[n++] = ' '; }
    unsigned size = strlen(name); if (n + size + 2 > sizeof(text)) size = sizeof(text) - n - 2;
    memcpy(text + n, name, size); n += size; text[n++] = '\n';
    if (n > capacity) return -36;
    memcpy(line, text, n); return (int)n;
}
uint64_t space_resident_pages(struct address_space* space) { return space && space != &kernel_space ? present_pages(space->root, 3, 0) : 0; }
struct address_space* space_current(void) { return active_space; }
bool space_dumpable(struct address_space* space) { return space->dumpable; }
void space_set_dumpable(struct address_space* space, bool dumpable) { space->dumpable = dumpable; }
static void cr3_load(struct address_space* space) {
    set_cr3(space->root); cpu_self()->loaded_space = space;
}
void space_activate(struct address_space* space) {
    /* Same space already loaded on this CPU (another thread, or the scheduler returning to it): keep
     * the TLB. TLB coherence across CPUs comes from shootdowns, never from reloading CR3. */
    struct address_space* next = space ? space : &kernel_space;
    active_space = next;
    if (cpu_self()->loaded_space != next || cpu_self()->loaded_root != next->root) cr3_load(next);
}
/* Scheduler: load a task's tables on this CPU without touching the kernel globals (no BKL). */
void space_load_cr3(struct address_space* space) {
    struct address_space* next = space ? space : &kernel_space; struct cpu* c = cpu_self();
    if (c->loaded_space != next || c->loaded_root != next->root) cr3_load(next);
}
/* Kernel context without a task: keep whatever this CPU has loaded (lazy CR3). */
void space_activate_lazy(void) {
    struct cpu* c = cpu_self();
    active_space = c->loaded_space ? c->loaded_space : &kernel_space;
    if (c->loaded_root != active_space->root) cr3_load(active_space);
}
/* Called (by the TLB IPI) on a CPU that still has a dying space loaded. */
void space_drop_loaded(uint64_t root) {
    struct cpu* c = cpu_self();
    if (c->loaded_root == root) cr3_load(&kernel_space);
    else __asm__ volatile("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
}
/* After PTE changes that remove or narrow access, or replace frames, in `space`: flush this CPU and
 * every other CPU that has the space loaded (they acknowledge before we continue). */
static void space_flush(struct address_space* space) {
    if (cpu_self()->loaded_root == space->root) __asm__ volatile("mov %%cr3,%%rax; mov %%rax,%%cr3" ::: "rax", "memory");
    smp_tlb_shootdown(space->root, false);
}
static void space_flush_page(struct address_space* space, uint64_t va) {
    if (cpu_self()->loaded_root == space->root) __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    smp_tlb_shootdown(space->root, false);
}
static struct address_space* space_create(void) {
    struct address_space* space = (void*)page_alloc(); if (!space) return NULL;
    space->root = frame_alloc();
    if (!space->root) { page_free((uint64_t)space); return NULL; }
    /* Lower half (user) starts empty; kernel entries 256-511 are shared (all exist before the first space). */
    memcpy((uint64_t*)phys_to_virt(space->root) + 256, boot_pml4 + 256, PAGE / 2);
    space->references = 1; space->dumpable = true; space->next = spaces; spaces = space; return space;
}
void space_retain(struct address_space* space) {
    if (!space || space == &kernel_space || !space->references || space->references == 0xFFFFFFFFU) panic("invalid address space reference");
    space->references++;
}
void space_release(struct address_space* space) {
    if (!space || space == &kernel_space || !space->references) panic("invalid address space release");
    if (--space->references) return;
    if (space == active_space) panic("releasing active address space");
    if (cpu_self()->vm_held == space) space_unlock_held(); /* execve dropped the space it ran in */
    /* No CPU may keep walking these tables (lazy CR3 after its last task left). */
    if (cpu_self()->loaded_root == space->root) cr3_load(&kernel_space);
    smp_tlb_shootdown(space->root, true);
    segments_space_release(space); /* its LDT (modify_ldt) */
    struct address_space** link = &spaces;
    while (*link && *link != space) link = &(*link)->next;
    if (!*link) panic("unregistered address space");
    *link = space->next;
    page_change(space->root, 3, 0, USER_START, USER_END, -1);
    while (space->maps) { struct vm_area* area = space->maps; space->maps = area->next; area_release(area); }
    page_free(space->root); page_free((uint64_t)space);
}
static bool clone_pages(struct address_space* source, struct address_space* child, uint64_t physical, unsigned level, uint64_t base) {
    uint64_t* entries = phys_to_virt(physical); unsigned shift = 12 + level * 9;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t address = base + ((uint64_t)i << shift), entry = entries[i];
        if (!level && swap_entry_is(entry) && address >= USER_START && address < USER_END) {   /* the child refers to the same slot */
            uint64_t* target = pte(child->root, address, true);
            if (!target) return false;
            if (!swap_dup(entry)) { say("[64] SWAP fork: entry without a slot reference "); hex(entry); put('\n'); return false; }
            *target = entry; continue;
        }
        if (!(entry & PRESENT) || address + (1ULL << shift) <= USER_START || address >= USER_END) continue;
        if (level) {
            if ((entry & LARGE) || !clone_pages(source, child, entry & ADDRESS, level - 1, address)) return false;
        } else {
            struct vm_area* a = space_area(source, address); if (!a) panic("user page without virtual area");
            uint64_t* target = pte(child->root, address, true); if (!target) return false;
            if (!a->shared && (a->prot & 2)) entry = (entry & ~WRITE) | COW;
            if (!(entry & MMIO)) page_retain(entry & ADDRESS);
            entries[i] = entry; *target = entry;
        }
    }
    return true;
}
struct address_space* space_clone(struct address_space* source) {
    if (!source || source == &kernel_space) return NULL;
    struct address_space* child = space_create(); if (!child) return NULL;
    child->map_hint = source->map_hint; child->brk = source->brk; child->brk_floor = source->brk_floor;
    child->arg_start = source->arg_start; child->arg_end = source->arg_end; child->env_start = source->env_start; child->env_end = source->env_end;
    child->dumpable = source->dumpable;
    if (!segments_space_clone(source, child)) { space_release(child); return NULL; } /* fork copies the LDT */
    struct vm_area** tail = &child->maps;
    for (struct vm_area* a = source->maps; a; a = a->next) {
        struct vm_area* copy = (void*)page_alloc();
        if (!copy) { space_release(child); return NULL; }
        *copy = *a; copy->next = NULL; copy->locked = false; *tail = copy; tail = &copy->next; /* fork drops mlock */
        if (copy->memory) memory_file_retain(copy->memory);
        if (copy->memory && copy->image_segment && copy->file_backed) copy->memory->exec_maps++;
        if (copy->memory && copy->shared && copy->may_write) copy->memory->writable_maps++;
        if (copy->memory) copy->memory->maps++;
        if (copy->framebuffer) framebuffer_retain();
    }
    bool ok = clone_pages(source, child, source->root, 3, 0);
    space_flush(source); /* the parent's writable pages just became copy-on-write */
    if (!ok) { space_release(child); return NULL; }
    return child;
}

struct idt_entry { uint16_t low, cs; uint8_t ist, flags; uint16_t mid; uint32_t high, zero; } __attribute__((packed));
struct descriptor { uint16_t limit; uint64_t base; } __attribute__((packed));
struct tss64 { uint32_t reserved0; uint64_t rsp[3], reserved1, ist[7], reserved2; uint16_t reserved3, io_base; } __attribute__((packed));
static struct idt_entry idt[256];
static struct tss64 tss[MAX_CPUS];
/* The running task's kernel stack: ring-3 interrupts (TSS RSP0) and SYSCALL (per-CPU stub). */
void cpu_task_stack(uint64_t top) { unsigned i = cpu_self()->index; tss[i].rsp[0] = cpu_kstack_top[i] = top; }
/* Per-CPU part of the descriptor tables and MSRs; the BSP runs it from cpu_tables, every AP from ap_main. */
void cpu_local_init(unsigned index, uint64_t kernel_top, uint64_t ist_top) {
    struct tss64* t = &tss[index];
    t->rsp[0] = kernel_top; t->ist[0] = ist_top; t->io_base = sizeof(*t);
    segments_cpu_init(index, (uint64_t)t, sizeof(*t) - 1); /* this CPU's GDT copy + TSS (ltr), LDT, CSTAR, SYSENTER */
    cpus[index].index = index; cpus[index].fallback_top = kernel_top; cpu_kstack_top[index] = kernel_top;
    struct descriptor d = {sizeof(idt) - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" :: "m"(d));
    wrmsr(0xC0000081, ((uint64_t)USER32_CS << 48) | ((uint64_t)KERNEL_CS << 32)); /* SYSRET 0x23 (32) / 0x33 (64), SS 0x2b */
    wrmsr(0xC0000082, syscall_entries[index]);
    wrmsr(0xC0000084, (1 << 8) | (1 << 9) | (1 << 10) | (1 << 14) | (1 << 18));
    uint32_t pat_a, pat_b, pat_c, pat_d; __asm__ volatile("cpuid" : "=a"(pat_a), "=b"(pat_b), "=c"(pat_c), "=d"(pat_d) : "a"(1), "c"(0));
    /* PAT: reset values except entry 7 = write-combining, used by the user framebuffer mapping
     * (identical on every CPU). */
    if (pat_d & (1U << 16)) { wrmsr(0x277, 0x0107040600070406ULL); if (!index) framebuffer_cache = WRITE_COMBINED; }
    if (index) { wrmsr(0xC0000100, 0); wrmsr(0xC0000101, 0); }
    if (!index) { /* RDTSCP (CPUID 0x80000001 EDX bit 27): TSC_AUX = CPU index for the vDSO clock */
        uint32_t e_a, e_b, e_c, e_d; __asm__ volatile("cpuid" : "=a"(e_a), "=b"(e_b), "=c"(e_c), "=d"(e_d) : "a"(0x80000000U), "c"(0));
        if (e_a >= 0x80000001U) {
            __asm__ volatile("cpuid" : "=a"(e_a), "=b"(e_b), "=c"(e_c), "=d"(e_d) : "a"(0x80000001U), "c"(0));
            smp_rdtscp = (e_d >> 27) & 1;
        }
    }
    if (smp_rdtscp) wrmsr(0xC0000103, index);
    security_cpu_init();   /* SMEP on this CPU (BSP and every AP) */
}
static void cpu_tables(void) {
    for (unsigned i = 0; i < 256; i++) {
        uint64_t a = isr_table[i];
        /* Ring 3 may raise int3, into (#OF from compatibility mode) and int $0x80, as on Linux. */
        idt[i] = (struct idt_entry){a, KERNEL_CS, i == 8 ? 1 : 0, i == 3 || i == 4 || i == 0x80 ? 0xEE : 0x8E, a >> 16, a >> 32, 0};
    }
    cpu_local_init(0, (uint64_t)syscall_stack_top, (uint64_t)ist_stack_top);
    /* Remap IRQs and enable only IRQ0; preserve the BIOS PIT channel 0 divisor. */
    out8(0x20, 0x11); out8(0xA0, 0x11); out8(0x21, 0x20); out8(0xA1, 0x28);
    out8(0x21, 4); out8(0xA1, 2); out8(0x21, 1); out8(0xA1, 1);
    out8(0x21, 0xFE); out8(0xA1, 0xFF);
    out8(0x43, 0x34); out8(0x40, 1193 & 0xFF); out8(0x40, 1193 >> 8);
}

static bool trace_processes;
enum descriptor_kind { FD_FILE, FD_CONSOLE, FD_SOCKET, FD_PIPE, FD_EPOLL, FD_COUNTER, FD_PROC, FD_DEVICE, FD_MEMORY, FD_INET, FD_OBJECT };
#define FD_INDEX_SLOTS 510
struct descriptor_table { unsigned references, reserved; struct open_file* slots[FD_INDEX_SLOTS]; };
_Static_assert(sizeof(struct descriptor_table) <= 4096, "descriptor table page");
struct open_file {
    struct open_file *next, *description; fat32_file_t file; uint64_t position, flags, id;
    uint32_t references, kernel_references; int fd; enum descriptor_kind kind;
    uint32_t owner_uid, owner_gid;
    unsigned device;
    struct input_reader input;
    struct memory_file* memory;
    struct descriptor_table* table;
    bool open, close_exec, reachable, scanned, releasing;   /* releasing: fd_release's teardown is running (it may sleep) */
    struct unix_socket* socket; struct pipe_end* pipe; struct event_poll* poll; struct event_counter* counter; struct proc_handle* proc;
    struct inet_socket* inet;
    const struct object_operations* ops; void* object; /* FD_OBJECT (pty.c, inotify.c) */
    struct open_file *view_next, *view_prev; /* descriptions sharing f->memory (disk caches) */
    struct open_file *sk_next, *sk_prev;     /* socket descriptions (garbage collector) */
    char path[FAT32_PATH_MAX];
};
static bool file_executing(const fat32_file_t* file);
int device_fsync(unsigned device);
static int file_sync(struct open_file* file) {
    if (file->kind == FD_MEMORY) return 0;
    if (file->kind == FD_DEVICE) return device_fsync(file->device);
    if (file->kind != FD_FILE) return -22;
    return file->memory ? memory_file_sync(file->memory, 0, ~0ULL) : 0;
}
_Static_assert(sizeof(struct open_file) <= PAGE, "descriptor allocation size");
static struct open_file* files;
static struct open_file* socket_descs;
static struct descriptor_table* active_descriptors;
static uint64_t descriptor_id;
struct filesystem_state { unsigned references; struct open_file *root, *cwd; unsigned umask; };
static struct filesystem_state initial_filesystem = {.umask = 022};
static uint32_t open_mode; /* mode argument of the open/openat/creat call in progress (shmfs creations) */
static struct filesystem_state* active_filesystem = &initial_filesystem;
struct filesystem_state* filesystem_current(void) { return active_filesystem; }
void filesystem_activate(struct filesystem_state* state) { active_filesystem = state ? state : &initial_filesystem; }
unsigned filesystem_umask(void) { return active_filesystem->umask; }
unsigned filesystem_exchange_umask(unsigned mask) { unsigned old = active_filesystem->umask; active_filesystem->umask = mask; return old; }
struct open_file* filesystem_directory(struct filesystem_state* state, bool root) { return root ? state->root : state->cwd; }
const char* filesystem_path(struct filesystem_state* state, bool root) {
    struct open_file* dir = filesystem_directory(state, root); return dir ? dir->path : "/";
}
bool filesystem_chrooted(void) { return strcmp(filesystem_path(active_filesystem, true), "/") != 0; }
const char* filesystem_visible_path(const char* path) {
    const char* root = filesystem_path(active_filesystem, true); unsigned size = strlen(root);
    if (size > 1 && !strncmp(path, root, size) && (!path[size] || path[size] == '/')) return path[size] ? path + size : "/";
    return path;
}
const char* fd_internal_path(struct open_file* f) { return f->path; }
struct filesystem_state* filesystem_clone(struct filesystem_state* source, bool share) {
    if (!source) source = &initial_filesystem;
    if (share) {
        if (!source->references || source->references == ~0u) panic("invalid shared filesystem state");
        source->references++; return source;
    }
    struct filesystem_state* state = (void*)page_alloc(); if (!state) return NULL;
    *state = *source; state->references = 1;
    if (state->root) fd_pin(state->root);
    if (state->cwd) fd_pin(state->cwd);
    return state;
}
void filesystem_release(struct filesystem_state* state) {
    if (!state || !state->references || state == &initial_filesystem) panic("unowned filesystem state");
    if (--state->references) return;
    if (state == active_filesystem) panic("releasing active filesystem state");
    if (state->root) fd_unpin(state->root);
    if (state->cwd) fd_unpin(state->cwd);
    page_free((uint64_t)state);
}
unsigned filesystem_users(struct filesystem_state* state) { return state ? state->references : 0; }   /* setns(user) */
_Static_assert(sizeof(struct linux_stat) == 144 && __builtin_offsetof(struct linux_stat, size) == 48, "x86-64 stat ABI");
static struct open_file* descriptor_in(struct descriptor_table* table, uint64_t fd) {
    /* Linux descriptor arguments are 32-bit, even in a 64-bit syscall register. */
    fd = (uint32_t)fd;
    if (table && fd < FD_INDEX_SLOTS) {
        struct open_file* f = table->slots[fd];
        return f && f->open && f->table == table && (uint64_t)f->fd == fd ? f : NULL;
    }
    for (struct open_file* f = files; f; f = f->next)
        if (f->open && f->table == table && (uint64_t)f->fd == fd) return f;
    return NULL;
}
static struct open_file* descriptor_get(uint64_t fd) { return descriptor_in(active_descriptors, fd); }
static struct open_file* file_get(uint64_t fd) {
    struct open_file* f = descriptor_get(fd); return f && f->description ? f->description : f;
}
struct open_file* fd_lookup(uint64_t fd) { return file_get(fd); }
struct open_file* fd_lookup_table(struct descriptor_table* table, uint64_t fd) {
    if (!table) return NULL;
    struct open_file* f = descriptor_in(table, fd); return f && f->description ? f->description : f;
}
int fd_next_number(struct descriptor_table* table, uint32_t minimum) {
    int result = -1;
    if (table) for (uint32_t i = minimum; i < FD_INDEX_SLOTS; i++) if (descriptor_in(table, i)) return (int)i;
    if (minimum < FD_INDEX_SLOTS) minimum = FD_INDEX_SLOTS;
    for (struct open_file* f = files; f; f = f->next)
        if (f->open && f->table == table && (uint32_t)f->fd >= minimum && (result < 0 || f->fd < result)) result = f->fd;
    return result;
}
int fd_description_path(struct open_file* f, char* result, unsigned capacity) {
    char text[FAT32_PATH_MAX + 16], value[24]; text[0] = 0;
    if (f->kind == FD_SOCKET || f->kind == FD_INET || f->kind == FD_PIPE) {
        strcpy(text, f->kind == FD_PIPE ? "pipe:[" : "socket:[");
        number64(f->kind == FD_PIPE ? pipe_identity(f->pipe) : f->id, value); strcat(text, value); strcat(text, "]");
    } else if (f->kind == FD_EPOLL) strcpy(text, "anon_inode:[eventpoll]");
    else if (f->kind == FD_COUNTER) strcpy(text, "anon_inode:[eventfd]");
    else strcpy(text, filesystem_visible_path(f->path));
    if (f->kind == FD_FILE && f->file.detached) strcat(text, " (deleted)");
    unsigned size = strlen(text); if (size >= capacity) return -36;
    memcpy(result, text, size + 1); return (int)size;
}
static struct open_file* descriptor_new(int number) {
    if (!active_descriptors) panic("descriptor allocation without table");
    struct open_file* f;
    for (f = files; f; f = f->next) if (!f->open && !f->references && !f->releasing) break;
    if (!f) {
        f = (void*)page_alloc(); if (!f) return NULL;
        f->next = files; files = f;
    }
    struct open_file* next = f->next; memset(f, 0, sizeof(*f)); f->next = next;
    f->fd = number; f->open = true; f->references = 1; f->id = ++descriptor_id; f->table = active_descriptors;
    if ((unsigned)number < FD_INDEX_SLOTS) active_descriptors->slots[number] = f;
    f->owner_uid = credentials_kernel_id(credentials_current(), false, 1); f->owner_gid = credentials_kernel_id(credentials_current(), true, 1);
    return f;
}
static int unused_descriptor_until(int minimum, uint64_t limit) {
    if (minimum < 0) return -22;
    if (limit > 0x80000000ULL) limit = 0x80000000ULL;
    for (uint64_t number = (unsigned)minimum; number < limit; number++) if (!descriptor_get(number)) return (int)number;
    return -24;
}
static int unused_descriptor(int minimum) { return unused_descriptor_until(minimum, task_resource_soft(7)); }
void fd_reference(struct open_file* f) { if (f->references == 0xFFFFFFFFU) panic("descriptor reference overflow"); f->references++; }
static bool prune_pending; /* a description may have died since the last descriptors_prune() */
void kernel_io_sleep(void* channel, uint64_t deadline);
unsigned spin_held_count(void);
/* Boot option test.fd-release-sleep (tests/test_x64_fdrace.py): every FD_FILE teardown sleeps 1 ms with the big lock
 * released where inotify_event's parent lookup can sleep in a disk wait, so other tasks run inside that window. */
static void fd_release_test_sleep(struct open_file* f) {
    static int on = -1; static int channel;
    if (on < 0) on = boot_option("test.fd-release-sleep");
    if (on && f->kind == FD_FILE && task_active() && kernel_lock_held() && spin_held_count() == (cpu_self()->vm_held ? 1u : 0u)) {   /* blockio.c may_sleep */
        if (on == 1) { on = 2; vga_print("[64] TEST fd-release-sleep active\n"); }
        kernel_io_sleep(&channel, native_clock_ns() + 1000000ULL);
    }
}
void fd_release(struct open_file* f) {
    if (!f || !f->references) panic("descriptor reference underflow");
    if (!--f->references) {
        /* The teardown can sleep with the big lock released (inotify_event's parent lookup, write-back and orphan
         * collection wait for the disk); I/O-free calls on other CPUs run meanwhile. Until it ends nobody may reuse
         * (descriptor_new) or free (descriptors_prune) this description, and its page-cache view goes first. */
        f->releasing = true;
        task_set_io_free(false); block_io_barrier();   /* also from an I/O-free call (recvmsg dropping SCM_RIGHTS): the teardown
                                                        * (orphan disposal) is filesystem work, never beside another disk wait */
        struct memory_file* memory = f->memory; f->memory = NULL;
        if (memory) {
            if (f->view_prev) f->view_prev->view_next = f->view_next;
            else if (memory->views == f) memory->views = f->view_next;
            if (f->view_next) f->view_next->view_prev = f->view_prev;
            f->view_next = f->view_prev = NULL;
        }
        file_locks_release(f);
        event_forget(f);
        if (f->kind == FD_SOCKET) {
            if (f->sk_prev) f->sk_prev->sk_next = f->sk_next; else if (socket_descs == f) socket_descs = f->sk_next;
            if (f->sk_next) f->sk_next->sk_prev = f->sk_prev;
            f->sk_next = f->sk_prev = NULL;
            ipc_close(f->socket);
        }
        if (f->kind == FD_INET) inet_close(f->inet);
        if (f->kind == FD_PIPE) pipe_close(f->pipe);
        if (f->kind == FD_EPOLL) event_destroy(f->poll);
        if (f->kind == FD_COUNTER) counter_destroy(f->counter);
        if (f->kind == FD_PROC) proc_close(f->proc);
        if (f->kind == FD_DEVICE) device_close(f->device, f->flags, &f->input);
        if (f->kind == FD_OBJECT) f->ops->release(f->object);
        if (f->kind == FD_FILE && inotify_watches) inotify_event(f->path, &f->file, (f->flags & 3) ? IN_CLOSE_WRITE : IN_CLOSE_NOWRITE, 0);
        fd_release_test_sleep(f);   /* tests/test_x64_fdrace.py: the disk wait inotify_event's lookup may take, on demand */
        if (memory) memory_file_release(memory);
        disk_orphan_collect();
        f->releasing = false; prune_pending = true;
    }
}
void fd_mark(struct open_file* f) { f->reachable = true; }
static void descriptors_collect(void) {
    /* Only sockets can hold descriptors in flight, so only sockets can form unreachable cycles. */
    if (!socket_descs) return;
    for (struct open_file* s = socket_descs; s; s = s->sk_next) s->reachable = s->scanned = false;
    for (struct open_file* f = files; f; f = f->next) {
        if (f->open) { struct open_file* d = f->description ? f->description : f; if (d->kind == FD_SOCKET) fd_mark(d); }
        if (f->kernel_references && f->kind == FD_SOCKET) fd_mark(f);
    }
    bool changed;
    do {
        changed = false;
        for (struct open_file* s = socket_descs; s; s = s->sk_next) if (s->reachable && !s->scanned) {
            s->scanned = true; changed = true; ipc_mark(s->socket);
        }
    } while (changed);
    /* Discarding can release other sockets (and unlink them), so restart after each one. */
    for (;;) {
        struct open_file* garbage = NULL;
        for (struct open_file* s = socket_descs; s; s = s->sk_next) if (s->references && !s->reachable) { garbage = s; break; }
        if (!garbage) break;
        garbage->reachable = true; ipc_discard(garbage->socket);
    }
}
static void descriptors_prune(void) {
    prune_pending = false;
    struct open_file** link = &files;
    while (*link) {
        struct open_file* f = *link;
        if (!f->open && !f->references && !f->releasing) { *link = f->next; page_free((uint64_t)f); }
        else link = &f->next;
    }
}
void fd_pin(struct open_file* f) { fd_reference(f); f->kernel_references++; }
void fd_unpin(struct open_file* f) {
    if (!f->kernel_references) panic("descriptor pin underflow");
    /* An open descriptor or another pin still roots the entire reachable graph; without descriptors
     * queued in messages nothing can be unreachable yet referenced (see collect_after). */
    bool rooted = f->open || f->kernel_references > 1;
    f->kernel_references--; fd_release(f);
    if (rooted) return;
    /* Free what died right away (exact page accounting at process exit); a plain socket call where
     * nothing died skips the walk over every description. */
    if (ipc_rights_inflight()) descriptors_collect();
    if (prune_pending || ipc_rights_inflight()) { prune_pending = false; descriptors_prune(); }
}
static int descriptor_close(struct open_file* entry) {
    if (!entry || !entry->open) return -9;
    struct open_file* d = entry->description ? entry->description : entry;
    int error = d->kind == FD_FILE && d->memory && d->memory->io_error ? file_sync(d) : 0;
    if (task_active() && entry->table == active_descriptors) file_locks_close(d, task_pid());
    if (entry->table && (unsigned)entry->fd < FD_INDEX_SLOTS && entry->table->slots[entry->fd] == entry) entry->table->slots[entry->fd] = NULL;
    entry->open = false; prune_pending = true; fd_release(d); return error;
}
static bool collect_after(struct open_file* entry) {
    /* Unreachable sockets only exist while SCM_RIGHTS descriptors are queued in messages. */
    if (!ipc_rights_inflight()) return false;
    struct open_file* d = entry && entry->description ? entry->description : entry;
    return !d || d->kind == FD_SOCKET || d->kind == FD_EPOLL;
}
static bool gc_pending;
void native_gc_poll(void) {
    static uint64_t last; uint64_t now;
    if (block_io_defer()) return;   /* closing a description may write: not while a disk wait sleeps */
    if ((!gc_pending && !prune_pending) || (now = native_clock_ns()) - last < 100000000ULL) return;
    last = now;
    if (gc_pending) { gc_pending = false; descriptors_collect(); }
    if (prune_pending) { prune_pending = false; descriptors_prune(); }
}
int fd_close(uint64_t fd) {
    struct open_file* entry = descriptor_get(fd); bool collect = collect_after(entry);
    int error = descriptor_close(entry); if (collect) descriptors_collect(); return error;
}
/* close_range(2): CLOSE_RANGE_UNSHARE (2) first gives the caller a private table; CLOSE_RANGE_CLOEXEC (4) marks instead. */
static int64_t fd_close_range(uint32_t first, uint32_t last, uint32_t flags) {
    if ((flags & ~6U) || first > last) return -22;
    if (flags & 2) { int64_t error = task_unshare(0x400); if (error) return error; }
    for (int fd = fd_next_number(active_descriptors, first); fd >= 0 && (uint32_t)fd <= last;
         fd = (uint32_t)fd >= 0x7FFFFFFFU ? -1 : fd_next_number(active_descriptors, (uint32_t)fd + 1)) {
        struct open_file* entry = descriptor_get((uint64_t)fd); if (!entry) continue;
        if (flags & 4) entry->close_exec = true; else fd_close((uint64_t)fd);
    }
    return 0;
}
int fd_install(struct open_file* description, int minimum, bool close_exec) {
    int number = unused_descriptor(minimum); if (number < 0) return number;
    struct open_file* entry = descriptor_new(number); if (!entry) return -12;
    entry->description = description; entry->references = 0; entry->close_exec = close_exec;
    fd_reference(description); return number;
}
/* seccomp USER_NOTIF ADDFD: install a copy of the supervisor's `srcfd` into the (sleeping) tracee `tid`'s
 * descriptor table, at a chosen number (setfd) or the lowest free one. Runs under the big kernel lock, so
 * borrowing active_descriptors for the target table is safe. Returns the new fd number, or -errno. */
int fd_install_into(uint32_t tid, uint32_t srcfd, uint32_t newfd, bool setfd, bool close_exec) {
    struct open_file* source = file_get(srcfd); if (!source) return -9;   /* in the supervisor's table */
    struct descriptor_table* target = task_files(tid); if (!target) return -3;
    if (setfd && newfd > 0x7FFFFFFF) return -22;
    struct descriptor_table* saved = active_descriptors; active_descriptors = target;
    int result;
    struct open_file* previous = setfd ? descriptor_in(target, newfd) : NULL;
    int number = setfd ? (int)newfd : unused_descriptor(0);
    if (number < 0) { result = number; goto done; }
    struct open_file* entry = descriptor_new(number);
    if (!entry) { result = -12; goto done; }
    entry->description = source; entry->references = 0; entry->close_exec = close_exec; fd_reference(source);
    if (previous) descriptor_close(previous);
    result = number;
done:
    active_descriptors = saved;
    return result;
}
static int fd_duplicate(uint64_t old, uint64_t target, bool close_exec, bool same_ok) {
    old = (uint32_t)old; target = (uint32_t)target;
    struct open_file* source = file_get(old); if (!source || target > 0x7FFFFFFF) return -9;
    if (old == target) return same_ok ? (int)target : -22;
    if (target >= task_resource_soft(7)) return -9;
    struct open_file* previous = descriptor_get(target);
    struct open_file* entry = descriptor_new((int)target); if (!entry) return -12;
    entry->description = source; entry->references = 0; entry->close_exec = close_exec; fd_reference(source);
    bool collect = previous && collect_after(previous);
    if (previous) descriptor_close(previous);
    if (collect) descriptors_collect();
    return (int)target;
}
int fd_create_socket(struct unix_socket* socket, uint64_t flags) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_SOCKET; f->socket = socket; f->flags = 2 | (flags & 0x800);
    f->sk_prev = NULL; f->sk_next = socket_descs; if (socket_descs) socket_descs->sk_prev = f; socket_descs = f;
    f->close_exec = (flags & 0x80000) != 0; return number;
}
int fd_create_inet(struct inet_socket* socket, uint64_t flags) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_INET; f->inet = socket; f->flags = 2 | (flags & 0x800);
    f->close_exec = (flags & 0x80000) != 0; return number;
}
struct inet_socket* fd_inet(struct open_file* f, uint64_t* flags) {
    if (!f || f->kind != FD_INET) return NULL;
    *flags = f->flags; return f->inet;
}
int fd_create_pipe(struct pipe_end* end, bool write, uint64_t flags) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_PIPE; f->pipe = end; f->flags = (write ? 1 : 0) | (flags & 0x800);
    f->close_exec = (flags & 0x80000) != 0; return number;
}
/* open(2) of /proc/PID/fd/N naming a pipe: a new description of the same pipe (pipe_reopen); O_RDWR unsupported */
static int pipe_reopen_descriptor(struct open_file* object, uint64_t flags) {
    if ((flags & 3) == 2 || (flags & 3) == 3) return -6;
    struct pipe_end* end = pipe_reopen(object->pipe, (flags & 3) == 1); if (!end) return -23;
    int fd = fd_create_pipe(end, (flags & 3) == 1, flags & (0x80000ULL | 0x800ULL));
    if (fd < 0) pipe_close(end);
    return fd;
}
int fd_create_epoll(struct event_poll* poll, bool close_exec) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_EPOLL; f->poll = poll; f->flags = 2; f->close_exec = close_exec; return number;
}
int fd_create_counter(struct event_counter* counter, uint64_t flags) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_COUNTER; f->counter = counter; f->flags = 2 | (flags & 0x800); f->close_exec = (flags & 0x80000) != 0;
    return number;
}
/* dma-buf and sync_file descriptors: device-table objects identified by reader.sequence (drm.c). */
int fd_create_device(unsigned device, uint64_t cookie, uint64_t flags, const char* path) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_DEVICE; f->device = device; f->input.sequence = cookie; f->flags = (flags & 3) | (flags & 0x800);
    f->close_exec = (flags & 0x80000) != 0; strcpy(f->path, path); return number;
}
struct input_reader* fd_device_reader(struct open_file* f, unsigned* device) {
    if (!f || f->kind != FD_DEVICE) return NULL;
    *device = f->device; return &f->input;
}
int fd_create_object(const struct object_operations* operations, void* object, uint64_t flags, const char* path) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_OBJECT; f->ops = operations; f->object = object; f->flags = flags & (3 | 0x400 | 0x800 | 0x8000 | 0x101000);
    f->close_exec = (flags & 0x80000) != 0; strcpy(f->path, path); return number;
}
void* fd_object(struct open_file* f, const struct object_operations* operations) { return f && f->kind == FD_OBJECT && f->ops == operations ? f->object : NULL; }
uint64_t fd_status_flags(struct open_file* f) { return f->flags; }
struct event_poll* fd_epoll(struct open_file* f) { return f && f->kind == FD_EPOLL ? f->poll : NULL; }
/* blockio.c syscall_io_free: descriptions whose read/write never reach a filesystem or a disk. */
bool fd_io_free(uint64_t fd) {
    struct open_file* f = fd_lookup(fd);
    return f && (f->kind == FD_CONSOLE || f->kind == FD_PIPE || f->kind == FD_SOCKET || f->kind == FD_INET || f->kind == FD_COUNTER || f->kind == FD_EPOLL);
}
bool fd_pollable(struct open_file* f) { return f->kind == FD_DEVICE ? device_pollable(f->device) : f->kind != FD_FILE && f->kind != FD_PROC && f->kind != FD_MEMORY; }
void* fd_event_source(struct open_file* f) {
    if (f->kind == FD_DEVICE) { void* source = device_event_source(f->device, &f->input); if (source) return source; }
    if (f->kind == FD_OBJECT) return f->ops->source(f->object);
    return f->kind == FD_SOCKET ? (void*)f->socket : f->kind == FD_INET ? (void*)f->inet : f->kind == FD_PIPE ? (void*)f->pipe :
           f->kind == FD_EPOLL ? (void*)f->poll : f->kind == FD_COUNTER ? (void*)f->counter : f;
}
uint32_t fd_ready(struct open_file* f) {
    uint32_t ready = f->kind == FD_PIPE ? pipe_ready(f->pipe) : f->kind == FD_SOCKET ? ipc_ready(f->socket) :
                     f->kind == FD_INET ? inet_ready(f->inet) :
                     f->kind == FD_DEVICE ? device_ready(f->device, &f->input) :
                     f->kind == FD_EPOLL ? event_ready(f->poll) : f->kind == FD_COUNTER ? counter_ready(f->counter) :
                     f->kind == FD_OBJECT ? f->ops->ready(f->object) :
                     f->kind == FD_FILE || f->kind == FD_PROC || f->kind == FD_MEMORY ? 5 : (f->flags & 3) ? 4 : 0;
    if (ready & 1) ready |= 0x40;
    if (ready & 4) ready |= 0x100;
    return ready;
}
bool iov_access(const struct iovec64* vectors, unsigned count, uint64_t skip, uint64_t length, bool write) {
    for (unsigned i = 0; i < count && length; i++) {
        if (skip >= vectors[i].size) { skip -= vectors[i].size; continue; }
        uint64_t part = vectors[i].size - skip; if (part > length) part = length;
        if (vectors[i].address > ~0ULL - skip || !accessible(vectors[i].address + skip, part, write)) return false;
        length -= part; skip = 0;
    }
    return !length;
}
void iov_copy(const struct iovec64* vectors, unsigned count, uint64_t skip, uint8_t* data, unsigned length, bool write) {
    for (unsigned i = 0; i < count && length; i++) {
        if (skip >= vectors[i].size) { skip -= vectors[i].size; continue; }
        uint64_t part = vectors[i].size - skip; if (part > length) part = length;
        void* user = (void*)(vectors[i].address + skip);
        if (write) copy_from_user(data, user, part); else copy_to_user(user, data, part);
        data += part; length -= part; skip = 0;
    }
}
struct unix_socket* fd_socket(uint64_t fd, uint64_t* flags) {
    struct open_file* f = file_get(fd);
    if (!f || f->kind != FD_SOCKET) return NULL;
    *flags = f->flags; return f->socket;
}
struct descriptor_table* fd_table_current(void) { return active_descriptors; }
void fd_table_activate(struct descriptor_table* table) { active_descriptors = table; }
void fd_table_release(struct descriptor_table* table) {
    if (!table || !table->references) panic("invalid descriptor table release");
    if (--table->references) return;
    if (table == active_descriptors) panic("releasing active descriptor table");
    for (struct open_file* f = files; f; f = f->next) if (f->open && f->table == table) descriptor_close(f);
    descriptors_collect();
    descriptors_prune();
    if (!files && ipc_live_pairs()) panic("socket endpoint ownership leak");
    page_free((uint64_t)table);
}
struct descriptor_table* fd_table_new(bool standard) {
    struct descriptor_table* table = (void*)page_alloc(); if (!table) return NULL;
    table->references = 1;
    struct descriptor_table* saved = active_descriptors; active_descriptors = table;
    if (standard) for (int i = 0; i < 3; i++) {
        struct open_file* f = descriptor_new(i);
        if (!f) { active_descriptors = saved; fd_table_release(table); return NULL; }
        f->kind = FD_CONSOLE; f->flags = i ? 1 : 0; strcpy(f->path, "/dev/console");
    }
    active_descriptors = saved; return table;
}
struct descriptor_table* fd_table_clone(struct descriptor_table* source, bool share) {
    if (!source || !source->references) return NULL;
    if (share) {
        if (source->references == 0xFFFFFFFFU) return NULL;
        source->references++; return source;
    }
    struct descriptor_table* table = fd_table_new(false); if (!table) return NULL;
    struct descriptor_table* saved = active_descriptors; active_descriptors = table;
    for (struct open_file* f = files; f; f = f->next) if (f->open && f->table == source) {
        struct open_file* copy = descriptor_new(f->fd);
        if (!copy) { active_descriptors = saved; fd_table_release(table); return NULL; }
        copy->description = f->description ? f->description : f; copy->references = 0;
        copy->close_exec = f->close_exec; fd_reference(copy->description);
    }
    active_descriptors = saved; return table;
}
void fd_close_exec(void) {
    for (struct open_file* f = files; f; f = f->next)
        if (f->table == active_descriptors && f->open && f->close_exec) descriptor_close(f);
    descriptors_collect(); descriptors_prune();
}
static int64_t descriptor_control(uint64_t fd, uint64_t command, uint64_t argument) {
    command = (uint32_t)command;
    struct open_file* entry = descriptor_get(fd); if (!entry) return -9;
    struct open_file* d = entry->description ? entry->description : entry;
    switch (command) {
    case 5: case 6: case 7: case 36: case 37: case 38: {
        fd_pin(d); int64_t result = file_lock_call(d, (uint32_t)fd, (uint32_t)command, argument, false); fd_unpin(d); return result;
    }
    case 1033: case 1034:
        if (d->kind != FD_MEMORY) return -22;
        if (command == 1034) return d->memory->seals;
        return !(d->flags & 3) ? -1 : memory_file_seal(d->memory, argument);
    case 1031: case 1032: return d->kind == FD_PIPE ? pipe_control(d->pipe, command, argument) : -9;
    case 0: case 1030:
        return argument > 0x7FFFFFFF || argument >= task_resource_soft(7) ? -22 : fd_install(d, (int)argument, command == 1030);
    case 1: return entry->close_exec;
    case 2: entry->close_exec = (argument & 1) != 0; return 0;
    case 3: return d->flags & ~(0x40ULL | 0x80ULL | 0x200ULL | 0x80000ULL);
    case 4:
        if (argument & (0x2000 | 0x4000)) return -22;
        d->flags = (d->flags & ~(0x400ULL | 0x800ULL)) | (argument & (0x400 | 0x800)); return 0;
    default: return -22;
    }
}
/* Locked memory (mlock, mlock2, mlockall, MAP_LOCKED): nothing is ever paged out, so locking marks the areas
 * (Linux VM_LOCKED; /proc/PID/status VmLck) and, unless MLOCK_ONFAULT/MCL_ONFAULT, faults the pages in (PROT_NONE
 * and device areas are skipped and never count). Without CAP_IPC_LOCK the locked total of the address space stays
 * within RLIMIT_MEMLOCK (default 8 MiB as on Linux; inherited, settable), so populating under the big lock stays
 * bounded. Errors as Linux: mlock ENOMEM over the limit or for a page that cannot be populated, EPERM when the
 * limit is 0, EAGAIN when a MAP_LOCKED/MCL_FUTURE mapping, brk or mremap growth would pass it. Fork does not
 * inherit locks or MCL_FUTURE; exec starts without them. */
static bool lock_special(const struct vm_area* a) { return a->framebuffer || a->device_memory; }
static bool memlock_permitted(void) {   /* Linux can_do_mlock */
    return (task_resource_soft(8) & ~(PAGE - 1)) || credentials_capable(credentials_current(), NULL, 14);
}
static bool memlock_room(uint64_t bytes) {   /* Linux mlock_future_ok */
    uint64_t locked = 0; for (struct vm_area* a = areas; a; a = a->next) if (a->locked) locked += a->end - a->start;
    return locked + bytes <= (task_resource_soft(8) & ~(PAGE - 1)) || credentials_capable(credentials_current(), NULL, 14);
}
/* Fault in [start, end), page by page: a fault may sleep, so the area is looked up again for every page. */
static int lock_populate(uint64_t start, uint64_t end, bool ignore_errors) {
    for (uint64_t cursor = start; cursor < end;) {
        struct vm_area* a = area_find(cursor);
        if (!a) {   /* a gap (unmapped meanwhile, or between mlockall areas): go on at the next area */
            struct vm_area* next = areas; while (next && next->end <= cursor) next = next->next;
            if (!next || next->start >= end) break;
            cursor = next->start; continue;
        }
        if (!a->prot || lock_special(a)) { cursor = a->end; continue; }
        if (!accessible(cursor, 1, false) && !ignore_errors) return -12;
        cursor += PAGE;
    }
    return 0;
}
static int64_t memory_map(uint64_t address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
    if (!length || length > USER_END - USER_START || (prot & ~7ULL) || (offset & (PAGE - 1))) return -22;
    /* JITs explicitly request executable user memory; ordinary data stays NX. */
    if ((flags & 3) != 1 && (flags & 3) != 2) return -95;
    bool shared = (flags & 3) == 1;
    if (flags & 0x40000) return -12;   /* MAP_HUGETLB: no huge pages here (Linux ENOMEM; PostgreSQL falls back) */
    /* + MAP_POPULATE / MAP_NONBLOCK: read-ahead hints, the pages fault in on use (libime's kenlm maps its models so) */
    if (flags & ~(3ULL | 0x10ULL | 0x20ULL | 0x40ULL | 0x800ULL | 0x1000ULL | 0x2000ULL | 0x4000ULL | 0x8000ULL | 0x10000ULL | 0x20000ULL | 0x100000ULL)) return -22;
    if ((flags & 0x2000) && !memlock_permitted()) return -1;   /* MAP_LOCKED with RLIMIT_MEMLOCK 0 */
    length = (length + PAGE - 1) & ~(PAGE - 1);
    bool fixed = (flags & (0x10 | 0x100000)) != 0;
    /* Linux mmap_min_addr: nothing maps below 64 KiB (EPERM); a lower hint is raised to it. MAP_32BIT
     * (without a fixed address) places the mapping in [1 GiB, 2 GiB), else above mmap_base. */
    if (fixed && !(address & (PAGE - 1)) && address < USER_START && length <= USER_END - USER_START) return -1;
    if (fixed && ((address & (PAGE - 1)) || !user_range(address, length))) return -22;
    if (!fixed) {
        uint64_t floor = flags & 0x40 ? 0x40000000ULL : mmap_base, ceiling = flags & 0x40 ? 0x80000000ULL : USER_END;
        address &= ~(PAGE - 1);
        if (address && address < USER_START) address = USER_START;
        if (!user_range(address, length) || address + length > ceiling || !area_free(address, address + length)) {
            address = floor;
            for (struct vm_area* a = areas; a; a = a->next) {
                if (a->end <= address) continue;
                if (a->start >= address && length <= a->start - address) break;
                address = a->end;
            }
        }
        if (!user_range(address, length) || address + length > ceiling) return -12;
    }
    if ((flags & 0x100000) && !area_free(address, address + length)) return -17;
    struct open_file* file = flags & 0x20 ? NULL : file_get(fd);
    if (!(flags & 0x20) && (!file || (file->flags & 0x200000))) return -9;   /* O_PATH descriptors give no access */
    bool device = file && file->kind == FD_DEVICE && device_framebuffer(file->device);
    struct memory_file* device_memory = file && file->kind == FD_DEVICE && !device ? device_mapping(file->device, &offset, length, &file->input) : NULL;
    if (!device_memory && file && file->kind == FD_OBJECT && shared) device_memory = packet_mapping(file, &offset, length);   /* AF_PACKET ring */
    if (file && file->kind != FD_FILE && file->kind != FD_MEMORY && !device && !device_memory) return -19;
    if (device && (!shared || (prot & 4) || offset >= display.size || length > display.size - offset)) return -22;
    if (file && (file->flags & 3) == 1) return -13;
    bool may_write = !shared || !file || (file->flags & 3) == 2;
    if (shared && (prot & 2) && !may_write) return -13;
    if (shared && may_write && file && file->kind == FD_FILE && (file_executing(&file->file) || swap_file_busy(file->memory))) return -26;
    if (shared && file && file->kind == FD_MEMORY && (file->memory->seals & 24)) {
        if (prot & 2) return -1;
        may_write = false;
    }
    if (file && ((file->kind == FD_FILE && file->file.is_dir) || (int64_t)offset < 0 || offset > ~0ULL - length)) return -22;
    uint64_t removed = 0, removed_data = 0;
    if (fixed) for (struct vm_area* old = areas; old; old = old->next) {
        uint64_t begin = old->start > address ? old->start : address, end = old->end < address + length ? old->end : address + length;
        if (begin >= end) continue;
        removed += end - begin;
        if (data_mapping(old->prot, old->shared, old->stack)) removed_data += end - begin;
    }
    if (!memory_growth(removed, length, removed_data, data_mapping(prot, shared, false) ? length : 0)) return -12;
    bool lock = (flags & 0x2000) || active_space->lock_future, populate = (flags & 0x2000) || !active_space->lock_onfault;
    if (lock && !memlock_room(length)) return -11;   /* Linux mlock_future_ok: EAGAIN */
    struct vm_area* a = (void*)page_alloc(); if (!a) return -12;
    if (file && (file->memory || device_memory)) { a->memory = device_memory ? device_memory : file->memory; a->device_memory = device_memory != NULL; memory_file_retain(a->memory); a->offset = offset; }
    else if (shared && !file) {
        a->memory = memory_file_create(2);
        if (!a->memory) { area_release(a); return -12; }
        a->memory->size = length; memory_file_swappable(a->memory);
    }
    if (fixed) {
        int error = area_unmap(address, address + length);
        if (error) { area_release(a); return error; }
    }
    a->start = address; a->end = address + length; a->prot = (unsigned)prot;
    a->shared = shared; a->may_write = may_write;
    if (a->memory && shared && may_write) a->memory->writable_maps++;
    if (a->memory) a->memory->maps++;
    if (device) { a->framebuffer = true; a->offset = offset; framebuffer_retain(); }
    if (file && file->kind == FD_FILE) { a->file_backed = true; a->file = file->file; a->offset = offset; }
    if (file && (file->kind == FD_FILE || file->kind == FD_MEMORY)) strcpy(a->path, file->path);
    else if (device) strcpy(a->path, "/dev/fb0");
    area_insert(a);
    if (lock && !lock_special(a)) { a->locked = true; if (populate) lock_populate(address, address + length, true); }
    return address;
}
/* sysvipc.c shmat/shmdt (Linux do_shmat, shmdt): a segment's memory mapped MAP_SHARED through a transient descriptor
 * (how: 0 anywhere, 1 fixed on free space, 2 fixed replacing: SHM_REMAP), and the areas of that memory unmapped. */
int64_t native_map_shared(struct memory_file* memory, uint64_t address, uint64_t length, unsigned prot, unsigned how, const char* path) {
    int fd = fd_create_memory(memory, path, prot & 2 ? 2 : 0); if (fd < 0) return fd;
    int64_t result = memory_map(address, length, prot, 1 | (how == 1 ? 0x100000ULL : how == 2 ? 0x10ULL : 0), (uint64_t)fd, 0);
    fd_close((uint64_t)fd); return result;
}
struct memory_file* native_area_memory(uint64_t address) {
    struct vm_area* a = area_find(address);
    return a && a->start == address && a->memory && a->shared && !a->offset && !a->device_memory ? a->memory : NULL;
}
static int area_unmap(uint64_t start, uint64_t end);
int native_unmap_memory(uint64_t address, uint64_t size, struct memory_file* memory) {
    for (;;) {
        struct vm_area* hit = NULL;
        for (struct vm_area* a = areas; a && !hit; a = a->next)
            if (a->memory == memory && a->start >= address && a->start < address + size && a->offset == a->start - address) hit = a;
        if (!hit) return 0;
        int error = area_unmap(hit->start, hit->end < address + size ? hit->end : address + size); if (error) return error;
    }
}
static int memory_protect(uint64_t address, uint64_t length, uint64_t prot) {
    if ((address & (PAGE - 1)) || (prot & ~7ULL) || length > USER_END - USER_START) return -22;
    if (!length) return 0;
    length = (length + PAGE - 1) & ~(PAGE - 1);
    if (!user_range(address, length)) return -12;
    uint64_t end = address + length, cursor = address, removed_data = 0, added_data = 0;
    while (cursor < end) {
        struct vm_area* a = area_find(cursor); if (!a) return -12;
        if (a->shared && (prot & 2) && !a->may_write) return -13;
        if (a->framebuffer && (prot & 4)) return -13;
        uint64_t next = a->end < end ? a->end : end;
        if (data_mapping(a->prot, a->shared, a->stack)) removed_data += next - cursor;
        if (data_mapping(prot, a->shared, a->stack)) added_data += next - cursor;
        cursor = next;
    }
    if (!memory_growth(0, 0, removed_data, added_data)) return -12;
    if ((prot & 2)) for (struct vm_area* a = areas; a && a->start < end; a = a->next)
        if (a->end > address && a->shared && a->file_backed && file_executing(&a->file)) return -26;
    if (!area_split(address) || !area_split(end)) return -12;
    for (struct vm_area* a = areas; a && a->start < end; a = a->next)
        if (a->start >= address) a->prot = prot;
    page_change(process_root, 3, 0, address, end, (int)prot);
    space_flush(active_space); return 0;
}
static int memory_unmap(uint64_t address, uint64_t length) {
    if (!length || length > USER_END - USER_START || (address & (PAGE - 1))) return -22;
    length = (length + PAGE - 1) & ~(PAGE - 1);
    return user_range(address, length) ? area_unmap(address, address + length) : -22;
}
/* mremap(2): shrink, grow in place, or move the VMA together with its page-table entries. */
static int64_t memory_remap(uint64_t old, uint64_t old_size, uint64_t new_size, uint64_t flags, uint64_t target) {
    bool may_move = flags & 1, fixed = (flags & 2) != 0, keep = (flags & 4) != 0;
    if ((old & (PAGE - 1)) || (flags & ~7ULL) || (fixed && !may_move) || (keep && !may_move)) return -22;
    old_size = (old_size + PAGE - 1) & ~(PAGE - 1); new_size = (new_size + PAGE - 1) & ~(PAGE - 1);
    if (!new_size || new_size > USER_END - USER_START || old_size > USER_END - USER_START) return -22;
    if (fixed && ((target & (PAGE - 1)) || !user_range(target, new_size) || (target < old + old_size && old < target + new_size))) return -22;
    if (keep && old_size != new_size) return -22;
    if (!user_range(old, old_size ? old_size : PAGE)) return -14;
    struct vm_area* a = area_find(old);
    if (!a || old + (old_size ? old_size : PAGE) > a->end) return -14;
    if (a->framebuffer) return -22;
    if (!old_size && (!a->shared || !a->memory || !may_move)) return -22;
    if (old_size && new_size <= old_size && !fixed && !keep) {
        if (new_size < old_size) { int error = area_unmap(old + new_size, old + old_size); if (error) return error; }
        return old;
    }
    uint64_t added = new_size > old_size ? new_size - old_size : 0, data = data_mapping(a->prot, a->shared, a->stack);
    bool locked = a->locked;
    if (locked && added && !memlock_room(added)) return -11;   /* Linux vma_to_resize: EAGAIN */
    if (old_size && !fixed && !keep && old + old_size == a->end && user_range(old, new_size) && area_free(a->end, old + new_size)) {
        if (!memory_growth(0, added, 0, data ? added : 0)) return -12;
        a->end = old + new_size; if (locked && added) lock_populate(old + old_size, old + new_size, true);
        return old;
    }
    if (!may_move) return -12;
    uint64_t grow = keep || !old_size ? new_size : added;
    if (!memory_growth(0, grow, 0, data ? grow : 0)) return -12;
    uint64_t destination = target;
    if (!fixed) {
        destination = mmap_base;
        for (struct vm_area* v = areas; v; v = v->next) {
            if (v->end <= destination) continue;
            if (v->start >= destination && new_size <= v->start - destination) break;
            destination = v->end;
        }
        if (!user_range(destination, new_size)) return -12;
    } else { int error = area_unmap(target, target + new_size); if (error) return error; }
    /* Page tables for every present source page first, so the move itself cannot fail. */
    for (uint64_t va = old; va < old + old_size;) {
        uint64_t* from = pte(process_root, va, false);
        if (!from) { va = (va + 0x200000) & ~0x1FFFFFULL; continue; }
        if (((*from & PRESENT) || swap_entry_is(*from)) && !pte(process_root, destination + va - old, true)) return -12;
        va += PAGE;
    }
    struct vm_area* copy = (void*)page_alloc(); if (!copy) return -12;
    if (!old_size || keep) {
        /* The source stays mapped: the copy describes it (duplicate shared pages, or an emptied range). */
        if (old_size && (!area_split(old) || !area_split(old + old_size))) { page_free((uint64_t)copy); return -12; }
        /* Linux: a duplicate (old_size 0) keeps VM_LOCKED, a MREMAP_DONTUNMAP source loses it */
        a = area_find(old); *copy = *a; copy->next = NULL; copy->locked = !old_size && a->locked;
        if (!old_size) { copy->offset += old - a->start; copy->start = destination; copy->end = destination + new_size; }
        if (copy->memory) { memory_file_retain(copy->memory); copy->memory->maps++; if (copy->shared && copy->may_write) copy->memory->writable_maps++; }
        if (copy->memory && copy->image_segment && copy->file_backed) copy->memory->exec_maps++;
        if (!old_size) {
            area_insert(copy);
            if (locked) lock_populate(destination, destination + new_size, true);
            return destination;
        }
    } else {
        page_free((uint64_t)copy); copy = NULL;
        if (!area_split(old) || !area_split(old + old_size)) return -12;
        a = area_find(old);
    }
    for (uint64_t va = old; va < old + old_size;) {
        uint64_t* from = pte(process_root, va, false);
        if (!from) { va = (va + 0x200000) & ~0x1FFFFFULL; continue; }
        if ((*from & PRESENT) || swap_entry_is(*from)) { *pte(process_root, destination + va - old, false) = *from; *from = 0; }
        va += PAGE;
    }
    space_flush(active_space);
    struct vm_area** link = &areas; while (*link != a) link = &(*link)->next;
    *link = a->next; if (active_space->lookup_hint == a) active_space->lookup_hint = NULL;
    a->start = destination; a->end = destination + new_size; area_insert(a);
    if (copy) area_insert(copy); /* MREMAP_DONTUNMAP: the old range refaults from its backing */
    if (locked && added) lock_populate(destination + old_size, destination + new_size, true);
    return destination;
}
/* A punched shared-memory page leaves every mapping that still points at its frame. */
void memory_file_unmap_page(struct memory_file* file, uint64_t index, uint64_t frame) {
    if (!file->maps) return;
    uint64_t offset = index * PAGE;
    for (struct address_space* space = spaces; space; space = space->next) for (struct vm_area* a = space->maps; a; a = a->next) {
        if (a->memory != file || offset < a->offset || offset - a->offset >= a->end - a->start) continue;
        uint64_t va = a->start + offset - a->offset, *entry = pte(space->root, va, false);
        if (entry && (*entry & PRESENT) && (*entry & ADDRESS) == frame) { *entry = 0; page_free(frame); space_flush_page(space, va); }
    }
}
bool memory_file_writable_maps(struct memory_file* file) { return file->writable_maps != 0; }
/* Hole punch (shmem.c): every mapping of the file's pages [first, last) is dropped, each space under its lock (the
 * lockless fault path edits the same tables), then shot down. */
void memory_file_invalidate_range(struct memory_file* file, uint64_t first, uint64_t last) {
    if (!file->maps || first >= last) return;
    uint64_t lo = first * PAGE, hi = last * PAGE;
    for (struct address_space* space = spaces; space; space = space->next) {
        bool locked = space_lock_other(space), changed = false;
        for (struct vm_area* a = space->maps; a; a = a->next) {
            if (a->memory != file) continue;
            uint64_t map_lo = a->offset, map_hi = a->offset + (a->end - a->start);
            uint64_t from = lo > map_lo ? lo : map_lo, to = hi < map_hi ? hi : map_hi;
            if (from >= to) continue;
            page_change(space->root, 3, 0, a->start + from - a->offset, a->start + to - a->offset, -1); changed = true;
        }
        if (changed) space_flush(space);
        if (locked) spin_release(&space->vm_lock);
    }
}
void memory_file_invalidate(struct memory_file* file, uint64_t size) {
    if (!file->maps) return;
    uint64_t aligned = (size + PAGE - 1) & ~(PAGE - 1);
    for (struct address_space* space = spaces; space; space = space->next) {
        bool locked = space_lock_other(space);
        for (struct vm_area* a = space->maps; a; a = a->next) {
            if (a->memory != file) continue;
            uint64_t begin = aligned <= a->offset ? a->start : a->start + aligned - a->offset;
            if (begin < a->end) { page_change(space->root, 3, 0, begin, a->end, -1); space_flush(space); }
            if ((size & (PAGE - 1)) && size >= a->offset && size - a->offset < a->end - a->start) {
                uint64_t va = a->start + size - a->offset; uint64_t* entry = pte(space->root, va, false);
                if (entry && (*entry & PRESENT)) memset((uint8_t*)phys_to_virt(*entry & ADDRESS) + (va & (PAGE - 1)), 0, PAGE - (va & (PAGE - 1)));
            }
        }
        if (locked) spin_release(&space->vm_lock);
    }
}
void memory_file_writeprotect(struct memory_file* file, uint64_t index) {
    if (!file->maps) return;
    uint64_t offset = index * PAGE;
    for (struct address_space* space = spaces; space; space = space->next) {
        bool locked = space_lock_other(space);
        for (struct vm_area* a = space->maps; a; a = a->next) {
            if (a->memory != file || !a->shared || !(a->prot & 2) || offset < a->offset || offset - a->offset >= a->end - a->start) continue;
            uint64_t va = a->start + offset - a->offset, *entry = pte(space->root, va, false);
            if (entry && (*entry & PRESENT)) {
                *entry = (*entry & ~(WRITE | 64ULL)) | FILE_WRITE;
                space_flush_page(space, va); /* writes on any CPU must fault again (dirty tracking) */
            }
        }
        if (locked) spin_release(&space->vm_lock);
    }
}
void memory_file_writeprotect_range(struct memory_file* file, uint64_t first, uint64_t count) {
    if (!file->maps) return;
    uint64_t lo = first * PAGE, hi = (first + count) * PAGE;
    for (struct address_space* space = spaces; space; space = space->next) {
        bool locked = space_lock_other(space);
        for (struct vm_area* a = space->maps; a; a = a->next) {
            if (a->memory != file || !a->shared || !(a->prot & 2)) continue;
            uint64_t map_lo = a->offset, map_hi = a->offset + (a->end - a->start);
            uint64_t from = lo > map_lo ? lo : map_lo, to = hi < map_hi ? hi : map_hi;
            bool changed = false;
            for (uint64_t offset = from; offset < to; offset += PAGE) {
                uint64_t va = a->start + offset - a->offset, *entry = pte(space->root, va, false);
                if (entry && (*entry & PRESENT)) { *entry = (*entry & ~(WRITE | 64ULL)) | FILE_WRITE; changed = true; }
            }
            if (changed) space_flush(space);
        }
        if (locked) spin_release(&space->vm_lock);
    }
}
/* Background write-back: short budgeted passes so interactive tasks are never held up. */
void native_writeback_poll(bool idle) {
    static uint64_t last_idle, last_busy;
    if (block_io_defer()) return;
    disk_commit_poll();
    if (!memory_file_dirty_files()) return;
    uint64_t now = native_clock_ns();
    if (idle) {
        if (now - last_idle < 20000000ULL) return;
        last_idle = now; memory_file_writeback(false, 200000000ULL, 512 * 1024);
    } else {
        if (now - last_busy < 250000000ULL) return;
        last_busy = now; memory_file_writeback(false, 3000000000ULL, 256 * 1024);
    }
}
static int64_t memory_core(uint64_t address, uint64_t length, uint64_t vector) { return vm_mincore(address, length, vector); }
static int memory_sync(uint64_t address, uint64_t length, uint64_t flags) {
    if ((address & (PAGE - 1)) || (flags & ~7ULL) || (flags & 5) == 5 || length > USER_END - USER_START) return -22;
    if (!length) return 0;
    length = (length + PAGE - 1) & ~(PAGE - 1);
    if (!user_range(address, length)) return -12;
    uint64_t end = address + length;
    for (uint64_t cursor = address; cursor < end;) {
        struct vm_area* a = area_find(cursor); if (!a) return -12;
        cursor = a->end < end ? a->end : end;
    }
    int error = sync_mappings(address, end);
    if (!error && (flags & 4) && disk_sync() < 0) error = -5; /* MS_SYNC: ext4 commits the journal (block allocations) */
    return error;
}
/* mlock, munlock, mlockall, munlockall, mlock2 (Wine's NtLockVirtualMemory): see memlock_room. */
static int memory_lock(uint64_t call, uint64_t address, uint64_t length, uint64_t flags) {
    if (call == 151) {   /* mlockall: MCL_CURRENT 1, MCL_FUTURE 2, MCL_ONFAULT 4 */
        if ((address & ~7ULL) || !(address & 3)) return -22;
        if (!memlock_permitted()) return -1;
        if (address & 1) {   /* Linux: the whole mapped size must fit the limit */
            uint64_t total = 0; for (struct vm_area* a = areas; a; a = a->next) total += a->end - a->start;
            if (total > (task_resource_soft(8) & ~(PAGE - 1)) && !credentials_capable(credentials_current(), NULL, 14)) return -12;
        }
        active_space->lock_future = (address & 2) != 0; active_space->lock_onfault = (address & 6) == 6;
        if (!(address & 1)) return 0;
        for (struct vm_area* a = areas; a; a = a->next) a->locked = !lock_special(a);
        if (!(address & 4)) lock_populate(USER_START, USER_END, true);
        return 0;
    }
    if (call == 152) {   /* munlockall */
        for (struct vm_area* a = areas; a; a = a->next) a->locked = false;
        active_space->lock_future = active_space->lock_onfault = false; return 0;
    }
    if (call == 325 && (flags & ~1ULL)) return -22;
    bool lock = call != 150;
    if (lock && !memlock_permitted()) return -1;
    if (!length) return 0;
    uint64_t end = address + length;
    if (end < address) return -12;
    address &= ~(PAGE - 1); end = (end + PAGE - 1) & ~(PAGE - 1);
    if (!user_range(address, end - address)) return -12;
    for (uint64_t cursor = address; cursor < end;) {
        struct vm_area* a = area_find(cursor); if (!a) return -12;
        cursor = a->end < end ? a->end : end;
    }
    if (lock) {   /* only what is not locked yet counts (Linux count_mm_mlocked_page_nr) */
        uint64_t adding = 0;
        for (struct vm_area* a = areas; a && a->start < end; a = a->next) {
            if (a->end <= address || a->locked || lock_special(a)) continue;
            adding += (a->end < end ? a->end : end) - (a->start > address ? a->start : address);
        }
        if (adding && !memlock_room(adding)) return -12;
    }
    if (!area_split(address) || !area_split(end)) return -12;
    for (struct vm_area* a = areas; a && a->start < end; a = a->next) if (a->start >= address) a->locked = lock && !lock_special(a);
    return lock && !(call == 325 && flags) ? lock_populate(address, end, false) : 0;   /* MLOCK_ONFAULT: no populate */
}
static int memory_advise(uint64_t address, uint64_t length, uint64_t advice) {
    if ((address & (PAGE - 1)) || length > USER_END - USER_START) return -22;
    if (advice > 4 && advice != 8 && advice != 9 && advice != 16 && advice != 17) return -22;
    if (!length) return 0;
    length = (length + PAGE - 1) & ~(PAGE - 1);
    if (!user_range(address, length)) return -12;
    uint64_t end = address + length, cursor = address;
    while (cursor < end) {
        struct vm_area* a = area_find(cursor); if (!a) return -12;
        if (advice == 8 && (a->file_backed || a->image_segment || a->memory)) return -22;
        if (advice == 9 && (!a->memory || a->image_segment)) return -22;
        if (advice == 9 && (!a->shared || !a->may_write)) return -13;
        if (advice == 9 && a->device_memory) return -19; /* sound/DRM buffers: no fallocate on a device (pages are host/kernel-owned) */
        if (advice == 9 && a->memory->disk_backed) return -95; /* vfat cannot punch holes */
        if (a->framebuffer && (advice == 4 || advice == 8)) return -22;
        cursor = a->end < end ? a->end : end;
    }
    if (advice == 9) for (cursor = address; cursor < end;) {
        struct vm_area* a = area_find(cursor); uint64_t stop = a->end < end ? a->end : end;
        int error = memory_file_punch(a->memory, a->offset + cursor - a->start, stop - cursor); if (error) return error;
        cursor = stop;
    }
    if (advice == 4 || advice == 8) {
        int error = sync_mappings(address, end); if (error) return error;
        page_change(process_root, 3, 0, address, end, -1); space_flush(active_space);
    } else if (advice == 16 || advice == 17) {
        if (!area_split(address) || !area_split(end)) return -12;
        for (struct vm_area* a = areas; a && a->start < end; a = a->next)
            if (a->start >= address) a->dontdump = advice == 16;
    }
    return 0;
}
static uint64_t memory_break(uint64_t value) {
    if (!value || value < break_floor || !user_range(value, PAGE)) return program_break;
    uint64_t old_end = (program_break + PAGE - 1) & ~(PAGE - 1), new_end = (value + PAGE - 1) & ~(PAGE - 1);
    if (new_end > old_end && active_space->lock_future && !memlock_room(new_end - old_end)) return program_break;
    if (new_end > old_end && !area_add(old_end, new_end, 3, false)) return program_break;
    if (new_end > old_end && active_space->lock_future) {   /* Linux brk: VM_LOCKED from def_flags, populated */
        struct vm_area* grown = area_find(old_end); if (grown) grown->locked = true;
        if (!active_space->lock_onfault) lock_populate(old_end, new_end, true);
    }
    if (new_end < old_end && area_unmap(new_end, old_end)) return program_break;
    program_break = value; return value;
}
bool native_random_available(void) {
    uint32_t a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (c & (1U << 30)) != 0;
}
bool native_random(void* output, uint64_t length) {
    if (!native_random_available()) return false;
    uint8_t* p = output;
    while (length) {
        uint64_t value = 0; uint8_t ok = 0;
        for (unsigned retry = 0; retry < 10 && !ok; retry++) __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) :: "cc");
        if (!ok) return false;
        uint64_t count = length < sizeof(value) ? length : sizeof(value);
        memcpy(p, &value, count); p += count; length -= count;
    }
    return true;
}
static int64_t console_write(uint64_t address, uint64_t length) {
    if (!length) return 0;
    if (!accessible(address, length, false)) return -14;
    for (uint64_t done = 0; done < length;) {   /* through a kernel buffer (SMAP: no direct user reads) */
        char chunk[256]; uint64_t part = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
        if (copy_from_user(chunk, (const void*)(address + done), part)) return done ? (int64_t)done : -14;
        if (framebuffer && !display_references) { for (uint64_t i = 0; i < part; i++) put_raw(chunk[i]); }
        else serial_write(chunk, part, false); /* sent after the big lock is dropped */
        done += part;
    }
    if (framebuffer && !display_references) serial_drain();
    return length;
}
/* mount.c: mounted filesystems beside the root (policy below follows the filesystem a path or handle is on) */
bool mount_path(const char* path); bool disk_file_readonly(const fat32_file_t* file); uint64_t disk_file_max_size(const fat32_file_t* file); bool mount_write_through(const fat32_file_t* file);
bool mount_statfs_dev(uint64_t dev, uint64_t values[12]); bool mount_syscall_targets_mount(struct registers* r); int64_t mount_call(struct registers* r); void mount_fstab(void);
static int user_path(uint64_t address, char* path) {
    int64_t length = strncpy_from_user(path, address, FAT32_PATH_MAX);   /* validated page by page (uaccess.c) */
    return length < 0 ? (int)length : 0;
}
static int memory_file_descriptor(struct memory_file* memory, const char* path, uint64_t flags) {
    int number = unused_descriptor(0); if (number < 0) return number;
    struct open_file* f = descriptor_new(number); if (!f) return -12;
    f->kind = FD_MEMORY; f->memory = memory; memory_file_retain(memory);
    f->flags = flags | 0x8000; f->close_exec = (flags & 0x80000) != 0; strcpy(f->path, path); return number;
}
int fd_create_memory(struct memory_file* memory, const char* path, uint64_t flags) { return memory_file_descriptor(memory, path, flags); }
static int memory_create(uint64_t address, uint64_t flags) {
    if ((flags & ~0x1BULL) || (flags & 0x18) == 0x18) return -22;
    char path[280] = "/memfd:"; unsigned length = 0;
    for (;;) {
        if (!accessible(address + length, 1, false)) return -14;
        char c = (char)uget8(address + length); if (!c) break;
        if (length == 249) return -22;
        path[7 + length++] = c;
    }
    path[7 + length] = 0; strcat(path, " (deleted)");
    struct memory_file* memory = memory_file_create((unsigned)flags); if (!memory) return -12;
    memory_file_swappable(memory);
    int result = memory_file_descriptor(memory, path, 2 | (flags & 1 ? 0x80000 : 0));
    memory_file_release(memory); return result;
}
static bool user_readable(const char* path);
int device_readlink(unsigned device, char* target, unsigned capacity);
static int link_contents(fat32_file_t* file, char* target) {
    if (file->native_kind != FAT32_NATIVE_SYMLINK) return -22;
    if (!file->size || file->size >= FAT32_PATH_MAX) return -5;
    if (fat32_read_file(file, 0, file->size, target) != (int32_t)file->size) return -5;
    for (unsigned i = 0; i < file->size; i++) if (!target[i]) return -5;
    target[file->size] = 0; return (int)file->size;
}
static int resolved_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* stat) {
    if (bound) return fd_description_stat(bound, stat);
    int device = device_lookup(path);
    if (device >= 0) { device_stat((unsigned)device, stat); return 0; }
    return proc_path(path) ? proc_path_stat(path, follow, stat) : disk_path_stat(path, stat);
}
static int resolve_path(char* path, bool follow_final, struct open_file* base, struct open_file** bound) {
    if (!path[0]) return -2;
    if (bound) *bound = NULL;
    struct open_file* anchor = path[0] == '/' ? active_filesystem->root : base ? base : active_filesystem->cwd;
    char pending[FAT32_PATH_MAX], out[FAT32_PATH_MAX]; strcpy(pending, path); strcpy(out, anchor ? anchor->path : "/");
    unsigned count = strlen(out), links = 0; const char* p = pending;
    while (*p) {
        while (*p == '/') p++;
        const char* part = p;
        while (*p && *p != '/') { if (*p == '\\') return -22; p++; }
        unsigned size = p - part;
        if (!size || (size == 1 && part[0] == '.')) continue;
        /* shortcut: detached-directory traversal needs retained parent dentries. */
        if (anchor && anchor->kind == FD_FILE && anchor->file.detached) return -2;
        if ((disk_posix() || mount_path(out)) && (!anchor || anchor->kind == FD_FILE)) { int denied = disk_may_search(out); if (denied) return denied; }
        if (size == 2 && part[0] == '.' && part[1] == '.') {
            if (!strcmp(out, filesystem_path(active_filesystem, true))) continue;
            while (count > 1 && out[count - 1] != '/') count--;
            if (count > 1) count--;
            out[count] = 0; anchor = NULL;
            if (!strcmp(out, filesystem_path(active_filesystem, true))) anchor = active_filesystem->root;
            struct linux_stat stat; int error = resolved_stat(out, anchor, true, &stat); if (error) return error;
            if (!user_readable(out)) return -13;
            continue;
        }
        unsigned parent = count; struct open_file* parent_anchor = anchor; anchor = NULL;
        if (count + (count > 1) + size >= sizeof(out)) return -36;
        if (count > 1) out[count++] = '/';
        memcpy(out + count, part, size); count += size; out[count] = 0;
        if (!user_readable(out)) return -13;
        const char* remaining = p; while (*remaining == '/') remaining++;
        bool more = *remaining != 0, trailing = *p == '/' && !more, link = false, directory = false;
        bool virtual = proc_path(out); int device = device_lookup(out), error = 0;
        struct linux_stat stat; fat32_file_t file;
        if (virtual) { error = proc_path_stat(out, false, &stat); link = !error && (stat.mode & 0170000) == 0120000; directory = !error && (stat.mode & 0170000) == 0040000; }
        else if (device >= 0) { device_stat((unsigned)device, &stat); directory = (stat.mode & 0170000) == 0040000; link = (stat.mode & 0170000) == 0120000; }
        else {
            if (!fat32_lookup(out, &file)) error = -2;
            else {
                if (file.native_kind > (disk_posix() || file.mount ? DISK_KIND_BLK : FAT32_NATIVE_SYMLINK)) return -5;
                link = file.native_kind == FAT32_NATIVE_SYMLINK; directory = file.is_dir;
                unsigned name_size = strlen(file.name), start = parent + (parent > 1);
                if (start + name_size >= sizeof(out)) return -36;
                strcpy(out + start, file.name); count = start + name_size;
            }
        }
        if (error && (error != -2 || more || trailing)) return error;
        if (link && (more || trailing || follow_final)) {
            if (++links > 40) return -40;
            char target[FAT32_PATH_MAX], expanded[FAT32_PATH_MAX];
            if (virtual) {
                int magic = proc_magic_target(out, target, sizeof(target), &anchor); if (magic < 0) return magic;
                if (magic) {
                    if (!user_readable(target)) return -13;
                    if (more || trailing) {
                        struct linux_stat stat; int error = resolved_stat(target, anchor, true, &stat); if (error) return error;
                        if ((stat.mode & 0170000) != 0040000) return -20;
                    }
                    strcpy(out, target); count = strlen(out);
                    strcpy(expanded, p); strcpy(pending, expanded); p = pending; continue;
                }
            }
            int length = virtual ? proc_readlink(out, target, sizeof(target)) : device >= 0 ? device_readlink((unsigned)device, target, sizeof(target)) : link_contents(&file, target);
            if (length < 0) return length;
            /* Non-path proc magic links remain bound objects for procfs to handle. */
            if (virtual && target[0] != '/' && (target[0] < '0' || target[0] > '9')) {
                if (more || trailing) return -20;
                strcpy(path, out); return 0;
            }
            if ((unsigned)length + strlen(p) >= sizeof(expanded)) return -36;
            strcpy(expanded, target); strcat(expanded, p);
            if (target[0] == '/') { anchor = active_filesystem->root; strcpy(out, filesystem_path(active_filesystem, true)); count = strlen(out); }
            else { anchor = parent_anchor; count = parent; out[count] = 0; }
            strcpy(pending, expanded); p = pending; continue;
        }
        if (!error && (more || trailing) && !directory) return -20;
    }
    out[count] = 0; strcpy(path, out); if (bound) *bound = anchor; return 0;
}
static int canonical_path(char* path) { return resolve_path(path, true, NULL, NULL); }
static int path_at_bound(uint64_t directory, char* path, bool follow_final, struct open_file** bound) {
    if (!path[0]) return -2;
    struct open_file* base = NULL;
    if (path[0] != '/' && (int32_t)directory != -100) {
        base = file_get(directory); if (!base) return -9;
        struct linux_stat stat; int error = fd_description_stat(base, &stat); if (error) return error;
        if ((stat.mode & 0170000) != 0040000) return -20;
    }
    return resolve_path(path, follow_final, base, bound);
}
static int path_at(uint64_t directory, char* path, bool follow_final) { return path_at_bound(directory, path, follow_final, NULL); }
static bool path_below(const char* path, const char* prefix) {
    unsigned i = 0;
    for (; prefix[i]; i++) {
        unsigned char c = path[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)prefix[i]) return false;
    }
    return !path[i] || path[i] == '/';
}
static bool private_path(const char* path) {
    return path_below(path, "/home/nexus") || path_below(path, "/tmp") || path_below(path, "/run/user/1000");
}
static bool user_readable(const char* path) {
    return disk_posix() || !private_path(path) || credentials_kernel_id(credentials_current(), false, 3) == 1000 ||
           credentials_capable(credentials_current(), NULL, 1) || credentials_capable(credentials_current(), NULL, 2);
}
bool kernel_private_path(const char* path) { return private_path(path); }
static bool user_writable(const char* path) {
    return disk_posix() || mount_path(path) || (private_path(path) && (credentials_kernel_id(credentials_current(), false, 3) == 1000 || credentials_capable(credentials_current(), NULL, 1)));
}
static void parent_path(const char* path, char* parent) {
    strcpy(parent, path);
    unsigned end = strlen(parent);
    while (end > 1 && parent[end - 1] != '/') end--;
    if (end > 1) end--;
    parent[end] = 0;
}
bool mount_path_readonly(const char* path);   /* mount.c: a read-only (bind) mount covers path */
static int writable_parent(const char* path) { /* 0 or -errno */
    if (mount_path_readonly(path)) return -30;
    if (disk_posix() || mount_path(path)) return disk_parent_access(path);
    char parent[FAT32_PATH_MAX]; parent_path(path, parent); return !fat32_is_readonly() && user_writable(parent) ? 0 : -30;
}
static bool same_file(const fat32_file_t* a, const fat32_file_t* b) {
    return disk_same_inode(a, b);
}
int disk_file_touch(fat32_file_t* file) {
    rtc_time_t now; rtc_read(&now);
    uint16_t date = (uint16_t)((now.year - 1980) << 9) | (now.month << 5) | now.day;
    uint16_t time = (now.hour << 11) | (now.minute << 5) | (now.second / 2);
    return fat32_set_times(file, date, time) ? -5 : 0;
}
/* Cache-scoped refresh: only descriptions on this cache, and VMAs only when it is mapped. */
void memory_file_refresh_views(struct memory_file* cache) {
    disk_orphan_refresh(&cache->disk);
    for (struct open_file* f = cache->views; f; f = f->view_next) if (!f->description && f->kind == FD_FILE) f->file = cache->disk;
    if (!cache->maps) return;
    for (struct address_space* space = spaces; space; space = space->next)
        for (struct vm_area* a = space->maps; a; a = a->next) if (a->memory == cache && a->file_backed) a->file = cache->disk;
}
void file_refresh_views(const fat32_file_t* file) {
    disk_orphan_refresh(file);
    for (struct open_file* f = files; f; f = f->next)
        if (!f->description && f->references && f->kind == FD_FILE && same_file(&f->file, file)) f->file = *file;
    for (struct address_space* space = spaces; space; space = space->next)
        for (struct vm_area* a = space->maps; a; a = a->next) if (a->file_backed && same_file(&a->file, file)) a->file = *file;
}
bool disk_inode_live(const fat32_file_t* file) {
    if (memory_file_disk_find(file)) return true;
    /* Regular-file descriptions (open) and file mappings (mmap, image segments) all hold the cache. */
    if (!file->is_dir) return false;
    for (struct open_file* f = files; f; f = f->next)
        if (!f->description && f->references && f->kind == FD_FILE && same_file(&f->file, file)) return true;
    for (struct address_space* space = spaces; space; space = space->next)
        for (struct vm_area* a = space->maps; a; a = a->next) if (a->file_backed && same_file(&a->file, file)) return true;
    return false;
}
bool kernel_mount_busy(unsigned id) {   /* mount.c: open files, working/root directories and mappings on a mount */
    for (struct open_file* f = files; f; f = f->next) if (f->references && f->kind == FD_FILE && f->file.mount == id) return true;
    for (struct address_space* space = spaces; space; space = space->next)
        for (struct vm_area* a = space->maps; a; a = a->next) if (a->file_backed && a->file.mount == id) return true;
    return false;
}
static bool file_executing(const fat32_file_t* file) {
    /* Every file-backed image segment holds a reference on its inode's cache. */
    struct memory_file* cache = memory_file_disk_find(file);
    return cache && cache->exec_maps;
}
static int executable_file(fat32_file_t* file) {
    for (struct open_file* f = files; f; f = f->next)
        if (!f->description && f->references && f->kind == FD_FILE && (f->flags & 3) && same_file(&f->file, file)) return -26;
    struct memory_file* cache = memory_file_disk_find(file);
    if (!cache) return 0;
    if (memory_file_writable_maps(cache)) return -26;
    memory_file_retain(cache); int error = memory_file_sync(cache, 0, ~0ULL);
    if (!error) *file = cache->disk;
    memory_file_release(cache); return error;
}
static int file_truncate(struct open_file* f, uint64_t size) {
    if (f->kind == FD_MEMORY) return !(f->flags & 3) ? -22 : memory_file_truncate(f->memory, size);
    if (f->kind != FD_FILE) return -22;
    if ((f->flags & 3) == 0) return -22;
    if ((int64_t)size < 0) return -22;
    if (size > disk_file_max_size(&f->file)) return -27;
    if (f->file.is_dir) return -21;
    if (disk_file_readonly(&f->file)) return -30;
    if (file_executing(&f->file)) return -26;
    int error = memory_file_truncate(f->memory, size);
    if (!error && f->file.mount && mount_write_through(&f->file)) error = memory_file_sync(f->memory, 0, ~0ULL);
    if (!error && inotify_watches) inotify_event(f->path, &f->file, IN_MODIFY, 0);
    return error;
}
static int64_t description_write(struct open_file* f, uint64_t buffer, uint64_t count, uint64_t offset, bool positional) {
    if (!(f->flags & 3) || (f->flags & 0x200000)) return -9;   /* read-only, or O_PATH */
    if (f->kind == FD_MEMORY) {
        if (f->flags & 0x400) offset = f->memory->size;
        else if (!positional) offset = f->position;
        int64_t result = memory_file_io(f->memory, buffer, count, offset, true);
        if (result > 0 && !positional) f->position = offset + result; return result;
    }
    if (f->kind != FD_FILE && f->kind != FD_PROC && positional && !(f->kind == FD_DEVICE && f->device >= DEVICE_STORAGE)) return -29;
    if (f->kind == FD_DEVICE) { if ((int64_t)offset < 0 && positional) return -22; return device_io(f->device, buffer, count, true, positional ? &offset : &f->position, f->flags, &f->input); }
    if (f->kind == FD_PROC) {
        if (!positional) offset = f->position;
        if ((int64_t)offset < 0) return -22;
        int64_t result = proc_write(f->proc, buffer, count, offset);
        if (result > 0 && !positional) f->position += result; return result;
    }
    if (f->kind == FD_EPOLL) return -22;
    if (f->kind == FD_COUNTER) {
        struct iovec64 vector = {buffer, count}; return counter_io(f->counter, &vector, 1, count, true, (f->flags & 0x800) != 0);
    }
    if (f->kind == FD_CONSOLE) return console_write(buffer, count);
    if (f->kind == FD_OBJECT) { struct iovec64 vector = {buffer, count}; return f->ops->io(f->object, &vector, 1, count, true, f->flags); }
    if (f->kind == FD_SOCKET) return ipc_write(f->socket, buffer, count, f->flags & 0x800 ? 0x40 : 0);
    if (f->kind == FD_INET) {
        struct iovec64 vector = {buffer, count}; return inet_vectors(f->inet, &vector, 1, count, true, f->flags & 0x800 ? 0x40 : 0);
    }
    if (f->kind == FD_PIPE) {
        struct iovec64 vector = {buffer, count}; return pipe_io(f->pipe, &vector, 1, count, true, (f->flags & 0x800) != 0);
    }
    if (f->file.is_dir) return -21;
    if (!count) return 0;
    if (disk_file_readonly(&f->file)) return -30;
    if (count > 0x7FFFF000) count = 0x7FFFF000;
    if (!accessible(buffer, count, false)) return -14;
    if (file_executing(&f->file)) return -26;
    if (f->flags & 0x400) offset = f->memory->size;
    else if (!positional) offset = f->position;
    if ((int64_t)offset < 0) return -22;
    if (offset > disk_file_max_size(&f->file) || count > disk_file_max_size(&f->file) - offset) return -27;
    { uint64_t limit = task_file_size_limit();   /* RLIMIT_FSIZE: SIGXFSZ and EFBIG at the limit, a short write below it */
      if (offset >= limit) { task_thread_signal(0, (int32_t)task_visible_pid(task_tid()), 25); return -27; }
      if (count > limit - offset) count = limit - offset; }
    disk_write_privs(&f->file);
    int64_t result = memory_file_io(f->memory, buffer, count, offset, true);
    if (result < 0) return result;
    if (f->file.mount && mount_write_through(&f->file)) { int error = memory_file_sync(f->memory, offset, offset + (uint64_t)result); if (error) return error; }   /* FUSE, -o sync */
    if (memory_file_dirty_total() > 16384) memory_file_writeback(false, 0, 1024 * 1024);
    if (!positional) f->position = offset + result;
    if (result > 0 && inotify_watches) inotify_event(f->path, &f->file, IN_MODIFY, 0);
    return result;
}
static int64_t file_write(uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset, bool positional) {
    struct open_file* f = file_get(fd); if (!f) return -9;
    fd_pin(f); int64_t result = description_write(f, buffer, count, offset, positional); fd_unpin(f); return result;
}
int path_moved(char* path, const char* from, const char* to, bool apply) {
    unsigned old = strlen(from), replacement = strlen(to), length = strlen(path);
    if (strncmp(path, from, old) || (path[old] && path[old] != '/')) return 0;
    if (replacement + length - old >= FAT32_PATH_MAX) return -36;
    if (apply) { char tail[FAT32_PATH_MAX]; strcpy(tail, path + old); strcpy(path, to); strcat(path, tail); }
    return 0;
}
static int file_paths_moved(const fat32_file_t* file, const char* from, const char* to, bool apply) {
    for (struct open_file* f = files; f; f = f->next) {
        if (f->description || !f->references || f->kind != FD_FILE) continue;
        if (!file->is_dir && !same_file(file, &f->file)) continue;
        if (f->file.detached && !strcmp(f->path, from)) continue;
        int error = path_moved(f->path, from, to, apply); if (error) return error;
    }
    return task_files_moved(from, to, disk_inode_id(file), file->is_dir, apply);
}
static int sync_inode(fat32_file_t* file) {
    struct memory_file* cache = memory_file_disk_find(file);
    if (cache) {
        int error = memory_file_sync(cache, 0, ~0ULL); if (error) return error;
        *file = cache->disk;
    }
    file->native_ino = disk_inode_id(file); return 0;
}
static void file_views_moved(const fat32_file_t* old, const fat32_file_t* moved) {
    struct memory_file* cache = old->is_dir ? NULL : memory_file_disk_find(old);
    if (!old->is_dir) {
        /* Every description and mapping of a regular file holds its cache (none: nothing to move). */
        if (cache) {
            for (struct open_file* f = cache->views; f; f = f->view_next)
                if (!f->description && f->references && f->kind == FD_FILE) f->file = *moved;
            if (cache->maps) for (struct address_space* space = spaces; space; space = space->next)
                for (struct vm_area* a = space->maps; a; a = a->next) if (a->file_backed && a->memory == cache) a->file = *moved;
        }
        memory_file_disk_moved(old, moved); return;
    }
    for (struct open_file* f = files; f; f = f->next)
        if (!f->description && f->references && f->kind == FD_FILE && same_file(&f->file, old)) f->file = *moved;
    for (struct address_space* space = spaces; space; space = space->next)
        for (struct vm_area* a = space->maps; a; a = a->next) if (a->file_backed && same_file(&a->file, old)) a->file = *moved;
    memory_file_disk_moved(old, moved);
}
static void file_retired(struct disk_orphan* owner, const fat32_file_t* old, fat32_file_t* detached, const char* path) {
    detached->native_ino = disk_inode_id(old);
    disk_orphan_adopt(owner, detached); file_views_moved(old, detached);
    if (old->native_kind == FAT32_NATIVE_SOCKET) ipc_path_unlinked(detached->native_ino);
    task_files_moved(path, NULL, detached->native_ino, old->is_dir, true);
}
static int file_detach(const char* path, fat32_file_t* file) {
    /* Removing the last link of a regular file writes nothing back: an orphan is never read from the disk again (it
     * lives in its cache until the last user is gone, write-back skips detached files, and exec reads it through the
     * cache: image_open). Unlinked temporary files
     * (Wine's anonymous mappings: wineserver/mapping.c) would otherwise be zero-filled onto the disk first. */
    int error = 0;
    if (file->is_dir || ((disk_posix() || file->mount) && file->nlink > 1)) error = sync_inode(file);
    else { struct memory_file* cache = memory_file_disk_find(file); if (cache) *file = cache->disk; file->native_ino = disk_inode_id(file); }
    if (error) return error;
    if ((error = disk_inode_reserve(1))) return error;
    struct disk_orphan* owner = disk_orphan_prepare(); if (!owner) return -12;
    fat32_file_t detached; error = fat32_detach_path(path, &detached);
    if (error) { disk_orphan_cancel(owner); return error; }
    if (!detached.detached) { disk_orphan_cancel(owner); file_refresh_views(&detached); return 0; } /* other links remain */
    file_retired(owner, file, &detached, path);
    disk_inode_rekey(file, NULL); disk_orphan_collect(); return 0;
}
static unsigned dot_leaf(const char* path) {
    unsigned length = strlen(path); while (length && path[length - 1] == '/') length--;
    unsigned begin = length; while (begin && path[begin - 1] != '/') begin--;
    return length - begin == 1 && path[begin] == '.' ? 1 :
           length - begin == 2 && path[begin] == '.' && path[begin + 1] == '.' ? 2 : 0;
}
static int file_rename(uint64_t old_dir, uint64_t old_address, uint64_t new_dir, uint64_t new_address, uint32_t flags) {
    if (flags & ~7u || (flags & 2 && flags & 5)) return -22;
    if (flags & 6) return -95;
    char from[FAT32_PATH_MAX], to[FAT32_PATH_MAX];
    int error = user_path(old_address, from); if (error) return error;
    if ((error = user_path(new_address, to))) return error;
    if (dot_leaf(from) || dot_leaf(to)) return -16;
    char spelled[FAT32_PATH_MAX]; strcpy(spelled, to);   /* the caller's spelling of the last component */
    if ((error = path_at(old_dir, from, false)) || (error = path_at(new_dir, to, false))) return error;
    if (shm_path(from) || shm_path(to)) return shm_rename_path(from, to, flags & 1);
    if ((error = writable_parent(from)) || (error = writable_parent(to))) return error;
    fat32_file_t source, target;
    if ((error = fat32_lookup_status(from, &source))) return error;
    error = fat32_lookup_status(to, &target); if (error && error != -2) return error;
    bool exists = !error;
    if ((disk_posix() || source.mount) && ((error = disk_delete_access(from, &source)) || (exists && (error = disk_delete_access(to, &target))))) return error;
    if ((disk_posix() || source.mount) && source.is_dir) { /* moving a directory rewrites its '..' entry */
        char a[FAT32_PATH_MAX], b[FAT32_PATH_MAX]; parent_path(from, a); parent_path(to, b);
        if (strcmp(a, b) && (error = disk_permission(&source, DISK_MAY_WRITE))) return error;
    }
    if (exists && flags & 1) return -17;
    if (exists && same_file(&source, &target)) {
        /* A case-insensitive mounted filesystem (vfat, exfat): a case-only rename keeps the caller's spelling. */
        unsigned n = strlen(spelled); while (n > 1 && spelled[n - 1] == '/') spelled[--n] = 0;
        const char* leaf = spelled + n; while (leaf > spelled && leaf[-1] != '/') leaf--;
        if (!source.mount || strcmp(from, to) || !strcmp(leaf, target.name)) return 0;
        unsigned keep = strlen(to); while (keep > 1 && to[keep - 1] != '/') keep--;
        if (keep + strlen(leaf) >= sizeof(to)) return -36;
        to[keep] = 0; strcat(to, leaf); exists = false;
    }
    if ((error = file_paths_moved(&source, from, to, false))) return error;
    if ((error = sync_inode(&source)) || (exists && (error = sync_inode(&target)))) return error;
    if ((error = disk_inode_reserve(2))) return error;
    struct disk_orphan* owner = exists ? disk_orphan_prepare() : NULL;
    if (exists && !owner) return -12;
    fat32_file_t moved, replaced;
    error = fat32_rename_path(from, to, (flags & 1) || !exists, &moved, &replaced);
    if (error) { disk_orphan_cancel(owner); return error; }
    if (exists && !replaced.detached && replaced.native_ino) { disk_orphan_cancel(owner); file_refresh_views(&replaced); exists = false; }
    if (exists) file_retired(owner, &target, &replaced, to);
    moved.native_ino = source.native_ino;
    file_views_moved(&source, &moved);
    if (file_paths_moved(&source, from, to, true)) panic("rename path changed after validation");
    disk_inode_rekey(&source, &moved); disk_orphan_collect();
    if (inotify_watches) {
        uint32_t cookie = inotify_cookie();
        inotify_event(from, &moved, IN_MOVED_FROM, cookie); inotify_event(to, &moved, IN_MOVED_TO, cookie);
        inotify_event(to, &moved, IN_MOVE_SELF, 0);
        if (exists) inotify_event(to, &target, IN_DELETE_SELF, 0);
    }
    return 0;
}
static int directory_change(uint64_t directory, uint64_t address, bool create, uint64_t mode) {
    char path[FAT32_PATH_MAX]; int result = user_path(address, path);
    if (result) return result;
    unsigned dot = dot_leaf(path); if (!create && dot) return dot == 1 ? -22 : -39;
    unsigned length = strlen(path); while (length > 1 && path[length - 1] == '/') path[--length] = 0;
    if ((result = path_at(directory, path, false))) return result;
    fat32_file_t file; bool exists = fat32_lookup(path, &file);
    if (create && exists) return -17;
    if (!create && !exists) return -2;
    if ((result = create || !(disk_posix() || file.mount) ? writable_parent(path) : disk_delete_access(path, &file))) return result;
    if (!create && !file.is_dir) return -20;
    if (!create && !strcmp(path, filesystem_path(active_filesystem, true))) return -16;
    if (!create) {
        fat32_file_t gone = file; gone.native_ino = disk_inode_id(&file);
        result = file_detach(path, &file);
        if (!result && inotify_watches) inotify_event(path, &gone, IN_DELETE | IN_DELETE_SELF, 0);
        return result;
    }
    result = disk_create_node(path, 0040000 | (mode & 07777), 0, NULL, &file);
    if (!result && inotify_watches) inotify_event(path, &file, IN_CREATE, 0);
    if (!result && !disk_posix() && !file.mount) return disk_file_touch(&file);
    return result;
}
static uint64_t file_identity(const fat32_file_t* file) { return disk_inode_id(file); }
int unix_path_inode(const char* source, bool create, uint64_t* inode) {
    char path[FAT32_PATH_MAX];
    unsigned size = strlen(source); if (!size || size >= sizeof(path)) return -36;
    if (source[size - 1] == '/') return -22;
    strcpy(path, source); int error = path_at((uint64_t)-100, path, !create); if (error) return error;
    fat32_file_t file; bool exists = fat32_lookup(path, &file);
    if (create) {
        if (exists) return -98;
        if ((error = writable_parent(path))) return error;
        error = fat32_create_socket_path(path, &file);
        if (error) return error == -2 ? -98 : error == -3 ? -2 : error == -4 ? -28 : -5;
        if (disk_file_touch(&file)) { fat32_remove_path(path); return -5; }
        ipc_path_unlinked(file_identity(&file));
        if (inotify_watches) inotify_event(path, &file, IN_CREATE, 0);
    } else {
        if (!exists) return -2;
        if (file.native_kind != FAT32_NATIVE_SOCKET) return -111;
        if (disk_posix() || file.mount ? disk_permission(&file, DISK_MAY_WRITE) != 0 : !user_writable(path)) return -13;
    }
    *inode = file_identity(&file); return 0;
}
static int file_unlink(uint64_t directory, uint64_t address, uint64_t flags) {
    if (flags & ~0x200ULL) return -22;
    if (flags & 0x200) return directory_change(directory, address, false, 0);
    char path[FAT32_PATH_MAX]; int error = user_path(address, path); if (error) return error;
    if ((error = path_at(directory, path, false))) return error;
    if (shm_path(path)) return shm_unlink_path(path);
    { bool storage_kernel_link(const char* path); if (storage_kernel_link(path)) return 0; }   /* /dev/<vg>/<lv>: kept (lvm2 re-links) */
    { bool storage_unlink_node(const char* path); if (storage_unlink_node(path)) return 0; }   /* a node mknod made in /dev */
    if (device_lookup(path) >= 0) return -1;   /* /dev nodes are the kernel's: EPERM, as devfs (libdevmapper expects it) */
    if ((error = writable_parent(path))) return error;
    fat32_file_t file; if (!fat32_lookup(path, &file)) return -2;
    if (file.is_dir) return -21;
    if ((error = disk_delete_access(path, &file))) return error;
    fat32_file_t gone = file; gone.native_ino = disk_inode_id(&file);
    error = file_detach(path, &file);
    if (!error && inotify_watches) inotify_event(path, &gone, IN_DELETE | IN_DELETE_SELF, 0);
    return error;
}
static int file_symlink(uint64_t target_address, uint64_t directory, uint64_t name_address) {
    char target[FAT32_PATH_MAX], path[FAT32_PATH_MAX];
    int error = user_path(target_address, target); if (error) return error;
    if (!target[0]) return -2;
    if ((error = user_path(name_address, path)) || (error = path_at(directory, path, false))) return error;
    { bool storage_kernel_link(const char* path); if (storage_kernel_link(path)) return 0; }   /* /dev/<vg>/<lv> is the kernel's already */
    if ((error = writable_parent(path))) return error;
    fat32_file_t file;
    if (fat32_lookup(path, &file)) return -17;
    error = disk_create_node(path, 0120777, 0, target, &file);
    if (!error && inotify_watches) inotify_event(path, &file, IN_CREATE, 0);
    return error;
}
static int file_open_resolved(const char* path, uint64_t flags, struct open_file* bound, bool internal, uint64_t mode) {
    if ((flags & 3) == 3) return -22;
    if (!internal) { int slot = unused_descriptor(0); if (slot < 0) return slot; }
    if (bound && bound->kind == FD_MEMORY) {
        if (flags & 0x10000) return -20;
        if (flags & ~(3ULL | 0x200ULL | 0x400ULL | 0x800ULL | 0x8000ULL | 0x80000ULL | 0x101000ULL)) return -22;
        if (((flags & 3) != 1 && !memory_file_access(bound->memory, false)) ||
            ((flags & (3 | 0x200)) && !memory_file_access(bound->memory, true))) return -13;
        if (flags & 0x200) { int error = memory_file_truncate(bound->memory, 0); if (error) return error; }
        return memory_file_descriptor(bound->memory, bound->path, flags);
    }
    if (shm_path(path)) return shm_open_path(path, flags, open_mode, active_filesystem->umask);
    if (kmsg_path(path)) return kmsg_open(flags); /* /dev/kmsg (kmsg.c) */
    if (!user_readable(path)) return -13;
    if (!strcmp(path, "/dev/net/tun")) return tun_open(flags);   /* TUN/TAP (tun.c): an FD_OBJECT, not a numbered device */
    int device = device_lookup(path);
    if (device >= 0 && (unsigned)device >= DEVICE_PTMX && (unsigned)device < DEVICE_STORAGE) return pty_open_device((unsigned)device, flags);
    if (device >= 0) {
        int error = device_open_check((unsigned)device, flags); if (error) return error;
        int fd = unused_descriptor_until(0, internal ? 0x80000000ULL : task_resource_soft(7)); if (fd < 0) return fd;
        struct open_file* f = descriptor_new(fd); if (!f) return -12;
        f->kind = FD_DEVICE; f->device = (unsigned)device; f->flags = flags; f->close_exec = (flags & 0x80000) != 0;
        f->position = device_open(f->device, flags, &f->input);
        strcpy(f->path, path); return fd;
    }
    if (proc_path(path)) {
        int error;
        struct open_file* object = bound ? NULL : proc_fd_object(path);
        if (object && object->kind == FD_PIPE) return pipe_reopen_descriptor(object, flags);
        struct proc_handle* proc = bound && bound->kind == FD_PROC ? proc_clone(bound->proc, flags, &error) : proc_open(path, flags, &error);
        if (!proc) return error;
        int fd = unused_descriptor_until(0, internal ? 0x80000000ULL : task_resource_soft(7));
        if (fd < 0) { proc_close(proc); return fd; }
        struct open_file* f = descriptor_new(fd);
        if (!f) { proc_close(proc); return -12; }
        f->kind = FD_PROC; f->proc = proc; f->flags = flags; f->close_exec = (flags & 0x80000) != 0;
        error = proc_descriptor_path(proc, f->path, sizeof(f->path));
        if (error < 0) { descriptor_close(f); return error; }
        return fd;
    }
    if ((flags & 0x10040) == 0x10040) return -22;
    /* O_PATH (coreutils cp/mv/install open their target folder so): only O_DIRECTORY, O_NOFOLLOW, O_CLOEXEC count;
     * the descriptor locates the file (fstat, fchdir, *at) but never reads or writes it */
    if (flags & 0x200000ULL) flags &= 0x200000ULL | 0x10000ULL | 0x20000ULL | 0x80000ULL;
    if (flags & ~(3ULL | 0x40ULL | 0x80ULL | 0x100ULL | 0x200ULL | 0x400ULL | 0x800ULL | 0x101000ULL |
                  0x80000ULL | 0x10000ULL | 0x20000ULL | 0x8000ULL | 0x200000ULL | 0x4000ULL)) return -22;   /* O_DIRECT: block nodes only (below) */
    fat32_file_t entry;
    bool exists;
    if (bound && bound->kind == FD_FILE) { entry = bound->file; exists = true; }
    else exists = fat32_lookup(path, &entry);
    if (exists && (entry.mode & 0170000) == 0060000) {   /* a block special file on disk (mdadm mknods a temporary one): the kernel's node */
        bool storage_rdev_path(uint32_t rdev, char* out, unsigned capacity); char node[64];
        return storage_rdev_path(entry.rdev, node, sizeof(node)) ? file_open_resolved(node, flags, NULL, internal, mode) : -6;
    }
    if (flags & 0x4000ULL) return -22;   /* O_DIRECT on files: not supported */
    if (!exists && !(flags & 0x40)) return -2;
    if (exists && (flags & 0xC0) == 0xC0) return -17;
    if (!disk_posix() && !mount_path(path) && (flags & (3 | 0x40 | 0x200 | 0x400)) && (fat32_is_readonly() || !user_writable(path))) return -30;
    if (exists && entry.native_kind == 3 && !(flags & 0x200000ULL)) {   /* a FIFO (mkfifo): its pipe (fifo.c) */
        if (disk_posix() || entry.mount) { int error = disk_open_access(&entry, flags); if (error) return error; }
        return fifo_open(disk_inode_id(&entry), flags, path);
    }
    if (exists && entry.native_kind) return entry.native_kind == FAT32_NATIVE_SYMLINK ? -40 : -6;
    if (exists && (flags & (3 | 0x200)) && mount_path_readonly(path)) return -30;   /* also binds of root directories */
    if (!exists) {
        if (!(flags & 0x40)) return -2;
        int error = writable_parent(path); if (error) return error;
        if ((error = disk_create_node(path, 0100000 | (mode & 07777), 0, NULL, &entry))) return error;
        if (!disk_posix() && !entry.mount && disk_file_touch(&entry)) return -5;
        if (inotify_watches) inotify_event(path, &entry, IN_CREATE, 0);
    } else if (disk_posix() || entry.mount) {
        if (entry.is_dir && (flags & (3 | 0x200))) return -21;
        int error = disk_open_access(&entry, flags); if (error) return error;
    }
    if ((flags & 0x10000) && !entry.is_dir) return -20;
    if (entry.is_dir && (flags & (3 | 0x200))) return -21;
    if ((flags & (3 | 0x200)) && entry.read_only && !credentials_capable(credentials_current(), NULL, 1)) return -13;
    if ((flags & 3) && file_executing(&entry)) return -26;
    int next_fd = unused_descriptor_until(0, internal ? 0x80000000ULL : task_resource_soft(7)); if (next_fd < 0) return next_fd;
    struct open_file* free_slot = descriptor_new(next_fd); if (!free_slot) return -12;
    entry.native_ino = disk_inode_id(&entry);
    free_slot->file = entry; free_slot->position = 0; free_slot->open = true;
    free_slot->flags = flags;
    free_slot->close_exec = (flags & 0x80000) != 0;
    strcpy(free_slot->path, path);
    if (!entry.is_dir) {
        free_slot->memory = memory_file_disk_get(&entry);
        if (!free_slot->memory) { descriptor_close(free_slot); return -12; }
        free_slot->view_prev = NULL; free_slot->view_next = free_slot->memory->views;
        if (free_slot->view_next) free_slot->view_next->view_prev = free_slot;
        free_slot->memory->views = free_slot;
    }
    if (inotify_watches) inotify_event(path, &entry, IN_OPEN, 0);
    if (flags & 0x200) {
        int error = file_truncate(free_slot, 0);
        if (error) { descriptor_close(free_slot); return error; }
    }
    return free_slot->fd;
}
static int file_open(uint64_t directory, uint64_t path_address, uint64_t flags, uint64_t mode) {
    char path[FAT32_PATH_MAX]; int error = user_path(path_address, path); if (error) return error;
    if (!path[0]) return -2;
    if (path[strlen(path) - 1] == '/') flags |= 0x10000;
    struct open_file* bound;
    error = path_at_bound(directory, path, !(flags & 0x20000) && (flags & 0xC0) != 0xC0, &bound);
    int result = error ? error : file_open_resolved(path, flags, bound, false, mode);
    static unsigned misses;
    if (trace_processes && result < 0 && result != -11 && misses < 128 && (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5) || !strncmp(path, "/dev/", 5))) {
        misses++; say("[64] OPEN_MISS path="); say(path); say(" error="); hex((uint64_t)(int64_t)result); put('\n');
    }
    return result;
}
int kernel_user_path(uint64_t address, char* path) { return user_path(address, path); }
int kernel_path_at(uint64_t directory, char* path, bool follow, struct open_file** bound) { return path_at_bound(directory, path, follow, bound); }
struct open_file* kernel_file(uint64_t fd) { return file_get(fd); }
/* loop.c: the page cache of a regular or memory file description (NULL otherwise), its O_ flags, and its path. */
struct memory_file* kernel_file_cache(struct open_file* f, uint64_t* flags, const char** path) {
    if (!f || !((f->kind == FD_FILE && !f->file.is_dir) || f->kind == FD_MEMORY) || !f->memory) return NULL;
    if (flags) *flags = f->flags;
    if (path) *path = f->path;
    return f->memory;
}
fat32_file_t* kernel_file_disk(struct open_file* f) { return f && f->kind == FD_FILE ? &f->file : NULL; }
int kernel_file_device(struct open_file* f, struct input_reader** reader) {   /* fuse.c: /dev/fuse descriptors in mount(2)'s fd= */
    if (!f || f->kind != FD_DEVICE) return -1;
    if (reader) *reader = &f->input;
    return (int)f->device;
}
struct memory_file* kernel_file_memory(struct open_file* f) { return f ? f->memory : NULL; }
int kernel_resolved_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* st) { return resolved_stat(path, bound, follow, st); }
int kernel_truncate_path(const char* source, uint64_t size) {
    char path[FAT32_PATH_MAX]; strcpy(path, source); struct open_file* bound;
    int error = path_at_bound((uint64_t)-100, path, true, &bound); if (error) return error;
    int fd = file_open_resolved(path, 1, bound, true, 0); if (fd < 0) return fd;
    struct open_file* f = file_get(fd); fd_pin(f); error = file_truncate(f, size); fd_unpin(f); fd_close(fd);
    return error;
}
static int filesystem_change(uint64_t call, uint64_t argument) {
    if (call == 161 && !credentials_capable(credentials_current(), credentials_namespace(credentials_current()), 18)) return -1;
    char path[FAT32_PATH_MAX]; struct open_file* bound;
    if (call == 81) {
        bound = file_get(argument); if (!bound) return -9;
        strcpy(path, bound->path);
    } else {
        int error = user_path(argument, path); if (error) return error;
        error = path_at_bound((uint64_t)-100, path, true, &bound); if (error) return error;
    }
    struct linux_stat stat; int error = resolved_stat(path, bound, true, &stat); if (error) return error;
    if ((stat.mode & 0170000) != 0040000) return -20;
    if (!user_readable(path)) return -13;
    /* The retained directory is a kernel reference, independent of user fd slots. */
    int fd = file_open_resolved(path, 0x10000, bound, true, 0); if (fd < 0) return fd;
    struct open_file* captured = file_get(fd); fd_pin(captured); fd_close(fd);
    struct open_file** target = call == 161 ? &active_filesystem->root : &active_filesystem->cwd;
    struct open_file* previous = *target; *target = captured;
    if (previous) fd_unpin(previous);
    return 0;
}
static int64_t filesystem_getcwd(uint64_t address, uint64_t capacity) {
    const char* cwd = filesystem_path(active_filesystem, false);
    if (active_filesystem->cwd) {
        struct linux_stat stat; int error = fd_description_stat(active_filesystem->cwd, &stat);
        if (error || !stat.nlink) return -2;
    }
    const char* visible = filesystem_visible_path(cwd); char text[FAT32_PATH_MAX + 16] = "";
    if (filesystem_chrooted() && visible == cwd) strcpy(text, "(unreachable)");
    strcat(text, visible); unsigned length = strlen(text) + 1;
    if (length > capacity) return -34;
    if (!accessible(address, length, true)) return -14;
    if (copy_to_user((void*)address, text, length)) return -14; return length;
}
static int64_t description_read(struct open_file* f, uint64_t buffer, uint64_t count, uint64_t offset, bool positional) {
    if ((f->flags & 3) == 1 || (f->flags & 0x200000)) return -9;   /* write-only, or O_PATH */
    if (f->kind == FD_MEMORY) {
        if (!positional) offset = f->position;
        int64_t result = memory_file_io(f->memory, buffer, count, offset, false);
        if (result > 0 && !positional) f->position += result; return result;
    }
    if (f->kind != FD_FILE && f->kind != FD_PROC && positional && !(f->kind == FD_DEVICE && f->device >= DEVICE_STORAGE)) return -29;
    if (f->kind == FD_DEVICE) { if ((int64_t)offset < 0 && positional) return -22; return device_io(f->device, buffer, count, false, positional ? &offset : &f->position, f->flags, &f->input); }
    if (f->kind == FD_PROC) {
        if (!positional) offset = f->position;
        if ((int64_t)offset < 0) return -22;
        int64_t result = proc_read(f->proc, buffer, count, offset);
        if (result > 0 && !positional) f->position += result; return result;
    }
    if (f->kind == FD_EPOLL) return -22;
    if (f->kind == FD_COUNTER) {
        struct iovec64 vector = {buffer, count}; return counter_io(f->counter, &vector, 1, count, false, (f->flags & 0x800) != 0);
    }
    if (f->kind == FD_CONSOLE) return count ? -11 : 0;
    if (f->kind == FD_OBJECT) { struct iovec64 vector = {buffer, count}; return f->ops->io(f->object, &vector, 1, count, false, f->flags); }
    if (f->kind == FD_SOCKET) return ipc_read(f->socket, buffer, count, f->flags & 0x800 ? 0x40 : 0);
    if (f->kind == FD_INET) {
        struct iovec64 vector = {buffer, count}; return inet_vectors(f->inet, &vector, 1, count, false, f->flags & 0x800 ? 0x40 : 0);
    }
    if (f->kind == FD_PIPE) {
        struct iovec64 vector = {buffer, count}; return pipe_io(f->pipe, &vector, 1, count, false, (f->flags & 0x800) != 0);
    }
    if (f->file.is_dir) return -21;
    if (!positional) offset = f->position;
    if ((int64_t)offset < 0) return -22;
    int64_t result = memory_file_io(f->memory, buffer, count, offset, false);
    if (result < 0) return result;
    if (!positional) f->position += (uint32_t)result;
    if (result > 0 && inotify_watches) inotify_event(f->path, &f->file, IN_ACCESS, 0);
    return result;
}
static int64_t file_read(uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset, bool positional) {
    struct open_file* f = file_get(fd); if (!f) return -9;
    fd_pin(f); int64_t result = description_read(f, buffer, count, offset, positional); fd_unpin(f); return result;
}
static int64_t file_vectors(uint64_t fd, uint64_t address, uint64_t count, bool write) {
    struct open_file* f = file_get(fd); if (!f || (write ? !(f->flags & 3) : (f->flags & 3) == 1) || (f->flags & 0x200000)) return -9;
    if (count > 1024) return -22;
    struct iovec64 vectors[1024]; uint64_t total = 0;
    if (count && !accessible(address, count * sizeof(*vectors), false)) return -14;
    if (count && copy_from_user(vectors, (void*)address, count * sizeof(*vectors))) return -14;
    for (unsigned i = 0; i < count; i++) {
        if (vectors[i].size > 0x7FFFFFFFFFFFFFFFULL - total) return -22;
        total += vectors[i].size;
    }
    if (!total) return 0;
    fd_pin(f); int64_t result = 0;
    if (f->kind == FD_PIPE) result = pipe_io(f->pipe, vectors, count, total, write, (f->flags & 0x800) != 0);
    else if (f->kind == FD_COUNTER) result = counter_io(f->counter, vectors, count, total, write, (f->flags & 0x800) != 0);
    else if (f->kind == FD_SOCKET) result = ipc_vectors(f->socket, vectors, count, total, write, f->flags & 0x800 ? 0x40 : 0);
    else if (f->kind == FD_INET) result = inet_vectors(f->inet, vectors, count, total, write, f->flags & 0x800 ? 0x40 : 0);
    else if (f->kind == FD_OBJECT) result = f->ops->io(f->object, vectors, count, total, write, f->flags);
    else if (write && count > 1 && f->kind == FD_DEVICE && f->device >= DEVICE_STORAGE && storage_message_device(f->device))
        result = storage_write_vectors(f->device, vectors, (unsigned)count, total, &f->input);   /* /dev/fuse: one reply per writev */
    else if (write && count > 1 && f->kind == FD_DEVICE && hwnode_write_vectors(f->device, vectors, (unsigned)count, total, f->flags, &f->input, &result)) {}   /* /dev/vhci: one packet per writev (hwnode.c) */
    else for (unsigned i = 0; i < count; i++) {
        if (!vectors[i].size) continue;
        int64_t part = write ? description_write(f, vectors[i].address, vectors[i].size, 0, false) :
                               description_read(f, vectors[i].address, vectors[i].size, 0, false);
        if (part < 0) { if (!result) result = part; break; }
        result += part;
        if ((uint64_t)part != vectors[i].size) break;
    }
    fd_unpin(f); return result;
}
static int64_t socket_send_messages(uint64_t fd, uint64_t address, uint64_t count, uint64_t flags) {
    struct batch_message { struct message64 message; uint32_t length, pad; };
    _Static_assert(sizeof(struct batch_message) == 64 && __builtin_offsetof(struct batch_message, length) == 56, "sendmmsg ABI");
    struct open_file* f = file_get(fd); if (!f) return -9;
    if (f->kind != FD_SOCKET && f->kind != FD_INET) return -88;
    count = (uint32_t)count; if (count > 1024) count = 1024;
    unsigned done = 0; int64_t error = 0; fd_pin(f);
    for (; done < count; done++) {
        if (address > ~0ULL - (uint64_t)done * 64 - 60) { error = -14; break; }
        uint64_t message = address + (uint64_t)done * 64;
        if (!accessible(message + 56, 4, true)) { error = -14; break; }
        struct registers call = {.rax = 46, .rdi = fd, .rsi = message, .rdx = flags};
        error = f->kind == FD_INET ? inet_socket_call(f->inet, f->flags, &call) : ipc_socket_call(f->socket, f->flags, &call);
        if (error < 0) break;
        if (!accessible(message + 56, 4, true)) { error = -14; break; }
        uput32(message + 56, (uint32_t)error);
    }
    fd_unpin(f); return done ? (int64_t)done : error;
}
/* recvmmsg(2): recvmsg per entry; MSG_WAITFORONE (or a timeout) makes the entries after the first non-blocking. */
static int64_t socket_receive_messages(uint64_t fd, uint64_t address, uint64_t count, uint64_t flags, uint64_t timeout) {
    struct open_file* f = file_get(fd); if (!f) return -9;
    if (f->kind != FD_SOCKET && f->kind != FD_INET) return -88;
    if (timeout && !accessible(timeout, 16, false)) return -14;
    count = (uint32_t)count; if (count > 1024) count = 1024;
    unsigned done = 0; int64_t error = 0; fd_pin(f);
    for (; done < count; done++) {
        if (address > ~0ULL - (uint64_t)done * 64 - 60) { error = -14; break; }
        uint64_t message = address + (uint64_t)done * 64;
        if (!accessible(message + 56, 4, true)) { error = -14; break; }
        uint64_t these = (flags & ~0x10000ULL) | (done && ((flags & 0x10000) || timeout) ? 0x40 : 0);
        struct registers call = {.rax = 47, .rdi = fd, .rsi = message, .rdx = these};
        error = f->kind == FD_INET ? inet_socket_call(f->inet, f->flags, &call) : ipc_socket_call(f->socket, f->flags, &call);
        if (error < 0) break;
        if (!accessible(message + 56, 4, true)) { error = -14; break; }
        uput32(message + 56, (uint32_t)error);
    }
    fd_unpin(f); return done ? (int64_t)done : error;
}
static int64_t file_seek(uint64_t fd, int64_t offset, uint64_t whence) {
    struct open_file* f = file_get(fd); if (!f) return -9;
    if (f->kind == FD_DEVICE) {
        return device_seek(f->device, &f->position, offset, whence, &f->input);
    }
    if (f->kind != FD_FILE && f->kind != FD_PROC && f->kind != FD_MEMORY) return -29;
    if (f->kind == FD_PROC && whence == 2) return -22;
    if ((whence == 3 || whence == 4) && f->kind == FD_FILE && !f->file.is_dir) {
        if (offset < 0) return -6;
        uint64_t found; int error = disk_seek_data(&f->file, f->memory, (uint64_t)offset, whence == 4, &found);
        if (error) return error;
        f->position = found; return (int64_t)found;
    }
    if (whence > 2) return -22;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->position : f->memory ? f->memory->size : f->file.size, position;
    if (__builtin_add_overflow(base, offset, &position) || position < 0) return -22;
    f->position = (uint64_t)position; return position;
}
static void stat_fill(const fat32_file_t* file, const char* path, struct linux_stat* output) {
    if (file && disk_stat(file, memory_file_disk_find(file), output)) return;
    struct linux_stat st = {0};
    st.dev = 1; st.nlink = 1; st.uid = 1000; st.gid = 1000; st.block_size = PAGE;
    if (file) {
        bool private = path && private_path(path);
        st.uid = credentials_visible_id(private ? 1000 : 0, false); st.gid = credentials_visible_id(private ? 1000 : 0, true);
        st.ino = disk_inode_id(file); st.nlink = file->detached ? 0 : 1;
        st.mode = (file->is_dir ? 0040000 : file->native_kind == FAT32_NATIVE_SOCKET ? 0140000 :
                   file->native_kind == FAT32_NATIVE_SYMLINK ? 0120000 : 0100000) |
                  ((file->native_kind == FAT32_NATIVE_SYMLINK ? 0777 : private ? 0700 : 0555) & (file->read_only ? ~0222U : ~0U));
        st.size = file->size; st.blocks = ((uint64_t)file->size + 511) / 512;
        st.atime = st.mtime = st.ctime = fat32_time_to_unix(file->wdate, file->wtime);
        struct memory_file* cache = memory_file_disk_find(file);
        if (cache) {
            st.size = cache->size; st.blocks = ((uint64_t)cache->disk.size + 511) / 512;
            st.mtime = st.ctime = cache->changed / 1000000000;
            st.mtime_ns = st.ctime_ns = cache->changed % 1000000000;
        }
    } else { st.ino = 1; st.mode = 0020666; st.rdev = 1; }
    *output = st;
}
static int stat_copy(const fat32_file_t* file, uint64_t address, const char* path) {
    if (!accessible(address, sizeof(struct linux_stat), true)) return -14;
    struct linux_stat st; stat_fill(file, path, &st); return copy_to_user((void*)address, &st, sizeof(st)) ? -14 : 0;
}
int disk_path_stat(const char* path, struct linux_stat* stat) {
    fat32_file_t file; int error = fat32_lookup_status(path, &file); if (error) return error;   /* -2, -5, -107 (dead FUSE mount) */
    stat_fill(&file, path, stat); return 0;
}
int fd_description_stat(struct open_file* f, struct linux_stat* st) {
    if (f->kind == FD_MEMORY) { memory_file_stat(f->memory, st); return 0; }
    if (f->kind == FD_DEVICE) { device_description_stat(f->device, &f->input, st); return 0; }
    if (f->kind == FD_PROC) return proc_stat(f->proc, st);
    if (f->kind == FD_OBJECT) { f->ops->stat(f->object, st); return 0; }
    stat_fill(f->kind == FD_FILE ? &f->file : NULL, f->path, st);
    if (f->kind != FD_FILE) {
        st->uid = credentials_visible_id(f->owner_uid, false); st->gid = credentials_visible_id(f->owner_gid, true);
    }
    if (f->kind == FD_SOCKET || f->kind == FD_INET) {
        st->mode = 0140777; st->ino = f->id; st->dev = 4; st->rdev = 0;
    }
    if (f->kind == FD_PIPE) {
        st->mode = 0010600; st->ino = pipe_identity(f->pipe); st->dev = 5; st->rdev = 0;
    }
    if (f->kind == FD_EPOLL || f->kind == FD_COUNTER) {
        st->mode = 0600; st->ino = f->id; st->dev = 6; st->rdev = 0;
    }
    return 0;
}
int fd_lock_info(struct open_file* file, struct lock_file_info* info) {
    if (file->kind != FD_FILE && file->kind != FD_MEMORY && file->kind != FD_DEVICE) return -95;   /* devices: ntfs-3g locks its block device */
    struct linux_stat stat; int error = fd_description_stat(file, &stat); if (error) return error;
    *info = (struct lock_file_info){stat.dev, stat.ino, file->position, stat.size, file->flags}; return 0;
}
struct statfs64 {
    uint64_t type, block_size, blocks, free, available, files, free_files;
    uint32_t fsid[2]; uint64_t name_max, fragment_size, flags, spare[4];
};
_Static_assert(sizeof(struct statfs64) == 120 && __builtin_offsetof(struct statfs64, name_max) == 64, "statfs ABI");
static int filesystem_stat(uint64_t source, uint64_t output, bool descriptor) {
    char path[FAT32_PATH_MAX]; struct linux_stat node; int error;
    if (descriptor) {
        struct open_file* file = file_get(source); if (!file) return -9;
        error = fd_description_stat(file, &node); strcpy(path, file->path);
        if (file->kind == FD_CONSOLE) node.dev = 9;
    } else {
        struct open_file* bound = NULL;
        error = user_path(source, path); if (error) return error;
        error = path_at_bound((uint64_t)-100, path, true, &bound); if (error) return error;
        error = resolved_stat(path, bound, true, &node);
    }
    if (error) return error;
    if (!accessible(output, sizeof(struct statfs64), true)) return -14;
    struct statfs64 stat = {.block_size = PAGE, .fragment_size = PAGE, .name_max = 255, .flags = 0x26};
    stat.fsid[0] = node.dev;
    uint64_t mounted[12];
    if (mount_statfs_dev(node.dev, mounted)) {   /* a mounted filesystem (mount.c) */
        stat.type = mounted[0]; stat.block_size = mounted[1]; stat.blocks = mounted[2]; stat.free = mounted[3]; stat.available = mounted[4];
        stat.files = mounted[5]; stat.free_files = mounted[6]; stat.fsid[0] = (uint32_t)mounted[7]; stat.fsid[1] = (uint32_t)mounted[8];
        stat.name_max = mounted[9]; stat.fragment_size = mounted[10]; stat.flags = mounted[11];
        return copy_to_user((void*)output, &stat, sizeof(stat)) ? -14 : 0;
    }
    switch (node.dev) {
    case 1: {
        /* FAT sizes and ext4 data are allocated at writeback; Linux allocates (vfat) or reserves (ext4) at once. */
        if (!fat32_is_readonly()) memory_file_writeback(false, 0, ~0ULL);
        fat32_space_t space; if (fat32_stat_volume(&space)) return -5;
        stat.type = 0x4D44; stat.block_size = stat.fragment_size = space.cluster_bytes;
        stat.blocks = space.total_clusters; stat.free = stat.available = space.free_clusters;
        stat.fsid[0] = space.serial; stat.fsid[1] = space.start_lba;
        if (disk_posix()) { disk_statfs(&stat.type, &stat.files, &stat.free_files); stat.flags = 0x420; /* ST_VALID|ST_NOATIME */ }
        if (disk_posix() ? fat32_is_readonly() : !private_path(path) || fat32_is_readonly()) stat.flags |= 1;
        break;
    }
    case 4: stat.type = 0x534F434B; stat.flags |= 8; break;
    case 5: stat.type = 0x50495045; stat.flags |= 8; break;
    case 6: stat.type = 0x09041934; stat.flags |= 8; break;
    case 7: stat.type = 0x9FA0; stat.flags |= 8; break;
    case 8: stat.type = 0x6E736673; stat.flags |= 9; break;
    case 9: stat.type = 0x01021994; stat.flags = 0x2B; break;
    case 10: stat.type = 0x01021994; break;
    case 11: stat.type = 0x1CD1; stat.flags |= 8; break; /* devpts */
    case 12: stat.type = 0x62656572; stat.flags |= 8; break; /* sysfs (sysfs.c st_dev 12; libudev's sd-device checks it) */
    default: return -38;
    }
    return copy_to_user((void*)output, &stat, sizeof(stat)) ? -14 : 0;
}
int fd_information(struct descriptor_table* table, uint32_t number, uint64_t values[4]) {
    if (!table) return -2;
    struct open_file* entry = descriptor_in(table, number); if (!entry) return -2;
    struct open_file* f = entry->description ? entry->description : entry;
    struct linux_stat stat; int error = fd_description_stat(f, &stat); if (error) return error;
    values[0] = f->position; values[1] = (f->flags & ~0x80000ULL) | (entry->close_exec ? 0x80000ULL : 0);
    values[2] = stat.dev; values[3] = stat.ino; return 0;
}
/* fdinfo.c: the type-specific /proc/PID/fdinfo lines (eventfd, epoll, and FD_OBJECT tables with an fdinfo hook) */
unsigned fdinfo_eventfd(struct event_counter* counter, char* out, unsigned capacity);
unsigned fdinfo_epoll(struct event_poll* poll, char* out, unsigned capacity);
unsigned timerfd_fdinfo_text(struct open_file* f, char* out, unsigned capacity);
unsigned signalfd_fdinfo_text(struct open_file* f, char* out, unsigned capacity);
unsigned pidfd_fdinfo_text(struct open_file* f, char* out, unsigned capacity);
unsigned inotify_fdinfo_text(struct open_file* f, char* out, unsigned capacity);
uint64_t fd_description_position(struct open_file* description) { return description->position; }
uint64_t fd_description_id(struct open_file* description) { return description->id; }   /* landlock.c: open order */
uint64_t fd_next_description_id(void) { return descriptor_id + 1; }
unsigned fd_information_text(struct descriptor_table* table, uint32_t number, char* out, unsigned capacity) {
    struct open_file* entry = table ? descriptor_in(table, number) : NULL; if (!entry || !capacity) return 0;
    struct open_file* f = entry->description ? entry->description : entry;
    if (f->kind == FD_OBJECT) {   /* timerfd, signalfd, pidfd, inotify: each matches its own operations table */
        unsigned (*const types[])(struct open_file*, char*, unsigned) = {timerfd_fdinfo_text, signalfd_fdinfo_text, pidfd_fdinfo_text, inotify_fdinfo_text};
        for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) { unsigned n = types[i](f, out, capacity); if (n) return n; }
        return 0;
    }
    if (f->kind == FD_EPOLL && f->poll) return fdinfo_epoll(f->poll, out, capacity);
    if (f->kind == FD_COUNTER && f->counter) return fdinfo_eventfd(f->counter, out, capacity);
    return 0;
}
static int file_stat(uint64_t fd, uint64_t address) {
    struct open_file* f = file_get(fd); if (!f) return -9;
    if (!accessible(address, sizeof(struct linux_stat), true)) return -14;
    struct linux_stat stat; int result = fd_description_stat(f, &stat);
    if (!result && copy_to_user((void*)address, &stat, sizeof(stat))) result = -14;
    return result;
}
static int path_stat(uint64_t directory, uint64_t path_address, uint64_t address, uint64_t flags) {
    if (flags & ~(0x100ULL | 0x1000ULL)) return -22;
    char path[FAT32_PATH_MAX]; int result = user_path(path_address, path);
    if (result) return result;
    if (!path[0]) return flags & 0x1000 ? file_stat(directory, address) : -2;
    struct open_file* bound;
    if ((result = path_at_bound(directory, path, !(flags & 0x100), &bound))) return result;
    if (!user_readable(path)) return -13;
    if (bound) {
        if (!accessible(address, sizeof(struct linux_stat), true)) return -14;
        struct linux_stat stat; int error = fd_description_stat(bound, &stat);
        if (!error && copy_to_user((void*)address, &stat, sizeof(stat))) error = -14; return error;
    }
    int device = device_lookup(path);
    if (device >= 0) {
        if (!accessible(address, sizeof(struct linux_stat), true)) return -14;
        struct linux_stat stat; device_stat((unsigned)device, &stat); return copy_to_user((void*)address, &stat, sizeof(stat)) ? -14 : 0;
    }
    if (proc_path(path)) {
        struct linux_stat stat; int error = proc_path_stat(path, !(flags & 0x100), &stat);
        if (error) return error;
        if (!accessible(address, sizeof(stat), true)) return -14;
        return copy_to_user((void*)address, &stat, sizeof(stat)) ? -14 : 0;
    }
    fat32_file_t file;
    return fat32_lookup(path, &file) ? stat_copy(&file, address, path) : -2;
}
/* Exported path/descriptor helpers (fsext.c): same resolution and permission rules as stat/open. */
int native_path_canonical(uint64_t directory, char* path, bool follow, struct open_file** bound) {
    if (!path[0]) return -2;
    int error = path_at_bound(directory, path, follow, bound); if (error) return error;
    return user_readable(path) ? 0 : -13;
}
int native_path_resolve(uint64_t directory, uint64_t address, bool follow, char* path, struct open_file** bound) {
    int error = user_path(address, path); return error ? error : native_path_canonical(directory, path, follow, bound);
}
bool native_parent_writable(const char* path) { return writable_parent(path) == 0; } /* writable_parent: 0 or -errno */
bool native_path_writable(const char* path) {
    if (mount_path(path)) { fat32_file_t f; return fat32_lookup(path, &f) ? !disk_file_readonly(&f) : true; }
    return !fat32_is_readonly() && user_writable(path);
}
int fd_regular(struct open_file* f, struct regular_view* view) {
    *view = (struct regular_view){f->memory, f->path, &f->position, f->flags};
    return f->kind == FD_MEMORY ? 2 : f->kind != FD_FILE ? 0 : f->file.is_dir ? 3 : 1;
}
int fd_resize(struct open_file* f, uint64_t size) { return file_truncate(f, size); }
uint64_t native_scratch_map(uint64_t bytes) { return (uint64_t)memory_map(0, bytes, 3, 0x22, (uint64_t)-1, 0); }
void native_scratch_unmap(uint64_t address, uint64_t bytes) { memory_unmap(address, bytes); }
int native_path_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* stat) { return resolved_stat(path, bound, follow, stat); }
bool native_path_private(const char* path) { return private_path(path); }
int64_t fd_transfer(struct open_file* f, uint64_t buffer, uint64_t count, uint64_t offset, bool positional, bool write) {
    fd_pin(f);
    int64_t result = write ? description_write(f, buffer, count, offset, positional) : description_read(f, buffer, count, offset, positional);
    fd_unpin(f); return result;
}
static int64_t directory_read(uint64_t fd, uint64_t address, uint64_t capacity) {
    struct open_file* f = file_get(fd); if (!f) return -9;
    if ((f->flags & 3) == 1 || (f->flags & 0x200000)) return -9;   /* O_PATH: no listing (Linux) */
    if (f->kind != FD_PROC && f->kind != FD_DEVICE && (f->kind != FD_FILE || !f->file.is_dir)) return -20;
    if (!capacity || capacity > 0x7FFFFFFF) return -22;
    uint64_t copied = 0;
    for (;;) {
        struct directory_item item = {0}; int result;
        if (f->kind == FD_PROC) result = proc_directory(f->proc, f->position, &item);
        else if (f->kind == FD_DEVICE) result = device_directory(f->device, f->position, &item);
        else if (f->position < 2) {
            strcpy(item.name, f->position ? ".." : "."); item.type = 4; item.next = f->position + 1;
            fat32_file_t entry = f->file;
            if (f->position) {
                char parent[FAT32_PATH_MAX]; parent_path(f->path, parent);
                if (!fat32_lookup(parent, &entry)) return copied ? (int64_t)copied : -5;
            }
            item.inode = disk_inode_id(&entry); result = 1;
        } else if (!f->file.dir_lba && f->position < 4) {
            strcpy(item.name, f->position == 2 ? "dev" : "proc"); item.type = 4; item.inode = 1; item.next = f->position + 1; result = 1;
        } else if (f->position - (f->file.dir_lba ? 2 : 4) > 0xFFFFFFFFULL) result = 0;
        else {
            /* FAT32 positions count entries; ext4 positions are byte cookies (stable while entries are unlinked) */
            fat32_file_t entry; uint64_t base = f->file.dir_lba ? 2 : 4, cookie = f->position - base;
            result = disk_read_dir_at(&f->file, &cookie, &entry);
            if (result < 0) result = -5;
            if (result > 0) {
                if (!f->file.dir_lba && (path_below(entry.name, "dev") || path_below(entry.name, "proc"))) { f->position = base + cookie; continue; }
                static const uint8_t dirent_type[6] = {8, 12, 10, 1, 2, 6}; /* regular, socket, symlink, fifo, chr, blk */
                strcpy(item.name, entry.name); item.type = entry.is_dir ? 4 : entry.native_kind < 6 ? dirent_type[entry.native_kind] : 8;
                item.next = base + cookie; item.inode = disk_inode_id(&entry);
            }
        }
        if (result <= 0) return copied ? (int64_t)copied : result;
        unsigned name_size = strlen(item.name) + 1, record_size = (19 + name_size + 7) & ~7U;
        if (record_size > capacity - copied) return copied ? (int64_t)copied : -22;
        if (address > ~0ULL - copied || !accessible(address + copied, record_size, true)) return copied ? (int64_t)copied : -14;
        uint8_t record[280] = {0};
        memcpy(record, &item.inode, 8); memcpy(record + 8, &item.next, 8);
        uint16_t length = record_size; memcpy(record + 16, &length, 2); record[18] = item.type;
        memcpy(record + 19, item.name, name_size); if (copy_to_user((void*)(address + copied), record, record_size)) return copied ? (int64_t)copied : -14;
        copied += record_size; f->position = item.next;
        if (copied == capacity) return copied;
    }
}
struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65]; };
static unsigned last_fault, last_error;
static char executable_path[FAT32_PATH_MAX];
static uint8_t ipc_seen[512];static bool trace_fresh(uint8_t* seen, uint64_t key) { if (key >= 512 || seen[key] >= 3) return false; seen[key]++; return true; }
void native_set_executable(const char* path) { strcpy(executable_path, path); }
/* blockio.c: a sleeping disk wait must resume with the kernel globals it left (exec builds the next image in a space it
 * activated itself; another task's big-lock section reloads every global meanwhile). */
void kernel_io_sleep(void* channel, uint64_t deadline) {
    struct address_space* space = active_space; struct descriptor_table* files = active_descriptors;
    struct signal_state* signals = signals_current(); struct credentials* credentials = credentials_current();
    struct filesystem_state* fs = active_filesystem; char path[FAT32_PATH_MAX];
    strcpy(path, executable_path);
    task_sleep_uninterruptible(channel, deadline);
    space_activate(space); fd_table_activate(files); signals_activate(signals); credentials_activate(credentials); filesystem_activate(fs);
    strcpy(executable_path, path);
}
unsigned user_termination_signal;
static int native_exec(struct registers* r, bool at);
struct image_source { uint64_t size; const uint8_t* memory; fat32_file_t file; const char* name; struct memory_file* orphan; };
/* A file image for exec (ETXTBSY checks; its cache is written back first). An unlinked file's data is never
 * written back (file_detach), so its headers and size are read through its cache, as read(2) would see them. */
static int image_open(struct image_source* source) {
    int error = executable_file(&source->file); if (error) return error;
    source->size = source->file.size;
    struct memory_file* cache = source->file.detached ? memory_file_disk_find(&source->file) : NULL;
    if (cache) { memory_file_retain(cache); source->orphan = cache; source->size = cache->size; }
    return 0;
}
static void image_close(struct image_source* source) {
    if (source->orphan) memory_file_release(source->orphan);
    source->orphan = NULL;
}
static bool image_read(struct image_source* source, uint64_t off, void* buffer, uint64_t count) {
    if (off > source->size || count > source->size - off) return false;
    if (source->memory) { memcpy(buffer, source->memory + off, count); return true; }
    uint8_t* output = buffer;
    while (count && source->orphan) {
        uint64_t part = PAGE - (off & (PAGE - 1)); if (part > count) part = count;
        uint64_t frame = memory_file_page(source->orphan, off / PAGE, true); if (!frame) return false;
        memcpy(output, (uint8_t*)phys_to_virt(frame) + (off & (PAGE - 1)), part);
        off += part; output += part; count -= part;
    }
    while (count) {
        uint32_t part = count < PAGE ? (uint32_t)count : PAGE;
        if (fat32_read_file(&source->file, (uint32_t)off, part, output) != (int32_t)part) return false;
        off += part; output += part; count -= part;
    }
    return true;
}
/* binfmt_script: "#!interpreter [one argument]" -> argv = interpreter, [argument], script, argv[1..].
 * Returns 1 for a non-script, 0 with a new vector (and the interpreter in `path`), or an error. */
static int script_interpreter(fat32_file_t* file, const char* requested, char*** arguments, int* argc, uint64_t* extra, char* path) {
    char line[256]; struct image_source source = {0}; source.file = *file;
    int error = image_open(&source); if (error) return error;
    *file = source.file;
    int got = source.size < sizeof(line) ? (int)source.size : (int)sizeof(line);
    if (!image_read(&source, 0, line, (uint64_t)got)) got = 0;
    image_close(&source);
    if (got < 2 || line[0] != '#' || line[1] != '!') return 1;
    int end = 2; while (end < got && line[end] != '\n') end++;
    if (end == (int)sizeof(line)) return -8;
    line[end] = 0;
    char* name = line + 2; while (*name == ' ' || *name == '\t') name++;
    char* cut = name; while (*cut && *cut != ' ' && *cut != '\t') cut++;
    char* option = *cut ? cut + 1 : cut; *cut = 0;
    while (*option == ' ' || *option == '\t') option++;
    for (char* tail = option + strlen(option); tail > option && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r'); ) *--tail = 0;
    if (!*name) return -8;
    unsigned name_size = strlen(name) + 1, option_size = *option ? strlen(option) + 1 : 0, script_size = strlen(requested) + 1;
    if (name_size > FAT32_PATH_MAX) return -36;
    int count = *argc + 1 + (option_size ? 1 : 0);
    uint64_t bytes = (uint64_t)(count + 1) * sizeof(char*) + name_size + option_size + script_size;
    char** next = (void*)arena_alloc(bytes); if (!next) return -12;
    char* cursor = (char*)(next + count + 1); int i = 0;
    next[i++] = cursor; memcpy(cursor, name, name_size); cursor += name_size;
    if (option_size) { next[i++] = cursor; memcpy(cursor, option, option_size); cursor += option_size; }
    next[i++] = cursor; memcpy(cursor, requested, script_size);
    for (int k = 1; k < *argc; k++) next[i++] = (*arguments)[k];
    next[i] = NULL; *arguments = next; *argc = count; *extra = bytes;
    strcpy(path, name); return 0;
}
static int fpu_exception_code(unsigned vector) {
    uint32_t error;
    if (vector == 16) { uint16_t status, control; __asm__ volatile("fnstsw %0; fnstcw %1" : "=m"(status), "=m"(control)); error = status & ~control; }
    else { uint32_t mxcsr; __asm__ volatile("stmxcsr %0" : "=m"(mxcsr)); error = ~(mxcsr >> 7) & mxcsr; }
    return error & 0x001 ? 7 : error & 0x004 ? 3 : error & 0x008 ? 4 : error & 0x012 ? 5 : error & 0x020 ? 6 : 0;
}
/* Diagnosis (session trace only): the last system calls of every thread, printed with a user fault of that process. */
struct syscall_note { uint32_t pid, tid; uint64_t call, a1, a2, a3, result, ns; };
static struct syscall_note syscall_ring[1024]; static uint32_t syscall_ring_next;
static void syscall_ring_add(uint64_t call, const struct registers* r) {
    uint32_t i = __atomic_fetch_add(&syscall_ring_next, 1, __ATOMIC_RELAXED) % 1024;
    syscall_ring[i] = (struct syscall_note){task_pid(), task_tid(), call, r->rdi, r->rsi, r->rdx, r->rax, native_clock_ns()};
}
static void syscall_ring_dump(uint32_t pid) {
    uint32_t end = __atomic_load_n(&syscall_ring_next, __ATOMIC_RELAXED), shown = 0;
    for (uint32_t k = end; k-- > (end > 1024 ? end - 1024 : 0) && shown < 48;) {
        const struct syscall_note* n = &syscall_ring[k % 1024];
        if (n->pid != pid) continue;
        shown++;
        say("[64] LAST_SYSCALL tid="); hex(n->tid); say(" call="); hex(n->call); say(" a1="); hex(n->a1); say(" a2="); hex(n->a2);
        say(" a3="); hex(n->a3); say(" result="); hex(n->result); say(" ms_ago="); hex((native_clock_ns() - n->ns) / 1000000); put('\n');
    }
}
static void fault_location(const char* label, uint64_t address) {
    struct vm_area* area = area_find(address);
    if (!area || !area->file_backed || !(area->prot & 4)) return;
    say(label); say(area->file.name); say(" file_offset="); hex(area->offset + address - area->start); put('\n');
}
void native_exit_trace(struct registers* r, unsigned status) {
    if (!trace_processes || (r && !status)) return;
    say(r ? "[64] USER_EXIT pid=" : "[64] PROCESS_EXIT pid="); hex(task_pid());
    say(" tid="); hex(task_tid()); say(" wait_status="); hex(status); put('\n');
    if (!r) return;
    fault_location("[64] EXIT_IMAGE ", r->rip);
    for (unsigned i = 0; i < 6 && accessible(r->rsp + i * 8, 8, false); i++)
        fault_location("[64] EXIT_STACK ", uget64(r->rsp + i * 8));
}
struct registers* native_dispatch(struct registers* r) {
    /* seccomp runs once at the top of the single syscall chokepoint (64-bit and int 0x80), a predicted branch
     * on the per-task mode for unfiltered tasks, so any new syscall is filtered automatically. */
    if (USER_MODE(r) && (r->vector == 128 || r->vector == IA32_SYSCALL_VECTOR) && task_seccomp_mode()) {
        struct registers* out; if (seccomp_enforce(r, &out)) return out;   /* errno/trap/notify/kill: fully handled */
    }
    if (r->vector == IA32_SYSCALL_VECTOR && USER_MODE(r)) return segments_int80(r);
    if (!USER_MODE(r) || r->vector != 128) {
        uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        if (r->vector == 14) {   /* blockio.c: page-in waits spin their first millisecond (latency) */
            block_io_fault(true);
            bool resolved = USER_MODE(r) ? page_resolve(cr2, (r->error & 2) != 0, (r->error & 16) != 0) :
                            !(r->error & 24) && cpu_self()->task && kernel_lock_held() && user_range(cr2, 1) && !uaccess_smap_violation(r, cr2) &&
                            spin_held(&active_space->vm_lock) && page_resolve(cr2, (r->error & 2) != 0, false);
            block_io_fault(false);
            if (resolved) return r;
        }
        if ((r->vector == 14 || r->vector == 13) && uaccess_fixup(r)) return r;   /* a user copy faulted: it returns short (uaccess.h) */
        if (USER_MODE(r) && r->vector == 13 && umip_emulate(r)) return r;   /* ring-3 SGDT/SIDT/SLDT/SMSW/STR (UMIP) */
        say(USER_MODE(r) ? "[64] USER_FAULT vector=" : "[64] KERNEL_FAULT vector=");
        hex(r->vector); say(" error="); hex(r->error); say(" rip="); hex(r->rip); say(" cr2="); hex(cr2);
        if (r->vector == 14 && uaccess_smap_violation(r, cr2)) say(" SMAP violation (user access outside uaccess.h)");
        put('\n');
        if (!USER_MODE(r)) {
            /* kguard: a recoverable kernel-mode exception caused by a task running a syscall is killed like a Linux
             * oops (SIGKILL), keeping the system alive; only an IRQ/boot context or unsafe locks leave a panic.
             * #DF is contained only when it is a kernel-stack overflow (guard-page hit); NMI (2) and #MC (18) never. */
            bool overflow = (r->vector == 8 || r->vector == 14) && task_kernel_stack_overflow(cr2, r->rsp);
            bool recoverable = r->vector == 8 ? overflow : (r->vector != 2 && r->vector != 18);
            if (recoverable && task_kguard_containable()) {
                say("[64] kguard: killed pid="); hex(task_pid()); say(" tid="); hex(task_tid());
                say(overflow ? " (kernel stack overflow)" : (r->vector == 14 && uaccess_smap_violation(r, cr2)) ? " (SMAP violation)" : " (kernel fault)");
                say(" - system alive\n"); serial_drain();
                task_kguard_kill();   /* noreturn: terminates this task and returns to the scheduler */
            }
            /* uncontained: likely return addresses on the rest of this stack page, for symbolizing kernel.elf */
            for (uint64_t at = r->rsp & ~7ULL, end = (r->rsp | 4095) + 1, shown = 0; at < end && shown < 24; at += 8) {
                uint64_t word = *(volatile uint64_t*)at;
                if (word >= 0xFFFFFFFF80100000ULL && word < 0xFFFFFFFF80800000ULL) { say("[64] KERNEL_STACK "); hex(word); put('\n'); shown++; }
            }
            panic("uncontained kernel exception");
        }
        fault_location("[64] FAULT_IMAGE ", r->rip);
        if (trace_processes) {
            say("[64] FAULT_REGS rax="); hex(r->rax); say(" rbx="); hex(r->rbx); say(" rcx="); hex(r->rcx); say(" rdx="); hex(r->rdx);
            say(" rsi="); hex(r->rsi); say(" rdi="); hex(r->rdi); say(" r12="); hex(r->r12); say(" r13="); hex(r->r13); put('\n');
            const uint64_t values[] = {r->rbx, r->rcx, r->rdi, r->rsi, r->r12, r->r13, r->rbp, r->rsp};
            const char* names[] = {"rbx", "rcx", "rdi", "rsi", "r12", "r13", "rbp", "rsp"};
            for (unsigned k = 0; k < 8; k++) {
                struct vm_area* va = area_find(values[k]); if (!va) continue;
                uint64_t* pte_slot = pte(process_root, values[k] & ~(PAGE - 1), false);
                say("[64] FAULT_AREA "); say(names[k]); say(" start="); hex(va->start); say(" end="); hex(va->end); say(" prot="); hex(va->prot);
                say(" shared="); hex(va->shared); say(" mem="); hex((uint64_t)(uintptr_t)va->memory); say(" disk="); hex(va->memory ? va->memory->disk_backed : 0);
                say(" swappable="); hex(va->memory ? va->memory->swappable : 0); say(" pte="); hex(pte_slot ? *pte_slot : 0);
                say(" path="); say(va->path); put('\n');
            }
            const uint64_t dumps[] = {r->r12, r->rdi, r->rbx};
            for (unsigned k = 0; k < 3; k++) {
                uint64_t at = dumps[k] & ~7ULL;
                if (!user_range(at, 64) || !accessible(at, 64, false)) continue;
                say("[64] FAULT_MEM "); hex(at); say(":");
                for (unsigned w = 0; w < 8; w++) { put(' '); hex(uget64(at + w * 8)); }
                put('\n');
            }
            syscall_ring_dump(task_pid());
        }
        uint64_t frame = r->rbp;
        for (unsigned depth = 0; depth < 6 && accessible(frame, 16, false); depth++) {
            uint64_t words[2]; copy_from_user(words, (const void*)frame, 16); fault_location("[64] CALLER_IMAGE ", words[1]);
            if (words[0] <= frame) break; frame = words[0];
        }
        last_fault = r->vector; last_error = r->error;
        unsigned signal = r->vector == 1 || r->vector == 3 ? 5 : r->vector == 6 ? 4 :
                          r->vector == 0 || r->vector == 16 || r->vector == 19 ? 8 :
                          r->vector == 11 || r->vector == 12 || r->vector == 17 ? 7 : 11; /* #NP/#SS/#AC: SIGBUS (Linux) */
        struct vm_area* area = r->vector == 14 ? area_find(cr2) : NULL;
        if (area && (area->file_backed || area->memory) && area->prot && (!(r->error & 2) || (area->prot & 2)) &&
            (!(r->error & 16) || (area->prot & 4))) signal = 7;
        int code = r->vector == 14 ? ((r->error & 1) ? 2 : 1) : r->vector == 3 ? 1 : r->vector == 6 ? 2 : 128;
        if (signal == 7 && r->vector == 14) code = 2;
        /* Linux si_code: #DE FPE_INTDIV, #AC BUS_ADRALN, #MF/#XM from the unmasked x87/SSE exception flags
         * (fpu__exception_code; no signal for a spurious trap). Wine maps SIGFPE by si_code. */
        if (r->vector == 0) code = 1;
        else if (r->vector == 17) code = 1;
        else if (r->vector == 16 || r->vector == 19) { code = fpu_exception_code(r->vector); if (!code) return r; }
        return signal_fault(r, signal, code, r->vector == 14 ? cr2 : r->vector == 3 ? r->rip - 1 : r->rip);
    }
    /* Linux x86-64 syscall numbers/registers; unsupported calls fail explicitly. */
    uint64_t call = r->rax;
    if (sandbox_trace && (call == 59 || call == 322)) {   /* sbxtrace: sandbox env */
        uint64_t envp = call == 59 ? r->rdx : r->r10;
        for (unsigned i = 0; i < 512 && accessible(envp + i * 8, 8, false); i++) {
            uint64_t e = uget64(envp + i * 8); if (!e) break;
            char text[FAT32_PATH_MAX]; if (user_path(e, text) == -14) break; text[63] = 0;   /* long strings: -36, prefix kept */
            if (!strncmp(text, "SBX_", 4)) { say("[64] SBXTRACE exec pid="); hex(task_pid()); say(" env="); say(text); put('\n'); }
        }
    }
    uint64_t wide[6]; uint16_t narrowed = kabi_narrow(r, wide);   /* int arguments ignore their upper halves (kabi.c) */
    bool io_free = syscall_io_free(r); task_set_io_free(io_free);   /* blockio.c: such calls run beside a sleeping disk wait */
    if (!io_free && block_io_busy()) block_io_barrier();
    int64_t landlocked = landlock_users ? landlock_check(r) : 0;   /* landlock.c: the caller's domain allows it */
    if (landlocked) r->rax = (uint64_t)landlocked;
    else switch (r->rax) {
    case 0: r->rax = file_read(r->rdi, r->rsi, r->rdx, 0, false); break;
    case 1: r->rax = file_write(r->rdi, r->rsi, r->rdx, 0, false); break;
    case 2: open_mode = (uint32_t)r->rdx; r->rax = file_open((uint64_t)-100, r->rdi, r->rsi, r->rdx); break;
    case 85: open_mode = (uint32_t)r->rsi; r->rax = file_open((uint64_t)-100, r->rdi, 0x241, r->rsi); break;
    case 3: r->rax = fd_close(r->rdi); break;
    case 7: case 23: case 213: case 232: case 233: case 270: case 271: case 281: case 284: case 290: case 291: r->rax = event_call(r); break;
    case 32: { struct open_file* f = file_get(r->rdi); r->rax = f ? fd_install(f, 0, false) : -9; break; }
    case 33: r->rax = fd_duplicate(r->rdi, r->rsi, false, true); break;
    case 292: r->rax = r->rdx & ~0x80000ULL ? -22 : fd_duplicate(r->rdi, r->rsi, r->rdx != 0, false); break;
    case 72: r->rax = descriptor_control(r->rdi, r->rsi, r->rdx); break;
    case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48: case 49: case 50:
    case 51: case 52: case 53: case 54: case 55: case 288:
        { struct open_file* f = call == 41 || call == 53 ? NULL : fd_lookup(r->rdi);
          if ((call == 41 && r->rdi == 16) || (f && netlink_descriptor(f))) { r->rax = netlink_call(r); break; } /* AF_NETLINK (netlink.c) */
          if ((call == 41 && r->rdi == 17) || (f && packet_descriptor(f))) { r->rax = packet_call(r); break; }   /* AF_PACKET (packet.c) */
          if ((call == 41 && (r->rdi == 31 || r->rdi == 38)) || (f && bluetooth_descriptor(f))) { r->rax = bluetooth_call(r); break; }   /* AF_BLUETOOTH, AF_ALG (bluetooth.c) */
          r->rax = (call == 41 && (r->rdi == 2 || r->rdi == 10)) || (f && f->kind == FD_INET) ? inet_call(r) : ipc_call(r); break; }   /* AF_INET, AF_INET6 */
    case 307: r->rax = socket_send_messages(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 299: r->rax = socket_receive_messages(r->rdi, r->rsi, r->rdx, r->r10, r->r8); break;
    case 56: r->rax = task_clone(r); break;
    case 57: case 58: r->rax = task_clone(r); break;
    case 59: case 322: r->rax = native_exec(r, call == 322); break;
    case 272: r->rax = task_unshare(r->rdi); break;
    case 308: { struct open_file* f = file_get(r->rdi); r->rax = !f ? -9 : f->kind == FD_PROC ? proc_setns(f->proc, r->rsi) : -22; break; } /* setns (namespaces.c) */
    case 61: r->rax = task_wait4((int32_t)r->rdi, r->rsi, r->rdx, r->r10); break;
    case 247: r->rax = r->rdi == 3 ? pidfd_call(r) : task_waitid(r->rdi, r->rsi, r->rdx, r->r10, r->r8); break;   /* P_PIDFD (pidfd.c) */
    case 248: case 249: case 250: { int64_t keyring_call(struct registers* registers); r->rax = keyring_call(r); break; }   /* add_key, request_key, keyctl (keyring.c) */
    case 24: task_sched_yield(); r->rax = 0; break;
    case 4: case 6: r->rax = path_stat((uint64_t)-100, r->rdi, r->rsi, call == 6 ? 0x100 : 0); break;
    case 5: r->rax = file_stat(r->rdi, r->rsi); break;
    case 217: r->rax = directory_read(r->rdi, r->rsi, r->rdx); break;
    case 8: r->rax = file_seek(r->rdi, (int64_t)r->rsi, r->rdx); break;
    case 9:
        r->rax = memory_map(r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9);
        if ((int64_t)r->rax < 0) {
            say("[64] mmap error="); hex(r->rax); say(" prot="); hex(r->rdx); say(" flags="); hex(r->r10);
            say(" fd="); hex(r->r8); say(" offset="); hex(r->r9); say(" bytes="); hex(r->rsi); say(" tid="); hex(task_tid()); put('\n');
            struct open_file* failed = file_get(r->r8);
            if (failed) { say("[64] mmap source kind="); hex(failed->kind); say(" flags="); hex(failed->flags); say(" path="); say(failed->path); put('\n'); }
        }
        break;
    case 10:
        r->rax = memory_protect(r->rdi, r->rsi, r->rdx);
        if ((int64_t)r->rax < 0) {
            say("[64] mprotect error="); hex(r->rax); say(" prot="); hex(r->rdx);
            say(" address="); hex(r->rdi); say(" bytes="); hex(r->rsi); say(" tid="); hex(task_tid()); put('\n');
        }
        break;
    case 11: r->rax = memory_unmap(r->rdi, r->rsi); break;
    case 26: r->rax = memory_sync(r->rdi, r->rsi, r->rdx); break;
    case 12: r->rax = memory_break(r->rdi); break;
    case 13: r->rax = signal_action(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 14: r->rax = signal_mask_change(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 15: return signal_return(r);
    case 16: {
        struct open_file* f = file_get(r->rdi);
        if (!f) r->rax = (uint64_t)-9;
        else if (r->rsi == 0x5421) {
            if (!accessible(r->rdx, 4, false)) r->rax = (uint64_t)-14;
            else { f->flags = (f->flags & ~0x800ULL) | (uget32(r->rdx) ? 0x800 : 0); r->rax = 0; }
        }
        else if (f->kind == FD_DEVICE) r->rax = device_ioctl(f->device, r->rsi, r->rdx, &f->input, f->flags);
        else if (f->kind == FD_OBJECT) { fd_pin(f); r->rax = f->ops->ioctl(f->object, r->rsi, r->rdx, f->flags); fd_unpin(f); }
        else if (f->kind == FD_INET && ((r->rsi & 0xFF00) == 0x8900 || (r->rsi & 0xFF00) == 0x8B00 || r->rsi == 0x5411))
            r->rax = inet_ioctl(f->inet, r->rsi, r->rdx); /* interface and route ioctls by socket family (netioctl.c), SIOCOUTQ */
        else if (f->kind == FD_SOCKET && ((r->rsi & 0xFF00) == 0x8900 || (r->rsi & 0xFF00) == 0x8B00))
            r->rax = netif_ioctl(r->rsi, r->rdx); /* SIOCGIF*: interface information (netioctl.c) */
        else if (f->kind == FD_SOCKET && r->rsi == 0x541B) {
            int64_t queued = ipc_available(f->socket);
            if (queued >= 0 && !accessible(r->rdx, 4, true)) queued = -14;
            if (queued >= 0) uput32(r->rdx, (uint32_t)queued);
            r->rax = (uint64_t)(queued < 0 ? queued : 0);
        }
        else if ((f->kind != FD_PIPE && f->kind != FD_INET) || r->rsi != 0x541B) r->rax = (uint64_t)-25;
        else if (!accessible(r->rdx, 4, true)) r->rax = (uint64_t)-14;
        else { uput32(r->rdx, (uint32_t)(f->kind == FD_INET ? inet_available(f->inet) : pipe_available(f->pipe))); r->rax = 0; }
        break;
    }
    case 17: r->rax = file_read(r->rdi, r->rsi, r->rdx, r->r10, true); break;
    case 18: r->rax = file_write(r->rdi, r->rsi, r->rdx, r->r10, true); break;
    case 162: memory_file_writeback(true, 0, ~0ULL); block_flush_all(); r->rax = 0; break;
    case 169: r->rax = platform_reboot_call(r); break;
    case 306: r->rax = file_get(r->rdi) ? (memory_file_writeback(true, 0, ~0ULL), block_flush_all(), 0) : (uint64_t)-9; break;
    case 74: case 75: {
        struct open_file* f = file_get(r->rdi);
        r->rax = f ? file_sync(f) : -9;
        if (!r->rax && f && f->kind == FD_FILE && disk_sync() < 0) r->rax = (uint64_t)-5;
        break;
    }
    case 73: {
        struct open_file* f = file_get(r->rdi);
        if (!f) r->rax = -9;
        else { fd_pin(f); r->rax = file_lock_call(f, (uint32_t)r->rdi, 0, r->rsi, true); fd_unpin(f); }
        break;
    }
    case 319: r->rax = memory_create(r->rdi, r->rsi); break;
    case 91: {
        struct open_file* f = file_get(r->rdi);
        r->rax = !f ? -9 : f->kind == FD_MEMORY ? memory_file_chmod(f->memory, r->rsi) : disk_posix() || (f->kind == FD_FILE && f->file.mount) ? vfs_fchmod(f, r->rsi) : fsext_call(r); break;
    }
    case 77: { struct open_file* f = file_get(r->rdi); r->rax = f ? file_truncate(f, r->rsi) : -9; break; }
    case 19: case 20: r->rax = file_vectors(r->rdi, r->rsi, r->rdx, call == 20); break;
    case 22: r->rax = pipe_create(r->rdi, 0); break;
    case 293: r->rax = pipe_create(r->rdi, r->rsi); break;
    case 21: {
        { struct registers a = *r; a.rax = 269; a.rdx = r->rsi; a.rsi = r->rdi; a.rdi = (uint64_t)-100; if (disk_posix() || mount_syscall_targets_mount(&a)) { r->rax = vfs_call(&a); break; } }
        char path[FAT32_PATH_MAX]; int error = user_path(r->rdi, path); fat32_file_t file;
        struct open_file* bound = NULL;
        if (!error) error = path_at_bound((uint64_t)-100, path, true, &bound);
        if (!error && bound) {
            struct linux_stat stat; error = fd_description_stat(bound, &stat);
            r->rax = error ? error : (r->rsi & ~7ULL) ? -22 : !user_readable(path) ? -13 :
                ((r->rsi & 2) && !(stat.mode & 0222)) || ((r->rsi & 1) && !(stat.mode & 0111)) ? -13 : 0;
            break;
        }
        int device = !error ? device_lookup(path) : -1;
        if (device >= 0) {
            struct linux_stat stat; device_stat((unsigned)device, &stat);
            r->rax = (r->rsi & ~7ULL) ? -22 : ((r->rsi & 2) && !(stat.mode & 0222)) || ((r->rsi & 1) && !(stat.mode & 0111)) ? -13 : 0;
            break;
        }
        if (!error && proc_path(path)) {
            struct linux_stat stat; error = proc_path_stat(path, true, &stat);
            if (!error && (r->rsi & ~7ULL)) error = -22;
            if (!error && (r->rsi & 2) && !(stat.mode & 0200)) error = -13;
            if (!error && (r->rsi & 1) && !(stat.mode & 0111)) error = -13;
            r->rax = error; break;
        }
        r->rax = error ? error : (r->rsi & ~7ULL) ? -22 : !user_readable(path) ? -13 : !fat32_lookup(path, &file) ? -2 :
                 (r->rsi & 2) && !user_writable(path) ? -30 : 0;
        break;
    }
    case 28: r->rax = memory_advise(r->rdi, r->rsi, r->rdx); break;
    case 149: case 150: case 151: case 152: case 325: r->rax = memory_lock(call, r->rdi, r->rsi, r->rdx); break;
    case 39: r->rax = task_visible_pid(task_pid()); break;
    case 110: r->rax = task_visible_pid(task_parent_pid()); break;
    case 109: case 111: case 112: case 121: case 124: r->rax = task_session(call, (int32_t)r->rdi, (int32_t)r->rsi); break;
    case 62: r->rax = signal_send((int32_t)r->rdi, r->rsi, 0); break;
    case 79: r->rax = filesystem_getcwd(r->rdi, r->rsi); break;
    case 80: case 81: case 161: r->rax = filesystem_change(call, r->rdi); break;
    case 82: r->rax = file_rename((uint64_t)-100, r->rdi, (uint64_t)-100, r->rsi, 0); break;
    case 264: case 316: r->rax = file_rename(r->rdi, r->rsi, r->rdx, r->r10, call == 316 ? r->r8 : 0); break;
    case 83: r->rax = directory_change((uint64_t)-100, r->rdi, true, r->rsi); break;
    case 84: r->rax = directory_change((uint64_t)-100, r->rdi, false, 0); break;
    case 258: r->rax = directory_change(r->rdi, r->rsi, true, r->rdx); break;
    case 87: r->rax = file_unlink((uint64_t)-100, r->rdi, 0); break;
    case 263: r->rax = file_unlink(r->rdi, r->rsi, r->rdx); break;
    case 88: r->rax = file_symlink(r->rdi, (uint64_t)-100, r->rsi); break;
    case 266: r->rax = file_symlink(r->rdi, r->rsi, r->rdx); break;
    case 89: case 267: {
        uint64_t address = call == 89 ? r->rsi : r->rdx, capacity = call == 89 ? r->rdx : r->r10;
        char path[FAT32_PATH_MAX]; int error = user_path(call == 89 ? r->rdi : r->rsi, path);
        struct open_file* bound = NULL;
        if (!error) error = path_at_bound(call == 89 ? (uint64_t)-100 : r->rdi, path, false, &bound);
        if (!error && bound) error = -22;
        if (error) r->rax = error;
        else if (!capacity) r->rax = (uint64_t)-22;
        else if (proc_path(path)) {
            char target[FAT32_PATH_MAX]; int size = proc_readlink(path, target, sizeof(target));
            if (size < 0) r->rax = size;
            else {
                uint64_t count = (uint64_t)size < capacity ? (uint64_t)size : capacity;
                if (!accessible(address, count, true)) r->rax = (uint64_t)-14;
                else { r->rax = copy_to_user((void*)address, target, count) ? (uint64_t)-14 : count; }
            }
        }
        else {
            fat32_file_t file; char target[FAT32_PATH_MAX]; int device = device_lookup(path);
            int length = device >= 0 ? device_readlink((unsigned)device, target, sizeof(target)) : fat32_lookup(path, &file) ? link_contents(&file, target) : -2;
            if (length < 0) r->rax = length;
            else {
                uint64_t count = (uint64_t)length < capacity ? (uint64_t)length : capacity;
                if (!accessible(address, count, true)) r->rax = (uint64_t)-14;
                else { r->rax = copy_to_user((void*)address, target, count) ? (uint64_t)-14 : count; }
            }
        }
        break;
    }
    case 127: r->rax = signal_pending_mask(r->rdi, r->rsi); break;
    case 115: case 116: case 125: case 126: case 157: case 105: case 106: case 113: case 114: case 117: case 119: case 122: case 123:
        r->rax = credentials_call(r); break;
    case 131: r->rax = signal_altstack(r->rdi, r->rsi, r->rsp); break;
    case 137: case 138: r->rax = filesystem_stat(r->rdi, r->rsi, call == 138); break;
    case 60: case 231:
        if (call == 231) task_exit_group(r->rdi & 255);
        user_termination_signal = 0; user_result = r->rdi & 255; return NULL;
    case 158: r->rax = segments_arch_prctl(r->rdi, r->rsi); break; /* FS/GS base + selector, CPUID mode (segments.c) */
    case 154: r->rax = segments_modify_ldt(r->rdi, r->rsi, r->rdx); break;
    case 186: r->rax = task_visible_pid(task_tid()); break;
    case 200: r->rax = task_thread_signal(0, (int32_t)r->rdi, r->rsi); break;
    case 234: r->rax = (int32_t)r->rdi <= 0 ? -22 : task_thread_signal((int32_t)r->rdi, (int32_t)r->rsi, r->rdx); break;
    case 201: {
        uint64_t seconds = native_realtime_ns() / 1000000000;
        if (r->rdi && !accessible(r->rdi, 8, true)) r->rax = (uint64_t)-14;
        else { if (r->rdi) uput64(r->rdi, seconds); r->rax = seconds; }
        break;
    }
    case 202: r->rax = task_futex(r); break;
    case 35: r->rax = clock_sleep(1, 0, r->rdi, r->rsi); break;
    case 230: r->rax = clock_sleep((uint32_t)r->rdi, (uint32_t)r->rsi, r->rdx, r->r10); break;
    case 228: case 229: {
        uint64_t value; bool resolution = r->rax == 229;
        if (!clock_ns(r->rdi, &value)) r->rax = (uint64_t)-22;
        else if (resolution && !r->rsi) r->rax = 0;
        else if (!accessible(r->rsi, 16, true)) r->rax = (uint64_t)-14;
        else {
            if (resolution) value = (uint32_t)r->rdi == 5 || (uint32_t)r->rdi == 6 ? 1000000 : (1000000000ULL + tsc_frequency - 1) / tsc_frequency;
            uint64_t ts[2] = {value / 1000000000, value % 1000000000}; r->rax = copy_to_user((void*)r->rsi, ts, 16) ? (uint64_t)-14 : 0;
        }
        break;
    }
    case 96: {
        uint64_t value;
        if ((r->rdi && !accessible(r->rdi, 16, true)) || (r->rsi && !accessible(r->rsi, 8, true))) r->rax = (uint64_t)-14;
        else {
            clock_ns(0, &value);
            if (r->rdi) { uint64_t tv[2] = {value / 1000000000, value % 1000000000 / 1000}; copy_to_user((void*)r->rdi, tv, 16); }
            if (r->rsi) uput64(r->rsi, 0);
            r->rax = 0;
        }
        break;
    }
    case 218:
        if (r->rdi && !accessible(r->rdi, 4, true)) r->rax = (uint64_t)-14;
        else { task_set_clear_tid(r->rdi); r->rax = task_visible_pid(task_tid()); }
        break;
    case 97: r->rax = r->rsi ? task_resource_limit(0, (uint32_t)r->rdi, 0, r->rsi) : -14; break;
    case 167: case 168: r->rax = (uint64_t)swap_call(r); break;   /* swapon, swapoff (swap.c) */
    case 99: { /* sysinfo */
        struct { int64_t uptime; uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
                 uint16_t procs, pad; uint32_t pad2; uint64_t totalhigh, freehigh; uint32_t mem_unit; uint32_t pad3; } info;
        _Static_assert(sizeof(info) == 112, "struct sysinfo ABI");
        if (!accessible(r->rdi, sizeof(info), true)) { r->rax = (uint64_t)-14; break; }
        memset(&info, 0, sizeof(info)); uint64_t cached;
        native_memory_info(&info.totalram, &info.freeram, &cached); info.bufferram = 0; info.sharedram = memory_file_shmem_pages() * PAGE;
        { uint64_t swap_total, swap_free, swap_cached; swap_counts(&swap_total, &swap_free, &swap_cached); info.totalswap = swap_total * PAGE; info.freeswap = swap_free * PAGE; }
        info.uptime = (int64_t)(native_clock_ns() / 1000000000ULL); info.procs = (uint16_t)task_count(); info.mem_unit = 1;
        r->rax = copy_to_user((void*)r->rdi, &info, sizeof(info)) ? (uint64_t)-14 : 0; break;
    }
    case 160: r->rax = r->rsi ? task_resource_limit(0, (uint32_t)r->rdi, r->rsi, 0) : -14; break;
    case 302: r->rax = task_resource_limit((uint32_t)r->rdi, (uint32_t)r->rsi, r->rdx, r->r10); break;
    case 318:
        if (r->rdx & ~3ULL) r->rax = (uint64_t)-22;
        else if (!r->rsi) r->rax = 0;
        else if (!accessible(r->rdi, r->rsi, true)) r->rax = (uint64_t)-14;
        else r->rax = (uint64_t)native_random_user(r->rdi, r->rsi);
        break;
    case 102: case 107: r->rax = credentials_id(credentials_current(), false, call == 102 ? 0 : 1); break;
    case 104: case 108: r->rax = credentials_id(credentials_current(), true, call == 104 ? 0 : 1); break;
    case 118: case 120:
        if (!accessible(r->rdi, 4, true) || !accessible(r->rsi, 4, true) || !accessible(r->rdx, 4, true)) r->rax = (uint64_t)-14;
        else {
            uput32(r->rdi, credentials_id(credentials_current(), call == 120, 0));
            uput32(r->rsi, credentials_id(credentials_current(), call == 120, 1));
            uput32(r->rdx, credentials_id(credentials_current(), call == 120, 2)); r->rax = 0;
        }
        break;
    case 257: open_mode = (uint32_t)r->r10; r->rax = file_open(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 95: case 132: case 133: case 235: case 259: case 261: case 280:
        r->rax = vfs_call(r); break;
    case 155: case 165: case 166: r->rax = mount_call(r); break;   /* pivot_root, mount, umount2 (mount.c) */
    case 262: r->rax = path_stat(r->rdi, r->rsi, r->rdx, r->r10); break;
    case 63: {
        if (!accessible(r->rdi, sizeof(struct utsname), true)) { r->rax = (uint64_t)-14; break; }
        /* Linux ABI level: programs parse uname (chrony refuses "0.x"); the version names NexusOS (procext.c agrees) */
        struct utsname info = {"Linux", "", "6.12.0-nexusos-amd64", "#1 SMP PREEMPT_DYNAMIC NexusOS", "x86_64", ""};
        strcpy(info.nodename, uts_hostname()); strcpy(info.domain, uts_domainname()); /* sethostname (uts.c) */
        r->rax = copy_to_user((void*)r->rdi, &info, sizeof(info)) ? (uint64_t)-14 : 0; break;
    }
    case 140: case 141: case 142: case 143: case 144: case 145: case 146: case 147: case 203: case 204:
        r->rax = task_sched_call(r); break;
    case 273: r->rax = 0; break; /* set_robust_list: owner-death recovery is not tracked */
    case 98: r->rax = task_rusage((int32_t)r->rdi, r->rsi); break;
    case 34: case 130: { /* pause, rt_sigsuspend: sleep until a signal is delivered */
        int error = call == 130 ? signals_wait_mask(r->rdi, r->rsi) : 0;
        if (!error && call == 130 && !r->rdi) error = -14;
        while (!error) error = task_wait_channel(signals_current());
        r->rax = error; break;
    }
    case 25: r->rax = memory_remap(r->rdi, r->rsi, r->rdx, r->r10, r->r8); break;
    /* Both roots: ext4 gets POSIX semantics (vfs.c), FAT gets Linux vfat semantics (fsext.c). */
    case 76: case 86: case 90: case 92: case 93: case 94: case 260: case 265: case 268: case 269: case 285: case 332: case 439: case 452:
        r->rax = disk_posix() || mount_syscall_targets_mount(r) ? vfs_call(r) : fsext_call(r); break;
    case 187: case 221: case 40: case 303: case 326:
    case 188: case 189: case 190: case 191: case 192: case 193: case 194: case 195: case 196: case 197: case 198: case 199: /* xattrs */
        r->rax = fsext_call(r); break;
    case 100: r->rax = task_times(r->rdi); break;
    case 253: case 254: case 255: case 294: r->rax = inotify_call(r); break;
    case 27: r->rax = memory_core(r->rdi, r->rsi, r->rdx); break;
    case 101: r->rax = task_ptrace(r->rdi, (int64_t)(int32_t)r->rsi, r->rdx, r->r10); break; /* ptrace subset (process.c) */
    case 317: r->rax = seccomp_syscall(r); break; /* seccomp(2): strict/filter/notify (seccomp.c) */
    case 0x6B01: { extern int64_t kguard_probe_smap(uint64_t); r->rax = (uint64_t)kguard_probe_smap(r->rdi); break; } /* kguard.c: stray SMAP read (test only) */
    case 0x6B02: { extern int64_t kguard_probe_overflow(void); r->rax = (uint64_t)kguard_probe_overflow(); break; }   /* kguard.c: kernel-stack overflow (test only) */
    case 135: r->rax = task_personality_call(r->rdi); break; /* personality(2): ADDR_NO_RANDOMIZE etc. (process.c) */
    case 36: case 37: case 38: r->rax = task_itimer(call, r->rdi, r->rsi, r->rdx); break;     /* getitimer, alarm, setitimer */
    case 103: r->rax = kmsg_syslog(r->rdi, r->rsi, r->rdx); break;                           /* syslog(2) (kmsg.c) */
    case 170: case 171: r->rax = uts_call(r); break;                                          /* sethostname, setdomainname */
    case 436: r->rax = fd_close_range((uint32_t)r->rdi, (uint32_t)r->rsi, (uint32_t)r->rdx); break;   /* close_range */
    case 277: { /* sync_file_range: any requested write/wait syncs the file's data (a superset of the range) */
        struct open_file* f = file_get(r->rdi);
        if (!f) r->rax = (uint64_t)-9;
        else if ((r->r10 & ~7ULL) || (int64_t)r->rsi < 0 || (int64_t)r->rdx < 0 || r->rsi + r->rdx < r->rsi) r->rax = (uint64_t)-22;
        else if (f->kind != FD_FILE) r->rax = (uint64_t)(f->kind == FD_PIPE || fifo_description(f) ? -29 : -22);
        else r->rax = r->r10 ? (uint64_t)(int64_t)file_sync(f) : 0;
        break;
    }
    case 159: case 164: case 227: case 305: r->rax = timekeeping_call(r); break;              /* adjtimex, settimeofday, clock_settime, clock_adjtime */
    case 222: case 223: case 224: case 225: case 226: case 283: case 286: case 287: r->rax = posix_timer_call(r); break;   /* timer_*, timerfd_* (posix_timers.c) */
    case 128: case 129: case 282: case 289: case 297: r->rax = signalfd_syscall(r); break;   /* rt_sigtimedwait, rt_sig(tg)queueinfo, signalfd(4) (signalfd.c) */
    case 424: case 434: case 435: case 438: r->rax = pidfd_call(r); break;   /* pidfd_send_signal, pidfd_open, clone3, pidfd_getfd (pidfd.c) */
    case 444: case 445: case 446: r->rax = landlock_call(r); break;   /* Landlock (landlock.c) */
    case 428: case 429: case 430: case 431: case 432: case 433: case 442: r->rax = mountapi_call(r); break;   /* new mount API (mountapi.c) */
    case 29: case 30: case 31: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 220:   /* System V IPC */
        r->rax = sysvipc_call(r); break;
    case 295: case 296: case 327: case 328:   /* preadv, pwritev, preadv2, pwritev2 (kabi.c) */
    case 179: case 237: case 238: case 239: case 251: case 252: case 309: case 330: case 331: case 334: case 441: case 443:   /* kabi.c */
        r->rax = kabi_call(r); break;   /* quotactl, mbind, set/get_mempolicy, ioprio_set/get, getcpu, pkey_alloc/free, rseq, quotactl_fd */
    case 329: r->rax = (int32_t)r->r10 == -1 || !(int32_t)r->r10 ? (uint64_t)(int64_t)memory_protect(r->rdi, r->rsi, r->rdx) : (uint64_t)-22; break;   /* pkey_mprotect: keys -1/0 only (kabi.c) */
    default: {
        static uint8_t enosys_seen[512];
        if (!trace_fresh(enosys_seen, r->rax)) { r->rax = (uint64_t)-38; break; }
        say("[64] ENOSYS syscall="); hex(r->rax);
        if (trace_processes) { say(" pid="); hex(task_pid()); say(" tid="); hex(task_tid()); }
        if (trace_processes && call == 25) {
            say(" old="); hex(r->rdi); say(" old_size="); hex(r->rsi); say(" new_size="); hex(r->rdx);
            say(" flags="); hex(r->r10); say(" new="); hex(r->r8);
        }
        put('\n'); r->rax = (uint64_t)-38; break;
    }
    }
    if (narrowed) kabi_restore(r, narrowed, wide);   /* the caller's registers, as the syscall ABI preserves them */
    if (sandbox_trace) {   /* sbxtrace: namespace probes and calls */
        uint64_t address = call == 21 || call == 4 || call == 6 || call == 2 ? r->rdi :
                           call == 269 || call == 439 || call == 262 || call == 332 || call == 257 || call == 267 ? r->rsi : 0;
        char text[FAT32_PATH_MAX];
        if (address && !user_path(address, text) && (strstr(text, "/ns") || strstr(text, "/fd/"))) {
            say("[64] SBXTRACE call="); hex(call); say(" pid="); hex(task_pid()); say(" path="); say(text);
            say(" result="); hex(r->rax); put('\n');
        }
        if ((call == 56 && (r->rdi & 0x7C000000)) || call == 272 || call == 308) {
            say("[64] SBXTRACE call="); hex(call); say(" pid="); hex(task_pid()); say(" flags="); hex(call == 308 ? r->rsi : r->rdi);
            say(" result="); hex(r->rax); put('\n');
        }
    }
    static unsigned enomem_lines;   /* memory-pressure diagnosis: fork/clone failures and the first ENOMEM results */
    if (trace_processes && (((call == 56 || call == 57 || call == 58) && (int64_t)r->rax < 0 && (int64_t)r->rax != -4) ||
                            ((int64_t)r->rax == -12 && enomem_lines < 64))) {
        enomem_lines++;
        say("[64] SYSCALL_ENOMEM call="); hex(call); say(" pid="); hex(task_pid()); say(" tid="); hex(task_tid()); say(" result="); hex(r->rax);
        say(" arg1="); hex(r->rdi); say(" arg2="); hex(r->rsi); say(" free_pages="); hex(memory_free_pages()); put('\n');
    }
    if (trace_processes && (int64_t)r->rax < 0 && (int64_t)r->rax != -11 && (int64_t)r->rax != -4 && (int64_t)r->rax != -2 &&
        (call == 7 || call == 16 || call == 72 || (call >= 41 && call <= 55) || call == 232 || call == 233 || call == 271 || call == 281 || call == 288 ||
         (call == 202 && (int64_t)r->rax != -110)) && trace_fresh(ipc_seen, call)) {
        say("[64] IPC_API_ERROR call="); hex(call); say(" pid="); hex(task_pid()); say(" tid="); hex(task_tid());
        say(" result="); hex(r->rax); say(" arg1="); hex(r->rdi); say(" arg2="); hex(r->rsi); say(" arg3="); hex(r->rdx); put('\n');
        fault_location("[64] IPC_ERROR_IMAGE ", r->rip);
    }
    if (trace_processes) syscall_ring_add(call, r);
    return native_syscall_finish(r, call);
}
/* The common syscall exit: restart after a SA_RESTART handler, then deliver signals (BKL held). */
struct registers* native_syscall_finish(struct registers* r, uint64_t call) {
    if (!user_range(r->rip, 1) || !user_range(r->rsp, 1)) { user_termination_signal = 11; user_result = 139; return NULL; }
    r->flags &= ~((3ULL << 12) | (1ULL << 14) | (1ULL << 18));
    if ((int64_t)r->rax == -4 && (call == 0 || call == 1 || call == 16 || call == 19 || call == 20 || (call >= 42 && call <= 47) || call == 61 || call == 247 || call == 288 || call == 307 ||
        (call == 202 && !r->r10) || call == 73 || (call == 72 && (r->rsi == 7 || r->rsi == 38))) && signals_should_restart()) {
        r->rax = call; r->rip -= 2;
    } else if ((int64_t)r->rax == -4 && (call == 7 || call == 23 || call == 34 || call == 130 || call == 270 || call == 271) && signals_stop_pending()) {
        r->rax = call; r->rip -= 2;
    }
    return signal_dispatch(r);
}

struct elf_header {
    uint8_t ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phsize, phnum, shsize, shnum, shstr;
};
struct program_header { uint32_t type, flags; uint64_t offset, va, pa, filesz, memsz, align; };
_Static_assert(sizeof(struct elf_header) == 64 && sizeof(struct program_header) == 56, "ELF64 layout");
_Static_assert(sizeof(struct registers) == 176 && sizeof(struct tss64) == 104, "CPU frame layout");
static bool elf_headers(struct image_source* source, struct elf_header* h, struct program_header** headers, uint64_t bias) {
    *headers = NULL;
    uint64_t n = source->size;
    if (!image_read(source, 0, h, sizeof(*h))) return false;
    if (h->ident[0] != 127 || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F' ||
        h->ident[4] != 2 || h->ident[5] != 1 || h->ident[6] != 1 || h->version != 1 ||
        (h->type != 2 && h->type != 3) || h->machine != 62 || h->ehsize != 64 || h->phsize != 56 || !h->phnum ||
        h->phoff > n || h->phnum > (n - h->phoff) / 56) return false;
    if (h->type == 2) bias = 0;
    if (h->entry > ~0ULL - bias || !user_range(h->entry + bias, 1)) return false;
    h->entry += bias;
    uint64_t phbytes = (uint64_t)h->phnum * sizeof(struct program_header);
    struct program_header* ph = (void*)arena_alloc(phbytes);
    *headers = ph;
    if (!ph || !image_read(source, h->phoff, ph, phbytes)) return false;
    bool entry_ok = false; unsigned interpreters = 0;
    for (unsigned i = 0; i < h->phnum; i++) {
        struct program_header* p = &ph[i];
        if (p->type == 3 && (++interpreters > 1 || p->filesz < 2 || p->filesz > FAT32_PATH_MAX ||
                            p->offset > n || p->filesz > n - p->offset)) return false;
        if (p->type == 0x6474E551 && (p->flags & 1)) return false;
        if (p->type != 1) continue;
        if (p->va > ~0ULL - bias) return false;
        p->va += bias;
        if (p->filesz > p->memsz || p->offset > n || p->filesz > n - p->offset ||
            !user_range(p->va, p->memsz) || (p->flags & 3) == 3 ||
            ((p->va - p->offset) & (PAGE - 1)) ||
            (p->align > 1 && ((p->align & (p->align - 1)) || ((p->va - p->offset) & (p->align - 1))))) return false;
        if (!p->memsz) continue;
        uint64_t start = p->va & ~(PAGE - 1), end = (p->va + p->memsz + PAGE - 1) & ~(PAGE - 1);
        if (start < STACK_TOP && end > STACK_TOP - STACK_SIZE - PAGE) return false;
        for (unsigned j = 0; j < i; j++) {
            if (ph[j].type != 1 || !ph[j].memsz) continue;
            uint64_t other_start = ph[j].va & ~(PAGE - 1);
            uint64_t other_end = (ph[j].va + ph[j].memsz + PAGE - 1) & ~(PAGE - 1);
            if (start < other_end && end > other_start) return false;
        }
        if ((p->flags & 1) && h->entry >= p->va && h->entry - p->va < p->filesz) entry_ok = true;
    }
    *headers = ph;
    return entry_ok;
}
static bool elf_valid(const uint8_t* data, uint64_t size) {
    struct image_source source = {.size = size, .memory = data};
    struct elf_header header; struct program_header* ph;
    uint64_t checkpoint = memory_used_pages();
    bool valid = elf_headers(&source, &header, &ph, 0x10000000000ULL);
    if (ph) arena_free((uint64_t)ph, (uint64_t)header.phnum * sizeof(*ph));
    if (memory_used_pages() != checkpoint) panic("ELF validation allocation leak");
    return valid;
}
static int load_segments(uint64_t root, struct image_source* source, const struct elf_header* h,
                          struct program_header* ph, uint64_t* high, uint64_t* phdr) {
    *high = 0; *phdr = 0;
    for (unsigned i = 0; i < h->phnum; i++) {
        if (ph[i].type != 1 || !ph[i].memsz) continue;
        uint64_t flags = (ph[i].flags & 2 ? WRITE : 0) | (ph[i].flags & 1 ? 0 : NX);
        unsigned prot = (ph[i].flags & 4 ? 1 : 0) | (ph[i].flags & 2 ? 2 : 0) | (ph[i].flags & 1 ? 4 : 0);
        uint64_t end = (ph[i].va + ph[i].memsz + PAGE - 1) & ~(PAGE - 1);
        if (end > *high) *high = end;
        if (h->phoff >= ph[i].offset && h->phoff - ph[i].offset <= ph[i].filesz &&
            (uint64_t)h->phnum * h->phsize <= ph[i].filesz - (h->phoff - ph[i].offset))
            *phdr = ph[i].va + h->phoff - ph[i].offset;
        if (!area_add(ph[i].va & ~(PAGE - 1), end, prot, false)) return -12;
        struct vm_area* area = area_find(ph[i].va);
        area->image_segment = true; area->file_backed = source->memory == NULL;
        if (source->name && strlen(source->name) < sizeof(area->path)) strcpy(area->path, source->name);
        area->file = source->file; area->image_memory = source->memory;
        if (area->file_backed) area->file.native_ino = disk_inode_id(&source->file);
        if (area->file_backed) {
            area->memory = memory_file_disk_get(&area->file);
            if (!area->memory) return -12;
            area->memory->exec_maps++; area->memory->maps++;
        }
        area->image_begin = ph[i].offset; area->image_end = ph[i].offset + ph[i].filesz;
        area->offset = ph[i].offset & ~(PAGE - 1);
        if (!source->memory) continue;
        for (uint64_t va = ph[i].va & ~(PAGE - 1); va < end; va += PAGE)
            if (!map_page(root, va, flags)) return -12;
        for (uint64_t copied = 0; copied < ph[i].filesz;) {
            uint64_t va = ph[i].va + copied, offset = va & (PAGE - 1);
            uint64_t* slot = pte(root, va, false);
            uint64_t part = PAGE - offset;
            if (part > ph[i].filesz - copied) part = ph[i].filesz - copied;
            if (!slot || !image_read(source, ph[i].offset + copied, (uint8_t*)phys_to_virt(*slot & ADDRESS) + offset, part)) return -8;
            copied += part;
        }
    }
    return 0;
}
static bool stack_bytes(uint64_t root, uint64_t* cursor, const void* source, uint64_t count, uint64_t* address) {
    if (count > *cursor - (STACK_TOP - STACK_SIZE)) return false;
    *cursor -= count; *address = *cursor;
    return copy_into(root, *cursor, source, count);
}
struct loaded_image { struct address_space* space; uint64_t entry, stack; };
static int prepare_image(struct image_source* source, int argc, const char* const* argv,
                         int envc, const char* const* environment, const char* executable, struct loaded_image* loaded) {
    if (argc < 1 || envc < 0 || (uint64_t)argc + envc > STACK_SIZE / 16) return -7;
    if (!source->memory) {
        int error = image_open(source); if (error) return error;
    }
    struct address_space* saved_space = active_space;
    struct address_space* space = space_create(); if (!space) { image_close(source); return -12; }
    space_activate(space);
    /* Linux would_dump: an image the caller may execute but not read leaves the process non-dumpable (no ptrace
     * or /proc/PID/mem reads of it by the same user). */
    if (!source->memory && disk_permission(&source->file, DISK_MAY_READ)) space->dumpable = false;
    struct image_source interpreter = {0};
    struct elf_header header = {0}; struct program_header *ph = NULL, *ip = NULL;
    uint64_t root = space->root, ip_bytes = 0, words_size = 0;
    uint64_t *argument_addresses = NULL, *words = NULL;
    int result = -8;
    uint64_t seed[4] = {0};
    if (!image_read(source, 0, &header, sizeof(header))) goto bad;
    bool randomized = native_random(seed, sizeof(seed));
    if (header.type == 3 && !randomized) { say("[64] Dynamic ELF requires secure entropy\n"); goto bad; }
    /* ASLR: honour /proc/sys/kernel/randomize_va_space (0 off, 1 stack/mmap/vdso, 2 + brk/data) and
     * personality(ADDR_NO_RANDOMIZE). AT_RANDOM is always seeded from the secure RNG. PIE/interpreter bases are
     * 2 MiB-granular because modern PIE segments carry p_align 0x200000; mmap/stack/brk are page-granular. */
    uint64_t extra = 0; native_random(&extra, sizeof(extra));
    int aslr_level = native_randomize_va_space;
    bool aslr = randomized && aslr_level != 0 && !(task_personality() & 0x0040000 /*ADDR_NO_RANDOMIZE*/);
    uint64_t app_bias = 0x10000000000ULL + (aslr ? (seed[0] & 0xFFFFFULL) << 21 : 0);      /* 20 bits, 2 MiB */
    uint64_t interp_bias = 0x40000000000ULL + (aslr ? (seed[1] & 0xFFFFFULL) << 21 : 0);   /* 20 bits, 2 MiB */
    mmap_base = 0x200000000000ULL + (aslr ? (seed[2] & 0xFFFFFFFULL) << 12 : 0);           /* 28 bits, 4 KiB */
    if (!elf_headers(source, &header, &ph, app_bias)) goto bad;
    const struct elf_header* h = &header;
    uint64_t high, phdr;
    int load_error = load_segments(root, source, h, ph, &high, &phdr);
    if (load_error) { result = load_error; goto bad; }
    /* brk (heap/data) randomised only at level 2, like Linux; up to 32 MiB above the image, page-granular. */
    uint64_t brk_gap = (aslr && aslr_level >= 2) ? ((extra >> 20) & 0x1FFFULL) << 12 : 0;
    program_break = break_floor = high + brk_gap;
    uint64_t entry = h->entry, interpreter_base = 0;
    for (unsigned i = 0; i < h->phnum; i++) if (ph[i].type == 3) {
        if (!randomized || !phdr) goto bad;
        char path[FAT32_PATH_MAX];
        if (!image_read(source, ph[i].offset, path, ph[i].filesz) || path[0] != '/' ||
            path[ph[i].filesz - 1] || strlen(path) != ph[i].filesz - 1) goto bad;
        interpreter.name = path;
        if (canonical_path(path) || !user_readable(path) || !fat32_lookup(path, &interpreter.file) || interpreter.file.is_dir || interpreter.file.native_kind) goto bad;
        int interpreter_error = image_open(&interpreter);
        if (interpreter_error) { result = interpreter_error; goto bad; }
        if (disk_permission(&interpreter.file, DISK_MAY_READ)) space->dumpable = false;
        struct elf_header ih;
        bool valid = elf_headers(&interpreter, &ih, &ip, interp_bias);
        ip_bytes = ip ? (uint64_t)ih.phnum * sizeof(*ip) : 0;
        if (!valid) goto bad;
        for (unsigned j = 0; j < ih.phnum; j++) if (ip[j].type == 3) goto bad;
        uint64_t ihigh, iph;
        load_error = load_segments(root, &interpreter, &ih, ip, &ihigh, &iph);
        if (load_error) { result = load_error; goto bad; }
        entry = ih.entry; interpreter_base = ih.type == 3 ? interp_bias : 0;
        say("[64] INTERPRETER "); say(path); say(" entry="); hex(entry); put('\n');
        arena_free((uint64_t)ip, ip_bytes); ip = NULL;
    }
    if (!area_add(STACK_TOP - STACK_SIZE, STACK_TOP, 3, true)) { result = -12; goto bad; }
    for (uint64_t va = STACK_TOP - STACK_SIZE; va < STACK_TOP; va += PAGE)
        if (!map_page(root, va, WRITE | NX)) goto bad;
    uint64_t vdso_address = 0;
    if (vdso_file && area_add(VDSO_ADDRESS, VDSO_ADDRESS + vdso_bytes, 1 | 4, false)) {
        struct vm_area* v = area_find(VDSO_ADDRESS);
        v->memory = vdso_file; memory_file_retain(vdso_file); vdso_file->maps++; v->shared = true; v->may_write = false; v->offset = 0;
        vdso_address = VDSO_ADDRESS;
    }
    /* Stack top random gap (bounded by the 8 MiB stack VMA): the initial SP, argv/env/auxv all shift down. */
    /* Linux's layout, ascending: argv strings, envp strings, the AT_EXECFN string, 8 zero bytes at the top. Programs that
     * rewrite their process title (avahi, Chrome) take argv + envp as one contiguous area with memory after it. */
    uint64_t cursor = STACK_TOP - (aslr ? ((extra & 0xFFULL) << 12) : 0) - 8, random_address, platform, executable_address;
    argument_addresses = (void*)arena_alloc(((uint64_t)argc + envc) * 8);
    if (!argument_addresses) goto bad;
    if (!stack_bytes(root, &cursor, executable, strlen(executable) + 1, &executable_address)) goto bad;
    for (int i = envc - 1; i >= 0; i--) /* ascending in memory, like argv: /proc/pid/environ reads in order */
        if (!stack_bytes(root, &cursor, environment[i], strlen(environment[i]) + 1, &argument_addresses[argc + i])) goto bad;
    for (int i = argc - 1; i >= 0; i--)
        if (!stack_bytes(root, &cursor, argv[i], strlen(argv[i]) + 1, &argument_addresses[i])) goto bad;
    space->arg_start = argument_addresses[0]; space->arg_end = envc ? argument_addresses[argc] : executable_address;
    space->env_start = envc ? argument_addresses[argc] : executable_address; space->env_end = executable_address;
    if (!stack_bytes(root, &cursor, seed, 16, &random_address) || !stack_bytes(root, &cursor, "x86_64", 7, &platform)) goto bad;
    uint32_t ca, cb, cc, cd; __asm__ volatile("cpuid" : "=a"(ca), "=b"(cb), "=c"(cc), "=d"(cd) : "a"(1), "c"(0));
    uint64_t aux[] = {3, phdr, 4, 56, 5, h->phnum, 6, PAGE, 7, interpreter_base, 8, 0, 9, h->entry,
                     11, credentials_id(credentials_current(), false, 0), 12, credentials_id(credentials_current(), false, 1),
                     13, credentials_id(credentials_current(), true, 0), 14, credentials_id(credentials_current(), true, 1),
                     15, platform, 16, cd, 17, 100, 23, credentials_exec_secure(!source->memory && disk_exec_secure(&source->file)),   /* AT_SECURE: ld.so ignores LD_* */
                     25, randomized ? random_address : 0, 26, 0, 31, executable_address,
                     vdso_address ? 33 : 0, vdso_address, 0, 0};
    words_size = ((uint64_t)argc + envc + 3) * 8 + sizeof(aux);
    if (words_size + 16 > cursor - (STACK_TOP - STACK_SIZE)) goto bad;
    words = (void*)arena_alloc(words_size); if (!words) goto bad;
    uint64_t wi = 0; words[wi++] = argc;
    for (int i = 0; i < argc; i++) words[wi++] = argument_addresses[i];
    words[wi++] = 0;
    for (int i = 0; i < envc; i++) words[wi++] = argument_addresses[argc + i];
    words[wi++] = 0; memcpy(words + wi, aux, sizeof(aux));
    uint64_t sp = (cursor - words_size) & ~15ULL;
    if (!copy_into(root, sp, words, words_size)) goto bad;
    *loaded = (struct loaded_image){space, entry, sp}; result = 0; space = NULL;
bad:
    space_activate(saved_space);
    if (space) space_release(space);
    image_close(&interpreter); image_close(source);
    if (ph) arena_free((uint64_t)ph, (uint64_t)header.phnum * sizeof(*ph));
    if (ip) arena_free((uint64_t)ip, ip_bytes);
    arena_free((uint64_t)argument_addresses, ((uint64_t)argc + envc) * 8);
    arena_free((uint64_t)words, words_size);
    return result;
}
uint64_t storage_background_pages(void);   /* storage_alloc.c */
uint64_t swap_kernel_pages(void);   /* swap.c: active areas' metadata (a session may swapoff/swapon fstab areas) */
static int run_image(struct image_source* source, int argc, const char* const* argv) {
    uint64_t network_pages = native_network_background_pages(), storage_pages = storage_background_pages() + swap_kernel_pages();
    uint64_t checkpoint = memory_used_pages() - memory_file_disk_pages() - disk_inode_pages() - network_pages - storage_pages;
    const char* environment[] = {"HOME=/home/nexus", "USER=nexus", "LANG=C.UTF-8", "PATH=/bin:/usr/bin:/opt/google/chrome"};
    struct loaded_image loaded = {0}; int result = -1;
    if (!prepare_image(source, argc, argv, 4, environment, executable_path, &loaded)) {
        struct descriptor_table* descriptors = fd_table_new(true);
        struct signal_state* signal_context = signals_create(NULL, false);
        if (descriptors && signal_context) {
            last_fault = last_error = user_termination_signal = 0; user_result = 255;
            result = task_run(loaded.entry, loaded.stack, loaded.space, descriptors, signal_context, executable_path);
        } else {
            if (descriptors) fd_table_release(descriptors);
            if (signal_context) signals_destroy(signal_context);
            space_release(loaded.space);
        }
    }
    network_pages = native_network_background_pages(); storage_pages = storage_background_pages() + swap_kernel_pages();
    if (memory_used_pages() - memory_file_disk_pages() - disk_inode_pages() - network_pages - storage_pages != checkpoint) panic("process allocation ownership leak");
    return result;
}
#define ARGUMENT_BUDGET (STACK_SIZE / 4)
static int user_string_size(uint64_t address, uint64_t* size) {
    for (uint64_t count = 0; count < 32 * PAGE;) {
        if (address > ~0ULL - count) return -14;
        uint64_t at = address + count, bytes = PAGE - (at & (PAGE - 1));
        if (bytes > 32 * PAGE - count) bytes = 32 * PAGE - count;
        if (!accessible(at, bytes, false)) return -14;
        uint64_t i = 0; uaccess_begin(); while (i < bytes && ((const char*)at)[i]) i++; uaccess_end();   /* validated, BKL: stays mapped */
        if (i < bytes) { *size = count + i + 1; return 0; }
        count += bytes;
    }
    return -7;
}
static int argument_sizes(uint64_t vector, int* count, uint64_t* bytes) {
    *count = 0; if (!vector) return 0;
    for (;;) {
        uint64_t offset = (uint64_t)*count * 8;
        if (vector > ~0ULL - offset || !accessible(vector + offset, 8, false)) return -14;
        uint64_t address = uget64(vector + offset); if (!address) return 0;
        uint64_t length; int error = user_string_size(address, &length); if (error) return error;
        if (*bytes > ARGUMENT_BUDGET - 8 || length > ARGUMENT_BUDGET - *bytes - 8) return -7;
        *bytes += length + 8; (*count)++;
    }
}
/* Real, effective and filesystem user then group IDs. Linux begin_new_exec/commit_creds: an exec by a caller whose
 * effective IDs differ from its real ones, or one that changes effective/filesystem IDs (set-ID), is not dumpable. */
static void exec_ids(uint32_t ids[6]) {
    struct credentials* c = credentials_current();
    for (unsigned i = 0; i < 6; i++) ids[i] = credentials_kernel_id(c, i >= 3, i % 3 == 2 ? 3 : i % 3);
}
static int native_exec(struct registers* r, bool at) {
    uint64_t argv = at ? r->rdx : r->rsi, envp = at ? r->r10 : r->rdx;
    uint32_t flags = at ? (uint32_t)r->r8 : 0;
    if (flags & ~0x1100u) return -22;
    char path[FAT32_PATH_MAX]; int error = user_path(at ? r->rsi : r->rdi, path); if (error) return error;
    char requested[FAT32_PATH_MAX]; strcpy(requested, path);
    struct open_file* bound = NULL;
    if (at && !*path) {
        if (!(flags & 0x1000)) return -2;
        if ((int32_t)r->rdi == -100) return -13;
        bound = file_get(r->rdi); if (!bound) return -9;
        strcpy(path, fd_internal_path(bound));
        char number[12]; uint_to_str((uint32_t)r->rdi, number); strcpy(requested, "/dev/fd/"); strcat(requested, number);
    } else if ((error = path_at_bound(at ? r->rdi : (uint64_t)-100, path, !(flags & 0x100), &bound))) return error;
    if (!user_readable(path)) return -13;
    struct image_source source = {0}; source.name = path;
    if (flags & 0x100) {
        struct linux_stat stat; if ((error = resolved_stat(path, bound, false, &stat))) return error;
        if ((stat.mode & 0170000) == 0120000) return -40;
    }
    if (bound) {
        if (bound->kind != FD_FILE) return -13;
        source.file = bound->file;
    } else if (!fat32_lookup(path, &source.file)) return -2;
    if (source.file.is_dir || source.file.native_kind) return -13;
    if ((error = disk_exec_access(&source.file))) return error;
    source.size = source.file.size;
    int argc, envc; uint64_t bytes = 32;
    if ((error = argument_sizes(argv, &argc, &bytes)) || (error = argument_sizes(envp, &envc, &bytes))) return error;
    int actual_argc = argc ? argc : 1;
    char** arguments = (void*)arena_alloc(bytes); if (!arguments) return -12;
    char** block = arguments; char** script_vectors[5] = {0}; uint64_t script_bytes[5] = {0};
    char** environment = arguments + actual_argc + 1;
    char* cursor = (char*)(environment + envc + 1);
    if (bound) fd_pin(bound);
    for (int pass = 0; pass < 2; pass++) {
        int count = pass ? envc : argc; char** pointers = pass ? environment : arguments;
        uint64_t vector = pass ? envp : argv;
        for (int i = 0; i < count; i++) {
            uint64_t address = uget64(vector + (uint64_t)i * 8), length;
            if ((error = user_string_size(address, &length))) goto done;
            if (length >= (uint64_t)((char*)block + bytes - cursor)) { error = -7; goto done; }   /* argv/envp changed since measured */
            pointers[i] = cursor; copy_from_user(cursor, (void*)address, length); cursor[length - 1] = 0; cursor += length;
        }
    }
    if (!argc) { arguments[0] = cursor; *cursor = 0; }
    char script_name[FAT32_PATH_MAX]; strcpy(script_name, requested); /* a nested level passes its interpreter */
    bool unreadable = false; /* a script the caller may not read: the image is not dumpable (prepare_image checks the ELF) */
    for (unsigned depth = 0;; depth++) {
        int found = script_interpreter(&source.file, script_name, &arguments, &actual_argc, &script_bytes[depth], path);
        if (found > 0) break;
        if (found < 0) { error = found; goto done; }
        if (disk_permission(&source.file, DISK_MAY_READ)) unreadable = true;
        script_vectors[depth] = arguments;
        if (depth == 4) { error = -40; goto done; }
        strcpy(script_name, path);
        if ((error = path_at_bound((uint64_t)-100, path, true, NULL))) goto done;
        if (!user_readable(path)) { error = -13; goto done; }
        if (!fat32_lookup(path, &source.file)) { error = -2; goto done; }
        if (source.file.is_dir || source.file.native_kind) { error = -13; goto done; }
        source.size = source.file.size;
        if (bound) { fd_unpin(bound); bound = NULL; }
    }
    if (trace_processes) {
        say("[64] EXEC_ARGS pid="); hex(task_pid());
        for (int i = 0; i < argc; i++)
            if (!strncmp(arguments[i], "--type=", 7) || !strncmp(arguments[i], "--utility-sub-type=", 19) ||
                !strncmp(arguments[i], "--mojo-platform-channel-handle=", 31)) { put(' '); say(arguments[i]); }
        put('\n');
    }
    struct loaded_image loaded = {0};
    /* The new image's stack is mapped eagerly from kernel frames: under memory pressure make room first, as clone does
     * (swap.h reclaim_kernel_room; the stack, its page tables, the vm_area and argument pages). */
    reclaim_kernel_room((unsigned)(STACK_SIZE / PAGE) + 64);
    error = prepare_image(&source, actual_argc, (const char* const*)arguments, envc, (const char* const*)environment, requested, &loaded);
    if (!error) {
        if (unreadable) space_set_dumpable(loaded.space, false);
        uint32_t ids[6], now[6]; exec_ids(ids);
        disk_exec_arm(&source.file);
        error = task_exec(r, loaded.entry, loaded.stack, loaded.space, path, disk_inode_id(&source.file), source.file.detached);
        disk_exec_finished();
        if (error) space_release(loaded.space);
        else {
            exec_ids(now);
            if (ids[1] != ids[0] || ids[4] != ids[3] || memcmp(ids, now, sizeof(ids))) space_set_dumpable(loaded.space, false);
        }
    }
done:
    for (unsigned depth = 0; depth < 5; depth++) if (script_vectors[depth]) arena_free((uint64_t)script_vectors[depth], script_bytes[depth]);
    arena_free((uint64_t)block, bytes); if (bound) fd_unpin(bound); return error;
}
static int run_program(const uint8_t* data, uint64_t size, char mode) {
    struct image_source source = {.size = size, .memory = data};
    strcpy(executable_path, "/selftest");
    char argument[] = {mode, 0}; const char* argv[] = {"selftest", argument};
    return run_image(&source, 2, argv);
}
/* Programs the kernel starts are looked up like execve: symbolic links are followed (merged /usr: /sbin -> usr/sbin). */
static bool boot_lookup(const char* path, fat32_file_t* file) {
    char resolved[FAT32_PATH_MAX];
    if (strlen(path) >= sizeof(resolved)) return false;
    if (fat32_lookup(path, file) && file->native_kind != FAT32_NATIVE_SYMLINK) return true;
    strcpy(resolved, path);
    return !canonical_path(resolved) && fat32_lookup(resolved, file) && file->native_kind != FAT32_NATIVE_SYMLINK;
}
static int run_file_args(const char* path, int argc, const char* const* argv) {
    struct image_source source = {0}; source.name = path;
    if (!boot_lookup(path, &source.file) || source.file.is_dir) return -1;
    if (strlen(path) >= sizeof(executable_path)) return -1;
    strcpy(executable_path, path);
    source.size = source.file.size;
    say("[64] LOADING "); say(path); say(" bytes="); hex(source.size); put('\n');
    int result = run_image(&source, argc, argv);
    memory_file_writeback(true, 0, ~0ULL);
    if (result) { say("[64] PROGRAM STATUS "); say(path); say(" "); hex((uint64_t)(int64_t)result); put(10); }
    return result;
}
static int run_file(const char* path, char mode) {
    char argument[] = {mode, 0}; const char* argv[] = {path, argument};
    return run_file_args(path, 2, argv);
}

static void descriptor_selftest(void) {
    uint64_t before = memory_used_pages();
    struct descriptor_table* saved = active_descriptors;
    struct descriptor_table* parent = fd_table_new(true); if (!parent) panic("descriptor context allocation");
    fd_table_activate(parent);
    struct open_file* output = file_get(1);
    struct descriptor_table* child = fd_table_clone(parent, false); if (!child) panic("descriptor context clone");
    fd_table_activate(child);
    if (file_get(1) != output || fd_close(1) || file_get(1)) panic("child descriptor isolation");
    fd_table_activate(parent);
    if (file_get(1) != output) panic("child closed parent handle");
    fd_pin(output);
    if (fd_close(1) || file_get(1) || fd_install(output, 1, false) != 1) panic("pinned descriptor lifetime");
    fd_unpin(output);
    struct descriptor_table* shared = fd_table_clone(parent, true);
    if (shared != parent || fd_close(2) || file_get(2)) panic("shared descriptor table semantics");
    fd_table_release(shared);
    fd_table_activate(child);
    if (!file_get(2)) panic("copied descriptor table lost handle");
    fd_table_activate(saved); fd_table_release(child); fd_table_release(parent);
    check(memory_used_pages() == before, "descriptor tables isolate handles and retain shared descriptions/pins");
}
static void space_selftest(void) {
    uint64_t before = memory_used_pages(), address = USER_START + 0x800000;
    struct address_space* saved = active_space;
    struct address_space* parent = space_create(); if (!parent) panic("COW parent allocation");
    space_activate(parent);
    if (!area_add(address, address + PAGE, 3, false) || !map_page(parent->root, address, WRITE | NX) ||
        !area_add(address + PAGE, address + 2 * PAGE, 1, false) || !map_page(parent->root, address + PAGE, NX)) panic("COW fixture mapping");
    uint64_t original = 0x1234567890ABCDEFULL;
    if (!copy_into(parent->root, address, &original, sizeof(original)) ||
        !copy_into(parent->root, address + PAGE, &original, sizeof(original))) panic("COW fixture data");
    uint64_t frame = *pte(parent->root, address, false) & ADDRESS;
    struct address_space* child = space_clone(parent);
    if (!child || page_references(frame) != 2) panic("COW shared page references");
    space_retain(parent); space_release(parent);
    space_activate(child);
    if (!accessible(address, 8, true)) panic("COW child write resolution");
    uput64(address, 99);
    if (memory_protect(address + PAGE, PAGE, 3) || !accessible(address + PAGE, 8, true)) panic("COW mprotect isolation");
    uput64(address + PAGE, 101);
    space_activate(parent);
    if (uget64(address) != original || uget64(address + PAGE) != original) panic("child modified parent memory");
    space_release(child);
    uint64_t owned = memory_used_pages();
    if (!accessible(address, 8, true) || memory_used_pages() != owned || page_references(frame) != 1) panic("single-owner COW upgrade");
    space_activate(saved); space_release(parent);
    check(memory_used_pages() == before, "COW address spaces preserve parent memory and reclaim ownership");
}
static void loader_tests(void) {
    uint64_t size = test_elf_end - test_elf;
    uint8_t* invalid_elf = (uint8_t*)arena_alloc(65536); if (!invalid_elf) panic("ELF fixture buffer");
    if (size > 65536 || !elf_valid(test_elf, size)) panic("invalid build fixture");
    for (unsigned i = 0; i < 11; i++) {
        memcpy(invalid_elf, test_elf, size);
        struct elf_header* h = (void*)invalid_elf;
        struct program_header* ph = (void*)(invalid_elf + h->phoff);
        unsigned first = 0; while (first < h->phnum && ph[first].type != 1) first++;
        if (first == h->phnum) panic("fixture has no PT_LOAD");
        struct program_header* p = &ph[first];
        switch (i) {
        case 0: h->ident[4] = 1; break;
        case 1: h->machine = 3; break;
        case 2: h->type = 1; break;
        case 3: h->phoff = ~0ULL; break;
        case 4: h->entry = 0x100000; break;
        case 5: p->filesz = size + 1; break;
        case 6: p->memsz = p->filesz - 1; break;
        case 7: p->flags = 7; break;
        case 8: p->offset = ~0ULL; break;
        case 9: p->va = ~0ULL - PAGE; break;
        case 10: p->align = 3; break;
        }
        if (elf_valid(invalid_elf, size)) panic("malformed ELF accepted");
    }
    arena_free((uint64_t)invalid_elf, 65536);
    check(!elf_valid(test_elf, 63), "ELF64 rejects truncated/malformed/unsupported images (12 cases)");
}
uint64_t xsave_mask; uint32_t xsave_size = 512;
extern const uint8_t vdso_image[], vdso_image_end[];
static void vdso_init(void) {
    const uint8_t* image = vdso_image; uint64_t size = (uint64_t)(vdso_image_end - vdso_image);
    if (size < 64 || memcmp(image, "\177ELF\2\1\1", 7)) return;
    uint64_t phoff = *(const uint64_t*)(image + 32); uint16_t phsize = *(const uint16_t*)(image + 54), phnum = *(const uint16_t*)(image + 56);
    if (phsize != 56 || phoff > size || (uint64_t)phnum * 56 > size - phoff) return;
    uint64_t span = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t* ph = image + phoff + i * 56; if (*(const uint32_t*)ph != 1) continue;
        uint64_t offset = *(const uint64_t*)(ph + 8), va = *(const uint64_t*)(ph + 16), filesz = *(const uint64_t*)(ph + 32), memsz = *(const uint64_t*)(ph + 40);
        if (offset > size || filesz > size - offset || filesz > memsz || va > 0x10000 || memsz > 0x10000) return;
        if (va + memsz > span) span = va + memsz;
    }
    span = (span + PAGE - 1) & ~(PAGE - 1); if (!span || span > 0x10000) return;
    struct memory_file* file = memory_file_create(0); if (!file) return;
    file->size = span;
    for (uint64_t page = 0; page < span / PAGE; page++) if (!memory_file_page(file, page, true)) { memory_file_release(file); return; }
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t* ph = image + phoff + i * 56; if (*(const uint32_t*)ph != 1) continue;
        uint64_t offset = *(const uint64_t*)(ph + 8), va = *(const uint64_t*)(ph + 16), filesz = *(const uint64_t*)(ph + 32);
        for (uint64_t done = 0; done < filesz; ) {
            uint64_t at = va + done, part = PAGE - (at & (PAGE - 1)); if (part > filesz - done) part = filesz - done;
            memcpy((uint8_t*)phys_to_virt(memory_file_page(file, at / PAGE, false)) + (at & (PAGE - 1)), image + offset + done, part); done += part;
        }
    }
    /* Fill the shared clock parameters (magic "NXVDSO04" then frequency, boot TSC, epoch, enabled, per-CPU
     * offsets in use, offsets[32], then the realtime line of timekeeping.c: seq, real_base, mono_base, mult, then
     * the CLOCK_BOOTTIME offset: time spent in system sleep, pm.c).
     * Runs after smp_init(). Skewed TSCs without RDTSCP: system calls only. */
    _Static_assert(MAX_CPUS <= 32, "vDSO TSC offset table");
    for (uint64_t page = 0; page < span / PAGE; page++) {
        uint8_t* base = phys_to_virt(memory_file_page(file, page, false));
        for (unsigned at = 0; at + 8 + 43 * 8 <= PAGE; at += 8) if (!memcmp(base + at, "NXVDSO04", 8)) {
            uint64_t* data = (uint64_t*)(base + at + 8); vdso_clock = data; data[41] = suspended_ns;   /* + boottime offset (pm.c) */
            data[0] = tsc_frequency; data[1] = boot_tsc; data[2] = realtime_epoch;
            data[3] = !smp_tsc_unstable && (!smp_tsc_skewed || smp_rdtscp);
            data[4] = smp_tsc_skewed && smp_rdtscp;
            data[42] = smp_rdtscp;   /* getcpu/sched_getcpu: RDTSCP's TSC_AUX is the CPU index (kabi) */
            for (unsigned i = 0; i < MAX_CPUS; i++) data[5 + i] = (uint64_t)cpus[i].tsc_offset;
            timekeeping_vdso(&data[5 + 32]);
            vdso_file = file; vdso_bytes = span;
            say("[64] vDSO ready bytes="); hex(span); put(10); return;
        }
    }
    memory_file_release(file);
}
/* Halt until the next interrupt (IRQ14 completion, or the 1 kHz tick as a fallback). */
static void ata_halt_wait(void) {
    if (block_io_sleep()) return;   /* task context: sleep without the big lock until IRQ14 or a tick (blockio.c) */
    /* Device IRQs reach only the BSP: an application processor polls the controller instead. */
    if (cpu_self()->index) { smp_tlb_poll(); __asm__ volatile("pause"); return; }
    /* Only the tick and IDE (IRQ_WAKE vectors) may interrupt a disk wait: NIC/input handlers must
     * not run while a syscall is mid-way through kernel state (irq.c). */
    irq_wait_restricted();
}
struct kernel_counters kstat;
void native_io_poll(void) {
    static uint64_t last; static struct block_stats previous; static struct kernel_counters kprev;
    usb_poll(); /* USB events, hot-plug worker, key repeat (BSP only, like the other device polls) */
    kmsg_poll(); /* wake /dev/kmsg readers (put() itself never wakes) */
    hwnode_poll(native_clock_ns()); /* cameras, force-feedback durations (hwnode.c) */
    timekeeping_poll(); /* end a finished adjtime slew */
    if (!trace_processes) return;
    uint64_t now = native_clock_ns(); if (now - last < 5000000000ULL) return;
    last = now; struct block_stats s; block_get_stats(&s);
    uint64_t misses, ahead; memory_file_cache_stats(&misses, &ahead);
    say("[64] IOSTAT rd="); hex(s.read_cmds - previous.read_cmds); say(" rdsec="); hex(s.read_sectors - previous.read_sectors);
    say(" wr="); hex(s.write_cmds - previous.write_cmds); say(" wrsec="); hex(s.write_sectors - previous.write_sectors);
    say(" flush="); hex(s.flushes - previous.flushes); say(" busy_ms="); hex((s.busy_ticks - previous.busy_ticks) * 1000 / tsc_frequency);
    say(" misses="); hex(misses); say(" ahead="); hex(ahead); say(" dirty="); hex(memory_file_dirty_total()); put(10);
    say("[64] KSTAT sw="); hex(kstat.switches - kprev.switches); say(" sys="); hex(kstat.syscalls - kprev.syscalls);
    say(" pf="); hex(kstat.faults - kprev.faults); say(" irq="); hex(kstat.irqs - kprev.irqs);
    say(" sync="); hex(kstat.syncs - kprev.syncs); say(" syncpg="); hex(kstat.sync_pages - kprev.sync_pages);
    say(" wbpass="); hex(kstat.flush_passes - kprev.flush_passes); say(" fork="); hex(kstat.forks - kprev.forks); put(10);
    previous = s; kprev = kstat;
    reclaim_trace();   /* [64] MEMSTAT: free memory, swap, OOM kills, disk-wait sleeps (reclaim.c) */
}
/* tick.c (tickless idle): when the boot CPU's write-back, journal commit, descriptor collection and trace polls need it
 * next: dirty file pages every 20 ms (native_writeback_poll's idle pass), ext4's 5 s commit / 30 s checkpoint, the
 * 100 ms collector, the 5 s IOSTAT/MEMSTAT trace. */
uint64_t disk_commit_next_event(uint64_t now);
uint64_t native_polls_next_event(uint64_t now) {
    uint64_t due = disk_commit_next_event(now);
    if (memory_file_dirty_files() && now + 20000000ULL < due) due = now + 20000000ULL;
    if ((gc_pending || prune_pending) && now + 100000000ULL < due) due = now + 100000000ULL;
    if (trace_processes && now + 5000000000ULL < due) due = now + 5000000000ULL;
    return due;
}
static void cpuid2(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static void xsetbv0(uint64_t value) {
    __asm__ volatile(".byte 0x0f,0x01,0xd1" :: "c"(0), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}
/* Enable x87|SSE|AVX extended state so applications may use AVX/AVX2/FMA. */
static void fpu_init(void) {
    uint32_t a, b, c, d; cpuid2(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 26))) { say("[64] FPU state: FXSAVE (no XSAVE)\n"); return; }
    uint64_t cr4; __asm__ volatile("mov %%cr4,%0" : "=r"(cr4)); __asm__ volatile("mov %0,%%cr4" :: "r"(cr4 | (1ULL << 18)) : "memory");
    cpuid2(0xD, 0, &a, &b, &c, &d);
    uint64_t mask = (((uint64_t)d << 32) | a) & 7;
    if ((mask & 3) != 3) mask = 3;
    xsetbv0(mask);
    cpuid2(0xD, 0, &a, &b, &c, &d);
    if (b < 576 || b > FPU_AREA_BYTES) {
        xsetbv0(3); __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory");
        say("[64] FPU state: FXSAVE (XSAVE area too large)\n"); return;
    }
    xsave_mask = mask; xsave_size = b;
    say("[64] FPU state: XSAVE xcr0="); hex(mask); say(" bytes="); hex(b); put('\n');
}
void fpu_cpu_init(void) {
    if (!xsave_mask) return;
    uint64_t cr4; __asm__ volatile("mov %%cr4,%0" : "=r"(cr4)); __asm__ volatile("mov %0,%%cr4" :: "r"(cr4 | (1ULL << 18)) : "memory");
    xsetbv0(xsave_mask);
}
void kernel64_main(void) {
    boot_info_init(); screen_init();
    say("NexusOS native x86-64 - custom kernel development target\n\n"); boot_info_print();
    sandbox_trace = boot_option("sbxtrace");   /* log sandbox setup: namespaces, SBX_* env, seccomp installs */
    smap_wanted = !boot_option("nosmap");   /* CR4.SMAP on unless `nosmap` (uaccess.h) */
    { extern bool kguard_probe_armed; kguard_probe_armed = boot_option("kguard.probe"); }   /* kguard.c test triggers */
    uint64_t efer; uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080)); efer = ((uint64_t)hi << 32) | lo;
    check(sizeof(void*) == 8 && (efer & (1 << 10)), "64-bit long mode active");
    cpu_tables(); fpu_init(); acpi_init(); protect_kernel(); memory_init(); memory_selftest(); serial_ring_init(); kmsg_init();
    platform_init(); screen_map_high();
    uint64_t kernel_pages = memory_used_pages();
    space_selftest(); descriptor_selftest(); signals_selftest(); credentials_selftest(); pid_selftest(); time_init(); kmsg_clock_start(); timekeeping_init(realtime_epoch);
    acpi_aml_init(); /* uACPI: namespace, SCI, EC, ACPI devices; table-only mode on failure (acpi_aml.c) */
    check(security_verify_wx(), "W^X: no kernel mapping is both writable and executable"); hardening_report();
    { char option[32]; lockdown_init(boot_value("lockdown", option, sizeof(option)) ? option : ""); }   /* Secure Boot or `lockdown=` */
    cpus[0].loaded_space = &kernel_space; smp_kernel_root = kernel_space.root; smp_init();
    input_init();
    memory_file_writeback_init();
    reclaim_init();   /* watermarks, kswapd (permanent: before the baselines below) */
    vdso_init();
    { /* ~4 MiB set-associative metadata cache (directory/FAT sectors) */
        uint64_t bytes = 1024ULL * 4112, cache = arena_alloc(bytes); if (cache) fat32_set_volume_cache((void*)cache, bytes);
        fat32_lookup_cache_init();
    }
    pci_bar_fixup(); /* before the first pci_init() scan */
    native_network_init();sound_init();virtio_gpu_init();drm_init();platform_selftest();
    power_init(); /* ACPI devices in /sys, power events, suspend/resume registry (power.c) */
    /* IDE, AHCI and NVMe disks, partitions and the root volume: FAT32 (blockdev.c) or ext4 (ext4_dev.c);
     * allocates before the baseline. */
    ata_set_clock(read_tsc, tsc_frequency); ata_set_wait_hook(ata_halt_wait);
    bool have_disk = block_probe(tsc_frequency);
    hibernate_resume(); /* a valid hibernation image is restored here (no return); a stale one is discarded (hibernate.c) */
    if (!have_disk) have_disk = disk_root_present();
    kernel_pages=memory_used_pages();
    hwnode_init(); /* /dev/uinput; the virtual camera when booted with vcam (hwnode.c) */
    usb_init(); kernel_pages = memory_used_pages(); /* xHCI + boot-time USB devices (PCI was scanned above) */
    { bool md_boot(void); bool cryptroot_boot(void); bool lvm_boot(void); bool verity_boot(void);   /* md (md.c), LUKS (cryptroot.c), LVM (lvm_boot.c), dm-verity (dm_verity.inc), stacked, then a hibernation image */
      if (!have_disk) { bool md = md_boot(), unlocked = cryptroot_boot(), lvm = lvm_boot(), verity = verity_boot();
        if (md || unlocked || lvm || verity) { have_disk = block_root_late(0) || disk_root_present(); void hibernate_resume_unlocked(void); hibernate_resume_unlocked(); kernel_pages = memory_used_pages(); } } }
    if (!have_disk) { have_disk = block_root_late(15000000000ULL) || disk_root_present(); kernel_pages = memory_used_pages(); } /* root on USB storage */
    diag_run(); kernel_pages = memory_used_pages(); /* diag: report (after a USB root appeared: NEXUSDIAG.TXT can reach the stick) */
    if (have_disk) {
        bool mounted = fat32_init();
        check(mounted, disk_posix() ? "native ATA/ext4 root mount" : "native ATA/FAT32 mount");
        native_nic_late_init(); /* NIC firmware from /lib/firmware (kept for resume: before the baseline) */
        kernel_pages = memory_used_pages(); /* the mounted volume's caches are permanent */
        void osinstall_check(void); osinstall_check(); kernel_pages = memory_used_pages(); /* USB system -> internal disk (osinstall.c) */
        fat32_file_t writeback_test;
        if (fat32_lookup("/nexus/test-network", &writeback_test)) {
            uint8_t port[2];
            check(writeback_test.size==2&&fat32_read_file(&writeback_test,0,2,port)==2&&
                  native_network_selftest((uint16_t)(port[0]|(uint16_t)port[1]<<8)),"native Ethernet/ARP/IPv4/TCP stream and ownership");
            say("[64] NETWORK CAMPAIGN PASSED\n");finish(0x10);
        }
        if (fat32_lookup("/nexus/test-tun", &writeback_test)) {   /* TUN/TAP (tests/test_x64_tun.py, tun.c) */
            check(run_file("/bin/tun64", '0') == 0, "TUN/TAP data path, ioctls, persistence");
            say("[64] TUN CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-packet", &writeback_test)) {   /* AF_PACKET (tests/test_x64_packet.py, packet.c) */
            uint64_t before = memory_used_pages() - native_network_background_pages() - disk_inode_pages() - memory_file_disk_pages();
            check(run_file("/bin/packet64", '0') == 0, "AF_PACKET capture, sending, BPF, memberships and mmap rings");
            check(!packet_socket_count && memory_used_pages() - native_network_background_pages() - disk_inode_pages() - memory_file_disk_pages() == before,
                  "packet sockets, rings and frames reclaimed");
            say("[64] PACKET CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-display", &writeback_test)) {
            check(run_file("/bin/display64", '0') == 0, "native framebuffer from userland");
            check(!display_references && memory_used_pages() == kernel_pages + disk_inode_pages(), "display mappings and descriptors reclaimed");
            say("[64] DISPLAY CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-audio", &writeback_test)) {
            uint64_t devices_before = storage_background_pages();   /* a USB card plugged meanwhile may stay */
            check(run_file("/bin/audio64", '0') == 0, "ALSA playback through the native AC97/HDA driver");
            check(memory_used_pages() - storage_background_pages() == kernel_pages - devices_before + disk_inode_pages() + memory_file_disk_pages(), "audio buffers and descriptors reclaimed");
            say("[64] AUDIO CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-hw", &writeback_test)) {   /* cameras, game controllers, Bluetooth (tests/test_x64_camera.py, ...) */
            uint64_t devices_before = storage_background_pages();   /* a device plugged meanwhile may stay */
            { fat32_file_t marker; credentials_boot_root(fat32_lookup("/nexus/init-as-root", &marker)); }   /* Bluetooth tests need root */
            check(run_file("/bin/hw64", '0') == 0, "hardware class test program");
            check(memory_used_pages() - storage_background_pages() == kernel_pages - devices_before + disk_inode_pages() + memory_file_disk_pages(), "device buffers and descriptors reclaimed");
            say("[64] HW CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-gpu", &writeback_test)) {
            check(drm_available() && run_file("/bin/gpu64", '0') == 0, "virgl rendering through the virtio-gpu DRM render node");
            drm_drain();
            check(!drm_live_objects() && memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "GPU objects, mappings and descriptors reclaimed");
            say("[64] GPU CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-kms", &writeback_test)) {   /* modesetting on the virtio-gpu scanouts (tests/test_x64_kms.py) */
            check(kms_available() && run_file("/bin/kms64", '0') == 0, "KMS modesetting, hot-plug and dumb buffers on /dev/dri/card0");
            drm_drain();
            check(!kms_live_objects() && memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "KMS framebuffers, dumb buffers and descriptors reclaimed");
            say("[64] KMS CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-apps", &writeback_test)) {
            check(run_file("/bin/pty64", '0') == 0, "pseudo-terminals, controlling terminals and job control");
            check(run_file("/bin/inotify64", '0') == 0, "inotify watches and events on the FAT root");
            check(run_file("/bin/sysext64", '0') == 0, "mremap, fallocate, sendfile, copy_file_range, statx and vfat attributes");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "terminals, watches and descriptors reclaimed");
            say("[64] APPS CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-wow32", &writeback_test)) { /* WoW64 CPU modes (tests/test_x64_wow32.py) */
            segments_selftest();
            check(run_file("/bin/wow32", '0') == 0, "LDT/TLS selectors, int $0x80, sigreturn selectors and 32-bit compatibility mode");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "LDTs, threads and descriptors reclaimed");
            say("[64] WOW32 CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-readonly", &writeback_test)) {
            check(run_file("/bin/statfs64", 'r') == 0, "unclean FAT is readable and rejects namespace/data mutations");
            say("[64] READONLY CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-writeback-errors", &writeback_test)) {
            check(run_file("/bin/filemap64", 'x') == 0, "device writeback errors reach applications");
            check(fat32_lookup("/home/nexus/writeback-errors.dat", &writeback_test), "writeback fixture exists");
            struct memory_file* cache = memory_file_disk_find(&writeback_test);
            uint64_t frame = cache ? memory_file_page(cache, 0, false) : 0;
            check(cache && cache->references == 1 && cache->io_error && frame && *(uint8_t*)phys_to_virt(frame) == 'Z', "dirty cache survives process exit after writeback failure");
            uint8_t byte; check(fat32_read_file(&writeback_test, 0, 1, &byte) == 1 && byte == '1', "failed writeback leaves original disk bytes");
            check(memory_used_pages() == kernel_pages + memory_file_disk_pages() + disk_inode_pages(), "retained cache has explicit ownership without process leaks");
            say("[64] WRITEBACK ERROR CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-crash", &writeback_test)) { /* ext4 crash consistency (tests/test_x64_ext4_crash.py) */
            char mode[16] = {0}; fat32_read_file(&writeback_test, 0, sizeof(mode) - 1, mode);
            if (!strncmp(mode, "commit:", 7)) disk_test_poweroff_after((unsigned)(mode[7] - '0'));
            check(run_file("/bin/crash64", 'c') == 0, "crash-consistency workload");
            say("[64] CRASH CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-seccomp", &writeback_test)) { /* seccomp conformance (tests/test_x64_seccomp.py) */
            check(run_file("/bin/seccomp64", '0') == 0, "seccomp: strict, BPF validate/interpret, actions, precedence, TSYNC, USER_NOTIF");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "seccomp campaign pages reclaimed");
            say("[64] SECCOMP CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-lockdown", &writeback_test)) { /* kernel lockdown (tests/test_x64_lockdown.py) */
            lockdown_selftest();
            credentials_boot_root(true);   /* writing /sys/kernel/security/lockdown needs CAP_SYS_ADMIN */
            check(run_file("/bin/secfs64", '0') == 0, "securityfs: /sys/kernel/security/lockdown read/write + ratchet (root)");
            say("[64] LOCKDOWN CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-kptr", &writeback_test)) { /* kernel-pointer hygiene (tests/test_x64_kptr.py) */
            credentials_boot_root(true);   /* kptr_restrict/dmesg_restrict writes need CAP_SYS_ADMIN, kallsyms reveal CAP_SYSLOG */
            check(run_file("/bin/kptr64", '0') == 0, "kptr: /proc/kallsyms address gating + writable kptr_restrict/dmesg_restrict");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "kptr campaign pages reclaimed");
            say("[64] KPTR CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-kguard", &writeback_test)) { /* kguard oops containment (tests/test_x64_kguard.py) */
            uint64_t baseline = kernel_pages + disk_inode_pages() + memory_file_disk_pages();
            check(run_file("/bin/kguard64", '0') != 0, "kguard: a stray ring-0 user access (SMAP) killed the task, not the kernel");
            check(memory_used_pages() == baseline, "kguard: SMAP-killed task fully reclaimed");
            check(run_file("/bin/selftest64", '0') == 0, "kguard: the kernel still runs user programs after an SMAP oops");
            check(run_file("/bin/kguard64", '1') != 0, "kguard: a kernel-stack overflow killed the task, not the kernel");
            check(memory_used_pages() == baseline, "kguard: stack-overflow-killed task fully reclaimed");
            check(run_file("/bin/selftest64", '0') == 0, "kguard: the kernel still runs user programs after a stack-overflow oops");
            say("[64] KGUARD CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-umip", &writeback_test)) { /* UMIP emulation (tests/test_x64_umip.py) */
            check(run_file("/bin/umip64", '0') == 0, "UMIP: SGDT/SIDT/SLDT/SMSW/STR emulated with Linux dummy values");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "UMIP campaign pages reclaimed");
            say("[64] UMIP CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-nsfs", &writeback_test)) { /* nsfs magic links + setns (tests/test_x64_nsfs.py) */
            check(run_file("/bin/nsfs64", '0') == 0, "nsfs: /proc/PID/ns + fd magic links, CLONE_NEWNET/NEWIPC, setns");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "nsfs campaign pages reclaimed");
            say("[64] NSFS CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-kabi", &writeback_test)) { /* Linux ABI: timers, signalfd, pidfd, clone3 (tests/test_x64_kabi.py) */
            check(run_file("/bin/kabi64", '0') == 0, "kabi: POSIX timers, timerfd, itimers, signalfd, sigtimedwait, pidfd, clone3, quotactl");
            check(run_file("/bin/kabi2", '0') == 0, "kabi2: System V IPC, Landlock, RLIMIT_SIGPENDING, fdinfo, mount API errors");
            credentials_boot_root(true);   /* clock steps, _ALARM clocks: as root */
            check(run_file("/bin/kabi64", 'r') == 0, "kabi as root: clock steps, CANCEL_ON_SET, _ALARM clocks");
            check(run_file("/bin/kabi2", 'r') == 0, "kabi2 as root: System V IPC, IPC sysctls and namespaces, mount API, _ALARM wake");
            credentials_boot_root(false);
            check(!sysvipc_objects() && !landlock_users, "kabi campaign System V IPC objects and Landlock domains released");
            check(!posix_timer_objects() && memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "kabi campaign timers and pages reclaimed");
            say("[64] KABI CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-aslr", &writeback_test)) { /* ASLR (tests/test_x64_aslr.py) */
            check(run_file("/bin/aslr64", '0') == 0, "ASLR: PIE/mmap/stack/brk randomized, AT_RANDOM fresh, ADDR_NO_RANDOMIZE honored");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "ASLR campaign pages reclaimed");
            say("[64] ASLR CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-memlayout", &writeback_test)) { /* user space from 64 KiB (tests/test_x64_memlayout.py) */
            char mode[2] = {'0', 0}; fat32_read_file(&writeback_test, 0, 1, mode);
            check(run_file("/bin/memlayout64", mode[0]) == 0, "user space from 64 KiB, MAP_FIXED/MAP_32BIT, low ET_EXEC (mode in log)");
            check(memory_used_pages() == kernel_pages + disk_inode_pages() + memory_file_disk_pages(), "memory layout campaign pages reclaimed");
            say("[64] MEMLAYOUT CAMPAIGN PASSED\n"); finish(0x10);
        }
        if (fat32_lookup("/nexus/test-power", &writeback_test)) { /* ACPI devices, power events, sleep (tests/test_x64_acpi.py, test_x64_suspend.py) */
            swap_boot();   /* /etc/fstab swap, as before init (hibernation into swap: test_x64_suspend.py hibernate-swap*) */
            int status = run_file("/bin/power64", 'a');
            say("[64] POWER CAMPAIGN STATUS="); hex((uint64_t)(int64_t)status); put(10); finish(0x10);
        }
        { void md_boot_late(void); md_boot_late(); void crypttab_boot(void); crypttab_boot(); void lvm_boot_late(void); lvm_boot_late(); }   /* arrays, /etc/crypttab, LVM VGs before fstab */
        mount_fstab();   /* /etc/fstab entries before the session (mount.c) */
        fat32_file_t init; char slot_init[32];   /* A/B core updates: the booted slot's Aurora (bootslot.c) */
        const char* init_path = boot_slot_init(slot_init, sizeof(slot_init));
        if (!init_path || !boot_lookup(init_path, &init)) init_path = "/sbin/init";
        else say("[64] INIT from the booted slot\n");
        if (boot_lookup(init_path, &init)) {
            { fat32_file_t marker; credentials_boot_root(fat32_lookup("/nexus/init-as-root", &marker)); }   /* test disks */
            swap_boot();   /* /etc/fstab swap entries (swap.c) */
            trace_processes = true; const char* argv[] = {init_path, "--chrome-session"};
            bool chrome_session = fw_chrome_session(session_select, session_read);
            say(chrome_session ? "[64] INIT SESSION chrome override\n" : "[64] INIT SESSION configured desktop\n");
            int session = run_file_args(argv[0], chrome_session ? 2 : 1, argv);   /* an orderly shutdown may have to kill init */
            check(session == 0 || power_orderly_active(), "native init session exited cleanly");
            say("[64] INIT SESSION RETURNED\n"); finish(0x10);
        }
        fat32_file_t chrome; const char* chrome_path = "/opt/google/chrome/chrome";
        if (fat32_lookup(chrome_path, &chrome)) {
            trace_processes = true;
            const char* defaults[] = {chrome_path, "--version"};
            const char** argv = defaults; int argc = 2;
            fat32_file_t config;
            if (fat32_lookup("/nexus/chrome.args", &config) && config.size) {
                if (config.size > STACK_SIZE / 2) panic("Chrome arguments exceed stack budget");
                char* data = (void*)arena_alloc((uint64_t)config.size + 1);
                if (!data || fat32_read_file(&config, 0, config.size, data) != (int32_t)config.size) panic("cannot read Chrome arguments");
                unsigned count = 1;
                for (uint32_t i = 0; i < config.size; i++) if (data[i] == '\n') count++;
                argv = (void*)arena_alloc(((uint64_t)count + 1) * sizeof(char*));
                if (!argv) panic("no memory for Chrome arguments");
                argc = 1; argv[0] = chrome_path;
                char* line = data;
                for (uint32_t i = 0; i <= config.size; i++) if (data[i] == '\n' || i == config.size) {
                    data[i] = 0; if (i && data[i - 1] == '\r') data[i - 1] = 0;
                    if (*line) argv[argc++] = line;
                    line = data + i + 1;
                }
            }
            bool version_probe = argc == 2 && !strcmp(argv[1], "--version");
            int result = run_file_args(chrome_path, argc, argv);
            say("[64] CHROME PROBE STATUS="); hex((uint64_t)(int64_t)result); put('\n');
            if (result) panic("Chrome startup probe failed");
            say(version_probe ? "[64] CHROME VERSION PROBE PASSED (interactive browsing still pending)\n" : "[64] CHROME PROBE RETURNED\n");
            finish(0x10);
        }
    }
    loader_tests();
    check(run_program(test_elf, test_elf_end - test_elf, '0') == 0, "ELF64 ring-3 + SYSCALL + pointers above 4 GiB");
    const unsigned vectors[] = {14, 14, 14, 14, 6, 13};
    const unsigned errors[] = {5, 4, 7, 21, 0, 0};
    for (unsigned i = 0; i < 6; i++) {
        int status = run_program(test_elf, test_elf_end - test_elf, '1' + i);
        if (status != (i == 4 ? 132 : 139) || last_fault != vectors[i] || last_error != errors[i]) panic("user isolation test");
    }
    check(memory_used_pages() == kernel_pages, "six user faults contained; process pages reclaimed");
    check(run_program(test_elf, test_elf_end - test_elf, '0') == 0, "user execution still works after fault recovery");
    if (have_disk) {
        check(run_file("/bin/selftest64", '0') == 0, "streamed ELF64 from disk, beyond boot-image size");
        check(run_file("/bin/truncated64", '0') == -1, "truncated disk executable rejected");
        check(run_file("/bin/missing64", '0') == -1, "missing executable rejected");
        check(run_file("/bin/dynamic64", '0') == 0, "unmodified glibc interpreter and dynamically linked application");
        check(run_file("/bin/dynamic64", 'p') == 139 && last_fault == 14 && last_error == 7, "mprotect blocks user writes");
        check(run_file("/bin/dynamic64", 'u') == 139 && last_fault == 14 && last_error == 4, "munmap removes user access");
        check(run_file("/bin/dynamic64", 'b') == 135 && last_fault == 14, "file mmap past EOF terminates with SIGBUS");
        check(run_file("/bin/dynamic64", '0') == 0, "dynamic execution recovers after VM protection faults");
        check(run_file("/bin/signals64", '0') == 0, "signal delivery, masks, alternate stack and register restore");
        check(run_file("/bin/signals64", 'c') == 139, "sigreturn rejects kernel code selector");
        check(run_file("/bin/signals64", 'f') == 139, "sigreturn rejects invalid MXCSR");
        check(run_file("/bin/dynamic64", '0') == 0, "process still runs after malicious signal frames");
        check(run_file("/bin/ipc64", '0') == 0, "native Unix socket and descriptor semantics");
        check(run_file("/bin/process64", '0') == 0, "native fork, timer preemption, blocking IPC and child reaping");
        {
            fat32_file_t marker; unsigned rounds = fat32_lookup("/nexus/test-threads-loop", &marker) ? 300 : 1;
            for (unsigned round = 0; round < rounds; round++) {
                int status = run_file("/bin/threads64", '0');
                if (status) { say("[64] THREADS STATUS="); hex((uint64_t)(int64_t)status); say(" round="); hex(round); put(10); }
                check(status == 0, "native pthread synchronization, signals, TLS and FPU state");
            }
            if (rounds > 1) { say("[64] THREADS LOOP PASSED\n"); finish(0x10); }
        }
        {
            fat32_file_t marker; unsigned rounds = fat32_lookup("/nexus/test-vm-loop", &marker) ? 100 : 1;
            for (unsigned round = 0; round < rounds; round++) {
                int status = run_file("/bin/vmstress64", '0');
                if (status) { say("[64] VMSTRESS STATUS="); hex((uint64_t)(int64_t)status); say(" round="); hex(round); put(10); }
                check(status == 0, "concurrent page faults: zero-fill, copy-on-write across processes, racing unmap/discard");
            }
            if (rounds > 1) { say("[64] VM LOOP PASSED\n"); finish(0x10); }
        }
        check(run_file("/bin/threads64", 'l') == 0, "thread group survives initial thread exit");
        check(run_file("/bin/threads64", 'g') == 42, "exit_group unwinds blocked IPC/futex threads");
        check(run_file("/bin/threads64", 'f') == 143, "fatal thread signal terminates the whole process");
        check(run_file("/bin/pipes64", '0') == 0, "native pipes and vectored I/O preserve bytes and atomic writes");
        check(run_file("/bin/pipes64", 'p') == 141, "broken pipe delivers default SIGPIPE termination");
        check(run_file("/bin/events64", '0') == 0, "native poll/epoll readiness and atomic signal-mask waits");
        check(run_file("/bin/namespaces64", '0') == 0, "user namespace credentials, procfs mappings and privilege boundaries");
        check(run_file("/bin/pid64", '0') == 0, "PID namespace visibility, process lifetime and retained identity");
        check(run_file("/bin/fs64", '0') == 0, "filesystem contexts, chroot boundaries and proc directory lifetime");
        check(run_file("/bin/shmem64", '0') == 0, "shared-memory files, seals and coherent process mappings");
        check(run_file("/bin/limits64", '0') == 0, "enforced memory/descriptor quotas and resource-limit inheritance");
        check(run_file("/bin/filemap64", '0') == 0, "read-only disk mappings and private COW preserve file contents");
        check(run_file("/bin/jit64", '0') == 0, "explicit JIT execution permissions preserve default NX and process isolation");
        check(run_file("/bin/locks64", '0') == 0, "advisory lock ranges, descriptor ownership and blocking semantics");
        check(run_file("/bin/statfs64", '0') == 0, "filesystem geometry and statfs/statvfs interfaces");
        check(run_file("/bin/unix64", '0') == 0, "native Unix connections, accept queues and endpoint ownership");
        fat32_file_t mutation_test;
        if (fat32_lookup("/nexus/test-writes", &mutation_test)) {
            check(run_file("/bin/dynamic64", 'd') == 0, "persistent directory API and path permissions");
            check(run_file("/bin/unix64", 'd') == 0, "named Unix sockets and symbolic links on persistent storage");
            check(run_file("/bin/fs64", 'd') == 0, "rooted symlinks and filesystem mutation boundaries");
            check(run_file("/bin/filemap64", 'd') == 0, "shared FAT mapping writeback and executable-file protection");
            check(run_file("/bin/locks64", 'd') == 0, "advisory locks on persistent FAT files");
            check(run_file("/bin/rename64", 'd') == 0, "rename/unlink preserve live inode identities and release replaced chains");
            check(run_file("/bin/statfs64", 'd') == 0, "filesystem free-space accounting tracks mutations");
        }
        if (fat32_lookup("/nexus/test-posix", &mutation_test)) {
            check(run_file("/bin/posix64", 'u') == 0, "ext4 POSIX semantics as an unprivileged user and as root after set-user-ID execve");
        }
        check(memory_used_pages() == kernel_pages + disk_inode_pages(), "disk loader reclaims process resources");
        say("[64] DISK TESTS PASSED\n");
    } else say("[64] No application disk attached\n");
    say("[64] SMP switches"); for (unsigned i = 0; i < MAX_CPUS; i++) if (cpus[i].online) { say(" cpu"); hex(i); say("="); hex(cpus[i].switches); } put(10);
    say("[64] ALL TESTS PASSED\n");
    if (boot_option("selftest.poweroff") || boot_option("selftest.restart")) { /* reboot(2) from ring 3 (test_x64_uefi.py) */
        int status = run_program(test_elf, test_elf_end - test_elf, boot_option("selftest.poweroff") ? 'r' : 'R');
        say("[64] REBOOT RETURNED status="); hex((uint64_t)(int64_t)status); put('\n'); panic("reboot(2) did not stop the machine");
    }
    finish(0x10);
}
