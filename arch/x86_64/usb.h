/* Native USB stack: xHCI host controllers (xhci.c), enumeration and the worker context (usb.c), the hub
 * class (usbhub.c) and the HID class (usbhid.c).
 *
 * Everything runs under the kernel lock on the bootstrap CPU, from usb_irq() (PIC IRQ) and usb_poll()
 * (scheduler loop, also the polled fallback without an IRQ line). Event-ring processing completes
 * commands and transfers and feeds HID reports straight into /dev/input. Enumeration, hub port
 * handling and endpoint recovery run sequentially in one cooperative worker with its own stack: it
 * yields back to usb_poll() whenever it waits (port reset, debounce, a command or control transfer), so
 * hot-plug never stalls the scheduler, and application processors never take part. */
#ifndef NEXUS_USB_H
#define NEXUS_USB_H
#include "pcidev.h"

/* xHCI default protocol speed IDs. */
enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4, USB_SPEED_SUPER_PLUS = 5 };
#define USB_MAX_DEVICES 32
#define USB_MAX_INTERFACES 8
#define USB_RING_TRBS 64

struct xhci;
struct usb_device;
struct usb_endpoint;
typedef void (*usb_complete)(struct usb_endpoint* endpoint, const uint8_t* data, unsigned length);

/* One periodic IN endpoint (HID report or hub status-change pipe). Its page holds a USB_RING_TRBS ring
 * and the report buffers the queued TRBs point at. */
struct usb_endpoint {
    struct usb_device* device;
    usb_complete complete;
    void* owner;
    uint64_t page;
    uint16_t max_packet;
    uint8_t address, dci, interval, ss_burst;
    unsigned enqueue, failures;
    bool cycle, configured, halted;
};
/* Bulk endpoints (xhci.c) for the mass-storage class (usbstorage.c). A TD is queued from any context that
 * holds the kernel lock (xhci_bulk_submit) and completes through event processing (done/code/actual);
 * xhci_bulk_reset (Reset Endpoint, Stop Endpoint, Set TR Dequeue Pointer) issues commands: worker only.
 * SuperSpeed UAS pipes use a Primary Stream Array of USB_BULK_STREAMS entries (stream IDs 1..3). */
#define USB_MAX_BULK 4
#define USB_BULK_STREAMS 4
#define USB_BULK_MAX_TRBS 16
struct usb_bulk_transfer {
    volatile bool done;
    uint8_t code;                       /* xHCI completion code: 1 success, 13 short packet, 6 stall, 26/27 stopped */
    uint32_t length, actual;
    unsigned first, count;              /* ring positions of its TRBs */
    uint32_t lengths[USB_BULK_MAX_TRBS];
};
struct usb_bulk_ring { uint64_t page; unsigned enqueue; bool cycle; struct usb_bulk_transfer* active; };
struct usb_bulk {
    struct usb_device* device;
    uint16_t max_packet;
    uint8_t address, dci, burst, streams; /* streams: 0, or USB_BULK_STREAMS */
    bool in, configured, halted;
    uint64_t stream_array;               /* page holding the stream context array */
    struct usb_bulk_ring rings[USB_BULK_STREAMS]; /* [0] without streams, [1..3] stream rings */
};
/* Pipes (xhci.c) for the class drivers in usbclass.c (audio, video, game controllers): isochronous or interrupt
 * endpoints with one transfer ring each; TDs on buffers the class driver owns, completions through `complete` with the
 * queue-time tag, bytes moved and the xHCI completion code (1 success, 13 short packet, 23 missed service, 26 stopped /
 * cancelled). open/close: any context under the kernel lock; configure/stop: USB worker only. */
