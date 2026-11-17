/* ============================================================================
 * NexusOS — Aurora UI toolkit (Implementation)
 * ============================================================================
 * macOS-style rendering on the 32bpp back buffer. All coverage math is
 * integer: distances are kept in 1/16 px and turned into 0..256 coverage, so
 * edges are anti-aliased without touching the FPU.
 * ============================================================================ */

#include "macui.h"
#include "uifont.h"
#include "framebuffer.h"
#include "string.h"
#include "rtc.h"
#include "memory.h"

#ifndef WALLPAPER_CACHE_ADDR
#define WALLPAPER_CACHE_ADDR 0x01800000
#define WALLPAPER_CACHE_SIZE 0x00800000
#endif

/* --------------------------------------------------------------------------
 * Back buffer access
 * -------------------------------------------------------------------------- */
static uint32_t* BB;
static int SW, SH;

static inline bool bb(void) {
    BB = fb_get_backbuffer();
    SW = (int)fb_get_width();
    SH = (int)fb_get_height();
    return BB != NULL && fb_is_vesa();
}

static inline uint32_t mixc(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t rb = (((s & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8) & 0xFF00FF;
    uint32_t g  = (((s & 0x00FF00) * a + (d & 0x00FF00) * (256 - a)) >> 8) & 0x00FF00;
    return rb | g;
}

uint32_t ui_mix(uint32_t d, uint32_t s, int a) {
    if (a <= 0) return d;
    if (a >= 256) return s;
    return mixc(d, s, (uint32_t)a);
}

static inline void put(int x, int y, uint32_t c, uint32_t a) {
    uint32_t* p = &BB[y * SW + x];
    *p = a >= 256 ? c : mixc(*p, c, a);
}

static inline uint32_t alpha256(int alpha) {
    return alpha >= 255 ? 256 : alpha <= 0 ? 0 : (uint32_t)alpha;
}

static inline int iabs(int v) { return v < 0 ? -v : v; }

static uint32_t isqrt(uint32_t n) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

static uint32_t lerpc(uint32_t a, uint32_t b, int t, int n) {   /* t/n of the way */
    if (n <= 0) return a;
    if (t <= 0) return a;
    if (t >= n) return b;
    return mixc(a, b, (uint32_t)(t * 256 / n));
}

/* Sine of a (1024 = full turn), scaled by 4096. Bhaskara I approximation. */
static int isin(int a) {
    a &= 1023;
    int sign = 1;
    if (a >= 512) { a -= 512; sign = -1; }
    int p = a * (512 - a);
    int v = (16 * p) * 1024 / (5 * 512 * 512 - 4 * p);
    return sign * v * 4;
}
static int icos(int a) { return isin(a + 256); }

/* --------------------------------------------------------------------------
 * Text
 * -------------------------------------------------------------------------- */
static const uifont_t* const FONTS[UF_COUNT] = { &uf_ui, &uf_uib, &uf_small, &uf_title, &uf_huge };

static const uifont_glyph_t* glyph_of(const uifont_t* f, uint8_t c) {
    if (c < f->first || c >= f->first + f->count) return NULL;
    return &f->g[c - f->first];
}

static int missing_adv(const uifont_t* f) {          /* e.g. space in the clock font */
    const uifont_glyph_t* g = glyph_of(f, '0');
    return g ? g->adv_q4 / 2 : 16;
}

static void blit_glyph(const uifont_t* f, const uifont_glyph_t* g, int gx, int gy, uint32_t color) {
    const uint8_t* bits = f->bits + g->off;
    int bw = (g->w + 1) >> 1;
    for (int r = 0; r < g->h; r++) {
        int y = gy + r;
        if (y < 0 || y >= SH) continue;
        const uint8_t* row = bits + r * bw;
        for (int c = 0; c < g->w; c++) {
            int x = gx + c;
            if (x < 0 || x >= SW) continue;
            uint32_t a4 = (c & 1) ? (row[c >> 1] >> 4) : (row[c >> 1] & 15);
            if (a4) put(x, y, color, a4 * 17 + (a4 >> 3));
        }
    }
}

int ui_text_n(int x, int y, const char* s, int n, int font, uint32_t color) {
    if (font < 0 || font >= UF_COUNT || !bb()) return x;
    const uifont_t* f = FONTS[font];
    int pen = x << 2;
    for (int i = 0; s[i] && i < n; i++) {
        const uifont_glyph_t* g = glyph_of(f, (uint8_t)s[i]);
        if (!g) { pen += missing_adv(f); continue; }
        if (g->w) blit_glyph(f, g, ((pen + 2) >> 2) + g->xoff, y + f->ascent + g->yoff, color);
        pen += g->adv_q4;
    }
    return (pen + 3) >> 2;
}

int ui_text(int x, int y, const char* s, int font, uint32_t color) {
    return ui_text_n(x, y, s, 0x7FFFFFFF, font, color);
}

int ui_text_width(const char* s, int font) {
    if (font < 0 || font >= UF_COUNT) return 0;
    const uifont_t* f = FONTS[font];
    int q = 0;
    for (; *s; s++) {
        const uifont_glyph_t* g = glyph_of(f, (uint8_t)*s);
        q += g ? g->adv_q4 : missing_adv(f);
    }
    return (q + 3) >> 2;
}

int ui_font_height(int font) {
    return (font >= 0 && font < UF_COUNT) ? FONTS[font]->line : 16;
}

void ui_text_center(int cx, int y, const char* s, int font, uint32_t color) {
    ui_text(cx - ui_text_width(s, font) / 2, y, s, font, color);
}

bool ui_cell_glyph(int px, int py, uint8_t c, uint32_t fg, uint32_t bg) {
    if (c < 32 || c > 126) return false;
    if (!bb()) return true;
    if (px < 0 || py < 0 || px + 8 > SW || py + 16 > SH) return true;
    const uint8_t* g = uf_cell_bits + (c - 32) * 64;
    uint32_t* dst = &BB[py * SW + px];
    for (int r = 0; r < 16; r++, dst += SW, g += 4) {
        for (int k = 0; k < 4; k++) {
            uint32_t a0 = g[k] & 15, a1 = g[k] >> 4;
            dst[2 * k]     = !a0 ? bg : a0 == 15 ? fg : mixc(bg, fg, a0 * 17 + 1);
            dst[2 * k + 1] = !a1 ? bg : a1 == 15 ? fg : mixc(bg, fg, a1 * 17 + 1);
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Shapes
 * -------------------------------------------------------------------------- */
void ui_blend_rect(int x, int y, int w, int h, uint32_t color, int alpha) {
    uint32_t a = alpha256(alpha);
    if (!a || !bb()) return;
    int x0 = x < 0 ? 0 : x, x1 = x + w > SW ? SW : x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h > SH ? SH : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = &BB[yy * SW];
        for (int xx = x0; xx < x1; xx++) row[xx] = a >= 256 ? color : mixc(row[xx], color, a);
    }
}

/* Coverage (0..256) of local pixel (i,j) in a w*h rect with corner radius r. */
static int rr_cov(int i, int j, int w, int h, int r) {
    int dx, dy;
    if (i < r) dx = r * 16 - (i * 16 + 8);
    else if (i >= w - r) dx = (i * 16 + 8) - (w - r) * 16;
    else return 256;
    if (j < r) dy = r * 16 - (j * 16 + 8);
    else if (j >= h - r) dy = (j * 16 + 8) - (h - r) * 16;
    else return 256;
    if (dx <= 0 || dy <= 0) return 256;
    int d2 = dx * dx + dy * dy, in = r * 16 - 8;
    if (in > 0 && d2 <= in * in) return 256;
    int c = (r * 16 + 8 - (int)isqrt((uint32_t)d2)) * 16;
    return c <= 0 ? 0 : c >= 256 ? 256 : c;
}

static void rrect_core(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int alpha) {
    uint32_t A = alpha256(alpha);
    if (!A || w <= 0 || h <= 0 || !bb()) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int x0 = x < 0 ? 0 : x, x1 = x + w > SW ? SW : x + w;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0) continue;
        if (yy >= SH) break;
        uint32_t color = top == bot ? top : lerpc(top, bot, j, h - 1);
        bool band = (j < r || j >= h - r);
        uint32_t* row = &BB[yy * SW];
        for (int xx = x0; xx < x1; xx++) {
            int i = xx - x;
            uint32_t a = A;
            if (band && (i < r || i >= w - r)) {
                int c = rr_cov(i, j, w, h, r);
                if (!c) continue;
                a = (a * (uint32_t)c) >> 8;
            }
            row[xx] = a >= 256 ? color : mixc(row[xx], color, a);
        }
    }
}

void ui_rrect(int x, int y, int w, int h, int r, uint32_t color, int alpha) {
    rrect_core(x, y, w, h, r, color, color, alpha);
}

void ui_rrect_vgrad(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int alpha) {
    rrect_core(x, y, w, h, r, top, bot, alpha);
}

void ui_glass(int x, int y, int w, int h, int r, uint32_t tint, int alpha) {
    rrect_core(x, y, w, h, r, tint, tint, alpha);
}

void ui_rrect_stroke(int x, int y, int w, int h, int r, uint32_t color, int alpha) {
    uint32_t A = alpha256(alpha);
    if (!A || w <= 2 || h <= 2 || !bb()) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int ri = r > 0 ? r - 1 : 0;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= SH) continue;
        bool band = (j <= r || j >= h - r - 1);
        for (int i = 0; i < w; i++) {
            if (!band && i > r && i < w - r - 1) { i = w - r - 2; continue; }
            int xx = x + i;
            if (xx < 0 || xx >= SW) continue;
            int co = rr_cov(i, j, w, h, r);
            int ci = (i >= 1 && j >= 1 && i < w - 1 && j < h - 1) ? rr_cov(i - 1, j - 1, w - 2, h - 2, ri) : 0;
            int c = co - ci;
            if (c > 0) put(xx, yy, color, (A * (uint32_t)c) >> 8);
        }
    }
}

