/* ============================================================================
 * NexusOS — Cloud File Sync + Settings Sync — Phase 45
 * ============================================================================
 * See sync.h for the protocol. Implementation notes:
 *
 *  - The SERVER mirrors httpd/rshell exactly: one tcp_listen() TCB slot, a
 *    poll() that drains rx_buf line-by-line and re-listens after each client.
 *  - The CLIENT path mirrors the ping/http blocking pattern: tcp_connect(),
 *    spin with net_poll()+hlt until connected (with a tick timeout), exchange,
 *    then close. Never blocks forever — every wait is tick-bounded.
 *  - Integer math only (no 64-bit mul/div — no libgcc in this freestanding
 *    kernel). File transfers respect the 4 KB RAM-FS per-file cap and the 1 KB
 *    TCP RX window (we drain the window each poll and reassemble across polls).
 * ============================================================================ */

#include "sync.h"
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "vfs.h"
#include "ramfs.h"
#include "users.h"
#include "vga.h"
#include "string.h"
#include "heap.h"

/* Live settings sources for settings sync */
#include "theme.h"
#include "font.h"
#include "accessibility.h"
#include "wallpaper.h"

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Server state
 * -------------------------------------------------------------------------- */
static bool sync_running = false;
static int  sync_conn_idx = -1;

/* Per-connection line/transfer reassembly (the 1 KB TCP RX window means a
 * request line or a PUT body can arrive across several polls). */
static char     sv_line[160];      /* accumulating request line             */
static int      sv_linelen = 0;
static bool     sv_in_put = false; /* receiving a PUT body                   */
static char     sv_put_name[FS_NAME_MAX];
static int      sv_put_size = 0;   /* expected body length                   */
static int      sv_put_got  = 0;   /* body bytes received so far             */
static uint8_t  sv_put_buf[RAMFS_MAX_FILE_SIZE];

/* Stats */
static uint32_t files_sent = 0;
static uint32_t files_recv = 0;

/* Settings persistence path */
#define SYNC_CFG_DEFAULT "settings.cfg"

/* --------------------------------------------------------------------------
 * Small helpers (no libc)
 * -------------------------------------------------------------------------- */

/* Parse a non-negative decimal integer from s. Returns -1 if there is no
 * leading digit (empty/non-numeric) so callers can reject malformed input —
 * a bare 0 return would be ambiguous with a valid "0". Stops at the first
 * non-digit; clamps absurdly long runs rather than wrapping negative. */
static int parse_uint(const char* s) {
    if (!s || *s < '0' || *s > '9') return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v > 100000000) return 1000000000;  /* clamp; way past any real size */
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* Find a TCB pointer by our stored index, or NULL if gone/inactive. */
static tcp_conn_t* sv_conn(void) {
    if (sync_conn_idx < 0) return NULL;
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    if (sync_conn_idx >= count) return NULL;
    tcp_conn_t* c = (tcp_conn_t*)&all[sync_conn_idx];
    if (!c->active) return NULL;
    return c;
}

static void sv_send(tcp_conn_t* c, const char* s) {
    if (c && c->connected) tcp_send(c, s, (uint16_t)strlen(s));
}

/* --------------------------------------------------------------------------
 * Settings serialize / deserialize
 * -------------------------------------------------------------------------- */
int settings_serialize(char* buf, int max) {
    int pos = 0;
    char num[12];

    #define SC_APPEND(s) do { const char* _s = (s); \
        while (*_s && pos < max - 1) buf[pos++] = *_s++; } while(0)
    #define SC_KV(k, v) do { SC_APPEND(k); SC_APPEND("="); \
        int_to_str((v), num); SC_APPEND(num); SC_APPEND("\n"); } while(0)

    SC_APPEND("# NexusOS settings v1\n");
    SC_KV("theme",    theme_get_index());
    SC_KV("fontscale", font_get_scale());
    SC_KV("reader",   accessibility_reader_on() ? 1 : 0);
    SC_KV("contrast", accessibility_contrast_on() ? 1 : 0);
    SC_KV("wallpaper", wallpaper_get());

    #undef SC_KV
    #undef SC_APPEND
    buf[pos] = '\0';
    return pos;
}

