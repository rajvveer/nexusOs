/* ============================================================================
 * NexusOS — Window Manager (Implementation) — Phase 17
 * ============================================================================
 * Pixel-perfect windows with rounded borders, gradient titlebars,
 * drop shadows, 8-direction resize, smooth dragging, and animations.
 * ============================================================================ */

#include "window.h"
#include "gui.h"
#include "gfx.h"
#include "vga.h"
#include "theme.h"
#include "string.h"
#include "framebuffer.h"
#include "font.h"
#include "macui.h"
#include "kguard.h"
#include "notify.h"
#include "syslog.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* Window table */
static window_t windows[MAX_WINDOWS];
static int next_z = 1;

/* ============================================================================
 * Phase 55: crash containment for app callbacks
 * ============================================================================
 * Every app callback (draw / key / click) runs inside a kguard. A fault in the
 * app (NULL deref, bad pointer, divide by zero, write into kernel text...) is
 * recovered by the fault handlers: the callback's frames are abandoned, the
 * offending window is closed, two toasts explain what happened and the
 * desktop carries on composing the remaining windows.
 * (Locals that must survive the second return of kguard_try are volatile:
 * this file is compiled at -O2.)
 * ============================================================================ */
static void win_append(char* b, int n, const char* s) {
    int len = (int)strlen(b);
    while (*s && len < n - 1) b[len++] = *s++;
    b[len] = '\0';
}

static void win_crashed(int id, int rc, const char* phase) {
    char title[16], msg[48], det[48];
    const char* t = (id >= 0 && id < MAX_WINDOWS && windows[id].title[0]) ? windows[id].title : "App";
    strncpy(title, t, 15); title[15] = '\0';

    window_destroy(id);                       /* never composed or clicked again */

    msg[0] = '\0';
    win_append(msg, sizeof(msg), title);
    win_append(msg, sizeof(msg), " crashed - window closed");
    notify_push(msg);
    /* Second toast: "<exception> @0xEIP addr ADDR" — compact so it fits the
     * 39 visible characters of a toast card. */
    {
        char h[12];
        det[0] = '\0';
        win_append(det, sizeof(det), kguard_exc_name(rc));
        win_append(det, sizeof(det), " @");
        hex_to_str(kguard_last_eip(), h); win_append(det, sizeof(det), h);
        if (rc == 1 + 14 || rc == 1 + 8) {
            win_append(det, sizeof(det), " addr ");
            hex_to_str(kguard_last_addr(), h); win_append(det, sizeof(det), h + 2);
        }
    }
    notify_push(det);

    msg[0] = '\0';
    win_append(msg, sizeof(msg), "Crash ");
    win_append(msg, sizeof(msg), kguard_exc_short(rc));
    win_append(msg, sizeof(msg), " in ");
    win_append(msg, sizeof(msg), title);
    win_append(msg, sizeof(msg), " ");
    win_append(msg, sizeof(msg), phase);
    syslog_add(msg);
}

static void win_call_draw(int id, int cx, int cy, int cw, int ch) {
    volatile int vid = id;
    kguard_ctx_t g;
    int rc = kguard_try(&g, "app draw");
    if (rc == 0) {
        windows[vid].draw(vid, cx, cy, cw, ch);
        kguard_end();
    } else {
        win_crashed(vid, rc, "draw");
    }
}

static void win_call_key(int id, char key) {
    volatile int vid = id;
    kguard_ctx_t g;
    int rc = kguard_try(&g, "app key");
    if (rc == 0) {
        windows[vid].on_key(vid, key);
        kguard_end();
    } else {
        win_crashed(vid, rc, "key");
    }
}

static void win_call_click(int id, int px, int py) {
    volatile int vid = id;
    kguard_ctx_t g;
    int rc = kguard_try(&g, "app click");
    if (rc == 0) {
        windows[vid].on_click(vid, px, py);
        kguard_end();
    } else {
        win_crashed(vid, rc, "click");
    }
}

/* Sync pixel coords from cell coords */
static void sync_px(window_t* win) {
    win->px = win->x * 8;
    win->py = win->y * 16;
    win->pw = win->w * 8;
    win->ph = win->h * 16;
}

