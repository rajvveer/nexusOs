/* ============================================================================
 * NexusOS — VGA Text Mode Driver (Implementation) — Phase 14
 * ============================================================================
 * Dual-mode: writes to VGA text buffer (0xB8000) in text mode,
 * or renders via bitmap font on VESA framebuffer in graphics mode.
 * ============================================================================ */

#include "vga.h"
#include "port.h"
#include "string.h"
#include "framebuffer.h"
#include "font.h"

/* Scrollable area = rows 0-23, row 24 = status bar */
#define SCROLL_HEIGHT 24

/* VGA state */
static uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;
static int cursor_row = 0;
static int cursor_col = 0;
static uint8_t current_color = VGA_DEFAULT_COLOR;

/* VESA mode flag — set after fb_init() */
static bool vesa_mode = false;

/* No offset — text starts at top-left corner */
#define TEXT_OFFSET_X  0
#define TEXT_OFFSET_Y  0

/* --------------------------------------------------------------------------
 * Make a VGA entry: char + color packed into 16 bits
 * -------------------------------------------------------------------------- */
static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

/* --------------------------------------------------------------------------
 * vesa_render_char: Render a character at (row, col) on framebuffer
 * -------------------------------------------------------------------------- */
static void vesa_render_char(int row, int col, char c, uint8_t color) {
    if (!vesa_mode) return;
    int px = TEXT_OFFSET_X + col * FONT_WIDTH;
    int py = TEXT_OFFSET_Y + row * FONT_HEIGHT;
    uint32_t fg = vga_attr_fg(color);
    uint32_t bg = vga_attr_bg(color);
    font_draw_char(px, py, (uint8_t)c, fg, bg);
}

/* --------------------------------------------------------------------------
 * vesa_scroll_up: Redraw entire screen after a scroll in VESA mode
 * -------------------------------------------------------------------------- */
static void vesa_scroll_up(void) {
    if (!vesa_mode) return;
    /* Redraw all rows from the VGA buffer */
    for (int row = 0; row < SCROLL_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            uint16_t entry = vga_buffer[row * VGA_WIDTH + col];
            char ch = (char)(entry & 0xFF);
            uint8_t color = (uint8_t)(entry >> 8);
            vesa_render_char(row, col, ch, color);
        }
    }
}

/* --------------------------------------------------------------------------
 * vga_init: Initialize VGA and clear screen
 * -------------------------------------------------------------------------- */
void vga_init(void) {
    cursor_row = 0;
    cursor_col = 0;
    current_color = VGA_DEFAULT_COLOR;

    /* Check if VESA is already active */
    vesa_mode = fb_is_vesa();

    vga_clear();
}

/* --------------------------------------------------------------------------
 * vga_clear: Clear entire screen
 * -------------------------------------------------------------------------- */
void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = vga_entry(' ', current_color);
    }
    cursor_row = 0;
    cursor_col = 0;

    if (vesa_mode) {
        /* Clear the framebuffer to background color */
        uint32_t bg = vga_attr_bg(current_color);
        fb_clear(bg);
    } else {
        vga_update_cursor();
    }
}

/* --------------------------------------------------------------------------
 * scroll: Scroll screen up by one line
 * -------------------------------------------------------------------------- */
static void scroll(void) {
    /* Move all rows up by one (only scrollable area) */
    for (int i = 0; i < (SCROLL_HEIGHT - 1) * VGA_WIDTH; i++) {
        vga_buffer[i] = vga_buffer[i + VGA_WIDTH];
    }
    /* Clear the last scrollable row */
    for (int i = (SCROLL_HEIGHT - 1) * VGA_WIDTH; i < SCROLL_HEIGHT * VGA_WIDTH; i++) {
        vga_buffer[i] = vga_entry(' ', current_color);
    }
    cursor_row = SCROLL_HEIGHT - 1;

    /* In VESA mode, redraw all visible rows */
    vesa_scroll_up();
}

/* --------------------------------------------------------------------------
 * vga_putchar: Print a single character at cursor position
 * -------------------------------------------------------------------------- */
void vga_putchar(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\t') {
        cursor_col = (cursor_col + 4) & ~3;  /* Align to 4 */
    } else if (c == '\b') {
        vga_backspace();
        return;
    } else {
        int offset = cursor_row * VGA_WIDTH + cursor_col;
        vga_buffer[offset] = vga_entry(c, current_color);
        vesa_render_char(cursor_row, cursor_col, c, current_color);
        cursor_col++;
    }

    /* Wrap at end of line */
    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
    }

    /* Scroll if past scrollable bottom */
    if (cursor_row >= SCROLL_HEIGHT) {
        scroll();
    }

    if (!vesa_mode) {
        vga_update_cursor();
    }
}

/* --------------------------------------------------------------------------
 * vga_print: Print a null-terminated string
 * -------------------------------------------------------------------------- */
void vga_print(const char* str) {
    while (*str) {
        vga_putchar(*str++);
    }

}

/* --------------------------------------------------------------------------
 * vga_print_color: Print string with specific color, then restore
 * -------------------------------------------------------------------------- */
void vga_print_color(const char* str, uint8_t color) {
    uint8_t old_color = current_color;
    current_color = color;
    vga_print(str);
    current_color = old_color;
}

/* --------------------------------------------------------------------------
 * vga_print_at: Print string at specific row/col
 * -------------------------------------------------------------------------- */
void vga_print_at(const char* str, int row, int col) {
    cursor_row = row;
    cursor_col = col;
    vga_print(str);
}

/* --------------------------------------------------------------------------
 * vga_set_color: Change current text color
 * -------------------------------------------------------------------------- */
void vga_set_color(uint8_t color) {
    current_color = color;
}

/* --------------------------------------------------------------------------
 * vga_newline: Move to next line
 * -------------------------------------------------------------------------- */
