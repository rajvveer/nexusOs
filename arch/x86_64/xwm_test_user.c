/* xwmtest: test client for Aurora's X11 window manager (arch/x86_64/aurora_xwm.c, tests/test_aurora_xwm.py).
 *
 *   xwmtest run [APPS]   the scripted X11 session (below); APPS: "xterm,xeyes" to also check those programs
 *   xwmtest wl           a Wayland window that reports the clipboard it is offered and copies on request (used by run)
 *
 * Dependency-free X core protocol (several connections), to the display Aurora publishes in
 * $XDG_RUNTIME_DIR/aurora-x11.env. Every step prints "[64] XWMTEST ..." lines; steps that need the user (the host
 * test: pointer, keyboard) print "XWMTEST STEP <name> READY ..." with root coordinates and wait for the X events the
 * action must produce. Checks print "XWMTEST CHECK <name> ok=0|1 ..."; the end is "XWMTEST DONE passed=N failed=M". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* format, ...) { va_list a; va_start(a, format); fputs("[64] XWMTEST ", stdout); vprintf(format, a); va_end(a); fputc('\n', stdout); fflush(stdout); }
static int passed, failed;
static bool check(const char* name, bool ok, const char* format, ...) __attribute__((format(printf, 3, 4)));
static bool check(const char* name, bool ok, const char* format, ...) {
    char detail[512]; va_list a; va_start(a, format); vsnprintf(detail, sizeof(detail), format, a); va_end(a);
    say("CHECK %s ok=%d %s", name, ok, detail); if (ok) passed++; else failed++; return ok;
}
static uint64_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void sleep_ms(int n) { struct timespec t = {n / 1000, (long)(n % 1000) * 1000000}; nanosleep(&t, NULL); }
static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* ================================================================ X connections ================================ */
#define QUEUE 512
struct xc { int fd; uint32_t base, mask, next, root; uint16_t root_w, root_h; uint16_t seq; bool dead; uint8_t queue[QUEUE][32]; int queued; };
static char display_name[32], xauthority[160];
static bool read_env(int seconds) {
    const char* runtime = getenv("XDG_RUNTIME_DIR"); char path[200]; snprintf(path, sizeof(path), "%s/aurora-x11.env", runtime ? runtime : "/run/user/1000");
    for (uint64_t end = ms() + (uint64_t)seconds * 1000; ms() < end; sleep_ms(250)) {
        FILE* f = fopen(path, "re"); if (!f) continue; char line[256];
        while (fgets(line, sizeof(line), f)) { line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "DISPLAY=", 8)) snprintf(display_name, sizeof(display_name), "%s", line + 8);
            else if (!strncmp(line, "XAUTHORITY=", 11)) snprintf(xauthority, sizeof(xauthority), "%s", line + 11); }
        fclose(f); if (display_name[0] == ':') { setenv("DISPLAY", display_name, 1); setenv("XAUTHORITY", xauthority, 1); return true; }
    }
    return false;
}
static bool xsend(struct xc* c, const void* data, size_t size) {
    const uint8_t* p = data;
    while (size && !c->dead) { ssize_t n = send(c->fd, p, size, MSG_NOSIGNAL); if (n > 0) { p += n; size -= (size_t)n; } else if (n < 0 && errno == EINTR) continue; else { c->dead = true; return false; } }
    return !c->dead;
}
static bool xrecv(struct xc* c, void* data, size_t size, int timeout_ms) {
    uint8_t* p = data; uint64_t end = ms() + (uint64_t)timeout_ms;
    while (size && !c->dead) {
        struct pollfd pf = {c->fd, POLLIN, 0}; int left = (int)(end > ms() ? end - ms() : 0);
        int r = poll(&pf, 1, left); if (r < 0 && errno == EINTR) continue; if (r <= 0) return false;
        ssize_t n = recv(c->fd, p, size, 0); if (n > 0) { p += n; size -= (size_t)n; } else if (n < 0 && errno == EINTR) continue; else { c->dead = true; return false; }
    }
    return !c->dead;
}
static uint16_t request(struct xc* c, const void* data, size_t size) { xsend(c, data, size); return ++c->seq; }
static bool cookie(uint8_t out[16]) {
    FILE* f = fopen(xauthority, "rb"); if (!f) return false; uint8_t b[512]; size_t n = fread(b, 1, sizeof(b), f), at = 0; fclose(f);
    while (at + 2 <= n) { const uint8_t* field[4]; unsigned len[4]; at += 2;
        for (int i = 0; i < 4; i++) { if (at + 2 > n) return false; len[i] = (unsigned)b[at] << 8 | b[at + 1]; at += 2; if (at + len[i] > n) return false; field[i] = b + at; at += len[i]; }
        if (len[2] == 18 && !memcmp(field[2], "MIT-MAGIC-COOKIE-1", 18) && len[3] == 16) { memcpy(out, field[3], 16); return true; } }
    return false;
}
static bool xconnect(struct xc* c) {
    memset(c, 0, sizeof(*c)); c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (c->fd < 0) return false;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/.X11-unix/X%d", atoi(display_name + 1));
    if (connect(c->fd, (struct sockaddr*)&a, sizeof(a))) { close(c->fd); c->fd = -1; return false; }
    uint8_t setup[48] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0}; size_t size = 12; uint8_t secret[16];
    if (cookie(secret)) { put16(setup + 6, 18); put16(setup + 8, 16); memcpy(setup + 12, "MIT-MAGIC-COOKIE-1", 18); memcpy(setup + 32, secret, 16); size = 48; }
    xsend(c, setup, size); uint8_t head[8]; if (!xrecv(c, head, 8, 60000)) return false;
    size_t extra = (size_t)get16(head + 6) * 4; uint8_t* d = malloc(extra ? extra : 1); if (!d || !xrecv(c, d, extra, 10000)) { free(d); return false; }
    if (head[0] != 1) { say("setup refused: %.*s", head[1], (char*)d); free(d); return false; }
    c->base = get32(d + 4); c->mask = get32(d + 8); uint16_t vendor = get16(d + 16); uint8_t formats = d[21];
    const uint8_t* s = d + 32 + ((vendor + 3u) & ~3u) + 8u * formats; c->root = get32(s); c->root_w = get16(s + 20); c->root_h = get16(s + 22);
    free(d); return true;
}
static uint32_t xid(struct xc* c) { c->next++; return c->base | ((c->next * (c->mask & -c->mask)) & c->mask); }
/* read one message (32 bytes, replies with their extra data in *extra); events go to the queue unless want_event */
static bool xread(struct xc* c, uint8_t m[32], uint8_t** extra, int timeout) {
    if (extra) *extra = NULL;
    if (!xrecv(c, m, 32, timeout)) return false;
    if (m[0] == 1 || (m[0] & 0x7F) == 35) { size_t n = (size_t)get32(m + 4) * 4; uint8_t* x = malloc(n ? n : 1); if (!x || !xrecv(c, x, n, 10000)) { free(x); return false; } if (extra) *extra = x; else free(x); }
    return true;
}
static void serve_selection(struct xc* c, const uint8_t* e);
static void incr_next(struct xc* c, const uint8_t* e);
static void queue_event(struct xc* c, const uint8_t m[32]) {
    if ((m[0] & 0x7F) == 30) { serve_selection(c, m); return; }   /* SelectionRequest: answered at once */
    if ((m[0] & 0x7F) == 28) incr_next(c, m);                     /* PropertyNotify: maybe the next INCR piece */
    if (c->queued == QUEUE) { memmove(c->queue[0], c->queue[1], (QUEUE - 1) * 32); c->queued--; }   /* the oldest goes */
    memcpy(c->queue[c->queued++], m, 32);
}
/* a reply: events in between are queued; NULL on an X error (code in *error) */
static uint8_t* xreply(struct xc* c, uint16_t seq, uint8_t first[32], int* error) {
    if (error) *error = 0;
    for (uint64_t end = ms() + 15000; ms() < end;) {
        uint8_t* extra; if (!xread(c, first, &extra, 15000)) return NULL;
        if (first[0] == 1 && get16(first + 2) == seq) return extra ? extra : calloc(1, 1);
        if (first[0] == 0 && get16(first + 2) == seq) { if (error) *error = first[1]; free(extra); return NULL; }
        if (first[0] > 1) queue_event(c, first);
        free(extra);
    }
    return NULL;
}
static void xsync(struct xc* c) { uint8_t r[4] = {43, 0, 1, 0}; uint16_t s = request(c, r, 4); uint8_t f[32]; free(xreply(c, s, f, NULL)); }   /* GetInputFocus round trip */
static uint32_t atom(struct xc* c, const char* name) {
    size_t n = strlen(name), size = 8 + ((n + 3) & ~(size_t)3); uint8_t r[256] = {16, 0}; if (size > sizeof(r)) return 0;
    put16(r + 2, (uint16_t)(size / 4)); put16(r + 4, (uint16_t)n); memcpy(r + 8, name, n);
    uint16_t s = request(c, r, size); uint8_t f[32]; uint8_t* x = xreply(c, s, f, NULL); free(x); return x ? get32(f + 8) : 0;
}
static void change_property(struct xc* c, uint32_t w, uint32_t prop, uint32_t type, uint8_t format, const void* data, uint32_t count) {
    size_t bytes = (size_t)count * (format / 8), size = 24 + ((bytes + 3) & ~(size_t)3); uint8_t* r = calloc(1, size); if (!r) return;
    r[0] = 18; put16(r + 2, (uint16_t)(size / 4)); put32(r + 4, w); put32(r + 8, prop); put32(r + 12, type); r[16] = format; put32(r + 20, count); memcpy(r + 24, data, bytes);
    request(c, r, size); free(r);
}
/* GetProperty: the value (malloc'd, NUL-terminated) and its type/format/count */
static uint8_t* get_property(struct xc* c, uint32_t w, uint32_t prop, bool del, uint32_t* type, uint8_t* format, uint32_t* count) {
    uint8_t r[24] = {20, del, 6, 0}; put32(r + 4, w); put32(r + 8, prop); put32(r + 12, 0); put32(r + 16, 0); put32(r + 20, 1 << 20);
    uint16_t s = request(c, r, 24); uint8_t f[32]; uint8_t* x = xreply(c, s, f, NULL); if (!x) return NULL;
    if (type) *type = get32(f + 8); if (format) *format = f[1]; if (count) *count = get32(f + 16);
    size_t bytes = (size_t)get32(f + 16) * (f[1] / 8); uint8_t* v = malloc(bytes + 1); if (v) { memcpy(v, x, bytes); v[bytes] = 0; } free(x); return v;
}
static uint32_t prop32(struct xc* c, uint32_t w, uint32_t prop) { uint32_t n = 0; uint8_t* v = get_property(c, w, prop, false, NULL, NULL, &n); uint32_t r = v && n ? get32(v) : 0; free(v); return r; }
static uint32_t create_window(struct xc* c, int x, int y, int w, int h, uint32_t color, bool override) {
    uint32_t id = xid(c); uint8_t r[44] = {1, 0}; put16(r + 2, override ? 11 : 10); put32(r + 4, id); put32(r + 8, c->root);
    put16(r + 12, (uint16_t)x); put16(r + 14, (uint16_t)y); put16(r + 16, (uint16_t)w); put16(r + 18, (uint16_t)h); put16(r + 20, 0); put16(r + 22, 1); put32(r + 24, 0);
    uint32_t events = 0x4 | 0x8000 | 0x20000 | 0x200000 | 0x400000 | 0x1;   /* ButtonPress Exposure StructureNotify FocusChange PropertyChange KeyPress */
    if (override) { put32(r + 28, 0x2 | 0x200 | 0x800); put32(r + 32, color); put32(r + 36, 1); put32(r + 40, events); request(c, r, 44); }
    else { put32(r + 28, 0x2 | 0x800); put32(r + 32, color); put32(r + 36, events); request(c, r, 40); }
    return id;
}
static void simple(struct xc* c, uint8_t op, uint32_t w) { uint8_t r[8] = {op, 0, 2, 0}; put32(r + 4, w); request(c, r, 8); }
static void map_window(struct xc* c, uint32_t w) { simple(c, 8, w); }
static void unmap_window(struct xc* c, uint32_t w) { simple(c, 10, w); }
static void destroy_window(struct xc* c, uint32_t w) { simple(c, 4, w); }
static void configure(struct xc* c, uint32_t w, uint16_t mask, const uint32_t* v, int n) {
    uint8_t r[40] = {12, 0}; put16(r + 2, (uint16_t)(3 + n)); put32(r + 4, w); put16(r + 8, mask); for (int i = 0; i < n; i++) put32(r + 12 + 4 * i, v[i]); request(c, r, 12 + 4 * (size_t)n);
}
static void send_event(struct xc* c, uint32_t to, uint32_t mask, const uint8_t e[32]) { uint8_t r[44] = {25, 0, 11, 0}; put32(r + 4, to); put32(r + 8, mask); memcpy(r + 12, e, 32); request(c, r, 44); }
static void client_message(struct xc* c, uint32_t to, uint32_t window, uint32_t type, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    uint8_t e[32] = {33, 32}; put32(e + 4, window); put32(e + 8, type); put32(e + 12, d0); put32(e + 16, d1); put32(e + 20, d2); put32(e + 24, d3);
    send_event(c, to, (1u << 19) | (1u << 20), e);   /* to the root: SubstructureNotify|SubstructureRedirect (EWMH) */
}
static void set_text(struct xc* c, uint32_t w, uint32_t prop, uint32_t type, const char* text) { change_property(c, w, prop, type, 8, text, (uint32_t)strlen(text)); }
/* wait for an event matching (code, window field at offset 4 or 8); NULL on timeout */
static bool want(const uint8_t* e, uint8_t code, uint32_t window) {
    uint8_t k = e[0] & 0x7F; if (k != code) return false; if (!window) return true;
    if (k == 9 || k == 10 || k == 12 || k == 28 || k == 33 || k == 4 || k == 5) return get32(e + (k == 4 || k == 5 ? 12 : 4)) == window;
    return get32(e + 8) == window || get32(e + 4) == window;
}
static bool wait_event(struct xc* c, uint8_t code, uint32_t window, int timeout, uint8_t out[32], bool (*extra)(const uint8_t*, void*), void* data) {
    for (uint64_t end = ms() + (uint64_t)timeout;;) {
        for (int i = 0; i < c->queued; i++) if (want(c->queue[i], code, window) && (!extra || extra(c->queue[i], data))) {
            if (out) memcpy(out, c->queue[i], 32); memmove(c->queue[i], c->queue[i + 1], (size_t)(c->queued - i - 1) * 32); c->queued--; return true; }
        uint64_t now = ms(); if (now >= end || c->dead) return false;
        uint8_t m[32]; uint8_t* x; if (!xread(c, m, &x, (int)(end - now))) { if (c->dead) return false; continue; }
        free(x); if (m[0] > 1) queue_event(c, m); else if (m[0] == 0) say("X error code=%u major=%u value=0x%x", m[1], m[10], get32(m + 4));
    }
}
static void pump(struct xc* c, int ms_) { uint8_t e[32]; wait_event(c, 0xFF, 0, ms_, e, NULL, NULL); }   /* read (and queue) events for a while */
/* ================================================================ Aurora's control socket ===================== */
static char* control(const char* command) {
    const char* runtime = getenv("XDG_RUNTIME_DIR"); struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/aurora-control", runtime ? runtime : "/run/user/1000");
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (s < 0 || connect(s, (struct sockaddr*)&a, sizeof(a))) { if (s >= 0) close(s); return NULL; }
    char line[256]; snprintf(line, sizeof(line), "%s\n", command); if (write(s, line, strlen(line)) < 0) { close(s); return NULL; }
    size_t cap = 65536, used = 0; char* out = malloc(cap); if (!out) { close(s); return NULL; }
    for (uint64_t end = ms() + 10000; ms() < end && used < cap - 1;) {
        struct pollfd pf = {s, POLLIN, 0}; if (poll(&pf, 1, 500) <= 0) continue;
        ssize_t n = read(s, out + used, cap - 1 - used); if (n <= 0) break; used += (size_t)n; out[used] = 0;
        if (strstr(out, "\nOK\n") || !strncmp(out, "OK\n", 3) || strstr(out, "ERROR")) break;
    }
    out[used] = 0; close(s); return out;
}
static bool control_has(const char* command, const char* needle) { char* t = control(command); bool r = t && strstr(t, needle); free(t); return r; }
static void control_show(const char* command, const char* match) {   /* the lines containing match */
    char* t = control(command); if (!t) { say("control %s: no answer", command); return; }
    for (char* save = NULL, *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) if (!match || strstr(l, match)) say("AURORA %s", l);
    free(t);
}
/* ================================================================ the clipboard (X side) ======================= */
static uint32_t A_UTF8, A_TARGETS, A_CLIPBOARD, A_INCR; static const char* owned_text; static uint32_t owned_window;
static struct { uint32_t requestor, property, type; size_t size, sent; bool active; } incr_out;   /* our INCR transfer (ICCCM 2.7.2) */
static char* big_text(size_t n) {   /* a-z lines, newline every 64 bytes; the host computes the same */
    char* b = malloc(n + 1); if (!b) return NULL; for (size_t i = 0; i < n; i++) b[i] = (char)(i % 64 == 63 ? '\n' : 'a' + (int)(i % 26)); b[n] = 0; return b;
}
static uint32_t text_sum(const char* s, size_t n) { uint32_t v = 0; for (size_t i = 0; i < n; i++) v += (uint8_t)s[i]; return v; }
static void incr_next(struct xc* c, const uint8_t* e) {
    if (!incr_out.active || get32(e + 4) != incr_out.requestor || get32(e + 8) != incr_out.property || e[16] != 1) return;
    size_t n = incr_out.size - incr_out.sent < 65536 ? incr_out.size - incr_out.sent : 65536;
    change_property(c, incr_out.requestor, incr_out.property, incr_out.type, 8, owned_text + incr_out.sent, (uint32_t)n); incr_out.sent += n;
    if (!n) { incr_out.active = false; say("INCR sent bytes=%zu", incr_out.size); }
}
static void serve_selection(struct xc* c, const uint8_t* e) {
    uint32_t time = get32(e + 4), requestor = get32(e + 12), selection = get32(e + 16), target = get32(e + 20), property = get32(e + 24);
    uint8_t n[32] = {31}; put32(n + 4, time); put32(n + 8, requestor); put32(n + 12, selection); put32(n + 16, target); put32(n + 20, 0);
    if (owned_text && selection == A_CLIPBOARD) {
        if (target == A_TARGETS) { uint32_t list[3] = {A_TARGETS, A_UTF8, 31}; change_property(c, requestor, property, 4, 32, list, 3); put32(n + 20, property); }
        else if ((target == A_UTF8 || target == 31) && strlen(owned_text) > 65536) {   /* too big for one request: INCR */
            uint8_t a[16] = {2, 0, 4, 0}; put32(a + 4, requestor); put32(a + 8, 1u << 11); put32(a + 12, 1u << 22); request(c, a, 16);   /* PropertyChange */
            uint32_t size = (uint32_t)strlen(owned_text); change_property(c, requestor, property, A_INCR, 32, &size, 1); put32(n + 20, property);
            incr_out.requestor = requestor; incr_out.property = property; incr_out.type = target; incr_out.size = size; incr_out.sent = 0; incr_out.active = true; }
        else if (target == A_UTF8 || target == 31) { set_text(c, requestor, property, target, owned_text); put32(n + 20, property); }
    }
    send_event(c, requestor, 0, n); say("served selection target=%u property=%s", target, get32(n + 20) ? "set" : "none");
}
static void own_clipboard(struct xc* c, uint32_t w, const char* text) {
    owned_text = text; owned_window = w; uint8_t r[16] = {22, 0, 4, 0}; put32(r + 4, w); put32(r + 8, A_CLIPBOARD); put32(r + 12, 0); request(c, r, 16);
}
static char* paste(struct xc* c, uint32_t w, uint32_t target) {   /* ConvertSelection CLIPBOARD -> property _XWMTEST on w */
    uint32_t prop = atom(c, "_XWMTEST"); uint8_t r[24] = {24, 0, 6, 0}; put32(r + 4, w); put32(r + 8, A_CLIPBOARD); put32(r + 12, target); put32(r + 16, prop); put32(r + 20, 0);
    request(c, r, 24); uint8_t e[32];
    if (!wait_event(c, 31, 0, 10000, e, NULL, NULL) || !get32(e + 20)) return NULL;
    uint32_t type = 0; uint8_t* v = get_property(c, w, prop, true, &type, NULL, NULL);
    if (v && type == A_INCR) {   /* chunks until an empty one */
        free(v); size_t used = 0; char* all = NULL;
        for (int i = 0; i < c->queued;) {   /* the INCR announcement's own NewValue is not a piece */
            if ((c->queue[i][0] & 0x7F) == 28 && get32(c->queue[i] + 4) == w && get32(c->queue[i] + 8) == prop) { memmove(c->queue[i], c->queue[i + 1], (size_t)(c->queued - i - 1) * 32); c->queued--; }
            else i++; }
        for (;;) { if (!wait_event(c, 28, w, 10000, e, NULL, NULL)) { free(all); return NULL; } if (get32(e + 8) != prop || e[16] != 0) continue;
            uint32_t count = 0; type = 0; uint8_t* part = get_property(c, w, prop, true, &type, NULL, &count); if (!part) { free(all); return NULL; }
            if (!count && !type) { free(part); continue; }   /* gone already: not the end marker */
            if (!count) { free(part); return all ? all : strdup(""); }
            char* grown = realloc(all, used + count + 1); if (!grown) { free(part); free(all); return NULL; } all = grown; memcpy(all + used, part, count); used += count; all[used] = 0; free(part); }
    }
    return (char*)v;
}
/* ================================================================ geometry from events ======================== */
struct geo { int x, y, w, h; bool synthetic; };
static bool synthetic_configure(const uint8_t* e, void* data) { (void)data; return (e[0] & 0x80) != 0; }
static bool last_configure(struct xc* c, uint32_t w, struct geo* g, int timeout) {   /* the latest synthetic ConfigureNotify within timeout */
    uint8_t e[32]; bool got = false; uint64_t end = ms() + (uint64_t)timeout;
    while (wait_event(c, 22, w, got ? 400 : (int)(end > ms() ? end - ms() : 1), e, synthetic_configure, NULL)) {
        g->x = (int16_t)get16(e + 16); g->y = (int16_t)get16(e + 18); g->w = get16(e + 20); g->h = get16(e + 22); g->synthetic = true; got = true; if (ms() > end) break; }
    return got;
}
static bool geometry_is(const uint8_t* e, void* data) { const struct geo* g = data; return (e[0] & 0x80) && (g->w <= 0 || abs(get16(e + 20) - g->w) <= 2) && (g->h <= 0 || abs(get16(e + 22) - g->h) <= 2) &&
    (g->x == -99999 || abs((int16_t)get16(e + 16) - g->x) <= 2) && (g->y == -99999 || abs((int16_t)get16(e + 18) - g->y) <= 2); }
