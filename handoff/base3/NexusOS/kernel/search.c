/* ============================================================================
 * NexusOS — File Search (Implementation)
 * ============================================================================
 * Searches VFS filenames and displays results.
 * ============================================================================ */

#include "search.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "vfs.h"
#include "appui.h"

#define SR_MAX 10
#define SR_QUERY_MAX 24

static char sr_query[SR_QUERY_MAX];
static int sr_qlen = 0;
static char sr_results[SR_MAX][32];
static int sr_count = 0;
static int sr_sel = 0;

static bool sr_match(const char* name, const char* query) {
    if (query[0] == '\0') return true;
    int nlen = strlen(name), qlen = strlen(query);
    for (int i = 0; i <= nlen - qlen; i++) {
        bool match = true;
        for (int j = 0; j < qlen; j++) {
            char a = name[i + j], b = query[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static void sr_search(void) {
    sr_count = 0;
    fs_node_t* root = vfs_get_root();
    if (!root) return;
    for (uint32_t i = 0; sr_count < SR_MAX; i++) {
        fs_node_t* f = vfs_readdir(root, i);
        if (!f) break;
        if (sr_match(f->name, sr_query)) {
            strncpy(sr_results[sr_count], f->name, 31);
            sr_results[sr_count][31] = '\0';
            sr_count++;
        }
    }
    sr_sel = 0;
}

static void sr_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    appui_theme_t ui = appui_theme();

    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "Search", "Find files in RamFS");
    int row = cy + 3;
    appui_input(cx + 1, row, cw - 2, sr_query, sr_qlen, true);
    row += 2;

    char cnt[16]; strcpy(cnt, "Found: ");
    char n[4]; int_to_str(sr_count, n); strcat(cnt, n);
    appui_text(cx + 1, row, cnt, cw - 2, ui.muted); row++;
    appui_hline(cx, row, cw, ui.muted); row++;

    for (int i = 0; i < sr_count && row < cy + ch - 1; i++) {
        bool sel = (i == sr_sel);
        uint8_t col = sel ? ui.selected : ui.text;
        appui_row(cx, row, cw, sel, ui.panel, ui.selected);
        gui_putchar(cx + 1, row, '*', sel ? ui.selected : ui.warn);
        appui_text(cx + 3, row, sr_results[i], cw - 4, col);
        row++;
    }

    if (sr_count == 0) appui_text(cx + 2, row, "No files found", cw - 4, ui.muted);
    (void)cw; (void)ch;
}

static void sr_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80 && sr_sel > 0) sr_sel--;
    if ((unsigned char)key == 0x81 && sr_sel < sr_count - 1) sr_sel++;
    if (key == '\b' && sr_qlen > 0) { sr_query[--sr_qlen] = '\0'; sr_search(); }
    else if (key >= 32 && key < 127 && sr_qlen < SR_QUERY_MAX - 1) {
        sr_query[sr_qlen++] = key; sr_query[sr_qlen] = '\0'; sr_search();
    }
}

int search_open(void) {
    sr_qlen = 0; sr_query[0] = '\0'; sr_count = 0; sr_sel = 0;
    sr_search();
    return window_create("Search", 18, 4, 44, 18, sr_draw, sr_key);
}
