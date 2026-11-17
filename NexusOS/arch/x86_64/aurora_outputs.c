/* Aurora displays: KMS outputs (or the fbdev fallback), their layout, scanout and persistence (aurora_outputs.h). */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aurora_outputs.h"
#include "aurora_control.h"
#include "aurora_graphics.h"
#include "../../kernel/macui.h"

#define HEADS 16
static struct aurora_output outputs[AURORA_OUTPUTS];
static int card = -1, uevents = -1, heads;
static bool kms;
static uint32_t crtc_ids[HEADS], connector_ids[HEADS];
static uint32_t* fbdev; static size_t fbdev_bytes; static int fbdev_w, fbdev_h; static size_t fbdev_stride;
static bool fbdev_bgr;   /* XBGR8888 firmware framebuffer (bytes R,G,B,X; some GOPs): R/B swapped at present (hw) */
static void swap_rb(uint32_t* pixels, size_t stride, int x, int y, int w, int h) {
    for (int row = y; row < y + h; row++) {
        uint32_t* p = pixels + (size_t)row * stride + x;
        for (int i = 0; i < w; i++) { uint32_t v = p[i]; p[i] = (v & 0xFF00FF00U) | (v >> 16 & 0xFFU) | (v & 0xFFU) << 16; }
    }
}
static uint32_t boot_fb; static int boot_w, boot_h;   /* the firmware framebuffer as KMS framebuffer object (CRTC 0) */
static unsigned serials;
/* Per-monitor saved layout (displays.conf). */
struct saved { char name[32], identity[80]; bool enabled, primary, automode; int x, y, mode_w, mode_h, refresh, scale; char mirror[32]; };   /* scale: percent */
static struct saved saved[16]; static int saved_count;

static int io(unsigned long request, void* argument) { int r; do r = ioctl(card, request, argument); while (r == -1 && errno == EINTR); return r; }
static void identity(const struct aurora_output* o, char* text, size_t size) {
    snprintf(text, size, "%s:%s:%s", o->make[0] ? o->make : "-", o->model[0] ? o->model : "-", o->serial[0] ? o->serial : "-");
    for (char* p = text; *p; p++) if (*p == ' ' || *p == '\t' || *p == '=') *p = '_';
}

/* ---- saved layout ---- */
static void load_saved(void) {
    saved_count = 0;
    FILE* f = fopen(AURORA_DISPLAYS_CONF, "re"); if (!f) return;
    char line[512];
    while (saved_count < 16 && fgets(line, sizeof(line), f)) {
        struct saved s = {.enabled = true, .scale = 100, .automode = true}; char key[16];
        if (sscanf(line, "%15s %31s %79s", key, s.name, s.identity) != 3 || strcmp(key, "display")) continue;
        for (char* t = strtok(line, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            int a, b, c;
            if (sscanf(t, "enabled=%d", &a) == 1) s.enabled = a != 0;
            else if (sscanf(t, "primary=%d", &a) == 1) s.primary = a != 0;
            else if (sscanf(t, "pos=%d,%d", &a, &b) == 2) { s.x = a; s.y = b; }
            else if (sscanf(t, "mode=%dx%d@%d", &a, &b, &c) == 3) { s.mode_w = a; s.mode_h = b; s.refresh = c; s.automode = false; }
            else if (sscanf(t, "scale=%d", &a) == 1 && AURORA_SCALE_VALID(a <= 4 ? a * 100 : a)) s.scale = a <= 4 ? a * 100 : a;   /* "150%" or 1..4 */
            else if (!strncmp(t, "mirror=", 7)) snprintf(s.mirror, sizeof(s.mirror), "%s", strcmp(t + 7, "-") ? t + 7 : "");
        }
        saved[saved_count++] = s;
    }
    fclose(f);
}
static void save(void) {
    mkdir(AURORA_STATE_DIR, 0755);
    char temp[] = AURORA_DISPLAYS_CONF ".tmp";
    FILE* f = fopen(temp, "we"); if (!f) { perror("Aurora displays.conf"); return; }
    fprintf(f, "# Aurora display layout (Settings > Displays). One line per monitor: connector, make:model:serial.\n");
    struct saved keep[16]; int kept = 0;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; if (!o->connected) continue;
        char id[80]; identity(o, id, sizeof(id));
        const struct aurora_output* m = o->mirror >= 0 ? &outputs[o->mirror] : NULL;
        fprintf(f, "display %s %s enabled=%d primary=%d pos=%d,%d mode=%dx%d@%d scale=%d%% mirror=%s\n", o->name, id, o->enabled, o->primary,
                o->x, o->y, o->mode_w, o->mode_h, o->refresh, o->scale120 * 5 / 6, m ? m->name : "-");
        if (kept < 16) { snprintf(keep[kept].name, sizeof(keep[kept].name), "%s", o->name); snprintf(keep[kept].identity, sizeof(keep[kept].identity), "%s", id); kept++; }
    }
    for (int i = 0; i < saved_count; i++) {   /* monitors not connected now keep their entries */
        bool present = false;
        for (int k = 0; k < kept; k++) if (!strcmp(keep[k].identity, saved[i].identity) && !strcmp(keep[k].name, saved[i].name)) present = true;
        if (present) continue;
        const struct saved* s = &saved[i];
        if (s->automode) fprintf(f, "display %s %s enabled=%d primary=%d pos=%d,%d scale=%d%% mirror=%s\n", s->name, s->identity, s->enabled, s->primary, s->x, s->y, s->scale, s->mirror[0] ? s->mirror : "-");
        else fprintf(f, "display %s %s enabled=%d primary=%d pos=%d,%d mode=%dx%d@%d scale=%d%% mirror=%s\n", s->name, s->identity, s->enabled, s->primary, s->x, s->y,
                     s->mode_w, s->mode_h, s->refresh, s->scale, s->mirror[0] ? s->mirror : "-");
    }
    if (fflush(f) || fsync(fileno(f))) perror("Aurora displays.conf");
    fclose(f);
    if (rename(temp, AURORA_DISPLAYS_CONF)) perror("Aurora displays.conf rename");
    load_saved();
    printf("[64] AURORA DISPLAYS SAVED\n"); fflush(stdout);
}
static const struct saved* saved_for(const struct aurora_output* o) {
    char id[80]; identity(o, id, sizeof(id));
    for (int i = 0; i < saved_count; i++) if (!strcmp(saved[i].identity, id) && !strcmp(saved[i].name, o->name)) return &saved[i];
    for (int i = 0; i < saved_count; i++) if (!strcmp(saved[i].identity, id) && strcmp(id, "-:-:-")) return &saved[i];
    return NULL;
}

