/* ============================================================================
 * NexusOS — USB Hub Class Driver — Phase 55
 * ============================================================================
 * Walks external hubs: powers the ports, resets each connected port, reads
 * the port speed and enumerates the child through the hub's own host
 * controller (usb_enumerate_device records the hub as the parent, which gives
 * UHCI the low-speed flag and xHCI the route string / TT fields). The hub's
 * interrupt-IN status-change endpoint is polled from usb_poll() so devices
 * plugged in or removed behind a hub after boot are picked up. Nested hubs
 * are followed up to USB_MAX_HUB_DEPTH tiers.
 * ============================================================================ */

#include "usb.h"
#include "string.h"

#define HUB_MAX 6

/* Hub class requests */
#define HUB_REQ_PORT  (USB_TYPE_CLASS | USB_RECIP_OTHER)        /* 0x23 */
#define HUB_REQ_HUB   (USB_TYPE_CLASS | USB_RECIP_DEVICE)       /* 0x20 */

/* Port features */
#define HUB_F_PORT_CONNECTION   0
#define HUB_F_PORT_ENABLE       1
#define HUB_F_PORT_RESET        4
#define HUB_F_PORT_POWER        8
#define HUB_F_C_PORT_CONNECTION 16
#define HUB_F_C_PORT_ENABLE     17
#define HUB_F_C_PORT_SUSPEND    18
#define HUB_F_C_PORT_OVERCURR   19
#define HUB_F_C_PORT_RESET      20
#define HUB_F_C_HUB_LOCAL_POWER 0
#define HUB_F_C_HUB_OVERCURR    1

/* wPortStatus bits */
#define PS_CONNECTION   0x0001
#define PS_ENABLE       0x0002
#define PS_SUSPEND      0x0004
#define PS_OVERCURRENT  0x0008
#define PS_RESET        0x0010
#define PS_POWER        0x0100
#define PS_LOW_SPEED    0x0200
#define PS_HIGH_SPEED   0x0400
/* wPortChange bits */
#define PC_CONNECTION   0x0001
#define PC_ENABLE       0x0002
#define PC_RESET        0x0010

typedef struct {
    usb_device_t*   dev;
    usb_endpoint_t* ep;        /* status-change interrupt IN endpoint */
    uint32_t        gen;
    uint8_t         nports;
    bool            in_use;
} hub_t;

static hub_t hubs[HUB_MAX];

static int hub_get_port_status(usb_device_t* hub, int port, uint16_t* ws, uint16_t* wc) {
    uint8_t st[4];
    int n = usb_control(hub, USB_DIR_IN | HUB_REQ_PORT, USB_REQ_GET_STATUS,
                        0, (uint16_t)port, 4, st);
    if (n < 4) return -1;
    *ws = (uint16_t)(st[0] | (st[1] << 8));
    *wc = (uint16_t)(st[2] | (st[3] << 8));
    return 0;
}

static int hub_set_port_feature(usb_device_t* hub, int port, uint16_t feature) {
    return usb_control(hub, USB_DIR_OUT | HUB_REQ_PORT, USB_REQ_SET_FEATURE,
                       feature, (uint16_t)port, 0, 0);
}

static int hub_clear_port_feature(usb_device_t* hub, int port, uint16_t feature) {
    return usb_control(hub, USB_DIR_OUT | HUB_REQ_PORT, USB_REQ_CLEAR_FEATURE,
                       feature, (uint16_t)port, 0, 0);
}

static void hub_log(usb_device_t* hub, int port, const char* what) {
    char path[24];
    usb_port_path(hub, path, sizeof(path));
    usb_log("USB-HUB: "); usb_log(path); usb_log(" port "); usb_log_dec(port);
    usb_log(" "); usb_log(what); usb_log("\n");
}

/* Reset a hub port with a connected device and enumerate the child. */
static void hub_connect(hub_t* h, int port) {
    usb_device_t* hub = h->dev;
    uint16_t ws = 0, wc = 0;

    usb_delay_ms(100);                                  /* connection debounce */
    if (hub_get_port_status(hub, port, &ws, &wc) < 0) return;
    if (!(ws & PS_CONNECTION)) { hub_log(hub, port, "gone before reset"); return; }

    if (hub_set_port_feature(hub, port, HUB_F_PORT_RESET) < 0) {
        hub_log(hub, port, "SET_FEATURE(PORT_RESET) failed");
        return;
    }
    int ok = 0;
    for (int i = 0; i < 50; i++) {
        usb_delay_ms(10);
        if (hub_get_port_status(hub, port, &ws, &wc) < 0) return;
        if (wc & PC_RESET) { ok = 1; break; }
        if (!(ws & PS_RESET) && (ws & PS_ENABLE)) { ok = 1; break; }
    }
    hub_clear_port_feature(hub, port, HUB_F_C_PORT_RESET);
    if (wc & PC_CONNECTION) hub_clear_port_feature(hub, port, HUB_F_C_PORT_CONNECTION);
    if (wc & PC_ENABLE)     hub_clear_port_feature(hub, port, HUB_F_C_PORT_ENABLE);
    if (!ok || !(ws & PS_ENABLE)) { hub_log(hub, port, "reset did not enable the port"); return; }

    int speed;
    if (hub->speed == USB_SPEED_SUPER)   speed = USB_SPEED_SUPER;
    else if (ws & PS_LOW_SPEED)          speed = USB_SPEED_LOW;
    else if (ws & PS_HIGH_SPEED)         speed = USB_SPEED_HIGH;
    else                                 speed = USB_SPEED_FULL;

    usb_delay_ms(10);                                   /* reset recovery */
    hub_log(hub, port, "device connected, enumerating");
    usb_enumerate_device(hub->hcd, hub->root_port, speed, hub, port);
}

