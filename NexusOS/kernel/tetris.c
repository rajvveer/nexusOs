/* ============================================================================
 * NexusOS — Tetris (Implementation)
 * ============================================================================
 * 10x20 grid, 7 tetrominoes, rotation, line clear, scoring.
 * ============================================================================ */

#include "tetris.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "speaker.h"

extern volatile uint32_t system_ticks;

#define TW 10
#define TH 16
#define PIECE_COUNT 7

static uint8_t t_board[TH][TW];
static int t_px, t_py, t_rot, t_piece;
static int t_next;
static int t_score, t_lines, t_level;
static bool t_over;
static uint32_t t_last_drop;

/* Piece definitions: 4 rotations x 4 blocks x 2 coords */
static const int8_t pieces[7][4][4][2] = {
    /* I */ {{{0,0},{1,0},{2,0},{3,0}},{{0,0},{0,1},{0,2},{0,3}},{{0,0},{1,0},{2,0},{3,0}},{{0,0},{0,1},{0,2},{0,3}}},
    /* O */ {{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}}},
    /* T */ {{{0,0},{1,0},{2,0},{1,1}},{{0,0},{0,1},{0,2},{1,1}},{{1,0},{0,1},{1,1},{2,1}},{{1,0},{1,1},{1,2},{0,1}}},
    /* S */ {{{1,0},{2,0},{0,1},{1,1}},{{0,0},{0,1},{1,1},{1,2}},{{1,0},{2,0},{0,1},{1,1}},{{0,0},{0,1},{1,1},{1,2}}},
    /* Z */ {{{0,0},{1,0},{1,1},{2,1}},{{1,0},{0,1},{1,1},{0,2}},{{0,0},{1,0},{1,1},{2,1}},{{1,0},{0,1},{1,1},{0,2}}},
    /* J */ {{{0,0},{0,1},{1,1},{2,1}},{{0,0},{1,0},{0,1},{0,2}},{{0,0},{1,0},{2,0},{2,1}},{{1,0},{1,1},{0,2},{1,2}}},
    /* L */ {{{2,0},{0,1},{1,1},{2,1}},{{0,0},{0,1},{0,2},{1,2}},{{0,0},{1,0},{2,0},{0,1}},{{0,0},{1,0},{1,1},{1,2}}},
};

static const uint8_t piece_colors[] = {
    VGA_LIGHT_CYAN, VGA_YELLOW, VGA_LIGHT_MAGENTA, VGA_LIGHT_GREEN,
    VGA_LIGHT_RED, VGA_LIGHT_BLUE, VGA_BROWN
};

static uint32_t t_rng_state;
static int t_random(int max) { t_rng_state = t_rng_state * 1103515245 + 12345; return ((t_rng_state >> 16) & 0x7FFF) % max; }

static bool t_fits(int piece, int rot, int px, int py) {
    for (int i = 0; i < 4; i++) {
        int x = px + pieces[piece][rot][i][0];
        int y = py + pieces[piece][rot][i][1];
        if (x < 0 || x >= TW || y < 0 || y >= TH) return false;
        if (t_board[y][x]) return false;
    }
    return true;
}

static void t_lock(void) {
    for (int i = 0; i < 4; i++) {
        int x = t_px + pieces[t_piece][t_rot][i][0];
        int y = t_py + pieces[t_piece][t_rot][i][1];
        if (x >= 0 && x < TW && y >= 0 && y < TH)
            t_board[y][x] = t_piece + 1;
    }
    /* Clear lines */
    int cleared = 0;
    for (int y = TH - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < TW; x++) if (!t_board[y][x]) { full = false; break; }
        if (full) {
            cleared++;
            for (int yy = y; yy > 0; yy--)
                for (int x = 0; x < TW; x++) t_board[yy][x] = t_board[yy-1][x];
            for (int x = 0; x < TW; x++) t_board[0][x] = 0;
            y++; /* Recheck this row */
        }
    }
    if (cleared) { t_lines += cleared; t_score += cleared * cleared * 100; t_level = t_lines / 5 + 1; beep(880, 2); }
    /* Next piece */
    t_piece = t_next; t_next = t_random(PIECE_COUNT);
    t_px = TW / 2 - 1; t_py = 0; t_rot = 0;
    if (!t_fits(t_piece, t_rot, t_px, t_py)) t_over = true;
}

static void t_init(void) {
    for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) t_board[y][x] = 0;
    t_rng_state = system_ticks;
    t_piece = t_random(PIECE_COUNT); t_next = t_random(PIECE_COUNT);
    t_px = TW / 2 - 1; t_py = 0; t_rot = 0;
    t_score = 0; t_lines = 0; t_level = 1; t_over = false;
    t_last_drop = system_ticks;
}

