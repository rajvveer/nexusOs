/* Video4Linux2 core (see v4l2.h): device nodes, the ioctl ABI, buffer queues and mmap streaming.
 *
 * Each registered camera gets /dev/videoN (char 81:N) through hwnode.c. Any number of fds may open it and query or set
 * formats and controls; the fd that requests buffers owns the queue until it frees them (REQBUFS 0), closes or the
 * device goes away (others get EBUSY, as with videobuf2). Buffers are page-backed memory files (pinned, sealed against
 * hole punching like the ALSA buffers), mapped with mmap at their V4L2 offset (index << 24). Queued buffers wait in
 * FIFO order for the driver (v4l2_frame_begin/end); completed ones wait for DQBUF (blocking or O_NONBLOCK -> EAGAIN)
 * and make poll() readable. Timestamps are CLOCK_MONOTONIC, sequence numbers count every frame the driver produced
 * or dropped. Formats follow V4L2 semantics: S_FMT/TRY_FMT never fail on an unsupported request but return the
 * nearest supported format and size. */
#include "v4l2.h"
#include "hwnode.h"
#include "uaccess.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define DEVICES 8U
#define HANDLES 16U
#define MAX_BUFFERS 32U
#define MAX_BUFFER_BYTES (16U << 20)
#define OFFSET_SHIFT 24
enum { E_PERM = -1, E_NOENT = -2, E_INTR = -4, E_IO = -5, E_AGAIN = -11, E_NOMEM = -12, E_FAULT = -14, E_BUSY = -16, E_NODEV = -19,
       E_INVAL = -22, E_NOTTY = -25, E_NOSPC = -28, E_PIPE = -32, E_NODATA = -61 };
enum { TYPE_CAPTURE = 1, MEMORY_MMAP = 1, FIELD_NONE = 1, COLORSPACE_SRGB = 8, COLORSPACE_JPEG = 7 };
enum { CAP_VIDEO_CAPTURE = 0x1, CAP_STREAMING = 0x04000000, CAP_EXT_PIX_FORMAT = 0x00200000, CAP_DEVICE_CAPS = 0x80000000 };
enum { BUF_MAPPED = 0x1, BUF_QUEUED = 0x2, BUF_DONE = 0x4, BUF_ERROR = 0x40, BUF_TIMESTAMP_MONOTONIC = 0x2000, BUF_TSTAMP_SRC_SOE = 0x10000 };
enum { FMT_FLAG_COMPRESSED = 1 };
enum { CTRL_FLAG_NEXT_CTRL = 0x80000000U, CTRL_FLAG_NEXT_COMPOUND = 0x40000000U, CTRL_TYPE_INTEGER = 1, CTRL_TYPE_BOOLEAN = 2, CTRL_TYPE_MENU = 3,
       CTRL_CLASS_USER = 0x00980000, CTRL_CLASS_CAMERA = 0x009A0000 };

