/* nexus-auth on an ext4 root (tests/test_x64_auth_ext4.py): /sbin/init of an apps64-ext4.img copy, run as uid 1000,
 * with /usr/libexec/nexus-auth set-user-ID root, /etc/shadow 0640 root:shadow and /bin/authtest-root (this program,
 * set-user-ID root) used once to make nexus an administrator. Every helper answer is checked (exit status AURORA_AUTH_*):
 *   status (privileged, writable); /etc/shadow unreadable to the user; LD_PRELOAD ignored by the set-ID helper (AT_SECURE)
 *   while a plain program loads it; verify right/wrong; a non-administrator cannot add users, change others' passwords or
 *   grant rights; own password change (wrong old refused); five wrong root passwords throttle root's checks (even the
 *   right one, also from three parallel helpers) while nexus still verifies; path-like and over-long input refused;
 *   as an administrator: useradd (uid 1001, private group, /home/alice 0700), admin grant, no self-demotion or
 *   self-deletion, userdel. Log: [64] AUTHTEST ... ; failures: [64] AUTHTEST FAIL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "aurora_auth.h"

#define CHECK(x) do { if (!(x)) { printf("[64] AUTHTEST FAIL line=%d: %s\n", __LINE__, #x); fflush(stdout); exit(1); } } while (0)
#define PASS(...) do { printf("[64] AUTHTEST PASS " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static const char* ROOT_PW = "rootSecret8", *NEXUS_PW = "nexusPass1", *NEW_PW = "nexusNew22";
static char out[256];
static char* const* environment;

/* Run the helper with argv (NULL-terminated, without the program) and stdin text; exit status, first output line in out. */
static int helper_env(const char* in, char* const* env, const char* const* args) {
    int pin[2], pout[2]; CHECK(!pipe2(pin, O_CLOEXEC) && !pipe2(pout, O_CLOEXEC));
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        dup2(pin[0], 0); dup2(pout[1], 1);
        const char* argv[8] = {"nexus-auth"}; for (int i = 0; i < 6 && args[i]; i++) argv[i + 1] = args[i];
        execve(AURORA_HELPER, (char* const*)argv, env); _exit(120);
    }
    close(pin[0]); close(pout[1]);
    size_t n = strlen(in); CHECK(write(pin[1], in, n) == (ssize_t)n || n > 4096); close(pin[1]);
    ssize_t got = read(pout[0], out, sizeof(out) - 1); out[got > 0 ? got : 0] = 0; char* nl = strchr(out, '\n'); if (nl) *nl = 0;
    close(pout[0]); int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}
#define HELPER(in, ...) helper_env(in, environment, (const char* const[]){__VA_ARGS__, NULL})
static bool exists(const char* path) { struct stat st; return !stat(path, &st); }
static bool file_has(const char* path, const char* text) {
    char data[4096] = {0}; int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return false;
    ssize_t n = read(fd, data, sizeof(data) - 1); close(fd); return n > 0 && strstr(data, text);
}

static int promote(void) {   /* /bin/authtest-root promote: nexus joins sudo (test setup only) */
    CHECK(geteuid() == 0);
    char data[4096] = {0}; int fd = open("/etc/group", O_RDONLY | O_CLOEXEC); CHECK(fd >= 0);
    ssize_t n = read(fd, data, sizeof(data) - 1); close(fd); CHECK(n > 0);
    char* at = strstr(data, "sudo:x:27:\n"); CHECK(at);
    char next[4200]; snprintf(next, sizeof(next), "%.*ssudo:x:27:nexus\n%s", (int)(at - data), data, at + strlen("sudo:x:27:\n"));
    fd = open("/etc/group", O_WRONLY | O_TRUNC | O_CLOEXEC); CHECK(fd >= 0 && write(fd, next, strlen(next)) == (ssize_t)strlen(next) && !close(fd));
    return 0;
}

