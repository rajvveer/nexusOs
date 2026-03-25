/* ============================================================================
 * NexusOS — Todo App (Implementation)
 * ============================================================================ */

#include "todo.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

#define TODO_MAX 12
#define TODO_TEXT_MAX 28

static struct { bool used; bool done; char text[TODO_TEXT_MAX]; } todos[TODO_MAX];
static int td_sel = 0;
static char td_input[TODO_TEXT_MAX];
static int td_ilen = 0;
static bool td_adding = false;

static void td_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t hi = t->menu_highlight;
    uint8_t done_col = VGA_COLOR(VGA_LIGHT_GREEN, bg);

    int row = cy;
    int total = 0, completed = 0;
    for (int i = 0; i < TODO_MAX; i++) if (todos[i].used) { total++; if (todos[i].done) completed++; }

    char hdr[30]; strcpy(hdr, "\xFB Todo [");
    char n1[4]; int_to_str(completed, n1); strcat(hdr, n1);
    strcat(hdr, "/");
    char n2[4]; int_to_str(total, n2); strcat(hdr, n2);
    strcat(hdr, "]");
    gui_draw_text(cx, row, hdr, accent); row++;
    for (int i = 0; i < cw - 1; i++) gui_putchar(cx + i, row, (char)0xC4, dim); row++;

    int vis = 0;
    for (int i = 0; i < TODO_MAX && row < cy + ch - 3; i++) {
        if (!todos[i].used) continue;
        bool sel = (vis == td_sel && !td_adding);
        uint8_t col = sel ? hi : tc;
        if (sel) for (int j = cx; j < cx + cw - 1; j++) gui_putchar(j, row, ' ', hi);
        gui_putchar(cx + 1, row, todos[i].done ? '\xFB' : '\xFA', todos[i].done ? done_col : dim);
        uint8_t txt_col = todos[i].done ? VGA_COLOR(VGA_DARK_GREY, sel ? ((hi>>4)&0xF) : bg) : col;
        gui_draw_text(cx + 3, row, todos[i].text, txt_col);
        row++; vis++;
    }

    if (total == 0 && !td_adding) { gui_draw_text(cx + 2, row, "No tasks. A:Add", dim); row++; }

    row = cy + ch - 2;
    if (td_adding) {
        gui_draw_text(cx, row, "> ", accent);
        for (int i = 0; i < td_ilen; i++) gui_putchar(cx + 2 + i, row, td_input[i], tc);
        gui_putchar(cx + 2 + td_ilen, row, '_', VGA_COLOR(VGA_WHITE, bg));
        row++;
        gui_draw_text(cx, row, "Enter:save Esc:cancel", dim);
    } else {
        gui_draw_text(cx, row, "A:Add Spc:Toggle D:Del", dim);
    }
    (void)cw; (void)ch;
}

static void td_key(int id, char key) {
    (void)id;
    if (td_adding) {
        if (key == 27) { td_adding = false; td_ilen = 0; }
        else if (key == '\n' && td_ilen > 0) {
            td_input[td_ilen] = '\0';
            for (int i = 0; i < TODO_MAX; i++) {
                if (!todos[i].used) {
                    todos[i].used = true; todos[i].done = false;
                    strncpy(todos[i].text, td_input, TODO_TEXT_MAX - 1);
                    todos[i].text[TODO_TEXT_MAX - 1] = '\0'; break;
                }
            }
            td_adding = false; td_ilen = 0;
        }
        else if (key == '\b' && td_ilen > 0) td_input[--td_ilen] = '\0';
        else if (key >= 32 && key < 127 && td_ilen < TODO_TEXT_MAX - 1) { td_input[td_ilen++] = key; td_input[td_ilen] = '\0'; }
        return;
    }
    if (key == 'a' || key == 'A') { td_adding = true; td_ilen = 0; td_input[0] = '\0'; }
    if ((unsigned char)key == 0x80 && td_sel > 0) td_sel--;
    if ((unsigned char)key == 0x81) td_sel++;
    if (key == ' ') {
        int vis = 0;
        for (int i = 0; i < TODO_MAX; i++) {
            if (!todos[i].used) continue;
            if (vis == td_sel) { todos[i].done = !todos[i].done; break; } vis++;
        }
    }
    if (key == 'd' || key == 'D') {
        int vis = 0;
        for (int i = 0; i < TODO_MAX; i++) {
            if (!todos[i].used) continue;
            if (vis == td_sel) { todos[i].used = false; break; } vis++;
        }
    }
}

int todo_open(void) {
    td_sel = 0; td_adding = false; td_ilen = 0;
    return window_create("Todo", 20, 2, 32, 16, td_draw, td_key);
}
