/* NexusOS — App Dock (Header) */
#ifndef DOCK_H
#define DOCK_H
#include "types.h"
void dock_init(void);
void dock_draw(void);
bool dock_hit(int x, int y);
int  dock_handle_click(int x);
#endif
