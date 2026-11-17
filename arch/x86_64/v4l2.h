/* Video4Linux2 capture devices (v4l2.c): /dev/videoN with the Linux V4L2 ABI that Chrome, Firefox (libwebrtc), ffmpeg,
 * GStreamer and v4l-utils use - QUERYCAP, ENUM_FMT/FRAMESIZES/FRAMEINTERVALS, G/S/TRY_FMT, G/S_PARM, inputs, controls
 * (QUERYCTRL/QUERY_EXT_CTRL/QUERYMENU, G/S_CTRL, G/S/TRY_EXT_CTRLS), MMAP streaming (REQBUFS, QUERYBUF, QBUF, DQBUF,
 * STREAMON/OFF, mmap, poll), priorities. Camera drivers (uvc.c for USB webcams, vcam.c the virtual test camera)
 * describe their formats and controls in a struct v4l2_cam, register it, and deliver frames into the buffers the
 * application queued through the producer calls below. All calls: kernel lock held. */
#ifndef NEXUS_V4L2_H
#define NEXUS_V4L2_H
#include "native.h"

#define V4L2_CAM_FORMATS 6
#define V4L2_CAM_SIZES 10
#define V4L2_CAM_INTERVALS 8
#define V4L2_CAM_CONTROLS 12
#define V4L2_CAM_MENU 4
#define V4L2_FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define V4L2_FMT_YUYV V4L2_FOURCC('Y', 'U', 'Y', 'V')
#define V4L2_FMT_NV12 V4L2_FOURCC('N', 'V', '1', '2')
#define V4L2_FMT_MJPEG V4L2_FOURCC('M', 'J', 'P', 'G')
#define V4L2_FMT_YU12 V4L2_FOURCC('Y', 'U', '1', '2')

struct v4l2_cam_size { uint16_t width, height; uint32_t intervals[V4L2_CAM_INTERVALS]; uint8_t interval_count; uint8_t driver_index; };
struct v4l2_cam_format {
    uint32_t fourcc; char description[32]; bool compressed;
    struct v4l2_cam_size sizes[V4L2_CAM_SIZES]; uint8_t size_count; uint8_t driver_index;
};
/* Integer/boolean/menu controls (V4L2 CIDs); intervals are in 100 ns units (UVC's dwFrameInterval). */
enum { V4L2_CAM_CONTROL_INTEGER = 1, V4L2_CAM_CONTROL_BOOLEAN = 2, V4L2_CAM_CONTROL_MENU = 3 };
struct v4l2_cam_control {
    uint32_t id; uint8_t type; char name[32]; int32_t minimum, maximum, step, def, value;
    char menu[V4L2_CAM_MENU][32];                /* menu entry names (index 0..maximum) */
    uint32_t driver_id;                          /* driver-private (UVC selector/unit) */
};
struct v4l2_cam;
struct v4l2_cam_ops {
    /* Begin streaming the chosen format/size/interval: frames go through v4l2_frame_*. 0 or -errno. */
    int (*start)(struct v4l2_cam* cam, const struct v4l2_cam_format* format, const struct v4l2_cam_size* size, uint32_t interval);
    void (*stop)(struct v4l2_cam* cam);
    int (*set_control)(struct v4l2_cam* cam, struct v4l2_cam_control* control, int32_t value);   /* optional */
    void (*poll)(struct v4l2_cam* cam, uint64_t now);                                            /* optional: periodic */
    uint64_t (*next)(struct v4l2_cam* cam, uint64_t now);   /* optional: when poll has work next (CLOCK_MONOTONIC ns); ~0 none */
};
struct v4l2_cam {
    char card[32], driver[16], bus_info[32];
    struct v4l2_cam_format formats[V4L2_CAM_FORMATS]; unsigned format_count;
    struct v4l2_cam_control controls[V4L2_CAM_CONTROLS]; unsigned control_count;
    const struct v4l2_cam_ops* ops; void* context;
    uint16_t usb_vendor, usb_product;            /* sysfs device/../idVendor, idProduct (0: not USB) */
    struct v4l2_device* core;                    /* v4l2.c */
};
/* Register: a free /dev/videoN appears (uevent SUBSYSTEM=video4linux); returns N or -errno. */
int v4l2_register(struct v4l2_cam* cam);
/* The device is gone (unplug): streaming stops, open fds get ENODEV / POLLERR|POLLHUP until closed, the node goes. */
void v4l2_unregister(struct v4l2_cam* cam);
/* Producer: the oldest buffer the application queued (NULL: none queued or not streaming - the frame is dropped),
   write it with v4l2_frame_write (offset < capacity), then complete it with the bytes used. */
struct v4l2_frame;
struct v4l2_frame* v4l2_frame_begin(struct v4l2_cam* cam, uint32_t* capacity);
void v4l2_frame_write(struct v4l2_frame* frame, uint32_t offset, const void* data, uint32_t length);
uint8_t* v4l2_frame_page(struct v4l2_frame* frame, uint32_t page);   /* kernel address of page `page` of the buffer */
void v4l2_frame_end(struct v4l2_cam* cam, struct v4l2_frame* frame, uint32_t bytes, bool error);
void v4l2_frame_drop(struct v4l2_cam* cam);                         /* a frame was lost: the sequence moves on */
void v4l2_poll(uint64_t now);                                       /* scheduler loop: drivers' periodic work */
void vcam_init(void);                                               /* virtual camera (boot option vcam) */
#endif
