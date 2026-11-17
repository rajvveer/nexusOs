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
#include "appui.h"

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
    appui_theme_t ui = appui_theme();

    char hdr[24]; char n[4]; int_to_str(cm_count, n);
    strcpy(hdr, n); strcat(hdr, " saved items");
    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "Clipboard", hdr);

    int row = cy + 3;
    for (int i = 0; i < cm_count && row < cy + ch - 2; i++) {
        bool sel = (i == cm_sel);
        uint8_t col = sel ? ui.selected : ui.text;
        appui_row(cx, row, cw, sel, ui.panel, ui.selected);

        char idx[4]; int_to_str(i + 1, idx);
        appui_text(cx + 1, row, idx, 3, sel ? ui.selected : ui.warn);
        gui_putchar(cx + 2, row, '.', col);

        int max = cw - 5;
        appui_text(cx + 4, row, cm_hist[i], max, col);
        row++;
    }

    if (cm_count == 0) appui_text(cx + 3, row, "Clipboard empty", cw - 6, ui.muted);

    appui_status(cx, cy + ch - 1, cw, "Enter Copy Selected");
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
    return window_create("Clipboard", 20, 4, 44, 16, cm_draw, cm_key);
}
