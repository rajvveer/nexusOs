/* Session launcher campaign (tests/test_x64_sessions.py): /sbin/init on an ext4 root, as the desktop user (uid 1000,
 * pid 1, like Aurora). It drives /usr/libexec/nexus-session (set-user-ID root) through the Aurora protocol
 * (aurora_session_proto.h) and checks, with the same program as /bin/sessprobe inside the sessions:
 *   refusals: wrong password (tally), unknown/system/root/nologin/locked accounts, a home owned by someone else, bad
 *   names and session ids, an over-long password, fd 3 not a SEQPACKET socket or made by another process than pid 1,
 *   the desktop user itself, LD_PRELOAD ignored (AT_SECURE), five failures throttle the account;
 *   a session: uid/gid/supplementary groups, no way back to root, clean environment (HOME, USER, XDG_RUNTIME_DIR,
 *   XDG_SESSION_ID, LANG from /etc/default/locale, overrides), first-login home (0700, /etc/skel, XDG folders),
 *   /run/user/UID 0700 with the three sockets handed to pid 1 (clients connect, peer uid = the user), another user's
 *   files and runtime folder and /etc/shadow unreadable, a passwordless account;
 *   seat (prctl PR_NEXUS_SEAT): console devices for the console user only, readers of others revoked on a switch;
 *   hostile agent records (bad counts, relative path, bad env, unterminated, oversized, garbage, descriptors attached);
 *   logout (a daemonized process too): every process of the user gone, runtime folder emptied, "bye"; an agent whose
 *   display server closes the socket logs out by itself.
 * Prints "[64] SESSTEST PASS <name>" / "[64] SESSTEST FAIL <name>: why" and "[64] SESSTEST ALL PASSED". */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "aurora_auth.h"
#include "aurora_session_proto.h"

#define PR_NEXUS_SEAT 0x4E585301
static int failures;
static void fail(const char* name, const char* format, ...) {
    va_list list; va_start(list, format); printf("[64] SESSTEST FAIL %s: ", name); vprintf(format, list); printf("\n"); va_end(list); fflush(stdout); failures++;
}
static void pass(const char* name) { printf("[64] SESSTEST PASS %s\n", name); fflush(stdout); }
#define CHECK(name, cond, ...) do { if (cond) pass(name); else fail(name, __VA_ARGS__); } while (0)
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL); }

