/* Aurora removable media (aurora_media.c): automounts USB drives from kernel block uevents. */
#ifndef AURORA_MEDIA_H
#define AURORA_MEDIA_H
#include <stdbool.h>
struct wl_event_loop;
/* Listens on NETLINK_KOBJECT_UEVENT, mounts drives already attached, and reports through notify (the desktop's notice). */
bool aurora_media_init(struct wl_event_loop* loop, void (*notify)(const char* text));
#endif
