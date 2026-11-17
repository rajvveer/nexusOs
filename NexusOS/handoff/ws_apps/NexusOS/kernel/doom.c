/* ============================================================================
 * NexusOS — NEXUSDOOM (Implementation) — Phase 42
 * ============================================================================
 * The Era 5 finale: a Doom-style FPS written from scratch on the NexusSDL
 * framework (game.h). Everything is Q16.16 integer math — no FPU, no libm,
 * no libgcc:
 *
 *   - grid raycaster: one DDA walk per screen column, perpendicular distance,
 *     textured wall columns with per-side + per-distance palette shading
 *   - procedural 64x64 wall textures (brick / stone / tech / exit door)
 *     and 32x32 billboard demons (normal + hit-flash frames)
 *   - sprites z-tested against the per-column wall depth buffer
 *   - hitscan pistol, chasing demons that melee, HUD, toggleable minimap
 *   - title / playing / dead / victory states; bump the glowing EXIT door
 *     after clearing all demons to win
 *
 * Runs tick-locked at ~18 fps (one PIT tick per frame, 55 ms) so the
 * simulation is deterministic. Heap use: one 320x200 surface (framework)
 * plus an 18 KB texture arena here — small on purpose, see SESSION_CONTEXT.
 * ============================================================================ */

#include "doom.h"
#include "game.h"
#include "audio.h"
#include "heap.h"
#include "vga.h"
#include "string.h"
#include "framebuffer.h"
#include "font8x8.h"

/* --------------------------------------------------------------------------
 * Tunables
 * -------------------------------------------------------------------------- */
#define SW          320
#define SH          200
#define HUD_H       14
#define VIEW_H      (SH - HUD_H)          /* 186 */
#define HORIZON     (VIEW_H / 2)          /* 93  */

#define MAP_W       24
#define MAP_H       24
#define MAX_IMPS    6
#define MAX_DDA     48

#define PLY_RADIUS  14418                  /* 0.22 cells (Q16)              */
#define IMP_RADIUS  19660                  /* 0.30 cells                    */
#define MOVE_SPEED  10486                  /* 0.16 cells / frame  (~2.9/s)  */
#define STRAFE_SPD  7864                   /* 0.12 cells / frame            */
#define TURN_SPEED  22                     /* of 1024 units (~7.7°/frame)   */
#define IMP_SPEED   3277                   /* 0.05 cells / frame            */

/* Cell types */
#define C_FLOOR 0
#define C_BRICK 1
#define C_STONE 2
#define C_TECH  3
#define C_EXIT  4

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */
static uint8_t world[MAP_H][MAP_W];

typedef struct {
    fx_t x, y;
    int  hp;
    bool alive, aggro;
    int  flash, atk_cd;
} imp_t;

static imp_t imps[MAX_IMPS];
static const fx_t imp_spawn[MAX_IMPS][2] = {
    { FX(12) + 32768, FX(12) + 32768 },    /* central ring     */
    { FX(18) + 32768, FX(7)  + 32768 },    /* below monolith   */
    { FX(20) + 32768, FX(12) + 32768 },    /* east wing        */
    { FX(4)  + 32768, FX(20) + 32768 },    /* south-west       */
    { FX(14) + 32768, FX(19) + 32768 },    /* exit approach    */
    { FX(9)  + 32768, FX(4)  + 32768 },    /* north corridor   */
};

static fx_t px, py;          /* player position (Q16 cells) */
static int  pang;            /* player angle, 0..1023       */
static int  hp, ammo, kills, shots, hits;
static int  gun_recoil, gun_flash, dmg_flash;
static bool show_map, exit_reached;
static char msg[40];
static int  msg_timer;
static uint32_t play_t0;

/* Texture arena (heap): 4 wall textures 64x64 + 2 imp frames 32x32 */
static uint8_t* tex_walls;                 /* [4][64*64] */
static uint8_t* tex_imp;                   /* [2][32*32] */

static fx_t zbuf[SW];

enum { ST_TITLE, ST_PLAY, ST_DEAD, ST_WIN };

/* --------------------------------------------------------------------------
 * Map — open arena with structures, built by code so it is always consistent
 * -------------------------------------------------------------------------- */
static void w_hwall(int x0, int x1, int y, uint8_t t) {
    for (int x = x0; x <= x1; x++) world[y][x] = t;
}
static void w_vwall(int x, int y0, int y1, uint8_t t) {
    for (int y = y0; y <= y1; y++) world[y][x] = t;
}

