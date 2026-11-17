/* aurora_xwm.c: rootless Xwayland, with Aurora as the X11 window manager (see aurora_xwm.h).
 *
 * Layers, top to bottom:
 *   1. the X server process: Aurora owns the display socket and lock file, starts Xwayland on the first connection
 *      (-listenfd), restarts it after an exit (not more than once every 3 s), MIT cookie access control;
 *   2. xwayland_shell_v1 (only Aurora's Xwayland sees it) and WL_SURFACE_ID: which wl_surface shows which X window;
 *   3. an X protocol client on the -wm socket (little endian, no libxcb): output buffer + reply cookies;
 *   4. window management: managed windows (MapRequest -> properties -> frame -> Aurora window), override-redirect
 *      windows, focus, stacking, ICCCM/EWMH properties and client messages, frames drawn with macui;
 *   5. the CLIPBOARD bridge (X <-> wl_data_device selection, TARGETS, INCR both ways).
 * Every X request goes out through the buffer; replies call back with the window id (never a pointer), so a window
 * that disappears meanwhile is simply not found. Hostile values (sizes, lengths, ids, forged ClientMessages, property
 * contents) are bounded and checked; oddities are logged a limited number of times. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "aurora_xwm.h"
#include "aurora_control.h"
#include "aurora_graphics.h"
#include "xwayland-shell-v1-server-protocol.h"
#include "../../kernel/macui.h"

#define XWAYLAND "/usr/bin/Xwayland"
#define TITLE_H 28            /* frame: title bar */
#define BORDER 4              /* frame: left, right and bottom border (resize handles) */
#define MAX_WINDOWS 4096      /* X windows tracked (a client creating more gets them unmanaged) */
#define MAX_TRANSFER (64u << 20)
#define CHUNK (256u << 10)

static void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* format, ...) {
    va_list a; va_start(a, format); fputs("[64] AURORA X11 ", stdout); vprintf(format, a); va_end(a); fputc('\n', stdout); fflush(stdout);
}
static unsigned odd_count;   /* hostile or broken input: logged the first 64 times */
#define ODD(...) do { if (odd_count < 64) { odd_count++; say(__VA_ARGS__); } } while (0)
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void destroy_request(struct wl_client* c, struct wl_resource* r) { (void)c; wl_resource_destroy(r); }

static struct wl_display* display; static struct wl_event_loop* loop;
static struct wl_client* server;             /* Aurora's Xwayland, as a Wayland client */
static pid_t server_pid;
static int display_number = -1;
static char auth_path[128];
static bool enabled = true, export_display, use_shell = true;

/* ================================================================================================================
 * 3. The X connection
 * ================================================================================================================ */
typedef void (*reply_fn)(const uint8_t* reply, size_t size, uint32_t a, uint32_t b);
struct cookie { uint32_t seq; reply_fn done; uint32_t a, b; };
static int xfd = -1; static struct wl_event_source* xsource; static bool xflush_queued, xbroken, xteardown_queued;
static uint8_t* obuf; static size_t ostart, oend, ocap;
static uint8_t* ibuf; static size_t ilen, icap;
static enum { XS_DOWN, XS_SETUP, XS_INIT, XS_READY } xstate;
static uint32_t xseq; static size_t xmax = 16384;
static struct cookie* cookies; static size_t c_head, c_count, c_cap;
static uint32_t id_base, id_mask, id_next;
static uint32_t root, root_visual, wm_window, gc; static uint8_t root_depth, image_bpp; static bool images_ok, lsb_server;
static uint16_t root_w, root_h;
static uint8_t composite_major, xfixes_major, xfixes_event; static int init_pending;
static uint32_t last_time;   /* the latest X server timestamp seen */
static uint8_t secret[16];

static void teardown(const char* why);
static void teardown_idle(void* data) { (void)data; xteardown_queued = false; teardown("connection lost"); }
static void x_break(const char* why) {
    if (xbroken) return;
    xbroken = true; say("window manager connection broken: %s", why);
    if (!xteardown_queued && loop) xteardown_queued = wl_event_loop_add_idle(loop, teardown_idle, NULL) != NULL;
}
static void x_flush(void) {
    while (xfd >= 0 && ostart < oend) {
        ssize_t n = send(xfd, obuf + ostart, oend - ostart, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { ostart += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        x_break(n < 0 ? strerror(errno) : "closed"); return;
    }
    if (ostart == oend) ostart = oend = 0;
    if (xsource) wl_event_source_fd_update(xsource, WL_EVENT_READABLE | (oend > ostart ? WL_EVENT_WRITABLE : 0));
}
static void flush_idle(void* data) { (void)data; xflush_queued = false; x_flush(); }
static uint8_t* x_reserve(size_t size) {   /* room for one request at the end of the buffer (NULL: not connected) */
    if (xfd < 0 || xbroken || (size & 3) || size < 4 || size > xmax) { if (xfd >= 0 && !xbroken && size > xmax) ODD("request of %zu bytes dropped", size); return NULL; }
    if (oend + size > ocap) {
        if (ostart) { memmove(obuf, obuf + ostart, oend - ostart); oend -= ostart; ostart = 0; }
        if (oend + size > ocap) {
            size_t want = ocap ? ocap : 65536; while (want < oend + size) want *= 2;
            if (want > ((size_t)64 << 20)) { x_break("the X server stopped reading"); return NULL; }
            uint8_t* grown = realloc(obuf, want); if (!grown) { x_break("out of memory"); return NULL; }
            obuf = grown; ocap = want;
        }
    }
    memset(obuf + oend, 0, size); return obuf + oend;
}
static uint32_t x_commit(size_t size) {
    put16(obuf + oend + 2, (uint16_t)(size / 4)); oend += size;
    if (!xflush_queued && loop) xflush_queued = wl_event_loop_add_idle(loop, flush_idle, NULL) != NULL;
    return ++xseq;
}
static void x_expect(uint32_t seq, reply_fn done, uint32_t a, uint32_t b) {
    if (c_count == c_cap) {
        size_t cap = c_cap ? c_cap * 2 : 256;
        if (cap > (1u << 16)) { x_break("too many requests without reply"); done(NULL, 0, a, b); return; }
        struct cookie* next = malloc(cap * sizeof(*next)); if (!next) { x_break("out of memory"); done(NULL, 0, a, b); return; }
        for (size_t i = 0; i < c_count; i++) next[i] = cookies[(c_head + i) % c_cap];
        free(cookies); cookies = next; c_cap = cap; c_head = 0;
    }
    cookies[(c_head + c_count) % c_cap] = (struct cookie){seq, done, a, b}; c_count++;
}
/* A request with a reply: on failure (not connected) the callback runs at once with NULL. */
static void x_call(size_t size, reply_fn done, uint32_t a, uint32_t b) { x_expect(x_commit(size), done, a, b); }
static void x_answer(uint16_t seq16, const uint8_t* reply, size_t size) {
    while (c_count) {
        struct cookie c = cookies[c_head]; int16_t d = (int16_t)(uint16_t)((uint16_t)c.seq - seq16);
        if (d > 0) return;   /* an error of a request nobody waits for */
        c_head = (c_head + 1) % c_cap; c_count--;
        c.done(d == 0 ? reply : NULL, d == 0 ? size : 0, c.a, c.b);
        if (d == 0) return;
    }
}
static uint32_t x_id(void) {
    if (!id_mask || id_next >= id_mask) { ODD("out of X resource ids"); return 0; }
    id_next++; uint32_t step = id_mask & -id_mask; return id_base | ((id_next * step) & id_mask);
}

/* ---- requests ---- */
static void x_simple(uint8_t op, uint8_t data, uint32_t word) { uint8_t* p = x_reserve(8); if (!p) return; p[0] = op; p[1] = data; put32(p + 4, word); x_commit(8); }
static void x_map(uint32_t w) { x_simple(8, 0, w); }
static void x_destroy(uint32_t w) { x_simple(4, 0, w); }
static void x_save_set(uint32_t w, bool insert) { x_simple(6, insert ? 0 : 1, w); }
static void x_kill_client(uint32_t w) { x_simple(113, 0, w); }
static void x_change_attributes(uint32_t w, uint32_t mask, uint32_t value) {   /* one attribute */
    uint8_t* p = x_reserve(16); if (!p) return; p[0] = 2; put32(p + 4, w); put32(p + 8, mask); put32(p + 12, value); x_commit(16);
}
/* ConfigureWindow; values in mask order: x, y, width, height, border, sibling, stack mode. */
static void x_configure(uint32_t w, uint16_t mask, const int32_t* values) {
    int n = __builtin_popcount(mask & 0x7F); uint8_t* p = x_reserve(12 + 4 * (size_t)n); if (!p) return;
    p[0] = 12; put32(p + 4, w); put16(p + 8, mask & 0x7F);
    for (int i = 0; i < n; i++) put32(p + 12 + 4 * i, (uint32_t)values[i]);
    x_commit(12 + 4 * (size_t)n);
}
static void x_change_property(uint32_t w, uint32_t property, uint32_t type, uint8_t format, const void* data, uint32_t count) {
    size_t bytes = (size_t)count * (format / 8), size = 24 + ((bytes + 3) & ~(size_t)3);
    uint8_t* p = x_reserve(size); if (!p) return;
    p[0] = 18; p[1] = 0; put32(p + 4, w); put32(p + 8, property); put32(p + 12, type); p[16] = format; put32(p + 20, count);
    if (bytes) memcpy(p + 24, data, bytes);
    x_commit(size);
}
static void x_delete_property(uint32_t w, uint32_t property) { uint8_t* p = x_reserve(12); if (!p) return; p[0] = 19; put32(p + 4, w); put32(p + 8, property); x_commit(12); }
static void x_get_property(uint32_t w, uint32_t property, bool del, uint32_t type, uint32_t offset, uint32_t length, reply_fn done, uint32_t a, uint32_t b) {
    uint8_t* p = x_reserve(24); if (!p) { done(NULL, 0, a, b); return; }
    p[0] = 20; p[1] = del; put32(p + 4, w); put32(p + 8, property); put32(p + 12, type); put32(p + 16, offset); put32(p + 20, length);
    x_call(24, done, a, b);
}
static void x_send_event(uint32_t destination, uint32_t mask, const uint8_t event[32]) {
    uint8_t* p = x_reserve(44); if (!p) return; p[0] = 25; put32(p + 4, destination); put32(p + 8, mask); memcpy(p + 12, event, 32); x_commit(44);
}
static void x_client_message(uint32_t w, uint32_t type, uint32_t d0, uint32_t d1, uint32_t d2) {
    uint8_t e[32] = {33, 32}; put32(e + 4, w); put32(e + 8, type); put32(e + 12, d0); put32(e + 16, d1); put32(e + 20, d2);
    x_send_event(w, 0, e);   /* event mask 0: to the client that created the window (ICCCM 4.2.8) */
}
static void x_set_focus(uint32_t w, uint8_t revert) { uint8_t* p = x_reserve(12); if (!p) return; p[0] = 42; p[1] = revert; put32(p + 4, w); put32(p + 8, 0); x_commit(12); }
static void x_set_selection_owner(uint32_t owner, uint32_t selection, uint32_t time) {
    uint8_t* p = x_reserve(16); if (!p) return; p[0] = 22; put32(p + 4, owner); put32(p + 8, selection); put32(p + 12, time); x_commit(16);
}
static void x_convert_selection(uint32_t requestor, uint32_t selection, uint32_t target, uint32_t property, uint32_t time) {
    uint8_t* p = x_reserve(24); if (!p) return; p[0] = 24; put32(p + 4, requestor); put32(p + 8, selection); put32(p + 12, target); put32(p + 16, property); put32(p + 20, time); x_commit(24);
}
static void x_fill(uint32_t drawable, uint32_t color, const int32_t (*rects)[4], int n) {
    uint8_t* p = x_reserve(16); if (!p) return; p[0] = 56; put32(p + 4, gc); put32(p + 8, 1u << 2); put32(p + 12, color); x_commit(16);   /* ChangeGC foreground */
    p = x_reserve(12 + 8 * (size_t)n); if (!p) return; p[0] = 70; put32(p + 4, drawable); put32(p + 8, gc);
    for (int i = 0; i < n; i++) { put16(p + 12 + 8 * i, (uint16_t)rects[i][0]); put16(p + 14 + 8 * i, (uint16_t)rects[i][1]); put16(p + 16 + 8 * i, (uint16_t)rects[i][2]); put16(p + 18 + 8 * i, (uint16_t)rects[i][3]); }
    x_commit(12 + 8 * (size_t)n);
}
static void x_put_image(uint32_t drawable, const uint32_t* pixels, int w, int h) {   /* ZPixmap, root depth, 32 bpp, in strips */
    size_t row = (size_t)w * 4; int rows = row ? (int)((xmax - 24) / row) : 0; if (rows <= 0) return;
    for (int y = 0; y < h; y += rows) {
        int n = h - y < rows ? h - y : rows; size_t bytes = row * (size_t)n; uint8_t* p = x_reserve(24 + bytes); if (!p) return;
        p[0] = 72; p[1] = 2; put32(p + 4, drawable); put32(p + 8, gc); put16(p + 12, (uint16_t)w); put16(p + 14, (uint16_t)n);
        put16(p + 16, 0); put16(p + 18, (uint16_t)y); p[20] = 0; p[21] = root_depth; memcpy(p + 24, pixels + (size_t)y * w, bytes);
        x_commit(24 + bytes);
    }
}

/* ---- atoms ---- */
#define ATOM_LIST(X) X(WM_PROTOCOLS) X(WM_DELETE_WINDOW) X(WM_TAKE_FOCUS) X(WM_STATE) X(WM_CHANGE_STATE) X(WM_S0) \
    X(_NET_WM_CM_S0) X(_NET_SUPPORTED) X(_NET_SUPPORTING_WM_CHECK) X(_NET_ACTIVE_WINDOW) X(_NET_CLIENT_LIST) \
    X(_NET_CLIENT_LIST_STACKING) X(_NET_WM_NAME) X(_NET_WM_PID) X(_NET_WM_STATE) X(_NET_WM_STATE_FULLSCREEN) \
    X(_NET_WM_STATE_MAXIMIZED_VERT) X(_NET_WM_STATE_MAXIMIZED_HORZ) X(_NET_WM_STATE_HIDDEN) X(_NET_WM_STATE_FOCUSED) \
    X(_NET_WM_STATE_MODAL) X(_NET_WM_WINDOW_TYPE) X(_NET_WM_WINDOW_TYPE_NORMAL) X(_NET_WM_WINDOW_TYPE_DIALOG) \
    X(_NET_WM_WINDOW_TYPE_UTILITY) X(_NET_WM_WINDOW_TYPE_TOOLBAR) X(_NET_WM_WINDOW_TYPE_SPLASH) X(_NET_WM_WINDOW_TYPE_MENU) \
    X(_NET_WM_WINDOW_TYPE_DROPDOWN_MENU) X(_NET_WM_WINDOW_TYPE_POPUP_MENU) X(_NET_WM_WINDOW_TYPE_TOOLTIP) \
    X(_NET_WM_WINDOW_TYPE_NOTIFICATION) X(_NET_WM_WINDOW_TYPE_COMBO) X(_NET_WM_WINDOW_TYPE_DND) X(_NET_WM_WINDOW_TYPE_DOCK) \
    X(_NET_WM_WINDOW_TYPE_DESKTOP) X(_NET_WM_MOVERESIZE) X(_NET_CLOSE_WINDOW) X(_NET_FRAME_EXTENTS) \
    X(_NET_REQUEST_FRAME_EXTENTS) X(_NET_WM_ALLOWED_ACTIONS) X(_NET_WM_ACTION_MOVE) X(_NET_WM_ACTION_RESIZE) \
    X(_NET_WM_ACTION_MINIMIZE) X(_NET_WM_ACTION_MAXIMIZE_HORZ) X(_NET_WM_ACTION_MAXIMIZE_VERT) X(_NET_WM_ACTION_FULLSCREEN) \
    X(_NET_WM_ACTION_CLOSE) X(_NET_WM_ICON) X(_MOTIF_WM_HINTS) X(_GTK_FRAME_EXTENTS) X(UTF8_STRING) X(TEXT) X(COMPOUND_TEXT) X(CLIPBOARD) \
    X(TARGETS) X(TIMESTAMP) X(INCR) X(MULTIPLE) X(_AURORA_TARGETS) X(_AURORA_DATA) X(WL_SURFACE_ID) X(WL_SURFACE_SERIAL)
enum {
#define X(n) A_##n,
    ATOM_LIST(X)
#undef X
    A_COUNT };
static const char* const atom_names[] = {
#define X(n) #n,
    ATOM_LIST(X)
#undef X
};
static uint32_t atoms[A_COUNT];
#define ATOM(n) atoms[A_##n]
enum { XA_PRIMARY = 1, XA_ATOM = 4, XA_CARDINAL = 6, XA_INTEGER = 19, XA_STRING = 31, XA_WINDOW = 33, XA_WM_HINTS = 35,
       XA_WM_NAME = 39, XA_WM_NORMAL_HINTS = 40, XA_WM_CLASS = 67, XA_WM_TRANSIENT_FOR = 68 };
/* event masks */
enum { M_BUTTON_PRESS = 1u << 2, M_EXPOSURE = 1u << 15, M_STRUCTURE = 1u << 17, M_SUBSTRUCTURE = 1u << 19, M_REDIRECT = 1u << 20,
       M_FOCUS = 1u << 21, M_PROPERTY = 1u << 22 };
enum { C_X = 1, C_Y = 2, C_W = 4, C_H = 8, C_BORDER = 16, C_SIBLING = 32, C_STACK = 64 };

/* ================================================================================================================
 * 4. Windows
 * ================================================================================================================ */
enum { KIND_NORMAL, KIND_DIALOG, KIND_POPUP, KIND_SPLASH };   /* POPUP: menus/tooltips mapped by request, shown unmanaged */
struct aurora_xwin {
    struct wl_list link;
    uint32_t id, frame;                  /* frame: 0 when the window is not reparented */
    bool override, mapped, managed, map_wanted, popup, gone;
    int ignore_unmaps;                   /* UnmapNotify events Aurora caused (reparenting a mapped window) */
    int x, y, w, h;                      /* as X reports it (x,y in the parent) */
    int fx, fy, fw, fh;                  /* the top-level window (frame, or the client itself) in root coordinates */
    int el, et, er, eb;                  /* frame extents in effect (0 when fullscreen or undecorated) */
    bool decorated;
    int pending;                         /* property replies outstanding before managing */
    char wm_name[128], net_name[128], app_id[96];
    bool proto_delete, proto_focus, input, iconic;
    uint32_t transient, type;
    bool modal, want_full, want_max;
    int min_w, min_h, max_w, max_h, gravity; bool position_hint;
    uint32_t pid;
    int gtk[4]; bool has_gtk;
    uint32_t* icon; int icon_w, icon_h;  /* _NET_WM_ICON scaled to AURORA_XICON^2, and the source size */
    uint64_t serial; bool has_serial; uint32_t surface_id;
    struct wl_resource* surface;
    struct aurora_window* host;
    bool is_max, is_full, is_min, active; unsigned flags;
    int deco_w, deco_h; bool deco_valid, deco_active; char deco_title[128];
};
static struct wl_list xwins; static unsigned xwin_count; static struct aurora_xwin* focused;
static uint32_t* stack_last; static int stack_last_n;

static struct aurora_xwin* find(uint32_t id) {
    struct aurora_xwin* w; if (!id) return NULL;
    wl_list_for_each(w, &xwins, link) if (w->id == id || w->frame == id) return w;
    return NULL;
}
static struct aurora_xwin* find_client(uint32_t id) {
    struct aurora_xwin* w; if (!id) return NULL;
    wl_list_for_each(w, &xwins, link) if (w->id == id) return w;
    return NULL;
}
static struct aurora_xwin* track(uint32_t id, bool override, int x, int y, int w, int h) {
    struct aurora_xwin* x11 = find_client(id); if (x11) return x11;
    if (xwin_count >= MAX_WINDOWS) { ODD("more than %d X windows: 0x%x left unmanaged", MAX_WINDOWS, id); return NULL; }
    x11 = calloc(1, sizeof(*x11)); if (!x11) return NULL;
    *x11 = (struct aurora_xwin){.id = id, .override = override, .x = x, .y = y, .w = w > 0 ? w : 1, .h = h > 0 ? h : 1, .input = true};
    wl_list_insert(xwins.prev, &x11->link); xwin_count++; return x11;
}
static const char* title_of(const struct aurora_xwin* w) { return w->net_name[0] ? w->net_name : w->wm_name; }
/* Two windows of one X client: the server gives every client its own resource-id base (the bits above the mask). */
static bool same_client(uint32_t a, uint32_t b) { return a && b && !((a ^ b) & ~id_mask); }
static bool focused_client(uint32_t window) { return focused && same_client(window, focused->id); }
static uint32_t top_of(const struct aurora_xwin* w) { return w->frame ? w->frame : w->id; }
static int client_w(const struct aurora_xwin* w) { return w->fw - w->el - w->er; }
static int client_h(const struct aurora_xwin* w) { return w->fh - w->et - w->eb; }
static void forget(struct aurora_xwin* w);

/* ---- text from properties ---- */
static void utf8_clean(char* out, size_t size, const uint8_t* in, size_t n, bool latin1) {   /* valid UTF-8, no controls */
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] && o + 4 < size;) {
        uint8_t c = in[i];
        if (latin1) { if (c < 0x20 || c == 0x7F) { out[o++] = ' '; i++; } else if (c < 0x80) { out[o++] = (char)c; i++; } else { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); i++; } continue; }
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0; bool ok = len > 0 && i + (size_t)len <= n;
        for (int k = 1; ok && k < len; k++) ok = (in[i + k] & 0xC0) == 0x80;
        if (ok && len == 2 && c < 0xC2) ok = false;   /* overlong */
        if (!ok) { out[o++] = '?'; i++; continue; }
        if (len == 1 && (c < 0x20 || c == 0x7F)) { out[o++] = ' '; i++; continue; }
        memcpy(out + o, in + i, (size_t)len); o += (size_t)len; i += (size_t)len;
    }
    out[o] = 0;
}
struct prop { uint8_t format; uint32_t type, after, count; const uint8_t* data; size_t bytes; };
static bool prop_parse(const uint8_t* r, size_t size, struct prop* p) {
    if (!r || size < 32) return false;
    p->format = r[1]; p->type = get32(r + 8); p->after = get32(r + 12); p->count = get32(r + 16); p->data = r + 32;
    if (!p->type || (p->format != 8 && p->format != 16 && p->format != 32)) return false;
    uint64_t bytes = (uint64_t)p->count * (p->format / 8); if (bytes > size - 32) return false;
    p->bytes = (size_t)bytes; return true;
}
static uint32_t prop_word(const struct prop* p, uint32_t i) { return p->format == 32 && i < p->count ? get32(p->data + 4 * i) : 0; }

