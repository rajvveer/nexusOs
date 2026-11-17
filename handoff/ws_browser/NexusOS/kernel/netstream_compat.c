/* ============================================================================
 * NexusOS — netstream.h COMPATIBILITY implementation (Phase 57, ws_browser)
 * ============================================================================
 * TEMPORARY. The real streaming client (netstream.c, with TLS) is written by
 * the ws_tls agent; this file exists only so the graphical browser can be
 * built and tested in the ws_browser worktree. It is deleted at merge time.
 *
 * It implements the netstream.h contract for plain http:// URLs on top of the
 * existing tcp.h / dns.h: non-blocking state machine driven by ns_poll(),
 * HTTP/1.1 request with "Connection: close", header parsing (status,
 * Content-Length, Content-Type, Location, Transfer-Encoding: chunked), a
 * chunked-transfer decoder, and a per-stream FIFO the consumer drains with
 * ns_read(). https:// URLs fail with "TLS not available".
 *
 * The TCP layer advertises a fixed 1 KB window and never shrinks it, so bytes
 * that arrive while rx_buf is full are dropped and later retransmitted by the
 * server. ns_poll therefore drains aggressively (several net_poll/tcp_recv
 * rounds per call) to keep throughput reasonable under QEMU.
 * ============================================================================ */

#include "netstream.h"
#include "tcp.h"
#include "dns.h"
#include "ip.h"
#include "net.h"
#include "pci.h"
#include "driver.h"
#include "string.h"
#include "keyboard.h"

extern volatile uint32_t system_ticks;

#define NSC_HDR_MAX    2048
#define NSC_FIFO       (48 * 1024)

/* IRQ-safe critical sections (the NIC-IRQ pump below shares the FIFOs). */
static inline uint32_t irq_save(void) {
    uint32_t f; __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory"); return f;
}
static inline void irq_restore(uint32_t f) {
    __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc");
}
#define NSC_CONNECT_T  (18 * 6)     /* ~6 s connect timeout                     */
#define NSC_IDLE_T     (18 * 12)    /* ~12 s with no bytes -> give up            */
#define NSC_DRAIN_ROUNDS 24         /* net_poll + tcp_recv rounds per ns_poll    */

enum { ST_FREE = 0, ST_RESOLVE, ST_CONNECT, ST_HEADERS, ST_BODY, ST_DONE, ST_ERROR };

struct net_stream {
    int       state;
    char      url[NS_MAX_URL];
    char      host[128];
    char      path[NS_MAX_URL];
    uint16_t  port;
    char      method[8];
    char      extra[200];
    uint32_t  ip;

    tcp_conn_t* conn;
    uint32_t  gen;
    uint32_t  t_start;      /* state entry tick                                 */
    uint32_t  t_last;       /* last byte received                               */

    char      hdr[NSC_HDR_MAX];
    int       hdr_len;

    uint8_t   fifo[NSC_FIFO];
    int       fifo_len;

    /* chunked decoder: 0 = reading size line, 1 = chunk data, 2 = CRLF after
     * data, 3 = trailer/eof */
    int       chunk_state;
    uint32_t  chunk_left;
    char      chunk_line[24];
    int       chunk_line_len;

    uint32_t  body_raw;     /* raw body bytes consumed from TCP                 */
    bool      eof;          /* no more body bytes will arrive                   */
    ns_info_t info;
    char      err[64];
};

static struct net_stream streams[NS_MAX_STREAMS];
static uint16_t next_port = 51000;
static void install_pump(void);                 /* NIC-IRQ drain, defined below */

/* --------------------------------------------------------------------------
 * small helpers
 * -------------------------------------------------------------------------- */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool prefix_ci(const char* s, const char* p) {
    while (*p) { if (lc(*s) != lc(*p)) return false; s++; p++; }
    return true;
}

static void set_err(struct net_stream* s, const char* m) {
    strncpy(s->err, m, sizeof(s->err) - 1);
    s->err[sizeof(s->err) - 1] = '\0';
    s->state = ST_ERROR;
}