/* ---------------------------------------------------------------- probe (inside a session) */
static int probe_failures;
static void pfail(const char* format, ...) {
    va_list list; va_start(list, format); printf("[64] SESSTEST FAIL probe(%d): ", (int)getuid()); vprintf(format, list); printf("\n"); va_end(list); fflush(stdout); probe_failures++;
}
static void expect_env(const char* key, const char* value) {
    const char* v = getenv(key);
    if (!v || (value && strcmp(v, value))) pfail("env %s=%s (want %s)", key, v ? v : "(unset)", value ? value : "(set)");
}
static void expect_denied_open(const char* path, int flags) {
    int fd = open(path, flags | O_CLOEXEC);
    if (fd >= 0 || errno != EACCES) { pfail("%s readable (fd %d errno %d)", path, fd, errno); if (fd >= 0) close(fd); }
}
static bool connect_path(const char* path, int* error) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    bool ok = fd >= 0 && !connect(fd, (struct sockaddr*)&a, sizeof(a)); *error = ok ? 0 : errno;
    if (fd >= 0) close(fd); return ok;
}
/* probe <name> <uid> <gid> <supplementary gid|0> <session id> <first 0|1> <mode: full|other|input> */
static int probe(char** argv) {
    const char* name = argv[2]; unsigned uid = (unsigned)atoi(argv[3]), gid = (unsigned)atoi(argv[4]), extra = (unsigned)atoi(argv[5]);
    const char* id = argv[6]; bool first = atoi(argv[7]) != 0; const char* mode = argv[8];
    char home[64], run[64], path[160]; snprintf(home, sizeof(home), "/home/%s", name); snprintf(run, sizeof(run), "/run/user/%u", uid);
    if (!strcmp(mode, "input")) {   /* the console user may open the keyboard once pid 1 made it the console user */
        int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) pfail("console user cannot open /dev/input/event0: %s", strerror(errno)); else close(fd);
        return probe_failures ? 1 : 0;
    }
    if (getuid() != uid || geteuid() != uid || getgid() != gid || getegid() != gid) pfail("ids %u/%u %u/%u", getuid(), geteuid(), getgid(), getegid());
    gid_t groups[64]; int n = getgroups(64, groups); bool own = false, sup = !extra, sudo = false;
    for (int i = 0; i < n; i++) { own |= groups[i] == gid; sup |= extra && groups[i] == extra; sudo |= groups[i] == 27; }
    if (n < 0 || !own || !sup || sudo) pfail("supplementary groups (n=%d own=%d extra=%d sudo=%d)", n, own, sup, sudo);
    if (!setuid(0) || !setgid(0) || !seteuid(0)) pfail("regained root");
    expect_env("HOME", home); expect_env("USER", name); expect_env("LOGNAME", name); expect_env("SHELL", "/bin/bash");
    expect_env("XDG_RUNTIME_DIR", run); expect_env("XDG_SESSION_ID", id); expect_env("WAYLAND_DISPLAY", "wayland-0");
    expect_env("XDG_SESSION_TYPE", "wayland"); expect_env("XDG_SEAT", "seat0"); expect_env("PROBE_EXTRA", "override-ok");
    expect_env("LANG", "hi_IN.UTF-8"); expect_env("PATH", NULL);
    if (getenv("SESSTEST_LEAK")) pfail("the display server's environment leaked into the session");
    if (first) expect_env("NEXUS_FIRST_LOGIN", "1"); else if (getenv("NEXUS_FIRST_LOGIN")) pfail("NEXUS_FIRST_LOGIN on a later login");
    char cwd[256]; if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, home)) pfail("cwd %s", cwd);
    struct stat st;
    if (stat(home, &st) || !S_ISDIR(st.st_mode) || st.st_uid != uid || (st.st_mode & 0777) != 0700) pfail("home %s mode %o uid %u", home, st.st_mode & 0777, st.st_uid);
    snprintf(path, sizeof(path), "%s/.profile", home);
    if (stat(path, &st) || st.st_uid != uid) pfail("/etc/skel .profile not copied");
    snprintf(path, sizeof(path), "%s/.config/skeltest/hello.txt", home); FILE* f = fopen(path, "re"); char text[64] = "";
    if (!f || !fgets(text, sizeof(text), f) || strcmp(text, "skel says hello\n")) pfail("skel subfolder file: '%s'", text);
    if (f) fclose(f);
    static const char* const folders[] = {"Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", ".config/user-dirs.dirs"};
    for (unsigned i = 0; i < sizeof(folders) / sizeof(*folders); i++) { snprintf(path, sizeof(path), "%s/%s", home, folders[i]); if (stat(path, &st) || st.st_uid != uid) pfail("%s missing", path); }
    if (stat(run, &st) || !S_ISDIR(st.st_mode) || st.st_uid != uid || (st.st_mode & 0777) != 0700) pfail("runtime %s mode %o uid %u", run, st.st_mode & 0777, st.st_uid);
    static const char* const sockets[] = {"wayland-0", "aurora-control", "aurora-notify"};
    for (unsigned i = 0; i < 3; i++) { snprintf(path, sizeof(path), "%s/%s", run, sockets[i]); if (stat(path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != uid || (st.st_mode & 077)) pfail("socket %s", path); }
    int error; snprintf(path, sizeof(path), "%s/wayland-0", run);
    if (!connect_path(path, &error)) pfail("connect %s: %s", path, strerror(error));
    if (connect_path("/run/user/1000/wayland-0", &error) || error != EACCES) pfail("the desktop user's wayland socket reachable (errno %d)", error);
    expect_denied_open("/home/nexus/secret.txt", O_RDONLY); expect_denied_open("/etc/shadow", O_RDONLY);
    DIR* d = opendir("/home/nexus/private"); if (d || errno != EACCES) { pfail("/home/nexus/private listable"); if (d) closedir(d); }
    d = opendir("/run/user/1000"); if (d || errno != EACCES) { pfail("/run/user/1000 listable"); if (d) closedir(d); }
    if (!strcmp(mode, "other")) { expect_denied_open("/home/alice/Documents/probe.txt", O_RDONLY); d = opendir("/home/alice"); if (d || errno != EACCES) { pfail("/home/alice listable"); if (d) closedir(d); } }
    else {
        snprintf(path, sizeof(path), "%s/Documents/probe.txt", home);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); if (fd < 0 || write(fd, "mine\n", 5) != 5) pfail("cannot write %s", path);
        if (fd >= 0) close(fd);
    }
    int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0 || errno != EACCES) { pfail("console device open while not the console user (fd %d errno %d)", fd, errno); if (fd >= 0) close(fd); }
    printf("[64] SESSTEST PROBE %s uid=%u failures=%d\n", name, uid, probe_failures); fflush(stdout);
    return probe_failures ? 1 : 0;
}
static int linger(void) {   /* a session program and a daemon it leaves behind (setsid, parent exits) */
    pid_t pid = fork();
    if (!pid) { if (fork()) _exit(0); setsid(); for (;;) pause(); }
    if (pid > 0) waitpid(pid, NULL, 0);
    for (;;) pause();
}

