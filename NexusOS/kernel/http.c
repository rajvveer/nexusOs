/* ============================================================================
 * NexusOS — HTTP Client — Phase 28 / 57
 * ============================================================================
 * http_get() is kept for the existing callers (browser.c, pkg.c, desktop.c)
 * and is now a thin wrapper over ns_fetch() (netstream.c): streaming,
 * chunked decoding, redirects, HTTPS via the TLS 1.3 client, 16 KB body cap.
 * ============================================================================ */

#include "http.h"
#include "netstream.h"
#include "string.h"
#include "vga.h"

/* --------------------------------------------------------------------------
 * http_init: Initialize HTTP client
 * -------------------------------------------------------------------------- */
void http_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("HTTP client initialized (streaming, TLS 1.3)\n");
}

/* --------------------------------------------------------------------------
 * http_get: Perform an HTTP(S) GET request
 * -------------------------------------------------------------------------- */
http_response_t http_get(const char* url) {
    http_response_t resp;
    ns_info_t info;
    memset(&resp, 0, sizeof(resp));

    int n = ns_fetch(url, (uint8_t*)resp.body, HTTP_MAX_BODY - 1, &info);
    resp.status_code    = info.status;
    resp.content_length = info.content_length;
    if (n < 0) {
        resp.body[0] = '\0';
        resp.body_len = 0;
        resp.success = false;
        return resp;
    }
    resp.body_len = (uint16_t)n;
    resp.body[n] = '\0';
    resp.success = (info.status >= 200 && info.status < 400);
    return resp;
}
