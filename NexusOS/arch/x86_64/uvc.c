/* USB Video Class 1.0/1.1/1.5 webcams as V4L2 capture devices (/dev/videoN through v4l2.c).
 *
 * Attach (USB worker): the VideoControl interface gives the camera input terminal and the processing unit, whose
 * controls (brightness, contrast, hue, saturation, sharpness, gamma, gain, white balance temperature + auto, backlight
 * compensation, power line frequency) become V4L2 controls with their GET_MIN/MAX/RES/DEF/CUR range. The first
 * VideoStreaming interface gives the formats the V4L2 core offers: uncompressed YUY2 / NV12 / I420 (by format GUID)
 * and MJPEG, each with its frame sizes and discrete intervals (a continuous range is offered as its minimum, default
 * and maximum), and the streaming endpoint: isochronous IN in alternate settings 1..n, or bulk IN in setting 0.
 * Streaming (STREAMON waits for the worker, since control transfers wait): PROBE SET_CUR/GET_CUR with the format,
 * frame and interval, COMMIT; the smallest isochronous alternate setting whose bandwidth (wMaxPacketSize x
 * transactions) carries the negotiated dwMaxPayloadTransferSize, else the largest; its pipe, SET_INTERFACE, then
 * UVC_DEPTH transfers kept queued. Payloads (bHeaderLength, bmHeaderInfo FID/EOF/ERR) are reassembled into the oldest
 * queued V4L2 buffer: a toggled FID or EOF ends a frame; ERR, an overflow, a lost transfer or (uncompressed) a size
 * other than the frame size marks it bad; a frame without a buffer is dropped (its sequence number skipped).
 * STREAMOFF, close and unplug only ask the worker to stop the pipe and select alternate setting 0. */
#include "usb.h"
#include "v4l2.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
/* Device state lives as long as the device, not the program that was running when it was plugged in: counted
 * allocations (storage_alloc.c), which the kernel's per-program allocation audit (kernel.c run_image) leaves out. */
#ifndef NEXUS_HOST_TEST
#include "storage_util.h"
#endif

#define UVC_DEPTH 96U
#define UVC_MAX_PACKET 3072U
#define UVC_MAX_ALTS 16U
#define UVC_MAX_DEVICES 4U
#define UVC_BULK_BYTES 65536U
enum { PU_BACKLIGHT = 1, PU_BRIGHTNESS, PU_CONTRAST, PU_GAIN, PU_POWER_LINE, PU_HUE, PU_SATURATION, PU_SHARPNESS, PU_GAMMA,
       PU_WB_TEMPERATURE, PU_WB_TEMPERATURE_AUTO };
enum { REQ_SET_CUR = 0x01, REQ_GET_CUR = 0x81, REQ_GET_MIN = 0x82, REQ_GET_MAX = 0x83, REQ_GET_RES = 0x84, REQ_GET_DEF = 0x87 };
enum { WORK_START = 1, WORK_STOP = 2, WORK_CONTROLS = 4 };

struct uvc_alt { uint8_t alt, endpoint; uint16_t max_packet; uint8_t interval; uint32_t bandwidth; };
struct uvc {
    struct usb_device* device; struct v4l2_cam cam; bool gone, registered, bulk;
    uint8_t control_interface, streaming_interface, processing_unit; bool have_control, have_streaming; uint16_t uvc_version; unsigned probe_length;
    struct uvc_alt alts[UVC_MAX_ALTS]; unsigned alt_count; uint8_t bulk_endpoint; uint16_t bulk_packet;
    uint8_t format_index[V4L2_CAM_FORMATS];               /* bFormatIndex per v4l2 format */
    volatile uint32_t work; int start_result; bool start_done;
    uint8_t want_format, want_frame; uint32_t want_interval; uint32_t frame_size; bool compressed;
    bool streaming; struct usb_pipe pipe; uint64_t buffers; unsigned buffer_bytes, slots, next_slot, inflight; uint16_t generation;
    uint8_t current_alt; uint32_t max_payload;
    struct v4l2_frame* frame; uint32_t frame_bytes, frame_capacity; int fid; bool frame_error, skipping;
    uint64_t payloads, frames_done, frames_bad, header_errors;
    volatile uint32_t dirty_controls;
    unsigned waiters; bool detached;                     /* STREAMON callers asleep on it: freed by the last one */
};
static struct uvc* instances[UVC_MAX_DEVICES];

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
bool uvc_service(void);