static void t_update(void) {
    if (t_over) return;
    uint32_t speed = 12 - t_level; if (speed < 2) speed = 2;
    if (system_ticks - t_last_drop >= speed) {
        t_last_drop = system_ticks;
        if (t_fits(t_piece, t_rot, t_px, t_py + 1)) t_py++;
        else t_lock();
    }
}

static void t_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id; (void)cw; (void)ch;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);

    t_update();
    int row = cy;

    /* Board */
    for (int y = 0; y < TH && row < cy + ch - 2; y++) {
        gui_putchar(cx, row, (char)0xB3, dim);
        for (int x = 0; x < TW; x++) {
            char c = ' '; uint8_t col = tc;
            if (t_board[y][x]) {
                c = (char)0xDB; col = VGA_COLOR(piece_colors[t_board[y][x]-1], bg);
            }
            /* Draw current piece */
            for (int i = 0; i < 4; i++) {
                int bx = t_px + pieces[t_piece][t_rot][i][0];
                int by = t_py + pieces[t_piece][t_rot][i][1];
                if (bx == x && by == y) { c = (char)0xDB; col = VGA_COLOR(piece_colors[t_piece], bg); }
            }
            gui_putchar(cx + 1 + x, row, c, col);
        }
        gui_putchar(cx + TW + 1, row, (char)0xB3, dim);

        /* Side panel */
        if (y == 0) { gui_draw_text(cx + TW + 3, row, "SCORE", VGA_COLOR(VGA_LIGHT_CYAN, bg)); }
        if (y == 1) { char s[8]; int_to_str(t_score, s); gui_draw_text(cx + TW + 3, row, s, VGA_COLOR(VGA_YELLOW, bg)); }
        if (y == 3) { gui_draw_text(cx + TW + 3, row, "LINES", VGA_COLOR(VGA_LIGHT_CYAN, bg)); }
        if (y == 4) { char s[8]; int_to_str(t_lines, s); gui_draw_text(cx + TW + 3, row, s, tc); }
        if (y == 6) { gui_draw_text(cx + TW + 3, row, "LEVEL", VGA_COLOR(VGA_LIGHT_CYAN, bg)); }
        if (y == 7) { char s[8]; int_to_str(t_level, s); gui_draw_text(cx + TW + 3, row, s, tc); }
        if (y == 9) { gui_draw_text(cx + TW + 3, row, "NEXT", dim); }
        if (y >= 10 && y <= 11) {
            for (int i = 0; i < 4; i++) {
                int bx = pieces[t_next][0][i][0];
                int by = pieces[t_next][0][i][1];
                if (by == y - 10) gui_putchar(cx + TW + 3 + bx, row, (char)0xDB, VGA_COLOR(piece_colors[t_next], bg));
            }
        }
        row++;
    }
    /* Bottom border */
    gui_putchar(cx, row, (char)0xC0, dim);
    for (int x = 0; x < TW; x++) gui_putchar(cx + 1 + x, row, (char)0xC4, dim);
    gui_putchar(cx + TW + 1, row, (char)0xD9, dim);
    row++;

    if (t_over) gui_draw_text(cx, row, "GAME OVER! R:Restart", VGA_COLOR(VGA_LIGHT_RED, bg));
    else gui_draw_text(cx, row, "\x1B\x1A:Move \x18:Rot Spc:Drop", dim);
}

static void t_key(int id, char key) {
    (void)id;
    if (key == 'r' || key == 'R') { t_init(); return; }
    if (t_over) return;
    if ((unsigned char)key == 0x82 && t_fits(t_piece, t_rot, t_px - 1, t_py)) t_px--;
    if ((unsigned char)key == 0x83 && t_fits(t_piece, t_rot, t_px + 1, t_py)) t_px++;
    if ((unsigned char)key == 0x80) {
        int nr = (t_rot + 1) % 4;
        if (t_fits(t_piece, nr, t_px, t_py)) t_rot = nr;
    }
    if ((unsigned char)key == 0x81) { while (t_fits(t_piece, t_rot, t_px, t_py + 1)) t_py++; t_lock(); }
    if (key == ' ') { while (t_fits(t_piece, t_rot, t_px, t_py + 1)) t_py++; t_lock(); }
}

int tetris_open(void) {
    t_init();
    return window_create("Tetris", 14, 2, TW + 14, TH + 4, t_draw, t_key);
}
