/* Aurora compositor damage helpers: rectangle lists, opaque-region span maps and the
 * premultiplied blend. Header-only and free of Wayland types so the host tests can check
 * them against brute-force references (tests/aurora_damage_test.c). */
#ifndef NEXUS_AURORA_DAMAGE_H
#define NEXUS_AURORA_DAMAGE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "popup_position.h"

/* wl_region contents: rectangles applied in order, add or subtract. */
struct rectangle { struct rectangle* next; int32_t x,y,w,h; bool add; };

static inline bool box_intersect(struct popup_box a, struct popup_box b, struct popup_box* out) {
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w < (int64_t)b.x + b.w ? (int64_t)a.x + a.w : (int64_t)b.x + b.w;
    int64_t y1 = (int64_t)a.y + a.h < (int64_t)b.y + b.h ? (int64_t)a.y + a.h : (int64_t)b.y + b.h;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || x0 >= x1 || y0 >= y1) return false;
    if (out) *out = (struct popup_box){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
    return true;
}
static inline struct popup_box box_union(struct popup_box a, struct popup_box b) {
    int64_t x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w > (int64_t)b.x + b.w ? (int64_t)a.x + a.w : (int64_t)b.x + b.w;
    int64_t y1 = (int64_t)a.y + a.h > (int64_t)b.y + b.h ? (int64_t)a.y + a.h : (int64_t)b.y + b.h;
    return (struct popup_box){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
}
static inline int64_t box_area(struct popup_box b) { return b.w > 0 && b.h > 0 ? (int64_t)b.w * b.h : 0; }
static inline bool box_equal(struct popup_box a, struct popup_box b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

/* Add b (clipped to bounds) to a list of at most capacity rectangles. Rectangles merge when
 * their bounding box wastes little area; a full list merges into the cheapest neighbour.
 * The list always covers every pixel ever added (it may cover more). */
static inline void rects_add(struct popup_box* list, int* count, int capacity, struct popup_box b, struct popup_box bounds) {
    if (!box_intersect(b, bounds, &b)) return;
    for (;;) {
        bool merged = false;
        for (int i = 0; i < *count; i++) {
            struct popup_box overlap, joined = box_union(list[i], b);
            int64_t shared = box_intersect(list[i], b, &overlap) ? box_area(overlap) : 0;
            int64_t separate = box_area(list[i]) + box_area(b) - shared, slack = separate / 8 > 4096 ? separate / 8 : 4096;
            if (box_area(joined) <= separate + slack) { b = joined; list[i] = list[--*count]; merged = true; break; }
        }
        if (merged) continue;
        if (*count < capacity) { list[(*count)++] = b; return; }
        int best = 0; int64_t cost = INT64_MAX;
        for (int i = 0; i < *count; i++) {
            int64_t grow = box_area(box_union(list[i], b)) - box_area(list[i]);
            if (grow < cost) { cost = grow; best = i; }
        }
        b = box_union(list[best], b); list[best] = list[--*count];
    }
}

/* Opaque region as y-bands of sorted disjoint x-spans (surface coordinates), plus the
 * largest opaque rectangles used as occluders by the painter. */
#define OPAQUE_REGION_RECTS 64
struct oband { int32_t y0, y1; int first, count; };
struct ospan { int32_t x0, x1; };
struct opaque_map { int bands, occluders; struct oband* band; struct ospan* span; struct popup_box occluder[4]; int32_t w, h; bool opaque; };
static inline void opaque_map_clear(struct opaque_map* m) { free(m->band); free(m->span); *m = (struct opaque_map){0}; }
/* Apply one add/subtract of [a,b) to sorted disjoint spans; keeps at most limit spans. */
static inline int spans_apply(struct ospan* spans, int count, int limit, int32_t a, int32_t b, bool add) {
    struct ospan out[2 * (OPAQUE_REGION_RECTS + 1) + 2]; int n = 0;
    if (add) {
        int i = 0;
        while (i < count && spans[i].x1 < a) out[n++] = spans[i++];
        for (; i < count && spans[i].x0 <= b; i++) { if (spans[i].x0 < a) a = spans[i].x0; if (spans[i].x1 > b) b = spans[i].x1; }
        out[n++] = (struct ospan){a, b};
        while (i < count) out[n++] = spans[i++];
    } else for (int i = 0; i < count; i++) {
        if (spans[i].x1 <= a || spans[i].x0 >= b) { out[n++] = spans[i]; continue; }
        if (spans[i].x0 < a) out[n++] = (struct ospan){spans[i].x0, a};
        if (spans[i].x1 > b) out[n++] = (struct ospan){b, spans[i].x1};
    }
    if (n > limit) n = limit;
    memcpy(spans, out, (size_t)n * sizeof(*out)); return n;
}
static inline int opaque_edge_compare(const void* a, const void* b) { int32_t x = *(const int32_t*)a, y = *(const int32_t*)b; return (x > y) - (x < y); }
/* Build the map for a w x h surface: everything when full (XRGB), else the region clipped
 * to the surface. Regions over OPAQUE_REGION_RECTS rectangles are ignored (blended). */
static inline void opaque_map_build(struct opaque_map* m, const struct rectangle* region, int32_t w, int32_t h, bool full) {
    opaque_map_clear(m); m->w = w; m->h = h; m->opaque = full;
    if (w <= 0 || h <= 0) return;
    int rects = 0; for (const struct rectangle* r = region; r; r = r->next) rects++;
    if (!full && (!rects || rects > OPAQUE_REGION_RECTS)) return;
    int32_t edges[2 * OPAQUE_REGION_RECTS + 2]; int edge_count = 0;
    edges[edge_count++] = 0; edges[edge_count++] = h;
    if (!full) for (const struct rectangle* r = region; r; r = r->next) {
        int64_t y0 = r->y, y1 = (int64_t)r->y + r->h;
        if (y0 > 0 && y0 < h) edges[edge_count++] = (int32_t)y0;
        if (y1 > 0 && y1 < h) edges[edge_count++] = (int32_t)y1;
    }
    qsort(edges, (size_t)edge_count, sizeof(*edges), opaque_edge_compare);
    m->band = calloc((size_t)edge_count, sizeof(*m->band)); m->span = calloc((size_t)edge_count * (OPAQUE_REGION_RECTS + 1), sizeof(*m->span));
    if (!m->band || !m->span) { opaque_map_clear(m); m->w = w; m->h = h; m->opaque = full; return; }
    int spans = 0;
    for (int e = 0; e + 1 < edge_count; e++) {
        int32_t y0 = edges[e], y1 = edges[e + 1]; if (y0 == y1) continue;
        struct ospan line[OPAQUE_REGION_RECTS + 1]; int n = 0;
        if (full) line[n++] = (struct ospan){0, w};
        else for (const struct rectangle* r = region; r; r = r->next) {
            if ((int64_t)r->y > y0 || (int64_t)r->y + r->h < y1) continue;
            int64_t a = r->x < 0 ? 0 : r->x, b = (int64_t)r->x + r->w > w ? w : (int64_t)r->x + r->w;
            if (a < b) n = spans_apply(line, n, OPAQUE_REGION_RECTS, (int32_t)a, (int32_t)b, r->add);
        }
        if (!n) continue;
        struct oband* previous = m->bands ? &m->band[m->bands - 1] : NULL;
        if (previous && previous->y1 == y0 && previous->count == n && !memcmp(m->span + previous->first, line, (size_t)n * sizeof(*line))) { previous->y1 = y1; continue; }
        m->band[m->bands++] = (struct oband){y0, y1, spans, n};
        memcpy(m->span + spans, line, (size_t)n * sizeof(*line)); spans += n;
    }
    for (int b = 0; b < m->bands; b++) for (int k = 0; k < m->band[b].count; k++) {
        struct ospan sp = m->span[m->band[b].first + k];
        struct popup_box candidate = {sp.x0, m->band[b].y0, sp.x1 - sp.x0, m->band[b].y1 - m->band[b].y0};
        int slot = m->occluders < 4 ? m->occluders++ : -1;
        if (slot < 0) {
            slot = 0; for (int i = 1; i < 4; i++) if (box_area(m->occluder[i]) < box_area(m->occluder[slot])) slot = i;
            if (box_area(m->occluder[slot]) >= box_area(candidate)) continue;
        }
        m->occluder[slot] = candidate;
    }
}

/* Premultiplied source-over for one pixel; ((v+128)*257)>>16 == (v+127)/255 for 8-bit products. */
static inline uint32_t blend_pixel(uint32_t p, uint32_t b) {
    uint32_t a = p >> 24, k = 255 - a;
    uint32_t r = ((p >> 16) & 255) + (((((b >> 16) & 255) * k + 128) * 257) >> 16);
    uint32_t g = ((p >> 8) & 255) + (((((b >> 8) & 255) * k + 128) * 257) >> 16);
    uint32_t blue = (p & 255) + ((((b & 255) * k + 128) * 257) >> 16);
    return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (blue > 255 ? 255 : blue);
}
#endif
