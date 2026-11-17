/* Aurora protocol extensions (see aurora_protocols.h): the protocol objects of wp_viewporter,
 * zwp_pointer_constraints_v1, zwp_relative_pointer_manager_v1, zxdg_output_manager_v1 and
 * zxdg_decoration_manager_v1. Viewport state is applied and drawn by desktop.c; pointer locking,
 * confinement and relative motion are driven from desktop.c's input path through the entry points below.
 *
 * Decorations: Aurora draws no window frames (macOS-style shell, client-drawn title bars), so every
 * toplevel is configured client_side whatever the client prefers; set_mode/unset_mode answer with a
 * new configure as the protocol requires. Wine's winewayland draws its own non-client area anyway. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "aurora_damage.h"
#pragma GCC diagnostic pop
#include "aurora_protocols.h"
#include "viewporter-server-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include <sys/random.h>
#include <time.h>

static void destroy_request(struct wl_client* client, struct wl_resource* resource) { (void)client; wl_resource_destroy(resource); }

/* ---- regions: copies of wl_region rectangle lists (NULL = infinite) ---- */
bool aurora_region_contains(const struct rectangle* region, int64_t x, int64_t y) {
    bool inside = false;
    for (const struct rectangle* r = region; r; r = r->next)
        if (x >= r->x && y >= r->y && x < (int64_t)r->x + r->w && y < (int64_t)r->y + r->h) inside = r->add;
    return inside;
}
static void region_free(struct rectangle* region) { while (region) { struct rectangle* next = region->next; free(region); region = next; } }
static bool region_copy(struct wl_resource* region, struct rectangle** out) {
    *out = NULL; if (!region) return true;
    struct rectangle** tail = out;
    for (const struct rectangle* r = aurora_host_region(region); r; r = r->next) {
        if (!(*tail = malloc(sizeof(**tail)))) { region_free(*out); *out = NULL; return false; }
        **tail = *r; (*tail)->next = NULL; tail = &(*tail)->next;
    }
    /* A region with nothing added is empty, not infinite: keep a zero-size marker so contains() is false. */
    if (!*out && !(*out = calloc(1, sizeof(**out)))) return false;
    return true;
}

/* ---- wp_viewporter ---- */
struct viewport { struct wl_resource* resource; struct wl_resource* surface; struct wl_listener surface_destroy; };
static void viewport_surface_gone(struct wl_listener* listener, void* data) {
    (void)data; struct viewport* v = wl_container_of(listener, v, surface_destroy);
    v->surface = NULL; wl_list_remove(&listener->link); wl_list_init(&listener->link);
}
static struct aurora_viewport_state* viewport_pending(struct wl_resource* resource) {
    struct viewport* v = wl_resource_get_user_data(resource); struct wl_resource** owner;
    if (!v->surface) { wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE, "the wl_surface was destroyed"); return NULL; }
    return aurora_host_viewport(v->surface, &owner);
}
static void viewport_destroy(struct wl_resource* resource) {
    struct viewport* v = wl_resource_get_user_data(resource);
    if (v->surface) {
        struct wl_resource** owner; struct aurora_viewport_state* state = aurora_host_viewport(v->surface, &owner);
        if (state) { state->source = state->destination = false; if (owner && *owner == resource) *owner = NULL; }
    }
    wl_list_remove(&v->surface_destroy.link); free(v);
}
static void viewport_source(struct wl_client* c, struct wl_resource* r, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h) {
    (void)c; struct aurora_viewport_state* state = viewport_pending(r); if (!state) return;
    wl_fixed_t unset = wl_fixed_from_int(-1);
    if (x == unset && y == unset && w == unset && h == unset) { state->source = false; return; }
    if (x < 0 || y < 0 || w <= 0 || h <= 0) { wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "source rectangle must have x,y >= 0 and w,h > 0"); return; }
    state->source = true; state->x = x; state->y = y; state->w = w; state->h = h;
}
static void viewport_destination(struct wl_client* c, struct wl_resource* r, int32_t w, int32_t h) {
    (void)c; struct aurora_viewport_state* state = viewport_pending(r); if (!state) return;
    if (w == -1 && h == -1) { state->destination = false; return; }
    if (w <= 0 || h <= 0) { wl_resource_post_error(r, WP_VIEWPORT_ERROR_BAD_VALUE, "destination size must be positive"); return; }
    state->destination = true; state->dw = w; state->dh = h;
}
static const struct wp_viewport_interface viewport_interface = {destroy_request, viewport_source, viewport_destination};
static void get_viewport(struct wl_client* c, struct wl_resource* manager, uint32_t id, struct wl_resource* surface) {
    struct wl_resource** owner; struct aurora_viewport_state* state = aurora_host_viewport(surface, &owner);
    if (!state) { wl_resource_post_error(manager, WL_DISPLAY_ERROR_INVALID_OBJECT, "not an Aurora surface"); return; }
    if (*owner) { wl_resource_post_error(manager, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, "the surface already has a viewport"); return; }
    struct viewport* v = calloc(1, sizeof(*v)); struct wl_resource* r = wl_resource_create(c, &wp_viewport_interface, wl_resource_get_version(manager), id);
    if (!v || !r) { free(v); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    v->resource = r; v->surface = surface; v->surface_destroy.notify = viewport_surface_gone;
    wl_resource_add_destroy_listener(surface, &v->surface_destroy);
    wl_resource_set_implementation(r, &viewport_interface, v, viewport_destroy);
    *state = (struct aurora_viewport_state){0}; *owner = r;
}
static const struct wp_viewporter_interface viewporter_interface = {destroy_request, get_viewport};
static void bind_viewporter(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &wp_viewporter_interface, version, id);
    if (!r) wl_client_post_no_memory(c); else wl_resource_set_implementation(r, &viewporter_interface, NULL, NULL);
}

