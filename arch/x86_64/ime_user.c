/* Input-method test pair for Aurora (tests/test_aurora_ime.py): "/bin/imtest app" (the desktop's child) and
 * "/bin/imtest im" (started by Aurora from ~/.aurora/input-method.conf over WAYLAND_SOCKET):
 *   the APP: an xdg toplevel "nexus.imtest" with zwp_text_input_v3 (cursor rectangle 40,60 2x20) that logs preedit,
 *     committed text and the wl_keyboard keys it gets; hostile: a 4050-byte surrounding text (over the 4000-byte limit: must be
 *     ignored); it must not see the input-method / virtual-keyboard globals, nor bind them by guessing their names;
 *   the IM: a toy transliterating input method on zwp_input_method_v2 with a keyboard grab, a candidate popup
 *     (160x32, colour 0x2E8B57) and zwp_virtual_keyboard_v1 for keys it does not use; letters build a preedit, space
 *     converts (ni -> 你, hao -> 好, namaste -> नमस्ते, zhongwen -> 中文), backspace edits it, anything else is forwarded.
 *     A second input method of the same client is "unavailable". After its second deactivate it asks for a popup on a
 *     subsurface: the protocol error "role" ends its connection, and Aurora restarts the program.
 * Log lines: [64] IMTEST ... ; failures: [64] IMTEST FAIL. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define LOG(...) do { printf("[64] IMTEST " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(x) do { if (!(x)) { printf("[64] IMTEST FAIL line=%d: %s\n", __LINE__, #x); fflush(stdout); exit(1); } } while (0)

struct globals { struct wl_compositor* compositor; struct wl_subcompositor* subcompositor; struct wl_shm* shm; struct wl_seat* seat; struct xdg_wm_base* wm;
                 struct zwp_text_input_manager_v3* ti; struct zwp_input_method_manager_v2* im; struct zwp_virtual_keyboard_manager_v1* vk; };
static void global(void* data, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    struct globals* g = data; (void)version;
    if (!strcmp(iface, "wl_compositor")) g->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_subcompositor")) g->subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, "wl_shm")) g->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_seat")) g->seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
    else if (!strcmp(iface, "xdg_wm_base")) g->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, "zwp_text_input_manager_v3")) g->ti = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
    else if (!strcmp(iface, "zwp_input_method_manager_v2")) g->im = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
    else if (!strcmp(iface, "zwp_virtual_keyboard_manager_v1")) g->vk = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void global_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d; (void)r; (void)n; }
static uint8_t listed[256];   /* global names the registry showed */
static void note_global(void* data, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (name < sizeof(listed)) listed[name] = 1;
    global(data, r, name, iface, version);
}
static const struct wl_registry_listener registry_listener = {note_global, global_remove};
static struct wl_display* connect_globals(struct globals* g) {
    struct wl_display* d = NULL;
    for (int i = 0; i < 100 && !(d = wl_display_connect(NULL)); i++) usleep(100000);
    CHECK(d); memset(g, 0, sizeof(*g));
    wl_registry_add_listener(wl_display_get_registry(d), &registry_listener, g); CHECK(wl_display_roundtrip(d) >= 0);
    CHECK(g->compositor && g->shm && g->seat); return d;
}
static struct wl_buffer* solid(struct wl_shm* shm, int w, int h, uint32_t argb) {
    size_t size = (size_t)w * h * 4; int fd = memfd_create("imtest", MFD_CLOEXEC); CHECK(fd >= 0 && !ftruncate(fd, (off_t)size));
    uint32_t* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(p != MAP_FAILED);
    for (size_t i = 0; i < size / 4; i++) p[i] = argb;
    munmap(p, size); struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)size); close(fd);
    struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888); wl_shm_pool_destroy(pool); return b;
}
static void tiny_keymap(struct zwp_virtual_keyboard_v1* vk) {   /* the compositor uses the session keymap; this one only satisfies the protocol */
    int fd = memfd_create("imtest-keymap", MFD_CLOEXEC); CHECK(fd >= 0 && write(fd, "x", 2) == 2);
    zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, 2); close(fd);
}

