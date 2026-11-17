/* Notification history and Do Not Disturb, per user (aurora_notices.c). */
#ifndef AURORA_NOTICES_H
#define AURORA_NOTICES_H
#include <stdbool.h>
void aurora_notices_init(void);                                   /* control commands */
bool aurora_notice_post(unsigned uid, const char* app, const char* text);   /* recorded; true: show the banner */
bool aurora_notice_dnd(unsigned uid);
int aurora_notice_unread(unsigned uid);
/* ---- services of the compositor ---- */
unsigned aurora_notices_session_uid(void);   /* the active session's user (aurora_session.c) */
void aurora_notices_changed(void);           /* the menu-bar bell needs a redraw */
#endif
