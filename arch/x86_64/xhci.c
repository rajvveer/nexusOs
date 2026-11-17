/* xHCI host controllers (USB 3): BIOS hand-off through the USB Legacy Support capability, reset,
 * DCBAA (+ scratchpad buffers), one command ring, one event ring with a single-segment ERST,
 * root-port status changes, USB2 port reset and USB3 warm reset, slot/endpoint commands (Enable Slot,
 * Address Device, Evaluate Context, Configure Endpoint, Reset Endpoint, Set TR Dequeue Pointer,
 * Disable Slot), control transfers (Setup/Data/Status TRBs) and interrupt IN transfers.
 *
 * Supported Protocol capabilities say which ports are USB2 and which USB3 (QEMU's nec-usb-xhci
 * lists the USB3 ports first, qemu-xhci last). Contexts are 32 or 64 bytes (HCCPARAMS1.CSZ).
 * Ring/context memory comes from a per-controller page pool reserved at boot, so hot-plug never
 * changes the kernel's page accounting. Commands are issued from the USB worker only (usb.c), one
 * at a time; event processing runs from usb_poll()/usb_irq() and only records completions, feeds
 * periodic endpoints and notes port changes. */
#include "usb.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define MAX_CONTROLLERS 8   /* AM5: CPU (2) + chipset (1-2) + USB4/ASMedia add-on controllers */
#define MAX_PORTS 64
#define MAX_SLOTS 32
#define POOL_PAGES 160
#define COMMAND_TRBS 64
#define EVENT_TRBS 256
#define PAGE 4096ULL

/* TRB types and fields */
enum { TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6, TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10,
       TRB_ADDRESS = 11, TRB_CONFIGURE = 12, TRB_EVALUATE = 13, TRB_RESET_ENDPOINT = 14, TRB_STOP_ENDPOINT = 15,
       TRB_SET_DEQUEUE = 16, TRB_TRANSFER_EVENT = 32, TRB_COMMAND_EVENT = 33, TRB_PORT_EVENT = 34, TRB_HOST_EVENT = 37 };
#define TRB_CYCLE 1U
#define TRB_TOGGLE 2U
#define TRB_ISP (1U << 2)
#define TRB_IOC (1U << 5)
#define TRB_IDT (1U << 6)
#define TRB_TYPE(type) ((uint32_t)(type) << 10)
#define TRB_IN (1U << 16)
enum { CC_SUCCESS = 1, CC_SHORT = 13, CC_STOPPED = 26, CC_STOPPED_LENGTH = 27 };
#define TRB_ISOCH 5
/* Operational registers */
#define USBCMD 0x00
#define USBSTS 0x04
#define PAGESIZE 0x08
#define CRCR 0x18
#define DCBAAP 0x30
#define CONFIG 0x38
#define PORTSC(port) (0x400 + 0x10 * ((port) - 1))
#define CMD_RUN 1U
#define CMD_RESET 2U
#define CMD_INTE 4U
#define STS_HALTED 1U
#define STS_EINT 8U
#define STS_NOT_READY (1U << 11)
/* PORTSC */
#define PORT_CCS 1U
#define PORT_PED 2U
#define PORT_PR (1U << 4)
#define PORT_PP (1U << 9)
#define PORT_CHANGES 0x00FE0000U /* CSC PEC WRC OCC PRC PLC CEC */
#define PORT_CSC (1U << 17)
#define PORT_PRC (1U << 21)
#define PORT_WRC (1U << 19)
#define PORT_WPR (1U << 31)
#define PORT_PRESERVE 0x4E00FFE9U /* RO and RWS bits: writing them back is neutral */
/* Interrupter 0 (runtime base + 0x20) */
#define IMAN 0x20
#define IMOD 0x24
#define ERSTSZ 0x28
#define ERSTBA 0x30
#define ERDP 0x38

struct trb { volatile uint64_t parameter; volatile uint32_t status, control; };
static uint64_t dma(uint64_t address) { return virt_to_phys((void*)address); } /* kernel address -> controller address */
struct xhci {
    const pci_device_t* pci;
    volatile uint8_t *base, *op, *runtime;
    volatile uint32_t* doorbells;
    unsigned index, vector, slots_enabled, ports, context_size, scratchpads;
    uint64_t dcbaa, command_ring, event_ring, erst, input_context, scratch, scratchpad_array, pool;
    unsigned command_enqueue, event_dequeue;
    bool command_cycle, event_cycle, processing, dead;
    volatile bool command_done;
    uint8_t command_code, command_slot;
    uint64_t command_trb;
    volatile uint64_t port_pending;
    uint8_t protocol[MAX_PORTS + 1];
    struct usb_device* slots[MAX_SLOTS + 1];
    uint32_t pool_used[(POOL_PAGES + 31) / 32];
    char name[8];
    bool ac64;              /* HCCPARAMS1.AC64: 64-bit DMA addresses */
    unsigned max_psa;       /* HCCPARAMS1.MaxPSASize (0: no streams) */
};
static struct xhci* controllers[MAX_CONTROLLERS];
static unsigned controller_count;
static volatile bool halted_endpoints;

unsigned xhci_count(void) { return controller_count; }
struct xhci* xhci_get(unsigned index) { return index < controller_count ? controllers[index] : NULL; }
unsigned xhci_port_count(const struct xhci* hc) { return hc->ports; }
const char* xhci_name(const struct xhci* hc) { return hc->name; }
const pci_device_t* xhci_pci(const struct xhci* hc) { return hc ? hc->pci : NULL; }
uint64_t xhci_scratch(struct xhci* hc) { return hc->scratch; }
unsigned xhci_pool_used(const struct xhci* hc) {
    unsigned used = 0;
    for (unsigned i = 0; i < POOL_PAGES; i++) used += (hc->pool_used[i / 32] >> (i % 32)) & 1;
    return used;
}
bool usb_endpoint_halted_pending(void) { bool pending = halted_endpoints; halted_endpoints = false; return pending; }

static uint32_t op_read(struct xhci* hc, unsigned reg) { return mmio_read32(hc->op, reg); }
static void op_write(struct xhci* hc, unsigned reg, uint32_t value) { mmio_write32(hc->op, reg, value); }
static uint32_t* input_dword(struct xhci* hc, unsigned context, unsigned dword) {
    return (uint32_t*)(hc->input_context + (uint64_t)context * hc->context_size) + dword;
}
/* Output device context entry `dci` (0 = slot). */
static uint32_t* output_dword(struct usb_device* device, unsigned dci, unsigned dword) {
    return (uint32_t*)(device->context + (uint64_t)dci * device->hc->context_size) + dword;
}

uint64_t xhci_page_alloc(struct xhci* hc) {
    for (unsigned i = 0; i < POOL_PAGES; i++) if (!(hc->pool_used[i / 32] & (1U << (i % 32)))) {
        hc->pool_used[i / 32] |= 1U << (i % 32);
        uint64_t page = hc->pool + i * PAGE; memset((void*)page, 0, PAGE); return page;
    }
    return 0;
}
void xhci_page_free(struct xhci* hc, uint64_t page) {
    if (!page) return;
    uint64_t index = (page - hc->pool) / PAGE;
    if (page < hc->pool || index >= POOL_PAGES || !(hc->pool_used[index / 32] & (1U << (index % 32)))) panic("xHCI pool page ownership");
    hc->pool_used[index / 32] &= ~(1U << (index % 32));
}

/* Ring helpers: the last TRB of every ring is a Link back to the start with Toggle Cycle. */
static void ring_init(uint64_t ring, unsigned trbs) {
    struct trb* link = (struct trb*)ring + trbs - 1;
    link->parameter = dma(ring); link->status = 0; link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
}
static struct trb* ring_put(uint64_t ring, unsigned trbs, unsigned* enqueue, bool* cycle, uint64_t parameter, uint32_t status, uint32_t control) {
    struct trb* trb = (struct trb*)ring + *enqueue;
    trb->parameter = parameter; trb->status = status;
    __asm__ volatile("" ::: "memory");
    trb->control = control | (*cycle ? TRB_CYCLE : 0);
    if (++*enqueue == trbs - 1) {
        struct trb* link = (struct trb*)ring + trbs - 1;
        link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE | (*cycle ? TRB_CYCLE : 0);
        *cycle = !*cycle; *enqueue = 0;
    }
    return trb;
}
static void doorbell(struct xhci* hc, unsigned slot, unsigned target) {
    __asm__ volatile("sfence" ::: "memory");
    hc->doorbells[slot] = target;
}

