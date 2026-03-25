/* ============================================================================
 * NexusOS — Recycle Bin (Implementation)
 * ============================================================================
 * Holds up to 8 deleted files. Supports restore and empty.
 * ============================================================================ */

#include "recycle.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "ramfs.h"

typedef struct {
    bool    used;
    char    name[TRASH_NAME_MAX];
    uint8_t data[TRASH_DATA_MAX];
    uint32_t size;
} trash_item_t;

static trash_item_t trash[TRASH_MAX];
static int rb_selected = 0;

void recycle_init(void) {
    for (int i = 0; i < TRASH_MAX; i++) trash[i].used = false;
    rb_selected = 0;
}

bool recycle_delete(const char* name, const uint8_t* data, uint32_t size) {
    for (int i = 0; i < TRASH_MAX; i++) {
        if (!trash[i].used) {
            trash[i].used = true;
            strncpy(trash[i].name, name, TRASH_NAME_MAX - 1);
            trash[i].name[TRASH_NAME_MAX - 1] = '\0';
            uint32_t copy = size < TRASH_DATA_MAX ? size : TRASH_DATA_MAX;
            for (uint32_t j = 0; j < copy; j++) trash[i].data[j] = data[j];
            trash[i].size = copy;
            return true;
        }
    }
    return false; /* Trash full */
}

bool recycle_restore(int index) {
    if (index < 0 || index >= TRASH_MAX || !trash[index].used) return false;
    /* Recreate the file in ramfs */
    ramfs_create(trash[index].name, FS_FILE);
    fs_node_t* root = vfs_get_root();
    fs_node_t* f = vfs_finddir(root, trash[index].name);
    if (f) vfs_write(f, 0, trash[index].size, trash[index].data);
    trash[index].used = false;
    return true;
}

void recycle_empty(void) {
    for (int i = 0; i < TRASH_MAX; i++) trash[i].used = false;
}

int recycle_count(void) {
    int c = 0;
    for (int i = 0; i < TRASH_MAX; i++) if (trash[i].used) c++;
    return c;
}

const char* recycle_get_name(int index) {
    if (index < 0 || index >= TRASH_MAX || !trash[index].used) return NULL;
    return trash[index].name;
}

static void rb_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t hi = t->menu_highlight;
    uint8_t warn = VGA_COLOR(VGA_YELLOW, bg);

    int row = cy;
    char title[24]; strcpy(title, "\xE8 Recycle Bin [");
    char cn[4]; int_to_str(recycle_count(), cn);
    strcat(title, cn); strcat(title, "]");
    gui_draw_text(cx, row, title, accent);
    row += 2;

    int vis = 0;
    for (int i = 0; i < TRASH_MAX && row < cy + ch - 2; i++) {
        if (!trash[i].used) continue;
        bool sel = (vis == rb_selected);
        uint8_t col = sel ? hi : tc;
        if (sel) for (int j = cx; j < cx + cw - 1; j++) gui_putchar(j, row, ' ', hi);
        gui_putchar(cx + 1, row, '\xE8', warn);
        gui_draw_text(cx + 3, row, trash[i].name, col);

        char sz[10]; int_to_str(trash[i].size, sz);
        strcat(sz, "B");
        gui_draw_text(cx + cw - 6, row, sz, sel ? col : dim);
        row++; vis++;
    }

    if (vis == 0) {
        gui_draw_text(cx + 2, row, "Trash is empty", dim);
    }

    gui_draw_text(cx, cy + ch - 1, "R:Restore E:Empty", dim);
    (void)cw; (void)ch;
}

static void rb_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80 && rb_selected > 0) rb_selected--;
    if ((unsigned char)key == 0x81) rb_selected++;

    if (key == 'r' || key == 'R') {
        int vis = 0;
        for (int i = 0; i < TRASH_MAX; i++) {
            if (!trash[i].used) continue;
            if (vis == rb_selected) { recycle_restore(i); break; }
            vis++;
        }
    }
    if (key == 'e' || key == 'E') recycle_empty();
}

int recycle_open(void) {
    rb_selected = 0;
    return window_create("Recycle Bin", 20, 3, 30, 14, rb_draw, rb_key);
}
