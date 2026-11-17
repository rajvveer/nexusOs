/* ============================================================================
 * NexusOS — VT100/ANSI terminal engine (Implementation) — Phase 57 "apps"
 * ============================================================================
 * See vt.h. Compiled at -O2 (build.bat hot list): it runs for every byte a
 * user program prints.
 * ============================================================================ */

#include "vt.h"
#include "string.h"

enum { ST_NORMAL = 0, ST_ESC, ST_CSI, ST_SKIP1 };

/* ANSI colour index -> VGA colour index (0..7) */
static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static inline int idx(const vt_t* v, int r, int c) { return r * v->cols + c; }

static void put_cell(vt_t* v, int r, int c, uint8_t ch, uint8_t attr) {
    int i = idx(v, r, c);
    v->ch[i] = ch; v->at[i] = attr;
    if (v->paint) v->paint(v, r, c, ch, attr);
}

static void clear_cells(vt_t* v, int r, int c0, int c1) {
    for (int c = c0; c <= c1; c++) put_cell(v, r, c, ' ', v->attr);
}

static void recompute_attr(vt_t* v) {
    uint8_t fg = v->fg, bg = v->bg;
    if (v->bold) fg |= 8;
    if (v->inverse) { uint8_t t = fg; fg = bg; bg = t; }
    v->attr = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void vt_init(vt_t* v, int cols, int rows, vt_paint_fn paint, vt_reply_fn reply, void* owner) {
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    v->cols = cols; v->rows = rows;
    v->paint = NULL;                       /* fill without painting */
    v->fg = 7; v->bg = 0; v->bold = false; v->inverse = false;
    recompute_attr(v);
    v->def_attr = v->attr;
    for (int i = 0; i < cols * rows; i++) { v->ch[i] = ' '; v->at[i] = v->attr; }
    v->cx = v->cy = 0; v->scx = v->scy = 0;
    v->wrap_pending = false; v->cursor_visible = true;
    v->top = 0; v->bot = rows - 1;
    v->state = ST_NORMAL; v->nparams = 0; v->priv = false;
    v->dirty = true;
    v->paint = paint; v->reply = reply; v->owner = owner;
}

void vt_import(vt_t* v, int row, int col, uint8_t ch, uint8_t attr) {
    if (row < 0 || row >= v->rows || col < 0 || col >= v->cols) return;
    int i = idx(v, row, col);
    v->ch[i] = ch; v->at[i] = attr;
}

void vt_set_cursor(vt_t* v, int row, int col) {
    if (row < 0) row = 0;
    if (row >= v->rows) row = v->rows - 1;
    if (col < 0) col = 0;
    if (col >= v->cols) col = v->cols - 1;
    v->cy = row; v->cx = col; v->wrap_pending = false;
}

void vt_repaint(vt_t* v) {
    if (!v->paint) return;
    for (int r = 0; r < v->rows; r++)
        for (int c = 0; c < v->cols; c++) {
            int i = idx(v, r, c);
            v->paint(v, r, c, v->ch[i], v->at[i]);
        }
}

/* Scroll the region [top..bot] up by n lines (blank lines appear at the bottom) */
static void scroll_up(vt_t* v, int top, int bot, int n) {
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    int w = v->cols;
    for (int r = top; r <= bot - n; r++) {
        memcpy(&v->ch[idx(v, r, 0)], &v->ch[idx(v, r + n, 0)], w);
        memcpy(&v->at[idx(v, r, 0)], &v->at[idx(v, r + n, 0)], w);
    }
    for (int r = bot - n + 1; r <= bot; r++)
        for (int c = 0; c < w; c++) { v->ch[idx(v, r, c)] = ' '; v->at[idx(v, r, c)] = v->attr; }
    vt_repaint(v);
}

/* Scroll the region down by n lines (blank lines appear at the top) */
static void scroll_down(vt_t* v, int top, int bot, int n) {
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    int w = v->cols;
    for (int r = bot; r >= top + n; r--) {
        memcpy(&v->ch[idx(v, r, 0)], &v->ch[idx(v, r - n, 0)], w);
        memcpy(&v->at[idx(v, r, 0)], &v->at[idx(v, r - n, 0)], w);
    }
    for (int r = top; r < top + n; r++)
        for (int c = 0; c < w; c++) { v->ch[idx(v, r, c)] = ' '; v->at[idx(v, r, c)] = v->attr; }
    vt_repaint(v);
}

static void linefeed(vt_t* v) {
    if (v->cy == v->bot) scroll_up(v, v->top, v->bot, 1);
    else if (v->cy < v->rows - 1) v->cy++;
}

static void erase_display(vt_t* v, int mode) {
    if (mode == 2) {
        for (int r = 0; r < v->rows; r++) clear_cells(v, r, 0, v->cols - 1);
    } else if (mode == 0) {
        clear_cells(v, v->cy, v->cx, v->cols - 1);
        for (int r = v->cy + 1; r < v->rows; r++) clear_cells(v, r, 0, v->cols - 1);
    } else if (mode == 1) {
        for (int r = 0; r < v->cy; r++) clear_cells(v, r, 0, v->cols - 1);
        clear_cells(v, v->cy, 0, v->cx);
    }
}

static void sgr(vt_t* v) {
    if (v->nparams == 0) { v->params[0] = 0; v->nparams = 1; }
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (p == 0) { v->fg = 7; v->bg = 0; v->bold = false; v->inverse = false; }
        else if (p == 1) v->bold = true;
        else if (p == 7) v->inverse = true;
        else if (p == 22) v->bold = false;
        else if (p == 27) v->inverse = false;
        else if (p >= 30 && p <= 37) v->fg = ansi2vga[p - 30];
        else if (p == 39) v->fg = 7;
        else if (p >= 40 && p <= 47) v->bg = ansi2vga[p - 40];
        else if (p == 49) v->bg = 0;
        else if (p >= 90 && p <= 97) v->fg = ansi2vga[p - 90] | 8;
        else if (p >= 100 && p <= 107) v->bg = ansi2vga[p - 100] | 8;
        else if (p == 38 || p == 48) {              /* 38;5;n / 38;2;r;g;b: skip args */
            if (i + 1 < v->nparams && v->params[i + 1] == 5) {
                if (i + 2 < v->nparams) {
                    int n = v->params[i + 2] & 0xFF;
                    uint8_t col = (n < 8) ? ansi2vga[n] : (n < 16) ? (ansi2vga[n - 8] | 8) : 7;
                    if (p == 38) v->fg = col; else v->bg = col;
                }
                i += 2;
            } else if (i + 1 < v->nparams && v->params[i + 1] == 2) i += 4;
        }
    }
    recompute_attr(v);
}

