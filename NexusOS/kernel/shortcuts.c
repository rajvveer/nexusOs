/* ============================================================================
 * NexusOS — Shortcut Overlay (Implementation)
 * ============================================================================
 * Full-screen overlay showing ALL keyboard shortcuts, grouped.
 * ============================================================================ */

#include "shortcuts.h"
#include "gui.h"
#include "vga.h"

static bool sc_open = false;

void shortcuts_open(void) { sc_open = true; }
void shortcuts_close(void) { sc_open = false; }
bool shortcuts_is_open(void) { return sc_open; }

typedef struct { const char* key; const char* desc; } sc_item_t;

static const sc_item_t sc_system[] = {
    {"Esc",     "Exit desktop"},
    {"Tab",     "Window switcher"},
    {"Ctrl+P",  "Command palette"},
    {"Ctrl+Q",  "Power menu"},
    {"Ctrl+L",  "Lock screen"},
    {"Ctrl+W",  "Close window"},
    {"F1",      "This help"},
    {0,0}
};
static const sc_item_t sc_apps[] = {
    {"Ctrl+T",  "Terminal"},
    {"Ctrl+F",  "File manager"},
    {"Ctrl+K",  "Calculator"},
    {"Ctrl+S",  "System monitor"},
    {"Ctrl+N",  "Settings"},
    {"Ctrl+B",  "Notepad"},
    {"Ctrl+G",  "Task manager"},
    {"Ctrl+D",  "Calendar"},
    {"Ctrl+H",  "Help"},
    {"Ctrl+I",  "Widgets"},
    {0,0}
};
static const sc_item_t sc_window[] = {
    {"Win+\x1B","Snap left"},
    {"Win+\x1A","Snap right"},
    {"Win+\x18","Maximize"},
    {"Win+\x19","Restore"},
    {"Ctrl+1-4","Switch desktop"},
    {0,0}
};

static void draw_group(const char* title, const sc_item_t* items, int x, int y, uint8_t bg) {
    uint8_t hdr = VGA_COLOR(VGA_LIGHT_CYAN, bg & 0xF);
    uint8_t key_col = VGA_COLOR(VGA_YELLOW, bg & 0xF);
    uint8_t desc_col = VGA_COLOR(VGA_WHITE, bg & 0xF);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg & 0xF);

    gui_draw_text(x, y, title, hdr);
    for (int i = 0; i < 20; i++) gui_putchar(x + i, y + 1, (char)0xC4, dim);
    int row = y + 2;
    for (int i = 0; items[i].key; i++) {
        gui_draw_text(x, row, items[i].key, key_col);
        gui_draw_text(x + 10, row, items[i].desc, desc_col);
        row++;
    }
}

void shortcuts_draw(void) {
    if (!sc_open) return;
    uint8_t bg = VGA_COLOR(VGA_WHITE, VGA_BLACK);

    /* Darken background */
    for (int y = 0; y < 25; y++)
        for (int x = 0; x < 80; x++)
            gui_putchar(x, y, ' ', bg);

    gui_draw_text(20, 1, "\x0F NexusOS Keyboard Shortcuts \x0F", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    for (int x = 10; x < 70; x++) gui_putchar(x, 2, (char)0xC4, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    draw_group("\xFE System", sc_system, 3, 4, VGA_BLACK);
    draw_group("\xFE Applications", sc_apps, 28, 4, VGA_BLACK);
    draw_group("\xFE Windows", sc_window, 56, 4, VGA_BLACK);

    gui_draw_text(25, 22, "Press any key to close", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

int shortcuts_handle_key(char key) {
    (void)key;
    sc_open = false;
    return 1;
}
