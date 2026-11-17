/* Aurora lock screen and greeter (aurora_lock.c).
 *
 * Locked (Super+L, menu bar > Lock Screen, idle timeout, before sleep) every display shows the dimmed wallpaper and the
 * clock; the main display asks for the session user's password (checked against /etc/shadow, aurora_auth.h, in a
 * child process so the desktop keeps running). Windows are hidden and get no input while locked. At boot the greeter
 * (the same screen with the list of accounts) comes first when the session user has a password and automatic login
 * is off (System Settings > Users; AURORA_LOGIN_CONF). Accounts without a password unlock with Enter.
 *
 * Power management (sleep/hibernate) contract: before suspending, call aurora_session_prepare_sleep() in the
 * compositor (or send "prepare-sleep" on the control socket, aurora_control.h, and wait for its OK): when "lock before
 * sleep" is on and the user has a password, the lock screen is drawn and presented before it returns, so the first
 * frame after resume is the lock screen. After resume call aurora_session_resumed() (or send "resumed"): the idle
 * timer restarts (and the displays are re-probed by the caller if it wants). Both are cheap when nothing is locked. */
#ifndef AURORA_LOCK_H
#define AURORA_LOCK_H
#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>

/* ---- services of desktop.c ---- */
void aurora_host_lock_changed(bool locked);   /* hide/show windows, keyboard focus away/back, repaint everything */
void aurora_host_present_now(void);           /* paint and present at once (before sleep) */
void aurora_host_lock_redraw(void);           /* the lock layer changed */

/* ---- aurora_lock.c ---- */
void aurora_lock_init(struct wl_event_loop* loop);   /* login.conf, idle timer, control commands, the greeter when required */
bool aurora_lock_active(void);
void aurora_lock_now(const char* reason);
void aurora_lock_key(unsigned key, const char* text);    /* key press while active (evdev code, its UTF-8 text) */
void aurora_lock_pointer(int x, int y);                  /* left click while active (desktop coordinates) */
void aurora_lock_draw(int dx, int dy, int dw, int dh, bool primary);   /* one display: dx..dh its desktop rectangle */
void aurora_lock_activity(void);                         /* input happened (idle timer) */
void aurora_lock_tick(void);                             /* the main loop, at least every 5 s: idle lock, the clock */
bool aurora_lock_reaped(pid_t pid, int status);          /* a child exited: true when it was a password check */
/* The screen reader's view (aurora_reader.c): 0 off, 1 locked, 2 greeter; accounts shown, the chosen one, the status
 * line; account i (the greeter's, else the locked session's user). */
int aurora_lock_a11y(int* count, int* chosen, const char** status);
const char* aurora_lock_a11y_user(int i, bool* signed_in);
bool aurora_session_prepare_sleep(void);                 /* true when the lock screen is on screen now */
void aurora_session_resumed(void);
#endif
