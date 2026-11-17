/* Virtual test camera (like Linux's vivid), boot option "vcam": /dev/videoN "NexusOS Virtual Camera" producing a known
 * pattern at the requested rate, so the whole capture path (V4L2 ABI, mmap streaming, Chrome/Firefox getUserMedia
 * video) is testable where no real camera exists (QEMU has no UVC device).
 *
 * Formats YUYV and NV12 at 320x240, 640x480 (default), 1280x720; 30 or 15 frames/s. Picture (BT.601 limited range):
 * eight vertical colour bars (white, yellow, cyan, green, magenta, red, blue, black) over the top three quarters, and
 * a frame-counter strip below: 16 blocks, block i white when bit (15 - i) of the frame's sequence number is set, else
 * black - a test reads it to check frames arrive in order without repeats. Controls: Brightness (0..255, offsets Y by
 * value - 128), Saturation (0..200 %, scales the chroma), Power Line Frequency (menu, accepted as cameras do).
 * Frames are made in v4l2_poll (scheduler loop) when due: frame k of a stream at start + k * interval; a frame that
 * finds no queued buffer is dropped (its sequence number is skipped), like a real camera with a slow reader. */
#include "v4l2.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

enum { CID_BRIGHTNESS = 0x00980900, CID_SATURATION = 0x00980902, CID_POWER_LINE_FREQUENCY = 0x00980918 };
struct vcam {
    struct v4l2_cam cam;
    bool streaming; uint32_t fourcc; unsigned width, height; uint64_t interval_ns, start_ns, next_ns, frames;
    uint8_t row[2 * 1280];                               /* one YUYV row, or the Y / UV rows of NV12 */
};
static struct vcam camera;
static const uint8_t BARS[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34}, {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};

