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
#include "fat32.h"
#include "users.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* File manager state */
static int fm_selected = 0;
static int fm_scroll = 0;
static bool fm_preview = false;
static bool fm_disk;
static fs_node_t* fm_dir;
static fat32_file_t fm_fatdir, fm_fatentry;
static char fm_path[FAT32_PATH_MAX] = "/";
static fs_node_t fm_synthetic;
static char fm_notice[96];

static bool fm_set_path(const char* path) {
    bool disk = !strncmp(path, "/disk", 5) && (!path[5] || path[5] == '/');
    if (strlen(path) >= sizeof(fm_path)) return false;
    if (disk) {
        if (!fat32_lookup(path + 5, &fm_fatdir) || !fm_fatdir.is_dir) return false;
    } else {
        fs_node_t* dir = ramfs_resolve(path, NULL);
        if (!dir || dir->type != FS_DIRECTORY || !vfs_check_perm(dir, users_current_uid(), ACC_READ | ACC_EXEC)) return false;
        fm_dir = dir;
    }
    fm_disk = disk;
    if (path != fm_path) strcpy(fm_path, path);
    fm_selected = fm_scroll = 0; fm_preview = false;
    fm_notice[0] = 0;
    return true;
}
static bool fm_child_path(const char* name, char* path) {
    int len = (int)strlen(fm_path);
    if (len + strlen(name) + 2 > FAT32_PATH_MAX) return false;
    strcpy(path, fm_path);
    if (len && path[len - 1] != '/') strcat(path, "/");
    strcat(path, name); return true;
}

/* Preview buffer */
#define FM_PREVIEW_SIZE 256
static char fm_preview_buf[FM_PREVIEW_SIZE];
static int  fm_preview_len = 0;

/* Auto-name counter for new files */
static int fm_new_file_counter = 0;

static void fm_text_clip(int x, int y, const char* text, int max, uint8_t color) {
    if (max <= 0) return;
    for (int i = 0; text[i] && i < max; i++)
        gui_putchar(x + i, y, text[i], color);
}

static void fm_clear_row(int x, int y, int w, uint8_t color) {
    if (w <= 0) return;
    for (int i = 0; i < w; i++)
        gui_putchar(x + i, y, ' ', color);
}

static void fm_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) fm_clear_row(x, y + r, w, color);
}

static void fm_draw_vline(int x, int y, int h, uint8_t color) {
    for (int i = 0; i < h; i++) gui_putchar(x, y + i, (char)0xB3, color);
}

static void fm_format_size(uint32_t size, char* out) {
    char n[12];
    if (size >= 1024) {
        int_to_str(size / 1024, n);
        strcpy(out, n);
        strcat(out, " KB");
    } else {
        int_to_str(size, n);
        strcpy(out, n);
        strcat(out, " B");
    }
}

/* --------------------------------------------------------------------------
 * fm_count_files: Count files in root directory
 * -------------------------------------------------------------------------- */
static int fm_count_files(void) {
    if (fm_disk) {
        fat32_file_t file; int n = 0;
        while (fat32_dir_entry(&fm_fatdir, n, &file)) n++;
        return n + 1;
    }
    if (!fm_dir) fm_dir = vfs_get_root();
    int count = 0;
    while (vfs_readdir(fm_dir, count) != NULL) count++;
    if (fm_dir->parent || fat32_is_mounted()) count++;
    return count;
}

/* --------------------------------------------------------------------------
 * fm_get_file: Get the file node at index
 * -------------------------------------------------------------------------- */
