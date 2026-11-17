#define _POSIX_C_SOURCE 200809L
#include "aurora_graphics.h"
#include "../../kernel/framebuffer.h"
#include "../../kernel/macui.h"
#include "../../kernel/rtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t* canvas;
static int width, height;                       /* physical */
static int scale = 120, lwidth, lheight;        /* UI scale (1/120) and logical size */
static bool video_bgr;
void aurora_video_bgr(bool bgr) { video_bgr = bgr; }
static void video_copy(uint32_t* video, const uint32_t* source, size_t bytes) {
    if (!video_bgr) { memcpy(video, source, bytes); return; }
    for (size_t i = 0; i < bytes / 4; i++) { uint32_t p = source[i]; video[i] = (p & 0xFF00FF00U) | (p >> 16 & 0xFFU) | (p & 0xFFU) << 16; }
}

void aurora_canvas_scaled(uint32_t* pixels, int w, int h, int scale120) {
    if ((!pixels && (w || h)) || (pixels && (w <= 0 || h <= 0 || (uint64_t)w * h > UINT32_MAX / 4))) abort();
    canvas = pixels; width = w; height = h;
    scale = scale120 >= 120 && scale120 <= 480 ? scale120 : 120;
    lwidth = (int)((int64_t)w * 120 / scale); lheight = (int)((int64_t)h * 120 / scale);
}
void aurora_canvas(uint32_t* pixels, int w, int h) { aurora_canvas_scaled(pixels, w, h, 120); }
int aurora_canvas_scale(void) { return scale; }
void aurora_canvas_physical(int* w, int* h) { *w = width; *h = height; }
/* logical -> physical (the logical edge lands on the physical edge) */
static int px_of(int64_t v, int span, int pspan) {
    if (scale == 120) return (int)v;
    if (v >= span) return (int)(pspan + (v - span) * scale / 120);
    return (int)(v >= 0 ? v * scale / 120 : -((-v * scale + 119) / 120));
}
#define PX(v) px_of((v), lwidth, width)
#define PY(v) px_of((v), lheight, height)

bool fb_is_vesa(void) { return canvas != NULL; }
uint32_t* fb_get_backbuffer(void) { return canvas; }
uint32_t fb_get_width(void) { return (uint32_t)lwidth; }
uint32_t fb_get_height(void) { return (uint32_t)lheight; }
uint32_t fb_get_pitch(void) { return (uint32_t)width * 4; }
uint32_t fb_get_bpp(void) { return canvas ? 32 : 0; }
uint32_t fb_get_lfb_size(void) { return (uint32_t)((uint64_t)width * height * 4); }

static bool clip(int x, int y, int w, int h, int* left, int* top, int* right, int* bottom) {
    if (!canvas || w <= 0 || h <= 0) return false;
    int64_t r = (int64_t)x + w, b = (int64_t)y + h;
    *left = x < 0 ? 0 : x; *top = y < 0 ? 0 : y;
    *right = r > width ? width : (int)r; *bottom = b > height ? height : (int)b;
    return *left < *right && *top < *bottom;
}

void fb_putpixel(int x, int y, uint32_t color) {
    if (scale != 120) { fb_fill_rect(x, y, 1, 1, color); return; }
    if (canvas && x >= 0 && y >= 0 && x < width && y < height) canvas[(size_t)y * width + x] = color;
}
uint32_t fb_getpixel(int x, int y) {
    if (scale != 120) { x = PX(x); y = PY(y); }
    return canvas && x >= 0 && y >= 0 && x < width && y < height ? canvas[(size_t)y * width + x] : 0;
}
void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    int left, top, right, bottom;
    if (scale != 120 && w > 0 && h > 0) { int x0 = PX(x), y0 = PY(y); w = PX((int64_t)x + w) - x0; h = PY((int64_t)y + h) - y0; x = x0; y = y0; }
    if (!clip(x, y, w, h, &left, &top, &right, &bottom)) return;
    for (int row = top; row < bottom; row++)
        for (int col = left; col < right; col++) canvas[(size_t)row * width + col] = color;
}
void fb_hline(int x, int y, int w, uint32_t color) { fb_fill_rect(x, y, w, 1, color); }
void fb_vline(int x, int y, int h, uint32_t color) { fb_fill_rect(x, y, 1, h, color); }
void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int64_t right = (int64_t)x + w - 1, bottom = (int64_t)y + h - 1;
    fb_hline(x, y, w, color); fb_vline(x, y, h, color);
    if (bottom >= 0 && bottom < height) fb_hline(x, (int)bottom, w, color);
    if (right >= 0 && right < width) fb_vline((int)right, y, h, color);
}
void fb_clear(uint32_t color) { fb_fill_rect(0, 0, lwidth, lheight, color); fb_mark_dirty_all(); }