/* ---------------- the application ---------------- */
static struct { struct zwp_text_input_v3* ti; char text[256], preedit[64], pending_commit[64], pending_preedit[64]; bool has_commit, has_preedit; bool enabled;
                unsigned direct_letters; uint32_t serial; struct xdg_surface* xs; struct wl_surface* surface; struct wl_shm* shm; bool configured; } app;
static bool finished(void) { size_t n = strlen(app.text), k = strlen("中文"); return n >= k && !strcmp(app.text + n - k, "中文"); }
static void app_state(void) {   /* the field's state after every change */
    zwp_text_input_v3_set_surrounding_text(app.ti, app.text, (int32_t)strlen(app.text), (int32_t)strlen(app.text));
    zwp_text_input_v3_set_content_type(app.ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    zwp_text_input_v3_set_cursor_rectangle(app.ti, 40, 60, 2, 20); zwp_text_input_v3_commit(app.ti); app.serial++;
}
static void ti_enter(void* d, struct zwp_text_input_v3* t, struct wl_surface* s) {
    (void)d; (void)s; LOG("APP ENTER"); if (finished()) return;
    static char big[4051]; memset(big, 'x', 4050);   /* fits a 4096-byte Wayland message */
    zwp_text_input_v3_enable(t); zwp_text_input_v3_set_surrounding_text(t, big, 0, 0); zwp_text_input_v3_commit(t); app.serial++;   /* hostile: over the 4000-byte protocol limit */
    zwp_text_input_v3_enable(t); app_state(); app.enabled = true;
}
static void ti_leave(void* d, struct zwp_text_input_v3* t, struct wl_surface* s) {
    (void)d; (void)s; LOG("APP LEAVE"); if (app.enabled) { zwp_text_input_v3_disable(t); zwp_text_input_v3_commit(t); app.serial++; app.enabled = false; } }
static void ti_preedit(void* d, struct zwp_text_input_v3* t, const char* text, int32_t b, int32_t e) {
    (void)d; (void)t; (void)b; (void)e; snprintf(app.pending_preedit, sizeof(app.pending_preedit), "%s", text ? text : ""); app.has_preedit = true; }
static void ti_commit(void* d, struct zwp_text_input_v3* t, const char* text) {
    (void)d; (void)t; snprintf(app.pending_commit, sizeof(app.pending_commit), "%s", text ? text : ""); app.has_commit = true; }
static void ti_delete(void* d, struct zwp_text_input_v3* t, uint32_t before, uint32_t after) { (void)d; (void)t; LOG("APP DELETE %u %u", before, after); }
static void ti_done(void* d, struct zwp_text_input_v3* t, uint32_t serial) {
    (void)d; (void)t;
    if (app.has_preedit && strcmp(app.preedit, app.pending_preedit)) { snprintf(app.preedit, sizeof(app.preedit), "%s", app.pending_preedit); LOG("APP PREEDIT text=%s", app.preedit); }
    if (app.has_commit) {
        if (strlen(app.text) + strlen(app.pending_commit) < sizeof(app.text)) strcat(app.text, app.pending_commit);
        LOG("APP COMMIT text=%s serial=%u/%u", app.text, serial, app.serial);
        if (app.enabled) app_state();
        if (app.enabled && finished()) {   /* done: leave the field */
            zwp_text_input_v3_disable(app.ti); zwp_text_input_v3_commit(app.ti); app.serial++; app.enabled = false; LOG("APP DISABLED"); }
    }
    app.has_commit = app.has_preedit = false;
}
static const struct zwp_text_input_v3_listener ti_listener = {ti_enter, ti_leave, ti_preedit, ti_commit, ti_delete, ti_done};
static void kb_keymap(void* d, struct wl_keyboard* k, uint32_t f, int32_t fd, uint32_t s) { (void)d; (void)k; (void)f; (void)s; close(fd); }
static void kb_enter(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* sf, struct wl_array* a) { (void)d; (void)k; (void)s; (void)sf; (void)a; }
static void kb_leave(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* sf) { (void)d; (void)k; (void)s; (void)sf; }
static void kb_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)time;
    bool letter = (key >= 16 && key <= 25) || (key >= 30 && key <= 38) || (key >= 44 && key <= 50);
    if (letter && app.enabled) app.direct_letters++;
    LOG("APP KEY key=%u state=%u direct_letters=%u", key, state, app.direct_letters);
}
static void kb_mods(void* d, struct wl_keyboard* k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g; }
static void kb_repeat(void* d, struct wl_keyboard* k, int32_t r, int32_t de) { (void)d; (void)k; (void)r; (void)de; }
static const struct wl_keyboard_listener kb_listener = {kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat};
static void wm_ping(void* d, struct xdg_wm_base* w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_listener = {wm_ping};
static void xs_configure(void* d, struct xdg_surface* xs, uint32_t serial) {
    (void)d; xdg_surface_ack_configure(xs, serial);
    if (!app.configured) { app.configured = true; wl_surface_attach(app.surface, solid(app.shm, 480, 240, 0xFFF4F4F4), 0, 0); wl_surface_damage(app.surface, 0, 0, 480, 240); }
    wl_surface_commit(app.surface);
}
static const struct xdg_surface_listener xs_listener = {xs_configure};
static void tl_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void* d, struct xdg_toplevel* t) { (void)d; (void)t; exit(0); }
static const struct xdg_toplevel_listener tl_listener = {.configure = tl_configure, .close = tl_close};
static void probe_hidden(void) {   /* hostile: bind every unlisted global name as the input-method manager */
    unsigned last = 0, refused = 0; for (unsigned i = 0; i < sizeof(listed); i++) if (listed[i]) last = i;
    for (uint32_t name = 1; name <= last + 2; name++) {
        if (name < sizeof(listed) && listed[name]) continue;
        struct wl_display* d = wl_display_connect(NULL); CHECK(d);
        struct wl_registry* r = wl_display_get_registry(d);
        wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
        for (int i = 0; i < 3 && wl_display_roundtrip(d) >= 0; i++) {}   /* the bind must end the connection */
        CHECK(wl_display_get_error(d)); refused++; wl_display_disconnect(d);
    }
    LOG("APP HIDDEN-BIND refused=%u", refused);
}
static int run_app(void) {
    struct globals g; struct wl_display* d = connect_globals(&g); CHECK(g.wm && g.ti);
    if (g.im || g.vk) { LOG("FAIL an application sees the input-method globals"); return 1; }
    LOG("APP NO-IM-GLOBALS"); probe_hidden();
    app.shm = g.shm; xdg_wm_base_add_listener(g.wm, &wm_listener, NULL);
    wl_keyboard_add_listener(wl_seat_get_keyboard(g.seat), &kb_listener, NULL);
    app.surface = wl_compositor_create_surface(g.compositor); app.xs = xdg_wm_base_get_xdg_surface(g.wm, app.surface);
    xdg_surface_add_listener(app.xs, &xs_listener, NULL); struct xdg_toplevel* tl = xdg_surface_get_toplevel(app.xs);
    xdg_toplevel_add_listener(tl, &tl_listener, NULL); xdg_toplevel_set_title(tl, "IME test"); xdg_toplevel_set_app_id(tl, "nexus.imtest");
    app.ti = zwp_text_input_manager_v3_get_text_input(g.ti, g.seat); zwp_text_input_v3_add_listener(app.ti, &ti_listener, NULL);
    wl_surface_commit(app.surface);
    while (!app.configured) CHECK(wl_display_dispatch(d) >= 0);
    for (int i = 0; i < 20; i++) CHECK(wl_display_roundtrip(d) >= 0);
    LOG("APP READY");
    while (wl_display_dispatch(d) >= 0) {}
    LOG("APP display error %d", wl_display_get_error(d)); return 1;
}

