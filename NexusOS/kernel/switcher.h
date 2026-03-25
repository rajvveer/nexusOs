/* ============================================================================
 * NexusOS — Alt+Tab Window Switcher (Header)
 * ============================================================================ */

#ifndef SWITCHER_H
#define SWITCHER_H

#include "types.h"

/* Open the switcher overlay */
void switcher_open(void);

/* Close the switcher and focus selected window */
void switcher_close(void);

/* Cancel the switcher (don't change focus) */
void switcher_cancel(void);

/* Move selection to next/previous window */
void switcher_next(void);
void switcher_prev(void);

/* Is switcher currently open? */
bool switcher_is_open(void);

/* Draw the switcher overlay */
void switcher_draw(void);

/* Get the currently selected window ID */
int  switcher_get_selected(void);

#endif /* SWITCHER_H */