void fb_save_rect(int x, int y, int w, int h, uint32_t* pixels) {
    int left, top, right, bottom;
    if (!pixels || (w > 0 && h > 0 && (uint64_t)w * h > SIZE_MAX / sizeof(*pixels)) ||
        !clip(x, y, w, h, &left, &top, &right, &bottom)) return;
    for (int row = top; row < bottom; row++)
        memcpy(pixels + (size_t)((int64_t)row - y) * w + ((int64_t)left - x),
               canvas + (size_t)row * width + left, (size_t)(right - left) * sizeof(*pixels));
}
void fb_restore_rect(int x, int y, int w, int h, const uint32_t* pixels) {
    int left, top, right, bottom;
    if (!pixels || (w > 0 && h > 0 && (uint64_t)w * h > SIZE_MAX / sizeof(*pixels)) ||
        !clip(x, y, w, h, &left, &top, &right, &bottom)) return;
    for (int row = top; row < bottom; row++)
        memcpy(canvas + (size_t)row * width + left,
               pixels + (size_t)((int64_t)row - y) * w + ((int64_t)left - x), (size_t)(right - left) * sizeof(*pixels));
}

size_t aurora_present_scene(uint32_t* video, size_t stride, uint32_t* previous, bool full) {
    if (!canvas || !video || !previous || stride < (size_t)width || stride > SIZE_MAX / 4 / (size_t)height) abort();
    size_t canvas_bytes = (size_t)width * height * 4, video_bytes = stride * height * 4;
    uintptr_t a = (uintptr_t)canvas, b = (uintptr_t)video, c = (uintptr_t)previous;
    if (a > UINTPTR_MAX - canvas_bytes || b > UINTPTR_MAX - video_bytes || c > UINTPTR_MAX - canvas_bytes ||
        (a < b + video_bytes && b < a + canvas_bytes) ||
        (a < c + canvas_bytes && c < a + canvas_bytes) ||
        (b < c + canvas_bytes && c < b + video_bytes)) abort();
    size_t copied = 0, row_bytes = (size_t)width * 4;
    for (int row = 0; row < height; row++) {
        const uint32_t* current = canvas + (size_t)row * width;
        uint32_t* shadow = previous + (size_t)row * width;
        uint32_t* display = video + (size_t)row * stride;
        if (full) {
            video_copy(display, current, row_bytes); memcpy(shadow, current, row_bytes);
            copied += row_bytes; continue;
        }
        if (!memcmp(current, shadow, row_bytes)) continue;
        for (int x = 0; x < width; x += 64) {
            size_t bytes = (size_t)(width - x < 64 ? width - x : 64) * 4;
            if (!memcmp(current + x, shadow + x, bytes)) continue;
            video_copy(display + x, current + x, bytes); memcpy(shadow + x, current + x, bytes);
            copied += bytes;
        }
    }
    return copied;
}

static size_t present_cursor_rect(uint32_t* video, size_t stride, int x, int y, bool arrow) {
    int left, top, right, bottom, offset = arrow ? 2 : 6;
    if (!clip(x - offset, y - offset, arrow ? UI_CURSOR_W : 13, arrow ? UI_CURSOR_H : 13,
              &left, &top, &right, &bottom)) return 0;
    for (int row = top; row < bottom; row++)
        video_copy(video + (size_t)row * stride + left, canvas + (size_t)row * width + left, (size_t)(right - left) * 4);
    return (size_t)(right - left) * (bottom - top) * 4;
}

