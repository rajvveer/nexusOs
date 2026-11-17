/* The secure attention key (aurora_sak.c): Super+Escape, handled by Aurora before anything else sees the keyboard -
 * applications, the input method, the screen reader's keyboard monitor and its grabs. Only physical keyboards reach
 * it: keys an input method injects (zwp_virtual_keyboard_v1) go straight to the focused application.
 *   - the focused window is the genuine authentication dialog (its process runs Aurora's own /bin/aurora-app
 *     authenticate, checked through /proc when its Wayland connection was made - not anything the client says -, with
 *     no LD_PRELOAD / LD_LIBRARY_PATH / LD_AUDIT, and the dialog makes itself non-dumpable once connected, so another
 *     process of the user cannot ptrace it or read its memory): a banner confirms it and names the requesting
 *     application and the action;
 *   - anything else (another window, a full-screen look-alike, nothing): the real lock screen comes up at once, saying
 *     that the window was not a NexusOS dialog;
 *   - on the lock screen or the greeter: it confirms that this is the NexusOS lock screen.
 * Lines "[64] AURORA SAK ..." for tests. */
#ifndef AURORA_SAK_H
#define AURORA_SAK_H
#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>

/* A key event (evdev code) with Super held or not: true when it is (part of) the secure attention key. */
bool aurora_sak_key(unsigned key, bool pressed, bool super_held);
void aurora_sak_init(struct wl_display* display);   /* watches new clients for the dialog */

/* ---- services of desktop.c ---- */
pid_t aurora_host_focus_pid(char* app_id, unsigned size, struct wl_client** client);   /* the focused toplevel's client, 0 when none */
/* ---- services of aurora_lock.c ---- */
void aurora_lock_sak(const char* text);                     /* the lock screen's status line */
#endif
