/* ============================================================================
 * NexusOS — Task Manager (Implementation)
 * ============================================================================
 * Process viewer with select + kill. Auto-refreshes.
 * ============================================================================ */

#include "taskmgr.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "process.h"
#include "memory.h"
#include "heap.h"
#include "appui.h"

extern volatile uint32_t system_ticks;

static int tm_selected = 0;

static void tm_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    appui_theme_t ui = appui_theme();
    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "Task Manager", "Processes and memory");

    int row = cy + 3;
    gui_draw_text(cx + 1, row, "PID", ui.muted);
    gui_draw_text(cx + 7, row, "Name", ui.muted);
    gui_draw_text(cx + 24, row, "State", ui.muted);
    gui_draw_text(cx + 34, row, "Ticks", ui.muted);
    row++;
    appui_hline(cx, row, cw, ui.muted);
    row++;

    process_t* table = process_get_table();
    int vis = 0;
    for (int i = 0; i < MAX_PROCESSES && row < cy + ch - 3; i++) {
        process_t* p = &table[i];
        if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED) continue;

        bool is_sel = (vis == tm_selected);
        uint8_t col = is_sel ? ui.selected : ui.text;
        appui_row(cx, row, cw, is_sel, ui.panel, ui.selected);

        char num[8];
        int_to_str(p->pid, num);
        gui_draw_text(cx + 1, row, num, col);
        appui_text(cx + 7, row, p->name, 16, col);

        const char* state_str;
        switch (p->state) {
            case PROC_RUNNING:  state_str = "RUN"; break;
            case PROC_READY:    state_str = "RDY"; break;
            case PROC_BLOCKED:  state_str = "BLK"; break;
            default:            state_str = "???"; break;
        }
        gui_draw_text(cx + 24, row, state_str, col);

        int_to_str(p->ticks, num);
        gui_draw_text(cx + 34, row, num, col);

        row++; vis++;
    }

    row = cy + ch - 3;
    appui_hline(cx, row, cw, ui.muted);
    row++;

    uint32_t total = pmm_get_total_pages();
    uint32_t used = pmm_get_used_pages();
    int bar_w = cw - 12;
    int filled = (total > 0) ? (int)((used * bar_w) / total) : 0;

    gui_draw_text(cx + 1, row, "Mem [", ui.muted);
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) gui_putchar(cx + 6 + i, row, (char)0xDB, ui.warn);
        else gui_putchar(cx + 6 + i, row, (char)0xB0, ui.muted);
    }
    gui_putchar(cx + 6 + bar_w, row, ']', ui.muted);
    row++;

    appui_status(cx, row, cw, "Up/Down Select   K Kill");
    (void)ch;
}

static void tm_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80 && tm_selected > 0) tm_selected--;
    if ((unsigned char)key == 0x81) tm_selected++;

    if (key == 'k' || key == 'K') {
        process_t* table = process_get_table();
        int vis = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t* p = &table[i];
            if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED) continue;
            if (vis == tm_selected) {
                if (p->pid > 1) process_terminate(p->pid);
                break;
            }
            vis++;
        }
    }
}

int taskmgr_open(void) {
    tm_selected = 0;
    return window_create("Task Manager", 14, 4, 50, 22, tm_draw, tm_key);
}
