/* HID game controllers (usbhid.c hands over every HID interface whose report descriptor has a Joystick or Game Pad
 * application collection, and Sony's controllers) as evdev + joydev devices (evdev.c).
 *
 * Generic HID: the report descriptor's input fields map like Linux hid-input - Generic Desktop X/Y/Z/Rx/Ry/Rz to the
 * ABS axes of the same name (fuzz = range/256, flat = range/16), Slider/Dial/Wheel to THROTTLE/RUDDER/WHEEL, the hat
 * switch to HAT0X/HAT0Y (-1..1, 4- or 8-position, its null state centred), Simulation Accelerator/Brake to GAS/BRAKE,
 * buttons 1..16 to BTN_GAMEPAD+n (Game Pad) or BTN_JOYSTICK+n (Joystick), further buttons to BTN_TRIGGER_HAPPY.
 * Values outside the logical range are dropped for fields with a null state and clamped otherwise (hid-input);
 * axes whose logical maximum is below the minimum and second mappings of a code are ignored. No force feedback (HID PID devices are not driven).
 * Sony DualShock 4 (054c:05c4, 09cc, adapter 0ba0) and DualSense (054c:0ce6, Edge 0df2): the hid-playstation layout that
 * Chrome's and SDL's mappings expect (sticks ABS_X/Y/RX/RY 0..255, triggers ABS_Z/RZ, the D-pad as HAT0, Cross =
 * BTN_SOUTH, Circle = BTN_EAST, Triangle = BTN_NORTH, Square = BTN_WEST, L1/R1/L2/R2, Share/Create = SELECT, Options =
 * START, PS = MODE, L3/R3), decoded from USB input report 0x01; rumble (FF_RUMBLE) through output report 0x05 (DS4) /
 * 0x02 (DualSense, compatible vibration) sent with SET_REPORT by the USB worker. */
#include "usb.h"
#include "evdev.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
/* Device state lives as long as the device, not the program that was running when it was plugged in: counted
 * allocations (storage_alloc.c), which the kernel's per-program allocation audit (kernel.c run_image) leaves out. */
#ifndef NEXUS_HOST_TEST
#include "storage_util.h"
#endif

#define PADS 8U
#define FIELDS 48U
enum { KIND_GENERIC = 0, KIND_DS4, KIND_DUALSENSE };
enum { BTN_TRIGGER_HAPPY = 0x2C0 };
struct pad_field { uint16_t bit; uint8_t size, report_id, kind; bool is_signed, null_state; int32_t min, max; uint16_t code; };   /* kind: 1 abs, 2 key, 3 hat */
struct pad {
    struct usb_device* device; unsigned interface, kind; struct evdev* evdev; bool uses_ids, gone;
    struct pad_field fields[FIELDS]; unsigned field_count;
    volatile bool rumble_pending; uint8_t strong, weak, sent_strong, sent_weak;
};
static struct pad* pads[PADS];