/* ---------- descriptors ---------- */
bool uvc_wanted(const struct usb_device* device) {
    unsigned length; const uint8_t* data = usb_config(device, &length);
    bool control = false, streaming = false;
    for (unsigned at = 0; data && at + 9 <= length && data[at] >= 2 && at + data[at] <= length; at += data[at])
        if (data[at + 1] == 4 && data[at + 5] == 0x0E) { control |= data[at + 6] == 1; streaming |= data[at + 6] == 2; }
    return control && streaming;
}
struct pu_control { uint8_t selector; uint32_t cid; uint8_t bit, size; bool is_signed; const char* name; uint8_t type; };
static const struct pu_control PU_CONTROLS[] = {
    {PU_BRIGHTNESS, 0x00980900, 0, 2, true, "Brightness", V4L2_CAM_CONTROL_INTEGER},
    {PU_CONTRAST, 0x00980901, 1, 2, false, "Contrast", V4L2_CAM_CONTROL_INTEGER},
    {PU_HUE, 0x00980903, 2, 2, true, "Hue", V4L2_CAM_CONTROL_INTEGER},
    {PU_SATURATION, 0x00980902, 3, 2, false, "Saturation", V4L2_CAM_CONTROL_INTEGER},
    {PU_SHARPNESS, 0x0098091B, 4, 2, false, "Sharpness", V4L2_CAM_CONTROL_INTEGER},
    {PU_GAMMA, 0x00980910, 5, 2, false, "Gamma", V4L2_CAM_CONTROL_INTEGER},
    {PU_WB_TEMPERATURE, 0x0098091A, 6, 2, false, "White Balance Temperature", V4L2_CAM_CONTROL_INTEGER},
    {PU_BACKLIGHT, 0x0098091C, 8, 2, false, "Backlight Compensation", V4L2_CAM_CONTROL_INTEGER},
    {PU_GAIN, 0x00980913, 9, 2, false, "Gain", V4L2_CAM_CONTROL_INTEGER},
    {PU_POWER_LINE, 0x00980918, 10, 1, false, "Power Line Frequency", V4L2_CAM_CONTROL_MENU},
    {PU_WB_TEMPERATURE_AUTO, 0x0098090C, 12, 1, false, "White Balance, Automatic", V4L2_CAM_CONTROL_BOOLEAN},
};
static int request(struct uvc* u, bool get, unsigned code, unsigned selector, unsigned entity, unsigned interface, void* data, unsigned length) {
    return usb_control(u->device, get ? 0xA1 : 0x21, (uint8_t)code, (uint16_t)(selector << 8), (uint16_t)(entity << 8 | interface), data, (uint16_t)length);
}
static bool read_value(struct uvc* u, unsigned code, unsigned selector, unsigned size, bool is_signed, int32_t* out) {
    uint8_t b[4] = {0, 0, 0, 0};
    if (request(u, true, code, selector, u->processing_unit, u->control_interface, b, size) != (int)size) return false;
    *out = size == 1 ? b[0] : is_signed ? (int16_t)le16(b) : le16(b);
    return true;
}
static void controls_setup(struct uvc* u, uint32_t bm_controls) {
    for (unsigned i = 0; i < sizeof(PU_CONTROLS) / sizeof(PU_CONTROLS[0]) && u->cam.control_count < V4L2_CAM_CONTROLS; i++) {
        const struct pu_control* d = &PU_CONTROLS[i];
        if (!(bm_controls >> d->bit & 1)) continue;
        struct v4l2_cam_control* c = &u->cam.controls[u->cam.control_count]; memset(c, 0, sizeof(*c));
        c->id = d->cid; c->type = d->type; strcpy(c->name, d->name); c->driver_id = d->selector | (uint32_t)d->size << 8 | (uint32_t)d->is_signed << 16;
        int32_t cur = 0;
        if (d->type == V4L2_CAM_CONTROL_MENU) {                          /* 0 disabled, 1 50 Hz, 2 60 Hz */
            c->minimum = 0; c->maximum = 2; c->step = 1; strcpy(c->menu[0], "Disabled"); strcpy(c->menu[1], "50 Hz"); strcpy(c->menu[2], "60 Hz");
            if (!read_value(u, REQ_GET_DEF, d->selector, 1, false, &c->def) || c->def > 2) c->def = 1;
        } else if (d->type == V4L2_CAM_CONTROL_BOOLEAN) {
            c->minimum = 0; c->maximum = 1; c->step = 1;
            if (!read_value(u, REQ_GET_DEF, d->selector, 1, false, &c->def) || c->def > 1) c->def = 1;
        } else {
            int32_t res = 1;
            if (!read_value(u, REQ_GET_MIN, d->selector, d->size, d->is_signed, &c->minimum) || !read_value(u, REQ_GET_MAX, d->selector, d->size, d->is_signed, &c->maximum) ||
                c->maximum < c->minimum) continue;                        /* a control without a usable range is not offered */
            if (!read_value(u, REQ_GET_RES, d->selector, d->size, false, &res) || res <= 0) res = 1;
            c->step = res;
            if (!read_value(u, REQ_GET_DEF, d->selector, d->size, d->is_signed, &c->def) || c->def < c->minimum || c->def > c->maximum) c->def = c->minimum;
        }
        c->value = read_value(u, REQ_GET_CUR, d->selector, d->size, d->is_signed, &cur) && cur >= c->minimum && cur <= c->maximum ? cur : c->def;
        u->cam.control_count++;
    }
}
static uint32_t guid_fourcc(const uint8_t* guid) {
    static const uint8_t tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    if (memcmp(guid + 4, tail, 12)) return 0;
    uint32_t code = le32(guid);
    if (code == V4L2_FOURCC('Y', 'U', 'Y', '2')) return V4L2_FMT_YUYV;
    if (code == V4L2_FMT_NV12) return V4L2_FMT_NV12;
    if (code == V4L2_FOURCC('I', '4', '2', '0')) return V4L2_FMT_YU12;
    return 0;
}
/* VideoControl + the first VideoStreaming interface (formats, frames, alternate settings). Malformed descriptors end
   the walk; what was parsed before stays usable. */
