#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/fb.h>
#include <linux/input.h>
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
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-server-protocol.h"
#include "popup_position.h"
#include "aurora_damage.h"
#include "aurora_graphics.h"
#include "aurora_dmabuf.h"
#include "aurora_protocols.h"
#include "aurora_wineapps.h"
#include "aurora_power.h"
#include "aurora_services.h"
#include "aurora_outputs.h"
#include "aurora_control.h"
#include "aurora_wm.h"
#include "aurora_lock.h"
#include "aurora_session.h"
#include "aurora_idle.h"
#include "aurora_reader.h"
#include "aurora_sak.h"
#include "aurora_clipboard.h"
#include "aurora_keyboard.h"
#include "aurora_a11y.h"
#include "aurora_ime.h"
#include "aurora_media.h"
#include "aurora_sound.h"
#include "aurora_xwm.h"
#include "viewporter-server-protocol.h"
#include "../../kernel/macui.h"
#include "../../kernel/dock.h"
#include "../../kernel/menubar.h"
#include "aurora_netstatus.h"
#include "../../kernel/spotlight.h"
#include "../../kernel/framebuffer.h"

struct region { struct rectangle* rectangles; };
enum role_kind { ROLE_NONE, ROLE_TOPLEVEL, ROLE_SUBSURFACE, ROLE_CURSOR, ROLE_POPUP, ROLE_IME_POPUP };
static bool ime_scene;   /* aurora_ime.c changed whether or where its candidate popup shows */
struct shell_binding { struct wl_resource* resource; unsigned references; };
/* Damage bookkeeping: small rectangle lists merged when that wastes little area. */
#define SURFACE_RECTS 16
struct rects { int n; struct popup_box r[SURFACE_RECTS]; };
/* Surface-local size and nearest sampling of the committed buffer: identity (w,h = buffer/scale) unless a wp_viewport
 * crops (source, surface-local units) or scales (destination). Buffer x of local column u: (x0 + u*dx) >> 16. */
struct view { int32_t w, h; bool on, fast; int64_t x0, dx, y0, dy; double sx, sy, kx, ky; };
struct cached_state {
    uint32_t* pixels; int width, height, scale;
    struct region input, opaque_region;
    bool valid, buffer_changed, input_changed, input_set, opaque, opaque_changed, in_place;
    struct popup_box geometry; bool geometry_changed;
};
struct surface {
    struct wl_list link, frames;
    struct wl_list window_link;
    struct wl_list children, pending_children, cached_children;
    struct wl_list plane, pending_plane, cached_plane;
    struct wl_list sibling, pending_sibling, cached_sibling;
    struct wl_resource *resource, *xdg, *role, *buffer;
    struct wl_resource* subsurface;
    struct surface* parent;
    struct surface* popup_parent;
    struct surface* transient_for; uint32_t configure_first; /* xdg_toplevel.set_parent; oldest unacknowledged configure */
    struct wl_list popup_link;
    struct shell_binding* shell;
    struct popup_rules positioner;
    struct popup_box popup_box, next_geometry;
    bool popup_dismissed, popup_grabbed, geometry_changed, geometry_clamp;
    struct cached_state cache;
    enum role_kind kind;
    bool synchronized;
    int32_t x, y, next_x, next_y, cached_x, cached_y;
    struct wl_listener buffer_destroy;
    uint32_t *pixels; int width, height, scale, next_scale;
    int geometry_x, geometry_y, geometry_w, geometry_h;
    struct region input, next_input; bool input_set, next_input_set, input_changed;
    bool attached, mapped, configured, acked, fullscreen, opaque;
    bool minimized, maximized, placed, configure_pending, configure_deferred;
    int screen; bool screen_set; unsigned entered;   /* display slot of a toplevel; wl_output slots this surface entered */
    int tile, workspace; bool desktop_hidden;       /* aurora_wm.h: AURORA_TILE_*, workspace index, minimized by "show desktop" */
    bool parked;                                    /* another user's session's window (aurora_session.h): off the window list */
    uint32_t* dock_thumb;                           /* DOCK_THUMB^2 picture while minimized (the dock shows it) */
    int requested_w, requested_h, min_w, min_h, max_w, max_h;
    int saved_x, saved_y, saved_w, saved_h;
    char title[128], app_id[96];
    uint32_t configure; unsigned references;
    struct rects damage_surface, damage_buffer, content_damage; /* pending (two spaces) and applied-unpainted */
    struct region opaque_region, next_opaque; bool next_opaque_changed;
    struct opaque_map omap;
    struct popup_box shown; bool was_shown; /* screen rectangle at the last paint */
    unsigned listed;
    struct aurora_viewport_state vp_next, vp_cached, vp; struct wl_resource* viewport; struct view view; /* wp_viewport */
    struct aurora_xwin* x11; bool x11_modal;        /* X11 role (aurora_xwm.c): the X window this Xwayland surface shows */
    uint32_t* x11_icon;                             /* its _NET_WM_ICON as AURORA_XICON^2 ARGB (the dock shows it) */
};
struct endpoint { struct wl_list link; struct wl_resource* resource; bool keyboard; uint32_t enter_serial; struct surface* cursor; int32_t hot_x, hot_y; bool cursor_set; };
struct output { struct wl_list link; struct wl_resource* resource; int slot; };   /* slot: aurora_output index */
enum frame_state { FRAME_PENDING, FRAME_CACHED, FRAME_READY };
struct frame { struct wl_list link; struct wl_resource* resource; enum frame_state state; };
static struct wl_display* display;
static struct wl_list surfaces, endpoints, popups, windows;
static struct wl_list x11_overrides;   /* override-redirect X windows (menus, tooltips): unmanaged, above every window */
static struct wl_list data_devices;
static struct wl_list outputs;
static struct surface *focus, *pointer_focus;
static struct surface* pointer_grab;
static struct surface *popup_grab, *button_origin, *key_origin;
static uint32_t button_serial,key_serial,swallowed_buttons;
static uint32_t pointer_buttons;
static int64_t pointer_local_x, pointer_local_y;
static struct xkb_context* xkb_context;
static struct xkb_keymap* keymap;
static struct xkb_state* key_state;
static int keymap_fd = -1; static size_t keymap_size;
/* width x height: the desktop (bounding box of the outputs, aurora_outputs.h); canvas/stride: the output being painted. */
static uint32_t *canvas; static unsigned width, height, stride;
static struct aurora_output* painting; static struct popup_box output_cursor[AURORA_OUTPUTS];
/* The shell (menu bar, dock, Spotlight, Launchpad, notices) is drawn on and hit-tested in primary-display coordinates. */
#define SHELL_X(value) ((int)((value) - aurora_output_primary()->x))
#define SHELL_Y(value) ((int)((value) - aurora_output_primary()->y))
static void shell_canvas(void) { struct aurora_output* p = aurora_output_primary(); if (p->canvas) aurora_canvas_scaled(p->canvas, p->mode_w, p->mode_h, p->scale120); else aurora_canvas(NULL, 0, 0); }
/* dirty: a paint pass is pending (scene diff + damage); cursor_dirty: only the sprite moved. */
static int pointer_x, pointer_y; static bool dirty = true, cursor_dirty = true, key_dropped, mouse_dropped;
static int cursor_x, cursor_y; static bool cursor_shown;
/* Client cursors (wl_pointer.set_cursor): the endpoint whose cursor is in effect while its client has pointer focus,
 * and the screen rectangle the cursor occupied at the last present. */
static struct endpoint* cursor_owner; static uint32_t* cursor_save; static size_t cursor_save_size;
static const struct wl_output_interface output_interface;
/* Pointer constraints and relative motion: the active constraint kind and this input frame's device motion. */
static enum aurora_constraint_kind constraint_kind; static bool constraint_busy;
static int64_t motion_dx, motion_dy, motion_abs[2] = {-1, -1}, motion_abs_last[2] = {-1, -1};
static void constraint_check(void);
#define SCREEN_RECTS 32
static struct popup_box screen_damage[SCREEN_RECTS]; static int screen_damage_count;
/* Shell layers: the first seven (power: aurora_power.c) live on the primary display; SNAP (the drop preview of a dragged window) and WM (switcher,
 * overview: aurora_wm.c) are desktop layers drawn on whichever displays they cover. */
enum { SHELL_DOCK, SHELL_MENUBAR, SHELL_MENU, SHELL_LAUNCHPAD, SHELL_SPOTLIGHT, SHELL_NOTICE, SHELL_POWER, SHELL_SNAP, SHELL_WM, SHELL_LOCK, SHELL_ZOOM, SHELL_COUNT };
#define SHELL_DESKTOP_LAYERS ((1U<<SHELL_SNAP)|(1U<<SHELL_WM)|(1U<<SHELL_LOCK)|(1U<<SHELL_ZOOM))
static int wm_workspace, wm_snap_kind, wm_snap_screen;   /* current workspace; drop preview: 0 none, 1 left, 2 right, 3 maximize */
#define DOCK_THUMB 96
static struct popup_box wm_tile_area(struct surface* s); static struct popup_box wm_snap_rect(void);
static void wm_snap_update(void); static void wm_drop(struct surface* s); static void restore_geometry(struct surface* s);
static void dock_extra_click(int k);
static bool sticky_key(unsigned key,int value);
#define SHELL_ALL ((1U<<SHELL_COUNT)-1)
static unsigned shell_dirty = SHELL_ALL;
static struct popup_box shell_shown[SHELL_COUNT]; static bool shell_was[SHELL_COUNT];
#define FRAME_NS 16666667ULL
static uint64_t nanoseconds(void);
static uint64_t last_paint_ns;
/* Learned oversleep of timed waits. Guest timeouts end on host timer ticks (about 15.6 ms on a
 * default Windows host), so a frame may start up to this much early instead of a whole tick late. */
static uint64_t wake_slack_ns = 2000000;
/* Input boost: for 60 ms after a key, button or wheel event, commits from the client that received it
 * are painted at once instead of on the frame cadence, so typing/clicking/scrolling feedback is not
 * delayed behind an animation's frame slot. */
static struct wl_client* boost_client; static uint64_t boost_until; static bool paint_asap;
static void input_boost(struct surface* target) { if (target && target->resource) { boost_client = wl_resource_get_client(target->resource); boost_until = nanoseconds() + 60000000ULL; } }
static uint64_t frame_due(void) { return last_paint_ns + FRAME_NS - (wake_slack_ns < FRAME_NS / 2 ? wake_slack_ns : FRAME_NS / 2); }
static bool stats_enabled;
static struct { unsigned paints, cursor_only, commits; uint64_t paint_ns, paint_max_ns, present_bytes, copy_ns, copy_bytes, drawn_pixels, last_report, stage_ns[3], commit_wait_ns, client_ns, first_commit, frame_sent; unsigned waits, turns; } stats;
static uint8_t pressed[(KEY_MAX + 8) / 8];
static bool aurora_mode, shutting_down, desktop_focus;
static struct surface* moving;
static uint32_t resize_edges;
static int move_x, move_y, move_w, move_h, move_px, move_py;
static uint8_t shell_keys[(KEY_MAX + 8) / 8];
static unsigned window_sequence;
static char shell_notice[160];
static uint32_t notice_until;
static bool shell_overlay(void);
static struct surface* popup_family(struct surface* surface);
static bool shell_hit(int x,int y);
static bool shell_bar_hit(int x,int y);
static void shell_action(int action);
static void shell_motion(void);
static void shell_click(int x,int y);
static bool shell_key(unsigned key,int value);
static void shell_reaped(pid_t pid);
static void shell_draw(unsigned mask);
static bool shell_element(int element,struct popup_box* bounds);
static char** load_arguments(const char* path,const char* program,int* count);
static void free_arguments(char** command);
static void cancel_move(struct surface* surface){if(moving==surface){moving=NULL;resize_edges=0;swallowed_buttons|=pointer_buttons;pointer_grab=NULL;}}
static struct popup_box screen_box(void) { return (struct popup_box){0, 0, (int32_t)width, (int32_t)height}; }
static void damage_screen(struct popup_box b) { rects_add(screen_damage, &screen_damage_count, SCREEN_RECTS, b, screen_box()); dirty = true; }
static void damage_box64(int64_t x, int64_t y, int64_t w, int64_t h) {
    if (w <= 0 || h <= 0) return;
    int64_t x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0; if (x1 > width) x1 = width; if (y1 > height) y1 = height;
    if (x < x1 && y < y1) damage_screen((struct popup_box){(int32_t)x, (int32_t)y, (int32_t)(x1 - x), (int32_t)(y1 - y)});
}
static void rects_clear(struct rects* r) { r->n = 0; }
static void rects_push(struct rects* r, struct popup_box b, struct popup_box bounds) { rects_add(r->r, &r->n, SURFACE_RECTS, b, bounds); }
void fb_mark_dirty(int x,int y,int w,int h){damage_box64(x,y,w,h);}
void fb_mark_dirty_all(void){damage_screen(screen_box());}
void fb_flip(void){dirty=true;}
void fb_flip_full(void){fb_mark_dirty_all();}
void fb_flip_legacy(void){fb_mark_dirty_all();}
void fb_flip_region(int x,int y,int w,int h){fb_mark_dirty(x,y,w,h);}
static uint32_t milliseconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static uint64_t nanoseconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec; }
static uint64_t cpu_nanoseconds(clockid_t clock) { struct timespec t; if (!stats_enabled || clock_gettime(clock, &t)) return 0; return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec; }
static void destroy_request(struct wl_client* client, struct wl_resource* resource) { (void)client; wl_resource_destroy(resource); }
static void region_clear(struct region* region) { while (region->rectangles) { struct rectangle* next = region->rectangles->next; free(region->rectangles); region->rectangles = next; } }
static bool region_copy(struct region* to, const struct region* from) {
    struct region next = {0}; struct rectangle** tail = &next.rectangles;
    for (struct rectangle* r = from->rectangles; r; r = r->next) {
        *tail = malloc(sizeof(**tail)); if (!*tail) { region_clear(&next); return false; }
        **tail = *r; (*tail)->next = NULL; tail = &(*tail)->next;
    }
    region_clear(to); *to = next; return true;
}
static bool region_has(const struct region* region, int x, int y) {
    bool result = false;
    for (struct rectangle* r = region->rectangles; r; r = r->next)
        if ((int64_t)x >= r->x && (int64_t)y >= r->y && (int64_t)x < (int64_t)r->x + r->w && (int64_t)y < (int64_t)r->y + r->h) result = r->add;
    return result;
}
static void region_destroy(struct wl_resource* resource) { struct region* region = wl_resource_get_user_data(resource); region_clear(region); free(region); }
static void region_rectangle(struct wl_resource* resource, int32_t x, int32_t y, int32_t w, int32_t h, bool add) {
    if (w <= 0 || h <= 0) return;
    struct rectangle* rectangle = malloc(sizeof(*rectangle)); if (!rectangle) { wl_resource_post_no_memory(resource); return; }
    *rectangle = (struct rectangle){.x=x,.y=y,.w=w,.h=h,.add=add};
    struct region* region = wl_resource_get_user_data(resource); struct rectangle** tail = &region->rectangles;
    while (*tail) tail = &(*tail)->next; *tail = rectangle;
}
static void region_add(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; region_rectangle(r,x,y,w,h,true); }
static void region_subtract(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; region_rectangle(r,x,y,w,h,false); }
static const struct wl_region_interface region_interface = {destroy_request,region_add,region_subtract};
static void create_region(struct wl_client* client, struct wl_resource* compositor, uint32_t id) {
    struct region* data = calloc(1,sizeof(*data)); struct wl_resource* resource = wl_resource_create(client,&wl_region_interface,1,id);
    if (!data || !resource) { free(data); if (resource) wl_resource_destroy(resource); wl_resource_post_no_memory(compositor); return; }
    wl_resource_set_implementation(resource,&region_interface,data,region_destroy);
}
/* Opaque coverage of the current buffer: XRGB is fully opaque, otherwise the client's opaque region. */
static void omap_build(struct surface* s) {
    opaque_map_build(&s->omap, s->opaque_region.rectangles, s->pixels ? s->view.w : 0, s->pixels ? s->view.h : 0, s->opaque);
}
/* View of a width x height buffer at scale under viewport state vp. fast: integral step with every sample inside. */
static struct view view_compute(const struct aurora_viewport_state* vp, int width, int height, int scale) {
    struct view v = {0};
    if (width <= 0 || height <= 0 || scale <= 0) return v;
    double sx = 0, sy = 0, sw = (double)width / scale, sh = (double)height / scale;
    if (vp->source) { sx = wl_fixed_to_double(vp->x); sy = wl_fixed_to_double(vp->y); sw = wl_fixed_to_double(vp->w); sh = wl_fixed_to_double(vp->h); }
    v.w = vp->destination ? vp->dw : vp->source ? (int32_t)sw : width / scale;
    v.h = vp->destination ? vp->dh : vp->source ? (int32_t)sh : height / scale;
    v.on = vp->source || vp->destination;
    if (v.w <= 0 || v.h <= 0 || sw <= 0 || sh <= 0) { v.w = v.h = 0; v.on = false; return v; }
    double stepx = sw * scale / v.w, stepy = sh * scale / v.h;   /* buffer pixels per surface-local pixel */
    v.sx = sx; v.sy = sy; v.kx = v.w / sw; v.ky = v.h / sh;
    v.dx = (int64_t)(stepx * 65536.0); v.dy = (int64_t)(stepy * 65536.0);
    v.x0 = (int64_t)((sx * scale + stepx / 2) * 65536.0); v.y0 = (int64_t)((sy * scale + stepy / 2) * 65536.0);
    if (!v.on) { v.x0 = v.y0 = 0; v.dx = v.dy = (int64_t)scale * 65536; }
    v.fast = v.dx == (int64_t)scale * 65536 && v.dy == (int64_t)scale * 65536 && v.x0 >= 0 && v.y0 >= 0 &&
             v.x0 / 65536 + (int64_t)(v.w - 1) * scale < width && v.y0 / 65536 + (int64_t)(v.h - 1) * scale < height;
    return v;
}
static bool view_equal(const struct view* a, const struct view* b) {
    return a->w == b->w && a->h == b->h && a->on == b->on && a->x0 == b->x0 && a->y0 == b->y0 && a->dx == b->dx && a->dy == b->dy;
}
/* View v drawn into pw x ph output pixels (an output of any scale, fractional included): each output pixel samples the
 * buffer at its centre (a buffer at the output's scale maps 1:1). bw x bh is the buffer size. */
static struct view view_output(const struct view* v, int64_t pw, int64_t ph, int bw, int bh) {
    if (pw == v->w && ph == v->h) return *v;
    struct view r = *v;
    if (pw <= 0 || ph <= 0 || v->w <= 0 || v->h <= 0) { r.w = r.h = 0; r.fast = false; return r; }
    r.w = (int32_t)pw; r.h = (int32_t)ph;
    int64_t x0 = v->on ? v->x0 - v->dx / 2 : 0, y0 = v->on ? v->y0 - v->dy / 2 : 0;   /* left/top edge of the first sample */
    r.dx = v->dx * v->w / pw; r.dy = v->dy * v->h / ph; r.x0 = x0 + r.dx / 2; r.y0 = y0 + r.dy / 2;
    r.fast = r.dx >= 65536 && r.dy >= 65536 && !(r.dx & 65535) && !(r.dy & 65535) && r.x0 >= 0 && r.y0 >= 0 &&
             r.x0 / 65536 + (int64_t)(r.w - 1) * (r.dx / 65536) < bw && r.y0 / 65536 + (int64_t)(r.h - 1) * (r.dy / 65536) < bh;
    return r;
}
static int64_t floor64(double x) { int64_t f = (int64_t)x; return f > x ? f - 1 : f; }
static int64_t ceil_div(int64_t a, int64_t b) { return a >= 0 ? (a + b - 1) / b : -(-a / b); }
/* Columns [*lo,*hi) of one view axis (n columns; column u samples buffer column (o + u*d) >> 16, clamped to the size)
 * that sample buffer columns [b0,b1): the same fixed-point mapping draw_scaled() uses, with one column of slack. */