/* ---------- ABI (x86-64; checked against linux/videodev2.h by tests/test_v4l2_abi.py) ---------- */
struct v4l2_capability { uint8_t driver[16], card[32], bus_info[32]; uint32_t version, capabilities, device_caps, reserved[3]; };
struct v4l2_fmtdesc { uint32_t index, type, flags; uint8_t description[32]; uint32_t pixelformat, mbus_code, reserved[3]; };
struct v4l2_pix_format { uint32_t width, height, pixelformat, field, bytesperline, sizeimage, colorspace, priv, flags, ycbcr_enc, quantization, xfer_func; };
struct v4l2_format { uint32_t type; uint32_t pad; union { struct v4l2_pix_format pix; uint8_t raw[200]; } fmt; };
struct v4l2_requestbuffers { uint32_t count, type, memory, capabilities; uint8_t flags, reserved[3]; };
struct v4l2_timecode { uint32_t type, flags; uint8_t frames, seconds, minutes, hours, userbits[4]; };
struct v4l2_buffer {
    uint32_t index, type, bytesused, flags, field, pad; int64_t tv_sec, tv_usec; struct v4l2_timecode timecode;
    uint32_t sequence, memory; union { uint32_t offset; uint64_t userptr; int32_t fd; } m; uint32_t length, reserved2; uint32_t request_fd, pad2;
};
struct v4l2_fract { uint32_t numerator, denominator; };
struct v4l2_captureparm { uint32_t capability, capturemode; struct v4l2_fract timeperframe; uint32_t extendedmode, readbuffers, reserved[4]; };
struct v4l2_streamparm { uint32_t type; union { struct v4l2_captureparm capture; uint8_t raw[200]; } parm; };
struct v4l2_input { uint32_t index; uint8_t name[32]; uint32_t type, audioset, tuner; uint64_t std; uint32_t status, capabilities, reserved[3]; uint32_t pad; };
struct v4l2_queryctrl { uint32_t id, type; uint8_t name[32]; int32_t minimum, maximum, step, default_value; uint32_t flags, reserved[2]; };
struct v4l2_querymenu { uint32_t id, index; uint8_t name[32]; uint32_t reserved; } __attribute__((packed));
struct v4l2_control { uint32_t id; int32_t value; };
struct v4l2_frmsizeenum { uint32_t index, pixel_format, type; uint32_t width, height, pad[4]; uint32_t reserved[2]; };
struct v4l2_frmivalenum { uint32_t index, pixel_format, width, height, type; struct v4l2_fract value, pad[2]; uint32_t reserved[2]; };
struct v4l2_query_ext_ctrl { uint32_t id, type; char name[32]; int64_t minimum, maximum; uint64_t step; int64_t default_value; uint32_t flags, elem_size, elems, nr_of_dims, dims[4], reserved[32]; };
struct v4l2_ext_control { uint32_t id, size, reserved2; union { int32_t value; int64_t value64; uint64_t ptr; }; } __attribute__((packed));
struct v4l2_ext_controls { uint32_t which, count, error_idx; int32_t request_fd; uint32_t reserved; uint32_t pad; uint64_t controls; };
_Static_assert(sizeof(struct v4l2_capability) == 104 && sizeof(struct v4l2_fmtdesc) == 64 && sizeof(struct v4l2_format) == 208, "v4l2 abi");
_Static_assert(sizeof(struct v4l2_requestbuffers) == 20 && sizeof(struct v4l2_buffer) == 88 && sizeof(struct v4l2_streamparm) == 204, "v4l2 abi");
_Static_assert(sizeof(struct v4l2_input) == 80 && sizeof(struct v4l2_queryctrl) == 68 && sizeof(struct v4l2_querymenu) == 44, "v4l2 abi");
_Static_assert(sizeof(struct v4l2_frmsizeenum) == 44 && sizeof(struct v4l2_frmivalenum) == 52 && sizeof(struct v4l2_query_ext_ctrl) == 232, "v4l2 abi");
_Static_assert(sizeof(struct v4l2_ext_control) == 20 && sizeof(struct v4l2_ext_controls) == 32, "v4l2 abi");
#define IOC(dir, nr, size) ((uint32_t)(dir) << 30 | (uint32_t)(size) << 16 | (uint32_t)'V' << 8 | (nr))
#define IOR(nr, size) IOC(2, nr, size)
#define IOW(nr, size) IOC(1, nr, size)
#define IOWR(nr, size) IOC(3, nr, size)
enum {
    QUERYCAP = IOR(0, 104), ENUM_FMT = IOWR(2, 64), G_FMT = IOWR(4, 208), S_FMT = IOWR(5, 208), REQBUFS = IOWR(8, 20),
    QUERYBUF = IOWR(9, 88), QBUF = IOWR(15, 88), EXPBUF = IOWR(16, 64), DQBUF = IOWR(17, 88), STREAMON = IOW(18, 4), STREAMOFF = IOW(19, 4),
    G_PARM = IOWR(21, 204), S_PARM = IOWR(22, 204), G_STD = IOR(23, 8), S_STD = IOW(24, 8), ENUMSTD = IOWR(25, 72), ENUMINPUT = IOWR(26, 80),
    G_CTRL = IOWR(27, 8), S_CTRL = IOWR(28, 8), QUERYCTRL = IOWR(36, 68), QUERYMENU = IOWR(37, 44), G_INPUT = IOR(38, 4), S_INPUT = IOWR(39, 4),
    CROPCAP = IOWR(58, 44), TRY_FMT = IOWR(64, 208), G_PRIORITY = IOR(67, 4), S_PRIORITY = IOW(68, 4), G_EXT_CTRLS = IOWR(71, 32),
    S_EXT_CTRLS = IOWR(72, 32), TRY_EXT_CTRLS = IOWR(73, 32), ENUM_FRAMESIZES = IOWR(74, 44), ENUM_FRAMEINTERVALS = IOWR(75, 52),
    G_SELECTION = IOWR(94, 64), SUBSCRIBE_EVENT = IOW(90, 32), UNSUBSCRIBE_EVENT = IOW(91, 32), DQEVENT = IOR(89, 136),
    QUERY_EXT_CTRL = IOWR(103, 232), LOG_STATUS = IOC(0, 70, 0)
};

/* ---------- state ---------- */
struct buffer {
    struct memory_file* file; uint64_t* pages; unsigned page_count;
    uint32_t length, bytesused, flags, sequence; uint64_t timestamp; bool queued, done;
};
struct v4l2_frame { struct buffer* buffer; unsigned index; };
struct v4l2_device {
    struct v4l2_cam* cam; int slot; unsigned minor; bool used, gone;
    unsigned format, size; uint32_t interval, bytesperline, sizeimage;
    struct buffer buffers[MAX_BUFFERS]; unsigned buffer_count; int owner;    /* owner: handle index or -1 */
    unsigned queue[MAX_BUFFERS], queue_head, queue_count;                    /* queued, oldest first */
    unsigned done[MAX_BUFFERS], done_head, done_count;                       /* completed, oldest first */
    bool streaming; uint32_t sequence; unsigned priority_count[4];
    struct v4l2_frame frame; bool frame_open;
    uint64_t frames, drops;
};
struct handle { bool used; struct v4l2_device* device; uint32_t priority; uint64_t generation; };
static struct v4l2_device devices[DEVICES];
static struct handle handles[HANDLES];
static uint64_t generations;

static struct v4l2_device* by_cam(const struct v4l2_cam* cam) { return cam && cam->core && cam->core->cam == cam ? cam->core : NULL; }
static struct handle* handle_of(const struct input_reader* reader) {
    unsigned i = reader ? (unsigned)(reader->sequence & 0xFF) : 0;
    if (!i || i > HANDLES || !handles[i - 1].used || handles[i - 1].generation != reader->sequence >> 8) return NULL;
    return &handles[i - 1];
}
static const struct v4l2_cam_format* current_format(const struct v4l2_device* d) { return &d->cam->formats[d->format]; }
static const struct v4l2_cam_size* current_size(const struct v4l2_device* d) { return &current_format(d)->sizes[d->size]; }
static void geometry(const struct v4l2_cam_format* f, const struct v4l2_cam_size* s, uint32_t* bytesperline, uint32_t* sizeimage) {
    uint32_t w = s->width, h = s->height;
    if (f->fourcc == V4L2_FMT_YUYV) { *bytesperline = w * 2; *sizeimage = w * h * 2; }
    else if (f->fourcc == V4L2_FMT_NV12 || f->fourcc == V4L2_FMT_YU12) { *bytesperline = w; *sizeimage = w * h * 3 / 2; }
    else { *bytesperline = 0; *sizeimage = w * h * 2 + 4096; }           /* compressed: worst case like uvcvideo */
}
static void set_format(struct v4l2_device* d, unsigned format, unsigned size) {
    d->format = format; d->size = size; geometry(current_format(d), current_size(d), &d->bytesperline, &d->sizeimage);
    if (!d->interval || current_size(d)->interval_count) {
        const struct v4l2_cam_size* s = current_size(d); bool listed = false;
        for (unsigned i = 0; i < s->interval_count; i++) listed |= s->intervals[i] == d->interval;
        if (!listed) d->interval = s->interval_count ? s->intervals[0] : 333333;
    }
}

