/* ============================================================================
 * NexusOS — Screensaver (Implementation)
 * ============================================================================
 * Starfield animation + bouncing NexusOS logo.
 * Activates after idle timeout. Any keypress/mouse exits.
 * ============================================================================ */

#include "screensaver.h"
#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Simple pseudo-random */
static uint32_t ss_seed = 12345;
static uint32_t ss_rand(void) {
    ss_seed = ss_seed * 1103515245 + 12345;
    return (ss_seed >> 16) & 0x7FFF;
}

/* Star */
#define MAX_STARS 60

typedef struct {
    int x, y;
    int speed;
    char ch;
} star_t;

static star_t stars[MAX_STARS];

/* Bouncing logo */
static int logo_x, logo_y;
static int logo_dx, logo_dy;
#define LOGO_W 10
#define LOGO_H 1

/* --------------------------------------------------------------------------
 * screensaver_run: Main loop — blocks until input
 * -------------------------------------------------------------------------- */
void screensaver_run(void) {
    ss_seed = system_ticks;

    /* Init stars */
    for (int i = 0; i < MAX_STARS; i++) {
        stars[i].x = ss_rand() % GUI_WIDTH;
        stars[i].y = ss_rand() % GUI_HEIGHT;
        stars[i].speed = (ss_rand() % 3) + 1;
        int brightness = ss_rand() % 3;
        if (brightness == 0) stars[i].ch = '.';
        else if (brightness == 1) stars[i].ch = '*';
        else stars[i].ch = '\x0F';  /* bright star */
    }

    /* Init logo */
    logo_x = GUI_WIDTH / 2 - LOGO_W / 2;
    logo_y = GUI_HEIGHT / 2;
    logo_dx = 1;
    logo_dy = 1;

    /* Flush any pending input */
    while (keyboard_has_key()) keyboard_getchar();
    mouse_clear_events();

    uint32_t last_frame = system_ticks;

    while (1) {
        /* Check for exit */
        if (keyboard_has_key()) {
            keyboard_getchar();  /* consume */
            break;
        }
        mouse_state_t ms = mouse_get_state();
        if (ms.buttons & (MOUSE_LEFT | MOUSE_RIGHT)) break;

        /* Throttle to ~10 fps */
        if (system_ticks - last_frame < 2) {
            __asm__ volatile("hlt");
            continue;
        }
        last_frame = system_ticks;

        /* Clear */
        gui_clear(' ', VGA_COLOR(VGA_BLACK, VGA_BLACK));

        /* Animate stars */
        for (int i = 0; i < MAX_STARS; i++) {
            stars[i].x -= stars[i].speed;
            if (stars[i].x < 0) {
                stars[i].x = GUI_WIDTH - 1;
                stars[i].y = ss_rand() % GUI_HEIGHT;
            }
            uint8_t col;
            if (stars[i].speed == 1) col = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK);
            else if (stars[i].speed == 2) col = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);
            else col = VGA_COLOR(VGA_WHITE, VGA_BLACK);
            gui_putchar(stars[i].x, stars[i].y, stars[i].ch, col);
        }

        /* Bounce logo */
        logo_x += logo_dx;
        logo_y += logo_dy;
        if (logo_x <= 0 || logo_x + LOGO_W >= GUI_WIDTH) logo_dx = -logo_dx;
        if (logo_y <= 0 || logo_y + LOGO_H >= GUI_HEIGHT) logo_dy = -logo_dy;
        if (logo_x < 0) logo_x = 0;
        if (logo_y < 0) logo_y = 0;

        /* Cycle colors */
        uint8_t logo_colors[] = {
            VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK),
            VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK),
            VGA_COLOR(VGA_YELLOW, VGA_BLACK),
            VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK),
            VGA_COLOR(VGA_LIGHT_MAGENTA, VGA_BLACK),
        };
        int color_idx = (system_ticks / 18) % 5;
        gui_draw_text(logo_x, logo_y, " NexusOS ", logo_colors[color_idx]);

        gui_flip();
        mouse_clear_events();
    }
}