static fs_node_t* fm_get_file(int index) {
    if (fm_disk) {
        memset(&fm_synthetic, 0, sizeof(fm_synthetic));
        if (!index) { strcpy(fm_synthetic.name, ".."); fm_synthetic.type = FS_DIRECTORY; return &fm_synthetic; }
        if (!fat32_dir_entry(&fm_fatdir, (uint32_t)index - 1, &fm_fatentry)) return NULL;
        strncpy(fm_synthetic.name, fm_fatentry.name, FS_NAME_MAX - 1);
        fm_synthetic.size = fm_fatentry.size;
        fm_synthetic.type = fm_fatentry.is_dir ? FS_DIRECTORY : FS_FILE;
        return &fm_synthetic;
    }
    if (!fm_dir) fm_dir = vfs_get_root();
    if (fm_dir->parent) {
        if (!index) { memset(&fm_synthetic, 0, sizeof(fm_synthetic)); strcpy(fm_synthetic.name, ".."); fm_synthetic.type = FS_DIRECTORY; return &fm_synthetic; }
        index--;
    }
    fs_node_t* node = vfs_readdir(fm_dir, index);
    if (!node && !fm_dir->parent && fat32_is_mounted()) {
        memset(&fm_synthetic, 0, sizeof(fm_synthetic)); strcpy(fm_synthetic.name, "disk"); fm_synthetic.type = FS_DIRECTORY; return &fm_synthetic;
    }
    return node;
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
    if (!fm_disk && !vfs_check_perm(node, users_current_uid(), ACC_READ)) {
        strcpy(fm_preview_buf, "Permission denied"); fm_preview_len = (int)strlen(fm_preview_buf); return;
    }

    uint32_t to_read = node->size;
    if (to_read > FM_PREVIEW_SIZE - 1) to_read = FM_PREVIEW_SIZE - 1;

    fm_preview_len = fm_disk ? fat32_read_file(&fm_fatentry, 0, to_read, fm_preview_buf) : vfs_read(node, 0, to_read, (uint8_t*)fm_preview_buf);
    if (fm_preview_len < 0) fm_preview_len = 0;
    fm_preview_buf[fm_preview_len] = '\0';
}

/* --------------------------------------------------------------------------
 * File manager window callbacks
 * -------------------------------------------------------------------------- */