/* Coverage of a pixel against a circle centred at (cx16,cy16) radius r16. */
static int circ_cov(int px, int py, int cx16, int cy16, int r16) {
    int dx = px * 16 + 8 - cx16, dy = py * 16 + 8 - cy16;
    int d2 = dx * dx + dy * dy;
    int in = r16 - 8, out = r16 + 8;
    if (d2 >= out * out) return 0;
    if (in > 0 && d2 <= in * in) return 256;
    int c = (r16 + 8 - (int)isqrt((uint32_t)d2)) * 16;
    return c <= 0 ? 0 : c >= 256 ? 256 : c;
}

void ui_circle(int cx, int cy, int r, uint32_t color, int alpha) {
    uint32_t A = alpha256(alpha);
    if (!A || r <= 0 || !bb()) return;
    int cx16 = cx * 16 + 8, cy16 = cy * 16 + 8, r16 = r * 16;
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        if (y < 0 || y >= SH) continue;
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < 0 || x >= SW) continue;
            int c = circ_cov(x, y, cx16, cy16, r16);
            if (c) put(x, y, color, (A * (uint32_t)c) >> 8);
        }
    }
}

void ui_ring(int cx, int cy, int r, int thick, uint32_t color, int alpha) {
    uint32_t A = alpha256(alpha);
    if (!A || r <= 0 || !bb()) return;
    int cx16 = cx * 16 + 8, cy16 = cy * 16 + 8, r16 = r * 16, ri16 = (r - thick) * 16;
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        if (y < 0 || y >= SH) continue;
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < 0 || x >= SW) continue;
            int c = circ_cov(x, y, cx16, cy16, r16) - (ri16 > 0 ? circ_cov(x, y, cx16, cy16, ri16) : 0);
            if (c > 0) put(x, y, color, (A * (uint32_t)c) >> 8);
        }
    }
}

/* Upper sector of a ring (Wi-Fi style arc), ~55 degrees either side of up. */
void ui_arc_up(int cx, int cy, int r, int thick, uint32_t color, int alpha) {
    uint32_t A = alpha256(alpha);
    if (!A || r <= 0 || !bb()) return;
    int cx16 = cx * 16 + 8, cy16 = cy * 16 + 8, r16 = r * 16, ri16 = (r - thick) * 16;
    for (int y = cy - r - 1; y <= cy; y++) {
        if (y < 0 || y >= SH) continue;
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < 0 || x >= SW) continue;
            int dx = iabs(x - cx), dy = cy - y;
            if (dy * 16 < dx * 11) continue;
            int c = circ_cov(x, y, cx16, cy16, r16) - (ri16 > 0 ? circ_cov(x, y, cx16, cy16, ri16) : 0);
            if (c > 0) put(x, y, color, (A * (uint32_t)c) >> 8);
        }
    }
}

