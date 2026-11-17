/* DRM uapi of the virtio-gpu (virgl) driver on /dev/dri/card0 and /dev/dri/renderD128, plus the
 * dma-buf and sync_file descriptors it hands out. Modesetting (the KMS ioctls, dumb buffers, page-flip
 * events on card0) lives in kms.c; without virgl only card0 exists and the 3D uapi answers ENOSYS.
 * Semantics follow Linux drivers/gpu/drm/virtio:
 *  - GEM objects are host virgl resources with guest backing pages (a memory file), attached to the
 *    file's host context while a handle exists; mmap goes through DRM_IOCTL_VIRTGPU_MAP offsets.
 *  - Every EXECBUFFER and 3D transfer is fenced. Fences live on one device timeline that the host
 *    completes in order, so an object's reservation is "last fence id" and a sync_file is one fence id.
 *  - PRIME exports dma-buf descriptors (mmap, llseek, poll, SYNC/EXPORT/IMPORT_SYNC_FILE ioctls) that
 *    travel over SCM_RIGHTS like any description; importing one returns the existing object.
 *  - Freed objects are unreferenced on the host with a fenced command; their pages are released only
 *    after that fence passed, so the device never touches freed memory. */
#include "native.h"
#include "gpu.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define PAGE 4096ULL
#define OBJECTS 16384U
#define CLIENTS 256U
#define SYNCS 8192U
#define HANDLES_INITIAL 1024U
#define WAIT_NS (15ULL * 1000000000ULL)
#define OBJECT_LIMIT (512ULL << 20)
enum { E_NOENT = -2, E_INTR = -4, E_BADF = -9, E_AGAIN = -11, E_NOMEM = -12, E_ACCES = -13, E_FAULT = -14, E_BUSY = -16,
       E_EXIST = -17, E_NODEV = -19, E_INVAL = -22, E_NOTTY = -25, E_SPIPE = -29, E_NOSYS = -38, E_OPNOTSUPP = -95, E_TIMEDOUT = -110 };
enum { CMD_RESOURCE_UNREF = 0x102, CMD_ATTACH_BACKING = 0x106, CMD_CTX_CREATE = 0x200, CMD_CTX_DESTROY = 0x201,
       CMD_CTX_ATTACH = 0x202, CMD_CTX_DETACH = 0x203, CMD_RESOURCE_CREATE_3D = 0x204, CMD_TRANSFER_TO_HOST_3D = 0x205,
       CMD_TRANSFER_FROM_HOST_3D = 0x206, CMD_SUBMIT_3D = 0x207 };
enum { FEATURE_BLOB = 3, FEATURE_CONTEXT_INIT = 4 };
#define O_CLOEXEC64 0x80000ULL

struct ctrl_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t context; uint8_t ring, pad[3]; };
struct mem_entry { uint64_t address; uint32_t length, pad; };
struct box { uint32_t x, y, z, w, h, d; };
/* Linux uapi structures (include/uapi/drm/drm.h, virtgpu_drm.h, dma-buf.h, sync_file.h). */
struct drm_version { int32_t major, minor, patch, pad; uint64_t name_len, name, date_len, date, desc_len, desc; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };
struct virtgpu_map { uint64_t offset; uint32_t handle, pad; };
struct virtgpu_execbuffer { uint32_t flags, size; uint64_t command, bo_handles; uint32_t num_bo_handles; int32_t fence_fd;
                            uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs; uint64_t in_syncobjs, out_syncobjs; };
struct virtgpu_getparam { uint64_t param, value; };
struct virtgpu_resource_create { uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags,
                                 bo_handle, res_handle, size, stride; };
struct virtgpu_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct virtgpu_transfer { uint32_t bo_handle; struct box box; uint32_t level, offset, stride, layer_stride; };
struct virtgpu_wait { uint32_t handle, flags; };
struct virtgpu_get_caps { uint32_t cap_set_id, cap_set_ver; uint64_t addr; uint32_t size, pad; };
struct virtgpu_context_init { uint32_t num_params, pad; uint64_t ctx_set_params; };
struct virtgpu_context_param { uint64_t param, value; };
struct sync_merge_data { char name[32]; int32_t fd2, fence; uint32_t flags, pad; };
struct sync_file_info { char name[32]; int32_t status; uint32_t flags, num_fences, pad; uint64_t sync_fence_info; };
struct sync_fence_info { char obj_name[32], driver_name[32]; int32_t status; uint32_t flags; uint64_t timestamp_ns; };
struct dma_buf_export_sync_file { uint32_t flags; int32_t fd; };
_Static_assert(sizeof(struct drm_version) == 64 && sizeof(struct virtgpu_execbuffer) == 64 && sizeof(struct virtgpu_resource_create) == 56 &&
               sizeof(struct virtgpu_transfer) == 44 && sizeof(struct virtgpu_get_caps) == 24 && sizeof(struct sync_merge_data) == 48 &&
               sizeof(struct sync_file_info) == 56 && sizeof(struct sync_fence_info) == 80, "DRM uapi layout");