/* ---- KMS probing ---- */
static void edid_identity(struct aurora_output* o, const uint8_t* e, size_t size) {
    if (size < 128) return;
    uint16_t vendor = (uint16_t)(e[8] << 8 | e[9]);
    o->make[0] = (char)('A' - 1 + ((vendor >> 10) & 31)); o->make[1] = (char)('A' - 1 + ((vendor >> 5) & 31)); o->make[2] = (char)('A' - 1 + (vendor & 31)); o->make[3] = 0;
    uint32_t serial = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
    snprintf(o->serial, sizeof(o->serial), "%u", serial);
    snprintf(o->model, sizeof(o->model), "%04X", (unsigned)(e[10] | e[11] << 8));
    for (int k = 0; k < 4; k++) {
        const uint8_t* d = e + 54 + k * 18;
        if (d[0] || d[1] || (d[3] != 0xFC && d[3] != 0xFF)) continue;
        char text[14]; int n = 0;
        for (int i = 5; i < 18 && d[i] != '\n' && d[i] >= 32 && d[i] < 127; i++) text[n++] = (char)d[i];
        while (n && text[n - 1] == ' ') n--;
        text[n] = 0; if (!n) continue;
        if (d[3] == 0xFC) snprintf(o->model, sizeof(o->model), "%s", text); else snprintf(o->serial, sizeof(o->serial), "%s", text);
    }
}
/* Read connector index i: connection, modes, EDID identity. */
static bool probe_connector(int i, struct aurora_output* o) {
    struct drm_mode_get_connector g = {.connector_id = connector_ids[i]};
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, &g)) return false;
    struct drm_mode_modeinfo modes[64]; uint32_t props[16]; uint64_t values[16];
    uint32_t count = g.count_modes > 64 ? 64 : g.count_modes;
    struct drm_mode_get_connector c = {.connector_id = connector_ids[i], .count_modes = count, .modes_ptr = (uintptr_t)modes,
                                       .count_props = 16, .props_ptr = (uintptr_t)props, .prop_values_ptr = (uintptr_t)values};
    if (!count) { c.count_modes = 1; c.modes_ptr = (uintptr_t)modes; }   /* a non-zero count: no second forced probe */
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, &c)) return false;
    if (count && c.count_modes != count) return false;   /* changed between the two calls: the caller retries */
    memset(o, 0, sizeof(*o)); o->index = i; o->mirror = -1; o->scale = 1; o->scale120 = 120; o->connector = connector_ids[i]; o->crtc = crtc_ids[i];
    snprintf(o->name, sizeof(o->name), "%s-%u", c.connector_type == DRM_MODE_CONNECTOR_VIRTUAL ? "Virtual" : c.connector_type == DRM_MODE_CONNECTOR_HDMIA ? "HDMI-A" :
             c.connector_type == DRM_MODE_CONNECTOR_DisplayPort ? "DP" : "Unknown", c.connector_type_id);
    o->connected = c.connection == 1; o->mm_w = (int)c.mm_width; o->mm_h = (int)c.mm_height;
    for (uint32_t k = 0; k < count && o->mode_count < AURORA_MODES; k++) {
        if (modes[k].hdisplay < 320 || modes[k].vdisplay < 200 || (modes[k].flags & DRM_MODE_FLAG_INTERLACE)) continue;
        o->modes[o->mode_count++] = (struct aurora_mode){modes[k].hdisplay, modes[k].vdisplay, (int)modes[k].vrefresh, (modes[k].type & DRM_MODE_TYPE_PREFERRED) != 0};
    }
    for (uint32_t k = 0; k < c.count_props && k < 16; k++) {
        struct drm_mode_get_property p = {.prop_id = props[k]};
        if (io(DRM_IOCTL_MODE_GETPROPERTY, &p) || strcmp(p.name, "EDID") || !values[k]) continue;
        struct drm_mode_get_blob b = {.blob_id = (uint32_t)values[k]};
        uint8_t edid[1024];
        if (io(DRM_IOCTL_MODE_GETPROPBLOB, &b) || b.length > sizeof(edid)) continue;
        b.data = (uintptr_t)edid;
        if (!io(DRM_IOCTL_MODE_GETPROPBLOB, &b)) edid_identity(o, edid, b.length);
    }
    return true;
}
static bool kms_mode(const struct aurora_output* o, int w, int h, int refresh, struct drm_mode_modeinfo* out) {
    struct drm_mode_get_connector g = {.connector_id = o->connector};
    struct drm_mode_modeinfo modes[64], one;
    g.count_modes = 1; g.modes_ptr = (uintptr_t)&one;   /* no probe */
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, &g)) return false;
    uint32_t count = g.count_modes > 64 ? 64 : g.count_modes; if (!count) return false;
    struct drm_mode_get_connector c = {.connector_id = o->connector, .count_modes = count, .modes_ptr = (uintptr_t)modes};
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.count_modes != count) return false;
    for (uint32_t k = 0; k < count; k++)
        if (modes[k].hdisplay == w && modes[k].vdisplay == h && (!refresh || (int)modes[k].vrefresh == refresh)) { *out = modes[k]; return true; }
    return false;
}

/* Scale in percent (invalid: 100 %): fractional scale, wl_output scale and the logical size from the mode. */
static void set_scale(struct aurora_output* o, int percent) {
    if (!AURORA_SCALE_VALID(percent)) percent = 100;
    while (percent > 100 && ((int64_t)o->mode_w * 100 / percent < 640 || (int64_t)o->mode_h * 100 / percent < 360)) percent -= 25;   /* a saved scale too large for this mode */
    o->scale120 = percent * 6 / 5; o->scale = (percent + 99) / 100;
    o->w = (int)((int64_t)o->mode_w * 120 / o->scale120); o->h = (int)((int64_t)o->mode_h * 120 / o->scale120);
}

