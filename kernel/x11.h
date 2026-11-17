/* ============================================================================
 * NexusOS — X11 Compatibility Shim (Header) — Phase 33
 * ============================================================================
 * Minimal X11 API mapped to NexusOS window manager and graphics primitives.
 * This is NOT a full X server — it's a direct API shim for running simple
 * X11-style apps within the NexusOS kernel environment.
 * ============================================================================ */

#ifndef X11_H
#define X11_H

#include "types.h"

/* ============================================================================
 * X11 Basic Types
 * ============================================================================ */

typedef uint32_t Window;
typedef uint32_t Drawable;
typedef uint32_t Pixmap;
typedef uint32_t Colormap;
typedef uint32_t Atom;
typedef uint32_t Time;
typedef uint32_t KeySym;
typedef uint32_t KeyCode;
typedef uint32_t Cursor;
typedef uint32_t Font;
typedef int      Bool;
typedef int      Status;

#define True  1
#define False 0
#define None  0

/* Special window IDs */
#define X11_ROOT_WINDOW    1
#define X11_MAX_WINDOWS   16
#define X11_MAX_GCS       32
#define X11_MAX_PIXMAPS    8
#define X11_MAX_ATOMS     64
#define X11_MAX_EVENTS    32

/* ============================================================================
 * X11 Event Types
 * ============================================================================ */

#define KeyPress          2
#define KeyRelease        3
#define ButtonPress       4
#define ButtonRelease     5
#define MotionNotify      6
#define EnterNotify       7
#define LeaveNotify       8
#define FocusIn           9
#define FocusOut         10
#define Expose           12
#define DestroyNotify    17
#define UnmapNotify      18
#define MapNotify        19
#define ConfigureNotify  22
#define ClientMessage    33

/* ============================================================================
 * X11 Event Masks
 * ============================================================================ */

#define NoEventMask              0x00000000
#define KeyPressMask             0x00000001
#define KeyReleaseMask           0x00000002
#define ButtonPressMask          0x00000004
#define ButtonReleaseMask        0x00000008
#define PointerMotionMask        0x00000040
#define ExposureMask             0x00008000
#define StructureNotifyMask      0x00020000
#define FocusChangeMask          0x00200000

/* ============================================================================
 * X11 GC Values Mask
 * ============================================================================ */

#define GCForeground   (1 << 0)
#define GCBackground   (1 << 1)
#define GCLineWidth    (1 << 2)
#define GCFont         (1 << 3)

/* Line styles */
#define LineSolid      0
#define LineOnOffDash  1

/* Cap styles */
#define CapButt        1
#define CapRound       2

/* Join styles */
#define JoinMiter      0
#define JoinRound      1

/* Fill styles */
#define FillSolid      0

/* Arc modes */
#define ArcChord       0
#define ArcPieSlice    1

/* ============================================================================
 * X11 Structures
 * ============================================================================ */

/* Graphics Context values */
typedef struct {
    uint32_t foreground;
    uint32_t background;
    int line_width;
    int line_style;
    int cap_style;
    int join_style;
    int fill_style;
    int font_id;
} XGCValues;

/* Graphics Context */
typedef struct {
    bool     active;
    uint32_t id;
    XGCValues values;
} x11_gc_t;

/* X11 Window state (maps to NexusOS window) */
typedef struct {
    bool     active;
    Window   id;
    Window   parent;
    int      nx_win_id;     /* NexusOS window manager ID */
    int      x, y;
    int      width, height;
    int      border_width;
    uint32_t background;
    uint32_t border_color;
    uint32_t event_mask;
    bool     mapped;
    bool     override_redirect;
    char     name[64];
} x11_window_t;

/* X11 Pixmap */
typedef struct {
    bool     active;
    Pixmap   id;
    int      width, height;
    uint32_t *data;
} x11_pixmap_t;

/* X11 Atom entry */
typedef struct {
    bool active;
    Atom id;
    char name[32];
} x11_atom_t;

/* Event structures */
typedef struct {
    int type;
    Window window;
    int x, y;
    int width, height;
    int count;            /* Expose: more events to come */
} XExposeEvent;

typedef struct {
    int type;
    Window window;
    uint32_t state;       /* modifier mask */
    KeyCode keycode;
    KeySym  keysym;
    char    string[8];    /* translated character */
    int     length;
    Time    time;
} XKeyEvent;

typedef struct {
    int type;
    Window window;
    int x, y;
    int x_root, y_root;
    uint32_t button;
    uint32_t state;
    Time time;
} XButtonEvent;

typedef struct {
    int type;
    Window window;
    int x, y;
    int x_root, y_root;
    uint32_t state;
    Time time;
} XMotionEvent;

typedef struct {
    int type;
    Window window;
    int x, y;
    int width, height;
} XConfigureEvent;