static void view_span(int64_t o, int64_t d, int64_t n, int64_t b0, int64_t b1, int64_t size, int64_t* lo, int64_t* hi) {
    *lo = d <= 0 ? 0 : ceil_div(b0 * 65536 - o, d) - 1; *hi = d <= 0 || b1 >= size ? n : ceil_div(b1 * 65536 - o, d) + 1;
    if (*lo < 0) *lo = 0; if (*hi > n) *hi = n;
}
/* Surface-local damage covering buffer rectangle r (buffer pixels of a width x height buffer) under view v. */
static struct popup_box view_damage(const struct view* v, struct popup_box r, int scale, int width, int height) {
    if (!v->on) {
        int32_t x0 = r.x / scale, y0 = r.y / scale, x1 = (r.x + r.w + scale - 1) / scale, y1 = (r.y + r.h + scale - 1) / scale;
        return (struct popup_box){x0, y0, x1 - x0, y1 - y0};
    }
    int64_t x0, x1, y0, y1;
    view_span(v->x0, v->dx, v->w, r.x, (int64_t)r.x + r.w, width, &x0, &x1); view_span(v->y0, v->dy, v->h, r.y, (int64_t)r.y + r.h, height, &y0, &y1);
    return x0 < x1 && y0 < y1 ? (struct popup_box){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)} : (struct popup_box){0, 0, 0, 0};
}
/* wp_viewport commit checks against the buffer that will be current (w x h, 0 when none). */
static bool viewport_check(struct surface* s, int w, int h) {
    const struct aurora_viewport_state* vp = &s->vp_next;
    if (!s->viewport || !vp->source) return true;
    if (!vp->destination && ((vp->w & 255) || (vp->h & 255))) {
        wl_resource_post_error(s->viewport, WP_VIEWPORT_ERROR_BAD_SIZE, "source size must be integral without a destination"); return false; }
    if (w > 0 && h > 0 && ((int64_t)vp->x + vp->w > (int64_t)w * 256 / s->next_scale || (int64_t)vp->y + vp->h > (int64_t)h * 256 / s->next_scale)) {
        wl_resource_post_error(s->viewport, WP_VIEWPORT_ERROR_OUT_OF_BUFFER, "source rectangle extends outside the buffer"); return false; }
    return true;
}
static void frame_destroy(struct wl_resource* resource) { struct frame* frame = wl_resource_get_user_data(resource); wl_list_remove(&frame->link); free(frame); }
static void detach_buffer(struct surface* surface) {
    if (surface->buffer) { wl_list_remove(&surface->buffer_destroy.link); surface->buffer = NULL; }
}
static void buffer_destroyed(struct wl_listener* listener, void* data) {
    (void)data; struct surface* surface = wl_container_of(listener,surface,buffer_destroy); detach_buffer(surface);
}
static void surface_release(struct surface* surface) {
    if (--surface->references) return;
    if (surface->was_shown) damage_screen(surface->shown);
    wl_list_remove(&surface->link); { struct surface* o; wl_list_for_each(o, &surfaces, link) if (o->transient_for == surface) o->transient_for = NULL; }
    region_clear(&surface->input); region_clear(&surface->next_input);
    region_clear(&surface->opaque_region); region_clear(&surface->next_opaque); region_clear(&surface->cache.opaque_region); opaque_map_clear(&surface->omap);
    region_clear(&surface->cache.input); free(surface->cache.pixels); free(surface->pixels); free(surface->dock_thumb); free(surface->x11_icon); free(surface);
}
static void focus_surface(struct surface* surface);
static bool surface_origin(struct surface* surface,int64_t* x,int64_t* y);
static int window_bounds(struct surface* surface,struct popup_box* box);
static void popup_configure(struct surface* surface);
static void popup_scene(void);
static void selection_focus(void);
static struct wl_list* parked_windows(void);
static bool dnd_pointer(void); static bool dnd_button(unsigned code,bool down); static bool dnd_cursor(struct popup_box* box,struct surface** image);
static bool dnd_escape(void); static void dnd_surface_gone(struct surface* surface);   /* drag and drop (wl_data_device v3) */
/* wl_surface.enter/leave: mask holds the display slots the surface is shown on now. */
static void output_membership_mask(struct surface* surface,unsigned mask) {
    if(!surface->resource||mask==surface->entered)return;
    struct output* output;
    wl_list_for_each(output,&outputs,link)if(output->slot>=0&&wl_resource_get_client(output->resource)==wl_resource_get_client(surface->resource)){
        unsigned bit=1U<<output->slot;
        if((mask&bit)&&!(surface->entered&bit))wl_surface_send_enter(surface->resource,output->resource);
        else if(!(mask&bit)&&(surface->entered&bit))wl_surface_send_leave(surface->resource,output->resource);
    }
    surface->entered=mask;
    aurora_protocols_preferred_scale(surface->resource,aurora_host_surface_scale(surface->resource));
}
static unsigned screens_of(struct popup_box b) {
    unsigned mask=0;
    for(int i=0;i<AURORA_OUTPUTS;i++){struct aurora_output* o=aurora_output_slot(i);
        if(o&&o->enabled&&box_intersect(b,(struct popup_box){o->x,o->y,o->w,o->h},NULL))mask|=1U<<i;}
    return mask;
}
/* The display of a toplevel (set when placed, moved or fullscreened on one); else the one under the pointer. */
static struct aurora_output* surface_screen(struct surface* s) {
    struct aurora_output* o=s&&s->screen_set?aurora_output_slot(s->screen):NULL;
    if(o&&o->enabled&&o->mirror<0)return o;
    o=aurora_output_at(pointer_x,pointer_y);return o?o:aurora_output_primary();
}
/* Where maximized windows go: the display, less the menu bar and dock on the primary one (full: the whole display). */
static struct popup_box work_area(struct aurora_output* o,bool full) {
    struct popup_box a={o->x,o->y,o->w,o->h};
    if(!full&&aurora_mode&&o->primary){a.y+=UI_TOP_RESERVE;a.h-=UI_TOP_RESERVE+88;}
    return a;
}
static void screen_update(struct surface* s) {   /* the display holding the window's centre */
    struct popup_box b;int64_t cx=s->x,cy=s->y;
    if(window_bounds(s,&b)>0){cx+=b.x+b.w/2;cy+=b.y+b.h/2;}
    if(cx<INT32_MIN/2||cx>INT32_MAX/2||cy<INT32_MIN/2||cy>INT32_MAX/2)return;
    struct aurora_output* o=aurora_output_at((int)cx,(int)cy);if(!o)o=aurora_output_nearest((int)cx,(int)cy);
    s->screen=o->index;s->screen_set=true;
}
static const struct wl_surface_interface surface_interface;
/* The scale a surface should render at (1/120): the largest of the displays it is on, else its window's display. */
int aurora_host_surface_scale(struct wl_resource* resource){
    if(!resource||!wl_resource_instance_of(resource,&wl_surface_interface,&surface_interface))return 0;
    struct surface* s=wl_resource_get_user_data(resource);int best=0;
    for(int i=0;i<AURORA_OUTPUTS;i++){struct aurora_output* o=aurora_output_slot(i);if(o&&o->enabled&&(s->entered&(1U<<i))&&o->scale120>best)best=o->scale120;}
    return best?best:surface_screen(popup_family(s)?popup_family(s):s)->scale120;
}
static void output_membership(struct surface* surface,bool entered) {
    output_membership_mask(surface,entered?1U<<surface_screen(popup_family(surface))->index:0);   /* refined at the next paint */
}
static void pointer_update(void);
static struct surface* root_surface(struct surface* surface) {
    while (surface && surface->parent) surface = surface->parent;
    return surface;
}
static bool surface_visible(struct surface* surface) {
    for (struct surface* s = surface; s; s = s->parent?s->parent:s->popup_parent) {
        if (!s->resource || !s->pixels) return false;
        if (s->kind == ROLE_SUBSURFACE) { if (!s->subsurface || !s->parent) return false; }
        else if(s->kind==ROLE_POPUP){if(!s->role||!s->popup_parent||s->popup_dismissed)return false;}
        else if(s->kind==ROLE_IME_POPUP){int px,py;return aurora_ime_popup(&px,&py)==s->resource;}
        else return s->kind == ROLE_TOPLEVEL && s->role && !s->minimized && !s->parked && (!aurora_mode || s->workspace == wm_workspace);
    }
    return false;
}
static void refresh_scene(void) {
    popup_scene();
    struct surface* surface, *prefer = NULL;
    wl_list_for_each(surface, &surfaces, link) {
        bool visible = surface_visible(surface);
        if (visible == surface->mapped) continue;
        surface->mapped = visible; shell_dirty |= 1U << SHELL_DOCK;
        if (surface->resource) output_membership(surface, visible);
        if (!visible && focus == surface) { focus_surface(NULL); if (surface->transient_for && surface->transient_for != surface) prefer = surface->transient_for; }
        if (visible && surface->kind == ROLE_TOPLEVEL&&!popup_grab&&!shell_overlay()&&(!surface->x11||aurora_xwm_takes_focus(surface->x11))) focus_surface(surface);
    }
    if(popup_grab&&popup_grab->mapped)focus_surface(popup_grab);
    if(!focus&&!shell_overlay()&&!desktop_focus&&prefer&&prefer->mapped)focus_surface(prefer); /* a closed dialog returns focus to its parent */
    if(!focus&&!shell_overlay()&&!desktop_focus){
        if(aurora_mode){wl_list_for_each_reverse(surface,&windows,window_link)if(surface->mapped){focus_surface(surface);break;}}
        else wl_list_for_each_reverse(surface,&surfaces,link)if(surface->mapped&&surface->kind==ROLE_TOPLEVEL){focus_surface(surface);break;}
    }
    if (pointer_grab && !pointer_grab->mapped) pointer_grab = NULL;
    pointer_update(); dirty = true;
    if (aurora_mode) {
        aurora_wm_windows_changed();   /* an open switcher/overview drops windows that went away */
        wl_list_for_each(surface, &windows, window_link) {   /* the dock shows minimized windows as pictures taken when they went */
            if (surface->minimized && surface->pixels && surface->role && !surface->dock_thumb) {
                surface->dock_thumb = malloc(DOCK_THUMB * DOCK_THUMB * 4);
                if (surface->dock_thumb) aurora_host_thumbnail((struct aurora_window*)surface, surface->dock_thumb, DOCK_THUMB, DOCK_THUMB);
                shell_dirty |= 1U << SHELL_DOCK;
            } else if (!surface->minimized && surface->dock_thumb) { free(surface->dock_thumb); surface->dock_thumb = NULL; shell_dirty |= 1U << SHELL_DOCK; }
        }
    }
}
static void surface_destroy(struct wl_resource* resource) {
    struct surface* surface = wl_resource_get_user_data(resource);
    if (surface->x11 && surface->role) wl_resource_destroy(surface->role);   /* an X window's role ends with its surface */
    { struct surface* other; wl_list_for_each(other, &surfaces, link) if (other->transient_for == surface) other->transient_for = NULL; }
    if (focus == surface && surface->transient_for && surface->transient_for != surface && surface->transient_for->mapped) focus_surface(surface->transient_for);
    if (focus == surface) focus_surface(NULL);
    if (pointer_focus == surface) pointer_focus = NULL;
    if (pointer_grab == surface) pointer_grab = NULL;
    cancel_move(surface);
    if(button_origin==surface)button_origin=NULL;
    if(key_origin==surface)key_origin=NULL;
    {struct endpoint* e;wl_list_for_each(e,&endpoints,link)if(e->cursor==surface){e->cursor=NULL;cursor_dirty=true;}}
    dnd_surface_gone(surface);
    detach_buffer(surface); surface->resource = NULL; surface->mapped = false;
    while (!wl_list_empty(&surface->frames)) { struct frame* frame = wl_container_of(surface->frames.next,frame,link); wl_resource_destroy(frame->resource); }
    refresh_scene(); surface_release(surface);
}
static void surface_attach(struct wl_client* client, struct wl_resource* resource, struct wl_resource* buffer, int32_t x, int32_t y) {
    (void)client; struct surface* surface = wl_resource_get_user_data(resource);
    if ((x || y) && surface->kind != ROLE_SUBSURFACE) { wl_resource_post_error(resource,WL_SURFACE_ERROR_INVALID_OFFSET,"nonzero attach offset is unsupported"); return; }
    if (buffer && !aurora_buffer_supported(buffer)) { wl_resource_post_error(resource,WL_SURFACE_ERROR_INVALID_SIZE,"SHM or dma-buf buffer required"); return; }
    detach_buffer(surface); surface->attached = true; surface->buffer = buffer;
    if (buffer) wl_resource_add_destroy_listener(buffer,&surface->buffer_destroy);
}
static const struct popup_box damage_limit = {0, 0, INT32_MAX, INT32_MAX};
static void surface_damage(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; struct surface* s = wl_resource_get_user_data(r); rects_push(&s->damage_surface, (struct popup_box){x, y, w, h}, damage_limit);
}
static void surface_damage_buffer(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; struct surface* s = wl_resource_get_user_data(r); rects_push(&s->damage_buffer, (struct popup_box){x, y, w, h}, damage_limit);
}
static void surface_frame(struct wl_client* client, struct wl_resource* resource, uint32_t id) {
    struct surface* surface = wl_resource_get_user_data(resource); struct frame* frame = calloc(1,sizeof(*frame));
    struct wl_resource* callback = wl_resource_create(client,&wl_callback_interface,1,id);
    if (!frame || !callback) { free(frame); if (callback) wl_resource_destroy(callback); wl_resource_post_no_memory(resource); return; }
    frame->resource = callback; wl_list_insert(surface->frames.prev,&frame->link); wl_resource_set_implementation(callback,NULL,frame,frame_destroy);
}
static void surface_opaque(struct wl_client* c, struct wl_resource* resource, struct wl_resource* region) {
    (void)c; struct surface* surface = wl_resource_get_user_data(resource);
    if (region && !region_copy(&surface->next_opaque,wl_resource_get_user_data(region))) { wl_resource_post_no_memory(resource); return; }
    if (!region) region_clear(&surface->next_opaque);
    surface->next_opaque_changed = true;
}
static void surface_input(struct wl_client* c, struct wl_resource* resource, struct wl_resource* region) {
    (void)c; struct surface* surface = wl_resource_get_user_data(resource);
    if (region && !region_copy(&surface->next_input,wl_resource_get_user_data(region))) { wl_resource_post_no_memory(resource); return; }
    if (!region) region_clear(&surface->next_input);
    surface->input_changed = true; surface->next_input_set = region != NULL;
}
/* An X window's configure (aurora_xwm.c): the outer frame Aurora wants, for its state. */
static void x11_configure(struct surface* s) {
    if (!s->role || !s->resource) return;
    int w = s->requested_w, h = s->requested_h; unsigned flags = popup_family(focus) == s ? AURORA_XCONF_ACTIVATED : 0;
    if (s->fullscreen || s->maximized) { struct popup_box a = work_area(surface_screen(s), s->fullscreen);
        w = a.w; h = a.h; s->x = a.x; s->y = a.y; s->screen = surface_screen(s)->index; s->screen_set = true;
        flags |= s->fullscreen ? AURORA_XCONF_FULLSCREEN : AURORA_XCONF_MAXIMIZED; }
    else if (s->tile) { struct popup_box a = wm_tile_area(s); w = a.w; h = a.h; s->x = a.x; s->y = a.y; flags |= AURORA_XCONF_TILED; }
    aurora_xwm_configure(s->x11, s->x, s->y, w, h, flags);
}
static void configure(struct surface* surface) {
    if (surface->x11) { x11_configure(surface); return; }
    if (!surface->xdg || !surface->role || !surface->resource) return;   /* role objects outliving their wl_surface are inert */
    if(surface->kind==ROLE_POPUP){popup_configure(surface);return;}
    if(aurora_mode&&surface->configure_pending&&moving==surface&&resize_edges){surface->configure_deferred=true;return;} /* throttle interactive resizes only */
    int w=width,h=height;
    if(aurora_mode){w=surface->requested_w;h=surface->requested_h;
        if(surface->fullscreen||surface->maximized){struct popup_box a=work_area(surface_screen(surface),surface->fullscreen);
            w=a.w;h=a.h;surface->x=a.x;surface->y=a.y;surface->screen=surface_screen(surface)->index;surface->screen_set=true;}}
    if(aurora_mode&&surface->tile&&!surface->fullscreen&&!surface->maximized){struct popup_box a=wm_tile_area(surface);w=a.w;h=a.h;surface->x=a.x;surface->y=a.y;}
    printf("[64] DESKTOP CONFIGURE surface=%u %dx%d\n",wl_resource_get_id(surface->resource),w,h);fflush(stdout);
    uint32_t values[8];unsigned count=0,version=wl_resource_get_version(surface->role);
    if(surface->fullscreen)values[count++]=XDG_TOPLEVEL_STATE_FULLSCREEN;
    else if(!aurora_mode||surface->maximized)values[count++]=XDG_TOPLEVEL_STATE_MAXIMIZED;
    else if(surface->tile&&version>=2){   /* half-screen tiles: the outer edges and top/bottom touch the display */
        values[count++]=surface->tile==AURORA_TILE_LEFT?XDG_TOPLEVEL_STATE_TILED_LEFT:XDG_TOPLEVEL_STATE_TILED_RIGHT;
        values[count++]=XDG_TOPLEVEL_STATE_TILED_TOP;values[count++]=XDG_TOPLEVEL_STATE_TILED_BOTTOM;}
    if(aurora_mode&&moving==surface&&resize_edges)values[count++]=XDG_TOPLEVEL_STATE_RESIZING;
    if(!aurora_mode||popup_family(focus)==surface)values[count++]=XDG_TOPLEVEL_STATE_ACTIVATED;
    struct wl_array states = {.size=count*sizeof(*values),.alloc=sizeof(values),.data=values};
    if(version>=4&&aurora_mode){struct popup_box a=work_area(surface_screen(surface),false);xdg_toplevel_send_configure_bounds(surface->role,a.w,a.h);}
    xdg_toplevel_send_configure(surface->role,w,h,&states);
    surface->configure = wl_display_next_serial(display); if(!aurora_mode||!surface->configured)surface->acked = false;
    if(!surface->configure_pending)surface->configure_first=surface->configure;
    surface->configure_pending=aurora_mode;surface->configure_deferred=false;
    xdg_surface_send_configure(surface->xdg,surface->configure); surface->configured = true;
}
static bool effective_sync(struct surface* surface) {
    for (struct surface* s = surface; s && s->parent; s = s->parent)
        if (s->subsurface && s->synchronized) return true;
    return false;
}
static void list_reset(struct wl_list* link) { wl_list_remove(link); wl_list_init(link); }
static void snapshot_children(struct surface* surface) {
    while (!wl_list_empty(&surface->cached_children)) list_reset(surface->cached_children.next);
    struct wl_list* link;
    for (link = surface->pending_children.next; link != &surface->pending_children; link = link->next) {
        if (link == &surface->pending_plane) wl_list_insert(surface->cached_children.prev, &surface->cached_plane);
        else {
            struct surface* child = wl_container_of(link, child, pending_sibling);
            child->cached_x = child->next_x; child->cached_y = child->next_y;
            wl_list_insert(surface->cached_children.prev, &child->cached_sibling);
        }
    }
}
static bool cache_surface(struct surface* surface) {
    struct wl_resource* resource = surface->resource;
    int w = surface->cache.buffer_changed ? surface->cache.width : surface->width;
    int h = surface->cache.buffer_changed ? surface->cache.height : surface->height;
    struct aurora_view view; struct aurora_view* buffer = surface->buffer && aurora_view_get(surface->buffer, &view) ? &view : NULL;
    if (surface->attached) { w = buffer ? buffer->width : 0; h = buffer ? buffer->height : 0; }
    if (w % surface->next_scale || h % surface->next_scale) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SIZE, "buffer dimensions do not match scale"); return false;
    }
    if (!viewport_check(surface, w, h)) return false;
    if (surface->attached) {
        if (buffer) {
            int pitch = buffer->stride;
            uint32_t format = buffer->format;
            if (w <= 0 || h <= 0 || (uint64_t)w * 4 > (uint64_t)pitch || (uint64_t)w * h > SIZE_MAX / 4 ||
                (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)) {
                wl_resource_post_error(resource,WL_SURFACE_ERROR_INVALID_SIZE,"invalid software buffer"); return false;
            }
            uint64_t started = cpu_nanoseconds(CLOCK_THREAD_CPUTIME_ID); bool xrgb = format == WL_SHM_FORMAT_XRGB8888; int scale = surface->next_scale;
            /* Buffer-space damage of this commit, clipped to the buffer: surface damage is scaled, or mapped back through
             * the pending wp_viewport (rounded outward; whole for views over 65536 wide, where 16.16 sampling drifts more). */
            struct rects damage = {0}; struct popup_box whole = {0, 0, w, h};
            struct view next_view = view_compute(&surface->vp_next, w, h, scale); bool view_same = view_equal(&next_view, &surface->view);
            for (int i = 0; i < surface->damage_buffer.n; i++) rects_push(&damage, surface->damage_buffer.r[i], whole);
            for (int i = 0; i < surface->damage_surface.n; i++) {
                struct popup_box r = surface->damage_surface.r[i]; double v[4] = {r.x, r.y, (double)r.x + r.w, (double)r.y + r.h}, slack = 0;
                if (next_view.on && (next_view.w > 65536 || next_view.h > 65536)) { rects_push(&damage, whole, whole); continue; }
                if (next_view.on) { v[0] = next_view.sx + v[0] / next_view.kx; v[2] = next_view.sx + v[2] / next_view.kx;
                    v[1] = next_view.sy + v[1] / next_view.ky; v[3] = next_view.sy + v[3] / next_view.ky; slack = 1; }
                for (int k = 0; k < 4; k++) { double limit = k & 1 ? h : w; v[k] = v[k] * scale + (k < 2 ? -slack : slack); v[k] = v[k] < 0 ? 0 : v[k] > limit ? limit : v[k]; }
                int32_t x0 = (int32_t)floor64(v[0]), y0 = (int32_t)floor64(v[1]), x1 = (int32_t)-floor64(-v[2]), y1 = (int32_t)-floor64(-v[3]);
                rects_push(&damage, (struct popup_box){x0, y0, x1 - x0, y1 - y0}, whole);
            }
            /* GPU (dma-buf) buffers are read back for the damaged box; the whole buffer when it is copied entirely. */
            struct popup_box area = {0, 0, 0, 0};
            for (int i = 0; i < damage.n; i++) {
                struct popup_box r = damage.r[i]; if (r.w <= 0 || r.h <= 0) continue;
                if (area.w <= 0) { area = r; continue; }
                int32_t x1 = area.x + area.w > r.x + r.w ? area.x + area.w : r.x + r.w, y1 = area.y + area.h > r.y + r.h ? area.y + area.h : r.y + r.h;
                area.x = area.x < r.x ? area.x : r.x; area.y = area.y < r.y ? area.y : r.y; area.w = x1 - area.x; area.h = y1 - area.y;
            }
            if (effective_sync(surface) || !surface->pixels || surface->width != w || surface->height != h || (surface->cache.buffer_changed && !surface->cache.in_place)) area = whole;
            if (!aurora_view_begin(buffer, area.x, area.y, area.w, area.h)) { wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SIZE, "GPU buffer readback failed"); return false; }
            const uint8_t* source = buffer->data;
            if (!effective_sync(surface)) {
                /* Applied right away: copy damaged rectangles into the persistent visible store. */
                struct cached_state* cache = &surface->cache;
                if (cache->buffer_changed && !cache->in_place) {
                    free(surface->pixels); surface->pixels = cache->pixels; cache->pixels = NULL;
                    surface->width = cache->width; surface->height = cache->height; surface->opaque = cache->opaque;
                }
                bool fresh = !surface->pixels || surface->width != w || surface->height != h;
                if (fresh) {
                    uint32_t* store = malloc((size_t)w * h * 4);
                    if (!store) { aurora_view_end(buffer); wl_resource_post_no_memory(resource); return false; }
                    if (surface->resource && (surface->width != w || surface->height != h)) {
                        printf("[64] DESKTOP BUFFER surface=%u %dx%d\n", wl_resource_get_id(surface->resource), w, h);
                        if (surface->kind == ROLE_TOPLEVEL)   /* how the client renders for HiDPI */
                            printf("[64] DESKTOP SCALE surface=%u app=%s buffer=%dx%d buffer_scale=%d viewport=%dx%d\n", wl_resource_get_id(surface->resource), surface->app_id,
                                   w, h, surface->next_scale, surface->vp_next.destination ? surface->vp_next.dw : 0, surface->vp_next.destination ? surface->vp_next.dh : 0);
                        fflush(stdout);
                    }
                    free(surface->pixels); surface->pixels = store; surface->width = w; surface->height = h;
                    damage.n = 1; damage.r[0] = whole;
                }
                for (int i = 0; i < damage.n; i++) {
                    struct popup_box r = damage.r[i];
                    for (int32_t y = r.y; y < r.y + r.h; y++) {
                        uint32_t* row = surface->pixels + (size_t)y * w + r.x;
                        memcpy(row, source + (size_t)y * pitch + (size_t)r.x * 4, (size_t)r.w * 4);
                        if (xrgb) for (int32_t x = 0; x < r.w; x++) row[x] |= 0xFF000000;
                    }
                    stats.copy_bytes += (uint64_t)box_area(r) * 4;
                    rects_push(&surface->content_damage, view_same ? view_damage(&next_view, r, scale, w, h) : damage_limit, damage_limit);
                }
                free(cache->pixels); cache->pixels = NULL; cache->in_place = true;
            } else {
                /* Synchronized subsurface: keep a full private copy until the parent applies it. */
                uint32_t* pixels = surface->cache.pixels && !surface->cache.in_place && surface->cache.width == w && surface->cache.height == h ?
                    surface->cache.pixels : malloc((size_t)w * h * 4);
                if (!pixels) { aurora_view_end(buffer); wl_resource_post_no_memory(resource); return false; }
                for (int y = 0; y < h; y++) memcpy(pixels + (size_t)y*w,source + (size_t)y*pitch,(size_t)w*4);
                if (xrgb) for (size_t i = 0; i < (size_t)w*h; i++) pixels[i] |= 0xFF000000;
                if (pixels != surface->cache.pixels) free(surface->cache.pixels);
                surface->cache.pixels = pixels; surface->cache.in_place = false;
                stats.copy_bytes += (uint64_t)w * h * 4;
            }
            aurora_view_end(buffer);
            surface->cache.opaque = xrgb;
            wl_buffer_send_release(surface->buffer); detach_buffer(surface);
            if (stats_enabled && !stats.first_commit) { stats.first_commit = nanoseconds(); if (stats.frame_sent) { stats.client_ns += stats.first_commit - stats.frame_sent; stats.turns++; } }
            stats.copy_ns += cpu_nanoseconds(CLOCK_THREAD_CPUTIME_ID) - started; stats.commits++;
        } else { free(surface->cache.pixels); surface->cache.pixels = NULL; surface->cache.in_place = false; }
        surface->cache.width = w; surface->cache.height = h; surface->cache.buffer_changed = true;
        surface->attached = false;
    }
    rects_clear(&surface->damage_surface); rects_clear(&surface->damage_buffer);
    if (surface->next_opaque_changed) {
        region_clear(&surface->cache.opaque_region); surface->cache.opaque_region = surface->next_opaque;
        surface->next_opaque.rectangles = NULL; surface->cache.opaque_changed = true; surface->next_opaque_changed = false;
    }
    surface->cache.scale = surface->next_scale; surface->vp_cached = surface->vp_next;
    if(surface->geometry_changed){surface->cache.geometry=surface->next_geometry;surface->cache.geometry_changed=true;surface->geometry_changed=false;}
    if (surface->input_changed) {
        region_clear(&surface->cache.input); surface->cache.input = surface->next_input;
        surface->next_input.rectangles = NULL; surface->cache.input_set = surface->next_input_set;
        surface->cache.input_changed = true; surface->input_changed = false;
    }
    struct frame* frame;
    wl_list_for_each(frame, &surface->frames, link) if (frame->state == FRAME_PENDING) frame->state = FRAME_CACHED;
    snapshot_children(surface); surface->cache.valid = true; return true;
}
static void apply_cached(struct surface* surface) {
    struct cached_state* cache = &surface->cache;
    if (!cache->valid) return;
    if (cache->buffer_changed) {
        if (!cache->in_place) {
            if (surface->resource && cache->pixels && (surface->width != cache->width || surface->height != cache->height)) {
                printf("[64] DESKTOP BUFFER surface=%u %dx%d\n", wl_resource_get_id(surface->resource), cache->width, cache->height);
                if (surface->kind == ROLE_TOPLEVEL)   /* how the client renders for HiDPI: buffer scale or viewport destination */
                    printf("[64] DESKTOP SCALE surface=%u app=%s buffer=%dx%d buffer_scale=%d viewport=%dx%d\n", wl_resource_get_id(surface->resource), surface->app_id,
                           cache->width, cache->height, cache->scale, surface->vp_cached.destination ? surface->vp_cached.dw : 0, surface->vp_cached.destination ? surface->vp_cached.dh : 0);
                fflush(stdout);
            }
            free(surface->pixels); surface->pixels = cache->pixels; cache->pixels = NULL;
            rects_clear(&surface->content_damage); rects_push(&surface->content_damage, damage_limit, damage_limit);
        }
        surface->width = cache->width; surface->height = cache->height;
        surface->opaque = cache->opaque; cache->in_place = false;
    }
    surface->scale = cache->scale; surface->vp = surface->vp_cached;
    struct view view = surface->pixels ? view_compute(&surface->vp, surface->width, surface->height, surface->scale) : (struct view){0};
    if (!view_equal(&view, &surface->view)) { rects_clear(&surface->content_damage); rects_push(&surface->content_damage, damage_limit, damage_limit); }
    surface->view = view;
    if (cache->opaque_changed) {
        region_clear(&surface->opaque_region); surface->opaque_region = cache->opaque_region; cache->opaque_region.rectangles = NULL;
    }
    struct opaque_map* m = &surface->omap;
    if (cache->opaque_changed || m->opaque != surface->opaque || m->w != (surface->pixels ? surface->view.w : 0) ||
        m->h != (surface->pixels ? surface->view.h : 0)) {
        omap_build(surface); rects_clear(&surface->content_damage); rects_push(&surface->content_damage, damage_limit, damage_limit);
    }
    cache->opaque_changed = false;
    if(cache->geometry_changed){surface->geometry_x=cache->geometry.x;surface->geometry_y=cache->geometry.y;
        surface->geometry_w=cache->geometry.w;surface->geometry_h=cache->geometry.h;surface->geometry_clamp=true;}
    if (cache->input_changed) {
        region_clear(&surface->input); surface->input = cache->input; cache->input.rectangles = NULL;
        surface->input_set = cache->input_set;
    }
    /* Preserve newly created children above the earlier committed stacking snapshot. */
    struct wl_list* link = surface->cached_children.prev;
    while (link != &surface->cached_children) {
        struct wl_list* previous = link->prev;
        if (link == &surface->cached_plane) { list_reset(&surface->plane); wl_list_insert(&surface->children, &surface->plane); }
        else {
            struct surface* child = wl_container_of(link, child, cached_sibling);
            child->x = child->cached_x; child->y = child->cached_y;
            list_reset(&child->sibling); wl_list_insert(&surface->children, &child->sibling);
        }
        list_reset(link); link = previous;
    }
    struct frame* frame;
    wl_list_for_each(frame, &surface->frames, link) if (frame->state == FRAME_CACHED) frame->state = FRAME_READY;
    cache->valid = cache->buffer_changed = cache->input_changed = cache->geometry_changed = false;
}
static void apply_tree(struct surface* root) {
    apply_cached(root);
    struct surface* surface;
    wl_list_for_each(surface, &surfaces, link) {
        if (surface == root) continue;
        struct surface* child = surface;
        while (child->parent && child->parent != root) child = child->parent;
        if (child->parent == root && child->synchronized) apply_cached(surface);
    }
    wl_list_for_each(surface,&surfaces,link)if(surface->geometry_clamp){
        struct popup_box bounds;
        int ready=window_bounds(surface,&bounds);
        if(!ready)continue; /* Initial geometry may be committed before the first buffer exists. */
        if(ready<0){surface->geometry_clamp=false;if(surface->xdg)wl_resource_post_error(surface->xdg,XDG_SURFACE_ERROR_INVALID_SIZE,"geometry outside surface bounds");continue;}
        surface->geometry_clamp=false;
        surface->geometry_x=bounds.x;surface->geometry_y=bounds.y;surface->geometry_w=bounds.w;surface->geometry_h=bounds.h;
    }
    if(aurora_mode&&root->kind==ROLE_TOPLEVEL&&root->pixels&&!root->placed){
        root->placed=true;
        if(!root->maximized&&!root->fullscreen){
            struct popup_box box;if(window_bounds(root,&box)>0){   /* cascade on the display under the pointer (or the parent's) */
                struct aurora_output* screen=root->transient_for&&root->transient_for->screen_set?surface_screen(root->transient_for):surface_screen(NULL);
                struct popup_box a=work_area(screen,false);
                root->x=a.x+60+(int)(window_sequence++%6)*28;root->y=a.y+28+(int)(window_sequence%5)*24;
                if((int64_t)root->x+box.w>a.x+a.w)root->x=a.x+a.w-box.w;
                if((int64_t)root->y+box.h>a.y+a.h)root->y=a.y+a.h-box.h;
                if(root->x<a.x)root->x=a.x;if(root->y<a.y)root->y=a.y;
                root->screen=screen->index;root->screen_set=true;
                if(box.w>a.w||box.h>a.h){root->requested_w=a.w;root->requested_h=a.h;configure(root);}
            }
        }
        printf("[64] AURORA WINDOW app=%s title=%s x=%d y=%d\n",root->app_id,root->title,root->x,root->y);fflush(stdout);
    }
    refresh_scene();
}
static void surface_commit(struct wl_client* client, struct wl_resource* resource) {
    (void)client; struct surface* surface = wl_resource_get_user_data(resource);
    if(surface->kind==ROLE_POPUP&&surface->popup_dismissed)return;
    if(surface->xdg&&!surface->role){wl_resource_post_error(surface->xdg,XDG_SURFACE_ERROR_NOT_CONSTRUCTED,"missing surface role");return;}
    if (surface->xdg && !surface->configured) { configure(surface); if (surface->buffer) wl_resource_post_error(surface->xdg,XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,"buffer before configure"); return; }
    if (surface->xdg && surface->buffer && !surface->acked) { wl_resource_post_error(surface->xdg,XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,"configure not acknowledged"); return; }
    if(surface->kind==ROLE_POPUP&&surface->buffer&&!surface_visible(surface->popup_parent)){
        wl_resource_post_error(surface->shell->resource,XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,"unmapped popup parent");return;}
    bool unmap=surface->xdg&&surface->attached&&!surface->buffer&&surface->pixels;
    if(unmap)cancel_move(surface);
    aurora_constraints_commit(resource);
    if (cache_surface(surface) && !effective_sync(surface)) apply_tree(surface);
    if (boost_client && surface->resource && wl_resource_get_client(surface->resource) == boost_client && nanoseconds() < boost_until) paint_asap = true;
    if (surface->kind == ROLE_CURSOR) cursor_dirty = true;
    if(unmap){surface->configured=surface->acked=false;surface->configure_pending=surface->configure_deferred=false;surface->geometry_w=surface->geometry_h=0;surface->geometry_clamp=false;surface->popup_grabbed=false;
        if(surface->kind==ROLE_TOPLEVEL){surface->placed=surface->minimized=surface->maximized=surface->fullscreen=false;
            surface->x=surface->y=surface->saved_x=surface->saved_y=surface->saved_w=surface->saved_h=0;
            surface->requested_w=surface->requested_h=surface->min_w=surface->min_h=surface->max_w=surface->max_h=0;surface->title[0]=surface->app_id[0]=0;}
        refresh_scene();}
}
static void surface_transform(struct wl_client* c, struct wl_resource* r, int32_t transform) { (void)c; if (transform) wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_TRANSFORM,"software output currently requires normal transform"); }
static void surface_scale(struct wl_client* c, struct wl_resource* r, int32_t scale) { (void)c; if (scale < 1) wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_SCALE,"invalid scale"); else ((struct surface*)wl_resource_get_user_data(r))->next_scale = scale; }
static const struct wl_surface_interface surface_interface = {.destroy=destroy_request,.attach=surface_attach,.damage=surface_damage,.frame=surface_frame,.set_opaque_region=surface_opaque,.set_input_region=surface_input,.commit=surface_commit,.set_buffer_transform=surface_transform,.set_buffer_scale=surface_scale,.damage_buffer=surface_damage_buffer};
static void create_surface(struct wl_client* client, struct wl_resource* compositor, uint32_t id) {
    struct surface* surface = calloc(1,sizeof(*surface)); struct wl_resource* resource = wl_resource_create(client,&wl_surface_interface,wl_resource_get_version(compositor),id);
    if (!surface || !resource) { free(surface); if (resource) wl_resource_destroy(resource); wl_resource_post_no_memory(compositor); return; }
    surface->resource = resource; surface->references = 1; surface->scale = surface->next_scale = 1;
    wl_list_init(&surface->children); wl_list_init(&surface->pending_children); wl_list_init(&surface->cached_children);
    wl_list_init(&surface->cached_plane); wl_list_init(&surface->sibling); wl_list_init(&surface->pending_sibling); wl_list_init(&surface->cached_sibling);
    wl_list_init(&surface->popup_link);
    wl_list_init(&surface->window_link);
    wl_list_insert(&surface->children, &surface->plane); wl_list_insert(&surface->pending_children, &surface->pending_plane);
    wl_list_init(&surface->frames); wl_list_insert(surfaces.prev,&surface->link); surface->buffer_destroy.notify = buffer_destroyed;
    wl_resource_set_implementation(resource,&surface_interface,surface,surface_destroy);
    if (aurora_mode) aurora_xwm_surface_created(resource);   /* an Xwayland surface an X window named (WL_SURFACE_ID) */
}
static const struct wl_compositor_interface compositor_interface = {create_surface,create_region};
static void bind_compositor(struct wl_client* client, void* data, uint32_t version, uint32_t id) { (void)data; struct wl_resource* r=wl_resource_create(client,&wl_compositor_interface,version,id); if (!r) wl_client_post_no_memory(client); else wl_resource_set_implementation(r,&compositor_interface,NULL,NULL); }

static void subsurface_destroy(struct wl_resource* resource) {
    struct surface* surface = wl_resource_get_user_data(resource);
    struct surface* parent = surface->parent;
    list_reset(&surface->sibling); list_reset(&surface->pending_sibling); list_reset(&surface->cached_sibling);
    surface->parent = NULL; surface->subsurface = NULL;
    surface->x = surface->y = surface->next_x = surface->next_y = 0;
    region_clear(&surface->cache.input); region_clear(&surface->cache.opaque_region); free(surface->cache.pixels); memset(&surface->cache, 0, sizeof(surface->cache));
    while (!wl_list_empty(&surface->cached_children)) list_reset(surface->cached_children.next);
    struct frame *frame, *next;
    wl_list_for_each_safe(frame, next, &surface->frames, link)
        if (frame->state == FRAME_CACHED) wl_resource_destroy(frame->resource);
    refresh_scene(); surface_release(parent); surface_release(surface);
}
static void subsurface_position(struct wl_client* client, struct wl_resource* resource, int32_t x, int32_t y) {
    (void)client; struct surface* surface = wl_resource_get_user_data(resource);
    if (!surface->resource || !surface->parent->resource) return;
    surface->next_x = x; surface->next_y = y;
}
static void subsurface_stack(struct wl_resource* resource, struct wl_resource* sibling, bool above) {
    struct surface* surface = wl_resource_get_user_data(resource);
    if (!surface->resource || !surface->parent->resource) return;
    struct surface* other = wl_resource_get_user_data(sibling);
    if (other == surface || (other != surface->parent && (!other->subsurface || other->parent != surface->parent))) {
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE, "stack reference must be parent or sibling"); return;
    }
    struct wl_list* position = other == surface->parent ? &other->pending_plane : &other->pending_sibling;
    list_reset(&surface->pending_sibling); wl_list_insert(above ? position : position->prev, &surface->pending_sibling);
}
static void subsurface_above(struct wl_client* c, struct wl_resource* r, struct wl_resource* sibling) { (void)c; subsurface_stack(r, sibling, true); }
static void subsurface_below(struct wl_client* c, struct wl_resource* r, struct wl_resource* sibling) { (void)c; subsurface_stack(r, sibling, false); }
static void subsurface_sync(struct wl_client* client, struct wl_resource* resource) {
    (void)client; struct surface* surface = wl_resource_get_user_data(resource);
    if (surface->resource && surface->parent->resource) surface->synchronized = true;
}
static void subsurface_desync(struct wl_client* client, struct wl_resource* resource) {
    (void)client; struct surface* surface = wl_resource_get_user_data(resource);
    if (!surface->resource || !surface->parent->resource) return;
    surface->synchronized = false;
    if (!effective_sync(surface)) apply_tree(surface);
}
static const struct wl_subsurface_interface subsurface_interface = {
    destroy_request, subsurface_position, subsurface_above, subsurface_below, subsurface_sync, subsurface_desync
};
static void get_subsurface(struct wl_client* client, struct wl_resource* manager, uint32_t id,
                           struct wl_resource* resource, struct wl_resource* parent_resource) {
    struct surface* surface = wl_resource_get_user_data(resource);
    struct surface* parent = wl_resource_get_user_data(parent_resource);
    if (surface->xdg || surface->subsurface || (surface->kind != ROLE_NONE && surface->kind != ROLE_SUBSURFACE)) {
        wl_resource_post_error(manager, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "surface already has a role"); return;
    }
    for (struct surface* ancestor = parent; ancestor; ancestor = ancestor->parent) if (ancestor == surface) {
        wl_resource_post_error(manager, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT, "subsurface cycle"); return;
    }
    struct wl_resource* role = wl_resource_create(client, &wl_subsurface_interface, 1, id);
    if (!role) { wl_resource_post_no_memory(manager); return; }
    surface->parent = parent; surface->subsurface = role; surface->kind = ROLE_SUBSURFACE; surface->synchronized = true;
    surface->references++; parent->references++;
    wl_list_insert(parent->children.prev, &surface->sibling);
    wl_list_insert(parent->pending_children.prev, &surface->pending_sibling);
    wl_resource_set_implementation(role, &subsurface_interface, surface, subsurface_destroy); refresh_scene();
}
static const struct wl_subcompositor_interface subcompositor_interface = {destroy_request, get_subsurface};
static void bind_subcompositor(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* resource = wl_resource_create(client, &wl_subcompositor_interface, version, id);
    if (!resource) wl_client_post_no_memory(client);
    else wl_resource_set_implementation(resource, &subcompositor_interface, NULL, NULL);
}

