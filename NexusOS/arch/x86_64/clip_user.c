/* Clipboard history test client (tests/test_aurora_clipboard.py): "cliptest script [SECONDS]" maps a window and, once
 * it has the keyboard, copies "first copied text", "second copied text" and then "hunter2-secret" offered with the
 * x-kde-passwordManagerHint type (a password manager's copy, which the history must not keep), prints Aurora's
 * history (aurora-app ctl clipboard-history) and then reports every clipboard it is offered while focused
 * ("[64] CLIPTEST selection: TEXT"). Lines "[64] CLIPTEST ..." report every step. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

#define SAY(...) do { printf("[64] CLIPTEST " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(test) do { if (!(test)) { SAY("FAIL line=%d errno=%d", __LINE__, errno); exit(1); } } while (0)
static struct wl_display* display;
static struct wl_compositor* compositor; static struct wl_shm* shm; static struct xdg_wm_base* shell; static struct wl_seat* seat;
static struct wl_data_device_manager* manager; static struct wl_data_device* device; static struct wl_keyboard* keyboard;
static struct wl_surface* surface; static bool configured, focused; static uint32_t focus_serial;
static void global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    (void)d;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) shell = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (!strcmp(iface, "wl_data_device_manager")) manager = wl_registry_bind(r, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
}
static void global_remove(void* d, struct wl_registry* r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void ping(void* d, struct xdg_wm_base* s, uint32_t serial) { (void)d; xdg_wm_base_pong(s, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void xdg_configure(void* d, struct xdg_surface* x, uint32_t serial) { (void)d; xdg_surface_ack_configure(x, serial); configured = true; }
static const struct xdg_surface_listener xdg_listener = {xdg_configure};
static void top_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void top_close(void* d, struct xdg_toplevel* t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure, .close = top_close};
static struct wl_buffer* buffer(int w, int h, uint32_t color) {
    int fd = memfd_create("clip", MFD_CLOEXEC); CHECK(fd >= 0 && !ftruncate(fd, w * h * 4));
    uint32_t* p = mmap(NULL, (size_t)w * h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(p != MAP_FAILED);
    for (int i = 0; i < w * h; i++) p[i] = color;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, w * h * 4); struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool); close(fd); return b;
}
/* ---- keyboard: focus and a serial for set_selection ---- */
static void kb_keymap(void* d, struct wl_keyboard* k, uint32_t f, int32_t fd, uint32_t size) { (void)d; (void)k; (void)f; (void)size; close(fd); }
static void kb_enter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys) { (void)d; (void)k; (void)s; (void)keys; focused = true; focus_serial = serial; SAY("focus in"); }
static void kb_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s) { (void)d; (void)k; (void)serial; (void)s; focused = false; SAY("focus out"); }
static void kb_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) { (void)d; (void)k; (void)t; (void)key; if (state) focus_serial = serial; }
static void kb_mods(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g; }
static void kb_repeat(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static const struct wl_keyboard_listener keyboard_listener = {kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat};
/* ---- sources ---- */
struct copy { struct wl_data_source* source; const char* text; };
static void src_target(void* d, struct wl_data_source* s, const char* mime) { (void)d; (void)s; (void)mime; }
static void src_send(void* d, struct wl_data_source* s, const char* mime, int32_t fd) {
    (void)s; struct copy* c = d; ssize_t n = write(fd, c->text, strlen(c->text)); close(fd); SAY("send %s %s wrote=%zd", mime, c->text, n);
}
static void src_cancelled(void* d, struct wl_data_source* s) { struct copy* c = d; SAY("source cancelled (%s)", c->text); wl_data_source_destroy(s); c->source = NULL; }
static const struct wl_data_source_listener source_listener = {.target = src_target, .send = src_send, .cancelled = src_cancelled};
static void copy(struct copy* c, bool secret) {
    c->source = wl_data_device_manager_create_data_source(manager); wl_data_source_add_listener(c->source, &source_listener, c);
    wl_data_source_offer(c->source, "text/plain;charset=utf-8"); wl_data_source_offer(c->source, "text/plain");
    if (secret) wl_data_source_offer(c->source, "x-kde-passwordManagerHint");
    wl_data_device_set_selection(device, c->source, focus_serial); SAY("copied %s%s", c->text, secret ? " (password manager hint)" : "");
}
/* ---- the offered selection ---- */
static struct wl_data_offer* pending; static bool pending_text;
static void offer_offer(void* d, struct wl_data_offer* o, const char* mime) { (void)d; (void)o; if (!strcmp(mime, "text/plain;charset=utf-8")) pending_text = true; }
static const struct wl_data_offer_listener offer_listener = {.offer = offer_offer};
static void dd_data_offer(void* d, struct wl_data_device* dev, struct wl_data_offer* o) { (void)d; (void)dev; pending_text = false; wl_data_offer_add_listener(o, &offer_listener, NULL); }
static void dd_enter(void* d, struct wl_data_device* dev, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o) { (void)d; (void)dev; (void)serial; (void)s; (void)x; (void)y; (void)o; }
static void dd_leave(void* d, struct wl_data_device* dev) { (void)d; (void)dev; }
static void dd_motion(void* d, struct wl_data_device* dev, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dev; (void)t; (void)x; (void)y; }
static void dd_drop(void* d, struct wl_data_device* dev) { (void)d; (void)dev; }
static void dd_selection(void* d, struct wl_data_device* dev, struct wl_data_offer* o) {
    (void)d; (void)dev; if (pending && pending != o) wl_data_offer_destroy(pending);
    pending = o; if (!o) SAY("selection: (none)");
}
static const struct wl_data_device_listener device_listener = {.data_offer = dd_data_offer, .enter = dd_enter, .leave = dd_leave, .motion = dd_motion, .drop = dd_drop, .selection = dd_selection};
static void read_pending(void) {   /* the offer's text: it may be our own source, so keep dispatching while reading */
    if (!pending || !pending_text) return;
    struct wl_data_offer* o = pending; pending = NULL;
    int p[2]; CHECK(!pipe2(p, O_CLOEXEC)); wl_data_offer_receive(o, "text/plain;charset=utf-8", p[1]); close(p[1]);
    char text[512] = ""; size_t used = 0; struct pollfd pf[2] = {{p[0], POLLIN, 0}, {wl_display_get_fd(display), POLLIN, 0}};
    for (int i = 0; i < 100 && used < sizeof(text) - 1; i++) {
        wl_display_flush(display);
        if (poll(pf, 2, 100) <= 0) continue;
        if (pf[1].revents & POLLIN) wl_display_dispatch(display);
        if (pf[0].revents & (POLLIN | POLLHUP)) { ssize_t n = read(p[0], text + used, sizeof(text) - 1 - used); if (n <= 0) break; used += (size_t)n; }
    }
    close(p[0]); text[used] = 0; wl_data_offer_destroy(o); SAY("selection: %s", text);
}
static void history(void) {   /* Aurora's view of the history, through its control socket */
    int p[2]; CHECK(!pipe2(p, O_CLOEXEC)); pid_t pid = fork();
    if (!pid) { dup2(p[1], 1); dup2(p[1], 2); execl("/bin/aurora-app", "aurora-app", "ctl", "clipboard-history", (char*)NULL); _exit(127); }
    close(p[1]); char out[4096]; size_t used = 0; ssize_t n;
    while (used < sizeof(out) - 1 && (n = read(p[0], out + used, sizeof(out) - 1 - used)) > 0) used += (size_t)n;
    close(p[0]); out[used] = 0; int status; waitpid(pid, &status, 0);
    for (char* save = NULL, *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) SAY("history %s", line);
}
static void pump(int ms) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); uint64_t end = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000 + (uint64_t)ms;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t); uint64_t now = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; if (now >= end) return;
        CHECK(wl_display_dispatch_pending(display) >= 0); wl_display_flush(display);
        struct pollfd pf = {wl_display_get_fd(display), POLLIN, 0};
        if (poll(&pf, 1, (int)(end - now < 200 ? end - now : 200)) > 0) CHECK(wl_display_dispatch(display) >= 0);
        if (focused) read_pending();
    }
}
int main(int argc, char** argv) {
    int seconds = argc > 2 ? atoi(argv[2]) : 300;
    if (argc < 2 || strcmp(argv[1], "script")) { fprintf(stderr, "usage: cliptest script [SECONDS]\n"); return 2; }
    display = wl_display_connect(NULL); CHECK(display);
    struct wl_registry* registry = wl_display_get_registry(display); wl_registry_add_listener(registry, &registry_listener, NULL);
    CHECK(wl_display_roundtrip(display) >= 0 && compositor && shm && shell && seat && manager);
    xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    device = wl_data_device_manager_get_data_device(manager, seat); wl_data_device_add_listener(device, &device_listener, NULL);
    keyboard = wl_seat_get_keyboard(seat); wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(shell, surface); xdg_surface_add_listener(xdg, &xdg_listener, NULL);
    struct xdg_toplevel* top = xdg_surface_get_toplevel(xdg); xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_app_id(top, "nexus.cliptest"); xdg_toplevel_set_title(top, "Clipboard test");
    wl_surface_commit(surface); while (!configured) CHECK(wl_display_dispatch(display) >= 0);
    wl_surface_attach(surface, buffer(400, 300, 0xFF8040A0), 0, 0); wl_surface_damage(surface, 0, 0, 400, 300); wl_surface_commit(surface);
    SAY("ready");
    for (int i = 0; i < 300 && !focused; i++) pump(100);
    CHECK(focused);
    static struct copy first = {NULL, "first copied text"}, second = {NULL, "second copied text"}, secret = {NULL, "hunter2-secret"};
    copy(&first, false); pump(1500); copy(&second, false); pump(1500); copy(&secret, true); pump(1500);
    history(); SAY("script done");
    pump(seconds * 1000);
    SAY("done"); return 0;
}
