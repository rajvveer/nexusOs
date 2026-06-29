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
#include "framebuffer.h"
#include "gfx.h"
#include "font.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

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
 * notify_update: Expire old notifications. Returns true if anything changed
 * (a toast expired), so the desktop knows it must do a full repaint to clear
 * the toast area back to wallpaper; otherwise the idle tick can take the cheap
 * taskbar-only path (Phase 53).
 * -------------------------------------------------------------------------- */
bool notify_update(void) {
    bool changed = false;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (notifications[i].active &&
            system_ticks >= notifications[i].expire_tick) {
            notifications[i].active = false;
            changed = true;
        }
    }

    /* Compact — shift active ones to fill gaps */
    for (int i = 0; i < NOTIFY_MAX - 1; i++) {
        if (!notifications[i].active && notifications[i + 1].active) {
            notifications[i] = notifications[i + 1];
            notifications[i + 1].active = false;
        }
    }
    return changed;
}

/* True if any toast is currently visible (forces full redraws while shown). */
bool notify_any_active(void) {
    for (int i = 0; i < NOTIFY_MAX; i++)
        if (notifications[i].active) return true;
    return false;
}

/* --------------------------------------------------------------------------
 * notify_draw: Render active notifications at top-right
 * -------------------------------------------------------------------------- */
void notify_draw(void) {
    if (fb_is_vesa()) {
        /* Modern pixel toast: a rounded dark card with a soft shadow, a slim
         * accent stripe, a bell glyph and crisp white text — no flat black
         * character box. */
        int sw = (int)fb_get_width();
        const int card_w = 248;   /* px */
        const int card_h = 40;
        const int gap    = 10;
        int nx = sw - card_w - 16;

        int count = 0;
        for (int i = 0; i < NOTIFY_MAX; i++) {
            if (!notifications[i].active) continue;
            int ny = 14 + count * (card_h + gap);

            /* Soft drop shadow */
            gfx_draw_shadow(nx, ny, card_w, card_h, 10, FB_RGB(0, 0, 0));
            /* Card body: dark translucent slate over the wallpaper */
            gfx_fill_rect_alpha(nx, ny, card_w, card_h, FB_RGB(28, 32, 46), 232);
            gfx_fill_rounded_rect(nx, ny, card_w, card_h, 10, FB_RGB(30, 34, 50));
            /* Hairline border + accent stripe down the left edge */
            gfx_draw_rounded_rect(nx, ny, card_w, card_h, 10, FB_RGB(70, 80, 110));
            gfx_fill_rounded_rect(nx + 3, ny + 6, 4, card_h - 12, 2, FB_RGB(90, 150, 255));

            /* Bell glyph (accent) */
            font_draw_char_transparent(nx + 16, ny + (card_h - 16) / 2,
                '\x0D', FB_RGB(255, 205, 90));

            /* Message text, truncated to the card width */
            int tx = nx + 34;
            int ty = ny + (card_h - 16) / 2;
            int maxc = (card_w - 44) / 8;
            const char* m = notifications[i].message;
            for (int j = 0; m[j] && j < maxc; j++)
                font_draw_char_transparent(tx + j * 8, ty, (uint8_t)m[j],
                    FB_RGB(238, 240, 248));

            count++;
            if (ny + card_h * 2 > (int)fb_get_height()) break;
        }
        return;
    }

    /* Text-mode fallback (legacy character toast) */
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
