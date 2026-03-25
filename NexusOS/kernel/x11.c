/* ============================================================================
 * NexusOS — X11 Compatibility Shim (Implementation) — Phase 33
 * ============================================================================
 * Maps X11 API calls to NexusOS window manager and gfx primitives.
 * ============================================================================ */

#include "x11.h"
#include "window.h"
#include "gui.h"
#include "gfx.h"
#include "framebuffer.h"
#include "font.h"
#include "string.h"
#include "keyboard.h"
#include "mouse.h"
#include "vga.h"
#include "speaker.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* ============================================================================
 * Internal State
 * ============================================================================ */

static Display        x11_display;
static x11_window_t   x11_windows[X11_MAX_WINDOWS];
static x11_gc_t       x11_gcs[X11_MAX_GCS];
static x11_atom_t     x11_atoms[X11_MAX_ATOMS];
static XEvent         x11_event_queue[X11_MAX_EVENTS];
static int            x11_eq_head = 0, x11_eq_tail = 0;
static uint32_t       x11_next_id = 100;   /* Next resource ID */
static int            x11_atom_count = 0;
static bool           x11_initialized = false;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static uint32_t alloc_id(void) { return x11_next_id++; }

static x11_window_t* find_xwin(Window id) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++)
        if (x11_windows[i].active && x11_windows[i].id == id) return &x11_windows[i];
    return NULL;
}

static x11_gc_t* find_gc(GC id) {
    for (int i = 0; i < X11_MAX_GCS; i++)
        if (x11_gcs[i].active && x11_gcs[i].id == id) return &x11_gcs[i];
    return NULL;
}

/* Get pixel coordinates of an X11 window's content area (inside NexusOS window) */
static bool get_content_origin(x11_window_t* xw, int* ox, int* oy) {
    if (!xw || xw->nx_win_id < 0) return false;
    window_t* nw = window_get(xw->nx_win_id);
    if (!nw) return false;
    /* Content starts after titlebar, inside borders */
    *ox = nw->px + 1;
    *oy = nw->py + WIN_TITLEBAR_H + 1;
    return true;
}

/* Push event to queue */
static void push_event(XEvent* ev) {
    int next = (x11_eq_head + 1) % X11_MAX_EVENTS;
    if (next == x11_eq_tail) return; /* Queue full, drop */
    x11_event_queue[x11_eq_head] = *ev;
    x11_eq_head = next;
}

/* ============================================================================
 * X11 Draw callback (called by NexusOS window manager)
 * ============================================================================ */

static void x11_win_draw(int id, int cx, int cy, int cw, int ch) {
    /* Generate Expose event for the X11 window */
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (x11_windows[i].active && x11_windows[i].nx_win_id == id) {
            if (x11_windows[i].event_mask & ExposureMask) {
                XEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = Expose;
                ev.xexpose.window = x11_windows[i].id;
                ev.xexpose.x = 0;
                ev.xexpose.y = 0;
                ev.xexpose.width = x11_windows[i].width;
                ev.xexpose.height = x11_windows[i].height;
                ev.xexpose.count = 0;
                push_event(&ev);
            }
            break;
        }
    }
}

/* Key callback */
static void x11_win_key(int id, char key) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (x11_windows[i].active && x11_windows[i].nx_win_id == id) {
            if (x11_windows[i].event_mask & KeyPressMask) {
                XEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = KeyPress;
                ev.xkey.window = x11_windows[i].id;
                ev.xkey.keycode = (KeyCode)key;
                ev.xkey.keysym = (KeySym)key;
                ev.xkey.string[0] = key;
                ev.xkey.length = 1;
                push_event(&ev);
            }
            break;
        }
    }
}

/* ============================================================================
 * Display Management
 * ============================================================================ */

