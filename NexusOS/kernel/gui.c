/* ============================================================================
 * NexusOS — GUI Framebuffer & Drawing Primitives (Implementation) — Phase 14
 * ============================================================================
 * Compatibility shim layer: all existing apps call gui_putchar/gui_draw_text
 * using character-cell coordinates (80x25). In VESA mode, these are translated
 * to pixel operations via the bitmap font renderer on the framebuffer.
 *
 * Text mode path: writes to VGA back buffer at 0xB8000 (original behavior).
 * VESA mode path: renders characters as 8x16 pixel bitmaps on framebuffer.
 * ============================================================================ */

#include "gui.h"
#include "vga.h"
#include "string.h"
#include "framebuffer.h"
#include "font.h"

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

/* Text-mode back buffer (used in both modes for getchar compatibility) */
static uint16_t backbuf[GUI_WIDTH * GUI_HEIGHT];

/* VGA memory pointer (text mode only) */
static uint16_t* vga_mem = (uint16_t*)0xB8000;

/* Is VESA active? */
static bool using_vesa = false;

/* --------------------------------------------------------------------------
 * make_entry: Pack char + color into VGA-format 16-bit word
 * -------------------------------------------------------------------------- */
static uint16_t make_entry(char ch, uint8_t color) {
    return (uint16_t)(uint8_t)ch | ((uint16_t)color << 8);
}

/* --------------------------------------------------------------------------
 * gui_init: Initialize the GUI framebuffer
 * -------------------------------------------------------------------------- */
