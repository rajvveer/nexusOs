/* ============================================================================
 * NexusOS — Spotlight search and Launchpad (Implementation)
 * ============================================================================ */

#include "spotlight.h"
#include "macui.h"
#include "framebuffer.h"
#include "string.h"

/* --------------------------------------------------------------------------
 * Search corpus: every registered app plus a few system actions.
 * -------------------------------------------------------------------------- */
typedef struct { const char* name; int action; int icon; const char* kind; } entry_t;

static const entry_t SYSTEM[] = {
    { "About This NexusOS", UI_ACT_ABOUT,     ICON_INFO,     "System" },
#ifndef NEXUS_AURORA
    { "Lock Screen",        UI_ACT_LOCK,      ICON_SETTINGS, "System" },
    { "Restart",            UI_ACT_RESTART,   ICON_SETTINGS, "System" },
    { "Shut Down",          UI_ACT_SHUTDOWN,  ICON_SETTINGS, "System" },
    { "Sleep",              UI_ACT_SLEEP,     ICON_SETTINGS, "System" },
    { "Exit to Console",    UI_ACT_CONSOLE,   ICON_TERMINAL, "System" },
    { "Keyboard Shortcuts", UI_ACT_SHORTCUTS, ICON_HELP,     "System" },
    { "Safari",             32,               ICON_WEB,      "Application" },   /* Phase 57: alias of Browser */
#else
    { "Shut Down",          UI_ACT_SHUTDOWN,  ICON_SETTINGS, "System" },
    { "Desktop Help",       14,               ICON_HELP,     "System" },
#endif
};
#define NSYS (int)(sizeof(SYSTEM) / sizeof(SYSTEM[0]))

#define SP_W        680
#define SP_FIELD_H  56
#define SP_ROW_H    40
#define SP_MAX_ROWS 8
#define SP_QMAX     40

static bool sp_open = false;
static char query[SP_QMAX];
static int  qlen = 0, sel = 0;
static entry_t results[SP_MAX_ROWS];
static int  nres = 0;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* 0 = no match, higher = better (prefix of the name or of a word). */
static int score(const char* name, const char* q) {
    if (!q[0]) return 0;
    for (int i = 0; name[i]; i++) {
        if (i > 0 && name[i - 1] != ' ') continue;
        int k = 0;
        while (q[k] && name[i + k] && lower(name[i + k]) == lower(q[k])) k++;
        if (!q[k]) return i == 0 ? 3 : 2;
    }
    for (int i = 0; name[i]; i++) {               /* plain substring */
        int k = 0;
        while (q[k] && name[i + k] && lower(name[i + k]) == lower(q[k])) k++;
        if (!q[k]) return 1;
    }
    return 0;
}

static void refilter(void) {
    int count;
    const ui_app_t* apps = ui_apps(&count);
    nres = 0;
    for (int pass = 3; pass >= 1; pass--) {
        for (int i = 0; i < count && nres < SP_MAX_ROWS; i++) {
            if (apps[i].action == UI_ACT_LAUNCHPAD && pass < 3) continue;
            if (score(apps[i].name, query) == pass) {
                entry_t e = { apps[i].name, apps[i].action, apps[i].icon, "Application" };
                results[nres++] = e;
            }
        }
        for (int i = 0; i < NSYS && nres < SP_MAX_ROWS; i++)
            if (score(SYSTEM[i].name, query) == pass) results[nres++] = SYSTEM[i];
    }
    if (sel >= nres) sel = nres ? nres - 1 : 0;
}

void spotlight_open(void)  { sp_open = true; qlen = 0; query[0] = 0; sel = 0; nres = 0; }
void spotlight_close(void) { sp_open = false; }
bool spotlight_is_open(void) { return sp_open; }

static int sp_x(void) { return (int)fb_get_width() / 2 - SP_W / 2; }
static int sp_y(void) { return (int)fb_get_height() * 22 / 100; }
static int sp_h(void) { return SP_FIELD_H + (nres ? nres * SP_ROW_H + 14 : 0); }

