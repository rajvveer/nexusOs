/* ============================================================================
 * NexusOS — Web engine (Phase 57): HTML tokenizer, element tree, layout, paint
 * ============================================================================
 * Pipeline:  we_parse()  source -> normalised text + element tree
 *            we_layout() tree   -> display list (runs, images, rules, boxes)
 *            we_paint()  list   -> back buffer (one scrolled viewport)
 *
 * Supported: html/head/title/body, h1-h6, p, div, span, br, hr, a, b/strong,
 * i/em, u, code/tt/kbd, pre, ul/ol/li (nested), dl/dt/dd, blockquote, img
 * (src/alt/width/height), table/tr/td/th (equal columns, borders), center,
 * align=center, font color=, small, entities (named/numeric), comments,
 * script/style/noscript skipped, unclosed and mis-nested tags tolerated,
 * whitespace collapsing, UTF-8 folded to ASCII.
 *
 * Fonts: the UI has 14 px regular/bold, 12 px small and 22 px title faces. h1
 * and h3 are produced by resampling the title/bold faces (bilinear) so the six
 * heading levels still step down in size; italics are a 3 px shear of the
 * rendered run (the page background is a known solid colour).
 * ============================================================================ */

#include "webengine.h"
#include "macui.h"
#include "framebuffer.h"
#include "image.h"
#include "string.h"

/* --------------------------------------------------------------------------
 * Tags
 * -------------------------------------------------------------------------- */
enum {
    T_TEXT = 0, T_ROOT, T_HTML, T_HEAD, T_BODY, T_TITLE,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6,
    T_P, T_DIV, T_SPAN, T_BR, T_HR, T_A, T_B, T_I, T_U, T_CODE, T_PRE,
    T_UL, T_OL, T_LI, T_DL, T_DT, T_DD, T_BLOCKQUOTE, T_IMG,
    T_TABLE, T_TR, T_TD, T_TH, T_CENTER, T_FONT, T_SMALL,
    T_SCRIPT, T_STYLE, T_BLOCK /* generic block */, T_INLINE /* generic inline */,
    T_COUNT
};

static const struct { const char* name; uint8_t tag; } TAGS[] = {
    { "html", T_HTML }, { "head", T_HEAD }, { "body", T_BODY }, { "title", T_TITLE },
    { "h1", T_H1 }, { "h2", T_H2 }, { "h3", T_H3 }, { "h4", T_H4 }, { "h5", T_H5 }, { "h6", T_H6 },
    { "p", T_P }, { "div", T_DIV }, { "span", T_SPAN }, { "br", T_BR }, { "hr", T_HR },
    { "a", T_A }, { "b", T_B }, { "strong", T_B }, { "i", T_I }, { "em", T_I }, { "cite", T_I },
    { "u", T_U }, { "code", T_CODE }, { "tt", T_CODE }, { "kbd", T_CODE }, { "samp", T_CODE },
    { "pre", T_PRE }, { "ul", T_UL }, { "ol", T_OL }, { "li", T_LI },
    { "dl", T_DL }, { "dt", T_DT }, { "dd", T_DD }, { "blockquote", T_BLOCKQUOTE },
    { "img", T_IMG }, { "table", T_TABLE }, { "tr", T_TR }, { "td", T_TD }, { "th", T_TH },
    { "center", T_CENTER }, { "font", T_FONT }, { "small", T_SMALL }, { "sub", T_SMALL }, { "sup", T_SMALL },
    { "script", T_SCRIPT }, { "style", T_STYLE }, { "noscript", T_SCRIPT }, { "template", T_SCRIPT },
    { "textarea", T_SCRIPT }, { "svg", T_SCRIPT }, { "iframe", T_SCRIPT },
    { "section", T_BLOCK }, { "article", T_BLOCK }, { "header", T_BLOCK }, { "footer", T_BLOCK },
    { "nav", T_BLOCK }, { "main", T_BLOCK }, { "aside", T_BLOCK }, { "figure", T_BLOCK },
    { "figcaption", T_BLOCK }, { "form", T_BLOCK }, { "fieldset", T_BLOCK }, { "address", T_BLOCK },
    { "tbody", T_INLINE }, { "thead", T_INLINE }, { "tfoot", T_INLINE },
};
#define NTAGS (int)(sizeof(TAGS) / sizeof(TAGS[0]))

static bool is_block(int t) {
    return t == T_P || t == T_DIV || (t >= T_H1 && t <= T_H6) || t == T_HR || t == T_PRE ||
           t == T_UL || t == T_OL || t == T_LI || t == T_DL || t == T_DT || t == T_DD ||
           t == T_BLOCKQUOTE || t == T_TABLE || t == T_TR || t == T_TD || t == T_TH ||
           t == T_CENTER || t == T_BLOCK || t == T_BODY || t == T_HTML || t == T_ROOT;
}

/* --------------------------------------------------------------------------
 * Storage
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  tag;
    uint8_t  center;      /* align=center attribute                            */
    int16_t  slot;        /* image slot (T_IMG) or -1                          */
    int      parent, first, last, next;
    int      off, len;    /* T_TEXT: text in the page buffer                   */
    int      s1;          /* T_A: href strpool off / T_FONT: colour (0=none)   */
    int      w, h;        /* T_IMG: width/height attributes                    */
} we_node_t;

enum { IT_TEXT = 0, IT_IMG, IT_RULE, IT_BULLET, IT_BOX, IT_FILL };
#define F_ITALIC   0x01
#define F_UNDER    0x02
#define F_MONO     0x04
#define F_LINK     0x08
#define F_PRE      0x10

typedef struct {
    uint8_t  kind, font, scale, flags;   /* scale: 4 = 1.0x, 5 = 1.25x, ...  */
    int16_t  x, w, h;
    int      y;
    int      off;                        /* text offset (IT_TEXT)             */
    int16_t  len, link, aux;             /* aux: image slot / bullet number   */
    uint32_t color;
} we_item_t;

static we_node_t nodes[WE_MAX_NODES] __attribute__((section(".app_bss")));
static int       nnodes;
static we_item_t items[WE_MAX_ITEMS] __attribute__((section(".app_bss")));
static int       nitems;
static we_link_t links[WE_MAX_LINKS] __attribute__((section(".app_bss")));
static int       nlinks;
static we_image_t images[WE_MAX_IMAGES] __attribute__((section(".app_bss")));
static int       nimages;
static char      strpool[WE_STRPOOL] __attribute__((section(".app_bss")));
static int       strpool_len;
static char      title[WE_TITLE_MAX];
static int       title_len;
static char      base_url[WE_URL_MAX];
static char      textbuf[WE_PAGE_MAX + 8] __attribute__((section(".app_bss")));   /* normalised text (nodes point here) */
static char*     src_buf = textbuf;
static int       doc_h;
static bool      truncated;      /* node/item tables filled up               */

/* Shared image memory: display-size pixels of ready images grow from the
 * start; the remainder is the transient decode arena (see we_image_decoded). */
static uint32_t  img_region[WE_IMG_POOL_PX] __attribute__((section(".app_bss")));
static uint32_t  img_pool_used;

/* scratch for scaled-text rendering (unscaled copy of one run) */
#define SCR_W 1600
#define SCR_H 32
static uint32_t  scratch[SCR_W * SCR_H] __attribute__((section(".app_bss")));
static uint32_t  rowbuf[WE_IMG_MAX_DIM] __attribute__((section(".app_bss")));

/* --------------------------------------------------------------------------
 * Fonts / metrics
 * -------------------------------------------------------------------------- */
#define NFONTS 4                       /* UF_UI, UF_UIB, UF_SMALL, UF_TITLE */
static uint8_t adv_q4[NFONTS][96];
static bool    adv_ready = false;

static void metrics_init(void) {
    if (adv_ready) return;
    char s[5] = { 0, 0, 0, 0, 0 };
    for (int f = 0; f < NFONTS; f++)
        for (int c = 32; c < 127; c++) {
            s[0] = s[1] = s[2] = s[3] = (char)c;
            int w = ui_text_width(s, f);          /* (4*adv+3)>>2 == adv exactly */
            adv_q4[f][c - 32] = (uint8_t)(w > 255 ? 255 : w);
        }
    adv_ready = true;
}

/* advance of text[0..n) in quarter pixels (proportional fonts) */
static int text_q4(const char* t, int n, int font) {
    int q = 0;
    const uint8_t* a = adv_q4[font];
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        q += a[(c >= 32 && c < 127) ? c - 32 : '?' - 32];
    }
    return q;
}
static int mono_cols(const char* t, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) c += (t[i] == '\t') ? 4 : 1;
    return c;
}
static int font_line(int font, int scale) {
    int l = ui_font_height(font);
    return (l * scale + 3) / 4 + 1;
}

/* --------------------------------------------------------------------------
 * strpool
 * -------------------------------------------------------------------------- */
static int str_add(const char* s, int n) {
    if (n < 0) n = (int)strlen(s);
    if (strpool_len + n + 1 > WE_STRPOOL) return -1;
    int off = strpool_len;
    memcpy(strpool + off, s, n);
    strpool[off + n] = '\0';
    strpool_len += n + 1;
    return off;
}
const char* we_str(int off) { return (off >= 0 && off < strpool_len) ? strpool + off : ""; }

/* --------------------------------------------------------------------------
 * Tree building
 * -------------------------------------------------------------------------- */
