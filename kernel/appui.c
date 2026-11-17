/* ============================================================================
 * NexusOS — App UI kit (Aurora, pixel implementation)
 * ============================================================================ */

#include "appui.h"
#include "macui.h"
#include "framebuffer.h"
#include "window.h"
#include "string.h"
#include "vga.h"

#define CW 8
#define CH 16

/* The theme still returns VGA attributes: any app code that draws straight to
 * gui_putchar keeps working, and the cell-API helpers below map those
 * attributes through the (now macOS-dark) palette to real RGB. */
appui_theme_t appui_theme(void) {
    appui_theme_t t;
    t.bg = VGA_BLACK;
    t.panel = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);
    t.toolbar = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY);
    t.text = VGA_COLOR(VGA_WHITE, VGA_BLACK);
    t.muted = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK);
    t.accent = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK);
    t.good = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK);
    t.warn = VGA_COLOR(VGA_YELLOW, VGA_BLACK);
    t.danger = VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK);
    t.selected = VGA_COLOR(VGA_WHITE, VGA_BLUE);
    t.status = VGA_COLOR(VGA_LIGHT_GREY, VGA_DARK_GREY);
    return t;
}

/* ---- pixel helpers ---------------------------------------------------------*/
#ifdef NEXUS_AURORA
void (*appui_a11y_note)(int kind, int x, int y, int w, int h, const char* text, int state);
#define A11Y(k, x, y, w, h, t, s) do { if (appui_a11y_note) appui_a11y_note(k, x, y, w, h, t, s); } while (0)
#else
#define A11Y(k, x, y, w, h, t, s) do { } while (0)
#endif
static int FH(int f) { return ui_font_height(f); }
/* vertical offset to centre a line of font f in a row of height h */
static int vcenter(int f, int h) { int o = (h - (FH(f) - 4)) / 2; return o < 0 ? 0 : o; }

void appui_px_fill(int x, int y, int w, int h, uint32_t c) { ui_blend_rect(x, y, w, h, c, 255); }
void appui_px_card(int x, int y, int w, int h, int r, uint32_t c) { ui_rrect(x, y, w, h, r, c, 255); }
void appui_px_sep(int x, int y, int w) { ui_blend_rect(x, y, w, 1, APPUI_SEP, 255); }
void appui_px_vsep(int x, int y, int h) { ui_blend_rect(x, y, 1, h, APPUI_SEP, 255); }
int  appui_px_text(int x, int y, const char* s, int f, uint32_t c) { int end = ui_text(x, y, s, f, c); A11Y(APPUI_A11Y_TEXT, x, y, end - x, FH(f), s, f); return end; }
void appui_px_text_c(int cx, int y, const char* s, int f, uint32_t c) { ui_text_center(cx, y, s, f, c); A11Y(APPUI_A11Y_TEXT, cx - ui_text_width(s, f) / 2, y, ui_text_width(s, f), FH(f), s, f); }
void appui_px_text_r(int rx, int y, const char* s, int f, uint32_t c) { ui_text(rx - ui_text_width(s, f), y, s, f, c); A11Y(APPUI_A11Y_TEXT, rx - ui_text_width(s, f), y, ui_text_width(s, f), FH(f), s, f); }
int  appui_px_text_w(const char* s, int f) { return ui_text_width(s, f); }
int  appui_px_font_h(int f) { return FH(f); }

bool appui_content(int id, int* x, int* y, int* w, int* h) { return window_content_px(id, x, y, w, h); }

void appui_px_row(int x, int y, int w, int h, bool selected) {
    if (selected) ui_rrect(x, y, w, h, h > 28 ? 8 : 6, APPUI_ACCENT, 255);
    A11Y(APPUI_A11Y_ROW, x, y, w, h, NULL, selected);
}

void appui_px_field(int x, int y, int w, int h, const char* text, bool caret) {
    ui_rrect(x, y, w, h, h / 2 > 10 ? 8 : h / 2, APPUI_FIELD, 255);
    ui_rrect_stroke(x, y, w, h, h / 2 > 10 ? 8 : h / 2, 0xFFFFFF, 30);
    int ty = y + vcenter(APPUI_F_BODY, h);
    int end = text && text[0] ? ui_text(x + 12, ty, text, APPUI_F_BODY, APPUI_TEXT) : x + 12;
    if (caret) ui_blend_rect(end + 1, y + 6, 2, h - 12, APPUI_ACCENT, 255);
    A11Y(APPUI_A11Y_FIELD, x, y, w, h, text, caret);
}
void appui_px_secret_field(int x, int y, int w, int h, const char* masked, bool caret) {
#ifdef NEXUS_AURORA
    void (*note)(int, int, int, int, int, const char*, int) = appui_a11y_note; appui_a11y_note = NULL;
    appui_px_field(x, y, w, h, masked, caret); appui_a11y_note = note;
    A11Y(APPUI_A11Y_FIELD, x, y, w, h, "", (caret ? 1 : 0) | 2);   /* state 2: secret */
#else
    appui_px_field(x, y, w, h, masked, caret);
#endif
}

