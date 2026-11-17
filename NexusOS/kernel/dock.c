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
#ifdef NEXUS_AURORA
static int magnify_percent = 44;   /* % growth of the icon under the pointer (0: reduce motion) */
void dock_set_magnify(int percent) { magnify_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
#define MAGNIFY magnify_percent
#else
#define MAGNIFY  44     /* % growth of the icon under the pointer */
#endif
#define SEP_W    21     /* separator slot before the Trash */
#define MAX_ITEMS 40

typedef struct { int x, y, s, app; } slot_t;
static slot_t slots[MAX_ITEMS];
static int    nslots = 0;
static int    hot = -1;            /* icon under the pointer */
static int    last_mx = -1;
static bool   was_inside = false;
static bool (*running_query)(const ui_app_t* app);
static bool trash_separator;
void dock_set_running_query(bool (*query)(const ui_app_t* app)) { running_query = query; }
#ifdef NEXUS_AURORA
/* Aurora: items after the fixed dock (running apps without a dock icon, minimized windows), drawn by the desktop. The
 * fixed part keeps its centred position; the extras extend the shelf to the right after a separator. */
static int (*extra_count)(void);
static void (*extra_draw)(int index, int x, int y, int size);
static const char* (*extra_name)(int index);
static bool (*extra_running)(int index);
static int extras;
void dock_set_extra(int (*count)(void), void (*draw)(int, int, int, int), const char* (*name)(int), bool (*running)(int)) {
    extra_count = count; extra_draw = draw; extra_name = name; extra_running = running;
}
static const char* dock_extra_name(int k) { const char* n = extra_name ? extra_name(k) : NULL; return n ? n : ""; }
static int key_focus = -1;   /* keyboard focus (Ctrl+F3, dock_key), -1: none */
#else
static const char* dock_extra_name(int k) { (void)k; return ""; }
#endif

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
    if (running_query) return running_query(a);
#ifndef NEXUS_AURORA
    if (!a->title) return false;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = window_get(i);
        if (w && !strcmp(w->title, a->title)) return true;
    }
#endif
    return false;
}

