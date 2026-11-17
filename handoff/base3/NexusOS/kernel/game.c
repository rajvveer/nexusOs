/* ============================================================================
 * NexusOS — Gaming Framework "NexusSDL" (Implementation) — Phase 42
 * ============================================================================
 * 256-color surface + integer-scaled GPU presents + tick-locked timing +
 * fixed-point math + non-blocking speaker sfx. See game.h for the contract.
 *
 * The present path: expand each 8-bit surface row through the palette into
 * the 32-bit back buffer (writing each pixel `scale` times, then rep-movsl
 * duplicating the row scale-1 times), and flush just the game rectangle
 * through the Phase 41 VirtIO-GPU dirty-rect present. At 320x200x3 that is
 * ~2.3 MB of writes + one GPU flush per frame — far below the tick budget.
 * ============================================================================ */

#include "game.h"
#include "framebuffer.h"
#include "gfx.h"
#include "gpu.h"
#include "heap.h"
#include "port.h"
#include "vga.h"
#include "string.h"
#include "font8x8.h"

extern volatile uint32_t system_ticks;   /* ~18.2 Hz PIT tick (kernel.c) */
#define TICK_MS 55

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */
static fx_t     sin_lut[1024];
static uint32_t pal32[256];
static uint8_t  pal_r[256], pal_g[256], pal_b[256];

static uint8_t* surf = NULL;
static int      gw, gh, gscale, gox, goy;
static char     gtitle[44];

static uint32_t frames_session, frames_total, sessions;
static uint32_t fps_now, fps_win_frames, fps_win_start;

static uint32_t rng_state = 0x4E455855;  /* "NEXU" */

/* PC speaker (PIT channel 2) — non-blocking sfx */
#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43
#define SPEAKER_PORT  0x61
#define PIT_FREQUENCY 1193180
static bool     sfx_active = false;
static uint32_t sfx_off_tick = 0;

static char status_buf[96];

/* --------------------------------------------------------------------------
 * Math
 * -------------------------------------------------------------------------- */
fx_t game_sin(int angle) { return sin_lut[angle & 1023]; }
fx_t game_cos(int angle) { return sin_lut[(angle + 256) & 1023]; }

uint32_t game_rand(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}
void game_srand(uint32_t seed) { rng_state = seed | 1; }

/* --------------------------------------------------------------------------
 * Palette
 * -------------------------------------------------------------------------- */
void game_palette_set(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 0 || i > 255) return;
    pal_r[i] = r; pal_g[i] = g; pal_b[i] = b;
    pal32[i] = FB_RGB(r, g, b);
}

void game_palette_default(void) {
    /* 0-15: classic VGA colors for text/HUD */
    static const uint8_t vga16[16][3] = {
        {0,0,0},       {0,0,170},     {0,170,0},    {0,170,170},
        {170,0,0},     {170,0,170},   {170,85,0},   {170,170,170},
        {85,85,85},    {85,85,255},   {85,255,85},  {85,255,255},
        {255,85,85},   {255,85,255},  {255,255,85}, {255,255,255}
    };
    for (int i = 0; i < 16; i++)
        game_palette_set(i, vga16[i][0], vga16[i][1], vga16[i][2]);

    /* 16-255: fifteen hue ramps x 16 shades (0 brightest .. 15 darkest) */
    static const uint8_t hues[15][3] = {
        {255,255,255},   /* RAMP_GRAY   */
        {255, 60, 60},   /* RAMP_RED    */
        {185,130, 80},   /* RAMP_BROWN  */
        {255,160, 40},   /* RAMP_ORANGE */
        {255,255, 70},   /* RAMP_YELLOW */
        { 70,230, 70},   /* RAMP_GREEN  */
        { 50,210,210},   /* RAMP_TEAL   */
        { 80,120,255},   /* RAMP_BLUE   */
        {185, 90,255},   /* RAMP_PURPLE */
        {255,110,185},   /* RAMP_PINK   */
        {255,205,160},   /* RAMP_FLESH  */
        {130, 90, 50},   /* RAMP_WOOD   */
        {215, 30, 30},   /* RAMP_BLOOD  */
        {140,255,110},   /* RAMP_SLIME  */
        {160,180,210}    /* RAMP_STEEL  */
    };
    for (int r = 0; r < 15; r++) {
        for (int s = 0; s < 16; s++) {
            int f = 16 - s;   /* brightness factor 16..1 */
            game_palette_set(16 + r * 16 + s,
                             (uint8_t)(hues[r][0] * f / 16),
                             (uint8_t)(hues[r][1] * f / 16),
                             (uint8_t)(hues[r][2] * f / 16));
        }
    }
}

uint8_t game_ramp(int ramp, int shade) {
    if (ramp < 0)   ramp = 0;
    if (ramp > 14)  ramp = 14;
    if (shade < 0)  shade = 0;
    if (shade > 15) shade = 15;
    return (uint8_t)(16 + ramp * 16 + shade);
}

