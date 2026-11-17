/* ============================================================================
 * NexusOS — Notepad App (Implementation)
 * ============================================================================
 * Simple multi-line text editor in a window. Saves to /notes.txt in RamFS.
 * ============================================================================ */

#include "notepad.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "ramfs.h"
#include "vfs.h"
#include "clipboard.h"
#include "notify.h"
#include "appui.h"
#include "macui.h"
#include "framebuffer.h"

#define NP_LINES    16
#define NP_COLS     36
#define NP_FILENAME "notes.txt"

static char np_buf[NP_LINES][NP_COLS];
static int  np_cursor_x = 0;
static int  np_cursor_y = 0;
static int  np_line_count = 1;
static bool np_dirty = false;

extern volatile uint32_t system_ticks;

static void np_init(void) {
    for (int i = 0; i < NP_LINES; i++) np_buf[i][0] = '\0';
    np_cursor_x = 0; np_cursor_y = 0; np_line_count = 1; np_dirty = false;

    /* Try to load existing file */
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, NP_FILENAME);
    if (node && node->type == FS_FILE && node->size > 0) {
        char tmp[NP_LINES * NP_COLS];
        int len = vfs_read(node, 0, sizeof(tmp) - 1, (uint8_t*)tmp);
        if (len > 0) {
            tmp[len] = '\0';
            int line = 0, col = 0;
            for (int i = 0; i < len && line < NP_LINES; i++) {
                if (tmp[i] == '\n') {
                    np_buf[line][col] = '\0';
                    line++; col = 0;
                } else if (col < NP_COLS - 1) {
                    np_buf[line][col++] = tmp[i];
                    np_buf[line][col] = '\0';
                }
            }
            np_line_count = line + 1;
            if (np_line_count > NP_LINES) np_line_count = NP_LINES;
        }
    }
}

static void np_save(void) {
    char content[NP_LINES * NP_COLS];
    content[0] = '\0';
    for (int i = 0; i < np_line_count; i++) {
        strcat(content, np_buf[i]);
        if (i < np_line_count - 1) strcat(content, "\n");
    }

    ramfs_create(NP_FILENAME, FS_FILE);
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, NP_FILENAME);
    if (node) {
        vfs_write(node, 0, strlen(content), (const uint8_t*)content);
        np_dirty = false;
        notify_push("Notes saved!");
    }
}

static void np_draw(int id, int cx, int cy, int cw, int ch) {
    (void)cx; (void)cy; (void)cw; (void)ch;
    int x, y, w, h;
    if (!appui_content(id, &x, &y, &w, &h)) return;
    appui_px_fill(x, y, w, h, APPUI_BG);

    /* title + line:col, like the Notes toolbar */
    char pos[16], n1[6], n2[6];
    int_to_str(np_cursor_y + 1, n1); int_to_str(np_cursor_x + 1, n2);
    strcpy(pos, "Ln "); strcat(pos, n1); strcat(pos, ", Col "); strcat(pos, n2);
    appui_px_text(x + 16, y + 10, np_dirty ? "Untitled *" : "Untitled", APPUI_F_BOLD, APPUI_TEXT);
    appui_px_text_r(x + w - 16, y + 12, pos, APPUI_F_SMALL, APPUI_SUBTLE);
    appui_px_sep(x, y + 34, w);

    /* text area — anti-aliased 8x16 monospace cells so the caret lines up */
    int tx = x + 16, ty = y + 44, lh = 16;
    for (int i = 0; i < np_line_count && ty + i * lh < y + h - 24; i++)
        for (int j = 0; np_buf[i][j] && tx + j * 8 < x + w - 8; j++)
            ui_cell_glyph(tx + j * 8, ty + i * lh, (uint8_t)np_buf[i][j], APPUI_TEXT, APPUI_BG);
    /* thin blinking caret */
    if ((system_ticks % 16) < 8) {
        int caret_x = tx + np_cursor_x * 8;
        int caret_y = ty + np_cursor_y * lh;
        ui_blend_rect(caret_x, caret_y, 2, 15, APPUI_ACCENT, 255);
    }

    /* bottom hint */
    appui_px_sep(x, y + h - 24, w);
    appui_px_text(x + 16, y + h - 19, "Ctrl+S  Save      Ctrl+V  Paste", APPUI_F_SMALL, APPUI_FAINT);
}