static void parse(struct uvc* u, const uint8_t* data, unsigned length) {
    unsigned interface = 0xFF, class_kind = 0, alt = 0; struct v4l2_cam_format* format = NULL; uint32_t pu_controls = 0;
    for (unsigned at = 0; at + 2 <= length;) {
        unsigned size = data[at], type = data[at + 1];
        if (size < 2 || at + size > length) break;
        const uint8_t* d = data + at;
        if (type == 4 && size >= 9) {
            interface = d[2]; alt = d[3]; class_kind = d[5] == 0x0E ? d[6] : 0; format = NULL;
            if (class_kind == 1 && !u->have_control) { u->control_interface = (uint8_t)interface; u->have_control = true; }
            if (class_kind == 2 && !u->have_streaming) { u->streaming_interface = (uint8_t)interface; u->have_streaming = true; }
            if (class_kind == 2 && interface != u->streaming_interface) class_kind = 0;   /* one streaming interface per device */
        } else if (type == 0x24 && class_kind == 1 && size >= 3) {
            if (d[2] == 0x01 && size >= 12) u->uvc_version = le16(d + 3);
            else if (d[2] == 0x05 && size >= 8 && !u->processing_unit) {
                u->processing_unit = d[3]; unsigned n = d[7]; pu_controls = 0;
                for (unsigned b = 0; b < n && b < 4 && 8 + b < size; b++) pu_controls |= (uint32_t)d[8 + b] << (8 * b);
            }
        } else if (type == 0x24 && class_kind == 2 && size >= 3) {
            unsigned subtype = d[2];
            if ((subtype == 0x04 && size >= 27) || (subtype == 0x06 && size >= 11)) {
                uint32_t fourcc = subtype == 0x06 ? V4L2_FMT_MJPEG : guid_fourcc(d + 5);
                format = NULL;
                if (fourcc && u->cam.format_count < V4L2_CAM_FORMATS) {
                    unsigned k = u->cam.format_count; format = &u->cam.formats[k]; memset(format, 0, sizeof(*format));
                    format->fourcc = fourcc; format->compressed = fourcc == V4L2_FMT_MJPEG; format->driver_index = d[3];
                    strcpy(format->description, fourcc == V4L2_FMT_MJPEG ? "Motion-JPEG" : fourcc == V4L2_FMT_YUYV ? "YUYV 4:2:2" : fourcc == V4L2_FMT_NV12 ? "Y/UV 4:2:0" : "Planar YUV 4:2:0");
                    u->format_index[k] = d[3]; u->cam.format_count++;
                }
            } else if ((subtype == 0x05 || subtype == 0x07) && size >= 26 && format && format->size_count < V4L2_CAM_SIZES) {
                uint16_t width = le16(d + 5), height = le16(d + 7); unsigned kind = d[25];
                if (width && height && width <= 4096 && height <= 4096) {
                    struct v4l2_cam_size* s = &format->sizes[format->size_count]; memset(s, 0, sizeof(*s));
                    s->width = width; s->height = height; s->driver_index = d[3];
                    if (kind == 0 && size >= 38) {                           /* continuous: min, default, max */
                        uint32_t min = le32(d + 26), max = le32(d + 30), def = le32(d + 21);
                        if (min) s->intervals[s->interval_count++] = min;
                        if (def > min && def < max) s->intervals[s->interval_count++] = def;
                        if (max > min) s->intervals[s->interval_count++] = max;
                    } else for (unsigned i = 0; i < kind && 26 + 4 * i + 4 <= size && s->interval_count < V4L2_CAM_INTERVALS; i++) {
                        uint32_t interval = le32(d + 26 + 4 * i);
                        if (interval >= 10000 && interval <= 100000000) s->intervals[s->interval_count++] = interval;   /* 1 ms .. 10 s */
                    }
                    if (s->interval_count) format->size_count++;
                }
            }
        } else if (type == 5 && size >= 7 && class_kind == 2 && (d[2] & 0x80)) {
            uint16_t mps = le16(d + 4); unsigned transfer = d[3] & 3;
            if (transfer == 1 && alt && u->alt_count < UVC_MAX_ALTS && (mps & 0x7FF)) {
                struct uvc_alt* a = &u->alts[u->alt_count++];
                a->alt = (uint8_t)alt; a->endpoint = d[2]; a->max_packet = mps; a->interval = d[6];
                a->bandwidth = (mps & 0x7FF) * (1 + ((mps >> 11) & 3));
            } else if (transfer == 2 && !alt && !u->bulk_endpoint && (mps & 0x7FF)) { u->bulk_endpoint = d[2]; u->bulk_packet = mps & 0x7FF; }
        }
        at += size;
    }
    for (unsigned k = 0; k < u->cam.format_count; k++) if (!u->cam.formats[k].size_count) {   /* formats without frames go */
        for (unsigned j = k + 1; j < u->cam.format_count; j++) { u->cam.formats[j - 1] = u->cam.formats[j]; u->format_index[j - 1] = u->format_index[j]; }
        u->cam.format_count--; k--;
    }
    u->bulk = !u->alt_count && u->bulk_endpoint;
    u->probe_length = u->uvc_version >= 0x0150 ? 48 : u->uvc_version >= 0x0110 ? 34 : 26;
    if (u->processing_unit && u->have_control) controls_setup(u, pu_controls);
}