/* ================================================================================================================
 * 2. xwayland_shell_v1 / WL_SURFACE_ID: the wl_surface of an X window
 * ================================================================================================================ */
struct xsurf { struct wl_list link; struct wl_resource* resource, *surface; struct wl_listener surface_gone; uint64_t serial; bool has_serial; };
static struct wl_list xsurfs; static struct wl_global* shell_global;
static void push_state(struct aurora_xwin* w);
static void associate(struct aurora_xwin* w, struct wl_resource* surface) {
    if (w->surface || w->gone || !surface || !server || wl_resource_get_client(surface) != server) return;
    bool unmanaged = w->override || w->popup;
    if (!unmanaged && !w->managed) return;   /* managed once its properties are in; the frame maps after that */
    struct aurora_window* host = unmanaged ? aurora_host_x11_attach(surface, w, true, w->x, w->y, w->w, w->h, title_of(w), w->app_id)
                                           : aurora_host_x11_attach(surface, w, false, w->fx, w->fy, w->fw, w->fh, title_of(w), w->app_id);
    if (!host) { ODD("surface of window 0x%x already has a role", w->id); return; }
    w->surface = surface; w->host = host;
    say("SURFACE window=0x%x %s app=%s x=%d y=%d w=%d h=%d", w->id, unmanaged ? "override-redirect" : "managed", w->app_id,
        unmanaged ? w->x : w->fx, unmanaged ? w->y : w->fy, unmanaged ? w->w : w->fw, unmanaged ? w->h : w->fh);
    if (!unmanaged) push_state(w);
}
static void xsurf_surface_gone(struct wl_listener* l, void* data) {
    (void)data; struct xsurf* x = wl_container_of(l, x, surface_gone);
    wl_list_remove(&x->surface_gone.link); wl_list_init(&x->surface_gone.link); x->surface = NULL;
}
static void xsurf_destroy(struct wl_resource* r) { struct xsurf* x = wl_resource_get_user_data(r); wl_list_remove(&x->surface_gone.link); wl_list_remove(&x->link); free(x); }
static void xsurf_set_serial(struct wl_client* c, struct wl_resource* r, uint32_t lo, uint32_t hi) {
    (void)c; struct xsurf* x = wl_resource_get_user_data(r); uint64_t serial = (uint64_t)hi << 32 | lo;
    if (x->has_serial) { wl_resource_post_error(r, XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED, "serial already set"); return; }
    if (!serial) { wl_resource_post_error(r, XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL, "serial 0"); return; }
    x->serial = serial; x->has_serial = true;
    struct aurora_xwin* w; wl_list_for_each(w, &xwins, link) if (w->has_serial && w->serial == serial && !w->surface) { associate(w, x->surface); break; }
}
static const struct xwayland_surface_v1_interface xsurf_interface = {xsurf_set_serial, destroy_request};
static void shell_get(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface) {
    struct xsurf* x;
    wl_list_for_each(x, &xsurfs, link) if (x->surface == surface) { wl_resource_post_error(r, XWAYLAND_SHELL_V1_ERROR_ROLE, "surface already has an xwayland_surface"); return; }
    x = calloc(1, sizeof(*x)); struct wl_resource* res = wl_resource_create(c, &xwayland_surface_v1_interface, wl_resource_get_version(r), id);
    if (!x || !res) { free(x); if (res) wl_resource_destroy(res); wl_resource_post_no_memory(r); return; }
    x->resource = res; x->surface = surface; x->surface_gone.notify = xsurf_surface_gone; wl_resource_add_destroy_listener(surface, &x->surface_gone);
    wl_list_insert(&xsurfs, &x->link); wl_resource_set_implementation(res, &xsurf_interface, x, xsurf_destroy);
}
static const struct xwayland_shell_v1_interface shell_interface = {destroy_request, shell_get};
static void shell_bind(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data;
    if (c != server) { wl_client_post_implementation_error(c, "xwayland_shell_v1 is for Aurora's Xwayland only"); return; }
    struct wl_resource* r = wl_resource_create(c, &xwayland_shell_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &shell_interface, NULL, NULL);
}
bool aurora_xwm_global_visible(const struct wl_client* client, const struct wl_global* global) { return global != shell_global || (server && client == server); }
void aurora_xwm_surface_created(struct wl_resource* surface) {   /* WL_SURFACE_ID: the message may come before the surface */
    if (!server || wl_resource_get_client(surface) != server) return;
    uint32_t id = wl_resource_get_id(surface); struct aurora_xwin* w;
    wl_list_for_each(w, &xwins, link) if (w->surface_id == id && !w->surface) { w->surface_id = 0; associate(w, surface); break; }
}
static void surface_id_message(struct aurora_xwin* w, uint32_t id) {
    w->surface_id = id; if (!server) return;
    struct wl_resource* r = wl_client_get_object(server, id);
    if (r && !strcmp(wl_resource_get_class(r), "wl_surface")) { w->surface_id = 0; associate(w, r); }
}
static void serial_message(struct aurora_xwin* w, uint64_t serial) {
    w->serial = serial; w->has_serial = serial != 0; w->surface_id = 0;
    struct xsurf* x; wl_list_for_each(x, &xsurfs, link) if (x->has_serial && x->serial == serial && x->surface) { associate(w, x->surface); break; }
}
void aurora_xwm_detached(struct aurora_xwin* w) { if (w) { w->host = NULL; w->surface = NULL; } }
static void detach(struct aurora_xwin* w) {
    struct aurora_window* h = w->host; w->host = NULL; w->surface = NULL;
    if (h) aurora_host_x11_detach(h);
    if (focused == w) focused = NULL;
}

/* ================================================================================================================
 * 4b. Decorations: a title bar with the traffic lights and the title, drawn with macui and sent as an image
 * ================================================================================================================ */
static void draw_frame(struct aurora_xwin* w, bool force) {
    if (!w->frame || !w->managed || xstate != XS_READY) return;
    const char* title = title_of(w);
    if (!force && w->deco_valid && w->deco_w == w->fw && w->deco_h == w->fh && w->deco_active == w->active && !strcmp(w->deco_title, title)) return;
    w->deco_valid = true; w->deco_w = w->fw; w->deco_h = w->fh; w->deco_active = w->active; snprintf(w->deco_title, sizeof(w->deco_title), "%s", title);
    if (!w->et && !w->el) return;   /* fullscreen: the client covers the frame */
    int fw = w->fw, fh = w->fh; uint32_t edge = w->active ? 0xC9C9C9 : 0xE2E2E2;
    int32_t sides[3][4] = {{0, w->et, w->el, fh - w->et}, {fw - w->er, w->et, w->er, fh - w->et}, {w->el, fh - w->eb, fw - w->el - w->er, w->eb}};
    x_fill(w->frame, edge, (const int32_t (*)[4])sides, 3);
    if (!w->et) return;
    uint32_t* pixels = images_ok && fw <= 8192 ? malloc((size_t)fw * w->et * 4) : NULL;
    if (!pixels) { int32_t bar[1][4] = {{0, 0, fw, w->et}}; x_fill(w->frame, w->active ? 0xE2E2E2 : 0xF2F2F2, (const int32_t (*)[4])bar, 1); return; }
    aurora_canvas(pixels, fw, w->et);
    if (w->active) ui_rrect_vgrad(0, 0, fw, w->et, 0, 0xEEEEEE, 0xDCDCDC, 255); else ui_rrect(0, 0, fw, w->et, 0, 0xF6F6F6, 255);
    ui_rrect(0, w->et - 1, fw, 1, 0, w->active ? 0xBDBDBD : 0xD8D8D8, 255);
    static const uint32_t lights[3] = {0xFF5F57, 0xFEBC2E, 0x28C840};
    for (int i = 0; i < 3; i++) { int cx = 16 + 20 * i, cy = w->et / 2;
        ui_circle(cx, cy, 6, w->active ? lights[i] : 0xD0D0D0, 255); ui_ring(cx, cy, 6, 1, w->active ? (lights[i] >> 1 & 0x7F7F7F) + 0x303030 : 0xBEBEBE, 90); }
    char shown[sizeof(w->deco_title)]; snprintf(shown, sizeof(shown), "%s", title);
    int room = fw - 2 * 76, length = (int)strlen(shown);
    while (length > 0 && ui_text_width(shown, UF_UIB) > room) {   /* drop whole UTF-8 characters, then "..." */
        do length--; while (length > 0 && ((uint8_t)shown[length] & 0xC0) == 0x80);
        snprintf(shown + length, sizeof(shown) - (size_t)length, "...");
        if (length == 0) shown[0] = 0;
    }
    if (room > 0 && shown[0]) ui_text_center(fw / 2, (w->et - ui_font_height(UF_UIB)) / 2, shown, UF_UIB, w->active ? 0x262626 : 0x9C9C9C);
    aurora_host_canvas_restore();
    x_put_image(w->frame, pixels, fw, w->et);
    free(pixels);
}
int aurora_xwm_frame_hit(struct aurora_xwin* w, int x, int y) {
    if (!w || !w->managed || !w->frame || (!w->et && !w->el)) return AURORA_XFRAME_CLIENT;
    int fw = w->fw, fh = w->fh, corner = 14;
    if (x < 0 || y < 0 || x >= fw || y >= fh) return AURORA_XFRAME_CLIENT;
    if (y < w->et) {
        for (int i = 0; i < 3; i++) { int dx = x - (16 + 20 * i), dy = y - w->et / 2; if (dx * dx + dy * dy <= 81) return AURORA_XFRAME_CLOSE + i; }
        if (y < 3) return x < corner ? 5 : x >= fw - corner ? 9 : 1;
        if (x < 3) return 4;
        if (x >= fw - 3) return 8;
        return AURORA_XFRAME_TITLE;
    }
    if (y >= fh - w->eb) return x < corner ? 6 : x >= fw - corner ? 10 : 2;
    if (x < w->el) return y >= fh - corner ? 6 : 4;
    if (x >= fw - w->er) return y >= fh - corner ? 10 : 8;
    return AURORA_XFRAME_CLIENT;
}

