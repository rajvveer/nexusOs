/* NexusOS — Workspaces (Header) */
#ifndef WORKSPACES_H
#define WORKSPACES_H
#include "types.h"
#define WS_COUNT 4
void workspaces_init(void);
void workspaces_switch(int ws);
int  workspaces_current(void);
void workspaces_assign_window(int id);
void workspaces_draw_indicator(int x, int y, uint8_t bg);
#endif
