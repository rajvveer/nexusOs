/* Native virtio-gpu driver: virtio 1.x modern PCI transport carrying virgl 3D commands for the DRM
 * render node (drm.c) and the 2D scanout commands of the KMS driver (kms.c).
 *
 * The VGA/VBE scanout is not taken over at boot: QEMU keeps presenting the firmware framebuffer
 * (/dev/fb0) until a DRM master sets a mode (kms.c), which then scans the same framebuffer pages out
 * through a 2D resource. A device without virgl (plain virtio-vga) is driven for 2D/KMS only
 * (gpu.ready without gpu.present). Display configuration changes (VIRTIO_GPU_EVENT_DISPLAY in the
 * config space) are polled every 100 ms and handed to kms_display_event().
 *
 * Every request is one ring descriptor pointing at an indirect table (command, optional payload,
 * response) that lives in a page owned by the request until the device returns it. Fenced commands
 * get monotonically increasing fence ids; QEMU answers them only after the host GPU passed the
 * fence and completes fences in id order, so "completed fence" is a single number.
 *
 * Completion is polled: virtio_gpu_poll() runs from the BSP device-poll sites (scheduler loop and
 * PIC interrupts), and waiters reap the used ring themselves. INTx is disabled in PCI_COMMAND and the
 * avail ring asks for no interrupts; an interrupt (e.g. MSI-X) handler only has to call
 * virtio_gpu_poll(). QEMU retires virgl fences when the control queue is notified (otherwise on a
 * 10 ms timer), so while fences are outstanding and something waits for one (a fence wait, or a poll
 * that found a fence unsignaled within the last 20 ms) the poll re-notifies the queue every 500 us. */
#include "native.h"
#include "gpu.h"
#include "../../kernel/pci.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define PAGE 4096ULL
#define QUEUE_LIMIT 256U
#define COMMAND_LIMIT 1024U
#define RESPONSE_INLINE 2048U
#define PAYLOAD_LIMIT (8U << 20)
#define CAPSET_LIMIT 8U
#define RESOURCE_IDS 65536U
enum { DESC_NEXT = 1, DESC_WRITE = 2, DESC_INDIRECT = 4 };
enum { STATUS_ACK = 1, STATUS_DRIVER = 2, STATUS_DRIVER_OK = 4, STATUS_FEATURES_OK = 8, STATUS_FAILED = 128 };
enum { FEATURE_VIRGL = 0, FEATURE_EDID = 1, FEATURE_UUID = 2, FEATURE_BLOB = 3, FEATURE_CONTEXT_INIT = 4,
       FEATURE_INDIRECT = 28, FEATURE_VERSION_1 = 32 };
enum { CMD_GET_CAPSET_INFO = 0x108, CMD_GET_CAPSET = 0x109, RESP_OK_NODATA = 0x1100, RESP_OK_CAPSET_INFO = 0x1102,
       RESP_OK_CAPSET = 0x1103, RESP_ERR = 0x1200 };

struct desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct ctrl_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t context; uint8_t ring, pad[3]; };
_Static_assert(sizeof(struct desc) == 16 && sizeof(struct ctrl_hdr) == 24, "virtio ABI");

struct request {
    uint64_t payload, payload_bytes;             /* arena copy of the trailing data */
    uint64_t response, response_alloc;          /* response buffer (inline or arena) */
    uint32_t response_bytes, command_bytes, type, result;
    uint64_t fence;
    volatile bool done; bool waiting, fenced;
    struct desc table[3];
    uint8_t command[COMMAND_LIMIT];
    uint8_t inline_response[RESPONSE_INLINE];
};
_Static_assert(sizeof(struct request) <= PAGE, "request page");

struct queue {
    uint16_t size, number, last_used, avail_index, free_count;
    volatile struct desc* descs; volatile uint16_t* avail; volatile uint8_t* used;
    volatile uint16_t* notify;
    struct request** slots; uint16_t* free; /* one arena page: QUEUE_LIMIT pointers, then QUEUE_LIMIT ids */
};
struct capset { uint32_t id, max_version, max_size; uint64_t data; uint32_t version; bool cached; };
static struct {
    bool present, ready; pci_device_t* pci; uint64_t config_poll; /* present: virgl 3D; ready: device driven (2D at least) */
    volatile uint8_t *common, *device;
    uint64_t notify_base; uint32_t notify_multiplier;
    uint64_t features;
    struct queue control, cursor;
    uint64_t fence_issued, fence_done, last_kick, interest_until, progress_ns, progress_fence; bool stall_logged;
    struct capset capsets[CAPSET_LIMIT]; unsigned capset_count;
    uint32_t* resource_bitmap; uint32_t resource_hint;
    uint64_t commands, fences, kicks, errors; unsigned logged;
    uint8_t fence_channel;
} gpu;
extern uint64_t boot_pml4[], boot_pd[];