/* Sync cell coords from pixel coords */
static void sync_cells(window_t* win) {
    win->x = win->px / 8;
    win->y = win->py / 16;
    win->w = win->pw / 8;
    win->h = win->ph / 16;
    if (win->w < 1) win->w = 1;
    if (win->h < 1) win->h = 1;
}

/* -------------------------------------------------------------------------- */
int window_create(const char* title, int x, int y, int w, int h,
                  win_draw_fn draw_fn, win_key_fn key_fn) {
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    window_t* win = &windows[slot];
    memset(win, 0, sizeof(window_t));

    win->id = slot;
    win->active = true;
    strncpy(win->title, title, WIN_TITLE_MAX - 1);
    win->title[WIN_TITLE_MAX - 1] = '\0';
    /* Keep new windows below the menu bar and on screen. */
    if (fb_is_vesa()) {
        int top = UI_TOP_RESERVE / 16;
        if (y < top) y = top;
        if (x + w > GUI_WIDTH) x = GUI_WIDTH - w;
        if (x < 0) x = 0;
    }
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->flags = WIN_VISIBLE | WIN_MOVABLE | WIN_CLOSABLE | WIN_RESIZABLE;
    win->z_order = next_z++;
    win->draw = draw_fn;
    win->on_key = key_fn;
    win->on_click = NULL;
    win->resize_edge = RESIZE_NONE;
    win->resizing = false;
    win->anim_frame = 0;
    win->anim_type = 0;
    sync_px(win);
    window_focus(slot);
    return slot;
}

void window_destroy(int id) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].active = false;
        windows[id].flags = 0;
    }
}

void window_focus(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].flags &= ~WIN_FOCUSED;
    windows[id].flags |= WIN_FOCUSED;
    windows[id].z_order = next_z++;
}

void window_move(int id, int new_x, int new_y) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x + windows[id].w > GUI_WIDTH) new_x = GUI_WIDTH - windows[id].w;
    if (new_y + windows[id].h > GUI_HEIGHT - 1) new_y = GUI_HEIGHT - 1 - windows[id].h;
    windows[id].x = new_x;
    windows[id].y = new_y;
    sync_px(&windows[id]);
}

void window_move_px(int id, int npx, int npy) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    /* Snap to character cell grid to align with gui_putchar */
    npx = (npx / 8) * 8;
    npy = (npy / 16) * 16;
    /* Like macOS: the window may slide partly off the sides and under the
     * dock, but the titlebar never goes under the menu bar or off screen. */
    if (npx > sw - 96) npx = ((sw - 96) / 8) * 8;
    if (npx + windows[id].pw < 96) npx = ((96 - windows[id].pw) / 8) * 8;
    if (npy > sh - 48) npy = ((sh - 48) / 16) * 16;
    if (npy < UI_TOP_RESERVE) npy = UI_TOP_RESERVE;
    windows[id].px = npx;
    windows[id].py = npy;
    sync_cells(&windows[id]);
}

void window_resize_px(int id, int nw, int nh) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    if (nw < WIN_MIN_W) nw = WIN_MIN_W;
    if (nh < WIN_MIN_H) nh = WIN_MIN_H;
    windows[id].pw = nw;
    windows[id].ph = nh;
    sync_cells(&windows[id]);
}

/* --------------------------------------------------------------------------
 * macOS-style "traffic light" button layout.
 * Buttons sit on the LEFT of the titlebar, ordered close, minimize, maximize.
 * win_btn_left() returns the left-edge x of button n (0/1/2). Both the renderer
 * and the hit-testers call it, so the clickable areas can never drift from what
 * is drawn.
 * -------------------------------------------------------------------------- */
#define WIN_BTN_GAP   8     /* gap between adjacent buttons */
#define WIN_BTN_INSET 13    /* left inset of the first button */

static int hover_x = -1, hover_y = -1;
void window_set_hover(int px, int py) { hover_x = px; hover_y = py; }

static int win_btn_left(const window_t* win, int n) {
    return win->px + WIN_BTN_INSET + n * (WIN_BTN_SIZE + WIN_BTN_GAP);
}

/* --------------------------------------------------------------------------
 * window_draw: Render a single window (pixel-perfect in VESA)
 * -------------------------------------------------------------------------- */