static void drop_conn(struct net_stream* s) {
    if (s->conn && s->conn->active && s->conn->gen == s->gen) tcp_close(s->conn);
    s->conn = NULL;
}

/* Parse "http://host[:port]/path". Returns false for other schemes. */
static bool parse_url(struct net_stream* s, const char* url) {
    const char* p = url;
    if (prefix_ci(p, "https://")) { set_err(s, "TLS not available in this build"); return false; }
    if (!prefix_ci(p, "http://")) { set_err(s, "Unsupported URL scheme"); return false; }
    p += 7;
    int hi = 0;
    while (*p && *p != ':' && *p != '/' && *p != '?' && hi < (int)sizeof(s->host) - 1) s->host[hi++] = *p++;
    s->host[hi] = '\0';
    if (!hi) { set_err(s, "Bad URL"); return false; }
    s->port = 80;
    if (*p == ':') {
        p++;
        unsigned pt = 0;
        while (*p >= '0' && *p <= '9') { pt = pt * 10 + (unsigned)(*p++ - '0'); if (pt > 65535) pt = 65535; }
        if (pt) s->port = (uint16_t)pt;
    }
    int pi = 0;
    if (*p == '/' || *p == '?') {
        if (*p == '?') s->path[pi++] = '/';
        while (*p && pi < (int)sizeof(s->path) - 1) {
            char c = *p++;
            if (c == '#') break;
            if (c == ' ') { if (pi + 3 < (int)sizeof(s->path) - 1) { s->path[pi++] = '%'; s->path[pi++] = '2'; s->path[pi++] = '0'; } continue; }
            s->path[pi++] = c;
        }
    } else {
        s->path[pi++] = '/';
    }
    s->path[pi] = '\0';
    return true;
}

/* Resolve `loc` (absolute or relative) against the stream's URL into out[]. */
static void resolve_location(const struct net_stream* s, const char* loc, char* out, int max) {
    int oi = 0;
    if (prefix_ci(loc, "http://") || prefix_ci(loc, "https://")) {
        while (*loc && oi < max - 1) out[oi++] = *loc++;
        out[oi] = '\0';
        return;
    }
    /* scheme://host[:port] */
    const char* u = s->url;
    const char* ss = strstr(u, "://");
    const char* hostend = ss ? ss + 3 : u;
    while (*hostend && *hostend != '/' && *hostend != '?') hostend++;
    for (const char* q = u; q < hostend && oi < max - 1; q++) out[oi++] = *q;
    if (loc[0] == '/') {
        while (*loc && oi < max - 1) out[oi++] = *loc++;
    } else {
        /* directory of the current path */
        const char* last = NULL;
        for (const char* q = hostend; *q && *q != '?'; q++) if (*q == '/') last = q;
        if (last) { for (const char* q = hostend; q <= last && oi < max - 1; q++) out[oi++] = *q; }
        else if (oi < max - 1) out[oi++] = '/';
        while (*loc && oi < max - 1) out[oi++] = *loc++;
    }
    out[oi] = '\0';
}

static void parse_headers(struct net_stream* s) {
    char* h = s->hdr;
    /* status line: HTTP/1.x NNN */
    int st = 0;
    char* sp = h;
    while (*sp && *sp != ' ' && *sp != '\r') sp++;
    while (*sp == ' ') sp++;
    while (*sp >= '0' && *sp <= '9') st = st * 10 + (*sp++ - '0');
    s->info.status = st;
    /* header lines */
    char* line = h;
    while (*line && *line != '\r') line++;
    if (*line == '\r') line += 2;
    while (*line && !(line[0] == '\r' && line[1] == '\n')) {
        char* eol = line;
        while (*eol && *eol != '\r') eol++;
        char saved = *eol; *eol = '\0';
        if (prefix_ci(line, "content-length:")) {
            const char* v = line + 15; while (*v == ' ') v++;
            uint32_t n = 0; while (*v >= '0' && *v <= '9') n = n * 10 + (uint32_t)(*v++ - '0');
            s->info.content_length = n;
        } else if (prefix_ci(line, "content-type:")) {
            const char* v = line + 13; while (*v == ' ') v++;
            strncpy(s->info.content_type, v, sizeof(s->info.content_type) - 1);
            s->info.content_type[sizeof(s->info.content_type) - 1] = '\0';
        } else if (prefix_ci(line, "location:")) {
            const char* v = line + 9; while (*v == ' ') v++;
            resolve_location(s, v, s->info.location, NS_MAX_URL);
        } else if (prefix_ci(line, "transfer-encoding:")) {
            const char* v = line + 18; while (*v == ' ') v++;
            if (prefix_ci(v, "chunked")) s->info.chunked = true;
        }
        *eol = saved;
        line = eol;
        if (*line == '\r') line += 2;
    }
    strncpy(s->info.host, s->host, sizeof(s->info.host) - 1);
    s->info.host[sizeof(s->info.host) - 1] = '\0';
}