/* ---------- payloads (event processing) ---------- */
static void frame_finish(struct uvc* u) {
    if (!u->frame) return;
    bool bad = u->frame_error || (!u->compressed && u->frame_bytes != u->frame_size) || !u->frame_bytes;
    v4l2_frame_end(&u->cam, u->frame, u->frame_bytes, bad);
    if (bad) u->frames_bad++; else u->frames_done++;
    u->frame = NULL; u->frame_bytes = 0; u->frame_error = false;
}
static void payload(struct uvc* u, const uint8_t* p, unsigned length) {
    if (length < 2) return;                                             /* an empty transfer: nothing this interval */
    unsigned header = p[0], info = p[1];
    if (header < 2 || header > length || header > 12 || (!(info & 0x80) && header > 2)) { u->header_errors++; if (u->frame) u->frame_error = true; return; }
    u->payloads++;
    int fid = info & 1;
    if (u->fid >= 0 && fid != u->fid) {                                  /* a new frame began without EOF */
        if (u->frame) frame_finish(u);
        else if (u->skipping) v4l2_frame_drop(&u->cam);
        u->skipping = false;
    }
    u->fid = fid;
    unsigned data_length = length - header;
    if (!u->frame && !u->skipping && data_length) {
        u->frame = v4l2_frame_begin(&u->cam, &u->frame_capacity);
        if (!u->frame) u->skipping = true;                              /* no buffer queued: the whole frame is dropped */
        u->frame_bytes = 0; u->frame_error = false;
    }
    if (u->frame && data_length) {
        if (u->frame_bytes + data_length > u->frame_capacity) { u->frame_error = true; data_length = u->frame_capacity - u->frame_bytes; }
        v4l2_frame_write(u->frame, u->frame_bytes, p + header, data_length); u->frame_bytes += data_length;
    }
    if (info & 0x40) u->frame_error = true;                              /* ERR */
    if (info & 0x02) {                                                   /* EOF */
        if (u->frame) frame_finish(u); else if (u->skipping) v4l2_frame_drop(&u->cam);
        u->skipping = false; u->fid = -1;                                /* the next payload starts a frame whatever its FID */
    }
}
static uint32_t tag_of(const struct uvc* u, unsigned slot) { return (uint32_t)u->generation << 16 | slot; }
static bool queue_one(struct uvc* u) {
    unsigned slot = u->next_slot;
    uint64_t buffer = u->buffers + (uint64_t)slot * u->buffer_bytes;
    if (usb_pipe_queue(&u->pipe, virt_to_phys((void*)buffer), u->buffer_bytes, tag_of(u, slot))) return false;
    u->next_slot = (slot + 1) % u->slots; u->inflight++;
    return true;
}
static void complete(struct usb_pipe* pipe, uint32_t tag, unsigned actual, unsigned code) {
    struct uvc* u = pipe->owner;
    if (u->inflight) u->inflight--;
    if (!u->streaming || tag >> 16 != u->generation || code == 26) return;
    if (code == 1 || code == 13) {
        const uint8_t* data = (const uint8_t*)(u->buffers + (uint64_t)(tag & 0xFFFF) * u->buffer_bytes);
        payload(u, data, actual > u->buffer_bytes ? u->buffer_bytes : actual);
    } else if (u->frame) u->frame_error = true;                         /* a lost or broken transfer spoils the frame */
    while (u->streaming && u->inflight < (u->bulk ? 4U : UVC_DEPTH) && queue_one(u)) {}
}

