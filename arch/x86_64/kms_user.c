/* KMS campaign (/nexus/test-kms, tests/test_x64_kms.py): modesetting on the virtio-gpu scanouts through the
 * Linux DRM uapi on /dev/dri/card0 - resources, connectors and EDID, dumb buffers, SETCRTC on two heads, DIRTYFB
 * damage, page flips with events, hot-plug uevents (the host enables/disables head 1 over VNC SetDesktopSize),
 * master rules, DPMS, universal planes, hostile arguments, the fbdev restore when the master closes, and libdrm
 * (drmModeGetResources/GetConnector) over the same ioctls. The host checks every frame in QEMU screendumps of each
 * head; each "READY" marker waits for one key press from the host before going on. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* libdrm's xf86drmMode.h records (the library is loaded with dlopen when the disk has it) */
typedef struct { int count_fbs; uint32_t* fbs; int count_crtcs; uint32_t* crtcs; int count_connectors; uint32_t* connectors;
                 int count_encoders; uint32_t* encoders; uint32_t min_width, max_width, min_height, max_height; } drmModeRes, *drmModeResPtr;
typedef struct { uint32_t connector_id, encoder_id, connector_type, connector_type_id; int connection; uint32_t mmWidth, mmHeight; int subpixel;
                 int count_modes; struct drm_mode_modeinfo* modes; int count_props; uint32_t* props; uint64_t* prop_values;
                 int count_encoders; uint32_t* encoders; } drmModeConnector, *drmModeConnectorPtr;
