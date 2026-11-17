/* pkexec: run a program as another user (root by default) after the caller is authorized, like PolicyKit's pkexec.
 * Installed set-user-ID root at /usr/bin/pkexec on an ext4 (POSIX) root.
 *
 *   pkexec [--user USER] PROGRAM [ARG...]
 *
 * Authorization for the action org.freedesktop.policykit.exec: the caller must be an administrator (a member of the
 * sudo or wheel group) and authenticate with their OWN password (polkit's auth_admin). root (uid 0) is not asked.
 * The password is read from the authentication agent's pipe named by $PKEXEC_PASSFD when set (the Aurora auth agent
 * writes it there), otherwise from stdin - never from a command-line argument. The password is checked only by the
 * nexus-auth helper (aurora_auth_verify_helper), never in this process, so pkexec gets the same per-account
 * five-tries lockout and 2 s delay as the lock screen and login. When the broker nexus-polkitd is reachable on
 * /run/nexus-polkit.sock the decision is delegated to it (so a desktop policy and its agent apply; it asks the same
 * helper); otherwise pkexec applies the same rule itself through the helper. Without the helper it refuses. On
 * success it sets the target's groups, gid and uid, sanitises the environment and exec()s the program.
 *
 * Exit status: 127 not authorized / usage / target errors; 126 the program could not be run; otherwise the program's.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "aurora_auth.h"

#define POLKIT_SOCK "/run/nexus-polkit.sock"
#define ACTION_EXEC "org.freedesktop.policykit.exec"

static void wipe(char* s) { if (s) memset(s, 0, strlen(s)); }

/* Read one password line (no newline) from $PKEXEC_PASSFD if set, else stdin. */
static int read_password(char* out, size_t size) {
    int fd = 0; const char* passfd = getenv("PKEXEC_PASSFD");
    if (passfd) { fd = atoi(passfd); if (fd < 0) return -1; }
    size_t used = 0; ssize_t n;
    while (used < size - 1 && (n = read(fd, out + used, 1)) == 1) { if (out[used] == '\n') break; used++; }
    if (n < 0) return -1;
    out[used] = 0; return 0;
}

/* Ask the broker to authorize (uid, action) with this password. Returns 1 authorized, 0 denied, -1 unreachable. */
static int broker_check(unsigned uid, const char* action, const char* password) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (s < 0) return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, POLKIT_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr))) { close(s); return -1; }
    char line[600]; int n = snprintf(line, sizeof(line), "CHECK %u %s %s\n", uid, action, password);
    if (n <= 0 || write(s, line, (size_t)n) != n) { wipe(line); close(s); return -1; }
    wipe(line);
    char reply[64]; ssize_t r = read(s, reply, sizeof(reply) - 1); close(s);
    if (r <= 0) return -1;
    reply[r] = 0;
    return strncmp(reply, "AUTHORIZED", 10) == 0 ? 1 : strncmp(reply, "DENIED throttled", 16) == 0 ? 2 : 0;
}

int main(int argc, char** argv) {
    const char* target = "root";
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--user")) { if (i + 1 >= argc) { fprintf(stderr, "pkexec: --user needs a name\n"); return 127; } target = argv[i + 1]; i += 2; }
    if (i >= argc) { fprintf(stderr, "usage: pkexec [--user USER] PROGRAM [ARG...]\n"); return 127; }
    char** command = &argv[i];

    struct aurora_user tgt;
    if (!aurora_user_find(target, &tgt)) { fprintf(stderr, "pkexec: unknown user '%s'\n", target); return 127; }

    uid_t caller_uid = getuid();
    if (caller_uid != 0) {   /* root is never challenged; everyone else must be an administrator and authenticate */
        struct aurora_user caller;
        if (!aurora_user_by_uid(caller_uid, &caller)) { fprintf(stderr, "pkexec: no account for uid %u\n", caller_uid); return 127; }
        if (!caller.admin) { fprintf(stderr, "pkexec: %s is not authorized (not an administrator)\n", caller.name); return 127; }
        char password[256];
        if (read_password(password, sizeof(password))) { fprintf(stderr, "pkexec: could not read the authentication\n"); return 127; }
        int decision = broker_check(caller_uid, ACTION_EXEC, password);   /* a running policy broker decides if present */
        if (decision < 0) {   /* no broker: the same rule, the password checked by the nexus-auth helper (lockout + delay) */
            int r = aurora_auth_verify_helper(caller.name, password);
            decision = r == AURORA_AUTH_OK ? 1 : r == AURORA_AUTH_THROTTLED ? 2 : 0;
        }
        wipe(password);
        if (decision == 2) { fprintf(stderr, "pkexec: too many failed attempts for %s; try again later\n", caller.name); return 127; }
        if (decision != 1) { fprintf(stderr, "pkexec: authentication failed\n"); return 127; }
    }

    /* Become the target: groups, gid, then uid (order matters so privilege is actually dropped/raised correctly). */
    if (initgroups(tgt.name, tgt.gid) && setgroups(0, NULL)) { fprintf(stderr, "pkexec: cannot set groups\n"); return 127; }
    if (setgid(tgt.gid) || setuid(tgt.uid)) { fprintf(stderr, "pkexec: cannot switch to %s\n", tgt.name); return 127; }
    if (setuid(0) == 0 && tgt.uid != 0) { fprintf(stderr, "pkexec: privilege drop failed\n"); return 127; }  /* uid change must be irreversible */

    /* Minimal, sanitised environment (polkit resets it): nothing inherited that ld.so or the shell would trust. */
    char uidbuf[16]; snprintf(uidbuf, sizeof(uidbuf), "%u", caller_uid);
    char* env[] = {NULL, NULL, NULL, NULL, NULL, NULL};
    char home[160], user[64], shell[96], path[] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin", logname[64];
    snprintf(home, sizeof(home), "HOME=%s", tgt.home[0] ? tgt.home : "/root");
    snprintf(user, sizeof(user), "USER=%s", tgt.name);
    snprintf(logname, sizeof(logname), "LOGNAME=%s", tgt.name);
    snprintf(shell, sizeof(shell), "SHELL=%s", tgt.shell[0] ? tgt.shell : "/bin/sh");
    int e = 0; env[e++] = path; env[e++] = home; env[e++] = user; env[e++] = logname; env[e++] = shell;
    setenv("PKEXEC_UID", uidbuf, 1);   /* polkit sets this for the target */
    environ = env;
    execvp(command[0], command);
    fprintf(stderr, "pkexec: cannot run %s: %s\n", command[0], strerror(errno));
    return 126;
}
