/* Settings > Appearance: the user's colour scheme (Default / Light / Dark) and accent colour. Saved per user in
 * ~/.config/aurora/appearance.conf (color_scheme=default|light|dark, accent=#RRGGBB), which the desktop portal
 * (xdg-desktop-portal-aurora: org.freedesktop.appearance color-scheme / accent-color) hands to Chrome, Firefox, GTK
 * and libadwaita applications; GTK 3 applications also get gtk-application-prefer-dark-theme in
 * ~/.config/gtk-3.0/settings.ini. Keyboard: Left/Right change the scheme, 1-7 pick an accent. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aurora_settings.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

static const char* const SCHEMES[3] = {"default", "light", "dark"};
static const char* const SCHEME_LABELS[3] = {"Default", "Light", "Dark"};
static const uint32_t ACCENTS[7] = {0x3584E4, 0x9141AC, 0xD56199, 0xE01B24, 0xFF7800, 0x2EC27E, 0x6F8396};
static const char* const ACCENT_NAMES[7] = {"Blue", "Purple", "Pink", "Red", "Orange", "Green", "Graphite"};
static int scheme, accent = -1, page_y, scheme_y, accent_y;

static bool config_path(char* out, size_t size, const char* tail) {
    const char* home = getenv("HOME"); if (!home || home[0] != '/') return false;
    return (size_t)snprintf(out, size, "%s/.config/%s", home, tail) < size;
}
static void load(void) {
    char path[512]; scheme = 0; accent = -1; if (!config_path(path, sizeof(path), "aurora/appearance.conf")) return;
    FILE* f = fopen(path, "re"); if (!f) return;
    char line[128]; unsigned rgb;
    while (fgets(line, sizeof(line), f)) {
        for (int i = 0; i < 3; i++) { char key[40]; snprintf(key, sizeof(key), "color_scheme=%s\n", SCHEMES[i]); if (!strcmp(line, key)) scheme = i; }
        if (sscanf(line, "accent=#%06x", &rgb) == 1) for (int i = 0; i < 7; i++) if (ACCENTS[i] == rgb) accent = i;
    }
    fclose(f);
}
static bool write_file(const char* path, const char* text) {
    char tmp[600]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "we"); if (!f) return false;
    bool ok = fputs(text, f) >= 0; ok = !fflush(f) && !fsync(fileno(f)) && ok; fclose(f);
    if (!ok || rename(tmp, path)) { unlink(tmp); return false; }
    return true;
}
static void gtk3_prefer_dark(bool dark) {   /* keep the file's other keys */
    char dir[512], path[600]; if (!config_path(dir, sizeof(dir), "gtk-3.0")) return;
    mkdir(dir, 0755); snprintf(path, sizeof(path), "%s/settings.ini", dir);
    char text[8192] = ""; size_t used = 0; bool section = false, done = false; FILE* f = fopen(path, "re");
    if (f) { char line[512];
        while (fgets(line, sizeof(line), f) && used + strlen(line) + 64 < sizeof(text)) {
            if (!strncmp(line, "gtk-application-prefer-dark-theme", 33)) continue;
            used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", line);
            if (!strcmp(line, "[Settings]\n")) { section = true; used += (size_t)snprintf(text + used, sizeof(text) - used, "gtk-application-prefer-dark-theme=%d\n", dark); done = true; }
        }
        fclose(f); }
    if (!section) used += (size_t)snprintf(text + used, sizeof(text) - used, "[Settings]\n");
    if (!done) snprintf(text + used, sizeof(text) - used, "gtk-application-prefer-dark-theme=%d\n", dark);
    write_file(path, text);
}
static void save(void) {
    char dir[512], path[600], text[160];
    if (!config_path(dir, sizeof(dir), "aurora")) { settings_message("No home folder: cannot save the appearance."); return; }
    char parent[512]; config_path(parent, sizeof(parent), ""); mkdir(parent, 0700); mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/appearance.conf", dir);
    int n = snprintf(text, sizeof(text), "# Aurora appearance (System Settings > Appearance)\ncolor_scheme=%s\n", SCHEMES[scheme]);
    if (accent >= 0) snprintf(text + n, sizeof(text) - (size_t)n, "accent=#%06X\n", ACCENTS[accent]);
    if (!write_file(path, text)) { char m[200]; snprintf(m, sizeof(m), "Could not save: %s", strerror(errno)); settings_message(m); return; }
    gtk3_prefer_dark(scheme == 2);
    printf("[64] SETTINGS APPEARANCE scheme=%s accent=%s\n", SCHEMES[scheme], accent >= 0 ? ACCENT_NAMES[accent] : "none"); fflush(stdout);
    settings_message("Saved. Applications that follow the desktop portal change at once."); settings_redraw();
}
static void enter(void) { load(); }
static void draw(int x, int y, int w, int h) {
    (void)w; (void)h; page_y = y;
    appui_px_text(x + 20, y + 14, "Appearance", APPUI_F_TITLE, APPUI_TEXT);
    scheme_y = y + 64; appui_px_text(x + 20, scheme_y + 6, "Style", APPUI_F_BODY, APPUI_TEXT);
    appui_px_segment(x + 220, scheme_y, 300, 28, SCHEME_LABELS, 3, scheme);
    accent_y = scheme_y + 56; appui_px_text(x + 20, accent_y + 6, "Accent colour", APPUI_F_BODY, APPUI_TEXT);
    for (int i = 0; i < 7; i++) {
        int cx = x + 236 + i * 44, cy = accent_y + 14;
        if (i == accent) ui_circle(cx, cy, 17, 0xFFFFFF, 220);
        ui_circle(cx, cy, 14, ACCENTS[i], 255);
    }
    appui_px_text(x + 20, accent_y + 52, accent >= 0 ? ACCENT_NAMES[accent] : "Accent: application default", APPUI_F_SMALL, APPUI_SUBTLE);
    appui_px_text(x + 20, accent_y + 84, "Chrome, Firefox and GTK applications read these through the desktop portal.", APPUI_F_SMALL, APPUI_SUBTLE);
    static int reported = -1;
    if (reported != scheme_y - y) { reported = scheme_y - y; printf("[64] SETTINGS APPEARANCE GRID scheme_x=220 scheme_y=%d scheme_w=300 accent_x=236 accent_y=%d accent_step=44\n", scheme_y - y + 14, accent_y - y + 14); fflush(stdout); }
}
static void click(int x, int y) {
    int sy = y - (scheme_y - page_y), ay = y - (accent_y - page_y);
    if (x >= 220 && x < 520 && sy >= 0 && sy < 28) { scheme = (x - 220) * 3 / 300; save(); return; }
    if (ay >= 0 && ay < 28) for (int i = 0; i < 7; i++) if (abs(x - (236 + i * 44)) <= 16) { accent = i; save(); return; }
}
static void key(char k) {
    unsigned char c = (unsigned char)k;
    if (c == 0x82 && scheme > 0) { scheme--; save(); } else if (c == 0x83 && scheme < 2) { scheme++; save(); }
    else if (k >= '1' && k <= '7') { accent = k - '1'; save(); }
}
const struct settings_page settings_appearance = {"appearance", "Appearance", ICON_APPEARANCE, enter, draw, click, NULL, key, NULL};
