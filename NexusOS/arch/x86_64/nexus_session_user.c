/* nexus-session: the privileged session launcher and per-user session agent (/usr/libexec/nexus-session, set-user-ID
 * root on ext4 roots). Protocol and call contract: aurora_session_proto.h.
 *
 *   nexus-session start USER SESSION-ID     fd 0: the password line; fd 3: SOCK_SEQPACKET socketpair made by pid 1
 *
 * As root (only): refuse unless fd 3 is a SEQPACKET socket whose peer is pid 1 (the display server: a socketpair carries
 * its creator's credentials, so nobody else can hand one in), check the password with the account helper
 * (/usr/libexec/nexus-auth verify: failure tally, two-second delay; in this process only when the helper is missing),
 * refuse system accounts, root, locked accounts, nologin shells and homes that belong to someone else; create the home
 * (0700, first login) and /run/user/UID (0700) below root-owned folders. Then it becomes the user for good
 * (initgroups, setresgid, setresuid; regaining root is checked to fail) and, as the user: copies /etc/skel and makes the
 * XDG user folders at the first login, binds the session's sockets in its runtime folder (so no root process ever
 * touches a path the user controls), starts the user's D-Bus session bus (nexusd "session UID"), sends them to Aurora
 * and serves as the session agent until logout (all session programs are its children; logout = the user's session
 * units stopped, SIGTERM then SIGKILL to every process of the user, the runtime folder emptied). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "aurora_auth.h"
#include "aurora_session_proto.h"
#include "nexusd_proto.h"

static const int ctl = NEXUS_SESSION_FD;
static struct aurora_user user;
static char runtime[64];
static int chld_pipe[2] = {-1, -1};

/* ---- records ---- */
static bool send_fields(const int* fds, unsigned nfds, const char* verb, ...) {
    static char buffer[4096]; size_t used = 0; va_list list; va_start(list, verb);
    for (const char* field = verb; field; field = va_arg(list, const char*)) {
        size_t n = strlen(field) + 1; if (used + n > sizeof(buffer)) { va_end(list); return false; }
        memcpy(buffer + used, field, n); used += n;
    }
    va_end(list);
    struct iovec iov = {buffer, used}; struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
    char control[CMSG_SPACE(sizeof(int) * 4)]; memset(control, 0, sizeof(control));
    if (nfds) {
        m.msg_control = control; m.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr* c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }
    ssize_t n; while ((n = sendmsg(ctl, &m, MSG_NOSIGNAL)) < 0 && errno == EINTR) {}
    return n == (ssize_t)used;
}
static void say_auth(int code) { char text[16]; snprintf(text, sizeof(text), "%d", code); send_fields(NULL, 0, "auth", text, (char*)NULL); }
static void say_number(const char* verb, long a, long b) {
    char x[24], y[24]; snprintf(x, sizeof(x), "%ld", a); snprintf(y, sizeof(y), "%ld", b); send_fields(NULL, 0, verb, x, y, (char*)NULL);
}
static void say_tag(const char* verb, const char* tag, long value) { char text[24]; snprintf(text, sizeof(text), "%ld", value); send_fields(NULL, 0, verb, tag, text, (char*)NULL); }
static void note(const char* format, ...) {   /* the serial console (Aurora's stderr): tests and the journal read it */
    char line[512]; int n = snprintf(line, sizeof(line), "[64] NEXUS-SESSION "); va_list list; va_start(list, format);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, list); va_end(list);
    size_t length = m < 0 ? (size_t)n : strlen(line);
    line[length++] = '\n'; if (write(2, line, length) < 0) {}   /* one write: other processes' lines do not cut in */
}

