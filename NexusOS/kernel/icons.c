/* ============================================================================
 * NexusOS — Desktop Icons (Phase 18)
 * ============================================================================
 * Renders desktop icon grid with 48x48 pixel-art icons in VESA mode.
 * Phase 18: Improved icons with gradients, shadows, hover glow, and
 * anti-aliased labels with drop shadows.
 * ============================================================================ */

#include "icons.h"
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
    char     symbol;
    const char* label;
    int      action;
    uint8_t  icon_fg;
} icon_entry_t;

static const icon_entry_t icon_list[] = {
    { '\xC4', "Terminal",    1,  VGA_LIGHT_GREEN  },
    { '\xE8', "Files",       2,  VGA_YELLOW       },
    { '\xF1', "Calculator",  3,  VGA_LIGHT_CYAN   },
    { '\x0F', "Monitor",     4,  VGA_LIGHT_RED    },
    { '\xF0', "Settings",    5,  VGA_LIGHT_MAGENTA},
    { '\xE9', "Notepad",     10, VGA_WHITE        },
    { '\xFE', "Task Mgr",    11, VGA_LIGHT_BLUE   },
    { '\x0E', "Music",       13, VGA_YELLOW       },
    { '\xEB', "Paint",       15, VGA_LIGHT_GREEN  },
    { '\x02', "Snake",       6,  VGA_LIGHT_GREEN  },
};

#define ICON_COUNT (sizeof(icon_list) / sizeof(icon_list[0]))

static int selected_icon = 0;

static int get_icon_w() { return fb_is_vesa() ? 10 : 12; }
static int get_icon_h() { return fb_is_vesa() ? 5 : 2; }

/* --------------------------------------------------------------------------
 * Emoji-style icon: rounded-square colored tile + simple centred glyph,
 * with a subtle vertical gradient and a soft drop shadow.
 * -------------------------------------------------------------------------- */
static void draw_emoji_tile(int x, int y, uint32_t color, uint32_t accent) {
    /* Drop shadow */
    gfx_fill_rounded_rect(x + 2, y + 6, 44, 40, 9, FB_RGB(0, 0, 0));
    /* Tile body */
    gfx_fill_rounded_rect(x, y + 4, 44, 40, 9, color);
    /* Top gradient (lighter) */
    uint32_t lr = ((color >> 16) & 0xFF); lr = lr + 40 > 255 ? 255 : lr + 40;
    uint32_t lg = ((color >>  8) & 0xFF); lg = lg + 40 > 255 ? 255 : lg + 40;
    uint32_t lb = ( color        & 0xFF); lb = lb + 40 > 255 ? 255 : lb + 40;
    gfx_draw_gradient_v(x + 2, y + 6, 40, 18, FB_RGB(lr, lg, lb), color);
    /* Inner highlight stripe */
    gfx_fill_rect(x + 4, y + 7, 36, 1, FB_RGB(lr, lg, lb));
    /* Accent ring (subtle border) */
    gfx_draw_rounded_rect(x, y + 4, 44, 40, 9, accent);
    (void)accent;
}