static int node_new(int tag, int parent) {
    if (nnodes >= WE_MAX_NODES) { truncated = true; return -1; }
    int i = nnodes++;
    we_node_t* n = &nodes[i];
    n->tag = (uint8_t)tag; n->center = 0; n->slot = -1;
    n->parent = parent; n->first = n->last = n->next = -1;
    n->off = n->len = 0; n->s1 = -1; n->w = n->h = 0;
    if (parent >= 0) {
        we_node_t* p = &nodes[parent];
        if (p->last >= 0) nodes[p->last].next = i; else p->first = i;
        p->last = i;
    }
    return i;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int tag_lookup(const char* name) {
    for (int i = 0; i < NTAGS; i++) if (!strcmp(TAGS[i].name, name)) return TAGS[i].tag;
    /* unknown: a few more void/skip tags map to nothing meaningful */
    return T_INLINE;
}

/* Attribute scan inside a tag body [p, end). Finds `attr` and copies its
 * value into out (max bytes). Returns true if present (even if empty). */
static bool attr_get(const char* p, const char* end, const char* attr, char* out, int max) {
    int alen = (int)strlen(attr);
    out[0] = '\0';
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '/')) p++;
        if (p >= end) break;
        const char* ns = p;
        while (p < end && *p != '=' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '/' && *p != '>') p++;
        int nlen = (int)(p - ns);
        bool match = (nlen == alen);
        for (int i = 0; match && i < alen; i++) if (lc(ns[i]) != attr[i]) match = false;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p < end && *p == '=') {
            p++;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            char q = 0;
            if (p < end && (*p == '"' || *p == '\'')) q = *p++;
            const char* vs = p;
            if (q) { while (p < end && *p != q) p++; }
            else { while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '>') p++; }
            if (match) {
                int vl = (int)(p - vs);
                if (vl > max - 1) vl = max - 1;
                /* strip control chars / newlines inside the value */
                int o = 0;
                for (int i = 0; i < vl; i++) { char c = vs[i]; if ((unsigned char)c >= 32) out[o++] = c; }
                out[o] = '\0';
                return true;
            }
            if (q && p < end) p++;
        } else if (match) {
            return true;
        }
    }
    return false;
}

static int parse_int_attr(const char* v) {
    int n = 0; bool any = false;
    while (*v == ' ') v++;
    while (*v >= '0' && *v <= '9') { n = n * 10 + (*v++ - '0'); any = true; if (n > 20000) n = 20000; }
    return any ? n : 0;
}

static uint32_t parse_color(const char* v) {
    while (*v == ' ') v++;
    if (*v == '#') {
        v++;
        int n = (int)strlen(v);
        uint32_t c = 0;
        if (n >= 6) {
            for (int i = 0; i < 6; i++) {
                char ch = lc(v[i]); int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : 0;
                c = (c << 4) | (uint32_t)d;
            }
        } else if (n >= 3) {
            for (int i = 0; i < 3; i++) {
                char ch = lc(v[i]); int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : 0;
                c = (c << 8) | (uint32_t)(d * 17);
            }
        }
        return c | 0x01000000;        /* bit 24 = "set" (black stays distinguishable) */
    }
    static const struct { const char* n; uint32_t c; } NAMED[] = {
        { "red", 0xD00000 }, { "blue", 0x0040C0 }, { "green", 0x008000 }, { "black", 0x000000 },
        { "white", 0xFFFFFF }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "yellow", 0xC0A000 },
        { "orange", 0xE07000 }, { "purple", 0x800080 }, { "navy", 0x000080 }, { "teal", 0x008080 },
        { "maroon", 0x800000 }, { "silver", 0xA0A0A0 }, { "brown", 0x8B4513 }, { "pink", 0xE05080 },
    };
    char low[16]; int i = 0;
    while (v[i] && i < 15) { low[i] = lc(v[i]); i++; }
    low[i] = '\0';
    for (unsigned k = 0; k < sizeof(NAMED) / sizeof(NAMED[0]); k++)
        if (!strcmp(low, NAMED[k].n)) return NAMED[k].c | 0x01000000;
    return 0;
}

/* ---- entity / UTF-8 folding -------------------------------------------- */
static const struct { const char* name; uint16_t cp; } ENT[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 160 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "hellip", 0x2026 }, { "laquo", '<' }, { "raquo", '>' }, { "ldquo", '"' }, { "rdquo", '"' },
    { "lsquo", '\'' }, { "rsquo", '\'' }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "times", 'x' },
    { "deg", 0xB0 }, { "euro", 'E' }, { "pound", 'L' }, { "yen", 'Y' }, { "cent", 'c' }, { "sect", 'S' },
    { "para", 'P' }, { "shy", 0 }, { "zwj", 0 }, { "zwnj", 0 }, { "ensp", ' ' }, { "emsp", ' ' }, { "thinsp", ' ' },
    { "larr", '<' }, { "rarr", '>' }, { "uarr", '^' }, { "darr", 'v' }, { "harr", '-' }, { "minus", '-' },
    { "plusmn", '+' }, { "frac12", '%' }, { "iexcl", '!' }, { "iquest", '?' }, { "tilde", '~' },
    { "circ", '^' }, { "lowbar", '_' }, { "sol", '/' }, { "bsol", '\\' }, { "verbar", '|' }, { "ast", '*' },
    { "colon", ':' }, { "semi", ';' }, { "excl", '!' }, { "num", '#' }, { "dollar", '$' }, { "percnt", '%' },
    { "lpar", '(' }, { "rpar", ')' }, { "comma", ',' }, { "period", '.' }, { "equals", '=' }, { "quest", '?' },
    { "commat", '@' }, { "lsqb", '[' }, { "rsqb", ']' }, { "lcub", '{' }, { "rcub", '}' }, { "grave", '`' },
};
#define NENT (int)(sizeof(ENT) / sizeof(ENT[0]))

/* Fold a code point to ASCII into out (<= 3 chars); returns length. */
static int fold_cp(uint32_t cp, char* out) {
    if (cp < 32) { if (cp == '\n' || cp == '\t') { out[0] = (char)cp; return 1; } return 0; }
    if (cp < 127) { out[0] = (char)cp; return 1; }
    switch (cp) {
    case 160: case 0x2002: case 0x2003: case 0x2009: case 0x200A: out[0] = ' '; return 1;
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return 0;
    case 0xA9: out[0] = '('; out[1] = 'c'; out[2] = ')'; return 3;
    case 0xAE: out[0] = '('; out[1] = 'R'; out[2] = ')'; return 3;
    case 0x2122: out[0] = 'T'; out[1] = 'M'; return 2;
    case 0x2013: case 0x2014: case 0x2212: case 0x2010: case 0x2011: out[0] = '-'; return 1;
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0xB4: out[0] = '\''; return 1;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0xAB: case 0xBB: out[0] = '"'; return 1;
    case 0x2026: out[0] = out[1] = out[2] = '.'; return 3;
    case 0x2022: case 0xB7: case 0x25CF: case 0x2023: out[0] = '*'; return 1;
    case 0xD7: out[0] = 'x'; return 1;
    case 0xB0: out[0] = 'o'; return 1;
    case 0x2192: case 0x21D2: case 0x27A1: out[0] = '-'; out[1] = '>'; return 2;
    case 0x2190: out[0] = '<'; out[1] = '-'; return 2;
    case 0x2713: case 0x2714: out[0] = 'v'; return 1;
    case 0xE9: case 0xE8: case 0xEA: case 0xEB: out[0] = 'e'; return 1;
    case 0xC9: case 0xC8: out[0] = 'E'; return 1;
    case 0xE0: case 0xE1: case 0xE2: case 0xE4: case 0xE5: out[0] = 'a'; return 1;
    case 0xC0: case 0xC1: case 0xC4: out[0] = 'A'; return 1;
    case 0xF6: case 0xF2: case 0xF3: case 0xF4: out[0] = 'o'; return 1;
    case 0xD6: case 0xD3: out[0] = 'O'; return 1;
    case 0xFC: case 0xF9: case 0xFA: out[0] = 'u'; return 1;
    case 0xDC: out[0] = 'U'; return 1;
    case 0xED: case 0xEC: case 0xEE: out[0] = 'i'; return 1;
    case 0xF1: out[0] = 'n'; return 1;
    case 0xE7: out[0] = 'c'; return 1;
    case 0xDF: out[0] = 's'; out[1] = 's'; return 2;
    default: out[0] = '?'; return 1;
    }
}

/* Decode an entity starting at s[0]=='&' (bounded by end). Returns the number
 * of source bytes consumed (>=1) and the folded text in out/outn. */
static int decode_entity(const char* s, const char* end, char* out, int* outn) {
    const char* p = s + 1;
    *outn = 0;
    if (p < end && *p == '#') {
        p++;
        uint32_t cp = 0; bool hex = false, any = false;
        if (p < end && (*p == 'x' || *p == 'X')) { hex = true; p++; }
        while (p < end) {
            char c = lc(*p);
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else break;
            cp = cp * (hex ? 16 : 10) + (uint32_t)d; any = true; p++;
            if (cp > 0x10FFFF) cp = '?';
        }
        if (!any) { out[0] = '&'; *outn = 1; return 1; }
        if (p < end && *p == ';') p++;
        *outn = fold_cp(cp, out);
        return (int)(p - s);
    }
    char name[12]; int nl = 0;
    while (p < end && nl < 11 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) name[nl++] = *p++;
    name[nl] = '\0';
    if (nl == 0) { out[0] = '&'; *outn = 1; return 1; }
    for (int i = 0; i < NENT; i++) {
        if (!strcmp(name, ENT[i].name)) {
            if (p < end && *p == ';') p++;
            *outn = fold_cp(ENT[i].cp, out);
            return (int)(p - s);
        }
    }
    out[0] = '&'; *outn = 1;              /* unknown: leave the ampersand */
    return 1;
}

