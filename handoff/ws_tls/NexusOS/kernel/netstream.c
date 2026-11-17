/* ============================================================================
 * NexusOS — Streaming HTTP/HTTPS client (Phase 57) — implements netstream.h
 * ============================================================================
 * State machine per stream: RESOLVE -> CONNECT -> (TLS) -> SEND -> HEADERS ->
 * BODY -> DONE. ns_poll() does bounded work; ns_read() decodes the body
 * (Content-Length / chunked / until-close) from the TCP connection or the TLS
 * record layer. Streams are static (4 x ~40 KB in .bss).
 * ============================================================================ */
#include "netstream.h"
#include "tls.h"
#include "tcp.h"
#include "dns.h"
#include "ip.h"
#include "net.h"
#include "string.h"
#include "keyboard.h"

extern volatile uint32_t system_ticks;

typedef enum {
    NS_IDLE = 0, NS_RESOLVE, NS_CONNECT, NS_TLS, NS_SEND, NS_HEADERS, NS_BODY, NS_DONE, NS_ERROR
} ns_state_t;

#define NS_HDR_BUF        4096
#define NS_REQ_BUF        1024
#define NS_CONNECT_TICKS  (10 * 18)
#define NS_TIMEOUT_TICKS  (30 * 18)

/* chunked decoder states */
enum { CH_SIZE = 0, CH_DATA, CH_CRLF, CH_TRAILER, CH_DONE };

struct net_stream {
    bool        used;
    ns_state_t  state;
    char        host[128];
    uint16_t    port;
    char        path[NS_MAX_URL];
    bool        secure, head, insecure;
    uint32_t    ip;
    tcp_conn_t* conn;
    uint32_t    conn_gen;
    uint32_t    t_start, t_last;
    tls_ctx_t   tls;
    uint8_t     req[NS_REQ_BUF];
    int         req_len, req_off;
    char        hdr[NS_HDR_BUF];
    int         hdr_len, body_start;        /* leftover body bytes: hdr[body_start..hdr_len) */
    bool        has_length;
    uint32_t    remaining;                  /* Content-Length countdown */
    int         chunk_state;
    uint32_t    chunk_remaining;
    uint32_t    chunk_size_acc;
    bool        chunk_ext, line_empty;
    bool        body_done;
    ns_info_t   info;
    char        error[96];
};

static net_stream_t streams[NS_MAX_STREAMS];

/* ---------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------- */
static void sappend(char* dst, uint32_t sz, const char* s) {
    uint32_t n = strlen(dst);
    while (s && *s && n < sz - 1) dst[n++] = *s++;
    dst[n] = '\0';
}
static void sappend_n(char* dst, uint32_t sz, const char* s, uint32_t max) {
    uint32_t n = strlen(dst), i = 0;
    while (s && s[i] && i < max && n < sz - 1) dst[n++] = s[i++];
    dst[n] = '\0';
}
static int ns_fail(net_stream_t* s, const char* a, const char* b) {
    s->error[0] = '\0';
    sappend(s->error, sizeof(s->error), a);
    if (b) sappend(s->error, sizeof(s->error), b);
    s->state = NS_ERROR;
    return -1;
}
static bool conn_valid(const net_stream_t* s) {
    return s->conn && s->conn->active && s->conn->gen == s->conn_gen;
}
static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool ieq_n(const char* a, const char* b, int n) {   /* case-insensitive prefix compare */
    for (int i = 0; i < n; i++) { if (lower_c(a[i]) != lower_c(b[i])) return false; if (!a[i]) return false; }
    return true;
}

/* ---------------------------------------------------------------------------
 * URL parsing: [scheme://]host[:port][/path]
 * ------------------------------------------------------------------------- */
