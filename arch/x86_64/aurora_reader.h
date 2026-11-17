/* Screen reader support of the Aurora shell (aurora_reader.c).
 *
 * Keyboard access: Ctrl+F2 opens the menu bar (arrows, Enter, Esc), Ctrl+F3 moves the keyboard focus into the Dock
 * (Left/Right/Home/End, Enter or Space opens the item, Esc leaves), Super+Alt+S switches the screen reader on or off.
 * Spotlight (Ctrl+Space), Launchpad (F4) and the greeter / lock screen are keyboard driven already.
 *
 * The screen reader is Orca (Debian's) with aurora-a11y-bridge (aurora_a11y_bridge.c), started as the active session's
 * user: the bridge exports the shell (menu bar and its menus, Dock, Spotlight, Launchpad, greeter / lock screen,
 * notification banners) as an AT-SPI2 application on the user's accessibility bus, so Orca speaks it like an
 * application's widgets; applications (GTK, Firefox, Chrome) talk to Orca themselves over AT-SPI2.
 * The bridge watches the shell over the control socket: "a11y-watch" with a SOCK_SEQPACKET descriptor (SCM_RIGHTS);
 * Aurora writes one record per change (records below, text, at most AURORA_READER_RECORD bytes), only while the
 * watcher's user owns the active session (root watchers always):
 *   "shell\n" + "focus PATH\n" + "node\tPATH\tPARENT\tROLE\tSTATES\tNAME\tDESCRIPTION[\tTEXT]\n"...   PATH "-": not in the shell
 *   "notice\tTEXT\n"   a banner was shown        "quit\n"   the screen reader was switched off: exit
 * The bridge also provides org.freedesktop.a11y.KeyboardMonitor (what Orca's keyboard device uses without an X
 * display): it asks for key events and grabs on the same descriptor ("keys", "grab-all", "grabs"; aurora_reader.c);
 * Aurora then reports every key of the session ("key" records) and keeps the screen reader's grabbed keys (its
 * modifier keys, every key while one is held, its keystrokes) from the applications and the shell. Nothing is
 * reported while the lock screen or the greeter is up, and only to the bridge Aurora itself started (the control
 * connection's process is checked): another program of the user cannot read the keyboard this way.
 * Control commands: "screen-reader" -> "screen_reader=0|1 keys=0|1", "set-screen-reader 0|1 [debug]" (the active
 * session; debug: the bridge traces on Aurora's console and Orca logs to /run/user/UID/orca-debug.txt).
 * The setting is kept per user across logins (/home/nexus/.aurora/screen-reader.conf, "UID 0|1"): when a session
 * comes on screen unlocked and its user left the screen reader on, Aurora starts it again (AUTOSTART).
 * Entries carry their text as an 8th field ("node\t...\tDESCRIPTION\tTEXT", Spotlight's query); password fields never. */
#ifndef AURORA_READER_H
#define AURORA_READER_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

#define AURORA_READER_PROGRAM "/usr/libexec/aurora-a11y-bridge"
#define AURORA_READER_RECORD 16384

void aurora_reader_init(struct wl_event_loop* loop);  /* control commands */
/* Every key (evdev code, after the xkb state was updated): keysym, UTF-32 character, X modifier mask. True: the
 * screen reader took it, nobody else gets it. */
bool aurora_reader_key_event(unsigned key, bool pressed, uint32_t keysym, uint32_t unichar, uint32_t state);
void aurora_reader_tick(void);                        /* the main loop: send the shell's state when it changed */
/* A shell key press (evdev code; value 1 press, 2 repeat; AURORA_MOD_* mods): true when taken; *action is then a
 * shell action to run (0 none). */
bool aurora_reader_key(unsigned key, int value, unsigned mods, int* action);
void aurora_reader_notice(const char* text);          /* a banner is shown */
bool aurora_reader_reaped(pid_t pid);                 /* a child exited: true when it was a screen reader */
void aurora_reader_session_end(unsigned uid);         /* that session logs out: its screen reader stops */
bool aurora_reader_dock_focus(void);                  /* the Dock has the keyboard focus (the shell takes keys) */
#endif
