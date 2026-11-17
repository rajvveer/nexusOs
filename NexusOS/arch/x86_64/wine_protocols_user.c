/* Aurora protocol test for the globals Wine's winewayland.drv (and Xwayland) need beyond xdg-shell:
 * wp_viewporter, zwp_pointer_constraints_v1, zwp_relative_pointer_manager_v1, zxdg_output_manager_v1,
 * zxdg_decoration_manager_v1, client cursor surfaces, wheel (axis_discrete) and keyboard repeat info.
 * Composition is checked in /dev/fb0 (the window is found by a marker block drawn through the viewport).
 *
 * Automatic part, then interactive phases driven by host PS/2 input (tests/test_aurora_wine_protocols.py):
 *   POINTER READY   move the pointer to the window centre (ORIGIN + 320,240): the client locks it (LOCKED);
 *                   then send relative motion totalling +70,+65 -> RELATIVE ... motions=0, HINT PASSED.
 *   CONFINED        move +300,+300 -> CONFINE MAX PASSED (stays at 249,149); move -400,-400 -> CONFINE MIN PASSED.
 *   CURSOR ...      automatic (client cursor drawn, hidden, stale serial ignored).
 *   WHEEL READY     one wheel-down notch -> WHEEL PASSED; press/release 'a' -> KEY PASSED; then PASSED. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "[64] WPTEST FAIL line=%d: %s\n", __LINE__, #x); fflush(stderr); exit(1); } } while (0)
#define MARKER 0x13579Bu
#define BORDER 0xFFFF00u
#define RED 0xFF0000u
#define GREEN 0x00C000u
#define BLUE 0x0000FFu
#define GRAY 0x808080u
#define ORANGE 0xFF8000u
#define PURPLE 0x8000FFu
#define MAGENTA 0xFF00FFu

struct conn {
    struct wl_display* display; struct wl_registry* registry;
    struct wl_compositor* compositor; struct wl_subcompositor* subcompositor; struct wl_shm* shm; struct xdg_wm_base* shell;
    struct wl_seat* seat; struct wl_output* output; struct wp_viewporter* viewporter; struct zwp_pointer_constraints_v1* constraints;
    struct zwp_relative_pointer_manager_v1* relative; struct zxdg_output_manager_v1* xdg_output; struct zxdg_decoration_manager_v1* decoration;
    unsigned globals;
};
static struct conn c;
static struct wl_surface *surface, *child, *cursor;
static bool mapped, recommit;   /* every configure is acked at once; the next dispatch commits */
static void mark(const char* format, ...) {
    va_list list; va_start(list, format); printf("[64] WPTEST "); vprintf(format, list); printf("\n"); va_end(list); fflush(stdout);
}
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* ---- registry ---- */
static void global(void* data, struct wl_registry* r, uint32_t id, const char* name, uint32_t version) {
    struct conn* k = data; k->globals++;
    if (k == &c) mark("GLOBAL %s %u", name, version);
    if (!strcmp(name, "wl_compositor")) k->compositor = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(name, "wl_subcompositor")) k->subcompositor = wl_registry_bind(r, id, &wl_subcompositor_interface, 1);
    else if (!strcmp(name, "wl_shm")) k->shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(name, "xdg_wm_base")) k->shell = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
    else if (!strcmp(name, "wl_seat")) { CHECK(version >= 5); k->seat = wl_registry_bind(r, id, &wl_seat_interface, 5); }
    else if (!strcmp(name, "wl_output") && !k->output) { CHECK(version >= 2); k->output = wl_registry_bind(r, id, &wl_output_interface, 2); }
    else if (!strcmp(name, "wp_viewporter")) k->viewporter = wl_registry_bind(r, id, &wp_viewporter_interface, 1);
    else if (!strcmp(name, "zwp_pointer_constraints_v1")) k->constraints = wl_registry_bind(r, id, &zwp_pointer_constraints_v1_interface, 1);
    else if (!strcmp(name, "zwp_relative_pointer_manager_v1")) k->relative = wl_registry_bind(r, id, &zwp_relative_pointer_manager_v1_interface, 1);
    else if (!strcmp(name, "zxdg_output_manager_v1")) { CHECK(version >= 3); k->xdg_output = wl_registry_bind(r, id, &zxdg_output_manager_v1_interface, 3); }
    else if (!strcmp(name, "zxdg_decoration_manager_v1")) k->decoration = wl_registry_bind(r, id, &zxdg_decoration_manager_v1_interface, 1);
}
static void global_remove(void* d, struct wl_registry* r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void ping(void* d, struct xdg_wm_base* shell, uint32_t serial) { (void)d; xdg_wm_base_pong(shell, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void connect_to(struct conn* k) {
    memset(k, 0, sizeof(*k)); k->display = wl_display_connect(NULL); CHECK(k->display);
    k->registry = wl_display_get_registry(k->display); wl_registry_add_listener(k->registry, &registry_listener, k);
    CHECK(wl_display_roundtrip(k->display) >= 0);
    CHECK(k->compositor && k->subcompositor && k->shm && k->shell && k->viewporter && k->constraints && k->relative && k->xdg_output && k->decoration && k->output);
    xdg_wm_base_add_listener(k->shell, &shell_listener, NULL);
}

/* ---- shm buffers ---- */
struct buffer { struct wl_buffer* buffer; uint32_t* pixels; int w, h; };
static struct buffer make_buffer(struct conn* k, int w, int h, uint32_t format) {
    struct buffer b = {0, 0, w, h}; size_t size = (size_t)w * h * 4;
    int fd = memfd_create("wptest", MFD_CLOEXEC); CHECK(fd >= 0 && !ftruncate(fd, (off_t)size));
    b.pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(b.pixels != MAP_FAILED);
    struct wl_shm_pool* pool = wl_shm_create_pool(k->shm, fd, (int32_t)size); CHECK(pool);
    b.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, format); CHECK(b.buffer);
    wl_shm_pool_destroy(pool); close(fd); return b;
}
static void fill(struct buffer* b, int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) b->pixels[(size_t)j * b->w + i] = color;
}

/* ---- framebuffer checks ---- */
static volatile uint32_t* fb; static unsigned fb_w, fb_h, fb_stride;
static void open_fb(void) {
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    struct fb_fix_screeninfo fix; struct fb_var_screeninfo var;
    CHECK(!ioctl(fd, FBIOGET_FSCREENINFO, &fix) && !ioctl(fd, FBIOGET_VSCREENINFO, &var) && var.bits_per_pixel == 32);
    fb = mmap(NULL, fix.smem_len, PROT_READ, MAP_SHARED, fd, 0); CHECK(fb != MAP_FAILED); close(fd);
    fb_w = var.xres; fb_h = var.yres; fb_stride = fix.line_length / 4;
}
static uint32_t pixel(int x, int y) { return x < 0 || y < 0 || (unsigned)x >= fb_w || (unsigned)y >= fb_h ? 0xFFFFFFFFu : fb[(size_t)y * fb_stride + x] & 0xFFFFFF; }
static bool area_is(int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) if (pixel(i, j) != color) return false;
    return true;
}
static void dispatch_for(int ms) {   /* process events for up to ms milliseconds */
    uint64_t end = now_ms() + (uint64_t)ms;
    for (;;) {
        CHECK(wl_display_dispatch_pending(c.display) >= 0);
        if (recommit) { recommit = false; wl_surface_commit(surface); }
        CHECK(wl_display_flush(c.display) >= 0);
        int64_t left = (int64_t)(end - now_ms()); if (left <= 0) return;
        struct pollfd p = {wl_display_get_fd(c.display), POLLIN, 0};
        if (poll(&p, 1, left > 50 ? 50 : (int)left) > 0) CHECK(wl_display_dispatch(c.display) >= 0);
    }
}
static bool wait_area(int x, int y, int w, int h, uint32_t color, int ms) {
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end) { if (area_is(x, y, w, h, color)) return true; dispatch_for(20); }
    return area_is(x, y, w, h, color);
}
static void expect_area(const char* what, int x, int y, int w, int h, uint32_t color) {
    if (wait_area(x, y, w, h, color, 4000)) return;
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) if (pixel(i, j) != color) {
        fprintf(stderr, "[64] WPTEST FAIL %s pixel %d,%d expected=%06x actual=%06x\n", what, i, j, color, pixel(i, j)); fflush(stderr); exit(1);
    }
}

