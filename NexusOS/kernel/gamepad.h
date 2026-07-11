/* ============================================================================
 * NexusOS — Game Controller Layer (Header) — Phase 42
 * ============================================================================
 * Presents the PS/2 keyboard as a 12-button SNES-style game controller for
 * the gaming framework (game.c). Hooks the keyboard driver at scancode level
 * so games get true held-key state and release events — the character buffer
 * only ever sees presses. While a game holds the pad (gamepad_acquire) the
 * keys are consumed, so nothing leaks into the shell input line.
 *
 * Buttons map from several keys each (arrows + WASD for the d-pad, etc.) so
 * both classic arrow-key and FPS-style WASD layouts work out of the box.
 * A USB HID gamepad would register the same way via gamepad_feed() — the
 * button state is source-agnostic.
 * ============================================================================ */

#ifndef GAMEPAD_H
#define GAMEPAD_H

#include "types.h"

/* Button ids (SNES-style layout) */
enum {
    PAD_UP = 0, PAD_DOWN, PAD_LEFT, PAD_RIGHT,    /* d-pad: arrows / WASD   */
    PAD_A,                                         /* fire:  Ctrl / X        */
    PAD_B,                                         /* alt:   Space / Z       */
    PAD_X,                                         /* item:  C               */
    PAD_Y,                                         /* map:   V / M           */
    PAD_L,                                         /* strafe left:  A / Q    */
    PAD_R,                                         /* strafe right: D / E    */
    PAD_START,                                     /* Enter                  */
    PAD_SELECT,                                    /* Esc / Tab (menu/quit)  */
    PAD_BTN_COUNT
};

/* SDL-style button event */
typedef struct {
    uint8_t btn;        /* PAD_* id                  */
    bool    down;       /* true = press, false = release */
} pad_event_t;

/* Install the keyboard hook (call once at boot, after keyboard_init). */
void gamepad_init(void);

/* Claim/release the controller for a game. While acquired, mapped keys are
 * consumed (never reach the shell) and state/events are tracked. Release
 * clears all state and drains any stray characters from the key buffer. */
void gamepad_acquire(void);
void gamepad_release(void);
bool gamepad_acquired(void);

/* Latch the per-frame edge state — call exactly once per game frame,
 * before testing gamepad_pressed(). */
void gamepad_poll(void);

uint16_t gamepad_buttons(void);          /* bitmask of held PAD_* (1<<btn)  */
bool gamepad_held(int btn);              /* level: held right now           */
bool gamepad_pressed(int btn);           /* edge: went down since last poll */

/* Drain the SDL-style event queue (returns false when empty). */
bool gamepad_next_event(pad_event_t* ev);

/* Raw access: is a specific scancode held? (ext = E0-prefixed) */
bool gamepad_key_held(uint8_t scancode, bool ext);

/* Inject a button transition from another input source (e.g. USB HID). */
void gamepad_feed(uint8_t btn, bool down);

/* Raw-scancode observer (IRQ1 context). gamepad_init() installs this as the
 * keyboard raw hook by default, but a layer that needs to own the single hook
 * slot (the Phase 43 accessibility module) can forward every scancode here
 * instead. Returns true when the pad consumes the key (a game owns the pad). */
bool gamepad_raw_observe(uint8_t scancode, bool ext, bool released);

const char* gamepad_name(void);          /* active controller description   */
const char* gamepad_btn_name(int btn);
const char* gamepad_btn_keys(int btn);   /* human-readable key mapping      */

#endif /* GAMEPAD_H */
