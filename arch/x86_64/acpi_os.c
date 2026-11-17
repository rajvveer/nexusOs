/* uACPI kernel API (uacpi/kernel_api.h) on NexusOS x86-64.
 *
 * Execution contexts. uACPI runs with the big kernel lock held, in one of:
 *   - a task (a system call such as a /sys/class/power_supply read): blocking waits sleep the task
 *     (task_wait_until releases the big lock), so a contended mutex never stalls other CPUs;
 *   - the ACPI worker: a kernel thread multiplexed on the boot CPU's scheduler loop (power_poll ->
 *     acpi_worker_poll). It runs deferred work (GPE methods, Notify() handlers) in FIFO order on CPU 0,
 *     as uACPI asks; when AML sleeps or waits it switches back to the loop and is resumed on a later pass;
 *   - an atomic context (boot before the scheduler, SCI interrupt): waits spin.
 * The SCI handler runs from irq.c (trap64, big lock held, interrupts off) and only takes spinlocks.
 *
 * Memory: a page-slab heap (16 B - 8 KiB size classes) in chunks taken from the page allocator at boot,
 * sized for the firmware's AML so that steady-state AML evaluation never allocates pages after the
 * kernel's memory baseline; larger blocks come straight from arena_alloc. Frees are sized
 * (UACPI_SIZED_FREES) but the class is taken from the page, never trusted from the hint.
 *
 * Mappings: physical memory below 4 GiB and RAM above it are in the kernel's direct map; other memory
 * above 4 GiB (64-bit opregions) goes through the permanent MMIO window. Unmapping is a no-op. */
#include <uacpi/kernel_api.h>
#include <uacpi/status.h>
#include "native.h"
#include "power.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

uint64_t acpi_table_rsdp(void);        /* acpi.c */
volatile void* pci_ecam_address(unsigned segment, unsigned bus, unsigned device, unsigned function);
int irq_attach_gsi(uint32_t gsi, unsigned flags, bool level, bool active_low, irq_handler_t handler, void* context);
extern void switch_context(uint64_t* previous, uint64_t next);

