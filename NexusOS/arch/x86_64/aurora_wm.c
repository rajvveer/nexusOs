/* Aurora window management: shortcuts, the Alt+Tab switcher, workspaces and their overview (aurora_wm.h). */
#define _GNU_SOURCE
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aurora_wm.h"
#include "../../kernel/macui.h"
#include "../../kernel/framebuffer.h"

#define ENTRIES 48
#define THUMB_W 320
#define THUMB_H 200
struct entry {
    struct aurora_window* window; char title[96], app_id[64]; int icon, screen, workspace; bool minimized;
    int x, y, w, h;                     /* overview: desktop rectangle of the thumbnail */
    uint32_t* thumb; int tw, th;        /* overview thumbnail (THUMB_W x THUMB_H at most) */
};
static struct entry entries[ENTRIES]; static int entry_count, selected = -1;
static enum { CLOSED, SWITCHER, OVERVIEW } mode;
static struct { int x, y, w, h; } panel, workspace_box[AURORA_WORKSPACES];
static int hover = -1, dragged = -1, drag_x, drag_y, press_x, press_y; static bool pressed;

static bool alive(struct aurora_window* w) {   /* still a toplevel of the desktop (entries never dereference dead ones) */
    struct aurora_window* list[ENTRIES]; int n = aurora_host_windows(list, ENTRIES);
    for (int i = 0; i < n; i++) if (list[i] == w) return true;
    return false;
}
static void free_entries(void) { for (int i = 0; i < entry_count; i++) free(entries[i].thumb); entry_count = 0; }
/* Windows of the current workspace (all workspaces for the switcher when all is set), topmost first. */
static void collect(bool thumbnails) {
    free_entries();
    struct aurora_window* list[ENTRIES]; int n = aurora_host_windows(list, ENTRIES), workspace = aurora_host_workspace();
    for (int i = 0; i < n && entry_count < ENTRIES; i++) {
        struct aurora_window_info info; aurora_host_window_info(list[i], &info);
        if (info.workspace != workspace) continue;
        struct entry* e = &entries[entry_count++]; memset(e, 0, sizeof(*e));
        e->window = list[i]; snprintf(e->title, sizeof(e->title), "%s", info.title ? info.title : ""); snprintf(e->app_id, sizeof(e->app_id), "%s", info.app_id ? info.app_id : "");
        e->icon = info.icon; e->screen = info.screen; e->workspace = info.workspace; e->minimized = info.minimized;
        e->x = info.x; e->y = info.y; e->w = info.w; e->h = info.h;
        if (thumbnails && info.w > 0 && info.h > 0) {
            int tw = THUMB_W, th = (int)((int64_t)info.h * THUMB_W / info.w);
            if (th > THUMB_H) { th = THUMB_H; tw = (int)((int64_t)info.w * THUMB_H / info.h); }
            if (tw < 8) tw = 8; if (th < 8) th = 8;
            e->thumb = malloc((size_t)tw * th * 4);
            if (e->thumb && !aurora_host_thumbnail(e->window, e->thumb, tw, th)) { free(e->thumb); e->thumb = NULL; }
            e->tw = tw; e->th = th;
        }
    }
}