static void map_build(void) {
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            world[y][x] = (x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1)
                          ? C_BRICK : C_FLOOR;

    /* spawn nook, top-left (stone) */
    w_vwall(6, 1, 5, C_STONE);  world[3][6] = C_FLOOR;
    w_hwall(1, 6, 6, C_STONE);  world[6][3] = C_FLOOR;

    /* north-east tech monolith (solid block) */
    for (int y = 2; y <= 5; y++) w_hwall(16, 21, y, C_TECH);

    /* central stone ring with a gap mid-each-side, tech corner pillars */
    w_hwall(9, 15, 9,  C_STONE); world[9][12]  = C_FLOOR;
    w_hwall(9, 15, 15, C_STONE); world[15][12] = C_FLOOR;
    w_vwall(9, 9, 15,  C_STONE); world[12][9]  = C_FLOOR;
    w_vwall(15, 9, 15, C_STONE); world[12][15] = C_FLOOR;
    world[10][10] = C_TECH; world[10][14] = C_TECH;
    world[14][10] = C_TECH; world[14][14] = C_TECH;

    /* south-west slabs */
    w_hwall(2, 8, 18, C_STONE);  world[18][5]  = C_FLOOR;
    w_vwall(8, 12, 18, C_STONE); world[15][8]  = C_FLOOR;

    /* east wing wall */
    w_vwall(19, 8, 16, C_TECH);  world[10][19] = C_FLOOR; world[14][19] = C_FLOOR;

    /* exit room, bottom-right; the glowing EXIT block sits inside */
    w_hwall(17, 22, 17, C_BRICK); world[17][19] = C_FLOOR;
    w_vwall(17, 17, 22, C_BRICK); world[20][17] = C_FLOOR;
    world[20][20] = C_EXIT;

    /* scattered pillars */
    world[3][12] = C_STONE; world[12][3] = C_STONE; world[21][12] = C_BRICK;
}

/* --------------------------------------------------------------------------
 * Procedural textures
 * -------------------------------------------------------------------------- */
static uint8_t* wall_tex(int t) { return tex_walls + (t & 3) * 64 * 64; }

