/* Clipboard history (aurora_clipboard.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "aurora_clipboard.h"
#include "aurora_control.h"
#include "aurora_session.h"

#define ALL_ENTRIES 64   /* every user's together */
#define CAPTURES 4
#define SERVES 8
struct entry { unsigned uid; char* data; size_t size; time_t when; };
static struct entry entries[ALL_ENTRIES]; static int count;   /* newest first */
struct capture { int fd; unsigned uid; char* data; size_t size, capacity; struct wl_event_source* source; uint64_t started; };
static struct capture captures[CAPTURES];
struct serve { int fd; char* data; size_t size, done; struct wl_event_source* source; uint64_t started; };
static struct serve serves[SERVES];
static struct wl_event_loop* loop;

static uint64_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void note(const char* format, unsigned uid, size_t bytes) {
    int mine = 0; for (int i = 0; i < count; i++) if (entries[i].uid == uid) mine++;
    printf("[64] AURORA CLIPBOARD %s uid=%u bytes=%zu entries=%d\n", format, uid, bytes, mine); fflush(stdout);
}
static void drop_entry(int i) {
    free(entries[i].data); memmove(&entries[i], &entries[i + 1], (size_t)(count - i - 1) * sizeof(*entries)); count--;
}
static void keep(unsigned uid, char* data, size_t size) {   /* takes data */
    for (int i = 0; i < count; i++) if (entries[i].uid == uid && entries[i].size == size && !memcmp(entries[i].data, data, size)) { drop_entry(i); break; }
    int mine = 0, oldest = -1; for (int i = 0; i < count; i++) if (entries[i].uid == uid) { mine++; oldest = i; }
    if (mine >= AURORA_CLIPBOARD_ENTRIES && oldest >= 0) drop_entry(oldest);
    if (count == ALL_ENTRIES) drop_entry(count - 1);
    memmove(&entries[1], &entries[0], (size_t)count * sizeof(*entries)); count++;
    entries[0] = (struct entry){uid, data, size, time(NULL)};
    note("kept", uid, size);
}
/* text only: no NUL bytes */
static bool text_like(const char* data, size_t size) { return size && !memchr(data, 0, size); }

/* ---- reading a new selection ---- */
static void capture_end(struct capture* c, bool complete) {
    if (c->source) wl_event_source_remove(c->source);
    close(c->fd);
    if (complete && c->size <= AURORA_CLIPBOARD_MAX && text_like(c->data, c->size)) { keep(c->uid, c->data, c->size); c->data = NULL; }
    else if (complete && c->size) note(c->size > AURORA_CLIPBOARD_MAX ? "too large: not kept" : "not text: not kept", c->uid, c->size);
    free(c->data); memset(c, 0, sizeof(*c)); c->fd = -1;
}
static int capture_ready(int fd, uint32_t mask, void* data) {
    struct capture* c = data; (void)fd;
    for (;;) {
        if (c->size + 4096 > c->capacity) {
            if (c->capacity > AURORA_CLIPBOARD_MAX) { c->size = AURORA_CLIPBOARD_MAX + 1; capture_end(c, true); return 0; }   /* reports "too large" */
            size_t grow = c->capacity ? c->capacity * 2 : 8192; char* more = realloc(c->data, grow + 1);
            if (!more) { capture_end(c, false); return 0; }
            c->data = more; c->capacity = grow;
        }
        ssize_t n = read(c->fd, c->data + c->size, c->capacity - c->size);
        if (n > 0) { c->size += (size_t)n; continue; }
        if (n == 0) { capture_end(c, true); return 0; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) capture_end(c, true); return 0; }
        capture_end(c, false); return 0;
    }
}
void aurora_clipboard_capture(int fd, unsigned uid) {
    if (fd < 0) return;
    if (!loop) { close(fd); return; }
    struct capture* c = NULL;
    for (int i = 0; i < CAPTURES && !c; i++) if (captures[i].fd < 0) c = &captures[i];
    if (!c) {   /* a source that never finished writing: the oldest gives way */
        c = &captures[0]; for (int i = 1; i < CAPTURES; i++) if (captures[i].started < c->started) c = &captures[i];
        capture_end(c, false);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    *c = (struct capture){.fd = fd, .uid = uid, .started = ms()};
    c->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, capture_ready, c);
    if (!c->source) capture_end(c, false);
}

