/* ============================================================================
 * NexusOS — Calculator (Aurora, macOS style)
 * ============================================================================
 * Integer expression evaluator with a big right-aligned display and a
 * clickable button grid. Typing still works (left-to-right +, -, *, /).
 * ============================================================================ */

#include "calculator.h"
#include "window.h"
#include "appui.h"
#include "macui.h"
#include "framebuffer.h"
#include "string.h"

#define EXPR_MAX 40
static char expr[EXPR_MAX];
static int  expr_len = 0;
static char shown[EXPR_MAX];     /* last evaluated result, shown until next key */
static bool have_result = false;

extern volatile uint32_t system_ticks;

/* ---- evaluation (left-to-right, integer) ---------------------------------- */
static int parse_int(const char** p) {
    int sign = 1;
    if (**p == '-') { sign = -1; (*p)++; }
    int v = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return v * sign;
}
static int eval(const char* e, bool* ok) {
    *ok = true;
    const char* p = e;
    while (*p == ' ') p++;
    if (!*p) { *ok = false; return 0; }
    int r = parse_int(&p);
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char op = *p++;
        while (*p == ' ') p++;
        int o = parse_int(&p);
        if (op == '+') r += o;
        else if (op == '-') r -= o;
        else if (op == '*') r *= o;
        else if (op == '/') { if (!o) { *ok = false; return 0; } r /= o; }
        else { *ok = false; return 0; }
    }
    return r;
}

static void do_equals(void) {
    if (!expr_len) return;
    bool ok;
    int v = eval(expr, &ok);
    if (ok) {
        char n[16];
        if (v < 0) { shown[0] = '-'; int_to_str(-v, n); strcpy(shown + 1, n); }
        else int_to_str(v, shown);
        /* result becomes the new starting operand (chain like a real calc) */
        strcpy(expr, shown);
        expr_len = (int)strlen(expr);
    } else {
        strcpy(shown, "Error");
        expr[0] = 0; expr_len = 0;
    }
    have_result = true;
}

static void feed(char key) {
    if (key == '\n') { do_equals(); return; }
    if (key == 3) { expr[0] = 0; expr_len = 0; shown[0] = 0; have_result = false; return; }
    if (key == '\b') { if (expr_len) expr[--expr_len] = 0; have_result = false; return; }
    if (have_result && key >= '0' && key <= '9') { expr[0] = 0; expr_len = 0; have_result = false; }
    else have_result = false;
    if (expr_len < EXPR_MAX - 1 && ((key >= '0' && key <= '9') || key == '+' || key == '-' ||
                                    key == '*' || key == '/' || key == '.' || key == ' ')) {
        expr[expr_len++] = key; expr[expr_len] = 0;
    }
}

/* ---- button grid ---------------------------------------------------------- */
typedef struct { int c, r, cs, rs; const char* label; char key; int kind; } btn_t;
enum { K_NUM = 0, K_OP, K_TOP };    /* number, operator (orange), top-row (grey) */

static const btn_t BTN[] = {
    { 0, 0, 1, 1, "AC", 3,    K_TOP }, { 1, 0, 1, 1, "<",  '\b', K_TOP },
    { 2, 0, 1, 1, "/",  '/',  K_OP  }, { 3, 0, 1, 1, "x",  '*',  K_OP  },
    { 0, 1, 1, 1, "7",  '7',  K_NUM }, { 1, 1, 1, 1, "8",  '8',  K_NUM },
    { 2, 1, 1, 1, "9",  '9',  K_NUM }, { 3, 1, 1, 1, "-",  '-',  K_OP  },
    { 0, 2, 1, 1, "4",  '4',  K_NUM }, { 1, 2, 1, 1, "5",  '5',  K_NUM },
    { 2, 2, 1, 1, "6",  '6',  K_NUM }, { 3, 2, 1, 1, "+",  '+',  K_OP  },
    { 0, 3, 1, 1, "1",  '1',  K_NUM }, { 1, 3, 1, 1, "2",  '2',  K_NUM },
    { 2, 3, 1, 1, "3",  '3',  K_NUM }, { 3, 3, 1, 2, "=",  '\n', K_OP  },
    { 0, 4, 2, 1, "0",  '0',  K_NUM }, { 2, 4, 1, 1, ".",  '.',  K_NUM },
};
#define NBTN (int)(sizeof(BTN) / sizeof(BTN[0]))