void window_draw(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    if (!(windows[id].flags & WIN_VISIBLE)) return;
    if (windows[id].flags & WIN_MINIMIZED) return;

    window_t* win = &windows[id];
    const theme_t* t = theme_get();
    bool focused = (win->flags & WIN_FOCUSED) != 0;
    bool vesa = fb_is_vesa();

    if (vesa) {
        int px = win->px, py = win->py, pw = win->pw, ph = win->ph;
        int R = (win->flags & WIN_MAXIMIZED) ? 0 : WIN_CORNER_R;

        /* Soft macOS-style shadow: big and dark for the key window. */
        if (focused) ui_shadow(px, py, pw, ph, R, WIN_SHADOW_R, 120, 10);
        else         ui_shadow(px, py, pw, ph, R, 14, 70, 4);

        /* Body (same tone as the cell palette's background) */
        ui_rrect(px, py, pw, ph, R, vga_to_rgb(VGA_BLACK), 255);

        /* Unified titlebar */
        uint32_t tb_top = focused ? UI_RGB(60, 60, 64) : UI_RGB(44, 44, 47);
        uint32_t tb_bot = focused ? UI_RGB(52, 52, 56) : UI_RGB(40, 40, 43);
        ui_rrect_vgrad(px, py, pw, WIN_TITLEBAR_H + R, R, tb_top, tb_bot, 255);
        ui_blend_rect(px, py + WIN_TITLEBAR_H, pw, R + 4, vga_to_rgb(VGA_BLACK), 255);
        ui_blend_rect(px, py + WIN_TITLEBAR_H, pw, 1, 0x000000, 150);

        /* Hairline frame + faint top highlight */
        ui_rrect_stroke(px, py, pw, ph, R, 0x000000, 170);
        ui_blend_rect(px + R, py + 1, pw - 2 * R, 1, 0xFFFFFF, 34);

        /* Traffic lights: close, minimize, zoom. Glyphs appear on hover. */
        int r = WIN_BTN_SIZE / 2;
        int cy = py + WIN_TITLEBAR_H / 2;
        int gx0 = win_btn_left(win, 0) - 2, gx1 = win_btn_left(win, 2) + WIN_BTN_SIZE + 2;
        bool hover = hover_y >= cy - r - 2 && hover_y <= cy + r + 2 && hover_x >= gx0 && hover_x <= gx1;
        static const uint32_t lit[3] = { UI_RGB(255, 95, 87), UI_RGB(254, 188, 46), UI_RGB(40, 200, 64) };
        static const uint32_t ink[3] = { UI_RGB(110, 20, 16), UI_RGB(130, 80, 6), UI_RGB(8, 90, 20) };
        for (int n = 0; n < 3; n++) {
            if (n == 0 && !(win->flags & WIN_CLOSABLE)) continue;
            int cx = win_btn_left(win, n) + r;
            bool on = focused || hover;
            ui_circle(cx, cy, r, on ? lit[n] : UI_RGB(78, 78, 82), 255);
            ui_ring(cx, cy, r, 1, 0x000000, on ? 50 : 70);
            if (hover) {
                if (n == 0) {
                    ui_line(cx - 3, cy - 3, cx + 3, cy + 3, 1, ink[0], 230);
                    ui_line(cx + 3, cy - 3, cx - 3, cy + 3, 1, ink[0], 230);
                } else if (n == 1) {
                    ui_line(cx - 3, cy, cx + 3, cy, 1, ink[1], 230);
                } else {
                    ui_tri(cx - 3, cy - 3, cx + 2, cy - 3, cx - 3, cy + 2, ink[2], 230);
                    ui_tri(cx + 3, cy + 3, cx - 2, cy + 3, cx + 3, cy - 2, ink[2], 230);
                }
            }
        }

        /* Centered title, kept clear of the lights */
        {
            int tw = ui_text_width(win->title, UF_UIB);
            int tx = px + (pw - tw) / 2;
            int lights_right = gx1 + 10;
            if (tx < lights_right) tx = lights_right;
            int ty = py + (WIN_TITLEBAR_H - ui_font_height(UF_UIB)) / 2 + 1;
            ui_text(tx, ty, win->title, UF_UIB, focused ? UI_RGB(228, 228, 232) : UI_RGB(140, 140, 146));
        }

    } else {
        /* Legacy text-mode rendering */
        uint8_t title_color = focused ? t->win_title_active : t->win_title_inactive;
        uint8_t border_color = focused ? t->win_border : t->win_title_inactive;

        /* Shadow */
        if (focused) {
            uint8_t sh = VGA_COLOR(VGA_BLACK, VGA_BLACK);
            for (int sy = win->y + 1; sy < win->y + win->h + 1 && sy < GUI_HEIGHT - 1; sy++)
                if (win->x + win->w < GUI_WIDTH) gui_putchar(win->x + win->w, sy, ' ', sh);
            for (int sx = win->x + 1; sx <= win->x + win->w && sx < GUI_WIDTH; sx++)
                if (win->y + win->h < GUI_HEIGHT - 1) gui_putchar(sx, win->y + win->h, ' ', sh);
        }

        /* Frame */
        gui_rect_t border = { win->x, win->y, win->w, win->h };
        if (focused) gui_draw_box_double(border, border_color);
        else gui_draw_box(border, border_color);

        /* Title bar fill */
        for (int bx = win->x + 1; bx < win->x + win->w - 1; bx++)
            gui_putchar(bx, win->y, ' ', title_color);

        /* Title text */
        uint8_t tbg = (title_color >> 4) & 0x0F;
        gui_putchar(win->x + 1, win->y, focused ? '\x0F' : '\xFA',
            VGA_COLOR(focused ? VGA_YELLOW : VGA_DARK_GREY, tbg));

        int title_len = strlen(win->title);
        int title_space = win->w - 10;
        if (title_len > title_space) title_len = title_space;
        for (int i = 0; i < title_len; i++)
            gui_putchar(win->x + 3 + i, win->y, win->title[i], title_color);

        /* Buttons */
        int bx = win->x + win->w - 2;
        if (win->flags & WIN_CLOSABLE)
            gui_putchar(bx, win->y, '\xFE', VGA_COLOR(VGA_LIGHT_RED, tbg));
        gui_putchar(bx - 2, win->y,
            (win->flags & WIN_MAXIMIZED) ? '\x12' : '\x18',
            VGA_COLOR(VGA_LIGHT_GREEN, tbg));
        gui_putchar(bx - 4, win->y, '\x19', VGA_COLOR(VGA_YELLOW, tbg));

        /* Content fill */
        gui_rect_t content = { win->x + 1, win->y + 1, win->w - 2, win->h - 2 };
        gui_fill_rect(content, ' ', t->win_content);
    }

    /* Content callback — always pass character-cell coordinates
     * because all existing apps use gui_putchar/gui_draw_text (cell-based) */
    if (win->draw) {
        int content_y = win->y + 1;
        int content_h = win->h - 2;
        if (vesa) {
            content_y = win->y + 2;
            content_h = win->h - 3;
        }
        if (content_h > 0)
            win_call_draw(id, win->x + 1, content_y, win->w - 2, content_h);   /* Phase 55: guarded */
    }
}

