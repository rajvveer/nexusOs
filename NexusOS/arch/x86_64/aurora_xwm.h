/* Rootless Xwayland managed by Aurora (aurora_xwm.c): X11 programs get real Aurora windows.
 *
 * Server: Aurora owns the X display socket (/tmp/.X11-unix/XN and its abstract twin, N = 0 or the first free of 2..32;
 * :1 stays the rootful nexus-x11-session's) and its lock file, and starts `Xwayland :N -rootless -wm FD -listenfd ...`
 * as its child when the first X client connects (an Xwayland that exits is started again by the next connection).
 * Access: MIT-MAGIC-COOKIE-1 in $XDG_RUNTIME_DIR/aurora-xauth-N (0600). Programs get DISPLAY/XAUTHORITY only when
 * they ask (Wine 10 prefers winex11 whenever DISPLAY is set, and Aurora's Wine path is winewayland): a launch file
 * line "X11=rootless", `nexus-x11-session --rootless PROGRAM ARGS`, or $XDG_RUNTIME_DIR/aurora-x11.env (DISPLAY=,
 * XAUTHORITY= lines); `export=1` in ~/.aurora/x11.conf exports them to everything Aurora starts, `rootless=0` turns
 * the server off.
 *
 * Window manager: Aurora is the X11 window manager on the -wm connection (its own X protocol client, no libxcb):
 * SubstructureRedirect on the root, Composite manual redirection, MapRequest/ConfigureRequest/UnmapNotify/
 * DestroyNotify, decorated windows reparented into a frame whose title bar (traffic lights, title) Aurora draws,
 * ICCCM (WM_STATE, WM_PROTOCOLS: WM_DELETE_WINDOW / WM_TAKE_FOCUS, WM_HINTS input, WM_NORMAL_HINTS, WM_TRANSIENT_FOR,
 * WM_CLASS) and EWMH (_NET_SUPPORTED, _NET_SUPPORTING_WM_CHECK, _NET_ACTIVE_WINDOW, _NET_CLIENT_LIST[_STACKING],
 * _NET_WM_NAME, _NET_WM_STATE fullscreen/maximized/hidden/modal, _NET_WM_WINDOW_TYPE, _NET_WM_MOVERESIZE,
 * _NET_CLOSE_WINDOW, _NET_FRAME_EXTENTS, _NET_REQUEST_FRAME_EXTENTS, _NET_WM_PID, _GTK_FRAME_EXTENTS, _MOTIF_WM_HINTS).
 * Surfaces are matched to X windows through xwayland_shell_v1 serials (WL_SURFACE_SERIAL), or WL_SURFACE_ID with an
 * older Xwayland. CLIPBOARD is bridged both ways with the Wayland selection (TARGETS, INCR in both directions).
 * Override-redirect windows (menus, tooltips, drag icons) are shown unmanaged at their X position above every window. */
#ifndef AURORA_XWM_H
#define AURORA_XWM_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server.h>

struct aurora_window;   /* a toplevel of desktop.c */
struct aurora_xwin;     /* aurora_xwm.c: an X window shown by Aurora */

/* aurora_xwm_frame_hit(): resize edges are xdg_toplevel's (1 top, 2 bottom, 4 left, 8 right and their sums) */
enum { AURORA_XFRAME_CLIENT = 0, AURORA_XFRAME_TITLE = 0x100, AURORA_XFRAME_CLOSE, AURORA_XFRAME_MINIMIZE, AURORA_XFRAME_ZOOM };
enum { AURORA_XCONF_MAXIMIZED = 1, AURORA_XCONF_FULLSCREEN = 2, AURORA_XCONF_ACTIVATED = 4, AURORA_XCONF_TILED = 8 };
enum { AURORA_XREQ_MAXIMIZE = 1, AURORA_XREQ_UNMAXIMIZE, AURORA_XREQ_FULLSCREEN, AURORA_XREQ_UNFULLSCREEN, AURORA_XREQ_MINIMIZE,
       AURORA_XREQ_ACTIVATE };

