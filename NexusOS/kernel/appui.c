/* ============================================================================
 * NexusOS - App UI Helpers
 * ============================================================================ */

#include "appui.h"
#include "gui.h"
#include "vga.h"
#include "string.h"

appui_theme_t appui_theme(void) {
    appui_theme_t t;
    t.bg = VGA_BLACK;
    t.panel = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);
    t.toolbar = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY);
    t.text = VGA_COLOR(VGA_WHITE, VGA_BLACK);
    t.muted = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK);
    t.accent = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK);
    t.good = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK);
    t.warn = VGA_COLOR(VGA_YELLOW, VGA_BLACK);
    t.danger = VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK);
    t.selected = VGA_COLOR(VGA_WHITE, VGA_BLUE);
    t.status = VGA_COLOR(VGA_LIGHT_GREY, VGA_DARK_GREY);
    return t;
}

void appui_fill(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            gui_putchar(x + col, y + row, ' ', color);
}

void appui_text(int x, int y, const char* text, int max, uint8_t color) {
    if (!text || max <= 0) return;
    for (int i = 0; text[i] && i < max; i++)
        gui_putchar(x + i, y, text[i], color);
}

void appui_hline(int x, int y, int w, uint8_t color) {
    if (w <= 0) return;
    for (int i = 0; i < w; i++)
        gui_putchar(x + i, y, (char)0xC4, color);
}

void appui_header(int x, int y, int w, const char* title, const char* subtitle) {
    appui_theme_t t = appui_theme();
    appui_fill(x, y, w, 2, t.toolbar);
    appui_text(x + 1, y, title, w - 2, t.toolbar);
    if (subtitle) appui_text(x + 1, y + 1, subtitle, w - 2, t.status);
}

void appui_status(int x, int y, int w, const char* text) {
    appui_theme_t t = appui_theme();
    appui_fill(x, y, w, 1, t.status);
    appui_text(x + 1, y, text, w - 2, t.status);
}

void appui_row(int x, int y, int w, bool selected, uint8_t normal, uint8_t selected_color) {
    appui_fill(x, y, w, 1, selected ? selected_color : normal);
}

void appui_kv(int x, int y, const char* key, const char* value, int value_x, int max) {
    appui_theme_t t = appui_theme();
    appui_text(x, y, key, value_x - x - 1, t.muted);
    appui_text(value_x, y, value, max, t.text);
}

void appui_input(int x, int y, int w, const char* text, int len, bool cursor) {
    appui_theme_t t = appui_theme();
    appui_fill(x, y, w, 1, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    appui_text(x + 1, y, text, w - 3, t.text);
    if (cursor && len < w - 2)
        gui_putchar(x + 1 + len, y, '_', VGA_COLOR(VGA_WHITE, VGA_BLACK));
}
