/* ============================================================================
 * NexusOS — Accessibility Subsystem (Implementation) — Phase 43
 * ============================================================================
 * See accessibility.h for the design overview and the four pillars.
 * ============================================================================ */

#include "accessibility.h"
#include "speaker.h"
#include "keyboard.h"
#include "gamepad.h"
#include "theme.h"
#include "font.h"
#include "vga.h"

/* ----------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static bool reader_on        = false;   /* screen reader master switch        */
static bool contrast_on      = false;   /* high-contrast theme active         */
static int  prev_theme       = THEME_NEXUS_DARK; /* theme to restore on toggle */

/* Modifier tracking at scancode level. The PS/2 driver tracks shift/ctrl but
 * NOT alt, and it updates its own state only AFTER our hook runs — so to make
 * the Alt+Shift chords reliable we track all three ourselves here. */
static bool mod_shift = false;
static bool mod_alt   = false;

/* Pending audible cue, set from the IRQ hook and drained in accessibility_poll
 * (because beep() blocks and must never run in interrupt context). 0 = none. */
static volatile int  pending_event = -1;   /* an acc_event_t, or -1           */
static volatile bool pending_announce = false; /* announce full status        */

/* PS/2 set-1 make codes we care about (high bit already stripped by the hook) */
#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_LALT    0x38     /* AltGr arrives E0-prefixed; left Alt is plain    */
#define SC_C       0x2E
#define SC_S       0x1F
#define SC_A       0x1E
#define SC_EQUALS  0x0D     /* '=' / '+' key                                   */
#define SC_MINUS   0x0C     /* '-' / '_' key                                   */

/* ----------------------------------------------------------------------------
 * Screen reader — earcons & spell-out (PC speaker, no FPU)
 * --------------------------------------------------------------------------
 * Tones are PIT square waves via beep(freq_hz, duration_ticks); one tick is
 * ~55 ms, so durations here are 1–3 ticks. beep() BLOCKS for the duration, so
 * everything below must run in normal (non-IRQ) context.
 * -------------------------------------------------------------------------- */

/* Per-event earcon: {frequency Hz, duration ticks}. Distinct shapes so users
 * learn them by ear (low buzz = error, rising pair = open, etc.). */
typedef struct { uint32_t f1, d1, f2, d2; } earcon_t;
static const earcon_t earcons[ACC_EV_COUNT] = {
    /* FOCUS      */ { 880,  1,    0, 0 },   /* single mid click                */
    /* ACTIVATE   */ { 1320, 1,    0, 0 },   /* bright confirm                  */
    /* BOUNDARY   */ { 440,  1,  440, 1 },   /* double low — "can't go further" */
    /* ERROR      */ { 196,  2,    0, 0 },   /* low buzz                        */
    /* OPEN       */ { 660,  1,  990, 1 },   /* rising pair                     */
    /* CLOSE      */ { 990,  1,  660, 1 },   /* falling pair                    */
    /* TOGGLE_ON  */ { 784,  1, 1175, 1 },   /* up — on                         */
    /* TOGGLE_OFF */ { 1175, 1,  784, 1 },   /* down — off                      */
};

static void play_earcon(acc_event_t ev) {
    if ((int)ev < 0 || ev >= ACC_EV_COUNT) return;
    const earcon_t* e = &earcons[ev];
    if (e->f1) beep(e->f1, e->d1);
    if (e->f2) beep(e->f2, e->d2);
    beep_stop();
}

/* Map a character to a stable, audibly-distinct frequency. Letters are placed
 * on a rising scale across two octaves; digits sit just above; everything else
 * gets a mid tone. Case is folded so "A" and "a" sound the same. */
static uint32_t char_pitch(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') {
        /* 262 Hz (C4) up to ~990 Hz across the 26 letters */
        int n = c - 'A';
        return 262u + (uint32_t)n * 28u;
    }
    if (c >= '0' && c <= '9') {
        int n = c - '0';
        return 1046u + (uint32_t)n * 40u;   /* high register for digits */
    }
    return 330u;   /* punctuation / symbols — neutral E4 */
}

void acc_event(acc_event_t ev) {
    if (!reader_on) return;
    play_earcon(ev);
}

static void say_impl(const char* text) {
    if (!text) return;
    /* Start cue: short rising chirp. */
    beep(523, 1); beep(784, 1);
    for (const char* p = text; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n') {
            beep_stop();
            /* brief silence = word gap; a 1-tick rest via a sub-audible tone */
            beep(40, 1);
            continue;
        }
        beep(char_pitch(c), 1);
    }
    beep_stop();
    /* End cue: short falling chirp. */
    beep(784, 1); beep(523, 1);
    beep_stop();
}

void acc_say(const char* text) {
    if (!reader_on) return;
    say_impl(text);
}

void acc_say_force(const char* text) {
    say_impl(text);
}

