/* Aurora zwp_linux_dmabuf_v1 (version 3): GPU clients hand over dma-buf wl_buffers.
 *
 * Buffers come from the virtio-gpu (virgl) render node, so their pixels live in host GPU memory.
 * Aurora imports each dma-buf on its own render-node descriptor (PRIME_FD_TO_HANDLE, RESOURCE_INFO,
 * MAP + mmap) and, when a commit is applied, reads the damaged box back into that mapping with
 * DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST + DRM_IOCTL_VIRTGPU_WAIT. The host processes the client's
 * rendering and this transfer in queue order, and WAIT returns after the transfer's fence, so the
 * copy that follows reads finished pixels. Composition itself stays the CPU compositor used for wl_shm.
 * Formats: ARGB8888/XRGB8888, single plane, linear or implicit (DRM_FORMAT_MOD_INVALID) layout. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server.h>
#include "aurora_dmabuf.h"

/* ---- protocol glue (what wayland-scanner emits for linux-dmabuf-v1.xml, interfaces at version 3) ---- */
extern const struct wl_interface zwp_linux_buffer_params_v1_interface;
static const struct wl_interface* dmabuf_types[] = {
    NULL, NULL, NULL, NULL, NULL, NULL,
    &zwp_linux_buffer_params_v1_interface,
    &wl_buffer_interface, NULL, NULL, NULL, NULL,
    &wl_buffer_interface,
};
static const struct wl_message dmabuf_requests[] = {{"destroy", "", dmabuf_types + 0}, {"create_params", "n", dmabuf_types + 6}};
static const struct wl_message dmabuf_events[] = {{"format", "u", dmabuf_types + 0}, {"modifier", "3uuu", dmabuf_types + 0}};
const struct wl_interface zwp_linux_dmabuf_v1_interface = {"zwp_linux_dmabuf_v1", 3, 2, dmabuf_requests, 2, dmabuf_events};
static const struct wl_message params_requests[] = {
    {"destroy", "", dmabuf_types + 0}, {"add", "huuuuu", dmabuf_types + 0},
    {"create", "iiuu", dmabuf_types + 0}, {"create_immed", "2niiuu", dmabuf_types + 7}};
static const struct wl_message params_events[] = {{"created", "n", dmabuf_types + 12}, {"failed", "", dmabuf_types + 0}};
const struct wl_interface zwp_linux_buffer_params_v1_interface = {"zwp_linux_buffer_params_v1", 3, 4, params_requests, 2, params_events};
struct dmabuf_interface { void (*destroy)(struct wl_client*, struct wl_resource*); void (*create_params)(struct wl_client*, struct wl_resource*, uint32_t); };
struct params_interface {
    void (*destroy)(struct wl_client*, struct wl_resource*);
    void (*add)(struct wl_client*, struct wl_resource*, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*create)(struct wl_client*, struct wl_resource*, int32_t, int32_t, uint32_t, uint32_t);
    void (*create_immed)(struct wl_client*, struct wl_resource*, uint32_t, int32_t, int32_t, uint32_t, uint32_t);
};
enum { ERROR_ALREADY_USED, ERROR_PLANE_IDX, ERROR_PLANE_SET, ERROR_INCOMPLETE, ERROR_INVALID_FORMAT, ERROR_INVALID_DIMENSIONS,
       ERROR_OUT_OF_BOUNDS, ERROR_INVALID_WL_BUFFER };

/* ---- virtio-gpu DRM uapi (include/uapi/drm/drm.h, virtgpu_drm.h) ---- */
struct prime_handle { uint32_t handle, flags; int32_t fd; };
struct gem_close { uint32_t handle, pad; };
struct resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct map_request { uint64_t offset; uint32_t handle, pad; };
struct transfer { uint32_t bo_handle, x, y, z, w, h, d, level, offset, stride, layer_stride; };
struct wait_request { uint32_t handle, flags; };
#define IOWR(nr, size) (0xC0000000U | ((uint32_t)(size) << 16) | ('d' << 8) | (nr))
#define IOW(nr, size) (0x40000000U | ((uint32_t)(size) << 16) | ('d' << 8) | (nr))
#define FORMAT_ARGB8888 0x34325241U
#define FORMAT_XRGB8888 0x34325258U
#define MODIFIER_LINEAR 0ULL
#define MODIFIER_INVALID 0x00FFFFFFFFFFFFFFULL

