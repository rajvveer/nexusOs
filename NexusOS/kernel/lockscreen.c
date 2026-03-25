/* ============================================================================
 * NexusOS — Lock Screen (Implementation)
 * ============================================================================
 * Displays clock and locked message. Requires password to unlock.
 * Accepts any password for now.
 * ============================================================================ */

#include "lockscreen.h"
#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "rtc.h"
#include "string.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * lockscreen_run: Show lock screen, block until password entered
 * -------------------------------------------------------------------------- */
void lockscreen_run(void) {
    char password[32];
    int pw_len = 0;
    bool unlocked = false;

    while (!unlocked) {
        /* Draw lock screen */
        gui_clear(' ', VGA_COLOR(VGA_BLACK, VGA_BLACK));

        /* Top accent line */
        for (int x = 0; x < GUI_WIDTH; x++)
            gui_putchar(x, 0, (char)0xDF, VGA_COLOR(VGA_BLUE, VGA_BLACK));

        /* Clock */
        rtc_time_t t;
        rtc_read(&t);
        char time_str[16];
        rtc_format_time(&t, time_str);
        int tx = (GUI_WIDTH - strlen(time_str)) / 2;
        gui_draw_text(tx, 5, time_str, VGA_COLOR(VGA_WHITE, VGA_BLACK));

        /* Lock icon + message */
        gui_draw_text(33, 8, "\x08 Locked \x08", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        gui_draw_text(28, 10, "NexusOS is locked", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

        /* Password box */
        int box_x = (GUI_WIDTH - 30) / 2;
        int box_y = 12;
        gui_rect_t box = { box_x, box_y, 30, 3 };
        gui_fill_rect(box, ' ', VGA_COLOR(VGA_WHITE, VGA_DARK_GREY));
        gui_draw_box(box, VGA_COLOR(VGA_LIGHT_CYAN, VGA_DARK_GREY));

        gui_draw_text(box_x + 2, box_y + 1, "Password: ", VGA_COLOR(VGA_WHITE, VGA_DARK_GREY));

        /* Show asterisks */
        for (int i = 0; i < pw_len && i < 16; i++)
            gui_putchar(box_x + 12 + i, box_y + 1, '*', VGA_COLOR(VGA_YELLOW, VGA_DARK_GREY));

        /* Cursor */
        if (pw_len < 16) {
            uint32_t blink = (system_ticks / 9) % 2;
            gui_putchar(box_x + 12 + pw_len, box_y + 1,
                       blink ? '_' : ' ', VGA_COLOR(VGA_WHITE, VGA_DARK_GREY));
        }

        gui_draw_text(28, 16, "Enter password to unlock", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

        /* Bottom accent line */
        for (int x = 0; x < GUI_WIDTH; x++)
            gui_putchar(x, GUI_HEIGHT - 1, (char)0xDC, VGA_COLOR(VGA_BLUE, VGA_BLACK));

        gui_flip();

        /* Wait for input */
        __asm__ volatile("hlt");

        while (keyboard_has_key()) {
            char key = keyboard_getchar();

            if (key == '\n') {
                /* Accept any password */
                if (pw_len > 0) {
                    unlocked = true;
                }
                break;
            } else if (key == '\b') {
                if (pw_len > 0) pw_len--;
            } else if (key >= 32 && key < 127 && pw_len < 31) {
                password[pw_len++] = key;
                password[pw_len] = '\0';
            }
        }
    }
}
