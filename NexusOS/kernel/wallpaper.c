/* ============================================================================
 * NexusOS — Wallpaper Patterns (Implementation) — Phase 14
 * ============================================================================
 * Multiple background patterns for the desktop. Selectable from Settings.
 * Phase 14: Added WP_PHOTO which draws actual pixel wallpaper in VESA mode.
 * ============================================================================ */

#include "wallpaper.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "framebuffer.h"

/* Phase 14: Include pixel wallpaper data */
#include "wallpaper_data.h"

/* Image wallpaper data for text mode (auto-generated from PNG) */
extern const unsigned char wp_image_chars[24][80];
extern const unsigned char wp_image_colors[24][80];

/* Current pattern */
static int current_pattern = WP_DOTS;

/* Phase 14: Track if pixel wallpaper has been rendered to back buffer */
static bool photo_rendered = false;

/* Pattern names */
static const char* pattern_names[WP_COUNT] = {
    "Dots", "Grid", "Diamonds", "Gradient", "Landscape", "Photo"
};

void wallpaper_set(int pattern) {
    if (pattern >= 0 && pattern < WP_COUNT) {
        current_pattern = pattern;
        photo_rendered = false;  /* Force re-render on change */
    }
}

int wallpaper_get(void) {
    return current_pattern;
}

const char* wallpaper_get_name(int pattern) {
    if (pattern >= 0 && pattern < WP_COUNT)
        return pattern_names[pattern];
    return "Unknown";
}

/* --------------------------------------------------------------------------
 * rgb565_to_rgb888: Convert 16-bit RGB565 to 32-bit RGB for framebuffer
 * -------------------------------------------------------------------------- */
static uint32_t rgb565_to_rgb888(uint16_t c) {
    uint8_t r = ((c >> 11) & 0x1F) << 3;
    uint8_t g = ((c >> 5)  & 0x3F) << 2;
    uint8_t b = (c & 0x1F) << 3;
    return FB_RGB(r, g, b);
}

/* --------------------------------------------------------------------------
 * draw_photo_wallpaper: Render pixel wallpaper with bilinear interpolation
 * Smooth upscale from 256x192 to 1024x768. Integer-only math.
 * -------------------------------------------------------------------------- */
static void draw_photo_wallpaper(void) {
    if (!fb_is_vesa()) return;

    uint32_t* backbuf = fb_get_backbuffer();
    if (!backbuf) return;

    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();

    /* Fixed-point 8-bit fractional: source coord = dst * (src_dim - 1) / (dst_dim - 1) */
    for (uint32_t dy = 0; dy < fb_h; dy++) {
        /* src_y in 8.8 fixed point */
        uint32_t sy_fp = dy * ((WP_IMG_HEIGHT - 1) << 8) / (fb_h - 1);
        int sy0 = sy_fp >> 8;
        int sy1 = sy0 + 1;
        if (sy1 >= WP_IMG_HEIGHT) sy1 = WP_IMG_HEIGHT - 1;
        int fy = sy_fp & 0xFF; /* fractional part 0-255 */

        for (uint32_t dx = 0; dx < fb_w; dx++) {
            uint32_t sx_fp = dx * ((WP_IMG_WIDTH - 1) << 8) / (fb_w - 1);
            int sx0 = sx_fp >> 8;
            int sx1 = sx0 + 1;
            if (sx1 >= WP_IMG_WIDTH) sx1 = WP_IMG_WIDTH - 1;
            int fx = sx_fp & 0xFF;

            /* Sample 4 source pixels (RGB565) */
            uint32_t c00 = rgb565_to_rgb888(wp_pixel_data[sy0 * WP_IMG_WIDTH + sx0]);
            uint32_t c10 = rgb565_to_rgb888(wp_pixel_data[sy0 * WP_IMG_WIDTH + sx1]);
            uint32_t c01 = rgb565_to_rgb888(wp_pixel_data[sy1 * WP_IMG_WIDTH + sx0]);
            uint32_t c11 = rgb565_to_rgb888(wp_pixel_data[sy1 * WP_IMG_WIDTH + sx1]);

            /* Extract channels and bilinear blend */
            int ifx = 256 - fx, ify = 256 - fy;

            int r = (int)(((c00 >> 16) & 0xFF) * ifx * ify +
                          ((c10 >> 16) & 0xFF) * fx * ify +
                          ((c01 >> 16) & 0xFF) * ifx * fy +
                          ((c11 >> 16) & 0xFF) * fx * fy) >> 16;
            int g = (int)(((c00 >> 8) & 0xFF) * ifx * ify +
                          ((c10 >> 8) & 0xFF) * fx * ify +
                          ((c01 >> 8) & 0xFF) * ifx * fy +
                          ((c11 >> 8) & 0xFF) * fx * fy) >> 16;
            int b = (int)((c00 & 0xFF) * ifx * ify +
                          (c10 & 0xFF) * fx * ify +
                          (c01 & 0xFF) * ifx * fy +
                          (c11 & 0xFF) * fx * fy) >> 16;

            backbuf[dy * fb_w + dx] = FB_RGB(r, g, b);
        }
    }
    photo_rendered = true;
}

