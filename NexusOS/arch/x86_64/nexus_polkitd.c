/* nexus-polkitd: a small authorization broker, the NexusOS stand-in for polkitd (org.freedesktop.PolicyKit1).
 * Installed at /usr/libexec/nexus-polkitd and started as root (by the session or a unit). It decides whether a
 * subject (a uid) may perform an action, consulting the shared accounts (/etc/passwd, group, shadow via
 * aurora_auth.c) so a privileged GUI operation can be gated on an administrator's password instead of the whole
 * session holding the capability. pkexec and desktop apps (e.g. the Network panel) ask it; it never runs the
 * privileged work itself - the caller does, once AUTHORIZED.
 *
 * Wire protocol on the SOCK_STREAM Unix socket /run/nexus-polkit.sock (one request line per connection):
 *   QUERY <uid> <action>                -> AUTHORIZED | CHALLENGE | DENIED <reason>
 *   CHECK <uid> <action> <password>     -> AUTHORIZED | DENIED <reason>
 *   ACTIONS                             -> one "<action> <rule>" line per known action, then END
 * This is the CheckAuthorization subset of org.freedesktop.PolicyKit1: QUERY is a check without
 * AllowUserInteraction (CHALLENGE == is_challenge), CHECK carries the authentication an auth agent collected
 * (AllowUserInteraction set). The caller passes the password it obtained from its own agent (the Aurora dialog);
 * the broker has it checked by the nexus-auth helper only (never in process), so the same per-account five-tries
 * lockout and 2 s delay as the lock screen and login apply; a locked-out account gets "DENIED throttled".
 *
 * Rules (freedesktop semantics): yes = always; auth_self = the subject's own password; auth_admin = the subject
 * must be an administrator (sudo/wheel) and give their own password; no = never.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "aurora_auth.h"

#define SOCK "/run/nexus-polkit.sock"
enum { RULE_NO, RULE_YES, RULE_AUTH_SELF, RULE_AUTH_ADMIN };
static const struct { const char* action; int rule; } POLICY[] = {
    {"org.freedesktop.policykit.exec", RULE_AUTH_ADMIN},   /* pkexec: run a program as root */
    {"org.nexus.network.configure", RULE_AUTH_ADMIN},      /* change interfaces/routes/firewall (Network panel) */
    {"org.nexus.network.vpn", RULE_AUTH_ADMIN},            /* bring a VPN up/down */
    {"org.nexus.settings.datetime", RULE_AUTH_ADMIN},      /* set the clock/timezone */
    {"org.nexus.settings.hostname", RULE_AUTH_ADMIN},
    {"org.nexus.accounts.manage", RULE_AUTH_ADMIN},       /* add/delete/promote accounts (Settings > Users, desk2) */
    {"org.nexus.policy.query", RULE_YES},                  /* a harmless probe, for testing a reachable broker */
};
static int rule_for(const char* action) {
    for (unsigned i = 0; i < sizeof(POLICY) / sizeof(POLICY[0]); i++) if (!strcmp(POLICY[i].action, action)) return POLICY[i].rule;
    return -1;
}
static void wipe(char* s, size_t n) { if (s) memset(s, 0, n); }

/* Decide (uid, action, password?). Returns 1 authorized, 0 challenge (needs auth, none given), -1 denied, -2 locked out.
 * The password is checked only by the nexus-auth helper (aurora_auth_verify_helper), never in this process: the broker
 * gets the same per-account five-tries lockout and 2 s wrong-password delay as the lock screen and login. */
static int decide(unsigned uid, const char* action, const char* password) {
    int rule = rule_for(action);
    if (rule < 0) return -1;                               /* unknown action: deny (implicit "no") */
    if (rule == RULE_NO) return -1;
    if (rule == RULE_YES) return 1;
    struct aurora_user u;
    if (!aurora_user_by_uid(uid, &u)) return -1;
    if (rule == RULE_AUTH_ADMIN && !u.admin) return -1;    /* only administrators for admin actions */
    if (!password || !password[0]) return 0;               /* QUERY / no interaction: challenge */
    int r = aurora_auth_verify_helper(u.name, password);   /* the helper sleeps 2 s on a wrong password */
    return r == AURORA_AUTH_OK ? 1 : r == AURORA_AUTH_THROTTLED ? -2 : -1;   /* helper missing (UNAVAILABLE): refuse */
}

static void serve(int c) {
    char line[700]; ssize_t n = read(c, line, sizeof(line) - 1);
    if (n <= 0) return;
    line[n] = 0; char* nl = strchr(line, '\n'); if (nl) *nl = 0;
    char verb[16], action[128], password[256] = ""; unsigned uid = 0;
    int fields = sscanf(line, "%15s %u %127s %255[^\n]", verb, &uid, action, password);
    const char* reply = "DENIED malformed\n";
    if (fields >= 1 && !strcmp(verb, "ACTIONS")) {
        for (unsigned i = 0; i < sizeof(POLICY) / sizeof(POLICY[0]); i++) {
            static const char* const names[] = {"no", "yes", "auth_self", "auth_admin"};
            char l[200]; int m = snprintf(l, sizeof(l), "%s %s\n", POLICY[i].action, names[POLICY[i].rule]);
            if (m > 0) (void)!write(c, l, (size_t)m);
        }
        (void)!write(c, "END\n", 4); wipe(line, sizeof(line)); return;
    }
    if (fields >= 3 && (!strcmp(verb, "QUERY") || !strcmp(verb, "CHECK"))) {
        /* The subject is the connecting process (SO_PEERCRED), never just the uid it claims: a client may only ask
         * about itself; root may ask on behalf of another uid (as polkit lets a privileged mechanism do). */
        struct ucred peer; socklen_t len = sizeof(peer);
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &peer, &len) || (peer.uid != 0 && peer.uid != uid))
            reply = "DENIED subject-mismatch\n";
        else {
            int d = decide(uid, action, strcmp(verb, "CHECK") ? "" : (fields >= 4 ? password : ""));
            reply = d == 1 ? "AUTHORIZED\n" : d == 0 ? "CHALLENGE\n" : d == -2 ? "DENIED throttled\n" : "DENIED not-authorized\n";
        }
    }
    (void)!write(c, reply, strlen(reply));
    wipe(line, sizeof(line)); wipe(password, sizeof(password));
}

int main(int argc, char** argv) {
    bool once = argc > 1 && !strcmp(argv[1], "--once");   /* test mode: serve a few requests then exit */
    signal(SIGPIPE, SIG_IGN);
    mkdir("/run", 0755);   /* the socket lives in /run (created if the root lacks it) */
    unlink(SOCK);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCK, sizeof(addr.sun_path) - 1);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr))) { perror("bind"); return 1; }
    chmod(SOCK, 0666);                                     /* any session process may ask; the broker authenticates */
    if (listen(s, 16)) { perror("listen"); return 1; }
    const char* readyfd = getenv("POLKITD_READYFD");   /* tell a launcher the socket is up (test synchronisation) */
    if (readyfd) { int fd = atoi(readyfd); if (fd >= 0) { (void)!write(fd, "R", 1); close(fd); } }
    for (int served = 0; !once || served < 64; served++) {
        int c = accept(s, NULL, NULL); if (c < 0) { if (errno == EINTR) continue; break; }
        serve(c); close(c);
    }
    return 0;
}