void accessibility_set_reader(bool on) {
    reader_on = on;
}
bool accessibility_reader_on(void) { return reader_on; }

/* ----------------------------------------------------------------------------
 * High contrast
 * -------------------------------------------------------------------------- */

bool accessibility_toggle_contrast(void) {
    if (!contrast_on) {
        prev_theme = theme_get_index();
        if (prev_theme == THEME_HICON) prev_theme = THEME_NEXUS_DARK;
        theme_set(THEME_HICON);
        contrast_on = true;
    } else {
        theme_set(prev_theme);
        contrast_on = false;
    }
    return contrast_on;
}
bool accessibility_contrast_on(void) {
    /* Stay honest if the user changed theme by another path. */
    contrast_on = (theme_get_index() == THEME_HICON);
    return contrast_on;
}

/* ----------------------------------------------------------------------------
 * Font scaling
 * -------------------------------------------------------------------------- */

int accessibility_font_larger(void) {
    int s = font_get_scale() + 1;
    font_set_scale(s);
    return font_get_scale();
}
int accessibility_font_smaller(void) {
    int s = font_get_scale() - 1;
    font_set_scale(s < 1 ? 1 : s);
    return font_get_scale();
}
int accessibility_font_scale(void) { return font_get_scale(); }

/* ----------------------------------------------------------------------------
 * Global hotkey raw hook (IRQ1 context — STATE ONLY, no sound, no drawing)
 * --------------------------------------------------------------------------
 * Owns the keyboard's single raw-hook slot and forwards every scancode to the
 * gamepad so the controller layer keeps working. Accessibility chords are
 * Alt+Shift+<key>; they are swallowed (consumed) so they never leak into the
 * shell, but only when no game owns the pad.
 * -------------------------------------------------------------------------- */

static bool acc_raw_hook(uint8_t scancode, bool ext, bool released) {
    /* Track our own modifier state first (the driver doesn't track Alt and
     * updates shift only after us). Modifiers are never E0-extended here for
     * the plain left Alt / either Shift. */
    if (!ext) {
        if (scancode == SC_LSHIFT || scancode == SC_RSHIFT) mod_shift = !released;
        if (scancode == SC_LALT)                            mod_alt   = !released;
    }

    /* Let the controller layer see the key regardless. If a game owns the pad
     * it consumes the key and we must NOT also act on it. */
    bool pad_consumed = gamepad_raw_observe(scancode, ext, released);
    if (pad_consumed) return true;

    /* Accessibility chords fire on key-DOWN of the action key while Alt+Shift
     * are both held. Everything else falls through untouched. */
    if (released || ext || !(mod_alt && mod_shift)) return false;

    switch (scancode) {
        case SC_C:                     /* Alt+Shift+C — toggle high contrast    */
            accessibility_toggle_contrast();
            pending_event = contrast_on ? ACC_EV_TOGGLE_ON : ACC_EV_TOGGLE_OFF;
            return true;
        case SC_S:                     /* Alt+Shift+S — toggle screen reader    */
            reader_on = !reader_on;
            /* Always cue the transition so the user hears the new state even
             * if turning the reader off. */
            pending_event = reader_on ? ACC_EV_TOGGLE_ON : ACC_EV_TOGGLE_OFF;
            return true;
        case SC_EQUALS:                /* Alt+Shift+'=' — text larger           */
            accessibility_font_larger();
            pending_event = ACC_EV_FOCUS;
            return true;
        case SC_MINUS:                 /* Alt+Shift+'-' — text smaller          */
            accessibility_font_smaller();
            pending_event = ACC_EV_FOCUS;
            return true;
        case SC_A:                     /* Alt+Shift+A — announce status         */
            pending_announce = true;
            return true;
        default:
            return false;
    }
}

/* ----------------------------------------------------------------------------
 * Poll — drain pending cues in normal context (called from main loops)
 * -------------------------------------------------------------------------- */

void accessibility_poll(void) {
    int ev = pending_event;
    if (ev >= 0) {
        pending_event = -1;
        /* Earcons for state toggles play even while the reader master switch
         * is off — they confirm the hotkey itself worked. */
        play_earcon((acc_event_t)ev);
    }
    if (pending_announce) {
        pending_announce = false;
        say_impl(reader_on ? "reader on" : "reader off");
    }
}

/* ----------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */

void accessibility_init(void) {
    reader_on = false;
    contrast_on = (theme_get_index() == THEME_HICON);
    prev_theme = THEME_NEXUS_DARK;
    mod_shift = mod_alt = false;
    pending_event = -1;
    pending_announce = false;

    /* Take ownership of the keyboard raw-hook slot, chaining the gamepad. */
    keyboard_set_raw_hook(acc_raw_hook);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Accessibility ready (hicon theme, font scale, screen reader, hotkeys)\n");
}