/* ---- buffers ---- */
static void release_buffers(struct aurora_output* o) {
    if (o->canvas && o->canvas != o->scan) free(o->canvas);
    if (o->own_fb && kms) {
        if (o->fb) { uint32_t id = o->fb; io(DRM_IOCTL_MODE_RMFB, &id); }
        if (o->scan) munmap(o->scan, o->scan_bytes);
        if (o->handle) { struct drm_mode_destroy_dumb d = {o->handle}; io(DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
    }
    free(o->wallpaper);
    o->canvas = o->scan = o->wallpaper = NULL; o->fb = o->handle = 0; o->own_fb = o->fbdev = false; o->scan_bytes = o->scan_stride = 0;
    o->buffer_w = o->buffer_h = 0;
}
static void (*present_filter)(uint32_t* pixels, size_t stride, int x, int y, int w, int h);
static bool wants_fbdev(const struct aurora_output* o) { return kms ? o->index == 0 && boot_fb && o->mode_w == boot_w && o->mode_h == boot_h : true; }
static bool make_buffers(struct aurora_output* o) {
    int w = o->mode_w, h = o->mode_h;
    if (wants_fbdev(o)) {   /* KMS head 0 at the boot mode keeps the firmware framebuffer: /dev/fb0 stays the screen */
        if (!fbdev || (kms ? w != boot_w || h != boot_h : w != fbdev_w || h != fbdev_h)) return false;
        o->scan = fbdev; o->scan_stride = fbdev_stride; o->scan_bytes = fbdev_bytes; o->fb = kms ? boot_fb : 0; o->fbdev = true;
    } else {
        struct drm_mode_create_dumb create = {.height = (uint32_t)h, .width = (uint32_t)w, .bpp = 32};
        if (io(DRM_IOCTL_MODE_CREATE_DUMB, &create)) { perror("Aurora dumb buffer"); return false; }
        o->handle = create.handle; o->own_fb = true;
        struct drm_mode_map_dumb map = {.handle = create.handle};
        if (io(DRM_IOCTL_MODE_MAP_DUMB, &map)) { release_buffers(o); return false; }
        void* pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, card, (off_t)map.offset);
        if (pixels == MAP_FAILED) { release_buffers(o); return false; }
        o->scan = pixels; o->scan_bytes = create.size; o->scan_stride = create.pitch / 4;
        struct drm_mode_fb_cmd2 add = {.width = (uint32_t)w, .height = (uint32_t)h, .pixel_format = DRM_FORMAT_XRGB8888, .handles = {create.handle}, .pitches = {create.pitch}};
        if (create.pitch != (uint32_t)w * 4 || io(DRM_IOCTL_MODE_ADDFB2, &add)) { perror("Aurora framebuffer"); release_buffers(o); return false; }
        o->fb = add.fb_id;
    }
    o->buffer_w = w; o->buffer_h = h;
    if (o->mirror >= 0) return true;   /* a scaled mirror composes nothing: its source presents into it */
    /* Dumb buffers are cached RAM: compose in place. The firmware framebuffer is write-combined: compose in RAM. */
    o->canvas = o->fbdev || present_filter ? malloc((size_t)w * h * 4) : o->scan;
    o->wallpaper = malloc((size_t)w * h * 4);
    if (!o->canvas || !o->wallpaper) { release_buffers(o); return false; }
    memset(o->canvas, 0, (size_t)w * h * 4);
    return true;
}
/* The wallpaper at w x h: macui renders up to 2048 pixels wide; wider outputs get a bilinear enlargement. */
static void render_wallpaper(struct aurora_output* o) {
    int w = o->mode_w, h = o->mode_h;
    if (w <= 2048 && (uint64_t)w * h * 4 <= 0x800000) { aurora_canvas(o->wallpaper, w, h); ui_wallpaper_draw(); return; }
    int sw = 1920, sh = (int)((int64_t)h * 1920 / w); if (sh > 1080) { sh = 1080; sw = (int)((int64_t)w * 1080 / h); }
    if (sw < 64) sw = 64; if (sh < 64) sh = 64;
    uint32_t* small = malloc((size_t)sw * sh * 4);
    if (!small) { for (size_t i = 0; i < (size_t)w * h; i++) o->wallpaper[i] = 0x1A1E3C; return; }
    aurora_canvas(small, sw, sh); ui_wallpaper_draw();
    for (int y = 0; y < h; y++) {
        int64_t fy = (int64_t)y * (sh - 1) * 256 / (h > 1 ? h - 1 : 1); int y0 = (int)(fy >> 8), y1 = y0 + 1 < sh ? y0 + 1 : y0; int ty = (int)(fy & 255);
        for (int x = 0; x < w; x++) {
            int64_t fx = (int64_t)x * (sw - 1) * 256 / (w > 1 ? w - 1 : 1); int x0 = (int)(fx >> 8), x1 = x0 + 1 < sw ? x0 + 1 : x0; int tx = (int)(fx & 255);
            uint32_t p[4] = {small[(size_t)y0 * sw + x0], small[(size_t)y0 * sw + x1], small[(size_t)y1 * sw + x0], small[(size_t)y1 * sw + x1]}, out = 0;
            for (int s = 0; s < 24; s += 8) {
                int top = (int)((p[0] >> s & 255) * (256 - tx) + (p[1] >> s & 255) * tx), bottom = (int)((p[2] >> s & 255) * (256 - tx) + (p[3] >> s & 255) * tx);
                out |= (uint32_t)(((top * (256 - ty) + bottom * ty) >> 16) & 255) << s;
            }
            o->wallpaper[(size_t)y * w + x] = out;
        }
    }
    free(small);
}
void aurora_output_wallpaper(const struct aurora_output* o, uint32_t* dst, size_t stride, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > o->mode_w) w = o->mode_w - x; if (y + h > o->mode_h) h = o->mode_h - y;
    if (w <= 0 || h <= 0 || !o->wallpaper) return;
    for (int row = y; row < y + h; row++) memcpy(dst + (size_t)row * stride + x, o->wallpaper + (size_t)row * o->mode_w + x, (size_t)w * 4);
}