/* Decode one UTF-8 sequence at s (bounded). Returns bytes consumed. */
static int decode_utf8(const char* s, const char* end, char* out, int* outn) {
    unsigned char c = (unsigned char)s[0];
    int need = 0; uint32_t cp = 0;
    if (c >= 0xF0) { need = 3; cp = c & 7; }
    else if (c >= 0xE0) { need = 2; cp = c & 15; }
    else if (c >= 0xC0) { need = 1; cp = c & 31; }
    else { out[0] = '?'; *outn = 1; return 1; }
    int n = 1;
    while (need-- > 0) {
        if (s + n >= end || ((unsigned char)s[n] & 0xC0) != 0x80) { out[0] = '?'; *outn = 1; return n; }
        cp = (cp << 6) | ((unsigned char)s[n] & 0x3F); n++;
    }
    *outn = fold_cp(cp, out);
    return n;
}

/* ---- parser state ------------------------------------------------------- */
#define STACK_MAX 96
static int  stack[STACK_MAX];
static int  sp;

static int stack_find(int tag) {
    for (int i = sp - 1; i >= 0; i--) if (nodes[stack[i]].tag == tag) return i;
    return -1;
}
static int  cur_parent(void) { return sp ? stack[sp - 1] : 0; }
static void pop_to(int level) { if (level < sp) sp = level; if (sp < 1) sp = 1; }
/* close the innermost open `tag` if any (and everything above it) */
static void close_tag(int tag) { int i = stack_find(tag); if (i > 0) pop_to(i); }
/* close `tag` only if no `stop` tag is open above it */
static void close_tag_until(int tag, int stop1, int stop2) {
    for (int i = sp - 1; i > 0; i--) {
        int t = nodes[stack[i]].tag;
        if (t == stop1 || t == stop2) return;
        if (t == tag) { pop_to(i); return; }
    }
}

static int body_node;   /* the <body> element (created on demand) */
static int head_node;
static bool in_title;

static int ensure_body(void) {
    if (body_node < 0) {
        /* body goes under html */
        int html = -1;
        for (int i = 0; i < nnodes; i++) if (nodes[i].tag == T_HTML) { html = i; break; }
        if (html < 0) html = node_new(T_HTML, 0);
        body_node = node_new(T_BODY, html < 0 ? 0 : html);
        if (body_node < 0) body_node = 0;
        sp = 1; stack[sp++] = body_node;
    }
    return body_node;
}

/* Append normalised text [off, off+len) as a text node under the current
 * parent; title text is routed to the title buffer. */
static void add_text(int off, int len) {
    if (len <= 0) return;
    if (in_title) {
        for (int i = 0; i < len && title_len < WE_TITLE_MAX - 1; i++) {
            char c = src_buf[off + i];
            if (c == '\n' || c == '\t') c = ' ';
            if (c == ' ' && (title_len == 0 || title[title_len - 1] == ' ')) continue;
            title[title_len++] = c;
        }
        title[title_len] = '\0';
        return;
    }
    /* whitespace-only text outside the body (e.g. between head tags) is noise */
    if (body_node < 0 || sp <= 1) {
        bool ws = true;
        for (int i = 0; i < len; i++) if (src_buf[off + i] != ' ' && src_buf[off + i] != '\n') { ws = false; break; }
        if (ws) return;
        ensure_body();
    }
    int n = node_new(T_TEXT, cur_parent());
    if (n < 0) return;
    nodes[n].off = off; nodes[n].len = len;
}

static void we_reset(const char* base) {
    metrics_init();
    nnodes = 0; nitems = 0; nlinks = 0; nimages = 0; strpool_len = 0;
    title_len = 0; title[0] = '\0'; doc_h = 0; truncated = false;
    img_pool_used = 0;
    for (int i = 0; i < WE_MAX_IMAGES; i++) images[i].px = NULL;
    src_buf = textbuf;
    strncpy(base_url, base ? base : "", WE_URL_MAX - 1); base_url[WE_URL_MAX - 1] = '\0';
    node_new(T_ROOT, -1);
    sp = 0; stack[sp++] = 0;
    body_node = -1; head_node = -1; in_title = false;
}