void gui_init(void) {
    using_vesa = fb_is_vesa();

    if (using_vesa) {
        /* Clear framebuffer to black */
        fb_clear(FB_COLOR_BLACK);
    }

    gui_clear(' ', VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * gui_is_vesa: Check if we're in VESA graphics mode
 * -------------------------------------------------------------------------- */
bool gui_is_vesa(void) {
    return using_vesa;
}

/* --------------------------------------------------------------------------
 * gui_get_backbuf: Get pointer to the character back buffer
 * -------------------------------------------------------------------------- */
uint16_t* gui_get_backbuf(void) {
    return backbuf;
}

/* --------------------------------------------------------------------------
 * gui_clear: Fill entire back buffer with character and color
 * -------------------------------------------------------------------------- */
void gui_clear(char ch, uint8_t color) {
    uint16_t entry = make_entry(ch, color);
    for (int i = 0; i < GUI_WIDTH * GUI_HEIGHT; i++) {
        backbuf[i] = entry;
    }

    if (using_vesa) {
        /* Fill entire screen with background color */
        uint32_t bg = vga_attr_bg(color);
        fb_clear(bg);

        /* If fill char is not space, render it everywhere */
        if (ch != ' ') {
            uint32_t fg = vga_attr_fg(color);
            for (int y = 0; y < GUI_HEIGHT; y++) {
                for (int x = 0; x < GUI_WIDTH; x++) {
                    int px = GUI_OFFSET_X + x * GUI_CELL_W;
                    int py = GUI_OFFSET_Y + y * GUI_CELL_H;
                    font_draw_char(px, py, (uint8_t)ch, fg, bg);
                }
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * gui_putchar: Write a character at (x, y) in the back buffer
 * -------------------------------------------------------------------------- */
void gui_putchar(int x, int y, char ch, uint8_t color) {
    if (x < 0 || x >= GUI_WIDTH || y < 0 || y >= GUI_HEIGHT) return;

    backbuf[y * GUI_WIDTH + x] = make_entry(ch, color);

    if (using_vesa) {
        int px = GUI_OFFSET_X + x * GUI_CELL_W;
        int py = GUI_OFFSET_Y + y * GUI_CELL_H;
        uint32_t fg = vga_attr_fg(color);
        uint32_t bg = vga_attr_bg(color);
        font_draw_char(px, py, (uint8_t)ch, fg, bg);
    }
}

/* --------------------------------------------------------------------------
 * gui_getchar: Read the 16-bit entry at (x, y) from back buffer
 * -------------------------------------------------------------------------- */
uint16_t gui_getchar(int x, int y) {
    if (x >= 0 && x < GUI_WIDTH && y >= 0 && y < GUI_HEIGHT) {
        return backbuf[y * GUI_WIDTH + x];
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * gui_draw_text: Draw a null-terminated string at (x, y)
 * -------------------------------------------------------------------------- */
void gui_draw_text(int x, int y, const char* text, uint8_t color) {
    while (*text && x < GUI_WIDTH) {
        gui_putchar(x, y, *text, color);
        text++;
        x++;
    }
}

/* --------------------------------------------------------------------------
 * gui_draw_box: Draw a single-line box outline
 * -------------------------------------------------------------------------- */
void gui_draw_box(gui_rect_t rect, uint8_t color) {
    int x1 = rect.x;
    int y1 = rect.y;
    int x2 = rect.x + rect.w - 1;
    int y2 = rect.y + rect.h - 1;

    /* Corners */
    gui_putchar(x1, y1, (char)BOX_TOP_LEFT, color);
    gui_putchar(x2, y1, (char)BOX_TOP_RIGHT, color);
    gui_putchar(x1, y2, (char)BOX_BOT_LEFT, color);
    gui_putchar(x2, y2, (char)BOX_BOT_RIGHT, color);

    /* Horizontal edges */
    for (int x = x1 + 1; x < x2; x++) {
        gui_putchar(x, y1, (char)BOX_HORIZ, color);
        gui_putchar(x, y2, (char)BOX_HORIZ, color);
    }

    /* Vertical edges */
    for (int y = y1 + 1; y < y2; y++) {
        gui_putchar(x1, y, (char)BOX_VERT, color);
        gui_putchar(x2, y, (char)BOX_VERT, color);
    }
}

/* --------------------------------------------------------------------------
 * gui_draw_box_double: Draw a double-line box outline
 * -------------------------------------------------------------------------- */
void gui_draw_box_double(gui_rect_t rect, uint8_t color) {
    int x1 = rect.x;
    int y1 = rect.y;
    int x2 = rect.x + rect.w - 1;
    int y2 = rect.y + rect.h - 1;

    /* Corners */
    gui_putchar(x1, y1, (char)BOX2_TOP_LEFT, color);
    gui_putchar(x2, y1, (char)BOX2_TOP_RIGHT, color);
    gui_putchar(x1, y2, (char)BOX2_BOT_LEFT, color);
    gui_putchar(x2, y2, (char)BOX2_BOT_RIGHT, color);

    /* Horizontal edges */
    for (int x = x1 + 1; x < x2; x++) {
        gui_putchar(x, y1, (char)BOX2_HORIZ, color);
        gui_putchar(x, y2, (char)BOX2_HORIZ, color);
    }

    /* Vertical edges */
    for (int y = y1 + 1; y < y2; y++) {
        gui_putchar(x1, y, (char)BOX2_VERT, color);
        gui_putchar(x2, y, (char)BOX2_VERT, color);
    }
}

/* --------------------------------------------------------------------------
 * gui_fill_rect: Fill a rectangle with a character and color
 * -------------------------------------------------------------------------- */
void gui_fill_rect(gui_rect_t rect, char ch, uint8_t color) {
    for (int y = rect.y; y < rect.y + rect.h; y++) {
        for (int x = rect.x; x < rect.x + rect.w; x++) {
            gui_putchar(x, y, ch, color);
        }
    }
}

/* --------------------------------------------------------------------------
 * gui_draw_hline: Draw a horizontal line
 * -------------------------------------------------------------------------- */
void gui_draw_hline(int x, int y, int len, char ch, uint8_t color) {
    for (int i = 0; i < len; i++) {
        gui_putchar(x + i, y, ch, color);
    }
}

/* --------------------------------------------------------------------------
 * gui_draw_vline: Draw a vertical line
 * -------------------------------------------------------------------------- */
void gui_draw_vline(int x, int y, int len, char ch, uint8_t color) {
    for (int i = 0; i < len; i++) {
        gui_putchar(x, y + i, ch, color);
    }
}

/* --------------------------------------------------------------------------
 * gui_flip: Copy back buffer to display (present frame)
 * -------------------------------------------------------------------------- */
void gui_flip(void) {
    if (using_vesa) {
        fb_flip();
    } else {
        /* Text mode: only copy the 80×25 visible area from the larger backbuf */
        for (int y = 0; y < 25 && y < GUI_HEIGHT; y++) {
            for (int x = 0; x < 80 && x < GUI_WIDTH; x++) {
                vga_mem[y * 80 + x] = backbuf[y * GUI_WIDTH + x];
            }
        }
    }
}