static struct surface* popup_family(struct surface* s) {
    while(s&&(s->parent||s->popup_parent))s=s->parent?s->parent:s->popup_parent;return s;
}
static int window_bounds(struct surface* root,struct popup_box* box) {
    if(root->geometry_w&&!root->geometry_clamp){*box=(struct popup_box){root->geometry_x,root->geometry_y,root->geometry_w,root->geometry_h};return true;}
    bool found=false;int64_t left=0,top=0,right=0,bottom=0;struct surface* s;
    wl_list_for_each(s,&surfaces,link){
        if(!s->pixels)continue;int64_t x=0,y=0;struct surface* p=s;
        while(p!=root&&p->parent){if(__builtin_add_overflow(x,p->x,&x)||__builtin_add_overflow(y,p->y,&y))return -1;p=p->parent;}
        if(p!=root)continue;int64_t r,b;
        if(__builtin_add_overflow(x,s->view.w,&r)||__builtin_add_overflow(y,s->view.h,&b))return -1;
        if(!found||x<left)left=x;if(!found||y<top)top=y;if(!found||r>right)right=r;if(!found||b>bottom)bottom=b;found=true;
    }
    if(!found)return false;
    if(root->geometry_w){int64_t r=(int64_t)root->geometry_x+root->geometry_w,b=(int64_t)root->geometry_y+root->geometry_h;
        if(left<root->geometry_x)left=root->geometry_x;if(top<root->geometry_y)top=root->geometry_y;if(right>r)right=r;if(bottom>b)bottom=b;}
    if(left<INT32_MIN||left>INT32_MAX||top<INT32_MIN||top>INT32_MAX||right<=left||bottom<=top||right-left>INT32_MAX||bottom-top>INT32_MAX)return -1;
    *box=(struct popup_box){left,top,right-left,bottom-top};return true;
}
static struct surface* topmost_popup(struct surface* surface) {
    struct surface *s,*family=popup_family(surface);
    wl_list_for_each_reverse(s,&popups,popup_link)if(s->role&&popup_family(s)==family)return s;
    return NULL;
}
static void popup_done(struct surface* s) {
    if(s->popup_dismissed)return;s->popup_dismissed=true;
    printf("[64] DESKTOP POPUP DONE surface=%u\n",s->resource?wl_resource_get_id(s->resource):0);fflush(stdout);
    if(s->role)xdg_popup_send_popup_done(s->role);
}
static void popup_scene(void) {
    struct surface* s;popup_grab=NULL;
    wl_list_for_each_reverse(s,&popups,popup_link){
        bool invalid=false;
        for(struct surface* p=s->popup_parent;p;p=p->popup_parent)
            if(!p->resource||!p->role||p->popup_dismissed||(s->mapped&&!surface_visible(p))){invalid=true;break;}
        if(invalid)popup_done(s);
        if(!popup_grab&&s->role&&s->popup_grabbed&&!s->popup_dismissed)popup_grab=s;
    }
    struct surface* restore=focus;
    while(restore&&restore->kind==ROLE_POPUP&&(!restore->role||restore->popup_dismissed||!surface_visible(restore)))restore=restore->popup_parent;
    if(restore!=focus)focus_surface(restore&&surface_visible(restore)?restore:NULL);
}
static void popup_dismiss(struct surface* target) {
    struct surface* s;
    wl_list_for_each_reverse(s,&popups,popup_link){
        for(struct surface* p=s;p;p=p->popup_parent)if(p==target){popup_done(s);break;}
    }
    refresh_scene();
}
static void popup_configure(struct surface* s) {
    struct surface* parent=s->popup_parent;struct popup_box geometry;int64_t x,y,lx,ly,hx,hy;
    if(!parent||!parent->resource||!parent->role||window_bounds(parent,&geometry)<=0){
        wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,"popup parent has no geometry");return;}
    const struct popup_rules* p=&s->positioner;struct aurora_output* screen=surface_screen(popup_family(parent));   /* popups stay on their window's display */
    int64_t left=aurora_mode?screen->x:0,top=aurora_mode?screen->y:0,right=aurora_mode?(int64_t)screen->x+screen->w:(int64_t)width,bottom=aurora_mode?(int64_t)screen->y+screen->h:(int64_t)height;
    if(p->x<0||p->y<0||(int64_t)p->x+p->rw>geometry.w||(int64_t)p->y+p->rh>geometry.h||
        !surface_origin(parent,&x,&y)||__builtin_add_overflow(x,geometry.x,&x)||__builtin_add_overflow(y,geometry.y,&y)||
        __builtin_sub_overflow(left,x,&lx)||__builtin_sub_overflow(top,y,&ly)||
        __builtin_sub_overflow(right,x,&hx)||__builtin_sub_overflow(bottom,y,&hy)||
        !popup_place(p,lx,ly,hx,hy,&s->popup_box)){
        wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_INVALID_POSITIONER,"invalid popup placement");return;}
    xdg_popup_send_configure(s->role,s->popup_box.x,s->popup_box.y,s->popup_box.w,s->popup_box.h);
    printf("[64] DESKTOP POPUP surface=%u x=%d y=%d w=%d h=%d\n",wl_resource_get_id(s->resource),s->popup_box.x,s->popup_box.y,s->popup_box.w,s->popup_box.h);fflush(stdout);
    s->configure=wl_display_next_serial(display);s->acked=false;s->configured=true;xdg_surface_send_configure(s->xdg,s->configure);
}
static void popup_destroy(struct wl_resource* resource) {
    struct surface* s=wl_resource_get_user_data(resource),*parent=s->popup_parent;
    s->role=NULL;s->popup_dismissed=true;list_reset(&s->popup_link);refresh_scene();
    s->popup_parent=NULL;if(parent)surface_release(parent);surface_release(s);
}
static void popup_destroy_request(struct wl_client* client,struct wl_resource* resource) {
    (void)client;struct surface* s=wl_resource_get_user_data(resource);
    if(topmost_popup(s)!=s){wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,"destroy nested popups first");return;}
    wl_resource_destroy(resource);
}
static void popup_grab_request(struct wl_client* client,struct wl_resource* resource,struct wl_resource* seat,uint32_t serial) {
    struct surface* s=wl_resource_get_user_data(resource),*parent=s->popup_parent;
    if(s->mapped||s->popup_grabbed){wl_resource_post_error(resource,XDG_POPUP_ERROR_INVALID_GRAB,"grab must precede mapping");return;}
    if(parent->kind==ROLE_POPUP&&!parent->popup_grabbed){wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,"parent popup has no grab");return;}
    if(parent->popup_dismissed){popup_dismiss(s);return;}
    if(topmost_popup(s)!=s||(popup_grab&&popup_grab!=parent)){
        wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,"popup is not above the active grab");return;}
    struct surface* origin=serial==button_serial?button_origin:serial==key_serial?key_origin:NULL;
    if(!origin||!origin->mapped||!origin->resource||!focus||!focus->resource||parent->popup_dismissed||
        wl_resource_get_client(origin->resource)!=client||wl_resource_get_client(focus->resource)!=client||
        wl_resource_get_client(seat)!=client||popup_family(origin)!=popup_family(s)){
        popup_dismiss(s);return;}
    s->popup_grabbed=true;pointer_grab=NULL;refresh_scene();
}
static void popup_reposition_request(struct wl_client* c,struct wl_resource* r,struct wl_resource* positioner,uint32_t token){
    (void)c;struct surface* s=wl_resource_get_user_data(r);const struct popup_rules* rules=wl_resource_get_user_data(positioner);
    if(rules->w<=0||rules->h<=0||rules->rw<=0||rules->rh<=0){wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_INVALID_POSITIONER,"incomplete positioner");return;}
    if(!s->role||s->popup_dismissed)return;
    s->positioner=*rules;xdg_popup_send_repositioned(s->role,token);popup_configure(s);   /* repositioned, then the new configure */
}
/* Reactive popups (xdg_positioner.set_reactive) of a window that moved get a new placement. */
static void popups_reconstrain(struct surface* root){
    struct surface* s;wl_list_for_each(s,&popups,popup_link)if(s->role&&!s->popup_dismissed&&s->positioner.reactive&&popup_family(s)==root)popup_configure(s);
}
static const struct xdg_popup_interface popup_interface={.destroy=popup_destroy_request,.grab=popup_grab_request,.reposition=popup_reposition_request};
static void positioner_destroy(struct wl_resource* resource){free(wl_resource_get_user_data(resource));}
static void positioner_size(struct wl_client* c,struct wl_resource* r,int32_t w,int32_t h){
    (void)c;if(w<=0||h<=0){wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid popup size");return;}
    struct popup_rules* p=wl_resource_get_user_data(r);p->w=w;p->h=h;
}
static void positioner_rect(struct wl_client* c,struct wl_resource* r,int32_t x,int32_t y,int32_t w,int32_t h){
    (void)c;if(w<0||h<0){wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid anchor rectangle");return;}
    struct popup_rules* p=wl_resource_get_user_data(r);p->x=x;p->y=y;p->rw=w;p->rh=h;
}
static void positioner_anchor(struct wl_client* c,struct wl_resource* r,uint32_t value){(void)c;if(value>8)wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid anchor");else((struct popup_rules*)wl_resource_get_user_data(r))->anchor=value;}
static void positioner_gravity(struct wl_client* c,struct wl_resource* r,uint32_t value){(void)c;if(value>8)wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid gravity");else((struct popup_rules*)wl_resource_get_user_data(r))->gravity=value;}
static void positioner_adjust(struct wl_client* c,struct wl_resource* r,uint32_t value){(void)c;if(value&~63u)wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid constraints");else((struct popup_rules*)wl_resource_get_user_data(r))->adjust=value;}
static void positioner_offset(struct wl_client* c,struct wl_resource* r,int32_t x,int32_t y){(void)c;struct popup_rules* p=wl_resource_get_user_data(r);p->ox=x;p->oy=y;}
/* v3: reactive popups are re-placed when their parent moves; the parent size/configure hints describe a parent the client
 * is about to commit: placement here always uses the parent's current geometry, which the protocol permits. */
static void positioner_reactive(struct wl_client* c,struct wl_resource* r){(void)c;((struct popup_rules*)wl_resource_get_user_data(r))->reactive=true;}
static void positioner_parent_size(struct wl_client* c,struct wl_resource* r,int32_t w,int32_t h){(void)c;if(w<0||h<0)wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid parent size");}
static void positioner_parent_configure(struct wl_client* c,struct wl_resource* r,uint32_t serial){(void)c;(void)r;(void)serial;}
static const struct xdg_positioner_interface positioner_interface={.destroy=destroy_request,.set_size=positioner_size,.set_anchor_rect=positioner_rect,.set_anchor=positioner_anchor,.set_gravity=positioner_gravity,.set_constraint_adjustment=positioner_adjust,.set_offset=positioner_offset,
    .set_reactive=positioner_reactive,.set_parent_size=positioner_parent_size,.set_parent_configure=positioner_parent_configure};

static void role_destroy(struct wl_resource* r) { struct surface* s=wl_resource_get_user_data(r),*o;cancel_move(s);list_reset(&s->window_link);s->role=NULL;refresh_scene();
    wl_list_for_each(o,&surfaces,link)if(o->transient_for==s)o->transient_for=s->transient_for;s->transient_for=NULL; /* its children follow its parent */
    surface_release(s); }
static void top_parent(struct wl_client* c,struct wl_resource* r,struct wl_resource* parent) {
    (void)c;struct surface* s=wl_resource_get_user_data(r),*p=parent?wl_resource_get_user_data(parent):NULL;
    if(p&&(!p->resource||p->role!=parent))p=NULL; /* a parent whose wl_surface is gone counts as no parent */
    for(struct surface* q=p;q;q=q->transient_for)if(q==s){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_PARENT,"parent loop");return;}
    s->transient_for=p;
}
static void top_title(struct wl_client* c,struct wl_resource* r,const char* text) { (void)c;struct surface* s=wl_resource_get_user_data(r);char old[sizeof(s->title)];memcpy(old,s->title,sizeof(old));snprintf(s->title,sizeof(s->title),"%s",text);
    if(aurora_mode&&s->placed&&strcmp(old,s->title)){printf("[64] AURORA TITLE app=%s title=%s x=%d y=%d\n",s->app_id,s->title,s->x,s->y);fflush(stdout);} /* title changes of mapped windows (tests) */
    shell_dirty|=1U<<SHELL_MENUBAR;dirty=true; }
static void top_app_id(struct wl_client* c,struct wl_resource* r,const char* text) { (void)c;struct surface* s=wl_resource_get_user_data(r);snprintf(s->app_id,sizeof(s->app_id),"%s",text);shell_dirty|=(1U<<SHELL_MENUBAR)|(1U<<SHELL_DOCK);dirty=true; }
static bool begin_move(struct wl_client* c,struct wl_resource* r,struct wl_resource* seat,uint32_t serial) {
    struct surface* s=wl_resource_get_user_data(r);struct popup_box box;
    if(!aurora_mode||!seat||wl_resource_get_client(seat)!=c||serial!=button_serial||!(pointer_buttons&1)||
        popup_family(button_origin)!=s||popup_grab||s->fullscreen||window_bounds(s,&box)<=0)return false;
    if(s->maximized||s->tile){   /* dragging a maximized or tiled window takes it back to its own size, under the pointer */
        int old_w=box.w,grab=pointer_x-(s->x+box.x);s->maximized=false;s->tile=0;restore_geometry(s);
        s->x=pointer_x-(int)((int64_t)grab*s->requested_w/(old_w?old_w:1))-box.x;s->y=pointer_y-14-box.y;
        box.w=s->requested_w;box.h=s->requested_h;if(s->configured)configure(s);
    }
    moving=s;resize_edges=0;move_x=s->x;move_y=s->y;move_w=box.w;move_h=box.h;move_px=pointer_x;move_py=pointer_y;
    pointer_grab=NULL;button_origin=NULL;button_serial=0;swallowed_buttons|=pointer_buttons&~1U;pointer_update();
    return true;
}
static void top_move(struct wl_client* c,struct wl_resource* r,struct wl_resource* seat,uint32_t serial) { (void)begin_move(c,r,seat,serial); }
static void top_menu(struct wl_client* c,struct wl_resource* r,struct wl_resource* seat,uint32_t serial,int32_t x,int32_t y) {
    (void)c;(void)r;(void)seat;(void)serial;(void)x;(void)y;
    /* shortcut: window commands live in Aurora's menu bar; add a pointer menu when needed. */
}
static void top_resize(struct wl_client* c,struct wl_resource* r,struct wl_resource* seat,uint32_t serial,uint32_t edges) {
    if(!edges)return;
    if(edges!=1&&edges!=2&&edges!=4&&edges!=5&&edges!=6&&edges!=8&&edges!=9&&edges!=10){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,"invalid resize edge");return;}
    if(begin_move(c,r,seat,serial)){resize_edges=edges;configure(moving);}
}
static void top_max_size(struct wl_client* c,struct wl_resource* r,int32_t w,int32_t h) { (void)c;struct surface* s=wl_resource_get_user_data(r);if(w<0||h<0){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"negative size constraint");return;}s->max_w=w;s->max_h=h; }
static void top_min_size(struct wl_client* c,struct wl_resource* r,int32_t w,int32_t h) { (void)c;struct surface* s=wl_resource_get_user_data(r);if(w<0||h<0){wl_resource_post_error(r,XDG_TOPLEVEL_ERROR_INVALID_SIZE,"negative size constraint");return;}s->min_w=w;s->min_h=h; }
static void save_window(struct surface* s){
    if(s->maximized||s->fullscreen)return;struct popup_box box;
    s->saved_x=s->x;s->saved_y=s->y;s->saved_w=s->requested_w;s->saved_h=s->requested_h;
    if(!s->x11&&window_bounds(s,&box)>0){s->saved_w=box.w;s->saved_h=box.h;}   /* X windows: the frame size is Aurora's (Xwayland's buffer may lag) */
}
static void restore_geometry(struct surface* s){
    struct aurora_output* screen=surface_screen(s);struct popup_box a=work_area(screen,false);
    s->requested_w=s->saved_w>0?s->saved_w:a.w*3/4;s->requested_h=s->saved_h>0?s->saved_h:a.h*3/4;
    if(s->requested_w>a.w)s->requested_w=a.w;if(s->requested_h>a.h)s->requested_h=a.h;
    s->x=s->saved_x;s->y=s->saved_y;
    if(!aurora_output_at(s->x+s->requested_w/2,s->y+14)||aurora_output_at(s->x+s->requested_w/2,s->y+14)!=screen){   /* saved on another display: centre here */
        s->x=a.x+(a.w-s->requested_w)/2;s->y=a.y+(a.h-s->requested_h)/2;}
    if(s->x<a.x+120-s->requested_w)s->x=a.x+120-s->requested_w;if(s->x>a.x+a.w-120)s->x=a.x+a.w-120;
    if(s->y<a.y)s->y=a.y;if(s->y>a.y+a.h-28)s->y=a.y+a.h-28;
}
static void top_maximize(struct wl_client* c,struct wl_resource* r) { (void)c;struct surface* s=wl_resource_get_user_data(r);cancel_move(s);if(aurora_mode){save_window(s);s->maximized=true;}else s->fullscreen=false;if(s->configured)configure(s); }
static void top_restore(struct wl_client* c,struct wl_resource* r) { (void)c;struct surface* s=wl_resource_get_user_data(r);cancel_move(s);if(aurora_mode&&(s->maximized||s->fullscreen))restore_geometry(s);s->maximized=s->fullscreen=false;if(s->configured)configure(s); }
static void top_unmaximize(struct wl_client* c,struct wl_resource* r){struct surface* s=wl_resource_get_user_data(r);if(aurora_mode&&s->fullscreen){s->maximized=false;if(s->configured)configure(s);}else top_restore(c,r);}
static void top_fullscreen(struct wl_client* c,struct wl_resource* r,struct wl_resource* output) { (void)c;struct surface* s=wl_resource_get_user_data(r);cancel_move(s);if(aurora_mode)save_window(s);s->fullscreen=true;
    if(output&&wl_resource_instance_of(output,&wl_output_interface,&output_interface)){struct output* o=wl_resource_get_user_data(output);   /* the requested display */
        struct aurora_output* d=o?aurora_output_slot(o->slot):NULL;if(d&&d->enabled&&d->mirror<0){s->screen=d->index;s->screen_set=true;}}
    if(s->configured)configure(s); }
static void top_unfullscreen(struct wl_client* c,struct wl_resource* r){struct surface* s=wl_resource_get_user_data(r);if(aurora_mode&&s->maximized){s->fullscreen=false;if(s->configured)configure(s);}else top_restore(c,r);}
static void top_minimize(struct wl_client* c,struct wl_resource* r) { (void)c;struct surface* s=wl_resource_get_user_data(r);if(aurora_mode){cancel_move(s);s->minimized=true;refresh_scene();printf("[64] AURORA MINIMIZE app=%s\n",s->app_id);fflush(stdout);} }
static const struct xdg_toplevel_interface toplevel_interface = {destroy_request,top_parent,top_title,top_app_id,top_menu,top_move,top_resize,top_max_size,top_min_size,top_maximize,top_unmaximize,top_fullscreen,top_unfullscreen,top_minimize};
static void toplevel_close(struct surface* s){if(s->x11)aurora_xwm_close(s->x11);else if(s->role)xdg_toplevel_send_close(s->role);}   /* X windows: WM_DELETE_WINDOW */
static void get_toplevel(struct wl_client* client,struct wl_resource* resource,uint32_t id) {
    struct surface* s=wl_resource_get_user_data(resource);
    if(!s->resource||s->role||s->kind!=ROLE_NONE){wl_resource_post_error(resource,XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,"surface role exists or was destroyed");return;}
    s->role=wl_resource_create(client,&xdg_toplevel_interface,wl_resource_get_version(resource),id);if(!s->role){wl_resource_post_no_memory(resource);return;}
    s->kind=ROLE_TOPLEVEL;s->references++;wl_list_insert(windows.prev,&s->window_link);wl_resource_set_implementation(s->role,&toplevel_interface,s,role_destroy);
    s->workspace=wm_workspace;
    if(aurora_mode&&!aurora_session_client_visible(client)){wl_list_remove(&s->window_link);wl_list_insert(parked_windows()->prev,&s->window_link);s->parked=true;}   /* a background session's window */
    if(wl_resource_get_version(s->role)>=5){   /* what the window manager offers (GTK shows only those title-bar buttons) */
        uint32_t caps[3]={XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE,XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN,XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE};
        struct wl_array list={.size=sizeof(caps),.alloc=sizeof(caps),.data=caps};xdg_toplevel_send_wm_capabilities(s->role,&list);}
}
static void get_popup(struct wl_client* c,struct wl_resource* r,uint32_t id,struct wl_resource* parent,struct wl_resource* positioner) {
    struct surface* s=wl_resource_get_user_data(r);struct popup_rules* rules=wl_resource_get_user_data(positioner);
    if(!s->resource||s->role||s->kind!=ROLE_NONE){wl_resource_post_error(r,XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,"surface already has role");return;}
    struct surface* p=parent?wl_resource_get_user_data(parent):NULL;
    if(!p||!p->resource||!p->role||(p->kind!=ROLE_TOPLEVEL&&p->kind!=ROLE_POPUP)){
        wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,"popup requires an xdg parent");return;}
    if(rules->w<=0||rules->h<=0||rules->rw<=0||rules->rh<=0){wl_resource_post_error(s->shell->resource,XDG_WM_BASE_ERROR_INVALID_POSITIONER,"incomplete positioner");return;}
    s->role=wl_resource_create(c,&xdg_popup_interface,wl_resource_get_version(r),id);if(!s->role){wl_resource_post_no_memory(r);return;}
    s->kind=ROLE_POPUP;s->popup_parent=p;p->references++;s->references++;s->positioner=*rules;
    wl_list_insert(popups.prev,&s->popup_link);wl_resource_set_implementation(s->role,&popup_interface,s,popup_destroy);
}
static void window_geometry(struct wl_client* c,struct wl_resource* r,int32_t x,int32_t y,int32_t w,int32_t h) {
    (void)c;struct surface* s=wl_resource_get_user_data(r);if(w<=0||h<=0){wl_resource_post_error(r,XDG_SURFACE_ERROR_INVALID_SIZE,"invalid geometry");return;}
    s->next_geometry=(struct popup_box){x,y,w,h};s->geometry_changed=true;
}
static void ack_configure(struct wl_client* c,struct wl_resource* r,uint32_t serial) { (void)c;struct surface* s=wl_resource_get_user_data(r);/* any configure sent since the last acknowledgement may be acked (a client may skip superseded ones) */
    bool sent=serial==s->configure||(s->configure_pending&&s->configure_first&&serial-s->configure_first<0x80000000U&&s->configure-serial<0x80000000U);
    if(!s->configured||!sent){wl_resource_post_error(r,XDG_SURFACE_ERROR_INVALID_SERIAL,"unknown configure serial");return;}
    s->acked=true;if(serial==s->configure){s->configure_pending=false;if(s->configure_deferred)configure(s);}else s->configure_first=serial+1; }
static void shell_release(struct shell_binding* shell){if(!--shell->references)free(shell);}
static void xdg_destroy_request(struct wl_client* c,struct wl_resource* r){(void)c;struct surface* s=wl_resource_get_user_data(r);if(s->role)wl_resource_post_error(r,XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,"destroy role before xdg_surface");else wl_resource_destroy(r);}
static const struct xdg_surface_interface xdg_interface = {xdg_destroy_request,get_toplevel,get_popup,window_geometry,ack_configure};
static void xdg_destroy(struct wl_resource* r) { struct surface* s=wl_resource_get_user_data(r);s->xdg=NULL;shell_release(s->shell);s->shell=NULL;surface_release(s); }
static void get_xdg_surface(struct wl_client* client,struct wl_resource* base,uint32_t id,struct wl_resource* surface) {
    struct surface* s=wl_resource_get_user_data(surface);
    if(s->xdg||s->kind!=ROLE_NONE||s->pixels||s->buffer){wl_resource_post_error(base,XDG_WM_BASE_ERROR_ROLE,"surface already has role or buffer");return;}
    s->xdg=wl_resource_create(client,&xdg_surface_interface,wl_resource_get_version(base),id);if(!s->xdg){wl_resource_post_no_memory(base);return;}
    s->shell=wl_resource_get_user_data(base);s->shell->references++;s->references++;wl_resource_set_implementation(s->xdg,&xdg_interface,s,xdg_destroy);
}
static void create_positioner(struct wl_client* c,struct wl_resource* r,uint32_t id) {
    struct popup_rules* p=calloc(1,sizeof(*p));struct wl_resource* positioner=wl_resource_create(c,&xdg_positioner_interface,wl_resource_get_version(r),id);
    if(!p||!positioner){free(p);if(positioner)wl_resource_destroy(positioner);wl_resource_post_no_memory(r);return;}
    wl_resource_set_implementation(positioner,&positioner_interface,p,positioner_destroy);
}
static void pong(struct wl_client* c,struct wl_resource* r,uint32_t serial) { (void)c;(void)r;(void)serial; }
static void shell_destroy(struct wl_resource* r){struct shell_binding* s=wl_resource_get_user_data(r);s->resource=NULL;shell_release(s);}
static void shell_destroy_request(struct wl_client* c,struct wl_resource* r){(void)c;struct shell_binding* s=wl_resource_get_user_data(r);if(s->references>1)wl_resource_post_error(r,XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,"destroy xdg surfaces before shell");else wl_resource_destroy(r);}
static const struct xdg_wm_base_interface shell_interface = {shell_destroy_request,create_positioner,get_xdg_surface,pong};
static void bind_shell(struct wl_client* c,void* data,uint32_t version,uint32_t id) {
    (void)data;struct shell_binding* s=calloc(1,sizeof(*s));struct wl_resource* r=wl_resource_create(c,&xdg_wm_base_interface,version,id);
    if(!s||!r){free(s);if(r)wl_resource_destroy(r);wl_client_post_no_memory(c);return;}
    s->resource=r;s->references=1;wl_resource_set_implementation(r,&shell_interface,s,shell_destroy);
}
static const struct wl_output_interface output_interface = {destroy_request};
static void output_destroy(struct wl_resource* resource) { struct output* output=wl_resource_get_user_data(resource);wl_list_remove(&output->link);free(output); }
/* wl_output state of one display: position, physical size, make/model, current mode, scale, name/description (v4). */
static void output_send(struct wl_resource* r,const struct aurora_output* d) {
    uint32_t version=wl_resource_get_version(r);bool preferred=false;
    for(int k=0;k<d->mode_count;k++)if(d->modes[k].w==d->mode_w&&d->modes[k].h==d->mode_h&&d->modes[k].preferred)preferred=true;
    wl_output_send_geometry(r,d->x,d->y,d->mm_w,d->mm_h,WL_OUTPUT_SUBPIXEL_UNKNOWN,d->make[0]?d->make:"NexusOS",d->model[0]?d->model:d->name,WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r,WL_OUTPUT_MODE_CURRENT|(preferred?WL_OUTPUT_MODE_PREFERRED:0),d->mode_w,d->mode_h,d->refresh*1000);
    if(version>=2)wl_output_send_scale(r,d->scale);
    if(version>=4){char text[96];wl_output_send_name(r,d->name);snprintf(text,sizeof(text),"%s %s (%s)",d->make[0]?d->make:"",d->model,d->name);wl_output_send_description(r,text);}
    if(version>=2)wl_output_send_done(r);
}
static void bind_output(struct wl_client* c,void* data,uint32_t version,uint32_t id) {
    int slot=(int)(intptr_t)data;struct output* output=calloc(1,sizeof(*output));struct wl_resource* r=wl_resource_create(c,&wl_output_interface,version,id);if(!output||!r){free(output);if(r)wl_resource_destroy(r);wl_client_post_no_memory(c);return;}
    output->resource=r;output->slot=slot;wl_list_insert(outputs.prev,&output->link);
    wl_resource_set_implementation(r,&output_interface,output,output_destroy);
    struct aurora_output* d=aurora_output_slot(slot);
    if(!d||!d->enabled){output->slot=-1;return;}   /* a display that was removed while the client bound it: inert */
    output_send(r,d);
    struct surface* surface;wl_list_for_each(surface,&surfaces,link)if(surface->resource&&surface->mapped&&(surface->entered&(1U<<slot))&&wl_resource_get_client(surface->resource)==c)wl_surface_send_enter(surface->resource,r);
}
/* After the displays changed: wl_output globals per enabled display, current state to every bound resource. */
static struct wl_global* output_globals[AURORA_OUTPUTS];static unsigned output_sent[AURORA_OUTPUTS];
static struct wl_global* removed_globals[AURORA_OUTPUTS];
static void outputs_sync(void) {
    for(int i=0;i<AURORA_OUTPUTS;i++)if(removed_globals[i]){wl_global_destroy(removed_globals[i]);removed_globals[i]=NULL;}   /* removed at an earlier change */
    for(int i=0;i<AURORA_OUTPUTS;i++){
        struct aurora_output* d=aurora_output_slot(i);bool want=d&&d->enabled;
        if(output_globals[i]&&!want){   /* gone: clients get global_remove; bound objects turn inert */
            wl_global_remove(output_globals[i]);removed_globals[i]=output_globals[i];output_globals[i]=NULL;
            struct output* o;wl_list_for_each(o,&outputs,link)if(o->slot==i)o->slot=-1;
        }
        if(want&&!output_globals[i]){output_globals[i]=wl_global_create(display,&wl_output_interface,4,(void*)(intptr_t)i,bind_output);output_sent[i]=d->serial_number;}
        if(want&&output_sent[i]!=d->serial_number){output_sent[i]=d->serial_number;struct output* o;
            wl_list_for_each(o,&outputs,link)if(o->slot==i){aurora_protocols_output_changed(o->resource);output_send(o->resource,d);}}
        if(d)d->global=output_globals[i];
    }
}

static bool endpoint_for(struct endpoint* e,struct surface* s) { return s&&s->resource&&wl_resource_get_client(e->resource)==wl_resource_get_client(s->resource); }
static void modifiers(struct endpoint* e,uint32_t serial) {
    wl_keyboard_send_modifiers(e->resource,serial,xkb_state_serialize_mods(key_state,XKB_STATE_MODS_DEPRESSED),xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LATCHED),xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LOCKED),xkb_state_serialize_layout(key_state,XKB_STATE_LAYOUT_EFFECTIVE));
}
static void keyboard_enter(struct endpoint* e) {
    uint32_t held[KEY_MAX+1];struct wl_array keys={.data=held,.alloc=sizeof(held),.size=0};
    for(unsigned key=0;key<=KEY_MAX;key++)if((pressed[key/8]&~shell_keys[key/8])&(1U<<(key%8)))held[keys.size/4]=key,keys.size+=4;
    uint32_t serial=wl_display_next_serial(display);wl_keyboard_send_enter(e->resource,serial,focus->resource,&keys);modifiers(e,serial);
}
/* X windows: a modal dialog takes its parent's focus; dialogs (WM_TRANSIENT_FOR) stay above the window they belong to. */
static struct surface* x11_modal(struct surface* s) {
    for (int depth = 0; depth < 8; depth++) { struct surface* o, *next = NULL;
        wl_list_for_each(o, &windows, window_link) if (o->transient_for == s && o->x11 && o->x11_modal && o->mapped && o != s) next = o;
        if (!next) break; s = next; }
    return s;
}
static void x11_raise_transients(struct surface* root, int depth) {
    struct surface* lift[16]; int n = 0, k = 0; struct surface* o;
    wl_list_for_each(o, &windows, window_link) if (n < 16 && o != root && o->x11 && o->transient_for == root) lift[n++] = o;
    for (; k < n; k++) { wl_list_remove(&lift[k]->window_link); wl_list_insert(windows.prev, &lift[k]->window_link); }
    for (k = 0; k < n && depth < 8; k++) x11_raise_transients(lift[k], depth + 1);
}
static void focus_surface(struct surface* surface) {
    if(surface&&aurora_mode&&aurora_lock_active())surface=NULL;   /* locked: no client has the keyboard (or the selection) */
    if(surface&&surface->x11){if(!aurora_xwm_takes_focus(surface->x11))return;surface=x11_modal(surface);}   /* override-redirect: never */
    if(focus==surface)return;
    struct surface* old=popup_family(focus);
    struct endpoint* e;uint32_t serial=wl_display_next_serial(display);
    wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus))wl_keyboard_send_leave(e->resource,serial,focus->resource);
    focus=surface;
    if(surface)desktop_focus=false;
    shell_dirty|=1U<<SHELL_MENUBAR;dirty=true;
    if(aurora_mode&&surface){struct surface* root=popup_family(surface);if(root->kind==ROLE_TOPLEVEL){wl_list_remove(&root->window_link);wl_list_insert(windows.prev,&root->window_link);if(root->x11)x11_raise_transients(root,0);}}
    wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus))keyboard_enter(e);
    if(aurora_mode){struct surface* next=popup_family(focus);if(old!=next){if(old&&old->mapped&&old->role)configure(old);if(next&&next->mapped&&next->role)configure(next);}}
    if(aurora_mode){struct surface* x=popup_family(focus);aurora_xwm_focus(x&&x->x11?x->x11:NULL);}   /* X input focus, stacking (aurora_xwm.c) */
    selection_focus();constraint_check();
    if(aurora_mode)aurora_ime_focus(focus&&focus->resource?focus->resource:NULL);   /* text-input-v3 enter/leave */
}
static void endpoint_destroy(struct wl_resource* r) { struct endpoint* e=wl_resource_get_user_data(r);if(cursor_owner==e){cursor_owner=NULL;cursor_dirty=true;}wl_list_remove(&e->link);free(e); }
static void cursor_request(struct wl_client* c,struct wl_resource* r,uint32_t serial,struct wl_resource* resource,int32_t x,int32_t y) {
    (void)c;struct endpoint* e=wl_resource_get_user_data(r);struct surface* s=resource?wl_resource_get_user_data(resource):NULL;
    if(s){if(s->xdg||(s->kind!=ROLE_NONE&&s->kind!=ROLE_CURSOR)){wl_resource_post_error(r,WL_POINTER_ERROR_ROLE,"cursor has another role");return;}s->kind=ROLE_CURSOR;}
    /* Only the client with pointer focus sets the cursor, with the serial of its latest enter; NULL hides it. */
    if(!endpoint_for(e,pointer_focus)||serial!=e->enter_serial)return;
    e->cursor=s;e->hot_x=x;e->hot_y=y;e->cursor_set=true;cursor_owner=e;cursor_dirty=true;
}
static const struct wl_pointer_interface pointer_interface={cursor_request,destroy_request};
static const struct wl_keyboard_interface keyboard_interface={destroy_request};
static wl_fixed_t coordinate(int64_t value);
static void get_endpoint(struct wl_client* c,struct wl_resource* seat,uint32_t id,bool keyboard) {
    struct endpoint* e=calloc(1,sizeof(*e));struct wl_resource* r=wl_resource_create(c,keyboard?&wl_keyboard_interface:&wl_pointer_interface,wl_resource_get_version(seat),id);
    if(!e||!r){free(e);if(r)wl_resource_destroy(r);wl_resource_post_no_memory(seat);return;}
    e->resource=r;e->keyboard=keyboard;wl_list_insert(endpoints.prev,&e->link);
    wl_resource_set_implementation(r,keyboard?(const void*)&keyboard_interface:(const void*)&pointer_interface,e,endpoint_destroy);
    if(keyboard){wl_keyboard_send_keymap(r,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,keymap_fd,keymap_size);if(wl_resource_get_version(r)>=4)wl_keyboard_send_repeat_info(r,25,500);if(endpoint_for(e,focus))keyboard_enter(e);}
    else if (endpoint_for(e,pointer_focus)) {
        e->enter_serial=wl_display_next_serial(display);
        wl_pointer_send_enter(r,e->enter_serial,pointer_focus->resource,coordinate(pointer_local_x),coordinate(pointer_local_y));
        if(wl_resource_get_version(r)>=5)wl_pointer_send_frame(r);
    }
}
static void get_keyboard(struct wl_client* c,struct wl_resource* r,uint32_t id){get_endpoint(c,r,id,true);}
static void get_pointer(struct wl_client* c,struct wl_resource* r,uint32_t id){get_endpoint(c,r,id,false);}
static void get_touch(struct wl_client* c,struct wl_resource* r,uint32_t id){(void)c;(void)id;wl_resource_post_error(r,WL_SEAT_ERROR_MISSING_CAPABILITY,"no touch hardware");}
static const struct wl_seat_interface seat_interface={get_pointer,get_keyboard,get_touch,destroy_request};
static void bind_seat(struct wl_client* c,void* data,uint32_t version,uint32_t id){(void)data;struct wl_resource* r=wl_resource_create(c,&wl_seat_interface,version,id);if(!r){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(r,&seat_interface,NULL,NULL);wl_seat_send_capabilities(r,WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);if(version>=2)wl_seat_send_name(r,"nexus-seat0");}
static bool surface_origin(struct surface* surface, int64_t* x, int64_t* y) {
    *x = *y = 0;
    for (struct surface* s = surface; s; s = s->parent?s->parent:s->popup_parent){
        int64_t sx=s->x,sy=s->y;
        if(s->kind==ROLE_POPUP){struct popup_box parent,own;
            if(!s->popup_parent||window_bounds(s->popup_parent,&parent)<=0)return false;
            if(window_bounds(s,&own)<=0)own=(struct popup_box){0};
            sx=(int64_t)parent.x+s->popup_box.x-own.x;sy=(int64_t)parent.y+s->popup_box.y-own.y;
        }
        if(s->kind==ROLE_IME_POPUP){int px,py;if(aurora_ime_popup(&px,&py)!=s->resource)return false;   /* below the text cursor, kept on the desktop */
            sx=px;sy=py;if(sx>(int64_t)width-s->view.w)sx=(int64_t)width-s->view.w;if(sy>(int64_t)height-s->view.h)sy=(int64_t)height-s->view.h;if(sx<0)sx=0;if(sy<0)sy=0;}
        if (__builtin_add_overflow(*x, sx, x) || __builtin_add_overflow(*y, sy, y)) return false;
    }
    return true;
}
/* Walk the parent plane and nested sibling stacks without a recursion depth limit. */
static struct surface* next_plane(struct surface* root, struct surface** owner, struct wl_list** position, bool reverse) {
    for (;;) {
        struct surface* s = *owner; struct wl_list* link = *position;
        if (link == &s->children) {
            if (s == root) return NULL;
            *position = reverse ? s->sibling.prev : s->sibling.next; *owner = s->parent;
        } else if (link == &s->plane) {
            *position = reverse ? link->prev : link->next; return s;
        } else {
            struct surface* child = wl_container_of(link, child, sibling);
            if (!child->mapped) *position = reverse ? link->prev : link->next;
            else { *owner = child; *position = reverse ? child->children.prev : child->children.next; }
        }
    }
}
static wl_fixed_t coordinate(int64_t value) {
    if (value > 8388607) return INT32_MAX;
    if (value < -8388608) return INT32_MIN;
    return (wl_fixed_t)(value * 256);
}
static struct surface* hit_tree(struct surface* root){
    if(!root->mapped)return NULL;struct surface *owner=root,*s;struct wl_list* position=root->children.prev;
    while((s=next_plane(root,&owner,&position,true))){
        int64_t x,y;if(!surface_origin(s,&x,&y))continue;
        int64_t sx,sy;if(__builtin_sub_overflow((int64_t)pointer_x,x,&sx)||__builtin_sub_overflow((int64_t)pointer_y,y,&sy))continue;
        if(sx>=0&&sy>=0&&sx<s->view.w&&sy<s->view.h&&(!s->input_set||region_has(&s->input,sx,sy)))return s;
    }
    return NULL;
}
static struct surface* hit_family(struct surface* root){
    struct surface *popup,*hit;wl_list_for_each_reverse(popup,&popups,popup_link)
        if(popup_family(popup)==root&&(hit=hit_tree(popup)))return hit;
    return hit_tree(root);
}
static void pointer_update(void) {
    if(dnd_pointer())return;   /* a drag: data_device enter/motion/leave instead of wl_pointer events */
    if(aurora_mode)shell_motion();
    struct surface* target = pointer_buttons && pointer_grab && pointer_grab->mapped ? pointer_grab : NULL;
    struct surface* root; int64_t local_x = 0, local_y = 0;
    struct surface* active=popup_family(focus);
    if(!target&&aurora_mode)wl_list_for_each_reverse(root,&x11_overrides,window_link)if(root->mapped&&(target=hit_family(root)))break;   /* X menus are on top */
    if(!target&&active&&active->kind==ROLE_TOPLEVEL)target=hit_family(active);
    if(!target&&aurora_mode)wl_list_for_each_reverse(root,&windows,window_link){if(!root->mapped)continue;target=hit_family(root);if(target)break;}
    if (!target&&!aurora_mode) wl_list_for_each_reverse(root, &surfaces, link) {
        if (root==active||root->parent || !root->mapped || root->kind != ROLE_TOPLEVEL) continue;
        target=hit_family(root);
        if (target) break;
    }
    if(aurora_mode&&!pointer_buttons&&!moving&&shell_hit(pointer_x,pointer_y))target=NULL;
    if(moving)target=NULL;
    if(target&&popup_grab&&wl_resource_get_client(target->resource)!=wl_resource_get_client(popup_grab->resource))target=NULL;
    if (target) {
        if (!surface_origin(target, &local_x, &local_y)) target = NULL;
        else { local_x = pointer_x - local_x; local_y = pointer_y - local_y; }
    }
    bool changed = target != pointer_focus, moved = local_x != pointer_local_x || local_y != pointer_local_y;
    struct endpoint* e; uint32_t serial = wl_display_next_serial(display), time = milliseconds();
    wl_list_for_each(e, &endpoints, link) if (!e->keyboard) {
        bool sent = false;
        if (changed && endpoint_for(e, pointer_focus)) { wl_pointer_send_leave(e->resource, serial, pointer_focus->resource); sent = true; }
        if (endpoint_for(e, target)) {
            if (changed) { e->enter_serial = serial; wl_pointer_send_enter(e->resource, serial, target->resource, coordinate(local_x), coordinate(local_y)); sent = true; }
            if (moved) { wl_pointer_send_motion(e->resource, time, coordinate(local_x), coordinate(local_y)); sent = true; }
        }
        if (sent && wl_resource_get_version(e->resource) >= 5) wl_pointer_send_frame(e->resource);
    }
    if (changed) { cursor_owner = NULL; cursor_dirty = true; }   /* the cursor is undefined after enter until the client sets one */
    pointer_focus = target; pointer_local_x = local_x; pointer_local_y = local_y;
    constraint_check();
}
/* Pointer constraints: activate/deactivate for the pointer focus; a released lock may warp to its position hint. */
static void confine_settle(struct surface* s);
static void constraint_check(void) {
    if (constraint_busy) return;
    constraint_busy = true;
    struct surface* target = pointer_focus && pointer_focus->resource ? pointer_focus : NULL;
    bool focused = target && focus && popup_family(target) == popup_family(focus) && !shell_overlay() && !moving;
    constraint_kind = aurora_constraints_update(target ? target->resource : NULL, (int32_t)pointer_local_x, (int32_t)pointer_local_y, focused);
    if (constraint_kind == AURORA_CONSTRAINT_CONFINE && target) {
        confine_settle(target);
        if (pointer_focus != target) constraint_kind = aurora_constraints_update(NULL, 0, 0, false);   /* warped onto another surface */
    }
    struct wl_resource* hinted; wl_fixed_t hint_x, hint_y; int64_t x, y;
    if (aurora_constraint_take_hint(&hinted, &hint_x, &hint_y) && target && target->resource == hinted && surface_origin(target, &x, &y)) {
        x += wl_fixed_to_int(hint_x); y += wl_fixed_to_int(hint_y);
        if (x >= 0 && y >= 0 && x < width && y < height && (x != pointer_x || y != pointer_y)) { pointer_x = (int)x; pointer_y = (int)y; cursor_dirty = true; pointer_update(); }
    }
    constraint_busy = false;
}
void aurora_host_constraints_changed(void) { constraint_check(); }
/* Confinement: whether screen point x,y is inside the confining surface (and its input and constraint regions) and not
 * on the menu bar or dock, which would take the pointer from the surface and so end the confinement. */
static bool confine_inside(struct surface* s, int64_t ox, int64_t oy, int64_t x, int64_t y) {
    int64_t lx = x - ox, ly = y - oy;
    return lx >= 0 && ly >= 0 && lx < s->view.w && ly < s->view.h && (!s->input_set || region_has(&s->input, (int)lx, (int)ly)) &&
           aurora_constraint_contains((int32_t)lx, (int32_t)ly) && !shell_bar_hit((int)x, (int)y);
}
/* An active confinement whose region (set_region), surface size or input region changed may no longer hold the pointer:
 * warp it to the nearest inside point (wl_pointer.motion, no relative motion, as the protocol allows). Candidates are the
 * pointer clamped into each region x input rectangle (and the view and screen); without one inside, the pointer moves
 * freely until it is inside again (confine()). */
static void confine_settle(struct surface* s) {
    int64_t ox, oy; if (!surface_origin(s, &ox, &oy) || confine_inside(s, ox, oy, pointer_x, pointer_y)) return;
    const struct rectangle all = {NULL, INT32_MIN / 2, INT32_MIN / 2, INT32_MAX, INT32_MAX, true}, *region = aurora_constraint_region();
    int64_t px = pointer_x - ox, py = pointer_y - oy, best = -1, bx = 0, by = 0; unsigned budget = 1024;
    for (const struct rectangle* a = region ? region : &all; a && budget; a = a->next) if (a->add)
        for (const struct rectangle* b = s->input_set ? s->input.rectangles : &all; b && budget; b = b->next, budget--) if (b->add) {
            int64_t x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
            int64_t x1 = (int64_t)a->x + a->w < (int64_t)b->x + b->w ? (int64_t)a->x + a->w : (int64_t)b->x + b->w;
            int64_t y1 = (int64_t)a->y + a->h < (int64_t)b->y + b->h ? (int64_t)a->y + a->h : (int64_t)b->y + b->h;
            if (x0 < 0) x0 = 0; if (x0 < -ox) x0 = -ox; if (y0 < 0) y0 = 0; if (y0 < -oy) y0 = -oy;
            if (x1 > s->view.w) x1 = s->view.w; if (x1 > (int64_t)width - ox) x1 = (int64_t)width - ox;
            if (y1 > s->view.h) y1 = s->view.h; if (y1 > (int64_t)height - oy) y1 = (int64_t)height - oy;
            if (x0 >= x1 || y0 >= y1) continue;
            int64_t x = px < x0 ? x0 : px >= x1 ? x1 - 1 : px, y = py < y0 ? y0 : py >= y1 ? y1 - 1 : py, d = (x - px) * (x - px) + (y - py) * (y - py);
            if ((best < 0 || d < best) && confine_inside(s, ox, oy, x + ox, y + oy)) { best = d; bx = x; by = y; }
        }
    if (best < 0) return;
    pointer_x = (int)(bx + ox); pointer_y = (int)(by + oy); cursor_dirty = true; pointer_update();
}
/* Move from the current pointer toward x,y without leaving the confinement: each axis goes as far as it can. */
static void confine(int64_t* x, int64_t* y) {
    struct surface* s = pointer_focus; int64_t ox, oy;
    if (!s || !surface_origin(s, &ox, &oy)) { *x = pointer_x; *y = pointer_y; return; }
    if (confine_inside(s, ox, oy, *x, *y)) return;
    if (!confine_inside(s, ox, oy, pointer_x, pointer_y)) return;   /* no inside start to search from: see confine_settle() */
    int64_t bx = pointer_x, by = pointer_y;
    for (int axis = 0; axis < 2; axis++) {
        int64_t want = axis ? *y : *x, lo = axis ? by : bx, hi = want;
        if (axis ? confine_inside(s, ox, oy, bx, want) : confine_inside(s, ox, oy, want, by)) lo = want;
        else while (hi - lo > 1 || lo - hi > 1) {
            int64_t mid = lo + (hi - lo) / 2;
            if (axis ? confine_inside(s, ox, oy, bx, mid) : confine_inside(s, ox, oy, mid, by)) lo = mid; else hi = mid;
        }
        if (axis) by = lo; else bx = lo;
    }
    *x = bx; *y = by;
}
/* One input frame of pointer motion: relative motion to the focus client, then the constrained absolute position.
 * Returns whether it ran pointer_update(). */
static bool pointer_motion(void) {
    int64_t dx = motion_dx, dy = motion_dy, ax = motion_abs[0], ay = motion_abs[1];
    motion_dx = motion_dy = 0; motion_abs[0] = motion_abs[1] = -1;
    if (!dx && !dy && ax < 0 && ay < 0) return false;
    struct surface* target = pointer_focus && pointer_focus->resource ? pointer_focus : NULL; bool relative = false;
    if (target && (dx || dy)) relative = aurora_relative_motion(wl_resource_get_client(target->resource), nanoseconds() / 1000, wl_fixed_from_int((int)dx), wl_fixed_from_int((int)dy));
    int old_x = pointer_x, old_y = pointer_y;
    if (constraint_kind != AURORA_CONSTRAINT_LOCK) {
        /* An absolute report that changes one axis keeps the device's last position on the other (as libinput does). */
        int64_t x = ax >= 0 ? ax : ay >= 0 ? (motion_abs_last[0] >= 0 ? motion_abs_last[0] : pointer_x) : (int64_t)pointer_x + dx,
                y = ay >= 0 ? ay : ax >= 0 ? (motion_abs_last[1] >= 0 ? motion_abs_last[1] : pointer_y) : (int64_t)pointer_y + dy;
        aurora_outputs_clamp(&x, &y);   /* the union of the displays, not their bounding box */
        if (constraint_kind == AURORA_CONSTRAINT_CONFINE) confine(&x, &y);
        pointer_x = (int)x; pointer_y = (int)y;
    }
    if (pointer_x != old_x || pointer_y != old_y) cursor_dirty = true;
    int64_t local_x = pointer_local_x, local_y = pointer_local_y; struct surface* before = pointer_focus;
    pointer_update();
    if (relative && before == pointer_focus && local_x == pointer_local_x && local_y == pointer_local_y) {
        struct endpoint* e;   /* no wl_pointer.motion followed: close the relative-motion frame */
        wl_list_for_each(e, &endpoints, link) if (!e->keyboard && endpoint_for(e, target) && wl_resource_get_version(e->resource) >= 5) wl_pointer_send_frame(e->resource);
    }
    return true;
}
/* A left press on an X frame (aurora_xwm.c draws it): title bar moves (double click zooms), edges resize, the traffic
 * lights close/minimize/zoom. The press never reaches the X client. */
static bool x11_begin(struct surface* s, uint32_t edges);
static bool x11_frame_press(struct surface* s, uint32_t mask) {
    static uint32_t title_ms; static struct surface* title_window;
    int part = aurora_xwm_frame_hit(s->x11, (int)pointer_local_x, (int)pointer_local_y); if (!part || !s->role) return false;
    focus_surface(s); pointer_buttons |= mask; uint32_t now = milliseconds();
    if (part == AURORA_XFRAME_TITLE && title_window == s && now - title_ms < 400) part = AURORA_XFRAME_ZOOM;
    else if (part <= AURORA_XFRAME_TITLE) { title_window = part == AURORA_XFRAME_TITLE ? s : NULL; title_ms = now;
        x11_begin(s, part == AURORA_XFRAME_TITLE ? 0 : (uint32_t)part); return true; }
    title_window = NULL; swallowed_buttons |= mask;
    printf("[64] AURORA X11 FRAME %s app=%s\n", part == AURORA_XFRAME_CLOSE ? "close" : part == AURORA_XFRAME_MINIMIZE ? "minimize" : "zoom", s->app_id); fflush(stdout);
    if (part == AURORA_XFRAME_CLOSE) toplevel_close(s);
    else if (part == AURORA_XFRAME_MINIMIZE) top_minimize(NULL, s->role);
    else if (s->maximized || s->fullscreen) top_restore(NULL, s->role); else top_maximize(NULL, s->role);
    return true;
}
static void pointer_button(unsigned code, bool down) {
    if (code < BTN_LEFT || code > BTN_EXTRA) return;
    if (dnd_button(code, down)) return;   /* the release of a drag's button drops */
    pointer_update(); uint32_t mask = 1U << (code - BTN_LEFT);
    if (aurora_mode && aurora_lock_active()) { aurora_lock_activity(); if (down && code == BTN_LEFT) aurora_lock_pointer(pointer_x, pointer_y); return; }   /* aurora_lock.h */
    if(down){button_origin=key_origin=NULL;button_serial=key_serial=0;}
    if(swallowed_buttons&mask){if(!down){swallowed_buttons&=~mask;pointer_buttons&=~mask;
            if(aurora_mode&&aurora_wm_overlay()){aurora_wm_pointer(pointer_x,pointer_y,code,false);refresh_scene();}   /* overview: drops end on release */
            if(!pointer_buttons)pointer_update();}return;}
    if(aurora_mode&&moving){
        if(code!=BTN_LEFT){if(down){pointer_buttons|=mask;swallowed_buttons|=mask;}else pointer_buttons&=~mask;return;}
        if(down)return;
    }
    if(down&&popup_grab){
        struct surface* hit=root_surface(pointer_focus);bool inside=false;
        for(struct surface* p=popup_grab;p&&p->kind==ROLE_POPUP;p=p->popup_parent)if(p==hit){inside=true;break;}
        if(!inside){struct surface* first=popup_grab;while(first->popup_parent&&first->popup_parent->kind==ROLE_POPUP&&first->popup_parent->popup_grabbed)first=first->popup_parent;
            popup_dismiss(first);swallowed_buttons|=mask;pointer_buttons|=mask;pointer_grab=NULL;return;}
    }
    if(aurora_mode&&moving&&!down&&code==BTN_LEFT){struct surface* moved=moving;bool resized=resize_edges!=0;moving=NULL;resize_edges=0;pointer_buttons&=~mask;pointer_grab=NULL;screen_update(moved);
        if(!resized)wm_drop(moved);   /* dropped on a snap edge: tiled or maximized there */
        configure(moved);popups_reconstrain(moved);pointer_update();
        printf("[64] AURORA MOVED app=%s x=%d y=%d screen=%s\n",moved->app_id,moved->x,moved->y,surface_screen(moved)->name);fflush(stdout);return;}
    if(aurora_mode&&down&&!pointer_buttons&&shell_hit(pointer_x,pointer_y)){
        pointer_buttons|=mask;swallowed_buttons|=mask;pointer_grab=NULL;
        if(code==BTN_LEFT)shell_click(pointer_x,pointer_y);return;
    }
    if(aurora_mode&&down&&!pointer_focus&&!popup_grab){focus_surface(NULL);desktop_focus=true;dirty=true;}
    if(aurora_mode&&down&&code==BTN_LEFT&&!pointer_buttons&&!popup_grab&&pointer_focus&&pointer_focus->x11&&x11_frame_press(pointer_focus,mask))return;   /* Aurora's X frames */
    if (down && !pointer_buttons) pointer_grab = pointer_focus;
    if (down && pointer_focus&&!popup_grab&&popup_family(pointer_focus)->kind!=ROLE_IME_POPUP) focus_surface(popup_family(pointer_focus));   /* a candidate click keeps the text focus */
    struct endpoint* e; uint32_t serial = wl_display_next_serial(display);
    if(down&&pointer_focus){button_serial=serial;button_origin=pointer_focus;}
    wl_list_for_each(e, &endpoints, link) if (!e->keyboard && endpoint_for(e, pointer_focus)) {
        wl_pointer_send_button(e->resource, serial, milliseconds(), code, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        if (wl_resource_get_version(e->resource) >= 5) wl_pointer_send_frame(e->resource);
    }
    input_boost(pointer_focus);
    if (down) pointer_buttons |= mask; else pointer_buttons &= ~mask;
    if (!pointer_buttons) { pointer_grab = NULL; pointer_update(); }
}
static void shell_super_used(void);   /* Super was part of a combination: its release opens nothing */
static void keyboard_event(unsigned key,int value) {
    if(key>KEY_MAX||value==2)return;
    if(value)pressed[key/8]|=1U<<(key%8);else pressed[key/8]&=~(1U<<(key%8));
    xkb_state_update_key(key_state,key+8,value?XKB_KEY_DOWN:XKB_KEY_UP);
    if(aurora_mode&&aurora_sak_key(key,value!=0,(pressed[KEY_LEFTMETA/8]&(1U<<(KEY_LEFTMETA%8)))||(pressed[KEY_RIGHTMETA/8]&(1U<<(KEY_RIGHTMETA%8))))){shell_super_used();return;}   /* the secure attention key (aurora_sak.h): before everyone */
    if(aurora_mode&&aurora_reader_key_event(key,value!=0,xkb_state_key_get_one_sym(key_state,key+8),xkb_state_key_get_utf32(key_state,key+8),xkb_state_serialize_mods(key_state,XKB_STATE_MODS_EFFECTIVE)))return;   /* the screen reader's keys */
    if(aurora_mode&&aurora_a11y()->sticky_keys&&sticky_key(key,value))return;
    if(aurora_mode&&aurora_lock_active()&&value&&key==KEY_S&&(pressed[KEY_LEFTMETA/8]&(1U<<(KEY_LEFTMETA%8))||pressed[KEY_RIGHTMETA/8]&(1U<<(KEY_RIGHTMETA%8)))&&(pressed[KEY_LEFTALT/8]&(1U<<(KEY_LEFTALT%8))||pressed[KEY_RIGHTALT/8]&(1U<<(KEY_RIGHTALT%8)))){int a;aurora_reader_key(key,1,AURORA_MOD_SUPER|AURORA_MOD_ALT,&a);return;}   /* screen reader on the lock screen */
    if(aurora_mode){aurora_lock_activity();if(aurora_lock_active()){if(value){char text[8]={0};xkb_state_key_get_utf8(key_state,key+8,text,sizeof(text));aurora_lock_key(key,text);}return;}}
    if(aurora_mode&&shell_key(key,value))return;
    if(value){button_origin=key_origin=NULL;button_serial=key_serial=0;}
    if(key==KEY_ESC&&value&&dnd_escape())return;   /* Esc cancels a drag */
    if(key==KEY_ESC&&value&&popup_grab){popup_dismiss(popup_grab);return;}
    struct surface* family=popup_family(focus);
    if(key==KEY_F4&&value&&(pressed[KEY_LEFTALT/8]&(1U<<(KEY_LEFTALT%8)))&&family&&family->role&&family->kind==ROLE_TOPLEVEL){if(aurora_mode)shell_keys[key/8]|=1U<<(key%8);toplevel_close(family);return;}
    if(aurora_mode&&focus){bool took=aurora_ime_key(milliseconds(),key,value?1:0);   /* an input method's keyboard grab */
        aurora_ime_modifiers(xkb_state_serialize_mods(key_state,XKB_STATE_MODS_DEPRESSED),xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LATCHED),xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LOCKED),xkb_state_serialize_layout(key_state,XKB_STATE_LAYOUT_EFFECTIVE));
        if(took){input_boost(focus);return;}}
    struct endpoint* e;uint32_t serial=wl_display_next_serial(display);
    if(value&&focus){key_serial=serial;key_origin=focus;}
    wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus)){wl_keyboard_send_key(e->resource,serial,milliseconds(),key,value?WL_KEYBOARD_KEY_STATE_PRESSED:WL_KEYBOARD_KEY_STATE_RELEASED);modifiers(e,serial);}
    input_boost(focus);
}
static int input_event(int fd,uint32_t mask,void* data) {
    (void)mask;bool mouse=(uintptr_t)data!=0;struct input_event events[32];ssize_t size;
    while((size=read(fd,events,sizeof(events)))>0)for(size_t i=0;i<(size_t)size/sizeof(*events);i++){
        struct input_event* event=events+i;bool* dropped=mouse?&mouse_dropped:&key_dropped;
        if(event->type==EV_SYN&&event->code==SYN_DROPPED){*dropped=true;continue;}
        if(*dropped){if(event->type==EV_SYN&&event->code==SYN_REPORT){uint8_t keys[sizeof(pressed)]={0};if(ioctl(fd,EVIOCGKEY(sizeof(keys)),keys)<0)return -1;
            if(!mouse){for(unsigned k=0;k<=KEY_MAX;k++)if((keys[k/8]^pressed[k/8])&(1U<<(k%8)))keyboard_event(k,!!(keys[k/8]&(1U<<(k%8))));}
            else for(unsigned k=BTN_LEFT;k<=BTN_EXTRA;k++){bool down=!!(keys[k/8]&(1U<<(k%8)));if(down!=!!(pointer_buttons&(1U<<(k-BTN_LEFT))))pointer_button(k,down);}
            *dropped=false;}continue;}
        if(!mouse){if(event->type==EV_KEY)keyboard_event(event->code,event->value);continue;}
        if(event->type==EV_REL&&(event->code==REL_X||event->code==REL_Y)){if(event->code==REL_X)motion_dx+=event->value;else motion_dy+=event->value;}
        else if(event->type==EV_SYN){aurora_lock_activity();if(!pointer_motion())pointer_update();}
        else if(event->type==EV_KEY){pointer_motion();pointer_button(event->code,event->value!=0);}
        /* Absolute pointers (USB tablets): ABS_X/ABS_Y scaled from the node's EVIOCGABS range to the first display (QEMU's
         * tablet reports positions on head 0, its console), so the pointer reaches the others by relative motion. */
        else if(event->type==EV_ABS&&event->code<=ABS_Y){static struct input_absinfo range[2];static bool known[2];unsigned axis=event->code;if(!known[axis]&&!ioctl(fd,EVIOCGABS(axis),&range[axis]))known[axis]=true;
            struct aurora_output* head=aurora_output_slot(0);if(!head||!head->enabled)head=aurora_output_primary();if(head->mirror>=0&&aurora_output_slot(head->mirror))head=aurora_output_slot(head->mirror);
            int64_t span=(int64_t)range[axis].maximum-range[axis].minimum,limit=axis?head->h:head->w,origin=axis?head->y:head->x;if(known[axis]&&span>0&&limit>0){int64_t v=((int64_t)event->value-range[axis].minimum)*(limit-1)/span;v=(v<0?0:v>=limit?limit-1:v)+origin;
            if(motion_abs_last[axis]>=0){if(axis)motion_dy+=v-motion_abs_last[axis];else motion_dx+=v-motion_abs_last[axis];}motion_abs_last[axis]=v;motion_abs[axis]=v;}}
        /* Wheels: axis_source + axis_discrete (seat v5) before the axis value, as winewayland and toolkits expect. */
        else if(event->type==EV_REL&&(event->code==REL_WHEEL||event->code==REL_HWHEEL)){if(!pointer_motion())pointer_update();struct endpoint* e;bool vertical=event->code==REL_WHEEL;int32_t steps=vertical?-event->value:event->value;
            uint32_t axis=vertical?WL_POINTER_AXIS_VERTICAL_SCROLL:WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            wl_list_for_each(e,&endpoints,link)if(!e->keyboard&&endpoint_for(e,pointer_focus)){bool framed=wl_resource_get_version(e->resource)>=5;
                if(framed){wl_pointer_send_axis_source(e->resource,WL_POINTER_AXIS_SOURCE_WHEEL);wl_pointer_send_axis_discrete(e->resource,axis,steps);}
                wl_pointer_send_axis(e->resource,milliseconds(),axis,coordinate((int64_t)steps*10));if(framed)wl_pointer_send_frame(e->resource);}
            input_boost(pointer_focus);}
    }
    if(size<0&&errno!=EAGAIN&&errno!=EINTR){perror("input");return -1;}return 0;
}
/* ---- Damage-driven composition -------------------------------------------------
 * paint() builds the bottom-to-top list of drawn surfaces, turns surface content
 * damage, moved/mapped/unmapped/restacked surfaces and changed shell elements into
 * screen damage, repaints only those rectangles (skipping layers hidden under an
 * opaque surface), and copies only them (plus the cursor) to the framebuffer. */
