/* Aurora keyboard layouts (aurora_keyboard.c): the input sources (xkb layouts, up to 4) of the session, persisted in
 * AURORA_KEYBOARD_CONF and set from System Settings > Keyboard through the control socket ("keyboard", "set-keyboard
 * layouts=us,de variants=,"). The compositor compiles one keymap with all of them (rules evdev, model pc105) and
 * clients follow the active one through wl_keyboard.modifiers (the xkb group): Super+Space and the input menu in the
 * menu bar switch to the next. */
#ifndef AURORA_KEYBOARD_H
#define AURORA_KEYBOARD_H
#include <stdbool.h>

#define AURORA_KEYBOARD_CONF "/home/nexus/.aurora/keyboard.conf"
#define AURORA_LAYOUTS_MAX 4
struct aurora_layout { const char* code; const char* variant; const char* label; const char* name; };
const struct aurora_layout* aurora_keyboard_catalog(int* count);

/* ---- services of desktop.c ---- */
bool aurora_host_keymap(const char* layouts, const char* variants);   /* compile and send to every client; false keeps the old */
void aurora_host_keyboard_group(unsigned group);                      /* make layout `group` the active one */

/* ---- aurora_keyboard.c ---- */
void aurora_keyboard_init(void);
void aurora_keyboard_next(void);
const char* aurora_keyboard_label(void);   /* short name of the active layout ("US", "DE", "HI"...) */
int aurora_keyboard_count(void);
#endif