static inline uint64_t irq_save(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_restore(uint64_t f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }
static void busy_until(uint64_t deadline) { while (native_clock_ns() < deadline) { smp_tlb_poll(); __asm__ volatile("pause" ::: "memory"); } }

void* acpi_os_memmove(void* dest, const void* src, __SIZE_TYPE__ n) {
    uint8_t* d = dest; const uint8_t* s = src;
    if (d == s || !n) return dest;
    if (d < s || d >= s + n) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

/* ---- heap ---- */
#define CHUNK_PAGES 64U
#define MAX_CHUNKS 64U
#define CLASS_SHIFT_MIN 4U
#define CLASSES 10U                 /* 16 .. 8192 bytes */
#define LARGE_TRACK 128U
struct chunk { uint8_t* base; uint8_t page_class[CHUNK_PAGES]; };  /* 0 free, 1 + class, 0xFF second page of an 8 KiB object */
static struct chunk chunks[MAX_CHUNKS];
static unsigned chunk_count;
static void* free_lists[CLASSES];
static struct { uint64_t address, bytes; } large[LARGE_TRACK];
static uint64_t large_pages;
static volatile int heap_lock;
static bool heap_mismatch_reported;
static unsigned class_for(uint64_t bytes) {
    unsigned c = 0; while (c < CLASSES - 1 && (16ULL << c) < bytes) c++;
    return c;
}
static bool chunk_add(void) {
    if (chunk_count == MAX_CHUNKS) return false;
    uint64_t base = arena_alloc(CHUNK_PAGES * 4096ULL); if (!base) return false;
    struct chunk* c = &chunks[chunk_count]; memset(c, 0, sizeof(*c)); c->base = (uint8_t*)base;
    __atomic_store_n(&chunk_count, chunk_count + 1, __ATOMIC_RELEASE);
    return true;
}
/* A fresh page (two for the 8 KiB class) for size class `cls`, split onto its free list. heap_lock held. */
static bool class_refill(unsigned cls, bool grow) {
    unsigned span = cls == CLASSES - 1 ? 2 : 1;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < chunk_count; i++) for (unsigned p = 0; p + span <= CHUNK_PAGES; p++) {
            if (chunks[i].page_class[p] || (span == 2 && chunks[i].page_class[p + 1])) continue;
            chunks[i].page_class[p] = (uint8_t)(cls + 1); if (span == 2) chunks[i].page_class[p + 1] = 0xFF;
            uint8_t* page = chunks[i].base + (uint64_t)p * 4096; unsigned size = 16U << cls;
            for (unsigned at = 0; at + size <= span * 4096U; at += size) { *(void**)(page + at) = free_lists[cls]; free_lists[cls] = page + at; }
            return true;
        }
        if (!grow || !chunk_add()) return false;
    }
    return false;
}
static int chunk_of(const void* p, unsigned* page) {
    for (unsigned i = 0; i < chunk_count; i++) {
        const uint8_t* b = chunks[i].base;
        if ((const uint8_t*)p >= b && (const uint8_t*)p < b + CHUNK_PAGES * 4096ULL) { *page = (unsigned)(((const uint8_t*)p - b) / 4096); return (int)i; }
    }
    return -1;
}
void* acpi_os_alloc(uint64_t bytes) {
    if (!bytes) bytes = 1;
    if (bytes > (16ULL << (CLASSES - 1))) {
        if (bytes > (1ULL << 32)) return NULL;
        uint64_t p = arena_alloc(bytes); if (!p) return NULL;   /* zeroed by the page allocator */
        uint64_t f = irq_save(); spin_acquire(&heap_lock);
        for (unsigned i = 0; i < LARGE_TRACK; i++) if (!large[i].address) { large[i].address = p; large[i].bytes = bytes; break; }
        large_pages += (bytes + 4095) / 4096;
        spin_release(&heap_lock); irq_restore(f);
        return (void*)p;
    }
    unsigned cls = class_for(bytes);
    uint64_t f = irq_save(); spin_acquire(&heap_lock);
    if (!free_lists[cls] && !class_refill(cls, true)) { spin_release(&heap_lock); irq_restore(f); return NULL; }
    void* p = free_lists[cls]; free_lists[cls] = *(void**)p;
    spin_release(&heap_lock); irq_restore(f);
    memset(p, 0, 16U << cls);
    return p;
}
void acpi_os_free(void* memory, uint64_t bytes) {
    if (!memory) return;
    uint64_t f = irq_save(); spin_acquire(&heap_lock);
    unsigned page; int index = chunk_of(memory, &page);
    if (index >= 0) {
        uint8_t mark = chunks[index].page_class[page];
        uint64_t within = (uint64_t)((uint8_t*)memory - chunks[index].base) & 4095;   /* objects never straddle into a page they do not own */
        if (!mark || mark == 0xFF || (within & ((mark - 1U == CLASSES - 1 ? 4096U : 16U << (mark - 1)) - 1))) {
            spin_release(&heap_lock); irq_restore(f);
            vga_print("[64] ACPI heap: invalid free ignored\n"); return;
        }
        unsigned cls = mark - 1U;
        if (cls != class_for(bytes) && !heap_mismatch_reported) heap_mismatch_reported = true;
        *(void**)memory = free_lists[cls]; free_lists[cls] = memory;
        spin_release(&heap_lock); irq_restore(f); return;
    }
    uint64_t size = 0;
    for (unsigned i = 0; i < LARGE_TRACK; i++) if (large[i].address == (uint64_t)memory) { size = large[i].bytes; large[i].address = 0; break; }
    if (!size) size = bytes;   /* untracked (table full): the hint is all there is */
    large_pages -= (size + 4095) / 4096 <= large_pages ? (size + 4095) / 4096 : large_pages;
    spin_release(&heap_lock); irq_restore(f);
    if (!size) { vga_print("[64] ACPI heap: zero-size free of a large block ignored\n"); return; }
    arena_free((uint64_t)memory, size);
}
unsigned acpi_os_heap_pages(void) { return chunk_count * CHUNK_PAGES + (unsigned)large_pages; }
static bool worker_create(void);
bool acpi_os_init(uint64_t aml_bytes) {
    /* Namespace + objects of typical firmware take a few times the AML size; growth after boot stays possible. */
    uint64_t want = 768 * 1024ULL + aml_bytes * 6, chunk_bytes = CHUNK_PAGES * 4096ULL;
    unsigned count = (unsigned)((want + chunk_bytes - 1) / chunk_bytes);
    if (count > 16) count = 16;
    for (unsigned i = 0; i < count; i++) if (!chunk_add()) break;
    return chunk_count > 0 && worker_create();
}
void* uacpi_kernel_alloc(uacpi_size size) { return acpi_os_alloc(size); }
void* uacpi_kernel_alloc_zeroed(uacpi_size size) { return acpi_os_alloc(size); }
void uacpi_kernel_free(void* mem, uacpi_size size_hint) { acpi_os_free(mem, size_hint); }