/* Anti-aliased capsule (thick line with round caps). Meant for short strokes
 * (< ~1000 px) so the 1/16 px products stay inside 32 bits. */
void ui_line(int x0, int y0, int x1, int y1, int thick, uint32_t color, int alpha) {
    uint32_t A = alpha256(alpha);
    if (!A || thick <= 0 || !bb()) return;
    int ax = x0 * 16 + 8, ay = y0 * 16 + 8, bx = x1 * 16 + 8, by = y1 * 16 + 8;
    int vx = bx - ax, vy = by - ay;
    int L2 = vx * vx + vy * vy, L = (int)isqrt((uint32_t)L2);
    int hr = thick * 8;
    int pad = thick / 2 + 2;
    int minx = (x0 < x1 ? x0 : x1) - pad, maxx = (x0 > x1 ? x0 : x1) + pad;
    int miny = (y0 < y1 ? y0 : y1) - pad, maxy = (y0 > y1 ? y0 : y1) + pad;
    for (int y = miny; y <= maxy; y++) {
        if (y < 0 || y >= SH) continue;
        for (int x = minx; x <= maxx; x++) {
            if (x < 0 || x >= SW) continue;
            int wx = x * 16 + 8 - ax, wy = y * 16 + 8 - ay;
            int t = wx * vx + wy * vy, d;
            if (L2 == 0 || t <= 0) d = (int)isqrt((uint32_t)(wx * wx + wy * wy));
            else if (t >= L2) {
                int ex = x * 16 + 8 - bx, ey = y * 16 + 8 - by;
                d = (int)isqrt((uint32_t)(ex * ex + ey * ey));
            } else d = iabs(wx * vy - wy * vx) / (L ? L : 1);
            int c = (hr + 8 - d) * 16;
            if (c <= 0) continue;
            if (c > 256) c = 256;
            put(x, y, color, (A * (uint32_t)c) >> 8);
        }
    }
}

/* Filled triangle, 4x4 supersampled. */
void ui_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, int alpha) {
    uint32_t A = alpha256(alpha);
    if (!A || !bb()) return;
    int ax = x0 * 8, ay = y0 * 8, bx = x1 * 8, by = y1 * 8, cx = x2 * 8, cy = y2 * 8;
    int area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) return;
    int sgn = area > 0 ? 1 : -1;
    int minx = x0 < x1 ? x0 : x1, maxx = x0 > x1 ? x0 : x1;
    int miny = y0 < y1 ? y0 : y1, maxy = y0 > y1 ? y0 : y1;
    minx = x2 < minx ? x2 : minx;  maxx = x2 > maxx ? x2 : maxx;
    miny = y2 < miny ? y2 : miny;  maxy = y2 > maxy ? y2 : maxy;
    for (int y = miny; y <= maxy; y++) {
        if (y < 0 || y >= SH) continue;
        for (int x = minx; x <= maxx; x++) {
            if (x < 0 || x >= SW) continue;
            int n = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                int px = x * 8 + sx * 2 + 1, py = y * 8 + sy * 2 + 1;
                int e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                int e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
                int e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
                if (e0 * sgn >= 0 && e1 * sgn >= 0 && e2 * sgn >= 0) n++;
            }
            if (n) put(x, y, color, (A * (uint32_t)(n * 16)) >> 8);
        }
    }
}

/* Soft drop shadow around a rounded rect. The window drawn afterwards covers
 * the middle, so only the margin (and the corner bands) is computed. */
void ui_shadow(int x, int y, int w, int h, int r, int spread, int alpha, int dy) {
    if (spread <= 0 || alpha <= 0 || !bb()) return;
    int sy = y + dy;
    int cx16 = x * 16 + w * 8, cy16 = sy * 16 + h * 8;
    int hx = w * 8 - r * 16, hy = h * 8 - r * 16;
    if (hx < 0) hx = 0;
    if (hy < 0) hy = 0;
    int s16 = spread * 16, r16 = r * 16;
    for (int yy = sy - spread; yy < sy + h + spread; yy++) {
        if (yy < 0 || yy >= SH) continue;
        int qy = iabs(yy * 16 + 8 - cy16) - hy;
        if (qy < 0) qy = 0;
        bool covered_row = (yy >= y + r && yy < y + h - r);
        uint32_t* row = &BB[yy * SW];
        for (int xx = x - spread; xx < x + w + spread; xx++) {
            if (covered_row && xx >= x && xx < x + w) { xx = x + w - 1; continue; }
            if (xx < 0 || xx >= SW) continue;
            int qx = iabs(xx * 16 + 8 - cx16) - hx;
            if (qx < 0) qx = 0;
            int d = (!qx) ? qy : (!qy) ? qx : (int)isqrt((uint32_t)(qx * qx + qy * qy));
            d -= r16;
            if (d >= s16) continue;
            int a;
            if (d <= 0) a = alpha;
            else { int t = s16 - d; a = alpha * t / s16; a = a * t / s16; }
            if (a > 0) row[xx] = mixc(row[xx], 0, (uint32_t)a);
        }
    }
}

/* --------------------------------------------------------------------------
 * Wallpaper: a dusk sky over three rolling hills (Sonoma-like), with a warm
 * glow at the horizon and faint stars. Rendered once into a reserved cache.
 * -------------------------------------------------------------------------- */
#define WP_MAXW 2048
static uint32_t* const wp_cache = (uint32_t*)WALLPAPER_CACHE_ADDR;
static bool wp_ready = false;
static int  wp_w = 0, wp_h = 0;
static int16_t hill_crest[3][WP_MAXW];

typedef struct { int base, amp, f, ph, amp2, f2, ph2; uint32_t top, bot, rim; int rim_a, span; } hill_t;
static const hill_t HILLS[3] = {
    { 590, 55, 850, 200, 18, 2700, 600, UI_RGB(150, 92, 178), UI_RGB(78, 44, 128),  UI_RGB(255, 190, 210), 150, 300 },
    { 715, 60, 650, 700, 14, 2100, 100, UI_RGB(96, 56, 150),  UI_RGB(40, 24, 90),   UI_RGB(230, 150, 220), 120, 280 },
    { 840, 50, 550, 400, 12, 1700, 900, UI_RGB(52, 30, 104),  UI_RGB(14, 10, 38),   UI_RGB(170, 120, 230), 100, 200 },
};