struct dmabuf { struct wl_resource* resource; uint32_t handle, width, height, format, stride, offset; uint8_t* map; size_t map_size; };
struct params { struct wl_resource* resource; int fd; uint32_t offset, stride; uint64_t modifier; bool set, used; };
static int render_fd = -1;
static struct { uint32_t handle, references; }* handles; static unsigned handle_count, handle_capacity;
static unsigned logged;

static int drm(unsigned long request, void* argument) {
    int result; do result = ioctl(render_fd, request, argument); while (result == -1 && (errno == EINTR || errno == EAGAIN)); return result;
}
/* PRIME import returns the existing handle for a buffer imported before; GEM handles are not counted. */
static bool handle_retain(uint32_t handle) {
    for (unsigned i = 0; i < handle_count; i++) if (handles[i].handle == handle) { handles[i].references++; return true; }
    if (handle_count == handle_capacity) {
        unsigned capacity = handle_capacity ? handle_capacity * 2 : 64; void* grown = realloc(handles, capacity * sizeof(*handles));
        if (!grown) return false;
        handles = grown; handle_capacity = capacity;
    }
    handles[handle_count].handle = handle; handles[handle_count++].references = 1; return true;
}
static void handle_release(uint32_t handle) {
    for (unsigned i = 0; i < handle_count; i++) if (handles[i].handle == handle) {
        if (--handles[i].references) return;
        handles[i] = handles[--handle_count];
        struct gem_close close_request = {handle, 0}; drm(IOW(0x09, sizeof(close_request)), &close_request);
        return;
    }
}

