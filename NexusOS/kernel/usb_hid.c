/* ============================================================================
 * NexusOS — USB HID Driver (Implementation) — Phase 25
 * ============================================================================
 * Scans USB devices for HID class and registers keyboard/mouse.
 * ============================================================================ */

#include "usb_hid.h"
#include "usb.h"
#include "vga.h"
#include "string.h"

static bool has_keyboard = false;
static bool has_mouse = false;

/* --------------------------------------------------------------------------
 * usb_hid_init: Scan for HID devices among enumerated USB devices
 * -------------------------------------------------------------------------- */
void usb_hid_init(void) {
    has_keyboard = false;
    has_mouse = false;

    usb_device_t* devs = usb_get_devices();
    int count = usb_device_count();

    for (int i = 0; i < count; i++) {
        if (!devs[i].active) continue;
        if (devs[i].class_code != USB_CLASS_HID) continue;

        /* Check protocol for keyboard or mouse */
        if (devs[i].protocol == USB_HID_PROTO_KEYBOARD ||
            devs[i].subclass == USB_HID_SUBCLASS_BOOT) {
            has_keyboard = true;
        }
        if (devs[i].protocol == USB_HID_PROTO_MOUSE) {
            has_mouse = true;
        }
    }

    int hid_count = 0;
    if (has_keyboard) hid_count++;
    if (has_mouse) hid_count++;

    if (hid_count > 0) {
        vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("USB HID: ");
        if (has_keyboard) vga_print("keyboard ");
        if (has_mouse) vga_print("mouse ");
        vga_print("\n");
    }
}

bool usb_hid_keyboard_present(void) { return has_keyboard; }
bool usb_hid_mouse_present(void) { return has_mouse; }