static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

/* Sky colour at row y as 8.8 fixed channels. */
static void sky_at(int y, int h, int* r, int* g, int* b) {
    static const int stop[5] = { 0, 380, 600, 700, 1000 };
    static const uint8_t col[5][3] = {
        { 10, 14, 40 }, { 38, 30, 92 }, { 120, 58, 140 }, { 236, 120, 128 }, { 236, 120, 128 } };
    int f = y * 1000 / h, k = 0;
    while (k < 3 && f > stop[k + 1]) k++;
    int t = (f - stop[k]) * 256 / (stop[k + 1] - stop[k]);
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    *r = col[k][0] * 256 + (col[k + 1][0] - col[k][0]) * t;
    *g = col[k][1] * 256 + (col[k + 1][1] - col[k][1]) * t;
    *b = col[k][2] * 256 + (col[k + 1][2] - col[k][2]) * t;
}

void ui_wallpaper_init(void) {
    if (!bb()) return;
    wp_w = SW; wp_h = SH;
    if (SW > WP_MAXW || (uint32_t)SW * (uint32_t)SH * 4u > WALLPAPER_CACHE_SIZE) { wp_ready = false; return; }

    for (int k = 0; k < 3; k++) {
        const hill_t* hl = &HILLS[k];
        int base16 = hl->base * SH * 16 / 1000;
        int amp16 = hl->amp * SH * 16 / 1000, amp216 = hl->amp2 * SH * 16 / 1000;
        for (int x = 0; x < SW; x++) {
            int u = x * 1024 / SW;
            int a1 = u * hl->f / 1000 + hl->ph, a2 = u * hl->f2 / 1000 + hl->ph2;
            hill_crest[k][x] = (int16_t)(base16 + amp16 * isin(a1) / 4096 + amp216 * isin(a2) / 4096);
        }
    }

    int gx = SW * 66 / 100, gy = SH * 64 / 100, gR = SW * 32 / 100;
    int gR2 = gR * gR;
    for (int y = 0; y < SH; y++) {
        int sr, sg, sb;
        sky_at(y, SH, &sr, &sg, &sb);
        int y16 = y * 16 + 8;
        uint32_t* out = wp_cache + y * SW;
        for (int x = 0; x < SW; x++) {
            int r = sr, g = sg, b = sb;
            /* horizon glow */
            int dx = x - gx, dy = (y - gy) * 2;
            int d2 = dx * dx + dy * dy;
            if (d2 < gR2) {
                int w = 256 - d2 / (gR2 / 256 + 1);           /* 0..256 */
                w = w * w / 256 * 115 / 256;                  /* soft, ~45% peak */
                r += (255 * 256 - r) * w / 256;
                g += (170 * 256 - g) * w / 256;
                b += (140 * 256 - b) * w / 256;
            }
            /* hills, back to front */
            for (int k = 0; k < 3; k++) {
                int c16 = hill_crest[k][x];
                if (y16 < c16 - 8) continue;
                const hill_t* hl = &HILLS[k];
                int cover = (y16 - c16 + 8) * 16;
                if (cover > 256) cover = 256;
                int depth = (y16 - c16) / 16;
                if (depth < 0) depth = 0;
                uint32_t hc = lerpc(hl->top, hl->bot, depth, hl->span * SH / 1000);
                if (depth < 14) hc = mixc(hc, hl->rim, (uint32_t)((14 - depth) * hl->rim_a / 14));
                int hr = (int)((hc >> 16) & 0xFF) * 256, hg = (int)((hc >> 8) & 0xFF) * 256, hb = (int)(hc & 0xFF) * 256;
                r += (hr - r) * cover / 256;
                g += (hg - g) * cover / 256;
                b += (hb - b) * cover / 256;
            }
            int dith = BAYER[(y & 3) * 4 + (x & 3)] * 16 + 8;
            int R = (r + dith) >> 8, G = (g + dith) >> 8, B = (b + dith) >> 8;
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            out[x] = (uint32_t)((R << 16) | (G << 8) | B);
        }
    }

    /* faint stars in the upper sky */
    uint32_t seed = 0x2545F491u;
    for (int i = 0; i < 320; i++) {
        seed = seed * 1103515245u + 12345u; int x = (int)((seed >> 8) % (uint32_t)SW);
        seed = seed * 1103515245u + 12345u; int y = (int)((seed >> 8) % (uint32_t)(SH * 45 / 100));
        seed = seed * 1103515245u + 12345u; int a = 40 + (int)((seed >> 8) % 150);
        a = a * (SH * 45 / 100 - y) / (SH * 45 / 100);
        uint32_t* p = &wp_cache[y * SW + x];
        *p = mixc(*p, 0xFFFFFF, (uint32_t)a);
        if (i % 9 == 0 && x + 1 < SW && y + 1 < SH) {
            p[1] = mixc(p[1], 0xFFFFFF, (uint32_t)a / 2);
            p[SW] = mixc(p[SW], 0xFFFFFF, (uint32_t)a / 2);
        }
    }
    wp_ready = true;
}

void ui_wallpaper_rect(int x, int y, int w, int h) {
    if (!bb()) return;
    if (!wp_ready || wp_w != SW || wp_h != SH) ui_wallpaper_init();
    int x0 = x < 0 ? 0 : x, x1 = x + w > SW ? SW : x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h > SH ? SH : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; yy++) {
        if (wp_ready) memcpy(&BB[yy * SW + x0], &wp_cache[yy * SW + x0], (size_t)(x1 - x0) * 4);
        else {
            int r, g, b;
            sky_at(yy, SH, &r, &g, &b);
            uint32_t c = (uint32_t)(((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8));
            for (int xx = x0; xx < x1; xx++) BB[yy * SW + xx] = c;
        }
    }
}

void ui_wallpaper_draw(void) { ui_wallpaper_rect(0, 0, 1 << 15, 1 << 15); }

uint32_t ui_wallpaper_at(int x, int y) {
    if (!wp_ready || x < 0 || y < 0 || x >= wp_w || y >= wp_h) return 0;
    return wp_cache[y * wp_w + x];
}