/* ---- window, decoration, xdg-output ---- */
static struct xdg_surface* xsurface; static struct xdg_toplevel* toplevel;
static struct zxdg_toplevel_decoration_v1* decoration;
static struct wp_viewport *viewport, *child_viewport, *cursor_viewport;
static uint32_t configure_serial; static unsigned configures, decoration_configures, decoration_mode, decoration_before_configure;
static int origin_x = -1, origin_y = -1;
static void xdg_configure(void* d, struct xdg_surface* x, uint32_t serial) {
    (void)d; configure_serial = serial; configures++; xdg_surface_ack_configure(x, serial); recommit = mapped;
}
static const struct xdg_surface_listener xdg_listener = {xdg_configure};
static void top_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states) { (void)d; (void)t; (void)w; (void)h; (void)states; }
static void top_close(void* d, struct xdg_toplevel* t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure, .close = top_close};
static void decoration_configure(void* d, struct zxdg_toplevel_decoration_v1* x, uint32_t mode) {
    (void)d; (void)x; decoration_mode = mode; decoration_configures++; if (!configures) decoration_before_configure++;
}
static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {decoration_configure};
static int out_mode_w, out_mode_h, out_dones, xo_w = -1, xo_h = -1, xo_x = -1, xo_y = -1; static char xo_name[64], xo_desc[128];
static void out_geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char* make, const char* model, int32_t t) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)t; }
static void out_mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)d; (void)o; (void)refresh; if (flags & WL_OUTPUT_MODE_CURRENT) { out_mode_w = w; out_mode_h = h; } }
static void out_done(void* d, struct wl_output* o) { (void)d; (void)o; out_dones++; }
static void out_scale(void* d, struct wl_output* o, int32_t f) { (void)d; (void)o; CHECK(f == 1); }
static const struct wl_output_listener output_listener = {out_geometry, out_mode, out_done, out_scale, NULL, NULL};
static void xo_position(void* d, struct zxdg_output_v1* o, int32_t x, int32_t y) { (void)d; (void)o; xo_x = x; xo_y = y; }
static void xo_size(void* d, struct zxdg_output_v1* o, int32_t w, int32_t h) { (void)d; (void)o; xo_w = w; xo_h = h; }
static void xo_done(void* d, struct zxdg_output_v1* o) { (void)d; (void)o; CHECK(!"xdg_output.done is deprecated in version 3"); }
static void xo_name_ev(void* d, struct zxdg_output_v1* o, const char* n) { (void)d; (void)o; snprintf(xo_name, sizeof(xo_name), "%s", n); }
static void xo_desc_ev(void* d, struct zxdg_output_v1* o, const char* n) { (void)d; (void)o; snprintf(xo_desc, sizeof(xo_desc), "%s", n); }
static const struct zxdg_output_v1_listener xo_listener = {xo_position, xo_size, xo_done, xo_name_ev, xo_desc_ev};

