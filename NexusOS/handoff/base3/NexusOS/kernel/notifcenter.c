/* ============================================================================
 * NexusOS — Notification Center (Implementation)
 * ============================================================================
 * Slide-in panel with notification history, timestamps, icons.
 * ============================================================================ */

#include "notifcenter.h"
#include "framebuffer.h"
#include "macui.h"
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
    int sw = (int)fb_get_width();
    int w = 360, x = sw - w - 12, y = MENUBAR_H + 8;
    /* date widget */
    char clk[32];
    ui_format_clock(clk);
    ui_shadow(x, y, w, 96, 20, 20, 90, 6);
    ui_rrect(x, y, w, 96, 20, UI_RGB(44, 44, 50), 236);
    ui_rrect_stroke(x, y, w, 96, 20, 0xFFFFFF, 36);
    ui_text(x + 20, y + 16, "Today", UF_UIB, UI_RGB(150, 150, 156));
    ui_text(x + 20, y + 42, clk, UF_TITLE, 0xFFFFFF);
    y += 108;

    if (nc_count == 0) {
        ui_rrect(x, y, w, 56, 16, UI_RGB(44, 44, 50), 220);
        ui_text_center(x + w / 2, y + 19, "No Notifications", UF_UI, UI_RGB(170, 170, 176));
        return;
    }
    int shown = 0;
    for (int i = nc_count - 1 - nc_scroll; i >= 0 && shown < 7; i--, shown++) {
        nc_entry_t* e = &nc_entries[i];
        int h = 62;
        ui_shadow(x, y, w, h, 16, 14, 70, 4);
        ui_rrect(x, y, w, h, 16, UI_RGB(46, 46, 52), 236);
        ui_rrect_stroke(x, y, w, h, 16, 0xFFFFFF, 36);
        ui_rrect_vgrad(x + 14, y + 13, 36, 36, 9, UI_RGB(120, 92, 236), UI_RGB(52, 40, 150), 255);
        ui_logo(x + 25, y + 22, 17, 0xFFFFFF);
        ui_text(x + 64, y + 12, "NexusOS", UF_UIB, UI_RGB(236, 236, 240));
        char tm[8];
        tm[0] = (char)('0' + e->hour / 10); tm[1] = (char)('0' + e->hour % 10); tm[2] = ':';
        tm[3] = (char)('0' + e->minute / 10); tm[4] = (char)('0' + e->minute % 10); tm[5] = 0;
        ui_text(x + w - 16 - ui_text_width(tm, UF_SMALL), y + 13, tm, UF_SMALL, UI_RGB(150, 150, 156));
        ui_text(x + 64, y + 33, e->msg, UF_UI, UI_RGB(206, 206, 212));
        y += h + 10;
    }
}

int notifcenter_handle_key(char key) {
    if (key == 27) { nc_open = false; return 1; }
    if (key == 'c' || key == 'C') { notifcenter_clear(); return 1; }
    if ((unsigned char)key == 0x80 && nc_scroll > 0) nc_scroll--;
    if ((unsigned char)key == 0x81 && nc_scroll < nc_count - 1) nc_scroll++;
    return 0;
}
