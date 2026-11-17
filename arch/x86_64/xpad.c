/* Xbox controllers over USB (vendor class, like Linux xpad): Xbox 360 wired / XInput (interface ff/5d/01) and Xbox One /
 * Series (GIP, ff/47/d0), as evdev + joydev game controllers with xpad's layout - sticks ABS_X/Y/RX/RY -32768..32767
 * (Y up negative, fuzz 16, flat 128), triggers ABS_Z/ABS_RZ (0..255 on the 360, 0..1023 on GIP), the D-pad as
 * HAT0X/HAT0Y, A/B = BTN_SOUTH/EAST, X/Y = BTN_X/BTN_Y, LB/RB = BTN_TL/TR, Back/View = SELECT, Start/Menu = START,
 * Guide = MODE, stick clicks = THUMBL/THUMBR - and FF_RUMBLE. Interrupt IN and OUT pipes (xhci.c); the 360 gets its
 * player-1 LED pattern, GIP controllers the power-on command (and an acknowledgement for every packet that asks
 * for one: announce, guide button). Rumble: 360 packet 00 08 00 strong weak; GIP command 0x09 (motors 0..127). */
#include "usb.h"
#include "evdev.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
/* Device state lives as long as the device, not the program that was running when it was plugged in: counted
 * allocations (storage_alloc.c), which the kernel's per-program allocation audit (kernel.c run_image) leaves out. */
#ifndef NEXUS_HOST_TEST
#include "storage_util.h"
#endif

#define PADS 4U
#define IN_DEPTH 4U
#define OUT_SLOTS 8U
enum { KIND_360 = 1, KIND_ONE = 2 };
enum { BTN_X = 0x133, BTN_Y = 0x134 };   /* xpad reports Xbox X/Y as BTN_X/BTN_Y (= BTN_NORTH/BTN_WEST), what Chrome's and SDL's mappings expect */
enum { GIP_ACK = 0x01, GIP_ANNOUNCE = 0x02, GIP_POWER = 0x05, GIP_VIRTUAL_KEY = 0x07, GIP_RUMBLE = 0x09, GIP_INPUT = 0x20, GIP_OPT_ACK = 0x10, GIP_OPT_INTERNAL = 0x20 };
struct xpad {
    struct usb_device* device; unsigned kind, interface; struct evdev* evdev; bool gone;
    struct usb_pipe in, out; uint64_t buffers; unsigned out_next, in_next; uint8_t sequence;
    volatile bool rumble_pending; uint8_t strong, weak;
    uint64_t packets, acks;
};
static struct xpad* pads[PADS];