/* ---- zxdg_output_manager_v1: logical position/size and name of each wl_output (desktop.c's displays) ---- */
struct xdg_output { struct wl_list link; struct wl_resource *resource, *output; struct wl_listener output_destroy; };
static struct wl_list xdg_outputs;
static void xdg_output_orphaned(struct wl_listener* listener, void* data) {
    (void)data; struct xdg_output* x = wl_container_of(listener, x, output_destroy);
    x->output = NULL; wl_list_remove(&listener->link); wl_list_init(&listener->link);
}
static void xdg_output_destroy(struct wl_resource* resource) {
    struct xdg_output* x = wl_resource_get_user_data(resource);
    wl_list_remove(&x->link); wl_list_remove(&x->output_destroy.link); free(x);
}
static void xdg_output_send(struct xdg_output* x, bool initial) {
    int32_t px, py, width, height; char name[32], description[96];
    if (!x->output || !aurora_host_output_info(x->output, &px, &py, &width, &height, name, description, sizeof(description))) return;
    uint32_t version = wl_resource_get_version(x->resource);
    zxdg_output_v1_send_logical_position(x->resource, px, py);
    zxdg_output_v1_send_logical_size(x->resource, width, height);
    if (version >= 2 && initial) { zxdg_output_v1_send_name(x->resource, name); zxdg_output_v1_send_description(x->resource, description); }   /* name: once */
    else if (version == 2) zxdg_output_v1_send_description(x->resource, description);
    if (version >= 3) { if (initial && wl_resource_get_version(x->output) >= 2) wl_output_send_done(x->output); }   /* xdg_output.done is deprecated in v3 */
    else zxdg_output_v1_send_done(x->resource);
}
static const struct zxdg_output_v1_interface xdg_output_interface = {destroy_request};
static void get_xdg_output(struct wl_client* c, struct wl_resource* manager, uint32_t id, struct wl_resource* output) {
    uint32_t version = wl_resource_get_version(manager);
    struct xdg_output* x = calloc(1, sizeof(*x)); struct wl_resource* r = wl_resource_create(c, &zxdg_output_v1_interface, version, id);
    if (!x || !r) { free(x); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    x->resource = r; x->output = output; x->output_destroy.notify = xdg_output_orphaned;
    wl_resource_add_destroy_listener(output, &x->output_destroy); wl_list_insert(xdg_outputs.prev, &x->link);
    wl_resource_set_implementation(r, &xdg_output_interface, x, xdg_output_destroy);
    xdg_output_send(x, true);
}
/* Before desktop.c re-sends a wl_output's state: its xdg_outputs' new position/size (v3: applied by that wl_output.done). */
void aurora_protocols_output_changed(struct wl_resource* output) {
    struct xdg_output* x; wl_list_for_each(x, &xdg_outputs, link) if (x->output == output) xdg_output_send(x, false);
}
static const struct zxdg_output_manager_v1_interface xdg_output_manager_interface = {destroy_request, get_xdg_output};
static void bind_xdg_output(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &zxdg_output_manager_v1_interface, version, id);
    if (!r) wl_client_post_no_memory(c); else wl_resource_set_implementation(r, &xdg_output_manager_interface, NULL, NULL);
}