size_t aurora_present_cursor(uint32_t* video, size_t stride, int old_x, int old_y,
                            int x, int y, bool scene_presented, bool arrow) {
    if (!canvas || !video || stride < (size_t)width || stride > SIZE_MAX / 4 / (size_t)height ||
        x < 0 || y < 0 || x >= width || y >= height ||
        (!scene_presented && (old_x < 0 || old_y < 0 || old_x >= width || old_y >= height))) abort();
    size_t canvas_bytes = (size_t)width * height * 4, video_bytes = stride * height * 4;
    uintptr_t a = (uintptr_t)canvas, b = (uintptr_t)video;
    if (a > UINTPTR_MAX - canvas_bytes || b > UINTPTR_MAX - video_bytes ||
        (a < b + video_bytes && b < a + canvas_bytes)) abort();
    _Static_assert(UI_CURSOR_W >= 13 && UI_CURSOR_H >= 13, "cursor scratch fits both styles");
    uint32_t saved[UI_CURSOR_W * UI_CURSOR_H];
    int offset = arrow ? 2 : 6, w = arrow ? UI_CURSOR_W : 13, h = arrow ? UI_CURSOR_H : 13;
    fb_save_rect(x - offset, y - offset, w, h, saved);
    if (arrow) ui_cursor_draw(x - 1, y - 1);
    else for (int d = -6; d <= 6; d++) { fb_putpixel(x + d, y, 0xFFFFFF); fb_putpixel(x, y + d, 0xFFFFFF); }
    size_t bytes = scene_presented ? 0 : present_cursor_rect(video, stride, old_x, old_y, arrow);
    bytes += present_cursor_rect(video, stride, x, y, arrow);
    fb_restore_rect(x - offset, y - offset, w, h, saved);
    return bytes;
}

static void check_video(const uint32_t* video, size_t stride) {
    if (!canvas || !video || stride < (size_t)width || stride > SIZE_MAX / 4 / (size_t)height) abort();
    size_t canvas_bytes = (size_t)width * height * 4, video_bytes = stride * height * 4;
    uintptr_t a = (uintptr_t)canvas, b = (uintptr_t)video;
    if (a > UINTPTR_MAX - canvas_bytes || b > UINTPTR_MAX - video_bytes ||
        (a < b + video_bytes && b < a + canvas_bytes)) abort();
}

size_t aurora_present_rect(uint32_t* video, size_t stride, int x, int y, int w, int h) {
    check_video(video, stride);
    int left, top, right, bottom;
    if (!clip(x, y, w, h, &left, &top, &right, &bottom)) return 0;
    size_t bytes = (size_t)(right - left) * 4;
    for (int row = top; row < bottom; row++)
        video_copy(video + (size_t)row * stride + left, canvas + (size_t)row * width + left, bytes);
    return bytes * (size_t)(bottom - top);
}

void aurora_cursor_bounds(int x, int y, bool arrow, int* left, int* top, int* w, int* h) {
    int offset = arrow ? 2 : 6;
    *left = x - offset; *top = y - offset; *w = arrow ? UI_CURSOR_W : 13; *h = arrow ? UI_CURSOR_H : 13;
}

void aurora_cursor_draw(int x, int y, bool arrow, uint32_t* saved) {
    int left, top, w, h;
    if (!canvas || !saved) abort();
    aurora_cursor_bounds(x, y, arrow, &left, &top, &w, &h);
    fb_save_rect(left, top, w, h, saved);
    if (arrow) ui_cursor_draw(x - 1, y - 1);
    else for (int d = -6; d <= 6; d++) { fb_putpixel(x + d, y, 0xFFFFFF); fb_putpixel(x, y + d, 0xFFFFFF); }
}

