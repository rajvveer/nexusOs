/* ============================================================================
 * NexusOS — Graphics Primitives (Implementation) — Phase 16
 * ============================================================================
 * Phase 16: Lines, circles, rounded rects, gradients, alpha, shadows.
 * All algorithms use integer math only (no FPU).
 * ============================================================================ */

#include "gfx.h"
#include "framebuffer.h"
#include "font.h"

/* ==========================================================================
 * Basic shapes (Phase 14)
 * ========================================================================== */

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    fb_draw_rect(x, y, w, h, color);
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color);
}

void gfx_draw_hline(int x, int y, int w, uint32_t color) {
    fb_hline(x, y, w, color);
}

void gfx_draw_vline(int x, int y, int h, uint32_t color) {
    fb_vline(x, y, h, color);
}

/* ==========================================================================
 * Text (Phase 15)
 * ========================================================================== */

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    font_draw_char(x, y, (uint8_t)c, fg, bg);
}

void gfx_draw_text(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    font_draw_string(x, y, str, fg, bg);
}

void gfx_draw_text_scaled(int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale) {
    font_draw_string_scaled(x, y, str, fg, bg, scale);
}

void gfx_draw_text_aa(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    font_draw_string_aa(x, y, str, fg, bg);
}

void gfx_set_font_size(FontSize size) {
    font_set_size(size);
}

void gfx_clear(uint32_t color) {
    fb_clear(color);
}

void gfx_flip(void) {
    fb_flip();
}

/* ==========================================================================
 * Phase 16: Bresenham's Line
 * ========================================================================== */

