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

/* Draw refined pixel-art icon at pixel coordinates */
static void draw_pixel_icon(int x, int y, int action) {
    switch (action) {
        case 1: /* Terminal — dark frame with green text */
            gfx_fill_rounded_rect(x, y + 4, 48, 40, 4, FB_RGB(30, 30, 36));
            gfx_draw_rounded_rect(x, y + 4, 48, 40, 4, FB_RGB(80, 85, 95));
            /* Titlebar gradient */
            gfx_draw_gradient_h(x + 2, y + 6, 44, 8,
                FB_RGB(60, 65, 75), FB_RGB(80, 85, 95));
            /* Terminal dots */
            gfx_fill_circle(x + 7, y + 10, 2, FB_RGB(255, 80, 80));
            gfx_fill_circle(x + 14, y + 10, 2, FB_RGB(255, 200, 50));
            gfx_fill_circle(x + 21, y + 10, 2, FB_RGB(80, 220, 80));
            /* Green prompt text */
            gfx_fill_rect(x + 6, y + 20, 2, 2, FB_RGB(0, 255, 0));
            gfx_fill_rect(x + 10, y + 20, 8, 2, FB_RGB(0, 255, 0));
            gfx_fill_rect(x + 6, y + 26, 2, 2, FB_RGB(0, 255, 0));
            gfx_fill_rect(x + 10, y + 26, 14, 2, FB_RGB(100, 255, 100));
            gfx_fill_rect(x + 6, y + 32, 2, 2, FB_RGB(0, 255, 0));
            gfx_fill_rect(x + 10, y + 32, 4, 2, FB_RGB(200, 200, 200));
            break;

        case 2: /* Files — folder with document */
            /* Back folder */
            gfx_fill_rounded_rect(x + 2, y + 8, 18, 6, 2, FB_RGB(255, 190, 40));
            /* Main folder body */
            gfx_fill_rounded_rect(x + 2, y + 14, 44, 28, 3, FB_RGB(255, 210, 70));
            gfx_draw_rounded_rect(x + 2, y + 14, 44, 28, 3, FB_RGB(200, 160, 20));
            /* Front flap with gradient */
            gfx_draw_gradient_v(x + 2, y + 18, 44, 24,
                FB_RGB(255, 220, 90), FB_RGB(255, 200, 50));
            /* Document peeking out */
            gfx_fill_rect(x + 12, y + 6, 26, 10, FB_RGB(255, 255, 255));
            gfx_draw_rect(x + 12, y + 6, 26, 10, FB_RGB(180, 180, 180));
            gfx_draw_hline(x + 15, y + 9, 16, FB_RGB(180, 180, 200));
            gfx_draw_hline(x + 15, y + 12, 12, FB_RGB(180, 180, 200));
            break;

        case 3: /* Calculator — modern flat design */
            gfx_fill_rounded_rect(x + 6, y + 4, 36, 40, 4, FB_RGB(60, 65, 75));
            gfx_draw_rounded_rect(x + 6, y + 4, 36, 40, 4, FB_RGB(90, 95, 105));
            /* Display */
            gfx_fill_rect(x + 10, y + 8, 28, 10, FB_RGB(40, 45, 50));
            gfx_draw_hline(x + 14, y + 14, 18, FB_RGB(0, 220, 200));
            /* Buttons grid */
            for (int bx = 0; bx < 3; bx++)
                for (int by = 0; by < 3; by++) {
                    uint32_t bc = (bx == 2 && by >= 1) ? FB_RGB(255, 140, 50) : FB_RGB(100, 105, 115);
                    gfx_fill_rect(x + 10 + bx * 10, y + 22 + by * 7, 7, 5, bc);
                }
            break;

        case 4: /* Monitor — system vitals */
            gfx_fill_rounded_rect(x + 4, y + 4, 40, 32, 3, FB_RGB(20, 22, 28));
            gfx_draw_rounded_rect(x + 4, y + 4, 40, 32, 3, FB_RGB(80, 85, 95));
            /* Grid lines */
            for (int i = 1; i < 4; i++) gfx_draw_hline(x + 6, y + 4 + i * 8, 36, FB_RGB(0, 60, 0));
            for (int i = 1; i < 4; i++) gfx_draw_vline(x + 4 + i * 10, y + 6, 28, FB_RGB(0, 60, 0));
            /* Line chart */
            gfx_draw_line(x + 8, y + 30, x + 16, y + 18, FB_RGB(0, 255, 100));
            gfx_draw_line(x + 16, y + 18, x + 24, y + 24, FB_RGB(0, 255, 100));
            gfx_draw_line(x + 24, y + 24, x + 32, y + 10, FB_RGB(0, 255, 100));
            gfx_draw_line(x + 32, y + 10, x + 40, y + 14, FB_RGB(0, 255, 100));
            /* Stand */
            gfx_fill_rect(x + 18, y + 36, 12, 4, FB_RGB(100, 105, 115));
            gfx_fill_rect(x + 14, y + 40, 20, 2, FB_RGB(80, 85, 95));
            break;

        case 5: /* Settings — gear */
            {
                int cx = x + 24, cy = y + 24;
                /* Outer ring */
                gfx_fill_circle(cx, cy, 16, FB_RGB(140, 145, 155));
                gfx_fill_circle(cx, cy, 11, FB_RGB(80, 85, 95));
                gfx_fill_circle(cx, cy, 7, FB_RGB(140, 145, 155));
                gfx_fill_circle(cx, cy, 4, FB_RGB(50, 55, 65));
                /* Teeth */
                gfx_fill_rect(cx - 3, cy - 18, 6, 6, FB_RGB(140, 145, 155));
                gfx_fill_rect(cx - 3, cy + 12, 6, 6, FB_RGB(140, 145, 155));
                gfx_fill_rect(cx - 18, cy - 3, 6, 6, FB_RGB(140, 145, 155));
                gfx_fill_rect(cx + 12, cy - 3, 6, 6, FB_RGB(140, 145, 155));
            }
            break;

        case 10: /* Notepad — white page with lines */
            gfx_fill_rounded_rect(x + 8, y + 4, 32, 40, 2, FB_RGB(255, 255, 245));
            gfx_draw_rounded_rect(x + 8, y + 4, 32, 40, 2, FB_RGB(180, 180, 180));
            /* Header bar */
            gfx_fill_rect(x + 9, y + 5, 30, 6, FB_RGB(70, 75, 85));
            /* Red margin line */
            gfx_draw_vline(x + 16, y + 12, 30, FB_RGB(255, 120, 120));
            /* Blue text lines */
            for (int i = 0; i < 5; i++)
                gfx_draw_hline(x + 18, y + 16 + i * 6, 18, FB_RGB(180, 190, 220));
            break;

        case 11: /* Task Mgr — bar chart */
            gfx_fill_rounded_rect(x + 4, y + 4, 40, 40, 3, FB_RGB(240, 240, 240));
            gfx_draw_rounded_rect(x + 4, y + 4, 40, 40, 3, FB_RGB(180, 180, 180));
            /* Header */
            gfx_fill_rect(x + 5, y + 5, 38, 8, FB_RGB(50, 60, 150));
            /* Bars */
            gfx_fill_rect(x + 8, y + 18, 24, 5, FB_RGB(80, 200, 100));
            gfx_fill_rect(x + 8, y + 26, 32, 5, FB_RGB(220, 80, 80));
            gfx_fill_rect(x + 8, y + 34, 16, 5, FB_RGB(80, 120, 220));
            break;

        case 13: /* Music — note */
            gfx_fill_rounded_rect(x + 6, y + 4, 36, 40, 4, FB_RGB(30, 30, 40));
            gfx_draw_rounded_rect(x + 6, y + 4, 36, 40, 4, FB_RGB(60, 60, 80));
            /* Musical notes */
            gfx_fill_circle(x + 16, y + 32, 5, FB_RGB(255, 80, 80));
            gfx_fill_rect(x + 20, y + 12, 2, 20, FB_RGB(255, 80, 80));
            gfx_fill_circle(x + 30, y + 28, 5, FB_RGB(100, 180, 255));
            gfx_fill_rect(x + 34, y + 10, 2, 18, FB_RGB(100, 180, 255));
            gfx_fill_rect(x + 20, y + 10, 16, 3, FB_RGB(255, 200, 60));
            break;

        case 15: /* Paint — palette */
            gfx_fill_circle(x + 24, y + 26, 18, FB_RGB(240, 220, 180));
            gfx_draw_circle(x + 24, y + 26, 18, FB_RGB(200, 180, 140));
            /* Color dots */
            gfx_fill_circle(x + 16, y + 18, 4, FB_RGB(255, 50, 50));
            gfx_fill_circle(x + 28, y + 14, 4, FB_RGB(50, 150, 255));
            gfx_fill_circle(x + 36, y + 22, 4, FB_RGB(50, 220, 50));
            gfx_fill_circle(x + 14, y + 30, 4, FB_RGB(255, 220, 50));
            gfx_fill_circle(x + 30, y + 34, 4, FB_RGB(200, 80, 255));
            /* Hole */
            gfx_fill_circle(x + 24, y + 26, 4, FB_RGB(180, 160, 120));
            break;

        case 6: /* Snake — game board */
            gfx_fill_rounded_rect(x + 6, y + 6, 36, 36, 3, FB_RGB(30, 50, 30));
            gfx_draw_rounded_rect(x + 6, y + 6, 36, 36, 3, FB_RGB(60, 100, 60));
            /* Snake body */
            gfx_fill_rect(x + 14, y + 26, 6, 6, FB_RGB(0, 180, 0));
            gfx_fill_rect(x + 20, y + 26, 6, 6, FB_RGB(0, 200, 0));
            gfx_fill_rect(x + 20, y + 20, 6, 6, FB_RGB(0, 220, 0));
            gfx_fill_rect(x + 26, y + 20, 6, 6, FB_RGB(0, 255, 0));
            /* Eye */
            gfx_fill_rect(x + 30, y + 22, 2, 2, FB_RGB(255, 255, 255));
            /* Apple */
            gfx_fill_circle(x + 16, y + 14, 3, FB_RGB(255, 50, 50));
            gfx_fill_rect(x + 16, y + 10, 1, 3, FB_RGB(0, 150, 0));
            break;

        default:
            gfx_fill_rounded_rect(x + 6, y + 6, 36, 36, 4, FB_RGB(120, 125, 135));
            gfx_draw_rounded_rect(x + 6, y + 6, 36, 36, 4, FB_RGB(160, 165, 175));
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
