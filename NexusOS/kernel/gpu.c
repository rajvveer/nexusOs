/* ============================================================================
 * NexusOS — VirtIO-GPU Driver + 2D Acceleration (Implementation) — Phase 41
 * ============================================================================
 * Bring-up sequence (QEMU "-device virtio-vga", PCI 1AF4:1050):
 *
 *   probe → transport setup → feature negotiation → controlq →
 *   DRIVER_OK → GET_DISPLAY_INFO → RESOURCE_CREATE_2D (B8G8R8X8) →
 *   ATTACH_BACKING (the kernel back buffer itself — zero-copy) →
 *   SET_SCANOUT → first flush.
 *
 * Once SET_SCANOUT lands, QEMU retires the VGA-compat output and this driver
 * owns the display: fb_flip() routes here and a frame becomes one
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH pair (dirty-rect capable) instead of
 * a 3 MB memcpy into the VESA aperture.
 *
 * All commands run synchronously on the controlq in polled mode, one at a
 * time, from two small static buffers (identity-mapped kernel BSS).
 * ============================================================================ */

#include "gpu.h"
#include "virtio.h"
#include "framebuffer.h"
#include "string.h"
#include "vga.h"

/* --- VirtIO-GPU command/response types (2D set) --- */
#define GPU_CMD_GET_DISPLAY_INFO    0x0100
#define GPU_CMD_RESOURCE_CREATE_2D  0x0101
#define GPU_CMD_SET_SCANOUT         0x0103
#define GPU_CMD_RESOURCE_FLUSH      0x0104
#define GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define GPU_CMD_ATTACH_BACKING      0x0106
#define GPU_RESP_OK_NODATA          0x1100
#define GPU_RESP_OK_DISPLAY_INFO    0x1101

#define GPU_FORMAT_B8G8R8X8_UNORM   2     /* bytes B,G,R,X == our 0xXXRRGGBB */
#define GPU_MAX_SCANOUTS            16
#define FB_RESOURCE_ID              1
#define CONTROLQ_MAX                16    /* we run one command at a time */

/* --- wire structures (little-endian, packed; 64-bit fields as lo/hi) --- */
typedef struct __attribute__((packed)) {
    uint32_t type, flags;
    uint32_t fence_lo, fence_hi;
    uint32_t ctx_id;
    uint8_t  ring_idx, pad[3];
} gpu_hdr_t;

typedef struct __attribute__((packed)) { uint32_t x, y, w, h; } gpu_rect_t;

typedef struct __attribute__((packed)) {
    gpu_hdr_t hdr; uint32_t resource_id, format, width, height;
} gpu_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    gpu_hdr_t hdr; gpu_rect_t r; uint32_t scanout_id, resource_id;
} gpu_set_scanout_t;

typedef struct __attribute__((packed)) {
    gpu_hdr_t hdr; gpu_rect_t r; uint32_t resource_id, padding;
} gpu_resource_flush_t;

typedef struct __attribute__((packed)) {
    gpu_hdr_t hdr; gpu_rect_t r; uint32_t offset_lo, offset_hi;
    uint32_t resource_id, padding;
} gpu_transfer_to_host_2d_t;

typedef struct __attribute__((packed)) {
    gpu_hdr_t hdr; uint32_t resource_id, nr_entries;
    uint32_t addr_lo, addr_hi, length, pad;   /* one inline backing entry */
} gpu_attach_backing_t;

typedef struct __attribute__((packed)) {
    gpu_hdr_t hdr;
    struct __attribute__((packed)) {
        gpu_rect_t r; uint32_t enabled, flags;
    } pmodes[GPU_MAX_SCANOUTS];
} gpu_display_info_t;

/* --- driver state --- */
static struct {
    bool         active;
    virtio_dev_t vdev;
    virtq_t      controlq;
    uint32_t*    backbuf;          /* the zero-copy backing (fb_back)      */
    int          width, height;
    int          pref_w, pref_h;
    int          num_scanouts;
    uint32_t     presents;
    char         status[96];
} gpu;

/* Command/response staging buffers — kernel BSS is identity-mapped. */
static union {
    gpu_hdr_t                 hdr;
    gpu_resource_create_2d_t  create;
    gpu_set_scanout_t         scanout;
    gpu_resource_flush_t      flush;
    gpu_transfer_to_host_2d_t transfer;
    gpu_attach_backing_t      attach;
} cmd;
static union {
    gpu_hdr_t          hdr;
    gpu_display_info_t display;
} resp;

/* --------------------------------------------------------------------------
 * gpu_cmd: send one command, await the response, check its type
 * -------------------------------------------------------------------------- */
