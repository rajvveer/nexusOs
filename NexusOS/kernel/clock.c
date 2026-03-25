/* ============================================================================
 * NexusOS — Clock App (Implementation)
 * ============================================================================
 * Clock, stopwatch, and timer in one window.
 * ============================================================================ */

#include "clock.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "rtc.h"
#include "speaker.h"

extern volatile uint32_t system_ticks;

static int clk_mode = 0; /* 0=clock, 1=stopwatch, 2=timer */
static uint32_t sw_start = 0, sw_elapsed = 0;
static bool sw_running = false;
static int tmr_seconds = 60;
static uint32_t tmr_start = 0;
static bool tmr_running = false;

/* Big digit font: each digit is 3 chars wide x 3 tall */
static const char* big_digits[10][3] = {
    {"\xDA\xC4\xBF","\xB3 \xB3","\xC0\xC4\xD9"},  /* 0 */
    {"  \xB3","  \xB3","  \xB3"},                     /* 1 */
    {"\xDA\xC4\xBF","\xDA\xC4\xD9","\xC0\xC4\xD9"}, /* 2 */
    {"\xDA\xC4\xBF"," \xC4\xB4","\xC0\xC4\xD9"},    /* 3 */
    {"\xB3 \xB3","\xC0\xC4\xB4","  \xB3"},           /* 4 */
    {"\xDA\xC4\xBF","\xC0\xC4\xBF","\xC0\xC4\xD9"}, /* 5 */
    {"\xDA\xC4\xBF","\xC3\xC4\xBF","\xC0\xC4\xD9"}, /* 6 */
    {"\xDA\xC4\xBF","  \xB3","  \xB3"},               /* 7 */
    {"\xDA\xC4\xBF","\xC3\xC4\xB4","\xC0\xC4\xD9"}, /* 8 */
    {"\xDA\xC4\xBF","\xC0\xC4\xB4","\xC0\xC4\xD9"}, /* 9 */
};

static void draw_big(int x, int y, int digit, uint8_t col) {
    if (digit < 0 || digit > 9) return;
    for (int r = 0; r < 3; r++)
        gui_draw_text(x, y + r, big_digits[digit][r], col);
}

static void clk_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t val = VGA_COLOR(VGA_YELLOW, bg);

    int row = cy;
    const char* modes[] = {"Clock", "Stopwatch", "Timer"};
    gui_draw_text(cx, row, "\x0F ", accent);
    gui_draw_text(cx + 2, row, modes[clk_mode], accent);
    row += 2;

    if (clk_mode == 0) {
        /* Clock: big digits */
        rtc_time_t time; rtc_read(&time);
        int h = time.hour, m = time.minute, s = time.second;
        int dx = cx + 2;
        draw_big(dx, row, h / 10, val); dx += 4;
        draw_big(dx, row, h % 10, val); dx += 4;
        gui_putchar(dx, row + 1, ':', accent); dx += 2;
        draw_big(dx, row, m / 10, val); dx += 4;
        draw_big(dx, row, m % 10, val); dx += 4;
        gui_putchar(dx, row + 1, ':', accent); dx += 2;
        draw_big(dx, row, s / 10, val); dx += 4;
        draw_big(dx, row, s % 10, val);
        row += 4;
        /* Date */
        char ds[16]; char n1[4], n2[4], n3[6];
        int_to_str(time.day, n1); int_to_str(time.month, n2); int_to_str(2000 + time.year, n3);
        strcpy(ds, n1); strcat(ds, "/"); strcat(ds, n2); strcat(ds, "/"); strcat(ds, n3);
        gui_draw_text(cx + 8, row, ds, dim);
    }
    else if (clk_mode == 1) {
        /* Stopwatch */
        uint32_t el = sw_elapsed;
        if (sw_running) el += (system_ticks - sw_start) / 18;
        int m = el / 60, s = el % 60;
        int dx = cx + 6;
        draw_big(dx, row, m / 10, val); dx += 4;
        draw_big(dx, row, m % 10, val); dx += 4;
        gui_putchar(dx, row + 1, ':', accent); dx += 2;
        draw_big(dx, row, s / 10, val); dx += 4;
        draw_big(dx, row, s % 10, val);
        row += 4;
        gui_draw_text(cx + 4, row, sw_running ? "RUNNING" : "STOPPED", sw_running ? VGA_COLOR(VGA_LIGHT_GREEN, bg) : dim);
        row += 2;
        gui_draw_text(cx, row, "Space:Start/Stop R:Reset", dim);
    }
    else {
        /* Timer */
        int remaining = tmr_seconds;
        if (tmr_running) {
            int elapsed = (system_ticks - tmr_start) / 18;
            remaining = tmr_seconds - elapsed;
            if (remaining <= 0) { remaining = 0; tmr_running = false; beep(880, 18); }
        }
        int m = remaining / 60, s = remaining % 60;
        int dx = cx + 6;
        draw_big(dx, row, m / 10, remaining <= 10 ? VGA_COLOR(VGA_LIGHT_RED, bg) : val); dx += 4;
        draw_big(dx, row, m % 10, remaining <= 10 ? VGA_COLOR(VGA_LIGHT_RED, bg) : val); dx += 4;
        gui_putchar(dx, row + 1, ':', accent); dx += 2;
        draw_big(dx, row, s / 10, remaining <= 10 ? VGA_COLOR(VGA_LIGHT_RED, bg) : val); dx += 4;
        draw_big(dx, row, s % 10, remaining <= 10 ? VGA_COLOR(VGA_LIGHT_RED, bg) : val);
        row += 4;
        gui_draw_text(cx + 4, row, tmr_running ? "COUNTING" : (remaining == 0 ? "DONE!" : "READY"), tmr_running ? VGA_COLOR(VGA_YELLOW, bg) : dim);
        row += 2;
        gui_draw_text(cx, row, "Space:Go +/-:Adjust R:Reset", dim);
    }

    gui_draw_text(cx, cy + ch - 1, "Tab:Mode", dim);
    (void)cw; (void)ch;
}

static void clk_key(int id, char key) {
    (void)id;
    if (key == '\t') { clk_mode = (clk_mode + 1) % 3; return; }

    if (clk_mode == 1) {
        if (key == ' ') {
            if (sw_running) { sw_elapsed += (system_ticks - sw_start) / 18; sw_running = false; }
            else { sw_start = system_ticks; sw_running = true; }
        }
        if (key == 'r' || key == 'R') { sw_running = false; sw_elapsed = 0; }
    }
    else if (clk_mode == 2) {
        if (key == ' ') {
            if (!tmr_running && tmr_seconds > 0) { tmr_start = system_ticks; tmr_running = true; }
            else tmr_running = false;
        }
        if (key == '+' || key == '=') { if (!tmr_running) tmr_seconds += 10; if (tmr_seconds > 3600) tmr_seconds = 3600; }
        if (key == '-') { if (!tmr_running && tmr_seconds > 10) tmr_seconds -= 10; }
        if (key == 'r' || key == 'R') { tmr_running = false; tmr_seconds = 60; }
    }
}

int clock_open(void) {
    clk_mode = 0; sw_running = false; sw_elapsed = 0;
    tmr_running = false; tmr_seconds = 60;
    return window_create("Clock", 12, 3, 32, 12, clk_draw, clk_key);
}
