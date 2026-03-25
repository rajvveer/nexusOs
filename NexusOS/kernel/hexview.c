/* ============================================================================
 * NexusOS — Hex Viewer (Implementation)
 * ============================================================================
 * View any VFS file as hex + ASCII, 8 bytes per row.
 * ============================================================================ */

#include "hexview.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "vfs.h"

#define HV_BUF 512
static uint8_t hv_data[HV_BUF];
static uint32_t hv_size = 0;
static int hv_scroll = 0;
static char hv_fname[32];

static const char hex_chars[] = "0123456789ABCDEF";

static void hv_load_first_file(void) {
    fs_node_t* root = vfs_get_root();
    if (!root) return;
    fs_node_t* f = vfs_readdir(root, 0);
    if (!f) { hv_size = 0; strcpy(hv_fname, "(none)"); return; }
    strncpy(hv_fname, f->name, 31); hv_fname[31] = '\0';
    f = vfs_finddir(root, hv_fname);
    if (f) { hv_size = vfs_read(f, 0, HV_BUF, hv_data); }
    else hv_size = 0;
}

static void hv_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t addr_col = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t hex_col = VGA_COLOR(VGA_YELLOW, bg);
    uint8_t asc_col = VGA_COLOR(VGA_LIGHT_GREEN, bg);

    int row = cy;
    char hdr[40]; strcpy(hdr, "\xE8 "); strcat(hdr, hv_fname);
    gui_draw_text(cx, row, hdr, VGA_COLOR(VGA_LIGHT_CYAN, bg));
    row++;

    /* Column header */
    gui_draw_text(cx, row, "ADDR", dim);
    gui_draw_text(cx + 6, row, "00 01 02 03 04 05 06 07", dim);
    gui_draw_text(cx + 30, row, "ASCII", dim);
    row++;

    int bytes_per_row = 8;
    int start = hv_scroll * bytes_per_row;
    for (int r = 0; row < cy + ch - 1 && (uint32_t)start < hv_size; r++) {
        /* Address */
        char addr[6];
        addr[0] = hex_chars[(start >> 8) & 0xF];
        addr[1] = hex_chars[(start >> 4) & 0xF];
        addr[2] = hex_chars[start & 0xF];
        addr[3] = '\0';
        gui_draw_text(cx, row, addr, addr_col);

        /* Hex */
        for (int i = 0; i < bytes_per_row && (uint32_t)(start + i) < hv_size; i++) {
            uint8_t b = hv_data[start + i];
            gui_putchar(cx + 6 + i * 3, row, hex_chars[b >> 4], hex_col);
            gui_putchar(cx + 7 + i * 3, row, hex_chars[b & 0xF], hex_col);
        }

        /* ASCII */
        for (int i = 0; i < bytes_per_row && (uint32_t)(start + i) < hv_size; i++) {
            uint8_t b = hv_data[start + i];
            char c = (b >= 32 && b < 127) ? (char)b : '.';
            gui_putchar(cx + 30 + i, row, c, asc_col);
        }

        start += bytes_per_row;
        row++;
    }

    char info[30]; strcpy(info, "Size:");
    char n[8]; int_to_str(hv_size, n); strcat(info, n); strcat(info, "B");
    gui_draw_text(cx, cy + ch - 1, info, dim);
    (void)cw;
}

static void hv_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80 && hv_scroll > 0) hv_scroll--;
    if ((unsigned char)key == 0x81) hv_scroll++;
    /* N key: load next file */
    if (key == 'n' || key == 'N') {
        fs_node_t* root = vfs_get_root();
        if (!root) return;
        bool found_current = false;
        for (uint32_t i = 0; ; i++) {
            fs_node_t* f = vfs_readdir(root, i);
            if (!f) { hv_load_first_file(); hv_scroll = 0; return; }
            if (found_current) {
                strncpy(hv_fname, f->name, 31); hv_fname[31] = '\0';
                f = vfs_finddir(root, hv_fname);
                if (f) hv_size = vfs_read(f, 0, HV_BUF, hv_data); else hv_size = 0;
                hv_scroll = 0; return;
            }
            if (strcmp(f->name, hv_fname) == 0) found_current = true;
        }
    }
}

int hexview_open(void) {
    hv_scroll = 0;
    hv_load_first_file();
    return window_create("Hex Viewer", 10, 2, 40, 18, hv_draw, hv_key);
}