Display* XOpenDisplay(const char* display_name) {
    (void)display_name;
    if (!x11_initialized) x11_init();
    x11_display.active = true;
    x11_display.screen = 0;
    x11_display.width = (int)fb_get_width();
    x11_display.height = (int)fb_get_height();
    x11_display.depth = 24;
    x11_display.root = X11_ROOT_WINDOW;
    x11_display.default_colormap = 1;
    x11_display.black_pixel = 0x000000;
    x11_display.white_pixel = 0xFFFFFF;
    return &x11_display;
}

void XCloseDisplay(Display* dpy) {
    if (!dpy) return;
    /* Destroy all X11 windows */
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (x11_windows[i].active) {
            if (x11_windows[i].nx_win_id >= 0)
                window_destroy(x11_windows[i].nx_win_id);
            x11_windows[i].active = false;
        }
    }
    /* Free all GCs */
    for (int i = 0; i < X11_MAX_GCS; i++) x11_gcs[i].active = false;
    dpy->active = false;
}

int XDefaultScreen(Display* dpy) { return dpy ? dpy->screen : 0; }
int XDisplayWidth(Display* dpy, int s) { (void)s; return dpy ? dpy->width : 1024; }
int XDisplayHeight(Display* dpy, int s) { (void)s; return dpy ? dpy->height : 768; }
Window XRootWindow(Display* dpy, int s) { (void)s; return dpy ? dpy->root : 0; }
int XDefaultDepth(Display* dpy, int s) { (void)s; return dpy ? dpy->depth : 24; }
uint32_t XBlackPixel(Display* dpy, int s) { (void)s; return dpy ? dpy->black_pixel : 0; }
uint32_t XWhitePixel(Display* dpy, int s) { (void)s; return dpy ? dpy->white_pixel : 0xFFFFFF; }

/* ============================================================================
 * Window Management
 * ============================================================================ */

Window XCreateSimpleWindow(Display* dpy, Window parent,
                           int x, int y, int w, int h,
                           int border_width, uint32_t border,
                           uint32_t background) {
    if (!dpy) return None;
    int slot = -1;
    for (int i = 0; i < X11_MAX_WINDOWS; i++)
        if (!x11_windows[i].active) { slot = i; break; }
    if (slot < 0) return None;

    x11_window_t* xw = &x11_windows[slot];
    memset(xw, 0, sizeof(x11_window_t));
    xw->active = true;
    xw->id = alloc_id();
    xw->parent = parent;
    xw->x = x; xw->y = y;
    xw->width = w; xw->height = h;
    xw->border_width = border_width;
    xw->background = background;
    xw->border_color = border;
    xw->mapped = false;
    xw->nx_win_id = -1;
    strcpy(xw->name, "X11 Window");
    return xw->id;
}

void XDestroyWindow(Display* dpy, Window win) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw) return;
    if (xw->nx_win_id >= 0) window_destroy(xw->nx_win_id);
    xw->active = false;
}

void XMapWindow(Display* dpy, Window win) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw || xw->mapped) return;

    /* Create the NexusOS window now */
    /* Convert pixel dimensions to character cells for NexusOS */
    int cx = xw->x / 8;
    int cy = xw->y / 16;
    int cw = (xw->width + WIN_TITLEBAR_H + 4) / 8;
    int ch = (xw->height + WIN_TITLEBAR_H + 4) / 16;
    if (cw < 10) cw = 10;
    if (ch < 5) ch = 5;
    if (cx < 2) cx = 2;
    if (cy < 2) cy = 2;

    int nid = window_create(xw->name, cx, cy, cw, ch, x11_win_draw, x11_win_key);
    if (nid >= 0) {
        xw->nx_win_id = nid;
        xw->mapped = true;

        /* Generate MapNotify */
        if (xw->event_mask & StructureNotifyMask) {
            XEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = MapNotify;
            ev.xconfigure.window = xw->id;
            push_event(&ev);
        }
    }
}

void XUnmapWindow(Display* dpy, Window win) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw || !xw->mapped) return;
    if (xw->nx_win_id >= 0) {
        window_destroy(xw->nx_win_id);
        xw->nx_win_id = -1;
    }
    xw->mapped = false;
}

