/* ============================================================================
 * NexusOS — Right-Click Context Menu (Header)
 * ============================================================================
 * Popup context menu appearing on right-click on the desktop.
 * ============================================================================ */

#ifndef CONTEXT_MENU_H
#define CONTEXT_MENU_H

#include "types.h"

/* Context menu actions */
#define CMENU_NONE       0
#define CMENU_NEW_FILE   1
#define CMENU_REFRESH    2
#define CMENU_THEME_NEXT 3
#define CMENU_ABOUT      4
#define CMENU_SETTINGS   5

/* Open the context menu at position (mx, my) */
void context_menu_open(int mx, int my);

/* Close the context menu */
void context_menu_close(void);

/* Is the context menu open? */
bool context_menu_is_open(void);

/* Draw the context menu overlay */
void context_menu_draw(void);

/* Handle key input. Returns action or CMENU_NONE */
int  context_menu_handle_key(char key);

/* Handle mouse click. Returns action or CMENU_NONE */
int  context_menu_handle_click(int mx, int my);

/* Check if click is inside the menu */
bool context_menu_hit(int mx, int my);

#endif /* CONTEXT_MENU_H */
