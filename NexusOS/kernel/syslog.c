/* ============================================================================
 * NexusOS — System Log (Implementation)
 * ============================================================================ */

#include "syslog.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "rtc.h"

typedef struct {
    char msg[SLOG_MSG_MAX];
    uint8_t hour, minute, second;
    bool used;
} log_entry_t;

static log_entry_t slog[SLOG_MAX];
static int slog_head = 0;
static int slog_total = 0;
static int sl_scroll = 0;

void syslog_init(void) {
    for (int i = 0; i < SLOG_MAX; i++) slog[i].used = false;
    slog_head = 0; slog_total = 0;
}

void syslog_add(const char* msg) {
    rtc_time_t t; rtc_read(&t);
    slog[slog_head].hour = t.hour;
    slog[slog_head].minute = t.minute;
    slog[slog_head].second = t.second;
    strncpy(slog[slog_head].msg, msg, SLOG_MSG_MAX - 1);
    slog[slog_head].msg[SLOG_MSG_MAX - 1] = '\0';
    slog[slog_head].used = true;
    slog_head = (slog_head + 1) % SLOG_MAX;
    if (slog_total < SLOG_MAX) slog_total++;
}

int syslog_count(void) { return slog_total; }

static void sl_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t time_col = VGA_COLOR(VGA_YELLOW, bg);

    int row = cy;
    char hdr[24]; strcpy(hdr, "\xFE System Log [");
    char n[4]; int_to_str(slog_total, n); strcat(hdr, n); strcat(hdr, "]");
    gui_draw_text(cx, row, hdr, accent); row++;

    /* Show entries newest first */
    int shown = 0;
    for (int i = 0; i < slog_total && row < cy + ch - 1; i++) {
        int idx = (slog_head - 1 - i - sl_scroll + SLOG_MAX * 2) % SLOG_MAX;
        if (idx < 0 || !slog[idx].used) continue;

        /* Time */
        char ts[10];
        ts[0] = '0' + slog[idx].hour / 10; ts[1] = '0' + slog[idx].hour % 10;
        ts[2] = ':';
        ts[3] = '0' + slog[idx].minute / 10; ts[4] = '0' + slog[idx].minute % 10;
        ts[5] = ':';
        ts[6] = '0' + slog[idx].second / 10; ts[7] = '0' + slog[idx].second % 10;
        ts[8] = '\0';
        gui_draw_text(cx, row, ts, time_col);
        gui_putchar(cx + 8, row, ' ', tc);

        /* Message (truncate to fit) */
        int max_msg = cw - 10;
        for (int j = 0; j < max_msg && slog[idx].msg[j]; j++)
            gui_putchar(cx + 9 + j, row, slog[idx].msg[j], tc);
        row++; shown++;
    }

    if (shown == 0) gui_draw_text(cx + 2, row, "No log entries", dim);
    (void)cw; (void)ch;
}

static void sl_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80 && sl_scroll > 0) sl_scroll--;
    if ((unsigned char)key == 0x81 && sl_scroll < slog_total - 1) sl_scroll++;
}

int syslog_open(void) {
    sl_scroll = 0;
    return window_create("System Log", 14, 2, 42, 16, sl_draw, sl_key);
}