/* --------------------------------------------------------------------------
 * Pointer
 * -------------------------------------------------------------------------- */
void ui_cursor_draw(int x, int y) {
    if (!bb()) return;
    x -= 1; y -= 1;
    for (int j = 0; j < UF_CURSOR_H; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= SH) continue;
        for (int i = 0; i < UF_CURSOR_W; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= SW) continue;
            uint32_t p = uf_cursor[j * UF_CURSOR_W + i], a = p >> 24;
            if (a) put(xx, yy, p & 0xFFFFFF, a + (a >> 7));
        }
    }
}

/* --------------------------------------------------------------------------
 * Misc artwork
 * -------------------------------------------------------------------------- */
void ui_logo(int x, int y, int h, uint32_t color) {
    int t = h * 17 / 100;
    if (t < 2) t = 2;
    int w = h * 82 / 100, o = t / 2;
    ui_line(x + o, y + h - o, x + o, y + o, t, color, 255);
    ui_line(x + o, y + o, x + w - o, y + h - o, t, color, 255);
    ui_line(x + w - o, y + h - o, x + w - o, y + o, t, color, 255);
}

void ui_avatar(int cx, int cy, int r) {
    if (r <= 0 || !bb()) return;
    int cx16 = cx * 16 + 8, cy16 = cy * 16 + 8, r16 = r * 16;
    int hy16 = cy16 - r16 * 22 / 100, hr16 = r16 * 34 / 100;
    int sy16 = cy16 + r16 * 80 / 100, sr16 = r16 * 60 / 100;
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        if (y < 0 || y >= SH) continue;
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < 0 || x >= SW) continue;
            int c = circ_cov(x, y, cx16, cy16, r16);
            if (!c) continue;
            uint32_t col = lerpc(UI_RGB(172, 172, 182), UI_RGB(112, 112, 124), y - (cy - r), 2 * r);
            int s = circ_cov(x, y, cx16, hy16, hr16), s2 = circ_cov(x, y, cx16, sy16, sr16);
            if (s2 > s) s = s2;
            if (s) col = mixc(col, UI_RGB(236, 236, 242), (uint32_t)s);
            put(x, y, col, (uint32_t)c);
        }
    }
}

static int weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static const char* const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void ui_format_clock(char* out) {
    rtc_time_t t;
    rtc_read(&t);
    int m = (t.month >= 1 && t.month <= 12) ? t.month : 1;
    char n[12];
    strcpy(out, WDAY[weekday(t.year, m, t.day)]);
    strcat(out, " ");
    int_to_str(t.day, n); strcat(out, n);
    strcat(out, " ");
    strcat(out, MON[m - 1]);
    strcat(out, "  ");
    if (t.hour < 10) strcat(out, "0");
    int_to_str(t.hour, n); strcat(out, n);
    strcat(out, ":");
    if (t.minute < 10) strcat(out, "0");
    int_to_str(t.minute, n); strcat(out, n);
}

/* --------------------------------------------------------------------------
 * App icons (Big Sur style squircles, drawn procedurally at any size)
 * -------------------------------------------------------------------------- */
#define P(v) ((v) * s / 100)

static void squircle(int x, int y, int s, uint32_t top, uint32_t bot) {
    ui_rrect_vgrad(x, y, s, s, s * 23 / 100, top, bot, 255);
}

/* Squircle whose colour comes from a per-pixel shader (local u,v). */
typedef uint32_t (*shade_fn)(int u, int v, int s);
static void squircle_shaded(int x, int y, int s, shade_fn fn) {
    if (!bb()) return;
    int r = s * 23 / 100;
    for (int j = 0; j < s; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= SH) continue;
        for (int i = 0; i < s; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= SW) continue;
            int c = rr_cov(i, j, s, s, r);
            if (c) put(xx, yy, fn(i, j, s), (uint32_t)c);
        }
    }
}

static uint32_t shade_finder(int u, int v, int s) {
    int edge = s * 52 / 100;
    if (v > s * 34 / 100 && v < s * 64 / 100) {           /* nose bump */
        int m = s * 49 / 100, d = iabs(v - m);
        edge -= (s * 15 / 100 - d) * 6 / 10;
    }
    return u < edge ? lerpc(UI_RGB(160, 225, 255), UI_RGB(95, 190, 252), v, s)
                    : lerpc(UI_RGB(40, 140, 245), UI_RGB(12, 88, 214), v, s);
}

static uint32_t shade_notes(int u, int v, int s) {
    (void)u;
    if (v < s * 27 / 100) return lerpc(UI_RGB(255, 220, 100), UI_RGB(250, 192, 45), v, s * 27 / 100);
    int lv = (v - s * 27 / 100) % (s * 14 / 100 + 1);
    if (u > s * 12 / 100 && u < s * 88 / 100 && lv == s * 12 / 100) return UI_RGB(206, 206, 212);
    return UI_RGB(252, 252, 250);
}

static void draw_glyph_text(int cx, int y, const char* t, int font, uint32_t c) {
    ui_text_center(cx, y, t, font, c);
}

