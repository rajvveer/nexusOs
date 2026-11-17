/* Aurora user sessions on the one seat (aurora_session.c).
 *
 * Aurora (pid 1) runs as the desktop user (the boot account, uid 1000): that account's session is Aurora's own children
 * (session 1). Any other account signs in at the greeter through the privileged launcher nexus-session
 * (aurora_session_proto.h): its programs run as that user (uid/gid/groups, own home, XDG_RUNTIME_DIR=/run/user/UID,
 * its own D-Bus session bus) and connect through sockets in its own runtime folder, which the launcher hands to Aurora.
 * One session is active: only its windows are mapped, listed (dock, switcher, overview) and get input; the others'
 * windows are parked; the clipboard is cleared on every switch; the console devices follow the active user (kernel
 * seat, prctl PR_NEXUS_SEAT); the control socket answers the active user (and root) only. Switching to a running
 * session needs that user's password (the greeter). Log Out ends the active session: every process of the user.
 * Needs an ext4 (POSIX) root with the set-user-ID launcher; on a FAT root only the desktop user can sign in.
 * logind (nexus_logind.c) mirrors this table ("sessions" on the control socket) and is poked on every change. */
#ifndef AURORA_SESSION_H
#define AURORA_SESSION_H
#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>

void aurora_session_init(struct wl_display* display);
unsigned aurora_session_active_uid(void);
bool aurora_session_foreign(void);                 /* the active session is not the desktop user's own */
bool aurora_session_running(unsigned uid);         /* signed in (boot: logged in; others: an agent) */
bool aurora_session_client_visible(struct wl_client* client);   /* belongs to the active session (or root) */
/* Sign in USER: the launcher checks the password; the verdict comes to aurora_lock_session_result(). */
void aurora_session_start(const char* user, const char* password);
void aurora_session_activate_uid(unsigned uid);    /* switch to that running session (after its password was checked) */
void aurora_session_boot_login(bool logged_in);    /* the desktop user's own session signed in / out */
void aurora_session_boot_greeter(void);            /* at boot the greeter comes first: not signed in yet */
void aurora_session_logout(void);                  /* end the active session; the greeter follows */
void aurora_session_shutdown(void);                /* Shut Down: every other session logs out */
/* Start a program in the active (foreign) session: leading KEY=VALUE entries become its environment. -1 on failure. */
pid_t aurora_session_run(char** command, const char* tag);
bool aurora_session_reaped(pid_t pid, int status); /* a launcher/agent process of ours exited */
void aurora_session_idle_inhibit(int delta);      /* idle inhibitors (zwp_idle_inhibit, logind "idle"): no idle lock */
bool aurora_session_idle_inhibited(void);
void aurora_session_notify(const char* text);      /* a notification of the desktop user's own session (history, DND) */

/* ---- services of desktop.c ---- */
void aurora_host_session_switched(void);           /* park/unpark windows, clear the selection, repaint */
void aurora_host_reaped(pid_t pid, int status);    /* a program of a foreign session ended (agent report) */
void aurora_host_session_end_clients(unsigned uid, int signal_number);   /* the desktop user's logout: its clients */
void aurora_shell_notice(const char* text);        /* the notice area */
void aurora_host_power_request(bool reboot);       /* logind PowerOff/Reboot: the orderly shutdown, forced */
/* ---- services of aurora_lock.c ---- */
void aurora_lock_session_result(int code, unsigned uid);   /* the launcher's verdict (AURORA_AUTH_*) */
void aurora_lock_session_ended(unsigned uid, bool was_active);
void aurora_lock_switch_user(void);
#endif
