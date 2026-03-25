/* ============================================================================
 * NexusOS — USB Core (Header) — Phase 25
 * ============================================================================
 * USB host controller detection, device enumeration, descriptor parsing.
 * Supports UHCI (Universal Host Controller Interface) via PCI.
 * ============================================================================ */

#ifndef USB_H
#define USB_H

#include "types.h"
#include "pci.h"

/* USB limits */
#define USB_MAX_DEVICES    16
#define USB_MAX_ENDPOINTS  8

/* USB speeds */
#define USB_SPEED_LOW      0   /* 1.5 Mbps */
#define USB_SPEED_FULL     1   /* 12 Mbps */
#define USB_SPEED_HIGH     2   /* 480 Mbps */

/* USB request types */
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG     0x09

/* USB descriptor types */
#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIG        0x02
#define USB_DESC_STRING        0x03
#define USB_DESC_INTERFACE     0x04
#define USB_DESC_ENDPOINT      0x05

/* USB class codes */
#define USB_CLASS_HID          0x03
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB          0x09

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

/* USB endpoint */
typedef struct {
    uint8_t  address;
    uint8_t  attributes;
    uint16_t max_packet_size;
    uint8_t  interval;
} usb_endpoint_t;

/* USB device */
typedef struct {
    uint8_t  address;       /* USB address (1-127) */
    uint8_t  speed;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  num_endpoints;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    bool     active;
    bool     configured;
} usb_device_t;

/* UHCI controller state */
typedef struct {
    uint16_t io_base;       /* I/O port base from PCI BAR4 */
    bool     active;
    pci_device_t* pci_dev;
} uhci_controller_t;

/* Initialize USB subsystem */
void usb_init(void);

/* Get USB devices */
usb_device_t* usb_get_devices(void);
int usb_device_count(void);

/* Get USB device class name */
const char* usb_class_name(uint8_t class_code);

#endif /* USB_H */
