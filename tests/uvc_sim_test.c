/* Host simulator for the USB Video Class driver (arch/x86_64/uvc.c): QEMU has no UVC device, so the driver's real entry
 * points run here against simulated cameras built from real descriptor layouts:
 *  - "c270": the Logitech C270 HD Webcam (046d:0825, UVC 1.0, high speed) layout - camera terminal, processing unit
 *    (bControlSize 2, bmControls 0x175B), YUY2 format 1 with nine frame sizes and seven discrete intervals, MJPEG format
 *    2, isochronous alternate settings 1..11 (wMaxPacketSize 0x00C0 .. 0x13FC, high-bandwidth multipliers);
 *  - "bulk": a UVC 1.1 camera that streams NV12 over a bulk endpoint in alternate setting 0;
 *  - hostile descriptor sets and a fuzzer.
 * The simulated device answers PROBE/COMMIT (and can refuse or change the format), the processing-unit requests, and
 * delivers payloads with real headers (12-byte PTS/SCR, FID toggling, EOF, ERR) at the negotiated payload size.
 * Checks: format/size/interval/control discovery, probe -> alternate-setting choice by bandwidth, frame reassembly
 * (bytes and content), every error path (ERR bit, lost transfer, oversize, short frame, header garbage, FID toggle
 * without EOF, no buffer queued -> drop), STREAMOFF/STREAMON, hot-unplug while streaming, all memory returned. */
#define NEXUS_HOST_TEST 1
#include "../arch/x86_64/usb.h"
#include "../arch/x86_64/v4l2.h"
#include "../kernel/string.h"
#include "../kernel/vga.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>

static unsigned allocations; static uint64_t allocated;
uint64_t arena_alloc(uint64_t bytes) { uint8_t* b = calloc(1, bytes + 4096); assert(b); allocations++; allocated += bytes; return ((uint64_t)(uintptr_t)b + 4095) & ~4095ULL; }
uint64_t dma32_alloc(uint64_t bytes) { return arena_alloc(bytes); }
void arena_free(uint64_t address, uint64_t bytes) { assert(address && allocations); allocations--; allocated -= bytes; }
static char log_text[65536]; static unsigned log_used;
void vga_print(const char* text) { size_t n = strlen(text); if (log_used + n < sizeof(log_text)) { memcpy(log_text + log_used, text, n); log_used += (unsigned)n; log_text[log_used] = 0; } fputs(text, stdout); }
void usb_log_device(const struct usb_device* device, const char* what) { (void)device; vga_print("[uvc] "); vga_print(what); vga_print("\n"); }
void usb_worker_kick(void) {}
bool usb_in_worker(void) { return true; }
uint64_t native_clock_ns(void) { static uint64_t t; return t += 1000; }
int task_wait_until(void* channel, uint64_t deadline) { (void)channel; (void)deadline; return -110; }
void task_wake_channel(void* channel) { (void)channel; }

/* ---------------- mock V4L2 core ---------------- */
struct v4l2_frame { int index; };
static struct { struct v4l2_cam* cam; bool registered; unsigned registrations; uint8_t buffers[4][4 << 20]; int queued[4], queue_count;
                struct v4l2_frame frame; bool open; uint32_t done_bytes[64]; bool done_error[64]; int done_index[64]; unsigned done; unsigned drops; } core;
int v4l2_register(struct v4l2_cam* cam) { assert(!core.registered); core.cam = cam; core.registered = true; core.registrations++; return 0; }
void v4l2_unregister(struct v4l2_cam* cam) { assert(core.registered && core.cam == cam); if (cam->ops->stop) cam->ops->stop(cam); core.registered = false; }
static void queue_all(void) { core.queue_count = 4; for (int i = 0; i < 4; i++) core.queued[i] = i; }
struct v4l2_frame* v4l2_frame_begin(struct v4l2_cam* cam, uint32_t* capacity) {
    assert(cam == core.cam); if (!core.queue_count) return NULL;
    if (!core.open) { core.frame.index = core.queued[0]; core.open = true; }
    *capacity = 4 << 20; return &core.frame;
}
void v4l2_frame_write(struct v4l2_frame* frame, uint32_t offset, const void* data, uint32_t length) { assert(offset + length <= (4u << 20)); memcpy(core.buffers[frame->index] + offset, data, length); }
uint8_t* v4l2_frame_page(struct v4l2_frame* frame, uint32_t page) { return core.buffers[frame->index] + page * 4096; }
void v4l2_frame_end(struct v4l2_cam* cam, struct v4l2_frame* frame, uint32_t bytes, bool error) {
    assert(cam == core.cam && core.open && frame == &core.frame && core.done < 64);
    core.done_bytes[core.done] = bytes; core.done_error[core.done] = error; core.done_index[core.done] = frame->index; core.done++;
    for (int i = 1; i < core.queue_count; i++) core.queued[i - 1] = core.queued[i];
    core.queue_count--; core.open = false;
}
void v4l2_frame_drop(struct v4l2_cam* cam) { assert(cam == core.cam); core.drops++; }