static bool parse_url(net_stream_t* s, const char* url) {
    const char* p = url;
    while (*p == ' ') p++;
    s->secure = false;
    if (ieq_n(p, "https://", 8)) { s->secure = true; p += 8; }
    else if (ieq_n(p, "http://", 7)) { p += 7; }
    else {
        /* another scheme? ("ftp://", "file:") -> unsupported */
        for (const char* q = p; *q && *q != '/' && *q != ':'; q++) ;
        const char* q = p; while (*q && *q != '/' && *q != ':') q++;
        if (*q == ':' && q[1] == '/' && q[2] == '/') return false;
    }
    if (*p == '[') return false;                 /* IPv6 literal: unsupported */
    int hi = 0;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#' && hi < (int)sizeof(s->host) - 1)
        s->host[hi++] = lower_c(*p++);
    s->host[hi] = '\0';
    if (hi == 0) return false;
    s->port = s->secure ? 443 : 80;
    if (*p == ':') {
        p++;
        uint32_t pt = 0;
        while (*p >= '0' && *p <= '9') { pt = pt * 10 + (uint32_t)(*p++ - '0'); if (pt > 65535) return false; }
        if (pt == 0) return false;
        s->port = (uint16_t)pt;
    }
    int pi = 0;
    if (*p == '\0' || *p == '#') { s->path[pi++] = '/'; }
    else {
        if (*p == '?') s->path[pi++] = '/';
        while (*p && *p != '#' && pi < NS_MAX_URL - 1) {
            char c = *p++;
            if (c == ' ') { if (pi + 3 >= NS_MAX_URL - 1) break; s->path[pi++] = '%'; s->path[pi++] = '2'; s->path[pi++] = '0'; }
            else s->path[pi++] = c;
        }
    }
    s->path[pi] = '\0';
    return true;
}

/* ---------------------------------------------------------------------------
 * Request building
 * ------------------------------------------------------------------------- */
static void req_put(net_stream_t* s, const char* str) {
    while (*str && s->req_len < NS_REQ_BUF - 1) s->req[s->req_len++] = (uint8_t)*str++;
}

static void build_request(net_stream_t* s, const char* method, const char* extra) {
    char num[12];
    s->req_len = 0; s->req_off = 0;
    req_put(s, method); req_put(s, " "); req_put(s, s->path); req_put(s, " HTTP/1.1\r\n");
    req_put(s, "Host: "); req_put(s, s->host);
    if (s->port != (s->secure ? 443 : 80)) { req_put(s, ":"); int_to_str(s->port, num); req_put(s, num); }
    req_put(s, "\r\nUser-Agent: NexusOS/5.1\r\nAccept: */*\r\nConnection: close\r\n");
    if (extra) {
        /* copy header lines; drop our private X-Nexus-Insecure switch */
        const char* p = extra;
        while (*p) {
            const char* e = p;
            while (*e && *e != '\n') e++;
            int ll = (int)(e - p);
            while (ll > 0 && (p[ll - 1] == '\r' || p[ll - 1] == ' ')) ll--;
            if (ll > 0) {
                if (ieq_n(p, "X-Nexus-Insecure:", 17)) {
                    s->insecure = true;
                } else {
                    int i = 0;
                    while (i < ll && s->req_len < NS_REQ_BUF - 3) s->req[s->req_len++] = (uint8_t)p[i++];
                    req_put(s, "\r\n");
                }
            }
            p = *e ? e + 1 : e;
        }
    }
    req_put(s, "\r\n");
}

/* ---------------------------------------------------------------------------
 * Header parsing
 * ------------------------------------------------------------------------- */
static int find_header_end(const char* h, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (h[i] == '\r' && h[i + 1] == '\n' && h[i + 2] == '\r' && h[i + 3] == '\n') return i + 4;
        if (h[i] == '\n' && h[i + 1] == '\n') return i + 2;
    }
    return -1;
}

static void resolve_location(net_stream_t* s, const char* loc, int len) {
    char* out = s->info.location;
    out[0] = '\0';
    while (len > 0 && *loc == ' ') { loc++; len--; }
    if (len <= 0) return;
    if ((len >= 7 && ieq_n(loc, "http://", 7)) || (len >= 8 && ieq_n(loc, "https://", 8))) {
        sappend_n(out, NS_MAX_URL, loc, (uint32_t)len);
        return;
    }
    char num[12];
    sappend(out, NS_MAX_URL, s->secure ? "https://" : "http://");
    if (len >= 2 && loc[0] == '/' && loc[1] == '/') {          /* scheme-relative */
        sappend_n(out, NS_MAX_URL, loc + 2, (uint32_t)(len - 2));
        return;
    }
    sappend(out, NS_MAX_URL, s->host);
    if (s->port != (s->secure ? 443 : 80)) { sappend(out, NS_MAX_URL, ":"); int_to_str(s->port, num); sappend(out, NS_MAX_URL, num); }
    if (loc[0] == '/') { sappend_n(out, NS_MAX_URL, loc, (uint32_t)len); return; }
    /* relative to the directory of the current path */
    int cut = 0;
    for (int i = 0; s->path[i]; i++) if (s->path[i] == '/') cut = i + 1;
    sappend_n(out, NS_MAX_URL, s->path, (uint32_t)cut);
    if (cut == 0) sappend(out, NS_MAX_URL, "/");
    sappend_n(out, NS_MAX_URL, loc, (uint32_t)len);
}