static void tex_build(void) {
    game_srand(0xD00D);

    /* 0: brick (used for C_BRICK=1) */
    uint8_t* t = wall_tex(0);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int row = y / 8;
            int xx  = (row & 1) ? x + 8 : x;
            bool mortar = (y % 8 == 0) || (xx % 16 == 0);
            uint8_t c = mortar ? game_ramp(RAMP_GRAY, 12)
                               : game_ramp(RAMP_RED, 5 + (int)(game_rand() % 3));
            t[y * 64 + x] = c;
        }
    }
    /* 1: stone blocks */
    t = wall_tex(1);
    for (int by = 0; by < 4; by++) {
        for (int bx = 0; bx < 4; bx++) {
            int base = 5 + (int)(game_rand() % 4);
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    bool edge = (x == 0 || y == 0);
                    int n = (int)(game_rand() % 2);
                    t[(by * 16 + y) * 64 + bx * 16 + x] =
                        edge ? game_ramp(RAMP_GRAY, 13)
                             : game_ramp(RAMP_GRAY, base + n);
                }
        }
    }
    /* 2: tech panels with conduits + lights */
    t = wall_tex(2);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t c = game_ramp(RAMP_STEEL, 7 + ((x / 32 + y / 32) & 1));
            if (x % 16 < 2)  c = game_ramp(RAMP_STEEL, 3);
            if (y % 32 == 0) c = game_ramp(RAMP_STEEL, 11);
            if (x % 16 == 8 && y % 8 == 4) c = game_ramp(RAMP_GREEN, 1);
            t[y * 64 + x] = c;
        }
    }
    /* 3: EXIT door — dark green panel with bright lettering */
    t = wall_tex(3);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint8_t c = game_ramp(RAMP_SLIME, 12 + ((x + y) & 1));
            if (x < 2 || x > 61 || y < 2 || y > 61) c = game_ramp(RAMP_SLIME, 4);
            t[y * 64 + x] = c;
        }
    {   /* "EXIT" 4 chars x 8 px, scaled x2 = 64x16, centered vertically */
        const char* s = "EXIT";
        for (int i = 0; i < 4; i++) {
            const uint8_t* glyph = font8x8_data[(uint8_t)s[i]];
            for (int row = 0; row < 8; row++)
                for (int col = 0; col < 8; col++) {
                    if (!(glyph[row] & (0x80 >> col))) continue;
                    int gx = i * 16 + col * 2, gy = 24 + row * 2;
                    for (int k = 0; k < 4; k++)
                        t[(gy + (k >> 1)) * 64 + gx + (k & 1)] =
                            game_ramp(RAMP_SLIME, 0);
                }
        }
    }

    /* Imp billboards: frame 0 normal, frame 1 = hit flash (white-hot).
     * Index 0 = transparent. */
    for (int f = 0; f < 2; f++) {
        uint8_t* s = tex_imp + f * 32 * 32;
        for (int i = 0; i < 32 * 32; i++) s[i] = 0;
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                int dx = x - 16, dy;
                uint8_t c = 0;
                /* torso: ellipse centered (16,20) */
                dy = y - 20;
                if (dx * dx * 4 + dy * dy * 9 <= 36 * 16)
                    c = game_ramp(RAMP_BROWN, 5 + ((x ^ y) & 3));
                /* legs */
                if (y >= 26 && y <= 31 && ((x >= 10 && x <= 13) || (x >= 19 && x <= 22)))
                    c = game_ramp(RAMP_BROWN, 9);
                /* arms */
                if (y >= 16 && y <= 24 && (x <= 6 || x >= 26) && (x >= 3 && x <= 28))
                    c = game_ramp(RAMP_BROWN, 7);
                /* head: circle centered (16,9) r6 */
                dy = y - 9;
                if (dx * dx + dy * dy <= 36)
                    c = game_ramp(RAMP_FLESH, 6 + ((x + y) & 1));
                /* horns */
                if (y >= 2 && y <= 5 && (x == 10 + (5 - y) || x == 21 - (5 - y)))
                    c = game_ramp(RAMP_YELLOW, 3);
                /* eyes */
                if (y == 8 && (x == 13 || x == 19)) c = game_ramp(RAMP_YELLOW, 0);
                /* mouth */
                if (y == 12 && x >= 14 && x <= 18) c = game_ramp(RAMP_BLOOD, 10);
                if (c && f == 1) c = game_ramp(RAMP_GRAY, (c & 15) >> 3);
                if (c) s[y * 32 + x] = c;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Collision
 * -------------------------------------------------------------------------- */
static bool solid_at(fx_t x, fx_t y) {
    int cx = x >> 16, cy = y >> 16;
    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return true;
    return world[cy][cx] != C_FLOOR;
}

static bool can_stand(fx_t x, fx_t y, fx_t r) {
    return !solid_at(x - r, y - r) && !solid_at(x + r, y - r) &&
           !solid_at(x - r, y + r) && !solid_at(x + r, y + r);
}

/* Does the blocked spot touch the EXIT door? */
static bool touches_exit(fx_t x, fx_t y, fx_t r) {
    for (int i = 0; i < 4; i++) {
        int cx = (x + ((i & 1) ? r : -r)) >> 16;
        int cy = (y + ((i & 2) ? r : -r)) >> 16;
        if (cx >= 0 && cy >= 0 && cx < MAP_W && cy < MAP_H &&
            world[cy][cx] == C_EXIT) return true;
    }
    return false;
}

static void set_msg(const char* s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(msg) - 1) { msg[i] = s[i]; i++; }
    msg[i] = '\0';
    msg_timer = 36;            /* ~2 s */
}

/* --------------------------------------------------------------------------
 * Rendering
 * -------------------------------------------------------------------------- */
