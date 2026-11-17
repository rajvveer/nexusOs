/* ============================================================================
 * NexusOS — HTTP Server — Phase 30 / Phase 55
 * ============================================================================
 * Phase 55 turned the fixed status page into a small HTTP/1.0 static file
 * server (Connection: close, one client at a time) over the RAM filesystem.
 *
 *  - Listener/poll/re-listen structure mirrors rshell.c / sync.c: one
 *    tcp_listen() TCB, httpd_poll() is pumped from the shell idle hook and the
 *    desktop loop (never from IRQ), and a new listener is armed as soon as a
 *    client's connection leaves ESTABLISHED.
 *  - Requests arrive through the 1 KB TCP RX window across several polls: the
 *    request line + headers accumulate in rq_buf until "\r\n\r\n"; a PUT body
 *    (<= RAMFS_MAX_FILE_SIZE) accumulates afterwards.
 *  - The whole response (headers + body) is rendered into resp_buf and then
 *    pushed in <= 1 KB tcp_send() chunks, a couple per poll with net_poll()
 *    between them, so a 4 KB file never bursts the NIC / peer window.
 *  - Document root (default /www, created at boot with index.html and
 *    style.css): GET / -> index.html, GET /<p> -> <root>/<p>, directories
 *    without index.html get an HTML listing. /status = the classic page,
 *    /api/status = JSON.
 *  - /fs/<abs path> serves ANY ramfs path, but needs HTTP Basic auth against
 *    the OS user DB (users_authenticate) and the per-uid permission bits
 *    (vfs_check_perm). PUT /fs/<path> writes/creates a file when the uid may
 *    write it (201 Created / 204 No Content). A bad login costs a 1 s lockout.
 *  - Integer math only (no libgcc).
 * ============================================================================ */

#include "httpd.h"
#include "version.h"
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "rtc.h"
#include "memory.h"
#include "vga.h"
#include "string.h"
#include "vfs.h"
#include "ramfs.h"
#include "users.h"
#include "syslog.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Limits
 * -------------------------------------------------------------------------- */
#define HTTPD_BODY_MAX        RAMFS_MAX_FILE_SIZE      /* PUT body cap (256 KB since Phase 57; one static buffer) */
#define HTTPD_PAGE_MAX        6144                     /* generated body cap   */
#define HTTPD_RESP_MAX        (HTTPD_PAGE_MAX + 512)   /* headers + body       */
#define HTTPD_CHUNK           1024                     /* bytes per tcp_send   */
#define HTTPD_CHUNKS_PER_POLL 2
#define HTTPD_LOCKOUT_TICKS   18                       /* 1 s after bad auth   */
#define HTTPD_IDLE_TICKS      180                      /* 10 s to send a request */
#define UID_ANON              0xFFFFFFFEu              /* anonymous visitor    */

typedef enum { RQ_HEADERS, RQ_BODY, RQ_LOCKOUT, RQ_SENDING, RQ_DONE } rq_state_t;

/* --------------------------------------------------------------------------
 * Server state
 * -------------------------------------------------------------------------- */
static bool httpd_running = false;
static int  httpd_conn_idx = -1;  /* Which TCB index we're using */
static uint32_t httpd_conn_gen = 0; /* ...and its allocation generation (slot reuse guard) */
static char docroot[FS_PATH_MAX] = "/www";

/* Per-connection request reassembly */
static uint8_t    rq_buf[HTTPD_MAX_REQUEST];
static int        rq_len = 0;
static uint8_t    body_buf[HTTPD_BODY_MAX];
static int        body_len = 0, body_want = 0;
static rq_state_t rq_state = RQ_HEADERS;
static bool       conn_seen = false;     /* saw this client connected       */
static uint32_t   conn_tick = 0;         /* when it connected (idle timeout) */

/* Parsed request */
static char req_method[8];
static char req_path[FS_PATH_MAX];
static char req_auth[128];               /* full "Authorization" value       */
static int  req_clen = 0;
static bool req_expect100 = false;
static bool req_head = false;

/* Response */
static uint8_t resp_buf[HTTPD_RESP_MAX];
static int     resp_len = 0, resp_off = 0;
static char    page[HTTPD_PAGE_MAX];
static int     page_len = 0;

/* Stats */
static uint32_t stat_requests = 0;
static uint32_t stat_auth_fail = 0;
static uint32_t last_client_ip = 0;
static uint32_t lockout_until = 0;

static bool httpd_relisten(void);   /* defined with the lifecycle code below */

/* --------------------------------------------------------------------------
 * Connection helper (re-fetch the TCB each poll, like rshell)
 * -------------------------------------------------------------------------- */
