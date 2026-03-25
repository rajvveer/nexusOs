/* ============================================================================
 * NexusOS — App Dock (Implementation)
 * ============================================================================
 * macOS-style dock bar at row 1 (below statusbar) with favorite apps.
 * ============================================================================ */

#include "dock.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"

#define DOCK_MAX 10
#define DOCK_Y 1

typedef struct {
    char label[12];
    char icon;
    uint8_t color;
    int action;
} dock_item_t;

static dock_item_t dock_items[DOCK_MAX] = {
    { "Term",    '\xB2', VGA_LIGHT_GREEN,   1  },
    { "Files",   '\xE8', VGA_YELLOW,        2  },
    { "Notes",   '\xE9', VGA_LIGHT_CYAN,    10 },
    { "Calc",    '\xF1', VGA_WHITE,         3  },
    { "Music",   '\x0E', VGA_YELLOW,        13 },
    { "Paint",   '\xEB', VGA_LIGHT_GREEN,   15 },
    { "Tetris",  '\xDB', VGA_LIGHT_CYAN,    23 },
    { "Clock",   '\x0F', VGA_YELLOW,        20 },
    { "Todo",    '\xFB', VGA_LIGHT_GREEN,   19 },
    { "Search",  '\x10', VGA_WHITE,         22 },
};

static int dock_count = DOCK_MAX;

void dock_init(void) { /* Already initialized statically */ }

void dock_draw(void) {
    const theme_t* t = theme_get();
    uint8_t bar_bg = VGA_COLOR(VGA_WHITE, VGA_BLACK);
    (void)t;

    /* Draw dock bar */
    for (int x = 0; x < 80; x++) gui_putchar(x, DOCK_Y, ' ', bar_bg);

    int spacing = 80 / dock_count;
    for (int i = 0; i < dock_count; i++) {
        int x = i * spacing + 1;
        gui_putchar(x, DOCK_Y, dock_items[i].icon, VGA_COLOR(dock_items[i].color, VGA_BLACK));
        /* Short label */
        int j = 0;
        while (dock_items[i].label[j] && j < 5) {
            gui_putchar(x + 2 + j, DOCK_Y, dock_items[i].label[j], bar_bg);
            j++;
        }
    }
}

bool dock_hit(int x, int y) {
    return y == DOCK_Y;
}

int dock_handle_click(int x) {
    int spacing = 80 / dock_count;
    int idx = x / spacing;
    if (idx >= 0 && idx < dock_count) return dock_items[idx].action;
    return 0;
}