/* ---------------- the input method ---------------- */
static struct { struct zwp_input_method_v2* im; struct zwp_virtual_keyboard_v1* vk; uint32_t dones; bool active, pending_active; char preedit[64];
                unsigned deactivations, keys_seen; size_t surrounding_max; struct wl_display* d; } ime;
static const struct { const char* latin; const char* text; } TABLE[] = {{"ni", "你"}, {"hao", "好"}, {"namaste", "नमस्ते"}, {"zhongwen", "中文"}};
static const char LETTERS[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";   /* evdev 16.. */
static void im_activate(void* d, struct zwp_input_method_v2* m) { (void)d; (void)m; ime.pending_active = true; }
static void im_deactivate(void* d, struct zwp_input_method_v2* m) { (void)d; (void)m; ime.pending_active = false; }
static void im_surrounding(void* d, struct zwp_input_method_v2* m, const char* text, uint32_t c, uint32_t a) {
    (void)d; (void)m; (void)c; (void)a; size_t n = strlen(text); if (n > ime.surrounding_max) ime.surrounding_max = n; }
static void im_cause(void* d, struct zwp_input_method_v2* m, uint32_t c) { (void)d; (void)m; (void)c; }
static void im_content(void* d, struct zwp_input_method_v2* m, uint32_t h, uint32_t p) { (void)d; (void)m; LOG("IM CONTENT hint=%u purpose=%u", h, p); }
static void im_done(void* d, struct zwp_input_method_v2* m) {
    (void)d; (void)m; ime.dones++;
    if (ime.pending_active != ime.active) {
        ime.active = ime.pending_active; ime.preedit[0] = 0;
        if (ime.active) LOG("IM ACTIVATE surrounding_max=%zu", ime.surrounding_max);
        else { ime.deactivations++; LOG("IM DEACTIVATE count=%u", ime.deactivations); }
    }
}
static void im_unavailable(void* d, struct zwp_input_method_v2* m) { (void)m; if (d) { *(bool*)d = true; return; } LOG("FAIL first input method unavailable"); exit(1); }
static const struct zwp_input_method_v2_listener im_listener = {im_activate, im_deactivate, im_surrounding, im_cause, im_content, im_done, im_unavailable};
static void im_send(const char* commit) {
    if (commit) zwp_input_method_v2_commit_string(ime.im, commit);
    zwp_input_method_v2_set_preedit_string(ime.im, ime.preedit, (int32_t)strlen(ime.preedit), (int32_t)strlen(ime.preedit));
    zwp_input_method_v2_commit(ime.im, ime.dones);
}
static uint8_t forwarded[0x300 / 8];   /* a release is forwarded only when its press was */
static void forward(uint32_t time, uint32_t key, uint32_t state) {
    if (key >= 0x300) return;
    uint8_t bit = (uint8_t)(1U << (key % 8));
    if (state) forwarded[key / 8] |= bit; else if (forwarded[key / 8] & bit) forwarded[key / 8] &= (uint8_t)~bit; else return;
    zwp_virtual_keyboard_v1_key(ime.vk, time, key, state); LOG("IM FORWARD key=%u state=%u", key, state);
}
static bool was_forwarded(uint32_t key) { return key < 0x300 && (forwarded[key / 8] & (1U << (key % 8))); }
static void gk_keymap(void* d, struct zwp_input_method_keyboard_grab_v2* g, uint32_t f, int32_t fd, uint32_t s) { (void)d; (void)g; (void)f; (void)s; close(fd); LOG("IM GRAB KEYMAP"); }
static void gk_key(void* d, struct zwp_input_method_keyboard_grab_v2* g, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)d; (void)g; (void)serial; ime.keys_seen++; LOG("IM KEY key=%u state=%u", key, state);
    char letter = key >= 16 && key < 16 + sizeof(LETTERS) - 1 ? LETTERS[key - 16] : 0;
    if (!ime.active || (!state && was_forwarded(key))) { forward(time, key, state); return; }
    if (letter) { if (state && strlen(ime.preedit) + 1 < sizeof(ime.preedit)) { size_t n = strlen(ime.preedit); ime.preedit[n] = letter; ime.preedit[n + 1] = 0; im_send(NULL); } return; }
    if (key == 57 && (ime.preedit[0] || !state)) {   /* space: convert (its release is ours) */
        if (!state) return;
        const char* out = ime.preedit; for (size_t i = 0; i < sizeof(TABLE) / sizeof(*TABLE); i++) if (!strcmp(TABLE[i].latin, ime.preedit)) out = TABLE[i].text;
        char text[64]; snprintf(text, sizeof(text), "%s", out); ime.preedit[0] = 0; im_send(text); LOG("IM CONVERT %s", text); return;
    }
    if (key == 14 && (ime.preedit[0] || !state)) { if (state) { ime.preedit[strlen(ime.preedit) - 1] = 0; im_send(NULL); } return; }
    forward(time, key, state);
}
static void gk_mods(void* d, struct zwp_input_method_keyboard_grab_v2* g, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t gr) {
    (void)d; (void)g; (void)s; zwp_virtual_keyboard_v1_modifiers(ime.vk, a, b, c, gr); }
