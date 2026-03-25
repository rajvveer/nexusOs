/* ============================================================================
 * NexusOS — Theme Engine (Header)
 * ============================================================================
 * Switchable color schemes for the desktop environment.
 * Each theme defines colors for every UI element.
 * ============================================================================ */

#ifndef THEME_H
#define THEME_H

#include "types.h"

/* Theme color slots */
typedef struct {
    uint8_t desktop_bg;       /* Desktop background */
    uint8_t desktop_fg;       /* Desktop wallpaper pattern color */
    char    desktop_char;     /* Desktop fill character */

    uint8_t win_border;       /* Window border color */
    uint8_t win_title_active; /* Active window title bar */
    uint8_t win_title_inactive; /* Inactive window title bar */
    uint8_t win_content;      /* Window content area */
    uint8_t win_text;         /* Window text color (fg only) */

    uint8_t taskbar_bg;       /* Taskbar background */
    uint8_t taskbar_text;     /* Taskbar text */
    uint8_t taskbar_active;   /* Active window button in taskbar */
    uint8_t taskbar_clock;    /* Clock color */

    uint8_t menu_bg;          /* Menu background */
    uint8_t menu_highlight;   /* Menu selected item */

    uint8_t cursor_color;     /* Mouse cursor color */

    const char* name;         /* Theme name */
} theme_t;

/* Theme IDs */
#define THEME_NEXUS_DARK   0
#define THEME_NEXUS_LIGHT  1
#define THEME_RETRO        2
#define THEME_OCEAN        3
#define THEME_COUNT        4

/* Set theme by ID */
void theme_set(int id);

/* Set theme by name (returns false if not found) */
bool theme_set_by_name(const char* name);

/* Get current theme */
const theme_t* theme_get(void);

/* Cycle to next theme */
void theme_cycle(void);

/* Get theme name by ID */
const char* theme_get_name(int id);

/* Get current theme index */
int theme_get_index(void);

#endif /* THEME_H */