static tcp_conn_t* get_conn(void) {
    if (httpd_conn_idx < 0) return NULL;
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    if (httpd_conn_idx >= count) return NULL;
    tcp_conn_t* c = (tcp_conn_t*)&all[httpd_conn_idx];
    if (!c->active) return NULL;
    if (c->gen != httpd_conn_gen) return NULL;   /* slot was recycled for someone else */
    return c;
}

static void reset_request(void) {
    rq_len = 0;
    body_len = body_want = 0;
    rq_state = RQ_HEADERS;
    conn_seen = false;
    req_method[0] = req_path[0] = req_auth[0] = '\0';
    req_clen = 0;
    req_expect100 = false;
    req_head = false;
    resp_len = resp_off = 0;
    page_len = 0;
}

/* --------------------------------------------------------------------------
 * Page builder (response body)
 * -------------------------------------------------------------------------- */
static void page_put(const char* s) {
    while (*s && page_len < HTTPD_PAGE_MAX) page[page_len++] = *s++;
}

static void page_num(int v) {
    char tmp[16];
    int_to_str(v, tmp);
    page_put(tmp);
}

/* Append s with the HTML-special characters neutralised (names come from
 * the filesystem, i.e. from users). */
static void page_put_esc(const char* s) {
    for (; *s; s++) {
        if (*s == '<') page_put("&lt;");
        else if (*s == '>') page_put("&gt;");
        else if (*s == '&') page_put("&amp;");
        else if (*s == '"') page_put("&quot;");
        else if (page_len < HTTPD_PAGE_MAX) page[page_len++] = *s;
    }
}

/* --------------------------------------------------------------------------
 * Response assembly: headers + (unless HEAD / 204) the page body
 * -------------------------------------------------------------------------- */
static void resp_put(const char* s) {
    while (*s && resp_len < HTTPD_RESP_MAX) resp_buf[resp_len++] = (uint8_t)*s++;
}

static void resp_build(int code, const char* reason, const char* ctype, const char* extra) {
    char num[16];
    resp_len = 0; resp_off = 0;
    resp_put("HTTP/1.0 "); int_to_str(code, num); resp_put(num);
    resp_put(" "); resp_put(reason); resp_put("\r\n");
    resp_put("Server: NexusOS/" NEXUS_VERSION "\r\n");
    if (code != 204) {
        resp_put("Content-Type: "); resp_put(ctype); resp_put("\r\n");
        resp_put("Content-Length: "); int_to_str(page_len, num); resp_put(num); resp_put("\r\n");
    }
    if (extra) resp_put(extra);
    resp_put("Connection: close\r\n\r\n");
    if (!req_head && code != 204 && page_len > 0) {
        int n = page_len;
        if (n > HTTPD_RESP_MAX - resp_len) n = HTTPD_RESP_MAX - resp_len;
        memcpy(resp_buf + resp_len, page, n);
        resp_len += n;
    }
    rq_state = RQ_SENDING;
}

static void respond_error(int code, const char* reason, const char* extra) {
    page_len = 0;
    page_put("<!DOCTYPE html><html><head><title>"); page_num(code); page_put(" ");
    page_put(reason); page_put("</title><link rel=\"stylesheet\" href=\"/style.css\"></head><body><h1>");
    page_num(code); page_put(" "); page_put(reason);
    page_put("</h1><p>"); page_put_esc(req_path[0] ? req_path : "-");
    page_put("</p><p class=\"f\"><a href=\"/\">NexusOS httpd</a></p></body></html>\n");
    resp_build(code, reason, "text/html", extra);
}

/* --------------------------------------------------------------------------
 * Small parsers
 * -------------------------------------------------------------------------- */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive "does line start with name followed by ':'?" Returns a
 * pointer to the value (leading spaces skipped) or NULL. */
static const char* hdr_value(const char* line, const char* name) {
    int i = 0;
    while (name[i]) {
        if (!line[i] || lower(line[i]) != lower(name[i])) return NULL;
        i++;
    }
    if (line[i] != ':') return NULL;
    i++;
    while (line[i] == ' ' || line[i] == '\t') i++;
    return line + i;
}

/* Decode the request target into req_path: strip the query string, percent-
 * decode, reject anything that is not a clean absolute path or that climbs
 * with "..". Returns false on a bad path. */
