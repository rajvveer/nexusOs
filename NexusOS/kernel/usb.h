/* ============================================================================
 * NexusOS — USB Core (Header) — Phase 25 / Phase 54 (real UHCI) / Phase 55
 * ============================================================================
 * Phase 55: host-controller abstraction. The core (usb.c) owns the device
 * table, descriptor-driven enumeration, the transfer API used by the class
 * drivers (usb_hid.c, usb_storage.c, usb_hub.c) and hot-plug polling. Each
 * host-controller driver (usb_uhci.c, usb_ohci.c, usb_ehci.c, usb_xhci.c)
 * registers a usb_hcd_t whose ops the core dispatches through dev->hcd.
 * Devices behind hubs are enumerated by the hub class driver (usb_hub.c)
 * through the same core entry point, usb_enumerate_device().
 * ============================================================================ */

#ifndef USB_H
#define USB_H

#include "types.h"
#include "pci.h"

/* USB limits */
#define USB_MAX_DEVICES    16
#define USB_MAX_ENDPOINTS  8
#define USB_MAX_HCDS       6
#define USB_MAX_HUB_DEPTH  3      /* nested hub tiers we will walk */

/* USB speeds */
#define USB_SPEED_LOW      0   /* 1.5 Mbps */
#define USB_SPEED_FULL     1   /* 12 Mbps */
#define USB_SPEED_HIGH     2   /* 480 Mbps */
#define USB_SPEED_SUPER    3   /* 5 Gbps */

/* USB request types (bmRequestType) */
#define USB_DIR_OUT            0x00
#define USB_DIR_IN             0x80
#define USB_TYPE_STANDARD      0x00
#define USB_TYPE_CLASS         0x20
#define USB_TYPE_VENDOR        0x40
#define USB_RECIP_DEVICE       0x00
#define USB_RECIP_INTERFACE    0x01
#define USB_RECIP_ENDPOINT     0x02
#define USB_RECIP_OTHER        0x03

/* USB standard requests (bRequest) */
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_FEATURE    0x03
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_GET_CONFIG     0x08
#define USB_REQ_SET_CONFIG     0x09
#define USB_REQ_SET_INTERFACE  0x0B

/* HID class requests */
#define USB_HID_SET_IDLE       0x0A
#define USB_HID_SET_PROTOCOL   0x0B

/* USB descriptor types */
#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIG        0x02
#define USB_DESC_STRING        0x03
#define USB_DESC_INTERFACE     0x04
#define USB_DESC_ENDPOINT      0x05
#define USB_DESC_HUB           0x29
#define USB_DESC_SS_HUB        0x2A

/* USB class codes */
#define USB_CLASS_HID          0x03
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB          0x09

/* USB endpoint attributes / direction */
#define USB_EP_DIR_IN          0x80
#define USB_EP_XFER_MASK       0x03
#define USB_EP_XFER_CTRL       0x00
#define USB_EP_XFER_ISOC       0x01
#define USB_EP_XFER_BULK       0x02
#define USB_EP_XFER_INT        0x03

/* Host-controller types (== PCI prog-if >> 4) */
#define USB_HCD_UHCI           0
#define USB_HCD_OHCI           1
#define USB_HCD_EHCI           2
#define USB_HCD_XHCI           3

/* USB device descriptor (18 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

/* USB config descriptor (9 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

/* USB interface descriptor (9 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_descriptor_t;

/* USB endpoint descriptor (7 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

/* USB hub descriptor (first 7 fixed bytes; port bitmaps follow) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;      /* 0x29 (USB2) / 0x2A (USB3) */
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;       /* in 2 ms units */
    uint8_t  bHubContrCurrent;
} usb_hub_descriptor_t;

struct usb_device;
struct usb_hcd;
typedef struct usb_device usb_device_t;
typedef struct usb_hcd    usb_hcd_t;

/* USB endpoint (runtime) */
typedef struct {
    uint8_t  address;         /* bEndpointAddress (incl. dir bit) */
    uint8_t  attributes;      /* transfer type */
    uint16_t max_packet_size;
    uint8_t  interval;
    uint8_t  toggle;          /* data toggle state for int/bulk */
    void*    hc_priv;         /* Phase 55: HCD per-endpoint state (ring / QH / ED) */
} usb_endpoint_t;

