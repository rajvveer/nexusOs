/* V4L2 capture test (tests/test_x64_camera.py), built against the real linux/videodev2.h (so it also checks the kernel's
   ABI), run as /bin/hw64 by the /nexus/test-hw campaign on a kernel booted with "vcam" (vcam.c's virtual camera).
   Mode = first byte of /nexus/test-hw: 'v' full campaign, 'n' no camera expected.
   Checks: device discovery like Chrome (/dev/video*, QUERYCAP device caps), formats/sizes/intervals, S_FMT/TRY_FMT
   nearest-match semantics, controls (QUERYCTRL enumeration, menus, clamping, extended controls), MMAP streaming with
   poll()/DQBUF: the colour bars and the frame counter strip of every frame, sequence numbers and monotonic timestamps
   at the requested rate, brightness applied mid-stream, NV12, a slow reader (dropped frames, no stall), hostile
   requests (bad indexes, double QBUF, foreign owner, mapped REQBUFS, USERPTR, read()), close while streaming. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/videodev2.h>

static int failures;
#define CHECK(test, ...) do { if (!(test)) { printf("[64] CAMERA FAIL line=%d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fflush(stdout); failures++; } } while (0)
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL); }
static int xioctl(int fd, unsigned long request, void* argument) { int r; do r = ioctl(fd, request, argument); while (r < 0 && errno == EINTR); return r; }

static int list_cameras(char names[8][32]) {
    DIR* dir = opendir("/dev"); int count = 0;
    for (struct dirent* e = dir ? readdir(dir) : NULL; e; e = readdir(dir))
        if (!strncmp(e->d_name, "video", 5) && count < 8) snprintf(names[count++], 32, "/dev/%s", e->d_name);
    if (dir) closedir(dir);
    return count;
}
static const uint8_t BARS[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34}, {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
/* Check a YUYV frame: bar colours at mid-bar on row 10 (brightness offset b), counter strip = sequence. */
static bool check_yuyv(const uint8_t* frame, unsigned w, unsigned h, uint32_t sequence, int b, uint32_t* counter_out) {
    bool ok = true;
    for (unsigned bar = 0; bar < 8; bar++) {
        unsigned x = (bar * w / 8 + w / 16) & ~1U; const uint8_t* p = frame + 10 * w * 2 + x * 2;
        int y = BARS[bar][0] + b; y = y < 16 ? 16 : y > 235 ? 235 : y;
        if (abs(p[0] - y) > 1 || abs(p[1] - BARS[bar][1]) > 1 || abs(p[3] - BARS[bar][2]) > 1) ok = false;
    }
    uint32_t counter = 0; const uint8_t* row = frame + (h - 4) * w * 2;
    for (unsigned i = 0; i < 16; i++) counter = counter << 1 | (row[((i * w / 16 + w / 32) & ~1U) * 2] > 128);
    if (counter_out) *counter_out = counter;
    return ok && counter == (sequence & 0xFFFF);
}
static bool check_nv12(const uint8_t* frame, unsigned w, unsigned h, uint32_t sequence) {
    bool ok = true;
    for (unsigned bar = 0; bar < 8; bar++) {
        unsigned x = (bar * w / 8 + w / 16) & ~1U;
        if (abs(frame[10 * w + x] - BARS[bar][0]) > 1) ok = false;
        const uint8_t* uv = frame + w * h + 5 * w + x;
        if (abs(uv[0] - BARS[bar][1]) > 1 || abs(uv[1] - BARS[bar][2]) > 1) ok = false;
    }
    uint32_t counter = 0;
    for (unsigned i = 0; i < 16; i++) counter = counter << 1 | (frame[(h - 4) * w + i * w / 16 + w / 32] > 128);
    return ok && counter == (sequence & 0xFFFF);
}
struct mapped { void* address; size_t length; };
/* Stream `frames` frames; returns frames whose pattern matched. slow_ms: hold each buffer that long (drops). */
static int stream(int fd, uint32_t fourcc, unsigned w, unsigned h, unsigned fps, int frames, int slow_ms, int brightness_at, double* rate) {
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE}; f.fmt.pix.width = w; f.fmt.pix.height = h; f.fmt.pix.pixelformat = fourcc;
    CHECK(!xioctl(fd, VIDIOC_S_FMT, &f) && f.fmt.pix.width == w && f.fmt.pix.height == h && f.fmt.pix.pixelformat == fourcc, "S_FMT %ux%u", w, h);
    struct v4l2_streamparm p = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE}; p.parm.capture.timeperframe = (struct v4l2_fract){1, fps};
    uint32_t interval = fps == 30 ? 333333 : 666666;
    CHECK(!xioctl(fd, VIDIOC_S_PARM, &p) && (p.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) && p.parm.capture.timeperframe.denominator == 10000000 &&
          p.parm.capture.timeperframe.numerator == interval, "S_PARM %u fps -> %u/%u", fps, p.parm.capture.timeperframe.numerator, p.parm.capture.timeperframe.denominator);
    struct v4l2_requestbuffers r = {.count = 4, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    CHECK(!xioctl(fd, VIDIOC_REQBUFS, &r) && r.count == 4, "REQBUFS 4 -> %u", r.count);
    struct mapped maps[4] = {{0}};
    for (unsigned i = 0; i < r.count && i < 4; i++) {
        struct v4l2_buffer b = {.index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        CHECK(!xioctl(fd, VIDIOC_QUERYBUF, &b) && b.length >= f.fmt.pix.sizeimage, "QUERYBUF %u", i);
        maps[i].length = b.length; maps[i].address = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        CHECK(maps[i].address != MAP_FAILED, "mmap buffer %u", i);
        CHECK(!xioctl(fd, VIDIOC_QBUF, &b), "QBUF %u", i);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE; CHECK(!xioctl(fd, VIDIOC_STREAMON, &type), "STREAMON");
    int good = 0, got = 0; uint32_t last_sequence = 0; double last_time = 0, first = 0, last = 0; bool order = true, times = true;
    while (got < frames) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int ready = poll(&pfd, 1, 3000);
        CHECK(ready == 1 && (pfd.revents & POLLIN), "poll for frame %d: %d revents %x", got, ready, pfd.revents);
        if (ready != 1) break;
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(fd, VIDIOC_DQBUF, &b)) { CHECK(0, "DQBUF"); break; }
        double t = b.timestamp.tv_sec + b.timestamp.tv_usec / 1e6;
        if (got && (b.sequence <= last_sequence)) order = false;
        if (got && t <= last_time) times = false;
        if (!got) first = t;
        last = t; last_sequence = b.sequence; last_time = t; got++;
        CHECK(b.bytesused == f.fmt.pix.sizeimage && (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) && !(b.flags & V4L2_BUF_FLAG_ERROR), "buffer flags/size");
        if (brightness_at >= 0 && got == brightness_at) { struct v4l2_control c = {V4L2_CID_BRIGHTNESS, 160}; CHECK(!xioctl(fd, VIDIOC_S_CTRL, &c), "brightness 160"); }
        int b_offset = brightness_at >= 0 && got > brightness_at + 4 ? 32 : 0;   /* up to 4 frames were already made */
        bool ok = fourcc == V4L2_PIX_FMT_YUYV ? check_yuyv(maps[b.index].address, w, h, b.sequence, b_offset, NULL) : check_nv12(maps[b.index].address, w, h, b.sequence);
        if (ok) good++;
        else if (brightness_at < 0 || got < brightness_at || got > brightness_at + 4) { uint32_t counter = 0; if (fourcc == V4L2_PIX_FMT_YUYV) check_yuyv(maps[b.index].address, w, h, b.sequence, b_offset, &counter); printf("[64] CAMERA frame %d seq %u pattern mismatch (counter %u)\n", got, b.sequence, counter); }
        if (slow_ms) sleep_ms(slow_ms);
        CHECK(!xioctl(fd, VIDIOC_QBUF, &b), "requeue");
    }
    CHECK(order && times, "sequence numbers and timestamps increase");
    if (rate) *rate = got > 1 && last > first ? (got - 1) / (last - first) : 0;
    printf("[64] CAMERA stream %c%c%c%c %ux%u@%u: frames=%d good=%d last_seq=%u rate=%.2f\n", fourcc & 255, fourcc >> 8 & 255, fourcc >> 16 & 255, fourcc >> 24,
           w, h, fps, got, good, last_sequence, rate ? *rate : 0); fflush(stdout);
    CHECK(!xioctl(fd, VIDIOC_STREAMOFF, &type), "STREAMOFF");
    struct v4l2_requestbuffers zero = {.count = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    CHECK(xioctl(fd, VIDIOC_REQBUFS, &zero) && errno == EBUSY, "REQBUFS 0 while mapped must be EBUSY");
    for (unsigned i = 0; i < 4; i++) if (maps[i].address && maps[i].address != MAP_FAILED) munmap(maps[i].address, maps[i].length);
    CHECK(!xioctl(fd, VIDIOC_REQBUFS, &zero), "REQBUFS 0 after munmap");
    return good;
}
static void campaign(void) {
    char names[8][32]; int count = list_cameras(names);
    CHECK(count == 1 && !strcmp(names[0], "/dev/video0"), "one camera /dev/video0 (found %d)", count);
    struct stat st; CHECK(!stat("/dev/video0", &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 81 && minor(st.st_rdev) == 0, "char 81:0");
    int fd = open("/dev/video0", O_RDWR | O_NONBLOCK | O_CLOEXEC); CHECK(fd >= 0, "open");
    if (fd < 0) return;
    struct v4l2_capability cap; CHECK(!xioctl(fd, VIDIOC_QUERYCAP, &cap), "QUERYCAP");
    printf("[64] CAMERA card=%s driver=%s bus=%s caps=%#x device_caps=%#x\n", cap.card, cap.driver, cap.bus_info, cap.capabilities, cap.device_caps);
    CHECK((cap.capabilities & V4L2_CAP_DEVICE_CAPS) && (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) && (cap.device_caps & V4L2_CAP_STREAMING) &&
          !(cap.device_caps & V4L2_CAP_VIDEO_OUTPUT) && !strcmp((char*)cap.driver, "vcam"), "capabilities");
    uint32_t formats[8]; int nformats = 0;
    for (struct v4l2_fmtdesc d = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE}; !xioctl(fd, VIDIOC_ENUM_FMT, &d) && nformats < 8; d.index++) formats[nformats++] = d.pixelformat;
    CHECK(nformats == 2 && formats[0] == V4L2_PIX_FMT_YUYV && formats[1] == V4L2_PIX_FMT_NV12, "formats YUYV, NV12 (%d)", nformats);
    int sizes = 0, intervals = 0;
    for (struct v4l2_frmsizeenum s = {.pixel_format = V4L2_PIX_FMT_YUYV}; !xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &s); s.index++) {
        sizes++; CHECK(s.type == V4L2_FRMSIZE_TYPE_DISCRETE, "discrete sizes");
        for (struct v4l2_frmivalenum i = {.pixel_format = V4L2_PIX_FMT_YUYV, .width = s.discrete.width, .height = s.discrete.height}; !xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &i); i.index++) intervals++;
    }
    CHECK(sizes == 3 && intervals == 6, "3 sizes x 2 intervals (%d, %d)", sizes, intervals);
    struct v4l2_frmsizeenum bad = {.pixel_format = 0x58585858}; CHECK(xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &bad) && errno == EINVAL, "sizes of an unknown format");
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    CHECK(!xioctl(fd, VIDIOC_G_FMT, &f) && f.fmt.pix.width == 640 && f.fmt.pix.height == 480 && f.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV &&
          f.fmt.pix.bytesperline == 1280 && f.fmt.pix.sizeimage == 640 * 480 * 2, "default format");
    f.fmt.pix.pixelformat = 0x58585858; f.fmt.pix.width = 1200; f.fmt.pix.height = 700;
    CHECK(!xioctl(fd, VIDIOC_TRY_FMT, &f) && f.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV && f.fmt.pix.width == 1280 && f.fmt.pix.height == 720, "TRY_FMT nearest");
    struct v4l2_format again = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE}; xioctl(fd, VIDIOC_G_FMT, &again); CHECK(again.fmt.pix.width == 640, "TRY_FMT changes nothing");
    struct v4l2_format wrong = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT}; CHECK(xioctl(fd, VIDIOC_G_FMT, &wrong) && errno == EINVAL, "output type refused");
    /* controls */
    int controls = 0; bool brightness = false, plf = false;
    for (struct v4l2_queryctrl q = {.id = V4L2_CTRL_FLAG_NEXT_CTRL}; !xioctl(fd, VIDIOC_QUERYCTRL, &q); q.id |= V4L2_CTRL_FLAG_NEXT_CTRL) {
        controls++; brightness |= q.id == V4L2_CID_BRIGHTNESS && q.minimum == 0 && q.maximum == 255 && q.default_value == 128;
        plf |= q.id == V4L2_CID_POWER_LINE_FREQUENCY && q.type == V4L2_CTRL_TYPE_MENU;
    }
    CHECK(controls == 3 && brightness && plf, "control enumeration (%d)", controls);
    struct v4l2_querymenu m = {.id = V4L2_CID_POWER_LINE_FREQUENCY, .index = 1};
    CHECK(!xioctl(fd, VIDIOC_QUERYMENU, &m) && !strcmp((char*)m.name, "50 Hz"), "menu item");
    m.index = 7; CHECK(xioctl(fd, VIDIOC_QUERYMENU, &m) && errno == EINVAL, "menu index out of range");
    struct v4l2_control c = {V4L2_CID_POWER_LINE_FREQUENCY, 2}; CHECK(!xioctl(fd, VIDIOC_S_CTRL, &c), "60 Hz (Chrome sets this)");
    c.value = 9; CHECK(xioctl(fd, VIDIOC_S_CTRL, &c) && errno == EINVAL, "menu value out of range");
    c = (struct v4l2_control){V4L2_CID_BRIGHTNESS, 999}; CHECK(!xioctl(fd, VIDIOC_S_CTRL, &c) && c.value == 255, "integer clamped");
    c = (struct v4l2_control){V4L2_CID_HUE, 0}; CHECK(xioctl(fd, VIDIOC_G_CTRL, &c) && errno == EINVAL, "unknown control");
    struct v4l2_ext_control ext[2] = {{.id = V4L2_CID_BRIGHTNESS, .value = 128}, {.id = V4L2_CID_SATURATION, .value = 100}};
    struct v4l2_ext_controls x = {.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 2, .controls = ext};
    CHECK(!xioctl(fd, VIDIOC_S_EXT_CTRLS, &x), "S_EXT_CTRLS");
    ext[0].value = ext[1].value = -1; CHECK(!xioctl(fd, VIDIOC_G_EXT_CTRLS, &x) && ext[0].value == 128 && ext[1].value == 100, "G_EXT_CTRLS");
    ext[1].id = V4L2_CID_HUE; CHECK(xioctl(fd, VIDIOC_S_EXT_CTRLS, &x) && errno == EINVAL, "S_EXT_CTRLS with an unknown control");
    c = (struct v4l2_control){V4L2_CID_BRIGHTNESS, 0}; xioctl(fd, VIDIOC_G_CTRL, &c); CHECK(c.value == 128, "a failed S_EXT_CTRLS changed nothing (%d)", c.value);
    /* streaming */
    double rate = 0;
    int good = stream(fd, V4L2_PIX_FMT_YUYV, 640, 480, 30, 60, 0, 30, &rate);
    CHECK(good >= 55 && rate > 27 && rate < 33, "YUYV 640x480 at 30 fps: %d good, %.2f fps", good, rate);
    c = (struct v4l2_control){V4L2_CID_BRIGHTNESS, 128}; xioctl(fd, VIDIOC_S_CTRL, &c);
    good = stream(fd, V4L2_PIX_FMT_NV12, 320, 240, 15, 20, 0, -1, &rate);
    CHECK(good == 20 && rate > 13 && rate < 17, "NV12 320x240 at 15 fps: %d good, %.2f fps", good, rate);
    good = stream(fd, V4L2_PIX_FMT_YUYV, 1280, 720, 30, 20, 120, -1, &rate);
    CHECK(good == 20, "slow reader at 720p: every delivered frame intact (%d)", good);
    /* hostile and ownership */
    int other = open("/dev/video0", O_RDWR | O_NONBLOCK); CHECK(other >= 0, "second open");
    struct v4l2_requestbuffers r = {.count = 1000, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    CHECK(!xioctl(fd, VIDIOC_REQBUFS, &r) && r.count == 32, "REQBUFS 1000 clamped to %u", r.count);
    r.count = 0; CHECK(!xioctl(fd, VIDIOC_REQBUFS, &r), "free");
    r = (struct v4l2_requestbuffers){.count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    CHECK(!xioctl(fd, VIDIOC_REQBUFS, &r) && r.count == 2, "REQBUFS 2");
    struct v4l2_requestbuffers theirs = r; CHECK(xioctl(other, VIDIOC_REQBUFS, &theirs) && errno == EBUSY, "another fd owns the queue");
    struct v4l2_buffer b = {.index = 5, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    CHECK(xioctl(fd, VIDIOC_QBUF, &b) && errno == EINVAL, "QBUF index out of range");
    b.index = 0; CHECK(!xioctl(fd, VIDIOC_QBUF, &b), "QBUF 0"); CHECK(xioctl(fd, VIDIOC_QBUF, &b) && errno == EINVAL, "double QBUF");
    CHECK(xioctl(other, VIDIOC_QBUF, &b) && errno == EBUSY, "QBUF from a foreign fd");
    CHECK(xioctl(fd, VIDIOC_DQBUF, &b) && errno == EINVAL, "DQBUF before STREAMON");
    void* bad_map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 7 << 24); CHECK(bad_map == MAP_FAILED, "mmap of a missing buffer");
    bad_map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 4096); CHECK(bad_map == MAP_FAILED, "mmap at an unaligned offset");
    struct v4l2_requestbuffers user = {.count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_USERPTR};
    CHECK(xioctl(fd, VIDIOC_REQBUFS, &user) && errno == EINVAL, "USERPTR refused");
    char byte; CHECK(read(fd, &byte, 1) < 0 && errno == EINVAL, "read() I/O refused");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE; CHECK(!xioctl(fd, VIDIOC_STREAMON, &type), "STREAMON with one buffer");
    struct v4l2_format change = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE}; xioctl(fd, VIDIOC_G_FMT, &change);
    CHECK(xioctl(fd, VIDIOC_S_FMT, &change) && errno == EBUSY, "S_FMT while streaming");
    struct v4l2_buffer d = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    sleep_ms(200); CHECK(!xioctl(fd, VIDIOC_DQBUF, &d), "a frame in the only buffer");
    CHECK(xioctl(fd, VIDIOC_DQBUF, &d) && errno == EAGAIN, "non-blocking DQBUF with nothing queued: EAGAIN");
    close(other);
    close(fd);                                              /* while streaming: everything released */
    fd = open("/dev/video0", O_RDWR); CHECK(fd >= 0, "reopen after a close while streaming");
    r = (struct v4l2_requestbuffers){.count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    CHECK(fd >= 0 && !xioctl(fd, VIDIOC_REQBUFS, &r), "the queue is free again");
    if (fd >= 0) close(fd);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char mode = 0; FILE* config = fopen("/nexus/test-hw", "r");
    if (config) { mode = (char)fgetc(config); fclose(config); }
    printf("[64] CAMERA TEST START mode=%c\n", mode);
    if (mode == 'v') campaign();
    else if (mode == 'n') { char names[8][32]; CHECK(list_cameras(names) == 0, "no camera without vcam"); CHECK(open("/dev/video0", O_RDWR) < 0 && errno == ENOENT, "no node"); }
    else CHECK(0, "unknown mode %c", mode);
    printf(failures ? "[64] CAMERA TEST FAILED failures=%d\n" : "[64] CAMERA TEST PASSED failures=%d\n", failures);
    return failures ? 1 : 0;
}