void XMoveWindow(Display* dpy, Window win, int x, int y) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw) return;
    xw->x = x; xw->y = y;
    if (xw->nx_win_id >= 0) window_move_px(xw->nx_win_id, x, y);
}

void XResizeWindow(Display* dpy, Window win, int w, int h) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw) return;
    xw->width = w; xw->height = h;
    if (xw->nx_win_id >= 0)
        window_resize_px(xw->nx_win_id, w + 4, h + WIN_TITLEBAR_H + 4);
}

void XRaiseWindow(Display* dpy, Window win) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (xw && xw->nx_win_id >= 0) window_focus(xw->nx_win_id);
}

void XSetWindowBackground(Display* dpy, Window win, uint32_t pixel) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (xw) xw->background = pixel;
}

void XClearWindow(Display* dpy, Window win) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw || xw->nx_win_id < 0) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy))
        gfx_fill_rect(ox, oy, xw->width, xw->height, xw->background);
}

void XStoreName(Display* dpy, Window win, const char* name) {
    if (!dpy || !name) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw) return;
    strncpy(xw->name, name, 63);
    xw->name[63] = '\0';
    /* Update NexusOS window title */
    if (xw->nx_win_id >= 0) {
        window_t* nw = window_get(xw->nx_win_id);
        if (nw) { strncpy(nw->title, name, WIN_TITLE_MAX - 1); nw->title[WIN_TITLE_MAX - 1] = '\0'; }
    }
}

/* ============================================================================
 * Graphics Context
 * ============================================================================ */

GC XCreateGC(Display* dpy, Drawable d, uint32_t valuemask, XGCValues* values) {
    (void)d;
    if (!dpy) return None;
    int slot = -1;
    for (int i = 0; i < X11_MAX_GCS; i++)
        if (!x11_gcs[i].active) { slot = i; break; }
    if (slot < 0) return None;

    x11_gc_t* gc = &x11_gcs[slot];
    gc->active = true;
    gc->id = alloc_id();
    gc->values.foreground = 0xFFFFFF;
    gc->values.background = 0x000000;
    gc->values.line_width = 1;
    gc->values.line_style = LineSolid;
    gc->values.cap_style = CapButt;
    gc->values.join_style = JoinMiter;
    gc->values.fill_style = FillSolid;
    gc->values.font_id = 0;

    if (values) {
        if (valuemask & GCForeground) gc->values.foreground = values->foreground;
        if (valuemask & GCBackground) gc->values.background = values->background;
        if (valuemask & GCLineWidth)  gc->values.line_width = values->line_width;
    }
    return gc->id;
}

void XFreeGC(Display* dpy, GC gc) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    if (g) g->active = false;
}

void XSetForeground(Display* dpy, GC gc, uint32_t pixel) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    if (g) g->values.foreground = pixel;
}

void XSetBackground(Display* dpy, GC gc, uint32_t pixel) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    if (g) g->values.background = pixel;
}

void XSetLineAttributes(Display* dpy, GC gc, int line_width,
                        int line_style, int cap_style, int join_style) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    if (g) {
        g->values.line_width = line_width;
        g->values.line_style = line_style;
        g->values.cap_style = cap_style;
        g->values.join_style = join_style;
    }
}

/* ============================================================================
 * Drawing Operations
 * ============================================================================ */

void XDrawPoint(Display* dpy, Drawable d, GC gc, int x, int y) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    x11_window_t* xw = find_xwin(d);
    if (!g || !xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy))
        fb_putpixel(ox + x, oy + y, g->values.foreground);
}

void XDrawLine(Display* dpy, Drawable d, GC gc, int x1, int y1, int x2, int y2) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    x11_window_t* xw = find_xwin(d);
    if (!g || !xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy))
        gfx_draw_line(ox + x1, oy + y1, ox + x2, oy + y2, g->values.foreground);
}

void XDrawRectangle(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    x11_window_t* xw = find_xwin(d);
    if (!g || !xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy))
        gfx_draw_rect(ox + x, oy + y, w, h, g->values.foreground);
}

