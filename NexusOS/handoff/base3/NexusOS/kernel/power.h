/* NexusOS — Power Menu (Header) */
#ifndef POWER_H
#define POWER_H
#include "types.h"
void power_menu_open(void);
void power_menu_close(void);
bool power_menu_is_open(void);
void power_menu_draw(void);
int  power_menu_handle_key(char key);
#define POWER_NONE     0
#define POWER_SHUTDOWN 1
#define POWER_RESTART  2
#define POWER_LOCK     3
void power_shutdown(void);
void power_restart(void);
#endif