static void parse_headers(net_stream_t* s, int end) {
    const char* h = s->hdr;
    int i = 0;
    /* status line */
    if (ieq_n(h, "HTTP/", 5)) {
        i = 5;
        while (i < end && h[i] != ' ' && h[i] != '\n') i++;
        while (i < end && h[i] == ' ') i++;
        int code = 0;
        while (i < end && h[i] >= '0' && h[i] <= '9') code = code * 10 + (h[i++] - '0');
        s->info.status = code;
    }
    while (i < end && h[i] != '\n') i++;
    if (i < end) i++;
    /* header lines */
    while (i < end) {
        int ls = i;
        while (i < end && h[i] != '\n') i++;
        int le = i;                                   /* [ls, le) one line */
        if (i < end) i++;
        if (le > ls && h[le - 1] == '\r') le--;
        if (le == ls) break;                          /* empty line */
        int colon = ls;
        while (colon < le && h[colon] != ':') colon++;
        if (colon >= le) continue;
        int nl = colon - ls;
        int vs = colon + 1;
        while (vs < le && h[vs] == ' ') vs++;
        int vl = le - vs;
        if (nl == 14 && ieq_n(h + ls, "Content-Length", 14)) {
            uint32_t v = 0;
            for (int k = 0; k < vl && h[vs + k] >= '0' && h[vs + k] <= '9'; k++) v = v * 10 + (uint32_t)(h[vs + k] - '0');
            s->info.content_length = v; s->has_length = true; s->remaining = v;
        } else if (nl == 12 && ieq_n(h + ls, "Content-Type", 12)) {
            s->info.content_type[0] = '\0';
            sappend_n(s->info.content_type, sizeof(s->info.content_type), h + vs, (uint32_t)vl);
        } else if (nl == 8 && ieq_n(h + ls, "Location", 8)) {
            resolve_location(s, h + vs, vl);
        } else if (nl == 17 && ieq_n(h + ls, "Transfer-Encoding", 17)) {
            for (int k = 0; k + 7 <= vl; k++) if (ieq_n(h + vs + k, "chunked", 7)) { s->info.chunked = true; break; }
        }
    }
    if (s->info.chunked) { s->has_length = false; s->info.content_length = 0; }
}

static void fill_tls_info(net_stream_t* s) {
    tls_ctx_t* t = &s->tls;
    char* o = s->info.tls_info;
    o[0] = '\0';
    s->info.trusted = t->trusted;
    sappend(o, sizeof(s->info.tls_info), "TLS 1.3 AES128-GCM x25519 CN=");
    sappend_n(o, sizeof(s->info.tls_info), t->cert_count ? t->certs[0].subject : "?", 28);
    sappend(o, sizeof(s->info.tls_info), " (");
    if (t->trusted) sappend(o, sizeof(s->info.tls_info), t->root_name[0] ? t->root_name : "trusted");
    else { sappend(o, sizeof(s->info.tls_info), "UNTRUSTED: "); sappend(o, sizeof(s->info.tls_info), t->reason); }
    uint32_t n = strlen(o);
    if (n >= sizeof(s->info.tls_info) - 1) n = sizeof(s->info.tls_info) - 2;
    o[n] = ')'; o[n + 1] = '\0';
}

/* ---------------------------------------------------------------------------
 * Raw source reads (TLS record layer or TCP). >0 bytes, 0 none, -1 EOF, -2 error
 * ------------------------------------------------------------------------- */
static int src_read_raw(net_stream_t* s, uint8_t* buf, int max) {
    if (s->secure) return tls_read(&s->tls, buf, max);
    if (!conn_valid(s)) return -1;
    return tcp_recv(s->conn, buf, (uint16_t)max);
}