/* Feed raw body bytes (post-header) through the chunked decoder / plain path
 * into the FIFO. Returns the number of raw bytes consumed (all of them unless
 * the FIFO is full, which the caller avoids by sizing reads to the room). */
static void feed_body(struct net_stream* s, const uint8_t* d, int n) {
    if (!s->info.chunked) {
        int room = NSC_FIFO - s->fifo_len;
        if (n > room) n = room;
        memcpy(s->fifo + s->fifo_len, d, n);
        s->fifo_len += n;
        s->body_raw += (uint32_t)n;
        if (s->info.content_length && s->body_raw >= s->info.content_length) s->eof = true;
        return;
    }
    for (int i = 0; i < n; i++) {
        uint8_t c = d[i];
        s->body_raw++;
        switch (s->chunk_state) {
        case 0: /* size line (hex, may have ;ext) */
            if (c == '\n') {
                uint32_t v = 0; bool any = false;
                for (int k = 0; k < s->chunk_line_len; k++) {
                    char ch = lc(s->chunk_line[k]);
                    if (ch >= '0' && ch <= '9') { v = v * 16 + (uint32_t)(ch - '0'); any = true; }
                    else if (ch >= 'a' && ch <= 'f') { v = v * 16 + (uint32_t)(ch - 'a' + 10); any = true; }
                    else break;
                }
                s->chunk_line_len = 0;
                if (!any) break;                 /* blank line between chunks */
                if (v == 0) { s->chunk_state = 3; s->eof = true; }
                else { s->chunk_left = v; s->chunk_state = 1; }
            } else if (c != '\r') {
                if (s->chunk_line_len < (int)sizeof(s->chunk_line) - 1) s->chunk_line[s->chunk_line_len++] = (char)c;
            }
            break;
        case 1:
            if (s->fifo_len < NSC_FIFO) s->fifo[s->fifo_len++] = c;
            if (--s->chunk_left == 0) s->chunk_state = 2;
            break;
        case 2:
            if (c == '\n') s->chunk_state = 0;
            break;
        default:
            break;                                /* trailers: ignore */
        }
    }
}

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */
net_stream_t* ns_open(const char* url, const char* method, const char* extra_headers) {
    if (!url) return NULL;
    install_pump();
    struct net_stream* s = NULL;
    for (int i = 0; i < NS_MAX_STREAMS; i++) if (streams[i].state == ST_FREE) { s = &streams[i]; break; }
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    strncpy(s->url, url, NS_MAX_URL - 1);
    strncpy(s->method, (method && method[0]) ? method : "GET", sizeof(s->method) - 1);
    if (extra_headers) strncpy(s->extra, extra_headers, sizeof(s->extra) - 1);
    s->state = ST_RESOLVE;
    s->t_start = s->t_last = system_ticks;
    if (!parse_url(s, url)) return s;            /* state = ST_ERROR, caller sees -1 */
    return s;
}

/* Move whatever the TCB holds into the header buffer / body FIFO.
 * Returns bytes moved, 0 nothing available, -1 peer closed and nothing left,
 * -2 no room on our side (the consumer must ns_read first), -3 error set. */