/* ---------- buffers ---------- */
static void buffers_free(struct v4l2_device* d) {
    for (unsigned i = 0; i < d->buffer_count; i++) {
        struct buffer* b = &d->buffers[i];
        for (unsigned k = 0; k < b->page_count; k++) page_free(b->pages[k]);
        if (b->pages) arena_free((uint64_t)b->pages, ((uint64_t)b->page_count * 8 + 4095) & ~4095ULL);
        if (b->file) memory_file_release(b->file);
        memset(b, 0, sizeof(*b));
    }
    d->buffer_count = 0; d->queue_count = d->done_count = 0; d->queue_head = d->done_head = 0; d->frame_open = false;
}
static bool buffers_mapped(const struct v4l2_device* d) {
    for (unsigned i = 0; i < d->buffer_count; i++) if (d->buffers[i].file && d->buffers[i].file->references > 1) return true;
    return false;
}
static int buffer_allocate(struct buffer* b, uint32_t length) {
    unsigned count = (length + 4095) / 4096;
    uint64_t list_bytes = ((uint64_t)count * 8 + 4095) & ~4095ULL;
    b->pages = (uint64_t*)arena_alloc(list_bytes); if (!b->pages) return E_NOMEM;
    b->file = memory_file_create(2);
    if (!b->file) { arena_free((uint64_t)b->pages, list_bytes); b->pages = NULL; return E_NOMEM; }
    b->file->size = (uint64_t)count * 4096; b->file->seals |= 16;           /* no hole punching under the producer */
    for (unsigned i = 0; i < count; i++) {
        uint64_t frame = memory_file_page(b->file, i, true);
        if (!frame) { b->page_count = i; return E_NOMEM; }
        page_retain(frame); b->pages[i] = (uint64_t)phys_to_virt(frame); b->page_count = i + 1;
    }
    b->length = length;
    return 0;
}
static void stream_stop(struct v4l2_device* d) {
    if (d->streaming && d->cam && !d->gone && d->cam->ops->stop) d->cam->ops->stop(d->cam);
    d->streaming = false; d->frame_open = false;
    for (unsigned i = 0; i < d->buffer_count; i++) { d->buffers[i].queued = d->buffers[i].done = false; d->buffers[i].flags &= ~(BUF_QUEUED | BUF_DONE); }
    d->queue_count = d->done_count = d->queue_head = d->done_head = 0;
    task_wake_channel(d); event_source_changed(d, 1U | 8U);
}

/* ---------- producer ---------- */
struct v4l2_frame* v4l2_frame_begin(struct v4l2_cam* cam, uint32_t* capacity) {
    struct v4l2_device* d = by_cam(cam);
    if (!d || d->gone || !d->streaming || !d->queue_count) return NULL;
    if (!d->frame_open) {
        unsigned index = d->queue[d->queue_head];
        d->frame = (struct v4l2_frame){&d->buffers[index], index}; d->frame_open = true;
    }
    *capacity = d->frame.buffer->length;
    return &d->frame;
}
uint8_t* v4l2_frame_page(struct v4l2_frame* frame, uint32_t page) { return page < frame->buffer->page_count ? (uint8_t*)frame->buffer->pages[page] : NULL; }
void v4l2_frame_write(struct v4l2_frame* frame, uint32_t offset, const void* data, uint32_t length) {
    struct buffer* b = frame->buffer; const uint8_t* source = data;
    if (offset >= b->length) return;
    if (length > b->length - offset) length = b->length - offset;
    while (length) {
        uint32_t page = offset / 4096, within = offset % 4096, n = 4096 - within; if (n > length) n = length;
        memcpy((uint8_t*)b->pages[page] + within, source, n);
        offset += n; source += n; length -= n;
    }
}
void v4l2_frame_end(struct v4l2_cam* cam, struct v4l2_frame* frame, uint32_t bytes, bool error) {
    struct v4l2_device* d = by_cam(cam);
    if (!d || !d->frame_open || frame != &d->frame || !d->queue_count) return;
    struct buffer* b = frame->buffer;
    d->queue_head = (d->queue_head + 1) % MAX_BUFFERS; d->queue_count--; d->frame_open = false;
    b->queued = false; b->done = true; b->bytesused = bytes > b->length ? b->length : bytes;
    b->flags = (b->flags & BUF_MAPPED) | BUF_DONE | BUF_TIMESTAMP_MONOTONIC | (error ? BUF_ERROR : 0);
    b->sequence = d->sequence++; b->timestamp = native_clock_ns(); d->frames++;
    d->done[(d->done_head + d->done_count) % MAX_BUFFERS] = frame->index; d->done_count++;
    task_wake_channel(d); event_source_changed(d, 1U);
}
void v4l2_frame_drop(struct v4l2_cam* cam) { struct v4l2_device* d = by_cam(cam); if (d && d->streaming) { d->sequence++; d->drops++; } }
/* The tickless boot CPU (power2's tick.c) asks when the cameras need v4l2_poll next: ~0 while none streams. */
uint64_t v4l2_next_event(uint64_t now) {
    uint64_t next = ~0ULL;
    for (unsigned i = 0; i < DEVICES; i++) {
        struct v4l2_device* d = &devices[i];
        if (!d->used || d->gone || !d->streaming || !d->cam->ops->poll) continue;
        uint64_t t = d->cam->ops->next ? d->cam->ops->next(d->cam, now) : now + 1000000ULL;
        if (t < next) next = t;
    }
    return next;
}
void v4l2_poll(uint64_t now) {
    for (unsigned i = 0; i < DEVICES; i++) {
        struct v4l2_device* d = &devices[i];
        if (d->used && !d->gone && d->cam->ops->poll) d->cam->ops->poll(d->cam, now);
    }
}

