/* Aurora dialogs for the desktop portal (xdg-desktop-portal-aurora): aurora-app filechooser | prompt.
 * Parameters come in the environment (the portal backend starts the dialog in the requesting user's session):
 *   AURORA_DIALOG_FD       descriptor the result is written to (required): chosen absolute paths, one per line, or
 *                          "allow"/"deny" (prompt)
 *   AURORA_DIALOG_TITLE    window title;  AURORA_DIALOG_BODY  the question (prompt);  AURORA_DIALOG_ACCEPT  button label
 *   AURORA_DIALOG_MODE     open | open-multiple | folder | save   (filechooser)
 *   AURORA_DIALOG_FOLDER   first folder (default $HOME);  AURORA_DIALOG_NAME  proposed file name (save)
 *   AURORA_DIALOG_PATTERNS glob patterns of files to list, ';'-separated (folders are always listed)
 * Keyboard: Up/Down select, Enter opens a folder or chooses, Backspace goes up (or edits the name when saving), Space
 * marks files (open-multiple), Tab moves between the name field and the list / between the buttons, Esc cancels.
 * Exit status 0 with a result, 1 cancelled, 2 bad parameters. Lines "[64] AURORA DIALOG ..." go to stdout for tests. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aurora_dialogs.h"
#include "aurora_places.h"
#include "../../kernel/window.h"
#include "../../kernel/appui.h"
#include "../../kernel/macui.h"

enum { OPEN, OPEN_MULTIPLE, FOLDER, SAVE };
static int mode, result_fd = -1;
static char folder[PATH_MAX], name[256], patterns[512], status_line[256], body[1024], accept_label[64];
struct entry { char* name; bool dir, marked; };
static struct entry* entries; static size_t count, selected; static int scroll, rows = 1;
static bool name_focus, deny_focus = true;

static void finish(const char* text, int code) {
    if (text && result_fd >= 0) { size_t n = strlen(text); if (write(result_fd, text, n) != (ssize_t)n) code = 1; }
    if (result_fd >= 0) close(result_fd);
    printf("[64] AURORA DIALOG %s\n", code ? "CANCELLED" : "DONE"); fflush(stdout);
    exit(code);
}
static bool listed(const char* file) {
    if (!patterns[0]) return true;
    char copy[sizeof(patterns)]; snprintf(copy, sizeof(copy), "%s", patterns);
    for (char* save = NULL, *p = strtok_r(copy, ";", &save); p; p = strtok_r(NULL, ";", &save)) if (!fnmatch(p, file, FNM_CASEFOLD)) return true;
    return false;
}
static int by_kind_then_name(const void* a, const void* b) {
    const struct entry *x = a, *y = b; if (x->dir != y->dir) return x->dir ? -1 : 1; return strcmp(x->name, y->name);
}
static void load(void) {
    for (size_t i = 0; i < count; i++) free(entries[i].name);
    free(entries); entries = NULL; count = selected = 0; scroll = 0;
    DIR* d = opendir(folder); if (!d) { snprintf(status_line, sizeof(status_line), "%s: %s", folder, strerror(errno)); return; }
    struct dirent* e;
    while ((e = readdir(d)) && count < 20000) {
        if (e->d_name[0] == '.') continue;   /* hidden files are not offered */
        char full[PATH_MAX]; struct stat st;
        if ((size_t)snprintf(full, sizeof(full), "%s/%s", folder, e->d_name) >= sizeof(full) || stat(full, &st)) continue;
        bool dir = S_ISDIR(st.st_mode);
        if (!dir && (mode == FOLDER || !S_ISREG(st.st_mode) || !listed(e->d_name))) continue;
        struct entry* grown = realloc(entries, (count + 1) * sizeof(*entries)); if (!grown) break;
        entries = grown; entries[count].name = strdup(e->d_name); entries[count].dir = dir; entries[count].marked = false;
        if (!entries[count].name) break;
        count++;
    }
    closedir(d); qsort(entries, count, sizeof(*entries), by_kind_then_name);
    snprintf(status_line, sizeof(status_line), "%zu items", count);
    printf("[64] AURORA DIALOG FOLDER path=%s items=%zu\n", folder, count); fflush(stdout);
}
static void go(const char* path) { char real[PATH_MAX]; if (realpath(path, real)) { snprintf(folder, sizeof(folder), "%s", real); load(); } }
static void up(void) { char next[PATH_MAX]; snprintf(next, sizeof(next), "%s", folder); char* slash = strrchr(next, '/'); if (slash && slash != next) *slash = 0; else strcpy(next, "/"); go(next); }
static void choose(void) {
    char out[65536] = ""; size_t used = 0;
#define ADD(path) do { int n_ = snprintf(out + used, sizeof(out) - used, "%s\n", path); if (n_ < 0 || (size_t)n_ >= sizeof(out) - used) finish(NULL, 1); used += (size_t)n_; } while (0)
    if (mode == SAVE) {
        if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) { snprintf(status_line, sizeof(status_line), "Type a file name."); return; }
        char full[PATH_MAX]; if ((size_t)snprintf(full, sizeof(full), "%s/%s", folder, name) >= sizeof(full)) return;
        ADD(full); finish(out, 0);
    }
    if (mode == FOLDER) {
        char full[PATH_MAX]; if (count && selected < count && entries[selected].dir) snprintf(full, sizeof(full), "%s/%s", folder, entries[selected].name); else snprintf(full, sizeof(full), "%s", folder);
        ADD(full); finish(out, 0);
    }
    bool any = false;
    for (size_t i = 0; mode == OPEN_MULTIPLE && i < count; i++) if (entries[i].marked) { char full[PATH_MAX]; snprintf(full, sizeof(full), "%s/%s", folder, entries[i].name); ADD(full); any = true; }
    if (!any) {
        if (!count || selected >= count) return;
        if (entries[selected].dir) { char next[PATH_MAX]; snprintf(next, sizeof(next), "%s/%s", folder, entries[selected].name); go(next); return; }
        char full[PATH_MAX]; snprintf(full, sizeof(full), "%s/%s", folder, entries[selected].name); ADD(full);
    }
