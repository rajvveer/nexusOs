/* USB hub class: USB2 hubs (hub descriptor 0x29) and USB3 hubs (0x2A, SET_HUB_DEPTH, warm reset).
 * The hub's slot is reconfigured as a hub (port count, TT think time) with Configure Endpoint, every
 * port is powered, and the status-change interrupt pipe marks ports for the worker, which reads the
 * port status, acknowledges each change, detaches what left and debounces/resets/enumerates what
 * arrived. Devices behind the hub get their route string and transaction-translator data from
 * usb_enumerate(). */
#include "usb.h"
#include "../../kernel/vga.h"

#define MAX_HUBS 16   /* every USB3 hub is two hubs (SuperSpeed + USB2 companion); boards carry several */
#define MAX_HUB_PORTS 31
enum { PORT_CONNECTION = 0, PORT_ENABLE = 1, PORT_RESET = 4, PORT_POWER = 8, BH_PORT_RESET = 28,
       C_PORT_CONNECTION = 16, C_PORT_ENABLE = 17, C_PORT_SUSPEND = 18, C_PORT_OVER_CURRENT = 19, C_PORT_RESET = 20,
       C_PORT_LINK_STATE = 25, C_PORT_CONFIG_ERROR = 26, C_BH_PORT_RESET = 29 };
struct hub_state {
    struct usb_device* device;
    unsigned ports, power_delay_ms;
    bool usb3;
    volatile uint32_t pending; /* bit 0: hub status, bit n: port n */
};
static struct hub_state hubs[MAX_HUBS];

