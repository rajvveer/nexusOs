/* ============================================================================
 * NexusOS — Settings App (Implementation)
 * ============================================================================
 * Desktop window with live theme selection, system info, and display info.
 * ============================================================================ */

#include "settings.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "memory.h"
#include "heap.h"
#include "string.h"
#include "rtc.h"
#include "wallpaper.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Settings state */
static int settings_tab = 0;
static int theme_cursor = 0;
static int wp_cursor = 0;

/* --------------------------------------------------------------------------
 * Settings window callbacks
 * -------------------------------------------------------------------------- */
static void settings_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t text_color = t->win_content;
    uint8_t bg = (text_color >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t val_col = VGA_COLOR(VGA_WHITE, bg);
    uint8_t sel_col = t->menu_highlight;

    int row = cy;

    /* Title */
    gui_draw_text(cx + 1, row, "\xF0 Settings", accent);
    row++;

    /* Tab bar */
    const char* tabs[] = { " Theme ", " System ", " About " };
    int tab_x = cx;
    for (int i = 0; i < 3; i++) {
        uint8_t tab_col = (i == settings_tab) ? sel_col : dim;
        gui_draw_text(tab_x, row, tabs[i], tab_col);
        tab_x += strlen(tabs[i]) + 1;
    }
    row++;

    /* Separator */
    for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
        gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    if (settings_tab == 0) {
        /* === Theme tab === */
        gui_draw_text(cx + 1, row, "Select a theme:", text_color);
        row++;
        row++;

        for (int i = 0; i < THEME_COUNT; i++) {
            bool is_sel = (i == theme_cursor);
            uint8_t tc = is_sel ? sel_col : text_color;

            if (is_sel) {
                for (int j = cx; j < cx + cw - 1; j++)
                    gui_putchar(j, row, ' ', sel_col);
            }

            const char* name = theme_get_name(i);
            gui_putchar(cx + 2, row, is_sel ? '\x10' : ' ', tc);
            gui_draw_text(cx + 4, row, name, tc);

            row++;
        }

        row++;
        gui_draw_text(cx + 1, row, "Up/Down: Select", dim);
        row++;
        gui_draw_text(cx + 1, row, "Enter: Apply theme", dim);
        row++;
        row++;

        /* Wallpaper section */
        gui_draw_text(cx + 1, row, "Wallpaper:", accent);
        row++;
        for (int i = 0; i < WP_COUNT; i++) {
            bool is_sel = (i == wp_cursor);
            uint8_t wc = is_sel ? sel_col : text_color;
            gui_putchar(cx + 2, row, is_sel ? '>' : ' ', wc);
            gui_draw_text(cx + 4, row, wallpaper_get_name(i), wc);
            if (i == wallpaper_get()) gui_draw_text(cx + 15, row, "*", VGA_COLOR(VGA_YELLOW, bg));
            row++;
        }

    } else if (settings_tab == 1) {
        /* === System tab === */
        gui_draw_text(cx + 1, row, "System Information", accent);
        row++;
        row++;

        /* Architecture */
        gui_draw_text(cx + 1, row, "Arch:    ", dim);
        gui_draw_text(cx + 10, row, "x86 (i386) 32-bit", val_col);
        row++;

        /* Display */
        gui_draw_text(cx + 1, row, "Display: ", dim);
        gui_draw_text(cx + 10, row, "VGA 80x25 text mode", val_col);
        row++;

        /* Input */
        gui_draw_text(cx + 1, row, "Input:   ", dim);
        gui_draw_text(cx + 10, row, "PS/2 Keyboard + Mouse", val_col);
        row++;
        row++;

        /* Memory */
        char num[16];
        gui_draw_text(cx + 1, row, "Memory:", accent);
        row++;

        uint32_t total = pmm_get_total_pages() * 4;
        uint32_t used = pmm_get_used_pages() * 4;
        uint32_t free_k = pmm_get_free_pages() * 4;

        gui_draw_text(cx + 2, row, "Total: ", dim);
        int_to_str(total, num);
        gui_draw_text(cx + 9, row, num, val_col);
        gui_draw_text(cx + 9 + strlen(num), row, " KB", dim);
        row++;

        gui_draw_text(cx + 2, row, "Used:  ", dim);
        int_to_str(used, num);
        gui_draw_text(cx + 9, row, num, VGA_COLOR(VGA_YELLOW, bg));
        gui_draw_text(cx + 9 + strlen(num), row, " KB", dim);
        row++;

        gui_draw_text(cx + 2, row, "Free:  ", dim);
        int_to_str(free_k, num);
        gui_draw_text(cx + 9, row, num, VGA_COLOR(VGA_LIGHT_GREEN, bg));
        gui_draw_text(cx + 9 + strlen(num), row, " KB", dim);
        row++;
        row++;

        /* Heap */
        gui_draw_text(cx + 1, row, "Heap:", accent);
        row++;
        uint32_t hu = heap_get_used();
        uint32_t hf = heap_get_free();
        gui_draw_text(cx + 2, row, "Used: ", dim);
        int_to_str(hu, num);
        gui_draw_text(cx + 8, row, num, val_col);
        gui_draw_text(cx + 8 + strlen(num), row, " B", dim);
        row++;
        gui_draw_text(cx + 2, row, "Free: ", dim);
        int_to_str(hf, num);
        gui_draw_text(cx + 8, row, num, val_col);
        gui_draw_text(cx + 8 + strlen(num), row, " B", dim);

    } else {
        /* === About tab === */
        gui_draw_text(cx + 1, row, "About NexusOS", accent);
        row++;
        row++;

        gui_draw_text(cx + 1, row, "NexusOS v0.7.0", val_col);
        row++;
        gui_draw_text(cx + 1, row, "Phase 7: Window Mgmt", VGA_COLOR(VGA_YELLOW, bg));
        row++;
        row++;
        gui_draw_text(cx + 1, row, "The Hybrid Operating System", text_color);
        row++;
        gui_draw_text(cx + 1, row, "Built from scratch in C & ASM", text_color);
        row++;
        row++;
        gui_draw_text(cx + 1, row, "Best of Windows + macOS + Linux", dim);
        row++;
        row++;

        /* Uptime */
        uint32_t secs = system_ticks / 18;
        uint32_t mins = secs / 60;
        secs %= 60;
        char tbuf[20]; char nb[8];
        tbuf[0] = '\0';
        int_to_str(mins, nb); strcat(tbuf, nb); strcat(tbuf, "m ");
        int_to_str(secs, nb); strcat(tbuf, nb); strcat(tbuf, "s");
        gui_draw_text(cx + 1, row, "Uptime: ", dim);
        gui_draw_text(cx + 9, row, tbuf, val_col);
    }

    (void)ch;
    (void)cw;
}

static void settings_key(int id, char key) {
    (void)id;

    /* Tab switch: left/right or [ / ] */
    if (key == '[' || (unsigned char)key == 0x82) {
        if (settings_tab > 0) settings_tab--;
        return;
    }
    if (key == ']' || (unsigned char)key == 0x83) {
        if (settings_tab < 2) settings_tab++;
        return;
    }

    if (settings_tab == 0) {
        if ((unsigned char)key == 0x80) {
            if (theme_cursor > 0) theme_cursor--;
        } else if ((unsigned char)key == 0x81) {
            if (theme_cursor < THEME_COUNT - 1) theme_cursor++;
        } else if (key == '\n') {
            theme_set(theme_cursor);
        } else if (key == 'w' || key == 'W') {
            /* Switch to wallpaper selection */
            wp_cursor = (wp_cursor + 1) % WP_COUNT;
            wallpaper_set(wp_cursor);
        }
    }
}

/* --------------------------------------------------------------------------
 * settings_open: Create a settings window
 * -------------------------------------------------------------------------- */
int settings_open(void) {
    settings_tab = 0;
    theme_cursor = 0;
    wp_cursor = wallpaper_get();
    return window_create("Settings", 18, 2, 34, 22, settings_draw, settings_key);
}
