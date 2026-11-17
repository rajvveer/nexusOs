/* ============================================================================
 * NexusOS — Cocoa & Core Foundation Shim (Header) — Phase 37
 * ============================================================================
 * A small, C-callable subset of Apple's Core Foundation and Cocoa/AppKit APIs,
 * mapped onto NexusOS primitives — the macOS counterpart of the Phase 34 Win32
 * shim. Core Foundation objects are reference-counted (CFRetain/CFRelease) out
 * of a fixed pool; NSWindow is bridged to the NexusOS window manager.
 * ============================================================================ */

#ifndef COCOA_H
#define COCOA_H

#include "types.h"

/* ============================================================================
 * Core Foundation base types
 * ============================================================================ */
typedef const void*   CFTypeRef;
typedef CFTypeRef     CFStringRef;
typedef CFTypeRef     CFNumberRef;
typedef CFTypeRef     CFArrayRef;
typedef int           CFIndex;
typedef unsigned long CFTypeID;
typedef int           Boolean;

/* Object kinds (returned by CFGetTypeID). */
enum {
    CF_TYPE_NONE = 0,
    CF_TYPE_STRING,
    CF_TYPE_NUMBER,
    CF_TYPE_ARRAY,
    CF_TYPE_WINDOW
};

/* CFStringEncoding subset */
#define kCFStringEncodingASCII  0x0600
#define kCFStringEncodingUTF8   0x08000100

/* CFNumberType subset */
#define kCFNumberIntType        9

#define COCOA_STR_MAX 128

/* ============================================================================
 * Core Foundation API
 * ============================================================================ */
CFStringRef CFStringCreateWithCString(void* alloc, const char* cstr, uint32_t encoding);
const char* CFStringGetCStringPtr(CFStringRef s, uint32_t encoding);
CFIndex     CFStringGetLength(CFStringRef s);

CFNumberRef CFNumberCreate(void* alloc, int type, const void* valuePtr);
Boolean     CFNumberGetValue(CFNumberRef n, int type, void* out);

CFTypeID    CFGetTypeID(CFTypeRef obj);
CFTypeRef   CFRetain(CFTypeRef obj);
void        CFRelease(CFTypeRef obj);
CFIndex     CFGetRetainCount(CFTypeRef obj);
void        CFShow(CFTypeRef obj);

/* ============================================================================
 * Cocoa / AppKit / Foundation subset (simplified, C-callable)
 * ============================================================================ */
typedef CFTypeRef NSWindowRef;
typedef CFStringRef NSStringRef;

void        NSLog(const char* message);              /* prints message + newline */
NSStringRef NSMakeNSString(const char* cstr);        /* NSString (== CFString) */
NSWindowRef NSWindowCreate(const char* title, int x, int y, int w, int h);
void        NSWindowOrderFront(NSWindowRef win);
void        NSWindowClose(NSWindowRef win);
void        NSApplicationRun(void);                  /* marker; desktop loop drives UI */

/* ============================================================================
 * NexusOS subsystem management
 * ============================================================================ */
void        cocoa_init(void);
const char* cocoa_get_status(void);
int         cocoa_object_count(void);                /* live CF objects */
int         cocoa_demo(void);                        /* create a Cocoa demo window */

#endif /* COCOA_H */
