/* ============================================================================
 * NexusOS — GUI Framebuffer & Drawing Primitives (Header) — Phase 14
 * ============================================================================
 * Compatibility shim: works in both VGA text mode AND VESA pixel mode.
 * In VESA mode, character-cell operations (80x25) are rendered as pixels
 * using the 8x16 bitmap font on the 1024x768 framebuffer.
 * ============================================================================ */

#ifndef GUI_H
#define GUI_H

#include "types.h"

/* Screen dimensions in character cells (fills full 1024x768 in VESA) */
#define GUI_WIDTH  128
#define GUI_HEIGHT 48

/* Character cell size in pixels (for VESA mode) */
#define GUI_CELL_W  8
#define GUI_CELL_H  16

/* VESA screen dimensions in pixels */
#define GUI_PIXEL_WIDTH   1024
#define GUI_PIXEL_HEIGHT  768

/* No offset — text starts at top-left of screen */
#define GUI_OFFSET_X  0
#define GUI_OFFSET_Y  0

/* Rectangle type */
typedef struct {
    int x, y, w, h;
} gui_rect_t;

/* --------------------------------------------------------------------------
 * Box-drawing characters (CP437 code page, used by VGA text mode)
 * -------------------------------------------------------------------------- */

/* Single-line box characters */
#define BOX_HORIZ       0xC4  /* ─ */
#define BOX_VERT        0xB3  /* │ */
#define BOX_TOP_LEFT    0xDA  /* ┌ */
#define BOX_TOP_RIGHT   0xBF  /* ┐ */
#define BOX_BOT_LEFT    0xC0  /* └ */
#define BOX_BOT_RIGHT   0xD9  /* ┘ */
#define BOX_TEE_LEFT    0xC3  /* ├ */
#define BOX_TEE_RIGHT   0xB4  /* ┤ */
#define BOX_TEE_TOP     0xC2  /* ┬ */
#define BOX_TEE_BOT     0xC1  /* ┴ */
#define BOX_CROSS       0xC5  /* ┼ */

/* Double-line box characters */
#define BOX2_HORIZ      0xCD  /* ═ */
#define BOX2_VERT       0xBA  /* ║ */
#define BOX2_TOP_LEFT   0xC9  /* ╔ */
#define BOX2_TOP_RIGHT  0xBB  /* ╗ */
#define BOX2_BOT_LEFT   0xC8  /* ╚ */
#define BOX2_BOT_RIGHT  0xBC  /* ╝ */

/* Shade characters for backgrounds */
#define SHADE_LIGHT     0xB0  /* ░ */
#define SHADE_MEDIUM    0xB1  /* ▒ */
#define SHADE_DARK      0xB2  /* ▓ */
#define SHADE_FULL      0xDB  /* █ */

/* UI symbols */
#define SYM_ARROW_RIGHT 0x10  /* ► */
#define SYM_ARROW_LEFT  0x11  /* ◄ */
#define SYM_ARROW_UP    0x1E  /* ▲ */
#define SYM_ARROW_DOWN  0x1F  /* ▼ */
#define SYM_BULLET      0x07  /* • */
#define SYM_DIAMOND     0x04  /* ♦ */
#define SYM_CHECK       0xFB  /* √ */

/* --------------------------------------------------------------------------
 * Core GUI functions
 * -------------------------------------------------------------------------- */

/* Initialize the GUI subsystem (detects VESA or text mode) */
void gui_init(void);

/* Clear back buffer with a character and color */
void gui_clear(char ch, uint8_t color);

/* Put a character at position in back buffer */
void gui_putchar(int x, int y, char ch, uint8_t color);

/* Read the character at a position in the back buffer */
uint16_t gui_getchar(int x, int y);

/* Draw a text string at position */
void gui_draw_text(int x, int y, const char* text, uint8_t color);

/* Draw a single-line box outline */
void gui_draw_box(gui_rect_t rect, uint8_t color);

/* Draw a double-line box outline */
void gui_draw_box_double(gui_rect_t rect, uint8_t color);

/* Fill a rectangular area with a character */
void gui_fill_rect(gui_rect_t rect, char ch, uint8_t color);

/* Draw horizontal line */
void gui_draw_hline(int x, int y, int len, char ch, uint8_t color);

/* Draw vertical line */
void gui_draw_vline(int x, int y, int len, char ch, uint8_t color);

/* Flip: copy back buffer to display */
void gui_flip(void);

/* Check if we're in VESA graphics mode */
bool gui_is_vesa(void);

#endif /* GUI_H */
