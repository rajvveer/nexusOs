/* ============================================================================
 * NexusOS — Notification Toasts (Implementation)
 * ============================================================================
 * Auto-dismiss popup notifications at the top-right of the desktop.
 * Supports up to 3 stacked toasts that dismiss after ~3 seconds.
 * ============================================================================ */

#include "notify.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Notification slot */
#define NOTIFY_MAX      3
#define NOTIFY_WIDTH   26
#define NOTIFY_MSG_MAX 24
#define NOTIFY_DURATION 54   /* ~3 seconds at 18.2 Hz */

typedef struct {
    bool     active;
    char     message[NOTIFY_MSG_MAX];
    uint32_t expire_tick;
} notification_t;

static notification_t notifications[NOTIFY_MAX];

/* --------------------------------------------------------------------------
 * notify_push: Add a new notification
 * -------------------------------------------------------------------------- */
void notify_push(const char* message) {
    /* Find empty slot, or evict oldest */
    int slot = -1;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notifications[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* Shift all up, evict oldest (index 0) */
        for (int i = 0; i < NOTIFY_MAX - 1; i++) {
            notifications[i] = notifications[i + 1];
        }
        slot = NOTIFY_MAX - 1;
    }

    notifications[slot].active = true;
    strncpy(notifications[slot].message, message, NOTIFY_MSG_MAX - 1);
    notifications[slot].message[NOTIFY_MSG_MAX - 1] = '\0';
    notifications[slot].expire_tick = system_ticks + NOTIFY_DURATION;
}

/* --------------------------------------------------------------------------
 * notify_update: Expire old notifications
 * -------------------------------------------------------------------------- */
void notify_update(void) {
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (notifications[i].active &&
            system_ticks >= notifications[i].expire_tick) {
            notifications[i].active = false;
        }
    }

    /* Compact — shift active ones to fill gaps */
    for (int i = 0; i < NOTIFY_MAX - 1; i++) {
        if (!notifications[i].active && notifications[i + 1].active) {
            notifications[i] = notifications[i + 1];
            notifications[i + 1].active = false;
        }
    }
}

/* --------------------------------------------------------------------------
 * notify_draw: Render active notifications at top-right
 * -------------------------------------------------------------------------- */
void notify_draw(void) {
    int count = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notifications[i].active) continue;

        int nx = GUI_WIDTH - NOTIFY_WIDTH - 1;
        int ny = 1 + count * 3;

        if (ny + 2 >= GUI_HEIGHT - 2) break;

        const theme_t* t = theme_get();
        uint8_t bg = t->menu_bg;
        uint8_t border_col = VGA_COLOR(VGA_LIGHT_CYAN, (bg >> 4) & 0x0F);

        /* Draw toast box */
        gui_rect_t rect = { nx, ny, NOTIFY_WIDTH, 3 };
        gui_fill_rect(rect, ' ', bg);
        gui_draw_box(rect, border_col);

        /* Bell icon + message */
        gui_putchar(nx + 1, ny + 1, '\x0D', VGA_COLOR(VGA_YELLOW, (bg >> 4) & 0x0F));
        gui_putchar(nx + 2, ny + 1, ' ', bg);

        /* Truncate message to fit */
        int max_len = NOTIFY_WIDTH - 5;
        int j = 0;
        while (notifications[i].message[j] && j < max_len) {
            gui_putchar(nx + 3 + j, ny + 1, notifications[i].message[j], bg);
            j++;
        }

        count++;
    }
}
