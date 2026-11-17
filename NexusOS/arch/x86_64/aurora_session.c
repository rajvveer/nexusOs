/* Aurora user sessions (aurora_session.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "aurora_session.h"
#include "aurora_session_proto.h"
#include "aurora_auth.h"
#include "aurora_control.h"
#include "aurora_lock.h"
#include "aurora_wm.h"
#include "aurora_idle.h"
#include "aurora_capture.h"
#include "aurora_notices.h"
#include "aurora_reader.h"
#include "aurora_clipboard.h"

#define PR_NEXUS_SEAT 0x4E585301
#define SESSIONS 8
#define LOGIND_NOTIFY "/run/nexus/logind-notify"   /* nexus-logind re-reads "sessions" when poked (nexus_logind.c) */
enum { FREE, STARTING, READY, ENDING };
struct session {
    int state, id; unsigned uid, gid; char name[33], home[129];
    bool boot, logged_in, was_up; uint64_t since_us;
    pid_t agent; int ctl, listen_fd, control_fd, notify_fd;
    struct wl_event_source *ctl_source, *listen_source, *notify_source;
};
static struct session sessions[SESSIONS];
static int active;   /* index of the active session (0 = the desktop user's own) */
static int next_id = 2, inhibitors;
static pid_t ended_agents[SESSIONS * 2];   /* agents released before they were reaped: their exit is ours, not an app's */
static struct wl_display* display;

static uint64_t realtime_us(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
static void logind_poke(void) {   /* best effort: logind refreshes its view of the seat */
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) return;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "%s", LOGIND_NOTIFY);
    if (sendto(fd, "changed", 7, MSG_NOSIGNAL | MSG_DONTWAIT, (struct sockaddr*)&a, sizeof(a)) < 0) {}
    close(fd);
}
static struct session* by_uid(unsigned uid) {
    for (int i = 0; i < SESSIONS; i++) if (sessions[i].state != FREE && sessions[i].uid == uid) return &sessions[i];
    return NULL;
}
unsigned aurora_session_active_uid(void) { return sessions[active].uid; }
bool aurora_session_foreign(void) { return !sessions[active].boot; }
bool aurora_session_running(unsigned uid) {
    struct session* s = by_uid(uid); if (!s) return false;
    return s->boot ? s->logged_in : s->state == READY;
}
bool aurora_session_client_visible(struct wl_client* client) {
    if (!client) return false;
    uid_t uid; wl_client_get_credentials(client, NULL, &uid, NULL);
    return uid == 0 || uid == sessions[active].uid;
}
void aurora_shell_redraw(bool everything);   /* desktop.c */
unsigned aurora_notices_session_uid(void) { return sessions[active].uid; }
void aurora_notices_changed(void) { aurora_shell_redraw(false); }
/* the desktop user's notifications (aurora_services.c, $XDG_RUNTIME_DIR/aurora-notify of the boot account) */
void aurora_session_notify(const char* text) { if (aurora_notice_post(sessions[0].uid, "", text) && active == 0) aurora_shell_notice(text); }
void aurora_session_idle_inhibit(int delta) { inhibitors += delta; if (inhibitors < 0) inhibitors = 0; }
bool aurora_session_idle_inhibited(void) { return inhibitors > 0 || aurora_idle_inhibited() > 0; }   /* logind's + Wayland clients' */
static void activate(int index) {
    struct session* s = &sessions[index]; bool changed = index != active;
    active = index;
    if (prctl(PR_NEXUS_SEAT, 1, s->uid, 0, 0)) {}   /* console devices follow the active user (no-op on older kernels) */
    aurora_control_session_user(s->uid);
    setenv("HOME", s->home, 1);   /* per-user things the shell reads (Windows program shortcuts) are the active user's */
    if (changed) aurora_host_session_switched();
    printf("[64] AURORA SESSION ACTIVE user=%s uid=%u id=%d\n", s->name, s->uid, s->id); fflush(stdout);
    logind_poke();
}
void aurora_session_activate_uid(unsigned uid) {
    struct session* s = by_uid(uid);
    if (s && (s->boot || s->state == READY)) activate((int)(s - sessions));
}
void aurora_session_boot_greeter(void) { sessions[0].logged_in = false; logind_poke(); }   /* boot: the greeter comes first */
void aurora_session_boot_login(bool logged_in) {
    struct session* b = &sessions[0];
    if (logged_in && !b->logged_in) b->since_us = realtime_us();
    b->logged_in = logged_in; if (logged_in) activate(0);
    printf("[64] AURORA SESSION %s user=%s uid=%u id=1\n", logged_in ? "LOGIN" : "LOGOUT", b->name, b->uid); fflush(stdout);
    logind_poke();
}