void ui_icon(int icon, int x, int y, int s) {
    if (s < 8 || !bb()) return;
    int c = s / 2;
    switch (icon) {
    case ICON_FINDER:
        squircle_shaded(x, y, s, shade_finder);
        ui_line(x + P(33), y + P(30), x + P(33), y + P(42), P(6) + 1, UI_RGB(18, 38, 84), 255);
        ui_line(x + P(70), y + P(30), x + P(70), y + P(42), P(6) + 1, UI_RGB(18, 38, 84), 255);
        ui_line(x + P(24), y + P(66), x + P(38), y + P(75), P(4) + 1, UI_RGB(18, 38, 84), 255);
        ui_line(x + P(38), y + P(75), x + P(56), y + P(77), P(4) + 1, UI_RGB(18, 38, 84), 255);
        ui_line(x + P(56), y + P(77), x + P(78), y + P(64), P(4) + 1, UI_RGB(18, 38, 84), 255);
        break;
    case ICON_LAUNCHPAD: {
        static const uint32_t cols[9] = {
            UI_RGB(255, 84, 84), UI_RGB(255, 160, 60), UI_RGB(255, 212, 60),
            UI_RGB(80, 200, 110), UI_RGB(60, 200, 210), UI_RGB(64, 140, 255),
            UI_RGB(160, 104, 240), UI_RGB(255, 104, 180), UI_RGB(130, 130, 142) };
        squircle(x, y, s, UI_RGB(232, 232, 238), UI_RGB(166, 166, 178));
        for (int k = 0; k < 9; k++) {
            int gx = x + P(20) + (k % 3) * P(22), gy = y + P(20) + (k / 3) * P(22);
            ui_rrect(gx, gy, P(17), P(17), P(5), cols[k], 255);
        }
        break; }
    case ICON_WEB:
        squircle(x, y, s, UI_RGB(250, 250, 252), UI_RGB(214, 214, 222));
        ui_circle(x + c, y + c, P(39), UI_RGB(30, 140, 250), 255);
        ui_circle(x + c, y + c - P(3), P(33), UI_RGB(70, 175, 255), 120);
        ui_ring(x + c, y + c, P(39), 1, UI_RGB(255, 255, 255), 200);
        for (int k = 0; k < 16; k++) {
            int a = k * 64, r1 = P(34), r2 = (k & 3) ? P(31) : P(28);
            ui_line(x + c + icos(a) * r1 / 4096, y + c + isin(a) * r1 / 4096,
                    x + c + icos(a) * r2 / 4096, y + c + isin(a) * r2 / 4096, 1, 0xFFFFFF, 170);
        }
        ui_tri(x + c + P(24), y + c - P(24), x + c - P(5), y + c - P(5), x + c + P(5), y + c + P(5), UI_RGB(255, 59, 48), 255);
        ui_tri(x + c - P(24), y + c + P(24), x + c - P(5), y + c - P(5), x + c + P(5), y + c + P(5), UI_RGB(245, 245, 247), 255);
        break;
    case ICON_TERMINAL:
        squircle(x, y, s, UI_RGB(232, 232, 236), UI_RGB(178, 178, 186));
        ui_rrect(x + P(9), y + P(11), P(82), P(78), P(12), UI_RGB(28, 28, 30), 255);
        ui_line(x + P(24), y + P(34), x + P(38), y + P(46), P(5) + 1, 0xFFFFFF, 255);
        ui_line(x + P(38), y + P(46), x + P(24), y + P(58), P(5) + 1, 0xFFFFFF, 255);
        ui_line(x + P(44), y + P(59), x + P(62), y + P(59), P(5) + 1, 0xFFFFFF, 255);
        break;
    case ICON_NOTES:
        squircle_shaded(x, y, s, shade_notes);
        break;
    case ICON_CALENDAR: {
        rtc_time_t t; rtc_read(&t);
        char d[4];
        int m = (t.month >= 1 && t.month <= 12) ? t.month : 1;
        int_to_str(t.day, d);
        squircle(x, y, s, UI_RGB(255, 255, 255), UI_RGB(238, 238, 240));
        draw_glyph_text(x + c, y + P(9), MON[m - 1], UF_SMALL, UI_RGB(240, 60, 50));
        draw_glyph_text(x + c, y + P(30), d, UF_TITLE, UI_RGB(40, 40, 44));
        break; }
    case ICON_MUSIC:
        squircle(x, y, s, UI_RGB(255, 104, 132), UI_RGB(240, 40, 86));
        ui_circle(x + P(34), y + P(68), P(10), 0xFFFFFF, 255);
        ui_circle(x + P(66), y + P(60), P(10), 0xFFFFFF, 255);
        ui_line(x + P(42), y + P(68), x + P(42), y + P(30), P(5) + 1, 0xFFFFFF, 255);
        ui_line(x + P(74), y + P(60), x + P(74), y + P(22), P(5) + 1, 0xFFFFFF, 255);
        ui_line(x + P(42), y + P(30), x + P(74), y + P(22), P(9) + 1, 0xFFFFFF, 255);
        break;
    case ICON_CALC:
        squircle(x, y, s, UI_RGB(92, 92, 98), UI_RGB(46, 46, 50));
        ui_circle(x + P(32), y + P(32), P(15), UI_RGB(165, 165, 170), 255);
        ui_circle(x + P(68), y + P(32), P(15), UI_RGB(255, 159, 10), 255);
        ui_circle(x + P(32), y + P(68), P(15), UI_RGB(110, 110, 116), 255);
        ui_circle(x + P(68), y + P(68), P(15), UI_RGB(255, 159, 10), 255);
        ui_line(x + P(25), y + P(32), x + P(39), y + P(32), P(3) + 1, UI_RGB(30, 30, 32), 255);
        ui_line(x + P(61), y + P(32), x + P(75), y + P(32), P(3) + 1, 0xFFFFFF, 255);
        ui_line(x + P(68), y + P(25), x + P(68), y + P(39), P(3) + 1, 0xFFFFFF, 255);
        ui_line(x + P(26), y + P(68), x + P(38), y + P(68), P(3) + 1, 0xFFFFFF, 255);
        ui_line(x + P(62), y + P(65), x + P(74), y + P(65), P(3) + 1, 0xFFFFFF, 255);
        ui_line(x + P(62), y + P(71), x + P(74), y + P(71), P(3) + 1, 0xFFFFFF, 255);
        break;
    case ICON_MONITOR: {
        static const int8_t gy[8] = { 62, 56, 66, 38, 58, 46, 52, 30 };
        squircle(x, y, s, UI_RGB(52, 52, 56), UI_RGB(18, 18, 20));
        for (int k = 1; k < 4; k++) ui_line(x + P(12), y + P(k * 22 + 4), x + P(88), y + P(k * 22 + 4), 1, UI_RGB(70, 70, 76), 255);
        for (int k = 0; k < 7; k++)
            ui_line(x + P(14 + k * 10), y + P(gy[k]), x + P(24 + k * 10), y + P(gy[k + 1]), P(4) + 1, UI_RGB(52, 215, 96), 255);
        break; }
    case ICON_SETTINGS:
        squircle(x, y, s, UI_RGB(206, 206, 212), UI_RGB(128, 128, 136));
        for (int k = 0; k < 8; k++) {
            int a = k * 128;
            ui_line(x + c + icos(a) * P(20) / 4096, y + c + isin(a) * P(20) / 4096,
                    x + c + icos(a) * P(35) / 4096, y + c + isin(a) * P(35) / 4096, P(11) + 1, UI_RGB(72, 72, 80), 255);
        }
        ui_circle(x + c, y + c, P(27), UI_RGB(72, 72, 80), 255);
        ui_circle(x + c, y + c, P(12), UI_RGB(176, 176, 184), 255);
        break;
    case ICON_TRASH:
        ui_rrect(x + P(22), y + P(26), P(56), P(66), P(9), UI_RGB(230, 230, 236), 90);
        ui_rrect_stroke(x + P(22), y + P(26), P(56), P(66), P(9), UI_RGB(245, 245, 250), 170);
        for (int k = 0; k < 3; k++) ui_line(x + P(37 + k * 13), y + P(36), x + P(37 + k * 13), y + P(82), 1, UI_RGB(240, 240, 245), 140);
        ui_rrect(x + P(15), y + P(15), P(70), P(10), P(4), UI_RGB(236, 236, 242), 190);
        ui_rrect(x + P(40), y + P(8), P(20), P(8), P(3), UI_RGB(236, 236, 242), 190);
        break;
    case ICON_CLOCK:
        squircle(x, y, s, UI_RGB(56, 56, 60), UI_RGB(24, 24, 27));
        ui_circle(x + c, y + c, P(38), UI_RGB(250, 250, 250), 255);
        for (int k = 0; k < 12; k++) {
            int a = k * 1024 / 12;
            ui_circle(x + c + icos(a) * P(32) / 4096, y + c + isin(a) * P(32) / 4096, P(2) > 0 ? P(2) : 1, UI_RGB(60, 60, 64), 255);
        }
        ui_line(x + c, y + c, x + c - P(14), y + c - P(10), P(5) + 1, UI_RGB(30, 30, 32), 255);
        ui_line(x + c, y + c, x + c + P(16), y + c - P(20), P(4) + 1, UI_RGB(30, 30, 32), 255);
        ui_line(x + c, y + c, x + c + P(6), y + c + P(26), 1, UI_RGB(255, 59, 48), 255);
        ui_circle(x + c, y + c, P(3) + 1, UI_RGB(255, 59, 48), 255);
        break;
    case ICON_TASKS:
        squircle(x, y, s, UI_RGB(84, 164, 255), UI_RGB(28, 108, 236));
        for (int k = 0; k < 3; k++) {
            ui_circle(x + P(26), y + P(32 + k * 18), P(5) + 1, 0xFFFFFF, 255);
            ui_line(x + P(40), y + P(32 + k * 18), x + P(76), y + P(32 + k * 18), P(7) + 1, 0xFFFFFF, 230);
        }
        break;
    case ICON_HELP:
        squircle(x, y, s, UI_RGB(255, 216, 84), UI_RGB(255, 168, 36));
        ui_circle(x + c, y + P(42), P(20), 0xFFFFFF, 255);
        ui_rrect(x + P(41), y + P(58), P(18), P(15), P(4), UI_RGB(250, 250, 250), 255);
        ui_line(x + P(43), y + P(77), x + P(57), y + P(77), P(3) + 1, UI_RGB(120, 90, 30), 255);
        break;
    case ICON_INFO:
        squircle(x, y, s, UI_RGB(214, 214, 220), UI_RGB(150, 150, 160));
        for (int k = 0; k < 4; k++) {
            int o = P(34) + k * P(10);
            ui_line(x + o, y + P(18), x + o, y + P(82), P(3) + 1, UI_RGB(214, 176, 92), 255);
            ui_line(x + P(18), y + o, x + P(82), y + o, P(3) + 1, UI_RGB(214, 176, 92), 255);
        }
        ui_rrect(x + P(27), y + P(27), P(46), P(46), P(8), UI_RGB(40, 40, 46), 255);
        ui_rrect_stroke(x + P(27), y + P(27), P(46), P(46), P(8), UI_RGB(90, 90, 98), 255);
        break;
    case ICON_REMINDERS: {
        static const uint32_t rc[3] = { UI_RGB(10, 132, 255), UI_RGB(255, 69, 58), UI_RGB(255, 159, 10) };
        squircle(x, y, s, UI_RGB(255, 255, 255), UI_RGB(236, 236, 240));
        for (int k = 0; k < 3; k++) {
            ui_ring(x + P(26), y + P(30 + k * 20), P(7) + 1, P(2) + 1, rc[k], 255);
            ui_line(x + P(40), y + P(30 + k * 20), x + P(78), y + P(30 + k * 20), P(3) + 1, UI_RGB(196, 196, 202), 255);
        }
        break; }
    case ICON_CONTACTS:
        squircle(x, y, s, UI_RGB(222, 192, 152), UI_RGB(186, 146, 104));
        ui_rrect(x + P(80), y + P(20), P(8), P(60), P(3), UI_RGB(150, 110, 70), 255);
        ui_circle(x + P(48), y + P(40), P(14), UI_RGB(120, 92, 66), 255);
        ui_circle(x + P(48), y + P(84), P(26), UI_RGB(120, 92, 66), 255);
        ui_rrect(x, y + P(92), s, P(8) + 1, 0, UI_RGB(186, 146, 104), 0);
        break;
    case ICON_HEX:
        squircle(x, y, s, UI_RGB(38, 42, 54), UI_RGB(18, 20, 26));
        draw_glyph_text(x + c, y + c - ui_font_height(UF_UIB) / 2, "0x1F", UF_UIB, UI_RGB(90, 225, 130));
        break;
    case ICON_LOG:
        squircle(x, y, s, UI_RGB(64, 64, 68), UI_RGB(30, 30, 34));
        for (int k = 0; k < 5; k++) {
            int len = (k * 37 % 40) + 30;
            ui_line(x + P(18), y + P(24 + k * 13), x + P(18 + len), y + P(24 + k * 13), P(4) + 1,
                    k == 2 ? UI_RGB(255, 214, 10) : UI_RGB(170, 170, 178), 255);
        }
        break;
    case ICON_CLIP:
        squircle(x, y, s, UI_RGB(124, 194, 224), UI_RGB(66, 146, 200));
        ui_rrect(x + P(26), y + P(20), P(48), P(66), P(6), UI_RGB(250, 250, 250), 255);
        ui_rrect(x + P(38), y + P(13), P(24), P(13), P(4), UI_RGB(70, 70, 78), 255);
        for (int k = 0; k < 3; k++) ui_line(x + P(34), y + P(40 + k * 13), x + P(66), y + P(40 + k * 13), P(3) + 1, UI_RGB(170, 180, 190), 255);
        break;
    case ICON_FOLDER:
        ui_rrect(x + P(10), y + P(22), P(34), P(16), P(5), UI_RGB(72, 160, 240), 255);
        ui_rrect_vgrad(x + P(10), y + P(30), P(80), P(52), P(8), UI_RGB(118, 196, 255), UI_RGB(60, 150, 240), 255);
        break;
    case ICON_PAINT: {
        static const uint32_t pc[8] = {
            UI_RGB(255, 196, 0), UI_RGB(255, 138, 0), UI_RGB(255, 59, 92), UI_RGB(206, 80, 220),
            UI_RGB(100, 90, 240), UI_RGB(0, 150, 255), UI_RGB(40, 200, 160), UI_RGB(140, 210, 40) };
        squircle(x, y, s, UI_RGB(255, 255, 255), UI_RGB(238, 238, 242));
        for (int k = 0; k < 8; k++) {
            int a = k * 128;
            ui_circle(x + c + icos(a) * P(19) / 4096, y + c + isin(a) * P(19) / 4096, P(15), pc[k], 190);
        }
        break; }
    case ICON_APPEARANCE:
        squircle(x, y, s, UI_RGB(64, 70, 96), UI_RGB(26, 28, 44));
        ui_circle(x + c, y + c, P(28), UI_RGB(250, 250, 252), 255);
        ui_circle(x + c + P(12), y + c - P(10), P(24), UI_RGB(40, 44, 66), 255);
        break;
    default:
        squircle(x, y, s, UI_RGB(170, 110, 240), UI_RGB(110, 60, 210));
        draw_glyph_text(x + c, y + c - ui_font_height(UF_TITLE) / 2, "?", UF_TITLE, 0xFFFFFF);
        break;
    }
}