/* ---- Commands (worker context) ---- */
static bool command(struct xhci* hc, uint64_t parameter, uint32_t control, uint8_t* slot) {
    if (hc->dead) return false;
    hc->command_done = false;
    struct trb* trb = ring_put(hc->command_ring, COMMAND_TRBS, &hc->command_enqueue, &hc->command_cycle, parameter, 0, control);
    hc->command_trb = dma((uint64_t)trb);
    doorbell(hc, 0, 0);
    uint64_t deadline = native_clock_ns() + 5000000000ULL;
    while (!hc->command_done) {
        if (native_clock_ns() >= deadline) {
            usb_log_number("[64] USB xHCI command timed out type=", (control >> 10) & 63, false);
            uint32_t status = op_read(hc, USBSTS);
            if (status == 0xFFFFFFFFU || (status & ((1U << 2) | (1U << 12) | STS_HALTED))) {
                hc->dead = true; usb_log_number("[64] USB xHCI controller stopped responding, USBSTS=", status, true);
            }
            hc->command_trb = 0; return false;
        }
        usb_sleep(0);
    }
    if (slot) *slot = hc->command_slot;
    if (hc->command_code != CC_SUCCESS) {
        usb_log_number("[64] USB xHCI command failed type=", (control >> 10) & 63, false);
        usb_log_number("[64] USB xHCI completion code=", hc->command_code, false);
    }
    return hc->command_code == CC_SUCCESS;
}

/* ---- Root ports ---- */
bool xhci_take_port_change(struct xhci* hc, unsigned* port) {
    uint64_t pending = hc->port_pending;
    if (!pending) return false;
    unsigned bit = (unsigned)__builtin_ctzll(pending);
    hc->port_pending &= ~(1ULL << bit);
    *port = bit + 1;
    return *port <= hc->ports;
}
bool xhci_port_service(struct xhci* hc, unsigned port, unsigned* speed, bool* connected, bool* changed) {
    uint32_t status = op_read(hc, PORTSC(port));
    if (status == 0xFFFFFFFFU) { *connected = false; *changed = true; return false; }
    if (status & PORT_CHANGES) op_write(hc, PORTSC(port), (status & PORT_PRESERVE) | (status & PORT_CHANGES));
    *speed = (status >> 10) & 15; *connected = (status & PORT_CCS) != 0; *changed = (status & PORT_CSC) != 0;
    return (status & PORT_PED) != 0;
}
/* USB2 ports: port reset enables the port. USB3 ports enable themselves after link training; a port
 * stuck without enable gets a warm reset. Waits by yielding. */
bool xhci_port_reset(struct xhci* hc, unsigned port, unsigned* speed) {
    bool usb3 = hc->protocol[port] == 3;
    uint32_t status = op_read(hc, PORTSC(port));
    if (!(status & PORT_CCS)) return false;
    if (usb3 && (status & PORT_PED)) { *speed = (status >> 10) & 15; return true; }
    op_write(hc, PORTSC(port), (status & PORT_PRESERVE) | (usb3 ? PORT_WPR : PORT_PR));
    uint64_t deadline = native_clock_ns() + 500000000ULL;
    for (;;) {
        usb_sleep(1000000ULL);
        status = op_read(hc, PORTSC(port));
        if (!(status & PORT_CCS)) return false;
        if ((status & (PORT_PRC | PORT_WRC)) && !(status & PORT_PR)) break;
        if (native_clock_ns() >= deadline) { vga_print("[64] USB root port reset timed out\n"); return false; }
    }
    op_write(hc, PORTSC(port), (status & PORT_PRESERVE) | (status & (PORT_PRC | PORT_WRC)));
    *speed = (status >> 10) & 15;
    return (status & PORT_PED) != 0;
}

/* ---- Device slots ---- */
static unsigned default_mps0(unsigned speed) { return speed >= USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8; }
static void slot_context(struct usb_device* device, unsigned entries, bool hub, unsigned ports, unsigned think_time, bool multi_tt) {
    struct xhci* hc = device->hc;
    uint32_t* slot = input_dword(hc, 1, 0);
    slot[0] = device->route | ((uint32_t)device->speed << 20) | (multi_tt || device->tt_multi ? 1U << 25 : 0) | (hub ? 1U << 26 : 0) | ((uint32_t)entries << 27);
    slot[1] = device->max_exit_latency | ((uint32_t)device->root_port << 16) | (hub ? (uint32_t)ports << 24 : 0);
    slot[2] = device->tt_slot | ((uint32_t)device->tt_port << 8) | ((uint32_t)(think_time & 3) << 16);
    slot[3] = 0;
}
bool xhci_device_open(struct usb_device* device) {
    struct xhci* hc = device->hc; uint8_t slot = 0;
    if (!command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot)) return false;
    if (!slot || slot > hc->slots_enabled || hc->slots[slot]) {
        vga_print("[64] USB xHCI returned an unusable slot\n");
        if (slot && slot <= hc->slots_enabled && !hc->slots[slot]) command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24), NULL);
        return false;
    }
    device->slot = slot;
    device->context = xhci_page_alloc(hc); device->ep0_ring = xhci_page_alloc(hc);
    if (!device->context || !device->ep0_ring) {
        xhci_page_free(hc, device->context); xhci_page_free(hc, device->ep0_ring); device->context = device->ep0_ring = 0;
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24), NULL); device->slot = 0;
        vga_print("[64] USB xHCI page pool exhausted\n"); return false;
    }
    hc->slots[slot] = device;
    ((volatile uint64_t*)hc->dcbaa)[slot] = dma(device->context);
    ring_init(device->ep0_ring, USB_RING_TRBS); device->ep0_enqueue = 0; device->ep0_cycle = true;
    device->mps0 = (uint16_t)default_mps0(device->speed);
    memset((void*)hc->input_context, 0, PAGE);
    *input_dword(hc, 0, 1) = 3; /* add slot + EP0 */
    slot_context(device, 1, false, 0, 0, false);
    uint32_t* ep0 = input_dword(hc, 2, 0);
    ep0[1] = (3U << 1) | (4U << 3) | ((uint32_t)device->mps0 << 16); /* CErr 3, control */
    ep0[2] = (uint32_t)dma(device->ep0_ring) | 1U; ep0[3] = (uint32_t)(dma(device->ep0_ring) >> 32);
    ep0[4] = 8;
    return command(hc, dma(hc->input_context), TRB_TYPE(TRB_ADDRESS) | ((uint32_t)slot << 24), NULL);
}
bool xhci_update_ep0(struct usb_device* device) {
    struct xhci* hc = device->hc;
    memset((void*)hc->input_context, 0, PAGE);
    *input_dword(hc, 0, 1) = 2;
    uint32_t* ep0 = input_dword(hc, 2, 0);
    memcpy(ep0, output_dword(device, 1, 0), 20);
    ep0[1] = (ep0[1] & 0xFFFFU) | ((uint32_t)device->mps0 << 16);
    return command(hc, dma(hc->input_context), TRB_TYPE(TRB_EVALUATE) | ((uint32_t)device->slot << 24), NULL);
}
static unsigned periodic_interval(unsigned speed, unsigned interval) {
    if (speed == USB_SPEED_HIGH || speed >= USB_SPEED_SUPER) { if (!interval) interval = 1; if (interval > 16) interval = 16; return interval - 1; }
    unsigned microframes = (interval ? interval : 1) * 8, exponent = 0;
    while ((2U << exponent) <= microframes) exponent++;
    return exponent < 3 ? 3 : exponent > 10 ? 10 : exponent;
}
/* Add every endpoint not yet configured; `hub` also writes the hub fields of the slot context. */
bool xhci_configure(struct usb_device* device, bool hub, unsigned ports, unsigned think_time, bool multi_tt) {
    struct xhci* hc = device->hc;
    memset((void*)hc->input_context, 0, PAGE);
    uint32_t add = 1, entries = (*output_dword(device, 0, 0) >> 27) & 31;
    for (unsigned i = 0; i < device->endpoint_count; i++) {
        struct usb_endpoint* endpoint = &device->endpoints[i];
        if (!endpoint->page || endpoint->configured) continue;
        unsigned dci = endpoint->dci, burst = endpoint->ss_burst;
        add |= 1U << dci; if (dci > entries) entries = dci;
        uint32_t* context = input_dword(hc, dci + 1, 0);
        uint32_t payload = (uint32_t)endpoint->max_packet * (burst + 1);
        context[0] = (uint32_t)periodic_interval(device->speed, endpoint->interval) << 16 | ((payload >> 16) << 24);
        context[1] = (3U << 1) | (7U << 3) | ((uint32_t)burst << 8) | ((uint32_t)endpoint->max_packet << 16); /* interrupt IN */
        context[2] = (uint32_t)dma(endpoint->page) | 1U; context[3] = (uint32_t)(dma(endpoint->page) >> 32);
        context[4] = (payload & 0xFFFF) | ((payload & 0xFFFF) << 16);
    }
    *input_dword(hc, 0, 1) = add;
    if (!entries) entries = 1;
    slot_context(device, entries, hub, ports, think_time, multi_tt);
    if (!command(hc, dma(hc->input_context), TRB_TYPE(TRB_CONFIGURE) | ((uint32_t)device->slot << 24), NULL)) return false;
    for (unsigned i = 0; i < device->endpoint_count; i++) if (device->endpoints[i].page) device->endpoints[i].configured = true;
    return true;
}
void xhci_device_close(struct usb_device* device) {
    struct xhci* hc = device->hc;
    if (device->slot) {
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)device->slot << 24), NULL);
        ((volatile uint64_t*)hc->dcbaa)[device->slot] = 0;
        hc->slots[device->slot] = NULL;
    }
    for (unsigned i = 0; i < device->endpoint_count; i++) { xhci_page_free(hc, device->endpoints[i].page); device->endpoints[i].page = 0; }
    for (unsigned i = 0; i < device->bulk_count && i < USB_MAX_BULK; i++) { /* mass storage rings, after Disable Slot */
        struct usb_bulk* bulk = &device->bulk[i];
        for (unsigned s = 0; s < USB_BULK_STREAMS; s++) {
            if (bulk->rings[s].active) { bulk->rings[s].active->code = CC_STOPPED; bulk->rings[s].active->done = true; }
            xhci_page_free(hc, bulk->rings[s].page); bulk->rings[s].page = 0; bulk->rings[s].active = NULL;
        }
        xhci_page_free(hc, bulk->stream_array); bulk->stream_array = 0; bulk->configured = false;
    }
    while (device->pipe_count) usb_pipe_close(device->pipes[device->pipe_count - 1]);   /* a class driver left them */
    xhci_page_free(hc, device->context); xhci_page_free(hc, device->ep0_ring);
    device->context = device->ep0_ring = 0; device->slot = 0;
}

