/* ============================================================================
 * NexusOS — USB Core (Implementation) — Phase 25 / 54 / 55
 * ============================================================================
 * Controller-independent part of the USB stack:
 *   - the device table and the host-controller (HCD) table,
 *   - descriptor-driven enumeration (usb_enumerate_device), used both for
 *     root-port devices and, through usb_hub.c, for devices behind hubs,
 *   - the transfer API the class drivers use (dispatched via dev->hcd),
 *   - hot-plug polling of root ports + hubs (usb_poll, from the idle pumps),
 *   - PIT-calibrated delays and a COM1 debug log.
 * The host-controller drivers live in usb_uhci.c / usb_ohci.c / usb_ehci.c /
 * usb_xhci.c and register themselves through usb_hcd_alloc/usb_hcd_register.
 * ============================================================================ */

#include "usb.h"
#include "usb_hid.h"
#include "usb_storage.h"
#include "port.h"
#include "vga.h"
#include "string.h"

extern volatile uint32_t system_ticks;

/* USB devices (slots are reused after a device is unplugged) */
static usb_device_t usb_devices[USB_MAX_DEVICES];
static int num_usb_devices = 0;          /* high-water mark */

/* Host controllers */
static usb_hcd_t hcds[USB_MAX_HCDS];
static int num_hcds = 0;

/* Bumped on every device add / remove (class drivers rescan on change). */
static uint32_t usb_gen = 0;

/* --------------------------------------------------------------------------
 * Serial debug log (COM1). The harness greps these lines.
 * -------------------------------------------------------------------------- */
static void ser_putc(char c) {
    int guard = 100000;
    while ((port_byte_in(0x3FD) & 0x20) == 0 && --guard > 0) { }
    port_byte_out(0x3F8, (uint8_t)c);
}

void usb_log(const char* s) {
    while (*s) {
        if (*s == '\n') ser_putc('\r');
        ser_putc(*s++);
    }
}

void usb_log_hex(uint32_t v) {
    const char* hx = "0123456789abcdef";
    char b[11];
    int n = 0;
    b[n++] = '0'; b[n++] = 'x';
    int started = 0;
    for (int i = 28; i >= 0; i -= 4) {
        int d = (v >> i) & 0xF;
        if (d || started || i == 0) { b[n++] = hx[d]; started = 1; }
    }
    b[n] = '\0';
    usb_log(b);
}

void usb_log_dec(uint32_t v) {
    char b[12];
    int_to_str(v, b);
    usb_log(b);
}

/* --------------------------------------------------------------------------
 * Timing. The old spin(20000)-per-ms loop is host-speed dependent (it is
 * ~20 us on a modern CPU — far too short for real USB reset/debounce times),
 * so we time busy-waits against PIT channel 0, which runs at 1.193182 MHz and
 * is never reprogrammed by the kernel (default 18.2 Hz reload of 65536).
 * In mode 3 (square wave) the count decrements by 2 per clock; detect the mode
 * once with the read-back command so both encodings give real milliseconds.
 * -------------------------------------------------------------------------- */
static uint32_t pit_units_per_ms = 1193;

static uint16_t pit_read(void) {
    port_byte_out(0x43, 0x00);               /* latch channel 0 count */
    uint8_t lo = port_byte_in(0x40);
    uint8_t hi = port_byte_in(0x40);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static void pit_detect(void) {
    port_byte_out(0x43, 0xE2);               /* read-back: status of channel 0 */
    uint8_t st = port_byte_in(0x40);
    int mode = (st >> 1) & 7;
    pit_units_per_ms = (mode == 3 || mode == 7) ? 2386 : 1193;
}

void usb_delay_us(uint32_t us) {
    if (us > 4000000u) us = 4000000u;
    uint32_t target = (us / 1000u) * pit_units_per_ms +
                      ((us % 1000u) * pit_units_per_ms) / 1000u;
    if (target == 0) target = 1;
    uint16_t last = pit_read();
    uint32_t elapsed = 0;
    uint32_t guard = 0;
    while (elapsed < target) {
        uint16_t now = pit_read();
        elapsed += (uint16_t)(last - now);   /* counter counts down; wraps */
        last = now;
        /* Safety net: a stopped PIT would hang us forever. */
        if (++guard > 50000000u) break;
    }
}

void usb_delay_ms(uint32_t ms) {
    while (ms--) usb_delay_us(1000);
}

/* --------------------------------------------------------------------------
 * Names
 * -------------------------------------------------------------------------- */
const char* usb_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "Unspecified";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case USB_CLASS_HID: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case USB_CLASS_MASS_STORAGE: return "Storage";
        case USB_CLASS_HUB: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0E: return "Video";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFE: return "App-Specific";
        case 0xFF: return "Vendor";
        default:   return "Unknown";
    }
}