/* ================================================================================================================
 * 4c. Window state as X sees it
 * ================================================================================================================ */
static void send_configure_notify(struct aurora_xwin* w) {   /* ICCCM 4.1.5: the client's root position and size */
    uint8_t e[32] = {22}; put32(e + 4, w->id); put32(e + 8, w->id); put32(e + 12, 0);
    put16(e + 16, (uint16_t)(int16_t)(w->fx + w->el)); put16(e + 18, (uint16_t)(int16_t)(w->fy + w->et));
    put16(e + 20, (uint16_t)clampi(client_w(w), 1, 32767)); put16(e + 22, (uint16_t)clampi(client_h(w), 1, 32767)); put16(e + 24, 0); e[26] = 0;
    x_send_event(w->id, M_STRUCTURE, e);
}
static void write_wm_state(struct aurora_xwin* w, uint32_t state) { uint32_t v[2] = {state, 0}; x_change_property(w->id, ATOM(WM_STATE), ATOM(WM_STATE), 32, v, 2); }
static void write_net_state(struct aurora_xwin* w) {
    uint32_t v[6]; uint32_t n = 0;
    if (w->is_full) v[n++] = ATOM(_NET_WM_STATE_FULLSCREEN);
    if (w->is_max) { v[n++] = ATOM(_NET_WM_STATE_MAXIMIZED_VERT); v[n++] = ATOM(_NET_WM_STATE_MAXIMIZED_HORZ); }
    if (w->is_min) v[n++] = ATOM(_NET_WM_STATE_HIDDEN);
    if (w->modal) v[n++] = ATOM(_NET_WM_STATE_MODAL);
    if (w->active) v[n++] = ATOM(_NET_WM_STATE_FOCUSED);
    x_change_property(w->id, ATOM(_NET_WM_STATE), XA_ATOM, 32, v, n);
}
static void write_extents(struct aurora_xwin* w) { uint32_t v[4] = {(uint32_t)w->el, (uint32_t)w->er, (uint32_t)w->et, (uint32_t)w->eb}; x_change_property(w->id, ATOM(_NET_FRAME_EXTENTS), XA_CARDINAL, 32, v, 4); }
static void client_list(void) {
    uint32_t list[256]; uint32_t n = 0; struct aurora_xwin* w;
    wl_list_for_each(w, &xwins, link) if (w->managed && n < 256) list[n++] = w->id;
    x_change_property(root, ATOM(_NET_CLIENT_LIST), XA_WINDOW, 32, list, n);
}
static void restack(void) {   /* X stacking follows Aurora's window order */
    struct aurora_xwin* order[256]; int n = aurora_host_x11_stack(order, 256); uint32_t ids[256]; int k = 0;
    for (int i = 0; i < n; i++) if (order[i]->managed) ids[k++] = order[i]->id;
    bool same = k == stack_last_n && stack_last && !memcmp(ids, stack_last, (size_t)k * 4);
    if (same) return;
    for (int i = 1; i < n; i++) if (order[i]->managed && order[i - 1]->managed) {
        int32_t v[2] = {(int32_t)top_of(order[i - 1]), 0 /* Above */}; x_configure(top_of(order[i]), C_SIBLING | C_STACK, v); }
    x_change_property(root, ATOM(_NET_CLIENT_LIST_STACKING), XA_WINDOW, 32, ids, (uint32_t)k);
    uint32_t* copy = k ? malloc((size_t)k * 4) : NULL; if (k && !copy) return;
    if (k) memcpy(copy, ids, (size_t)k * 4);
    free(stack_last); stack_last = copy; stack_last_n = k;
}
/* Frame and client geometry: frame (or bare client) at fx,fy, outer size fw x fh. */
static void apply_geometry(struct aurora_xwin* w, int fx, int fy, int fw, int fh, bool notify) {
    fx = clampi(fx, -32000, 32000); fy = clampi(fy, -32000, 32000);
    fw = clampi(fw, w->el + w->er + 1, 32767); fh = clampi(fh, w->et + w->eb + 1, 32767);
    bool moved = fx != w->fx || fy != w->fy, sized = fw != w->fw || fh != w->fh;
    w->fx = fx; w->fy = fy; w->fw = fw; w->fh = fh;
    int32_t frame[4] = {fx, fy, fw, fh}, client[4] = {w->el, w->et, client_w(w), client_h(w)};
    if (w->frame) { if (moved || sized) x_configure(w->frame, C_X | C_Y | C_W | C_H, frame); if (sized || notify) x_configure(w->id, C_X | C_Y | C_W | C_H, client); }
    else if (moved || sized) { int32_t bare[4] = {fx, fy, fw, fh}; x_configure(w->id, C_X | C_Y | C_W | C_H, bare); }
    if (moved || sized || notify) send_configure_notify(w);
    if (sized) draw_frame(w, false);
}
static bool set_extents(struct aurora_xwin* w, bool full) {   /* true: the client's place in the frame changed */
    int el = 0, et = 0; if (w->frame && w->decorated && !full) { el = BORDER; et = TITLE_H; }
    if (el == w->el && et == w->et) return false;
    int cw = client_w(w), ch = client_h(w);
    w->el = w->er = w->eb = el; w->et = et; w->fw = cw + 2 * el; w->fh = ch + et + el;
    write_extents(w); w->deco_valid = false; return true;
}
/* Aurora decided a frame geometry: the client is told (and limited by its size hints). */
void aurora_xwm_configure(struct aurora_xwin* w, int x, int y, int width, int height, unsigned flags) {
    if (!w || !w->managed || xstate != XS_READY) return;
    bool full = flags & AURORA_XCONF_FULLSCREEN, max = flags & AURORA_XCONF_MAXIMIZED, active = flags & AURORA_XCONF_ACTIVATED;
    bool moved_inside = set_extents(w, full);
    int cw = width - w->el - w->er, ch = height - w->et - w->eb;
    if (!full) { if (w->max_w && cw > w->max_w) cw = w->max_w; if (w->max_h && ch > w->max_h) ch = w->max_h;
        if (w->min_w && cw < w->min_w) cw = w->min_w; if (w->min_h && ch < w->min_h) ch = w->min_h; }
    int fw = clampi(cw, 1, 32767) + w->el + w->er, fh = clampi(ch, 1, 32767) + w->et + w->eb;
    bool state = full != w->is_full || max != w->is_max || active != w->active, changed = state || x != w->fx || y != w->fy || fw != w->fw || fh != w->fh;
    w->is_full = full; w->is_max = max; w->active = active; w->flags = flags;
    apply_geometry(w, x, y, fw, fh, moved_inside);
    if ((fw != width || fh != height) && w->host && !full && !max && !(flags & AURORA_XCONF_TILED)) aurora_host_x11_resized(w->host, fw, fh);
    if (state) { write_net_state(w); draw_frame(w, false); }
    if (changed) say("CONFIGURE window=0x%x x=%d y=%d w=%d h=%d client=%dx%d%s%s%s", w->id, w->fx, w->fy, w->fw, w->fh, client_w(w), client_h(w),
                     full ? " fullscreen" : "", max ? " maximized" : "", active ? " active" : "");
}
void aurora_xwm_sync(struct aurora_xwin* w, int x, int y, bool minimized) {
    if (!w || !w->managed || xstate != XS_READY) return;
    if (x != w->fx || y != w->fy) apply_geometry(w, x, y, w->fw, w->fh, false);
    if (minimized != w->is_min) { w->is_min = minimized; write_wm_state(w, minimized ? 3 : 1); write_net_state(w); say("%s window=0x%x", minimized ? "ICONIC" : "NORMAL", w->id); }
}
void aurora_xwm_focus(struct aurora_xwin* w) {
    if (xstate != XS_READY) return;
    if (w && (!w->managed || w->gone)) w = NULL;
    struct aurora_xwin* old = focused;
    if (w != old) {
        focused = w;
        if (old && old->managed) { old->active = false; write_net_state(old); draw_frame(old, false); }
        if (w) {
            w->active = true; write_net_state(w); draw_frame(w, false);
            if (w->input || !w->proto_focus) x_set_focus(w->id, 1 /* RevertToPointerRoot */);
            if (w->proto_focus) x_client_message(w->id, ATOM(WM_PROTOCOLS), ATOM(WM_TAKE_FOCUS), last_time, 0);
            uint32_t id = w->id; x_change_property(root, ATOM(_NET_ACTIVE_WINDOW), XA_WINDOW, 32, &id, 1);
            say("FOCUS window=0x%x app=%s%s", w->id, w->app_id, w->proto_focus ? " take_focus" : "");
        } else {
            x_set_focus(0 /* None */, 0); uint32_t none = 0; x_change_property(root, ATOM(_NET_ACTIVE_WINDOW), XA_WINDOW, 32, &none, 1);
        }
    }
    restack();
}
void aurora_xwm_close(struct aurora_xwin* w) {
    if (!w || xstate != XS_READY) return;
    if (w->proto_delete) { x_client_message(w->id, ATOM(WM_PROTOCOLS), ATOM(WM_DELETE_WINDOW), last_time, 0); say("CLOSE window=0x%x app=%s via=WM_DELETE_WINDOW", w->id, w->app_id); }
    else { x_kill_client(w->id); say("CLOSE window=0x%x app=%s via=XKillClient", w->id, w->app_id); }
}
bool aurora_xwm_takes_focus(struct aurora_xwin* w) { return w && !w->override && !w->popup; }
/* What the client asked for before or while it was mapped goes to Aurora once the window has a surface. */
static void push_state(struct aurora_xwin* w) {
    if (!w->host) return;
    aurora_host_x11_title(w->host, title_of(w), w->app_id);
    int min_w = w->min_w ? w->min_w + w->el + w->er : 0, min_h = w->min_h ? w->min_h + w->et + w->eb : 0;
    int max_w = w->max_w ? w->max_w + w->el + w->er : 0, max_h = w->max_h ? w->max_h + w->et + w->eb : 0;
    aurora_host_x11_limits(w->host, min_w, min_h, max_w, max_h);
    struct aurora_xwin* parent = w->transient ? find_client(w->transient) : NULL;
    aurora_host_x11_transient(w->host, parent && parent != w ? parent->host : NULL, w->modal);
    if (w->icon) aurora_host_x11_icon(w->host, w->icon);
    if (w->has_gtk && !w->frame) aurora_host_x11_geometry(w->host, w->gtk[0], w->gtk[2], w->fw - w->gtk[0] - w->gtk[1], w->fh - w->gtk[2] - w->gtk[3]);
    if (w->want_full) { w->want_full = false; aurora_host_x11_request(w->host, AURORA_XREQ_FULLSCREEN); }
    if (w->want_max) { w->want_max = false; aurora_host_x11_request(w->host, AURORA_XREQ_MAXIMIZE); }
    if (w->iconic) { w->iconic = false; aurora_host_x11_request(w->host, AURORA_XREQ_MINIMIZE); }
}

/* ================================================================================================================
 * 4d. Properties
 * ================================================================================================================ */
static void manage(struct aurora_xwin* w);
/* _NET_WM_ICON: CARDINAL (width, height, width*height ARGB)...; the smallest image of at least AURORA_XICON, else the
 * largest, scaled (nearest, aspect kept) into a transparent AURORA_XICON square. */
