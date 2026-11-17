/* ============================================================================
 * NexusOS — PS/2 Keyboard Driver (Header)
 * ============================================================================ */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

/* Keyboard I/O ports */
#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* Key buffer size */
#define KEY_BUFFER_SIZE 256

/* Special key codes */
#define KEY_BACKSPACE  0x0E
#define KEY_ENTER      0x1C
#define KEY_LSHIFT     0x2A
#define KEY_RSHIFT     0x36
#define KEY_CAPS_LOCK  0x3A
#define KEY_LCTRL      0x1D
#define KEY_LALT       0x38
#define KEY_TAB        0x0F
#define KEY_ESCAPE     0x01
#define KEY_UP         0x48
#define KEY_DOWN       0x50
#define KEY_LEFT       0x4B
#define KEY_RIGHT      0x4D

/* Translated codes for non-ASCII keys (values >= 0x80 in the key buffer).
 * Arrows 0x80-0x83 (up, down, left, right); Ctrl+arrows 0x84-0x87;
 * Ctrl+F5..F8 0x88-0x8B; F1..F12 0x90-0x9B. */
#define KEYC_UP         0x80
#define KEYC_DOWN       0x81
#define KEYC_LEFT       0x82
#define KEYC_RIGHT      0x83
#define KEYC_SNAP_LEFT  0x84
#define KEYC_SNAP_RIGHT 0x85
#define KEYC_MAXIMIZE   0x86
#define KEYC_RESTORE    0x87
#define KEYC_DESKTOP(n) (0x88 + (n))      /* n = 0..3 */
#define KEYC_F(n)       (0x8F + (n))      /* n = 1..12 */
#define KEYC_SPOTLIGHT  0x9C              /* Ctrl+Space or Win+Space */
#define KEYC_DELETE     0x7F

/* Raw scancode hook (Phase 42 gaming) — sees every make/break code before
 * ASCII translation. `ext` = E0-prefixed. Return true to consume the key
 * (it is then NOT translated into the character buffer). Runs in IRQ1
 * context — keep it short. Modifier state (shift/ctrl/caps) is tracked
 * regardless of consumption so the shell is consistent afterwards. */
typedef bool (*keyboard_raw_hook_t)(uint8_t scancode, bool ext, bool released);
void keyboard_set_raw_hook(keyboard_raw_hook_t hook);

/* Initialize keyboard (register IRQ1 handler) */
void keyboard_init(void);

/* Get a character from the key buffer (blocking) */
char keyboard_getchar(void);

/* Check if a key is available in the buffer */
bool keyboard_has_key(void);

/* Read a line of input into buffer (blocks until Enter) */
int keyboard_readline(char* buffer, int max_len);

/* Phase 45: inject a translated character into the key buffer as if typed
 * locally (used by the VNC server to deliver remote KeyEvents). */
void keyboard_inject_char(char c);

/* Phase 45: idle hook invoked while keyboard_getchar() blocks waiting for a
 * key. The shell installs one that pumps backgrounded network servers
 * (net_poll/vnc_poll/sync_poll) so remote desktop / sync stay alive at the
 * text prompt. Pass NULL to clear. Keep the hook short and non-blocking. */
typedef void (*keyboard_idle_hook_t)(void);
void keyboard_set_idle_hook(keyboard_idle_hook_t hook);

#endif /* KEYBOARD_H */