struct object {
    uint32_t references, resource, next_zombie, next_watch;
    uint64_t size, fence;
    struct memory_file* memory;
    bool used, zombie, watched;
    char name[32];
};
struct client {
    bool used, initialized, closing; unsigned node, busy; /* busy: ioctls in progress; closing: freed when they finish */
    uint32_t context, capset, rings, hint;
    uint64_t poll_rings;
    uint32_t* handles; uint32_t capacity;
    char debug_name[64];
};
struct sync { uint32_t references, next; uint64_t fence, signaled_ns, serial; bool used, listed, notified; };
static struct object* objects; static struct client* clients; static struct sync* syncs;
static uint32_t object_hint, sync_hint, zombies, watches, pending_syncs; static uint64_t sync_serials;
static unsigned live_objects, logged_ioctls;
static uint8_t event_channel;

static void log_hex(const char* label, uint64_t value) {
    char text[19] = "0x"; for (int i = 0; i < 16; i++) text[2 + i] = "0123456789ABCDEF"[(value >> ((15 - i) * 4)) & 15];
    text[18] = 0; vga_print(label); vga_print(text);
}
bool drm_available(void) { return objects != NULL; }
bool drm_render_available(void) { return objects != NULL && virtio_gpu_present(); }
unsigned drm_live_objects(void) { return live_objects; }
static struct ctrl_hdr header(uint32_t type, uint32_t context) { return (struct ctrl_hdr){type, 0, 0, context, 0, {0}}; }
static int send(const void* command, uint32_t bytes, const void* data, uint64_t user, uint32_t data_bytes, bool fence, uint64_t* fence_id) {
    struct gpu_command c = {.command = command, .command_bytes = bytes, .data = data, .user_data = user, .data_bytes = data_bytes, .fence = fence};
    int error = virtio_gpu_command(&c);
    if (!error && fence_id) *fence_id = c.fence_id;
    return error;
}
static int copy_in(void* kernel, uint64_t user, uint64_t bytes) {
    if (!bytes) return 0;
    if (!accessible(user, bytes, false)) return E_FAULT;
    return copy_from_user(kernel, (const void*)user, bytes) ? E_FAULT : 0;
}
static int copy_out(uint64_t user, const void* kernel, uint64_t bytes) {
    if (!bytes) return 0;
    if (!accessible(user, bytes, true)) return E_FAULT;
    return copy_to_user((void*)user, kernel, bytes) ? E_FAULT : 0;
}
static bool signaled(uint64_t fence) { return fence <= virtio_gpu_fence_completed(); }

/* ---- objects ---- */
static uint32_t object_index(const struct object* o) { return (uint32_t)(o - objects) + 1; }
static struct object* object_new(uint64_t size) {
    for (uint32_t n = 0; n < OBJECTS; n++) {
        uint32_t i = (object_hint + n) % OBJECTS;
        if (objects[i].used || objects[i].watched) continue;
        struct object* o = &objects[i]; memset(o, 0, sizeof(*o));
        o->memory = memory_file_create(2); if (!o->memory) return NULL;
        o->memory->size = size;
        for (uint64_t page = 0; page < size / PAGE; page++)
            if (!memory_file_page(o->memory, page, true)) { memory_file_release(o->memory); o->memory = NULL; return NULL; }
        o->resource = virtio_gpu_resource_id();
        if (!o->resource) { memory_file_release(o->memory); o->memory = NULL; return NULL; }
        o->used = true; o->references = 1; o->size = size; object_hint = i + 1; live_objects++;
        return o;
    }
    return NULL;
}
static int object_attach_backing(struct object* o) {
    uint64_t pages = o->size / PAGE, bytes = pages * sizeof(struct mem_entry);
    struct mem_entry* entries = (void*)arena_alloc(bytes); if (!entries) return E_NOMEM;
    uint32_t count = 0;
    for (uint64_t page = 0; page < pages; page++) {
        uint64_t frame = memory_file_page(o->memory, page, false);
        if (count && entries[count - 1].address + entries[count - 1].length == frame && entries[count - 1].length <= 0x7FFFF000U - PAGE)
            entries[count - 1].length += (uint32_t)PAGE;
        else entries[count++] = (struct mem_entry){frame, (uint32_t)PAGE, 0};
    }
    struct { struct ctrl_hdr h; uint32_t resource, count; } command = {header(CMD_ATTACH_BACKING, 0), o->resource, count};
    int error = send(&command, sizeof(command), entries, 0, count * (uint32_t)sizeof(struct mem_entry), false, NULL);
    arena_free((uint64_t)entries, bytes);
    return error;
}
static void reclaim(void) {
    uint32_t* link = &zombies;
    while (*link) {
        struct object* o = &objects[*link - 1];
        if (!signaled(o->fence)) { link = &o->next_zombie; continue; }
        *link = o->next_zombie;
        memory_file_release(o->memory); o->memory = NULL; o->used = o->zombie = false; live_objects--;
    }
}
static void object_release(struct object* o) {
    if (!o->used || o->zombie || !o->references) panic("DRM object reference underflow");
    if (--o->references) return;
    struct { struct ctrl_hdr h; uint32_t resource, pad; } command = {header(CMD_RESOURCE_UNREF, 0), o->resource, 0};
    uint64_t fence = 0;
    if (send(&command, sizeof(command), NULL, 0, 0, true, &fence)) fence = 0;
    virtio_gpu_resource_id_free(o->resource);
    o->zombie = true; o->fence = fence; o->next_zombie = zombies; zombies = object_index(o);
    reclaim();
}
static void object_fence(struct object* o, uint64_t fence) { if (fence > o->fence) o->fence = fence; }

