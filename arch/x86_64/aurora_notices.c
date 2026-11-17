/* Notification history and Do Not Disturb (aurora_notices.h). Each user's notifications (org.freedesktop.Notifications
 * through nexus-notifyd, the desktop portal's Notification portal, Aurora's own notices) are kept per user, newest
 * first, at most NOTICES each; with Do Not Disturb on they are only recorded, not shown as banners. The Notification
 * Centre (aurora-app notifications, the bell in the menu bar) reads and clears them over the control socket:
 *   notices            -> "notice time=<unix> app=<app> text=<text>" lines (the requester's own), then unread/dnd
 *   notices-clear      forget them      notices-seen   mark them read
 *   dnd                -> "dnd=0|1"     set-dnd 0|1    switch Do Not Disturb for the requester */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "aurora_notices.h"
#include "aurora_control.h"
#include "../../kernel/macui.h"
#include "../../kernel/menubar.h"

#define NOTICES 100
#define USERS 8
struct notice { time_t when; char app[64], text[384]; };
struct user_notices { bool used, dnd; unsigned uid; int count, unread; struct notice list[NOTICES]; };
static struct user_notices users[USERS];

static struct user_notices* of(unsigned uid, bool create) {
    for (int i = 0; i < USERS; i++) if (users[i].used && users[i].uid == uid) return &users[i];
    if (!create) return NULL;
    for (int i = 0; i < USERS; i++) if (!users[i].used) { memset(&users[i], 0, sizeof(users[i])); users[i].used = true; users[i].uid = uid; return &users[i]; }
    return &users[0];   /* full: the oldest slot is reused */
}
static void clean(char* out, size_t size, const char* text) {
    snprintf(out, size, "%s", text ? text : ""); for (char* p = out; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
}
bool aurora_notice_post(unsigned uid, const char* app, const char* text) {
    struct user_notices* u = of(uid, true);
    memmove(&u->list[1], &u->list[0], sizeof(u->list[0]) * (NOTICES - 1));
    u->list[0].when = time(NULL); clean(u->list[0].app, sizeof(u->list[0].app), app); clean(u->list[0].text, sizeof(u->list[0].text), text);
    if (u->count < NOTICES) u->count++;
    if (u->unread < NOTICES) u->unread++;
    printf("[64] AURORA NOTICE uid=%u dnd=%d app=%s text=%s\n", uid, u->dnd, u->list[0].app, u->list[0].text); fflush(stdout);
    return !u->dnd;
}
bool aurora_notice_dnd(unsigned uid) { struct user_notices* u = of(uid, false); return u && u->dnd; }
int aurora_notice_unread(unsigned uid) { struct user_notices* u = of(uid, false); return u ? u->unread : 0; }
static unsigned requester(void) { return (unsigned)aurora_control_peer_uid(); }
static unsigned subject(void) { unsigned uid = requester(); return uid ? uid : aurora_notices_session_uid(); }   /* root asks about the active user */
static bool control_list(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; struct user_notices* u = of(subject(), false);
    for (int i = 0; u && i < u->count; i++) aurora_control_print(reply, "notice time=%lld app=%s text=%s\n", (long long)u->list[i].when, u->list[i].app, u->list[i].text);
    aurora_control_print(reply, "count=%d unread=%d dnd=%d\n", u ? u->count : 0, u ? u->unread : 0, u ? u->dnd : 0);
    return true;
}
static bool control_clear(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; struct user_notices* u = of(subject(), false);
    if (u) { u->count = u->unread = 0; } aurora_notices_changed(); return true;
}
static bool control_seen(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; struct user_notices* u = of(subject(), false);
    if (u) u->unread = 0; aurora_notices_changed(); return true;
}
static bool control_dnd(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; aurora_control_print(reply, "dnd=%d\n", aurora_notice_dnd(subject())); return true;
}
static bool control_set_dnd(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply;
    if (strcmp(args, "0") && strcmp(args, "1")) { snprintf(error, size, "usage: set-dnd 0|1"); return false; }
    struct user_notices* u = of(subject(), true); u->dnd = args[0] == '1';
    printf("[64] AURORA DND uid=%u on=%d\n", u->uid, u->dnd); fflush(stdout); aurora_notices_changed(); return true;
}
/* the menu bar's bell: unread count of the active user; a click opens the Notification Centre (UI_ACT_NOTIFS) */
static int bell_width(void) { int n = aurora_notice_unread(aurora_notices_session_uid()); return n || aurora_notice_dnd(aurora_notices_session_uid()) ? 30 + (n > 9 ? 8 : 0) : 22; }
static void bell_draw(int x, int cy) {
    unsigned uid = aurora_notices_session_uid(); int n = aurora_notice_unread(uid); bool dnd = aurora_notice_dnd(uid);
    static int reported_x = -1; if (x != reported_x) { reported_x = x; printf("[64] AURORA BELL x=%d cy=%d\n", x, cy); fflush(stdout); }   /* for tests */
    ui_circle(x + 9, cy - 1, 6, 0xFFFFFF, dnd ? 110 : 230); ui_blend_rect(x + 3, cy + 1, 12, 4, 0xFFFFFF, dnd ? 110 : 230); ui_circle(x + 9, cy + 6, 2, 0xFFFFFF, dnd ? 110 : 230);
    if (dnd) ui_blend_rect(x + 2, cy - 1, 15, 2, 0xFF8A80, 255);   /* Do Not Disturb: struck through */
    if (n) { char text[8]; snprintf(text, sizeof(text), "%d", n > 99 ? 99 : n); ui_circle(x + 20, cy - 4, 7, 0xE5484D, 255); ui_text_center(x + 20, cy - 4 - ui_font_height(UF_SMALL) / 2, text, UF_SMALL, 0xFFFFFF); }
}
void aurora_notices_init(void) {
    static const menubar_status_t bell = {bell_width, bell_draw, UI_ACT_NOTIFS}; menubar_status_add(&bell);
    aurora_control_register("notices", false, control_list);
    aurora_control_register("notices-clear", false, control_clear);
    aurora_control_register("notices-seen", false, control_seen);
    aurora_control_register("dnd", false, control_dnd);
    aurora_control_register("set-dnd", false, control_set_dnd);
}
