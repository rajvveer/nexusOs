/* ============================================================================
 * NexusOS — Terminal Control (termios) Header — Phase 31
 * ============================================================================
 * Basic POSIX-like terminal I/O control.
 * ============================================================================ */

#ifndef TERMIOS_H
#define TERMIOS_H

#include "types.h"

/* Local flags (c_lflag) */
#define TERMIOS_ECHO    0x01    /* Echo input characters */
#define TERMIOS_ICANON  0x02    /* Canonical mode (line buffered) */
#define TERMIOS_ISIG    0x04    /* Enable signals (Ctrl+C, Ctrl+Z) */

/* Default flags */
#define TERMIOS_DEFAULT (TERMIOS_ECHO | TERMIOS_ICANON | TERMIOS_ISIG)

/* termios structure */
typedef struct {
    uint32_t c_lflag;       /* Local flags */
} termios_t;

/* Initialize terminal control */
void termios_init(void);

/* Get current termios settings */
termios_t* termios_get(void);

/* Set termios settings */
void termios_set(const termios_t* t);

/* Check if in raw mode */
bool termios_is_raw(void);

/* Set raw mode (no echo, no canon) */
void termios_set_raw(void);

/* Restore default mode */
void termios_set_cooked(void);

#endif /* TERMIOS_H */
