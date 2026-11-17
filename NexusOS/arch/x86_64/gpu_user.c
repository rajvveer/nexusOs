/* virtio-gpu / DRM render node test (run by the kernel's /nexus/test-gpu campaign).
   Discovery goes through the unmodified Debian libdrm (dlopen'ed: drmGetDevices2, drmGetDevice2,
   drmGetVersion) and the sysfs files it reads; rendering uses the raw virtgpu uapi the way Mesa's virgl
   winsys does: GETPARAM, GET_CAPS, CONTEXT_INIT, RESOURCE_CREATE, MAP+mmap, EXECBUFFER (virgl clear and
   copy commands, in/out fence fds), TRANSFER_TO/FROM_HOST, WAIT, PRIME export/import of dma-bufs (mmap,
   llseek, poll, SCM_RIGHTS, DMA_BUF_IOCTL_SYNC) and sync_file poll/merge/info. */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IOWR(nr, size) (0xC0000000U | ((uint32_t)(size) << 16) | ('d' << 8) | (nr))
#define IOW(nr, size) (0x40000000U | ((uint32_t)(size) << 16) | ('d' << 8) | (nr))
struct drm_version { int major, minor, patch; size_t name_len; char* name; size_t date_len; char* date; size_t desc_len; char* desc; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };
struct virtgpu_map { uint64_t offset; uint32_t handle, pad; };
struct virtgpu_execbuffer { uint32_t flags, size; uint64_t command, bo_handles; uint32_t num_bo_handles; int32_t fence_fd;
                            uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs; uint64_t in_syncobjs, out_syncobjs; };
struct virtgpu_getparam { uint64_t param, value; };
struct virtgpu_resource_create { uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags,
                                 bo_handle, res_handle, size, stride; };
struct virtgpu_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct virtgpu_box { uint32_t x, y, z, w, h, d; };
struct virtgpu_transfer { uint32_t bo_handle; struct virtgpu_box box; uint32_t level, offset, stride, layer_stride; };
struct virtgpu_wait { uint32_t handle, flags; };
struct virtgpu_get_caps { uint32_t cap_set_id, cap_set_ver; uint64_t addr; uint32_t size, pad; };
struct virtgpu_context_init { uint32_t num_params, pad; uint64_t ctx_set_params; };
struct virtgpu_context_param { uint64_t param, value; };
struct sync_merge_data { char name[32]; int32_t fd2, fence; uint32_t flags, pad; };
struct sync_file_info { char name[32]; int32_t status; uint32_t flags, num_fences, pad; uint64_t sync_fence_info; };
struct sync_fence_info { char obj_name[32], driver_name[32]; int32_t status; uint32_t flags; uint64_t timestamp_ns; };
#define DRM_VERSION IOWR(0x00, sizeof(struct drm_version))
#define DRM_GEM_CLOSE IOW(0x09, sizeof(struct drm_gem_close))
#define DRM_GET_CAP IOWR(0x0C, sizeof(struct drm_get_cap))
#define DRM_PRIME_TO_FD IOWR(0x2D, sizeof(struct drm_prime_handle))
#define DRM_PRIME_TO_HANDLE IOWR(0x2E, sizeof(struct drm_prime_handle))
#define DRM_MODE_GETRESOURCES IOWR(0xA0, 64)
#define VIRTGPU_MAP IOWR(0x41, sizeof(struct virtgpu_map))
#define VIRTGPU_EXECBUFFER IOWR(0x42, sizeof(struct virtgpu_execbuffer))
#define VIRTGPU_EXECBUFFER_V1 IOWR(0x42, 32)
#define VIRTGPU_GETPARAM IOWR(0x43, sizeof(struct virtgpu_getparam))
#define VIRTGPU_RESOURCE_CREATE IOWR(0x44, sizeof(struct virtgpu_resource_create))
#define VIRTGPU_RESOURCE_INFO IOWR(0x45, sizeof(struct virtgpu_resource_info))
#define VIRTGPU_FROM_HOST IOWR(0x46, sizeof(struct virtgpu_transfer))
#define VIRTGPU_TO_HOST IOWR(0x47, sizeof(struct virtgpu_transfer))
#define VIRTGPU_WAIT IOWR(0x48, sizeof(struct virtgpu_wait))
#define VIRTGPU_GET_CAPS IOWR(0x49, sizeof(struct virtgpu_get_caps))
#define VIRTGPU_CONTEXT_INIT IOWR(0x4B, sizeof(struct virtgpu_context_init))
#define DMA_BUF_SYNC 0x40086200U
#define SYNC_MERGE 0xC0303E03U
#define SYNC_FILE_INFO 0xC0383E04U
/* virgl protocol (virglrenderer src/virgl_protocol.h) */
#define CMD0(cmd, obj, len) ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))
enum { CCMD_CREATE_OBJECT = 1, CCMD_DESTROY_OBJECT = 3, CCMD_SET_FRAMEBUFFER_STATE = 5, CCMD_CLEAR = 7, CCMD_RESOURCE_COPY_REGION = 17 };
enum { OBJECT_SURFACE = 8, FORMAT_B8G8R8A8_UNORM = 1, TARGET_TEXTURE_2D = 2, BIND_RENDER_TARGET = 1 << 1, BIND_SAMPLER_VIEW = 1 << 3 };
#define W 64
#define H 64

