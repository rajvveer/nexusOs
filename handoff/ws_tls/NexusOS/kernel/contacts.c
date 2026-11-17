/* ============================================================================
 * NexusOS — Contacts App (Implementation)
 * ============================================================================ */

#include "contacts.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "appui.h"

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
    appui_theme_t ui = appui_theme();

    int total = 0; for (int i = 0; i < CT_MAX; i++) if (ct_list[i].used) total++;
    char hdr[24]; char n[4]; int_to_str(total, n);
    strcpy(hdr, n); strcat(hdr, " contacts");
    appui_fill(cx, cy, cw, ch, ui.panel);
    appui_header(cx, cy, cw, "Contacts", hdr);

    int row = cy + 3;
    if (ct_adding) {
        appui_text(cx + 1, row, ct_field == 0 ? "Name" : "Info", cw - 2, ui.accent); row += 2;
        appui_input(cx + 1, row, cw - 2, ct_buf, ct_blen, true);
        appui_status(cx, cy + ch - 1, cw, "Enter Next   Esc Cancel");
    } else {
        int vis = 0;
        for (int i = 0; i < CT_MAX && row < cy + ch - 2; i++) {
            if (!ct_list[i].used) continue;
            bool sel = (vis == ct_sel);
            uint8_t col = sel ? ui.selected : ui.text;
            appui_row(cx, row, cw, sel, ui.panel, ui.selected);
            gui_putchar(cx + 1, row, '@', sel ? ui.selected : ui.good);
            appui_text(cx + 3, row, ct_list[i].name, cw - 4, col); row++;
            if (sel) {
                appui_text(cx + 3, row, ct_list[i].info, cw - 4, ui.muted);
                row++;
            }
            vis++;
        }
        if (total == 0) appui_text(cx + 2, row, "No contacts yet", cw - 4, ui.muted);
        appui_status(cx, cy + ch - 1, cw, "A Add Contact   D Delete");
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
    return window_create("Contacts", 18, 4, 42, 18, ct_draw, ct_key);
}
