/* NexusOS — Dock (Header)
 * macOS-style dock: frosted shelf centred at the bottom, icons that magnify
 * under the pointer, a name bubble, running-app dots and the Trash. */
#ifndef DOCK_H
#define DOCK_H
#include "types.h"
#include "macui.h"

void dock_init(void);
void dock_set_running_query(bool (*query)(const ui_app_t* app));
/* Draw the dock; (mx,my) is the pointer for magnification/tooltips. */
void dock_draw(int mx, int my);
bool dock_hit(int px, int py);
/* Click at (px,py): returns the app's action id, or 0. */
int  dock_click(int px, int py);
/* True when pointer motion changes the dock (magnify/tooltip) -> redraw. */
bool dock_hover_changed(int px, int py);
/* Top edge of the dock shelf in pixels. */
int  dock_top(void);
#define DOCK_EXTRA_ACTION 0x40000
#ifdef NEXUS_AURORA
void dock_set_magnify(int percent);   /* Aurora: hover growth of the icons in %, 0 for reduced motion */
#endif   /* dock_click() of extra item k returns DOCK_EXTRA_ACTION + k */
#ifdef NEXUS_AURORA
/* Conservative rectangle dock_draw(mx,my) may touch; false when there is no dock. */
bool dock_bounds(int mx, int my, int* x, int* y, int* w, int* h);
/* Items after the fixed dock icons (running apps without one, minimized windows), drawn by draw(k, x, y, size). */
void dock_set_extra(int (*count)(void), void (*draw)(int k, int x, int y, int size), const char* (*name)(int k), bool (*running)(int k));
/* Keyboard access (Ctrl+F3): key 0 focuses the first item, Left/Right/Home/End move, Enter/Space return the item's
 * action (as dock_click), Esc leaves. The screen reader's view (aurora_a11y.c): items in shelf order. */
int  dock_key(char key);
int  dock_a11y_focus(void);   /* -1: no keyboard focus */
int  dock_a11y_count(void);
const char* dock_a11y_name(int i, bool* running);
#endif

#endif