/* Body source: leftover header-buffer bytes first */
static int src_read(net_stream_t* s, uint8_t* buf, int max) {
    if (s->body_start < s->hdr_len) {
        int n = s->hdr_len - s->body_start;
        if (n > max) n = max;
        memcpy(buf, s->hdr + s->body_start, n);
        s->body_start += n;
        return n;
    }
    return src_read_raw(s, buf, max);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
net_stream_t* ns_open(const char* url, const char* method, const char* extra_headers) {
    if (!url) return NULL;
    net_stream_t* s = NULL;
    for (int i = 0; i < NS_MAX_STREAMS; i++) if (!streams[i].used) { s = &streams[i]; break; }
    if (!s) return NULL;
    /* reset everything except the TLS context's big buffers */
    s->used = true;
    s->state = NS_IDLE;
    s->host[0] = 0; s->port = 0; s->path[0] = 0; s->secure = s->head = s->insecure = false;
    s->ip = 0; s->conn = NULL; s->conn_gen = 0; s->t_start = s->t_last = 0;
    s->req_len = s->req_off = 0; s->hdr_len = s->body_start = 0;
    s->has_length = false; s->remaining = 0;
    s->chunk_state = CH_SIZE; s->chunk_remaining = 0; s->chunk_size_acc = 0; s->chunk_ext = false; s->line_empty = true;
    s->body_done = false;
    memset(&s->info, 0, sizeof(s->info));
    s->error[0] = '\0';
    s->tls.state = TLS_S_IDLE;

    if (!parse_url(s, url)) { s->used = false; return NULL; }
    s->head = method && (method[0] == 'H' || method[0] == 'h');
    build_request(s, s->head ? "HEAD" : "GET", extra_headers);
    strncpy(s->info.host, s->host, sizeof(s->info.host) - 1);
    s->info.host[sizeof(s->info.host) - 1] = '\0';
    s->info.secure = s->secure;
    s->state = NS_RESOLVE;
    s->t_start = s->t_last = system_ticks;
    return s;
}

int ns_poll(net_stream_t* s) {
    if (!s || !s->used) return -1;
    net_poll();
    uint32_t now = system_ticks;
    switch (s->state) {
        case NS_RESOLVE: {
            uint32_t ip = dns_resolve(s->host);            /* blocking <= 2 s, once */
            if (!ip) return ns_fail(s, "DNS lookup failed for ", s->host);
            s->ip = ip;
            s->conn = tcp_connect(ip, s->port, tcp_ephemeral_port());
            if (!s->conn) return ns_fail(s, "no free TCP connection", NULL);
            s->conn_gen = s->conn->gen;
            s->state = NS_CONNECT;
            s->t_start = s->t_last = system_ticks;
            return 0;
        }
        case NS_CONNECT:
            if (!conn_valid(s)) return ns_fail(s, "connection refused", NULL);
            if (s->conn->connected) {
                if (s->secure) {
                    if (tls_start(&s->tls, s->conn, s->host, s->insecure) != 0)
                        return ns_fail(s, "TLS: ", tls_error(&s->tls));
                    s->state = NS_TLS;
                } else {
                    s->state = NS_SEND;
                }
                s->t_last = now;
                return 0;
            }
            if (now - s->t_start > NS_CONNECT_TICKS) return ns_fail(s, "connect timeout", NULL);
            return 0;
        case NS_TLS: {
            int r = tls_poll(&s->tls);
            if (r < 0) return ns_fail(s, "TLS: ", tls_error(&s->tls));
            if (r == 1) { fill_tls_info(s); s->state = NS_SEND; s->t_last = now; return 0; }
            if (now - s->t_last > NS_TIMEOUT_TICKS) return ns_fail(s, "TLS handshake timeout", NULL);
            return 0;
        }
        case NS_SEND: {
            int n;
            if (s->secure) {
                n = tls_write(&s->tls, s->req + s->req_off, s->req_len - s->req_off);
                if (n < 0) return ns_fail(s, "TLS: ", tls_error(&s->tls));
            } else {
                if (!conn_valid(s)) return ns_fail(s, "connection reset", NULL);
                n = tcp_send_nb(s->conn, s->req + s->req_off, (uint16_t)(s->req_len - s->req_off));
                if (n < 0) return ns_fail(s, "send failed", NULL);
            }
            s->req_off += n;
            if (s->req_off >= s->req_len) { s->state = NS_HEADERS; s->t_last = now; }
            else if (now - s->t_last > NS_TIMEOUT_TICKS) return ns_fail(s, "send timeout", NULL);
            return 0;
        }
        case NS_HEADERS:
            for (int iter = 0; iter < 8; iter++) {
                int space = NS_HDR_BUF - 1 - s->hdr_len;
                if (space <= 0) return ns_fail(s, "response headers too large", NULL);
                int n = src_read_raw(s, (uint8_t*)s->hdr + s->hdr_len, space);
                if (n == -2) return ns_fail(s, "TLS: ", tls_error(&s->tls));
                if (n == -1) return ns_fail(s, s->hdr_len ? "connection closed inside response headers"
                                                           : "connection closed before any response", NULL);
                if (n == 0) {
                    if (now - s->t_last > NS_TIMEOUT_TICKS) return ns_fail(s, "timeout waiting for response", NULL);
                    return 0;
                }
                s->hdr_len += n;
                s->t_last = now;
                int end = find_header_end(s->hdr, s->hdr_len);
                if (end >= 0) {
                    s->hdr[s->hdr_len] = '\0';
                    parse_headers(s, end);
                    s->body_start = end;
                    bool no_body = s->head || s->info.status / 100 == 1 || s->info.status == 204 || s->info.status == 304 ||
                                   (s->has_length && s->remaining == 0);
                    if (no_body) { s->body_done = true; s->state = NS_DONE; }
                    else s->state = NS_BODY;
                    return 1;
                }
            }
            return 0;
        case NS_BODY:
        case NS_DONE:
            return 1;
        default:
            return -1;
    }
}

/* one byte from the source for the chunk-framing states */
static int src_byte(net_stream_t* s, uint8_t* b) { return src_read(s, b, 1); }

static int read_chunked(net_stream_t* s, uint8_t* buf, int max) {
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t b;
        int n;
        switch (s->chunk_state) {
            case CH_SIZE:
                n = src_byte(s, &b);
                if (n <= 0) return n == 0 ? 0 : (ns_fail(s, "connection closed inside chunked body", NULL), -2);
                if (b == '\n') {
                    if (s->line_empty) { s->chunk_ext = false; continue; }   /* tolerate blank line */
                    s->chunk_remaining = s->chunk_size_acc;
                    s->chunk_size_acc = 0; s->chunk_ext = false; s->line_empty = true;
                    s->chunk_state = s->chunk_remaining ? CH_DATA : CH_TRAILER;
                } else if (b == '\r') {
                } else if (b == ';') { s->chunk_ext = true; s->line_empty = false; }
                else if (!s->chunk_ext) {
                    int v = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
                    if (v < 0) return ns_fail(s, "bad chunk size", NULL), -2;
                    if (s->chunk_size_acc > 0x0FFFFFFF) return ns_fail(s, "chunk too large", NULL), -2;
                    s->chunk_size_acc = (s->chunk_size_acc << 4) | (uint32_t)v;
                    s->line_empty = false;
                }
                break;
            case CH_DATA: {
                int want = max;
                if ((uint32_t)want > s->chunk_remaining) want = (int)s->chunk_remaining;
                n = src_read(s, buf, want);
                if (n == 0) return 0;
                if (n < 0) return ns_fail(s, "connection closed inside chunk", NULL), -2;
                s->chunk_remaining -= (uint32_t)n;
                s->info.received += (uint32_t)n;
                if (s->chunk_remaining == 0) s->chunk_state = CH_CRLF;
                return n;
            }
            case CH_CRLF:
                n = src_byte(s, &b);
                if (n <= 0) return n == 0 ? 0 : (ns_fail(s, "connection closed inside chunked body", NULL), -2);
                if (b == '\n') { s->chunk_state = CH_SIZE; s->line_empty = true; }
                break;
            case CH_TRAILER:
                n = src_byte(s, &b);
                if (n < 0) { s->chunk_state = CH_DONE; s->body_done = true; s->state = NS_DONE; return -1; }
                if (n == 0) return 0;
                if (b == '\n') {
                    if (s->line_empty) { s->chunk_state = CH_DONE; s->body_done = true; s->state = NS_DONE; return -1; }
                    s->line_empty = true;
                } else if (b != '\r') s->line_empty = false;
                break;
            default:
                return -1;
        }
    }
    return 0;
}

