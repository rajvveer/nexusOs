/* ============================================================================
 * NexusOS — File Operations (Implementation)
 * ============================================================================ */

#include "fileops.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "vfs.h"
#include "ramfs.h"

#define FO_NAME_MAX 28

static int fo_sel = 0;
static char fo_files[8][32];
static int fo_count = 0;
static bool fo_renaming = false;
static char fo_buf[FO_NAME_MAX];
static int fo_blen = 0;

static void fo_refresh(void) {
    fo_count = 0;
    fs_node_t* root = vfs_get_root();
    if (!root) return;
    for (uint32_t i = 0; fo_count < 8; i++) {
        fs_node_t* f = vfs_readdir(root, i);
        if (!f) break;
        strncpy(fo_files[fo_count], f->name, 31);
        fo_files[fo_count][31] = '\0';
        fo_count++;
    }
}

int fileops_rename(const char* old_name, const char* new_name) {
    fs_node_t* root = vfs_get_root();
    if (!root) return -1;
    fs_node_t* f = vfs_finddir(root, old_name);
    if (!f) return -1;
    /* Read data, delete old, create new */
    uint8_t buf[512];
    uint32_t sz = vfs_read(f, 0, 512, buf);
    ramfs_delete(old_name);
    fs_node_t* nf = ramfs_create(new_name, FS_FILE);
    if (nf && sz > 0) vfs_write(nf, 0, sz, buf);
    return 0;
}

int fileops_duplicate(const char* name) {
    fs_node_t* root = vfs_get_root();
    if (!root) return -1;
    fs_node_t* f = vfs_finddir(root, name);
    if (!f) return -1;
    uint8_t buf[512];
    uint32_t sz = vfs_read(f, 0, 512, buf);
    char dup[32]; strcpy(dup, "copy_"); strcat(dup, name);
    if (strlen(dup) > 28) dup[28] = '\0';
    fs_node_t* nf = ramfs_create(dup, FS_FILE);
    if (nf && sz > 0) vfs_write(nf, 0, sz, buf);
    return 0;
}

static void fo_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t hi = t->menu_highlight;

    int row = cy;
    gui_draw_text(cx, row, "\xE8 File Operations", accent); row++;
    for (int i = 0; i < cw - 1; i++) gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    if (fo_renaming) {
        gui_draw_text(cx, row, "New name:", accent); row++;
        for (int i = 0; i < fo_blen; i++) gui_putchar(cx + 1 + i, row, fo_buf[i], tc);
        gui_putchar(cx + 1 + fo_blen, row, '_', VGA_COLOR(VGA_WHITE, bg)); row++;
        gui_draw_text(cx, row + 1, "Enter:save Esc:cancel", dim);
    } else {
        for (int i = 0; i < fo_count && row < cy + ch - 2; i++) {
            bool sel = (i == fo_sel);
            uint8_t col = sel ? hi : tc;
            if (sel) for (int j = cx; j < cx + cw - 1; j++) gui_putchar(j, row, ' ', hi);
            gui_putchar(cx + 1, row, '\xE8', VGA_COLOR(VGA_YELLOW, sel ? ((hi >> 4) & 0xF) : bg));
            gui_draw_text(cx + 3, row, fo_files[i], col);
            row++;
        }
        if (fo_count == 0) gui_draw_text(cx + 2, row, "No files", dim);
        gui_draw_text(cx, cy + ch - 1, "R:Rename D:Dup X:Del", dim);
    }
    (void)cw; (void)ch;
}

static void fo_key(int id, char key) {
    (void)id;
    if (fo_renaming) {
        if (key == 27) { fo_renaming = false; fo_blen = 0; }
        else if (key == '\n' && fo_blen > 0) {
            fo_buf[fo_blen] = '\0';
            fileops_rename(fo_files[fo_sel], fo_buf);
            fo_renaming = false; fo_blen = 0;
            fo_refresh();
        }
        else if (key == '\b' && fo_blen > 0) fo_buf[--fo_blen] = '\0';
        else if (key >= 32 && key < 127 && fo_blen < FO_NAME_MAX - 1) { fo_buf[fo_blen++] = key; fo_buf[fo_blen] = '\0'; }
        return;
    }
    if ((unsigned char)key == 0x80 && fo_sel > 0) fo_sel--;
    if ((unsigned char)key == 0x81 && fo_sel < fo_count - 1) fo_sel++;
    if ((key == 'r' || key == 'R') && fo_sel < fo_count) {
        fo_renaming = true; fo_blen = 0; fo_buf[0] = '\0';
    }
    if ((key == 'd' || key == 'D') && fo_sel < fo_count) {
        fileops_duplicate(fo_files[fo_sel]); fo_refresh();
    }
    if ((key == 'x' || key == 'X') && fo_sel < fo_count) {
        ramfs_delete(fo_files[fo_sel]); fo_refresh();
        if (fo_sel >= fo_count && fo_sel > 0) fo_sel--;
    }
}

int fileops_open(void) {
    fo_sel = 0; fo_renaming = false; fo_blen = 0;
    fo_refresh();
    return window_create("File Ops", 18, 2, 34, 14, fo_draw, fo_key);
}