/* Apply one "key=value" pair to the live system. */
static void settings_apply_kv(const char* key, int val) {
    if (strcmp(key, "theme") == 0) {
        if (val >= 0 && val < THEME_COUNT) theme_set(val);
    } else if (strcmp(key, "fontscale") == 0) {
        if (val >= 1 && val <= 4) font_set_scale(val);
    } else if (strcmp(key, "reader") == 0) {
        accessibility_set_reader(val ? true : false);
    } else if (strcmp(key, "contrast") == 0) {
        /* toggle to match desired state */
        if ((val != 0) != accessibility_contrast_on())
            accessibility_toggle_contrast();
    } else if (strcmp(key, "wallpaper") == 0) {
        if (val >= 0 && val < WP_COUNT) wallpaper_set(val);
    }
}

int settings_save(const char* path) {
    if (!path) path = SYNC_CFG_DEFAULT;
    char buf[256];
    int len = settings_serialize(buf, sizeof(buf));

    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, path);
    if (!node) {
        node = ramfs_create(path, FS_FILE);
        if (!node) return -1;
    }
    int32_t w = vfs_write(node, 0, (uint32_t)len, (const uint8_t*)buf);
    if (w < 0) return -1;
    return (int)w;
}

int settings_load(const char* path) {
    if (!path) path = SYNC_CFG_DEFAULT;
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, path);
    if (!node) return -1;

    char buf[512];
    uint32_t n = node->size;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    int32_t r = vfs_read(node, 0, n, (uint8_t*)buf);
    if (r < 0) return -1;
    buf[r] = '\0';

    /* Walk lines of "key=value", skipping blanks and '#' comments. */
    int i = 0;
    while (i < r) {
        /* extract a line into key/value */
        char line[80];
        int ll = 0;
        while (i < r && buf[i] != '\n' && ll < (int)sizeof(line) - 1)
            line[ll++] = buf[i++];
        while (i < r && buf[i] != '\n') i++;   /* skip rest of long line */
        if (i < r) i++;                          /* consume newline       */
        line[ll] = '\0';

        if (ll == 0 || line[0] == '#') continue;
        /* split on '=' */
        int eq = -1;
        for (int k = 0; k < ll; k++) if (line[k] == '=') { eq = k; break; }
        if (eq <= 0) continue;
        line[eq] = '\0';
        settings_apply_kv(line, parse_uint(line + eq + 1));
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Server: handle one complete request line
 * -------------------------------------------------------------------------- */
static void sv_handle_line(tcp_conn_t* c, char* line) {
    if (strncmp(line, "LIST", 4) == 0) {
        fs_node_t* root = vfs_get_root();
        uint32_t idx = 0;
        fs_node_t* n;
        char hdr[80], num[12];
        while ((n = vfs_readdir(root, idx++)) != NULL) {
            if (n->type != FS_FILE) continue;
            strcpy(hdr, "FILE ");
            strcat(hdr, n->name);
            strcat(hdr, " ");
            int_to_str((int)n->size, num);
            strcat(hdr, num);
            strcat(hdr, "\r\n");
            sv_send(c, hdr);
        }
        sv_send(c, "END\r\n");
    }
    else if (strncmp(line, "GET ", 4) == 0) {
        const char* name = line + 4;
        fs_node_t* node = vfs_finddir(vfs_get_root(), name);
        if (!node || node->type != FS_FILE) { sv_send(c, "ERR nofile\r\n"); return; }
        char buf[RAMFS_MAX_FILE_SIZE];
        uint32_t sz = node->size;
        if (sz > sizeof(buf)) sz = sizeof(buf);
        int32_t r = vfs_read(node, 0, sz, (uint8_t*)buf);
        if (r < 0) { sv_send(c, "ERR perm\r\n"); return; }
        char hdr[80], num[12];
        strcpy(hdr, "FILE "); strcat(hdr, name); strcat(hdr, " ");
        int_to_str((int)r, num); strcat(hdr, num); strcat(hdr, "\r\n");
        sv_send(c, hdr);
        if (r > 0) tcp_send(c, buf, (uint16_t)r);
        files_sent++;
    }
    else if (strncmp(line, "PUT ", 4) == 0) {
        /* "PUT <name> <size>" — body follows as raw bytes */
        char name[FS_NAME_MAX]; int ni = 0;
        const char* p = line + 4;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && ni < FS_NAME_MAX - 1) name[ni++] = *p++;
        name[ni] = '\0';
        while (*p == ' ') p++;
        int size = parse_uint(p);
        if (size < 0 || size > RAMFS_MAX_FILE_SIZE) { sv_send(c, "ERR size\r\n"); return; }
        strcpy(sv_put_name, name);
        sv_put_size = size;
        sv_put_got = 0;
        sv_in_put = (size > 0);
        if (size == 0) {
            /* empty file: create immediately */
            fs_node_t* node = vfs_finddir(vfs_get_root(), name);
            if (!node) node = ramfs_create(name, FS_FILE);
            if (node) { vfs_write(node, 0, 0, sv_put_buf); files_recv++; sv_send(c, "OK\r\n"); }
            else sv_send(c, "ERR create\r\n");
        }
    }
    else if (strncmp(line, "BYE", 3) == 0) {
        sv_send(c, "OK\r\n");
        tcp_close(c);
    }
    else if (line[0] != '\0') {
        sv_send(c, "ERR cmd\r\n");
    }
}

