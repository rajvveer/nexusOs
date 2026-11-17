/* ============================================================================
 * NexusOS — Alt+Tab Window Switcher (Implementation)
 * ============================================================================
 * Visual overlay bar showing all open windows for quick switching.
 * ============================================================================ */

#include "switcher.h"
#include "framebuffer.h"
#include "macui.h"
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
    const int S = 84, G = 18, P = 22;
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    int w = sw_count * S + (sw_count - 1) * G + 2 * P, h = S + 2 * P + 30;
    int x = sw / 2 - w / 2, y = sh / 2 - h / 2;
    ui_shadow(x, y, w, h, 24, 30, 120, 10);
    ui_rrect(x, y, w, h, 24, UI_RGB(40, 40, 46), 230);
    ui_rrect_stroke(x, y, w, h, 24, 0xFFFFFF, 40);
    for (int i = 0; i < sw_count; i++) {
        window_t* win = window_get(sw_items[i]);
        if (!win) continue;
        const ui_app_t* app = ui_app_by_title(win->title);
        int ix = x + P + i * (S + G), iy = y + P;
        if (i == sw_selected) ui_rrect(ix - 8, iy - 8, S + 16, S + 16, 18, 0xFFFFFF, 46);
        ui_icon(app ? app->icon : (win->title[0] ? ICON_GENERIC : ICON_INFO), ix, iy, S);
        if (i == sw_selected) {
            const char* nm = app ? app->name : (win->title[0] ? win->title : "About This NexusOS");
            ui_text_center(ix + S / 2, iy + S + 12, nm, UF_UI, UI_RGB(240, 240, 244));
        }
    }

}
