/* Aurora input methods (aurora_ime.h): text-input-v3, input-method-v2, virtual-keyboard-v1. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server.h>
#include "aurora_ime.h"
#include "aurora_clipboard.h"
#include "aurora_xwm.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "input-method-unstable-v2-server-protocol.h"
#include "virtual-keyboard-unstable-v1-server-protocol.h"

struct ti_state { bool enabled; char* surrounding; uint32_t cursor, anchor, cause, hint, purpose; int32_t x, y, w, h; bool rect; };
struct text_input { struct wl_list link; struct wl_resource* resource; struct ti_state pending, current; uint32_t commits; struct wl_resource* entered; };
static struct wl_list inputs = {&inputs, &inputs};
static struct wl_resource* focused;          /* keyboard focus surface */
static uint8_t grabbed[0x300 / 8];          /* keys whose press went to the grab: their release goes there too */
static uint32_t sent_mods[4]; static bool mods_sent;
static struct {
    struct wl_resource* resource, *grab, *popup, *popup_surface; struct wl_listener popup_gone;
    bool active; char* commit; char* preedit; int32_t begin, end; uint32_t before, after; bool has_commit, has_preedit, has_delete;
} im;

static void destroy_request(struct wl_client* c, struct wl_resource* r) { (void)c; wl_resource_destroy(r); }
static void state_clear(struct ti_state* s) { free(s->surrounding); memset(s, 0, sizeof(*s)); }
static struct text_input* active_input(void) {
    struct text_input* t;
    wl_list_for_each(t, &inputs, link) if (focused && t->entered == focused && t->current.enabled) return t;
    return NULL;
}
/* aurora_clipboard.h: a password / PIN field, or one marked sensitive, has the keyboard */
bool aurora_ime_secret_focus(void) {
    struct text_input* t = active_input(); if (!t) return false;
    return t->current.purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD || t->current.purpose == ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN ||
           (t->current.hint & ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA);
}
static void im_done(void) { if (im.resource) zwp_input_method_v2_send_done(im.resource); }
/* The active text input's state to the input method (after a commit of the application or a focus change). */
static struct text_input* serving;          /* the text input the input method is active for */
static void im_deactivate(void) {
    if (!im.resource || !im.active) return;
    im.active = false; zwp_input_method_v2_send_deactivate(im.resource); im_done(); printf("[64] AURORA IME DEACTIVATE\n"); fflush(stdout);
}
static void im_update(void) {
    struct text_input* t = active_input();
    if (t != serving) im_deactivate();       /* another field: deactivate, then activate for it */
    serving = t; aurora_host_ime_redraw();
    if (!im.resource || !t) return;
    if (!im.active) { im.active = true; zwp_input_method_v2_send_activate(im.resource); printf("[64] AURORA IME ACTIVATE purpose=%u\n", t->current.purpose); fflush(stdout); }
    if (t->current.surrounding) zwp_input_method_v2_send_surrounding_text(im.resource, t->current.surrounding, t->current.cursor, t->current.anchor);
    zwp_input_method_v2_send_text_change_cause(im.resource, t->current.cause);
    zwp_input_method_v2_send_content_type(im.resource, t->current.hint, t->current.purpose);
    im_done();
    if (im.popup && t->current.rect) zwp_input_popup_surface_v2_send_text_input_rectangle(im.popup, t->current.x, t->current.y, t->current.w, t->current.h);
}

/* ---- zwp_text_input_v3 ---- */
static void ti_enable(struct wl_client* c, struct wl_resource* r) { (void)c; struct text_input* t = wl_resource_get_user_data(r); state_clear(&t->pending); t->pending.enabled = true; }
static void ti_disable(struct wl_client* c, struct wl_resource* r) { (void)c; struct text_input* t = wl_resource_get_user_data(r); state_clear(&t->pending); }
static void ti_surrounding(struct wl_client* c, struct wl_resource* r, const char* text, int32_t cursor, int32_t anchor) {
    (void)c; struct text_input* t = wl_resource_get_user_data(r); size_t n = strlen(text);
    if (n > 4000 || cursor < 0 || anchor < 0 || (size_t)cursor > n || (size_t)anchor > n) return;   /* the protocol caps it at 4000 bytes */
    free(t->pending.surrounding); t->pending.surrounding = strdup(text); t->pending.cursor = (uint32_t)cursor; t->pending.anchor = (uint32_t)anchor;
}
static void ti_cause(struct wl_client* c, struct wl_resource* r, uint32_t cause) { (void)c; ((struct text_input*)wl_resource_get_user_data(r))->pending.cause = cause; }
static void ti_content(struct wl_client* c, struct wl_resource* r, uint32_t hint, uint32_t purpose) {
    (void)c; struct text_input* t = wl_resource_get_user_data(r); t->pending.hint = hint; t->pending.purpose = purpose; }
