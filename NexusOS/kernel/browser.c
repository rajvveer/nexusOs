/* ============================================================================
 * NexusOS — Nexus Browser — Phase 57
 * ============================================================================
 * A windowed graphical web browser for the Aurora desktop.
 *
 *   chrome   : Back / Forward / Reload / Home, editable address bar with a
 *              lock / warning glyph, bookmarks bar, 2 px progress bar, status
 *              line (hover target, "Loading... N KB", errors).
 *   engine   : webengine.c (tokenizer -> tree -> layout -> paint).
 *   loading  : everything through netstream.h; ns_poll() is pumped from the
 *              desktop loop by browser_poll(). Redirects followed (<= 4),
 *              HTML parsed progressively (every 32 KB) and when complete,
 *              text/plain shown monospace, images shown alone, anything else
 *              saved to /home/<user>/Downloads/<name>. Images on a page load
 *              lazily afterwards, two at a time, 256 KB each.
 *   history  : 24 entries, Back / Forward.
 *
 * Page/image arenas use the reserved .app_bss region.
 * ============================================================================ */

#include "browser.h"
#include "webengine.h"
#include "netstream.h"
#include "window.h"
#include "appui.h"
#include "macui.h"
#include "framebuffer.h"
#include "mouse.h"
#include "keyboard.h"
#include "clipboard.h"
#include "string.h"
#include "syslog.h"
#include "ramfs.h"
#include "vfs.h"
#include "users.h"
#include "vga.h"
#include "download.h"
#include "filemgr.h"

extern volatile uint32_t system_ticks;

/* ---- geometry ----------------------------------------------------------- */
#define PAD        12
#define TOOLBAR_H  44
#define BOOKBAR_H  26
#define PROGRESS_H 2
#define CHROME_TOP (TOOLBAR_H + BOOKBAR_H + PROGRESS_H)
#define STATUS_H   22
#define BTN        30
#define BTN_Y      7
#define ADDR_X     184
#define SCROLL_STEP 40

/* ---- colours ------------------------------------------------------------ */
#define C_CHROME   0xECECEF
#define C_CHROME2  0xF6F6F8
#define C_SEP      0xD4D4D9
#define C_FIELD    0xFFFFFF
#define C_FIELDBD  0xC9C9CF
#define C_TXT      0x1D1D1F
#define C_SUB      0x6E6E73
#define C_DIS      0xB8B8BE
#define C_ACCENT   0x0A84FF
#define C_GREEN    0x2E9E4F
#define C_WARN     0xE0871A
#define C_RED      0xD93025

/* ---- page / load state --------------------------------------------------- */
enum { LS_IDLE = 0, LS_LOADING, LS_DONE, LS_ERROR };
enum { CT_HTML = 0, CT_PLAIN, CT_IMAGE, CT_OTHER };

static char page_buf[WE_PAGE_MAX + 1] __attribute__((section(".app_bss")));
static int  page_len;
static bool page_truncated;
static bool headers_seen, disk_mode, force_download, save_requested;
static download_t disk_download;
static uint8_t download_buf[16384];
static int  page_ct;
static char cur_url[WE_URL_MAX];        /* URL shown / being loaded (final after redirects) */
static char req_url[WE_URL_MAX];        /* the URL the user asked for (history entry)      */
static net_stream_t* page_ns;
static int  redirects;
static bool insecure_ok;
static bool certificate_warning;
static char insecure_origin[WE_URL_MAX];
static int  load_state;
static char err_msg[96];
static char status_msg[160];
static int  last_prog_len;
static uint32_t last_redraw;
static bool need_redraw;
static bool secure_page, trusted_page;
static char tls_info[96];
static char synth[1800];                /* generated pages (home / errors / download report) */

/* ---- images --------------------------------------------------------------- */
#define IMG_BUFS 2
static uint8_t img_buf[IMG_BUFS][WE_IMG_FILE_MAX] __attribute__((section(".app_bss")));
static int  img_len[IMG_BUFS];
static int  img_slot[IMG_BUFS];
static int  img_redir[IMG_BUFS];
static net_stream_t* img_ns[IMG_BUFS];
static int  next_img;

/* ---- history ------------------------------------------------------------- */
#define HIST 24
static char hist[HIST][WE_URL_MAX];
static int  hist_n, hist_pos;           /* hist_pos = index of the current entry (-1 none) */

/* ---- view ----------------------------------------------------------------- */
static int  win_id = -1;
static int  scroll_y;
static int  focus_link = -1;
static int  hover_link = -1;
static int  layout_w;                   /* content width the current layout used */
static bool addr_focus, addr_selall;
static char addr[WE_URL_MAX];
static int  addr_len;
static char pending_url[WE_URL_MAX];
static bool has_pending;
static int  hover_btn = -1;

static const struct { const char* label; const char* url; } BOOKMARKS[] = {
    { "Home",         "about:home" },
    { "example.com",  "https://example.com/" },
    { "CERN",         "http://info.cern.ch/" },
    { "neverssl",     "http://neverssl.com/" },
    { "Wikipedia",    "https://en.wikipedia.org/wiki/Main_Page" },
    { "Hacker News",  "https://news.ycombinator.com/" },
    { "GitHub",       "https://github.com/nexusos" },
    { "about:blank",  "about:blank" },
    { "Downloads",    "about:downloads" },
};
#define NBOOK (int)(sizeof(BOOKMARKS) / sizeof(BOOKMARKS[0]))
static int book_x[NBOOK + 1];           /* x extents of the bookmark labels (content-relative) */

static const char HOME_HTML[] =
"<html><head><title>Nexus Browser</title></head><body>"
"<br><center><h1>Nexus Browser</h1>"
"<p><font color=\"#6e6e73\">A small web browser for NexusOS. Type an address above, or pick a site below.</font></p></center>"
"<br><h3>Suggested sites</h3><ul>"
"<li><a href=\"https://example.com/\">example.com</a> &mdash; the IANA example page</li>"
"<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> &mdash; the first website</li>"
"<li><a href=\"http://neverssl.com/\">neverssl.com</a> &mdash; a plain-HTTP test page</li>"
"<li><a href=\"https://en.wikipedia.org/wiki/Main_Page\">en.wikipedia.org</a></li>"
"<li><a href=\"https://news.ycombinator.com/\">news.ycombinator.com</a></li>"
"<li><a href=\"https://github.com/nexusos\">github.com/nexusos</a></li>"
"<li><a href=\"about:blank\">about:blank</a></li></ul>"
"<h3>Keyboard</h3><table>"
"<tr><th>Key</th><th>Action</th></tr>"
"<tr><td>Ctrl+L</td><td>Edit the address, Enter to go</td></tr>"
"<tr><td>Ctrl+S</td><td>Save any URL to disk, including large pages and images</td></tr>"
"<tr><td>Tab, Enter</td><td>Next link, follow link</td></tr>"
"<tr><td>Backspace, Left / Right</td><td>Back / Forward</td></tr>"
"<tr><td>F5 or Ctrl+R</td><td>Reload</td></tr>"
"<tr><td>Up / Down / Space</td><td>Scroll</td></tr>"
"<tr><td>Esc</td><td>Stop loading</td></tr>"
"</table><p><small><font color=\"#6e6e73\">HTTP and verified HTTPS are supported. Best for simple HTML pages; JavaScript apps are not supported.</font></small></p>"
"</body></html>";