void spotlight_draw(void) {
    if (!sp_open) return;
    int x = sp_x(), y = sp_y(), h = sp_h();
    ui_shadow(x, y, SP_W, h, 16, 34, 140, 12);
    ui_rrect(x, y, SP_W, h, 16, UI_RGB(38, 38, 42), 238);
    ui_rrect_stroke(x, y, SP_W, h, 16, UI_RGB(255, 255, 255), 44);

    /* magnifier + query */
    int fy = y + (SP_FIELD_H - ui_font_height(UF_TITLE)) / 2;
    ui_ring(x + 30, y + SP_FIELD_H / 2 - 2, 9, 2, UI_RGB(170, 170, 176), 255);
    ui_line(x + 36, y + SP_FIELD_H / 2 + 4, x + 42, y + SP_FIELD_H / 2 + 10, 3, UI_RGB(170, 170, 176), 255);
    int tx = x + 56;
    if (qlen) {
        int end = ui_text(tx, fy, query, UF_TITLE, UI_RGB(240, 240, 244));
        ui_blend_rect(end + 2, y + 15, 2, SP_FIELD_H - 30, UI_RGB(10, 132, 255), 255);
    } else {
        ui_blend_rect(tx, y + 15, 2, SP_FIELD_H - 30, UI_RGB(10, 132, 255), 255);
        ui_text(tx + 6, fy, "Spotlight Search", UF_TITLE, UI_RGB(120, 120, 126));
    }

    if (!nres) return;
    ui_blend_rect(x + 1, y + SP_FIELD_H, SP_W - 2, 1, 0xFFFFFF, 30);
    int ry = y + SP_FIELD_H + 7;
    for (int i = 0; i < nres; i++, ry += SP_ROW_H) {
        if (i == sel) ui_rrect(x + 8, ry, SP_W - 16, SP_ROW_H, 8, UI_RGB(10, 100, 230), 255);
        ui_icon(results[i].icon, x + 18, ry + 5, 30);
        ui_text(x + 60, ry + (SP_ROW_H - ui_font_height(UF_UI)) / 2 + 1, results[i].name, UF_UI, 0xFFFFFF);
        int kw = ui_text_width(results[i].kind, UF_SMALL);
        ui_text(x + SP_W - 24 - kw, ry + (SP_ROW_H - ui_font_height(UF_SMALL)) / 2 + 1, results[i].kind, UF_SMALL,
                i == sel ? UI_RGB(220, 230, 255) : UI_RGB(140, 140, 146));
    }
}

#ifdef NEXUS_AURORA
bool spotlight_bounds(int* bx, int* by, int* bw, int* bh) {
    if (!sp_open) return false;
    *bx = sp_x() - 36; *by = sp_y() - 24; *bw = SP_W + 72; *bh = sp_h() + 50 + 24;
    return true;
}
/* The screen reader's view (aurora_a11y.c): the query, the results and the selected one. */
const char* spotlight_a11y(int* count, int* selected) { *count = sp_open ? nres : 0; *selected = sp_open && nres ? sel : -1; return sp_open ? query : NULL; }
const char* spotlight_a11y_result(int i, const char** kind) {
    if (i < 0 || i >= nres) { *kind = ""; return ""; }
    *kind = results[i].kind; return results[i].name;
}
#endif

int spotlight_key(char key) {
    if (!sp_open) return 0;
    unsigned char k = (unsigned char)key;
    if (k == 27) { spotlight_close(); return 0; }
    if (k == '\n') {
        int a = nres ? results[sel].action : 0;
        spotlight_close();
        return a;
    }
    if (k == 0x81) { if (sel + 1 < nres) sel++; return 0; }
    if (k == 0x80) { if (sel > 0) sel--; return 0; }
    if (k == '\b') { if (qlen) query[--qlen] = 0; sel = 0; refilter(); return 0; }
    if (k >= 32 && k < 127 && qlen < SP_QMAX - 1) {
        query[qlen++] = (char)k; query[qlen] = 0; sel = 0; refilter();
    }
    return 0;
}

