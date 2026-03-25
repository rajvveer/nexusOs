/* ============================================================================
 * NexusOS — Right-Click Context Menu (Implementation)
 * ============================================================================
 * Popup menu appearing on right-click on the desktop background.
 * Auto-positions at click coordinates, clamped to screen bounds.
 * ============================================================================ */

#include "context_menu.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

/* Menu item */
typedef struct {
    const char* label;
    int action;
} cmenu_item_t;

static const cmenu_item_t items[] = {
    { "\x10 New File",      CMENU_NEW_FILE   },
    { "\x10 Refresh",       CMENU_REFRESH    },
    { "\x10 Next Theme",    CMENU_THEME_NEXT },
    { "\x10 About",         CMENU_ABOUT      },
    { "\x10 Settings",      CMENU_SETTINGS   },
};

#define CMENU_ITEM_COUNT (sizeof(items) / sizeof(items[0]))
#define CMENU_WIDTH   18
#define CMENU_HEIGHT  ((int)CMENU_ITEM_COUNT + 2)  /* items + border */

/* State */
static bool cmenu_open = false;
static int  cmenu_x = 0;
static int  cmenu_y = 0;
static int  cmenu_selected = 0;

/* --------------------------------------------------------------------------
 * context_menu_open: Show menu at (mx, my), clamped to screen
 * -------------------------------------------------------------------------- */
void context_menu_open(int mx, int my) {
    cmenu_x = mx;
    cmenu_y = my;

    /* Clamp to screen bounds */
    if (cmenu_x + CMENU_WIDTH >= GUI_WIDTH)
        cmenu_x = GUI_WIDTH - CMENU_WIDTH - 1;
    if (cmenu_y + CMENU_HEIGHT >= GUI_HEIGHT - 1)
        cmenu_y = GUI_HEIGHT - 1 - CMENU_HEIGHT;
    if (cmenu_x < 0) cmenu_x = 0;
    if (cmenu_y < 0) cmenu_y = 0;

    cmenu_selected = 0;
    cmenu_open = true;
}

void context_menu_close(void) {
    cmenu_open = false;
}

bool context_menu_is_open(void) {
    return cmenu_open;
}

/* --------------------------------------------------------------------------
 * context_menu_draw: Render the context menu popup
 * -------------------------------------------------------------------------- */
void context_menu_draw(void) {
    if (!cmenu_open) return;

    const theme_t* t = theme_get();
    uint8_t bg = t->menu_bg;
    uint8_t hi = t->menu_highlight;
    uint8_t border_col = bg;

    int x = cmenu_x;
    int y = cmenu_y;

    /* Fill background */
    gui_rect_t rect = { x, y, CMENU_WIDTH, CMENU_HEIGHT };
    gui_fill_rect(rect, ' ', bg);
    gui_draw_box(rect, border_col);

    /* Draw items */
    for (int i = 0; i < (int)CMENU_ITEM_COUNT; i++) {
        int row = y + 1 + i;
        uint8_t item_col = (i == cmenu_selected) ? hi : bg;

        /* Fill row */
        for (int j = x + 1; j < x + CMENU_WIDTH - 1; j++) {
            gui_putchar(j, row, ' ', item_col);
        }

        /* Label */
        gui_draw_text(x + 2, row, items[i].label, item_col);
    }
}

/* --------------------------------------------------------------------------
 * context_menu_handle_key: Keyboard navigation
 * -------------------------------------------------------------------------- */
int context_menu_handle_key(char key) {
    if (!cmenu_open) return CMENU_NONE;

    /* Up arrow */
    if ((unsigned char)key == 0x80) {
        if (cmenu_selected > 0) cmenu_selected--;
        return CMENU_NONE;
    }
    /* Down arrow */
    if ((unsigned char)key == 0x81) {
        if (cmenu_selected < (int)CMENU_ITEM_COUNT - 1) cmenu_selected++;
        return CMENU_NONE;
    }
    /* Enter */
    if (key == '\n') {
        int action = items[cmenu_selected].action;
        cmenu_open = false;
        return action;
    }
    /* Escape */
    if (key == 27) {
        cmenu_open = false;
        return CMENU_NONE;
    }
    return CMENU_NONE;
}

/* --------------------------------------------------------------------------
 * context_menu_hit: Check if click is inside menu
 * -------------------------------------------------------------------------- */
bool context_menu_hit(int mx, int my) {
    if (!cmenu_open) return false;
    return (mx >= cmenu_x && mx < cmenu_x + CMENU_WIDTH &&
            my >= cmenu_y && my < cmenu_y + CMENU_HEIGHT);
}

/* --------------------------------------------------------------------------
 * context_menu_handle_click: Mouse click on menu item
 * -------------------------------------------------------------------------- */
int context_menu_handle_click(int mx, int my) {
    if (!cmenu_open) return CMENU_NONE;

    if (!context_menu_hit(mx, my)) {
        cmenu_open = false;
        return CMENU_NONE;
    }

    int item_row = my - cmenu_y - 1;
    if (item_row >= 0 && item_row < (int)CMENU_ITEM_COUNT) {
        int action = items[item_row].action;
        cmenu_open = false;
        return action;
    }
    return CMENU_NONE;
}
