/* ============================================================================
 * NexusOS — Notification Toasts (Header)
 * ============================================================================
 * Auto-dismiss popup notifications at the top-right of the desktop.
 * ============================================================================ */

#ifndef NOTIFY_H
#define NOTIFY_H

#include "types.h"

/* Push a notification message (auto-dismisses after ~3 seconds) */
void notify_push(const char* message);

/* Update notification timers (call each frame/tick) */
void notify_update(void);

/* Draw active notifications to GUI buffer */
void notify_draw(void);

#endif /* NOTIFY_H */
