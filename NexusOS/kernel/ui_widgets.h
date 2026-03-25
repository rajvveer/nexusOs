/* ============================================================================
 * NexusOS — UI Widget Toolkit (Header) — Phase 16
 * ============================================================================
 * Pixel-mode widgets: button, checkbox, slider, scrollbar, text input.
 * All widgets render to the framebuffer back buffer.
 * ============================================================================ */

#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "types.h"

/* Widget colors — can be overridden per-widget */
#define UI_COLOR_BG        0x2D2D30   /* Dark background */
#define UI_COLOR_FG        0xF0F0F0   /* Light foreground text */
#define UI_COLOR_ACCENT    0x007ACC   /* Blue accent */
#define UI_COLOR_HOVER     0x3E3E42   /* Hover highlight */
#define UI_COLOR_PRESSED   0x094771   /* Pressed state */
#define UI_COLOR_BORDER    0x555555   /* Border */
#define UI_COLOR_CHECK     0x73C991   /* Checkmark green */
#define UI_COLOR_TRACK     0x3E3E42   /* Slider/scrollbar track */
#define UI_COLOR_THUMB     0x007ACC   /* Slider/scrollbar thumb */
#define UI_COLOR_INPUT_BG  0x1E1E1E   /* Text input background */
#define UI_COLOR_CURSOR    0xFFFFFF   /* Text cursor */

/* --------------------------------------------------------------------------
 * Button
 * -------------------------------------------------------------------------- */
typedef struct {
    int x, y, w, h;
    const char* label;
    bool pressed;
    bool hovered;
    uint32_t bg_color;
    uint32_t fg_color;
    int corner_radius;
} ui_button_t;

void ui_button_init(ui_button_t* btn, int x, int y, int w, int h, const char* label);
void ui_button_draw(ui_button_t* btn);
bool ui_button_hit(ui_button_t* btn, int mx, int my);

/* --------------------------------------------------------------------------
 * Checkbox
 * -------------------------------------------------------------------------- */
typedef struct {
    int x, y;
    const char* label;
    bool checked;
    bool hovered;
    uint32_t fg_color;
} ui_checkbox_t;

void ui_checkbox_init(ui_checkbox_t* cb, int x, int y, const char* label, bool checked);
void ui_checkbox_draw(ui_checkbox_t* cb);
bool ui_checkbox_hit(ui_checkbox_t* cb, int mx, int my);
void ui_checkbox_toggle(ui_checkbox_t* cb);

/* --------------------------------------------------------------------------
 * Slider
 * -------------------------------------------------------------------------- */
typedef struct {
    int x, y, w;
    int min_val, max_val, value;
    bool dragging;
    uint32_t track_color;
    uint32_t thumb_color;
} ui_slider_t;

void ui_slider_init(ui_slider_t* s, int x, int y, int w, int min_v, int max_v, int val);
void ui_slider_draw(ui_slider_t* s);
bool ui_slider_hit(ui_slider_t* s, int mx, int my);
void ui_slider_update(ui_slider_t* s, int mx);

/* --------------------------------------------------------------------------
 * Scrollbar (vertical)
 * -------------------------------------------------------------------------- */
typedef struct {
    int x, y, h;
    int position;    /* Current scroll position (0-based) */
    int total;       /* Total items */
    int visible;     /* Visible items */
    bool dragging;
    uint32_t track_color;
    uint32_t thumb_color;
} ui_scrollbar_t;

void ui_scrollbar_init(ui_scrollbar_t* sb, int x, int y, int h, int total, int visible);
void ui_scrollbar_draw(ui_scrollbar_t* sb);
bool ui_scrollbar_hit(ui_scrollbar_t* sb, int mx, int my);
void ui_scrollbar_update(ui_scrollbar_t* sb, int my);

/* --------------------------------------------------------------------------
 * Text Input
 * -------------------------------------------------------------------------- */
#define UI_INPUT_MAX_LEN 128

typedef struct {
    int x, y, w;
    char text[UI_INPUT_MAX_LEN];
    int cursor;
    int length;
    bool focused;
    const char* placeholder;
    uint32_t bg_color;
    uint32_t fg_color;
    uint32_t border_color;
} ui_textinput_t;

void ui_textinput_init(ui_textinput_t* ti, int x, int y, int w, const char* placeholder);
void ui_textinput_draw(ui_textinput_t* ti);
bool ui_textinput_hit(ui_textinput_t* ti, int mx, int my);
void ui_textinput_key(ui_textinput_t* ti, char key);
const char* ui_textinput_get_text(ui_textinput_t* ti);

#endif /* UI_WIDGETS_H */
