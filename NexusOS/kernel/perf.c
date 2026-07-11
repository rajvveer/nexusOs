/* ============================================================================
 * NexusOS — Performance / Profiling — Phase 48
 * ============================================================================
 * See perf.h. Timing uses the established tick-boundary trick (spin to a tick
 * edge, run for N ticks, count ops) at the 18.2 Hz PIT resolution (≈55 ms/tick)
 * — the same approach gpubench uses. Integer-only, no high-res timer.
 * ============================================================================ */

#include "perf.h"
#include "vga.h"
#include "string.h"
#include "heap.h"

extern volatile uint32_t system_ticks;

#define TICK_MS 55u

/* Boot-time stamp: ticks elapsed from timer-online (system_ticks=0 at pic_init)
 * to the end of automated init. */
static uint32_t boot_ticks = 0;

void perf_mark_boot_done(void) {
    boot_ticks = system_ticks;
}
uint32_t perf_boot_ticks(void) { return boot_ticks; }

/* --------------------------------------------------------------------------
 * Tick-boundary benchmark driver: run fn() repeatedly for `min_ticks` PIT
 * ticks starting on a tick edge; return op count, fill *ms_out.
 * -------------------------------------------------------------------------- */
static uint32_t bench(void (*fn)(void), uint32_t min_ticks, uint32_t* ms_out) {
    uint32_t t0 = system_ticks;
    while (system_ticks == t0) { }          /* align to a tick edge */
    uint32_t start = system_ticks, n = 0;
    while ((system_ticks - start) < min_ticks) { fn(); n++; }
    uint32_t ms = (system_ticks - start) * TICK_MS;
    if (ms == 0) ms = 1;
    *ms_out = ms;
    return n;
}

/* --- benchmark bodies ----------------------------------------------------- */
#define BUF_BYTES (64u * 1024u)   /* 64 KB working set — modest (heap landmine) */
static uint8_t* pa = 0;
static uint8_t* pb = 0;

static void bm_memcpy(void) { memcpy(pa, pb, BUF_BYTES); }
static void bm_memset(void) { memset(pa, 0xA5, BUF_BYTES); }

/* a tight integer loop = a "no-op work" baseline for ops/s */
static volatile uint32_t sink;
static void bm_loop(void) {
    uint32_t acc = 0;
    for (int i = 0; i < 100000; i++) acc += (uint32_t)i;
    sink = acc;
}

/* --------------------------------------------------------------------------
 * Report helpers
 * -------------------------------------------------------------------------- */
static void print_kv(const char* k, uint32_t v, const char* unit) {
    char b[12];
    vga_print("  ");
    vga_print_color((char*)k, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    int_to_str((int)v, b); vga_print(b); vga_print(" "); vga_print(unit); vga_print("\n");
}

void perf_smp_status(void) {
    vga_print_color("\n  SMP / Multi-core Status\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =======================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  CPUs online: ");
    vga_print_color("1", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("   (QEMU is started single-core; NexusOS runs uniprocessor)\n");
    vga_print("\n  Real SMP would require:\n");
    vga_print_color("   - parse ACPI MADT / MP tables to enumerate CPUs\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   - Local APIC + IO-APIC (replace the legacy 8259 PIC)\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   - AP bring-up via INIT-SIPI-SIPI + a trampoline\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   - per-CPU state (GDT/TSS/stack) and spinlocks on every\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("     shared structure (heap, VGA, scheduler, net, VFS)\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  The kernel is currently single-cooperative-thread by design.\n");
}

void perf_preempt_status(void) {
    vga_print_color("\n  Preemption / Scheduler Status\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =============================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  Model: ");
    vga_print_color("cooperative (round-robin scheduler present, left disabled)\n",
                    VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("  The timer IRQ0 advances the clock and could drive preemption,\n");
    vga_print("  but the scheduler is intentionally NOT started: the kernel has\n");
    vga_print("  one shared address space with no locks on the heap/VGA/VFS, so\n");
    vga_print("  preempting mid-syscall would corrupt them (and aggravates the\n");
    vga_print("  known boot-time layout fault). Apps cooperate via the main\n");
    vga_print("  loops; idle paths HLT so an idle CPU isn't spinning.\n");
    vga_print_color("  (Phase 48 added a boot-init guard so IRQ-time work is\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   deferred until init completes — closes the fault window.)\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * perf_run: the `perf` command
 * -------------------------------------------------------------------------- */
void perf_run(void) {
    vga_print_color("\n  NexusOS Performance Report (Phase 48)\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =====================================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Honest status first (it's long), so the measured NUMBERS below stay
     * on-screen as the takeaway. */
    perf_smp_status();
    perf_preempt_status();

    vga_print_color("\n  Measurements\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ------------\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Boot time */
    uint32_t bt = boot_ticks;
    char b[12];
    vga_print("  Boot (init) time    : ");
    int_to_str((int)bt, b); vga_print_color(b, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(" ticks (~");
    int_to_str((int)(bt * TICK_MS), b); vga_print_color(b, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(" ms)  ");
    if (bt > 0 && bt * TICK_MS < 1000)
        vga_print_color("[under 1 second]\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    else
        vga_print("\n");

    /* Memory throughput benchmarks (need two aligned buffers). */
    pa = (uint8_t*)kmalloc_aligned(BUF_BYTES, 4);
    pb = (uint8_t*)kmalloc_aligned(BUF_BYTES, 4);
    if (pa && pb) {
        memset(pb, 0x5A, BUF_BYTES);
        uint32_t ms;
        uint32_t n;

        n = bench(bm_memcpy, 9, &ms);
        /* MB/s = n * 64KB / ms / 1000 ... compute as (n*64*1000)/ms KB/s -> /1024 MB/s.
         * Keep 32-bit: n*64 (KB) per op; total KB = n*64; KB/s = total*1000/ms. */
        {
            uint32_t kb = n * (BUF_BYTES / 1024u);        /* total KB moved */
            uint32_t kbps = (kb / ms) * 1000u;            /* avoid overflow: kb/ms first */
            print_kv("memcpy (rep movsl)  : ", kbps / 1024u, "MB/s");
        }
        n = bench(bm_memset, 9, &ms);
        {
            uint32_t kb = n * (BUF_BYTES / 1024u);
            uint32_t kbps = (kb / ms) * 1000u;
            print_kv("memset (rep stosl)  : ", kbps / 1024u, "MB/s");
        }
        kfree(pa); kfree(pb); pa = pb = 0;
    } else {
        vga_print("  (memory benchmark skipped — alloc failed)\n");
        if (pa) kfree(pa);
        if (pb) kfree(pb);
        pa = pb = 0;
    }

    /* Integer loop baseline (100k adds per op). */
    {
        uint32_t ms;
        uint32_t n = bench(bm_loop, 9, &ms);
        uint32_t mops = (n / ms) * 100u;   /* (ops * 100k) / ms / 1000 = n*100/ms Mops/s */
        print_kv("int loop            : ", mops, "Mops/s (100k adds/op)");
    }
    vga_print("\n");
}
