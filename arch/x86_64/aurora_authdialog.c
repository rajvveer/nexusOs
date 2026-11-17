/* The Aurora authentication agent for the authorization broker (aurora_authdialog.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "aurora_authdialog.h"
#include "aurora_auth.h"
#include "../../kernel/window.h"
#include "../../kernel/appui.h"
#include "../../kernel/macui.h"

#define TRIES 5            /* the account helper's tally (aurora_auth.c / nexus-auth) */
#define LOCKOUT_S 60       /* how long the dialog refuses input after the broker reports the lockout */

/* ---------------------------------------------------------------- the broker */
bool aurora_authz_request(const char* line, char* reply, size_t size) {
    reply[0] = 0;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (s < 0) return false;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "%s", AURORA_POLKIT_SOCKET);
    bool ok = false; bool listing = !strncmp(line, "ACTIONS", 7);
    if (!connect(s, (struct sockaddr*)&a, sizeof(a)) && write(s, line, strlen(line)) == (ssize_t)strlen(line)) {
        size_t used = 0; struct pollfd p = {s, POLLIN, 0};   /* one request per connection; a wrong password costs 2 s */
        while (used < size - 1 && poll(&p, 1, 15000) > 0) {
            ssize_t n = read(s, reply + used, size - 1 - used); if (n <= 0) break;
            used += (size_t)n; reply[used] = 0;
            if (listing ? strstr(reply, "END\n") != NULL : strchr(reply, '\n') != NULL) break;
        }
        ok = used > 0;
    }
    close(s);
    if (!ok) reply[0] = 0; else if (!listing) reply[strcspn(reply, "\n")] = 0;
    return ok;
}
bool aurora_authz_available(void) { char reply[64]; char line[64]; snprintf(line, sizeof(line), "QUERY %u org.nexus.policy.query\n", (unsigned)getuid()); return aurora_authz_request(line, reply, sizeof(reply)) && !strcmp(reply, "AUTHORIZED"); }
bool aurora_authz_known(const char* action) {
    char reply[4096]; if (!aurora_authz_request("ACTIONS\n", reply, sizeof(reply))) return false;
    for (char* save = NULL, *l = strtok_r(reply, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) { size_t n = strlen(action); if (!strncmp(l, action, n) && l[n] == ' ') return true; }
    return false;
}

/* ---------------------------------------------------------------- the dialog */
static const struct { const char* action; const char* what; } WHAT[] = {
    {AURORA_ACTION_ACCOUNTS, "change user accounts"}, {AURORA_ACTION_EXEC, "run a program as an administrator"},
    {"org.nexus.network.configure", "change the network settings"}, {"org.nexus.network.vpn", "connect or disconnect a VPN"},
    {"org.nexus.settings.datetime", "set the date and time"}, {"org.nexus.settings.hostname", "change the computer's name"},
};
static char action[128], app_name[96], what[160], who[160], typed[256], status_line[160], **program;
static int typed_len, failures, pass_fd = -1;
static bool cancel_focus;
static time_t locked_until;
static pid_t checker = -1; static int checker_pipe = -1;   /* a CHECK runs in a child: the window keeps drawing */

static void wipe(void) { memset(typed, 0, sizeof(typed)); typed_len = 0; }
static void leave(int code, const char* why) {
    wipe(); printf("[64] AURORA AUTH %s action=%s\n", why, action); fflush(stdout); exit(code);
}
static int run_program(const char* password) {   /* PROGRAM through pkexec, the password on $PKEXEC_PASSFD */
    int p[2]; if (pipe(p)) return 126;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return 126; }
    if (!pid) {
        close(p[1]); char fd[16]; snprintf(fd, sizeof(fd), "%d", p[0]); setenv("PKEXEC_PASSFD", fd, 1);
        int n = 0; while (program[n]) n++;
        char** argv = calloc((size_t)n + 2, sizeof(char*)); if (!argv) _exit(126);
        argv[0] = (char*)"pkexec"; for (int i = 0; i < n; i++) argv[i + 1] = program[i];
        execv("/usr/bin/pkexec", argv); _exit(126);
    }
    close(p[0]); size_t length = strlen(password);
    if (write(p[1], password, length) != (ssize_t)length || write(p[1], "\n", 1) != 1) {}
    close(p[1]);
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 126;
}
static void authorized(const char* password) {
    if (pass_fd >= 0) { size_t n = strlen(password); if (write(pass_fd, password, n) != (ssize_t)n || write(pass_fd, "\n", 1) != 1) {} close(pass_fd); pass_fd = -1; }
    printf("[64] AURORA AUTH AUTHORIZED action=%s\n", action); fflush(stdout);
    int code = program ? run_program(password) : 0;
    if (program) { printf("[64] AURORA AUTH PROGRAM status=%d\n", code); fflush(stdout); }
    wipe(); exit(code);
}
static void submit(void) {
    if (checker > 0 || !typed_len || time(NULL) < locked_until) return;
    int p[2]; if (pipe2(p, O_CLOEXEC)) return;
    char line[512]; snprintf(line, sizeof(line), "CHECK %u %s %s\n", (unsigned)getuid(), action, typed);
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); memset(line, 0, sizeof(line)); return; }
    if (!pid) {
        close(p[0]); char reply[128]; if (!aurora_authz_request(line, reply, sizeof(reply))) strcpy(reply, "UNREACHABLE");
        if (write(p[1], reply, strlen(reply)) < 0) {} _exit(0);
    }
    memset(line, 0, sizeof(line)); close(p[1]); checker = pid; checker_pipe = p[0];
    fcntl(checker_pipe, F_SETFL, O_NONBLOCK); snprintf(status_line, sizeof(status_line), "Checking...");
}
bool aurora_authdialog_tick(void) {
    static bool sealed; if (!sealed) { sealed = true; prctl(PR_SET_DUMPABLE, 0); }   /* connected (Aurora checked it, aurora_sak.h): no ptrace from here on */
    static time_t shown; bool redraw = false; time_t now = time(NULL);
    if (locked_until && now != shown) { shown = now; redraw = true;
        if (now >= locked_until) { locked_until = 0; snprintf(status_line, sizeof(status_line), "You can try again."); }
        else snprintf(status_line, sizeof(status_line), "Too many failed attempts: try again in %ld s.", (long)(locked_until - now)); }
    if (checker <= 0) return redraw;
    char reply[128]; ssize_t n = read(checker_pipe, reply, sizeof(reply) - 1);
    if (n < 0 && errno == EAGAIN) return redraw;
    reply[n > 0 ? n : 0] = 0; close(checker_pipe); checker_pipe = -1; waitpid(checker, NULL, 0); checker = -1;
    if (!strcmp(reply, "AUTHORIZED")) { char password[256]; snprintf(password, sizeof(password), "%s", typed); authorized(password); }
    wipe();
    if (!strcmp(reply, "DENIED throttled")) {
        locked_until = now + LOCKOUT_S; printf("[64] AURORA AUTH LOCKED action=%s\n", action); fflush(stdout);
        snprintf(status_line, sizeof(status_line), "Too many failed attempts: try again in %d s.", LOCKOUT_S);
    } else if (!strncmp(reply, "DENIED", 6)) {
        failures++; printf("[64] AURORA AUTH WRONG attempt=%d reply=%s\n", failures, reply); fflush(stdout);
        if (failures >= TRIES) { locked_until = now + LOCKOUT_S; snprintf(status_line, sizeof(status_line), "Too many failed attempts: try again in %d s.", LOCKOUT_S); }
        else snprintf(status_line, sizeof(status_line), "Wrong password. %d of %d attempts left.", TRIES - failures, TRIES);
    } else snprintf(status_line, sizeof(status_line), "The authorization service did not answer.");
    return true;
}
static void key(int id, char k) {
    (void)id; unsigned char c = (unsigned char)k;
    if (k == 27) leave(1, "CANCELLED");
    if (k == '\t') { cancel_focus = !cancel_focus; return; }
    if (k == '\n') { if (cancel_focus) leave(1, "CANCELLED"); submit(); return; }
    if (checker > 0 || time(NULL) < locked_until) return;
    if (k == '\b') { if (typed_len) typed[--typed_len] = 0; return; }
    if (c >= 32 && c < 127 && typed_len < (int)sizeof(typed) - 1) { typed[typed_len++] = k; typed[typed_len] = 0; if (status_line[0] && failures < TRIES) status_line[0] = 0; }
}
static void click(int id, int x, int y) {
    int cx, cy, w, h; window_content_px(id, &cx, &cy, &w, &h); (void)cx; (void)cy;
    if (y >= h - 52 && y < h - 16) { if (x >= w - 270 && x < w - 150) leave(1, "CANCELLED"); if (x >= w - 140 && x < w - 20) submit(); }
}
static void draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d; int x, y, w, h; window_content_px(id, &x, &y, &w, &h); appui_px_fill(x, y, w, h, APPUI_BG);
    appui_px_text(x + 20, y + 16, "Authentication Required", APPUI_F_TITLE, APPUI_TEXT);
    char line[300]; snprintf(line, sizeof(line), "%s wants to %s.", app_name, what); appui_px_text(x + 20, y + 52, line, APPUI_F_BODY, APPUI_TEXT);
    snprintf(line, sizeof(line), "Action: %s", action); appui_px_text(x + 20, y + 78, line, APPUI_F_SMALL, APPUI_SUBTLE);
    appui_px_text(x + 20, y + 102, who, APPUI_F_BODY, APPUI_SUBTLE);
    char shown[64]; int n = typed_len < 40 ? typed_len : 40; for (int i = 0; i < n; i++) shown[i] = '*'; shown[n] = 0;
    appui_px_text(x + 20, y + 140, "Password", APPUI_F_BODY, APPUI_SUBTLE); appui_px_secret_field(x + 120, y + 134, w - 150, 30, shown, !cancel_focus && checker <= 0 && !locked_until);
    if (status_line[0]) appui_px_text(x + 120, y + 172, status_line, APPUI_F_SMALL, failures || locked_until ? APPUI_RED : APPUI_SUBTLE);
    appui_px_text(x + 20, y + h - 44, "Super+Esc checks this dialog", APPUI_F_SMALL, APPUI_SUBTLE);
    appui_px_button(x + w - 270, y + h - 52, 120, 34, "Cancel", cancel_focus); appui_px_button(x + w - 140, y + h - 52, 120, 34, "Authenticate", !cancel_focus);
}
int aurora_authdialog_main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3 || argv[2][0] == '-') { fprintf(stderr, "usage: aurora-app authenticate ACTION [--app NAME] [--description TEXT] [--passfd N] [-- PROGRAM ARG...]\n"); exit(2); }
    snprintf(action, sizeof(action), "%s", argv[2]);
    for (const char* p = action; *p; p++) if (!(*p == '.' || *p == '-' || *p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) { fprintf(stderr, "aurora-app authenticate: bad action\n"); exit(2); }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--") && i + 1 < argc) { program = argv + i + 1; break; }
        if (i + 1 >= argc) { fprintf(stderr, "aurora-app authenticate: %s needs a value\n", argv[i]); exit(2); }
        if (!strcmp(argv[i], "--app")) snprintf(app_name, sizeof(app_name), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--description")) snprintf(what, sizeof(what), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--passfd")) { char* end; long fd = strtol(argv[++i], &end, 10); if (*end || fd < 3 || fd > 1024 || fcntl((int)fd, F_GETFD) < 0) { fprintf(stderr, "aurora-app authenticate: bad --passfd\n"); exit(2); } pass_fd = (int)fd; }
        else { fprintf(stderr, "aurora-app authenticate: unknown option %s\n", argv[i]); exit(2); }
    }
    if (!app_name[0]) {   /* the requesting program: the parent's name */
        char path[64], name[64] = ""; snprintf(path, sizeof(path), "/proc/%d/comm", (int)getppid()); FILE* f = fopen(path, "re");
        if (f) { if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0; fclose(f); }
        snprintf(app_name, sizeof(app_name), "%s", name[0] ? name : "An application");
    }
    for (char* p = app_name; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    for (char* p = what; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    if (!what[0]) { snprintf(what, sizeof(what), "perform %s", action); for (size_t i = 0; i < sizeof(WHAT) / sizeof(*WHAT); i++) if (!strcmp(WHAT[i].action, action)) snprintf(what, sizeof(what), "%s", WHAT[i].what); }
    char line[256], reply[128]; snprintf(line, sizeof(line), "QUERY %u %s\n", (unsigned)getuid(), action);
    if (!aurora_authz_request(line, reply, sizeof(reply))) leave(2, "UNAVAILABLE");
    if (!strcmp(reply, "AUTHORIZED")) { printf("[64] AURORA AUTH NO-DIALOG action=%s\n", action); fflush(stdout); authorized(""); }
    if (strcmp(reply, "CHALLENGE")) { printf("[64] AURORA AUTH DENIED action=%s reply=%s\n", action, reply); fflush(stdout); exit(!strcmp(reply, "DENIED throttled") ? 4 : 3); }
    struct aurora_user me; bool known = aurora_user_by_uid((unsigned)getuid(), &me);
    snprintf(who, sizeof(who), "Type the password of %s (%s)%s.", known && me.gecos[0] ? me.gecos : known ? me.name : "this account",
             known ? me.name : "?", known && me.admin ? ", an administrator" : "");
    printf("[64] AURORA AUTH DIALOG action=%s app=%s user=%s\n", action, app_name, known ? me.name : "?"); fflush(stdout);
    int id = window_create("Authentication Required", 0, 0, 72, 16, draw, key); window_set_on_click(id, click); return id;
}
