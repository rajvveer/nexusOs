/* ============================================================================
 * NexusOS - Taskbar (Implementation) - Phase 18
 * ============================================================================
 * Pixel-perfect taskbar with gradient background, rounded window buttons,
 * refined system tray icons, and accent indicators.
 * ============================================================================ */

#include "taskbar.h"
#include "gui.h"
#include "gfx.h"
#include "theme.h"
#include "window.h"
#include "rtc.h"
#include "memory.h"
#include "string.h"
#include "vga.h"
#include "notify.h"
#include "clipboard.h"
#include "framebuffer.h"
#include "font.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* Taskbar height in pixels */
#define TB_H  24
#define TB_BTN_H 18

/* Window button tracking */
typedef struct {
    int start_col;
    int end_col;
    int window_id;
} taskbar_button_t;

static taskbar_button_t buttons[MAX_WINDOWS];
static int button_count = 0;

/* System tray positions */
static int tray_bell_col = 0;
static int tray_clip_col = 0;
static int tray_lock_col = 0;

/* -------------------------------------------------------------------------- */
void taskbar_draw(void) {
    const theme_t* t = theme_get();
    int row = TASKBAR_ROW;
    button_count = 0;
    bool vesa = fb_is_vesa();

    if (vesa) {
        int sw = (int)fb_get_width();
        int sh = (int)fb_get_height();
        int tb_y = sh - TB_H;

        /* Gradient background: dark charcoal → slightly lighter */
        gfx_draw_gradient_v(0, tb_y, sw, TB_H,
            FB_RGB(50, 55, 62), FB_RGB(30, 33, 38));

        /* Top highlight line */
        gfx_draw_hline(0, tb_y, sw, FB_RGB(80, 88, 100));

        /* Start button with rounded rect and gradient */
        gfx_fill_rounded_rect(4, tb_y + 3, 90, TB_BTN_H, 4, FB_RGB(25, 110, 190));
        gfx_draw_gradient_h(5, tb_y + 4, 88, TB_BTN_H - 2,
            FB_RGB(30, 120, 210), FB_RGB(50, 150, 240));
        gfx_draw_rounded_rect(4, tb_y + 3, 90, TB_BTN_H, 4, FB_RGB(60, 160, 255));

        /* Mini Windows-style logo */
        int lx = 10, ly = tb_y + 7;
        gfx_fill_rect(lx, ly, 4, 4, FB_RGB(255, 80, 80));
        gfx_fill_rect(lx + 5, ly, 4, 4, FB_RGB(80, 255, 80));
        gfx_fill_rect(lx, ly + 5, 4, 4, FB_RGB(80, 80, 255));
        gfx_fill_rect(lx + 5, ly + 5, 4, 4, FB_RGB(255, 255, 80));

        /* "NexusOS" text */
        font_draw_string(24, tb_y + 4, "NexusOS", FB_RGB(255, 255, 255), FB_RGB(30, 120, 210));

        int col = 12; /* character-cell col for hit testing */
        int px_x = 100; /* pixel x for drawing */

        /* Window buttons */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t* win = window_get(i);
            if (win == NULL) continue;

            bool focused = (win->flags & WIN_FOCUSED) != 0;
            int max_title = 10;
            int btn_pw = (max_title + 2) * 8;

            buttons[button_count].start_col = col;
            buttons[button_count].window_id = i;

            if (focused) {
                /* Active button: slightly lighter with accent */
                gfx_fill_rounded_rect(px_x + 2, tb_y + 3, btn_pw - 4, TB_BTN_H, 3,
                    FB_RGB(70, 78, 90));
                gfx_draw_rounded_rect(px_x + 2, tb_y + 3, btn_pw - 4, TB_BTN_H, 3,
                    FB_RGB(100, 140, 200));
                /* Active accent bar at bottom */
                gfx_fill_rect(px_x + 10, tb_y + TB_BTN_H + 1, btn_pw - 20, 2,
                    FB_RGB(60, 160, 255));
            } else {
                /* Inactive button */
                gfx_fill_rounded_rect(px_x + 2, tb_y + 3, btn_pw - 4, TB_BTN_H, 3,
                    FB_RGB(45, 50, 58));
                gfx_draw_rounded_rect(px_x + 2, tb_y + 3, btn_pw - 4, TB_BTN_H, 3,
                    FB_RGB(60, 65, 72));
            }

            /* Window title text */
            char tbuf[12];
            strncpy(tbuf, win->title, max_title);
            tbuf[max_title] = 0;
            font_draw_string(px_x + 8, tb_y + 4, tbuf,
                focused ? FB_RGB(255, 255, 255) : FB_RGB(160, 165, 175),
                focused ? FB_RGB(70, 78, 90) : FB_RGB(45, 50, 58));

            col += max_title + 2;
            px_x += btn_pw;
            buttons[button_count].end_col = col - 1;
            button_count++;

            px_x += 4; /* gap between buttons */
            col++;
        }

        /* Right section: system tray + clock + memory */
        char time_str[10];
        rtc_time_t time;
        rtc_read(&time);
        rtc_format_time(&time, time_str);

        uint32_t free_kb = pmm_get_free_pages() * 4;
        char mem_str[10];
        int_to_str(free_kb, mem_str);

        char right_buf[25];
        strcpy(right_buf, time_str);
        strcat(right_buf, "  ");
        strcat(right_buf, mem_str);
        strcat(right_buf, "KB");

        int right_len = strlen(right_buf);
        int right_px = sw - right_len * 8 - 8;

        /* Clock + memory text */
        font_draw_string(right_px, tb_y + 4, right_buf,
            FB_RGB(200, 210, 225), FB_RGB(35, 38, 44));

        /* System tray area */
        int tray_px = right_px - 80;
        int tray_start = tray_px / 8;
        tray_bell_col = tray_start;
        tray_clip_col = tray_start + 2;
        tray_lock_col = tray_start + 4;

        /* Separator line */
        gfx_draw_vline(tray_px - 4, tb_y + 4, TB_H - 8, FB_RGB(70, 75, 85));
        gfx_draw_vline(tray_px + 55, tb_y + 4, TB_H - 8, FB_RGB(70, 75, 85));

        /* Bell icon (notification) */
        {
            int bx = tray_px + 4, by = tb_y + 5;
            gfx_fill_rect(bx + 3, by, 8, 2, FB_RGB(255, 200, 60));
            gfx_fill_rect(bx + 1, by + 2, 12, 6, FB_RGB(255, 200, 60));
            gfx_fill_rect(bx + 0, by + 8, 14, 2, FB_RGB(255, 200, 60));
            gfx_fill_rect(bx + 5, by + 10, 4, 2, FB_RGB(255, 200, 60));
        }

        /* Clipboard icon */
        {
            int bx = tray_px + 22, by = tb_y + 4;
            uint32_t cc = clipboard_has_content() ? FB_RGB(100, 230, 120) : FB_RGB(120, 125, 135);
            gfx_fill_rect(bx + 3, by, 6, 3, cc);
            gfx_fill_rect(bx + 1, by + 3, 10, 10, cc);
            gfx_draw_rect(bx + 1, by + 3, 10, 10, FB_RGB(60, 65, 70));
            /* Lines on clipboard */
            gfx_draw_hline(bx + 3, by + 6, 6, FB_RGB(40, 45, 50));
            gfx_draw_hline(bx + 3, by + 9, 6, FB_RGB(40, 45, 50));
        }

        /* Lock icon */
        {
            int bx = tray_px + 40, by = tb_y + 4;
            gfx_draw_rect(bx + 3, by, 6, 5, FB_RGB(140, 190, 255));
            gfx_fill_rect(bx + 1, by + 5, 10, 8, FB_RGB(140, 190, 255));
            gfx_fill_rect(bx + 5, by + 8, 2, 3, FB_RGB(40, 45, 50));
        }

    } else {
        /* Legacy text-mode taskbar */
        for (int x = 0; x < GUI_WIDTH; x++)
            gui_putchar(x, row, ' ', t->taskbar_bg);

        uint8_t start_bg = VGA_COLOR(VGA_WHITE, VGA_CYAN);
        for (int i = 0; i <= 11; i++) gui_putchar(i, row, ' ', start_bg);
        gui_putchar(1, row, '\x0F', VGA_COLOR(VGA_YELLOW, VGA_CYAN));
        gui_draw_text(3, row, "NexusOS", start_bg);

        int col = 12;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t* win = window_get(i);
            if (win == NULL) continue;
            bool focused = (win->flags & WIN_FOCUSED) != 0;
            buttons[button_count].start_col = col;
            buttons[button_count].window_id = i;
            int max_title = 10;
            uint8_t btn_color = focused ? t->taskbar_active : t->taskbar_text;
            gui_putchar(col++, row, ' ', btn_color);
            int j = 0;
            while (win->title[j] && j < max_title) {
                gui_putchar(col++, row, win->title[j], btn_color); j++;
            }
            while(j < max_title) { gui_putchar(col++, row, ' ', btn_color); j++; }
            gui_putchar(col++, row, ' ', btn_color);
            buttons[button_count].end_col = col - 1;
            button_count++;
            if (col < GUI_WIDTH - 20) { gui_putchar(col, row, ' ', t->taskbar_bg); col++; }
        }

        int tray_start = GUI_WIDTH - 20;
        tray_bell_col = tray_start;
        tray_clip_col = tray_start + 2;
        tray_lock_col = tray_start + 4;
        gui_putchar(tray_start - 1, row, (char)BOX_VERT, t->taskbar_bg);
        gui_putchar(tray_bell_col, row, '\x0D', VGA_COLOR(VGA_YELLOW, (t->taskbar_bg >> 4) & 0x0F));
        uint8_t clip_col = clipboard_has_content() ?
            VGA_COLOR(VGA_LIGHT_GREEN, (t->taskbar_bg >> 4) & 0x0F) :
            VGA_COLOR(VGA_DARK_GREY, (t->taskbar_bg >> 4) & 0x0F);
        gui_putchar(tray_clip_col, row, '\xFE', clip_col);
        gui_putchar(tray_lock_col, row, '\x08', VGA_COLOR(VGA_LIGHT_CYAN, (t->taskbar_bg >> 4) & 0x0F));
        gui_putchar(tray_start + 5, row, (char)BOX_VERT, t->taskbar_bg);

        char time_str[10]; rtc_time_t time; rtc_read(&time); rtc_format_time(&time, time_str);
        uint32_t free_kb = pmm_get_free_pages() * 4;
        char mem_str[10]; int_to_str(free_kb, mem_str);
        char right_buf[25]; strcpy(right_buf, time_str);
        strcat(right_buf, " "); strcat(right_buf, mem_str); strcat(right_buf, "KB");
        int right_len = strlen(right_buf);
        int right_start = GUI_WIDTH - right_len - 1;
        gui_draw_text(right_start, row, right_buf, t->taskbar_clock);
    }
}

/* -------------------------------------------------------------------------- */
bool taskbar_hit(int mx, int my) {
    (void)mx;
    return my == TASKBAR_ROW;
}

bool taskbar_start_hit(int mx) {
    return (mx >= 0 && mx <= 10);
}

int taskbar_handle_click(int mx) {
    for (int i = 0; i < button_count; i++) {
        if (mx >= buttons[i].start_col && mx <= buttons[i].end_col)
            return buttons[i].window_id;
    }
    return -1;
}

int taskbar_tray_hit(int mx) {
    if (mx == tray_bell_col || mx == tray_bell_col + 1) return 1;
    if (mx == tray_clip_col || mx == tray_clip_col + 1) return 2;
    if (mx == tray_lock_col || mx == tray_lock_col + 1) return 3;
    return 0;
}