void appui_px_button(int x, int y, int w, int h, const char* label, bool primary) {
    ui_rrect(x, y, w, h, 7, primary ? APPUI_ACCENT : 0x48484E, 255);
    if (!primary) ui_rrect_stroke(x, y, w, h, 7, 0xFFFFFF, 40);
    ui_text_center(x + w / 2, y + vcenter(APPUI_F_BODY, h), label, APPUI_F_BODY, primary ? 0xFFFFFF : APPUI_TEXT);
    A11Y(APPUI_A11Y_BUTTON, x, y, w, h, label, primary);
}

void appui_px_toggle(int x, int y, bool on) {
    int w = 40, h = 22;
    ui_rrect(x, y, w, h, h / 2, on ? APPUI_GREEN : 0x55555C, 255);
    ui_circle(x + (on ? w - h / 2 : h / 2), y + h / 2, h / 2 - 3, 0xFFFFFF, 255);
    A11Y(APPUI_A11Y_TOGGLE, x, y, w, h, NULL, on);
}

void appui_px_segment(int x, int y, int w, int h, const char* const* labels, int n, int sel) {
    ui_rrect(x, y, w, h, 7, 0x3A3A40, 255);
    int sw = w / n;
    for (int i = 0; i < n; i++) {
        if (i == sel) ui_rrect(x + i * sw + 2, y + 2, sw - 4, h - 4, 6, 0x5A5A62, 255);
        ui_text_center(x + i * sw + sw / 2, y + vcenter(APPUI_F_BODY, h), labels[i], APPUI_F_BODY,
                       i == sel ? 0xFFFFFF : APPUI_SUBTLE);
        A11Y(APPUI_A11Y_SEGMENT, x + i * sw, y, sw, h, labels[i], i == sel);
    }
}

void appui_px_appicon(int icon, int x, int y, int size) { ui_icon(icon, x, y, size); }

/* ---- cell-coordinate API (rendered as pixels) -----------------------------*/
void appui_fill(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    ui_blend_rect(x * CW, y * CH, w * CW, h * CH, vga_attr_bg(color), 255);
}

static void clip_copy(char* dst, const char* src, int max) {
    int i = 0;
    for (; src && src[i] && i < max && i < 127; i++) dst[i] = src[i];
    dst[i] = 0;
}

void appui_text(int x, int y, const char* text, int max, uint8_t color) {
    if (!text || max <= 0) return;
    char buf[128];
    clip_copy(buf, text, max);
    ui_text(x * CW, y * CH + 1, buf, APPUI_F_BODY, vga_attr_fg(color));
}

void appui_hline(int x, int y, int w, uint8_t color) {
    (void)color;
    if (w <= 0) return;
    ui_blend_rect(x * CW, y * CH + CH / 2, w * CW, 1, APPUI_SEP, 255);
}

void appui_header(int x, int y, int w, const char* title, const char* subtitle) {
    int px = x * CW, py = y * CH, pw = w * CW;
    ui_blend_rect(px, py, pw, 2 * CH, APPUI_BG, 255);
    if (title) ui_text(px + 2, py + 1, title, APPUI_F_BOLD, APPUI_TEXT);
    if (subtitle) ui_text(px + 2, py + CH + 1, subtitle, APPUI_F_SMALL, APPUI_SUBTLE);
    appui_px_sep(px, py + 2 * CH - 1, pw);
}

void appui_status(int x, int y, int w, const char* text) {
    int px = x * CW, py = y * CH, pw = w * CW;
    ui_blend_rect(px, py, pw, CH, APPUI_PANEL, 255);
    appui_px_sep(px, py, pw);
    char buf[128];
    clip_copy(buf, text, w - 2);
    ui_text(px + 6, py + 1, buf, APPUI_F_SMALL, APPUI_SUBTLE);
}

void appui_row(int x, int y, int w, bool selected, uint8_t normal, uint8_t selected_color) {
    (void)normal; (void)selected_color;
    if (selected) ui_rrect(x * CW + 2, y * CH, w * CW - 4, CH, 6, APPUI_ACCENT, 255);
}

void appui_kv(int x, int y, const char* key, const char* value, int value_x, int max) {
    char kb[128], vb[128];
    clip_copy(kb, key, value_x - x - 1);
    clip_copy(vb, value, max);
    ui_text(x * CW, y * CH + 1, kb, APPUI_F_BODY, APPUI_SUBTLE);
    ui_text(value_x * CW, y * CH + 1, vb, APPUI_F_BODY, APPUI_TEXT);
}

void appui_input(int x, int y, int w, const char* text, int len, bool cursor) {
    (void)len;
    char buf[128];
    clip_copy(buf, text, w - 2);
    appui_px_field(x * CW, y * CH - 1, w * CW, CH + 2, buf, cursor);
}