const char* usb_speed_name(uint8_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:   return "LS";
        case USB_SPEED_FULL:  return "FS";
        case USB_SPEED_HIGH:  return "HS";
        case USB_SPEED_SUPER: return "SS";
        default:              return "??";
    }
}

/* --------------------------------------------------------------------------
 * Device / controller table accessors
 * -------------------------------------------------------------------------- */
usb_device_t* usb_get_devices(void)   { return usb_devices; }
int           usb_device_count(void)  { return num_usb_devices; }
uint32_t      usb_generation(void)    { return usb_gen; }
int           usb_hcd_count(void)     { return num_hcds; }
usb_hcd_t*    usb_hcd_get(int i)      { return (i >= 0 && i < num_hcds) ? &hcds[i] : NULL; }

bool usb_controller_present(void) {
    for (int i = 0; i < num_hcds; i++) if (hcds[i].active) return true;
    return false;
}

int usb_device_index(usb_device_t* dev) {
    if (!dev) return -1;
    int i = (int)(dev - usb_devices);
    return (i >= 0 && i < USB_MAX_DEVICES) ? i : -1;
}

usb_device_t* usb_device_on_root_port(usb_hcd_t* hc, int port) {
    for (int i = 0; i < num_usb_devices; i++) {
        usb_device_t* d = &usb_devices[i];
        if (d->active && d->hcd == hc && d->parent < 0 && d->port == port) return d;
    }
    return NULL;
}

usb_device_t* usb_device_on_hub_port(usb_device_t* hub, int port) {
    int hi = usb_device_index(hub);
    if (hi < 0) return NULL;
    for (int i = 0; i < num_usb_devices; i++) {
        usb_device_t* d = &usb_devices[i];
        if (d->active && d->parent == hi && d->port == port) return d;
    }
    return NULL;
}

/* "bus-rootport.hubport.hubport" */
void usb_port_path(usb_device_t* dev, char* out, int outlen) {
    uint8_t chain[USB_MAX_HUB_DEPTH + 2];
    int n = 0;
    usb_device_t* d = dev;
    while (d && n < (int)sizeof(chain)) {
        chain[n++] = d->port;
        if (d->parent < 0 || d->parent >= USB_MAX_DEVICES) break;
        d = &usb_devices[d->parent];
    }
    int pos = 0;
    char num[8];
    #define PATH_PUT(c) do { if (pos < outlen - 1) out[pos++] = (c); } while (0)
    #define PATH_NUM(v) do { int_to_str((v), num); for (int k = 0; num[k]; k++) PATH_PUT(num[k]); } while (0)
    PATH_NUM(dev && dev->hcd ? dev->hcd->bus : 0);
    PATH_PUT('-');
    for (int i = n - 1; i >= 0; i--) {
        PATH_NUM(chain[i]);
        if (i) PATH_PUT('.');
    }
    #undef PATH_PUT
    #undef PATH_NUM
    if (outlen > 0) out[pos] = '\0';
}

/* --------------------------------------------------------------------------
 * Transfer API — dispatched through the device's host controller
 * -------------------------------------------------------------------------- */
