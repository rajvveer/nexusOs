/* ============================================================================
 * NexusOS — Paint App (Implementation)
 * ============================================================================
 * ASCII art drawing with arrow keys, character selection, and colors.
 * ============================================================================ */

#include "paint.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

#define PT_W 24
#define PT_H 10

static char   pt_canvas[PT_H][PT_W];
static uint8_t pt_colors[PT_H][PT_W];
static int    pt_cx = 0, pt_cy = 0;  /* cursor */
static char   pt_brush = '#';
static uint8_t pt_fg = VGA_WHITE;
static uint8_t pt_bg_color = VGA_BLACK;

static const char brushes[] = "#*@.+$%&!~0123456789";
static int brush_idx = 0;

static void pt_init(void) {
    for (int y = 0; y < PT_H; y++)
        for (int x = 0; x < PT_W; x++) {
            pt_canvas[y][x] = ' ';
            pt_colors[y][x] = VGA_COLOR(VGA_WHITE, VGA_BLACK);
        }
    pt_cx = 0; pt_cy = 0;
    pt_brush = '#'; brush_idx = 0;
    pt_fg = VGA_WHITE; pt_bg_color = VGA_BLACK;
}

static void pt_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);

    int row = cy;

    /* Title + brush info */
    gui_draw_text(cx, row, "\x0F Paint", accent);
    char info[20];
    strcpy(info, " Brush:");
    info[7] = pt_brush; info[8] = '\0';
    strcat(info, " C:");
    char cn[3]; int_to_str(pt_fg, cn);
    strcat(info, cn);
    gui_draw_text(cx + 8, row, info, dim);
    row++;

    /* Canvas border */
    gui_putchar(cx, row, (char)0xDA, dim);
    for (int i = 0; i < PT_W; i++) gui_putchar(cx + 1 + i, row, (char)0xC4, dim);
    gui_putchar(cx + PT_W + 1, row, (char)0xBF, dim);
    row++;

    /* Canvas */
    for (int y = 0; y < PT_H && row < cy + ch - 2; y++) {
        gui_putchar(cx, row, (char)0xB3, dim);
        for (int x = 0; x < PT_W; x++) {
            bool at_cursor = (x == pt_cx && y == pt_cy);
            uint8_t col = at_cursor ? VGA_COLOR(VGA_BLACK, VGA_WHITE) : pt_colors[y][x];
            gui_putchar(cx + 1 + x, row, pt_canvas[y][x], col);
        }
        gui_putchar(cx + PT_W + 1, row, (char)0xB3, dim);
        row++;
    }

    gui_putchar(cx, row, (char)0xC0, dim);
    for (int i = 0; i < PT_W; i++) gui_putchar(cx + 1 + i, row, (char)0xC4, dim);
    gui_putchar(cx + PT_W + 1, row, (char)0xD9, dim);
    row++;

    gui_draw_text(cx, row, "Space:paint C:color B:brush", dim);
    (void)cw; (void)ch;
}

static void pt_key(int id, char key) {
    (void)id;

    /* Arrow keys */
    if ((unsigned char)key == 0x80 && pt_cy > 0) pt_cy--;
    if ((unsigned char)key == 0x81 && pt_cy < PT_H - 1) pt_cy++;
    if ((unsigned char)key == 0x82 && pt_cx > 0) pt_cx--;
    if ((unsigned char)key == 0x83 && pt_cx < PT_W - 1) pt_cx++;

    /* Space: paint */
    if (key == ' ') {
        pt_canvas[pt_cy][pt_cx] = pt_brush;
        pt_colors[pt_cy][pt_cx] = VGA_COLOR(pt_fg, pt_bg_color);
    }

    /* Backspace: erase */
    if (key == '\b') {
        pt_canvas[pt_cy][pt_cx] = ' ';
        pt_colors[pt_cy][pt_cx] = VGA_COLOR(VGA_WHITE, VGA_BLACK);
    }

    /* C: cycle foreground color */
    if (key == 'c' || key == 'C') {
        pt_fg = (pt_fg + 1) & 0x0F;
    }

    /* B: cycle brush */
    if (key == 'b' || key == 'B') {
        brush_idx = (brush_idx + 1) % (int)strlen(brushes);
        pt_brush = brushes[brush_idx];
    }

    /* F: fill canvas with current brush */
    if (key == 'f' || key == 'F') {
        for (int y = 0; y < PT_H; y++)
            for (int x = 0; x < PT_W; x++) {
                pt_canvas[y][x] = pt_brush;
                pt_colors[y][x] = VGA_COLOR(pt_fg, pt_bg_color);
            }
    }

    /* X: clear canvas */
    if (key == 'x' || key == 'X') { pt_init(); }
}

int paint_open(void) {
    pt_init();
    return window_create("Paint", 14, 2, PT_W + 4, PT_H + 5, pt_draw, pt_key);
}