/* ---- root part ---- */
static bool valid_name(const char* name) {
    if (!name[0] || strlen(name) > 31 || !(islower((unsigned char)name[0]) || name[0] == '_')) return false;
    for (const char* p = name; *p; p++) if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    return true;
}
static int read_password(char* out, size_t size) {   /* one line from fd 0, bounded */
    size_t used = 0; ssize_t n;
    while (used < size - 1 && (n = read(0, out + used, size - 1 - used)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        used += (size_t)n; if (memchr(out, '\n', used)) break;
    }
    out[used] = 0; char* end = strchr(out, '\n');
    if (!end && used == size - 1) return -1;   /* longer than any password we accept */
    if (end) *end = 0;
    return 0;
}
/* The account helper (failure tally + delay); AURORA_AUTH_UNAVAILABLE when it cannot run. */
static int helper_verify(const char* name, const char* password) {
    struct stat st; if (stat(AURORA_HELPER, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) return AURORA_AUTH_UNAVAILABLE;
    int in[2]; if (pipe2(in, O_CLOEXEC)) return AURORA_AUTH_UNAVAILABLE;
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); return AURORA_AUTH_UNAVAILABLE; }
    if (!pid) {
        dup2(in[0], 0); int devnull = open("/dev/null", O_WRONLY); if (devnull >= 0) dup2(devnull, 1);
        char* const env[] = {NULL}; char* const args[] = {"nexus-auth", "verify", (char*)name, NULL};
        execve(AURORA_HELPER, args, env); _exit(AURORA_AUTH_UNAVAILABLE);
    }
    close(in[0]);
    size_t length = strlen(password); ssize_t wrote = write(in[1], password, length); ssize_t nl = write(in[1], "\n", 1); close(in[1]);
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (wrote != (ssize_t)length || nl != 1 || !WIFEXITED(status)) return AURORA_AUTH_UNAVAILABLE;
    return WEXITSTATUS(status);
}
static bool login_shell(const char* shell) {
    static const char* const refused[] = {"/usr/sbin/nologin", "/sbin/nologin", "/bin/false", "/usr/bin/false"};
    for (unsigned i = 0; i < sizeof(refused) / sizeof(*refused); i++) if (!strcmp(shell, refused[i])) return false;
    return true;
}
static bool safe_parent(const char* path) {   /* root-owned, not writable by others: nobody else can swap entries in it */
    char parent[PATH_MAX]; snprintf(parent, sizeof(parent), "%s", path);
    char* slash = strrchr(parent, '/'); if (!slash) return false;
    if (slash == parent) slash[1] = 0; else *slash = 0;
    struct stat st; return !lstat(parent, &st) && S_ISDIR(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022);
}
static int prepare_home(bool* created) {
    *created = false;
    if (user.home[0] != '/' || strstr(user.home, "/..") || strstr(user.home, "//") || strlen(user.home) < 2) return EINVAL;
    struct stat st;
    if (!lstat(user.home, &st)) return S_ISDIR(st.st_mode) && st.st_uid == user.uid ? 0 : EPERM;
    if (errno != ENOENT) return errno;
    if (!safe_parent(user.home)) return EPERM;
    if (mkdir(user.home, 0700)) return errno;
    if (lchown(user.home, user.uid, user.gid)) { int error = errno; rmdir(user.home); return error; }
    *created = true; return 0;
}
static int prepare_runtime(void) {
    mkdir("/run", 0755); mkdir("/run/user", 0755);
    struct stat st;
    if (lstat("/run/user", &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) return EPERM;
    snprintf(runtime, sizeof(runtime), "/run/user/%u", user.uid);
    if (!lstat(runtime, &st)) {
        if (!S_ISDIR(st.st_mode) || st.st_uid != user.uid) return EPERM;
        return chmod(runtime, 0700) ? errno : 0;
    }
    if (errno != ENOENT) return errno;
    if (mkdir(runtime, 0700)) return errno;
    if (lchown(runtime, user.uid, user.gid)) { int error = errno; rmdir(runtime); return error; }
    return 0;
}
static void drop_privileges(void) {
    if (initgroups(user.name, user.gid) || setresgid(user.gid, user.gid, user.gid) || setresuid(user.uid, user.uid, user.uid)) {
        note("cannot become %s: %s", user.name, strerror(errno)); say_auth(AURORA_AUTH_UNAVAILABLE); _exit(AURORA_AUTH_UNAVAILABLE);
    }
    uid_t r, e, s; gid_t gr, ge, gs;
    if (!setuid(0) || !seteuid(0) || getresuid(&r, &e, &s) || r != user.uid || e != user.uid || s != user.uid ||
        getresgid(&gr, &ge, &gs) || gr != user.gid || ge != user.gid || gs != user.gid) { note("privileges still held"); _exit(AURORA_AUTH_UNAVAILABLE); }
}

/* ---- user part ---- */
static unsigned copied;
static void copy_tree(int from, int to, unsigned depth) {   /* /etc/skel: folders, files (<= 16 MiB), links; never overwrites */
    DIR* d = fdopendir(from); if (!d) { close(from); return; }
    struct dirent* e;
    while ((e = readdir(d)) && copied < 2000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st; if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        if (S_ISDIR(st.st_mode) && depth < 8) {
            if (mkdirat(to, e->d_name, (st.st_mode & 0777) | 0700) && errno != EEXIST) continue;
            int a = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            int b = openat(to, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (a >= 0 && b >= 0) copy_tree(a, b, depth + 1); else if (a >= 0) close(a);
            if (b >= 0) close(b);
        } else if (S_ISREG(st.st_mode) && st.st_size <= (16 << 20)) {
            int a = openat(dirfd(d), e->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (a < 0) continue;
            int b = openat(to, e->d_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, st.st_mode & 0777);
            if (b >= 0) { char buf[16384]; ssize_t n; while ((n = read(a, buf, sizeof(buf))) > 0) if (write(b, buf, (size_t)n) != n) break; close(b); copied++; }
            close(a);
        } else if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX]; ssize_t n = readlinkat(dirfd(d), e->d_name, target, sizeof(target) - 1);
            if (n > 0) { target[n] = 0; if (!symlinkat(target, to, e->d_name)) copied++; }
        }
    }
    closedir(d);
}
static const char* const XDG_DIRS[][2] = {{"DESKTOP", "Desktop"}, {"DOWNLOAD", "Downloads"}, {"TEMPLATES", "Templates"}, {"PUBLICSHARE", "Public"},
                                          {"DOCUMENTS", "Documents"}, {"MUSIC", "Music"}, {"PICTURES", "Pictures"}, {"VIDEOS", "Videos"}};
static void first_login(void) {
    int skel = open("/etc/skel", O_RDONLY | O_DIRECTORY | O_CLOEXEC), home = open(user.home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (skel >= 0 && home >= 0) copy_tree(skel, home, 0); else if (skel >= 0) close(skel);
    if (home < 0) return;
    for (unsigned i = 0; i < sizeof(XDG_DIRS) / sizeof(*XDG_DIRS); i++) mkdirat(home, XDG_DIRS[i][1], 0755);
    mkdirat(home, ".config", 0700);
    int conf = openat(home, ".config/user-dirs.dirs", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (conf >= 0) {
        FILE* f = fdopen(conf, "w");
        if (f) { fprintf(f, "# NexusOS first login (xdg-user-dirs format)\n");
            for (unsigned i = 0; i < sizeof(XDG_DIRS) / sizeof(*XDG_DIRS); i++) fprintf(f, "XDG_%s_DIR=\"$HOME/%s\"\n", XDG_DIRS[i][0], XDG_DIRS[i][1]);
            fclose(f); } else close(conf);
    }
    close(home);
}
static int listener(const char* name, int type, int backlog) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if ((size_t)snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", runtime, name) >= sizeof(a.sun_path)) return -1;
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0); if (fd < 0) return -1;
    unlink(a.sun_path);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) || chmod(a.sun_path, 0600) || (backlog && listen(fd, backlog))) { int error = errno; close(fd); errno = error; return -1; }
    return fd;
}
static void nexusd_request(const char* format, unsigned uid) {   /* one request per connection (nexusd_proto.h) */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (fd < 0) return;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "%s", NEXUSD_CONTROL);
    struct timeval tv = {5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (!connect(fd, (struct sockaddr*)&a, sizeof(a))) {
        char line[96]; int n = snprintf(line, sizeof(line), format, uid);
        if (write(fd, line, (size_t)n) == n) { char reply[256]; while (read(fd, reply, sizeof(reply)) > 0) {} }
    }
    close(fd);
}
/* the user's session units: D-Bus session bus (and a user manager where nexusd has one) */
static void session_bus(void) { if (!access("/usr/bin/dbus-daemon", X_OK) || !access("/usr/lib/nexus/nexusd", X_OK)) nexusd_request("session %u\n", user.uid); }
static void nexusd_stop_session_units(void) { nexusd_request("session-end %u\n", user.uid); nexusd_request("stop dbus-session@%u.service\n", user.uid); }
static char** session_env;   /* the clean environment every session program starts from */
static unsigned env_count, env_capacity;
static void env_put(const char* key, const char* value) {
    if (env_count + 2 > env_capacity) { env_capacity = env_capacity ? env_capacity * 2 : 32; session_env = realloc(session_env, env_capacity * sizeof(char*)); if (!session_env) _exit(AURORA_AUTH_UNAVAILABLE); }
    size_t k = strlen(key);
    for (unsigned i = 0; i < env_count; i++) if (!strncmp(session_env[i], key, k) && session_env[i][k] == '=') { free(session_env[i]); session_env[i] = NULL; if (asprintf(&session_env[i], "%s=%s", key, value) < 0) _exit(1); return; }
    if (asprintf(&session_env[env_count], "%s=%s", key, value) < 0) _exit(AURORA_AUTH_UNAVAILABLE);
    session_env[++env_count] = NULL;
}
static void build_env(const char* session_id, bool first) {
    char text[PATH_MAX + 64];
    env_put("HOME", user.home); env_put("USER", user.name); env_put("LOGNAME", user.name); env_put("SHELL", user.shell[0] ? user.shell : "/bin/sh");
    env_put("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/games");
    env_put("LANG", "C.UTF-8");
    FILE* f = fopen("/etc/default/locale", "re");
    if (f) { char line[256]; while (fgets(line, sizeof(line), f)) { char* s = line; while (isspace((unsigned char)*s)) s++; s[strcspn(s, "\r\n")] = 0;
            char* eq = strchr(s, '='); if (!eq || (strncmp(s, "LANG=", 5) && strncmp(s, "LC_", 3) && strncmp(s, "LANGUAGE=", 9))) continue;
            *eq = 0; char* v = eq + 1; size_t vl = strlen(v); if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }
            bool ok = true; for (char* p = s; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_')) ok = false;
            if (ok) env_put(s, v); }
        fclose(f); }
    env_put("XDG_RUNTIME_DIR", runtime); env_put("WAYLAND_DISPLAY", "wayland-0");
    env_put("XDG_SESSION_TYPE", "wayland"); env_put("XDG_SESSION_CLASS", "user"); env_put("XDG_SESSION_DESKTOP", "aurora");
    env_put("XDG_CURRENT_DESKTOP", "NexusOS"); env_put("XDG_SEAT", "seat0"); env_put("XDG_SESSION_ID", session_id);
    if (!access("/usr/bin/dbus-daemon", X_OK)) { snprintf(text, sizeof(text), "unix:path=%s/bus", runtime); env_put("DBUS_SESSION_BUS_ADDRESS", text); }
    if (first) env_put("NEXUS_FIRST_LOGIN", "1");
}

