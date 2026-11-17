/* ============================================================================
 * NexusOS — Snake Game (Implementation)
 * ============================================================================
 * Classic snake game in VGA text mode.
 * Arrow keys to move. Eat food (*) to grow. Hit walls or self = game over.
 * Uses timer ticks for game speed.
 * ============================================================================ */

#include "snake.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"

#define GAME_WIDTH   40
#define GAME_HEIGHT  20
#define GAME_OFF_X   20   /* Center horizontally: (80-40)/2 */
#define GAME_OFF_Y   1    /* Start at row 1 */
#define MAX_SNAKE    200

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Direction enum */
enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

/* Point */
typedef struct {
    int x, y;
} point_t;

/* Game state */
static point_t snake[MAX_SNAKE];
static int snake_len;
static int direction;
static point_t food;
static int score;
static int game_over;
static int game_speed;   /* Ticks between moves */

/* Simple pseudo-random */
static uint32_t lcg_seed;
static uint32_t rand_next(void) {
    lcg_seed = lcg_seed * 1103515245 + 12345;
    return (lcg_seed >> 16) & 0x7FFF;
}

/* --------------------------------------------------------------------------
 * spawn_food: Place food at random empty position
 * -------------------------------------------------------------------------- */
static void spawn_food(void) {
    int attempts = 0;
    while (attempts < 500) {
        int x = (rand_next() % (GAME_WIDTH - 2)) + 1;
        int y = (rand_next() % (GAME_HEIGHT - 2)) + 1;

        /* Check not on snake */
        int on_snake = 0;
        for (int i = 0; i < snake_len; i++) {
            if (snake[i].x == x && snake[i].y == y) {
                on_snake = 1;
                break;
            }
        }
        if (!on_snake) {
            food.x = x;
            food.y = y;
            return;
        }
        attempts++;
    }
    /* Fallback */
    food.x = GAME_WIDTH / 2;
    food.y = GAME_HEIGHT / 2;
}

/* --------------------------------------------------------------------------
 * draw_border: Draw game border
 * -------------------------------------------------------------------------- */
static void draw_border(void) {
    uint8_t border_color = VGA_COLOR(VGA_WHITE, VGA_BLACK);
    uint8_t corner_color = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK);

    /* Top and bottom */
    for (int x = 0; x < GAME_WIDTH; x++) {
        vga_putchar_at('#', GAME_OFF_Y, GAME_OFF_X + x, border_color);
        vga_putchar_at('#', GAME_OFF_Y + GAME_HEIGHT - 1, GAME_OFF_X + x, border_color);
    }
    /* Left and right */
    for (int y = 0; y < GAME_HEIGHT; y++) {
        vga_putchar_at('#', GAME_OFF_Y + y, GAME_OFF_X, border_color);
        vga_putchar_at('#', GAME_OFF_Y + y, GAME_OFF_X + GAME_WIDTH - 1, border_color);
    }
    /* Corners */
    vga_putchar_at('+', GAME_OFF_Y, GAME_OFF_X, corner_color);
    vga_putchar_at('+', GAME_OFF_Y, GAME_OFF_X + GAME_WIDTH - 1, corner_color);
    vga_putchar_at('+', GAME_OFF_Y + GAME_HEIGHT - 1, GAME_OFF_X, corner_color);
    vga_putchar_at('+', GAME_OFF_Y + GAME_HEIGHT - 1, GAME_OFF_X + GAME_WIDTH - 1, corner_color);
}

/* --------------------------------------------------------------------------
 * draw_game: Render game state
 * -------------------------------------------------------------------------- */
static void draw_game(void) {
    uint8_t bg_color = VGA_COLOR(VGA_BLACK, VGA_BLACK);
    uint8_t food_color = VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK);
    uint8_t head_color = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK);
    uint8_t body_color = VGA_COLOR(VGA_GREEN, VGA_BLACK);

    /* Clear game area */
    for (int y = 1; y < GAME_HEIGHT - 1; y++) {
        for (int x = 1; x < GAME_WIDTH - 1; x++) {
            vga_putchar_at(' ', GAME_OFF_Y + y, GAME_OFF_X + x, bg_color);
        }
    }

    /* Draw food */
    vga_putchar_at('*', GAME_OFF_Y + food.y, GAME_OFF_X + food.x, food_color);

    /* Draw snake */
    for (int i = 0; i < snake_len; i++) {
        char ch;
        uint8_t color;
        if (i == 0) {
            ch = '@';
            color = head_color;
        } else {
            ch = 'o';
            color = body_color;
        }
        vga_putchar_at(ch, GAME_OFF_Y + snake[i].y, GAME_OFF_X + snake[i].x, color);
    }

    /* Draw score */
    uint8_t info_color = VGA_COLOR(VGA_YELLOW, VGA_BLACK);
    char score_str[32] = "Score: ";
    char num[8];
    int_to_str(score, num);
    strcat(score_str, num);

    int col = GAME_OFF_X;
    int row = GAME_OFF_Y + GAME_HEIGHT;
    for (int i = 0; score_str[i]; i++) {
        vga_putchar_at(score_str[i], row, col + i, info_color);
    }

    /* Speed indicator */
    char speed_str[32] = "  Speed: ";
    int_to_str(11 - game_speed, num);
    strcat(speed_str, num);
    int off = strlen(score_str);
    for (int i = 0; speed_str[i]; i++) {
        vga_putchar_at(speed_str[i], row, col + off + i, info_color);
    }

    /* Controls hint */
    uint8_t hint_color = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK);
    const char* hint = "Arrow Keys: Move  Q: Quit";
    row = GAME_OFF_Y + GAME_HEIGHT + 1;
    for (int i = 0; hint[i]; i++) {
        vga_putchar_at(hint[i], row, col + i, hint_color);
    }
}

