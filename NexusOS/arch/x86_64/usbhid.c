/* USB HID class: keyboards, mice and absolute pointers (tablets) feeding /dev/input through input.c.
 *
 * Boot keyboards run the boot protocol (8-byte reports: modifiers, reserved, six key usages). Every
 * other interface runs the report protocol and its report descriptor is parsed (usage pages, usages
 * and ranges, logical minimum/maximum, report size/count/ID, push/pop, constant padding). Reports are
 * then decoded generically: keyboard page 7 (variable modifier bits and key arrays, rollover), button
 * page 9, Generic Desktop X/Y (relative -> REL_X/REL_Y, absolute -> ABS_X/ABS_Y scaled to 0..32767),
 * wheel, Consumer AC Pan, digitizer tip/barrel switches. The boot keyboard itself is described by the
 * HID 1.11 Appendix B.1 descriptor, so both paths share one decoder.
 *
 * Keys reach the keyboard node (index 0), pointer events the pointer node (index 1) that the PS/2
 * devices also use; key repeat is generated here (250 ms delay, 33 ms period, like Linux's software
 * autorepeat for USB keyboards) as value-2 events, which is what PS/2 typematic produces in input.c. */
#include "usb.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define MAX_STATES 16
#define MAX_ITEMS 150
#define MAX_USAGES 64
#define MAX_REPORT_IDS 16
#define ABS_RANGE 32767
#define REPEAT_DELAY_NS 250000000ULL
#define REPEAT_PERIOD_NS 33000000ULL
#define USAGE(page, id) (((uint32_t)(page) << 16) | (id))
enum { ITEM_CONSTANT = 1, ITEM_VARIABLE = 2, ITEM_RELATIVE = 4 };
enum { EV_KEY = 1, EV_REL = 2, EV_ABS = 3, BTN_LEFT = 0x110 };

struct hid_item {
    uint32_t usage_min, usage_max;
    int32_t logical_min, logical_max;
    uint16_t bit;
    uint8_t size, count, report_id, flags;
};
struct hid_state {
    struct usb_device* device;
    unsigned interface, item_count, repeat_code;
    uint64_t repeat_due;
    bool uses_ids, boot, keyboard, pointer, absolute;
    uint8_t keys[32];      /* pressed keyboard usages */
    uint8_t buttons;
    int32_t last_x, last_y;
    struct hid_item items[MAX_ITEMS];
};
_Static_assert(sizeof(struct hid_state) <= 4096, "HID state must fit one pool page");
static struct hid_state* states[MAX_STATES];

/* HID keyboard usage -> Linux key code (same table as Linux hid-input.c; 0 = no key). */
static const uint8_t usage_keys[256] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,179,180,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,  0,  0,  0,  0,
     29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
    150,158,159,128,136,177,178,176,142,152,173,140,  0,  0,  0,  0,
};
/* HID 1.11 Appendix B.1 (keyboard) and B.2 (mouse) boot-protocol report descriptors. */
static const uint8_t boot_keyboard[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01, 0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02, 0x95,0x01, 0x75,0x03,
    0x91,0x01, 0x95,0x06, 0x75,0x08, 0x15,0x00, 0x26,0xFF,0x00, 0x05,0x07, 0x19,0x00, 0x2A,0xFF,0x00, 0x81,0x00, 0xC0,
};
static const uint8_t boot_mouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01, 0x95,0x03,
    0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x05, 0x81,0x01, 0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F, 0x75,0x08,
    0x95,0x02, 0x81,0x06, 0xC0, 0xC0,
};

