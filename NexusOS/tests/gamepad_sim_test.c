/* Host simulator for the game-controller drivers: QEMU has no USB game controller, so the real entry points of
 * arch/x86_64/gamepad.c (HID gamepads/joysticks, Sony DualShock 4 / DualSense) and arch/x86_64/xpad.c (Xbox 360 XInput,
 * Xbox One / Series GIP) run here against simulated devices built from real descriptor and packet layouts:
 *  - "f310": the Logitech Dual Action / F310 (DirectInput mode, 046d:c216) report descriptor - a Joystick application,
 *    four 8-bit axes (X, Y, Z, Rz), a 4-bit hat with a null state, 12 buttons, 16 vendor bits;
 *  - "pad16": a Game Pad with report IDs, 16 buttons from a usage range, signed 16-bit X/Y/Rx/Ry, a 1..8 hat, padding,
 *    Simulation Accelerator/Brake and an output report;
 *  - DualShock 4 (054c:05c4) and DualSense (054c:0ce6) USB input report 0x01 and rumble output reports 0x05 / 0x02;
 *  - Xbox 360 wired (045e:028e, interface ff/5d/01) and Xbox One S (045e:02ea, ff/47/d0) with interrupt IN/OUT pipes:
 *    LED / power-on packets, input packets, the GIP guide button with its acknowledgement, rumble packets, a halted IN
 *    pipe being reset, unplug.
 * The input core is a recording mock of evdev.h (descriptions, events, syncs, force-feedback callbacks); evdev.c itself is
 * tested in the guest through uinput (tests/test_x64_gamepad.py). Hostile input: truncated and random report
 * descriptors, oversized/nested/odd fields, short and random reports for every driver; all memory must come back. */
#define NEXUS_HOST_TEST 1
#include "../arch/x86_64/usb.h"
#include "../arch/x86_64/evdev.h"
#include "../kernel/string.h"
#include "../kernel/vga.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>

static unsigned allocations; static uint64_t allocated;
uint64_t arena_alloc(uint64_t bytes) { uint8_t* b = calloc(1, bytes + 4096); assert(b); allocations++; allocated += bytes; return ((uint64_t)(uintptr_t)b + 4095) & ~4095ULL; }
uint64_t dma32_alloc(uint64_t bytes) { return arena_alloc(bytes); }
void arena_free(uint64_t address, uint64_t bytes) { assert(address && allocations); allocations--; allocated -= bytes; }
static bool quiet;
void vga_print(const char* text) { if (!quiet) fputs(text, stdout); }
void usb_log_device(const struct usb_device* device, const char* what) { (void)device; if (!quiet) printf("[usb] %s\n", what); }
static unsigned kicks;
void usb_worker_kick(void) { kicks++; }

/* ---------------- recording evdev mock ---------------- */
struct event { unsigned type, code; int value; };
struct evdev { bool used, destroyed; struct evdev_desc desc; const struct evdev_ops* ops; void* context; struct event events[4096]; unsigned count, syncs; int abs[ABS_MAX + 1]; uint8_t keys[96]; };
static struct evdev mocks[8];
struct evdev* evdev_create(const struct evdev_desc* desc, const struct evdev_ops* ops, void* context) {
    for (unsigned i = 0; i < 8; i++) if (!mocks[i].used) {
        struct evdev* e = &mocks[i]; memset(e, 0, sizeof(*e)); e->used = true; e->desc = *desc; e->ops = ops; e->context = context;
        for (unsigned a = 0; a <= ABS_MAX; a++) e->abs[a] = desc->absinfo[a].value;
        return e;
    }
    return NULL;
}
void evdev_destroy(struct evdev* e) { assert(e && e->used && !e->destroyed); e->destroyed = true; e->used = false; }
void* evdev_context(const struct evdev* e) { return e ? e->context : NULL; }
void evdev_emit(struct evdev* e, unsigned type, unsigned code, int value) {
    assert(e && e->used && !e->destroyed);
    assert(type == EV_KEY || type == EV_ABS);
    if (type == EV_KEY) { assert(code <= KEY_MAX && evdev_test_bit(e->desc.key, code) && (value == 0 || value == 1)); if (value) evdev_set_bit(e->keys, code); else e->keys[code / 8] &= (uint8_t)~(1U << (code % 8)); }
    if (type == EV_ABS) {                                                /* the driver must stay inside its declared range */
        assert(code <= ABS_MAX && evdev_test_bit(e->desc.abs, code));
        if (e->desc.absinfo[code].minimum < e->desc.absinfo[code].maximum) assert(value >= e->desc.absinfo[code].minimum && value <= e->desc.absinfo[code].maximum);
        e->abs[code] = value;
    }
    if (e->count < 4096) e->events[e->count++] = (struct event){type, code, value};
}
void evdev_sync(struct evdev* e) { assert(e && e->used); e->syncs++; }
static bool key(const struct evdev* e, unsigned code) { return evdev_test_bit(e->keys, code); }
static struct evdev* live_mock(void) { for (unsigned i = 0; i < 8; i++) if (mocks[i].used) return &mocks[i]; return NULL; }
static unsigned live_mocks(void) { unsigned n = 0; for (unsigned i = 0; i < 8; i++) n += mocks[i].used; return n; }