static void csi_dispatch(vt_t* v, uint8_t f) {
    int p0 = v->nparams > 0 ? v->params[0] : 0;
    int p1 = v->nparams > 1 ? v->params[1] : 0;
    int n = p0 > 0 ? p0 : 1;
    if (v->priv) {
        if (f == 'h' || f == 'l') {
            bool on = (f == 'h');
            for (int i = 0; i < v->nparams; i++) {
                if (v->params[i] == 25) v->cursor_visible = on;
                else if (v->params[i] == 1049 || v->params[i] == 47 || v->params[i] == 1047) {
                    erase_display(v, 2); v->cx = v->cy = 0; v->wrap_pending = false;
                }
            }
        }
        return;
    }
    switch (f) {
    case 'H': case 'f': {
        int r = (p0 > 0 ? p0 : 1) - 1, c = (p1 > 0 ? p1 : 1) - 1;
        vt_set_cursor(v, r, c);
        break; }
    case 'A': v->cy -= n; if (v->cy < 0) v->cy = 0; v->wrap_pending = false; break;
    case 'B': v->cy += n; if (v->cy >= v->rows) v->cy = v->rows - 1; v->wrap_pending = false; break;
    case 'C': v->cx += n; if (v->cx >= v->cols) v->cx = v->cols - 1; v->wrap_pending = false; break;
    case 'D': v->cx -= n; if (v->cx < 0) v->cx = 0; v->wrap_pending = false; break;
    case 'E': v->cx = 0; v->cy += n; if (v->cy >= v->rows) v->cy = v->rows - 1; v->wrap_pending = false; break;
    case 'F': v->cx = 0; v->cy -= n; if (v->cy < 0) v->cy = 0; v->wrap_pending = false; break;
    case 'G': case '`': vt_set_cursor(v, v->cy, n - 1); break;
    case 'd': vt_set_cursor(v, n - 1, v->cx); break;
    case 'J': erase_display(v, p0); break;
    case 'K':
        if (p0 == 0) clear_cells(v, v->cy, v->cx, v->cols - 1);
        else if (p0 == 1) clear_cells(v, v->cy, 0, v->cx);
        else clear_cells(v, v->cy, 0, v->cols - 1);
        break;
    case 'L': if (v->cy >= v->top && v->cy <= v->bot) scroll_down(v, v->cy, v->bot, n); break;
    case 'M': if (v->cy >= v->top && v->cy <= v->bot) scroll_up(v, v->cy, v->bot, n); break;
    case 'S': scroll_up(v, v->top, v->bot, n); break;
    case 'T': scroll_down(v, v->top, v->bot, n); break;
    case '@': {                                    /* insert blanks at cursor */
        int w = v->cols; if (n > w - v->cx) n = w - v->cx;
        for (int c = w - 1; c >= v->cx + n; c--) {
            int d = idx(v, v->cy, c), s = idx(v, v->cy, c - n);
            v->ch[d] = v->ch[s]; v->at[d] = v->at[s];
            if (v->paint) v->paint(v, v->cy, c, v->ch[d], v->at[d]);
        }
        clear_cells(v, v->cy, v->cx, v->cx + n - 1);
        break; }
    case 'P': {                                    /* delete chars at cursor */
        int w = v->cols; if (n > w - v->cx) n = w - v->cx;
        for (int c = v->cx; c < w - n; c++) {
            int d = idx(v, v->cy, c), s = idx(v, v->cy, c + n);
            v->ch[d] = v->ch[s]; v->at[d] = v->at[s];
            if (v->paint) v->paint(v, v->cy, c, v->ch[d], v->at[d]);
        }
        clear_cells(v, v->cy, w - n, w - 1);
        break; }
    case 'X': { int e = v->cx + n - 1; if (e >= v->cols) e = v->cols - 1; clear_cells(v, v->cy, v->cx, e); break; }
    case 'm': sgr(v); break;
    case 'r': {
        int t = (p0 > 0 ? p0 : 1) - 1, b = (p1 > 0 ? p1 : v->rows) - 1;
        if (b >= v->rows) b = v->rows - 1;
        if (t < b) { v->top = t; v->bot = b; }
        else { v->top = 0; v->bot = v->rows - 1; }
        vt_set_cursor(v, 0, 0);
        break; }
    case 's': v->scx = v->cx; v->scy = v->cy; break;
    case 'u': vt_set_cursor(v, v->scy, v->scx); break;
    case 'n':
        if (p0 == 6 && v->reply) {
            char b[20]; int i = 0; char t[8]; int k;
            b[i++] = 27; b[i++] = '[';
            int_to_str(v->cy + 1, t); for (k = 0; t[k]; k++) b[i++] = t[k];
            b[i++] = ';';
            int_to_str(v->cx + 1, t); for (k = 0; t[k]; k++) b[i++] = t[k];
            b[i++] = 'R'; b[i] = 0;
            v->reply(v, b);
        }
        break;
    default: break;                                /* unsupported: ignore */
    }
}