/* ---- sync files ---- */
static struct sync* sync_new(uint64_t fence) {
    for (uint32_t n = 0; n < SYNCS; n++) {
        uint32_t i = (sync_hint + n) % SYNCS;
        if (syncs[i].used || syncs[i].listed) continue;
        struct sync* s = &syncs[i]; memset(s, 0, sizeof(*s));
        s->used = true; s->references = 1; s->fence = fence; s->serial = ++sync_serials; sync_hint = i + 1;
        if (signaled(fence)) { s->notified = true; s->signaled_ns = native_clock_ns(); }
        else { s->listed = true; s->next = pending_syncs; pending_syncs = i + 1; }
        return s;
    }
    return NULL;
}
static void sync_release(struct sync* s) { if (!s->used || !s->references) panic("sync_file reference underflow"); if (!--s->references) s->used = false; }
static int sync_install(struct sync* s) {
    int fd = fd_create_device((unsigned)device_index(DRM_NODE_SYNC), (uint64_t)(s - syncs) + 1, O_CLOEXEC64, "anon_inode:sync_file");
    if (fd < 0) sync_release(s);
    return fd;
}
static struct sync* sync_from_fd(int32_t fd) {
    struct open_file* f = fd_lookup((uint64_t)(uint32_t)fd); unsigned device;
    struct input_reader* reader = f ? fd_device_reader(f, &device) : NULL;
    if (!reader || device_node(device) != DRM_NODE_SYNC || !reader->sequence || reader->sequence > SYNCS) return NULL;
    struct sync* s = &syncs[reader->sequence - 1]; return s->used ? s : NULL;
}
static struct object* object_from_fd(int32_t fd) {
    struct open_file* f = fd_lookup((uint64_t)(uint32_t)fd); unsigned device;
    struct input_reader* reader = f ? fd_device_reader(f, &device) : NULL;
    if (!reader || device_node(device) != DRM_NODE_DMABUF || !reader->sequence || reader->sequence > OBJECTS) return NULL;
    struct object* o = &objects[reader->sequence - 1]; return o->used && !o->zombie ? o : NULL;
}
void drm_fences_signaled(void) {
    if (!objects) return;
    uint64_t now = native_clock_ns();
    uint32_t* link = &pending_syncs;
    while (*link) {
        struct sync* s = &syncs[*link - 1];
        if (s->used && !signaled(s->fence)) { link = &s->next; continue; }
        *link = s->next; s->listed = false;
        if (s->used) { s->notified = true; s->signaled_ns = now; event_source_changed(s, 1); }
    }
    link = &watches;
    while (*link) {
        struct object* o = &objects[*link - 1];
        if (o->used && !o->zombie && !signaled(o->fence)) { link = &o->next_watch; continue; }
        *link = o->next_watch; o->watched = false;
        if (o->used && !o->zombie) event_source_changed(o, 1 | 4);
    }
    reclaim();
}

/* ---- clients and handles ---- */
static struct client* client_of(const struct input_reader* reader) {
    uint64_t index = reader->sequence;
    return index && index <= CLIENTS && clients[index - 1].used ? &clients[index - 1] : NULL;
}
static struct object* handle_object(struct client* c, uint32_t handle) {
    if (!handle || handle > c->capacity || !c->handles[handle - 1]) return NULL;
    return &objects[c->handles[handle - 1] - 1];
}
static int context_create(struct client* c) {
    if (c->context) return 0;
    uint32_t id = (uint32_t)(c - clients) + 1;
    struct { struct ctrl_hdr h; uint32_t length, context_init; char name[64]; } command = {header(CMD_CTX_CREATE, id), 0, c->capset, {0}};
    if (c->debug_name[0]) strcpy(command.name, c->debug_name); else task_get_name(command.name);
    command.name[63] = 0; command.length = (uint32_t)strlen(command.name);
    int error = send(&command, sizeof(command), NULL, 0, 0, false, NULL);
    if (!error) c->context = id;
    return error;
}
static int handle_new(struct client* c, struct object* o, uint32_t* handle) {
    int error = context_create(c); if (error) return error;
    uint32_t slot = c->capacity;
    for (uint32_t n = 0; n < c->capacity; n++) { uint32_t i = (c->hint + n) % c->capacity; if (!c->handles[i]) { slot = i; break; } }
    if (slot == c->capacity) {
        if (c->capacity >= OBJECTS) return E_NOMEM;
        uint32_t capacity = c->capacity ? c->capacity * 2 : HANDLES_INITIAL;
        uint32_t* grown = (void*)arena_alloc((uint64_t)capacity * sizeof(uint32_t)); if (!grown) return E_NOMEM;
        if (c->handles) {
            memcpy(grown, c->handles, (uint64_t)c->capacity * sizeof(uint32_t));
            arena_free((uint64_t)c->handles, (uint64_t)c->capacity * sizeof(uint32_t));
        }
        c->handles = grown; slot = c->capacity; c->capacity = capacity;
    }
    struct { struct ctrl_hdr h; uint32_t resource, pad; } attach = {header(CMD_CTX_ATTACH, c->context), o->resource, 0};
    error = send(&attach, sizeof(attach), NULL, 0, 0, false, NULL); if (error) return error;
    c->handles[slot] = object_index(o); c->hint = slot + 1; o->references++;
    *handle = slot + 1; return 0;
}
static int handle_close(struct client* c, uint32_t handle) {
    struct object* o = handle_object(c, handle); if (!o) return E_INVAL;
    c->handles[handle - 1] = 0;
    if (handle - 1 < c->hint) c->hint = handle - 1;
    struct { struct ctrl_hdr h; uint32_t resource, pad; } detach = {header(CMD_CTX_DETACH, c->context), o->resource, 0};
    if (c->context) send(&detach, sizeof(detach), NULL, 0, 0, false, NULL);
    object_release(o); return 0;
}
int drm_open_check(void) {
    if (!objects) return E_NODEV;
    for (unsigned i = 0; i < CLIENTS; i++) if (!clients[i].used) return 0;
    return -24;
}
int drm_open(unsigned node, uint64_t flags, struct input_reader* reader) {
    (void)flags;
    if (!objects || node > DRM_NODE_RENDER || (node == DRM_NODE_RENDER && !virtio_gpu_present())) return E_NODEV;
    for (unsigned i = 0; i < CLIENTS; i++) if (!clients[i].used) {
        struct client* c = &clients[i]; memset(c, 0, sizeof(*c));
        c->used = true; c->node = node; reader->sequence = i + 1;
        kms_open(i, node);
        return 0;
    }
    return -24;
}
static void client_finish(struct client* c);
void drm_close(unsigned node, struct input_reader* reader) {
    if (!objects) return;
    if (node == DRM_NODE_DMABUF) {
        if (reader->sequence && reader->sequence <= OBJECTS) object_release(&objects[reader->sequence - 1]);
        reader->sequence = 0;
        if (!task_active() || task_exiting()) drm_drain();
        return;
    }
    if (node == DRM_NODE_SYNC) {
        if (reader->sequence && reader->sequence <= SYNCS) sync_release(&syncs[reader->sequence - 1]);
        reader->sequence = 0; return;
    }
    struct client* c = client_of(reader); reader->sequence = 0;
    if (!c) return;
    /* Another thread may still sleep inside an ioctl of this description: it finishes the close. */
    if (c->busy) { c->closing = true; return; }
    client_finish(c);
}
static void client_finish(struct client* c) {
    kms_close((unsigned)(c - clients));   /* its framebuffers (scanouts showing them are disabled), dumb buffers, events */
    for (uint32_t h = 1; h <= c->capacity; h++) if (c->handles[h - 1]) handle_close(c, h);
    if (c->context) {
        struct ctrl_hdr destroy = header(CMD_CTX_DESTROY, c->context);
        send(&destroy, sizeof(destroy), NULL, 0, 0, false, NULL);
    }
    if (c->handles) arena_free((uint64_t)c->handles, (uint64_t)c->capacity * sizeof(uint32_t));
    memset(c, 0, sizeof(*c));
    drm_drain();
}

