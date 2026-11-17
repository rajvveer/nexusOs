/* Aurora power UI (aurora_power.c): menu-bar battery/brightness status and panel, the power-button dialog,
 * Sleep / Hibernate / Restart requests and the kernel's power-management notifications.
 *
 * desktop.c calls these from its shell hooks; aurora_power.c calls back aurora_shell_notice() and
 * aurora_shell_redraw() (defined in desktop.c). Actions returned to the shell are UI_ACT_* ids (macui.h).
 *
 * Lock screen hook: a desktop component that wants to lock before the machine sleeps defines
 * aurora_lock_for_sleep() (weak here). It runs when the kernel announces PM_EVENT=suspend-prepare /
 * hibernation-prepare / freeze-prepare; Aurora acknowledges (/sys/power/policy/sleep_ack) once the next frame
 * is on screen, so the locked screen is what shows on wake. */
#ifndef AURORA_POWER_H
#define AURORA_POWER_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct wl_event_loop;
bool aurora_power_init(struct wl_event_loop* loop);   /* uevent socket, power-key policy, first status read */
bool aurora_power_open(void);                        /* dialog or panel shown: the shell takes all input */
bool aurora_power_key(unsigned key, int value);      /* evdev key; true when consumed */
int  aurora_power_click(int x, int y);               /* click while open -> action for shell_action, or 0 */
bool aurora_power_action(int action);                /* handles UI_ACT_SLEEP/HIBERNATE/BATTERY/POWERDIALOG */
bool aurora_power_bounds(int* x, int* y, int* w, int* h);
void aurora_power_draw(void);
int  aurora_power_timeout(uint32_t now);             /* ms until aurora_power_tick wants to run, -1: none */
int  aurora_power_tick(uint32_t now, bool painted);  /* -> action (dialog timeout: UI_ACT_SHUTDOWN_NOW) or 0 */
bool aurora_power_reaped(pid_t pid, int status);     /* true when pid was a sleep request of ours */
void aurora_power_restart(bool restart);             /* Restart chosen: reboot instead of power-off at exit */
void aurora_power_finish(void);                      /* desktop exiting after Shut Down / Restart */
void aurora_lock_for_sleep(void) __attribute__((weak));
void aurora_lock_after_sleep(void) __attribute__((weak));   /* after the resume (PM_EVENT=post-*): aurora_lock.c */

/* desktop.c */
void aurora_shell_notice(const char* text);
void aurora_shell_redraw(bool everything);
#endif