void we_parse(const char* src, int len, const char* base) {
    we_reset(base);
    const char* end = src + len;
    int i = 0;            /* read index in src                  */
    int o = 0;            /* write index in textbuf             */
    int text_start = -1;  /* start (write index) of the pending text run   */
    bool in_pre = false;
    bool last_space = true;   /* collapse: previous emitted char was a space */

    #define FLUSH_TEXT() do { if (text_start >= 0) { add_text(text_start, o - text_start); text_start = -1; } } while (0)

    while (i < len) {
        char c = src[i];
        if (c == '<') {
            /* comment / doctype / processing instruction */
            if (i + 3 < len && src[i+1] == '!' && src[i+2] == '-' && src[i+3] == '-') {
                int j = i + 4;
                while (j + 2 < len && !(src[j] == '-' && src[j+1] == '-' && src[j+2] == '>')) j++;
                i = (j + 2 < len) ? j + 3 : len;
                continue;
            }
            if (i + 1 < len && (src[i+1] == '!' || src[i+1] == '?')) {
                int j = i + 2;
                while (j < len && src[j] != '>') j++;
                i = (j < len) ? j + 1 : len;
                continue;
            }
            /* a tag needs a letter or '/' next, otherwise it is text */
            if (!(i + 1 < len && (src[i+1] == '/' || (lc(src[i+1]) >= 'a' && lc(src[i+1]) <= 'z')))) {
                goto text_char;
            }
            int j = i + 1;
            bool closing = false;
            if (src[j] == '/') { closing = true; j++; }
            char name[16]; int nl = 0;
            while (j < len && nl < 15 && src[j] != '>' && src[j] != ' ' && src[j] != '\t' && src[j] != '\n' && src[j] != '\r' && src[j] != '/') name[nl++] = lc(src[j++]);
            name[nl] = '\0';
            /* find the end of the tag, honouring quotes */
            int k = j; char q = 0;
            while (k < len) {
                if (q) { if (src[k] == q) q = 0; }
                else if (src[k] == '"' || src[k] == '\'') q = src[k];
                else if (src[k] == '>') break;
                k++;
            }
            if (k >= len) { i = len; break; }        /* unterminated tag: drop the tail */
            const char* abeg = src + j;
            const char* aend = src + k;
            int tag_end = k + 1;

            int tag = tag_lookup(name);
            if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) tag = T_H1 + (name[1] - '1');

            FLUSH_TEXT();
            if (in_title && tag != T_TITLE && (closing || tag != T_INLINE)) in_title = false;   /* unclosed <title> */

            if (tag == T_SCRIPT || tag == T_STYLE) {
                if (!closing) {
                    /* skip to the matching close tag */
                    int m = tag_end;
                    while (m < len) {
                        if (src[m] == '<' && m + 1 < len && src[m+1] == '/') {
                            int t = m + 2, nn = 0; bool same = true;
                            while (nn < nl && t < len) { if (lc(src[t]) != name[nn]) { same = false; break; } t++; nn++; }
                            if (same && nn == nl) { while (t < len && src[t] != '>') t++; m = t + 1; break; }
                        }
                        m++;
                    }
                    tag_end = m < len ? m : len;
                }
                i = tag_end;
                continue;
            }

            if (closing) {
                switch (tag) {
                case T_TITLE: in_title = false; break;
                case T_HTML: case T_BODY: break;             /* keep the body open */
                case T_HEAD: head_node = -1; break;
                case T_P: close_tag_until(T_P, T_TABLE, T_TABLE); break;
                case T_LI: close_tag_until(T_LI, T_UL, T_OL); break;
                case T_TD: case T_TH: close_tag_until(tag, T_TR, T_TABLE); break;
                case T_TR: close_tag_until(T_TR, T_TABLE, T_TABLE); break;
                case T_INLINE: break;
                default: close_tag(tag); break;
                }
                if (tag == T_PRE) in_pre = (stack_find(T_PRE) > 0);
                i = tag_end;
                continue;
            }

            /* ---- start tag ---- */
            if (tag == T_HTML) { if (nnodes == 1) node_new(T_HTML, 0); i = tag_end; continue; }
            if (tag == T_HEAD) { i = tag_end; continue; }
            if (tag == T_TITLE) { in_title = (title_len == 0); i = tag_end; continue; }
            if (tag == T_BODY) {
                ensure_body();
                char v[16];
                if (attr_get(abeg, aend, "bgcolor", v, sizeof(v))) { /* ignored */ }
                i = tag_end; continue;
            }
            if (tag == T_INLINE) {
                /* unknown or transparent tag: keep structure flat */
                if (!strcmp(name, "input") || !strcmp(name, "button") || !strcmp(name, "select") || !strcmp(name, "option") ||
                    !strcmp(name, "meta") || !strcmp(name, "link") || !strcmp(name, "base") || !strcmp(name, "source") ||
                    !strcmp(name, "wbr") || !strcmp(name, "canvas") || !strcmp(name, "video") || !strcmp(name, "audio")) {
                    if (!strcmp(name, "base")) {
                        char v[WE_URL_MAX];
                        if (attr_get(abeg, aend, "href", v, sizeof(v)) && v[0]) { strncpy(base_url, v, WE_URL_MAX - 1); }
                    }
                    i = tag_end; continue;
                }
                i = tag_end; continue;
            }
            if (body_node < 0) ensure_body();

            /* implicit closes */
            if (tag == T_P || is_block(tag)) { if (tag != T_TD && tag != T_TH && tag != T_TR) close_tag_until(T_P, T_TABLE, T_TD); }
            if (tag >= T_H1 && tag <= T_H6) for (int t = T_H1; t <= T_H6; t++) close_tag(t);
            if (tag == T_LI) close_tag_until(T_LI, T_UL, T_OL);
            if (tag == T_DT || tag == T_DD) { close_tag_until(T_DT, T_DL, T_DL); close_tag_until(T_DD, T_DL, T_DL); }
            if (tag == T_TR) { close_tag_until(T_TD, T_TABLE, T_TABLE); close_tag_until(T_TH, T_TABLE, T_TABLE); close_tag_until(T_TR, T_TABLE, T_TABLE); }
            if (tag == T_TD || tag == T_TH) { close_tag_until(T_TD, T_TR, T_TABLE); close_tag_until(T_TH, T_TR, T_TABLE); if (stack_find(T_TR) < 0 && stack_find(T_TABLE) > 0) { int tr = node_new(T_TR, cur_parent()); if (tr >= 0 && sp < STACK_MAX) stack[sp++] = tr; } }
            if (tag == T_TABLE && sp >= STACK_MAX - 4) { i = tag_end; continue; }

            int n = node_new(tag, cur_parent());
            if (n < 0) { i = tag_end; continue; }
            char v[WE_URL_MAX];
            if (tag == T_A) {
                if (attr_get(abeg, aend, "href", v, sizeof(v)) && v[0]) nodes[n].s1 = str_add(v, -1);
            } else if (tag == T_IMG) {
                if (nimages < WE_MAX_IMAGES && attr_get(abeg, aend, "src", v, sizeof(v)) && v[0]) {
                    int s = str_add(v, -1);
                    if (s >= 0) {
                        we_image_t* im = &images[nimages];
                        memset(im, 0, sizeof(*im));
                        im->src = s; im->alt = -1; im->state = WE_IMG_NEW;
                        if (attr_get(abeg, aend, "alt", v, 160)) im->alt = str_add(v, -1);
                        char d[16];
                        if (attr_get(abeg, aend, "width", d, sizeof(d))) im->attr_w = parse_int_attr(d);
                        if (attr_get(abeg, aend, "height", d, sizeof(d))) im->attr_h = parse_int_attr(d);
                        we_resolve_href(s, im->url);
                        nodes[n].slot = (int16_t)nimages;
                        nodes[n].w = im->attr_w; nodes[n].h = im->attr_h;
                        nimages++;
                    }
                }
            } else if (tag == T_FONT) {
                if (attr_get(abeg, aend, "color", v, 32)) nodes[n].s1 = (int)parse_color(v);
                else nodes[n].s1 = 0;
            }
            if (tag == T_P || tag == T_DIV || tag == T_TD || tag == T_TH || tag == T_TR || tag == T_TABLE || (tag >= T_H1 && tag <= T_H6)) {
                if (attr_get(abeg, aend, "align", v, 16)) {
                    char lw[16]; int z = 0; while (v[z] && z < 15) { lw[z] = lc(v[z]); z++; } lw[z] = 0;
                    if (!strcmp(lw, "center")) nodes[n].center = 1;
                }
            }
            if (tag == T_TH) nodes[n].center = 1;
            /* void elements are not pushed */
            bool is_void = (tag == T_BR || tag == T_HR || tag == T_IMG);
            if (!is_void && !(k > j && src[k-1] == '/' && tag != T_A && tag != T_DIV)) {
                if (sp < STACK_MAX) stack[sp++] = n;
            }
            if (tag == T_PRE) { in_pre = true; last_space = true; }
            else if (is_block(tag) || tag == T_BR) last_space = true;
            i = tag_end;
            continue;
        }

text_char:
        {
            /* text: decode and normalise into textbuf at o */
            char out[4]; int outn = 0; int adv;
            if (c == '&') adv = decode_entity(src + i, end, out, &outn);
            else if ((unsigned char)c >= 0x80) adv = decode_utf8(src + i, end, out, &outn);
            else { out[0] = c; outn = 1; adv = 1; }
            for (int t = 0; t < outn && o < WE_PAGE_MAX; t++) {
                char ch = out[t];
                if (!in_pre) {
                    if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\f' || ch == '\v') ch = ' ';
                    if (ch == ' ') { if (last_space) continue; last_space = true; }
                    else last_space = false;
                } else {
                    if (ch == '\r') continue;
                }
                if ((unsigned char)ch < 32 && ch != '\n' && ch != '\t') continue;
                if (text_start < 0) text_start = o;
                textbuf[o++] = ch;
            }
            i += adv;
        }
    }
    FLUSH_TEXT();
    #undef FLUSH_TEXT
    if (body_node < 0) ensure_body();
    if (truncated) {
        /* leave a marker the layout can show */
    }
    (void)head_node;
}

void we_parse_plain(const char* src, int len, const char* base) {
    we_reset(base);
    ensure_body();
    int pre = node_new(T_PRE, body_node);
    if (pre < 0) return;
    /* fold UTF-8 / strip CR into textbuf */
    int o = 0;
    const char* end = src + len;
    for (int i = 0; i < len && o < WE_PAGE_MAX - 4;) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r') { i++; continue; }
        if (c >= 0x80) {
            char out[4]; int outn = 0;
            int adv = decode_utf8(src + i, end, out, &outn);
            for (int t = 0; t < outn; t++) textbuf[o++] = out[t];
            i += adv;
            continue;
        }
        if (c < 32 && c != '\n' && c != '\t') { i++; continue; }
        textbuf[o++] = (char)c; i++;
    }
    int n = node_new(T_TEXT, pre);
    if (n >= 0) { nodes[n].off = 0; nodes[n].len = o; }
}

/* --------------------------------------------------------------------------
 * URL resolution
 * -------------------------------------------------------------------------- */
static bool has_scheme(const char* s) {
    int i = 0;
    while (s[i] && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (i && (s[i] == '+' || s[i] == '-' || s[i] == '.' || (s[i] >= '0' && s[i] <= '9'))))) i++;
    return i > 1 && s[i] == ':';
}

void we_resolve_href(int off, char* out) {
    const char* h = we_str(off);
    out[0] = '\0';
    while (*h == ' ') h++;
    if (has_scheme(h)) { strncpy(out, h, WE_URL_MAX - 1); out[WE_URL_MAX - 1] = '\0'; return; }
    const char* b = base_url;
    const char* ss = strstr(b, "://");
    if (!ss) { strncpy(out, h, WE_URL_MAX - 1); out[WE_URL_MAX - 1] = '\0'; return; }
    const char* host = ss + 3;
    const char* hostend = host;
    while (*hostend && *hostend != '/' && *hostend != '?' && *hostend != '#') hostend++;
    int oi = 0;
    if (h[0] == '/' && h[1] == '/') {                 /* scheme-relative */
        for (const char* q = b; q < host && oi < WE_URL_MAX - 1; q++) out[oi++] = *q;
        h += 2;
        while (*h && oi < WE_URL_MAX - 1) out[oi++] = *h++;
        out[oi] = '\0'; return;
    }
    for (const char* q = b; q < hostend && oi < WE_URL_MAX - 1; q++) out[oi++] = *q;
    if (h[0] == '/') {
        while (*h && oi < WE_URL_MAX - 1) out[oi++] = *h++;
        out[oi] = '\0'; return;
    }
    if (h[0] == '?' || h[0] == '#') {
        /* keep the base path */
        const char* pe = hostend;
        while (*pe && *pe != '?' && *pe != '#') pe++;
        if (h[0] == '#') { while (*pe && *pe != '#') pe++; }
        for (const char* q = hostend; q < pe && oi < WE_URL_MAX - 1; q++) out[oi++] = *q;
        if (oi == (int)(hostend - b) && oi < WE_URL_MAX - 1) out[oi++] = '/';
        while (*h && oi < WE_URL_MAX - 1) out[oi++] = *h++;
        out[oi] = '\0'; return;
    }
    /* relative path: base directory + href, then collapse ./ and ../ */
    char path[WE_URL_MAX]; int pi = 0;
    const char* last = NULL;
    for (const char* q = hostend; *q && *q != '?' && *q != '#'; q++) if (*q == '/') last = q;
    if (last) { for (const char* q = hostend; q <= last && pi < WE_URL_MAX - 1; q++) path[pi++] = *q; }
    else path[pi++] = '/';
    while (*h && pi < WE_URL_MAX - 1) path[pi++] = *h++;
    path[pi] = '\0';
    /* collapse segments */
    char norm[WE_URL_MAX]; int ni = 0;
    int p = 0;
    while (path[p]) {
        /* path[p] == '/' at a segment start */
        if (path[p] == '/' && path[p+1] == '.' && (path[p+2] == '/' || path[p+2] == '\0')) { p += 2; if (!path[p]) { if (ni < WE_URL_MAX - 1) norm[ni++] = '/'; } continue; }
        if (path[p] == '/' && path[p+1] == '.' && path[p+2] == '.' && (path[p+3] == '/' || path[p+3] == '\0' || path[p+3] == '?' || path[p+3] == '#')) {
            while (ni > 0 && norm[ni-1] != '/') ni--;
            if (ni > 0) ni--;                      /* drop the previous segment's slash */
            p += 3;
            if (!path[p] || path[p] == '?' || path[p] == '#') { if (ni < WE_URL_MAX - 1) norm[ni++] = '/'; }
            continue;
        }
        if (ni < WE_URL_MAX - 1) norm[ni++] = path[p];
        p++;
    }
    norm[ni] = '\0';
    if (ni == 0) { norm[0] = '/'; norm[1] = '\0'; }
    for (int z = 0; norm[z] && oi < WE_URL_MAX - 1; z++) out[oi++] = norm[z];
    out[oi] = '\0';
}