/* Commit a fully-received PUT body to the RAM-FS. */
static void sv_commit_put(tcp_conn_t* c) {
    fs_node_t* node = vfs_finddir(vfs_get_root(), sv_put_name);
    if (!node) node = ramfs_create(sv_put_name, FS_FILE);
    if (!node) { sv_send(c, "ERR create\r\n"); sv_in_put = false; return; }
    int32_t w = vfs_write(node, 0, (uint32_t)sv_put_size, sv_put_buf);
    if (w < 0) sv_send(c, "ERR write\r\n");
    else { files_recv++; sv_send(c, "OK\r\n"); }
    sv_in_put = false;
}

/* --------------------------------------------------------------------------
 * Server lifecycle
 * -------------------------------------------------------------------------- */
void sync_init(void) {
    sync_running = false;
    sync_conn_idx = -1;
    sv_linelen = 0;
    sv_in_put = false;
    files_sent = files_recv = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Cloud sync initialized (port 7070)\n");
}

void sync_server_start(void) {
    if (sync_running) return;
    tcp_conn_t* conn = tcp_listen(SYNC_PORT);
    if (!conn) {
        vga_print_color("  SYNC: No free TCP slots\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    sync_running = true;
    sv_linelen = 0;
    sv_in_put = false;

    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    sync_conn_idx = -1;
    for (int i = 0; i < count; i++)
        if (&all[i] == conn) { sync_conn_idx = i; break; }

    vga_print_color("  SYNC: Listening on port 7070\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
}

void sync_server_stop(void) {
    if (!sync_running) return;
    tcp_conn_t* c = sv_conn();
    if (c) tcp_close(c);
    sync_running = false;
    sync_conn_idx = -1;
    vga_print_color("  SYNC: Stopped\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

bool sync_server_running(void) { return sync_running; }

/* Re-arm the sync listener in a free TCB slot. Returns true on success. */
static bool sync_relisten(void) {
    tcp_conn_t* nc = tcp_listen(SYNC_PORT);
    if (!nc) { sync_running = false; sync_conn_idx = -1; return false; }
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    for (int i = 0; i < count; i++)
        if (&all[i] == nc) { sync_conn_idx = i; break; }
    sv_linelen = 0; sv_in_put = false;
    return true;
}

void sync_poll(void) {
    if (!sync_running || sync_conn_idx < 0) return;
    tcp_conn_t* c = sv_conn();

    if (!c) {
        /* TCB slot freed (connection fully closed) — re-arm the listener. */
        sync_relisten();
        return;
    }

    /* If the remote closed (or our close completed past ESTABLISHED), tear our
     * side down and immediately re-listen so the next client isn't refused. */
    if (c->closed || (c->connected && c->state != TCP_ESTABLISHED)) {
        tcp_close(c);
        sync_relisten();
        return;
    }

    if (!c->connected || c->rx_len == 0) return;

    /* Drain the RX window byte-by-byte through the reassembly state machine. */
    for (int i = 0; i < c->rx_len; i++) {
        uint8_t b = c->rx_buf[i];

        if (sv_in_put) {
            if (sv_put_got < sv_put_size && sv_put_got < RAMFS_MAX_FILE_SIZE)
                sv_put_buf[sv_put_got++] = b;
            if (sv_put_got >= sv_put_size) {
                sv_commit_put(c);
                sv_linelen = 0;   /* fresh line after the body */
            }
            continue;
        }

        if (b == '\n') {
            /* trim a trailing CR */
            if (sv_linelen > 0 && sv_line[sv_linelen - 1] == '\r') sv_linelen--;
            sv_line[sv_linelen] = '\0';
            sv_handle_line(c, sv_line);
            sv_linelen = 0;
        } else if (sv_linelen < (int)sizeof(sv_line) - 1) {
            sv_line[sv_linelen++] = (char)b;
        }
    }
    c->rx_len = 0;
}

/* --------------------------------------------------------------------------
 * Client helpers
 * -------------------------------------------------------------------------- */

/* Open a connection and spin until established or timeout. NULL on failure. */
static tcp_conn_t* cl_connect(uint32_t host_ip) {
    uint16_t lport = (uint16_t)(49200 + (system_ticks & 0x1FF));
    tcp_conn_t* c = tcp_connect(host_ip, SYNC_PORT, lport);
    if (!c) return NULL;
    uint32_t start = system_ticks;
    while (!c->connected && (system_ticks - start) < 54) {  /* ~3s */
        net_poll();
        __asm__ volatile("hlt");
        if (!c->active) return NULL;
    }
    return c->connected ? c : NULL;
}

/* Read bytes from the connection until `want` collected, the peer closes, or
 * timeout. Returns bytes read into buf (>=0). */
static int cl_read(tcp_conn_t* c, uint8_t* buf, int want, int timeout_ticks) {
    int got = 0;
    uint32_t last = system_ticks;
    while (got < want) {
        int r = tcp_recv(c, buf + got, (uint16_t)(want - got));
        if (r > 0) { got += r; last = system_ticks; continue; }
        if (r < 0) break;                 /* closed */
        if ((system_ticks - last) >= (uint32_t)timeout_ticks) break;
        net_poll();
        __asm__ volatile("hlt");
    }
    return got;
}

/* Read a single CRLF/LF-terminated line into line[max]. Returns length (>=0)
 * or -1 on closed/timeout-with-nothing. */
static int cl_read_line(tcp_conn_t* c, char* line, int max, int timeout_ticks) {
    int len = 0;
    uint32_t last = system_ticks;
    for (;;) {
        uint8_t ch;
        int r = tcp_recv(c, &ch, 1);
        if (r == 1) {
            last = system_ticks;
            if (ch == '\n') { if (len > 0 && line[len-1] == '\r') len--; line[len] = '\0'; return len; }
            if (len < max - 1) line[len++] = (char)ch;
            continue;
        }
        if (r < 0) { line[len] = '\0'; return len > 0 ? len : -1; }
        if ((system_ticks - last) >= (uint32_t)timeout_ticks) { line[len] = '\0'; return len > 0 ? len : -1; }
        net_poll();
        __asm__ volatile("hlt");
    }
}

/* --------------------------------------------------------------------------
 * Client operations
 * -------------------------------------------------------------------------- */
int sync_client_list(uint32_t host_ip) {
    tcp_conn_t* c = cl_connect(host_ip);
    if (!c) { vga_print_color("  SYNC: connect failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return -1; }

    sv_send(c, "LIST\r\n");
    vga_print_color("\n  Remote files:\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    int n = 0;
    for (;;) {
        char line[160];
        int l = cl_read_line(c, line, sizeof(line), 36);
        if (l < 0) break;
        if (strncmp(line, "END", 3) == 0) break;
        if (strncmp(line, "FILE ", 5) == 0) {
            vga_print("    ");
            vga_print(line + 5);
            vga_print("\n");
            n++;
        }
    }
    char num[12]; int_to_str(n, num);
    vga_print_color("  (", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print(num); vga_print(" file(s))\n\n");
    sv_send(c, "BYE\r\n");
    tcp_close(c);
    return 0;
}

/* Parse a "FILE <name> <size>" header line; returns size or -1. */
static int parse_file_hdr(const char* line, char* name_out) {
    if (strncmp(line, "FILE ", 5) != 0) return -1;
    const char* p = line + 5;
    int ni = 0;
    while (*p && *p != ' ' && ni < FS_NAME_MAX - 1) name_out[ni++] = *p++;
    name_out[ni] = '\0';
    while (*p == ' ') p++;
    return parse_uint(p);
}

int sync_client_pull(uint32_t host_ip, const char* name) {
    tcp_conn_t* c = cl_connect(host_ip);
    if (!c) { vga_print_color("  SYNC: connect failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return -1; }

    char req[80];
    strcpy(req, "GET "); strcat(req, name); strcat(req, "\r\n");
    sv_send(c, req);

    char line[160], fname[FS_NAME_MAX];
    int l = cl_read_line(c, line, sizeof(line), 36);
    if (l < 0 || strncmp(line, "FILE ", 5) != 0) {
        vga_print_color("  SYNC: remote has no '", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print(name); vga_print("'\n");
        sv_send(c, "BYE\r\n"); tcp_close(c); return -1;
    }
    int size = parse_file_hdr(line, fname);
    if (size < 0 || size > RAMFS_MAX_FILE_SIZE) { tcp_close(c); return -1; }

    static uint8_t body[RAMFS_MAX_FILE_SIZE];
    int got = cl_read(c, body, size, 54);

    fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
    if (!node) node = ramfs_create(fname, FS_FILE);
    if (node) {
        vfs_write(node, 0, (uint32_t)got, body);
        files_recv++;
        char num[12]; int_to_str(got, num);
        vga_print_color("  Pulled ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(fname); vga_print(" ("); vga_print(num); vga_print(" bytes)\n");
    } else {
        vga_print_color("  SYNC: cannot create local file\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    }
    sv_send(c, "BYE\r\n");
    tcp_close(c);
    return node ? 0 : -1;
}

int sync_client_push(uint32_t host_ip, const char* name) {
    fs_node_t* node = vfs_finddir(vfs_get_root(), name);
    if (!node || node->type != FS_FILE) {
        vga_print_color("  SYNC: no local file '", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print(name); vga_print("'\n");
        return -1;
    }
    static uint8_t body[RAMFS_MAX_FILE_SIZE];
    uint32_t sz = node->size;
    if (sz > sizeof(body)) sz = sizeof(body);
    int32_t r = vfs_read(node, 0, sz, body);
    if (r < 0) { vga_print_color("  SYNC: read denied\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return -1; }

    tcp_conn_t* c = cl_connect(host_ip);
    if (!c) { vga_print_color("  SYNC: connect failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return -1; }

    char hdr[80], num[12];
    strcpy(hdr, "PUT "); strcat(hdr, name); strcat(hdr, " ");
    int_to_str((int)r, num); strcat(hdr, num); strcat(hdr, "\r\n");
    sv_send(c, hdr);
    if (r > 0) tcp_send(c, body, (uint16_t)r);

    char line[80];
    int l = cl_read_line(c, line, sizeof(line), 54);
    bool ok = (l >= 0 && strncmp(line, "OK", 2) == 0);
    if (ok) {
        files_sent++;
        vga_print_color("  Pushed ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(name); vga_print(" ("); vga_print(num); vga_print(" bytes)\n");
    } else {
        vga_print_color("  SYNC: push rejected\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    }
    sv_send(c, "BYE\r\n");
    tcp_close(c);
    return ok ? 0 : -1;
}

int sync_client_pull_all(uint32_t host_ip) {
    tcp_conn_t* c = cl_connect(host_ip);
    if (!c) { vga_print_color("  SYNC: connect failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return -1; }

    sv_send(c, "LIST\r\n");
    /* Collect the names first (the list is small), then fetch each one with a
     * fresh GET on the same connection. */
    char names[16][FS_NAME_MAX];
    int count = 0;
    for (;;) {
        char line[160], fname[FS_NAME_MAX];
        int l = cl_read_line(c, line, sizeof(line), 36);
        if (l < 0) break;
        if (strncmp(line, "END", 3) == 0) break;
        if (strncmp(line, "FILE ", 5) == 0 && count < 16) {
            parse_file_hdr(line, fname);
            strcpy(names[count++], fname);
        }
    }

    int pulled = 0;
    for (int i = 0; i < count; i++) {
        char req[80];
        strcpy(req, "GET "); strcat(req, names[i]); strcat(req, "\r\n");
        sv_send(c, req);
        char line[160], fname[FS_NAME_MAX];
        int l = cl_read_line(c, line, sizeof(line), 36);
        if (l < 0 || strncmp(line, "FILE ", 5) != 0) continue;
        int size = parse_file_hdr(line, fname);
        if (size < 0 || size > RAMFS_MAX_FILE_SIZE) continue;
        static uint8_t body[RAMFS_MAX_FILE_SIZE];
        int got = cl_read(c, body, size, 54);
        fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
        if (!node) node = ramfs_create(fname, FS_FILE);
        if (node) { vfs_write(node, 0, (uint32_t)got, body); files_recv++; pulled++;
            vga_print("    + "); vga_print(fname); vga_print("\n"); }
    }
    char num[12]; int_to_str(pulled, num);
    vga_print_color("  Pulled ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(num); vga_print(" file(s)\n");
    sv_send(c, "BYE\r\n");
    tcp_close(c);
    return 0;
}

uint32_t sync_files_sent(void) { return files_sent; }
uint32_t sync_files_recv(void) { return files_recv; }
