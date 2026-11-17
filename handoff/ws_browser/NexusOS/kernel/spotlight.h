/* NexusOS — Spotlight search and Launchpad (Header)
 * Spotlight: centred search field that finds apps and system actions.
 * Launchpad: full-screen grid of every app. Both return action ids. */
#ifndef SPOTLIGHT_H
#define SPOTLIGHT_H
#include "types.h"

void spotlight_open(void);
void spotlight_close(void);
bool spotlight_is_open(void);
void spotlight_draw(void);
int  spotlight_key(char key);              /* -> action id or 0 */
bool spotlight_hit(int px, int py);
int  spotlight_click(int px, int py);      /* -> action id or 0 */

void launchpad_open(void);
void launchpad_close(void);
bool launchpad_is_open(void);
void launchpad_draw(void);
int  launchpad_key(char key);              /* -> action id or 0 */
int  launchpad_click(int px, int py);      /* -> action id or 0 (closes on empty space) */

#endif