/* --------------------------------------------------------------------------
 * Layout
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  font, scale, flags;
    uint8_t  center;
    int      link;
    uint32_t color;
} style_t;

#define C_TEXT   0x1D1D1F
#define C_LINK   0x0A5BD6
#define C_MUTED  0x6E6E73
#define C_CODEBG 0xF2F2F4
#define C_RULE   0xD2D2D7
#define C_BORDER 0xC8C8CE
#define C_THBG   0xF0F0F3
#define C_QUOTE  0xD0D0D8
#define C_PAGE   0xFFFFFF

static int cur_y, x0, x1;          /* block cursor and inline extent         */
static int pen_q;                  /* inline pen in quarter px               */
static int line_top, line_h, line_first;
static bool line_content;
static bool pending_space;
static int pending_margin;
static uint8_t line_center;
static int list_depth;
static int list_counter[20];
static bool list_ordered[20];
static int content_w;

static we_item_t* item_new(int kind) {
    if (nitems >= WE_MAX_ITEMS) { truncated = true; return NULL; }
    we_item_t* it = &items[nitems++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind; it->link = -1; it->scale = 4;
    return it;
}

static void line_begin(void) {
    if (pending_margin) { cur_y += pending_margin; pending_margin = 0; }
    line_top = cur_y; line_h = 0; line_first = nitems; line_content = false;
    pen_q = x0 << 2; pending_space = false;
}

/* finish the current line: vertical alignment, centring, advance cur_y */
static void line_flush(void) {
    if (!line_content) return;
    int pen_px = (pen_q + 3) >> 2;
    int shift = 0;
    if (line_center && pen_px < x1) shift = (x1 - pen_px) / 2;
    for (int i = line_first; i < nitems; i++) {
        we_item_t* it = &items[i];
        if (it->kind == IT_TEXT || it->kind == IT_IMG || it->kind == IT_BULLET) {
            if (it->h < line_h) it->y += line_h - it->h;          /* bottom-align ~ baseline */
            it->x = (int16_t)(it->x + shift);
        }
    }
    cur_y = line_top + line_h;
    line_content = false;
    line_begin();
}

static void block_gap(int m) { line_flush(); if (pending_margin < m) pending_margin = m; }

static void place_text(int off, int len, int w_q4, int h, const style_t* st) {
    /* try to extend the previous run (same style, contiguous text, same line) */
    if (nitems > 0) {
        we_item_t* p = &items[nitems - 1];
        if (p->kind == IT_TEXT && p->off + p->len <= off && off - (p->off + p->len) <= 1 &&
            p->font == st->font && p->scale == st->scale && p->flags == st->flags && p->link == st->link &&
            p->color == st->color && p->y == line_top && nitems - 1 >= line_first && off + len - p->off < 30000) {
            /* include the gap (a single space) */
            int gap = off - (p->off + p->len);
            if (gap == 1 && src_buf[p->off + p->len] != ' ') gap = -1;
            if (gap >= 0) {
                p->len = (int16_t)(off + len - p->off);
                int total_q = (st->flags & F_MONO) ? mono_cols(src_buf + p->off, p->len) * 32 * st->scale / 4
                                                   : text_q4(src_buf + p->off, p->len, st->font) * st->scale / 4;
                p->w = (int16_t)((total_q + 3) >> 2);
                pen_q = (p->x << 2) + total_q;
                if (h > line_h) line_h = h;
                line_content = true;
                return;
            }
        }
    }
    we_item_t* it = item_new(IT_TEXT);
    if (!it) return;
    int x = (pen_q + 3) >> 2;
    pen_q = x << 2;
    it->x = (int16_t)x; it->y = line_top; it->w = (int16_t)((w_q4 + 3) >> 2); it->h = (int16_t)h;
    it->off = off; it->len = (int16_t)len; it->font = st->font; it->scale = st->scale; it->flags = st->flags;
    it->link = (int16_t)st->link; it->color = st->color;
    pen_q += w_q4;
    if (h > line_h) line_h = h;
    line_content = true;
}

/* width in q4 of text (style-aware) */
static int style_q4(const char* t, int n, const style_t* st) {
    if (st->flags & F_MONO) return mono_cols(t, n) * 32 * st->scale / 4;
    return text_q4(t, n, st->font) * st->scale / 4;
}
static int style_h(const style_t* st) {
    if (st->flags & F_MONO) return 18 * st->scale / 4;
    return font_line(st->font, st->scale);
}

static void inline_text(int off, int len, const style_t* st) {
    const char* t = src_buf + off;
    int h = style_h(st);
    int space_q = style_q4(" ", 1, st);
    int i = 0;
    if (st->flags & F_PRE) {
        /* preformatted: split on newlines, no wrapping */
        while (i < len) {
            int s = i;
            while (i < len && t[i] != '\n') i++;
            if (i > s) {
                if (!line_content) { if (pending_margin) { cur_y += pending_margin; pending_margin = 0; } line_top = cur_y; }
                place_text(off + s, i - s, style_q4(t + s, i - s, st), h, st);
            }
            if (i < len && t[i] == '\n') {
                if (!line_content) { line_top = cur_y; line_h = h; line_content = true; line_first = nitems; }
                line_flush();
                i++;
            }
        }
        return;
    }
    while (i < len) {
        if (t[i] == ' ') { pending_space = true; i++; continue; }
        int s = i;
        while (i < len && t[i] != ' ') i++;
        int wl = i - s;
        int wq = style_q4(t + s, wl, st);
        int need = (line_content && pending_space ? space_q : 0) + wq;
        if (line_content && ((pen_q + need + 3) >> 2) > x1) line_flush();
        if (!line_content && pending_margin) { cur_y += pending_margin; pending_margin = 0; line_top = cur_y; }
        /* advance over the inter-word space (a merged run recomputes the pen
         * from its own start, which already includes that space) */
        if (line_content && pending_space) pen_q += space_q;
        pending_space = false;
        if (((pen_q + wq + 3) >> 2) > x1 && !line_content) {
            /* a single word wider than the line: break it by characters */
            int avail_q = (x1 - ((pen_q + 3) >> 2)) << 2;
            int k = 0, used = 0;
            while (k < wl) {
                int cq = style_q4(t + s + k, 1, st);
                if (used + cq > avail_q && k > 0) {
                    place_text(off + s, k, used, h, st);
                    line_flush();
                    s += k; wl -= k; k = 0; used = 0;
                    avail_q = (x1 - x0) << 2;
                    continue;
                }
                used += cq; k++;
            }
            if (wl > 0) place_text(off + s, wl, used, h, st);
            continue;
        }
        place_text(off + s, wl, wq, h, st);
    }
}

static void layout_node(int n, style_t st);

static void layout_children(int n, const style_t* st) {
    for (int c = nodes[n].first; c >= 0; c = nodes[c].next) layout_node(c, *st);
}

static void layout_table(int n, style_t st) {
    /* collect rows (tr children, possibly wrapped in tbody/inline) */
    int rows[128]; int nrows = 0;
    for (int c = nodes[n].first; c >= 0 && nrows < 128; c = nodes[c].next) {
        if (nodes[c].tag == T_TR) rows[nrows++] = c;
        else if (nodes[c].tag == T_INLINE || nodes[c].tag == T_SPAN)
            for (int d = nodes[c].first; d >= 0 && nrows < 128; d = nodes[d].next) if (nodes[d].tag == T_TR) rows[nrows++] = d;
    }
    int ncols = 0;
    for (int r = 0; r < nrows; r++) {
        int k = 0;
        for (int c = nodes[rows[r]].first; c >= 0; c = nodes[c].next) if (nodes[c].tag == T_TD || nodes[c].tag == T_TH) k++;
        if (k > ncols) ncols = k;
    }
    if (!nrows || !ncols) { layout_children(n, &st); return; }
    if (ncols > 24) ncols = 24;
    block_gap(10);
    line_flush();
    if (pending_margin) { cur_y += pending_margin; pending_margin = 0; }
    int tx = x0, tw = x1 - x0;
    int cw = tw / ncols;
    if (cw < 24) { cw = 24; }
    int pad = 6;
    int save_x0 = x0, save_x1 = x1;
    int table_top = cur_y;
    int depth_fill_items[24];
    for (int r = 0; r < nrows; r++) {
        int row_top = cur_y;
        int row_bottom = row_top;
        int k = 0;
        for (int c = nodes[rows[r]].first; c >= 0 && k < ncols; c = nodes[c].next) {
            if (nodes[c].tag != T_TD && nodes[c].tag != T_TH) continue;
            int cx = tx + k * cw;
            depth_fill_items[k] = -1;
            if (nodes[c].tag == T_TH) {
                we_item_t* f = item_new(IT_FILL);
                if (f) { f->x = (int16_t)cx; f->y = row_top; f->w = (int16_t)cw; f->h = 0; f->color = C_THBG; depth_fill_items[k] = (int)(f - items); }
            }
            x0 = cx + pad; x1 = cx + cw - pad;
            cur_y = row_top + pad;
            style_t cs = st;
            cs.center = nodes[c].center;
            if (nodes[c].tag == T_TH) cs.font = UF_UIB;
            line_begin(); line_center = cs.center;
            layout_children(c, &cs);
            line_flush();
            int bottom = cur_y + pad;
            if (!line_content && cur_y == row_top + pad) bottom = row_top + pad + style_h(&st) + pad;  /* empty cell */
            if (bottom > row_bottom) row_bottom = bottom;
            k++;
        }
        if (row_bottom == row_top) row_bottom = row_top + style_h(&st) + 2 * pad;
        for (int c2 = 0; c2 < k; c2++) if (depth_fill_items[c2] >= 0) items[depth_fill_items[c2]].h = (int16_t)(row_bottom - row_top);
        /* cell borders */
        for (int c2 = 0; c2 < ncols; c2++) {
            we_item_t* b = item_new(IT_BOX);
            if (b) { b->x = (int16_t)(tx + c2 * cw); b->y = row_top; b->w = (int16_t)(cw + 1); b->h = (int16_t)(row_bottom - row_top + 1); b->color = C_BORDER; }
        }
        cur_y = row_bottom;
    }
    (void)table_top;
    x0 = save_x0; x1 = save_x1;
    line_begin();
    block_gap(10);
}

static void layout_node(int n, style_t st) {
    we_node_t* nd = &nodes[n];
    switch (nd->tag) {
    case T_TEXT:
        inline_text(nd->off, nd->len, &st);
        return;
    case T_BR:
        if (!line_content) { line_top = cur_y; line_h = style_h(&st); line_content = true; line_first = nitems; if (pending_margin) { cur_y += pending_margin; pending_margin = 0; line_top = cur_y; } }
        line_flush();
        return;
    case T_HR: {
        block_gap(10); line_flush();
        if (pending_margin) { cur_y += pending_margin; pending_margin = 0; }
        we_item_t* it = item_new(IT_RULE);
        if (it) { it->x = (int16_t)x0; it->y = cur_y; it->w = (int16_t)(x1 - x0); it->h = 1; it->color = C_RULE; }
        cur_y += 1;
        line_begin();
        block_gap(10);
        return;
    }
    case T_IMG: {
        if (nd->slot < 0) return;
        we_image_t* im = &images[nd->slot];
        int w = im->attr_w, h = im->attr_h;
        if (im->state == WE_IMG_READY) {
            if (!w && !h) { w = im->nat_w; h = im->nat_h; }
            else if (!w) w = (im->nat_w * h + im->nat_h / 2) / (im->nat_h ? im->nat_h : 1);
            else if (!h) h = (im->nat_h * w + im->nat_w / 2) / (im->nat_w ? im->nat_w : 1);
        } else {
            if (!w) w = h ? h : 120;
            if (!h) h = 60;
            if (im->state == WE_IMG_FAILED && im->alt >= 0 && !im->attr_w) {
                int tw = (text_q4(we_str(im->alt), (int)strlen(we_str(im->alt)), UF_SMALL) + 3) / 4 + 24;
                if (tw > w) w = tw;
            }
        }
        int maxw = x1 - x0;
        if (w > maxw && maxw > 8) { h = (h * maxw + w / 2) / w; w = maxw; }
        if (h < 1) h = 1;
        if (w < 1) w = 1;
        if (line_content && ((pen_q + 3) >> 2) + w > x1) line_flush();
        if (!line_content && pending_margin) { cur_y += pending_margin; pending_margin = 0; line_top = cur_y; }
        if (line_content && pending_space) pen_q += style_q4(" ", 1, &st);
        pending_space = false;
        we_item_t* it = item_new(IT_IMG);
        if (!it) return;
        int x = (pen_q + 3) >> 2;
        it->x = (int16_t)x; it->y = line_top; it->w = (int16_t)w; it->h = (int16_t)h; it->aux = nd->slot; it->link = (int16_t)st.link;
        it->flags = st.flags;
        pen_q = (x + w) << 2;
        if (h > line_h) line_h = h;
        line_content = true;
        return;
    }
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
        int lvl = nd->tag - T_H1;
        static const uint8_t HF[6] = { UF_TITLE, UF_TITLE, UF_UIB, UF_UIB, UF_UIB, UF_SMALL };
        static const uint8_t HS[6] = { 6, 4, 5, 4, 4, 4 };     /* /4: 1.5x, 1x, 1.25x ... */
        static const uint8_t HM[6] = { 22, 18, 16, 14, 12, 12 };
        block_gap(HM[lvl]);
        st.font = HF[lvl]; st.scale = HS[lvl]; st.color = (st.color == C_TEXT) ? 0x111113 : st.color;
        st.flags &= (uint8_t)~F_ITALIC;
        line_center = nd->center ? 1 : st.center;
        layout_children(n, &st);
        block_gap(HM[lvl] * 2 / 3);
        line_center = st.center;
        return;
    }
    case T_P:
        block_gap(12);
        line_center = nd->center ? 1 : st.center; st.center = line_center;
        layout_children(n, &st);
        block_gap(12);
        return;
    case T_DIV: case T_BLOCK: case T_BODY: case T_HTML: case T_ROOT: case T_CENTER: case T_DT: case T_TR:
        if (nd->tag == T_CENTER) st.center = 1;
        if (nd->center) st.center = 1;
        if (nd->tag != T_ROOT && nd->tag != T_HTML && nd->tag != T_BODY) block_gap(0);
        line_center = st.center;
        if (nd->tag == T_DT) st.font = UF_UIB;
        layout_children(n, &st);
        if (nd->tag != T_ROOT && nd->tag != T_HTML && nd->tag != T_BODY) block_gap(0);
        return;
    case T_SPAN: case T_INLINE:
        layout_children(n, &st);
        return;
    case T_A:
        if (nd->s1 >= 0 && nlinks < WE_MAX_LINKS) {
            int li = nlinks++;
            links[li].href = nd->s1; links[li].first_item = nitems;
            links[li].x = links[li].y = links[li].w = links[li].h = 0;
            st.link = li; st.flags |= F_LINK | F_UNDER; st.color = C_LINK;
            layout_children(n, &st);
            /* bounding box of the first run */
            for (int i = links[li].first_item; i < nitems; i++) {
                if (items[i].link == li && (items[i].kind == IT_TEXT || items[i].kind == IT_IMG)) {
                    links[li].x = items[i].x; links[li].y = items[i].y; links[li].w = items[i].w; links[li].h = items[i].h;
                    break;
                }
            }
            if (links[li].w == 0) { nlinks--; }   /* empty anchor: forget it */
        } else {
            layout_children(n, &st);
        }
        return;
    case T_B: st.font = (st.font == UF_UI) ? UF_UIB : st.font; layout_children(n, &st); return;
    case T_I: st.flags |= F_ITALIC; layout_children(n, &st); return;
    case T_U: st.flags |= F_UNDER; layout_children(n, &st); return;
    case T_SMALL: if (st.font == UF_UI || st.font == UF_UIB) st.font = UF_SMALL; layout_children(n, &st); return;
    case T_FONT:
        if (nd->s1 > 0) st.color = (uint32_t)nd->s1 & 0xFFFFFF;
        layout_children(n, &st);
        return;
    case T_CODE:
        st.flags |= F_MONO;
        layout_children(n, &st);
        return;
    case T_PRE: {
        block_gap(12); line_flush();
        if (pending_margin) { cur_y += pending_margin; pending_margin = 0; }
        int top = cur_y;
        we_item_t* bg = item_new(IT_FILL);
        int bgi = bg ? (int)(bg - items) : -1;
        if (bg) { bg->x = (int16_t)x0; bg->y = top; bg->w = (int16_t)(x1 - x0); bg->color = C_CODEBG; }
        cur_y += 8;
        int sx0 = x0, sx1 = x1;
        x0 += 10; x1 -= 10;
        st.flags |= F_MONO | F_PRE;
        line_begin();
        layout_children(n, &st);
        line_flush();
        x0 = sx0; x1 = sx1;
        cur_y += 8;
        if (bgi >= 0) items[bgi].h = (int16_t)(cur_y - top);
        line_begin();
        block_gap(12);
        return;
    }
    case T_UL: case T_OL:
        block_gap(list_depth ? 2 : 10);
        if (list_depth < 19) {
            list_depth++;
            list_ordered[list_depth] = (nd->tag == T_OL);
            list_counter[list_depth] = 0;
            x0 += 28;
            line_begin();
            layout_children(n, &st);
            line_flush();
            x0 -= 28;
            list_depth--;
        } else layout_children(n, &st);
        line_begin();
        block_gap(list_depth ? 2 : 10);
        return;
    case T_LI: {
        block_gap(3); line_flush();
        if (pending_margin) { cur_y += pending_margin; pending_margin = 0; }
        line_begin();
        int d = list_depth > 0 ? list_depth : 1;
        list_counter[d]++;
        we_item_t* b = item_new(IT_BULLET);
        if (b) {
            b->x = (int16_t)(x0 - 20); b->y = line_top; b->w = 16; b->h = (int16_t)style_h(&st);
            b->aux = (int16_t)(list_ordered[d] ? list_counter[d] : (d > 1 ? -1 : 0));
            b->color = st.color; b->font = st.font;
            line_h = b->h; line_content = true;
        }
        layout_children(n, &st);
        line_flush();
        block_gap(3);
        return;
    }
    case T_DD:
        block_gap(2);
        x0 += 28; line_begin();
        layout_children(n, &st);
        line_flush(); x0 -= 28; line_begin();
        block_gap(2);
        return;
    case T_BLOCKQUOTE: {
        block_gap(12); line_flush();
        if (pending_margin) { cur_y += pending_margin; pending_margin = 0; }
        int top = cur_y;
        we_item_t* bar = item_new(IT_FILL);
        int bi = bar ? (int)(bar - items) : -1;
        if (bar) { bar->x = (int16_t)x0; bar->y = top; bar->w = 3; bar->color = C_QUOTE; }
        x0 += 20; x1 -= 8;
        st.color = (st.color == C_TEXT) ? 0x4A4A50 : st.color;
        line_begin();
        layout_children(n, &st);
        line_flush();
        x0 -= 20; x1 += 8;
        if (bi >= 0) items[bi].h = (int16_t)(cur_y - top);
        line_begin();
        block_gap(12);
        return;
    }
    case T_TABLE:
        layout_table(n, st);
        return;
    case T_TD: case T_TH:
        /* a stray cell outside a table: treat as a block */
        block_gap(0); layout_children(n, &st); block_gap(0);
        return;
    case T_HEAD: case T_TITLE: case T_SCRIPT: case T_STYLE:
        return;
    default:
        layout_children(n, &st);
        return;
    }
}

