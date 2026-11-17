/* Settings > Sound: the default output and input device (aurora_sound.h: "sound", "set-sound output=ID input=ID").
 * Each list starts with "Automatic" (the device plugged in last, like a headset taking over on macOS) followed by the
 * cards that can play or record; the active device is marked. The page refreshes every 2 s, so a USB headset that is
 * plugged in or pulled out while it is open shows up at once. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aurora_settings.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

#define MAX_CARDS 8
struct entry { char id[16], name[40]; bool playback, capture; };
static struct entry cards[MAX_CARDS]; static int count, list_y, page_y, ticks;
static char output[16], input[16], want_output[16] = "auto", want_input[16] = "auto";
static bool foreign;

static void value(const char* line, const char* key, char* out) {   /* "key=VALUE " -> VALUE (may be empty) */
    const char* p = strstr(line, key); if (!p) return;
    p += strlen(key); int n = 0; while (p[n] && p[n] != ' ' && n < 15) { out[n] = p[n]; n++; } out[n] = 0;
}
static void load(void) {
    char reply[4096];
    if (!settings_request("sound\n", reply, sizeof(reply))) { settings_message(reply); return; }
    count = 0;
    for (char* line = strtok(reply, "\n"); line; line = strtok(NULL, "\n")) {
        struct entry e; memset(&e, 0, sizeof(e)); int index, playback, capture, used = 0;
        if (count < MAX_CARDS && sscanf(line, "card %d id=%15s playback=%d capture=%d name=%n", &index, e.id, &playback, &capture, &used) == 4 && used) {
            snprintf(e.name, sizeof(e.name), "%s", line + used); e.playback = playback; e.capture = capture; cards[count++] = e;
        } else if (!strncmp(line, "output=", 7)) {
            value(line, "output=", output); value(line, " input=", input); value(line, "want_output=", want_output);
            value(line, "want_input=", want_input); char f[4] = "0"; value(line, "foreign=", f); foreign = f[0] == '1';
        }
    }
    settings_redraw();
}
static void set(const char* key, const char* value) {
    char request[96], reply[512]; snprintf(request, sizeof(request), "set-sound %s=%s\n", key, value);
    if (!settings_request(request, reply, sizeof(reply))) { char text[600]; snprintf(text, sizeof(text), "Could not change it: %s", reply); settings_message(text); }
    load();
}
static void enter(void) { ticks = 0; load(); }
/* Rows of one direction: Automatic, then the cards; returns the next y. */
static int rows(int x, int y, bool capture, const char* want, const char* active) {
    appui_px_row(x + 16, y, 560, 30, !strcmp(want, "auto"));
    char text[120]; const char* current = "none";
    for (int i = 0; i < count; i++) if (!strcmp(cards[i].id, active)) current = cards[i].name;
    snprintf(text, sizeof(text), "Automatic (now: %s)", current);
    appui_px_text(x + 28, y + 7, text, APPUI_F_BODY, APPUI_TEXT); y += 34;
    for (int i = 0; i < count; i++) {
        if (capture ? !cards[i].capture : !cards[i].playback) continue;
        appui_px_row(x + 16, y, 560, 30, !strcmp(want, cards[i].id));
        appui_px_text(x + 28, y + 7, cards[i].name, APPUI_F_BODY, APPUI_TEXT);
        if (!strcmp(active, cards[i].id)) appui_px_text_r(x + 566, y + 7, "in use", APPUI_F_SMALL, APPUI_SUBTLE);
        y += 34;
    }
    return y;
}
static int output_end, input_start;
static void draw(int x, int y, int w, int h) {
    (void)w; (void)h; page_y = y;
    appui_px_text(x + 20, y + 14, "Sound", APPUI_F_TITLE, APPUI_TEXT);
    list_y = y + 56;
    appui_px_text(x + 20, list_y, "Output", APPUI_F_BODY, APPUI_SUBTLE);
    output_end = rows(x, list_y + 24, false, want_output, output);
    input_start = output_end + 16;
    appui_px_text(x + 20, input_start, "Input", APPUI_F_BODY, APPUI_SUBTLE);
    int end = rows(x, input_start + 24, true, want_input, input);
    if (foreign) appui_px_text(x + 20, end + 16, "/etc/asound.conf was written by hand: these choices are not applied.", APPUI_F_SMALL, APPUI_SUBTLE);
    else if (!count) appui_px_text(x + 20, end + 16, "No sound device.", APPUI_F_SMALL, APPUI_SUBTLE);
}
/* Which entry a row y hits: -1 automatic, card index, or -2 none. */
static int hit(int y, int top, bool capture) {
    if (y < top) return -2;
    int row = (y - top) / 34;
    if (!row) return -1;
    for (int i = 0, k = 1; i < count; i++) {
        if (capture ? !cards[i].capture : !cards[i].playback) continue;
        if (k++ == row) return i;
    }
    return -2;
}
static void click(int x, int y) {
    if (x < 16 || x > 576) return;
    int absolute = page_y + y;
    if (absolute < output_end) { int i = hit(absolute, list_y + 24, false); if (i >= -1) set("output", i < 0 ? "auto" : cards[i].id); }
    else if (absolute > input_start) { int i = hit(absolute, input_start + 24, true); if (i >= -1) set("input", i < 0 ? "auto" : cards[i].id); }
}
static bool tick(void) { if (++ticks % 40 == 0) load(); return false; }
const struct settings_page settings_sound = {"sound", "Sound", ICON_MUSIC, enter, draw, click, NULL, NULL, tick};