/* --------------------------------------------------------------------------
 * App registry
 * -------------------------------------------------------------------------- */
static const ui_app_t APPS[] = {
    { "Finder",             2,                ICON_FINDER,     "File Manager" },
    { "Launchpad",          UI_ACT_LAUNCHPAD, ICON_LAUNCHPAD,  NULL },
    { "Browser",            32,               ICON_WEB,        "Nexus Browser" },
    { "Terminal",           1,                ICON_TERMINAL,   "Terminal" },
    { "Notes",              10,               ICON_NOTES,      "Notepad" },
    { "Calendar",           12,               ICON_CALENDAR,   "Calendar" },
    { "Music",              13,               ICON_MUSIC,      "Music Player" },
    { "Calculator",         3,                ICON_CALC,       "Calculator" },
    { "Activity Monitor",   4,                ICON_MONITOR,    "System Monitor" },
    { "System Settings",    5,                ICON_SETTINGS,   "Settings" },
    /* Launchpad / Spotlight only */
    { "Clock",              20,               ICON_CLOCK,      "Clock" },
    { "Task Manager",       11,               ICON_TASKS,      "Task Manager" },
    { "Reminders",          19,               ICON_REMINDERS,  "Todo" },
    { "Contacts",           25,               ICON_CONTACTS,   "Contacts" },
    { "Paint",              15,               ICON_PAINT,      "Paint" },
    { "System Information", 18,               ICON_INFO,       "System Info" },
    { "Tips",               14,               ICON_HELP,       "Help" },
    { "Console",            27,               ICON_LOG,        "System Log" },
    { "Hex Viewer",         24,               ICON_HEX,        "Hex Viewer" },
    { "Clipboard",          28,               ICON_CLIP,       "Clipboard" },
    { "File Operations",    31,               ICON_FOLDER,     "File Ops" },
    { "Appearance",         29,               ICON_APPEARANCE, "Appearance" },
    { "Trash",              17,               ICON_TRASH,      "Recycle Bin" },
};

