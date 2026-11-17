#ifndef NEXUS_AURORA_EXTRA_H
#define NEXUS_AURORA_EXTRA_H
#include <stdbool.h>
int aurora_extra_open(const char *name, const char *path);
bool aurora_extra_tick(void);
bool aurora_extra_close(void);
void aurora_extra_scroll(int direction);
void aurora_extra_cleanup(void);
/* System Settings (aurora_settings.c) and the pointer-drag hook of the Aurora app host (aurora_app.c):
 * drag(x, y, phase) gets content coordinates, phase 0 press, 1 motion with the left button held, 2 release. */
int aurora_settings_open(const char *page);
bool aurora_settings_tick(void);
void aurora_app_set_drag(void (*drag)(int x, int y, int phase));
#endif