/* ---------------- simulated device: control requests, HID endpoint, pipes ---------------- */
static struct usb_device dev;
static uint8_t config[512]; static unsigned config_length;
struct control { uint8_t type, request; uint16_t value, index, length; uint8_t data[64]; };
static struct control controls[64]; static unsigned control_count;
static int stall_set_idle;
int usb_control(struct usb_device* device, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length) {
    assert(device == &dev);
    if (device->gone) return -19;
    if (control_count < 64) { controls[control_count] = (struct control){type, request, value, index, length, {0}}; if (data && length) memcpy(controls[control_count].data, data, length > 64 ? 64 : length); control_count++; }
    if (type == 0x21 && request == 0x0A) return stall_set_idle ? -32 : 0;
    if (type == 0x21 && request == 0x09) return length;
    return -32;
}
static bool endpoint_started;
bool xhci_endpoint_start(struct usb_endpoint* endpoint) { assert(endpoint->complete && endpoint->owner); endpoint_started = true; return true; }
const uint8_t* usb_config(const struct usb_device* device, unsigned* length) { (void)device; *length = config_length; return config; }
static void hid_report(unsigned interface, const uint8_t* data, unsigned length) {
    struct usb_endpoint* e = &dev.endpoints[interface];
    if (e->complete) e->complete(e, data, length);
}

struct td { uint64_t physical; uint32_t length, tag; };
static struct { struct usb_pipe* pipe; struct td tds[64]; unsigned head, tail; bool open; uint8_t sent[64][64]; unsigned sent_length[64], sent_count; } pipes[2];
static int find_pipe(struct usb_pipe* p) { for (int i = 0; i < 2; i++) if (pipes[i].open && pipes[i].pipe == p) return i; return -1; }
bool usb_pipe_open(struct usb_device* device, struct usb_pipe* pipe) {
    pipe->device = device; pipe->in = (pipe->address & 0x80) != 0;
    for (int i = 0; i < 2; i++) if (!pipes[i].open) { memset(&pipes[i], 0, sizeof(pipes[i])); pipes[i].pipe = pipe; pipes[i].open = true; pipe->ring = 1; return true; }
    return false;
}
void usb_pipe_close(struct usb_pipe* pipe) { int i = find_pipe(pipe); assert(i >= 0); pipes[i].open = false; pipe->device = NULL; pipe->configured = false; }
bool usb_pipes_configure(struct usb_device* device, struct usb_pipe* const* add, unsigned adds, struct usb_pipe* const* drop, unsigned drops) {
    (void)device;
    for (unsigned i = 0; i < drops; i++) {
        int k = find_pipe(drop[i]);
        if (k >= 0) while (pipes[k].head != pipes[k].tail) { struct td t = pipes[k].tds[pipes[k].head++ % 64]; drop[i]->queued--; drop[i]->complete(drop[i], t.tag, 0, 26); }
        drop[i]->configured = false;
    }
    for (unsigned i = 0; i < adds; i++) { assert(find_pipe(add[i]) >= 0); add[i]->configured = true; add[i]->queued = 0; add[i]->halted = false; }
    return true;
}
int usb_pipe_queue(struct usb_pipe* pipe, uint64_t physical, uint32_t length, uint32_t tag) {
    int k = find_pipe(pipe); assert(k >= 0);
    if (dev.gone) return -19;
    if (!pipe->configured || pipe->halted) return -32;
    if (pipes[k].tail - pipes[k].head >= 32) return -16;
    if (!pipe->in) {                                                     /* OUT: the device takes the packet at once */
        assert(length <= 64);                                            /* the last 64 packets are kept */
        memcpy(pipes[k].sent[pipes[k].sent_count % 64], (const void*)(uintptr_t)physical, length); pipes[k].sent_length[pipes[k].sent_count++ % 64] = length;
        pipe->complete(pipe, tag, length, 1);
        return 0;
    }
    pipes[k].tds[pipes[k].tail++ % 64] = (struct td){physical, length, tag}; pipe->queued++;
    return 0;
}
bool usb_pipe_stop(struct usb_pipe* pipe) {
    int k = find_pipe(pipe); assert(k >= 0);
    while (pipes[k].head != pipes[k].tail) { struct td t = pipes[k].tds[pipes[k].head++ % 64]; pipe->queued--; pipe->complete(pipe, t.tag, 0, 26); }
    pipe->halted = false;
    return true;
}
static int in_pipe(void) { for (int i = 0; i < 2; i++) if (pipes[i].open && pipes[i].pipe->in) return i; return -1; }
static int out_pipe(void) { for (int i = 0; i < 2; i++) if (pipes[i].open && !pipes[i].pipe->in) return i; return -1; }
static unsigned in_queued(void) { int k = in_pipe(); return k < 0 ? 0 : pipes[k].tail - pipes[k].head; }
/* the controller sends one interrupt IN packet (code 1 = success; another code = an error completion) */
static bool interrupt_in(const uint8_t* data, unsigned length, unsigned code) {
    int k = in_pipe(); if (k < 0 || pipes[k].head == pipes[k].tail) return false;
    struct usb_pipe* pipe = pipes[k].pipe; struct td t = pipes[k].tds[pipes[k].head++ % 64]; pipe->queued--;
    assert(length <= t.length);
    if (data) memcpy((void*)(uintptr_t)t.physical, data, length);
    if (code != 1 && code != 13) pipe->halted = true;
    pipe->complete(pipe, t.tag, code == 1 || code == 13 ? length : 0, code);
    return true;
}
static const uint8_t* last_out(unsigned* length) { int k = out_pipe(); if (k < 0 || !pipes[k].sent_count) return NULL; *length = pipes[k].sent_length[(pipes[k].sent_count - 1) % 64]; return pipes[k].sent[(pipes[k].sent_count - 1) % 64]; }
static unsigned out_count(void) { int k = out_pipe(); return k < 0 ? 0 : pipes[k].sent_count; }

