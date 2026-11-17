/* DRM KMS (modesetting) for the virtio-gpu scanouts, on /dev/dri/card0 (drm.c routes the ioctls here).
 *
 * Object model as Linux drivers/gpu/drm/virtio: per scanout ("head") one CRTC, one primary plane, one virtual encoder
 * and one Virtual-N connector (DRM_MODE_CONNECTOR_VIRTUAL). A head is "connected" while the host reports it enabled
 * (GET_DISPLAY_INFO); its modes come from the host EDID (GET_EDID: detailed, standard, established and CEA timings)
 * plus a CVT mode for the size the host asks for (the host window), which is the preferred mode and listed first.
 * Probing happens at boot and on DRM_IOCTL_MODE_GETCONNECTOR with count_modes == 0 after a display event.
 *
 * Legacy KMS uapi only (no atomic): GETRESOURCES/CRTC/ENCODER/CONNECTOR/PLANE(RESOURCES), SETCRTC, PAGE_FLIP (the
 * flip completes when the host processed it; the event is queued at once, as Linux virtio-gpu sends it without a
 * fence), SETPLANE on primary planes, DIRTYFB (damage -> TRANSFER_TO_HOST_2D + RESOURCE_FLUSH per clip), dumb buffers
 * (CREATE/MAP/DESTROY_DUMB, GEM_CLOSE), ADDFB/ADDFB2/RMFB/GETFB/GETFB2, properties EDID (blob), DPMS (enum) and plane
 * "type" (enum), master handling (the first card0 opener is master; SET/DROP_MASTER). Modesetting ioctls need the
 * master, as in Linux. Framebuffers need pitch == width * 4 (the host copies with its own stride).
 *
 * The firmware framebuffer of a virtio-vga device is framebuffer object FB_BASE ("boot fb"), shown on CRTC 0 at boot
 * through VGA. A master that sets it on CRTC 0 keeps drawing through /dev/fb0 (the same pages), so fbdev readers keep
 * seeing the screen; the first SETCRTC switches QEMU from VGA to the virtio scanout. When the master goes away the
 * boot fb is restored on CRTC 0 (Linux fbdev "lastclose" restore).
 *
 * Hot-plug: virtio_gpu_poll() reports VIRTIO_GPU_EVENT_DISPLAY; the next probe re-reads the heads and a
 * "change@.../drm/card0 ... HOTPLUG=1" kernel uevent goes to NETLINK_KOBJECT_UEVENT listeners (netlink_uevent).
 *
 * Locking: ioctls run with the big kernel lock; virtio commands may sleep, so the KMS state is also guarded by a
 * sleeping flag lock (kms.locked) held across a whole ioctl. The poll-context entry (kms_display_event) only sets a
 * flag and sends the uevent. */
#include "native.h"
#include "gpu.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define PAGE 4096ULL
#define HEADS 16U
#define FBS 64U
#define DUMBS 256U
#define MODES 32U
#define EVENTS 16U
#define EDID_MAX 1024U
#define MAX_SIZE 8192U
#define DUMB_LIMIT (128ULL << 20)
#define DUMB_TOTAL (768ULL << 20)
#define CLIPS_MAX 256U
enum { E_PERM = -1, E_NOENT = -2, E_INTR = -4, E_IO = -5, E_NXIO = -6, E_BADF = -9, E_AGAIN = -11, E_NOMEM = -12, E_ACCES = -13,
       E_FAULT = -14, E_BUSY = -16, E_INVAL = -22, E_NOSPC = -28, E_NOSYS = -38, E_OPNOTSUPP = -95 };
/* object ids (one id space, as the DRM mode-object IDR) */
enum { PROP_EDID = 1, PROP_DPMS = 2, PROP_TYPE = 3, CRTC_BASE = 16, ENCODER_BASE = 32, CONNECTOR_BASE = 48, PLANE_BASE = 64,
       FB_BASE = 128, BLOB_BASE = 256 };
#define OBJECT_CRTC 0xccccccccU
#define OBJECT_CONNECTOR 0xc0c0c0c0U
#define OBJECT_ENCODER 0xe0e0e0e0U
#define OBJECT_PLANE 0xeeeeeeeeU
#define OBJECT_FB 0xfbfbfbfbU
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FORMAT_XRGB8888 FOURCC('X', 'R', '2', '4')
#define FORMAT_ARGB8888 FOURCC('A', 'R', '2', '4')
#define FORMAT_XBGR8888 FOURCC('X', 'B', '2', '4')
#define FORMAT_ABGR8888 FOURCC('A', 'B', '2', '4')
enum { CMD_GET_DISPLAY_INFO = 0x100, CMD_RESOURCE_CREATE_2D = 0x101, CMD_RESOURCE_UNREF = 0x102, CMD_SET_SCANOUT = 0x103,
       CMD_RESOURCE_FLUSH = 0x104, CMD_TRANSFER_TO_HOST_2D = 0x105, CMD_ATTACH_BACKING = 0x106, CMD_GET_EDID = 0x10A,
       RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO = 0x1101, RESP_OK_EDID = 0x1104 };
enum { FEATURE_EDID = 1 };
enum { MODE_TYPE_PREFERRED = 1 << 3, MODE_TYPE_DRIVER = 1 << 6, MODE_FLAG_PHSYNC = 1, MODE_FLAG_NHSYNC = 2, MODE_FLAG_PVSYNC = 4,
       MODE_FLAG_NVSYNC = 8, MODE_FLAG_INTERLACE = 16 };

struct ctrl_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t context; uint8_t ring, pad[3]; };
struct rect { uint32_t x, y, w, h; };
struct mem_entry { uint64_t address; uint32_t length, pad; };
/* Linux uapi (include/uapi/drm/drm_mode.h, drm.h) */
struct modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan;
                  uint32_t vrefresh, flags, type; char name[32]; };
struct card_res { uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
                  uint32_t count_fbs, count_crtcs, count_connectors, count_encoders, min_width, max_width, min_height, max_height; };
struct mode_crtc { uint64_t set_connectors_ptr; uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct modeinfo mode; };
struct get_encoder { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };
struct get_connector { uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
                       uint32_t count_modes, count_props, count_encoders, encoder_id, connector_id, connector_type, connector_type_id,
                                connection, mm_width, mm_height, subpixel, pad; };
struct get_property { uint64_t values_ptr, enum_blob_ptr; uint32_t prop_id, flags; char name[32]; uint32_t count_values, count_enum_blobs; };
struct property_enum { uint64_t value; char name[32]; };
struct connector_set_property { uint64_t value; uint32_t prop_id, connector_id; };
struct obj_get_properties { uint64_t props_ptr, prop_values_ptr; uint32_t count_props, obj_id, obj_type, pad; };
struct obj_set_property { uint64_t value; uint32_t prop_id, obj_id, obj_type, pad; };
struct get_blob { uint32_t blob_id, length; uint64_t data; };
struct fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct fb_cmd2 { uint32_t fb_id, width, height, pixel_format, flags, handles[4], pitches[4], offsets[4], pad; uint64_t modifier[4]; };
struct page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct dirty_cmd { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; };
struct clip_rect { uint16_t x1, y1, x2, y2; };
struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct plane_res { uint64_t plane_id_ptr; uint32_t count_planes, pad; };
struct get_plane { uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types; uint64_t format_type_ptr; };
struct set_plane { uint32_t plane_id, crtc_id, fb_id, flags; int32_t crtc_x, crtc_y; uint32_t crtc_w, crtc_h, src_x, src_y, src_h, src_w; };
struct event_vblank { uint32_t type, length; uint64_t user_data; uint32_t tv_sec, tv_usec, sequence, crtc_id; };
_Static_assert(sizeof(struct modeinfo) == 68 && sizeof(struct card_res) == 64 && sizeof(struct mode_crtc) == 104 && sizeof(struct get_connector) == 80 &&
               sizeof(struct get_property) == 64 && sizeof(struct property_enum) == 40 && sizeof(struct obj_get_properties) == 32 &&
               sizeof(struct obj_set_property) == 24 && sizeof(struct fb_cmd2) == 104 && sizeof(struct page_flip) == 24 && sizeof(struct dirty_cmd) == 24 &&
               sizeof(struct create_dumb) == 32 && sizeof(struct get_plane) == 32 && sizeof(struct set_plane) == 48 && sizeof(struct event_vblank) == 32 &&
               sizeof(struct plane_res) == 16 && sizeof(struct fb_cmd) == 28, "DRM KMS uapi layout");

struct head {
    bool connected, probed; uint32_t want_w, want_h, mm_w, mm_h;
    struct modeinfo modes[MODES]; unsigned mode_count;
    uint8_t edid[EDID_MAX]; uint32_t edid_size, edid_blob;
    /* CRTC state */
    uint32_t fb;                 /* framebuffer slot + 1, 0 = none */
    bool mode_valid, active;     /* active: the connector drives this CRTC */
    bool virtio_scanout;         /* the host shows our resource (false: VGA or nothing) */
    struct modeinfo mode; uint32_t x, y, dpms, sequence;
    bool lid_off;                /* kms_lid: head 0, the built-in panel, dark while the lid is closed */
};
struct dumb { bool used; uint16_t owner; uint32_t width, height, pitch, bpp, references; bool handle; uint64_t size; struct memory_file* memory; };
struct framebuffer { bool used; uint16_t owner; /* client + 1; 0: the kernel's boot framebuffer */
                     uint32_t width, height, pitch, format, depth, bpp, dumb /* dumb + 1, 0 = boot */, resource; };
