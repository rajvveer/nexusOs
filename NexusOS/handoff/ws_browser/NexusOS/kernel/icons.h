/* ============================================================================
 * NexusOS — Desktop Icons (Header)
 * ============================================================================
 * Clickable app shortcuts on the desktop wallpaper.
 * ============================================================================ */

#ifndef ICONS_H
#define ICONS_H

#include "types.h"

/* Icon actions (matches start_menu actions) */
#define ICON_TERMINAL  1
#define ICON_FILEMGR   2
#define ICON_CALC      3
#define ICON_SYSMON    4
#define ICON_SETTINGS  5
#define ICON_SNAKE     6

/* Initialize desktop icons */
void icons_init(void);

/* Draw all desktop icons to the GUI buffer */
void icons_draw(void);

/* Handle keyboard: Up/Down to select, Enter to launch. Returns action or 0 */
int  icons_handle_key(char key);

/* Handle mouse click on desktop area. Returns action or 0 */
int  icons_handle_click(int mx, int my);

/* Get the currently selected icon index */
int  icons_get_selected(void);

#endif /* ICONS_H */
