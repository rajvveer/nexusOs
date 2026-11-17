/* Drag-and-drop test pair (tests/test_aurora_dnd.py): "dndtest pair" starts a target window (child) and a source
 * window (this process). The source offers text/plain (copy|move) when its window is pressed, with a drag icon; the
 * target accepts text/plain (copy|move, prefers copy), receives the data on drop and finishes. The source first tries
 * a drag with a stale serial (must be cancelled). Lines "[64] DNDTEST ..." report every event. */
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

#define SAY(...) do { printf("[64] DNDTEST " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(test) do { if (!(test)) { SAY("FAIL line=%d errno=%d", __LINE__, errno); exit(1); } } while (0)
static struct wl_compositor* compositor; static struct wl_shm* shm; static struct xdg_wm_base* shell; static struct wl_seat* seat;
static struct wl_data_device_manager* manager; static struct wl_data_device* device; static struct wl_pointer* pointer;
static struct wl_surface* surface; static bool configured, target_role; static int drags;
static void global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    (void)d;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) shell = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (!strcmp(iface, "wl_data_device_manager")) { SAY("manager version %u", version); manager = wl_registry_bind(r, name, &wl_data_device_manager_interface, version < 3 ? version : 3); }
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
    int fd = memfd_create("dnd", MFD_CLOEXEC); CHECK(fd >= 0 && !ftruncate(fd, w * h * 4));
    uint32_t* p = mmap(NULL, (size_t)w * h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(p != MAP_FAILED);
    for (int i = 0; i < w * h; i++) p[i] = color;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, w * h * 4); struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool); close(fd); return b;
}
/* ---- source ---- */
static struct wl_data_source *source, *stale;
static void src_target(void* d, struct wl_data_source* s, const char* mime) { (void)d; (void)s; SAY("SOURCE target %s", mime ? mime : "(none)"); }
static void src_send(void* d, struct wl_data_source* s, const char* mime, int32_t fd) {
    (void)d; (void)s; const char text[] = "dragged text 42"; ssize_t n = write(fd, text, strlen(text)); close(fd); SAY("SOURCE send %s wrote=%zd", mime, n);
}
static void src_cancelled(void* d, struct wl_data_source* s) {
    (void)d; SAY("SOURCE %s cancelled", s == stale ? "stale-serial" : "drag");
    wl_data_source_destroy(s); if (s == source) source = NULL; if (s == stale) stale = NULL;
}
static void src_performed(void* d, struct wl_data_source* s) { (void)d; (void)s; SAY("SOURCE drop performed"); }
static void src_finished(void* d, struct wl_data_source* s) { (void)d; SAY("SOURCE finished"); wl_data_source_destroy(s); source = NULL; }
static void src_action(void* d, struct wl_data_source* s, uint32_t a) { (void)d; (void)s; SAY("SOURCE action %u", a); }
static const struct wl_data_source_listener source_listener = {.target = src_target, .send = src_send, .cancelled = src_cancelled,
    .dnd_drop_performed = src_performed, .dnd_finished = src_finished, .action = src_action};
