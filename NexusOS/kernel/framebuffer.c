/* ============================================================================
 * NexusOS — VESA Framebuffer Driver (Implementation) — Phase 14
 * ============================================================================
 * Reads VESA mode info stored by boot2.asm at physical address 0x9000:
 *   0x9000: uint32_t  framebuffer physical address
 *   0x9004: uint16_t  pitch (bytes per scanline)
 *   0x9006: uint16_t  width
 *   0x9008: uint16_t  height
 *   0x900A: uint8_t   VESA active flag (1 = active)
 * ============================================================================ */

#include "framebuffer.h"
#include "string.h"
#include "port.h"
#include "gpu.h"
#include "memory.h"

static void serial_print_hex(uint32_t val) {
    char hex[16] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        while ((port_byte_in(0x3F8 + 5) & 0x20) == 0);
        port_byte_out(0x3F8, hex[(val >> i) & 0xF]);
    }
    while ((port_byte_in(0x3F8 + 5) & 0x20) == 0);
    port_byte_out(0x3F8, '\n');
}

/* Internal state */
static uint32_t fb_phys_addr = 0;
static uint8_t* fb_front = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static uint8_t  fb_bpp = 0;
static uint8_t  fb_bytes_per_pixel = 0;
static bool     vesa_active = false;

/* Phase 53: the 32-bit back buffer lives at 16MB. At 1920x1080x4 it is ~8MB
 * (0x01000000-0x017E9000), which sits ABOVE the heap (HEAP_MAX=0xA00000) and
 * above the 15MB demand-paging window, entirely inside the 32MB identity map
 * (see paging.c IDENTITY_TABLES). pmm_reserve_range() marks these pages used so
 * the physical allocator never hands them out. Old location (0x500000) collided
 * with the heap region and could not hold a 1080p buffer. */
#define FB_BACKBUF_ADDR  0x01000000
static uint32_t* fb_back = NULL;
static uint32_t fb_back_pitch = 0;
static uint32_t fb_buf_size = 0;

/* --------------------------------------------------------------------------
 * Dirty-rectangle accumulator (Phase 52: only present what changed).
 * A single coarse bounding box over the back buffer. fb_mark_dirty() unions
 * a rect into the box; fb_flip() presents only that box and resets it. This
 * collapses a cursor move from a whole-screen (~3MB) blit to a few rows.
 * -------------------------------------------------------------------------- */
static bool fb_dirty_valid = false;
static int  fb_dx1 = 0, fb_dy1 = 0, fb_dx2 = 0, fb_dy2 = 0;  /* [x1,x2) [y1,y2) */
static bool gpu_present_region(int x, int y, int w, int h);  /* fwd decl */

/* rep stosl fill of `count` 32-bit words at dst with `color`. Mirrors the
 * memset fast path in string.c but stores a full 32-bit colour, not a
 * broadcast byte. dst must be dword-aligned (the back buffer always is). */
static inline void fb_memset32(uint32_t* dst, uint32_t color, uint32_t count) {
    __asm__ volatile("cld; rep stosl"
                     : "+D"(dst), "+c"(count)
                     : "a"(color)
                     : "memory");
}

/* VGA 16-color to 32-bit RGB lookup table */
static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

/* ============================================================================
 * fb_init: Read VESA info from bootloader and set up framebuffer
 * ============================================================================ */