/* Compute the grid origin/cell size inside a content rect. */
static void grid(int cx, int cy, int cw, int ch, int* gx, int* gy, int* cwd, int* chd, int* disp_h) {
    int dh = 78;                      /* display height */
    int pad = 10, gap = 8;
    *disp_h = dh;
    *gx = cx + pad;
    *gy = cy + dh;
    int gw = cw - 2 * pad;
    int gh = ch - dh - pad;
    *cwd = (gw - 3 * gap) / 4;
    *chd = (gh - 4 * gap) / 5;
}

static void calc_draw(int id, int ccx, int ccy, int ccw, int cch) {
    (void)ccx; (void)ccy; (void)ccw; (void)cch;
    int cx, cy, cw, ch;
    if (!appui_content(id, &cx, &cy, &cw, &ch)) return;
    appui_px_fill(cx, cy, cw, ch, APPUI_BG);

    int gx, gy, cwd, chd, dh;
    grid(cx, cy, cw, ch, &gx, &gy, &cwd, &chd, &dh);

    /* Display: current expression (or result) right-aligned, big */
    const char* text = expr_len ? expr : (shown[0] ? shown : "0");
    int tw = appui_px_text_w(text, APPUI_F_TITLE);
    int font = APPUI_F_TITLE;
    if (tw > cw - 24) { font = APPUI_F_BOLD; tw = appui_px_text_w(text, font); }
    appui_px_text(cx + cw - 16 - tw, cy + dh - 16 - appui_px_font_h(font) / 2 + 6, text, font, APPUI_TEXT);
    if (expr_len && (system_ticks % 16 < 8))
        ui_blend_rect(cx + cw - 14, cy + dh - 44, 2, 30, APPUI_ACCENT, 255);

    /* Buttons */
    int gap = 8;
    for (int i = 0; i < NBTN; i++) {
        const btn_t* b = &BTN[i];
        int bx = gx + b->c * (cwd + gap);
        int by = gy + b->r * (chd + gap);
        int bw = b->cs * cwd + (b->cs - 1) * gap;
        int bh = b->rs * chd + (b->rs - 1) * gap;
        uint32_t fill = b->kind == K_OP ? APPUI_ORANGE : b->kind == K_TOP ? 0x6E6E76 : 0x3A3A3E;
        uint32_t txt  = b->kind == K_TOP ? 0x1E1E20 : 0xFFFFFF;
        int rad = bh / 2 > 16 ? 14 : bh / 2;
        ui_rrect(bx, by, bw, bh, rad, fill, 255);
        int lf = (b->label[0] >= '0' && b->label[0] <= '9') ? APPUI_F_TITLE : APPUI_F_BOLD;
        appui_px_text_c(bx + bw / 2, by + bh / 2 - appui_px_font_h(lf) / 2 + 2, b->label, lf, txt);
    }
}

static void calc_key(int id, char key) { (void)id; feed(key); }

static void calc_click(int id, int px, int py) {
    int cx, cy, cw, ch;
    if (!appui_content(id, &cx, &cy, &cw, &ch)) return;
    /* click arrives content-relative; convert grid to content-relative too */
    int gx, gy, cwd, chd, dh, gap = 8;
    grid(0, 0, cw, ch, &gx, &gy, &cwd, &chd, &dh);
    for (int i = 0; i < NBTN; i++) {
        const btn_t* b = &BTN[i];
        int bx = gx + b->c * (cwd + gap), by = gy + b->r * (chd + gap);
        int bw = b->cs * cwd + (b->cs - 1) * gap, bh = b->rs * chd + (b->rs - 1) * gap;
        if (px >= bx && px < bx + bw && py >= by && py < by + bh) { feed(b->key); return; }
    }
}

int calculator_open(void) {
    expr[0] = 0; expr_len = 0; shown[0] = 0; have_result = false;
    int id = window_create("Calculator", 44, 3, 30, 24, calc_draw, calc_key);
    window_set_on_click(id, calc_click);
    return id;
}