void vt_putc(vt_t* v, uint8_t c) {
    v->dirty = true;
    switch (v->state) {
    case ST_ESC:
        if (c == '[') { v->state = ST_CSI; v->nparams = 0; v->params[0] = 0; v->priv = false; return; }
        v->state = ST_NORMAL;
        switch (c) {
        case '7': v->scx = v->cx; v->scy = v->cy; return;
        case '8': vt_set_cursor(v, v->scy, v->scx); return;
        case 'c': vt_init(v, v->cols, v->rows, v->paint, v->reply, v->owner); vt_repaint(v); return;
        case 'D': linefeed(v); return;
        case 'E': v->cx = 0; linefeed(v); return;
        case 'M': if (v->cy == v->top) scroll_down(v, v->top, v->bot, 1); else if (v->cy > 0) v->cy--; return;
        case '(': case ')': case '#': v->state = ST_SKIP1; return;   /* charset / line attrs: skip arg */
        default: return;                           /* ESC =, ESC >, ... ignored */
        }
    case ST_SKIP1:
        v->state = ST_NORMAL; return;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (v->nparams == 0) v->nparams = 1;
            int* p = &v->params[v->nparams - 1];
            if (*p < 10000) *p = *p * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (v->nparams == 0) v->nparams = 1;
            if (v->nparams < 8) { v->params[v->nparams] = 0; v->nparams++; }
            return;
        }
        if (c == '?') { v->priv = true; return; }
        if (c == '>' || c == '=' || c == '!' || c == '<') return;   /* private markers */
        if (c >= 0x20 && c < 0x40) return;                           /* intermediates */
        v->state = ST_NORMAL;
        if (c >= 0x40 && c <= 0x7E) csi_dispatch(v, c);
        return;
    default: break;
    }

    /* ST_NORMAL */
    switch (c) {
    case 27: v->state = ST_ESC; return;
    case '\r': v->cx = 0; v->wrap_pending = false; return;
    case '\n': case 11: case 12: linefeed(v); v->wrap_pending = false; return;
    case '\b': if (v->wrap_pending) v->wrap_pending = false; else if (v->cx > 0) v->cx--; return;
    case '\t': {
        v->wrap_pending = false;
        int nx = (v->cx + 8) & ~7;
        if (nx >= v->cols) nx = v->cols - 1;
        v->cx = nx; return; }
    case 7: return;                                /* bell */
    case 0: case 14: case 15: return;              /* NUL, SO, SI */
    default: break;
    }
    if (c < 32) return;                            /* other controls: ignore */

    if (v->wrap_pending) {                         /* deferred autowrap */
        v->wrap_pending = false;
        v->cx = 0;
        linefeed(v);
    }
    put_cell(v, v->cy, v->cx, c, v->attr);
    if (v->cx == v->cols - 1) v->wrap_pending = true;
    else v->cx++;
}

void vt_write(vt_t* v, const char* s, int n) {
    for (int i = 0; i < n; i++) vt_putc(v, (uint8_t)s[i]);
}

void vt_puts(vt_t* v, const char* s) {
    while (*s) vt_putc(v, (uint8_t)*s++);
}

int vt_row_text(const vt_t* v, int r, char* out, int max) {
    int n = 0;
    if (r < 0 || r >= v->rows || max <= 0) { if (max > 0) out[0] = 0; return 0; }
    int last = -1;
    for (int c = 0; c < v->cols; c++) if (v->ch[idx(v, r, c)] != ' ') last = c;
    for (int c = 0; c <= last && n < max - 1; c++) out[n++] = (char)v->ch[idx(v, r, c)];
    out[n] = 0;
    return n;
}
