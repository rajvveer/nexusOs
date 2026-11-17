/* nexus-software - the Software app: search, install and remove Debian packages, see and apply updates.
 * Reading (search, installed list, available updates) runs as the user (apt-cache, dpkg-query, apt list). Changes run
 * /usr/lib/nexus/nexus-pkg-helper through sudo with the password typed into the window, so sudoers (group sudo) and
 * PAM decide; the helper reports apt's progress (APT::Status-Fd) on stdout and its log on stderr.
 * Tabs: Explore (search the archive), Installed (filter), Updates (Check for updates = apt-get update, Update all =
 * full-upgrade). Test hooks: "[64] SOFTWARE ..." lines on stdout (tests/test_x64_software.py). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "../../kernel/uifont.h"

enum tab {EXPLORE, INSTALLED, UPDATES};
enum query {Q_NONE, Q_INSTALLED, Q_SEARCH, Q_UPDATES};
struct item { char name[96], version[64], summary[160], old[64]; bool installed; };
static struct item* items; static size_t item_count, selected, scroll;
struct installed { char name[96], version[64], summary[160]; };
static struct installed* inst; static size_t inst_count;
static enum tab tab = EXPLORE;
static char search[96], password[128], message[256] = "Search for applications and tools from Debian.", log_text[16384];
static size_t log_size; static int progress = -1; static bool asking, focus_password, closed, redraw = true, busy, configured;
static char pending_op[16], pending_args[8][96]; static int pending_count;
/* background processes: a read-only query, or the privileged job */
static pid_t query_pid = -1, job_pid = -1; static enum query query_kind; static int query_fd = -1, job_out = -1, job_err = -1;
static char* query_buf; static size_t query_size, query_cap; static char status_line[512]; static size_t status_len;
static struct wl_display* display; static struct wl_compositor* compositor; static struct wl_shm* shm; static struct xdg_wm_base* shell;
static struct wl_surface* surface; static struct xdg_surface* xdg; static struct xdg_toplevel* top;
static struct wl_buffer* buffer; static uint32_t* pixels; static size_t buffer_size; static int buffer_w, buffer_h;
static int width = 1100, height = 760, mouse_x, mouse_y;
static struct xkb_context* xkb; static struct xkb_keymap* keymap; static struct xkb_state* kstate;
static int card_x, card_y, card_w, card_h, list_y, rows, bottom_y;

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("[64] SOFTWARE "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); }
static void set_message(const char* text) { snprintf(message, sizeof(message), "%s", text); redraw = true; }
static void log_append(const char* data, size_t n) {
    const size_t limit = sizeof(log_text) - 1;
    if (n >= limit) { data += n - limit; n = limit; log_size = 0; }
    if (n > limit - log_size) { size_t drop = n - (limit - log_size); memmove(log_text, log_text + drop, log_size - drop); log_size -= drop; }
    memcpy(log_text + log_size, data, n); log_size += n; log_text[log_size] = 0; redraw = true;
}
static bool valid_name(const char* s) {
    if (!*s || strlen(s) >= 96 || s[0] == '-') return false;
    for (const char* p = s; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("+.-:", *p))) return false;
    return true;
}
/* ------------------------------------------------------------------------------------------------ queries */
static void start_query(enum query kind) {
    if (query_pid > 0) { kill(query_pid, SIGTERM); waitpid(query_pid, NULL, 0); query_pid = -1; if (query_fd >= 0) { close(query_fd); query_fd = -1; } }
    int p[2]; if (pipe2(p, O_CLOEXEC)) { set_message("Could not start a query."); return; }
    char q[100] = ""; for (const char* s = search; *s && strlen(q) < 95; s++) if ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.') { size_t l = strlen(q); q[l] = *s; q[l + 1] = 0; }
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); set_message("Could not start a query."); return; }
    if (!pid) {
        dup2(p[1], 1); int null = open("/dev/null", O_RDWR); if (null >= 0) { dup2(null, 0); dup2(null, 2); }
        setenv("LC_ALL", "C.UTF-8", 1);
        if (kind == Q_INSTALLED) execl("/usr/bin/dpkg-query", "dpkg-query", "-W", "-f", "${db:Status-Abbrev}\t${Package}\t${Version}\t${binary:Summary}\n", (char*)NULL);
        if (kind == Q_UPDATES) execl("/usr/bin/apt", "apt", "list", "--upgradable", (char*)NULL);
        if (kind == Q_SEARCH) execl("/usr/bin/apt-cache", "apt-cache", "search", "--names-only", "--", q[0] ? q : ".", (char*)NULL);
        _exit(127);
    }
    close(p[1]); query_fd = p[0]; fcntl(query_fd, F_SETFL, O_NONBLOCK); query_pid = pid; query_kind = kind; query_size = 0;
}
static int cmp_items(const void* a, const void* b) { return strcmp(((const struct item*)a)->name, ((const struct item*)b)->name); }
static const struct installed* find_installed(const char* name) {
    for (size_t i = 0; i < inst_count; i++) if (!strcmp(inst[i].name, name)) return &inst[i];
    return NULL;
}
static void add_item(const char* name, const char* version, const char* summary, const char* old, bool installed) {
    if (item_count % 256 == 0) { struct item* grown = realloc(items, (item_count + 256) * sizeof(*items)); if (!grown) return; items = grown; }
    struct item* it = &items[item_count++]; memset(it, 0, sizeof(*it));
    snprintf(it->name, sizeof(it->name), "%s", name); snprintf(it->version, sizeof(it->version), "%s", version);
    snprintf(it->summary, sizeof(it->summary), "%s", summary); snprintf(it->old, sizeof(it->old), "%s", old); it->installed = installed;
}
static void rebuild_installed_view(void) {
    item_count = 0; selected = scroll = 0;
    for (size_t i = 0; i < inst_count; i++) if (!search[0] || strstr(inst[i].name, search)) add_item(inst[i].name, inst[i].version, inst[i].summary, "", true);
}
static void finish_query(void) {
    if (!query_buf) return;
    query_buf[query_size] = 0;
    if (query_kind == Q_INSTALLED) {
        inst_count = 0; free(inst); inst = NULL; size_t cap = 0;
        for (char* save = NULL, *line = strtok_r(query_buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char* f[4] = {line, NULL, NULL, NULL}; for (int k = 1; k < 4; k++) { f[k] = strchr(f[k - 1], '\t'); if (!f[k]) break; *f[k]++ = 0; }
            if (!f[3] || strncmp(f[0], "ii", 2)) continue;
            if (inst_count == cap) { cap = cap ? cap * 2 : 512; struct installed* g = realloc(inst, cap * sizeof(*inst)); if (!g) break; inst = g; }
            snprintf(inst[inst_count].name, 96, "%s", f[1]); snprintf(inst[inst_count].version, 64, "%s", f[2]); snprintf(inst[inst_count].summary, 160, "%s", f[3]); inst_count++;
        }
        say("INSTALLED count=%zu", inst_count);
        if (tab == INSTALLED) rebuild_installed_view();
        if (tab == EXPLORE && search[0]) start_query(Q_SEARCH);
    } else if (query_kind == Q_SEARCH && tab == EXPLORE) {
        item_count = 0; selected = scroll = 0;
        for (char* save = NULL, *line = strtok_r(query_buf, "\n", &save); line && item_count < 400; line = strtok_r(NULL, "\n", &save)) {
            char* dash = strstr(line, " - "); if (!dash) continue; *dash = 0;
            const struct installed* in = find_installed(line);
            add_item(line, in ? in->version : "", dash + 3, "", in != NULL);
        }
        qsort(items, item_count, sizeof(*items), cmp_items);
        say("SEARCH query=%s results=%zu", search, item_count);
        snprintf(message, sizeof(message), item_count ? "%zu packages found." : "No packages found.", item_count);
    } else if (query_kind == Q_UPDATES && tab == UPDATES) {
        item_count = 0; selected = scroll = 0;
        for (char* save = NULL, *line = strtok_r(query_buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char name[96], suite[96], version[64], arch[32], old[64];
            if (sscanf(line, "%95[^/]/%95s %63s %31s [upgradable from: %63[^]]]", name, suite, version, arch, old) == 5) add_item(name, version, suite, old, true);
        }
        say("UPDATES count=%zu", item_count);
        snprintf(message, sizeof(message), item_count ? "%zu updates available." : "Your system is up to date.", item_count);
    }
    redraw = true;
}
static void query_events(void) {
    if (query_fd >= 0) {
        for (;;) {
            if (query_size + 4096 + 1 > query_cap) { size_t cap = query_cap ? query_cap * 2 : 65536; if (cap > (64 << 20)) break; char* g = realloc(query_buf, cap); if (!g) break; query_buf = g; query_cap = cap; }
            ssize_t n = read(query_fd, query_buf + query_size, 4096);
            if (n > 0) { query_size += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            close(query_fd); query_fd = -1; break;
        }
    }
    if (query_pid > 0 && query_fd < 0) { int status; if (waitpid(query_pid, &status, WNOHANG) == query_pid) { query_pid = -1; finish_query(); } }
}
/* ------------------------------------------------------------------------------------------------ privileged jobs */
static void start_job(void) {
    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC)) return;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return; }
    if (pipe2(err, O_CLOEXEC)) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return; }
    char* argv[16]; int n = 0;
    argv[n++] = "sudo"; argv[n++] = "-S"; argv[n++] = "-k"; argv[n++] = "-p"; argv[n++] = ""; argv[n++] = "/usr/lib/nexus/nexus-pkg-helper"; argv[n++] = pending_op;
    for (int i = 0; i < pending_count && n < 15; i++) argv[n++] = pending_args[i];
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid < 0) { set_message("Could not start the package manager."); return; }
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); dup2(err[1], 2); setenv("LC_ALL", "C.UTF-8", 1); execv("/usr/bin/sudo", argv); _exit(127); }
    close(in[0]); close(out[1]); close(err[1]);
    char secret[200]; int len = snprintf(secret, sizeof(secret), "%s\n", password);
    if (len > 0 && write(in[1], secret, (size_t)len) < 0) {}
    close(in[1]); memset(secret, 0, sizeof(secret)); memset(password, 0, sizeof(password));
    job_out = out[0]; job_err = err[0]; fcntl(job_out, F_SETFL, O_NONBLOCK); fcntl(job_err, F_SETFL, O_NONBLOCK);
    job_pid = pid; progress = 0; log_size = 0; log_text[0] = 0; status_len = 0;
    say("JOB start op=%s count=%d first=%s", pending_op, pending_count, pending_count ? pending_args[0] : "-");
    set_message(!strcmp(pending_op, "install") ? "Installing..." : !strcmp(pending_op, "remove") ? "Removing..." : !strcmp(pending_op, "update") ? "Checking for updates..." : "Updating...");
}
static void status_input(const char* line) {
    /* dlstatus:N:PERCENT:text  pmstatus:PKG:PERCENT:text  pmerror:PKG:PERCENT:text */
    char kind[16]; char rest[400]; if (sscanf(line, "%15[^:]:%399[^\n]", kind, rest) != 2) return;
    char* a = strchr(rest, ':'); if (!a) return; char* b = strchr(a + 1, ':'); if (!b) return;
    *b = 0; double pct = atof(a + 1);
    if (!strcmp(kind, "pmstatus") || !strcmp(kind, "dlstatus")) {
        int p = !strcmp(kind, "dlstatus") ? (int)(pct * 0.3) : 30 + (int)(pct * 0.7); if (p > progress) progress = p;
        snprintf(message, sizeof(message), "%s", b + 1); say("PROGRESS %d %s", progress, kind);
    } else if (!strcmp(kind, "pmerror")) snprintf(message, sizeof(message), "Error: %s", b + 1);
    redraw = true;
}
static void job_events(void) {
    char data[4096];
    if (job_out >= 0) for (;;) {
        ssize_t n = read(job_out, data, sizeof(data));
        if (n > 0) { for (ssize_t i = 0; i < n; i++) { if (data[i] == '\n' || status_len == sizeof(status_line) - 1) { status_line[status_len] = 0; status_input(status_line); status_len = 0; } else status_line[status_len++] = data[i]; } continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        close(job_out); job_out = -1; break;
    }
    if (job_err >= 0) for (;;) {
        ssize_t n = read(job_err, data, sizeof(data));
        if (n > 0) { log_append(data, (size_t)n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        close(job_err); job_err = -1; break;
    }
    if (job_pid > 0 && job_out < 0 && job_err < 0) {
        int status; if (waitpid(job_pid, &status, WNOHANG) != job_pid) return;
        job_pid = -1; int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        bool auth = strstr(log_text, "incorrect password") || strstr(log_text, "no password was provided") || strstr(log_text, "Sorry, try again");
        bool denied = strstr(log_text, "not in the sudoers") != NULL;
        say("JOB done op=%s status=%d auth=%d denied=%d", pending_op, code, auth, denied);
        if (!code) { progress = 100; set_message(!strcmp(pending_op, "install") ? "Installed." : !strcmp(pending_op, "remove") ? "Removed." : !strcmp(pending_op, "update") ? "Package lists are up to date." : "All updates are installed."); }
        else if (denied) set_message("Your account is not allowed to change software (administrators only).");
        else if (auth) set_message("Authentication failed: wrong password.");
        else set_message("The operation failed. See the details below.");
        progress = code ? -1 : 100;
        start_query(Q_INSTALLED);
        if (tab == UPDATES) start_query(Q_UPDATES);
    }
}
static void request(const char* op, int count, const char* const* args) {
    if (job_pid > 0) return;
    snprintf(pending_op, sizeof(pending_op), "%s", op); pending_count = 0;
    for (int i = 0; i < count && i < 8; i++) if (valid_name(args[i])) snprintf(pending_args[pending_count++], 96, "%s", args[i]);
    if (strcmp(op, "update") && strcmp(op, "upgrade") && !pending_count) return;
    asking = true; focus_password = true; password[0] = 0; redraw = true; say("ASK op=%s", op);
}
/* ------------------------------------------------------------------------------------------------ drawing */
static void rect(int x, int y, int w, int h, uint32_t c) {
    int r = x + w, b = y + h; if (x < 0) x = 0; if (y < 0) y = 0; if (r > width) r = width; if (b > height) b = height;
    for (int row = y; row < b; row++) for (int col = x; col < r; col++) pixels[(size_t)row * width + col] = c;
}
static void text(int x, int y, const char* value, const uifont_t* font, uint32_t color, int right) {
    int pen = x * 4;
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        unsigned c = *p; if (c < font->first || c >= font->first + font->count) c = '?';
        const uifont_glyph_t* g = &font->g[c - font->first];
        int gx = ((pen + 2) >> 2) + g->xoff, gy = y + font->ascent + g->yoff; if (gx + (int)g->w > right) break;
        for (unsigned row = 0; row < g->h; row++) for (unsigned col = 0; col < g->w; col++) {
            uint8_t packed = font->bits[g->off + row * ((g->w + 1) / 2) + col / 2]; unsigned alpha = ((col & 1) ? packed >> 4 : packed & 15) * 17;
            int px = gx + (int)col, py = gy + (int)row; if (px < 0 || px >= width || py < 0 || py >= height) continue;
            uint32_t bg = pixels[(size_t)py * width + px];
            unsigned r = (((color >> 16) & 255) * alpha + ((bg >> 16) & 255) * (255 - alpha) + 127) / 255;
            unsigned gr = (((color >> 8) & 255) * alpha + ((bg >> 8) & 255) * (255 - alpha) + 127) / 255, b = ((color & 255) * alpha + (bg & 255) * (255 - alpha) + 127) / 255;
            pixels[(size_t)py * width + px] = (r << 16) | (gr << 8) | b;
        }
        pen += g->adv_q4;
    }
}
static void release_buffer(void* d, struct wl_buffer* b) { (void)d; (void)b; busy = false; }
static const struct wl_buffer_listener buffer_listener = {release_buffer};
static bool make_buffer(void) {
    if (buffer && buffer_w == width && buffer_h == height) return true;
    if (buffer) { wl_buffer_destroy(buffer); munmap(pixels, buffer_size); buffer = NULL; }
    if (width < 500 || height < 400 || (uint64_t)width * height > INT32_MAX / 4) return false;
    buffer_size = (size_t)width * height * 4; int fd = memfd_create("nexus-software", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)buffer_size)) { if (fd >= 0) close(fd); return false; }
    pixels = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (pixels == MAP_FAILED) { close(fd); return false; }
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)buffer_size); close(fd);
    buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888); wl_shm_pool_destroy(pool);
    if (!buffer) return false;
    wl_buffer_add_listener(buffer, &buffer_listener, NULL); buffer_w = width; buffer_h = height; return true;
}
static void layout(void) {
    card_w = width - 40; card_h = height - 40; card_x = 20; card_y = 20;
    list_y = card_y + 168; bottom_y = card_y + card_h - 64; rows = (bottom_y - 150 - list_y) / 44; if (rows < 1) rows = 1;
}
static const char* primary_label(void) {
    if (tab == UPDATES) return "Update all";
    if (selected < item_count && items[selected].installed) return "Remove";
    return "Install";
}
static void paint(void) {
    if (!configured || closed || busy || !redraw) return;
    if (!make_buffer()) { closed = true; return; }
    layout();
    rect(0, 0, width, height, 0x101827); rect(card_x, card_y, card_w, card_h, 0xf5f7fb); rect(card_x, card_y, card_w, 5, 0x2f9d6a);
    text(card_x + 28, card_y + 20, "NexusOS  /  Software", &uf_ui, 0x527091, card_x + card_w - 28);
    text(card_x + 28, card_y + 44, "Software", &uf_title, 0x17273d, card_x + card_w - 28);
    static const char* const tabs[] = {"Explore", "Installed", "Updates"};
    for (int i = 0; i < 3; i++) { int x = card_x + 28 + i * 150;
        rect(x, card_y + 92, 140, 34, tab == (enum tab)i ? 0x2f9d6a : 0xe1e7f0);
        text(x + 18, card_y + 98, tabs[i], &uf_uib, tab == (enum tab)i ? 0xffffff : 0x213b59, x + 136); }
    if (tab != UPDATES) {
        int sx = card_x + 500; rect(sx, card_y + 92, card_w - 528, 34, focus_password ? 0xe1e7f0 : 0xffffff);
        rect(sx, card_y + 124, card_w - 528, 2, 0x2f9d6a);
        text(sx + 10, card_y + 98, search[0] ? search : (tab == EXPLORE ? "Search Debian packages (type, then Enter)" : "Filter installed packages"),
             &uf_ui, search[0] ? 0x17273d : 0x8a99ab, card_x + card_w - 36);
    } else {
        rect(card_x + 500, card_y + 92, 220, 34, 0xe1e7f0); text(card_x + 518, card_y + 98, "Check for updates", &uf_uib, 0x213b59, card_x + 716);
    }
    text(card_x + 28, card_y + 138, message, &uf_ui, strncmp(message, "Error", 5) && !strstr(message, "failed") && !strstr(message, "not allowed") ? 0x356b55 : 0xb32632, card_x + card_w - 28);
    for (int r = 0; r < rows && scroll + (size_t)r < item_count; r++) {
        size_t i = scroll + (size_t)r; int y = list_y + r * 44; struct item* it = &items[i];
        rect(card_x + 24, y, card_w - 48, 42, i == selected ? 0xdcefe4 : 0xffffff);
        text(card_x + 36, y + 3, it->name, &uf_uib, 0x17273d, card_x + 330);
        text(card_x + 340, y + 3, it->summary, &uf_ui, 0x3c5068, card_x + card_w - 260);
        char state[160];
        if (tab == UPDATES) snprintf(state, sizeof(state), "%s -> %s", it->old, it->version);
        else snprintf(state, sizeof(state), "%s", it->installed ? "Installed" : "Available");
        text(card_x + card_w - 250, y + 3, state, &uf_ui, it->installed ? 0x2f9d6a : 0x527091, card_x + card_w - 30);
        if (it->version[0] && tab != UPDATES) text(card_x + 36, y + 22, it->version, &uf_small, 0x6a7b8f, card_x + 330);
    }
    int ly = bottom_y - 140;
    if (progress >= 0) { rect(card_x + 28, ly, card_w - 56, 10, 0xdbe4f2); rect(card_x + 28, ly, (card_w - 56) * progress / 100, 10, 0x2f9d6a); }
    rect(card_x + 28, ly + 16, card_w - 56, 112, 0xe9edf4);
    const char* start = log_text + log_size; unsigned lines = 0;
    while (start > log_text) { start--; if (*start == '\n' && start != log_text + log_size - 1 && ++lines == 5) { start++; break; } }
    for (int l = 0; *start && l < 5; l++) { char row[256]; size_t n = 0; while (*start && *start != '\n') { if (n < sizeof(row) - 1) row[n++] = *start; start++; } if (*start) start++; row[n] = 0; text(card_x + 36, ly + 22 + l * 20, row, &uf_small, 0x344960, card_x + card_w - 36); }
    int px = card_x + card_w - 220; bool enabled = job_pid < 0 && (tab == UPDATES ? item_count > 0 : selected < item_count);
    rect(px, bottom_y, 192, 44, enabled ? 0x2f9d6a : 0xa4b4cb); text(px + 24, bottom_y + 6, job_pid > 0 ? "Working..." : primary_label(), &uf_uib, 0xffffff, px + 186);
    rect(card_x + 28, bottom_y, 140, 44, 0xe1e7f0); text(card_x + 52, bottom_y + 6, "Close", &uf_uib, 0x213b59, card_x + 164);
    if (asking) {
        int w = 520, h = 210, x = (width - w) / 2, y = (height - h) / 2;
        rect(x - 4, y - 4, w + 8, h + 8, 0x0b1220); rect(x, y, w, h, 0xffffff);
        text(x + 24, y + 18, "Authentication required", &uf_uib, 0x17273d, x + w - 24);
        text(x + 24, y + 48, "Enter your password to change the installed software.", &uf_ui, 0x3c5068, x + w - 24);
        rect(x + 24, y + 84, w - 48, 36, 0xf0f3f8); rect(x + 24, y + 118, w - 48, 2, 0x2f9d6a);
        char dots[130]; size_t n = strlen(password); if (n > 120) n = 120; memset(dots, '*', n); dots[n] = 0; text(x + 34, y + 90, dots, &uf_uib, 0x17273d, x + w - 34);
        rect(x + 24, y + 146, 140, 40, 0xe1e7f0); text(x + 48, y + 151, "Cancel", &uf_uib, 0x213b59, x + 160);
        rect(x + w - 184, y + 146, 160, 40, 0x2f9d6a); text(x + w - 166, y + 151, "Authenticate", &uf_uib, 0xffffff, x + w - 28);
    }
    wl_surface_attach(surface, buffer, 0, 0); wl_surface_damage_buffer(surface, 0, 0, width, height); wl_surface_commit(surface);
    busy = true; redraw = false;
    say("UI tab=%d items=%zu selected=%s asking=%d primary=%d,%d search=%d,%d row0=%d,%d tab1=%d,%d tab2=%d,%d check=%d,%d auth=%d,%d",
        tab, item_count, selected < item_count ? items[selected].name : "-", asking, px + 96, bottom_y + 22, card_x + 600, card_y + 109,
        card_x + 200, list_y + 21, card_x + 248, card_y + 109, card_x + 398, card_y + 109, card_x + 610, card_y + 109,
        (width + 520) / 2 - 104, (height - 210) / 2 + 166);
}
/* ------------------------------------------------------------------------------------------------ input */
static void switch_tab(enum tab t) {
    tab = t; item_count = 0; selected = scroll = 0; redraw = true;
    if (t == INSTALLED) { rebuild_installed_view(); set_message("Installed packages."); }
    if (t == UPDATES) { set_message("Looking for updates..."); start_query(Q_UPDATES); }
    if (t == EXPLORE) { set_message("Search for applications and tools from Debian."); if (search[0]) start_query(Q_SEARCH); }
}
static void activate(void) {
    if (job_pid > 0) return;
    if (tab == UPDATES) { if (item_count) request("upgrade", 0, NULL); return; }
    if (selected >= item_count) return;
    const char* name = items[selected].name;
    request(items[selected].installed ? "remove" : "install", 1, &name);
}
static void select_row(int64_t i) {
    if (!item_count) return; if (i < 0) i = 0; if ((size_t)i >= item_count) i = (int64_t)item_count - 1;
    selected = (size_t)i; if (selected < scroll) scroll = selected; if (selected >= scroll + (size_t)rows) scroll = selected - rows + 1; redraw = true;
}
static void type_char(const char* utf8) {
    char* field = asking ? password : search; size_t cap = asking ? sizeof(password) : sizeof(search);
    size_t n = strlen(field); if (n + strlen(utf8) + 1 >= cap) return;
    if (!asking && !(((unsigned char)utf8[0] >= 'a' && utf8[0] <= 'z') || (utf8[0] >= '0' && utf8[0] <= '9') || strchr("-+.", utf8[0]))) return;
    strcat(field, utf8); redraw = true;
    if (!asking && tab == INSTALLED) rebuild_installed_view();
}
static void submit(void) {
    if (asking) { asking = false; focus_password = false; start_job(); redraw = true; return; }
    if (tab == EXPLORE) { if (search[0]) { set_message("Searching..."); start_query(Q_SEARCH); } else activate(); }
    else activate();
}
static void keymap_event(void* d, struct wl_keyboard* k, uint32_t format, int fd, uint32_t size) {
    (void)d; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd); if (map == MAP_FAILED) return;
    struct xkb_keymap* next = xkb_keymap_new_from_string(xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS); munmap(map, size);
    if (!next) return;
    if (kstate) xkb_state_unref(kstate); if (keymap) xkb_keymap_unref(keymap);
    keymap = next; kstate = xkb_state_new(keymap);
}
static void key_enter(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* f, struct wl_array* keys) { (void)d; (void)k; (void)s; (void)f; (void)keys; }
static void key_leave(void* d, struct wl_keyboard* k, uint32_t s, struct wl_surface* f) { (void)d; (void)k; (void)s; (void)f; }
static void key_event(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)time; if (!state || closed) return;
    if (code == 1) { if (asking) { asking = false; memset(password, 0, sizeof(password)); redraw = true; } else closed = true; return; }   /* Esc */
    if (code == 28 || code == 96) { submit(); return; }                                                                            /* Enter */
    if (code == 14) { char* field = asking ? password : search; size_t n = strlen(field); if (n) field[n - 1] = 0; if (!asking && tab == INSTALLED) rebuild_installed_view(); redraw = true; return; }
    if (!asking) {
        if (code == 103) { select_row((int64_t)selected - 1); return; } if (code == 108) { select_row((int64_t)selected + 1); return; }
        if (code == 15) { switch_tab((enum tab)((tab + 1) % 3)); return; }
    }
    if (!kstate) return;
    char utf8[8]; int n = xkb_state_key_get_utf8(kstate, code + 8, utf8, sizeof(utf8));
    if (n > 0 && (unsigned char)utf8[0] >= 32 && utf8[0] != 127) type_char(utf8);
}
static void modifiers(void* d, struct wl_keyboard* k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    (void)d; (void)k; (void)s; if (kstate) xkb_state_update_mask(kstate, dep, lat, lock, 0, 0, group);
}
static void repeat_info(void* d, struct wl_keyboard* k, int32_t r, int32_t delay) { (void)d; (void)k; (void)r; (void)delay; }
static const struct wl_keyboard_listener keyboard_listener = {keymap_event, key_enter, key_leave, key_event, modifiers, repeat_info};
static void p_enter(void* d, struct wl_pointer* p, uint32_t s, struct wl_surface* f, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)s; (void)f; mouse_x = wl_fixed_to_int(x); mouse_y = wl_fixed_to_int(y); }
static void p_leave(void* d, struct wl_pointer* p, uint32_t s, struct wl_surface* f) { (void)d; (void)p; (void)s; (void)f; }
static void p_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; mouse_x = wl_fixed_to_int(x); mouse_y = wl_fixed_to_int(y); }
static bool inside(int x, int y, int w, int h) { return mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h; }
static void p_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) {
    (void)d; (void)p; (void)serial; (void)time; if (!state || code != 272 || closed) return;
    if (asking) {
        int w = 520, h = 210, x = (width - w) / 2, y = (height - h) / 2;
        if (inside(x + 24, y + 146, 140, 40)) { asking = false; memset(password, 0, sizeof(password)); redraw = true; }
        else if (inside(x + w - 184, y + 146, 160, 40)) submit();
        return;
    }
    for (int i = 0; i < 3; i++) if (inside(card_x + 28 + i * 150, card_y + 92, 140, 34)) { switch_tab((enum tab)i); return; }
    if (tab == UPDATES && inside(card_x + 500, card_y + 92, 220, 34)) { request("update", 0, NULL); return; }
    if (inside(card_x + 28, bottom_y, 140, 44)) { closed = true; return; }
    if (inside(card_x + card_w - 220, bottom_y, 192, 44)) { activate(); return; }
    if (inside(card_x + 24, list_y, card_w - 48, rows * 44)) select_row((int64_t)scroll + (mouse_y - list_y) / 44);
}
static void p_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t which, wl_fixed_t value) { (void)d; (void)p; (void)t; if (!which) select_row((int64_t)selected + (value > 0 ? 3 : -3)); }
static void p_frame(void* d, struct wl_pointer* p) { (void)d; (void)p; }
static const struct wl_pointer_listener pointer_listener = {.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis, .frame = p_frame};
static void caps(void* d, struct wl_seat* seat, uint32_t c) {
    (void)d;
    if (c & WL_SEAT_CAPABILITY_POINTER) wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, NULL);
    if (c & WL_SEAT_CAPABILITY_KEYBOARD) wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, NULL);
}
static void seat_name(void* d, struct wl_seat* s, const char* n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {caps, seat_name};
static void configure_surface(void* d, struct xdg_surface* s, uint32_t serial) { (void)d; xdg_surface_ack_configure(s, serial); configured = true; redraw = true; }
static const struct xdg_surface_listener xdg_listener = {configure_surface};
static void configure_top(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* st) { (void)d; (void)t; (void)st; if (w > 0) width = w; if (h > 0) height = h; redraw = true; }
static void close_top(void* d, struct xdg_toplevel* t) { (void)d; (void)t; closed = true; }
static const struct xdg_toplevel_listener top_listener = {.configure = configure_top, .close = close_top};
static void ping(void* d, struct xdg_wm_base* b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void global(void* d, struct wl_registry* r, uint32_t id, const char* name, uint32_t v) {
    (void)d; (void)v;
    if (!strcmp(name, "wl_compositor")) compositor = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(name, "wl_shm")) shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(name, "xdg_wm_base")) { shell = wl_registry_bind(r, id, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(shell, &shell_listener, NULL); }
    else if (!strcmp(name, "wl_seat")) { struct wl_seat* seat = wl_registry_bind(r, id, &wl_seat_interface, 5); wl_seat_add_listener(seat, &seat_listener, NULL); }
}
static void global_remove(void* d, struct wl_registry* r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener registry_listener = {global, global_remove};
int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--updates")) tab = UPDATES;
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    display = wl_display_connect(NULL); if (!display) { perror("nexus-software: connect to the desktop"); return 3; }
    struct wl_registry* registry = wl_display_get_registry(display); wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !compositor || !shm || !shell) { fprintf(stderr, "nexus-software: desktop interfaces unavailable\n"); return 3; }
    surface = wl_compositor_create_surface(compositor); xdg = xdg_wm_base_get_xdg_surface(shell, surface); xdg_surface_add_listener(xdg, &xdg_listener, NULL);
    top = xdg_surface_get_toplevel(xdg); xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_title(top, "Software"); xdg_toplevel_set_app_id(top, "nexus.software"); wl_surface_commit(surface);
    start_query(Q_INSTALLED);
    if (tab == UPDATES) start_query(Q_UPDATES);
    say("STARTED");
    while (!closed || job_pid > 0) {
        if (!closed) { if (wl_display_dispatch_pending(display) < 0) closed = true; paint(); if (wl_display_flush(display) < 0 && errno != EAGAIN) closed = true; }
        struct pollfd fds[4]; nfds_t n = 0;
        if (!closed) fds[n++] = (struct pollfd){wl_display_get_fd(display), POLLIN, 0};
        if (query_fd >= 0) fds[n++] = (struct pollfd){query_fd, POLLIN, 0};
        if (job_out >= 0) fds[n++] = (struct pollfd){job_out, POLLIN, 0};
        if (job_err >= 0) fds[n++] = (struct pollfd){job_err, POLLIN, 0};
        int ready = poll(fds, n, 100);
        if (ready < 0 && errno != EINTR) break;
        if (!closed && n && fds[0].revents && wl_display_dispatch(display) < 0) closed = true;
        query_events(); job_events();
    }
    say("CLOSED");
    return 0;
}
