/* ============================================================================
 * NexusOS — Font Rendering Engine (Header) — Phase 15
 * ============================================================================
 * Multi-size bitmap font system with:
 *   - 8×8 and 8×16 fonts (switchable)
 *   - Integer-scaled rendering (1x–4x)
 *   - Anti-aliased text (2×2 supersampled)
 *   - Unicode Latin-1 Supplement mapping
 * ============================================================================ */

#ifndef FONT_H
#define FONT_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Font sizes
 * -------------------------------------------------------------------------- */
typedef enum {
    FONT_SIZE_8x8  = 0,   /* Compact: 8 wide × 8 tall  */
    FONT_SIZE_8x16 = 1    /* Standard: 8 wide × 16 tall */
} FontSize;

/* Base font dimensions (unscaled) */
#define FONT_WIDTH   8
#define FONT_HEIGHT  16    /* Height of the default (8x16) font */
#define FONT8_HEIGHT 8     /* Height of the compact (8x8) font  */

/* Number of glyphs */
#define FONT_GLYPHS  256

/* Max scale factor */
#define FONT_MAX_SCALE 4

/* --------------------------------------------------------------------------
 * Font size selection
 * -------------------------------------------------------------------------- */

/* Set the active font size (affects all non-explicit draw calls) */
void font_set_size(FontSize size);

/* Get current active font size */
FontSize font_get_size(void);

/* Get active font dimensions (UNSCALED base glyph size — the global
 * accessibility scale is NOT folded in here, because the renderers use these
 * as the base from which they compute scaled output. Layout code that wants
 * on-screen pixel extents should multiply by font_get_scale()). */
int font_get_active_width(void);
int font_get_active_height(void);

/* --------------------------------------------------------------------------
 * Phase 43: Global accessibility text scale (1..FONT_MAX_SCALE)
 * --------------------------------------------------------------------------
 * Sets a system-wide integer magnification applied automatically by the
 * non-scaled draw/measure helpers (font_draw_char/string, _transparent, _aa,
 * font_measure_string). Raising it enlarges ALL UI text for low-vision users.
 * The explicit *_scaled() calls are NOT affected (they request an exact
 * scale). Out-of-range values are clamped. */
void font_set_scale(int scale);
int  font_get_scale(void);

/* --------------------------------------------------------------------------
 * Core rendering (uses active font size)
 * -------------------------------------------------------------------------- */

/* Get the bitmap data for a character (8 or 16 bytes depending on size) */
const uint8_t* font_get_glyph(uint8_t c);

/* Draw a single character at pixel position (x, y) to framebuffer */
void font_draw_char(int x, int y, uint8_t c, uint32_t fg, uint32_t bg);

/* Like font_draw_char but ALWAYS 1x — ignores the global accessibility scale.
 * For fixed-grid renderers (the raw VESA text console in vga.c) that would
 * otherwise overlap if the global scale were >1. */
void font_draw_char_fixed(int x, int y, uint8_t c, uint32_t fg, uint32_t bg);

/* Draw a null-terminated string at pixel position (x, y) */
void font_draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg);

/* Draw a character with transparent background (bg pixels unchanged) */
void font_draw_char_transparent(int x, int y, uint8_t c, uint32_t fg);

/* --------------------------------------------------------------------------
 * Scaled rendering
 * -------------------------------------------------------------------------- */

/* Draw a character at Nx integer scale (scale = 1..4) */
void font_draw_char_scaled(int x, int y, uint8_t c, uint32_t fg, uint32_t bg, int scale);

/* Draw a string at Nx integer scale */
void font_draw_string_scaled(int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale);

/* Draw a character scaled with transparent background */
void font_draw_char_scaled_transparent(int x, int y, uint8_t c, uint32_t fg, int scale);

/* --------------------------------------------------------------------------
 * Anti-aliased rendering
 * -------------------------------------------------------------------------- */

/* Draw an anti-aliased character (2x2 supersampled for smooth edges) */
void font_draw_char_aa(int x, int y, uint8_t c, uint32_t fg, uint32_t bg);

/* Draw an anti-aliased string */
void font_draw_string_aa(int x, int y, const char* str, uint32_t fg, uint32_t bg);

/* --------------------------------------------------------------------------
 * Text measurement
 * -------------------------------------------------------------------------- */

/* Measure pixel width of a string at current font size (unscaled) */
int font_measure_string(const char* str);

/* Measure pixel width of a string at given scale */
int font_measure_string_scaled(const char* str, int scale);

/* --------------------------------------------------------------------------
 * Unicode support
 * -------------------------------------------------------------------------- */

/* Map a Unicode codepoint (U+0000–U+00FF) to a CP437 glyph index */
uint8_t font_unicode_to_cp437(uint16_t codepoint);

#endif /* FONT_H */
