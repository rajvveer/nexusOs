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

extern volatile uint32_t system_ticks;

static int si_scroll = 0;

static void si_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t val = VGA_COLOR(VGA_YELLOW, bg);
    uint8_t check = VGA_COLOR(VGA_LIGHT_GREEN, bg);

    int row = cy;

    gui_draw_text(cx, row, "\x0F System Information", accent);
    row += 2;

    /* System */
    gui_draw_text(cx, row, "OS:", dim);
    gui_draw_text(cx + 12, row, "NexusOS v1.0.0", val); row++;
    gui_draw_text(cx, row, "Kernel:", dim);
    gui_draw_text(cx + 12, row, "Phase 10", val); row++;
    gui_draw_text(cx, row, "Arch:", dim);
    gui_draw_text(cx + 12, row, "x86 (i386)", val); row++;
    gui_draw_text(cx, row, "Mode:", dim);
    gui_draw_text(cx + 12, row, "32-bit Protected", val); row++;

    row++;
    /* Memory */
    char mb[20]; char mn[10];
    gui_draw_text(cx, row, "Total Mem:", dim);
    int_to_str(pmm_get_total_pages() * 4, mn); strcpy(mb, mn); strcat(mb, " KB");
    gui_draw_text(cx + 12, row, mb, val); row++;
    gui_draw_text(cx, row, "Free Mem:", dim);
    int_to_str(pmm_get_free_pages() * 4, mn); strcpy(mb, mn); strcat(mb, " KB");
    gui_draw_text(cx + 12, row, mb, val); row++;
    gui_draw_text(cx, row, "Used Mem:", dim);
    int_to_str(pmm_get_used_pages() * 4, mn); strcpy(mb, mn); strcat(mb, " KB");
    gui_draw_text(cx + 12, row, mb, val); row++;

    row++;
    /* Processes */
    gui_draw_text(cx, row, "Processes:", dim);
    int_to_str(process_count(), mn);
    gui_draw_text(cx + 12, row, mn, val); row++;
    gui_draw_text(cx, row, "Uptime:", dim);
    int_to_str(system_ticks / 18, mn); strcpy(mb, mn); strcat(mb, " sec");
    gui_draw_text(cx + 12, row, mb, val); row++;

    row++;
    /* Features */
    gui_draw_text(cx, row, "Features:", accent); row++;
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
        gui_putchar(cx + 1, row, '\xFB', check);
        gui_draw_text(cx + 3, row, feats[i], tc);
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
    return window_create("System Info", 16, 1, 34, 22, si_draw, si_key);
}
