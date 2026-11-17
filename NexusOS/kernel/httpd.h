/* ============================================================================
 * NexusOS — HTTP Server Header — Phase 30 / Phase 55
 * ============================================================================
 * Phase 55: a small HTTP/1.0 (Connection: close) static file server over the
 * RAM filesystem, on port 8080. See httpd.c for the URL map:
 *   /            -> <docroot>/index.html         (docroot defaults to /www)
 *   /<p>         -> <docroot>/<p>  (files; directories list unless index.html)
 *   /status      -> the classic status page      /api/status -> JSON
 *   /fs/<path>   -> ANY ramfs path, HTTP Basic auth against the OS user DB,
 *                   GET/HEAD need read permission, PUT (<= 4 KB) write perm.
 * ============================================================================ */

#ifndef HTTPD_H
#define HTTPD_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define HTTPD_PORT          8080
#define HTTPD_MAX_REQUEST   2048    /* request-line + headers accumulator      */

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize HTTP server (creates /www with index.html + style.css) */
void httpd_init(void);

/* Start listening */
void httpd_start(void);

/* Stop the server */
void httpd_stop(void);

/* Poll for connections (call periodically from a non-IRQ loop) */
void httpd_poll(void);

/* Check if running */
bool httpd_is_running(void);

/* ---- Phase 55 ---- */
/* `httpd status`: requests served, last client, auth failures, docroot. */
void httpd_print_status(void);
/* `httpd root <dir>`: change the document root (must be an existing dir).
 * Returns true on success (prints the outcome either way). */
bool httpd_set_root(const char* path);
const char* httpd_get_root(void);
uint32_t httpd_requests_served(void);

#endif /* HTTPD_H */