void fb_init(void) {
    /* Read VESA info stored by boot2.asm at 0x9000 */
    volatile uint8_t* vinfo = (volatile uint8_t*)0x9000;

    /* Check VESA active flag at offset 0x0A */
    if (vinfo[0x0A] != 1) {
        vesa_active = false;
        return;
    }

    /* Read framebuffer address (4 bytes at offset 0) */
    fb_phys_addr = *((volatile uint32_t*)&vinfo[0]);

    /* Read pitch (2 bytes at offset 4) */
    fb_pitch = *((volatile uint16_t*)&vinfo[4]);

    /* Read resolution (2 bytes each at offset 6, 8) */
    fb_width  = *((volatile uint16_t*)&vinfo[6]);
    fb_height = *((volatile uint16_t*)&vinfo[8]);

    /* Sanity check */
    if (fb_width == 0 || fb_height == 0 || fb_phys_addr == 0) {
        vesa_active = false;
        return;
    }

    fb_bpp = vinfo[0x0B];
    if (fb_bpp == 0) fb_bpp = 32; // fallback
    fb_bytes_per_pixel = fb_bpp / 8;

    /* DEBUG: print values to COM1 */
    while ((port_byte_in(0x3F8 + 5) & 0x20) == 0); port_byte_out(0x3F8, 'F');
    while ((port_byte_in(0x3F8 + 5) & 0x20) == 0); port_byte_out(0x3F8, 'B');
    while ((port_byte_in(0x3F8 + 5) & 0x20) == 0); port_byte_out(0x3F8, '\n');
    serial_print_hex(fb_phys_addr);
    serial_print_hex(fb_width);
    serial_print_hex(fb_height);
    serial_print_hex(fb_pitch);
    serial_print_hex(fb_bpp);

    /* Set up buffer pointers */
    fb_front = (uint8_t*)fb_phys_addr;
    fb_back  = (uint32_t*)FB_BACKBUF_ADDR;
    fb_back_pitch = fb_width * 4;
    fb_buf_size = fb_height * fb_back_pitch;

    /* Phase 53: protect the back buffer from the physical allocator. fb_init
     * runs after pmm_init, so the bitmap exists. Without this, demand paging /
     * page-table allocation could hand out pages inside the buffer. */
    pmm_reserve_range(FB_BACKBUF_ADDR, fb_buf_size);

    /* Clear back buffer to black */
    fb_memset32(fb_back, 0, fb_width * fb_height);

    vesa_active = true;
}

/* --------------------------------------------------------------------------
 * fb_mark_dirty: union a rect into the pending present box. Clamped to the
 * screen. Callers that change pixels in the back buffer should mark the
 * affected region; if nobody marks anything, fb_flip presents nothing.
 * -------------------------------------------------------------------------- */
void fb_mark_dirty(int x, int y, int w, int h) {
    if (!vesa_active) return;
    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)fb_width)  x2 = (int)fb_width;
    if (y2 > (int)fb_height) y2 = (int)fb_height;
    if (x1 >= x2 || y1 >= y2) return;
    if (!fb_dirty_valid) {
        fb_dx1 = x1; fb_dy1 = y1; fb_dx2 = x2; fb_dy2 = y2;
        fb_dirty_valid = true;
    } else {
        if (x1 < fb_dx1) fb_dx1 = x1;
        if (y1 < fb_dy1) fb_dy1 = y1;
        if (x2 > fb_dx2) fb_dx2 = x2;
        if (y2 > fb_dy2) fb_dy2 = y2;
    }
}

/* Mark the whole screen dirty (heavy frames: wallpaper/window changes). */
void fb_mark_dirty_all(void) {
    if (!vesa_active) return;
    fb_dx1 = 0; fb_dy1 = 0; fb_dx2 = (int)fb_width; fb_dy2 = (int)fb_height;
    fb_dirty_valid = true;
}

bool fb_is_vesa(void) { return vesa_active; }
uint32_t fb_get_width(void)  { return fb_width; }
uint32_t fb_get_height(void) { return fb_height; }
uint32_t fb_get_pitch(void)  { return fb_pitch; }
uint32_t fb_get_phys_addr(void) { return fb_phys_addr; }
uint32_t* fb_get_backbuffer(void) { return fb_back; }

/* Byte size of the hardware framebuffer (pitch*height) — used to map the LFB. */
uint32_t fb_get_lfb_size(void) { return fb_pitch * fb_height; }

void fb_putpixel(int x, int y, uint32_t color) {
    if (!vesa_active) return;
    if (x < 0 || (uint32_t)x >= fb_width || y < 0 || (uint32_t)y >= fb_height) return;
    fb_back[y * fb_width + x] = color;
}

uint32_t fb_getpixel(int x, int y) {
    if (!vesa_active) return 0;
    if (x < 0 || (uint32_t)x >= fb_width || y < 0 || (uint32_t)y >= fb_height) return 0;
    return fb_back[y * fb_width + x];
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vesa_active) return;
    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)fb_width) x2 = (int)fb_width;
    if (y2 > (int)fb_height) y2 = (int)fb_height;
    if (x1 >= x2 || y1 >= y2) return;
    uint32_t cw = (uint32_t)(x2 - x1);
    for (int row = y1; row < y2; row++) {
        fb_memset32(&fb_back[row * fb_width + x1], color, cw);
    }
}

/* Save a w×h region of the back buffer into buf (row-major, w stride).
 * Off-screen pixels are left untouched in buf; fb_restore_rect uses the same
 * clipping so only on-screen pixels are written back. */
void fb_save_rect(int x, int y, int w, int h, uint32_t* buf) {
    if (!vesa_active) return;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= (int)fb_height) continue;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= (int)fb_width) continue;
            buf[row * w + col] = fb_back[sy * fb_width + sx];
        }
    }
}