struct kclient { bool open, master_before, universal, stereo, aspect; struct event_vblank events[EVENTS]; unsigned head, count; };
static struct {
    bool ready, locked, event_pending, boot_valid; unsigned heads; int master;   /* client + 1 */
    struct head* head; struct dumb* dumbs; struct framebuffer fbs[FBS]; struct kclient* clients;
    uint32_t next_blob; uint64_t dumb_bytes, uevents;
    uint8_t channel;
} kms;

static void log_number(const char* label, uint64_t value) { char text[24]; uint_to_str((uint32_t)value, text); vga_print(label); vga_print(text); }
static int copy_out(uint64_t user, const void* kernel, uint64_t bytes) {
    if (!bytes) return 0;
    if (!user || !accessible(user, bytes, true)) return E_FAULT;
    return copy_to_user((void*)user, kernel, bytes) ? E_FAULT : 0;
}
static int copy_in(void* kernel, uint64_t user, uint64_t bytes) {
    if (!bytes) return 0;
    if (!user || !accessible(user, bytes, false)) return E_FAULT;
    return copy_from_user(kernel, (const void*)user, bytes) ? E_FAULT : 0;
}
static void kms_lock(void) {
    while (kms.locked) { if (!task_active()) panic("KMS lock contention before scheduling"); task_wait_until(&kms.locked, native_clock_ns() + 1000000ULL); }
    kms.locked = true;
}
static void kms_unlock(void) { kms.locked = false; if (task_active()) task_wake_channel(&kms.locked); }
static struct ctrl_hdr header(uint32_t type) { return (struct ctrl_hdr){type, 0, 0, 0, 0, {0}}; }
/* One control command; wait: block for the answer (a response type other than ok_type is EIO). */
static int command(const void* request, uint32_t bytes, const void* data, uint32_t data_bytes, void* response, uint32_t response_bytes,
                   uint32_t ok_type, bool wait) {
    struct gpu_command c = {.command = request, .command_bytes = bytes, .data = data, .data_bytes = data_bytes,
                            .response = response, .response_bytes = response_bytes, .wait = wait};
    int error = virtio_gpu_command(&c);
    if (error) return error == -19 ? E_IO : error;
    if (wait && c.response_type != ok_type) return E_IO;
    return 0;
}