#define USB_PIPE_TRBS 256U
#define USB_MAX_PIPES 8
enum { USB_PIPE_ISOCH = 1, USB_PIPE_BULK = 2, USB_PIPE_INTERRUPT = 3 };
struct usb_pipe {
    struct usb_device* device;
    void (*complete)(struct usb_pipe* pipe, uint32_t tag, unsigned actual, unsigned code);
    void* owner;
    uint64_t ring;
    uint16_t max_packet;                 /* wMaxPacketSize bits 10:0 */
    uint8_t address, dci, type, interval, burst, mult;   /* interval: bInterval; burst: HS extra transactions / SS bMaxBurst */
    bool in, configured, halted, cycle;
    unsigned enqueue, dequeue, queued;
    uint32_t tags[USB_PIPE_TRBS], lengths[USB_PIPE_TRBS];
};
bool usb_pipe_open(struct usb_device* device, struct usb_pipe* pipe);   /* address/type/... set by the caller */
void usb_pipe_close(struct usb_pipe* pipe);
bool usb_pipes_configure(struct usb_device* device, struct usb_pipe* const* add, unsigned adds, struct usb_pipe* const* drop, unsigned drops);
int usb_pipe_queue(struct usb_pipe* pipe, uint64_t physical, uint32_t length, uint32_t tag);
bool usb_pipe_stop(struct usb_pipe* pipe);
uint32_t usb_frame_index(struct usb_device* device);   /* MFINDEX: 125 us microframes, 14 bits */
struct usb_interface_info {
    uint8_t number, alternate, class_code, subclass, protocol;
    uint16_t report_length;           /* HID: report descriptor length */
    uint8_t in_address, in_interval, ss_burst;
    uint16_t in_max_packet;
};
struct usb_device {
    struct xhci* hc;
    struct usb_device* parent;        /* hub, NULL on a root port */
    void* class_state[USB_MAX_INTERFACES];
    struct usb_endpoint endpoints[USB_MAX_INTERFACES];
    uint64_t context, ep0_ring;       /* output device context page; EP0 transfer ring page */
    uint32_t route;                   /* xHCI route string (hub ports, 4 bits per tier) */
    uint16_t vendor, product, mps0, max_exit_latency;
    uint8_t slot, port, root_port, depth, speed, tt_slot, tt_port;
    uint8_t device_class, configuration, interface_count, endpoint_count;
    unsigned ep0_enqueue;
    bool used, ep0_cycle, gone, tt_multi, is_hub, hub_multi_tt;
    /* control transfer completion, written by event processing */
    volatile bool control_done;
    uint8_t control_code;
    uint32_t control_residual;
    uint64_t control_status_trb, control_data_trb;
    struct usb_interface_info interfaces[USB_MAX_INTERFACES];
    struct usb_bulk bulk[USB_MAX_BULK];  /* mass storage (usbstorage.c); rings freed by xhci_device_close */
    uint8_t bulk_count;
    struct usb_pipe* pipes[USB_MAX_PIPES];   /* class-driver pipes (usbclass.c) */
    uint8_t pipe_count;
    uint8_t* config; uint16_t config_length; /* raw configuration descriptor (usb_config) */
    void* class_driver[4];                   /* usbclass.c: audio, video, game controller */
    char product_name[48];                   /* iProduct string (ASCII), "" if none */
};

/* Standard requests (usb.c). Worker context only. Returns bytes transferred or a negative error. */
int usb_control(struct usb_device* device, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length);
/* Worker context: yield until `nanoseconds` have passed. */
void usb_sleep(uint64_t nanoseconds);
/* Enumerate a device that a root or hub port just enabled; NULL if it failed. Worker context. */
struct usb_device* usb_enumerate(struct xhci* hc, struct usb_device* parent, unsigned port, unsigned speed);
/* Detach a device and everything behind it: class drivers, slot, rings. Worker context. */
void usb_detach(struct usb_device* device);
/* Wake the worker (port or hub status changes noted from event processing). */
void usb_worker_kick(void);
/* Configure the periodic IN endpoints of the bound interfaces and start them. Worker context. */
struct usb_device* usb_child(struct usb_device* hub, unsigned port);   /* device on a hub port, or NULL */
void usb_log_device(const struct usb_device* device, const char* what);
void usb_log_number(const char* label, uint64_t value, bool hexadecimal);

