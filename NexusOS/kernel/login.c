/* ============================================================================
 * NexusOS — Login Screen (Implementation)
 * ============================================================================
 * Full-screen animated boot splash → username/password → desktop.
 * Uses VGA text mode directly (not the GUI back buffer).
 * ============================================================================ */

#include "login.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"
#include "port.h"
#include "speaker.h"
#include "users.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * delay_ticks: Busy-wait for N timer ticks (~55ms each)
 * -------------------------------------------------------------------------- */
static void delay_ticks(uint32_t n) {
    uint32_t start = system_ticks;
    while (system_ticks - start < n) {
        vga_flush();
        __asm__ volatile("hlt");
    }
}

/* --------------------------------------------------------------------------
 * draw_centered: Draw text centered on a row
 * -------------------------------------------------------------------------- */
static void draw_centered(int row, const char* text, uint8_t color) {
    int len = strlen(text);
    int col = (80 - len) / 2;
    if (col < 0) col = 0;
    vga_set_cursor(row, col);
    vga_print_color(text, color);
}

/* --------------------------------------------------------------------------
 * draw_progress_bar: Animated loading bar
 * -------------------------------------------------------------------------- */
static void draw_progress_bar(int row, int width, int filled) {
    int start_col = (80 - width - 2) / 2;
    vga_set_cursor(row, start_col);
    vga_print_color("[", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            vga_print_color("\xDB", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        } else {
            vga_print_color("\xB0", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        }
    }
    vga_print_color("]", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * read_input: Read a line of input (optionally masked with *)
 * -------------------------------------------------------------------------- */
static int read_input(char* buf, int max, bool masked) {
    int len = 0;
    while (len < max - 1) {
        vga_flush();
        char c = keyboard_getchar();
        if (c == '\n') {
            buf[len] = '\0';
            vga_putchar('\n');
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                vga_backspace();
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            buf[len++] = c;
            vga_putchar(masked ? '*' : c);
        }
    }
    buf[len] = '\0';
    return len;
}

/* --------------------------------------------------------------------------
 * login_run: Main login screen
 * -------------------------------------------------------------------------- */
uint32_t login_run(void) {
    /* Hide cursor during splash */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 0x20);

    vga_clear();

    /* === BOOT SPLASH === */
    /* Draw NexusOS logo */
    draw_centered(5, "\xDB\xDB\xDB\xDB\xDB\xDB    NexusOS    \xDB\xDB\xDB\xDB\xDB\xDB", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    draw_centered(7, "The Hybrid Operating System", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    draw_centered(8, "v0.6.0", VGA_COLOR(VGA_WHITE, VGA_BLACK));

    /* Animated loading bar */
    int bar_width = 40;
    draw_centered(11, "Loading system...", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    for (int i = 0; i <= bar_width; i++) {
        draw_progress_bar(13, bar_width, i);
        delay_ticks(1);  /* ~55ms per step */
    }

    draw_centered(15, "\xFB System ready", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    delay_ticks(10);

    /* === LOGIN PROMPT === */
    vga_clear();

    /* Top bar */
    for (int i = 0; i < 80; i++) {
        vga_set_cursor(0, i);
        vga_print_color(" ", VGA_COLOR(VGA_WHITE, VGA_BLUE));
    }
    vga_set_cursor(0, 2);
    vga_print_color(" NexusOS Login ", VGA_COLOR(VGA_WHITE, VGA_BLUE));

    /* Login box */
    draw_centered(6, "\x0F Welcome to NexusOS \x0F", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));

    /* Decorative line */
    vga_set_cursor(7, 20);
    for (int i = 0; i < 40; i++)
        vga_print_color("\xC4", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    draw_centered(9, "Please log in to continue", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    draw_centered(19, "(try root/root or guest/guest)", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Re-enable cursor */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 14);
    port_byte_out(0x3D4, 0x0B);
    port_byte_out(0x3D5, 15);

    /* Phase 44: real authentication — loop until credentials verify. */
    char username[32];
    char password[32];
    uint32_t uid = UID_INVALID;
    while (uid == UID_INVALID) {
        /* Clear any prior input/error rows */
        for (int r = 11; r <= 13; r++) {
            vga_set_cursor(r, 0);
            for (int c = 0; c < 80; c++) vga_print(" ");
        }

        vga_set_cursor(11, 25);
        vga_print_color("Username: ", VGA_COLOR(VGA_WHITE, VGA_BLACK));
        read_input(username, 32, false);

        vga_set_cursor(12, 25);
        vga_print_color("Password: ", VGA_COLOR(VGA_WHITE, VGA_BLACK));
        read_input(password, 32, true);

        uid = users_authenticate(username, password);
        if (uid == UID_INVALID) {
            play_error_sound();
            vga_set_cursor(13, 25);
            vga_print_color("Invalid username or password.",
                            VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            /* brief pause so the message is visible */
            delay_ticks(18);
        }
    }

    /* Authenticated — make this the current user. */
    users_set_current(uid);

    /* Welcome message */
    /* Hide cursor for animation */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 0x20);

    vga_set_cursor(14, 0);
    draw_centered(14, "\xFB Login successful!", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));

    char welcome[60];
    strcpy(welcome, "Welcome, ");
    if (username[0] != '\0') {
        strcat(welcome, username);
    } else {
        strcat(welcome, "admin");
    }
    strcat(welcome, "!");
    draw_centered(15, welcome, VGA_COLOR(VGA_WHITE, VGA_BLACK));

    draw_centered(17, "Starting desktop environment...", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Brief animation */
    for (int i = 0; i < 3; i++) {
        delay_ticks(6);
        vga_set_cursor(19, 38 + i);
        vga_print_color(".", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    }
    delay_ticks(10);

    /* Re-enable cursor */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 14);
    port_byte_out(0x3D4, 0x0B);
    port_byte_out(0x3D5, 15);

    vga_clear();
    vga_flush();

    return uid;
}