static bool wait_geometry(struct xc* c, uint32_t w, int x, int y, int width, int height, int timeout, struct geo* got) {
    struct geo want_g = {x, y, width, height, true}; uint8_t e[32];
    if (!wait_event(c, 22, w, timeout, e, geometry_is, &want_g)) return false;
    if (got) { got->x = (int16_t)get16(e + 16); got->y = (int16_t)get16(e + 18); got->w = get16(e + 20); got->h = get16(e + 22); }
    return true;
}
static bool net_state_has(struct xc* c, uint32_t w, uint32_t a) {
    uint32_t n = 0; uint8_t* v = get_property(c, w, atom(c, "_NET_WM_STATE"), false, NULL, NULL, &n); bool r = false;
    for (uint32_t i = 0; v && i < n; i++) if (get32(v + 4 * i) == a) r = true; free(v); return r;
}
static uint32_t parent_of(struct xc* c, uint32_t w) {
    uint8_t r[8] = {15, 0, 2, 0}; put32(r + 4, w); uint16_t s = request(c, r, 8); uint8_t f[32]; uint8_t* x = xreply(c, s, f, NULL); free(x); return x ? get32(f + 12) : 0;
}
static int stack_index(struct xc* c, uint32_t top) {   /* position among the root's children (higher = above) */
    uint8_t r[8] = {15, 0, 2, 0}; put32(r + 4, c->root); uint16_t s = request(c, r, 8); uint8_t f[32]; uint8_t* x = xreply(c, s, f, NULL); int at = -1;
    for (int i = 0; x && i < get16(f + 16); i++) if (get32(x + 4 * i) == top) at = i;
    free(x); return at;
}
static bool real_geometry(struct xc* c, uint32_t w, int* x, int* y, int* width, int* height) {
    uint8_t r[16] = {40, 0, 4, 0}; put32(r + 4, w); put32(r + 8, c->root); put32(r + 12, 0); uint16_t s = request(c, r, 16); uint8_t f[32]; uint8_t* v = xreply(c, s, f, NULL);
    if (!v) return false; free(v); *x = (int16_t)get16(f + 12); *y = (int16_t)get16(f + 14);
    uint8_t g[8] = {14, 0, 2, 0}; put32(g + 4, w); s = request(c, g, 8); v = xreply(c, s, f, NULL); if (!v) return false; free(v);
    *width = get16(f + 16); *height = get16(f + 18); return true;
}
static uint32_t focus_window(struct xc* c) { uint8_t r[4] = {43, 0, 1, 0}; uint16_t s = request(c, r, 4); uint8_t f[32]; uint8_t* x = xreply(c, s, f, NULL); free(x); return x ? get32(f + 8) : 0; }
static void set_normal(struct xc* c, uint32_t w, const char* name, const char* utf8, const char* cls, bool delete_protocol) {
    set_text(c, w, 39, 31, name); set_text(c, w, atom(c, "_NET_WM_NAME"), A_UTF8, utf8);
    change_property(c, w, 67, 31, 8, cls, (uint32_t)(strlen(cls) + 1 + strlen(cls + strlen(cls) + 1) + 1));
    uint32_t protocols[2] = {atom(c, "WM_DELETE_WINDOW"), atom(c, "WM_TAKE_FOCUS")};
    if (delete_protocol) change_property(c, w, atom(c, "WM_PROTOCOLS"), 4, 32, protocols, 1);
    uint32_t hints[9] = {1, 1}; change_property(c, w, 35, 35, 32, hints, 9);
    uint32_t pid = (uint32_t)getpid(); change_property(c, w, atom(c, "_NET_WM_PID"), 6, 32, &pid, 1);
}
static void fill(struct xc* c, uint32_t w, uint32_t gc, uint32_t color, int x, int y, int width, int height) {
    uint8_t g[16] = {56, 0, 4, 0}; put32(g + 4, gc); put32(g + 8, 4); put32(g + 12, color); request(c, g, 16);
    uint8_t r[20] = {70, 0, 5, 0}; put32(r + 4, w); put32(r + 8, gc); put16(r + 12, (uint16_t)x); put16(r + 14, (uint16_t)y); put16(r + 16, (uint16_t)width); put16(r + 18, (uint16_t)height); request(c, r, 20);
}
/* ================================================================ the Wayland side ============================ */
static struct wl_display* wd; static struct wl_compositor* wcomp; static struct wl_shm* wshm; static struct xdg_wm_base* wshell; static struct wl_seat* wseat;
static struct wl_data_device_manager* wmanager; static struct wl_data_device* wdevice; static bool wconfigured, wfocused; static uint32_t wserial;
static struct wl_data_offer* woffer; static bool woffer_text;
static void wg(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    (void)d;
    if (!strcmp(iface, "wl_compositor")) wcomp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) wshm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) wshell = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, "wl_seat")) wseat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (!strcmp(iface, "wl_data_device_manager")) wmanager = wl_registry_bind(r, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
    else if (!strcmp(iface, "xwayland_shell_v1")) say("WL SEES xwayland_shell_v1 (must not)");
}
static void wgr(void* d, struct wl_registry* r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener wreg = {wg, wgr};
static void wping(void* d, struct xdg_wm_base* s, uint32_t serial) { (void)d; xdg_wm_base_pong(s, serial); }
static const struct xdg_wm_base_listener wshell_l = {wping};
static void wxconf(void* d, struct xdg_surface* x, uint32_t serial) { (void)d; xdg_surface_ack_configure(x, serial); wconfigured = true; }
static const struct xdg_surface_listener wxdg_l = {wxconf};
static void wtconf(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void wtclose(void* d, struct xdg_toplevel* t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener wtop_l = {.configure = wtconf, .close = wtclose};
static void wkmap(void* d, struct wl_keyboard* k, uint32_t f, int32_t fd, uint32_t size) { (void)d; (void)k; (void)f; (void)size; close(fd); }
static void wkenter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys) { (void)d; (void)k; (void)s; (void)keys; wfocused = true; wserial = serial; say("WL focus in"); }
static void wkleave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s) { (void)d; (void)k; (void)serial; (void)s; wfocused = false; say("WL focus out"); }
static void wkkey(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) { (void)d; (void)k; (void)t; (void)key; if (state) wserial = serial; }
static void wkmods(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g; }
static void wkrep(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static const struct wl_keyboard_listener wkb = {wkmap, wkenter, wkleave, wkkey, wkmods, wkrep};
static void woffer_type(void* d, struct wl_data_offer* o, const char* mime) { (void)d; (void)o; if (!strcmp(mime, "text/plain;charset=utf-8")) woffer_text = true; }
static const struct wl_data_offer_listener woffer_l = {.offer = woffer_type};
static void wdd_offer(void* d, struct wl_data_device* dev, struct wl_data_offer* o) { (void)d; (void)dev; woffer_text = false; wl_data_offer_add_listener(o, &woffer_l, NULL); }
static void wdd_enter(void* d, struct wl_data_device* dev, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o) { (void)d; (void)dev; (void)serial; (void)s; (void)x; (void)y; (void)o; }
static void wdd_leave(void* d, struct wl_data_device* dev) { (void)d; (void)dev; }
static void wdd_motion(void* d, struct wl_data_device* dev, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dev; (void)t; (void)x; (void)y; }
static void wdd_drop(void* d, struct wl_data_device* dev) { (void)d; (void)dev; }
static void wdd_selection(void* d, struct wl_data_device* dev, struct wl_data_offer* o) { (void)d; (void)dev; if (woffer && woffer != o) wl_data_offer_destroy(woffer); woffer = o; }
static const struct wl_data_device_listener wdd_l = {.data_offer = wdd_offer, .enter = wdd_enter, .leave = wdd_leave, .motion = wdd_motion, .drop = wdd_drop, .selection = wdd_selection};
static const char* wcopy_text;
static struct wl_data_source* wsource;   /* our copy while it is the selection (cancelled: someone else's) */
static void wsrc_target(void* d, struct wl_data_source* s, const char* mime) { (void)d; (void)s; (void)mime; }
static void wsrc_send(void* d, struct wl_data_source* s, const char* mime, int32_t fd) {
    (void)d; (void)s; size_t size = strlen(wcopy_text), done = 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);   /* a reader that stops reading ends this at the deadline */
    for (uint64_t end = ms() + 30000; done < size && ms() < end;) {   /* the pipe may be small: wait while it is full */
        ssize_t n = write(fd, wcopy_text + done, size - done); if (n > 0) { done += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) { struct pollfd pf = {fd, POLLOUT, 0}; poll(&pf, 1, 200); continue; }
        break; }
    close(fd); say("WL sent %s bytes=%zu", mime, done);
}
static void wsrc_cancelled(void* d, struct wl_data_source* s) { (void)d; if (s == wsource) wsource = NULL; wl_data_source_destroy(s); say("WL source cancelled"); }
static const struct wl_data_source_listener wsrc_l = {.target = wsrc_target, .send = wsrc_send, .cancelled = wsrc_cancelled};
static void wl_read_offer(void) {
    if (!woffer || !woffer_text) return;
    /* the offer of our own copy: a toolkit pastes that from memory - receiving it through a pipe this one thread must
     * also write (wsrc_send) deadlocks once the copy is larger than the pipe */
    if (wsource) { wl_data_offer_destroy(woffer); woffer = NULL; return; }
    struct wl_data_offer* o = woffer; woffer = NULL; int p[2]; if (pipe2(p, O_CLOEXEC)) return;
    wl_data_offer_receive(o, "text/plain;charset=utf-8", p[1]); close(p[1]); wl_display_flush(wd);
    size_t cap = 4u << 20, used = 0; char* text = malloc(cap + 1); if (!text) { close(p[0]); return; }
    struct pollfd pf[2] = {{p[0], POLLIN, 0}, {wl_display_get_fd(wd), POLLIN, 0}};
    for (uint64_t end = ms() + 30000; used < cap && ms() < end;) { wl_display_flush(wd); if (poll(pf, 2, 100) <= 0) continue;
        if (pf[1].revents & POLLIN) wl_display_dispatch(wd);
        if (pf[0].revents & (POLLIN | POLLHUP)) { ssize_t n = read(p[0], text + used, cap - used); if (n <= 0) break; used += (size_t)n; } }
    close(p[0]); text[used] = 0; wl_data_offer_destroy(o);
    if (used <= 200) say("WL selection: %s", text); else say("WL selection bytes=%zu sum=%u", used, text_sum(text, used));
    free(text);
}
static int wayland_side(void) {   /* stdin: "copy TEXT" lines from the X side */
    wd = wl_display_connect(NULL); if (!wd) { say("WL cannot connect"); return 1; }
    struct wl_registry* reg = wl_display_get_registry(wd); wl_registry_add_listener(reg, &wreg, NULL);
    if (wl_display_roundtrip(wd) < 0 || !wcomp || !wshm || !wshell || !wseat || !wmanager) { say("WL globals missing"); return 1; }
    xdg_wm_base_add_listener(wshell, &wshell_l, NULL);
    wdevice = wl_data_device_manager_get_data_device(wmanager, wseat); wl_data_device_add_listener(wdevice, &wdd_l, NULL);
    struct wl_keyboard* kb = wl_seat_get_keyboard(wseat); wl_keyboard_add_listener(kb, &wkb, NULL);
    struct wl_surface* s = wl_compositor_create_surface(wcomp); struct xdg_surface* x = xdg_wm_base_get_xdg_surface(wshell, s); xdg_surface_add_listener(x, &wxdg_l, NULL);
    struct xdg_toplevel* t = xdg_surface_get_toplevel(x); xdg_toplevel_add_listener(t, &wtop_l, NULL); xdg_toplevel_set_app_id(t, "nexus.xwmtest.wayland"); xdg_toplevel_set_title(t, "XWM Wayland side");
    wl_surface_commit(s); while (!wconfigured) if (wl_display_dispatch(wd) < 0) return 1;
    int fd = memfd_create("xwm", MFD_CLOEXEC); if (fd < 0 || ftruncate(fd, 320 * 200 * 4)) return 1;
    uint32_t* px = mmap(NULL, 320 * 200 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (px == MAP_FAILED) return 1; for (int i = 0; i < 320 * 200; i++) px[i] = 0xFF30A050;
    struct wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, 320 * 200 * 4); struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, 320, 200, 1280, WL_SHM_FORMAT_ARGB8888);
    wl_surface_attach(s, b, 0, 0); wl_surface_damage(s, 0, 0, 320, 200); wl_surface_commit(s); say("WL ready");
    char line[512]; size_t used = 0;
    for (;;) {
        wl_display_flush(wd); struct pollfd pf[2] = {{wl_display_get_fd(wd), POLLIN, 0}, {0, POLLIN, 0}};
        if (poll(pf, 2, 200) < 0 && errno != EINTR) return 1;
        if ((pf[0].revents & POLLIN) && wl_display_dispatch(wd) < 0) return 1;
        if (wfocused) wl_read_offer();
        if (pf[1].revents & (POLLIN | POLLHUP)) { ssize_t n = read(0, line + used, sizeof(line) - 1 - used); if (n <= 0) return 0; used += (size_t)n; line[used] = 0;
            char* nl; while ((nl = strchr(line, '\n'))) { *nl = 0;
                if (!strncmp(line, "copy ", 5) && wfocused) { static char text[256]; snprintf(text, sizeof(text), "%s", line + 5); wcopy_text = text;
                    struct wl_data_source* src = wl_data_device_manager_create_data_source(wmanager); wl_data_source_add_listener(src, &wsrc_l, NULL);
                    wl_data_source_offer(src, "text/plain;charset=utf-8"); wl_data_source_offer(src, "text/plain"); wl_data_device_set_selection(wdevice, src, wserial); wsource = src; say("WL copied %s", text); }
                else if (!strncmp(line, "copybig ", 8) && wfocused) { size_t n = strtoul(line + 8, NULL, 10); wcopy_text = big_text(n ? n : 1);
                    struct wl_data_source* src = wl_data_device_manager_create_data_source(wmanager); wl_data_source_add_listener(src, &wsrc_l, NULL);
                    wl_data_source_offer(src, "text/plain;charset=utf-8"); wl_data_device_set_selection(wdevice, src, wserial); wsource = src; say("WL copied bytes=%zu", n); }
                else if (!strncmp(line, "copy", 4)) say("WL cannot copy: not focused");
                size_t rest = used - (size_t)(nl + 1 - line); memmove(line, nl + 1, rest); used = rest; line[used] = 0; } }
    }
}

