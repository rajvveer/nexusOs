/* Aurora accessibility (aurora_a11y.h). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aurora_a11y.h"
#include "aurora_control.h"

static struct aurora_a11y settings = {.zoom = 200};
const struct aurora_a11y* aurora_a11y(void) { return &settings; }
bool aurora_a11y_load(struct aurora_a11y* out) {
    *out = (struct aurora_a11y){.zoom = 200};
    FILE* f = fopen(AURORA_A11Y_CONF, "re"); if (!f) return false;
    char line[128]; int v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "larger_text=%d", &v) == 1) out->larger_text = v;
        else if (sscanf(line, "contrast=%d", &v) == 1) out->contrast = v;
        else if (sscanf(line, "invert=%d", &v) == 1) out->invert = v;
        else if (sscanf(line, "magnifier=%d", &v) == 1) out->magnifier = v;
        else if (sscanf(line, "reduce_motion=%d", &v) == 1) out->reduce_motion = v;
        else if (sscanf(line, "sticky_keys=%d", &v) == 1) out->sticky_keys = v;
        else if (sscanf(line, "zoom=%d", &v) == 1 && v >= 150 && v <= 400) out->zoom = v;
    }
    fclose(f); return true;
}
static void save(void) {
    mkdir("/home/nexus/.aurora", 0755);
    FILE* f = fopen(AURORA_A11Y_CONF ".tmp", "we"); if (!f) return;
    fprintf(f, "# Aurora accessibility (System Settings > Accessibility)\nlarger_text=%d\ncontrast=%d\ninvert=%d\nmagnifier=%d\nreduce_motion=%d\nsticky_keys=%d\nzoom=%d\n",
            settings.larger_text, settings.contrast, settings.invert, settings.magnifier, settings.reduce_motion, settings.sticky_keys, settings.zoom);
    bool ok = !fflush(f) && !fsync(fileno(f)); fclose(f);
    if (ok) rename(AURORA_A11Y_CONF ".tmp", AURORA_A11Y_CONF);
}
static void report(void) {
    printf("[64] AURORA A11Y larger_text=%d contrast=%d invert=%d magnifier=%d zoom=%d reduce_motion=%d sticky_keys=%d\n", settings.larger_text, settings.contrast,
           settings.invert, settings.magnifier, settings.zoom, settings.reduce_motion, settings.sticky_keys); fflush(stdout);
}
void aurora_a11y_zoom(int step) {
    if (step > 0) { if (!settings.magnifier) settings.magnifier = true; else if (settings.zoom < 400) settings.zoom += 50; }
    else if (settings.magnifier) { if (settings.zoom > 150) settings.zoom -= 50; else settings.magnifier = false; }
    save(); report(); aurora_host_a11y_changed();
}
static bool control_get(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size;
    aurora_control_print(reply, "larger_text=%d contrast=%d invert=%d magnifier=%d zoom=%d reduce_motion=%d sticky_keys=%d\n", settings.larger_text, settings.contrast,
                         settings.invert, settings.magnifier, settings.zoom, settings.reduce_motion, settings.sticky_keys);
    return true;
}
static bool control_set(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; struct aurora_a11y next = settings;
    char copy[256]; snprintf(copy, sizeof(copy), "%s", args ? args : "");
    for (char* save_ptr = NULL, *word = strtok_r(copy, " ", &save_ptr); word; word = strtok_r(NULL, " ", &save_ptr)) {
        char key[32]; int v; char extra;
        if (sscanf(word, "%31[a-z_]=%d%c", key, &v, &extra) != 2) { snprintf(error, size, "bad setting %.40s", word); return false; }
        if (!strcmp(key, "zoom") && v >= 150 && v <= 400) next.zoom = v;
        else if (v != 0 && v != 1) { snprintf(error, size, "%s takes 0 or 1", key); return false; }
        else if (!strcmp(key, "larger_text")) next.larger_text = v;
        else if (!strcmp(key, "contrast")) next.contrast = v;
        else if (!strcmp(key, "invert")) next.invert = v;
        else if (!strcmp(key, "magnifier")) next.magnifier = v;
        else if (!strcmp(key, "reduce_motion")) next.reduce_motion = v;
        else if (!strcmp(key, "sticky_keys")) next.sticky_keys = v;
        else { snprintf(error, size, "unknown setting %.31s", key); return false; }
    }
    settings = next; save(); report(); aurora_host_a11y_changed(); return true;
}
void aurora_a11y_init(void) {
    aurora_a11y_load(&settings);
    aurora_control_register("a11y", false, control_get);
    aurora_control_register("set-a11y", false, control_set);
    if (settings.larger_text || settings.contrast || settings.invert || settings.magnifier || settings.reduce_motion || settings.sticky_keys) { report(); aurora_host_a11y_changed(); }
}
bool aurora_a11y_filtering(void) { return settings.contrast || settings.invert; }
void aurora_a11y_filter(uint32_t* canvas, size_t stride, int x, int y, int w, int h) {
    if (!aurora_a11y_filtering() || w <= 0 || h <= 0) return;
    static uint8_t table[256]; static int built = -1; int kind = settings.contrast | settings.invert << 1;
    if (built != kind) {   /* contrast: steeper around mid-grey; invert: 255 - v (after contrast) */
        for (int v = 0; v < 256; v++) { int c = settings.contrast ? (v - 128) * 8 / 5 + 128 : v; c = c < 0 ? 0 : c > 255 ? 255 : c; table[v] = (uint8_t)(settings.invert ? 255 - c : c); }
        built = kind;
    }
    for (int row = y; row < y + h; row++) {
        uint32_t* p = canvas + (size_t)row * stride + x;
        for (int col = 0; col < w; col++) { uint32_t v = p[col]; p[col] = (uint32_t)table[v >> 16 & 255] << 16 | (uint32_t)table[v >> 8 & 255] << 8 | table[v & 255]; }
    }
}
bool aurora_a11y_lens(int px, int py, int* x, int* y, int* w, int* h) {
    if (!settings.magnifier) return false;
    *w = 480; *h = 300; *x = px - *w / 2; *y = py - *h / 2; return true;
}
void aurora_a11y_magnify(uint32_t* canvas, int cw, int ch, size_t stride, int lx, int ly, int lw, int lh, int cx, int cy) {
    if (lw <= 0 || lh <= 0) return;
    int sw = lw * 100 / settings.zoom, sh = lh * 100 / settings.zoom; if (sw < 1 || sh < 1) return;
    int sx = cx - sw / 2, sy = cy - sh / 2;
    uint32_t* source = malloc((size_t)sw * sh * 4); if (!source) return;
    for (int r = 0; r < sh; r++) for (int c = 0; c < sw; c++) {   /* the area under the pointer, before the lens covers it */
        int yy = sy + r, xx = sx + c; source[(size_t)r * sw + c] = yy >= 0 && yy < ch && xx >= 0 && xx < cw ? canvas[(size_t)yy * stride + xx] : 0x101018;
    }
    for (int r = 0; r < lh; r++) {
        int yy = ly + r; if (yy < 0 || yy >= ch) continue;
        const uint32_t* line = source + (size_t)(r * sh / lh) * sw;
        for (int c = 0; c < lw; c++) { int xx = lx + c; if (xx >= 0 && xx < cw) canvas[(size_t)yy * stride + xx] = line[c * sw / lw]; }
    }
    free(source);
    for (int k = 0; k < 3; k++) for (int c = 0; c < lw; c++) {   /* frame */
        int xx = lx + c; if (xx < 0 || xx >= cw) continue;
        if (ly + k >= 0 && ly + k < ch) canvas[(size_t)(ly + k) * stride + xx] = 0xFFD60A;
        if (ly + lh - 1 - k >= 0 && ly + lh - 1 - k < ch) canvas[(size_t)(ly + lh - 1 - k) * stride + xx] = 0xFFD60A;
    }
    for (int k = 0; k < 3; k++) for (int r = 0; r < lh; r++) {
        int yy = ly + r; if (yy < 0 || yy >= ch) continue;
        if (lx + k >= 0 && lx + k < cw) canvas[(size_t)yy * stride + lx + k] = 0xFFD60A;
        if (lx + lw - 1 - k >= 0 && lx + lw - 1 - k < cw) canvas[(size_t)yy * stride + lx + lw - 1 - k] = 0xFFD60A;
    }
}
