/* ============================================================================
 * NexusOS — Calculator App (Implementation)
 * ============================================================================
 * Integer calculator that runs as a desktop window.
 * Supports +, -, *, / on integers. Type expression, Enter to evaluate.
 * ============================================================================ */

#include "calculator.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "framebuffer.h"

#ifndef FB_RGB
#define FB_RGB(r, g, b) (((0xFF & r) << 16) | ((0xFF & g) << 8) | (0xFF & b))
#endif

/* Calculator state */
#define CALC_EXPR_MAX  30
#define CALC_HIST_MAX  8
#define CALC_HIST_COLS 28

static char calc_expr[CALC_EXPR_MAX];
static int  calc_expr_len = 0;
static char calc_result[CALC_EXPR_MAX];
static char calc_history[CALC_HIST_MAX][CALC_HIST_COLS];
static int  calc_hist_count = 0;

/* Frame counter for cursor blink (shared via extern) */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * calc_add_history: Store a line in the calculator history
 * -------------------------------------------------------------------------- */
static void calc_add_history(const char* text) {
    if (calc_hist_count < CALC_HIST_MAX) {
        strncpy(calc_history[calc_hist_count], text, CALC_HIST_COLS - 1);
        calc_history[calc_hist_count][CALC_HIST_COLS - 1] = '\0';
        calc_hist_count++;
    } else {
        for (int i = 0; i < CALC_HIST_MAX - 1; i++) {
            strcpy(calc_history[i], calc_history[i + 1]);
        }
        strncpy(calc_history[CALC_HIST_MAX - 1], text, CALC_HIST_COLS - 1);
        calc_history[CALC_HIST_MAX - 1][CALC_HIST_COLS - 1] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * parse_int: Parse an integer from a string, advancing the pointer
 * -------------------------------------------------------------------------- */
static int parse_int(const char** p) {
    int sign = 1;
    if (**p == '-') { sign = -1; (*p)++; }
    int val = 0;
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val * sign;
}

/* --------------------------------------------------------------------------
 * calc_evaluate: Evaluate a simple integer expression (left-to-right)
 * Supports: +, -, *, /
 * -------------------------------------------------------------------------- */
static int calc_evaluate(const char* expr, bool* ok) {
    *ok = true;
    const char* p = expr;

    /* Skip spaces */
    while (*p == ' ') p++;
    if (*p == '\0') { *ok = false; return 0; }

    int result = parse_int(&p);

    while (*p) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        char op = *p;
        if (op != '+' && op != '-' && op != '*' && op != '/') {
            *ok = false;
            return 0;
        }
        p++;
        while (*p == ' ') p++;

        int operand = parse_int(&p);

        switch (op) {
            case '+': result += operand; break;
            case '-': result -= operand; break;
            case '*': result *= operand; break;
            case '/':
                if (operand == 0) { *ok = false; return 0; }
                result /= operand;
                break;
        }
    }

    return result;
}

/* --------------------------------------------------------------------------
 * calc_process: Process current expression
 * -------------------------------------------------------------------------- */
static void calc_process(void) {
    if (calc_expr_len == 0) return;

    /* Add expression to history */
    char hist_line[CALC_HIST_COLS];
    strcpy(hist_line, "> ");
    strcat(hist_line, calc_expr);
    calc_add_history(hist_line);

    /* Evaluate */
    bool ok;
    int val = calc_evaluate(calc_expr, &ok);

    if (ok) {
        char num_buf[16];
        /* Handle negative numbers */
        if (val < 0) {
            strcpy(calc_result, "= -");
            val = -val;
            int_to_str(val, num_buf);
            strcat(calc_result, num_buf);
        } else {
            strcpy(calc_result, "= ");
            int_to_str(val, num_buf);
            strcat(calc_result, num_buf);
        }
    } else {
        strcpy(calc_result, "= Error");
    }

    calc_add_history(calc_result);

    /* Clear expression */
    calc_expr[0] = '\0';
    calc_expr_len = 0;
}

/* --------------------------------------------------------------------------
 * Calc window callbacks
 * -------------------------------------------------------------------------- */
static void calc_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t text_color = t->win_content;
    uint8_t bg = (text_color >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    bool vesa = fb_is_vesa();

    if (vesa) {
        int px = cx * 8;
        int py = cy * 16;
        int pw = cw * 8;
        int ph = ch * 16;
        
        /* App background */
        fb_fill_rect(px, py, pw, ph, FB_RGB(30, 30, 30));
        
        /* Input box background */
        fb_fill_rect(px + 8, py + ph - 22, pw - 16, 18, FB_RGB(10, 10, 15));
        fb_draw_rect(px + 8, py + ph - 22, pw - 16, 18, FB_RGB(80, 80, 90));
    }

    /* Draw header */
    gui_draw_text(cx + 1, cy, "\x04 Calculator", accent);

    /* Separator */
    if (!vesa) {
        for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
            gui_putchar(cx + i, cy + 1, (char)0xC4, dim);
    } else {
        fb_fill_rect(cx * 8, cy * 16 + 18, cw * 8, 1, FB_RGB(100, 100, 100));
    }

    /* History lines */
    int hist_start = cy + 2;
    int visible = ch - 4;
    int start_idx = calc_hist_count - visible;
    if (start_idx < 0) start_idx = 0;

    for (int i = start_idx; i < calc_hist_count; i++) {
        int row = hist_start + (i - start_idx);
        if (row >= cy + ch - 2) break;

        /* Color: results in cyan, input in default */
        uint8_t line_col = (calc_history[i][0] == '=') ? accent : text_color;
        int j = 0;
        while (calc_history[i][j] && j < cw - 1) {
            gui_putchar(cx + j, row, calc_history[i][j], line_col);
            j++;
        }
    }

    /* Separator above input */
    int sep_row = cy + ch - 2;
    if (!vesa) {
        for (int i = 0; i < cw - 1 && cx + i < GUI_WIDTH; i++)
            gui_putchar(cx + i, sep_row, (char)0xC4, dim);
    }

    /* Input line */
    int input_row = cy + ch - 1;
    gui_putchar(cx, input_row, '>', VGA_COLOR(VGA_LIGHT_GREEN, bg));
    gui_putchar(cx + 1, input_row, ' ', text_color);

    int max_input = cw - 3;
    for (int i = 0; i < calc_expr_len && i < max_input; i++) {
        gui_putchar(cx + 2 + i, input_row, calc_expr[i], text_color);
    }

    /* Blinking cursor */
    if (calc_expr_len < max_input) {
        char cursor_ch = (system_ticks % 16 < 8) ? '_' : ' ';
        gui_putchar(cx + 2 + calc_expr_len, input_row, cursor_ch,
                    VGA_COLOR(VGA_WHITE, bg));
    }
}

static void calc_key(int id, char key) {
    (void)id;

    if (key == '\n') {
        calc_process();
    } else if (key == '\b') {
        if (calc_expr_len > 0) {
            calc_expr_len--;
            calc_expr[calc_expr_len] = '\0';
        }
    } else if (key == 3) {  /* Ctrl+C = clear */
        calc_expr[0] = '\0';
        calc_expr_len = 0;
        calc_hist_count = 0;
        calc_result[0] = '\0';
    } else if (key >= 32 && key < 127 && calc_expr_len < CALC_EXPR_MAX - 1) {
        calc_expr[calc_expr_len++] = key;
        calc_expr[calc_expr_len] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * calculator_open: Create a calculator window
 * -------------------------------------------------------------------------- */
int calculator_open(void) {
    /* Reset state */
    calc_expr[0] = '\0';
    calc_expr_len = 0;
    calc_result[0] = '\0';
    calc_hist_count = 0;

    calc_add_history("Type expression + Enter");
    calc_add_history("e.g. 12+34  50*2");
    calc_add_history("Ctrl+C to clear");

    return window_create("Calculator", 30, 3, 28, 14, calc_draw, calc_key);
}
