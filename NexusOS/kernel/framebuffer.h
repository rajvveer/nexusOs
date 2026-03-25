/* ============================================================================
 * NexusOS — VESA Framebuffer Driver (Header) — Phase 14
 * ============================================================================
 * Manages the linear framebuffer set up by VESA VBE 2.0 in boot2.asm.
 * Provides double-buffered pixel plotting at 1024×768×32bpp.
 * ============================================================================ */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "types.h"

/* Default VESA resolution */
#define FB_DEFAULT_WIDTH   1024
#define FB_DEFAULT_HEIGHT  768
#define FB_DEFAULT_BPP     32

/* Bytes per pixel for 32-bit color */
#define FB_BYTES_PER_PIXEL 4

/* Boot info passed from boot2.asm at fixed memory addresses */
#define VESA_INFO_ADDR     0x9000
#define VESA_FB_ADDR_OFF   0    /* uint32_t: framebuffer physical address */
#define VESA_PITCH_OFF     4    /* uint16_t: bytes per scanline */
#define VESA_WIDTH_OFF     6    /* uint16_t: screen width */
#define VESA_HEIGHT_OFF    8    /* uint16_t: screen height */
#define VESA_FLAG_OFF      0x0A /* uint8_t:  1=VESA active, 0=text mode */
#define VESA_BPP_OFF       0x0B /* uint8_t:  bits per pixel */

/* Color macros (32-bit XRGB) */
#define FB_RGB(r, g, b)    ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))
#define FB_RED(c)          (((c) >> 16) & 0xFF)
#define FB_GREEN(c)        (((c) >> 8) & 0xFF)
#define FB_BLUE(c)         ((c) & 0xFF)

/* Common colors */
#define FB_COLOR_BLACK      FB_RGB(0, 0, 0)
#define FB_COLOR_WHITE      FB_RGB(255, 255, 255)
#define FB_COLOR_RED        FB_RGB(255, 0, 0)
#define FB_COLOR_GREEN      FB_RGB(0, 255, 0)
#define FB_COLOR_BLUE       FB_RGB(0, 0, 255)
#define FB_COLOR_CYAN       FB_RGB(0, 170, 170)
#define FB_COLOR_DARK_GREY  FB_RGB(85, 85, 85)

/* --------------------------------------------------------------------------
 * Core functions
 * -------------------------------------------------------------------------- */

/* Initialize framebuffer — reads VESA info from boot2 */
void fb_init(void);

/* Check if VESA mode is active (vs text mode fallback) */
bool fb_is_vesa(void);

/* Get screen dimensions */
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t fb_get_pitch(void);

/* --------------------------------------------------------------------------
 * Pixel operations (operate on back buffer)
 * -------------------------------------------------------------------------- */

/* Plot a single pixel */
void fb_putpixel(int x, int y, uint32_t color);

/* Get pixel color at position */
uint32_t fb_getpixel(int x, int y);

/* Fill a rectangle with solid color */
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);

/* Draw horizontal line */
void fb_hline(int x, int y, int w, uint32_t color);

/* Draw vertical line */
void fb_vline(int x, int y, int h, uint32_t color);

/* Draw outlined rectangle */
void fb_draw_rect(int x, int y, int w, int h, uint32_t color);

/* Clear entire screen with color */
void fb_clear(uint32_t color);

/* Flip: copy back buffer to framebuffer (present frame) */
void fb_flip(void);

/* Get pointer to back buffer (for direct manipulation) */
uint32_t* fb_get_backbuffer(void);

/* Get hardware framebuffer physical address (for page mapping) */
uint32_t fb_get_phys_addr(void);

/* --------------------------------------------------------------------------
 * VGA color conversion
 * -------------------------------------------------------------------------- */

/* Convert VGA 4-bit color attribute (fg|bg<<4) to fg/bg RGB pair */
uint32_t vga_to_rgb(uint8_t vga_color);

/* Convert a VGA color attribute byte to foreground RGB */
uint32_t vga_attr_fg(uint8_t attr);

/* Convert a VGA color attribute byte to background RGB */
uint32_t vga_attr_bg(uint8_t attr);

#endif /* FRAMEBUFFER_H */