/* ================================================================ the scripted session ======================== */
static struct xc X;
struct win { uint32_t id; int x, y, w, h, l, r, t, b; };
static void refresh(struct win* w) { pump(&X, 600); real_geometry(&X, w->id, &w->x, &w->y, &w->w, &w->h); }
static void read_extents(struct win* w) {
    uint32_t n = 0; uint8_t* v = get_property(&X, w->id, atom(&X, "_NET_FRAME_EXTENTS"), false, NULL, NULL, &n);
    if (v && n >= 4) { w->l = (int)get32(v); w->r = (int)get32(v + 4); w->t = (int)get32(v + 8); w->b = (int)get32(v + 12); } free(v);
}
static bool wait_mapped(struct win* w, int timeout) {
    uint8_t e[32]; if (!wait_event(&X, 19, w->id, timeout, e, NULL, NULL)) return false;
    struct geo g; if (last_configure(&X, w->id, &g, 2500)) { w->x = g.x; w->y = g.y; w->w = g.w; w->h = g.h; }
    read_extents(w); return true;
}
static bool aurora_line(uint32_t id, const char* field) {   /* control "x11": the window's line has field */
    char needle[48]; snprintf(needle, sizeof(needle), "id=0x%x ", id); char* t = control("x11"); bool ok = false;
    char* l = t ? strstr(t, needle) : NULL; if (l) { char* end = strchr(l, '\n'); if (end) *end = 0; ok = strstr(l, field) != NULL; }
    free(t); return ok;
}
static bool wait_aurora(uint32_t id, const char* field, int timeout) {
    for (uint64_t end = ms() + (uint64_t)timeout; ms() < end; pump(&X, 200)) if (aurora_line(id, field)) return true;
    return false;
}
static bool wait_focus_in(uint32_t id, int timeout) { uint8_t e[32]; return wait_event(&X, 9, id, timeout, e, NULL, NULL); }
static int count_windows(void) { char* t = control("windows"); int n = 0; for (char* p = t; p && (p = strstr(p, "window app=")); p++) n++; free(t); return n; }
static bool wait_key(int timeout) { uint8_t e[32]; return wait_event(&X, 2, 0, timeout, e, NULL, NULL); }   /* the host's "go on" */
static void step(const char* name, const char* format, ...) __attribute__((format(printf, 2, 3)));
static void step(const char* name, const char* format, ...) {
    char text[256]; va_list a; va_start(a, format); vsnprintf(text, sizeof(text), format, a); va_end(a); X.queued = 0; say("STEP %s READY %s", name, text);
}
static uint32_t gc_for(uint32_t w) { uint32_t gc = xid(&X); uint8_t r[16] = {55, 0, 4, 0}; put32(r + 4, gc); put32(r + 8, w); put32(r + 12, 0); request(&X, r, 16); return gc; }
static void state_message(uint32_t w, uint32_t action, uint32_t a1, uint32_t a2) { client_message(&X, X.root, w, atom(&X, "_NET_WM_STATE"), action, a1, a2, 1); }
static pid_t spawn(char* const argv[], int* to_child) {
    int p[2] = {-1, -1}; if (to_child && pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (!pid) { if (to_child) { dup2(p[0], 0); } execv(argv[0], argv); _exit(127); }
    if (to_child) { close(p[0]); *to_child = p[1]; }
    return pid;
}
static bool wait_line_file(const char* path, const char* text, int timeout) {
    for (uint64_t end = ms() + (uint64_t)timeout; ms() < end; pump(&X, 300)) { FILE* f = fopen(path, "re"); if (!f) continue; char line[256] = ""; bool ok = fgets(line, sizeof(line), f) && strstr(line, text); fclose(f); if (ok) return true; }
    return false;
}
static void hostile(struct win* A);
static void crash_client(void);
static void real_apps(const char* apps);
static void xwayland_restart(void) {   /* Xwayland killed: every X client loses it; the next connection starts a new one */
    char* t = control("x11"); char* at = t ? strstr(t, " pid=") : NULL; int pid = at ? atoi(at + 5) : 0; free(t);
    if (!check("xwayland_pid", pid > 1, "Xwayland pid=%d", pid)) return;
    kill(pid, SIGKILL); for (uint64_t end = ms() + 15000; ms() < end && !X.dead;) { uint8_t m[32]; uint8_t* x; if (!xread(&X, m, &x, 500) && X.dead) break; free(x); }
    check("xwayland_killed", X.dead, "the X connection ended with Xwayland");
    close(X.fd); sleep_ms(4000); bool again = false;
    for (uint64_t end = ms() + 60000; ms() < end && !again; sleep_ms(500)) again = xconnect(&X);
    struct win R = {.id = again ? create_window(&X, 0, 0, 220, 140, 0x308080, false) : 0};
    if (again) { set_normal(&X, R.id, "XWM Again", "XWM Again", "xwmagain\0XwmAgain", true); map_window(&X, R.id); }
    check("xwayland_restarted", again && wait_mapped(&R, 30000) && wait_aurora(R.id, "surface=1", 30000), "a new Xwayland on the same display manages new windows");
}
static int run(const char* apps) {
    say("START pid=%d", (int)getpid());
    if (!read_env(180)) { say("FAIL no aurora-x11.env (rootless X11 not offered)"); return 1; }
    uint64_t t0 = ms(); bool connected = xconnect(&X);
    if (!check("connect", connected, "display=%s in %llu ms (Xwayland starts on demand)", display_name, (unsigned long long)(ms() - t0))) return 1;
    say("CONNECTED display=%s root=%ux%u", display_name, X.root_w, X.root_h);
    A_UTF8 = atom(&X, "UTF8_STRING"); A_TARGETS = atom(&X, "TARGETS"); A_CLIPBOARD = atom(&X, "CLIPBOARD"); A_INCR = atom(&X, "INCR");
    /* ---- the window manager announces itself ---- */
    char name[64] = ""; uint32_t wm = 0;
    for (uint64_t end = ms() + 20000; ms() < end && !name[0]; sleep_ms(200)) {
        wm = prop32(&X, X.root, atom(&X, "_NET_SUPPORTING_WM_CHECK"));
        if (wm) { uint8_t* v = get_property(&X, wm, atom(&X, "_NET_WM_NAME"), false, NULL, NULL, NULL); if (v) snprintf(name, sizeof(name), "%s", (char*)v); free(v); } }
    check("wm_check", !strcmp(name, "Aurora") && prop32(&X, wm, atom(&X, "_NET_SUPPORTING_WM_CHECK")) == wm, "_NET_SUPPORTING_WM_CHECK=0x%x name=%s", wm, name);
    { uint32_t n = 0; uint8_t* v = get_property(&X, X.root, atom(&X, "_NET_SUPPORTED"), false, NULL, NULL, &n); int found = 0;
      const char* need[] = {"_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_ACTIVE_WINDOW", "_NET_WM_MOVERESIZE", "_NET_FRAME_EXTENTS", "_NET_CLIENT_LIST", "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_CLOSE_WINDOW"};
      for (unsigned k = 0; k < sizeof(need) / sizeof(*need); k++) { uint32_t a = atom(&X, need[k]); for (uint32_t i = 0; v && i < n; i++) if (get32(v + 4 * i) == a) { found++; break; } }
      check("net_supported", found == (int)(sizeof(need) / sizeof(*need)), "atoms=%u needed=%d/%d", n, found, (int)(sizeof(need) / sizeof(*need))); free(v); }
    uint32_t WMS_OWNER; { uint8_t r[8] = {23, 0, 2, 0}; put32(r + 4, atom(&X, "WM_S0")); uint16_t s = request(&X, r, 8); uint8_t f[32]; uint8_t* x = xreply(&X, s, f, NULL); WMS_OWNER = x ? get32(f + 8) : 0; free(x); }
    check("wm_s0", WMS_OWNER == wm, "WM_S0 owner=0x%x", WMS_OWNER);
    /* ---- map, title, dock entry ---- */
    struct win A = {.id = create_window(&X, 0, 0, 400, 300, 0x2060C0, false)};
    set_normal(&X, A.id, "XWM Test A", "XWM Test \xc3\x84 \xe2\x9c\x93", "xwmtest\0XwmTest", true);
    { uint32_t hints[18] = {16 | 32, 0, 0, 0, 0, 200, 150, 1200, 900}; change_property(&X, A.id, 40, 41, 32, hints, 18); }
    {   /* _NET_WM_ICON: a 16x16 and a 96x96 picture (the dock takes the 96 one: the smallest of at least 64) */
        static uint32_t icon[2 + 16 * 16 + 2 + 96 * 96]; uint32_t k = 0; icon[k++] = 16; icon[k++] = 16; for (int i = 0; i < 16 * 16; i++) icon[k++] = 0xFFFF0000;
        icon[k++] = 96; icon[k++] = 96; for (int i = 0; i < 96 * 96; i++) icon[k++] = 0xFF00FF40;
        change_property(&X, A.id, atom(&X, "_NET_WM_ICON"), 6, 32, icon, k); }
    map_window(&X, A.id);
    bool mapped = wait_mapped(&A, 30000);
    check("map", mapped && parent_of(&X, A.id) != X.root && A.t == 28 && A.l == 4, "A=0x%x parent=0x%x root=%d,%d %dx%d extents=%d,%d,%d,%d", A.id, parent_of(&X, A.id), A.x, A.y, A.w, A.h, A.l, A.r, A.t, A.b);
    check("wm_state", prop32(&X, A.id, atom(&X, "WM_STATE")) == 1, "WM_STATE=%u", prop32(&X, A.id, atom(&X, "WM_STATE")));
    check("aurora_window", wait_aurora(A.id, "surface=1", 30000) && aurora_line(A.id, "app=xwmtest") && aurora_line(A.id, "title=XWM Test \xc3\x84 \xe2\x9c\x93"), "managed, shown, app id and UTF-8 title");
    check("dock_entry", control_has("windows", "app=xwmtest title=XWM Test \xc3\x84"), "Aurora's window list (dock, switcher) has it");
    check("net_wm_icon", aurora_line(A.id, "icon=96x96"), "_NET_WM_ICON read (the 96x96 picture chosen for the dock)");
    check("focus_on_map", wait_focus_in(A.id, 10000) && focus_window(&X) == A.id, "FocusIn, GetInputFocus=0x%x", focus_window(&X));
    check("net_active", prop32(&X, X.root, atom(&X, "_NET_ACTIVE_WINDOW")) == A.id, "_NET_ACTIVE_WINDOW=0x%x", prop32(&X, X.root, atom(&X, "_NET_ACTIVE_WINDOW")));
    { uint32_t n = 0; uint8_t* v = get_property(&X, X.root, atom(&X, "_NET_CLIENT_LIST"), false, NULL, NULL, &n); bool in = false; for (uint32_t i = 0; v && i < n; i++) if (get32(v + 4 * i) == A.id) in = true; free(v); check("client_list", in, "_NET_CLIENT_LIST has A"); }
    uint32_t gcA = gc_for(A.id); fill(&X, A.id, gcA, 0x2060C0, 0, 0, 4000, 4000); xsync(&X);
    say("WINDOW A id=0x%x x=%d y=%d w=%d h=%d frame_x=%d frame_y=%d color=2060c0", A.id, A.x, A.y, A.w, A.h, A.x - A.l, A.y - A.t);
    /* ---- focus by click: B takes the focus, a click gives it back to A ---- */
    struct win B = {.id = create_window(&X, 0, 0, 260, 180, 0x20A040, false)};
    set_normal(&X, B.id, "XWM Test B", "XWM Test B", "xwmtest\0XwmTest", true);
    { uint32_t both[2] = {atom(&X, "WM_DELETE_WINDOW"), atom(&X, "WM_TAKE_FOCUS")}; change_property(&X, B.id, atom(&X, "WM_PROTOCOLS"), 4, 32, both, 2); }
    map_window(&X, B.id);
    check("map_b", wait_mapped(&B, 20000) && wait_aurora(B.id, "surface=1", 20000) && wait_focus_in(B.id, 10000), "B=0x%x root=%d,%d", B.id, B.x, B.y);
    { uint8_t e[32]; bool take = false; uint32_t P = atom(&X, "WM_PROTOCOLS"), T = atom(&X, "WM_TAKE_FOCUS");
      for (uint64_t end = ms() + 5000; ms() < end && !take;) { if (!wait_event(&X, 33, B.id, (int)(end - ms()), e, NULL, NULL)) break; take = get32(e + 8) == P && get32(e + 12) == T; }
      check("take_focus", take, "WM_TAKE_FOCUS for a window that lists it"); }
    uint32_t gcB = gc_for(B.id); fill(&X, B.id, gcB, 0x20A040, 0, 0, 4000, 4000); xsync(&X);
    { int px = A.x + 20, py = A.y + A.h - 20; if (px >= B.x - B.l && px < B.x + B.w + B.r && py >= B.y - B.t && py < B.y + B.h + B.b) px = A.x + A.w - 20;
      step("focus", "x=%d y=%d", px, py);
      check("focus_click", wait_focus_in(A.id, 90000), "FocusIn on A after the click");
      pump(&X, 300);
      check("focus_active", prop32(&X, X.root, atom(&X, "_NET_ACTIVE_WINDOW")) == A.id && focus_window(&X) == A.id, "_NET_ACTIVE_WINDOW=0x%x", prop32(&X, X.root, atom(&X, "_NET_ACTIVE_WINDOW")));
      check("focus_raise", stack_index(&X, parent_of(&X, A.id)) > stack_index(&X, parent_of(&X, B.id)), "A's frame above B's in the X stacking order");
      uint8_t e[32]; check("click_passed", wait_event(&X, 4, A.id, 5000, e, NULL, NULL), "the click also reached A (ButtonPress)"); }
    /* ---- move by the title bar ---- */
    {   int tx = A.x - A.l + 120, ty = A.y - A.t + 12; step("move", "x=%d y=%d dx=120 dy=80", tx, ty);
        struct geo g; bool moved = wait_geometry(&X, A.id, A.x + 120, A.y + 80, A.w, A.h, 90000, &g);
        check("move", moved, "ConfigureNotify root=%d,%d (was %d,%d)", g.x, g.y, A.x, A.y); refresh(&A);
        pump(&X, 500); uint32_t pid = parent_of(&X, A.id); (void)pid;
        check("move_aurora", wait_aurora(A.id, "", 1000), "still managed"); }
    /* ---- resize by the bottom-right corner ---- */
    {   int cx = A.x + A.w + A.r - 2, cy = A.y + A.h + A.b - 2; step("resize", "x=%d y=%d dx=100 dy=60", cx, cy);
        struct geo g; bool sized = wait_geometry(&X, A.id, -99999, -99999, A.w + 100, A.h + 60, 90000, &g);
        check("resize", sized, "ConfigureNotify %dx%d (was %dx%d)", g.w, g.h, A.w, A.h); refresh(&A);
        X.queued = 0; uint32_t v[2] = {520, 380}; configure(&X, A.id, 4 | 8, v, 2);
        check("client_resize", wait_geometry(&X, A.id, -99999, -99999, 520, 380, 10000, &g), "the client's own resize: %dx%d", g.w, g.h);
        uint32_t small[2] = {50, 50}; configure(&X, A.id, 4 | 8, small, 2);
        check("min_size", wait_geometry(&X, A.id, -99999, -99999, 200, 150, 10000, &g), "a request below WM_NORMAL_HINTS min: %dx%d", g.w, g.h);
        uint32_t back[2] = {420, 300}; configure(&X, A.id, 4 | 8, back, 2); wait_geometry(&X, A.id, -99999, -99999, 420, 300, 10000, &g); refresh(&A);
        check("real_size", A.w == 420 && A.h == 300, "the server has the client at %dx%d at %d,%d", A.w, A.h, A.x, A.y); }
    /* ---- maximize / fullscreen, from the client and from the frame ---- */
    uint32_t FULL = atom(&X, "_NET_WM_STATE_FULLSCREEN"), MAXV = atom(&X, "_NET_WM_STATE_MAXIMIZED_VERT"), MAXH = atom(&X, "_NET_WM_STATE_MAXIMIZED_HORZ");
    {   struct geo g; X.queued = 0; state_message(A.id, 1, MAXV, MAXH);
        bool big = wait_geometry(&X, A.id, -99999, -99999, -1, -1, 10000, &g) && g.w > A.w + 200;
        pump(&X, 300); check("maximize", big && net_state_has(&X, A.id, MAXV) && net_state_has(&X, A.id, MAXH), "client=%dx%d at %d,%d, _NET_WM_STATE maximized", g.w, g.h, g.x, g.y);
        X.queued = 0; state_message(A.id, 0, MAXV, MAXH);
        check("unmaximize", wait_geometry(&X, A.id, A.x, A.y, A.w, A.h, 10000, &g) && (pump(&X, 300), !net_state_has(&X, A.id, MAXV)), "back to %dx%d at %d,%d", g.w, g.h, g.x, g.y);
        X.queued = 0; state_message(A.id, 1, FULL, 0);
        bool full = wait_geometry(&X, A.id, 0, 0, X.root_w, X.root_h, 10000, &g); pump(&X, 300);
        check("fullscreen", full && net_state_has(&X, A.id, FULL), "client=%dx%d at %d,%d", g.w, g.h, g.x, g.y);
        step("shot_fullscreen", "w=%u h=%u color=2060c0", X.root_w, X.root_h); check("fullscreen_key", wait_key(90000), "the host's key reached the fullscreen window");
        X.queued = 0; state_message(A.id, 2, FULL, 0);   /* toggle back */
        check("unfullscreen", wait_geometry(&X, A.id, A.x, A.y, A.w, A.h, 10000, &g) && (pump(&X, 300), !net_state_has(&X, A.id, FULL)), "back to %dx%d at %d,%d", g.w, g.h, g.x, g.y);
        int zx = A.x - A.l + 56, zy = A.y - A.t + 14; step("zoom", "x=%d y=%d", zx, zy);
        big = wait_geometry(&X, A.id, -99999, -99999, -1, -1, 90000, &g) && g.w > A.w + 200; pump(&X, 300);
        check("zoom_button", big && net_state_has(&X, A.id, MAXV), "the frame's green button maximized it: %dx%d at %d,%d", g.w, g.h, g.x, g.y);
        read_extents(&A); step("unzoom", "x=%d y=%d", g.x - A.l + 56, g.y - A.t + 14);
        check("unzoom_button", wait_geometry(&X, A.id, A.x, A.y, A.w, A.h, 90000, &g) && (pump(&X, 300), !net_state_has(&X, A.id, MAXV)), "and back: %dx%d at %d,%d", g.w, g.h, g.x, g.y); }
    /* ---- minimize (WM_CHANGE_STATE, the yellow button), restore (_NET_ACTIVE_WINDOW from a pager) ---- */
    {   uint32_t WMSTATE = atom(&X, "WM_STATE"), CHANGE = atom(&X, "WM_CHANGE_STATE"), ACTIVE = atom(&X, "_NET_ACTIVE_WINDOW"), HIDDEN = atom(&X, "_NET_WM_STATE_HIDDEN"); bool ok = false;
        client_message(&X, X.root, A.id, CHANGE, 3, 0, 0, 0);
        for (uint64_t end = ms() + 10000; ms() < end && !ok; pump(&X, 200)) ok = prop32(&X, A.id, WMSTATE) == 3;
        check("iconify_request", ok && net_state_has(&X, A.id, HIDDEN) && control_has("windows", "minimized=1"), "WM_CHANGE_STATE Iconic: WM_STATE=%u, _NET_WM_STATE_HIDDEN, Aurora minimized", prop32(&X, A.id, WMSTATE));
        X.queued = 0; client_message(&X, X.root, A.id, ACTIVE, 2, 0, 0, 0); ok = false;
        for (uint64_t end = ms() + 10000; ms() < end && !ok; pump(&X, 200)) ok = prop32(&X, A.id, WMSTATE) == 1;
        check("restore_request", ok && wait_focus_in(A.id, 5000), "_NET_ACTIVE_WINDOW (pager) restored and focused it");
        refresh(&A); read_extents(&A); step("minimize", "x=%d y=%d", A.x - A.l + 36, A.y - A.t + 14); ok = false;
        for (uint64_t end = ms() + 90000; ms() < end && !ok; pump(&X, 200)) ok = prop32(&X, A.id, WMSTATE) == 3;
        check("minimize_button", ok, "the yellow button: WM_STATE=%u", prop32(&X, A.id, WMSTATE));
        X.queued = 0; client_message(&X, X.root, A.id, ACTIVE, 2, 0, 0, 0); ok = false;
        for (uint64_t end = ms() + 10000; ms() < end && !ok; pump(&X, 200)) ok = prop32(&X, A.id, WMSTATE) == 1;
        check("restore_again", ok, "restored"); wait_focus_in(A.id, 5000); refresh(&A); }
    /* ---- a transient dialog stays above its parent ---- */
    struct win D = {.id = create_window(&X, 0, 0, 300, 140, 0xFF8000, false)};
    set_normal(&X, D.id, "XWM Dialog", "XWM Dialog", "xwmtest\0XwmTest", true);
    change_property(&X, D.id, 68, 33, 32, &A.id, 1); { uint32_t type = atom(&X, "_NET_WM_WINDOW_TYPE_DIALOG"); change_property(&X, D.id, atom(&X, "_NET_WM_WINDOW_TYPE"), 4, 32, &type, 1); }
    map_window(&X, D.id);
    {   bool ok = wait_mapped(&D, 20000) && wait_aurora(D.id, "surface=1", 20000); uint32_t gcD = gc_for(D.id); fill(&X, D.id, gcD, 0xFF8000, 0, 0, 4000, 4000); xsync(&X);
        int dcx = D.x + D.w / 2, dcy = D.y + D.h / 2, acx = A.x + A.w / 2, acy = A.y + A.h / 2;
        check("dialog", ok && wait_focus_in(D.id, 10000) && abs(dcx - acx) < 40 && abs(dcy - acy) < 60 && !aurora_line(D.id, "transient=0x0 "), "D=0x%x centre %d,%d over A's %d,%d", D.id, dcx, dcy, acx, acy);
        int px = A.x + 10, py = A.y + A.h - 10; step("transient", "x=%d y=%d dx=%d dy=%d", px, py, dcx, dcy);
        check("transient_click", wait_focus_in(A.id, 90000), "A focused by a click");
        pump(&X, 500);
        check("transient_above", stack_index(&X, parent_of(&X, D.id)) > stack_index(&X, parent_of(&X, A.id)), "D's frame stays above A's (X stacking)");
        char* t = control("windows"); char* d = t ? strstr(t, "title=XWM Dialog") : NULL; char* a = t ? strstr(t, "title=XWM Test \xc3\x84") : NULL;
        check("transient_aurora", d && a && d < a, "Aurora's list (topmost first) has the dialog above A"); free(t);
        step("shot_transient", "x=%d y=%d color=ff8000", dcx, dcy); check("transient_shot_key", wait_key(90000), "go on"); }
    /* ---- an override-redirect menu at its X position, unmanaged, above ---- */
    {   int mx = A.x + 30, my = A.y + 40; int listed = count_windows(); struct win M = {.id = create_window(&X, mx, my, 160, 110, 0xC000C0, true)};
        map_window(&X, M.id); uint8_t e[32]; bool shown = wait_event(&X, 19, M.id, 10000, e, NULL, NULL) && wait_aurora(M.id, "kind=override", 20000) && wait_aurora(M.id, "surface=1", 20000);
        uint32_t gcM = gc_for(M.id); fill(&X, M.id, gcM, 0xC000C0, 0, 0, 160, 110); xsync(&X);
        check("menu", shown && parent_of(&X, M.id) == X.root && count_windows() == listed, "M=0x%x mapped at %d,%d, not reparented, not in the window list (%d windows)", M.id, mx, my, listed);
        step("menu", "x=%d y=%d color=c000c0", mx + 80, my + 55);
        check("menu_click", wait_event(&X, 4, M.id, 90000, e, NULL, NULL), "ButtonPress on the menu at %d,%d", (int16_t)get16(e + 24), (int16_t)get16(e + 26));
        pump(&X, 300); check("menu_no_focus", focus_window(&X) == A.id, "the menu did not take the focus (focus=0x%x)", focus_window(&X));
        unmap_window(&X, M.id); destroy_window(&X, M.id); xsync(&X); }
    /* ---- clipboard X -> Wayland -> X ---- */
    {   struct geo g; uint32_t right[2] = {(uint32_t)(X.root_w - A.w - 80), 330}; configure(&X, A.id, 1 | 2, right, 2);   /* away from where new windows cascade */
        wait_geometry(&X, A.id, -99999, -99999, -1, -1, 10000, &g); refresh(&A);
        own_clipboard(&X, A.id, "x11 to wayland \xe2\x9c\x93"); xsync(&X); pump(&X, 800);
        int to_wl = -1; char self[] = "/bin/xwmtest", mode[] = "wl"; char* argv[] = {self, mode, NULL}; pid_t wl = spawn(argv, &to_wl);
        say("WAYLAND SIDE pid=%d", (int)wl); step("clipboard_wayland", "wait=WL selection");
        pump(&X, 6000);
        if (to_wl >= 0) { const char* line = "copy wayland to x11 \xe2\x9c\x94\n"; if (write(to_wl, line, strlen(line)) < 0) say("pipe to the Wayland side failed"); }
        pump(&X, 2000);
        step("clipboard_x", "x=%d y=%d", A.x + A.w / 2, A.y + A.h / 2);
        check("clipboard_focus", wait_focus_in(A.id, 90000), "A focused again");
        pump(&X, 500); owned_text = NULL;
        char* text = paste(&X, A.id, A_UTF8);
        check("paste_from_wayland", text && !strcmp(text, "wayland to x11 \xe2\x9c\x94"), "UTF8_STRING=%s", text ? text : "(none)"); free(text);
        uint32_t prop = atom(&X, "_XWMTEST"); (void)prop;
        uint8_t r[24] = {24, 0, 6, 0}; put32(r + 4, A.id); put32(r + 8, A_CLIPBOARD); put32(r + 12, A_TARGETS); put32(r + 16, atom(&X, "_XWMTEST_T")); put32(r + 20, 0); request(&X, r, 24);
        uint8_t e[32]; uint32_t n = 0; bool targets = false;
        if (wait_event(&X, 31, 0, 10000, e, NULL, NULL) && get32(e + 20)) { uint8_t* v = get_property(&X, A.id, atom(&X, "_XWMTEST_T"), true, NULL, NULL, &n); for (uint32_t i = 0; v && i < n; i++) if (get32(v + 4 * i) == A_UTF8) targets = true; free(v); }
        check("targets", targets, "TARGETS offers UTF8_STRING (%u targets)", n);
        if (to_wl >= 0) close(to_wl); if (wl > 0) { kill(wl, SIGTERM); waitpid(wl, NULL, 0); }
        /* INCR both ways: 400 kB from X to a new Wayland window (it reads on focus), 600 kB back */
        char* big = big_text(400000); own_clipboard(&X, A.id, big); xsync(&X); pump(&X, 1500);
        to_wl = -1; wl = spawn(argv, &to_wl); say("WAYLAND SIDE pid=%d (large copies)", (int)wl); step("clipboard_big", "bytes=400000 sum=%u", text_sum(big, 400000));
        for (uint64_t end = ms() + 30000; ms() < end && !(incr_out.size == 400000 && !incr_out.active); ) pump(&X, 300);
        check("incr_to_wayland", incr_out.size == 400000 && incr_out.sent == 400000 && !incr_out.active, "400000 bytes sent to Aurora in INCR pieces (%zu sent)", incr_out.sent);
        pump(&X, 2000);
        if (to_wl >= 0) { const char* line = "copybig 600000\n"; if (write(to_wl, line, strlen(line)) < 0) say("pipe to the Wayland side failed"); }
        pump(&X, 2000); owned_text = NULL;
        step("clipboard_x2", "x=%d y=%d", A.x + A.w / 2, A.y + A.h / 2);
        check("clipboard_focus2", wait_focus_in(A.id, 90000), "A focused again"); pump(&X, 500);
        char* got = paste(&X, A.id, A_UTF8); char* want_text = big_text(600000);
        check("incr_from_wayland", got && strlen(got) == 600000 && !strcmp(got, want_text), "pasted %zu bytes through INCR (sum %u)", got ? strlen(got) : 0, got ? text_sum(got, strlen(got)) : 0);
        free(got); free(want_text); free(big);
        if (to_wl >= 0) close(to_wl); if (wl > 0) { kill(wl, SIGTERM); waitpid(wl, NULL, 0); } }
    /* ---- close: WM_DELETE_WINDOW (frame button), XKillClient (Alt+F4 on a window without it) ---- */
    {   read_extents(&D); struct geo g; if (last_configure(&X, D.id, &g, 300)) { D.x = g.x; D.y = g.y; }
        step("close", "x=%d y=%d", D.x - D.l + 16, D.y - D.t + 14);
        uint8_t e[32]; bool got = false;
        for (uint64_t end = ms() + 90000; ms() < end && !got;) { if (!wait_event(&X, 33, D.id, (int)(end - ms()), e, NULL, NULL)) break; got = get32(e + 8) == atom(&X, "WM_PROTOCOLS") && get32(e + 12) == atom(&X, "WM_DELETE_WINDOW"); }
        check("close_delete", got, "WM_DELETE_WINDOW for the dialog");
        destroy_window(&X, D.id); xsync(&X);
        struct xc K; bool k = xconnect(&K); uint32_t kw = k ? create_window(&K, 0, 0, 240, 160, 0x804020, false) : 0;
        if (k) { set_text(&K, kw, 39, 31, "XWM Kill"); change_property(&K, kw, 67, 31, 8, "xwmkill\0XwmKill", 16); uint8_t m[8] = {8, 0, 2, 0}; put32(m + 4, kw); request(&K, m, 8); xsync(&K); }
        check("kill_mapped", k && wait_aurora(kw, "surface=1", 20000), "K=0x%x (no WM_PROTOCOLS)", kw);
        step("kill", "x=%d y=%d", 0, 0);
        bool dead = false; for (uint64_t end = ms() + 90000; ms() < end && !dead;) { uint8_t m[32]; uint8_t* x; if (!xread(&K, m, &x, 1000)) dead = K.dead; free(x); pump(&X, 1); }
        check("close_kill", dead, "Aurora ended the client (XKillClient)"); if (K.fd >= 0) close(K.fd); }
    hostile(&A);
    crash_client();
    if (apps && apps[0]) real_apps(apps);
    {   struct geo g; X.queued = 0; uint32_t v[2] = {430, 310}; configure(&X, A.id, 4 | 8, v, 2);
        check("alive", wait_geometry(&X, A.id, -99999, -99999, 430, 310, 10000, &g) && control_has("ping", "OK"), "after everything the window manager still answers"); }
    xwayland_restart();
    say("DONE passed=%d failed=%d", passed, failed);
    pump(&X, 2000); return failed ? 1 : 0;
}
static void hostile(struct win* A) {
    struct xc H; if (!check("hostile_connect", xconnect(&H), "a second client")) return;
    uint32_t SERIAL = atom(&H, "WL_SURFACE_SERIAL"), ID = atom(&H, "WL_SURFACE_ID"), STATE = atom(&H, "_NET_WM_STATE"), ACTIVE = atom(&H, "_NET_ACTIVE_WINDOW");
    uint32_t frame = parent_of(&X, A->id);
    for (int i = 0; i < 20; i++) {   /* forged surface associations, bogus EWMH requests */
        client_message(&H, H.root, frame, SERIAL, (uint32_t)(0x1000 + i), 0, 0, 0); client_message(&H, H.root, A->id, ID, (uint32_t)i, 0, 0, 0);
        client_message(&H, H.root, A->id, STATE, (uint32_t)(i % 9), 0xFFFFFFF0u - (uint32_t)i, (uint32_t)i, 1);
        client_message(&H, H.root, 0x1234567u + (uint32_t)i, ACTIVE, 1, 0, 0, 0);
        client_message(&H, H.root, A->id, atom(&H, "_NET_WM_MOVERESIZE"), 10, 10, (uint32_t)(i * 7), 1);
    }
    uint32_t w1 = create_window(&H, -30000, -30000, 1, 1, 0x101010, false), w2 = create_window(&H, 40, 40, 200, 100, 0x404040, false);
    static uint8_t junk[65536]; for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i % 7 == 0 ? 0xFF : i % 5 == 0 ? 0x01 : 'A' + i % 26);
    change_property(&H, w1, 39, 31, 8, junk, sizeof(junk));                                     /* 64 KiB WM_NAME with control bytes */
    change_property(&H, w1, atom(&H, "_NET_WM_NAME"), atom(&H, "UTF8_STRING"), 8, "\xff\xfe\xc0\x80 bad \x1b[31m utf8 \xe2\x82", 25);
    uint32_t short_hints[3] = {0xFFFFFFFF, 1, 2}; change_property(&H, w1, 40, 41, 32, short_hints, 3);   /* WM_NORMAL_HINTS too short */
    uint32_t bad_hints[18] = {16 | 32, 0, 0, 0, 0, 5000, 5000, 10, 10}; change_property(&H, w2, 40, 41, 32, bad_hints, 18);   /* min > max */
    change_property(&H, w1, 67, 31, 8, junk, 1000);                                              /* WM_CLASS without NUL */
    change_property(&H, w1, 68, 33, 32, &w2, 1); change_property(&H, w2, 68, 33, 32, &w1, 1);   /* a transient cycle */
    uint32_t self = w2; (void)self;
    uint32_t types[1000]; for (int i = 0; i < 1000; i++) types[i] = (uint32_t)(i * 2654435761u); change_property(&H, w1, atom(&H, "_NET_WM_WINDOW_TYPE"), 4, 32, types, 1000);
    uint32_t motif[1] = {2}; change_property(&H, w2, atom(&H, "_MOTIF_WM_HINTS"), atom(&H, "_MOTIF_WM_HINTS"), 32, motif, 1);
    uint32_t protocols[3] = {0, 0xFFFFFFFF, 12345}; change_property(&H, w1, atom(&H, "WM_PROTOCOLS"), 4, 32, protocols, 3);
    uint8_t m1[8] = {8, 0, 2, 0}; put32(m1 + 4, w1); request(&H, m1, 8); put32(m1 + 4, w2); request(&H, m1, 8);
    uint32_t zero[2] = {0, 0}; configure(&H, w2, 4 | 8, zero, 2); uint32_t huge[2] = {65535, 65535}; configure(&H, w2, 4 | 8, huge, 2);
    uint32_t far[2] = {(uint32_t)-30000, (uint32_t)-30000}; configure(&H, w1, 1 | 2, far, 2);
    xsync(&H); pump(&X, 3000);
    char* t = control("x11"); bool sane = t != NULL; size_t longest = 0;
    for (char* save = NULL, *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
        char* title = strstr(l, "title="); if (!title) continue; size_t n = strlen(title + 6); if (n > longest) longest = n;
        for (char* p = title + 6; *p; p++) if ((uint8_t)*p < 0x20) sane = false; }
    free(t);
    check("hostile_titles", sane && longest <= 127, "titles cleaned (longest %zu bytes, no control characters)", longest);
    check("hostile_surface", aurora_line(A->id, "surface=1") && aurora_line(A->id, "app=xwmtest"), "forged WL_SURFACE_SERIAL/ID did not take A's surface");
    check("hostile_focus", focus_window(&X) == A->id || prop32(&X, X.root, atom(&X, "_NET_ACTIVE_WINDOW")) != 0x1234567u, "no activation of a window that does not exist");
    close(H.fd); pump(&X, 1500);
    check("hostile_gone", !aurora_line(w2, "managed") && control_has("ping", "OK"), "the hostile client's windows went with it; Aurora answers");
}
static void crash_client(void) {   /* a client that dies holding a mapped window, the focus and the clipboard */
    pid_t pid = fork();
    if (!pid) {
        struct xc C; if (!xconnect(&C)) _exit(2);
        X = C; A_UTF8 = atom(&X, "UTF8_STRING"); A_TARGETS = atom(&X, "TARGETS"); A_CLIPBOARD = atom(&X, "CLIPBOARD");
        uint32_t w = create_window(&X, 0, 0, 300, 200, 0x606060, false); set_text(&X, w, 39, 31, "XWM Crash"); change_property(&X, w, 67, 31, 8, "xwmcrash\0XwmCrash", 18);
        map_window(&X, w); uint8_t e[32]; wait_event(&X, 9, w, 20000, e, NULL, NULL);
        own_clipboard(&X, w, "crash copy"); xsync(&X); say("CRASH CLIENT window=0x%x about to abort", w); sleep_ms(1500);
        abort();
    }
    int status = 0; waitpid(pid, &status, 0);
    check("crash_died", WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "the client died with signal %d", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    pump(&X, 3000);
    check("crash_survived", control_has("ping", "OK") && !control_has("x11", "app=xwmcrash"), "Aurora runs on; the dead client's window is gone");
}
static void real_apps(const char* apps) {
    if (strstr(apps, "xterm") && !access("/usr/bin/xterm", X_OK)) {
        unlink("/tmp/xwm-xterm.txt");
        char p[] = "/usr/bin/xterm", g[] = "-geometry", geo[] = "80x20+300+200", t[] = "-T", title[] = "xterm under Aurora"; char* argv[] = {p, g, geo, t, title, NULL};
        pid_t pid = spawn(argv, NULL); bool shown = false; char* text = NULL;
        for (uint64_t end = ms() + 60000; ms() < end && !shown; pump(&X, 300)) { text = control("x11"); shown = text && strstr(text, "app=xterm") && strstr(text, "surface=1"); if (!shown) { free(text); text = NULL; } }
        check("xterm_window", shown && strstr(text, "title=xterm under Aurora"), "xterm pid=%d managed with its title", (int)pid); free(text);
        control_show("x11", "app=xterm");
        step("xterm", "type=echo xwm-xterm-ok > /tmp/xwm-xterm.txt");
        check("xterm_keys", wait_line_file("/tmp/xwm-xterm.txt", "xwm-xterm-ok", 90000), "the typed command ran in xterm (keyboard focus)");
        step("xterm_close", "keys=alt+f4");
        int status = -1; for (uint64_t end = ms() + 60000; ms() < end; pump(&X, 300)) if (waitpid(pid, &status, WNOHANG) == pid) break;
        bool gone = false; for (uint64_t end = ms() + 10000; ms() < end && !gone; pump(&X, 300)) gone = !control_has("x11", "app=xterm");   /* the server notices the exit */
        check("xterm_close", status >= 0 && gone, "Alt+F4 closed xterm (WM_DELETE_WINDOW), status=0x%x", status);
    }
    if (strstr(apps, "wine") && !access("/usr/bin/wine", X_OK)) {   /* Wine with DISPLAY set: winex11 (Wine 10 tries x11 before wayland) */
        char p[] = "/usr/bin/wine", n[] = "notepad"; char* argv[] = {p, n, NULL}; setenv("WINEDEBUG", "-all", 1);
        pid_t pid = spawn(argv, NULL); bool shown = false; char* text = NULL;
        for (uint64_t end = ms() + 240000; ms() < end && !shown; pump(&X, 500)) { text = control("x11"); shown = text && strstr(text, "app=notepad.exe") && strstr(text, "surface=1") && strstr(text, "Notepad"); if (!shown) { free(text); text = NULL; } }
        check("winex11_window", shown, "wine notepad pid=%d is an X window Aurora manages (winex11)", (int)pid); free(text);
        control_show("x11", "app=notepad.exe");
        step("wine_close", "keys=alt+f4");
        bool gone = false; for (uint64_t end = ms() + 60000; ms() < end && !gone; pump(&X, 500)) gone = !control_has("x11", "app=notepad.exe");
        check("winex11_close", gone, "Alt+F4 closed it (WM_DELETE_WINDOW -> WM_CLOSE)");
        int status; waitpid(pid, &status, WNOHANG);
    }
    if (strstr(apps, "firefox") && !access("/opt/firefox/firefox-bin", X_OK)) {   /* a GTK 3 program forced to X11 */
        pid_t pid = fork();
        if (!pid) { setenv("MOZ_ENABLE_WAYLAND", "0", 1); setenv("GDK_BACKEND", "x11", 1); setenv("MOZ_CRASHREPORTER_DISABLE", "1", 1);
            execl("/opt/firefox/firefox-bin", "firefox-bin", "--profile", "/tmp/xwm-firefox", "--no-remote", "about:blank", (char*)NULL); _exit(127); }
        mkdir("/tmp/xwm-firefox", 0700); bool shown = false; char* text = NULL;
        for (uint64_t end = ms() + 240000; ms() < end && !shown; pump(&X, 500)) { text = control("x11"); char* l = text ? strstr(text, "app=firefox") : NULL;
            shown = l && strstr(l, "surface=1") && strstr(l, "Firefox"); if (!shown) { free(text); text = NULL; } }
        int fx = 0, fy = 0; bool framed = false;
        if (text) { char* l = strstr(text, "app=firefox"); char* line_start = l; while (line_start > text && line_start[-1] != '\n') line_start--;
            char* x = strstr(l, " x="); char* y = strstr(l, " y="); if (x) fx = atoi(x + 3); if (y) fy = atoi(y + 3); framed = !strstr(line_start, "frame=0x0 "); }
        check("gtk_x11_window", shown, "Firefox (GTK 3, GDK_BACKEND=x11) pid=%d is an X window Aurora manages at %d,%d (%s)", (int)pid, fx, fy,
              framed ? "Aurora frame" : "its own title bar: _MOTIF_WM_HINTS without decorations"); free(text);
        control_show("x11", "app=firefox");
        step("firefox", "x=%d y=%d framed=%d", framed ? fx + 16 : fx + 300, framed ? fy + 14 : fy + 200, framed);
        int status = -1; for (uint64_t end = ms() + 90000; ms() < end; pump(&X, 500)) if (waitpid(pid, &status, WNOHANG) == pid) break;
        check("gtk_x11_close", status >= 0, "%s closed Firefox (status=0x%x)", framed ? "the red light" : "Alt+F4 (WM_DELETE_WINDOW)", status);
        if (status < 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    }
    if (strstr(apps, "xeyes") && !access("/usr/bin/xeyes", X_OK)) {
        char p[] = "/usr/bin/xeyes", g[] = "-geometry", geo[] = "240x180+500+260"; char* argv[] = {p, g, geo, NULL};
        pid_t pid = spawn(argv, NULL); bool shown = false; char* text = NULL;
        for (uint64_t end = ms() + 60000; ms() < end && !shown; pump(&X, 300)) { text = control("x11"); shown = text && strstr(text, "app=xeyes") && strstr(text, "surface=1"); if (!shown) { free(text); text = NULL; } }
        int fx = 0, fy = 0; if (text) { char* l = strstr(text, "app=xeyes"); char* x = l ? strstr(l, " x=") : NULL; char* y = l ? strstr(l, " y=") : NULL; if (x) fx = atoi(x + 3); if (y) fy = atoi(y + 3); }
        check("xeyes_window", shown, "xeyes pid=%d managed at frame %d,%d", (int)pid, fx, fy); free(text);
        step("xeyes", "x=%d y=%d shot_x=%d shot_y=%d", fx + 16, fy + 14, fx + 120, fy + 28 + 90);
        int status = -1; for (uint64_t end = ms() + 90000; ms() < end; pump(&X, 300)) if (waitpid(pid, &status, WNOHANG) == pid) break;
        check("xeyes_close", status >= 0, "the close button ended xeyes (status=0x%x)", status);
    }
}
int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2 && !strcmp(argv[1], "wl")) return wayland_side();
    if (argc >= 2 && !strcmp(argv[1], "run")) return run(argc > 2 ? argv[2] : "");
    fprintf(stderr, "usage: xwmtest run [xterm,xeyes] | wl\n"); return 2;
}
