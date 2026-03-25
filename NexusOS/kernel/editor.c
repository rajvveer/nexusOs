/* ============================================================================
 * NexusOS — Text Editor (Implementation)
 * ============================================================================
 * Simple nano-like editor in VGA text mode.
 * - Arrow keys to navigate
 * - Type to insert characters
 * - Ctrl+S = save, Ctrl+Q = quit
 * - Header bar shows filename, footer bar shows shortcuts
 * ============================================================================ */

#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "vfs.h"
#include "ramfs.h"
#include "string.h"

#define EDITOR_MAX_LINES  50
#define EDITOR_MAX_COLS   80
#define EDITOR_VIEW_ROWS  22   /* Rows 1-22 for text (row 0 = header, row 23 = shortcut bar) */

/* Editor state */
static char lines[EDITOR_MAX_LINES][EDITOR_MAX_COLS];
static int num_lines;
static int cursor_line;
static int cursor_col_ed;
static int scroll_offset;
static char editor_filename[64];
static int editor_modified;

/* --------------------------------------------------------------------------
 * draw_header: Draw editor header bar
 * -------------------------------------------------------------------------- */
static void draw_header(void) {
    uint8_t hdr_color = VGA_COLOR(VGA_WHITE, VGA_CYAN);
    for (int c = 0; c < VGA_WIDTH; c++) {
        vga_putchar_at(' ', 0, c, hdr_color);
    }

    /* Title */
    const char* title = " NexusOS Editor";
    for (int i = 0; title[i]; i++) {
        vga_putchar_at(title[i], 0, i, hdr_color);
    }

    /* Filename */
    if (editor_filename[0]) {
        int col = 20;
        const char* prefix = "- ";
        for (int i = 0; prefix[i]; i++, col++) {
            vga_putchar_at(prefix[i], 0, col, hdr_color);
        }
        for (int i = 0; editor_filename[i] && col < VGA_WIDTH - 1; i++, col++) {
            vga_putchar_at(editor_filename[i], 0, col, hdr_color);
        }
    }

    /* Modified indicator */
    if (editor_modified) {
        const char* mod = "[Modified]";
        int col = VGA_WIDTH - 12;
        for (int i = 0; mod[i]; i++, col++) {
            vga_putchar_at(mod[i], 0, col, hdr_color);
        }
    }
}

/* --------------------------------------------------------------------------
 * draw_footer: Draw shortcut bar
 * -------------------------------------------------------------------------- */
static void draw_footer(void) {
    uint8_t foot_color = VGA_COLOR(VGA_WHITE, VGA_CYAN);
    int row = 23;
    for (int c = 0; c < VGA_WIDTH; c++) {
        vga_putchar_at(' ', row, c, foot_color);
    }
    const char* shortcuts = " ^S Save  ^Q Quit  Arrow Keys Navigate";
    for (int i = 0; shortcuts[i] && i < VGA_WIDTH; i++) {
        vga_putchar_at(shortcuts[i], row, i, foot_color);
    }

    /* Line/Col indicator */
    char pos[20];
    pos[0] = 'L';
    char num[8];
    int_to_str(cursor_line + 1, num);
    int p = 1;
    for (int i = 0; num[i]; i++) pos[p++] = num[i];
    pos[p++] = ':';
    pos[p++] = 'C';
    int_to_str(cursor_col_ed + 1, num);
    for (int i = 0; num[i]; i++) pos[p++] = num[i];
    pos[p] = '\0';

    int col = VGA_WIDTH - p - 1;
    for (int i = 0; pos[i]; i++, col++) {
        vga_putchar_at(pos[i], row, col, foot_color);
    }
}

/* --------------------------------------------------------------------------
 * draw_text: Render text area (rows 1-22)
 * -------------------------------------------------------------------------- */
