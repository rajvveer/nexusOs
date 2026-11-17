/* zwp_idle_inhibit_manager_v1 (idle-inhibit-unstable-v1): a client keeps the session from going idle while one of its
 * surfaces is shown, as Chromium (video, presentations: WaylandScreen::SetScreenSaverSuspended) and Firefox (video
 * wake locks) ask on Wayland. Aurora consults aurora_idle_inhibited() before the idle lock (aurora_lock.c, through
 * aurora_session_idle_inhibited()); logind's "idle" inhibitors join through the control socket (aurora_session.c).
 * An inhibitor counts only while its surface is mapped in the active session (protocol: "visible"). */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-server.h>
#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "aurora_idle.h"

struct inhibitor { struct wl_list link; struct wl_resource* resource; struct wl_resource* surface; struct wl_listener surface_gone; };
static struct wl_list inhibitors;
static bool logged;

static void inhibitor_free(struct wl_resource* resource) {
    struct inhibitor* in = wl_resource_get_user_data(resource);
    if (!in) return;
    wl_list_remove(&in->link); if (in->surface) wl_list_remove(&in->surface_gone.link);
    free(in);
    printf("[64] AURORA IDLE INHIBITOR released (%d left)\n", wl_list_length(&inhibitors)); fflush(stdout);
}
static void surface_gone(struct wl_listener* listener, void* data) {   /* the surface died first: the inhibitor is inert */
    (void)data; struct inhibitor* in = wl_container_of(listener, in, surface_gone);
    wl_list_remove(&in->surface_gone.link); wl_list_init(&in->surface_gone.link); in->surface = NULL;
}
static void destroy(struct wl_client* client, struct wl_resource* resource) { (void)client; wl_resource_destroy(resource); }
static const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {.destroy = destroy};
static void create_inhibitor(struct wl_client* client, struct wl_resource* manager, uint32_t id, struct wl_resource* surface) {
    struct inhibitor* in = calloc(1, sizeof(*in));
    struct wl_resource* r = wl_resource_create(client, &zwp_idle_inhibitor_v1_interface, wl_resource_get_version(manager), id);
    if (!in || !r) { free(in); if (r) wl_resource_destroy(r); wl_client_post_no_memory(client); return; }
    in->resource = r; in->surface = surface; in->surface_gone.notify = surface_gone;
    wl_resource_add_destroy_listener(surface, &in->surface_gone);
    wl_list_insert(&inhibitors, &in->link);
    wl_resource_set_implementation(r, &inhibitor_impl, in, inhibitor_free);
    printf("[64] AURORA IDLE INHIBITOR created (%d)\n", wl_list_length(&inhibitors)); fflush(stdout);
}
static const struct zwp_idle_inhibit_manager_v1_interface manager_impl = {.destroy = destroy, .create_inhibitor = create_inhibitor};
static void bind_manager(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* r = wl_resource_create(client, &zwp_idle_inhibit_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(r, &manager_impl, NULL, NULL);
}
bool aurora_idle_init(struct wl_display* display) {
    wl_list_init(&inhibitors);
    return wl_global_create(display, &zwp_idle_inhibit_manager_v1_interface, 1, NULL, bind_manager) != NULL;
}
int aurora_idle_inhibited(void) {
    int active = 0; struct inhibitor* in;
    wl_list_for_each(in, &inhibitors, link) if (in->surface && aurora_host_surface_shown(in->surface)) active++;
    if (active && !logged) { printf("[64] AURORA IDLE INHIBITED surfaces=%d\n", active); fflush(stdout); }
    logged = active > 0;
    return active;
}