uint8_t game_darken(uint8_t c, int amount) {
    if (c < 16) return c;
    int idx = c - 16;
    int s = (idx & 15) + amount;
    if (s > 15) s = 15;
    if (s < 0)  s = 0;
    return (uint8_t)(16 + (idx & ~15) + s);
}

/* --------------------------------------------------------------------------
 * Boot init (kernel.c)
 * -------------------------------------------------------------------------- */
void game_init(void) {
    /* Bhaskara I sine on a 1024-unit circle: for the half period x in
     * [0,512), sin = 16x(512-x) / (5*512^2 - 4x(512-x)), built in Q16. */
    for (int i = 0; i < 512; i++) {
        int32_t t   = i * (512 - i);          /* 0..65536      */
        int32_t num = 16 * t;                  /* <= 1,048,576  */
        int32_t den = 1310720 - 4 * t;         /* >= 1,048,576  */
        fx_t q = fx_div(num, den);
        if (q > FX_ONE) q = FX_ONE;
        sin_lut[i]       = q;
        sin_lut[512 + i] = -q;
    }
    game_palette_default();

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Gaming framework ready (NexusSDL: 8-bit surface, Q16 math, pad)\n");
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
bool game_open(int w, int h, const char* title) {
    if (!fb_is_vesa() || surf != NULL) return false;
    if (w < 64 || h < 64 || w > GAME_MAX_W || h > GAME_MAX_H) return false;

    surf = (uint8_t*)kmalloc((uint32_t)(w * h));
    if (!surf) return false;
    gw = w; gh = h;

    int fbw = (int)fb_get_width(), fbh = (int)fb_get_height();
    gscale = fbw / w;
    if (fbh / h < gscale) gscale = fbh / h;
    if (gscale < 1) gscale = 1;
    if (gscale > 4) gscale = 4;
    gox = (fbw - w * gscale) / 2;
    goy = (fbh - h * gscale) / 2;

    gtitle[0] = '\0';
    if (title) {
        int i = 0;
        while (title[i] && i < (int)sizeof(gtitle) - 1) { gtitle[i] = title[i]; i++; }
        gtitle[i] = '\0';
    }

    /* Chrome: dark backdrop + frame + caption, drawn once */
    gpu_fill(0, 0, fbw, fbh, FB_RGB(8, 8, 14));
    gpu_fill(gox - 2, goy - 2, w * gscale + 4, h * gscale + 4, FB_RGB(60, 66, 82));
    gpu_fill(gox - 1, goy - 1, w * gscale + 2, h * gscale + 2, FB_RGB(10, 10, 16));
    if (goy >= 16 && gtitle[0])
        gfx_draw_text(gox, goy - 14, gtitle, FB_RGB(225, 228, 240), FB_RGB(8, 8, 14));
    fb_flip();

    game_clear(0);
    gamepad_acquire();

    frames_session = 0; fps_now = 0; fps_win_frames = 0;
    fps_win_start = system_ticks;
    sessions++;
    return true;
}

void game_close(void) {
    if (!surf) return;
    game_sfx_stop();
    gamepad_release();
    kfree(surf);
    surf = NULL;
}

bool game_is_open(void) { return surf != NULL; }

uint8_t* game_surface(void) { return surf; }
int game_w(void) { return gw; }
int game_h(void) { return gh; }

/* --------------------------------------------------------------------------
 * Drawing
 * -------------------------------------------------------------------------- */
void game_clear(uint8_t c) {
    if (!surf) return;
    memset(surf, c, (size_t)(gw * gh));
}

void game_pset(int x, int y, uint8_t c) {
    if (!surf || x < 0 || y < 0 || x >= gw || y >= gh) return;
    surf[y * gw + x] = c;
}

void game_hline(int x, int y, int w, uint8_t c) {
    if (!surf || y < 0 || y >= gh) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > gw) w = gw - x;
    if (w <= 0) return;
    memset(surf + y * gw + x, c, (size_t)w);
}

void game_vline(int x, int y0, int y1, uint8_t c) {
    if (!surf || x < 0 || x >= gw) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= gh) y1 = gh - 1;
    uint8_t* p = surf + y0 * gw + x;
    for (int y = y0; y <= y1; y++, p += gw) *p = c;
}

void game_fill(int x, int y, int w, int h, uint8_t c) {
    for (int i = 0; i < h; i++) game_hline(x, y + i, w, c);
}

void game_rect(int x, int y, int w, int h, uint8_t c) {
    game_hline(x, y, w, c);
    game_hline(x, y + h - 1, w, c);
    game_vline(x, y, y + h - 1, c);
    game_vline(x + w - 1, y, y + h - 1, c);
}

