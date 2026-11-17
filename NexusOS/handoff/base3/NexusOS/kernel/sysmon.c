/* ============================================================================
 * NexusOS — Activity Monitor (Aurora, macOS style)
 * ============================================================================
 * Memory + heap gauges and a live process table. Read-only, refreshes every
 * frame. Uses only the existing process API so it survives a scheduler rewrite.
 * ============================================================================ */

#include "sysmon.h"
#include "window.h"
#include "appui.h"
#include "macui.h"
#include "framebuffer.h"
#include "memory.h"
#include "heap.h"
#include "process.h"
#include "rtc.h"
#include "string.h"

extern volatile uint32_t system_ticks;

static void gauge(int x, int y, int w, const char* label, uint32_t used, uint32_t total,
                  const char* detail, uint32_t col) {
    appui_px_text(x, y, label, APPUI_F_BODY, APPUI_TEXT);
    appui_px_text_r(x + w, y, detail, APPUI_F_SMALL, APPUI_SUBTLE);
    int by = y + 24;
    ui_rrect(x, by, w, 10, 5, 0x3A3A40, 255);
    if (total) {
        uint32_t u = used, t = total;
        while (u > 4000000u) { u >>= 4; t >>= 4; }
        int fw = t ? (int)(u * (uint32_t)w / t) : 0;
        if (fw < 3 && used) fw = 3;
        if (fw > w) fw = w;
        ui_rrect(x, by, fw, 10, 5, col, 255);
    }
}

static void sysmon_draw(int id, int a, int b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    int x, y, w, h;
    if (!appui_content(id, &x, &y, &w, &h)) return;
    appui_px_fill(x, y, w, h, APPUI_BG);

    int pad = 20, cx = x + pad, cw = w - 2 * pad;
    appui_px_text(cx, y + 16, "Activity Monitor", APPUI_F_TITLE, APPUI_TEXT);

    /* uptime + time, right-aligned */
    char line[40], n[12];
    uint32_t s = system_ticks / 18, m = s / 60, hh = m / 60; s %= 60; m %= 60;
    line[0] = 0; int_to_str(hh, n); strcat(line, n); strcat(line, "h ");
    int_to_str(m, n); strcat(line, n); strcat(line, "m ");
    int_to_str(s, n); strcat(line, n); strcat(line, "s uptime");
    appui_px_text_r(cx + cw, y + 26, line, APPUI_F_SMALL, APPUI_SUBTLE);

    /* gauges card */
    int gy = y + 58;
    ui_rrect(cx, gy, cw, 118, 12, APPUI_PANEL, 255);
    int gx = cx + 18, gw = cw - 36;
    char det[40];
    uint32_t used = pmm_get_used_pages() * 4, mapped = 32 * 1024;
    int_to_str(used / 1024, n); strcpy(det, n); strcat(det, " / 32 MB kernel map");
    gauge(gx, gy + 16, gw, "Memory", used, mapped, det, APPUI_ACCENT);
    uint32_t hu = heap_get_used(), ht = hu + heap_get_free();
    int_to_str(hu / 1024, n); strcpy(det, n); strcat(det, " / "); int_to_str(ht / 1024, n); strcat(det, n); strcat(det, " KB heap");
    gauge(gx, gy + 70, gw, "Heap", hu, ht, det, APPUI_GREEN);

    /* process table */
    int ty = gy + 134;
    appui_px_text(cx, ty, "Processes", APPUI_F_BOLD, APPUI_TEXT);
    int_to_str(process_count(), n);
    appui_px_text_r(cx + cw, ty, n, APPUI_F_BODY, APPUI_SUBTLE);
    ty += 26;
    appui_px_text(cx + 8, ty, "PID", APPUI_F_SMALL, APPUI_FAINT);
    appui_px_text(cx + 70, ty, "Process Name", APPUI_F_SMALL, APPUI_FAINT);
    appui_px_text_r(cx + cw - 8, ty, "State", APPUI_F_SMALL, APPUI_FAINT);
    ty += 18;
    appui_px_sep(cx, ty, cw); ty += 6;

    int rh = 26, idx = 0;
    for (int i = 0; i < MAX_PROCESSES && ty + rh < y + h; i++) {
        process_t* p = process_get_by_pid(i);
        if (!p || p->state == PROC_UNUSED || p->state == PROC_TERMINATED) continue;
        if (idx & 1) ui_rrect(cx, ty, cw, rh, 6, APPUI_PANEL, 160);
        int tyc = ty + (rh - appui_px_font_h(APPUI_F_BODY)) / 2 + 2;
        int_to_str(p->pid, n);
        appui_px_text(cx + 8, tyc, n, APPUI_F_BODY, APPUI_SUBTLE);
        ui_rrect(cx + 42, ty + 4, 18, 18, 5, p->state == PROC_RUNNING ? 0x3A6EA5 : 0x55555C, 255);
        appui_px_text_c(cx + 51, ty + 4, "N", APPUI_F_SMALL, 0xFFFFFF);
        char nm[24]; int j = 0; for (; p->name[j] && j < 22; j++) nm[j] = p->name[j]; nm[j] = 0;
        appui_px_text(cx + 70, tyc, nm, APPUI_F_BODY, APPUI_TEXT);
        const char* st; uint32_t sc;
        switch (p->state) {
            case PROC_RUNNING: st = "Running"; sc = APPUI_GREEN; break;
            case PROC_READY:   st = "Ready";   sc = APPUI_ACCENT; break;
            case PROC_BLOCKED: st = "Blocked"; sc = APPUI_ORANGE; break;
            default:           st = "-";       sc = APPUI_FAINT; break;
        }
        int sw2 = appui_px_text_w(st, APPUI_F_SMALL);
        ui_rrect(cx + cw - sw2 - 16, ty + (rh - 18) / 2, sw2 + 16, 18, 9, sc, 46);
        appui_px_text_r(cx + cw - 8, tyc, st, APPUI_F_SMALL, sc);
        ty += rh; idx++;
    }
}

static void sysmon_key(int id, char key) { (void)id; (void)key; }

int sysmon_open(void) {
    return window_create("Activity Monitor", 18, 3, 52, 27, sysmon_draw, sysmon_key);
}
