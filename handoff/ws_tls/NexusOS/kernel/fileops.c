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
#include "appui.h"

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

/* Rename in place: no copy, so the data can't be lost or truncated, and an
 * existing target is refused rather than clobbered. */
int fileops_rename(const char* old_name, const char* new_name) {
    return ramfs_rename(old_name, new_name);
}

int fileops_duplicate(const char* name) {
    fs_node_t* root = vfs_get_root();
    fs_node_t* f = root ? vfs_finddir(root, name) : NULL;
    if (!f || f->type != FS_FILE) return -1;
    uint8_t buf[512];
    int32_t n = vfs_read(f, 0, sizeof(buf), buf);
    if (n < 0) return -1;                       /* unreadable: make no copy */
    char dup[FO_NAME_MAX + 1];                  /* "copy_" + name, <= 28 chars */
    strcpy(dup, "copy_");
    strncpy(dup + 5, name, FO_NAME_MAX - 5);
    dup[FO_NAME_MAX] = '\0';
    fs_node_t* nf = ramfs_create(dup, FS_FILE);
    if (!nf) return -1;
    /* Chunked copy: works for any file size with a fixed stack buffer. */
    uint32_t off = 0;
    while (n > 0) {
        if (vfs_write(nf, off, (uint32_t)n, buf) != n) break;
        off += (uint32_t)n;
        n = vfs_read(f, off, sizeof(buf), buf);
    }
    if (n != 0) { ramfs_delete(dup); return -1; }   /* no partial copies */
    return 0;
}

static void fo_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    appui_theme_t ui = appui_theme();

    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "File Operations", "Rename, duplicate, or delete files");
    int row = cy + 3;

    if (fo_renaming) {
        appui_text(cx + 1, row, "New name", cw - 2, ui.accent); row += 2;
        appui_input(cx + 1, row, cw - 2, fo_buf, fo_blen, true);
        appui_status(cx, cy + ch - 1, cw, "Enter Save   Esc Cancel");
    } else {
        for (int i = 0; i < fo_count && row < cy + ch - 2; i++) {
            bool sel = (i == fo_sel);
            uint8_t col = sel ? ui.selected : ui.text;
            appui_row(cx, row, cw, sel, ui.panel, ui.selected);
            gui_putchar(cx + 1, row, '*', sel ? ui.selected : ui.warn);
            appui_text(cx + 3, row, fo_files[i], cw - 4, col);
            row++;
        }
        if (fo_count == 0) appui_text(cx + 2, row, "No files", cw - 4, ui.muted);
        appui_status(cx, cy + ch - 1, cw, "R Rename   D Duplicate   X Delete");
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
    return window_create("File Ops", 18, 4, 44, 18, fo_draw, fo_key);
}