/* ---- agent ---- */
static void child_signal(int sig) { (void)sig; int saved = errno; if (write(chld_pipe[1], "c", 1) < 0) {} errno = saved; }
static void report_exits(void) {
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) say_number("exit", pid, status);
}
static void run_program(const char* tag, char** argv, char** env, unsigned envc) {
    pid_t pid = fork();
    if (pid < 0) { say_tag("failed", tag, errno); return; }
    if (!pid) {
        setsid();
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < NSIG; s++) signal(s, SIG_DFL);
        close(ctl); close(chld_pipe[0]); close(chld_pipe[1]);
        if (chdir(user.home)) { if (chdir("/")) {} }
        char** final = calloc(env_count + envc + 1, sizeof(char*)); if (!final) _exit(127);
        unsigned n = 0; for (unsigned i = 0; i < env_count; i++) final[n++] = session_env[i];
        for (unsigned i = 0; i < envc; i++) {   /* overrides replace a session variable of the same name */
            size_t k = strcspn(env[i], "=");
            for (unsigned j = 0; j < n; j++) if (!strncmp(final[j], env[i], k + 1)) { final[j] = final[--n]; break; }
            final[n++] = env[i];
        }
        final[n] = NULL;
        execve(argv[0], argv, final); _exit(127);
    }
    say_tag("started", tag, pid);
}
static bool user_processes_left(void) {
    DIR* d = opendir("/proc"); if (!d) return false;
    struct dirent* e; bool found = false;
    while (!found && (e = readdir(d))) {
        char* end; long pid = strtol(e->d_name, &end, 10); if (*end || pid <= 1 || pid == (long)getpid()) continue;
        char path[64], line[256]; snprintf(path, sizeof(path), "/proc/%ld/status", pid);
        FILE* f = fopen(path, "re"); if (!f) continue;
        while (fgets(line, sizeof(line), f)) { unsigned real; char state = 0;
            if (sscanf(line, "State: %c", &state) == 1 && state == 'Z') break;   /* already gone, its parent has not reaped it */
            if (sscanf(line, "Uid: %u", &real) == 1) { found = real == user.uid; break; } }
        fclose(f);
    }
    closedir(d); return found;
}
static void remove_tree(int dir, unsigned depth) {
    DIR* d = fdopendir(dir); if (!d) { close(dir); return; }
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st; if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        if (S_ISDIR(st.st_mode) && depth < 16) {
            int sub = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub >= 0) remove_tree(sub, depth + 1);
            unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR);
        } else unlinkat(dirfd(d), e->d_name, 0);
    }
    closedir(d);
}
static void logout(const char* why) {
    note("logout user=%s reason=%s", user.name, why);
    nexusd_stop_session_units();
    signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
    kill(-1, SIGTERM); kill(-1, SIGCONT);   /* every process of this user (kill(-1) spares init and the caller) */
    for (int i = 0; i < 50 && user_processes_left(); i++) { report_exits(); struct timespec t = {0, 100000000}; nanosleep(&t, NULL); }
    if (user_processes_left()) { note("SIGKILL to what is left of %s", user.name); kill(-1, SIGKILL); }
    for (int i = 0; i < 20 && user_processes_left(); i++) { report_exits(); struct timespec t = {0, 100000000}; nanosleep(&t, NULL); }
    report_exits();
    int dir = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); if (dir >= 0) remove_tree(dir, 0);   /* XDG: removed at the last logout */
    send_fields(NULL, 0, "bye", (char*)NULL);
}
static void handle_record(char* data, size_t size) {
    if (!size || data[size - 1]) { send_fields(NULL, 0, "error", "unterminated record", (char*)NULL); return; }
    char* fields[NEXUS_SESSION_MAX_ARGS + NEXUS_SESSION_MAX_ENV + 8]; unsigned count = 0;
    for (char* p = data; p < data + size && count < sizeof(fields) / sizeof(*fields); p += strlen(p) + 1) fields[count++] = p;
    if (!strcmp(fields[0], "ping") && count == 1) { send_fields(NULL, 0, "pong", (char*)NULL); return; }
    if (!strcmp(fields[0], "logout") && count == 1) { logout("request"); _exit(0); }
    if (!strcmp(fields[0], "run") && count >= 4) {
        const char* tag = fields[1]; char* end; unsigned long argc = strtoul(fields[2], &end, 10);
        if (*end || !argc || argc > NEXUS_SESSION_MAX_ARGS || 3 + argc >= count) { send_fields(NULL, 0, "failed", tag, "22", (char*)NULL); return; }
        unsigned long envc = strtoul(fields[3 + argc], &end, 10);
        if (*end || envc > NEXUS_SESSION_MAX_ENV || 4 + argc + envc != count || fields[3][0] != '/') { send_fields(NULL, 0, "failed", tag, "22", (char*)NULL); return; }
        char* argv[NEXUS_SESSION_MAX_ARGS + 1]; for (unsigned long i = 0; i < argc; i++) argv[i] = fields[3 + i]; argv[argc] = NULL;
        char** env = fields + 4 + argc;
        for (unsigned long i = 0; i < envc; i++) if (!strchr(env[i], '=') || env[i][0] == '=') { send_fields(NULL, 0, "failed", tag, "22", (char*)NULL); return; }
        run_program(tag, argv, env, (unsigned)envc); return;
    }
    send_fields(NULL, 0, "error", "unknown request", (char*)NULL);
}
static void agent(void) {
    static char record[NEXUS_SESSION_RECORD + 1];
    for (;;) {
        struct pollfd p[2] = {{ctl, POLLIN, 0}, {chld_pipe[0], POLLIN, 0}};
        if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; logout("poll"); _exit(1); }
        if (p[1].revents) { char b[64]; while (read(chld_pipe[0], b, sizeof(b)) > 0) {} report_exits(); }
        if (p[0].revents & POLLIN) {
            char control[CMSG_SPACE(sizeof(int) * 16)]; struct iovec iov = {record, NEXUS_SESSION_RECORD};
            struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
            ssize_t n = recvmsg(ctl, &m, MSG_CMSG_CLOEXEC);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            for (struct cmsghdr* c = n >= 0 ? CMSG_FIRSTHDR(&m) : NULL; c; c = CMSG_NXTHDR(&m, c))   /* nobody sends us descriptors */
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) { int* fd = (int*)CMSG_DATA(c); for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) close(fd[i]); }
            if (n <= 0) { logout("display server gone"); _exit(0); }
            if (m.msg_flags & MSG_TRUNC) { send_fields(NULL, 0, "error", "record too long", (char*)NULL); continue; }
            handle_record(record, (size_t)n);
        } else if (p[0].revents & (POLLHUP | POLLERR)) { logout("display server gone"); _exit(0); }
    }
}

