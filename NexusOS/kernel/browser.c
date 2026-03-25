/* ============================================================================
 * NexusOS — Text Web Browser — Phase 29
 * ============================================================================
 * Lynx-style text browser rendered inside a GUI window.
 * Uses window_create() with draw/key callbacks (non-blocking).
 * Navigate links with Tab/Enter, history with Backspace.
 * ============================================================================ */

#include "browser.h"
#include "http.h"
#include "html.h"
#include "dns.h"
#include "ip.h"
#include "vga.h"
#include "gui.h"
#include "window.h"
#include "theme.h"
#include "keyboard.h"
#include "string.h"
#include "syslog.h"
#include "notifcenter.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Browser State
 * -------------------------------------------------------------------------- */
static char current_url[BROWSER_URL_MAX];
static html_page_t page;
static int scroll_pos = 0;
static int selected_link = -1;
static char status_msg[60];
static int browser_win_id = -1;
static bool browser_loading = false;

/* URL input mode */
static bool url_input_mode = false;
static char url_input_buf[BROWSER_URL_MAX];
static int  url_input_len = 0;

/* History */
static char history[BROWSER_HISTORY_SIZE][BROWSER_URL_MAX];
static int  history_count = 0;
static int  history_pos = 0;

/* Bookmarks */
static char bookmarks[BROWSER_BOOKMARK_SIZE][BROWSER_URL_MAX];
static int  bookmark_count = 0;

/* Rendered lines */
#define MAX_LINES 200
#define LINE_WIDTH 70
static char lines[MAX_LINES][LINE_WIDTH + 1];
static uint8_t line_colors[MAX_LINES];
static int  line_link[MAX_LINES];   /* Link index, or -1 */
static int  total_lines = 0;

/* Link table */
#define MAX_LINKS 32
static char link_hrefs[MAX_LINKS][HTML_MAX_HREF];
static int  link_line[MAX_LINKS];   /* Which rendered line */
static int  total_links = 0;

/* --------------------------------------------------------------------------
 * browser_init
 * -------------------------------------------------------------------------- */
void browser_init(void) {
    current_url[0] = '\0';
    history_count = 0;
    history_pos = 0;
    bookmark_count = 0;
    scroll_pos = 0;
    selected_link = -1;
    total_lines = 0;
    total_links = 0;
    status_msg[0] = '\0';
    browser_win_id = -1;
    browser_loading = false;
    url_input_mode = false;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Text browser initialized\n");
}

/* --------------------------------------------------------------------------
 * add_line: Add a rendered line
 * -------------------------------------------------------------------------- */
static void add_line(const char* text, uint8_t color, int link_idx) {
    if (total_lines >= MAX_LINES) return;
    int len = strlen(text);
    if (len > LINE_WIDTH) len = LINE_WIDTH;
    memcpy(lines[total_lines], text, len);
    lines[total_lines][len] = '\0';
    line_colors[total_lines] = color;
    line_link[total_lines] = link_idx;
    total_lines++;
}

/* --------------------------------------------------------------------------
 * word_wrap: Add text with word wrapping
 * -------------------------------------------------------------------------- */
static void word_wrap(const char* text, uint8_t color, int link_idx,
                      const char* prefix) {
    int plen = prefix ? strlen(prefix) : 0;
    int tlen = strlen(text);
    int pos = 0;
    bool first = true;

    while (pos < tlen) {
        char line[LINE_WIDTH + 1];
        int li = 0;

        /* Add prefix on first line */
        if (first && prefix) {
            for (int i = 0; i < plen && li < LINE_WIDTH; i++)
                line[li++] = prefix[i];
            first = false;
        } else if (prefix) {
            /* Indent continuation */
            for (int i = 0; i < plen && li < LINE_WIDTH; i++)
                line[li++] = ' ';
        }

        /* Fill line */
        int last_space = -1;
        while (pos < tlen && li < LINE_WIDTH) {
            if (text[pos] == ' ') last_space = li;
            line[li++] = text[pos++];
        }

        /* Word wrap at last space if we hit the limit */
        if (pos < tlen && last_space > plen) {
            /* Rewind to last space */
            pos -= (li - last_space - 1);
            li = last_space;
        }

        line[li] = '\0';
        add_line(line, color, link_idx);
    }
}

/* --------------------------------------------------------------------------
 * render_page: Convert parsed HTML elements to text lines
 * -------------------------------------------------------------------------- */