/* ---- layout ---- */
static void normalize(void) {
    int min_x = 1 << 30, min_y = 1 << 30;
    for (int i = 0; i < AURORA_OUTPUTS; i++) { struct aurora_output* o = &outputs[i]; if (o->enabled && o->mirror < 0) { if (o->x < min_x) min_x = o->x; if (o->y < min_y) min_y = o->y; } }
    if (min_x == 1 << 30) return;
    for (int i = 0; i < AURORA_OUTPUTS; i++) { struct aurora_output* o = &outputs[i]; if (o->enabled && o->mirror < 0) { o->x -= min_x; o->y -= min_y; } }
    for (int i = 0; i < AURORA_OUTPUTS; i++) {   /* mirrors take their source's rectangle (logical size of the source) */
        struct aurora_output* o = &outputs[i];
        if (o->enabled && o->mirror >= 0) { o->x = outputs[o->mirror].x; o->y = outputs[o->mirror].y; o->w = outputs[o->mirror].w; o->h = outputs[o->mirror].h; }
        else if (o->enabled) set_scale(o, o->scale120 * 5 / 6);
    }
}
static bool overlap(const struct aurora_output* a, const struct aurora_output* b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}
/* SETCRTC every enabled output (sources first: a mirror in the same mode scans out its source's framebuffer). */
static bool modeset_all(char* error, size_t size) {
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i];
        if (!o->connected) continue;
        if (!o->enabled) {
            if (pass == 0 && kms && o->crtc) { struct drm_mode_crtc off = {.crtc_id = o->crtc}; io(DRM_IOCTL_MODE_SETCRTC, &off); }
            continue;
        }
        if ((o->mirror >= 0) != (pass == 1) || !kms) continue;
        struct aurora_output* source = o->mirror >= 0 ? &outputs[o->mirror] : NULL;
        uint32_t fb = o->scan ? o->fb : source ? source->fb : 0;
        if (!fb) { snprintf(error, size, "%s has no framebuffer", o->name); return false; }
        struct drm_mode_modeinfo mode;
        if (!kms_mode(o, o->mode_w, o->mode_h, o->refresh, &mode)) { snprintf(error, size, "%s has no mode %dx%d@%d", o->name, o->mode_w, o->mode_h, o->refresh); return false; }
        uint32_t connector = o->connector;
        struct drm_mode_crtc set = {.crtc_id = o->crtc, .fb_id = fb, .set_connectors_ptr = (uintptr_t)&connector, .count_connectors = 1, .mode = mode, .mode_valid = 1};
        if (io(DRM_IOCTL_MODE_SETCRTC, &set)) { snprintf(error, size, "%s: SETCRTC %dx%d failed: %s", o->name, o->mode_w, o->mode_h, strerror(errno)); return false; }
        o->refresh = (int)mode.vrefresh;
    }
    return true;
}
/* Make the buffers of every slot match its fields, then modeset. */
static bool realize(char* error, size_t size) {
    normalize();
    bool fresh[AURORA_OUTPUTS] = {0};
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i];
        if (!o->connected || !o->enabled) { release_buffers(o); continue; }
        struct aurora_output* source = o->mirror >= 0 ? &outputs[o->mirror] : NULL;
        if (source && source->mode_w == o->mode_w && source->mode_h == o->mode_h) { release_buffers(o); continue; }   /* shares the source's scanout */
        bool good = o->scan && o->buffer_w == o->mode_w && o->buffer_h == o->mode_h && o->fbdev == wants_fbdev(o) && (o->canvas != NULL) == (o->mirror < 0) &&
                    (o->mirror >= 0 || o->fbdev || (o->canvas != o->scan) == (present_filter != NULL));
        if (good) continue;
        release_buffers(o);
        if (!make_buffers(o)) { snprintf(error, size, "%s: no buffers for %dx%d", o->name, o->mode_w, o->mode_h); return false; }
        fresh[i] = true;
    }
    if (!modeset_all(error, size)) return false;
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < AURORA_OUTPUTS; i++) {   /* the primary last: macui keeps its wallpaper cache at that size */
        struct aurora_output* o = &outputs[i];
        if (o->enabled && o->mirror < 0 && o->wallpaper && o->primary == (pass == 1) && (fresh[i] || o->primary)) render_wallpaper(o);
    }
    serials++;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; o->serial_number = serials;
        if (o->connected) printf("[64] AURORA LAYOUT %s %s %dx%d@%d at %d,%d size %dx%d scale=%d primary=%d mirror=%s\n", o->name, o->enabled ? "on" : "off", o->mode_w,
                                 o->mode_h, o->refresh, o->x, o->y, o->w, o->h, o->scale120 * 5 / 6, o->primary, o->mirror >= 0 ? outputs[o->mirror].name : "-");
    }
    fflush(stdout);
    return true;
}
/* Back to an earlier layout after a failed apply: drop every buffer, restore the fields, rebuild. */
static void restore_layout(const struct aurora_output* before) {
    for (int i = 0; i < AURORA_OUTPUTS; i++) release_buffers(&outputs[i]);
    memcpy(outputs, before, sizeof(outputs));
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i];
        o->canvas = o->scan = o->wallpaper = NULL; o->fb = o->handle = 0; o->own_fb = o->fbdev = false; o->scan_bytes = o->scan_stride = 0; o->buffer_w = o->buffer_h = 0;
    }
    char error[160];
    if (!realize(error, sizeof(error))) fprintf(stderr, "Aurora display restore: %s\n", error);
}
static int mode_index(const struct aurora_output* o, int w, int h, int refresh) {
    for (int k = 0; k < o->mode_count; k++) if (o->modes[k].w == w && o->modes[k].h == h && (!refresh || o->modes[k].refresh == refresh)) return k;
    return -1;
}
/* Defaults and the saved layout for a newly connected slot (placed right of the others when nothing is saved). */
static void place_new(struct aurora_output* o, int current_w, int current_h) {
    const struct saved* s = saved_for(o);
    int k = -1;
    if (s && !s->automode) k = mode_index(o, s->mode_w, s->mode_h, s->refresh);
    if (k < 0 && current_w) k = mode_index(o, current_w, current_h, 0);   /* the mode the CRTC already shows (boot) */
    if (k < 0) { k = 0; for (int i = 0; i < o->mode_count; i++) if (o->modes[i].preferred) { k = i; break; } }
    o->mode_w = o->modes[k].w; o->mode_h = o->modes[k].h; o->refresh = o->modes[k].refresh;
    set_scale(o, s ? s->scale : 100);
    o->enabled = s ? s->enabled : true;
    int right = 0; bool any = false;
    for (int i = 0; i < AURORA_OUTPUTS; i++) { struct aurora_output* p = &outputs[i]; if (p != o && p->enabled && p->mirror < 0 && p->connected) { any = true; if (p->x + p->w > right) right = p->x + p->w; } }
    o->x = right; o->y = 0;
    if (s) {
        struct aurora_output test = *o; test.x = s->x; test.y = s->y; bool clash = false;
        for (int i = 0; i < AURORA_OUTPUTS; i++) { struct aurora_output* p = &outputs[i]; if (p != o && p->enabled && p->mirror < 0 && p->connected && overlap(&test, p)) clash = true; }
        if (!clash) { o->x = s->x; o->y = s->y; }
        o->primary = s->primary;
    }
    if (!any) o->primary = true;
}
static void resolve_mirrors(void);
/* When every connected monitor has a saved entry, restore that layout whole (positions are only meaningful together). */
static bool apply_saved_set(void) {
    struct aurora_output trial[AURORA_OUTPUTS]; memcpy(trial, outputs, sizeof(trial));
    int connected = 0;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; if (!o->connected) continue;
        const struct saved* s = saved_for(o); if (!s) return false;
        connected++;
        int k = s->automode ? -1 : mode_index(o, s->mode_w, s->mode_h, s->refresh);
        if (k < 0) for (int m = 0; m < o->mode_count; m++) if (o->modes[m].preferred) { k = m; break; }
        if (k < 0) k = 0;
        o->mode_w = o->modes[k].w; o->mode_h = o->modes[k].h; o->refresh = o->modes[k].refresh;
        set_scale(o, s->scale);
        o->enabled = s->enabled; o->primary = s->primary && s->enabled; o->x = s->x; o->y = s->y;
    }
    if (!connected) return false;
    resolve_mirrors();
    bool any = false, clash = false;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* a = &outputs[i]; if (!a->enabled || a->mirror >= 0) continue;
        any = true;
        for (int k = i + 1; k < AURORA_OUTPUTS; k++) { struct aurora_output* b = &outputs[k]; if (b->enabled && b->mirror < 0 && overlap(a, b)) clash = true; }
    }
    if (!any || clash) { memcpy(outputs, trial, sizeof(trial)); return false; }
    return true;
}
static void fix_primary(void) {
    struct aurora_output* first = NULL; int primaries = 0;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i];
        if (!o->enabled || o->mirror >= 0) { o->primary = false; continue; }
        if (!first) first = o;
        if (o->primary && ++primaries > 1) o->primary = false;
    }
    if (!primaries && first) first->primary = true;
}
static void resolve_mirrors(void) {
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; const struct saved* s = o->connected ? saved_for(o) : NULL;
        o->mirror = -1;
        if (!s || !s->mirror[0] || !o->enabled) continue;
        for (int k = 0; k < AURORA_OUTPUTS; k++) if (k != i && outputs[k].connected && outputs[k].enabled && !strcmp(outputs[k].name, s->mirror)) o->mirror = k;
        if (o->mirror >= 0 && outputs[o->mirror].mirror >= 0) o->mirror = -1;   /* no chains */
    }
}

