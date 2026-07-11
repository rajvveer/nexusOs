/* ============================================================================
 * NexusOS — Image Subsystem (Implementation) — Phase 39
 * ============================================================================
 * Format detection, a BMP decoder, the decode dispatcher, the boot-time sample
 * installer, and the fullscreen framebuffer viewer. PNG/JPEG/GIF live in their
 * own files (png.c / jpeg.c / gif.c) and plug in through image.h.
 * ============================================================================ */

#include "image.h"
#include "framebuffer.h"
#include "gfx.h"
#include "keyboard.h"
#include "string.h"
#include "heap.h"
#include "vga.h"
#include "vfs.h"
#include "ramfs.h"
#include "sample_images.h"

extern volatile uint32_t system_ticks;

/* Decoders cap dimensions to keep heap allocations modest (the kernel heap is
 * small and shares its address range with the stack/back-buffer). */
#define IMAGE_MAX_DIM 256

/* --------------------------------------------------------------------------
 * Little-endian readers
 * -------------------------------------------------------------------------- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool image_dim_ok(int w, int h) {
    return w > 0 && h > 0 && w <= IMAGE_MAX_DIM && h <= IMAGE_MAX_DIM;
}

/* --------------------------------------------------------------------------
 * BMP decoder (uncompressed 24/32-bit)
 * -------------------------------------------------------------------------- */
static bool bmp_decode(const uint8_t* d, uint32_t size, image_t* img) {
    if (size < 54 || d[0] != 'B' || d[1] != 'M') return false;
    uint32_t off  = rd32(d + 10);
    int32_t  w    = (int32_t)rd32(d + 18);
    int32_t  h    = (int32_t)rd32(d + 22);
    uint16_t bpp  = rd16(d + 28);
    uint32_t comp = rd32(d + 30);
    if (comp != 0) return false;
    if (bpp != 24 && bpp != 32) return false;

    bool topdown = false;
    if (h < 0) { topdown = true; h = -h; }
    if (!image_dim_ok(w, h)) return false;

    int bytespp = bpp / 8;
    uint32_t rowsize = (((uint32_t)w * bytespp) + 3) & ~3u;   /* w,h capped: no overflow */
    if (off + rowsize * (uint32_t)h > size) return false;

    uint32_t* px = (uint32_t*)kmalloc((uint32_t)w * h * 4);
    if (!px) return false;

    for (int y = 0; y < h; y++) {
        int srcrow = topdown ? y : (h - 1 - y);
        const uint8_t* row = d + off + (uint32_t)srcrow * rowsize;
        for (int x = 0; x < w; x++) {
            const uint8_t* p = row + x * bytespp;
            px[y * w + x] = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        }
    }
    img->pixels = px; img->width = w; img->height = h;
    img->frames = 1; img->format = "BMP";
    return true;
}

/* --------------------------------------------------------------------------
 * Format detection + dispatch
 * -------------------------------------------------------------------------- */
const char* image_format_name(const uint8_t* d, uint32_t size) {
    if (size >= 2 && d[0] == 'B' && d[1] == 'M') return "BMP";
    if (size >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "PNG";
    if (size >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "JPEG";
    if (size >= 4 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') return "GIF";
    return "?";
}

bool image_decode(const uint8_t* data, uint32_t size, image_t* img) {
    if (!data || !img || size < 8) return false;
    const char* fmt = image_format_name(data, size);
    if (fmt[0] == 'B') return bmp_decode(data, size, img);
    if (fmt[0] == 'P') return png_decode(data, size, img);
    if (fmt[0] == 'J') return jpeg_decode(data, size, img);
    if (fmt[0] == 'G') return gif_decode(data, size, img);
    return false;
}

void image_free(image_t* img) {
    if (img && img->pixels) { kfree(img->pixels); img->pixels = NULL; }
}

/* --------------------------------------------------------------------------
 * Fullscreen viewer — scale to fit, centered, with a caption
 * -------------------------------------------------------------------------- */
static void blit_scaled(const image_t* img, int x0, int y0, int scale) {
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            uint32_t c = img->pixels[y * img->width + x] & 0xFFFFFF;
            int px = x0 + x * scale, py = y0 + y * scale;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    fb_putpixel(px + sx, py + sy, c);
        }
    }
}

static void build_caption(char* out, const char* title, const image_t* img, int frame) {
    char n[12];
    strcpy(out, title); strcat(out, "  ");
    int_to_str(img->width, n);  strcat(out, n); strcat(out, "x");
    int_to_str(img->height, n); strcat(out, n); strcat(out, "  ");
    strcat(out, img->format);
    if (img->frames > 1) {
        strcat(out, "  frame ");
        int_to_str(frame + 1, n); strcat(out, n); strcat(out, "/");
        int_to_str(img->frames, n); strcat(out, n);
    }
}

/* Render a single decoded frame to the framebuffer (scaled, centered, captioned)
 * and present it. Does not wait for input — reused by the video player. */
void image_present(const image_t* img, const char* caption) {
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    int avail_w = sw - 40, avail_h = sh - 80;
    int scale = 1;
    while (img->width * (scale + 1) <= avail_w &&
           img->height * (scale + 1) <= avail_h && scale < 24) scale++;
    int dw = img->width * scale, dh = img->height * scale;
    int x0 = (sw - dw) / 2, y0 = (sh - dh) / 2 + 8;

    uint32_t bg = FB_RGB(18, 18, 26);
    fb_clear(bg);
    blit_scaled(img, x0, y0, scale);
    gfx_draw_text((sw - (int)strlen(caption) * 8) / 2, y0 - 22, caption, FB_RGB(235, 235, 245), bg);
    gfx_draw_text(20, sh - 22, "Press any key to stop", FB_RGB(120, 120, 140), bg);
    fb_flip();
}

void image_view(const uint8_t* data, uint32_t size, const char* title) {
    image_t img;
    if (!image_decode(data, size, &img)) {
        vga_print_color("  Cannot decode image (", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)image_format_name(data, size));
        vga_print(" unsupported or malformed)\n");
        return;
    }

    int frame = 0;
    for (;;) {
        char cap[96]; build_caption(cap, title, &img, frame);
        image_present(&img, cap);

        if (img.frames <= 1) { keyboard_getchar(); break; }

        /* Animated GIF: hold the frame ~330ms, advance, stop on keypress. */
        uint32_t start = system_ticks; bool quit = false;
        while ((system_ticks - start) < 6) {
            if (keyboard_has_key()) { keyboard_getchar(); quit = true; break; }
            __asm__ volatile("hlt");
        }
        if (quit) break;
        frame = (frame + 1) % img.frames;
        image_free(&img);
        if (!gif_decode_frame(data, size, &img, frame)) break;
    }
    image_free(&img);
}

/* --------------------------------------------------------------------------
 * Boot-time sample installer
 * -------------------------------------------------------------------------- */
void image_init(void) {
    fs_node_t* root = vfs_get_root();
    int n = 0;
    if (root) {
        for (int i = 0; i < SAMPLE_IMAGE_COUNT; i++) {
            const sample_image_t* s = &sample_images[i];
            if (vfs_finddir(root, (char*)s->name)) continue;
            fs_node_t* node = ramfs_create((char*)s->name, FS_FILE);
            if (node) { vfs_write(node, 0, s->len, s->data); n++; }
        }
    }
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Image subsystem ready (BMP/PNG/JPEG/GIF), ");
    char b[12]; int_to_str(n, b); vga_print(b);
    vga_print(" samples installed\n");
}
