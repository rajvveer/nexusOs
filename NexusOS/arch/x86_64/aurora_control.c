/* Aurora control socket (aurora_control.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "aurora_control.h"

#define COMMANDS 64
#define CLIENTS 16
#define REQUEST_MAX 65536
struct command { char name[32]; bool block; aurora_control_handler handler; };
struct aurora_control_reply { char* text; size_t used, size; bool failed; };
struct client { int fd; struct wl_event_source* source; char* buffer; size_t used; char* out; size_t out_used, out_sent; uid_t uid; int passed; pid_t pid; };
static struct command commands[COMMANDS]; static int command_count;
static struct client clients[CLIENTS];
static int listener = -1; static struct wl_event_loop* event_loop; static uid_t session_uid = 1000;
/* Listeners of other users' sessions (aurora_session.c: the socket in their runtime folder): owner = their uid. */
struct extra_listener { int fd; uid_t owner; struct wl_event_source* source; };
static struct extra_listener extras[8];
static uid_t peer_uid;   /* the requester of the command being run */
uid_t aurora_control_peer_uid(void) { return peer_uid; }
static pid_t peer_pid;
pid_t aurora_control_peer_pid(void) { return peer_pid; }
static int* peer_fd;   /* the descriptor that came with the request being run (or -1) */
int aurora_control_peer_fd(void) { if (!peer_fd) return -1; int fd = *peer_fd; *peer_fd = -1; return fd; }