static bool decode_path(const char* src, int n) {
    int o = 0;
    if (n <= 0 || src[0] != '/') return false;
    for (int i = 0; i < n; i++) {
        char c = src[i];
        if (c == '?' || c == '#') break;
        if (c == '%') {
            if (i + 2 >= n) return false;
            int h = hexval(src[i + 1]), l = hexval(src[i + 2]);
            if (h < 0 || l < 0) return false;
            c = (char)(h * 16 + l);
            i += 2;
        }
        if ((uint8_t)c < 0x20 || c == 0x7F) return false;
        if (o >= FS_PATH_MAX - 1) return false;
        req_path[o++] = c;
    }
    req_path[o] = '\0';
    /* reject any ".." component (also catches encoded %2e%2e) */
    for (int i = 0; req_path[i]; ) {
        while (req_path[i] == '/') i++;
        int s = i;
        while (req_path[i] && req_path[i] != '/') i++;
        if (i - s == 2 && req_path[s] == '.' && req_path[s + 1] == '.') return false;
    }
    return true;
}

/* Parse rq_buf (which ends in "\r\n\r\n"). Returns 0 or an HTTP error code. */
static int parse_request(void) {
    char line[HTTPD_MAX_REQUEST];
    int pos = 0;
    req_method[0] = req_path[0] = req_auth[0] = '\0';
    req_clen = 0; req_expect100 = false;

    /* --- request line --- */
    int ll = 0;
    while (pos < rq_len && rq_buf[pos] != '\n' && ll < (int)sizeof(line) - 1) line[ll++] = (char)rq_buf[pos++];
    if (pos < rq_len) pos++;                         /* consume '\n' */
    if (ll > 0 && line[ll - 1] == '\r') ll--;
    line[ll] = '\0';

    int i = 0, mi = 0;
    while (line[i] && line[i] != ' ' && mi < 7) req_method[mi++] = line[i++];
    req_method[mi] = '\0';
    if (line[i] != ' ') return 400;
    while (line[i] == ' ') i++;
    int ts = i;
    while (line[i] && line[i] != ' ') i++;
    if (!decode_path(line + ts, i - ts)) return 400;
    while (line[i] == ' ') i++;
    if (strncmp(line + i, "HTTP/1.", 7) != 0) return 400;

    /* --- headers --- */
    while (pos < rq_len) {
        ll = 0;
        while (pos < rq_len && rq_buf[pos] != '\n' && ll < (int)sizeof(line) - 1) line[ll++] = (char)rq_buf[pos++];
        if (pos < rq_len) pos++;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        line[ll] = '\0';
        if (ll == 0) break;                          /* end of headers */

        const char* v;
        if ((v = hdr_value(line, "Authorization")) != NULL) {
            strncpy(req_auth, v, sizeof(req_auth) - 1);
            req_auth[sizeof(req_auth) - 1] = '\0';
        } else if ((v = hdr_value(line, "Content-Length")) != NULL) {
            int cl = 0;
            if (*v < '0' || *v > '9') return 400;
            while (*v >= '0' && *v <= '9') { if (cl < 100000000) cl = cl * 10 + (*v - '0'); v++; }
            req_clen = cl;
        } else if ((v = hdr_value(line, "Expect")) != NULL) {
            if (strncmp(v, "100-continue", 12) == 0) req_expect100 = true;
        }
    }
    return 0;
}

/* Standard base64 decoder; returns byte count or -1. */
static int b64_decode(const char* in, uint8_t* out, int outmax) {
    int n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != ' ' && *in != '\r' && *in != '\n'; in++) {
        char c = *in;
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outmax) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

/* HTTP Basic auth -> uid. 0 = ok, 1 = no credentials, 2 = bad credentials. */
static int do_auth(uint32_t* uid_out) {
    if (!req_auth[0]) return 1;
    const char* p = req_auth;
    if (!(lower(p[0]) == 'b' && lower(p[1]) == 'a' && lower(p[2]) == 's' &&
          lower(p[3]) == 'i' && lower(p[4]) == 'c' && p[5] == ' ')) return 2;
    p += 6;
    while (*p == ' ') p++;
    uint8_t dec[96];
    int n = b64_decode(p, dec, sizeof(dec) - 1);
    if (n <= 0) return 2;
    dec[n] = '\0';
    char* colon = NULL;
    for (int i = 0; i < n; i++) if (dec[i] == ':') { colon = (char*)dec + i; break; }
    if (!colon) return 2;
    *colon = '\0';
    uint32_t uid = users_authenticate((const char*)dec, colon + 1);
    memset(dec, 0, sizeof(dec));
    if (uid == UID_INVALID) return 2;
    *uid_out = uid;
    return 0;
}

/* --------------------------------------------------------------------------
 * MIME types by extension
 * -------------------------------------------------------------------------- */
