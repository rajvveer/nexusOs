/* Class drivers beyond hub/HID/mass storage, bound after SET_CONFIGURATION (usb.c bind): USB Audio Class (uaudio.c),
 * USB Video Class (uvc.c) and Xbox game controllers (xpad.c). A device may carry several (a webcam with a microphone:
 * video + audio). Detach runs before the slot is disabled; after a power loss (device->gone) without commands. */
#include "usb.h"
#include "../../kernel/string.h"

bool uaudio_wanted(const struct usb_device* device);
void uaudio_attach(struct usb_device* device);
void uaudio_detach(struct usb_device* device);
bool uaudio_service(void);
bool uvc_wanted(const struct usb_device* device);
void uvc_attach(struct usb_device* device);
void uvc_detach(struct usb_device* device);
bool uvc_service(void);
void uvc_poll(uint64_t now);
bool xpad_wanted(const struct usb_device* device);
void xpad_attach(struct usb_device* device);
void xpad_detach(struct usb_device* device);
bool xpad_service(void);
void xpad_poll(uint64_t now);

bool btusb_wanted(const struct usb_device* device);   /* Bluetooth controllers (btusb.c): class_driver[3] */
void btusb_attach(struct usb_device* device);
void btusb_detach(struct usb_device* device);
bool btusb_service(void);

bool usbclass_wanted(const struct usb_device* device) { return uaudio_wanted(device) || uvc_wanted(device) || xpad_wanted(device) || btusb_wanted(device); }
const char* usbclass_name(const struct usb_device* device) {
    bool audio = uaudio_wanted(device), video = uvc_wanted(device);
    return video && audio ? "USB video + audio device" : video ? "USB video device" : audio ? "USB audio device" : xpad_wanted(device) ? "game controller" :
           btusb_wanted(device) ? "Bluetooth controller" : "class device";
}
void usbclass_attach(struct usb_device* device) {
    if (btusb_wanted(device)) { btusb_attach(device); return; }       /* a Bluetooth adapter's audio/ISO interfaces stay its own */
    if (uvc_wanted(device)) uvc_attach(device);
    if (uaudio_wanted(device)) uaudio_attach(device);
    if (xpad_wanted(device)) xpad_attach(device);
}
void usbclass_detach(struct usb_device* device) {
    if (device->class_driver[3]) btusb_detach(device);
    if (device->class_driver[2]) xpad_detach(device);
    if (device->class_driver[0]) uaudio_detach(device);
    if (device->class_driver[1]) uvc_detach(device);
}
bool usbclass_service(void) {
    bool work = uaudio_service();
    work |= uvc_service();
    work |= xpad_service();
    work |= gamepad_service();
    work |= btusb_service();
    return work;
}
void usbclass_poll(uint64_t now) { uvc_poll(now); xpad_poll(now); }