bool spotlight_hit(int px, int py) {
    return sp_open && px >= sp_x() && px < sp_x() + SP_W && py >= sp_y() && py < sp_y() + sp_h();
}

int spotlight_click(int px, int py) {
    if (!spotlight_hit(px, py)) { spotlight_close(); return 0; }
    int ry = sp_y() + SP_FIELD_H + 7;
    if (py < ry) return 0;
    int i = (py - ry) / SP_ROW_H;
    if (i < 0 || i >= nres) return 0;
    int a = results[i].action;
    spotlight_close();
    return a;
}

/* --------------------------------------------------------------------------
 * Launchpad
 * -------------------------------------------------------------------------- */
#define LP_ICON 84
#define LP_CW   164
#define LP_CH   146
#define LP_COLS 7
#define LP_MAX  192   /* registry capacity: the fixed apps plus installed Windows programs (Aurora) */

static bool lp_open = false;
static int  lp_sel = 0, lp_page = 0;
static int  lp_idx[LP_MAX];

void launchpad_open(void)  { lp_open = true; lp_sel = 0; lp_page = 0; }
void launchpad_close(void) { lp_open = false; }
bool launchpad_is_open(void) { return lp_open; }

/* Launchpad lists every app except itself (registry order, at most LP_MAX). */
static int lp_items(int* idx) {
    int count, n = 0;
    const ui_app_t* apps = ui_apps(&count);
    for (int i = 0; i < count && n < LP_MAX; i++) if (apps[i].action != UI_ACT_LAUNCHPAD && apps[i].icon != ICON_TRASH) idx[n++] = i;
    return n;
}

/* Pages of whole rows between the search pill and the page dots. */
static int lp_per_page(void) {
    int rows = ((int)fb_get_height() - 260) / LP_CH;
    return (rows < 1 ? 1 : rows) * LP_COLS;
}
static int lp_pages(int n) { int per = lp_per_page(); return n > per ? (n + per - 1) / per : 1; }

/* Grid origin of the shown page: a single page is centred on its items, several pages share the full-page grid. */
static void lp_origin(int n, int* ox, int* oy) {
    int per = lp_per_page(), shown = n > per ? per : n;
    int cols = shown < LP_COLS ? shown : LP_COLS, rows = (shown + LP_COLS - 1) / LP_COLS;
    *ox = (int)fb_get_width() / 2 - cols * LP_CW / 2;
    *oy = (int)fb_get_height() / 2 - rows * LP_CH / 2 + 10;
}
static int lp_dot_x(int pages, int page) { return (int)fb_get_width() / 2 - (pages - 1) * 9 + page * 18; }

