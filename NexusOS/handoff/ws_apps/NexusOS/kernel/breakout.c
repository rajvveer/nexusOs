/* ============================================================================
 * NexusOS — Breakout (Implementation) — Phase 42
 * ============================================================================
 * Brick-breaker on NexusSDL: Q16 ball physics, paddle-english rebounds,
 * palette-ramp brick rows, speaker sfx. Deliberately small (~200 lines) —
 * this file is the "how to write a NexusOS game" reference.
 * ============================================================================ */

#include "breakout.h"
#include "game.h"
#include "vga.h"
#include "string.h"
#include "framebuffer.h"

#define SW        320
#define SH        200
#define ROWS      6
#define COLS      10
#define BRICK_W   30
#define BRICK_H   10
#define BRICK_X0  10
#define BRICK_Y0  24
#define PAD_W     44
#define PAD_H     6
#define PAD_Y     186
#define BALL_SZ   4

static bool bricks[ROWS][COLS];
static int  bricks_left, score, lives;
static int  pad_x;                       /* paddle left edge (pixels)   */
static fx_t bx, by, bvx, bvy;            /* ball pos/vel (Q16 pixels)   */
static bool ball_stuck;                  /* riding the paddle pre-serve */

static const uint8_t row_ramp[ROWS] = {
    RAMP_RED, RAMP_ORANGE, RAMP_YELLOW, RAMP_GREEN, RAMP_TEAL, RAMP_BLUE
};

static void level_reset(bool full) {
    if (full) {
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) bricks[r][c] = true;
        bricks_left = ROWS * COLS;
        score = 0; lives = 3;
    }
    pad_x = (SW - PAD_W) / 2;
    ball_stuck = true;
    bx = FX(SW / 2); by = FX(PAD_Y - BALL_SZ);
    bvx = FX(3); bvy = FX(-4);
}

static void serve(void) {
    ball_stuck = false;
    bvx = ((int)(game_rand() & 1) ? FX(3) : FX(-3));
    bvy = FX(-4);
    game_sfx(800, 1);
}

static void draw_frame(int frame) {
    game_clear(game_ramp(RAMP_GRAY, 15));
    game_rect(0, 14, SW, SH - 14, game_ramp(RAMP_GRAY, 10));

    /* header */
    char b[12], line[28];
    game_fill(0, 0, SW, 14, game_ramp(RAMP_GRAY, 13));
    strcpy(line, "SCORE "); int_to_str(score, b); strcat(line, b);
    game_text(8, 3, line, 14);
    strcpy(line, "LIVES "); int_to_str(lives, b); strcat(line, b);
    game_text(130, 3, line, 12);
    strcpy(line, "FPS "); int_to_str((int)game_fps(), b); strcat(line, b);
    game_text(264, 3, line, 7);

    /* bricks: bright top edge, dark bottom edge for depth */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            if (!bricks[r][c]) continue;
            int x = BRICK_X0 + c * BRICK_W, y = BRICK_Y0 + r * BRICK_H;
            game_fill(x + 1, y + 1, BRICK_W - 2, BRICK_H - 2, game_ramp(row_ramp[r], 4));
            game_hline(x + 1, y + 1, BRICK_W - 2, game_ramp(row_ramp[r], 1));
            game_hline(x + 1, y + BRICK_H - 2, BRICK_W - 2, game_ramp(row_ramp[r], 9));
        }

    /* paddle + ball */
    game_fill(pad_x, PAD_Y, PAD_W, PAD_H, game_ramp(RAMP_STEEL, 4));
    game_hline(pad_x, PAD_Y, PAD_W, game_ramp(RAMP_STEEL, 1));
    game_fill(FX_INT(bx) - BALL_SZ / 2, FX_INT(by) - BALL_SZ / 2,
              BALL_SZ, BALL_SZ, game_ramp(RAMP_YELLOW, 1));

    if (ball_stuck && (frame & 8))
        game_text_center(120, "CTRL / SPACE  SERVE", 15);
}

