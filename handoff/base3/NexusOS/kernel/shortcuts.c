/* ============================================================================
 * NexusOS — Shortcut Overlay (Implementation)
 * ============================================================================
 * Full-screen overlay showing ALL keyboard shortcuts, grouped.
 * ============================================================================ */

#include "shortcuts.h"
#include "framebuffer.h"
#include "macui.h"
#include "gui.h"
#include "vga.h"

static bool sc_open = false;

void shortcuts_open(void) { sc_open = true; }
void shortcuts_close(void) { sc_open = false; }
bool shortcuts_is_open(void) { return sc_open; }

typedef struct { const char* key; const char* desc; } sc_item_t;

static const sc_item_t sc_system[] = {
    {"Ctrl+Space", "Spotlight Search"},
    {"F4",         "Launchpad"},
    {"Tab / F3",   "Switch apps"},
    {"Ctrl+L",     "Lock Screen"},
    {"Ctrl+W",     "Close window"},
    {"F12",        "Exit to Console"},
    {"F1",         "Keyboard Shortcuts"},
    {0,0}
};
static const sc_item_t sc_apps[] = {
    {"Ctrl+T",  "Terminal"},
    {"Ctrl+F",  "Finder"},
    {"Ctrl+K",  "Calculator"},
    {"Ctrl+S",  "Activity Monitor"},
    {"Ctrl+B",  "Notes"},
    {"Ctrl+G",  "Task Manager"},
    {"Ctrl+D",  "Calendar"},
    {"Ctrl+N",  "Notification Center"},
    {"Ctrl+A",  "About This NexusOS"},
    {0,0}
};
static const sc_item_t sc_window[] = {
    {"Ctrl+Left",   "Tile left"},
    {"Ctrl+Right",  "Tile right"},
    {"Ctrl+Up",     "Zoom"},
    {"Ctrl+Down",   "Restore"},
    {"Ctrl+F5-F8",  "Desktops 1-4"},
    {"Ctrl+I",      "Widgets"},
    {0,0}
};

static int draw_group(const char* title, const sc_item_t* items, int x, int y, int w) {
    ui_text(x, y, title, UF_UIB, UI_RGB(150, 150, 156));
    y += 30;
    for (int i = 0; items[i].key; i++) {
        int kw = ui_text_width(items[i].key, UF_UI) + 16;
        ui_rrect(x, y - 2, kw, 22, 6, 0xFFFFFF, 30);
        ui_text(x + 8, y + 1, items[i].key, UF_UI, UI_RGB(236, 236, 240));
        ui_text(x + w / 2, y + 1, items[i].desc, UF_UI, UI_RGB(206, 206, 212));
        y += 30;
    }
    return y;
}

void shortcuts_draw(void) {
    if (!sc_open) return;
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    int w = 900, h = 420, x = sw / 2 - w / 2, y = sh / 2 - h / 2 - 20;
    ui_shadow(x, y, w, h, 18, 36, 140, 14);
    ui_rrect(x, y, w, h, 18, UI_RGB(38, 38, 42), 244);
    ui_rrect_stroke(x, y, w, h, 18, 0xFFFFFF, 40);
    ui_text_center(sw / 2, y + 22, "Keyboard Shortcuts", UF_TITLE, 0xFFFFFF);
    int cw = (w - 80) / 3;
    draw_group("System",  sc_system, x + 30,          y + 76, cw);
    draw_group("Apps",    sc_apps,   x + 40 + cw,     y + 76, cw);
    draw_group("Windows", sc_window, x + 50 + 2 * cw, y + 76, cw);
    ui_text_center(sw / 2, y + h - 30, "Press any key to close", UF_SMALL, UI_RGB(130, 130, 136));
}

int shortcuts_handle_key(char key) {
    (void)key;
    sc_open = false;
    return 1;
}