int ns_read(net_stream_t* s, void* vbuf, int max) {
    if (!s || !s->used) return -2;
    if (s->state == NS_ERROR) return -2;
    if (s->state == NS_DONE || s->body_done) return -1;
    if (s->state != NS_BODY) return 0;
    if (max <= 0) return 0;
    uint8_t* buf = (uint8_t*)vbuf;

    if (s->info.chunked) return read_chunked(s, buf, max);

    if (s->has_length) {
        if (s->remaining == 0) { s->body_done = true; s->state = NS_DONE; return -1; }
        int want = (uint32_t)max < s->remaining ? max : (int)s->remaining;
        int n = src_read(s, buf, want);
        if (n > 0) {
            s->remaining -= (uint32_t)n;
            s->info.received += (uint32_t)n;
            if (s->remaining == 0) { s->body_done = true; s->state = NS_DONE; }
            return n;
        }
        if (n == 0) return 0;
        if (n == -2) return ns_fail(s, "TLS: ", tls_error(&s->tls)), -2;
        /* EOF before Content-Length satisfied */
        char num[12]; int_to_str((int)s->remaining, num);
        return ns_fail(s, "connection closed early, bytes missing: ", num), -2;
    }
    /* identity, until close */
    int n = src_read(s, buf, max);
    if (n > 0) { s->info.received += (uint32_t)n; return n; }
    if (n == 0) return 0;
    if (n == -2) return ns_fail(s, "TLS: ", tls_error(&s->tls)), -2;
    s->body_done = true; s->state = NS_DONE;
    return -1;
}

