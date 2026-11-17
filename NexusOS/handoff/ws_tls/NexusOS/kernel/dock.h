/* NexusOS — Dock (Header)
 * macOS-style dock: frosted shelf centred at the bottom, icons that magnify
 * under the pointer, a name bubble, running-app dots and the Trash. */
#ifndef DOCK_H
#define DOCK_H
#include "types.h"

void dock_init(void);
/* Draw the dock; (mx,my) is the pointer for magnification/tooltips. */
void dock_draw(int mx, int my);
bool dock_hit(int px, int py);
/* Click at (px,py): returns the app's action id, or 0. */
int  dock_click(int px, int py);
/* True when pointer motion changes the dock (magnify/tooltip) -> redraw. */
bool dock_hover_changed(int px, int py);
/* Top edge of the dock shelf in pixels. */
int  dock_top(void);

#endif
