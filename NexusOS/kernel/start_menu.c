/* ============================================================================
 * NexusOS — Start Menu (Two-Column Layout)
 * ============================================================================
 * Wide popup menu with two columns so all 28 apps fit without clipping.
 * ============================================================================ */

#include "start_menu.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

typedef struct {
    const char* label;
    int action;
    char icon;
    uint8_t icon_color;
} menu_item_t;

static const menu_item_t items[] = {
    /* Column 1 (left) */
    { "Terminal",      SMENU_TERMINAL, '\xC4', VGA_LIGHT_GREEN   },
    { "File Manager",  SMENU_FILEMGR,  '\xE8', VGA_YELLOW        },
    { "Calculator",    SMENU_CALC,     '\xF1', VGA_LIGHT_CYAN    },
    { "Sys Monitor",   SMENU_SYSMON,   '\x0F', VGA_LIGHT_RED     },
    { "Settings",      SMENU_SETTINGS, '\xF0', VGA_LIGHT_MAGENTA },
    { "Notepad",       SMENU_NOTEPAD,  '\xE9', VGA_WHITE         },
    { "Task Manager",  SMENU_TASKMGR,  '\xFE', VGA_LIGHT_BLUE    },
    { "Calendar",      SMENU_CALENDAR, '\x04', VGA_LIGHT_CYAN    },
    { "Music Player",  SMENU_MUSIC,    '\x0E', VGA_YELLOW        },
    { "Help",          SMENU_HELP,     '\x3F', VGA_WHITE         },
    { "Paint",         SMENU_PAINT,    '\xEB', VGA_LIGHT_GREEN   },
    { "Minesweeper",   SMENU_MINESWEEP,'\x0F', VGA_LIGHT_CYAN    },
    { "Recycle Bin",   SMENU_RECYCLE,  '\xE8', VGA_YELLOW        },
    { "System Info",   SMENU_SYSINFO,  '\x69', VGA_LIGHT_BLUE    },
    /* Column 2 (right) */
    { "Todo",          SMENU_TODO,     '\xFB', VGA_LIGHT_GREEN   },
    { "Clock",         SMENU_CLOCK,    '\x0F', VGA_YELLOW        },
    { "Pong",          SMENU_PONG,     '\x04', VGA_LIGHT_CYAN    },
    { "Search",        SMENU_SEARCH,   '\x10', VGA_WHITE         },
    { "Tetris",        SMENU_TETRIS,   '\xDB', VGA_LIGHT_CYAN    },
    { "Hex Viewer",    SMENU_HEXVIEW,  '\xFE', VGA_YELLOW        },
    { "Contacts",      SMENU_CONTACTS, '\x02', VGA_LIGHT_GREEN   },
    { "Color Picker",  SMENU_COLORPICK,'\xDB', VGA_LIGHT_MAGENTA },
    { "System Log",    SMENU_SYSLOG,   '\xFE', VGA_LIGHT_BLUE    },
    { "Clipboard",     SMENU_CLIPMGR,  '\xE8', VGA_LIGHT_CYAN    },
    { "Appearance",    SMENU_APPEAR,   '\xFE', VGA_LIGHT_MAGENTA },
    { "File Ops",      SMENU_FILEOPS,  '\xE8', VGA_YELLOW        },
    { "Snake Game",    SMENU_SNAKE,    '\x02', VGA_LIGHT_GREEN   },
    { "Web Browser",   SMENU_BROWSER,  '\xEB', VGA_LIGHT_CYAN    },
};

#define ITEM_COUNT ((int)(sizeof(items) / sizeof(items[0])))
#define COL_WIDTH  18   /* Width of each column */
#define ITEMS_PER_COL 14 /* Max items per column */

static bool menu_open = false;
static int  selected = 0;

void start_menu_toggle(void) { menu_open = !menu_open; if (menu_open) selected = 0; }
void start_menu_close(void) { menu_open = false; }
bool start_menu_is_open(void) { return menu_open; }