/* ---- modes ---- */
static void mode_name(struct modeinfo* m) {
    char number[12]; m->name[0] = 0;
    uint_to_str(m->hdisplay, number); strcat(m->name, number); strcat(m->name, "x"); uint_to_str(m->vdisplay, number); strcat(m->name, number);
}
/* A CVT reduced-blanking (v1) timing for w x h at refresh Hz. */
static struct modeinfo mode_cvt(unsigned w, unsigned h, unsigned refresh) {
    struct modeinfo m; memset(&m, 0, sizeof(m));
    unsigned vblank = (unsigned)((uint64_t)h * refresh * 460 / 1000000) + 1; if (vblank < 9) vblank = 9;   /* >= 460 us of lines */
    m.hdisplay = (uint16_t)w; m.hsync_start = (uint16_t)(w + 48); m.hsync_end = (uint16_t)(w + 80); m.htotal = (uint16_t)(w + 160);
    m.vdisplay = (uint16_t)h; m.vsync_start = (uint16_t)(h + 3); m.vsync_end = (uint16_t)(h + 9); m.vtotal = (uint16_t)(h + vblank);
    m.vrefresh = refresh; m.clock = (uint32_t)((uint64_t)m.htotal * m.vtotal * refresh / 1000);
    m.flags = MODE_FLAG_PHSYNC | MODE_FLAG_NVSYNC; m.type = MODE_TYPE_DRIVER; mode_name(&m); return m;
}
static bool mode_usable(unsigned w, unsigned h) { return w >= 320 && h >= 200 && w <= MAX_SIZE && h <= MAX_SIZE; }
static void mode_add(struct head* hd, const struct modeinfo* m) {
    if (!mode_usable(m->hdisplay, m->vdisplay) || hd->mode_count >= MODES) return;
    for (unsigned i = 0; i < hd->mode_count; i++)
        if (hd->modes[i].hdisplay == m->hdisplay && hd->modes[i].vdisplay == m->vdisplay && hd->modes[i].vrefresh == m->vrefresh) return;
    hd->modes[hd->mode_count++] = *m;
}
static void mode_add_cvt(struct head* hd, unsigned w, unsigned h, unsigned refresh) { struct modeinfo m = mode_cvt(w, h, refresh); mode_add(hd, &m); }
/* An 18-byte EDID detailed timing descriptor (pixel clock != 0). */
static void mode_dtd(struct head* hd, const uint8_t* d) {
    uint32_t clock = (uint32_t)(d[0] | d[1] << 8) * 10;   /* kHz */
    unsigned ha = d[2] | (d[4] & 0xF0) << 4, hb = d[3] | (d[4] & 0x0F) << 8, va = d[5] | (d[7] & 0xF0) << 4, vb = d[6] | (d[7] & 0x0F) << 8;
    unsigned ho = d[8] | (d[11] & 0xC0) << 2, hw = d[9] | (d[11] & 0x30) << 4, vo = (d[10] >> 4) | (d[11] & 0x0C) << 2, vw = (d[10] & 15) | (d[11] & 3) << 4;
    if (!clock || !ha || !va || !hb || !vb || (d[17] & 0x80)) return;   /* interlaced timings are not offered */
    struct modeinfo m; memset(&m, 0, sizeof(m));
    m.clock = clock; m.hdisplay = (uint16_t)ha; m.hsync_start = (uint16_t)(ha + ho); m.hsync_end = (uint16_t)(ha + ho + hw); m.htotal = (uint16_t)(ha + hb);
    m.vdisplay = (uint16_t)va; m.vsync_start = (uint16_t)(va + vo); m.vsync_end = (uint16_t)(va + vo + vw); m.vtotal = (uint16_t)(va + vb);
    if (m.hsync_end > m.htotal || m.vsync_end > m.vtotal) return;
    m.vrefresh = (uint32_t)(((uint64_t)clock * 1000 + (uint64_t)m.htotal * m.vtotal / 2) / ((uint64_t)m.htotal * m.vtotal));
    if (!m.vrefresh) return;
    unsigned sync = d[17] >> 1 & 15;   /* digital separate sync: bit 2 vsync +, bit 1 hsync + */
    m.flags = (sync & 8) ? ((sync & 2 ? MODE_FLAG_PVSYNC : MODE_FLAG_NVSYNC) | (sync & 1 ? MODE_FLAG_PHSYNC : MODE_FLAG_NHSYNC)) : (MODE_FLAG_NHSYNC | MODE_FLAG_NVSYNC);
    m.type = MODE_TYPE_DRIVER; mode_name(&m); mode_add(hd, &m);
}
static void mode_standard(struct head* hd, uint8_t a, uint8_t b) {
    if ((a == 1 && b == 1) || !a) return;
    unsigned w = ((unsigned)a + 31) * 8, aspect = b >> 6, h = aspect == 0 ? w * 10 / 16 : aspect == 1 ? w * 3 / 4 : aspect == 2 ? w * 4 / 5 : w * 9 / 16;
    mode_add_cvt(hd, w, h, (b & 63) + 60);
}
static void parse_edid(struct head* hd) {
    const uint8_t* e = hd->edid; uint32_t size = hd->edid_size;
    static const uint8_t magic[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (size < 128 || memcmp(e, magic, 8)) return;
    uint8_t sum = 0; for (unsigned i = 0; i < 128; i++) sum = (uint8_t)(sum + e[i]);
    if (sum) return;   /* a corrupt base block is ignored (the host size still gives the preferred mode) */
    if (e[21] && e[22]) { hd->mm_w = e[21] * 10U; hd->mm_h = e[22] * 10U; }
    for (unsigned k = 0; k < 4; k++) {
        const uint8_t* d = e + 54 + k * 18;
        if (d[0] || d[1]) { mode_dtd(hd, d); if (k == 0 && (d[12] | (d[14] & 0xF0) << 4)) { hd->mm_w = d[12] | (d[14] & 0xF0) << 4; hd->mm_h = d[13] | (d[14] & 0x0F) << 8; } continue; }
        if (d[3] == 0xF7) {   /* established timings III */
            static const uint16_t table[6][8][3] = {
                {{1152, 864, 75}, {1024, 768, 85}, {800, 600, 85}, {848, 480, 60}, {640, 480, 85}, {720, 400, 85}, {640, 400, 85}, {640, 350, 85}},
                {{1280, 1024, 85}, {1280, 1024, 60}, {1280, 960, 85}, {1280, 960, 60}, {1280, 768, 85}, {1280, 768, 75}, {1280, 768, 60}, {1280, 768, 60}},
                {{1400, 1050, 75}, {1400, 1050, 60}, {1400, 1050, 60}, {1440, 900, 85}, {1440, 900, 75}, {1440, 900, 60}, {1440, 900, 60}, {1360, 768, 60}},
                {{1600, 1200, 70}, {1600, 1200, 65}, {1600, 1200, 60}, {1680, 1050, 85}, {1680, 1050, 75}, {1680, 1050, 60}, {1680, 1050, 60}, {1400, 1050, 85}},
                {{1920, 1200, 60}, {1920, 1200, 60}, {1856, 1392, 75}, {1856, 1392, 60}, {1792, 1344, 75}, {1792, 1344, 60}, {1600, 1200, 85}, {1600, 1200, 75}},
                {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {1920, 1440, 75}, {1920, 1440, 60}, {1920, 1200, 85}, {1920, 1200, 75}}};
            for (unsigned byte = 0; byte < 6; byte++) for (unsigned bit = 0; bit < 8; bit++)
                if ((d[6 + byte] >> bit & 1) && table[byte][bit][0]) mode_add_cvt(hd, table[byte][bit][0], table[byte][bit][1], table[byte][bit][2]);
        } else if (d[3] == 0xFA) for (unsigned i = 0; i < 6; i++) mode_standard(hd, d[5 + 2 * i], d[6 + 2 * i]);
    }
    for (unsigned i = 0; i < 8; i++) mode_standard(hd, e[38 + 2 * i], e[39 + 2 * i]);
    static const uint16_t established[][4] = {{35, 5, 640, 480}, {35, 0, 800, 600}, {36, 3, 1024, 768}};   /* the 60 Hz ones */
    for (unsigned i = 0; i < 3; i++) if (e[established[i][0]] >> established[i][1] & 1) mode_add_cvt(hd, established[i][2], established[i][3], 60);
    /* CEA-861 extensions: short video descriptors and detailed timings */
    for (unsigned block = 1; block <= e[126] && (block + 1) * 128 <= size; block++) {
        const uint8_t* x = e + block * 128;
        if (x[0] != 2) continue;
        unsigned dtd = x[2]; if (dtd < 4 || dtd > 127) dtd = 4;
        for (unsigned at = 4; at < dtd;) {
            unsigned tag = x[at] >> 5, length = x[at] & 31;
            if (at + 1 + length > dtd) break;
            if (tag == 2) for (unsigned i = 0; i < length; i++) {
                static const uint16_t vic[][4] = {{1, 640, 480, 60}, {4, 1280, 720, 60}, {16, 1920, 1080, 60}, {19, 1280, 720, 50}, {31, 1920, 1080, 50},
                                                  {89, 2560, 1080, 50}, {90, 2560, 1080, 60}, {95, 3840, 2160, 30}, {96, 3840, 2160, 50}, {97, 3840, 2160, 60},
                                                  {101, 4096, 2160, 50}, {102, 4096, 2160, 60}, {125, 5120, 2160, 50}, {126, 5120, 2160, 60}};
                unsigned code = x[at + 1 + i] & 0x7F;
                for (unsigned k = 0; k < sizeof(vic) / sizeof(vic[0]); k++) if (vic[k][0] == code) mode_add_cvt(hd, vic[k][1], vic[k][2], vic[k][3]);
            }
            at += 1 + length;
        }
        for (unsigned at = dtd; at + 18 <= 127; at += 18) if (x[at] || x[at + 1]) mode_dtd(hd, x + at);
    }
}
/* Re-read the heads from the host: connection, the size the host asks for, EDID, modes. */
static void probe(void) {
    struct { struct ctrl_hdr h; struct { struct rect r; uint32_t enabled, flags; } pmodes[16]; } info;
    struct ctrl_hdr request = header(CMD_GET_DISPLAY_INFO);
    memset(&info, 0, sizeof(info));
    kms.event_pending = false;
    if (command(&request, sizeof(request), NULL, 0, &info, sizeof(info), RESP_OK_DISPLAY_INFO, true)) {
        vga_print("[64] KMS: GET_DISPLAY_INFO failed\n"); return;
    }
    for (unsigned i = 0; i < kms.heads; i++) {
        struct head* hd = &kms.head[i];
        bool connected = info.pmodes[i].enabled && info.pmodes[i].r.w && info.pmodes[i].r.h;
        uint32_t w = info.pmodes[i].r.w, h = info.pmodes[i].r.h;
        struct { struct ctrl_hdr h; uint32_t size, pad; uint8_t edid[EDID_MAX]; } *answer = NULL;
        uint32_t edid_size = 0;
        if (connected && virtio_gpu_feature(FEATURE_EDID) && (answer = (void*)arena_alloc(sizeof(*answer)))) {
            struct { struct ctrl_hdr h; uint32_t scanout, pad; } edid = {header(CMD_GET_EDID), i, 0};
            memset(answer, 0, sizeof(*answer));
            if (!command(&edid, sizeof(edid), NULL, 0, answer, sizeof(*answer), RESP_OK_EDID, true)) edid_size = answer->size > EDID_MAX ? EDID_MAX : answer->size;
        }
        bool edid_changed = edid_size != hd->edid_size || (answer && memcmp(answer->edid, hd->edid, edid_size));
        if (connected != hd->connected || w != hd->want_w || h != hd->want_h || edid_changed || !hd->probed) {
            vga_print("[64] KMS head "); log_number("", i); vga_print(connected ? " connected " : " disconnected ");
            log_number("", w); log_number("x", h); log_number(" edid=", edid_size); vga_print("\n");
        }
        hd->connected = connected; hd->want_w = w; hd->want_h = h; hd->probed = true;
        if (edid_changed) {
            if (answer) memcpy(hd->edid, answer->edid, edid_size);
            hd->edid_size = edid_size; hd->edid_blob = edid_size ? kms.next_blob++ : 0;
            if (kms.next_blob >= 0x7FFFFFF0U) kms.next_blob = BLOB_BASE + HEADS;
        }
        if (answer) arena_free((uint64_t)answer, sizeof(*answer));
        hd->mode_count = 0; hd->mm_w = hd->mm_h = 0;
        if (!connected) continue;
        if (mode_usable(w, h)) { struct modeinfo m = mode_cvt(w, h, 60); m.type |= MODE_TYPE_PREFERRED; mode_add(hd, &m); }
        parse_edid(hd);
        if (!hd->edid_size) {   /* no EDID: the common DMT sizes (drm_add_modes_noedid) */
            static const uint16_t common[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1200}, {1920, 1080}, {1680, 1050}, {1600, 900}, {1440, 900},
                                                 {1366, 768}, {1280, 1024}, {1280, 800}, {1280, 720}, {1024, 768}, {800, 600}, {640, 480}};
            for (unsigned k = 0; k < sizeof(common) / sizeof(common[0]); k++) mode_add_cvt(hd, common[k][0], common[k][1], 60);
        }
        if (!hd->mm_w || !hd->mm_h) { hd->mm_w = w * 254 / 960; hd->mm_h = h * 254 / 960; }   /* 96 dpi */
    }
}

/* ---- host resources and scanouts ---- */
static uint32_t virtio_format(uint32_t format) {
    return format == FORMAT_XRGB8888 ? 2 : format == FORMAT_ARGB8888 ? 1 : format == FORMAT_XBGR8888 ? 134 : format == FORMAT_ABGR8888 ? 67 : 0;
}
/* The host resource of framebuffer f with its pages attached as backing (created on first use; kms_resume: again,
 * under the id it had). */
