#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios saved;
static bool raw;
static void restore(void) { if (raw) tcsetattr(0, TCSANOW, &saved); printf("\033[0m\033[?25h\n"); }
static int key(void) {
    unsigned char c;
    if (read(0, &c, 1) != 1) return 0;
    if (c == 27) {
        unsigned char a, b;
        if (read(0, &a, 1) == 1 && a == '[' && read(0, &b, 1) == 1) {
            if (b == 'A') return 'w';
            if (b == 'B') return 's';
            if (b == 'C') return 'd';
            if (b == 'D') return 'a';
        }
        return 0;
    }
    return c;
}
static unsigned tiles[4][4], score;
static bool slide(int dir) {
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        unsigned v[4] = {0}, merged[4] = {0}; int n = 0, m = 0;
        for (int j = 0; j < 4; j++) {
            int x = dir < 2 ? (dir == 0 ? j : 3 - j) : i;
            int y = dir < 2 ? i : (dir == 2 ? j : 3 - j);
            if (tiles[y][x]) v[n++] = tiles[y][x];
        }
        for (int j = 0; j < n; j++) {
            unsigned t = v[j];
            if (j + 1 < n && t == v[j + 1]) { t *= 2; score += t; j++; }
            merged[m++] = t;
        }
        for (int j = 0; j < 4; j++) {
            int x = dir < 2 ? (dir == 0 ? j : 3 - j) : i;
            int y = dir < 2 ? i : (dir == 2 ? j : 3 - j);
            if (tiles[y][x] != merged[j]) changed = true;
            tiles[y][x] = merged[j];
        }
    }
    return changed;
}
static void add_tile(void) {
    int empty[16], n = 0;
    for (int i = 0; i < 16; i++) if (!tiles[i / 4][i % 4]) empty[n++] = i;
    if (n) { int i = empty[rand() % n]; tiles[i / 4][i % 4] = rand() % 10 ? 2 : 4; }
}
static bool can_move(void) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        if (!tiles[y][x] || (x < 3 && tiles[y][x] == tiles[y][x + 1]) || (y < 3 && tiles[y][x] == tiles[y + 1][x])) return true;
    }
    return false;
}
static void puzzle(void) {
    add_tile(); add_tile();
    for (;;) {
        printf("\033[H\033[2J2048      Score: %u\n\n", score);
        for (int y = 0; y < 4; y++) {
            puts("+------+------+------+------+");
            for (int x = 0; x < 4; x++) {
                if (tiles[y][x]) printf("|\033[1;33m%6u\033[0m", tiles[y][x]); else printf("|      ");
            }
            puts("|");
        }
        puts("+------+------+------+------+");
        puts("Arrows / WASD: move   N: new game   Q: quit");
        if (!can_move()) puts("Game over. Press N for another game.");
        fflush(stdout);
        int c; do { c = key(); } while (!c);
        if (c == 'q') return;
        if (c == 'n') { memset(tiles, 0, sizeof(tiles)); score = 0; add_tile(); add_tile(); continue; }
        int dir = c == 'a' ? 0 : c == 'd' ? 1 : c == 'w' ? 2 : c == 's' ? 3 : -1;
        if (dir >= 0 && slide(dir)) add_tile();
    }
}

static uint8_t field[20][10];
static const uint16_t shapes[] = {0x00f0, 0x0660, 0x0072, 0x0036, 0x0063, 0x0071, 0x0074};
static uint16_t rotate(uint16_t shape) {
    uint16_t r = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (shape & (1u << (y * 4 + x))) r |= 1u << (x * 4 + 3 - y);
    return r;
}
static bool fits(uint16_t shape, int x, int y) {
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) if (shape & (1u << (j * 4 + i))) {
        int px = x + i, py = y + j;
        if (px < 0 || px >= 10 || py < 0 || py >= 20 || field[py][px]) return false;
    }
    return true;
}
static int clear_rows(void) {
    int cleared = 0;
    for (int y = 19; y >= 0; y--) {
        int n = 0; for (int x = 0; x < 10; x++) n += field[y][x] != 0;
        if (n == 10) { memmove(field[1], field[0], y * sizeof(field[0])); memset(field[0], 0, sizeof(field[0])); cleared++; y++; }
    }
    return cleared;
}
static void falling_blocks(void) {
    uint16_t shape = shapes[rand() % 7]; int x = 3, y = 0, lines = 0;
    clock_t last = clock(); bool dirty = true;
    while (fits(shape, x, y)) {
        if (dirty) {
            printf("\033[H\033[2JTetris    Lines: %d\n", lines);
            for (int row = 0; row < 20; row++) {
                putchar('|');
                for (int col = 0; col < 10; col++) {
                    bool current = col >= x && col < x + 4 && row >= y && row < y + 4 && (shape & (1u << ((row - y) * 4 + col - x)));
                    printf(current ? "\033[46m  \033[0m" : field[row][col] ? "\033[44m  \033[0m" : "  ");
                }
                puts("|");
            }
            puts("+--------------------+");
            puts("Arrows/WASD: move/rotate  Space: drop  Q: quit"); fflush(stdout); dirty = false;
        }
        int c = key(); bool down = c == 's';
        if (c == 'q') return;
        if ((c == 'a' || c == 'd') && fits(shape, x + (c == 'a' ? -1 : 1), y)) { x += c == 'a' ? -1 : 1; dirty = true; }
        if (c == 'w') {
            uint16_t next = rotate(shape);
            if (fits(next, x, y)) { shape = next; dirty = true; }
            else if (fits(next, x - 1, y)) { shape = next; x--; dirty = true; }
            else if (fits(next, x + 1, y)) { shape = next; x++; dirty = true; }
        }
        if (c == ' ') { while (fits(shape, x, y + 1)) y++; down = true; }
        if (clock() - last >= CLOCKS_PER_SEC / 2) down = true;
        if (down) {
            last = clock(); dirty = true;
            if (fits(shape, x, y + 1)) y++;
            else {
                for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) if (shape & (1u << (j * 4 + i))) field[y + j][x + i] = 1;
                lines += clear_rows(); shape = shapes[rand() % 7]; x = 3; y = 0;
            }
        }
    }
    puts("Game over. Press Q to exit."); while (key() != 'q') {}
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--test")) {
        tiles[0][0] = tiles[0][1] = tiles[0][2] = tiles[0][3] = 2;
        assert(slide(0) && tiles[0][0] == 4 && tiles[0][1] == 4 && score == 8);
        assert(slide(0) && tiles[0][0] == 8 && score == 16);
        assert(!slide(0) && slide(1) && tiles[0][3] == 8 && can_move());
        uint16_t s = shapes[2]; for (int i = 0; i < 4; i++) s = rotate(s); assert(s == shapes[2]);
        assert(fits(shapes[0], 3, 0) && !fits(shapes[0], 9, 0));
        memset(field[19], 1, 10); assert(clear_rows() == 1 && field[19][0] == 0);
        FILE *f = fopen("/disk/game.ok", "w"); assert(f); assert(fputs("GAMES ALL PASSED\n", f) >= 0 && fclose(f) == 0);
        puts("GAMES ALL PASSED"); return 0;
    }
    if (tcgetattr(0, &saved)) { perror("terminal"); return 1; }
    struct termios t = saved;
    t.c_lflag &= ~(ICANON | ECHO); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 1;
    if (tcsetattr(0, TCSANOW, &t)) return 1;
    raw = true; atexit(restore); printf("\033[?25l");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    if (strstr(argv[0], "2048")) puzzle(); else falling_blocks();
    return 0;
}
