/* Aurora window management (aurora_wm.c): keyboard shortcuts, the Alt+Tab switcher, workspaces and their overview
 * (Mission Control), snap previews. Window geometry itself (tiling, maximizing, moving between displays) is done by
 * desktop.c through the aurora_host_* services below.
 *
 * Shortcuts (Super = the Windows/Command key): Super alone opens Launchpad; Super+Left/Right tile the window to the
 * left/right half of its display; Super+Up maximizes, Super+Down restores (or minimizes a normal window);
 * Super+Shift+Left/Right moves it to the neighbouring display; Super+D shows the desktop (and back); Super+L locks;
 * Super+W (or F3) opens the overview; Super+1..4 switch workspace, Super+Shift+1..4 move the window there;
 * Ctrl+Super+Left/Right switch to the previous/next workspace; Alt+Tab / Alt+Shift+Tab switch windows. */
#ifndef AURORA_WM_H
#define AURORA_WM_H
#include <stdbool.h>
#include <stdint.h>

enum { AURORA_TILE_NONE, AURORA_TILE_LEFT, AURORA_TILE_RIGHT };
#define AURORA_WORKSPACES 4
enum { AURORA_MOD_SHIFT = 1, AURORA_MOD_CTRL = 2, AURORA_MOD_ALT = 4, AURORA_MOD_SUPER = 8 };
struct aurora_window;   /* a toplevel of desktop.c */
struct aurora_window_info {
    const char *title, *app_id; int x, y, w, h;     /* desktop rectangle of the window geometry */
    bool minimized, maximized, fullscreen, focused; int tile, workspace, screen, icon;
};

/* ---- services of desktop.c ---- */
int aurora_host_windows(struct aurora_window** list, int capacity);   /* toplevels with content, topmost first, all workspaces */
void aurora_host_window_info(struct aurora_window* w, struct aurora_window_info* info);
struct aurora_window* aurora_host_focus(void);
void aurora_host_focus_none(void);                                     /* keyboard focus to nobody while an overlay is open */
void aurora_host_activate(struct aurora_window* w);                    /* its workspace, unminimized, raised and focused */
void aurora_host_minimize(struct aurora_window* w, bool minimized);
void aurora_host_tile(struct aurora_window* w, int tile, bool maximize); /* AURORA_TILE_*, or maximize; NONE+false restores */
void aurora_host_move_screen(struct aurora_window* w, int direction);  /* to the display left (-1) or right (+1) of its own */
void aurora_host_window_workspace(struct aurora_window* w, int workspace);
int aurora_host_workspace(void);
void aurora_host_set_workspace(int workspace);
void aurora_host_close(struct aurora_window* w);
void aurora_host_desktop_toggle(bool hide);   /* Super+D: minimize the workspace's windows, or bring those back */
void aurora_host_lock(void);
void aurora_host_launchpad(void);
void aurora_host_redraw(void);                                         /* the overlay changed: repaint it */
/* The window's content (its surface tree) scaled into width x height ARGB pixels (cleared first); false if it has none. */
bool aurora_host_thumbnail(struct aurora_window* w, uint32_t* pixels, int width, int height);
void aurora_host_screen_rect(int screen, int* x, int* y, int* w, int* h);   /* -1: the primary display */
int aurora_host_screen_count(void);

/* ---- aurora_wm.c ---- */
bool aurora_wm_key(unsigned key, int value, unsigned modifiers);       /* evdev key; true: consumed by the window manager */
bool aurora_wm_overlay(void);                                          /* switcher or overview shown */
bool aurora_wm_overview(void);                                         /* the overview: windows are shown only as its thumbnails */
bool aurora_wm_overlay_bounds(int* x, int* y, int* w, int* h);         /* desktop rectangle the overlay covers */
/* Draw the overlay part on the display whose desktop rectangle is dx,dy,dw,dh into the current aurora_canvas
 * (output pixels, scale k). */
void aurora_wm_draw(int dx, int dy, int dw, int dh, int k);
bool aurora_wm_pointer(int x, int y, int button, bool down);           /* desktop coordinates; true: consumed */
void aurora_wm_motion(int x, int y);
void aurora_wm_windows_changed(void);                                  /* a window appeared, went or changed */
#endif