/* Look at one port: handle connect / disconnect and clear its change bits. */
static void hub_probe_port(hub_t* h, int port, bool initial) {
    usb_device_t* hub = h->dev;
    uint16_t ws = 0, wc = 0;
    if (hub_get_port_status(hub, port, &ws, &wc) < 0) return;

    if (wc & PC_CONNECTION) hub_clear_port_feature(hub, port, HUB_F_C_PORT_CONNECTION);
    if (wc & PC_ENABLE)     hub_clear_port_feature(hub, port, HUB_F_C_PORT_ENABLE);
    if (wc & PC_RESET)      hub_clear_port_feature(hub, port, HUB_F_C_PORT_RESET);
    if (wc & 0x0008)        hub_clear_port_feature(hub, port, HUB_F_C_PORT_OVERCURR);
    if (wc & 0x0004)        hub_clear_port_feature(hub, port, HUB_F_C_PORT_SUSPEND);

    usb_device_t* child = usb_device_on_hub_port(hub, port);

    if (ws & PS_CONNECTION) {
        if (child && !(wc & PC_CONNECTION)) return;     /* already enumerated, no change */
        if (child) { hub_log(hub, port, "re-plugged"); usb_device_remove(child); }
        if (!initial && !(wc & PC_CONNECTION)) return;  /* spurious */
        hub_connect(h, port);
    } else if (child) {
        hub_log(hub, port, "device removed");
        usb_device_remove(child);
    }
}

/* --------------------------------------------------------------------------
 * Called by the core once a hub device is configured.
 * -------------------------------------------------------------------------- */
void usb_hub_attach(usb_device_t* hub) {
    if (!hub || !hub->is_hub) return;
    if (hub->depth >= USB_MAX_HUB_DEPTH) {
        usb_log("USB-HUB: nesting too deep, not walking this hub\n");
        return;
    }
    hub_t* h = NULL;
    for (int i = 0; i < HUB_MAX; i++) if (!hubs[i].in_use) { h = &hubs[i]; break; }
    if (!h) { usb_log("USB-HUB: hub table full\n"); return; }

    uint8_t nports = hub->hub_nports;
    if (nports == 0) {
        /* The core could not read the hub descriptor; try once more here. */
        uint8_t buf[12];
        int got = usb_control(hub, USB_DIR_IN | HUB_REQ_HUB, USB_REQ_GET_DESCRIPTOR,
                              (USB_DESC_HUB << 8), 0, 9, buf);
        if (got >= 7) {
            hub->hub_nports = buf[2];
            hub->hub_char = (uint16_t)(buf[3] | (buf[4] << 8));
            hub->hub_pwr2good = buf[5];
            nports = hub->hub_nports;
        }
    }
    if (nports == 0 || nports > 15) { usb_log("USB-HUB: bad port count\n"); return; }

    memset(h, 0, sizeof(*h));
    h->in_use = true;
    h->dev = hub;
    h->gen = hub->gen;
    h->nports = nports;
    h->ep = usb_find_endpoint(hub, USB_EP_XFER_INT, 1);

    /* Power every port, wait bPwrOn2PwrGood * 2 ms (+ margin). */
    for (int p = 1; p <= nports; p++) hub_set_port_feature(hub, p, HUB_F_PORT_POWER);
    usb_delay_ms((uint32_t)hub->hub_pwr2good * 2 + 20);

    /* Clear stale hub-level change bits. */
    usb_control(hub, USB_DIR_OUT | HUB_REQ_HUB, USB_REQ_CLEAR_FEATURE, HUB_F_C_HUB_LOCAL_POWER, 0, 0, 0);
    usb_control(hub, USB_DIR_OUT | HUB_REQ_HUB, USB_REQ_CLEAR_FEATURE, HUB_F_C_HUB_OVERCURR, 0, 0, 0);

    for (int p = 1; p <= nports; p++) hub_probe_port(h, p, true);
}

void usb_hub_detach(usb_device_t* hub) {
    for (int i = 0; i < HUB_MAX; i++) {
        if (hubs[i].in_use && hubs[i].dev == hub) {
            hubs[i].in_use = false;
            hubs[i].dev = NULL;
            hubs[i].ep = NULL;
        }
    }
}

/* --------------------------------------------------------------------------
 * Hot-plug: poll each hub's status-change endpoint (called from usb_poll).
 * -------------------------------------------------------------------------- */
void usb_hub_poll(void) {
    for (int i = 0; i < HUB_MAX; i++) {
        hub_t* h = &hubs[i];
        if (!h->in_use) continue;
        usb_device_t* hub = h->dev;
        if (!hub || !hub->active || hub->gen != h->gen) { h->in_use = false; continue; }
        if (!h->ep) continue;

        uint8_t bits[4];
        memset(bits, 0, sizeof(bits));
        int n = usb_interrupt_in(hub, h->ep, bits, 4);
        if (n <= 0) continue;
        uint32_t map = bits[0] | ((uint32_t)bits[1] << 8);
        if (map & 1) {
            /* Hub-level change: just acknowledge it. */
            usb_control(hub, USB_DIR_OUT | HUB_REQ_HUB, USB_REQ_CLEAR_FEATURE, HUB_F_C_HUB_LOCAL_POWER, 0, 0, 0);
            usb_control(hub, USB_DIR_OUT | HUB_REQ_HUB, USB_REQ_CLEAR_FEATURE, HUB_F_C_HUB_OVERCURR, 0, 0, 0);
        }
        for (int p = 1; p <= h->nports; p++) {
            if (!(map & (1u << p))) continue;
            hub_log(hub, p, "status change");
            hub_probe_port(h, p, false);
            if (!h->in_use) break;                  /* hub itself vanished */
        }
    }
}