/* ---- virtio-gpu ioctls ---- */
static int resource_create(struct client* c, struct virtgpu_resource_create* rc) {
    uint64_t size = rc->size ? rc->size : PAGE;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    uint64_t total, free, cached; native_memory_info(&total, &free, &cached);
    if (size > OBJECT_LIMIT || size > free / 2) return E_NOMEM;
    int error = context_create(c); if (error) return error;
    struct object* o = object_new(size); if (!o) return E_NOMEM;
    struct { struct ctrl_hdr h; uint32_t resource, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, pad; }
        command = {header(CMD_RESOURCE_CREATE_3D, 0), o->resource, rc->target, rc->format, rc->bind, rc->width, rc->height, rc->depth,
                   rc->array_size, rc->last_level, rc->nr_samples, rc->flags, 0};
    uint64_t fence = 0;
    error = send(&command, sizeof(command), NULL, 0, 0, true, &fence);
    if (!error) { object_fence(o, fence); error = object_attach_backing(o); }
    uint32_t handle = 0;
    if (!error) error = handle_new(c, o, &handle);
    object_release(o); /* the handle owns the object now */
    if (error) return error;
    rc->res_handle = o->resource; rc->bo_handle = handle; return 0;
}
static int transfer(struct client* c, struct virtgpu_transfer* t, bool to_host) {
    if (t->stride || t->layer_stride) return E_INVAL; /* guest-backed (non-blob) resources use the host layout */
    int error = context_create(c); if (error) return error;
    struct object* o = handle_object(c, t->bo_handle); if (!o) return E_NOENT;
    struct { struct ctrl_hdr h; struct box box; uint64_t offset; uint32_t resource, level, stride, layer_stride; }
        command = {header(to_host ? CMD_TRANSFER_TO_HOST_3D : CMD_TRANSFER_FROM_HOST_3D, c->context), t->box, t->offset,
                   o->resource, t->level, 0, 0};
    uint64_t fence = 0;
    error = send(&command, sizeof(command), NULL, 0, 0, true, &fence);
    if (!error) object_fence(o, fence);
    return error;
}
static int execbuffer(struct client* c, struct virtgpu_execbuffer* e) {
    if (e->flags & ~7U) return E_INVAL;
    if ((e->flags & 4) && e->ring_idx >= c->rings) return E_INVAL;
    if (e->num_in_syncobjs || e->num_out_syncobjs) return E_INVAL; /* DRM_CAP_SYNCOBJ is 0 */
    if (e->num_bo_handles > OBJECTS) return E_INVAL;
    int error = context_create(c); if (error) return error;
    if ((e->flags & 1) && !sync_from_fd(e->fence_fd)) return E_INVAL;
    /* In-fences come from this device's single in-order timeline: the host already orders them. */
    uint32_t count = e->num_bo_handles, bytes = count * (uint32_t)sizeof(uint32_t);
    uint32_t* handles = NULL;
    if (count) {
        handles = (void*)arena_alloc(bytes); if (!handles) return E_NOMEM;
        error = copy_in(handles, e->bo_handles, bytes);
        for (uint32_t i = 0; !error && i < count; i++) if (!handle_object(c, handles[i])) error = E_NOENT;
        if (error) { arena_free((uint64_t)handles, bytes); return error; }
        for (uint32_t i = 0; i < count; i++) { handles[i] = object_index(handle_object(c, handles[i])); objects[handles[i] - 1].references++; }
    }
    struct sync* out = NULL; int fd = -1;
    if (e->flags & 2) {
        out = sync_new(~0ULL);
        if (!out) error = E_NOMEM;
        else { out->references++; fd = sync_install(out); if (fd < 0) { error = fd; sync_release(out); out = NULL; } }
    }
    uint64_t fence = 0;
    if (!error) {
        struct { struct ctrl_hdr h; uint32_t size, pad; } command = {header(CMD_SUBMIT_3D, c->context), e->size, 0};
        error = send(&command, sizeof(command), NULL, e->command, e->size, true, &fence);
        if (error && fd >= 0) { fd_close((uint64_t)fd); fd = -1; }
    }
    if (out) { out->fence = error ? 0 : fence; if (error || signaled(out->fence)) { out->notified = true; out->signaled_ns = native_clock_ns(); } sync_release(out); }
    for (uint32_t i = 0; i < count; i++) { struct object* o = &objects[handles[i] - 1]; if (!error) object_fence(o, fence); object_release(o); }
    if (handles) arena_free((uint64_t)handles, bytes);
    if (error) return error;
    e->fence_fd = (e->flags & 2) ? fd : -1;
    return 0;
}
static int getparam(struct virtgpu_getparam* p) {
    uint64_t mask = 0;
    for (unsigned i = 0; i < virtio_gpu_capset_count(); i++) { uint32_t id, version, size; if (virtio_gpu_capset(i, &id, &version, &size) && id < 64) mask |= 1ULL << id; }
    int32_t value;
    switch (p->param) {
    case 1: value = virtio_gpu_present(); break;                         /* 3D_FEATURES */
    case 2: value = 1; break;                                            /* CAPSET_QUERY_FIX */
    case 3: value = virtio_gpu_feature(FEATURE_BLOB); break;             /* RESOURCE_BLOB */
    case 4: value = 0; break;                                            /* HOST_VISIBLE: no shared memory region */
    case 5: value = 0; break;                                            /* CROSS_DEVICE: no resource UUIDs */
    case 6: case 8: value = virtio_gpu_feature(FEATURE_CONTEXT_INIT); break; /* CONTEXT_INIT, EXPLICIT_DEBUG_NAME */
    case 7: value = (int32_t)mask; break;                                /* SUPPORTED_CAPSET_IDs */
    default: return E_INVAL;
    }
    return copy_out(p->value, &value, sizeof(value));
}
static int get_caps(struct virtgpu_get_caps* g) {
    if (!virtio_gpu_capset_count()) return E_NOSYS;
    if (!g->size) return E_INVAL;
    const void* data; uint32_t size;
    int error = virtio_gpu_capset_data(g->cap_set_id, g->cap_set_ver, &data, &size); if (error) return error == -22 ? E_INVAL : error;
    return copy_out(g->addr, data, g->size < size ? g->size : size);
}
static int context_init(struct client* c, struct virtgpu_context_init* init) {
    if (!virtio_gpu_feature(FEATURE_CONTEXT_INIT)) return E_INVAL;
    if (init->num_params > 4 || init->pad) return E_INVAL;
    if (c->context || c->initialized) return E_EXIST;
    struct virtgpu_context_param params[4];
    int error = copy_in(params, init->ctx_set_params, (uint64_t)init->num_params * sizeof(params[0])); if (error) return error;
    uint32_t capset = 0, rings = 0; uint64_t poll = 0; char name[64] = {0};
    for (uint32_t i = 0; i < init->num_params; i++) {
        uint64_t value = params[i].value;
        if (params[i].param == 1) {
            if (value >= 64) return E_INVAL;
            uint64_t mask = 0;
            for (unsigned k = 0; k < virtio_gpu_capset_count(); k++) { uint32_t id, version, size; if (virtio_gpu_capset(k, &id, &version, &size) && id < 64) mask |= 1ULL << id; }
            if (!(mask & (1ULL << value)) || capset) return E_INVAL;
            capset = (uint32_t)value;
        } else if (params[i].param == 2) { if (value > 64 || rings) return E_INVAL; rings = (uint32_t)value; }
        else if (params[i].param == 3) poll = value;
        else if (params[i].param == 4) {
            if (!value) return E_INVAL;
            for (unsigned k = 0; k < 63; k++) {
                if (!accessible(value + k, 1, false)) return E_FAULT;
                name[k] = (char)uget8(value + k); if (!name[k]) break;
            }
            name[63] = 0;
        } else return E_INVAL;
    }
    if (poll && !rings) return E_INVAL;
    c->capset = capset; c->rings = rings; c->poll_rings = poll; c->initialized = true;
    if (name[0]) memcpy(c->debug_name, name, sizeof(name));
    return context_create(c);
}
static int wait_object(struct client* c, struct virtgpu_wait* w) {
    struct object* o = handle_object(c, w->handle); if (!o) return E_NOENT;
    uint64_t fence = o->fence;
    if (signaled(fence)) return 0;
    if (w->flags & 1) return E_BUSY;
    int error = virtio_gpu_fence_wait(fence, native_clock_ns() + WAIT_NS, true);
    return error == E_TIMEDOUT ? E_BUSY : error;
}
static void copy_field(uint64_t* length, uint64_t user, const char* value) {
    uint64_t size = strlen(value), count = size < *length ? size : *length;
    if (count && user) copy_out(user, value, count);
    *length = size;
}
static int prime_export(struct client* c, struct drm_prime_handle* p) {
    if (p->flags & ~(uint32_t)(O_CLOEXEC64 | 2)) return E_INVAL;
    struct object* o = handle_object(c, p->handle); if (!o) return E_NOENT;
    int fd = fd_create_device((unsigned)device_index(DRM_NODE_DMABUF), object_index(o), p->flags, "/dmabuf:");
    if (fd < 0) return fd;
    o->references++; p->fd = fd; return 0;
}
static int prime_import(struct client* c, struct drm_prime_handle* p) {
    struct object* o = object_from_fd(p->fd); if (!o) return E_INVAL;
    uint32_t index = object_index(o);
    for (uint32_t h = 0; h < c->capacity; h++) if (c->handles[h] == index) { p->handle = h + 1; return 0; }
    return handle_new(c, o, &p->handle);
}
static int dispatch(struct client* c, unsigned node, uint32_t nr, void* data) {
    bool render = node == DRM_NODE_RENDER;
    unsigned index = (unsigned)(c - clients);
    if (nr >= 0x41 && nr <= 0x4B && nr != 0x43 && !virtio_gpu_present()) return E_NOSYS;   /* virtgpu 3D uapi without virgl */
    if (nr == 0x09 && kms_handle(((struct drm_gem_close*)data)->handle)) return kms_handle_close(index, ((struct drm_gem_close*)data)->handle);
    if (nr == 0x1E || nr == 0x1F || nr == 0x3A || (nr >= 0xA0 && nr <= 0xCF)) {               /* SET/DROP_MASTER, WAIT_VBLANK, KMS */
        if (render) return E_ACCES;
        return kms_available() ? kms_ioctl(index, node, nr, data) : nr >= 0xA0 ? E_OPNOTSUPP : E_INVAL;
    }
    switch (nr) {
    case 0x00: {
        struct drm_version* v = data; v->major = 0; v->minor = 1; v->patch = 0;
        copy_field(&v->name_len, v->name, "virtio_gpu"); copy_field(&v->date_len, v->date, "0"); copy_field(&v->desc_len, v->desc, "virtio GPU");
        return 0;
    }
    case 0x09: return handle_close(c, ((struct drm_gem_close*)data)->handle);
    case 0x0C: {
        struct drm_get_cap* cap = data;
        if (cap->capability == 6) { cap->value = 1; return 0; }            /* TIMESTAMP_MONOTONIC */
        if (cap->capability == 5) { cap->value = 3; return 0; }            /* PRIME import | export */
        if (cap->capability == 0x13 || cap->capability == 0x14) { cap->value = 0; return 0; } /* SYNCOBJ(_TIMELINE) */
        return kms_available() ? kms_get_cap(cap->capability, &cap->value) : E_OPNOTSUPP;   /* KMS capabilities */
    }
    case 0x0D: return render ? E_ACCES : kms_available() ? kms_set_client_cap(index, ((uint64_t*)data)[0], ((uint64_t*)data)[1]) : E_OPNOTSUPP;
    case 0x2D: return prime_export(c, data);
    case 0x2E: return prime_import(c, data);
    case 0x41: {
        struct virtgpu_map* m = data; struct object* o = handle_object(c, m->handle); if (!o) return E_NOENT;
        m->offset = (uint64_t)object_index(o) << 32; return 0;
    }
    case 0x42: return execbuffer(c, data);
    case 0x43: return getparam(data);
    case 0x44: return resource_create(c, data);
    case 0x45: {
        struct virtgpu_resource_info* info = data; struct object* o = handle_object(c, info->bo_handle); if (!o) return E_NOENT;
        info->res_handle = o->resource; info->size = (uint32_t)o->size; info->blob_mem = 0; return 0;
    }
    case 0x46: return transfer(c, data, false);
    case 0x47: return transfer(c, data, true);
    case 0x48: return wait_object(c, data);
    case 0x49: return get_caps(data);
    case 0x4A: return E_INVAL;                                             /* RESOURCE_CREATE_BLOB: blob feature not negotiated */
    case 0x4B: return context_init(c, data);
    }
    return E_INVAL;
}
static int dmabuf_ioctl(struct input_reader* reader, uint32_t command, uint64_t argument) {
    if (!reader->sequence || reader->sequence > OBJECTS) return E_BADF;
    struct object* o = &objects[reader->sequence - 1];
    if (command == 0x40086200) {                                           /* DMA_BUF_IOCTL_SYNC */
        uint64_t flags; int error = copy_in(&flags, argument, 8); if (error) return error;
        if ((flags & ~7ULL) || !(flags & 3)) return E_INVAL;
        if (flags & 4) return 0;                                           /* END: CPU access finished */
        uint64_t fence = o->fence;                                         /* START: implicit fences of the buffer */
        return signaled(fence) ? 0 : virtio_gpu_fence_wait(fence, 0, true);
    }
    if (command == 0x40086201 || command == 0x40046201) {                  /* DMA_BUF_SET_NAME */
        uint64_t name = 0; int error = copy_in(&name, argument, command == 0x40046201 ? 4 : 8); if (error) return error;
        char text[32] = {0};
        for (unsigned i = 0; i < 31; i++) { if (copy_in(&text[i], name + i, 1)) return E_FAULT; if (!text[i]) break; }
        memcpy(o->name, text, sizeof(text)); return 0;
    }
    if (command == 0xC0086202) {                                           /* DMA_BUF_IOCTL_EXPORT_SYNC_FILE */
        struct dma_buf_export_sync_file e; int error = copy_in(&e, argument, sizeof(e)); if (error) return error;
        if (!(e.flags & 3) || (e.flags & ~3U)) return E_INVAL;
        struct sync* s = sync_new(o->fence); if (!s) return E_NOMEM;
        int fd = sync_install(s); if (fd < 0) return fd;
        e.fd = fd; return copy_out(argument, &e, sizeof(e));
    }
    if (command == 0x40086203) {                                           /* DMA_BUF_IOCTL_IMPORT_SYNC_FILE */
        struct dma_buf_export_sync_file e; int error = copy_in(&e, argument, sizeof(e)); if (error) return error;
        if (!(e.flags & 3) || (e.flags & ~3U)) return E_INVAL;
        struct sync* s = sync_from_fd(e.fd); if (!s) return E_INVAL;
        object_fence(o, s->fence); return 0;
    }
    return E_NOTTY;
}
static int sync_ioctl(struct input_reader* reader, uint32_t command, uint64_t argument) {
    if (!reader->sequence || reader->sequence > SYNCS) return E_BADF;
    struct sync* s = &syncs[reader->sequence - 1];
    if (command == 0xC0303E03) {                                           /* SYNC_IOC_MERGE */
        struct sync_merge_data m; int error = copy_in(&m, argument, sizeof(m)); if (error) return error;
        if (m.flags || m.pad) return E_INVAL;
        struct sync* other = sync_from_fd(m.fd2); if (!other) return E_NOENT;
        struct sync* merged = sync_new(s->fence > other->fence ? s->fence : other->fence); if (!merged) return E_NOMEM;
        int fd = sync_install(merged); if (fd < 0) return fd;
        m.fence = fd; return copy_out(argument, &m, sizeof(m));
    }
    if (command == 0xC0383E04) {                                           /* SYNC_IOC_FILE_INFO */
        struct sync_file_info info; int error = copy_in(&info, argument, sizeof(info)); if (error) return error;
        if (info.flags || info.pad) return E_INVAL;
        bool done = signaled(s->fence);
        memset(info.name, 0, sizeof(info.name)); strcpy(info.name, "virtio_gpu fence");
        info.status = done ? 1 : 0;
        if (info.num_fences) {
            struct sync_fence_info fence = {{0}, {0}, done ? 1 : 0, 0, done ? s->signaled_ns : 0};
            strcpy(fence.obj_name, "virtio_gpu"); strcpy(fence.driver_name, "virtio_gpu");
            error = copy_out(info.sync_fence_info, &fence, sizeof(fence)); if (error) return error;
        }
        info.num_fences = 1;
        return copy_out(argument, &info, sizeof(info));
    }
    if (command == 0x40103E05) {                                           /* SYNC_IOC_SET_DEADLINE: a scheduling hint */
        uint64_t deadline[2]; int error = copy_in(deadline, argument, sizeof(deadline)); if (error) return error;
        return deadline[1] ? E_INVAL : 0;
    }
    return E_NOTTY;
}
int drm_ioctl(unsigned node, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags) {
    (void)flags;
    if (!objects) return E_NODEV;
    uint32_t cmd = (uint32_t)command;
    if (node == DRM_NODE_DMABUF) return dmabuf_ioctl(reader, cmd, argument);
    if (node == DRM_NODE_SYNC) return sync_ioctl(reader, cmd, argument);
    struct client* c = client_of(reader); if (!c) return E_BADF;
    if (((cmd >> 8) & 0xFF) != 'd') return E_NOTTY;
    uint32_t nr = cmd & 0xFF, size = (cmd >> 16) & 0x3FFF, direction = cmd >> 30;
    uint64_t data[16] = {0};
    if (size > sizeof(data)) return E_INVAL;
    int error = copy_in(data, argument, direction & 1 ? size : 0); if (error) return error;
    c->busy++;
    int result = dispatch(c, node, nr, data);
    if (!--c->busy && c->closing) client_finish(c);
    if (result == E_INVAL && nr != 0x09 && logged_ioctls < 64) { logged_ioctls++; log_hex("[64] DRM ioctl rejected cmd=", cmd); vga_print("\n"); }
    if (direction & 2) { error = copy_out(argument, data, size); if (error) result = error; }
    return result;
}