bool ns_done(net_stream_t* s) {
    if (!s || !s->used) return true;
    return s->state == NS_DONE || s->body_done || s->state == NS_ERROR;
}

const ns_info_t* ns_info(net_stream_t* s) {
    static ns_info_t empty;
    if (!s || !s->used) { memset(&empty, 0, sizeof(empty)); return &empty; }
    return &s->info;
}

const char* ns_error(net_stream_t* s) {
    if (!s || !s->used) return "";
    return s->error;
}

void ns_close(net_stream_t* s) {
    if (!s || !s->used) return;
    if (s->secure && s->tls.state == TLS_S_CONNECTED) tls_close(&s->tls);
    if (conn_valid(s)) tcp_close(s->conn);
    s->conn = NULL;
    s->used = false;
    s->state = NS_IDLE;
}

int ns_active_count(void) {
    int n = 0;
    for (int i = 0; i < NS_MAX_STREAMS; i++) if (streams[i].used) n++;
    return n;
}

/* Raw response headers (status line + header lines) — private accessor used
 * by `curl -I`; not part of the shared contract. */
int ns_raw_headers(net_stream_t* s, char* out, int max) {
    if (!s || !s->used || max <= 0) return 0;
    int n = s->body_start;
    if (n >= max) n = max - 1;
    memcpy(out, s->hdr, n);
    out[n] = '\0';
    return n;
}

/* ---------------------------------------------------------------------------
 * Blocking convenience fetch
 * ------------------------------------------------------------------------- */
static bool esc_pressed(void) {
    if (!keyboard_has_key()) return false;
    return keyboard_getchar() == 27;
}

int ns_fetch(const char* url, uint8_t* buf, int max, ns_info_t* info_out) {
    char cur[NS_MAX_URL];
    strncpy(cur, url, NS_MAX_URL - 1); cur[NS_MAX_URL - 1] = '\0';
    if (info_out) memset(info_out, 0, sizeof(*info_out));

    for (int redirects = 0; redirects <= 4; redirects++) {
        net_stream_t* s = ns_open(cur, "GET", NULL);
        if (!s) return -1;
        uint32_t last = system_ticks;
        int r;
        while ((r = ns_poll(s)) == 0) {
            if (esc_pressed()) { ns_close(s); return -1; }
            if (system_ticks - last > NS_TIMEOUT_TICKS + NS_CONNECT_TICKS) { ns_close(s); return -1; }
            __asm__ volatile("hlt");
        }
        if (r < 0) { if (info_out) *info_out = s->info; ns_close(s); return -1; }
        const ns_info_t* in = ns_info(s);
        if ((in->status == 301 || in->status == 302 || in->status == 303 || in->status == 307 || in->status == 308) &&
            in->location[0] && redirects < 4) {
            strncpy(cur, in->location, NS_MAX_URL - 1); cur[NS_MAX_URL - 1] = '\0';
            ns_close(s);
            continue;
        }
        int total = 0;
        last = system_ticks;
        while (total < max) {
            int n = ns_read(s, buf + total, max - total);
            if (n > 0) { total += n; last = system_ticks; continue; }
            if (n == -1) break;
            if (n == -2) { if (info_out) *info_out = s->info; ns_close(s); return -1; }
            if (esc_pressed()) { ns_close(s); return -1; }
            ns_poll(s);
            if (system_ticks - last > NS_TIMEOUT_TICKS) { if (info_out) *info_out = s->info; ns_close(s); return -1; }
            __asm__ volatile("hlt");
        }
        if (info_out) *info_out = s->info;
        ns_close(s);
        return total;
    }
    return -1;
}