bool xpad_wanted(const struct usb_device* device) {
    for (unsigned i = 0; i < device->interface_count; i++) {
        const struct usb_interface_info* f = &device->interfaces[i];
        if (f->class_code == 0xFF && ((f->subclass == 0x5D && f->protocol == 0x01) || (f->subclass == 0x47 && f->protocol == 0xD0))) return true;
    }
    return false;
}
static uint8_t* buffer(struct xpad* x, unsigned slot) { return (uint8_t*)(x->buffers + (uint64_t)slot * 64); }   /* 0..3 IN, 4..11 OUT */
static void send(struct xpad* x, const uint8_t* data, unsigned length) {
    if (x->gone || !x->out.configured || length > 64) return;
    unsigned slot = IN_DEPTH + x->out_next; x->out_next = (x->out_next + 1) % OUT_SLOTS;
    memcpy(buffer(x, slot), data, length);
    usb_pipe_queue(&x->out, virt_to_phys(buffer(x, slot)), length, slot);
}
static int16_t s16(const uint8_t* p) { return (int16_t)(p[0] | p[1] << 8); }
static int invert(int16_t v) { return -1 - (int)v; }                    /* ~v: Y axes up negative, like xpad */
static void dpad(struct evdev* e, bool up, bool down, bool left, bool right) {
    evdev_emit(e, EV_ABS, ABS_HAT0X, (int)right - (int)left); evdev_emit(e, EV_ABS, ABS_HAT0Y, (int)down - (int)up);
}
static void parse_360(struct xpad* x, const uint8_t* d, unsigned length) {
    if (length < 14 || d[0] != 0x00 || d[1] < 0x14) return;          /* input report (other types: LED/rumble status) */
    struct evdev* e = x->evdev;
    dpad(e, d[2] & 1, d[2] >> 1 & 1, d[2] >> 2 & 1, d[2] >> 3 & 1);
    evdev_emit(e, EV_KEY, BTN_START, d[2] >> 4 & 1); evdev_emit(e, EV_KEY, BTN_SELECT, d[2] >> 5 & 1);
    evdev_emit(e, EV_KEY, BTN_THUMBL, d[2] >> 6 & 1); evdev_emit(e, EV_KEY, BTN_THUMBR, d[2] >> 7 & 1);
    evdev_emit(e, EV_KEY, BTN_TL, d[3] & 1); evdev_emit(e, EV_KEY, BTN_TR, d[3] >> 1 & 1); evdev_emit(e, EV_KEY, BTN_MODE, d[3] >> 2 & 1);
    evdev_emit(e, EV_KEY, BTN_SOUTH, d[3] >> 4 & 1); evdev_emit(e, EV_KEY, BTN_EAST, d[3] >> 5 & 1);
    evdev_emit(e, EV_KEY, BTN_X, d[3] >> 6 & 1); evdev_emit(e, EV_KEY, BTN_Y, d[3] >> 7 & 1);
    evdev_emit(e, EV_ABS, ABS_Z, d[4]); evdev_emit(e, EV_ABS, ABS_RZ, d[5]);
    evdev_emit(e, EV_ABS, ABS_X, s16(d + 6)); evdev_emit(e, EV_ABS, ABS_Y, invert(s16(d + 8)));
    evdev_emit(e, EV_ABS, ABS_RX, s16(d + 10)); evdev_emit(e, EV_ABS, ABS_RY, invert(s16(d + 12)));
    evdev_sync(e);
}
static void gip_ack(struct xpad* x, const uint8_t* d) {
    uint8_t ack[13] = {GIP_ACK, GIP_OPT_INTERNAL, d[2], 0x09, 0x00, d[0], d[1] & 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    if (d[0] == GIP_VIRTUAL_KEY) ack[6] = GIP_OPT_INTERNAL;
    send(x, ack, sizeof(ack)); x->acks++;
}
static void parse_one(struct xpad* x, const uint8_t* d, unsigned length) {
    if (length < 4) return;
    struct evdev* e = x->evdev;
    if (d[1] & GIP_OPT_ACK) gip_ack(x, d);
    if (d[0] == GIP_VIRTUAL_KEY && length >= 5) { evdev_emit(e, EV_KEY, BTN_MODE, d[4] & 1); evdev_sync(e); return; }
    if (d[0] != GIP_INPUT || length < 18) return;
    evdev_emit(e, EV_KEY, BTN_START, d[4] >> 2 & 1); evdev_emit(e, EV_KEY, BTN_SELECT, d[4] >> 3 & 1);
    evdev_emit(e, EV_KEY, BTN_SOUTH, d[4] >> 4 & 1); evdev_emit(e, EV_KEY, BTN_EAST, d[4] >> 5 & 1);
    evdev_emit(e, EV_KEY, BTN_X, d[4] >> 6 & 1); evdev_emit(e, EV_KEY, BTN_Y, d[4] >> 7 & 1);
    dpad(e, d[5] & 1, d[5] >> 1 & 1, d[5] >> 2 & 1, d[5] >> 3 & 1);
    evdev_emit(e, EV_KEY, BTN_TL, d[5] >> 4 & 1); evdev_emit(e, EV_KEY, BTN_TR, d[5] >> 5 & 1);
    evdev_emit(e, EV_KEY, BTN_THUMBL, d[5] >> 6 & 1); evdev_emit(e, EV_KEY, BTN_THUMBR, d[5] >> 7 & 1);
    evdev_emit(e, EV_ABS, ABS_Z, (int)(uint16_t)s16(d + 6) & 0x3FF); evdev_emit(e, EV_ABS, ABS_RZ, (int)(uint16_t)s16(d + 8) & 0x3FF);
    evdev_emit(e, EV_ABS, ABS_X, s16(d + 10)); evdev_emit(e, EV_ABS, ABS_Y, invert(s16(d + 12)));
    evdev_emit(e, EV_ABS, ABS_RX, s16(d + 14)); evdev_emit(e, EV_ABS, ABS_RY, invert(s16(d + 16)));
    evdev_sync(e);
}
static void in_complete(struct usb_pipe* pipe, uint32_t tag, unsigned actual, unsigned code) {
    struct xpad* x = pipe->owner;
    if (x->gone || code == 26) return;
    if ((code == 1 || code == 13) && x->evdev) { x->packets++; if (x->kind == KIND_360) parse_360(x, buffer(x, tag), actual); else parse_one(x, buffer(x, tag), actual); }
    if (!pipe->halted) usb_pipe_queue(pipe, virt_to_phys(buffer(x, tag)), 64, tag);
    else usb_worker_kick();                                              /* xpad_service resets a halted pipe */
}
static void out_complete(struct usb_pipe* pipe, uint32_t tag, unsigned actual, unsigned code) { (void)pipe; (void)tag; (void)actual; (void)code; }
static void ff_play(struct evdev* e, unsigned strong, unsigned weak) {
    struct xpad* x = evdev_context(e); if (!x || x->gone) return;
    x->strong = (uint8_t)(strong >> 8); x->weak = (uint8_t)(weak >> 8); x->rumble_pending = true; usb_worker_kick();
}
static const struct evdev_ops ops = {ff_play, NULL, NULL, NULL};
static void rumble(struct xpad* x) {
    if (x->kind == KIND_360) { uint8_t p[8] = {0x00, 0x08, 0x00, x->strong, x->weak, 0x00, 0x00, 0x00}; send(x, p, 8); }
    else { uint8_t p[13] = {GIP_RUMBLE, 0x00, x->sequence++, 0x09, 0x00, 0x0F, 0x00, 0x00, (uint8_t)(x->strong >> 1), (uint8_t)(x->weak >> 1), 0xFF, 0x00, 0xFF}; send(x, p, 13); }
}
/* Endpoints of the vendor interface from the raw configuration descriptor. */
static bool endpoints(struct usb_device* device, unsigned number, uint8_t* in, uint16_t* in_packet, uint8_t* in_interval, uint8_t* out, uint16_t* out_packet, uint8_t* out_interval) {
    unsigned length; const uint8_t* d = usb_config(device, &length); bool inside = false; *in = *out = 0;
    for (unsigned at = 0; d && at + 2 <= length && d[at] >= 2 && at + d[at] <= length; at += d[at]) {
        if (d[at + 1] == 4 && d[at] >= 9) inside = d[at + 2] == number && d[at + 3] == 0;
        else if (inside && d[at + 1] == 5 && d[at] >= 7 && (d[at + 3] & 3) == 3) {
            if ((d[at + 2] & 0x80) && !*in) { *in = d[at + 2]; *in_packet = (uint16_t)((d[at + 4] | d[at + 5] << 8) & 0x7FF); *in_interval = d[at + 6]; }
            else if (!(d[at + 2] & 0x80) && !*out) { *out = d[at + 2]; *out_packet = (uint16_t)((d[at + 4] | d[at + 5] << 8) & 0x7FF); *out_interval = d[at + 6]; }
        }
    }
    return *in && *out && *in_packet && *out_packet;
}
static const char* name_of(const struct usb_device* device, unsigned kind) {
    if (device->vendor == 0x045E) switch (device->product) {
        case 0x028E: return "Microsoft X-Box 360 pad";
        case 0x02D1: case 0x02DD: return "Microsoft X-Box One pad";
        case 0x02E3: return "Microsoft X-Box One Elite pad";
        case 0x02EA: return "Microsoft X-Box One S pad";
        case 0x0B12: return "Microsoft Xbox Series S|X Controller";
    }
    return device->product_name[0] ? device->product_name : kind == KIND_360 ? "Generic X-Box pad" : "Generic Xbox One pad";
}
static void release(struct xpad* x) {
    if (x->in.device) { if (!x->device->gone && x->in.configured) { struct usb_pipe* drop[2] = {&x->in, &x->out}; usb_pipes_configure(x->device, NULL, 0, drop, x->out.configured ? 2 : 1); } usb_pipe_close(&x->in); }
    if (x->out.device) usb_pipe_close(&x->out);
    if (x->buffers) arena_free(x->buffers, 4096);
    x->buffers = 0;
}
void xpad_attach(struct usb_device* device) {
    unsigned slot = 0; while (slot < PADS && pads[slot]) slot++;
    if (slot >= PADS) { usb_log_device(device, "too many Xbox controllers"); return; }
    const struct usb_interface_info* f = NULL; unsigned kind = 0;
    for (unsigned i = 0; i < device->interface_count && !f; i++) {
        const struct usb_interface_info* g = &device->interfaces[i];
        if (g->class_code == 0xFF && g->subclass == 0x5D && g->protocol == 0x01) { f = g; kind = KIND_360; }
        else if (g->class_code == 0xFF && g->subclass == 0x47 && g->protocol == 0xD0) { f = g; kind = KIND_ONE; }
    }
    uint8_t in, out, in_interval = 4, out_interval = 4; uint16_t in_packet = 32, out_packet = 32;
    if (!f || !endpoints(device, f->number, &in, &in_packet, &in_interval, &out, &out_packet, &out_interval)) { usb_log_device(device, "Xbox controller: no interrupt endpoints"); return; }
    struct xpad* x = (struct xpad*)arena_alloc(sizeof(struct xpad)); if (!x) return;
    memset(x, 0, sizeof(*x)); x->device = device; x->kind = kind; x->interface = f->number;
    x->buffers = dma32_alloc(4096);
    if (!x->buffers) { arena_free((uint64_t)x, sizeof(struct xpad)); return; }
    memset((void*)x->buffers, 0, 4096);
    x->in = (struct usb_pipe){.owner = x, .complete = in_complete, .address = in, .type = USB_PIPE_INTERRUPT, .interval = in_interval, .max_packet = in_packet};
    x->out = (struct usb_pipe){.owner = x, .complete = out_complete, .address = out, .type = USB_PIPE_INTERRUPT, .interval = out_interval, .max_packet = out_packet};
    struct usb_pipe* add[2] = {&x->in, &x->out};
    if (!usb_pipe_open(device, &x->in) || !usb_pipe_open(device, &x->out) || !usb_pipes_configure(device, add, 2, NULL, 0)) {
        usb_log_device(device, "Xbox controller: endpoint configuration failed"); release(x); arena_free((uint64_t)x, sizeof(struct xpad)); return;
    }
    struct evdev_desc* d = (struct evdev_desc*)arena_alloc(sizeof(struct evdev_desc));
    if (!d) { release(x); arena_free((uint64_t)x, sizeof(struct xpad)); return; }
    memset(d, 0, sizeof(*d)); const char* name = name_of(device, kind);
    unsigned n = 0; for (; name[n] && n < EVDEV_NAME - 1; n++) d->name[n] = name[n]; d->name[n] = 0;
    strcpy(d->phys, "usb-xhci-xpad/input0");
    d->bustype = BUS_USB; d->vendor = device->vendor; d->product = device->product; d->version = kind == KIND_360 ? 0x0114 : 0x0408;
    evdev_set_bit(d->ev, EV_KEY); evdev_set_bit(d->ev, EV_ABS); evdev_set_bit(d->ev, EV_FF); evdev_set_bit(d->ff, FF_RUMBLE); d->ff_max = 16;
    static const uint16_t buttons[11] = {BTN_SOUTH, BTN_EAST, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
    for (unsigned i = 0; i < 11; i++) evdev_set_bit(d->key, buttons[i]);
    static const uint16_t sticks[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
    for (unsigned i = 0; i < 4; i++) { evdev_set_bit(d->abs, sticks[i]); d->absinfo[sticks[i]] = (struct evdev_absinfo){0, -32768, 32767, 16, 128, 0}; }
    unsigned trigger = kind == KIND_360 ? 255 : 1023;
    evdev_set_bit(d->abs, ABS_Z); d->absinfo[ABS_Z] = (struct evdev_absinfo){0, 0, (int)trigger, 0, 0, 0};
    evdev_set_bit(d->abs, ABS_RZ); d->absinfo[ABS_RZ] = (struct evdev_absinfo){0, 0, (int)trigger, 0, 0, 0};
    evdev_set_bit(d->abs, ABS_HAT0X); d->absinfo[ABS_HAT0X] = (struct evdev_absinfo){0, -1, 1, 0, 0, 0};
    evdev_set_bit(d->abs, ABS_HAT0Y); d->absinfo[ABS_HAT0Y] = (struct evdev_absinfo){0, -1, 1, 0, 0, 0};
    x->evdev = evdev_create(d, &ops, x);
    arena_free((uint64_t)d, sizeof(struct evdev_desc));
    if (!x->evdev) { release(x); arena_free((uint64_t)x, sizeof(struct xpad)); return; }
    pads[slot] = x; device->class_driver[2] = x;
    for (unsigned i = 0; i < IN_DEPTH; i++) usb_pipe_queue(&x->in, virt_to_phys(buffer(x, i)), 64, i);
    if (kind == KIND_360) { uint8_t led[3] = {0x01, 0x03, 0x06}; send(x, led, 3); }                     /* player 1, steady */
    else { uint8_t power[5] = {GIP_POWER, GIP_OPT_INTERNAL, x->sequence++, 0x01, 0x00}; send(x, power, 5); }
    usb_log_device(device, kind == KIND_360 ? "game controller: Xbox 360 (XInput)" : "game controller: Xbox One / Series (GIP)");
}
void xpad_detach(struct usb_device* device) {
    struct xpad* x = device->class_driver[2]; if (!x) return;
    x->gone = true; evdev_destroy(x->evdev); x->evdev = NULL;
    release(x);
    for (unsigned i = 0; i < PADS; i++) if (pads[i] == x) pads[i] = NULL;
    device->class_driver[2] = NULL; arena_free((uint64_t)x, sizeof(struct xpad));
}
bool xpad_service(void) {
    bool work = false;
    for (unsigned i = 0; i < PADS; i++) {
        struct xpad* x = pads[i]; if (!x || x->gone) continue;
        if (x->in.halted) {                                              /* stall/transaction error: reset and requeue */
            work = true;
            if (usb_pipe_stop(&x->in)) for (unsigned k = 0; k < IN_DEPTH; k++) usb_pipe_queue(&x->in, virt_to_phys(buffer(x, k)), 64, k);
        }
        if (x->rumble_pending) { x->rumble_pending = false; rumble(x); work = true; }
    }
    return work;
}
void xpad_poll(uint64_t now) { (void)now; }