/* ---- report descriptor parser ---- */
struct parse_global { uint32_t page, size, count, report_id, raw_max; int32_t logical_min, logical_max; };
static void add_item(struct hid_state* state, const struct hid_item* item) {
    if (state->item_count < MAX_ITEMS) state->items[state->item_count++] = *item;
}
static bool parse(struct hid_state* state, const uint8_t* data, unsigned length) {
    struct parse_global global = {0}, stack[4];
    unsigned depth = 0, usage_count = 0, report_count = 0;
    uint32_t usages[MAX_USAGES], usage_min = 0, usage_max = 0;
    bool have_min = false, have_max = false;
    struct { uint8_t id; uint16_t bits; } offsets[MAX_REPORT_IDS];
    state->item_count = 0; state->uses_ids = false;
    for (unsigned at = 0; at < length;) {
        uint8_t prefix = data[at++];
        if (prefix == 0xFE) { if (at + 2 > length) return false; at += 2U + data[at]; continue; } /* long item */
        unsigned size = prefix & 3; if (size == 3) size = 4;
        if (at + size > length) return false;
        uint32_t value = 0;
        for (unsigned k = 0; k < size; k++) value |= (uint32_t)data[at + k] << (8 * k);
        at += size;
        int32_t signed_value = size == 1 ? (int8_t)value : size == 2 ? (int16_t)value : (int32_t)value;
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        /* usages shorter than 4 bytes take the usage page current at the main item */
        #define RESOLVE(u) ((u) & 0x80000000U ? (u) & 0x7FFFFFFFU : USAGE(global.page, (u) & 0xFFFF))
        if (type == 1) {
            switch (tag) {
            case 0: global.page = value & 0xFFFF; break;
            case 1: global.logical_min = signed_value; break;
            case 2: global.logical_max = signed_value; global.raw_max = value; break;
            case 7: global.size = value; break;
            case 8: global.report_id = value & 0xFF; state->uses_ids = true; break;
            case 9: global.count = value; break;
            case 10: if (depth < 4) stack[depth++] = global; break;
            case 11: if (depth) global = stack[--depth]; break;
            default: break;
            }
        } else if (type == 2) {
            uint32_t usage = size == 4 ? (value | 0x80000000U) : value; /* marker: page already included */
            if (tag == 0 && usage_count < MAX_USAGES) usages[usage_count++] = usage;
            else if (tag == 1) { usage_min = usage; have_min = true; }
            else if (tag == 2) { usage_max = usage; have_max = true; }
        } else if (type == 0) {
            if (tag == 8) { /* Input */
                unsigned slot = 0;
                while (slot < report_count && offsets[slot].id != global.report_id) slot++;
                if (slot == report_count) {
                    if (report_count == MAX_REPORT_IDS) return false;
                    offsets[report_count].id = (uint8_t)global.report_id; offsets[report_count].bits = 0; report_count++;
                }
                unsigned position = offsets[slot].bits, total = global.size * global.count;
                if (global.size > 32 || total > 4096 || position + total > 0xFFFF) return false;
                int32_t logical_max = global.logical_min >= 0 && global.logical_max < global.logical_min ? (int32_t)global.raw_max : global.logical_max;
                if (!(value & ITEM_CONSTANT) && global.size && global.count) {
                    bool range = have_min && have_max;
                    if (value & ITEM_VARIABLE) {
                        for (unsigned k = 0; k < global.count && k < 255; k++) {
                            uint32_t usage = k < usage_count ? RESOLVE(usages[k]) : range ? RESOLVE(usage_min) + k : usage_count ? RESOLVE(usages[usage_count - 1]) : 0;
                            if (range && k >= usage_count && usage > RESOLVE(usage_max)) break;
                            struct hid_item item = {usage, usage, global.logical_min, logical_max, (uint16_t)(position + k * global.size),
                                                    (uint8_t)global.size, 1, (uint8_t)global.report_id, (uint8_t)(value & 7)};
                            add_item(state, &item);
                        }
                    } else {
                        uint32_t first = range ? RESOLVE(usage_min) : usage_count ? RESOLVE(usages[0]) : 0;
                        uint32_t last = range ? RESOLVE(usage_max) : usage_count ? RESOLVE(usages[usage_count - 1]) : 0;
                        struct hid_item item = {first, last, global.logical_min, logical_max, (uint16_t)position, (uint8_t)global.size,
                                                (uint8_t)(global.count > 255 ? 255 : global.count), (uint8_t)global.report_id, (uint8_t)(value & 7)};
                        add_item(state, &item);
                    }
                }
                offsets[slot].bits = (uint16_t)(position + total);
            }
            if (tag == 8 || tag == 9 || tag == 10 || tag == 11 || tag == 12) { usage_count = 0; have_min = have_max = false; }
        }
        #undef RESOLVE
    }
    return state->item_count != 0;
}

