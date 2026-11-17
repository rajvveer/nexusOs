/* ============================================================================
 * NexusOS — System Settings (Aurora, macOS style)
 * ============================================================================
 * Left sidebar of sections + a detail pane. Sidebar rows are clickable; Up/Down
 * also move the selection. The Appearance pane applies the classic cell themes.
 * ============================================================================ */

#include "settings.h"
#include "version.h"
#include "window.h"
#include "appui.h"
#include "macui.h"
#include "framebuffer.h"
#include "theme.h"
#include "memory.h"
#include "heap.h"
#include "string.h"
#include "users.h"

extern volatile uint32_t system_ticks;

enum { SEC_GENERAL = 0, SEC_APPEARANCE, SEC_DISPLAYS, SEC_MEMORY, SEC_ABOUT, SEC_COUNT };
static int section = 0;

static const char* const SEC_NAME[SEC_COUNT] = { "General", "Appearance", "Displays", "Memory", "About" };
static const int SEC_ICON[SEC_COUNT] = { ICON_SETTINGS, ICON_APPEARANCE, ICON_MONITOR, ICON_TASKS, ICON_INFO };

#define SIDEBAR_W 190
#define ROW_H     38

static void kv(int x, int y, int w, const char* k, const char* v) {
    appui_px_text(x, y, k, APPUI_F_BODY, APPUI_SUBTLE);
    appui_px_text_r(x + w, y, v, APPUI_F_BODY, APPUI_TEXT);
}

static void bar(int x, int y, int w, int used, int total, uint32_t col) {
    ui_rrect(x, y, w, 8, 4, 0x3A3A40, 255);
    if (total > 0) {
        /* keep 32-bit: scale down if the product would overflow */
        uint32_t u = (uint32_t)used, t = (uint32_t)total;
        while (u > 4000000u) { u >>= 4; t >>= 4; }
        int fw = t ? (int)(u * (uint32_t)w / t) : 0;
        if (fw < 2 && used > 0) fw = 2;
        if (fw > w) fw = w;
        ui_rrect(x, y, fw, 8, 4, col, 255);
    }
}

static void num_kb(uint32_t kb, char* out) {
    char n[16];
    if (kb < 1024) { int_to_str(kb, n); strcpy(out, n); strcat(out, " KB"); }
    else if (kb < 1024 * 1024) { int_to_str(kb / 1024, n); strcpy(out, n); strcat(out, " MB"); }
    else { int_to_str(kb / (1024 * 1024), n); strcpy(out, n); strcat(out, " GB"); }
}