void launchpad_draw(void) {
    if (!lp_open) return;
    int n = lp_items(lp_idx), count;
    const ui_app_t* apps = ui_apps(&count);
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    ui_wallpaper_draw();
    ui_blend_rect(0, 0, sw, sh, UI_RGB(14, 12, 26), 150);

    /* search pill */
    int pw = 220, px = sw / 2 - pw / 2, py = 54;
    ui_rrect(px, py, pw, 30, 8, 0xFFFFFF, 34);
    ui_ring(px + 18, py + 14, 5, 1, UI_RGB(200, 200, 206), 255);
    ui_line(px + 22, py + 18, px + 25, py + 21, 1, UI_RGB(200, 200, 206), 255);
    ui_text(px + 32, py + (30 - ui_font_height(UF_UI)) / 2 + 1, "Search", UF_UI, UI_RGB(200, 200, 206));

    int per = lp_per_page(), pages = lp_pages(n);
    if (lp_page >= pages) lp_page = pages - 1;
    int first = lp_page * per, shown = n - first < per ? n - first : per;
    int ox, oy;
    lp_origin(n, &ox, &oy);
    for (int k = 0; k < shown; k++) {
        const ui_app_t* a = &apps[lp_idx[first + k]];
        int cx = ox + (k % LP_COLS) * LP_CW, cy = oy + (k / LP_COLS) * LP_CH;
        int ix = cx + (LP_CW - LP_ICON) / 2;
        if (first + k == lp_sel) ui_rrect(ix - 10, cy - 10, LP_ICON + 20, LP_ICON + 46, 16, 0xFFFFFF, 30);
        ui_icon(a->icon, ix, cy, LP_ICON);
        /* long names (Windows programs) are shortened to their cell with an ellipsis */
        char label[72]; unsigned length = strlen(a->name) < 64 ? strlen(a->name) : 64;
        memcpy(label, a->name, length); label[length] = 0;
        while (length > 1 && ui_text_width(label, UF_UI) > LP_CW - 10) { length--; memcpy(label + length, "...", 4); }
        ui_text_center(cx + LP_CW / 2, cy + LP_ICON + 8, label, UF_UI, UI_RGB(244, 244, 248));
    }
    /* page dots (click one, or move the selection past the page edge) */
    for (int p = 0; p < pages; p++) ui_circle(lp_dot_x(pages, p), sh - 120, 3, 0xFFFFFF, p == lp_page ? 220 : 90);
}

int launchpad_key(char key) {
    if (!lp_open) return 0;
    int n = lp_items(lp_idx), count;
    const ui_app_t* apps = ui_apps(&count);
    unsigned char k = (unsigned char)key;
    if (k == 27) { launchpad_close(); return 0; }
    if (k == 0x83 && lp_sel + 1 < n) lp_sel++;
    else if (k == 0x82 && lp_sel > 0) lp_sel--;
    else if (k == 0x81 && lp_sel + LP_COLS < n) lp_sel += LP_COLS;
    else if (k == 0x80 && lp_sel >= LP_COLS) lp_sel -= LP_COLS;
    else if (k == '\n' && lp_sel < n) { launchpad_close(); return apps[lp_idx[lp_sel]].action; }
    lp_page = lp_sel / lp_per_page();
    return 0;
}

int launchpad_click(int px, int py) {
    if (!lp_open) return 0;
    int n = lp_items(lp_idx), count;
    const ui_app_t* apps = ui_apps(&count);
    int per = lp_per_page(), pages = lp_pages(n), sh = (int)fb_get_height();
    if (pages > 1 && py >= sh - 130 && py < sh - 110)
        for (int p = 0; p < pages; p++)
            if (px >= lp_dot_x(pages, p) - 9 && px < lp_dot_x(pages, p) + 9) { lp_page = p; lp_sel = p * per; return 0; }
    if (lp_page >= pages) lp_page = pages - 1;
    int first = lp_page * per, shown = n - first < per ? n - first : per;
    int ox, oy;
    lp_origin(n, &ox, &oy);
    for (int k = 0; k < shown; k++) {
        int cx = ox + (k % LP_COLS) * LP_CW, cy = oy + (k / LP_COLS) * LP_CH;
        int ix = cx + (LP_CW - LP_ICON) / 2;
        if (px >= ix - 10 && px < ix + LP_ICON + 10 && py >= cy - 10 && py < cy + LP_ICON + 36) {
            launchpad_close();
            return apps[lp_idx[first + k]].action;
        }
    }
    launchpad_close();
    return 0;
}
#ifdef NEXUS_AURORA
const char* launchpad_a11y(int* count, int* selected) {
    int idx[LP_MAX], n = lp_items(idx), c; const ui_app_t* apps = ui_apps(&c);
    *count = lp_open ? n : 0; *selected = lp_open && lp_sel < n ? lp_sel : -1;
    return *selected >= 0 ? apps[idx[lp_sel]].name : NULL;
}
#endif