/* ---- Transfers ---- */
static bool recover_ring(struct usb_device* device, unsigned dci, uint64_t ring, unsigned enqueue, bool cycle) {
    struct xhci* hc = device->hc;
    if (!device->slot || device->gone) return false;
    uint32_t target = ((uint32_t)dci << 16) | ((uint32_t)device->slot << 24);
    unsigned state = *output_dword(device, dci, 0) & 7;
    if (state == 2 && !command(hc, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | target, NULL)) return false; /* halted */
    state = *output_dword(device, dci, 0) & 7;
    if (state == 1 && !command(hc, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | target, NULL)) return false;  /* running */
    return command(hc, (dma(ring) + (uint64_t)enqueue * 16) | (cycle ? 1 : 0), TRB_TYPE(TRB_SET_DEQUEUE) | target, NULL);
}
int xhci_control_transfer(struct usb_device* device, const uint8_t setup[8], uint64_t buffer, uint16_t length, bool in) {
    if (!device->slot || device->gone || device->hc->dead) return -19;
    uint64_t packet; memcpy(&packet, setup, 8);
    uint32_t transfer = length ? (in ? 3U : 2U) : 0U;
    device->control_done = false; device->control_residual = 0; device->control_code = 0;
    ring_put(device->ep0_ring, USB_RING_TRBS, &device->ep0_enqueue, &device->ep0_cycle, packet, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (transfer << 16));
    device->control_data_trb = 0;
    if (length)
        device->control_data_trb = dma((uint64_t)ring_put(device->ep0_ring, USB_RING_TRBS, &device->ep0_enqueue, &device->ep0_cycle, dma(buffer), length,
                                                    TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_IN : 0)));
    device->control_status_trb = dma((uint64_t)ring_put(device->ep0_ring, USB_RING_TRBS, &device->ep0_enqueue, &device->ep0_cycle, 0, 0,
                                                    TRB_TYPE(TRB_STATUS) | TRB_IOC | (length && in ? 0 : TRB_IN)));
    doorbell(device->hc, device->slot, 1);
    uint64_t deadline = native_clock_ns() + 2000000000ULL;
    while (!device->control_done) {
        if (native_clock_ns() >= deadline || device->gone) {
            recover_ring(device, 1, device->ep0_ring, device->ep0_enqueue, device->ep0_cycle);
            return device->gone ? -19 : -110;
        }
        usb_sleep(0);
    }
    uint8_t code = device->control_code;
    if (code != CC_SUCCESS && code != CC_SHORT) {
        recover_ring(device, 1, device->ep0_ring, device->ep0_enqueue, device->ep0_cycle);
        return code == 6 ? -32 : -71; /* stall: EPIPE; others: EPROTO */
    }
    return length - (int)(device->control_residual > length ? length : device->control_residual);
}
/* Periodic IN endpoints keep four TRBs queued, each pointing at its own buffer in the endpoint page
 * (ring in the first KiB, buffers after it); a completed TRB is re-queued with the same buffer. */
