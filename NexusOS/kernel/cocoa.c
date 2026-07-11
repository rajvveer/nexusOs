/* ============================================================================
 * NexusOS — Cocoa & Core Foundation Shim (Implementation) — Phase 37
 * ============================================================================
 * Core Foundation objects (CFString, CFNumber, NSWindow) live in a fixed pool
 * and are reference-counted exactly like the real CF — CFRetain bumps the
 * count, CFRelease drops it, and an NSWindow whose count hits zero tears down
 * its NexusOS window. NSWindow is bridged to the window manager so Cocoa code
 * draws real windows on the desktop. The macOS analogue of win32.c.
 * ============================================================================ */

#include "cocoa.h"
#include "vga.h"
#include "string.h"
#include "window.h"
#include "appui.h"

#define CF_MAX_OBJECTS 32

typedef struct {
    bool    active;
    uint8_t type;        /* CF_TYPE_*                         */
    int     refcount;
    char    str[COCOA_STR_MAX];  /* string value / window title */
    int     num;                 /* number value               */
    int     win_id;              /* NexusOS WM id (-1 if none)  */
} cf_object_t;

static cf_object_t cf_pool[CF_MAX_OBJECTS];

/* --------------------------------------------------------------------------
 * Pool helpers
 * -------------------------------------------------------------------------- */
static cf_object_t* cf_alloc(uint8_t type) {
    for (int i = 0; i < CF_MAX_OBJECTS; i++) {
        if (!cf_pool[i].active) {
            cf_object_t* o = &cf_pool[i];
            memset(o, 0, sizeof(*o));
            o->active = true;
            o->type = type;
            o->refcount = 1;
            o->win_id = -1;
            return o;
        }
    }
    return NULL;
}

/* Validate a CFTypeRef points at a live object inside our pool. */
static cf_object_t* cf_obj(CFTypeRef ref) {
    if (!ref) return NULL;
    cf_object_t* o = (cf_object_t*)ref;
    if (o < cf_pool || o >= cf_pool + CF_MAX_OBJECTS) return NULL;
    return o->active ? o : NULL;
}

/* --------------------------------------------------------------------------
 * Core Foundation — strings
 * -------------------------------------------------------------------------- */
CFStringRef CFStringCreateWithCString(void* alloc, const char* cstr, uint32_t encoding) {
    (void)alloc; (void)encoding;
    cf_object_t* o = cf_alloc(CF_TYPE_STRING);
    if (!o) return NULL;
    if (cstr) { strncpy(o->str, cstr, COCOA_STR_MAX - 1); o->str[COCOA_STR_MAX - 1] = '\0'; }
    return (CFStringRef)o;
}

const char* CFStringGetCStringPtr(CFStringRef s, uint32_t encoding) {
    (void)encoding;
    cf_object_t* o = cf_obj(s);
    return (o && o->type == CF_TYPE_STRING) ? o->str : NULL;
}

CFIndex CFStringGetLength(CFStringRef s) {
    cf_object_t* o = cf_obj(s);
    return (o && o->type == CF_TYPE_STRING) ? (CFIndex)strlen(o->str) : 0;
}

/* --------------------------------------------------------------------------
 * Core Foundation — numbers
 * -------------------------------------------------------------------------- */
CFNumberRef CFNumberCreate(void* alloc, int type, const void* valuePtr) {
    (void)alloc; (void)type;
    cf_object_t* o = cf_alloc(CF_TYPE_NUMBER);
    if (!o) return NULL;
    if (valuePtr) o->num = *(const int*)valuePtr;
    return (CFNumberRef)o;
}

Boolean CFNumberGetValue(CFNumberRef n, int type, void* out) {
    (void)type;
    cf_object_t* o = cf_obj(n);
    if (!o || o->type != CF_TYPE_NUMBER || !out) return 0;
    *(int*)out = o->num;
    return 1;
}

/* --------------------------------------------------------------------------
 * Core Foundation — generic object protocol
 * -------------------------------------------------------------------------- */
CFTypeID CFGetTypeID(CFTypeRef obj) {
    cf_object_t* o = cf_obj(obj);
    return o ? (CFTypeID)o->type : (CFTypeID)CF_TYPE_NONE;
}

CFTypeRef CFRetain(CFTypeRef obj) {
    cf_object_t* o = cf_obj(obj);
    if (o) o->refcount++;
    return obj;
}

void CFRelease(CFTypeRef obj) {
    cf_object_t* o = cf_obj(obj);
    if (!o) return;
    if (--o->refcount <= 0) {
        if (o->type == CF_TYPE_WINDOW && o->win_id >= 0) window_destroy(o->win_id);
        o->active = false;
    }
}

CFIndex CFGetRetainCount(CFTypeRef obj) {
    cf_object_t* o = cf_obj(obj);
    return o ? (CFIndex)o->refcount : 0;
}

