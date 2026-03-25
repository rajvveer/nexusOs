/* ============================================================================
 * NexusOS — Alt+Tab Window Switcher (Implementation)
 * ============================================================================
 * Visual overlay bar showing all open windows for quick switching.
 * ============================================================================ */

#include "switcher.h"
#include "gui.h"
#include "theme.h"
#include "window.h"
#include "vga.h"
#include "string.h"

/* State */
static bool sw_open = false;
static int  sw_items[MAX_WINDOWS];
static int  sw_count = 0;
static int  sw_selected = 0;

/* --------------------------------------------------------------------------
 * switcher_open: Populate list and open overlay
 * -------------------------------------------------------------------------- */
void switcher_open(void) {
    sw_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = window_get(i);
        if (w && w->active) {
            sw_items[sw_count++] = i;
        }
    }
    if (sw_count == 0) return;

    /* Start at the second window (next after current) */
    sw_selected = 0;
    int focused = window_get_focused();
    for (int i = 0; i < sw_count; i++) {
        if (sw_items[i] == focused) {
            sw_selected = (i + 1) % sw_count;
            break;
        }
    }
    sw_open = true;
}

void switcher_close(void) {
    if (sw_open && sw_count > 0) {
        window_focus(sw_items[sw_selected]);
    }
    sw_open = false;
}

void switcher_cancel(void) {
    sw_open = false;
}

void switcher_next(void) {
    if (sw_count > 0)
        sw_selected = (sw_selected + 1) % sw_count;
}

void switcher_prev(void) {
    if (sw_count > 0)
        sw_selected = (sw_selected - 1 + sw_count) % sw_count;
}

bool switcher_is_open(void) {
    return sw_open;
}

int switcher_get_selected(void) {
    if (sw_count > 0) return sw_items[sw_selected];
    return -1;
}

/* --------------------------------------------------------------------------
 * switcher_draw: Render the switcher overlay bar
 * -------------------------------------------------------------------------- */
void switcher_draw(void) {
    if (!sw_open || sw_count == 0) return;

    const theme_t* t = theme_get();

    /* Calculate bar dimensions */
    int item_w = 14;
    int total_w = sw_count * item_w + 2;
    if (total_w > GUI_WIDTH - 2) total_w = GUI_WIDTH - 2;
    int bar_x = (GUI_WIDTH - total_w) / 2;
    int bar_y = (GUI_HEIGHT - 1) / 2 - 2;

    /* Background */
    uint8_t bg = t->menu_bg;
    uint8_t hi = t->menu_highlight;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, (bg >> 4) & 0x0F);
    uint8_t title_col = VGA_COLOR(VGA_WHITE, (bg >> 4) & 0x0F);

    gui_rect_t rect = { bar_x, bar_y, total_w, 5 };
    gui_fill_rect(rect, ' ', bg);
    gui_draw_box_double(rect, bg);

    /* Title */
    gui_draw_text(bar_x + 2, bar_y, " Switch Window ", title_col);

    /* Draw items */
    int x = bar_x + 1;
    for (int i = 0; i < sw_count && x + item_w < bar_x + total_w; i++) {
        window_t* w = window_get(sw_items[i]);
        if (!w) continue;

        bool is_sel = (i == sw_selected);
        uint8_t col = is_sel ? hi : bg;

        /* Fill item bg */
        for (int j = 0; j < item_w; j++)
            gui_putchar(x + j, bar_y + 2, ' ', col);

        /* Icon */
        gui_putchar(x + 1, bar_y + 2, is_sel ? '\x10' : '\x07', col);

        /* Title (truncated) */
        int max_title = item_w - 3;
        int tlen = strlen(w->title);
        if (tlen > max_title) tlen = max_title;
        for (int j = 0; j < tlen; j++)
            gui_putchar(x + 3 + j, bar_y + 2, w->title[j], col);

        x += item_w;
    }

    /* Hint */
    gui_draw_text(bar_x + 2, bar_y + 3, "Tab:next  Enter:select", dim);
}
