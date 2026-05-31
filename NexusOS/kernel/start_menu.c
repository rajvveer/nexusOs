/* ============================================================================
 * NexusOS — Start Menu (GNOME-style Full-Screen Launcher)
 * ============================================================================
 * In VESA mode: dark translucent full-screen overlay with grid of icon tiles.
 * In text mode: legacy two-column popup.
 * ============================================================================ */

#include "start_menu.h"
#include "gui.h"
#include "gfx.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "framebuffer.h"
#include "font.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

typedef struct {
    const char* label;
    int action;
    uint32_t tile_color;   /* Tile background color (emoji vibe) */
    char glyph;            /* Glyph for text mode fallback / overlay */
    uint8_t icon_color;    /* Text-mode icon color */
} menu_item_t;

static const menu_item_t items[] = {
    { "Terminal",     SMENU_TERMINAL,  FB_RGB( 40,  44,  52), '\xC4', VGA_LIGHT_GREEN   },
    { "Files",        SMENU_FILEMGR,   FB_RGB(255, 200,  60), '\xE8', VGA_YELLOW        },
    { "Calculator",   SMENU_CALC,      FB_RGB( 90, 110, 140), '\xF1', VGA_LIGHT_CYAN    },
    { "Monitor",      SMENU_SYSMON,    FB_RGB(220,  70,  80), '\x0F', VGA_LIGHT_RED     },
    { "Settings",     SMENU_SETTINGS,  FB_RGB(120, 100, 180), '\xF0', VGA_LIGHT_MAGENTA },
    { "Notepad",      SMENU_NOTEPAD,   FB_RGB(240, 240, 240), '\xE9', VGA_WHITE         },
    { "Task Mgr",     SMENU_TASKMGR,   FB_RGB( 70, 140, 220), '\xFE', VGA_LIGHT_BLUE    },
    { "Calendar",     SMENU_CALENDAR,  FB_RGB(100, 180, 220), '\x04', VGA_LIGHT_CYAN    },
    { "Music",        SMENU_MUSIC,     FB_RGB(240, 170,  60), '\x0E', VGA_YELLOW        },
    { "Help",         SMENU_HELP,      FB_RGB(220, 220, 230), '\x3F', VGA_WHITE         },
    { "Paint",        SMENU_PAINT,     FB_RGB( 80, 200, 120), '\xEB', VGA_LIGHT_GREEN   },
    { "Mines",        SMENU_MINESWEEP, FB_RGB(100, 190, 210), '\x0F', VGA_LIGHT_CYAN    },
    { "Trash",        SMENU_RECYCLE,   FB_RGB(255, 180,  50), '\xE8', VGA_YELLOW        },
    { "Sys Info",     SMENU_SYSINFO,   FB_RGB( 80, 130, 200), '\x69', VGA_LIGHT_BLUE    },
    { "Todo",         SMENU_TODO,      FB_RGB( 90, 200, 120), '\xFB', VGA_LIGHT_GREEN   },
    { "Clock",        SMENU_CLOCK,     FB_RGB(240, 200,  70), '\x0F', VGA_YELLOW        },
    { "Pong",         SMENU_PONG,      FB_RGB(120, 200, 200), '\x04', VGA_LIGHT_CYAN    },
    { "Search",       SMENU_SEARCH,    FB_RGB(230, 230, 240), '\x10', VGA_WHITE         },
    { "Tetris",       SMENU_TETRIS,    FB_RGB(100, 190, 200), '\xDB', VGA_LIGHT_CYAN    },
    { "Hex View",     SMENU_HEXVIEW,   FB_RGB(220, 180,  70), '\xFE', VGA_YELLOW        },
    { "Contacts",     SMENU_CONTACTS,  FB_RGB( 90, 200, 130), '\x02', VGA_LIGHT_GREEN   },
    { "Colors",       SMENU_COLORPICK, FB_RGB(180, 100, 200), '\xDB', VGA_LIGHT_MAGENTA },
    { "Log",          SMENU_SYSLOG,    FB_RGB( 80, 130, 210), '\xFE', VGA_LIGHT_BLUE    },
    { "Clipboard",    SMENU_CLIPMGR,   FB_RGB(110, 190, 210), '\xE8', VGA_LIGHT_CYAN    },
    { "Appearance",   SMENU_APPEAR,    FB_RGB(180, 110, 210), '\xFE', VGA_LIGHT_MAGENTA },
    { "File Ops",     SMENU_FILEOPS,   FB_RGB(230, 180,  60), '\xE8', VGA_YELLOW        },
    { "Snake",        SMENU_SNAKE,     FB_RGB( 90, 200, 110), '\x02', VGA_LIGHT_GREEN   },
    { "Browser",      SMENU_BROWSER,   FB_RGB(100, 190, 220), '\xEB', VGA_LIGHT_CYAN    },
};

