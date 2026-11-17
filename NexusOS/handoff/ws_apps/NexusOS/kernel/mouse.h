/* ============================================================================
 * NexusOS — PS/2 Mouse Driver (Header)
 * ============================================================================
 * Provides IRQ12-based PS/2 mouse input with position tracking and button
 * state. Cursor position is in character-cell coordinates (0-79, 0-24).
 * ============================================================================ */

#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

/* Mouse button masks */
#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

/* Mouse state */
typedef struct {
    int      x;            /* Cursor column (0 to GUI_WIDTH-1) */
    int      y;            /* Cursor row (0 to GUI_HEIGHT-1) */
    int      px;           /* Cursor actual pixel X (0 to 1023) */
    int      py;           /* Cursor actual pixel Y (0 to 767) */
    uint8_t  buttons;      /* Button state (MOUSE_LEFT | RIGHT | MIDDLE) */
    bool     moved;        /* True if cursor moved since last check */
    bool     clicked;      /* True if button pressed since last check */
} mouse_state_t;

/* Initialize PS/2 mouse (enables aux device, registers IRQ12) */
void mouse_init(void);

/* Get current mouse state */
mouse_state_t mouse_get_state(void);

/* Clear the moved/clicked flags after processing */
void mouse_clear_events(void);

/* Check if there are pending mouse events */
bool mouse_has_event(void);

/* Phase 45: inject an absolute pointer event (used by the VNC server to drive
 * the local cursor from a remote client). px/py are pixel coordinates
 * (0..1023, 0..767); buttons is a MOUSE_LEFT|RIGHT|MIDDLE mask. Updates the
 * shared mouse state exactly as a real PS/2 packet would, so the desktop and
 * GUI hit-testing see remote input transparently. */
void mouse_inject(int px, int py, uint8_t buttons);

#endif /* MOUSE_H */
