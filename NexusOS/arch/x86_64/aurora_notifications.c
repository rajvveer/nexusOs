/* aurora-app notifications: the Notification Centre (the menu-bar bell). Lists the user's notifications, newest first
 * (aurora_notices.c over the control socket), switches Do Not Disturb and clears the list. Keyboard: D toggles Do Not
 * Disturb, C clears, Up/Down scroll; opening it marks everything read. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aurora_notifications.h"
#include "aurora_settings.h"
#include "../../kernel/window.h"
#include "../../kernel/appui.h"
#include "../../kernel/macui.h"

#define ROWS 100
static char apps[ROWS][64], texts[ROWS][384]; static long long times[ROWS];
static int count, scroll, dnd; static char status[200]; static unsigned long long last_load;
static unsigned long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000; }
static void load(void) {
    static char reply[65536]; count = 0;
    if (!settings_request("notices\n", reply, sizeof(reply))) { snprintf(status, sizeof(status), "%s", reply); return; }
    for (char* save = NULL, *line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        long long when; char* app = strstr(line, " app="); char* text = strstr(line, " text=");
        if (!strncmp(line, "notice time=", 12) && app && text && count < ROWS && sscanf(line + 12, "%lld", &when) == 1) {
            *text = 0; times[count] = when; snprintf(apps[count], sizeof(apps[count]), "%s", app + 5); snprintf(texts[count], sizeof(texts[count]), "%s", text + 6); count++;
        } else if (!strncmp(line, "count=", 6)) { char* d = strstr(line, "dnd="); if (d) dnd = atoi(d + 4); }
    }
    snprintf(status, sizeof(status), "%d notification%s%s", count, count == 1 ? "" : "s", dnd ? " - Do Not Disturb is on" : "");
    last_load = now_ms();
}
static void set_dnd(int on) {
    char request[32], reply[256]; snprintf(request, sizeof(request), "set-dnd %d\n", on);
    if (!settings_request(request, reply, sizeof(reply))) snprintf(status, sizeof(status), "%s", reply);
    printf("[64] AURORA NOTIFICATIONS dnd=%d\n", on); fflush(stdout); load();
}
static void clear_all(void) { char reply[256]; settings_request("notices-clear\n", reply, sizeof(reply)); printf("[64] AURORA NOTIFICATIONS cleared\n"); fflush(stdout); load(); }
static void draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d; int x, y, w, h; window_content_px(id, &x, &y, &w, &h); appui_px_fill(x, y, w, h, APPUI_BG);
    appui_px_text(x + 16, y + 14, "Notifications", APPUI_F_TITLE, APPUI_TEXT);
    appui_px_text(x + w - 330, y + 18, "Do Not Disturb", APPUI_F_BODY, APPUI_TEXT); appui_px_toggle(x + w - 190, y + 14, dnd != 0);
    appui_px_button(x + w - 110, y + 12, 94, 30, "Clear All", false);
    appui_px_sep(x, y + 54, w);
    int rows = (h - 90) / 52; if (scroll > count - 1) scroll = count > 0 ? count - 1 : 0;
    for (int r = 0; r < rows && scroll + r < count; r++) {
        int i = scroll + r, ry = y + 62 + r * 52; char when[32]; time_t t = (time_t)times[i]; struct tm tm; localtime_r(&t, &tm); strftime(when, sizeof(when), "%H:%M", &tm);
        appui_px_card(x + 12, ry, w - 24, 46, 8, APPUI_SIDEBAR);
        appui_px_text(x + 24, ry + 4, apps[i][0] ? apps[i] : "System", APPUI_F_SMALL, APPUI_SUBTLE); appui_px_text_r(x + w - 24, ry + 4, when, APPUI_F_SMALL, APPUI_SUBTLE);
        appui_px_text(x + 24, ry + 22, texts[i], APPUI_F_BODY, APPUI_TEXT);
    }
    if (!count) appui_px_text_c(x + w / 2, y + h / 2 - 10, "No notifications", APPUI_F_BODY, APPUI_SUBTLE);
    appui_px_sep(x, y + h - 28, w); appui_px_text(x + 12, y + h - 22, status, APPUI_F_SMALL, APPUI_SUBTLE);
}
static void key(int id, char k) {
    (void)id; unsigned char c = (unsigned char)k;
    if (k == 'd' || k == 'D') set_dnd(!dnd); else if (k == 'c' || k == 'C') clear_all();
    else if (c == 0x80 && scroll > 0) scroll--; else if (c == 0x81 && scroll + 1 < count) scroll++;
}
static void click(int id, int px, int py) {
    int x, y, w, h; window_content_px(id, &x, &y, &w, &h); (void)x; (void)y; (void)h;
    if (py >= 12 && py < 44 && px >= w - 200 && px < w - 130) set_dnd(!dnd);
    else if (py >= 12 && py < 44 && px >= w - 110 && px < w - 16) clear_all();
}
int aurora_notifications_open(void) {
    load(); char reply[256]; settings_request("notices-seen\n", reply, sizeof(reply));
    printf("[64] AURORA NOTIFICATIONS open count=%d dnd=%d\n", count, dnd); fflush(stdout);
    int id = window_create("Notifications", 0, 0, 72, 30, draw, key); window_set_on_click(id, click); return id;
}
bool aurora_notifications_tick(void) { if (now_ms() - last_load < 2000) return false; load(); return true; }
