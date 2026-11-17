/* ============================================================================
 * NexusOS — HTTP Client Header — Phase 28
 * ============================================================================
 * Simple HTTP/1.1 GET client over TCP.
 * ============================================================================ */

#ifndef HTTP_H
#define HTTP_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define HTTP_DEFAULT_PORT   80
#define HTTP_MAX_URL        128
#define HTTP_MAX_HOST       64
#define HTTP_MAX_PATH       64
#define HTTP_MAX_BODY       2048    /* Max response body we store */
#define HTTP_TIMEOUT_TICKS  54      /* ~3 seconds at 18.2 Hz */
#define HTTP_CONNECT_TICKS  36      /* ~2 seconds for TCP connect */

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

/* Perform HTTP GET request (URL like "http://host/path") */
http_response_t http_get(const char* url);

#endif /* HTTP_H */