static void log_hex(const char* label, uint64_t value) {
    char text[19] = "0x"; for (int i = 0; i < 16; i++) text[2 + i] = "0123456789ABCDEF"[(value >> ((15 - i) * 4)) & 15];
    text[18] = 0; vga_print(label); vga_print(text);
}
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }
static inline uint8_t read8(volatile uint8_t* base, unsigned offset) { return *(volatile uint8_t*)(base + offset); }
static inline uint16_t read16(volatile uint8_t* base, unsigned offset) { return *(volatile uint16_t*)(base + offset); }
static inline uint32_t read32(volatile uint8_t* base, unsigned offset) { return *(volatile uint32_t*)(base + offset); }
static inline void write8(volatile uint8_t* base, unsigned offset, uint8_t value) { *(volatile uint8_t*)(base + offset) = value; }
static inline void write16(volatile uint8_t* base, unsigned offset, uint16_t value) { *(volatile uint16_t*)(base + offset) = value; }
static inline void write32(volatile uint8_t* base, unsigned offset, uint32_t value) { *(volatile uint32_t*)(base + offset) = value; }
static inline void write64(volatile uint8_t* base, unsigned offset, uint64_t value) {
    write32(base, offset, (uint32_t)value); write32(base, offset + 4, (uint32_t)(value >> 32));
}

/* The only BAR-mapping hook of this driver (merge point for the platform's mmio_map(phys, size, MMIO_UNCACHED)).
 * MMIO below 4 GiB is in the kernel's direct map (2 MiB pages, made uncacheable here). A BAR placed above
 * 4 GiB goes through the platform's MMIO window, which every address space shares (this runs before any
 * process exists). Returns the kernel virtual address. */
static uint64_t gpu_bar_map(uint64_t physical, uint64_t size) {
    if (!size || physical + size < physical) return 0;
    if (physical + size <= 0x100000000ULL) {
        for (uint64_t i = physical >> 21; i <= (physical + size - 1) >> 21; i++) {
            boot_pd[i] |= 24; __asm__ volatile("invlpg (%0)" :: "r"(phys_to_virt(i << 21)) : "memory");
        }
        return (uint64_t)phys_to_virt(physical);
    }
    return (uint64_t)(uintptr_t)mmio_map(physical, size, MMIO_UNCACHED); /* platform MMIO window (UEFI puts BARs high) */
}
static uint64_t bar_address(pci_device_t* d, unsigned bar) {
    if (bar > 5) return 0;
    uint32_t low = pci_read32(d->bus, d->device, d->function, (uint8_t)(0x10 + bar * 4));
    if (low & 1) return 0;
    uint64_t address = low & ~0xFULL;
    if (((low >> 1) & 3) == 2 && bar < 5) address |= (uint64_t)pci_read32(d->bus, d->device, d->function, (uint8_t)(0x14 + bar * 4)) << 32;
    return address;
}

