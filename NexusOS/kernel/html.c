/* ============================================================================
 * NexusOS — HTML Parser — Phase 29
 * ============================================================================
 * Parses HTML subset into flat element array for text rendering.
 * Handles: p, h1-h6, a, ul/ol/li, pre, br, b, i, title
 * Strips: script, style, comments, unknown tags
 * Decodes: &amp; &lt; &gt; &quot; &nbsp;
 * ============================================================================ */

#include "html.h"
#include "string.h"

/* --------------------------------------------------------------------------
 * Helper: case-insensitive compare for tag names
 * -------------------------------------------------------------------------- */
static bool tag_eq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

/* --------------------------------------------------------------------------
 * Helper: decode HTML entity at position, returns decoded char
 * -------------------------------------------------------------------------- */
static char decode_entity(const char* p, int* advance) {
    if (p[0] == '&') {
        if (p[1]=='a' && p[2]=='m' && p[3]=='p' && p[4]==';') { *advance = 5; return '&'; }
        if (p[1]=='l' && p[2]=='t' && p[3]==';') { *advance = 4; return '<'; }
        if (p[1]=='g' && p[2]=='t' && p[3]==';') { *advance = 4; return '>'; }
        if (p[1]=='q' && p[2]=='u' && p[3]=='o' && p[4]=='t' && p[5]==';') { *advance = 6; return '"'; }
        if (p[1]=='n' && p[2]=='b' && p[3]=='s' && p[4]=='p' && p[5]==';') { *advance = 6; return ' '; }
        if (p[1]=='#') {
            /* Numeric entity — just skip it */
            int i = 2;
            while (p[i] && p[i] != ';') i++;
            if (p[i] == ';') i++;
            *advance = i;
            return ' ';
        }
        /* Unknown entity — skip to ; */
        int i = 1;
        while (p[i] && p[i] != ';' && i < 10) i++;
        if (p[i] == ';') i++;
        *advance = i;
        return '?';
    }
    *advance = 1;
    return p[0];
}

/* --------------------------------------------------------------------------
 * Helper: add element to page
 * -------------------------------------------------------------------------- */
static html_element_t* add_element(html_page_t* page, html_type_t type) {
    if (page->count >= HTML_MAX_ELEMENTS) return NULL;
    html_element_t* el = &page->elements[page->count++];
    el->type = type;
    el->text[0] = '\0';
    el->href[0] = '\0';
    el->level = 0;
    el->active = true;
    return el;
}

/* --------------------------------------------------------------------------
 * Helper: extract attribute value from tag
 * Looks for attr="value" or attr='value'
 * -------------------------------------------------------------------------- */
static void extract_attr(const char* tag, int tag_len,
                         const char* attr, char* out, int max_out) {
    out[0] = '\0';
    int alen = strlen(attr);

    for (int i = 0; i < tag_len - alen; i++) {
        bool match = true;
        for (int j = 0; j < alen; j++) {
            char a = tag[i+j], b = attr[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != b) { match = false; break; }
        }
        if (!match) continue;

        /* Found attr name, skip to = */
        int p = i + alen;
        while (p < tag_len && tag[p] == ' ') p++;
        if (p >= tag_len || tag[p] != '=') continue;
        p++;
        while (p < tag_len && tag[p] == ' ') p++;

        char quote = 0;
        if (p < tag_len && (tag[p] == '"' || tag[p] == '\'')) {
            quote = tag[p++];
        }

        int oi = 0;
        while (p < tag_len && oi < max_out - 1) {
            if (quote && tag[p] == quote) break;
            if (!quote && (tag[p] == ' ' || tag[p] == '>')) break;
            out[oi++] = tag[p++];
        }
        out[oi] = '\0';
        return;
    }
}

/* --------------------------------------------------------------------------
 * html_parse: Parse HTML into elements
 * -------------------------------------------------------------------------- */