void vga_newline(void) {
    vga_putchar('\n');
}

/* --------------------------------------------------------------------------
 * vga_backspace: Delete character before cursor
 * -------------------------------------------------------------------------- */
void vga_backspace(void) {
    if (cursor_col > 0) {
        cursor_col--;
    } else if (cursor_row > 0) {
        cursor_row--;
        cursor_col = VGA_WIDTH - 1;
    }
    int offset = cursor_row * VGA_WIDTH + cursor_col;
    vga_buffer[offset] = vga_entry(' ', current_color);
    vesa_render_char(cursor_row, cursor_col, ' ', current_color);

    if (!vesa_mode) {
        vga_update_cursor();
    }
}

/* --------------------------------------------------------------------------
 * vga_update_cursor: Move hardware cursor to match our position
 * -------------------------------------------------------------------------- */
void vga_update_cursor(void) {
    if (vesa_mode) return;  /* No hardware cursor in VESA mode */

    uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;
    port_byte_out(0x3D4, 0x0F);
    port_byte_out(0x3D5, (uint8_t)(pos & 0xFF));
    port_byte_out(0x3D4, 0x0E);
    port_byte_out(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* --------------------------------------------------------------------------
 * Getters for cursor position
 * -------------------------------------------------------------------------- */
int vga_get_cursor_row(void) { return cursor_row; }
int vga_get_cursor_col(void) { return cursor_col; }

/* --------------------------------------------------------------------------
 * vga_putchar_at: Write a character at a specific position with color
 * -------------------------------------------------------------------------- */
void vga_putchar_at(char c, int row, int col, uint8_t color) {
    if (row >= 0 && row < VGA_HEIGHT && col >= 0 && col < VGA_WIDTH) {
        int offset = row * VGA_WIDTH + col;
        vga_buffer[offset] = vga_entry(c, color);
        vesa_render_char(row, col, c, color);
    }
}

/* --------------------------------------------------------------------------
 * vga_set_cursor: Set cursor position directly
 * -------------------------------------------------------------------------- */
void vga_set_cursor(int row, int col) {
    cursor_row = row;
    cursor_col = col;
    if (!vesa_mode) {
        vga_update_cursor();
    }
}

/* --------------------------------------------------------------------------
 * vga_update_statusbar: Draw persistent status bar on row 24
 * -------------------------------------------------------------------------- */
void vga_update_statusbar(const char* left, const char* center, const char* right) {
    uint8_t bar_color = VGA_COLOR(VGA_WHITE, VGA_BLUE);
    int bar_row = VGA_HEIGHT - 1;  /* Row 24 */

    /* Fill entire row with blue background */
    for (int i = 0; i < VGA_WIDTH; i++) {
        vga_buffer[bar_row * VGA_WIDTH + i] = vga_entry(' ', bar_color);
        vesa_render_char(bar_row, i, ' ', bar_color);
    }

    /* Left-aligned text */
    if (left) {
        int col = 1;
        while (*left && col < VGA_WIDTH) {
            vga_buffer[bar_row * VGA_WIDTH + col] = vga_entry(*left, bar_color);
            vesa_render_char(bar_row, col, *left, bar_color);
            left++;
            col++;
        }
    }

    /* Center text */
    if (center) {
        int len = 0;
        const char* tmp = center;
        while (*tmp++) len++;
        int col = (VGA_WIDTH - len) / 2;
        if (col < 0) col = 0;
        while (*center && col < VGA_WIDTH) {
            vga_buffer[bar_row * VGA_WIDTH + col] = vga_entry(*center, bar_color);
            vesa_render_char(bar_row, col, *center, bar_color);
            center++;
            col++;
        }
    }

    /* Right-aligned text */
    if (right) {
        int len = 0;
        const char* tmp = right;
        while (*tmp++) len++;
        int col = VGA_WIDTH - len - 1;
        if (col < 0) col = 0;
        while (*right && col < VGA_WIDTH) {
            vga_buffer[bar_row * VGA_WIDTH + col] = vga_entry(*right, bar_color);
            vesa_render_char(bar_row, col, *right, bar_color);
            right++;
            col++;
        }
    }

    if (vesa_mode) {
        fb_flip();
    }
}

/* --------------------------------------------------------------------------
 * vga_clear_rows: Clear specific rows
 * -------------------------------------------------------------------------- */
void vga_clear_rows(int start_row, int end_row) {
    for (int row = start_row; row <= end_row && row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            vga_buffer[row * VGA_WIDTH + col] = vga_entry(' ', current_color);
            vesa_render_char(row, col, ' ', current_color);
        }
    }
}

/* --------------------------------------------------------------------------
 * vga_reinit_vesa: Switch to VESA framebuffer rendering mode
 * Called after fb_init() to enable pixel rendering for all VGA output.
 * Draws a gradient border and redraws current text buffer on framebuffer.
 * -------------------------------------------------------------------------- */
void vga_reinit_vesa(void) {
    vesa_mode = fb_is_vesa();
    if (!vesa_mode) return;

    /* Clear framebuffer to black */
    fb_clear(0x000000);

    /* Redraw everything currently in the text buffer onto framebuffer */
    for (int row = 0; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            uint16_t entry = vga_buffer[row * VGA_WIDTH + col];
            char ch = (char)(entry & 0xFF);
            uint8_t color = (uint8_t)(entry >> 8);
            vesa_render_char(row, col, ch, color);
        }
    }

    fb_flip();
}

/* --------------------------------------------------------------------------
 * vga_flush: Explicitly flush the framebuffer (for VESA mode)
 * Call this when you want the screen to update immediately.
 * -------------------------------------------------------------------------- */
void vga_flush(void) {
    if (vesa_mode) {
        fb_flip();
    }
}

