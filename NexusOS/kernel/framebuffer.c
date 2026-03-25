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

#define FB_BACKBUF_ADDR  0x500000
static uint32_t* fb_back = NULL;
static uint32_t fb_back_pitch = 0;
static uint32_t fb_buf_size = 0;

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

    /* Clear back buffer to black */
    uint32_t pixels = fb_width * fb_height;
    for (uint32_t i = 0; i < pixels; i++) {
        fb_back[i] = 0;
    }

    vesa_active = true;
}

bool fb_is_vesa(void) { return vesa_active; }
uint32_t fb_get_width(void)  { return fb_width; }
uint32_t fb_get_height(void) { return fb_height; }
uint32_t fb_get_pitch(void)  { return fb_pitch; }
uint32_t fb_get_phys_addr(void) { return fb_phys_addr; }
uint32_t* fb_get_backbuffer(void) { return fb_back; }

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
    int cw = x2 - x1;
    for (int row = y1; row < y2; row++) {
        uint32_t* dst = &fb_back[row * fb_width + x1];
        for (int i = 0; i < cw; i++) dst[i] = color;
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
    uint32_t pixels = fb_width * fb_height;
    for (uint32_t i = 0; i < pixels; i++) fb_back[i] = color;
}

void fb_flip(void) {
    if (!vesa_active) return;

    if (fb_bpp == 32) {
        /* Direct copy if pitch matches, otherwise row by row */
        if (fb_pitch == fb_width * 4) {
            memcpy(fb_front, fb_back, fb_buf_size);
        } else {
            for (uint32_t y = 0; y < fb_height; y++) {
                memcpy(&fb_front[y * fb_pitch], &fb_back[y * fb_width], fb_width * 4);
            }
        }
    } else if (fb_bpp == 24) {
        /* Convert 32-bit backbuffer to 24-bit physical framebuffer */
        for (uint32_t y = 0; y < fb_height; y++) {
            uint8_t* dst_row = &fb_front[y * fb_pitch];
            uint32_t* src_row = &fb_back[y * fb_width];
            for (uint32_t x = 0; x < fb_width; x++) {
                uint32_t color = src_row[x];
                dst_row[x * 3 + 0] = color & 0xFF;         // Blue
                dst_row[x * 3 + 1] = (color >> 8) & 0xFF;  // Green
                dst_row[x * 3 + 2] = (color >> 16) & 0xFF; // Red
            }
        }
    }
}

uint32_t vga_to_rgb(uint8_t vga_color) { return vga_palette[vga_color & 0x0F]; }
uint32_t vga_attr_fg(uint8_t attr) { return vga_palette[attr & 0x0F]; }
uint32_t vga_attr_bg(uint8_t attr) { return vga_palette[(attr >> 4) & 0x0F]; }
