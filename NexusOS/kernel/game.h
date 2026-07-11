/* ============================================================================
 * NexusOS — Gaming Framework "NexusSDL" (Header) — Phase 42
 * ============================================================================
 * An SDL-like 2D game API for kernel-space games, the Era 5 finale. Gives a
 * game everything it needs from one header:
 *
 *   video   — a 256-color palettized surface (default 320x200, the classic
 *             VGA mode 13h shape) presented integer-scaled + centered through
 *             the Phase 41 VirtIO-GPU dirty-rect path (VESA flip fallback)
 *   input   — the 12-button virtual game pad (gamepad.h, re-exported)
 *   timing  — PIT-tick frame lock (~18 fps, every frame an identical 55 ms,
 *             so game logic is deterministic) + an fps counter
 *   math    — Q16.16 fixed point with overflow-safe mul/div (no libgcc in
 *             this freestanding kernel — div is one inline idiv) and
 *             LUT trig on a 1024-unit circle (Bhaskara-seeded)
 *   sound   — non-blocking PC-speaker sfx (auto-stopped by game_present)
 *
 * The 8-bit surface keeps per-game allocations to ~64 KB — deliberately small
 * (see SESSION_CONTEXT: large kmallocs aggravate a latent heap/stack layout
 * bug) and authentic to the era of the games it hosts.
 * ============================================================================ */

#ifndef GAME_H
#define GAME_H

#include "types.h"
#include "gamepad.h"

#define GAME_MAX_W   320
#define GAME_MAX_H   240

/* --------------------------------------------------------------------------
 * Q16.16 fixed-point math
 * -------------------------------------------------------------------------- */
typedef int32_t fx_t;
#define FX_ONE       65536
#define FX(n)        ((fx_t)((n) * 65536))
#define FX_INT(a)    ((a) >> 16)

/* a*b>>16 — the (int64_t) cast compiles to a single widening imull;
 * no __muldi3 is pulled in. */
static inline fx_t fx_mul(fx_t a, fx_t b) {
    return (fx_t)(((int64_t)a * b) >> 16);
}

/* (a<<16)/b via one 64/32 idivl (EDX:EAX dividend), saturating instead of
 * faulting #DE when the quotient would overflow 32 bits. No __divdi3. */
static inline fx_t fx_div(fx_t a, fx_t b) {
    uint32_t A = (uint32_t)(a < 0 ? -a : a);
    uint32_t B = (uint32_t)(b < 0 ? -b : b);
    if (B == 0 || (A >> 15) >= B)
        return ((a ^ b) < 0) ? (fx_t)-0x7FFFFFFF : (fx_t)0x7FFFFFFF;
    fx_t q, r;
    fx_t hi = a >> 16;                    /* (int64)a<<16, high dword */
    fx_t lo = (fx_t)((uint32_t)a << 16);  /* ... low dword            */
    __asm__("idivl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(b));
    return q;
}

/* Trig on a 1024-unit circle (1024 = 360 degrees), Q16 result in [-1, 1]. */
fx_t game_sin(int angle);
fx_t game_cos(int angle);

/* Fast LCG random */
uint32_t game_rand(void);
void     game_srand(uint32_t seed);

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
void game_init(void);    /* boot init: trig LUT + default palette (kernel.c) */

/* Open a w x h 8-bit surface (<= GAME_MAX_W/H), pick the largest integer
 * scale that fits the screen, draw the title chrome and acquire the game
 * pad. false if VESA is off or the heap is exhausted. */
bool game_open(int w, int h, const char* title);
void game_close(void);   /* free surface, release pad, stop sfx */
bool game_is_open(void);

/* --------------------------------------------------------------------------
 * Surface + palette
 * -------------------------------------------------------------------------- */
uint8_t* game_surface(void);
int  game_w(void);
int  game_h(void);

/* Palette: indices 0-15 = classic VGA colors (text/HUD); 16-255 = fifteen
 * 16-step brightness ramps (s=0 brightest .. s=15 darkest) for shading. */
void    game_palette_set(int i, uint8_t r, uint8_t g, uint8_t b);
void    game_palette_default(void);
uint8_t game_ramp(int ramp, int shade);          /* ramp 0-14, shade 0-15  */
uint8_t game_darken(uint8_t c, int amount);      /* deeper into the ramp   */

/* Ramp ids of the default palette */
enum {
    RAMP_GRAY = 0, RAMP_RED, RAMP_BROWN, RAMP_ORANGE, RAMP_YELLOW,
    RAMP_GREEN, RAMP_TEAL, RAMP_BLUE, RAMP_PURPLE, RAMP_PINK,
    RAMP_FLESH, RAMP_WOOD, RAMP_BLOOD, RAMP_SLIME, RAMP_STEEL
};

/* --------------------------------------------------------------------------
 * Drawing (all clipped to the surface)
 * -------------------------------------------------------------------------- */
void game_clear(uint8_t c);
void game_pset(int x, int y, uint8_t c);
void game_fill(int x, int y, int w, int h, uint8_t c);
void game_hline(int x, int y, int w, uint8_t c);
void game_vline(int x, int y0, int y1, uint8_t c);
void game_rect(int x, int y, int w, int h, uint8_t c);

/* Blit an 8-bit pixel block; pixels equal to `key` are transparent
 * (key < 0 = opaque copy). */
void game_blit(const uint8_t* src, int sw, int sh, int dx, int dy, int key);

/* 8x8 bitmap text (CP437), optionally integer-scaled */
void game_text(int x, int y, const char* s, uint8_t c);
void game_text_big(int x, int y, const char* s, uint8_t c, int scale);
void game_text_center(int y, const char* s, uint8_t c);
void game_text_center_big(int y, const char* s, uint8_t c, int scale);

/* --------------------------------------------------------------------------
 * Present + timing
 * -------------------------------------------------------------------------- */
void game_present(void);     /* scale-expand to fb_back + GPU dirty-rect flush */
void game_sync(void);        /* sleep (hlt) until the next PIT tick (~55 ms)   */
uint32_t game_ms(void);      /* uptime in ms (tick-granular)                   */
uint32_t game_fps(void);     /* measured present rate                          */
uint32_t game_frames(void);  /* frames presented since game_open               */

/* --------------------------------------------------------------------------
 * Sound effects (PC speaker, non-blocking; AC'97 jingles via audio.h)
 * -------------------------------------------------------------------------- */
void game_sfx(uint32_t freq_hz, uint32_t ticks);   /* start tone, returns now */
void game_sfx_stop(void);

/* --------------------------------------------------------------------------
 * Status (for the gameinfo shell command)
 * -------------------------------------------------------------------------- */
const char* game_status(void);
uint32_t game_total_frames(void);   /* lifetime frames across all games */
uint32_t game_sessions(void);       /* game_open count                  */

#endif /* GAME_H */
