/* Windows programs on the Aurora desktop.
 *
 * Wine's winemenubuilder turns Start-menu shortcuts of installed programs into freedesktop entries
 * ($HOME/.local/share/applications/wine/Programs/.../NAME.desktop; file associations are wine-extension-EXT.desktop with
 * NoDisplay=true) and their icons into PNG files ($HOME/.local/share/icons/hicolor/<size>/apps/<Icon>.png).
 * wineapps_refresh() lists the visible entries after the fixed Aurora apps in the registry Spotlight and Launchpad
 * show, wineapps_launch() runs an entry (Exec unquoted per the Desktop Entry Specification, field codes dropped,
 * working folder from Path), and the icon hook draws the PNG (libpng is loaded on demand; without it, or without
 * the file, macui's generic icon is drawn). Also used by Files: wineapps_open_file() runs .exe/.msi/.lnk/.bat with
 * "wine start /unix FILE". */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aurora_wineapps.h"
#include "../../kernel/framebuffer.h"

struct wineapp { char name[64], exec[1024], path[512], icon[160], desktop[PATH_MAX]; int64_t mtime; uint32_t* pixels; int size; bool icon_tried; };
static struct wineapp apps[WINEAPPS_MAX];
static int app_count, scan_budget;   /* directory entries one refresh may still visit */
static ui_app_t registry[64 + WINEAPPS_MAX];
static pid_t launchers[16];   /* running launcher processes (start.exe /exec waits for the program, see wineapps_reaped) */

/* ---- .desktop parsing ---- */
static void unescape_string(const char* in, char* out, size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        if (*in == '\\' && in[1]) {
            char c = *++in; out[n++] = c == 's' ? ' ' : c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
        } else out[n++] = *in;
    }
    out[n] = 0;
}
/* A regular file of at most limit bytes, opened without blocking: a FIFO or a device (/dev/zero) behind a .desktop or
 * icon name must not stall the compositor. */
static FILE* open_regular(const char* file, off_t limit, struct stat* st) {
    int fd = open(file, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY); if (fd < 0) return NULL;
    FILE* f = !fstat(fd, st) && S_ISREG(st->st_mode) && st->st_size <= limit ? fdopen(fd, "r") : NULL;
    if (!f) close(fd);
    return f;
}
static bool read_entry(const char* file, struct wineapp* app) {
    struct stat st; FILE* f = open_regular(file, 65536, &st); if (!f) return false;
    char line[2048]; bool group = false, visible = true, application = false; memset(app, 0, sizeof(*app));
    app->mtime = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
    while (ftell(f) <= 65536 && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (line[0] == '[') { group = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!group || line[0] == '#') continue;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; const char* key = line; char* value = eq + 1;
        size_t k = strlen(key); while (k && key[k - 1] == ' ') line[--k] = 0;
        while (*value == ' ') value++;
        if (!strcmp(key, "Name")) unescape_string(value, app->name, sizeof(app->name));
        else if (!strcmp(key, "Exec")) unescape_string(value, app->exec, sizeof(app->exec));
        else if (!strcmp(key, "Path")) unescape_string(value, app->path, sizeof(app->path));
        else if (!strcmp(key, "Icon")) unescape_string(value, app->icon, sizeof(app->icon));
        else if (!strcmp(key, "Type")) application = !strcmp(value, "Application");
        else if ((!strcmp(key, "NoDisplay") || !strcmp(key, "Hidden")) && !strcmp(value, "true")) visible = false;
    }
    fclose(f);
    snprintf(app->desktop, sizeof(app->desktop), "%s", file);
    return application && visible && app->name[0] && app->exec[0];
}
static int compare_apps(const void* a, const void* b) { return strcasecmp(((const struct wineapp*)a)->name, ((const struct wineapp*)b)->name); }
/* The walk runs on the compositor thread: symlinked directories are not followed (a link to '.' or '/' would make it
 * endless) and one refresh visits at most 4096 entries. */