static void fm_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t bg = VGA_BLACK;
    uint8_t panel = VGA_COLOR(VGA_LIGHT_GREY, bg);
    uint8_t sidebar = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLUE);
    uint8_t toolbar = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY);
    uint8_t text_color = VGA_COLOR(VGA_WHITE, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t muted = VGA_COLOR(VGA_LIGHT_GREY, bg);
    uint8_t sel_color = t->menu_highlight;
    uint8_t dir_color = VGA_COLOR(VGA_LIGHT_BLUE, bg);
    uint8_t status_col = VGA_COLOR(VGA_LIGHT_GREY, VGA_DARK_GREY);

    int file_count = fm_count_files();
    bool vesa = fb_is_vesa();

    fm_fill_rect(cx, cy, cw, ch, panel);

    int side_w = 18;
    int info_w = 24;
    if (cw < 76) {
        side_w = 14;
        info_w = 0;
    }
    int main_x = cx + side_w + 1;
    int info_x = cx + cw - info_w;
    int main_w = cw - side_w - 2 - info_w;
    if (info_w > 0) main_w--;
    if (main_w < 24) main_w = cw - side_w - 2;

    fm_fill_rect(cx, cy, side_w, ch - 1, sidebar);
    fm_fill_rect(main_x, cy, main_w, 3, toolbar);
    if (info_w > 0) fm_fill_rect(info_x, cy, info_w, ch - 1, panel);

    if (vesa) {
        fb_fill_rect(cx * 8, cy * 16, side_w * 8, (ch - 1) * 16, FB_RGB(24, 34, 54));
        fb_fill_rect(main_x * 8, cy * 16, main_w * 8, 48, FB_RGB(45, 47, 53));
        if (info_w > 0)
            fb_fill_rect(info_x * 8, cy * 16, info_w * 8, (ch - 1) * 16, FB_RGB(18, 18, 22));
    }

    fm_text_clip(cx + 2, cy + 1, "Places", side_w - 4, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLUE));
    const char* places[] = { "Home", "Downloads", "System", "RamFS", "Disk" };
    for (int i = 0; i < 5 && cy + 3 + i < cy + ch - 1; i++) {
        int row = cy + 3 + i;
        int selected_place = fm_disk ? (!strncmp(fm_path, "/disk/Downloads", 15) ? 1 : 4) : !strcmp(fm_path, "/") ? 3 : !strncmp(fm_path, "/etc", 4) ? 2 : 0;
        uint8_t pc = (i == selected_place) ? VGA_COLOR(VGA_WHITE, VGA_CYAN) : sidebar;
        fm_clear_row(cx + 1, row, side_w - 2, pc);
        gui_putchar(cx + 2, row, (i == selected_place) ? '>' : ' ', pc);
        fm_text_clip(cx + 4, row, places[i], side_w - 5, pc);
    }

    fm_draw_vline(cx + side_w, cy, ch - 1, dim);
    if (info_w > 0) fm_draw_vline(info_x - 1, cy, ch - 1, dim);

    fm_text_clip(main_x + 1, cy, "<  >  Up", main_w - 2, toolbar);
    fm_text_clip(main_x + 13, cy, "New", main_w - 14, VGA_COLOR(VGA_LIGHT_GREEN, VGA_DARK_GREY));
    fm_text_clip(main_x + 19, cy, "Delete", main_w - 20, VGA_COLOR(VGA_LIGHT_RED, VGA_DARK_GREY));

    fm_clear_row(main_x + 1, cy + 1, main_w - 2, VGA_COLOR(VGA_WHITE, bg));
    fm_text_clip(main_x + 2, cy + 1, fm_path, main_w - 4, VGA_COLOR(VGA_LIGHT_GREY, bg));

    char count_str[12];
    int_to_str(file_count, count_str);
    char count_line[28];
    strcpy(count_line, count_str);
    strcat(count_line, " items");
    fm_text_clip(main_x + 1, cy + 2, count_line, main_w - 2, toolbar);
    gui_draw_hline(main_x, cy + 3, main_w, (char)0xC4, dim);

    if (fm_preview) {
        fs_node_t* node = fm_get_file(fm_selected);
        if (node) {
            char title[48];
            strcpy(title, "< ");
            strcat(title, node->name);
            fm_text_clip(main_x + 1, cy + 4, title, main_w - 2, accent);
            gui_draw_hline(main_x, cy + 5, main_w, (char)0xC4, dim);

            int row = cy + 6;
            int col_off = 0;
            for (int i = 0; i < fm_preview_len && row < cy + ch - 1; i++) {
                if (fm_preview_buf[i] == '\n' || col_off >= main_w - 3) {
                    row++;
                    col_off = 0;
                    if (fm_preview_buf[i] == '\n') continue;
                }
                gui_putchar(main_x + 1 + col_off, row, fm_preview_buf[i], text_color);
                col_off++;
            }
        }

        fm_clear_row(cx, cy + ch - 1, cw, status_col);
        fm_text_clip(cx + 1, cy + ch - 1, "Enter Back", cw - 2, status_col);
        return;
    }

    int name_x = main_x + 4;
    int type_x = main_x + main_w - 19;
    int size_x = main_x + main_w - 9;
    int visible = ch - 7;
    if (visible < 1) visible = 1;

    gui_draw_text(name_x, cy + 4, "Name", dim);
    gui_draw_text(type_x, cy + 4, "Kind", dim);
    gui_draw_text(size_x, cy + 4, "Size", dim);
    gui_draw_hline(main_x, cy + 5, main_w, (char)0xC4, dim);

    if (fm_selected < fm_scroll) fm_scroll = fm_selected;
    if (fm_selected >= fm_scroll + visible) fm_scroll = fm_selected - visible + 1;

    for (int i = 0; i < visible && (fm_scroll + i) < file_count; i++) {
        int idx = fm_scroll + i;
        int row = cy + 6 + i;
        fs_node_t* node = fm_get_file(idx);
        if (!node) break;

        bool is_selected = (idx == fm_selected);
        uint8_t line_col = is_selected ? sel_color : text_color;

        if (is_selected) {
            if (vesa) {
                fb_fill_rect(main_x * 8 + 4, row * 16, main_w * 8 - 8, 16, FB_RGB(42, 92, 170));
            } else {
                fm_clear_row(main_x, row, main_w, sel_color);
            }
        }

        if (vesa) {
            int px = (main_x + 1) * 8;
            int py = row * 16 + 2;
            if (node->type & FS_DIRECTORY) {
                fb_fill_rect(px + 2, py + 2, 6, 2, FB_RGB(255, 220, 100));
                fb_fill_rect(px, py + 4, 12, 8, FB_RGB(255, 200, 50));
                fb_draw_rect(px, py + 4, 12, 8, FB_RGB(200, 150, 0));
            } else {
                fb_fill_rect(px + 2, py, 10, 14, FB_RGB(255, 255, 255));
                fb_draw_rect(px + 2, py, 10, 14, FB_RGB(150, 150, 150));
                fb_fill_rect(px + 9, py, 3, 3, FB_RGB(200, 200, 200));
                fb_fill_rect(px + 4, py + 4, 6, 1, FB_RGB(150, 150, 150));
                fb_fill_rect(px + 4, py + 7, 6, 1, FB_RGB(150, 150, 150));
            }
        } else {
            char icon = (node->type & FS_DIRECTORY) ? '\x10' : '\x07';
            uint8_t icon_col = is_selected ? sel_color :
                               (node->type & FS_DIRECTORY) ? dir_color : dim;
            gui_putchar(main_x + 1, row, icon, icon_col);
        }

        fm_text_clip(name_x, row, node->name, type_x - name_x - 1, line_col);
        fm_text_clip(type_x, row, (node->type & FS_DIRECTORY) ? "Folder" : "File",
                     size_x - type_x - 1, is_selected ? sel_color : muted);
        if (!(node->type & FS_DIRECTORY)) {
            char size_str[14];
            fm_format_size(node->size, size_str);
            fm_text_clip(size_x, row, size_str, main_x + main_w - size_x - 1,
                         is_selected ? sel_color : dim);
        }
    }

    if (info_w > 0) {
        fs_node_t* node = fm_get_file(fm_selected);
        fm_text_clip(info_x + 2, cy + 1, "Details", info_w - 4, accent);
        gui_draw_hline(info_x, cy + 2, info_w, (char)0xC4, dim);
        if (node) {
            fm_text_clip(info_x + 2, cy + 4, node->name, info_w - 4, text_color);
            fm_text_clip(info_x + 2, cy + 6, (node->type & FS_DIRECTORY) ? "Folder" : "File", info_w - 4, muted);
            if (!(node->type & FS_DIRECTORY)) {
                char size_str[14];
                char size_line[26];
                fm_format_size(node->size, size_str);
                strcpy(size_line, "Size: ");
                strcat(size_line, size_str);
                fm_text_clip(info_x + 2, cy + 7, size_line, info_w - 4, muted);

                fm_load_preview();
                gui_draw_hline(info_x, cy + 9, info_w, (char)0xC4, dim);
                fm_text_clip(info_x + 2, cy + 10, "Preview", info_w - 4, accent);
                int row = cy + 12;
                int col = 0;
                for (int i = 0; i < fm_preview_len && row < cy + ch - 2; i++) {
                    if (fm_preview_buf[i] == '\n' || col >= info_w - 4) {
                        row++;
                        col = 0;
                        if (fm_preview_buf[i] == '\n') continue;
                    }
                    gui_putchar(info_x + 2 + col, row, fm_preview_buf[i], dim);
                    col++;
                }
            }
        }
    }

    fm_clear_row(cx, cy + ch - 1, cw, status_col);
    fm_text_clip(cx + 1, cy + ch - 1,
                 fm_notice[0] ? fm_notice : "Arrows Select   Enter Open/Preview   Backspace Up   N New   D Delete",
                 cw - 2, status_col);
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
    if (key == '\b' || (key == '\n' && (fm_disk || (fm_dir && fm_dir->parent)) && fm_selected == 0)) {
        char parent[FAT32_PATH_MAX]; strcpy(parent, fm_path);
        int len = (int)strlen(parent);
        while (len > 1 && parent[len - 1] == '/') parent[--len] = 0;
        while (len > 1 && parent[len - 1] != '/') parent[--len] = 0;
        if (len > 1) parent[len - 1] = 0;
        fm_set_path(parent); return;
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
            fs_node_t* node = fm_get_file(fm_selected);
            if (node && node->type == FS_DIRECTORY) {
                char path[FAT32_PATH_MAX];
                if (!fm_child_path(fm_disk ? fm_fatentry.name : node->name, path) || !fm_set_path(path)) strcpy(fm_notice, "Cannot open folder");
            } else { fm_load_preview(); fm_preview = true; }
        }
        return;
    }

    /* D — delete */
    if (key == 'd' || key == 'D') {
        if (file_count > 0) {
            fs_node_t* node = fm_get_file(fm_selected);
            if (node && !(node == &fm_synthetic && !fm_disk) && !(fm_disk && fm_selected == 0)) {
                int result;
                if (fm_disk) {
                    char path[FAT32_PATH_MAX];
                    result = fm_child_path(fm_fatentry.name, path) ? fat32_remove_path(path + 5) : -1;
                } else result = ramfs_rm_path(node->name, fm_dir);
                if (result < 0) strcpy(fm_notice, "Could not delete (permission, nonempty folder or I/O error)");
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
        if (fm_disk) {
            char path[FAT32_PATH_MAX];
            if (!fm_child_path(name, path) || fat32_create_path(path + 5, false, NULL) < 0) strcpy(fm_notice, "Could not create file");
        } else if (!vfs_check_perm(fm_dir, users_current_uid(), ACC_WRITE | ACC_EXEC) || !ramfs_mknode(fm_dir, name, FS_FILE)) strcpy(fm_notice, "Could not create file");
        return;
    }
}

/* --------------------------------------------------------------------------
 * filemgr_open: Create a file manager window
 * -------------------------------------------------------------------------- */
static void fm_click(int id, int px, int py) {
    int x, y, w, h;
    if (!window_content_px(id, &x, &y, &w, &h)) return;
    int side = w / 8 < 76 ? 14 : 18;
    int col = px / 8, row = py / 16;
    if (col < side && row >= 3 && row < 8) {
        const char* paths[] = {"/home", "/disk/Downloads", "/etc", "/", "/disk"};
        if (row == 4) { if (filemgr_open_path(paths[1]) < 0) strcpy(fm_notice, "Cannot open Downloads"); return; }
        if (!fm_set_path(paths[row - 3])) strcpy(fm_notice, "Cannot open folder");
        return;
    }
    if (col > side && row == 0) {
        int relative = col - side - 1;
        if (relative < 12) fm_key(id, '\b');
        else if (relative < 19) fm_key(id, 'n');
        else fm_key(id, 'd');
        return;
    }
    if (col > side && row >= 6) {
        int index = fm_scroll + row - 6;
        if (index < fm_count_files()) {
            bool again = index == fm_selected;
            fm_selected = index;
            if (again) fm_key(id, '\n');
        }
    }
}

int filemgr_open_path(const char* path) {
    if (!strcmp(path, "/disk/Downloads") && fat32_is_mounted()) {
        fat32_file_t dir;
        if (!fat32_lookup("Downloads", &dir)) fat32_create_path("Downloads", true, NULL);
    }
    if (!fm_set_path(path)) return -1;
    fm_preview_buf[0] = '\0';
    fm_preview_len = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* win = window_get(i);
        if (win && win->active && win->draw == fm_draw) { window_restore(i); window_focus(i); return i; }
    }
    int id = window_create("File Manager", 6, 4, 104, 34, fm_draw, fm_key);
    if (id >= 0) window_set_on_click(id, fm_click);
    return id;
}
int filemgr_open(void) { return filemgr_open_path("/"); }