/* ---------- registration ---------- */
static const struct hwnode_ops node_ops;
static void uevent(struct v4l2_device* d, const char* action) {
    char path[64] = "/devices/virtual/video4linux/video", name[16] = "DEVNAME=video", minor[16] = "MINOR=";
    char digit[3] = {(char)('0' + d->minor), 0, 0}; strcat(path, digit); strcat(name, digit); strcat(minor, digit);
    const char* pairs[] = {"SUBSYSTEM=video4linux", name, "MAJOR=81", minor};
    storage_uevent(action, path, pairs, 4);
}
int v4l2_register(struct v4l2_cam* cam) {
    if (!cam || !cam->ops || !cam->ops->start || !cam->format_count || cam->format_count > V4L2_CAM_FORMATS) return E_INVAL;
    for (unsigned f = 0; f < cam->format_count; f++) if (!cam->formats[f].size_count || cam->formats[f].size_count > V4L2_CAM_SIZES) return E_INVAL;
    unsigned minor = 0;
    for (; minor < DEVICES; minor++) {
        bool taken = false; for (unsigned i = 0; i < DEVICES; i++) taken |= devices[i].used && devices[i].minor == minor;
        if (!taken) break;
    }
    struct v4l2_device* d = NULL;
    for (unsigned i = 0; i < DEVICES && !d; i++) if (!devices[i].used) d = &devices[i];
    if (!d || minor >= DEVICES) return E_NOSPC;
    memset(d, 0, sizeof(*d)); d->cam = cam; d->minor = minor; d->owner = -1; d->used = true; cam->core = d;
    set_format(d, 0, 0);
    char path[24] = "/dev/video"; char digit[2] = {(char)('0' + minor), 0}; strcat(path, digit);
    d->slot = hwnode_add(path, 81, minor, (unsigned)(d - devices), 0660, 1000, &node_ops, "video4linux");
    if (d->slot < 0) { d->used = false; cam->core = NULL; return E_NOSPC; }
    vga_print("[64] V4L2 "); vga_print(path); vga_print(": "); vga_print(cam->card); vga_print("\n");
    uevent(d, "add");
    return (int)minor;
}
static bool device_busy(const struct v4l2_device* d) {
    for (unsigned i = 0; i < HANDLES; i++) if (handles[i].used && handles[i].device == d) return true;
    return false;
}
void v4l2_unregister(struct v4l2_cam* cam) {
    struct v4l2_device* d = by_cam(cam);
    if (!d) return;
    stream_stop(d); d->gone = true; d->cam = NULL; cam->core = NULL;
    hwnode_remove(d->slot); uevent(d, "remove");
    vga_print("[64] V4L2 video device removed\n");
    task_wake_channel(d); event_source_changed(d, 1U | 8U | 16U);
    if (!device_busy(d)) { buffers_free(d); d->used = false; }   /* else: the last close frees it */
}

