/* ============================================================================
 * NexusOS — Clipboard Manager (Implementation)
 * ============================================================================ */

#include "clipmgr.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "clipboard.h"

#define CM_MAX 8
#define CM_ENTRY_MAX 40

static char cm_hist[CM_MAX][CM_ENTRY_MAX];
static int cm_count = 0;
static int cm_sel = 0;

void clipmgr_record(const char* text) {
    if (!text || text[0] == '\0') return;
    /* Shift history down */
    if (cm_count < CM_MAX) cm_count++;
    for (int i = cm_count - 1; i > 0; i--)
        strcpy(cm_hist[i], cm_hist[i - 1]);
    strncpy(cm_hist[0], text, CM_ENTRY_MAX - 1);
    cm_hist[0][CM_ENTRY_MAX - 1] = '\0';
}

static void cm_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t hi = t->menu_highlight;

    int row = cy;
    char hdr[20]; strcpy(hdr, "\xE8 Clipboard [");
    char n[4]; int_to_str(cm_count, n); strcat(hdr, n); strcat(hdr, "]");
    gui_draw_text(cx, row, hdr, accent); row++;
    for (int i = 0; i < cw - 1; i++) gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    for (int i = 0; i < cm_count && row < cy + ch - 2; i++) {
        bool sel = (i == cm_sel);
        uint8_t col = sel ? hi : tc;
        if (sel) for (int j = cx; j < cx + cw - 1; j++) gui_putchar(j, row, ' ', hi);

        char idx[4]; int_to_str(i + 1, idx);
        gui_draw_text(cx + 1, row, idx, VGA_COLOR(VGA_YELLOW, sel ? ((hi >> 4) & 0xF) : bg));
        gui_putchar(cx + 2, row, '.', col);

        int max = cw - 5;
        for (int j = 0; j < max && cm_hist[i][j]; j++)
            gui_putchar(cx + 4 + j, row, cm_hist[i][j], col);
        row++;
    }

    if (cm_count == 0) gui_draw_text(cx + 3, row, "Clipboard empty", dim);

    gui_draw_text(cx, cy + ch - 1, "Enter:Copy Esc:Close", dim);
    (void)cw; (void)ch;
}

static void cm_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80 && cm_sel > 0) cm_sel--;
    if ((unsigned char)key == 0x81 && cm_sel < cm_count - 1) cm_sel++;
    if (key == '\n' && cm_sel < cm_count) {
        clipboard_copy(cm_hist[cm_sel]);
    }
}

int clipmgr_open(void) {
    cm_sel = 0;
    return window_create("Clipboard", 20, 3, 34, 12, cm_draw, cm_key);
}
