/* ============================================================================
 * NexusOS — UI Widget Toolkit (Implementation) — Phase 16
 * ============================================================================
 * Pixel-mode widgets rendered on the framebuffer.
 * ============================================================================ */

#include "ui_widgets.h"
#include "gfx.h"
#include "framebuffer.h"
#include "font.h"
#include "string.h"

/* ==========================================================================
 * Button
 * ========================================================================== */

void ui_button_init(ui_button_t* btn, int x, int y, int w, int h, const char* label) {
    btn->x = x; btn->y = y; btn->w = w; btn->h = h;
    btn->label = label;
    btn->pressed = false;
    btn->hovered = false;
    btn->bg_color = UI_COLOR_ACCENT;
    btn->fg_color = UI_COLOR_FG;
    btn->corner_radius = 4;
}

void ui_button_draw(ui_button_t* btn) {
    uint32_t bg = btn->bg_color;
    if (btn->pressed) bg = UI_COLOR_PRESSED;
    else if (btn->hovered) bg = UI_COLOR_HOVER;

    /* Shadow */
    if (!btn->pressed)
        gfx_fill_rect_alpha(btn->x + 2, btn->y + 2, btn->w, btn->h, 0x000000, 60);

    /* Button body */
    gfx_fill_rounded_rect(btn->x, btn->y, btn->w, btn->h, btn->corner_radius, bg);

    /* Border */
    gfx_draw_rounded_rect(btn->x, btn->y, btn->w, btn->h, btn->corner_radius, UI_COLOR_BORDER);

    /* Label (centered) */
    if (btn->label) {
        int tw = font_measure_string(btn->label);
        int tx = btn->x + (btn->w - tw) / 2;
        int ty = btn->y + (btn->h - font_get_active_height()) / 2;
        if (btn->pressed) { tx++; ty++; }
        font_draw_string(tx, ty, btn->label, btn->fg_color, bg);
    }
}

bool ui_button_hit(ui_button_t* btn, int mx, int my) {
    return mx >= btn->x && mx < btn->x + btn->w &&
           my >= btn->y && my < btn->y + btn->h;
}

/* ==========================================================================
 * Checkbox
 * ========================================================================== */

void ui_checkbox_init(ui_checkbox_t* cb, int x, int y, const char* label, bool checked) {
    cb->x = x; cb->y = y;
    cb->label = label;
    cb->checked = checked;
    cb->hovered = false;
    cb->fg_color = UI_COLOR_FG;
}

void ui_checkbox_draw(ui_checkbox_t* cb) {
    int box_size = 14;
    uint32_t box_bg = cb->checked ? UI_COLOR_ACCENT : UI_COLOR_INPUT_BG;
    if (cb->hovered && !cb->checked) box_bg = UI_COLOR_HOVER;

    /* Box */
    gfx_fill_rect(cb->x, cb->y, box_size, box_size, box_bg);
    gfx_draw_rect(cb->x, cb->y, box_size, box_size, UI_COLOR_BORDER);

    /* Check mark */
    if (cb->checked) {
        uint32_t ck = UI_COLOR_FG;
        int bx = cb->x + 3, by = cb->y + 3;
        /* Simple check mark shape */
        gfx_draw_line(bx, by + 4, bx + 2, by + 7, ck);
        gfx_draw_line(bx + 1, by + 4, bx + 3, by + 7, ck);
        gfx_draw_line(bx + 3, by + 7, bx + 8, by, ck);
        gfx_draw_line(bx + 2, by + 7, bx + 7, by, ck);
    }

    /* Label */
    if (cb->label) {
        int lx = cb->x + box_size + 6;
        int ly = cb->y + (box_size - font_get_active_height()) / 2;
        font_draw_string(lx, ly, cb->label, cb->fg_color, 0x000000);
    }
}

bool ui_checkbox_hit(ui_checkbox_t* cb, int mx, int my) {
    int tw = cb->label ? font_measure_string(cb->label) + 20 : 14;
    return mx >= cb->x && mx < cb->x + tw &&
           my >= cb->y && my < cb->y + 14;
}

void ui_checkbox_toggle(ui_checkbox_t* cb) {
    cb->checked = !cb->checked;
}

/* ==========================================================================
 * Slider
 * ========================================================================== */

void ui_slider_init(ui_slider_t* s, int x, int y, int w, int min_v, int max_v, int val) {
    s->x = x; s->y = y; s->w = w;
    s->min_val = min_v; s->max_val = max_v; s->value = val;
    s->dragging = false;
    s->track_color = UI_COLOR_TRACK;
    s->thumb_color = UI_COLOR_THUMB;
}

void ui_slider_draw(ui_slider_t* s) {
    int track_h = 4;
    int track_y = s->y + 6;
    int range = s->max_val - s->min_val;
    int thumb_x = s->x;
    if (range > 0)
        thumb_x = s->x + ((s->value - s->min_val) * (s->w - 12)) / range;

    /* Track background */
    gfx_fill_rounded_rect(s->x, track_y, s->w, track_h, 2, s->track_color);

    /* Filled portion */
    if (thumb_x > s->x)
        gfx_fill_rounded_rect(s->x, track_y, thumb_x - s->x + 6, track_h, 2, s->thumb_color);

    /* Thumb (circle) */
    gfx_fill_circle(thumb_x + 6, s->y + 8, 6, s->thumb_color);
    gfx_draw_circle(thumb_x + 6, s->y + 8, 6, UI_COLOR_BORDER);
}