struct item { struct surface* s; int64_t x, y; int32_t w, h; struct popup_box box; struct view v; int k, k120; };   /* v: the view on the output; k: its whole scale (0: fractional k120/120) */
static struct item* items; static size_t item_count, item_capacity;
static struct surface** last_order; static size_t last_count, last_capacity;
static unsigned paint_serial;
static void item_add(struct surface* s) {
    int64_t x, y; if (!s->pixels || !surface_origin(s, &x, &y)) return;
    int32_t w = s->view.w, h = s->view.h; if (w <= 0 || h <= 0) return;
    if (item_count == item_capacity) {
        size_t next = item_capacity ? item_capacity * 2 : 16; struct item* grown = realloc(items, next * sizeof(*grown));
        if (!grown) return; items = grown; item_capacity = next;
    }
    struct popup_box box = {0};
    int64_t x1 = x + w, y1 = y + h, x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    if (x1 > width) x1 = width; if (y1 > height) y1 = height;
    if (x0 < x1 && y0 < y1) box = (struct popup_box){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
    items[item_count++] = (struct item){s, x, y, w, h, box, s->view, 1, 120};
}
static void items_family(struct surface* root) {
    struct surface *owner = root, *s; struct wl_list* position = root->children.next;
    while ((s = next_plane(root, &owner, &position, false))) item_add(s);
    struct surface* popup; wl_list_for_each(popup, &popups, popup_link) if (popup->mapped && popup_family(popup) == root) {
        owner = popup; position = popup->children.next; while ((s = next_plane(popup, &owner, &position, false))) item_add(s);
    }
}
static void items_build(void) {
    struct surface* root; item_count = 0;
    if (aurora_mode && (aurora_wm_overview() || aurora_lock_active())) return;   /* overview: thumbnails; locked: nothing */
    if (aurora_mode) { wl_list_for_each(root, &windows, window_link) if (root->mapped) items_family(root);
        wl_list_for_each(root, &x11_overrides, window_link) if (root->mapped) items_family(root);   /* X menus and tooltips */
        int px, py; struct wl_resource* ime = aurora_ime_popup(&px, &py); struct surface* p = ime ? wl_resource_get_user_data(ime) : NULL;
        if (p && p->mapped) items_family(p);   /* the input method's candidates above every window */
        return; }
    struct surface* active = popup_family(focus);
    for (unsigned pass = 0; pass < 2; pass++) wl_list_for_each(root, &surfaces, link)
        if (!root->parent && root->mapped && root->kind == ROLE_TOPLEVEL && ((root == active) == (pass != 0))) items_family(root);
}
static void blend_run(uint32_t* dst, const uint32_t* src, int64_t n, int scale) {
    for (int64_t i = 0; i < n; i++) {
        uint32_t p = src[i * scale], a = p >> 24;
        if (a == 255) dst[i] = p & 0xFFFFFF; else if (a) dst[i] = blend_pixel(p, dst[i]);
    }
}
static void copy_run(uint32_t* dst, const uint32_t* src, int64_t n, int scale) {
    if (scale == 1) { memcpy(dst, src, (size_t)n * 4); return; }
    for (int64_t i = 0; i < n; i++) dst[i] = src[i * scale];
}
/* Draw clip c (inside the item's screen box) of one surface: opaque spans copy, the rest blends. */
static void draw_scaled(const struct item* it, struct popup_box c) {
    const struct surface* s = it->s; const struct view* v = &it->v;
    for (int64_t y = c.y; y < (int64_t)c.y + c.h; y++) {
        int64_t by = (v->y0 + (y - it->y) * v->dy) / 65536; by = by < 0 ? 0 : by >= s->height ? s->height - 1 : by;
        const uint32_t* row = s->pixels + (size_t)by * (size_t)s->width; uint32_t* dst = canvas + (size_t)y * stride;
        for (int64_t x = c.x; x < (int64_t)c.x + c.w; x++) {
            int64_t bx = (v->x0 + (x - it->x) * v->dx) / 65536; bx = bx < 0 ? 0 : bx >= s->width ? s->width - 1 : bx;
            uint32_t p = row[bx], a = p >> 24;
            if (a == 255) dst[x] = p & 0xFFFFFF; else if (a) dst[x] = blend_pixel(p, dst[x]);
        }
    }
}
static void draw_item(const struct item* it, struct popup_box c) {
    const struct surface* s = it->s; const struct opaque_map* m = &s->omap; int band = 0, f = it->k;   /* f: output scale */
    if (!it->v.fast || f <= 0) { draw_scaled(it, c); return; }
    int scale = (int)(it->v.dx / 65536), vscale = (int)(it->v.dy / 65536);   /* buffer pixels per drawn pixel */
    int64_t ox = it->v.x0 / 65536, oy = it->v.y0 / 65536;   /* wp_viewport crop origin (0 without a viewport) */
    for (int64_t y = c.y; y < (int64_t)c.y + c.h; y++) {
        int64_t sy = (y - it->y) / f, x = c.x, end = (int64_t)c.x + c.w;   /* sy: surface-local row (opaque map) */
        const uint32_t* row = s->pixels + (size_t)(oy + (y - it->y) * vscale) * (size_t)s->width + ox;
        uint32_t* dst = canvas + (size_t)y * stride;
#define SOURCE(at) (row + (size_t)(((at) - it->x) * scale))
        while (band < m->bands && m->band[band].y1 <= sy) band++;
        if (band < m->bands && m->band[band].y0 <= sy) {
            const struct ospan* span = m->span + m->band[band].first;
            for (int k = 0; k < m->band[band].count && x < end; k++) {
                int64_t a = it->x + (int64_t)span[k].x0 * f, b = it->x + (int64_t)span[k].x1 * f;
                if (b <= x) continue;
                if (a >= end) break;
                if (a > x) { blend_run(dst + x, SOURCE(x), a - x, scale); x = a; }
                if (b > end) b = end;
                copy_run(dst + x, SOURCE(x), b - x, scale); x = b;
            }
        }
        if (x < end) blend_run(dst + x, SOURCE(x), end - x, scale);
#undef SOURCE
    }
}
static void draw_background(struct popup_box p) {
    if (aurora_mode && painting) { aurora_output_wallpaper(painting, canvas, stride, p.x, p.y, p.w, p.h); return; }
    for (int32_t y = p.y; y < p.y + p.h; y++) for (int32_t x = p.x; x < p.x + p.w; x++) canvas[(size_t)y * stride + x] = 0x101522;
}
static void draw_piece(struct popup_box p, ssize_t start) {
    if (start < 0) { draw_background(p); start = 0; }
    for (size_t j = (size_t)start; j < item_count; j++) { struct popup_box c; if (box_intersect(p, items[j].box, &c)) draw_item(&items[j], c); }
    stats.drawn_pixels += (uint64_t)box_area(p);
}
/* Painter's algorithm per damage rectangle, starting each piece at the topmost opaque cover. */
#define PIECES 96
static void paint_rect(struct popup_box r) {
    struct popup_box pending[PIECES], next[PIECES]; int count = 1; pending[0] = r;
    for (ssize_t i = (ssize_t)item_count - 1; i >= 0 && count; i--) {
        const struct item* it = &items[i]; const struct opaque_map* m = &it->s->omap;
        for (int o = 0; o < m->occluders && count; o++) {
            int64_t ox = it->x + (int64_t)m->occluder[o].x * it->k, oy = it->y + (int64_t)m->occluder[o].y * it->k;
            int64_t ow = (int64_t)m->occluder[o].w * it->k, oh = (int64_t)m->occluder[o].h * it->k;
            if (it->k <= 0) {   /* fractional output: the covered pixels, shrunk by one for the sampling's rounding */
                int64_t q = it->k120, l = ((int64_t)m->occluder[o].x * q + 119) / 120 + 1, t = ((int64_t)m->occluder[o].y * q + 119) / 120 + 1;
                int64_t r = ((int64_t)m->occluder[o].x + m->occluder[o].w) * q / 120 - 1, b = ((int64_t)m->occluder[o].y + m->occluder[o].h) * q / 120 - 1;
                ox = it->x + l; oy = it->y + t; ow = r - l; oh = b - t;
                if (ow <= 0 || oh <= 0) continue;
            }
            if (ox < INT32_MIN / 2 || oy < INT32_MIN / 2 || ox > INT32_MAX / 2 || oy > INT32_MAX / 2 || ow > INT32_MAX / 2 || oh > INT32_MAX / 2) continue;
            struct popup_box cover = {(int32_t)ox, (int32_t)oy, (int32_t)ow, (int32_t)oh}; int made = 0;
            for (int k = 0; k < count; k++) {
                struct popup_box hit, p = pending[k];
                if (!box_intersect(p, cover, &hit)) { next[made++] = p; continue; }
                if (made + 4 > PIECES) { draw_piece(p, -1); continue; }
                draw_piece(hit, i);
                if (hit.y > p.y) next[made++] = (struct popup_box){p.x, p.y, p.w, hit.y - p.y};
                if (hit.y + hit.h < p.y + p.h) next[made++] = (struct popup_box){p.x, hit.y + hit.h, p.w, p.y + p.h - hit.y - hit.h};
                if (hit.x > p.x) next[made++] = (struct popup_box){p.x, hit.y, hit.x - p.x, hit.h};
                if (hit.x + hit.w < p.x + p.w) next[made++] = (struct popup_box){hit.x + hit.w, hit.y, p.x + p.w - hit.x - hit.w, hit.h};
            }
            memcpy(pending, next, (size_t)made * sizeof(*next)); count = made;
        }
    }
    for (int k = 0; k < count; k++) draw_piece(pending[k], -1);
}
static bool damage_hits(struct popup_box b) {
    for (int i = 0; i < screen_damage_count; i++) if (box_intersect(screen_damage[i], b, NULL)) return true;
    return false;
}
/* Turn scene changes since the last paint into screen damage. */
static void collect_damage(void) {
    paint_serial++;
    for (size_t i = 0; i < item_count; i++) {
        struct surface* s = items[i].s; struct popup_box now = items[i].box; s->listed = paint_serial;
        if (!s->was_shown || !box_equal(s->shown, now)) { if (s->was_shown) damage_screen(s->shown); damage_screen(now); output_membership_mask(s, screens_of(now)); }
        s->shown = now; s->was_shown = true;
        for (int k = 0; k < s->content_damage.n; k++) {
            struct popup_box r = s->content_damage.r[k], hit;
            int64_t x = items[i].x + r.x, y = items[i].y + r.y;
            if (x < INT32_MIN / 2 || y < INT32_MIN / 2 || x > INT32_MAX / 2 || y > INT32_MAX / 2) continue;
            if (box_intersect((struct popup_box){(int32_t)x, (int32_t)y, r.w, r.h}, now, &hit)) damage_screen(hit);
        }
    }
    struct surface* s;
    wl_list_for_each(s, &surfaces, link) {
        if (s->listed != paint_serial && s->was_shown) { damage_screen(s->shown); s->was_shown = false; }
        rects_clear(&s->content_damage);
    }
    size_t same = 0;
    while (same < item_count && same < last_count && items[same].s == last_order[same]) same++;
    for (size_t i = same; i < item_count; i++) damage_screen(items[i].box);
    if (item_count > last_capacity) {
        struct surface** grown = realloc(last_order, item_count * sizeof(*grown));
        if (grown) { last_order = grown; last_capacity = item_count; }
    }
    if (item_count <= last_capacity) { for (size_t i = 0; i < item_count; i++) last_order[i] = items[i].s; last_count = item_count; }
    else { last_count = 0; damage_screen(screen_box()); }
}
/* Cursor in effect: 0 hidden, 1 Aurora's arrow, 2 the pointer-focus client's cursor surface; box is its screen rectangle. */
static int cursor_current(struct popup_box* box, struct surface** image) {
    struct endpoint* e = cursor_owner; *image = NULL;
    if (dnd_cursor(box, image)) return 2;   /* the drag icon follows the pointer */
    if (e && e->cursor_set && endpoint_for(e, pointer_focus) && !moving && !shell_hit(pointer_x, pointer_y)) {
        struct surface* s = e->cursor;
        if (!s || !s->resource || !s->pixels || s->view.w <= 0 || s->view.h <= 0) return 0;
        int64_t x = (int64_t)pointer_x - e->hot_x, y = (int64_t)pointer_y - e->hot_y;
        if (x < -4096 || y < -4096 || x > 65536 || y > 65536) return 0;
        *box = (struct popup_box){(int32_t)x, (int32_t)y, s->view.w > 256 ? 256 : s->view.w, s->view.h > 256 ? 256 : s->view.h}; *image = s; return 2;
    }
    int x, y, w, h; aurora_cursor_bounds(pointer_x, pointer_y, aurora_mode, &x, &y, &w, &h); *box = (struct popup_box){x, y, w, h}; return 1;
}
static bool send_frames(void) {
    uint32_t time = milliseconds(); struct surface* s; bool sent = false;
    struct popup_box cursor_box; struct surface* image;
    if (cursor_current(&cursor_box, &image) == 2 && !image->mapped) {
        struct frame *f, *next;
        wl_list_for_each_safe(f, next, &image->frames, link) if (f->state == FRAME_READY) { wl_callback_send_done(f->resource, time); wl_resource_destroy(f->resource); sent = true; }
    }
    wl_list_for_each(s, &surfaces, link) if (s->mapped) {
        struct frame *f, *next;
        wl_list_for_each_safe(f, next, &s->frames, link) if (f->state == FRAME_READY) { wl_callback_send_done(f->resource, time); wl_resource_destroy(f->resource); sent = true; }
    }
    return sent;
}
/* Copy the painted damage (scene) plus old/new cursor rectangles to video, drawing the cursor
 * into the canvas under a save. Between paints the canvas equals the screen, so a cursor-only
 * present leaves pending damage for the next paint. */
/* Output-local physical rectangle of desktop rectangle b on output o (false when they do not meet): both edges round
 * down, as item edges do (aurora_output_phys). */
static bool output_rect(const struct aurora_output* o, struct popup_box b, struct popup_box* out) {
    struct popup_box hit; if (!box_intersect(b, (struct popup_box){o->x, o->y, o->w, o->h}, &hit)) return false;
    int64_t x0 = AURORA_PHYS_X(o, hit.x), y0 = AURORA_PHYS_Y(o, hit.y), x1 = AURORA_PHYS_X(o, (int64_t)hit.x + hit.w), y1 = AURORA_PHYS_Y(o, (int64_t)hit.y + hit.h);
    if (x1 <= x0 || y1 <= y0) return false;
    *out = (struct popup_box){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)}; return true;
}
/* Damage: on a fractional output one pixel more on each side (a pixel samples its neighbour's logical column). */
static bool output_damage(const struct aurora_output* o, struct popup_box b, struct popup_box* out) {
    if (!output_rect(o, b, out)) return false;
    if (o->scale120 % 120) {
        int x0 = out->x > 0 ? out->x - 1 : 0, y0 = out->y > 0 ? out->y - 1 : 0;
        int x1 = out->x + out->w < o->mode_w ? out->x + out->w + 1 : o->mode_w, y1 = out->y + out->h < o->mode_h ? out->y + out->h + 1 : o->mode_h;
        *out = (struct popup_box){x0, y0, x1 - x0, y1 - y0};
    }
    return true;
}
/* An item (desktop coordinates, logical size) placed on output o: physical position, size, view and scale. */
static struct item item_on(const struct aurora_output* o, struct item it, int bw, int bh) {
    int64_t x1 = AURORA_PHYS_X(o, it.x + it.w), y1 = AURORA_PHYS_Y(o, it.y + it.h);
    it.x = AURORA_PHYS_X(o, it.x); it.y = AURORA_PHYS_Y(o, it.y);
    int64_t pw = x1 - it.x, ph = y1 - it.y; if (pw < 0) pw = 0; if (ph < 0) ph = 0;
    it.v = view_output(&it.v, pw, ph, bw, bh); it.w = (int32_t)pw; it.h = (int32_t)ph;
    it.k = o->scale120 % 120 ? 0 : o->scale120 / 120; it.k120 = o->scale120;
    return it;
}
static void present(bool scene) {
    static uint32_t saved[AURORA_CURSOR_SAVE]; struct popup_box box = {0}; struct surface* image;
    int shape = cursor_current(&box, &image);
    for (int slot = 0; slot < AURORA_OUTPUTS; slot++) {
        struct aurora_output* o = aurora_output_slot(slot);
        if (!o || !o->enabled || !o->canvas) continue;
        struct popup_box clip = {0}; bool shown = shape && output_rect(o, box, &clip);
        canvas = o->canvas; stride = (unsigned)o->mode_w; aurora_canvas_scaled(o->canvas, o->mode_w, o->mode_h, o->scale120);
        int lx = pointer_x - o->x, ly = pointer_y - o->y;
        if (shape == 1 && shown) {   /* the arrow at the output's scale: its physical box, clipped to the output */
            int l, t, w, h; aurora_cursor_box(lx, ly, aurora_mode, &l, &t, &w, &h);
            shown = w > 0 && box_intersect((struct popup_box){l, t, w, h}, (struct popup_box){0, 0, o->mode_w, o->mode_h}, &clip);
        }
        int32_t rects[SCREEN_RECTS + 2][4]; int n = 0;
        if (shape == 2 && shown) {   /* client cursor: save the canvas under it and draw the surface there */
            size_t need = (size_t)clip.w * clip.h;
            if (need > cursor_save_size) { uint32_t* grown = realloc(cursor_save, need * 4); if (grown) { cursor_save = grown; cursor_save_size = need; } else shown = false; }
            if (shown) {
                for (int32_t y = 0; y < clip.h; y++) memcpy(cursor_save + (size_t)y * clip.w, canvas + (size_t)(clip.y + y) * stride + clip.x, (size_t)clip.w * 4);
                struct item it = item_on(o, (struct item){image, box.x, box.y, box.w, box.h, clip, image->view, 1, 120}, image->width, image->height);
                it.box = clip; draw_item(&it, clip);
            }
        } else if (shape == 1 && shown) aurora_cursor_draw_scaled(lx, ly, aurora_mode, saved);
        for (int i = 0; scene && i < screen_damage_count; i++) {
            struct popup_box r; if (!output_damage(o, screen_damage[i], &r)) continue;
            rects[n][0] = r.x; rects[n][1] = r.y; rects[n][2] = r.w; rects[n][3] = r.h; n++; stats.present_bytes += (uint64_t)box_area(r) * 4;
        }
        struct popup_box old = output_cursor[slot];
        if (old.w > 0 && !(shown && box_equal(old, clip))) { rects[n][0] = old.x; rects[n][1] = old.y; rects[n][2] = old.w; rects[n][3] = old.h; n++; }
        if (shown) { rects[n][0] = clip.x; rects[n][1] = clip.y; rects[n][2] = clip.w; rects[n][3] = clip.h; n++; stats.present_bytes += (uint64_t)box_area(clip) * 4; }
        aurora_output_present(o, (const int32_t (*)[4])rects, n);
        if (shape == 2 && shown) { for (int32_t y = 0; y < clip.h; y++) memcpy(canvas + (size_t)(clip.y + y) * stride + clip.x, cursor_save + (size_t)y * clip.w, (size_t)clip.w * 4); }
        else if (shape == 1 && shown) aurora_cursor_restore_scaled(lx, ly, aurora_mode, saved);
        output_cursor[slot] = shown ? clip : (struct popup_box){0, 0, 0, 0};
    }
    cursor_x = pointer_x; cursor_y = pointer_y; cursor_shown = true; cursor_dirty = false;
    if (scene) screen_damage_count = 0;
}
static void present_cursor(void) { present(false); stats.cursor_only++; }
/* One output's share of a paint pass: the damage inside it, drawn into its canvas in output pixels (scale k). */
static struct item* output_items; static size_t output_capacity;
static void paint_output(struct aurora_output* o, unsigned draw) {
    if (item_count > output_capacity) {
        struct item* grown = realloc(output_items, item_count * sizeof(*grown));
        if (!grown) return;
        output_items = grown; output_capacity = item_count;
    }
    for (size_t j = 0; j < item_count; j++) {
        struct item it = item_on(o, items[j], items[j].s->width, items[j].s->height); struct popup_box b;
        it.box = output_rect(o, items[j].box, &b) && box_intersect(b, (struct popup_box){(int32_t)it.x, (int32_t)it.y, it.w, it.h}, &b) ? b : (struct popup_box){0, 0, 0, 0};
        output_items[j] = it;
    }
    struct item* desktop_items = items; items = output_items;
    canvas = o->canvas; stride = (unsigned)o->mode_w; painting = o;
    bool launchpad = o->primary && (draw & (1U << SHELL_LAUNCHPAD));   /* Launchpad covers the primary screen whole */
    for (int i = 0; i < screen_damage_count && !launchpad; i++) { struct popup_box r; if (output_damage(o, screen_damage[i], &r)) paint_rect(r); }
    if (o->primary) {
        aurora_canvas_scaled(o->canvas, o->mode_w, o->mode_h, o->scale120);
        shell_draw((launchpad ? draw & ~((1U << SHELL_DOCK) | (1U << SHELL_MENUBAR) | (1U << SHELL_MENU)) : draw) & ~SHELL_DESKTOP_LAYERS);
    }
    if (draw & SHELL_DESKTOP_LAYERS) {   /* the snap preview and the window manager's overlay, on every display they cover */
        aurora_canvas_scaled(o->canvas, o->mode_w, o->mode_h, o->scale120);   /* logical, output-relative drawing */
        struct popup_box r = wm_snap_rect(), local;
        if ((draw & (1U << SHELL_SNAP)) && moving && wm_snap_kind && output_rect(o, r, &local)) {
            int x = r.x - o->x, y = r.y - o->y;
            ui_rrect(x + 8, y + 8, r.w - 16, r.h - 16, 12, 0x5A9CFF, 70);
            ui_rrect_stroke(x + 8, y + 8, r.w - 16, r.h - 16, 12, 0xFFFFFF, 110);
        }
        if (draw & (1U << SHELL_WM)) aurora_wm_draw(o->x, o->y, o->w, o->h, 1);
        if (draw & (1U << SHELL_LOCK)) aurora_lock_draw(o->x, o->y, o->w, o->h, o->primary);
        struct popup_box lens, lens_local;   /* the magnifier, last: it enlarges what was composed under it */
        if ((draw & (1U << SHELL_ZOOM)) && aurora_a11y_lens(pointer_x, pointer_y, &lens.x, &lens.y, &lens.w, &lens.h) && output_rect(o, lens, &lens_local)) {
            int64_t lx = AURORA_PHYS_X(o, lens.x), ly = AURORA_PHYS_Y(o, lens.y), lw = AURORA_PHYS_X(o, (int64_t)lens.x + lens.w) - lx, lh = AURORA_PHYS_Y(o, (int64_t)lens.y + lens.h) - ly;
            aurora_a11y_magnify(o->canvas, o->mode_w, o->mode_h, (size_t)o->mode_w, (int)lx, (int)ly, (int)lw, (int)lh, (int)AURORA_PHYS_X(o, pointer_x), (int)AURORA_PHYS_Y(o, pointer_y));
        }
    }
    items = desktop_items; painting = NULL;
}
static struct popup_box shell_global(struct popup_box b) { struct aurora_output* p = aurora_output_primary(); b.x += p->x; b.y += p->y; return b; }
static void paint(void) {
    uint64_t start = cpu_nanoseconds(CLOCK_THREAD_CPUTIME_ID);
    if (aurora_mode) { struct surface* s; wl_list_for_each(s, &windows, window_link) if (s->x11) aurora_xwm_sync(s->x11, s->x, s->y, s->minimized); }   /* X frames follow moves */
    if (ime_scene) { ime_scene = false; refresh_scene(); }
    items_build(); collect_damage();
    struct popup_box bounds[SHELL_COUNT]; bool visible[SHELL_COUNT]; unsigned draw = 0;   /* bounds: primary-output coordinates */
    bool covered = shell_was[SHELL_LAUNCHPAD] && shell_element(SHELL_LAUNCHPAD, &bounds[SHELL_LAUNCHPAD]) && !(shell_dirty & (1U << SHELL_LAUNCHPAD));
    if (covered && aurora_outputs_count() == 1) screen_damage_count = 0;   /* Launchpad hides every window; their updates cannot show. */
    bool launchpad = launchpad_is_open();
    for (int e = 0; e < SHELL_COUNT; e++) {
        /* Launchpad paints the whole screen; the bar layers beneath it cannot change what is seen. */
        bool hidden = launchpad && (e == SHELL_DOCK || e == SHELL_MENUBAR || e == SHELL_MENU);
        visible[e] = !hidden && shell_element(e, &bounds[e]);
        if (hidden) bounds[e] = (struct popup_box){0};
        if (visible[e] != shell_was[e] || (visible[e] && !box_equal(bounds[e], shell_shown[e])) || (shell_dirty & (1U << e))) {
            if (shell_was[e]) damage_screen(shell_global(shell_shown[e]));
            if (visible[e]) damage_screen(shell_global(bounds[e]));
        }
        shell_was[e] = visible[e]; shell_shown[e] = bounds[e];
    }
    shell_dirty = 0;
    /* A repainted base erases any shell element over it: redraw those whole (fixpoint, they overlap). */
    for (bool grew = true; grew;) {
        grew = false;
        for (int e = 0; e < SHELL_COUNT; e++)
            if (visible[e] && !(draw & (1U << e)) && damage_hits(shell_global(bounds[e]))) { draw |= 1U << e; damage_screen(shell_global(bounds[e])); grew = true; }
    }
    bool drew = screen_damage_count > 0;
    uint64_t staged = cpu_nanoseconds(CLOCK_THREAD_CPUTIME_ID);
    if (drew) for (int slot = 0; slot < AURORA_OUTPUTS; slot++) {
        struct aurora_output* o = aurora_output_slot(slot);
        if (o && o->enabled && o->canvas) paint_output(o, draw);
    }
    uint64_t drawn = cpu_nanoseconds(CLOCK_THREAD_CPUTIME_ID);
    if (drew || cursor_dirty) present(true);
    if (drew) { uint64_t presented = cpu_nanoseconds(CLOCK_THREAD_CPUTIME_ID); stats.stage_ns[0] += staged - start; stats.stage_ns[1] += drawn - staged; stats.stage_ns[2] += presented - drawn; }
    bool framed = send_frames(); paint_asap = false;
    if (stats_enabled && drew) { uint64_t at = nanoseconds(); if (stats.first_commit) { stats.commit_wait_ns += at - stats.first_commit; stats.waits++; } stats.first_commit = 0; stats.frame_sent = at; }
    dirty = false;
    uint64_t cost = cpu_nanoseconds(CLOCK_THREAD_CPUTIME_ID) - start;
    if (drew || framed) last_paint_ns = nanoseconds();   /* frame callbacks alone also follow the cadence */
    if (drew) { stats.paints++; stats.paint_ns += cost; if (cost > stats.paint_max_ns) stats.paint_max_ns = cost; }
}
struct mime_type { struct mime_type* next; char name[]; };
struct source { struct wl_resource* resource; struct mime_type* types; unsigned references; bool used; uint32_t actions; char* owned; size_t owned_size; bool x11; };   /* owned: Aurora's own text (clipboard history); x11: the X CLIPBOARD (aurora_xwm.c) */
struct data_device { struct wl_list link; struct wl_resource* resource; };
static struct source* selection;
static void source_release(struct source* source) {
    if(--source->references)return;
    while(source->types){struct mime_type* next=source->types->next;free(source->types);source->types=next;}
    free(source->owned);free(source);
}
static bool focused_client(struct wl_client* client) { return focus&&focus->resource&&wl_resource_get_client(focus->resource)==client; }
static bool source_has(struct source* source,const char* mime) {
    for(struct mime_type* type=source->types;type;type=type->next)if(!strcmp(type->name,mime))return true;
    return false;
}
static void offer_destroy(struct wl_resource* resource) { source_release(wl_resource_get_user_data(resource)); }
static void offer_accept(struct wl_client* client,struct wl_resource* resource,uint32_t serial,const char* mime) {
    (void)client;(void)resource;(void)serial;(void)mime; /* Clipboard offers do not negotiate drag acceptance. */
}
static void offer_receive(struct wl_client* client,struct wl_resource* resource,const char* mime,int32_t fd) {
    struct source* source=wl_resource_get_user_data(resource);
    if(source==selection&&source->owned&&focused_client(client)&&source_has(source,mime)){aurora_clipboard_serve(fd,source->owned,source->owned_size);return;}   /* aurora_clipboard.h */
    if(source==selection&&source->x11&&focused_client(client)&&source_has(source,mime)){aurora_xwm_selection_send(mime,fd);return;}   /* an X program's copy */
    if(source==selection&&source->resource&&focused_client(client)&&source_has(source,mime))wl_data_source_send_send(source->resource,mime,fd);
    close(fd);
}
static void offer_finish(struct wl_client* client,struct wl_resource* resource){(void)client;wl_resource_post_error(resource,WL_DATA_OFFER_ERROR_INVALID_FINISH,"not a drag-and-drop offer");}
static void offer_actions(struct wl_client* client,struct wl_resource* resource,uint32_t actions,uint32_t preferred){(void)client;(void)actions;(void)preferred;wl_resource_post_error(resource,WL_DATA_OFFER_ERROR_INVALID_OFFER,"not a drag-and-drop offer");}
static const struct wl_data_offer_interface offer_interface={.accept=offer_accept,.receive=offer_receive,.destroy=destroy_request,.finish=offer_finish,.set_actions=offer_actions};
static void selection_offer(struct data_device* device) {
    struct wl_client* client=wl_resource_get_client(device->resource);
    if(!selection||(!selection->resource&&!selection->owned&&!selection->x11)||!focused_client(client)){wl_data_device_send_selection(device->resource,NULL);return;}
    struct wl_resource* offer=wl_resource_create(client,&wl_data_offer_interface,wl_resource_get_version(device->resource),0);
    if(!offer){wl_client_post_no_memory(client);return;}
    selection->references++;wl_resource_set_implementation(offer,&offer_interface,selection,offer_destroy);
    wl_data_device_send_data_offer(device->resource,offer);
    for(struct mime_type* type=selection->types;type;type=type->next)wl_data_offer_send_offer(offer,type->name);
    wl_data_device_send_selection(device->resource,offer);
}
static void selection_focus(void) { struct data_device* device;wl_list_for_each(device,&data_devices,link)selection_offer(device);
    if(aurora_mode)aurora_xwm_selection_changed(selection,selection&&selection->x11); }   /* the X CLIPBOARD follows */