/* Draw refined pixel-art icon at pixel coordinates */
static void draw_pixel_icon(int x, int y, int action) {
    switch (action) {
        case 1: /* Terminal — dark tile, green prompt */
            draw_emoji_tile(x, y, FB_RGB(34, 36, 44), FB_RGB(70, 78, 92));
            /* Big '>' prompt + cursor */
            gfx_fill_rect(x + 12, y + 18, 3, 3, FB_RGB(80, 240, 120));
            gfx_fill_rect(x + 15, y + 21, 3, 3, FB_RGB(80, 240, 120));
            gfx_fill_rect(x + 12, y + 24, 3, 3, FB_RGB(80, 240, 120));
            gfx_fill_rect(x + 22, y + 24, 10, 3, FB_RGB(200, 240, 220));
            break;

        case 2: /* Files — yellow folder tile */
            draw_emoji_tile(x, y, FB_RGB(255, 195, 70), FB_RGB(220, 160, 30));
            /* Folder tab + body silhouette */
            gfx_fill_rounded_rect(x + 8, y + 14, 14, 4, 2, FB_RGB(255, 235, 140));
            gfx_fill_rounded_rect(x + 8, y + 17, 28, 18, 3, FB_RGB(255, 235, 140));
            gfx_draw_rounded_rect(x + 8, y + 17, 28, 18, 3, FB_RGB(200, 140, 20));
            break;

        case 3: /* Calculator — flat dark tile w/ orange accent */
            draw_emoji_tile(x, y, FB_RGB(70, 80, 100), FB_RGB(100, 110, 130));
            /* Mini screen */
            gfx_fill_rounded_rect(x + 8, y + 10, 28, 8, 2, FB_RGB(20, 24, 30));
            gfx_fill_rect(x + 28, y + 13, 6, 2, FB_RGB(60, 220, 200));
            /* Buttons */
            for (int bx = 0; bx < 3; bx++)
                for (int by = 0; by < 3; by++) {
                    uint32_t bc = (bx == 2 && by == 2) ? FB_RGB(255, 140, 60) : FB_RGB(180, 188, 205);
                    gfx_fill_rounded_rect(x + 9 + bx * 9, y + 22 + by * 7, 7, 5, 1, bc);
                }
            break;

        case 4: /* Monitor — red tile + chart */
            draw_emoji_tile(x, y, FB_RGB(220, 80, 90), FB_RGB(180, 50, 60));
            /* Chart curve */
            gfx_draw_line(x + 8, y + 32, x + 16, y + 22, FB_RGB(255, 255, 255));
            gfx_draw_line(x + 16, y + 22, x + 24, y + 28, FB_RGB(255, 255, 255));
            gfx_draw_line(x + 24, y + 28, x + 32, y + 14, FB_RGB(255, 255, 255));
            gfx_draw_line(x + 32, y + 14, x + 38, y + 18, FB_RGB(255, 255, 255));
            break;

        case 5: /* Settings — purple gear */
            draw_emoji_tile(x, y, FB_RGB(140, 110, 200), FB_RGB(100, 80, 160));
            {
                int cx = x + 22, cy = y + 24;
                gfx_fill_circle(cx, cy, 10, FB_RGB(240, 240, 250));
                gfx_fill_circle(cx, cy, 4, FB_RGB(140, 110, 200));
                /* 4 teeth */
                gfx_fill_rect(cx - 2, cy - 14, 4, 4, FB_RGB(240, 240, 250));
                gfx_fill_rect(cx - 2, cy + 10, 4, 4, FB_RGB(240, 240, 250));
                gfx_fill_rect(cx - 14, cy - 2, 4, 4, FB_RGB(240, 240, 250));
                gfx_fill_rect(cx + 10, cy - 2, 4, 4, FB_RGB(240, 240, 250));
            }
            break;

        case 10: /* Notepad — white tile + lines */
            draw_emoji_tile(x, y, FB_RGB(250, 250, 252), FB_RGB(200, 200, 215));
            gfx_fill_rect(x + 10, y + 12, 24, 3, FB_RGB(220, 80, 80));
            for (int i = 0; i < 4; i++)
                gfx_fill_rect(x + 10, y + 19 + i * 5, 22, 2, FB_RGB(170, 180, 210));
            break;

        case 11: /* Task Mgr — blue tile + bars */
            draw_emoji_tile(x, y, FB_RGB(80, 140, 220), FB_RGB(50, 110, 190));
            gfx_fill_rounded_rect(x + 8,  y + 28, 8, 8, 1, FB_RGB(240, 240, 250));
            gfx_fill_rounded_rect(x + 18, y + 20, 8, 16, 1, FB_RGB(240, 240, 250));
            gfx_fill_rounded_rect(x + 28, y + 14, 8, 22, 1, FB_RGB(240, 240, 250));
            break;

        case 13: /* Music — orange tile + note */
            draw_emoji_tile(x, y, FB_RGB(240, 170, 60), FB_RGB(200, 130, 30));
            gfx_fill_circle(x + 16, y + 32, 5, FB_RGB(255, 255, 255));
            gfx_fill_rect(x + 20, y + 14, 3, 18, FB_RGB(255, 255, 255));
            gfx_fill_rect(x + 20, y + 14, 12, 4, FB_RGB(255, 255, 255));
            break;

        case 15: /* Paint — green tile + palette */
            draw_emoji_tile(x, y, FB_RGB(90, 200, 130), FB_RGB(60, 170, 100));
            gfx_fill_circle(x + 14, y + 22, 4, FB_RGB(255, 80, 80));
            gfx_fill_circle(x + 30, y + 18, 4, FB_RGB(80, 150, 255));
            gfx_fill_circle(x + 22, y + 32, 4, FB_RGB(255, 220, 70));
            gfx_fill_circle(x + 32, y + 32, 4, FB_RGB(220, 100, 230));
            break;

        case 6: /* Snake — dark green tile */
            draw_emoji_tile(x, y, FB_RGB(60, 160, 90), FB_RGB(40, 120, 70));
            /* Snake body */
            gfx_fill_rect(x + 12, y + 26, 6, 6, FB_RGB(255, 255, 255));
            gfx_fill_rect(x + 18, y + 26, 6, 6, FB_RGB(255, 255, 255));
            gfx_fill_rect(x + 18, y + 20, 6, 6, FB_RGB(255, 255, 255));
            gfx_fill_rect(x + 24, y + 20, 6, 6, FB_RGB(255, 255, 255));
            /* Apple */
            gfx_fill_circle(x + 32, y + 14, 3, FB_RGB(255, 90, 90));
            break;

        default:
            draw_emoji_tile(x, y, FB_RGB(120, 130, 150), FB_RGB(90, 100, 120));
            break;
    }
}

