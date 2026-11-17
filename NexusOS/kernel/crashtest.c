/* ============================================================================
 * NexusOS — crashtest (implementation) — Phase 55
 * ============================================================================
 * Each sub-command provokes one real CPU exception in ring-0 kernel code.
 * The shell runs every command inside a kguard, so the expected outcome is a
 * red "Command crashed: ... - recovered" line and a working prompt, with one
 * "!KR:" line (and no "!EX:") on the serial log.
 * ============================================================================ */

#include "crashtest.h"
#include "kguard.h"
#include "vga.h"
#include "string.h"
#include "window.h"
#include "desktop.h"
#include "framebuffer.h"
#include "paging.h"

extern uint8_t _text_start[];

#define RED  VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)
#define CYAN VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK)
#define GREY VGA_COLOR(VGA_DARK_GREY, VGA_BLACK)

/* Unbounded recursion with a real frame: ~272 bytes per level, so the 1MB
 * boot stack is exhausted after ~3800 levels and the next push lands in the
 * unmapped guard band -> #PF whose frame cannot be pushed -> #DF task gate. */
static int recurse(volatile int n) {
    volatile char pad[256];
    pad[0] = (char)n;
    pad[255] = (char)(n >> 8);
    return recurse(n + 1) + pad[0] + pad[255];
}

/* The "app": a window whose content callback writes through a NULL pointer. */
static void crash_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id; (void)cx; (void)cy; (void)cw; (void)ch;
    volatile uint32_t* p = (volatile uint32_t*)0;
    *p = 0xDEADBEEF;                        /* #PF: write to the unmapped NULL page */
}
static void crash_key(int id, char key) { (void)id; (void)key; }

static void usage(void) {
    vga_print_color("  Usage: crashtest <null|gpf|ud|div|wp|stack|app|stats>\n", GREY);
    vga_print("  Each one raises a real ring-0 CPU exception; kguard must contain it.\n");
}

void cmd_crashtest(int argc, char* argv[]) {
    if (argc < 2) { usage(); return; }
    const char* w = argv[1];

    if (strcmp(w, "stats") == 0) {
        char b[12];
        vga_print_color("  kguard recovered faults: ", CYAN);
        int_to_str((int)kguard_recovered_count(), b); vga_print(b); vga_print("\n");
        if (kguard_last_int() >= 0) {
            char d[64]; kguard_describe(1 + kguard_last_int(), d, sizeof(d));
            vga_print("  last: "); vga_print(d); vga_print("\n");
        }
        vga_print("  guard band: 0x"); hex_to_str(paging_kstack_guard_base(), b); vga_print(b + 2);
        vga_print(" (16KB) below the boot stack; page 0 unmapped; .text/.rodata read-only\n");
        return;
    }

    vga_print_color("  crashtest: raising ", CYAN);
    vga_print(w);
    vga_print(" in ring 0...\n");
    vga_flush();

    if (strcmp(w, "null") == 0) {
        volatile uint32_t v = *(volatile uint32_t*)0;     /* #PF (not present, read) */
        char b[12]; hex_to_str(v, b);
        vga_print_color("  NOT CONTAINED: read page 0 = ", RED); vga_print(b); vga_print("\n");
    } else if (strcmp(w, "gpf") == 0) {
        /* Selector 0x53 (index 10) is past the 7-entry GDT -> #GP(0x50). */
        __asm__ volatile("movw $0x53, %%ax\n\tmovw %%ax, %%ds" ::: "ax", "memory");
        vga_print_color("  NOT CONTAINED: bad selector loaded\n", RED);
    } else if (strcmp(w, "ud") == 0) {
        __asm__ volatile("ud2");                            /* #UD */
        vga_print_color("  NOT CONTAINED: ud2 returned\n", RED);
    } else if (strcmp(w, "div") == 0) {
        volatile int zero = 0;
        volatile int r = 1 / zero;                          /* #DE */
        char b[12]; int_to_str(r, b);
        vga_print_color("  NOT CONTAINED: 1/0 = ", RED); vga_print(b); vga_print("\n");
    } else if (strcmp(w, "wp") == 0) {
        *(volatile uint8_t*)_text_start = 0;                /* #PF (present, write, kernel) */
        vga_print_color("  NOT CONTAINED: kernel .text is writable!\n", RED);
    } else if (strcmp(w, "stack") == 0) {
        int r = recurse(0);                                 /* #PF on guard -> #DF */
        char b[12]; int_to_str(r, b);
        vga_print_color("  NOT CONTAINED: recursion returned ", RED); vga_print(b); vga_print("\n");
    } else if (strcmp(w, "app") == 0) {
        /* Like the other GUI launcher commands: enter the desktop. The window
         * faults on its first compose; window.c closes it and posts toasts. */
        int c = fb_is_vesa() ? (int)fb_get_width() / 16 : 80;
        int r = fb_is_vesa() ? (int)fb_get_height() / 16 : 25;
        int id = window_create("Crash Test", c / 2 - 20, r / 2 - 7, 40, 14, crash_draw, crash_key);
        if (id < 0) { vga_print_color("  No free window slot.\n", RED); return; }
        vga_print_color("  Launching desktop with the crashing app (F12 returns)...\n", CYAN);
        desktop_run();
        vga_print_color("\n  Returned from desktop.\n\n", GREY);
    } else {
        usage();
    }
}
