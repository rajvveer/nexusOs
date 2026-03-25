/* ============================================================================
 * NexusOS — Color Picker (Implementation)
 * ============================================================================ */

#include "colorpick.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

static int cp_fg = VGA_WHITE, cp_bg = VGA_BLACK;
static int cp_mode = 0; /* 0=fg, 1=bg */
static int cp_sel = 0;

static const char* color_names[] = {
    "Black","Blue","Green","Cyan","Red","Magenta","Brown","LtGrey",
    "DkGrey","LtBlue","LtGreen","LtCyan","LtRed","LtMagenta","Yellow","White"
};

static void cp_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);

    int row = cy;
    gui_draw_text(cx, row, "\xFE Color Picker", accent); row++;
    gui_draw_text(cx, row, cp_mode == 0 ? "Mode: Foreground" : "Mode: Background",
        VGA_COLOR(VGA_YELLOW, bg)); row++;

    /* Color grid: 8x2 */
    for (int i = 0; i < 16; i++) {
        int gx = cx + 1 + (i % 8) * 3;
        int gy = row + (i / 8) * 2;
        bool sel = (i == cp_sel);
        gui_putchar(gx, gy, (char)0xDB, VGA_COLOR(i, i));
        gui_putchar(gx + 1, gy, (char)0xDB, VGA_COLOR(i, i));
        if (sel) {
            gui_putchar(gx - 1, gy, '[', accent);
            gui_putchar(gx + 2, gy, ']', accent);
        }
    }
    row += 5;

    /* Selected color name */
    gui_draw_text(cx, row, "Color: ", dim);
    gui_draw_text(cx + 7, row, color_names[cp_sel], VGA_COLOR(cp_sel, bg)); row++;

    /* Preview */
    row++;
    gui_draw_text(cx, row, "Preview: ", dim);
    uint8_t prev = VGA_COLOR(cp_fg, cp_bg);
    gui_draw_text(cx + 9, row, " Sample Text ", prev); row++;

    /* Current values */
    row++;
    char fg_s[20]; strcpy(fg_s, "FG: "); strcat(fg_s, color_names[cp_fg]);
    gui_draw_text(cx, row, fg_s, VGA_COLOR(cp_fg, bg)); row++;
    char bg_s[20]; strcpy(bg_s, "BG: "); strcat(bg_s, color_names[cp_bg]);
    gui_draw_text(cx, row, bg_s, VGA_COLOR(cp_bg, bg)); row++;

    gui_draw_text(cx, cy + ch - 1, "Tab:FG/BG Enter:Set", dim);
    (void)cw; (void)ch;
}

static void cp_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x82 && cp_sel > 0) cp_sel--;
    if ((unsigned char)key == 0x83 && cp_sel < 15) cp_sel++;
    if ((unsigned char)key == 0x80 && cp_sel >= 8) cp_sel -= 8;
    if ((unsigned char)key == 0x81 && cp_sel < 8) cp_sel += 8;
    if (key == '\t') cp_mode = !cp_mode;
    if (key == '\n') { if (cp_mode == 0) cp_fg = cp_sel; else cp_bg = cp_sel; }
}

int colorpick_open(void) {
    cp_sel = 0; cp_mode = 0;
    return window_create("Color Picker", 20, 3, 28, 16, cp_draw, cp_key);
}
