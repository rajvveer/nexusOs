/* Host checks for arch/x86_64/aurora_damage.h against brute-force references. */
#include "../arch/x86_64/aurora_damage.h"
#include <assert.h>
#include <stdio.h>

static uint32_t seed = 0x9E3779B9u;
static uint32_t next_random(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static int range(int low, int high) { return low + (int)(next_random() % (uint32_t)(high - low + 1)); }

static void blend(void) {
    /* Exhaustive per channel against the previous compositor's exact formula. */
    for (uint32_t a = 0; a < 256; a++) for (uint32_t c = 0; c < 256; c++) for (uint32_t d = 0; d < 256; d++) {
        uint32_t p = a << 24 | c << 16 | c << 8 | c, b = d << 16 | d << 8 | d;
        uint32_t expected = c + (d * (255 - a) + 127) / 255; if (expected > 255) expected = 255;
        assert(blend_pixel(p, b) == (expected << 16 | expected << 8 | expected));
    }
    puts("PASS blend_pixel equals (v+127)/255 premultiplied over for every alpha/source/destination byte");
}

static bool covered(const struct popup_box* list, int count, int x, int y) {
    for (int i = 0; i < count; i++) if (x >= list[i].x && y >= list[i].y && x < list[i].x + list[i].w && y < list[i].y + list[i].h) return true;
    return false;
}
static void damage_lists(void) {
    enum { W = 64, H = 48 };
    const struct popup_box bounds = {0, 0, W, H};
    for (int round = 0; round < 4000; round++) {
        struct popup_box list[8]; int count = 0, capacity = range(1, 8); bool want[H][W] = {{false}};
        int adds = range(1, 20);
        for (int k = 0; k < adds; k++) {
            struct popup_box b = {range(-20, W + 5), range(-20, H + 5), range(-3, 40), range(-3, 30)};
            if (k == 0 && round % 7 == 0) b = (struct popup_box){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};
            rects_add(list, &count, capacity, b, bounds);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
                if (b.w > 0 && b.h > 0 && (int64_t)x >= b.x && (int64_t)y >= b.y && (int64_t)x < (int64_t)b.x + b.w && (int64_t)y < (int64_t)b.y + b.h) want[y][x] = true;
            assert(count <= capacity);
            for (int i = 0; i < count; i++) { struct popup_box clipped; assert(box_intersect(list[i], bounds, &clipped) && box_equal(clipped, list[i])); }
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (want[y][x]) assert(covered(list, count, x, y));
        }
    }
    struct popup_box list[4]; int count = 0;
    rects_add(list, &count, 4, (struct popup_box){0, 0, 10, 10}, (struct popup_box){0, 0, 100, 100});
    rects_add(list, &count, 4, (struct popup_box){90, 90, 10, 10}, (struct popup_box){0, 0, 100, 100});
    assert(count == 2);   /* distant rectangles stay separate instead of growing to the whole screen */
    rects_add(list, &count, 4, (struct popup_box){10, 0, 10, 10}, (struct popup_box){0, 0, 100, 100});
    assert(count == 2);   /* an adjacent rectangle merges without waste */
    puts("PASS damage lists: bounded count, clipped to bounds, every damaged pixel covered, cheap merges only");
}

static bool region_has(const struct rectangle* region, int x, int y) {
    bool result = false;
    for (const struct rectangle* r = region; r; r = r->next)
        if ((int64_t)x >= r->x && (int64_t)y >= r->y && (int64_t)x < (int64_t)r->x + r->w && (int64_t)y < (int64_t)r->y + r->h) result = r->add;
    return result;
}
static bool map_has(const struct opaque_map* m, int x, int y) {
    for (int b = 0; b < m->bands; b++) if (y >= m->band[b].y0 && y < m->band[b].y1)
        for (int k = 0; k < m->band[b].count; k++) { struct ospan s = m->span[m->band[b].first + k]; if (x >= s.x0 && x < s.x1) return true; }
    return false;
}
static void opaque_maps(void) {
    struct rectangle cells[OPAQUE_REGION_RECTS + 2];
    for (int round = 0; round < 3000; round++) {
        int w = range(1, 40), h = range(1, 40), n = range(0, round % 50 == 0 ? OPAQUE_REGION_RECTS + 1 : 12);
        struct rectangle* region = NULL;
        for (int i = n - 1; i >= 0; i--) {
            cells[i] = (struct rectangle){region, range(-10, w + 5), range(-10, h + 5), range(1, 50), range(1, 50), next_random() % 3 != 0};
            if (round % 11 == 0 && i == 0) cells[i] = (struct rectangle){region, INT32_MIN / 2, INT32_MIN / 2, INT32_MAX, INT32_MAX, true};
            region = &cells[i];
        }
        bool full = round % 9 == 0;
        struct opaque_map m = {0};
        opaque_map_build(&m, region, w, h, full);
        assert(m.w == w && m.h == h && m.opaque == full);
        bool ignored = !full && n > OPAQUE_REGION_RECTS;
        for (int b = 0; b < m.bands; b++) {
            assert(m.band[b].y0 < m.band[b].y1 && m.band[b].y0 >= 0 && m.band[b].y1 <= h && (b == 0 || m.band[b - 1].y1 <= m.band[b].y0));
            for (int k = 0; k < m.band[b].count; k++) {
                struct ospan s = m.span[m.band[b].first + k];
                assert(s.x0 < s.x1 && s.x0 >= 0 && s.x1 <= w && (k == 0 || m.span[m.band[b].first + k - 1].x1 < s.x0));
            }
        }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
            assert(map_has(&m, x, y) == (ignored ? false : full || region_has(region, x, y)));
        for (int o = 0; o < m.occluders; o++)
            for (int y = m.occluder[o].y; y < m.occluder[o].y + m.occluder[o].h; y++)
                for (int x = m.occluder[o].x; x < m.occluder[o].x + m.occluder[o].w; x++) assert(map_has(&m, x, y));
        if (full) assert(m.occluders == 1 && box_equal(m.occluder[0], (struct popup_box){0, 0, w, h}));
        opaque_map_clear(&m);
    }
    struct opaque_map m = {0};
    opaque_map_build(&m, NULL, 0, 10, true); assert(!m.bands && !m.occluders);
    puts("PASS opaque maps: band/span coverage equals in-order add/subtract region evaluation, clipped, occluders inside coverage, oversized regions ignored");
}

int main(void) { blend(); damage_lists(); opaque_maps(); puts("PASS aurora damage helpers"); return 0; }
