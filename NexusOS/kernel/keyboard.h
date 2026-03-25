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

/* Initialize keyboard (register IRQ1 handler) */
void keyboard_init(void);

/* Get a character from the key buffer (blocking) */
char keyboard_getchar(void);

/* Check if a key is available in the buffer */
bool keyboard_has_key(void);

/* Read a line of input into buffer (blocks until Enter) */
int keyboard_readline(char* buffer, int max_len);

#endif /* KEYBOARD_H */