/* ---------------------------------------------------------------- the launcher from pid 1 */
struct launch { pid_t pid; int fd; };
enum { FD3_SEQPACKET, FD3_PIPE, FD3_STREAM, FD3_CHILD_PAIR };
static struct launch start(const char* user, const char* password, size_t password_length, const char* id, int kind, const char* verb, bool preload) {
    struct launch l = {-1, -1}; int sv[2] = {-1, -1}, pw[2];
    if (kind == FD3_SEQPACKET || kind == FD3_STREAM) {
        if (socketpair(AF_UNIX, (kind == FD3_STREAM ? SOCK_STREAM : SOCK_SEQPACKET) | SOCK_CLOEXEC, 0, sv)) { fail("socketpair", "%s", strerror(errno)); return l; }
    } else if (kind == FD3_PIPE && pipe2(sv, O_CLOEXEC)) return l;
    if (pipe2(pw, O_CLOEXEC)) return l;
    pid_t pid = fork();
    if (!pid) {
        dup2(pw[0], 0);
        if (kind == FD3_CHILD_PAIR) { int mine[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, mine)) _exit(120); dup2(mine[1], 3); }
        else dup2(kind == FD3_PIPE ? sv[0] : sv[1], 3);
        char* env[] = {preload ? "LD_PRELOAD=/bin/authpre.so" : "SESSTEST_LEAK=1", "SESSTEST_LEAK=1", NULL};
        char* args[] = {"nexus-session", (char*)verb, (char*)user, (char*)id, NULL};
        execve(NEXUS_SESSION_PROGRAM, args, env); _exit(127);
    }
    close(pw[0]);
    if (password) { ssize_t w = write(pw[1], password, password_length); (void)w; }
    close(pw[1]);
    if (kind == FD3_SEQPACKET || kind == FD3_STREAM) { close(sv[1]); l.fd = sv[0]; } else if (kind == FD3_PIPE) { close(sv[0]); close(sv[1]); }
    l.pid = pid; return l;
}
/* One record (fields NUL-separated) within ms; fds receives up to 4 descriptors. Returns field count, 0 EOF, -1 timeout. */
static int receive(int fd, char* buffer, size_t size, char** fields, int max, int* fds, int* nfds, int ms) {
    struct pollfd p = {fd, POLLIN, 0}; if (nfds) *nfds = 0;
    if (poll(&p, 1, ms) <= 0) return -1;
    char control[CMSG_SPACE(sizeof(int) * 8)]; struct iovec iov = {buffer, size - 1};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    ssize_t n = recvmsg(fd, &m, MSG_CMSG_CLOEXEC); if (n <= 0) return 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)); int* got = (int*)CMSG_DATA(c);
            for (int i = 0; i < count; i++) { if (fds && nfds && *nfds < 4) fds[(*nfds)++] = got[i]; else close(got[i]); }
        }
    buffer[n] = 0; int count = 0;
    for (char* p2 = buffer; p2 < buffer + n && count < max; p2 += strlen(p2) + 1) fields[count++] = p2;
    return count;
}
static bool send_record(int fd, const char* const* fields, int count) {
    char buffer[8192]; size_t used = 0;
    for (int i = 0; i < count; i++) { size_t n = strlen(fields[i]) + 1; if (used + n > sizeof(buffer)) return false; memcpy(buffer + used, fields[i], n); used += n; }
    return send(fd, buffer, used, MSG_NOSIGNAL) == (ssize_t)used;
}
static int finish(pid_t pid, int ms) {   /* exit status of the launcher, -1 if still running */
    for (int t = 0; t < ms; t += 50) { int status; pid_t r = waitpid(pid, &status, WNOHANG); if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status); sleep_ms(50); }
    return -1;
}
static int auth_code(struct launch* l, int ms) {
    char buffer[512]; char* f[8]; int n = receive(l->fd, buffer, sizeof(buffer), f, 8, NULL, NULL, ms);
    if (n == 2 && !strcmp(f[0], "auth")) return atoi(f[1]);
    return n == 0 ? -2 : -1;
}
static void refused(const char* name, const char* user, const char* password, const char* id, int want) {
    char line[600]; snprintf(line, sizeof(line), "%s\n", password);
    struct launch l = start(user, line, strlen(line), id, FD3_SEQPACKET, "start", false);
    int code = auth_code(&l, 20000), status = finish(l.pid, 20000);
    CHECK(name, code == want && status == want, "auth record %d exit %d (want %d)", code, status, want);
    if (l.fd >= 0) close(l.fd);
}
struct session { struct launch l; int fds[4], nfds; pid_t agent; unsigned uid, gid; bool first; };
static bool open_session(const char* name, const char* password, const char* id, struct session* s) {
    memset(s, 0, sizeof(*s)); char line[128]; snprintf(line, sizeof(line), "%s\n", password);
    s->l = start(name, line, strlen(line), id, FD3_SEQPACKET, "start", false);
    int code = auth_code(&s->l, 30000);
    if (code) { fail("session", "%s: auth %d", name, code); return false; }
    char buffer[1024]; char* f[16]; int n = receive(s->l.fd, buffer, sizeof(buffer), f, 16, s->fds, &s->nfds, 30000);
    if (n != 7 || strcmp(f[0], "ready") || s->nfds != 3) { fail("session", "%s: ready record n=%d fds=%d (%s)", name, n, s->nfds, n > 0 ? f[0] : "-"); return false; }
    s->agent = atoi(f[1]); s->uid = (unsigned)atoi(f[2]); s->gid = (unsigned)atoi(f[3]); s->first = f[4][0] == '1';
    CHECK("ready record", s->agent == s->l.pid && f[6][0] == '/', "agent %d launcher %d runtime %s", (int)s->agent, (int)s->l.pid, f[6]);
    return true;
}
/* run a program in the session; returns its exit status (or -1) */
static int run_in(struct session* s, const char* tag, const char* const* argv, int argc, const char* const* env, int envc, int ms) {
    const char* fields[80]; int n = 0; char count[16], ecount[16];
    snprintf(count, sizeof(count), "%d", argc); snprintf(ecount, sizeof(ecount), "%d", envc);
    fields[n++] = "run"; fields[n++] = tag; fields[n++] = count;
    for (int i = 0; i < argc; i++) fields[n++] = argv[i];
    fields[n++] = ecount; for (int i = 0; i < envc; i++) fields[n++] = env[i];
    if (!send_record(s->l.fd, fields, n)) return -1;
    char buffer[512]; char* f[8]; pid_t pid = -1;
    for (int waited = 0; waited < ms; waited += 100) {
        int got = receive(s->l.fd, buffer, sizeof(buffer), f, 8, NULL, NULL, 100);
        if (got == 0) return -1;
        if (got == 3 && !strcmp(f[0], "started") && !strcmp(f[1], tag)) pid = atoi(f[2]);
        else if (got == 3 && !strcmp(f[0], "failed") && !strcmp(f[1], tag)) return -2;
        else if (got == 3 && !strcmp(f[0], "exit") && atoi(f[1]) == pid) { int st = atoi(f[2]); return WIFEXITED(st) ? WEXITSTATUS(st) : 128; }
    }
    return -1;
}
static bool ping(struct session* s) {
    const char* p[] = {"ping"}; if (!send_record(s->l.fd, p, 1)) return false;
    char buffer[512]; char* f[8];
    for (int i = 0; i < 50; i++) { int n = receive(s->l.fd, buffer, sizeof(buffer), f, 8, NULL, NULL, 200); if (n == 1 && !strcmp(f[0], "pong")) return true; if (n == 0) return false; }
    return false;
}
static int processes_of(unsigned uid) {
    DIR* d = opendir("/proc"); if (!d) return -1; struct dirent* e; int count = 0;
    while ((e = readdir(d))) {
        char* end; long pid = strtol(e->d_name, &end, 10); if (*end || pid <= 1) continue;
        char path[64], line[256]; snprintf(path, sizeof(path), "/proc/%ld/status", pid); FILE* f = fopen(path, "re"); if (!f) continue;
        while (fgets(line, sizeof(line), f)) { unsigned real; char state = 0;
            if (sscanf(line, "State: %c", &state) == 1 && state == 'Z') break;
            if (sscanf(line, "Uid: %u", &real) == 1) { count += real == uid; break; } }
        fclose(f);
    }
    closedir(d); return count;
}