static int fb_resource_create(struct framebuffer* f, uint32_t id) {
    struct { struct ctrl_hdr h; uint32_t resource, format, width, height; } create = {header(CMD_RESOURCE_CREATE_2D), id, virtio_format(f->format), f->width, f->height};
    int error = command(&create, sizeof(create), NULL, 0, NULL, 0, RESP_OK_NODATA, true);
    if (error) return error;
    uint64_t bytes = (uint64_t)f->pitch * f->height, pages = (bytes + PAGE - 1) / PAGE;
    struct mem_entry* entries; uint32_t count = 0; uint64_t table = 0;
    if (!f->dumb) {   /* the boot framebuffer: one physically contiguous range */
        const struct framebuffer_info* boot = framebuffer_get();
        static struct mem_entry single; single = (struct mem_entry){boot->physical, (uint32_t)bytes, 0}; entries = &single; count = 1;
    } else {
        struct dumb* d = &kms.dumbs[f->dumb - 1];
        table = pages * sizeof(struct mem_entry); entries = (void*)arena_alloc(table);
        if (!entries) error = E_NOMEM;
        for (uint64_t page = 0; !error && page < pages; page++) {
            uint64_t frame = memory_file_page(d->memory, page, false);
            if (!frame) { error = E_NOMEM; break; }
            if (count && entries[count - 1].address + entries[count - 1].length == frame && entries[count - 1].length <= 0x7FFFF000U - PAGE)
                entries[count - 1].length += (uint32_t)PAGE;
            else entries[count++] = (struct mem_entry){frame, (uint32_t)PAGE, 0};
        }
        if (!error && count > 16384) error = E_NOMEM;   /* the host maps at most 16384 entries */
    }
    if (!error) {
        struct { struct ctrl_hdr h; uint32_t resource, count; } attach = {header(CMD_ATTACH_BACKING), id, count};
        error = command(&attach, sizeof(attach), entries, count * (uint32_t)sizeof(struct mem_entry), NULL, 0, RESP_OK_NODATA, true);
    }
    if (table) arena_free((uint64_t)entries, table);
    if (error) {
        struct { struct ctrl_hdr h; uint32_t resource, pad; } unref = {header(CMD_RESOURCE_UNREF), id, 0};
        command(&unref, sizeof(unref), NULL, 0, NULL, 0, RESP_OK_NODATA, true);
        return error;
    }
    return 0;
}
static int fb_resource(struct framebuffer* f) {
    if (f->resource) return 0;
    uint32_t id = virtio_gpu_resource_id(); if (!id) return E_NOMEM;
    int error = fb_resource_create(f, id);
    if (error) { virtio_gpu_resource_id_free(id); return error; }
    f->resource = id; return 0;
}
static int transfer_flush(struct framebuffer* f, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool wait) {
    if (!w || !h) return 0;
    struct { struct ctrl_hdr h; struct rect r; uint64_t offset; uint32_t resource, pad; } t = {header(CMD_TRANSFER_TO_HOST_2D), {x, y, w, h},
        (uint64_t)y * f->pitch + (uint64_t)x * 4, f->resource, 0};
    int error = command(&t, sizeof(t), NULL, 0, NULL, 0, RESP_OK_NODATA, false);
    struct { struct ctrl_hdr h; struct rect r; uint32_t resource, pad; } flush = {header(CMD_RESOURCE_FLUSH), {x, y, w, h}, f->resource, 0};
    if (!error) error = command(&flush, sizeof(flush), NULL, 0, NULL, 0, RESP_OK_NODATA, wait);
    return error;
}
static int scanout_off(unsigned i) {
    struct head* hd = &kms.head[i];
    if (!hd->virtio_scanout) return 0;
    struct { struct ctrl_hdr h; struct rect r; uint32_t scanout, resource; } s = {header(CMD_SET_SCANOUT), {0, 0, 0, 0}, i, 0};
    hd->virtio_scanout = false;
    return command(&s, sizeof(s), NULL, 0, NULL, 0, RESP_OK_NODATA, true);
}
/* Show framebuffer slot fb at x,y on head i with mode m (or switch the head off: fb 0). */
static int head_show(unsigned i, uint32_t fb, uint32_t x, uint32_t y, const struct modeinfo* m) {
    struct head* hd = &kms.head[i];
    if (!fb) { int error = scanout_off(i); hd->fb = 0; hd->mode_valid = hd->active = false; return error; }
    struct framebuffer* f = &kms.fbs[fb - 1];
    int error = fb_resource(f); if (error) return error;
    if (hd->dpms == 0 && !hd->lid_off) {
        struct { struct ctrl_hdr h; struct rect r; uint32_t scanout, resource; } s = {header(CMD_SET_SCANOUT), {x, y, m->hdisplay, m->vdisplay}, i, f->resource};
        error = command(&s, sizeof(s), NULL, 0, NULL, 0, RESP_OK_NODATA, true);
        if (!error) error = transfer_flush(f, x, y, m->hdisplay, m->vdisplay, true);
        if (error) return error;
        hd->virtio_scanout = true;
    }
    hd->fb = fb; hd->x = x; hd->y = y; hd->mode = *m; hd->mode_valid = hd->active = true;
    return 0;
}
static void fb_destroy(unsigned slot) {
    struct framebuffer* f = &kms.fbs[slot];
    for (unsigned i = 0; i < kms.heads; i++) if (kms.head[i].fb == slot + 1) head_show(i, 0, 0, 0, NULL);   /* Linux: the CRTC is disabled */
    if (f->resource) {
        struct { struct ctrl_hdr h; uint32_t resource, pad; } unref = {header(CMD_RESOURCE_UNREF), f->resource, 0};
        command(&unref, sizeof(unref), NULL, 0, NULL, 0, RESP_OK_NODATA, true);   /* the host no longer reads the pages */
        virtio_gpu_resource_id_free(f->resource);
    }
    if (f->dumb) {
        struct dumb* d = &kms.dumbs[f->dumb - 1];
        if (!--d->references) { memory_file_release(d->memory); kms.dumb_bytes -= d->size; memset(d, 0, sizeof(*d)); }
    }
    memset(f, 0, sizeof(*f));
}
/* fbdev restore: the boot framebuffer back on CRTC 0 when nothing else is shown there. */
static void boot_restore(void) {
    if (!kms.boot_valid || !kms.heads || kms.head[0].fb) return;
    const struct framebuffer_info* boot = framebuffer_get();
    struct modeinfo m = mode_cvt(boot->width, boot->height, 60);
    if (head_show(0, 1, 0, 0, &m)) vga_print("[64] KMS: boot framebuffer restore failed\n");
}

/* ---- lookups ---- */
static bool master(unsigned client) { return kms.master == (int)client + 1; }
static int crtc_index(uint32_t id) { return id >= CRTC_BASE && id < CRTC_BASE + kms.heads ? (int)(id - CRTC_BASE) : -1; }
static int fb_slot(uint32_t id) { return id >= FB_BASE && id < FB_BASE + FBS && kms.fbs[id - FB_BASE].used ? (int)(id - FB_BASE) : -1; }
static int dumb_of(unsigned client, uint32_t handle) {
    if (handle < KMS_HANDLE_BASE || handle - KMS_HANDLE_BASE >= DUMBS) return -1;
    struct dumb* d = &kms.dumbs[handle - KMS_HANDLE_BASE];
    return d->used && d->handle && d->owner == client + 1 ? (int)(handle - KMS_HANDLE_BASE) : -1;
}
static bool mode_check(const struct modeinfo* m) {
    return m->clock && m->hdisplay && m->vdisplay && m->hdisplay <= MAX_SIZE && m->vdisplay <= MAX_SIZE &&
           m->hdisplay <= m->hsync_start && m->hsync_start <= m->hsync_end && m->hsync_end <= m->htotal &&
           m->vdisplay <= m->vsync_start && m->vsync_start <= m->vsync_end && m->vsync_end <= m->vtotal && !(m->flags & MODE_FLAG_INTERLACE);
}
static void queue_event(unsigned client, unsigned i, uint64_t user_data) {
    struct kclient* k = &kms.clients[client]; uint64_t now = native_clock_ns();
    struct event_vblank e = {2, sizeof(e), user_data, (uint32_t)(now / 1000000000ULL), (uint32_t)(now % 1000000000ULL / 1000), ++kms.head[i].sequence, CRTC_BASE + i};
    k->events[(k->head + k->count++) % EVENTS] = e;
    task_wake_channel(&kms.channel); event_source_changed(&kms.channel, 1);
}