/* ---- a foreign session's sockets ---- */
static int wayland_ready(int fd, uint32_t mask, void* data) {
    struct session* s = data; (void)mask;
    for (;;) {
        int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) { if (errno == EINTR) continue; return 0; }
        struct ucred cred; socklen_t size = sizeof(cred);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &size) || (cred.uid != 0 && cred.uid != s->uid) || !wl_client_create(display, client)) close(client);
    }
}
static int notify_ready(int fd, uint32_t mask, void* data) {
    struct session* s = data; (void)mask;
    for (;;) {
        char buf[1024]; ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT); if (n <= 0) return 0;
        buf[n] = 0; char* fields[3] = {buf, "", ""}; unsigned count = 1;
        for (char* p = buf; *p && count < 3; p++) if (*p == '\x1f') { *p = 0; fields[count++] = p + 1; }
        for (unsigned i = 0; i < 3; i++) for (char* p = fields[i]; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
        printf("[64] AURORA NOTIFY app=%s summary=%s user=%s%s\n", fields[0], fields[1], s->name, s == &sessions[active] ? "" : " (background session: not shown)"); fflush(stdout);
        char text[512]; snprintf(text, sizeof(text), "%s%s%s%s%s", fields[0], fields[0][0] ? ": " : "", fields[1], fields[2][0] ? " - " : "", fields[2]);
        bool banner = aurora_notice_post(s->uid, fields[0], text);   /* history (Notification Centre); Do Not Disturb */
        if (s == &sessions[active] && banner) aurora_shell_notice(text);   /* one user's notifications are not shown to another */
    }
}
static void release(struct session* s) {
    bool was_active = s == &sessions[active]; unsigned uid = s->uid;
    if (!s->boot) aurora_reader_session_end(uid);
    aurora_clipboard_forget(uid);   /* its clipboard history */
    if (s->ctl_source) wl_event_source_remove(s->ctl_source);
    if (s->listen_source) wl_event_source_remove(s->listen_source);
    if (s->notify_source) wl_event_source_remove(s->notify_source);
    if (s->control_fd >= 0) aurora_control_remove_listener(s->control_fd);
    if (s->ctl >= 0) close(s->ctl);
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->notify_fd >= 0) close(s->notify_fd);
    bool was_ready = s->was_up;
    if (!was_ready && s->id == next_id - 1) next_id--;   /* a refused sign-in leaves no gap in the session numbers */
    if (s->agent > 0) for (unsigned i = 0; i < sizeof(ended_agents) / sizeof(*ended_agents); i++) if (!ended_agents[i]) { ended_agents[i] = s->agent; break; }
    memset(s, 0, sizeof(*s)); s->state = FREE; s->ctl = s->listen_fd = s->control_fd = s->notify_fd = -1;
    if (was_ready) {   /* its clients go with it (their processes were ended by the agent) */
        struct wl_list* list = wl_display_get_client_list(display); struct wl_list* link, *next;
        for (link = list->next; link != list; link = next) {
            next = link->next; struct wl_client* c = wl_client_from_link(link); uid_t cuid; wl_client_get_credentials(c, NULL, &cuid, NULL);
            if (cuid == uid) wl_client_destroy(c);
        }
    }
    if (was_active) activate(0);   /* the desktop user's windows come back (unparked) behind the greeter */
    printf("[64] AURORA SESSION END uid=%u\n", uid); fflush(stdout);
    logind_poke();
    if (was_ready || was_active) aurora_lock_session_ended(uid, was_active);
}
static void record(struct session* s, char* data, size_t size, int* fds, int nfds) {
    char* f[16]; int n = 0;
    if (!size || data[size - 1]) goto drop;
    for (char* p = data; p < data + size && n < 16; p += strlen(p) + 1) f[n++] = p;
    if (!strcmp(f[0], "auth") && n == 2) {
        int code = atoi(f[1]);
        if (code) { s->state = ENDING; aurora_lock_session_result(code, s->uid); }
        goto drop;
    }
    if (!strcmp(f[0], "ready") && n == 7 && nfds == 3 && s->state == STARTING) {
        if ((unsigned)atoi(f[2]) != s->uid || (pid_t)atoi(f[1]) != s->agent) { printf("[64] AURORA SESSION refused: the launcher reported uid %s pid %s\n", f[2], f[1]); fflush(stdout); goto drop; }
        s->gid = (unsigned)atoi(f[3]); snprintf(s->home, sizeof(s->home), "%s", f[5]);
        s->listen_fd = fds[0]; s->control_fd = fds[1]; s->notify_fd = fds[2]; fds[0] = fds[1] = fds[2] = -1;
        struct wl_event_loop* loop = wl_display_get_event_loop(display);
        fcntl(s->listen_fd, F_SETFL, O_NONBLOCK); fcntl(s->notify_fd, F_SETFL, O_NONBLOCK);
        s->listen_source = wl_event_loop_add_fd(loop, s->listen_fd, WL_EVENT_READABLE, wayland_ready, s);
        s->notify_source = wl_event_loop_add_fd(loop, s->notify_fd, WL_EVENT_READABLE, notify_ready, s);
        if (!s->listen_source || !s->notify_source || !aurora_control_add_listener(s->control_fd, s->uid)) {
            const char logout[] = "logout"; if (send(s->ctl, logout, sizeof(logout), MSG_NOSIGNAL) < 0) {}
            s->state = ENDING; aurora_lock_session_result(AURORA_AUTH_UNAVAILABLE, s->uid); goto drop;
        }
        s->state = READY; s->was_up = true; s->since_us = realtime_us();
        printf("[64] AURORA SESSION LOGIN user=%s uid=%u id=%d agent=%ld first=%s\n", s->name, s->uid, s->id, (long)s->agent, f[4]); fflush(stdout);
        activate((int)(s - sessions));
        aurora_lock_session_result(AURORA_AUTH_OK, s->uid);
        goto drop;
    }
    if (!strcmp(f[0], "exit") && n == 3) { if (!aurora_reader_reaped((pid_t)atoi(f[1]))) aurora_host_reaped((pid_t)atoi(f[1]), atoi(f[2])); goto drop; }
    if (!strcmp(f[0], "bye")) { s->state = ENDING; goto drop; }
drop:
    for (int i = 0; i < nfds; i++) if (fds[i] >= 0) close(fds[i]);
}
/* One record from the agent (non-blocking); false when the socket is finished. */
static bool read_record(struct session* s, char* out, size_t* size, int* fds, int* nfds) {
    char control[CMSG_SPACE(sizeof(int) * 8)]; struct iovec iov = {out, NEXUS_SESSION_RECORD};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    *nfds = 0; ssize_t n = recvmsg(s->ctl, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) { *size = 0; return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)); int* got = (int*)CMSG_DATA(c);
            for (int i = 0; i < count; i++) { if (*nfds < 4) fds[(*nfds)++] = got[i]; else close(got[i]); }
        }
    if (n == 0) { for (int i = 0; i < *nfds; i++) close(fds[i]); *nfds = 0; *size = 0; return false; }
    out[n] = 0; *size = (size_t)n; return true;
}
static char buffer[NEXUS_SESSION_RECORD + 1];
static int ctl_ready(int fd, uint32_t mask, void* data) {
    struct session* s = data; (void)fd;
    for (;;) {
        size_t size; int fds[4], nfds;
        if (!read_record(s, buffer, &size, fds, &nfds)) { release(s); return 0; }
        if (!size) break;
        record(s, buffer, size, fds, nfds);
    }
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) release(s);
    return 0;
}
void aurora_session_start(const char* name, const char* password) {
    struct aurora_user u; struct session* s = NULL;
    if (!aurora_user_find(name, &u)) { aurora_lock_session_result(AURORA_AUTH_UNKNOWN_USER, 0); return; }
    if (by_uid(u.uid)) { aurora_lock_session_result(AURORA_AUTH_INVALID, u.uid); return; }
    if (access(NEXUS_SESSION_PROGRAM, X_OK)) { aurora_lock_session_result(AURORA_AUTH_UNAVAILABLE, u.uid); return; }
    for (int i = 1; i < SESSIONS && !s; i++) if (sessions[i].state == FREE) s = &sessions[i];
    if (!s) { aurora_lock_session_result(AURORA_AUTH_DENIED, u.uid); return; }
    int sv[2], pw[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { aurora_lock_session_result(AURORA_AUTH_UNAVAILABLE, u.uid); return; }
    if (pipe2(pw, O_CLOEXEC)) { close(sv[0]); close(sv[1]); aurora_lock_session_result(AURORA_AUTH_UNAVAILABLE, u.uid); return; }
    int id = next_id++; char id_text[16]; snprintf(id_text, sizeof(id_text), "%d", id);
    pid_t pid = fork();
    if (!pid) {
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL); signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (dup2(pw[0], 0) < 0 || dup2(sv[1], NEXUS_SESSION_FD) < 0) _exit(127);
        execl(NEXUS_SESSION_PROGRAM, "nexus-session", "start", u.name, id_text, (char*)NULL); _exit(127);
    }
    close(pw[0]); close(sv[1]);
    if (pid < 0) { close(pw[1]); close(sv[0]); aurora_lock_session_result(AURORA_AUTH_UNAVAILABLE, u.uid); return; }
    size_t length = strlen(password); ssize_t a = write(pw[1], password, length), b = write(pw[1], "\n", 1); (void)a; (void)b; close(pw[1]);
    memset(s, 0, sizeof(*s)); s->state = STARTING; s->id = id; s->uid = u.uid; s->gid = u.gid; s->agent = pid; s->ctl = sv[0];
    s->listen_fd = s->control_fd = s->notify_fd = -1;
    snprintf(s->name, sizeof(s->name), "%s", u.name); snprintf(s->home, sizeof(s->home), "%s", u.home);
    s->ctl_source = wl_event_loop_add_fd(wl_display_get_event_loop(display), s->ctl, WL_EVENT_READABLE, ctl_ready, s);
    if (!s->ctl_source) { kill(pid, SIGTERM); release(s); aurora_lock_session_result(AURORA_AUTH_UNAVAILABLE, u.uid); return; }
    printf("[64] AURORA SESSION STARTING user=%s uid=%u id=%d launcher=%ld\n", s->name, s->uid, s->id, (long)pid); fflush(stdout);
}
bool aurora_session_reaped(pid_t pid, int status) {
    for (unsigned i = 0; i < sizeof(ended_agents) / sizeof(*ended_agents); i++) if (pid > 0 && ended_agents[i] == pid) { ended_agents[i] = 0; return true; }
    for (int i = 1; i < SESSIONS; i++) if (sessions[i].state != FREE && sessions[i].agent == pid) {
        printf("[64] AURORA SESSION AGENT EXIT user=%s status=%d\n", sessions[i].name, status); fflush(stdout);
        sessions[i].agent = 0;
        if (sessions[i].state == STARTING) aurora_lock_session_result(WIFEXITED(status) && WEXITSTATUS(status) ? WEXITSTATUS(status) : AURORA_AUTH_UNAVAILABLE, sessions[i].uid);
        release(&sessions[i]); return true;
    }
    return false;
}
static bool send_logout(struct session* s) { const char logout[] = "logout"; return s->ctl >= 0 && send(s->ctl, logout, sizeof(logout), MSG_NOSIGNAL) == (ssize_t)sizeof(logout); }
void aurora_session_logout(void) {
    struct session* s = &sessions[active];
    printf("[64] AURORA SESSION LOGOUT REQUEST user=%s uid=%u\n", s->name, s->uid); fflush(stdout);
    aurora_reader_session_end(s->uid); aurora_clipboard_forget(s->uid);
    if (s->boot) {   /* the desktop user's own session: its applications are asked to end, then the greeter */
        aurora_host_session_end_clients(s->uid, SIGTERM); aurora_session_boot_login(false); aurora_lock_session_ended(s->uid, true); return;
    }
    if (s->state != READY) return;
    s->state = ENDING; if (!send_logout(s) && s->agent > 0) kill(s->agent, SIGTERM);
    activate(0); aurora_lock_session_ended(s->uid, true);   /* the greeter at once; the agent ends the user's processes */
}
void aurora_session_shutdown(void) { for (int i = 1; i < SESSIONS; i++) if (sessions[i].state == READY) { sessions[i].state = ENDING; send_logout(&sessions[i]); } }
pid_t aurora_session_run(char** command, const char* tag) {
    struct session* s = &sessions[active];
    if (s->boot || s->state != READY) return -1;
    static char out[NEXUS_SESSION_RECORD]; size_t used = 0; char** env = command; int envc = 0;
    while (command[0] && command[0][0] != '/' && strchr(command[0], '=')) { command++; envc++; }
    int argc = 0; while (command[argc]) argc++;
    if (!argc || argc > NEXUS_SESSION_MAX_ARGS || envc > NEXUS_SESSION_MAX_ENV) return -1;
    char counts[2][16]; snprintf(counts[0], 16, "%d", argc); snprintf(counts[1], 16, "%d", envc);
    const char* head[] = {"run", tag, counts[0]};
#define PUT(text) do { size_t n_ = strlen(text) + 1; if (used + n_ > sizeof(out)) return -1; memcpy(out + used, text, n_); used += n_; } while (0)
    for (int i = 0; i < 3; i++) PUT(head[i]);
    for (int i = 0; i < argc; i++) PUT(command[i]);
    PUT(counts[1]);
    for (int i = 0; i < envc; i++) PUT(env[i]);
#undef PUT
    if (send(s->ctl, out, used, MSG_NOSIGNAL) != (ssize_t)used) return -1;
    /* the agent answers at once ("started"/"failed"); other records that come first are handled on the way */
    for (int waited = 0; waited < 3000; waited += 20) {
        struct pollfd p = {s->ctl, POLLIN, 0}; if (poll(&p, 1, 20) <= 0) continue;
        size_t size; int fds[4], nfds;
        if (!read_record(s, buffer, &size, fds, &nfds)) return -1;   /* closed: ctl_ready will release it */
        if (!size) continue;
        if (size > 8 && !memcmp(buffer, "started", 8) && !strcmp(buffer + 8, tag)) { for (int i = 0; i < nfds; i++) close(fds[i]); return (pid_t)atol(buffer + 8 + strlen(tag) + 1); }
        if (size > 7 && !memcmp(buffer, "failed", 7) && !strcmp(buffer + 7, tag)) { for (int i = 0; i < nfds; i++) close(fds[i]); return -1; }
        record(s, buffer, size, fds, nfds);
    }
    return -1;
}