/* --------------------------------------------------------------------------
 * small helpers
 * -------------------------------------------------------------------------- */
static void scpy(char* d, const char* s, int max) { strncpy(d, s, max - 1); d[max - 1] = '\0'; }
static void scat(char* d, const char* s, int max) { int n = (int)strlen(d); while (*s && n < max - 1) d[n++] = *s++; d[n] = '\0'; }
static void scat_num(char* d, int v, int max) { char n[12]; int_to_str(v, n); scat(d, n, max); }
static void scat_bytes(char* d, uint32_t v, int max) { char n[12]; uint_to_str(v, n); scat(d, n, max); }
static void scat_html(char* d, const char* s, int max) {
    for (; *s; s++) {
        char ch[2] = {*s, 0};
        const char* escaped = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : ch;
        scat(d, escaped, max);
    }
}

static bool prefix_ci(const char* s, const char* p) {
    for (; *p; s++, p++) {
        char a = *s, b = *p;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

static window_t* my_window(void) {
    window_t* w = window_get(win_id);
    return (win_id >= 0 && w && w->active) ? w : NULL;
}

static void set_status(const char* s) { scpy(status_msg, s, sizeof(status_msg)); }

static void set_title(const char* t) {
    window_t* w = my_window();
    if (!w) return;
    const char* use = (t && t[0]) ? t : "Nexus Browser";
    scpy(w->title, use, WIN_TITLE_MAX);
}

/* viewport metrics (content-relative) */
static bool viewport(int* vx, int* vy, int* vw, int* vh) {
    int x, y, w, h;
    if (!window_content_px(win_id, &x, &y, &w, &h)) return false;
    *vx = x; *vy = y + CHROME_TOP; *vw = w; *vh = h - CHROME_TOP - STATUS_H;
    return *vh > 0;
}

static int max_scroll(void) {
    int vx, vy, vw, vh;
    if (!viewport(&vx, &vy, &vw, &vh)) return 0;
    int m = we_doc_height() + 2 * PAD - vh;
    return m > 0 ? m : 0;
}
static void clamp_scroll(void) {
    int m = max_scroll();
    if (scroll_y > m) scroll_y = m;
    if (scroll_y < 0) scroll_y = 0;
}

static void relayout(void) {
    int vx, vy, vw, vh;
    if (!viewport(&vx, &vy, &vw, &vh)) { we_layout(1000); layout_w = 1000; return; }
    layout_w = vw - 2 * PAD - 8;
    we_layout(layout_w);
    clamp_scroll();
    if (focus_link >= we_link_count()) focus_link = -1;
}

/* --------------------------------------------------------------------------
 * images
 * -------------------------------------------------------------------------- */
static void images_cancel(void) {
    for (int b = 0; b < IMG_BUFS; b++) {
        if (img_ns[b]) { ns_close(img_ns[b]); img_ns[b] = NULL; }
        img_slot[b] = -1;
    }
    next_img = 0;
}

static const char* fetch_headers(const char* url) {
    if (!insecure_ok) return NULL;
    int i = 0;
    while (insecure_origin[i] && insecure_origin[i] == url[i]) i++;
    return !insecure_origin[i] && (!url[i] || url[i] == '/' || url[i] == '?' || url[i] == '#')
        ? "X-Nexus-Insecure: 1\r\n" : NULL;
}

static void image_finish(int b, bool ok) {
    int slot = img_slot[b];
    we_image_t* im = we_image(slot);
    if (im) {
        if (ok && img_len[b] > 0) we_image_decoded(slot, img_buf[b], (uint32_t)img_len[b], layout_w);
        else im->state = WE_IMG_FAILED;
    }
    if (img_ns[b]) { ns_close(img_ns[b]); img_ns[b] = NULL; }
    img_slot[b] = -1;
    relayout();
    need_redraw = true;
}

static void images_pump(void) {
    /* start new downloads */
    for (int b = 0; b < IMG_BUFS; b++) {
        if (img_ns[b]) continue;
        while (next_img < we_image_count()) {
            we_image_t* im = we_image(next_img);
            if (im->state != WE_IMG_NEW) { next_img++; continue; }
            if (!prefix_ci(im->url, "http://") && !prefix_ci(im->url, "https://")) { im->state = WE_IMG_FAILED; next_img++; continue; }
            img_ns[b] = ns_open(im->url, "GET", fetch_headers(im->url));
            if (!img_ns[b]) { im->state = WE_IMG_FAILED; next_img++; relayout(); continue; }
            im->state = WE_IMG_LOADING;
            img_slot[b] = next_img; img_len[b] = 0; img_redir[b] = 0;
            next_img++;
            break;
        }
    }
    /* pump */
    for (int b = 0; b < IMG_BUFS; b++) {
        if (!img_ns[b]) continue;
        int rc = ns_poll(img_ns[b]);
        if (rc < 0) { image_finish(b, false); continue; }
        if (rc == 0) continue;
        const ns_info_t* in = ns_info(img_ns[b]);
        if (in->status >= 300 && in->status < 400 && in->location[0]) {
            if (img_redir[b]++ >= 4) { image_finish(b, false); continue; }
            char loc[WE_URL_MAX]; scpy(loc, in->location, sizeof(loc));
            ns_close(img_ns[b]);
            img_ns[b] = ns_open(loc, "GET", fetch_headers(loc));
            if (!img_ns[b]) { we_image_t* im = we_image(img_slot[b]); if (im) im->state = WE_IMG_FAILED; img_slot[b] = -1; relayout(); need_redraw = true; }
            continue;
        }
        if (in->status >= 400) { image_finish(b, false); continue; }
        for (int k = 0; k < 8; k++) {
            int room = WE_IMG_FILE_MAX - img_len[b];
            if (room <= 0) { image_finish(b, true); break; }        /* too big: decode what we have */
            int n = ns_read(img_ns[b], img_buf[b] + img_len[b], room);
            if (n > 0) { img_len[b] += n; continue; }
            if (n == -1) { image_finish(b, true); break; }
            if (n == -2) { image_finish(b, false); break; }
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * page completion / generated pages
 * -------------------------------------------------------------------------- */
static void show_html(const char* html, int len, const char* base) {
    we_parse(html, len, base);
    relayout();
    set_title(we_title());
    need_redraw = true;
}

static void show_error(const char* headline, const char* detail, bool offer_insecure) {
    load_state = LS_ERROR;
    certificate_warning = offer_insecure;
    scpy(err_msg, detail, sizeof(err_msg));
    synth[0] = '\0';
    scat(synth, "<html><head><title>", sizeof(synth)); scat_html(synth, headline, sizeof(synth));
    scat(synth, "</title></head><body><br><h2>", sizeof(synth)); scat_html(synth, headline, sizeof(synth));
    scat(synth, "</h2><p>", sizeof(synth)); scat_html(synth, detail, sizeof(synth));
    scat(synth, "</p><p><code>", sizeof(synth)); scat_html(synth, cur_url, sizeof(synth)); scat(synth, "</code></p>", sizeof(synth));
    if (offer_insecure) {
        scat(synth, "<p>The site's certificate could not be validated against a built-in root. "
                    "<a href=\"about:continue-insecure\">Continue anyway (insecure)</a></p>", sizeof(synth));
    }
    scat(synth, "<p><a href=\"", sizeof(synth)); scat_html(synth, cur_url, sizeof(synth));
    scat(synth, "\">Try again</a> &nbsp; <a href=\"about:home\">Home</a></p></body></html>", sizeof(synth));
    show_html(synth, (int)strlen(synth), cur_url);
    set_status(detail);
}

static const char* url_leaf(const char* url, char* out, int max) {
    const char* p = url;
    const char* last = NULL;
    for (const char* q = p; *q && *q != '?' && *q != '#'; q++) if (*q == '/') last = q;
    const char* s = last ? last + 1 : p;
    int n = 0;
    while (s[n] && s[n] != '?' && s[n] != '#' && n < max - 1) { out[n] = s[n]; n++; }
    out[n] = '\0';
    if (!n) scpy(out, "download", max);
    /* ramfs names are short */
    if ((int)strlen(out) >= FS_NAME_MAX) out[FS_NAME_MAX - 1] = '\0';
    return out;
}

static void save_download(void) {
    if (download_finish(&disk_download) < 0) {
        char error[96]; scpy(error, disk_download.error, sizeof(error));
        download_cancel(&disk_download); show_error("Download failed", error, false); return;
    }
    synth[0] = 0;
    scat(synth, "<html><head><title>Download complete</title></head><body><h2>Download complete</h2><p>Saved <code>", sizeof(synth));
    scat_html(synth, disk_download.path, sizeof(synth));
    scat(synth, "</code></p><p>", sizeof(synth)); scat_bytes(synth, disk_download.written, sizeof(synth));
    scat(synth, " bytes saved.</p><p><a href=\"about:downloads\">Open Downloads</a> &nbsp; <a href=\"about:home\">Home</a></p></body></html>", sizeof(synth));
    show_html(synth, strlen(synth), cur_url); set_status("Download saved to disk");
}

static void finish_page(void) {
    load_state = LS_DONE;
    if (page_ns) { ns_close(page_ns); page_ns = NULL; }
    images_cancel();                        /* fresh image table for this page */
    switch (page_ct) {
    case CT_HTML:
        page_buf[page_len] = '\0';
        we_parse(page_buf, page_len, cur_url);
        relayout();
        set_title(we_title());
        break;
    case CT_PLAIN:
        we_parse_plain(page_buf, page_len, cur_url);
        relayout();
        { char t[48]; url_leaf(cur_url, t, sizeof(t)); set_title(t); }
        break;
    case CT_IMAGE: {
        char leaf[48]; url_leaf(cur_url, leaf, sizeof(leaf));
        synth[0] = '\0';
        scat(synth, "<html><head><title>", sizeof(synth)); scat_html(synth, leaf, sizeof(synth));
        scat(synth, "</title></head><body><img src=\"", sizeof(synth)); scat_html(synth, cur_url, sizeof(synth));
        scat(synth, "\" alt=\"", sizeof(synth)); scat_html(synth, leaf, sizeof(synth)); scat(synth, "\"></body></html>", sizeof(synth));
        we_parse(synth, (int)strlen(synth), cur_url);
        int vx, vy, vw, vh;
        int cw = viewport(&vx, &vy, &vw, &vh) ? vw - 2 * PAD - 8 : 1000;
        if (we_image_count() > 0) {
            we_image_decoded(0, (const uint8_t*)page_buf, (uint32_t)page_len, cw);
            we_image_t* im = we_image(0);
            if (im && im->state == WE_IMG_READY) {
                char t[64]; scpy(t, leaf, sizeof(t)); scat(t, " (", sizeof(t)); scat_num(t, im->nat_w, sizeof(t));
                scat(t, " x ", sizeof(t)); scat_num(t, im->nat_h, sizeof(t)); scat(t, ")", sizeof(t)); set_status(t);
            } else set_status("Image could not be decoded (format or size unsupported)");
        }
        relayout();
        set_title(leaf);
        break;
    }
    default:
        save_download();
        break;
    }
    if (page_ct == CT_HTML || page_ct == CT_PLAIN) {
        char s[160];
        if (page_truncated) scpy(s, "Page truncated at 256 KB", sizeof(s));
        else { scpy(s, "Done - ", sizeof(s)); scat_num(s, (page_len + 1023) / 1024, sizeof(s)); scat(s, " KB", sizeof(s)); }
        if (tls_info[0]) { scat(s, "  |  ", sizeof(s)); scat(s, tls_info, sizeof(s)); }
        set_status(s);
    }
    need_redraw = true;
}

/* --------------------------------------------------------------------------
 * navigation
 * -------------------------------------------------------------------------- */
static void cancel_load(void) {
    if (page_ns) { ns_close(page_ns); page_ns = NULL; }
    if (disk_download.active) download_cancel(&disk_download);
    images_cancel();
}

static void stop_loading(void) {
    if (load_state != LS_LOADING) return;
    cancel_load();
    if (disk_mode) {
        if (disk_download.file.pending) { show_error("Cleanup failed", disk_download.error, false); return; }
        static const char cancelled[] = "<html><head><title>Download cancelled</title></head><body><h2>Download cancelled</h2><p>The temporary download was removed.</p><p><a href=\"about:downloads\">Open Downloads</a></p></body></html>";
        load_state = LS_DONE; show_html(cancelled, sizeof(cancelled) - 1, cur_url); set_status("Download cancelled");
    } else if (page_len > 0 && page_ct == CT_HTML) { page_truncated = true; finish_page(); }
    else { load_state = LS_DONE; set_status("Stopped"); }
}

static void history_push(const char* url) {
    /* drop the forward branch */
    if (hist_pos + 1 < hist_n) hist_n = hist_pos + 1;
    if (hist_n > 0 && !strcmp(hist[hist_n - 1], url)) { hist_pos = hist_n - 1; return; }
    if (hist_n == HIST) { for (int i = 0; i < HIST - 1; i++) strcpy(hist[i], hist[i + 1]); hist_n--; }
    scpy(hist[hist_n], url, WE_URL_MAX);
    hist_pos = hist_n;
    hist_n++;
}

static void start_fetch(const char* url) {
    page_ns = ns_open(url, "GET", fetch_headers(url));
    if (!page_ns) { show_error("Could not load page", "No free network stream (or unsupported URL scheme).", false); return; }
    load_state = LS_LOADING;
    page_len = 0; page_truncated = false; page_ct = CT_HTML; last_prog_len = 0;
    headers_seen = false; disk_mode = false;
    set_status("Connecting...");
}

static void navigate(const char* url_in, bool push) {
    char url[WE_URL_MAX];
    scpy(url, url_in, sizeof(url));
    /* trim */
    { int n = (int)strlen(url); while (n > 0 && (url[n-1] == ' ' || url[n-1] == '\r' || url[n-1] == '\n')) url[--n] = '\0'; }
    { const char* p = url; while (*p == ' ') p++; if (p != url) { char t[WE_URL_MAX]; scpy(t, p, sizeof(t)); scpy(url, t, sizeof(url)); } }
    if (!url[0]) scpy(url, "about:home", sizeof(url));
    if (!strcmp(url, "about:cancel-download")) { stop_loading(); return; }
    if (!strcmp(url, "about:downloads")) { scpy(addr, cur_url, sizeof(addr)); addr_len = (int)strlen(addr); filemgr_open_path("/disk/Downloads"); return; }
    if (!strcmp(url, "about:continue-insecure")) {
        if (!certificate_warning || load_state != LS_ERROR) return;
        insecure_ok = true;
        scpy(insecure_origin, cur_url, sizeof(insecure_origin));
        int end = 8;
        while (insecure_origin[end] && insecure_origin[end] != '/' && insecure_origin[end] != '?' && insecure_origin[end] != '#') end++;
        insecure_origin[end] = '\0';
        scpy(url, cur_url, sizeof(url));
        push = false;
    } else insecure_ok = false;
    certificate_warning = false;
    cancel_load();
    force_download = save_requested; save_requested = false;
    load_state = LS_IDLE;
    err_msg[0] = '\0'; tls_info[0] = '\0'; secure_page = trusted_page = false;
    scroll_y = 0; focus_link = -1; hover_link = -1; redirects = 0;
    addr_focus = false;

    if (!strcmp(url, "about:home") || !strcmp(url, "about:")) {
        scpy(cur_url, "about:home", sizeof(cur_url));
        scpy(addr, cur_url, sizeof(addr)); addr_len = (int)strlen(addr);
        if (push) history_push(cur_url);
        load_state = LS_DONE;
        show_html(HOME_HTML, (int)sizeof(HOME_HTML) - 1, cur_url);
        set_status("Nexus Browser - type an address or pick a bookmark");
        return;
    }
    if (!strcmp(url, "about:blank")) {
        scpy(cur_url, url, sizeof(cur_url));
        scpy(addr, cur_url, sizeof(addr)); addr_len = (int)strlen(addr);
        if (push) history_push(cur_url);
        load_state = LS_DONE;
        show_html("<html><head><title>about:blank</title></head><body></body></html>", 66, cur_url);
        set_status("");
        return;
    }
    if (!prefix_ci(url, "http://") && !prefix_ci(url, "https://")) {
        /* bare host or unsupported scheme */
        bool scheme = false;
        for (int i = 0; url[i] && i < 12; i++) if (url[i] == ':') { scheme = true; break; }
        if (scheme) {
            scpy(cur_url, url, sizeof(cur_url));
            scpy(addr, cur_url, sizeof(addr)); addr_len = (int)strlen(addr);
            show_error("Unsupported address", "Only http:// and https:// addresses can be opened.", false);
            return;
        }
        char t[WE_URL_MAX]; scpy(t, "http://", sizeof(t)); scat(t, url, sizeof(t)); scpy(url, t, sizeof(url));
    }
    scpy(cur_url, url, sizeof(cur_url));
    scpy(req_url, url, sizeof(req_url));
    scpy(addr, cur_url, sizeof(addr)); addr_len = (int)strlen(addr);
    if (push) history_push(cur_url);
    start_fetch(cur_url);
    need_redraw = true;
}

static void go_back(void)    { if (hist_pos > 0) { hist_pos--; navigate(hist[hist_pos], false); } }
static void go_forward(void) { if (hist_pos + 1 < hist_n) { hist_pos++; navigate(hist[hist_pos], false); } }
static void reload(void)     { insecure_ok = false; navigate(cur_url, false); }
static void save_current(void) {
    if (!prefix_ci(cur_url, "http://") && !prefix_ci(cur_url, "https://")) { set_status("Open a web address before saving"); return; }
    save_requested = true;
    navigate(cur_url, false);
}

static void follow_link(int li) {
    const we_link_t* l = we_link(li);
    if (!l) return;
    char target[WE_URL_MAX];
    we_resolve_href(l->href, target);
    if (!target[0]) return;
    if (target[0] == '#') { scroll_y = 0; need_redraw = true; return; }
    navigate(target, true);
}

/* --------------------------------------------------------------------------
 * page stream pump (called from browser_poll)
 * -------------------------------------------------------------------------- */
static void classify_content(const ns_info_t* in) {
    const char* ct = in->content_type;
    if (!ct[0]) {
        /* sniff */
        if (page_len >= 4 && (!memcmp(page_buf, "\x89PNG", 4) || !memcmp(page_buf, "GIF8", 4) || !memcmp(page_buf, "\xFF\xD8\xFF", 3) || !memcmp(page_buf, "BM", 2)))
            page_ct = CT_IMAGE;
        else page_ct = CT_HTML;
        return;
    }
    if (prefix_ci(ct, "text/html") || prefix_ci(ct, "application/xhtml")) page_ct = CT_HTML;
    else if (prefix_ci(ct, "text/")) page_ct = CT_PLAIN;
    else if (we_is_image_type(ct)) page_ct = CT_IMAGE;
    else if (prefix_ci(ct, "application/json") || prefix_ci(ct, "application/xml") || prefix_ci(ct, "application/javascript")) page_ct = CT_PLAIN;
    else page_ct = CT_OTHER;
}

static void page_pump(void) {
    if (!page_ns) return;
    uint32_t t0 = system_ticks;
    for (int round = 0; round < 12 && page_ns; round++) {
        int rc = ns_poll(page_ns);
        if (rc < 0) {
            char d[96]; scpy(d, ns_error(page_ns), sizeof(d));
            if (!d[0]) scpy(d, "The connection failed.", sizeof(d));
            ns_close(page_ns); page_ns = NULL;
            const char* head = "Could not connect";
            if (strstr(d, "DNS")) head = "Server not found";
            else if (strstr(d, "TLS") || strstr(d, "certificate")) head = "Secure connection failed";
              show_error(head, d, prefix_ci(cur_url, "https://") && strstr(d, "certificate rejected:") != NULL);
            return;
        }
        if (rc == 0) { if (system_ticks - t0 >= 1) break; else continue; }
        const ns_info_t* in = ns_info(page_ns);
        /* TLS trust */
        if (in->secure && !in->trusted && !insecure_ok) {
            secure_page = true; trusted_page = false;
            scpy(tls_info, in->tls_info, sizeof(tls_info));
            ns_close(page_ns); page_ns = NULL;
            show_error("Untrusted certificate", "This site uses HTTPS but its certificate is not trusted.", true);
            return;
        }
        /* redirect */
        if (in->status >= 300 && in->status < 400 && in->location[0]) {
            if (redirects++ >= 4) { ns_close(page_ns); page_ns = NULL; show_error("Too many redirects", "The page redirected more than 4 times.", false); return; }
            char loc[WE_URL_MAX]; scpy(loc, in->location, sizeof(loc));
            ns_close(page_ns); page_ns = NULL;
            scpy(cur_url, loc, sizeof(cur_url));
            scpy(addr, cur_url, sizeof(addr)); addr_len = (int)strlen(addr);
            if (hist_pos >= 0 && hist_pos < hist_n) scpy(hist[hist_pos], cur_url, WE_URL_MAX);
            start_fetch(cur_url);
            { char s[160]; scpy(s, "Redirected to ", sizeof(s)); scat(s, cur_url, sizeof(s)); set_status(s); }
            need_redraw = true;
            continue;
        }
        if (!headers_seen) {
            headers_seen = true;
            secure_page = in->secure; trusted_page = in->trusted;
            scpy(tls_info, in->tls_info, sizeof(tls_info));
            classify_content(in);
            if (force_download || in->attachment || page_ct == CT_OTHER) {
                page_ct = CT_OTHER; disk_mode = true;
                if (in->status < 200 || in->status >= 300) { cancel_load(); show_error("Download failed", "The server returned an HTTP error.", false); return; }
                if (download_begin(&disk_download, cur_url, in->filename, NULL) < 0) {
                    char error[96]; scpy(error, disk_download.error, sizeof(error));
                    cancel_load(); show_error("Download failed", error, false); return;
                }
                images_cancel();
                static const char progress[] = "<html><head><title>Downloading</title></head><body><h2>Downloading to disk</h2><p>Progress appears below. Press Esc to cancel.</p><p><a href=\"about:downloads\">Open Downloads</a> &nbsp; <a href=\"about:cancel-download\">Cancel download</a></p></body></html>";
                show_html(progress, sizeof(progress) - 1, cur_url);
            }
        }
        /* body */
        bool done = false;
        for (int k = 0; k < (disk_mode ? 2 : 16); k++) {
            int room = disk_mode ? (int)sizeof(download_buf) : WE_PAGE_MAX - page_len;
            if (room <= 0) { page_truncated = true; done = true; break; }
            int n = ns_read(page_ns, disk_mode ? (void*)download_buf : (void*)(page_buf + page_len), room);
            if (n > 0) {
                if (disk_mode) {
                    if (download_write(&disk_download, download_buf, (uint32_t)n) < 0) {
                        char error[96]; scpy(error, disk_download.error, sizeof(error));
                        cancel_load(); show_error("Download failed", error, false); return;
                    }
                } else { page_len += n; if (!in->content_type[0] && page_len <= 4096) classify_content(in); }
                continue;
            }
            if (n == -1) { done = true; break; }
            if (n == -2) {
                char error[96]; scpy(error, ns_error(page_ns), sizeof(error));
                cancel_load(); show_error("Download interrupted", error, false); return;
            }
            break;
        }
        {
            uint32_t received = disk_mode ? disk_download.written : (uint32_t)page_len;
            char s[96]; scpy(s, disk_mode ? "Downloading... " : "Loading... ", sizeof(s)); scat_bytes(s, received / 1024, sizeof(s)); scat(s, " KiB", sizeof(s));
            if (in->content_length) { scat(s, " of ", sizeof(s)); scat_bytes(s, in->content_length / 1024, sizeof(s)); scat(s, " KiB", sizeof(s)); }
            set_status(s);
        }
        if (done) { finish_page(); return; }
        /* progressive render of HTML every 32 KB */
        if (page_ct == CT_HTML && page_len - last_prog_len >= 32 * 1024) {
            last_prog_len = page_len;
            page_buf[page_len] = '\0';
            we_parse(page_buf, page_len, cur_url);
            relayout();
            set_title(we_title());
            need_redraw = true;
        }
        if (system_ticks - t0 >= 2) break;
    }
}

/* --------------------------------------------------------------------------
 * drawing
 * -------------------------------------------------------------------------- */
static void glyph_arrow(int cx, int cy, bool left, uint32_t c) {
    int d = left ? -1 : 1;
    ui_line(cx - 5 * d, cy, cx + 5 * d, cy, 2, c, 255);
    ui_line(cx + 5 * d, cy, cx + 0 * d, cy - 5, 2, c, 255);
    ui_line(cx + 5 * d, cy, cx + 0 * d, cy + 5, 2, c, 255);
}
static void glyph_reload(int cx, int cy, uint32_t c) {
    ui_ring(cx, cy, 6, 2, c, 255);
    fb_fill_rect(cx + 1, cy - 8, 7, 7, C_CHROME);            /* gap */
    ui_tri(cx + 2, cy - 9, cx + 9, cy - 5, cx + 2, cy - 2, c, 255);
}
static void glyph_home(int cx, int cy, uint32_t c) {
    ui_tri(cx, cy - 8, cx - 9, cy, cx + 9, cy, c, 255);
    fb_fill_rect(cx - 6, cy - 1, 12, 8, c);
    fb_fill_rect(cx - 2, cy + 2, 4, 5, C_CHROME);
}
static void glyph_lock(int cx, int cy, uint32_t c) {
    ui_ring(cx, cy - 3, 4, 2, c, 255);
    fb_fill_rect(cx - 6, cy - 2, 12, 8, c);
}
static void glyph_warn(int cx, int cy, uint32_t c) {
    ui_tri(cx, cy - 8, cx - 8, cy + 6, cx + 8, cy + 6, c, 255);
    fb_fill_rect(cx - 1, cy - 4, 2, 6, 0xFFFFFF);
    fb_fill_rect(cx - 1, cy + 3, 2, 2, 0xFFFFFF);
}

static void draw_button(int x, int y, int idx, bool enabled, bool hot) {
    uint32_t fill = hot && enabled ? 0xDADAE0 : C_CHROME;
    if (fill != C_CHROME) ui_rrect(x, y, BTN, BTN, 7, fill, 255);
    uint32_t c = enabled ? 0x3A3A40 : C_DIS;
    int cx = x + BTN / 2, cy = y + BTN / 2;
    switch (idx) {
    case 0: glyph_arrow(cx, cy, true, c); break;
    case 1: glyph_arrow(cx, cy, false, c); break;
    case 2: glyph_reload(cx, cy, c); break;
    case 4:
        ui_line(cx, cy - 7, cx, cy + 4, 2, c, 255);
        ui_line(cx - 5, cy, cx, cy + 5, 2, c, 255); ui_line(cx, cy + 5, cx + 5, cy, 2, c, 255);
        ui_line(cx - 7, cy + 9, cx + 7, cy + 9, 2, c, 255); break;
    default: glyph_home(cx, cy, c); break;
    }
}

static void browser_draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    int x, y, w, h;
    if (!appui_content(id, &x, &y, &w, &h)) return;
    int vx, vy, vw, vh;
    if (!viewport(&vx, &vy, &vw, &vh)) return;
    int cw = vw - 2 * PAD - 8;
    if (cw != layout_w && cw > 60) { layout_w = cw; we_layout(cw); clamp_scroll(); }

    /* ---- page viewport (drawn first; the chrome overpaints any spill) ---- */
    fb_fill_rect(vx, vy, vw, vh, 0xFFFFFF);
    we_paint(vx + PAD, vy, cw, vh, scroll_y - PAD, focus_link);

    /* overlay scrollbar */
    int doc = we_doc_height() + 2 * PAD;
    if (doc > vh && vh > 40) {
        int track = vh - 8;
        int th = (int)((uint32_t)track * (uint32_t)vh / (uint32_t)doc);
        if (th < 30) th = 30;
        int ms = max_scroll();
        int ty = vy + 4 + (ms > 0 ? (int)((uint32_t)(track - th) * (uint32_t)scroll_y / (uint32_t)ms) : 0);
        ui_rrect(vx + vw - 11, ty, 7, th, 3, 0x000000, 90);
    }

    /* ---- toolbar ---- */
    fb_fill_rect(x, y, w, TOOLBAR_H, C_CHROME);
    draw_button(x + 10, y + BTN_Y, 0, hist_pos > 0, hover_btn == 0);
    draw_button(x + 44, y + BTN_Y, 1, hist_pos + 1 < hist_n, hover_btn == 1);
    draw_button(x + 78, y + BTN_Y, 2, true, hover_btn == 2);
    draw_button(x + 112, y + BTN_Y, 3, true, hover_btn == 3);
    draw_button(x + 146, y + BTN_Y, 4, true, hover_btn == 4);
    /* address field */
    int fx = x + ADDR_X, fw = w - ADDR_X - 12, fy = y + 7, fh = 30;
    ui_rrect(fx, fy, fw, fh, 8, C_FIELD, 255);
    ui_rrect_stroke(fx, fy, fw, fh, 8, addr_focus ? C_ACCENT : C_FIELDBD, 255);
    if (addr_focus) ui_rrect_stroke(fx - 1, fy - 1, fw + 2, fh + 2, 9, C_ACCENT, 120);
    int gx = fx + 16, gy = fy + fh / 2;
    bool is_https = prefix_ci(cur_url, "https://");
    if (is_https && secure_page && trusted_page) glyph_lock(gx, gy, C_GREEN);
    else if (is_https) glyph_warn(gx, gy, C_WARN);
    else if (prefix_ci(cur_url, "http://")) glyph_warn(gx, gy, C_DIS);
    int tx = fx + 32, tmax = fx + fw - 12;
    int ty = fy + (fh - ui_font_height(UF_UI)) / 2 + 1;
    const char* shown = addr;
    int sl = addr_len;
    /* show the tail that fits */
    int start = 0;
    while (start < sl && tx + ui_text_width(shown + start, UF_UI) > tmax) start += 8;
    if (start > sl) start = sl;
    if (addr_focus && addr_selall && sl > 0) {
        int tw = ui_text_width(shown + start, UF_UI);
        ui_rrect(tx - 2, fy + 5, (tw < tmax - tx ? tw : tmax - tx) + 4, fh - 10, 3, 0xB4D5FE, 255);
    }
    int end = ui_text(tx, ty, shown + start, UF_UI, C_TXT);
    if (addr_focus && (system_ticks % 16) < 10) fb_fill_rect(end + 1, fy + 7, 2, fh - 14, C_ACCENT);
    if (!addr_focus && load_state == LS_LOADING) {
        /* small spinner dot */
        int ph = (int)(system_ticks % 8);
        ui_circle(fx + fw - 14, gy, 3, ph < 4 ? C_ACCENT : 0x9CC7FF, 255);
    }

    /* ---- bookmarks bar ---- */
    int by = y + TOOLBAR_H;
    fb_fill_rect(x, by, w, BOOKBAR_H, C_CHROME2);
    fb_fill_rect(x, by, w, 1, C_SEP);
    {
        int bx = x + 14;
        int bty = by + (BOOKBAR_H - ui_font_height(UF_SMALL)) / 2 + 1;
        for (int i = 0; i < NBOOK; i++) {
            int tw = ui_text_width(BOOKMARKS[i].label, UF_SMALL);
            book_x[i] = bx - x;
            if (bx + tw > x + w - 10) { book_x[i] = -1; continue; }
            ui_text(bx, bty, BOOKMARKS[i].label, UF_SMALL, 0x3A3A40);
            bx += tw + 22;
        }
        book_x[NBOOK] = bx - x;
    }
    /* ---- progress bar ---- */
    int py = by + BOOKBAR_H;
    fb_fill_rect(x, py, w, PROGRESS_H, C_SEP);
    if (load_state == LS_LOADING) {
        const ns_info_t* in = page_ns ? ns_info(page_ns) : NULL;
        if (in && in->content_length) {
            uint32_t received = disk_mode ? disk_download.written : (uint32_t)page_len;
            uint32_t pw = (uint32_t)w * download_percent(received, in->content_length) / 100;
            if (pw > (uint32_t)w) pw = (uint32_t)w;
            fb_fill_rect(x, py, (int)pw, PROGRESS_H, C_ACCENT);
        } else {
            int seg = w / 5, pos = (int)((system_ticks * 23) % (uint32_t)(w + seg)) - seg;
            int sx = pos < 0 ? x : x + pos, ex = x + pos + seg; if (ex > x + w) ex = x + w;
            if (ex > sx) fb_fill_rect(sx, py, ex - sx, PROGRESS_H, C_ACCENT);
        }
    }

    /* ---- status line ---- */
    int sy = y + h - STATUS_H;
    fb_fill_rect(x, sy, w, STATUS_H, C_CHROME);
    fb_fill_rect(x, sy, w, 1, C_SEP);
    const char* st = status_msg;
    char hov[WE_URL_MAX];
    if (hover_link >= 0 && we_link(hover_link)) { we_resolve_href(we_link(hover_link)->href, hov); st = hov; }
    else if (focus_link >= 0 && we_link(focus_link)) { we_resolve_href(we_link(focus_link)->href, hov); st = hov; }
    uint32_t sc = (load_state == LS_ERROR && st == status_msg) ? C_RED : C_SUB;
    int smax = w - 24;
    {
        /* clip by characters */
        char line[200]; int k = 0;
        while (st[k] && k < 199) { line[k] = st[k]; k++; }
        line[k] = '\0';
        while (k > 0 && ui_text_width(line, UF_SMALL) > smax) { line[--k] = '\0'; }
        ui_text(x + 12, sy + (STATUS_H - ui_font_height(UF_SMALL)) / 2 + 1, line, UF_SMALL, sc);
    }
    if (we_link_count() > 0) {
        char lc[24]; scpy(lc, "", sizeof(lc)); scat_num(lc, we_link_count(), sizeof(lc)); scat(lc, " links", sizeof(lc));
        int lw = ui_text_width(lc, UF_SMALL);
        ui_text(x + w - 12 - lw, sy + (STATUS_H - ui_font_height(UF_SMALL)) / 2 + 1, lc, UF_SMALL, C_DIS);
    }
}

/* --------------------------------------------------------------------------
 * input
 * -------------------------------------------------------------------------- */
static void addr_commit(void) {
    addr[addr_len] = '\0';
    addr_focus = false; addr_selall = false;
    navigate(addr, true);
}

static void scroll_to_link(int li) {
    const we_link_t* l = we_link(li);
    int vx, vy, vw, vh;
    if (!l || !viewport(&vx, &vy, &vw, &vh)) return;
    int top = l->y + PAD, bot = top + l->h;
    if (top < scroll_y + 10) scroll_y = top - 10;
    else if (bot > scroll_y + vh - 10) scroll_y = bot - vh + 10;
    clamp_scroll();
}

static void browser_key(int id, char key) {
    (void)id;
    unsigned char k = (unsigned char)key;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    bool have_vp = viewport(&vx, &vy, &vw, &vh);
    need_redraw = true;

    if (key == 19) { addr_focus = false; save_current(); return; }
    if (addr_focus) {
        if (key == '\n') { addr_commit(); return; }
        if (key == 27) { addr_focus = false; addr_selall = false; scpy(addr, cur_url, sizeof(addr)); addr_len = (int)strlen(addr); return; }
        if (key == 12) { addr_selall = true; return; }
        if (key == 22) {
            const char* c = clipboard_paste();
            if (addr_selall) { addr_len = 0; addr_selall = false; }
            if (c) while (*c && addr_len < WE_URL_MAX - 1) { if (*c >= 32 && *c < 127) addr[addr_len++] = *c; c++; }
            addr[addr_len] = '\0';
            return;
        }
        if (key == '\b') { if (addr_selall) { addr_len = 0; addr_selall = false; } else if (addr_len > 0) addr_len--; addr[addr_len] = '\0'; return; }
        if (key >= 32 && key < 127) {
            if (addr_selall) { addr_len = 0; addr_selall = false; }
            if (addr_len < WE_URL_MAX - 1) { addr[addr_len++] = key; addr[addr_len] = '\0'; }
            return;
        }
        if (k == KEYC_LEFT || k == KEYC_RIGHT || k == KEYC_UP || k == KEYC_DOWN) { addr_selall = false; return; }
        return;
    }

    switch (k) {
    case 12:                                     /* Ctrl+L */
        addr_focus = true; addr_selall = true; return;
    case 19:                                     /* Ctrl+S */
        save_current(); return;
    case 27:                                     /* Esc: stop */
        stop_loading();
        return;
    case '\t':
        if (we_link_count() > 0) { focus_link = (focus_link + 1) % we_link_count(); scroll_to_link(focus_link); }
        return;
    case '\n':
        if (focus_link >= 0) follow_link(focus_link);
        return;
    case '\b': case KEYC_LEFT:
        go_back(); return;
    case KEYC_RIGHT:
        go_forward(); return;
    case KEYC_UP:
        scroll_y -= SCROLL_STEP; clamp_scroll(); return;
    case KEYC_DOWN:
        scroll_y += SCROLL_STEP; clamp_scroll(); return;
    case ' ':
        if (have_vp) scroll_y += vh - 40;
        clamp_scroll();
        return;
    case 18:                                     /* Ctrl+R */
        reload(); return;
    case 'r': case 'R':
        if (load_state != LS_LOADING) reload();
        return;
    default:
        if (k == (unsigned char)KEYC_F(5)) { reload(); return; }
        break;
    }
}

static void browser_click(int id, int px, int py) {
    int x, y, w, h;
    if (!appui_content(id, &x, &y, &w, &h)) return;
    need_redraw = true;
    /* toolbar */
    if (py >= 0 && py < TOOLBAR_H) {
        if (py >= BTN_Y && py < BTN_Y + BTN) {
            if (px >= 10 && px < 10 + BTN) { go_back(); return; }
            if (px >= 44 && px < 44 + BTN) { go_forward(); return; }
            if (px >= 78 && px < 78 + BTN) { reload(); return; }
            if (px >= 112 && px < 112 + BTN) { navigate("about:home", true); return; }
            if (px >= 146 && px < 146 + BTN) { save_current(); return; }
        }
        if (px >= ADDR_X && px < w - 12) { addr_focus = true; addr_selall = true; return; }
        addr_focus = false;
        return;
    }
    addr_focus = false;
    /* bookmarks */
    if (py >= TOOLBAR_H && py < TOOLBAR_H + BOOKBAR_H) {
        for (int i = 0; i < NBOOK; i++) {
            if (book_x[i] < 0) continue;
            int tw = ui_text_width(BOOKMARKS[i].label, UF_SMALL);
            if (px >= book_x[i] - 6 && px < book_x[i] + tw + 8) { navigate(BOOKMARKS[i].url, true); return; }
        }
        return;
    }
    /* viewport */
    int vx, vy, vw, vh;
    if (!viewport(&vx, &vy, &vw, &vh)) return;
    int ry = py - CHROME_TOP;
    if (ry < 0 || ry >= vh) return;
    /* overlay scrollbar: jump so the thumb centres on the click */
    if (px >= w - 16 && we_doc_height() + 2 * PAD > vh) {
        int ms = max_scroll();
        int track = vh - 8;
        int th = (int)((uint32_t)track * (uint32_t)vh / (uint32_t)(we_doc_height() + 2 * PAD));
        if (th < 30) th = 30;
        int rel = ry - 4 - th / 2;
        if (rel < 0) rel = 0;
        if (track - th > 0) scroll_y = (int)((uint32_t)rel * (uint32_t)ms / (uint32_t)(track - th));
        clamp_scroll();
        return;
    }
    int li = we_link_at(px - PAD, ry + scroll_y - PAD);
    if (li >= 0) { focus_link = li; follow_link(li); }
}

/* --------------------------------------------------------------------------
 * public API
 * -------------------------------------------------------------------------- */
void browser_init(void) {
    win_id = -1; hist_n = 0; hist_pos = -1; has_pending = false;
    for (int b = 0; b < IMG_BUFS; b++) { img_ns[b] = NULL; img_slot[b] = -1; }
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Nexus Browser initialized\n");
}

static void create_window(void) {
    int cols = (int)fb_get_width() / 8, rows = (int)fb_get_height() / 16;
    int w = cols >= 160 ? 150 : cols - 8;
    int h = rows >= 60 ? 54 : rows - 10;
    if (w < 60) w = 60;
    if (h < 20) h = 20;
    win_id = window_create("Nexus Browser", (cols - w) / 2, 4, w, h, browser_draw, browser_key);
    if (win_id < 0) return;
    window_set_on_click(win_id, browser_click);
    scroll_y = 0; focus_link = -1; hover_link = -1; addr_focus = false; layout_w = 0;
    hist_n = 0; hist_pos = -1;
}

void browser_open(const char* url) {
    window_t* w = my_window();
    if (w && w->draw != browser_draw) w = NULL;           /* slot reused by another app */
    if (!w) {
        cancel_load();
        create_window();
        if (win_id < 0) { syslog_add("Browser: no window slot"); return; }
    } else {
        if (w->flags & WIN_MINIMIZED) window_restore(win_id); else window_focus(win_id);
        if (!url || !url[0]) return;
    }
    navigate((url && url[0]) ? url : "about:home", true);
    syslog_add("Browser opened");
}

void browser_set_pending(const char* url) {
    scpy(pending_url, (url && url[0]) ? url : "about:home", sizeof(pending_url));
    has_pending = true;
}

bool browser_claims_key(char key) {
    window_t* w = my_window();
    if (!w || w->draw != browser_draw || !(w->flags & WIN_FOCUSED)) return false;
    unsigned char k = (unsigned char)key;
    return key == '\t' || key == 12 || key == 19 || key == 27 || k == (unsigned char)KEYC_F(5);
}

bool browser_poll(void) {
    /* window closed (traffic light / Ctrl+W / desktop exit)? */
    window_t* w = my_window();
    if (win_id >= 0 && (!w || w->draw != browser_draw)) {
        cancel_load();
        win_id = -1;
        load_state = LS_IDLE;
    }
    if (has_pending) {
        has_pending = false;
        browser_open(pending_url);
        return true;
    }
    if (win_id < 0) return false;

    need_redraw = false;
    page_pump();
    /* images load only once the page is complete: a progressive re-parse
     * rebuilds the image table, which would orphan in-flight downloads */
    if (load_state != LS_LOADING) images_pump();

    /* link hover -> status line */
    {
        mouse_state_t ms = mouse_get_state();
        int vx, vy, vw, vh;
        int hl = -1, hb = -1;
        if (viewport(&vx, &vy, &vw, &vh) && !(w->flags & WIN_MINIMIZED)) {
            int x, y, cw, ch;
            window_content_px(win_id, &x, &y, &cw, &ch);
            int px = ms.px - x, py = ms.py - y;
            if (px >= 0 && px < cw && py >= CHROME_TOP && py < CHROME_TOP + vh && window_hit_test_px(ms.px, ms.py) == win_id)
                hl = we_link_at(px - PAD, py - CHROME_TOP + scroll_y - PAD);
            if (py >= BTN_Y && py < BTN_Y + BTN && window_hit_test_px(ms.px, ms.py) == win_id) {
                if (px >= 10 && px < 40) hb = 0; else if (px >= 44 && px < 74) hb = 1;
                else if (px >= 78 && px < 108) hb = 2; else if (px >= 112 && px < 142) hb = 3;
                else if (px >= 146 && px < 176) hb = 4;
            }
        }
        if (hl != hover_link) { hover_link = hl; need_redraw = true; }
        if (hb != hover_btn) { hover_btn = hb; need_redraw = true; }
    }
    /* periodic repaint while loading (progress bar / status), throttled */
    if (load_state == LS_LOADING && system_ticks - last_redraw >= 9) need_redraw = true;
    if (addr_focus && system_ticks - last_redraw >= 8) need_redraw = true;   /* caret blink */
    if (need_redraw) last_redraw = system_ticks;
    return need_redraw;
}