static void render_page(void) {
    total_lines = 0;
    total_links = 0;

    for (int i = 0; i < page.count && total_lines < MAX_LINES; i++) {
        html_element_t* el = &page.elements[i];
        if (!el->active) continue;

        switch (el->type) {
            case HTML_LINEBREAK:
                add_line("", VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK), -1);
                break;

            case HTML_HEADING: {
                uint8_t color;
                switch (el->level) {
                    case 1: color = VGA_COLOR(VGA_WHITE, VGA_BLACK); break;
                    case 2: color = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK); break;
                    case 3: color = VGA_COLOR(VGA_YELLOW, VGA_BLACK); break;
                    default: color = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK); break;
                }
                if (el->level <= 2)
                    add_line("", VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK), -1);
                word_wrap(el->text, color, -1, NULL);
                break;
            }

            case HTML_PARAGRAPH:
            case HTML_TEXT:
                word_wrap(el->text, VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK), -1, NULL);
                break;

            case HTML_BOLD:
                word_wrap(el->text, VGA_COLOR(VGA_WHITE, VGA_BLACK), -1, NULL);
                break;

            case HTML_ITALIC:
                word_wrap(el->text, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK), -1, NULL);
                break;

            case HTML_LINK: {
                int link_idx = -1;
                if (total_links < MAX_LINKS && el->href[0]) {
                    link_idx = total_links;
                    int hlen = strlen(el->href);
                    if (hlen >= HTML_MAX_HREF) hlen = HTML_MAX_HREF - 1;
                    memcpy(link_hrefs[total_links], el->href, hlen);
                    link_hrefs[total_links][hlen] = '\0';
                    link_line[total_links] = total_lines;
                    total_links++;
                }
                /* Render link with brackets */
                char buf[HTML_MAX_TEXT + 4];
                buf[0] = '[';
                int bl = 1;
                for (int j = 0; el->text[j] && bl < HTML_MAX_TEXT + 2; j++)
                    buf[bl++] = el->text[j];
                buf[bl++] = ']';
                buf[bl] = '\0';
                word_wrap(buf, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK), link_idx, NULL);
                break;
            }

            case HTML_LIST_ITEM:
                word_wrap(el->text, VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK), -1, " * ");
                break;

            case HTML_PREFORMATTED:
                word_wrap(el->text, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK), -1, "  ");
                break;
        }
    }
}

/* --------------------------------------------------------------------------
 * history_push: Add URL to history
 * -------------------------------------------------------------------------- */
static void history_push(const char* url) {
    if (history_count < BROWSER_HISTORY_SIZE) {
        strcpy(history[history_count], url);
        history_pos = history_count;
        history_count++;
    } else {
        for (int i = 0; i < BROWSER_HISTORY_SIZE - 1; i++)
            strcpy(history[i], history[i+1]);
        strcpy(history[BROWSER_HISTORY_SIZE - 1], url);
        history_pos = BROWSER_HISTORY_SIZE - 1;
    }
}

/* --------------------------------------------------------------------------
 * navigate: Fetch and render a URL
 * -------------------------------------------------------------------------- */
static bool navigate(const char* url) {
    strcpy(status_msg, "Loading...");
    browser_loading = true;

    /* Copy URL */
    int ul = strlen(url);
    if (ul >= BROWSER_URL_MAX) ul = BROWSER_URL_MAX - 1;
    memcpy(current_url, url, ul);
    current_url[ul] = '\0';

    /* Fetch */
    http_response_t resp = http_get(url);
    browser_loading = false;

    if (!resp.success) {
        strcpy(status_msg, "Failed to load page.");
        page.count = 0;
        total_lines = 0;
        total_links = 0;
        html_element_t* el = &page.elements[0];
        el->type = HTML_TEXT;
        strcpy(el->text, "Error: Could not fetch page.");
        el->active = true;
        page.count = 1;
        render_page();
        return false;
    }

    /* Parse */
    html_parse(resp.body, resp.body_len, &page);
    render_page();

    scroll_pos = 0;
    selected_link = total_links > 0 ? 0 : -1;
    status_msg[0] = '\0';

    history_push(url);
    return true;
}

/* --------------------------------------------------------------------------
 * resolve_href: Resolve relative URL against current URL
 * -------------------------------------------------------------------------- */