static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("[64] KMS FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fflush(stdout); } } while (0)
static void pass(const char* what) { printf("[64] KMS PASS %s\n", what); fflush(stdout); }
static int io(int fd, unsigned long request, void* argument) { int r; do r = ioctl(fd, request, argument); while (r == -1 && errno == EINTR); return r; }
static int fails(int fd, unsigned long request, void* argument) { errno = 0; int r = io(fd, request, argument); return r == -1 ? errno : 0; }

static int keyboard = -1;
static void ready(const char* marker) {
    printf("[64] KMS %s READY\n", marker); fflush(stdout);
    for (;;) {   /* the host answers with one key press after checking the screen */
        struct pollfd p = {keyboard, POLLIN, 0};
        int n = poll(&p, 1, 120000);
        if (n <= 0) { printf("[64] KMS FAIL no host answer after %s\n", marker); fflush(stdout); exit(1); }
        struct input_event e[16]; ssize_t size = read(keyboard, e, sizeof(e));
        for (ssize_t i = 0; i < size / (ssize_t)sizeof(*e); i++) if (e[i].type == EV_KEY && e[i].value == 1) return;
    }
}
struct buffer { uint32_t handle, fb, width, height, pitch; uint64_t size; uint32_t* pixels; };
static bool buffer_make(int fd, struct buffer* b, uint32_t width, uint32_t height, uint32_t color) {
    struct drm_mode_create_dumb create = {.height = height, .width = width, .bpp = 32};
    if (io(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) return false;
    struct drm_mode_map_dumb map = {.handle = create.handle};
    if (io(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) return false;
    void* pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
    if (pixels == MAP_FAILED) return false;
    struct drm_mode_fb_cmd2 add = {.width = width, .height = height, .pixel_format = DRM_FORMAT_XRGB8888, .handles = {create.handle}, .pitches = {create.pitch}};
    if (io(fd, DRM_IOCTL_MODE_ADDFB2, &add)) return false;
    *b = (struct buffer){create.handle, add.fb_id, width, height, create.pitch, create.size, pixels};
    for (uint64_t i = 0; i < (uint64_t)width * height; i++) b->pixels[i] = color;
    return true;
}
static void square(struct buffer* b, uint32_t x, uint32_t y, uint32_t size, uint32_t color) {
    for (uint32_t r = y; r < y + size && r < b->height; r++) for (uint32_t c = x; c < x + size && c < b->width; c++) b->pixels[(uint64_t)r * b->width + c] = color;
}
static void buffer_free(int fd, struct buffer* b) {
    if (b->fb) { uint32_t id = b->fb; CHECK(!io(fd, DRM_IOCTL_MODE_RMFB, &id), "RMFB"); }
    if (b->pixels) munmap(b->pixels, b->size);
    if (b->handle) { struct drm_mode_destroy_dumb d = {b->handle}; CHECK(!io(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d), "DESTROY_DUMB"); }
    memset(b, 0, sizeof(*b));
}
struct connector { uint32_t id, status, modes, encoder, mm_w, mm_h; struct drm_mode_modeinfo mode[64]; uint32_t props[8]; uint64_t values[8]; uint32_t nprops; };
static bool connector_get(int fd, uint32_t id, struct connector* c, bool probe) {
    struct drm_mode_get_connector g = {.connector_id = id};
    if (!probe) g.count_modes = 1;   /* a non-zero count skips the forced probe */
    struct drm_mode_modeinfo one;
    if (!probe) g.modes_ptr = (uint64_t)(uintptr_t)&one;
    if (io(fd, DRM_IOCTL_MODE_GETCONNECTOR, &g)) return false;
    uint32_t count = g.count_modes; if (count > 64) return false;
    memset(c, 0, sizeof(*c));
    struct drm_mode_get_connector again = {.connector_id = id, .count_modes = count, .modes_ptr = (uint64_t)(uintptr_t)c->mode,
                                           .count_props = 8, .props_ptr = (uint64_t)(uintptr_t)c->props, .prop_values_ptr = (uint64_t)(uintptr_t)c->values};
    if (!count) { again.count_modes = 1; again.modes_ptr = (uint64_t)(uintptr_t)&one; }
    if (io(fd, DRM_IOCTL_MODE_GETCONNECTOR, &again)) return false;
    c->id = id; c->status = again.connection; c->modes = again.count_modes; c->encoder = again.encoder_id; c->mm_w = again.mm_width; c->mm_h = again.mm_height;
    c->nprops = again.count_props < 8 ? again.count_props : 8;
    return again.count_modes == count;
}
static int set_crtc(int fd, uint32_t crtc, uint32_t fb, uint32_t connector, const struct drm_mode_modeinfo* mode) {
    struct drm_mode_crtc s = {.crtc_id = crtc, .fb_id = fb};
    if (mode) { s.mode = *mode; s.mode_valid = 1; s.count_connectors = 1; s.set_connectors_ptr = (uint64_t)(uintptr_t)&connector; }
    return io(fd, DRM_IOCTL_MODE_SETCRTC, &s) ? errno : 0;
}
static bool wait_uevent(int sock, const char* what) {
    for (int tries = 0; tries < 8; tries++) {
        struct pollfd p = {sock, POLLIN, 0};
        if (poll(&p, 1, 30000) <= 0) break;
        char message[2048]; ssize_t n = recv(sock, message, sizeof(message) - 1, 0);
        if (n <= 0) break;
        message[n] = 0;
        bool hotplug = false, drm = false, change = !strncmp(message, "change@", 7) && strstr(message, "/drm/card0");
        for (ssize_t at = 0; at < n; at += (ssize_t)strlen(message + at) + 1) {
            if (!strcmp(message + at, "HOTPLUG=1")) hotplug = true;
            if (!strcmp(message + at, "SUBSYSTEM=drm")) drm = true;
        }
        printf("[64] KMS uevent %s (%s) hotplug=%d\n", message, what, hotplug); fflush(stdout);
        if (change && hotplug && drm) return true;
    }
    return false;
}
static uint32_t property_id(int fd, const char* name) {
    for (uint32_t id = 1; id < 16; id++) {
        struct drm_mode_get_property p = {.prop_id = id};
        if (!io(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) && !strcmp(p.name, name)) return id;
    }
    return 0;
}

int main(void) {
    keyboard = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
    CHECK(keyboard >= 0, "keyboard for host answers");
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0, "open card0"); if (fd < 0) return 1;
    char name[32] = {0}; struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
    CHECK(!io(fd, DRM_IOCTL_VERSION, &version) && !strcmp(name, "virtio_gpu"), "driver name %s", name);
    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    CHECK(!io(fd, DRM_IOCTL_GET_CAP, &cap) && cap.value == 1, "DUMB_BUFFER cap");
    cap = (struct drm_get_cap){.capability = DRM_CAP_CRTC_IN_VBLANK_EVENT};
    CHECK(!io(fd, DRM_IOCTL_GET_CAP, &cap) && cap.value == 1, "CRTC_IN_VBLANK_EVENT cap");
    struct drm_mode_card_res res = {0};
    CHECK(!io(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) && res.count_crtcs >= 1 && res.count_crtcs == res.count_connectors && res.count_crtcs == res.count_encoders, "GETRESOURCES");
    uint32_t heads = res.count_crtcs > 16 ? 16 : res.count_crtcs, crtcs[16], connectors[16], encoders[16];
    res = (struct drm_mode_card_res){.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs, .connector_id_ptr = (uint64_t)(uintptr_t)connectors,
                                     .encoder_id_ptr = (uint64_t)(uintptr_t)encoders, .count_crtcs = heads, .count_connectors = heads, .count_encoders = heads};
    CHECK(!io(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) && res.max_width >= 4096 && res.count_fbs == 0, "GETRESOURCES ids");
    printf("[64] KMS heads=%u\n", heads); fflush(stdout);
    struct connector c0, c1;
    CHECK(connector_get(fd, connectors[0], &c0, true) && c0.status == 1 && c0.modes >= 2, "connector 0 connected with modes (%u)", c0.modes);
    CHECK(c0.mode[0].type & DRM_MODE_TYPE_PREFERRED, "first mode preferred");
    for (uint32_t i = 0; i < c0.modes && i < 64; i++) printf("[64] KMS mode %ux%u@%u%s\n", c0.mode[i].hdisplay, c0.mode[i].vdisplay, c0.mode[i].vrefresh,
                                                             c0.mode[i].type & DRM_MODE_TYPE_PREFERRED ? " preferred" : "");
    printf("[64] KMS CONNECTOR 0 status=%u modes=%u preferred=%ux%u mm=%ux%u\n", c0.status, c0.modes, c0.mode[0].hdisplay, c0.mode[0].vdisplay, c0.mm_w, c0.mm_h);
    bool has1080 = false; for (uint32_t i = 0; i < c0.modes; i++) if (c0.mode[i].hdisplay == 1920 && c0.mode[i].vdisplay == 1080) has1080 = true;
    CHECK(has1080, "EDID standard timing 1920x1080 listed");
    struct drm_mode_get_encoder encoder = {.encoder_id = encoders[0]};
    CHECK(!io(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) && encoder.possible_crtcs == 1 && encoder.encoder_type == DRM_MODE_ENCODER_VIRTUAL, "encoder 0");
    /* EDID blob */
    uint32_t edid_prop = property_id(fd, "EDID"), dpms_prop = property_id(fd, "DPMS");
    CHECK(edid_prop && dpms_prop, "EDID and DPMS properties");
    uint64_t edid_blob = 0; for (uint32_t i = 0; i < c0.nprops; i++) if (c0.props[i] == edid_prop) edid_blob = c0.values[i];
    struct drm_mode_get_blob blob = {.blob_id = (uint32_t)edid_blob};
    CHECK(edid_blob && !io(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) && blob.length >= 128, "EDID blob length %u", blob.length);
    uint8_t edid[1024] = {0}; blob.data = (uint64_t)(uintptr_t)edid;
    CHECK(blob.length <= sizeof(edid) && !io(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) && edid[0] == 0 && edid[1] == 0xFF && edid[7] == 0, "EDID header");
    bool monitor = false; for (unsigned i = 54; i + 18 <= 126; i += 18) if (!edid[i] && !edid[i + 1] && edid[i + 3] == 0xFC && !memcmp(edid + i + 5, "QEMU Monitor", 12)) monitor = true;
    CHECK(monitor, "EDID monitor name");
    /* the boot framebuffer on CRTC 0 (virtio-gpu-pci without VGA, marker "pci": no firmware framebuffer, CRTC 0 empty) */
    char marker[8] = {0}; FILE* mf = fopen("/nexus/test-kms", "re"); if (mf) { if (!fread(marker, 1, sizeof(marker) - 1, mf)) marker[0] = 0; fclose(mf); }
    bool kms_only = !strncmp(marker, "pci", 3);
    struct drm_mode_crtc crtc = {.crtc_id = crtcs[0]};
    if (kms_only) CHECK(!io(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) && !crtc.fb_id, "CRTC 0 starts without a framebuffer (no firmware framebuffer)");
    else CHECK(!io(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) && crtc.fb_id && crtc.mode_valid, "CRTC 0 shows the boot framebuffer");
    uint32_t boot_fb = crtc.fb_id; struct drm_mode_modeinfo boot_mode = crtc.mode;
    struct drm_mode_fb_cmd getfb = {.fb_id = kms_only ? 128 : boot_fb};
    if (kms_only) CHECK(fails(fd, DRM_IOCTL_MODE_GETFB, &getfb) == ENOENT, "no boot framebuffer object");
    else CHECK(!io(fd, DRM_IOCTL_MODE_GETFB, &getfb) && getfb.width == boot_mode.hdisplay && getfb.handle == 0 && getfb.pitch == getfb.width * 4, "GETFB boot framebuffer");
    uint32_t id = kms_only ? 128 : boot_fb; CHECK(fails(fd, DRM_IOCTL_MODE_RMFB, &id) == ENOENT, "the boot framebuffer cannot be removed");
    pass("resources, connectors, EDID and the boot framebuffer");

    /* head 0: a dumb buffer in the preferred mode */
    const struct drm_mode_modeinfo* m0 = &c0.mode[0];
    struct buffer a = {0}, b = {0}, small = {0}, h1 = {0};
    CHECK(buffer_make(fd, &a, m0->hdisplay, m0->vdisplay, 0x00C81E3C), "dumb A");
    for (uint32_t r = 0; r < a.height; r++) for (uint32_t x = a.width / 2; x < a.width; x++) a.pixels[(uint64_t)r * a.width + x] = 0x002878DC;
    square(&a, 100, 100, 64, 0x00FFFFFF);
    CHECK(!set_crtc(fd, crtcs[0], a.fb, connectors[0], m0), "SETCRTC head 0");
    crtc = (struct drm_mode_crtc){.crtc_id = crtcs[0]};
    CHECK(!io(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) && crtc.fb_id == a.fb && crtc.mode.hdisplay == m0->hdisplay, "GETCRTC after SETCRTC");
    printf("[64] KMS FRAME head=0 w=%u h=%u\n", a.width, a.height); ready("FRAME0");
    /* damage only: the square turns green; a pixel block outside the clip changes in memory only */
    square(&a, 100, 100, 64, 0x0000FF00); square(&a, 600, 300, 40, 0x00000000);
    struct drm_clip_rect clip = {100, 100, 164, 164};
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = a.fb, .num_clips = 1, .clips_ptr = (uint64_t)(uintptr_t)&clip};
    CHECK(!io(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty), "DIRTYFB one clip");
    ready("DIRTY");
    {   /* presentation cost: 200 one-clip DIRTYFB calls (a compositor's per-frame damage) */
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < 200; i++) CHECK(!io(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty), "DIRTYFB timing");
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long us = (long)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
        printf("[64] KMS DIRTYFB average %ld us\n", us / 200); fflush(stdout);
    }
    struct drm_clip_rect full = {600, 300, 640, 340}; dirty.clips_ptr = (uint64_t)(uintptr_t)&full;
    CHECK(!io(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty), "DIRTYFB second clip");
    ready("DIRTY2");
    /* page flip with an event */
    CHECK(buffer_make(fd, &b, m0->hdisplay, m0->vdisplay, 0x00F0B428), "dumb B");
    square(&b, 300, 200, 50, 0x00102030);
    struct drm_mode_crtc_page_flip flip = {.crtc_id = crtcs[0], .fb_id = b.fb, .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = 0x1234ABCD};
    CHECK(!io(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip), "PAGE_FLIP");
    struct pollfd p = {fd, POLLIN, 0};
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "flip event readable");
    char tiny[8]; CHECK(read(fd, tiny, sizeof(tiny)) == 0, "an event never splits into a short buffer");
    struct drm_event_vblank event = {0};
    CHECK(read(fd, &event, sizeof(event)) == sizeof(event) && event.base.type == DRM_EVENT_FLIP_COMPLETE && event.user_data == 0x1234ABCD && event.crtc_id == crtcs[0], "flip event");
    int flags = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    CHECK(read(fd, &event, sizeof(event)) == -1 && errno == EAGAIN, "no event: EAGAIN"); fcntl(fd, F_SETFL, flags);
    flip.flags = DRM_MODE_PAGE_FLIP_ASYNC; CHECK(fails(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == EINVAL, "async flips are refused");
    ready("FLIP");
    pass("SETCRTC, DIRTYFB damage and page flips on head 0");

    /* hostile arguments */
    CHECK(set_crtc(fd, 999, b.fb, connectors[0], m0) == ENOENT, "unknown CRTC");
    CHECK(buffer_make(fd, &small, 64, 64, 0), "small dumb");
    CHECK(set_crtc(fd, crtcs[0], small.fb, connectors[0], m0) == ENOSPC, "framebuffer smaller than the mode");
    struct drm_mode_modeinfo bad = *m0; bad.hsync_start = bad.hdisplay - 1;
    CHECK(set_crtc(fd, crtcs[0], b.fb, connectors[0], &bad) == EINVAL, "inconsistent mode timings");
    struct drm_mode_crtc noconn = {.crtc_id = crtcs[0], .fb_id = b.fb, .mode = *m0, .mode_valid = 1};
    CHECK(fails(fd, DRM_IOCTL_MODE_SETCRTC, &noconn) == EINVAL, "mode without connectors");
    noconn.count_connectors = 1; noconn.set_connectors_ptr = 16;
    CHECK(fails(fd, DRM_IOCTL_MODE_SETCRTC, &noconn) == EFAULT, "connector list at a bad address");
    if (heads > 1) CHECK(set_crtc(fd, crtcs[0], b.fb, connectors[1], m0) == EINVAL, "connector of another head");
    struct drm_mode_fb_cmd2 add = {.width = 64, .height = 64, .pixel_format = DRM_FORMAT_XRGB8888, .handles = {small.handle}, .pitches = {300}};
    CHECK(fails(fd, DRM_IOCTL_MODE_ADDFB2, &add) == EINVAL, "pitch not width * 4");
    add.pitches[0] = 256; add.pixel_format = 0x20203852;   /* 'R8  ' */
    CHECK(fails(fd, DRM_IOCTL_MODE_ADDFB2, &add) == EINVAL, "unsupported format");
    add.pixel_format = DRM_FORMAT_XRGB8888; add.handles[0] = 0x7777;
    CHECK(fails(fd, DRM_IOCTL_MODE_ADDFB2, &add) == ENOENT, "unknown buffer handle");
    add.handles[0] = small.handle; add.height = 65;
    CHECK(fails(fd, DRM_IOCTL_MODE_ADDFB2, &add) == EINVAL, "framebuffer larger than its buffer");
    struct drm_mode_create_dumb huge = {.width = 8192, .height = 8192, .bpp = 32};
    CHECK(fails(fd, DRM_IOCTL_MODE_CREATE_DUMB, &huge) == ENOMEM, "256 MiB dumb buffer refused");
    struct drm_mode_create_dumb odd = {.width = 64, .height = 64, .bpp = 12};
    CHECK(fails(fd, DRM_IOCTL_MODE_CREATE_DUMB, &odd) == EINVAL, "12 bpp refused");
    odd.bpp = 32; odd.width = 0; CHECK(fails(fd, DRM_IOCTL_MODE_CREATE_DUMB, &odd) == EINVAL, "zero width refused");
    odd.width = 16; odd.flags = 1; CHECK(fails(fd, DRM_IOCTL_MODE_CREATE_DUMB, &odd) == EINVAL, "flags refused");
    struct drm_mode_map_dumb map = {.handle = 0x01000000 + 250}; CHECK(fails(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == ENOENT, "MAP_DUMB of a free handle");
    struct drm_mode_fb_dirty_cmd many = {.fb_id = b.fb, .num_clips = 1000, .clips_ptr = (uint64_t)(uintptr_t)&clip};
    CHECK(fails(fd, DRM_IOCTL_MODE_DIRTYFB, &many) == EINVAL, "too many clips");
    many.num_clips = 2; many.clips_ptr = 8; CHECK(fails(fd, DRM_IOCTL_MODE_DIRTYFB, &many) == EFAULT, "clips at a bad address");
    struct drm_mode_get_blob noblob = {.blob_id = 77777}; CHECK(fails(fd, DRM_IOCTL_MODE_GETPROPBLOB, &noblob) == ENOENT, "unknown blob");
    struct drm_mode_get_connector noconnector = {.connector_id = 4242}; CHECK(fails(fd, DRM_IOCTL_MODE_GETCONNECTOR, &noconnector) == ENOENT, "unknown connector");
    struct drm_mode_get_encoder noencoder = {.encoder_id = 4242}; CHECK(fails(fd, DRM_IOCTL_MODE_GETENCODER, &noencoder) == ENOENT, "unknown encoder");
    struct drm_mode_obj_get_properties noobj = {.obj_id = 4242, .obj_type = DRM_MODE_OBJECT_CONNECTOR}; CHECK(fails(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &noobj) == ENOENT, "unknown object");
    struct drm_mode_card_res badres = {.count_crtcs = 4, .crtc_id_ptr = 16}; CHECK(fails(fd, DRM_IOCTL_MODE_GETRESOURCES, &badres) == EFAULT, "resource ids at a bad address");
    struct drm_set_client_cap atomic = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1}; CHECK(fails(fd, DRM_IOCTL_SET_CLIENT_CAP, &atomic) == EOPNOTSUPP, "no atomic modesetting");
    struct drm_mode_cursor cursor = {.flags = DRM_MODE_CURSOR_BO, .crtc_id = crtcs[0]}; CHECK(fails(fd, DRM_IOCTL_MODE_CURSOR, &cursor) == ENXIO, "no cursor plane");
    union drm_wait_vblank vblank = {.request = {.type = _DRM_VBLANK_RELATIVE, .sequence = 1}}; CHECK(fails(fd, DRM_IOCTL_WAIT_VBLANK, &vblank) == EOPNOTSUPP, "no vblank waits");
    struct drm_gem_close close_bad = {.handle = 0x01000000 + 251}; CHECK(fails(fd, DRM_IOCTL_GEM_CLOSE, &close_bad) == EINVAL, "GEM_CLOSE of a free dumb handle");
    pass("hostile KMS arguments");

    /* universal planes */
    struct drm_set_client_cap universal = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
    CHECK(!io(fd, DRM_IOCTL_SET_CLIENT_CAP, &universal), "UNIVERSAL_PLANES");
    uint32_t planes[16] = {0}; struct drm_mode_get_plane_res plane_res = {.plane_id_ptr = (uint64_t)(uintptr_t)planes, .count_planes = 16};
    CHECK(!io(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) && plane_res.count_planes == heads, "one primary plane per CRTC");
    uint32_t formats[8] = {0}; struct drm_mode_get_plane plane = {.plane_id = planes[0], .count_format_types = 8, .format_type_ptr = (uint64_t)(uintptr_t)formats};
    CHECK(!io(fd, DRM_IOCTL_MODE_GETPLANE, &plane) && plane.crtc_id == crtcs[0] && plane.fb_id == b.fb && formats[0] == DRM_FORMAT_XRGB8888, "GETPLANE");
    uint32_t pprops[4]; uint64_t pvalues[4];
    struct drm_mode_obj_get_properties op = {.props_ptr = (uint64_t)(uintptr_t)pprops, .prop_values_ptr = (uint64_t)(uintptr_t)pvalues, .count_props = 4, .obj_id = planes[0], .obj_type = DRM_MODE_OBJECT_PLANE};
    CHECK(!io(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) && op.count_props == 1 && pprops[0] == property_id(fd, "type") && pvalues[0] == 1, "plane type Primary");
    struct drm_mode_set_plane sp = {.plane_id = planes[0], .crtc_id = crtcs[0], .fb_id = a.fb, .crtc_w = m0->hdisplay, .crtc_h = m0->vdisplay,
                                    .src_w = (uint32_t)m0->hdisplay << 16, .src_h = (uint32_t)m0->vdisplay << 16};
    CHECK(!io(fd, DRM_IOCTL_MODE_SETPLANE, &sp), "SETPLANE full primary");
    sp.crtc_w = 100; CHECK(fails(fd, DRM_IOCTL_MODE_SETPLANE, &sp) == EINVAL, "partial primary plane refused");
    universal.value = 0; io(fd, DRM_IOCTL_SET_CLIENT_CAP, &universal);
    plane_res = (struct drm_mode_get_plane_res){0}; CHECK(!io(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_res) && plane_res.count_planes == 0, "no planes without the cap");
    pass("universal planes");

    /* a second file: not master */
    int other = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(other >= 0, "second open");
    CHECK(set_crtc(other, crtcs[0], a.fb, connectors[0], m0) == EACCES, "SETCRTC needs the master");
    struct drm_mode_fb_dirty_cmd odirty = {.fb_id = a.fb}; CHECK(fails(other, DRM_IOCTL_MODE_DIRTYFB, &odirty) == EACCES, "DIRTYFB needs the master");
    id = a.fb; CHECK(fails(other, DRM_IOCTL_MODE_RMFB, &id) == ENOENT, "another file's framebuffer cannot be removed");
    struct drm_mode_map_dumb omap = {.handle = a.handle}; CHECK(fails(other, DRM_IOCTL_MODE_MAP_DUMB, &omap) == ENOENT, "another file's handle is not visible");
    CHECK(fails(other, DRM_IOCTL_SET_MASTER, NULL) == EBUSY, "SET_MASTER while another file is master");
    CHECK(!io(fd, DRM_IOCTL_DROP_MASTER, NULL), "DROP_MASTER");
    CHECK(set_crtc(fd, crtcs[0], a.fb, connectors[0], m0) == EACCES, "no modesetting after DROP_MASTER");
    bool root = geteuid() == 0;   /* a file that never was master needs CAP_SYS_ADMIN (Linux drm_master_check_perm) */
    CHECK(fails(other, DRM_IOCTL_SET_MASTER, NULL) == (root ? 0 : EPERM), "SET_MASTER of a file that never was master (root=%d)", root);
    if (root) CHECK(!io(other, DRM_IOCTL_DROP_MASTER, NULL), "DROP_MASTER of the second file");
    CHECK(!io(fd, DRM_IOCTL_SET_MASTER, NULL), "the previous master takes it back");
    close(other);
    pass("master rules");

    /* DPMS */
    struct drm_mode_connector_set_property dpms = {.value = DRM_MODE_DPMS_OFF, .prop_id = dpms_prop, .connector_id = connectors[0]};
    CHECK(!io(fd, DRM_IOCTL_MODE_SETPROPERTY, &dpms), "DPMS off");
    dpms.value = DRM_MODE_DPMS_ON; CHECK(!io(fd, DRM_IOCTL_MODE_SETPROPERTY, &dpms), "DPMS on");
    dpms.value = 9; CHECK(fails(fd, DRM_IOCTL_MODE_SETPROPERTY, &dpms) == EINVAL, "bad DPMS value");
    dpms.prop_id = edid_prop; dpms.value = 0; CHECK(fails(fd, DRM_IOCTL_MODE_SETPROPERTY, &dpms) == EINVAL, "EDID is immutable");

    /* hot-plug of head 1 */
    int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    CHECK(sock >= 0 && !bind(sock, (struct sockaddr*)&address, sizeof(address)), "uevent socket");
    if (heads > 1) {
        CHECK(connector_get(fd, connectors[1], &c1, true) && c1.status == 2 && c1.modes == 0, "head 1 disconnected at boot (status %u)", c1.status);
        ready("HOTPLUG");
        CHECK(wait_uevent(sock, "plug"), "hot-plug uevent");
        CHECK(connector_get(fd, connectors[1], &c1, true) && c1.status == 1 && c1.modes >= 1, "head 1 connected");
        printf("[64] KMS CONNECTOR 1 status=%u modes=%u preferred=%ux%u\n", c1.status, c1.modes, c1.mode[0].hdisplay, c1.mode[0].vdisplay);
        CHECK(c1.mode[0].hdisplay == 1024 && c1.mode[0].vdisplay == 768, "head 1 prefers the host size");
        CHECK(buffer_make(fd, &h1, c1.mode[0].hdisplay, c1.mode[0].vdisplay, 0x0050C878), "dumb head 1");
        square(&h1, 200, 200, 80, 0x00FFFFFF);
        CHECK(!set_crtc(fd, crtcs[1], h1.fb, connectors[1], &c1.mode[0]), "SETCRTC head 1");
        printf("[64] KMS FRAME head=1 w=%u h=%u\n", h1.width, h1.height); ready("FRAME1");
        /* both heads mirror one framebuffer region */
        CHECK(!set_crtc(fd, crtcs[1], a.fb, connectors[1], &c1.mode[0]), "SETCRTC head 1 on head 0's framebuffer");
        square(&a, 10, 10, 30, 0x00FF00FF);
        struct drm_clip_rect corner = {10, 10, 40, 40}; struct drm_mode_fb_dirty_cmd d2 = {.fb_id = a.fb, .num_clips = 1, .clips_ptr = (uint64_t)(uintptr_t)&corner};
        CHECK(!io(fd, DRM_IOCTL_MODE_DIRTYFB, &d2), "DIRTYFB of a mirrored framebuffer");
        ready("MIRROR");
        ready("UNPLUG");
        CHECK(wait_uevent(sock, "unplug"), "unplug uevent");
        CHECK(connector_get(fd, connectors[1], &c1, true) && c1.status == 2 && c1.modes == 0, "head 1 disconnected again");
        CHECK(!set_crtc(fd, crtcs[1], 0, 0, NULL), "disable CRTC 1");
        crtc = (struct drm_mode_crtc){.crtc_id = crtcs[1]};
        CHECK(!io(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) && !crtc.mode_valid && !crtc.fb_id, "CRTC 1 off");
        pass("hot-plug of head 1 with uevents, two heads, mirroring");
    }
    /* RMFB of the shown framebuffer switches the CRTC off */
    CHECK(!set_crtc(fd, crtcs[0], b.fb, connectors[0], m0), "SETCRTC B");
    buffer_free(fd, &b);
    crtc = (struct drm_mode_crtc){.crtc_id = crtcs[0]};
    CHECK(!io(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) && !crtc.mode_valid && !crtc.fb_id, "RMFB of the shown framebuffer disables CRTC 0");
    /* a mapping outlives its buffer */
    struct buffer keep = {0}; CHECK(buffer_make(fd, &keep, 128, 128, 0x11223344), "dumb kept mapped");
    uint32_t* kept = keep.pixels; keep.pixels = NULL; buffer_free(fd, &keep);
    kept[5] = 7; CHECK(kept[5] == 7 && kept[6] == 0x11223344, "the mapping keeps the pages"); munmap(kept, 128 * 128 * 4);
    /* libdrm over the same ioctls */
    void* drm = dlopen("libdrm.so.2", RTLD_NOW);
    if (drm) {
        drmModeResPtr (*resources)(int) = (drmModeResPtr (*)(int))dlsym(drm, "drmModeGetResources");
        void (*free_resources)(drmModeResPtr) = (void (*)(drmModeResPtr))dlsym(drm, "drmModeFreeResources");
        drmModeConnectorPtr (*connector)(int, uint32_t) = (drmModeConnectorPtr (*)(int, uint32_t))dlsym(drm, "drmModeGetConnector");
        void (*free_connector)(drmModeConnectorPtr) = (void (*)(drmModeConnectorPtr))dlsym(drm, "drmModeFreeConnector");
        drmModeResPtr r = resources ? resources(fd) : NULL;
        CHECK(r && r->count_crtcs == (int)heads && r->count_connectors == (int)heads, "libdrm drmModeGetResources");
        drmModeConnectorPtr k = r && connector ? connector(fd, r->connectors[0]) : NULL;
        CHECK(k && k->connection == 1 && k->count_modes == (int)c0.modes && k->connector_type == DRM_MODE_CONNECTOR_VIRTUAL, "libdrm drmModeGetConnector");
        if (k) free_connector(k);
        if (r) free_resources(r);
        dlclose(drm);
        pass("libdrm drmModeGetResources/drmModeGetConnector");
    } else printf("[64] KMS libdrm not installed: skipped\n");
    buffer_free(fd, &a); buffer_free(fd, &small); buffer_free(fd, &h1);
    close(fd);
    /* the master went away: the boot framebuffer is back on CRTC 0 */
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    crtc = (struct drm_mode_crtc){.crtc_id = crtcs[0]};
    CHECK(fd >= 0 && !io(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) && crtc.fb_id == boot_fb && (!boot_fb || crtc.mode.hdisplay == boot_mode.hdisplay), "boot framebuffer restored on CRTC 0");
    res = (struct drm_mode_card_res){0}; CHECK(!io(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) && res.count_fbs == 0, "no framebuffers left");
    close(fd); close(sock);
    ready("RESTORE");
    if (failures) { printf("[64] KMS FAILED: %d of %d checks\n", failures, checks); return 1; }
    printf("[64] KMS ALL PASSED: %d checks\n", checks); fflush(stdout); return 0;
}
