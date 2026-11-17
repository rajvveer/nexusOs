/* ============================================================================
 * NexusOS — Nexus Browser (Header) — Phase 57
 * ============================================================================
 * Graphical web browser for the Aurora desktop: toolbar, address bar,
 * bookmarks bar, progress/status, and a page viewport rendered by webengine.c.
 * All network I/O goes through netstream.h and is pumped from the desktop
 * loop via browser_poll(). (Replaces the Phase 29 Lynx-style text browser.)
 * ============================================================================ */

#ifndef BROWSER_H
#define BROWSER_H

#include "types.h"

/* Boot-time init (prints a status line). */
void browser_init(void);

/* Open the browser window (or focus it if open) and navigate to `url`
 * (NULL or "" = about:home). Must be called while the desktop is running. */
void browser_open(const char* url);

/* From the text console: remember a URL to open on the first desktop frame
 * (used by the shell's `browse <url>` before it launches the desktop). */
void browser_set_pending(const char* url);

/* Per-frame work: pumps network streams, decodes images, tracks link hover.
 * Call from the desktop main loop. Returns true if the screen needs a repaint. */
bool browser_poll(void);

/* True if `key` should be delivered to the browser window instead of being
 * handled as a desktop hotkey (Tab = link cycling, Ctrl+L = address bar). */
bool browser_claims_key(char key);

#endif /* BROWSER_H */