/* ---------- streaming setup (USB worker) ---------- */
static void release_stream(struct uvc* u) {
    if (u->pipe.device) {
        if (!u->device->gone && u->pipe.configured) { struct usb_pipe* drop[1] = {&u->pipe}; usb_pipes_configure(u->device, NULL, 0, drop, 1); }
        usb_pipe_close(&u->pipe);
    }
    if (u->current_alt && !u->device->gone) usb_control(u->device, 0x01, 11, 0, u->streaming_interface, NULL, 0);
    u->current_alt = 0;
    if (u->buffers) arena_free(u->buffers, (uint64_t)u->slots * u->buffer_bytes);
    u->buffers = 0; u->inflight = 0;
}
static int start_stream(struct uvc* u) {
    uint8_t probe[48]; memset(probe, 0, sizeof(probe));
    put16(probe, 1);                                                     /* bmHint: dwFrameInterval fixed */
    probe[2] = u->want_format; probe[3] = u->want_frame; put32(probe + 4, u->want_interval);
    if (request(u, false, REQ_SET_CUR, 1, 0, u->streaming_interface, probe, u->probe_length) < 0) return -5;
    uint8_t got[48]; memset(got, 0, sizeof(got));
    if (request(u, true, REQ_GET_CUR, 1, 0, u->streaming_interface, got, u->probe_length) < 26) return -5;
    if (got[2] != u->want_format || got[3] != u->want_frame) return -22;   /* the camera did not take the format */
    if (request(u, false, REQ_SET_CUR, 2, 0, u->streaming_interface, got, u->probe_length) < 0) return -5;
    u->max_payload = le32(got + 22); uint32_t frame_size = le32(got + 18);
    if (!u->compressed && frame_size && frame_size != u->frame_size) return -22;   /* not the size the format promised */
    if (!u->max_payload) u->max_payload = u->bulk ? UVC_BULK_BYTES : UVC_MAX_PACKET;
    const struct uvc_alt* chosen = NULL;
    if (!u->bulk) {
        for (unsigned i = 0; i < u->alt_count; i++) {
            const struct uvc_alt* a = &u->alts[i];
            if (a->bandwidth > UVC_MAX_PACKET) continue;
            if (a->bandwidth >= u->max_payload && (!chosen || a->bandwidth < chosen->bandwidth)) chosen = a;
        }
        if (!chosen) for (unsigned i = 0; i < u->alt_count; i++) if (u->alts[i].bandwidth <= UVC_MAX_PACKET && (!chosen || u->alts[i].bandwidth > chosen->bandwidth)) chosen = &u->alts[i];
        if (!chosen) return -28;
    }
    unsigned bytes = 64;
    unsigned want = u->bulk ? (u->max_payload > UVC_BULK_BYTES ? UVC_BULK_BYTES : u->max_payload) : chosen->bandwidth;
    while (bytes < want) bytes <<= 1;
    u->buffer_bytes = bytes; u->slots = u->bulk ? 8 : UVC_DEPTH + 16;
    u->buffers = dma32_alloc((uint64_t)u->slots * bytes);
    if (!u->buffers) return -12;
    bool high = u->device->speed == USB_SPEED_HIGH;
    u->pipe = (struct usb_pipe){.owner = u, .complete = complete, .address = u->bulk ? u->bulk_endpoint : chosen->endpoint,
                                .type = u->bulk ? USB_PIPE_BULK : USB_PIPE_ISOCH, .interval = u->bulk ? 0 : chosen->interval,
                                .max_packet = (uint16_t)((u->bulk ? u->bulk_packet : chosen->max_packet) & 0x7FF),
                                .burst = (uint8_t)(!u->bulk && high ? (chosen->max_packet >> 11) & 3 : 0)};
    struct usb_pipe* add[1] = {&u->pipe};
    if (!usb_pipe_open(u->device, &u->pipe) || !usb_pipes_configure(u->device, add, 1, NULL, 0)) { release_stream(u); return -28; }
    if (!u->bulk) {
        if (usb_control(u->device, 0x01, 11, chosen->alt, u->streaming_interface, NULL, 0) < 0) { release_stream(u); return -5; }
        u->current_alt = chosen->alt;
    }
    u->generation++; u->next_slot = 0; u->inflight = 0; u->fid = -1; u->frame = NULL; u->frame_bytes = 0; u->skipping = false;
    u->streaming = true;
    while (u->inflight < (u->bulk ? 4U : UVC_DEPTH) && queue_one(u)) {}
    if (!u->inflight) { u->streaming = false; release_stream(u); return -5; }
    char text[120] = "camera streaming: format "; unsigned n = (unsigned)strlen(text);
    text[n++] = (char)('0' + u->want_format % 10); text[n++] = ' '; text[n++] = 'f'; text[n++] = 'r'; text[n++] = 'a'; text[n++] = 'm'; text[n++] = 'e';
    text[n++] = ' '; text[n++] = (char)('0' + u->want_frame % 10); text[n] = 0;
    strcat(text, u->bulk ? " bulk" : " isochronous alt "); if (!u->bulk) { n = (unsigned)strlen(text); text[n++] = (char)('0' + chosen->alt % 10); text[n] = 0; }
    usb_log_device(u->device, text);
    return 0;
}
static void stop_stream(struct uvc* u) {
    u->streaming = false; u->generation++;
    if (u->frame) { v4l2_frame_end(&u->cam, u->frame, u->frame_bytes, true); u->frame = NULL; }
    if (u->pipe.configured && !u->device->gone) usb_pipe_stop(&u->pipe);
    release_stream(u);
}
static void apply_controls(struct uvc* u) {
    while (u->dirty_controls) {
        unsigned index = (unsigned)__builtin_ctz(u->dirty_controls); u->dirty_controls &= ~(1U << index);
        if (index >= u->cam.control_count) continue;
        const struct v4l2_cam_control* c = &u->cam.controls[index];
        unsigned selector = c->driver_id & 0xFF, size = (c->driver_id >> 8) & 0xFF;
        uint8_t data[4] = {0, 0, 0, 0}; put16(data, (uint32_t)c->value);
        request(u, false, REQ_SET_CUR, selector, u->processing_unit, u->control_interface, data, size);
    }
}