bool ui_slider_hit(ui_slider_t* s, int mx, int my) {
    return mx >= s->x - 6 && mx < s->x + s->w + 6 &&
           my >= s->y && my < s->y + 16;
}

void ui_slider_update(ui_slider_t* s, int mx) {
    int range = s->max_val - s->min_val;
    if (range <= 0 || s->w <= 12) return;
    int rel = mx - s->x;
    if (rel < 0) rel = 0;
    if (rel > s->w - 12) rel = s->w - 12;
    s->value = s->min_val + (rel * range) / (s->w - 12);
}

/* ==========================================================================
 * Scrollbar
 * ========================================================================== */

void ui_scrollbar_init(ui_scrollbar_t* sb, int x, int y, int h, int total, int visible) {
    sb->x = x; sb->y = y; sb->h = h;
    sb->position = 0;
    sb->total = total;
    sb->visible = visible;
    sb->dragging = false;
    sb->track_color = UI_COLOR_TRACK;
    sb->thumb_color = UI_COLOR_THUMB;
}

void ui_scrollbar_draw(ui_scrollbar_t* sb) {
    int track_w = 12;

    /* Track */
    gfx_fill_rect(sb->x, sb->y, track_w, sb->h, sb->track_color);
    gfx_draw_rect(sb->x, sb->y, track_w, sb->h, UI_COLOR_BORDER);

    /* Thumb */
    if (sb->total <= 0 || sb->visible >= sb->total) {
        /* No scrolling needed — fill entire track */
        gfx_fill_rect(sb->x + 1, sb->y + 1, track_w - 2, sb->h - 2, sb->thumb_color);
    } else {
        int thumb_h = (sb->visible * sb->h) / sb->total;
        if (thumb_h < 16) thumb_h = 16;
        int max_pos = sb->total - sb->visible;
        int thumb_y = sb->y;
        if (max_pos > 0)
            thumb_y = sb->y + (sb->position * (sb->h - thumb_h)) / max_pos;
        gfx_fill_rounded_rect(sb->x + 1, thumb_y, track_w - 2, thumb_h, 3, sb->thumb_color);
    }
}

bool ui_scrollbar_hit(ui_scrollbar_t* sb, int mx, int my) {
    return mx >= sb->x && mx < sb->x + 12 &&
           my >= sb->y && my < sb->y + sb->h;
}

void ui_scrollbar_update(ui_scrollbar_t* sb, int my) {
    if (sb->total <= sb->visible) { sb->position = 0; return; }
    int max_pos = sb->total - sb->visible;
    int thumb_h = (sb->visible * sb->h) / sb->total;
    if (thumb_h < 16) thumb_h = 16;
    int track_range = sb->h - thumb_h;
    if (track_range <= 0) { sb->position = 0; return; }
    int rel = my - sb->y - thumb_h / 2;
    if (rel < 0) rel = 0;
    if (rel > track_range) rel = track_range;
    sb->position = (rel * max_pos) / track_range;
}

/* ==========================================================================
 * Text Input
 * ========================================================================== */

void ui_textinput_init(ui_textinput_t* ti, int x, int y, int w, const char* placeholder) {
    ti->x = x; ti->y = y; ti->w = w;
    ti->text[0] = '\0';
    ti->cursor = 0;
    ti->length = 0;
    ti->focused = false;
    ti->placeholder = placeholder;
    ti->bg_color = UI_COLOR_INPUT_BG;
    ti->fg_color = UI_COLOR_FG;
    ti->border_color = UI_COLOR_BORDER;
}

void ui_textinput_draw(ui_textinput_t* ti) {
    int h = font_get_active_height() + 8;
    uint32_t border = ti->focused ? UI_COLOR_ACCENT : ti->border_color;

    /* Background */
    gfx_fill_rect(ti->x, ti->y, ti->w, h, ti->bg_color);
    gfx_draw_rect(ti->x, ti->y, ti->w, h, border);

    /* Text or placeholder */
    int tx = ti->x + 4;
    int ty = ti->y + 4;
    if (ti->length > 0) {
        font_draw_string(tx, ty, ti->text, ti->fg_color, ti->bg_color);
    } else if (ti->placeholder) {
        font_draw_string(tx, ty, ti->placeholder, UI_COLOR_BORDER, ti->bg_color);
    }

    /* Cursor */
    if (ti->focused) {
        int cx = tx + ti->cursor * FONT_WIDTH;
        gfx_draw_vline(cx, ti->y + 2, h - 4, UI_COLOR_CURSOR);
    }
}

bool ui_textinput_hit(ui_textinput_t* ti, int mx, int my) {
    int h = font_get_active_height() + 8;
    return mx >= ti->x && mx < ti->x + ti->w &&
           my >= ti->y && my < ti->y + h;
}

void ui_textinput_key(ui_textinput_t* ti, char key) {
    if (!ti->focused) return;

    if (key == '\b') {
        /* Backspace */
        if (ti->cursor > 0) {
            for (int i = ti->cursor - 1; i < ti->length; i++)
                ti->text[i] = ti->text[i + 1];
            ti->cursor--;
            ti->length--;
        }
    } else if (key >= 32 && key < 127) {
        /* Printable character */
        if (ti->length < UI_INPUT_MAX_LEN - 1) {
            for (int i = ti->length; i >= ti->cursor; i--)
                ti->text[i + 1] = ti->text[i];
            ti->text[ti->cursor] = key;
            ti->cursor++;
            ti->length++;
        }
    }
}

const char* ui_textinput_get_text(ui_textinput_t* ti) {
    return ti->text;
}
