/* aurora-app's accessible tree for the screen reader (aurora_a11y_client.c). Around each frame the appui kit reports
 * what it draws (texts, buttons, fields, toggles, rows, segments; kernel/appui.h appui_a11y_note); the frame becomes
 * a tree (a frame named by the window title, labels and headings, push buttons, entries - password fields without
 * their text -, check boxes, list items, radio buttons; names from the text next to a control or inside a row; the
 * focused element: the field with the caret, else the selected row) and goes to aurora-a11y-bridge on
 * $XDG_RUNTIME_DIR/aurora-a11y-apps (SOCK_SEQPACKET, one record per change), which exports it over AT-SPI2:
 *   "app\tPID\tAPP\tTITLE\tACTIVE\n" "focus PATH\n" "node\tPATH\tPARENT\tROLE\tSTATES\tNAME\tDESCRIPTION\tTEXT\n"...
 * Nothing is sent while no screen reader runs (the socket does not exist); aurora_a11y_tick() (the main loop) asks
 * for a frame when the bridge appears, so an idle window is announced too. */
#ifndef AURORA_A11Y_CLIENT_H
#define AURORA_A11Y_CLIENT_H
#include <stdbool.h>
void aurora_a11y_paint_begin(void);
void aurora_a11y_paint_end(const char* app, const char* title, bool active);
bool aurora_a11y_tick(void);   /* true: draw a frame (a screen reader just appeared) */
#endif