void aurora_control_session_user(uid_t uid) { session_uid = uid; }
bool aurora_control_register(const char* name, bool block, aurora_control_handler handler) {
    if (command_count >= COMMANDS || strlen(name) >= sizeof(commands[0].name) || !handler) return false;
    for (int i = 0; i < command_count; i++) if (!strcmp(commands[i].name, name)) return false;
    struct command* c = &commands[command_count++]; snprintf(c->name, sizeof(c->name), "%s", name); c->block = block; c->handler = handler;
    return true;
}
void aurora_control_print(struct aurora_control_reply* r, const char* format, ...) {
    if (r->failed) return;
    for (;;) {
        va_list list; va_start(list, format);
        int n = vsnprintf(r->text + r->used, r->size - r->used, format, list); va_end(list);
        if (n < 0) { r->failed = true; return; }
        if ((size_t)n < r->size - r->used) { r->used += (size_t)n; return; }
        if (r->size > REQUEST_MAX * 4) { r->failed = true; return; }
        char* grown = realloc(r->text, r->size * 2 + (size_t)n); if (!grown) { r->failed = true; return; }
        r->text = grown; r->size = r->size * 2 + (size_t)n;
    }
}
static void client_close(struct client* c) {
    if (c->source) wl_event_source_remove(c->source);
    if (c->fd >= 0) close(c->fd);
    if (c->passed >= 0) close(c->passed);
    free(c->buffer); free(c->out); memset(c, 0, sizeof(*c)); c->fd = -1; c->passed = -1;
}
static int client_ready(int fd, uint32_t mask, void* data);
static void client_queue(struct client* c, const char* text, size_t size) {
    if (c->out_used + size > REQUEST_MAX * 8) { client_close(c); return; }   /* a peer that never reads */
    char* grown = realloc(c->out, c->out_used + size); if (!grown) { client_close(c); return; }
    c->out = grown; memcpy(c->out + c->out_used, text, size); c->out_used += size;
    wl_event_source_fd_update(c->source, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
}
/* One complete request (a line, or a block up to "end"): run its handler, queue the reply. */
static void run(struct client* c, const char* line, const char* body) {
    char name[32] = ""; size_t n = strcspn(line, " \t");
    if (n < sizeof(name)) { memcpy(name, line, n); name[n] = 0; }
    const char* args = line + n; while (*args == ' ' || *args == '\t') args++;
    struct aurora_control_reply reply = {malloc(1024), 0, 1024, false}; char error[256] = "";
    if (!reply.text) { client_close(c); return; }
    bool ok = false; struct command* found = NULL;
    for (int i = 0; i < command_count; i++) if (!strcmp(commands[i].name, name)) found = &commands[i];
    if (!found) snprintf(error, sizeof(error), "unknown command %.40s", name);
    else if (c->uid != 0 && c->uid != session_uid) snprintf(error, sizeof(error), "permission denied: not the active session");
    else { peer_uid = c->uid; peer_pid = c->pid; peer_fd = &c->passed; ok = found->handler(args, body, &reply, error, sizeof(error)); peer_uid = (uid_t)-1; peer_pid = 0; peer_fd = NULL; }
    if (c->passed >= 0) { close(c->passed); c->passed = -1; }   /* a descriptor the command did not take */
    if (reply.failed) { ok = false; snprintf(error, sizeof(error), "reply too large"); reply.used = 0; reply.failed = false; }
    if (ok) aurora_control_print(&reply, "OK\n"); else aurora_control_print(&reply, "ERROR %s\n", error[0] ? error : "failed");
    if (!reply.failed) client_queue(c, reply.text, reply.used);
    free(reply.text);
}
static void process(struct client* c) {
    size_t start = 0;
    for (;;) {
        char* newline = memchr(c->buffer + start, '\n', c->used - start); if (!newline) break;
        *newline = 0; char* line = c->buffer + start; size_t length = (size_t)(newline - line);
        if (length && line[length - 1] == '\r') line[--length] = 0;
        char name[32] = ""; size_t n = strcspn(line, " \t"); if (n < sizeof(name)) { memcpy(name, line, n); name[n] = 0; }
        bool block = false; for (int i = 0; i < command_count; i++) if (!strcmp(commands[i].name, name) && commands[i].block) block = true;
        if (!block) { size_t next = (size_t)(newline - c->buffer) + 1; if (length) run(c, line, NULL); if (c->fd < 0) return; start = next; continue; }
        /* block: the body runs up to a line "end" */
        char* body = newline + 1; char* end = NULL;
        for (char* p = body; p < c->buffer + c->used;) {
            char* eol = memchr(p, '\n', (size_t)(c->buffer + c->used - p)); if (!eol) break;
            size_t len = (size_t)(eol - p); if (len && p[len - 1] == '\r') len--;
            if (len == 3 && !memcmp(p, "end", 3)) { end = p; break; }
            p = eol + 1;
        }
        if (!end) { *newline = '\n'; break; }   /* wait for the rest */
        char* after = memchr(end, '\n', (size_t)(c->buffer + c->used - end));
        size_t next = (size_t)(after - c->buffer) + 1;
        *end = 0;
        run(c, line, body); if (c->fd < 0) return;
        start = next;
    }
    memmove(c->buffer, c->buffer + start, c->used - start); c->used -= start;
}
static int client_ready(int fd, uint32_t mask, void* data) {
    struct client* c = data; (void)fd;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { client_close(c); return 0; }
    if (mask & WL_EVENT_WRITABLE) {
        while (c->out_sent < c->out_used) {
            ssize_t n = send(c->fd, c->out + c->out_sent, c->out_used - c->out_sent, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) { client_close(c); return 0; }
            c->out_sent += (size_t)n;
        }
        if (c->out_sent == c->out_used) { c->out_sent = c->out_used = 0; wl_event_source_fd_update(c->source, WL_EVENT_READABLE); }
    }
    if (mask & WL_EVENT_READABLE) {
        for (;;) {
            if (c->used == REQUEST_MAX) { client_close(c); return 0; }   /* an over-long request */
            /* one descriptor may come with a request (SCM_RIGHTS: screenshot / frame targets); extras are closed */
            char control[CMSG_SPACE(sizeof(int) * 4)]; struct iovec iov = {c->buffer + c->used, REQUEST_MAX - c->used};
            struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
            ssize_t n = recvmsg(c->fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            for (struct cmsghdr* h = n >= 0 ? CMSG_FIRSTHDR(&m) : NULL; h; h = CMSG_NXTHDR(&m, h))
                if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
                    int* got = (int*)CMSG_DATA(h); size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    for (size_t i = 0; i < count; i++) { if (c->passed < 0 && i == 0) c->passed = got[i]; else close(got[i]); }
                }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) { if (c->out_used > c->out_sent) shutdown(c->fd, SHUT_RD); else { client_close(c); return 0; } break; }
            c->used += (size_t)n;
            process(c); if (c->fd < 0) return 0;
        }
    }
    return 0;
}
static int accept_ready(int fd, uint32_t mask, void* data) {
    (void)mask; const struct extra_listener* extra = data;   /* NULL: the desktop user's own socket */
    for (;;) {
        int peer = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (peer < 0) { if (errno == EINTR) continue; return 0; }
        struct ucred cred; socklen_t size = sizeof(cred);
        if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &size) || (cred.uid != 0 && cred.uid != (extra ? extra->owner : session_uid))) {
            static const char refusal[] = "ERROR permission denied\n"; ssize_t ignored = send(peer, refusal, sizeof(refusal) - 1, MSG_NOSIGNAL | MSG_DONTWAIT); (void)ignored;
            close(peer); continue;
        }
        struct client* c = NULL; for (int i = 0; i < CLIENTS; i++) if (clients[i].fd < 0) { c = &clients[i]; break; }
        if (!c) { close(peer); continue; }
        c->fd = peer; c->uid = cred.uid; c->pid = cred.pid; c->passed = -1; c->buffer = malloc(REQUEST_MAX);
        c->source = c->buffer ? wl_event_loop_add_fd(event_loop, peer, WL_EVENT_READABLE, client_ready, c) : NULL;
        if (!c->source) client_close(c);
    }
}
static bool ping(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; aurora_control_print(reply, "pong\n"); return true;
}
bool aurora_control_add_listener(int fd, uid_t owner) {
    for (int i = 0; i < 8; i++) if (!extras[i].source) {
        fcntl(fd, F_SETFL, O_NONBLOCK); extras[i].fd = fd; extras[i].owner = owner;
        extras[i].source = event_loop ? wl_event_loop_add_fd(event_loop, fd, WL_EVENT_READABLE, accept_ready, &extras[i]) : NULL;
        return extras[i].source != NULL;
    }
    return false;
}
void aurora_control_remove_listener(int fd) {
    for (int i = 0; i < 8; i++) if (extras[i].source && extras[i].fd == fd) { wl_event_source_remove(extras[i].source); memset(&extras[i], 0, sizeof(extras[i])); }
    for (int i = 0; i < CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].uid != 0 && clients[i].uid != session_uid) client_close(&clients[i]);
}
bool aurora_control_init(struct wl_event_loop* loop) {
    for (int i = 0; i < CLIENTS; i++) clients[i].fd = clients[i].passed = -1;
    event_loop = loop; aurora_control_register("ping", false, ping);
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX}; snprintf(address.sun_path, sizeof(address.sun_path), "%s", AURORA_CONTROL_SOCKET);
    unlink(AURORA_CONTROL_SOCKET);
    if (listener < 0 || bind(listener, (struct sockaddr*)&address, sizeof(address)) || chmod(AURORA_CONTROL_SOCKET, 0666) || listen(listener, 8) ||
        !wl_event_loop_add_fd(loop, listener, WL_EVENT_READABLE, accept_ready, NULL)) {
        perror("Aurora control socket"); if (listener >= 0) close(listener); listener = -1; return false;
    }
    return true;
}