#define IN_BUFFERS 4U
#define IN_BUFFER_BYTES 512U
static uint64_t in_buffer(struct usb_endpoint* endpoint, unsigned index) { return endpoint->page + 1024 + (uint64_t)index * IN_BUFFER_BYTES; }
static unsigned in_length(struct usb_endpoint* endpoint) {
    unsigned length = endpoint->max_packet * (endpoint->ss_burst + 1U);
    return length > IN_BUFFER_BYTES ? IN_BUFFER_BYTES : length ? length : 8;
}
bool xhci_queue_in(struct usb_endpoint* endpoint, unsigned index) {
    struct usb_device* device = endpoint->device;
    if (!device->slot || device->gone || endpoint->halted) return false;
    ring_put(endpoint->page, USB_RING_TRBS, &endpoint->enqueue, &endpoint->cycle, dma(in_buffer(endpoint, index)), in_length(endpoint),
             TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    doorbell(device->hc, device->slot, endpoint->dci);
    return true;
}
bool xhci_endpoint_init(struct usb_endpoint* endpoint) {
    endpoint->page = xhci_page_alloc(endpoint->device->hc);
    if (!endpoint->page) { vga_print("[64] USB xHCI page pool exhausted\n"); return false; }
    ring_init(endpoint->page, USB_RING_TRBS); endpoint->enqueue = 0; endpoint->cycle = true;
    endpoint->configured = endpoint->halted = false; endpoint->failures = 0;
    return true;
}
bool xhci_endpoint_start(struct usb_endpoint* endpoint) {
    for (unsigned i = 0; i < IN_BUFFERS; i++) if (!xhci_queue_in(endpoint, i)) return false;
    return true;
}
bool xhci_recover_endpoint(struct usb_endpoint* endpoint) {
    struct usb_device* device = endpoint->device;
    if (!recover_ring(device, endpoint->dci, endpoint->page, endpoint->enqueue, endpoint->cycle)) return false;
    endpoint->halted = false;
    for (unsigned i = 0; i < IN_BUFFERS; i++) if (!xhci_queue_in(endpoint, i)) return false;
    return true;
}

/* ---- Bulk endpoints and streams (mass storage) ----
 * One page per transfer ring (255 TRBs + Link). A TD is a chain of Normal TRBs, each inside one 64 KiB
 * boundary, ISP on IN TRBs and IOC on the last: it completes with the event of the last TRB (success) or
 * with the first short-packet/error event (later events of that TD are ignored: some controllers also
 * report the IOC TRB after a short packet, QEMU does not). The first TRB is handed over last. */
#define BULK_TRBS 256U
#define TRB_CHAIN (1U << 4)
uint64_t xhci_dma_limit(const struct xhci* hc) { return hc->ac64 ? ~0ULL : 0x100000000ULL; }
bool xhci_streams_supported(const struct xhci* hc) { return hc->max_psa >= 1; }
unsigned xhci_port_count_public(const struct xhci* hc) { return hc->ports; }
bool xhci_port_connected(struct xhci* hc, unsigned port) {
    if (!port || port > hc->ports || hc->dead) return false;
    uint32_t status = op_read(hc, PORTSC(port));
    return status != 0xFFFFFFFFU && (status & PORT_CCS);
}
/* Commands with a status dword (Set TR Dequeue Pointer for a stream); worker only, like command(). */
static bool command_with_status(struct xhci* hc, uint64_t parameter, uint32_t status, uint32_t control) {
    if (hc->dead) return false;
    hc->command_done = false;
    struct trb* trb = ring_put(hc->command_ring, COMMAND_TRBS, &hc->command_enqueue, &hc->command_cycle, parameter, status, control);
    hc->command_trb = dma((uint64_t)trb);
    doorbell(hc, 0, 0);
    uint64_t deadline = native_clock_ns() + 5000000000ULL;
    while (!hc->command_done) {
        if (native_clock_ns() >= deadline) {
            usb_log_number("[64] USB xHCI command timed out type=", (control >> 10) & 63, false);
            uint32_t s = op_read(hc, USBSTS);
            if (s == 0xFFFFFFFFU || (s & ((1U << 2) | (1U << 12) | STS_HALTED))) hc->dead = true;
            hc->command_trb = 0; return false;
        }
        usb_sleep(0);
    }
    return hc->command_code == CC_SUCCESS;
}
static void bulk_ring_reset(struct usb_bulk_ring* ring) {
    memset((void*)ring->page, 0, PAGE); ring_init(ring->page, BULK_TRBS); ring->enqueue = 0; ring->cycle = true; ring->active = NULL;
}
bool xhci_bulk_init(struct usb_bulk* bulk, bool streams) {
    struct xhci* hc = bulk->device->hc;
    bulk->streams = streams ? USB_BULK_STREAMS : 0;
    unsigned rings = streams ? USB_BULK_STREAMS : 1;
    for (unsigned s = streams ? 1 : 0; s < rings; s++) {
        bulk->rings[s].page = xhci_page_alloc(hc);
        if (!bulk->rings[s].page) goto exhausted;
        bulk_ring_reset(&bulk->rings[s]);
    }
    if (streams) {
        bulk->stream_array = xhci_page_alloc(hc);
        if (!bulk->stream_array) goto exhausted;
        uint64_t* contexts = (uint64_t*)bulk->stream_array;   /* 16-byte stream contexts; entry 0 reserved */
        for (unsigned s = 1; s < USB_BULK_STREAMS; s++) { contexts[2 * s] = dma(bulk->rings[s].page) | 1U | (1U << 1); contexts[2 * s + 1] = 0; }
    }
    bulk->configured = bulk->halted = false;
    return true;
exhausted:
    for (unsigned s = 0; s < USB_BULK_STREAMS; s++) { xhci_page_free(hc, bulk->rings[s].page); bulk->rings[s].page = 0; }
    vga_print("[64] USB xHCI page pool exhausted (bulk rings)\n");
    return false;
}
bool xhci_bulk_configure(struct usb_device* device) {
    struct xhci* hc = device->hc;
    memset((void*)hc->input_context, 0, PAGE);
    uint32_t add = 1, entries = (*output_dword(device, 0, 0) >> 27) & 31;
    for (unsigned i = 0; i < device->bulk_count; i++) {
        struct usb_bulk* bulk = &device->bulk[i];
        if (bulk->configured || (!bulk->rings[0].page && !bulk->stream_array)) continue;
        unsigned dci = bulk->dci; add |= 1U << dci; if (dci > entries) entries = dci;
        uint32_t* context = input_dword(hc, dci + 1, 0);
        uint64_t dequeue = bulk->streams ? dma(bulk->stream_array) : dma(bulk->rings[0].page) | 1U;
        context[0] = bulk->streams ? (1U << 10) | (1U << 15) : 0;   /* MaxPStreams 1 (4 contexts), linear stream array */
        context[1] = (3U << 1) | ((bulk->in ? 6U : 2U) << 3) | ((uint32_t)bulk->burst << 8) | ((uint32_t)bulk->max_packet << 16);
        context[2] = (uint32_t)dequeue; context[3] = (uint32_t)(dequeue >> 32);
        context[4] = 3072;                                         /* average TRB length (bulk) */
    }
    if (add == 1) return true;
    *input_dword(hc, 0, 1) = add;
    slot_context(device, entries, false, 0, 0, false);
    if (!command(hc, dma(hc->input_context), TRB_TYPE(TRB_CONFIGURE) | ((uint32_t)device->slot << 24), NULL)) return false;
    for (unsigned i = 0; i < device->bulk_count; i++) device->bulk[i].configured = true;
    return true;
}
static void bulk_put(struct usb_bulk_ring* ring, uint64_t parameter, uint32_t status, uint32_t control, bool chain, bool owned) {
    struct trb* trb = (struct trb*)ring->page + ring->enqueue;
    trb->parameter = parameter; trb->status = status;
    __asm__ volatile("" ::: "memory");
    trb->control = control | ((ring->cycle ? TRB_CYCLE : 0) ^ (owned ? 0 : TRB_CYCLE));
    if (++ring->enqueue == BULK_TRBS - 1) {
        struct trb* link = (struct trb*)ring->page + BULK_TRBS - 1;
        __asm__ volatile("" ::: "memory");
        link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE | (chain ? TRB_CHAIN : 0) | (ring->cycle ? TRB_CYCLE : 0);
        ring->cycle = !ring->cycle; ring->enqueue = 0;
    }
}
int xhci_bulk_submit(struct usb_bulk* bulk, unsigned stream, uint64_t physical, uint32_t length, struct usb_bulk_transfer* t) {
    struct usb_device* device = bulk->device;
    if (!device || !device->slot || device->gone || device->hc->dead) return -19;
    if (!bulk->configured || bulk->halted) return -32;
    if (bulk->streams ? !stream || stream >= USB_BULK_STREAMS : stream != 0) return -22;
    struct usb_bulk_ring* ring = &bulk->rings[stream];
    if (!ring->page || (ring->active && !ring->active->done)) return -16;
    if (!length || physical + length < physical || physical + length > xhci_dma_limit(device->hc)) return -22;
    memset(t, 0, sizeof(*t)); t->length = length;
    uint64_t at = physical; uint32_t left = length; unsigned count = 0;
    while (left) {
        if (count == USB_BULK_MAX_TRBS) return -22;
        uint32_t chunk = 0x10000U - (uint32_t)(at & 0xFFFF); if (chunk > left) chunk = left;
        t->lengths[count++] = chunk; at += chunk; left -= chunk;
    }
    t->first = ring->enqueue; t->count = count; ring->active = t;
    struct trb* first = (struct trb*)ring->page + ring->enqueue;
    uint32_t done = 0, packet = bulk->max_packet ? bulk->max_packet : 512;
    at = physical;
    for (unsigned i = 0; i < count; i++) {
        bool last = i + 1 == count;
        done += t->lengths[i];
        uint32_t packets = last ? 0 : (length - done + packet - 1) / packet; if (packets > 31) packets = 31; /* TD Size */
        bulk_put(ring, at, t->lengths[i] | (packets << 17), TRB_TYPE(TRB_NORMAL) | (last ? TRB_IOC : TRB_CHAIN) | (bulk->in ? TRB_ISP : 0), !last, i != 0);
        at += t->lengths[i];
    }
    __asm__ volatile("sfence" ::: "memory");
    first->control ^= TRB_CYCLE;            /* hand the whole TD over at once */
    doorbell(device->hc, device->slot, bulk->dci | ((uint32_t)stream << 16));
    return 0;
}
/* Halted (stall, babble, transaction error) or abandoned (timeout, cancelled) endpoint: Reset Endpoint when
 * halted, Stop Endpoint when still running, then every ring's dequeue pointer jumps to its enqueue pointer;
 * transfers still queued complete as stopped. The device side (CLEAR_FEATURE(HALT)) is the class driver's. */
bool xhci_bulk_reset(struct usb_bulk* bulk) {
    struct usb_device* device = bulk->device;
    if (!device || !device->slot || device->gone || device->hc->dead || !bulk->configured) return false;
    struct xhci* hc = device->hc;
    uint32_t target = ((uint32_t)bulk->dci << 16) | ((uint32_t)device->slot << 24);
    unsigned state = *output_dword(device, bulk->dci, 0) & 7;
    if (state == 2 && !command(hc, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | target, NULL)) return false;
    state = *output_dword(device, bulk->dci, 0) & 7;
    if (state == 1 && !command(hc, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | target, NULL)) return false;
    for (unsigned s = bulk->streams ? 1 : 0; s < (bulk->streams ? USB_BULK_STREAMS : 1u); s++) {
        struct usb_bulk_ring* ring = &bulk->rings[s];
        if (ring->active && !ring->active->done) { ring->active->code = CC_STOPPED; ring->active->done = true; }
        ring->active = NULL;
        uint64_t dequeue = (dma(ring->page) + (uint64_t)ring->enqueue * 16) | (ring->cycle ? 1U : 0) | (bulk->streams ? 1U << 1 : 0);
        if (!command_with_status(hc, dequeue, (uint32_t)s << 16, TRB_TYPE(TRB_SET_DEQUEUE) | target)) return false;
    }
    bulk->halted = false;
    return true;
}
static bool bulk_event(struct usb_device* device, unsigned dci, uint64_t pointer, unsigned code, uint32_t residual) {
    for (unsigned b = 0; b < device->bulk_count && b < USB_MAX_BULK; b++) {
        struct usb_bulk* bulk = &device->bulk[b];
        if (bulk->dci != dci) continue;
        for (unsigned s = 0; s < USB_BULK_STREAMS; s++) {
            struct usb_bulk_ring* ring = &bulk->rings[s];
            if (!ring->page || (pointer & ~(PAGE - 1)) != dma(ring->page)) continue;
            unsigned index = (unsigned)((pointer & (PAGE - 1)) / 16);
            struct usb_bulk_transfer* t = ring->active;
            bool error = code != CC_SUCCESS && code != CC_SHORT && code != CC_STOPPED && code != CC_STOPPED_LENGTH;
            if (error) bulk->halted = true;   /* stall/babble/transaction errors halt the endpoint (xhci_bulk_reset) */
            if (!t || t->done || index >= BULK_TRBS - 1) return true;
            unsigned offset = (index + (BULK_TRBS - 1) - t->first) % (BULK_TRBS - 1);
            if (offset >= t->count) return true;                 /* a late event of an earlier TD */
            if (code == CC_SUCCESS && offset + 1 != t->count) return true;
            uint32_t before = 0; for (unsigned i = 0; i < offset; i++) before += t->lengths[i];
            uint32_t moved = code == CC_SUCCESS || code == CC_SHORT || code == CC_STOPPED ?   /* 27: length invalid */
                             t->lengths[offset] - (residual > t->lengths[offset] ? t->lengths[offset] : residual) : 0;
            t->actual = before + moved; t->code = (uint8_t)code;
            ring->active = NULL;
            __asm__ volatile("" ::: "memory");
            t->done = true;
            return true;
        }
        return true;
    }
    return false;
}

/* ---- Pipes: isochronous and interrupt endpoints of the class drivers (usbclass.c: audio, video, game controllers) ----
 * A pipe owns one transfer ring page from the controller pool (255 TRBs + Link). Each TD is one TRB on a buffer the
 * class driver owns (inside one 64 KiB boundary): an Isoch TRB with Start Isoch ASAP (the controller schedules queued TDs
 * back to back, one per service interval) or a Normal TRB for interrupt/bulk endpoints, always with IOC. Events arrive
 * in ring order, so an event for TRB k also completes every older TD the controller skipped without an event (Missed
 * Service); a Ring Underrun/Overrun (isochronous ring ran empty) is only informational. Completions
 * call the owner's callback from event processing (kernel lock held) with its tag, the bytes moved and the completion
 * code. Configure/stop are commands: USB worker only. */
#define PIPE_TRBS USB_PIPE_TRBS
enum { CC_MISSED_SERVICE = 23, CC_RING_UNDERRUN = 14, CC_RING_OVERRUN = 15, CC_CANCELLED = 26 };
static void pipe_ring_reset(struct usb_pipe* pipe) {
    memset((void*)pipe->ring, 0, PAGE); ring_init(pipe->ring, PIPE_TRBS);
    pipe->enqueue = pipe->dequeue = pipe->queued = 0; pipe->cycle = true;
}
bool usb_pipe_open(struct usb_device* device, struct usb_pipe* pipe) {
    if (device->pipe_count >= USB_MAX_PIPES || !device->slot || device->gone) return false;
    pipe->device = device;
    pipe->dci = (uint8_t)((pipe->address & 15) * 2 + ((pipe->address & 0x80) ? 1 : 0));
    pipe->in = (pipe->address & 0x80) != 0;
    if (pipe->dci < 2) return false;
    pipe->ring = xhci_page_alloc(device->hc);
    if (!pipe->ring) { vga_print("[64] USB xHCI page pool exhausted (pipe ring)\n"); return false; }
    pipe_ring_reset(pipe); pipe->configured = pipe->halted = false;
    device->pipes[device->pipe_count++] = pipe;
    return true;
}
static void pipe_cancel_all(struct usb_pipe* pipe, unsigned code) {
    /* Only the TDs outstanding now: a callback may queue new ones (they belong to the ring that follows). */
    for (unsigned count = pipe->queued; count && pipe->queued; count--) {
        uint32_t tag = pipe->tags[pipe->dequeue];
        pipe->dequeue = (pipe->dequeue + 1) % (PIPE_TRBS - 1); pipe->queued--;
        if (pipe->complete) pipe->complete(pipe, tag, 0, code);
    }
}
void usb_pipe_close(struct usb_pipe* pipe) {
    struct usb_device* device = pipe->device;
    if (!device) return;
    for (unsigned i = 0; i < device->pipe_count; i++) if (device->pipes[i] == pipe) {
        for (unsigned j = i + 1; j < device->pipe_count; j++) device->pipes[j - 1] = device->pipes[j];
        device->pipe_count--; break;
    }
    pipe->queued = 0;   /* no callbacks: the owner is tearing down */
    if (device->hc && pipe->ring) xhci_page_free(device->hc, pipe->ring);
    pipe->ring = 0; pipe->configured = false; pipe->device = NULL;
}
static unsigned pipe_interval(const struct usb_device* device, const struct usb_pipe* pipe) {
    unsigned raw = pipe->interval ? pipe->interval : 1;
    if (device->speed == USB_SPEED_HIGH || device->speed >= USB_SPEED_SUPER) return raw > 16 ? 15 : raw - 1;
    if (pipe->type == USB_PIPE_ISOCH) return (raw > 16 ? 16 : raw) - 1 + 3;     /* full speed: 2^(bInterval-1) frames */
    return periodic_interval(device->speed, raw);
}
/* The highest endpoint context still in use (slot context Context Entries). */
static unsigned last_context(struct usb_device* device, const struct usb_pipe* const* drop, unsigned drops) {
    unsigned last = 1;
    for (unsigned i = 0; i < device->endpoint_count; i++) if (device->endpoints[i].page && device->endpoints[i].configured && device->endpoints[i].dci > last) last = device->endpoints[i].dci;
    for (unsigned i = 0; i < device->bulk_count && i < USB_MAX_BULK; i++) if (device->bulk[i].configured && device->bulk[i].dci > last) last = device->bulk[i].dci;
    for (unsigned i = 0; i < device->pipe_count; i++) {
        const struct usb_pipe* p = device->pipes[i]; bool dropped = false;
        for (unsigned k = 0; k < drops; k++) dropped |= drop[k] == p;
        if (p->configured && !dropped && p->dci > last) last = p->dci;
    }
    return last;
}
/* Configure Endpoint: drop (alternate setting going away) and add pipes in one command. Dropped pipes lose their
 * queued TDs (completed as cancelled) and keep their ring for a later add. */
bool usb_pipes_configure(struct usb_device* device, struct usb_pipe* const* add, unsigned adds, struct usb_pipe* const* drop, unsigned drops) {
    struct xhci* hc = device->hc;
    if (!device->slot || device->gone || hc->dead) return false;
    memset((void*)hc->input_context, 0, PAGE);
    uint32_t drop_flags = 0, add_flags = 1;
    for (unsigned k = 0; k < drops; k++) if (drop[k] && drop[k]->configured) drop_flags |= 1U << drop[k]->dci;
    unsigned entries = last_context(device, (const struct usb_pipe* const*)drop, drops);
    for (unsigned k = 0; k < adds; k++) {
        struct usb_pipe* pipe = add[k];
        if (!pipe || !pipe->ring) return false;
        if (pipe->configured) drop_flags |= 1U << pipe->dci;          /* re-add: replace the context */
        pipe_ring_reset(pipe);
        unsigned dci = pipe->dci, burst = pipe->burst, mult = pipe->mult;
        add_flags |= 1U << dci; if (dci > entries) entries = dci;
        uint32_t* context = input_dword(hc, dci + 1, 0);
        uint32_t payload = (uint32_t)pipe->max_packet * (burst + 1) * (mult + 1);
        unsigned type = pipe->type == USB_PIPE_ISOCH ? (pipe->in ? 5U : 1U) : pipe->type == USB_PIPE_INTERRUPT ? (pipe->in ? 7U : 3U) : (pipe->in ? 6U : 2U);
        context[0] = ((uint32_t)mult << 8) | (pipe->type == USB_PIPE_BULK ? 0 : (uint32_t)pipe_interval(device, pipe) << 16) | ((payload >> 16) << 24);
        context[1] = (pipe->type == USB_PIPE_ISOCH ? 0 : 3U << 1) | (type << 3) | ((uint32_t)burst << 8) | ((uint32_t)pipe->max_packet << 16);
        context[2] = (uint32_t)dma(pipe->ring) | 1U; context[3] = (uint32_t)(dma(pipe->ring) >> 32);
        context[4] = (pipe->type == USB_PIPE_BULK ? 3072U : payload & 0xFFFF) | ((payload & 0xFFFF) << 16);
    }
    *input_dword(hc, 0, 0) = drop_flags & ~3U; *input_dword(hc, 0, 1) = add_flags;
    slot_context(device, entries, false, 0, 0, false);
    if (!command(hc, dma(hc->input_context), TRB_TYPE(TRB_CONFIGURE) | ((uint32_t)device->slot << 24), NULL)) return false;
    for (unsigned k = 0; k < drops; k++) if (drop[k] && drop[k]->configured) { drop[k]->configured = false; pipe_cancel_all(drop[k], CC_CANCELLED); }
    for (unsigned k = 0; k < adds; k++) { add[k]->configured = true; add[k]->halted = false; }
    return true;
}
/* Queue one TD. Returns 0, -EBUSY (ring full), -ENODEV, -EPIPE (halted/not configured) or -EINVAL. */
int usb_pipe_queue(struct usb_pipe* pipe, uint64_t physical, uint32_t length, uint32_t tag) {
    struct usb_device* device = pipe->device;
    if (!device || !device->slot || device->gone || device->hc->dead) return -19;
    if (!pipe->configured || pipe->halted) return -32;
    if (pipe->queued >= PIPE_TRBS - 3) return -16;
    if (length > 0x10000 || (length && ((physical & 0xFFFF) + length > 0x10000 || physical + length > xhci_dma_limit(device->hc)))) return -22;
    unsigned index = pipe->enqueue;
    uint32_t control;
    if (pipe->type == USB_PIPE_ISOCH) {
        unsigned packet = pipe->max_packet ? pipe->max_packet : 1, burst = pipe->burst + 1U;
        unsigned packets = length ? (length + packet - 1) / packet : 1;
        unsigned tbc = (packets + burst - 1) / burst - 1, tlbpc = packets % burst ? packets % burst - 1 : burst - 1;
        control = TRB_TYPE(TRB_ISOCH) | TRB_IOC | (pipe->in ? TRB_ISP : 0) | ((tbc & 3U) << 7) | ((tlbpc & 15U) << 16) | (1U << 31); /* SIA */
    } else control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | (pipe->in ? TRB_ISP : 0);
    pipe->tags[index] = tag; pipe->lengths[index] = length;
    ring_put(pipe->ring, PIPE_TRBS, &pipe->enqueue, &pipe->cycle, physical, length, control);
    pipe->queued++;
    doorbell(device->hc, device->slot, pipe->dci);
    return 0;
}
/* Worker: halted or running pipe -> stopped with an empty ring; outstanding TDs complete as cancelled. */
bool usb_pipe_stop(struct usb_pipe* pipe) {
    struct usb_device* device = pipe->device;
    if (!device || !device->slot || device->gone || device->hc->dead || !pipe->configured) { if (pipe->device) pipe_cancel_all(pipe, CC_CANCELLED); return false; }
    struct xhci* hc = device->hc; uint32_t target = ((uint32_t)pipe->dci << 16) | ((uint32_t)device->slot << 24);
    unsigned state = *output_dword(device, pipe->dci, 0) & 7;
    if (state == 2 && !command(hc, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | target, NULL)) return false;   /* halted */
    state = *output_dword(device, pipe->dci, 0) & 7;
    if (state == 1 && !command(hc, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | target, NULL)) return false;    /* running */
    pipe_cancel_all(pipe, CC_CANCELLED);
    pipe_ring_reset(pipe);
    if (!command(hc, dma(pipe->ring) | 1U, TRB_TYPE(TRB_SET_DEQUEUE) | target, NULL)) return false;
    pipe->halted = false;
    return true;
}
uint32_t usb_frame_index(struct usb_device* device) { return device && device->hc ? mmio_read32(device->hc->runtime, 0) & 0x3FFF : 0; }
static bool pipe_event(struct usb_device* device, unsigned dci, uint64_t pointer, unsigned code, uint32_t residual) {
    struct usb_pipe* pipe = NULL;
    for (unsigned i = 0; i < device->pipe_count && !pipe; i++) if (device->pipes[i]->dci == dci) pipe = device->pipes[i];
    if (!pipe) return false;
    /* Ring Underrun/Overrun: the isochronous ring was empty at a service opportunity. Informational: TDs queued since
       then are valid and run after their doorbell. Events without a TRB pointer carry nothing to complete. */
    if (!pipe->queued || code == CC_RING_UNDERRUN || code == CC_RING_OVERRUN || !pointer) return true;
    if ((pointer & ~(PAGE - 1)) != dma(pipe->ring)) return true;
    unsigned index = (unsigned)((pointer & (PAGE - 1)) / 16);
    if (index >= PIPE_TRBS - 1) return true;
    unsigned ahead = (index + (PIPE_TRBS - 1) - pipe->dequeue) % (PIPE_TRBS - 1);
    if (ahead >= pipe->queued) return true;                           /* stale event of a cancelled TD */
    while (ahead--) {                                                 /* TDs the controller skipped without an event */
        uint32_t tag = pipe->tags[pipe->dequeue];
        pipe->dequeue = (pipe->dequeue + 1) % (PIPE_TRBS - 1); pipe->queued--;
        if (pipe->complete && !device->gone) pipe->complete(pipe, tag, 0, CC_MISSED_SERVICE);
    }
    uint32_t tag = pipe->tags[index], length = pipe->lengths[index];
    pipe->dequeue = (pipe->dequeue + 1) % (PIPE_TRBS - 1); pipe->queued--;
    bool moved = code == CC_SUCCESS || code == CC_SHORT || code == CC_STOPPED;
    unsigned actual = moved ? length - (residual > length ? length : residual) : 0;
    if (!moved && code != CC_MISSED_SERVICE && code != CC_STOPPED_LENGTH && pipe->type != USB_PIPE_ISOCH) pipe->halted = true;
    if (pipe->complete && !device->gone) pipe->complete(pipe, tag, actual, code);
    return true;
}

/* ---- Events ---- */
static void transfer_event(struct xhci* hc, struct trb* event) {
    unsigned slot = event->control >> 24, dci = (event->control >> 16) & 31, code = event->status >> 24;
    if (!slot || slot > hc->slots_enabled || !hc->slots[slot]) return;
    struct usb_device* device = hc->slots[slot];
    uint64_t pointer = event->parameter;
    if (dci == 1) {
        if (!device->control_status_trb) return;
        if (pointer == device->control_data_trb && (code == CC_SUCCESS || code == CC_SHORT)) { device->control_residual = event->status & 0xFFFFFF; return; }
        if (pointer == device->control_status_trb || (code != CC_SUCCESS && code != CC_SHORT)) {
            device->control_code = (uint8_t)code; device->control_status_trb = 0; device->control_done = true;
        }
        return;
    }
    if (device->bulk_count && bulk_event(device, dci, pointer, code, event->status & 0xFFFFFF)) return;
    if (device->pipe_count && pipe_event(device, dci, pointer, code, event->status & 0xFFFFFF)) return;
    for (unsigned i = 0; i < device->endpoint_count; i++) {
        struct usb_endpoint* endpoint = &device->endpoints[i];
        if (endpoint->dci != dci || !endpoint->page) continue;
        if ((pointer & ~(PAGE - 1)) != dma(endpoint->page) || (pointer & (PAGE - 1)) >= (USB_RING_TRBS - 1) * 16) return;
        uint64_t buffer = ((struct trb*)(endpoint->page + (pointer & (PAGE - 1))))->parameter;
        unsigned index = (unsigned)((buffer - dma(endpoint->page) - 1024) / IN_BUFFER_BYTES);
        if (index >= IN_BUFFERS) return;
        if (code == CC_SUCCESS || code == CC_SHORT) {
            unsigned residual = event->status & 0xFFFFFF, length = in_length(endpoint);
            endpoint->failures = 0;
            if (endpoint->complete && !device->gone) endpoint->complete(endpoint, (const uint8_t*)in_buffer(endpoint, index), residual > length ? 0 : length - residual);
            xhci_queue_in(endpoint, index);
        } else if (code != CC_STOPPED && code != CC_STOPPED_LENGTH && !endpoint->halted && !device->gone) {
            endpoint->halted = true; endpoint->failures++; halted_endpoints = true; usb_worker_kick();
        }
        return;
    }
}
static void handle_event(struct xhci* hc, struct trb* event) {
    unsigned type = (event->control >> 10) & 63;
    if (type == TRB_TRANSFER_EVENT) transfer_event(hc, event);
    else if (type == TRB_COMMAND_EVENT) {
        if (hc->command_trb && event->parameter == hc->command_trb) {
            hc->command_code = (uint8_t)(event->status >> 24); hc->command_slot = (uint8_t)(event->control >> 24);
            hc->command_trb = 0; hc->command_done = true;
        }
    } else if (type == TRB_PORT_EVENT) {
        unsigned port = (unsigned)(event->parameter >> 24) & 255;
        if (port >= 1 && port <= hc->ports) { hc->port_pending |= 1ULL << (port - 1); usb_worker_kick(); }
    } else if (type == TRB_HOST_EVENT) {
        usb_log_number("[64] USB xHCI host controller event code=", event->status >> 24, false);
        for (unsigned port = 1; port <= hc->ports; port++) hc->port_pending |= 1ULL << (port - 1); /* event ring full: rescan */
        usb_worker_kick();
    }
}
static bool process_events(struct xhci* hc) {
    if (hc->processing || hc->dead) return false;
    struct trb* ring = (struct trb*)hc->event_ring;
    if ((ring[hc->event_dequeue].control & TRB_CYCLE) != (hc->event_cycle ? 1U : 0U)) return false;
    hc->processing = true;
    for (unsigned budget = EVENT_TRBS * 2; budget; budget--) {
        struct trb* event = &ring[hc->event_dequeue];
        if ((event->control & TRB_CYCLE) != (hc->event_cycle ? 1U : 0U)) break;
        __asm__ volatile("lfence" ::: "memory");
        handle_event(hc, event);
        if (++hc->event_dequeue == EVENT_TRBS) { hc->event_dequeue = 0; hc->event_cycle = !hc->event_cycle; }
    }
    mmio_write32(hc->runtime, ERDP, (uint32_t)(dma(hc->event_ring) + (uint64_t)hc->event_dequeue * 16) | 8U); /* clear EHB */
    hc->processing = false;
    return true;
}
bool xhci_poll_events(void) {
    bool any = false;
    for (unsigned i = 0; i < controller_count; i++) any |= process_events(controllers[i]);
    return any;
}
bool xhci_polled(void) {   /* tick.c via usb.c: a live controller without an interrupt is serviced by polling */
    for (unsigned i = 0; i < controller_count; i++) if (!controllers[i]->dead && !controllers[i]->vector) return true;
    return false;
}
/* PIC IRQ: acknowledge (the line is level-triggered and may be shared) and process events. */
void xhci_irq(unsigned vector) {
    for (unsigned i = 0; i < controller_count; i++) {
        struct xhci* hc = controllers[i];
        if (hc->vector != vector || hc->dead) continue;
        uint32_t interrupt = mmio_read32(hc->runtime, IMAN);
        if (interrupt == 0xFFFFFFFFU || !(interrupt & 1)) continue;
        op_write(hc, USBSTS, STS_EINT);
        mmio_write32(hc->runtime, IMAN, 3U); /* clear IP, keep IE */
        process_events(hc);
    }
}

/* ---- Controller bring-up ---- */
static bool wait_status(struct xhci* hc, unsigned reg, uint32_t mask, uint32_t value, uint64_t nanoseconds) {
    uint64_t deadline = native_clock_ns() + nanoseconds;
    while ((op_read(hc, reg) & mask) != value) { if (native_clock_ns() >= deadline) return false; boot_wait(); }
    return true;
}
static void extended_capabilities(struct xhci* hc, uint32_t hcc1) {
    unsigned offset = (hcc1 >> 16) << 2;
    for (unsigned guard = 0; offset && guard < 64; guard++) {
        volatile uint8_t* cap = hc->base + offset;
        uint32_t header = mmio_read32(cap, 0); unsigned id = header & 0xFF, next = (header >> 8) & 0xFF;
        if (id == 1) {
            if (header & (1U << 16)) {
                mmio_write32(cap, 0, header | (1U << 24)); /* HC OS Owned */
                uint64_t deadline = native_clock_ns() + 1000000000ULL;
                while ((mmio_read32(cap, 0) & (1U << 16)) && native_clock_ns() < deadline) boot_wait();
                if (mmio_read32(cap, 0) & (1U << 16)) {
                    vga_print("[64] USB xHCI BIOS did not release the controller; taking it\n");
                    mmio_write32(cap, 0, (mmio_read32(cap, 0) & ~(1U << 16)) | (1U << 24));
                }
            } else mmio_write32(cap, 0, header | (1U << 24));
            /* disable SMIs and clear their latched status (RW1C) */
            mmio_write32(cap, 4, (mmio_read32(cap, 4) & ~0x0000E011U) | 0xE0000000U);
        } else if (id == 2) {
            unsigned major = header >> 24;
            uint32_t ports = mmio_read32(cap, 8); unsigned first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (unsigned port = first; port < first + count && port <= MAX_PORTS; port++) if (port) hc->protocol[port] = (uint8_t)major;
        }
        offset = next ? offset + (next << 2) : 0;
    }
}
static bool start(struct xhci* hc) {
    uint64_t size, physical = pci_bar_memory(hc->pci, 0, &size);
    if (!physical || size < 0x1000) { vga_print("[64] USB xHCI without a memory BAR\n"); return false; }
    hc->base = pci_mmio_map(physical, size > 0x100000 ? 0x100000 : size);
    if (!hc->base) { vga_print("[64] USB xHCI registers cannot be mapped\n"); return false; }
    uint16_t command_word = pci_enable_dma(hc->pci);
    uint32_t capability = mmio_read32(hc->base, 0), hcs1 = mmio_read32(hc->base, 4), hcs2 = mmio_read32(hc->base, 8);
    uint32_t hcc1 = mmio_read32(hc->base, 0x10), doorbell_offset = mmio_read32(hc->base, 0x14) & ~3U, runtime_offset = mmio_read32(hc->base, 0x18) & ~0x1FU;
    hc->op = hc->base + (capability & 0xFF); hc->runtime = hc->base + runtime_offset; hc->doorbells = (volatile uint32_t*)(hc->base + doorbell_offset);
    hc->ports = hcs1 >> 24; if (hc->ports > MAX_PORTS) hc->ports = MAX_PORTS;
    hc->slots_enabled = hcs1 & 0xFF; if (hc->slots_enabled > MAX_SLOTS) hc->slots_enabled = MAX_SLOTS;
    hc->context_size = (hcc1 & 4) ? 64 : 32;
    hc->ac64 = (hcc1 & 1) != 0; hc->max_psa = (hcc1 >> 12) & 15;
    hc->scratchpads = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    for (unsigned port = 1; port <= hc->ports; port++) hc->protocol[port] = 2;
    extended_capabilities(hc, hcc1);
    if (!wait_status(hc, USBSTS, STS_NOT_READY, 0, 1000000000ULL)) goto failed;
    op_write(hc, USBCMD, op_read(hc, USBCMD) & ~(CMD_RUN | CMD_INTE));
    if (!wait_status(hc, USBSTS, STS_HALTED, STS_HALTED, 100000000ULL)) goto failed;
    op_write(hc, USBCMD, CMD_RESET);
    for (volatile unsigned i = 0; i < 2000; i++) cpu_pause(); /* some controllers hang if polled right after HCRST */
    if (!wait_status(hc, USBCMD, CMD_RESET, 0, 1000000000ULL) || !wait_status(hc, USBSTS, STS_NOT_READY, 0, 1000000000ULL)) goto failed;
    if (!(op_read(hc, PAGESIZE) & 1)) { vga_print("[64] USB xHCI does not support 4 KiB pages\n"); goto failed; }
    hc->pool = dma32_alloc(POOL_PAGES * PAGE);
    if (!hc->pool) goto failed;
    hc->dcbaa = xhci_page_alloc(hc); hc->command_ring = xhci_page_alloc(hc); hc->event_ring = xhci_page_alloc(hc);
    hc->erst = xhci_page_alloc(hc); hc->input_context = xhci_page_alloc(hc); hc->scratch = xhci_page_alloc(hc);
    if (hc->scratchpads) {
        if (hc->scratchpads > 512) goto failed;
        hc->scratchpad_array = dma32_alloc(PAGE + (uint64_t)hc->scratchpads * PAGE);
        if (!hc->scratchpad_array) goto failed;
        for (unsigned i = 0; i < hc->scratchpads; i++) ((uint64_t*)hc->scratchpad_array)[i] = dma(hc->scratchpad_array) + PAGE + (uint64_t)i * PAGE;
        ((uint64_t*)hc->dcbaa)[0] = dma(hc->scratchpad_array);
    }
    op_write(hc, CONFIG, (op_read(hc, CONFIG) & ~0xFFU) | hc->slots_enabled);
    mmio_write64(hc->op, DCBAAP, dma(hc->dcbaa));
    ring_init(hc->command_ring, COMMAND_TRBS); hc->command_enqueue = 0; hc->command_cycle = true;
    mmio_write64(hc->op, CRCR, dma(hc->command_ring) | 1U);
    ((volatile uint64_t*)hc->erst)[0] = dma(hc->event_ring); ((volatile uint32_t*)hc->erst)[2] = EVENT_TRBS;
    hc->event_dequeue = 0; hc->event_cycle = true;
    mmio_write32(hc->runtime, ERSTSZ, 1);
    mmio_write64(hc->runtime, ERDP, dma(hc->event_ring));
    mmio_write64(hc->runtime, ERSTBA, dma(hc->erst));
    mmio_write32(hc->runtime, IMOD, 4000); /* at most one interrupt per millisecond */
    hc->vector = pci_intx_route(hc->pci);
    mmio_write32(hc->runtime, IMAN, 3U);
    op_write(hc, USBCMD, CMD_RUN | CMD_INTE);
    if (!wait_status(hc, USBSTS, STS_HALTED, 0, 100000000ULL)) goto failed;
    if (hcc1 & 8) { /* port power control */
        for (unsigned port = 1; port <= hc->ports; port++) {
            uint32_t status = op_read(hc, PORTSC(port));
            if (!(status & PORT_PP)) op_write(hc, PORTSC(port), (status & PORT_PRESERVE) | PORT_PP);
        }
        uint64_t deadline = native_clock_ns() + 20000000ULL; while (native_clock_ns() < deadline) boot_wait();
    }
    for (unsigned port = 1; port <= hc->ports; port++) hc->port_pending |= 1ULL << (port - 1);
    return true;
failed:
    op_write(hc, USBCMD, 0);
    pci_restore_command(hc->pci, command_word & ~4U);
    if (hc->pool) arena_free(hc->pool, POOL_PAGES * PAGE);
    if (hc->scratchpad_array) arena_free(hc->scratchpad_array, PAGE + (uint64_t)hc->scratchpads * PAGE);
    hc->pool = hc->scratchpad_array = 0;
    vga_print("[64] USB xHCI controller initialization failed\n");
    return false;
}
void xhci_init_all(void) {
    pci_device_t* devices = pci_get_devices(); int count = pci_device_count();
    for (int i = 0; i < count && controller_count < MAX_CONTROLLERS; i++) {
        pci_device_t* pci = &devices[i];
        if (pci->class_code != PCI_CLASS_SERIAL || pci->subclass != PCI_SUBCLASS_USB || pci->prog_if != PCI_PROGIF_XHCI) continue;
        struct xhci* hc = (struct xhci*)arena_alloc(sizeof(struct xhci));
        if (!hc) break;
        hc->pci = pci; hc->index = controller_count;
        memcpy(hc->name, "xhci0", 6); hc->name[4] = (char)('0' + controller_count);
        if (!start(hc)) { arena_free((uint64_t)hc, sizeof(struct xhci)); continue; }
        controllers[controller_count++] = hc;
        unsigned usb2 = 0, usb3 = 0;
        for (unsigned port = 1; port <= hc->ports; port++) { if (hc->protocol[port] == 3) usb3++; else usb2++; }
        vga_print("[64] USB "); vga_print(hc->name);
        usb_log_number(pci->vendor_id == 0x1033 ? " (NEC) ready ports usb2=" : " ready ports usb2=", usb2, false);
        usb_log_number("     usb3 ports=", usb3, false);
        usb_log_number("     slots=", hc->slots_enabled, false);
        usb_log_number("     context bytes=", hc->context_size, false);
        if (!hc->vector) vga_print("     no legacy IRQ line: polled\n");
        else usb_log_number("     irq vector=", hc->vector, false);
    }
}

/* ---- System sleep (power.h; usb.c registers it) ----
 * The schedule stops before the power goes. After the power loss the controller is reset and programmed again over
 * the same memory; every slot is gone, so usb.c forgets the devices (xhci_device_forget: no command) and the worker
 * enumerates every root port afresh. */
void xhci_halt(struct xhci* hc) {
    if (!hc->pool) return;
    op_write(hc, USBCMD, op_read(hc, USBCMD) & ~(CMD_RUN | CMD_INTE));
    wait_status(hc, USBSTS, STS_HALTED, STS_HALTED, 100000000ULL);
}
void xhci_device_forget(struct usb_device* device) {
    struct xhci* hc = device->hc;
    if (device->slot && device->slot <= MAX_SLOTS) { ((volatile uint64_t*)hc->dcbaa)[device->slot] = 0; hc->slots[device->slot] = NULL; }
    for (unsigned i = 0; i < device->endpoint_count; i++) { xhci_page_free(hc, device->endpoints[i].page); device->endpoints[i].page = 0; }
    while (device->pipe_count) usb_pipe_close(device->pipes[device->pipe_count - 1]);   /* a class driver left them */
    xhci_page_free(hc, device->context); xhci_page_free(hc, device->ep0_ring);
    device->context = device->ep0_ring = 0; device->slot = 0;
}
bool xhci_restart(struct xhci* hc) {
    if (!hc->pool) return false;
    uint32_t hcc1 = mmio_read32(hc->base, 0x10);
    extended_capabilities(hc, hcc1);   /* the firmware may own the controller again */
    if (!wait_status(hc, USBSTS, STS_NOT_READY, 0, 1000000000ULL)) return false;
    op_write(hc, USBCMD, op_read(hc, USBCMD) & ~(CMD_RUN | CMD_INTE));
    if (!wait_status(hc, USBSTS, STS_HALTED, STS_HALTED, 100000000ULL)) return false;
    op_write(hc, USBCMD, CMD_RESET);
    for (volatile unsigned i = 0; i < 2000; i++) cpu_pause();
    if (!wait_status(hc, USBCMD, CMD_RESET, 0, 1000000000ULL) || !wait_status(hc, USBSTS, STS_NOT_READY, 0, 1000000000ULL)) return false;
    uint64_t scratchpads = ((volatile uint64_t*)hc->dcbaa)[0];
    memset((void*)hc->dcbaa, 0, PAGE); ((volatile uint64_t*)hc->dcbaa)[0] = scratchpads;
    for (unsigned i = 0; i <= MAX_SLOTS; i++) hc->slots[i] = NULL;
    op_write(hc, CONFIG, (op_read(hc, CONFIG) & ~0xFFU) | hc->slots_enabled);
    mmio_write64(hc->op, DCBAAP, dma(hc->dcbaa));
    memset((void*)hc->command_ring, 0, PAGE); ring_init(hc->command_ring, COMMAND_TRBS);
    hc->command_enqueue = 0; hc->command_cycle = true; hc->command_done = false;
    mmio_write64(hc->op, CRCR, dma(hc->command_ring) | 1U);
    memset((void*)hc->event_ring, 0, PAGE);
    ((volatile uint64_t*)hc->erst)[0] = dma(hc->event_ring); ((volatile uint32_t*)hc->erst)[2] = EVENT_TRBS;
    hc->event_dequeue = 0; hc->event_cycle = true; hc->processing = false;
    mmio_write32(hc->runtime, ERSTSZ, 1);
    mmio_write64(hc->runtime, ERDP, dma(hc->event_ring));
    mmio_write64(hc->runtime, ERSTBA, dma(hc->erst));
    mmio_write32(hc->runtime, IMOD, 4000);
    mmio_write32(hc->runtime, IMAN, 3U);
    op_write(hc, USBCMD, CMD_RUN | CMD_INTE);
    if (!wait_status(hc, USBSTS, STS_HALTED, 0, 100000000ULL)) return false;
    if (hcc1 & 8) {
        for (unsigned port = 1; port <= hc->ports; port++) {
            uint32_t status = op_read(hc, PORTSC(port));
            if (!(status & PORT_PP)) op_write(hc, PORTSC(port), (status & PORT_PRESERVE) | PORT_PP);
        }
        uint64_t deadline = native_clock_ns() + 20000000ULL; while (native_clock_ns() < deadline) boot_wait();
    }
    for (unsigned port = 1; port <= hc->ports; port++) hc->port_pending |= 1ULL << (port - 1);
    hc->dead = false;
    return true;
}