static bool gpu_cmd(uint32_t type, uint32_t cmd_len, uint32_t resp_len, uint32_t ok_type) {
    cmd.hdr.type = type;
    cmd.hdr.flags = 0;
    cmd.hdr.fence_lo = 0; cmd.hdr.fence_hi = 0;
    cmd.hdr.ctx_id = 0; cmd.hdr.ring_idx = 0;
    cmd.hdr.pad[0] = cmd.hdr.pad[1] = cmd.hdr.pad[2] = 0;
    memset(&resp, 0, resp_len);

    int n = virtio_run(&gpu.vdev, &gpu.controlq, &cmd, cmd_len, &resp, resp_len);
    return n >= (int)sizeof(gpu_hdr_t) && resp.hdr.type == ok_type;
}

/* --------------------------------------------------------------------------
 * Presentation: TRANSFER_TO_HOST_2D + RESOURCE_FLUSH of a rectangle
 * -------------------------------------------------------------------------- */
static bool gpu_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    memset(&cmd.transfer, 0, sizeof(cmd.transfer));
    cmd.transfer.r.x = x; cmd.transfer.r.y = y;
    cmd.transfer.r.w = w; cmd.transfer.r.h = h;
    /* Backing stride is resource width: offset = (y*width + x) * 4 */
    cmd.transfer.offset_lo = (y * (uint32_t)gpu.width + x) * 4;
    cmd.transfer.resource_id = FB_RESOURCE_ID;
    if (!gpu_cmd(GPU_CMD_TRANSFER_TO_HOST_2D, sizeof(cmd.transfer),
                 sizeof(gpu_hdr_t), GPU_RESP_OK_NODATA))
        return false;

    memset(&cmd.flush, 0, sizeof(cmd.flush));
    cmd.flush.r.x = x; cmd.flush.r.y = y;
    cmd.flush.r.w = w; cmd.flush.r.h = h;
    cmd.flush.resource_id = FB_RESOURCE_ID;
    if (!gpu_cmd(GPU_CMD_RESOURCE_FLUSH, sizeof(cmd.flush),
                 sizeof(gpu_hdr_t), GPU_RESP_OK_NODATA))
        return false;

    gpu.presents++;
    return true;
}

bool gpu_flip(void) {
    if (!gpu.active) return false;
    return gpu_present_rect(0, 0, (uint32_t)gpu.width, (uint32_t)gpu.height);
}

void gpu_present(int x, int y, int w, int h) {
    if (!gpu.active) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gpu.width)  w = gpu.width - x;
    if (y + h > gpu.height) h = gpu.height - y;
    if (w <= 0 || h <= 0) return;
    gpu_present_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

/* --------------------------------------------------------------------------
 * Accelerated 2D primitives on the back buffer (rep stosl / rep movsl)
 * -------------------------------------------------------------------------- */
static inline void fill32(uint32_t* dst, uint32_t val, uint32_t count) {
    __asm__ volatile("rep stosl" : "+D"(dst), "+c"(count) : "a"(val) : "memory");
}
static inline void copy32(uint32_t* dst, const uint32_t* src, uint32_t count) {
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
}

void gpu_fill(int x, int y, int w, int h, uint32_t color) {
    uint32_t* bb = fb_get_backbuffer();
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    if (!bb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++)
        fill32(bb + row * sw + x, color, (uint32_t)w);
}

void gpu_blit(int dx, int dy, int sx, int sy, int w, int h) {
    uint32_t* bb = fb_get_backbuffer();
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    if (!bb) return;
    /* Clip both rectangles together. */
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > sw) w = sw - sx;
    if (sy + h > sh) h = sh - sy;
    if (dx + w > sw) w = sw - dx;
    if (dy + h > sh) h = sh - dy;
    if (w <= 0 || h <= 0) return;

    if (dy > sy) {                       /* overlap: walk rows bottom-up */
        for (int row = h - 1; row >= 0; row--)
            copy32(bb + (dy + row) * sw + dx, bb + (sy + row) * sw + sx, (uint32_t)w);
    } else if (dy == sy && dx > sx) {    /* same rows, shifting right    */
        for (int row = 0; row < h; row++) {
            uint32_t* d = bb + (dy + row) * sw + dx;
            uint32_t* s = bb + (sy + row) * sw + sx;
            for (int i = w - 1; i >= 0; i--) d[i] = s[i];
        }
    } else {
        for (int row = 0; row < h; row++)
            copy32(bb + (dy + row) * sw + dx, bb + (sy + row) * sw + sx, (uint32_t)w);
    }
}

/* --------------------------------------------------------------------------
 * gpu_init: probe, negotiate, create + scan out the framebuffer resource
 * -------------------------------------------------------------------------- */