/* ---- zxdg_decoration_manager_v1 ---- */
struct decoration { struct wl_list link; struct wl_resource *resource, *toplevel; struct wl_listener toplevel_destroy; };
static struct wl_list decorations;
static void decoration_orphaned(struct wl_listener* listener, void* data) {
    (void)data; struct decoration* d = wl_container_of(listener, d, toplevel_destroy);
    d->toplevel = NULL; wl_list_remove(&listener->link); wl_list_init(&listener->link);
}
static void decoration_configure(struct decoration* d) {
    zxdg_toplevel_decoration_v1_send_configure(d->resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    if (aurora_host_toplevel_configured(d->toplevel)) aurora_host_toplevel_configure(d->toplevel);
}
static void decoration_destroy(struct wl_resource* resource) {
    struct decoration* d = wl_resource_get_user_data(resource);
    wl_list_remove(&d->link); wl_list_remove(&d->toplevel_destroy.link); free(d);
}
static void decoration_set_mode(struct wl_client* c, struct wl_resource* r, uint32_t mode) {
    (void)c; struct decoration* d = wl_resource_get_user_data(r);
    if (!d->toplevel) { wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED, "xdg_toplevel destroyed before its decoration"); return; }
    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE && mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE, "unknown decoration mode"); return; }
    decoration_configure(d);   /* Aurora has no server-side frames: the answer is always client_side */
}
static void decoration_unset_mode(struct wl_client* c, struct wl_resource* r) {
    (void)c; struct decoration* d = wl_resource_get_user_data(r);
    if (!d->toplevel) { wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED, "xdg_toplevel destroyed before its decoration"); return; }
    decoration_configure(d);
}
static const struct zxdg_toplevel_decoration_v1_interface decoration_interface = {destroy_request, decoration_set_mode, decoration_unset_mode};
static void get_decoration(struct wl_client* c, struct wl_resource* manager, uint32_t id, struct wl_resource* toplevel) {
    struct decoration* d;
    wl_list_for_each(d, &decorations, link) if (d->toplevel == toplevel) {
        wl_resource_post_error(manager, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED, "the toplevel already has a decoration object"); return; }
    if (aurora_host_toplevel_has_buffer(toplevel)) {
        wl_resource_post_error(manager, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER, "the toplevel already has a buffer"); return; }
    d = calloc(1, sizeof(*d)); struct wl_resource* r = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(manager), id);
    if (!d || !r) { free(d); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    d->resource = r; d->toplevel = toplevel; d->toplevel_destroy.notify = decoration_orphaned;
    wl_resource_add_destroy_listener(toplevel, &d->toplevel_destroy); wl_list_insert(&decorations, &d->link);
    wl_resource_set_implementation(r, &decoration_interface, d, decoration_destroy);
    decoration_configure(d);
}
static const struct zxdg_decoration_manager_v1_interface decoration_manager_interface = {destroy_request, get_decoration};
static void bind_decoration(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &zxdg_decoration_manager_v1_interface, version, id);
    if (!r) wl_client_post_no_memory(c); else wl_resource_set_implementation(r, &decoration_manager_interface, NULL, NULL);
}