static uint64_t features_read(void) {
    write32(gpu.common, 0x00, 0); uint64_t low = read32(gpu.common, 0x04);
    write32(gpu.common, 0x00, 1); return low | (uint64_t)read32(gpu.common, 0x04) << 32;
}
static bool queue_setup(struct queue* q, uint16_t number) {
    write16(gpu.common, 0x16, number);
    uint16_t size = read16(gpu.common, 0x18);
    if (!size) return false;
    if (size > QUEUE_LIMIT) size = QUEUE_LIMIT;
    if (size & (size - 1)) { uint16_t p = 1; while ((uint16_t)(p << 1) <= size) p <<= 1; size = p; }
    write16(gpu.common, 0x18, size);
    uint64_t descs = arena_alloc(PAGE), avail = arena_alloc(PAGE), used = arena_alloc(PAGE), book = arena_alloc(PAGE);
    if (!descs || !avail || !used || !book) return false;
    q->slots = (struct request**)book; q->free = (uint16_t*)(book + QUEUE_LIMIT * sizeof(struct request*));
    q->size = size; q->number = number; q->descs = (void*)descs; q->avail = (void*)avail; q->used = (void*)used;
    q->avail[0] = 1; /* VIRTQ_AVAIL_F_NO_INTERRUPT: completions are polled */
    for (uint16_t i = 0; i < size; i++) q->free[i] = (uint16_t)(size - 1 - i);
    q->free_count = size;
    write16(gpu.common, 0x1A, 0xFFFF);
    write64(gpu.common, 0x20, virt_to_phys((void*)descs)); write64(gpu.common, 0x28, virt_to_phys((void*)avail));
    write64(gpu.common, 0x30, virt_to_phys((void*)used));
    q->notify = (volatile uint16_t*)(gpu.notify_base + (uint64_t)read16(gpu.common, 0x1E) * gpu.notify_multiplier);
    write16(gpu.common, 0x1C, 1);
    return read16(gpu.common, 0x1C) == 1;
}
static void queue_notify(struct queue* q) { barrier(); *q->notify = q->number; }

static void request_free(struct request* r) {
    if (r->payload) arena_free(r->payload, r->payload_bytes);
    if (r->response_alloc) arena_free(r->response_alloc, r->response_bytes);
    page_free((uint64_t)r);
}
static void complete(struct request* r) {
    const struct ctrl_hdr* answer = (const void*)r->response;
    r->result = answer->type;
    if (r->fenced && r->fence > gpu.fence_done) gpu.fence_done = r->fence;
    if (r->result >= RESP_ERR) {
        gpu.errors++;
        if (gpu.logged < 32) {
            gpu.logged++; log_hex("[64] VIRTIO-GPU error cmd=", r->type); log_hex(" response=", r->result);
            log_hex(" context=", ((const struct ctrl_hdr*)r->command)->context); vga_print("\n");
        }
    }
    r->done = true;
    if (r->waiting) task_wake_channel(r); else request_free(r);
}
static void reap(struct queue* q) {
    if (!q->size) return;
    bool any = false;
    for (;;) {
        barrier();
        uint16_t index = *(volatile uint16_t*)(q->used + 2);
        if (q->last_used == index) break;
        barrier();
        uint32_t slot = *(volatile uint32_t*)(q->used + 4 + 8 * (q->last_used % q->size));
        q->last_used++;
        if (slot >= q->size || !q->slots[slot]) { log_hex("[64] VIRTIO-GPU bogus used id=", slot); vga_print("\n"); continue; }
        struct request* r = q->slots[slot]; q->slots[slot] = NULL; q->free[q->free_count++] = (uint16_t)slot;
        complete(r); any = true;
    }
    if (any) task_wake_channel(q);
}
static void reap_all(void) {
    uint64_t before = gpu.fence_done;
    reap(&gpu.control); reap(&gpu.cursor);
    if (gpu.fence_done != before) { task_wake_channel(&gpu.fence_channel); drm_fences_signaled(); }
}
void virtio_gpu_poll(void) {
    if (!gpu.ready) return;
    reap_all();
    uint64_t at = native_clock_ns();
    if (at - gpu.config_poll >= 100000000ULL) {   /* virtio_gpu_config.events_read: display changes (hot-plug, host resize) */
        gpu.config_poll = at;
        uint32_t events = read32(gpu.device, 0);
        if (events) { write32(gpu.device, 4, events); if (events & 1) kms_display_event(); }
    }
    if (gpu.fence_issued > gpu.fence_done) {
        uint64_t now = native_clock_ns();
        if (gpu.progress_fence != gpu.fence_done || !gpu.progress_ns) { gpu.progress_fence = gpu.fence_done; gpu.progress_ns = now; gpu.stall_logged = false; }
        else if (!gpu.stall_logged && now - gpu.progress_ns > 5000000000ULL) {
            gpu.stall_logged = true; /* diagnostic: the host has not retired a fence for 5 s */
            log_hex("[64] VIRTIO-GPU fence stall done=", gpu.fence_done); log_hex(" issued=", gpu.fence_issued);
            log_hex(" in_flight=", (uint64_t)(gpu.control.size - gpu.control.free_count)); log_hex(" kicks=", gpu.kicks); vga_print("\n");
        }
        if (now < gpu.interest_until && now - gpu.last_kick >= 500000ULL) { gpu.last_kick = now; gpu.kicks++; queue_notify(&gpu.control); }
    }
}
void virtio_gpu_fence_interest(void) { gpu.interest_until = native_clock_ns() + 20000000ULL; }
bool virtio_gpu_busy(void) { return gpu.interest_until && native_clock_ns() < gpu.interest_until; }
/* Sleep (or spin before the scheduler runs) until woken, at most until 'until'. */
static int pause_until(void* channel, uint64_t until, bool interruptible) {
    if (!task_active()) { for (unsigned i = 0; i < 256; i++) __asm__ volatile("pause"); reap_all(); return 0; }
    int result = task_wait_until(channel, until);
    if (result == -4) {
        if (interruptible) return -4;
        task_yield();
    }
    return 0;
}
static void queue_submit(struct queue* q, struct request* r) {
    while (!q->free_count) {
        reap_all();
        if (q->free_count) break;
        queue_notify(q);
        pause_until(q, native_clock_ns() + 1000000ULL, false);
    }
    uint16_t slot = q->free[--q->free_count];
    unsigned count = 0;
    r->table[count] = (struct desc){virt_to_phys(r->command), r->command_bytes, DESC_NEXT, (uint16_t)(count + 1)}; count++;
    if (r->payload) { r->table[count] = (struct desc){virt_to_phys((void*)r->payload), (uint32_t)r->payload_bytes, DESC_NEXT, (uint16_t)(count + 1)}; count++; }
    r->table[count] = (struct desc){virt_to_phys((void*)r->response), r->response_bytes, DESC_WRITE, 0}; count++;
    if (r->fenced) {
        struct ctrl_hdr* header = (void*)r->command;
        r->fence = ++gpu.fence_issued; header->flags |= 1; header->fence_id = r->fence; gpu.fences++;
    }
    q->slots[slot] = r;
    q->descs[slot] = (struct desc){virt_to_phys(r->table), count * (uint32_t)sizeof(struct desc), DESC_INDIRECT, 0};
    barrier();
    q->avail[2 + (q->avail_index % q->size)] = slot;
    barrier();
    q->avail[1] = ++q->avail_index;
    gpu.commands++;
    queue_notify(q);
}
static int request_wait(struct request* r, bool interruptible) {
    uint64_t started = native_clock_ns();
    while (!r->done) {
        virtio_gpu_fence_interest();
        reap_all();
        if (r->done) break;
        if (!task_active() && native_clock_ns() - started > 10000000000ULL) {
            vga_print("[64] VIRTIO-GPU device did not answer\n"); r->waiting = false; return -110;
        }
        if (pause_until(r, native_clock_ns() + 1000000ULL, interruptible) == -4 && !r->done) {
            r->waiting = false; return -4; /* the reaper frees it */
        }
    }
    return 0;
}