/* ---- desktop.c -> aurora_xwm.c ---- */
bool aurora_xwm_init(struct wl_display* display);                      /* Aurora mode: sockets, globals, control command */
void aurora_xwm_child_env(void);                                       /* in a forked child: DISPLAY + XAUTHORITY */
bool aurora_xwm_global_visible(const struct wl_client* client, const struct wl_global* global);   /* xwayland_shell_v1 */
bool aurora_xwm_reaped(pid_t pid, int status);                         /* true: it was Xwayland */
void aurora_xwm_surface_created(struct wl_resource* surface);          /* WL_SURFACE_ID association */
void aurora_xwm_detached(struct aurora_xwin* w);                       /* its X11 role ended (the surface went) */
void aurora_xwm_configure(struct aurora_xwin* w, int x, int y, int width, int height, unsigned flags);   /* outer frame */
void aurora_xwm_sync(struct aurora_xwin* w, int x, int y, bool minimized);   /* before every paint */
void aurora_xwm_focus(struct aurora_xwin* w);                          /* NULL: no X window has the keyboard */
void aurora_xwm_close(struct aurora_xwin* w);                          /* WM_DELETE_WINDOW, else XKillClient */
int aurora_xwm_frame_hit(struct aurora_xwin* w, int x, int y);         /* surface-local point on the frame */
bool aurora_xwm_takes_focus(struct aurora_xwin* w);                    /* false: override-redirect or "no input" */
void aurora_xwm_selection_changed(const void* selection, bool from_x11);   /* the Wayland selection (maybe the same) */
void aurora_xwm_selection_send(const char* mime, int fd);              /* the X CLIPBOARD to fd (fd is ours to close) */

/* ---- desktop.c services ---- */
struct aurora_window* aurora_host_x11_attach(struct wl_resource* surface, struct aurora_xwin* w, bool unmanaged, int x, int y,
                                             int width, int height, const char* title, const char* app_id);   /* NULL: has a role */
void aurora_host_x11_detach(struct aurora_window* w);
void aurora_host_x11_title(struct aurora_window* w, const char* title, const char* app_id);
void aurora_host_x11_moved(struct aurora_window* w, int x, int y);    /* the X side moved it (desktop coordinates) */
void aurora_host_x11_resized(struct aurora_window* w, int width, int height);   /* the X side sized the frame */
void aurora_host_x11_limits(struct aurora_window* w, int min_w, int min_h, int max_w, int max_h);
void aurora_host_x11_geometry(struct aurora_window* w, int x, int y, int width, int height);   /* visible part (CSD shadows) */
#define AURORA_XICON 64                                                      /* dock picture of an X window's _NET_WM_ICON */
void aurora_host_x11_icon(struct aurora_window* w, const uint32_t* argb);   /* AURORA_XICON^2 ARGB; NULL: none */
void aurora_host_x11_transient(struct aurora_window* w, struct aurora_window* parent, bool modal);
void aurora_host_x11_request(struct aurora_window* w, int request);  /* AURORA_XREQ_* from the client */
bool aurora_host_x11_begin(struct aurora_window* w, uint32_t edges);  /* interactive move (0) or resize: left button held */
void aurora_host_x11_place(struct aurora_window* parent, int width, int height, int* x, int* y);
void aurora_host_x11_work_area(int x, int y, int* ax, int* ay, int* aw, int* ah);   /* of the display at (or nearest) x,y */
bool aurora_host_x11_focused(void);                                   /* the keyboard focus is an X window */
int aurora_host_x11_stack(struct aurora_xwin** list, int capacity);   /* managed X windows, bottom to top */
void aurora_host_x11_selection(const char* const* mimes, int count);  /* the X CLIPBOARD owner's types; 0: it went away */
bool aurora_host_selection_send(const char* mime, int fd);             /* the Wayland selection to fd (always closed) */
int aurora_host_selection_types(const char** types, int capacity);     /* MIME types of the current Wayland selection */
void aurora_host_canvas_restore(void);                                 /* after drawing into a private canvas */
#endif