/* ---- zwp_relative_pointer_manager_v1 ---- */
struct relative { struct wl_list link; struct wl_resource* resource; };
static struct wl_list relatives;
static void relative_destroy(struct wl_resource* resource) { struct relative* p = wl_resource_get_user_data(resource); wl_list_remove(&p->link); free(p); }
static const struct zwp_relative_pointer_v1_interface relative_interface = {destroy_request};
static void get_relative(struct wl_client* c, struct wl_resource* manager, uint32_t id, struct wl_resource* pointer) {
    (void)pointer; struct relative* p = calloc(1, sizeof(*p));
    struct wl_resource* r = wl_resource_create(c, &zwp_relative_pointer_v1_interface, wl_resource_get_version(manager), id);
    if (!p || !r) { free(p); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    p->resource = r; wl_list_insert(relatives.prev, &p->link);
    wl_resource_set_implementation(r, &relative_interface, p, relative_destroy);
}
static const struct zwp_relative_pointer_manager_v1_interface relative_manager_interface = {destroy_request, get_relative};
static void bind_relative(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &zwp_relative_pointer_manager_v1_interface, version, id);
    if (!r) wl_client_post_no_memory(c); else wl_resource_set_implementation(r, &relative_manager_interface, NULL, NULL);
}
bool aurora_relative_motion(struct wl_client* client, uint64_t usec, wl_fixed_t dx, wl_fixed_t dy) {
    bool sent = false; struct relative* p;
    wl_list_for_each(p, &relatives, link) if (wl_resource_get_client(p->resource) == client) {
        /* No acceleration is applied, so the accelerated and unaccelerated deltas are the same. */
        zwp_relative_pointer_v1_send_relative_motion(p->resource, (uint32_t)(usec >> 32), (uint32_t)usec, dx, dy, dx, dy); sent = true;
    }
    return sent;
}