int virtio_gpu_command(struct gpu_command* c) {
    if (!gpu.ready) return -19;
    if (c->command_bytes < sizeof(struct ctrl_hdr) || c->command_bytes > COMMAND_LIMIT || c->data_bytes > PAYLOAD_LIMIT) return -22;
    struct request* r = (void*)page_alloc(); if (!r) return -12;
    memcpy(r->command, c->command, c->command_bytes); r->command_bytes = c->command_bytes;
    r->type = ((const struct ctrl_hdr*)c->command)->type; r->fenced = c->fence; r->waiting = c->wait;
    if (c->data_bytes) {
        r->payload_bytes = c->data_bytes; r->payload = arena_alloc(c->data_bytes);
        if (!r->payload) { page_free((uint64_t)r); return -12; }
        if (c->data) memcpy((void*)r->payload, c->data, c->data_bytes);
        else {
            if (!accessible(c->user_data, c->data_bytes, false)) { request_free(r); return -14; }
            copy_from_user((void*)r->payload, (const void*)c->user_data, c->data_bytes);
        }
    }
    uint32_t response_bytes = c->wait && c->response_bytes > sizeof(struct ctrl_hdr) ? c->response_bytes : (uint32_t)sizeof(struct ctrl_hdr);
    r->response_bytes = response_bytes;
    if (response_bytes <= RESPONSE_INLINE) r->response = (uint64_t)r->inline_response;
    else {
        r->response = r->response_alloc = arena_alloc(response_bytes);
        if (!r->response) { request_free(r); return -12; }
    }
    queue_submit(&gpu.control, r);
    c->fence_id = r->fence;
    if (!c->wait) return 0;
    int error = request_wait(r, false);
    if (error) return error;
    c->response_type = r->result;
    if (c->response && c->response_bytes) memcpy(c->response, (const void*)r->response, c->response_bytes < response_bytes ? c->response_bytes : response_bytes);
    int result = r->result >= RESP_ERR ? -5 : 0;
    request_free(r); return result;
}