void game_blit(const uint8_t* src, int sw, int sh, int dx, int dy, int key) {
    if (!surf || !src) return;
    for (int y = 0; y < sh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= gh) continue;
        const uint8_t* s = src + y * sw;
        uint8_t* d = surf + ty * gw;
        for (int x = 0; x < sw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= gw) continue;
            if (key >= 0 && s[x] == (uint8_t)key) continue;
            d[tx] = s[x];
        }
    }
}

void game_text_big(int x, int y, const char* s, uint8_t c, int scale) {
    if (!surf || scale < 1) return;
    for (int i = 0; s[i]; i++) {
        const uint8_t* glyph = font8x8_data[(uint8_t)s[i]];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < 8; col++) {
                if (!(bits & (0x80 >> col))) continue;
                if (scale == 1) game_pset(x + i * 8 + col, y + row, c);
                else game_fill(x + (i * 8 + col) * scale, y + row * scale,
                               scale, scale, c);
            }
        }
    }
}

void game_text(int x, int y, const char* s, uint8_t c) {
    game_text_big(x, y, s, c, 1);
}

void game_text_center_big(int y, const char* s, uint8_t c, int scale) {
    game_text_big((gw - (int)strlen(s) * 8 * scale) / 2, y, s, c, scale);
}

void game_text_center(int y, const char* s, uint8_t c) {
    game_text_center_big(y, s, c, 1);
}

/* --------------------------------------------------------------------------
 * Present + timing
 * -------------------------------------------------------------------------- */
static inline void copy32(uint32_t* dst, const uint32_t* src, int count) {
    __asm__ volatile("rep movsl"
                     : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
}

static void sfx_update(void) {
    if (sfx_active && (int32_t)(system_ticks - sfx_off_tick) >= 0)
        game_sfx_stop();
}

void game_present(void) {
    if (!surf) return;
    uint32_t* bb = fb_get_backbuffer();
    int stride = (int)fb_get_width();

    for (int y = 0; y < gh; y++) {
        const uint8_t* srow = surf + y * gw;
        uint32_t* drow = bb + (goy + y * gscale) * stride + gox;
        uint32_t* d = drow;
        for (int x = 0; x < gw; x++) {
            uint32_t c = pal32[srow[x]];
            for (int k = 0; k < gscale; k++) *d++ = c;
        }
        for (int k = 1; k < gscale; k++)
            copy32(bb + (goy + y * gscale + k) * stride + gox, drow, gw * gscale);
    }

    if (gpu_active()) gpu_present(gox, goy, gw * gscale, gh * gscale);
    else fb_flip();

    frames_session++; frames_total++; fps_win_frames++;
    if ((system_ticks - fps_win_start) >= 9) {      /* ~0.5 s window */
        fps_now = fps_win_frames * 1000 / ((system_ticks - fps_win_start) * TICK_MS);
        fps_win_frames = 0;
        fps_win_start = system_ticks;
    }
    sfx_update();
}

void game_sync(void) {
    uint32_t t = system_ticks;
    while (system_ticks == t) __asm__ volatile("hlt");
    sfx_update();
}

uint32_t game_ms(void)     { return system_ticks * TICK_MS; }
uint32_t game_fps(void)    { return fps_now; }
uint32_t game_frames(void) { return frames_session; }

/* --------------------------------------------------------------------------
 * Sound effects
 * -------------------------------------------------------------------------- */
void game_sfx(uint32_t freq_hz, uint32_t ticks) {
    if (freq_hz < 20 || freq_hz > 20000) return;
    uint32_t divisor = PIT_FREQUENCY / freq_hz;
    port_byte_out(PIT_COMMAND, 0xB6);
    port_byte_out(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    port_byte_out(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));
    uint8_t tmp = port_byte_in(SPEAKER_PORT);
    port_byte_out(SPEAKER_PORT, tmp | 0x03);
    sfx_off_tick = system_ticks + (ticks ? ticks : 1);
    sfx_active = true;
}

void game_sfx_stop(void) {
    uint8_t tmp = port_byte_in(SPEAKER_PORT);
    port_byte_out(SPEAKER_PORT, tmp & 0xFC);
    sfx_active = false;
}

/* --------------------------------------------------------------------------
 * Status
 * -------------------------------------------------------------------------- */
const char* game_status(void) {
    char b[12];
    strcpy(status_buf, "NexusSDL: ");
    if (surf) {
        int_to_str(gw, b);  strcat(status_buf, b);
        strcat(status_buf, "x");
        int_to_str(gh, b);  strcat(status_buf, b);
        strcat(status_buf, " x");
        int_to_str(gscale, b); strcat(status_buf, b);
        strcat(status_buf, " surface open");
    } else {
        strcat(status_buf, "idle, ");
        int_to_str((int)sessions, b); strcat(status_buf, b);
        strcat(status_buf, " sessions, ");
        int_to_str((int)frames_total, b); strcat(status_buf, b);
        strcat(status_buf, " frames presented");
    }
    return status_buf;
}

uint32_t game_total_frames(void) { return frames_total; }
uint32_t game_sessions(void)     { return sessions; }
