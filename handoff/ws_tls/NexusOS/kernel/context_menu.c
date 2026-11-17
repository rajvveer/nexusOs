/* ============================================================================
 * NexusOS — Right-Click Context Menu (Implementation)
 * ============================================================================
 * macOS-style popup for right-clicks on the desktop background. Coordinates
 * are pixels; the menu opens at the pointer and is kept on screen.
 * ============================================================================ */

#include "context_menu.h"
#include "macui.h"
#include "framebuffer.h"
#include "string.h"

/* Menu item (label NULL = separator) */
typedef struct {
    const char* label;
    int action;
} cmenu_item_t;

static const cmenu_item_t items[] = {
    { "New File",                CMENU_NEW_FILE   },
    { NULL,                      CMENU_NONE       },
    { "Get Info",                CMENU_ABOUT      },
    { "Change Appearance...",    CMENU_THEME_NEXT },
    { "System Settings...",      CMENU_SETTINGS   },
    { NULL,                      CMENU_NONE       },
    { "Refresh Desktop",         CMENU_REFRESH    },
};

#define CMENU_ITEM_COUNT ((int)(sizeof(items) / sizeof(items[0])))
#define CM_W     220
#define CM_ROW   24
#define CM_SEP   10
#define CM_PAD   6

/* State */
static bool cmenu_open = false;
static int  cmenu_x = 0;
static int  cmenu_y = 0;
static int  cmenu_selected = -1;

static int cm_height(void) {
    int h = CM_PAD * 2;
    for (int i = 0; i < CMENU_ITEM_COUNT; i++) h += items[i].label ? CM_ROW : CM_SEP;
    return h;
}

static int item_at(int px, int py) {
    if (px < cmenu_x || px >= cmenu_x + CM_W) return -1;
    int y = cmenu_y + CM_PAD;
    for (int i = 0; i < CMENU_ITEM_COUNT; i++) {
        int h = items[i].label ? CM_ROW : CM_SEP;
        if (py >= y && py < y + h) return items[i].label ? i : -1;
        y += h;
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * context_menu_open: Show menu at pixel (mx, my), clamped to screen
 * -------------------------------------------------------------------------- */
void context_menu_open(int mx, int my) {
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    cmenu_x = mx + 2;
    cmenu_y = my + 2;
    if (cmenu_x + CM_W > sw - 4) cmenu_x = mx - CM_W - 2;
    if (cmenu_y + cm_height() > sh - 4) cmenu_y = sh - 4 - cm_height();
    if (cmenu_x < 4) cmenu_x = 4;
    if (cmenu_y < MENUBAR_H + 2) cmenu_y = MENUBAR_H + 2;
    cmenu_selected = -1;
    cmenu_open = true;
}

void context_menu_close(void) {
    cmenu_open = false;
}

bool context_menu_is_open(void) {
    return cmenu_open;
}

/* --------------------------------------------------------------------------
 * context_menu_draw: Render the popup
 * -------------------------------------------------------------------------- */
void context_menu_draw(void) {
    if (!cmenu_open) return;
    int h = cm_height();
    ui_shadow(cmenu_x, cmenu_y, CM_W, h, 8, 18, 110, 6);
    ui_rrect(cmenu_x, cmenu_y, CM_W, h, 8, UI_RGB(42, 42, 46), 246);
    ui_rrect_stroke(cmenu_x, cmenu_y, CM_W, h, 8, UI_RGB(110, 110, 116), 120);
    int y = cmenu_y + CM_PAD;
    for (int i = 0; i < CMENU_ITEM_COUNT; i++) {
        if (!items[i].label) {
            ui_blend_rect(cmenu_x + 12, y + CM_SEP / 2, CM_W - 24, 1, 0xFFFFFF, 40);
            y += CM_SEP;
            continue;
        }
        if (i == cmenu_selected) ui_rrect(cmenu_x + 5, y, CM_W - 10, CM_ROW, 5, UI_RGB(10, 100, 230), 255);
        ui_text(cmenu_x + 18, y + (CM_ROW - ui_font_height(UF_UI)) / 2 + 1, items[i].label, UF_UI, 0xFFFFFF);
        y += CM_ROW;
    }
}

/* --------------------------------------------------------------------------
 * context_menu_hover: Track the item under the pointer (true if it changed)
 * -------------------------------------------------------------------------- */
bool context_menu_hover(int px, int py) {
    if (!cmenu_open) return false;
    int i = item_at(px, py);
    if (i == cmenu_selected) return false;
    cmenu_selected = i;
    return true;
}

/* --------------------------------------------------------------------------
 * context_menu_handle_key: Keyboard navigation
 * -------------------------------------------------------------------------- */
int context_menu_handle_key(char key) {
    if (!cmenu_open) return CMENU_NONE;
    unsigned char k = (unsigned char)key;
    if (k == 0x80 || k == 0x81) {
        int step = k == 0x81 ? 1 : -1, i = cmenu_selected;
        for (int n = 0; n < CMENU_ITEM_COUNT; n++) {
            i = (i + step + CMENU_ITEM_COUNT) % CMENU_ITEM_COUNT;
            if (items[i].label) { cmenu_selected = i; break; }
        }
        return CMENU_NONE;
    }
    if (key == '\n' && cmenu_selected >= 0) {
        int action = items[cmenu_selected].action;
        cmenu_open = false;
        return action;
    }
    if (key == 27) cmenu_open = false;
    return CMENU_NONE;
}

/* --------------------------------------------------------------------------
 * context_menu_hit: Is the pixel inside the menu?
 * -------------------------------------------------------------------------- */
bool context_menu_hit(int mx, int my) {
    if (!cmenu_open) return false;
    return mx >= cmenu_x && mx < cmenu_x + CM_W && my >= cmenu_y && my < cmenu_y + cm_height();
}

/* --------------------------------------------------------------------------
 * context_menu_handle_click: Mouse click on a menu item
 * -------------------------------------------------------------------------- */
int context_menu_handle_click(int mx, int my) {
    if (!cmenu_open) return CMENU_NONE;
    if (!context_menu_hit(mx, my)) { cmenu_open = false; return CMENU_NONE; }
    int i = item_at(mx, my);
    if (i < 0) return CMENU_NONE;
    cmenu_open = false;
    return items[i].action;
}
