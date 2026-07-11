/* ============================================================================
 * NexusOS — Image Subsystem (Header) — Phase 39
 * ============================================================================
 * Format detection + unified decode for BMP, PNG, JPEG (baseline) and GIF
 * (animated), plus a fullscreen framebuffer viewer. Each decoder produces a
 * heap-allocated 32-bit ARGB pixel buffer; the viewer scales it to fit the
 * 1024x768 screen with nearest-neighbour sampling.
 * ============================================================================ */

#ifndef IMAGE_H
#define IMAGE_H

#include "types.h"

typedef struct {
    uint32_t* pixels;     /* 0x00RRGGBB, width*height, kmalloc'd               */
    int       width;
    int       height;
    int       frames;     /* >1 for animated GIF (pixels = first frame)         */
    const char* format;   /* "BMP" / "PNG" / "JPEG" / "GIF"                     */
} image_t;

/* Decode the first frame of an image blob. Fills `img`, returns true on success.
 * On success the caller owns img->pixels and must call image_free(). */
bool image_decode(const uint8_t* data, uint32_t size, image_t* img);
void image_free(image_t* img);

/* Magic-byte format name, or "?" if unrecognized. Never decodes. */
const char* image_format_name(const uint8_t* data, uint32_t size);

/* Shared dimension sanity check (decoders cap allocations). */
bool image_dim_ok(int w, int h);

/* Install the embedded sample images into the RAM-FS (called at boot). */
void image_init(void);

/* Decode `data` and show it fullscreen (scaled + centered) until a key is
 * pressed. Animated GIFs cycle their frames. */
void image_view(const uint8_t* data, uint32_t size, const char* title);

/* Render one already-decoded frame fullscreen (scaled, centered, captioned)
 * without waiting for input. Used by the video player. */
void image_present(const image_t* img, const char* caption);

/* --- Per-format decoders (implemented in png.c / gif.c / jpeg.c) ------------*/
bool png_decode(const uint8_t* data, uint32_t size, image_t* img);
bool gif_decode(const uint8_t* data, uint32_t size, image_t* img);
bool jpeg_decode(const uint8_t* data, uint32_t size, image_t* img);
/* GIF: decode a specific frame index (0-based) for animation playback. */
bool gif_decode_frame(const uint8_t* data, uint32_t size, image_t* img, int frame);

#endif /* IMAGE_H */
