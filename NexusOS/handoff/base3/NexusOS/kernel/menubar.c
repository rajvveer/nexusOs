/* ============================================================================
 * NexusOS — Menu bar (Implementation)
 * ============================================================================ */

#include "menubar.h"
#include "macui.h"
#include "framebuffer.h"
#include "string.h"
#include "net.h"

typedef struct { const char* label; int action; const char* key; } mitem_t;   /* label NULL = separator */
typedef struct { const char* title; const mitem_t* items; int count; } menu_t;

#define DIS (-1)   /* disabled item */

static const mitem_t M_NEXUS[] = {
    { "About This NexusOS", UI_ACT_ABOUT, NULL },
    { NULL, 0, NULL },
    { "System Settings...", 5, NULL },
    { "Appearance...", 29, NULL },
    { NULL, 0, NULL },
    { "Force Quit...", 11, NULL },
    { NULL, 0, NULL },
    { "Sleep", UI_ACT_SLEEP, NULL },
    { "Restart...", UI_ACT_RESTART, NULL },
    { "Shut Down...", UI_ACT_SHUTDOWN, NULL },
    { NULL, 0, NULL },
    { "Lock Screen", UI_ACT_LOCK, "^L" },
    { "Exit to Console", UI_ACT_CONSOLE, NULL },
};
static const mitem_t M_APP[] = {
    { "About This NexusOS", UI_ACT_ABOUT, NULL },
    { NULL, 0, NULL },
    { "Hide", UI_ACT_MINIMIZE, NULL },
    { "Quit", UI_ACT_CLOSEWIN, "^W" },
};
static const mitem_t M_FILE[] = {
    { "New Terminal Window", 1, "^T" },
    { "New File on Desktop", UI_ACT_NEWFILE, NULL },
    { "Open Finder", 2, "^F" },
    { NULL, 0, NULL },
    { "Close Window", UI_ACT_CLOSEWIN, "^W" },
};
static const mitem_t M_EDIT[] = {
    { "Undo", DIS, NULL },
    { "Redo", DIS, NULL },
    { NULL, 0, NULL },
    { "Cut", DIS, NULL },
    { "Copy", DIS, NULL },
    { "Paste", DIS, "^V" },
    { NULL, 0, NULL },
    { "Clipboard History...", 28, NULL },
};
static const mitem_t M_VIEW[] = {
    { "Enter Full Screen", UI_ACT_ZOOM, NULL },
    { NULL, 0, NULL },
    { "Notification Center", UI_ACT_NOTIFS, "^N" },
    { "Keyboard Shortcuts", UI_ACT_SHORTCUTS, "F1" },
};
static const mitem_t M_GO[] = {
    { "Applications", UI_ACT_LAUNCHPAD, NULL },
    { "Spotlight Search", UI_ACT_SPOTLIGHT, "^Space" },
    { NULL, 0, NULL },
    { "Finder", 2, NULL },
    { "Web", 32, NULL },
    { "Terminal", 1, NULL },
};
static const mitem_t M_WINDOW[] = {
    { "Minimize", UI_ACT_MINIMIZE, NULL },
    { "Zoom", UI_ACT_ZOOM, NULL },
    { NULL, 0, NULL },
    { "Activity Monitor", 4, "^S" },
};
static const mitem_t M_HELP[] = {
    { "Tips", 14, "F2" },
    { "Keyboard Shortcuts", UI_ACT_SHORTCUTS, "F1" },
};

#define N(a) (int)(sizeof(a) / sizeof(a[0]))
static const menu_t MENUS[] = {
    { "",       M_NEXUS,  N(M_NEXUS) },     /* logo */
    { NULL,     M_APP,    N(M_APP) },       /* app name */
    { "File",   M_FILE,   N(M_FILE) },
    { "Edit",   M_EDIT,   N(M_EDIT) },
    { "View",   M_VIEW,   N(M_VIEW) },
    { "Go",     M_GO,     N(M_GO) },
    { "Window", M_WINDOW, N(M_WINDOW) },
    { "Help",   M_HELP,   N(M_HELP) },
};
#define MENU_COUNT N(MENUS)

#define ITEM_H 24
#define SEP_H  10
#define PAD_V  6

static int  title_x[MENU_COUNT], title_w[MENU_COUNT];
static int  open_menu = -1, hot_item = -1;
static int  st_clock_x = 0, st_search_x = 0, st_cc_x = 0;   /* status item left edges */
static char app_label[32] = "Finder";