static void dnd_source_gone(struct source* source);
static void source_destroy(struct wl_resource* resource) {
    struct source* source=wl_resource_get_user_data(resource);source->resource=NULL;
    dnd_source_gone(source);   /* its drag ends with it */
    if(selection==source){selection=NULL;source_release(source);selection_focus();}
    source_release(source);
}
static void source_offer(struct wl_client* client,struct wl_resource* resource,const char* mime) {
    (void)client;struct source* source=wl_resource_get_user_data(resource);
    if(source->used){wl_resource_post_error(resource,WL_DATA_SOURCE_ERROR_INVALID_SOURCE,"cannot change an active source");return;}
    if(source_has(source,mime))return;
    size_t length=strlen(mime);if(length>SIZE_MAX-sizeof(struct mime_type)-1){wl_resource_post_no_memory(resource);return;}
    struct mime_type* type=malloc(sizeof(*type)+length+1);if(!type){wl_resource_post_no_memory(resource);return;}
    memcpy(type->name,mime,length+1);type->next=source->types;source->types=type;
}
#define DND_ACTIONS (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY|WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE|WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
static void source_set_actions(struct wl_client* client,struct wl_resource* resource,uint32_t actions){
    (void)client;struct source* source=wl_resource_get_user_data(resource);
    if(actions&~DND_ACTIONS){wl_resource_post_error(resource,WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,"unknown drag-and-drop action");return;}
    if(source->used){wl_resource_post_error(resource,WL_DATA_SOURCE_ERROR_INVALID_SOURCE,"cannot change an active source");return;}
    source->actions=actions;
}
static const struct wl_data_source_interface source_interface={.offer=source_offer,.destroy=destroy_request,.set_actions=source_set_actions};
static void create_source(struct wl_client* client,struct wl_resource* manager,uint32_t id) {
    struct source* source=calloc(1,sizeof(*source));struct wl_resource* resource=wl_resource_create(client,&wl_data_source_interface,wl_resource_get_version(manager),id);
    if(!source||!resource){free(source);if(resource)wl_resource_destroy(resource);wl_resource_post_no_memory(manager);return;}
    source->resource=resource;source->references=1;wl_resource_set_implementation(resource,&source_interface,source,source_destroy);
}
static void clipboard_record(struct source* source);
static void set_selection(struct wl_client* client,struct wl_resource* resource,struct wl_resource* offered,uint32_t serial) {
    if(!focused_client(client)||!serial||(int32_t)(wl_display_get_serial(display)-serial)<0)return;
    struct source* source=offered?wl_resource_get_user_data(offered):NULL;
    if(source==selection)return;
    if(source&&source->used){wl_resource_post_error(resource,WL_DATA_SOURCE_ERROR_INVALID_SOURCE,"source already used");return;}
    if(selection){if(selection->resource)wl_data_source_send_cancelled(selection->resource);source_release(selection);}
    selection=source;if(source){source->used=true;source->references++;}selection_focus();
    if(source&&aurora_mode)clipboard_record(source);
}
/* Clipboard history (aurora_clipboard.h): Aurora reads the new selection's text - unless it comes from a password field. */
static const char* const text_types[]={"text/plain;charset=utf-8","UTF8_STRING","text/plain","TEXT","STRING"};
static void clipboard_record(struct source* source){
    const char* mime=NULL;for(unsigned i=0;i<sizeof(text_types)/sizeof(*text_types)&&!mime;i++)if(source_has(source,text_types[i]))mime=text_types[i];
    if(!mime||(!source->resource&&!source->x11))return;
    if(source_has(source,"x-kde-passwordManagerHint")||aurora_ime_secret_focus()||aurora_lock_active()){puts("[64] AURORA CLIPBOARD private (password field): not kept");fflush(stdout);return;}
    int p[2];if(pipe2(p,O_CLOEXEC))return;
    if(source->x11)aurora_xwm_selection_send(mime,p[1]);else{wl_data_source_send_send(source->resource,mime,p[1]);close(p[1]);}
    aurora_clipboard_capture(p[0],aurora_session_active_uid());
}
bool aurora_host_set_text_selection(const char* data,size_t size){
    struct source* source=calloc(1,sizeof(*source));if(!source)return false;
    source->owned=malloc(size?size:1);if(!source->owned){free(source);return false;}memcpy(source->owned,data,size);source->owned_size=size;
    for(int i=(int)(sizeof(text_types)/sizeof(*text_types))-1;i>=0;i--){size_t n=strlen(text_types[i]);struct mime_type* t=malloc(sizeof(*t)+n+1);if(!t)break;memcpy(t->name,text_types[i],n+1);t->next=source->types;source->types=t;}
    source->references=1;source->used=true;
    if(selection){if(selection->resource)wl_data_source_send_cancelled(selection->resource);source_release(selection);}
    selection=source;selection_focus();return true;
}
/* ---- drag and drop (wl_data_device v1-v3) ----
 * A drag starts from the surface a still-held button was pressed on (its serial); the pointer then belongs to the drag:
 * the surface under it gets wl_data_device enter/motion/leave with a fresh offer of the source's MIME types (only the
 * drag's own client when it has no source), the target accepts a type and (v3) negotiates copy/move/ask; releasing the
 * button drops on an accepting target (dnd_drop_performed, then dnd_finished on its finish) or cancels. Esc cancels.
 * The drag icon is drawn in place of the cursor. Inactive sessions' windows are parked, so drags never cross users. */
static struct { bool active, dropped; struct source* source; struct wl_client* origin; struct surface* icon, *target;
                struct wl_resource* offers[4]; int offers_used; bool accepted; uint32_t offer_actions, preferred, action, mask; } dnd;