void icons_init(void) { selected_icon = 0; }

void icons_draw(void) {
    const theme_t* t = theme_get();
    uint8_t bg_nibble = (t->desktop_bg >> 4) & 0x0F;
    bool vesa = fb_is_vesa();

    int w = get_icon_w();
    int h = get_icon_h();
    int max_rows = (GUI_HEIGHT - 3) / h;
    if (max_rows < 1) max_rows = 1;

    for (int i = 0; i < (int)ICON_COUNT; i++) {
        int col = i / max_rows;
        int row = i % max_rows;
        int ix = 2 + col * w;
        int iy = 1 + row * h;
        bool is_sel = (i == selected_icon);

        if (vesa) {
            int px = ix * 8;
            int py = iy * 16;

            /* Hover glow for selected icon */
            if (is_sel) {
                gfx_fill_rect_alpha(px - 6, py - 6, 60, 72, FB_RGB(100, 180, 255), 60);
                gfx_draw_rounded_rect(px - 6, py - 6, 60, 72, 6, FB_RGB(100, 200, 255));
            }

            draw_pixel_icon(px, py, icon_list[i].action);

            /* Label with drop shadow */
            int lbl_len = strlen(icon_list[i].label);
            int text_w = lbl_len * 8;
            int text_x = px + 24 - (text_w / 2);
            if (text_x < px - 6) text_x = px - 6;

            /* Shadow text (offset 1px down-right) */
            font_draw_string(text_x + 1, py + 53, icon_list[i].label,
                FB_RGB(0, 0, 0), FB_RGB(0, 0, 0));
            /* Main text */
            font_draw_string(text_x, py + 52, icon_list[i].label,
                FB_RGB(255, 255, 255), FB_RGB(0, 0, 0));

        } else {
            if (is_sel) {
                for (int j = 0; j < 12; j++) gui_putchar(ix + j, iy, ' ', VGA_COLOR(VGA_WHITE, bg_nibble));
            }
            uint8_t sym_col = VGA_COLOR(icon_list[i].icon_fg, bg_nibble);
            gui_putchar(ix + 1, iy, icon_list[i].symbol, sym_col);
            uint8_t lbl_col = is_sel ? VGA_COLOR(VGA_WHITE, bg_nibble) : VGA_COLOR(VGA_LIGHT_GREY, bg_nibble);
            gui_draw_text(ix + 3, iy, icon_list[i].label, lbl_col);
            if (is_sel) gui_putchar(ix, iy, '\x10', VGA_COLOR(VGA_LIGHT_CYAN, bg_nibble));
        }
    }
}

int icons_handle_key(char key) {
    int max_rows = (GUI_HEIGHT - 3) / get_icon_h();
    if (max_rows < 1) max_rows = 1;

    if ((unsigned char)key == 0x80) { if (selected_icon % max_rows > 0) selected_icon--; return 0; }
    if ((unsigned char)key == 0x81) { if (selected_icon % max_rows < max_rows - 1 && selected_icon < (int)ICON_COUNT - 1) selected_icon++; return 0; }
    if ((unsigned char)key == 0x82) { if (selected_icon >= max_rows) selected_icon -= max_rows; return 0; }
    if ((unsigned char)key == 0x83) { if (selected_icon + max_rows < (int)ICON_COUNT) selected_icon += max_rows; return 0; }
    if (key == '\n') return icon_list[selected_icon].action;
    return 0;
}

int icons_handle_click(int mx, int my) {
    int w = get_icon_w();
    int h = get_icon_h();
    int max_rows = (GUI_HEIGHT - 3) / h;
    if (max_rows < 1) max_rows = 1;

    for (int i = 0; i < (int)ICON_COUNT; i++) {
        int col = i / max_rows;
        int row = i % max_rows;
        int ix = 2 + col * w;
        int iy = 1 + row * h;

        if (fb_is_vesa()) {
            if (mx >= ix - 1 && mx < ix + 8 && my >= iy && my < iy + 5) {
                selected_icon = i;
                return icon_list[i].action;
            }
        } else {
            if (mx >= ix && mx < ix + 12 && my == iy) {
                selected_icon = i;
                return icon_list[i].action;
            }
        }
    }
    return 0;
}

int icons_get_selected(void) { return selected_icon; }