/* --- dropdown geometry --- */
static int menu_width(int m) {
    int w = 200;
    for (int i = 0; i < MENUS[m].count; i++) {
        const mitem_t* it = &MENUS[m].items[i];
        if (!it->label) continue;
        int iw = ui_text_width(it->label, UF_UI) + 40 + (it->key ? ui_text_width(it->key, UF_UI) + 24 : 0);
        if (iw > w) w = iw;
    }
    return w;
}
static int menu_height(int m) {
    int h = PAD_V * 2;
    for (int i = 0; i < MENUS[m].count; i++) h += MENUS[m].items[i].label ? ITEM_H : SEP_H;
    return h;
}
static int menu_x(int m) {
    int x = title_x[m] - 4, w = menu_width(m), sw = (int)fb_get_width();
    if (x + w > sw - 6) x = sw - 6 - w;
    return x;
}
#define MENU_Y (MENUBAR_H + 2)

static int item_at(int m, int px, int py) {
    int x = menu_x(m), w = menu_width(m);
    if (px < x || px >= x + w) return -1;
    int y = MENU_Y + PAD_V;
    for (int i = 0; i < MENUS[m].count; i++) {
        int h = MENUS[m].items[i].label ? ITEM_H : SEP_H;
        if (py >= y && py < y + h) return MENUS[m].items[i].label ? i : -1;
        y += h;
    }
    return -1;
}

/* --- status icons --- */
static void icon_wifi(int x, int cy, bool up) {
    uint32_t c = up ? 0xFFFFFF : UI_RGB(120, 120, 126);
    int bx = x + 9, by = cy + 7;
    ui_circle(bx, by - 1, 2, c, 255);
    ui_arc_up(bx, by, 8, 2, c, 255);
    ui_arc_up(bx, by, 13, 2, c, up ? 255 : 160);
}
static void icon_search(int x, int cy) {
    ui_ring(x + 7, cy - 1, 6, 2, 0xFFFFFF, 255);
    ui_line(x + 11, cy + 3, x + 15, cy + 7, 2, 0xFFFFFF, 255);
}
static void icon_cc(int x, int cy) {
    ui_rrect(x, cy - 7, 20, 7, 3, 0xFFFFFF, 255);
    ui_circle(x + 15, cy - 4, 2, UI_RGB(40, 40, 44), 255);
    ui_rrect(x, cy + 1, 20, 7, 3, 0xFFFFFF, 255);
    ui_circle(x + 4, cy + 4, 2, UI_RGB(40, 40, 44), 255);
}

void menubar_draw(const char* app_name) {
    int sw = (int)fb_get_width();
    if (app_name && app_name[0]) { strncpy(app_label, app_name, sizeof(app_label) - 1); app_label[sizeof(app_label) - 1] = 0; }

    /* Translucent dark bar over the wallpaper */
    ui_wallpaper_rect(0, 0, sw, MENUBAR_H);
    ui_blend_rect(0, 0, sw, MENUBAR_H, UI_RGB(22, 22, 28), 120);
    ui_blend_rect(0, MENUBAR_H - 1, sw, 1, 0x000000, 60);

    int ty = (MENUBAR_H - ui_font_height(UF_UI)) / 2 + 1;
    int x = 12;
    for (int m = 0; m < MENU_COUNT; m++) {
        const char* t = MENUS[m].title ? MENUS[m].title : app_label;
        int font = MENUS[m].title ? UF_UI : UF_UIB;
        int w = m == 0 ? 18 : ui_text_width(t, font);
        title_x[m] = x;
        title_w[m] = w + 20;
        if (open_menu == m) ui_rrect(x, 3, w + 20, MENUBAR_H - 6, 5, 0xFFFFFF, 46);
        if (m == 0) ui_logo(x + 10, 7, 14, 0xFFFFFF);
        else ui_text(x + 10, ty, t, font, 0xFFFFFF);
        x += w + 20;
    }

    /* Right side: [wifi] [search] [control centre]  Thu 8 Oct  12:40 */
    char clk[32];
    ui_format_clock(clk);
    int cw = ui_text_width(clk, UF_UI);
    st_clock_x = sw - 16 - cw;
    ui_text(st_clock_x, ty, clk, UF_UI, 0xFFFFFF);
    st_cc_x = st_clock_x - 40;
    icon_cc(st_cc_x + 4, MENUBAR_H / 2);
    st_search_x = st_cc_x - 36;
    icon_search(st_search_x + 6, MENUBAR_H / 2);
    bool up = net_device_count() > 0 && net_get_device(0)->link_up;
    icon_wifi(st_search_x - 36, MENUBAR_H / 2 - 2, up);
}

