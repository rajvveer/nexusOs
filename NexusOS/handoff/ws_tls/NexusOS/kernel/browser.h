/* ============================================================================
 * NexusOS — Text Web Browser Header — Phase 29
 * ============================================================================
 * Lynx-style text browser with link navigation, history, bookmarks.
 * Renders inside a GUI window (not VGA text mode).
 * ============================================================================ */

#ifndef BROWSER_H
#define BROWSER_H

#include "types.h"
#include "html.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define BROWSER_HISTORY_SIZE    8
#define BROWSER_BOOKMARK_SIZE   8
#define BROWSER_URL_MAX         128

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize browser */
void browser_init(void);

/* Open browser with URL (windowed GUI app) */
void browser_open(const char* url);

#endif /* BROWSER_H */
