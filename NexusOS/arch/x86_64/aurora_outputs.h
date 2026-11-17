/* Aurora displays (aurora_outputs.c): the outputs of the desktop and their scanout.
 *
 * KMS backend (/dev/dri/card0 with modesetting: virtio-gpu): one output per connected connector, each with a mode,
 * a position in the desktop, a scale (100..400 %, steps of 25 %), an optional mirror source, and its own composition canvas
 * (mode-sized, physical pixels). Presentation copies damaged rectangles from the canvas to the scanout buffer and
 * reports them with DRM_IOCTL_MODE_DIRTYFB. Head 0 keeps the firmware framebuffer (/dev/fb0) as its scanout
 * buffer while its mode equals the boot mode, so fbdev readers keep seeing the screen. Hot-plug arrives as a kernel
 * uevent (NETLINK_KOBJECT_UEVENT) on aurora_outputs_event_fd().
 * fbdev fallback (no KMS: VMware SVGA, UEFI GOP, real GPUs without a driver): one output on /dev/fb0.
 *
 * Desktop ("logical") coordinates: every enabled output covers the rectangle x,y,w,h (w,h = mode * 120 / scale120 rounded
 * down; aurora_output_phys() maps the logical edge to the mode's edge); the
 * bounding box of all outputs starts at 0,0. Mirrors show their source's rectangle. The layout is persisted in
 * AURORA_DISPLAYS_CONF (per monitor identity: connector + EDID make/model/serial). */
#ifndef AURORA_OUTPUTS_H
#define AURORA_OUTPUTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AURORA_OUTPUTS 8
#define AURORA_MODES 48
/* Aurora's persistent machine state lives in the owner's home: on FAT roots only /home/nexus, /tmp and /run/user/1000
 * are writable (kernel.c private_path), so /etc cannot hold it. */
#define AURORA_STATE_DIR "/home/nexus/.aurora"
#define AURORA_DISPLAYS_CONF AURORA_STATE_DIR "/displays.conf"
struct aurora_mode { int w, h, refresh; bool preferred; };
struct aurora_output {
    int index;                                 /* slot (stable while connected) */
    char name[32], make[16], model[32], serial[24];
    bool connected, enabled, primary;
    int x, y, w, h;                            /* logical rectangle */
    int scale;                                 /* wl_output scale: scale120 rounded up to a whole number (1..4) */
    int scale120;                              /* real scale in 1/120 (as wp_fractional_scale_v1): 120 = 100 %, 180 = 150 % */
    int mode_w, mode_h, refresh;               /* current mode, pixels / Hz */
    int mirror;                                /* slot this output mirrors, or -1 */
    int mm_w, mm_h;
    struct aurora_mode modes[AURORA_MODES]; int mode_count;
    uint32_t* canvas;                          /* mode_w x mode_h composition buffer (NULL for a mirror) */
    uint32_t* wallpaper;                       /* mode_w x mode_h wallpaper */
    uint32_t* scan; size_t scan_stride, scan_bytes; bool fbdev;   /* scanout buffer (pixels, stride in pixels) */
    uint32_t crtc, connector, fb, handle; bool own_fb;            /* KMS objects */
    int buffer_w, buffer_h;                    /* size of scan/canvas/wallpaper (0: none) */
    void* global;                              /* struct wl_global* of its wl_output (desktop.c) */
    unsigned serial_number;                    /* changes whenever mode/position/scale/enabled change */
};
/* A requested layout for one output (Settings > Displays / the control socket). */
struct aurora_output_request { char name[32]; bool enabled, primary; int x, y, mode_w, mode_h, refresh, scale; char mirror[32]; };   /* scale: percent */
#define AURORA_SCALE_VALID(percent) ((percent) >= 100 && (percent) <= 400 && (percent) % 25 == 0)
/* Physical offset of logical offset d from an output's origin (span logical = pspan physical pixels, scale120): rounded
 * down, the logical edge lands on the physical edge, monotonic beyond both edges. */
static inline int64_t aurora_output_phys(int64_t d, int scale120, int span, int pspan) {
    if (d >= span) return pspan + (d - span) * scale120 / 120;
    return d >= 0 ? d * scale120 / 120 : -((-d * scale120 + 119) / 120);
}
#define AURORA_PHYS_X(o, value) aurora_output_phys((int64_t)(value) - (o)->x, (o)->scale120, (o)->w, (o)->mode_w)
#define AURORA_PHYS_Y(o, value) aurora_output_phys((int64_t)(value) - (o)->y, (o)->scale120, (o)->h, (o)->mode_h)

bool aurora_outputs_open(void);                /* probe and light up the outputs (saved layout or defaults) */
void aurora_outputs_close(void);
int aurora_outputs_count(void);                /* slots in use (connected outputs, enabled or not) */
struct aurora_output* aurora_output_slot(int index);   /* NULL for an unused slot */
struct aurora_output* aurora_output_primary(void);     /* never NULL after a successful open */
struct aurora_output* aurora_output_at(int x, int y);  /* enabled, non-mirror output containing x,y, or NULL */
struct aurora_output* aurora_output_nearest(int x, int y);
void aurora_outputs_bounds(int* w, int* h);
bool aurora_outputs_kms(void);
/* Clamp x,y into the union of the enabled outputs (nearest point of the nearest output). */
void aurora_outputs_clamp(int64_t* x, int64_t* y);
/* Copy n physical rectangles of o's canvas to its scanout (and to its scaled mirrors) and present them. */
void aurora_output_present(struct aurora_output* o, const int32_t (*rects)[4], int n);
/* Wallpaper for output-local physical rectangle x,y,w,h into dst (stride in pixels). */
void aurora_output_wallpaper(const struct aurora_output* o, uint32_t* dst, size_t stride, int x, int y, int w, int h);
int aurora_outputs_event_fd(void);             /* uevent socket, -1 without KMS */
/* A colour filter applied to what is presented (accessibility): every output then composes in a RAM canvas and the
 * filter runs on the copy in the scanout buffer. NULL: off. */
void aurora_outputs_filter(void (*filter)(uint32_t* pixels, size_t stride, int x, int y, int w, int h));
/* After a uevent (or periodically): re-probe. Returns true when the layout changed (outputs added, removed, resized). */
bool aurora_outputs_rescan(void);
/* Apply a layout; error receives a reason on failure (nothing changes then). Saves on success. */
bool aurora_outputs_apply(const struct aurora_output_request* requests, int count, char* error, size_t error_size);
/* Text description of every output and its modes (control socket "displays"). */
size_t aurora_outputs_describe(char* text, size_t size);
/* Register "displays" and "apply-displays" on the control socket; changed() runs after a layout was applied. */
void aurora_outputs_control(void (*changed)(void));
#endif