static void render_world(fx_t cx, fx_t cy, int ang) {
    fx_t dirx = game_cos(ang),            diry = game_sin(ang);
    fx_t planx = fx_mul(-diry, 43254),    plany = fx_mul(dirx, 43254); /* 0.66 */

    /* flat ceiling + floor, fading dark toward the horizon */
    for (int y = 0; y < HORIZON; y++)
        game_hline(0, y, SW, game_ramp(RAMP_GRAY, 8 + y * 8 / HORIZON));
    for (int y = HORIZON; y < VIEW_H; y++)
        game_hline(0, y, SW, game_ramp(RAMP_WOOD, 15 - (y - HORIZON) * 9 / HORIZON));

    for (int x = 0; x < SW; x++) {
        fx_t camx = fx_div(2 * x - SW, SW);
        fx_t rdx = dirx + fx_mul(planx, camx);
        fx_t rdy = diry + fx_mul(plany, camx);
        if (rdx > -64 && rdx < 64) rdx = (rdx < 0) ? -64 : 64;
        if (rdy > -64 && rdy < 64) rdy = (rdy < 0) ? -64 : 64;

        fx_t deltax = fx_div(FX_ONE, rdx); if (deltax < 0) deltax = -deltax;
        fx_t deltay = fx_div(FX_ONE, rdy); if (deltay < 0) deltay = -deltay;

        int mapx = cx >> 16, mapy = cy >> 16;
        int stepx, stepy, side = 0;
        fx_t sidex, sidey;

        if (rdx < 0) { stepx = -1; sidex = fx_mul(cx - FX(mapx), deltax); }
        else         { stepx =  1; sidex = fx_mul(FX(mapx + 1) - cx, deltax); }
        if (rdy < 0) { stepy = -1; sidey = fx_mul(cy - FX(mapy), deltay); }
        else         { stepy =  1; sidey = fx_mul(FX(mapy + 1) - cy, deltay); }

        uint8_t cell = C_BRICK;
        for (int i = 0; i < MAX_DDA; i++) {
            if (sidex < sidey) { sidex += deltax; mapx += stepx; side = 0; }
            else               { sidey += deltay; mapy += stepy; side = 1; }
            if (mapx < 0 || mapy < 0 || mapx >= MAP_W || mapy >= MAP_H) break;
            if (world[mapy][mapx] != C_FLOOR) { cell = world[mapy][mapx]; break; }
        }

        fx_t dist = side ? sidey - deltay : sidex - deltax;
        if (dist < 1024) dist = 1024;                       /* min 0.016     */
        zbuf[x] = dist;

        int lineh = fx_div(VIEW_H, dist);                    /* pixels        */
        int y0 = HORIZON - lineh / 2, y1 = y0 + lineh;
        int cy0 = y0 < 0 ? 0 : y0;
        int cy1 = y1 > VIEW_H - 1 ? VIEW_H - 1 : y1;

        /* texture column */
        fx_t wallx = side ? cx + fx_mul(dist, rdx) : cy + fx_mul(dist, rdy);
        int texx = ((wallx & 0xFFFF) * 64) >> 16;
        if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) texx = 63 - texx;

        const uint8_t* tx = wall_tex(cell - 1) + texx;       /* column base   */
        fx_t tstep = fx_div(64, lineh ? lineh : 1);
        fx_t tpos  = (cy0 - y0) * tstep;

        int shade = (int)(dist >> 17);                       /* +1 per 2 cells*/
        if (shade > 9) shade = 9;
        if (side) shade += 1;

        uint8_t* dst = game_surface() + cy0 * SW + x;
        for (int y = cy0; y <= cy1; y++) {
            int texy = (tpos >> 16) & 63;
            tpos += tstep;
            *dst = game_darken(tx[texy * 64], shade);
            dst += SW;
        }
    }
}

