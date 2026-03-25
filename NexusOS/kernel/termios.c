/* ============================================================================
 * NexusOS — Terminal Control (termios) — Phase 31
 * ============================================================================
 * Basic terminal I/O control: raw/cooked mode, echo, signal generation.
 * ============================================================================ */

#include "termios.h"
#include "vga.h"
#include "string.h"

/* Current terminal settings */
static termios_t current_termios;

/* --------------------------------------------------------------------------
 * termios_init
 * -------------------------------------------------------------------------- */
void termios_init(void) {
    current_termios.c_lflag = TERMIOS_DEFAULT;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("termios initialized (canonical mode)\n");
}

/* --------------------------------------------------------------------------
 * termios_get
 * -------------------------------------------------------------------------- */
termios_t* termios_get(void) {
    return &current_termios;
}

/* --------------------------------------------------------------------------
 * termios_set
 * -------------------------------------------------------------------------- */
void termios_set(const termios_t* t) {
    if (t) {
        current_termios.c_lflag = t->c_lflag;
    }
}

/* --------------------------------------------------------------------------
 * termios_is_raw
 * -------------------------------------------------------------------------- */
bool termios_is_raw(void) {
    return !(current_termios.c_lflag & TERMIOS_ICANON);
}

/* --------------------------------------------------------------------------
 * termios_set_raw
 * -------------------------------------------------------------------------- */
void termios_set_raw(void) {
    current_termios.c_lflag &= ~(TERMIOS_ECHO | TERMIOS_ICANON);
}

/* --------------------------------------------------------------------------
 * termios_set_cooked
 * -------------------------------------------------------------------------- */
void termios_set_cooked(void) {
    current_termios.c_lflag = TERMIOS_DEFAULT;
}