void we_layout(int cw) {
    metrics_init();
    if (cw < 60) cw = 60;
    content_w = cw;
    nitems = 0; nlinks = 0;
    cur_y = 0; x0 = 0; x1 = cw; pending_margin = 0; list_depth = 0; line_center = 0;
    style_t st = { UF_UI, 4, 0, 0, -1, C_TEXT };
    line_begin();
    if (nnodes > 0) layout_node(0, st);
    line_flush();
    if (truncated) {
        block_gap(16); line_flush();
        if (pending_margin) { cur_y += pending_margin; pending_margin = 0; }
        /* the marker text lives in the strpool so it has a stable address */
        static const char MSG[] = "[ page truncated: too much content for this browser ]";
        int off = str_add(MSG, -1);
        if (off >= 0) {
            we_item_t* it = item_new(IT_TEXT);
            if (it) {
                it->x = (int16_t)x0; it->y = cur_y; it->h = 20; it->off = -1 - off; it->len = (int16_t)(sizeof(MSG) - 1);
                it->font = UF_SMALL; it->scale = 4; it->color = C_MUTED;
                it->w = (int16_t)((text_q4(MSG, it->len, UF_SMALL) + 3) >> 2);
                cur_y += 20;
            }
        }
    }
    doc_h = cur_y + 4;
}