static uint8_t clamp8(int v) { return (uint8_t)(v < 16 ? 16 : v > 235 ? 235 : v); }
static uint8_t chroma(int c, int saturation) { int v = 128 + (c - 128) * saturation / 100; return (uint8_t)(v < 16 ? 16 : v > 240 ? 240 : v); }
static int control(uint32_t id) { for (unsigned i = 0; i < camera.cam.control_count; i++) if (camera.cam.controls[i].id == id) return camera.cam.controls[i].value; return 0; }
/* YUYV row: bars (counter = false) or the counter strip for `sequence`. */
static void row_yuyv(uint8_t* out, unsigned width, bool counter, uint32_t sequence) {
    int brightness = control(CID_BRIGHTNESS) - 128, saturation = control(CID_SATURATION);
    for (unsigned x = 0; x < width; x += 2) {
        uint8_t y, u, v;
        if (counter) { bool on = (sequence >> (15 - x * 16 / width)) & 1; y = on ? 235 : 16; u = v = 128; }
        else { const uint8_t* bar = BARS[x * 8 / width]; y = bar[0]; u = chroma(bar[1], saturation); v = chroma(bar[2], saturation); }
        uint8_t yy = clamp8(y + brightness);
        out[2 * x] = yy; out[2 * x + 1] = u; out[2 * x + 2] = yy; out[2 * x + 3] = v;
    }
}
static void produce(uint32_t sequence) {
    uint32_t capacity = 0; struct v4l2_frame* frame = v4l2_frame_begin(&camera.cam, &capacity);
    if (!frame) { v4l2_frame_drop(&camera.cam); return; }
    unsigned w = camera.width, h = camera.height, split = h * 3 / 4 & ~1U;
    uint32_t offset = 0;
    if (camera.fourcc == V4L2_FMT_YUYV) {
        uint32_t size = w * h * 2; if (size > capacity) { v4l2_frame_end(&camera.cam, frame, 0, true); return; }
        row_yuyv(camera.row, w, false, sequence);
        for (unsigned y = 0; y < split; y++, offset += w * 2) v4l2_frame_write(frame, offset, camera.row, w * 2);
        row_yuyv(camera.row, w, true, sequence);
        for (unsigned y = split; y < h; y++, offset += w * 2) v4l2_frame_write(frame, offset, camera.row, w * 2);
        v4l2_frame_end(&camera.cam, frame, size, false); return;
    }
    /* NV12: Y plane, then interleaved UV at half resolution */
    uint32_t size = w * h * 3 / 2; if (size > capacity) { v4l2_frame_end(&camera.cam, frame, 0, true); return; }
    uint8_t yuyv[2 * 1280];
    for (unsigned band = 0; band < 2; band++) {
        row_yuyv(yuyv, w, band == 1, sequence);
        for (unsigned x = 0; x < w; x++) camera.row[x] = yuyv[2 * x];
        for (unsigned y = band ? split : 0; y < (band ? h : split); y++) v4l2_frame_write(frame, y * w, camera.row, w);
        for (unsigned x = 0; x < w; x += 2) { camera.row[x] = yuyv[2 * x + 1]; camera.row[x + 1] = yuyv[2 * x + 3]; }
        for (unsigned y = (band ? split : 0) / 2; y < (band ? h : split) / 2; y++) v4l2_frame_write(frame, w * h + y * w, camera.row, w);
    }
    v4l2_frame_end(&camera.cam, frame, size, false);
}
static int vcam_start(struct v4l2_cam* cam, const struct v4l2_cam_format* format, const struct v4l2_cam_size* size, uint32_t interval) {
    (void)cam;
    if (size->width > 1280 || size->height > 720) return -22;
    camera.fourcc = format->fourcc; camera.width = size->width; camera.height = size->height;
    camera.interval_ns = (uint64_t)interval * 100; if (camera.interval_ns < 1000000) camera.interval_ns = 33333300;
    camera.start_ns = camera.next_ns = native_clock_ns() + camera.interval_ns; camera.frames = 0; camera.streaming = true;
    return 0;
}
static void vcam_stop(struct v4l2_cam* cam) { (void)cam; camera.streaming = false; }
static int vcam_set_control(struct v4l2_cam* cam, struct v4l2_cam_control* c, int32_t value) { (void)cam; (void)c; (void)value; return 0; }
static void vcam_poll(struct v4l2_cam* cam, uint64_t now) {
    (void)cam;
    if (!camera.streaming || now < camera.next_ns) return;
    uint64_t due = (now - camera.start_ns) / camera.interval_ns + 1;      /* frames due by now */
    if (due - camera.frames > 8) { while (due - camera.frames > 1) { v4l2_frame_drop(&camera.cam); camera.frames++; } }   /* long stall: skip */
    while (camera.frames < due && camera.streaming) { produce((uint32_t)camera.frames); camera.frames++; }
    camera.next_ns = camera.start_ns + camera.frames * camera.interval_ns;
}
static uint64_t vcam_next(struct v4l2_cam* cam, uint64_t now) { (void)cam; return camera.streaming ? (camera.next_ns > now ? camera.next_ns : now) : ~0ULL; }
static const struct v4l2_cam_ops ops = {vcam_start, vcam_stop, vcam_set_control, vcam_poll, vcam_next};
static void add_format(uint32_t fourcc, const char* description) {
    struct v4l2_cam_format* f = &camera.cam.formats[camera.cam.format_count++];
    f->fourcc = fourcc; strcpy(f->description, description);
    static const uint16_t sizes[3][2] = {{640, 480}, {1280, 720}, {320, 240}};
    for (unsigned s = 0; s < 3; s++) {
        struct v4l2_cam_size* z = &f->sizes[f->size_count++]; z->width = sizes[s][0]; z->height = sizes[s][1];
        z->intervals[0] = 333333; z->intervals[1] = 666666; z->interval_count = 2;
    }
}
static void add_control(uint32_t id, unsigned type, const char* name, int min, int max, int step, int def) {
    struct v4l2_cam_control* c = &camera.cam.controls[camera.cam.control_count++];
    c->id = id; c->type = (uint8_t)type; strcpy(c->name, name); c->minimum = min; c->maximum = max; c->step = step; c->def = c->value = def;
}
void vcam_init(void) {
    if (!boot_option("vcam")) return;
    memset(&camera, 0, sizeof(camera));
    strcpy(camera.cam.card, "NexusOS Virtual Camera"); strcpy(camera.cam.driver, "vcam"); strcpy(camera.cam.bus_info, "platform:vcam-0");
    add_format(V4L2_FMT_YUYV, "YUYV 4:2:2"); add_format(V4L2_FMT_NV12, "Y/UV 4:2:0");
    add_control(CID_BRIGHTNESS, V4L2_CAM_CONTROL_INTEGER, "Brightness", 0, 255, 1, 128);
    add_control(CID_SATURATION, V4L2_CAM_CONTROL_INTEGER, "Saturation", 0, 200, 1, 100);
    add_control(CID_POWER_LINE_FREQUENCY, V4L2_CAM_CONTROL_MENU, "Power Line Frequency", 0, 2, 1, 1);
    struct v4l2_cam_control* plf = &camera.cam.controls[camera.cam.control_count - 1];
    strcpy(plf->menu[0], "Disabled"); strcpy(plf->menu[1], "50 Hz"); strcpy(plf->menu[2], "60 Hz");
    camera.cam.ops = &ops; camera.cam.context = &camera;
    if (v4l2_register(&camera.cam) < 0) vga_print("[64] VCAM: no free video device\n");
}