static void ball_update(void) {
    if (ball_stuck) {
        bx = FX(pad_x + PAD_W / 2);
        by = FX(PAD_Y - BALL_SZ);
        return;
    }
    bx += bvx; by += bvy;
    int x = FX_INT(bx), y = FX_INT(by);

    /* walls (play field: x 1..319, y 15..) */
    if (x <= 2)      { bx = FX(2);      bvx = -bvx; game_sfx(600, 1); }
    if (x >= SW - 3) { bx = FX(SW - 3); bvx = -bvx; game_sfx(600, 1); }
    if (y <= 17)     { by = FX(17);     bvy = -bvy; game_sfx(600, 1); }

    /* paddle: rebound angle from hit position ("english") */
    if (bvy > 0 && y >= PAD_Y - BALL_SZ / 2 && y <= PAD_Y + PAD_H &&
        x >= pad_x - 2 && x <= pad_x + PAD_W + 2) {
        int off = x - (pad_x + PAD_W / 2);           /* -22 .. 22 */
        bvx = FX(off) / 6;
        if (bvx > -FX_ONE && bvx < FX_ONE) bvx = (off >= 0) ? FX_ONE : -FX_ONE;
        bvy = -bvy;
        by = FX(PAD_Y - BALL_SZ / 2);
        game_sfx(800, 1);
    }

    /* bricks */
    int c = (x - BRICK_X0) / BRICK_W, r = (y - BRICK_Y0) / BRICK_H;
    if (r >= 0 && r < ROWS && c >= 0 && c < COLS && bricks[r][c]) {
        bricks[r][c] = false;
        bricks_left--;
        score += (ROWS - r) * 10;
        bvy = -bvy;
        game_sfx(1000 + (ROWS - r) * 100, 1);
        /* speed up slightly every 10 bricks */
        if ((bricks_left % 10) == 0) {
            bvx += (bvx > 0) ? 13107 : -13107;
            bvy += (bvy > 0) ? 13107 : -13107;
        }
    }

    /* lost below the paddle */
    if (y > SH + 4) {
        lives--;
        game_sfx(200, 2);
        if (lives > 0) level_reset(false);
    }
}

void breakout_run(void) {
    if (!fb_is_vesa()) { vga_print("  breakout needs VESA mode.\n"); return; }
    if (!game_open(SW, SH, "NEXUS BREAKOUT - Phase 42 Gaming Framework (Esc quits)")) {
        vga_print("  breakout: could not open game surface.\n");
        return;
    }

    game_srand(game_ms() | 1);
    level_reset(true);
    uint32_t t0 = game_ms();
    enum { ST_PLAY, ST_OVER, ST_WIN } state = ST_PLAY;
    bool quit = false;
    int frame = 0;

    while (!quit) {
        gamepad_poll();
        if (gamepad_pressed(PAD_SELECT)) break;

        if (state == ST_PLAY) {
            if (gamepad_held(PAD_LEFT)  || gamepad_held(PAD_L)) pad_x -= 7;
            if (gamepad_held(PAD_RIGHT) || gamepad_held(PAD_R)) pad_x += 7;
            if (pad_x < 2) pad_x = 2;
            if (pad_x > SW - PAD_W - 2) pad_x = SW - PAD_W - 2;
            if (ball_stuck && (gamepad_pressed(PAD_A) || gamepad_pressed(PAD_B) ||
                               gamepad_pressed(PAD_START)))
                serve();

            ball_update();
            if (bricks_left == 0) state = ST_WIN;
            else if (lives <= 0)  state = ST_OVER;

            draw_frame(frame);
        } else {
            draw_frame(frame);
            game_fill(60, 80, 200, 56, game_ramp(RAMP_GRAY, 14));
            game_rect(60, 80, 200, 56, game_ramp(RAMP_GRAY, 8));
            if (state == ST_WIN)
                game_text_center_big(88, "YOU WIN!", game_ramp(RAMP_GREEN, 1), 2);
            else
                game_text_center_big(88, "GAME OVER", game_ramp(RAMP_RED, 1), 2);
            if (frame & 8)
                game_text_center(116, "ENTER RESTART   ESC QUIT", 15);
            if (gamepad_pressed(PAD_START)) {
                level_reset(true);
                state = ST_PLAY;
            }
        }

        game_present();
        game_sync();
        frame++;
    }

    int final_score = score, final_left = bricks_left;
    bool won = (state == ST_WIN);
    uint32_t frames = game_frames(), ms = game_ms() - t0;
    game_close();

    vga_clear();
    char b[12];
    vga_print_color("\n  Breakout - Phase 42\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ===================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  Result:   ");
    if (won) vga_print_color("ALL BRICKS CLEARED\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    else { vga_print((char*)(final_left ? "game over, " : "quit, "));
           int_to_str(final_left, b); vga_print(b); vga_print(" bricks left\n"); }
    vga_print("  Score:    "); int_to_str(final_score, b); vga_print(b); vga_print("\n");
    vga_print("  Frames:   "); int_to_str((int)frames, b); vga_print(b);
    if (ms > 0) {
        vga_print(" in "); int_to_str((int)(ms / 1000), b); vga_print(b);
        vga_print("s");
    }
    vga_print("\n\n");
}
