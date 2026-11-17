/* USB core: device enumeration (device/configuration/interface/endpoint/HID descriptors), class driver
 * binding (hub, HID), detach of whole subtrees, and the worker context.
 *
 * The worker is a cooperative context with its own stack. usb_poll() resumes it when it was kicked
 * (port/hub change, halted endpoint) or while it waits; it yields from usb_sleep(), which every wait
 * (port reset, debounce, xHCI command, control transfer) goes through. Code in the worker therefore
 * reads like straight-line driver code but never blocks the scheduler. At boot usb_init() drives the
 * worker until the devices present at power-on are enumerated (bounded), before the kernel's memory
 * baseline is taken. */
#include "usb.h"
#include "power.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define WORKER_STACK (32 * 1024)
#define BOOT_ENUMERATION_NS 5000000000ULL
static struct usb_device* devices;
static bool active, in_poll;
static uint64_t worker_stack, worker_rsp, caller_rsp;
static volatile bool worker_kicked;
static bool worker_idle, worker_running;

/* Save callee-saved registers on the current stack, store its pointer, switch to the other one. */
__asm__(".text\n.globl usb_context_switch\nusb_context_switch:\n"
        "push %rbp\npush %rbx\npush %r12\npush %r13\npush %r14\npush %r15\n"
        "mov %rsp,(%rdi)\nmov %rsi,%rsp\n"
        "pop %r15\npop %r14\npop %r13\npop %r12\npop %rbx\npop %rbp\nret\n");
void usb_context_switch(uint64_t* save, uint64_t load);

