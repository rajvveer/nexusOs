/* ============================================================================
 * NexusOS — Streaming HTTP/HTTPS client (Phase 57)
 * ============================================================================
 * One non-blocking fetch object: open a URL, pump it from an idle loop with
 * ns_poll(), pull decoded body bytes with ns_read(). http:// and https:// use
 * the same API; the TLS 1.3 layer (tls.c) is driven inside ns_poll(). Consumers:
 * the graphical browser (page + images), `wget`, the package manager.
 *
 * Contract (both the implementer and every consumer rely on this):
 *   - Never blocks: ns_open() only parses the URL and starts DNS/connect; all
 *     progress happens in ns_poll() which does a bounded amount of work per call
 *     (net_poll() is called inside it). Callers must keep calling ns_poll() from
 *     a non-IRQ context (desktop loop / shell idle hook / a busy loop with hlt).
 *   - ns_read() returns decoded body bytes (chunked transfer-encoding removed,
 *     TLS records decrypted); headers are never part of the body.
 *   - Redirects (301/302/303/307/308) are NOT followed automatically by
 *     ns_open/ns_poll; ns_info()->location carries the target and the caller
 *     decides (ns_fetch() follows up to 4).
 *   - Streams are a scarce resource (NS_MAX_STREAMS); always ns_close().
 * ============================================================================ */
#ifndef NETSTREAM_H
#define NETSTREAM_H

#include "types.h"

#define NS_MAX_URL      512
#define NS_MAX_STREAMS  4

typedef struct net_stream net_stream_t;

typedef struct {
    int      status;             /* HTTP status code; 0 until the headers are parsed */
    uint32_t content_length;     /* from Content-Length, 0 if unknown/chunked      */
    uint32_t received;           /* body bytes handed out so far                   */
    char     content_type[64];   /* "text/html; charset=utf-8" etc. ("" if none)   */
    bool     attachment;
    char     filename[256];
    char     location[NS_MAX_URL]; /* redirect target for 3xx, resolved to absolute */
    bool     chunked;            /* Transfer-Encoding: chunked (decoded for you)   */
    bool     secure;             /* https:// (TLS record layer active)             */
    bool     trusted;            /* certificate chain validated to a built-in root */
    char     tls_info[96];       /* "TLS 1.3 AES128-GCM x25519 CN=example.com (DigiCert)" or "" */
    char     host[128];          /* final host actually connected to               */
} ns_info_t;

/* Start a request. method = "GET" or "HEAD"; extra_headers = NULL or
 * "Name: value\r\n..." lines appended to the request. Returns NULL if no
 * stream slot is free or the URL is unsupported (scheme other than http/https). */
net_stream_t* ns_open(const char* url, const char* method, const char* extra_headers);

/* Drive the stream. Returns  0 = still working (connect / handshake / headers),
 *                            1 = headers parsed, body may be read with ns_read,
 *                           -1 = failed (see ns_error). Safe to call after 1. */
int ns_poll(net_stream_t* s);

/* Read decoded body bytes. >0 bytes copied, 0 nothing available yet (keep
 * polling), -1 end of body (all data delivered), -2 error. */
int ns_read(net_stream_t* s, void* buf, int max);

/* True once the whole body has been received (Content-Length reached, last
 * chunk seen, or the server closed the connection). */
bool ns_done(net_stream_t* s);

const ns_info_t* ns_info(net_stream_t* s);
const char*      ns_error(net_stream_t* s);   /* human-readable last error or "" */
void             ns_close(net_stream_t* s);

/* Convenience: blocking fetch of a whole body into buf (follows up to 4
 * redirects, pumps net/idle work itself, aborts on Esc). Returns the number
 * of body bytes stored (truncated to max), or -1 on failure. info_out may be NULL. */
int ns_fetch(const char* url, uint8_t* buf, int max, ns_info_t* info_out);

/* Diagnostics for `netstat`/`tlsinfo`: number of streams in use. */
int ns_active_count(void);

#endif /* NETSTREAM_H */