typedef struct {
    int type;
    Window window;
    Atom   message_type;
    int    format;
    uint32_t data[5];
} XClientMessageEvent;

/* Union of all event types */
typedef union {
    int               type;
    XExposeEvent      xexpose;
    XKeyEvent         xkey;
    XButtonEvent      xbutton;
    XMotionEvent      xmotion;
    XConfigureEvent   xconfigure;
    XClientMessageEvent xclient;
    Window            xany_window;
    /* Padding to ensure consistent size */
    char              pad[64];
} XEvent;

/* Display connection */
typedef struct {
    bool     active;
    int      screen;
    int      width;
    int      height;
    int      depth;
    Window   root;
    Colormap default_colormap;
    uint32_t black_pixel;
    uint32_t white_pixel;
} Display;

/* Screen info */
typedef struct {
    Window   root;
    int      width, height;
    int      depth;
    Colormap colormap;
} Screen;

/* ============================================================================
 * X11 Core API — Display Management
 * ============================================================================ */

Display* XOpenDisplay(const char* display_name);
void     XCloseDisplay(Display* dpy);
int      XDefaultScreen(Display* dpy);
int      XDisplayWidth(Display* dpy, int screen);
int      XDisplayHeight(Display* dpy, int screen);
Window   XRootWindow(Display* dpy, int screen);
int      XDefaultDepth(Display* dpy, int screen);
uint32_t XBlackPixel(Display* dpy, int screen);
uint32_t XWhitePixel(Display* dpy, int screen);

/* ============================================================================
 * X11 Core API — Window Management
 * ============================================================================ */

Window  XCreateSimpleWindow(Display* dpy, Window parent,
                            int x, int y, int w, int h,
                            int border_width, uint32_t border,
                            uint32_t background);
void    XDestroyWindow(Display* dpy, Window win);
void    XMapWindow(Display* dpy, Window win);
void    XUnmapWindow(Display* dpy, Window win);
void    XMoveWindow(Display* dpy, Window win, int x, int y);
void    XResizeWindow(Display* dpy, Window win, int w, int h);
void    XRaiseWindow(Display* dpy, Window win);
void    XSetWindowBackground(Display* dpy, Window win, uint32_t pixel);
void    XClearWindow(Display* dpy, Window win);
void    XStoreName(Display* dpy, Window win, const char* name);

/* ============================================================================
 * X11 Core API — Graphics Context
 * ============================================================================ */

typedef uint32_t GC;

GC      XCreateGC(Display* dpy, Drawable d, uint32_t valuemask, XGCValues* values);
void    XFreeGC(Display* dpy, GC gc);
void    XSetForeground(Display* dpy, GC gc, uint32_t pixel);
void    XSetBackground(Display* dpy, GC gc, uint32_t pixel);
void    XSetLineAttributes(Display* dpy, GC gc, int line_width,
                           int line_style, int cap_style, int join_style);

/* ============================================================================
 * X11 Core API — Drawing
 * ============================================================================ */

void    XDrawPoint(Display* dpy, Drawable d, GC gc, int x, int y);
void    XDrawLine(Display* dpy, Drawable d, GC gc, int x1, int y1, int x2, int y2);
void    XDrawRectangle(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h);
void    XFillRectangle(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h);
void    XDrawArc(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h,
                 int angle1, int angle2);
void    XFillArc(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h,
                 int angle1, int angle2);
void    XDrawString(Display* dpy, Drawable d, GC gc, int x, int y,
                    const char* str, int len);
void    XClearArea(Display* dpy, Window win, int x, int y, int w, int h, Bool exposures);

/* ============================================================================
 * X11 Core API — Events
 * ============================================================================ */

void    XSelectInput(Display* dpy, Window win, uint32_t event_mask);
int     XNextEvent(Display* dpy, XEvent* event_return);
int     XPending(Display* dpy);
Bool    XCheckMaskEvent(Display* dpy, uint32_t event_mask, XEvent* event_return);

/* ============================================================================
 * X11 Core API — Atoms & Properties
 * ============================================================================ */

Atom    XInternAtom(Display* dpy, const char* atom_name, Bool only_if_exists);
char*   XGetAtomName(Display* dpy, Atom atom);

/* ============================================================================
 * X11 Core API — Misc
 * ============================================================================ */

void    XFlush(Display* dpy);
void    XSync(Display* dpy, Bool discard);
void    XBell(Display* dpy, int percent);
int     XConnectionNumber(Display* dpy);

/* ============================================================================
 * NexusOS Extensions — X11 Subsystem Management
 * ============================================================================ */

void    x11_init(void);
int     x11_get_window_count(void);
int     x11_get_gc_count(void);
void    x11_pump_events(void);    /* Called from main loop to inject input events */
const char* x11_get_status(void);

#endif /* X11_H */
