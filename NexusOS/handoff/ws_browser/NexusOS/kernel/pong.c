/* ============================================================================
 * NexusOS — Pong Game (Implementation)
 * ============================================================================
 * Single-player pong vs AI.
 * ============================================================================ */

#include "pong.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "speaker.h"
#include "appui.h"

extern volatile uint32_t system_ticks;

#define PG_W 28
#define PG_H 12
#define PADDLE_H 3

static int p_player_y, p_ai_y;
static int p_ball_x, p_ball_y;
static int p_dx, p_dy;
static int p_score_p, p_score_a;
static bool p_paused;
static uint32_t p_last_tick;

static void pg_reset_ball(void) {
    p_ball_x = PG_W / 2; p_ball_y = PG_H / 2;
    p_dx = (p_score_p + p_score_a) % 2 ? 1 : -1;
    p_dy = 1;
    p_last_tick = system_ticks;
}

static void pg_init(void) {
    p_player_y = PG_H / 2 - 1; p_ai_y = PG_H / 2 - 1;
    p_score_p = 0; p_score_a = 0; p_paused = false;
    pg_reset_ball();
}

static void pg_update(void) {
    if (p_paused) return;
    uint32_t speed = 4;
    if (system_ticks - p_last_tick < speed) return;
    p_last_tick = system_ticks;

    /* Move ball */
    p_ball_x += p_dx; p_ball_y += p_dy;

    /* Top/bottom bounce */
    if (p_ball_y <= 0) { p_ball_y = 0; p_dy = 1; }
    if (p_ball_y >= PG_H - 1) { p_ball_y = PG_H - 1; p_dy = -1; }

    /* Player paddle (left) */
    if (p_ball_x <= 1 && p_ball_y >= p_player_y && p_ball_y < p_player_y + PADDLE_H) {
        p_dx = 1; beep(440, 1);
    }
    /* AI paddle (right) */
    if (p_ball_x >= PG_W - 2 && p_ball_y >= p_ai_y && p_ball_y < p_ai_y + PADDLE_H) {
        p_dx = -1; beep(440, 1);
    }

    /* Scoring */
    if (p_ball_x <= 0) { p_score_a++; beep(220, 3); pg_reset_ball(); }
    if (p_ball_x >= PG_W - 1) { p_score_p++; beep(660, 3); pg_reset_ball(); }

    /* AI: follow ball with slight delay */
    int ai_center = p_ai_y + PADDLE_H / 2;
    if (ai_center < p_ball_y && p_ai_y < PG_H - PADDLE_H) p_ai_y++;
    else if (ai_center > p_ball_y && p_ai_y > 0) p_ai_y--;
}

static void pg_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    appui_theme_t ui = appui_theme();
    uint8_t tc = ui.text, bg = ui.bg;
    uint8_t dim = ui.muted;
    uint8_t ball_col = VGA_COLOR(VGA_YELLOW, bg);
    uint8_t pad_col = VGA_COLOR(VGA_LIGHT_GREEN, bg);

    pg_update();

    /* Score */
    char sc[20]; strcpy(sc, "You:");
    char n1[4]; int_to_str(p_score_p, n1); strcat(sc, n1);
    strcat(sc, "  CPU:");
    char n2[4]; int_to_str(p_score_a, n2); strcat(sc, n2);
    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "Pong", sc);
    int row = cy + 3;

    /* Field */
    for (int y = 0; y < PG_H && row < cy + ch - 1; y++) {
        for (int x = 0; x < PG_W; x++) {
            char c = ' '; uint8_t col = tc;
            /* Center line */
            if (x == PG_W / 2) { c = (char)0xB3; col = dim; }
            /* Player paddle */
            if (x == 0 && y >= p_player_y && y < p_player_y + PADDLE_H) { c = (char)0xDB; col = pad_col; }
            /* AI paddle */
            if (x == PG_W - 1 && y >= p_ai_y && y < p_ai_y + PADDLE_H) { c = (char)0xDB; col = VGA_COLOR(VGA_LIGHT_RED, bg); }
            /* Ball */
            if (x == p_ball_x && y == p_ball_y) { c = '\x04'; col = ball_col; }
            gui_putchar(cx + x, row, c, col);
        }
        row++;
    }

    appui_status(cx, cy + ch - 1, cw, p_paused ? "P Resume   Up/Down Move" : "P Pause   Up/Down Move");
}

static void pg_key(int id, char key) {
    (void)id;
    if (key == 'p' || key == 'P') p_paused = !p_paused;
    if ((unsigned char)key == 0x80 && p_player_y > 0) p_player_y--;
    if ((unsigned char)key == 0x81 && p_player_y < PG_H - PADDLE_H) p_player_y++;
    if (key == 'r' || key == 'R') pg_init();
}

int pong_open(void) {
    pg_init();
    return window_create("Pong", 16, 4, PG_W + 6, PG_H + 7, pg_draw, pg_key);
}