int we_doc_height(void) { return doc_h; }
const char* we_title(void) { return title; }
int we_link_count(void) { return nlinks; }
const we_link_t* we_link(int i) { return (i >= 0 && i < nlinks) ? &links[i] : NULL; }
int we_image_count(void) { return nimages; }
we_image_t* we_image(int i) { return (i >= 0 && i < nimages) ? &images[i] : NULL; }

int we_link_at(int dx, int dy) {
    for (int i = 0; i < nitems; i++) {
        const we_item_t* it = &items[i];
        if (it->link < 0 || (it->kind != IT_TEXT && it->kind != IT_IMG)) continue;
        if (dx >= it->x && dx < it->x + it->w && dy >= it->y && dy < it->y + it->h) return it->link;
    }
    return -1;
}

bool we_is_image_type(const char* ct) {
    return ct && lc(ct[0]) == 'i' && lc(ct[1]) == 'm' && lc(ct[2]) == 'a' && lc(ct[3]) == 'g' && lc(ct[4]) == 'e' && ct[5] == '/';
}

/* --------------------------------------------------------------------------
 * Images: decode into the arena, box-filter into the pool
 * -------------------------------------------------------------------------- */
static void scale_image(uint32_t* dst, int dw, int dh, const uint32_t* s, int sw, int sh) {
    /* dst may start before the source (same region): rows are produced top to
     * bottom into rowbuf first, and dst row j never reaches past source row j'
     * (dw <= sw, dh <= sh), so nothing still needed is overwritten. */
    for (int j = 0; j < dh; j++) {
        int sy0 = (j * sh) / dh, sy1 = ((j + 1) * sh) / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        for (int i = 0; i < dw; i++) {
            int sx0 = (i * sw) / dw, sx1 = ((i + 1) * sw) / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = sy0; y < sy1; y++) {
                const uint32_t* row = s + y * sw;
                for (int x = sx0; x < sx1; x++) {
                    uint32_t c = row[x];
                    r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; n++;
                }
            }
            if (!n) n = 1;
            rowbuf[i] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
        memcpy(dst + j * dw, rowbuf, (uint32_t)dw * 4);
    }
}

void we_image_decoded(int i, const uint8_t* data, uint32_t size, int max_w) {
    we_image_t* im = we_image(i);
    if (!im) return;
    uint32_t free_px = WE_IMG_POOL_PX - img_pool_used;
    if (free_px < 64 * 64 || !data || size < 8) { im->state = WE_IMG_FAILED; return; }
    uint8_t* arena = (uint8_t*)(img_region + img_pool_used);
    image_arena_begin(arena, free_px * 4, WE_IMG_MAX_DIM, WE_IMG_MAX_PX);
    image_t img;
    memset(&img, 0, sizeof(img));
    bool ok = image_decode(data, size, &img);
    if (!ok || !img.pixels || img.width <= 0 || img.height <= 0) {
        image_arena_end();
        im->state = WE_IMG_FAILED;
        return;
    }
    im->nat_w = img.width; im->nat_h = img.height;
    /* display size: attributes, then fit to the content width */
    int w = im->attr_w, h = im->attr_h;
    if (!w && !h) { w = img.width; h = img.height; }
    else if (!w) w = (img.width * h + img.height / 2) / img.height;
    else if (!h) h = (img.height * w + img.width / 2) / img.width;
    if (max_w > 8 && w > max_w) { h = (h * max_w + w / 2) / w; w = max_w; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    /* never store upscaled pixels (paint stretches); never exceed the natural size */
    int sw = w < img.width ? w : img.width;
    int sh = h < img.height ? h : img.height;
    if (w > img.width || h > img.height) {
        sw = img.width; sh = img.height;
        if (sw > w) { sh = (sh * w + sw / 2) / sw; sw = w; }
        if (sh > h) { sw = (sw * h + sh / 2) / sh; sh = h; }
    }
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    if ((uint32_t)sw * (uint32_t)sh > free_px) { image_arena_end(); im->state = WE_IMG_FAILED; return; }
    uint32_t* dst = img_region + img_pool_used;
    scale_image(dst, sw, sh, img.pixels, img.width, img.height);
    image_arena_end();
    im->px = dst; im->disp_w = sw; im->disp_h = sh;
    img_pool_used += (uint32_t)sw * (uint32_t)sh;
    im->state = WE_IMG_READY;
}

/* --------------------------------------------------------------------------
 * Paint
 * -------------------------------------------------------------------------- */
static uint32_t* BB; static int SW, SH;

void we_fill(int x, int y, int w, int h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color);
}

static inline uint32_t mix3(uint32_t a, uint32_t b, int t) {   /* t 0..256 */
    uint32_t ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    uint32_t br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    uint32_t r = ar + (((int)(br - ar) * t) >> 8), g = ag + (((int)(bg - ag) * t) >> 8), bl = ab + (((int)(bb - ab) * t) >> 8);
    return (r << 16) | (g << 8) | bl;
}

/* Draw text at x,y resampled by scale/4 (bilinear). The rect must be fully on
 * screen; bg is the solid colour under the run. */
static void draw_text_scaled(int x, int y, const char* t, int n, int font, uint32_t color, int scale, uint32_t bg) {
    int w0 = (text_q4(t, n, font) + 3) >> 2, h0 = ui_font_height(font);
    int w1 = (w0 * scale + 3) / 4, h1 = (h0 * scale + 3) / 4;
    if (w0 <= 0 || w0 > SCR_W || h0 > SCR_H || x < 0 || y < 0 || x + w1 >= SW || y + h1 >= SH || x + w0 >= SW || y + h0 >= SH) {
        ui_text_n(x, y, t, n, font, color);
        return;
    }
    fb_fill_rect(x, y, w0, h0, bg);
    ui_text_n(x, y, t, n, font, color);
    for (int r = 0; r < h0; r++) memcpy(scratch + r * w0, BB + (y + r) * SW + x, (uint32_t)w0 * 4);
    fb_fill_rect(x, y, w1, h1, bg);
    for (int j = 0; j < h1; j++) {
        int syq = (j * 4 * 256) / scale;             /* 24.8 fixed */
        int sy = syq >> 8, fy = syq & 255;
        if (sy >= h0 - 1) { sy = h0 - 1; fy = 0; }
        const uint32_t* r0 = scratch + sy * w0;
        const uint32_t* r1 = scratch + (fy ? sy + 1 : sy) * w0;
        uint32_t* d = BB + (y + j) * SW + x;
        for (int i = 0; i < w1; i++) {
            int sxq = (i * 4 * 256) / scale;
            int sx = sxq >> 8, fx = sxq & 255;
            if (sx >= w0 - 1) { sx = w0 - 1; fx = 0; }
            int sx2 = fx ? sx + 1 : sx;
            uint32_t a = mix3(r0[sx], r0[sx2], fx), b = mix3(r1[sx], r1[sx2], fx);
            d[i] = mix3(a, b, fy);
        }
    }
}