static void render_imps(fx_t cx, fx_t cy, int ang) {
    fx_t dirx = game_cos(ang),         diry = game_sin(ang);
    fx_t planx = fx_mul(-diry, 43254), plany = fx_mul(dirx, 43254);
    fx_t det = fx_mul(planx, diry) - fx_mul(dirx, plany);
    if (det == 0) return;
    fx_t invdet = fx_div(FX_ONE, det);

    /* far-to-near ordering (insertion sort on squared distance) */
    int order[MAX_IMPS], n = 0;
    for (int i = 0; i < MAX_IMPS; i++) {
        if (!imps[i].alive) continue;
        order[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = order[i], j = i - 1;
        fx_t dvx = (imps[v].x - cx) >> 8, dvy = (imps[v].y - cy) >> 8;
        fx_t dv = dvx * dvx + dvy * dvy;
        while (j >= 0) {
            fx_t dux = (imps[order[j]].x - cx) >> 8, duy = (imps[order[j]].y - cy) >> 8;
            if (dux * dux + duy * duy >= dv) break;
            order[j + 1] = order[j]; j--;
        }
        order[j + 1] = v;
    }

    for (int o = 0; o < n; o++) {
        imp_t* im = &imps[order[o]];
        fx_t relx = im->x - cx, rely = im->y - cy;
        fx_t try_ = fx_mul(invdet, fx_mul(-plany, relx) + fx_mul(planx, rely));
        if (try_ < 13107) continue;                          /* behind/too close */
        fx_t trx = fx_mul(invdet, fx_mul(diry, relx) - fx_mul(dirx, rely));

        int sx = SW / 2 + FX_INT(fx_mul(fx_div(trx, try_), FX(SW / 2)));
        int size = fx_div(VIEW_H, try_);
        if (size < 2 || size > 600) continue;
        int x0 = sx - size / 2, y0 = HORIZON - size / 2;

        const uint8_t* tex = tex_imp + (im->flash > 0 ? 32 * 32 : 0);
        int shade = (int)(try_ >> 17);
        if (shade > 8) shade = 8;
        if (im->flash > 0) shade = 0;

        fx_t tstep = fx_div(32, size);
        for (int x = x0 < 0 ? 0 : x0; x < x0 + size && x < SW; x++) {
            if (try_ >= zbuf[x]) continue;
            int texx = ((x - x0) * tstep) >> 16;
            if (texx < 0)  texx = 0;
            if (texx > 31) texx = 31;
            fx_t tpos = (y0 < 0 ? -y0 : 0) * tstep;
            uint8_t* dst = game_surface() + (y0 < 0 ? 0 : y0) * SW + x;
            for (int y = y0 < 0 ? 0 : y0; y < y0 + size && y < VIEW_H; y++) {
                int texy = (tpos >> 16) & 31;
                tpos += tstep;
                uint8_t c = tex[texy * 32 + texx];
                if (c) *dst = game_darken(c, shade);
                dst += SW;
            }
        }
    }
}

static void draw_gun(void) {
    int gy = VIEW_H - 38 + gun_recoil * 2;
    if (gun_flash > 0) {
        game_fill(150, gy - 12, 20, 12, game_ramp(RAMP_YELLOW, 0));
        game_fill(143, gy - 6, 34, 6,  game_ramp(RAMP_ORANGE, 2));
    }
    game_fill(150, gy, 20, 26, game_ramp(RAMP_STEEL, 6));     /* barrel   */
    game_fill(153, gy, 4, 26,  game_ramp(RAMP_STEEL, 3));     /* highlight*/
    game_fill(150, gy, 20, 3,  game_ramp(RAMP_STEEL, 10));    /* muzzle   */
    game_fill(144, gy + 22, 32, 12, game_ramp(RAMP_STEEL, 8));/* body     */
    game_fill(154, gy + 34, 14, 10, game_ramp(RAMP_WOOD, 6)); /* grip     */

    uint8_t ch = game_ramp(RAMP_GRAY, 4);                     /* crosshair*/
    game_hline(152, HORIZON, 5, ch);
    game_hline(163, HORIZON, 5, ch);
    game_vline(160, HORIZON - 7, HORIZON - 3, ch);
    game_vline(160, HORIZON + 3, HORIZON + 7, ch);
}

static void draw_hud(void) {
    char b[12], line[36];
    game_fill(0, VIEW_H, SW, HUD_H, game_ramp(RAMP_GRAY, 13));
    game_hline(0, VIEW_H, SW, game_ramp(RAMP_GRAY, 10));

    strcpy(line, "HP ");  int_to_str(hp, b);   strcat(line, b);
    game_text(8, VIEW_H + 3, line, hp > 50 ? 10 : (hp > 25 ? 14 : 12));

    strcpy(line, "AMMO "); int_to_str(ammo, b); strcat(line, b);
    game_text(80, VIEW_H + 3, line, 14);

    strcpy(line, "KILLS "); int_to_str(kills, b); strcat(line, b);
    strcat(line, "/");      int_to_str(MAX_IMPS, b); strcat(line, b);
    game_text(170, VIEW_H + 3, line, 12);

    strcpy(line, "FPS "); int_to_str((int)game_fps(), b); strcat(line, b);
    game_text(264, VIEW_H + 3, line, 7);
}

static void draw_minimap(void) {
    const int s = 4, ox = SW - MAP_W * s - 6, oy = 6;
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            uint8_t c;
            switch (world[y][x]) {
                case C_BRICK: c = game_ramp(RAMP_RED, 7);   break;
                case C_STONE: c = game_ramp(RAMP_GRAY, 8);  break;
                case C_TECH:  c = game_ramp(RAMP_STEEL, 6); break;
                case C_EXIT:  c = game_ramp(RAMP_SLIME, 1); break;
                default:      c = game_ramp(RAMP_GRAY, 15); break;
            }
            game_fill(ox + x * s, oy + y * s, s, s, c);
        }
    for (int i = 0; i < MAX_IMPS; i++) {
        if (!imps[i].alive) continue;
        game_fill(ox + ((imps[i].x * s) >> 16) - 1, oy + ((imps[i].y * s) >> 16) - 1,
                  2, 2, game_ramp(RAMP_RED, 0));
    }
    int mx = ox + ((px * s) >> 16), my = oy + ((py * s) >> 16);
    game_fill(mx - 1, my - 1, 2, 2, 15);
    game_pset(mx + FX_INT(game_cos(pang) * 3), my + FX_INT(game_sin(pang) * 3), 14);
}

