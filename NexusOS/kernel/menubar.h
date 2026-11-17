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
#ifdef NEXUS_AURORA
/* Lay out the bar titles for app_name without drawing (menu geometry follows it). */
void menubar_layout(const char* app_name);
/* Conservative rectangle menubar_draw_menu() touches; false when no menu is open. */
bool menubar_menu_bounds(int* x, int* y, int* w, int* h);
/* Power status item (aurora_power.c): percent < 0 = no battery; state 1 charging, 2 discharging, 3 charged;
 * brightness = a backlight exists (shown as a sun when there is no battery). Click: UI_ACT_BATTERY. */
void menubar_set_power(int percent, int state, bool brightness);
int  menubar_power_x(void);   /* left edge of the item (anchors its panel) */
/* Status items left of the search icon (right to left in registration order); a click returns action. */
typedef struct { int (*width)(void); void (*draw)(int x, int cy); int action; } menubar_status_t;
bool menubar_status_add(const menubar_status_t* item);
/* Keyboard access and the screen reader's view of the bar (aurora_a11y.c). */
void menubar_open_first(void);                 /* open the first menu, first item highlighted */
int menubar_a11y_menus(void);
const char* menubar_a11y_title(int m);
int menubar_a11y_items(int m);
const char* menubar_a11y_item(int m, int i, bool* enabled);   /* NULL: separator */
int menubar_a11y_state(int* item);             /* open menu (-1 none), highlighted item in *item */
#endif

#endif /* MENUBAR_H_INCLUDED */