int main(int argc, char** argv) {
    umask(022);
    if (argc != 4 || strcmp(argv[1], "start")) { fprintf(stderr, "usage: nexus-session start USER SESSION-ID (from the display server)\n"); return AURORA_AUTH_INVALID; }
    struct stat st; int type = 0; socklen_t length = sizeof(type); struct ucred peer; socklen_t plen = sizeof(peer);
    if (fstat(ctl, &st) || !S_ISSOCK(st.st_mode) || getsockopt(ctl, SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_SEQPACKET) {
        fprintf(stderr, "nexus-session: fd 3 is not the display server's session socket\n"); return AURORA_AUTH_INVALID; }
    fcntl(ctl, F_SETFD, FD_CLOEXEC);
    if (getsockopt(ctl, SOL_SOCKET, SO_PEERCRED, &peer, &plen) || peer.pid != 1) { note("refused: not started by the display server (peer pid %d)", (int)peer.pid); say_auth(AURORA_AUTH_DENIED); return AURORA_AUTH_DENIED; }
    const char* name = argv[2]; const char* session_id = argv[3];
    bool digits = session_id[0] && strlen(session_id) <= 9; for (const char* p = session_id; *p; p++) if (!isdigit((unsigned char)*p)) digits = false;
    if (!valid_name(name) || !digits) { say_auth(AURORA_AUTH_INVALID); return AURORA_AUTH_INVALID; }
    if (geteuid() != 0) { note("not privileged (no set-user-ID root: FAT root?)"); say_auth(AURORA_AUTH_UNAVAILABLE); return AURORA_AUTH_UNAVAILABLE; }
    char password[512]; int code = AURORA_AUTH_OK;
    if (read_password(password, sizeof(password))) code = AURORA_AUTH_INVALID;
    int devnull = open("/dev/null", O_RDONLY); if (devnull >= 0) { dup2(devnull, 0); if (devnull) close(devnull); }
    if (!code && !aurora_user_find(name, &user)) code = getpwnam(name) ? AURORA_AUTH_DENIED : AURORA_AUTH_UNKNOWN_USER;   /* system accounts are not listed */
    if (!code && (user.uid < 1000 || user.uid >= 60000 || user.uid == getuid() || !login_shell(user.shell))) code = AURORA_AUTH_DENIED;
    if (!code && user.password == AURORA_PW_LOCKED) code = AURORA_AUTH_LOCKED;
    if (!code) {
        code = helper_verify(name, password);
        if (code == AURORA_AUTH_UNAVAILABLE) { code = aurora_auth_verify(name, password); if (code == AURORA_AUTH_WRONG) sleep(2); }
        if (code == AURORA_AUTH_NO_PASSWORD) code = AURORA_AUTH_OK;   /* no password: the seat's user pressed Enter (pid 1 only) */
    }
    memset(password, 0, sizeof(password));
    if (code) { note("sign-in refused user=%s code=%d", name, code); say_auth(code); return code; }
    bool created = false; int error = prepare_home(&created);
    if (!error) error = prepare_runtime();
    if (error) { note("cannot prepare %s: %s", user.name, strerror(error)); say_auth(AURORA_AUTH_DENIED); return AURORA_AUTH_DENIED; }
    drop_privileges();
    if (clearenv()) return AURORA_AUTH_UNAVAILABLE;
    if (chdir(user.home)) { note("home %s: %s", user.home, strerror(errno)); say_auth(AURORA_AUTH_DENIED); return AURORA_AUTH_DENIED; }
    if (created) first_login();
    say_auth(AURORA_AUTH_OK);
    build_env(session_id, created);
    int fds[3] = {listener("wayland-0", SOCK_STREAM, 64), listener("aurora-control", SOCK_STREAM, 8), listener("aurora-notify", SOCK_DGRAM, 0)};
    if (fds[0] < 0 || fds[1] < 0 || fds[2] < 0) { note("sockets in %s: %s", runtime, strerror(errno)); return AURORA_AUTH_UNAVAILABLE; }
    session_bus();
    if (pipe2(chld_pipe, O_CLOEXEC | O_NONBLOCK)) return AURORA_AUTH_UNAVAILABLE;
    struct sigaction action = {.sa_handler = child_signal, .sa_flags = SA_RESTART | SA_NOCLDSTOP}; sigemptyset(&action.sa_mask);
    sigaction(SIGCHLD, &action, NULL); signal(SIGPIPE, SIG_IGN);
    char pid[24], uid[16], gid[16]; snprintf(pid, sizeof(pid), "%ld", (long)getpid()); snprintf(uid, sizeof(uid), "%u", user.uid); snprintf(gid, sizeof(gid), "%u", user.gid);
    if (!send_fields(fds, 3, "ready", pid, uid, gid, created ? "1" : "0", user.home, runtime, (char*)NULL)) { logout("ready not delivered"); return 1; }
    for (int i = 0; i < 3; i++) close(fds[i]);
    note("session user=%s uid=%u id=%s first=%d agent=%s", user.name, user.uid, session_id, created, pid);
    agent();
    return 0;
}