static void gk_repeat(void* d, struct zwp_input_method_keyboard_grab_v2* g, int32_t r, int32_t de) { (void)d; (void)g; LOG("IM GRAB REPEAT %d %d", r, de); }
static const struct zwp_input_method_keyboard_grab_v2_listener gk_listener = {gk_keymap, gk_key, gk_mods, gk_repeat};
static void popup_rect(void* d, struct zwp_input_popup_surface_v2* p, int32_t x, int32_t y, int32_t w, int32_t h) { (void)d; (void)p; LOG("IM POPUP RECT %d %d %d %d", x, y, w, h); }
static const struct zwp_input_popup_surface_v2_listener popup_listener = {popup_rect};
static int run_im(void) {
    struct globals g; struct wl_display* d = connect_globals(&g); CHECK(g.im && g.vk && g.subcompositor); ime.d = d;
    ime.im = zwp_input_method_manager_v2_get_input_method(g.im, g.seat); zwp_input_method_v2_add_listener(ime.im, &im_listener, NULL);
    struct zwp_input_method_keyboard_grab_v2* grab = zwp_input_method_v2_grab_keyboard(ime.im); zwp_input_method_keyboard_grab_v2_add_listener(grab, &gk_listener, NULL);
    ime.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g.vk, g.seat); tiny_keymap(ime.vk);
    struct wl_surface* popup = wl_compositor_create_surface(g.compositor);
    struct zwp_input_popup_surface_v2* ps = zwp_input_method_v2_get_input_popup_surface(ime.im, popup); zwp_input_popup_surface_v2_add_listener(ps, &popup_listener, NULL);
    wl_surface_attach(popup, solid(g.shm, 160, 32, 0xFF2E8B57), 0, 0); wl_surface_damage(popup, 0, 0, 160, 32); wl_surface_commit(popup);
    bool unavailable = false; struct zwp_input_method_v2* second = zwp_input_method_manager_v2_get_input_method(g.im, g.seat);
    zwp_input_method_v2_add_listener(second, &im_listener, &unavailable);
    CHECK(wl_display_roundtrip(d) >= 0); CHECK(unavailable); LOG("IM READY second-unavailable");
    while (wl_display_dispatch(d) >= 0) {
        if (ime.deactivations < 2) continue;
        LOG("IM DONE keys_seen=%u surrounding_max=%zu", ime.keys_seen, ime.surrounding_max);
        struct wl_surface* parent = wl_compositor_create_surface(g.compositor), *child = wl_compositor_create_surface(g.compositor);
        wl_subcompositor_get_subsurface(g.subcompositor, child, parent);
        zwp_input_method_v2_get_input_popup_surface(second, child);   /* hostile: a popup on a surface with another role */
        while (wl_display_roundtrip(d) >= 0) {}
        const struct wl_interface* iface = NULL; uint32_t id = 0; int code = wl_display_get_protocol_error(d, &iface, &id);
        CHECK(iface == &zwp_input_method_v2_interface && code == ZWP_INPUT_METHOD_V2_ERROR_ROLE);
        LOG("IM ROLE-ERROR ok"); wl_display_disconnect(d); return 0;
    }
    LOG("FAIL IM display error %d", wl_display_get_error(d)); return 1;
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "im")) return run_im();
    if (argc == 2 && !strcmp(argv[1], "app")) return run_app();
    fprintf(stderr, "usage: imtest im|app\n"); return 2;
}
