/* ============================================================================
 * NexusOS — POSIX Output Capture Header — Phase 31
 * ============================================================================
 * Captures VGA output into a buffer for pipe/redirect operations.
 * ============================================================================ */

#ifndef POSIX_H
#define POSIX_H

#include "types.h"

/* Capture buffer size */
#define POSIX_CAPTURE_SIZE 4096

/* Initialize POSIX helpers */
void posix_init(void);

/* Start capturing VGA output into internal buffer */
void posix_capture_start(void);

/* Stop capturing, return to normal VGA output */
void posix_capture_stop(void);

/* Get captured output buffer and length */
const char* posix_capture_get(int* len);

/* Clear capture buffer */
void posix_capture_clear(void);

/* Check if currently capturing */
bool posix_is_capturing(void);

/* Write string to capture buffer (called by vga_print when capturing) */
void posix_capture_write(const char* s);

#endif /* POSIX_H */
