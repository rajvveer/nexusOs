/* Native GPU stack: virtio-gpu transport (virtio_gpu.c), DRM render/primary nodes, dma-buf and
 * sync_file objects (drm.c) and the sysfs tree libdrm uses to discover them (sysfs.c). */
#ifndef NEXUS_GPU_H
#define NEXUS_GPU_H
#include "native.h"

/* ---- virtio_gpu.c: virtio 1.x modern PCI transport ---- */
struct gpu_command {
    const void* command; uint32_t command_bytes;   /* request structure starting with virtio_gpu_ctrl_hdr */
    const void* data; uint64_t user_data; uint32_t data_bytes; /* trailing payload: kernel buffer or user address */
    void* response; uint32_t response_bytes;       /* filled only when wait is set */
    bool fence;                                    /* VIRTIO_GPU_FLAG_FENCE with the next fence id */
    bool wait;                                     /* block until the device answered */
    uint64_t fence_id;                             /* out: fence id assigned to a fenced command */
    uint32_t response_type;                        /* out (wait): VIRTIO_GPU_RESP_* */
};
bool virtio_gpu_init(void);
bool virtio_gpu_present(void);                     /* initialised with virgl 3D */
void virtio_gpu_poll(void);                         /* reap completions; called from the BSP device polls */
void virtio_gpu_drain(void);                        /* wait for every queued command and fence */
void virtio_gpu_fence_interest(void);               /* a waiter exists: retire host fences promptly */
bool virtio_gpu_busy(void);                         /* a fence waiter exists: poll every tick */
int virtio_gpu_command(struct gpu_command* command);
bool virtio_gpu_feature(unsigned bit);
uint64_t virtio_gpu_fence_completed(void);
uint64_t virtio_gpu_fence_issued(void);
int virtio_gpu_fence_wait(uint64_t fence, uint64_t deadline, bool interruptible);
unsigned virtio_gpu_capset_count(void);
bool virtio_gpu_capset(unsigned index, uint32_t* id, uint32_t* max_version, uint32_t* max_size);
int virtio_gpu_capset_data(uint32_t id, uint32_t version, const void** data, uint32_t* size);
uint32_t virtio_gpu_resource_id(void);
void virtio_gpu_resource_id_free(uint32_t id);
uint32_t virtio_gpu_pci_location(void);            /* bus << 8 | device << 3 | function */
void virtio_gpu_pci_ids(uint16_t ids[5]);          /* vendor, device, subsystem vendor, subsystem device, revision */
void virtio_gpu_pci_config(uint8_t config[256]);
uint64_t virtio_gpu_statistic(unsigned which);     /* 0 commands, 1 fences, 2 kicks, 3 errors */
bool virtio_gpu_ready(void);                       /* the device is driven (2D scanouts at least; virgl: present) */
unsigned virtio_gpu_scanouts(void);                /* virtio_gpu_config.num_scanouts (<= 16), 0 without a device */
bool virtio_gpu_vga(void);                         /* the device is VGA-compatible (virtio-vga: the boot framebuffer is its VRAM) */

