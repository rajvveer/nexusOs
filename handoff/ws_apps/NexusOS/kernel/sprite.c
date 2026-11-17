/* ============================================================================
 * NexusOS — Sprite Engine (Implementation) — Phase 41
 * ============================================================================
 * Fixed pool of ARGB sprites alpha-blended onto the back buffer in z order.
 * Blending is pure integer ( d + (s-d)*a/256 per channel ) — no FPU, and
 * fully clipped so sprites can fly off-screen. The composited frame is then
 * presented by the caller through the VirtIO-GPU (fb_flip → gpu_flip).
 * ============================================================================ */

#include "sprite.h"
#include "framebuffer.h"
#include "heap.h"
#include "string.h"

typedef struct {
    bool      used, visible;
    int       x, y, z;
    int       w, h;
    uint32_t* pix;
} sprite_t;

static sprite_t pool[SPRITE_MAX];

int sprite_create(int w, int h) {
    if (w <= 0 || h <= 0 || w > SPRITE_MAX_DIM || h > SPRITE_MAX_DIM) return -1;
    for (int i = 0; i < SPRITE_MAX; i++) {
        if (pool[i].used) continue;
        uint32_t* pix = (uint32_t*)kmalloc((uint32_t)(w * h * 4));
        if (!pix) return -1;
        memset(pix, 0, (uint32_t)(w * h * 4));
        pool[i].used = true;
        pool[i].visible = true;
        pool[i].x = pool[i].y = pool[i].z = 0;
        pool[i].w = w; pool[i].h = h;
        pool[i].pix = pix;
        return i;
    }
    return -1;
}

static sprite_t* get(int id) {
    if (id < 0 || id >= SPRITE_MAX || !pool[id].used) return NULL;
    return &pool[id];
}

uint32_t* sprite_pixels(int id) { sprite_t* s = get(id); return s ? s->pix : NULL; }
void sprite_move(int id, int x, int y) { sprite_t* s = get(id); if (s) { s->x = x; s->y = y; } }
void sprite_set_z(int id, int z)       { sprite_t* s = get(id); if (s) s->z = z; }
void sprite_show(int id, bool visible) { sprite_t* s = get(id); if (s) s->visible = visible; }

void sprite_destroy(int id) {
    sprite_t* s = get(id);
    if (!s) return;
    if (s->pix) kfree(s->pix);
    memset(s, 0, sizeof(*s));
}

void sprite_destroy_all(void) {
    for (int i = 0; i < SPRITE_MAX; i++) sprite_destroy(i);
}

int sprite_count(void) {
    int n = 0;
    for (int i = 0; i < SPRITE_MAX; i++) if (pool[i].used) n++;
    return n;
}

/* --------------------------------------------------------------------------
 * sprite_composite: blend visible sprites onto the back buffer, z-ordered
 * -------------------------------------------------------------------------- */
static void blend_one(const sprite_t* s, uint32_t* bb, int sw, int sh) {
    int x0 = s->x, y0 = s->y;
    int px0 = 0, py0 = 0, w = s->w, h = s->h;
    if (x0 < 0) { px0 = -x0; w += x0; x0 = 0; }
    if (y0 < 0) { py0 = -y0; h += y0; y0 = 0; }
    if (x0 + w > sw) w = sw - x0;
    if (y0 + h > sh) h = sh - y0;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        const uint32_t* src = s->pix + (py0 + row) * s->w + px0;
        uint32_t*       dst = bb + (y0 + row) * sw + x0;
        for (int i = 0; i < w; i++) {
            uint32_t sp = src[i];
            uint32_t a = sp >> 24;
            if (a == 0) continue;
            if (a == 255) { dst[i] = sp & 0x00FFFFFF; continue; }
            uint32_t dp = dst[i];
            int sr = (int)((sp >> 16) & 0xFF), sg = (int)((sp >> 8) & 0xFF), sb = (int)(sp & 0xFF);
            int dr = (int)((dp >> 16) & 0xFF), dg = (int)((dp >> 8) & 0xFF), db = (int)(dp & 0xFF);
            dr += ((sr - dr) * (int)a) >> 8;
            dg += ((sg - dg) * (int)a) >> 8;
            db += ((sb - db) * (int)a) >> 8;
            dst[i] = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
        }
    }
}

void sprite_composite(void) {
    uint32_t* bb = fb_get_backbuffer();
    if (!bb) return;
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();

    /* Draw in ascending z; the pool is tiny, so selection passes are fine. */
    bool drawn[SPRITE_MAX] = { false };
    for (;;) {
        int best = -1;
        for (int i = 0; i < SPRITE_MAX; i++) {
            if (!pool[i].used || !pool[i].visible || drawn[i]) continue;
            if (best < 0 || pool[i].z < pool[best].z) best = i;
        }
        if (best < 0) break;
        drawn[best] = true;
        blend_one(&pool[best], bb, sw, sh);
    }
}

/* --------------------------------------------------------------------------
 * sprite_paint_ball: shaded ball with anti-aliased rim, for the demo
 * -------------------------------------------------------------------------- */
void sprite_paint_ball(int id, uint32_t color) {
    sprite_t* s = get(id);
    if (!s) return;
    int w = s->w, h = s->h;
    int r  = ((w < h ? w : h) / 2) - 1;
    if (r < 2) return;
    int cx = w / 2, cy = h / 2;
    int lx = cx - r / 3, ly = cy - r / 3;        /* top-left light source */
    uint32_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) { s->pix[y * w + x] = 0; continue; }

            /* Rim anti-aliasing: fade alpha across the outermost ring. */
            uint32_t a = 255;
            int inner = (r - 1) * (r - 1);
            if (d2 > inner) a = (uint32_t)(255 * (r * r - d2) / (r * r - inner));

            /* Diffuse shade by squared distance from the light point. */
            int bx = x - lx, by = y - ly;
            int b2 = bx * bx + by * by;
            int shade = 255 - (b2 * 160) / (4 * r * r);
            if (shade < 70) shade = 70;
            uint32_t pr = cr * (uint32_t)shade / 255;
            uint32_t pg = cg * (uint32_t)shade / 255;
            uint32_t pb = cb * (uint32_t)shade / 255;

            /* Specular highlight near the light point. */
            if (b2 < (r * r) / 9) {
                uint32_t boost = (uint32_t)(200 - b2 * 1800 / (r * r));
                pr += boost; pg += boost; pb += boost;
                if (pr > 255) pr = 255;
                if (pg > 255) pg = 255;
                if (pb > 255) pb = 255;
            }
            s->pix[y * w + x] = (a << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }
}
