/* ============================================================================
 * NexusOS — POSIX Output Capture — Phase 31
 * ============================================================================
 * Redirects vga_print output into a buffer for pipe/redirect operators.
 * When capturing is active, vga_print calls posix_capture_write().
 * ============================================================================ */

#include "posix.h"
#include "vga.h"
#include "string.h"

/* Capture state */
static bool capturing = false;
static char capture_buf[POSIX_CAPTURE_SIZE];
static int  capture_len = 0;

/* --------------------------------------------------------------------------
 * posix_init
 * -------------------------------------------------------------------------- */
void posix_init(void) {
    capturing = false;
    capture_len = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("POSIX layer initialized\n");
}

/* --------------------------------------------------------------------------
 * posix_capture_start
 * -------------------------------------------------------------------------- */
void posix_capture_start(void) {
    capture_len = 0;
    capture_buf[0] = '\0';
    capturing = true;
}

/* --------------------------------------------------------------------------
 * posix_capture_stop
 * -------------------------------------------------------------------------- */
void posix_capture_stop(void) {
    capturing = false;
}

/* --------------------------------------------------------------------------
 * posix_capture_get
 * -------------------------------------------------------------------------- */
const char* posix_capture_get(int* len) {
    if (len) *len = capture_len;
    return capture_buf;
}

/* --------------------------------------------------------------------------
 * posix_capture_clear
 * -------------------------------------------------------------------------- */
void posix_capture_clear(void) {
    capture_len = 0;
    capture_buf[0] = '\0';
}

/* --------------------------------------------------------------------------
 * posix_is_capturing
 * -------------------------------------------------------------------------- */
bool posix_is_capturing(void) {
    return capturing;
}

/* --------------------------------------------------------------------------
 * posix_capture_write: Append string to capture buffer
 * -------------------------------------------------------------------------- */
void posix_capture_write(const char* s) {
    while (*s && capture_len < POSIX_CAPTURE_SIZE - 1) {
        capture_buf[capture_len++] = *s++;
    }
    capture_buf[capture_len] = '\0';
}