void menubar_draw_menu(void) {
    if (open_menu < 0) return;
    int m = open_menu, x = menu_x(m), w = menu_width(m), h = menu_height(m);
    ui_shadow(x, MENU_Y, w, h, 8, 18, 110, 6);
    ui_rrect(x, MENU_Y, w, h, 8, UI_RGB(42, 42, 46), 246);
    ui_rrect_stroke(x, MENU_Y, w, h, 8, UI_RGB(110, 110, 116), 120);
    int y = MENU_Y + PAD_V;
    for (int i = 0; i < MENUS[m].count; i++) {
        const mitem_t* it = &MENUS[m].items[i];
        if (!it->label) {
            ui_blend_rect(x + 12, y + SEP_H / 2, w - 24, 1, 0xFFFFFF, 40);
            y += SEP_H;
            continue;
        }
        bool dis = it->action == DIS;
        if (i == hot_item && !dis) ui_rrect(x + 5, y, w - 10, ITEM_H, 5, UI_RGB(10, 100, 230), 255);
        uint32_t tc = dis ? UI_RGB(120, 120, 126) : 0xFFFFFF;
        int ty = y + (ITEM_H - ui_font_height(UF_UI)) / 2 + 1;
        ui_text(x + 20, ty, it->label, UF_UI, tc);
        if (it->key) {
            int kw = ui_text_width(it->key, UF_UI);
            ui_text(x + w - 16 - kw, ty, it->key, UF_UI, i == hot_item ? 0xFFFFFF : UI_RGB(150, 150, 156));
        }
        y += ITEM_H;
    }
}

bool menubar_hit(int px, int py) { (void)px; return py >= 0 && py < MENUBAR_H; }
bool menubar_menu_open(void) { return open_menu >= 0; }
void menubar_close(void) { open_menu = -1; hot_item = -1; }

bool menubar_menu_hit(int px, int py) {
    if (open_menu < 0) return false;
    int x = menu_x(open_menu);
    return px >= x && px < x + menu_width(open_menu) && py >= MENU_Y && py < MENU_Y + menu_height(open_menu);
}

int menubar_click(int px, int py) {
    if (!menubar_hit(px, py)) return 0;
    for (int m = 0; m < MENU_COUNT; m++) {
        if ((px >= title_x[m] || m == 0) && px < title_x[m] + title_w[m]) {   /* logo reaches the corner */
            if (open_menu == m) menubar_close();
            else { open_menu = m; hot_item = -1; }
            return 0;
        }
    }
    menubar_close();
    if (px >= st_clock_x) return UI_ACT_NOTIFS;
    if (px >= st_cc_x && px < st_cc_x + 32) return 5;
    if (px >= st_search_x && px < st_search_x + 30) return UI_ACT_SPOTLIGHT;
    return 0;
}

int menubar_menu_click(int px, int py) {
    if (open_menu < 0) return 0;
    int i = item_at(open_menu, px, py);
    if (i < 0) return 0;
    int a = MENUS[open_menu].items[i].action;
    if (a == DIS) return 0;
    menubar_close();
    return a;
}

bool menubar_hover(int px, int py) {
    if (open_menu < 0) return false;
    if (menubar_hit(px, py)) {                     /* slide across titles like macOS */
        for (int m = 0; m < MENU_COUNT; m++)
            if (px >= title_x[m] && px < title_x[m] + title_w[m] && m != open_menu) {
                open_menu = m; hot_item = -1; return true;
            }
        return false;
    }
    int i = item_at(open_menu, px, py);
    if (i >= 0 && MENUS[open_menu].items[i].action == DIS) i = -1;
    if (i != hot_item) { hot_item = i; return true; }
    return false;
}

int menubar_handle_key(char key) {
    if (open_menu < 0) return 0;
    unsigned char k = (unsigned char)key;
    const menu_t* mn = &MENUS[open_menu];
    if (k == 27) { menubar_close(); return 0; }
    if (k == 0x82 || k == 0x83) {
        open_menu = (open_menu + (k == 0x83 ? 1 : MENU_COUNT - 1)) % MENU_COUNT;
        hot_item = -1;
        return 0;
    }
    if (k == 0x80 || k == 0x81) {
        int step = k == 0x81 ? 1 : -1, i = hot_item;
        for (int n = 0; n < mn->count; n++) {
            i = (i + step + mn->count) % mn->count;
            if (mn->items[i].label && mn->items[i].action != DIS) { hot_item = i; break; }
        }
        return 0;
    }
    if (key == '\n' && hot_item >= 0) {
        int a = mn->items[hot_item].action;
        menubar_close();
        return a == DIS ? 0 : a;
    }
    return 0;
}