/* -------------------------------------------------------------------------- */
static int get_z_sorted(int sorted[MAX_WINDOWS]) {
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].active) sorted[count++] = i;
    for (int i = 1; i < count; i++) {
        int key = sorted[i], j = i - 1;
        while (j >= 0 && windows[sorted[j]].z_order > windows[key].z_order) {
            sorted[j + 1] = sorted[j]; j--;
        }
        sorted[j + 1] = key;
    }
    return count;
}

/* Content area of a window in PIXELS (below the 28px titlebar), for apps that
 * draw pixel-perfect Mac UI instead of the 8x16 cell grid. Returns false for
 * an invalid window or text mode. */
bool window_content_px(int id, int* x, int* y, int* w, int* h) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active || !fb_is_vesa()) return false;
    window_t* win = &windows[id];
    int top = win->py + WIN_TITLEBAR_H + 1;
    if (x) *x = win->px + 1;
    if (y) *y = top;
    if (w) *w = win->pw - 2;
    if (h) *h = win->py + win->ph - top - 1;
    return true;
}

void window_draw_all(void) {
    int sorted[MAX_WINDOWS];
    int count = get_z_sorted(sorted);
    for (int i = 0; i < count; i++) window_draw(sorted[i]);
}

/* --------------------------------------------------------------------------
 * Hit testing (character-cell, backward compat)
 * -------------------------------------------------------------------------- */