bool gamepad_sony(uint16_t vendor, uint16_t product, unsigned* kind) {
    if (vendor != 0x054C) return false;
    if (product == 0x05C4 || product == 0x09CC || product == 0x0BA0) { *kind = KIND_DS4; return true; }
    if (product == 0x0CE6 || product == 0x0DF2) { *kind = KIND_DUALSENSE; return true; }
    return false;
}
/* Report descriptor: is the first application collection a Joystick (0x04) or Game Pad (0x05)? */
bool gamepad_descriptor(const uint8_t* data, unsigned length) {
    uint32_t page = 0, usage = 0;
    for (unsigned at = 0; at < length;) {
        uint8_t prefix = data[at++];
        if (prefix == 0xFE) { if (at + 2 > length) return false; at += 2U + data[at]; continue; }
        unsigned size = prefix & 3; if (size == 3) size = 4;
        if (at + size > length) return false;
        uint32_t value = 0; for (unsigned k = 0; k < size; k++) value |= (uint32_t)data[at + k] << (8 * k);
        at += size;
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1 && tag == 0) page = value & 0xFFFF;
        else if (type == 2 && tag == 0) usage = size == 4 ? value : (page << 16 | (value & 0xFFFF));
        else if (type == 0 && tag == 10) {                               /* Collection */
            if (value == 1) return usage == 0x00010004 || usage == 0x00010005;   /* the first Application collection decides */
        }
    }
    return false;
}
/* ---------- generic HID ---------- */
static uint16_t axis_code(uint32_t usage) {
    switch (usage) {
    case 0x00010030: return ABS_X; case 0x00010031: return ABS_Y; case 0x00010032: return ABS_Z;
    case 0x00010033: return ABS_RX; case 0x00010034: return ABS_RY; case 0x00010035: return ABS_RZ;
    case 0x00010036: return ABS_THROTTLE; case 0x00010037: return ABS_RUDDER; case 0x00010038: return ABS_WHEEL;
    case 0x000200C4: return ABS_GAS; case 0x000200C5: return ABS_BRAKE; case 0x000200BB: return ABS_THROTTLE; case 0x000200BA: return ABS_RUDDER;
    case 0x000200C8: return ABS_WHEEL;
    }
    return 0xFFFF;
}
static bool parse_generic(struct pad* p, const uint8_t* data, unsigned length, bool* gamepad) {
    struct { uint32_t page, size, count, report_id; int32_t min, max; uint32_t raw_max; } g = {0}, stack[4];
    unsigned depth = 0, usage_count = 0, report_count = 0;
    uint32_t usages[32], usage_min = 0, usage_max = 0; bool have_min = false, have_max = false; bool application_seen = false;
    struct { uint8_t id; uint16_t bits; } offsets[16];
    *gamepad = true; p->field_count = 0;
    for (unsigned at = 0; at < length;) {
        uint8_t prefix = data[at++];
        if (prefix == 0xFE) { if (at + 2 > length) return false; at += 2U + data[at]; continue; }
        unsigned size = prefix & 3; if (size == 3) size = 4;
        if (at + size > length) return false;
        uint32_t value = 0; for (unsigned k = 0; k < size; k++) value |= (uint32_t)data[at + k] << (8 * k);
        at += size;
        int32_t svalue = size == 1 ? (int8_t)value : size == 2 ? (int16_t)value : (int32_t)value;
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        #define RESOLVE(u) ((u) & 0x80000000U ? (u) & 0x7FFFFFFFU : ((g.page << 16) | ((u) & 0xFFFF)))
        if (type == 1) switch (tag) {
            case 0: g.page = value & 0xFFFF; break;
            case 1: g.min = svalue; break;
            case 2: g.max = svalue; g.raw_max = value; break;
            case 7: g.size = value; break;
            case 8: g.report_id = value & 0xFF; p->uses_ids = true; break;
            case 9: g.count = value; break;
            case 10: if (depth < 4) stack[depth++] = g; break;
            case 11: if (depth) g = stack[--depth]; break;
        } else if (type == 2) {
            uint32_t usage = size == 4 ? (value | 0x80000000U) : value;
            if (tag == 0 && usage_count < 32) usages[usage_count++] = usage;
            else if (tag == 1) { usage_min = usage; have_min = true; }
            else if (tag == 2) { usage_max = usage; have_max = true; }
        } else if (type == 0) {
            if (tag == 10 && value == 1 && !application_seen) { application_seen = true; *gamepad = usage_count && RESOLVE(usages[usage_count - 1]) == 0x00010005; }
            if (tag == 8) {
                unsigned slot = 0;
                while (slot < report_count && offsets[slot].id != g.report_id) slot++;
                if (slot == report_count) { if (report_count == 16) return false; offsets[report_count].id = (uint8_t)g.report_id; offsets[report_count++].bits = 0; }
                unsigned position = offsets[slot].bits, total = g.size * g.count;
                if (g.size > 32 || total > 8192 || position + total > 0xFFFF) return false;
                int32_t max = g.min >= 0 && g.max < g.min ? (int32_t)g.raw_max : g.max;
                if (!(value & 1) && (value & 2) && g.size) {                 /* data, variable */
                    bool range = have_min && have_max;
                    for (unsigned k = 0; k < g.count && k < 64; k++) {
                        uint32_t usage = k < usage_count ? RESOLVE(usages[k]) : range ? RESOLVE(usage_min) + k : usage_count ? RESOLVE(usages[usage_count - 1]) : 0;
                        if (range && k >= usage_count && usage > RESOLVE(usage_max)) break;
                        struct pad_field f = {(uint16_t)(position + k * g.size), (uint8_t)g.size, (uint8_t)g.report_id, 0, g.min < 0, (value & 0x40) != 0, g.min, max, 0};
                        if (usage == 0x00010039) { f.kind = 3; f.code = ABS_HAT0X; }
                        else if ((usage >> 16) == 9 && (usage & 0xFFFF) >= 1) {
                            unsigned n = (usage & 0xFFFF) - 1; f.kind = 2;
                            f.code = (uint16_t)(n <= 0xF ? (*gamepad ? BTN_GAMEPAD : BTN_JOYSTICK) + n : BTN_TRIGGER_HAPPY + n - 0x10);
                            if (f.code > KEY_MAX) continue;
                        } else if ((f.code = axis_code(usage)) != 0xFFFF) { if (max < g.min) continue; f.kind = 1; }
                        else continue;
                        bool duplicate = false;                                  /* hid-input ignores a second mapping of a code */
                        for (unsigned i = 0; i < p->field_count && !duplicate; i++) duplicate = p->fields[i].kind == f.kind && p->fields[i].code == f.code;
                        if (!duplicate && p->field_count < FIELDS) p->fields[p->field_count++] = f;
                    }
                }
                offsets[slot].bits = (uint16_t)(position + total);
            }
            if (tag == 8 || tag == 9 || tag == 10 || tag == 11 || tag == 12) { usage_count = 0; have_min = have_max = false; }
        }
        #undef RESOLVE
    }
    return p->field_count != 0;
}
static uint32_t bits_at(const uint8_t* data, unsigned bit, unsigned size) {
    uint32_t value = 0; for (unsigned i = 0; i < size; i++, bit++) if (data[bit >> 3] & (1U << (bit & 7))) value |= 1U << i;
    return value;
}
static void hat(struct evdev* e, int direction) {           /* 0 = north, clockwise; anything else centred */
    static const int8_t dx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    bool valid = direction >= 0 && direction < 8;
    evdev_emit(e, EV_ABS, ABS_HAT0X, valid ? dx[direction] : 0); evdev_emit(e, EV_ABS, ABS_HAT0Y, valid ? dy[direction] : 0);
}
static void report_generic(struct pad* p, const uint8_t* data, unsigned length) {
    uint8_t id = 0;
    if (p->uses_ids) { if (!length) return; id = data[0]; data++; length--; }
    for (unsigned i = 0; i < p->field_count; i++) {
        const struct pad_field* f = &p->fields[i];
        if (f->report_id != id || f->bit + f->size > length * 8) continue;
        uint32_t raw = bits_at(data, f->bit, f->size); int32_t value = (int32_t)raw;
        if (f->is_signed && f->size < 32 && (raw & (1U << (f->size - 1)))) value = (int32_t)(raw | ~0U << f->size);
        if (f->kind == 3) {                                              /* hid-input: (value - min) * 8 / positions */
            int64_t positions = (int64_t)f->max - f->min + 1;
            hat(p->evdev, f->min < f->max && value >= f->min && value <= f->max ? (int)(((int64_t)value - f->min) * 8 / positions) : -1);
            continue;
        }
        if (f->min < f->max && (value < f->min || value > f->max)) {
            if (f->null_state) continue;
            value = value < f->min ? f->min : f->max;
        }
        if (f->kind == 2) evdev_emit(p->evdev, EV_KEY, f->code, value != 0);
        else if (f->kind == 1) evdev_emit(p->evdev, EV_ABS, f->code, value);
    }
    evdev_sync(p->evdev);
}
/* ---------- Sony ---------- */
static void report_sony(struct pad* p, const uint8_t* data, unsigned length) {
    if (length < 11 || data[0] != 0x01) return;                         /* USB input report 0x01 */
    struct evdev* e = p->evdev; bool ds4 = p->kind == KIND_DS4;
    const uint8_t* b = data + (ds4 ? 5 : 8);
    evdev_emit(e, EV_ABS, ABS_X, data[1]); evdev_emit(e, EV_ABS, ABS_Y, data[2]); evdev_emit(e, EV_ABS, ABS_RX, data[3]); evdev_emit(e, EV_ABS, ABS_RY, data[4]);
    evdev_emit(e, EV_ABS, ABS_Z, ds4 ? data[8] : data[5]); evdev_emit(e, EV_ABS, ABS_RZ, ds4 ? data[9] : data[6]);
    hat(e, b[0] & 0x0F);
    evdev_emit(e, EV_KEY, BTN_WEST, b[0] >> 4 & 1); evdev_emit(e, EV_KEY, BTN_SOUTH, b[0] >> 5 & 1);
    evdev_emit(e, EV_KEY, BTN_EAST, b[0] >> 6 & 1); evdev_emit(e, EV_KEY, BTN_NORTH, b[0] >> 7 & 1);
    static const uint16_t second[8] = {BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR};
    for (unsigned i = 0; i < 8; i++) evdev_emit(e, EV_KEY, second[i], b[1] >> i & 1);
    evdev_emit(e, EV_KEY, BTN_MODE, b[2] & 1);
    evdev_sync(e);
}
static void sony_rumble_send(struct pad* p) {
    uint8_t report[64]; memset(report, 0, sizeof(report)); unsigned length; uint8_t id;
    if (p->kind == KIND_DS4) { id = 0x05; length = 32; report[0] = id; report[1] = 0x01 | 0x02; report[4] = p->weak; report[5] = p->strong; report[8] = 0x40; }   /* motors + lightbar (blue) */
    else { id = 0x02; length = 63; report[0] = id; report[1] = 0x01 | 0x02; report[3] = p->weak; report[4] = p->strong; }        /* compatible vibration + haptics select */
    usb_control(p->device, 0x21, 0x09, (uint16_t)(0x0200 | id), (uint16_t)p->device->interfaces[p->interface].number, report, (uint16_t)length);
    p->sent_strong = p->strong; p->sent_weak = p->weak;
}
static void ff_play(struct evdev* e, unsigned strong, unsigned weak) {
    struct pad* p = evdev_context(e); if (!p || p->gone) return;
    p->strong = (uint8_t)(strong >> 8); p->weak = (uint8_t)(weak >> 8); p->rumble_pending = true; usb_worker_kick();
}
static const struct evdev_ops sony_ops = {ff_play, NULL, NULL, NULL};

