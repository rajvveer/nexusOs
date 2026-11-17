/* ============================================================================
 * NexusOS — Virtual Workspaces (Implementation)
 * ============================================================================
 * 4 virtual desktops. Each workspace remembers which windows belong to it.
 * Switching workspaces shows/hides windows accordingly.
 * ============================================================================ */

#include "workspaces.h"
#include "window.h"
#include "gui.h"
#include "vga.h"
#include "string.h"

static int current_ws = 0;

/* Per-window workspace assignment: -1 = unassigned, 0-3 = workspace */
static int win_workspace[MAX_WINDOWS];

void workspaces_init(void) {
    current_ws = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        win_workspace[i] = 0;
}

void workspaces_switch(int ws) {
    if (ws < 0 || ws >= WS_COUNT || ws == current_ws) return;

    /* Hide windows of current workspace */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = window_get(i);
        if (w && w->active && win_workspace[i] == current_ws) {
            w->flags &= ~WIN_VISIBLE;
        }
    }

    current_ws = ws;

    /* Show windows of new workspace */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = window_get(i);
        if (w && w->active && win_workspace[i] == current_ws) {
            if (!(w->flags & WIN_MINIMIZED))
                w->flags |= WIN_VISIBLE;
        }
    }
}

int workspaces_current(void) { return current_ws; }

/* Called when a new window is created — assign to current workspace */
void workspaces_assign_window(int id) {
    if (id >= 0 && id < MAX_WINDOWS)
        win_workspace[id] = current_ws;
}

void workspaces_draw_indicator(int x, int y, uint8_t bg) {
    uint8_t bg_nibble = (bg >> 4) & 0x0F;
    for (int i = 0; i < WS_COUNT; i++) {
        bool active = (i == current_ws);
        uint8_t col = active ?
            VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN) :
            VGA_COLOR(VGA_LIGHT_GREY, bg_nibble);
        char num = '1' + i;
        gui_putchar(x + i * 2, y, num, col);
        gui_putchar(x + i * 2 + 1, y, ' ', bg);
    }
}