int window_hit_test(int mx, int my) {
    int sorted[MAX_WINDOWS];
    int count = get_z_sorted(sorted);
    for (int i = count - 1; i >= 0; i--) {
        window_t* win = &windows[sorted[i]];
        if ((win->flags & WIN_VISIBLE) && !(win->flags & WIN_MINIMIZED)) {
            if (mx >= win->x && mx < win->x + win->w &&
                my >= win->y && my < win->y + win->h)
                return sorted[i];
        }
    }
    return -1;
}

bool window_titlebar_hit(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    return (my == win->y && mx > win->x && mx < win->x + win->w - 1);
}

bool window_close_hit(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    if (!(windows[id].flags & WIN_CLOSABLE)) return false;
    window_t* win = &windows[id];
    return (my == win->y && mx == win->x + win->w - 2);
}

/* --------------------------------------------------------------------------
 * Pixel-based hit testing (Phase 17)
 * -------------------------------------------------------------------------- */
int window_hit_test_px(int mx, int my) {
    int sorted[MAX_WINDOWS];
    int count = get_z_sorted(sorted);
    for (int i = count - 1; i >= 0; i--) {
        window_t* win = &windows[sorted[i]];
        if ((win->flags & WIN_VISIBLE) && !(win->flags & WIN_MINIMIZED)) {
            if (mx >= win->px && mx < win->px + win->pw &&
                my >= win->py && my < win->py + win->ph)
                return sorted[i];
        }
    }
    return -1;
}

bool window_titlebar_hit_px(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    return (mx >= win->px && mx < win->px + win->pw &&
            my >= win->py && my < win->py + WIN_TITLEBAR_H);
}

bool window_close_hit_px(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    if (!(windows[id].flags & WIN_CLOSABLE)) return false;
    window_t* win = &windows[id];
    int bx = win_btn_left(win, 0);
    int by = win->py + (WIN_TITLEBAR_H - WIN_BTN_SIZE) / 2;
    int cx = bx + WIN_BTN_SIZE/2, cy = by + WIN_BTN_SIZE/2;
    int dx = mx - cx, dy = my - cy;
    return (dx*dx + dy*dy <= (WIN_BTN_SIZE/2 + 2) * (WIN_BTN_SIZE/2 + 2));
}

bool window_minimize_hit_px(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    int bx = win_btn_left(win, 1);
    int by = win->py + (WIN_TITLEBAR_H - WIN_BTN_SIZE) / 2;
    int cx = bx + WIN_BTN_SIZE/2, cy = by + WIN_BTN_SIZE/2;
    int dx = mx - cx, dy = my - cy;
    return (dx*dx + dy*dy <= (WIN_BTN_SIZE/2 + 2) * (WIN_BTN_SIZE/2 + 2));
}

bool window_maximize_hit_px(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    int bx = win_btn_left(win, 2);
    int by = win->py + (WIN_TITLEBAR_H - WIN_BTN_SIZE) / 2;
    int cx = bx + WIN_BTN_SIZE/2, cy = by + WIN_BTN_SIZE/2;
    int dx = mx - cx, dy = my - cy;
    return (dx*dx + dy*dy <= (WIN_BTN_SIZE/2 + 2) * (WIN_BTN_SIZE/2 + 2));
}

resize_edge_t window_resize_hit(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return RESIZE_NONE;
    if (!(windows[id].flags & WIN_RESIZABLE)) return RESIZE_NONE;
    window_t* win = &windows[id];
    int g = WIN_RESIZE_GRAB;
    bool top    = (my >= win->py && my < win->py + g);
    bool bottom = (my > win->py + win->ph - g && my <= win->py + win->ph);
    bool left   = (mx >= win->px && mx < win->px + g);
    bool right  = (mx > win->px + win->pw - g && mx <= win->px + win->pw);

    if (top && left) return RESIZE_NW;
    if (top && right) return RESIZE_NE;
    if (bottom && left) return RESIZE_SW;
    if (bottom && right) return RESIZE_SE;
    if (top) return RESIZE_N;
    if (bottom) return RESIZE_S;
    if (left) return RESIZE_W;
    if (right) return RESIZE_E;
    return RESIZE_NONE;
}

