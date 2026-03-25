/* ============================================================================
 * NexusOS — File Manager App (Implementation)
 * ============================================================================
 * Browse, view, create, and delete files from the RAM filesystem.
 * Runs as a desktop window with keyboard navigation.
 *
 * Keys:
 *   Up/Down   — Navigate file list
 *   Enter     — Toggle file content preview
 *   D         — Delete selected file
 *   N         — Create a new file (auto-named)
 * ============================================================================ */

#include "filemgr.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "ramfs.h"
#include "string.h"
#include "framebuffer.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* File manager state */
static int fm_selected = 0;
static int fm_scroll = 0;
static bool fm_preview = false;

/* Preview buffer */
#define FM_PREVIEW_SIZE 256
static char fm_preview_buf[FM_PREVIEW_SIZE];
static int  fm_preview_len = 0;

/* Auto-name counter for new files */
static int fm_new_file_counter = 0;

/* --------------------------------------------------------------------------
 * fm_count_files: Count files in root directory
 * -------------------------------------------------------------------------- */
static int fm_count_files(void) {
    fs_node_t* root = vfs_get_root();
    if (!root) return 0;
    int count = 0;
    while (vfs_readdir(root, count) != NULL) count++;
    return count;
}

/* --------------------------------------------------------------------------
 * fm_get_file: Get the file node at index
 * -------------------------------------------------------------------------- */
static fs_node_t* fm_get_file(int index) {
    fs_node_t* root = vfs_get_root();
    if (!root) return NULL;
    return vfs_readdir(root, index);
}

/* --------------------------------------------------------------------------
 * fm_load_preview: Load selected file content into preview buffer
 * -------------------------------------------------------------------------- */
static void fm_load_preview(void) {
    fs_node_t* node = fm_get_file(fm_selected);
    if (!node || (node->type & FS_DIRECTORY)) {
        fm_preview_buf[0] = '\0';
        fm_preview_len = 0;
        return;
    }

    uint32_t to_read = node->size;
    if (to_read > FM_PREVIEW_SIZE - 1) to_read = FM_PREVIEW_SIZE - 1;

    fm_preview_len = vfs_read(node, 0, to_read, (uint8_t*)fm_preview_buf);
    if (fm_preview_len < 0) fm_preview_len = 0;
    fm_preview_buf[fm_preview_len] = '\0';
}

/* --------------------------------------------------------------------------
 * File manager window callbacks
 * -------------------------------------------------------------------------- */