/* ---- logging ---- */
void usb_log_number(const char* label, uint64_t value, bool hexadecimal) {
    char text[24]; unsigned length = 0;
    if (hexadecimal) {
        text[length++] = '0'; text[length++] = 'x';
        unsigned digits = 1; while (digits < 16 && (value >> (4 * digits))) digits++;
        while (digits--) text[length++] = "0123456789abcdef"[(value >> (4 * digits)) & 15];
    } else {
        char reverse[21]; unsigned count = 0;
        do { reverse[count++] = (char)('0' + value % 10); value /= 10; } while (value);
        while (count) text[length++] = reverse[--count];
    }
    text[length++] = '\n'; text[length] = 0;
    vga_print(label); vga_print(text);
}
static void append(char* text, unsigned* length, const char* part) { while (*part && *length < 190) text[(*length)++] = *part++; }
static void append_hex(char* text, unsigned* length, unsigned value, unsigned digits) {
    while (digits-- && *length < 190) text[(*length)++] = "0123456789abcdef"[(value >> (4 * digits)) & 15];
}
static void append_decimal(char* text, unsigned* length, unsigned value) {
    char reverse[12]; unsigned count = 0;
    do { reverse[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count && *length < 190) text[(*length)++] = reverse[--count];
}
void usb_log_device(const struct usb_device* device, const char* what) {
    static const char* speeds[] = {"?", "full", "low", "high", "super", "super+"};
    char text[200]; unsigned length = 0;
    append(text, &length, "[64] USB "); append(text, &length, xhci_name(device->hc));
    append(text, &length, " port "); append_decimal(text, &length, device->root_port);
    if (device->route) { append(text, &length, " route 0x"); append_hex(text, &length, device->route, 5); }
    append(text, &length, " "); append(text, &length, device->speed <= 5 ? speeds[device->speed] : "?");
    append(text, &length, "-speed "); append_hex(text, &length, device->vendor, 4); append(text, &length, ":");
    append_hex(text, &length, device->product, 4); append(text, &length, " slot "); append_decimal(text, &length, device->slot);
    append(text, &length, ": "); append(text, &length, what); text[length++] = '\n'; text[length] = 0;
    vga_print(text);
}

/* ---- worker ---- */
void usb_worker_kick(void) { worker_kicked = true; }
void usb_sleep(uint64_t nanoseconds) {
    if (!worker_running) panic("usb_sleep outside the USB worker");
    uint64_t deadline = native_clock_ns() + nanoseconds;
    do usb_context_switch(&worker_rsp, caller_rsp); while (native_clock_ns() < deadline);
}
static struct usb_device* child(struct xhci* hc, struct usb_device* parent, unsigned port) {
    for (unsigned i = 0; i < USB_MAX_DEVICES; i++)
        if (devices[i].used && devices[i].hc == hc && devices[i].parent == parent && devices[i].port == port) return &devices[i];
    return NULL;
}
struct usb_device* usb_child(struct usb_device* hub, unsigned port) { return child(hub->hc, hub, port); }
static void root_port(struct xhci* hc, unsigned port) {
    unsigned speed; bool connected, changed;
    xhci_port_service(hc, port, &speed, &connected, &changed);
    struct usb_device* device = child(hc, NULL, port);
    if (device && (changed || !connected)) { usb_detach(device); device = NULL; }
    if (!connected || device) return;
    usb_sleep(100000000ULL); /* connect debounce (USB 2.0 TATTDB) */
    xhci_port_service(hc, port, &speed, &connected, &changed);
    if (!connected) return;
    if (!xhci_port_reset(hc, port, &speed)) { usb_log_number("[64] USB root port could not be enabled: port ", port, false); return; }
    usb_sleep(10000000ULL); /* reset recovery (TRSTRCY) */
    usb_enumerate(hc, NULL, port, speed);
}
static bool recover_endpoints(void) {
    bool work = false;
    for (unsigned i = 0; i < USB_MAX_DEVICES; i++) {
        struct usb_device* device = &devices[i];
        if (!device->used || device->gone) continue;
        for (unsigned j = 0; j < device->endpoint_count; j++) {
            struct usb_endpoint* endpoint = &device->endpoints[j];
            if (!endpoint->halted || !endpoint->page) continue;
            work = true;
            if (endpoint->failures > 3) continue; /* stays halted until the device is unplugged */
            usb_log_device(device, "recovering halted interrupt endpoint");
            if (!xhci_recover_endpoint(endpoint)) usb_log_device(device, "interrupt endpoint recovery failed");
        }
    }
    return work;
}
static void worker_main(void) {
    for (;;) {
        bool work = false;
        if (usb_endpoint_halted_pending()) { recover_endpoints(); work = true; }
        for (unsigned i = 0; i < xhci_count(); i++) {
            struct xhci* hc = xhci_get(i); unsigned port;
            while (xhci_take_port_change(hc, &port)) { root_port(hc, port); work = true; }
        }
        if (usbhub_service()) work = true;
        if (usbstorage_service()) work = true;
        if (usbclass_service()) work = true;
        if (!work) { worker_idle = true; usb_context_switch(&worker_rsp, caller_rsp); }
    }
}
static void worker_resume(void) {
    if (worker_idle && !worker_kicked) return;
    worker_idle = false; worker_kicked = false; worker_running = true;
    usb_context_switch(&caller_rsp, worker_rsp);
    worker_running = false;
}
bool usb_in_worker(void) { return worker_running; }
/* A class driver waiting outside the worker (block I/O under the kernel lock, any CPU): what usb_poll()
 * does, so completions, hot-plug and requested endpoint recovery make progress while it holds the lock. */
void usb_drive(void) {
    if (!active || in_poll || worker_running) return;
    in_poll = true; xhci_poll_events(); worker_resume(); in_poll = false;
}

/* ---- requests ---- */
int usb_control(struct usb_device* device, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length) {
    if (length > 4096) return -22;
    bool in = (type & 0x80) != 0;
    uint64_t scratch = xhci_scratch(device->hc);
    uint8_t setup[8] = {type, request, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    if (!in && length && data) memcpy((void*)scratch, data, length);
    int result = xhci_control_transfer(device, setup, scratch, length, in);
    if (in && result > 0 && data) memcpy(data, (const void*)scratch, (unsigned)result);
    return result;
}
static bool get_descriptor(struct usb_device* device, uint16_t value, void* data, uint16_t length, int* result) {
    *result = usb_control(device, 0x80, 6, value, 0, data, length);
    return *result >= 2;
}
/* Interfaces at alternate setting 0, their class/protocol, first interrupt IN endpoint and HID report length. */
static void parse_configuration(struct usb_device* device, const uint8_t* data, unsigned length) {
    struct usb_interface_info* current = NULL; bool last_was_input = false;
    device->interface_count = 0;
    for (unsigned at = 0; at + 2 <= length;) {
        unsigned size = data[at], type = data[at + 1];
        if (size < 2 || at + size > length) break;
        const uint8_t* d = data + at;
        if (type == 4 && size >= 9) {
            current = NULL;
            if (!d[3] && device->interface_count < USB_MAX_INTERFACES) {
                current = &device->interfaces[device->interface_count++];
                *current = (struct usb_interface_info){.number = d[2], .class_code = d[5], .subclass = d[6], .protocol = d[7]};
            }
            last_was_input = false;
        } else if (type == 0x21 && size >= 9 && current) {
            for (unsigned k = 0; k < d[5] && 6 + 3 * k + 2 < size; k++)
                if (d[6 + 3 * k] == 0x22) { current->report_length = (uint16_t)(d[7 + 3 * k] | d[8 + 3 * k] << 8); break; }
        } else if (type == 5 && size >= 7 && current) {
            last_was_input = false;
            if ((d[3] & 3) == 3 && (d[2] & 0x80) && !current->in_address) {
                current->in_address = d[2]; current->in_max_packet = (uint16_t)(d[4] | d[5] << 8); current->in_interval = d[6];
                last_was_input = true;
            }
        } else if (type == 0x30 && size >= 2 && current && last_was_input) current->ss_burst = size >= 3 ? d[2] : 0;
        at += size;
    }
}
static bool endpoint_prepare(struct usb_device* device, unsigned index) {
    struct usb_interface_info* interface = &device->interfaces[index];
    struct usb_endpoint* endpoint = &device->endpoints[index];
    if (!interface->in_address) return false;
    *endpoint = (struct usb_endpoint){.device = device, .address = interface->in_address, .interval = interface->in_interval,
                                      .dci = (uint8_t)((interface->in_address & 15) * 2 + 1), .max_packet = interface->in_max_packet & 0x7FF};
    endpoint->ss_burst = device->speed >= USB_SPEED_SUPER ? interface->ss_burst : device->speed == USB_SPEED_HIGH ? (interface->in_max_packet >> 11) & 3 : 0;
    if (endpoint->ss_burst > 15) endpoint->ss_burst = 15;
    if (!xhci_endpoint_init(endpoint)) return false;
    if (device->endpoint_count <= index) device->endpoint_count = (uint8_t)(index + 1);
    return true;
}
static const char* class_name(const struct usb_device* device) {
    if (device->is_hub) return device->speed >= USB_SPEED_SUPER ? "USB3 hub" : "USB2 hub";
    for (unsigned i = 0; i < device->interface_count; i++) if (device->interfaces[i].class_code == 3) {
        unsigned protocol = device->interfaces[i].protocol;
        return device->interfaces[i].subclass == 1 && protocol == 1 ? "HID keyboard" : device->interfaces[i].subclass == 1 && protocol == 2 ? "HID mouse" : "HID device";
    }
    return "no driver";
}
static uint8_t* config_pool;                                              /* USB_MAX_DEVICES x 4 KiB */
const uint8_t* usb_config(const struct usb_device* device, unsigned* length) { *length = device->config ? device->config_length : 0; return device->config; }
static void read_product(struct usb_device* device, unsigned index) {
    uint8_t text[96]; device->product_name[0] = 0;
    if (!index) return;
    int n = usb_control(device, 0x80, 6, (uint16_t)(0x0300 | index), 0x0409, text, sizeof(text));
    if (n < 4 || text[1] != 3) return;
    unsigned length = 0;
    for (int i = 2; i + 1 < n && i < text[0] && length + 1 < sizeof(device->product_name); i += 2)
        device->product_name[length++] = text[i] >= 32 && text[i] < 127 && !text[i + 1] ? (char)text[i] : '?';
    device->product_name[length] = 0;
}
static void bind(struct usb_device* device) {
    device->is_hub = device->device_class == 9 || (device->interface_count && device->interfaces[0].class_code == 9);
    bool any = false, classes = !device->is_hub && usbclass_wanted(device);
    for (unsigned i = 0; i < device->interface_count; i++) {
        bool wanted = device->is_hub ? i == 0 : device->interfaces[i].class_code == 3;
        if (wanted && endpoint_prepare(device, i)) any = true;
    }
    if (!any && !classes && !device->is_hub && usbstorage_match(device)) { if (!usbstorage_attach(device)) usb_log_device(device, "mass storage setup failed"); return; }
    if (!any && !classes) { device->is_hub = false; usb_log_device(device, "no supported interface (left addressed)"); return; }
    if (any && !xhci_configure(device, false, 0, 0, false)) { usb_log_device(device, "Configure Endpoint failed"); return; }
    int result = usb_control(device, 0x00, 9, device->configuration, 0, NULL, 0);
    if (result < 0) { usb_log_device(device, "SET_CONFIGURATION failed"); return; }
    usb_log_device(device, classes && !any ? usbclass_name(device) : class_name(device));
    if (device->is_hub) { if (!usbhub_attach(device)) usb_log_device(device, "hub setup failed"); return; }
    for (unsigned i = 0; i < device->interface_count; i++)
        if (device->interfaces[i].class_code == 3 && device->endpoints[i].page && !usbhid_attach(device, i))
            usb_log_device(device, "HID interface not usable");
    if (classes) usbclass_attach(device);
}
struct usb_device* usb_enumerate(struct xhci* hc, struct usb_device* parent, unsigned port, unsigned speed) {
    if (parent && parent->depth >= 5) { vga_print("[64] USB hub tree too deep (5 tiers)\n"); return NULL; }
    if (!speed || speed > USB_SPEED_SUPER_PLUS) { vga_print("[64] USB port reports an unknown speed\n"); return NULL; }
    struct usb_device* device = NULL;
    for (unsigned i = 0; i < USB_MAX_DEVICES && !device; i++) if (!devices[i].used) device = &devices[i];
    if (!device) { vga_print("[64] USB device table full\n"); return NULL; }
    memset(device, 0, sizeof(*device));
    device->used = true; device->hc = hc; device->parent = parent; device->port = (uint8_t)port; device->speed = (uint8_t)speed;
    device->root_port = parent ? parent->root_port : (uint8_t)port;
    device->depth = parent ? (uint8_t)(parent->depth + 1) : 0;
    device->route = parent ? parent->route | ((uint32_t)(port > 15 ? 15 : port) << (4 * parent->depth)) : 0;
    if (parent && (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)) {
        if (parent->speed == USB_SPEED_HIGH) { device->tt_slot = parent->slot; device->tt_port = (uint8_t)port; device->tt_multi = parent->hub_multi_tt; }
        else { device->tt_slot = parent->tt_slot; device->tt_port = parent->tt_port; device->tt_multi = parent->tt_multi; }
    }
    uint8_t descriptor[18]; int result = 0;
    if (!xhci_device_open(device)) { vga_print("[64] USB Address Device failed\n"); goto failed; }
    usb_sleep(2000000ULL); /* SET_ADDRESS recovery */
    if (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW) {
        if (!get_descriptor(device, 0x0100, descriptor, 8, &result) || result < 8) goto descriptor_failed;
        unsigned mps0 = descriptor[7];
        if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64) mps0 = 8;
        if (mps0 != device->mps0) { device->mps0 = (uint16_t)mps0; if (!xhci_update_ep0(device)) goto failed; }
    }
    if (!get_descriptor(device, 0x0100, descriptor, 18, &result) || result < 18) goto descriptor_failed;
    if (speed >= USB_SPEED_SUPER && descriptor[7] == 9 && device->mps0 != 512) { device->mps0 = 512; if (!xhci_update_ep0(device)) goto failed; }
    device->vendor = (uint16_t)(descriptor[8] | descriptor[9] << 8); device->product = (uint16_t)(descriptor[10] | descriptor[11] << 8);
    device->device_class = descriptor[4];
    read_product(device, descriptor[15]);
    uint8_t header[9];
    if (!get_descriptor(device, 0x0200, header, 9, &result) || result < 9 || header[1] != 2) goto descriptor_failed;
    unsigned total = (unsigned)(header[2] | header[3] << 8); if (total > 4096) total = 4096; if (total < 9) goto descriptor_failed;
    device->configuration = header[5];
    if (!get_descriptor(device, 0x0200, NULL, (uint16_t)total, &result) || result < 9) goto descriptor_failed;
    parse_configuration(device, (const uint8_t*)xhci_scratch(hc), (unsigned)result);
    if (config_pool && result <= 4096) {                                  /* the device's slot in the pool (usb_init) */
        device->config = config_pool + (uint64_t)(device - devices) * 4096;
        memcpy(device->config, (const void*)xhci_scratch(hc), (unsigned)result); device->config_length = (uint16_t)result;
    }
    bind(device);
    return device;
descriptor_failed:
    usb_log_number("[64] USB descriptor read failed, error ", (uint64_t)-(int64_t)result, false);
failed:
    xhci_device_close(device);
    memset(device, 0, sizeof(*device));
    return NULL;
}
void usb_detach(struct usb_device* device) {
    device->gone = true;
    for (unsigned i = 0; i < USB_MAX_DEVICES; i++) if (devices[i].used && devices[i].parent == device) usb_detach(&devices[i]);
    if (device->is_hub) usbhub_detach(device);
    else if (device->bulk_count) usbstorage_detach(device);
    else for (unsigned i = 0; i < USB_MAX_INTERFACES; i++) if (device->class_state[i]) usbhid_detach(device, i);
    if (!device->is_hub) usbclass_detach(device);
    usb_log_device(device, "detached");
    xhci_device_close(device);
    usb_log_number("[64] USB pool pages in use=", xhci_pool_used(device->hc), false);
    memset(device, 0, sizeof(*device));
}

/* ---- entry points ---- */
void usb_init(void) {
    xhci_init_all();
    if (!xhci_count()) return;
    devices = (struct usb_device*)arena_alloc(sizeof(struct usb_device) * USB_MAX_DEVICES);
    config_pool = (uint8_t*)arena_alloc(4096ULL * USB_MAX_DEVICES);       /* raw configuration copies (usb_config) */
    worker_stack = arena_alloc(WORKER_STACK);
    if (!devices || !worker_stack) { vga_print("[64] USB: no memory for the device table\n"); return; }
    uint64_t* stack = (uint64_t*)(worker_stack + WORKER_STACK);
    *--stack = 0;                         /* worker_main never returns */
    *--stack = (uint64_t)worker_main;     /* first switch "returns" here */
    for (unsigned i = 0; i < 6; i++) *--stack = 0;
    worker_rsp = (uint64_t)stack; worker_kicked = true;
    usbhid_register_capabilities();
    active = true;
    extern const struct pm_ops usb_pm; pm_register(&usb_pm);   /* system sleep (end of this file) */
    uint64_t deadline = native_clock_ns() + BOOT_ENUMERATION_NS;
    in_poll = true;
    while (native_clock_ns() < deadline) {
        xhci_poll_events();
        worker_resume();
        if (worker_idle && !worker_kicked) break;
        boot_wait();
    }
    in_poll = false;
    unsigned count = 0;
    for (unsigned i = 0; i < USB_MAX_DEVICES; i++) count += devices[i].used;
    usb_log_number(worker_idle ? "[64] USB boot enumeration complete, devices=" : "[64] USB boot enumeration continues in background, devices=", count, false);
}
void usb_poll(void) {
    if (!active || in_poll) return;
    in_poll = true;
    xhci_poll_events();
    worker_resume();
    usbhid_poll(native_clock_ns());
    usbstorage_tick(native_clock_ns());
    usbclass_poll(native_clock_ns());
    in_poll = false;
}
/* tick.c: when usb_poll needs the boot CPU next while the system idles: a running or kicked worker (enumeration,
 * hot-plug, endpoint recovery: its waits are polled) and controllers without an interrupt at the 1 ms poll, else the
 * next key repeat or removable-media check. Events of interrupt-driven controllers wake the CPU themselves. */
uint64_t usbhid_next_event(void); uint64_t usbstorage_next_event(void); bool xhci_polled(void);
uint64_t usb_next_event(uint64_t now) {
    if (!active) return ~0ULL;
    if (!worker_idle || worker_kicked || xhci_polled()) return now + 1000000ULL;
    uint64_t due = usbhid_next_event(), media = usbstorage_next_event();
    return media < due ? media : due;
}
void usb_irq(unsigned vector) {
    if (!active) return;
    xhci_irq(vector);
}

/* ---- System sleep (power.h) ----
 * Suspend lets a running enumeration finish (the worker must not sit between commands when the controller stops)
 * and halts the controllers. After a power loss every device is forgotten without commands (the controller lost its
 * slots; pressed keys and buttons are released), the controllers are reset and programmed again and the worker
 * enumerates every port afresh: HID devices merge into the same /dev/input nodes as before. Suspend-to-idle leaves
 * USB running. */
static int usb_pm_suspend(void* context, unsigned state) {
    (void)context;
    if (state == PM_STATE_FREEZE) return 0;
    uint64_t deadline = native_clock_ns() + 3000000000ULL;
    while ((!worker_idle || worker_kicked) && native_clock_ns() < deadline) { usb_poll(); __asm__ volatile("pause"); }
    if (!worker_idle) { vga_print("[64] USB: enumeration still running; sleep refused\n"); return -16; }
    for (unsigned i = 0; i < xhci_count(); i++) xhci_halt(xhci_get(i));
    return 0;
}
static void usb_pm_resume(void* context, unsigned state) {
    (void)context;
    if (state == PM_STATE_FREEZE || !devices) return;
    for (unsigned i = 0; i < USB_MAX_DEVICES; i++) if (devices[i].used) {
        struct usb_device* device = &devices[i];
        if (device->is_hub) usbhub_detach(device);
        else for (unsigned j = 0; j < USB_MAX_INTERFACES; j++) if (device->class_state[j]) usbhid_detach(device, j);
        if (!device->is_hub) { device->gone = true; usbclass_detach(device); }
        xhci_device_forget(device);
        memset(device, 0, sizeof(*device));
    }
    for (unsigned i = 0; i < xhci_count(); i++) if (!xhci_restart(xhci_get(i))) vga_print("[64] USB: an xHCI controller did not come back after resume\n");
    worker_kicked = true;
}
const struct pm_ops usb_pm = {"usb", usb_pm_suspend, usb_pm_resume, NULL};