/* --------------------------------------------------------------------------
 * Gameplay
 * -------------------------------------------------------------------------- */
static void level_reset(void) {
    map_build();
    px = FX(3) + 32768; py = FX(3) + 32768; pang = 0;
    hp = 100; ammo = 60; kills = 0; shots = 0; hits = 0;
    gun_recoil = 0; gun_flash = 0; dmg_flash = 0;
    show_map = false; exit_reached = false; msg_timer = 0;
    for (int i = 0; i < MAX_IMPS; i++) {
        imps[i].x = imp_spawn[i][0]; imps[i].y = imp_spawn[i][1];
        imps[i].hp = 2; imps[i].alive = true; imps[i].aggro = false;
        imps[i].flash = 0; imps[i].atk_cd = 0;
    }
    play_t0 = game_ms();
}

static void player_move(fx_t dx, fx_t dy) {
    fx_t nx = px + dx, ny = py + dy;
    if (can_stand(nx, py, PLY_RADIUS)) px = nx;
    else if (touches_exit(nx, py, PLY_RADIUS)) goto exit_bump;
    if (can_stand(px, ny, PLY_RADIUS)) py = ny;
    else if (touches_exit(px, ny, PLY_RADIUS)) goto exit_bump;
    return;
exit_bump:
    if (kills >= MAX_IMPS) exit_reached = true;
    else { set_msg("EXIT LOCKED - DEMONS REMAIN"); game_sfx(120, 1); }
}

static void fire_gun(void) {
    if (ammo <= 0) { game_sfx(100, 1); set_msg("OUT OF AMMO"); return; }
    ammo--; shots++;
    gun_recoil = 3; gun_flash = 2;
    game_sfx(900, 1);

    /* hitscan: nearest demon in the center cone, not occluded by a wall */
    fx_t dirx = game_cos(pang),        diry = game_sin(pang);
    fx_t planx = fx_mul(-diry, 43254), plany = fx_mul(dirx, 43254);
    fx_t det = fx_mul(planx, diry) - fx_mul(dirx, plany);
    if (det == 0) return;
    fx_t invdet = fx_div(FX_ONE, det);

    int best = -1; fx_t best_d = FX(64);
    for (int i = 0; i < MAX_IMPS; i++) {
        if (!imps[i].alive) continue;
        fx_t relx = imps[i].x - px, rely = imps[i].y - py;
        fx_t try_ = fx_mul(invdet, fx_mul(-plany, relx) + fx_mul(planx, rely));
        if (try_ < 6554 || try_ > FX(20)) continue;          /* 0.1 .. 20 cells */
        fx_t trx = fx_mul(invdet, fx_mul(diry, relx) - fx_mul(dirx, rely));
        fx_t ratio = fx_div(trx, try_);
        if (ratio < 0) ratio = -ratio;
        if (ratio > 13000) continue;                         /* ~0.2 half-cone  */
        if (try_ >= zbuf[SW / 2] + 32768) continue;          /* wall in the way */
        if (try_ < best_d) { best_d = try_; best = i; }
    }
    if (best >= 0) {
        imp_t* im = &imps[best];
        im->hp--; im->flash = 2; im->aggro = true;
        hits++;
        if (im->hp <= 0) {
            im->alive = false; kills++;
            game_sfx(150, 2);
            set_msg(kills >= MAX_IMPS ? "ALL DEMONS SLAIN - FIND THE EXIT"
                                      : "DEMON DOWN");
        } else {
            game_sfx(600, 1);
        }
    }
}