static void buffer_destroy_request(struct wl_client* client, struct wl_resource* resource) { (void)client; wl_resource_destroy(resource); }
static const struct wl_buffer_interface buffer_interface = {buffer_destroy_request};
static void buffer_destroyed(struct wl_resource* resource) {
    struct dmabuf* buffer = wl_resource_get_user_data(resource);
    if (buffer->map) munmap(buffer->map, buffer->map_size);
    handle_release(buffer->handle); free(buffer);
}
static struct dmabuf* import(struct params* p, int32_t width, int32_t height, uint32_t format, uint32_t flags, uint32_t* error, const char** why) {
    *error = ERROR_INVALID_WL_BUFFER;
    if (p->used) { *error = ERROR_ALREADY_USED; *why = "params already used"; return NULL; }
    if (!p->set) { *error = ERROR_INCOMPLETE; *why = "plane 0 missing"; return NULL; }
    if (format != FORMAT_ARGB8888 && format != FORMAT_XRGB8888) { *error = ERROR_INVALID_FORMAT; *why = "unsupported format"; return NULL; }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) { *error = ERROR_INVALID_DIMENSIONS; *why = "invalid size"; return NULL; }
    if (flags || (p->modifier != MODIFIER_LINEAR && p->modifier != MODIFIER_INVALID)) { *why = "unsupported layout"; return NULL; }
    if ((uint64_t)p->stride < (uint64_t)width * 4 || (p->stride & 3) || (p->offset & 3)) { *error = ERROR_OUT_OF_BOUNDS; *why = "invalid stride"; return NULL; }
    off_t size = lseek(p->fd, 0, SEEK_END);
    if (size < 0 || (uint64_t)p->offset + (uint64_t)p->stride * (uint64_t)(height - 1) + (uint64_t)width * 4 > (uint64_t)size) {
        *error = ERROR_OUT_OF_BOUNDS; *why = "plane exceeds the dma-buf"; return NULL;
    }
    struct prime_handle prime = {0, 0, p->fd};
    if (drm(IOWR(0x2E, sizeof(prime)), &prime)) { *why = "dma-buf is not a virtio-gpu buffer"; return NULL; }
    if (!handle_retain(prime.handle)) { *why = "out of memory"; return NULL; }
    struct dmabuf* buffer = calloc(1, sizeof(*buffer));
    struct resource_info info = {prime.handle, 0, 0, 0};
    struct map_request map = {0, prime.handle, 0};
    if (!buffer || drm(IOWR(0x45, sizeof(info)), &info) || info.size < (uint64_t)size || drm(IOWR(0x41, sizeof(map)), &map)) {
        free(buffer); handle_release(prime.handle); *why = "cannot inspect the GPU buffer"; return NULL;
    }
    buffer->map_size = info.size;
    buffer->map = mmap(NULL, buffer->map_size, PROT_READ, MAP_SHARED, render_fd, (off_t)map.offset);
    if (buffer->map == MAP_FAILED) { free(buffer); handle_release(prime.handle); *why = "cannot map the GPU buffer"; return NULL; }
    buffer->handle = prime.handle; buffer->width = (uint32_t)width; buffer->height = (uint32_t)height; buffer->format = format;
    buffer->stride = p->stride; buffer->offset = p->offset;
    p->used = true; close(p->fd); p->fd = -1;
    if (logged < 4) { logged++; printf("[64] AURORA DMABUF %dx%d format=%08x stride=%u resource=%u\n", width, height, format, p->stride, info.res_handle); fflush(stdout); }
    return buffer;
}
static void params_create_common(struct wl_client* client, struct wl_resource* resource, uint32_t id, int32_t width, int32_t height,
                                 uint32_t format, uint32_t flags, bool immediate) {
    struct params* p = wl_resource_get_user_data(resource); uint32_t error; const char* why = "";
    struct dmabuf* buffer = import(p, width, height, format, flags, &error, &why);
    if (!buffer) {
        if (error == ERROR_INVALID_WL_BUFFER && !immediate) { p->used = true; wl_resource_post_event(resource, 1); return; }
        wl_resource_post_error(resource, error, "%s", why); return;
    }
    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer->resource) { munmap(buffer->map, buffer->map_size); handle_release(buffer->handle); free(buffer); wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(buffer->resource, &buffer_interface, buffer, buffer_destroyed);
    if (!immediate) wl_resource_post_event(resource, 0, buffer->resource);
}
static void params_destroy_request(struct wl_client* client, struct wl_resource* resource) { (void)client; wl_resource_destroy(resource); }
static void params_add(struct wl_client* client, struct wl_resource* resource, int32_t fd, uint32_t plane, uint32_t offset, uint32_t stride,
                       uint32_t modifier_hi, uint32_t modifier_lo) {
    (void)client; struct params* p = wl_resource_get_user_data(resource);
    if (p->used) { close(fd); wl_resource_post_error(resource, ERROR_ALREADY_USED, "params already used"); return; }
    if (plane > 3) { close(fd); wl_resource_post_error(resource, ERROR_PLANE_IDX, "plane index %u", plane); return; }
    if (plane > 0) { close(fd); wl_resource_post_error(resource, ERROR_INVALID_FORMAT, "multi-planar buffers are unsupported"); return; }
    if (p->set) { close(fd); wl_resource_post_error(resource, ERROR_PLANE_SET, "plane already set"); return; }
    p->fd = fd; p->offset = offset; p->stride = stride; p->modifier = (uint64_t)modifier_hi << 32 | modifier_lo; p->set = true;
}
static void params_create(struct wl_client* client, struct wl_resource* resource, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    params_create_common(client, resource, 0, width, height, format, flags, false);
}
static void params_create_immed(struct wl_client* client, struct wl_resource* resource, uint32_t id, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    params_create_common(client, resource, id, width, height, format, flags, true);
}
static const struct params_interface params_implementation = {params_destroy_request, params_add, params_create, params_create_immed};
static void params_destroyed(struct wl_resource* resource) {
    struct params* p = wl_resource_get_user_data(resource);
    if (p->fd >= 0) close(p->fd);
    free(p);
}
static void dmabuf_destroy_request(struct wl_client* client, struct wl_resource* resource) { (void)client; wl_resource_destroy(resource); }
static void dmabuf_create_params(struct wl_client* client, struct wl_resource* resource, uint32_t id) {
    struct params* p = calloc(1, sizeof(*p));
    struct wl_resource* params = p ? wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(resource), id) : NULL;
    if (!params) { free(p); wl_client_post_no_memory(client); return; }
    p->resource = params; p->fd = -1;
    wl_resource_set_implementation(params, &params_implementation, p, params_destroyed);
}
static const struct dmabuf_interface dmabuf_implementation = {dmabuf_destroy_request, dmabuf_create_params};
static void bind_dmabuf(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
    (void)data; struct wl_resource* resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &dmabuf_implementation, NULL, NULL);
    const uint32_t formats[] = {FORMAT_ARGB8888, FORMAT_XRGB8888};
    const uint64_t modifiers[] = {MODIFIER_LINEAR, MODIFIER_INVALID};
    for (unsigned i = 0; i < 2; i++) {
        wl_resource_post_event(resource, 0, formats[i]);
        if (version >= 3) for (unsigned m = 0; m < 2; m++) wl_resource_post_event(resource, 1, formats[i], (uint32_t)(modifiers[m] >> 32), (uint32_t)modifiers[m]);
    }
}

