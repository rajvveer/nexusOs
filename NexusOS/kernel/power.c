/* ============================================================================
 * NexusOS — Power Menu (Implementation)
 * ============================================================================
 * Overlay: Shutdown / Restart / Lock / Cancel.
 * ============================================================================ */

#include "power.h"
#include "gui.h"
#include "vga.h"
#include "string.h"
#include "port.h"

static bool pm_open = false;
static int  pm_sel = 0;

void power_menu_open(void) { pm_open = true; pm_sel = 3; /* default Cancel */ }
void power_menu_close(void) { pm_open = false; }
bool power_menu_is_open(void) { return pm_open; }

void power_menu_draw(void) {
    if (!pm_open) return;

    int x = 25, y = 7, w = 30, h = 10;
    uint8_t bg = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY);
    uint8_t hi = VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN);
    uint8_t dim = VGA_COLOR(VGA_LIGHT_GREY, VGA_DARK_GREY);
    uint8_t title = VGA_COLOR(VGA_LIGHT_CYAN, VGA_DARK_GREY);

    gui_rect_t r = { x, y, w, h };
    gui_fill_rect(r, ' ', bg);
    gui_draw_box_double(r, bg);

    gui_draw_text(x + 8, y, " Power Options ", title);

    const char* opts[] = { "\x1F Shutdown", "\x18 Restart", "\xFE Lock Screen", "\x11 Cancel" };
    for (int i = 0; i < 4; i++) {
        int row = y + 2 + i * 2;
        uint8_t col = (i == pm_sel) ? hi : bg;
        for (int j = x + 2; j < x + w - 2; j++) gui_putchar(j, row, ' ', col);
        gui_draw_text(x + 8, row, opts[i], col);
        if (i == pm_sel) {
            gui_putchar(x + 5, row, '\x10', VGA_COLOR(VGA_LIGHT_CYAN, (col >> 4) & 0xF));
        }
    }
}

int power_menu_handle_key(char key) {
    if (!pm_open) return POWER_NONE;
    if ((unsigned char)key == 0x80 && pm_sel > 0) pm_sel--;
    if ((unsigned char)key == 0x81 && pm_sel < 3) pm_sel++;
    if (key == 27) { pm_open = false; return POWER_NONE; }
    if (key == '\n') {
        pm_open = false;
        switch (pm_sel) {
            case 0: return POWER_SHUTDOWN;
            case 1: return POWER_RESTART;
            case 2: return POWER_LOCK;
            default: return POWER_NONE;
        }
    }
    return POWER_NONE;
}

void power_shutdown(void) {
    /* Clear screen and show shutdown message */
    for (int y = 0; y < 25; y++)
        for (int x = 0; x < 80; x++)
            gui_putchar(x, y, ' ', VGA_COLOR(VGA_WHITE, VGA_BLACK));
    gui_draw_text(30, 12, "NexusOS shutting down...", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    gui_flip();
    /* HLT loop */
    __asm__ volatile("cli; hlt");
}

void power_restart(void) {
    /* Use keyboard controller to trigger reboot */
    uint8_t good = 0x02;
    while (good & 0x02) good = port_byte_in(0x64);
    port_byte_out(0x64, 0xFE);
    __asm__ volatile("cli; hlt");
}