static void np_key(int id, char key) {
    (void)id;

    /* Ctrl+S save */
    if (key == 19) { np_save(); return; }

    /* Ctrl+V paste */
    if (key == 22) {
        const char* clip = clipboard_paste();
        if (clip) {
            while (*clip && np_cursor_x < NP_COLS - 1) {
                if (*clip == '\n') {
                    if (np_line_count < NP_LINES) {
                        np_buf[np_cursor_y][np_cursor_x] = '\0';
                        np_cursor_y++; np_cursor_x = 0;
                        np_line_count++;
                        np_buf[np_cursor_y][0] = '\0';
                    }
                } else {
                    np_buf[np_cursor_y][np_cursor_x++] = *clip;
                    np_buf[np_cursor_y][np_cursor_x] = '\0';
                }
                clip++;
            }
            np_dirty = true;
        }
        return;
    }

    /* Arrow keys */
    if ((unsigned char)key == 0x80) { if (np_cursor_y > 0) { np_cursor_y--; int len = strlen(np_buf[np_cursor_y]); if (np_cursor_x > len) np_cursor_x = len; } return; }
    if ((unsigned char)key == 0x81) { if (np_cursor_y < np_line_count - 1) { np_cursor_y++; int len = strlen(np_buf[np_cursor_y]); if (np_cursor_x > len) np_cursor_x = len; } return; }
    if ((unsigned char)key == 0x82) { if (np_cursor_x > 0) np_cursor_x--; return; }
    if ((unsigned char)key == 0x83) { int len = strlen(np_buf[np_cursor_y]); if (np_cursor_x < len) np_cursor_x++; return; }

    /* Enter */
    if (key == '\n') {
        if (np_line_count < NP_LINES) {
            /* Shift lines down */
            for (int i = np_line_count; i > np_cursor_y + 1; i--)
                strcpy(np_buf[i], np_buf[i - 1]);
            /* Split current line */
            char rest[NP_COLS];
            strcpy(rest, np_buf[np_cursor_y] + np_cursor_x);
            np_buf[np_cursor_y][np_cursor_x] = '\0';
            np_cursor_y++; np_cursor_x = 0;
            strcpy(np_buf[np_cursor_y], rest);
            np_line_count++;
            np_dirty = true;
        }
        return;
    }

    /* Backspace */
    if (key == '\b') {
        if (np_cursor_x > 0) {
            int len = strlen(np_buf[np_cursor_y]);
            for (int i = np_cursor_x - 1; i < len; i++)
                np_buf[np_cursor_y][i] = np_buf[np_cursor_y][i + 1];
            np_cursor_x--;
            np_dirty = true;
        } else if (np_cursor_y > 0) {
            /* Join with previous line */
            int prev_len = strlen(np_buf[np_cursor_y - 1]);
            if (prev_len + (int)strlen(np_buf[np_cursor_y]) < NP_COLS - 1) {
                strcat(np_buf[np_cursor_y - 1], np_buf[np_cursor_y]);
                for (int i = np_cursor_y; i < np_line_count - 1; i++)
                    strcpy(np_buf[i], np_buf[i + 1]);
                np_line_count--;
                np_cursor_y--;
                np_cursor_x = prev_len;
                np_dirty = true;
            }
        }
        return;
    }

    /* Normal character */
    if (key >= 32 && key < 127 && np_cursor_x < NP_COLS - 2) {
        int len = strlen(np_buf[np_cursor_y]);
        if (len < NP_COLS - 2) {
            for (int i = len + 1; i > np_cursor_x; i--)
                np_buf[np_cursor_y][i] = np_buf[np_cursor_y][i - 1];
            np_buf[np_cursor_y][np_cursor_x++] = key;
            np_dirty = true;
        }
    }
}

int notepad_open(void) {
    np_init();
    return window_create("Notepad", 20, 4, 48, 24, np_draw, np_key);
}
