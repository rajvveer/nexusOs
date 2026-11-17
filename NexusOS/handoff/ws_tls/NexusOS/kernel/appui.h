/* ============================================================================
 * NexusOS — App UI kit (Aurora)
 * ============================================================================
 * Two layers:
 *   (1) the original cell-coordinate API (appui_fill/text/header/kv/input/...)
 *       — unchanged signatures, now rendered as anti-aliased macOS-style pixels
 *       so every existing app is upgraded without touching its logic.
 *   (2) a pixel API (appui_px_*) for apps rewritten with bespoke Mac layouts.
 * ============================================================================ */
#ifndef APPUI_H
#define APPUI_H
#include "types.h"

typedef struct {
    uint8_t bg;
    uint8_t panel;
    uint8_t toolbar;
    uint8_t text;
    uint8_t muted;
    uint8_t accent;
    uint8_t good;
    uint8_t warn;
    uint8_t danger;
    uint8_t selected;
    uint8_t status;
} appui_theme_t;

appui_theme_t appui_theme(void);

/* --- Cell-coordinate API (x,y,w,h in 8x16 character cells) --- */
void appui_fill(int x, int y, int w, int h, uint8_t color);
void appui_text(int x, int y, const char* text, int max, uint8_t color);
void appui_header(int x, int y, int w, const char* title, const char* subtitle);
void appui_status(int x, int y, int w, const char* text);
void appui_hline(int x, int y, int w, uint8_t color);
void appui_row(int x, int y, int w, bool selected, uint8_t normal, uint8_t selected_color);
void appui_kv(int x, int y, const char* key, const char* value, int value_x, int max);
void appui_input(int x, int y, int w, const char* text, int len, bool cursor);

/* --- Pixel API (x,y,w,h in pixels) --- */

/* macOS dark system colours */
#define APPUI_BG       0x1E1E20   /* window content background */
#define APPUI_PANEL    0x2A2A2E   /* raised panel / card */
#define APPUI_SIDEBAR  0x252528   /* sidebar fill */
#define APPUI_TEXT     0xF5F5F7   /* primary label */
#define APPUI_SUBTLE   0x9A9AA0   /* secondary label */
#define APPUI_FAINT    0x6E6E76   /* tertiary / disabled */
#define APPUI_SEP      0x3A3A3E   /* hairline separator */
#define APPUI_ACCENT   0x0A84FF   /* system blue */
#define APPUI_GREEN    0x32D74B
#define APPUI_RED      0xFF453A
#define APPUI_ORANGE   0xFF9F0A
#define APPUI_FIELD    0x3A3A40   /* text field fill */

/* Fonts (mirror macui UF_*) */
#define APPUI_F_BODY   0    /* UF_UI   */
#define APPUI_F_BOLD   1    /* UF_UIB  */
#define APPUI_F_SMALL  2    /* UF_SMALL */
#define APPUI_F_TITLE  3    /* UF_TITLE */

/* Content pixel rect of a window (below its titlebar). false in text mode. */
bool appui_content(int win_id, int* x, int* y, int* w, int* h);

void appui_px_fill(int x, int y, int w, int h, uint32_t color);
void appui_px_card(int x, int y, int w, int h, int r, uint32_t color);   /* rounded fill */
void appui_px_sep(int x, int y, int w);                                   /* 1px hairline */
void appui_px_vsep(int x, int y, int h);
int  appui_px_text(int x, int y, const char* s, int font, uint32_t color);     /* left */
void appui_px_text_c(int cx, int y, const char* s, int font, uint32_t color);  /* centred */
void appui_px_text_r(int rx, int y, const char* s, int font, uint32_t color);  /* right-aligned to rx */
int  appui_px_text_w(const char* s, int font);
int  appui_px_font_h(int font);
/* a selectable list row; returns true if the point is inside (for hit-testing) */
void appui_px_row(int x, int y, int w, int h, bool selected);
/* rounded text field with caret; pass caret<0 for none */
void appui_px_field(int x, int y, int w, int h, const char* text, bool caret);
/* pill button; primary = filled blue, else grey */
void appui_px_button(int x, int y, int w, int h, const char* label, bool primary);
/* iOS-style toggle */
void appui_px_toggle(int x, int y, bool on);
/* segmented control; returns nothing, draws `n` segments, `sel` highlighted */
void appui_px_segment(int x, int y, int w, int h, const char* const* labels, int n, int sel);
/* a small circular app glyph badge using a macui ICON_* id */
void appui_px_appicon(int icon, int x, int y, int size);

#endif /* APPUI_H */