static void scan(const char* directory, int depth) {
    DIR* dir = opendir(directory); if (!dir) return;
    struct dirent* e;
    while (scan_budget >= 0 && (e = readdir(dir)) && app_count < WINEAPPS_MAX) {
        if (scan_budget-- == 0) { printf("[64] AURORA WINEAPPS SCAN LIMIT dir=%s\n", directory); fflush(stdout); break; }
        if (e->d_name[0] == '.') continue;
        char full[PATH_MAX]; if (snprintf(full, sizeof(full), "%s/%s", directory, e->d_name) >= (int)sizeof(full)) continue;
        struct stat st; if (lstat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 6) scan(full, depth + 1); continue; }
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 8, ".desktop")) continue;
        struct wineapp candidate;
        if (!read_entry(full, &candidate)) continue;   /* only a regular file (also behind a symlink) */
        bool duplicate = false;
        for (int i = 0; i < app_count; i++) if (!strcmp(apps[i].name, candidate.name) && !strcmp(apps[i].exec, candidate.exec)) duplicate = true;
        if (!duplicate) apps[app_count++] = candidate;
    }
    closedir(dir);
}
int wineapps_refresh(const ui_app_t* fixed, int fixed_count, int dock_count) {
    /* Decoded icons survive the rescan (each Spotlight/Launchpad open) while the entry (file, Icon, mtime) is unchanged. */
    struct wineapp* old = app_count ? malloc((size_t)app_count * sizeof(*old)) : NULL; int old_count = old ? app_count : 0;
    if (old) memcpy(old, apps, (size_t)old_count * sizeof(*old));
    else for (int i = 0; i < app_count; i++) free(apps[i].pixels);
    app_count = 0; scan_budget = 4096;
    const char* home = getenv("HOME"); char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s/.local/share/applications/wine", home && home[0] ? home : "/home/nexus");
    scan(root, 0);
    qsort(apps, (size_t)app_count, sizeof(apps[0]), compare_apps);
    for (int i = 0; i < app_count; i++) for (int k = 0; k < old_count; k++)
        if (old[k].pixels && old[k].mtime == apps[i].mtime && !strcmp(old[k].desktop, apps[i].desktop) && !strcmp(old[k].icon, apps[i].icon)) {
            apps[i].pixels = old[k].pixels; apps[i].size = old[k].size; apps[i].icon_tried = true; old[k].pixels = NULL; break;
        }
    for (int k = 0; k < old_count; k++) free(old[k].pixels);
    free(old);
    int n = 0;
    for (int i = 0; i < fixed_count && n < (int)(sizeof(registry) / sizeof(*registry)); i++) registry[n++] = fixed[i];
    for (int i = 0; i < app_count && n < (int)(sizeof(registry) / sizeof(*registry)); i++)
        registry[n++] = (ui_app_t){apps[i].name, WINEAPPS_ACTION_BASE + i, WINEAPPS_ICON_BASE + i, NULL};
    ui_set_apps(registry, n, dock_count);
    /* log the list when it changes (tests and diagnostics) */
    static char last[1024]; char names[1024] = ""; size_t used = 0;
    for (int i = 0; i < app_count && used + 2 < sizeof(names); i++) used += (size_t)snprintf(names + used, sizeof(names) - used, "%s%s", i ? "|" : "", apps[i].name);
    if (strcmp(names, last)) { strcpy(last, names); printf("[64] AURORA WINEAPPS count=%d names=%s\n", app_count, names); fflush(stdout); }
    return app_count;
}
const char* wineapps_name(int action) {
    int i = action - WINEAPPS_ACTION_BASE; return i >= 0 && i < app_count ? apps[i].name : NULL;
}
/* Exec tokenizer: shell-like quoting (backslash outside quotes; \" \` \$ \\ inside double quotes); field codes removed. */
static int split_exec(const char* exec, char* storage, size_t size, char** argv, int max) {
    int argc = 0; size_t used = 0; const char* p = exec;
    while (*p && argc + 1 < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char* arg = storage + used; size_t n = 0; char quote = 0; bool field_only = false;
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            char c = *p++;
            if (quote) {
                if (c == '"') { quote = 0; continue; }
                if (c == '\\' && (*p == '"' || *p == '`' || *p == '$' || *p == '\\')) c = *p++;
            } else if (c == '"') { quote = '"'; continue; }
            else if (c == '\\' && *p) c = *p++;
            else if (c == '%' && *p) {
                char code = *p++;
                if (code == '%') c = '%';
                else { if (!n && (*p == ' ' || !*p)) field_only = true; continue; }   /* %f %F %u %U %i %c %k ... */
            }
            if (used + n + 2 >= size) return -1;
            arg[n++] = c;
        }
        if (field_only && !n) continue;
        arg[n] = 0; used += n + 1; argv[argc++] = arg;
    }
    argv[argc] = NULL; return argc;
}
int wineapps_launch(int action, char* name, unsigned name_size) {
    int i = action - WINEAPPS_ACTION_BASE;
    if (i < 0 || i >= app_count) return 0;
    if (name && name_size) snprintf(name, name_size, "%s", apps[i].name);
    char storage[2048]; char* argv[64];
    if (split_exec(apps[i].exec, storage, sizeof(storage), argv, 64) <= 0) return -EINVAL;
    pid_t pid = fork();
    if (pid < 0) return -errno;
    if (!pid) {
        if (apps[i].path[0] && chdir(apps[i].path)) { const char* home = getenv("HOME"); if (home && chdir(home)) {} }
        signal(SIGPIPE, SIG_DFL); setsid();
        execvp(argv[0], argv); perror("Windows program"); _exit(127);
    }
    for (unsigned k = 0; k < sizeof(launchers) / sizeof(*launchers); k++) if (!launchers[k]) { launchers[k] = pid; break; }
    return pid;
}
bool wineapps_reaped(int pid) {
    for (unsigned k = 0; k < sizeof(launchers) / sizeof(*launchers); k++) if (pid > 0 && launchers[k] == pid) { launchers[k] = 0; return true; }
    return false;
}
static bool has_suffix(const char* path, const char* suffix) {
    size_t n = strlen(path), k = strlen(suffix); return n > k && !strcasecmp(path + n - k, suffix);
}
bool wineapps_is_windows_file(const char* path) {   /* .exe/.com need the MZ header: "example.com" may be a text file */
    static const char* const kinds[] = {".exe", ".msi", ".lnk", ".bat", ".cmd", ".com"};
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) if (has_suffix(path, kinds[i])) {
        if (i && i + 1 < sizeof(kinds) / sizeof(*kinds)) return true;
        char magic[2]; int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY); if (fd < 0) return false;
        bool program = read(fd, magic, 2) == 2 && magic[0] == 'M' && magic[1] == 'Z'; close(fd); return program;
    }
    return false;
}
int wineapps_open_file(const char* path) {
    if (access("/usr/bin/wine", X_OK)) return -ENOENT;
    pid_t pid = fork();
    if (pid < 0) return -errno;
    if (!pid) {
        char directory[PATH_MAX]; snprintf(directory, sizeof(directory), "%s", path);
        char* slash = strrchr(directory, '/'); if (slash && slash != directory) { *slash = 0; if (chdir(directory)) {} }
        signal(SIGPIPE, SIG_DFL); setsid();
        execl("/usr/bin/wine", "wine", "start", "/unix", path, (char*)NULL); perror("wine"); _exit(127);
    }
    return pid;
}

