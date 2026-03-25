/* NexusOS — Shortcut Overlay (Header) */
#ifndef SHORTCUTS_H
#define SHORTCUTS_H
#include "types.h"
void shortcuts_open(void);
void shortcuts_close(void);
bool shortcuts_is_open(void);
void shortcuts_draw(void);
int  shortcuts_handle_key(char key);
#endif
