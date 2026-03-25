/* ============================================================================
 * NexusOS — System Monitor App (Implementation)
 * ============================================================================
 * Live-updating system info window: memory, heap, uptime, processes.
 * Auto-refreshes each render frame.
 * ============================================================================ */

#include "sysmon.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "memory.h"
#include "heap.h"
#include "process.h"
#include "rtc.h"
#include "string.h"
#include "framebuffer.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* System tick counter */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Helper: draw a labeled value line
 * -------------------------------------------------------------------------- */
static void draw_label_value(int cx, int row, const char* label,
                             uint32_t value, const char* unit,
                             uint8_t label_col, uint8_t val_col) {
    gui_draw_text(cx + 1, row, label, label_col);
    int llen = strlen(label);

    char num[16];
    int_to_str(value, num);
    gui_draw_text(cx + 1 + llen, row, num, val_col);
    int nlen = strlen(num);
    gui_draw_text(cx + 1 + llen + nlen, row, unit, label_col);
}

/* --------------------------------------------------------------------------
 * Helper: draw a memory bar [████░░░░░░]
 * -------------------------------------------------------------------------- */
static void draw_bar(int cx, int row, int bar_w, uint32_t used, uint32_t total,
                     uint8_t fill_col, uint8_t empty_col, uint32_t fill_rgb, uint32_t empty_rgb) {
    if (total == 0) total = 1;
    
    if (fb_is_vesa()) {
        int px = cx * 8;
        int py = row * 16;
        int pw = (bar_w + 2) * 8; /* +2 for the brackets area */
        int ph = 12;
        int filled_pw = (used * pw) / total;
        if (filled_pw > pw) filled_pw = pw;
        
        fb_fill_rect(px, py + 2, pw, ph, empty_rgb);
        fb_draw_rect(px, py + 2, pw, ph, FB_RGB(100, 100, 100));
        if (filled_pw > 2) {
            fb_fill_rect(px + 1, py + 3, filled_pw - 2, ph - 2, fill_rgb);
            /* Gloss highlight */
            fb_fill_rect(px + 1, py + 3, filled_pw - 2, 3, FB_RGB(255, 255, 255));
        }
    } else {
        int filled = (used * bar_w) / total;
        if (filled > bar_w) filled = bar_w;

        gui_putchar(cx, row, '[', fill_col);
        for (int i = 0; i < bar_w; i++) {
            if (i < filled) {
                gui_putchar(cx + 1 + i, row, (char)0xDB, fill_col);  /* █ */
            } else {
                gui_putchar(cx + 1 + i, row, (char)0xB0, empty_col); /* ░ */
            }
        }
        gui_putchar(cx + 1 + bar_w, row, ']', fill_col);
    }
}

/* --------------------------------------------------------------------------
 * Sysmon window callbacks
 * -------------------------------------------------------------------------- */
