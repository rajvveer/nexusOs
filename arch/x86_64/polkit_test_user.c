/* Authorization-broker + pkexec end-to-end check (tests/test_x64_polkit.py). Started as /sbin/init (root, via
 * /nexus/init-as-root). It launches /usr/libexec/nexus-polkitd and exercises the broker and pkexec:
 *   - broker QUERY: an auth_admin action challenges; a "yes" action is authorized; an unknown one is denied;
 *   - broker CHECK with the right / wrong password: authorized / denied; a non-admin is denied an admin action;
 *   - the subject is bound to the connecting peer (SO_PEERCRED);
 *   - pkexec (through the broker) with the right password runs a program as root; wrong password / non-admin refused;
 *   - lockout on the broker path: 5 wrong passwords for 'lockb' (each delayed 2 s by nexus-auth) and the 6th attempt -
 *     even with the RIGHT password - is "DENIED throttled"; the shared tally /var/lib/nexus-auth/lockb counts 5;
 *   - lockout on pkexec's fallback (broker stopped): the same for 'lockp';
 *   - recovery: once the 60 s lockout has expired both accounts authenticate again and their tallies reset to 0.
 * Prints POLKIT PASS lines and finally "POLKIT ALL PASSED"; any failure prints "POLKIT FAIL <n>" and exits 1. */
#define _GNU_SOURCE
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SOCK "/run/nexus-polkit.sock"
#define NEXUS_PW "nexusPass1"
#define ALICE_PW "alicePass2"
#define LOCKB_PW "lockbPass3"   /* uid 1002, admin: locked out through the broker */
#define LOCKP_PW "lockpPass4"   /* uid 1003, admin: locked out through pkexec's fallback */

static int fail(int n) { printf("[64] POLKIT FAIL %d\n", n); fflush(stdout); return 1; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* One broker request; the reply line (no newline) into out. Returns 0 on success. */
static int broker_line(const char* request, char* out, size_t size) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0); if (s < 0) return -1;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; strncpy(a.sun_path, SOCK, sizeof(a.sun_path) - 1);
    if (connect(s, (struct sockaddr*)&a, sizeof(a))) { close(s); return -1; }
    if (write(s, request, strlen(request)) != (ssize_t)strlen(request)) { close(s); return -1; }
    char buf[128]; ssize_t n = read(s, buf, sizeof(buf) - 1); close(s);
    if (n <= 0) return -1; buf[n] = 0;
    buf[strcspn(buf, "\n")] = 0;
    strncpy(out, buf, size - 1); out[size - 1] = 0; return 0;
}
/* The reply's first token only (e.g. "DENIED not-authorized" -> "DENIED"). */
static int broker(const char* request, char* out, size_t size) {
    if (broker_line(request, out, size)) return -1;
    out[strcspn(out, " ")] = 0; return 0;
}

/* Run pkexec as uid/gid with `password` on a pipe (PKEXEC_PASSFD), executing /bin/pkexec-probe which writes its
 * euid to stdout. Returns the probe's euid, or -1 if pkexec refused; pkexec's message goes to msg (if given). */