/* ---------- ioctls ---------- */
static bool get(uint64_t address, void* data, uint64_t size) { if (!accessible(address, size, false)) return false; copy_from_user(data, (const void*)address, size); return true; }
static bool put(uint64_t address, const void* data, uint64_t size) { if (!accessible(address, size, true)) return false; copy_to_user((void*)address, data, size); return true; }
static void fill_buffer(const struct v4l2_device* d, unsigned index, struct v4l2_buffer* out) {
    const struct buffer* b = &d->buffers[index];
    memset(out, 0, sizeof(*out)); out->index = index; out->type = TYPE_CAPTURE; out->memory = MEMORY_MMAP; out->field = FIELD_NONE;
    out->bytesused = b->done ? b->bytesused : 0; out->flags = b->flags | BUF_TIMESTAMP_MONOTONIC | (b->queued ? BUF_QUEUED : 0) | (b->done ? BUF_DONE : 0);
    if (b->file && b->file->references > 1) out->flags |= BUF_MAPPED;
    out->sequence = b->sequence; out->length = b->length; out->m.offset = index << OFFSET_SHIFT;
    out->tv_sec = (int64_t)(b->timestamp / 1000000000ULL); out->tv_usec = (int64_t)(b->timestamp % 1000000000ULL / 1000);
}
static const struct v4l2_cam_control* control_find(const struct v4l2_device* d, uint32_t id, unsigned* index) {
    for (unsigned i = 0; i < d->cam->control_count; i++) if (d->cam->controls[i].id == id) { if (index) *index = i; return &d->cam->controls[i]; }
    return NULL;
}
static const struct v4l2_cam_control* control_next(const struct v4l2_device* d, uint32_t after) {
    const struct v4l2_cam_control* best = NULL;
    for (unsigned i = 0; i < d->cam->control_count; i++) {
        const struct v4l2_cam_control* c = &d->cam->controls[i];
        if (c->id > after && (!best || c->id < best->id)) best = c;
    }
    return best;
}
/* Apply a value already checked by clamp_control (booleans 0/1, integers on their step). */
static int control_set(struct v4l2_device* d, uint32_t id, int32_t value) {
    unsigned index; const struct v4l2_cam_control* c = control_find(d, id, &index);
    if (!c) return E_INVAL;
    if (c->type == V4L2_CAM_CONTROL_BOOLEAN) value = value ? 1 : 0;
    else if (c->type == V4L2_CAM_CONTROL_INTEGER && c->step > 1) {
        value = c->minimum + (int32_t)(((int64_t)value - c->minimum + c->step / 2) / c->step * c->step);
        if (value > c->maximum) value = c->maximum;
    }
    struct v4l2_cam_control* writable = &d->cam->controls[index];
    if (d->cam->ops->set_control) { int error = d->cam->ops->set_control(d->cam, writable, value); if (error) return error; }
    writable->value = value; return 0;
}
static int clamp_control(const struct v4l2_device* d, uint32_t id, int32_t* value) {
    const struct v4l2_cam_control* c = control_find(d, id, NULL);
    if (!c) return E_INVAL;
    if (c->type == V4L2_CAM_CONTROL_MENU && (*value < c->minimum || *value > c->maximum)) return E_INVAL;
    if (*value < c->minimum) *value = c->minimum;
    if (*value > c->maximum) *value = c->maximum;
    return 0;
}
/* The format/size nearest to a request (exact fourcc, else the first format; the closest frame area). */
static void nearest(const struct v4l2_device* d, uint32_t fourcc, uint32_t width, uint32_t height, unsigned* format, unsigned* size) {
    *format = 0;
    for (unsigned f = 0; f < d->cam->format_count; f++) if (d->cam->formats[f].fourcc == fourcc) *format = f;
    const struct v4l2_cam_format* f = &d->cam->formats[*format]; uint64_t best = ~0ULL; *size = 0;
    for (unsigned s = 0; s < f->size_count; s++) {
        int64_t dw = (int64_t)f->sizes[s].width - width, dh = (int64_t)f->sizes[s].height - height;
        uint64_t distance = (uint64_t)(dw * dw + dh * dh);
        if (distance < best) { best = distance; *size = s; }
    }
}
static void fill_format(const struct v4l2_device* d, unsigned format, unsigned size, struct v4l2_format* out) {
    const struct v4l2_cam_format* f = &d->cam->formats[format]; const struct v4l2_cam_size* s = &f->sizes[size];
    memset(out, 0, sizeof(*out)); out->type = TYPE_CAPTURE;
    out->fmt.pix.width = s->width; out->fmt.pix.height = s->height; out->fmt.pix.pixelformat = f->fourcc; out->fmt.pix.field = FIELD_NONE;
    geometry(f, s, &out->fmt.pix.bytesperline, &out->fmt.pix.sizeimage);
    out->fmt.pix.colorspace = f->compressed ? COLORSPACE_JPEG : COLORSPACE_SRGB;
}
static int dqbuf(struct v4l2_device* d, struct handle* h, struct v4l2_buffer* out, bool nonblock) {
    uint64_t start = native_clock_ns();
    while (!d->done_count) {
        if (d->gone) return E_NODEV;
        if (!d->streaming) return E_INVAL;
        if (nonblock) return E_AGAIN;
        int error = task_wait_until(d, native_clock_ns() + 50000000ULL);
        if (error && error != -110) return error;
        if (!h->used || h->device != d) return E_NODEV;
        if (native_clock_ns() - start > 10000000000ULL) return E_IO;     /* no frame for 10 s: the camera stalled */
    }
    unsigned index = d->done[d->done_head]; d->done_head = (d->done_head + 1) % MAX_BUFFERS; d->done_count--;
    fill_buffer(d, index, out);                                          /* bytesused of the done buffer */
    d->buffers[index].done = false;
    out->flags = (out->flags & ~BUF_DONE) | (d->buffers[index].flags & BUF_ERROR);
    return 0;
}
static int64_t ioctl_device(struct v4l2_device* d, struct handle* h, uint32_t command, uint64_t argument, bool nonblock) {
    int index_self = (int)(h - handles);
    switch (command) {
    case QUERYCAP: {
        struct v4l2_capability c; memset(&c, 0, sizeof(c));
        strcpy((char*)c.driver, d->cam->driver); strcpy((char*)c.card, d->cam->card); strcpy((char*)c.bus_info, d->cam->bus_info);
        c.version = 0x00060B00; c.device_caps = CAP_VIDEO_CAPTURE | CAP_STREAMING | CAP_EXT_PIX_FORMAT; c.capabilities = c.device_caps | CAP_DEVICE_CAPS;
        return put(argument, &c, sizeof(c)) ? 0 : E_FAULT;
    }
    case ENUM_FMT: {
        struct v4l2_fmtdesc f; if (!get(argument, &f, sizeof(f))) return E_FAULT;
        if (f.type != TYPE_CAPTURE || f.index >= d->cam->format_count) return E_INVAL;
        const struct v4l2_cam_format* cf = &d->cam->formats[f.index]; uint32_t index = f.index;
        memset(&f, 0, sizeof(f)); f.index = index; f.type = TYPE_CAPTURE; f.flags = cf->compressed ? FMT_FLAG_COMPRESSED : 0;
        memcpy(f.description, cf->description, 31); f.pixelformat = cf->fourcc;
        return put(argument, &f, sizeof(f)) ? 0 : E_FAULT;
    }
    case ENUM_FRAMESIZES: {
        struct v4l2_frmsizeenum f; if (!get(argument, &f, sizeof(f))) return E_FAULT;
        unsigned format = d->cam->format_count;
        for (unsigned i = 0; i < d->cam->format_count; i++) if (d->cam->formats[i].fourcc == f.pixel_format) format = i;
        if (format >= d->cam->format_count || f.index >= d->cam->formats[format].size_count) return E_INVAL;
        uint32_t index = f.index, fourcc = f.pixel_format; memset(&f, 0, sizeof(f));
        f.index = index; f.pixel_format = fourcc; f.type = 1; f.width = d->cam->formats[format].sizes[index].width; f.height = d->cam->formats[format].sizes[index].height;
        return put(argument, &f, sizeof(f)) ? 0 : E_FAULT;
    }
    case ENUM_FRAMEINTERVALS: {
        struct v4l2_frmivalenum f; if (!get(argument, &f, sizeof(f))) return E_FAULT;
        const struct v4l2_cam_size* s = NULL;
        for (unsigned i = 0; i < d->cam->format_count && !s; i++) if (d->cam->formats[i].fourcc == f.pixel_format)
            for (unsigned k = 0; k < d->cam->formats[i].size_count; k++) if (d->cam->formats[i].sizes[k].width == f.width && d->cam->formats[i].sizes[k].height == f.height) s = &d->cam->formats[i].sizes[k];
        if (!s || f.index >= s->interval_count) return E_INVAL;
        struct v4l2_frmivalenum out; memset(&out, 0, sizeof(out));
        out.index = f.index; out.pixel_format = f.pixel_format; out.width = f.width; out.height = f.height; out.type = 1;
        out.value = (struct v4l2_fract){s->intervals[f.index], 10000000};
        return put(argument, &out, sizeof(out)) ? 0 : E_FAULT;
    }
    case G_FMT: {
        struct v4l2_format f; if (!get(argument, &f, sizeof(f))) return E_FAULT;
        if (f.type != TYPE_CAPTURE) return E_INVAL;
        fill_format(d, d->format, d->size, &f); return put(argument, &f, sizeof(f)) ? 0 : E_FAULT;
    }
    case S_FMT: case TRY_FMT: {
        struct v4l2_format f; if (!get(argument, &f, sizeof(f))) return E_FAULT;
        if (f.type != TYPE_CAPTURE) return E_INVAL;
        unsigned format, size; nearest(d, f.fmt.pix.pixelformat, f.fmt.pix.width, f.fmt.pix.height, &format, &size);
        if (command == S_FMT) {
            if (d->streaming || (d->buffer_count && d->owner != index_self)) return E_BUSY;
            if (d->buffer_count) return E_BUSY;                              /* videobuf2: buffers allocated */
            set_format(d, format, size);
        }
        fill_format(d, format, size, &f); return put(argument, &f, sizeof(f)) ? 0 : E_FAULT;
    }
    case G_PARM: case S_PARM: {
        struct v4l2_streamparm p; if (!get(argument, &p, sizeof(p))) return E_FAULT;
        if (p.type != TYPE_CAPTURE) return E_INVAL;
        if (command == S_PARM && p.parm.capture.timeperframe.denominator && p.parm.capture.timeperframe.numerator) {
            if (d->streaming) return E_BUSY;
            uint64_t wanted = (uint64_t)p.parm.capture.timeperframe.numerator * 10000000ULL / p.parm.capture.timeperframe.denominator;
            const struct v4l2_cam_size* s = current_size(d); uint64_t best = ~0ULL;
            for (unsigned i = 0; i < s->interval_count; i++) {
                uint64_t distance = s->intervals[i] > wanted ? s->intervals[i] - wanted : wanted - s->intervals[i];
                if (distance < best) { best = distance; d->interval = s->intervals[i]; }
            }
        }
        memset(&p, 0, sizeof(p)); p.type = TYPE_CAPTURE; p.parm.capture.capability = 0x1000;   /* V4L2_CAP_TIMEPERFRAME */
        p.parm.capture.timeperframe = (struct v4l2_fract){d->interval, 10000000}; p.parm.capture.readbuffers = 0;
        return put(argument, &p, sizeof(p)) ? 0 : E_FAULT;
    }
    case ENUMINPUT: {
        struct v4l2_input in; if (!get(argument, &in, sizeof(in))) return E_FAULT;
        if (in.index) return E_INVAL;
        memset(&in, 0, sizeof(in)); strcpy((char*)in.name, "Camera 1"); in.type = 2;   /* V4L2_INPUT_TYPE_CAMERA */
        return put(argument, &in, sizeof(in)) ? 0 : E_FAULT;
    }
    case G_INPUT: { int32_t zero = 0; return put(argument, &zero, 4) ? 0 : E_FAULT; }
    case S_INPUT: { int32_t value; if (!get(argument, &value, 4)) return E_FAULT; return value ? E_INVAL : 0; }
    case G_STD: case S_STD: case ENUMSTD: return E_NODATA;     /* cameras have no analog TV standard */
    case G_PRIORITY: { uint32_t priority = 2; for (unsigned p = 3; p > 0; p--) if (d->priority_count[p]) { priority = p; break; } return put(argument, &priority, 4) ? 0 : E_FAULT; }
    case S_PRIORITY: {
        uint32_t priority; if (!get(argument, &priority, 4)) return E_FAULT;
        if (priority > 3) return E_INVAL;
        if (priority == 3 && d->priority_count[3] && h->priority != 3) return E_BUSY;
        d->priority_count[h->priority]--; h->priority = priority; d->priority_count[priority]++; return 0;
    }
    case QUERYCTRL: {
        struct v4l2_queryctrl q; if (!get(argument, &q, sizeof(q))) return E_FAULT;
        const struct v4l2_cam_control* c = q.id & CTRL_FLAG_NEXT_CTRL ? control_next(d, q.id & ~(CTRL_FLAG_NEXT_CTRL | CTRL_FLAG_NEXT_COMPOUND)) : control_find(d, q.id, NULL);
        if (!c) return E_INVAL;
        memset(&q, 0, sizeof(q)); q.id = c->id; q.type = c->type; memcpy(q.name, c->name, 31);
        q.minimum = c->minimum; q.maximum = c->maximum; q.step = c->type == V4L2_CAM_CONTROL_MENU ? 1 : c->step; q.default_value = c->def;
        return put(argument, &q, sizeof(q)) ? 0 : E_FAULT;
    }
    case QUERY_EXT_CTRL: {
        static struct v4l2_query_ext_ctrl q;               /* 232 bytes, kernel lock held */
        if (!get(argument, &q, sizeof(q))) return E_FAULT;
        const struct v4l2_cam_control* c = q.id & CTRL_FLAG_NEXT_CTRL ? control_next(d, q.id & ~(CTRL_FLAG_NEXT_CTRL | CTRL_FLAG_NEXT_COMPOUND)) : control_find(d, q.id, NULL);
        if (!c) return E_INVAL;
        memset(&q, 0, sizeof(q)); q.id = c->id; q.type = c->type; memcpy(q.name, c->name, 31);
        q.minimum = c->minimum; q.maximum = c->maximum; q.step = c->type == V4L2_CAM_CONTROL_MENU ? 1 : (uint64_t)c->step; q.default_value = c->def;
        q.elem_size = 4; q.elems = 1;
        return put(argument, &q, sizeof(q)) ? 0 : E_FAULT;
    }
    case QUERYMENU: {
        struct v4l2_querymenu m; if (!get(argument, &m, sizeof(m))) return E_FAULT;
        const struct v4l2_cam_control* c = control_find(d, m.id, NULL);
        if (!c || c->type != V4L2_CAM_CONTROL_MENU || (int32_t)m.index < c->minimum || (int32_t)m.index > c->maximum || m.index >= V4L2_CAM_MENU || !c->menu[m.index][0]) return E_INVAL;
        uint32_t id = m.id, index = m.index; memset(&m, 0, sizeof(m)); m.id = id; m.index = index; memcpy(m.name, c->menu[index], 31);
        return put(argument, &m, sizeof(m)) ? 0 : E_FAULT;
    }
    case G_CTRL: case S_CTRL: {
        struct v4l2_control c; if (!get(argument, &c, sizeof(c))) return E_FAULT;
        const struct v4l2_cam_control* cc = control_find(d, c.id, NULL); if (!cc) return E_INVAL;
        if (command == S_CTRL) { int32_t value = c.value; int error = clamp_control(d, c.id, &value); if (!error) error = control_set(d, c.id, value); if (error) return error; }
        c.value = control_find(d, c.id, NULL)->value; return put(argument, &c, sizeof(c)) ? 0 : E_FAULT;
    }
    case G_EXT_CTRLS: case S_EXT_CTRLS: case TRY_EXT_CTRLS: {
        struct v4l2_ext_controls x; if (!get(argument, &x, sizeof(x))) return E_FAULT;
        if (x.count > 64) return E_INVAL;
        if (x.which != 0 && x.which != CTRL_CLASS_USER && x.which != CTRL_CLASS_CAMERA && x.which != 0x0F010000) return E_INVAL;  /* CUR / class / DEF_VAL */
        for (unsigned pass = 0; pass < 2; pass++)                 /* validate all first, then apply (atomic for the caller) */
            for (uint32_t i = 0; i < x.count; i++) {
                struct v4l2_ext_control c; uint64_t at = x.controls + (uint64_t)i * sizeof(c);
                if (at < x.controls || !get(at, &c, sizeof(c))) return E_FAULT;
                const struct v4l2_cam_control* cc = control_find(d, c.id, NULL);
                if (!cc) { x.error_idx = command == S_EXT_CTRLS && pass ? i : x.count; put(argument, &x, sizeof(x)); return E_INVAL; }
                if (command == G_EXT_CTRLS) { if (pass) { c.value = x.which == 0x0F010000 ? cc->def : cc->value; if (!put(at, &c, sizeof(c))) return E_FAULT; } continue; }
                int32_t value = c.value; int error = clamp_control(d, c.id, &value);
                if (error) { x.error_idx = i; put(argument, &x, sizeof(x)); return error; }
                if (pass && command == S_EXT_CTRLS) { error = control_set(d, c.id, value); if (error) { x.error_idx = i; put(argument, &x, sizeof(x)); return error; } }
                if (pass) { c.value = value; if (!put(at, &c, sizeof(c))) return E_FAULT; }
            }
        return 0;
    }
    case REQBUFS: {
        struct v4l2_requestbuffers r; if (!get(argument, &r, sizeof(r))) return E_FAULT;
        if (r.type != TYPE_CAPTURE) return E_INVAL;
        if (r.memory != MEMORY_MMAP) return E_INVAL;              /* USERPTR/DMABUF: not supported (capabilities say so) */
        if (d->owner >= 0 && d->owner != index_self) return E_BUSY;
        if (d->streaming) { if (r.count) return E_BUSY; stream_stop(d); }
        if (d->buffer_count && buffers_mapped(d)) return E_BUSY;
        buffers_free(d); d->owner = -1;
        uint32_t count = r.count > MAX_BUFFERS ? MAX_BUFFERS : r.count;
        if (d->sizeimage > MAX_BUFFER_BYTES) return E_INVAL;
        for (uint32_t i = 0; i < count; i++) {
            int error = buffer_allocate(&d->buffers[i], d->sizeimage);
            d->buffer_count = i + 1;
            if (error) { buffers_free(d); return error; }               /* all or nothing */
        }
        if (count) d->owner = index_self;
        memset(&r, 0, sizeof(r)); r.count = count; r.type = TYPE_CAPTURE; r.memory = MEMORY_MMAP; r.capabilities = 1;   /* V4L2_BUF_CAP_SUPPORTS_MMAP */
        return put(argument, &r, sizeof(r)) ? 0 : E_FAULT;
    }
    case QUERYBUF: case QBUF: {
        struct v4l2_buffer b; if (!get(argument, &b, sizeof(b))) return E_FAULT;
        if (b.type != TYPE_CAPTURE || b.index >= d->buffer_count) return E_INVAL;
        if (command == QBUF) {
            if (d->owner != index_self) return E_BUSY;
            if (b.memory != MEMORY_MMAP) return E_INVAL;
            struct buffer* buffer = &d->buffers[b.index];
            if (buffer->queued || buffer->done || d->queue_count >= MAX_BUFFERS) return E_INVAL;   /* already queued */
            buffer->queued = true; buffer->flags &= ~(BUF_ERROR | BUF_DONE);
            d->queue[(d->queue_head + d->queue_count) % MAX_BUFFERS] = b.index; d->queue_count++;
        }
        fill_buffer(d, b.index, &b); return put(argument, &b, sizeof(b)) ? 0 : E_FAULT;
    }
    case DQBUF: {
        struct v4l2_buffer b; if (!get(argument, &b, sizeof(b))) return E_FAULT;
        if (b.type != TYPE_CAPTURE || b.memory != MEMORY_MMAP) return E_INVAL;
        if (d->owner != index_self) return E_BUSY;
        int error = dqbuf(d, h, &b, nonblock); if (error) return error;
        return put(argument, &b, sizeof(b)) ? 0 : E_FAULT;
    }
    case STREAMON: case STREAMOFF: {
        int32_t type; if (!get(argument, &type, 4)) return E_FAULT;
        if (type != TYPE_CAPTURE) return E_INVAL;
        if (d->owner != index_self) return d->owner < 0 && command == STREAMOFF ? 0 : E_BUSY;
        if (command == STREAMOFF) { stream_stop(d); return 0; }
        if (d->streaming) return 0;
        if (!d->buffer_count) return E_INVAL;
        d->sequence = 0; d->streaming = true; d->done_count = d->done_head = 0;
        int error = d->cam->ops->start(d->cam, current_format(d), current_size(d), d->interval);
        if (error) { d->streaming = false; return error; }
        return 0;
    }
    case EXPBUF: case G_SELECTION: case CROPCAP: case SUBSCRIBE_EVENT: case UNSUBSCRIBE_EVENT: case DQEVENT: return E_NOTTY;
    case LOG_STATUS: return 0;
    }
    return E_NOTTY;
}