static int pull(struct net_stream* s) {
    int moved = 0;
    for (;;) {
        if (s->state == ST_HEADERS) {
            int room = NSC_HDR_MAX - 1 - s->hdr_len;
            if (room <= 0) { set_err(s, "Response headers too large"); return -3; }
            int got = tcp_recv(s->conn, s->hdr + s->hdr_len, (uint16_t)room);
            if (got < 0) return moved ? moved : -1;
            if (got == 0) return moved;
            moved += got;
            s->t_last = system_ticks;
            s->hdr_len += got;
            s->hdr[s->hdr_len] = '\0';
            for (int i = 0; i + 3 < s->hdr_len; i++) {
                if (s->hdr[i] == '\r' && s->hdr[i+1] == '\n' && s->hdr[i+2] == '\r' && s->hdr[i+3] == '\n') {
                    s->hdr[i + 2] = '\0';
                    parse_headers(s);
                    s->state = ST_BODY;
                    int body0 = i + 4;
                    if (s->hdr_len > body0) feed_body(s, (const uint8_t*)s->hdr + body0, s->hdr_len - body0);
                    s->hdr_len = 0;
                    if (!strcmp(s->method, "HEAD") || s->info.status == 204 || s->info.status == 304) s->eof = true;
                    break;
                }
            }
            if (s->state == ST_HEADERS) continue;      /* more header bytes may be queued */
        }
        if (s->state != ST_BODY || s->eof) return moved;
        int room = NSC_FIFO - s->fifo_len;
        if (s->info.chunked) room -= 8;                 /* chunk framing can expand into the FIFO */
        if (room <= 0) return moved ? moved : -2;
        uint8_t tmp[1024];
        int want = room < (int)sizeof(tmp) ? room : (int)sizeof(tmp);
        int got = tcp_recv(s->conn, tmp, (uint16_t)want);
        if (got < 0) { s->eof = true; return moved ? moved : -1; }   /* closed, rx empty */
        if (got == 0) return moved;
        moved += got;
        s->t_last = system_ticks;
        feed_body(s, tmp, got);
    }
}

#define NSC_IDLE_SPIN 200    /* empty net_poll rounds before yielding to the caller */

/* ---- NIC-IRQ pump ----------------------------------------------------------
 * tcp.c advertises a fixed 1 KB window and ACKs at IRQ time, so the server's
 * next 1 KB arrives ~1 ms after each ACK; if that segment is processed (by the
 * timer-IRQ net_poll or any task-context net_poll) while the previous 1 KB is
 * still unread in the TCB it is dropped and only retransmitted 1.5-3 s later.
 * A main loop that composites 1080p frames can never drain that fast, so the
 * compat layer chains a handler on the RTL8139 IRQ: right after the driver
 * queued the frame, dispatch it and pull the TCB empty into our FIFO. The
 * exchange then completes inside the IRQ, ahead of the next segment. The
 * real netstream.c does not need this (ws_tls fixes the window in tcp.c). */
static bool pump_installed = false;

static void ns_irq_pump(struct registers* regs) {
    (void)regs;
    bool any = false;
    for (int i = 0; i < NS_MAX_STREAMS; i++)
        if (streams[i].state == ST_HEADERS || streams[i].state == ST_BODY) { any = true; break; }
    if (!any) return;
    net_poll();                                    /* no-op if a task-context net_poll is running */
    for (int i = 0; i < NS_MAX_STREAMS; i++) {
        struct net_stream* s = &streams[i];
        if ((s->state == ST_HEADERS || s->state == ST_BODY) && !s->eof &&
            s->conn && s->conn->active && s->conn->gen == s->gen)
            pull(s);
    }
}

static void install_pump(void) {
    if (pump_installed) return;
    pump_installed = true;                         /* try once; no NIC = nothing to pump */
    pci_device_t* d = pci_find_device(0x10EC, 0x8139);
    if (d) irq_register_handler(d->irq_line, ns_irq_pump);
}

/* Task-context drain: pull (IRQ-safe) BEFORE net_poll so the TCB is empty when
 * new segments are dispatched, then briefly spin for the next burst. */