/* ---- switcher ---- */
static void switcher_layout(void) {
    int sx, sy, sw, sh; struct aurora_window* focus = aurora_host_focus(); int screen = -1;
    if (focus) { struct aurora_window_info info; aurora_host_window_info(focus, &info); screen = info.screen; }
    aurora_host_screen_rect(screen, &sx, &sy, &sw, &sh);
    int per_row = (sw - 80) / 136; if (per_row < 1) per_row = 1; if (per_row > entry_count) per_row = entry_count;
    int rows = (entry_count + per_row - 1) / per_row;
    panel.w = per_row * 136 + 24; panel.h = rows * 150 + 24; panel.x = sx + (sw - panel.w) / 2; panel.y = sy + (sh - panel.h) / 2;
    for (int i = 0; i < entry_count; i++) { entries[i].x = panel.x + 12 + (i % per_row) * 136; entries[i].y = panel.y + 12 + (i / per_row) * 150; entries[i].w = 128; entries[i].h = 142; }
}
static struct aurora_window* original;   /* focused when the overlay opened (back to it on Esc) */
static void switcher_open(int direction) {
    collect(false);
    if (!entry_count) return;
    original = aurora_host_focus(); aurora_host_focus_none();   /* the old window must not see Alt released (menu activation) */
    mode = SWITCHER; selected = entry_count > 1 ? (direction > 0 ? 1 : entry_count - 1) : 0;
    switcher_layout(); aurora_host_redraw();
    printf("[64] AURORA SWITCHER open windows=%d\n", entry_count); fflush(stdout);
}
static void close_overlay(void) { if (mode == CLOSED) return; mode = CLOSED; free_entries(); selected = hover = dragged = -1; pressed = false; aurora_host_redraw(); }
static void cancel(void) { struct aurora_window* back = original; close_overlay(); if (back && alive(back)) aurora_host_activate(back); }
static void activate(int index) {
    if (index < 0 || index >= entry_count) return;
    struct aurora_window* w = entries[index].window; char app[64]; snprintf(app, sizeof(app), "%s", entries[index].app_id);
    close_overlay();
    if (alive(w)) { aurora_host_activate(w); printf("[64] AURORA SWITCHED app=%s\n", app); fflush(stdout); }
}

/* ---- overview (all windows of the workspace, per display; workspace strip on the primary display) ---- */
static void overview_layout(void) {
    int px, py, pw, ph; aurora_host_screen_rect(-1, &px, &py, &pw, &ph);
    int bw = 176, bh = 99, gap = 18, strip = AURORA_WORKSPACES * bw + (AURORA_WORKSPACES - 1) * gap;
    for (int i = 0; i < AURORA_WORKSPACES; i++) { workspace_box[i].x = px + (pw - strip) / 2 + i * (bw + gap); workspace_box[i].y = py + 40; workspace_box[i].w = bw; workspace_box[i].h = bh; }
    for (int screen = 0; screen < 8; screen++) {   /* a grid per display */
        int n = 0; for (int i = 0; i < entry_count; i++) n += entries[i].screen == screen;
        if (!n) continue;
        int sx, sy, sw, sh; aurora_host_screen_rect(screen, &sx, &sy, &sw, &sh);
        int top = sy + (sx == px && sy == py ? 40 + bh + 40 : 40), bottom = sy + sh - 40, columns = 1;
        while (columns * columns < n) columns++;
        int rows = (n + columns - 1) / columns, cw = (sw - 80) / columns, ch = (bottom - top) / (rows ? rows : 1), k = 0;
        for (int i = 0; i < entry_count; i++) {
            struct entry* e = &entries[i]; if (e->screen != screen) continue;
            int cx = sx + 40 + (k % columns) * cw, cy = top + (k / columns) * ch; k++;
            int w = cw - 40, h = ch - 50; if (w < 40) w = 40; if (h < 30) h = 30;
            /* keep the window's aspect, at most its own size */
            int iw = e->w > 0 ? e->w : 1, ih = e->h > 0 ? e->h : 1;
            int64_t fw = (int64_t)w * 1000 / iw, fh = (int64_t)h * 1000 / ih, f = fw < fh ? fw : fh; if (f > 1000) f = 1000;
            e->w = (int)(iw * f / 1000); e->h = (int)(ih * f / 1000); if (e->w < 24) e->w = 24; if (e->h < 16) e->h = 16;
            e->x = cx + (cw - 40 - e->w) / 2 + 20; e->y = cy + (ch - 50 - e->h) / 2;
        }
    }
}
static void overview_open(void) {
    collect(true); original = aurora_host_focus(); aurora_host_focus_none(); mode = OVERVIEW; selected = entry_count ? 0 : -1; overview_layout(); aurora_host_redraw();
    printf("[64] AURORA OVERVIEW open windows=%d workspace=%d\n", entry_count, aurora_host_workspace() + 1); fflush(stdout);
}
static void overview_refresh(void) { if (mode == OVERVIEW) { int keep = selected; collect(true); overview_layout(); selected = keep < entry_count ? keep : entry_count - 1; aurora_host_redraw(); } }
static int entry_at(int x, int y) {
    for (int i = 0; i < entry_count; i++) if (x >= entries[i].x && y >= entries[i].y && x < entries[i].x + entries[i].w && y < entries[i].y + entries[i].h + 24) return i;
    return -1;
}
static int workspace_at(int x, int y) {
    if (mode != OVERVIEW) return -1;
    for (int i = 0; i < AURORA_WORKSPACES; i++) if (x >= workspace_box[i].x && y >= workspace_box[i].y && x < workspace_box[i].x + workspace_box[i].w && y < workspace_box[i].y + workspace_box[i].h) return i;
    return -1;
}