bool virtio_gpu_present(void) { return gpu.present; }
bool virtio_gpu_ready(void) { return gpu.ready; }
unsigned virtio_gpu_scanouts(void) { if (!gpu.ready) return 0; uint32_t n = read32(gpu.device, 8); return n > 16 ? 16 : n; }
bool virtio_gpu_vga(void) { return gpu.pci && pci_read16(gpu.pci->bus, gpu.pci->device, gpu.pci->function, 0x0A) == 0x0300; }
void virtio_gpu_drain(void) {
    uint64_t deadline = native_clock_ns() + 5000000000ULL; /* a wedged host must not hang process teardown */
    while (gpu.ready &&(gpu.control.free_count != gpu.control.size || gpu.cursor.free_count != gpu.cursor.size || gpu.fence_done < gpu.fence_issued)) {
        virtio_gpu_fence_interest();
        virtio_gpu_poll();
        if (gpu.control.free_count == gpu.control.size && gpu.cursor.free_count == gpu.cursor.size && gpu.fence_done >= gpu.fence_issued) break;
        if (native_clock_ns() >= deadline) { vga_print("[64] VIRTIO-GPU drain timed out; freed objects stay queued\n"); break; }
        pause_until(&gpu.fence_channel, native_clock_ns() + 1000000ULL, false);
    }
}
bool virtio_gpu_feature(unsigned bit) { return bit < 64 && (gpu.features >> bit & 1); }
uint64_t virtio_gpu_fence_completed(void) { return gpu.fence_done; }
uint64_t virtio_gpu_fence_issued(void) { return gpu.fence_issued; }
int virtio_gpu_fence_wait(uint64_t fence, uint64_t deadline, bool interruptible) {
    if (!gpu.ready) return 0;
    while (gpu.fence_done < fence) {
        virtio_gpu_fence_interest();
        virtio_gpu_poll();
        if (gpu.fence_done >= fence) break;
        uint64_t now = native_clock_ns();
        if (deadline && now >= deadline) return -110;
        uint64_t until = now + 1000000ULL; if (deadline && until > deadline) until = deadline;
        if (pause_until(&gpu.fence_channel, until, interruptible) == -4) return -4;
    }
    return 0;
}
unsigned virtio_gpu_capset_count(void) { return gpu.capset_count; }
bool virtio_gpu_capset(unsigned index, uint32_t* id, uint32_t* max_version, uint32_t* max_size) {
    if (index >= gpu.capset_count) return false;
    *id = gpu.capsets[index].id; *max_version = gpu.capsets[index].max_version; *max_size = gpu.capsets[index].max_size; return true;
}
int virtio_gpu_capset_data(uint32_t id, uint32_t version, const void** data, uint32_t* size) {
    struct capset* set = NULL;
    for (unsigned i = 0; i < gpu.capset_count; i++) if (gpu.capsets[i].id == id && gpu.capsets[i].max_version >= version) { set = &gpu.capsets[i]; break; }
    if (!set) return -22;
    if (!set->cached || set->version != version) {
        struct { struct ctrl_hdr header; uint32_t id, version; } query = {{CMD_GET_CAPSET, 0, 0, 0, 0, {0}}, id, version};
        struct gpu_command c = {.command = &query, .command_bytes = sizeof(query), .response = (void*)set->data,
                                .response_bytes = set->max_size + (uint32_t)sizeof(struct ctrl_hdr), .wait = true};
        int error = virtio_gpu_command(&c);
        if (error) return error;
        if (c.response_type != RESP_OK_CAPSET) return -5;
        set->cached = true; set->version = version;
    }
    *data = (const void*)(set->data + sizeof(struct ctrl_hdr)); *size = set->max_size; return 0;
}
uint32_t virtio_gpu_resource_id(void) {
    if (!gpu.resource_bitmap) return 0;
    for (uint32_t n = 0; n < RESOURCE_IDS; n++) {
        uint32_t id = (gpu.resource_hint + n) % RESOURCE_IDS;
        if (!id) continue;
        if (!(gpu.resource_bitmap[id >> 5] & (1U << (id & 31)))) {
            gpu.resource_bitmap[id >> 5] |= 1U << (id & 31); gpu.resource_hint = id + 1; return id;
        }
    }
    return 0;
}
void virtio_gpu_resource_id_free(uint32_t id) {
    if (!id || id >= RESOURCE_IDS || !gpu.resource_bitmap || !(gpu.resource_bitmap[id >> 5] & (1U << (id & 31)))) panic("virtio-gpu resource id ownership");
    gpu.resource_bitmap[id >> 5] &= ~(1U << (id & 31));
}
uint32_t virtio_gpu_pci_location(void) { return gpu.pci ? (uint32_t)gpu.pci->bus << 8 | (uint32_t)gpu.pci->device << 3 | gpu.pci->function : 0; }
void virtio_gpu_pci_ids(uint16_t ids[5]) {
    pci_device_t* d = gpu.pci; memset(ids, 0, 5 * sizeof(uint16_t)); if (!d) return;
    ids[0] = d->vendor_id; ids[1] = d->device_id;
    ids[2] = pci_read16(d->bus, d->device, d->function, 0x2C); ids[3] = pci_read16(d->bus, d->device, d->function, 0x2E);
    ids[4] = pci_read8(d->bus, d->device, d->function, PCI_REVISION);
}
void virtio_gpu_pci_config(uint8_t config[256]) {
    memset(config, 0, 256); if (!gpu.pci) return;
    for (unsigned i = 0; i < 256; i += 4) {
        uint32_t value = pci_read32(gpu.pci->bus, gpu.pci->device, gpu.pci->function, (uint8_t)i); memcpy(config + i, &value, 4);
    }
}
uint64_t virtio_gpu_statistic(unsigned which) {
    return which == 0 ? gpu.commands : which == 1 ? gpu.fences : which == 2 ? gpu.kicks : which == 3 ? gpu.errors : 0;
}