void XFillRectangle(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h) {
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    x11_window_t* xw = find_xwin(d);
    if (!g || !xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy))
        gfx_fill_rect(ox + x, oy + y, w, h, g->values.foreground);
}

void XDrawArc(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h,
              int angle1, int angle2) {
    (void)angle1; (void)angle2;
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    x11_window_t* xw = find_xwin(d);
    if (!g || !xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy)) {
        int cx = ox + x + w / 2;
        int cy = oy + y + h / 2;
        int r = (w < h ? w : h) / 2;
        gfx_draw_circle(cx, cy, r, g->values.foreground);
    }
}

void XFillArc(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h,
              int angle1, int angle2) {
    (void)angle1; (void)angle2;
    if (!dpy) return;
    x11_gc_t* g = find_gc(gc);
    x11_window_t* xw = find_xwin(d);
    if (!g || !xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy)) {
        int cx = ox + x + w / 2;
        int cy = oy + y + h / 2;
        int r = (w < h ? w : h) / 2;
        gfx_fill_circle(cx, cy, r, g->values.foreground);
    }
}

void XDrawString(Display* dpy, Drawable d, GC gc, int x, int y,
                 const char* str, int len) {
    (void)len;
    if (!dpy || !str) return;
    x11_gc_t* g = find_gc(gc);
    x11_window_t* xw = find_xwin(d);
    if (!g || !xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy))
        font_draw_string(ox + x, oy + y, str, g->values.foreground, g->values.background);
}

void XClearArea(Display* dpy, Window win, int x, int y, int w, int h, Bool exposures) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (!xw) return;
    int ox, oy;
    if (get_content_origin(xw, &ox, &oy)) {
        int cw = (w == 0) ? xw->width : w;
        int ch = (h == 0) ? xw->height : h;
        gfx_fill_rect(ox + x, oy + y, cw, ch, xw->background);
    }
    if (exposures && (xw->event_mask & ExposureMask)) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = Expose;
        ev.xexpose.window = win;
        ev.xexpose.x = x; ev.xexpose.y = y;
        ev.xexpose.width = w; ev.xexpose.height = h;
        push_event(&ev);
    }
}

/* ============================================================================
 * Events
 * ============================================================================ */

void XSelectInput(Display* dpy, Window win, uint32_t event_mask) {
    if (!dpy) return;
    x11_window_t* xw = find_xwin(win);
    if (xw) xw->event_mask = event_mask;
}

int XPending(Display* dpy) {
    if (!dpy) return 0;
    int count = x11_eq_head - x11_eq_tail;
    if (count < 0) count += X11_MAX_EVENTS;
    return count;
}

int XNextEvent(Display* dpy, XEvent* event_return) {
    if (!dpy || !event_return) return -1;
    /* Block until event available */
    while (x11_eq_head == x11_eq_tail) {
        x11_pump_events();
        __asm__ volatile("hlt");
    }
    *event_return = x11_event_queue[x11_eq_tail];
    x11_eq_tail = (x11_eq_tail + 1) % X11_MAX_EVENTS;
    return 0;
}

Bool XCheckMaskEvent(Display* dpy, uint32_t event_mask, XEvent* event_return) {
    if (!dpy || !event_return) return False;
    int i = x11_eq_tail;
    while (i != x11_eq_head) {
        XEvent* ev = &x11_event_queue[i];
        bool match = false;
        switch (ev->type) {
            case KeyPress:   match = (event_mask & KeyPressMask) != 0; break;
            case KeyRelease: match = (event_mask & KeyReleaseMask) != 0; break;
            case ButtonPress: match = (event_mask & ButtonPressMask) != 0; break;
            case Expose:     match = (event_mask & ExposureMask) != 0; break;
            default: break;
        }
        if (match) {
            *event_return = *ev;
            /* Remove from queue by shifting */
            int j = i;
            while (j != x11_eq_head) {
                int next = (j + 1) % X11_MAX_EVENTS;
                x11_event_queue[j] = x11_event_queue[next];
                j = next;
            }
            x11_eq_head = (x11_eq_head - 1 + X11_MAX_EVENTS) % X11_MAX_EVENTS;
            return True;
        }
        i = (i + 1) % X11_MAX_EVENTS;
    }
    return False;
}