/* xHCI (xhci.c) */
void xhci_init_all(void);
bool xhci_poll_events(void);          /* any controller's event ring; true if events were consumed */
void xhci_irq(unsigned vector);
unsigned xhci_count(void);
struct xhci* xhci_get(unsigned index);
bool xhci_take_port_change(struct xhci* hc, unsigned* port); /* worker: next root port with pending changes */
bool xhci_port_service(struct xhci* hc, unsigned port, unsigned* speed, bool* connected, bool* changed);
bool xhci_port_reset(struct xhci* hc, unsigned port, unsigned* speed);
unsigned xhci_port_count(const struct xhci* hc);
const char* xhci_name(const struct xhci* hc);
const pci_device_t* xhci_pci(const struct xhci* hc);   /* diag.c */
bool xhci_device_open(struct usb_device* device);        /* Enable Slot + Address Device */
bool xhci_update_ep0(struct usb_device* device);          /* Evaluate Context with the real bMaxPacketSize0 */
bool xhci_configure(struct usb_device* device, bool hub, unsigned ports, unsigned think_time, bool multi_tt);
void xhci_device_close(struct usb_device* device);        /* Disable Slot */
int xhci_control_transfer(struct usb_device* device, const uint8_t setup[8], uint64_t buffer, uint16_t length, bool in);
bool xhci_endpoint_init(struct usb_endpoint* endpoint);     /* allocate its ring/buffer page */
bool xhci_endpoint_start(struct usb_endpoint* endpoint);    /* queue every report buffer */
bool xhci_queue_in(struct usb_endpoint* endpoint, unsigned index); /* queue the TRB for buffer `index` and ring */
bool xhci_recover_endpoint(struct usb_endpoint* endpoint);
uint64_t xhci_page_alloc(struct xhci* hc);
void xhci_page_free(struct xhci* hc, uint64_t page);
uint64_t xhci_scratch(struct xhci* hc);                   /* one DMA page for control-transfer data */
unsigned xhci_pool_used(const struct xhci* hc);
bool usb_endpoint_halted_pending(void);
void xhci_halt(struct xhci* hc);                          /* system sleep: stop the schedule */
void xhci_device_forget(struct usb_device* device);       /* after a power loss: release without commands */
bool xhci_restart(struct xhci* hc);                       /* after a power loss: reset and program again */

/* Hub class (usbhub.c) */
bool usbhub_attach(struct usb_device* device);
void usbhub_detach(struct usb_device* device);
bool usbhub_service(void);            /* worker: handle pending hub port changes; true if work was done */

/* Bulk transfers and streams (xhci.c) */
bool xhci_bulk_init(struct usb_bulk* bulk, bool streams);   /* rings (+ stream array) from the pool */
bool xhci_bulk_configure(struct usb_device* device);       /* worker: Configure Endpoint for every bulk[] not configured */
bool xhci_streams_supported(const struct xhci* hc);
int  xhci_bulk_submit(struct usb_bulk* bulk, unsigned stream, uint64_t physical, uint32_t length, struct usb_bulk_transfer* t);
bool xhci_bulk_reset(struct usb_bulk* bulk);               /* worker: endpoint stopped/reset, rings emptied, transfers cancelled */
uint64_t xhci_dma_limit(const struct xhci* hc);            /* highest DMA address + 1 (AC64) */
bool xhci_port_connected(struct xhci* hc, unsigned port);  /* root port CCS, no side effects */
unsigned xhci_port_count_public(const struct xhci* hc);
/* Worker helpers for class drivers that wait outside the worker (block I/O): */
bool usb_in_worker(void);
void usb_drive(void);                 /* outside the worker: process events and run the worker once */

/* Raw configuration descriptor of an enumerated device (usb.c). */
const uint8_t* usb_config(const struct usb_device* device, unsigned* length);
/* Class drivers beyond hub/HID/storage (usbclass.c): audio (uaudio.c), video (uvc.c), game controllers (xpad.c). */
bool usbclass_wanted(const struct usb_device* device);
void usbclass_attach(struct usb_device* device);
void usbclass_detach(struct usb_device* device);
bool usbclass_service(void);            /* worker: class control requests queued from other contexts */
void usbclass_poll(uint64_t now);       /* usb_poll */
const char* usbclass_name(const struct usb_device* device);

/* Mass storage class (usbstorage.c) */
bool usbstorage_match(const struct usb_device* device);
bool usbstorage_attach(struct usb_device* device);
void usbstorage_detach(struct usb_device* device);
bool usbstorage_service(void);        /* worker: endpoint recovery and resets requested by I/O paths */
void usbstorage_tick(uint64_t now);   /* usb_poll: medium polling of removable logical units */
unsigned usbstorage_pending(void);    /* devices still being probed (boot root wait) */

/* HID class (usbhid.c) */
bool usbhid_attach(struct usb_device* device, unsigned interface);
void usbhid_detach(struct usb_device* device, unsigned interface);
void usbhid_poll(uint64_t now);       /* software key repeat */
void usbhid_register_capabilities(void);
/* HID game controllers (gamepad.c), handed over by usbhid_attach */
bool gamepad_sony(uint16_t vendor, uint16_t product, unsigned* kind);
bool gamepad_descriptor(const uint8_t* data, unsigned length);
bool gamepad_hid_attach(struct usb_device* device, unsigned interface, const uint8_t* descriptor, unsigned length);
bool gamepad_hid_detach(struct usb_device* device, unsigned interface);
bool gamepad_service(void);
#endif