static void imps_update(void) {
    for (int i = 0; i < MAX_IMPS; i++) {
        imp_t* im = &imps[i];
        if (!im->alive) continue;
        if (im->flash > 0) im->flash--;
        if (im->atk_cd > 0) im->atk_cd--;

        fx_t dx8 = (px - im->x) >> 8, dy8 = (py - im->y) >> 8;
        fx_t d2 = dx8 * dx8 + dy8 * dy8;                     /* Q16 of dist^2 */

        if (d2 < FX(64)) im->aggro = true;                   /* within 8 cells */
        if (!im->aggro) continue;

        if (d2 < 94372) {                                    /* < ~1.2 cells   */
            if (im->atk_cd == 0) {
                hp -= 8; dmg_flash = 3; im->atk_cd = 9;
                game_sfx(220, 1);
            }
            continue;
        }
        /* chase, sliding per-axis */
        fx_t sx = (px > im->x) ? IMP_SPEED : -IMP_SPEED;
        fx_t sy = (py > im->y) ? IMP_SPEED : -IMP_SPEED;
        if (can_stand(im->x + sx, im->y, IMP_RADIUS)) im->x += sx;
        if (can_stand(im->x, im->y + sy, IMP_RADIUS)) im->y += sy;
    }
}

/* --------------------------------------------------------------------------
 * Screens
 * -------------------------------------------------------------------------- */
static void title_screen(int frame) {
    render_world(FX(12), FX(12) + 32768, frame * 3);
    game_fill(0, VIEW_H, SW, HUD_H, game_ramp(RAMP_GRAY, 13));

    game_text_center_big(31, "NEXUSDOOM", 0, 4);             /* drop shadow */
    game_text_center_big(28, "NEXUSDOOM", game_ramp(RAMP_RED, 1), 4);
    game_text_center(70, "A FROM-SCRATCH RAYCASTER - PHASE 42", 7);

    game_text_center(102, "ARROWS / WASD  MOVE + TURN", 15);
    game_text_center(114, "A / D  STRAFE    CTRL / X  FIRE", 15);
    game_text_center(126, "V  MAP    ESC  QUIT", 15);
    game_text_center(144, "CLEAR ALL 6 DEMONS, THEN FIND THE EXIT",
                     game_ramp(RAMP_YELLOW, 2));
    if (frame & 8)
        game_text_center(166, ">> PRESS ENTER <<", game_ramp(RAMP_SLIME, 0));
}

static void end_screen(bool won, int frame) {
    char b[12], line[40];
    if (won) {
        game_fill(0, 0, SW, SH, game_ramp(RAMP_SLIME, 14));
        game_text_center_big(30, "LEVEL CLEARED", game_ramp(RAMP_SLIME, 0), 3);
        uint32_t secs = (game_ms() - play_t0) / 1000;
        strcpy(line, "TIME    "); int_to_str((int)secs, b); strcat(line, b);
        strcat(line, "s"); game_text_center(86, line, 15);
    } else {
        game_fill(0, 0, SW, SH, game_ramp(RAMP_BLOOD, 13));
        game_text_center_big(30, "YOU DIED", game_ramp(RAMP_BLOOD, 1), 4);
        game_text_center(86, "THE DEMONS FEAST TONIGHT", 7);
    }
    strcpy(line, "KILLS   "); int_to_str(kills, b); strcat(line, b);
    strcat(line, "/"); int_to_str(MAX_IMPS, b); strcat(line, b);
    game_text_center(100, line, 15);
    strcpy(line, "SHOTS   "); int_to_str(shots, b); strcat(line, b);
    game_text_center(112, line, 15);
    if (shots > 0) {
        strcpy(line, "ACCURACY "); int_to_str(hits * 100 / shots, b);
        strcat(line, b); strcat(line, "%");
        game_text_center(124, line, 15);
    }
    if (frame & 8)
        game_text_center(156, "ENTER  RESTART     ESC  QUIT", 14);
}

/* --------------------------------------------------------------------------
 * Main entry
 * -------------------------------------------------------------------------- */
