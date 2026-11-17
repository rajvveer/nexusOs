/* Aurora: zwp_linux_dmabuf_v1 buffers from GPU clients (Chrome's GPU process), read back through the
 * virtio-gpu render node, and one accessor for wl_shm and dma-buf wl_buffers. */
#ifndef AURORA_DMABUF_H
#define AURORA_DMABUF_H
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server.h>

struct aurora_view {
    int width, height, stride;
    uint32_t format;            /* WL_SHM_FORMAT_ARGB8888 or WL_SHM_FORMAT_XRGB8888 (same memory layout for dma-bufs) */
    const uint8_t* data;
    struct wl_shm_buffer* shm;
    void* dmabuf;
};
bool aurora_dmabuf_init(struct wl_display* display);            /* advertises the global when a render node exists */
bool aurora_buffer_supported(struct wl_resource* buffer);        /* wl_shm or dma-buf wl_buffer */
bool aurora_view_get(struct wl_resource* buffer, struct aurora_view* view);
/* Make the pixels of the area x,y,w,h (buffer coordinates) current in view->data; false if the GPU readback failed. */
bool aurora_view_begin(struct aurora_view* view, int32_t x, int32_t y, int32_t w, int32_t h);
void aurora_view_end(struct aurora_view* view);
#endif
