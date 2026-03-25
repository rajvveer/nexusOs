/* ============================================================================
 * NexusOS — HTTP Server Header — Phase 30
 * ============================================================================
 * Simple HTTP server serving a built-in NexusOS status page.
 * ============================================================================ */

#ifndef HTTPD_H
#define HTTPD_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define HTTPD_PORT          8080
#define HTTPD_MAX_REQUEST   512

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize HTTP server */
void httpd_init(void);

/* Start listening */
void httpd_start(void);

/* Stop the server */
void httpd_stop(void);

/* Poll for connections (call periodically) */
void httpd_poll(void);

/* Check if running */
bool httpd_is_running(void);

#endif /* HTTPD_H */
