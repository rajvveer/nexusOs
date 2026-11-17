/* ============================================================================
 * NexusOS — VGA Text Mode Driver (Header)
 * ============================================================================
 * 80x25 color text mode at memory address 0xB8000.
 * Each character = 2 bytes: [ASCII char] [color attribute]
 * ============================================================================ */

#ifndef VGA_H
#define VGA_H

#include "types.h"

/* VGA dimensions */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

/* VGA Colors */
enum vga_color {
    VGA_BLACK        = 0,
    VGA_BLUE         = 1,
    VGA_GREEN        = 2,
    VGA_CYAN         = 3,
    VGA_RED          = 4,
    VGA_MAGENTA      = 5,
    VGA_BROWN        = 6,
    VGA_LIGHT_GREY   = 7,
    VGA_DARK_GREY    = 8,
    VGA_LIGHT_BLUE   = 9,
    VGA_LIGHT_GREEN  = 10,
    VGA_LIGHT_CYAN   = 11,
    VGA_LIGHT_RED    = 12,
    VGA_LIGHT_MAGENTA= 13,
    VGA_YELLOW       = 14,
    VGA_WHITE        = 15,
};

/* Create color attribute byte: foreground + background */
#define VGA_COLOR(fg, bg) ((bg << 4) | fg)

/* Default color: light grey on black */
#define VGA_DEFAULT_COLOR VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK)

/* Functions */
void vga_init(void);
void vga_clear(void);
void vga_putchar(char c);
void vga_print(const char* str);
void vga_print_color(const char* str, uint8_t color);
void vga_print_at(const char* str, int row, int col);
void vga_set_color(uint8_t color);
void vga_newline(void);
void vga_backspace(void);
void vga_update_cursor(void);
int vga_get_cursor_row(void);
int vga_get_cursor_col(void);

/* Phase 3 additions */
void vga_putchar_at(char c, int row, int col, uint8_t color);
void vga_set_cursor(int row, int col);
void vga_update_statusbar(const char* left, const char* center, const char* right);
void vga_clear_rows(int start_row, int end_row);

/* Phase 14: Switch to VESA framebuffer rendering */
void vga_reinit_vesa(void);

/* Phase 14: Explicitly flush framebuffer to screen */
void vga_flush(void);

#endif /* VGA_H */
