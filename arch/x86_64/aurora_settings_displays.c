/* Settings > Displays: arrangement by drag (snapped edge to edge), main display, resolution, scale, mirroring and
 * on/off per display. Changes apply at once through the control socket ("apply-displays", saved by the desktop in
 * ~/.aurora/displays.conf); a resolution or scale change asks to be kept and reverts after 15 seconds otherwise.
 * Scales: 100, 125, 150, 175, 200 and 300 % (fractional ones through wp_fractional_scale_v1 + wp_viewporter). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aurora_settings.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

#define DISPLAYS 8
#define MODES 48
struct display {
    char name[32], model[40], mirror[32]; bool enabled, primary;
    int x, y, w, h, mode_w, mode_h, refresh, scale;
    struct { int w, h, refresh; bool preferred; } modes[MODES]; int mode_count;
};
static struct display displays[DISPLAYS], previous[DISPLAYS]; static int count, previous_count, selected;
static char backend[16];
static int area_x, area_y, area_w, area_h; static double factor; static int origin_x, origin_y;   /* arrangement view */
static int dragging = -1, drag_dx, drag_dy, drag_x, drag_y; static bool moved;
static uint64_t confirm_until;   /* a pending "keep these settings?" question */
static int rects[DISPLAYS][4];   /* last reported arrangement rectangles */
#define SCALES 6
static const int SCALE_VALUES[SCALES] = {100, 125, 150, 175, 200, 300};   /* percent; Aurora renders fractional scales natively */
static struct { int x, y, w, h; } hit_main, hit_res_prev, hit_res_next, hit_scale, hit_use, hit_enable, hit_keep, hit_revert, hit_select;

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static bool inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }
static void load(void) {
    char* text = malloc(65536); if (!text) return;
    if (!settings_request("displays\n", text, 65536)) { settings_message(text); free(text); return; }
    char selected_name[32] = ""; if (selected >= 0 && selected < count) snprintf(selected_name, sizeof(selected_name), "%s", displays[selected].name);
    count = 0; memset(displays, 0, sizeof(displays)); memset(rects, 0, sizeof(rects));
    for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "backend ", 8)) snprintf(backend, sizeof(backend), "%s", line + 8);
        else if (!strncmp(line, "output ", 7) && count < DISPLAYS) {
            struct display* d = &displays[count++]; char mirror[32] = "-", model[40] = "-"; int enabled = 0, primary = 0;
            sscanf(line, "output %31s enabled=%d primary=%d x=%d y=%d w=%d h=%d mode=%dx%d@%d scale=%d mirror=%31s", d->name, &enabled, &primary,
                   &d->x, &d->y, &d->w, &d->h, &d->mode_w, &d->mode_h, &d->refresh, &d->scale, mirror);
            char* m = strstr(line, " model="); if (m) sscanf(m, " model=%39s", model);
            d->enabled = enabled; d->primary = primary; snprintf(d->mirror, sizeof(d->mirror), "%s", strcmp(mirror, "-") ? mirror : "");
            snprintf(d->model, sizeof(d->model), "%s", strcmp(model, "-") ? model : "Display"); for (char* p = d->model; *p; p++) if (*p == '_') *p = ' ';
        } else if (!strncmp(line, "mode ", 5)) {
            char name[32]; int w, h, r;
            if (sscanf(line, "mode %31s %dx%d@%d", name, &w, &h, &r) != 4) continue;
            for (int i = 0; i < count; i++) if (!strcmp(displays[i].name, name) && displays[i].mode_count < MODES) {
                struct display* d = &displays[i]; d->modes[d->mode_count].w = w; d->modes[d->mode_count].h = h; d->modes[d->mode_count].refresh = r;
                d->modes[d->mode_count].preferred = strstr(line, "preferred") != NULL; d->mode_count++;
            }
        }
    }
    free(text);
    for (int i = 0; i < count; i++) {   /* largest first, then by refresh rate */
        struct display* d = &displays[i];
        for (int a = 1; a < d->mode_count; a++) for (int b = a; b > 0; b--) {
            int64_t p = (int64_t)d->modes[b - 1].w * d->modes[b - 1].h, q = (int64_t)d->modes[b].w * d->modes[b].h;
            if (p > q || (p == q && d->modes[b - 1].refresh >= d->modes[b].refresh)) break;
            typeof(d->modes[0]) swap = d->modes[b - 1]; d->modes[b - 1] = d->modes[b]; d->modes[b] = swap;
        }
    }
    selected = 0; for (int i = 0; i < count; i++) if (!strcmp(displays[i].name, selected_name)) selected = i;
    settings_redraw();
}
/* Apply the layout in displays[]; remember the previous one for a revert when ask is set. */
static bool apply(bool ask) {
    char request[4096]; size_t used = (size_t)snprintf(request, sizeof(request), "apply-displays\n");
    for (int i = 0; i < count && used < sizeof(request); i++) {
        struct display* d = &displays[i];
        used += (size_t)snprintf(request + used, sizeof(request) - used, "display %s enabled=%d primary=%d x=%d y=%d mode=%dx%d@%d scale=%d mirror=%s\n", d->name,
                                 d->enabled, d->primary, d->x, d->y, d->mode_w, d->mode_h, d->refresh, d->scale, d->mirror[0] ? d->mirror : "-");
    }
    if (used + 5 > sizeof(request)) { settings_message("Too many displays."); return false; }
    strcat(request, "end\n");
    char reply[512];
    if (!settings_request(request, reply, sizeof(reply))) { char text[600]; snprintf(text, sizeof(text), "Could not change the displays: %s", reply); settings_message(text); load(); return false; }
    printf("[64] SETTINGS DISPLAYS APPLIED%s\n", ask ? " (confirm)" : ""); fflush(stdout);
    if (ask) confirm_until = now_ms() + 15000; else confirm_until = 0;
    settings_message(ask ? "Keep these display settings? They revert in 15 seconds." : "Display settings saved.");
    load(); return true;
}
static void snapshot(void) { memcpy(previous, displays, sizeof(previous)); previous_count = count; }
static void revert(void) {
    confirm_until = 0; if (!previous_count) return;
    memcpy(displays, previous, sizeof(displays)); count = previous_count; apply(false);
    settings_message("The previous display settings were restored."); printf("[64] SETTINGS DISPLAYS REVERTED\n"); fflush(stdout);
}
static int mode_index(const struct display* d) {
    for (int k = 0; k < d->mode_count; k++) if (d->modes[k].w == d->mode_w && d->modes[k].h == d->mode_h && d->modes[k].refresh == d->refresh) return k;
    return -1;
}

