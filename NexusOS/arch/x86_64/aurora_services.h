/* Aurora (pid 1) and the service manager nexusd (aurora_services.c). */
#ifndef AURORA_SERVICES_H
#define AURORA_SERVICES_H
#include <stdbool.h>
#include <sys/types.h>
struct wl_event_loop;
/* boot: desktop notifications socket (notify shows a message), nexusd and the desktop user's session units */
void aurora_services_start(struct wl_event_loop* loop, void (*notify)(const char* text));
bool aurora_services_reaped(pid_t pid, int status, bool shutting_down); /* true when pid was nexusd or the session helper */
void aurora_services_stop(void);                                    /* shutdown: SIGTERM to nexusd (it stops every unit) */
bool aurora_services_running(void);
#endif