const ui_app_t* ui_apps(int* count) {
    if (count) *count = (int)(sizeof(APPS) / sizeof(APPS[0]));
    return APPS;
}

int ui_dock_app_count(void) { return 10; }

const ui_app_t* ui_app_by_title(const char* title) {
    if (!title) return NULL;
    for (unsigned i = 0; i < sizeof(APPS) / sizeof(APPS[0]); i++)
        if (APPS[i].title && !strcmp(APPS[i].title, title)) return &APPS[i];
    return NULL;
}

/* --------------------------------------------------------------------------
 * Boot splash
 * -------------------------------------------------------------------------- */
#define SPLASH_EXPECTED 46
#define SPLASH_BAR_W    220
#define SPLASH_BAR_H    6
static bool splash_on = false;
static int  splash_n = 0;

static void splash_bar(int n) {
    int bx = SW / 2 - SPLASH_BAR_W / 2, by = SH / 2 + 70;
    int fill = n * SPLASH_BAR_W / SPLASH_EXPECTED;
    if (fill > SPLASH_BAR_W) fill = SPLASH_BAR_W;
    fb_fill_rect(bx - 2, by - 2, SPLASH_BAR_W + 4, SPLASH_BAR_H + 4, 0);
    ui_rrect(bx, by, SPLASH_BAR_W, SPLASH_BAR_H, SPLASH_BAR_H / 2, UI_RGB(64, 64, 68), 255);
    if (fill >= SPLASH_BAR_H) ui_rrect(bx, by, fill, SPLASH_BAR_H, SPLASH_BAR_H / 2, UI_RGB(236, 236, 240), 255);
    fb_mark_dirty(bx - 2, by - 2, SPLASH_BAR_W + 4, SPLASH_BAR_H + 4);
}

void ui_splash_begin(void) {
    if (!bb()) return;
    splash_on = true;
    splash_n = 0;
    fb_clear(0);
    int h = 104, w = h * 82 / 100;
    ui_logo(SW / 2 - w / 2, SH / 2 - h + 10, h, UI_RGB(240, 240, 244));
    splash_bar(0);
    fb_mark_dirty_all();
    fb_flip();
}

void ui_splash_step(void) {
    if (!splash_on || !bb()) return;
    splash_n++;
    splash_bar(splash_n);
    fb_flip();
}

void ui_splash_end(void) {
    if (!splash_on) return;
    if (bb()) { splash_bar(SPLASH_EXPECTED); fb_flip(); }
    splash_on = false;
}

bool ui_splash_active(void) { return splash_on; }