int usb_control(usb_device_t* dev, uint8_t bmRequestType, uint8_t bRequest,
                uint16_t wValue, uint16_t wIndex, uint16_t wLength, void* data) {
    if (!dev || !dev->hcd || !dev->hcd->active || !dev->hcd->control) return -1;
    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = wValue & 0xFF;   setup[3] = wValue >> 8;
    setup[4] = wIndex & 0xFF;   setup[5] = wIndex >> 8;
    setup[6] = wLength & 0xFF;  setup[7] = wLength >> 8;
    return dev->hcd->control(dev->hcd, dev, setup, data, wLength,
                             (bmRequestType & USB_DIR_IN) != 0);
}

int usb_interrupt_in(usb_device_t* dev, usb_endpoint_t* ep, void* buf, int len) {
    if (!dev || !ep || !dev->active || !dev->hcd || !dev->hcd->active || !dev->hcd->interrupt_in) return -1;
    int n = dev->hcd->interrupt_in(dev->hcd, dev, ep, buf, len);
    if (n > 0) dev->reports++;
    return n;
}

int usb_bulk(usb_device_t* dev, usb_endpoint_t* ep, void* buf, int len, int dir_in) {
    if (!dev || !ep || !dev->active || !dev->hcd || !dev->hcd->active || !dev->hcd->bulk) return -1;
    return dev->hcd->bulk(dev->hcd, dev, ep, buf, len, dir_in);
}

usb_endpoint_t* usb_find_endpoint(usb_device_t* dev, uint8_t xfer_type, int dir_in) {
    if (!dev) return NULL;
    for (int i = 0; i < dev->num_endpoints; i++) {
        usb_endpoint_t* ep = &dev->endpoints[i];
        if ((ep->attributes & USB_EP_XFER_MASK) != xfer_type) continue;
        int ep_in = (ep->address & USB_EP_DIR_IN) != 0;
        if (ep_in == (dir_in != 0)) return ep;
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Enumeration
 * -------------------------------------------------------------------------- */
static usb_device_t* alloc_device_slot(void) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!usb_devices[i].active) {
            memset(&usb_devices[i], 0, sizeof(usb_device_t));
            if (i >= num_usb_devices) num_usb_devices = i + 1;
            return &usb_devices[i];
        }
    }
    return NULL;
}

/* Lowest USB address not used by an active device on this controller. */
static int alloc_address(usb_hcd_t* hc) {
    for (int a = 1; a < 128; a++) {
        bool used = false;
        for (int i = 0; i < num_usb_devices; i++) {
            usb_device_t* d = &usb_devices[i];
            if (d->active && d->hcd == hc && d->address == a) { used = true; break; }
        }
        if (!used) return a;
    }
    return -1;
}

static void log_device(const char* what, usb_device_t* dev) {
    char path[24];
    usb_port_path(dev, path, sizeof(path));
    usb_log("USB: "); usb_log(what); usb_log(" ");
    usb_log(dev->hcd ? dev->hcd->name : "?"); usb_log(" "); usb_log(path);
    usb_log(" "); usb_log_hex(dev->vendor_id); usb_log(":"); usb_log_hex(dev->product_id);
    usb_log(" "); usb_log(usb_speed_name(dev->speed));
    usb_log(" class="); usb_log(usb_class_name(dev->class_code));
    usb_log(" addr="); usb_log_dec(dev->address);
    if (dev->is_hub) { usb_log(" hub ports="); usb_log_dec(dev->hub_nports); }
    usb_log("\n");
}

