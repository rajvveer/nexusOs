/* ============================================================================
 * NexusOS — Menu bar (Header)
 * ============================================================================
 * macOS-style translucent bar across the top of the screen: the NexusOS menu,
 * the active app's name and its menus on the left, status items and the clock
 * on the right. Menu items return UI_ACT_* / desktop action ids.
 * ============================================================================ */

#ifndef MENUBAR_H_INCLUDED
#define MENUBAR_H_INCLUDED

#include "types.h"

/* Draw the bar. app_name is the focused app ("Finder" when none). */
void menubar_draw(const char* app_name);
/* Draw the open dropdown (call late so it sits above windows). */
void menubar_draw_menu(void);

bool menubar_hit(int px, int py);               /* point is on the bar */
int  menubar_click(int px, int py);             /* click on the bar -> action or 0 */
bool menubar_menu_open(void);
bool menubar_menu_hit(int px, int py);          /* point is inside the dropdown */
int  menubar_menu_click(int px, int py);        /* click in dropdown -> action or 0 */
bool menubar_hover(int px, int py);             /* true if the highlight changed */
int  menubar_handle_key(char key);              /* keyboard nav of open menu */
void menubar_close(void);

#endif /* MENUBAR_H_INCLUDED */