static const char* mime_for(const char* name) {
    const char* ext = NULL;
    for (const char* p = name; *p; p++) if (*p == '.') ext = p + 1;
    if (!ext) return "application/octet-stream";
    char e[6]; int i = 0;
    while (ext[i] && i < 5) { e[i] = lower(ext[i]); i++; }
    e[i] = '\0';
    if (!strcmp(e, "html") || !strcmp(e, "htm")) return "text/html";
    if (!strcmp(e, "css"))  return "text/css";
    if (!strcmp(e, "js"))   return "application/javascript";
    if (!strcmp(e, "txt") || !strcmp(e, "md") || !strcmp(e, "cfg") || !strcmp(e, "ns") || !strcmp(e, "c") || !strcmp(e, "h")) return "text/plain";
    if (!strcmp(e, "json")) return "application/json";
    if (!strcmp(e, "png"))  return "image/png";
    if (!strcmp(e, "jpg") || !strcmp(e, "jpeg")) return "image/jpeg";
    if (!strcmp(e, "gif"))  return "image/gif";
    if (!strcmp(e, "bmp"))  return "image/bmp";
    if (!strcmp(e, "wav"))  return "audio/wav";
    if (!strcmp(e, "svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

/* --------------------------------------------------------------------------
 * Status page (classic) and JSON
 * -------------------------------------------------------------------------- */
static void build_status_page(void) {
    page_len = 0;
    page_put("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>NexusOS status</title>");
    page_put("<link rel=\"stylesheet\" href=\"/style.css\"></head><body>");
    page_put("<h1>" NEXUS_VERSION_STRING "</h1>");
    page_put("<p>The Hybrid Operating System &middot; <a href=\"/\">home</a> &middot; "
             "<a href=\"/api/status\">JSON</a> &middot; <a href=\"/fs/\">files</a></p><h2>Status</h2>");

    /* Uptime */
    page_put("<div class='s'><span class='d'>Uptime: </span>");
    uint32_t secs = system_ticks / 18;
    uint32_t mins = secs / 60; secs %= 60;
    page_num((int)mins); page_put("m "); page_num((int)secs); page_put("s</div>");

    /* Time */
    rtc_time_t t; rtc_read(&t);
    char tb[9]; rtc_format_time(&t, tb);
    page_put("<div class='s'><span class='d'>Time: </span>"); page_put(tb); page_put("</div>");

    /* Memory */
    page_put("<div class='s'><span class='d'>Memory: </span>");
    page_num((int)(pmm_get_free_pages() * 4)); page_put(" KB free / ");
    page_num((int)(pmm_get_total_pages() * 4)); page_put(" KB</div>");

    /* Files / users */
    page_put("<div class='s'><span class='d'>RAM FS: </span>");
    page_num((int)ramfs_get_file_count()); page_put(" nodes, document root ");
    page_put_esc(docroot); page_put("</div>");
    page_put("<div class='s'><span class='d'>Users: </span>"); page_num(users_count()); page_put("</div>");

    /* Network */
    page_put("<h2>Network</h2>");
    const net_config_t* cfg = ip_get_config();
    char ip_str[16];
    page_put("<div class='s'><span class='d'>IP: </span>");
    ip_to_string(cfg->ip, ip_str); page_put(ip_str); page_put("</div>");
    page_put("<div class='s'><span class='d'>GW: </span>");
    ip_to_string(cfg->gateway, ip_str); page_put(ip_str); page_put("</div>");
    page_put("<div class='s'><span class='d'>Requests served: </span>");
    page_num((int)stat_requests); page_put("</div>");

    page_put("<p class='g'>Server on port 8080</p></body></html>\n");
}

static void build_json(void) {
    char tmp[16];
    page_len = 0;
    rtc_time_t t; rtc_read(&t);
    char tb[9]; rtc_format_time(&t, tb);
    const net_config_t* cfg = ip_get_config();
    ip_to_string(cfg->ip, tmp);

    page_put("{\"os\":\"" NEXUS_VERSION_STRING "\",");
    page_put("\"uptime\":"); page_num((int)(system_ticks / 18)); page_put(",");
    page_put("\"ticks\":"); page_num((int)system_ticks); page_put(",");
    page_put("\"time\":\""); page_put(tb); page_put("\",");
    page_put("\"mem_free_kb\":"); page_num((int)(pmm_get_free_pages() * 4)); page_put(",");
    page_put("\"mem_total_kb\":"); page_num((int)(pmm_get_total_pages() * 4)); page_put(",");
    page_put("\"ip\":\""); page_put(tmp); page_put("\",");
    page_put("\"users\":"); page_num(users_count()); page_put(",");
    page_put("\"files\":"); page_num((int)ramfs_get_file_count()); page_put(",");
    page_put("\"requests\":"); page_num((int)stat_requests); page_put(",");
    page_put("\"auth_failures\":"); page_num((int)stat_auth_fail); page_put(",");
    page_put("\"docroot\":\""); page_put_esc(docroot); page_put("\"}\n");
}

/* --------------------------------------------------------------------------
 * Serving files and directories
 * -------------------------------------------------------------------------- */
static void serve_file(fs_node_t* node) {
    page_len = 0;
    uint32_t n = node->size;
    if (n > HTTPD_PAGE_MAX) n = HTTPD_PAGE_MAX;
    /* Permission was checked by the caller for the *HTTP* identity; read via
     * the node op so the console user's identity does not interfere. */
    int32_t r = node->read ? node->read(node, 0, n, (uint8_t*)page) : 0;
    page_len = r > 0 ? (int)r : 0;
    resp_build(200, "OK", mime_for(node->name), NULL);
}

/* HTML directory listing with absolute hrefs (so it works with or without a
 * trailing slash on the request). */
static void serve_listing(fs_node_t* dir, const char* urlpath) {
    char base[FS_PATH_MAX + 2];
    strncpy(base, urlpath, FS_PATH_MAX); base[FS_PATH_MAX] = '\0';
    int bl = (int)strlen(base);
    if (bl == 0 || base[bl - 1] != '/') { base[bl++] = '/'; base[bl] = '\0'; }

    page_len = 0;
    page_put("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Index of ");
    page_put_esc(base);
    page_put("</title><link rel=\"stylesheet\" href=\"/style.css\"></head><body><h1>Index of ");
    page_put_esc(base);
    page_put("</h1><table><tr><th>Name</th><th>Size</th><th>Mode</th><th>Owner</th></tr>\n");

    if (bl > 1) {
        /* parent: strip the trailing slash and the last component */
        char parent[FS_PATH_MAX + 2];
        strcpy(parent, base);
        int pl = bl - 1;
        while (pl > 0 && parent[pl - 1] != '/') pl--;
        parent[pl] = '\0';
        page_put("<tr><td><a href=\""); page_put_esc(parent);
        page_put("\">../</a></td><td></td><td></td><td></td></tr>\n");
    }

    uint32_t idx = 0; int count = 0;
    fs_node_t* n;
    while ((n = vfs_readdir(dir, idx++)) != NULL) {
        if (page_len > HTTPD_PAGE_MAX - 260) { page_put("<tr><td colspan=4>...</td></tr>\n"); break; }
        char mode[11]; vfs_mode_string(n, mode);
        bool isdir = (n->type == FS_DIRECTORY);
        page_put("<tr><td><a href=\""); page_put_esc(base); page_put_esc(n->name);
        if (isdir) page_put("/");
        page_put("\">"); page_put_esc(n->name); if (isdir) page_put("/");
        page_put("</a></td><td>"); page_num((int)n->size);
        page_put("</td><td>"); page_put(mode);
        page_put("</td><td>"); page_put_esc(users_name_of(n->uid));
        page_put("</td></tr>\n");
        count++;
    }
    page_put("</table><p class=\"f\">NexusOS httpd &middot; "); page_num(count);
    page_put(" entries</p></body></html>\n");
    resp_build(200, "OK", "text/html", NULL);
}

/* Serve a resolved node as `uid` (UID_ANON for the public document root).
 * use_index: website semantics (a directory with index.html serves it);
 * false for the /fs/ browser, where a directory always lists. */
static void serve_node(fs_node_t* node, const char* urlpath, uint32_t uid, bool use_index) {
    if (!vfs_check_perm(node, uid, ACC_READ)) { respond_error(403, "Forbidden", NULL); return; }
    if (node->type == FS_DIRECTORY) {
        fs_node_t* idx = use_index ? vfs_finddir(node, "index.html") : NULL;
        if (idx && idx->type == FS_FILE && vfs_check_perm(idx, uid, ACC_READ)) { serve_file(idx); return; }
        serve_listing(node, urlpath);
        return;
    }
    serve_file(node);
}

/* PUT /fs/<path>: write or create a file as `uid`. */
static void handle_put(const char* fspath, uint32_t uid) {
    fs_node_t* n = ramfs_resolve(fspath, NULL);
    bool created = false;
    if (n) {
        if (n->type != FS_FILE) { respond_error(409, "Conflict", NULL); return; }
        if (!vfs_check_perm(n, uid, ACC_WRITE)) { respond_error(403, "Forbidden", NULL); return; }
    } else {
        char leaf[FS_NAME_MAX];
        fs_node_t* parent = ramfs_resolve_parent(fspath, NULL, leaf);
        if (!parent || parent->type != FS_DIRECTORY) { respond_error(404, "Not Found", NULL); return; }
        if (!leaf[0] || !strcmp(leaf, ".") || !strcmp(leaf, "..")) { respond_error(400, "Bad Request", NULL); return; }
        if (!vfs_check_perm(parent, uid, ACC_WRITE)) { respond_error(403, "Forbidden", NULL); return; }
        /* create as the authenticated user so the new file is owned by them */
        uint32_t prev = users_current_uid();
        users_set_current(uid);
        n = ramfs_mknode(parent, leaf, FS_FILE);
        users_set_current(prev);
        if (!n) { respond_error(507, "Insufficient Storage", NULL); return; }
        created = true;
    }
    n->size = 0;                                   /* truncate, then write */
    if (body_len > 0 && n->write) {
        int32_t w = n->write(n, 0, (uint32_t)body_len, body_buf);
        if (w < 0) { respond_error(500, "Internal Server Error", NULL); return; }
    }
    page_len = 0;
    if (created) {
        char loc[FS_PATH_MAX + 16];
        strcpy(loc, "Location: "); strcat(loc, req_path); strcat(loc, "\r\n");
        page_put("created\n");
        resp_build(201, "Created", "text/plain", loc);
    } else {
        resp_build(204, "No Content", "text/plain", NULL);
    }
    syslog_add("httpd: PUT stored a file");
}

/* --------------------------------------------------------------------------
 * Route one complete request to a response
 * -------------------------------------------------------------------------- */
static void handle_request(void) {
    tcp_conn_t* c = get_conn();
    if (c) last_client_ip = c->remote_ip;

    bool is_get  = strcmp(req_method, "GET") == 0;
    bool is_head = strcmp(req_method, "HEAD") == 0;
    bool is_put  = strcmp(req_method, "PUT") == 0;
    req_head = is_head;
    if (!is_get && !is_head && !is_put) {
        respond_error(405, "Method Not Allowed", "Allow: GET, HEAD, PUT\r\n");
        return;
    }

    /* --- protected tree: /fs/<abs path> --- */
    if (strncmp(req_path, "/fs", 3) == 0 && (req_path[3] == '\0' || req_path[3] == '/')) {
        uint32_t uid = UID_INVALID;
        int a = do_auth(&uid);
        if (a != 0) {
            if (a == 2) {
                stat_auth_fail++;
                lockout_until = system_ticks + HTTPD_LOCKOUT_TICKS;
                syslog_add("httpd: Basic auth failed");
            }
            respond_error(401, "Unauthorized", "WWW-Authenticate: Basic realm=\"NexusOS\"\r\n");
            return;
        }
        const char* fspath = req_path[3] ? req_path + 3 : "/";
        if (is_put) { handle_put(fspath, uid); return; }
        fs_node_t* n = ramfs_resolve(fspath, NULL);
        if (!n) { respond_error(404, "Not Found", NULL); return; }
        serve_node(n, req_path, uid, false);   /* /fs/: directories always list */
        return;
    }

    if (is_put) { respond_error(405, "Method Not Allowed", "Allow: GET, HEAD\r\n"); return; }

    if (strcmp(req_path, "/status") == 0) {
        build_status_page();
        resp_build(200, "OK", "text/html", NULL);
        return;
    }
    if (strcmp(req_path, "/api/status") == 0) {
        build_json();
        resp_build(200, "OK", "application/json", NULL);
        return;
    }

    /* --- public document root --- */
    char full[FS_PATH_MAX * 2];
    strcpy(full, docroot);
    if (strcmp(req_path, "/") != 0) strcat(full, req_path);
    fs_node_t* n = ramfs_resolve(full, NULL);
    if (!n) { respond_error(404, "Not Found", NULL); return; }
    serve_node(n, req_path, UID_ANON, true);
}

/* Called once a request (and any PUT body) is complete. A pending auth
 * lockout delays credentialed requests until it expires. */
static void start_request(void) {
    if (req_auth[0] && (int32_t)(lockout_until - system_ticks) > 0) {
        rq_state = RQ_LOCKOUT;
        return;
    }
    handle_request();
}

/* --------------------------------------------------------------------------
 * RX: accumulate headers, then the body
 * -------------------------------------------------------------------------- */
static void rx_append(tcp_conn_t* c) {
    uint8_t in[TCP_RX_BUF_SIZE];
    int n = tcp_recv(c, in, sizeof(in));
    if (n <= 0) return;
    int i = 0;

    if (rq_state == RQ_HEADERS) {
        while (i < n) {
            if (rq_len >= HTTPD_MAX_REQUEST) {
                respond_error(431, "Request Header Fields Too Large", NULL);
                return;
            }
            rq_buf[rq_len++] = in[i++];
            if (rq_len >= 4 && memcmp(rq_buf + rq_len - 4, "\r\n\r\n", 4) == 0) {
                int code = parse_request();
                if (code != 0) { respond_error(code, code == 400 ? "Bad Request" : "Error", NULL); return; }
                if (strcmp(req_method, "PUT") == 0 && req_clen > 0) {
                    if (req_clen > HTTPD_BODY_MAX) { respond_error(413, "Payload Too Large", NULL); return; }
                    body_want = req_clen; body_len = 0;
                    rq_state = RQ_BODY;
                    if (req_expect100) tcp_send(c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
                    break;                       /* rest of `in` is body */
                }
                start_request();
                return;
            }
        }
    }
    if (rq_state == RQ_BODY) {
        while (i < n && body_len < body_want) body_buf[body_len++] = in[i++];
        if (body_len >= body_want) start_request();
    }
}

/* --------------------------------------------------------------------------
 * TX: push the response in <= 1 KB chunks, a few per poll
 * -------------------------------------------------------------------------- */
static void tx_pump(tcp_conn_t* c) {
    for (int k = 0; k < HTTPD_CHUNKS_PER_POLL && resp_off < resp_len; k++) {
        int n = resp_len - resp_off;
        if (n > HTTPD_CHUNK) n = HTTPD_CHUNK;
        if (tcp_send(c, resp_buf + resp_off, (uint16_t)n) < 0) { resp_off = resp_len; break; }
        resp_off += n;
        if (resp_off < resp_len) net_poll();   /* let ACKs / RX flow between chunks */
    }
    if (resp_off >= resp_len) {
        stat_requests++;
        tcp_close(c);                          /* FIN behind the last byte */
        rq_state = RQ_DONE;
        /* Re-arm right away (adopts a client that already connected while
         * this response was being sent); the old TCB finishes closing on
         * its own inside the TCP state machine. */
        httpd_relisten();
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
static void seed_file(fs_node_t* dir, const char* name, const char* text) {
    if (!dir || vfs_finddir(dir, name)) return;
    fs_node_t* n = ramfs_mknode(dir, name, FS_FILE);
    if (n && n->write) n->write(n, 0, (uint32_t)strlen(text), (const uint8_t*)text);
}

void httpd_init(void) {
    httpd_running = false;
    httpd_conn_idx = -1;
    reset_request();

    /* Document root with a welcome page + stylesheet (boot runs as root). */
    if (!ramfs_resolve("/www", NULL)) ramfs_mkdir("/www", NULL);
    fs_node_t* www = ramfs_resolve("/www", NULL);
    seed_file(www, "index.html",
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>NexusOS</title>\n"
        "<link rel=\"stylesheet\" href=\"/style.css\"></head>\n<body>\n"
        "<h1>Welcome to NexusOS</h1>\n"
        "<p>This page is served by the NexusOS kernel's built-in HTTP server straight from the RAM filesystem (<code>/www</code>).</p>\n"
        "<ul>\n"
        "<li><a href=\"/status\">System status</a></li>\n"
        "<li><a href=\"/api/status\">Status as JSON</a> (<code>/api/status</code>)</li>\n"
        "<li><a href=\"/fs/\">Browse the filesystem</a> (<code>/fs/&lt;path&gt;</code>, login required)</li>\n"
        "<li><a href=\"/style.css\">style.css</a></li>\n"
        "</ul>\n"
        "<p>Upload: <code>PUT /fs/home/root/file.txt</code> with HTTP Basic auth.</p>\n"
        "<p class=\"f\">" NEXUS_VERSION_STRING " httpd</p>\n</body></html>\n");
    seed_file(www, "style.css",
        "body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;max-width:720px;margin:40px auto;padding:20px}\n"
        "h1{color:#0ff}h2{color:#8bf}a{color:#7cf}code{color:#fc8}\n"
        "table{border-collapse:collapse}td,th{padding:4px 14px;text-align:left;border-bottom:1px solid #333}\n"
        ".s{background:#16213e;padding:8px;margin:6px 0;border-radius:4px}.g{color:#0f0}.d{color:#888}.f{color:#666;margin-top:30px}\n");

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("HTTP server initialized (port 8080, docroot /www)\n");
}

void httpd_start(void) {
    if (httpd_running) {
        vga_print_color("  HTTPD: already running\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        return;
    }

    tcp_conn_t* conn = tcp_listen(HTTPD_PORT);
    if (!conn) {
        vga_print_color("  HTTPD: No free TCP slots\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }

    httpd_running = true;
    reset_request();

    /* Find which index this connection is */
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    httpd_conn_idx = -1;
    for (int i = 0; i < count; i++) {
        if (&all[i] == conn) { httpd_conn_idx = i; break; }
    }
    httpd_conn_gen = conn->gen;

    vga_print_color("  HTTPD: Listening on port 8080\n",
                    VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("  HTTPD: public docroot "); vga_print(docroot);
    vga_print(" (+ /status, /api/status); /fs/<path> needs HTTP Basic auth\n");
}

void httpd_stop(void) {
    if (!httpd_running) return;

    tcp_conn_t* c = get_conn();
    if (c) tcp_close(c);

    httpd_running = false;
    httpd_conn_idx = -1;
    reset_request();
    vga_print_color("  HTTPD: Stopped\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

/* Re-arm the listener in a free TCB slot and reset the request machine. */
static bool httpd_relisten(void) {
    tcp_conn_t* nc = tcp_listen(HTTPD_PORT);
    if (!nc) { httpd_running = false; httpd_conn_idx = -1; return false; }
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    for (int i = 0; i < count; i++)
        if (&all[i] == nc) { httpd_conn_idx = i; break; }
    httpd_conn_gen = nc->gen;
    reset_request();
    return true;
}

/* --------------------------------------------------------------------------
 * httpd_poll: drive one client (non-IRQ context)
 * -------------------------------------------------------------------------- */
void httpd_poll(void) {
    if (!httpd_running || httpd_conn_idx < 0) return;

    tcp_conn_t* c = get_conn();
    if (!c) {
        /* TCB slot freed (connection fully closed) — re-arm the listener. */
        httpd_relisten();
        return;
    }

    /* Remote closed, or our own close progressed past ESTABLISHED: hand the
     * old TCB to the TCP state machine and listen again right away. */
    if (c->closed || (c->connected && c->state != TCP_ESTABLISHED)) {
        tcp_close(c);
        httpd_relisten();
        return;
    }

    if (!c->connected) return;

    if (!conn_seen) { conn_seen = true; conn_tick = system_ticks; }

    if (rq_state == RQ_HEADERS || rq_state == RQ_BODY) {
        if (c->rx_len > 0) rx_append(c);
        else if (system_ticks - conn_tick > HTTPD_IDLE_TICKS) {
            respond_error(408, "Request Timeout", NULL);   /* slow / silent client */
        }
    }
    if (rq_state == RQ_LOCKOUT && (int32_t)(lockout_until - system_ticks) <= 0)
        handle_request();
    if (rq_state == RQ_SENDING) tx_pump(c);
}

bool httpd_is_running(void) {
    return httpd_running;
}

/* --------------------------------------------------------------------------
 * Phase 55: shell helpers
 * -------------------------------------------------------------------------- */
void httpd_print_status(void) {
    char num[16], ip[16];
    vga_print("\n  HTTPD: ");
    vga_print(httpd_running ? "running (port 8080)\n" : "stopped\n");
    vga_print("  Document root:   "); vga_print(docroot); vga_print("\n");
    vga_print("  Requests served: "); int_to_str((int)stat_requests, num); vga_print(num);
    vga_print("   Auth failures: "); int_to_str((int)stat_auth_fail, num); vga_print(num); vga_print("\n");
    vga_print("  Last client:     ");
    if (last_client_ip) { ip_to_string(last_client_ip, ip); vga_print(ip); } else vga_print("-");
    vga_print("\n  URL map: /  /status  /api/status  /fs/<path> (Basic auth, GET/HEAD/PUT)\n\n");
}

bool httpd_set_root(const char* path) {
    fs_node_t* n = path ? ramfs_resolve(path, NULL) : NULL;
    if (!n || n->type != FS_DIRECTORY) {
        vga_print_color("  HTTPD: no such directory: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(path ? path : "(null)"); vga_print("\n");
        return false;
    }
    ramfs_abspath(n, docroot, sizeof(docroot));
    vga_print_color("  HTTPD: document root is now ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(docroot); vga_print("\n");
    return true;
}

const char* httpd_get_root(void) { return docroot; }
uint32_t httpd_requests_served(void) { return stat_requests; }