static int port_request(struct hub_state* hub, uint8_t request, unsigned feature, unsigned port) {
    return usb_control(hub->device, 0x23, request, (uint16_t)feature, (uint16_t)port, NULL, 0);
}
static bool port_status(struct hub_state* hub, unsigned port, uint16_t* status, uint16_t* change) {
    uint8_t data[4];
    if (usb_control(hub->device, 0xA3, 0, 0, (uint16_t)port, data, 4) < 4) return false;
    *status = (uint16_t)(data[0] | data[1] << 8); *change = (uint16_t)(data[2] | data[3] << 8);
    return true;
}
/* Acknowledge every reported change (CLEAR_FEATURE of the matching C_PORT_* selector). */
static void clear_changes(struct hub_state* hub, unsigned port, uint16_t change) {
    static const uint8_t usb2[] = {C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_SUSPEND, C_PORT_OVER_CURRENT, C_PORT_RESET};
    static const uint8_t usb3[] = {C_PORT_CONNECTION, 0, 0, C_PORT_OVER_CURRENT, C_PORT_RESET, C_BH_PORT_RESET, C_PORT_LINK_STATE, C_PORT_CONFIG_ERROR};
    const uint8_t* features = hub->usb3 ? usb3 : usb2; unsigned count = hub->usb3 ? sizeof(usb3) : sizeof(usb2);
    for (unsigned bit = 0; bit < count; bit++) if ((change & (1U << bit)) && features[bit]) port_request(hub, 1, features[bit], port);
}
static unsigned port_speed(struct hub_state* hub, uint16_t status) {
    if (hub->usb3) return USB_SPEED_SUPER;
    return (status & (1U << 9)) ? USB_SPEED_LOW : (status & (1U << 10)) ? USB_SPEED_HIGH : USB_SPEED_FULL;
}
static bool reset_port(struct hub_state* hub, unsigned port, bool warm, uint16_t* status) {
    uint16_t change;
    if (port_request(hub, 3, warm ? BH_PORT_RESET : PORT_RESET, port) < 0) return false;
    uint64_t deadline = native_clock_ns() + 800000000ULL;
    for (;;) {
        usb_sleep(10000000ULL);
        if (!port_status(hub, port, status, &change)) return false;
        if (!(*status & (1U << PORT_CONNECTION))) return false;
        if ((change & ((1U << 4) | (hub->usb3 ? 1U << 5 : 0))) && !(*status & (1U << PORT_RESET))) break;
        if (native_clock_ns() >= deadline) { usb_log_device(hub->device, "hub port reset timed out"); return false; }
    }
    clear_changes(hub, port, change);
    return (*status & (1U << PORT_ENABLE)) != 0;
}
static void port_change(struct hub_state* hub, unsigned port) {
    uint16_t status, change;
    if (!port_status(hub, port, &status, &change)) return;
    clear_changes(hub, port, change);
    bool connected = (status & (1U << PORT_CONNECTION)) != 0;
    struct usb_device* device = usb_child(hub->device, port);
    if (device && ((change & 1) || !connected)) { usb_detach(device); device = NULL; }
    unsigned power_bit = hub->usb3 ? 9 : 8;
    if (!(status & (1U << power_bit))) { port_request(hub, 3, PORT_POWER, port); return; } /* lost power (over-current): re-power, wait for connect */
    if (!connected || device) return;
    usb_sleep(100000000ULL); /* connect debounce */
    if (!port_status(hub, port, &status, &change) || !(status & (1U << PORT_CONNECTION))) return;
    clear_changes(hub, port, change);
    bool link_error = hub->usb3 && ((status >> 5) & 15) == 6; /* SS.Inactive needs a warm reset */
    if (!reset_port(hub, port, link_error, &status) && !(hub->usb3 && reset_port(hub, port, true, &status))) {
        usb_log_number("[64] USB hub port could not be enabled: port ", port, false); return;
    }
    usb_sleep(10000000ULL); /* reset recovery */
    usb_enumerate(hub->device->hc, hub->device, port, port_speed(hub, status));
}
static void hub_status(struct hub_state* hub) {
    uint8_t data[4];
    if (usb_control(hub->device, 0xA0, 0, 0, 0, data, 4) < 4) return;
    uint16_t change = (uint16_t)(data[2] | data[3] << 8);
    if (change & 1) usb_control(hub->device, 0x20, 1, 0, 0, NULL, 0); /* C_HUB_LOCAL_POWER */
    if (change & 2) usb_control(hub->device, 0x20, 1, 1, 0, NULL, 0); /* C_HUB_OVER_CURRENT */
}
static void status_change(struct usb_endpoint* endpoint, const uint8_t* data, unsigned length) {
    struct hub_state* hub = endpoint->owner; uint32_t bits = 0;
    for (unsigned i = 0; i < length && i < 4; i++) bits |= (uint32_t)data[i] << (8 * i);
    if (bits && hub->device) { hub->pending |= bits; usb_worker_kick(); }
}
bool usbhub_attach(struct usb_device* device) {
    struct hub_state* hub = NULL;
    for (unsigned i = 0; i < MAX_HUBS && !hub; i++) if (!hubs[i].device) hub = &hubs[i];
    if (!hub) { usb_log_device(device, "too many hubs"); return false; }
    bool usb3 = device->speed >= USB_SPEED_SUPER;
    uint8_t descriptor[16];
    int length = usb_control(device, 0xA0, 6, (uint16_t)((usb3 ? 0x2A : 0x29) << 8), 0, descriptor, usb3 ? 12 : 16);
    if (length < 7 || descriptor[1] != (usb3 ? 0x2A : 0x29)) { usb_log_device(device, "hub descriptor unreadable"); return false; }
    unsigned ports = descriptor[2], characteristics = (unsigned)(descriptor[3] | descriptor[4] << 8);
    if (!ports) return false;
    if (ports > MAX_HUB_PORTS) { usb_log_device(device, "hub has more than 31 ports; using the first 31"); ports = MAX_HUB_PORTS; }
    *hub = (struct hub_state){.device = device, .ports = ports, .usb3 = usb3, .power_delay_ms = descriptor[5] * 2U};
    device->hub_multi_tt = false; /* alternate setting 0: one TT for all ports */
    if (!xhci_configure(device, true, ports, usb3 ? 0 : (characteristics >> 5) & 3, false)) { hub->device = NULL; return false; }
    if (usb3 && usb_control(device, 0x20, 12, device->depth, 0, NULL, 0) < 0) { usb_log_device(device, "SET_HUB_DEPTH failed"); hub->device = NULL; return false; }
    device->class_state[0] = hub;
    for (unsigned port = 1; port <= ports; port++) port_request(hub, 3, PORT_POWER, port);
    usb_sleep((uint64_t)(hub->power_delay_ms + 20) * 1000000ULL);
    struct usb_endpoint* endpoint = &device->endpoints[0];
    endpoint->owner = hub; endpoint->complete = status_change;
    if (!xhci_endpoint_start(endpoint)) usb_log_device(device, "hub status pipe could not start; ports are scanned once");
    hub->pending = ((ports >= 31 ? 0xFFFFFFFFU : (2U << ports) - 1U)) & ~1U; /* initial scan of every port */
    usb_log_number("[64] USB hub ports=", ports, false);
    usb_worker_kick();
    return true;
}
void usbhub_detach(struct usb_device* device) {
    struct hub_state* hub = device->class_state[0];
    if (hub) { hub->device = NULL; hub->pending = 0; }
    device->class_state[0] = NULL;
}
bool usbhub_service(void) {
    bool work = false;
    for (unsigned i = 0; i < MAX_HUBS; i++) {
        struct hub_state* hub = &hubs[i];
        while (hub->device && hub->pending && !hub->device->gone) {
            uint32_t bits = hub->pending; hub->pending = 0; work = true;
            if (bits & 1) hub_status(hub);
            for (unsigned port = 1; port <= hub->ports && hub->device; port++) if (bits & (1U << port)) port_change(hub, port);
        }
    }
    return work;
}
