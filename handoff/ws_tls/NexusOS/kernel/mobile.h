/* ============================================================================
 * NexusOS — Mobile / Embedded Mode (Header) — Phase 47
 * ============================================================================
 * Era 6 (Polish). The four roadmap pillars, scoped honestly for a QEMU i386
 * target (no real touchscreen, no ARM CPU here):
 *
 *   1. Touch input — a gesture-recognition layer that interprets the existing
 *      PS/2 mouse as a touch surface: tap, long-press, drag, and swipe
 *      (up/down/left/right). Apps/desktop can poll the last gesture. This is a
 *      real input-translation layer (pointer-down/move/up -> gesture), the same
 *      shape a true touch driver would feed.
 *
 *   2. Responsive UI — a logical orientation (landscape/portrait) + a
 *      "mobile layout" flag that layout code can consult via mobile_screen_w()/
 *      mobile_screen_h() (which report the orientation-adjusted logical size)
 *      and mobile_is_portrait(). We do NOT physically rotate the framebuffer
 *      (every blitter assumes width=1024 — a separate large rewrite); this is a
 *      logical orientation model components opt into.
 *
 *   3. Low-power mode — a real reduced-activity state: the desktop throttles its
 *      idle (clock) redraw cadence and the system leans on HLT, cutting the
 *      busy-frame rate way down when idle. Measurable via the frame counter.
 *
 *   4. ARM / embedded target — an honest status report. There is no ARM
 *      toolchain or runtime in this build; `arm` explains exactly what a real
 *      AArch32/AArch64 port would require rather than pretending one exists.
 * ============================================================================ */

#ifndef MOBILE_H
#define MOBILE_H

#include "types.h"

/* Recognized touch gestures. */
typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP,
    GESTURE_LONG_PRESS,
    GESTURE_DRAG,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN,
    GESTURE_SWIPE_LEFT,
    GESTURE_SWIPE_RIGHT,
    GESTURE_COUNT
} gesture_t;

typedef enum { ORIENT_LANDSCAPE = 0, ORIENT_PORTRAIT = 1 } orientation_t;

/* One-time init at boot. */
void mobile_init(void);

/* --- Pillar 1: touch / gestures ------------------------------------------- */
/* Drive the gesture recognizer from the current mouse state. Call once per
 * frame from a non-IRQ loop (desktop / shell idle). Detects pointer
 * down/move/up transitions and emits a gesture when one completes. */
void mobile_poll(void);

/* Last completed gesture (and where it happened, in pixels). Reading it does
 * not clear it; mobile_gesture_take() returns + clears for one-shot consumers. */
gesture_t mobile_last_gesture(void);
gesture_t mobile_gesture_take(int* x, int* y);
const char* mobile_gesture_name(gesture_t g);

/* Enable/disable touch-mode gesture recognition. */
void mobile_set_touch(bool on);
bool mobile_touch_on(void);

/* Count of gestures recognized this session (for mobileinfo / verification). */
uint32_t mobile_gesture_count(void);

/* --- Pillar 2: responsive / orientation ----------------------------------- */
void          mobile_set_orientation(orientation_t o);
orientation_t mobile_orientation(void);
bool          mobile_is_portrait(void);
/* Orientation-adjusted logical screen size (swaps W/H in portrait). Layout
 * code that wants to be responsive reads these instead of fb_get_width/height
 * directly. */
int mobile_screen_w(void);
int mobile_screen_h(void);

/* --- Pillar 3: low-power mode --------------------------------------------- */
void mobile_set_lowpower(bool on);
bool mobile_lowpower_on(void);
/* The desktop idle-redraw cadence in ticks (small = responsive, large =
 * low-power). The desktop consults this for its periodic refresh. */
int  mobile_redraw_interval(void);

/* --- Pillar 4: ARM / embedded status -------------------------------------- */
/* Print an honest report of the (x86) build target and what an ARM port needs. */
void mobile_arm_status(void);

#endif /* MOBILE_H */