/* ---- ioctls ---- */
static int get_resources(unsigned client, struct card_res* r) {
    uint32_t ids[FBS]; unsigned n = 0;
    for (unsigned s = 0; s < FBS; s++) if (kms.fbs[s].used && kms.fbs[s].owner == client + 1) ids[n++] = FB_BASE + s;
    int error = copy_out(r->fb_id_ptr, ids, (uint64_t)(r->count_fbs < n ? r->count_fbs : n) * 4);
    for (unsigned kind = 0; !error && kind < 3; kind++) {
        uint32_t list[HEADS], base = kind == 0 ? CRTC_BASE : kind == 1 ? CONNECTOR_BASE : ENCODER_BASE;
        for (unsigned i = 0; i < kms.heads; i++) list[i] = base + i;
        uint32_t want = kind == 0 ? r->count_crtcs : kind == 1 ? r->count_connectors : r->count_encoders;
        error = copy_out(kind == 0 ? r->crtc_id_ptr : kind == 1 ? r->connector_id_ptr : r->encoder_id_ptr, list, (uint64_t)(want < kms.heads ? want : kms.heads) * 4);
    }
    if (error) return error;
    r->count_fbs = n; r->count_crtcs = r->count_connectors = r->count_encoders = kms.heads;
    r->min_width = r->min_height = 32; r->max_width = r->max_height = MAX_SIZE; return 0;
}
static int get_connector(struct get_connector* c) {
    if (c->connector_id < CONNECTOR_BASE || c->connector_id >= CONNECTOR_BASE + kms.heads) return E_NOENT;
    unsigned i = c->connector_id - CONNECTOR_BASE; struct head* hd = &kms.head[i];
    if (!c->count_modes && (kms.event_pending || !hd->probed)) probe();   /* a forced probe, as drm_mode_getconnector */
    int error = 0;
    if (c->count_modes >= hd->mode_count && hd->mode_count) error = copy_out(c->modes_ptr, hd->modes, (uint64_t)hd->mode_count * sizeof(struct modeinfo));
    uint32_t props[2] = {PROP_EDID, PROP_DPMS}; uint64_t values[2] = {hd->edid_blob, hd->dpms};
    if (!error && c->count_props >= 2) { error = copy_out(c->props_ptr, props, sizeof(props)); if (!error) error = copy_out(c->prop_values_ptr, values, sizeof(values)); }
    uint32_t encoder = ENCODER_BASE + i;
    if (!error && c->count_encoders >= 1) error = copy_out(c->encoders_ptr, &encoder, 4);
    if (error) return error;
    c->count_modes = hd->mode_count; c->count_props = 2; c->count_encoders = 1;
    c->encoder_id = hd->active ? encoder : 0; c->connector_type = 15; c->connector_type_id = i + 1;   /* DRM_MODE_CONNECTOR_VIRTUAL, Virtual-N */
    c->connection = hd->connected ? 1 : 2; c->mm_width = hd->mm_w; c->mm_height = hd->mm_h; c->subpixel = 1; c->pad = 0;
    return 0;
}
static int set_crtc(unsigned client, struct mode_crtc* c) {
    if (!master(client)) return E_ACCES;
    int i = crtc_index(c->crtc_id); if (i < 0) return E_NOENT;
    if (c->count_connectors > HEADS) return E_INVAL;
    if (!c->mode_valid) {
        if (c->count_connectors) return E_INVAL;
        return head_show((unsigned)i, 0, 0, 0, NULL) ? E_IO : 0;
    }
    if (!c->count_connectors) return E_INVAL;
    uint32_t connectors[HEADS]; int error = copy_in(connectors, c->set_connectors_ptr, (uint64_t)c->count_connectors * 4); if (error) return error;
    for (uint32_t k = 0; k < c->count_connectors; k++) {
        if (connectors[k] < CONNECTOR_BASE || connectors[k] >= CONNECTOR_BASE + kms.heads) return E_NOENT;
        if (connectors[k] != CONNECTOR_BASE + (uint32_t)i) return E_INVAL;   /* encoder i can only drive CRTC i */
    }
    struct head* hd = &kms.head[i];
    uint32_t slot = c->fb_id == 0xFFFFFFFFU ? hd->fb : (uint32_t)(fb_slot(c->fb_id) + 1);
    if (!slot) return c->fb_id == 0xFFFFFFFFU ? E_INVAL : E_NOENT;
    if (!mode_check(&c->mode)) return E_INVAL;
    struct framebuffer* f = &kms.fbs[slot - 1];
    if ((uint64_t)c->x + c->mode.hdisplay > f->width || (uint64_t)c->y + c->mode.vdisplay > f->height) return E_NOSPC;
    struct modeinfo m = c->mode; m.name[31] = 0;
    if (head_show((unsigned)i, slot, c->x, c->y, &m)) return E_IO;
    return 0;
}
static int get_crtc(struct mode_crtc* c) {
    int i = crtc_index(c->crtc_id); if (i < 0) return E_NOENT;
    struct head* hd = &kms.head[i];
    c->fb_id = hd->fb ? FB_BASE + hd->fb - 1 : 0; c->x = hd->x; c->y = hd->y; c->gamma_size = 0; c->mode_valid = hd->mode_valid;
    if (hd->mode_valid) c->mode = hd->mode; else memset(&c->mode, 0, sizeof(c->mode));
    return 0;
}
static int page_flip(unsigned client, struct page_flip* p) {
    if (!master(client)) return E_ACCES;
    if (p->flags & ~1U) return E_INVAL;          /* only DRM_MODE_PAGE_FLIP_EVENT: no async or target flips */
    if (p->reserved) return E_INVAL;
    int i = crtc_index(p->crtc_id); if (i < 0) return E_NOENT;
    struct head* hd = &kms.head[i];
    if (!hd->mode_valid || !hd->fb) return E_INVAL;
    int slot = fb_slot(p->fb_id); if (slot < 0) return E_NOENT;
    struct framebuffer* f = &kms.fbs[slot], *old = &kms.fbs[hd->fb - 1];
    if (f->format != old->format) return E_INVAL;
    if ((uint64_t)hd->x + hd->mode.hdisplay > f->width || (uint64_t)hd->y + hd->mode.vdisplay > f->height) return E_NOSPC;
    if ((p->flags & 1) && kms.clients[client].count >= EVENTS) return E_NOMEM;
    struct modeinfo m = hd->mode;
    if (head_show((unsigned)i, (uint32_t)slot + 1, hd->x, hd->y, &m)) return E_IO;
    if (p->flags & 1) queue_event(client, (unsigned)i, p->user_data);
    return 0;
}
static int set_plane(unsigned client, struct set_plane* p) {
    if (!master(client)) return E_ACCES;
    if (p->plane_id < PLANE_BASE || p->plane_id >= PLANE_BASE + kms.heads) return E_NOENT;
    unsigned i = p->plane_id - PLANE_BASE;
    if (!p->fb_id) return head_show(i, 0, 0, 0, NULL) ? E_IO : 0;   /* disabling the primary plane disables the CRTC */
    if (p->crtc_id != CRTC_BASE + i) return p->crtc_id >= CRTC_BASE && p->crtc_id < CRTC_BASE + kms.heads ? E_INVAL : E_NOENT;
    struct head* hd = &kms.head[i];
    int slot = fb_slot(p->fb_id); if (slot < 0) return E_NOENT;
    if (!hd->mode_valid) return E_INVAL;
    /* a primary plane must cover the whole CRTC, unscaled, from an integer source position */
    if (p->crtc_x || p->crtc_y || p->crtc_w != hd->mode.hdisplay || p->crtc_h != hd->mode.vdisplay || (p->src_x & 0xFFFF) || (p->src_y & 0xFFFF) ||
        p->src_w != (uint32_t)hd->mode.hdisplay << 16 || p->src_h != (uint32_t)hd->mode.vdisplay << 16) return E_INVAL;
    struct framebuffer* f = &kms.fbs[slot]; uint32_t x = p->src_x >> 16, y = p->src_y >> 16;
    if ((uint64_t)x + hd->mode.hdisplay > f->width || (uint64_t)y + hd->mode.vdisplay > f->height) return E_NOSPC;
    struct modeinfo m = hd->mode;
    return head_show(i, (uint32_t)slot + 1, x, y, &m) ? E_IO : 0;
}
static int dirty_fb(unsigned client, struct dirty_cmd* d) {
    if (!master(client)) return E_ACCES;
    int slot = fb_slot(d->fb_id); if (slot < 0) return E_NOENT;
    if (d->flags & ~3U || d->flags == 3 || d->num_clips > CLIPS_MAX || ((d->flags & 1) && (d->num_clips & 1))) return E_INVAL;
    struct framebuffer* f = &kms.fbs[slot]; bool shown = false;
    for (unsigned i = 0; i < kms.heads; i++) if (kms.head[i].fb == (uint32_t)slot + 1 && kms.head[i].virtio_scanout) shown = true;
    if (!shown || !f->resource) return 0;   /* not on screen: nothing to present */
    struct clip_rect clips[CLIPS_MAX]; unsigned count = d->num_clips;
    if (!count) { clips[0] = (struct clip_rect){0, 0, (uint16_t)(f->width > 0xFFFF ? 0xFFFF : f->width), (uint16_t)(f->height > 0xFFFF ? 0xFFFF : f->height)}; count = 1; }
    else { int error = copy_in(clips, d->clips_ptr, (uint64_t)count * sizeof(struct clip_rect)); if (error) return error; }
    int error = 0; unsigned last = count;
    for (unsigned k = 0; k < count; k++) {   /* clamp; the last non-empty clip waits for the host */
        uint32_t x1 = clips[k].x1 < f->width ? clips[k].x1 : f->width, x2 = clips[k].x2 < f->width ? clips[k].x2 : f->width;
        uint32_t y1 = clips[k].y1 < f->height ? clips[k].y1 : f->height, y2 = clips[k].y2 < f->height ? clips[k].y2 : f->height;
        clips[k] = (struct clip_rect){(uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2};
        if (x2 > x1 && y2 > y1) last = k;
    }
    for (unsigned k = 0; !error && k < count; k++)
        if (clips[k].x2 > clips[k].x1 && clips[k].y2 > clips[k].y1)
            error = transfer_flush(f, clips[k].x1, clips[k].y1, clips[k].x2 - clips[k].x1, clips[k].y2 - clips[k].y1, k == last);
    return error ? E_IO : 0;
}
static int create_dumb(unsigned client, struct create_dumb* c) {
    if (c->flags || !c->width || !c->height || c->width > MAX_SIZE || c->height > MAX_SIZE) return E_INVAL;
    if (c->bpp != 8 && c->bpp != 16 && c->bpp != 24 && c->bpp != 32) return E_INVAL;
    uint64_t pitch = (uint64_t)c->width * (c->bpp / 8), size = (pitch * c->height + PAGE - 1) & ~(PAGE - 1);
    uint64_t total, free, cached; native_memory_info(&total, &free, &cached);
    if (size > DUMB_LIMIT || kms.dumb_bytes + size > DUMB_TOTAL || size > free / 2) return E_NOMEM;
    for (unsigned n = 0; n < DUMBS; n++) if (!kms.dumbs[n].used) {
        struct dumb* d = &kms.dumbs[n];
        struct memory_file* memory = memory_file_create(2); if (!memory) return E_NOMEM;
        memory->size = size;
        for (uint64_t page = 0; page < size / PAGE; page++)
            if (!memory_file_page(memory, page, true)) { memory_file_release(memory); return E_NOMEM; }
        *d = (struct dumb){true, (uint16_t)(client + 1), c->width, c->height, (uint32_t)pitch, c->bpp, 1, true, size, memory};
        kms.dumb_bytes += size;
        c->handle = KMS_HANDLE_BASE + n; c->pitch = (uint32_t)pitch; c->size = size; return 0;
    }
    return E_NOMEM;
}
static int handle_drop(unsigned client, uint32_t handle) {
    int n = dumb_of(client, handle); if (n < 0) return E_INVAL;
    struct dumb* d = &kms.dumbs[n]; d->handle = false;
    if (!--d->references) { memory_file_release(d->memory); kms.dumb_bytes -= d->size; memset(d, 0, sizeof(*d)); }
    return 0;
}
static int add_fb(unsigned client, uint32_t* id, uint32_t width, uint32_t height, uint32_t format, uint32_t handle, uint32_t pitch, uint32_t offset) {
    if (!width || !height || width > MAX_SIZE || height > MAX_SIZE || !virtio_format(format)) return E_INVAL;
    if (pitch != width * 4 || offset) return E_INVAL;   /* the host copies rows with width * 4 bytes of stride */
    int n = dumb_of(client, handle); if (n < 0) return E_NOENT;
    struct dumb* d = &kms.dumbs[n];
    if ((uint64_t)pitch * height > d->size) return E_INVAL;
    for (unsigned s = 1; s < FBS; s++) if (!kms.fbs[s].used) {   /* slot 0 is the boot framebuffer */
        kms.fbs[s] = (struct framebuffer){true, (uint16_t)(client + 1), width, height, pitch, format,
                                          format == FORMAT_ARGB8888 || format == FORMAT_ABGR8888 ? 32 : 24, 32, (uint32_t)n + 1, 0};
        d->references++; *id = FB_BASE + s; return 0;
    }
    return E_NOSPC;
}
static int remove_fb(unsigned client, uint32_t id) {
    int slot = fb_slot(id); if (slot < 0) return E_NOENT;
    if (kms.fbs[slot].owner != client + 1) return E_NOENT;   /* only the creating file may remove it (the boot fb: never) */
    fb_destroy((unsigned)slot); return 0;
}
static int get_fb(unsigned client, uint32_t id, struct fb_cmd2* out, struct fb_cmd* legacy) {
    int slot = fb_slot(id); if (slot < 0) return E_NOENT;
    struct framebuffer* f = &kms.fbs[slot];
    uint32_t handle = f->dumb && f->owner == client + 1 && kms.dumbs[f->dumb - 1].handle ? KMS_HANDLE_BASE + f->dumb - 1 : 0;
    if (legacy) { legacy->width = f->width; legacy->height = f->height; legacy->pitch = f->pitch; legacy->bpp = f->bpp; legacy->depth = f->depth; legacy->handle = handle; }
    if (out) {
        memset(out->handles, 0, sizeof(out->handles) + sizeof(out->pitches) + sizeof(out->offsets)); memset(out->modifier, 0, sizeof(out->modifier));
        out->width = f->width; out->height = f->height; out->pixel_format = f->format; out->flags = 0; out->handles[0] = handle; out->pitches[0] = f->pitch;
    }
    return 0;
}
static int get_property(struct get_property* p) {
    static const struct property_enum dpms[4] = {{0, "On"}, {1, "Standby"}, {2, "Suspend"}, {3, "Off"}};
    static const struct property_enum types[3] = {{0, "Overlay"}, {1, "Primary"}, {2, "Cursor"}};
    const struct property_enum* list = NULL; unsigned count = 0; int error = 0;
    memset(p->name, 0, sizeof(p->name));
    if (p->prop_id == PROP_EDID) { strcpy(p->name, "EDID"); p->flags = 0x10 | 0x4; p->count_values = 0; p->count_enum_blobs = 0; return 0; }   /* BLOB | IMMUTABLE */
    if (p->prop_id == PROP_DPMS) { strcpy(p->name, "DPMS"); p->flags = 0x8; list = dpms; count = 4; }                                      /* ENUM */
    else if (p->prop_id == PROP_TYPE) { strcpy(p->name, "type"); p->flags = 0x8 | 0x4; list = types; count = 3; }                         /* ENUM | IMMUTABLE */
    else return E_NOENT;
    for (unsigned k = 0; !error && k < count; k++) {
        if (k < p->count_values) error = copy_out(p->values_ptr + k * 8, &list[k].value, 8);
        if (!error && k < p->count_enum_blobs) error = copy_out(p->enum_blob_ptr + k * sizeof(struct property_enum), &list[k], sizeof(struct property_enum));
    }
    if (error) return error;
    p->count_values = count; p->count_enum_blobs = count; return 0;
}
static int get_blob(struct get_blob* b) {
    for (unsigned i = 0; i < kms.heads; i++) if (kms.head[i].edid_blob && kms.head[i].edid_blob == b->blob_id) {
        if (b->length == kms.head[i].edid_size) { int error = copy_out(b->data, kms.head[i].edid, kms.head[i].edid_size); if (error) return error; }
        b->length = kms.head[i].edid_size; return 0;
    }
    return E_NOENT;
}
static int set_dpms(unsigned client, unsigned i, uint64_t value) {
    if (!master(client)) return E_ACCES;
    if (value > 3) return E_INVAL;
    struct head* hd = &kms.head[i]; uint32_t on = value == 0 ? 0 : 3;   /* standby/suspend act as off (one state, as Linux atomic helpers) */
    if (hd->dpms == on) return 0;
    hd->dpms = on;
    if (on) return scanout_off(i) ? E_IO : 0;
    if (hd->mode_valid && hd->fb) { struct modeinfo m = hd->mode; return head_show(i, hd->fb, hd->x, hd->y, &m) ? E_IO : 0; }
    return 0;
}
static int object_properties(unsigned client, struct obj_get_properties* o) {
    (void)client;
    uint32_t props[2]; uint64_t values[2]; unsigned count = 0;
    if ((o->obj_type == OBJECT_CONNECTOR || !o->obj_type) && o->obj_id >= CONNECTOR_BASE && o->obj_id < CONNECTOR_BASE + kms.heads) {
        struct head* hd = &kms.head[o->obj_id - CONNECTOR_BASE];
        props[0] = PROP_EDID; values[0] = hd->edid_blob; props[1] = PROP_DPMS; values[1] = hd->dpms; count = 2;
    } else if ((o->obj_type == OBJECT_PLANE || !o->obj_type) && o->obj_id >= PLANE_BASE && o->obj_id < PLANE_BASE + kms.heads) {
        props[0] = PROP_TYPE; values[0] = 1; count = 1;
    } else if ((o->obj_type == OBJECT_CRTC || !o->obj_type) && crtc_index(o->obj_id) >= 0) count = 0;
    else if ((o->obj_type == OBJECT_ENCODER || !o->obj_type) && o->obj_id >= ENCODER_BASE && o->obj_id < ENCODER_BASE + kms.heads) return E_INVAL;   /* encoders have no properties object */
    else if ((o->obj_type == OBJECT_FB || !o->obj_type) && fb_slot(o->obj_id) >= 0) return E_INVAL;
    else return E_NOENT;
    if (o->count_props >= count && count) {
        int error = copy_out(o->props_ptr, props, count * 4); if (!error) error = copy_out(o->prop_values_ptr, values, count * 8);
        if (error) return error;
    }
    o->count_props = count; return 0;
}

int kms_ioctl(unsigned client, unsigned node, uint32_t nr, void* data) {
    (void)node;
    if (!kms.ready || client >= DRM_CLIENTS) return E_INVAL;
    kms_lock();
    int r;
    struct kclient* k = &kms.clients[client];
    switch (nr) {
    case 0x1E:   /* SET_MASTER: the previous master again, or CAP_SYS_ADMIN, while nobody else is master */
        if (master(client)) r = 0;
        else if (kms.master) r = E_BUSY;
        else if (!k->master_before && !credentials_capable(credentials_current(), NULL, 21)) r = E_PERM;
        else { kms.master = (int)client + 1; k->master_before = true; r = 0; }
        break;
    case 0x1F: if (master(client)) { kms.master = 0; r = 0; } else r = E_INVAL; break;   /* DROP_MASTER */
    case 0x3A: r = E_OPNOTSUPP; break;                                    /* WAIT_VBLANK: no vblank interrupt (flip events instead) */
    case 0xA0: r = get_resources(client, data); break;
    case 0xA1: r = get_crtc(data); break;
    case 0xA2: r = set_crtc(client, data); break;
    case 0xA3: case 0xBB: r = master(client) ? E_NXIO : E_ACCES; break;  /* CURSOR(2): no hardware cursor plane (software cursors) */
    case 0xA4: case 0xA5: r = crtc_index(((uint32_t*)data)[0]) < 0 ? E_NOENT : E_INVAL; break;   /* gamma: gamma_size 0 */
    case 0xA6: {
        struct get_encoder* e = data;
        if (e->encoder_id < ENCODER_BASE || e->encoder_id >= ENCODER_BASE + kms.heads) { r = E_NOENT; break; }
        unsigned i = e->encoder_id - ENCODER_BASE;
        e->encoder_type = 5; e->crtc_id = kms.head[i].active ? CRTC_BASE + i : 0; e->possible_crtcs = 1U << i; e->possible_clones = 0; r = 0; break;
    }
    case 0xA7: r = get_connector(data); break;
    case 0xAA: r = get_property(data); break;
    case 0xAB: {
        struct connector_set_property* s = data;
        if (s->connector_id < CONNECTOR_BASE || s->connector_id >= CONNECTOR_BASE + kms.heads) r = E_NOENT;
        else if (s->prop_id == PROP_DPMS) r = set_dpms(client, s->connector_id - CONNECTOR_BASE, s->value);
        else r = s->prop_id == PROP_EDID ? E_INVAL : E_NOENT;
        break;
    }
    case 0xAC: r = get_blob(data); break;
    case 0xAD: r = get_fb(client, ((struct fb_cmd*)data)->fb_id, NULL, data); break;
    case 0xCE: r = get_fb(client, ((struct fb_cmd2*)data)->fb_id, data, NULL); break;
    case 0xAE: {
        struct fb_cmd* f = data;
        if (f->bpp != 32 || (f->depth != 24 && f->depth != 32)) { r = E_INVAL; break; }
        r = add_fb(client, &f->fb_id, f->width, f->height, f->depth == 32 ? FORMAT_ARGB8888 : FORMAT_XRGB8888, f->handle, f->pitch, 0); break;
    }
    case 0xB8: {
        struct fb_cmd2* f = data;
        if (f->flags & ~2U) { r = E_INVAL; break; }                       /* no interlaced framebuffers */
        if ((f->flags & 2) && f->modifier[0]) { r = E_INVAL; break; }     /* DRM_FORMAT_MOD_LINEAR only */
        if (f->handles[1] || f->handles[2] || f->handles[3] || f->pitches[1] || f->offsets[1]) { r = E_INVAL; break; }
        r = add_fb(client, &f->fb_id, f->width, f->height, f->pixel_format, f->handles[0], f->pitches[0], f->offsets[0]); break;
    }
    case 0xAF: r = remove_fb(client, *(uint32_t*)data); break;
    case 0xB0: r = page_flip(client, data); break;
    case 0xB1: r = dirty_fb(client, data); break;
    case 0xB2: r = create_dumb(client, data); break;
    case 0xB3: {
        struct map_dumb* m = data; int n = dumb_of(client, m->handle);
        if (n < 0) { r = E_NOENT; break; }
        m->offset = (KMS_MAP_BASE + (uint64_t)n) << 32; r = 0; break;
    }
    case 0xB4: r = handle_drop(client, *(uint32_t*)data); break;
    case 0xB5: {
        struct plane_res* p = data; unsigned count = k->universal ? kms.heads : 0; uint32_t ids[HEADS];
        for (unsigned i = 0; i < count; i++) ids[i] = PLANE_BASE + i;
        r = copy_out(p->plane_id_ptr, ids, (uint64_t)(p->count_planes < count ? p->count_planes : count) * 4);
        if (!r) p->count_planes = count;
        break;
    }
    case 0xB6: {
        struct get_plane* p = data;
        if (p->plane_id < PLANE_BASE || p->plane_id >= PLANE_BASE + kms.heads) { r = E_NOENT; break; }
        unsigned i = p->plane_id - PLANE_BASE; static const uint32_t formats[4] = {FORMAT_XRGB8888, FORMAT_ARGB8888, FORMAT_XBGR8888, FORMAT_ABGR8888};
        r = p->count_format_types >= 4 ? copy_out(p->format_type_ptr, formats, sizeof(formats)) : 0;
        if (r) break;
        p->crtc_id = kms.head[i].active ? CRTC_BASE + i : 0; p->fb_id = kms.head[i].fb ? FB_BASE + kms.head[i].fb - 1 : 0;
        p->possible_crtcs = 1U << i; p->gamma_size = 0; p->count_format_types = 4; break;
    }
    case 0xB7: r = set_plane(client, data); break;
    case 0xB9: r = object_properties(client, data); break;
    case 0xBA: {
        struct obj_set_property* s = data;
        if ((s->obj_type == OBJECT_CONNECTOR || !s->obj_type) && s->obj_id >= CONNECTOR_BASE && s->obj_id < CONNECTOR_BASE + kms.heads)
            r = s->prop_id == PROP_DPMS ? set_dpms(client, s->obj_id - CONNECTOR_BASE, s->value) : s->prop_id == PROP_EDID ? E_INVAL : E_NOENT;
        else if ((s->obj_type == OBJECT_PLANE || !s->obj_type) && s->obj_id >= PLANE_BASE && s->obj_id < PLANE_BASE + kms.heads)
            r = s->prop_id == PROP_TYPE ? E_INVAL : E_NOENT;   /* immutable */
        else r = E_NOENT;
        break;
    }
    case 0xBC: r = E_OPNOTSUPP; break;                                    /* ATOMIC: legacy KMS only (DRIVER_ATOMIC not offered) */
    default: r = E_INVAL;
    }
    kms_unlock();
    return r;
}
int kms_get_cap(uint64_t capability, uint64_t* value) {
    switch (capability) {
    case 1: *value = 1; return 0;            /* DUMB_BUFFER */
    case 2: *value = 0; return 0;            /* VBLANK_HIGH_CRTC */
    case 3: *value = 24; return 0;           /* DUMB_PREFERRED_DEPTH */
    case 4: *value = 0; return 0;            /* DUMB_PREFER_SHADOW */
    case 7: *value = 0; return 0;            /* ASYNC_PAGE_FLIP */
    case 8: case 9: *value = 64; return 0;   /* CURSOR_WIDTH/HEIGHT (no cursor plane: CURSOR answers ENXIO) */
    case 0x10: *value = 0; return 0;         /* ADDFB2_MODIFIERS */
    case 0x11: *value = 0; return 0;         /* PAGE_FLIP_TARGET */
    case 0x12: *value = 1; return 0;         /* CRTC_IN_VBLANK_EVENT */
    case 0x15: *value = 0; return 0;         /* ATOMIC_ASYNC_PAGE_FLIP */
    }
    return E_INVAL;
}
int kms_set_client_cap(unsigned client, uint64_t capability, uint64_t value) {
    if (client >= DRM_CLIENTS) return E_INVAL;
    struct kclient* k = &kms.clients[client];
    if (value > 1 && capability != 3) return E_INVAL;
    switch (capability) {
    case 1: k->stereo = value; return 0;      /* STEREO_3D: no stereo modes are listed either way */
    case 2: k->universal = value; return 0;   /* UNIVERSAL_PLANES */
    case 4: k->aspect = value; return 0;      /* ASPECT_RATIO: modes carry no aspect flags either way */
    case 3: return E_OPNOTSUPP;               /* ATOMIC */
    case 5: return E_INVAL;                   /* WRITEBACK_CONNECTORS needs ATOMIC */
    case 6: return E_OPNOTSUPP;               /* CURSOR_PLANE_HOTSPOT needs ATOMIC */
    }
    return E_INVAL;
}
bool kms_handle(uint32_t handle) { return handle >= KMS_HANDLE_BASE && handle - KMS_HANDLE_BASE < DUMBS; }
int kms_handle_close(unsigned client, uint32_t handle) {
    if (!kms.ready) return E_INVAL;
    kms_lock(); int r = handle_drop(client, handle); kms_unlock(); return r;
}
struct memory_file* kms_mapping(unsigned client, uint64_t* offset, uint64_t length) {
    if (!kms.ready || client >= DRM_CLIENTS) return NULL;
    uint64_t index = (*offset >> 32) - KMS_MAP_BASE, inside = *offset & 0xFFFFFFFFULL;
    if (index >= DUMBS) return NULL;
    struct dumb* d = &kms.dumbs[index];
    if (!d->used || d->owner != client + 1 || inside > d->size || length > d->size - inside) return NULL;
    *offset = inside; return d->memory;
}
uint32_t kms_ready(unsigned client) { return kms.ready && client < DRM_CLIENTS && kms.clients[client].count ? 1U : 0; }
void* kms_event_source(void) { return &kms.channel; }
int64_t kms_read(unsigned client, uint64_t buffer, uint64_t count, uint64_t flags) {
    if (!kms.ready || client >= DRM_CLIENTS) return E_INVAL;
    struct kclient* k = &kms.clients[client]; int64_t done = 0;
    for (;;) {
        if (!k->count) {
            if (done) return done;
            if (flags & 0x800) return E_AGAIN;
            int error = task_wait_channel(&kms.channel); if (error) return error;
            if (!k->open) return E_BADF;
            continue;
        }
        struct event_vblank* e = &k->events[k->head];
        if (e->length > count - (uint64_t)done) return done;   /* Linux drm_read: an event never splits */
        if (!accessible(buffer + (uint64_t)done, e->length, true)) return done ? done : E_FAULT;
        if (copy_to_user((void*)(buffer + (uint64_t)done), e, e->length)) return done ? done : E_FAULT; done += e->length;
        k->head = (k->head + 1) % EVENTS; k->count--;
    }
}
void kms_open(unsigned client, unsigned node) {
    if (!kms.ready || client >= DRM_CLIENTS) return;
    memset(&kms.clients[client], 0, sizeof(kms.clients[client])); kms.clients[client].open = true;
    if (node == DRM_NODE_CARD && !kms.master) { kms.master = (int)client + 1; kms.clients[client].master_before = true; }
}
void kms_close(unsigned client) {
    if (!kms.ready || client >= DRM_CLIENTS || !kms.clients[client].open) return;
    kms_lock();
    for (unsigned s = 1; s < FBS; s++) if (kms.fbs[s].used && kms.fbs[s].owner == client + 1) fb_destroy(s);
    for (unsigned n = 0; n < DUMBS; n++) if (kms.dumbs[n].used && kms.dumbs[n].owner == client + 1 && kms.dumbs[n].handle) handle_drop(client, KMS_HANDLE_BASE + n);
    bool was_master = master(client);
    memset(&kms.clients[client], 0, sizeof(kms.clients[client]));
    if (was_master) { kms.master = 0; boot_restore(); }
    kms_unlock();
    task_wake_channel(&kms.channel);
}
void kms_display_event(void) {
    if (!kms.ready) return;
    kms.event_pending = true;
    /* change@/devices/pci0000:00/<slot>/virtio0/drm/card0, as the DRM hot-plug uevent (drm_sysfs_hotplug_event) */
    char path[96] = "/devices/pci0000:00/0000:", message[384]; uint32_t location = virtio_gpu_pci_location();
    const char* hex = "0123456789abcdef"; unsigned at = strlen(path);
    path[at++] = hex[(location >> 12) & 15]; path[at++] = hex[(location >> 8) & 15]; path[at++] = ':';
    path[at++] = hex[((location >> 3) & 31) >> 4]; path[at++] = hex[((location >> 3) & 31) & 15]; path[at++] = '.'; path[at++] = hex[location & 7]; path[at] = 0;
    strcat(path, "/virtio0/drm/card0");
    char sequence[24]; uint_to_str((uint32_t)++kms.uevents, sequence);
    const char* fields[][2] = {{"change@", path}, {"ACTION=change", ""}, {"DEVPATH=", path}, {"SUBSYSTEM=drm", ""}, {"HOTPLUG=1", ""},
                               {"MAJOR=226", ""}, {"MINOR=0", ""}, {"DEVNAME=dri/card0", ""}, {"DEVTYPE=drm_minor", ""}, {"SEQNUM=", sequence}};
    unsigned length = 0;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {   /* "KEY=VALUE\0" fields */
        unsigned a = strlen(fields[i][0]), b = strlen(fields[i][1]);
        if (length + a + b + 1 > sizeof(message)) return;
        memcpy(message + length, fields[i][0], a); memcpy(message + length + a, fields[i][1], b); length += a + b; message[length++] = 0;
    }
    netlink_uevent(message, length);
}
bool kms_available(void) { return kms.ready; }
/* After S3/hibernation (virtio_gpu.c negotiated the reset device again; tasks are frozen): every framebuffer that had a
 * host resource gets it again under the same id, and every head the host showed scans it out again with its contents.
 * A command that fails leaves that framebuffer without a resource / the head off the virtio scanout (DIRTYFB then
 * presents nothing instead of waiting). An ioctl frozen in the middle of a command got an error for it. */
void kms_resume(void) {
    if (!kms.ready) return;
    unsigned restored = 0, shown = 0;
    for (unsigned s = 0; s < FBS; s++) {
        struct framebuffer* f = &kms.fbs[s]; if (!f->used || !f->resource) continue;
        if (fb_resource_create(f, f->resource)) { virtio_gpu_resource_id_free(f->resource); f->resource = 0; } else restored++;
    }
    for (unsigned i = 0; i < kms.heads; i++) {
        struct head* hd = &kms.head[i]; if (!hd->virtio_scanout) continue;
        hd->virtio_scanout = false;
        if (!hd->fb || hd->dpms != 0 || hd->lid_off) continue;
        struct framebuffer* f = &kms.fbs[hd->fb - 1]; if (!f->resource) continue;
        struct { struct ctrl_hdr h; struct rect r; uint32_t scanout, resource; } s = {header(CMD_SET_SCANOUT), {hd->x, hd->y, hd->mode.hdisplay, hd->mode.vdisplay}, i, f->resource};
        if (command(&s, sizeof(s), NULL, 0, NULL, 0, RESP_OK_NODATA, true) || transfer_flush(f, hd->x, hd->y, hd->mode.hdisplay, hd->mode.vdisplay, true)) continue;
        hd->virtio_scanout = true; shown++;
    }
    log_number("[64] KMS resumed resources=", restored); log_number(" scanouts=", shown); vga_print("\n");
}
unsigned kms_live_objects(void) {   /* dumb buffers and framebuffers other than the boot framebuffer (campaign leak check) */
    unsigned n = 0; if (!kms.ready) return 0;
    for (unsigned i = 0; i < DUMBS; i++) n += kms.dumbs[i].used;
    for (unsigned s = 1; s < FBS; s++) n += kms.fbs[s].used;
    return n;
}
void kms_init(void) {
    if (kms.ready || !virtio_gpu_ready()) return;
    unsigned heads = virtio_gpu_scanouts(); if (!heads) return;
    kms.head = (void*)arena_alloc((uint64_t)HEADS * sizeof(struct head));
    kms.dumbs = (void*)arena_alloc((uint64_t)DUMBS * sizeof(struct dumb));
    kms.clients = (void*)arena_alloc((uint64_t)DRM_CLIENTS * sizeof(struct kclient));
    if (!kms.head || !kms.dumbs || !kms.clients) { vga_print("[64] KMS: no memory for tables\n"); return; }
    memset(kms.head, 0, (uint64_t)HEADS * sizeof(struct head)); memset(kms.dumbs, 0, (uint64_t)DUMBS * sizeof(struct dumb));
    memset(kms.clients, 0, (uint64_t)DRM_CLIENTS * sizeof(struct kclient));
    kms.heads = heads; kms.next_blob = BLOB_BASE + HEADS; kms.ready = true;
    probe();
    const struct framebuffer_info* boot = framebuffer_get();
    /* The firmware framebuffer is this device's VRAM: framebuffer object FB_BASE, shown on CRTC 0 (through VGA for now). */
    if (boot && virtio_gpu_vga() && boot->stride == boot->width * 4 && mode_usable(boot->width, boot->height)) {
        kms.boot_valid = true;
        kms.fbs[0] = (struct framebuffer){true, 0, boot->width, boot->height, boot->stride, FORMAT_XRGB8888, 24, 32, 0, 0};
        kms.head[0].fb = 1; kms.head[0].mode = mode_cvt(boot->width, boot->height, 60); kms.head[0].mode_valid = kms.head[0].active = true;
    }
    unsigned connected = 0; for (unsigned i = 0; i < heads; i++) connected += kms.head[i].connected;
    log_number("[64] KMS ready heads=", heads); log_number(" connected=", connected); vga_print(kms.boot_valid ? " boot-fb=crtc0\n" : "\n");
}
/* power.c lid policy ("docked" = an external display): heads the host reports connected (0 before KMS is ready). */
unsigned kms_connected_heads(void) {
    unsigned connected = 0;
    if (kms.ready && kms.head) for (unsigned i = 0; i < kms.heads; i++) connected += kms.head[i].connected;
    return connected;
}
/* power.c lid policy: with the lid closed and the machine kept up (lid policy ignore, or docked) the built-in panel,
 * head 0, goes dark whatever a DRM master asks meanwhile, and shows its CRTC again when the lid opens. Called on the
 * boot CPU's pass (no task: the virtio commands are polled); -11 while an ioctl holds the KMS state (retry later). */
int kms_lid(bool closed) {
    if (!kms.ready || !kms.heads || !kms.head) return 0;
    struct head* hd = &kms.head[0];
    if (hd->lid_off == closed) return 0;
    if (kms.locked) return -11;
    kms.locked = true; hd->lid_off = closed;
    int error = 0;
    if (closed) error = scanout_off(0);
    else if (hd->dpms == 0 && hd->mode_valid && hd->fb) { struct modeinfo m = hd->mode; error = head_show(0, hd->fb, hd->x, hd->y, &m); }
    kms_unlock();
    vga_print(closed ? "[64] KMS built-in panel off (lid closed)" : "[64] KMS built-in panel on (lid open)"); vga_print(error ? ", command failed\n" : "\n");
    return 0;
}