void html_parse(const char* html, int len, html_page_t* page) {
    memset(page, 0, sizeof(html_page_t));
    page->count = 0;
    page->title[0] = '\0';

    bool in_pre = false;
    bool in_script = false;
    bool in_style = false;
    bool in_title = false;
    bool in_link = false;
    char link_href[HTML_MAX_HREF];
    link_href[0] = '\0';

    /* Current text accumulator */
    char text_buf[HTML_MAX_TEXT];
    int text_len = 0;
    html_type_t cur_type = HTML_TEXT;
    uint8_t cur_level = 0;

    /* Flush accumulated text */
    #define FLUSH_TEXT() do { \
        if (text_len > 0) { \
            text_buf[text_len] = '\0'; \
            html_element_t* el; \
            if (in_link) { \
                el = add_element(page, HTML_LINK); \
                if (el) { memcpy(el->text, text_buf, text_len+1); \
                    memcpy(el->href, link_href, strlen(link_href)+1); } \
            } else { \
                el = add_element(page, cur_type); \
                if (el) { memcpy(el->text, text_buf, text_len+1); \
                    el->level = cur_level; } \
            } \
            text_len = 0; \
        } \
    } while(0)

    int i = 0;
    while (i < len) {
        /* HTML comment */
        if (i + 3 < len && html[i] == '<' && html[i+1] == '!' &&
            html[i+2] == '-' && html[i+3] == '-') {
            i += 4;
            while (i + 2 < len) {
                if (html[i] == '-' && html[i+1] == '-' && html[i+2] == '>') {
                    i += 3; break;
                }
                i++;
            }
            continue;
        }

        /* Tag */
        if (html[i] == '<') {
            int tag_start = i + 1;
            int tag_end = tag_start;
            while (tag_end < len && html[tag_end] != '>') tag_end++;
            if (tag_end >= len) { i++; continue; }

            int tag_len = tag_end - tag_start;
            const char* tag = html + tag_start;
            i = tag_end + 1;

            /* Check if closing tag */
            bool closing = false;
            const char* tname = tag;
            int tname_start = 0;
            if (tag[0] == '/') { closing = true; tname = tag + 1; tname_start = 1; }

            /* Get tag name (until space or >) */
            char name[16];
            int ni = 0;
            int tn = tname_start;
            while (tn < tag_len && tag[tn] != ' ' && tag[tn] != '>' && ni < 15) {
                char c = tag[tn];
                if (c >= 'A' && c <= 'Z') c += 32;
                name[ni++] = c;
                tn++;
            }
            name[ni] = '\0';

            /* Script/style skip */
            if (tag_eq(name, "script")) { in_script = !closing; continue; }
            if (tag_eq(name, "style")) { in_style = !closing; continue; }
            if (in_script || in_style) continue;

            /* Title */
            if (tag_eq(name, "title")) {
                in_title = !closing;
                if (closing) {
                    FLUSH_TEXT();
                    /* Copy last flushed text to title */
                    if (page->count > 0) {
                        memcpy(page->title, page->elements[page->count-1].text, HTML_MAX_TITLE-1);
                        page->title[HTML_MAX_TITLE-1] = '\0';
                        page->count--; /* Remove from elements */
                    }
                }
                continue;
            }

            /* Headings */
            if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
                FLUSH_TEXT();
                if (!closing) {
                    cur_type = HTML_HEADING;
                    cur_level = name[1] - '0';
                } else {
                    add_element(page, HTML_LINEBREAK);
                    cur_type = HTML_TEXT;
                    cur_level = 0;
                }
                continue;
            }

            /* Paragraph */
            if (tag_eq(name, "p")) {
                FLUSH_TEXT();
                if (!closing) {
                    cur_type = HTML_PARAGRAPH;
                } else {
                    add_element(page, HTML_LINEBREAK);
                    cur_type = HTML_TEXT;
                }
                continue;
            }

            /* Link */
            if (tag_eq(name, "a")) {
                FLUSH_TEXT();
                if (!closing) {
                    in_link = true;
                    extract_attr(tag, tag_len, "href", link_href, HTML_MAX_HREF);
                } else {
                    in_link = false;
                    link_href[0] = '\0';
                }
                continue;
            }

            /* List item */
            if (tag_eq(name, "li")) {
                FLUSH_TEXT();
                if (!closing) {
                    cur_type = HTML_LIST_ITEM;
                } else {
                    add_element(page, HTML_LINEBREAK);
                    cur_type = HTML_TEXT;
                }
                continue;
            }

            /* Preformatted */
            if (tag_eq(name, "pre")) {
                FLUSH_TEXT();
                in_pre = !closing;
                if (!closing) cur_type = HTML_PREFORMATTED;
                else { cur_type = HTML_TEXT; add_element(page, HTML_LINEBREAK); }
                continue;
            }

            /* Line break */
            if (tag_eq(name, "br")) {
                FLUSH_TEXT();
                add_element(page, HTML_LINEBREAK);
                continue;
            }

            /* Div/section/header/footer/main just break */
            if (tag_eq(name, "div") || tag_eq(name, "section") ||
                tag_eq(name, "header") || tag_eq(name, "footer") ||
                tag_eq(name, "main") || tag_eq(name, "nav") ||
                tag_eq(name, "article")) {
                FLUSH_TEXT();
                if (!closing) add_element(page, HTML_LINEBREAK);
                continue;
            }

            /* UL/OL just break */
            if (tag_eq(name, "ul") || tag_eq(name, "ol")) {
                FLUSH_TEXT();
                add_element(page, HTML_LINEBREAK);
                continue;
            }

            /* Bold/strong */
            if (tag_eq(name, "b") || tag_eq(name, "strong")) {
                FLUSH_TEXT();
                cur_type = closing ? HTML_TEXT : HTML_BOLD;
                continue;
            }

            /* Italic/em */
            if (tag_eq(name, "i") || tag_eq(name, "em")) {
                FLUSH_TEXT();
                cur_type = closing ? HTML_TEXT : HTML_ITALIC;
                continue;
            }

            /* All other tags: ignore */
            continue;
        }

        /* Skip script/style content */
        if (in_script || in_style) { i++; continue; }

        /* Regular text */
        char ch;
        if (html[i] == '&') {
            int adv;
            ch = decode_entity(html + i, &adv);
            i += adv;
        } else {
            ch = html[i++];
        }

        /* Collapse whitespace unless in <pre> */
        if (!in_pre) {
            if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            if (ch == ' ' && text_len > 0 && text_buf[text_len-1] == ' ') continue;
            if (ch == ' ' && text_len == 0) continue;
        }

        /* Add to text buffer */
        if (text_len < HTML_MAX_TEXT - 1) {
            text_buf[text_len++] = ch;
        } else {
            /* Buffer full — flush and start new element */
            FLUSH_TEXT();
            text_buf[text_len++] = ch;
        }
    }

    FLUSH_TEXT();
    #undef FLUSH_TEXT
}
