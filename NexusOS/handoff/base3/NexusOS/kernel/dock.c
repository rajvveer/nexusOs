/* ============================================================================
 * NexusOS — Dock (Implementation)
 * ============================================================================ */

#include "dock.h"
#include "macui.h"
#include "window.h"
#include "framebuffer.h"
#include "string.h"

#define ICON     56     /* resting icon size */
#define GAP      8      /* space between icons */
#define PADX     10     /* shelf padding left/right */
#define PADY     8      /* shelf padding top/bottom */
#define MARGIN   6      /* gap between shelf and screen bottom */
#define MAGNIFY  44     /* % growth of the icon under the pointer */
#define SEP_W    21     /* separator slot before the Trash */
#define MAX_ITEMS 16

typedef struct { int x, y, s, app; } slot_t;
static slot_t slots[MAX_ITEMS];
static int    nslots = 0;
static int    hot = -1;            /* icon under the pointer */
static int    last_mx = -1;
static bool   was_inside = false;

void dock_init(void) { hot = -1; last_mx = -1; was_inside = false; }

static int shelf_h(void) { return ICON + PADY * 2; }
int dock_top(void) { return (int)fb_get_height() - MARGIN - shelf_h(); }

/* Indices into the app registry shown in the dock: the dock apps + Trash. */
static int dock_items(int* list) {
    int count, n = 0;
    const ui_app_t* apps = ui_apps(&count);
    for (int i = 0; i < ui_dock_app_count() && n < MAX_ITEMS - 1; i++) list[n++] = i;
    for (int i = 0; i < count; i++) if (apps[i].icon == ICON_TRASH) { list[n++] = i; break; }
    return n;
}

static bool app_running(const ui_app_t* a) {
    if (!a->title) return false;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = window_get(i);
        if (w && !strcmp(w->title, a->title)) return true;
    }
    return false;
}

/* Lay the icons out for pointer x (or no magnification when mx < 0). */
static void layout(int mx, int my) {
    int list[MAX_ITEMS], n = dock_items(list);
    int sw = (int)fb_get_width();
    int base_w = n * ICON + (n - 1) * GAP + SEP_W;
    int bx = sw / 2 - base_w / 2;
    bool magnify = mx >= 0 && my >= dock_top() - 30;
    int total = 0;
    for (int i = 0; i < n; i++) {
        int s = ICON;
        if (magnify) {
            int c = bx + i * (ICON + GAP) + (i == n - 1 ? SEP_W : 0) + ICON / 2;
            int d = mx - c; if (d < 0) d = -d;
            int reach = (ICON + GAP) * 5 / 2;
            if (d < reach) {
                int t = 256 - d * 256 / reach;             /* 0..256 */
                s += ICON * MAGNIFY / 100 * t / 256 * t / 256;
            }
        }
        slots[i].s = s;
        slots[i].app = list[i];
        total += s;
    }
    total += (n - 1) * GAP + SEP_W;
    int x = sw / 2 - total / 2, bottom = dock_top() + PADY + ICON;
    for (int i = 0; i < n; i++) {
        if (i == n - 1) x += SEP_W;
        slots[i].x = x;
        slots[i].y = bottom - slots[i].s;
        x += slots[i].s + GAP;
    }
    nslots = n;
}

void dock_draw(int mx, int my) {
    int count;
    const ui_app_t* apps = ui_apps(&count);
    layout(mx, my);
    if (!nslots) return;
    int top = dock_top(), h = shelf_h();
    int x0 = slots[0].x - PADX, x1 = slots[nslots - 1].x + slots[nslots - 1].s + PADX;

    /* Frosted shelf */
    ui_shadow(x0, top, x1 - x0, h, 18, 16, 70, 4);
    ui_glass(x0, top, x1 - x0, h, 18, UI_RGB(34, 34, 42), 150);
    ui_rrect_stroke(x0, top, x1 - x0, h, 18, UI_RGB(255, 255, 255), 48);
    ui_blend_rect(x0 + 18, top + 1, x1 - x0 - 36, 1, 0xFFFFFF, 26);

    /* Separator before the Trash */
    int sx = slots[nslots - 1].x - SEP_W / 2 - GAP / 2;
    ui_blend_rect(sx, top + 12, 1, h - 24, 0xFFFFFF, 60);

    hot = -1;
    for (int i = 0; i < nslots; i++) {
        const ui_app_t* a = &apps[slots[i].app];
        ui_icon(a->icon, slots[i].x, slots[i].y, slots[i].s);
        if (app_running(a)) ui_circle(slots[i].x + slots[i].s / 2, top + h - 4, 2, UI_RGB(225, 225, 230), 255);
        if (mx >= slots[i].x && mx < slots[i].x + slots[i].s && my >= slots[i].y && my < top + h) hot = i;
    }

    /* Name bubble above the hovered icon */
    if (hot >= 0) {
        const char* name = apps[slots[hot].app].name;
        int tw = ui_text_width(name, UF_UI), bw = tw + 24, bh = 28;
        int cx = slots[hot].x + slots[hot].s / 2, by = slots[hot].y - bh - 12;
        ui_shadow(cx - bw / 2, by, bw, bh, 8, 10, 80, 3);
        ui_rrect(cx - bw / 2, by, bw, bh, 8, UI_RGB(44, 44, 48), 240);
        ui_rrect_stroke(cx - bw / 2, by, bw, bh, 8, UI_RGB(255, 255, 255), 40);
        ui_text(cx - tw / 2, by + (bh - ui_font_height(UF_UI)) / 2 + 1, name, UF_UI, UI_RGB(236, 236, 240));
    }
}

bool dock_hit(int px, int py) {
    if (!nslots) layout(-1, -1);
    if (py < dock_top() || !nslots) return false;
    return px >= slots[0].x - PADX && px < slots[nslots - 1].x + slots[nslots - 1].s + PADX;
}

int dock_click(int px, int py) {
    int count;
    const ui_app_t* apps = ui_apps(&count);
    layout(px, py);
    for (int i = 0; i < nslots; i++)
        if (px >= slots[i].x - GAP / 2 && px < slots[i].x + slots[i].s + GAP / 2 && py >= dock_top())
            return apps[slots[i].app].action;
    return 0;
}

bool dock_hover_changed(int px, int py) {
    bool inside = py >= dock_top() - 30 && dock_hit(px, py < dock_top() ? dock_top() : py);
    bool changed = inside ? (px != last_mx || !was_inside) : was_inside;
    was_inside = inside;
    last_mx = inside ? px : -1;
    return changed;
}