/* Lay the icons out for pointer x (or no magnification when mx < 0). */
static void layout(int mx, int my) {
    int list[MAX_ITEMS], n = dock_items(list);
    int count; const ui_app_t* apps = ui_apps(&count);
    trash_separator = n > 0 && apps[list[n - 1]].icon == ICON_TRASH;
    int sw = (int)fb_get_width();
    int base_w = n * ICON + (n - 1) * GAP + (trash_separator ? SEP_W : 0);
    int bx = sw / 2 - base_w / 2;
    bool magnify = mx >= 0 && my >= dock_top() - 30;
    int total = 0;
    for (int i = 0; i < n; i++) {
        int s = ICON;
        if (magnify) {
            int c = bx + i * (ICON + GAP) + (trash_separator && i == n - 1 ? SEP_W : 0) + ICON / 2;
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
    total += (n - 1) * GAP + (trash_separator ? SEP_W : 0);
    int x = sw / 2 - total / 2, bottom = dock_top() + PADY + ICON;
    for (int i = 0; i < n; i++) {
        if (trash_separator && i == n - 1) x += SEP_W;
        slots[i].x = x;
        slots[i].y = bottom - slots[i].s;
        x += slots[i].s + GAP;
    }
    nslots = n;
#ifdef NEXUS_AURORA
    extras = extra_count ? extra_count() : 0;
    if (extras > MAX_ITEMS - n) extras = MAX_ITEMS - n;
    if (extras > 0) x += SEP_W - GAP / 2;   /* separator, then the extras */
    for (int k = 0; k < extras; k++) {
        int s = ICON, i = n + k;
        if (magnify) {
            int c = x + ICON / 2, d = mx - c; if (d < 0) d = -d;
            int reach = (ICON + GAP) * 5 / 2;
            if (d < reach) { int t = 256 - d * 256 / reach; s += ICON * MAGNIFY / 100 * t / 256 * t / 256; }
        }
        slots[i] = (slot_t){x, bottom - s, s, -1 - k};
        x += s + GAP;
    }
    nslots = n + extras;
#endif
}
#ifdef NEXUS_AURORA
#define EXTRA(i) (slots[i].app < 0)
#else
#define EXTRA(i) false
#endif

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
    if (trash_separator) ui_blend_rect(sx, top + 12, 1, h - 24, 0xFFFFFF, 60);
#ifdef NEXUS_AURORA
    for (int i = 1; i < nslots; i++) if (EXTRA(i) && !EXTRA(i - 1)) ui_blend_rect(slots[i].x - SEP_W / 2 - GAP / 4, top + 12, 1, h - 24, 0xFFFFFF, 60);
#endif

    hot = -1;
    for (int i = 0; i < nslots; i++) {
#ifdef NEXUS_AURORA
        if (EXTRA(i)) {
            int k = -1 - slots[i].app; extra_draw(k, slots[i].x, slots[i].y, slots[i].s);
            if (extra_running && extra_running(k)) ui_circle(slots[i].x + slots[i].s / 2, top + h - 4, 2, UI_RGB(225, 225, 230), 255);
            if (mx >= slots[i].x && mx < slots[i].x + slots[i].s && my >= slots[i].y && my < top + h) hot = i;
            continue;
        }
#endif
        const ui_app_t* a = &apps[slots[i].app];
        ui_icon(a->icon, slots[i].x, slots[i].y, slots[i].s);
        if (app_running(a)) ui_circle(slots[i].x + slots[i].s / 2, top + h - 4, 2, UI_RGB(225, 225, 230), 255);
        if (mx >= slots[i].x && mx < slots[i].x + slots[i].s && my >= slots[i].y && my < top + h) hot = i;
    }

#ifdef NEXUS_AURORA
    if (key_focus >= nslots) key_focus = nslots - 1;
    if (key_focus >= 0) {   /* the keyboard focus: a ring, and its name bubble */
        hot = key_focus;
        for (int r = 3; r <= 4; r++) ui_rrect_stroke(slots[hot].x - r, slots[hot].y - r, slots[hot].s + r * 2, slots[hot].s + r * 2, 12 + r, UI_RGB(10, 132, 255), 255);
    }
#endif
    /* Name bubble above the hovered icon */
    if (hot >= 0) {
        const char* name = EXTRA(hot) ? dock_extra_name(-1 - slots[hot].app) : apps[slots[hot].app].name;
        int tw = ui_text_width(name, UF_UI), bw = tw + 24, bh = 28;
        int cx = slots[hot].x + slots[hot].s / 2, by = slots[hot].y - bh - 12;
        ui_shadow(cx - bw / 2, by, bw, bh, 8, 10, 80, 3);
        ui_rrect(cx - bw / 2, by, bw, bh, 8, UI_RGB(44, 44, 48), 240);
        ui_rrect_stroke(cx - bw / 2, by, bw, bh, 8, UI_RGB(255, 255, 255), 40);
        ui_text(cx - tw / 2, by + (bh - ui_font_height(UF_UI)) / 2 + 1, name, UF_UI, UI_RGB(236, 236, 240));
    }
}

#ifdef NEXUS_AURORA
/* Everything dock_draw(mx,my) may touch: shelf shadow, magnified icons, name bubble. */
bool dock_bounds(int mx, int my, int* bx, int* by, int* bw, int* bh) {
    int count;
    const ui_app_t* apps = ui_apps(&count);
    layout(mx, my);
    if (!nslots) return false;
    int top = dock_top(), h = shelf_h();
    int x0 = slots[0].x - PADX - 17, x1 = slots[nslots - 1].x + slots[nslots - 1].s + PADX + 17;
    int y0 = top - 13, y1 = top + h + 21;
    for (int i = 0; i < nslots; i++) {
        if (slots[i].y < y0) y0 = slots[i].y;
        if ((mx >= slots[i].x && mx < slots[i].x + slots[i].s && my >= slots[i].y && my < top + h) || i == key_focus) {
            int bw2 = ui_text_width(EXTRA(i) ? dock_extra_name(-1 - slots[i].app) : apps[slots[i].app].name, UF_UI) + 24, cx = slots[i].x + slots[i].s / 2;
            int by2 = slots[i].y - 28 - 12;
            if (cx - bw2 / 2 - 11 < x0) x0 = cx - bw2 / 2 - 11;
            if (cx + bw2 / 2 + 11 > x1) x1 = cx + bw2 / 2 + 11;
            if (by2 - 8 < y0) y0 = by2 - 8;
        }
    }
    *bx = x0 - 2; *by = y0 - 2; *bw = x1 - x0 + 4; *bh = y1 - y0 + 4;
    return true;
}
/* Keyboard access (Ctrl+F3) and the screen reader's view (aurora_a11y.c), in shelf order: dock apps, Trash, extras. */
int dock_a11y_count(void) {
    int list[MAX_ITEMS], n = dock_items(list), e = extra_count ? extra_count() : 0;
    return n + (e > MAX_ITEMS - n ? MAX_ITEMS - n : e);
}
const char* dock_a11y_name(int i, bool* running) {
    int list[MAX_ITEMS], n = dock_items(list), count; const ui_app_t* apps = ui_apps(&count);
    if (i < 0 || i >= dock_a11y_count()) { *running = false; return ""; }
    if (i >= n) { *running = extra_running && extra_running(i - n); return dock_extra_name(i - n); }
    *running = app_running(&apps[list[i]]); return apps[list[i]].name;
}
int dock_a11y_focus(void) { return key_focus; }
int dock_key(char key) {
    unsigned char k = (unsigned char)key; int count = dock_a11y_count();
    if (!k) { key_focus = count ? 0 : -1; return 0; }   /* enter the dock */
    if (key_focus < 0 || !count) return 0;
    if (k == 27) key_focus = -1;
    else if (k == 0x82 || k == 0x83) key_focus = (key_focus + (k == 0x83 ? 1 : count - 1)) % count;
    else if (k == 0x9D || k == 0x9E) key_focus = k == 0x9D ? 0 : count - 1;
    else if (k == '\n' || k == ' ') {
        int list[MAX_ITEMS], n = dock_items(list), c, i = key_focus; const ui_app_t* apps = ui_apps(&c);
        key_focus = -1; return i >= n ? DOCK_EXTRA_ACTION + (i - n) : apps[list[i]].action;
    }
    return 0;
}
#endif

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
            return EXTRA(i) ? DOCK_EXTRA_ACTION + (-1 - slots[i].app) : apps[slots[i].app].action;
    return 0;
}

bool dock_hover_changed(int px, int py) {
    bool inside = py >= dock_top() - 30 && dock_hit(px, py < dock_top() ? dock_top() : py);
    bool changed = inside ? (px != last_mx || !was_inside) : was_inside;
    was_inside = inside;
    last_mx = inside ? px : -1;
    return changed;
}
