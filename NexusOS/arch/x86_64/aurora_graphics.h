#ifndef NEXUS_AURORA_GRAPHICS_H
#define NEXUS_AURORA_GRAPHICS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Caller-owned, tightly packed 32-bit RGB canvas. NULL,0,0 detaches it.
 * Invalid dimensions abort; fb_* byte counts retain their uint32_t ABI.
 * The owner supplies fb_mark_dirty* and fb_flip* for its real presentation path. */
void aurora_canvas(uint32_t* pixels, int width, int height);
/* A canvas with a UI scale (1/120: 120 = 100 %, 180 = 150 %): pixels is width x height physical; drawing code sees
 * fb_get_width/height() = the logical size (physical * 120 / scale, rounded down) and draws in logical coordinates,
 * which fb_* and macui convert (macui draws shapes and text natively at the physical size; the logical edge maps to
 * the physical edge). aurora_canvas() is scale 120. */
void aurora_canvas_scaled(uint32_t* pixels, int width, int height, int scale120);
int aurora_canvas_scale(void);
void aurora_canvas_physical(int* width, int* height);
/* Physical sprite rectangle of the cursor whose hot spot is at logical canvas position x,y (scaled canvases draw a
 * larger arrow); aurora_cursor_draw_scaled/aurora_cursor_restore_scaled save into saved (AURORA_CURSOR_SAVE pixels). */
#define AURORA_CURSOR_SAVE (26 * 20 * 16)
void aurora_cursor_box(int x, int y, bool arrow, int* left, int* top, int* w, int* h);
void aurora_cursor_draw_scaled(int x, int y, bool arrow, uint32_t* saved);
void aurora_cursor_restore_scaled(int x, int y, bool arrow, const uint32_t* saved);

/* Present changed 64-pixel chunks without reading video. previous is a distinct,
 * tightly packed width*height RAM shadow of the cursor-free scene. full copies
 * every pixel and initializes that shadow. stride is in pixels; returns bytes
 * copied to video. After a delta, restore the old cursor with scene_presented=false. */
size_t aurora_present_scene(uint32_t* video, size_t stride, uint32_t* previous, bool full);

/* Present the current cursor over the cursor-free canvas, preserving the canvas.
 * stride is in pixels; video and canvas must not overlap. Positions are canvas
 * coordinates; old position is ignored after a full scene copy. Returns bytes
 * copied to video. Arrow retains Aurora's existing -2 offset; cross uses -6. */
size_t aurora_present_cursor(uint32_t* video, size_t stride, int old_x, int old_y,
                            int x, int y, bool scene_presented, bool arrow);

/* Damage-driven presentation. aurora_present_rect copies one canvas rectangle,
 * clipped to the canvas, to video (stride in pixels, no overlap); returns bytes.
 * aurora_cursor_bounds reports the sprite rectangle for a cursor position.
 * aurora_cursor_draw draws the sprite into the canvas after saving the pixels
 * it covers into saved (UI_CURSOR_W*UI_CURSOR_H entries); aurora_cursor_restore
 * puts them back, leaving the canvas cursor-free again. */
size_t aurora_present_rect(uint32_t* video, size_t stride, int x, int y, int w, int h);
/* Video memory in XBGR8888 (bytes R,G,B,X: /dev/fb0 red.offset 0, blue.offset 16): every copy to video
 * swaps red and blue; the canvas stays XRGB8888. */
void aurora_video_bgr(bool bgr);
void aurora_cursor_bounds(int x, int y, bool arrow, int* left, int* top, int* w, int* h);
void aurora_cursor_draw(int x, int y, bool arrow, uint32_t* saved);
void aurora_cursor_restore(int x, int y, bool arrow, const uint32_t* saved);

#endif