/* ---- report decoding ---- */
static uint32_t field(const uint8_t* data, unsigned bit, unsigned size, bool is_signed) {
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++, bit++) if (data[bit >> 3] & (1U << (bit & 7))) value |= 1U << i;
    if (is_signed && size < 32 && (value & (1U << (size - 1)))) value |= ~0U << size;
    return value;
}
static int32_t scale(int32_t value, const struct hid_item* item) {
    int64_t span = (int64_t)item->logical_max - item->logical_min;
    if (span <= 0) return 0;
    int64_t scaled = ((int64_t)value - item->logical_min) * ABS_RANGE / span;
    return scaled < 0 ? 0 : scaled > ABS_RANGE ? ABS_RANGE : (int32_t)scaled;
}
static bool key_down(const uint8_t* keys, unsigned usage) { return (keys[usage / 8] & (1U << (usage % 8))) != 0; }
static void keyboard_update(struct hid_state* state, const uint8_t* keys) {
    bool changed = false; uint64_t now = native_clock_ns();
    for (unsigned pass = 0; pass < 2; pass++) /* releases before presses */
        for (unsigned usage = 4; usage < 256; usage++) {
            bool now_down = key_down(keys, usage), was_down = key_down(state->keys, usage);
            if (now_down == was_down || now_down != (pass == 1)) continue;
            unsigned code = usage_keys[usage];
            if (!code) continue;
            input_source_key(0, code, now_down); changed = true;
            if (now_down && (usage < 0xE0 || usage > 0xE7)) { state->repeat_code = code; state->repeat_due = now + REPEAT_DELAY_NS; }
            else if (!now_down && state->repeat_code == code) state->repeat_code = 0;
        }
    memcpy(state->keys, keys, sizeof(state->keys));
    if (changed) input_source_report(0);
}
static void report(struct usb_endpoint* endpoint, const uint8_t* data, unsigned length) {
    struct hid_state* state = endpoint->owner;
    if (!state || !state->device) return;
    uint8_t id = 0;
    if (state->uses_ids) { if (!length) return; id = data[0]; data++; length--; }
    uint8_t keys[32] = {0}; bool keyboard = false, rollover = false, pointer = false, motion = false;
    int32_t dx = 0, dy = 0, wheel = 0, pan = 0, x = -1, y = -1; unsigned buttons = 0;
    for (unsigned i = 0; i < state->item_count; i++) {
        const struct hid_item* item = &state->items[i];
        if (item->report_id != id || item->bit + (unsigned)item->size * item->count > length * 8) continue;
        bool is_signed = item->logical_min < 0;
        if (item->flags & ITEM_VARIABLE) {
            int32_t value = (int32_t)field(data, item->bit, item->size, is_signed);
            uint32_t usage = item->usage_min; unsigned page = usage >> 16, number = usage & 0xFFFF;
            if (page == 7) { keyboard = true; if (value && number < 256) keys[number / 8] |= (uint8_t)(1U << (number % 8)); }
            else if (page == 9) { pointer = true; if (value && number >= 1 && number <= 8) buttons |= 1U << (number - 1); }
            else if (usage == USAGE(0x0D, 0x42) || usage == USAGE(0x0D, 0x44)) { pointer = true; if (value) buttons |= usage == USAGE(0x0D, 0x42) ? 1U : 2U; }
            else if (usage == USAGE(1, 0x30) || usage == USAGE(1, 0x31)) {
                pointer = true; bool horizontal = usage == USAGE(1, 0x30);
                if (item->flags & ITEM_RELATIVE) { if (horizontal) dx += value; else dy += value; }
                else if (horizontal) x = scale(value, item); else y = scale(value, item);
            } else if (usage == USAGE(1, 0x38)) { pointer = true; wheel += value; }
            else if (usage == USAGE(0x0C, 0x238)) { pointer = true; pan += value; }
        } else {
            for (unsigned k = 0; k < item->count; k++) {
                int32_t value = (int32_t)field(data, item->bit + k * item->size, item->size, is_signed);
                if (value < item->logical_min || value > item->logical_max) continue;
                uint32_t usage = item->usage_min + (uint32_t)(value - item->logical_min);
                if (usage > item->usage_max) continue;
                unsigned page = usage >> 16, number = usage & 0xFFFF;
                if (page == 7) {
                    keyboard = true;
                    if (number >= 1 && number <= 3) rollover = true; /* ErrorRollOver / POSTFail / ErrorUndefined */
                    else if (number >= 4 && number < 256) keys[number / 8] |= (uint8_t)(1U << (number % 8));
                } else if (page == 9 && number >= 1 && number <= 8) { pointer = true; buttons |= 1U << (number - 1); }
            }
        }
    }
    if (keyboard && !rollover && state->keyboard) keyboard_update(state, keys);
    if (!pointer || !state->pointer) return;
    if (dx) { input_source_motion(EV_REL, 0, dx); motion = true; }
    if (dy) { input_source_motion(EV_REL, 1, dy); motion = true; }
    if (x >= 0 && x != state->last_x) { input_source_motion(EV_ABS, 0, x); state->last_x = x; motion = true; }
    if (y >= 0 && y != state->last_y) { input_source_motion(EV_ABS, 1, y); state->last_y = y; motion = true; }
    if (wheel) { input_source_motion(EV_REL, 8, wheel); motion = true; }
    if (pan) { input_source_motion(EV_REL, 6, pan); motion = true; }
    for (unsigned b = 0; b < 8; b++) if ((buttons ^ state->buttons) & (1U << b)) { input_source_key(1, BTN_LEFT + b, (buttons >> b) & 1); motion = true; }
    state->buttons = (uint8_t)buttons;
    if (motion) input_source_report(1);
}

