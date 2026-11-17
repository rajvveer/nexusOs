/* Idle-inhibit test client (tests/test_aurora_idle.py): a window (app_id nexus.idletest) that holds a
 * zwp_idle_inhibitor_v1 on its surface for HOLD seconds, releases it and stays KEEP seconds more.
 *   idle-test HOLD KEEP [hostile]
 * hostile: before the real inhibitor, an inhibitor on a surface that is destroyed first, a second inhibitor on the same
 * surface and the manager destroyed before its inhibitors (the compositor must not crash or leak them). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "[64] IDLETEST FAIL line=%d errno=%d\n", __LINE__, errno); exit(1); } } while (0)
static struct wl_compositor* compositor; static struct wl_shm* shm; static struct xdg_wm_base* shell; static struct zwp_idle_inhibit_manager_v1* manager;
static bool configured;
static void global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    (void)d; (void)version;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) shell = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, "zwp_idle_inhibit_manager_v1")) manager = wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, 1);
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
static struct wl_buffer* buffer(int w, int h) {
    int fd = memfd_create("idle-test", MFD_CLOEXEC); CHECK(fd >= 0 && !ftruncate(fd, w * h * 4));
    uint32_t* pixels = mmap(NULL, (size_t)w * h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(pixels != MAP_FAILED);
    for (int i = 0; i < w * h; i++) pixels[i] = 0xFF2060A0;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, w * h * 4); struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool); close(fd); return b;
}
static void pump(struct wl_display* display, int seconds) {
    struct timespec end; clock_gettime(CLOCK_MONOTONIC, &end); end.tv_sec += seconds;
    for (;;) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) break;
        CHECK(wl_display_roundtrip(display) >= 0); struct timespec t = {0, 200000000}; nanosleep(&t, NULL); }
}
int main(int argc, char** argv) {
    int hold = argc > 1 ? atoi(argv[1]) : 30, keep = argc > 2 ? atoi(argv[2]) : 30; bool hostile = argc > 3 && !strcmp(argv[3], "hostile");
    struct wl_display* display = wl_display_connect(NULL); CHECK(display);
    struct wl_registry* registry = wl_display_get_registry(display); wl_registry_add_listener(registry, &registry_listener, NULL);
    CHECK(wl_display_roundtrip(display) >= 0); CHECK(compositor && shm && shell);
    if (!manager) { printf("[64] IDLETEST no zwp_idle_inhibit_manager_v1\n"); fflush(stdout); return 2; }
    xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    if (hostile) {
        struct wl_surface* doomed = wl_compositor_create_surface(compositor);
        struct zwp_idle_inhibitor_v1* orphan = zwp_idle_inhibit_manager_v1_create_inhibitor(manager, doomed);
        wl_surface_destroy(doomed); CHECK(wl_display_roundtrip(display) >= 0);   /* surface gone first: inert */
        zwp_idle_inhibitor_v1_destroy(orphan); CHECK(wl_display_roundtrip(display) >= 0);
        printf("[64] IDLETEST hostile steps done\n"); fflush(stdout);
    }
    struct wl_surface* surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(shell, surface); xdg_surface_add_listener(xdg, &xdg_listener, NULL);
    struct xdg_toplevel* top = xdg_surface_get_toplevel(xdg); xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_app_id(top, "nexus.idletest"); xdg_toplevel_set_title(top, "Idle inhibitor test");
    wl_surface_commit(surface); while (!configured) CHECK(wl_display_dispatch(display) >= 0);
    wl_surface_attach(surface, buffer(320, 200), 0, 0); wl_surface_damage(surface, 0, 0, 320, 200); wl_surface_commit(surface);
    struct zwp_idle_inhibitor_v1* inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(manager, surface);
    struct zwp_idle_inhibitor_v1* twice = hostile ? zwp_idle_inhibit_manager_v1_create_inhibitor(manager, surface) : NULL;
    if (hostile) zwp_idle_inhibit_manager_v1_destroy(manager);   /* the inhibitors outlive their manager */
    CHECK(wl_display_roundtrip(display) >= 0);
    printf("[64] IDLETEST inhibiting for %d s\n", hold); fflush(stdout);
    pump(display, hold);
    zwp_idle_inhibitor_v1_destroy(inhibitor); if (twice) zwp_idle_inhibitor_v1_destroy(twice);
    CHECK(wl_display_roundtrip(display) >= 0);
    printf("[64] IDLETEST released, window stays %d s\n", keep); fflush(stdout);
    pump(display, keep);
    printf("[64] IDLETEST done\n"); fflush(stdout);
    return 0;
}
