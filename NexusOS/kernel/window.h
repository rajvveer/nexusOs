/* ============================================================================
 * NexusOS — Window Manager (Header) — Phase 17
 * ============================================================================
 * Pixel-perfect windows with rounded borders, gradient titlebars,
 * drop shadows, resize handles, and smooth dragging.
 * ============================================================================ */

#ifndef WINDOW_H
#define WINDOW_H

#include "types.h"
#include "gui.h"

/* Limits */
#define MAX_WINDOWS     8
#define WIN_TITLE_MAX   30

/* Window flags */
#define WIN_VISIBLE     0x01
#define WIN_MOVABLE     0x02
#define WIN_CLOSABLE    0x04
#define WIN_FOCUSED     0x08
#define WIN_MINIMIZED   0x10
#define WIN_MAXIMIZED   0x20
#define WIN_RESIZABLE   0x40

/* Resize edge identifiers */
typedef enum {
    RESIZE_NONE = 0,
    RESIZE_N, RESIZE_S, RESIZE_E, RESIZE_W,
    RESIZE_NE, RESIZE_NW, RESIZE_SE, RESIZE_SW
} resize_edge_t;

/* Window dimensions in pixels */
#define WIN_TITLEBAR_H  16      /* Titlebar height (= 1 cell for alignment) */
#define WIN_BORDER_W    1       /* Border width */
#define WIN_CORNER_R    6       /* Corner radius */
#define WIN_RESIZE_GRAB 5       /* Resize grab zone pixels */
#define WIN_MIN_W       120     /* Minimum width */
#define WIN_MIN_H       80      /* Minimum height */
#define WIN_SHADOW_R    4       /* Shadow radius */
#define WIN_BTN_SIZE    12      /* Title button size */

/* Window content draw callback — pixel coordinates */
typedef void (*win_draw_fn)(int win_id, int cx, int cy, int cw, int ch);

/* Window key handler callback */
typedef void (*win_key_fn)(int win_id, char key);

/* Window structure */
typedef struct {
    int         id;
    bool        active;
    char        title[WIN_TITLE_MAX];

    /* Pixel coordinates */
    int         px, py, pw, ph;

    /* Character-cell coordinates (for backward compat) */
    int         x, y, w, h;

    /* Saved state for maximize/restore */
    int         saved_x, saved_y;
    int         saved_w, saved_h;

    uint8_t     flags;
    int         z_order;

    /* Resize state */
    resize_edge_t resize_edge;
    bool        resizing;

    /* Animation state */
    int         anim_frame;     /* 0 = no animation */
    int         anim_type;      /* 0=none, 1=minimize, 2=maximize, 3=close */

    /* Callbacks */
    win_draw_fn draw;
    win_key_fn  on_key;
} window_t;

/* Window management */
int  window_create(const char* title, int x, int y, int w, int h,
                   win_draw_fn draw_fn, win_key_fn key_fn);
void window_destroy(int id);
void window_focus(int id);
void window_move(int id, int new_x, int new_y);
void window_draw(int id);
void window_draw_all(void);

/* Pixel-based positioning (Phase 17) */
void window_move_px(int id, int new_px, int new_py);
void window_resize_px(int id, int new_w, int new_h);

/* Hit testing */
int  window_hit_test(int mx, int my);
bool window_titlebar_hit(int id, int mx, int my);
bool window_close_hit(int id, int mx, int my);
resize_edge_t window_resize_hit(int id, int mx, int my);

/* Pixel-based hit testing (Phase 17) */
int  window_hit_test_px(int mx, int my);
bool window_titlebar_hit_px(int id, int mx, int my);
bool window_close_hit_px(int id, int mx, int my);
bool window_minimize_hit_px(int id, int mx, int my);
bool window_maximize_hit_px(int id, int mx, int my);

/* Accessors */
window_t* window_get(int id);
int  window_get_focused(void);
int  window_count(void);

/* Send key to focused window */
void window_send_key(char key);

/* Minimize/maximize/restore */
void window_minimize(int id);
void window_maximize(int id);
void window_restore(int id);

/* Legacy hit test for minimize/maximize buttons */
bool window_minimize_hit(int id, int mx, int my);
bool window_maximize_hit(int id, int mx, int my);

/* Window snapping */
void window_snap_left(int id);
void window_snap_right(int id);

#endif /* WINDOW_H */
