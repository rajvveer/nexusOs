/* ============================================================================
 * NexusOS — Notification Center (Implementation)
 * ============================================================================
 * Slide-in panel with notification history, timestamps, icons.
 * ============================================================================ */

#include "notifcenter.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "rtc.h"

#define NC_MAX 16
#define NC_MSG_MAX 30
#define NC_WIDTH 28
#define NC_X (80 - NC_WIDTH)

typedef struct {
    char msg[NC_MSG_MAX];
    uint8_t icon;
    uint8_t hour, minute;
    bool used;
} nc_entry_t;

static nc_entry_t nc_entries[NC_MAX];
static int nc_count = 0;
static bool nc_open = false;
static int nc_scroll = 0;

void notifcenter_toggle(void) { nc_open = !nc_open; nc_scroll = 0; }
bool notifcenter_is_open(void) { return nc_open; }

void notifcenter_add(const char* msg, uint8_t icon) {
    rtc_time_t t; rtc_read(&t);
    if (nc_count < NC_MAX) {
        nc_entries[nc_count].icon = icon;
        nc_entries[nc_count].hour = t.hour;
        nc_entries[nc_count].minute = t.minute;
        strncpy(nc_entries[nc_count].msg, msg, NC_MSG_MAX - 1);
        nc_entries[nc_count].msg[NC_MSG_MAX - 1] = '\0';
        nc_entries[nc_count].used = true;
        nc_count++;
    } else {
        for (int i = 0; i < NC_MAX - 1; i++) nc_entries[i] = nc_entries[i + 1];
        nc_entries[NC_MAX - 1].icon = icon;
        nc_entries[NC_MAX - 1].hour = t.hour;
        nc_entries[NC_MAX - 1].minute = t.minute;
        strncpy(nc_entries[NC_MAX - 1].msg, msg, NC_MSG_MAX - 1);
        nc_entries[NC_MAX - 1].msg[NC_MSG_MAX - 1] = '\0';
        nc_entries[NC_MAX - 1].used = true;
    }
}

void notifcenter_clear(void) { nc_count = 0; }

void notifcenter_draw(void) {
    if (!nc_open) return;
    uint8_t bg = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY);
    uint8_t title = VGA_COLOR(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    uint8_t dim = VGA_COLOR(VGA_LIGHT_GREY, VGA_DARK_GREY);
    uint8_t time_col = VGA_COLOR(VGA_YELLOW, VGA_DARK_GREY);

    /* Panel background */
    for (int y = 0; y < 23; y++)
        for (int x = NC_X; x < 80; x++)
            gui_putchar(x, y, ' ', bg);

    /* Border */
    for (int y = 0; y < 23; y++) gui_putchar(NC_X, y, (char)0xB3, VGA_COLOR(VGA_LIGHT_CYAN, VGA_DARK_GREY));

    /* Title */
    gui_draw_text(NC_X + 2, 0, "\x0F Notifications", title);
    char cnt[8]; int_to_str(nc_count, cnt);
    gui_draw_text(NC_X + NC_WIDTH - 4, 0, cnt, dim);

    /* Separator */
    for (int x = NC_X + 1; x < 80; x++) gui_putchar(x, 1, (char)0xC4, dim);

    /* Entries (newest first) */
    int row = 2;
    for (int i = nc_count - 1 - nc_scroll; i >= 0 && row < 21; i--) {
        if (!nc_entries[i].used) continue;
        /* Time */
        char ts[6];
        ts[0] = '0' + nc_entries[i].hour / 10; ts[1] = '0' + nc_entries[i].hour % 10;
        ts[2] = ':';
        ts[3] = '0' + nc_entries[i].minute / 10; ts[4] = '0' + nc_entries[i].minute % 10;
        ts[5] = '\0';
        gui_draw_text(NC_X + 1, row, ts, time_col);
        gui_putchar(NC_X + 7, row, nc_entries[i].icon ? nc_entries[i].icon : '\x07', VGA_COLOR(VGA_LIGHT_GREEN, VGA_DARK_GREY));
        /* Message */
        int max = NC_WIDTH - 10;
        for (int j = 0; j < max && nc_entries[i].msg[j]; j++)
            gui_putchar(NC_X + 9 + j, row, nc_entries[i].msg[j], bg);
        row++;
    }

    if (nc_count == 0) gui_draw_text(NC_X + 4, 10, "No notifications", dim);

    /* Footer */
    gui_draw_text(NC_X + 2, 22, "Esc:Close C:Clear", dim);
}

int notifcenter_handle_key(char key) {
    if (key == 27) { nc_open = false; return 1; }
    if (key == 'c' || key == 'C') { notifcenter_clear(); return 1; }
    if ((unsigned char)key == 0x80 && nc_scroll > 0) nc_scroll--;
    if ((unsigned char)key == 0x81 && nc_scroll < nc_count - 1) nc_scroll++;
    return 0;
}