/* ---- zwp_pointer_constraints_v1 ---- */
struct constraint {
    struct wl_list link; struct wl_resource *resource, *surface; struct wl_listener surface_destroy;
    enum aurora_constraint_kind kind; uint32_t lifetime;
    struct rectangle *region, *pending; bool pending_set;
    bool hint, pending_hint; wl_fixed_t hint_x, hint_y, pending_x, pending_y;
    bool defunct;
};
static struct wl_list constraints;
static struct constraint* active;
static struct { bool valid; struct wl_resource* surface; wl_fixed_t x, y; } released;
static void constraint_deactivate(void) {
    struct constraint* c = active; if (!c) return;
    active = NULL;
    if (c->kind == AURORA_CONSTRAINT_LOCK) {
        zwp_locked_pointer_v1_send_unlocked(c->resource);
        if (c->hint && c->surface) released = (typeof(released)){true, c->surface, c->hint_x, c->hint_y};
    } else zwp_confined_pointer_v1_send_unconfined(c->resource);
    if (c->lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT) c->defunct = true;
    printf("[64] AURORA CONSTRAINT %s off\n", c->kind == AURORA_CONSTRAINT_LOCK ? "lock" : "confine"); fflush(stdout);
}
static void constraint_surface_gone(struct wl_listener* listener, void* data) {
    (void)data; struct constraint* c = wl_container_of(listener, c, surface_destroy);
    if (active == c) constraint_deactivate();
    if (released.surface == c->surface) released.valid = false, released.surface = NULL;
    c->surface = NULL; c->defunct = true; wl_list_remove(&listener->link); wl_list_init(&listener->link);
}
static void constraint_destroy(struct wl_resource* resource) {
    struct constraint* c = wl_resource_get_user_data(resource);
    if (active == c) {   /* destroying an active lock ends it: its position hint applies as on unlock */
        active = NULL;
        if (c->kind == AURORA_CONSTRAINT_LOCK && c->hint && c->surface) released = (typeof(released)){true, c->surface, c->hint_x, c->hint_y};
        printf("[64] AURORA CONSTRAINT %s off\n", c->kind == AURORA_CONSTRAINT_LOCK ? "lock" : "confine"); fflush(stdout);
    }
    wl_list_remove(&c->link); wl_list_remove(&c->surface_destroy.link); region_free(c->region); region_free(c->pending); free(c);
    aurora_host_constraints_changed();
}
static void constraint_region(struct wl_client* client, struct wl_resource* r, struct wl_resource* region) {
    (void)client; struct constraint* c = wl_resource_get_user_data(r); struct rectangle* copy;
    if (!region_copy(region, &copy)) { wl_resource_post_no_memory(r); return; }
    region_free(c->pending); c->pending = copy; c->pending_set = true;
}
static void lock_hint(struct wl_client* client, struct wl_resource* r, wl_fixed_t x, wl_fixed_t y) {
    (void)client; struct constraint* c = wl_resource_get_user_data(r); c->pending_hint = true; c->pending_x = x; c->pending_y = y;
}
static const struct zwp_locked_pointer_v1_interface locked_interface = {destroy_request, lock_hint, constraint_region};
static const struct zwp_confined_pointer_v1_interface confined_interface = {destroy_request, constraint_region};
static void constrain(struct wl_client* client, struct wl_resource* manager, uint32_t id, struct wl_resource* surface,
                      struct wl_resource* region, uint32_t lifetime, enum aurora_constraint_kind kind) {
    struct wl_resource** owner;
    if (!aurora_host_viewport(surface, &owner)) { wl_resource_post_error(manager, WL_DISPLAY_ERROR_INVALID_OBJECT, "not an Aurora surface"); return; }
    if (lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT && lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT) {
        wl_resource_post_error(manager, WL_DISPLAY_ERROR_INVALID_METHOD, "invalid constraint lifetime"); return; }
    struct constraint* c;
    wl_list_for_each(c, &constraints, link) if (c->surface == surface) {
        wl_resource_post_error(manager, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED, "the surface already has a pointer constraint"); return; }
    c = calloc(1, sizeof(*c));
    struct wl_resource* r = wl_resource_create(client, kind == AURORA_CONSTRAINT_LOCK ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface,
                                               wl_resource_get_version(manager), id);
    if (!c || !r || !region_copy(region, &c->region)) { if (c) free(c); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    c->resource = r; c->surface = surface; c->kind = kind; c->lifetime = lifetime; c->surface_destroy.notify = constraint_surface_gone;
    wl_resource_add_destroy_listener(surface, &c->surface_destroy); wl_list_insert(constraints.prev, &c->link);
    wl_resource_set_implementation(r, kind == AURORA_CONSTRAINT_LOCK ? (const void*)&locked_interface : (const void*)&confined_interface, c, constraint_destroy);
    aurora_host_constraints_changed();
}
static void lock_pointer(struct wl_client* client, struct wl_resource* manager, uint32_t id, struct wl_resource* surface,
                         struct wl_resource* pointer, struct wl_resource* region, uint32_t lifetime) {
    (void)pointer; constrain(client, manager, id, surface, region, lifetime, AURORA_CONSTRAINT_LOCK);
}
static void confine_pointer(struct wl_client* client, struct wl_resource* manager, uint32_t id, struct wl_resource* surface,
                            struct wl_resource* pointer, struct wl_resource* region, uint32_t lifetime) {
    (void)pointer; constrain(client, manager, id, surface, region, lifetime, AURORA_CONSTRAINT_CONFINE);
}
static const struct zwp_pointer_constraints_v1_interface constraints_interface = {destroy_request, lock_pointer, confine_pointer};
static void bind_constraints(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &zwp_pointer_constraints_v1_interface, version, id);
    if (!r) wl_client_post_no_memory(c); else wl_resource_set_implementation(r, &constraints_interface, NULL, NULL);
}
void aurora_constraints_commit(struct wl_resource* surface) {
    struct constraint* c; bool changed = false;
    wl_list_for_each(c, &constraints, link) if (c->surface == surface) {
        if (c->pending_set) { region_free(c->region); c->region = c->pending; c->pending = NULL; c->pending_set = false; changed = true; }
        if (c->pending_hint) { c->hint = true; c->hint_x = c->pending_x; c->hint_y = c->pending_y; c->pending_hint = false; }
    }
    if (changed) aurora_host_constraints_changed();
}
enum aurora_constraint_kind aurora_constraints_update(struct wl_resource* surface, int32_t x, int32_t y, bool focused) {
    if (active && (!surface || !focused || active->surface != surface)) constraint_deactivate();
    if (!active && surface && focused) {
        struct constraint* c;
        wl_list_for_each(c, &constraints, link)
            if (c->surface == surface && !c->defunct && (!c->region || aurora_region_contains(c->region, x, y))) {
                active = c; released.valid = false;
                if (c->kind == AURORA_CONSTRAINT_LOCK) zwp_locked_pointer_v1_send_locked(c->resource);
                else zwp_confined_pointer_v1_send_confined(c->resource);
                printf("[64] AURORA CONSTRAINT %s on x=%d y=%d\n", c->kind == AURORA_CONSTRAINT_LOCK ? "lock" : "confine", x, y); fflush(stdout);
                break;
            }
    }
    return active ? active->kind : AURORA_CONSTRAINT_NONE;
}
bool aurora_constraint_contains(int32_t x, int32_t y) {
    return !active || active->kind != AURORA_CONSTRAINT_CONFINE || !active->region || aurora_region_contains(active->region, x, y);
}
const struct rectangle* aurora_constraint_region(void) { return active && active->kind == AURORA_CONSTRAINT_CONFINE ? active->region : NULL; }
bool aurora_constraint_take_hint(struct wl_resource** surface, wl_fixed_t* x, wl_fixed_t* y) {
    if (!released.valid) return false;
    released.valid = false; *surface = released.surface; *x = released.x; *y = released.y; return true;
}