static int drain(struct net_stream* s) {
    /* returns bytes moved this call (headers or body), -1 if the connection
     * vanished underneath us */
    if (!s->conn || !s->conn->active || s->conn->gen != s->gen) return -1;
    int moved = 0, idle = 0;
    for (int round = 0; round < NSC_DRAIN_ROUNDS; round++) {
        uint32_t f = irq_save();
        int got = pull(s);
        irq_restore(f);
        if (got == -3 || got == -2 || got == -1) break;
        moved += got;
        if (!s->conn->active || s->conn->gen != s->gen) break;
        net_poll();
        f = irq_save();
        got = pull(s);
        irq_restore(f);
        if (got == -3 || got == -2 || got == -1) break;
        if (got > 0) { moved += got; idle = 0; continue; }
        if (++idle > NSC_IDLE_SPIN) break;
        round--;                                    /* empty spins do not count as rounds */
    }
    return moved;
}

int ns_poll(net_stream_t* s) {
    if (!s) return -1;
    switch (s->state) {
    case ST_ERROR: return -1;
    case ST_DONE:  return 1;
    case ST_FREE:  return -1;
    case ST_RESOLVE: {
        uint32_t ip = 0;
        if (s->host[0] >= '0' && s->host[0] <= '9') ip = ip_parse(s->host);
        if (!ip) ip = dns_resolve(s->host);          /* blocking (<= 2 s) in the compat layer */
        if (!ip) { set_err(s, "DNS lookup failed"); return -1; }
        s->ip = ip;
        uint16_t lp = next_port++;
        if (next_port > 51900) next_port = 51000;
        s->conn = tcp_connect(ip, s->port, lp);
        if (!s->conn) { set_err(s, "No free TCP connection"); return -1; }
        s->gen = s->conn->gen;
        s->state = ST_CONNECT;
        s->t_start = system_ticks;
        return 0;
    }
    case ST_CONNECT: {
        net_poll();
        if (!s->conn->active || s->conn->gen != s->gen) { s->conn = NULL; set_err(s, "Connection refused"); return -1; }
        if (!s->conn->connected) {
            if (system_ticks - s->t_start > NSC_CONNECT_T) { drop_conn(s); set_err(s, "Could not connect (timeout)"); return -1; }
            return 0;
        }
        /* send the request */
        char req[900];
        int n = 0;
        #define APP(str) do { const char* _q = (str); while (*_q && n < (int)sizeof(req) - 1) req[n++] = *_q++; } while (0)
        APP(s->method); APP(" "); APP(s->path); APP(" HTTP/1.1\r\nHost: "); APP(s->host);
        if (s->port != 80) { char pb[8]; int_to_str(s->port, pb); APP(":"); APP(pb); }
        APP("\r\nUser-Agent: NexusOS/5.1 NexusBrowser\r\nAccept: text/html,image/*,*/*\r\nConnection: close\r\n");
        if (s->extra[0]) { APP(s->extra); if (!(n >= 2 && req[n-2] == '\r' && req[n-1] == '\n')) APP("\r\n"); }
        APP("\r\n");
        #undef APP
        req[n] = '\0';
        if (tcp_send(s->conn, req, (uint16_t)n) < 0) { drop_conn(s); set_err(s, "Send failed"); return -1; }
        s->state = ST_HEADERS;
        s->t_start = s->t_last = system_ticks;
        return 0;
    }
    case ST_HEADERS:
    case ST_BODY: {
        int was_hdr = (s->state == ST_HEADERS);
        int r = drain(s);
        if (s->state == ST_ERROR) { drop_conn(s); return -1; }
        if (r < 0) {
            /* TCB gone (RST / freed) */
            s->conn = NULL;
            if (was_hdr) { set_err(s, "Connection closed by server"); return -1; }
            s->eof = true;
        }
        if (s->state == ST_HEADERS) {
            if (s->conn && s->conn->closed && s->conn->rx_len == 0) { drop_conn(s); set_err(s, "Empty reply from server"); return -1; }
            if (system_ticks - s->t_last > NSC_IDLE_T) { drop_conn(s); set_err(s, "Timed out waiting for response"); return -1; }
            return 0;
        }
        if (s->state == ST_BODY && !s->eof && r == 0 && system_ticks - s->t_last > NSC_IDLE_T) {
            drop_conn(s); set_err(s, "Transfer stalled (no data for 12 s)"); return -1;
        }
        if (s->eof && !s->info.chunked && s->info.content_length && s->body_raw < s->info.content_length) {
            /* bytes were lost on the way (1 KB window overflow in tcp.c): say so
             * rather than hand the consumer a page with a hole in it */
            char m[64]; int n = 0;
            const char* a = "Incomplete transfer: "; while (*a) m[n++] = *a++;
            char num[12]; int_to_str((int)s->body_raw, num); for (int i = 0; num[i]; i++) m[n++] = num[i];
            a = " of "; while (*a) m[n++] = *a++;
            int_to_str((int)s->info.content_length, num); for (int i = 0; num[i]; i++) m[n++] = num[i];
            a = " bytes"; while (*a) m[n++] = *a++;
            m[n] = '\0';
            drop_conn(s); set_err(s, m); return -1;
        }
        if (s->eof && s->conn) drop_conn(s);
        return 1;
    }
    }
    return -1;
}

