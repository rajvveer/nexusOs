/* ============================================================================
 * NexusOS — Aurora UI toolkit (Header)
 * ============================================================================
 * The macOS-style look of the desktop: anti-aliased proportional text, alpha
 * blended rounded shapes, soft shadows, the procedural wallpaper, app icons,
 * the pointer and the boot splash. Everything draws into the 32bpp back buffer.
 * ============================================================================ */

#ifndef MACUI_H
#define MACUI_H

#include "types.h"

/* Top menu bar height and the first pixel row windows may occupy (2 cells). */
#define MENUBAR_H       28
#define UI_TOP_RESERVE  32

#define UI_RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

/* Fonts (see uifont.h) */
enum { UF_UI = 0, UF_UIB, UF_SMALL, UF_TITLE, UF_HUGE, UF_COUNT };

/* Special launcher actions (ordinary apps use the desktop's action ids) */
#define UI_ACT_LAUNCHPAD  100
#define UI_ACT_ABOUT      101
#define UI_ACT_SPOTLIGHT  102
#define UI_ACT_LOCK       103
#define UI_ACT_RESTART    104
#define UI_ACT_SHUTDOWN   105
#define UI_ACT_CONSOLE    106
#define UI_ACT_SLEEP      107
#define UI_ACT_FORCEQUIT  108
#define UI_ACT_CLOSEWIN   109
#define UI_ACT_MINIMIZE   110
#define UI_ACT_ZOOM       111
#define UI_ACT_SHORTCUTS  112
#define UI_ACT_NOTIFS     113
#define UI_ACT_NEWFILE    114
/* Aurora power (arch/x86_64/aurora_power.c) */
#define UI_ACT_HIBERNATE     140
#define UI_ACT_BATTERY       141   /* menu-bar battery/brightness item: toggle its panel */
#define UI_ACT_POWERDIALOG   142   /* Shut Down / Restart / Sleep / Cancel dialog */
#define UI_ACT_SHUTDOWN_NOW  143   /* start Shut Down (never cancels one in progress) */
#define UI_ACT_LOGOUT        144   /* Aurora: log out of the active session (aurora_session.h) */
#define UI_ACT_SWITCHUSER    145   /* Aurora: the greeter, the session stays */
#define UI_ACT_SETTINGS   50     /* Aurora: System Settings (aurora-app settings) */
#define UI_ACT_INPUT_NEXT 51     /* Aurora: the next keyboard layout (menu bar input menu) */

/* Colour helpers */
uint32_t ui_mix(uint32_t dst, uint32_t src, int a);      /* a = 0..256 */

/* Text. y is the top of the line box; returns the pen x after the text. */
int  ui_text(int x, int y, const char* s, int font, uint32_t color);
int  ui_text_n(int x, int y, const char* s, int n, int font, uint32_t color);
int  ui_text_width(const char* s, int font);
int  ui_font_height(int font);
void ui_text_center(int cx, int y, const char* s, int font, uint32_t color);
/* Anti-aliased 8x16 monospace cell glyph; false if c has no smooth glyph. */
bool ui_cell_glyph(int px, int py, uint8_t c, uint32_t fg, uint32_t bg);

/* Shapes. alpha = 0..255. Coordinates are pixels; shapes are clipped. */
void ui_blend_rect(int x, int y, int w, int h, uint32_t color, int alpha);
void ui_rrect(int x, int y, int w, int h, int r, uint32_t color, int alpha);
void ui_rrect_stroke(int x, int y, int w, int h, int r, uint32_t color, int alpha);
void ui_rrect_vgrad(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int alpha);
void ui_circle(int cx, int cy, int r, uint32_t color, int alpha);
void ui_ring(int cx, int cy, int r, int thick, uint32_t color, int alpha);
void ui_arc_up(int cx, int cy, int r, int thick, uint32_t color, int alpha);
void ui_line(int x0, int y0, int x1, int y1, int thick, uint32_t color, int alpha);
void ui_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, int alpha);
void ui_shadow(int x, int y, int w, int h, int r, int spread, int alpha, int dy);
/* Frosted panel: tint blended over whatever is already in the back buffer. */
void ui_glass(int x, int y, int w, int h, int r, uint32_t tint, int alpha);

/* Wallpaper (rendered once into a reserved cache, then blitted). */
void     ui_wallpaper_init(void);
void     ui_wallpaper_draw(void);
void     ui_wallpaper_rect(int x, int y, int w, int h);
uint32_t ui_wallpaper_at(int x, int y);

/* Pointer sprite (hotspot at +1,+1). */
#define UI_CURSOR_W 20
#define UI_CURSOR_H 26
void ui_cursor_draw(int x, int y);

/* App icons */
enum {
    ICON_FINDER, ICON_LAUNCHPAD, ICON_WEB, ICON_TERMINAL, ICON_NOTES,
    ICON_CALENDAR, ICON_MUSIC, ICON_CALC, ICON_MONITOR, ICON_SETTINGS,
    ICON_TRASH, ICON_CLOCK, ICON_TASKS, ICON_HELP, ICON_INFO, ICON_REMINDERS,
    ICON_CONTACTS, ICON_HEX, ICON_LOG, ICON_CLIP, ICON_FOLDER, ICON_PAINT,
    ICON_APPEARANCE, ICON_GENERIC, ICON_CHROME,
    ICON_VSCODE, ICON_FIREFOX, ICON_WRITER, ICON_SHEET
};
void ui_icon(int icon, int x, int y, int size);

/* App registry shared by the dock, Launchpad and Spotlight. */
typedef struct {
    const char* name;     /* user-facing (macOS-style) name */
    int         action;   /* desktop action id */
    int         icon;     /* ICON_* */
    const char* title;    /* window title the app opens (running indicator) */
} ui_app_t;
const ui_app_t* ui_apps(int* count);
int             ui_dock_app_count(void);   /* first N registry entries live in the dock */
const ui_app_t* ui_app_by_title(const char* title);   /* NULL if no app owns it */
#ifdef NEXUS_AURORA
/* Registry storage remains owned by the caller; NULL,0,0 restores the legacy registry. */
bool ui_set_apps(const ui_app_t* apps, int count, int dock_count);
/* Icons with ids >= 1000 are drawn by the hook (Aurora's Windows programs); false falls back to ICON_GENERIC. */
void ui_set_icon_hook(bool (*hook)(int icon, int x, int y, int size));
void ui_wallpaper_release(void);
/* An ARGB/XRGB image scaled into the logical rectangle x,y,w,h at the canvas's physical resolution. */
void ui_image(int x, int y, int w, int h, const uint32_t* pixels, int pw, int ph, int stride, bool has_alpha);
/* Larger text (accessibility): every UI font at percent (100..200) of its size. */
void ui_set_text_scale(int percent);
#endif

/* Misc artwork */
void ui_logo(int x, int y, int h, uint32_t color);           /* NexusOS mark */
void ui_avatar(int cx, int cy, int r);                       /* default user picture */
void ui_format_clock(char* out);                             /* "Thu 8 Oct  12:40" */

/* Boot splash: logo + progress bar while the kernel initializes. */
void ui_splash_begin(void);
void ui_splash_step(void);
void ui_splash_end(void);
bool ui_splash_active(void);

#endif /* MACUI_H */