int main(int argc, char** argv) {
    if (argc >= 9 && !strcmp(argv[1], "probe")) return probe(argv);
    if (argc >= 2 && !strcmp(argv[1], "linger")) return linger();
    if (argc >= 2 && !strcmp(argv[1], "sleep")) { sleep(600); return 0; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[64] SESSTEST START pid=%d uid=%d\n", (int)getpid(), (int)getuid());
    signal(SIGPIPE, SIG_IGN);
    /* ---- seat ---- */
    CHECK("seat: console user defaults to 1000", prctl(PR_NEXUS_SEAT, 0, 0, 0, 0) == 1000, "got %d", (int)prctl(PR_NEXUS_SEAT, 0, 0, 0, 0));
    CHECK("seat: invalid uid refused", prctl(PR_NEXUS_SEAT, 1, 0xFFFFFFFFUL, 0, 0) == -1 && errno == EINVAL, "errno %d", errno);
    CHECK("seat: unknown operation refused", prctl(PR_NEXUS_SEAT, 7, 0, 0, 0) == -1 && errno == EINVAL, "errno %d", errno);
    int report[2]; if (pipe(report)) return 1;
    pid_t keeper = fork();   /* a process of the desktop user (not pid 1) holding the keyboard open across a switch */
    if (!keeper) {
        close(report[0]); char result[8] = "xxxxx";
        int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC); result[0] = fd >= 0 ? 'o' : 'e';
        result[1] = prctl(PR_NEXUS_SEAT, 1, 1000, 0, 0) == -1 && errno == EPERM ? 'p' : 'P';
        if (write(report[1], result, 2) != 2) _exit(1);
        sleep_ms(1500);   /* pid 1 switches the console user meanwhile */
        char buf[64]; ssize_t r = read(fd, buf, sizeof(buf)); result[2] = r < 0 && errno == ENODEV ? 'r' : 'R';
        struct pollfd p = {fd, POLLIN, 0}; result[3] = poll(&p, 1, 0) == 1 ? 'q' : 'Q';
        int again = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC); result[4] = again < 0 && errno == EACCES ? 'a' : 'A';
        if (write(report[1], result + 2, 3) != 3) _exit(1);
        _exit(0);
    }
    close(report[1]); char first[2] = {0}, later[3] = {0};
    if (read(report[0], first, 2) != 2) fail("seat keeper", "no report");
    sleep_ms(300);
    int pid1_reader = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    CHECK("seat: pid 1 sets the console user", prctl(PR_NEXUS_SEAT, 1, 1001, 0, 0) == 0 && prctl(PR_NEXUS_SEAT, 0, 0, 0, 0) == 1001, "errno %d", errno);
    if (read(report[0], later, 3) != 3) fail("seat keeper", "no second report");
    waitpid(keeper, NULL, 0); close(report[0]);
    CHECK("seat: the desktop user's process could open the keyboard while console user", first[0] == 'o', "%c", first[0]);
    CHECK("seat: a process other than pid 1 cannot set the console user", first[1] == 'p', "%c", first[1]);
    CHECK("seat: its keyboard reader is revoked after the switch (ENODEV, poll ready)", later[0] == 'r' && later[1] == 'q', "%.2s", later);
    CHECK("seat: it cannot open the keyboard again", later[2] == 'a', "%c", later[2]);
    { char b[64]; ssize_t r = read(pid1_reader, b, sizeof(b)); CHECK("seat: pid 1's reader is never revoked", pid1_reader >= 0 && (r >= 0 || errno == EAGAIN), "fd %d r %zd errno %d", pid1_reader, r, errno); }
    struct stat st; CHECK("seat: device nodes owned by the console user", !stat("/dev/input/event0", &st) && st.st_uid == 1001, "uid %u", st.st_uid);
    prctl(PR_NEXUS_SEAT, 1, 1000, 0, 0);

    /* ---- refusals ---- */
    refused("wrong password refused (code 1)", "alice", "not-her-password", "2", AURORA_AUTH_WRONG);
    refused("unknown account refused", "zed", "x", "2", AURORA_AUTH_UNKNOWN_USER);
    refused("system account refused", "daemon1", "daemonPass9", "2", AURORA_AUTH_DENIED);
    refused("root refused", "root", "rootPass9", "2", AURORA_AUTH_DENIED);
    refused("nologin shell refused", "nolog", "nologPass9", "2", AURORA_AUTH_DENIED);
    refused("locked account refused", "carol", "carolPass9", "2", AURORA_AUTH_LOCKED);
    refused("home owned by someone else refused", "dave", "davePass99", "2", AURORA_AUTH_DENIED);
    refused("the desktop user itself refused", "nexus", "nexusPass1", "2", AURORA_AUTH_DENIED);
    refused("invalid user name refused", "Alice", "x", "2", AURORA_AUTH_INVALID);
    refused("path-like user name refused", "../etc", "x", "2", AURORA_AUTH_INVALID);
    refused("invalid session id refused", "alice", "alicePass1", "12a", AURORA_AUTH_INVALID);
    {   char big[700]; memset(big, 'a', sizeof(big));
        struct launch l = start("alice", big, sizeof(big), "2", FD3_SEQPACKET, "start", false);
        int code = auth_code(&l, 20000), status = finish(l.pid, 20000);
        CHECK("over-long password refused", code == AURORA_AUTH_INVALID && status == AURORA_AUTH_INVALID, "auth %d exit %d", code, status); if (l.fd >= 0) close(l.fd); }
    {   struct launch l = start("alice", "alicePass1\n", 11, "2", FD3_PIPE, "start", false); int status = finish(l.pid, 20000);
        CHECK("fd 3 a pipe: refused", status == AURORA_AUTH_INVALID, "exit %d", status); }
    {   struct launch l = start("alice", "alicePass1\n", 11, "2", FD3_STREAM, "start", false); int status = finish(l.pid, 20000);
        CHECK("fd 3 a stream socket: refused", status == AURORA_AUTH_INVALID, "exit %d", status); if (l.fd >= 0) close(l.fd); }
    {   struct launch l = start("alice", "alicePass1\n", 11, "2", FD3_CHILD_PAIR, "start", false); int status = finish(l.pid, 20000);
        CHECK("socket made by another process than pid 1: refused", status == AURORA_AUTH_DENIED, "exit %d", status); }
    {   struct launch l = start("alice", "alicePass1\n", 11, "2", FD3_SEQPACKET, "stop", false); int status = finish(l.pid, 20000);
        CHECK("unknown verb refused", status == AURORA_AUTH_INVALID, "exit %d", status); if (l.fd >= 0) close(l.fd); }
    {   unlink("/tmp/authpre-ran"); struct launch l = start("alice", "wrong\n", 6, "2", FD3_SEQPACKET, "start", true);
        auth_code(&l, 20000); finish(l.pid, 20000); if (l.fd >= 0) close(l.fd);
        CHECK("LD_PRELOAD ignored by the set-user-ID launcher (AT_SECURE)", access("/tmp/authpre-ran", F_OK) != 0, "the preload ran"); }
    for (int i = 0; i < 5; i++) { char line[] = "wrong-erin\n"; struct launch l = start("erin", line, strlen(line), "2", FD3_SEQPACKET, "start", false); auth_code(&l, 20000); finish(l.pid, 20000); if (l.fd >= 0) close(l.fd); }
    refused("five failures throttle the account (right password refused)", "erin", "erinPass99", "2", AURORA_AUTH_THROTTLED);

    /* ---- alice's session ---- */
    struct session alice;
    if (!open_session("alice", "alicePass1", "2", &alice)) { printf("[64] SESSTEST ABORT\n"); return 1; }
    CHECK("alice: uid/gid in the ready record, first login", alice.uid == 1001 && alice.gid == 1001 && alice.first, "uid %u gid %u first %d", alice.uid, alice.gid, alice.first);
    {   int type = 0, listening = 0; socklen_t len = sizeof(type);
        bool ok = !getsockopt(alice.fds[0], SOL_SOCKET, SO_TYPE, &type, &len) && type == SOCK_STREAM;
        len = sizeof(listening); ok = ok && !getsockopt(alice.fds[0], SOL_SOCKET, SO_ACCEPTCONN, &listening, &len) && listening;
        len = sizeof(type); ok = ok && !getsockopt(alice.fds[2], SOL_SOCKET, SO_TYPE, &type, &len) && type == SOCK_DGRAM;
        CHECK("alice: listening wayland/control sockets and the notify socket handed to pid 1", ok, "type %d listening %d", type, listening); }
    const char* probe_args[] = {"/bin/sessprobe", "probe", "alice", "1001", "1001", "2001", "2", "1", "full"};
    const char* env[] = {"PROBE_EXTRA=override-ok", "LANG=hi_IN.UTF-8"};
    int status = run_in(&alice, "probe1", probe_args, 9, env, 1, 30000);
    CHECK("alice: the session probe passes (ids, groups, env, home, runtime, isolation)", status == 0, "status %d", status);
    {   int c = accept4(alice.fds[0], NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC); struct ucred cred = {0}; socklen_t len = sizeof(cred);
        bool ok = c >= 0 && !getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) && cred.uid == 1001;
        CHECK("alice: pid 1 accepts her client on the handed-over socket (peer uid 1001)", ok, "fd %d uid %u", c, cred.uid); if (c >= 0) close(c); }
    prctl(PR_NEXUS_SEAT, 1, 1001, 0, 0);
    const char* input_args[] = {"/bin/sessprobe", "probe", "alice", "1001", "1001", "2001", "2", "1", "input"};
    status = run_in(&alice, "probe2", input_args, 9, NULL, 0, 30000);
    CHECK("alice: as console user she may open the keyboard", status == 0, "status %d", status);
    prctl(PR_NEXUS_SEAT, 1, 1000, 0, 0);
    /* hostile records: the agent answers every one and stays usable */
    {   const char* a1[] = {"run", "t1"}; const char* a2[] = {"run", "t2", "1", "relative/path", "0"}; const char* a3[] = {"run", "t3", "0", "0"};
        const char* a4[] = {"run", "t4", "99999", "/bin/sessprobe", "0"}; const char* a5[] = {"run", "t5", "1", "/bin/sessprobe", "1", "NOEQUALS"};
        const char* a6[] = {"run", "t6", "1", "/bin/sessprobe", "1", "=x"}; const char* a7[] = {"fly"}; const char* a8[] = {"run", "t8", "1", "/bin/sessprobe", "5", "A=1"};
        const char* const* all[] = {a1, a2, a3, a4, a5, a6, a7, a8}; int counts[] = {2, 5, 4, 5, 6, 6, 1, 6}; int answered = 0;
        for (int i = 0; i < 8; i++) { if (!send_record(alice.l.fd, all[i], counts[i])) continue; char b[512]; char* f[8]; int n = receive(alice.l.fd, b, sizeof(b), f, 8, NULL, NULL, 5000);
            if (n >= 1 && (!strcmp(f[0], "failed") || !strcmp(f[0], "error"))) answered++; else printf("[64] SESSTEST note: hostile %d answered %s\n", i, n > 0 ? f[0] : "-"); }
        CHECK("agent: malformed run requests refused", answered == 8, "%d of 8 answered with failed/error", answered);
        char junk[1024]; for (unsigned i = 0; i < sizeof(junk); i++) junk[i] = (char)(i * 37 + 11);
        int sent = 0; if (send(alice.l.fd, junk, sizeof(junk), MSG_NOSIGNAL) == (ssize_t)sizeof(junk)) sent++;   /* unterminated garbage */
        static char huge[NEXUS_SESSION_RECORD + 4096]; memset(huge, 'x', sizeof(huge)); huge[sizeof(huge) - 1] = 0;
        if (send(alice.l.fd, huge, sizeof(huge), MSG_NOSIGNAL) == (ssize_t)sizeof(huge)) sent++;   /* longer than a record */
        {   int devnull = open("/dev/null", O_RDONLY); char rec[] = "ping"; struct iovec iov = {rec, 5}; char control[CMSG_SPACE(sizeof(int))];
            struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
            struct cmsghdr* c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &devnull, sizeof(int));
            if (sendmsg(alice.l.fd, &m, MSG_NOSIGNAL) == 5) sent++; close(devnull); }
        char b[512]; char* f[8]; int replies = 0; while (receive(alice.l.fd, b, sizeof(b), f, 8, NULL, NULL, 2000) > 0) replies++;
        CHECK("agent: garbage, an over-long record and a record with a descriptor do not break it", ping(&alice), "no pong (sent %d, replies %d)", sent, replies);
    }

    /* ---- bob: no password; cannot read alice's files ---- */
    struct session bob;
    if (open_session("bob", "", "3", &bob)) {
        const char* bob_args[] = {"/bin/sessprobe", "probe", "bob", "1002", "1002", "0", "3", "1", "other"};
        const char* bob_env[] = {"PROBE_EXTRA=override-ok"};
        status = run_in(&bob, "bprobe", bob_args, 9, bob_env, 1, 30000);
        CHECK("bob (no password): session works and alice's files are unreachable", status == 0, "status %d", status);
    }

    /* ---- logout ---- */
    const char* linger_args[] = {"/bin/sessprobe", "linger"};
    const char* fields[] = {"run", "linger", "2", linger_args[0], linger_args[1], "0"}; send_record(alice.l.fd, fields, 6);
    sleep_ms(1500); int before = processes_of(1001);
    CHECK("alice: lingering processes running before logout", before >= 3, "%d processes", before);
    { const char* out[] = {"logout"}; send_record(alice.l.fd, out, 1); }
    bool bye = false; char b[512]; char* f[8];
    for (int i = 0; i < 300 && !bye; i++) { int n = receive(alice.l.fd, b, sizeof(b), f, 8, NULL, NULL, 100); if (n == 1 && !strcmp(f[0], "bye")) bye = true; if (n == 0) break; }
    int agent_status = finish(alice.agent, 10000); while (waitpid(-1, NULL, WNOHANG) > 0) {}   /* the daemon was reparented to us */
    int after = processes_of(1001);
    CHECK("alice: logout ends every process of hers (the daemon too) and says bye", bye && agent_status == 0 && after == 0, "bye %d agent %d left %d", bye, agent_status, after);
    { DIR* d = opendir("/run/user/1001"); int entries = 0; struct dirent* e; while (d && (e = readdir(d))) entries += e->d_name[0] != '.';
      if (d) closedir(d); CHECK("alice: runtime folder emptied at logout", entries == 0, "%d entries left", entries); }
    for (int i = 0; i < alice.nfds; i++) close(alice.fds[i]);
    close(alice.l.fd);
    if (bob.l.fd > 0) {
        close(bob.l.fd); int st2 = finish(bob.agent, 15000); while (waitpid(-1, NULL, WNOHANG) > 0) {}
        CHECK("bob: the agent logs out when the display server closes its socket", st2 == 0 && processes_of(1002) == 0, "agent %d left %d", st2, processes_of(1002));
        for (int i = 0; i < bob.nfds; i++) close(bob.fds[i]);
    }
    /* second login of alice: not a first login any more */
    struct session again;
    if (open_session("alice", "alicePass1", "4", &again)) {
        CHECK("alice: second login is not a first login", !again.first, "first %d", again.first);
        const char* args2[] = {"/bin/sessprobe", "probe", "alice", "1001", "1001", "2001", "4", "0", "full"};
        const char* env2[] = {"PROBE_EXTRA=override-ok"};
        status = run_in(&again, "probe3", args2, 9, env2, 1, 30000);
        CHECK("alice: second session probe", status == 0, "status %d", status);
        const char* out[] = {"logout"}; send_record(again.l.fd, out, 1); finish(again.agent, 15000); close(again.l.fd);
        for (int i = 0; i < again.nfds; i++) close(again.fds[i]);
    }
    printf(failures ? "[64] SESSTEST FAILED %d\n" : "[64] SESSTEST ALL PASSED\n", failures); fflush(stdout);
    sync();
    return 0;
}
