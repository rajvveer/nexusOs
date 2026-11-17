/* System Settings host (aurora_settings.h): sidebar, page dispatch and the control-socket client. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "aurora_settings.h"
#include "aurora_extra.h"
#include "aurora_control.h"
#include "../../kernel/window.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

/* ---- the page table: one line per page ---- */
static const struct settings_page* const pages[] = {
    &settings_displays,
    &settings_users,
    &settings_keyboard,
    &settings_upgrade,   /* System Upgrade: FAT32 disk -> the full system (aurora_settings_upgrade.c, migrate_disk.py) */
    &settings_a11y,
    &settings_sound,
    &settings_appearance,
};
#define PAGE_COUNT ((int)(sizeof(pages) / sizeof(pages[0])))
#define SIDEBAR_W 200
static int current, window_id = -1;
static char status[200];
static bool needs_redraw = true;

void settings_message(const char* text) { snprintf(status, sizeof(status), "%s", text); needs_redraw = true; window_send_key(0); }
void settings_redraw(void) { needs_redraw = true; }
bool settings_request(const char* request, char* reply, size_t size) {
    if (size) reply[0] = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX}; const char* runtime = getenv("XDG_RUNTIME_DIR");   /* the session's own socket (aurora_session.h) */
    if (runtime && runtime[0] == '/' && strlen(runtime) < 80) snprintf(address.sun_path, sizeof(address.sun_path), "%s/aurora-control", runtime);
    else snprintf(address.sun_path, sizeof(address.sun_path), "%s", AURORA_CONTROL_SOCKET);
    if (fd < 0 || connect(fd, (struct sockaddr*)&address, sizeof(address))) {
        snprintf(reply, size, "cannot reach the desktop: %s", strerror(errno)); if (fd >= 0) close(fd); return false;
    }
    size_t length = strlen(request), sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, request + sent, length - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { snprintf(reply, size, "send: %s", strerror(errno)); close(fd); return false; }
        sent += (size_t)n;
    }
    char* text = malloc(65536); size_t used = 0; bool ok = false, done = false;
    if (!text) { close(fd); snprintf(reply, size, "out of memory"); return false; }
    while (!done && used < 65535) {
        struct pollfd p = {fd, POLLIN, 0};
        int ready = poll(&p, 1, 10000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { snprintf(reply, size, "the desktop did not answer"); break; }
        ssize_t n = recv(fd, text + used, 65535 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { snprintf(reply, size, "the desktop closed the connection"); break; }
        used += (size_t)n; text[used] = 0;
        /* the reply ends with a line "OK" or "ERROR ..." */
        char* last = used > 1 ? text + used - 2 : text; while (last > text && last[-1] != '\n') last--;
        if (used && text[used - 1] == '\n') {
            if (!strcmp(last, "OK\n")) { ok = done = true; *last = 0; snprintf(reply, size, "%s", text); }
            else if (!strncmp(last, "ERROR ", 6)) { done = true; last[strlen(last) - 1] = 0; snprintf(reply, size, "%s", last + 6); }
        }
    }
    free(text); close(fd);
    return ok;
}

/* ---- window ---- */
static void draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d; int x, y, w, h; window_content_px(id, &x, &y, &w, &h);
    appui_px_fill(x, y, w, h, APPUI_BG);
    appui_px_fill(x, y, SIDEBAR_W, h, APPUI_SIDEBAR); appui_px_vsep(x + SIDEBAR_W, y, h);
    for (int i = 0; i < PAGE_COUNT; i++) {
        int ry = y + 12 + i * 34;
        appui_px_row(x + 8, ry, SIDEBAR_W - 16, 30, i == current);
        appui_px_appicon(pages[i]->icon, x + 16, ry + 5, 20);
        appui_px_text(x + 44, ry + 7, pages[i]->name, APPUI_F_BODY, APPUI_TEXT);
    }
    int cx = x + SIDEBAR_W + 1, cw = w - SIDEBAR_W - 1, ch = h - 28;
    if (pages[current]->draw) pages[current]->draw(cx, y, cw, ch);
    appui_px_sep(cx, y + h - 28, cw);
    appui_px_text(cx + 12, y + h - 22, status, APPUI_F_SMALL, APPUI_SUBTLE);
    needs_redraw = false;
}
static void select_page(int index) {
    if (index < 0 || index >= PAGE_COUNT) return;
    current = index; status[0] = 0;
    if (pages[current]->enter) pages[current]->enter();
    printf("[64] SETTINGS PAGE %s\n", pages[current]->id); fflush(stdout);
}
static void click(int id, int x, int y) {
    (void)id;
    if (x < SIDEBAR_W) { int row = (y - 12) / 34; if (y >= 12 && row < PAGE_COUNT) select_page(row); return; }
    if (pages[current]->click) pages[current]->click(x - SIDEBAR_W - 1, y);
}
static void drag(int x, int y, int phase) {
    if (pages[current]->drag && (phase != 0 || x > SIDEBAR_W)) pages[current]->drag(x - SIDEBAR_W - 1, y, phase);
}
static void key(int id, char k) { (void)id; if (k && pages[current]->key) pages[current]->key(k); }
bool aurora_settings_tick(void) {
    bool changed = pages[current]->tick ? pages[current]->tick() : false;
    return changed || needs_redraw;
}
int aurora_settings_open(const char* page) {
    window_id = window_create("System Settings", 0, 0, 112, 42, draw, key);
    if (window_id < 0) return -1;
    window_set_on_click(window_id, click);
    aurora_app_set_drag(drag);
    int start = 0;
    for (int i = 0; page && i < PAGE_COUNT; i++) if (!strcmp(pages[i]->id, page)) start = i;
    select_page(start);
    return window_id;
}