/* ---- seat ---- */
static struct wl_pointer* pointer; static struct wl_keyboard* keyboard;
static struct zwp_relative_pointer_v1* relative;
static struct zwp_locked_pointer_v1* lock; static struct zwp_confined_pointer_v1* confine;
static uint32_t enter_serial; static struct wl_surface* pointer_surface; static int px = -1, py = -1;
static bool locked, confined; static unsigned motions_locked, motions; static int rel_x, rel_y, max_x, max_y, min_x = 1 << 30, min_y = 1 << 30;
static int axis_source = -1, axis_discrete; static wl_fixed_t axis_value; static bool axis_framed;
static int repeat_rate = -1, repeat_delay = -1, key_state; static unsigned keys; static bool keymap_ok;
static void p_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; enter_serial = serial; pointer_surface = s; px = wl_fixed_to_int(x); py = wl_fixed_to_int(y);
    mark("ENTER x=%d y=%d main=%d", px, py, s == surface);
}
static void p_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) { (void)d; (void)p; (void)serial; (void)s; pointer_surface = NULL; mark("LEAVE"); }
static void p_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t; px = wl_fixed_to_int(x); py = wl_fixed_to_int(y); motions++;
    if (locked) motions_locked++;
    if (confined) {
        if (px < 50 || py < 50 || px >= 250 || py >= 150) { fprintf(stderr, "[64] WPTEST FAIL confined motion escaped %d,%d\n", px, py); exit(1); }
        if (px > max_x) max_x = px; if (py > max_y) max_y = py; if (px < min_x) min_x = px; if (py < min_y) min_y = py;
    }
}
static void p_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t t, uint32_t b, uint32_t s) { (void)d; (void)p; (void)serial; (void)t; (void)b; (void)s; }
static void p_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t axis, wl_fixed_t v) { (void)d; (void)p; (void)t; if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) axis_value = v; }
static void p_frame(void* d, struct wl_pointer* p) { (void)d; (void)p; if (axis_value) axis_framed = true; }
static void p_source(void* d, struct wl_pointer* p, uint32_t s) { (void)d; (void)p; axis_source = (int)s; }
static void p_stop(void* d, struct wl_pointer* p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_discrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t v) { (void)d; (void)p; if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) axis_discrete += v; }
static const struct wl_pointer_listener pointer_listener = {p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_source, p_stop, p_discrete, NULL, NULL};
static uint64_t last_relative;
static void r_motion(void* d, struct zwp_relative_pointer_v1* r, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t ux, wl_fixed_t uy) {
    (void)d; (void)r; (void)hi; (void)lo; CHECK(dx == ux && dy == uy); rel_x += wl_fixed_to_int(dx); rel_y += wl_fixed_to_int(dy); last_relative = now_ms();
}
static void quiet(void) {   /* wait until the host has stopped moving the mouse for 1.5 s */
    uint64_t end = now_ms() + 60000;
    while (now_ms() - last_relative < 1500 && now_ms() < end) dispatch_for(100);
}
static const struct zwp_relative_pointer_v1_listener relative_listener = {r_motion};
static void l_locked(void* d, struct zwp_locked_pointer_v1* l) { (void)d; (void)l; locked = true; motions_locked = 0; rel_x = rel_y = 0; mark("LOCKED x=%d y=%d", px, py); }
static void l_unlocked(void* d, struct zwp_locked_pointer_v1* l) { (void)d; (void)l; locked = false; mark("UNLOCKED"); }
static const struct zwp_locked_pointer_v1_listener lock_listener = {l_locked, l_unlocked};
static void c_confined(void* d, struct zwp_confined_pointer_v1* x) { (void)d; (void)x; confined = true; mark("CONFINED x=%d y=%d", px, py); }
static void c_unconfined(void* d, struct zwp_confined_pointer_v1* x) { (void)d; (void)x; confined = false; mark("UNCONFINED"); }
static const struct zwp_confined_pointer_v1_listener confine_listener = {c_confined, c_unconfined};
static void k_keymap(void* d, struct wl_keyboard* k, uint32_t format, int fd, uint32_t size) {
    (void)d; (void)k; char* text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    keymap_ok = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && text != MAP_FAILED && strstr(text, "xkb_keymap") != NULL;
    if (text != MAP_FAILED) munmap(text, size); close(fd);
}
static void k_enter(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* x, struct wl_array* keys_held) { (void)d; (void)k; (void)s; (void)keys_held; mark("KEYBOARD FOCUS main=%d", x == surface); }
static void k_leave(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* x) { (void)d; (void)k; (void)s; (void)x; }
static void k_key(void* d, struct wl_keyboard* k, uint32_t s, uint32_t t, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)s; (void)t; if (key == 30) { keys++; key_state = (int)state; }
}
static void k_mods(void* d, struct wl_keyboard* k, uint32_t s, uint32_t a, uint32_t b, uint32_t x, uint32_t g) { (void)d; (void)k; (void)s; (void)a; (void)b; (void)x; (void)g; }
static void k_repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) { (void)d; (void)k; repeat_rate = rate; repeat_delay = delay; }
static const struct wl_keyboard_listener keyboard_listener = {k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat};