/* ---------- V4L2 ops ---------- */
static struct uvc* of(struct v4l2_cam* cam) { return (struct uvc*)cam->context; }
static int uvc_start(struct v4l2_cam* cam, const struct v4l2_cam_format* format, const struct v4l2_cam_size* size, uint32_t interval) {
    struct uvc* u = of(cam);
    if (u->gone) return -19;
    unsigned index = (unsigned)(format - cam->formats);
    u->want_format = u->format_index[index]; u->want_frame = size->driver_index; u->want_interval = interval;
    u->compressed = format->compressed;
    u->frame_size = format->fourcc == V4L2_FMT_YUYV ? size->width * size->height * 2U : format->compressed ? 0 : size->width * size->height * 3U / 2;
    u->start_done = false; u->start_result = -5; u->work = (u->work & ~WORK_STOP) | WORK_START; usb_worker_kick();
    if (usb_in_worker()) { uvc_service(); return u->start_result; }
    uint64_t deadline = native_clock_ns() + 5000000000ULL;            /* control transfers run in the USB worker */
    int result = 0; u->waiters++;
    while (!u->start_done && !u->gone && native_clock_ns() < deadline) {
        int error = task_wait_until(u, native_clock_ns() + 10000000ULL);
        if (error && error != -110) { result = error; break; }
    }
    u->waiters--;
    if (u->detached) { if (!u->waiters) arena_free((uint64_t)u, sizeof(struct uvc)); return -19; }   /* unplugged meanwhile */
    if (!result && !u->start_done) result = -110;
    if (result) { u->work |= WORK_STOP; usb_worker_kick(); return result; }
    return u->start_result;
}
static void uvc_stop(struct v4l2_cam* cam) {
    struct uvc* u = of(cam);
    u->streaming = false; u->generation++;
    if (u->frame) { v4l2_frame_end(&u->cam, u->frame, u->frame_bytes, true); u->frame = NULL; }
    u->work = (u->work & ~WORK_START) | WORK_STOP; usb_worker_kick();
}
static int uvc_set_control(struct v4l2_cam* cam, struct v4l2_cam_control* control, int32_t value) {
    struct uvc* u = of(cam);
    if (u->gone) return -19;
    control->value = value;
    u->dirty_controls |= 1U << (unsigned)(control - cam->controls); u->work |= WORK_CONTROLS; usb_worker_kick();
    return 0;
}
static const struct v4l2_cam_ops ops = {uvc_start, uvc_stop, uvc_set_control, NULL, NULL};   /* frames come from USB completions */