/* ---- control socket: the seat for logind and tests ---- */
static bool control_sessions(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size;
    for (int i = 0; i < SESSIONS; i++) {
        struct session* s = &sessions[i];
        if (s->state == FREE || (s->boot && !s->logged_in)) continue;
        bool is_active = i == active && (!s->boot || s->logged_in);
        aurora_control_print(reply, "id=%d uid=%u gid=%u user=%s leader=%ld state=%s active=%d locked=%d since=%llu home=%s\n", s->id, s->uid, s->gid, s->name,
                             s->boot ? 1L : (long)s->agent, s->state == ENDING ? "closing" : is_active ? "active" : "online", is_active,
                             is_active ? aurora_lock_active() : 1, (unsigned long long)s->since_us, s->home);
    }
    aurora_control_print(reply, "seat active_uid=%u locked=%d idle_inhibited=%d idle_logind=%d idle_wayland=%d\n", sessions[active].uid, aurora_lock_active(),
                         aurora_session_idle_inhibited(), inhibitors, aurora_idle_inhibited());
    return true;
}
static bool control_switch(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; aurora_lock_switch_user(); return true;
}
static bool control_logout(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; aurora_session_logout(); return true;
}
static bool root_only(char* error, size_t size) {
    if (aurora_control_peer_uid() == 0) return true;
    snprintf(error, size, "permission denied: root only"); return false;
}
static struct session* by_id_text(const char* text) {
    char* end; long id = strtol(text, &end, 10); if (end == text || (*end && *end != ' ')) return NULL;
    for (int i = 0; i < SESSIONS; i++) if (sessions[i].state != FREE && sessions[i].id == id && (!sessions[i].boot || sessions[i].logged_in)) return &sessions[i];
    return NULL;
}
/* session-activate ID (root: logind ActivateSession): switch to it locked (its user's password brings it back) */
static bool control_activate(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; if (!root_only(error, size)) return false;
    struct session* s = by_id_text(args); if (!s) { snprintf(error, size, "no such session"); return false; }
    if (s != &sessions[active]) { activate((int)(s - sessions)); aurora_lock_now("switch"); }
    return true;
}
static bool control_terminate(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; if (!root_only(error, size)) return false;
    struct session* s = by_id_text(args); if (!s) { snprintf(error, size, "no such session"); return false; }
    if (s == &sessions[active]) aurora_session_logout();
    else if (s->boot) { aurora_reader_session_end(s->uid); aurora_clipboard_forget(s->uid); aurora_host_session_end_clients(s->uid, SIGTERM); s->logged_in = false; logind_poke(); }
    else { s->state = ENDING; send_logout(s); }
    return true;
}
static bool control_inhibit(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; if (!root_only(error, size)) return false;
    static int from_logind; int count;
    if (sscanf(args, "idle=%d", &count) != 1 || count < 0 || count > 100000) { snprintf(error, size, "usage: inhibit idle=N"); return false; }
    aurora_session_idle_inhibit(count - from_logind); from_logind = count; return true;
}
/* power-off / reboot (root: logind PowerOff/Reboot): Aurora's orderly shutdown, not cancelled by open windows */
static bool control_power(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; if (!root_only(error, size)) return false;
    aurora_host_power_request(false); return true;
}
static bool control_reboot(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; if (!root_only(error, size)) return false;
    aurora_host_power_request(true); return true;
}
/* windows: what the active session shows (tests; the others' windows are parked) */
static bool control_windows(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size;
    struct aurora_window* list[128]; int n = aurora_host_windows(list, 128);
    for (int i = 0; i < n; i++) { struct aurora_window_info info; aurora_host_window_info(list[i], &info);
        aurora_control_print(reply, "window app=%s title=%s minimized=%d focused=%d workspace=%d\n", info.app_id, info.title, info.minimized, info.focused, info.workspace); }
    aurora_control_print(reply, "windows=%d session_uid=%u\n", n, sessions[active].uid); return true;
}
void aurora_session_init(struct wl_display* d) {
    display = d;
    for (int i = 0; i < SESSIONS; i++) { sessions[i].ctl = sessions[i].listen_fd = sessions[i].control_fd = sessions[i].notify_fd = -1; }
    struct session* b = &sessions[0]; struct aurora_user u;
    b->state = READY; b->boot = true; b->id = 1; b->uid = (unsigned)getuid(); b->gid = (unsigned)getgid(); b->logged_in = true; b->since_us = realtime_us();
    if (aurora_user_by_uid(b->uid, &u)) { snprintf(b->name, sizeof(b->name), "%s", u.name); snprintf(b->home, sizeof(b->home), "%s", u.home); }
    else { snprintf(b->name, sizeof(b->name), "nexus"); snprintf(b->home, sizeof(b->home), "/home/nexus"); }
    active = 0; aurora_control_session_user(b->uid);
    aurora_control_register("sessions", false, control_sessions);
    aurora_control_register("switch-user", false, control_switch);
    aurora_control_register("logout", false, control_logout);
    aurora_control_register("session-activate", false, control_activate);
    aurora_control_register("session-terminate", false, control_terminate);
    aurora_control_register("inhibit", false, control_inhibit);
    aurora_control_register("power-off", false, control_power);
    aurora_control_register("windows", false, control_windows);
    aurora_capture_init();   /* screenshot / frame for the desktop portal */
    aurora_notices_init();   /* notification history, Do Not Disturb, the menu-bar bell */
    aurora_reader_init(wl_display_get_event_loop(display));
    aurora_clipboard_init(wl_display_get_event_loop(display));   /* clipboard history, Super+V */   /* screen reader: aurora-a11y-bridge + Orca, keyboard access to the shell */
    aurora_control_register("reboot", false, control_reboot);
    logind_poke();
}