/* ---- attach / detach ---- */
static bool classify(struct hid_state* state) {
    for (unsigned i = 0; i < state->item_count; i++) {
        uint32_t usage = state->items[i].usage_min, page = usage >> 16;
        if (page == 7 || (state->items[i].usage_max >> 16) == 7) state->keyboard = true;
        if (usage == USAGE(1, 0x30) || usage == USAGE(1, 0x31)) {
            state->pointer = true;
            if (!(state->items[i].flags & ITEM_RELATIVE)) state->absolute = true;
        }
        if (page == 9 || usage == USAGE(1, 0x38)) state->pointer = true;
    }
    return state->keyboard || state->pointer;
}
bool usbhid_attach(struct usb_device* device, unsigned interface) {
    struct usb_interface_info* info = &device->interfaces[interface];
    unsigned slot = 0;
    while (slot < MAX_STATES && states[slot]) slot++;
    if (slot == MAX_STATES) return false;
    if (!(info->subclass == 1 && (info->protocol == 1 || info->protocol == 2)) && info->report_length) {   /* game controllers: gamepad.c */
        static uint8_t descriptor[4096];                                 /* worker only; the scratch page is reused by requests */
        unsigned length = info->report_length > 4096 ? 4096 : info->report_length; unsigned kind;
        int result = usb_control(device, 0x81, 6, 0x2200, info->number, NULL, (uint16_t)length);
        if (result > 0) memcpy(descriptor, (const void*)xhci_scratch(device->hc), (unsigned)result);
        if (result > 0 && (gamepad_sony(device->vendor, device->product, &kind) || gamepad_descriptor(descriptor, (unsigned)result)))
            return gamepad_hid_attach(device, interface, descriptor, (unsigned)result);
    }
    struct hid_state* state = (struct hid_state*)xhci_page_alloc(device->hc);
    if (!state) return false;
    state->device = device; state->interface = interface; state->last_x = state->last_y = -1;
    bool parsed = false;
    if (info->subclass == 1 && info->protocol == 1 && usb_control(device, 0x21, 0x0B, 0, info->number, NULL, 0) >= 0) {
        parsed = parse(state, boot_keyboard, sizeof(boot_keyboard)); state->boot = true;
    }
    if (!parsed && info->report_length) {
        if (info->subclass == 1) usb_control(device, 0x21, 0x0B, 1, info->number, NULL, 0); /* report protocol */
        unsigned length = info->report_length > 4096 ? 4096 : info->report_length;
        int result = usb_control(device, 0x81, 6, 0x2200, info->number, NULL, (uint16_t)length);
        if (result > 0) parsed = parse(state, (const uint8_t*)xhci_scratch(device->hc), (unsigned)result);
        if (!parsed) usb_log_device(device, "HID report descriptor not understood");
    }
    if (!parsed && info->subclass == 1 && info->protocol == 2 && usb_control(device, 0x21, 0x0B, 0, info->number, NULL, 0) >= 0) {
        parsed = parse(state, boot_mouse, sizeof(boot_mouse)); state->boot = true;
    }
    if (!parsed || !classify(state)) { xhci_page_free(device->hc, (uint64_t)state); return false; }
    usb_control(device, 0x21, 0x0A, 0, info->number, NULL, 0); /* SET_IDLE 0: reports on change only (may stall) */
    states[slot] = state; device->class_state[interface] = state;
    struct usb_endpoint* endpoint = &device->endpoints[interface];
    endpoint->owner = state; endpoint->complete = report;
    if (!xhci_endpoint_start(endpoint)) usb_log_device(device, "HID interrupt endpoint could not start");
    usb_log_device(device, state->keyboard ? (state->boot ? "keyboard ready (boot protocol)" : "keyboard ready (report protocol)") :
                   state->absolute ? "absolute pointer ready (tablet, ABS_X/ABS_Y 0..32767)" : "relative pointer ready (mouse)");
    return true;
}
void usbhid_detach(struct usb_device* device, unsigned interface) {
    if (gamepad_hid_detach(device, interface)) return;                  /* a game controller (gamepad.c) */
    struct hid_state* state = device->class_state[interface];
    if (!state) return;
    uint8_t none[32] = {0};
    if (state->keyboard) keyboard_update(state, none);
    if (state->pointer && state->buttons) {
        for (unsigned b = 0; b < 8; b++) if (state->buttons & (1U << b)) input_source_key(1, BTN_LEFT + b, 0);
        state->buttons = 0; input_source_report(1);
    }
    state->repeat_code = 0; state->device = NULL;
    for (unsigned i = 0; i < MAX_STATES; i++) if (states[i] == state) states[i] = NULL;
    device->class_state[interface] = NULL;
    device->endpoints[interface].owner = NULL; device->endpoints[interface].complete = NULL;
    xhci_page_free(device->hc, (uint64_t)state);
}
void usbhid_poll(uint64_t now) {
    for (unsigned i = 0; i < MAX_STATES; i++) {
        struct hid_state* state = states[i];
        if (!state || !state->repeat_code || now < state->repeat_due) continue;
        input_source_key(0, state->repeat_code, 2); input_source_report(0);
        state->repeat_due = now - state->repeat_due > REPEAT_PERIOD_NS ? now + REPEAT_PERIOD_NS : state->repeat_due + REPEAT_PERIOD_NS;
    }
}
uint64_t usbhid_next_event(void) {   /* tick.c via usb.c: the next key repeat (~0: no key held) */
    uint64_t due = ~0ULL;
    for (unsigned i = 0; i < MAX_STATES; i++) { const struct hid_state* s = states[i]; if (s && s->repeat_code && s->repeat_due < due) due = s->repeat_due; }
    return due;
}
void usbhid_register_capabilities(void) {
    uint8_t keys[96] = {0}, buttons[96] = {0};
    for (unsigned usage = 0; usage < 256; usage++) if (usage_keys[usage]) keys[usage_keys[usage] / 8] |= (uint8_t)(1U << (usage_keys[usage] % 8));
    for (unsigned b = 0; b < 8; b++) buttons[(BTN_LEFT + b) / 8] |= (uint8_t)(1U << ((BTN_LEFT + b) % 8));
    input_source_attach(0, keys, false);
    input_source_attach(1, buttons, true);
}
