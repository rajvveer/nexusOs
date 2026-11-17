/* ============================================================================
 * NexusOS — Desktop Widgets (Implementation)
 * ============================================================================
 * Mini clock + system stats drawn over the desktop background.
 * ============================================================================ */

#include "widgets.h"
#include "gui.h"
#include "vga.h"
#include "rtc.h"
#include "memory.h"
#include "process.h"
#include "string.h"

extern volatile uint32_t system_ticks;

static bool wid_visible = false;

void widgets_toggle(void) { wid_visible = !wid_visible; }
bool widgets_visible(void) { return wid_visible; }

void widgets_draw(void) {
    if (!wid_visible) return;

    uint8_t frame = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK);
    uint8_t text  = VGA_COLOR(VGA_WHITE, VGA_BLACK);
    uint8_t dim   = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK);
    uint8_t val   = VGA_COLOR(VGA_YELLOW, VGA_BLACK);

    /* Clock widget - top right corner */
    int wx = GUI_WIDTH - 14;
    int wy = 1;

    gui_rect_t r1 = { wx, wy, 13, 5 };
    gui_fill_rect(r1, ' ', VGA_COLOR(VGA_WHITE, VGA_BLACK));
    gui_draw_box(r1, frame);
    gui_draw_text(wx + 1, wy, " Clock ", frame);

    rtc_time_t t;
    rtc_read(&t);
    char ts[12];
    rtc_format_time(&t, ts);
    gui_draw_text(wx + 2, wy + 1, ts, text);

    /* Date */
    char ds[12]; char n1[4]; char n2[4]; char n3[6];
    int_to_str(t.day, n1); int_to_str(t.month, n2); int_to_str(2000 + t.year, n3);
    strcpy(ds, n1); strcat(ds, "/"); strcat(ds, n2); strcat(ds, "/"); strcat(ds, n3);
    gui_draw_text(wx + 2, wy + 2, ds, dim);

    /* Uptime */
    char up[14]; char sec_s[8];
    int_to_str(system_ticks / 18, sec_s);
    strcpy(up, "Up:"); strcat(up, sec_s); strcat(up, "s");
    gui_draw_text(wx + 2, wy + 3, up, dim);

    /* Stats widget - below clock */
    int sy = wy + 6;
    gui_rect_t r2 = { wx, sy, 13, 6 };
    gui_fill_rect(r2, ' ', VGA_COLOR(VGA_WHITE, VGA_BLACK));
    gui_draw_box(r2, frame);
    gui_draw_text(wx + 1, sy, " Stats ", frame);

    /* Memory */
    char mb[16]; char mn[8];
    uint32_t free_kb = pmm_get_free_pages() * 4;
    int_to_str(free_kb, mn);
    strcpy(mb, "Mem:"); strcat(mb, mn); strcat(mb, "K");
    gui_draw_text(wx + 1, sy + 1, mb, val);

    /* Memory bar */
    uint32_t total = pmm_get_total_pages();
    uint32_t used = pmm_get_used_pages();
    int bar_w = 9;
    int filled = total > 0 ? (int)((used * bar_w) / total) : 0;
    gui_putchar(wx + 1, sy + 2, '[', dim);
    for (int i = 0; i < bar_w; i++)
        gui_putchar(wx + 2 + i, sy + 2, i < filled ? (char)0xDB : (char)0xB0, i < filled ? val : dim);
    gui_putchar(wx + 2 + bar_w, sy + 2, ']', dim);

    /* Process count */
    char pb[16]; char pn[4];
    int_to_str(process_count(), pn);
    strcpy(pb, "Proc:"); strcat(pb, pn);
    gui_draw_text(wx + 1, sy + 3, pb, text);

    /* Ticks */
    char tb[16]; char tn[10];
    int_to_str(system_ticks, tn);
    strcpy(tb, "Tick:"); strcat(tb, tn);
    gui_draw_text(wx + 1, sy + 4, tb, dim);
}