/* ---- serving Aurora's own selection (an entry put back) ---- */
static void serve_end(struct serve* s) { if (s->source) wl_event_source_remove(s->source); close(s->fd); free(s->data); memset(s, 0, sizeof(*s)); s->fd = -1; }
static int serve_ready(int fd, uint32_t mask, void* data) {
    struct serve* s = data; (void)fd;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { serve_end(s); return 0; }
    while (s->done < s->size) {
        /* the reader may have gone (a pipe): EPIPE, never SIGPIPE, whose default would end Aurora (pid 1) */
        struct sigaction ignore = {.sa_handler = SIG_IGN}, old; sigemptyset(&ignore.sa_mask); bool swapped = !sigaction(SIGPIPE, &ignore, &old);
        ssize_t n = write(s->fd, s->data + s->done, s->size - s->done); int error = errno;
        if (swapped) sigaction(SIGPIPE, &old, NULL);
        errno = error;
        if (n > 0) { s->done += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        break;
    }
    serve_end(s); return 0;
}
void aurora_clipboard_serve(int fd, const char* data, size_t size) {
    if (fd < 0) return;
    struct serve* s = NULL;
    for (int i = 0; i < SERVES && !s; i++) if (serves[i].fd < 0) s = &serves[i];
    if (!s) { s = &serves[0]; for (int i = 1; i < SERVES; i++) if (serves[i].started < s->started) s = &serves[i]; serve_end(s); }
    char* copy = malloc(size ? size : 1); if (!copy || !loop) { free(copy); close(fd); return; }
    memcpy(copy, data, size); fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    *s = (struct serve){.fd = fd, .data = copy, .size = size, .started = ms()};
    s->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE, serve_ready, s);
    if (!s->source) serve_end(s);
}

void aurora_clipboard_forget(unsigned uid) {
    int dropped = 0; for (int i = count - 1; i >= 0; i--) if (entries[i].uid == uid) { drop_entry(i); dropped++; }
    for (int i = 0; i < CAPTURES; i++) if (captures[i].fd >= 0 && captures[i].uid == uid) capture_end(&captures[i], false);
    if (dropped) { printf("[64] AURORA CLIPBOARD forgotten uid=%u entries=%d (session ended)\n", uid, dropped); fflush(stdout); }
}

/* ---- control commands: the requester's own (active) session ---- */
static unsigned requester(void) { return aurora_session_active_uid(); }   /* the control socket admits root and that user */
static int nth(unsigned uid, int index) {   /* entries[] position of the user's INDEX-th entry */
    for (int i = 0, k = 0; i < count; i++) if (entries[i].uid == uid && k++ == index) return i;
    return -1;
}
static bool index_argument(const char* args, unsigned uid, int* at, char* error, size_t size) {
    char* end; long v = strtol(args, &end, 10);
    if (!args[0] || *end || v < 0 || v > ALL_ENTRIES || (*at = nth(uid, (int)v)) < 0) { snprintf(error, size, "no such clipboard entry"); return false; }
    return true;
}
static bool control_history(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; unsigned uid = requester(); int mine = 0; time_t now = time(NULL);
    for (int i = 0; i < count; i++) if (entries[i].uid == uid) mine++;
    aurora_control_print(reply, "entries=%d\n", mine);
    for (int i = 0, k = 0; i < count; i++) {
        if (entries[i].uid != uid) continue;
        char preview[124]; size_t n = 0;
        for (size_t j = 0; j < entries[i].size && n < sizeof(preview) - 4; j++) {
            unsigned char c = (unsigned char)entries[i].data[j];
            preview[n++] = c == '\t' || c == '\n' || c == '\r' ? ' ' : c < 32 ? '?' : (char)c;
        }
        if (entries[i].size > n) { memcpy(preview + n, "...", 3); n += 3; }
        preview[n] = 0;
        aurora_control_print(reply, "entry\t%d\t%zu\t%ld\t%s\n", k++, entries[i].size, (long)(now - entries[i].when), preview);
    }
    return true;
}
static bool control_use(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; unsigned uid = requester(); int at;
    if (!index_argument(args, uid, &at, error, size)) return false;
    struct entry e = entries[at]; memmove(&entries[1], &entries[0], (size_t)at * sizeof(*entries)); entries[0] = e; entries[0].when = time(NULL);
    if (!aurora_host_set_text_selection(e.data, e.size)) { snprintf(error, size, "the clipboard could not be set"); return false; }
    note("use", uid, e.size); return true;
}
static bool control_remove(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; unsigned uid = requester(); int at;
    if (!index_argument(args, uid, &at, error, size)) return false;
    size_t bytes = entries[at].size; drop_entry(at); note("removed", uid, bytes); return true;
}
static bool control_clear(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)reply; (void)error; (void)size; unsigned uid = requester(); int dropped = 0;
    for (int i = count - 1; i >= 0; i--) if (entries[i].uid == uid) { drop_entry(i); dropped++; }
    printf("[64] AURORA CLIPBOARD cleared uid=%u entries=%d\n", uid, dropped); fflush(stdout); return true;
}
void aurora_clipboard_init(struct wl_event_loop* event_loop) {
    loop = event_loop;
    for (int i = 0; i < CAPTURES; i++) captures[i].fd = -1;
    for (int i = 0; i < SERVES; i++) serves[i].fd = -1;
    aurora_control_register("clipboard-history", false, control_history);
    aurora_control_register("clipboard-use", false, control_use);
    aurora_control_register("clipboard-remove", false, control_remove);
    aurora_control_register("clipboard-clear", false, control_clear);
}