static void draw_text(void) {
    uint8_t text_color = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLACK);
    uint8_t linenum_color = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK);

    for (int row = 0; row < EDITOR_VIEW_ROWS; row++) {
        int line_idx = row + scroll_offset;
        int screen_row = row + 1;  /* Offset by header */

        if (line_idx < num_lines) {
            /* Line number (3 chars wide) */
            char lnum[4];
            int n = line_idx + 1;
            lnum[0] = (n >= 100) ? ('0' + n / 100) : ' ';
            lnum[1] = (n >= 10) ? ('0' + (n / 10) % 10) : ' ';
            lnum[2] = '0' + (n % 10);
            lnum[3] = '\0';

            for (int i = 0; i < 3; i++) {
                vga_putchar_at(lnum[i], screen_row, i, linenum_color);
            }
            vga_putchar_at('|', screen_row, 3, linenum_color);

            /* Line content */
            int len = strlen(lines[line_idx]);
            for (int c = 0; c < VGA_WIDTH - 4; c++) {
                char ch = (c < len) ? lines[line_idx][c] : ' ';
                vga_putchar_at(ch, screen_row, c + 4, text_color);
            }
        } else {
            /* Empty line: show tilde */
            vga_putchar_at('~', screen_row, 0, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            for (int c = 1; c < VGA_WIDTH; c++) {
                vga_putchar_at(' ', screen_row, c, text_color);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * draw_screen: Full redraw
 * -------------------------------------------------------------------------- */
static void draw_screen(void) {
    draw_header();
    draw_text();
    draw_footer();
    /* Position cursor in text area */
    int screen_col = cursor_col_ed + 4;  /* +4 for line numbers */
    int screen_row = cursor_line - scroll_offset + 1;
    vga_set_cursor(screen_row, screen_col);
}

/* --------------------------------------------------------------------------
 * ensure_visible: Scroll to keep cursor visible
 * -------------------------------------------------------------------------- */
static void ensure_visible(void) {
    if (cursor_line < scroll_offset) {
        scroll_offset = cursor_line;
    }
    if (cursor_line >= scroll_offset + EDITOR_VIEW_ROWS) {
        scroll_offset = cursor_line - EDITOR_VIEW_ROWS + 1;
    }
}

/* --------------------------------------------------------------------------
 * insert_char: Insert a character at cursor position
 * -------------------------------------------------------------------------- */
static void insert_char(char c) {
    int len = strlen(lines[cursor_line]);
    if (len >= EDITOR_MAX_COLS - 2) return;

    /* Shift right */
    for (int i = len; i >= cursor_col_ed; i--) {
        lines[cursor_line][i + 1] = lines[cursor_line][i];
    }
    lines[cursor_line][cursor_col_ed] = c;
    cursor_col_ed++;
    editor_modified = 1;
}

/* --------------------------------------------------------------------------
 * delete_char: Delete character at cursor (backspace)
 * -------------------------------------------------------------------------- */
static void delete_char(void) {
    if (cursor_col_ed > 0) {
        int len = strlen(lines[cursor_line]);
        for (int i = cursor_col_ed - 1; i < len; i++) {
            lines[cursor_line][i] = lines[cursor_line][i + 1];
        }
        cursor_col_ed--;
        editor_modified = 1;
    } else if (cursor_line > 0) {
        /* Join with previous line */
        int prev_len = strlen(lines[cursor_line - 1]);
        int cur_len = strlen(lines[cursor_line]);

        if (prev_len + cur_len < EDITOR_MAX_COLS - 1) {
            strcat(lines[cursor_line - 1], lines[cursor_line]);
            /* Shift lines up */
            for (int i = cursor_line; i < num_lines - 1; i++) {
                strcpy(lines[i], lines[i + 1]);
            }
            num_lines--;
            cursor_line--;
            cursor_col_ed = prev_len;
            editor_modified = 1;
        }
    }
}

/* --------------------------------------------------------------------------
 * insert_newline: Split line at cursor
 * -------------------------------------------------------------------------- */
static void insert_newline(void) {
    if (num_lines >= EDITOR_MAX_LINES) return;

    /* Shift lines down */
    for (int i = num_lines; i > cursor_line + 1; i--) {
        strcpy(lines[i], lines[i - 1]);
    }

    /* Split current line */
    strcpy(lines[cursor_line + 1], &lines[cursor_line][cursor_col_ed]);
    lines[cursor_line][cursor_col_ed] = '\0';

    num_lines++;
    cursor_line++;
    cursor_col_ed = 0;
    editor_modified = 1;
}

/* --------------------------------------------------------------------------
 * save_file: Write buffer to ramfs
 * -------------------------------------------------------------------------- */
static void save_file(void) {
    if (!editor_filename[0]) return;

    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, editor_filename);
    if (!node) {
        node = ramfs_create(editor_filename, FS_FILE);
    }
    if (!node) return;

    /* Build content string */
    char content[4096];
    content[0] = '\0';
    int pos = 0;
    for (int i = 0; i < num_lines; i++) {
        int len = strlen(lines[i]);
        for (int j = 0; j < len && pos < 4090; j++) {
            content[pos++] = lines[i][j];
        }
        if (i < num_lines - 1 && pos < 4090) {
            content[pos++] = '\n';
        }
    }
    content[pos] = '\0';

    vfs_write(node, 0, pos, (uint8_t*)content);
    editor_modified = 0;
}

/* --------------------------------------------------------------------------
 * load_file: Read file from ramfs into editor buffer
 * -------------------------------------------------------------------------- */
static void load_file(const char* filename) {
    /* Copy filename safely */
    int k;
    for (k = 0; filename[k] && k < 63; k++) {
        editor_filename[k] = filename[k];
    }
    editor_filename[k] = '\0';

    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, filename);

    num_lines = 1;
    memset(lines, 0, sizeof(lines));
    cursor_line = 0;
    cursor_col_ed = 0;
    scroll_offset = 0;
    editor_modified = 0;

    if (node && node->size > 0) {
        char content[4096];
        uint32_t bytes = vfs_read(node, 0, node->size < 4095 ? node->size : 4095, (uint8_t*)content);
        content[bytes] = '\0';

        /* Parse into lines */
        int line = 0, col = 0;
        for (uint32_t i = 0; i < bytes && line < EDITOR_MAX_LINES; i++) {
            if (content[i] == '\n') {
                lines[line][col] = '\0';
                line++;
                col = 0;
            } else if (col < EDITOR_MAX_COLS - 2) {
                lines[line][col++] = content[i];
            }
        }
        lines[line][col] = '\0';
        num_lines = line + 1;
    }
}

/* --------------------------------------------------------------------------
 * editor_run: Main editor loop
 * -------------------------------------------------------------------------- */
void editor_run(const char* filename) {
    if (filename) {
        load_file(filename);
    } else {
        editor_filename[0] = '\0';
        num_lines = 1;
        memset(lines, 0, sizeof(lines));
        cursor_line = 0;
        cursor_col_ed = 0;
        scroll_offset = 0;
        editor_modified = 0;
    }

    /* Clear screen and enter editor mode */
    vga_clear_rows(0, 23);
    draw_screen();

    int running = 1;
    while (running) {
        char c = keyboard_getchar();

        /* Ctrl+Q = quit */
        if (c == 17) {  /* Ctrl+Q */
            running = 0;
            break;
        }

        /* Ctrl+S = save */
        if (c == 19) {  /* Ctrl+S */
            save_file();
            draw_screen();
            continue;
        }

        switch ((unsigned char)c) {
            case 0x80:  /* Up arrow */
                if (cursor_line > 0) {
                    cursor_line--;
                    int len = strlen(lines[cursor_line]);
                    if (cursor_col_ed > len) cursor_col_ed = len;
                }
                break;

            case 0x81:  /* Down arrow */
                if (cursor_line < num_lines - 1) {
                    cursor_line++;
                    int len = strlen(lines[cursor_line]);
                    if (cursor_col_ed > len) cursor_col_ed = len;
                }
                break;

            case 0x82:  /* Left arrow */
                if (cursor_col_ed > 0) {
                    cursor_col_ed--;
                } else if (cursor_line > 0) {
                    cursor_line--;
                    cursor_col_ed = strlen(lines[cursor_line]);
                }
                break;

            case 0x83:  /* Right arrow */
                if (cursor_col_ed < (int)strlen(lines[cursor_line])) {
                    cursor_col_ed++;
                } else if (cursor_line < num_lines - 1) {
                    cursor_line++;
                    cursor_col_ed = 0;
                }
                break;

            case '\b':  /* Backspace */
                delete_char();
                break;

            case '\n':  /* Enter */
                insert_newline();
                break;

            default:
                if (c >= 32 && c < 127) {
                    insert_char(c);
                }
                break;
        }

        ensure_visible();
        draw_screen();
    }

    /* Restore screen */
    vga_clear();
}