void doom_run(void) {
    if (!fb_is_vesa()) { vga_print("  doom needs VESA mode.\n"); return; }

    tex_walls = (uint8_t*)kmalloc(4 * 64 * 64 + 2 * 32 * 32);
    if (!tex_walls) { vga_print("  doom: out of memory.\n"); return; }
    tex_imp = tex_walls + 4 * 64 * 64;

    if (!game_open(SW, SH, "NEXUSDOOM - Phase 42 Gaming Framework (Esc quits)")) {
        kfree(tex_walls); tex_walls = NULL;
        vga_print("  doom: could not open game surface.\n");
        return;
    }

    tex_build();
    level_reset();

    /* Session clock for the exit stats — play_t0 resets on every restart,
     * but game_frames() spans the whole session, so time the session too. */
    uint32_t session_t0 = game_ms();

    int state = ST_TITLE, frame = 0;
    bool quit = false, won = false;

    while (!quit) {
        gamepad_poll();

        switch (state) {
        case ST_TITLE:
            if (gamepad_pressed(PAD_SELECT)) { quit = true; break; }
            if (gamepad_pressed(PAD_START) || gamepad_pressed(PAD_A)) {
                level_reset();
                state = ST_PLAY;
                game_sfx(660, 1);
                break;
            }
            title_screen(frame);
            break;

        case ST_PLAY: {
            if (gamepad_pressed(PAD_SELECT)) { quit = true; break; }

            if (gamepad_held(PAD_LEFT))  pang = (pang - TURN_SPEED) & 1023;
            if (gamepad_held(PAD_RIGHT)) pang = (pang + TURN_SPEED) & 1023;

            fx_t dirx = game_cos(pang), diry = game_sin(pang);
            if (gamepad_held(PAD_UP))
                player_move(fx_mul(dirx, MOVE_SPEED), fx_mul(diry, MOVE_SPEED));
            if (gamepad_held(PAD_DOWN))
                player_move(fx_mul(dirx, -MOVE_SPEED), fx_mul(diry, -MOVE_SPEED));
            if (gamepad_held(PAD_L))           /* strafe = move along plane */
                player_move(fx_mul(diry, STRAFE_SPD), fx_mul(dirx, -STRAFE_SPD));
            if (gamepad_held(PAD_R))
                player_move(fx_mul(diry, -STRAFE_SPD), fx_mul(dirx, STRAFE_SPD));

            if (gamepad_pressed(PAD_Y)) show_map = !show_map;
            if (gamepad_pressed(PAD_A)) fire_gun();

            imps_update();
            if (gun_recoil > 0) gun_recoil--;
            if (gun_flash  > 0) gun_flash--;
            if (dmg_flash  > 0) dmg_flash--;
            if (msg_timer  > 0) msg_timer--;

            if (exit_reached) {
                won = true; state = ST_WIN;
                audio_play_tone(523, 120, 70);
                audio_play_tone(659, 120, 70);
                audio_play_tone(784, 200, 70);
                break;
            }
            if (hp <= 0) {
                won = false; state = ST_DEAD;
                audio_play_tone(140, 400, 70);
                break;
            }

            render_world(px, py, pang);
            render_imps(px, py, pang);
            if (msg_timer > 0) {
                game_text_center(72, msg, 0);
                game_text_center(71, msg, game_ramp(RAMP_YELLOW, 1));
            }
            draw_gun();
            draw_hud();
            if (show_map) draw_minimap();
            if (dmg_flash > 0)
                for (int i = 0; i < 4; i++)
                    game_rect(i, i, SW - 2 * i, VIEW_H - 2 * i,
                              game_ramp(RAMP_BLOOD, 2));
            break;
        }

        case ST_WIN:
        case ST_DEAD:
            if (gamepad_pressed(PAD_SELECT)) { quit = true; break; }
            if (gamepad_pressed(PAD_START)) {
                level_reset();
                state = ST_PLAY;
                break;
            }
            end_screen(won, frame);
            break;
        }

        if (quit) break;
        game_present();
        game_sync();
        frame++;
    }

    uint32_t total_frames = game_frames();
    uint32_t ms = game_ms() - session_t0;
    game_close();
    kfree(tex_walls); tex_walls = NULL;

    vga_clear();
    char b[12];
    vga_print_color("\n  NEXUSDOOM - Phase 42\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ====================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  Result:   ");
    if (won) vga_print_color("LEVEL CLEARED\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    else if (hp <= 0) vga_print_color("KILLED IN ACTION\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    else vga_print("left the arena\n");
    vga_print("  Kills:    "); int_to_str(kills, b); vga_print(b);
    vga_print("/");            int_to_str(MAX_IMPS, b); vga_print(b); vga_print("\n");
    vga_print("  Shots:    "); int_to_str(shots, b); vga_print(b);
    if (shots > 0) {
        vga_print("  (accuracy "); int_to_str(hits * 100 / shots, b);
        vga_print(b); vga_print("%)");
    }
    vga_print("\n  Frames:   "); int_to_str((int)total_frames, b); vga_print(b);
    if (ms > 0) {
        vga_print(" in "); int_to_str((int)(ms / 1000), b); vga_print(b);
        vga_print("s = "); int_to_str((int)(total_frames * 1000 / ms), b); vga_print(b);
        vga_print(" fps (tick-locked)");
    }
    vga_print("\n\n");
}