static void resolve_href(const char* href, char* out, int max) {
    if (href[0] == 'h' && href[1] == 't' && href[2] == 't' && href[3] == 'p') {
        /* Absolute URL */
        int len = strlen(href);
        if (len >= max) len = max - 1;
        memcpy(out, href, len);
        out[len] = '\0';
        return;
    }

    if (href[0] == '/') {
        /* Root-relative: extract scheme + host from current URL */
        int pos = 0;
        /* Find "://" */
        while (current_url[pos] && !(current_url[pos] == ':' && current_url[pos+1] == '/' && current_url[pos+2] == '/')) pos++;
        if (current_url[pos]) pos += 3;
        /* Find end of host */
        while (current_url[pos] && current_url[pos] != '/') pos++;
        /* Copy scheme+host */
        int oi = 0;
        for (int i = 0; i < pos && oi < max - 1; i++) out[oi++] = current_url[i];
        /* Append href */
        for (int i = 0; href[i] && oi < max - 1; i++) out[oi++] = href[i];
        out[oi] = '\0';
        return;
    }

    /* Relative — just append to current base (simplified) */
    int oi = 0;
    /* Copy current URL up to last / */
    int last_slash = 0;
    for (int i = 0; current_url[i]; i++) {
        if (current_url[i] == '/') last_slash = i;
    }
    for (int i = 0; i <= last_slash && oi < max - 1; i++) out[oi++] = current_url[i];
    for (int i = 0; href[i] && oi < max - 1; i++) out[oi++] = href[i];
    out[oi] = '\0';
}

/* --------------------------------------------------------------------------
 * browser_draw: Window draw callback — renders browser UI in GUI
 * -------------------------------------------------------------------------- */
static void browser_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0xF;

    /* --- URL bar (row 0) --- */
    uint8_t url_bg = VGA_COLOR(VGA_WHITE, VGA_BLUE);
    for (int i = 0; i < cw; i++) gui_putchar(cx + i, cy, ' ', url_bg);

    if (url_input_mode) {
        /* Show editable URL input */
        gui_draw_text(cx, cy, "URL: ", url_bg);
        int maxc = cw - 6;
        for (int i = 0; i < url_input_len && i < maxc; i++)
            gui_putchar(cx + 5 + i, cy, url_input_buf[i], url_bg);
        /* Cursor */
        if (url_input_len < maxc)
            gui_putchar(cx + 5 + url_input_len, cy, '_', VGA_COLOR(VGA_YELLOW, VGA_BLUE));
    } else {
        /* Show current URL */
        int ulen = strlen(current_url);
        if (ulen > cw - 1) {
            for (int i = 0; i < cw - 4; i++)
                gui_putchar(cx + 1 + i, cy, current_url[i], url_bg);
            gui_draw_text(cx + cw - 3, cy, "...", url_bg);
        } else {
            gui_putchar(cx, cy, ' ', url_bg);
            gui_draw_text(cx + 1, cy, current_url, url_bg);
        }
    }

    /* --- Content area (rows 1 to ch-2) --- */
    int content_rows = ch - 2;
    for (int row = 0; row < content_rows; row++) {
        int line_idx = scroll_pos + row;
        int sy = cy + 1 + row;

        if (line_idx < total_lines) {
            /* Determine color */
            uint8_t color = line_colors[line_idx];
            /* Remap to use window background */
            uint8_t fg_part = color & 0x0F;
            color = VGA_COLOR(fg_part, bg);

            /* Highlight selected link */
            if (line_link[line_idx] >= 0 && line_link[line_idx] == selected_link) {
                color = VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN);
            }

            /* Draw text */
            int j = 0;
            while (lines[line_idx][j] && j < cw) {
                gui_putchar(cx + j, sy, lines[line_idx][j], color);
                j++;
            }
        }
    }

    /* --- Status bar (last row) --- */
    uint8_t sb = VGA_COLOR(VGA_BLACK, VGA_LIGHT_GREY);
    for (int i = 0; i < cw; i++) gui_putchar(cx + i, cy + ch - 1, ' ', sb);

    if (browser_loading) {
        gui_draw_text(cx + 1, cy + ch - 1, "Loading...", sb);
    } else if (status_msg[0]) {
        gui_draw_text(cx + 1, cy + ch - 1, status_msg, sb);
    } else {
        gui_draw_text(cx + 1, cy + ch - 1, "Tab:links Enter:go Bksp:back G:url B:bkmk", sb);
    }

    /* Scroll indicator */
    if (total_lines > content_rows && total_lines > 0) {
        int pct = 0;
        int max_scroll = total_lines - content_rows;
        if (max_scroll > 0) pct = (scroll_pos * 100) / max_scroll;
        if (pct > 100) pct = 100;
        char n[8];
        int_to_str(pct, n);
        int nlen = strlen(n);
        gui_draw_text(cx + cw - nlen - 1, cy + ch - 1, n, sb);
        gui_putchar(cx + cw - 1, cy + ch - 1, '%', sb);
    }
}