static int failures;
#define CHECK(test, ...) do { if (!(test)) { printf("[64] GPU FAIL line=%d errno=%d: ", __LINE__, errno); printf(__VA_ARGS__); printf("\n"); fflush(stdout); failures++; } } while (0)
static void pass(const char* what) { printf("[64] GPU PASS %s\n", what); fflush(stdout); }

typedef struct { uint16_t domain; uint8_t bus, dev, func; } drm_pci_bus;
typedef struct { uint16_t vendor_id, device_id, subvendor_id, subdevice_id; uint8_t revision_id; } drm_pci_device;
typedef struct { char** nodes; int available_nodes, bustype; union { drm_pci_bus* pci; void* other; } businfo; union { drm_pci_device* pci; void* other; } deviceinfo; } drm_device;
typedef struct { int major, minor, patch, name_len; char* name; int date_len; char* date; int desc_len; char* desc; } drm_version_info;

static void discovery(void) {
    char link[PATH_MAX] = {0}, real[PATH_MAX] = {0}, line[256];
    ssize_t n = readlink("/sys/dev/char/226:128/device/subsystem", link, sizeof(link) - 1);
    CHECK(n > 0 && !strcmp(strrchr(link, '/'), "/virtio"), "virtio subsystem link '%s'", link);
    struct stat st;
    CHECK(stat("/sys/dev/char/226:128/device/drm", &st) == 0 && S_ISDIR(st.st_mode), "drm directory below the device");
    CHECK(realpath("/sys/dev/char/226:128/device", real) && strstr(real, "/virtio0") && !strncmp(real, "/sys/devices/pci0000:00/", 24), "realpath %s", real);
    char* virtio = strrchr(real, '/'); if (virtio) *virtio = 0;
    char path[PATH_MAX + 32]; snprintf(path, sizeof(path), "%s/uevent", real);
    FILE* f = fopen(path, "r"); bool slot = false;
    while (f && fgets(line, sizeof(line), f)) if (!strncmp(line, "PCI_SLOT_NAME=0000:", 19)) slot = true;
    if (f) fclose(f);
    CHECK(slot, "PCI_SLOT_NAME in %s", path);
    unsigned vendor = 0; snprintf(path, sizeof(path), "%s/vendor", real); f = fopen(path, "r");
    CHECK(f && fscanf(f, "%x", &vendor) == 1 && vendor == 0x1af4, "vendor %x", vendor); if (f) fclose(f);
    DIR* d = opendir("/sys/dev/char/226:128/device/drm"); bool render = false, card = false;
    for (struct dirent* e; d && (e = readdir(d));) { render |= !strcmp(e->d_name, "renderD128"); card |= !strcmp(e->d_name, "card0"); }
    if (d) closedir(d);
    CHECK(render && card, "drm minors listed");
    CHECK(stat("/dev/dri/renderD128", &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 226 && minor(st.st_rdev) == 128, "render node rdev");
    void* drm = dlopen("libdrm.so.2", RTLD_NOW);
    CHECK(drm, "libdrm: %s", dlerror());
    if (!drm) return;
    int (*get_devices)(uint32_t, drm_device**, int) = (int (*)(uint32_t, drm_device**, int))dlsym(drm, "drmGetDevices2");
    int (*get_device)(int, uint32_t, drm_device**) = (int (*)(int, uint32_t, drm_device**))dlsym(drm, "drmGetDevice2");
    void (*free_devices)(drm_device**, int) = (void (*)(drm_device**, int))dlsym(drm, "drmFreeDevices");
    void (*free_device)(drm_device**) = (void (*)(drm_device**))dlsym(drm, "drmFreeDevice");
    drm_version_info* (*get_version)(int) = (drm_version_info* (*)(int))dlsym(drm, "drmGetVersion");
    void (*free_version)(drm_version_info*) = (void (*)(drm_version_info*))dlsym(drm, "drmFreeVersion");
    char* (*render_name)(int) = (char* (*)(int))dlsym(drm, "drmGetRenderDeviceNameFromFd");
    drm_device* devices[8]; int count = get_devices(0, devices, 8);
    CHECK(count == 1, "drmGetDevices2 count %d", count);
    if (count >= 1) {
        drm_device* dev = devices[0];
        CHECK((dev->available_nodes & 5) == 5 && !strcmp(dev->nodes[2], "/dev/dri/renderD128") && !strcmp(dev->nodes[0], "/dev/dri/card0"), "nodes");
        CHECK(dev->bustype == 0 && dev->deviceinfo.pci->vendor_id == 0x1af4 && dev->deviceinfo.pci->device_id == 0x1050, "pci ids %x:%x",
              dev->deviceinfo.pci->vendor_id, dev->deviceinfo.pci->device_id);
        free_devices(devices, count);
    }
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    drm_device* one = NULL;
    CHECK(fd >= 0 && get_device(fd, 0, &one) == 0 && one && one->deviceinfo.pci->vendor_id == 0x1af4, "drmGetDevice2");
    if (one) free_device(&one);
    drm_version_info* version = get_version(fd);
    CHECK(version && !strcmp(version->name, "virtio_gpu") && version->major == 0 && version->minor == 1, "drmGetVersion");
    if (version) free_version(version);
    char* name = render_name(fd);
    CHECK(name && !strcmp(name, "/dev/dri/renderD128"), "drmGetRenderDeviceNameFromFd %s", name ? name : "(null)");
    free(name); close(fd);
    if (!failures) pass("discovery: libdrm drmGetDevices2/drmGetDevice2/drmGetVersion over /sys and /dev/dri");
}

static int drm_ioctl(int fd, unsigned long request, void* argument) {
    int r; do r = ioctl(fd, request, argument); while (r == -1 && (errno == EINTR || errno == EAGAIN)); return r;
}
static uint64_t param(int fd, uint64_t which) {
    uint64_t value = 0; struct virtgpu_getparam p = {which, (uint64_t)(uintptr_t)&value};
    return drm_ioctl(fd, VIRTGPU_GETPARAM, &p) ? ~0ULL : value;
}
static uint32_t resource(int fd, uint32_t* res, uint32_t bind) {
    struct virtgpu_resource_create rc = {.target = TARGET_TEXTURE_2D, .format = FORMAT_B8G8R8A8_UNORM, .bind = bind,
        .width = W, .height = H, .depth = 1, .array_size = 1, .size = W * H * 4, .stride = W * 4};
    if (drm_ioctl(fd, VIRTGPU_RESOURCE_CREATE, &rc)) return 0;
    if (res) *res = rc.res_handle;
    return rc.bo_handle;
}
static uint32_t* map_bo(int fd, uint32_t handle) {
    struct virtgpu_map m = {.handle = handle};
    if (drm_ioctl(fd, VIRTGPU_MAP, &m)) return NULL;
    void* p = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
    return p == MAP_FAILED ? NULL : p;
}
static int transfer(int fd, uint32_t handle, bool to_host) {
    struct virtgpu_transfer t = {.bo_handle = handle, .box = {0, 0, 0, W, H, 1}};
    return drm_ioctl(fd, to_host ? VIRTGPU_TO_HOST : VIRTGPU_FROM_HOST, &t);
}
static int wait_bo(int fd, uint32_t handle, bool nowait) { struct virtgpu_wait w = {handle, nowait ? 1 : 0}; return drm_ioctl(fd, VIRTGPU_WAIT, &w); }
static int submit(int fd, const uint32_t* commands, unsigned dwords, const uint32_t* handles, unsigned count, int in_fence, int* out_fence) {
    struct virtgpu_execbuffer e = {.size = dwords * 4, .command = (uint64_t)(uintptr_t)commands, .bo_handles = (uint64_t)(uintptr_t)handles,
                                   .num_bo_handles = count, .fence_fd = in_fence};
    if (in_fence >= 0) e.flags |= 1;
    if (out_fence) e.flags |= 2;
    int r = drm_ioctl(fd, VIRTGPU_EXECBUFFER, &e);
    if (!r && out_fence) *out_fence = e.fence_fd;
    return r;
}
static bool pixel_near(uint32_t value, uint32_t expected) {
    for (int shift = 0; shift < 32; shift += 8) { int a = (value >> shift) & 255, b = (expected >> shift) & 255; if (a - b > 1 || b - a > 1) return false; }
    return true;
}
static unsigned clear_commands(uint32_t* c, uint32_t surface, uint32_t res, float r, float g, float b, float a) {
    unsigned n = 0; union { float f; uint32_t u; } v;
    c[n++] = CMD0(CCMD_CREATE_OBJECT, OBJECT_SURFACE, 5); c[n++] = surface; c[n++] = res; c[n++] = FORMAT_B8G8R8A8_UNORM; c[n++] = 0; c[n++] = 0;
    c[n++] = CMD0(CCMD_SET_FRAMEBUFFER_STATE, 0, 3); c[n++] = 1; c[n++] = 0; c[n++] = surface;
    c[n++] = CMD0(CCMD_CLEAR, 0, 8); c[n++] = 1 << 2;
    v.f = r; c[n++] = v.u; v.f = g; c[n++] = v.u; v.f = b; c[n++] = v.u; v.f = a; c[n++] = v.u;
    c[n++] = 0; c[n++] = 0; c[n++] = 0;
    return n;
}

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
/* Round-trip latency of the paths Chrome's frames take: a fenced submit waited through its sync_file,
   and a readback (TRANSFER_FROM_HOST + WAIT) as Aurora does per committed frame. */
static void latency(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC); uint32_t res = 0, bo = fd >= 0 ? resource(fd, &res, BIND_RENDER_TARGET) : 0;
    CHECK(bo, "latency resource"); if (!bo) { if (fd >= 0) close(fd); return; }
    uint32_t commands[32]; unsigned n = clear_commands(commands, 9, res, 1, 0, 0, 1);
    double start = now_us(), worst = 0; unsigned rounds = 200;
    for (unsigned i = 0; i < rounds; i++) {
        double a = now_us(); int fence = -1;
        if (submit(fd, commands, n, &bo, 1, -1, &fence)) { CHECK(0, "latency submit"); break; }
        struct pollfd p = {fence, POLLIN, 0}; poll(&p, 1, 5000); close(fence);
        double d = now_us() - a; if (d > worst) worst = d;
    }
    double fence_us = (now_us() - start) / rounds; start = now_us();
    for (unsigned i = 0; i < rounds; i++) if (transfer(fd, bo, false) || wait_bo(fd, bo, false)) { CHECK(0, "latency readback"); break; }
    double readback_us = (now_us() - start) / rounds;
    printf("[64] GPU LATENCY fence_roundtrip_us=%.0f worst_us=%.0f readback_64x64_us=%.0f\n", fence_us, worst, readback_us); fflush(stdout);
    close(fd);
}
static void render(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0, "open render node"); if (fd < 0) return;
    struct drm_version v = {0}; char name[32] = {0};
    v.name = name; v.name_len = sizeof(name) - 1;
    CHECK(!drm_ioctl(fd, DRM_VERSION, &v) && !strcmp(name, "virtio_gpu") && v.name_len == 10, "VERSION");
    struct drm_get_cap cap = {5, 0};
    CHECK(!drm_ioctl(fd, DRM_GET_CAP, &cap) && cap.value == 3, "PRIME cap %llu", (unsigned long long)cap.value);
    CHECK(param(fd, 1) == 1 && param(fd, 2) == 1, "3D features / capset fix");
    uint64_t capsets = param(fd, 7), context_init = param(fd, 6);
    CHECK(capsets != ~0ULL && (capsets & 6), "capset mask %llx", (unsigned long long)capsets);
    uint32_t caps[1024] = {0}; uint32_t set = capsets & 4 ? 2 : 1;
    struct virtgpu_get_caps g = {set, 0, (uint64_t)(uintptr_t)caps, sizeof(caps), 0};
    CHECK(!drm_ioctl(fd, VIRTGPU_GET_CAPS, &g) && caps[0] >= 1, "GET_CAPS set %u max_version %u", set, caps[0]);
    if (context_init == 1) {
        struct virtgpu_context_param p = {1, set};
        struct virtgpu_context_init init = {1, 0, (uint64_t)(uintptr_t)&p};
        CHECK(!drm_ioctl(fd, VIRTGPU_CONTEXT_INIT, &init), "CONTEXT_INIT capset %u", set);
        CHECK(drm_ioctl(fd, VIRTGPU_CONTEXT_INIT, &init) == -1 && errno == EEXIST, "second CONTEXT_INIT");
    }
    if (!failures) pass("GETPARAM, GET_CAPS and CONTEXT_INIT");

    /* Render target cleared by the host GPU, read back through TRANSFER_FROM_HOST into the mapping. */
    uint32_t res_a = 0, a = resource(fd, &res_a, BIND_RENDER_TARGET | BIND_SAMPLER_VIEW);
    CHECK(a && res_a, "RESOURCE_CREATE");
    struct virtgpu_resource_info info = {.bo_handle = a};
    CHECK(!drm_ioctl(fd, VIRTGPU_RESOURCE_INFO, &info) && info.res_handle == res_a && info.size == W * H * 4, "RESOURCE_INFO");
    uint32_t* pixels = map_bo(fd, a);
    CHECK(pixels, "MAP + mmap");
    uint32_t commands[64]; int fence = -1;
    unsigned n = clear_commands(commands, 1, res_a, 0.25f, 0.5f, 0.75f, 1.0f);
    CHECK(!submit(fd, commands, n, &a, 1, -1, &fence) && fence >= 0, "EXECBUFFER clear with out-fence");
    struct pollfd p = {fence, POLLIN, 0};
    CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN), "sync_file poll");
    struct sync_fence_info fi; struct sync_file_info si = {.num_fences = 1, .sync_fence_info = (uint64_t)(uintptr_t)&fi};
    CHECK(!ioctl(fence, SYNC_FILE_INFO, &si) && si.status == 1 && fi.status == 1 && fi.timestamp_ns && !strcmp(fi.driver_name, "virtio_gpu"), "SYNC_IOC_FILE_INFO");
    CHECK(!transfer(fd, a, false) && !wait_bo(fd, a, false), "TRANSFER_FROM_HOST + WAIT");
    bool cleared = pixels != NULL;
    for (unsigned i = 0; pixels && i < W * H; i++) if (!pixel_near(pixels[i], 0xFF4080BF)) { cleared = false; printf("[64] GPU pixel %u = %08x\n", i, pixels[i]); break; }
    CHECK(cleared, "host-rendered clear color in guest memory");
    if (!failures) pass("virgl clear rendered on the host GPU and read back (RESOURCE_CREATE, EXECBUFFER, TRANSFER_FROM_HOST, WAIT)");

    /* Upload a pattern, copy it on the GPU into a second texture, read that back. */
    uint32_t res_b = 0, b = resource(fd, &res_b, BIND_RENDER_TARGET | BIND_SAMPLER_VIEW);
    uint32_t* other = map_bo(fd, b);
    CHECK(b && other, "second resource");
    for (unsigned i = 0; pixels && i < W * H; i++) pixels[i] = 0xFF000000U | (i * 2654435761U >> 8);
    CHECK(!transfer(fd, a, true), "TRANSFER_TO_HOST");
    unsigned m = 0; uint32_t copy[16];
    copy[m++] = CMD0(CCMD_RESOURCE_COPY_REGION, 0, 13); copy[m++] = res_b; copy[m++] = 0; copy[m++] = 0; copy[m++] = 0; copy[m++] = 0;
    copy[m++] = res_a; copy[m++] = 0; copy[m++] = 0; copy[m++] = 0; copy[m++] = 0; copy[m++] = W; copy[m++] = H; copy[m++] = 1;
    uint32_t both[2] = {a, b};
    int copy_fence = -1;
    CHECK(!submit(fd, copy, m, both, 2, fence, &copy_fence), "EXECBUFFER copy with in- and out-fence");
    struct sync_merge_data merge = {.fd2 = copy_fence}; strcpy(merge.name, "merged");
    CHECK(!ioctl(fence, SYNC_MERGE, &merge) && merge.fence >= 0, "SYNC_IOC_MERGE");
    p = (struct pollfd){merge.fence, POLLIN, 0};
    CHECK(poll(&p, 1, 5000) == 1, "merged fence signals");
    CHECK(!transfer(fd, b, false), "TRANSFER_FROM_HOST b");
    int busy = wait_bo(fd, b, true); CHECK(!busy || errno == EBUSY, "WAIT NOWAIT");
    CHECK(!wait_bo(fd, b, false) && !wait_bo(fd, b, true), "WAIT then idle");
    bool copied = other && pixels && !memcmp(other, pixels, W * H * 4);
    CHECK(copied, "GPU copy matches the uploaded pattern");
    /* MADV_REMOVE must not punch a bo the host uses as backing (Linux: ENODEV, a device has no fallocate);
       the mapping must still alias the frames that TRANSFER_FROM_HOST writes. */
    errno = 0; CHECK(other && madvise(other, W * H * 4, MADV_REMOVE) == -1 && errno == ENODEV, "MADV_REMOVE on a bo mapping: %s", strerror(errno));
    if (other) memset(other, 0, W * H * 4);
    CHECK(!transfer(fd, b, false) && !wait_bo(fd, b, false) && other && pixels && !memcmp(other, pixels, W * H * 4), "bo backing intact after MADV_REMOVE");
    close(fence); close(copy_fence); close(merge.fence);
    /* The older 32-byte EXECBUFFER layout is accepted as well. */
    struct virtgpu_execbuffer old = {.size = 0}; CHECK(!drm_ioctl(fd, VIRTGPU_EXECBUFFER_V1, &old), "32-byte EXECBUFFER");
    if (!failures) pass("TRANSFER_TO_HOST, GPU copy, fence merge/in-fence, WAIT NOWAIT");

    /* dma-buf: export, mmap, llseek, poll, DMA_BUF_IOCTL_SYNC, SCM_RIGHTS, import into a second context. */
    struct drm_prime_handle prime = {.handle = b, .flags = O_CLOEXEC | O_RDWR};
    CHECK(!drm_ioctl(fd, DRM_PRIME_TO_FD, &prime) && prime.fd >= 0, "PRIME_HANDLE_TO_FD");
    CHECK(lseek(prime.fd, 0, SEEK_END) == W * H * 4 && lseek(prime.fd, 0, SEEK_SET) == 0, "dma-buf llseek size");
    uint64_t sync = 1 | 0; CHECK(!ioctl(prime.fd, DMA_BUF_SYNC, &sync), "DMA_BUF_IOCTL_SYNC start");
    uint32_t* shared = mmap(NULL, W * H * 4, PROT_READ, MAP_SHARED, prime.fd, 0);
    CHECK(shared != MAP_FAILED && pixels && !memcmp(shared, pixels, W * H * 4), "dma-buf mmap contents");
    sync = 1 | 4; CHECK(!ioctl(prime.fd, DMA_BUF_SYNC, &sync), "DMA_BUF_IOCTL_SYNC end");
    p = (struct pollfd){prime.fd, POLLIN | POLLOUT, 0}; CHECK(poll(&p, 1, 1000) == 1, "dma-buf poll");
    struct drm_prime_handle again = {.fd = prime.fd};
    CHECK(!drm_ioctl(fd, DRM_PRIME_TO_HANDLE, &again) && again.handle == b, "self import returns the same handle");
    int pair[2]; CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "socketpair");
    char byte = 'x'; char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {&byte, 1}; struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &prime.fd, sizeof(int));
    CHECK(sendmsg(pair[0], &msg, 0) == 1, "send dma-buf");
    memset(control, 0, sizeof(control)); msg.msg_controllen = sizeof(control);
    CHECK(recvmsg(pair[1], &msg, 0) == 1 && (cm = CMSG_FIRSTHDR(&msg)) && cm->cmsg_type == SCM_RIGHTS, "receive dma-buf");
    int received = -1; if (cm) memcpy(&received, CMSG_DATA(cm), sizeof(int));
    struct stat s1, s2; CHECK(!fstat(prime.fd, &s1) && !fstat(received, &s2) && s1.st_ino == s2.st_ino && s1.st_size == W * H * 4, "dma-buf identity");
    pid_t child = fork();
    if (child == 0) {
        int fd2 = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        struct drm_prime_handle import = {.fd = received};
        int ok = fd2 >= 0 && !drm_ioctl(fd2, DRM_PRIME_TO_HANDLE, &import) && import.handle;
        struct virtgpu_resource_info ri = {.bo_handle = import.handle};
        ok = ok && !drm_ioctl(fd2, VIRTGPU_RESOURCE_INFO, &ri) && ri.res_handle == res_b;
        uint32_t* view = ok ? map_bo(fd2, import.handle) : NULL;
        if (view) memset(view, 0, W * H * 4);
        ok = ok && view && !transfer(fd2, import.handle, false) && !wait_bo(fd2, import.handle, false);
        uint32_t expect = 0xFF000000U | (77 * 2654435761U >> 8);
        ok = ok && view[77] == expect;
        _exit(ok ? 0 : 1);
    }
    int status = -1; CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child import status %x", status);
    close(pair[0]); close(pair[1]); close(received); if (shared != MAP_FAILED) munmap(shared, W * H * 4); close(prime.fd);
    if (!failures) pass("dma-buf export, mmap, llseek, poll, SCM_RIGHTS and import in another process");

    struct drm_gem_close gc = {a, 0};
    CHECK(!drm_ioctl(fd, DRM_GEM_CLOSE, &gc) && drm_ioctl(fd, DRM_GEM_CLOSE, &gc) == -1 && errno == EINVAL, "GEM_CLOSE");
    if (pixels) munmap(pixels, W * H * 4); if (other) munmap(other, W * H * 4);
    /* card0 is the KMS primary node (kms.c) next to virgl's render node: GETRESOURCES lists the scanouts */
    int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC); CHECK(card >= 0, "open card0");
    struct { uint64_t ids[4]; uint32_t counts[4]; uint32_t min_width, max_width, min_height, max_height; } resources = {0};
    _Static_assert(sizeof(resources) == 64, "drm_mode_card_res");
    int got = card >= 0 ? ioctl(card, DRM_MODE_GETRESOURCES, &resources) : -1;
    CHECK(!got && resources.counts[1] >= 1 && resources.counts[1] == resources.counts[2] && resources.max_width >= 4096,
          "card0 GETRESOURCES (result %d, %u CRTCs, %u connectors)", got, resources.counts[1], resources.counts[2]);
    if (card >= 0) close(card);
    close(fd);
    if (!failures) pass("GEM_CLOSE and the KMS primary node next to the render node");
}

int main(void) {
    discovery();
    render();
    latency();
    if (failures) { printf("[64] GPU FAILURES=%d\n", failures); return 1; }
    printf("[64] GPU ALL PASSED: discovery, virgl rendering, fences, transfers, dma-buf sharing\n");
    return 0;
}