static struct wl_data_source* new_source(void) {
    struct wl_data_source* s = wl_data_device_manager_create_data_source(manager); wl_data_source_add_listener(s, &source_listener, NULL);
    wl_data_source_offer(s, "text/plain;charset=utf-8"); wl_data_source_offer(s, "text/plain");
    wl_data_source_set_actions(s, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE); return s;
}
static struct wl_surface* icon;
static void pointer_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)serial; (void)s; (void)x; (void)y; }
static void pointer_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) { (void)d; (void)p; (void)serial; (void)s; if (!target_role) SAY("SOURCE pointer leave"); }
static void pointer_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void pointer_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)t;
    if (target_role || button != 272 || state != WL_POINTER_BUTTON_STATE_PRESSED) return;
    if (!drags++) { stale = new_source(); wl_data_device_start_drag(device, stale, surface, NULL, serial + 1000); SAY("SOURCE stale-serial drag requested"); }
    if (source) wl_data_source_destroy(source);
    source = new_source();
    if (!icon) { icon = wl_compositor_create_surface(compositor); }
    wl_surface_attach(icon, buffer(32, 32, 0xFFE05020), 0, 0); wl_surface_damage(icon, 0, 0, 32, 32); wl_surface_commit(icon);
    wl_data_device_start_drag(device, source, surface, icon, serial); SAY("SOURCE drag %d started", drags);
}
static void pointer_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void pointer_frame(void* d, struct wl_pointer* p) { (void)d; (void)p; }
static void pointer_axis_source(void* d, struct wl_pointer* p, uint32_t s) { (void)d; (void)p; (void)s; }
static void pointer_axis_stop(void* d, struct wl_pointer* p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void pointer_axis_discrete(void* d, struct wl_pointer* p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener pointer_listener = {.enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion, .button = pointer_button, .axis = pointer_axis,
    .frame = pointer_frame, .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop, .axis_discrete = pointer_axis_discrete};
/* ---- target ---- */
static struct wl_data_offer* offer; static bool has_text; static int motions; static uint32_t drop_action;
static void offer_offer(void* d, struct wl_data_offer* o, const char* mime) { (void)d; (void)o; if (!strcmp(mime, "text/plain;charset=utf-8")) has_text = true; }
static void offer_source_actions(void* d, struct wl_data_offer* o, uint32_t a) { (void)d; (void)o; SAY("TARGET source actions %u", a); }
static void offer_action(void* d, struct wl_data_offer* o, uint32_t a) { (void)d; (void)o; drop_action = a; SAY("TARGET action %u", a); }
static const struct wl_data_offer_listener offer_listener = {.offer = offer_offer, .source_actions = offer_source_actions, .action = offer_action};
static void dd_data_offer(void* d, struct wl_data_device* dev, struct wl_data_offer* o) { (void)d; (void)dev; has_text = false; wl_data_offer_add_listener(o, &offer_listener, NULL); }
static void dd_enter(void* d, struct wl_data_device* dev, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o) {
    (void)d; (void)dev; (void)x; (void)y; offer = o; motions = 0;
    SAY("TARGET enter own=%d offer=%d text=%d", s == surface, o != NULL, has_text);
    if (o && s == surface && has_text) { wl_data_offer_accept(o, serial, "text/plain;charset=utf-8");
        wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY); }
}
static void dd_leave(void* d, struct wl_data_device* dev) { (void)d; (void)dev; SAY("TARGET leave motions=%d", motions); if (offer) wl_data_offer_destroy(offer); offer = NULL; }
static void dd_motion(void* d, struct wl_data_device* dev, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dev; (void)t; (void)x; (void)y; motions++; }
static void dd_drop(void* d, struct wl_data_device* dev) {
    (void)d; (void)dev; SAY("TARGET drop motions=%d action=%u", motions, drop_action); if (!offer) return;
    int p[2]; CHECK(!pipe2(p, O_CLOEXEC)); wl_data_offer_receive(offer, "text/plain;charset=utf-8", p[1]); close(p[1]);
    struct wl_display* display = d; (void)display;
    wl_display_flush(wl_proxy_get_display((struct wl_proxy*)offer) ? wl_proxy_get_display((struct wl_proxy*)offer) : NULL);
    char text[128] = ""; size_t used = 0; struct pollfd pf = {p[0], POLLIN, 0};
    for (int i = 0; i < 50 && used < sizeof(text) - 1; i++) {
        wl_display_flush(wl_proxy_get_display((struct wl_proxy*)offer)); wl_display_roundtrip(wl_proxy_get_display((struct wl_proxy*)offer));
        if (poll(&pf, 1, 100) > 0) { ssize_t n = read(p[0], text + used, sizeof(text) - 1 - used); if (n <= 0) break; used += (size_t)n; }
    }
    close(p[0]); text[used] = 0; SAY("TARGET received: %s", text);
    wl_data_offer_finish(offer); wl_data_offer_destroy(offer); offer = NULL;
}
static void dd_selection(void* d, struct wl_data_device* dev, struct wl_data_offer* o) { (void)d; (void)dev; if (o && o != offer) wl_data_offer_destroy(o); }
static const struct wl_data_device_listener device_listener = {.data_offer = dd_data_offer, .enter = dd_enter, .leave = dd_leave, .motion = dd_motion, .drop = dd_drop, .selection = dd_selection};
static int run(bool as_target, int seconds) {
    target_role = as_target;
    struct wl_display* display = wl_display_connect(NULL); CHECK(display);
    struct wl_registry* registry = wl_display_get_registry(display); wl_registry_add_listener(registry, &registry_listener, NULL);
    CHECK(wl_display_roundtrip(display) >= 0 && compositor && shm && shell && seat && manager);
    xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    device = wl_data_device_manager_get_data_device(manager, seat); wl_data_device_add_listener(device, &device_listener, display);
    pointer = wl_seat_get_pointer(seat); wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(shell, surface); xdg_surface_add_listener(xdg, &xdg_listener, NULL);
    struct xdg_toplevel* top = xdg_surface_get_toplevel(xdg); xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_app_id(top, as_target ? "nexus.dndtarget" : "nexus.dndsource"); xdg_toplevel_set_title(top, as_target ? "Drop here" : "Drag from here");
    wl_surface_commit(surface); while (!configured) CHECK(wl_display_dispatch(display) >= 0);
    wl_surface_attach(surface, buffer(400, 300, as_target ? 0xFF3060C0 : 0xFF30A050), 0, 0); wl_surface_damage(surface, 0, 0, 400, 300); wl_surface_commit(surface);
    SAY("%s ready", as_target ? "TARGET" : "SOURCE");
    time_t end = time(NULL) + seconds;
    while (time(NULL) < end) { if (wl_display_dispatch_pending(display) < 0) break; wl_display_flush(display);
        struct pollfd pf = {wl_display_get_fd(display), POLLIN, 0}; if (poll(&pf, 1, 200) > 0 && wl_display_dispatch(display) < 0) { SAY("%s disconnected", as_target ? "TARGET" : "SOURCE"); return 1; } }
    SAY("%s done", as_target ? "TARGET" : "SOURCE"); return 0;
}
int main(int argc, char** argv) {
    int seconds = argc > 2 ? atoi(argv[2]) : 240;
    if (argc > 1 && !strcmp(argv[1], "target")) return run(true, seconds);
    if (argc > 1 && !strcmp(argv[1], "pair")) {
        pid_t pid = fork(); if (!pid) { execl("/proc/self/exe", argv[0], "target", argc > 2 ? argv[2] : "240", (char*)NULL); _exit(127); }
        struct timespec t = {1, 0}; nanosleep(&t, NULL);   /* the target maps first */
        int r = run(false, seconds); int status; waitpid(pid, &status, 0); return r;
    }
    return run(false, seconds);
}