void aurora_cursor_restore(int x, int y, bool arrow, const uint32_t* saved) {
    int left, top, w, h;
    if (!canvas || !saved) abort();
    aurora_cursor_bounds(x, y, arrow, &left, &top, &w, &h);
    fb_restore_rect(left, top, w, h, saved);
}
static int length(int v) { return (int)(((int64_t)v * scale + 60) / 120); }
void aurora_cursor_box(int x, int y, bool arrow, int* left, int* top, int* w, int* h) {
    int offset = length(arrow ? 2 : 6); if (offset < 1) offset = 1;
    *left = PX(x) - offset; *top = PY(y) - offset; *w = length(arrow ? UI_CURSOR_W : 13) + 1; *h = length(arrow ? UI_CURSOR_H : 13) + 1;
    if ((int64_t)*w * *h > AURORA_CURSOR_SAVE) *w = *h = 0;
}
static void copy_rect(int x, int y, int w, int h, uint32_t* to, const uint32_t* from) {   /* physical, clipped; to/from: w*h buffer */
    for (int row = 0; row < h; row++) {
        int py = y + row; if (py < 0 || py >= height) continue;
        for (int col = 0; col < w; col++) {
            int px = x + col; if (px < 0 || px >= width) continue;
            if (to) to[(size_t)row * w + col] = canvas[(size_t)py * width + px]; else canvas[(size_t)py * width + px] = from[(size_t)row * w + col];
        }
    }
}
void aurora_cursor_draw_scaled(int x, int y, bool arrow, uint32_t* saved) {
    int left, top, w, h;
    if (!canvas || !saved) abort();
    aurora_cursor_box(x, y, arrow, &left, &top, &w, &h);
    copy_rect(left, top, w, h, saved, NULL);
    if (arrow) ui_cursor_draw(x - 1, y - 1);   /* as aurora_cursor_draw; macui scales the sprite */
    else { int cx = PX(x), cy = PY(y), r = length(6), t = length(1) > 0 ? length(1) : 1;
        for (int d = -r; d <= r; d++) for (int k = 0; k < t; k++) {
            if (cy + k < height && cx + d >= 0 && cx + d < width && cy + k >= 0) canvas[(size_t)(cy + k) * width + cx + d] = 0xFFFFFF;
            if (cx + k < width && cy + d >= 0 && cy + d < height && cx + k >= 0) canvas[(size_t)(cy + d) * width + cx + k] = 0xFFFFFF; } }
}
void aurora_cursor_restore_scaled(int x, int y, bool arrow, const uint32_t* saved) {
    int left, top, w, h;
    if (!canvas || !saved) abort();
    aurora_cursor_box(x, y, arrow, &left, &top, &w, &h);
    copy_rect(left, top, w, h, NULL, saved);
}

uint32_t vga_to_rgb(uint8_t color) {
    static const uint32_t palette[16] = {
        0x1E1E20,0x0A60D0,0x28A850,0x0096AA,0xD73A32,0xA046C8,0xC8781E,0xC8C8CD,
        0x6E6E76,0x409CFF,0x32D75F,0x5AC8FA,0xFF5F55,0xDC78F0,0xFFD60A,0xF5F5F7
    };
    return palette[color & 15];
}
uint32_t vga_attr_fg(uint8_t attr) { return vga_to_rgb(attr & 15); }
uint32_t vga_attr_bg(uint8_t attr) { return vga_to_rgb(attr >> 4); }

void rtc_read(rtc_time_t* result) {
    struct timespec now; struct tm utc;
    if (clock_gettime(CLOCK_REALTIME, &now) || !gmtime_r(&now.tv_sec, &utc) || utc.tm_year < -1900 || utc.tm_year > 65535 - 1900) abort();
    *result = (rtc_time_t){utc.tm_sec, utc.tm_min, utc.tm_hour, utc.tm_mday, utc.tm_mon + 1, utc.tm_year + 1900};
}
void rtc_format_time(rtc_time_t* t, char* text) { snprintf(text, 9, "%02u:%02u:%02u", (unsigned)t->hour, (unsigned)t->minute, (unsigned)t->second); }
void rtc_format_date(rtc_time_t* t, char* text) { snprintf(text, 11, "%02u/%02u/%04u", (unsigned)t->day, (unsigned)t->month, (unsigned)t->year); }
void int_to_str(int value, char* text) { snprintf(text, 12, "%d", value); }
void uint_to_str(uint32_t value, char* text) { snprintf(text, 11, "%u", (unsigned)value); }
void hex_to_str(uint32_t value, char* text) { snprintf(text, 11, "0x%08X", (unsigned)value); }
void int_to_hex(uint32_t value, char* text) { hex_to_str(value, text); }