int main(int argc, char** argv, char** envp) {
    environment = envp; signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "promote")) return promote();
    if (argc == 2 && !strcmp(argv[1], "probe")) return 0;   /* LD_PRELOAD positive control */
    CHECK(getuid() == 1000 && geteuid() == 1000);
    char line[300];

    int code = HELPER("", "status"); printf("[64] AUTHTEST helper status=%d: %s\n", code, out); fflush(stdout);
    CHECK(code == 0 && !strcmp(out, "posix=1 writable=1 euid=0"));
    CHECK(open("/etc/shadow", O_RDONLY | O_CLOEXEC) < 0 && errno == EACCES);
    PASS("status: privileged and writable; /etc/shadow unreadable to uid 1000");

    /* set-ID secure mode: the helper does not load LD_PRELOAD, a plain program does */
    char* preload[] = {"LD_PRELOAD=/bin/authpre.so", "LD_LIBRARY_PATH=/tmp", NULL};
    unlink("/tmp/authpre-ran");
    code = helper_env("", preload, (const char* const[]){"status", NULL});
    printf("[64] AUTHTEST preloaded helper status=%d: %s marker=%d\n", code, out, exists("/tmp/authpre-ran")); fflush(stdout);
    CHECK(code == 0 && !exists("/tmp/authpre-ran"));
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) { char* args[] = {"/sbin/init", "probe", NULL}; execve("/sbin/init", args, preload); _exit(120); }
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status) && exists("/tmp/authpre-ran"));
    unlink("/tmp/authpre-ran");
    PASS("AT_SECURE: LD_PRELOAD ignored by the set-user-ID helper, loaded by a plain program");

    snprintf(line, sizeof(line), "%s\n", NEXUS_PW); code = HELPER(line, "verify", "nexus");
    if (code != AURORA_AUTH_OK) { printf("[64] AUTHTEST verify nexus status=%d\n", code); fflush(stdout); }
    CHECK(code == AURORA_AUTH_OK);
    CHECK(HELPER("not-it\n", "verify", "nexus") == AURORA_AUTH_WRONG);
    PASS("verify: right and wrong password");

    snprintf(line, sizeof(line), "%s\nalicePw1\n", NEXUS_PW);
    CHECK(HELPER(line, "useradd", "alice", "Alice", "0") == AURORA_AUTH_DENIED);
    snprintf(line, sizeof(line), "%s\nhijack99\n", NEXUS_PW); CHECK(HELPER(line, "passwd", "root") == AURORA_AUTH_DENIED);
    snprintf(line, sizeof(line), "%s\n", NEXUS_PW); CHECK(HELPER(line, "admin", "nexus", "1") == AURORA_AUTH_DENIED);
    PASS("a non-administrator cannot add users, change root's password or grant rights");

    snprintf(line, sizeof(line), "wrong-old\n%s\n", NEW_PW); CHECK(HELPER(line, "passwd", "nexus") == AURORA_AUTH_WRONG);
    snprintf(line, sizeof(line), "%s\n%s\n", NEXUS_PW, NEW_PW); CHECK(HELPER(line, "passwd", "nexus") == AURORA_AUTH_OK);
    snprintf(line, sizeof(line), "%s\n", NEW_PW); CHECK(HELPER(line, "verify", "nexus") == AURORA_AUTH_OK);
    snprintf(line, sizeof(line), "%s\n", NEXUS_PW); CHECK(HELPER(line, "verify", "nexus") == AURORA_AUTH_WRONG);
    PASS("own password changed (a wrong old password refused)");

    for (int i = 0; i < 5; i++) CHECK(HELPER("guess\n", "verify", "root") == AURORA_AUTH_WRONG);
    snprintf(line, sizeof(line), "%s\n", ROOT_PW); CHECK(HELPER(line, "verify", "root") == AURORA_AUTH_THROTTLED);
    pid_t kids[3];
    for (int i = 0; i < 3; i++) { kids[i] = fork(); CHECK(kids[i] >= 0); if (!kids[i]) _exit(HELPER(line, "verify", "root")); }
    for (int i = 0; i < 3; i++) CHECK(waitpid(kids[i], &status, 0) == kids[i] && WIFEXITED(status) && WEXITSTATUS(status) == AURORA_AUTH_THROTTLED);
    snprintf(line, sizeof(line), "%s\n", NEW_PW); CHECK(HELPER(line, "verify", "nexus") == AURORA_AUTH_OK);
    PASS("throttle: 5 wrong root passwords stop root's checks for 60 s (also 3 parallel helpers); nexus unaffected");

    CHECK(HELPER("x\n", "verify", "../../etc/passwd") == AURORA_AUTH_INVALID);
    CHECK(open("/var/lib/nexus-auth/root", O_RDONLY | O_CLOEXEC) < 0 && errno == EACCES);   /* the tally is root's */
    static char big[6000]; memset(big, 'a', sizeof(big) - 2); big[sizeof(big) - 2] = '\n';
    CHECK(HELPER(big, "verify", "nexus") == AURORA_AUTH_INVALID);
    snprintf(line, sizeof(line), "%s\nx1\n", NEW_PW); CHECK(HELPER(line, "useradd", "bad:name", "B", "0") == AURORA_AUTH_INVALID);
    CHECK(HELPER(line, "frobnicate", "nexus") == AURORA_AUTH_INVALID);
    PASS("hostile input: path-like and odd user names, over-long stdin, unknown command refused; tally unreadable to the user");

    pid = fork(); CHECK(pid >= 0);
    if (!pid) { char* args[] = {"/bin/authtest-root", "promote", NULL}; execve(args[0], args, envp); _exit(120); }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status) && file_has("/etc/group", "sudo:x:27:nexus\n"));
    snprintf(line, sizeof(line), "%s\nalicePw1\n", NEW_PW);
    CHECK(HELPER(line, "useradd", "alice", "Alice Example", "0") == AURORA_AUTH_OK && !strcmp(out, "created alice uid=1001"));
    struct stat st; CHECK(!stat("/home/alice", &st) && S_ISDIR(st.st_mode) && st.st_uid == 1001 && st.st_gid == 1001 && (st.st_mode & 0777) == 0700);
    CHECK(file_has("/etc/passwd", "alice:x:1001:1001:Alice Example:/home/alice:/bin/bash\n") && file_has("/etc/group", "alice:x:1001:\n"));
    CHECK(HELPER("alicePw1\n", "verify", "alice") == AURORA_AUTH_OK);
    snprintf(line, sizeof(line), "%s\n", NEW_PW); CHECK(HELPER(line, "admin", "alice", "1") == AURORA_AUTH_OK && file_has("/etc/group", "sudo:x:27:nexus,alice\n"));
    CHECK(HELPER(line, "admin", "nexus", "0") == AURORA_AUTH_DENIED && HELPER(line, "userdel", "nexus") == AURORA_AUTH_DENIED);
    CHECK(HELPER(line, "userdel", "alice") == AURORA_AUTH_OK && !file_has("/etc/passwd", "alice:") && file_has("/etc/group", "sudo:x:27:nexus\n"));
    CHECK(HELPER("alicePw1\n", "verify", "alice") == AURORA_AUTH_UNKNOWN_USER);
    PASS("administrator: useradd (uid 1001, group, /home/alice 0700), admin grant, no self-demotion or self-deletion, userdel");
    puts("[64] AUTHTEST ALL PASSED"); fflush(stdout);
    return 0;
}
