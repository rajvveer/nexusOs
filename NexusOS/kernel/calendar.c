/* ============================================================================
 * NexusOS — Calendar Widget (Implementation)
 * ============================================================================
 * Monthly calendar grid + large clock display.
 * ============================================================================ */

#include "calendar.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "rtc.h"

extern volatile uint32_t system_ticks;

static int cal_month = 0;  /* 1-12, 0 = auto from RTC */
static int cal_year = 0;

static const char* month_names[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const int month_days[] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Zeller's formula - day of week (0=Sun, 1=Mon, ..., 6=Sat) */
static int day_of_week(int y, int m, int d) {
    if (m < 3) { m += 12; y--; }
    int k = y % 100;
    int j = y / 100;
    int h = (d + (13*(m+1))/5 + k + k/4 + j/4 + 5*j) % 7;
    /* Convert Zeller (0=Sat) to 0=Sun */
    return ((h + 6) % 7);
}

static int get_days(int m, int y) {
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return month_days[m];
}

static void cal_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t today_col = VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN);
    uint8_t val_col = VGA_COLOR(VGA_WHITE, bg);

    /* Get time */
    rtc_time_t now;
    rtc_read(&now);
    int cur_m = cal_month > 0 ? cal_month : now.month;
    int cur_y = cal_year > 0 ? cal_year : 2000 + now.year;
    int cur_d = now.day;

    int row = cy;

    /* Large clock */
    char time_buf[12];
    rtc_format_time(&now, time_buf);
    int tx = cx + (cw - strlen(time_buf)) / 2;
    gui_draw_text(tx, row, time_buf, accent);
    row++;

    /* Month + year */
    char header[30];
    strcpy(header, month_names[cur_m]);
    strcat(header, " ");
    char ystr[6]; int_to_str(cur_y, ystr);
    strcat(header, ystr);
    int hx = cx + (cw - strlen(header)) / 2;
    gui_draw_text(cx + 1, row, "<", dim);
    gui_draw_text(hx, row, header, val_col);
    gui_draw_text(cx + cw - 2, row, ">", dim);
    row++;

    /* Separator */
    for (int i = 0; i < cw - 1; i++) gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    /* Day headers */
    const char* day_hdrs = "Su Mo Tu We Th Fr Sa";
    gui_draw_text(cx + 1, row, day_hdrs, accent);
    row++;

    /* Calendar grid */
    int total_days = get_days(cur_m, cur_y);
    int start_dow = day_of_week(cur_y, cur_m, 1);

    int col = start_dow;
    int grid_row = row;

    /* Empty cells before first day */
    for (int i = 0; i < start_dow; i++) {
        /* nothing */
    }

    for (int d = 1; d <= total_days; d++) {
        int px = cx + 1 + col * 3;
        bool is_today = (d == cur_d && (cal_month == 0 || cal_month == now.month) && (cal_year == 0 || cal_year == 2000 + now.year));

        char ds[4];
        if (d < 10) { ds[0] = ' '; ds[1] = '0' + d; ds[2] = '\0'; }
        else { ds[0] = '0' + d / 10; ds[1] = '0' + d % 10; ds[2] = '\0'; }

        uint8_t dc = is_today ? today_col : tc;
        if (is_today) {
            gui_putchar(px, grid_row, ds[0], dc);
            gui_putchar(px + 1, grid_row, ds[1], dc);
        } else {
            gui_draw_text(px, grid_row, ds, dc);
        }

        col++;
        if (col >= 7) { col = 0; grid_row++; }
    }

    /* Bottom hint */
    int hint_row = cy + ch - 1;
    gui_draw_text(cx + 1, hint_row, "</>: Change month", dim);
    (void)ch;
}

static void cal_key(int id, char key) {
    (void)id;
    rtc_time_t now; rtc_read(&now);

    if (cal_month == 0) { cal_month = now.month; cal_year = 2000 + now.year; }

    if (key == '<' || key == ',' || (unsigned char)key == 0x82) {
        cal_month--;
        if (cal_month < 1) { cal_month = 12; cal_year--; }
    }
    if (key == '>' || key == '.' || (unsigned char)key == 0x83) {
        cal_month++;
        if (cal_month > 12) { cal_month = 1; cal_year++; }
    }
    /* 't' goes back to today */
    if (key == 't' || key == 'T') { cal_month = 0; cal_year = 0; }
}

int calendar_open(void) {
    cal_month = 0; cal_year = 0;
    return window_create("Calendar", 26, 3, 24, 14, cal_draw, cal_key);
}