/* ---------------- simulated camera ---------------- */
static struct {
    uint8_t config[2048]; unsigned length; unsigned probe_length;
    uint8_t probe[48], commit[48]; bool committed; unsigned alt[4]; unsigned set_interfaces;
    int16_t pu_min[16], pu_max[16], pu_res[16], pu_def[16], pu_cur[16];
    uint32_t max_payload; bool refuse_format, change_frame; unsigned requests;
} cam;
static struct usb_device dev;
static void sput16(uint8_t* p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void sput32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
const uint8_t* usb_config(const struct usb_device* device, unsigned* length) { (void)device; *length = cam.length; return cam.config; }
/* frame size table of the current camera, for PROBE answers */
static uint16_t frame_w[3][16], frame_h[3][16]; static bool format_compressed[3]; static bool format_nv12[3];
int usb_control(struct usb_device* device, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length) {
    assert(device == &dev); cam.requests++;
    uint8_t* d = data;
    if (device->gone) return -19;
    if (type == 0x01 && request == 11) { cam.set_interfaces++; if ((index & 0xFF) < 4) cam.alt[index & 0xFF] = value; return 0; }
    if ((type == 0x21 || type == 0xA1) && (index & 0xFF) == 1) {                     /* VideoStreaming: probe/commit */
        unsigned selector = value >> 8;
        if (length != cam.probe_length) return -32;
        if (type == 0x21 && request == 0x01 && selector == 1) {
            memcpy(cam.probe, d, length);
            unsigned f = cam.probe[2], fr = cam.probe[3];
            if (cam.refuse_format) cam.probe[2] = 9;
            if (cam.change_frame && fr > 1) cam.probe[3] = (uint8_t)(fr - 1);
            if (f >= 1 && f <= 2 && fr >= 1 && fr <= 15) {
                uint32_t size = format_compressed[f] ? frame_w[f][fr] * frame_h[f][fr] / 4 : format_nv12[f] ? frame_w[f][fr] * frame_h[f][fr] * 3 / 2 : frame_w[f][fr] * frame_h[f][fr] * 2;
                sput32(cam.probe + 18, size); sput32(cam.probe + 22, cam.max_payload);
            }
            return 0;
        }
        if (type == 0xA1 && request == 0x81 && selector == 1) { memcpy(d, cam.probe, length); return length; }
        if (type == 0x21 && request == 0x01 && selector == 2) { memcpy(cam.commit, d, length); cam.committed = true; return 0; }
        return -32;
    }
    if ((type == 0x21 || type == 0xA1) && (index & 0xFF) == 0 && (index >> 8) == 3) { /* processing unit 3 */
        unsigned selector = value >> 8; if (selector >= 16) return -32;
        if (type == 0x21 && request == 0x01) { cam.pu_cur[selector] = (int16_t)(length == 1 ? d[0] : rd16(d)); return 0; }
        int16_t v = request == 0x81 ? cam.pu_cur[selector] : request == 0x82 ? cam.pu_min[selector] : request == 0x83 ? cam.pu_max[selector] : request == 0x84 ? cam.pu_res[selector] : request == 0x87 ? cam.pu_def[selector] : 0;
        if (request < 0x81 || request > 0x87 || !cam.pu_max[selector]) return -32;
        if (length == 1) { d[0] = (uint8_t)v; return 1; }
        sput16(d, (uint16_t)v); return 2;
    }
    return -32;
}

/* ---------------- simulated pipes ---------------- */
struct td { uint64_t physical; uint32_t length, tag; };
static struct { struct usb_pipe* pipe; struct td tds[512]; unsigned head, tail; bool open; } pipes[2];
static int find_pipe(struct usb_pipe* p) { for (int i = 0; i < 2; i++) if (pipes[i].open && pipes[i].pipe == p) return i; return -1; }
bool usb_pipe_open(struct usb_device* device, struct usb_pipe* pipe) {
    pipe->device = device; pipe->in = true; pipe->dci = (uint8_t)((pipe->address & 15) * 2 + 1);
    for (int i = 0; i < 2; i++) if (!pipes[i].open) { pipes[i].pipe = pipe; pipes[i].open = true; pipes[i].head = pipes[i].tail = 0; pipe->ring = 1; return true; }
    return false;
}
void usb_pipe_close(struct usb_pipe* pipe) { int i = find_pipe(pipe); assert(i >= 0); pipes[i].open = false; pipe->device = NULL; pipe->configured = false; }
bool usb_pipes_configure(struct usb_device* device, struct usb_pipe* const* add, unsigned adds, struct usb_pipe* const* drop, unsigned drops) {
    (void)device;
    for (unsigned i = 0; i < drops; i++) { int k = find_pipe(drop[i]); if (k >= 0) { while (pipes[k].head != pipes[k].tail) { struct td t = pipes[k].tds[pipes[k].head++ % 512]; drop[i]->queued--; drop[i]->complete(drop[i], t.tag, 0, 26); } } drop[i]->configured = false; }
    for (unsigned i = 0; i < adds; i++) { assert(find_pipe(add[i]) >= 0); add[i]->configured = true; add[i]->queued = 0; }
    return true;
}
int usb_pipe_queue(struct usb_pipe* pipe, uint64_t physical, uint32_t length, uint32_t tag) {
    int k = find_pipe(pipe); assert(k >= 0);
    if (dev.gone) return -19;
    if (!pipe->configured) return -32;
    if (pipes[k].tail - pipes[k].head >= 250) return -16;
    pipes[k].tds[pipes[k].tail++ % 512] = (struct td){physical, length, tag}; pipe->queued++;
    return 0;
}
bool usb_pipe_stop(struct usb_pipe* pipe) {
    int k = find_pipe(pipe); assert(k >= 0);
    while (pipes[k].head != pipes[k].tail) { struct td t = pipes[k].tds[pipes[k].head++ % 512]; pipe->queued--; pipe->complete(pipe, t.tag, 0, 26); }
    return true;
}
/* The device fills the oldest TD with `data` (code 1), or reports `code`. */
static bool deliver(const uint8_t* data, unsigned length, unsigned code) {
    int k = pipes[0].open ? 0 : -1; if (k < 0 || pipes[k].head == pipes[k].tail) return false;
    struct usb_pipe* pipe = pipes[k].pipe; struct td t = pipes[k].tds[pipes[k].head++ % 512]; pipe->queued--;
    assert(length <= t.length);
    if (data) memcpy((void*)(uintptr_t)t.physical, data, length);
    pipe->complete(pipe, t.tag, code == 1 || code == 13 ? length : 0, code);
    return true;
}

#include "../arch/x86_64/uvc.c"
/* what the simulated camera sends per transfer: the negotiated payload, at most the chosen alternate setting's bandwidth */
static unsigned uvc_instance_payload(void) {
    struct uvc* u = instances[0]; unsigned limit = u->max_payload;
    if (!u->bulk) for (unsigned i = 0; i < u->alt_count; i++) if (u->alts[i].alt == u->current_alt && u->alts[i].bandwidth < limit) limit = u->alts[i].bandwidth;
    if (limit > u->buffer_bytes) limit = u->buffer_bytes;
    return limit > 4096 ? 4096 : limit;
}
static uint64_t uvc_instance_header_errors(void) { return instances[0]->header_errors; }
static unsigned uvc_instance_buffer(void) { return instances[0] ? instances[0]->buffer_bytes : 0; }

/* ---------------- descriptor builders ---------------- */
static void put(const uint8_t* bytes, unsigned n) { memcpy(cam.config + cam.length, bytes, n); cam.length += n; }
static void interface(unsigned number, unsigned alt, unsigned endpoints, unsigned subclass) { uint8_t d[9] = {9, 4, (uint8_t)number, (uint8_t)alt, (uint8_t)endpoints, 0x0E, (uint8_t)subclass, 0, 0}; put(d, 9); }
static void endpoint(unsigned address, unsigned attributes, unsigned packet, unsigned interval) { uint8_t d[7] = {7, 5, (uint8_t)address, (uint8_t)attributes, (uint8_t)packet, (uint8_t)(packet >> 8), (uint8_t)interval}; put(d, 7); }
static const uint8_t YUY2[16] = {'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
static const uint8_t NV12[16] = {'N', 'V', '1', '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
static void frame(unsigned subtype, unsigned format, unsigned index, unsigned w, unsigned h, const uint32_t* intervals, unsigned count) {
    uint8_t d[64]; memset(d, 0, sizeof(d)); unsigned n = 26 + 4 * count;
    d[0] = (uint8_t)n; d[1] = 0x24; d[2] = (uint8_t)subtype; d[3] = (uint8_t)index; put16(d + 5, w); put16(d + 7, h);
    put32(d + 9, w * h * 16 * 5); put32(d + 13, w * h * 16 * 30); put32(d + 17, w * h * 2); put32(d + 21, intervals[0]); d[25] = (uint8_t)count;
    for (unsigned i = 0; i < count; i++) put32(d + 26 + 4 * i, intervals[i]);
    put(d, n); frame_w[format][index] = (uint16_t)w; frame_h[format][index] = (uint16_t)h;
}
static void reset(void) {
    memset(&cam, 0, sizeof(cam)); memset(&dev, 0, sizeof(dev)); memset(&core, 0, sizeof(core)); memset(pipes, 0, sizeof(pipes));
    memset(frame_w, 0, sizeof(frame_w)); memset(frame_h, 0, sizeof(frame_h));
    dev.slot = 1; dev.speed = USB_SPEED_HIGH; dev.root_port = 3; strcpy(dev.product_name, "C270 HD WEBCAM"); dev.vendor = 0x046D; dev.product = 0x0825;
    cam.probe_length = 26; cam.max_payload = 3060;
    for (unsigned s = 1; s <= 12; s++) { cam.pu_min[s] = 0; cam.pu_max[s] = 255; cam.pu_res[s] = 1; cam.pu_def[s] = 128; cam.pu_cur[s] = 128; }
    cam.pu_max[5] = 2; cam.pu_def[5] = 2; cam.pu_cur[5] = 2; cam.pu_max[11] = 1; cam.pu_def[11] = 1; cam.pu_cur[11] = 1;
    cam.pu_min[10] = 2800; cam.pu_max[10] = 6500; cam.pu_res[10] = 10; cam.pu_def[10] = 4000; cam.pu_cur[10] = 4000;
}
static void build_c270(void) {
    reset();
    uint8_t c[9] = {9, 2, 0, 0, 2, 1, 0, 0x80, 250}; put(c, 9);
    uint8_t iad[8] = {8, 11, 0, 2, 0x0E, 3, 0, 0}; put(iad, 8);
    interface(0, 0, 1, 1);
    unsigned header = cam.length; uint8_t h[13] = {13, 0x24, 1, 0x00, 0x01, 0, 0, 0x80, 0x8D, 0x5B, 0x00, 1, 1}; put(h, 13);
    uint8_t it[18] = {18, 0x24, 2, 1, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0x0E, 0x00, 0x00}; put(it, 18);
    uint8_t pu[11] = {11, 0x24, 5, 3, 1, 0, 0, 2, 0x5B, 0x17, 0}; put(pu, 11);
    uint8_t ot[9] = {9, 0x24, 3, 4, 0x01, 0x01, 0, 3, 0}; put(ot, 9);
    put16(cam.config + header + 5, cam.length - header);
    endpoint(0x87, 3, 16, 8);
    interface(1, 0, 0, 2);
    unsigned vs = cam.length; uint8_t ih[15] = {15, 0x24, 1, 2, 0, 0, 0x81, 0, 4, 0, 0, 0, 1, 0, 4}; put(ih, 15);
    uint8_t yuv[27] = {27, 0x24, 4, 1, 3}; memcpy(yuv + 5, YUY2, 16); yuv[21] = 16; yuv[22] = 1; put(yuv, 27);
    static const uint32_t iv[7] = {333333, 400000, 500000, 666666, 1000000, 1333333, 2000000};
    frame(5, 1, 1, 640, 480, iv, 7); frame(5, 1, 2, 160, 120, iv, 7); frame(5, 1, 3, 1280, 720, iv + 6, 1);
    uint8_t mj[11] = {11, 0x24, 6, 2, 2, 1, 1, 0, 0, 0, 0}; put(mj, 11);
    frame(7, 2, 1, 640, 480, iv, 7); frame(7, 2, 2, 1280, 720, iv, 2);
    format_compressed[2] = true;
    put16(cam.config + vs + 4, cam.length - vs);
    static const uint16_t packets[11] = {0x00C0, 0x0180, 0x0200, 0x0280, 0x0320, 0x03B0, 0x0A80, 0x0B20, 0x0BE0, 0x1380, 0x13FC};
    for (unsigned a = 0; a < 11; a++) { interface(1, a + 1, 1, 2); endpoint(0x81, 5, packets[a], 1); }
    put16(cam.config + 2, cam.length);
}
static void build_bulk(void) {
    reset(); strcpy(dev.product_name, "USB3 Bulk Camera"); cam.probe_length = 34; cam.max_payload = 16384;
    uint8_t c[9] = {9, 2, 0, 0, 2, 1, 0, 0x80, 250}; put(c, 9);
    interface(0, 0, 0, 1);
    unsigned header = cam.length; uint8_t h[13] = {13, 0x24, 1, 0x10, 0x01, 0, 0, 0x80, 0x8D, 0x5B, 0x00, 1, 1}; put(h, 13);
    uint8_t it[18] = {18, 0x24, 2, 1, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0}; put(it, 18);
    uint8_t pu[11] = {11, 0x24, 5, 3, 1, 0, 0, 2, 0x01, 0x00, 0}; put(pu, 11);
    put16(cam.config + header + 5, cam.length - header);
    interface(1, 0, 1, 2);
    unsigned vs = cam.length; uint8_t ih[14] = {14, 0x24, 1, 1, 0, 0, 0x82, 0, 4, 0, 0, 0, 1, 0}; put(ih, 14);
    uint8_t nv[27] = {27, 0x24, 4, 1, 1}; memcpy(nv + 5, NV12, 16); nv[21] = 12; nv[22] = 1; put(nv, 27);
    static const uint32_t iv[1] = {333333}; frame(5, 1, 1, 320, 240, iv, 1); format_nv12[1] = true;
    put16(cam.config + vs + 4, cam.length - vs);
    endpoint(0x82, 2, 512, 0);
    put16(cam.config + 2, cam.length);
}

/* ---------------- streaming helpers ---------------- */
static unsigned payload_size(void) { return uvc_instance_payload(); }
/* Send one frame of `bytes` bytes: payloads with 12-byte headers, FID `fid`, EOF on the last (unless no_eof). */
static void send_frame(uint32_t bytes, int fid, bool error, bool no_eof, uint8_t seed) {
    unsigned chunk = payload_size() - 12; uint8_t packet[4096];
    for (uint32_t offset = 0; offset < bytes; offset += chunk) {
        unsigned n = bytes - offset < chunk ? bytes - offset : chunk; bool last = offset + n >= bytes;
        packet[0] = 12; packet[1] = (uint8_t)(0x80 | 0x04 | 0x08 | fid | (last && !no_eof ? 0x02 : 0) | (error && last ? 0x40 : 0));
        memset(packet + 2, 0x11, 10);
        for (unsigned i = 0; i < n; i++) packet[12 + i] = (uint8_t)(seed + offset + i);
        assert(deliver(packet, 12 + n, 1));
    }
}
static bool frame_content_ok(unsigned done, uint32_t bytes, uint8_t seed) {
    const uint8_t* b = core.buffers[core.done_index[done]];
    for (uint32_t i = 0; i < bytes; i++) if (b[i] != (uint8_t)(seed + i)) return false;
    return true;
}
static struct v4l2_cam_format* format_of(uint32_t fourcc) { for (unsigned i = 0; i < core.cam->format_count; i++) if (core.cam->formats[i].fourcc == fourcc) return &core.cam->formats[i]; return NULL; }
static int start(uint32_t fourcc, unsigned size_index, uint32_t interval) {
    struct v4l2_cam_format* f = format_of(fourcc); assert(f);
    return core.cam->ops->start(core.cam, f, &f->sizes[size_index], interval);
}

static void scenario_c270(void) {
    build_c270();
    uvc_attach(&dev);
    assert(core.registered && !strcmp(core.cam->card, "C270 HD WEBCAM") && !strcmp(core.cam->driver, "uvcvideo"));
    assert(core.cam->format_count == 2 && core.cam->formats[0].fourcc == V4L2_FMT_YUYV && core.cam->formats[1].fourcc == V4L2_FMT_MJPEG);
    assert(core.cam->formats[0].size_count == 3 && core.cam->formats[0].sizes[0].interval_count == 7 && core.cam->formats[0].sizes[0].intervals[0] == 333333);
    assert(core.cam->formats[1].compressed && core.cam->formats[1].sizes[1].width == 1280);
    /* bmControls 0x175B: brightness, contrast, saturation, sharpness, WB temperature, backlight, gain, power line, WB auto */
    assert(core.cam->control_count == 9);
    bool plf = false; for (unsigned i = 0; i < core.cam->control_count; i++) { struct v4l2_cam_control* c = &core.cam->controls[i];
        if (c->id == 0x00980918) { plf = c->type == V4L2_CAM_CONTROL_MENU && c->def == 2 && c->value == 2; }
        if (c->id == 0x0098091A) assert(c->minimum == 2800 && c->maximum == 6500 && c->step == 10 && c->def == 4000); }
    assert(plf && strstr(log_text, "camera: UVC, formats 2, isochronous, controls 09"));
    /* YUYV 640x480 at 30 fps: payload 3060 -> the smallest alternate setting carrying it: 0x0BE0 (3 x 992 = 2976)? no: 0x13FC (3 x 1020 = 3060) */
    queue_all(); assert(start(V4L2_FMT_YUYV, 0, 333333) == 0);
    assert(cam.committed && cam.commit[2] == 1 && cam.commit[3] == 1 && rd32(cam.commit + 4) == 333333 && cam.alt[1] == 11);
    assert(strstr(log_text, "camera streaming: format 1 frame 1 isochronous alt"));
    uint32_t size = 640 * 480 * 2;
    send_frame(size, 0, false, false, 7); assert(core.done == 1 && core.done_bytes[0] == size && !core.done_error[0] && frame_content_ok(0, size, 7));
    send_frame(size, 1, true, false, 9); assert(core.done == 2 && core.done_error[1]);                     /* ERR bit */
    send_frame(size - 100, 0, false, false, 3); assert(core.done == 3 && core.done_error[2]);              /* short uncompressed frame */
    queue_all();
    send_frame(size / 2, 1, false, true, 4); send_frame(size, 0, false, false, 5);                          /* FID toggle without EOF */
    assert(core.done == 5 && core.done_error[3] && !core.done_error[4] && frame_content_ok(4, size, 5));
    send_frame(size, 1, false, false, 6); send_frame(size, 0, false, false, 7); assert(core.done == 7 && core.queue_count == 0);
    send_frame(size, 1, false, false, 6); assert(core.done == 7 && core.drops == 1);                        /* no buffer queued: dropped */
    queue_all();
    uint8_t garbage[64] = {200, 0x81}; assert(deliver(garbage, 64, 1)); assert(uvc_instance_header_errors() == 1);
    assert(deliver(NULL, 0, 4));                                                                              /* a lost transfer */
    send_frame(size, 0, false, false, 8);
    unsigned before = core.done; send_frame(size, 1, false, false, 9); assert(core.done == before + 1 && !core.done_error[before]);
    /* controls: brightness 200 -> SET_CUR on PU 3 selector 2 */
    struct v4l2_cam_control* bright = NULL; for (unsigned i = 0; i < core.cam->control_count; i++) if (core.cam->controls[i].id == 0x00980900) bright = &core.cam->controls[i];
    assert(bright && core.cam->ops->set_control(core.cam, bright, 200) == 0); uvc_service(); assert(cam.pu_cur[2] == 200);
    /* STREAMOFF then MJPEG 1280x720 at 2 intervals */
    core.cam->ops->stop(core.cam); uvc_service(); assert(cam.alt[1] == 0 && !pipes[0].open);
    queue_all(); core.done = 0; cam.max_payload = 1024;
    assert(start(V4L2_FMT_MJPEG, 1, 333333) == 0 && cam.commit[2] == 2 && cam.commit[3] == 2 && cam.alt[1] == 7);   /* 0x0A80: 2 x 640 = 1280 >= 1024 */
    send_frame(54321, 0, false, false, 1); assert(core.done == 1 && core.done_bytes[0] == 54321 && !core.done_error[0]);   /* compressed: any size */
    /* refused / changed format: STREAMON fails cleanly, nothing left configured */
    core.cam->ops->stop(core.cam); uvc_service();
    cam.refuse_format = true; assert(start(V4L2_FMT_YUYV, 0, 333333) == -22 && !pipes[0].open); cam.refuse_format = false;
    cam.change_frame = true; assert(start(V4L2_FMT_YUYV, 1, 333333) == -22 && !pipes[0].open); cam.change_frame = false;
    /* unplug while streaming */
    queue_all(); assert(start(V4L2_FMT_YUYV, 0, 333333) == 0);
    send_frame(size / 3, 1, false, true, 2);
    dev.gone = true; uvc_detach(&dev);
    assert(!core.registered && !pipes[0].open && allocations == 0 && allocated == 0);
    printf("c270 (UVC 1.0 isochronous, YUYV + MJPEG, 9 controls): PASSED\n");
}
static void scenario_bulk(void) {
    build_bulk();
    uvc_attach(&dev);
    assert(core.registered && core.cam->format_count == 1 && core.cam->formats[0].fourcc == V4L2_FMT_NV12 && core.cam->control_count == 1);
    queue_all(); assert(start(V4L2_FMT_NV12, 0, 333333) == 0 && cam.set_interfaces == 0 && strstr(log_text, "bulk"));
    uint32_t size = 320 * 240 * 3 / 2;
    send_frame(size, 0, false, false, 3); assert(core.done == 1 && core.done_bytes[0] == size && !core.done_error[0] && frame_content_ok(0, size, 3));
    dev.gone = false; uvc_detach(&dev);                                    /* orderly: pipe dropped */
    assert(!core.registered && allocations == 0);
    printf("bulk camera (UVC 1.1, NV12): PASSED\n");
}
static void scenario_hostile(void) {
    srand(4321); unsigned attached = 0;
    for (unsigned round = 0; round < 4000; round++) {
        if (round % 2) build_bulk(); else build_c270();
        unsigned flips = 1 + (unsigned)rand() % 8;
        for (unsigned f = 0; f < flips; f++) cam.config[(unsigned)rand() % cam.length] = (uint8_t)rand();
        if (rand() % 6 == 0) cam.length = (unsigned)rand() % cam.length;
        uvc_attach(&dev);
        if (core.registered) {
            attached++; queue_all();
            for (unsigned k = 0; k < core.cam->format_count; k++) {
                const struct v4l2_cam_format* f = &core.cam->formats[k];
                assert(f->size_count && f->size_count <= V4L2_CAM_SIZES);
                for (unsigned s = 0; s < f->size_count; s++) assert(f->sizes[s].interval_count && f->sizes[s].width && f->sizes[s].height);
            }
            if (core.cam->ops->start(core.cam, &core.cam->formats[0], &core.cam->formats[0].sizes[0], core.cam->formats[0].sizes[0].intervals[0]) == 0) {
                uint8_t packet[4096];
                for (unsigned i = 0; i < 40; i++) { unsigned n = 2 + (unsigned)rand() % 3000; for (unsigned b = 0; b < n; b++) packet[b] = (uint8_t)rand(); if (rand() % 2) packet[0] = 12;
                    if (!deliver(packet, n > uvc_instance_buffer() ? uvc_instance_buffer() : n, rand() % 5 ? 1 : 4)) break; }
            }
            dev.gone = rand() % 2; uvc_detach(&dev);
        }
        assert(allocations == 0 && allocated == 0 && !pipes[0].open && !core.registered);
    }
    printf("hostile: 4000 fuzzed descriptor sets + garbage payloads (%u attached): PASSED\n", attached);
}
int main(void) {
    scenario_c270(); scenario_bulk(); scenario_hostile();
    printf("UVC SIMULATOR TESTS PASSED\n");
    return 0;
}