/* ---- shortcuts ---- */
static void show_desktop(void) {
    struct aurora_window* list[ENTRIES]; int n = aurora_host_windows(list, ENTRIES), workspace = aurora_host_workspace(); bool any = false;
    for (int i = 0; i < n; i++) { struct aurora_window_info info; aurora_host_window_info(list[i], &info); if (info.workspace == workspace && !info.minimized) any = true; }
    aurora_host_desktop_toggle(any);
}
bool aurora_wm_key(unsigned key, int value, unsigned mods) {
    bool super = mods & AURORA_MOD_SUPER, shift = mods & AURORA_MOD_SHIFT, ctrl = mods & AURORA_MOD_CTRL, alt = mods & AURORA_MOD_ALT;
    if (mode == SWITCHER) {
        if (!value) { if ((key == KEY_LEFTALT || key == KEY_RIGHTALT || key == KEY_LEFTMETA || key == KEY_RIGHTMETA) && !(mods & (AURORA_MOD_ALT | AURORA_MOD_SUPER))) activate(selected); return false; }
        if (key == KEY_TAB || key == KEY_RIGHT || key == KEY_LEFT || key == KEY_GRAVE) {
            int step = (key == KEY_LEFT || shift) ? -1 : 1; selected = (selected + step + entry_count) % entry_count; aurora_host_redraw(); return true; }
        if (key == KEY_ESC) { cancel(); return true; }
        if (key == KEY_ENTER) { activate(selected); return true; }
        if (key == KEY_Q && entry_count) { struct aurora_window* w = entries[selected].window; if (alive(w)) aurora_host_close(w); return true; }
        return true;
    }
    if (mode == OVERVIEW) {
        if (!value) return true;
        if (key == KEY_ESC || (super && key == KEY_W)) { cancel(); return true; }
        if (key == KEY_ENTER) { activate(selected); return true; }
        if (key == KEY_RIGHT || key == KEY_TAB) { if (entry_count) selected = (selected + 1) % entry_count; aurora_host_redraw(); return true; }
        if (key == KEY_LEFT) { if (entry_count) selected = (selected + entry_count - 1) % entry_count; aurora_host_redraw(); return true; }
        if (super && key >= KEY_1 && key < KEY_1 + AURORA_WORKSPACES) { aurora_host_set_workspace((int)(key - KEY_1)); overview_refresh(); return true; }
        return true;   /* the overview owns the keyboard while open */
    }
    if (!value) return false;
    if (alt && !super && key == KEY_TAB) { switcher_open(shift ? -1 : 1); return mode == SWITCHER; }
    if (!super) return false;
    struct aurora_window* focus = aurora_host_focus();
    switch (key) {
    case KEY_LEFT: case KEY_RIGHT:
        if (ctrl) { int ws = aurora_host_workspace() + (key == KEY_LEFT ? -1 : 1); if (shift && focus && ws >= 0 && ws < AURORA_WORKSPACES) aurora_host_window_workspace(focus, ws);
                    aurora_host_set_workspace(ws); return true; }
        if (!focus) return true;
        if (shift) { aurora_host_move_screen(focus, key == KEY_LEFT ? -1 : 1); return true; }
        { struct aurora_window_info info; aurora_host_window_info(focus, &info); int want = key == KEY_LEFT ? AURORA_TILE_LEFT : AURORA_TILE_RIGHT;
          if (info.tile == want) aurora_host_tile(focus, AURORA_TILE_NONE, false);
          else if (info.tile && info.tile != want) aurora_host_tile(focus, AURORA_TILE_NONE, false);   /* left half -> Super+Right: back to normal */
          else aurora_host_tile(focus, want, false); }
        return true;
    case KEY_UP: if (focus) aurora_host_tile(focus, AURORA_TILE_NONE, true); return true;
    case KEY_DOWN:
        if (focus) { struct aurora_window_info info; aurora_host_window_info(focus, &info);
                     if (info.maximized || info.tile) aurora_host_tile(focus, AURORA_TILE_NONE, false); else aurora_host_minimize(focus, true); }
        return true;
    case KEY_D: show_desktop(); return true;
    case KEY_L: aurora_host_lock(); return true;
    case KEY_W: overview_open(); return true;
    case KEY_TAB: switcher_open(shift ? -1 : 1); return true;
    }
    if (key >= KEY_1 && key < KEY_1 + AURORA_WORKSPACES) {
        int ws = (int)(key - KEY_1);
        if (shift) { if (focus) aurora_host_window_workspace(focus, ws); }
        else aurora_host_set_workspace(ws);
        return true;
    }
    return false;
}
bool aurora_wm_overlay(void) { return mode != CLOSED; }
bool aurora_wm_overview(void) { return mode == OVERVIEW; }
bool aurora_wm_overlay_bounds(int* x, int* y, int* w, int* h) {
    if (mode == SWITCHER) { *x = panel.x - 24; *y = panel.y - 24; *w = panel.w + 48; *h = panel.h + 48; return true; }
    if (mode == OVERVIEW) {   /* every display */
        int left = 1 << 30, top = 1 << 30, right = -(1 << 30), bottom = -(1 << 30), n = aurora_host_screen_count();
        for (int i = 0; i < 8 && n; i++) { int sx, sy, sw, sh; aurora_host_screen_rect(i, &sx, &sy, &sw, &sh);
            if (sx < left) left = sx; if (sy < top) top = sy; if (sx + sw > right) right = sx + sw; if (sy + sh > bottom) bottom = sy + sh; }
        *x = left; *y = top; *w = right - left; *h = bottom - top; return *w > 0 && *h > 0;
    }
    return false;
}
/* A thumbnail into the logical rectangle x,y,w,h (macui scales it to the display's resolution). */
static void blit(const struct entry* e, int x, int y, int w, int h) {
    if (!e->thumb) { ui_rrect(x, y, w, h, 6, 0x2A2A30, 255); ui_icon(e->icon, x + w / 2 - 24, y + h / 2 - 24, 48); return; }
    ui_image(x, y, w, h, e->thumb, e->tw, e->th, e->tw, false);
}
void aurora_wm_draw(int dx, int dy, int dw, int dh, int k) {
    if (mode == SWITCHER) {
        int x = (panel.x - dx) * k, y = (panel.y - dy) * k;
        if (panel.x + panel.w < dx || panel.x > dx + dw || panel.y + panel.h < dy || panel.y > dy + dh) return;
        ui_shadow(x, y, panel.w * k, panel.h * k, 18 * k, 18 * k, 90, 4 * k);
        ui_rrect(x, y, panel.w * k, panel.h * k, 18 * k, 0x1C1C22, 235); ui_rrect_stroke(x, y, panel.w * k, panel.h * k, 18 * k, 0xFFFFFF, 40);
        for (int i = 0; i < entry_count; i++) {
            const struct entry* e = &entries[i]; int ex = (e->x - dx) * k, ey = (e->y - dy) * k;
            if (i == selected) ui_rrect(ex, ey, e->w * k, e->h * k, 12 * k, 0x3A6FD8, 200);
            ui_icon(e->icon, ex + 24 * k, ey + 12 * k, 80 * k);
            if (e->minimized) ui_text(ex + 8 * k, ey + 4 * k, "-", UF_SMALL, 0xC0C0C8);
            char title[24]; snprintf(title, sizeof(title), "%.21s%s", e->title, strlen(e->title) > 21 ? "..." : "");
            ui_text_center(ex + e->w * k / 2, ey + 104 * k, title, UF_SMALL, 0xF2F2F5);
        }
        return;
    }
    if (mode != OVERVIEW) return;
    int px, py, pw, ph; aurora_host_screen_rect(-1, &px, &py, &pw, &ph);
    ui_blend_rect(0, 0, dw * k, dh * k, 0x08080C, 165);   /* the desktop, dimmed */
    if (dx == px && dy == py) {   /* the workspace strip */
        for (int i = 0; i < AURORA_WORKSPACES; i++) {
            int x = (workspace_box[i].x - dx) * k, y = (workspace_box[i].y - dy) * k, w = workspace_box[i].w * k, h = workspace_box[i].h * k;
            ui_rrect(x, y, w, h, 8 * k, i == aurora_host_workspace() ? 0x3A6FD8 : 0x26262E, 230);
            ui_rrect_stroke(x, y, w, h, 8 * k, 0xFFFFFF, i == aurora_host_workspace() ? 160 : 50);
            char label[24]; snprintf(label, sizeof(label), "Desktop %d", i + 1); ui_text_center(x + w / 2, y + h / 2 - 8 * k, label, UF_UI, 0xF2F2F5);
        }
    }
    for (int i = 0; i < entry_count; i++) {
        const struct entry* e = &entries[i];
        int ex = e->x, ey = e->y; if (i == dragged) { ex = drag_x - e->w / 2; ey = drag_y - e->h / 2; }
        if (ex + e->w < dx || ex > dx + dw || ey + e->h + 24 < dy || ey > dy + dh) continue;
        int x = (ex - dx) * k, y = (ey - dy) * k, w = e->w * k, h = e->h * k;
        ui_shadow(x, y, w, h, 6 * k, 14 * k, 110, 3 * k);
        blit(e, x, y, w, h);
        if (i == selected || i == hover) ui_rrect_stroke(x - 3 * k, y - 3 * k, w + 6 * k, h + 6 * k, 8 * k, i == selected ? 0x5A9CFF : 0xFFFFFF, 220);
        char title[48]; snprintf(title, sizeof(title), "%.40s%s", e->title, strlen(e->title) > 40 ? "..." : "");
        ui_text_center(x + w / 2, y + h + 6 * k, title, UF_SMALL, 0xF2F2F5);
    }
}
bool aurora_wm_pointer(int x, int y, int button, bool down) {
    if (mode == CLOSED) return false;
    if (mode == SWITCHER) { if (down && button == BTN_LEFT) { int i = entry_at(x, y); if (i >= 0) activate(i); else cancel(); } return true; }
    if (button != BTN_LEFT) return true;
    if (down) { pressed = true; press_x = x; press_y = y; dragged = -1; return true; }
    if (!pressed) return true;
    pressed = false;
    int target_ws = workspace_at(x, y);
    if (dragged >= 0) {   /* a thumbnail dropped on a workspace moves the window there */
        int index = dragged; dragged = -1;
        if (target_ws >= 0 && alive(entries[index].window)) aurora_host_window_workspace(entries[index].window, target_ws);
        overview_refresh(); return true;
    }
    if (target_ws >= 0) { aurora_host_set_workspace(target_ws); overview_refresh(); return true; }
    int i = entry_at(x, y);
    if (i >= 0) activate(i); else cancel();
    return true;
}
void aurora_wm_motion(int x, int y) {
    if (mode != OVERVIEW) return;
    int h = entry_at(x, y);
    if (pressed && dragged < 0) { int i = entry_at(press_x, press_y); if (i >= 0 && (abs(x - press_x) > 8 || abs(y - press_y) > 8)) dragged = i; }
    if (dragged >= 0) { drag_x = x; drag_y = y; aurora_host_redraw(); }
    if (h != hover) { hover = h; aurora_host_redraw(); }
}
void aurora_wm_windows_changed(void) {
    if (mode == CLOSED) return;
    {   /* the same windows on this workspace (commits and focus changes land here too): nothing to do */
        struct aurora_window* list[ENTRIES]; int n = aurora_host_windows(list, ENTRIES), same = 0, workspace = aurora_host_workspace(), here = 0;
        for (int i = 0; i < n; i++) { struct aurora_window_info info; aurora_host_window_info(list[i], &info); if (info.workspace != workspace) continue;
            here++; for (int k = 0; k < entry_count; k++) if (entries[k].window == list[i] && entries[k].minimized == info.minimized) { same++; break; } }
        if (here == entry_count && same == entry_count) return;
    }
    if (mode == SWITCHER) {   /* drop windows that went away */
        int n = 0; for (int i = 0; i < entry_count; i++) { if (alive(entries[i].window)) entries[n++] = entries[i]; else free(entries[i].thumb); }
        entry_count = n; if (!n) { close_overlay(); return; } if (selected >= n) selected = n - 1; switcher_layout(); aurora_host_redraw();
    } else if (mode == OVERVIEW) overview_refresh();
}