static void sysmon_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t text_color = t->win_content;
    uint8_t bg = (text_color >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t val_col = VGA_COLOR(VGA_WHITE, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t bar_fill = VGA_COLOR(VGA_LIGHT_GREEN, bg);
    uint8_t bar_empty = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t warn_col = VGA_COLOR(VGA_YELLOW, bg);

    int row = cy;

    /* Title */
    gui_draw_text(cx + 1, row, "\x04 System Monitor", accent);
    row++;

    /* Separator */
    for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
        gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    /* Uptime */
    uint32_t seconds = system_ticks / 18;
    uint32_t minutes = seconds / 60;
    uint32_t hours = minutes / 60;
    seconds %= 60;
    minutes %= 60;

    gui_draw_text(cx + 1, row, "Uptime: ", dim);
    char tbuf[20];
    char num[12];
    tbuf[0] = '\0';
    int_to_str(hours, num); strcat(tbuf, num); strcat(tbuf, "h ");
    int_to_str(minutes, num); strcat(tbuf, num); strcat(tbuf, "m ");
    int_to_str(seconds, num); strcat(tbuf, num); strcat(tbuf, "s");
    gui_draw_text(cx + 9, row, tbuf, val_col);
    row++;

    /* Date/Time */
    rtc_time_t rtc;
    rtc_read(&rtc);
    char time_str[10];
    rtc_format_time(&rtc, time_str);
    gui_draw_text(cx + 1, row, "Time:   ", dim);
    gui_draw_text(cx + 9, row, time_str, val_col);
    row++;

    /* Separator */
    for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
        gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    /* Physical Memory */
    uint32_t total_pages = pmm_get_total_pages();
    uint32_t used_pages = pmm_get_used_pages();
    uint32_t free_pages = pmm_get_free_pages();

    gui_draw_text(cx + 1, row, "Physical Memory", accent);
    row++;

    draw_label_value(cx, row, " Total: ", total_pages * 4, " KB", dim, val_col);
    row++;
    draw_label_value(cx, row, " Used:  ", used_pages * 4, " KB", dim, warn_col);
    row++;
    draw_label_value(cx, row, " Free:  ", free_pages * 4, " KB", dim, bar_fill);
    row++;

    /* Memory bar */
    int bar_w = cw - 4;
    if (bar_w > 30) bar_w = 30;
    draw_bar(cx + 1, row, bar_w, used_pages, total_pages, bar_fill, bar_empty, FB_RGB(100, 255, 100), FB_RGB(40, 40, 40));
    row++;

    /* Separator */
    for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
        gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    /* Heap */
    uint32_t heap_used = heap_get_used();
    uint32_t heap_free = heap_get_free();
    uint32_t heap_total = heap_used + heap_free;

    gui_draw_text(cx + 1, row, "Heap Memory", accent);
    row++;

    draw_label_value(cx, row, " Used: ", heap_used, " B", dim, warn_col);
    row++;
    draw_label_value(cx, row, " Free: ", heap_free, " B", dim, bar_fill);
    row++;

    /* Heap bar */
    if (row < cy + ch) {
        draw_bar(cx + 1, row, bar_w, heap_used, heap_total, bar_fill, bar_empty, FB_RGB(100, 255, 100), FB_RGB(40, 40, 40));
        row++;
    }

    /* Separator */
    if (row < cy + ch) {
        for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
            gui_putchar(cx + i, row, (char)0xC4, dim);
        row++;
    }

    /* Process list */
    if (row < cy + ch) {
        gui_draw_text(cx + 1, row, "Processes", accent);

        char pcount[8];
        int_to_str(process_count(), pcount);
        int pcl = strlen(pcount);
        gui_draw_text(cx + cw - 2 - pcl, row, pcount, val_col);
        row++;
    }

    if (row < cy + ch) {
        gui_draw_text(cx + 1, row, "PID  NAME          STATE", dim);
        row++;
    }

    for (int i = 0; i < MAX_PROCESSES && row < cy + ch; i++) {
        process_t* p = process_get_by_pid(i);
        if (!p || p->state == PROC_UNUSED || p->state == PROC_TERMINATED) continue;

        char pid_str[8];
        int_to_str(p->pid, pid_str);

        gui_draw_text(cx + 1, row, pid_str, val_col);

        /* Name (padded to 14 chars) */
        int j = 0;
        while (p->name[j] && j < 14) {
            gui_putchar(cx + 6 + j, row, p->name[j], text_color);
            j++;
        }

        /* State */
        const char* state_str;
        uint8_t state_col = dim;
        switch (p->state) {
            case PROC_READY:   state_str = "READY";   state_col = val_col; break;
            case PROC_RUNNING: state_str = "RUNNING"; state_col = bar_fill; break;
            case PROC_BLOCKED: state_str = "BLOCKED"; state_col = warn_col; break;
            default:           state_str = "???";     break;
        }
        gui_draw_text(cx + 20, row, state_str, state_col);
        row++;
    }

    (void)ch;
}

static void sysmon_key(int id, char key) {
    (void)id;
    (void)key;
    /* System monitor is read-only — no key handling needed */
}

/* --------------------------------------------------------------------------
 * sysmon_open: Create a system monitor window
 * -------------------------------------------------------------------------- */
int sysmon_open(void) {
    return window_create("System Monitor", 20, 1, 38, 22, sysmon_draw, sysmon_key);
}