/* --------------------------------------------------------------------------
 * game_tick: Process one game step
 * -------------------------------------------------------------------------- */
static void game_tick(void) {
    /* Calculate new head position */
    point_t new_head = snake[0];
    switch (direction) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
    }

    /* Check wall collision */
    if (new_head.x <= 0 || new_head.x >= GAME_WIDTH - 1 ||
        new_head.y <= 0 || new_head.y >= GAME_HEIGHT - 1) {
        game_over = 1;
        return;
    }

    /* Check self collision */
    for (int i = 0; i < snake_len; i++) {
        if (snake[i].x == new_head.x && snake[i].y == new_head.y) {
            game_over = 1;
            return;
        }
    }

    /* Check food */
    int ate_food = (new_head.x == food.x && new_head.y == food.y);

    /* Move snake: shift body backwards */
    if (!ate_food) {
        /* Remove tail */
        for (int i = snake_len - 1; i > 0; i--) {
            snake[i] = snake[i - 1];
        }
    } else {
        /* Grow: shift everything, don't remove tail */
        if (snake_len < MAX_SNAKE) snake_len++;
        for (int i = snake_len - 1; i > 0; i--) {
            snake[i] = snake[i - 1];
        }
        score += 10;
        /* Speed up every 50 points */
        if (game_speed > 2 && score % 50 == 0) {
            game_speed--;
        }
        spawn_food();
    }

    snake[0] = new_head;
}

/* --------------------------------------------------------------------------
 * snake_run: Main game loop
 * -------------------------------------------------------------------------- */
void snake_run(void) {
    /* Initialize game */
    lcg_seed = system_ticks;
    snake_len = 3;
    direction = DIR_RIGHT;
    score = 0;
    game_over = 0;
    game_speed = 6;  /* Ticks between moves (~330ms) */

    /* Initial snake position */
    snake[0].x = GAME_WIDTH / 2;
    snake[0].y = GAME_HEIGHT / 2;
    snake[1].x = snake[0].x - 1;
    snake[1].y = snake[0].y;
    snake[2].x = snake[0].x - 2;
    snake[2].y = snake[0].y;

    spawn_food();

    /* Clear screen */
    vga_clear_rows(0, 23);
    draw_border();
    draw_game();

    /* Title */
    uint8_t title_color = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK);
    const char* title = "=== NEXUS SNAKE ===";
    int title_col = (VGA_WIDTH - 19) / 2;
    for (int i = 0; title[i]; i++) {
        vga_putchar_at(title[i], 0, title_col + i, title_color);
    }

    uint32_t last_tick = system_ticks;

    while (!game_over) {
        /* Handle input (non-blocking) */
        if (keyboard_has_key()) {
            char c = keyboard_getchar();
            switch ((unsigned char)c) {
                case 0x80:  /* Up */
                    if (direction != DIR_DOWN) direction = DIR_UP;
                    break;
                case 0x81:  /* Down */
                    if (direction != DIR_UP) direction = DIR_DOWN;
                    break;
                case 0x82:  /* Left */
                    if (direction != DIR_RIGHT) direction = DIR_LEFT;
                    break;
                case 0x83:  /* Right */
                    if (direction != DIR_LEFT) direction = DIR_RIGHT;
                    break;
                case 'q':
                case 'Q':
                    game_over = 1;
                    break;
            }
        }

        /* Game tick based on speed */
        if ((system_ticks - last_tick) >= (uint32_t)game_speed) {
            game_tick();
            draw_game();
            last_tick = system_ticks;
        }

        __asm__ volatile("hlt");
    }

    /* Game Over screen */
    uint8_t go_color = VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK);
    const char* go_msg = "GAME OVER!";
    int go_col = GAME_OFF_X + (GAME_WIDTH - 10) / 2;
    int go_row = GAME_OFF_Y + GAME_HEIGHT / 2;
    for (int i = 0; go_msg[i]; i++) {
        vga_putchar_at(go_msg[i], go_row, go_col + i, go_color);
    }

    char final_score[32] = "Final Score: ";
    char num[8];
    int_to_str(score, num);
    strcat(final_score, num);
    int fs_col = GAME_OFF_X + (GAME_WIDTH - (int)strlen(final_score)) / 2;
    for (int i = 0; final_score[i]; i++) {
        vga_putchar_at(final_score[i], go_row + 1, fs_col + i, VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    }

    const char* press = "Press any key...";
    int p_col = GAME_OFF_X + (GAME_WIDTH - 16) / 2;
    for (int i = 0; press[i]; i++) {
        vga_putchar_at(press[i], go_row + 3, p_col + i, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    }

    keyboard_getchar();  /* Wait for keypress */

    /* Restore screen */
    vga_clear();
}
