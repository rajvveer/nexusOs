/* ============================================================================
 * NexusOS — Contacts App (Implementation)
 * ============================================================================ */

#include "contacts.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

#define CT_MAX 8
#define CT_NAME 20
#define CT_INFO 24

static struct { bool used; char name[CT_NAME]; char info[CT_INFO]; } ct_list[CT_MAX];
static int ct_sel = 0;
static bool ct_adding = false;
static int ct_field = 0; /* 0=name, 1=info */
static char ct_buf[CT_INFO];
static int ct_blen = 0;
static char ct_name_buf[CT_NAME];

static void ct_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content, bg = (tc >> 4) & 0xF;
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t hi = t->menu_highlight;

    int row = cy;
    int total = 0; for (int i = 0; i < CT_MAX; i++) if (ct_list[i].used) total++;
    char hdr[20]; strcpy(hdr, "\x02 Contacts [");
    char n[4]; int_to_str(total, n); strcat(hdr, n); strcat(hdr, "]");
    gui_draw_text(cx, row, hdr, accent); row++;
    for (int i = 0; i < cw - 1; i++) gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    if (ct_adding) {
        gui_draw_text(cx, row, ct_field == 0 ? "Name:" : "Info:", accent); row++;
        for (int i = 0; i < ct_blen; i++) gui_putchar(cx + 1 + i, row, ct_buf[i], tc);
        gui_putchar(cx + 1 + ct_blen, row, '_', VGA_COLOR(VGA_WHITE, bg)); row++;
        gui_draw_text(cx, row + 1, "Enter:next Esc:cancel", dim);
    } else {
        int vis = 0;
        for (int i = 0; i < CT_MAX && row < cy + ch - 2; i++) {
            if (!ct_list[i].used) continue;
            bool sel = (vis == ct_sel);
            uint8_t col = sel ? hi : tc;
            if (sel) for (int j = cx; j < cx + cw - 1; j++) gui_putchar(j, row, ' ', hi);
            gui_putchar(cx + 1, row, '\x02', VGA_COLOR(VGA_LIGHT_GREEN, sel ? ((hi >> 4) & 0xF) : bg));
            gui_draw_text(cx + 3, row, ct_list[i].name, col); row++;
            if (sel) {
                gui_draw_text(cx + 3, row, ct_list[i].info, VGA_COLOR(VGA_DARK_GREY, sel ? ((hi >> 4) & 0xF) : bg));
                row++;
            }
            vis++;
        }
        if (total == 0) gui_draw_text(cx + 2, row, "No contacts. A:Add", dim);
        gui_draw_text(cx, cy + ch - 1, "A:Add D:Delete", dim);
    }
    (void)cw; (void)ch;
}

static void ct_key(int id, char key) {
    (void)id;
    if (ct_adding) {
        if (key == 27) { ct_adding = false; ct_blen = 0; }
        else if (key == '\n') {
            ct_buf[ct_blen] = '\0';
            if (ct_field == 0) { strncpy(ct_name_buf, ct_buf, CT_NAME - 1); ct_name_buf[CT_NAME-1] = '\0'; ct_field = 1; ct_blen = 0; ct_buf[0] = '\0'; }
            else {
                for (int i = 0; i < CT_MAX; i++) {
                    if (!ct_list[i].used) {
                        ct_list[i].used = true;
                        strncpy(ct_list[i].name, ct_name_buf, CT_NAME - 1);
                        strncpy(ct_list[i].info, ct_buf, CT_INFO - 1);
                        break;
                    }
                }
                ct_adding = false; ct_blen = 0;
            }
        }
        else if (key == '\b' && ct_blen > 0) ct_buf[--ct_blen] = '\0';
        else if (key >= 32 && key < 127 && ct_blen < CT_INFO - 1) { ct_buf[ct_blen++] = key; ct_buf[ct_blen] = '\0'; }
        return;
    }
    if (key == 'a' || key == 'A') { ct_adding = true; ct_field = 0; ct_blen = 0; ct_buf[0] = '\0'; }
    if ((unsigned char)key == 0x80 && ct_sel > 0) ct_sel--;
    if ((unsigned char)key == 0x81) ct_sel++;
    if (key == 'd' || key == 'D') {
        int vis = 0;
        for (int i = 0; i < CT_MAX; i++) {
            if (!ct_list[i].used) continue;
            if (vis == ct_sel) { ct_list[i].used = false; break; } vis++;
        }
    }
}

int contacts_open(void) {
    ct_sel = 0; ct_adding = false;
    return window_create("Contacts", 18, 2, 30, 16, ct_draw, ct_key);
}
