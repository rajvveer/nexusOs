/* ============================================================================
 * NexusOS — System Info (Implementation)
 * ============================================================================
 * Shows detailed system information: hardware, kernel, features.
 * ============================================================================ */

#include "sysinfo.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "memory.h"
#include "process.h"
#include "rtc.h"
#include "appui.h"

extern volatile uint32_t system_ticks;

static int si_scroll = 0;

static void si_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    appui_theme_t ui = appui_theme();
    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "System Information", "Hardware, kernel, and runtime");

    int row = cy + 3;

    /* System */
    appui_kv(cx + 1, row, "OS", "NexusOS", cx + 14, cw - 15); row++;
    appui_kv(cx + 1, row, "Kernel", "Phase 34 desktop", cx + 14, cw - 15); row++;
    appui_kv(cx + 1, row, "Arch", "x86 (i386)", cx + 14, cw - 15); row++;
    appui_kv(cx + 1, row, "Mode", "32-bit protected", cx + 14, cw - 15); row++;

    row++;
    /* Memory */
    char mb[20]; char mn[10];
    int_to_str(pmm_get_total_pages() * 4, mn); strcpy(mb, mn); strcat(mb, " KB");
    appui_kv(cx + 1, row, "Total Mem", mb, cx + 14, cw - 15); row++;
    int_to_str(pmm_get_free_pages() * 4, mn); strcpy(mb, mn); strcat(mb, " KB");
    appui_kv(cx + 1, row, "Free Mem", mb, cx + 14, cw - 15); row++;
    int_to_str(pmm_get_used_pages() * 4, mn); strcpy(mb, mn); strcat(mb, " KB");
    appui_kv(cx + 1, row, "Used Mem", mb, cx + 14, cw - 15); row++;

    row++;
    /* Processes */
    int_to_str(process_count(), mn);
    appui_kv(cx + 1, row, "Processes", mn, cx + 14, cw - 15); row++;
    int_to_str(system_ticks / 18, mn); strcpy(mb, mn); strcat(mb, " sec");
    appui_kv(cx + 1, row, "Uptime", mb, cx + 14, cw - 15); row++;

    row++;
    /* Features */
    appui_text(cx + 1, row, "Features", cw - 2, ui.accent); row++;
    const char* feats[] = {
        "GUI Window Manager",
        "Virtual Filesystem",
        "Process Scheduler",
        "Mouse + Keyboard",
        "PC Speaker Audio",
        "Virtual Desktops",
        "Recycle Bin",
        NULL
    };
    for (int i = 0; feats[i] && row < cy + ch - 1; i++) {
        gui_putchar(cx + 1, row, '+', ui.good);
        appui_text(cx + 3, row, feats[i], cw - 4, ui.text);
        row++;
    }

    (void)cw; (void)ch; (void)si_scroll;
}

static void si_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80) { if (si_scroll > 0) si_scroll--; }
    if ((unsigned char)key == 0x81) si_scroll++;
}

int sysinfo_open(void) {
    si_scroll = 0;
    return window_create("System Info", 16, 4, 46, 24, si_draw, si_key);
}
