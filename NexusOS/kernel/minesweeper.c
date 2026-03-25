/* ============================================================================
 * NexusOS — Minesweeper (Implementation)
 * ============================================================================
 * Classic minesweeper: 10x8 grid, 10 mines, flag/reveal, win/lose.
 * ============================================================================ */

#include "minesweeper.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

#define MS_W 10
#define MS_H 8
#define MS_MINES 10

static int  ms_mines[MS_H][MS_W];    /* 1=mine */
static int  ms_adj[MS_H][MS_W];      /* adjacent mine count */
static int  ms_reveal[MS_H][MS_W];   /* 0=hidden, 1=revealed, 2=flagged */
static int  ms_cx, ms_cy;            /* cursor */
static bool ms_gameover, ms_won;
static int  ms_flags_placed;

static void ms_init_board(void) {
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) {
            ms_mines[y][x] = 0; ms_adj[y][x] = 0; ms_reveal[y][x] = 0;
        }
    /* Place mines using a simple deterministic LCG seeded by ticks */
    extern volatile uint32_t system_ticks;
    uint32_t seed = system_ticks;
    int placed = 0;
    while (placed < MS_MINES) {
        seed = seed * 1103515245 + 12345;
        int px = (seed >> 16) % MS_W;
        seed = seed * 1103515245 + 12345;
        int py = (seed >> 16) % MS_H;
        if (!ms_mines[py][px]) { ms_mines[py][px] = 1; placed++; }
    }
    /* Compute adjacency */
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++) {
            int c = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x+dx, ny = y+dy;
                    if (nx >= 0 && nx < MS_W && ny >= 0 && ny < MS_H)
                        c += ms_mines[ny][nx];
                }
            ms_adj[y][x] = c;
        }
    ms_cx = 0; ms_cy = 0; ms_gameover = false; ms_won = false; ms_flags_placed = 0;
}

static void ms_flood_reveal(int x, int y) {
    if (x < 0 || x >= MS_W || y < 0 || y >= MS_H) return;
    if (ms_reveal[y][x]) return;
    ms_reveal[y][x] = 1;
    if (ms_adj[y][x] == 0 && !ms_mines[y][x]) {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                ms_flood_reveal(x+dx, y+dy);
    }
}

static void ms_check_win(void) {
    int hidden = 0;
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++)
            if (ms_reveal[y][x] != 1) hidden++;
    if (hidden == MS_MINES) { ms_won = true; ms_gameover = true; }
}

static const uint8_t num_colors[] = {
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_RED,
    VGA_BLUE, VGA_RED, VGA_LIGHT_CYAN, VGA_WHITE, VGA_DARK_GREY
};

static void ms_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id; (void)cw; (void)ch;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);

    int row = cy;
    /* Title + mine count */
    char info[24]; strcpy(info, "\x0F Minesweeper ");
    char mn[4]; int_to_str(MS_MINES - ms_flags_placed, mn);
    strcat(info, "["); strcat(info, mn); strcat(info, "]");
    gui_draw_text(cx, row, info, accent);
    row++;

    /* Grid */
    for (int y = 0; y < MS_H && row < cy + ch - 1; y++) {
        for (int x = 0; x < MS_W; x++) {
            bool at_cursor = (x == ms_cx && y == ms_cy && !ms_gameover);
            char ch_c; uint8_t col;
            if (ms_reveal[y][x] == 1) {
                if (ms_mines[y][x]) { ch_c = '*'; col = VGA_COLOR(VGA_LIGHT_RED, bg); }
                else if (ms_adj[y][x] > 0) {
                    ch_c = '0' + ms_adj[y][x];
                    col = VGA_COLOR(num_colors[ms_adj[y][x]], bg);
                } else { ch_c = '\xFA'; col = VGA_COLOR(VGA_DARK_GREY, bg); }
            } else if (ms_reveal[y][x] == 2) {
                ch_c = '\x10'; col = VGA_COLOR(VGA_YELLOW, bg);
            } else {
                ch_c = (char)0xFE; col = VGA_COLOR(VGA_LIGHT_GREY, bg);
            }
            if (at_cursor) col = VGA_COLOR(col & 0x0F, VGA_WHITE);
            gui_putchar(cx + x * 2, row, ch_c, col);
            gui_putchar(cx + x * 2 + 1, row, ' ', at_cursor ? VGA_COLOR(VGA_BLACK, VGA_WHITE) : tc);
        }
        row++;
    }

    row++;
    if (ms_gameover) {
        gui_draw_text(cx, row, ms_won ? "YOU WIN! R:Restart" : "BOOM! R:Restart",
            ms_won ? VGA_COLOR(VGA_LIGHT_GREEN, bg) : VGA_COLOR(VGA_LIGHT_RED, bg));
    } else {
        gui_draw_text(cx, row, "Spc:Reveal F:Flag R:New", dim);
    }
}

static void ms_key(int id, char key) {
    (void)id;
    if (key == 'r' || key == 'R') { ms_init_board(); return; }
    if (ms_gameover) return;

    if ((unsigned char)key == 0x80 && ms_cy > 0) ms_cy--;
    if ((unsigned char)key == 0x81 && ms_cy < MS_H - 1) ms_cy++;
    if ((unsigned char)key == 0x82 && ms_cx > 0) ms_cx--;
    if ((unsigned char)key == 0x83 && ms_cx < MS_W - 1) ms_cx++;

    if (key == ' ' && ms_reveal[ms_cy][ms_cx] == 0) {
        if (ms_mines[ms_cy][ms_cx]) {
            /* Hit a mine — reveal all mines */
            ms_gameover = true;
            for (int y = 0; y < MS_H; y++)
                for (int x = 0; x < MS_W; x++)
                    if (ms_mines[y][x]) ms_reveal[y][x] = 1;
        } else {
            ms_flood_reveal(ms_cx, ms_cy);
            ms_check_win();
        }
    }
    if ((key == 'f' || key == 'F') && ms_reveal[ms_cy][ms_cx] != 1) {
        if (ms_reveal[ms_cy][ms_cx] == 2) { ms_reveal[ms_cy][ms_cx] = 0; ms_flags_placed--; }
        else { ms_reveal[ms_cy][ms_cx] = 2; ms_flags_placed++; }
    }
}

int minesweeper_open(void) {
    ms_init_board();
    return window_create("Minesweeper", 18, 3, MS_W * 2 + 3, MS_H + 5, ms_draw, ms_key);
}