#define ITEM_COUNT ((int)(sizeof(items) / sizeof(items[0])))

/* Launcher grid layout (VESA) */
#define GRID_COLS 6
#define TILE_W    96
#define TILE_H    96
#define TILE_GAP  32
#define LABEL_GAP 6        /* gap between tile and label */

static bool menu_open = false;
static int  selected  = 0;

void start_menu_toggle(void)  { menu_open = !menu_open; if (menu_open) selected = 0; }
void start_menu_close(void)   { menu_open = false; }
bool start_menu_is_open(void) { return menu_open; }

/* --------------------------------------------------------------------------
 * Draw a recognizable icon shape inside a tile of size TILE_W x TILE_H
 * starting at (tx, ty). The drawing area is the upper ~64% of the tile.
 * Uses simple shapes that look distinct at small sizes.
 * -------------------------------------------------------------------------- */
static void draw_tile_icon(int tx, int ty, int action, uint32_t tile_color) {
    /* Centre of icon zone */
    int cx = tx + TILE_W / 2;
    int cy = ty + TILE_H / 2 - 6;

    /* Pick contrasting foreground */
    int br = (((tile_color >> 16) & 0xFF) + ((tile_color >> 8) & 0xFF) + (tile_color & 0xFF)) / 3;
    uint32_t fg = (br > 170) ? FB_RGB(40, 42, 50) : FB_RGB(245, 245, 250);
    uint32_t fg_dim = (br > 170) ? FB_RGB(100, 100, 110) : FB_RGB(180, 180, 190);

    switch (action) {
        case SMENU_TERMINAL: {
            /* > and cursor */
            gfx_fill_rect(cx - 14, cy - 6, 4, 4, fg);
            gfx_fill_rect(cx - 10, cy - 2, 4, 4, fg);
            gfx_fill_rect(cx - 14, cy + 2, 4, 4, fg);
            gfx_fill_rect(cx - 2,  cy + 4, 14, 4, fg);
            break;
        }
        case SMENU_FILEMGR: {
            /* Folder */
            gfx_fill_rounded_rect(cx - 16, cy - 10, 14, 4, 1, FB_RGB(240, 240, 250));
            gfx_fill_rounded_rect(cx - 16, cy - 7, 32, 22, 3, fg);
            gfx_draw_rounded_rect(cx - 16, cy - 7, 32, 22, 3, fg_dim);
            break;
        }
        case SMENU_CALC: {
            /* Calculator keypad */
            gfx_fill_rounded_rect(cx - 14, cy - 14, 28, 8, 2, FB_RGB(30, 32, 40));
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    gfx_fill_rounded_rect(cx - 13 + c * 9, cy - 4 + r * 7, 7, 5, 1,
                        (c == 2 && r == 2) ? FB_RGB(255, 140, 60) : FB_RGB(220, 225, 240));
            break;
        }
        case SMENU_SYSMON: {
            /* Heartbeat line */
            gfx_draw_line(cx - 18, cy + 4, cx - 8, cy + 4, fg);
            gfx_draw_line(cx - 8,  cy + 4, cx - 4, cy - 8, fg);
            gfx_draw_line(cx - 4,  cy - 8, cx + 0, cy + 10, fg);
            gfx_draw_line(cx + 0,  cy + 10, cx + 6, cy + 4, fg);
            gfx_draw_line(cx + 6,  cy + 4, cx + 18, cy + 4, fg);
            /* thicken */
            gfx_draw_line(cx - 18, cy + 5, cx - 8, cy + 5, fg);
            gfx_draw_line(cx - 8,  cy + 5, cx - 4, cy - 7, fg);
            gfx_draw_line(cx - 4,  cy - 7, cx + 0, cy + 11, fg);
            gfx_draw_line(cx + 0,  cy + 11, cx + 6, cy + 5, fg);
            gfx_draw_line(cx + 6,  cy + 5, cx + 18, cy + 5, fg);
            break;
        }
        case SMENU_SETTINGS: {
            /* Gear */
            gfx_fill_circle(cx, cy, 11, fg);
            gfx_fill_circle(cx, cy, 5, tile_color);
            gfx_fill_rect(cx - 2, cy - 17, 4, 5, fg);
            gfx_fill_rect(cx - 2, cy + 12, 4, 5, fg);
            gfx_fill_rect(cx - 17, cy - 2, 5, 4, fg);
            gfx_fill_rect(cx + 12, cy - 2, 5, 4, fg);
            break;
        }
        case SMENU_NOTEPAD: {
            /* Notepad page */
            gfx_fill_rounded_rect(cx - 14, cy - 16, 28, 32, 2, fg);
            gfx_draw_rounded_rect(cx - 14, cy - 16, 28, 32, 2, fg_dim);
            gfx_fill_rect(cx - 12, cy - 8, 22, 2, fg_dim);
            gfx_fill_rect(cx - 12, cy - 2, 22, 2, fg_dim);
            gfx_fill_rect(cx - 12, cy + 4, 16, 2, fg_dim);
            break;
        }
        case SMENU_TASKMGR: {
            /* Bar chart */
            gfx_fill_rounded_rect(cx - 16, cy + 6, 6, 8,  1, fg);
            gfx_fill_rounded_rect(cx - 8,  cy - 2, 6, 16, 1, fg);
            gfx_fill_rounded_rect(cx + 0,  cy - 10,6, 24, 1, fg);
            gfx_fill_rounded_rect(cx + 8,  cy - 4, 6, 18, 1, fg);
            break;
        }
        case SMENU_CALENDAR: {
            /* Calendar page */
            gfx_fill_rounded_rect(cx - 16, cy - 14, 32, 28, 2, fg);
            gfx_fill_rect(cx - 16, cy - 14, 32, 6, fg_dim);
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++)
                    gfx_fill_rect(cx - 13 + c * 8, cy - 4 + r * 6, 4, 3, fg_dim);
            break;
        }
        case SMENU_MUSIC: {
            /* Music note */
            gfx_fill_circle(cx - 6, cy + 8, 5, fg);
            gfx_fill_circle(cx + 10, cy + 4, 5, fg);
            gfx_fill_rect(cx - 1, cy - 12, 3, 20, fg);
            gfx_fill_rect(cx + 15, cy - 16, 3, 20, fg);
            gfx_fill_rect(cx - 1, cy - 12, 19, 4, fg);
            break;
        }
        case SMENU_HELP: {
            /* Question mark */
            gfx_fill_circle(cx, cy - 6, 9, fg);
            gfx_fill_circle(cx, cy - 6, 4, tile_color);
            gfx_fill_rect(cx - 2, cy + 0, 4, 8, fg);
            gfx_fill_rect(cx - 2, cy + 12, 4, 4, fg);
            break;
        }
        case SMENU_PAINT: {
            /* Brush */
            gfx_fill_rounded_rect(cx - 10, cy - 14, 20, 10, 3, fg);
            gfx_fill_rect(cx - 6, cy - 4, 12, 10, fg_dim);
            gfx_fill_rect(cx - 4, cy + 6, 8, 8, fg);
            break;
        }
        case SMENU_MINESWEEP: {
            /* Bomb */
            gfx_fill_circle(cx, cy + 2, 10, fg);
            gfx_fill_rect(cx - 2, cy - 12, 4, 6, fg);
            gfx_fill_circle(cx - 4, cy - 2, 2, FB_RGB(255, 255, 255));
            break;
        }
        case SMENU_RECYCLE: {
            /* Trash bin */
            gfx_fill_rect(cx - 12, cy - 12, 24, 4, fg);
            gfx_fill_rect(cx - 4,  cy - 16, 8, 4, fg);
            gfx_fill_rounded_rect(cx - 10, cy - 6, 20, 22, 2, fg);
            gfx_fill_rect(cx - 6, cy - 4, 2, 16, tile_color);
            gfx_fill_rect(cx - 1, cy - 4, 2, 16, tile_color);
            gfx_fill_rect(cx + 4, cy - 4, 2, 16, tile_color);
            break;
        }
        case SMENU_SYSINFO: {
            /* Info "i" */
            gfx_fill_circle(cx, cy, 14, fg);
            gfx_fill_circle(cx, cy, 10, tile_color);
            gfx_fill_rect(cx - 2, cy - 6, 4, 4, fg);
            gfx_fill_rect(cx - 2, cy + 0, 4, 10, fg);
            break;
        }
        case SMENU_TODO: {
            /* Checkbox */
            gfx_fill_rounded_rect(cx - 12, cy - 12, 24, 24, 3, fg);
            gfx_draw_line(cx - 6, cy + 2, cx - 2, cy + 6, tile_color);
            gfx_draw_line(cx - 2, cy + 6, cx + 8, cy - 6, tile_color);
            gfx_draw_line(cx - 6, cy + 3, cx - 2, cy + 7, tile_color);
            gfx_draw_line(cx - 2, cy + 7, cx + 8, cy - 5, tile_color);
            break;
        }
        case SMENU_CLOCK: {
            /* Clock face */
            gfx_fill_circle(cx, cy, 14, fg);
            gfx_fill_circle(cx, cy, 11, tile_color);
            gfx_fill_rect(cx - 1, cy - 9, 2, 10, fg);
            gfx_fill_rect(cx - 1, cy, 8, 2, fg);
            break;
        }
        case SMENU_PONG: {
            /* Ball + paddles */
            gfx_fill_rect(cx - 16, cy - 6, 3, 12, fg);
            gfx_fill_rect(cx + 13, cy - 6, 3, 12, fg);
            gfx_fill_circle(cx, cy, 4, fg);
            break;
        }
        case SMENU_SEARCH: {
            /* Magnifier */
            gfx_draw_circle(cx - 2, cy - 4, 10, fg);
            gfx_draw_circle(cx - 2, cy - 4, 9, fg);
            gfx_draw_line(cx + 6, cy + 4, cx + 14, cy + 12, fg);
            gfx_draw_line(cx + 7, cy + 5, cx + 15, cy + 13, fg);
            break;
        }
        case SMENU_TETRIS: {
            /* Tetris L block */
            gfx_fill_rect(cx - 10, cy - 8,  8, 8, fg);
            gfx_fill_rect(cx - 10, cy + 0,  8, 8, fg);
            gfx_fill_rect(cx - 2,  cy + 0,  8, 8, fg);
            gfx_fill_rect(cx + 6,  cy + 0,  8, 8, fg);
            break;
        }
        case SMENU_HEXVIEW: {
            /* Hex digits */
            font_draw_string(cx - 14, cy - 6, "1A", fg, tile_color);
            font_draw_string(cx - 14, cy + 4, "FF", fg, tile_color);
            font_draw_string(cx + 2,  cy - 6, "2B", fg, tile_color);
            font_draw_string(cx + 2,  cy + 4, "C0", fg, tile_color);
            break;
        }
        case SMENU_CONTACTS: {
            /* Person */
            gfx_fill_circle(cx, cy - 6, 5, fg);
            gfx_fill_rounded_rect(cx - 9, cy + 2, 18, 12, 3, fg);
            break;
        }
        case SMENU_COLORPICK: {
            /* Color wheel */
            gfx_fill_circle(cx, cy, 12, FB_RGB(255, 80, 80));
            gfx_fill_rect(cx, cy - 12, 12, 12, FB_RGB(80, 200, 100));
            gfx_fill_rect(cx, cy, 12, 12, FB_RGB(80, 150, 255));
            gfx_fill_rect(cx - 12, cy, 12, 12, FB_RGB(255, 220, 70));
            gfx_fill_circle(cx, cy, 4, fg);
            break;
        }
        case SMENU_SYSLOG: {
            /* Document with lines */
            gfx_fill_rounded_rect(cx - 12, cy - 14, 24, 28, 2, fg);
            gfx_fill_rect(cx - 9, cy - 9, 18, 2, tile_color);
            gfx_fill_rect(cx - 9, cy - 4, 18, 2, tile_color);
            gfx_fill_rect(cx - 9, cy + 1, 14, 2, tile_color);
            gfx_fill_rect(cx - 9, cy + 6, 18, 2, tile_color);
            break;
        }
        case SMENU_CLIPMGR: {
            /* Clipboard */
            gfx_fill_rounded_rect(cx - 10, cy - 12, 20, 28, 2, fg);
            gfx_fill_rect(cx - 5, cy - 16, 10, 6, fg);
            gfx_fill_rect(cx - 6, cy - 4, 12, 2, tile_color);
            gfx_fill_rect(cx - 6, cy + 0, 12, 2, tile_color);
            gfx_fill_rect(cx - 6, cy + 4, 8, 2, tile_color);
            break;
        }
        case SMENU_APPEAR: {
            /* Paint roller / palette */
            gfx_fill_circle(cx - 4, cy, 12, fg);
            gfx_fill_circle(cx - 8, cy - 4, 3, FB_RGB(255, 100, 100));
            gfx_fill_circle(cx + 0, cy - 6, 3, FB_RGB(100, 200, 100));
            gfx_fill_circle(cx + 4, cy + 2, 3, FB_RGB(100, 150, 255));
            gfx_fill_circle(cx - 6, cy + 6, 3, FB_RGB(255, 220, 70));
            break;
        }
        case SMENU_FILEOPS: {
            /* Document w/ arrows */
            gfx_fill_rounded_rect(cx - 12, cy - 12, 18, 24, 2, fg);
            gfx_fill_rect(cx + 4, cy - 4, 14, 4, fg);
            gfx_fill_rect(cx + 14, cy - 8, 4, 12, fg);
            break;
        }
        case SMENU_SNAKE: {
            gfx_fill_rect(cx - 14, cy + 4, 6, 6, fg);
            gfx_fill_rect(cx - 8,  cy + 4, 6, 6, fg);
            gfx_fill_rect(cx - 2,  cy + 4, 6, 6, fg);
            gfx_fill_rect(cx - 2,  cy - 2, 6, 6, fg);
            gfx_fill_rect(cx + 4,  cy - 2, 6, 6, fg);
            gfx_fill_circle(cx + 12, cy - 8, 3, FB_RGB(255, 90, 90));
            break;
        }
        case SMENU_BROWSER: {
            /* Globe */
            gfx_fill_circle(cx, cy, 14, fg);
            gfx_draw_circle(cx, cy, 14, fg_dim);
            gfx_draw_hline(cx - 14, cy, 28, fg_dim);
            gfx_draw_vline(cx, cy - 14, 28, fg_dim);
            gfx_draw_circle(cx, cy, 7, fg_dim);
            break;
        }
        default: {
            gfx_fill_rounded_rect(cx - 10, cy - 10, 20, 20, 4, fg);
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * VESA full-screen launcher
 * -------------------------------------------------------------------------- */
static void draw_launcher_vesa(void) {
    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    uint32_t bg = FB_RGB(18, 18, 22);

    /* Dark backdrop */
    gfx_fill_rect(0, 0, sw, sh, bg);

    /* Header */
    int header_y = 36;
    font_draw_string(48, header_y, "Activities", FB_RGB(240, 240, 245), bg);
    font_draw_string(48, header_y + 20, "Type or click to launch an app",
                     FB_RGB(120, 125, 135), bg);

    /* Compute grid origin to center horizontally */
    int row_height = TILE_H + LABEL_GAP + 16;   /* tile + gap + label line */
    int grid_w = GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP;
    int origin_x = (sw - grid_w) / 2;
    int origin_y = header_y + 60;

    for (int i = 0; i < ITEM_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int tx = origin_x + col * (TILE_W + TILE_GAP);
        int ty = origin_y + row * (row_height + TILE_GAP);

        if (ty + TILE_H + 22 > sh - 40) break;  /* off-screen */

        bool sel = (i == selected);
        uint32_t tc = items[i].tile_color;

        /* Selection ring (soft blue glow) */
        if (sel) {
            gfx_fill_rounded_rect(tx - 5, ty - 5, TILE_W + 10, TILE_H + 10, 16, FB_RGB(80, 165, 255));
            gfx_fill_rounded_rect(tx - 3, ty - 3, TILE_W + 6,  TILE_H + 6,  14, bg);
        }

        /* Tile body */
        gfx_fill_rounded_rect(tx, ty, TILE_W, TILE_H, 14, tc);

        /* Top half gentle lighten for depth */
        uint32_t lr = ((tc >> 16) & 0xFF); lr = lr + 24 > 255 ? 255 : lr + 24;
        uint32_t lg = ((tc >>  8) & 0xFF); lg = lg + 24 > 255 ? 255 : lg + 24;
        uint32_t lb = ( tc        & 0xFF); lb = lb + 24 > 255 ? 255 : lb + 24;
        gfx_draw_gradient_v(tx + 2, ty + 2, TILE_W - 4, TILE_H / 2 - 2, FB_RGB(lr, lg, lb), tc);

        /* Subtle inner border */
        gfx_draw_rounded_rect(tx, ty, TILE_W, TILE_H, 14, FB_RGB(lr, lg, lb));

        /* Draw the actual icon */
        draw_tile_icon(tx, ty, items[i].action, tc);

        /* Label below the tile, on dark bg — always readable */
        int label_len = strlen(items[i].label);
        int label_x = tx + (TILE_W - label_len * 8) / 2;
        int label_y = ty + TILE_H + LABEL_GAP;
        uint32_t label_color = sel ? FB_RGB(120, 190, 255) : FB_RGB(220, 220, 230);
        font_draw_string(label_x, label_y, items[i].label, label_color, bg);
    }

    /* Footer hint */
    font_draw_string(48, sh - 28, "Arrows: Navigate    Enter: Launch    Esc: Close",
        FB_RGB(110, 115, 125), bg);
}

/* --------------------------------------------------------------------------
 * Text-mode fallback — simple bottom-anchored panel
 * -------------------------------------------------------------------------- */
static void draw_launcher_text(void) {
    const theme_t* t = theme_get();
    uint8_t bg = t->menu_bg;
    uint8_t hi = t->menu_highlight;
    uint8_t bg_nibble = (bg >> 4) & 0x0F;

    int x = 1, y = 4, w = 78, h = 19;
    gui_rect_t rect = { x, y, w, h };
    gui_fill_rect(rect, ' ', bg);
    gui_draw_box_double(rect, bg);
    gui_draw_text(x + 2, y, "  NexusOS — Activities  ", VGA_COLOR(VGA_WHITE, bg_nibble));

    int cols = 4;
    int per_col = (ITEM_COUNT + cols - 1) / cols;
    int cw = (w - 2) / cols;

    for (int i = 0; i < ITEM_COUNT; i++) {
        int c = i / per_col;
        int r = i % per_col;
        int cx = x + 1 + c * cw;
        int ry = y + 2 + r;
        if (ry >= y + h - 1) continue;

        bool sel = (i == selected);
        uint8_t row_bg = sel ? hi : bg;
        uint8_t row_nibble = (row_bg >> 4) & 0x0F;
        for (int j = 0; j < cw - 1; j++) gui_putchar(cx + j, ry, ' ', row_bg);
        gui_putchar(cx + 1, ry, items[i].glyph, VGA_COLOR(items[i].icon_color, row_nibble));
        gui_draw_text(cx + 3, ry, items[i].label, row_bg);
    }
}

void start_menu_draw(void) {
    if (!menu_open) return;
    if (fb_is_vesa()) draw_launcher_vesa();
    else              draw_launcher_text();
}

/* --------------------------------------------------------------------------
 * Keyboard navigation — grid-aware in VESA mode
 * -------------------------------------------------------------------------- */
int start_menu_handle_key(char key) {
    if (!menu_open) return SMENU_NONE;

    int cols = fb_is_vesa() ? GRID_COLS : 4;

    if ((unsigned char)key == 0x80) {            /* Up */
        if (selected - cols >= 0) selected -= cols;
        return SMENU_NONE;
    }
    if ((unsigned char)key == 0x81) {            /* Down */
        if (selected + cols < ITEM_COUNT) selected += cols;
        return SMENU_NONE;
    }
    if ((unsigned char)key == 0x82) {            /* Left */
        if (selected > 0) selected--;
        return SMENU_NONE;
    }
    if ((unsigned char)key == 0x83) {            /* Right */
        if (selected < ITEM_COUNT - 1) selected++;
        return SMENU_NONE;
    }
    if (key == '\n') {
        int a = items[selected].action;
        menu_open = false;
        return a;
    }
    if (key == 27) {
        menu_open = false;
        return SMENU_NONE;
    }
    return SMENU_NONE;
}

/* --------------------------------------------------------------------------
 * Click handling — works in both modes
 * -------------------------------------------------------------------------- */
bool start_menu_hit(int mx, int my) {
    if (!menu_open) return false;
    /* Whole screen — clicking anywhere is either a tile or a close */
    (void)mx; (void)my;
    return true;
}

int start_menu_handle_click(int mx, int my) {
    if (!menu_open) return SMENU_NONE;

    if (fb_is_vesa()) {
        int sw = (int)fb_get_width();
        int row_height = TILE_H + LABEL_GAP + 16;
        int grid_w = GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP;
        int origin_x = (sw - grid_w) / 2;
        int origin_y = 36 + 60;
        int pmx = mx * 8;   /* char-cell → pixel */
        int pmy = my * 16;

        for (int i = 0; i < ITEM_COUNT; i++) {
            int col = i % GRID_COLS;
            int row = i / GRID_COLS;
            int tx = origin_x + col * (TILE_W + TILE_GAP);
            int ty = origin_y + row * (row_height + TILE_GAP);
            /* Hit area includes the tile + the label below */
            if (pmx >= tx && pmx < tx + TILE_W && pmy >= ty && pmy < ty + TILE_H + 20) {
                int a = items[i].action;
                menu_open = false;
                return a;
            }
        }
        /* Click outside any tile — close menu */
        menu_open = false;
        return SMENU_NONE;
    }

    /* Text-mode click — approximate */
    int x = 1, y = 4, w = 78, h = 19;
    if (mx < x || mx >= x + w || my < y || my >= y + h) {
        menu_open = false;
        return SMENU_NONE;
    }
    int cols = 4;
    int per_col = (ITEM_COUNT + cols - 1) / cols;
    int cw = (w - 2) / cols;
    int c = (mx - x - 1) / cw;
    int r = my - y - 2;
    int idx = c * per_col + r;
    if (idx >= 0 && idx < ITEM_COUNT) {
        int a = items[idx].action;
        menu_open = false;
        return a;
    }
    return SMENU_NONE;
}