/* ---- tables, mappings, logging ---- */
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out) {
    uint64_t rsdp = acpi_table_rsdp(); if (!rsdp) return UACPI_STATUS_NOT_FOUND;
    *out = rsdp; return UACPI_STATUS_OK;
}
void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    if (!len) len = 1;
    if (addr + len < addr || addr + len > (1ULL << 46)) return UACPI_MAP_FAILED;
    if (addr + len <= 0x100000000ULL || memory_kernel_buffer(phys_to_virt(addr), len)) return phys_to_virt(addr);
    volatile void* window = mmio_map(addr, len, MMIO_UNCACHED);
    return window ? (void*)window : UACPI_MAP_FAILED;
}
void uacpi_kernel_unmap(void* addr, uacpi_size len) { (void)addr; (void)len; }
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* message) {
    if (level > UACPI_LOG_INFO) return;
    vga_print(level == UACPI_LOG_ERROR ? "[64] ACPI error: " : level == UACPI_LOG_WARN ? "[64] ACPI warning: " : "[64] ACPI: ");
    vga_print(message);
}

/* ---- PCI configuration space: CF8/CFC with the exact width (segment 0, offsets < 256), else ECAM ---- */
struct pci_handle { uint16_t segment; uint8_t bus, device, function; };
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle* out) {
    if (address.device > 31 || address.function > 7) return UACPI_STATUS_INVALID_ARGUMENT;
    struct pci_handle* h = acpi_os_alloc(sizeof(*h)); if (!h) return UACPI_STATUS_OUT_OF_MEMORY;
    h->segment = address.segment; h->bus = address.bus; h->device = address.device; h->function = address.function;
    *out = h; return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle handle) { acpi_os_free(handle, sizeof(struct pci_handle)); }
static uacpi_status pci_access(uacpi_handle handle, uacpi_size offset, unsigned width, uint32_t* value, bool write) {
    const struct pci_handle* h = handle;
    if (offset > 4096 - width || (offset & (width - 1))) return UACPI_STATUS_INVALID_ARGUMENT;
    if (h->segment == 0 && offset < 256) {
        uint64_t f = irq_save();
        port_dword_out(0xCF8, 0x80000000U | (uint32_t)h->bus << 16 | (uint32_t)h->device << 11 | (uint32_t)h->function << 8 | (uint32_t)(offset & 0xFC));
        uint16_t port = (uint16_t)(0xCFC + (offset & 3));
        if (write) { if (width == 1) port_byte_out(port, (uint8_t)*value); else if (width == 2) port_word_out(port, (uint16_t)*value); else port_dword_out(port, *value); }
        else *value = width == 1 ? port_byte_in(port) : width == 2 ? port_word_in(port) : port_dword_in(port);
        irq_restore(f); return UACPI_STATUS_OK;
    }
    volatile uint8_t* config = (volatile uint8_t*)pci_ecam_address(h->segment, h->bus, h->device, h->function);
    if (!config) { if (!write) *value = 0xFFFFFFFFU >> (32 - 8 * width); return UACPI_STATUS_OK; }
    if (write) { if (width == 1) config[offset] = (uint8_t)*value; else if (width == 2) *(volatile uint16_t*)(config + offset) = (uint16_t)*value; else *(volatile uint32_t*)(config + offset) = *value; }
    else *value = width == 1 ? config[offset] : width == 2 ? *(volatile uint16_t*)(config + offset) : *(volatile uint32_t*)(config + offset);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read8(uacpi_handle d, uacpi_size o, uacpi_u8* v) { uint32_t x; uacpi_status s = pci_access(d, o, 1, &x, false); *v = (uint8_t)x; return s; }
uacpi_status uacpi_kernel_pci_read16(uacpi_handle d, uacpi_size o, uacpi_u16* v) { uint32_t x; uacpi_status s = pci_access(d, o, 2, &x, false); *v = (uint16_t)x; return s; }
uacpi_status uacpi_kernel_pci_read32(uacpi_handle d, uacpi_size o, uacpi_u32* v) { return pci_access(d, o, 4, v, false); }
uacpi_status uacpi_kernel_pci_write8(uacpi_handle d, uacpi_size o, uacpi_u8 v) { uint32_t x = v; return pci_access(d, o, 1, &x, true); }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle d, uacpi_size o, uacpi_u16 v) { uint32_t x = v; return pci_access(d, o, 2, &x, true); }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle d, uacpi_size o, uacpi_u32 v) { uint32_t x = v; return pci_access(d, o, 4, &x, true); }