/* ---- wp_fractional_scale_manager_v1: one wp_fractional_scale_v1 per surface, told the surface's preferred scale ---- */
struct fractional { struct wl_list link; struct wl_resource* resource; struct wl_resource* surface; struct wl_listener surface_gone; int sent; };
static struct wl_list fractionals = {&fractionals, &fractionals};
static void fractional_surface_gone(struct wl_listener* listener, void* data) {
    (void)data; struct fractional* f = wl_container_of(listener, f, surface_gone); wl_list_remove(&f->surface_gone.link); f->surface = NULL;
}
static void fractional_destroy(struct wl_resource* resource) {
    struct fractional* f = wl_resource_get_user_data(resource); wl_list_remove(&f->link);
    if (f->surface) wl_list_remove(&f->surface_gone.link);
    free(f);
}
static const struct wp_fractional_scale_v1_interface fractional_interface = {destroy_request};
static void get_fractional_scale(struct wl_client* c, struct wl_resource* manager, uint32_t id, struct wl_resource* surface) {
    struct fractional* f;
    wl_list_for_each(f, &fractionals, link) if (f->surface == surface) {
        wl_resource_post_error(manager, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS, "the surface has a wp_fractional_scale_v1"); return; }
    int scale = aurora_host_surface_scale(surface);
    f = calloc(1, sizeof(*f)); struct wl_resource* r = wl_resource_create(c, &wp_fractional_scale_v1_interface, wl_resource_get_version(manager), id);
    if (!f || !r) { free(f); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    f->resource = r; f->surface = surface; f->surface_gone.notify = fractional_surface_gone;
    wl_resource_add_destroy_listener(surface, &f->surface_gone); wl_list_insert(&fractionals, &f->link);
    wl_resource_set_implementation(r, &fractional_interface, f, fractional_destroy);
    if (scale > 0) { wp_fractional_scale_v1_send_preferred_scale(r, (uint32_t)scale); f->sent = scale; }
}
static const struct wp_fractional_scale_manager_v1_interface fractional_manager_interface = {destroy_request, get_fractional_scale};
static void bind_fractional(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &wp_fractional_scale_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &fractional_manager_interface, NULL, NULL);
}
void aurora_protocols_preferred_scale(struct wl_resource* surface, int scale120) {
    struct fractional* f;
    wl_list_for_each(f, &fractionals, link) if (f->surface == surface && scale120 > 0 && f->sent != scale120) {
        wp_fractional_scale_v1_send_preferred_scale(f->resource, (uint32_t)scale120); f->sent = scale120; }
}
/* ---- xdg_activation_v1: tokens (single use, 30 s) for app-requested activation ---- */
struct token { struct wl_list link; char text[33]; struct wl_client* client; bool focused, issued; uint64_t at; struct wl_resource* resource; };
static struct wl_list tokens = {&tokens, &tokens};
static uint64_t seconds_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec; }
static void token_forget(struct token* t) { wl_list_remove(&t->link); free(t); }
static void token_resource_destroy(struct wl_resource* r) {
    struct token* t = wl_resource_get_user_data(r); if (!t) return; t->resource = NULL;
    if (!t->issued) token_forget(t);   /* never committed: nothing refers to it */
}
static void token_set_serial(struct wl_client* c, struct wl_resource* r, uint32_t serial, struct wl_resource* seat) { (void)c; (void)r; (void)serial; (void)seat; }
static void token_set_app_id(struct wl_client* c, struct wl_resource* r, const char* app_id) { (void)c; (void)r; (void)app_id; }
static void token_set_surface(struct wl_client* c, struct wl_resource* r, struct wl_resource* surface) { (void)c; (void)r; (void)surface; }
static void token_commit(struct wl_client* c, struct wl_resource* r) {
    struct token* t = wl_resource_get_user_data(r);
    if (t->issued) { wl_resource_post_error(r, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED, "token already committed"); return; }
    unsigned char bytes[16];
    if (getrandom(bytes, sizeof(bytes), 0) != (ssize_t)sizeof(bytes)) { wl_client_post_no_memory(c); return; }
    for (int i = 0; i < 16; i++) snprintf(t->text + 2 * i, 3, "%02x", bytes[i]);
    t->issued = true; t->at = seconds_now(); t->focused = aurora_host_client_focused(c);
    xdg_activation_token_v1_send_done(r, t->text);
}
static const struct xdg_activation_token_v1_interface token_interface = {token_set_serial, token_set_app_id, token_set_surface, token_commit, destroy_request};
static void get_activation_token(struct wl_client* c, struct wl_resource* manager, uint32_t id) {
    struct token* t = calloc(1, sizeof(*t)); struct wl_resource* r = wl_resource_create(c, &xdg_activation_token_v1_interface, wl_resource_get_version(manager), id);
    if (!t || !r) { free(t); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    struct token* old, *next; uint64_t now = seconds_now(); int count = 0;
    wl_list_for_each_safe(old, next, &tokens, link) if (old->issued && (now - old->at > 30 || ++count > 64)) { if (old->resource) wl_resource_set_user_data(old->resource, NULL); token_forget(old); }
    t->client = c; t->resource = r; wl_list_insert(&tokens, &t->link);
    wl_resource_set_implementation(r, &token_interface, t, token_resource_destroy);
}
static void activate(struct wl_client* c, struct wl_resource* manager, const char* text, struct wl_resource* surface) {
    (void)c; (void)manager; struct token* t;
    wl_list_for_each(t, &tokens, link) if (t->issued && !strcmp(t->text, text)) {
        bool fresh = seconds_now() - t->at <= 30; struct wl_client* requester = t->client; bool focused = t->focused;
        if (t->resource) wl_resource_set_user_data(t->resource, NULL);
        token_forget(t);   /* single use */
        if (fresh) aurora_host_activation(surface, requester, focused);
        return;
    }
}
static const struct xdg_activation_v1_interface activation_interface = {destroy_request, get_activation_token, activate};
static void bind_activation(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &xdg_activation_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &activation_interface, NULL, NULL);
}
bool aurora_protocols_init(struct wl_display* display) {
    wl_list_init(&decorations); wl_list_init(&relatives); wl_list_init(&constraints); wl_list_init(&xdg_outputs);
    return wl_global_create(display, &wp_viewporter_interface, 1, NULL, bind_viewporter) &&
           wl_global_create(display, &zxdg_output_manager_v1_interface, 3, NULL, bind_xdg_output) &&
           wl_global_create(display, &zxdg_decoration_manager_v1_interface, 1, NULL, bind_decoration) &&
           wl_global_create(display, &zwp_relative_pointer_manager_v1_interface, 1, NULL, bind_relative) &&
           wl_global_create(display, &zwp_pointer_constraints_v1_interface, 1, NULL, bind_constraints) &&
           wl_global_create(display, &wp_fractional_scale_manager_v1_interface, 1, NULL, bind_fractional) &&
           wl_global_create(display, &xdg_activation_v1_interface, 1, NULL, bind_activation);
}