/* USB device */
struct usb_device {
    uint8_t  address;         /* USB address (1-127); xHCI: slot id */
    uint8_t  speed;           /* USB_SPEED_* */
    uint8_t  class_code;      /* effective class (interface class for comp devs) */
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  config_value;    /* bConfigurationValue to select */
    uint8_t  num_endpoints;
    uint16_t max_packet0;     /* bMaxPacketSize0 in bytes (8/16/32/64; 512 for SS) */
    uint16_t vendor_id;
    uint16_t product_id;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    bool     active;
    bool     configured;

    /* ---- Phase 55: topology ---- */
    usb_hcd_t* hcd;           /* owning host controller */
    uint8_t  port;            /* port number (1-based) on the root hub or on the parent hub */
    uint8_t  root_port;       /* root-hub port this device hangs off (1-based) */
    int8_t   parent;          /* index of the parent hub device in the table, -1 = root port */
    uint8_t  depth;           /* hub tiers between device and root hub (0 = on a root port) */
    uint32_t route;           /* xHCI route string (4 bits per tier) */
    uint8_t  slot_id;         /* xHCI device slot */
    uint8_t  is_hub;
    uint8_t  hub_nports;      /* hub: bNbrPorts */
    uint8_t  hub_pwr2good;    /* hub: bPwrOn2PwrGood (2 ms units) */
    uint16_t hub_char;        /* hub: wHubCharacteristics */
    uint32_t gen;             /* unique id of this enumeration (changes on re-plug) */
    uint32_t reports;         /* interrupt-IN transfers that returned data (HID proof) */
    void*    hc_priv;         /* HCD per-device state (xHCI slot/contexts) */
};

/* Host-controller driver instance. Port numbers are 1-based everywhere. */
struct usb_hcd {
    const char*   name;       /* "UHCI" / "OHCI" / "EHCI" / "xHCI" */
    uint8_t       type;       /* USB_HCD_* */
    uint8_t       bus;        /* 1-based bus number used in port paths ("1-2.3") */
    uint8_t       nports;     /* root-hub ports */
    bool          active;
    uint32_t      base;       /* I/O base (UHCI) or MMIO base, for display */
    pci_device_t* pci;
    void*         priv;

    /* Root hub: connect state of a port (1/0). *changed is set if a
     * connect-status-change was pending; the change bits are cleared. */
    int  (*port_status)(usb_hcd_t* hc, int port, int* changed);
    /* Reset + enable a connected port. Returns the attached device's
     * USB_SPEED_*, or -1 if nothing usable is there (or the port was handed to
     * a companion controller). */
    int  (*port_reset)(usb_hcd_t* hc, int port);

    /* Transfers. control: 8-byte SETUP packet + optional data stage. All
     * return bytes moved (>=0) or -1. interrupt_in returns 0 when no report
     * has arrived yet (it keeps one transfer armed between polls). */
    int  (*control)(usb_hcd_t* hc, usb_device_t* dev, const uint8_t* setup,
                    void* data, int len, int dir_in);
    int  (*interrupt_in)(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                         void* buf, int len);
    int  (*bulk)(usb_hcd_t* hc, usb_device_t* dev, usb_endpoint_t* ep,
                 void* buf, int len, int dir_in);

    /* Optional device-lifecycle hooks (xHCI needs all four; NULL otherwise).
     * enable_device: before the first GET_DESCRIPTOR — allocate a slot and
     *   address the device (the core then skips SET_ADDRESS).
     * update_mps0:   after the real bMaxPacketSize0 is known.
     * configure:     endpoints parsed, before SET_CONFIGURATION.
     * release_device: device unplugged or enumeration failed — free HC state
     *   (every HCD should at least drop its per-endpoint interrupt slots). */
    int  (*enable_device)(usb_hcd_t* hc, usb_device_t* dev);
    int  (*update_mps0)(usb_hcd_t* hc, usb_device_t* dev);
    int  (*configure)(usb_hcd_t* hc, usb_device_t* dev);
    void (*release_device)(usb_hcd_t* hc, usb_device_t* dev);
};