/* ---- icons: PNG through libpng's simplified API, loaded with dlopen ---- */
typedef struct { void* opaque; uint32_t version, width, height, format, flags, colormap_entries, warning_or_error; char message[64]; } png_image_t;
static int (*png_begin)(png_image_t*, FILE*);
static int (*png_finish)(png_image_t*, const void*, void*, int32_t, void*);
static void (*png_free_image)(png_image_t*);
static bool png_ready(void) {
    static int state;   /* 0 unknown, 1 loaded, -1 unavailable */
    if (state) return state > 0;
    void* lib = dlopen("libpng16.so.16", RTLD_NOW | RTLD_LOCAL); state = -1;
    if (!lib) return false;
    png_begin = (int (*)(png_image_t*, FILE*))dlsym(lib, "png_image_begin_read_from_stdio");
    png_finish = (int (*)(png_image_t*, const void*, void*, int32_t, void*))dlsym(lib, "png_image_finish_read");
    png_free_image = (void (*)(png_image_t*))dlsym(lib, "png_image_free");
    if (png_begin && png_finish && png_free_image) state = 1;
    return state > 0;
}
static void load_icon(struct wineapp* app) {
    app->icon_tried = true;
    if (!app->icon[0] || !png_ready()) return;
    const char* home = getenv("HOME"); char file[PATH_MAX];
    static const char* const sizes[] = {"256x256", "48x48", "64x64", "32x32", "16x16"};
    for (unsigned s = 0; s <= sizeof(sizes) / sizeof(*sizes); s++) {
        if (s == sizeof(sizes) / sizeof(*sizes)) { if (app->icon[0] != '/') return; snprintf(file, sizeof(file), "%s", app->icon); }
        else snprintf(file, sizeof(file), "%s/.local/share/icons/hicolor/%s/apps/%s.png", home && home[0] ? home : "/home/nexus", sizes[s], app->icon);
        struct stat st; FILE* f = open_regular(file, 4 << 20, &st); if (!f) continue;
        png_image_t image; memset(&image, 0, sizeof(image)); image.version = 1;
        if (!png_begin(&image, f)) { fclose(f); continue; }
        image.format = 0x13;   /* PNG_FORMAT_BGRA: little-endian 0xAARRGGBB words */
        if (!image.width || image.width != image.height || image.width > 512) { png_free_image(&image); fclose(f); continue; }
        uint32_t* pixels = malloc((size_t)image.width * image.height * 4);
        if (pixels && png_finish(&image, NULL, pixels, 0, NULL)) {
            fclose(f); app->pixels = pixels; app->size = (int)image.width;
            printf("[64] AURORA WINEAPPS ICON app=%s size=%d file=%s\n", app->name, app->size, file); fflush(stdout);
            return;
        }
        free(pixels); png_free_image(&image); fclose(f);
    }
}
static bool draw_icon(int icon, int x, int y, int size) {
    int i = icon - WINEAPPS_ICON_BASE;
    if (i < 0 || i >= app_count) return false;
    struct wineapp* app = &apps[i];
    if (!app->icon_tried) load_icon(app);
    if (!app->pixels) return false;
    uint32_t* canvas = fb_get_backbuffer(); int width = (int)fb_get_width(), height = (int)fb_get_height();
    if (!canvas) return false;
    for (int dy = 0; dy < size; dy++) {
        int cy = y + dy; if (cy < 0 || cy >= height) continue;
        for (int dx = 0; dx < size; dx++) {
            int cx = x + dx; if (cx < 0 || cx >= width) continue;
            uint32_t s = app->pixels[(dy * app->size / size) * app->size + dx * app->size / size], a = s >> 24;
            if (!a) continue;
            uint32_t* d = &canvas[cy * width + cx], o = *d;
            uint32_t r = (((s >> 16) & 255) * a + ((o >> 16) & 255) * (255 - a)) / 255;
            uint32_t g = (((s >> 8) & 255) * a + ((o >> 8) & 255) * (255 - a)) / 255;
            uint32_t b = ((s & 255) * a + (o & 255) * (255 - a)) / 255;
            *d = r << 16 | g << 8 | b;
        }
    }
    return true;
}
__attribute__((constructor)) static void wineapps_register(void) { ui_set_icon_hook(draw_icon); }