#include "../arch/x86_64/gamepad.c"
static unsigned gamepad_live(void) { unsigned n = 0; for (unsigned i = 0; i < PADS; i++) n += pads[i] != NULL; return n; }
static unsigned gamepad_fields(void) { for (unsigned i = 0; i < PADS; i++) if (pads[i]) return pads[i]->field_count; return 0; }
#undef PADS
#define pads xpad_pads
#define ff_play xpad_ff_play
#include "../arch/x86_64/xpad.c"
#undef pads
#undef ff_play

/* ---------------- devices ---------------- */
static void reset(uint16_t vendor, uint16_t product, const char* name) {
    memset(&dev, 0, sizeof(dev)); memset(mocks, 0, sizeof(mocks)); memset(controls, 0, sizeof(controls)); control_count = 0;
    memset(pipes, 0, sizeof(pipes)); config_length = 0; endpoint_started = false; stall_set_idle = 0;
    dev.slot = 1; dev.root_port = 2; dev.vendor = vendor; dev.product = product; dev.speed = USB_SPEED_FULL; strcpy(dev.product_name, name);
    dev.interface_count = 1; dev.interfaces[0] = (struct usb_interface_info){.number = 0, .class_code = 3, .subclass = 0, .protocol = 0};
}
static const uint8_t F310[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0xA1, 0x02, 0x75, 0x08, 0x95, 0x04, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x35, 0x00, 0x46, 0xFF, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02, 0x75, 0x04, 0x95, 0x01, 0x25, 0x07, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x09, 0x39,
    0x81, 0x42, 0x65, 0x00, 0x75, 0x01, 0x95, 0x0C, 0x25, 0x01, 0x45, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0C, 0x81, 0x02, 0x06, 0x00, 0xFF,
    0x75, 0x01, 0x95, 0x10, 0x25, 0x01, 0x45, 0x01, 0x09, 0x01, 0x81, 0x02, 0xC0, 0xA1, 0x02, 0x75, 0x08, 0x95, 0x07, 0x46, 0xFF, 0x00, 0x26,
    0xFF, 0x00, 0x09, 0x02, 0x91, 0x02, 0xC0, 0xC0};
static const uint8_t PAD16[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x66, 0x14, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x02, 0x09, 0xC4, 0x09, 0xC5, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x85, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02,
    0xC0};
static const uint8_t KEYBOARD[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};

