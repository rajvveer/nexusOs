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

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* Window table */
static window_t windows[MAX_WINDOWS];
static int next_z = 1;

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
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->flags = WIN_VISIBLE | WIN_MOVABLE | WIN_CLOSABLE | WIN_RESIZABLE;
    win->z_order = next_z++;
    win->draw = draw_fn;
    win->on_key = key_fn;
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
    if (npx < 0) npx = 0;
    if (npy < 0) npy = 0;
    /* Snap to character cell grid to align with gui_putchar */
    npx = (npx / 8) * 8;
    npy = (npy / 16) * 16;
    if (npx + windows[id].pw > sw) npx = sw - windows[id].pw;
    if (npy + windows[id].ph > sh) npy = sh - windows[id].ph;
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

        /* Drop shadow for focused windows */
        if (focused) {
            gfx_draw_shadow(px, py, pw, ph, WIN_SHADOW_R, 0x000000);
        }

        /* Window body with rounded corners */
        gfx_fill_rounded_rect(px, py, pw, ph, WIN_CORNER_R, FB_RGB(245, 245, 245));

        /* Titlebar with gradient */
        uint32_t tb_left, tb_right;
        if (focused) {
            tb_left = FB_RGB(30, 120, 220);
            tb_right = FB_RGB(80, 160, 240);
        } else {
            tb_left = FB_RGB(140, 140, 140);
            tb_right = FB_RGB(170, 170, 170);
        }

        /* Clip titlebar to rounded top corners */
        gfx_fill_rounded_rect(px, py, pw, WIN_TITLEBAR_H, WIN_CORNER_R, tb_left);
        /* Bottom half of titlebar is square */
        gfx_fill_rect(px + 1, py + WIN_CORNER_R, pw - 2, WIN_TITLEBAR_H - WIN_CORNER_R, tb_left);
        /* Gradient overlay */
        gfx_draw_gradient_h(px + 1, py + 1, pw - 2, WIN_TITLEBAR_H - 1, tb_left, tb_right);

        /* Titlebar separator line */
        gfx_draw_hline(px, py + WIN_TITLEBAR_H, pw, focused ? FB_RGB(20, 90, 180) : FB_RGB(120, 120, 120));

        /* Border outline */
        gfx_draw_rounded_rect(px, py, pw, ph, WIN_CORNER_R,
            focused ? FB_RGB(50, 130, 230) : FB_RGB(160, 160, 160));

        /* Title bar buttons — close (red), maximize (green), minimize (yellow) */
        int btn_y = py + (WIN_TITLEBAR_H - WIN_BTN_SIZE) / 2;

        /* Close button */
        if (win->flags & WIN_CLOSABLE) {
            int bx = px + pw - WIN_BTN_SIZE - 8;
            gfx_fill_circle(bx + WIN_BTN_SIZE/2, btn_y + WIN_BTN_SIZE/2, WIN_BTN_SIZE/2, FB_RGB(255, 95, 87));
            gfx_draw_circle(bx + WIN_BTN_SIZE/2, btn_y + WIN_BTN_SIZE/2, WIN_BTN_SIZE/2, FB_RGB(220, 60, 50));
            /* X mark */
            gfx_draw_line(bx + 3, btn_y + 3, bx + WIN_BTN_SIZE - 3, btn_y + WIN_BTN_SIZE - 3, FB_RGB(130, 30, 20));
            gfx_draw_line(bx + WIN_BTN_SIZE - 3, btn_y + 3, bx + 3, btn_y + WIN_BTN_SIZE - 3, FB_RGB(130, 30, 20));
        }

        /* Maximize button */
        {
            int bx = px + pw - 2 * (WIN_BTN_SIZE + 6) - 4;
            gfx_fill_circle(bx + WIN_BTN_SIZE/2, btn_y + WIN_BTN_SIZE/2, WIN_BTN_SIZE/2, FB_RGB(39, 201, 63));
            gfx_draw_circle(bx + WIN_BTN_SIZE/2, btn_y + WIN_BTN_SIZE/2, WIN_BTN_SIZE/2, FB_RGB(20, 160, 40));
        }

        /* Minimize button */
        {
            int bx = px + pw - 3 * (WIN_BTN_SIZE + 6) - 2;
            gfx_fill_circle(bx + WIN_BTN_SIZE/2, btn_y + WIN_BTN_SIZE/2, WIN_BTN_SIZE/2, FB_RGB(255, 189, 46));
            gfx_draw_circle(bx + WIN_BTN_SIZE/2, btn_y + WIN_BTN_SIZE/2, WIN_BTN_SIZE/2, FB_RGB(210, 150, 20));
            /* Dash mark */
            gfx_draw_hline(bx + 3, btn_y + WIN_BTN_SIZE/2, WIN_BTN_SIZE - 6, FB_RGB(150, 100, 10));
        }

        /* Title text */
        {
            char tbuf[WIN_TITLE_MAX];
            strncpy(tbuf, win->title, WIN_TITLE_MAX - 1);
            tbuf[WIN_TITLE_MAX - 1] = 0;
            int ty = py + (WIN_TITLEBAR_H - font_get_active_height()) / 2;
            font_draw_string(px + 10, ty, tbuf, FB_RGB(255, 255, 255), tb_left);
        }

        /* Content area background (mapped from theme color) */
        uint8_t tb_bg = (t->win_content >> 4) & 0x0F;
        uint32_t bg_pixel = vga_attr_bg(t->win_content);
        gfx_fill_rect(px + 1, py + WIN_TITLEBAR_H + 1, pw - 2, ph - WIN_TITLEBAR_H - 2, bg_pixel);

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
        win->draw(id, win->x + 1, win->y + 1, win->w - 2, win->h - 2);
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
    int bx = win->px + win->pw - WIN_BTN_SIZE - 8;
    int by = win->py + (WIN_TITLEBAR_H - WIN_BTN_SIZE) / 2;
    int cx = bx + WIN_BTN_SIZE/2, cy = by + WIN_BTN_SIZE/2;
    int dx = mx - cx, dy = my - cy;
    return (dx*dx + dy*dy <= (WIN_BTN_SIZE/2 + 2) * (WIN_BTN_SIZE/2 + 2));
}

bool window_minimize_hit_px(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    int bx = win->px + win->pw - 3 * (WIN_BTN_SIZE + 6) - 2;
    int by = win->py + (WIN_TITLEBAR_H - WIN_BTN_SIZE) / 2;
    int cx = bx + WIN_BTN_SIZE/2, cy = by + WIN_BTN_SIZE/2;
    int dx = mx - cx, dy = my - cy;
    return (dx*dx + dy*dy <= (WIN_BTN_SIZE/2 + 2) * (WIN_BTN_SIZE/2 + 2));
}

bool window_maximize_hit_px(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].active) return false;
    window_t* win = &windows[id];
    int bx = win->px + win->pw - 2 * (WIN_BTN_SIZE + 6) - 4;
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
        windows[focused].on_key(focused, key);
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
            win->px = 0; win->py = 0;
            win->pw = (int)fb_get_width();
            win->ph = (int)fb_get_height() - 32;
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
        win->px = 0; win->py = 0;
        win->pw = (int)fb_get_width() / 2;
        win->ph = (int)fb_get_height() - 32;
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
        win->px = half; win->py = 0;
        win->pw = half;
        win->ph = (int)fb_get_height() - 32;
        sync_cells(win);
    } else {
        win->x = GUI_WIDTH / 2; win->y = 0;
        win->w = GUI_WIDTH / 2; win->h = GUI_HEIGHT - 1;
        sync_px(win);
    }
    win->flags &= ~WIN_MAXIMIZED;
}