/* --------------------------------------------------------------------------
 * wallpaper_draw: Render the wallpaper pattern
 * -------------------------------------------------------------------------- */
void wallpaper_draw(void) {
    const theme_t* t = theme_get();
    uint8_t bg = t->desktop_bg;
    uint8_t fg = t->desktop_fg;

    /* Phase 14: Photo wallpaper in VESA mode */
    if (current_pattern == WP_PHOTO && fb_is_vesa()) {
        draw_photo_wallpaper();
        /* DON'T use gui_putchar — it renders pixels that overwrite the wallpaper.
         * Just write transparent spaces to the backbuf for UI hit-testing. */
        extern uint16_t* gui_get_backbuf(void);
        uint16_t* buf = gui_get_backbuf();
        if (buf) {
            for (int y = 0; y < GUI_HEIGHT - 1; y++) {
                for (int x = 0; x < GUI_WIDTH; x++) {
                    buf[y * GUI_WIDTH + x] = (' ' | (VGA_COLOR(VGA_WHITE, VGA_BLACK) << 8));
                }
            }
        }
        return;
    }

    for (int y = 0; y < GUI_HEIGHT - 1; y++) {
        for (int x = 0; x < GUI_WIDTH; x++) {
            char ch;
            uint8_t color;

            switch (current_pattern) {
                case WP_DOTS:
                    /* Subtle dot pattern */
                    if ((x + y) % 4 == 0) {
                        ch = (char)0xFA;  /* middle dot */
                        color = fg;
                    } else {
                        ch = t->desktop_char;
                        color = bg;
                    }
                    break;

                case WP_GRID:
                    /* Grid lines */
                    if (x % 8 == 0) {
                        ch = (char)0xB3;  /* vertical line */
                        color = fg;
                    } else if (y % 4 == 0) {
                        ch = (char)0xC4;  /* horizontal line */
                        color = fg;
                    } else {
                        ch = ' ';
                        color = bg;
                    }
                    /* Intersections */
                    if (x % 8 == 0 && y % 4 == 0) {
                        ch = (char)0xC5;  /* cross */
                        color = fg;
                    }
                    break;

                case WP_DIAMONDS:
                    /* Diamond/checkerboard pattern */
                    if (((x + y) % 4 == 0) || ((x - y + 100) % 4 == 0)) {
                        ch = (char)0x04;  /* diamond */
                        color = fg;
                    } else {
                        ch = t->desktop_char;
                        color = bg;
                    }
                    break;

                case WP_GRADIENT:
                    /* Vertical gradient using shading characters */
                    if (y < 6) {
                        ch = (char)0xDB;  /* full block */
                    } else if (y < 10) {
                        ch = (char)0xB2;  /* dark shade */
                    } else if (y < 14) {
                        ch = (char)0xB1;  /* medium shade */
                    } else if (y < 18) {
                        ch = (char)0xB0;  /* light shade */
                    } else {
                        ch = ' ';
                    }
                    color = bg;
                    break;

                case WP_IMAGE:
                    if (y < 24 && x < 80) {
                        ch = (char)wp_image_chars[y][x];
                        color = wp_image_colors[y][x];
                    } else {
                        ch = t->desktop_char;
                        color = bg;
                    }
                    break;

                default:
                    ch = t->desktop_char;
                    color = bg;
                    break;
            }

            gui_putchar(x, y, ch, color);
        }
    }
}
