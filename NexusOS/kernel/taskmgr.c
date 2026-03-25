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

extern volatile uint32_t system_ticks;

static int tm_selected = 0;

static void tm_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t hi = t->menu_highlight;
    uint8_t warn_col = VGA_COLOR(VGA_YELLOW, bg);

    int row = cy;

    gui_draw_text(cx + 1, row, "\x0F Task Manager", accent);
    row++;

    gui_draw_text(cx + 1, row, "PID", dim);
    gui_draw_text(cx + 5, row, "Name", dim);
    gui_draw_text(cx + 18, row, "State", dim);
    gui_draw_text(cx + 26, row, "Ticks", dim);
    row++;

    for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
        gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    process_t* table = process_get_table();
    int vis = 0;
    for (int i = 0; i < MAX_PROCESSES && row < cy + ch - 3; i++) {
        process_t* p = &table[i];
        if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED) continue;

        bool is_sel = (vis == tm_selected);
        uint8_t col = is_sel ? hi : tc;

        if (is_sel) {
            for (int j = cx; j < cx + cw - 1; j++)
                gui_putchar(j, row, ' ', hi);
        }

        char num[8];
        int_to_str(p->pid, num);
        gui_draw_text(cx + 1, row, num, col);
        gui_draw_text(cx + 5, row, p->name, col);

        const char* state_str;
        switch (p->state) {
            case PROC_RUNNING:  state_str = "RUN"; break;
            case PROC_READY:    state_str = "RDY"; break;
            case PROC_BLOCKED:  state_str = "BLK"; break;
            default:            state_str = "???"; break;
        }
        gui_draw_text(cx + 18, row, state_str, col);

        int_to_str(p->ticks, num);
        gui_draw_text(cx + 26, row, num, col);

        row++; vis++;
    }

    row = cy + ch - 3;
    for (int i = 0; i < cw - 1; i++)
        gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    uint32_t total = pmm_get_total_pages();
    uint32_t used = pmm_get_used_pages();
    int bar_w = cw - 12;
    int filled = (total > 0) ? (int)((used * bar_w) / total) : 0;

    gui_draw_text(cx + 1, row, "Mem [", dim);
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) gui_putchar(cx + 6 + i, row, (char)0xDB, warn_col);
        else gui_putchar(cx + 6 + i, row, (char)0xB0, dim);
    }
    gui_putchar(cx + 6 + bar_w, row, ']', dim);
    row++;

    gui_draw_text(cx + 1, row, "K:Kill  Up/Down:Select", dim);
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
    return window_create("Task Manager", 14, 2, 34, 18, tm_draw, tm_key);
}