static int run_pkexec(unsigned uid, unsigned gid, const char* password, char* msg, size_t msgsize) {
    int pw[2], rp[2]; if (pipe(pw) || pipe(rp)) return -1;
    pid_t pid = fork(); if (pid < 0) return -1;
    if (pid == 0) {
        dup2(rp[1], 1); dup2(rp[1], 2); close(rp[0]); close(rp[1]);
        char fd[8]; snprintf(fd, sizeof(fd), "%d", pw[0]); setenv("PKEXEC_PASSFD", fd, 1); close(pw[1]);
        if (gid) { setgroups(0, NULL); setgid(gid); } if (uid) setuid(uid);
        char* argv[] = {"pkexec", "/bin/pkexec-probe", NULL};
        execv("/usr/bin/pkexec", argv); _exit(126);
    }
    close(pw[0]); close(rp[1]);
    (void)!write(pw[1], password, strlen(password)); (void)!write(pw[1], "\n", 1); close(pw[1]);
    char buf[256]; ssize_t n, used = 0;
    while (used < (ssize_t)sizeof(buf) - 1 && (n = read(rp[0], buf + used, sizeof(buf) - 1 - used)) > 0) used += n;
    close(rp[0]); buf[used] = 0;
    if (msg) { strncpy(msg, buf, msgsize - 1); msg[msgsize - 1] = 0; }
    int status; waitpid(pid, &status, 0);
    if (!used || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return atoi(buf);
}

/* nexus-auth's failure tally for user: the count, or -1 if there is none. */
static int tally(const char* user) {
    char path[96]; snprintf(path, sizeof(path), "/var/lib/nexus-auth/%s", user);
    FILE* f = fopen(path, "r"); if (!f) return -1;
    unsigned count = 0; int ok = fscanf(f, "%u", &count); fclose(f);
    return ok == 1 ? (int)count : -1;
}

static pid_t start_broker(void) {
    int rf[2]; if (pipe(rf)) return -1;
    unlink(SOCK);
    pid_t daemon = fork();
    if (daemon == 0) {
        close(rf[0]); char fd[8]; snprintf(fd, sizeof(fd), "%d", rf[1]); setenv("POLKITD_READYFD", fd, 1);
        char* a[] = {"nexus-polkitd", NULL}; execv("/usr/libexec/nexus-polkitd", a); _exit(127);
    }
    close(rf[1]); char r; ssize_t got = read(rf[0], &r, 1); close(rf[0]);
    return got == 1 ? daemon : -1;
}
static void stop_broker(pid_t daemon) { kill(daemon, 15); waitpid(daemon, NULL, 0); unlink(SOCK); }

int main(int argc, char** argv) {
    (void)argc;
    /* Installed as /sbin/init (the test) AND /bin/pkexec-probe (what pkexec runs): the probe just prints its euid so
     * the parent sees it really became root. Distinguish by the program name pkexec invoked us with. */
    if (argv[0] && strstr(argv[0], "probe")) { printf("%u\n", (unsigned)geteuid()); return 0; }
    if (getuid() != 0) return fail(99);   /* the test half must be launched as root (init-as-root) */

    pid_t daemon = start_broker(); if (daemon < 0) return fail(2);
    printf("[64] POLKIT PASS broker started\n"); fflush(stdout);

    char out[128];
    if (broker("QUERY 1000 org.nexus.network.configure\n", out, sizeof(out)) || strcmp(out, "CHALLENGE")) return fail(3);
    printf("[64] POLKIT PASS QUERY auth_admin -> %s\n", out); fflush(stdout);
    if (broker("QUERY 1000 org.nexus.policy.query\n", out, sizeof(out)) || strcmp(out, "AUTHORIZED")) return fail(4);
    if (broker("QUERY 1000 org.nexus.bogus.action\n", out, sizeof(out)) || strcmp(out, "DENIED")) return fail(5);
    if (broker("CHECK 1000 org.nexus.network.configure " NEXUS_PW "\n", out, sizeof(out)) || strcmp(out, "AUTHORIZED")) return fail(6);
    printf("[64] POLKIT PASS CHECK admin+password -> AUTHORIZED\n"); fflush(stdout);
    if (broker("CHECK 1000 org.nexus.network.configure wrongpw\n", out, sizeof(out)) || strcmp(out, "DENIED")) return fail(7);
    printf("[64] POLKIT PASS CHECK admin+wrong-password -> DENIED\n"); fflush(stdout);
    /* alice (uid 1001) is not an administrator: an admin action is denied even with her correct password. */
    if (broker("CHECK 1001 org.nexus.network.configure " ALICE_PW "\n", out, sizeof(out)) || strcmp(out, "DENIED")) return fail(8);
    printf("[64] POLKIT PASS CHECK non-admin -> DENIED\n"); fflush(stdout);
    /* Subject binding (SO_PEERCRED): a non-root client may only ask about its own uid. */
    pid_t asker = fork();
    if (asker == 0) {
        setgroups(0, NULL); setgid(1001); setuid(1001);
        char o[64];
        if (broker("QUERY 1000 org.nexus.policy.query\n", o, sizeof(o)) || strcmp(o, "DENIED")) _exit(1);   /* about someone else */
        if (broker("QUERY 1001 org.nexus.policy.query\n", o, sizeof(o)) || strcmp(o, "AUTHORIZED")) _exit(2); /* about itself */
        _exit(0);
    }
    int st; waitpid(asker, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("[64] POLKIT subject-binding child status %d\n", st); return fail(9); }
    printf("[64] POLKIT PASS subject bound to the peer uid (SO_PEERCRED)\n"); fflush(stdout);

    if (run_pkexec(1000, 1000, NEXUS_PW, NULL, 0) != 0) return fail(10);   /* nexus is admin: via the broker */
    printf("[64] POLKIT PASS pkexec(admin, right password) ran as root (euid=0)\n"); fflush(stdout);
    if (run_pkexec(1000, 1000, "wrongpw", NULL, 0) == 0) return fail(11);
    printf("[64] POLKIT PASS pkexec(admin, wrong password) refused\n"); fflush(stdout);
    if (run_pkexec(1001, 1001, ALICE_PW, NULL, 0) == 0) return fail(12);
    printf("[64] POLKIT PASS pkexec(non-admin) refused\n"); fflush(stdout);

    /* Lockout on the broker path (lockb): five wrong passwords, each delayed by the helper, then the right one. */
    double t0 = now();
    for (int i = 0; i < 5; i++)
        if (broker_line("CHECK 1002 org.nexus.network.configure wrongpw\n", out, sizeof(out)) || strcmp(out, "DENIED not-authorized")) return fail(20 + i);
    double took = now() - t0;
    if (took < 9.0) { printf("[64] POLKIT broker 5 wrong took %.1f s\n", took); return fail(26); }   /* 5 x 2 s helper delay */
    if (tally("lockb") != 5) { printf("[64] POLKIT lockb tally %d\n", tally("lockb")); return fail(27); }
    if (broker_line("CHECK 1002 org.nexus.network.configure " LOCKB_PW "\n", out, sizeof(out)) || strcmp(out, "DENIED throttled")) {
        printf("[64] POLKIT broker 6th reply '%s'\n", out); return fail(28);
    }
    if (tally("lockb") != 5) return fail(29);   /* the locked-out attempt is not even checked */
    printf("[64] POLKIT PASS broker lockout: 5 wrong (%.1f s, tally 5), 6th (right password) -> DENIED throttled\n", took); fflush(stdout);

    /* pkexec's fallback: stop the broker, so pkexec checks through the helper itself. */
    stop_broker(daemon);
    char msg[256];
    if (run_pkexec(1000, 1000, NEXUS_PW, msg, sizeof(msg)) != 0) { printf("[64] POLKIT fallback: %s\n", msg); return fail(30); }
    printf("[64] POLKIT PASS pkexec fallback (no broker) ran as root with the right password\n"); fflush(stdout);
    t0 = now();
    for (int i = 0; i < 5; i++) if (run_pkexec(1003, 1003, "wrongpw", NULL, 0) == 0) return fail(31 + i);
    took = now() - t0;
    if (took < 9.0) { printf("[64] POLKIT pkexec 5 wrong took %.1f s\n", took); return fail(36); }
    if (tally("lockp") != 5) { printf("[64] POLKIT lockp tally %d\n", tally("lockp")); return fail(37); }
    double locked_at = now();
    if (run_pkexec(1003, 1003, LOCKP_PW, msg, sizeof(msg)) == 0 || !strstr(msg, "too many failed attempts")) {
        printf("[64] POLKIT pkexec 6th: %s\n", msg); return fail(38);
    }
    if (tally("lockp") != 5) return fail(39);
    printf("[64] POLKIT PASS pkexec fallback lockout: 5 wrong (%.1f s, tally 5), 6th (right password) refused: too many failed attempts\n", took); fflush(stdout);

    /* Recovery: nexus-auth stops an account's checks for 60 s after the 5th failure; wait it out. */
    double wait = 62.0 - (now() - locked_at); if (wait > 0) sleep((unsigned)wait + 1);
    if (run_pkexec(1003, 1003, LOCKP_PW, msg, sizeof(msg)) != 0) { printf("[64] POLKIT lockp recovery: %s\n", msg); return fail(40); }
    if (tally("lockp") != 0) return fail(41);   /* a right password resets the count */
    daemon = start_broker(); if (daemon < 0) return fail(42);
    if (broker_line("CHECK 1002 org.nexus.network.configure " LOCKB_PW "\n", out, sizeof(out)) || strcmp(out, "AUTHORIZED")) {
        printf("[64] POLKIT lockb recovery reply '%s'\n", out); return fail(43);
    }
    if (tally("lockb") != 0) return fail(44);
    printf("[64] POLKIT PASS lockout expired after 60 s: lockb (broker) and lockp (pkexec) authenticate again, tallies reset\n"); fflush(stdout);

    stop_broker(daemon);
    printf("[64] POLKIT ALL PASSED\n"); fflush(stdout);
    return 0;
}
