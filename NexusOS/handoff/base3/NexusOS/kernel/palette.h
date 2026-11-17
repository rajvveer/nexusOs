/* ============================================================================
 * NexusOS — Command Palette (Header)
 * ============================================================================ */

#ifndef PALETTE_H
#define PALETTE_H

#include "types.h"

/* Palette actions (same IDs as start menu + extras) */
#define PAL_TERMINAL   1
#define PAL_FILEMGR    2
#define PAL_CALC       3
#define PAL_SYSMON     4
#define PAL_SETTINGS   5
#define PAL_SNAKE      6
#define PAL_ABOUT      7
#define PAL_LOCK       8
#define PAL_THEME      9
#define PAL_NOTEPAD   10
#define PAL_TASKMGR   11
#define PAL_CALENDAR  12

void palette_open(void);
void palette_close(void);
bool palette_is_open(void);
void palette_draw(void);
int  palette_handle_key(char key);

#endif /* PALETTE_H */
