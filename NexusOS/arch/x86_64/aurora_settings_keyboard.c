/* Settings > Keyboard: the input sources (xkb layouts, up to 4) of the session. A click on a layout adds it to the
 * end of the list or removes it; the desktop compiles the keymap, sends it to every application and saves it
 * (aurora_keyboard.h: "keyboard", "set-keyboard"). Super+Space or the menu bar's input menu switch between them. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aurora_settings.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

#define LAYOUTS 32
static struct { char code[16], variant[24], label[8], name[48]; } catalog[LAYOUTS]; static int catalog_count;
static int active[4], active_count;   /* catalog indices, in order */
static int rows_y, row_h = 34, page_x, page_y, col_w;

static void load(void) {
    char* text = malloc(16384); if (!text) return;
    if (!settings_request("keyboard\n", text, 16384)) { settings_message(text); free(text); return; }
    char layouts[128] = "", variants[128] = ""; catalog_count = active_count = 0;
    for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "layouts=", 8)) sscanf(line, "layouts=%127s variants=%127s", layouts, variants);
        else if (!strncmp(line, "layout ", 7) && catalog_count < LAYOUTS) {
            int used = 0; char* c = catalog[catalog_count].code; char* v = catalog[catalog_count].variant;
            if (sscanf(line, "layout %15s %23s %7s %n", c, v, catalog[catalog_count].label, &used) >= 3 && used) {
                if (!strcmp(v, "-")) v[0] = 0; snprintf(catalog[catalog_count].name, sizeof(catalog[0].name), "%s", line + used); catalog_count++; }
        }
    }
    free(text);
    if (!strncmp(variants, "variants=", 9)) variants[0] = 0;   /* no variants field value */
    char* ls = NULL, *vs = variants;
    for (char* code = strtok_r(layouts, ",", &ls); code && active_count < 4; code = strtok_r(NULL, ",", &ls)) {
        char variant[24] = ""; char* comma = vs ? strchr(vs, ',') : NULL;
        if (vs) { snprintf(variant, sizeof(variant), "%.*s", comma ? (int)(comma - vs) : (int)strlen(vs), vs); vs = comma ? comma + 1 : NULL; }
        for (int i = 0; i < catalog_count; i++) if (!strcmp(catalog[i].code, code) && !strcmp(catalog[i].variant, variant)) active[active_count++] = i;
    }
    printf("[64] SETTINGS KEYBOARD active=%d catalog=%d\n", active_count, catalog_count); fflush(stdout);
    settings_redraw();
}
static void apply(void) {
    char request[400] = "set-keyboard layouts=", variants[160] = "";
    for (int i = 0; i < active_count; i++) {
        snprintf(request + strlen(request), sizeof(request) - strlen(request), "%s%s", i ? "," : "", catalog[active[i]].code);
        snprintf(variants + strlen(variants), sizeof(variants) - strlen(variants), "%s%s", i ? "," : "", catalog[active[i]].variant);
    }
    snprintf(request + strlen(request), sizeof(request) - strlen(request), " variants=%s\n", variants);
    char reply[256];
    if (!settings_request(request, reply, sizeof(reply))) { char text[300]; snprintf(text, sizeof(text), "Could not change the layouts: %s", reply); settings_message(text); }
    else settings_message(active_count > 1 ? "Switch input sources with Super+Space or the menu bar." : "Keyboard layout saved.");
    load();
}
static void enter(void) { load(); }
static void draw(int x, int y, int w, int h) {
    (void)h; page_x = x; page_y = y;
    appui_px_text(x + 20, y + 14, "Keyboard", APPUI_F_TITLE, APPUI_TEXT);
    char current[160] = "Input sources: ";
    for (int i = 0; i < active_count; i++) snprintf(current + strlen(current), sizeof(current) - strlen(current), "%s%s", i ? ", " : "", catalog[active[i]].name);
    appui_px_text(x + 20, y + 44, active_count ? current : "Input sources: English (US)", APPUI_F_SMALL, APPUI_SUBTLE);
    rows_y = y + 70;
    int columns = 2, cw = (w - 40) / columns; col_w = cw;
    for (int i = 0; i < catalog_count; i++) {
        int cx = x + 20 + (i % columns) * cw, cy = rows_y + (i / columns) * row_h; int order = 0;
        for (int k = 0; k < active_count; k++) if (active[k] == i) order = k + 1;
        appui_px_row(cx, cy, cw - 8, row_h - 4, order != 0);
        char label[80]; snprintf(label, sizeof(label), "%s  %s", order ? "[x]" : "[ ]", catalog[i].name);
        appui_px_text(cx + 10, cy + 8, label, APPUI_F_BODY, APPUI_TEXT);
        appui_px_text_r(cx + cw - 20, cy + 8, catalog[i].label, APPUI_F_SMALL, APPUI_SUBTLE);
    }
    static int reported = -1;
    if (reported != rows_y + cw) { reported = rows_y + cw; printf("[64] SETTINGS KEYBOARD GRID x=%d y=%d w=%d h=%d columns=%d\n", 20, rows_y - y, cw, row_h, columns); fflush(stdout); }
}
static void click(int x, int y) {
    int columns = 2, ry = y - (rows_y - page_y); if (ry < 0 || x < 20 || col_w <= 0) return;   /* the grid of the last draw */
    int row = ry / row_h, col = (x - 20) / col_w; if (col >= columns) return;
    int i = row * columns + col; if (i < 0 || i >= catalog_count) return;
    int at = -1; for (int k = 0; k < active_count; k++) if (active[k] == i) at = k;
    if (at >= 0) { if (active_count == 1) { settings_message("At least one layout stays."); return; }
        for (int k = at; k + 1 < active_count; k++) active[k] = active[k + 1]; active_count--; }
    else { if (active_count == 4) { settings_message("At most four input sources."); return; } active[active_count++] = i; }
    apply();
}
const struct settings_page settings_keyboard = {"keyboard", "Keyboard", ICON_INFO, enter, draw, click, NULL, NULL, NULL};
