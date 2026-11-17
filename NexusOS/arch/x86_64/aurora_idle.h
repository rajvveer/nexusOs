/* Idle inhibition by Wayland clients (aurora_idle.c: zwp_idle_inhibit_manager_v1). */
#ifndef AURORA_IDLE_H
#define AURORA_IDLE_H
#include <stdbool.h>
#include <wayland-server-core.h>
bool aurora_idle_init(struct wl_display* display);
int aurora_idle_inhibited(void);   /* inhibitors whose surface is shown now */
/* ---- service of desktop.c ---- */
bool aurora_host_surface_shown(struct wl_resource* surface);   /* mapped in the active session */
#endif