static void read_icon(struct aurora_xwin* w, const struct prop* p, bool present) {
    free(w->icon); w->icon = NULL; w->icon_w = w->icon_h = 0;
    if (!present || p->format != 32) return;
    uint32_t best = 0, bw = 0, bh = 0;
    for (uint32_t i = 0; i + 2 <= p->count;) {
        uint32_t iw = prop_word(p, i), ih = prop_word(p, i + 1);
        if (!iw || !ih || iw > 1024 || ih > 1024 || (uint64_t)iw * ih > p->count - i - 2) break;
        uint32_t side = iw > ih ? iw : ih, best_side = bw > bh ? bw : bh;
        bool better = !bw || (side >= AURORA_XICON ? (best_side < AURORA_XICON || iw * ih < bw * bh) : (best_side < AURORA_XICON && iw * ih > bw * bh));
        if (better) { best = i + 2; bw = iw; bh = ih; }
        i += 2 + iw * ih;
    }
    if (!bw) return;
    uint32_t* out = calloc(AURORA_XICON * AURORA_XICON, 4); if (!out) return;
    uint32_t side = bw > bh ? bw : bh;
    for (uint32_t y = 0; y < AURORA_XICON; y++) for (uint32_t x = 0; x < AURORA_XICON; x++) {
        int64_t sx = (int64_t)x * side / AURORA_XICON - (int64_t)(side - bw) / 2, sy = (int64_t)y * side / AURORA_XICON - (int64_t)(side - bh) / 2;
        if (sx >= 0 && sy >= 0 && sx < bw && sy < bh) out[y * AURORA_XICON + x] = prop_word(p, best + (uint32_t)sy * bw + (uint32_t)sx);
    }
    w->icon = out; w->icon_w = (int)bw; w->icon_h = (int)bh;
}
static void apply_property(struct aurora_xwin* w, uint32_t atom, const struct prop* p, bool present) {
    if (atom == XA_WM_NAME) { w->wm_name[0] = 0; if (present && p->format == 8) utf8_clean(w->wm_name, sizeof(w->wm_name), p->data, p->bytes, p->type == XA_STRING); }
    else if (atom == ATOM(_NET_WM_NAME)) { w->net_name[0] = 0; if (present && p->format == 8) utf8_clean(w->net_name, sizeof(w->net_name), p->data, p->bytes, false); }
    else if (atom == XA_WM_CLASS) {
        w->app_id[0] = 0; if (!present || p->format != 8) return;
        size_t first = strnlen((const char*)p->data, p->bytes); const uint8_t* cls = p->data + first + 1; size_t n = first + 1 < p->bytes ? p->bytes - first - 1 : 0;
        if (!n || !cls[0]) { cls = p->data; n = first; }   /* no class: the instance name */
        size_t o = 0; for (size_t i = 0; i < n && cls[i] && o + 1 < sizeof(w->app_id); i++) { uint8_t c = cls[i];
            if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') w->app_id[o++] = (char)c; }
        w->app_id[o] = 0;
    }
    else if (atom == XA_WM_HINTS) {
        w->input = true; if (!present || p->count < 2) return; uint32_t flags = prop_word(p, 0);
        if (flags & 1) w->input = prop_word(p, 1) != 0;
        if ((flags & 2) && p->count >= 3 && !w->managed) w->iconic = prop_word(p, 2) == 3;
    }
    else if (atom == XA_WM_NORMAL_HINTS) {
        w->min_w = w->min_h = w->max_w = w->max_h = 0; w->gravity = 1; w->position_hint = false;
        if (!present || p->count < 15) return; uint32_t flags = prop_word(p, 0);
        w->position_hint = flags & (1 | 4);
        if (flags & 16) { w->min_w = clampi((int)prop_word(p, 5), 0, 32767); w->min_h = clampi((int)prop_word(p, 6), 0, 32767); }
        else if ((flags & 256) && p->count >= 17) { w->min_w = clampi((int)prop_word(p, 15), 0, 32767); w->min_h = clampi((int)prop_word(p, 16), 0, 32767); }
        if (flags & 32) { w->max_w = clampi((int)prop_word(p, 7), 0, 32767); w->max_h = clampi((int)prop_word(p, 8), 0, 32767); }
        if (w->max_w && w->max_w < w->min_w) w->max_w = 0;
        if (w->max_h && w->max_h < w->min_h) w->max_h = 0;
        if ((flags & 512) && p->count >= 18) w->gravity = clampi((int)prop_word(p, 17), 0, 10);
    }
    else if (atom == ATOM(WM_PROTOCOLS)) {
        w->proto_delete = w->proto_focus = false; if (!present || p->format != 32) return;
        for (uint32_t i = 0; i < p->count && i < 64; i++) { uint32_t a = prop_word(p, i);
            if (a == ATOM(WM_DELETE_WINDOW)) w->proto_delete = true; else if (a == ATOM(WM_TAKE_FOCUS)) w->proto_focus = true; }
    }
    else if (atom == XA_WM_TRANSIENT_FOR) { uint32_t t = present ? prop_word(p, 0) : 0; w->transient = t == w->id || t == root ? 0 : t; }
    else if (atom == ATOM(_NET_WM_WINDOW_TYPE)) {
        w->type = 0; if (!present || p->format != 32) return;
        for (uint32_t i = 0; i < p->count && i < 32 && !w->type; i++) { uint32_t a = prop_word(p, i);
            for (int k = A__NET_WM_WINDOW_TYPE_NORMAL; k <= A__NET_WM_WINDOW_TYPE_DESKTOP; k++) if (a == atoms[k]) { w->type = a; break; } }
    }
    else if (atom == ATOM(_NET_WM_STATE) && !w->managed) {   /* the initial state (EWMH: set before mapping) */
        if (!present || p->format != 32) return;
        for (uint32_t i = 0; i < p->count && i < 32; i++) { uint32_t a = prop_word(p, i);
            if (a == ATOM(_NET_WM_STATE_FULLSCREEN)) w->want_full = true;
            else if (a == ATOM(_NET_WM_STATE_MAXIMIZED_VERT) || a == ATOM(_NET_WM_STATE_MAXIMIZED_HORZ)) w->want_max = true;
            else if (a == ATOM(_NET_WM_STATE_MODAL)) w->modal = true;
            else if (a == ATOM(_NET_WM_STATE_HIDDEN)) w->iconic = true; }
    }
    else if (atom == ATOM(_NET_WM_PID)) w->pid = present ? prop_word(p, 0) : 0;
    else if (atom == ATOM(_MOTIF_WM_HINTS) && !w->managed) {
        w->decorated = true; if (!present || p->format != 32 || p->count < 3) return;
        uint32_t flags = prop_word(p, 0), decor = prop_word(p, 2);
        if ((flags & 2) && !(decor & 1) && !(decor & 8)) w->decorated = false;   /* MWM_HINTS_DECORATIONS without ALL or TITLE */
    }
    else if (atom == ATOM(_NET_WM_ICON)) read_icon(w, p, present);
    else if (atom == ATOM(_GTK_FRAME_EXTENTS)) {
        w->has_gtk = present && p->format == 32 && p->count >= 4;
        for (int i = 0; i < 4; i++) w->gtk[i] = w->has_gtk ? clampi((int)prop_word(p, (uint32_t)i), 0, 512) : 0;
    }
}
static void property_reply(const uint8_t* r, size_t size, uint32_t window, uint32_t atom) {
    struct aurora_xwin* w = find_client(window); if (!w) return;
    struct prop p = {0}; bool present = prop_parse(r, size, &p);
    if (atom == ATOM(_MOTIF_WM_HINTS) && !w->managed) w->decorated = true;
    apply_property(w, atom, &p, present);
    if (w->pending > 0 && --w->pending == 0) { if (w->map_wanted && !w->managed) manage(w); return; }
    if (w->pending || !w->host) return;
    if (atom == XA_WM_NAME || atom == ATOM(_NET_WM_NAME) || atom == XA_WM_CLASS) { aurora_host_x11_title(w->host, title_of(w), w->app_id); draw_frame(w, false); }
    else if (atom == XA_WM_NORMAL_HINTS || atom == XA_WM_TRANSIENT_FOR || atom == ATOM(_GTK_FRAME_EXTENTS)) push_state(w);
    else if (atom == ATOM(_NET_WM_ICON)) aurora_host_x11_icon(w->host, w->icon);
}
static const uint32_t* property_atoms(int* n) {
    static uint32_t list[13];
    uint32_t v[13] = {XA_WM_NAME, ATOM(_NET_WM_NAME), XA_WM_CLASS, XA_WM_HINTS, XA_WM_NORMAL_HINTS, ATOM(WM_PROTOCOLS), XA_WM_TRANSIENT_FOR,
                      ATOM(_NET_WM_WINDOW_TYPE), ATOM(_NET_WM_STATE), ATOM(_NET_WM_PID), ATOM(_MOTIF_WM_HINTS), ATOM(_GTK_FRAME_EXTENTS), ATOM(_NET_WM_ICON)};
    memcpy(list, v, sizeof(v)); *n = 13; return list;
}
static uint32_t property_length(uint32_t atom) {   /* 32-bit words */
    return atom == ATOM(_NET_WM_ICON) ? 65536 : atom == XA_WM_NAME || atom == ATOM(_NET_WM_NAME) || atom == XA_WM_CLASS ? 256 : 64; }
static void read_property(struct aurora_xwin* w, uint32_t atom) { x_get_property(w->id, atom, false, 0, 0, property_length(atom), property_reply, w->id, atom); }

/* ================================================================================================================
 * 4e. Managing a window
 * ================================================================================================================ */
static bool popup_type(uint32_t type) {
    return type && (type == ATOM(_NET_WM_WINDOW_TYPE_MENU) || type == ATOM(_NET_WM_WINDOW_TYPE_DROPDOWN_MENU) || type == ATOM(_NET_WM_WINDOW_TYPE_POPUP_MENU) ||
        type == ATOM(_NET_WM_WINDOW_TYPE_TOOLTIP) || type == ATOM(_NET_WM_WINDOW_TYPE_NOTIFICATION) || type == ATOM(_NET_WM_WINDOW_TYPE_COMBO) ||
        type == ATOM(_NET_WM_WINDOW_TYPE_DND) || type == ATOM(_NET_WM_WINDOW_TYPE_DOCK) || type == ATOM(_NET_WM_WINDOW_TYPE_DESKTOP));
}
static void map_request(uint32_t id) {
    struct aurora_xwin* w = find_client(id);
    if (!w) { w = track(id, false, 0, 0, 1, 1); if (!w) { x_map(id); return; } }
    if (w->managed || w->popup) { x_map(id); return; }
    w->map_wanted = true; if (w->pending) return;
    int n; const uint32_t* list = property_atoms(&n); w->decorated = true;
    for (int i = 0; i < n; i++) { w->pending++; read_property(w, list[i]); }
}
static void manage(struct aurora_xwin* w) {
    if (w->managed || w->gone || xstate != XS_READY) return;
    if (popup_type(w->type)) {   /* a menu or tooltip that is not override-redirect: unmanaged, where it asked to be */
        w->popup = true; w->map_wanted = false; x_map(w->id); say("POPUP window=0x%x x=%d y=%d w=%d h=%d", w->id, w->x, w->y, w->w, w->h); return;
    }
    if (w->type == ATOM(_NET_WM_WINDOW_TYPE_SPLASH)) w->decorated = false;
    uint32_t frame = w->decorated ? x_id() : 0;
    int el = frame ? BORDER : 0, et = frame ? TITLE_H : 0;
    int cw = w->w, ch = w->h;
    if (w->max_w && cw > w->max_w) cw = w->max_w; if (w->max_h && ch > w->max_h) ch = w->max_h;
    if (w->min_w && cw < w->min_w) cw = w->min_w; if (w->min_h && ch < w->min_h) ch = w->min_h;
    cw = clampi(cw, 1, 32767); ch = clampi(ch, 1, 32767);
    int fw = cw + 2 * el, fh = ch + et + el, fx, fy;
    struct aurora_xwin* parent = w->transient ? find_client(w->transient) : NULL;
    int ax, ay, aw, ah;
    if (parent && parent->host) aurora_host_x11_place(parent->host, fw, fh, &fx, &fy);
    else if (w->position_hint && (w->x || w->y)) {   /* USPosition/PPosition: NorthWest puts the frame there, Static the client */
        fx = w->gravity == 10 ? w->x - el : w->x; fy = w->gravity == 10 ? w->y - et : w->y;
        aurora_host_x11_work_area(fx + fw / 2, fy + 10, &ax, &ay, &aw, &ah);
        if (fy < ay) fy = ay; if (fy > ay + ah - TITLE_H) fy = ay + ah - TITLE_H;
        if (fx > ax + aw - 60) fx = ax + aw - 60; if (fx + fw < ax + 60) fx = ax + 60 - fw;
    } else aurora_host_x11_place(NULL, fw, fh, &fx, &fy);
    aurora_host_x11_work_area(fx + fw / 2, fy + fh / 2, &ax, &ay, &aw, &ah);
    if (fw > aw && aw > 2 * el + 1) { fw = aw; cw = fw - 2 * el; fx = ax; }   /* larger than the display: it fits */
    if (fh > ah && ah > et + el + 1) { fh = ah; ch = fh - et - el; fy = ay; }
    w->frame = frame; w->el = w->er = w->eb = el; w->et = et; w->fx = fx; w->fy = fy; w->fw = fw; w->fh = fh;
    if (frame) {
        uint8_t* p = x_reserve(44);
        if (p) { p[0] = 1; p[1] = 0 /* CopyFromParent depth */; put32(p + 4, frame); put32(p + 8, root); put16(p + 12, (uint16_t)(int16_t)fx); put16(p + 14, (uint16_t)(int16_t)fy);
            put16(p + 16, (uint16_t)fw); put16(p + 18, (uint16_t)fh); put16(p + 20, 0); put16(p + 22, 1 /* InputOutput */); put32(p + 24, 0 /* CopyFromParent */);
            put32(p + 28, (1u << 1) | (1u << 3) | (1u << 11)); put32(p + 32, 0xE6E6E6); put32(p + 36, 0); put32(p + 40, M_REDIRECT | M_SUBSTRUCTURE | M_EXPOSURE);
            x_commit(44); }
        x_save_set(w->id, true); if (w->mapped) w->ignore_unmaps++;   /* reparenting a viewable window unmaps it once */
        int32_t border0[1] = {0}; x_configure(w->id, C_BORDER, border0);
        uint8_t* q = x_reserve(16); if (q) { q[0] = 7; put32(q + 4, w->id); put32(q + 8, frame); put16(q + 12, (uint16_t)el); put16(q + 14, (uint16_t)et); x_commit(16); }
        int32_t size[2] = {cw, ch}; x_configure(w->id, C_W | C_H, size);
    } else { int32_t geometry[5] = {fx, fy, cw, ch, 0}; x_configure(w->id, C_X | C_Y | C_W | C_H | C_BORDER, geometry); }
    x_change_attributes(w->id, 1u << 11, M_PROPERTY | M_FOCUS);
    w->managed = true; w->map_wanted = false; w->is_min = false;
    write_wm_state(w, 1); write_extents(w);
    uint32_t actions[8] = {ATOM(_NET_WM_ACTION_MOVE), ATOM(_NET_WM_ACTION_RESIZE), ATOM(_NET_WM_ACTION_MINIMIZE), ATOM(_NET_WM_ACTION_MAXIMIZE_HORZ),
                           ATOM(_NET_WM_ACTION_MAXIMIZE_VERT), ATOM(_NET_WM_ACTION_FULLSCREEN), ATOM(_NET_WM_ACTION_CLOSE)};
    x_change_property(w->id, ATOM(_NET_WM_ALLOWED_ACTIONS), XA_ATOM, 32, actions, 7);
    write_net_state(w);
    x_map(w->id); if (frame) x_map(frame);
    send_configure_notify(w); w->deco_valid = false; draw_frame(w, true); client_list();
    say("MANAGE window=0x%x frame=0x%x app=%s title=%s x=%d y=%d w=%d h=%d client=%dx%d%s%s", w->id, frame, w->app_id, title_of(w), fx, fy, fw, fh, cw, ch,
        w->transient ? " transient" : "", w->modal ? " modal" : "");
}
/* The client withdrew or went: the frame goes, the client returns to the root (unless destroyed). */
static void unmanage(struct aurora_xwin* w, bool destroyed) {
    detach(w);
    if (w->managed) {
        if (!destroyed) {
            write_wm_state(w, 0); x_delete_property(w->id, ATOM(_NET_WM_STATE));
            if (w->frame) { uint8_t* q = x_reserve(16); if (q) { q[0] = 7; put32(q + 4, w->id); put32(q + 8, root); put16(q + 12, (uint16_t)(int16_t)(w->fx + w->el)); put16(q + 14, (uint16_t)(int16_t)(w->fy + w->et)); x_commit(16); }
                x_save_set(w->id, false); }
        }
        if (w->frame) x_destroy(w->frame);
        say("UNMANAGE window=0x%x app=%s%s", w->id, w->app_id, destroyed ? " destroyed" : "");
    }
    w->x = w->fx + w->el; w->y = w->fy + w->et; if (w->managed) { w->w = client_w(w); w->h = client_h(w); }
    w->frame = 0; w->managed = false; w->popup = false; w->map_wanted = false; w->el = w->et = w->er = w->eb = 0;
    w->is_max = w->is_full = w->is_min = w->active = false; w->deco_valid = false; w->has_serial = false; w->surface_id = 0;
    client_list();
}
static void forget(struct aurora_xwin* w) {
    if (w->managed) unmanage(w, true); else detach(w);
    w->gone = true; if (focused == w) focused = NULL;
    wl_list_remove(&w->link); xwin_count--; free(w->icon); free(w);
}

/* ================================================================================================================
 * 5. CLIPBOARD bridge
 * ================================================================================================================ */
struct incoming { int fd; char mime[96]; uint32_t target; uint8_t* data; size_t size, cap, written; bool incr, finished; struct wl_event_source* source, *timer; };
struct outgoing { int fd; uint32_t requestor, property, type; uint8_t* data; size_t size, cap, sent; bool incr, reading, waiting; struct wl_event_source* source, *timer; };
#define TRANSFERS 8
static struct incoming in_queue[TRANSFERS]; static int in_head, in_count;   /* X -> Wayland, one conversion at a time */
static struct outgoing out[TRANSFERS];                                     /* Wayland -> X */
static uint32_t sel_owner; static unsigned sel_generation; static bool sel_from_x;
static struct { char mime[96]; uint32_t atom; } sel_targets[32]; static int sel_target_count, sel_names_pending;
static bool own_clipboard; static const void* last_selection; static bool last_from_x = true;
static struct { char mime[96]; uint32_t atom; } wl_atoms[32]; static int wl_atom_count;   /* atoms of the Wayland selection's types */

static void in_next(void);
static struct incoming* in_current(void) { return in_count ? &in_queue[in_head] : NULL; }
static void in_finish(bool ok) {
    struct incoming* t = in_current(); if (!t) return;
    if (t->source) wl_event_source_remove(t->source); if (t->timer) wl_event_source_remove(t->timer);
    if (t->fd >= 0) close(t->fd); free(t->data);
    if (!ok) ODD("X selection transfer (%s) failed", t->mime);
    *t = (struct incoming){.fd = -1}; in_head = (in_head + 1) % TRANSFERS; in_count--;
    in_next();
}
/* A write to a client's pipe whose reader may be gone: EPIPE, never SIGPIPE (whose default would end Aurora). */
static ssize_t pipe_write(int fd, const void* data, size_t size) {
    struct sigaction ignore = {.sa_handler = SIG_IGN}, old; sigemptyset(&ignore.sa_mask);
    bool swapped = !sigaction(SIGPIPE, &ignore, &old); ssize_t n = write(fd, data, size); int error = errno;
    if (swapped) sigaction(SIGPIPE, &old, NULL);
    errno = error; return n;
}
static int in_writable(int fd, uint32_t mask, void* data) {
    (void)fd; (void)mask; (void)data; struct incoming* t = in_current(); if (!t) return 0;
    while (t->written < t->size) {
        ssize_t n = pipe_write(t->fd, t->data + t->written, t->size - t->written);
        if (n > 0) { t->written += (size_t)n; if (t->timer) wl_event_source_timer_update(t->timer, 10000); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        in_finish(false); return 0;   /* the reader went away */
    }
    if (t->finished) { in_finish(true); return 0; }
    if (t->source) { wl_event_source_remove(t->source); t->source = NULL; }
    return 0;
}
static void in_push(const uint8_t* data, size_t n) {
    struct incoming* t = in_current(); if (!t || !n) return;
    if (t->size + n > MAX_TRANSFER) { in_finish(false); return; }
    if (t->size + n > t->cap) { size_t cap = t->cap ? t->cap : 4096; while (cap < t->size + n) cap *= 2;
        uint8_t* grown = realloc(t->data, cap); if (!grown) { in_finish(false); return; } t->data = grown; t->cap = cap; }
    memcpy(t->data + t->size, data, n); t->size += n;
}
static void in_flush(void) {   /* write what arrived; finished transfers end once written */
    struct incoming* t = in_current(); if (!t) return;
    in_writable(t->fd, 0, NULL); t = in_current(); if (!t || t->written >= t->size) return;
    if (!t->source) t->source = wl_event_loop_add_fd(loop, t->fd, WL_EVENT_WRITABLE, in_writable, NULL);
}
static int in_timeout(void* data) { (void)data; struct incoming* t = in_current(); if (t) { ODD("X selection owner did not answer"); in_finish(false); } return 0; }   /* in_finish removes the timer (safe in its own callback) */
static void in_property(const uint8_t* r, size_t size, uint32_t a, uint32_t b);
static void in_read(uint32_t offset) { x_get_property(wm_window, ATOM(_AURORA_DATA), false, 0, offset, CHUNK / 4, in_property, sel_generation, offset); }
static void in_property(const uint8_t* r, size_t size, uint32_t generation, uint32_t offset) {
    struct incoming* t = in_current(); if (!t || generation != sel_generation) return;
    struct prop p = {0};
    if (!prop_parse(r, size, &p)) { if (t->incr && r) { t->finished = true; x_delete_property(wm_window, ATOM(_AURORA_DATA)); in_flush(); } else in_finish(false); return; }
    if (p.type == ATOM(INCR) && !t->incr && offset == 0) { t->incr = true; x_delete_property(wm_window, ATOM(_AURORA_DATA)); return; }   /* chunks follow */
    if (t->incr && offset == 0 && p.bytes == 0) { t->finished = true; x_delete_property(wm_window, ATOM(_AURORA_DATA)); in_flush(); return; }
    in_push(p.data, p.bytes); t = in_current(); if (!t) return;
    if (t->timer) wl_event_source_timer_update(t->timer, 10000);
    if (p.after) { in_read(offset + (uint32_t)(p.bytes / 4)); return; }
    x_delete_property(wm_window, ATOM(_AURORA_DATA));   /* INCR: the deletion asks for the next chunk */
    if (!t->incr) t->finished = true;
    in_flush();
}
static void in_next(void) {
    struct incoming* t = in_current(); if (!t || t->target || xstate != XS_READY) return;
    if (!sel_owner) { in_finish(false); return; }
    t->target = XA_STRING;
    for (int i = 0; i < sel_target_count; i++) if (!strcmp(sel_targets[i].mime, t->mime)) { t->target = sel_targets[i].atom; break; }
    x_convert_selection(wm_window, ATOM(CLIPBOARD), t->target, ATOM(_AURORA_DATA), last_time);
    t->timer = wl_event_loop_add_timer(loop, in_timeout, NULL); if (t->timer) wl_event_source_timer_update(t->timer, 5000);
}
void aurora_xwm_selection_send(const char* mime, int fd) {
    if (fd < 0) return;
    if (xstate != XS_READY || !sel_owner || !mime || in_count == TRANSFERS) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct incoming* t = &in_queue[(in_head + in_count) % TRANSFERS]; *t = (struct incoming){.fd = fd}; snprintf(t->mime, sizeof(t->mime), "%s", mime); in_count++;
    if (in_count == 1) in_next();
}
static void in_clear(void) { while (in_count) in_finish(false); }
/* the owner's TARGETS become the Wayland source's MIME types */
static void targets_publish(void) {
    if (sel_names_pending) return;
    const char* mimes[40]; int n = 0;
    for (int i = 0; i < sel_target_count && n < 40; i++) { bool dup = false; for (int k = 0; k < n; k++) if (!strcmp(mimes[k], sel_targets[i].mime)) dup = true; if (!dup) mimes[n++] = sel_targets[i].mime; }
    if (!n) return;
    sel_from_x = true; last_selection = NULL; last_from_x = true;
    aurora_host_x11_selection(mimes, n);
    say("CLIPBOARD from X owner=0x%x types=%d", sel_owner, n);
}
static void target_add(const char* mime, uint32_t atom) {
    if (sel_target_count < 32) { snprintf(sel_targets[sel_target_count].mime, sizeof(sel_targets[0].mime), "%s", mime); sel_targets[sel_target_count++].atom = atom; }
}
static void atom_name_reply(const uint8_t* r, size_t size, uint32_t generation, uint32_t atom) {
    if (generation != sel_generation) return;
    if (r && size >= 32) { size_t n = get16(r + 8); if (n <= size - 32 && n > 0 && n < 96) { char name[96]; memcpy(name, r + 32, n); name[n] = 0;
        bool ok = strchr(name, '/') || !strcmp(name, "x-kde-passwordManagerHint");
        for (size_t i = 0; i < n && ok; i++) ok = name[i] > 0x20 && name[i] < 0x7F;
        if (ok) target_add(name, atom); } }
    if (sel_names_pending > 0 && --sel_names_pending == 0) targets_publish();
}
static void targets_reply(const uint8_t* r, size_t size, uint32_t generation, uint32_t unused) {
    (void)unused; if (generation != sel_generation) return;
    struct prop p = {0}; sel_target_count = 0; sel_names_pending = 0;
    if (!prop_parse(r, size, &p) || p.format != 32) { target_add("text/plain;charset=utf-8", ATOM(UTF8_STRING)); target_add("UTF8_STRING", ATOM(UTF8_STRING)); targets_publish(); return; }
    for (uint32_t i = 0; i < p.count && i < 64; i++) {
        uint32_t a = prop_word(&p, i);
        if (a == ATOM(UTF8_STRING)) { target_add("text/plain;charset=utf-8", a); target_add("UTF8_STRING", a); }
        else if (a == XA_STRING) { target_add("STRING", a); target_add("text/plain", a); }
        else if (a == ATOM(TEXT)) target_add("TEXT", a);
        else if (a == ATOM(TARGETS) || a == ATOM(TIMESTAMP) || a == ATOM(MULTIPLE) || a == ATOM(COMPOUND_TEXT) || !a) continue;
        else if (sel_names_pending < 24) { uint8_t* q = x_reserve(8); if (q) { q[0] = 17; put32(q + 4, a); sel_names_pending++; x_call(8, atom_name_reply, sel_generation, a); } }
    }
    if (!sel_names_pending) targets_publish();
}
static void selection_owner_changed(uint32_t owner) {   /* XFixes: CLIPBOARD has a new owner */
    if (owner == wm_window) return;
    sel_generation++; in_clear(); sel_owner = 0; sel_target_count = 0;
    if (!owner) { if (sel_from_x) { sel_from_x = false; aurora_host_x11_selection(NULL, 0); } return; }
    if (!aurora_host_x11_focused()) {   /* only while X has the keyboard (Wine owns it from another connection: no same-client test) */
        ODD("CLIPBOARD taken by X window 0x%x without the keyboard focus: not shared", owner); if (sel_from_x) { sel_from_x = false; aurora_host_x11_selection(NULL, 0); } return; }
    own_clipboard = false; sel_owner = owner;
    x_convert_selection(wm_window, ATOM(CLIPBOARD), ATOM(TARGETS), ATOM(_AURORA_TARGETS), last_time);
}
static void selection_notify(const uint8_t* e) {   /* an answer to our ConvertSelection */
    uint32_t requestor = get32(e + 8), selection = get32(e + 12), target = get32(e + 16), property = get32(e + 20);
    if (requestor != wm_window || selection != ATOM(CLIPBOARD)) return;
    if (target == ATOM(TARGETS)) { if (property) x_get_property(wm_window, ATOM(_AURORA_TARGETS), true, XA_ATOM, 0, 64, targets_reply, sel_generation, 0); else targets_reply(NULL, 0, sel_generation, 0); return; }
    struct incoming* t = in_current(); if (!t || t->target != target) return;
    if (t->timer) wl_event_source_timer_update(t->timer, 10000);   /* from now on: 10 s without progress ends it (INCR owners too) */
    if (!property) { in_finish(false); return; }
    in_read(0);
}
/* ---- Wayland -> X: Aurora owns CLIPBOARD on the X side while a Wayland program's copy is the selection ---- */
static void wl_atom_reply(const uint8_t* r, size_t size, uint32_t index, uint32_t generation) {
    if (generation != sel_generation || (int)index >= wl_atom_count) return;
    wl_atoms[index].atom = r && size >= 12 ? get32(r + 8) : 0;
}
void aurora_xwm_selection_changed(const void* selection, bool from_x11) {
    if (xstate != XS_READY) { last_selection = NULL; return; }
    if (selection == last_selection && from_x11 == last_from_x) return;
    last_selection = selection; last_from_x = from_x11;
    if (from_x11) return;   /* the X owner's own copy */
    sel_generation++; in_clear(); wl_atom_count = 0;
    if (sel_from_x) sel_from_x = false;
    if (!selection) { if (own_clipboard) { x_set_selection_owner(0, ATOM(CLIPBOARD), 0); own_clipboard = false; } return; }
    sel_owner = 0; own_clipboard = true; x_set_selection_owner(wm_window, ATOM(CLIPBOARD), 0);
    const char* types[32]; int n = aurora_host_selection_types(types, 32);
    for (int i = 0; i < n && wl_atom_count < 32; i++) {
        if (!strcmp(types[i], "text/plain;charset=utf-8") || !strcmp(types[i], "text/plain") || !strcmp(types[i], "UTF8_STRING") || !strcmp(types[i], "TEXT") || !strcmp(types[i], "STRING")) continue;
        size_t len = strlen(types[i]); if (!len || len >= sizeof(wl_atoms[0].mime)) continue;
        snprintf(wl_atoms[wl_atom_count].mime, sizeof(wl_atoms[0].mime), "%s", types[i]); wl_atoms[wl_atom_count].atom = 0;
        uint8_t* q = x_reserve(8 + ((len + 3) & ~(size_t)3)); if (!q) continue;
        q[0] = 16; q[1] = 0; put16(q + 4, (uint16_t)len); memcpy(q + 8, types[i], len); x_call(8 + ((len + 3) & ~(size_t)3), wl_atom_reply, (uint32_t)wl_atom_count, sel_generation);
        wl_atom_count++;
    }
    say("CLIPBOARD to X types=%d", n);
}
static void notify_requestor(uint32_t requestor, uint32_t selection, uint32_t target, uint32_t property, uint32_t time) {
    uint8_t e[32] = {31}; put32(e + 4, time); put32(e + 8, requestor); put32(e + 12, selection); put32(e + 16, target); put32(e + 20, property);
    x_send_event(requestor, 0, e);
}
static void out_end(struct outgoing* o) {
    if (o->source) wl_event_source_remove(o->source); if (o->timer) wl_event_source_remove(o->timer);
    if (o->fd >= 0) close(o->fd);
    bool shared = false; for (int i = 0; i < TRANSFERS; i++) if (&out[i] != o && out[i].incr && out[i].requestor == o->requestor) shared = true;   /* another INCR to it still needs PropertyNotify */
    if (o->incr && !shared) { struct aurora_xwin* w = find_client(o->requestor); x_change_attributes(o->requestor, 1u << 11, w && w->managed ? M_PROPERTY | M_FOCUS : 0); }
    free(o->data); *o = (struct outgoing){.fd = -1};
}
static void out_chunk(struct outgoing* o) {   /* INCR: the next piece (zero length: the end) */
    size_t limit = xmax - 32 < CHUNK ? xmax - 32 : CHUNK, n = o->size - o->sent < limit ? o->size - o->sent : limit;
    x_change_property(o->requestor, o->property, o->type, 8, o->data + o->sent, (uint32_t)n); o->sent += n; o->waiting = true;
    if (o->timer) wl_event_source_timer_update(o->timer, 10000);   /* 10 s for the requestor to take each piece */
    if (!n) out_end(o);
}
static void out_deliver(struct outgoing* o) {
    size_t limit = xmax - 32 < CHUNK ? xmax - 32 : CHUNK;
    if (o->size <= limit) { x_change_property(o->requestor, o->property, o->type, 8, o->data, (uint32_t)o->size); notify_requestor(o->requestor, ATOM(CLIPBOARD), o->type == XA_STRING ? XA_STRING : o->type, o->property, last_time); out_end(o); return; }
    o->incr = true; x_change_attributes(o->requestor, 1u << 11, M_PROPERTY | (find_client(o->requestor) && find_client(o->requestor)->managed ? M_FOCUS : 0));
    uint32_t size = (uint32_t)o->size; x_change_property(o->requestor, o->property, ATOM(INCR), 32, &size, 1);
    notify_requestor(o->requestor, ATOM(CLIPBOARD), o->type, o->property, last_time); o->waiting = true; o->sent = 0;
    if (o->timer) wl_event_source_timer_update(o->timer, 10000);   /* 10 s for the requestor to take the INCR announcement */
}
static int out_readable(int fd, uint32_t mask, void* data) {
    struct outgoing* o = data; (void)mask;
    for (;;) {
        if (o->size == o->cap) { size_t cap = o->cap ? o->cap * 2 : 4096; if (cap > MAX_TRANSFER) { notify_requestor(o->requestor, ATOM(CLIPBOARD), o->type, 0, last_time); out_end(o); return 0; }
            uint8_t* grown = realloc(o->data, cap); if (!grown) { notify_requestor(o->requestor, ATOM(CLIPBOARD), o->type, 0, last_time); out_end(o); return 0; } o->data = grown; o->cap = cap; }
        ssize_t n = read(fd, o->data + o->size, o->cap - o->size);
        if (n > 0) { o->size += (size_t)n; if (o->timer) wl_event_source_timer_update(o->timer, 10000); continue; }   /* 10 s without progress */
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        break;
    }
    wl_event_source_remove(o->source); o->source = NULL; close(o->fd); o->fd = -1; o->reading = false;
    out_deliver(o); return 0;
}
static int out_timeout(void* data) { struct outgoing* o = data; ODD("CLIPBOARD transfer to X window 0x%x timed out", o->requestor); if (o->reading) notify_requestor(o->requestor, ATOM(CLIPBOARD), o->type, 0, last_time); out_end(o); return 0; }
static void selection_request(const uint8_t* e) {
    uint32_t time = get32(e + 4), owner = get32(e + 8), requestor = get32(e + 12), selection = get32(e + 16), target = get32(e + 20), property = get32(e + 24);
    if (!property) property = target;   /* an obsolete client */
    if (owner != wm_window || selection != ATOM(CLIPBOARD) || !own_clipboard || !requestor) { notify_requestor(requestor, selection, target, 0, time); return; }
    const char* types[32]; int n = aurora_host_selection_types(types, 32);
    if (target == ATOM(TARGETS)) {
        uint32_t list[40]; uint32_t k = 0; list[k++] = ATOM(TARGETS); list[k++] = ATOM(TIMESTAMP);
        bool text = false; for (int i = 0; i < n; i++) if (!strncmp(types[i], "text/plain", 10) || !strcmp(types[i], "UTF8_STRING") || !strcmp(types[i], "STRING") || !strcmp(types[i], "TEXT")) text = true;
        if (text) { list[k++] = ATOM(UTF8_STRING); list[k++] = XA_STRING; list[k++] = ATOM(TEXT); }
        for (int i = 0; i < wl_atom_count && k < 40; i++) if (wl_atoms[i].atom) list[k++] = wl_atoms[i].atom;
        x_change_property(requestor, property, XA_ATOM, 32, list, k); notify_requestor(requestor, selection, target, property, time); return;
    }
    if (target == ATOM(TIMESTAMP)) { uint32_t t = last_time; x_change_property(requestor, property, XA_INTEGER, 32, &t, 1); notify_requestor(requestor, selection, target, property, time); return; }
    const char* mime = NULL;
    if (target == ATOM(UTF8_STRING) || target == XA_STRING || target == ATOM(TEXT)) {
        static const char* const order[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};
        for (unsigned k = 0; k < 5 && !mime; k++) for (int i = 0; i < n; i++) if (!strcmp(types[i], order[k])) { mime = order[k]; break; }
    } else for (int i = 0; i < wl_atom_count; i++) if (wl_atoms[i].atom == target) { mime = wl_atoms[i].mime; break; }
    struct outgoing* o = NULL; for (int i = 0; i < TRANSFERS && !o; i++) if (out[i].fd < 0 && !out[i].incr && !out[i].reading) o = &out[i];
    bool allowed = aurora_host_x11_focused();   /* while X has the keyboard (X itself has no isolation between its clients) */
    if (!mime || !o || !allowed) {
        if (mime && !allowed) ODD("CLIPBOARD request from X window 0x%x without the keyboard focus: refused", requestor);
        notify_requestor(requestor, selection, target, 0, time); return; }
    int p[2]; if (pipe2(p, O_CLOEXEC)) { notify_requestor(requestor, selection, target, 0, time); return; }
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);   /* only Aurora's end: the source writes as it likes */
    *o = (struct outgoing){.fd = p[0], .requestor = requestor, .property = property, .type = target == ATOM(TEXT) ? ATOM(UTF8_STRING) : target, .reading = true};
    o->source = wl_event_loop_add_fd(loop, p[0], WL_EVENT_READABLE, out_readable, o);
    o->timer = wl_event_loop_add_timer(loop, out_timeout, o); if (o->timer) wl_event_source_timer_update(o->timer, 10000);
    if (!o->source || !aurora_host_selection_send(mime, p[1])) { notify_requestor(requestor, selection, target, 0, time); out_end(o); }
}
static void out_property_deleted(uint32_t window, uint32_t property) {
    for (int i = 0; i < TRANSFERS; i++) if (out[i].incr && out[i].waiting && out[i].requestor == window && out[i].property == property) { out[i].waiting = false; out_chunk(&out[i]); return; }
}
static void out_clear(void) { for (int i = 0; i < TRANSFERS; i++) if (out[i].fd >= 0 || out[i].incr || out[i].reading || out[i].data) out_end(&out[i]); }

/* ================================================================================================================
 * 4f. X events
 * ================================================================================================================ */
static void configure_request(const uint8_t* e) {
    uint32_t id = get32(e + 8); uint16_t mask = get16(e + 26);
    int x = (int16_t)get16(e + 16), y = (int16_t)get16(e + 18), cw = clampi(get16(e + 20), 1, 32767), ch = clampi(get16(e + 22), 1, 32767);
    struct aurora_xwin* w = find_client(id);
    if (!w || !w->managed) {   /* not managed (yet): as asked */
        int32_t v[7]; int n = 0;
        if (mask & C_X) v[n++] = x; if (mask & C_Y) v[n++] = y; if (mask & C_W) v[n++] = cw; if (mask & C_H) v[n++] = ch;
        if (mask & C_BORDER) v[n++] = get16(e + 24); if (mask & C_SIBLING) v[n++] = (int32_t)get32(e + 12); if (mask & C_STACK) v[n++] = e[1];
        x_configure(id, mask & 0x7F, v);
        if (w) { if (mask & C_X) w->x = x; if (mask & C_Y) w->y = y; if (mask & C_W) w->w = cw; if (mask & C_H) w->h = ch; }
        return;
    }
    if (w->is_full || w->is_max || (w->flags & AURORA_XCONF_TILED)) { send_configure_notify(w); return; }   /* Aurora decides those */
    int ncw = mask & C_W ? cw : client_w(w), nch = mask & C_H ? ch : client_h(w);
    if (w->max_w && ncw > w->max_w) ncw = w->max_w; if (w->max_h && nch > w->max_h) nch = w->max_h;
    if (w->min_w && ncw < w->min_w) ncw = w->min_w; if (w->min_h && nch < w->min_h) nch = w->min_h;
    int fx = w->fx, fy = w->fy;
    if (mask & C_X) fx = w->gravity == 10 ? x - w->el : x;
    if (mask & C_Y) fy = w->gravity == 10 ? y - w->et : y;
    if (mask & (C_X | C_Y)) { int ax, ay, aw, ah; aurora_host_x11_work_area(fx + 40, fy + 10, &ax, &ay, &aw, &ah);   /* the title bar stays reachable */
        if (fy < ay) fy = ay; if (fy > ay + ah - TITLE_H) fy = ay + ah - TITLE_H; if (fx > ax + aw - 60) fx = ax + aw - 60; }
    int fw = ncw + w->el + w->er, fh = nch + w->et + w->eb; bool sized = fw != w->fw || fh != w->fh, moved = fx != w->fx || fy != w->fy;
    apply_geometry(w, fx, fy, fw, fh, true);
    if (w->host) { if (sized) aurora_host_x11_resized(w->host, w->fw, w->fh); if (moved) aurora_host_x11_moved(w->host, w->fx, w->fy); }
    if ((mask & C_STACK) && e[1] == 0 && !(mask & C_SIBLING) && w->host && focused && (focused == w || find_client(w->transient) == focused))
        aurora_host_x11_request(w->host, AURORA_XREQ_ACTIVATE);   /* a raise: only within the focused application */
    if (sized || moved) say("CONFIGURE REQUEST window=0x%x x=%d y=%d client=%dx%d", w->id, w->fx + w->el, w->fy + w->et, client_w(w), client_h(w));
}
static void client_message(const uint8_t* e) {
    uint32_t window = get32(e + 4), type = get32(e + 8), d0 = get32(e + 12), d1 = get32(e + 16), d2 = get32(e + 20); bool sent = e[0] & 0x80;
    if (e[1] != 32 || !type) return;
    if (type == ATOM(WL_SURFACE_SERIAL) || type == ATOM(WL_SURFACE_ID)) {   /* only Xwayland itself (not SendEvent) may name surfaces */
        if (sent) { ODD("forged %s for window 0x%x ignored", type == ATOM(WL_SURFACE_ID) ? "WL_SURFACE_ID" : "WL_SURFACE_SERIAL", window); return; }
        struct aurora_xwin* w = find(window); if (!w) { ODD("surface message for unknown window 0x%x", window); return; }
        if (type == ATOM(WL_SURFACE_SERIAL)) serial_message(w, (uint64_t)d1 << 32 | d0); else surface_id_message(w, d0);
        return;
    }
    struct aurora_xwin* w = find_client(window);
    if (type == ATOM(_NET_WM_STATE)) {
        if (!w || d0 > 2) return;
        bool full = false, max = false, modal = false;
        for (int i = 0; i < 2; i++) { uint32_t p = i ? d2 : d1;
            if (p == ATOM(_NET_WM_STATE_FULLSCREEN)) full = true; else if (p == ATOM(_NET_WM_STATE_MAXIMIZED_VERT) || p == ATOM(_NET_WM_STATE_MAXIMIZED_HORZ)) max = true;
            else if (p == ATOM(_NET_WM_STATE_MODAL)) modal = true; }
        if (!w->host) {   /* not shown yet: it starts that way */
            if (full) w->want_full = d0 == 1 || (d0 == 2 && !w->want_full); if (max) w->want_max = d0 == 1 || (d0 == 2 && !w->want_max);
            if (modal) w->modal = d0 == 1 || (d0 == 2 && !w->modal); return; }
        if (full) aurora_host_x11_request(w->host, d0 == 1 || (d0 == 2 && !w->is_full) ? AURORA_XREQ_FULLSCREEN : AURORA_XREQ_UNFULLSCREEN);
        if (max) aurora_host_x11_request(w->host, d0 == 1 || (d0 == 2 && !w->is_max) ? AURORA_XREQ_MAXIMIZE : AURORA_XREQ_UNMAXIMIZE);
        if (modal) { w->modal = d0 == 1 || (d0 == 2 && !w->modal); write_net_state(w); push_state(w); }
        say("STATE REQUEST window=0x%x action=%u%s%s%s", window, d0, full ? " fullscreen" : "", max ? " maximized" : "", modal ? " modal" : "");
    }
    else if (type == ATOM(_NET_ACTIVE_WINDOW)) {
        if (!w || !w->host) return;   /* granted to the application that has the focus, or for its own minimized window */
        if (focused_client(window) || w->is_min) aurora_host_x11_request(w->host, AURORA_XREQ_ACTIVATE);
        else ODD("activation of 0x%x refused (focus stealing prevention)", window);
    }
    else if (type == ATOM(WM_CHANGE_STATE)) { if (w && w->host && d0 == 3) aurora_host_x11_request(w->host, AURORA_XREQ_MINIMIZE); }
    else if (type == ATOM(_NET_CLOSE_WINDOW)) { if (w && w->managed) aurora_xwm_close(w); }
    else if (type == ATOM(_NET_WM_MOVERESIZE)) {
        static const uint32_t edges[9] = {5, 1, 9, 8, 10, 2, 6, 4, 0};   /* topleft, top, topright, right, bottomright, bottom, bottomleft, left, move */
        if (w && w->host && d2 <= 8) { if (!aurora_host_x11_begin(w->host, edges[d2])) ODD("move/resize of 0x%x refused (no button held)", window); }
    }
    else if (type == ATOM(_NET_REQUEST_FRAME_EXTENTS)) {
        bool frame = !w || (w->managed ? w->frame && w->et : w->decorated || !w->pending);
        uint32_t v[4] = {frame ? BORDER : 0, frame ? BORDER : 0, frame ? TITLE_H : 0, frame ? BORDER : 0};
        x_change_property(window, ATOM(_NET_FRAME_EXTENTS), XA_CARDINAL, 32, v, 4);
    }
}
static void handle_event(const uint8_t* e) {
    uint8_t code = e[0] & 0x7F; struct aurora_xwin* w;
    switch (code) {
    case 12: { uint32_t id = get32(e + 4); w = find(id); if (w && w->frame == id && get16(e + 16) == 0) draw_frame(w, true); break; }   /* Expose */
    case 16: { uint32_t parent = get32(e + 4), id = get32(e + 8);   /* CreateNotify */
        if (parent == root && id != wm_window && !find(id)) track(id, e[22] != 0, (int16_t)get16(e + 12), (int16_t)get16(e + 14), get16(e + 16), get16(e + 18));
        break; }
    case 17: if ((w = find_client(get32(e + 8)))) forget(w); break;   /* DestroyNotify (frames are not clients) */
    case 18: {   /* UnmapNotify */
        uint32_t event = get32(e + 4); w = find_client(get32(e + 8)); if (!w) break;
        if (w->ignore_unmaps > 0 && !(e[0] & 0x80)) { w->ignore_unmaps--; break; }
        if (w->managed) { if ((e[0] & 0x80) || event == w->frame || event == root) unmanage(w, false); }
        else { w->mapped = false; if (e[0] & 0x80) w->map_wanted = false;   /* withdrawn before Aurora managed it */
            if (w->override || w->popup) { detach(w); w->has_serial = false; w->popup = false; } }
        break; }
    case 19: if ((w = find_client(get32(e + 8)))) { w->mapped = true; if (!w->managed && e[12]) w->override = true; } break;   /* MapNotify */
    case 20: map_request(get32(e + 8)); break;
    case 21: {   /* ReparentNotify */
        uint32_t id = get32(e + 8), parent = get32(e + 12); w = find_client(id);
        if (!w) { if (parent == root && id != wm_window && !find(id)) track(id, e[20] != 0, (int16_t)get16(e + 16), (int16_t)get16(e + 18), 1, 1); break; }
        if (parent != root && parent != w->frame) { say("window 0x%x was embedded elsewhere (0x%x): no longer managed", id, parent); forget(w); }
        break; }
    case 22: {   /* ConfigureNotify of a root child */
        if (get32(e + 4) != root) break; w = find_client(get32(e + 8)); if (!w || w->managed) break;
        w->x = (int16_t)get16(e + 16); w->y = (int16_t)get16(e + 18); w->w = clampi(get16(e + 20), 1, 32767); w->h = clampi(get16(e + 22), 1, 32767);
        if ((w->override || w->popup) && w->host) { aurora_host_x11_moved(w->host, w->x, w->y); aurora_host_x11_resized(w->host, w->w, w->h); }
        break; }
    case 23: configure_request(e); break;
    case 28: {   /* PropertyNotify */
        uint32_t window = get32(e + 4), atom = get32(e + 8); uint8_t state = e[16]; last_time = get32(e + 12);
        if (window == wm_window) { struct incoming* t = in_current(); if (atom == ATOM(_AURORA_DATA) && state == 0 && t && t->incr) in_read(0); break; }
        if (state == 1) out_property_deleted(window, atom);
        w = find_client(window); if (!w || w->pending) break;
        int n; const uint32_t* list = property_atoms(&n);
        for (int i = 0; i < n; i++) if (list[i] == atom) { read_property(w, atom); break; }
        break; }
    case 29: last_time = get32(e + 4); if (get32(e + 8) == wm_window && get32(e + 12) == ATOM(CLIPBOARD)) own_clipboard = false; break;   /* SelectionClear */
    case 30: if (get32(e + 4)) last_time = get32(e + 4); selection_request(e); break;
    case 31: selection_notify(e); break;
    case 33: client_message(e); break;
    default:
        if (xfixes_event && code == xfixes_event) { last_time = get32(e + 16); if (get32(e + 12) == ATOM(CLIPBOARD)) selection_owner_changed(get32(e + 8)); }
        break;
    }
}
static uint32_t redirect_seq;
static void handle_error(const uint8_t* e) {
    uint16_t seq16 = get16(e + 2);
    if (redirect_seq && seq16 == (uint16_t)redirect_seq && xstate != XS_READY) { say("another X11 window manager runs on :%d", display_number); x_break("not the window manager"); return; }
    x_answer(seq16, NULL, 0);
    if (e[1] != 3 && e[1] != 9) ODD("X error code=%u major=%u minor=%u value=0x%x", e[1], e[10], get16(e + 8), get32(e + 4));   /* BadWindow/BadDrawable: windows that went */
}

/* ================================================================================================================
 * 3b. Connecting: setup, atoms, extensions, the root
 * ================================================================================================================ */
static void finish_init(void);
static void init_step(void) { if (init_pending > 0 && --init_pending == 0) finish_init(); }
static void atom_reply(const uint8_t* r, size_t size, uint32_t index, uint32_t unused) {
    (void)unused; if (r && size >= 12 && index < A_COUNT) atoms[index] = get32(r + 8); else if (index < A_COUNT) say("atom %s not interned", atom_names[index]);
    init_step();
}
static void extension_reply(const uint8_t* r, size_t size, uint32_t which, uint32_t unused) {
    (void)unused; bool present = r && size >= 12 && r[8];
    if (which == 0) composite_major = present ? r[9] : 0; else { xfixes_major = present ? r[9] : 0; xfixes_event = present ? r[10] : 0; }
    init_step();
}
static void ignore_reply(const uint8_t* r, size_t size, uint32_t a, uint32_t b) { (void)r; (void)size; (void)a; (void)b; }
static void adopt_geometry(const uint8_t* r, size_t size, uint32_t id, uint32_t unused) {   /* windows that existed before Aurora connected */
    (void)unused; struct aurora_xwin* w = find_client(id); if (!w || !r || size < 24) return;
    w->x = (int16_t)get16(r + 12); w->y = (int16_t)get16(r + 14); w->w = clampi(get16(r + 16), 1, 32767); w->h = clampi(get16(r + 18), 1, 32767);
    if (w->mapped && !w->override) map_request(id);   /* managed now (a framed one is unmapped once by the reparent) */
}
static void adopt_attributes(const uint8_t* r, size_t size, uint32_t id, uint32_t unused) {
    (void)unused; if (!r || size < 32 || get16(r + 12) == 2 /* InputOnly */) return;
    struct aurora_xwin* w = track(id, r[27] != 0, 0, 0, 1, 1); if (w) w->mapped = r[26] == 2;
}
static void tree_reply(const uint8_t* r, size_t size, uint32_t a, uint32_t b) {
    (void)a; (void)b; if (!r || size < 32) return;
    size_t n = get16(r + 16); if (32 + 4 * n > size) return;
    for (size_t i = 0; i < n && i < MAX_WINDOWS; i++) { uint32_t id = get32(r + 32 + 4 * i); if (id == wm_window || find(id)) continue;
        uint8_t* p = x_reserve(8); if (p) { p[0] = 3; put32(p + 4, id); x_call(8, adopt_attributes, id, 0); }
        p = x_reserve(8); if (p) { p[0] = 14; put32(p + 4, id); x_call(8, adopt_geometry, id, 0); } }
}
static void finish_init(void) {
    if (xbroken) return;
    if (!composite_major) { say("Xwayland has no Composite extension: X windows cannot be managed"); x_break("no Composite"); return; }
    uint8_t* p = x_reserve(12); if (p) { p[0] = composite_major; p[1] = 0; put32(p + 4, 0); put32(p + 8, 4); x_call(12, ignore_reply, 0, 0); }   /* QueryVersion 0.4 */
    p = x_reserve(12); if (p) { p[0] = composite_major; p[1] = 2; put32(p + 4, root); p[8] = 1; x_commit(12); }                       /* RedirectSubwindows(root, Manual) */
    if (xfixes_major) {
        p = x_reserve(12); if (p) { p[0] = xfixes_major; p[1] = 0; put32(p + 4, 5); put32(p + 8, 0); x_call(12, ignore_reply, 0, 0); }   /* QueryVersion 5.0 */
        p = x_reserve(16); if (p) { p[0] = xfixes_major; p[1] = 2; put32(p + 4, wm_window); put32(p + 8, ATOM(CLIPBOARD)); put32(p + 12, 7); x_commit(16); }   /* SelectSelectionInput */
    } else say("no XFIXES: the clipboard is not shared with X programs");
    x_change_property(root, ATOM(_NET_SUPPORTING_WM_CHECK), XA_WINDOW, 32, &wm_window, 1);
    x_change_property(wm_window, ATOM(_NET_SUPPORTING_WM_CHECK), XA_WINDOW, 32, &wm_window, 1);
    x_change_property(wm_window, ATOM(_NET_WM_NAME), ATOM(UTF8_STRING), 8, "Aurora", 6);
    static const int supported_ids[] = {A__NET_SUPPORTED, A__NET_SUPPORTING_WM_CHECK, A__NET_ACTIVE_WINDOW, A__NET_CLIENT_LIST, A__NET_CLIENT_LIST_STACKING,
        A__NET_WM_NAME, A__NET_WM_PID, A__NET_WM_STATE, A__NET_WM_STATE_FULLSCREEN, A__NET_WM_STATE_MAXIMIZED_VERT, A__NET_WM_STATE_MAXIMIZED_HORZ,
        A__NET_WM_STATE_HIDDEN, A__NET_WM_STATE_FOCUSED, A__NET_WM_STATE_MODAL, A__NET_WM_WINDOW_TYPE, A__NET_WM_WINDOW_TYPE_NORMAL,
        A__NET_WM_WINDOW_TYPE_DIALOG, A__NET_WM_WINDOW_TYPE_UTILITY, A__NET_WM_WINDOW_TYPE_SPLASH, A__NET_WM_WINDOW_TYPE_MENU,
        A__NET_WM_WINDOW_TYPE_DROPDOWN_MENU, A__NET_WM_WINDOW_TYPE_POPUP_MENU, A__NET_WM_WINDOW_TYPE_TOOLTIP, A__NET_WM_WINDOW_TYPE_NOTIFICATION,
        A__NET_WM_WINDOW_TYPE_COMBO, A__NET_WM_WINDOW_TYPE_DND, A__NET_WM_MOVERESIZE, A__NET_CLOSE_WINDOW, A__NET_FRAME_EXTENTS,
        A__NET_REQUEST_FRAME_EXTENTS, A__NET_WM_ALLOWED_ACTIONS, A__NET_WM_ACTION_MOVE, A__NET_WM_ACTION_RESIZE, A__NET_WM_ACTION_MINIMIZE,
        A__NET_WM_ACTION_MAXIMIZE_HORZ, A__NET_WM_ACTION_MAXIMIZE_VERT, A__NET_WM_ACTION_FULLSCREEN, A__NET_WM_ACTION_CLOSE, A__NET_WM_ICON};
    uint32_t supported[sizeof(supported_ids) / sizeof(*supported_ids)];
    for (unsigned i = 0; i < sizeof(supported_ids) / sizeof(*supported_ids); i++) supported[i] = atoms[supported_ids[i]];
    x_change_property(root, ATOM(_NET_SUPPORTED), XA_ATOM, 32, supported, (uint32_t)(sizeof(supported) / sizeof(*supported)));
    uint32_t none = 0; x_change_property(root, ATOM(_NET_ACTIVE_WINDOW), XA_WINDOW, 32, &none, 1); client_list();
    x_set_selection_owner(wm_window, ATOM(WM_S0), 0); x_set_selection_owner(wm_window, ATOM(_NET_WM_CM_S0), 0);
    xstate = XS_READY;
    p = x_reserve(8); if (p) { p[0] = 15; put32(p + 4, root); x_call(8, tree_reply, 0, 0); }   /* QueryTree: windows mapped before now */
    say("WINDOW MANAGER READY display=:%d root=%ux%u depth=%u decorations=%s", display_number, root_w, root_h, root_depth, images_ok ? "image" : "solid");
}
static void setup_reply(const uint8_t* m, size_t size) {
    if (m[0] != 1) { say("Xwayland refused the window manager connection: %.*s", (size_t)m[1] <= size - 8 ? (int)m[1] : 0, (const char*)m + 8); x_break("setup refused"); return; }
    const uint8_t* d = m + 8; size_t n = size - 8;
    if (n < 32) { x_break("short setup"); return; }
    id_base = get32(d + 4); id_mask = get32(d + 8); uint16_t vendor = get16(d + 16); xmax = (size_t)get16(d + 18) * 4; if (xmax < 4096) xmax = 4096;
    uint8_t screens = d[20], formats = d[21]; lsb_server = d[22] == 0;
    size_t at = 32 + ((vendor + 3u) & ~3u), screen = at + 8u * formats;
    if (!screens || screen + 40 > n || !id_mask) { x_break("bad setup"); return; }
    const uint8_t* s = d + screen; root = get32(s); root_w = get16(s + 20); root_h = get16(s + 22); root_visual = get32(s + 32); root_depth = s[38];
    for (unsigned i = 0; i < formats; i++) if (d[at + 8 * i] == root_depth) image_bpp = d[at + 8 * i + 1];
    images_ok = lsb_server && image_bpp == 32 && root_depth == 24;
    xstate = XS_INIT; id_next = 0;
    /* the root first: SubstructureRedirect makes Aurora the window manager (BadAccess if another one is) */
    uint8_t* p = x_reserve(16); if (p) { p[0] = 2; put32(p + 4, root); put32(p + 8, 1u << 11); put32(p + 12, M_REDIRECT | M_SUBSTRUCTURE | M_PROPERTY); redirect_seq = x_commit(16); }
    init_pending = 1;
    for (int i = 0; i < A_COUNT; i++) { size_t len = strlen(atom_names[i]), sz = 8 + ((len + 3) & ~(size_t)3);
        if (!(p = x_reserve(sz))) break; p[0] = 16; put16(p + 4, (uint16_t)len); memcpy(p + 8, atom_names[i], len); init_pending++; x_call(sz, atom_reply, (uint32_t)i, 0); }
    static const char* const extensions[2] = {"Composite", "XFIXES"};
    for (uint32_t i = 0; i < 2; i++) { size_t len = strlen(extensions[i]), sz = 8 + ((len + 3) & ~(size_t)3);
        if (!(p = x_reserve(sz))) break; p[0] = 98; put16(p + 4, (uint16_t)len); memcpy(p + 8, extensions[i], len); init_pending++; x_call(sz, extension_reply, i, 0); }
    wm_window = x_id(); gc = x_id();
    if ((p = x_reserve(40))) { p[0] = 1; put32(p + 4, wm_window); put32(p + 8, root); put16(p + 12, (uint16_t)-100); put16(p + 14, (uint16_t)-100); put16(p + 16, 1); put16(p + 18, 1);
        put16(p + 22, 1); put32(p + 28, (1u << 9) | (1u << 11)); put32(p + 32, 1); put32(p + 36, M_PROPERTY); x_commit(40); }
    if ((p = x_reserve(16))) { p[0] = 55; put32(p + 4, gc); put32(p + 8, root); put32(p + 12, 0); x_commit(16); }
    init_step();   /* the 1 above: finish once every reply is in */
}
static void x_input(void) {
    size_t at = 0;
    while (!xbroken && xfd >= 0) {
        size_t left = ilen - at; const uint8_t* m = ibuf + at;
        if (xstate == XS_SETUP) { if (left < 8) break; size_t need = 8 + 4 * (size_t)get16(m + 6); if (left < need) break; setup_reply(m, need); at += need; continue; }
        if (left < 32) break;
        size_t need = 32; if (m[0] == 1 || (m[0] & 0x7F) == 35) need += 4 * (size_t)get32(m + 4);
        if (need > (4u << 20)) { x_break("oversized message"); break; }
        if (left < need) break;
        if (m[0] == 0) handle_error(m); else if (m[0] == 1) x_answer(get16(m + 2), m, need); else handle_event(m);
        at += need;
    }
    if (at && at <= ilen) { memmove(ibuf, ibuf + at, ilen - at); ilen -= at; }
}
static int x_ready(int fd, uint32_t mask, void* data) {
    (void)data;
    if (mask & WL_EVENT_WRITABLE) x_flush();
    if (mask & WL_EVENT_READABLE) for (;;) {
        if (icap - ilen < 65536) { size_t cap = icap ? icap * 2 : 131072; if (cap > (8u << 20)) { x_break("input overflow"); return 0; }
            uint8_t* grown = realloc(ibuf, cap); if (!grown) { x_break("out of memory"); return 0; } ibuf = grown; icap = cap; }
        ssize_t n = recv(fd, ibuf + ilen, icap - ilen, MSG_DONTWAIT);
        if (n > 0) { ilen += (size_t)n; x_input(); if (xbroken) return 0; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        x_break(n ? strerror(errno) : "Xwayland closed the connection"); return 0;
    }
    else if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) x_break("hangup");
    return 0;
}
static void x_connect(int fd) {
    xfd = fd; xbroken = false; xstate = XS_SETUP; xseq = 0; xmax = 16384; ostart = oend = 0; ilen = 0; c_head = c_count = 0; id_next = 0;
    composite_major = xfixes_major = xfixes_event = 0; redirect_seq = 0; memset(atoms, 0, sizeof(atoms)); focused = NULL;
    uint8_t* p = x_reserve(48);
    if (p) { p[0] = 'l'; put16(p + 2, 11); put16(p + 4, 0); put16(p + 6, 18); put16(p + 8, 16); memcpy(p + 12, "MIT-MAGIC-COOKIE-1", 18); memcpy(p + 32, secret, 16); oend += 48; }
    xsource = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE | WL_EVENT_WRITABLE, x_ready, NULL);
    if (!xsource) x_break("event source");
}
static void teardown(const char* why) {
    if (xfd < 0 && xstate == XS_DOWN) return;
    say("window manager stopped (%s)", why);
    in_clear(); out_clear();
    if (sel_from_x) { sel_from_x = false; aurora_host_x11_selection(NULL, 0); }
    sel_owner = 0; own_clipboard = false; last_selection = NULL; last_from_x = true; wl_atom_count = 0; sel_target_count = 0; sel_names_pending = 0; sel_generation++;
    struct aurora_xwin* w, *t; wl_list_for_each_safe(w, t, &xwins, link) { detach(w); w->gone = true; wl_list_remove(&w->link); free(w->icon); free(w); }
    xwin_count = 0; focused = NULL; free(stack_last); stack_last = NULL; stack_last_n = 0;
    c_head = c_count = 0;
    if (xsource) wl_event_source_remove(xsource); xsource = NULL;
    if (xfd >= 0) close(xfd); xfd = -1; ostart = oend = 0; ilen = 0; xstate = XS_DOWN; xbroken = false;
    if (server && server_pid > 0) { say("Xwayland pid=%d stopped: its window manager is gone", server_pid); kill(server_pid, SIGTERM); }
}