static void dnd_end(void);
static void dnd_offer_destroy(struct wl_resource* resource){
    bool listed=false;for(int i=0;i<dnd.offers_used;i++)if(dnd.offers[i]==resource){dnd.offers[i]=dnd.offers[--dnd.offers_used];listed=true;break;}
    struct source* source=wl_resource_get_user_data(resource);if(source)source_release(source);
    if(listed&&dnd.dropped&&!dnd.offers_used){   /* the target is done (v1/v2), or gave up without finish (v3: cancelled) */
        if(dnd.source&&dnd.source->resource&&wl_resource_get_version(dnd.source->resource)>=3)wl_data_source_send_cancelled(dnd.source->resource);
        dnd_end();}
}
static bool dnd_current(struct wl_resource* offer){for(int i=0;i<dnd.offers_used;i++)if(dnd.offers[i]==offer)return true;return false;}
static void dnd_choose_action(void){
    uint32_t source_actions=dnd.source?dnd.source->actions:0,common=source_actions&dnd.offer_actions,action=0;
    if(common&dnd.preferred)action=dnd.preferred;else if(common&WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)action=WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    else if(common&WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)action=WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;else if(common)action=WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
    if(action==dnd.action)return;dnd.action=action;
    for(int i=0;i<dnd.offers_used;i++)if(wl_resource_get_version(dnd.offers[i])>=3)wl_data_offer_send_action(dnd.offers[i],action);
    if(dnd.source&&dnd.source->resource&&wl_resource_get_version(dnd.source->resource)>=3)wl_data_source_send_action(dnd.source->resource,action);
}
static void dnd_offer_accept(struct wl_client* client,struct wl_resource* resource,uint32_t serial,const char* mime){
    (void)client;(void)serial;if(!dnd_current(resource)||dnd.dropped)return;
    dnd.accepted=mime!=NULL;if(dnd.source&&dnd.source->resource)wl_data_source_send_target(dnd.source->resource,mime);
}
static void dnd_offer_receive(struct wl_client* client,struct wl_resource* resource,const char* mime,int32_t fd){
    (void)client;struct source* source=wl_resource_get_user_data(resource);
    if(source&&source->resource&&source==dnd.source&&dnd_current(resource)&&source_has(source,mime))wl_data_source_send_send(source->resource,mime,fd);
    close(fd);
}
static void dnd_end(void){   /* offers left with the clients become inert */
    for(int i=0;i<dnd.offers_used;i++){struct source* held=wl_resource_get_user_data(dnd.offers[i]);if(held)source_release(held);wl_resource_set_user_data(dnd.offers[i],NULL);}
    if(dnd.source)source_release(dnd.source);
    bool icon=dnd.icon!=NULL;memset(&dnd,0,sizeof(dnd));if(icon)cursor_dirty=true;
}
static void dnd_offer_finish(struct wl_client* client,struct wl_resource* resource){
    (void)client;
    if(!dnd.dropped||!dnd_current(resource)||!dnd.accepted||(wl_resource_get_version(resource)>=3&&!dnd.action)){wl_resource_post_error(resource,WL_DATA_OFFER_ERROR_INVALID_FINISH,"finish without a completed drop");return;}
    if(dnd.source&&dnd.source->resource&&wl_resource_get_version(dnd.source->resource)>=3)wl_data_source_send_dnd_finished(dnd.source->resource);
    printf("[64] AURORA DND FINISHED action=%u\n",dnd.action);fflush(stdout);dnd_end();
}
static void dnd_offer_set_actions(struct wl_client* client,struct wl_resource* resource,uint32_t actions,uint32_t preferred){
    (void)client;
    if(actions&~DND_ACTIONS){wl_resource_post_error(resource,WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,"unknown drag-and-drop action");return;}
    if(preferred&&((preferred&(preferred-1))||!(preferred&actions))){wl_resource_post_error(resource,WL_DATA_OFFER_ERROR_INVALID_ACTION,"preferred action must be one of the actions");return;}
    if(!dnd_current(resource)||dnd.dropped)return;
    dnd.offer_actions=actions;dnd.preferred=preferred;dnd_choose_action();
}
static const struct wl_data_offer_interface dnd_offer_interface={.accept=dnd_offer_accept,.receive=dnd_offer_receive,.destroy=destroy_request,.finish=dnd_offer_finish,.set_actions=dnd_offer_set_actions};
static void dnd_leave(void){
    if(!dnd.target||!dnd.target->resource){dnd.target=NULL;return;}
    struct wl_client* client=wl_resource_get_client(dnd.target->resource);struct data_device* d;
    wl_list_for_each(d,&data_devices,link)if(wl_resource_get_client(d->resource)==client)wl_data_device_send_leave(d->resource);
    for(int i=0;i<dnd.offers_used;i++){struct source* source=wl_resource_get_user_data(dnd.offers[i]);if(source)source_release(source);wl_resource_set_user_data(dnd.offers[i],NULL);}
    dnd.offers_used=0;dnd.target=NULL;dnd.accepted=false;dnd.offer_actions=dnd.preferred=0;dnd.action=0;
    if(dnd.source&&dnd.source->resource)wl_data_source_send_target(dnd.source->resource,NULL);
}
static void dnd_enter(struct surface* target,int64_t x,int64_t y){
    dnd.target=target;struct wl_client* client=wl_resource_get_client(target->resource);
    if(!dnd.source&&client!=dnd.origin)return;   /* a drag without a source stays inside its client */
    uint32_t serial=wl_display_next_serial(display);struct data_device* d;
    wl_list_for_each(d,&data_devices,link)if(wl_resource_get_client(d->resource)==client){
        struct wl_resource* offer=NULL;
        if(dnd.source&&dnd.offers_used<4&&(offer=wl_resource_create(client,&wl_data_offer_interface,wl_resource_get_version(d->resource),0))){
            dnd.source->references++;wl_resource_set_implementation(offer,&dnd_offer_interface,dnd.source,dnd_offer_destroy);dnd.offers[dnd.offers_used++]=offer;
            wl_data_device_send_data_offer(d->resource,offer);
            for(struct mime_type* type=dnd.source->types;type;type=type->next)wl_data_offer_send_offer(offer,type->name);
            if(wl_resource_get_version(offer)>=3)wl_data_offer_send_source_actions(offer,dnd.source->actions);
        }
        wl_data_device_send_enter(d->resource,serial,target->resource,coordinate(x),coordinate(y),offer);
    }
}
static bool dnd_pointer(void){
    if(!dnd.active)return false;
    struct surface *root,*target=NULL;int64_t x=0,y=0;
    wl_list_for_each_reverse(root,&windows,window_link){if(!root->mapped)continue;if((target=hit_family(root)))break;}
    if(!aurora_mode)wl_list_for_each_reverse(root,&surfaces,link){if(target)break;if(root->parent||!root->mapped||root->kind!=ROLE_TOPLEVEL)continue;target=hit_family(root);}
    if(target&&aurora_mode&&shell_hit(pointer_x,pointer_y))target=NULL;
    if(target&&(!target->resource||!surface_origin(target,&x,&y)))target=NULL;
    x=pointer_x-x;y=pointer_y-y;
    if(target!=dnd.target){dnd_leave();if(target)dnd_enter(target,x,y);}
    else if(target&&target->resource){struct wl_client* client=wl_resource_get_client(target->resource);struct data_device* d;uint32_t time=milliseconds();
        if(dnd.source||client==dnd.origin)wl_list_for_each(d,&data_devices,link)if(wl_resource_get_client(d->resource)==client)wl_data_device_send_motion(d->resource,time,coordinate(x),coordinate(y));}
    cursor_dirty=true;return true;
}
static void dnd_cancel(const char* why){
    printf("[64] AURORA DND CANCELLED %s\n",why);fflush(stdout);
    dnd_leave();if(dnd.source&&dnd.source->resource)wl_data_source_send_cancelled(dnd.source->resource);
    dnd_end();pointer_grab=NULL;pointer_update();
}
static bool dnd_button(unsigned code,bool down){
    if(!dnd.active)return false;
    uint32_t mask=1U<<(code-BTN_LEFT);
    if(down){pointer_buttons|=mask;return true;}
    pointer_buttons&=~mask;if(!(mask&dnd.mask))return true;
    bool v3=dnd.offers_used&&wl_resource_get_version(dnd.offers[0])>=3;
    if(dnd.target&&dnd.target->resource&&dnd.accepted&&(!v3||dnd.action)&&(dnd.source||!dnd.offers_used)){
        struct wl_client* client=wl_resource_get_client(dnd.target->resource);struct data_device* d;
        wl_list_for_each(d,&data_devices,link)if(wl_resource_get_client(d->resource)==client)wl_data_device_send_drop(d->resource);
        if(dnd.source&&dnd.source->resource&&wl_resource_get_version(dnd.source->resource)>=3)wl_data_source_send_dnd_drop_performed(dnd.source->resource);
        printf("[64] AURORA DND DROP app=%s action=%u\n",dnd.target->app_id[0]?dnd.target->app_id:popup_family(dnd.target)->app_id,dnd.action);fflush(stdout);
        dnd.active=false;dnd.dropped=true;   /* until the target's finish (v3) or until it destroys the offer */
        pointer_grab=NULL;pointer_update();return true;
    }
    dnd_cancel("no target accepted");return true;
}
static bool dnd_cursor(struct popup_box* box,struct surface** image){
    struct surface* s=dnd.active?dnd.icon:NULL;
    if(!s||!s->resource||!s->pixels||s->view.w<=0||s->view.h<=0)return false;
    *box=(struct popup_box){pointer_x,pointer_y,s->view.w>256?256:s->view.w,s->view.h>256?256:s->view.h};*image=s;return true;
}
static bool dnd_escape(void){if(!dnd.active)return false;pointer_buttons=0;dnd_cancel("escape");return true;}
static void dnd_source_gone(struct source* source){
    if(dnd.source!=source||(!dnd.active&&!dnd.dropped))return;
    bool was=dnd.active;if(was)dnd_leave();dnd_end();if(was){pointer_grab=NULL;pointer_update();}
}
static void dnd_surface_gone(struct surface* surface){
    if(dnd.icon==surface){dnd.icon=NULL;cursor_dirty=true;}
    if(dnd.target==surface){dnd_leave();}
}
static void start_drag(struct wl_client* client,struct wl_resource* resource,struct wl_resource* source_resource,struct wl_resource* origin,struct wl_resource* icon,uint32_t serial) {
    (void)resource;struct source* source=source_resource?wl_resource_get_user_data(source_resource):NULL;
    struct surface* from=origin?wl_resource_get_user_data(origin):NULL,*image=icon?wl_resource_get_user_data(icon):NULL;
    if(image&&image!=from&&(image->xdg||(image->kind!=ROLE_NONE&&image->kind!=ROLE_CURSOR))){wl_resource_post_error(resource,WL_DATA_DEVICE_ERROR_ROLE,"the drag icon has another role");return;}
    if(source&&source->used){wl_resource_post_error(resource,WL_DATA_SOURCE_ERROR_INVALID_SOURCE,"source already used");return;}
    /* only the client whose surface got the still-held button, with that button's serial */
    bool valid=!dnd.active&&!dnd.dropped&&pointer_buttons&&serial&&serial==button_serial&&from&&button_origin&&popup_family(button_origin)==popup_family(from)&&
               wl_resource_get_client(from->resource)==client&&!moving&&!(aurora_mode&&aurora_lock_active());
    if(!valid){if(source_resource)wl_data_source_send_cancelled(source_resource);return;}
    if(dnd.dropped)dnd_end();
    if(image){image->kind=ROLE_CURSOR;}
    dnd.active=true;dnd.source=source;dnd.origin=client;dnd.icon=image;dnd.mask=pointer_buttons;
    if(source){source->used=true;source->references++;}
    struct endpoint* e;uint32_t leave=wl_display_next_serial(display);   /* the pointer leaves the clients while the drag lasts */
    if(pointer_focus)wl_list_for_each(e,&endpoints,link)if(!e->keyboard&&endpoint_for(e,pointer_focus)){wl_pointer_send_leave(e->resource,leave,pointer_focus->resource);if(wl_resource_get_version(e->resource)>=5)wl_pointer_send_frame(e->resource);}
    pointer_focus=NULL;cursor_owner=NULL;
    int types=0;if(source)for(struct mime_type* t=source->types;t;t=t->next)types++;
    printf("[64] AURORA DND START app=%s types=%d\n",popup_family(from)->app_id,types);fflush(stdout);
    pointer_update();
}
static const struct wl_data_device_interface data_device_interface={.start_drag=start_drag,.set_selection=set_selection,.release=destroy_request};
static void data_device_destroy(struct wl_resource* resource) { struct data_device* device=wl_resource_get_user_data(resource);wl_list_remove(&device->link);free(device); }
static void get_data_device(struct wl_client* client,struct wl_resource* manager,uint32_t id,struct wl_resource* seat) {
    (void)seat;struct data_device* device=calloc(1,sizeof(*device));struct wl_resource* resource=wl_resource_create(client,&wl_data_device_interface,wl_resource_get_version(manager),id);
    if(!device||!resource){free(device);if(resource)wl_resource_destroy(resource);wl_resource_post_no_memory(manager);return;}
    device->resource=resource;wl_list_insert(data_devices.prev,&device->link);wl_resource_set_implementation(resource,&data_device_interface,device,data_device_destroy);selection_offer(device);
}
static const struct wl_data_device_manager_interface data_manager_interface={create_source,get_data_device};
static void bind_data_manager(struct wl_client* client,void* data,uint32_t version,uint32_t id) {
    (void)data;struct wl_resource* resource=wl_resource_create(client,&wl_data_device_manager_interface,version,id);
    if(!resource){wl_client_post_no_memory(client);return;}wl_resource_set_implementation(resource,&data_manager_interface,NULL,NULL);
}
static const ui_app_t aurora_apps[]={
    {"Files",2,ICON_FINDER,"Files"},{"Launchpad",UI_ACT_LAUNCHPAD,ICON_LAUNCHPAD,NULL},
    {"Chrome",32,ICON_CHROME,"Google Chrome"},{"Terminal",1,ICON_TERMINAL,"Terminal"},
    {"Notes",10,ICON_NOTES,"Notes"},{"Calendar",12,ICON_CALENDAR,"Calendar"},
    {"Calculator",3,ICON_CALC,"Calculator"},{"Paint",15,ICON_PAINT,"Paint"},
    {"Minesweeper",16,ICON_GENERIC,"Minesweeper"},{"Desktop Help",14,ICON_HELP,"Desktop Help"},
    {"About This NexusOS",UI_ACT_ABOUT,ICON_INFO,"About This NexusOS"},
    {"Visual Studio Code",40,ICON_VSCODE,NULL},{"Firefox",41,ICON_FIREFOX,NULL},
    {"LibreOffice Writer",42,ICON_WRITER,NULL},{"LibreOffice Calc",43,ICON_SHEET,NULL},
    {"X11 Desktop",44,ICON_MONITOR,NULL},  /* rootful Xwayland (wine_install.py --only xwayland) */
    {"Wine Configuration",45,ICON_SETTINGS,NULL},{"Windows Programs",46,ICON_FOLDER,NULL},   /* wine_install.py --only wine */
    {"Network",AURORA_ACTION_NETWORK,ICON_SETTINGS,"Network"},   /* aurora_network.c */
    {"Software",48,ICON_TASKS,NULL},   /* apt front end (sysinstall.py services: /nexus/apps/software.args) */
    {"System Settings",UI_ACT_SETTINGS,ICON_SETTINGS,"System Settings"}
};
static struct {int action;const char* name;const char* app_id;pid_t pid;} app_commands[]={
    {2,"files","nexus.files",0},{32,"chrome","google-chrome",0},{1,"terminal","nexus.terminal",0},
    {10,"notes","nexus.notes",0},{12,"calendar","nexus.calendar",0},{3,"calculator","nexus.calculator",0},
    {15,"paint","nexus.paint",0},{16,"minesweeper","nexus.minesweeper",0},{14,"help","nexus.help",0},
    {UI_ACT_ABOUT,"about","nexus.about",0},
    {40,"vscode","code",0},{41,"firefox","firefox",0},{42,"writer","libreoffice-writer",0},{43,"calc","libreoffice-calc",0},
    {44,"x11","org.freedesktop.Xwayland",0},{45,"winecfg","winecfg.exe",0},{46,"wineprograms","explorer.exe",0},
    {AURORA_ACTION_NETWORK,"network","nexus.network",0},
    {48,"software","nexus.software",0},
    {UI_ACT_SETTINGS,"settings","nexus.settings",0},
    {UI_ACT_NOTIFS,"notifications","nexus.notifications",0},   /* the Notification Centre (menu-bar bell) */
    {AURORA_ACTION_CLIPBOARD,"clipboard","nexus.clipboard",0}   /* Super+V: the clipboard history (aurora_clipboard.h) */
};
/* Installed desktop applications start from a launch file: KEY=VALUE lines, then the program and its arguments. */
static const struct {int action;const char* path;} app_configs[]={{32,"/nexus/chrome-desktop.args"},
    {40,"/nexus/apps/vscode.args"},{41,"/nexus/apps/firefox.args"},{42,"/nexus/apps/writer.args"},{43,"/nexus/apps/calc.args"},
    {44,"/nexus/apps/x11.args"},{45,"/nexus/apps/winecfg.args"},{46,"/nexus/apps/wine-programs.args"},
    {48,"/nexus/apps/software.args"}};
/* Chrome: the GPU launch file when a virgl render node exists, else the software one. */
static const char* chrome_arguments(const char* fallback){
    if(!access("/dev/dri/renderD128",R_OK|W_OK)&&!access("/nexus/chrome-desktop-gpu.args",R_OK))return "/nexus/chrome-desktop-gpu.args";
    return fallback;
}
static bool key_held(unsigned key){return !!(pressed[key/8]&(1U<<(key%8)));}
static void shell_message(const char* message){snprintf(shell_notice,sizeof(shell_notice),"%s",message);notice_until=milliseconds()+6000;shell_dirty|=1U<<SHELL_NOTICE;dirty=true;aurora_reader_notice(message);}
static bool shell_overlay(void){return aurora_mode&&(spotlight_is_open()||launchpad_is_open()||menubar_menu_open()||aurora_power_open()||aurora_wm_overlay());}
/* aurora_power.c services */
void aurora_shell_notice(const char* text){shell_message(text);}
void aurora_shell_redraw(bool everything){if(everything){damage_screen(screen_box());shell_dirty=SHELL_ALL;}else shell_dirty|=(1U<<SHELL_POWER)|(1U<<SHELL_MENUBAR);dirty=true;}
/* The dock magnifies toward the pointer, so its extent depends on the pointer: x,y is tested against the dock the next
 * frame lays out for a pointer there (confine() probes points), then the layout for the real pointer is restored. */
static bool dock_hit_at(int x,int y){   /* primary-display coordinates */
    if(y<dock_top())return false;
    int bx,by,bw,bh,px=SHELL_X(pointer_x),py=SHELL_Y(pointer_y);bool hit=dock_bounds(x,y,&bx,&by,&bw,&bh)&&dock_hit(x,y);
    if(x!=px||y!=py)dock_bounds(px,py,&bx,&by,&bw,&bh);
    return hit;
}
static bool shell_bar_hit(int x,int y){struct surface* root=popup_family(focus);if(!aurora_mode||(root&&root->fullscreen))return false;
    int lx=SHELL_X(x),ly=SHELL_Y(y);struct aurora_output* p=aurora_output_primary();shell_canvas();
    return lx>=0&&ly>=0&&lx<p->w&&ly<p->h&&(menubar_hit(lx,ly)||dock_hit_at(lx,ly));}
static bool shell_hit(int x,int y){
    if(!aurora_mode)return false;
    if(shell_overlay())return true;
    return shell_bar_hit(x,y);
}
static bool app_running(const ui_app_t* app){
    for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)if(app_commands[i].action==app->action){
        if(app_commands[i].pid>0)return true;
        struct surface* s;wl_list_for_each(s,&windows,window_link)if(s->resource&&s->pixels&&!strcmp(s->app_id,app_commands[i].app_id))return true;
    }
    return false;
}
static void shell_reaped(pid_t pid){
    for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)if(app_commands[i].pid==pid){app_commands[i].pid=0;shell_dirty|=1U<<SHELL_DOCK;dirty=true;}
}
/* Shut Down: once no window is left, what still runs (windowless Windows programs, background tools) is ended: after 3 s
 * the Wine session ends (wineboot --end-session: WM_QUERYENDSESSION/WM_ENDSESSION), after 8 s wineserver -k, after 15 s
 * SIGTERM, 90 s later SIGKILL (systemd's stop timeout); signals go to every process when this desktop is init, else to its
 * children. A window, or a package installation (nexus-install must not be cut off), restarts the wait. */
static uint32_t shutdown_quiet;static int shutdown_stage;
/* Forced (power button pressed twice, dialog timeout): windows that are still open 10 s later get SIGTERM, 25 s SIGKILL. */
static int shutdown_forced;static uint32_t shutdown_began;
/* With a name: whether a process whose command starts with it runs. Without: signal what Shut Down ends. */
static bool processes(const char* name,int number){
    if(!name&&getpid()==1){kill(-1,number);return true;}
    DIR* proc=opendir("/proc");struct dirent* e;bool found=false;if(!proc)return false;
    while(!found&&(e=readdir(proc))){char* end;long pid=strtol(e->d_name,&end,10),parent=0;char file[64],text[512];if(*end||pid<=1||pid==(long)getpid())continue;
        snprintf(file,sizeof(file),"/proc/%ld/stat",pid);FILE* f=fopen(file,"re");if(!f)continue;
        size_t n=fread(text,1,sizeof(text)-1,f);fclose(f);text[n]=0;char *open=strchr(text,'('),*close=strrchr(text,')');if(!open||!close)continue;
        if(name)found=!strncmp(open+1,name,strlen(name));
        else if(sscanf(close+1," %*c %ld",&parent)==1&&parent==(long)getpid())kill((pid_t)pid,number);}
    closedir(proc);return found;
}
static void shutdown_run(const char* program,char** argv){
    printf("[64] AURORA SHUTDOWN RUN %s %s\n",program,argv[1]);fflush(stdout);
    pid_t pid=fork();if(!pid){execv(program,argv);_exit(127);}
}
static void shutdown_step(uint32_t now){
    struct surface* s;const char* text=NULL;int stage=shutdown_stage;
    if(shutdown_forced&&now-shutdown_began>=(shutdown_forced==1?10000U:25000U)){printf("[64] AURORA SHUTDOWN FORCED %s\n",shutdown_forced==1?"SIGTERM":"SIGKILL");fflush(stdout);
        processes(NULL,shutdown_forced==1?SIGTERM:SIGKILL);shutdown_forced=shutdown_forced==1?2:0;}
    wl_list_for_each(s,&windows,window_link)if(s->resource&&s->pixels&&s->role){text="Close applications to shut down. Choose Shut Down again to cancel.";break;}
    if(!text&&processes("nexus-install",0))text="Waiting for an application installation to finish. Choose Shut Down again to cancel.";
    if(text){shutdown_quiet=now;shutdown_stage=0;}
    else{uint32_t quiet=now-shutdown_quiet;
        if(!shutdown_stage&&quiet>=3000){shutdown_stage=1;printf("[64] AURORA SHUTDOWN ENDING PROGRAMS\n");fflush(stdout);aurora_services_stop();
            if(!access("/usr/bin/wine",X_OK)&&processes("wineserver",0)){char* end[]={"wine","wineboot","--end-session","--shutdown","--force",NULL};shutdown_run("/usr/bin/wine",end);}}
        else if(shutdown_stage==1&&quiet>=8000){shutdown_stage=2;
            static const char* const servers[]={"/usr/bin/wineserver","/usr/lib/x86_64-linux-gnu/wine/wineserver","/usr/lib/wine/wineserver64","/usr/lib/wine/wineserver","/opt/wine-stable/bin/wineserver"};
            const char* server=getenv("WINESERVER");if(!server||server[0]!='/'||access(server,X_OK))server=NULL;
            for(unsigned i=0;!server&&i<sizeof(servers)/sizeof(*servers);i++)if(!access(servers[i],X_OK))server=servers[i];
            if(server&&processes("wineserver",0)){char* kill_all[]={"wineserver","-k",NULL};shutdown_run(server,kill_all);}}
        else if(shutdown_stage==2&&quiet>=15000&&(!aurora_services_running()||quiet>=60000)){shutdown_stage=3;printf("[64] AURORA SHUTDOWN SIGTERM\n");fflush(stdout);processes(NULL,SIGTERM);}
        else if(shutdown_stage==3&&quiet>=105000){shutdown_stage=4;printf("[64] AURORA SHUTDOWN SIGKILL\n");fflush(stdout);processes(NULL,SIGKILL);}
        if(shutdown_stage)text="Ending programs that are still running. Choose Shut Down again to cancel.";
    }
    /* keep the reason on screen for as long as the wait lasts */
    if(text&&(stage!=shutdown_stage||!shell_notice[0]||(int32_t)(notice_until-now)<1000))shell_message(text);
}
static void shell_action(int action){
    if(!action)return;
    shell_canvas();
    if(action==UI_ACT_SPOTLIGHT||action==UI_ACT_LAUNCHPAD)wineapps_refresh(aurora_apps,sizeof(aurora_apps)/sizeof(*aurora_apps),8); /* newly installed Windows programs */
    if(action==UI_ACT_SPOTLIGHT){spotlight_open();launchpad_close();menubar_close();focus_surface(NULL);shell_dirty=SHELL_ALL;dirty=true;return;}
    if(action==UI_ACT_LAUNCHPAD){launchpad_open();spotlight_close();menubar_close();focus_surface(NULL);shell_dirty=SHELL_ALL;dirty=true;return;}
    menubar_close();spotlight_close();launchpad_close();shell_dirty=SHELL_ALL;dirty=true;
    if(aurora_power_action(action))return;   /* Sleep, Hibernate, power dialog, battery panel */
    if(action==UI_ACT_RESTART||action==UI_ACT_SHUTDOWN_NOW){
        if(!shutting_down){shell_action(UI_ACT_SHUTDOWN);shutdown_forced=action==UI_ACT_SHUTDOWN_NOW;}
        aurora_power_restart(action==UI_ACT_RESTART);return;}
    struct surface* active=popup_family(focus);
    if(!active&&!desktop_focus){struct surface* candidate;wl_list_for_each_reverse(candidate,&windows,window_link)if(candidate->mapped){active=candidate;break;}}
    if(action==UI_ACT_LOCK){aurora_lock_now("menu");return;}
    if(action==UI_ACT_LOGOUT){aurora_session_logout();return;}if(action==UI_ACT_SWITCHUSER){aurora_lock_switch_user();return;}   /* aurora_session.h */
    if(action==UI_ACT_INPUT_NEXT){aurora_keyboard_next();return;}
    if(action==UI_ACT_SHUTDOWN){
        shutting_down=!shutting_down;shutdown_quiet=shutdown_began=milliseconds();shutdown_stage=shutdown_forced=0;aurora_power_restart(false);aurora_ime_restarts(!shutting_down);   /* no input-method restarts while ending programs */
        if(shutting_down){aurora_session_shutdown();struct surface* s;wl_list_for_each(s,&windows,window_link)if(s->role)toplevel_close(s);
            shell_message("Close applications to shut down. Choose Shut Down again to cancel.");}
        else shell_message("Shutdown cancelled.");return;
    }
    if(action==UI_ACT_CLOSEWIN){if(active&&active->role)toplevel_close(active);return;}
    if(action==UI_ACT_MINIMIZE){if(active&&active->role)top_minimize(wl_resource_get_client(active->role),active->role);return;}
    if(action==UI_ACT_ZOOM){if(active&&active->role){if(active->maximized||active->fullscreen)top_restore(wl_resource_get_client(active->role),active->role);else top_maximize(wl_resource_get_client(active->role),active->role);}return;}
    if(action>=DOCK_EXTRA_ACTION){dock_extra_click(action-DOCK_EXTRA_ACTION);return;}   /* a running app or minimized window in the dock */
    if(shutting_down){shell_message("Finish or cancel shutdown before opening another application.");return;}
    if(action>=WINEAPPS_ACTION_BASE&&action<WINEAPPS_ACTION_BASE+WINEAPPS_MAX){char name[64]="";int pid=wineapps_launch(action,name,sizeof(name));
        if(pid>0){desktop_focus=false;printf("[64] AURORA LAUNCH app=wine:%s pid=%d\n",name,pid);fflush(stdout);shell_message("Opening Windows program...");}
        else shell_message("The Windows program could not be started.");return;}
    for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)if(app_commands[i].action==action){
        struct surface* s;wl_list_for_each_reverse(s,&windows,window_link)if(s->resource&&s->pixels&&!strcmp(s->app_id,app_commands[i].app_id)){
            if(s->workspace!=wm_workspace)aurora_host_set_workspace(s->workspace);   /* bring the app's desktop forward */
            s->minimized=s->desktop_hidden=false;desktop_focus=false;refresh_scene();focus_surface(s);pointer_update();dirty=true;
            printf("[64] AURORA RESTORE app=%s\n",s->app_id);fflush(stdout);return;
        }
        if(app_commands[i].pid>0){shell_message("Application is starting...");return;}
        char** loaded=NULL;char* native[]={"/bin/aurora-app",(char*)app_commands[i].name,NULL};char** command=native;
        for(unsigned k=0;k<sizeof(app_configs)/sizeof(*app_configs);k++)if(app_configs[k].action==action){int count=0;loaded=load_arguments(app_configs[k].action==32?chrome_arguments(app_configs[k].path):app_configs[k].path,"/sbin/init",&count);
            if(!loaded){shell_message(action==32?"Chrome launch configuration could not be read.":"This application is not installed.");return;}
            command=loaded+1;if(count>1&&!strcmp(command[0],"--trace"))command++;
            char** program=command;while(program[0]&&program[0][0]!='/'&&strchr(program[0],'='))program++;
            if(!program[0]||program[0][0]!='/'){free_arguments(loaded);shell_message("The launch configuration is invalid.");return;}}
        pid_t child=aurora_session_foreign()?aurora_session_run(command,app_commands[i].name):fork();   /* another user's session: its agent starts it as that user */
        if(!child){while(command[0]&&command[0][0]!='/'&&strchr(command[0],'=')){if(!strcmp(command[0],"X11=rootless"))aurora_xwm_child_env();else putenv(command[0]);command++;}   /* X11=rootless: Aurora's X display */
            execv(command[0],command);perror("Aurora application exec");_exit(127);}
        free_arguments(loaded);
        if(child<0){shell_message("Could not start application: process or memory limit.");return;}
        app_commands[i].pid=child;desktop_focus=false;
        printf("[64] AURORA LAUNCH app=%s pid=%ld\n",app_commands[i].name,(long)child);fflush(stdout);
        shell_message(action==32?"Opening Chrome...":"Opening application...");return;
    }
}
static void shell_click(int x,int y){
    shell_dirty=SHELL_ALL;dirty=true;shell_canvas();
    if(aurora_wm_overlay()){aurora_wm_pointer(x,y,BTN_LEFT,true);refresh_scene();return;}   /* switcher / overview (desktop coordinates) */
    x=SHELL_X(x);y=SHELL_Y(y);
    if(aurora_power_open()){shell_action(aurora_power_click(x,y));refresh_scene();return;}
    if(spotlight_is_open()){int action=spotlight_click(x,y);if(!spotlight_hit(x,y))spotlight_close();shell_action(action);refresh_scene();return;}
    if(launchpad_is_open()){shell_action(launchpad_click(x,y));refresh_scene();return;}
    if(menubar_menu_open()&&!menubar_hit(x,y)){
        if(menubar_menu_hit(x,y))shell_action(menubar_menu_click(x,y));else menubar_close();refresh_scene();return;
    }
    if(menubar_hit(x,y)){shell_action(menubar_click(x,y));if(menubar_menu_open())focus_surface(NULL);else refresh_scene();dirty=true;return;}
    shell_action(dock_click(x,y));
}
static void shell_motion(void){
    shell_canvas();bool dock_changed=dock_hover_changed(SHELL_X(pointer_x),SHELL_Y(pointer_y)),menu_changed=menubar_hover(SHELL_X(pointer_x),SHELL_Y(pointer_y));
    if(aurora_a11y()->magnifier){shell_dirty|=1U<<SHELL_ZOOM;dirty=true;}   /* the lens follows the pointer */
    if(dock_changed){shell_dirty|=1U<<SHELL_DOCK;dirty=true;}
    if(menu_changed){shell_dirty|=(1U<<SHELL_MENUBAR)|(1U<<SHELL_MENU);dirty=true;}
    if(aurora_wm_overlay())aurora_wm_motion(pointer_x,pointer_y);
    if(!moving)return;
    int64_t dx=(int64_t)pointer_x-move_px,dy=(int64_t)pointer_y-move_py;
    struct aurora_output* under=aurora_output_at(pointer_x,pointer_y);struct popup_box a=work_area(under?under:surface_screen(moving),false);   /* the display the pointer drags across */
    if(!resize_edges){
        int64_t x=(int64_t)move_x+dx,y=(int64_t)move_y+dy,min_x=a.x+120-(int64_t)move_w,max_x=a.x+a.w-120,max_y=a.y+a.h-28;
        if(x<min_x)x=min_x;if(x>max_x)x=max_x;if(y<a.y)y=a.y;if(y>max_y)y=max_y;
        moving->x=x;moving->y=y;wm_snap_update();
    }else{
        int64_t w=(int64_t)move_w+(resize_edges&4?-dx:resize_edges&8?dx:0),h=(int64_t)move_h+(resize_edges&1?-dy:resize_edges&2?dy:0);
        int min_w=moving->min_w>120?moving->min_w:120,min_h=moving->min_h>80?moving->min_h:80;
        if(w<min_w)w=min_w;if(h<min_h)h=min_h;
        if(moving->max_w&&w>moving->max_w)w=moving->max_w;if(moving->max_h&&h>moving->max_h)h=moving->max_h;
        if(w>(int)width)w=width;if(h>(int)height)h=height;
        if(resize_edges&4){int64_t x=(int64_t)move_x+move_w-w;if(x<a.x+120-w)x=a.x+120-w;if(x>a.x+a.w-120)x=a.x+a.w-120;moving->x=x;}
        if(resize_edges&1){int64_t y=(int64_t)move_y+move_h-h;if(y<a.y)y=a.y;if(y>a.y+a.h-28)y=a.y+a.h-28;moving->y=y;}
        if(moving->requested_w!=w||moving->requested_h!=h){moving->requested_w=w;moving->requested_h=h;configure(moving);}
    }
    dirty=true;
}
static bool super_alone;   /* Super went down and nothing else since: its release opens Launchpad */
static void shell_super_used(void){super_alone=false;}
static bool shell_key(unsigned key,int value){
    if(aurora_power_key(key,value))return true;   /* power/sleep keys, Ctrl+Alt+Del, the power dialog */
    unsigned mask=1U<<(key%8);shell_canvas();
    bool control=key_held(KEY_LEFTCTRL)||key_held(KEY_RIGHTCTRL),alt=key_held(KEY_LEFTALT)||key_held(KEY_RIGHTALT);
    bool shift=key_held(KEY_LEFTSHIFT)||key_held(KEY_RIGHTSHIFT),super=key_held(KEY_LEFTMETA)||key_held(KEY_RIGHTMETA);
    unsigned mods=(shift?AURORA_MOD_SHIFT:0)|(control?AURORA_MOD_CTRL:0)|(alt?AURORA_MOD_ALT:0)|(super?AURORA_MOD_SUPER:0);
    if(key==KEY_LEFTMETA||key==KEY_RIGHTMETA){   /* Super belongs to the shell: alone it opens Launchpad, with keys it is a shortcut */
        if(value){super_alone=true;shell_keys[key/8]|=mask;return true;}
        shell_keys[key/8]&=~mask;aurora_wm_key(key,0,mods);
        if(super_alone&&!super){super_alone=false;
            if(shell_overlay()&&!aurora_wm_overlay()){menubar_close();spotlight_close();launchpad_close();refresh_scene();}
            else if(!aurora_wm_overlay())shell_action(UI_ACT_LAUNCHPAD);
            shell_dirty=SHELL_ALL;dirty=true;}
        return true;
    }
    if(value)super_alone=false;
    if(value&&super&&key==KEY_SPACE){shell_keys[key/8]|=mask;aurora_keyboard_next();return true;}   /* Super+Space: next input source */
    if(value&&super&&key==KEY_V&&!aurora_lock_active()){shell_keys[key/8]|=mask;shell_action(AURORA_ACTION_CLIPBOARD);return true;}   /* Super+V: clipboard history */
    if(value&&super&&(key==KEY_EQUAL||key==KEY_MINUS)){shell_keys[key/8]|=mask;aurora_a11y_zoom(key==KEY_EQUAL?1:-1);return true;}   /* magnifier */
    if(!value&&(shell_keys[key/8]&mask)){shell_keys[key/8]&=~mask;return true;}
    {int action=0;if(aurora_reader_key(key,value,mods,&action)){if(value)shell_keys[key/8]|=mask;if(action)shell_action(action);refresh_scene();shell_dirty=SHELL_ALL;dirty=true;return true;}}   /* aurora_reader.h */
    if(aurora_wm_key(key,value,mods)){if(value)shell_keys[key/8]|=mask;shell_dirty=SHELL_ALL;dirty=true;return true;}   /* aurora_wm.h shortcuts */
    if(value&&((key==KEY_SPACE&&control)||(key==KEY_F4&&!alt&&!control))){
        shell_keys[key/8]|=mask;
        if(shell_overlay()){menubar_close();spotlight_close();launchpad_close();refresh_scene();}
        else shell_action(key==KEY_SPACE?UI_ACT_SPOTLIGHT:UI_ACT_LAUNCHPAD);
        shell_dirty=SHELL_ALL;dirty=true;return true;
    }
    if(value&&control&&alt&&(key==KEY_T||key==KEY_C)){shell_keys[key/8]|=mask;shell_action(key==KEY_T?1:32);return true;}
    if(shell_overlay()){
        if(value){shell_keys[key/8]|=mask;char text[8]={0};char character=0;
            if(key==KEY_UP)character=(char)0x80;else if(key==KEY_DOWN)character=(char)0x81;else if(key==KEY_LEFT)character=(char)0x82;else if(key==KEY_RIGHT)character=(char)0x83;
            else if(key==KEY_ESC)character=27;else if(key==KEY_ENTER)character='\n';else if(key==KEY_BACKSPACE)character='\b';
            else if(xkb_state_key_get_utf8(key_state,key+8,text,sizeof(text))==1)character=text[0];
            if(character){int action=spotlight_is_open()?spotlight_key(character):launchpad_is_open()?launchpad_key(character):menubar_handle_key(character);shell_action(action);refresh_scene();}
            shell_dirty=SHELL_ALL;dirty=true;
        }
        return true;
    }
    return false;
}
static bool shell_bar(void){struct surface* active=popup_family(focus);return !aurora_lock_active()&&(!active||!active->fullscreen||shell_overlay());}
static const char* shell_label(void){
    struct surface* active=popup_family(focus);const char* label="Finder";
    if(active){label=active->title[0]?active->title:"Application";
        for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)if(!strcmp(active->app_id,app_commands[i].app_id))
            for(unsigned j=0;j<sizeof(aurora_apps)/sizeof(*aurora_apps);j++)if(aurora_apps[j].action==app_commands[i].action)label=aurora_apps[j].name;}
    return label;
}
static bool notice_visible(void){return shell_notice[0]&&(int32_t)(notice_until-milliseconds())>0;}
/* Current screen rectangle of one shell layer (conservative: shadows and bubbles included). */
static bool shell_element(int element,struct popup_box* bounds){
    int x=0,y=0,w=0,h=0;*bounds=(struct popup_box){0};
    if(!aurora_mode)return false;
    struct aurora_output* p=aurora_output_primary();int sw=p->w,sh=p->h;   /* the shell lives on the primary display */
    shell_canvas();   /* layout queries (dock, menus) measure that screen */
    switch(element){
    case SHELL_DOCK:if(!shell_bar()||!dock_bounds(SHELL_X(pointer_x),SHELL_Y(pointer_y),&x,&y,&w,&h))return false;break;
    case SHELL_MENUBAR:if(!shell_bar())return false;w=sw;h=MENUBAR_H;break;
    case SHELL_MENU:if(!shell_bar())return false;menubar_layout(shell_label());if(!menubar_menu_bounds(&x,&y,&w,&h))return false;break;
    case SHELL_LAUNCHPAD:if(!launchpad_is_open())return false;w=sw;h=sh;break;
    case SHELL_SPOTLIGHT:if(!spotlight_bounds(&x,&y,&w,&h))return false;break;
    case SHELL_NOTICE:{
        if(!notice_visible())return false;
        int box=sw-40;if(box>760)box=760;x=sw/2-box/2;y=48;h=48;
        char shown[sizeof(shell_notice)];snprintf(shown,sizeof(shown),"%.*s",(box-32)/8,shell_notice);
        int text=16+ui_text_width(shown,UF_UI)+4;w=text>box?text:box;break;}
    case SHELL_POWER:if(!aurora_power_bounds(&x,&y,&w,&h))return false;break;
    case SHELL_SNAP:case SHELL_WM:case SHELL_LOCK:case SHELL_ZOOM:{   /* desktop layers: desktop rectangle, handed over in primary coordinates */
        struct popup_box r={0};
        if(element==SHELL_LOCK){if(!aurora_lock_active())return false;r=(struct popup_box){0,0,(int32_t)width,(int32_t)height};}
        else if(element==SHELL_ZOOM){if(!aurora_a11y_lens(pointer_x,pointer_y,&r.x,&r.y,&r.w,&r.h))return false;}
        else if(element==SHELL_SNAP){if(!moving||!wm_snap_kind)return false;r=wm_snap_rect();}
        else if(!aurora_wm_overlay_bounds(&r.x,&r.y,&r.w,&r.h))return false;
        if(r.w<=0||r.h<=0)return false;
        *bounds=(struct popup_box){r.x-p->x,r.y-p->y,r.w,r.h};return true;}
    default:return false;
    }
    return box_intersect((struct popup_box){x,y,w,h},(struct popup_box){0,0,sw,sh},bounds);
}
/* Draw the selected shell layers whole, in the original stacking order (primary-display coordinates). */
static void shell_draw(unsigned mask){
    int sw=aurora_output_primary()->w;
    if(mask&(1U<<SHELL_DOCK))dock_draw(SHELL_X(pointer_x),SHELL_Y(pointer_y));
    if(mask&(1U<<SHELL_MENUBAR))menubar_draw(shell_label());
    if(mask&(1U<<SHELL_MENU))menubar_draw_menu();
    if(mask&(1U<<SHELL_LAUNCHPAD))launchpad_draw();
    if(mask&(1U<<SHELL_SPOTLIGHT))spotlight_draw();
    if((mask&(1U<<SHELL_NOTICE))&&notice_visible()){
        int box=sw-40;if(box>760)box=760;int x=sw/2-box/2;
        ui_rrect(x,48,box,48,12,0x1A2234,245);ui_text_n(x+16,63,shell_notice,(box-32)/8,UF_UI,0xFFFFFF);
    }
    if(mask&(1U<<SHELL_POWER))aurora_power_draw();
}