/* ---- port I/O ---- */
struct io_handle { uint16_t base; uint32_t length; };
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle* out) {
    if (!len || base > 0xFFFF || len > 0x10000 - base) return UACPI_STATUS_INVALID_ARGUMENT;
    struct io_handle* h = acpi_os_alloc(sizeof(*h)); if (!h) return UACPI_STATUS_OUT_OF_MEMORY;
    h->base = (uint16_t)base; h->length = (uint32_t)len; *out = h; return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle handle) { acpi_os_free(handle, sizeof(struct io_handle)); }
static bool io_ok(uacpi_handle handle, uacpi_size offset, unsigned width, uint16_t* port) {
    const struct io_handle* h = handle;
    if (offset > h->length || width > h->length - offset) return false;
    *port = (uint16_t)(h->base + offset); return true;
}
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size o, uacpi_u8* v) { uint16_t p; if (!io_ok(h, o, 1, &p)) return UACPI_STATUS_INVALID_ARGUMENT; *v = port_byte_in(p); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size o, uacpi_u16* v) { uint16_t p; if (!io_ok(h, o, 2, &p)) return UACPI_STATUS_INVALID_ARGUMENT; *v = port_word_in(p); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size o, uacpi_u32* v) { uint16_t p; if (!io_ok(h, o, 4, &p)) return UACPI_STATUS_INVALID_ARGUMENT; *v = port_dword_in(p); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size o, uacpi_u8 v) { uint16_t p; if (!io_ok(h, o, 1, &p)) return UACPI_STATUS_INVALID_ARGUMENT; port_byte_out(p, v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v) { uint16_t p; if (!io_ok(h, o, 2, &p)) return UACPI_STATUS_INVALID_ARGUMENT; port_word_out(p, v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v) { uint16_t p; if (!io_ok(h, o, 4, &p)) return UACPI_STATUS_INVALID_ARGUMENT; port_dword_out(p, v); return UACPI_STATUS_OK; }

/* ---- time ---- */
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return native_clock_ns(); }
void uacpi_kernel_stall(uacpi_u8 usec) { busy_until(native_clock_ns() + (uint64_t)usec * 1000); }

/* ---- the worker ---- */
#define WORK_SLOTS 256U
#define WORKER_STACK (64 * 1024ULL)
struct work { uacpi_work_handler handler; uacpi_handle context; };
static struct work queue[WORK_SLOTS];
static unsigned queue_head, queue_count;
static volatile int queue_lock;
static uint64_t worker_rsp, worker_return_rsp, worker_stack, worker_wake_at;
static volatile bool worker_in, worker_waiting, worker_item;
static volatile unsigned irq_depth;          /* SCI handlers in flight (any CPU) */
static unsigned worker_marker;               /* its thread id */
static bool queue_pop(struct work* w) {
    uint64_t f = irq_save(); spin_acquire(&queue_lock);
    bool any = queue_count != 0;
    if (any) { *w = queue[queue_head]; queue_head = (queue_head + 1) % WORK_SLOTS; queue_count--; }
    spin_release(&queue_lock); irq_restore(f);
    return any;
}
static void worker_main(void) {
    for (;;) {
        struct work w;
        while (queue_pop(&w)) { worker_item = true; w.handler(w.context); worker_item = false; }
        switch_context(&worker_rsp, worker_return_rsp);
    }
}
static bool worker_create(void) {
    if (worker_stack) return true;
    worker_stack = arena_alloc(WORKER_STACK); if (!worker_stack) return false;
    uint64_t* frame = (uint64_t*)(worker_stack + WORKER_STACK - 64);
    for (unsigned i = 0; i < 6; i++) frame[i] = 0;               /* r15 r14 r13 r12 rbx rbp */
    frame[6] = (uint64_t)worker_main;
    worker_rsp = (uint64_t)frame;
    return true;
}
bool acpi_worker_busy(void) { return queue_count || worker_item || worker_waiting; }
/* Boot CPU, big lock held, no task on this CPU, not inside an interrupt handler. */
static void worker_enter(void) {
    if (!worker_stack || worker_in || cpu_self()->index || cpu_self()->task || irq_depth) return;
    if (worker_waiting && native_clock_ns() < worker_wake_at) return;
    if (!worker_waiting && !queue_count) return;
    worker_in = true;
    switch_context(&worker_return_rsp, worker_rsp);
    worker_in = false;
}
void acpi_worker_poll(void) { worker_enter(); }
/* tick.c: queued work runs on the boot CPU's next pass; sleeping AML (Sleep/Stall waits) at its deadline. */
uint64_t acpi_worker_next_event(uint64_t now) {
    if (!worker_stack) return ~0ULL;
    if (__atomic_load_n(&queue_count, __ATOMIC_RELAXED)) return now;
    return worker_waiting ? worker_wake_at : ~0ULL;
}
/* The worker blocks: back to whoever entered it, resumed on a pass at or after `deadline`. */
static void worker_block(uint64_t deadline) {
    worker_waiting = true; worker_wake_at = deadline;
    switch_context(&worker_rsp, worker_return_rsp);
    worker_waiting = false;
}
void acpi_os_drain(uint64_t timeout_ms) {
    uint64_t end = native_clock_ns() + timeout_ms * 1000000ULL;
    while (acpi_worker_busy() && native_clock_ns() < end) {
        bool waiting = worker_waiting; uint64_t at = worker_wake_at;
        worker_enter();
        if (waiting && worker_waiting && at == worker_wake_at) busy_until(native_clock_ns() + 100000);  /* sleeping AML */
        if (cpu_self()->index || cpu_self()->task || irq_depth) break;   /* not a context that can run it */
    }
}
int acpi_os_queue(void (*work)(void* context), void* context) {
    if (!worker_stack) return -19;
    return uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, work, context) == UACPI_STATUS_OK ? 0 : -28;
}
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler, uacpi_handle context) {
    (void)type;   /* one FIFO on CPU 0 serves both kinds: GPE methods, notifications, GPE restores in order */
    uint64_t f = irq_save(); spin_acquire(&queue_lock);
    bool full = queue_count == WORK_SLOTS;
    if (!full) { queue[(queue_head + queue_count) % WORK_SLOTS] = (struct work){handler, context}; queue_count++; }
    spin_release(&queue_lock); irq_restore(f);
    return full ? UACPI_STATUS_OUT_OF_MEMORY : UACPI_STATUS_OK;
}

