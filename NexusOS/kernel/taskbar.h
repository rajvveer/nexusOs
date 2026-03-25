/* ============================================================================
 * NexusOS — Taskbar (Header)
 * ============================================================================
 * Bottom row of the screen. Shows [N] start button, open windows, and clock.
 * Phase 5: Start button triggers the Start Menu.
 * ============================================================================ */

#ifndef TASKBAR_H
#define TASKBAR_H

#include "types.h"
#include "gui.h"

/* Taskbar occupies the bottom row of the GUI grid */
#define TASKBAR_ROW  (GUI_HEIGHT - 1)

/* Draw the taskbar to the GUI back buffer */
void taskbar_draw(void);

/* Check if a click at (mx, my) is on the taskbar */
bool taskbar_hit(int mx, int my);

/* Handle a click on the taskbar at column mx. Returns window ID to focus, or -1 */
int taskbar_handle_click(int mx);

/* Check if the click is on the Start (NexusOS) button */
bool taskbar_start_hit(int mx);

/* System tray hit: 1=bell, 2=clipboard, 3=lock, 0=none */
int  taskbar_tray_hit(int mx);

#endif /* TASKBAR_H */