bool aurora_dmabuf_init(struct wl_display* display) {
    render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (render_fd < 0) return false;
    if (!wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 3, NULL, bind_dmabuf)) { close(render_fd); render_fd = -1; return false; }
    printf("[64] AURORA DMABUF zwp_linux_dmabuf_v1 on /dev/dri/renderD128\n"); fflush(stdout);
    return true;
}
static struct dmabuf* dmabuf_of(struct wl_resource* buffer) {
    return buffer && wl_resource_instance_of(buffer, &wl_buffer_interface, &buffer_interface) ? wl_resource_get_user_data(buffer) : NULL;
}
bool aurora_buffer_supported(struct wl_resource* buffer) { return wl_shm_buffer_get(buffer) || dmabuf_of(buffer); }
bool aurora_view_get(struct wl_resource* resource, struct aurora_view* view) {
    memset(view, 0, sizeof(*view));
    struct wl_shm_buffer* shm = resource ? wl_shm_buffer_get(resource) : NULL;
    if (shm) {
        view->shm = shm; view->width = wl_shm_buffer_get_width(shm); view->height = wl_shm_buffer_get_height(shm);
        view->stride = wl_shm_buffer_get_stride(shm); view->format = wl_shm_buffer_get_format(shm); return true;
    }
    struct dmabuf* buffer = dmabuf_of(resource); if (!buffer) return false;
    view->dmabuf = buffer; view->width = (int)buffer->width; view->height = (int)buffer->height; view->stride = (int)buffer->stride;
    view->format = buffer->format == FORMAT_ARGB8888 ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
    view->data = buffer->map + buffer->offset; return true;
}
bool aurora_view_begin(struct aurora_view* view, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (view->shm) { wl_shm_buffer_begin_access(view->shm); view->data = wl_shm_buffer_get_data(view->shm); return true; }
    struct dmabuf* buffer = view->dmabuf;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w > (int32_t)buffer->width - x) w = (int32_t)buffer->width - x;
    if (h > (int32_t)buffer->height - y) h = (int32_t)buffer->height - y;
    if (w <= 0 || h <= 0) return true;
    struct transfer t = {buffer->handle, (uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, 1, 0,
                         buffer->offset + (uint32_t)y * buffer->stride + (uint32_t)x * 4, 0, 0};
    struct wait_request wait = {buffer->handle, 0};
    if (drm(IOWR(0x46, sizeof(t)), &t) || drm(IOWR(0x48, sizeof(wait)), &wait)) {
        if (logged < 8) { logged++; printf("[64] AURORA DMABUF readback failed errno=%d\n", errno); fflush(stdout); }
        return false;
    }
    return true;
}
void aurora_view_end(struct aurora_view* view) { if (view->shm) wl_shm_buffer_end_access(view->shm); }