/* ---------------------------------------------------------------------------
 * Core API (used by kernel.c, shell.c and the class drivers)
 * ------------------------------------------------------------------------- */
void usb_init(void);              /* bring up every controller, enumerate */
void usb_poll(void);              /* hot-plug: root ports + hubs (idle pumps) */

usb_device_t* usb_get_devices(void);
int           usb_device_count(void);   /* high-water mark; skip !active */
const char*   usb_class_name(uint8_t class_code);
const char*   usb_speed_name(uint8_t speed);   /* "LS"/"FS"/"HS"/"SS" */
bool          usb_controller_present(void);
uint32_t      usb_generation(void);      /* bumps on every add/remove */

/* Controllers */
int        usb_hcd_count(void);
usb_hcd_t* usb_hcd_get(int i);

/* Port path of a device, e.g. "1-2.3" (bus-rootport.hubport...). */
void usb_port_path(usb_device_t* dev, char* out, int outlen);

/* Control transfer on endpoint 0. Returns bytes transferred (>=0) or -1. */
int usb_control(usb_device_t* dev, uint8_t bmRequestType, uint8_t bRequest,
                uint16_t wValue, uint16_t wIndex, uint16_t wLength, void* data);

/* Interrupt IN poll. Returns bytes received, 0 if nothing yet, -1 on error. */
int usb_interrupt_in(usb_device_t* dev, usb_endpoint_t* ep, void* buf, int len);

/* Bulk transfer (dir_in != 0 => IN). Returns bytes transferred or -1. */
int usb_bulk(usb_device_t* dev, usb_endpoint_t* ep, void* buf, int len, int dir_in);

/* Find first endpoint matching a transfer type + direction. NULL if none. */
usb_endpoint_t* usb_find_endpoint(usb_device_t* dev, uint8_t xfer_type, int dir_in);

/* ---------------------------------------------------------------------------
 * HCD-facing API
 * ------------------------------------------------------------------------- */
/* Get an empty controller slot (NULL if the table is full). The HCD fills the
 * ops and calls usb_hcd_register(), which scans the root ports. */
usb_hcd_t* usb_hcd_alloc(void);
void       usb_hcd_register(usb_hcd_t* hc);

/* Enumerate the device on an already-reset port (root port if hub == NULL,
 * else `hubport` of `hub`). Returns the device or NULL. */
usb_device_t* usb_enumerate_device(usb_hcd_t* hc, int port, int speed,
                                   usb_device_t* hub, int hubport);
/* Mark a device (and everything behind it) gone and free its HC state. */
void usb_device_remove(usb_device_t* dev);
/* Device attached to root port `port` of `hc` (NULL if none). */
usb_device_t* usb_device_on_root_port(usb_hcd_t* hc, int port);
/* Child of `hub` on hub port `port` (NULL if none). */
usb_device_t* usb_device_on_hub_port(usb_device_t* hub, int port);
/* Index of a device in the table. */
int usb_device_index(usb_device_t* dev);

/* Timing (PIT-calibrated busy waits; task context only, never from an IRQ). */
void usb_delay_ms(uint32_t ms);
void usb_delay_us(uint32_t us);

/* Serial (COM1) debug log — the test harness greps for these lines. */
void usb_log(const char* s);
void usb_log_hex(uint32_t v);
void usb_log_dec(uint32_t v);

/* Host-controller drivers: scan PCI for their class and register. */
void usb_uhci_init(void);
void usb_ohci_init(void);
void usb_ehci_init(void);
void usb_xhci_init(void);

/* Hub class driver (usb_hub.c). */
void usb_hub_attach(usb_device_t* hub);   /* power ports + enumerate children */
void usb_hub_detach(usb_device_t* hub);   /* hub unplugged (children already removed) */
void usb_hub_poll(void);                  /* status-change endpoints (hot-plug) */

#endif /* USB_H */