/* ---------- attach / detach / worker ---------- */
void uvc_attach(struct usb_device* device) {
    unsigned slot = 0; while (slot < UVC_MAX_DEVICES && instances[slot]) slot++;
    if (slot >= UVC_MAX_DEVICES) { usb_log_device(device, "too many cameras"); return; }
    struct uvc* u = (struct uvc*)arena_alloc(sizeof(struct uvc));
    if (!u) { usb_log_device(device, "camera: out of memory"); return; }
    memset(u, 0, sizeof(*u)); u->device = device; u->fid = -1;
    unsigned length; const uint8_t* config = usb_config(device, &length);
    if (config) parse(u, config, length);
    if (!u->have_control || !u->have_streaming || !u->cam.format_count || (!u->alt_count && !u->bulk_endpoint)) {
        usb_log_device(device, "camera: no usable format (YUYV/NV12/I420/MJPEG) or streaming endpoint");
        arena_free((uint64_t)u, sizeof(struct uvc)); return;
    }
    const char* product = device->product_name[0] ? device->product_name : "USB Camera";
    unsigned n = 0; for (; product[n] && n < 31; n++) u->cam.card[n] = product[n]; u->cam.card[n] = 0;
    strcpy(u->cam.driver, "uvcvideo"); strcpy(u->cam.bus_info, "usb-xhci-");
    char port[3] = {(char)('0' + device->root_port / 10 % 10), (char)('0' + device->root_port % 10), 0}; strcat(u->cam.bus_info, port);
    u->cam.usb_vendor = device->vendor; u->cam.usb_product = device->product;
    u->cam.ops = &ops; u->cam.context = u;
    if (v4l2_register(&u->cam) < 0) { usb_log_device(device, "camera: no free video device"); arena_free((uint64_t)u, sizeof(struct uvc)); return; }
    u->registered = true; instances[slot] = u; device->class_driver[1] = u;
    char text[96] = "camera: UVC, formats "; unsigned k = (unsigned)strlen(text); text[k++] = (char)('0' + u->cam.format_count); text[k] = 0;
    strcat(text, u->bulk ? ", bulk" : ", isochronous"); strcat(text, ", controls "); k = (unsigned)strlen(text); text[k++] = (char)('0' + u->cam.control_count / 10); text[k++] = (char)('0' + u->cam.control_count % 10); text[k] = 0;
    usb_log_device(device, text);
}
void uvc_detach(struct usb_device* device) {
    struct uvc* u = device->class_driver[1];
    if (!u) return;
    u->gone = true; u->streaming = false;
    if (u->registered) v4l2_unregister(&u->cam);                        /* calls uvc_stop: flags only, no waiting */
    u->frame = NULL;
    release_stream(u);                                                   /* commands only while the device is there */
    task_wake_channel(u);
    for (unsigned i = 0; i < UVC_MAX_DEVICES; i++) if (instances[i] == u) instances[i] = NULL;
    device->class_driver[1] = NULL; u->detached = true;
    if (!u->waiters) arena_free((uint64_t)u, sizeof(struct uvc));       /* else the STREAMON waiter frees it */
}
bool uvc_service(void) {
    bool work = false;
    for (unsigned i = 0; i < UVC_MAX_DEVICES; i++) {
        struct uvc* u = instances[i];
        if (!u || u->gone || !u->work) continue;
        uint32_t todo = u->work; u->work = 0; work = true;
        if (todo & WORK_STOP) stop_stream(u);
        if (todo & WORK_START) {
            if (u->streaming || u->pipe.device) stop_stream(u);
            u->start_result = start_stream(u); u->start_done = true;
            if (u->start_result) release_stream(u);
            task_wake_channel(u);
        }
        if ((todo & WORK_CONTROLS) && instances[i] == u) apply_controls(u);
    }
    return work;
}
void uvc_poll(uint64_t now) { (void)now; }