enum { CONTEXT_TASK, CONTEXT_WORKER, CONTEXT_ATOMIC };
static int context_kind(void) {
    if (irq_depth && acpi_os_irq_context()) return CONTEXT_ATOMIC;
    if (worker_in && !cpu_self()->index) return CONTEXT_WORKER;
    if (cpu_self()->task && kernel_lock_held()) return CONTEXT_TASK;
    return CONTEXT_ATOMIC;
}
static volatile int irq_cpu_mask;            /* CPUs inside an SCI handler */
bool acpi_os_irq_context(void) { return (__atomic_load_n(&irq_cpu_mask, __ATOMIC_ACQUIRE) >> cpu_self()->index) & 1; }
uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    int kind = context_kind();
    if (kind == CONTEXT_WORKER) return UACPI_STATUS_OK;   /* called from work: everything before it has run */
    uint64_t end = native_clock_ns() + 30000000000ULL; int channel;
    while ((irq_depth || acpi_worker_busy()) && native_clock_ns() < end) {
        if (kind == CONTEXT_TASK) task_wait_until(&channel, native_clock_ns() + 1000000);
        else { acpi_os_drain(1); busy_until(native_clock_ns() + 50000); }
    }
    return irq_depth || acpi_worker_busy() ? UACPI_STATUS_HARDWARE_TIMEOUT : UACPI_STATUS_OK;
}