void start_menu_draw(void) {
    if (!menu_open) return;

    const theme_t* t = theme_get();
    uint8_t bg = t->menu_bg;
    uint8_t hi = t->menu_highlight;
    uint8_t bg_nibble = (bg >> 4) & 0x0F;

    int x = SMENU_X;
    int y = SMENU_Y;
    int w = SMENU_WIDTH;
    int h = SMENU_HEIGHT;

    /* Fill + border */
    gui_rect_t rect = { x, y, w, h };
    gui_fill_rect(rect, ' ', bg);
    gui_draw_box_double(rect, bg);

    /* Title bar */
    uint8_t title_col = VGA_COLOR(VGA_LIGHT_CYAN, bg_nibble);
    gui_putchar(x + 2, y, '\x0F', title_col);
    gui_draw_text(x + 4, y, "NexusOS", VGA_COLOR(VGA_WHITE, bg_nibble));
    gui_draw_text(x + 12, y, " v2.5", VGA_COLOR(VGA_DARK_GREY, bg_nibble));

    /* Separator under title */
    for (int i = x + 1; i < x + w - 1; i++)
        gui_putchar(i, y + 1, (char)0xC4, VGA_COLOR(VGA_DARK_GREY, bg_nibble));

    /* Column divider */
    int mid = x + COL_WIDTH + 2;
    for (int row = y + 2; row < y + h - 1; row++)
        gui_putchar(mid, row, (char)0xB3, VGA_COLOR(VGA_DARK_GREY, bg_nibble));

    /* Draw items in two columns */
    for (int i = 0; i < ITEM_COUNT; i++) {
        int col = (i < ITEMS_PER_COL) ? 0 : 1;
        int row_idx = (col == 0) ? i : i - ITEMS_PER_COL;
        int cx = (col == 0) ? x + 1 : mid + 1;
        int row = y + 2 + row_idx;

        if (row >= y + h - 1) break;

        bool is_sel = (i == selected);
        uint8_t item_bg = is_sel ? hi : bg;
        uint8_t h_nibble = (item_bg >> 4) & 0x0F;

        /* Fill row within column */
        int col_end = (col == 0) ? mid : x + w - 1;
        for (int j = cx; j < col_end; j++)
            gui_putchar(j, row, ' ', item_bg);

        /* Icon */
        gui_putchar(cx + 1, row, items[i].icon,
            VGA_COLOR(items[i].icon_color, h_nibble));

        /* Label */
        gui_draw_text(cx + 3, row, items[i].label, item_bg);

        /* Selection marker */
        if (is_sel)
            gui_putchar(cx, row, '\x10', VGA_COLOR(VGA_LIGHT_CYAN, h_nibble));
    }

    /* Bottom: shortcut hints */
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg_nibble);
    gui_draw_text(x + 2, y + h - 1, "Arrows:Nav  Enter:Open", dim);
}

int start_menu_handle_key(char key) {
    if (!menu_open) return SMENU_NONE;

    /* Up arrow */
    if ((unsigned char)key == 0x80) {
        if (selected > 0) selected--;
        return SMENU_NONE;
    }
    /* Down arrow */
    if ((unsigned char)key == 0x81) {
        if (selected < ITEM_COUNT - 1) selected++;
        return SMENU_NONE;
    }
    /* Right arrow — jump to second column */
    if ((unsigned char)key == 0x83) {
        if (selected < ITEMS_PER_COL && selected + ITEMS_PER_COL < ITEM_COUNT)
            selected += ITEMS_PER_COL;
        return SMENU_NONE;
    }
    /* Left arrow — jump to first column */
    if ((unsigned char)key == 0x82) {
        if (selected >= ITEMS_PER_COL)
            selected -= ITEMS_PER_COL;
        return SMENU_NONE;
    }
    /* Enter */
    if (key == '\n') {
        int a = items[selected].action;
        menu_open = false;
        return a;
    }
    /* Escape */
    if (key == 27) {
        menu_open = false;
        return SMENU_NONE;
    }
    return SMENU_NONE;
}

bool start_menu_hit(int mx, int my) {
    if (!menu_open) return false;
    return (mx >= SMENU_X && mx < SMENU_X + SMENU_WIDTH &&
            my >= SMENU_Y && my < SMENU_Y + SMENU_HEIGHT);
}

int start_menu_handle_click(int mx, int my) {
    if (!menu_open) return SMENU_NONE;
    if (!start_menu_hit(mx, my)) { menu_open = false; return SMENU_NONE; }

    int row_offset = my - SMENU_Y - 2;  /* Skip title + separator */
    if (row_offset < 0 || row_offset >= ITEMS_PER_COL) return SMENU_NONE;

    /* Determine which column was clicked */
    int mid = SMENU_X + COL_WIDTH + 2;
    int item_idx;
    if (mx < mid) {
        item_idx = row_offset;  /* Left column */
    } else {
        item_idx = row_offset + ITEMS_PER_COL;  /* Right column */
    }

    if (item_idx >= 0 && item_idx < ITEM_COUNT) {
        int action = items[item_idx].action;
        menu_open = false;
        return action;
    }
    return SMENU_NONE;
}
