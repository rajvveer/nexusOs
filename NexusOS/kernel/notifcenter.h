/* NexusOS — Notification Center (Header) */
#ifndef NOTIFCENTER_H
#define NOTIFCENTER_H
#include "types.h"
void notifcenter_toggle(void);
bool notifcenter_is_open(void);
void notifcenter_draw(void);
void notifcenter_add(const char* msg, uint8_t icon);
int  notifcenter_handle_key(char key);
void notifcenter_clear(void);
#endif