/* Wait until `deadline` (or a 1 ms slice) the way the current context allows. */
static void wait_slice(void* channel, uint64_t deadline) {
    uint64_t now = native_clock_ns(), slice = now + 1000000ULL;
    if (deadline > slice) deadline = slice;
    switch (context_kind()) {
    case CONTEXT_TASK: if (task_wait_until(channel, deadline) == -4) busy_until(deadline); break;   /* signal pending: AML still waits */
    case CONTEXT_WORKER: worker_block(deadline); break;
    default:
        if (!cpu_self()->index && !cpu_self()->task && !irq_depth && worker_stack && !worker_in) worker_enter();   /* boot CPU: let the worker progress */
        busy_until(deadline < now + 20000 ? deadline : now + 20000);
    }
}
void uacpi_kernel_sleep(uacpi_u64 msec) {
    uint64_t deadline = native_clock_ns() + (msec > 3600000 ? 3600000ULL : msec) * 1000000ULL; int channel;
    while (native_clock_ns() < deadline) wait_slice(&channel, deadline);
}

/* ---- threads, mutexes, events, spinlocks ---- */
uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    if (worker_in && !cpu_self()->index && !acpi_os_irq_context()) return &worker_marker;
    struct cpu* c = cpu_self();
    return c->task ? (void*)c->task : (void*)c;
}
struct acpi_mutex { void* volatile owner; };
uacpi_handle uacpi_kernel_create_mutex(void) { return acpi_os_alloc(sizeof(struct acpi_mutex)); }
void uacpi_kernel_free_mutex(uacpi_handle m) { acpi_os_free(m, sizeof(struct acpi_mutex)); }
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    struct acpi_mutex* m = handle; void* self = uacpi_kernel_get_thread_id();
    uint64_t deadline = timeout == 0xFFFF ? ~0ULL : native_clock_ns() + (uint64_t)timeout * 1000000ULL;
    for (;;) {
        void* expected = NULL;
        if (__atomic_compare_exchange_n(&m->owner, &expected, self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return UACPI_STATUS_OK;
        if (!timeout || native_clock_ns() >= deadline) return UACPI_STATUS_TIMEOUT;
        wait_slice(m, deadline);
    }
}
void uacpi_kernel_release_mutex(uacpi_handle handle) {
    struct acpi_mutex* m = handle;
    __atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);
    task_wake_channel(m);
}
struct acpi_event { volatile uint64_t count; };
uacpi_handle uacpi_kernel_create_event(void) { return acpi_os_alloc(sizeof(struct acpi_event)); }
void uacpi_kernel_free_event(uacpi_handle e) { acpi_os_free(e, sizeof(struct acpi_event)); }
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    struct acpi_event* e = handle;
    uint64_t deadline = timeout == 0xFFFF ? ~0ULL : native_clock_ns() + (uint64_t)timeout * 1000000ULL;
    for (;;) {
        uint64_t count = __atomic_load_n(&e->count, __ATOMIC_ACQUIRE);
        while (count) if (__atomic_compare_exchange_n(&e->count, &count, count - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return UACPI_TRUE;
        if (!timeout || native_clock_ns() >= deadline) return UACPI_FALSE;
        wait_slice(e, deadline);
    }
}
void uacpi_kernel_signal_event(uacpi_handle handle) { struct acpi_event* e = handle; __atomic_add_fetch(&e->count, 1, __ATOMIC_RELEASE); task_wake_channel(e); }
void uacpi_kernel_reset_event(uacpi_handle handle) { struct acpi_event* e = handle; __atomic_store_n(&e->count, 0, __ATOMIC_RELEASE); }
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return irq_save(); }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) { irq_restore(state); }
struct acpi_spinlock { volatile int lock; };
uacpi_handle uacpi_kernel_create_spinlock(void) { return acpi_os_alloc(sizeof(struct acpi_spinlock)); }
void uacpi_kernel_free_spinlock(uacpi_handle s) { acpi_os_free(s, sizeof(struct acpi_spinlock)); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    uint64_t f = irq_save(); spin_acquire(&((struct acpi_spinlock*)handle)->lock); return f;
}
void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    spin_release(&((struct acpi_spinlock*)handle)->lock); irq_restore(flags);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* request) {
    char text[24];
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL) {
        vga_print("[64] ACPI: AML Fatal(type "); uint_to_str(request->fatal.type, text); vga_print(text);
        vga_print(", code "); uint_to_str(request->fatal.code, text); vga_print(text); vga_print(")\n");
    } else vga_print("[64] ACPI: AML Breakpoint\n");
    return UACPI_STATUS_OK;
}