/* ---- window management (aurora_wm.h): tiles, snapping, workspaces, services for aurora_wm.c ---- */
static struct popup_box wm_tile_area(struct surface* s){   /* the half of the display a tiled window takes */
    struct popup_box a=work_area(surface_screen(s),false);
    if(s->tile==AURORA_TILE_LEFT)a.w/=2;else if(s->tile==AURORA_TILE_RIGHT){a.x+=a.w/2;a.w-=a.w/2;}
    return a;
}
static struct popup_box wm_snap_rect(void){   /* where a window dropped now would go */
    struct aurora_output* d=aurora_output_slot(wm_snap_screen);if(!d||!d->enabled||!wm_snap_kind)return (struct popup_box){0,0,0,0};
    struct popup_box a=work_area(d,false);
    if(wm_snap_kind==1)a.w/=2;else if(wm_snap_kind==2){a.x+=a.w/2;a.w-=a.w/2;}
    return a;
}
/* While a window is dragged: the pointer at a free outer edge of a display (no display continues there) previews a
 * half-screen tile (left/right) or maximizing (top). */
static void wm_snap_update(void){
    int kind=0,screen=wm_snap_screen;struct aurora_output* d=aurora_output_at(pointer_x,pointer_y);
    if(moving&&!resize_edges&&d){
        int top=d->y+(d->primary&&aurora_mode?MENUBAR_H:0);
        if(pointer_x<=d->x+2&&!aurora_output_at(d->x-1,pointer_y))kind=1;
        else if(pointer_x>=d->x+d->w-3&&!aurora_output_at(d->x+d->w,pointer_y))kind=2;
        else if(pointer_y<=top+2&&!aurora_output_at(pointer_x,d->y-1))kind=3;
        screen=d->index;
    }
    if(kind!=wm_snap_kind||(kind&&screen!=wm_snap_screen)){
        if(wm_snap_kind)damage_screen(wm_snap_rect());
        wm_snap_kind=kind;wm_snap_screen=screen;
        if(kind)damage_screen(wm_snap_rect());
        shell_dirty|=1U<<SHELL_SNAP;dirty=true;
    }
}
static void wm_drop(struct surface* s){
    int kind=wm_snap_kind;wm_snap_update();   /* moving is already NULL: this clears the preview */
    if(!kind)return;
    s->screen=wm_snap_screen;s->screen_set=true;save_window(s);
    if(kind==3){s->maximized=true;s->tile=0;}else{s->tile=kind==1?AURORA_TILE_LEFT:AURORA_TILE_RIGHT;s->maximized=false;}
    printf("[64] AURORA SNAP app=%s %s screen=%s\n",s->app_id,kind==3?"maximized":kind==1?"left":"right",surface_screen(s)->name);fflush(stdout);
}
int aurora_host_windows(struct aurora_window** list,int capacity){
    int n=0;struct surface* s;
    wl_list_for_each_reverse(s,&windows,window_link)if(n<capacity&&s->role&&s->resource&&s->pixels&&s->kind==ROLE_TOPLEVEL)list[n++]=(struct aurora_window*)s;
    return n;
}
void aurora_host_window_info(struct aurora_window* w,struct aurora_window_info* info){
    struct surface* s=(struct surface*)w;struct popup_box b;if(window_bounds(s,&b)<=0)b=(struct popup_box){0,0,s->view.w,s->view.h};
    *info=(struct aurora_window_info){s->title[0]?s->title:s->app_id,s->app_id,s->x+b.x,s->y+b.y,b.w,b.h,s->minimized,s->maximized,s->fullscreen,
        popup_family(focus)==s,s->tile,s->workspace,surface_screen(s)->index,ICON_GENERIC};
    for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)if(!strcmp(s->app_id,app_commands[i].app_id))
        for(unsigned j=0;j<sizeof(aurora_apps)/sizeof(*aurora_apps);j++)if(aurora_apps[j].action==app_commands[i].action)info->icon=aurora_apps[j].icon;
}
struct aurora_window* aurora_host_focus(void){struct surface* s=popup_family(focus);return s&&s->kind==ROLE_TOPLEVEL?(struct aurora_window*)s:NULL;}
static void wm_follow(struct surface* s,int workspace){   /* a window and its dialogs live on one workspace */
    s->workspace=workspace;struct surface* o;wl_list_for_each(o,&windows,window_link)if(o->transient_for==s&&o!=s)o->workspace=workspace;
}
void aurora_host_set_workspace(int workspace){
    if(workspace<0||workspace>=AURORA_WORKSPACES||workspace==wm_workspace)return;
    wm_workspace=workspace;focus_surface(NULL);desktop_focus=false;refresh_scene();damage_screen(screen_box());shell_dirty=SHELL_ALL;
    printf("[64] AURORA WORKSPACE %d\n",workspace+1);fflush(stdout);
}
int aurora_host_workspace(void){return wm_workspace;}
void aurora_host_activate(struct aurora_window* w){
    struct surface* s=(struct surface*)w;if(!s||!s->role||s->parked)return;
    if(s->workspace!=wm_workspace)aurora_host_set_workspace(s->workspace);
    s->minimized=s->desktop_hidden=false;desktop_focus=false;refresh_scene();focus_surface(s);pointer_update();dirty=true;
    printf("[64] AURORA ACTIVATE app=%s\n",s->app_id);fflush(stdout);
}
/* xdg_activation_v1: granted to the client that has (or had, when it got the token) the keyboard, and to a client
 * bringing back its own minimized window (Wine's restore); anything else would steal focus and is only logged. */
bool aurora_host_client_focused(struct wl_client* client){return focus&&focus->resource&&wl_resource_get_client(focus->resource)==client;}
void aurora_host_activation(struct wl_resource* resource,struct wl_client* requester,bool focused){
    if(!resource||!wl_resource_instance_of(resource,&wl_surface_interface,&surface_interface))return;
    struct surface* s=popup_family(wl_resource_get_user_data(resource));if(!s||!s->role||s->kind!=ROLE_TOPLEVEL||!s->resource)return;
    bool own=wl_resource_get_client(s->resource)==requester;
    if(focused||aurora_host_client_focused(requester)||(own&&s->minimized)){printf("[64] AURORA ACTIVATION app=%s granted\n",s->app_id);fflush(stdout);aurora_host_activate((struct aurora_window*)s);}
    else{printf("[64] AURORA ACTIVATION app=%s refused (focus stealing prevention)\n",s->app_id);fflush(stdout);}
}
void aurora_host_minimize(struct aurora_window* w,bool minimized){
    struct surface* s=(struct surface*)w;if(!s||!s->role)return;
    cancel_move(s);s->minimized=minimized;if(!minimized)s->desktop_hidden=false;refresh_scene();
    printf("[64] AURORA %s app=%s\n",minimized?"MINIMIZE":"RESTORE",s->app_id);fflush(stdout);
}
void aurora_host_tile(struct aurora_window* w,int tile,bool maximize){
    struct surface* s=(struct surface*)w;if(!s||!s->role||s->fullscreen)return;cancel_move(s);
    if(maximize){if(!s->tile)save_window(s);s->maximized=true;s->tile=0;}
    else if(tile){if(!s->maximized&&!s->tile)save_window(s);s->tile=tile;s->maximized=false;}
    else{if(!s->maximized&&!s->tile)return;s->maximized=false;s->tile=0;restore_geometry(s);}
    if(s->configured)configure(s);
    printf("[64] AURORA TILE app=%s %s\n",s->app_id,maximize?"maximized":tile==AURORA_TILE_LEFT?"left":tile==AURORA_TILE_RIGHT?"right":"restored");fflush(stdout);
}
void aurora_host_move_screen(struct aurora_window* w,int direction){
    struct surface* s=(struct surface*)w;if(!s||!s->role)return;
    struct aurora_output* from=surface_screen(s),*to=NULL;int best=0;
    for(int i=0;i<AURORA_OUTPUTS;i++){struct aurora_output* d=aurora_output_slot(i);if(!d||!d->enabled||d->mirror>=0||d==from)continue;
        int distance=(d->x+d->w/2)-(from->x+from->w/2);if((direction<0)!=(distance<0)||!distance)continue;
        if(!to||abs(distance)<best){to=d;best=abs(distance);}}
    if(!to)return;
    struct popup_box b;int bw=window_bounds(s,&b)>0?b.w:s->requested_w;
    int64_t nx=to->x+(int64_t)(s->x-from->x)*to->w/(from->w?from->w:1),ny=to->y+(int64_t)(s->y-from->y)*to->h/(from->h?from->h:1);
    struct popup_box a=work_area(to,false);
    if(nx+bw>a.x+a.w)nx=a.x+a.w-bw;if(nx<a.x)nx=a.x;if(ny<a.y)ny=a.y;if(ny>a.y+a.h-28)ny=a.y+a.h-28;
    s->x=(int)nx;s->y=(int)ny;s->screen=to->index;s->screen_set=true;
    if(s->configured&&(s->maximized||s->fullscreen||s->tile))configure(s);
    refresh_scene();dirty=true;
    printf("[64] AURORA MOVED app=%s x=%d y=%d screen=%s\n",s->app_id,s->x,s->y,to->name);fflush(stdout);
}
void aurora_host_window_workspace(struct aurora_window* w,int workspace){
    struct surface* s=(struct surface*)w;if(!s||!s->role||workspace<0||workspace>=AURORA_WORKSPACES)return;
    wm_follow(s,workspace);refresh_scene();damage_screen(screen_box());
    printf("[64] AURORA WINDOW WORKSPACE app=%s workspace=%d\n",s->app_id,workspace+1);fflush(stdout);
}
void aurora_host_close(struct aurora_window* w){struct surface* s=(struct surface*)w;if(s&&s->role)toplevel_close(s);}
void aurora_host_focus_none(void){focus_surface(NULL);}
void aurora_host_lock(void){aurora_lock_now("shortcut");}
/* Accessibility (aurora_a11y.h): text size, dock motion, the colour filter of every output, a full repaint. */
void aurora_host_a11y_changed(void){
    const struct aurora_a11y* a=aurora_a11y();ui_set_text_scale(a->larger_text?125:100);dock_set_magnify(a->reduce_motion?0:44);
    aurora_outputs_filter(aurora_a11y_filtering()?aurora_a11y_filter:NULL);
    memset(output_cursor,0,sizeof(output_cursor));struct surface* s;wl_list_for_each(s,&surfaces,link)s->was_shown=false;
    refresh_scene();damage_screen(screen_box());shell_dirty=SHELL_ALL;cursor_dirty=true;dirty=true;
}
/* Sticky keys: Shift, Ctrl, Alt or Super pressed and released alone stays active for the next key. */
static xkb_mod_mask_t sticky_latched;static unsigned sticky_candidate;
static bool sticky_key(unsigned key,int value){
    const char* name=key==KEY_LEFTSHIFT||key==KEY_RIGHTSHIFT?XKB_MOD_NAME_SHIFT:key==KEY_LEFTCTRL||key==KEY_RIGHTCTRL?XKB_MOD_NAME_CTRL:
                     key==KEY_LEFTALT||key==KEY_RIGHTALT?XKB_MOD_NAME_ALT:key==KEY_LEFTMETA||key==KEY_RIGHTMETA?XKB_MOD_NAME_LOGO:NULL;
    struct endpoint* e;uint32_t serial;
    if(name){
        if(value){sticky_candidate=key;return false;}
        if(sticky_candidate!=key)return false;
        sticky_candidate=0;xkb_mod_index_t index=xkb_keymap_mod_get_index(keymap,name);if(index==XKB_MOD_INVALID)return false;
        sticky_latched^=1U<<index;   /* tapped twice: released again */
        xkb_state_update_mask(key_state,xkb_state_serialize_mods(key_state,XKB_STATE_MODS_DEPRESSED),sticky_latched,xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LOCKED),0,0,
                              xkb_state_serialize_layout(key_state,XKB_STATE_LAYOUT_EFFECTIVE));
        serial=wl_display_next_serial(display);wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus))modifiers(e,serial);
        printf("[64] AURORA STICKY %s latched=0x%x\n",name,sticky_latched);fflush(stdout);return false;
    }
    sticky_candidate=0;
    if(!value&&sticky_latched){   /* the key that used the latch went up: clear it after delivering the release */
        sticky_latched=0;xkb_state_update_mask(key_state,xkb_state_serialize_mods(key_state,XKB_STATE_MODS_DEPRESSED),0,xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LOCKED),0,0,
                                               xkb_state_serialize_layout(key_state,XKB_STATE_LAYOUT_EFFECTIVE));
        serial=wl_display_next_serial(display);
        wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus)){wl_keyboard_send_key(e->resource,serial,milliseconds(),key,WL_KEYBOARD_KEY_STATE_RELEASED);modifiers(e,serial);}
        return true;
    }
    return false;
}
/* Keyboard layouts (aurora_keyboard.h): one keymap with every input source; the xkb group is the active one. */
static bool keymap_build(const char* layouts,const char* variants){
    struct xkb_rule_names rules={.rules="evdev",.model="pc105",.layout=layouts,.variant=variants&&variants[0]?variants:NULL};
    struct xkb_keymap* map=xkb_keymap_new_from_names(xkb_context,&rules,XKB_KEYMAP_COMPILE_NO_FLAGS);if(!map)return false;
    struct xkb_state* state=xkb_state_new(map);char* text=state?xkb_keymap_get_as_string(map,XKB_KEYMAP_FORMAT_TEXT_V1):NULL;
    size_t size=text?strlen(text)+1:0;int fd=text?memfd_create("nexus-keymap",MFD_CLOEXEC|MFD_ALLOW_SEALING):-1;
    bool ok=fd>=0&&write(fd,text,size)==(ssize_t)size&&!fcntl(fd,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE|F_SEAL_SEAL);free(text);
    if(!ok){if(fd>=0)close(fd);if(state)xkb_state_unref(state);xkb_keymap_unref(map);return false;}
    if(key_state)xkb_state_unref(key_state);if(keymap)xkb_keymap_unref(keymap);if(keymap_fd>=0)close(keymap_fd);
    keymap=map;key_state=state;keymap_fd=fd;keymap_size=size;return true;
}
bool aurora_host_keymap(const char* layouts,const char* variants){
    if(!keymap_build(layouts,variants))return false;
    struct endpoint* e;uint32_t serial=wl_display_next_serial(display);
    wl_list_for_each(e,&endpoints,link)if(e->keyboard){wl_keyboard_send_keymap(e->resource,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,keymap_fd,keymap_size);if(endpoint_for(e,focus))modifiers(e,serial);}
    aurora_ime_keymap_changed();shell_dirty|=1U<<SHELL_MENUBAR;dirty=true;return true;
}
void aurora_host_keyboard_group(unsigned group){
    xkb_state_update_mask(key_state,xkb_state_serialize_mods(key_state,XKB_STATE_MODS_DEPRESSED),xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LATCHED),
                          xkb_state_serialize_mods(key_state,XKB_STATE_MODS_LOCKED),0,0,group);
    struct endpoint* e;uint32_t serial=wl_display_next_serial(display);wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus))modifiers(e,serial);
    shell_dirty|=1U<<SHELL_MENUBAR;dirty=true;
}
/* aurora_ime.h services: keys the input method gives back reach the focused client without the shell or the grab. */
struct wl_resource* aurora_host_focus_resource(void){return focus&&focus->resource?focus->resource:NULL;}
bool aurora_host_surface_origin(struct wl_resource* r,int* x,int* y){struct surface* s=r?wl_resource_get_user_data(r):NULL;int64_t sx,sy;
    if(!s||popup_family(s)->kind==ROLE_IME_POPUP||!surface_origin(s,&sx,&sy)||sx<-(1<<20)||sy<-(1<<20)||sx>(1<<20)||sy>(1<<20))return false;*x=(int)sx;*y=(int)sy;return true;}
pid_t aurora_host_focus_pid(char* app_id,unsigned size,struct wl_client** client){struct surface* s=popup_family(focus);pid_t pid=0;if(!s||!s->resource)return 0;if(client)*client=wl_resource_get_client(s->resource);wl_client_get_credentials(wl_resource_get_client(s->resource),&pid,NULL,NULL);if(app_id&&size)snprintf(app_id,size,"%s",s->app_id);return pid;}   /* aurora_sak.h */
void aurora_host_inject_key(uint32_t time,uint32_t key,uint32_t state){
    if(aurora_lock_active()||!focus)return;struct endpoint* e;uint32_t serial=wl_display_next_serial(display);if(state)key_serial=serial,key_origin=focus;
    wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus))wl_keyboard_send_key(e->resource,serial,time,key,state);}
void aurora_host_inject_modifiers(uint32_t depressed,uint32_t latched,uint32_t locked,uint32_t group){
    if(aurora_lock_active()||!focus)return;struct endpoint* e;uint32_t serial=wl_display_next_serial(display);
    wl_list_for_each(e,&endpoints,link)if(e->keyboard&&endpoint_for(e,focus))wl_keyboard_send_modifiers(e->resource,serial,depressed,latched,locked,group);}
bool aurora_host_keymap_fd(int* fd,uint32_t* size){if(keymap_fd<0)return false;*fd=keymap_fd;*size=(uint32_t)keymap_size;return true;}
bool aurora_host_claim_ime_popup(struct wl_resource* r){struct surface* s=wl_resource_get_user_data(r);
    if(!s||!s->resource||s->xdg||s->subsurface||(s->kind!=ROLE_NONE&&s->kind!=ROLE_IME_POPUP))return false;s->kind=ROLE_IME_POPUP;return true;}
void aurora_host_ime_redraw(void){ime_scene=true;dirty=true;}
static int input_menu_width(void){return aurora_keyboard_count()>1?ui_text_width(aurora_keyboard_label(),UF_UIB)+14:0;}
static void input_menu_draw(int x,int cy){int w=input_menu_width();ui_rrect(x,cy-9,w,18,5,0xFFFFFF,70);ui_text_center(x+w/2,cy-ui_font_height(UF_UIB)/2,aurora_keyboard_label(),UF_UIB,0xFFFFFF);}
/* aurora_lock.h services: while locked no window is shown or focused; afterwards the last focus comes back. */
static struct surface* focus_before_lock;
void aurora_host_lock_changed(bool locked){
    if(locked){focus_before_lock=popup_family(focus);menubar_close();spotlight_close();launchpad_close();moving=NULL;focus_surface(NULL);}
    else{struct surface* s;bool alive=false;wl_list_for_each(s,&windows,window_link)if(s==focus_before_lock&&s->mapped)alive=true;
        if(alive)focus_surface(focus_before_lock);focus_before_lock=NULL;}
    refresh_scene();damage_screen(screen_box());shell_dirty=SHELL_ALL;cursor_dirty=true;dirty=true;
}
void aurora_host_lock_redraw(void){shell_dirty|=1U<<SHELL_LOCK;dirty=true;}
/* aurora_session.h services: the active session's windows only (the others are parked); no clipboard across users. */
static struct wl_list* parked_windows(void){static struct wl_list list;static bool ready;if(!ready){wl_list_init(&list);ready=true;}return &list;}
void aurora_host_session_switched(void){
    struct surface *s,*t;struct wl_list* parked=parked_windows();
    wl_list_for_each_safe(s,t,&windows,window_link)if(!s->resource||!aurora_session_client_visible(wl_resource_get_client(s->resource))){wl_list_remove(&s->window_link);wl_list_insert(parked->prev,&s->window_link);s->parked=true;}
    wl_list_for_each_safe(s,t,parked,window_link)if(s->resource&&aurora_session_client_visible(wl_resource_get_client(s->resource))){wl_list_remove(&s->window_link);wl_list_insert(windows.prev,&s->window_link);s->parked=false;}
    wl_list_for_each(s,&x11_overrides,window_link)s->parked=!s->resource||!aurora_session_client_visible(wl_resource_get_client(s->resource));   /* X menus */
    if(selection){if(selection->resource)wl_data_source_send_cancelled(selection->resource);source_release(selection);selection=NULL;}
    for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)app_commands[i].pid=0;   /* "starting" belongs to the session left */
    moving=NULL;focus_before_lock=NULL;focus_surface(NULL);refresh_scene();pointer_update();damage_screen(screen_box());shell_dirty=SHELL_ALL;cursor_dirty=true;dirty=true;
}
bool aurora_host_surface_shown(struct wl_resource* r){struct surface* s=r?wl_resource_get_user_data(r):NULL;return s&&s->resource&&s->mapped&&!aurora_lock_active();}
void aurora_host_power_request(bool reboot){shell_action(reboot?UI_ACT_RESTART:UI_ACT_SHUTDOWN_NOW);if(shutting_down&&!shutdown_forced){shutdown_forced=1;shutdown_began=milliseconds();}}
void aurora_host_reaped(pid_t pid,int status){shell_reaped(pid);printf("[64] AURORA CHILD EXIT pid=%ld status=%d session=%u\n",(long)pid,status,aurora_session_active_uid());fflush(stdout);}
void aurora_host_session_end_clients(unsigned uid,int number){   /* the applications of the desktop user's own session */
    for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)if(app_commands[i].pid>0)kill(app_commands[i].pid,number);
    struct wl_list* lists[2]={&windows,parked_windows()};struct surface* s;   /* shown and parked windows */
    for(int l=0;l<2;l++)wl_list_for_each(s,lists[l],window_link)if(s->resource&&s->role){pid_t pid;uid_t cuid;wl_client_get_credentials(wl_resource_get_client(s->resource),&pid,&cuid,NULL);
        if(cuid==uid&&pid>1&&pid!=getpid())kill(pid,number);}
    printf("[64] AURORA SESSION CLIENTS uid=%u signal=%d\n",uid,number);fflush(stdout);
}
void aurora_host_present_now(void){refresh_scene();damage_screen(screen_box());shell_dirty=SHELL_ALL;dirty=true;paint();}
void aurora_host_desktop_toggle(bool hide){   /* Super+D */
    struct surface* s;int count=0;
    wl_list_for_each(s,&windows,window_link)if(s->role&&s->pixels&&s->workspace==wm_workspace){
        if(hide&&!s->minimized){s->minimized=s->desktop_hidden=true;count++;}
        else if(!hide&&s->desktop_hidden){s->minimized=s->desktop_hidden=false;count++;}
    }
    desktop_focus=hide;if(hide)focus_surface(NULL);refresh_scene();
    printf("[64] AURORA SHOW DESKTOP %s windows=%d\n",hide?"on":"off",count);fflush(stdout);
}
void aurora_host_launchpad(void){shell_action(UI_ACT_LAUNCHPAD);}
void aurora_host_redraw(void){shell_dirty|=1U<<SHELL_WM;dirty=true;}
void aurora_host_screen_rect(int screen,int* x,int* y,int* w,int* h){
    struct aurora_output* d=screen>=0?aurora_output_slot(screen):NULL;if(!d||!d->enabled)d=aurora_output_primary();
    *x=d->x;*y=d->y;*w=d->w;*h=d->h;
}
int aurora_host_screen_count(void){return aurora_outputs_count();}
/* Thumbnail: the window's surface tree drawn with a scaled view into a private canvas (outside paint()). */
static bool thumbnail_sampled(struct surface* root,uint32_t* pixels,int tw,int th);
/* Smaller than the window: drawn whole at 1:1 into a scratch canvas, then box-filtered (legible text); otherwise sampled. */
bool aurora_host_thumbnail(struct aurora_window* w,uint32_t* pixels,int tw,int th){
    struct surface* root=(struct surface*)w;struct popup_box b;
    for(int i=0;i<tw*th;i++)pixels[i]=0x202024;
    if(!root||!root->pixels||tw<=0||th<=0||window_bounds(root,&b)<=0)return false;
    if(b.w<=tw||b.h<=th||b.w>4096||b.h>4096)return thumbnail_sampled(root,pixels,tw,th);
    uint32_t* full=malloc((size_t)b.w*b.h*4);if(!full)return thumbnail_sampled(root,pixels,tw,th);
    if(!thumbnail_sampled(root,full,b.w,b.h)){free(full);return false;}
    int64_t f=(int64_t)tw*65536/b.w;if((int64_t)th*65536/b.h<f)f=(int64_t)th*65536/b.h;
    int ow=(int)(b.w*f>>16),oh=(int)(b.h*f>>16),ox=(tw-ow)/2,oy=(th-oh)/2;
    for(int y=0;y<oh;y++){int y0=(int)((int64_t)y*b.h/oh),y1=(int)((int64_t)(y+1)*b.h/oh);if(y1<=y0)y1=y0+1;
        for(int x=0;x<ow;x++){int x0=(int)((int64_t)x*b.w/ow),x1=(int)((int64_t)(x+1)*b.w/ow);if(x1<=x0)x1=x0+1;
            uint32_t r=0,g=0,bl=0,n=0;
            for(int yy=y0;yy<y1;yy++)for(int xx=x0;xx<x1;xx++){uint32_t p=full[(size_t)yy*b.w+xx];r+=p>>16&255;g+=p>>8&255;bl+=p&255;n++;}
            pixels[(size_t)(y+oy)*tw+x+ox]=(r/n)<<16|(g/n)<<8|bl/n;}}
    free(full);return true;
}
static bool thumbnail_sampled(struct surface* root,uint32_t* pixels,int tw,int th){
    struct popup_box b;if(window_bounds(root,&b)<=0)return false;
    for(int i=0;i<tw*th;i++)pixels[i]=0x202024;
    int64_t f=(int64_t)tw*65536/b.w;if((int64_t)th*65536/b.h<f)f=(int64_t)th*65536/b.h;if(f<=0)return false;   /* 16.16 scale */
    int64_t ox=((int64_t)tw-(b.w*f>>16))/2,oy=((int64_t)th-(b.h*f>>16))/2;
    struct surface* family[64];bool mapped[64];int members=0;struct surface* s0;   /* minimized/other-workspace windows render too */
    wl_list_for_each(s0,&surfaces,link)if(members<64&&s0->pixels&&root_surface(s0)==root){family[members]=s0;mapped[members++]=s0->mapped;s0->mapped=true;}
    size_t saved=item_count;items_family(root);
    uint32_t* saved_canvas=canvas;unsigned saved_stride=stride;canvas=pixels;stride=(unsigned)tw;
    for(size_t j=saved;j<item_count;j++){
        struct item it=items[j];struct surface* s=it.s;int64_t rx,ry;if(!surface_origin(root,&rx,&ry))continue;
        it.x=(it.x-rx-b.x)*f/65536+ox;it.y=(it.y-ry-b.y)*f/65536+oy;it.w=(int32_t)(s->view.w*f/65536);it.h=(int32_t)(s->view.h*f/65536);
        if(it.w<=0||it.h<=0)continue;
        struct view v=s->view;int64_t x0=v.on?v.x0-v.dx/2:0,y0=v.on?v.y0-v.dy/2:0;
        v.w=it.w;v.h=it.h;v.dx=s->view.dx*65536/f;v.dy=s->view.dy*65536/f;v.x0=x0+v.dx/2;v.y0=y0+v.dy/2;v.fast=false;
        it.v=v;it.k=1;
        struct popup_box c;if(!box_intersect((struct popup_box){(int32_t)it.x,(int32_t)it.y,it.w,it.h},(struct popup_box){0,0,tw,th},&c))continue;
        draw_scaled(&it,c);
    }
    canvas=saved_canvas;stride=saved_stride;item_count=saved;for(int i=0;i<members;i++)family[i]->mapped=mapped[i];
    return true;
}

/* Dock extras (dock_set_extra): running apps without a dock icon, then minimized windows. */
enum { EXTRA_APP, EXTRA_UNKNOWN, EXTRA_WINDOW };
struct dock_extra { int kind, app; struct surface* s; };
static struct dock_extra dock_extras[24]; static int dock_extra_total;
static int app_index_of(const struct surface* s){
    for(unsigned i=0;i<sizeof(app_commands)/sizeof(*app_commands);i++)if(s->app_id[0]&&!strcmp(s->app_id,app_commands[i].app_id))
        for(unsigned j=0;j<sizeof(aurora_apps)/sizeof(*aurora_apps);j++)if(aurora_apps[j].action==app_commands[i].action)return (int)j;
    return -1;
}
static int dock_extra_count(void){
    int n=0;struct surface* s;
    wl_list_for_each_reverse(s,&windows,window_link){
        if(!s->role||!s->resource||!s->pixels||s->transient_for||n>=24)continue;
        int app=app_index_of(s);if(app>=0&&app<8)continue;   /* the fixed dock already shows it */
        bool listed=false;for(int k=0;k<n;k++)if(dock_extras[k].kind!=EXTRA_WINDOW&&(app>=0?dock_extras[k].app==app:dock_extras[k].kind==EXTRA_UNKNOWN&&!strcmp(dock_extras[k].s->app_id,s->app_id)))listed=true;
        if(!listed)dock_extras[n++]=(struct dock_extra){app>=0?EXTRA_APP:EXTRA_UNKNOWN,app,s};
    }
    wl_list_for_each_reverse(s,&windows,window_link)if(n<24&&s->role&&s->resource&&s->minimized&&s->pixels)dock_extras[n++]=(struct dock_extra){EXTRA_WINDOW,app_index_of(s),s};
    dock_extra_total=n;return n;
}
/* An X program's _NET_WM_ICON: the window's own, else one of another window of the app (second windows and dialogs
 * often set none; the dock entry stands for whichever was on top when the dock was drawn). */
static const uint32_t* dock_x11_icon(struct surface* s){
    if(s->x11_icon||!s->x11||!s->app_id[0])return s->x11_icon;
    struct surface* o;wl_list_for_each_reverse(o,&windows,window_link)if(o->x11&&o->x11_icon&&o->role&&!strcmp(o->app_id,s->app_id))return o->x11_icon;
    return NULL;
}
static void dock_extra_draw(int k,int x,int y,int size){
    if(k<0||k>=dock_extra_total)return;struct dock_extra* e=&dock_extras[k];
    int icon=e->app>=0?aurora_apps[e->app].icon:ICON_GENERIC;const uint32_t* own=e->app<0?dock_x11_icon(e->s):NULL;   /* an X program's _NET_WM_ICON */
    if(e->kind!=EXTRA_WINDOW||!e->s->dock_thumb){if(own)ui_image(x,y,size,size,own,AURORA_XICON,AURORA_XICON,AURORA_XICON,true);else ui_icon(icon,x,y,size);return;}
    ui_rrect(x,y,size,size,6,0x1A1A1E,230);
    ui_image(x+2,y+2,size-4,size-4,e->s->dock_thumb,DOCK_THUMB,DOCK_THUMB,DOCK_THUMB,false);
    if(own)ui_image(x+size-size/3,y+size-size/3,size/3,size/3,own,AURORA_XICON,AURORA_XICON,AURORA_XICON,true);
    else ui_icon(icon,x+size-size/3,y+size-size/3,size/3);   /* whose window it is */
}
static const char* dock_extra_name(int k){
    if(k<0||k>=dock_extra_total)return "";struct dock_extra* e=&dock_extras[k];
    if(e->kind==EXTRA_APP)return aurora_apps[e->app].name;
    return e->s->title[0]?e->s->title:e->s->app_id;
}
static bool dock_extra_running(int k){return k>=0&&k<dock_extra_total&&dock_extras[k].kind!=EXTRA_WINDOW;}
static void dock_extra_click(int k){
    if(k<0||k>=dock_extra_total)return;struct dock_extra e=dock_extras[k];struct surface* s;bool alive=false;
    wl_list_for_each(s,&windows,window_link)if(s==e.s&&s->role)alive=true;
    if(!alive)return;
    if(e.kind==EXTRA_WINDOW){aurora_host_activate((struct aurora_window*)e.s);return;}
    struct surface* best=NULL;   /* the app's topmost window */
    wl_list_for_each_reverse(s,&windows,window_link)if(s->role&&s->pixels&&!s->transient_for&&(e.kind==EXTRA_APP?app_index_of(s)==e.app:!strcmp(s->app_id,e.s->app_id))){best=s;break;}
    if(best)aurora_host_activate((struct aurora_window*)best);
}

/* ---- services for aurora_xwm.c (X11 windows) ----
 * The X11 role: a server-side placeholder resource that is never announced to Xwayland stands in for the xdg_toplevel,
 * so every "is a window" test (s->role, ROLE_TOPLEVEL) holds; configure and close go to aurora_xwm.c through s->x11.
 * Managed windows live in `windows` (dock, switcher, focus); override-redirect ones in x11_overrides, drawn on top. */