/* ---- public ---- */
static bool open_fbdev(void) {
    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC); struct fb_fix_screeninfo fix; struct fb_var_screeninfo var;
    if (fb < 0) return false;
    bool layout = false;
    if (!ioctl(fb, FBIOGET_FSCREENINFO, &fix) && !ioctl(fb, FBIOGET_VSCREENINFO, &var)) {
        fbdev_bgr = !var.red.offset && var.blue.offset == 16;   /* XBGR8888 (device.c reports it) */
        layout = var.bits_per_pixel == 32 && var.green.offset == 8 && (fbdev_bgr || (var.red.offset == 16 && !var.blue.offset));
    }
    if (!layout || fix.line_length < var.xres * 4 || fix.smem_len < (uint64_t)fix.line_length * var.yres) { close(fb); fprintf(stderr, "unsupported framebuffer\n"); return false; }
    void* map = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0); close(fb);
    if (map == MAP_FAILED) return false;
    fbdev = map; fbdev_bytes = fix.smem_len; fbdev_w = (int)var.xres; fbdev_h = (int)var.yres; fbdev_stride = fix.line_length / 4;
    return true;
}
static bool open_kms(void) {
    card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card < 0) return false;
    struct drm_mode_card_res res = {0};
    if (io(DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || res.count_crtcs != res.count_connectors) { close(card); card = -1; return false; }
    heads = res.count_crtcs > HEADS ? HEADS : (int)res.count_crtcs;
    res = (struct drm_mode_card_res){.crtc_id_ptr = (uintptr_t)crtc_ids, .connector_id_ptr = (uintptr_t)connector_ids, .count_crtcs = (uint32_t)heads, .count_connectors = (uint32_t)heads};
    if (io(DRM_IOCTL_MODE_GETRESOURCES, &res)) { close(card); card = -1; return false; }
    if (io(DRM_IOCTL_SET_MASTER, NULL) && errno != EINVAL) { /* already master (first opener) or refused below */ }
    struct drm_mode_crtc crtc = {.crtc_id = crtc_ids[0]};
    if (!io(DRM_IOCTL_MODE_GETCRTC, &crtc) && crtc.fb_id && crtc.mode_valid) {
        struct drm_mode_fb_cmd get = {.fb_id = crtc.fb_id};
        if (!io(DRM_IOCTL_MODE_GETFB, &get) && !get.handle && fbdev && (int)get.width == fbdev_w && (int)get.height == fbdev_h) { boot_fb = crtc.fb_id; boot_w = fbdev_w; boot_h = fbdev_h; }
    }
    uevents = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (uevents >= 0 && bind(uevents, (struct sockaddr*)&address, sizeof(address))) { close(uevents); uevents = -1; }
    kms = true; return true;
}
bool aurora_outputs_open(void) {
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < AURORA_OUTPUTS; i++) outputs[i].mirror = -1;
    bool have_fbdev = open_fbdev();
    load_saved();
    if (getenv("AURORA_NO_KMS") || !open_kms()) {
        if (!have_fbdev) return false;
        struct aurora_output* o = &outputs[0];
        *o = (struct aurora_output){.index = 0, .connected = true, .enabled = true, .primary = true, .w = fbdev_w, .h = fbdev_h, .scale = 1, .scale120 = 120, .mode_w = fbdev_w,
                                    .mode_h = fbdev_h, .refresh = 60, .mirror = -1, .mode_count = 1};
        snprintf(o->name, sizeof(o->name), "FB-1"); snprintf(o->model, sizeof(o->model), "Firmware framebuffer");
        o->modes[0] = (struct aurora_mode){fbdev_w, fbdev_h, 60, true};
        o->mm_w = fbdev_w * 254 / 960; o->mm_h = fbdev_h * 254 / 960;
        char error[160];
        if (!realize(error, sizeof(error))) { fprintf(stderr, "Aurora display: %s\n", error); return false; }
        printf("[64] AURORA OUTPUT %s fbdev %dx%d\n", o->name, fbdev_w, fbdev_h); fflush(stdout);
        return true;
    }
    int current_w = 0, current_h = 0;
    { struct drm_mode_crtc crtc = {.crtc_id = crtc_ids[0]}; if (!io(DRM_IOCTL_MODE_GETCRTC, &crtc) && crtc.mode_valid) { current_w = crtc.mode.hdisplay; current_h = crtc.mode.vdisplay; } }
    for (int i = 0; i < heads && i < AURORA_OUTPUTS; i++) {
        struct aurora_output o;
        bool ok = false; for (int attempt = 0; attempt < 3 && !(ok = probe_connector(i, &o)); attempt++) {}
        if (!ok || !o.connected || !o.mode_count) { if (kms) { struct drm_mode_crtc off = {.crtc_id = crtc_ids[i]}; if (i) io(DRM_IOCTL_MODE_SETCRTC, &off); } continue; }
        outputs[i] = o;
        place_new(&outputs[i], i == 0 ? current_w : 0, i == 0 ? current_h : 0);
    }
    if (!apply_saved_set()) resolve_mirrors();   /* the saved layout of this set of monitors, else per-monitor defaults */
    fix_primary();
    bool any = false; for (int i = 0; i < AURORA_OUTPUTS; i++) any |= outputs[i].enabled;
    if (!any) for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].connected) { outputs[i].enabled = true; fix_primary(); break; }
    char error[160];
    if (!realize(error, sizeof(error))) {
        fprintf(stderr, "Aurora display: %s; trying defaults\n", error);
        saved_count = 0;
        for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].connected) { outputs[i].mirror = -1; place_new(&outputs[i], i == 0 ? current_w : 0, i == 0 ? current_h : 0); }
        fix_primary();
        if (!realize(error, sizeof(error))) { fprintf(stderr, "Aurora display: %s\n", error); return false; }
    }
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; if (!o->connected) continue;
        printf("[64] AURORA OUTPUT %s %s %dx%d@%d at %d,%d scale=%d%s%s%s model=%s\n", o->name, o->enabled ? "on" : "off", o->mode_w, o->mode_h, o->refresh, o->x, o->y,
               o->scale120 * 5 / 6, o->primary ? " primary" : "", o->mirror >= 0 ? " mirror-of=" : "", o->mirror >= 0 ? outputs[o->mirror].name : "", o->model);
    }
    fflush(stdout);
    return true;
}
void aurora_outputs_close(void) {
    for (int i = 0; i < AURORA_OUTPUTS; i++) release_buffers(&outputs[i]);
    if (uevents >= 0) close(uevents);
    if (card >= 0) close(card);   /* the kernel restores the firmware framebuffer on CRTC 0 */
    if (fbdev) munmap(fbdev, fbdev_bytes);
    card = uevents = -1; fbdev = NULL; kms = false;
}
int aurora_outputs_count(void) { int n = 0; for (int i = 0; i < AURORA_OUTPUTS; i++) n += outputs[i].connected; return n; }
struct aurora_output* aurora_output_slot(int index) { return index >= 0 && index < AURORA_OUTPUTS && outputs[index].connected ? &outputs[index] : NULL; }
struct aurora_output* aurora_output_primary(void) {
    for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].enabled && outputs[i].primary) return &outputs[i];
    for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].enabled) return &outputs[i];
    return &outputs[0];
}
struct aurora_output* aurora_output_at(int x, int y) {
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i];
        if (o->enabled && o->mirror < 0 && x >= o->x && y >= o->y && x < o->x + o->w && y < o->y + o->h) return o;
    }
    return NULL;
}
struct aurora_output* aurora_output_nearest(int x, int y) {
    struct aurora_output* best = NULL; int64_t distance = 0;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; if (!o->enabled || o->mirror >= 0) continue;
        int64_t cx = x < o->x ? o->x : x >= o->x + o->w ? o->x + o->w - 1 : x, cy = y < o->y ? o->y : y >= o->y + o->h ? o->y + o->h - 1 : y;
        int64_t d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (!best || d < distance) { best = o; distance = d; }
    }
    return best ? best : aurora_output_primary();
}
void aurora_outputs_bounds(int* w, int* h) {
    *w = *h = 0;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; if (!o->enabled) continue;
        if (o->x + o->w > *w) *w = o->x + o->w;
        if (o->y + o->h > *h) *h = o->y + o->h;
    }
}
bool aurora_outputs_kms(void) { return kms; }
void aurora_outputs_clamp(int64_t* x, int64_t* y) {
    if (*x > INT32_MAX / 2) *x = INT32_MAX / 2; if (*x < INT32_MIN / 2) *x = INT32_MIN / 2;
    if (*y > INT32_MAX / 2) *y = INT32_MAX / 2; if (*y < INT32_MIN / 2) *y = INT32_MIN / 2;
    if (aurora_output_at((int)*x, (int)*y)) return;
    struct aurora_output* o = aurora_output_nearest((int)*x, (int)*y);
    if (*x < o->x) *x = o->x; if (*x >= o->x + o->w) *x = o->x + o->w - 1;
    if (*y < o->y) *y = o->y; if (*y >= o->y + o->h) *y = o->y + o->h - 1;
}
void aurora_output_present(struct aurora_output* o, const int32_t (*rects)[4], int n) {
    if (!o->enabled || !o->canvas || n <= 0) return;
    struct drm_clip_rect clips[64]; int clip_count = 0;
    for (int k = 0; k < n; k++) {
        int x = rects[k][0], y = rects[k][1], w = rects[k][2], h = rects[k][3];
        if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
        if (x + w > o->mode_w) w = o->mode_w - x; if (y + h > o->mode_h) h = o->mode_h - y;
        if (w <= 0 || h <= 0) continue;
        if (o->canvas != o->scan) {
            for (int row = y; row < y + h; row++) memcpy(o->scan + (size_t)row * o->scan_stride + x, o->canvas + (size_t)row * o->mode_w + x, (size_t)w * 4);
            if (present_filter) present_filter(o->scan, o->scan_stride, x, y, w, h);
            if (o->fbdev && fbdev_bgr) swap_rb(o->scan, o->scan_stride, x, y, w, h);
        }
        if (clip_count < 64) clips[clip_count++] = (struct drm_clip_rect){(uint16_t)x, (uint16_t)y, (uint16_t)(x + w), (uint16_t)(y + h)};
        else clips[63] = (struct drm_clip_rect){0, 0, (uint16_t)o->mode_w, (uint16_t)o->mode_h};
        /* scaled mirrors (another mode): nearest-neighbour copy of the same logical area into their own scanout */
        for (int i = 0; i < AURORA_OUTPUTS; i++) {
            struct aurora_output* m = &outputs[i];
            if (!m->enabled || m->mirror != o->index || !m->scan) continue;   /* (a same-mode mirror has no scan: it shares o's framebuffer) */
            int mx0 = (int)((int64_t)x * m->mode_w / o->mode_w), mx1 = (int)(((int64_t)(x + w) * m->mode_w + o->mode_w - 1) / o->mode_w);
            int my0 = (int)((int64_t)y * m->mode_h / o->mode_h), my1 = (int)(((int64_t)(y + h) * m->mode_h + o->mode_h - 1) / o->mode_h);
            if (mx1 > m->mode_w) mx1 = m->mode_w; if (my1 > m->mode_h) my1 = m->mode_h;
            for (int my = my0; my < my1; my++) {
                const uint32_t* source = o->canvas + (size_t)((int64_t)my * o->mode_h / m->mode_h) * o->mode_w;
                uint32_t* target = m->scan + (size_t)my * m->scan_stride;
                for (int mx = mx0; mx < mx1; mx++) target[mx] = source[(int64_t)mx * o->mode_w / m->mode_w];
            }
            if (m->fbdev && fbdev_bgr && mx1 > mx0 && my1 > my0) swap_rb(m->scan, m->scan_stride, mx0, my0, mx1 - mx0, my1 - my0);
            if (kms && mx1 > mx0 && my1 > my0) {
                struct drm_clip_rect c = {(uint16_t)mx0, (uint16_t)my0, (uint16_t)mx1, (uint16_t)my1};
                struct drm_mode_fb_dirty_cmd d = {.fb_id = m->fb, .num_clips = 1, .clips_ptr = (uintptr_t)&c};
                io(DRM_IOCTL_MODE_DIRTYFB, &d);
            }
        }
    }
    if (kms && clip_count && o->fb) {
        struct drm_mode_fb_dirty_cmd d = {.fb_id = o->fb, .num_clips = (uint32_t)clip_count, .clips_ptr = (uintptr_t)clips};
        if (io(DRM_IOCTL_MODE_DIRTYFB, &d) && errno != EACCES) perror("Aurora DIRTYFB");
    }
}
int aurora_outputs_event_fd(void) { return uevents; }
void aurora_outputs_filter(void (*filter)(uint32_t* pixels, size_t stride, int x, int y, int w, int h)) {
    if (filter == present_filter) return;
    present_filter = filter;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {   /* in-place canvases get a RAM copy (or lose it) */
        struct aurora_output* o = &outputs[i]; if (!o->enabled || o->mirror >= 0 || o->fbdev || !o->scan) continue;
        if (filter && o->canvas == o->scan) {
            uint32_t* copy = malloc((size_t)o->mode_w * o->mode_h * 4); if (!copy) continue;
            for (int row = 0; row < o->mode_h; row++) memcpy(copy + (size_t)row * o->mode_w, o->scan + (size_t)row * o->scan_stride, (size_t)o->mode_w * 4);
            o->canvas = copy;
        } else if (!filter && o->canvas != o->scan) {
            for (int row = 0; row < o->mode_h; row++) memcpy(o->scan + (size_t)row * o->scan_stride, o->canvas + (size_t)row * o->mode_w, (size_t)o->mode_w * 4);
            free(o->canvas); o->canvas = o->scan;
        }
    }
}
bool aurora_outputs_rescan(void) {
    if (!kms) return false;
    char message[4096]; bool hotplug = false;
    while (uevents >= 0) {
        ssize_t n = recv(uevents, message, sizeof(message) - 1, MSG_DONTWAIT);
        if (n <= 0) break;
        message[n] = 0;
        for (ssize_t at = 0; at < n; at += (ssize_t)strlen(message + at) + 1) if (!strcmp(message + at, "HOTPLUG=1")) hotplug = true;
    }
    if (!hotplug) return false;
    bool changed = false;
    for (int i = 0; i < heads && i < AURORA_OUTPUTS; i++) {
        struct aurora_output probe; bool ok = false;
        for (int attempt = 0; attempt < 3 && !(ok = probe_connector(i, &probe)); attempt++) {}
        if (!ok) continue;
        struct aurora_output* o = &outputs[i];
        bool now = probe.connected && probe.mode_count;
        char a[80], b[80]; identity(o, a, sizeof(a)); identity(&probe, b, sizeof(b));
        if (o->connected && (!now || strcmp(a, b))) {   /* unplugged (or another monitor on this connector) */
            printf("[64] AURORA OUTPUT %s removed\n", o->name); fflush(stdout);
            if (o->crtc) { struct drm_mode_crtc off = {.crtc_id = o->crtc}; io(DRM_IOCTL_MODE_SETCRTC, &off); }
            for (int k = 0; k < AURORA_OUTPUTS; k++) if (outputs[k].mirror == i) outputs[k].mirror = -1;
            release_buffers(o); memset(o, 0, sizeof(*o)); o->mirror = -1; changed = true;
        }
        if (now && !o->connected) {
            *o = probe; place_new(o, 0, 0); changed = true;
            printf("[64] AURORA OUTPUT %s added %dx%d\n", o->name, o->mode_w, o->mode_h); fflush(stdout);
        } else if (now) {   /* still there: refresh the mode list; an automatic mode follows the preferred one */
            memcpy(o->modes, probe.modes, sizeof(o->modes)); o->mode_count = probe.mode_count; o->mm_w = probe.mm_w; o->mm_h = probe.mm_h;
            const struct saved* s = saved_for(o);
            int preferred = 0; for (int k = 0; k < o->mode_count; k++) if (o->modes[k].preferred) { preferred = k; break; }
            if (o->index != 0 && (!s || s->automode) && (o->modes[preferred].w != o->mode_w || o->modes[preferred].h != o->mode_h)) {
                o->mode_w = o->modes[preferred].w; o->mode_h = o->modes[preferred].h; o->refresh = o->modes[preferred].refresh; changed = true;
            } else if (mode_index(o, o->mode_w, o->mode_h, 0) < 0 && o->mode_count) {
                o->mode_w = o->modes[preferred].w; o->mode_h = o->modes[preferred].h; o->refresh = o->modes[preferred].refresh; changed = true;
            }
        }
    }
    if (!changed) return false;
    if (!apply_saved_set()) resolve_mirrors();
    fix_primary();
    bool any = false; for (int i = 0; i < AURORA_OUTPUTS; i++) any |= outputs[i].enabled;
    if (!any) for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].connected) { outputs[i].enabled = true; fix_primary(); break; }
    char error[160];
    if (!realize(error, sizeof(error))) fprintf(stderr, "Aurora display rescan: %s\n", error);
    return true;
}
bool aurora_outputs_apply(const struct aurora_output_request* requests, int count, char* error, size_t size) {
    struct aurora_output before[AURORA_OUTPUTS]; memcpy(before, outputs, sizeof(before));
    bool seen[AURORA_OUTPUTS] = {0};
    for (int r = 0; r < count; r++) {
        const struct aurora_output_request* q = &requests[r]; struct aurora_output* o = NULL;
        for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].connected && !strcmp(outputs[i].name, q->name)) o = &outputs[i];
        if (!o) { snprintf(error, size, "no connected display %s", q->name); goto refuse; }
        if (seen[o->index]) { snprintf(error, size, "%s given twice", q->name); goto refuse; }
        seen[o->index] = true;
        if (!AURORA_SCALE_VALID(q->scale)) { snprintf(error, size, "%s: scale must be 100%% to 400%% in steps of 25%%", q->name); goto refuse; }
        if (q->enabled && mode_index(o, q->mode_w, q->mode_h, q->refresh) < 0) { snprintf(error, size, "%s: no mode %dx%d@%d", q->name, q->mode_w, q->mode_h, q->refresh); goto refuse; }
        if (q->enabled && ((int64_t)q->mode_w * 100 / q->scale < 640 || (int64_t)q->mode_h * 100 / q->scale < 360)) {
            snprintf(error, size, "%s: %d%% leaves less than 640x360 of %dx%d", q->name, q->scale, q->mode_w, q->mode_h); goto refuse; }
        if (q->x < -32768 || q->x > 32768 || q->y < -32768 || q->y > 32768) { snprintf(error, size, "%s: position out of range", q->name); goto refuse; }
        o->enabled = q->enabled; o->primary = q->primary && q->enabled; o->x = q->x; o->y = q->y;
        if (q->enabled) { o->mode_w = q->mode_w; o->mode_h = q->mode_h; o->refresh = o->modes[mode_index(o, q->mode_w, q->mode_h, q->refresh)].refresh; set_scale(o, q->scale); }
        else set_scale(o, o->scale120 * 5 / 6);
        o->mirror = -1;
    }
    for (int r = 0; r < count; r++) if (requests[r].mirror[0] && requests[r].enabled) {
        struct aurora_output* o = NULL, *m = NULL;
        for (int i = 0; i < AURORA_OUTPUTS; i++) { if (outputs[i].connected && !strcmp(outputs[i].name, requests[r].name)) o = &outputs[i]; if (outputs[i].connected && !strcmp(outputs[i].name, requests[r].mirror)) m = &outputs[i]; }
        if (!m || m == o || !m->enabled) { snprintf(error, size, "%s: cannot mirror %s", requests[r].name, requests[r].mirror); goto refuse; }
        o->mirror = m->index; o->primary = false;
    }
    for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].mirror >= 0 && outputs[outputs[i].mirror].mirror >= 0) { snprintf(error, size, "%s mirrors a mirror", outputs[i].name); goto refuse; }
    bool any = false;
    for (int i = 0; i < AURORA_OUTPUTS; i++) { struct aurora_output* o = &outputs[i]; if (o->enabled && o->mirror < 0) any = true; }
    if (!any) { snprintf(error, size, "at least one display must stay on"); goto refuse; }
    for (int i = 0; i < AURORA_OUTPUTS; i++) for (int k = i + 1; k < AURORA_OUTPUTS; k++) {
        struct aurora_output* a = &outputs[i], *b = &outputs[k];
        if (a->enabled && b->enabled && a->mirror < 0 && b->mirror < 0 && overlap(a, b)) { snprintf(error, size, "%s and %s overlap", a->name, b->name); goto refuse; }
    }
    fix_primary();
    if (!realize(error, size)) { restore_layout(before); return false; }
    save();
    return true;