/* ---------- attach / detach (usbhid.c, USB worker) ---------- */
static void add_abs(struct evdev_desc* d, unsigned code, int min, int max, int fuzz, int flat) {
    evdev_set_bit(d->abs, code); d->absinfo[code] = (struct evdev_absinfo){(int32_t)(((int64_t)min + max) / 2), min, max, fuzz, flat, 0};
    if (code == ABS_Z || code == ABS_RZ || code == ABS_GAS || code == ABS_BRAKE) d->absinfo[code].value = min;   /* triggers rest at 0 */
}
static void report(struct usb_endpoint* endpoint, const uint8_t* data, unsigned length) {
    struct pad* p = endpoint->owner;
    if (!p || p->gone || !p->evdev) return;
    if (p->kind == KIND_GENERIC) report_generic(p, data, length); else report_sony(p, data, length);
}
bool gamepad_hid_attach(struct usb_device* device, unsigned interface, const uint8_t* descriptor, unsigned length) {
    unsigned slot = 0; while (slot < PADS && pads[slot]) slot++;
    if (slot >= PADS) return false;
    struct pad* p = (struct pad*)arena_alloc(sizeof(struct pad)); if (!p) return false;
    memset(p, 0, sizeof(*p)); p->device = device; p->interface = interface;
    struct evdev_desc* d = (struct evdev_desc*)arena_alloc(sizeof(struct evdev_desc));
    if (!d) { arena_free((uint64_t)p, sizeof(struct pad)); return false; }
    memset(d, 0, sizeof(*d));
    d->bustype = BUS_USB; d->vendor = device->vendor; d->product = device->product; d->version = 0x0111;
    evdev_set_bit(d->ev, EV_KEY); evdev_set_bit(d->ev, EV_ABS);
    unsigned kind = KIND_GENERIC;
    if (gamepad_sony(device->vendor, device->product, &kind)) {
        p->kind = kind; d->version = 0x8111;
        strcpy(d->name, kind == KIND_DS4 ? "Sony Interactive Entertainment Wireless Controller" : "Sony Interactive Entertainment DualSense Wireless Controller");
        static const uint16_t axes[6] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};
        for (unsigned i = 0; i < 6; i++) add_abs(d, axes[i], 0, 255, 0, 0);
        add_abs(d, ABS_HAT0X, -1, 1, 0, 0); add_abs(d, ABS_HAT0Y, -1, 1, 0, 0);
        static const uint16_t buttons[13] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
        for (unsigned i = 0; i < 13; i++) evdev_set_bit(d->key, buttons[i]);
        evdev_set_bit(d->ev, EV_FF); evdev_set_bit(d->ff, FF_RUMBLE); d->ff_max = 16;
    } else {
        bool gamepad = true;
        if (!parse_generic(p, descriptor, length, &gamepad)) { arena_free((uint64_t)d, sizeof(*d)); arena_free((uint64_t)p, sizeof(struct pad)); return false; }
        if (device->product_name[0]) { unsigned n = 0; for (; device->product_name[n] && n < EVDEV_NAME - 1; n++) d->name[n] = device->product_name[n]; d->name[n] = 0; }
        else strcpy(d->name, gamepad ? "USB Gamepad" : "USB Joystick");
        for (unsigned i = 0; i < p->field_count; i++) {
            const struct pad_field* f = &p->fields[i];
            if (f->kind == 2) evdev_set_bit(d->key, f->code);
            else if (f->kind == 3) { add_abs(d, ABS_HAT0X, -1, 1, 0, 0); add_abs(d, ABS_HAT0Y, -1, 1, 0, 0); }
            else if (f->kind == 1) { int64_t range = (int64_t)f->max - f->min; add_abs(d, f->code, f->min, f->max, (int)(range >> 8), (int)(range >> 4)); }
        }
    }
    char phys[64] = "usb-xhci-"; unsigned n = (unsigned)strlen(phys);
    phys[n++] = (char)('0' + device->root_port / 10 % 10); phys[n++] = (char)('0' + device->root_port % 10); phys[n] = 0; strcat(phys, "/input0");
    strcpy(d->phys, phys);
    p->evdev = evdev_create(d, p->kind == KIND_GENERIC ? NULL : &sony_ops, p);
    arena_free((uint64_t)d, sizeof(*d));
    if (!p->evdev) { arena_free((uint64_t)p, sizeof(struct pad)); return false; }
    pads[slot] = p; device->class_state[interface] = p;   /* usb.c calls usbhid_detach for it */
    struct usb_endpoint* endpoint = &device->endpoints[interface];
    endpoint->owner = p; endpoint->complete = report;
    usb_control(device, 0x21, 0x0A, 0, device->interfaces[interface].number, NULL, 0);   /* SET_IDLE 0 (may stall) */
    if (!xhci_endpoint_start(endpoint)) usb_log_device(device, "game controller endpoint could not start");
    usb_log_device(device, p->kind == KIND_DS4 ? "game controller: DualShock 4" : p->kind == KIND_DUALSENSE ? "game controller: DualSense" : "game controller: HID");
    return true;
}
bool gamepad_hid_detach(struct usb_device* device, unsigned interface) {
    for (unsigned i = 0; i < PADS; i++) {
        struct pad* p = pads[i];
        if (!p || p->device != device || p->interface != interface) continue;
        p->gone = true; evdev_destroy(p->evdev);
        device->endpoints[interface].owner = NULL; device->endpoints[interface].complete = NULL; device->class_state[interface] = NULL;
        pads[i] = NULL; arena_free((uint64_t)p, sizeof(struct pad));
        return true;
    }
    return false;
}
/* USB worker: rumble changes go out as SET_REPORT. */
bool gamepad_service(void) {
    bool work = false;
    for (unsigned i = 0; i < PADS; i++) {
        struct pad* p = pads[i];
        if (!p || p->gone || !p->rumble_pending) continue;
        p->rumble_pending = false; work = true;
        if (p->strong != p->sent_strong || p->weak != p->sent_weak) sony_rumble_send(p);
    }
    return work;
}