static void detail(int x, int y, int w, int h) {
    (void)h;
    char buf[32], n[16];
    appui_px_text(x, y, SEC_NAME[section], APPUI_F_TITLE, APPUI_TEXT);
    int ry = y + 46;
    int cardx = x, cardw = w;

    if (section == SEC_GENERAL) {
        ui_rrect(cardx, ry, cardw, 140, 12, APPUI_PANEL, 255);
        int ix = cardx + 18, iw = cardw - 36, iy = ry + 16;
        kv(ix, iy, iw, "Name", "NexusOS"); appui_px_sep(ix, iy + 28, iw);
        kv(ix, iy + 38, iw, "User", users_current_name()); appui_px_sep(ix, iy + 66, iw);
        uint32_t secs = system_ticks / 18, mins = secs / 60; secs %= 60;
        buf[0] = 0; int_to_str(mins, n); strcat(buf, n); strcat(buf, "m "); int_to_str(secs, n); strcat(buf, n); strcat(buf, "s");
        kv(ix, iy + 76, iw, "Uptime", buf); appui_px_sep(ix, iy + 104, iw);
        kv(ix, iy + 114, iw, "Software", NEXUS_VERSION " " NEXUS_CODENAME);
    } else if (section == SEC_APPEARANCE) {
        appui_px_text(cardx, ry, "Theme", APPUI_F_BODY, APPUI_SUBTLE);
        ry += 26;
        for (int i = 0; i < THEME_COUNT; i++) {
            int rh = 34;
            if (i == theme_get_index()) appui_px_row(cardx, ry, cardw, rh, true);
            appui_px_text(cardx + 14, ry + (rh - appui_px_font_h(APPUI_F_BODY)) / 2 + 2, theme_get_name(i),
                          APPUI_F_BODY, i == theme_get_index() ? 0xFFFFFF : APPUI_TEXT);
            if (i == theme_get_index())
                appui_px_text_r(cardx + cardw - 14, ry + (rh - appui_px_font_h(APPUI_F_BODY)) / 2 + 2, "Active", APPUI_F_SMALL, 0xFFFFFF);
            ry += rh + 4;
        }
        appui_px_text(cardx, ry + 8, "Click a theme to apply it to classic apps.", APPUI_F_SMALL, APPUI_FAINT);
    } else if (section == SEC_DISPLAYS) {
        ui_rrect(cardx, ry, cardw, 108, 12, APPUI_PANEL, 255);
        int ix = cardx + 18, iw = cardw - 36, iy = ry + 16;
        int_to_str((int)fb_get_width(), n); strcpy(buf, n); strcat(buf, " x "); int_to_str((int)fb_get_height(), n); strcat(buf, n);
        kv(ix, iy, iw, "Resolution", buf); appui_px_sep(ix, iy + 28, iw);
        int_to_str((int)fb_get_bpp(), n); strcpy(buf, n); strcat(buf, "-bit colour");
        kv(ix, iy + 38, iw, "Depth", buf); appui_px_sep(ix, iy + 66, iw);
        kv(ix, iy + 76, iw, "Engine", fb_is_vesa() ? "VESA linear FB" : "VGA text");
    } else if (section == SEC_MEMORY) {
        ui_rrect(cardx, ry, cardw, 150, 12, APPUI_PANEL, 255);
        int ix = cardx + 18, iw = cardw - 36, iy = ry + 16;
        uint32_t tot = pmm_get_total_pages() * 4, used = pmm_get_used_pages() * 4, mapped = 32 * 1024;
        num_kb(tot, buf); kv(ix, iy, iw, "Installed RAM", buf);
        appui_px_text(ix, iy + 30, "Kernel memory (32 MB window)", APPUI_F_SMALL, APPUI_SUBTLE);
        bar(ix, iy + 50, iw, used, mapped, APPUI_ACCENT);
        num_kb(used, buf); strcat(buf, " used");
        appui_px_text(ix, iy + 62, buf, APPUI_F_SMALL, APPUI_FAINT);
        appui_px_text(ix, iy + 92, "Heap", APPUI_F_SMALL, APPUI_SUBTLE);
        uint32_t hu = heap_get_used(), ht = hu + heap_get_free();
        bar(ix, iy + 110, iw, hu, ht, APPUI_GREEN);
        int_to_str(hu / 1024, n); strcpy(buf, n); strcat(buf, " / "); int_to_str(ht / 1024, n); strcat(buf, n); strcat(buf, " KB");
        appui_px_text(ix, iy + 122, buf, APPUI_F_SMALL, APPUI_FAINT);
    } else {
        int mx = x + w / 2;
        appui_px_appicon(ICON_INFO, mx - 44, ry, 88);
        appui_px_text_c(mx, ry + 100, "NexusOS", APPUI_F_TITLE, APPUI_TEXT);
        appui_px_text_c(mx, ry + 132, "Version " NEXUS_VERSION " (" NEXUS_CODENAME ")", APPUI_F_BODY, APPUI_SUBTLE);
        appui_px_text_c(mx, ry + 168, "A hybrid OS built from scratch in C and x86 assembly.", APPUI_F_SMALL, APPUI_FAINT);
        appui_px_text_c(mx, ry + 190, "No GRUB. No Linux. Just raw metal.", APPUI_F_SMALL, APPUI_FAINT);
    }
}

static void settings_draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    int x, y, w, h;
    if (!appui_content(id, &x, &y, &w, &h)) return;
    appui_px_fill(x, y, w, h, APPUI_BG);
    /* sidebar */
    ui_blend_rect(x, y, SIDEBAR_W, h, APPUI_SIDEBAR, 255);
    appui_px_vsep(x + SIDEBAR_W, y, h);
    int ry = y + 16;
    for (int i = 0; i < SEC_COUNT; i++) {
        int rx = x + 10, rw = SIDEBAR_W - 20;
        if (i == section) appui_px_row(rx, ry, rw, ROW_H, true);
        appui_px_appicon(SEC_ICON[i], rx + 7, ry + (ROW_H - 24) / 2, 24);
        appui_px_text(rx + 40, ry + (ROW_H - appui_px_font_h(APPUI_F_BODY)) / 2 + 2, SEC_NAME[i],
                      APPUI_F_BODY, i == section ? 0xFFFFFF : APPUI_TEXT);
        ry += ROW_H + 4;
    }
    detail(x + SIDEBAR_W + 28, y + 20, w - SIDEBAR_W - 56, h - 40);
}

static void settings_key(int id, char key) {
    (void)id;
    unsigned char k = (unsigned char)key;
    if (k == 0x80 && section > 0) section--;
    else if (k == 0x81 && section < SEC_COUNT - 1) section++;
    else if (section == SEC_APPEARANCE && key == '\n') {} /* handled by click */
}

static void settings_click(int id, int px, int py) {
    int x, y, w, h;
    if (!appui_content(id, &x, &y, &w, &h)) return;
    /* content-relative: sidebar starts at x=0 inside content */
    if (px < SIDEBAR_W) {
        int ry = 16;
        for (int i = 0; i < SEC_COUNT; i++) {
            if (py >= ry && py < ry + ROW_H) { section = i; return; }
            ry += ROW_H + 4;
        }
    } else if (section == SEC_APPEARANCE) {
        int ry = 20 + 46 + 26;         /* matches detail() appearance layout */
        for (int i = 0; i < THEME_COUNT; i++) {
            if (py >= ry && py < ry + 34) { theme_set(i); return; }
            ry += 38;
        }
    }
}

int settings_open(void) {
    section = 0;
    int id = window_create("Settings", 20, 3, 66, 26, settings_draw, settings_key);
    window_set_on_click(id, settings_click);
    return id;
}