refuse:
    memcpy(outputs, before, sizeof(before));
    return false;
}
/* ---- control socket: "displays" (state) and "apply-displays" (a block of "display NAME key=value..." lines) ---- */
static void (*layout_changed)(void);
static bool control_displays(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size;
    char text[16384]; aurora_outputs_describe(text, sizeof(text)); aurora_control_print(reply, "%s", text); return true;
}
static bool control_apply(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)reply;
    struct aurora_output_request requests[AURORA_OUTPUTS]; int count = 0;
    for (const char* line = body; line && *line;) {
        const char* eol = strchr(line, '\n'); size_t length = eol ? (size_t)(eol - line) : strlen(line);
        char text[256]; if (length >= sizeof(text)) { snprintf(error, size, "line too long"); return false; }
        memcpy(text, line, length); text[length] = 0; line = eol ? eol + 1 : NULL;
        if (!text[0] || text[0] == '#') continue;
        if (count == AURORA_OUTPUTS) { snprintf(error, size, "too many displays"); return false; }
        struct aurora_output_request* q = &requests[count]; memset(q, 0, sizeof(*q)); q->enabled = true; q->scale = 100;
        char* save = NULL; char* word = strtok_r(text, " \t", &save);
        if (!word || strcmp(word, "display") || !(word = strtok_r(NULL, " \t", &save)) || strlen(word) >= sizeof(q->name)) { snprintf(error, size, "expected: display NAME key=value..."); return false; }
        snprintf(q->name, sizeof(q->name), "%s", word);
        bool have_mode = false;
        while ((word = strtok_r(NULL, " \t", &save))) {
            int a, b, c; char extra;
            if (sscanf(word, "enabled=%d%c", &a, &extra) == 1 && (a == 0 || a == 1)) q->enabled = a;
            else if (sscanf(word, "primary=%d%c", &a, &extra) == 1 && (a == 0 || a == 1)) q->primary = a;
            else if (sscanf(word, "x=%d%c", &a, &extra) == 1) q->x = a;
            else if (sscanf(word, "y=%d%c", &a, &extra) == 1) q->y = a;
            else if (sscanf(word, "mode=%dx%d@%d%c", &a, &b, &c, &extra) == 3 && a > 0 && b > 0 && c >= 0) { q->mode_w = a; q->mode_h = b; q->refresh = c; have_mode = true; }
            else if (sscanf(word, "mode=%dx%d%c", &a, &b, &extra) == 2 && a > 0 && b > 0) { q->mode_w = a; q->mode_h = b; q->refresh = 0; have_mode = true; }
            else if (!strncmp(word, "scale=", 6) && (sscanf(word, "scale=%d%c", &a, &extra) == 1 || (sscanf(word, "scale=%d%c%c", &a, &extra, &extra) == 2 && extra == '%')))
                q->scale = a >= 1 && a <= 4 ? a * 100 : a;   /* 150% (percent) or a whole 1..4 */
            else if (!strncmp(word, "mirror=", 7) && strlen(word + 7) < sizeof(q->mirror)) snprintf(q->mirror, sizeof(q->mirror), "%s", strcmp(word + 7, "-") ? word + 7 : "");
            else { snprintf(error, size, "bad setting %.60s", word); return false; }
        }
        if (q->enabled && !have_mode) {   /* keep the current mode */
            for (int i = 0; i < AURORA_OUTPUTS; i++) if (outputs[i].connected && !strcmp(outputs[i].name, q->name)) { q->mode_w = outputs[i].mode_w; q->mode_h = outputs[i].mode_h; q->refresh = outputs[i].refresh; }
        }
        count++;
    }
    if (!count) { snprintf(error, size, "no displays given"); return false; }
    if (!aurora_outputs_apply(requests, count, error, size)) return false;
    if (layout_changed) layout_changed();
    return true;
}
/* "scale NAME PERCENT": one display's scale, everything else as it is (scripts, tests). */
static bool control_scale(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; char name[32]; int percent; char extra;
    if (!args || sscanf(args, "%31s %d%c", name, &percent, &extra) != 2) { snprintf(error, size, "expected: scale NAME PERCENT"); return false; }
    struct aurora_output_request requests[AURORA_OUTPUTS]; int count = 0; bool found = false;
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; if (!o->connected) continue;
        struct aurora_output_request* q = &requests[count++];
        *q = (struct aurora_output_request){.enabled = o->enabled, .primary = o->primary, .x = o->x, .y = o->y, .mode_w = o->mode_w, .mode_h = o->mode_h,
                                            .refresh = o->refresh, .scale = o->scale120 * 5 / 6};
        snprintf(q->name, sizeof(q->name), "%s", o->name);
        if (o->mirror >= 0) snprintf(q->mirror, sizeof(q->mirror), "%s", outputs[o->mirror].name);
        if (!strcmp(o->name, name)) { q->scale = percent; found = true; }
    }
    if (!found) { snprintf(error, size, "no display %s", name); return false; }
    if (!aurora_outputs_apply(requests, count, error, size)) return false;
    if (layout_changed) layout_changed();
    return true;
}
void aurora_outputs_control(void (*changed)(void)) {
    layout_changed = changed;
    aurora_control_register("scale", false, control_scale);
    aurora_control_register("displays", false, control_displays);
    aurora_control_register("apply-displays", true, control_apply);
}
size_t aurora_outputs_describe(char* text, size_t size) {
    size_t used = 0;
#define ADD(...) do { int n = snprintf(text + used, used < size ? size - used : 0, __VA_ARGS__); if (n > 0) used += (size_t)n; } while (0)
    ADD("backend %s\n", kms ? "kms" : "fbdev");
    for (int i = 0; i < AURORA_OUTPUTS; i++) {
        struct aurora_output* o = &outputs[i]; if (!o->connected) continue;
        char make[16], model[32], serial[24];   /* one word each: spaces become '_' */
        snprintf(make, sizeof(make), "%s", o->make[0] ? o->make : "-"); snprintf(model, sizeof(model), "%s", o->model[0] ? o->model : "-");
        snprintf(serial, sizeof(serial), "%s", o->serial[0] ? o->serial : "-");
        for (char* p = make; *p; p++) if (*p == ' ') *p = '_'; for (char* p = model; *p; p++) if (*p == ' ') *p = '_'; for (char* p = serial; *p; p++) if (*p == ' ') *p = '_';
        ADD("output %s enabled=%d primary=%d x=%d y=%d w=%d h=%d mode=%dx%d@%d scale=%d mirror=%s mm=%dx%d make=%s model=%s serial=%s\n", o->name, o->enabled, o->primary,
            o->x, o->y, o->w, o->h, o->mode_w, o->mode_h, o->refresh, o->scale120 * 5 / 6, o->mirror >= 0 ? outputs[o->mirror].name : "-", o->mm_w, o->mm_h, make, model, serial);
        for (int k = 0; k < o->mode_count; k++) ADD("mode %s %dx%d@%d%s\n", o->name, o->modes[k].w, o->modes[k].h, o->modes[k].refresh, o->modes[k].preferred ? " preferred" : "");
    }
#undef ADD
    if (used >= size && size) text[size - 1] = 0;
    return used < size ? used : size ? size - 1 : 0;
}