usb_device_t* usb_enumerate_device(usb_hcd_t* hc, int port, int speed,
                                   usb_device_t* hub, int hubport) {
    if (!hc || !hc->active) return NULL;
    if (speed < USB_SPEED_LOW || speed > USB_SPEED_SUPER) return NULL;

    usb_device_t* dev = alloc_device_slot();
    if (!dev) { usb_log("USB: device table full\n"); return NULL; }

    dev->hcd = hc;
    dev->speed = (uint8_t)speed;
    dev->address = 0;                        /* default address during setup */
    if (hub) {
        dev->parent = (int8_t)usb_device_index(hub);
        dev->port = (uint8_t)hubport;
        dev->root_port = hub->root_port;
        dev->depth = hub->depth + 1;
        dev->route = hub->route | ((uint32_t)(hubport & 0xF) << (4 * hub->depth));
    } else {
        dev->parent = -1;
        dev->port = (uint8_t)port;
        dev->root_port = (uint8_t)port;
        dev->depth = 0;
        dev->route = 0;
    }
    dev->max_packet0 = (speed == USB_SPEED_HIGH) ? 64 : (speed == USB_SPEED_SUPER) ? 512 : 8;
    dev->active = true;                      /* reserve the slot while we work */
    dev->gen = ++usb_gen;

    {
        char path[24];
        usb_port_path(dev, path, sizeof(path));
        usb_log("USB: enumerating "); usb_log(hc->name); usb_log(" "); usb_log(path);
        usb_log(" "); usb_log(usb_speed_name(dev->speed)); usb_log("\n");
    }

    uint8_t buf[256];
    const char* step = "slot";

    /* 0. xHCI: Enable Slot + Address Device before we can talk to EP0. */
    if (hc->enable_device && hc->enable_device(hc, dev) < 0) goto fail;

    /* 1. First 8 bytes of the device descriptor (gives bMaxPacketSize0). */
    step = "get_desc8";
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8), 0, 8, buf) < 0)
        goto fail;
    {
        uint16_t mps0 = buf[7];
        if (speed == USB_SPEED_SUPER) mps0 = (buf[7] <= 9) ? (uint16_t)(1u << buf[7]) : 512;
        if (mps0 == 8 || mps0 == 16 || mps0 == 32 || mps0 == 64 || mps0 == 512)
            dev->max_packet0 = mps0;
    }

    /* 2. Address the device (xHCI did that already; it updates EP0's MPS). */
    if (hc->update_mps0) {
        step = "eval_ctx";
        if (hc->update_mps0(hc, dev) < 0) goto fail;
    } else {
        step = "set_address";
        int addr = alloc_address(hc);
        if (addr < 0) goto fail;
        if (usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_SET_ADDRESS, (uint16_t)addr, 0, 0, 0) < 0)
            goto fail;
        dev->address = (uint8_t)addr;
        usb_delay_ms(5);                     /* SET_ADDRESS recovery */
    }

    /* 3. Full device descriptor. */
    step = "get_desc18";
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8), 0, 18, buf) < 18)
        goto fail;
    {
        usb_device_descriptor_t* dd = (usb_device_descriptor_t*)buf;
        dev->vendor_id  = dd->idVendor;
        dev->product_id = dd->idProduct;
        dev->class_code = dd->bDeviceClass;
        dev->subclass   = dd->bDeviceSubClass;
        dev->protocol   = dd->bDeviceProtocol;
    }

    /* 4. Config descriptor: header first, then the whole thing. */
    step = "get_config";
    if (usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0, 9, buf) < 9)
        goto fail;
    int total;
    {
        usb_config_descriptor_t* cd = (usb_config_descriptor_t*)buf;
        dev->config_value = cd->bConfigurationValue;
        total = cd->wTotalLength;
        if (total > (int)sizeof(buf)) total = sizeof(buf);
        if (total < 9) total = 9;
    }
    {
        int got = usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIG << 8), 0,
                              (uint16_t)total, buf);
        if (got < 9) goto fail;
        if (got < total) total = got;
    }

    /* 5. Walk interface + endpoint descriptors. */
    {
        int off = 0;
        dev->num_endpoints = 0;
        while (off + 2 <= total) {
            uint8_t blen = buf[off];
            uint8_t btype = buf[off + 1];
            if (blen == 0) break;
            if (btype == USB_DESC_INTERFACE && off + 9 <= total) {
                usb_interface_descriptor_t* id = (usb_interface_descriptor_t*)&buf[off];
                /* Composite devices report class at interface level; take the
                 * first interface's class as the device's effective class. */
                if (dev->class_code == 0 || dev->class_code == 0xEF) {
                    dev->class_code = id->bInterfaceClass;
                    dev->subclass   = id->bInterfaceSubClass;
                    dev->protocol   = id->bInterfaceProtocol;
                } else if (id->bInterfaceClass == dev->class_code && id->bAlternateSetting == 0) {
                    dev->subclass = id->bInterfaceSubClass;
                    dev->protocol = id->bInterfaceProtocol;
                }
            } else if (btype == USB_DESC_ENDPOINT && off + 7 <= total &&
                       dev->num_endpoints < USB_MAX_ENDPOINTS) {
                usb_endpoint_descriptor_t* ed = (usb_endpoint_descriptor_t*)&buf[off];
                usb_endpoint_t* ep = &dev->endpoints[dev->num_endpoints++];
                ep->address = ed->bEndpointAddress;
                ep->attributes = ed->bmAttributes;
                ep->max_packet_size = ed->wMaxPacketSize & 0x7FF;
                ep->interval = ed->bInterval;
                ep->toggle = 0;
                ep->hc_priv = NULL;
            }
            off += blen;
        }
    }

    /* 6. Hubs: read the hub descriptor now — xHCI needs the port count in the
     *    slot context before Configure Endpoint. */
    if (dev->class_code == USB_CLASS_HUB) {
        dev->is_hub = 1;
        uint8_t dtype = (speed == USB_SPEED_SUPER) ? USB_DESC_SS_HUB : USB_DESC_HUB;
        int got = usb_control(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, (uint16_t)(dtype << 8), 0, 12, buf);
        if (got < 7 && dtype == USB_DESC_HUB)
            got = usb_control(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                              USB_REQ_GET_DESCRIPTOR, (USB_DESC_SS_HUB << 8), 0, 12, buf);
        if (got >= 7) {
            usb_hub_descriptor_t* hd = (usb_hub_descriptor_t*)buf;
            dev->hub_nports   = hd->bNbrPorts;
            dev->hub_char     = hd->wHubCharacteristics;
            dev->hub_pwr2good = hd->bPwrOn2PwrGood;
        }
    }

    /* 7. Controller-side endpoint setup (xHCI Configure Endpoint). */
    step = "configure";
    if (hc->configure && hc->configure(hc, dev) < 0) goto fail;

    /* 8. Select the configuration. */
    step = "set_config";
    if (usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_CONFIG, dev->config_value, 0, 0, 0) < 0)
        goto fail;
    dev->configured = true;

    log_device("device", dev);

    /* 9. Hubs: power the ports and enumerate what is behind them. */
    if (dev->is_hub) usb_hub_attach(dev);
    return dev;