void gpu_init(void) {
    memset(&gpu, 0, sizeof(gpu));
    strcpy(gpu.status, "VirtIO-GPU: not present");

    if (!fb_is_vesa()) return;
    gpu.backbuf = fb_get_backbuffer();
    gpu.width   = (int)fb_get_width();
    gpu.height  = (int)fb_get_height();

    pci_device_t* dev = pci_find_device(VIRTIO_VENDOR, VIRTIO_DEV_GPU);
    if (!dev) {
        vga_print_color("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("VirtIO-GPU: not found (add QEMU '-device virtio-vga')\n");
        return;
    }

    if (!virtio_pci_setup(&gpu.vdev, dev) ||
        !virtio_negotiate(&gpu.vdev, 0, 0) ||
        !virtio_queue_init(&gpu.vdev, &gpu.controlq, 0, CONTROLQ_MAX)) {
        strcpy(gpu.status, "VirtIO-GPU: transport setup failed");
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("VirtIO-GPU: transport setup failed\n");
        return;
    }

    /* num_scanouts from the device config (events_read/clear precede it). */
    if (gpu.vdev.device_cfg)
        gpu.num_scanouts = (int)((volatile uint32_t*)gpu.vdev.device_cfg)[2];

    virtio_driver_ok(&gpu.vdev);

    /* Host's current/preferred mode (virtio-vga reports the VBE mode). */
    if (gpu_cmd(GPU_CMD_GET_DISPLAY_INFO, sizeof(gpu_hdr_t),
                sizeof(gpu_display_info_t), GPU_RESP_OK_DISPLAY_INFO)) {
        gpu.pref_w = (int)resp.display.pmodes[0].r.w;
        gpu.pref_h = (int)resp.display.pmodes[0].r.h;
    }

    /* Framebuffer resource matching the back buffer, backed by it directly. */
    memset(&cmd.create, 0, sizeof(cmd.create));
    cmd.create.resource_id = FB_RESOURCE_ID;
    cmd.create.format = GPU_FORMAT_B8G8R8X8_UNORM;
    cmd.create.width  = (uint32_t)gpu.width;
    cmd.create.height = (uint32_t)gpu.height;
    if (!gpu_cmd(GPU_CMD_RESOURCE_CREATE_2D, sizeof(cmd.create),
                 sizeof(gpu_hdr_t), GPU_RESP_OK_NODATA)) goto fail;

    memset(&cmd.attach, 0, sizeof(cmd.attach));
    cmd.attach.resource_id = FB_RESOURCE_ID;
    cmd.attach.nr_entries  = 1;
    cmd.attach.addr_lo = (uint32_t)gpu.backbuf;   /* identity-mapped */
    cmd.attach.length  = (uint32_t)(gpu.width * gpu.height * 4);
    if (!gpu_cmd(GPU_CMD_ATTACH_BACKING, sizeof(cmd.attach),
                 sizeof(gpu_hdr_t), GPU_RESP_OK_NODATA)) goto fail;

    memset(&cmd.scanout, 0, sizeof(cmd.scanout));
    cmd.scanout.r.w = (uint32_t)gpu.width;
    cmd.scanout.r.h = (uint32_t)gpu.height;
    cmd.scanout.scanout_id  = 0;
    cmd.scanout.resource_id = FB_RESOURCE_ID;
    if (!gpu_cmd(GPU_CMD_SET_SCANOUT, sizeof(cmd.scanout),
                 sizeof(gpu_hdr_t), GPU_RESP_OK_NODATA)) goto fail;

    gpu.active = true;

    /* The VGA-compat output is gone now — present the console right away. */
    gpu_flip();

    /* Status line: "VirtIO-GPU 1024x768 ctrlq=16 (zero-copy scanout)" */
    {
        char* p = gpu.status; char b[12];
        const char* s1 = "VirtIO-GPU ";
        while (*s1) *p++ = *s1++;
        int_to_str(gpu.width, b);  for (char* q = b; *q; q++) *p++ = *q;
        *p++ = 'x';
        int_to_str(gpu.height, b); for (char* q = b; *q; q++) *p++ = *q;
        const char* s2 = " ctrlq=";
        while (*s2) *p++ = *s2++;
        int_to_str(gpu.controlq.size, b); for (char* q = b; *q; q++) *p++ = *q;
        const char* s3 = " (zero-copy scanout)";
        while (*s3) *p++ = *s3++;
        *p = '\0';
    }

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("VirtIO-GPU: scanout active, zero-copy presents\n");
    return;

fail:
    gpu.active = false;
    strcpy(gpu.status, "VirtIO-GPU: command failed (VESA fallback)");
    vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("VirtIO-GPU: init command failed, using VESA\n");
}

bool gpu_active(void) { return gpu.active; }
const char* gpu_status(void) { return gpu.status; }

bool gpu_get_info(gpu_info_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->active       = gpu.active;
    out->width        = gpu.width;
    out->height       = gpu.height;
    out->pref_width   = gpu.pref_w;
    out->pref_height  = gpu.pref_h;
    out->num_scanouts = gpu.num_scanouts;
    out->queue_size   = gpu.controlq.size;
    out->features_lo  = gpu.vdev.dev_features_lo;
    out->features_hi  = gpu.vdev.dev_features_hi;
    out->presents     = gpu.presents;
    out->backing_addr = (uint32_t)gpu.backbuf;
    return gpu.vdev.present;
}
