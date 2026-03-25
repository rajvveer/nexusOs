/* ============================================================================
 * NexusOS — Graphics Primitives (Header) — Phase 16
 * ============================================================================
 * Drawing routines on top of the framebuffer.
 * Phase 16: Lines, circles, rounded rects, gradients, alpha, shadows.
 * ============================================================================ */

#ifndef GFX_H
#define GFX_H

#include "types.h"
#include "font.h"

/* Color macro */
#define GFX_RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

/* --------------------------------------------------------------------------
 * Basic shapes (Phase 14)
 * -------------------------------------------------------------------------- */
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_hline(int x, int y, int w, uint32_t color);
void gfx_draw_vline(int x, int y, int h, uint32_t color);

/* --------------------------------------------------------------------------
 * Text (Phase 15)
 * -------------------------------------------------------------------------- */
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void gfx_draw_text(int x, int y, const char* str, uint32_t fg, uint32_t bg);
void gfx_draw_text_scaled(int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale);
void gfx_draw_text_aa(int x, int y, const char* str, uint32_t fg, uint32_t bg);
void gfx_set_font_size(FontSize size);

/* --------------------------------------------------------------------------
 * Phase 16: Lines
 * -------------------------------------------------------------------------- */
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

/* --------------------------------------------------------------------------
 * Phase 16: Circles
 * -------------------------------------------------------------------------- */
void gfx_draw_circle(int cx, int cy, int r, uint32_t color);
void gfx_fill_circle(int cx, int cy, int r, uint32_t color);

/* --------------------------------------------------------------------------
 * Phase 16: Rounded rectangles
 * -------------------------------------------------------------------------- */
void gfx_draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color);
void gfx_fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t color);

/* --------------------------------------------------------------------------
 * Phase 16: Gradients
 * -------------------------------------------------------------------------- */
void gfx_draw_gradient_h(int x, int y, int w, int h, uint32_t c1, uint32_t c2);
void gfx_draw_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2);

/* --------------------------------------------------------------------------
 * Phase 16: Alpha blending & transparency
 * -------------------------------------------------------------------------- */
uint32_t gfx_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha);
void gfx_fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha);

/* --------------------------------------------------------------------------
 * Phase 16: Drop shadow
 * -------------------------------------------------------------------------- */
void gfx_draw_shadow(int x, int y, int w, int h, int radius, uint32_t color);

/* --------------------------------------------------------------------------
 * Screen control
 * -------------------------------------------------------------------------- */
void gfx_clear(uint32_t color);
void gfx_flip(void);

#endif /* GFX_H */