static void enter(void) { confirm_until = 0; dragging = -1; load(); }
static void draw(int x, int y, int w, int h) {
    (void)h;
    appui_px_text(x + 20, y + 14, "Displays", APPUI_F_TITLE, APPUI_TEXT);
    if (!count) { appui_px_text(x + 20, y + 60, "No display information (is the desktop running?)", APPUI_F_BODY, APPUI_SUBTLE); return; }
    /* arrangement */
    area_x = x + 20; area_y = y + 50; area_w = w - 40; area_h = 230;
    appui_px_card(area_x, area_y, area_w, area_h, 10, APPUI_PANEL);
    int left = 1 << 30, top = 1 << 30, right = -(1 << 30), bottom = -(1 << 30);
    for (int i = 0; i < count; i++) { struct display* d = &displays[i]; if (!d->enabled || d->mirror[0]) continue;
        int dx = i == dragging ? drag_x : d->x, dy = i == dragging ? drag_y : d->y;
        if (dx < left) left = dx; if (dy < top) top = dy; if (dx + d->w > right) right = dx + d->w; if (dy + d->h > bottom) bottom = dy + d->h; }
    if (right <= left) { right = left + 1; bottom = top + 1; }
    double fx = (double)(area_w - 60) / (right - left), fy = (double)(area_h - 50) / (bottom - top);
    if (dragging < 0) { factor = fx < fy ? fx : fy; if (factor > 0.2) factor = 0.2;
        origin_x = area_x + (area_w - (int)((right - left) * factor)) / 2 - (int)(left * factor); origin_y = area_y + (area_h - (int)((bottom - top) * factor)) / 2 - (int)(top * factor); }
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < count; i++) {   /* the dragged one on top */
        struct display* d = &displays[i]; if (!d->enabled || d->mirror[0] || (i == dragging) != (pass == 1)) continue;
        int dx = i == dragging ? drag_x : d->x, dy = i == dragging ? drag_y : d->y;
        int rx = origin_x + (int)(dx * factor), ry = origin_y + (int)(dy * factor), rw = (int)(d->w * factor), rh = (int)(d->h * factor);
        ui_rrect(rx, ry, rw, rh, 4, i == selected ? 0x2F6FD6 : 0x3A4A6A, 255);
        if (i == selected) ui_rrect_stroke(rx, ry, rw, rh, 4, 0xFFFFFF, 220);
        if (d->primary) ui_blend_rect(rx + 3, ry + 3, rw - 6, 5, 0xFFFFFF, 220);   /* the menu bar marks the main display */
        char size[32]; snprintf(size, sizeof(size), "%dx%d", d->mode_w, d->mode_h);
        appui_px_text_c(rx + rw / 2, ry + rh / 2 - 16, d->name, APPUI_F_BOLD, APPUI_TEXT);
        appui_px_text_c(rx + rw / 2, ry + rh / 2 + 2, size, APPUI_F_SMALL, APPUI_TEXT);
        for (int k = 0; k < count; k++) if (displays[k].mirror[0] && !strcmp(displays[k].mirror, d->name))
            appui_px_text_c(rx + rw / 2, ry + rh / 2 + 18, "mirrored", APPUI_F_SMALL, APPUI_TEXT);
        if (dragging < 0 && (rects[i][0] != rx || rects[i][1] != ry || rects[i][2] != rw || rects[i][3] != rh)) {   /* window-local rectangle (tests) */
            rects[i][0] = rx; rects[i][1] = ry; rects[i][2] = rw; rects[i][3] = rh;
            printf("[64] SETTINGS LAYOUT %s x=%d y=%d w=%d h=%d\n", d->name, rx, ry, rw, rh); fflush(stdout);
        }
    }
    appui_px_text(area_x + 12, area_y + area_h - 22, "Drag displays to arrange them. The white bar marks the main display.", APPUI_F_SMALL, APPUI_SUBTLE);
    /* every display (mirrored and switched-off ones are not in the arrangement) */
    const char* names[DISPLAYS]; for (int i = 0; i < count; i++) names[i] = displays[i].name;
    hit_select.x = x + 20; hit_select.y = area_y + area_h + 12; hit_select.w = 130 * count; hit_select.h = 28;
    appui_px_segment(hit_select.x, hit_select.y, hit_select.w, hit_select.h, names, count, selected);
    /* the selected display */
    struct display* d = &displays[selected]; int ry = area_y + area_h + 56;
    char title[96]; snprintf(title, sizeof(title), "%s  (%s)", d->model, d->name);
    appui_px_text(x + 20, ry, title, APPUI_F_BOLD, APPUI_TEXT); ry += 30;
    memset(&hit_main, 0, sizeof(hit_main)); memset(&hit_use, 0, sizeof(hit_use)); memset(&hit_enable, 0, sizeof(hit_enable));
    memset(&hit_res_prev, 0, sizeof(hit_res_prev)); memset(&hit_res_next, 0, sizeof(hit_res_next)); memset(&hit_scale, 0, sizeof(hit_scale));
    if (!d->primary) {
        appui_px_text(x + 20, ry + 6, "Display", APPUI_F_BODY, APPUI_SUBTLE);
        appui_px_toggle(x + 200, ry + 2, d->enabled); hit_enable.x = x + 200; hit_enable.y = ry; hit_enable.w = 52; hit_enable.h = 30;
        appui_px_text(x + 262, ry + 6, d->enabled ? "On" : "Off", APPUI_F_BODY, APPUI_TEXT);
        if (d->enabled && !d->mirror[0]) { appui_px_button(x + 330, ry, 170, 28, "Use as main display", false); hit_main.x = x + 330; hit_main.y = ry; hit_main.w = 170; hit_main.h = 28; }
        ry += 40;
        if (d->enabled) {
            appui_px_text(x + 20, ry + 6, "Use as", APPUI_F_BODY, APPUI_SUBTLE);
            const struct display* p = NULL; for (int i = 0; i < count; i++) if (displays[i].primary) p = &displays[i];
            char mirror_label[64]; snprintf(mirror_label, sizeof(mirror_label), "Mirror %s", p ? p->name : "main");
            const char* labels[2] = {"Extended display", mirror_label};
            appui_px_segment(x + 200, ry, 300, 28, labels, 2, d->mirror[0] ? 1 : 0); hit_use.x = x + 200; hit_use.y = ry; hit_use.w = 300; hit_use.h = 28;
            ry += 40;
        }
    }
    if (d->enabled) {
        appui_px_text(x + 20, ry + 6, "Resolution", APPUI_F_BODY, APPUI_SUBTLE);
        char mode[48]; int k = mode_index(d);
        snprintf(mode, sizeof(mode), "%d x %d  %d Hz%s", d->mode_w, d->mode_h, d->refresh, k >= 0 && d->modes[k].preferred ? "  (default)" : "");
        appui_px_button(x + 200, ry, 34, 28, "<", false); appui_px_field(x + 240, ry, 220, 28, mode, false); appui_px_button(x + 466, ry, 34, 28, ">", false);
        hit_res_prev.x = x + 200; hit_res_prev.y = ry; hit_res_prev.w = 34; hit_res_prev.h = 28;
        hit_res_next.x = x + 466; hit_res_next.y = ry; hit_res_next.w = 34; hit_res_next.h = 28;
        ry += 40;
        appui_px_text(x + 20, ry + 6, "Scale", APPUI_F_BODY, APPUI_SUBTLE);
        static const char* const scales[SCALES] = {"100%", "125%", "150%", "175%", "200%", "300%"};
        int current = -1; for (int i = 0; i < SCALES; i++) if (SCALE_VALUES[i] == d->scale) current = i;
        appui_px_segment(x + 200, ry, 360, 28, scales, SCALES, current); hit_scale.x = x + 200; hit_scale.y = ry; hit_scale.w = 360; hit_scale.h = 28;
        ry += 40;
    }
    memset(&hit_keep, 0, sizeof(hit_keep)); memset(&hit_revert, 0, sizeof(hit_revert));
    if (confirm_until) {
        uint64_t now = now_ms(); int left_s = now < confirm_until ? (int)((confirm_until - now + 999) / 1000) : 0;
        char question[96]; snprintf(question, sizeof(question), "Keep these display settings? Reverting in %d s.", left_s);
        appui_px_card(x + 20, ry + 4, w - 40, 44, 8, 0x3A3320); appui_px_text(x + 34, ry + 17, question, APPUI_F_BODY, APPUI_TEXT);
        appui_px_button(x + w - 220, ry + 12, 90, 28, "Revert", false); appui_px_button(x + w - 120, ry + 12, 90, 28, "Keep", true);
        hit_revert.x = x + w - 220; hit_revert.y = ry + 12; hit_revert.w = 90; hit_revert.h = 28;
        hit_keep.x = x + w - 120; hit_keep.y = ry + 12; hit_keep.w = 90; hit_keep.h = 28;
    }
    if (strcmp(backend, "kms")) appui_px_text(x + 20, y + h - 22, "This display is the firmware framebuffer: no other modes or displays.", APPUI_F_SMALL, APPUI_FAINT);
    char controls[512]; static char reported[512];   /* window-local control rectangles (tests drive the page by them) */
    snprintf(controls, sizeof(controls), "[64] SETTINGS CONTROLS %s main=%d,%d,%d,%d enable=%d,%d,%d,%d use=%d,%d,%d,%d prev=%d,%d,%d,%d next=%d,%d,%d,%d scale=%d,%d,%d,%d keep=%d,%d,%d,%d revert=%d,%d,%d,%d\n",
             d->name, hit_main.x, hit_main.y, hit_main.w, hit_main.h, hit_enable.x, hit_enable.y, hit_enable.w, hit_enable.h, hit_use.x, hit_use.y, hit_use.w, hit_use.h,
             hit_res_prev.x, hit_res_prev.y, hit_res_prev.w, hit_res_prev.h, hit_res_next.x, hit_res_next.y, hit_res_next.w, hit_res_next.h,
             hit_scale.x, hit_scale.y, hit_scale.w, hit_scale.h, hit_keep.x, hit_keep.y, hit_keep.w, hit_keep.h, hit_revert.x, hit_revert.y, hit_revert.w, hit_revert.h);
    size_t used = strlen(controls) - 1;   /* the display selector: one segment per display */
    used += (size_t)snprintf(controls + used, sizeof(controls) - used, " select=%d,%d,%d,%d\n", hit_select.x, hit_select.y, hit_select.w, hit_select.h);
    if (strcmp(controls, reported)) { strcpy(reported, controls); fputs(controls, stdout); fflush(stdout); }
}
/* Absolute window-content coordinates arrive here relative to the page; the hit boxes were recorded absolute. */
static int page_x, page_y;
static void set_scale(struct display* d, int percent) {   /* the logical size the desktop will give it */
    d->scale = percent; d->w = (int)((int64_t)d->mode_w * 100 / percent); d->h = (int)((int64_t)d->mode_h * 100 / percent);
}
/* After d changed size from old_w x old_h: displays that touched its right or bottom edge keep touching it. */
static void reflow(struct display* d, int old_w, int old_h) {
    for (int i = 0; i < count; i++) {
        struct display* o = &displays[i]; if (o == d || !o->enabled || o->mirror[0]) continue;
        if (o->x == d->x + old_w) o->x = d->x + d->w;
        if (o->y == d->y + old_h) o->y = d->y + d->h;
    }
}
static void step_mode(struct display* d, int direction) {
    int k = mode_index(d); if (!d->mode_count) return;
    k = k < 0 ? 0 : (k + direction + d->mode_count) % d->mode_count;
    snapshot(); int old_w = d->w, old_h = d->h;
    d->mode_w = d->modes[k].w; d->mode_h = d->modes[k].h; d->refresh = d->modes[k].refresh;
    set_scale(d, d->scale >= 100 ? d->scale : 100);
    reflow(d, old_w, old_h);
    apply(true);
}
static void click(int x, int y) {
    x += page_x; y += page_y;
    struct display* d = count ? &displays[selected] : NULL;
    if (confirm_until && inside(x, y, hit_keep.x, hit_keep.y, hit_keep.w, hit_keep.h)) { confirm_until = 0; settings_message("Display settings kept."); printf("[64] SETTINGS DISPLAYS KEPT\n"); fflush(stdout); return; }
    if (confirm_until && inside(x, y, hit_revert.x, hit_revert.y, hit_revert.w, hit_revert.h)) { revert(); return; }
    if (!d) return;
    if (inside(x, y, hit_select.x, hit_select.y, hit_select.w, hit_select.h)) { selected = (x - hit_select.x) * count / hit_select.w; settings_redraw(); return; }
    if (inside(x, y, hit_main.x, hit_main.y, hit_main.w, hit_main.h)) { snapshot(); for (int i = 0; i < count; i++) displays[i].primary = i == selected; apply(false); return; }
    if (inside(x, y, hit_enable.x, hit_enable.y, hit_enable.w, hit_enable.h)) {
        snapshot(); d->enabled = !d->enabled; d->mirror[0] = 0;
        if (d->enabled) {   /* back on: right of everything */
            int right = 0; for (int i = 0; i < count; i++) if (i != selected && displays[i].enabled && !displays[i].mirror[0] && displays[i].x + displays[i].w > right) right = displays[i].x + displays[i].w;
            d->x = right; d->y = 0;
            if (!d->mode_w && d->mode_count) { d->mode_w = d->modes[0].w; d->mode_h = d->modes[0].h; d->refresh = d->modes[0].refresh; }
        }
        apply(false); return;
    }
    if (inside(x, y, hit_use.x, hit_use.y, hit_use.w, hit_use.h)) {
        bool mirror = x >= hit_use.x + hit_use.w / 2; snapshot();
        if (mirror) { for (int i = 0; i < count; i++) if (displays[i].primary) snprintf(d->mirror, sizeof(d->mirror), "%s", displays[i].name); }
        else { d->mirror[0] = 0; int right = 0; for (int i = 0; i < count; i++) if (i != selected && displays[i].enabled && !displays[i].mirror[0] && displays[i].x + displays[i].w > right) right = displays[i].x + displays[i].w; d->x = right; d->y = 0; }
        apply(false); return;
    }
    if (inside(x, y, hit_res_prev.x, hit_res_prev.y, hit_res_prev.w, hit_res_prev.h)) { step_mode(d, 1); return; }    /* the list is largest first */
    if (inside(x, y, hit_res_next.x, hit_res_next.y, hit_res_next.w, hit_res_next.h)) { step_mode(d, -1); return; }
    if (inside(x, y, hit_scale.x, hit_scale.y, hit_scale.w, hit_scale.h)) {
        int index = (x - hit_scale.x) * SCALES / hit_scale.w, scale = SCALE_VALUES[index < 0 ? 0 : index >= SCALES ? SCALES - 1 : index];
        if (scale != d->scale) { snapshot(); int old_w = d->w, old_h = d->h; set_scale(d, scale); reflow(d, old_w, old_h); apply(true); }
        return;
    }
    for (int i = count - 1; i >= 0; i--) {   /* select a display in the arrangement */
        struct display* e = &displays[i]; if (!e->enabled || e->mirror[0]) continue;
        int rx = origin_x + (int)(e->x * factor), ry = origin_y + (int)(e->y * factor);
        if (inside(x, y, rx, ry, (int)(e->w * factor), (int)(e->h * factor))) { selected = i; settings_redraw(); return; }
    }
}
/* Snap the dragged display edge to edge with the nearest other display, without overlapping any. */
static void snap(struct display* d, int* x, int* y) {
    long best = -1; int bx = *x, by = *y;
    for (int i = 0; i < count; i++) {
        struct display* o = &displays[i]; if (o == d || !o->enabled || o->mirror[0]) continue;
        int candidates[4][2] = {{o->x - d->w, *y}, {o->x + o->w, *y}, {*x, o->y - d->h}, {*x, o->y + o->h}};
        for (int k = 0; k < 4; k++) {
            int cx = candidates[k][0], cy = candidates[k][1];
            if (k < 2) { if (cy < o->y - d->h + 1) cy = o->y - d->h + 1; if (cy > o->y + o->h - 1) cy = o->y + o->h - 1; }
            else { if (cx < o->x - d->w + 1) cx = o->x - d->w + 1; if (cx > o->x + o->w - 1) cx = o->x + o->w - 1; }
            if (abs(cx - o->x) < 48) cx = o->x; if (abs(cy - o->y) < 48) cy = o->y;   /* align edges that are almost aligned */
            if (abs(cx + d->w - o->x - o->w) < 48) cx = o->x + o->w - d->w; if (abs(cy + d->h - o->y - o->h) < 48) cy = o->y + o->h - d->h;
            bool clash = false;
            for (int j = 0; j < count; j++) { struct display* p = &displays[j]; if (p == d || !p->enabled || p->mirror[0]) continue;
                if (cx < p->x + p->w && p->x < cx + d->w && cy < p->y + p->h && p->y < cy + d->h) clash = true; }
            if (clash) continue;
            long distance = (long)(cx - *x) * (cx - *x) + (long)(cy - *y) * (cy - *y);
            if (best < 0 || distance < best) { best = distance; bx = cx; by = cy; }
        }
    }
    *x = bx; *y = by;
}
static void drag(int x, int y, int phase) {
    x += page_x; y += page_y;
    if (phase == 0) {
        dragging = -1; moved = false;
        for (int i = count - 1; i >= 0; i--) {
            struct display* e = &displays[i]; if (!e->enabled || e->mirror[0]) continue;
            int rx = origin_x + (int)(e->x * factor), ry = origin_y + (int)(e->y * factor);
            if (inside(x, y, rx, ry, (int)(e->w * factor), (int)(e->h * factor))) { dragging = i; drag_dx = x - rx; drag_dy = y - ry; drag_x = e->x; drag_y = e->y; selected = i; break; }
        }
        return;
    }
    if (dragging < 0) return;
    if (factor > 0) { drag_x = (int)((x - drag_dx - origin_x) / factor); drag_y = (int)((y - drag_dy - origin_y) / factor); }
    if (drag_x != displays[dragging].x || drag_y != displays[dragging].y) moved = true;
    if (phase == 2) {
        int index = dragging; dragging = -1;
        if (!moved) { settings_redraw(); return; }
        struct display* d = &displays[index]; int nx = drag_x, ny = drag_y; snap(d, &nx, &ny);
        if (nx == d->x && ny == d->y) { settings_redraw(); return; }
        snapshot(); d->x = nx; d->y = ny;
        printf("[64] SETTINGS DISPLAY MOVED %s x=%d y=%d\n", d->name, nx, ny); fflush(stdout);
        apply(false);
    }
    settings_redraw();
}
static bool tick(void) {
    if (!confirm_until) return false;
    if (now_ms() >= confirm_until) { revert(); return true; }
    static uint64_t last; uint64_t second = now_ms() / 1000; if (second != last) { last = second; return true; }
    return false;
}
static void key(char k) { if (k == 27 && confirm_until) revert(); else if (k == '\n' && confirm_until) { confirm_until = 0; settings_message("Display settings kept."); } }
static void draw_page(int x, int y, int w, int h) { page_x = x; page_y = y; draw(x, y, w, h); }
const struct settings_page settings_displays = {"displays", "Displays", ICON_MONITOR, enter, draw_page, click, drag, key, tick};