fail:
    {
        char path[24];
        usb_port_path(dev, path, sizeof(path));
        usb_log("USB: enumeration FAILED at "); usb_log(step);
        usb_log(" ("); usb_log(hc->name); usb_log(" "); usb_log(path); usb_log(")\n");
    }
    if (hc->release_device) hc->release_device(hc, dev);
    dev->active = false;
    dev->configured = false;
    usb_gen++;
    return NULL;
}

/* --------------------------------------------------------------------------
 * Removal (recursive for hubs)
 * -------------------------------------------------------------------------- */
void usb_device_remove(usb_device_t* dev) {
    if (!dev || !dev->active) return;
    int idx = usb_device_index(dev);
    for (int i = 0; i < num_usb_devices; i++) {
        if (usb_devices[i].active && usb_devices[i].parent == idx && &usb_devices[i] != dev)
            usb_device_remove(&usb_devices[i]);
    }
    if (dev->is_hub) usb_hub_detach(dev);
    if (dev->hcd && dev->hcd->release_device) dev->hcd->release_device(dev->hcd, dev);
    log_device("removed", dev);
    dev->active = false;
    dev->configured = false;
    for (int i = 0; i < USB_MAX_ENDPOINTS; i++) dev->endpoints[i].hc_priv = NULL;
    usb_gen++;
}

/* --------------------------------------------------------------------------
 * HCD registration: assign a bus number and enumerate the root ports.
 * -------------------------------------------------------------------------- */