#undef ADD
    finish(out, 0);
}
static void chooser_key(int id, char key) {
    (void)id; unsigned char k = (unsigned char)key;
    if (key == 27) finish(NULL, 1);
    if (key == '\t' && mode == SAVE) { name_focus = !name_focus; return; }
    if (mode == SAVE && name_focus) {
        size_t n = strlen(name);
        if (key == '\b') { if (n) name[n - 1] = 0; return; }
        if (key == '\n') { choose(); return; }
        if (k >= 32 && k < 127 && key != '/' && n + 1 < sizeof(name)) { name[n] = key; name[n + 1] = 0; }
        return;
    }
    if (k == 0x80 && selected) selected--;
    if (k == 0x81 && selected + 1 < count) selected++;
    if (key == '\b') up();
    if (key == ' ' && mode == OPEN_MULTIPLE && selected < count && !entries[selected].dir) entries[selected].marked = !entries[selected].marked;
    if (key == '\n') {
        if (count && selected < count && entries[selected].dir && mode != FOLDER) { char next[PATH_MAX]; snprintf(next, sizeof(next), "%s/%s", folder, entries[selected].name); go(next); return; }
        choose();
    }
}
static int list_top(void) { return mode == SAVE ? 120 : 82; }
static void chooser_click(int id, int x, int y) {
    (void)id;
    if (x < PLACES_WIDTH) { char next[PATH_MAX], note[256]; snprintf(next, sizeof(next), "%s", folder); int hit = places_click(x, y, next, sizeof(next), note, sizeof(note)); if (hit & 1) go(next); return; }
    x -= PLACES_WIDTH;
    if (y >= 8 && y < 38) { if (x >= 12 && x < 92) up(); else if (x >= 102 && x < 192) finish(NULL, 1); else if (x >= 202 && x < 302) choose(); return; }
    if (mode == SAVE && y >= 84 && y < 112) { name_focus = true; return; }
    int top = list_top();
    if (y >= top && y < top + rows * 28) { size_t row = (size_t)((y - top) / 28 + scroll); if (row < count) { if (row == selected) chooser_key(id, '\n'); else selected = row; name_focus = false; } }
}
static void chooser_draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d; int x, y, w, h; window_content_px(id, &x, &y, &w, &h); appui_px_fill(x, y, w, h, APPUI_BG);
    places_draw(x, y, h - 28, folder); x += PLACES_WIDTH; w -= PLACES_WIDTH;
    appui_px_button(x + 12, y + 8, 80, 30, "Up", false); appui_px_button(x + 102, y + 8, 90, 30, "Cancel", false);
    appui_px_button(x + 202, y + 8, 100, 30, accept_label[0] ? accept_label : mode == SAVE ? "Save" : mode == FOLDER ? "Choose" : "Open", true);
    appui_px_text(x + 14, y + 48, folder, APPUI_F_BODY, APPUI_TEXT);
    if (mode == SAVE) { appui_px_text(x + 14, y + 90, "Name:", APPUI_F_BODY, APPUI_SUBTLE); appui_px_field(x + 70, y + 84, w - 90, 28, name, name_focus); }
    int top = list_top(); appui_px_sep(x, y + top - 6, w);
    rows = (h - top - 36) / 28; if (rows < 1) rows = 1;
    if (selected < (size_t)scroll) scroll = (int)selected; if (selected >= (size_t)(scroll + rows)) scroll = (int)selected - rows + 1;
    for (int r = 0; r < rows && (size_t)(scroll + r) < count; r++) {
        size_t n = (size_t)(scroll + r); appui_px_row(x + 8, y + top + r * 28, w - 16, 26, n == selected && !name_focus);
        ui_icon(entries[n].dir ? ICON_FOLDER : ICON_NOTES, x + 14, y + top + 2 + r * 28, 20);
        char label[300]; snprintf(label, sizeof(label), "%s%s", entries[n].marked ? "[x] " : "", entries[n].name);
        appui_px_text(x + 44, y + top + 2 + r * 28, label, APPUI_F_BODY, APPUI_TEXT);
    }
    appui_px_sep(x, y + h - 28, w); appui_px_text(x + 12, y + h - 22, status_line, APPUI_F_SMALL, APPUI_SUBTLE);
}
/* ---- prompt ---- */
static void prompt_key(int id, char key) {
    (void)id;
    if (key == 27) finish("deny\n", 1);
    if (key == '\t' || (unsigned char)key == 0x82 || (unsigned char)key == 0x83) { deny_focus = !deny_focus; return; }
    if (key == '\n' || key == ' ') { if (deny_focus) finish("deny\n", 1); else finish("allow\n", 0); }
}
static void prompt_click(int id, int x, int y) {
    int cx, cy, w, h; window_content_px(id, &cx, &cy, &w, &h); (void)cx; (void)cy;
    if (y >= h - 52 && y < h - 16) { if (x >= w - 250 && x < w - 140) finish("deny\n", 1); if (x >= w - 130 && x < w - 20) finish("allow\n", 0); }
}
static void prompt_draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d; int x, y, w, h; window_content_px(id, &x, &y, &w, &h); appui_px_fill(x, y, w, h, APPUI_BG);
    int line_y = y + 20; char copy[sizeof(body)]; snprintf(copy, sizeof(copy), "%s", body);
    for (char* save = NULL, *line = strtok_r(copy, "\n", &save); line && line_y < y + h - 70; line = strtok_r(NULL, "\n", &save)) { appui_px_text(x + 20, line_y, line, APPUI_F_BODY, APPUI_TEXT); line_y += 26; }
    appui_px_button(x + w - 250, y + h - 52, 110, 34, "Deny", deny_focus); appui_px_button(x + w - 130, y + h - 52, 110, 34, accept_label[0] ? accept_label : "Allow", !deny_focus);
}
static void copy_env(char* out, size_t size, const char* key, const char* fallback) {
    const char* v = getenv(key); snprintf(out, size, "%s", v ? v : fallback);
    for (char* p = out; *p; p++) if ((unsigned char)*p < 32 && *p != '\n') *p = ' ';
}
int aurora_dialog_open(const char* kind) {
    const char* fd = getenv("AURORA_DIALOG_FD"); char* end; long n = fd ? strtol(fd, &end, 10) : -1;
    if (!fd || *end || n < 3 || n > 1024 || fcntl((int)n, F_GETFD) < 0) { fprintf(stderr, "aurora-app %s: AURORA_DIALOG_FD missing or invalid\n", kind); exit(2); }
    result_fd = (int)n;
    char title[128]; copy_env(title, sizeof(title), "AURORA_DIALOG_TITLE", !strcmp(kind, "prompt") ? "Permission" : "Choose a File");
    copy_env(accept_label, sizeof(accept_label), "AURORA_DIALOG_ACCEPT", "");
    if (!strcmp(kind, "prompt")) {
        copy_env(body, sizeof(body), "AURORA_DIALOG_BODY", "Allow this?");
        printf("[64] AURORA DIALOG PROMPT title=%s\n", title); fflush(stdout);
        int id = window_create(title, 0, 0, 70, 14, prompt_draw, prompt_key); window_set_on_click(id, prompt_click); return id;
    }
    const char* m = getenv("AURORA_DIALOG_MODE"); m = m ? m : "open";
    mode = !strcmp(m, "save") ? SAVE : !strcmp(m, "folder") ? FOLDER : !strcmp(m, "open-multiple") ? OPEN_MULTIPLE : !strcmp(m, "open") ? OPEN : -1;
    if (mode < 0) { fprintf(stderr, "aurora-app filechooser: unknown AURORA_DIALOG_MODE %s\n", m); exit(2); }
    copy_env(patterns, sizeof(patterns), "AURORA_DIALOG_PATTERNS", ""); copy_env(name, sizeof(name), "AURORA_DIALOG_NAME", "");
    if (strchr(name, '/')) name[0] = 0;
    name_focus = mode == SAVE;
    const char* first = getenv("AURORA_DIALOG_FOLDER"); const char* home = getenv("HOME");
    if (!first || first[0] != '/' || !realpath(first, folder)) { if (!home || !realpath(home, folder)) strcpy(folder, "/"); }
    load();
    printf("[64] AURORA DIALOG FILECHOOSER mode=%s title=%s\n", m, title); fflush(stdout);
    int id = window_create(title, 0, 0, 94, 38, chooser_draw, chooser_key); window_set_on_click(id, chooser_click); return id;
}
