/* Settings > Accessibility: larger text, increased contrast, inverted colours, the magnifier (and its zoom), reduced
 * motion and sticky keys (aurora_a11y.h: "a11y", "set-a11y key=value"), and the screen reader (Orca with
 * aurora-a11y-bridge for the session's user, aurora_reader.h: "screen-reader", "set-screen-reader 0|1"). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aurora_settings.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

static const char* const KEYS[6] = {"larger_text", "contrast", "invert", "magnifier", "reduce_motion", "sticky_keys"};
static const char* const LABELS[6] = {"Larger text (Aurora, after an app restart)", "Increase contrast", "Invert colours", "Magnifier (Super+= / Super+-)", "Reduce motion", "Sticky keys"};
static int values[6], zoom = 200, rows_y, page_y, reader;
static const int ZOOMS[4] = {150, 200, 300, 400};

static void load(void) {
    char reply[512];
    if (!settings_request("a11y\n", reply, sizeof(reply))) { settings_message(reply); return; }
    for (int i = 0; i < 6; i++) { char key[40]; snprintf(key, sizeof(key), "%s=", KEYS[i]); char* p = strstr(reply, key); values[i] = p ? atoi(p + strlen(key)) : 0; }
    char* z = strstr(reply, "zoom="); if (z) zoom = atoi(z + 5);
    printf("[64] SETTINGS A11Y %s", reply); fflush(stdout);
    char state[128]; if (settings_request("screen-reader\n", state, sizeof(state))) { char* r = strstr(state, "screen_reader="); reader = r ? atoi(r + 14) : 0; }
    settings_redraw();
}
static void set(const char* key, int value) {
    char request[96], reply[256]; snprintf(request, sizeof(request), "set-a11y %s=%d\n", key, value);
    if (!settings_request(request, reply, sizeof(reply))) { char text[300]; snprintf(text, sizeof(text), "Could not change it: %s", reply); settings_message(text); }
    load();
}
static void enter(void) { load(); }
static void draw(int x, int y, int w, int h) {
    (void)w; (void)h; page_y = y;
    appui_px_text(x + 20, y + 14, "Accessibility", APPUI_F_TITLE, APPUI_TEXT);
    rows_y = y + 56;
    for (int i = 0; i < 6; i++) {
        int ry = rows_y + i * 40;
        appui_px_text(x + 20, ry + 6, LABELS[i], APPUI_F_BODY, APPUI_TEXT); appui_px_toggle(x + 420, ry, values[i] != 0);
    }
    int zy = rows_y + 6 * 40;
    appui_px_text(x + 20, zy + 6, "Magnifier zoom", APPUI_F_BODY, APPUI_TEXT);
    static const char* const zooms[4] = {"150%", "200%", "300%", "400%"}; int current = 1; for (int i = 0; i < 4; i++) if (ZOOMS[i] == zoom) current = i;
    appui_px_segment(x + 420, zy, 280, 28, zooms, 4, current);
    appui_px_text(x + 20, zy + 50, "Screen reader (Orca, Super+Alt+S)", APPUI_F_BODY, APPUI_TEXT); appui_px_toggle(x + 420, zy + 44, reader != 0);
    appui_px_text(x + 20, zy + 84, "Ctrl+F2: menu bar, Ctrl+F3: Dock, Ctrl+Space: Spotlight. Orca needs the orca package (Software).", APPUI_F_SMALL, APPUI_SUBTLE);
    static int reported = -1;
    if (reported != rows_y - y) { reported = rows_y - y; printf("[64] SETTINGS A11Y GRID toggle_x=%d first_y=%d step=40 zoom_y=%d reader_y=%d\n", 420 + 26, rows_y - y + 14, zy - y + 14, zy - y + 58); fflush(stdout); }
}
static void click(int x, int y) {
    int ry = y - (rows_y - page_y);
    if (x >= 420 && x < 420 + 52 && ry >= 0 && ry < 6 * 40) { int i = ry / 40; set(KEYS[i], !values[i]); return; }
    if (x >= 420 && x < 700 && ry >= 6 * 40 && ry < 6 * 40 + 28) { int i = (x - 420) * 4 / 280; set("zoom", ZOOMS[i < 0 ? 0 : i > 3 ? 3 : i]); }
    if (x >= 420 && x < 420 + 52 && ry >= 6 * 40 + 44 && ry < 6 * 40 + 72) {   /* the screen reader */
        char request[48], reply[256]; snprintf(request, sizeof(request), "set-screen-reader %d\n", !reader);
        if (!settings_request(request, reply, sizeof(reply))) { char text[300]; snprintf(text, sizeof(text), "Could not change it: %s", reply); settings_message(text); }
        printf("[64] SETTINGS A11Y READER set=%d\n", !reader); fflush(stdout); load();
    }
}
const struct settings_page settings_a11y = {"accessibility", "Accessibility", ICON_APPEARANCE, enter, draw, click, NULL, NULL, NULL};