static void shear_rect(int x, int y, int w, int h, uint32_t bg) {
    if (x < 0 || y < 0 || x + w + 3 >= SW || y + h >= SH) return;
    for (int r = 0; r < h; r++) {
        int shift = ((h - 1 - r) * 3) / h;
        if (!shift) continue;
        uint32_t* row = BB + (y + r) * SW + x;
        for (int i = w - 1; i >= 0; i--) row[i + shift] = row[i];
        for (int i = 0; i < shift; i++) row[i] = bg;
    }
}

static void draw_mono(int x, int y, const char* t, int n, uint32_t fg, uint32_t bg, int clip_x1) {
    int col = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        int reps = (c == '\t') ? 4 : 1;
        if (c == '\t') c = ' ';
        for (int r = 0; r < reps; r++, col++) {
            int px = x + col * 8;
            if (px + 8 > clip_x1) return;
            if (c < 32 || c > 126) c = '?';
            ui_cell_glyph(px, y, c, fg, bg);
        }
    }
}

static void blit_image(const we_image_t* im, int x, int y, int w, int h, int cx0, int cy0, int cx1, int cy1) {
    int dx0 = x > cx0 ? x : cx0, dy0 = y > cy0 ? y : cy0;
    int dx1 = x + w < cx1 ? x + w : cx1, dy1 = y + h < cy1 ? y + h : cy1;
    if (dx0 >= dx1 || dy0 >= dy1) return;
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > SW) dx1 = SW;
    if (dy1 > SH) dy1 = SH;
    bool direct = (im->disp_w == w && im->disp_h == h);
    for (int yy = dy0; yy < dy1; yy++) {
        int sy = direct ? yy - y : ((yy - y) * im->disp_h) / h;
        if (sy >= im->disp_h) sy = im->disp_h - 1;
        const uint32_t* srow = im->px + sy * im->disp_w;
        uint32_t* drow = BB + yy * SW;
        if (direct) {
            memcpy(drow + dx0, srow + (dx0 - x), (uint32_t)(dx1 - dx0) * 4);
        } else {
            for (int xx = dx0; xx < dx1; xx++) {
                int sx = ((xx - x) * im->disp_w) / w;
                if (sx >= im->disp_w) sx = im->disp_w - 1;
                drow[xx] = srow[sx];
            }
        }
    }
}

void we_paint(int vx, int vy, int vw, int vh, int scroll_y, int focused) {
    BB = fb_get_backbuffer(); SW = (int)fb_get_width(); SH = (int)fb_get_height();
    if (!BB) return;
    fb_fill_rect(vx, vy, vw, vh, C_PAGE);
    int cx1 = vx + vw, cy1 = vy + vh;
    for (int i = 0; i < nitems; i++) {
        const we_item_t* it = &items[i];
        int y = vy + it->y - scroll_y;
        int x = vx + it->x;
        if (y >= cy1 || y + it->h <= vy) continue;
        switch (it->kind) {
        case IT_FILL: {
            int yy0 = y > vy ? y : vy, yy1 = y + it->h < cy1 ? y + it->h : cy1;
            int xx0 = x > vx ? x : vx, xx1 = x + it->w < cx1 ? x + it->w : cx1;
            if (yy1 > yy0 && xx1 > xx0) fb_fill_rect(xx0, yy0, xx1 - xx0, yy1 - yy0, it->color);
            break;
        }
        case IT_BOX: {
            int yy0 = y > vy ? y : vy, yy1 = y + it->h < cy1 ? y + it->h : cy1;
            int xx0 = x > vx ? x : vx, xx1 = x + it->w < cx1 ? x + it->w : cx1;
            if (yy1 <= yy0 || xx1 <= xx0) break;
            if (y >= vy) fb_fill_rect(xx0, y, xx1 - xx0, 1, it->color);
            if (y + it->h - 1 < cy1) fb_fill_rect(xx0, y + it->h - 1, xx1 - xx0, 1, it->color);
            if (x >= vx) fb_fill_rect(x, yy0, 1, yy1 - yy0, it->color);
            if (x + it->w - 1 < cx1) fb_fill_rect(x + it->w - 1, yy0, 1, yy1 - yy0, it->color);
            break;
        }
        case IT_RULE:
            if (y >= vy && y < cy1) fb_fill_rect(x, y, it->w, 1, it->color);
            break;
        case IT_BULLET: {
            if (y < vy || y + it->h > cy1) break;
            if (it->aux > 0) {
                char nb[8]; int_to_str(it->aux, nb); strcat(nb, ".");
                int w = ui_text_width(nb, it->font);
                ui_text(x + 16 - w, y + (it->h - ui_font_height(it->font)) / 2, nb, it->font, it->color);
            } else if (it->aux == 0) {
                ui_circle(x + 10, y + it->h / 2, 3, it->color, 255);
            } else {
                ui_ring(x + 10, y + it->h / 2, 3, 1, it->color, 255);
            }
            break;
        }
        case IT_IMG: {
            const we_image_t* im = we_image(it->aux);
            if (!im) break;
            if (im->state == WE_IMG_READY && im->px) {
                blit_image(im, x, y, it->w, it->h, vx, vy, cx1, cy1);
                if (it->link >= 0 && it->link == focused) {
                    ui_rrect_stroke(x - 2, y - 2, it->w + 4, it->h + 4, 3, C_LINK, 255);
                }
            } else {
                int yy0 = y > vy ? y : vy, yy1 = y + it->h < cy1 ? y + it->h : cy1;
                int xx0 = x > vx ? x : vx, xx1 = x + it->w < cx1 ? x + it->w : cx1;
                if (yy1 > yy0 && xx1 > xx0) fb_fill_rect(xx0, yy0, xx1 - xx0, yy1 - yy0, 0xF4F4F6);
                if (y >= vy && y + it->h <= cy1 && x >= vx && x + it->w <= cx1) {
                    ui_rrect_stroke(x, y, it->w, it->h, 2, C_BORDER, 255);
                    const char* alt = (im->alt >= 0) ? we_str(im->alt) : "";
                    const char* msg = im->state == WE_IMG_FAILED ? (alt[0] ? alt : "[image]") : (alt[0] ? alt : "loading...");
                    if (it->h >= 16 && it->w >= 24) {
                        int tw = ui_text_width(msg, UF_SMALL);
                        if (tw <= it->w - 8) ui_text(x + (it->w - tw) / 2, y + (it->h - ui_font_height(UF_SMALL)) / 2, msg, UF_SMALL, C_MUTED);
                    }
                }
            }
            break;
        }
        case IT_TEXT: {
            if (y + it->h > cy1) break;            /* bottom-crossing runs wait for the next scroll */
            const char* t = it->off >= 0 ? src_buf + it->off : strpool + (-1 - it->off);
            uint32_t bg = (it->flags & F_PRE) ? C_CODEBG : C_PAGE;
            int ty;
            if (it->flags & F_MONO) {
                if (!(it->flags & F_PRE)) {
                    fb_fill_rect(x - 2, y, it->w + 4, it->h, C_CODEBG);
                    bg = C_CODEBG;
                }
                ty = y + (it->h - 16) / 2;
                if (it->link >= 0 && it->link == focused) { fb_fill_rect(x - 2, y, it->w + 4, it->h, 0xDCEBFF); bg = 0xDCEBFF; }
                draw_mono(x, ty, t, it->len, it->color, bg, cx1);
            } else {
                int fh = ui_font_height(it->font) * it->scale / 4;
                ty = y + (it->h - fh) / 2;
                if (it->link >= 0 && it->link == focused) { ui_rrect(x - 3, y, it->w + 6, it->h, 3, 0xDCEBFF, 255); bg = 0xDCEBFF; }
                if (it->scale != 4) draw_text_scaled(x, ty, t, it->len, it->font, it->color, it->scale, bg);
                else {
                    if (x + it->w > cx1) {
                        /* clip long runs to the viewport by characters */
                        int n = 0, q = 0, lim = (cx1 - x) << 2;
                        while (n < it->len) { int a = adv_q4[it->font][((unsigned char)t[n] >= 32 && (unsigned char)t[n] < 127) ? t[n] - 32 : '?' - 32]; if (q + a > lim) break; q += a; n++; }
                        ui_text_n(x, ty, t, n, it->font, it->color);
                    } else ui_text_n(x, ty, t, it->len, it->font, it->color);
                    if (it->flags & F_ITALIC) shear_rect(x, ty, it->w, ui_font_height(it->font), bg);
                }
            }
            if (it->flags & F_UNDER) {
                int uy = y + it->h - 3;
                if (uy >= vy && uy < cy1) fb_fill_rect(x, uy, it->w < cx1 - x ? it->w : cx1 - x, 1, it->color);
            }
            break;
        }
        default: break;
        }
    }
}
