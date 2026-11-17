/* ============================================================================
 * NexusOS — App Store window (Implementation) — Phase 57 "apps"
 * ============================================================================
 * Drawn in pixels with the Aurora toolkit (like About This NexusOS). One row
 * per catalog entry; the "Open" button starts the program in the Terminal.
 * Installed state and size come from the FAT32 volume at draw time.
 * ============================================================================ */

#include "appstore_ui.h"
#include "window.h"
#include "macui.h"
#include "fat32.h"
#include "string.h"
#include "vga.h"
#include "framebuffer.h"
#include "keyboard.h"

static const store_app_t CATALOG[] = {
    { "Lua",            "lua",    "lua",       "5.4.7", "MIT",   "https://www.lua.org/ftp/lua-5.4.7.tar.gz",
      "The Lua interpreter and REPL, compiled unmodified against the NexusOS libc" },
    { "Kilo Editor",    "kilo",   "kilo",      "0.0.1", "BSD-2", "https://github.com/antirez/kilo",
      "antirez's 1-file text editor: raw-mode termios + ANSI, Ctrl+S save, Ctrl+Q quit" },
    { "2048",           "2048",   "2048",      "1.0",   "MIT",   "NexusOS userland/apps/2048",
      "The sliding-tiles puzzle in the terminal (arrow keys, q quits)" },
    { "Tetris",         "tetris", "tetris",    "1.0",   "MIT",   "NexusOS userland/apps/tetris",
      "Classic falling blocks, ANSI colours, raw keyboard input" },
    { "Coreutils-lite", "sh",     "coreutils", "1.0",   "MIT",   "NexusOS userland/apps/coreutils",
      "sh with pipes/redirects + ls cat echo wc grep head tail cp mv rm mkdir sort uniq tr hexdump..." },
};
#define NCAT ((int)(sizeof(CATALOG) / sizeof(CATALOG[0])))

const store_app_t* appstore_ui_catalog(int* count) { if (count) *count = NCAT; return CATALOG; }

uint32_t appstore_ui_installed_size(const store_app_t* a) {
    fat32_file_t f;
    char p[32];
    if (!a || !fat32_is_mounted()) return 0;
    strcpy(p, "bin/"); strcat(p, a->bin);
    if (!fat32_lookup(p, &f) || f.is_dir) return 0;
    return f.size;
}

/* layout */
#define ROW_H   74
#define HEAD_H  52
static int sel = 0;
static int win_id = -1;
static int btn_x[NCAT], btn_y[NCAT], btn_w = 72, btn_h = 26;   /* absolute pixel rects */

static void store_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    int x = cx * 8, y = cy * 16, w = cw * 8, h = ch * 16;
    ui_blend_rect(x, y, w, h, UI_RGB(30, 30, 34), 255);
    /* header */
    ui_rrect_vgrad(x, y, w, HEAD_H, 0, UI_RGB(44, 44, 50), UI_RGB(34, 34, 40), 255);
    ui_icon(ICON_LAUNCHPAD, x + 14, y + 8, 36);
    ui_text(x + 62, y + 8, "App Store", UF_TITLE, 0xFFFFFF);
    ui_text(x + 62, y + 32, "Real programs compiled from source for NexusOS - they run in the Terminal", UF_SMALL, UI_RGB(160, 160, 168));
    int ry = y + HEAD_H + 8;
    for (int i = 0; i < NCAT && ry + ROW_H <= y + h; i++, ry += ROW_H) {
        const store_app_t* a = &CATALOG[i];
        uint32_t size = appstore_ui_installed_size(a);
        bool inst = size > 0;
        if (i == sel) ui_rrect(x + 8, ry - 4, w - 16, ROW_H - 4, 10, UI_RGB(10, 100, 230), 60);
        ui_icon(i == 0 ? ICON_TERMINAL : i == 1 ? ICON_NOTES : i == 4 ? ICON_FOLDER : ICON_GENERIC, x + 18, ry + 6, 48);
        ui_text(x + 80, ry + 2, a->name, UF_UIB, 0xFFFFFF);
        char meta[96], nb[12];
        strcpy(meta, "v"); strcat(meta, a->version); strcat(meta, "   ");
        if (inst) { int_to_str((int)((size + 1023) / 1024), nb); strcat(meta, nb); strcat(meta, " KB on /disk/bin   "); }
        else strcat(meta, "not installed (run build.bat apps)   ");
        strcat(meta, a->licence); strcat(meta, " licence");
        ui_text(x + 80, ry + 22, meta, UF_SMALL, UI_RGB(170, 170, 178));
        ui_text(x + 80, ry + 38, a->desc, UF_SMALL, UI_RGB(200, 200, 206));
        ui_text(x + 80, ry + 52, a->url, UF_SMALL, UI_RGB(110, 140, 200));
        /* button */
        int bx = x + w - btn_w - 18, by = ry + 14;
        btn_x[i] = bx; btn_y[i] = by;
        ui_rrect(bx, by, btn_w, btn_h, 8, inst ? UI_RGB(10, 132, 255) : UI_RGB(80, 80, 86), 255);
        ui_text_center(bx + btn_w / 2, by + (btn_h - ui_font_height(UF_UIB)) / 2 + 1, inst ? "Open" : "Missing", UF_UIB, 0xFFFFFF);
        if (i < NCAT - 1) ui_blend_rect(x + 16, ry + ROW_H - 6, w - 32, 1, 0xFFFFFF, 20);
    }
    ui_text(x + 14, y + h - 18, "Enter/click Open: launches in the Terminal.  Up/Down: select.  Esc: close.", UF_SMALL, UI_RGB(120, 120, 128));
}

static void store_open_sel(int i) {
    if (i < 0 || i >= NCAT) return;
    if (!appstore_ui_installed_size(&CATALOG[i])) return;
    desktop_terminal_run(CATALOG[i].bin);
}

static void store_key(int id, char key) {
    unsigned char k = (unsigned char)key;
    if (k == KEYC_DOWN && sel + 1 < NCAT) sel++;
    else if (k == KEYC_UP && sel > 0) sel--;
    else if (key == '\n') store_open_sel(sel);
    else if (key == 27) window_destroy(id);
    else if (key >= '1' && key < '1' + NCAT) { sel = key - '1'; store_open_sel(sel); }
}

static void store_click(int id, int px, int py) {
    window_t* w = window_get(id);
    if (!w) return;
    int ax = w->px + px, ay = w->py + WIN_TITLEBAR_H + py;
    for (int i = 0; i < NCAT; i++) {
        if (ax >= btn_x[i] && ax < btn_x[i] + btn_w && ay >= btn_y[i] && ay < btn_y[i] + btn_h) { sel = i; store_open_sel(i); return; }
        if (ay >= btn_y[i] - 14 && ay < btn_y[i] - 14 + ROW_H) sel = i;
    }
}

void appstore_ui_open(void) {
    /* already open: focus */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = window_get(i);
        if (w && w->active && strcmp(w->title, "App Store") == 0) { window_focus(i); return; }
    }
    int c = (int)fb_get_width() / 16, r = (int)fb_get_height() / 16;
    int cw = 92, ch = 30;
    win_id = window_create("App Store", c - cw / 2, r / 2 - ch / 2, cw, ch, store_draw, store_key);
    if (win_id >= 0) window_set_on_click(win_id, store_click);
    sel = 0;
}