void CFShow(CFTypeRef obj) {
    cf_object_t* o = cf_obj(obj);
    vga_print("  ");
    if (!o) { vga_print("(null)\n"); return; }
    switch (o->type) {
        case CF_TYPE_STRING:
            vga_print_color("CFString ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
            vga_print("\""); vga_print(o->str); vga_print("\"\n");
            break;
        case CF_TYPE_NUMBER: {
            vga_print_color("CFNumber ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
            char b[12]; int_to_str(o->num, b); vga_print(b); vga_print("\n");
            break;
        }
        case CF_TYPE_WINDOW:
            vga_print_color("NSWindow ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
            vga_print("\""); vga_print(o->str); vga_print("\"\n");
            break;
        default:
            vga_print("CFType <unknown>\n");
            break;
    }
}

/* --------------------------------------------------------------------------
 * Cocoa / AppKit — NSWindow bridged to the NexusOS window manager
 * -------------------------------------------------------------------------- */
static void cocoa_win_draw(int id, int cx, int cy, int cw, int ch) {
    const char* title = "Cocoa";
    for (int i = 0; i < CF_MAX_OBJECTS; i++)
        if (cf_pool[i].active && cf_pool[i].type == CF_TYPE_WINDOW && cf_pool[i].win_id == id) {
            title = cf_pool[i].str; break;
        }

    appui_theme_t ui = appui_theme();
    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, title, "AppKit on NexusOS");

    int y = cy + 3;
    appui_text(cx + 1, y++, "\xFB NSWindow bridged to the WM",   cw - 2, ui.text);
    appui_text(cx + 1, y++, "\xFB CoreFoundation: reference-counted", cw - 2, ui.text);
    appui_text(cx + 1, y++, "\xFB CFString / CFNumber / NSWindow", cw - 2, ui.accent);
    y++;
    appui_text(cx + 1, y++, "NSLog(\"Hello from Cocoa!\")", cw - 2, ui.good);
}

static void cocoa_win_key(int id, char key) { (void)id; (void)key; }

NSWindowRef NSWindowCreate(const char* title, int x, int y, int w, int h) {
    cf_object_t* o = cf_alloc(CF_TYPE_WINDOW);
    if (!o) return NULL;
    if (title) { strncpy(o->str, title, COCOA_STR_MAX - 1); o->str[COCOA_STR_MAX - 1] = '\0'; }
    int id = window_create(o->str, x, y, w, h, cocoa_win_draw, cocoa_win_key);
    o->win_id = id;
    return (NSWindowRef)o;
}

void NSWindowOrderFront(NSWindowRef win) {
    cf_object_t* o = cf_obj(win);
    if (o && o->type == CF_TYPE_WINDOW && o->win_id >= 0) window_focus(o->win_id);
}

void NSWindowClose(NSWindowRef win) {
    cf_object_t* o = cf_obj(win);
    if (o && o->type == CF_TYPE_WINDOW && o->win_id >= 0) {
        window_destroy(o->win_id);
        o->win_id = -1;
    }
}

NSStringRef NSMakeNSString(const char* cstr) {
    return CFStringCreateWithCString(NULL, cstr, kCFStringEncodingUTF8);
}

void NSLog(const char* message) {
    vga_print_color("  [NSLog] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print(message ? message : "(null)");
    vga_print("\n");
}

void NSApplicationRun(void) {
    /* The desktop compositor is the run loop; nothing to spin here. */
}

/* --------------------------------------------------------------------------
 * Subsystem
 * -------------------------------------------------------------------------- */
int cocoa_object_count(void) {
    int c = 0;
    for (int i = 0; i < CF_MAX_OBJECTS; i++) if (cf_pool[i].active) c++;
    return c;
}

void cocoa_init(void) {
    memset(cf_pool, 0, sizeof(cf_pool));
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Cocoa shim initialized (Core Foundation + AppKit)\n");
}

const char* cocoa_get_status(void) {
    return "Cocoa: CoreFoundation (CFString/CFNumber, refcounted) + AppKit (NSWindow)";
}

/* Build a few CF objects and a live NSWindow — invoked by `cocoademo`. */
int cocoa_demo(void) {
    CFStringRef s = CFStringCreateWithCString(NULL, "Hello from Cocoa!", kCFStringEncodingUTF8);
    CFShow(s);

    int v = 42;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFShow(n);

    NSLog("NSApplicationMain: did finish launching");

    NSWindowRef w = NSWindowCreate("Cocoa Demo", 36, 6, 40, 14);
    if (!w) { CFRelease(s); CFRelease(n); return -1; }
    NSWindowOrderFront(w);
    CFShow(w);

    /* The window keeps its own reference (it stays on the desktop); release
     * the transient string and number. */
    CFRelease(s);
    CFRelease(n);
    return 0;
}