static void fm_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t text_color = t->win_content;
    uint8_t bg = (text_color >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t sel_color = t->menu_highlight;
    uint8_t dir_color = VGA_COLOR(VGA_LIGHT_BLUE, bg);

    int file_count = fm_count_files();

    /* Header */
    gui_draw_text(cx + 1, cy, "\x04 File Manager", accent);

    /* File count */
    char count_str[16];
    int_to_str(file_count, count_str);
    char header_right[20];
    strcpy(header_right, "[");
    strcat(header_right, count_str);
    strcat(header_right, " files]");
    int hr_len = strlen(header_right);
    if (cx + cw - 1 - hr_len > cx) {
        gui_draw_text(cx + cw - 1 - hr_len, cy, header_right, dim);
    }

    /* Separator */
    for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
        gui_putchar(cx + i, cy + 1, (char)0xC4, dim);

    if (fm_preview) {
        /* Preview mode */
        fs_node_t* node = fm_get_file(fm_selected);
        if (node) {
            char title[40];
            strcpy(title, "\x11 ");
            strcat(title, node->name);
            gui_draw_text(cx + 1, cy + 2, title, accent);

            /* Display content */
            int row = cy + 3;
            int col_off = 0;
            for (int i = 0; i < fm_preview_len && row < cy + ch - 1; i++) {
                if (fm_preview_buf[i] == '\n' || col_off >= cw - 2) {
                    row++;
                    col_off = 0;
                    if (fm_preview_buf[i] == '\n') continue;
                }
                gui_putchar(cx + col_off, row, fm_preview_buf[i], text_color);
                col_off++;
            }
        }

        /* Bottom bar */
        gui_draw_text(cx + 1, cy + ch - 1, "Enter:Back", dim);
    } else {
        /* File list mode */
        int visible = ch - 3;  /* header + sep + bottom bar */

        /* Adjust scroll */
        if (fm_selected < fm_scroll) fm_scroll = fm_selected;
        if (fm_selected >= fm_scroll + visible) fm_scroll = fm_selected - visible + 1;

        for (int i = 0; i < visible && (fm_scroll + i) < file_count; i++) {
            int idx = fm_scroll + i;
            int row = cy + 2 + i;
            fs_node_t* node = fm_get_file(idx);
            if (!node) break;

            bool is_selected = (idx == fm_selected);
            uint8_t line_col = is_selected ? sel_color : text_color;
            bool vesa = fb_is_vesa();

            /* Fill row background if selected */
            if (is_selected) {
                if (vesa) {
                    fb_fill_rect(cx * 8 + 4, row * 16, cw * 8 - 8, 16, FB_RGB(60, 100, 180));
                } else {
                    for (int j = cx; j < cx + cw - 1; j++)
                        gui_putchar(j, row, ' ', sel_color);
                }
            }

            /* Icon */
            if (vesa) {
                int px = (cx + 1) * 8;
                int py = row * 16 + 2;
                if (node->type & FS_DIRECTORY) {
                    /* Folder icon */
                    fb_fill_rect(px + 2, py + 2, 6, 2, FB_RGB(255, 220, 100));
                    fb_fill_rect(px, py + 4, 12, 8, FB_RGB(255, 200, 50));
                    fb_draw_rect(px, py + 4, 12, 8, FB_RGB(200, 150, 0));
                } else {
                    /* File icon */
                    fb_fill_rect(px + 2, py, 10, 14, FB_RGB(255, 255, 255));
                    fb_draw_rect(px + 2, py, 10, 14, FB_RGB(150, 150, 150));
                    fb_fill_rect(px + 9, py, 3, 3, FB_RGB(200, 200, 200)); /* Ear */
                    fb_fill_rect(px + 4, py + 4, 6, 1, FB_RGB(150, 150, 150));
                    fb_fill_rect(px + 4, py + 6, 6, 1, FB_RGB(150, 150, 150));
                    fb_fill_rect(px + 4, py + 8, 6, 1, FB_RGB(150, 150, 150));
                }
            } else {
                char icon = (node->type & FS_DIRECTORY) ? '\x10' : '\x07';
                uint8_t icon_col = is_selected ? sel_color :
                                   (node->type & FS_DIRECTORY) ? dir_color : dim;
                gui_putchar(cx + 1, row, icon, icon_col);
            }

            /* Filename */
            int name_max = cw - 12;
            int j = 0;
            while (node->name[j] && j < name_max) {
                gui_putchar(cx + 3 + j, row, node->name[j], line_col);
                j++;
            }

            /* Size */
            if (!(node->type & FS_DIRECTORY)) {
                char size_str[10];
                int_to_str(node->size, size_str);
                int sl = strlen(size_str);
                int sx = cx + cw - 2 - sl - 1;
                if (sx > cx + 3 + j) {
                    gui_draw_text(sx, row, size_str, is_selected ? sel_color : dim);
                    gui_putchar(sx + sl, row, 'B', is_selected ? sel_color : dim);
                }
            }
        }

        /* Bottom bar */
        gui_draw_text(cx + 1, cy + ch - 1, "N:New D:Del Enter:View", dim);
    }
}

static void fm_key(int id, char key) {
    (void)id;
    int file_count = fm_count_files();

    if (fm_preview) {
        /* In preview mode, Enter goes back */
        if (key == '\n' || key == 27 || key == '\b') {
            fm_preview = false;
        }
        return;
    }

    /* Up arrow */
    if ((unsigned char)key == 0x80) {
        if (fm_selected > 0) fm_selected--;
        return;
    }

    /* Down arrow */
    if ((unsigned char)key == 0x81) {
        if (fm_selected < file_count - 1) fm_selected++;
        return;
    }

    /* Enter — preview */
    if (key == '\n') {
        if (file_count > 0) {
            fm_load_preview();
            fm_preview = true;
        }
        return;
    }

    /* D — delete */
    if (key == 'd' || key == 'D') {
        if (file_count > 0) {
            fs_node_t* node = fm_get_file(fm_selected);
            if (node) {
                ramfs_delete(node->name);
                if (fm_selected > 0 && fm_selected >= fm_count_files()) {
                    fm_selected--;
                }
            }
        }
        return;
    }

    /* N — create new file */
    if (key == 'n' || key == 'N') {
        char name[FS_NAME_MAX];
        strcpy(name, "file");
        char num[8];
        int_to_str(fm_new_file_counter++, num);
        strcat(name, num);
        strcat(name, ".txt");
        ramfs_create(name, FS_FILE);
        return;
    }
}

/* --------------------------------------------------------------------------
 * filemgr_open: Create a file manager window
 * -------------------------------------------------------------------------- */
int filemgr_open(void) {
    fm_selected = 0;
    fm_scroll = 0;
    fm_preview = false;
    fm_preview_buf[0] = '\0';
    fm_preview_len = 0;

    return window_create("File Manager", 8, 2, 36, 18, fm_draw, fm_key);
}
