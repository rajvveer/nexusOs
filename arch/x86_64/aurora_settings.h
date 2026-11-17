/* System Settings (aurora-app settings [PAGE]): a sidebar of pages and the selected page's content.
 *
 * Adding a page: write it in its own file (aurora_settings_<name>.c) as a `const struct settings_page`, then add one
 * line to the page table in aurora_settings.c (and the file to aurora.py's Aurora app sources). Pages talk to the
 * compositor through settings_request() (the control socket, aurora_control.h) or to the system through their own
 * files. Coordinates handed to a page are relative to its content rectangle (right of the sidebar). */
#ifndef AURORA_SETTINGS_H
#define AURORA_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>

struct settings_page {
    const char* id;                              /* command-line name: aurora-app settings <id> */
    const char* name;                            /* sidebar label */
    int icon;                                    /* macui ICON_* */
    void (*enter)(void);                         /* the page is shown (refresh its state) */
    void (*draw)(int x, int y, int w, int h);    /* absolute pixel rectangle of the content */
    void (*click)(int x, int y);                 /* button press */
    void (*drag)(int x, int y, int phase);       /* 0 press, 1 motion with the button held, 2 release; may be NULL */
    void (*key)(char key);                       /* may be NULL */
    bool (*tick)(void);                          /* about every 50 ms; true: redraw; may be NULL */
};
/* Send request (one or more "\n"-terminated lines) on the control socket; true on "OK". reply receives the reply
 * text before the status line (or the error reason). */
bool settings_request(const char* request, char* reply, size_t size);
void settings_message(const char* text);         /* status line at the bottom of the window */
void settings_redraw(void);

extern const struct settings_page settings_displays;
extern const struct settings_page settings_users;
extern const struct settings_page settings_keyboard;
extern const struct settings_page settings_a11y;
extern const struct settings_page settings_sound;
extern const struct settings_page settings_upgrade;
extern const struct settings_page settings_appearance;
#endif