/* ================================================================================================================
 * 1. The X server process
 * ================================================================================================================ */
static int listen_fd[2] = {-1, -1}; static struct wl_event_source* listen_source[2];
static int ready_fd = -1; static struct wl_event_source* ready_source, *restart_timer;
static struct wl_listener server_listener; static uint64_t started_ms; static unsigned starts;
static void spawn(void);
static int listen_ready(int fd, uint32_t mask, void* data);
static void listen_watch(bool on) {
    for (int i = 0; i < 2; i++) {
        if (on && !listen_source[i] && listen_fd[i] >= 0) listen_source[i] = wl_event_loop_add_fd(loop, listen_fd[i], WL_EVENT_READABLE, listen_ready, NULL);
        if (!on && listen_source[i]) { wl_event_source_remove(listen_source[i]); listen_source[i] = NULL; }
    }
}
static int restart_due(void* data) { (void)data; if (restart_timer) { wl_event_source_remove(restart_timer); restart_timer = NULL; } spawn(); return 0; }
static int listen_ready(int fd, uint32_t mask, void* data) {   /* an X client connects: Xwayland starts (and accepts it) */
    (void)fd; (void)mask; (void)data;
    if (server || server_pid > 0) { listen_watch(false); return 0; }
    uint64_t now = now_ms();
    if (started_ms && now - started_ms < 3000) {   /* it just exited: not more often than every 3 s */
        listen_watch(false);
        if (!restart_timer && (restart_timer = wl_event_loop_add_timer(loop, restart_due, NULL))) wl_event_source_timer_update(restart_timer, (int)(3000 - (now - started_ms)));
        return 0; }
    spawn(); return 0;
}
static void server_destroyed(struct wl_listener* l, void* data) {
    (void)l; (void)data; server = NULL; say("Xwayland's Wayland connection closed");
    teardown("Xwayland left");
    if (ready_source) { wl_event_source_remove(ready_source); ready_source = NULL; } if (ready_fd >= 0) { close(ready_fd); ready_fd = -1; }
    if (!restart_timer) listen_watch(true);
}
static int server_ready(int fd, uint32_t mask, void* data) {
    (void)mask; (void)data; char text[16] = {0}; ssize_t n = read(fd, text, sizeof(text) - 1);
    if (n > 0 && memchr(text, '\n', (size_t)n)) say("READY Xwayland pid=%d display=:%d", server_pid, atoi(text));
    if (n > 0 && !memchr(text, '\n', (size_t)n)) return 0;
    if (ready_source) { wl_event_source_remove(ready_source); ready_source = NULL; } close(fd); ready_fd = -1; return 0;
}
static void spawn(void) {
    if (server || server_pid > 0 || display_number < 0 || !enabled) return;
    int wl[2] = {-1, -1}, wm[2] = {-1, -1}, ready[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl) || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) || pipe2(ready, O_CLOEXEC)) {
        say("cannot start Xwayland: %s", strerror(errno)); int all[6] = {wl[0], wl[1], wm[0], wm[1], ready[0], ready[1]}; for (int i = 0; i < 6; i++) if (all[i] >= 0) close(all[i]); return; }
    char name[16]; snprintf(name, sizeof(name), ":%d", display_number);
    pid_t pid = fork();
    if (pid == 0) {
        int keep[5] = {wl[1], wm[1], ready[1], listen_fd[0], listen_fd[1]}, high[5];
        for (int i = 0; i < 5; i++) high[i] = keep[i] >= 0 ? fcntl(keep[i], F_DUPFD, 40) : -1;
        for (int i = 0; i < 5; i++) if (high[i] >= 0) { if (dup2(high[i], 3 + i) < 0) _exit(127); close(high[i]); }
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL); signal(SIGPIPE, SIG_DFL);
        setenv("WAYLAND_SOCKET", "3", 1); unsetenv("WAYLAND_DISPLAY"); unsetenv("DISPLAY");
        char* argv[24]; int n = 0;
        argv[n++] = "Xwayland"; argv[n++] = name; argv[n++] = "-rootless"; argv[n++] = "-wm"; argv[n++] = "4"; argv[n++] = "-displayfd"; argv[n++] = "5";
        argv[n++] = "-listenfd"; argv[n++] = "6"; if (high[4] >= 0) { argv[n++] = "-listenfd"; argv[n++] = "7"; }
        argv[n++] = "-auth"; argv[n++] = auth_path; argv[n++] = "-noreset"; argv[n++] = "-shm"; argv[n++] = "-dumbSched"; argv[n++] = "-nolisten"; argv[n++] = "tcp"; argv[n] = NULL;
        execv(XWAYLAND, argv); _exit(127);
    }
    close(wl[1]); close(wm[1]); close(ready[1]);
    if (pid < 0) { say("cannot start Xwayland: %s", strerror(errno)); close(wl[0]); close(wm[0]); close(ready[0]); return; }
    if (!(server = wl_client_create(display, wl[0]))) { say("cannot start Xwayland: no Wayland client"); close(wl[0]); close(wm[0]); close(ready[0]); kill(pid, SIGKILL); return; }
    server_listener.notify = server_destroyed; wl_client_add_destroy_listener(server, &server_listener);
    server_pid = pid; starts++; started_ms = now_ms();
    fcntl(ready[0], F_SETFL, O_NONBLOCK); ready_fd = ready[0]; ready_source = wl_event_loop_add_fd(loop, ready_fd, WL_EVENT_READABLE, server_ready, NULL);
    fcntl(wm[0], F_SETFL, O_NONBLOCK); x_connect(wm[0]);
    listen_watch(false);
    say("STARTED Xwayland pid=%d display=:%d start=%u", pid, display_number, starts);
}
bool aurora_xwm_reaped(pid_t pid, int status) {
    if (pid <= 0 || pid != server_pid) return false;
    say("Xwayland exited pid=%d status=%d", pid, status); server_pid = 0;
    if (server) wl_client_destroy(server);   /* server_destroyed: the window manager stops, the sockets wait for the next client */
    return true;
}
/* The display: /tmp/.X11-unix/XN (+ abstract @/tmp/.X11-unix/XN) and /tmp/.XN-lock, N = 0 or 2..32. */
static bool open_display(void) {
    struct stat st; if (mkdir("/tmp/.X11-unix", 01777) && errno != EEXIST) { say("cannot create /tmp/.X11-unix: %s", strerror(errno)); return false; }
    if (!stat("/tmp/.X11-unix", &st) && st.st_uid == getuid()) chmod("/tmp/.X11-unix", 01777);
    for (int n = 0; n <= 32; n++) {
        if (n == 1) continue;   /* the rootful session's (nexus-x11-session) */
        char lock[40], path[108]; snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", n); snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", n);
        int fd = open(lock, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0444);
        if (fd < 0 && errno == EEXIST) {   /* stale when its process is gone */
            char text[16] = {0}; int old = open(lock, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); ssize_t got = old >= 0 ? read(old, text, sizeof(text) - 1) : -1; if (old >= 0) close(old);
            long pid = got > 0 ? strtol(text, NULL, 10) : 0;
            if (pid > 0 && pid != getpid() && (kill((pid_t)pid, 0) == 0 || errno == EPERM)) continue;
            unlink(lock); unlink(path); fd = open(lock, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0444);
        }
        if (fd < 0) continue;
        char text[16]; int len = snprintf(text, sizeof(text), "%10d\n", (int)getpid()); bool written = write(fd, text, (size_t)len) == len; close(fd);
        if (!written) { unlink(lock); continue; }
        int s[2] = {socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        struct sockaddr_un a = {.sun_family = AF_UNIX}, b = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", path); memcpy(b.sun_path + 1, path, strlen(path));
        socklen_t blen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(path));
        unlink(path);
        bool ok = s[0] >= 0 && !bind(s[0], (struct sockaddr*)&a, sizeof(a)) && !listen(s[0], 128);
        bool abstract = ok && s[1] >= 0 && !bind(s[1], (struct sockaddr*)&b, blen) && !listen(s[1], 128);
        if (!ok || (s[1] >= 0 && !abstract && errno == EADDRINUSE)) {   /* another server has this number */
            if (s[0] >= 0) close(s[0]); if (s[1] >= 0) close(s[1]); unlink(path); unlink(lock); continue; }
        if (!abstract && s[1] >= 0) { close(s[1]); s[1] = -1; }
        chmod(path, 0777);
        listen_fd[0] = s[0]; listen_fd[1] = s[1]; display_number = n; return true;
    }
    say("no free X display number"); return false;
}
static bool write_auth(void) {
    const char* runtime = getenv("XDG_RUNTIME_DIR"); if (!runtime || runtime[0] != '/') runtime = "/run/user/1000";
    snprintf(auth_path, sizeof(auth_path), "%s/aurora-xauth-%d", runtime, display_number);
    if (getrandom(secret, sizeof(secret), 0) != (ssize_t)sizeof(secret)) { say("no random cookie: %s", strerror(errno)); return false; }
    char number[8]; int digits = snprintf(number, sizeof(number), "%d", display_number); uint8_t entry[64]; size_t n = 0;
    entry[n++] = 0xFF; entry[n++] = 0xFF; entry[n++] = 0; entry[n++] = 0;   /* FamilyWild, no address */
    entry[n++] = 0; entry[n++] = (uint8_t)digits; memcpy(entry + n, number, (size_t)digits); n += (size_t)digits;
    entry[n++] = 0; entry[n++] = 18; memcpy(entry + n, "MIT-MAGIC-COOKIE-1", 18); n += 18;
    entry[n++] = 0; entry[n++] = 16; memcpy(entry + n, secret, 16); n += 16;
    unlink(auth_path); int fd = open(auth_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool ok = fd >= 0 && write(fd, entry, n) == (ssize_t)n; if (fd >= 0 && close(fd)) ok = false;
    if (!ok) { say("cannot write %s: %s", auth_path, strerror(errno)); return false; }
    char env[192], tmp[160], dest[160]; int len = snprintf(env, sizeof(env), "DISPLAY=:%d\nXAUTHORITY=%s\n", display_number, auth_path);
    snprintf(tmp, sizeof(tmp), "%s/.aurora-x11.env.tmp", runtime); snprintf(dest, sizeof(dest), "%s/aurora-x11.env", runtime);
    unlink(tmp); fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd >= 0) { bool done = write(fd, env, (size_t)len) == len; close(fd); if (done) rename(tmp, dest); else unlink(tmp); }
    return true;
}
static void read_config(void) {
    const char* home = getenv("HOME"); char path[256]; snprintf(path, sizeof(path), "%s/.aurora/x11.conf", home && home[0] == '/' && strcmp(home, "/") ? home : "/home/nexus");
    FILE* f = fopen(path, "re"); if (!f) return; char line[128];
    while (fgets(line, sizeof(line), f)) { if (!strncmp(line, "export=", 7)) export_display = line[7] == '1'; else if (!strncmp(line, "rootless=", 9)) enabled = line[9] != '0';
        else if (!strncmp(line, "shell=", 6)) use_shell = line[6] != '0'; }   /* shell=0: WL_SURFACE_ID as with Xwayland before 23.1 */
    fclose(f);
}
static void start_idle(void* data) {   /* after the session is set up (XDG_RUNTIME_DIR) */
    (void)data;
    if (!enabled) { say("rootless X11 off (x11.conf rootless=0)"); return; }
    if (access(XWAYLAND, X_OK)) { say("%s is not installed: no rootless X11", XWAYLAND); return; }
    if (!open_display()) return;
    if (!write_auth()) { close(listen_fd[0]); if (listen_fd[1] >= 0) close(listen_fd[1]); listen_fd[0] = listen_fd[1] = -1; display_number = -1; return; }
    if (export_display) aurora_xwm_child_env();
    listen_watch(true);
    say("DISPLAY=:%d waits for X clients (Xwayland starts on the first connection; XAUTHORITY=%s%s)", display_number, auth_path, export_display ? ", exported" : "");
}
void aurora_xwm_child_env(void) {
    if (display_number < 0) return;
    char name[16]; snprintf(name, sizeof(name), ":%d", display_number); setenv("DISPLAY", name, 1); setenv("XAUTHORITY", auth_path, 1);
}
static bool control_x11(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body;
    if (display_number < 0) { snprintf(error, size, "rootless X11 is off"); return false; }
    aurora_control_print(reply, "display=:%d state=%s pid=%d starts=%u xauthority=%s windows=%u\n", display_number,
                         xstate == XS_READY ? "running" : server ? "starting" : "idle", (int)server_pid, starts, auth_path, xwin_count);
    struct aurora_xwin* w;
    wl_list_for_each(w, &xwins, link) if (w->managed || ((w->override || w->popup) && w->mapped))
        aurora_control_print(reply, "xwindow id=0x%x frame=0x%x kind=%s app=%s x=%d y=%d w=%d h=%d client=%dx%d focused=%d maximized=%d fullscreen=%d minimized=%d "
                             "transient=0x%x modal=%d surface=%d net_wm_pid=%u icon=%dx%d title=%s\n", w->id, w->frame, w->managed ? "managed" : "override", w->app_id,
                             w->managed ? w->fx : w->x, w->managed ? w->fy : w->y, w->managed ? w->fw : w->w, w->managed ? w->fh : w->h,
                             w->managed ? client_w(w) : w->w, w->managed ? client_h(w) : w->h, focused == w, w->is_max, w->is_full, w->is_min,
                             w->transient, w->modal, w->surface != NULL, w->pid, w->icon_w, w->icon_h, title_of(w));
    return true;
}
bool aurora_xwm_init(struct wl_display* d) {
    display = d; loop = wl_display_get_event_loop(d); wl_list_init(&xwins); wl_list_init(&xsurfs);
    for (int i = 0; i < TRANSFERS; i++) { in_queue[i].fd = -1; out[i].fd = -1; }
    read_config();
    if (!enabled) return true;
    if (use_shell) shell_global = wl_global_create(d, &xwayland_shell_v1_interface, 1, NULL, shell_bind);   /* Xwayland 23.1+: serials instead of WL_SURFACE_ID */
    if (!shell_global) say("no xwayland_shell_v1 global: WL_SURFACE_ID only");
    aurora_control_register("x11", false, control_x11);
    return wl_event_loop_add_idle(loop, start_idle, NULL) != NULL;
}