static const struct wl_interface x11_role_interface = {"aurora_x11_window", 1, 0, NULL, 0, NULL};
static void x11_role_destroy(struct wl_resource* r){struct surface* s=wl_resource_get_user_data(r);struct aurora_xwin* x=s->x11;s->x11=NULL;s->x11_modal=false;role_destroy(r);if(x)aurora_xwm_detached(x);}
struct aurora_window* aurora_host_x11_attach(struct wl_resource* r,struct aurora_xwin* x,bool unmanaged,int x0,int y0,int w,int h,const char* title,const char* app_id){
    if(!r||!x||!wl_resource_instance_of(r,&wl_surface_interface,&surface_interface))return NULL;
    struct surface* s=wl_resource_get_user_data(r);struct wl_client* client=wl_resource_get_client(r);
    if(!s->resource||s->role||s->xdg||s->subsurface||s->kind!=ROLE_NONE)return NULL;   /* a surface with another role */
    if(!(s->role=wl_resource_create(client,&x11_role_interface,1,0)))return NULL;
    wl_resource_set_implementation(s->role,NULL,s,x11_role_destroy);
    s->kind=ROLE_TOPLEVEL;s->x11=x;s->references++;s->configured=s->acked=s->placed=true;
    s->x=x0;s->y=y0;s->requested_w=w;s->requested_h=h;s->workspace=wm_workspace;
    snprintf(s->title,sizeof(s->title),"%s",title?title:"");snprintf(s->app_id,sizeof(s->app_id),"%s",app_id?app_id:"");
    struct aurora_output* o=aurora_output_at(x0+w/2,y0+h/2);if(!o)o=aurora_output_nearest(x0+w/2,y0+h/2);s->screen=o->index;s->screen_set=true;
    wl_list_insert((unmanaged?&x11_overrides:&windows)->prev,&s->window_link);
    if(!aurora_session_client_visible(client)){s->parked=true;if(!unmanaged){wl_list_remove(&s->window_link);wl_list_insert(parked_windows()->prev,&s->window_link);}}
    if(!unmanaged){printf("[64] AURORA WINDOW app=%s title=%s x=%d y=%d\n",s->app_id,s->title,s->x,s->y);fflush(stdout);}
    shell_dirty|=1U<<SHELL_DOCK;refresh_scene();dirty=true;return (struct aurora_window*)s;
}
void aurora_host_x11_detach(struct aurora_window* w){struct surface* s=(struct surface*)w;if(s&&s->x11&&s->role)wl_resource_destroy(s->role);}
void aurora_host_x11_title(struct aurora_window* w,const char* title,const char* app_id){
    struct surface* s=(struct surface*)w;if(!s||!s->x11)return;char old[sizeof(s->title)];memcpy(old,s->title,sizeof(old));
    if(title)snprintf(s->title,sizeof(s->title),"%s",title);if(app_id)snprintf(s->app_id,sizeof(s->app_id),"%s",app_id);
    if(strcmp(old,s->title)){printf("[64] AURORA TITLE app=%s title=%s x=%d y=%d\n",s->app_id,s->title,s->x,s->y);fflush(stdout);}
    shell_dirty|=(1U<<SHELL_MENUBAR)|(1U<<SHELL_DOCK);dirty=true;
}
void aurora_host_x11_moved(struct aurora_window* w,int x,int y){
    struct surface* s=(struct surface*)w;if(!s||!s->x11||(s->x==x&&s->y==y))return;
    s->x=x;s->y=y;if(!s->maximized&&!s->fullscreen&&!s->tile){struct aurora_output* o=aurora_output_at(x,y);if(o){s->screen=o->index;s->screen_set=true;}}
    popups_reconstrain(s);dirty=true;
}
void aurora_host_x11_resized(struct aurora_window* w,int width,int height){struct surface* s=(struct surface*)w;if(s&&s->x11){s->requested_w=width;s->requested_h=height;dirty=true;}}
void aurora_host_x11_limits(struct aurora_window* w,int min_w,int min_h,int max_w,int max_h){
    struct surface* s=(struct surface*)w;if(!s||!s->x11)return;s->min_w=min_w>0?min_w:0;s->min_h=min_h>0?min_h:0;s->max_w=max_w>0?max_w:0;s->max_h=max_h>0?max_h:0;}
void aurora_host_x11_geometry(struct aurora_window* w,int x,int y,int width,int height){
    struct surface* s=(struct surface*)w;if(!s||!s->x11)return;
    if(width<=0||height<=0){s->geometry_x=s->geometry_y=s->geometry_w=s->geometry_h=0;}
    else{s->geometry_x=x;s->geometry_y=y;s->geometry_w=width;s->geometry_h=height;}
    s->geometry_clamp=s->geometry_w>0;dirty=true;   /* checked against the surface at its next commit (window_bounds) */
}
void aurora_host_x11_icon(struct aurora_window* w,const uint32_t* argb){
    struct surface* s=(struct surface*)w;if(!s||!s->x11)return;
    if(!argb){free(s->x11_icon);s->x11_icon=NULL;}
    else{if(!s->x11_icon&&!(s->x11_icon=malloc(AURORA_XICON*AURORA_XICON*4)))return;memcpy(s->x11_icon,argb,AURORA_XICON*AURORA_XICON*4);}
    shell_dirty|=1U<<SHELL_DOCK;dirty=true;
}
void aurora_host_x11_transient(struct aurora_window* w,struct aurora_window* parent,bool modal){
    struct surface* s=(struct surface*)w,*p=(struct surface*)parent;int steps=0;if(!s||!s->x11)return;
    if(p&&(!p->resource||!p->role||p->kind!=ROLE_TOPLEVEL))p=NULL;
    for(struct surface* q=p;q&&steps<64;q=q->transient_for,steps++)if(q==s){p=NULL;break;}   /* never a loop */
    if(steps>=64)p=NULL;
    s->transient_for=p;s->x11_modal=p&&modal;if(p)wm_follow(s,p->workspace);
}
void aurora_host_x11_request(struct aurora_window* w,int request){
    struct surface* s=(struct surface*)w;if(!s||!s->x11||!s->role)return;
    switch(request){
    case AURORA_XREQ_MAXIMIZE:if(!s->maximized)top_maximize(NULL,s->role);break;
    case AURORA_XREQ_UNMAXIMIZE:if(s->maximized)top_unmaximize(NULL,s->role);break;
    case AURORA_XREQ_FULLSCREEN:if(!s->fullscreen)top_fullscreen(NULL,s->role,NULL);break;
    case AURORA_XREQ_UNFULLSCREEN:if(s->fullscreen)top_unfullscreen(NULL,s->role);break;
    case AURORA_XREQ_MINIMIZE:if(!s->minimized)top_minimize(NULL,s->role);break;
    case AURORA_XREQ_ACTIVATE:aurora_host_activate(w);break;
    default:break;
    }
}
bool aurora_host_x11_begin(struct aurora_window* w,uint32_t edges){   /* _NET_WM_MOVERESIZE: only for the window the held button went to */
    struct surface* s=(struct surface*)w;
    if(!s||(pointer_grab!=s&&popup_family(button_origin)!=s))return false;
    return x11_begin(s,edges);
}
static bool x11_begin(struct surface* s,uint32_t edges){
    struct popup_box box;
    if(!aurora_mode||!s||!s->x11||!s->role||!s->mapped||!(pointer_buttons&1)||moving||popup_grab||s->fullscreen)return false;
    if(!(s->maximized||s->tile)||window_bounds(s,&box)<=0)box=(struct popup_box){0,0,s->requested_w,s->requested_h};   /* the frame size Aurora gave (the buffer may lag) */
    if(s->maximized||s->tile){int old_w=box.w,grab=pointer_x-(s->x+box.x);s->maximized=false;s->tile=0;restore_geometry(s);
        s->x=pointer_x-(int)((int64_t)grab*s->requested_w/(old_w?old_w:1))-box.x;s->y=pointer_y-14-box.y;box.w=s->requested_w;box.h=s->requested_h;configure(s);}
    moving=s;resize_edges=edges;move_x=s->x;move_y=s->y;move_w=box.w;move_h=box.h;move_px=pointer_x;move_py=pointer_y;
    pointer_grab=NULL;button_origin=NULL;button_serial=0;swallowed_buttons|=pointer_buttons&~1U;pointer_update();
    if(edges)configure(s);
    printf("[64] AURORA X11 %s app=%s\n",edges?"RESIZE":"MOVE",s->app_id);fflush(stdout);return true;
}
void aurora_host_x11_place(struct aurora_window* parent,int w,int h,int* x,int* y){
    struct surface* p=(struct surface*)parent;struct popup_box a;
    if(p&&p->role&&p->resource&&p->mapped){struct popup_box b;if(p->x11||window_bounds(p,&b)<=0)b=(struct popup_box){0,0,p->requested_w,p->requested_h};
        *x=p->x+b.x+(b.w-w)/2;*y=p->y+b.y+(b.h-h)/2;a=work_area(surface_screen(p),false);}   /* a dialog: centred over its window */
    else{a=work_area(surface_screen(NULL),false);   /* cascade on the display under the pointer, as for Wayland windows */
        *x=a.x+60+(int)(window_sequence++%6)*28;*y=a.y+28+(int)(window_sequence%5)*24;}
    if((int64_t)*x+w>a.x+a.w)*x=a.x+a.w-w;if((int64_t)*y+h>a.y+a.h)*y=a.y+a.h-h;if(*x<a.x)*x=a.x;if(*y<a.y)*y=a.y;
}
void aurora_host_x11_work_area(int x,int y,int* ax,int* ay,int* aw,int* ah){
    struct aurora_output* o=aurora_output_at(x,y);if(!o)o=aurora_output_nearest(x,y);struct popup_box a=work_area(o,false);*ax=a.x;*ay=a.y;*aw=a.w;*ah=a.h;}
bool aurora_host_x11_focused(void){struct surface* f=popup_family(focus);return f&&f->x11;}
int aurora_host_x11_stack(struct aurora_xwin** list,int capacity){int n=0;struct surface* s;wl_list_for_each(s,&windows,window_link)if(s->x11&&n<capacity)list[n++]=s->x11;return n;}
void aurora_host_x11_selection(const char* const* mimes,int count){
    if(count<=0){if(selection&&selection->x11){source_release(selection);selection=NULL;selection_focus();}return;}
    struct source* source=calloc(1,sizeof(*source));if(!source)return;
    for(int i=count-1;i>=0;i--){size_t n=strlen(mimes[i]);struct mime_type* t=malloc(sizeof(*t)+n+1);if(!t)break;memcpy(t->name,mimes[i],n+1);t->next=source->types;source->types=t;}
    source->references=1;source->used=true;source->x11=true;
    if(selection){if(selection->resource)wl_data_source_send_cancelled(selection->resource);source_release(selection);}
    selection=source;selection_focus();clipboard_record(source);
}
bool aurora_host_selection_send(const char* mime,int fd){   /* like a Wayland client's offer: only for the focused (X) window */
    struct surface* f=popup_family(focus);
    if(!selection||selection->x11||!f||!f->x11||!mime||!source_has(selection,mime)){close(fd);return false;}
    if(selection->owned){aurora_clipboard_serve(fd,selection->owned,selection->owned_size);return true;}
    if(selection->resource){wl_data_source_send_send(selection->resource,mime,fd);close(fd);return true;}
    close(fd);return false;
}
int aurora_host_selection_types(const char** types,int capacity){int n=0;if(!selection||selection->x11)return 0;for(struct mime_type* t=selection->types;t&&n<capacity;t=t->next)types[n++]=t->name;return n;}
void aurora_host_canvas_restore(void){shell_canvas();}

/* ---- services for aurora_protocols.c ---- */
struct aurora_viewport_state* aurora_host_viewport(struct wl_resource* resource, struct wl_resource*** owner) {
    if (!resource || !wl_resource_instance_of(resource, &wl_surface_interface, &surface_interface)) return NULL;
    struct surface* s = wl_resource_get_user_data(resource); *owner = &s->viewport; return &s->vp_next;
}
const struct rectangle* aurora_host_region(struct wl_resource* resource) {
    if (!resource || !wl_resource_instance_of(resource, &wl_region_interface, &region_interface)) return NULL;
    return ((struct region*)wl_resource_get_user_data(resource))->rectangles;
}
static struct surface* host_toplevel(struct wl_resource* resource) {
    if (!resource || !wl_resource_instance_of(resource, &xdg_toplevel_interface, &toplevel_interface)) return NULL;
    struct surface* s = wl_resource_get_user_data(resource); return s && s->role == resource && s->resource ? s : NULL;
}
bool aurora_host_toplevel_configured(struct wl_resource* toplevel) { struct surface* s = host_toplevel(toplevel); return s && s->configured; }
bool aurora_host_toplevel_has_buffer(struct wl_resource* toplevel) { struct surface* s = host_toplevel(toplevel); return s && (s->buffer || s->pixels); }
void aurora_host_toplevel_configure(struct wl_resource* toplevel) { struct surface* s = host_toplevel(toplevel); if (s && s->configured) configure(s); }
void aurora_host_output_size(int32_t* w, int32_t* h) { *w = (int32_t)width; *h = (int32_t)height; }
bool aurora_host_output_info(struct wl_resource* resource, int32_t* x, int32_t* y, int32_t* w, int32_t* h, char* name, char* description, size_t size) {
    if (!resource || !wl_resource_instance_of(resource, &wl_output_interface, &output_interface)) return false;
    struct output* o = wl_resource_get_user_data(resource); struct aurora_output* d = o && o->slot >= 0 ? aurora_output_slot(o->slot) : NULL;
    if (!d || !d->enabled) return false;
    *x = d->x; *y = d->y; *w = d->w; *h = d->h;
    snprintf(name, 32, "%s", d->name); snprintf(description, size, "%s %s (%s)", d->make, d->model, d->name);
    return true;
}

static int child_pipe[2]={-1,-1};
static void child_signal(int number){(void)number;int saved=errno;if(child_pipe[1]>=0){ssize_t written=write(child_pipe[1],"",1);(void)written;}errno=saved;}
static int child_ready(int fd,uint32_t mask,void* data){(void)mask;(void)data;char drain[64];while(read(fd,drain,sizeof(drain))>0){}return 0;}
/* Milliseconds until the menu-bar clock (minute resolution) can next change. */
static uint32_t clock_delay(void){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return 60000-(uint32_t)((t.tv_sec%60)*1000+t.tv_nsec/1000000)+20;}
static int loop_timeout(uint32_t now,uint32_t clock_due){
    int timeout=-1;
    if(dirty){uint64_t at=nanoseconds(),due=frame_due();timeout=at>=due||paint_asap?0:(int)((due-at+999999)/1000000);}
    if(cursor_dirty)timeout=0;
    if(aurora_mode){int32_t left=(int32_t)(clock_due-now);if(left<0)left=0;if(timeout<0||left<timeout)timeout=left;}
    if(shell_notice[0]){int32_t left=(int32_t)(notice_until-now);if(left<0)left=0;if(timeout<0||left<timeout)timeout=left;}
    if(shutting_down&&(timeout<0||timeout>250))timeout=250;   /* shutdown_step() */
    if(aurora_mode){int power=aurora_power_timeout(now);if(power>=0&&(timeout<0||power<timeout))timeout=power;}
    if(aurora_mode&&(timeout<0||timeout>5000))timeout=5000;   /* aurora_lock_tick(): idle lock, lock screen clock */
    return timeout;
}
/* --stats: every 5 s, the compositor's own CPU time (process clock) and per-paint/per-commit CPU cost. */
static void stats_report(void){
    static uint64_t cpu_mark;uint64_t now=nanoseconds(),cpu=cpu_nanoseconds(CLOCK_PROCESS_CPUTIME_ID);
    if(!stats.last_report){stats.last_report=now;cpu_mark=cpu;return;}
    if(now-stats.last_report<5000000000ULL)return;
    unsigned divisor=stats.paints?stats.paints:1;
    printf("[64] AURORA STATS window_ms=%llu cpu_ms=%llu stage_us=%llu/%llu/%llu commit_to_paint_us=%llu frame_to_commit_us=%llu paints=%u avg_paint_us=%llu max_paint_us=%llu drawn_mpx=%.2f present_mb=%.2f cursor_only=%u commits=%u avg_copy_us=%llu copy_mb=%.2f\n",
        (unsigned long long)((now-stats.last_report)/1000000),(unsigned long long)((cpu-cpu_mark)/1000000),
        (unsigned long long)(stats.stage_ns[0]/divisor/1000),(unsigned long long)(stats.stage_ns[1]/divisor/1000),(unsigned long long)(stats.stage_ns[2]/divisor/1000),
        (unsigned long long)(stats.waits?stats.commit_wait_ns/stats.waits/1000:0),(unsigned long long)(stats.turns?stats.client_ns/stats.turns/1000:0),
        stats.paints,(unsigned long long)(stats.paints?stats.paint_ns/stats.paints/1000:0),(unsigned long long)(stats.paint_max_ns/1000),
        stats.drawn_pixels/1e6,stats.present_bytes/1e6,stats.cursor_only,stats.commits,(unsigned long long)(stats.commits?stats.copy_ns/stats.commits/1000:0),stats.copy_bytes/1e6);
    fflush(stdout);
    uint64_t first=stats.first_commit,sent=stats.frame_sent;memset(&stats,0,sizeof(stats));stats.last_report=now;cpu_mark=cpu;stats.first_commit=first;stats.frame_sent=sent;
}
/* Hot-plug (kernel uevent) or a new layout (Settings): windows stay on their display (they move with it); those on a
 * display that went away move to the primary one. screen_origin: each slot's position at the previous layout. */
static int screen_origin[AURORA_OUTPUTS][3];   /* x, y, valid */
static void screens_remember(void){for(int i=0;i<AURORA_OUTPUTS;i++){struct aurora_output* d=aurora_output_slot(i);
    screen_origin[i][2]=d&&d->enabled&&d->mirror<0;if(screen_origin[i][2]){screen_origin[i][0]=d->x;screen_origin[i][1]=d->y;}}}
static void outputs_changed(void) {
    int w,h;aurora_outputs_bounds(&w,&h);width=(unsigned)w;height=(unsigned)h;
    outputs_sync();
    struct surface* s;
    {   /* the pointer stays where it was on its display */
        struct aurora_output* d=NULL;for(int i=0;i<AURORA_OUTPUTS&&!d;i++)if(screen_origin[i][2]&&pointer_x>=screen_origin[i][0]&&pointer_y>=screen_origin[i][1]){
            struct aurora_output* c=aurora_output_slot(i);if(c&&c->enabled&&c->mirror<0&&pointer_x<screen_origin[i][0]+c->w&&pointer_y<screen_origin[i][1]+c->h)d=c;}
        if(d){pointer_x+=d->x-screen_origin[d->index][0];pointer_y+=d->y-screen_origin[d->index][1];}
    }
    wl_list_for_each(s,&windows,window_link){
        if(!s->role||!s->resource)continue;
        struct aurora_output* d=s->screen_set?aurora_output_slot(s->screen):NULL;
        if(d&&d->enabled&&d->mirror<0&&screen_origin[d->index][2]&&(d->x!=screen_origin[d->index][0]||d->y!=screen_origin[d->index][1])){
            s->x+=d->x-screen_origin[d->index][0];s->y+=d->y-screen_origin[d->index][1];
            printf("[64] AURORA WINDOW SHIFTED app=%s screen=%s x=%d y=%d\n",s->app_id,d->name,s->x,s->y);fflush(stdout);}
        if(!d||!d->enabled||d->mirror>=0){   /* its display is gone (or now mirrors another): keep the offset on the primary one */
            struct aurora_output* p=aurora_output_primary();struct popup_box a=work_area(p,false),b;
            int bw=window_bounds(s,&b)>0?b.w:s->requested_w;
            s->x=a.x+40;s->y=a.y+30;if(bw<a.w)s->x=a.x+(a.w-bw)/2;
            s->screen=p->index;s->screen_set=true;
            printf("[64] AURORA WINDOW MOVED app=%s screen=%s x=%d y=%d\n",s->app_id,p->name,s->x,s->y);fflush(stdout);
        }
        else if(!s->maximized&&!s->fullscreen&&!s->tile){   /* a display that shrank (scale, mode): the window stays inside it */
            struct popup_box a=work_area(d,false),b;a.h=d->y+d->h-a.y;   /* below the menu bar, down to the display's edge */
            if(window_bounds(s,&b)>0){int64_t right=(int64_t)s->x+b.x+b.w,bottom=(int64_t)s->y+b.y+b.h,nx=s->x,ny=s->y;
                if(right>a.x+a.w)nx-=right-(a.x+a.w);if(bottom>a.y+a.h)ny-=bottom-(a.y+a.h);
                if(nx+b.x<a.x)nx=a.x-b.x;if(ny+b.y<a.y)ny=a.y-b.y;
                if(nx!=s->x||ny!=s->y){s->x=(int)nx;s->y=(int)ny;printf("[64] AURORA WINDOW SHIFTED app=%s screen=%s x=%d y=%d\n",s->app_id,d->name,s->x,s->y);fflush(stdout);}}
        }
        if(s->configured&&(s->maximized||s->fullscreen||s->tile))configure(s);   /* the display may have a new size */
    }
    wl_list_for_each(s,&surfaces,link)if(s->resource)aurora_protocols_preferred_scale(s->resource,aurora_host_surface_scale(s->resource));   /* scales may have changed */
    { int64_t x=pointer_x,y=pointer_y;aurora_outputs_clamp(&x,&y);pointer_x=(int)x;pointer_y=(int)y; }
    motion_abs_last[0]=motion_abs_last[1]=-1;   /* the tablet's display may have moved */
    screens_remember();
    memset(output_cursor,0,sizeof(output_cursor));last_count=0;
    wl_list_for_each(s,&surfaces,link)s->was_shown=false;
    screen_damage_count=0;damage_screen(screen_box());shell_dirty=SHELL_ALL;cursor_dirty=true;
    refresh_scene();
    printf("[64] AURORA OUTPUTS CHANGED count=%d desktop=%ux%u\n",aurora_outputs_count(),width,height);fflush(stdout);
}
static int outputs_event(int fd,uint32_t mask,void* data){(void)fd;(void)mask;(void)data;if(aurora_outputs_rescan())outputs_changed();return 0;}
static int setup(void) {
    if(!aurora_outputs_open()){perror("Nexus desktop displays");return -1;}
    {int w,h;aurora_outputs_bounds(&w,&h);width=(unsigned)w;height=(unsigned)h;}
    struct aurora_output* primary=aurora_output_primary();
    pointer_x=primary->x+primary->w/2;pointer_y=primary->y+primary->h/2;
    shell_canvas();screens_remember();
    if(aurora_mode){
        if(primary->mode_w<640||primary->mode_h<480){errno=ENOTSUP;return -1;}
        if(!ui_set_apps(aurora_apps,sizeof(aurora_apps)/sizeof(*aurora_apps),8)){errno=EINVAL;return -1;}
        wineapps_refresh(aurora_apps,sizeof(aurora_apps)/sizeof(*aurora_apps),8);
        ui_wallpaper_init();dock_init();dock_set_running_query(app_running);dock_set_extra(dock_extra_count,dock_extra_draw,dock_extra_name,dock_extra_running);
    }
    wl_list_init(&surfaces);wl_list_init(&endpoints);wl_list_init(&data_devices);wl_list_init(&outputs);wl_list_init(&popups);wl_list_init(&windows);xkb_context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    wl_list_init(&x11_overrides);
    if(!xkb_context)return -1;
    if(!keymap_build("us",""))return -1;
    display=wl_display_create();if(!display||wl_display_init_shm(display))return -1;
    aurora_dmabuf_init(display); /* GPU clients: zwp_linux_dmabuf_v1 when /dev/dri/renderD128 exists */
    if(!wl_global_create(display,&wl_compositor_interface,4,NULL,bind_compositor)||!wl_global_create(display,&xdg_wm_base_interface,5,NULL,bind_shell)||!wl_global_create(display,&wl_seat_interface,5,NULL,bind_seat))return -1;
    outputs_sync();   /* one wl_output per display */
    if(aurora_control_init(wl_display_get_event_loop(display)))aurora_outputs_control(outputs_changed);   /* Settings > Displays */
    if(aurora_mode)aurora_sak_init(display);   /* the secure attention key's dialog check (aurora_sak.h) */
    if(aurora_mode)aurora_session_init(display);   /* user sessions: the desktop user's own, others through nexus-session */
    if(aurora_mode&&!aurora_idle_init(display))return -1;   /* zwp_idle_inhibit_manager_v1 (aurora_idle.h) */
    if(aurora_mode)aurora_lock_init(wl_display_get_event_loop(display));   /* lock screen, greeter, idle and sleep */
    if(aurora_mode){aurora_a11y_init();aurora_keyboard_init();static const menubar_status_t input_menu={input_menu_width,input_menu_draw,UI_ACT_INPUT_NEXT};menubar_status_add(&input_menu);}
    if(aurora_outputs_event_fd()>=0&&!wl_event_loop_add_fd(wl_display_get_event_loop(display),aurora_outputs_event_fd(),WL_EVENT_READABLE,outputs_event,NULL))return -1;
    if(!wl_global_create(display,&wl_data_device_manager_interface,3,NULL,bind_data_manager))return -1;   /* v3: drag-and-drop actions */
    if(!wl_global_create(display,&wl_subcompositor_interface,1,NULL,bind_subcompositor))return -1;
    if(!aurora_protocols_init(display))return -1; /* viewporter, pointer constraints, relative pointer, xdg-output, xdg-decoration */
    if(aurora_mode)aurora_power_init(wl_display_get_event_loop(display)); /* battery, brightness, power keys, sleep notifications */
    if(aurora_mode&&!aurora_ime_init(display))return -1;   /* text-input-v3, input-method-v2, virtual-keyboard-v1 */
    if(aurora_mode)aurora_xwm_init(display);   /* rootless Xwayland: Aurora is the X11 window manager (aurora_xwm.h) */
    if(setenv("XDG_RUNTIME_DIR","/run/user/1000",1)||setenv("WAYLAND_DISPLAY","wayland-0",1)||
       setenv("XDG_SESSION_TYPE","wayland",1)||setenv("XDG_CURRENT_DESKTOP","NexusOS",1)||wl_display_add_socket(display,"wayland-0"))return -1;
    for(unsigned i=0;i<2;i++){int fd=open(i?"/dev/input/event1":"/dev/input/event0",O_RDONLY|O_NONBLOCK|O_CLOEXEC);int clock=CLOCK_MONOTONIC;if(fd<0||ioctl(fd,EVIOCSCLOCKID,&clock)||!wl_event_loop_add_fd(wl_display_get_event_loop(display),fd,WL_EVENT_READABLE,input_event,(void*)(uintptr_t)i))return -1;}
    /* SIGCHLD wakes the event loop through a self-pipe, so no periodic polling is needed. */
    struct sigaction action={.sa_handler=child_signal,.sa_flags=SA_RESTART|SA_NOCLDSTOP};sigemptyset(&action.sa_mask);
    if(pipe2(child_pipe,O_NONBLOCK|O_CLOEXEC)||!wl_event_loop_add_fd(wl_display_get_event_loop(display),child_pipe[0],WL_EVENT_READABLE,child_ready,NULL)||sigaction(SIGCHLD,&action,NULL))return -1;
    return 0;
}
static void free_arguments(char** command){if(command){for(char** p=command;*p;p++)free(*p);free(command);}}
static char** load_arguments(const char* path,const char* program,int* count){
    FILE* file=fopen(path,"r");if(!file)return NULL;
    char* line=NULL;size_t capacity=0,total=0;ssize_t size;int n=1;char** command=calloc(2,sizeof(*command));
    if(!command){fclose(file);return NULL;}command[0]=strdup(program);if(!command[0]){free(command);fclose(file);return NULL;}
    while((size=getline(&line,&capacity,file))>=0){
        if(memchr(line,0,(size_t)size)){errno=EINVAL;goto failed;}
        while(size&&(line[size-1]=='\n'||line[size-1]=='\r'))line[--size]=0;if(!size)continue;
        if(size>32767||(total+=(size_t)size+1)>2*1024*1024){errno=E2BIG;goto failed;}
        char** next=realloc(command,(size_t)(n+2)*sizeof(*next));if(!next)goto failed;command=next;
        command[n]=strdup(line);command[n+1]=NULL;if(!command[n])goto failed;n++;
    }
    if(ferror(file))goto failed;free(line);fclose(file);*count=n;return command;
failed: {int error=errno?errno:EIO;free(line);fclose(file);free_arguments(command);errno=error;return NULL;}
}
int main(int argc,char** argv) {
    bool chrome_session=argc==2&&!strcmp(argv[1],"--chrome-session");
    if(argc==1||chrome_session){int count=0;char** command=load_arguments(chrome_session?chrome_arguments("/nexus/chrome-desktop.args"):"/nexus/desktop.args",argv[0],&count);
        if(command){argc=count;argv=command;}else if(chrome_session||errno!=ENOENT){perror("desktop arguments");return 1;}}
    if(argc>1&&!strcmp(argv[1],"--trace")){if(setenv("WAYLAND_DEBUG","server",1))return 1;for(int i=1;i<argc;i++)argv[i]=argv[i+1];argc--;}
    if(argc>1&&!strcmp(argv[1],"--aurora")){aurora_mode=true;for(int i=1;i<argc;i++)argv[i]=argv[i+1];argc--;}
    if(argc>1&&!strcmp(argv[1],"--stats")){stats_enabled=true;for(int i=1;i<argc;i++)argv[i]=argv[i+1];argc--;}
    if(setup()){perror("Nexus desktop setup");return 1;}fb_mark_dirty_all();paint();puts("[64] DESKTOP READY");fflush(stdout);
    aurora_services_start(wl_display_get_event_loop(display),aurora_mode?aurora_session_notify:shell_message); /* notifications (history, Do Not Disturb: aurora_notices.c), nexusd services + journal (aurora_services.c) */
    if(aurora_mode){puts("[64] AURORA READY persistent desktop");fflush(stdout);aurora_netstatus_boot();aurora_media_init(wl_display_get_event_loop(display),shell_message);aurora_sound_init(wl_display_get_event_loop(display),shell_message);} /* USB drives: automount + notices */
    pid_t child=-1;if(argc>1){child=fork();if(child<0){perror("desktop fork");return 1;}if(!child){execv(argv[1],argv+1);perror("desktop exec");_exit(127);}}
    uint32_t clock_due=milliseconds()+clock_delay();int status=0;bool main_done=false;char clock_text[64];ui_format_clock(clock_text);
    /* Event driven: sleep until input, a client request, a child exit or the next due frame/clock/notice. */
    for(;;){wl_display_flush_clients(display);
        int timeout=loop_timeout(milliseconds(),clock_due);uint64_t asked=timeout>0?nanoseconds()+(uint64_t)timeout*1000000:0;
        if(wl_event_loop_dispatch(wl_display_get_event_loop(display),timeout)<0&&errno!=EINTR){perror("desktop dispatch");status=1;break;}
        if(asked){uint64_t woke=nanoseconds();if(woke>=asked)wake_slack_ns=(wake_slack_ns*7+(woke-asked))/8;}
        uint32_t now=milliseconds();
        if(aurora_mode&&aurora_netstatus_poll()){shell_dirty|=1U<<SHELL_MENUBAR;dirty=true;}   /* network status item */
        if(aurora_mode){aurora_lock_tick();aurora_reader_tick();}
        if(aurora_mode&&(int32_t)(now-clock_due)>=0){char next[64];clock_due=now+clock_delay();ui_format_clock(next);if(strcmp(next,clock_text)){strcpy(clock_text,next);shell_dirty|=1U<<SHELL_MENUBAR;dirty=true;}}
        if(shell_notice[0]&&(int32_t)(now-notice_until)>=0){shell_notice[0]=0;dirty=true;}
        if(child>0||aurora_mode){int result;pid_t done;
            while((done=waitpid(-1,&result,WNOHANG))>0){
                if(aurora_services_reaped(done,result,shutting_down))continue;
                if(aurora_mode&&aurora_lock_reaped(done,result))continue;   /* a lock screen password check */
                if(aurora_mode&&aurora_session_reaped(done,result))continue;   /* a session launcher/agent (aurora_session.h) */
                if(aurora_mode&&aurora_reader_reaped(done))continue;   /* the screen reader (aurora_reader.h) */
                if(aurora_mode&&aurora_xwm_reaped(done,result))continue;   /* rootless Xwayland (aurora_xwm.h) */
                if(aurora_mode){shell_reaped(done);printf("[64] AURORA CHILD EXIT pid=%ld status=%d\n",(long)done,result);fflush(stdout);if(!aurora_power_reaped(done,result)&&!wineapps_reaped(done)&&result&&!shutting_down)shell_message("An application exited. The desktop is still running.");}
                else if(done==child&&!main_done){status=WIFEXITED(result)?WEXITSTATUS(result):1;main_done=true;}
            }
            /* Adopted installers/apps must finish even if their launching browser has closed. */
            if(!aurora_mode&&main_done)aurora_services_stop();
            if(done<0&&errno==ECHILD&&(aurora_mode?shutting_down:main_done))break;
            if(done<0&&errno!=ECHILD&&errno!=EINTR){perror("desktop wait");status=1;break;}
            if(aurora_mode&&shutting_down&&!done)shutdown_step(now);
        }
        if(dirty&&(paint_asap||nanoseconds()>=frame_due()))paint();
        else if(cursor_dirty)present_cursor(); /* The pointer never waits for the frame cadence. */
        if(aurora_mode){int action=aurora_power_tick(milliseconds(),!dirty);if(action)shell_action(action);}
        if(stats_enabled)stats_report();
    }
    wl_display_destroy_clients(display);
    if(!wl_list_empty(&surfaces)||!wl_list_empty(&windows)||!wl_list_empty(&popups)||!wl_list_empty(&endpoints)||!wl_list_empty(&outputs)||!wl_list_empty(&data_devices)||selection){fprintf(stderr,"[64] DESKTOP RESOURCE LEAK\n");status=1;}
    if(aurora_mode)ui_wallpaper_release();aurora_canvas(NULL,0,0);
    wl_display_destroy(display);close(keymap_fd);xkb_state_unref(key_state);xkb_keymap_unref(keymap);xkb_context_unref(xkb_context);aurora_outputs_close();
    free(items);free(last_order);free(output_items);signal(SIGCHLD,SIG_DFL);int pipe_end=child_pipe[1];child_pipe[1]=-1;close(pipe_end);close(child_pipe[0]);
    printf("[64] DESKTOP EXIT status=%d\n",status);if(aurora_mode)aurora_power_finish();return status;
}