int ns_read(net_stream_t* s, void* buf, int max) {
    if (!s || !buf || max <= 0) return -2;
    if (s->state == ST_ERROR) return -2;
    if (s->state != ST_BODY && s->state != ST_DONE) return 0;
    uint32_t f = irq_save();
    if (s->fifo_len > 0) {
        int n = s->fifo_len < max ? s->fifo_len : max;
        memcpy(buf, s->fifo, n);
        if (n < s->fifo_len) memcpy(s->fifo, s->fifo + n, s->fifo_len - n);
        s->fifo_len -= n;
        s->info.received += (uint32_t)n;
        irq_restore(f);
        return n;
    }
    bool eof = s->eof;
    irq_restore(f);
    if (eof) return -1;
    return 0;
}

bool ns_done(net_stream_t* s) {
    return s && (s->state == ST_ERROR || ((s->state == ST_BODY || s->state == ST_DONE) && s->eof));
}

const ns_info_t* ns_info(net_stream_t* s) { return s ? &s->info : NULL; }
const char* ns_error(net_stream_t* s) { return (s && s->state == ST_ERROR) ? s->err : ""; }

void ns_close(net_stream_t* s) {
    if (!s) return;
    uint32_t f = irq_save();
    s->state = ST_FREE;                            /* the IRQ pump skips free slots */
    irq_restore(f);
    drop_conn(s);
}

int ns_active_count(void) {
    int n = 0;
    for (int i = 0; i < NS_MAX_STREAMS; i++) if (streams[i].state != ST_FREE) n++;
    return n;
}

int ns_fetch(const char* url, uint8_t* buf, int max, ns_info_t* info_out) {
    char cur[NS_MAX_URL];
    strncpy(cur, url, NS_MAX_URL - 1); cur[NS_MAX_URL - 1] = '\0';
    for (int hop = 0; hop < 5; hop++) {
        net_stream_t* s = ns_open(cur, "GET", NULL);
        if (!s) return -1;
        int total = 0, rc;
        bool abort = false;
        for (;;) {
            rc = ns_poll(s);
            if (rc < 0) break;
            if (rc == 1) {
                const ns_info_t* in = ns_info(s);
                if (in->status >= 300 && in->status < 400 && in->location[0]) break;
                int n = ns_read(s, buf + total, max - total);
                if (n > 0) total += n;
                else if (n == -1) break;
                else if (n == -2) { rc = -1; break; }
                if (total >= max) break;              /* truncated to the caller's buffer */
            }
            if (keyboard_has_key() && keyboard_getchar() == 27) { abort = true; break; }
            __asm__ volatile("hlt");
        }
        const ns_info_t* in = ns_info(s);
        bool redirect = (rc >= 0 && in->status >= 300 && in->status < 400 && in->location[0]);
        if (info_out) *info_out = *in;
        if (redirect) strncpy(cur, in->location, NS_MAX_URL - 1);
        ns_close(s);
        if (abort || rc < 0) return -1;
        if (!redirect) return total;
    }
    return -1;
}
