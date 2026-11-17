/* ============================================================================
 * NexusOS — Mobile / Embedded Mode — Phase 47
 * ============================================================================
 * See mobile.h. Integer-only, freestanding. The gesture recognizer observes
 * the shared PS/2 mouse state (mouse_get_state) each frame and classifies a
 * pointer-down -> move -> up sequence into tap / long-press / drag / swipe.
 * It only READS mouse state, so existing desktop click handling is untouched.
 * ============================================================================ */

#include "mobile.h"
#include "mouse.h"
#include "framebuffer.h"
#include "vga.h"
#include "string.h"

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Tunables
 * -------------------------------------------------------------------------- */
#define TAP_MAX_TICKS     9    /* press shorter than this (and ~still) = tap   */
#define LONGPRESS_TICKS   14   /* held longer than this, ~still = long-press   */
#define MOVE_THRESHOLD    12   /* px of motion that makes it a drag/swipe      */
#define SWIPE_THRESHOLD   60   /* px of net travel that makes a drag a swipe   */
#define REDRAW_NORMAL     18   /* ~1s idle refresh (matches desktop default)   */
#define REDRAW_LOWPOWER   72   /* ~4s idle refresh in low-power mode           */

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */
static bool          touch_on   = false;
static orientation_t orient     = ORIENT_LANDSCAPE;
static bool          lowpower   = false;

/* gesture recognizer */
static bool      pressing   = false;   /* a pointer-down is in progress       */
static int       down_x, down_y;       /* where the press started (px)        */
static uint32_t  down_tick;            /* when it started                     */
static int       max_travel;           /* peak distance from the start        */
static gesture_t last_gesture = GESTURE_NONE;
static int       last_gx, last_gy;
static uint32_t  gesture_total = 0;

static int iabs(int v) { return v < 0 ? -v : v; }

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */
void mobile_init(void) {
    touch_on = false;
    orient = ORIENT_LANDSCAPE;
    lowpower = false;
    pressing = false;
    last_gesture = GESTURE_NONE;
    gesture_total = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Mobile/embedded mode ready (touch/orientation/power/arm)\n");
}

/* --------------------------------------------------------------------------
 * Pillar 1: gesture recognizer
 * -------------------------------------------------------------------------- */
static void emit(gesture_t g, int x, int y) {
    last_gesture = g;
    last_gx = x; last_gy = y;
    if (g != GESTURE_NONE) gesture_total++;
}

/* Classify a completed press at release time. */
static void classify_release(int up_x, int up_y, uint32_t held) {
    int dx = up_x - down_x;
    int dy = up_y - down_y;
    int adx = iabs(dx), ady = iabs(dy);

    if (max_travel < MOVE_THRESHOLD) {
        /* barely moved: tap vs long-press by duration */
        if (held >= LONGPRESS_TICKS) emit(GESTURE_LONG_PRESS, up_x, up_y);
        else                          emit(GESTURE_TAP, up_x, up_y);
        return;
    }
    /* moved: swipe if net travel is large (use the same Manhattan metric as
     * max_travel so diagonal swipes count too), classified by the dominant
     * axis; otherwise it's a drag. */
    if (adx + ady >= SWIPE_THRESHOLD) {
        if (adx >= ady) emit(dx > 0 ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT, up_x, up_y);
        else            emit(dy > 0 ? GESTURE_SWIPE_DOWN  : GESTURE_SWIPE_UP,   up_x, up_y);
    } else {
        emit(GESTURE_DRAG, up_x, up_y);
    }
}

void mobile_poll(void) {
    if (!touch_on) return;

    mouse_state_t m = mouse_get_state();
    bool down = (m.buttons & MOUSE_LEFT) != 0;
    int px = m.px, py = m.py;

    if (down && !pressing) {
        /* pointer down */
        pressing = true;
        down_x = px; down_y = py;
        down_tick = system_ticks;
        max_travel = 0;
    } else if (down && pressing) {
        /* pointer move: track peak travel */
        int d = iabs(px - down_x) + iabs(py - down_y);
        if (d > max_travel) max_travel = d;
    } else if (!down && pressing) {
        /* pointer up: classify */
        pressing = false;
        uint32_t held = system_ticks - down_tick;
        int d = iabs(px - down_x) + iabs(py - down_y);
        if (d > max_travel) max_travel = d;
        classify_release(px, py, held);
    }
}

gesture_t mobile_last_gesture(void) { return last_gesture; }

gesture_t mobile_gesture_take(int* x, int* y) {
    gesture_t g = last_gesture;
    if (x) *x = last_gx;
    if (y) *y = last_gy;
    last_gesture = GESTURE_NONE;
    return g;
}

const char* mobile_gesture_name(gesture_t g) {
    switch (g) {
        case GESTURE_TAP:         return "tap";
        case GESTURE_LONG_PRESS:  return "long-press";
        case GESTURE_DRAG:        return "drag";
        case GESTURE_SWIPE_UP:    return "swipe-up";
        case GESTURE_SWIPE_DOWN:  return "swipe-down";
        case GESTURE_SWIPE_LEFT:  return "swipe-left";
        case GESTURE_SWIPE_RIGHT: return "swipe-right";
        default:                  return "none";
    }
}

void mobile_set_touch(bool on) { touch_on = on; if (!on) pressing = false; }
bool mobile_touch_on(void)     { return touch_on; }
uint32_t mobile_gesture_count(void) { return gesture_total; }

/* --------------------------------------------------------------------------
 * Pillar 2: responsive / orientation
 * -------------------------------------------------------------------------- */
void mobile_set_orientation(orientation_t o) { orient = o; }
orientation_t mobile_orientation(void)       { return orient; }
bool mobile_is_portrait(void)                { return orient == ORIENT_PORTRAIT; }

int mobile_screen_w(void) {
    int w = (int)fb_get_width(), h = (int)fb_get_height();
    return (orient == ORIENT_PORTRAIT) ? h : w;
}
int mobile_screen_h(void) {
    int w = (int)fb_get_width(), h = (int)fb_get_height();
    return (orient == ORIENT_PORTRAIT) ? w : h;
}

/* --------------------------------------------------------------------------
 * Pillar 3: low-power mode
 * -------------------------------------------------------------------------- */
void mobile_set_lowpower(bool on) { lowpower = on; }
bool mobile_lowpower_on(void)     { return lowpower; }
int  mobile_redraw_interval(void) { return lowpower ? REDRAW_LOWPOWER : REDRAW_NORMAL; }

/* --------------------------------------------------------------------------
 * Pillar 4: ARM / embedded status (honest)
 * -------------------------------------------------------------------------- */
void mobile_arm_status(void) {
    vga_print_color("\n  ARM / Embedded Target Status\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ============================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  Current build target: ");
    vga_print_color("i686-elf (x86, 32-bit)\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("  ARM toolchain present: ");
    vga_print_color("no\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("\n  A real ARM (AArch32/64) port would require:\n");
    vga_print_color("   - an arm-none-eabi / aarch64-elf cross toolchain\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   - an ARM boot path (no BIOS/VBE; device tree + UART)\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   - rewriting x86 asm: GDT/IDT/PIC, port I/O, cli/hlt,\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("     context switch, paging -> GIC, MMIO, WFI, TTBR/MMU\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   - a framebuffer driver for the ARM platform (e.g. virt)\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  The C kernel is largely portable; the HAL is the work.\n\n");
}
