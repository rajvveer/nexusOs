/* ============================================================================
 * NexusOS — VirtIO-GPU Driver + 2D Acceleration (Header) — Phase 41
 * ============================================================================
 * Drives the QEMU VirtIO-GPU (-device virtio-vga) over the virtio transport:
 * a host-side framebuffer resource is created and backed *directly* by the
 * kernel back buffer (zero-copy), so presenting a frame is just a
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH of the dirty rectangle instead of a
 * 3 MB memcpy. Also provides rep-string accelerated fill/blit primitives
 * for the desktop and the Phase 41 sprite engine.
 * ============================================================================ */

#ifndef GPU_H
#define GPU_H

#include "types.h"

typedef struct {
    bool     active;          /* virtio scanout owns the display          */
    int      width, height;   /* scanout resource size                    */
    int      pref_width, pref_height; /* host's preferred mode            */
    int      num_scanouts;
    uint16_t queue_size;      /* controlq entries                         */
    uint32_t features_lo, features_hi;
    uint32_t presents;        /* flush commands issued so far             */
    uint32_t backing_addr;    /* guest phys of the zero-copy backing      */
} gpu_info_t;

/* Probe PCI for a VirtIO-GPU, take over scanout 0 (falls back silently to
 * the VESA framebuffer when absent). Call after pci/heap/fb init. */
void gpu_init(void);

bool gpu_active(void);

/* Present the whole back buffer via the GPU. Returns false when the GPU is
 * inactive (caller should fall back to the legacy VESA copy). */
bool gpu_flip(void);

/* Present only a dirty rectangle of the back buffer (clipped). */
void gpu_present(int x, int y, int w, int h);

/* Accelerated 2D ops on the back buffer (rep stosl / rep movsl). */
void gpu_fill(int x, int y, int w, int h, uint32_t color);
void gpu_blit(int dx, int dy, int sx, int sy, int w, int h);

bool gpu_get_info(gpu_info_t* out);
const char* gpu_status(void);

#endif /* GPU_H */
