/* aurora-app's accessible tree (aurora_a11y_client.h): what the appui kit drew in the last frame, as text, sent to the
 * screen reader bridge (aurora-a11y-bridge) on $XDG_RUNTIME_DIR/aurora-a11y-apps when it changed. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "aurora_a11y_client.h"
#include "../../kernel/appui.h"

#define DRAWN 256
struct drawn { int kind, x, y, w, h, state; char text[128]; bool used; };
static struct drawn drawn[DRAWN]; static int count;
static int fd = -1; static uint64_t last_try, sent;
static char record[16384]; static size_t used;

static void note_drawn(int kind, int x, int y, int w, int h, const char* text, int state) {
    if (count == DRAWN) return;
    struct drawn* d = &drawn[count++]; *d = (struct drawn){kind, x, y, w, h, state, "", false};
    snprintf(d->text, sizeof(d->text), "%s", text ? text : "");
    for (char* p = d->text; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
}
static uint64_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static bool connected(void) {
    if (fd >= 0) return true;
    if (ms() - last_try < 3000) return false;
    last_try = ms(); const char* run = getenv("XDG_RUNTIME_DIR"); if (!run || run[0] != '/') return false;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; if (snprintf(a.sun_path, sizeof(a.sun_path), "%s/aurora-a11y-apps", run) >= (int)sizeof(a.sun_path)) return false;
    if (access(a.sun_path, F_OK)) return false;
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) return false;
    if (connect(fd, (struct sockaddr*)&a, sizeof(a))) { close(fd); fd = -1; return false; }
    sent = 0; return true;
}
bool aurora_a11y_tick(void) {
    static uint64_t checked; if (ms() - checked < 1000) return false;
    checked = ms();
    if (fd >= 0) {   /* the bridge went: forget it (a new one gets a full record) */
        struct pollfd p = {fd, POLLIN, 0}; char byte;
        if (poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR) || recv(fd, &byte, 1, MSG_DONTWAIT) == 0)) { close(fd); fd = -1; last_try = 0; }
        return false;
    }
    return connected();
}
void aurora_a11y_paint_begin(void) { count = 0; appui_a11y_note = connected() ? note_drawn : NULL; }
static void put(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void put(const char* format, ...) {
    va_list list; va_start(list, format); int n = vsnprintf(record + used, sizeof(record) - used, format, list); va_end(list);
    if (n > 0) used = used + (size_t)n < sizeof(record) ? used + (size_t)n : sizeof(record) - 1;
}
static bool same_row(const struct drawn* a, const struct drawn* b) { int ca = a->y + a->h / 2, cb = b->y + b->h / 2; return abs(ca - cb) <= 14; }
static bool inside(const struct drawn* box, const struct drawn* d) { return d->x >= box->x && d->y >= box->y && d->x < box->x + box->w && d->y < box->y + box->h; }
/* the text label just left of (or just above) a control */
static int label_for(int control) {
    const struct drawn* c = &drawn[control]; int best = -1, distance = 1 << 30;
    for (int i = 0; i < count; i++) {
        const struct drawn* t = &drawn[i]; if (t->kind != APPUI_A11Y_TEXT || t->used || !t->text[0]) continue;
        int dx = c->x - (t->x + t->w), dy = c->y - (t->y + t->h);
        if (same_row(t, c) && dx >= -4 && dx < distance) { best = i; distance = dx; }
        else if (dy >= -2 && dy < 26 && abs(t->x - c->x) < 40 && 1000 + dy < distance) { best = i; distance = 1000 + dy; }
    }
    return best;
}
static bool secret(const char* text) { if (!text[0]) return false; for (const char* p = text; *p; p++) if (*p != '*') return false; return true; }
void aurora_a11y_paint_end(const char* app, const char* title, bool active) {
    if (!appui_a11y_note) return;
    appui_a11y_note = NULL;
    used = 0; int focus = -1;
    for (int i = 0; i < count; i++) drawn[i].used = false;
    /* names: a row is named by the text inside it, a toggle or field by its label */
    for (int i = 0; i < count; i++) if (drawn[i].kind == APPUI_A11Y_ROW)
        for (int j = 0; j < count; j++) if (drawn[j].kind == APPUI_A11Y_TEXT && !drawn[j].used && inside(&drawn[i], &drawn[j])) {
            if (!drawn[i].text[0]) snprintf(drawn[i].text, sizeof(drawn[i].text), "%s", drawn[j].text);
            drawn[j].used = true;
        }
    char names[DRAWN][128]; memset(names, 0, sizeof(names));
    for (int i = 0; i < count; i++) if (drawn[i].kind == APPUI_A11Y_TOGGLE || drawn[i].kind == APPUI_A11Y_FIELD) {
        int l = label_for(i); if (l >= 0) { snprintf(names[i], sizeof(names[i]), "%s", drawn[l].text); drawn[l].used = true; }
    }
    for (int i = 0; i < count && focus < 0; i++) if (drawn[i].kind == APPUI_A11Y_FIELD && (drawn[i].state & 1)) focus = i;
    for (int i = 0; i < count && focus < 0; i++) if (drawn[i].kind == APPUI_A11Y_ROW && drawn[i].state) focus = i;
    put("app\t%ld\t%s\t%s\t%d\n", (long)getpid(), app, title, active);
    if (focus >= 0) put("focus w/%d\n", focus); else put("focus w\n");
    put("node\tw\t\tframe\t%s\t%s\t\t\n", active ? "active" : "", title);
    for (int i = 0; i < count; i++) {
        const struct drawn* d = &drawn[i]; const char* role = NULL; char states[96] = ""; const char* name = d->text; const char* text = ""; bool hidden;
        switch (d->kind) {
        case APPUI_A11Y_TEXT: if (d->used || !d->text[0]) continue; role = d->state == APPUI_F_TITLE ? "heading" : "label"; break;
        case APPUI_A11Y_BUTTON: role = "push_button"; strcpy(states, d->state ? "focusable,default" : "focusable"); break;
        case APPUI_A11Y_FIELD:
            hidden = (d->state & 2) || secret(d->text);   /* a password field (appui_px_secret_field, or only '*'): never its text */
            role = hidden ? "password_text" : "entry"; name = names[i]; text = hidden ? "" : d->text;
            snprintf(states, sizeof(states), "focusable,editable,single_line%s", d->state & 1 ? ",focused" : ""); break;
        case APPUI_A11Y_TOGGLE: role = "check_box"; name = names[i]; snprintf(states, sizeof(states), "focusable,checkable%s", d->state ? ",checked" : ""); break;
        case APPUI_A11Y_ROW: role = "list_item"; snprintf(states, sizeof(states), "focusable,selectable%s", d->state ? ",selected,focused" : ""); break;
        case APPUI_A11Y_SEGMENT: role = "radio_button"; snprintf(states, sizeof(states), "focusable,checkable%s", d->state ? ",checked" : ""); break;
        default: continue;
        }
        if (focus != i) { char* f = strstr(states, ",focused"); if (f) *f = 0; }
        put("node\tw/%d\tw\t%s\t%s\t%s\t\t%s\n", i, role, states, name, text);
    }
    uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < used; i++) { h ^= (unsigned char)record[i]; h *= 1099511628211ULL; }
    if (h == sent) return;
    if (send(fd, record, used, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)used) sent = h;
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) { close(fd); fd = -1; }
}