static void ti_rect(struct wl_client* c, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; struct text_input* t = wl_resource_get_user_data(r); t->pending.x = x; t->pending.y = y; t->pending.w = w; t->pending.h = h; t->pending.rect = true; }
static void ti_commit(struct wl_client* c, struct wl_resource* r) {
    (void)c; struct text_input* t = wl_resource_get_user_data(r);
    free(t->current.surrounding); t->current = t->pending; t->current.surrounding = t->pending.surrounding ? strdup(t->pending.surrounding) : NULL; t->commits++;
    if (t->entered && t->entered == focused) im_update();
}
static const struct zwp_text_input_v3_interface ti_interface = {destroy_request, ti_enable, ti_disable, ti_surrounding, ti_cause, ti_content, ti_rect, ti_commit};
static void ti_destroy(struct wl_resource* r) {
    struct text_input* t = wl_resource_get_user_data(r);
    if (serving == t) { serving = NULL; im_deactivate(); }
    wl_list_remove(&t->link); state_clear(&t->pending); state_clear(&t->current); free(t);
    im_update();
}
static void get_text_input(struct wl_client* c, struct wl_resource* manager, uint32_t id, struct wl_resource* seat) {
    (void)seat; struct text_input* t = calloc(1, sizeof(*t)); struct wl_resource* r = wl_resource_create(c, &zwp_text_input_v3_interface, wl_resource_get_version(manager), id);
    if (!t || !r) { free(t); if (r) wl_resource_destroy(r); wl_resource_post_no_memory(manager); return; }
    t->resource = r; wl_list_insert(&inputs, &t->link); wl_resource_set_implementation(r, &ti_interface, t, ti_destroy);
    if (focused && wl_resource_get_client(focused) == c) { t->entered = focused; zwp_text_input_v3_send_enter(r, focused); }
}
static const struct zwp_text_input_manager_v3_interface ti_manager_interface = {destroy_request, get_text_input};
static void bind_ti_manager(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; } wl_resource_set_implementation(r, &ti_manager_interface, NULL, NULL);
}

/* ---- zwp_input_method_v2 ---- */
static void im_commit_string(struct wl_client* c, struct wl_resource* r, const char* text) {
    (void)c; if (r != im.resource) return; free(im.commit); im.commit = strdup(text); im.has_commit = true; }
static void im_preedit(struct wl_client* c, struct wl_resource* r, const char* text, int32_t begin, int32_t end) {
    (void)c; if (r != im.resource) return; free(im.preedit); im.preedit = strdup(text); im.begin = begin; im.end = end; im.has_preedit = true; }
static void im_delete(struct wl_client* c, struct wl_resource* r, uint32_t before, uint32_t after) {
    (void)c; if (r != im.resource) return; im.before = before; im.after = after; im.has_delete = true; }
static void im_commit(struct wl_client* c, struct wl_resource* r, uint32_t serial) {
    (void)c; (void)serial; if (r != im.resource) return;
    struct text_input* t = active_input();
    if (t) {   /* text-input-v3 order: (preedit replaced) delete, commit, new preedit, done */
        if (im.has_delete) zwp_text_input_v3_send_delete_surrounding_text(t->resource, im.before, im.after);
        if (im.has_commit) zwp_text_input_v3_send_commit_string(t->resource, im.commit);
        zwp_text_input_v3_send_preedit_string(t->resource, im.has_preedit ? im.preedit : NULL, im.has_preedit ? im.begin : 0, im.has_preedit ? im.end : 0);
        zwp_text_input_v3_send_done(t->resource, t->commits);
        printf("[64] AURORA IME COMMIT commit_bytes=%zu preedit_bytes=%zu delete=%u,%u\n", im.has_commit ? strlen(im.commit) : 0, im.has_preedit ? strlen(im.preedit) : 0,
               im.has_delete ? im.before : 0, im.has_delete ? im.after : 0);   /* sizes only: typed text stays out of the log */
        fflush(stdout);
    }
    free(im.commit); free(im.preedit); im.commit = im.preedit = NULL; im.has_commit = im.has_preedit = im.has_delete = false;
}
static void popup_surface_gone(struct wl_listener* l, void* data) {
    (void)l; (void)data; wl_list_remove(&im.popup_gone.link); im.popup_surface = NULL; aurora_host_ime_redraw(); }