/* -------------------------------------------------------------------------- */
window_t* window_get(int id) {
    if (id >= 0 && id < MAX_WINDOWS && windows[id].active) return &windows[id];
    return NULL;
}

int window_get_focused(void) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].active && (windows[i].flags & WIN_FOCUSED)) return i;
    return -1;
}

int window_count(void) {
    int c = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].active) c++;
    return c;
}

void window_send_key(char key) {
    int focused = window_get_focused();
    if (focused >= 0 && windows[focused].on_key)
        win_call_key(focused, key);                 /* Phase 55: guarded */
}

void window_set_on_click(int id, win_click_fn fn) {
    if (id >= 0 && id < MAX_WINDOWS && windows[id].active) windows[id].on_click = fn;
}

void window_send_click(int id, int px, int py) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active || !windows[id].on_click) return;
    /* Pass coords relative to the content area (below the titlebar). */
    win_call_click(id, px - windows[id].px, py - (windows[id].py + WIN_TITLEBAR_H));   /* Phase 55: guarded */
}

void window_minimize(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    windows[id].flags |= WIN_MINIMIZED;
    windows[id].flags &= ~WIN_FOCUSED;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && !(windows[i].flags & WIN_MINIMIZED)) {
            window_focus(i); return;
        }
    }
}

void window_maximize(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    window_t* win = &windows[id];
    if (win->flags & WIN_MAXIMIZED) {
        window_restore(id);
    } else {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
        if (fb_is_vesa()) {
            win->px = 0; win->py = UI_TOP_RESERVE;
            win->pw = (int)fb_get_width();
            win->ph = (int)fb_get_height() - UI_TOP_RESERVE - WIN_DOCK_RESERVE;
            sync_cells(win);
        } else {
            win->x = 0; win->y = 0;
            win->w = GUI_WIDTH; win->h = GUI_HEIGHT - 1;
            sync_px(win);
        }
        win->flags |= WIN_MAXIMIZED;
    }
}

void window_restore(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    window_t* win = &windows[id];
    if (win->flags & WIN_MAXIMIZED) {
        win->x = win->saved_x; win->y = win->saved_y;
        win->w = win->saved_w; win->h = win->saved_h;
        sync_px(win);
        win->flags &= ~WIN_MAXIMIZED;
    }
    win->flags &= ~WIN_MINIMIZED;
    window_focus(id);
}

bool window_minimize_hit(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    return (my == win->y && (mx == win->x + win->w - 6 || mx == win->x + win->w - 5));
}

bool window_maximize_hit(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    return (my == win->y && (mx == win->x + win->w - 4 || mx == win->x + win->w - 3));
}

void window_snap_left(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    window_t* win = &windows[id];
    if (!(win->flags & WIN_MAXIMIZED)) {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    if (fb_is_vesa()) {
        win->px = 0; win->py = UI_TOP_RESERVE;
        win->pw = (int)fb_get_width() / 2;
        win->ph = (int)fb_get_height() - UI_TOP_RESERVE - WIN_DOCK_RESERVE;
        sync_cells(win);
    } else {
        win->x = 0; win->y = 0;
        win->w = GUI_WIDTH / 2; win->h = GUI_HEIGHT - 1;
        sync_px(win);
    }
    win->flags &= ~WIN_MAXIMIZED;
}

void window_snap_right(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return;
    window_t* win = &windows[id];
    if (!(win->flags & WIN_MAXIMIZED)) {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    if (fb_is_vesa()) {
        int half = (int)fb_get_width() / 2;
        win->px = half; win->py = UI_TOP_RESERVE;
        win->pw = half;
        win->ph = (int)fb_get_height() - UI_TOP_RESERVE - WIN_DOCK_RESERVE;
        sync_cells(win);
    } else {
        win->x = GUI_WIDTH / 2; win->y = 0;
        win->w = GUI_WIDTH / 2; win->h = GUI_HEIGHT - 1;
        sync_px(win);
    }
    win->flags &= ~WIN_MAXIMIZED;
}
