/* ============================================================================
 * NexusOS — Accessibility Subsystem (Header) — Phase 43
 * ============================================================================
 * Era 6 (Polish) opener. Four pillars of inclusive design, built on top of
 * subsystems that already existed in the kernel:
 *
 *   1. Screen reader via the PC speaker (speaker.c `beep`) — audible feedback
 *      for UI events and an audible "spell-out" of arbitrary text. Distinct
 *      pitches per character give a consistent, headlessly-capturable channel
 *      (no TTS engine, no FPU — pure PIT square waves).
 *   2. High-contrast theme — selectable through the theme engine (theme.c,
 *      the "hicon" theme) and toggled instantly with a global hotkey.
 *   3. Font scaling — drives the font engine's global scale (font_set_scale)
 *      so all UI text enlarges 1x–4x for low-vision users.
 *   4. Keyboard-only navigation + global hotkeys — a raw-scancode hook (owning
 *      the keyboard's single hook slot and chaining to the gamepad) catches
 *      accessibility chords (Alt+Shift+...) regardless of which window or app
 *      has focus.
 *
 * IRQ SAFETY: the raw hook runs in IRQ1 context and `beep()` BLOCKS, so the
 * hook never produces sound itself — it only flips state and arms a pending
 * audio cue. accessibility_poll(), called from the desktop/shell main loops,
 * drains the cue in normal (interruptible) context.
 * ============================================================================ */

#ifndef ACCESSIBILITY_H
#define ACCESSIBILITY_H

#include "types.h"

/* UI event categories — map to distinct earcons (short speaker tones). */
typedef enum {
    ACC_EV_FOCUS = 0,   /* focus moved to a new element        */
    ACC_EV_ACTIVATE,    /* element activated (Enter/Space)     */
    ACC_EV_BOUNDARY,    /* hit the edge of a list / no-op move */
    ACC_EV_ERROR,       /* error / rejected action             */
    ACC_EV_OPEN,        /* a window / menu opened              */
    ACC_EV_CLOSE,       /* a window / menu closed              */
    ACC_EV_TOGGLE_ON,   /* a setting turned on                 */
    ACC_EV_TOGGLE_OFF,  /* a setting turned off                */
    ACC_EV_COUNT
} acc_event_t;

/* Install the accessibility layer: claims the keyboard raw-hook slot (chaining
 * to the gamepad) and arms the global hotkeys. Call once at boot, AFTER
 * gamepad_init() and speaker_init(). */
void accessibility_init(void);

/* Drain any pending audible cue queued by the IRQ hook. Call once per frame
 * from a non-IRQ main loop (desktop_run / shell). Cheap when nothing pending. */
void accessibility_poll(void);

/* --- Screen reader --------------------------------------------------------- */

/* Master enable for the screen reader. When off, acc_event()/acc_say() are
 * no-ops (so the rest of the system can call them unconditionally). */
void accessibility_set_reader(bool on);
bool accessibility_reader_on(void);

/* Emit the earcon for a UI event (no-op if the reader is off). Safe to call
 * from normal context; it produces a short blocking beep. */
void acc_event(acc_event_t ev);

/* Audibly "spell" a string: a short rising start cue, one distinct pitch per
 * character (letters/digits/punctuation each map to a stable frequency,
 * spaces become a brief silence), then a falling end cue. Blocking; intended
 * for explicit announcements (the `say` command, focus labels), not IRQ
 * context. No-op if the reader is off. */
void acc_say(const char* text);

/* Speak a string even if the reader master switch is off (used by the `say`
 * shell command so it always demonstrates). */
void acc_say_force(const char* text);

/* --- High contrast --------------------------------------------------------- */

/* Toggle between the high-contrast theme and the previously-active theme.
 * Returns true if high contrast is now active. */
bool accessibility_toggle_contrast(void);
bool accessibility_contrast_on(void);

/* --- Font scaling ---------------------------------------------------------- */

/* Adjust the global UI text scale by +1 / -1 (clamped 1..4). Returns the new
 * scale. Thin wrappers over font_set_scale so callers needn't include font.h. */
int accessibility_font_larger(void);
int accessibility_font_smaller(void);
int accessibility_font_scale(void);

#endif /* ACCESSIBILITY_H */
