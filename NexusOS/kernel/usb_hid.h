/* ============================================================================
 * NexusOS — USB HID Driver (Header) — Phase 25
 * ============================================================================
 * USB Human Interface Device driver for keyboards and mice.
 * ============================================================================ */

#ifndef USB_HID_H
#define USB_HID_H

#include "types.h"
#include "usb.h"

/* HID subclass */
#define USB_HID_SUBCLASS_BOOT   0x01

/* HID protocol */
#define USB_HID_PROTO_KEYBOARD  0x01
#define USB_HID_PROTO_MOUSE     0x02

/* USB HID keyboard boot report (8 bytes) */
typedef struct {
    uint8_t modifiers;     /* Ctrl, Shift, Alt, GUI */
    uint8_t reserved;
    uint8_t keys[6];       /* Up to 6 simultaneous keys */
} usb_kbd_report_t;

/* USB HID mouse boot report (4 bytes) */
typedef struct {
    uint8_t buttons;       /* Button state */
    int8_t  x_delta;       /* X movement */
    int8_t  y_delta;       /* Y movement */
    int8_t  wheel;         /* Scroll wheel */
} usb_mouse_report_t;

/* Initialize USB HID driver */
void usb_hid_init(void);

/* Check if USB keyboard/mouse is available */
bool usb_hid_keyboard_present(void);
bool usb_hid_mouse_present(void);

#endif /* USB_HID_H */