/* ---- protocol error checks: each runs on its own connection, which the error closes ---- */
static void expect_error(const char* name, const struct wl_interface* interface, uint32_t code, void (*provoke)(struct conn*)) {
    struct conn k; connect_to(&k); provoke(&k);
    CHECK(wl_display_roundtrip(k.display) == -1 && wl_display_get_error(k.display) == EPROTO);
    const struct wl_interface* got = NULL; uint32_t id = 0, error = wl_display_get_protocol_error(k.display, &got, &id);
    if (!got || strcmp(got->name, interface->name) || error != code) {
        fprintf(stderr, "[64] WPTEST FAIL %s: got %s error %u, expected %s error %u\n", name, got ? got->name : "?", error, interface->name, code); exit(1);
    }
    wl_display_disconnect(k.display); mark("ERROR %s PASSED", name);
}
static struct wl_surface* error_toplevel(struct conn* k, struct xdg_toplevel** top) {
    struct wl_surface* s = wl_compositor_create_surface(k->compositor); struct xdg_surface* x = xdg_wm_base_get_xdg_surface(k->shell, s);
    *top = xdg_surface_get_toplevel(x); return s;
}
static void provoke_exists(struct conn* k) { struct wl_surface* s = wl_compositor_create_surface(k->compositor); wp_viewporter_get_viewport(k->viewporter, s); wp_viewporter_get_viewport(k->viewporter, s); }
static void provoke_source(struct conn* k) { wp_viewport_set_source(wp_viewporter_get_viewport(k->viewporter, wl_compositor_create_surface(k->compositor)), wl_fixed_from_int(-2), 0, wl_fixed_from_int(4), wl_fixed_from_int(4)); }
static void provoke_destination(struct conn* k) { wp_viewport_set_destination(wp_viewporter_get_viewport(k->viewporter, wl_compositor_create_surface(k->compositor)), 0, 5); }
static void provoke_size(struct conn* k) {
    struct wl_surface* s = wl_compositor_create_surface(k->compositor); struct wp_viewport* v = wp_viewporter_get_viewport(k->viewporter, s);
    wp_viewport_set_source(v, 0, 0, wl_fixed_from_double(10.5), wl_fixed_from_int(10)); wl_surface_commit(s);
}
static void provoke_out_of_buffer(struct conn* k) {
    struct wl_surface* s = wl_compositor_create_surface(k->compositor); struct wp_viewport* v = wp_viewporter_get_viewport(k->viewporter, s);
    struct buffer b = make_buffer(k, 20, 20, WL_SHM_FORMAT_XRGB8888);
    wp_viewport_set_source(v, wl_fixed_from_int(10), 0, wl_fixed_from_int(15), wl_fixed_from_int(10)); wp_viewport_set_destination(v, 30, 20);
    wl_surface_attach(s, b.buffer, 0, 0); wl_surface_commit(s);
}
static void provoke_no_surface(struct conn* k) {
    struct wl_surface* s = wl_compositor_create_surface(k->compositor); struct wp_viewport* v = wp_viewporter_get_viewport(k->viewporter, s);
    wl_surface_destroy(s); wp_viewport_set_destination(v, 10, 10);
}
static void provoke_decorated_twice(struct conn* k) { struct xdg_toplevel* t; error_toplevel(k, &t); zxdg_decoration_manager_v1_get_toplevel_decoration(k->decoration, t); zxdg_decoration_manager_v1_get_toplevel_decoration(k->decoration, t); }
static void provoke_bad_mode(struct conn* k) { struct xdg_toplevel* t; error_toplevel(k, &t); zxdg_toplevel_decoration_v1_set_mode(zxdg_decoration_manager_v1_get_toplevel_decoration(k->decoration, t), 7); }
static void provoke_orphan(struct conn* k) {
    struct xdg_toplevel* t; error_toplevel(k, &t); struct zxdg_toplevel_decoration_v1* d = zxdg_decoration_manager_v1_get_toplevel_decoration(k->decoration, t);
    xdg_toplevel_destroy(t); zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
static void provoke_constrained(struct conn* k) {
    struct wl_surface* s = wl_compositor_create_surface(k->compositor); struct wl_pointer* p = wl_seat_get_pointer(k->seat);
    zwp_pointer_constraints_v1_lock_pointer(k->constraints, s, p, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_pointer_constraints_v1_confine_pointer(k->constraints, s, p, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
}

static void draw_main(void) {
    /* 330x250 buffer; the viewport source (5,5,320,240) crops the yellow border, the destination scales it to 640x480. */
    struct buffer b = make_buffer(&c, 330, 250, WL_SHM_FORMAT_XRGB8888);
    fill(&b, 0, 0, 330, 250, BORDER);
    fill(&b, 5, 5, 160, 120, RED); fill(&b, 165, 5, 160, 120, GREEN); fill(&b, 5, 125, 160, 120, BLUE); fill(&b, 165, 125, 160, 120, GRAY);
    fill(&b, 5, 5, 5, 5, MARKER);            /* -> 10x10 block at the window origin */
    fill(&b, 105, 25, 1, 200, 0x000000);     /* one source column -> two screen columns at x=200,201 */
    wp_viewport_set_source(viewport, wl_fixed_from_int(5), wl_fixed_from_int(5), wl_fixed_from_int(320), wl_fixed_from_int(240));
    wp_viewport_set_destination(viewport, 640, 480);
    xdg_surface_set_window_geometry(xsurface, 0, 0, 640, 480);
    wl_surface_attach(surface, b.buffer, 0, 0); wl_surface_damage_buffer(surface, 0, 0, 330, 250); wl_surface_commit(surface);
}
static void draw_child(void) {
    /* 100x100 buffer, left half orange, right half purple, scaled down to 40x20 at (580,20). */
    struct buffer b = make_buffer(&c, 100, 100, WL_SHM_FORMAT_XRGB8888);
    fill(&b, 0, 0, 50, 100, ORANGE); fill(&b, 50, 0, 50, 100, PURPLE);
    wp_viewport_set_destination(child_viewport, 40, 20);
    wl_surface_attach(child, b.buffer, 0, 0); wl_surface_damage_buffer(child, 0, 0, 100, 100); wl_surface_commit(child);
}
static void find_origin(void) {
    uint64_t end = now_ms() + 8000;
    while (now_ms() < end) {
        for (unsigned y = 1; y + 10 < fb_h; y++) for (unsigned x = 1; x + 10 < fb_w; x++)
            if (pixel(x, y) == MARKER && pixel(x + 9, y + 9) == MARKER && pixel(x - 1, y) != MARKER && pixel(x, y - 1) != MARKER &&
                pixel(x + 10, y) != MARKER && pixel(x, y + 10) != MARKER) { origin_x = (int)x; origin_y = (int)y; return; }
        dispatch_for(100);
    }
    CHECK(!"window marker not found on screen");
}
static void wait_until(bool* flag, int ms, const char* what) {
    uint64_t end = now_ms() + (uint64_t)ms;
    while (!*flag && now_ms() < end) dispatch_for(50);
    if (!*flag) { fprintf(stderr, "[64] WPTEST FAIL timeout waiting for %s\n", what); exit(1); }
}

int main(int argc, char** argv) {
    /* --wayland-info PATH: run Debian's wayland-info against Aurora first (its listing goes to the log). */
    if (argc == 3 && !strcmp(argv[1], "--wayland-info")) {
        pid_t child_pid = fork(); CHECK(child_pid >= 0);
        if (!child_pid) { execl(argv[2], argv[2], (char*)NULL); perror("wayland-info"); _exit(127); }
        int status; CHECK(waitpid(child_pid, &status, 0) == child_pid);
        mark("WAYLAND-INFO status=%d", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } else CHECK(argc == 1);
    open_fb(); connect_to(&c);
    mark("GLOBALS %u", c.globals);
    /* wl_output (v2) + zxdg_output_v1 (v3): name, logical geometry; v3 ends with wl_output.done */
    wl_output_add_listener(c.output, &output_listener, NULL);
    struct zxdg_output_v1* xo = zxdg_output_manager_v1_get_xdg_output(c.xdg_output, c.output); zxdg_output_v1_add_listener(xo, &xo_listener, NULL);
    CHECK(wl_display_roundtrip(c.display) >= 0); CHECK(wl_display_roundtrip(c.display) >= 0);
    CHECK(out_mode_w == (int)fb_w && out_mode_h == (int)fb_h && xo_w == out_mode_w && xo_h == out_mode_h && xo_x == 0 && xo_y == 0 && out_dones >= 2);
    CHECK((!strncmp(xo_name, "Virtual-", 8) || !strcmp(xo_name, "FB-1")) && xo_desc[0]);   /* KMS connector name, or the fbdev display */
    mark("XDG OUTPUT PASSED name=%s size=%dx%d desc=%s", xo_name, xo_w, xo_h, xo_desc);
    /* seat */
    pointer = wl_seat_get_pointer(c.seat); keyboard = wl_seat_get_keyboard(c.seat);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL); wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    relative = zwp_relative_pointer_manager_v1_get_relative_pointer(c.relative, pointer); zwp_relative_pointer_v1_add_listener(relative, &relative_listener, NULL);
    CHECK(wl_display_roundtrip(c.display) >= 0);
    CHECK(keymap_ok && repeat_rate == 25 && repeat_delay == 500);
    mark("KEYBOARD INFO PASSED repeat=%d/%d", repeat_rate, repeat_delay);
    /* toplevel with decoration created before the first commit */
    surface = wl_compositor_create_surface(c.compositor); viewport = wp_viewporter_get_viewport(c.viewporter, surface);
    xsurface = xdg_wm_base_get_xdg_surface(c.shell, surface); xdg_surface_add_listener(xsurface, &xdg_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xsurface); xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Wine protocol test"); xdg_toplevel_set_app_id(toplevel, "nexus.wptest");
    decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(c.decoration, toplevel);
    zxdg_toplevel_decoration_v1_add_listener(decoration, &decoration_listener, NULL);
    zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    wl_surface_commit(surface);
    while (!configures) CHECK(wl_display_dispatch(c.display) >= 0);
    CHECK(decoration_configures >= 1 && decoration_before_configure >= 1 && decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    child = wl_compositor_create_surface(c.compositor); child_viewport = wp_viewporter_get_viewport(c.viewporter, child);
    struct wl_subsurface* sub = wl_subcompositor_get_subsurface(c.subcompositor, child, surface);
    wl_subsurface_set_position(sub, 580, 20); wl_subsurface_set_desync(sub);
    draw_child(); draw_main(); mapped = true;
    CHECK(wl_display_roundtrip(c.display) >= 0); dispatch_for(300);
    /* after mapping: set_mode again -> client_side again plus a new xdg_surface.configure */
    unsigned before = configures, decorated = decoration_configures;
    zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    uint64_t end = now_ms() + 3000;
    while ((configures == before || decoration_configures == decorated) && now_ms() < end) dispatch_for(50);
    CHECK(configures > before && decoration_configures > decorated && decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    mark("DECORATION PASSED mode=client_side configures=%u", decoration_configures);
    /* viewport composition */
    find_origin();
    mark("ORIGIN x=%d y=%d", origin_x, origin_y);
    expect_area("marker", origin_x, origin_y, 10, 10, MARKER);
    expect_area("red quadrant", origin_x + 10, origin_y + 10, 180, 220, RED);
    expect_area("red quadrant right of the column", origin_x + 210, origin_y + 10, 100, 220, RED);
    expect_area("blue quadrant", origin_x + 10, origin_y + 250, 180, 220, BLUE);
    expect_area("blue quadrant right of the column", origin_x + 210, origin_y + 250, 100, 220, BLUE);
    expect_area("gray quadrant", origin_x + 330, origin_y + 250, 300, 220, GRAY);
    expect_area("green quadrant", origin_x + 330, origin_y + 50, 240, 180, GREEN);
    expect_area("scaled column", origin_x + 200, origin_y + 40, 2, 400, 0x000000);
    CHECK(pixel(origin_x + 199, origin_y + 100) == RED && pixel(origin_x + 202, origin_y + 100) == RED);
    for (int y = origin_y; y < origin_y + 480; y++) for (int x = origin_x; x < origin_x + 640; x++) CHECK(pixel(x, y) != BORDER);
    CHECK(pixel(origin_x + 640, origin_y + 240) != GRAY && pixel(origin_x + 640, origin_y + 240) != GREEN);   /* destination size, not buffer size */
    mark("VIEWPORT SCALE PASSED");
    expect_area("downscaled left", origin_x + 580, origin_y + 20, 20, 20, ORANGE);
    expect_area("downscaled right", origin_x + 600, origin_y + 20, 20, 20, PURPLE);
    CHECK(pixel(origin_x + 620, origin_y + 30) == GREEN && pixel(origin_x + 590, origin_y + 40) == GREEN);
    mark("VIEWPORT DOWNSCALE PASSED");
    /* protocol errors */
    expect_error("viewport_exists", &wp_viewporter_interface, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, provoke_exists);
    expect_error("bad_source", &wp_viewport_interface, WP_VIEWPORT_ERROR_BAD_VALUE, provoke_source);
    expect_error("bad_destination", &wp_viewport_interface, WP_VIEWPORT_ERROR_BAD_VALUE, provoke_destination);
    expect_error("bad_size", &wp_viewport_interface, WP_VIEWPORT_ERROR_BAD_SIZE, provoke_size);
    expect_error("out_of_buffer", &wp_viewport_interface, WP_VIEWPORT_ERROR_OUT_OF_BUFFER, provoke_out_of_buffer);
    expect_error("no_surface", &wp_viewport_interface, WP_VIEWPORT_ERROR_NO_SURFACE, provoke_no_surface);
    expect_error("decoration_twice", &zxdg_decoration_manager_v1_interface, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED, provoke_decorated_twice);
    expect_error("decoration_mode", &zxdg_toplevel_decoration_v1_interface, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE, provoke_bad_mode);
    expect_error("decoration_orphaned", &zxdg_toplevel_decoration_v1_interface, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED, provoke_orphan);
    expect_error("already_constrained", &zwp_pointer_constraints_v1_interface, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED, provoke_constrained);
    mark("AUTOMATIC PASSED");

    /* ---- pointer lock + relative motion ---- */
    mark("POINTER READY center=%d,%d", origin_x + 320, origin_y + 240);
    end = now_ms() + 60000;
    while ((pointer_surface != surface || px != 320 || py != 240) && now_ms() < end) dispatch_for(50);
    CHECK(pointer_surface == surface && px == 320 && py == 240);
    lock = zwp_pointer_constraints_v1_lock_pointer(c.constraints, surface, pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(lock, &lock_listener, NULL); wl_surface_commit(surface);
    wait_until(&locked, 10000, "locked");
    int lock_x = px, lock_y = py;
    end = now_ms() + 60000;
    while ((rel_x < 70 || rel_y < 65) && now_ms() < end) dispatch_for(50);
    dispatch_for(300);
    CHECK(rel_x == 70 && rel_y == 65 && motions_locked == 0 && px == lock_x && py == lock_y);
    mark("RELATIVE dx=%d dy=%d motions=%u", rel_x, rel_y, motions_locked);
    zwp_locked_pointer_v1_set_cursor_position_hint(lock, wl_fixed_from_int(100), wl_fixed_from_int(80)); wl_surface_commit(surface);
    CHECK(wl_display_roundtrip(c.display) >= 0);
    zwp_locked_pointer_v1_destroy(lock); lock = NULL; locked = false;
    end = now_ms() + 5000;
    while ((px != 100 || py != 80) && now_ms() < end) dispatch_for(50);
    CHECK(px == 100 && py == 80);
    mark("HINT PASSED x=%d y=%d", px, py);

    /* ---- confinement to (50,50) 200x100 ---- */
    struct wl_region* region = wl_compositor_create_region(c.compositor); wl_region_add(region, 50, 50, 200, 100);
    confine = zwp_pointer_constraints_v1_confine_pointer(c.constraints, surface, pointer, region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    wl_region_destroy(region); zwp_confined_pointer_v1_add_listener(confine, &confine_listener, NULL); wl_surface_commit(surface);
    wait_until(&confined, 10000, "confined");
    end = now_ms() + 60000; rel_x = rel_y = 0;
    while ((px != 249 || py != 149) && now_ms() < end) dispatch_for(50);
    quiet();
    CHECK(px == 249 && py == 149 && rel_x > 0 && rel_y > 0);
    mark("CONFINE MAX PASSED x=%d y=%d relative=%d,%d", px, py, rel_x, rel_y);
    end = now_ms() + 60000;
    while ((px != 50 || py != 50) && now_ms() < end) dispatch_for(50);
    quiet();
    CHECK(px == 50 && py == 50 && min_x == 50 && min_y == 50 && max_x == 249 && max_y == 149);
    mark("CONFINE MIN PASSED x=%d y=%d", px, py);
    zwp_confined_pointer_v1_destroy(confine); confine = NULL; confined = false;
    CHECK(wl_display_roundtrip(c.display) >= 0);

    /* ---- client cursor surfaces (pointer at local 50,50 over the red quadrant) ---- */
    CHECK(pointer_surface == surface);
    cursor = wl_compositor_create_surface(c.compositor); cursor_viewport = wp_viewporter_get_viewport(c.viewporter, cursor);
    struct buffer cb = make_buffer(&c, 12, 12, WL_SHM_FORMAT_ARGB8888); fill(&cb, 0, 0, 12, 12, 0xFF000000u | MAGENTA);
    wl_surface_attach(cursor, cb.buffer, 0, 0); wl_surface_damage_buffer(cursor, 0, 0, 12, 12);
    wp_viewport_set_destination(cursor_viewport, 24, 24); wl_surface_commit(cursor);
    wl_pointer_set_cursor(pointer, enter_serial, cursor, 12, 12);
    int sx = origin_x + px, sy = origin_y + py;
    expect_area("client cursor", sx - 12, sy - 12, 24, 24, MAGENTA);
    CHECK(pixel(sx - 13, sy) == RED && pixel(sx + 12, sy) == RED);
    mark("CURSOR SURFACE PASSED at=%d,%d", sx, sy);
    wl_pointer_set_cursor(pointer, enter_serial, NULL, 0, 0);
    expect_area("hidden cursor", sx - 16, sy - 16, 40, 40, RED);
    mark("CURSOR HIDDEN PASSED");
    wl_pointer_set_cursor(pointer, enter_serial + 1000, cursor, 12, 12);   /* stale serial: ignored */
    dispatch_for(600);
    CHECK(area_is(sx - 16, sy - 16, 40, 40, RED));
    mark("CURSOR SERIAL PASSED");
    wl_pointer_set_cursor(pointer, enter_serial, cursor, 0, 0);
    expect_area("cursor hotspot", sx, sy, 24, 24, MAGENTA);
    mark("CURSOR HOTSPOT PASSED");

    /* ---- wheel and key ---- */
    axis_discrete = 0; axis_source = -1; axis_value = 0; axis_framed = false; keys = 0;
    mark("WHEEL READY");
    end = now_ms() + 60000;
    while (!axis_framed && now_ms() < end) dispatch_for(50);
    CHECK(axis_framed && axis_source == WL_POINTER_AXIS_SOURCE_WHEEL && axis_discrete == 1 && axis_value == wl_fixed_from_int(10));
    mark("WHEEL PASSED discrete=%d value=%d", axis_discrete, wl_fixed_to_int(axis_value));
    end = now_ms() + 60000;
    while (keys < 2 && now_ms() < end) dispatch_for(50);
    CHECK(keys == 2 && key_state == WL_KEYBOARD_KEY_STATE_RELEASED);
    mark("KEY PASSED");
    mark("PASSED");
    wl_display_disconnect(c.display);
    return 0;
}