/* ---- SCI and GPE-block interrupts ---- */
struct acpi_irq { uacpi_interrupt_handler handler; uacpi_handle context; int vector; };
/* Trigger/polarity that a device's _CRS gave for an interrupt uACPI will install (GPE block devices); without a
 * hint an ISA line is the SCI's shareable level/active-low unless the MADT overrides it. */
static struct { uint32_t irq; bool level, low, used; } irq_hints[8];
void acpi_os_irq_hint(uint32_t irq, bool level, bool active_low) {
    for (unsigned i = 0; i < 8; i++) if (!irq_hints[i].used || irq_hints[i].irq == irq) {
        irq_hints[i].irq = irq; irq_hints[i].level = level; irq_hints[i].low = active_low; irq_hints[i].used = true; return;
    }
}
static void acpi_irq_entry(unsigned vector, void* context) {
    (void)vector; struct acpi_irq* binding = context;
    unsigned bit = 1U << cpu_self()->index;
    __atomic_add_fetch(&irq_depth, 1, __ATOMIC_ACQ_REL); __atomic_or_fetch(&irq_cpu_mask, (int)bit, __ATOMIC_ACQ_REL);
    binding->handler(binding->context);
    __atomic_and_fetch(&irq_cpu_mask, (int)~bit, __ATOMIC_ACQ_REL); __atomic_sub_fetch(&irq_depth, 1, __ATOMIC_ACQ_REL);
}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle context, uacpi_handle* out) {
    struct acpi_irq* binding = acpi_os_alloc(sizeof(*binding)); if (!binding) return UACPI_STATUS_OUT_OF_MEMORY;
    binding->handler = handler; binding->context = context;
    /* The SCI: shareable, level-triggered, active-low unless the MADT overrides it (ACPI 5.2.12.5). */
    bool level = true, low = true, hinted = false;
    for (unsigned i = 0; i < 8; i++) if (irq_hints[i].used && irq_hints[i].irq == irq) { level = irq_hints[i].level; low = irq_hints[i].low; hinted = true; }
    int vector = irq < 16 ? irq_attach_isa(irq, hinted && !level && !low ? 0 : IRQ_PCI_INTX, acpi_irq_entry, binding) :
                            irq_attach_gsi(irq, 0, level, low, acpi_irq_entry, binding);
    if (vector < 0) { acpi_os_free(binding, sizeof(*binding)); return UACPI_STATUS_INTERNAL_ERROR; }
    binding->vector = vector; *out = binding;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle handle) {
    struct acpi_irq* binding = handle;
    if (!binding || binding->handler != handler) return UACPI_STATUS_INVALID_ARGUMENT;
    irq_unregister((unsigned)binding->vector, acpi_irq_entry, binding);
    while (__atomic_load_n(&irq_depth, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    acpi_os_free(binding, sizeof(*binding));
    return UACPI_STATUS_OK;
}
