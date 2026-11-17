/* ============================================================================
 * NexusOS — Appearance Panel (Implementation)
 * ============================================================================
 * Theme, wallpaper pattern, and accent color selection.
 * ============================================================================ */

#include "appearance.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "wallpaper.h"
#include "appui.h"

static int ap_section = 0; /* 0=theme, 1=wallpaper, 2=preview */
static int ap_sel = 0;

static const char* theme_names[] = { "Dark", "Light", "Retro", "Ocean", "HighContrast" };
static const char* wp_patterns[] = { "Dots", "Lines", "Grid", "Gradient", "Stars", "Waves" };
#define THEME_COUNT 5
#define WP_COUNT 6

static void ap_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    appui_theme_t ui = appui_theme();
    uint8_t tc = ui.text, bg = ui.bg;
    uint8_t dim = ui.muted;
    uint8_t accent = ui.accent;
    uint8_t hi = t->menu_highlight;

    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "Appearance", "Themes, wallpaper, and color preview");
    int row = cy + 3;

    /* Section tabs */
    const char* tabs[] = {"Theme", "Wallpaper", "Preview"};
    for (int i = 0; i < 3; i++) {
        uint8_t col = (i == ap_section) ? VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN) : dim;
        gui_putchar(cx + i * 10, row, '[', col);
        gui_draw_text(cx + i * 10 + 1, row, tabs[i], col);
        gui_putchar(cx + i * 10 + 1 + (int)strlen(tabs[i]), row, ']', col);
    }
    row += 2;

    if (ap_section == 0) {
        gui_draw_text(cx, row, "Select Theme:", accent); row++;
        for (int i = 0; i < THEME_COUNT; i++) {
            bool sel = (i == ap_sel);
            uint8_t col = sel ? hi : tc;
            if (sel) appui_row(cx, row, cw, true, ui.panel, hi);
            gui_putchar(cx + 1, row, sel ? '\x10' : ' ', VGA_COLOR(VGA_LIGHT_CYAN, sel ? ((hi >> 4) & 0xF) : bg));
            gui_draw_text(cx + 3, row, theme_names[i], col);
            row++;
        }
        appui_status(cx, cy + ch - 1, cw, "Enter Apply   Tab Section");
    }
    else if (ap_section == 1) {
        gui_draw_text(cx, row, "Wallpaper Pattern:", accent); row++;
        for (int i = 0; i < WP_COUNT; i++) {
            bool sel = (i == ap_sel);
            uint8_t col = sel ? hi : tc;
            if (sel) appui_row(cx, row, cw, true, ui.panel, hi);
            gui_putchar(cx + 1, row, sel ? '\x10' : ' ', VGA_COLOR(VGA_LIGHT_CYAN, sel ? ((hi >> 4) & 0xF) : bg));
            gui_draw_text(cx + 3, row, wp_patterns[i], col);
            row++;
        }
        appui_status(cx, cy + ch - 1, cw, "Enter Apply   Tab Section");
    }
    else {
        gui_draw_text(cx, row, "Current Settings:", accent); row++;
        row++;
        gui_draw_text(cx + 1, row, "Theme:", dim);
        gui_draw_text(cx + 9, row, theme_names[theme_get_index()], tc); row++;
        row++;
        /* Color preview */
        gui_draw_text(cx + 1, row, "Colors:", dim); row++;
        for (int i = 0; i < 16; i++) {
            gui_putchar(cx + 1 + i * 2, row, (char)0xDB, VGA_COLOR(i, i));
        }
        row += 2;
        /* Window preview */
        gui_draw_text(cx + 1, row, "Title:", dim);
        for (int i = 0; i < 12; i++) gui_putchar(cx + 8 + i, row, ' ', t->win_title_active);
        gui_draw_text(cx + 9, row, "Preview", t->win_title_active); row++;
        gui_draw_text(cx + 1, row, "Content:", dim);
        for (int i = 0; i < 12; i++) gui_putchar(cx + 10 + i, row, ' ', tc);
        gui_draw_text(cx + 11, row, "Text", tc);
    }
    (void)cw; (void)ch;
}

static void ap_key(int id, char key) {
    (void)id;
    if (key == '\t') { ap_section = (ap_section + 1) % 3; ap_sel = 0; }
    if ((unsigned char)key == 0x80 && ap_sel > 0) ap_sel--;
    if ((unsigned char)key == 0x81) ap_sel++;

    if (key == '\n') {
        if (ap_section == 0 && ap_sel < THEME_COUNT) {
            /* theme_names[] is index-aligned with the themes[] table; select by
             * index (theme_set_by_name expects the lowercase canonical name,
             * not these display-capitalized strings). */
            theme_set(ap_sel);
        }
        else if (ap_section == 1 && ap_sel < WP_COUNT) {
            wallpaper_set(ap_sel);
        }
    }
}

int appearance_open(void) {
    ap_section = 0; ap_sel = 0;
    return window_create("Appearance", 16, 4, 46, 20, ap_draw, ap_key);
}
