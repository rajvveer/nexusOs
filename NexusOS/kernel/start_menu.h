/* ============================================================================
 * NexusOS — Start Menu (Header)
 * ============================================================================
 * Two-column popup menu with all NexusOS apps.
 * ============================================================================ */

#ifndef START_MENU_H
#define START_MENU_H

#include "types.h"

/* Menu dimensions — two-column layout */
#define SMENU_WIDTH   40
#define SMENU_HEIGHT  20
#define SMENU_X       1
#define SMENU_Y       (24 - SMENU_HEIGHT)  /* Above taskbar (row 24) */

/* Menu actions (returned by start_menu_handle_*) */
#define SMENU_NONE      0
#define SMENU_TERMINAL  1
#define SMENU_FILEMGR   2
#define SMENU_CALC      3
#define SMENU_SYSMON    4
#define SMENU_ABOUT     5
#define SMENU_SNAKE     6
#define SMENU_SETTINGS  7
#define SMENU_NOTEPAD   10
#define SMENU_TASKMGR   11
#define SMENU_CALENDAR  12
#define SMENU_MUSIC     13
#define SMENU_HELP      14
#define SMENU_PAINT     15
#define SMENU_MINESWEEP 16
#define SMENU_RECYCLE   17
#define SMENU_SYSINFO   18
#define SMENU_TODO      19
#define SMENU_CLOCK     20
#define SMENU_PONG      21
#define SMENU_SEARCH    22
#define SMENU_TETRIS    23
#define SMENU_HEXVIEW   24
#define SMENU_CONTACTS  25
#define SMENU_COLORPICK 26
#define SMENU_SYSLOG    27
#define SMENU_CLIPMGR   28
#define SMENU_APPEAR    29
#define SMENU_POWER     30
#define SMENU_FILEOPS   31
#define SMENU_BROWSER   32

/* Toggle the start menu open/closed */
void start_menu_toggle(void);

/* Close the menu */
void start_menu_close(void);

/* Is the menu currently open? */
bool start_menu_is_open(void);

/* Draw the start menu overlay to the GUI buffer */
void start_menu_draw(void);

/* Handle a keyboard event: returns SMENU_* action, or SMENU_NONE */
int  start_menu_handle_key(char key);

/* Handle a mouse click: returns SMENU_* action, or SMENU_NONE */
int  start_menu_handle_click(int mx, int my);

/* Check if a click is inside the start menu area */
bool start_menu_hit(int mx, int my);

#endif /* START_MENU_H */
