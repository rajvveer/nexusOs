/* aurora-app clipboard: the clipboard history (aurora_clipboard.h) over the control socket. Up/Down choose, Enter puts
 * the chosen text back on the clipboard and closes (paste it with Ctrl+V), Delete removes it, C or "Clear History"
 * forgets everything, Esc closes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aurora_clipboard_app.h"
#include "aurora_settings.h"
#include "../../kernel/window.h"
#include "../../kernel/appui.h"
#include "../../kernel/macui.h"

#define ROWS 25
static char previews[ROWS][128]; static long sizes[ROWS], ages[ROWS];
static int count, selected, window_id = -1; static char status[200]; static unsigned long long last_load;
static unsigned long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000; }
static void load(void) {
    static char reply[65536]; count = 0;
    if (!settings_request("clipboard-history\n", reply, sizeof(reply))) { snprintf(status, sizeof(status), "%s", reply); return; }
    for (char* save = NULL, *line = strtok_r(reply, "\n", &save); line && count < ROWS; line = strtok_r(NULL, "\n", &save)) {
        int index; long bytes, age; int used = 0;
        if (sscanf(line, "entry\t%d\t%ld\t%ld\t%n", &index, &bytes, &age, &used) < 3 || !used) continue;
        sizes[count] = bytes; ages[count] = age; snprintf(previews[count], sizeof(previews[count]), "%s", line + used); count++;
    }
    if (selected >= count) selected = count ? count - 1 : 0;
    snprintf(status, sizeof(status), count ? "%d item%s - Enter copies, Delete removes, C clears all" : "Nothing copied yet", count, count == 1 ? "" : "s");
    last_load = now_ms();
}
static void request(const char* line, const char* what) {
    char reply[256]; if (!settings_request(line, reply, sizeof(reply))) snprintf(status, sizeof(status), "%s", reply);
    printf("[64] AURORA CLIPBOARD APP %s\n", what); fflush(stdout);
}
static void use(int i) {
    if (i < 0 || i >= count) return;
    char line[32]; snprintf(line, sizeof(line), "clipboard-use %d\n", i); request(line, "use"); window_destroy(window_id);
}
static void remove_entry(int i) { if (i < 0 || i >= count) return; char line[40]; snprintf(line, sizeof(line), "clipboard-remove %d\n", i); request(line, "remove"); load(); }
static void clear_all(void) { request("clipboard-clear\n", "clear"); load(); }
static void draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d; int x, y, w, h; window_content_px(id, &x, &y, &w, &h); appui_px_fill(x, y, w, h, APPUI_BG);
    appui_px_text(x + 16, y + 14, "Clipboard History", APPUI_F_TITLE, APPUI_TEXT);
    appui_px_button(x + w - 140, y + 12, 124, 30, "Clear History", false);
    appui_px_sep(x, y + 54, w);
    int rows = (h - 90) / 40, first = selected >= rows ? selected - rows + 1 : 0;
    for (int r = 0; r < rows && first + r < count; r++) {
        int i = first + r, ry = y + 60 + r * 40; char meta[48];
        appui_px_row(x + 8, ry, w - 16, 36, i == selected);
        appui_px_text(x + 20, ry + 9, previews[i], APPUI_F_BODY, i == selected ? 0xFFFFFF : APPUI_TEXT);
        if (ages[i] < 60) snprintf(meta, sizeof(meta), "%ld B, %ld s ago", sizes[i], ages[i]); else snprintf(meta, sizeof(meta), "%ld B, %ld min ago", sizes[i], ages[i] / 60);
        appui_px_text_r(x + w - 20, ry + 11, meta, APPUI_F_SMALL, i == selected ? 0xFFFFFF : APPUI_SUBTLE);
    }
    if (!count) appui_px_text_c(x + w / 2, y + h / 2 - 10, "Copied text appears here (not from password fields)", APPUI_F_BODY, APPUI_SUBTLE);
    appui_px_sep(x, y + h - 28, w); appui_px_text(x + 12, y + h - 22, status, APPUI_F_SMALL, APPUI_SUBTLE);
}
static void key(int id, char k) {
    (void)id; unsigned char c = (unsigned char)k;
    if (c == 0x80 && selected > 0) selected--; else if (c == 0x81 && selected + 1 < count) selected++;
    else if (k == '\n') use(selected); else if (c == 127 || k == '\b') remove_entry(selected);
    else if (k == 'c' || k == 'C') clear_all(); else if (k == 27) window_destroy(window_id);
}
static void click(int id, int px, int py) {
    int x, y, w, h; window_content_px(id, &x, &y, &w, &h); (void)x; (void)y;
    if (py >= 12 && py < 42 && px >= w - 140 && px < w - 16) { clear_all(); return; }
    int rows = (h - 90) / 40, first = selected >= rows ? selected - rows + 1 : 0, r = (py - 60) / 40;
    if (py >= 60 && r < rows && first + r < count) { if (first + r == selected) use(selected); else selected = first + r; }
}
int aurora_clipboard_open(void) {
    load(); printf("[64] AURORA CLIPBOARD APP open entries=%d\n", count); fflush(stdout);
    window_id = window_create("Clipboard History", 0, 0, 64, 26, draw, key); window_set_on_click(window_id, click); return window_id;
}
bool aurora_clipboard_tick(void) { if (now_ms() - last_load < 2000) return false; load(); return true; }