/* --------------------------------------------------------------------------
 * browser_key: Window key callback — handles keyboard input
 * -------------------------------------------------------------------------- */
static void browser_key(int id, char key) {
    (void)id;

    /* URL input mode: capture keys for URL entry */
    if (url_input_mode) {
        if (key == '\n') {
            /* Submit URL */
            url_input_buf[url_input_len] = '\0';
            url_input_mode = false;
            if (url_input_len > 0) {
                navigate(url_input_buf);
            }
        } else if (key == 27) {
            /* Cancel URL input */
            url_input_mode = false;
        } else if (key == '\b') {
            if (url_input_len > 0) url_input_len--;
        } else if (key >= 32 && key < 127 && url_input_len < BROWSER_URL_MAX - 1) {
            url_input_buf[url_input_len++] = key;
        }
        return;
    }

    /* Approximate content area height (window height minus URL bar and status) */
    /* We use a rough estimate since we don't get cw/ch in key callback */
    int content_rows = 18; /* Default reasonable guess */

    switch (key) {
        /* Scroll up */
        case (char)0x80: /* Up arrow */
            if (scroll_pos > 0) scroll_pos--;
            break;

        /* Scroll down */
        case (char)0x81: /* Down arrow */
            if (scroll_pos < total_lines - content_rows) scroll_pos++;
            if (scroll_pos < 0) scroll_pos = 0;
            break;

        /* Page up */
        case (char)0x82:
            scroll_pos -= content_rows;
            if (scroll_pos < 0) scroll_pos = 0;
            break;

        /* Page down */
        case (char)0x83:
            scroll_pos += content_rows;
            if (scroll_pos > total_lines - content_rows)
                scroll_pos = total_lines - content_rows;
            if (scroll_pos < 0) scroll_pos = 0;
            break;

        /* Tab — next link */
        case '\t':
            if (total_links > 0) {
                selected_link = (selected_link + 1) % total_links;
                /* Scroll to link */
                int lline = link_line[selected_link];
                if (lline < scroll_pos) scroll_pos = lline;
                if (lline >= scroll_pos + content_rows)
                    scroll_pos = lline - content_rows / 2;
                if (scroll_pos < 0) scroll_pos = 0;
                /* Show link target in status */
                strcpy(status_msg, link_hrefs[selected_link]);
            }
            break;

        /* Enter — follow link */
        case '\n':
            if (selected_link >= 0 && selected_link < total_links) {
                char resolved[BROWSER_URL_MAX];
                resolve_href(link_hrefs[selected_link], resolved, BROWSER_URL_MAX);
                navigate(resolved);
            }
            break;

        /* Backspace — go back */
        case '\b':
            if (history_pos > 0) {
                history_pos--;
                navigate(history[history_pos]);
                /* Remove duplicate from history */
                if (history_count > 0) history_count--;
            }
            break;

        /* G — goto URL */
        case 'g':
        case 'G':
            url_input_mode = true;
            url_input_len = 0;
            url_input_buf[0] = '\0';
            strcpy(status_msg, "Enter URL and press Enter...");
            break;

        /* B — toggle bookmark */
        case 'b':
        case 'B':
            if (current_url[0]) {
                bool found = false;
                for (int i = 0; i < bookmark_count; i++) {
                    if (strcmp(bookmarks[i], current_url) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found && bookmark_count < BROWSER_BOOKMARK_SIZE) {
                    strcpy(bookmarks[bookmark_count++], current_url);
                    strcpy(status_msg, "Bookmarked!");
                } else if (found) {
                    strcpy(status_msg, "Already bookmarked.");
                } else {
                    strcpy(status_msg, "Bookmarks full.");
                }
            }
            break;

        /* R — refresh */
        case 'r':
        case 'R':
            navigate(current_url);
            break;

        default:
            break;
    }
}

/* --------------------------------------------------------------------------
 * browser_open: Open a browser window and navigate to URL
 * -------------------------------------------------------------------------- */
void browser_open(const char* url) {
    scroll_pos = 0;
    selected_link = -1;
    total_lines = 0;
    total_links = 0;
    status_msg[0] = '\0';
    url_input_mode = false;
    browser_loading = false;

    strcpy(current_url, url);

    /* Create a GUI window for the browser */
    browser_win_id = window_create("NexusBrowse", 5, 1, 75, 22,
                                    browser_draw, browser_key);

    /* Navigate to the URL */
    navigate(url);
}
