/* ============================================================================
 * NexusOS — Sprite Engine (Header) — Phase 41
 * ============================================================================
 * A small hardware-style sprite layer on top of the Phase 41 GPU path:
 * up to 32 ARGB sprites with per-pixel alpha, z-ordering and show/hide,
 * composited onto the back buffer each frame and presented through the
 * VirtIO-GPU dirty-rect flush (or the VESA flip when no GPU is present).
 * ============================================================================ */

#ifndef SPRITE_H
#define SPRITE_H

#include "types.h"

#define SPRITE_MAX      32
#define SPRITE_MAX_DIM  64     /* per side; keeps heap allocations modest */

/* Allocate a w×h sprite (pixels start fully transparent). Returns an id,
 * or -1 if the pool/heap is exhausted. */
int sprite_create(int w, int h);

/* The sprite's ARGB pixel buffer (row-major, w×h), for drawing into.
 * Alpha 0 = transparent, 255 = opaque, in-between = blended. */
uint32_t* sprite_pixels(int id);

void sprite_move(int id, int x, int y);
void sprite_set_z(int id, int z);          /* higher z drawn on top */
void sprite_show(int id, bool visible);
void sprite_destroy(int id);
void sprite_destroy_all(void);
int  sprite_count(void);                   /* live sprites */

/* Alpha-blend every visible sprite onto the back buffer in z order
 * (clipped; does not present — call fb_flip()/gpu_present after). */
void sprite_composite(void);

/* Helper for demos: paint a shaded, anti-aliased ball into a sprite. */
void sprite_paint_ball(int id, uint32_t color);

#endif /* SPRITE_H */
