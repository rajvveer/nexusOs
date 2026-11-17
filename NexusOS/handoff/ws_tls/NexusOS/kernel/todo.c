/* ============================================================================
 * NexusOS — Todo App (Implementation)
 * ============================================================================ */

#include "todo.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "appui.h"

#define TODO_MAX 12
#define TODO_TEXT_MAX 28

static struct { bool used; bool done; char text[TODO_TEXT_MAX]; } todos[TODO_MAX];
static int td_sel = 0;
static char td_input[TODO_TEXT_MAX];
static int td_ilen = 0;
static bool td_adding = false;

static void td_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    appui_theme_t ui = appui_theme();

    int total = 0, completed = 0;
    for (int i = 0; i < TODO_MAX; i++) if (todos[i].used) { total++; if (todos[i].done) completed++; }

    char hdr[30]; strcpy(hdr, "Tasks ");
    char n1[4]; int_to_str(completed, n1); strcat(hdr, n1);
    strcat(hdr, "/");
    char n2[4]; int_to_str(total, n2); strcat(hdr, n2);
    strcat(hdr, " done");
    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "Todo", hdr);

    int row = cy + 3;
    int vis = 0;
    for (int i = 0; i < TODO_MAX && row < cy + ch - 3; i++) {
        if (!todos[i].used) continue;
        bool sel = (vis == td_sel && !td_adding);
        appui_row(cx, row, cw, sel, ui.panel, ui.selected);
        gui_putchar(cx + 1, row, todos[i].done ? 'x' : ' ', todos[i].done ? ui.good : ui.muted);
        uint8_t txt_col = todos[i].done ? ui.muted : (sel ? ui.selected : ui.text);
        appui_text(cx + 3, row, todos[i].text, cw - 4, txt_col);
        row++; vis++;
    }

    if (total == 0 && !td_adding) { appui_text(cx + 2, row, "No tasks yet", cw - 4, ui.muted); row++; }

    row = cy + ch - 2;
    if (td_adding) {
        appui_input(cx, row, cw, td_input, td_ilen, true);
        appui_status(cx, row + 1, cw, "Enter Save   Esc Cancel");
    } else {
        appui_status(cx, cy + ch - 1, cw, "A Add   Space Toggle   D Delete");
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
    return window_create("Todo", 20, 4, 40, 18, td_draw, td_key);
}