usb_hcd_t* usb_hcd_alloc(void) {
    if (num_hcds >= USB_MAX_HCDS) return NULL;
    usb_hcd_t* hc = &hcds[num_hcds];
    memset(hc, 0, sizeof(*hc));
    hc->bus = (uint8_t)(num_hcds + 1);
    return hc;
}

void usb_hcd_register(usb_hcd_t* hc) {
    if (!hc || hc != &hcds[num_hcds]) return;    /* must be the slot from usb_hcd_alloc */
    hc->active = true;
    num_hcds++;

    usb_log("USB: "); usb_log(hc->name); usb_log(" bus "); usb_log_dec(hc->bus);
    usb_log(" @ "); usb_log_hex(hc->base); usb_log(" ports="); usb_log_dec(hc->nports); usb_log("\n");

    for (int p = 1; p <= hc->nports; p++) {
        int chg = 0;
        if (!hc->port_status(hc, p, &chg)) continue;
        int speed = hc->port_reset(hc, p);
        if (speed < 0) continue;
        usb_enumerate_device(hc, p, speed, NULL, 0);
    }
}

/* --------------------------------------------------------------------------
 * Hot-plug polling (idle pumps). At most once per PIT tick (~55 ms).
 * -------------------------------------------------------------------------- */
void usb_poll(void) {
    static uint32_t last_tick = 0xFFFFFFFFu;
    if (num_hcds == 0) return;
    if (system_ticks == last_tick) return;
    last_tick = system_ticks;

    uint32_t g0 = usb_gen;

    for (int h = 0; h < num_hcds; h++) {
        usb_hcd_t* hc = &hcds[h];
        if (!hc->active) continue;
        for (int p = 1; p <= hc->nports; p++) {
            int chg = 0;
            int conn = hc->port_status(hc, p, &chg);
            if (!chg) continue;
            usb_log("USB: "); usb_log(hc->name); usb_log(" root port "); usb_log_dec(p);
            usb_log(conn ? " connected\n" : " disconnected\n");
            usb_device_t* old = usb_device_on_root_port(hc, p);
            if (old) usb_device_remove(old);     /* a change means (re)plugged */
            if (conn) {
                usb_delay_ms(100);               /* connection debounce */
                int speed = hc->port_reset(hc, p);
                if (speed >= 0) usb_enumerate_device(hc, p, speed, NULL, 0);
            }
        }
    }

    usb_hub_poll();

    if (usb_gen != g0) {
        usb_hid_rescan();
        usb_storage_rescan();
    }
}

/* --------------------------------------------------------------------------
 * usb_init: bring up every host controller found on PCI, enumerate devices.
 * -------------------------------------------------------------------------- */
void usb_init(void) {
    memset(usb_devices, 0, sizeof(usb_devices));
    memset(hcds, 0, sizeof(hcds));
    num_usb_devices = 0;
    num_hcds = 0;
    usb_gen = 0;
    pit_detect();
    usb_log("USB: PIT delay calibration "); usb_log_dec(pit_units_per_ms); usb_log(" units/ms\n");

    /* Order matters: EHCI must claim its ports and hand full/low-speed devices
     * to the companion UHCI/OHCI controllers before those are initialized. */
    usb_xhci_init();
    usb_ehci_init();
    usb_uhci_init();
    usb_ohci_init();

    int ndev = 0;
    for (int i = 0; i < num_usb_devices; i++) if (usb_devices[i].active) ndev++;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    if (num_hcds == 0) {
        vga_print("USB: no host controller (UHCI/OHCI/EHCI/xHCI)\n");
        usb_log("USB: no host controller\n");
        return;
    }
    vga_print("USB: ");
    char b[12];
    for (int i = 0; i < num_hcds; i++) {
        if (i) vga_print(", ");
        vga_print(hcds[i].name);
        vga_print(" @");
        int_to_hex(hcds[i].base, b);
        vga_print(hcds[i].type == USB_HCD_UHCI ? b + 6 : b + 2);
    }
    vga_print("; ");
    int_to_str(ndev, b); vga_print(b);
    vga_print(" device(s)\n");
    usb_log("USB: init done, "); usb_log_dec(ndev); usb_log(" device(s)\n");
}
