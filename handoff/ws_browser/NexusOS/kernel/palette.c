/* ============================================================================
 * NexusOS — Command Palette (Implementation)
 * ============================================================================
 * Spotlight-style quick-launch overlay. Type to filter, Enter to launch.
 * ============================================================================ */

#include "palette.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

/* Palette item */
typedef struct {
    const char* name;
    int action;
} pal_item_t;

static const pal_item_t all_items[] = {
    { "Terminal",      PAL_TERMINAL },
    { "File Manager",  PAL_FILEMGR  },
    { "Calculator",    PAL_CALC     },
    { "Sys Monitor",   PAL_SYSMON   },
    { "Settings",      PAL_SETTINGS },
    { "Snake Game",    PAL_SNAKE    },
    { "About",         PAL_ABOUT    },
    { "Lock Screen",   PAL_LOCK     },
    { "Next Theme",    PAL_THEME    },
    { "Notepad",       PAL_NOTEPAD  },
    { "Task Manager",  PAL_TASKMGR  },
    { "Calendar",      PAL_CALENDAR },
    { "Music Player",   13  },
    { "Help",           14  },
    { "Paint",          15  },
    { "Minesweeper",    16  },
    { "Recycle Bin",    17  },
    { "System Info",    18  },
    { "Todo",           19  },
    { "Clock",          20  },
    { "Pong",           21  },
    { "File Search",    22  },
    { "Tetris",         23  },
    { "Hex Viewer",     24  },
    { "Contacts",       25  },
    { "Color Picker",   26  },
    { "System Log",     27  },
    { "Clipboard",      28  },
    { "Appearance",     29  },
    { "File Ops",       31  },
};

#define ALL_COUNT (sizeof(all_items) / sizeof(all_items[0]))
#define PAL_MAX_VISIBLE 8

/* State */
static bool pal_open_flag = false;
static char pal_query[20];
static int  pal_query_len = 0;
static int  pal_filtered[ALL_COUNT];
static int  pal_filtered_count = 0;
static int  pal_selected = 0;

/* Layout */
#define PAL_WIDTH   30
#define PAL_HEIGHT  12
#define PAL_X       ((GUI_WIDTH - PAL_WIDTH) / 2)
#define PAL_Y       3

/* --------------------------------------------------------------------------
 * Simple case-insensitive substring match
 * -------------------------------------------------------------------------- */
static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static bool fuzzy_match(const char* haystack, const char* needle) {
    if (needle[0] == '\0') return true;
    int ni = 0;
    for (int i = 0; haystack[i]; i++) {
        if (to_lower(haystack[i]) == to_lower(needle[ni])) {
            ni++;
            if (needle[ni] == '\0') return true;
        }
    }
    return false;
}

static void pal_refilter(void) {
    pal_filtered_count = 0;
    for (int i = 0; i < (int)ALL_COUNT; i++) {
        if (fuzzy_match(all_items[i].name, pal_query)) {
            pal_filtered[pal_filtered_count++] = i;
        }
    }
    if (pal_selected >= pal_filtered_count)
        pal_selected = pal_filtered_count > 0 ? pal_filtered_count - 1 : 0;
}

/* -------------------------------------------------------------------------- */
void palette_open(void) {
    pal_query[0] = '\0';
    pal_query_len = 0;
    pal_selected = 0;
    pal_refilter();
    pal_open_flag = true;
}

void palette_close(void) { pal_open_flag = false; }
bool palette_is_open(void) { return pal_open_flag; }

/* --------------------------------------------------------------------------
 * palette_draw
 * -------------------------------------------------------------------------- */
void palette_draw(void) {
    if (!pal_open_flag) return;
    const theme_t* t = theme_get();
    uint8_t bg = t->menu_bg;
    uint8_t hi = t->menu_highlight;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, (bg >> 4) & 0x0F);
    uint8_t input_bg = VGA_COLOR(VGA_WHITE, VGA_BLACK);
    uint8_t title_col = VGA_COLOR(VGA_WHITE, (bg >> 4) & 0x0F);

    int h = 4 + (pal_filtered_count < PAL_MAX_VISIBLE ? pal_filtered_count : PAL_MAX_VISIBLE);
    gui_rect_t rect = { PAL_X, PAL_Y, PAL_WIDTH, h };
    gui_fill_rect(rect, ' ', bg);
    gui_draw_box_double(rect, bg);

    /* Title */
    gui_draw_text(PAL_X + 2, PAL_Y, " Quick Launch ", title_col);

    /* Input field */
    int iy = PAL_Y + 1;
    for (int i = PAL_X + 1; i < PAL_X + PAL_WIDTH - 1; i++)
        gui_putchar(i, iy, ' ', input_bg);
    gui_putchar(PAL_X + 2, iy, '>', VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    for (int i = 0; i < pal_query_len && i < PAL_WIDTH - 6; i++)
        gui_putchar(PAL_X + 4 + i, iy, pal_query[i], input_bg);

    /* Results */
    int ry = PAL_Y + 2;
    int vis = pal_filtered_count < PAL_MAX_VISIBLE ? pal_filtered_count : PAL_MAX_VISIBLE;
    for (int i = 0; i < vis; i++) {
        int idx = pal_filtered[i];
        bool is_sel = (i == pal_selected);
        uint8_t col = is_sel ? hi : bg;

        for (int j = PAL_X + 1; j < PAL_X + PAL_WIDTH - 1; j++)
            gui_putchar(j, ry + i, ' ', col);
        gui_putchar(PAL_X + 2, ry + i, is_sel ? '\x10' : ' ', col);
        gui_draw_text(PAL_X + 4, ry + i, all_items[idx].name, col);
    }

    /* Hint */
    gui_draw_text(PAL_X + 2, ry + vis, "Enter:run Esc:close", dim);
}

/* --------------------------------------------------------------------------
 * palette_handle_key
 * -------------------------------------------------------------------------- */
int palette_handle_key(char key) {
    if (!pal_open_flag) return 0;

    if (key == 27) { pal_open_flag = false; return 0; }

    if ((unsigned char)key == 0x80) { /* Up */
        if (pal_selected > 0) pal_selected--;
        return 0;
    }
    if ((unsigned char)key == 0x81) { /* Down */
        if (pal_selected < pal_filtered_count - 1) pal_selected++;
        return 0;
    }
    if (key == '\n') {
        if (pal_filtered_count > 0) {
            int action = all_items[pal_filtered[pal_selected]].action;
            pal_open_flag = false;
            return action;
        }
        return 0;
    }
    if (key == '\b') {
        if (pal_query_len > 0) pal_query[--pal_query_len] = '\0';
        pal_refilter();
        return 0;
    }
    if (key >= 32 && key < 127 && pal_query_len < 18) {
        pal_query[pal_query_len++] = key;
        pal_query[pal_query_len] = '\0';
        pal_refilter();
        return 0;
    }
    return 0;
}
