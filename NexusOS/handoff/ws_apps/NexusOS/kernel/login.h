/* ============================================================================
 * NexusOS — Login Screen (Header)
 * ============================================================================
 * Animated boot splash and login prompt. Called before shell/desktop.
 * ============================================================================ */

#ifndef LOGIN_H
#define LOGIN_H

#include "types.h"

/* Run the login screen. Loops until valid credentials are entered (Phase 44),
 * sets the current user, and returns the authenticated uid. */
uint32_t login_run(void);

/* VESA lock screen: blocks until the current user's password is entered. */
void login_lock_screen(void);

#endif /* LOGIN_H */