void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!fb_is_vesa()) return;

    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;

    while (1) {
        fb_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* ==========================================================================
 * Phase 16: Midpoint Circle
 * ========================================================================== */

void gfx_draw_circle(int cx, int cy, int r, uint32_t color) {
    if (!fb_is_vesa() || r <= 0) return;

    int x = r, y = 0;
    int d = 1 - r;

    while (x >= y) {
        fb_putpixel(cx + x, cy + y, color);
        fb_putpixel(cx - x, cy + y, color);
        fb_putpixel(cx + x, cy - y, color);
        fb_putpixel(cx - x, cy - y, color);
        fb_putpixel(cx + y, cy + x, color);
        fb_putpixel(cx - y, cy + x, color);
        fb_putpixel(cx + y, cy - x, color);
        fb_putpixel(cx - y, cy - x, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint32_t color) {
    if (!fb_is_vesa() || r <= 0) return;

    int x = r, y = 0;
    int d = 1 - r;

    while (x >= y) {
        /* Draw horizontal spans for each octant pair */
        fb_hline(cx - x, cy + y, 2 * x + 1, color);
        fb_hline(cx - x, cy - y, 2 * x + 1, color);
        fb_hline(cx - y, cy + x, 2 * y + 1, color);
        fb_hline(cx - y, cy - x, 2 * y + 1, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

/* ==========================================================================
 * Phase 16: Rounded Rectangles
 * ========================================================================== */

void gfx_draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (!fb_is_vesa() || w <= 0 || h <= 0) return;
    if (r <= 0) { gfx_draw_rect(x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Straight edges */
    fb_hline(x + r, y, w - 2 * r, color);           /* Top */
    fb_hline(x + r, y + h - 1, w - 2 * r, color);   /* Bottom */
    fb_vline(x, y + r, h - 2 * r, color);            /* Left */
    fb_vline(x + w - 1, y + r, h - 2 * r, color);   /* Right */

    /* Corner arcs using midpoint circle */
    int cx, cy, px = r, py = 0, d = 1 - r;
    while (px >= py) {
        /* Top-left */
        cx = x + r; cy = y + r;
        fb_putpixel(cx - px, cy - py, color);
        fb_putpixel(cx - py, cy - px, color);
        /* Top-right */
        cx = x + w - 1 - r; cy = y + r;
        fb_putpixel(cx + px, cy - py, color);
        fb_putpixel(cx + py, cy - px, color);
        /* Bottom-left */
        cx = x + r; cy = y + h - 1 - r;
        fb_putpixel(cx - px, cy + py, color);
        fb_putpixel(cx - py, cy + px, color);
        /* Bottom-right */
        cx = x + w - 1 - r; cy = y + h - 1 - r;
        fb_putpixel(cx + px, cy + py, color);
        fb_putpixel(cx + py, cy + px, color);

        py++;
        if (d <= 0) {
            d += 2 * py + 1;
        } else {
            px--;
            d += 2 * (py - px) + 1;
        }
    }
}

void gfx_fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (!fb_is_vesa() || w <= 0 || h <= 0) return;
    if (r <= 0) { gfx_fill_rect(x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Fill the center rectangle */
    fb_fill_rect(x, y + r, w, h - 2 * r, color);

    /* Fill the corner spans using midpoint circle */
    int px = r, py = 0, d = 1 - r;
    while (px >= py) {
        /* Top spans */
        fb_hline(x + r - px, y + r - py, w - 2 * r + 2 * px, color);
        fb_hline(x + r - py, y + r - px, w - 2 * r + 2 * py, color);
        /* Bottom spans */
        fb_hline(x + r - px, y + h - 1 - r + py, w - 2 * r + 2 * px, color);
        fb_hline(x + r - py, y + h - 1 - r + px, w - 2 * r + 2 * py, color);

        py++;
        if (d <= 0) {
            d += 2 * py + 1;
        } else {
            px--;
            d += 2 * (py - px) + 1;
        }
    }
}

/* ==========================================================================
 * Phase 16: Gradients (integer interpolation)
 * ========================================================================== */

/* Interpolate between two colors. t ranges from 0 to 'steps' */
static inline uint32_t lerp_color(uint32_t c1, uint32_t c2, int t, int steps) {
    if (steps <= 0) return c1;
    uint32_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint32_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint32_t r = r1 + (((int)r2 - (int)r1) * t) / steps;
    uint32_t g = g1 + (((int)g2 - (int)g1) * t) / steps;
    uint32_t b = b1 + (((int)b2 - (int)b1) * t) / steps;
    return (r << 16) | (g << 8) | b;
}

void gfx_draw_gradient_h(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    if (!fb_is_vesa() || w <= 0 || h <= 0) return;
    for (int col = 0; col < w; col++) {
        uint32_t color = lerp_color(c1, c2, col, w - 1);
        fb_vline(x + col, y, h, color);
    }
}

void gfx_draw_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    if (!fb_is_vesa() || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t color = lerp_color(c1, c2, row, h - 1);
        fb_hline(x, y + row, w, color);
    }
}

/* ==========================================================================
 * Phase 16: Alpha Blending
 * ========================================================================== */

uint32_t gfx_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;
    uint32_t inv = 255 - alpha;
    uint32_t r = (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv) / 255;
    uint32_t g = (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv) / 255;
    uint32_t b = ((fg & 0xFF) * alpha + (bg & 0xFF) * inv) / 255;
    return (r << 16) | (g << 8) | b;
}

void gfx_fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    if (!fb_is_vesa() || w <= 0 || h <= 0) return;
    if (alpha == 255) { fb_fill_rect(x, y, w, h, color); return; }
    if (alpha == 0) return;

    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    uint32_t* backbuf = fb_get_backbuffer();
    if (!backbuf) return;

    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)fb_w) x2 = (int)fb_w;
    if (y2 > (int)fb_h) y2 = (int)fb_h;
    if (x1 >= x2 || y1 >= y2) return;

    for (int row = y1; row < y2; row++) {
        uint32_t* dst = &backbuf[row * fb_w + x1];
        for (int col = 0; col < x2 - x1; col++) {
            dst[col] = gfx_alpha_blend(color, dst[col], alpha);
        }
    }
}

/* ==========================================================================
 * Phase 16: Drop Shadow
 * ========================================================================== */

void gfx_draw_shadow(int x, int y, int w, int h, int radius, uint32_t color) {
    if (!fb_is_vesa() || radius <= 0) return;

    /* Draw layered semi-transparent rects expanding outward */
    for (int i = 1; i <= radius; i++) {
        uint8_t alpha = (uint8_t)(80 / i);  /* Fade out */
        /* Bottom edge */
        gfx_fill_rect_alpha(x + i, y + h + i - 1, w, 1, color, alpha);
        /* Right edge */
        gfx_fill_rect_alpha(x + w + i - 1, y + i, 1, h, color, alpha);
    }
    /* Corner shadow */
    for (int i = 1; i <= radius; i++) {
        uint8_t alpha = (uint8_t)(60 / i);
        gfx_fill_rect_alpha(x + w, y + h, i, i, color, alpha);
    }
}
