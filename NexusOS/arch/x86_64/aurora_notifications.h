/* aurora-app notifications: the Notification Centre window (aurora_notifications.c). */
#ifndef AURORA_NOTIFICATIONS_H
#define AURORA_NOTIFICATIONS_H
#include <stdbool.h>
int aurora_notifications_open(void);
bool aurora_notifications_tick(void);   /* reload every 2 s; true: redraw */
#endif