/* ---- file operations used by device.c ---- */
struct memory_file* drm_mapping(unsigned node, uint64_t* offset, uint64_t length, struct input_reader* reader) {
    if (!objects) return NULL;
    struct object* o = NULL; uint64_t inside = *offset;
    if (node == DRM_NODE_DMABUF) { if (reader->sequence && reader->sequence <= OBJECTS) o = &objects[reader->sequence - 1]; }
    else if (node <= DRM_NODE_RENDER) {
        struct client* c = client_of(reader); uint64_t index = *offset >> 32; inside = *offset & 0xFFFFFFFFULL;
        if (c && node == DRM_NODE_CARD && index >= KMS_MAP_BASE) return kms_mapping((unsigned)(c - clients), offset, length);   /* dumb buffers */
        if (c && index && index <= OBJECTS) {
            for (uint32_t h = 0; h < c->capacity; h++) if (c->handles[h] == index) { o = &objects[index - 1]; break; }
        }
    }
    if (!o || !o->used || o->zombie || inside > o->size || length > o->size - inside) return NULL;
    *offset = inside; return o->memory;
}
uint32_t drm_ready(unsigned node, const struct input_reader* reader) {
    if (!objects) return 0;
    if (node == DRM_NODE_SYNC) {
        if (!reader->sequence || reader->sequence > SYNCS) return 8;
        if (signaled(syncs[reader->sequence - 1].fence)) return 1;
        virtio_gpu_fence_interest(); return 0;
    }
    if (node == DRM_NODE_DMABUF) {
        if (!reader->sequence || reader->sequence > OBJECTS) return 8;
        struct object* o = &objects[reader->sequence - 1];
        if (signaled(o->fence)) return 1 | 4;
        virtio_gpu_fence_interest();
        if (!o->watched) { o->watched = true; o->next_watch = watches; watches = object_index(o); }
        return 0;
    }
    if (node == DRM_NODE_CARD && kms_available()) { struct client* c = client_of(reader); return c ? kms_ready((unsigned)(c - clients)) : 0; }
    return 0; /* render nodes get no DRM events (vblank/page flip are KMS events) */
}
void* drm_event_source(unsigned node, const struct input_reader* reader) {
    if (!objects) return NULL;
    if (node == DRM_NODE_SYNC && reader->sequence && reader->sequence <= SYNCS) return &syncs[reader->sequence - 1];
    if (node == DRM_NODE_DMABUF && reader->sequence && reader->sequence <= OBJECTS) return &objects[reader->sequence - 1];
    if (node == DRM_NODE_CARD && kms_available()) return kms_event_source();
    return node <= DRM_NODE_RENDER ? (void*)&event_channel : NULL;
}
int64_t drm_io(unsigned node, uint64_t buffer, uint64_t count, bool write, uint64_t flags, struct input_reader* reader) {
    if (node != DRM_NODE_CARD && node != DRM_NODE_RENDER) return E_INVAL;
    if (write) return E_INVAL;
    if (node == DRM_NODE_CARD && kms_available()) { struct client* c = client_of(reader); return c ? kms_read((unsigned)(c - clients), buffer, count, flags) : E_BADF; }
    if (flags & 0x800) return E_AGAIN;
    int error = task_wait_channel(&event_channel); /* no events ever arrive: sleep until a signal */
    return error ? error : E_INTR;
}
int64_t drm_seek(unsigned node, uint64_t* position, int64_t offset, uint64_t whence, struct input_reader* reader) {
    if (node == DRM_NODE_DMABUF) {
        if (!reader->sequence || reader->sequence > OBJECTS || offset) return E_INVAL;
        if (whence == 0) return 0;
        if (whence == 2) return (int64_t)objects[reader->sequence - 1].size;
        return E_INVAL;
    }
    if (node == DRM_NODE_SYNC) return E_SPIPE;
    return (int64_t)*position;
}
void drm_stat(unsigned node, const struct input_reader* reader, struct linux_stat* stat) {
    if (!objects) return;
    if (node == DRM_NODE_DMABUF && reader->sequence && reader->sequence <= OBJECTS) {
        struct object* o = &objects[reader->sequence - 1];
        stat->dev = 11; stat->ino = o->memory ? o->memory->inode : 0; stat->mode = 0600; stat->rdev = 0; stat->size = (int64_t)o->size;
        stat->blocks = (int64_t)(o->size / 512);
    } else if (node == DRM_NODE_SYNC && reader->sequence && reader->sequence <= SYNCS) {
        stat->dev = 11; stat->ino = (1ULL << 40) | syncs[reader->sequence - 1].serial; stat->mode = 0600; stat->rdev = 0;
    }
}
void drm_init(void) {
    _Static_assert(CLIENTS == DRM_CLIENTS, "kms.c client table");
    if (virtio_gpu_ready() && !objects) {
        objects = (void*)arena_alloc((uint64_t)OBJECTS * sizeof(struct object));
        clients = (void*)arena_alloc((uint64_t)CLIENTS * sizeof(struct client));
        syncs = (void*)arena_alloc((uint64_t)SYNCS * sizeof(struct sync));
        if (!objects || !clients || !syncs) { objects = NULL; vga_print("[64] DRM: no memory for object tables\n"); }
        else if (virtio_gpu_present()) vga_print("[64] DRM render node /dev/dri/renderD128 and /dev/dri/card0 ready (virtio_gpu 0.1.0)\n");
        else vga_print("[64] DRM primary node /dev/dri/card0 ready (virtio_gpu 0.1.0, KMS without virgl)\n");
        if (objects) kms_init();
    }
    sysfs_init(); /* after the tables: the GPU's sysfs nodes exist only with a usable render node */
}
/* Wait until the device finished every queued command and freed objects released their pages, so a
 * process that exits leaves no GPU-owned memory behind (run_file checks exact page ownership). */
void drm_drain(void) {
    if (!objects) return;
    virtio_gpu_drain();
    drm_fences_signaled();
}
