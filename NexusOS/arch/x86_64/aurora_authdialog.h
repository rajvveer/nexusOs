/* The Aurora authentication agent (aurora_authdialog.c): the password dialog for the authorization broker
 * nexus-polkitd (sec2: /usr/libexec/nexus-polkitd on /run/nexus-polkit.sock, the PolicyKit1 CheckAuthorization subset).
 *
 *   aurora-app authenticate ACTION [--app NAME] [--description TEXT] [--passfd N] [-- PROGRAM ARG...]
 *
 * QUERY uid ACTION; AUTHORIZED: done without a dialog; CHALLENGE: a modal dialog names the requesting application, what
 * it wants to do and the action id, and asks for the user's own password (auth_self / auth_admin rules); each attempt
 * is a CHECK, wrong ones are reported with the attempt count, the broker's lockout (five tries, "DENIED throttled")
 * locks the dialog with a countdown. On success the password goes to descriptor N (for helpers that verify it again,
 * e.g. nexus-auth) and/or PROGRAM runs through /usr/bin/pkexec with it on $PKEXEC_PASSFD.
 * Exit status: 0 authorized (or PROGRAM's status), 1 cancelled, 2 usage or no broker, 3 not allowed, 4 locked out.
 * Lines "[64] AURORA AUTH ..." go to stdout for tests (never the password). */
#ifndef AURORA_AUTHDIALOG_H
#define AURORA_AUTHDIALOG_H
#include <stdbool.h>
#include <stddef.h>

#define AURORA_POLKIT_SOCKET "/run/nexus-polkit.sock"
#define AURORA_ACTION_ACCOUNTS "org.nexus.accounts.manage"   /* add/remove accounts, administrators (auth_admin) */
#define AURORA_ACTION_EXEC "org.freedesktop.policykit.exec"     /* pkexec: run a program as root (auth_admin) */
/* One request to the broker: "AUTHORIZED", "CHALLENGE", "DENIED <reason>", or "" when it cannot be reached. */
bool aurora_authz_request(const char* line, char* reply, size_t size);
bool aurora_authz_available(void);                      /* the broker answers */
bool aurora_authz_known(const char* action);             /* ACTIONS lists it */
int aurora_authdialog_main(int argc, char** argv);       /* aurora-app authenticate ...: the window id, or exits */
bool aurora_authdialog_tick(void);                       /* the main loop: a CHECK finished, the lockout ticks */
#endif