/* ---- kms.c: modesetting on the virtio-gpu scanouts (legacy KMS uapi on card0) ---- */
#define DRM_CLIENTS 256U
#define KMS_HANDLE_BASE 0x01000000U                /* dumb-buffer GEM handles; virgl handles stay below */
#define KMS_MAP_BASE 0x10000ULL                    /* MAP_DUMB offsets: (KMS_MAP_BASE + dumb) << 32 */
void kms_init(void);                               /* drm_init: probe the scanouts; tables are permanent */
bool kms_available(void);
unsigned kms_live_objects(void);                   /* dumb buffers + framebuffers besides the boot framebuffer */
void kms_display_event(void);                      /* virtio_gpu_poll: VIRTIO_GPU_EVENT_DISPLAY (BSP poll context) */
void kms_resume(void);                             /* virtio_gpu_pm resume (tasks frozen): host resources + scanouts again */
void kms_open(unsigned client, unsigned node);     /* drm_open: the first primary-node opener becomes master */
void kms_close(unsigned client);                   /* client_finish: its framebuffers, dumb buffers, events, mastership */
int kms_ioctl(unsigned client, unsigned node, uint32_t nr, void* data);  /* 0x1E/0x1F, 0x3A, 0xA0-0xCF; data in/out (<= 128 bytes) */
int kms_get_cap(uint64_t capability, uint64_t* value);   /* -95 when not a KMS capability */
int kms_set_client_cap(unsigned client, uint64_t capability, uint64_t value);
bool kms_handle(uint32_t handle);
int kms_handle_close(unsigned client, uint32_t handle);
struct memory_file* kms_mapping(unsigned client, uint64_t* offset, uint64_t length);
uint32_t kms_ready(unsigned client);               /* POLLIN when DRM events (page flips) are queued */
void* kms_event_source(void);
int64_t kms_read(unsigned client, uint64_t buffer, uint64_t count, uint64_t flags);
/* netlink.c: a kernel uevent (kobject_uevent) to every NETLINK_KOBJECT_UEVENT socket of group 1 */
void netlink_uevent(const char* message, unsigned size);

/* ---- drm.c: DRM uapi on /dev/dri/card0 and /dev/dri/renderD128 ---- */
enum { DRM_NODE_CARD, DRM_NODE_RENDER, DRM_NODE_DMABUF, DRM_NODE_SYNC };
void drm_init(void);
void drm_drain(void);
bool drm_available(void);                          /* /dev/dri/card0 (KMS and/or virgl) */
bool drm_render_available(void);                   /* /dev/dri/renderD128 (virgl 3D) */
int drm_open_check(void);
int drm_open(unsigned node, uint64_t flags, struct input_reader* reader);
void drm_close(unsigned node, struct input_reader* reader);
int drm_ioctl(unsigned node, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags);
struct memory_file* drm_mapping(unsigned node, uint64_t* offset, uint64_t length, struct input_reader* reader);
uint32_t drm_ready(unsigned node, const struct input_reader* reader);
void* drm_event_source(unsigned node, const struct input_reader* reader);
void drm_stat(unsigned node, const struct input_reader* reader, struct linux_stat* stat);
int64_t drm_io(unsigned node, uint64_t buffer, uint64_t count, bool write, uint64_t flags, struct input_reader* reader);
int64_t drm_seek(unsigned node, uint64_t* position, int64_t offset, uint64_t whence, struct input_reader* reader);
void drm_fences_signaled(void);                    /* virtio_gpu.c: the completed fence advanced */
unsigned drm_live_objects(void);

/* ---- kernel.c: descriptors backed by device objects (dma-buf, sync_file) ---- */
int fd_create_device(unsigned device, uint64_t cookie, uint64_t flags, const char* path);
struct input_reader* fd_device_reader(struct open_file* description, unsigned* device);
int device_index(unsigned node);                   /* device.c: device table index of a DRM node */
int device_node(unsigned device);                  /* device.c: DRM node of a device table index, or -1 */

/* ---- sysfs.c: /sys served through the procfs handle interface ---- */
bool sysfs_path(const char* path);
void sysfs_init(void);
int sysfs_resolve(const char* path, bool follow, uint32_t* node);
bool sysfs_directory(uint32_t node);
bool sysfs_link(uint32_t node);
int sysfs_stat(uint32_t node, struct linux_stat* stat);
int64_t sysfs_read(uint32_t node, uint64_t address, uint64_t count, uint64_t offset);
int sysfs_entry(uint32_t node, uint64_t position, struct directory_item* item);
int sysfs_readlink(uint32_t node, char* result, unsigned capacity);
int sysfs_node_path(uint32_t node, char* result, unsigned capacity);
/* Nodes served by another subsystem (power.c: power_sysfs_read/write by id): type 0 directory, 1 read-only
 * attribute, 2 read-write attribute (mode 0644), 3 symbolic link to `target`. Parents must exist first. */
int sysfs_add(const char* path, unsigned type, unsigned id, const char* target);
bool sysfs_writable(uint32_t node);
int64_t sysfs_write(uint32_t node, uint64_t address, uint64_t count);
#endif