bool virtio_gpu_init(void) {
    pci_device_t* d = pci_find_device(0x1AF4, 0x1050);
    if (!d) return false;
    uint16_t status = pci_read16(d->bus, d->device, d->function, PCI_STATUS);
    if (!(status & 0x10)) return false;
    uint8_t cap = pci_read8(d->bus, d->device, d->function, 0x34) & 0xFC;
    uint64_t common = 0, device = 0, notify = 0;
    for (unsigned guard = 0; cap && guard < 48; guard++, cap = pci_read8(d->bus, d->device, d->function, (uint8_t)(cap + 1)) & 0xFC) {
        if (pci_read8(d->bus, d->device, d->function, cap) != 0x09) continue;
        uint8_t type = pci_read8(d->bus, d->device, d->function, (uint8_t)(cap + 3));
        uint8_t bar = pci_read8(d->bus, d->device, d->function, (uint8_t)(cap + 4));
        uint32_t offset = pci_read32(d->bus, d->device, d->function, (uint8_t)(cap + 8));
        uint32_t length = pci_read32(d->bus, d->device, d->function, (uint8_t)(cap + 12));
        uint64_t base = bar_address(d, bar);
        if (!base || type < 1 || type > 4 || type == 3) continue;
        uint64_t mapped = gpu_bar_map(base + offset, length);
        if (!mapped) { log_hex("[64] VIRTIO-GPU cannot map BAR at ", base); vga_print("\n"); return false; }
        if (type == 1 && !common) common = mapped;
        if (type == 4 && !device) device = mapped;
        if (type == 2 && !notify) { notify = mapped; gpu.notify_multiplier = pci_read32(d->bus, d->device, d->function, (uint8_t)(cap + 16)); }
    }
    if (!common || !device || !notify) { vga_print("[64] VIRTIO-GPU without modern capabilities\n"); return false; }
    gpu.common = (volatile uint8_t*)common; gpu.device = (volatile uint8_t*)device; gpu.notify_base = notify;
    uint64_t offered = features_read();
    log_hex("[64] VIRTIO-GPU features=", offered); vga_print("\n");
    if (!(offered >> FEATURE_VERSION_1 & 1) || !(offered >> FEATURE_INDIRECT & 1)) {
        vga_print("[64] VIRTIO-GPU without virtio 1.0 indirect descriptors left in VGA mode\n"); return false;
    }
    bool virgl = offered >> FEATURE_VIRGL & 1;   /* without it (plain virtio-vga): 2D scanouts for KMS only */
    uint16_t command = pci_read16(d->bus, d->device, d->function, PCI_COMMAND);
    pci_write32(d->bus, d->device, d->function, PCI_COMMAND, (command | 6U | 0x400U) & 0xFFFFU);
    write8(gpu.common, 0x14, 0);
    for (unsigned i = 0; i < 1000000 && read8(gpu.common, 0x14); i++) __asm__ volatile("pause");
    write8(gpu.common, 0x14, STATUS_ACK); write8(gpu.common, 0x14, STATUS_ACK | STATUS_DRIVER);
    uint64_t wanted = (1ULL << FEATURE_VERSION_1) | (1ULL << FEATURE_INDIRECT) | (1ULL << FEATURE_EDID) |
                      (virgl ? (1ULL << FEATURE_VIRGL) | (1ULL << FEATURE_BLOB) | (1ULL << FEATURE_CONTEXT_INIT) : 0);
    gpu.features = offered & wanted;
    write32(gpu.common, 0x08, 0); write32(gpu.common, 0x0C, (uint32_t)gpu.features);
    write32(gpu.common, 0x08, 1); write32(gpu.common, 0x0C, (uint32_t)(gpu.features >> 32));
    write8(gpu.common, 0x14, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(read8(gpu.common, 0x14) & STATUS_FEATURES_OK)) { write8(gpu.common, 0x14, STATUS_FAILED); vga_print("[64] VIRTIO-GPU feature negotiation failed\n"); return false; }
    write16(gpu.common, 0x10, 0xFFFF);
    if (read16(gpu.common, 0x12) < 2 || !queue_setup(&gpu.control, 0) || !queue_setup(&gpu.cursor, 1)) {
        write8(gpu.common, 0x14, STATUS_FAILED); vga_print("[64] VIRTIO-GPU queue setup failed\n"); return false;
    }
    gpu.resource_bitmap = (uint32_t*)arena_alloc(RESOURCE_IDS / 8);
    if (!gpu.resource_bitmap) { write8(gpu.common, 0x14, STATUS_FAILED); return false; }
    gpu.resource_hint = 1;
    write8(gpu.common, 0x14, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    gpu.pci = d; gpu.ready = true; gpu.present = virgl;
    if (!virgl) {
        log_hex("[64] VIRTIO-GPU 2D ready (no virgl) features=", gpu.features); log_hex(" scanouts=", read32(gpu.device, 8));
        log_hex(" pci=", virtio_gpu_pci_location()); vga_print("\n");
        return true;
    }
    unsigned capsets = read32(gpu.device, 12);
    for (unsigned i = 0; i < capsets && gpu.capset_count < CAPSET_LIMIT; i++) {
        struct { struct ctrl_hdr header; uint32_t index, pad; } query = {{CMD_GET_CAPSET_INFO, 0, 0, 0, 0, {0}}, i, 0};
        struct { struct ctrl_hdr header; uint32_t id, max_version, max_size, pad; } info;
        struct gpu_command c = {.command = &query, .command_bytes = sizeof(query), .response = &info, .response_bytes = sizeof(info), .wait = true};
        if (virtio_gpu_command(&c) || c.response_type != RESP_OK_CAPSET_INFO || !info.max_size || info.max_size > 65536) continue;
        uint64_t data = arena_alloc(info.max_size + sizeof(struct ctrl_hdr)); /* cache buffer owned from boot */
        if (!data) continue;
        gpu.capsets[gpu.capset_count++] = (struct capset){info.id, info.max_version, info.max_size, data, 0, false};
        log_hex("[64] VIRTIO-GPU capset id=", info.id); log_hex(" version=", info.max_version); log_hex(" size=", info.max_size); vga_print("\n");
    }
    log_hex("[64] VIRTIO-GPU virgl ready features=", gpu.features); log_hex(" scanouts=", read32(gpu.device, 8));
    log_hex(" pci=", virtio_gpu_pci_location()); vga_print("\n");
    return true;
}

/* System sleep (pm.c registers this for a driven device): virgl's host-side state (contexts, resources) cannot be
 * saved, so S3/hibernation is refused (EBUSY) while DRM objects exist; otherwise the device is reset and, when the
 * power returns, negotiated again on the same queue memory. The 2D KMS state (scanout resources) is rebuilt by
 * kms_resume. Suspend-to-idle keeps it as it is. */
#include "power.h"
static bool queue_restart(struct queue* q) {
    for (uint16_t i = 0; i < q->size; i++) {   /* requests the reset lost: answered with an error (their waiters go on) */
        struct request* r = q->slots[i]; if (!r) continue;
        q->slots[i] = NULL; ((struct ctrl_hdr*)r->response)->type = RESP_ERR; complete(r);
    }
    task_wake_channel(q);
    write16(gpu.common, 0x16, q->number);
    if (read16(gpu.common, 0x18) < q->size) return false;
    write16(gpu.common, 0x18, q->size);
    memset((void*)q->descs, 0, PAGE); memset((void*)q->avail, 0, PAGE); memset((void*)q->used, 0, PAGE);
    q->avail[0] = 1; q->avail_index = 0; q->last_used = 0; q->free_count = q->size;
    for (uint16_t i = 0; i < q->size; i++) { q->free[i] = (uint16_t)(q->size - 1 - i); q->slots[i] = NULL; }
    write16(gpu.common, 0x1A, 0xFFFF);
    write64(gpu.common, 0x20, virt_to_phys((void*)q->descs)); write64(gpu.common, 0x28, virt_to_phys((void*)q->avail));
    write64(gpu.common, 0x30, virt_to_phys((void*)q->used));
    q->notify = (volatile uint16_t*)(gpu.notify_base + (uint64_t)read16(gpu.common, 0x1E) * gpu.notify_multiplier);
    write16(gpu.common, 0x1C, 1);
    return read16(gpu.common, 0x1C) == 1;
}
static int gpu_pm_suspend(void* context, unsigned state) {
    (void)context;
    if (!gpu.ready) return 0;
    if (gpu.present && drm_live_objects()) { vga_print("[64] VIRTIO-GPU: 3D objects in use (virgl contexts or resources): sleep refused\n"); return -16; }
    if (state == PM_STATE_FREEZE) return 0;
    virtio_gpu_drain();   /* outstanding 2D commands (a frozen compositor's last DIRTYFB) finish first, 5 s bound */
    write8(gpu.common, 0x14, 0);
    for (unsigned i = 0; i < 1000000 && read8(gpu.common, 0x14); i++) __asm__ volatile("pause");
    return 0;
}
static void gpu_pm_resume(void* context, unsigned state) {
    (void)context;
    if (!gpu.ready || state == PM_STATE_FREEZE) return;
    uint16_t command = pci_read16(gpu.pci->bus, gpu.pci->device, gpu.pci->function, PCI_COMMAND);
    pci_write32(gpu.pci->bus, gpu.pci->device, gpu.pci->function, PCI_COMMAND, (command | 6U | 0x400U) & 0xFFFFU);
    write8(gpu.common, 0x14, 0);
    for (unsigned i = 0; i < 1000000 && read8(gpu.common, 0x14); i++) __asm__ volatile("pause");
    write8(gpu.common, 0x14, STATUS_ACK); write8(gpu.common, 0x14, STATUS_ACK | STATUS_DRIVER);
    write32(gpu.common, 0x08, 0); write32(gpu.common, 0x0C, (uint32_t)gpu.features);
    write32(gpu.common, 0x08, 1); write32(gpu.common, 0x0C, (uint32_t)(gpu.features >> 32));
    write8(gpu.common, 0x14, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    write16(gpu.common, 0x10, 0xFFFF);
    if (!(read8(gpu.common, 0x14) & STATUS_FEATURES_OK) || !queue_restart(&gpu.control) || !queue_restart(&gpu.cursor)) {
        write8(gpu.common, 0x14, STATUS_FAILED); gpu.present = gpu.ready = false; vga_print("[64] VIRTIO-GPU did not come back after resume\n"); return;
    }
    write8(gpu.common, 0x14, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    kms_resume();   /* the host forgot the scanout resources with the reset */
}
const struct pm_ops virtio_gpu_pm = {"virtio-gpu", gpu_pm_suspend, gpu_pm_resume, NULL};