void fb_restore_rect(int x, int y, int w, int h, const uint32_t* buf) {
    if (!vesa_active) return;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= (int)fb_height) continue;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= (int)fb_width) continue;
            fb_back[sy * fb_width + sx] = buf[row * w + col];
        }
    }
}

void fb_hline(int x, int y, int w, uint32_t color) { fb_fill_rect(x, y, w, 1, color); }
void fb_vline(int x, int y, int h, uint32_t color) { fb_fill_rect(x, y, 1, h, color); }

void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
    fb_hline(x, y, w, color);
    fb_hline(x, y + h - 1, w, color);
    fb_vline(x, y, h, color);
    fb_vline(x + w - 1, y, h, color);
}

void fb_clear(uint32_t color) {
    if (!vesa_active) return;
    fb_memset32(fb_back, color, fb_width * fb_height);
    fb_mark_dirty_all();
}

/* Force a whole-screen present, ignoring the dirty box. Used by content frames
 * (window open/move/drag) so a partial-present accumulator bug can never leave a
 * stale strip on screen (Phase 53 grey-band safety net). */
void fb_flip_full(void) {
    if (!vesa_active) return;
    fb_dirty_valid = false;
    if (gpu_present_region(0, 0, (int)fb_width, (int)fb_height)) return;
    fb_flip_region(0, 0, (int)fb_width, (int)fb_height);
}

void fb_flip(void) {
    if (!vesa_active) return;

    /* Backward-compatible default: a caller that drew without marking any
     * dirty region (lockscreen, games, screensaver, shell, …) gets a full
     * present, exactly as before. Only callers that opt into partial present
     * by calling fb_mark_dirty() pay the cheap path. */
    int dx1, dy1, dx2, dy2;
    if (fb_dirty_valid) {
        dx1 = fb_dx1; dy1 = fb_dy1; dx2 = fb_dx2; dy2 = fb_dy2;
    } else {
        dx1 = 0; dy1 = 0; dx2 = (int)fb_width; dy2 = (int)fb_height;
    }
    fb_dirty_valid = false;

    /* Phase 41/52: when the VirtIO-GPU owns the scanout, present only the
     * dirty rectangle (transfer+flush of that span) — no full-screen copy. */
    if (gpu_present_region(dx1, dy1, dx2 - dx1, dy2 - dy1)) return;

    fb_flip_region(dx1, dy1, dx2 - dx1, dy2 - dy1);
}

/* GPU partial present wrapper: returns false when no GPU scanout is active so
 * the caller falls back to the legacy VESA copy. */
static bool gpu_present_region(int x, int y, int w, int h) {
    if (!gpu_active()) return false;
    gpu_present(x, y, w, h);
    return true;
}

/* Legacy VESA present of one rectangle: copy only [x,x+w) on rows [y,y+h)
 * from the back buffer into the hardware framebuffer. */
void fb_flip_region(int x, int y, int w, int h) {
    if (!vesa_active) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_width)  w = (int)fb_width  - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    if (w <= 0 || h <= 0) return;

    if (fb_bpp == 32) {
        for (int row = y; row < y + h; row++) {
            memcpy(&fb_front[row * fb_pitch + x * 4],
                   &fb_back[row * fb_width + x],
                   (uint32_t)w * 4);
        }
    } else if (fb_bpp == 24) {
        /* Convert 32-bit backbuffer to 24-bit, dirty span only */
        for (int row = y; row < y + h; row++) {
            uint8_t*  dst_row = &fb_front[row * fb_pitch + x * 3];
            uint32_t* src_row = &fb_back[row * fb_width + x];
            for (int i = 0; i < w; i++) {
                uint32_t color = src_row[i];
                dst_row[i * 3 + 0] = color & 0xFF;         // Blue
                dst_row[i * 3 + 1] = (color >> 8) & 0xFF;  // Green
                dst_row[i * 3 + 2] = (color >> 16) & 0xFF; // Red
            }
        }
    }
}

void fb_flip_legacy(void) {
    if (!vesa_active) return;
    fb_flip_region(0, 0, (int)fb_width, (int)fb_height);
}

uint32_t vga_to_rgb(uint8_t vga_color) { return vga_palette[vga_color & 0x0F]; }
uint32_t vga_attr_fg(uint8_t attr) { return vga_palette[attr & 0x0F]; }
uint32_t vga_attr_bg(uint8_t attr) { return vga_palette[(attr >> 4) & 0x0F]; }