static void popup_destroy(struct wl_resource* r) {
    if (r != im.popup) return;
    im.popup = NULL; if (im.popup_surface) { wl_list_remove(&im.popup_gone.link); im.popup_surface = NULL; }
    aurora_host_ime_redraw();
}
static const struct zwp_input_popup_surface_v2_interface popup_interface = {destroy_request};
static void im_popup(struct wl_client* c, struct wl_resource* r, uint32_t id, struct wl_resource* surface) {
    struct wl_resource* p = wl_resource_create(c, &zwp_input_popup_surface_v2_interface, wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(p, &popup_interface, NULL, popup_destroy);
    if (!aurora_host_claim_ime_popup(surface)) { wl_resource_post_error(r, ZWP_INPUT_METHOD_V2_ERROR_ROLE, "wl_surface has another role"); return; }
    if (r != im.resource || im.popup) return;   /* an inactive input method, or a second popup: inert */
    im.popup = p; im.popup_surface = surface; im.popup_gone.notify = popup_surface_gone; wl_resource_add_destroy_listener(surface, &im.popup_gone);
    struct text_input* t = active_input();
    if (t && t->current.rect) zwp_input_popup_surface_v2_send_text_input_rectangle(p, t->current.x, t->current.y, t->current.w, t->current.h);
}
static void grab_destroy(struct wl_resource* r) { if (im.grab == r) { im.grab = NULL; memset(grabbed, 0, sizeof(grabbed)); printf("[64] AURORA IME GRAB released\n"); fflush(stdout); } }
static void grab_release(struct wl_client* c, struct wl_resource* r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_input_method_keyboard_grab_v2_interface grab_interface = {grab_release};
static void im_grab(struct wl_client* c, struct wl_resource* r, uint32_t id) {
    struct wl_resource* g = wl_resource_create(c, &zwp_input_method_keyboard_grab_v2_interface, wl_resource_get_version(r), id);
    if (!g) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(g, &grab_interface, NULL, grab_destroy);
    if (r != im.resource || im.grab) return;
    im.grab = g; mods_sent = false; int fd; uint32_t size;
    if (aurora_host_keymap_fd(&fd, &size)) zwp_input_method_keyboard_grab_v2_send_keymap(g, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
    zwp_input_method_keyboard_grab_v2_send_repeat_info(g, 25, 500);
    printf("[64] AURORA IME GRAB keyboard\n"); fflush(stdout);
}
static void im_destroy_request(struct wl_client* c, struct wl_resource* r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_input_method_v2_interface im_interface = {im_commit_string, im_preedit, im_delete, im_commit, im_popup, im_grab, im_destroy_request};
static void im_resource_destroy(struct wl_resource* r) {
    if (r != im.resource) return;
    if (im.grab) wl_resource_set_destructor(im.grab, NULL);
    memset(grabbed, 0, sizeof(grabbed));
    free(im.commit); free(im.preedit);
    if (im.popup_surface) wl_list_remove(&im.popup_gone.link);
    memset(&im, 0, sizeof(im));
    struct text_input* t = active_input();
    if (t) { zwp_text_input_v3_send_preedit_string(t->resource, NULL, 0, 0); zwp_text_input_v3_send_done(t->resource, t->commits); }   /* no stale preedit */
    printf("[64] AURORA IME GONE\n"); fflush(stdout); aurora_host_ime_redraw();
}
static void get_input_method(struct wl_client* c, struct wl_resource* manager, struct wl_resource* seat, uint32_t id) {
    (void)seat; struct wl_resource* r = wl_resource_create(c, &zwp_input_method_v2_interface, wl_resource_get_version(manager), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &im_interface, NULL, im_resource_destroy);
    if (im.resource) { zwp_input_method_v2_send_unavailable(r); return; }   /* one input method at a time */
    im.resource = r; printf("[64] AURORA IME REGISTERED\n"); fflush(stdout);
    im_update();
}
static const struct zwp_input_method_manager_v2_interface im_manager_interface = {get_input_method, destroy_request};
static void bind_im_manager(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &zwp_input_method_manager_v2_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; } wl_resource_set_implementation(r, &im_manager_interface, NULL, NULL);
}

/* ---- zwp_virtual_keyboard_v1 (the input method's client only) ---- */
static void vk_keymap(struct wl_client* c, struct wl_resource* r, uint32_t format, int32_t fd, uint32_t size) {
    (void)c; (void)format; (void)size; close(fd); wl_resource_set_user_data(r, (void*)1);   /* keys are interpreted with the session keymap */
}
static void vk_key(struct wl_client* c, struct wl_resource* r, uint32_t time, uint32_t key, uint32_t state) {
    if (!wl_resource_get_user_data(r)) { wl_resource_post_error(r, ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, "no keymap"); return; }
    if (key > 0x2ff || state > 1 || !im.resource || wl_resource_get_client(im.resource) != c) return;   /* the input method went away */
    aurora_host_inject_key(time, key, state);
}
static void vk_modifiers(struct wl_client* c, struct wl_resource* r, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    if (!wl_resource_get_user_data(r)) { wl_resource_post_error(r, ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, "no keymap"); return; }
    if (!im.resource || wl_resource_get_client(im.resource) != c) return;
    aurora_host_inject_modifiers(depressed, latched, locked, group);
}
static const struct zwp_virtual_keyboard_v1_interface vk_interface = {vk_keymap, vk_key, vk_modifiers, destroy_request};
static void create_vk(struct wl_client* c, struct wl_resource* manager, struct wl_resource* seat, uint32_t id) {
    (void)seat;   /* any client may create one, but its keys reach applications only while its client is the input method */
    struct wl_resource* r = wl_resource_create(c, &zwp_virtual_keyboard_v1_interface, wl_resource_get_version(manager), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &vk_interface, NULL, NULL);
}
static const struct zwp_virtual_keyboard_manager_v1_interface vk_manager_interface = {create_vk};
static void bind_vk_manager(struct wl_client* c, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(c, &zwp_virtual_keyboard_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; } wl_resource_set_implementation(r, &vk_manager_interface, NULL, NULL);
}

/* ---- desktop.c entry points ---- */
/* ---- the input-method program: Aurora starts it over a socketpair (WAYLAND_SOCKET), and only that client sees the
 * input-method and virtual-keyboard globals, so no other application can grab the keyboard or inject keys. ---- */
static struct wl_display* ime_display; static struct wl_global *im_global, *vk_global;
static struct wl_client* trusted; static struct wl_listener trusted_gone; static unsigned starts; static bool restarts_off;
static bool global_filter(const struct wl_client* c, const struct wl_global* g, void* data) {
    (void)data; if (!aurora_xwm_global_visible(c, g)) return false;   /* xwayland_shell_v1: Aurora's Xwayland only */
    return (g != im_global && g != vk_global) || (trusted && c == trusted);
}
static void start_program(void* data);
static void trusted_destroyed(struct wl_listener* l, void* data) {
    (void)l; (void)data; trusted = NULL; printf("[64] AURORA IME PROGRAM GONE starts=%u\n", starts); fflush(stdout);
    if (starts < AURORA_IME_RESTARTS && !restarts_off) wl_event_loop_add_idle(wl_display_get_event_loop(ime_display), start_program, NULL);
}
void aurora_ime_restarts(bool allowed) {
    restarts_off = !allowed;
    if (allowed && ime_display && !trusted && starts && starts < AURORA_IME_RESTARTS) wl_event_loop_add_idle(wl_display_get_event_loop(ime_display), start_program, NULL);
}
static void start_program(void* data) {
    (void)data; char line[512], *argv[9], *save = NULL; int argc = 0; FILE* f = fopen(AURORA_IME_CONF, "re");
    if (!f || trusted || restarts_off) { if (f) fclose(f); return; }
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) found = !strncmp(line, "program=", 8);
    fclose(f); if (!found) return;
    for (char* t = strtok_r(line + 8, " \t\r\n", &save); t && argc < 8; t = strtok_r(NULL, " \t\r\n", &save)) argv[argc++] = t;
    argv[argc] = NULL;
    if (!argc || argv[0][0] != '/' || access(argv[0], X_OK)) { printf("[64] AURORA IME PROGRAM invalid in %s\n", AURORA_IME_CONF); fflush(stdout); return; }
    extern char** environ; size_t n = 0; while (environ[n]) n++;
    char** env = calloc(n + 2, sizeof(*env)); int sv[2];
    if (!env || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { free(env); return; }
    size_t k = 0; for (size_t i = 0; i < n; i++) if (strncmp(environ[i], "WAYLAND_SOCKET=", 15)) env[k++] = environ[i];
    env[k++] = "WAYLAND_SOCKET=3"; env[k] = NULL;
    starts++; pid_t pid = fork();
    if (!pid) { if (dup2(sv[1], 3) < 0 || fcntl(3, F_SETFD, 0) < 0) _exit(127); execve(argv[0], argv, env); _exit(127); }
    free(env); close(sv[1]);
    if (pid < 0 || !(trusted = wl_client_create(ime_display, sv[0]))) { close(sv[0]); if (pid > 0) kill(pid, SIGTERM); return; }
    trusted_gone.notify = trusted_destroyed; wl_client_add_destroy_listener(trusted, &trusted_gone);
    printf("[64] AURORA IME PROGRAM %s pid=%ld start=%u\n", argv[0], (long)pid, starts); fflush(stdout);
}
bool aurora_ime_init(struct wl_display* display) {
    ime_display = display;
    im_global = wl_global_create(display, &zwp_input_method_manager_v2_interface, 1, NULL, bind_im_manager);
    vk_global = wl_global_create(display, &zwp_virtual_keyboard_manager_v1_interface, 1, NULL, bind_vk_manager);
    if (!im_global || !vk_global || !wl_global_create(display, &zwp_text_input_manager_v3_interface, 1, NULL, bind_ti_manager)) return false;
    wl_display_set_global_filter(display, global_filter, NULL);
    return wl_event_loop_add_idle(wl_display_get_event_loop(display), start_program, NULL) != NULL;   /* once the session is set up */
}
void aurora_ime_focus(struct wl_resource* surface) {
    if (surface == focused) return;
    struct text_input* t;
    wl_list_for_each(t, &inputs, link) if (t->entered) { zwp_text_input_v3_send_leave(t->resource, t->entered); t->entered = NULL; }
    focused = surface;
    if (surface) wl_list_for_each(t, &inputs, link) if (wl_resource_get_client(t->resource) == wl_resource_get_client(surface)) {
        t->entered = surface; zwp_text_input_v3_send_enter(t->resource, surface); }
    im_update();
}
bool aurora_ime_key(uint32_t time, uint32_t key, uint32_t state) {
    if (!im.grab || key >= 0x300) return false;
    uint8_t bit = (uint8_t)(1U << (key % 8));
    if (state) { if (!im.active) return false; grabbed[key / 8] |= bit; }
    else { if (!(grabbed[key / 8] & bit)) return false; grabbed[key / 8] &= (uint8_t)~bit; }   /* pressed before the grab: the application's */
    zwp_input_method_keyboard_grab_v2_send_key(im.grab, wl_display_next_serial(wl_client_get_display(wl_resource_get_client(im.grab))), time, key, state);
    return true;
}
void aurora_ime_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    uint32_t now[4] = {depressed, latched, locked, group};
    if (!im.grab || (mods_sent && !memcmp(now, sent_mods, sizeof(now)))) return;
    memcpy(sent_mods, now, sizeof(now)); mods_sent = true;
    zwp_input_method_keyboard_grab_v2_send_modifiers(im.grab, wl_display_next_serial(wl_client_get_display(wl_resource_get_client(im.grab))), depressed, latched, locked, group);
}
void aurora_ime_keymap_changed(void) {
    int fd; uint32_t size;
    if (im.grab && aurora_host_keymap_fd(&fd, &size)) zwp_input_method_keyboard_grab_v2_send_keymap(im.grab, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
}
struct wl_resource* aurora_ime_popup(int* x, int* y) {
    struct text_input* t = active_input(); int ox, oy;
    if (!im.popup || !im.popup_surface || !t || !aurora_host_surface_origin(t->entered, &ox, &oy)) return NULL;
    int64_t px = (int64_t)ox + (t->current.rect ? t->current.x : 0), py = (int64_t)oy + (t->current.rect ? (int64_t)t->current.y + t->current.h : 0);
    const int64_t limit = 1 << 20;   /* client rectangles are arbitrary int32: no overflow, the desktop clamps further */
    *x = (int)(px < -limit ? -limit : px > limit ? limit : px); *y = (int)(py < -limit ? -limit : py > limit ? limit : py);
    return im.popup_surface;
}