static void test_f310(void) {
    reset(0x046D, 0xC216, "Logitech Dual Action");
    assert(gamepad_descriptor(F310, sizeof(F310)) && !gamepad_descriptor(KEYBOARD, sizeof(KEYBOARD)));
    unsigned before = allocations;
    assert(gamepad_hid_attach(&dev, 0, F310, sizeof(F310)));
    struct evdev* e = live_mock(); assert(e && endpoint_started && dev.class_state[0]);
    assert(!strcmp(e->desc.name, "Logitech Dual Action") && e->desc.vendor == 0x046D && e->desc.bustype == BUS_USB && !strcmp(e->desc.phys, "usb-xhci-02/input0"));
    assert(control_count == 1 && controls[0].request == 0x0A);       /* SET_IDLE */
    /* Joystick application: buttons from BTN_JOYSTICK (BTN_TRIGGER), axes X Y Z RZ 0..255, hat */
    for (unsigned b = 0; b < 12; b++) assert(evdev_test_bit(e->desc.key, BTN_JOYSTICK + b));
    assert(!evdev_test_bit(e->desc.key, BTN_JOYSTICK + 12) && !evdev_test_bit(e->desc.key, BTN_SOUTH));
    static const unsigned axes[4] = {ABS_X, ABS_Y, ABS_Z, ABS_RZ};
    for (unsigned a = 0; a < 4; a++) assert(evdev_test_bit(e->desc.abs, axes[a]) && e->desc.absinfo[axes[a]].minimum == 0 && e->desc.absinfo[axes[a]].maximum == 255 && e->desc.absinfo[axes[a]].flat == 15);
    assert(evdev_test_bit(e->desc.abs, ABS_HAT0X) && evdev_test_bit(e->desc.abs, ABS_HAT0Y) && !evdev_test_bit(e->desc.abs, ABS_RX));
    assert(!evdev_test_bit(e->desc.ev, EV_FF));
    /* report: X=0x10 Y=0xF0 Z=0x80 Rz=0x7F, hat 2 (east), buttons 1 and 4 (low nibble of byte 4 is the hat), 9 and 12 */
    uint8_t r[8] = {0x10, 0xF0, 0x80, 0x7F, (uint8_t)(0x02 | (0x1 << 4) | (0x8 << 4)), 0x10 | 0x80, 0x55, 0xAA};
    hid_report(0, r, sizeof(r));
    assert(e->abs[ABS_X] == 0x10 && e->abs[ABS_Y] == 0xF0 && e->abs[ABS_Z] == 0x80 && e->abs[ABS_RZ] == 0x7F);
    assert(e->abs[ABS_HAT0X] == 1 && e->abs[ABS_HAT0Y] == 0);
    assert(key(e, BTN_JOYSTICK + 0) && key(e, BTN_JOYSTICK + 3) && !key(e, BTN_JOYSTICK + 1) && key(e, BTN_JOYSTICK + 8) && key(e, BTN_JOYSTICK + 11) && !key(e, BTN_JOYSTICK + 9));
    assert(e->syncs == 1);
    r[4] = 0x08; r[5] = 0; hid_report(0, r, sizeof(r));               /* hat null state: centred, buttons released */
    assert(e->abs[ABS_HAT0X] == 0 && e->abs[ABS_HAT0Y] == 0 && !key(e, BTN_JOYSTICK) && !key(e, BTN_JOYSTICK + 11));
    r[4] = 0x07; hid_report(0, r, sizeof(r)); assert(e->abs[ABS_HAT0X] == -1 && e->abs[ABS_HAT0Y] == -1);   /* north-west */
    unsigned syncs = e->syncs;
    hid_report(0, r, 3); hid_report(0, NULL, 0);                         /* short reports: fields beyond the data are skipped */
    assert(e->syncs == syncs + 2 && e->abs[ABS_X] == 0x10);
    assert(gamepad_hid_detach(&dev, 0) && e->destroyed && !dev.class_state[0] && !dev.endpoints[0].complete);
    assert(!gamepad_hid_detach(&dev, 0) && allocations == before);
    printf("f310: Joystick application, 4 axes + hat + 12 buttons as BTN_TRIGGER.. like hid-input\n");
}
static void test_pad16(void) {
    reset(0x2DC8, 0x3106, "");
    unsigned before = allocations;
    assert(gamepad_descriptor(PAD16, sizeof(PAD16)) && gamepad_hid_attach(&dev, 0, PAD16, sizeof(PAD16)));
    struct evdev* e = live_mock(); assert(e && !strcmp(e->desc.name, "USB Gamepad"));
    for (unsigned b = 0; b < 16; b++) assert(evdev_test_bit(e->desc.key, BTN_GAMEPAD + b));
    assert(e->desc.absinfo[ABS_X].minimum == -32768 && e->desc.absinfo[ABS_X].maximum == 32767 && e->desc.absinfo[ABS_X].fuzz == 255 && e->desc.absinfo[ABS_X].flat == 4095);
    assert(evdev_test_bit(e->desc.abs, ABS_RX) && evdev_test_bit(e->desc.abs, ABS_RY) && evdev_test_bit(e->desc.abs, ABS_GAS) && evdev_test_bit(e->desc.abs, ABS_BRAKE));
    assert(e->desc.absinfo[ABS_GAS].value == 0 && e->desc.absinfo[ABS_X].value == 0);
    /* report 1: buttons 0x8001 (1 and 16), X=-32768 Y=32767 Rx=-1 Ry=0x1234, hat 3 (east), gas 200 brake 7 */
    uint8_t r[14] = {0x01, 0x01, 0x80, 0x00, 0x80, 0xFF, 0x7F, 0xFF, 0xFF, 0x34, 0x12, 0x03, 200, 7};
    hid_report(0, r, sizeof(r));
    assert(key(e, BTN_SOUTH) && key(e, BTN_GAMEPAD + 15) && !key(e, BTN_EAST));
    assert(e->abs[ABS_X] == -32768 && e->abs[ABS_Y] == 32767 && e->abs[ABS_RX] == -1 && e->abs[ABS_RY] == 0x1234);
    assert(e->abs[ABS_HAT0X] == 1 && e->abs[ABS_HAT0Y] == 0 && e->abs[ABS_GAS] == 200 && e->abs[ABS_BRAKE] == 7);   /* hat 1..8: 1 = north, 3 = east */
    unsigned events = e->count;
    uint8_t other[5] = {0x02, 1, 2, 3, 4}; hid_report(0, other, sizeof(other));   /* another report id: no fields */
    assert(e->count == events);
    r[11] = 0x00; hid_report(0, r, sizeof(r)); assert(e->abs[ABS_HAT0X] == 0 && e->abs[ABS_HAT0Y] == 0);   /* 0: outside 1..8 */
    assert(gamepad_hid_detach(&dev, 0) && allocations == before);
    printf("pad16: Game Pad with report ids, BTN_SOUTH.., signed 16-bit sticks, 1..8 hat, gas/brake\n");
}
static void test_sony(bool dualsense) {
    reset(0x054C, dualsense ? 0x0CE6 : 0x05C4, dualsense ? "DualSense Wireless Controller" : "Wireless Controller");
    dev.interfaces[0].number = 3;
    unsigned kind = 0; assert(gamepad_sony(dev.vendor, dev.product, &kind) && kind == (dualsense ? KIND_DUALSENSE : KIND_DS4));
    unsigned before = allocations;
    assert(gamepad_hid_attach(&dev, 0, KEYBOARD, sizeof(KEYBOARD)));   /* Sony: the fixed layout, whatever the descriptor */
    struct evdev* e = live_mock(); assert(e);
    assert(!strcmp(e->desc.name, dualsense ? "Sony Interactive Entertainment DualSense Wireless Controller" : "Sony Interactive Entertainment Wireless Controller"));
    assert(evdev_test_bit(e->desc.ev, EV_FF) && evdev_test_bit(e->desc.ff, FF_RUMBLE) && e->desc.ff_max == 16 && e->desc.version == 0x8111);
    static const unsigned buttons[13] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
    for (unsigned i = 0; i < 13; i++) assert(evdev_test_bit(e->desc.key, buttons[i]));
    uint8_t r[64]; memset(r, 0, sizeof(r)); r[0] = 0x01; r[1] = 0x00; r[2] = 0xFF; r[3] = 0x80; r[4] = 0x7F;
    unsigned b = dualsense ? 8 : 5;
    if (dualsense) { r[5] = 0x40; r[6] = 0xC0; } else { r[8] = 0x40; r[9] = 0xC0; }
    r[b] = 0x06 | 0x20 | 0x80;          /* hat 6 (west), Cross, Triangle */
    r[b + 1] = 0x01 | 0x20 | 0x80;      /* L1, Options, R3 */
    r[b + 2] = 0x01;                    /* PS */
    hid_report(0, r, dualsense ? 64 : 64);
    assert(e->abs[ABS_X] == 0 && e->abs[ABS_Y] == 255 && e->abs[ABS_RX] == 0x80 && e->abs[ABS_RY] == 0x7F && e->abs[ABS_Z] == 0x40 && e->abs[ABS_RZ] == 0xC0);
    assert(e->abs[ABS_HAT0X] == -1 && e->abs[ABS_HAT0Y] == 0);
    assert(key(e, BTN_SOUTH) && key(e, BTN_NORTH) && !key(e, BTN_WEST) && !key(e, BTN_EAST));
    assert(key(e, BTN_TL) && key(e, BTN_START) && key(e, BTN_THUMBR) && !key(e, BTN_TR) && key(e, BTN_MODE));
    r[b] = 0x08; r[b + 1] = 0; r[b + 2] = 0; hid_report(0, r, 64);      /* hat 8 = released */
    assert(e->abs[ABS_HAT0X] == 0 && e->abs[ABS_HAT0Y] == 0 && !key(e, BTN_SOUTH) && !key(e, BTN_MODE));
    unsigned events = e->count;
    hid_report(0, r, 10); uint8_t bt[64] = {0x11}; hid_report(0, bt, 64);   /* too short / not report 1 (the Bluetooth layout) */
    assert(e->count == events);
    /* rumble: ff_play from the input core -> SET_REPORT from the worker */
    unsigned controls_before = control_count;
    e->ops->ff_play(e, 0xFF00, 0x8000);
    assert(control_count == controls_before);                          /* nothing from the caller's context */
    assert(gamepad_service() && control_count == controls_before + 1);
    struct control* c = &controls[control_count - 1];
    assert(c->type == 0x21 && c->request == 0x09 && c->index == 3);
    if (dualsense) assert(c->value == 0x0202 && c->length == 63 && c->data[0] == 0x02 && c->data[1] == 0x03 && c->data[3] == 0x80 && c->data[4] == 0xFF);
    else assert(c->value == 0x0205 && c->length == 32 && c->data[0] == 0x05 && (c->data[1] & 1) && c->data[4] == 0x80 && c->data[5] == 0xFF);
    e->ops->ff_play(e, 0xFF00, 0x8000); assert(gamepad_service() && control_count == controls_before + 1);   /* unchanged: not resent */
    e->ops->ff_play(e, 0, 0); assert(gamepad_service() && control_count == controls_before + 2);
    assert(!gamepad_service());
    /* unplug with rumble pending: nothing is sent to a gone device */
    e->ops->ff_play(e, 0x4000, 0);
    assert(gamepad_hid_detach(&dev, 0) && !gamepad_service() && allocations == before);
    printf("%s: hid-playstation layout, rumble report 0x%02x via SET_REPORT from the worker\n", dualsense ? "dualsense" : "ds4", dualsense ? 2 : 5);
}
static void build_xbox(bool one) {
    config_length = 0;
    uint8_t c[9] = {9, 2, 0, 0, 1, 1, 0, 0xA0, 0xFA}; memcpy(config, c, 9); config_length = 9;
    uint8_t i[9] = {9, 4, 0, 0, 2, 0xFF, (uint8_t)(one ? 0x47 : 0x5D), (uint8_t)(one ? 0xD0 : 0x01), 0}; memcpy(config + config_length, i, 9); config_length += 9;
    if (!one) { uint8_t vendor[17] = {17, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0, 0, 0, 0, 0x13, 0x02, 0x08, 0, 0}; memcpy(config + config_length, vendor, 17); config_length += 17; }
    uint8_t in[7] = {7, 5, 0x81, 3, (uint8_t)(one ? 64 : 32), 0, (uint8_t)(one ? 4 : 4)}; memcpy(config + config_length, in, 7); config_length += 7;
    uint8_t out[7] = {7, 5, (uint8_t)(one ? 0x01 : 0x02), 3, (uint8_t)(one ? 64 : 32), 0, 8}; memcpy(config + config_length, out, 7); config_length += 7;
    config[2] = (uint8_t)config_length;
    dev.interfaces[0] = (struct usb_interface_info){.number = 0, .class_code = 0xFF, .subclass = (uint8_t)(one ? 0x47 : 0x5D), .protocol = (uint8_t)(one ? 0xD0 : 0x01)};
}
static void test_xbox360(void) {
    reset(0x045E, 0x028E, "Controller"); build_xbox(false);
    unsigned before = allocations;
    assert(xpad_wanted(&dev));
    xpad_attach(&dev);
    struct evdev* e = live_mock(); assert(e && dev.class_driver[2]);
    assert(!strcmp(e->desc.name, "Microsoft X-Box 360 pad") && e->desc.vendor == 0x045E && e->desc.product == 0x028E && e->desc.version == 0x0114);
    assert(evdev_test_bit(e->desc.key, BTN_SOUTH) && evdev_test_bit(e->desc.key, 0x133) && evdev_test_bit(e->desc.key, 0x134) && evdev_test_bit(e->desc.key, BTN_MODE));
    assert(e->desc.absinfo[ABS_X].minimum == -32768 && e->desc.absinfo[ABS_Z].maximum == 255 && e->desc.absinfo[ABS_X].flat == 128);
    assert(in_queued() == 4);
    unsigned n; const uint8_t* led = last_out(&n); assert(led && n == 3 && led[0] == 0x01 && led[1] == 0x03 && led[2] == 0x06);
    /* input: up+start+LS, LB+guide+A+Y, LT 0x80 RT 0xFF, LX -32768 LY 32767 (up: reported -32768), RX 1 RY -1 */
    uint8_t r[20] = {0x00, 0x14, 0x01 | 0x10 | 0x40, 0x01 | 0x04 | 0x10 | 0x80, 0x80, 0xFF, 0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    assert(interrupt_in(r, 20, 1));
    assert(e->abs[ABS_HAT0Y] == -1 && e->abs[ABS_HAT0X] == 0 && key(e, BTN_START) && key(e, BTN_THUMBL) && !key(e, BTN_SELECT));
    assert(key(e, BTN_TL) && key(e, BTN_MODE) && key(e, BTN_SOUTH) && key(e, 0x134) && !key(e, 0x133) && !key(e, BTN_EAST));
    assert(e->abs[ABS_Z] == 0x80 && e->abs[ABS_RZ] == 0xFF && e->abs[ABS_X] == -32768 && e->abs[ABS_Y] == -32768 && e->abs[ABS_RX] == 1 && e->abs[ABS_RY] == 0);
    assert(in_queued() == 4);                                            /* the buffer went back */
    unsigned syncs = e->syncs;
    uint8_t status[3] = {0x01, 0x03, 0x02}; assert(interrupt_in(status, 3, 1));   /* LED status: not an input report */
    uint8_t shortr[10] = {0x00, 0x14}; assert(interrupt_in(shortr, 10, 1));
    assert(e->syncs == syncs);
    /* rumble */
    unsigned outs = out_count();
    e->ops->ff_play(e, 0xC000, 0x4000); assert(out_count() == outs);
    assert(xpad_service() && out_count() == outs + 1);
    const uint8_t* p = last_out(&n); assert(n == 8 && p[0] == 0x00 && p[1] == 0x08 && p[2] == 0x00 && p[3] == 0xC0 && p[4] == 0x40);
    /* a transaction error halts the IN pipe; the worker resets and requeues it */
    assert(interrupt_in(NULL, 0, 4) && pipes[in_pipe()].pipe->halted);
    assert(xpad_service() && !pipes[in_pipe()].pipe->halted && in_queued() == 4);
    r[3] = 0x20; assert(interrupt_in(r, 20, 1) && key(e, BTN_EAST) && !key(e, BTN_SOUTH));
    /* unplug */
    xpad_detach(&dev);
    assert(e->destroyed && !dev.class_driver[2] && in_pipe() < 0 && out_pipe() < 0 && allocations == before && !xpad_service());
    printf("xbox360: xpad layout (X/Y = BTN_X/BTN_Y, Y up negative), LED, rumble 00 08 00 s w, halted pipe reset\n");
}
static void test_xbox_one(void) {
    reset(0x045E, 0x02EA, "Controller"); build_xbox(true);
    unsigned before = allocations;
    xpad_attach(&dev);
    struct evdev* e = live_mock(); assert(e && !strcmp(e->desc.name, "Microsoft X-Box One S pad") && e->desc.absinfo[ABS_Z].maximum == 1023);
    unsigned n; const uint8_t* power = last_out(&n); assert(power && n == 5 && power[0] == 0x05 && power[1] == 0x20 && power[3] == 0x01 && power[4] == 0x00);
    /* GIP input: menu + A + X, down + RB + RS, LT 1023 RT 512, LX 1000 LY -1000, RX -32768 RY 32767 */
    uint8_t r[18] = {0x20, 0x00, 0x05, 0x0E, 0x04 | 0x10 | 0x40, 0x02 | 0x20 | 0x80, 0xFF, 0x03, 0x00, 0x02, 0xE8, 0x03, 0x18, 0xFC, 0x00, 0x80, 0xFF, 0x7F};
    assert(interrupt_in(r, 18, 1));
    assert(key(e, BTN_START) && key(e, BTN_SOUTH) && key(e, 0x133) && !key(e, 0x134) && !key(e, BTN_SELECT));
    assert(e->abs[ABS_HAT0Y] == 1 && key(e, BTN_TR) && key(e, BTN_THUMBR) && !key(e, BTN_TL));
    assert(e->abs[ABS_Z] == 1023 && e->abs[ABS_RZ] == 512 && e->abs[ABS_X] == 1000 && e->abs[ABS_Y] == 999 && e->abs[ABS_RX] == -32768 && e->abs[ABS_RY] == -32768);
    /* guide button: virtual key packet asking for an acknowledgement */
    unsigned outs = out_count();
    uint8_t guide[6] = {0x07, 0x30, 0x09, 0x02, 0x01, 0x5B}; assert(interrupt_in(guide, 6, 1));
    assert(key(e, BTN_MODE) && out_count() == outs + 1);
    const uint8_t* ack = last_out(&n); assert(n == 13 && ack[0] == 0x01 && ack[1] == 0x20 && ack[2] == 0x09 && ack[3] == 0x09 && ack[5] == 0x07 && ack[6] == 0x20);
    guide[1] = 0x20; guide[4] = 0x00; assert(interrupt_in(guide, 6, 1) && !key(e, BTN_MODE) && out_count() == outs + 1);
    /* rumble: command 0x09, motors 0..127 */
    e->ops->ff_play(e, 0xFFFF, 0x8000); assert(xpad_service());
    const uint8_t* p = last_out(&n); assert(n == 13 && p[0] == 0x09 && p[3] == 0x09 && p[5] == 0x0F && p[8] == 0x7F && p[9] == 0x40 && p[10] == 0xFF && p[12] == 0xFF);
    uint8_t tiny[3] = {0x20, 0, 0}; unsigned syncs = e->syncs; assert(interrupt_in(tiny, 3, 1) && e->syncs == syncs);
    uint8_t short_input[10] = {0x20, 0x00, 0x06, 0x0E}; assert(interrupt_in(short_input, 10, 1) && e->syncs == syncs);
    dev.gone = true;                                                     /* surprise removal: no commands */
    xpad_detach(&dev);
    assert(e->destroyed && allocations == before);
    printf("xbox one: GIP power-on, input, guide with acknowledgement, rumble command 0x09\n");
}
/* ---------------- hostile ---------------- */
static uint32_t lcg = 0xC0FFEE;
static uint32_t random32(void) { lcg = lcg * 1664525U + 1013904223U; return lcg; }
static void test_hostile(void) {
    quiet = true;
    unsigned before = allocations, attached = 0, refused = 0;
    /* truncated real descriptors */
    for (unsigned cut = 0; cut < sizeof(PAD16); cut++) {
        reset(0x1234, 0x0001, "cut");
        gamepad_descriptor(PAD16, cut);
        if (gamepad_hid_attach(&dev, 0, PAD16, cut)) { attached++; uint8_t r[14] = {1, 0xFF, 0xFF, 0x00, 0x80}; hid_report(0, r, 14); assert(gamepad_hid_detach(&dev, 0)); }
        else refused++;
        assert(allocations == before && !live_mocks());
    }
    /* odd but valid shapes: deep push/pop, 32-bit fields, 1-bit signed, a huge report count, many report ids, 300 fields */
    static uint8_t d[4096]; unsigned n = 0;
    #define B(...) do { const uint8_t bytes[] = {__VA_ARGS__}; memcpy(d + n, bytes, sizeof(bytes)); n += sizeof(bytes); } while (0)
    B(0x05, 0x01, 0x09, 0x05, 0xA1, 0x01);
    for (int k = 0; k < 6; k++) B(0xA4);                                   /* push beyond the stack */
    for (int k = 0; k < 6; k++) B(0xB4);
    B(0x09, 0x30, 0x17, 0x00, 0x00, 0x00, 0x80, 0x27, 0xFF, 0xFF, 0xFF, 0x7F, 0x75, 0x20, 0x95, 0x01, 0x81, 0x02);   /* 32-bit signed X */
    B(0x09, 0x31, 0x15, 0xFF, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02);   /* 1-bit signed Y */
    B(0x75, 0x07, 0x81, 0x03);
    B(0x05, 0x09, 0x19, 0x01, 0x29, 0xFF, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x96, 0xFF, 0x00, 0x81, 0x02);   /* 255 buttons */
    for (int id = 2; id < 20; id++) B(0x85, (uint8_t)id, 0x05, 0x01, 0x09, 0x32, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02);
    B(0xC0);
    reset(0x1234, 0x0002, "odd");
    if (gamepad_hid_attach(&dev, 0, d, n)) {
        assert(gamepad_fields() <= FIELDS);
        for (int k = 0; k < 2000; k++) { uint8_t r[64]; unsigned len = random32() % 65; for (unsigned i = 0; i < len; i++) r[i] = (uint8_t)random32(); hid_report(0, r, len); }
        assert(gamepad_hid_detach(&dev, 0));
    }
    assert(allocations == before);
    /* random descriptors: half are grammar-built (a Joystick/Game Pad application with random fields: pages, usages,
     * ranges, sizes 1..33, counts, report ids, signed/unsigned limits, constant/array/relative inputs, outputs), half
     * raw item soup; reports of random length go to whatever attached (the mock asserts every value is in range) */
    unsigned random_attached = 0;
    for (int k = 0; k < 20000; k++) {
        uint8_t* r = malloc(512); unsigned len = 0;
        #define PUT(...) do { const uint8_t bytes[] = {__VA_ARGS__}; if (len + sizeof(bytes) <= 512) { memcpy(r + len, bytes, sizeof(bytes)); len += sizeof(bytes); } } while (0)
        PUT(0x05, 0x01, 0x09, (uint8_t)(random32() & 1 ? 0x05 : 0x04), 0xA1, 0x01);
        if (k % 2) {
            unsigned groups = 1 + random32() % 10;
            for (unsigned g = 0; g < groups; g++) {
                if (random32() % 4 == 0) PUT(0x85, (uint8_t)(1 + random32() % 4));
                unsigned page = random32() % 4; PUT(0x05, (uint8_t)(page == 0 ? 0x01 : page == 1 ? 0x09 : page == 2 ? 0x02 : 0xFF));
                static const uint8_t usages[] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0xC4, 0xC5, 0xBB, 0xBA, 0xC8, 0x01, 0x05};
                if (random32() % 3) for (unsigned u = random32() % 5; u; u--) PUT(0x09, usages[random32() % sizeof(usages)]);
                else PUT(0x19, (uint8_t)(random32() % 3), 0x29, (uint8_t)(random32() % 40));
                switch (random32() % 4) {
                case 0: PUT(0x15, (uint8_t)random32(), 0x25, (uint8_t)random32()); break;
                case 1: PUT(0x16, (uint8_t)random32(), (uint8_t)random32(), 0x26, (uint8_t)random32(), (uint8_t)random32()); break;
                case 2: PUT(0x17, (uint8_t)random32(), (uint8_t)random32(), (uint8_t)random32(), (uint8_t)random32(), 0x27, (uint8_t)random32(), (uint8_t)random32(), (uint8_t)random32(), (uint8_t)random32()); break;
                default: PUT(0x15, 0x00, 0x25, 0x01); break;
                }
                PUT(0x75, (uint8_t)(1 + random32() % 33), 0x95, (uint8_t)(1 + random32() % 20));
                static const uint8_t mains[] = {0x02, 0x02, 0x02, 0x03, 0x42, 0x00, 0x06, 0x01};
                if (random32() % 8) PUT(0x81, mains[random32() % sizeof(mains)]); else PUT(0x91, 0x02);
                if (random32() % 16 == 0) PUT(0xA1, 0x02);
                if (random32() % 16 == 0) PUT(0xC0);
                if (random32() % 32 == 0) PUT((uint8_t)random32());           /* noise */
            }
            PUT(0xC0);
        } else {
            static const uint8_t items[] = {0x05, 0x09, 0x15, 0x25, 0x75, 0x95, 0x81, 0x85, 0xA1, 0xC0, 0x19, 0x29, 0x06, 0x0B, 0xFE, 0x16, 0x26, 0xA4, 0xB4, 0x91, 0x03, 0x07};
            for (unsigned i = random32() % 128; i; i--) PUT(random32() % 3 ? items[random32() % sizeof(items)] : (uint8_t)random32());
        }
        #undef PUT
        if (random32() % 8 == 0 && len) len = random32() % len;             /* truncated */
        reset(0x1234, 0x0003, "fuzz");
        gamepad_descriptor(r, len);
        if (gamepad_hid_attach(&dev, 0, r, len)) {
            random_attached++;
            for (int m = 0; m < 8; m++) { uint8_t rep[64]; unsigned rl = random32() % 65; for (unsigned i = 0; i < rl; i++) rep[i] = (uint8_t)random32(); hid_report(0, rep, rl); }
            assert(gamepad_hid_detach(&dev, 0));
        }
        free(r);
        assert(allocations == before && !gamepad_live());
    }
    /* random reports for Sony and Xbox */
    for (int kind = 0; kind < 4; kind++) {
        if (kind < 2) { reset(0x054C, kind ? 0x0CE6 : 0x09CC, "sony"); assert(gamepad_hid_attach(&dev, 0, KEYBOARD, sizeof(KEYBOARD))); }
        else { reset(0x045E, kind == 2 ? 0x028E : 0x0B12, "xbox"); build_xbox(kind == 3); xpad_attach(&dev); assert(dev.class_driver[2]); }
        for (int k = 0; k < 5000; k++) {
            uint8_t r[64]; unsigned len = random32() % 65; for (unsigned i = 0; i < len; i++) r[i] = (uint8_t)random32();
            if (random32() % 2 && len) r[0] = kind < 2 ? 0x01 : kind == 2 ? 0x00 : (random32() % 2 ? 0x20 : 0x07);
            if (kind < 2) hid_report(0, r, len);
            else { unsigned code = random32() % 10 ? 1 : 4; interrupt_in(r, len, code); if (code != 1) xpad_service(); }
            if (k % 97 == 0) { struct evdev* e = live_mock(); e->ops->ff_play(e, random32() & 0xFFFF, random32() & 0xFFFF); if (kind < 2) gamepad_service(); else xpad_service(); }
        }
        if (kind < 2) assert(gamepad_hid_detach(&dev, 0)); else xpad_detach(&dev);
        assert(allocations == before && !live_mocks());
    }
    /* Xbox interface without usable endpoints */
    reset(0x045E, 0x028E, "noep"); build_xbox(false); config_length = 18; xpad_attach(&dev); assert(!dev.class_driver[2] && allocations == before);
    quiet = false;
    printf("hostile: %u/%u truncated descriptors attached/refused, %u of 20000 random descriptors attached, 20000 random reports, no leak\n", attached, refused, random_attached);
}

int main(void) {
    test_f310();
    test_pad16();
    test_sony(false);
    test_sony(true);
    test_xbox360();
    test_xbox_one();
    test_hostile();
    assert(allocations == 0);
    printf("GAMEPAD SIMULATOR TESTS PASSED\n");
    return 0;
}