/* ---------- hwnode ops (cookie = device index) ---------- */
static struct v4l2_device* device_of(unsigned cookie) { return cookie < DEVICES && devices[cookie].used ? &devices[cookie] : NULL; }
static void node_open(unsigned cookie, uint64_t flags, struct input_reader* reader) {
    (void)flags; struct v4l2_device* d = device_of(cookie);
    if (!d) return;
    for (unsigned i = 0; i < HANDLES; i++) if (!handles[i].used) {
        handles[i] = (struct handle){true, d, 2, ++generations}; d->priority_count[2]++;
        reader->sequence = handles[i].generation << 8 | (i + 1); return;
    }
}
static int node_open_check(unsigned cookie, uint64_t flags) {
    (void)flags; struct v4l2_device* d = device_of(cookie);
    if (!d || d->gone) return E_NODEV;
    for (unsigned i = 0; i < HANDLES; i++) if (!handles[i].used) return 0;
    return E_BUSY;
}
static void node_close(unsigned cookie, uint64_t flags, struct input_reader* reader) {
    (void)flags; (void)cookie; struct handle* h = handle_of(reader);
    if (!h) return;
    struct v4l2_device* d = h->device; int self = (int)(h - handles);
    /* Our references go; a mapping that outlives the fd keeps its pages until munmap (the producer no longer writes). */
    if (d->owner == self) { stream_stop(d); d->owner = -1; buffers_free(d); }
    if (d->priority_count[h->priority]) d->priority_count[h->priority]--;
    h->used = false; reader->sequence = 0;
    if (d->gone && !device_busy(d)) { buffers_free(d); d->used = false; }
}
static int64_t node_ioctl(unsigned cookie, struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)cookie; struct handle* h = handle_of(reader);
    if (!h) return E_NODEV;
    struct v4l2_device* d = h->device;
    if (d->gone || !d->cam) return E_NODEV;
    return ioctl_device(d, h, (uint32_t)command, argument, (flags & 0x800) != 0);
}
static int64_t node_io(unsigned cookie, struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags) {
    (void)cookie; (void)buffer; (void)count; (void)write; (void)flags;
    return handle_of(reader) ? E_INVAL : E_NODEV;                        /* no read()/write() I/O (CAP_READWRITE not set) */
}
static uint32_t node_ready(unsigned cookie, const struct input_reader* reader) {
    (void)cookie; struct handle* h = handle_of(reader);
    if (!h || h->device->gone) return 8U | 16U;                          /* POLLERR | POLLHUP */
    struct v4l2_device* d = h->device;
    if (d->owner != (int)(h - handles)) return 0;
    if (d->done_count) return 1U | 64U;                                  /* POLLIN | POLLRDNORM */
    return d->streaming || d->queue_count ? 0 : 8U;                      /* nothing streaming: POLLERR, as videobuf2 */
}
static void* node_source(unsigned cookie, const struct input_reader* reader) { (void)cookie; struct handle* h = handle_of(reader); return h ? (void*)h->device : NULL; }
static struct memory_file* node_mapping(unsigned cookie, uint64_t* offset, uint64_t length, struct input_reader* reader) {
    (void)cookie; struct handle* h = handle_of(reader);
    if (!h || h->device->gone) return NULL;
    struct v4l2_device* d = h->device;
    uint64_t index = *offset >> OFFSET_SHIFT;
    if ((*offset & ((1ULL << OFFSET_SHIFT) - 1)) || index >= d->buffer_count) return NULL;
    struct buffer* b = &d->buffers[index];
    if (!b->file || length > b->file->size) return NULL;
    *offset = 0; return b->file;
}
static const struct hwnode_ops node_ops = {node_open_check, node_open, node_close, node_io, node_ioctl, node_ready, node_source, node_mapping, NULL};