/* ============================================================================
 * Atoms & Properties
 * ============================================================================ */

Atom XInternAtom(Display* dpy, const char* atom_name, Bool only_if_exists) {
    if (!dpy || !atom_name) return None;
    /* Check existing */
    for (int i = 0; i < x11_atom_count; i++) {
        if (x11_atoms[i].active && strcmp(x11_atoms[i].name, atom_name) == 0)
            return x11_atoms[i].id;
    }
    if (only_if_exists) return None;
    if (x11_atom_count >= X11_MAX_ATOMS) return None;

    x11_atom_t* a = &x11_atoms[x11_atom_count++];
    a->active = true;
    a->id = alloc_id();
    strncpy(a->name, atom_name, 31);
    a->name[31] = '\0';
    return a->id;
}

char* XGetAtomName(Display* dpy, Atom atom) {
    if (!dpy) return NULL;
    for (int i = 0; i < x11_atom_count; i++) {
        if (x11_atoms[i].active && x11_atoms[i].id == atom)
            return x11_atoms[i].name;
    }
    return NULL;
}

/* ============================================================================
 * Misc
 * ============================================================================ */

void XFlush(Display* dpy) {
    (void)dpy;
    if (fb_is_vesa()) fb_flip();
}

void XSync(Display* dpy, Bool discard) {
    XFlush(dpy);
    if (discard) { x11_eq_head = 0; x11_eq_tail = 0; }
}

void XBell(Display* dpy, int percent) {
    (void)dpy;
    int freq = 800 + percent * 4;
    beep((uint32_t)freq, 2);
}

int XConnectionNumber(Display* dpy) { (void)dpy; return 0; }

/* ============================================================================
 * NexusOS Extensions
 * ============================================================================ */

void x11_init(void) {
    memset(x11_windows, 0, sizeof(x11_windows));
    memset(x11_gcs, 0, sizeof(x11_gcs));
    memset(x11_atoms, 0, sizeof(x11_atoms));
    memset(&x11_display, 0, sizeof(x11_display));
    x11_eq_head = 0;
    x11_eq_tail = 0;
    x11_next_id = 100;
    x11_atom_count = 0;
    x11_initialized = true;

    /* Pre-register standard atoms */
    XInternAtom(&x11_display, "WM_PROTOCOLS", False);
    XInternAtom(&x11_display, "WM_DELETE_WINDOW", False);
    XInternAtom(&x11_display, "WM_NAME", False);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("X11 shim initialized (Phase 33)\n");
}

int x11_get_window_count(void) {
    int c = 0;
    for (int i = 0; i < X11_MAX_WINDOWS; i++) if (x11_windows[i].active) c++;
    return c;
}

int x11_get_gc_count(void) {
    int c = 0;
    for (int i = 0; i < X11_MAX_GCS; i++) if (x11_gcs[i].active) c++;
    return c;
}

void x11_pump_events(void) {
    /* This is called from the main loop to inject mouse/keyboard events
     * into the X11 event queue for any active X11 windows. */
    /* Currently, keyboard events are handled via window_send_key callback
     * which calls x11_win_key. Mouse events could be added here later. */
}

static char x11_status_buf[80];
const char* x11_get_status(void) {
    char n1[12], n2[12], n3[12];
    strcpy(x11_status_buf, "X11: ");
    int_to_str(x11_get_window_count(), n1);
    strcat(x11_status_buf, n1);
    strcat(x11_status_buf, " wins, ");
    int_to_str(x11_get_gc_count(), n2);
    strcat(x11_status_buf, n2);
    strcat(x11_status_buf, " GCs, ");
    int_to_str(x11_atom_count, n3);
    strcat(x11_status_buf, n3);
    strcat(x11_status_buf, " atoms");
    return x11_status_buf;
}
