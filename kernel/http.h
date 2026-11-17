/* ============================================================================
 * NexusOS — HTTP Client Header — Phase 28 / 57
 * ============================================================================
 * Legacy blocking GET API, now a thin wrapper over the streaming client in
 * netstream.c (http:// and https://, redirects followed, 16 KB body cap).
 * ============================================================================ */

#ifndef HTTP_H
#define HTTP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define HTTP_DEFAULT_PORT   80
#define HTTP_MAX_URL        512
#define HTTP_MAX_HOST       128
#define HTTP_MAX_PATH       512
#define HTTP_MAX_BODY       16384   /* Max response body we store (Phase 57: was 2 KB) */
#define HTTP_TIMEOUT_TICKS  (30 * 18)
#define HTTP_CONNECT_TICKS  (10 * 18)

/* --------------------------------------------------------------------------
 * HTTP Response
 * -------------------------------------------------------------------------- */
typedef struct {
    int      status_code;           /* 200, 404, etc. */
    uint32_t content_length;        /* From Content-Length header, if present */
    char     body[HTTP_MAX_BODY];   /* Response body (truncated if too large) */
    uint16_t body_len;              /* Actual body bytes received */
    bool     success;               /* True if we got a valid response */
} http_response_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize HTTP client */
void http_init(void);

/* Perform HTTP(S) GET request (URL like "http://host/path" or "https://...") */
http_response_t http_get(const char* url);

#endif /* HTTP_H */
