/* ============================================================================
 * NexusOS — Wallpaper Patterns (Header)
 * ============================================================================ */

#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "types.h"

/* Available wallpaper patterns */
#define WP_DOTS      0
#define WP_GRID      1
#define WP_DIAMONDS  2
#define WP_GRADIENT  3
#define WP_IMAGE     4
#define WP_PHOTO     5    /* Phase 14: Pixel wallpaper (VESA only) */
#define WP_COUNT     6

/* Set the wallpaper pattern */
void wallpaper_set(int pattern);

/* Get the current pattern index */
int  wallpaper_get(void);

/* Get pattern name */
const char* wallpaper_get_name(int pattern);

/* Draw wallpaper to the GUI back buffer (rows 0 to 23) */
void wallpaper_draw(void);

#endif /* WALLPAPER_H */
