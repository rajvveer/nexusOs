/* Bluetooth HCI over USB (Core Vol 4 Part B, like Linux btusb): interface class E0/01/01 (and the vendor devices Linux
 * binds the same way). Commands = class control requests (bmRequestType 0x20, bRequest 0, wLength = length) sent from
 * the USB worker (usbclass_service); events = the interrupt IN endpoint; ACL = bulk IN/OUT. Events and ACL packets may be
 * split over several transfers (or several may share one bulk transfer): both are reassembled as byte streams by their
 * headers, bounded (an event 2 + 255 bytes, ACL 4 + 1021 at most - beyond that the stream resynchronizes at the next
 * transfer). SCO (isochronous interface 1) is not offered: the core never opens SCO links. Unplug unregisters hciN (its
 * links, sockets and mgmt state go). Device state lives with the device: counted allocations (storage_alloc).
 * MediaTek MT79xx (MT7921/MT7922/MT7925: 0e8d:xxxx and partner IDs) need the vendor firmware patch download first
 * (Linux btmtk: WMT commands 0xFC6F with the BT_RAM_CODE_MT79xx patch from linux-firmware, then WMT FUNC_CTRL); that
 * vendor step is NOT implemented (no hardware to verify it): such a controller is registered and the standard init is
 * tried, and the kernel log says why it may fail. See docs/DEVELOPMENT_NOTES.md (Bluetooth) for the exact sequence. */
#include "usb.h"
#include "bt.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#ifndef NEXUS_HOST_TEST
#include "storage_util.h"
#else
uint64_t storage_alloc(uint64_t bytes);
void storage_free(uint64_t address, uint64_t bytes);
#endif

#define BTUSB_DEVICES 4U
#define EVT_DEPTH 4U
#define IN_DEPTH 4U
#define OUT_SLOTS 16U
#define SLOT 1040U
#define BUFFER_BYTES (4096U + 4096U + OUT_SLOTS * SLOT)
struct btusb {
    struct usb_device* device; struct bt_dev* bt; int index; uint32_t gen; bool gone, mediatek;
    struct usb_pipe evt, acl_in, acl_out; uint64_t buffers; uint16_t evt_packet, in_packet;
    uint8_t ev[260]; unsigned ev_have, ev_need; uint8_t acl[1100]; unsigned acl_have, acl_need;
    struct bt_queue commands; uint32_t out_busy; uint64_t dropped;
};
static struct btusb* devices[BTUSB_DEVICES];

static bool mediatek_id(uint16_t vendor, uint16_t product) {
    if (vendor == 0x0E8D) return product == 0x0608 || product == 0x7922 || product == 0x7961 || product == 0x0616 || product == 0x0717 || product == 0x7925;
    static const uint16_t partners[][2] = {{0x13D3, 0x3563}, {0x13D3, 0x3564}, {0x13D3, 0x3567}, {0x13D3, 0x3568}, {0x13D3, 0x3584}, {0x13D3, 0x3585},
                                           {0x0489, 0xE0C8}, {0x0489, 0xE0CD}, {0x0489, 0xE0D8}, {0x0489, 0xE0D9}, {0x0489, 0xE0E0}, {0x0489, 0xE0F2}, {0x04CA, 0x3804}, {0x35F5, 0x7922}};
    for (unsigned i = 0; i < sizeof(partners) / sizeof(partners[0]); i++) if (partners[i][0] == vendor && partners[i][1] == product) return true;
    return false;
}
bool btusb_wanted(const struct usb_device* device);
bool btusb_wanted(const struct usb_device* device) {
    if (mediatek_id(device->vendor, device->product)) return true;
    for (unsigned i = 0; i < device->interface_count; i++) {
        const struct usb_interface_info* f = &device->interfaces[i];
        if (f->class_code == 0xE0 && f->subclass == 0x01 && f->protocol == 0x01) return true;
    }
    return false;
}
static uint8_t* evt_buffer(struct btusb* b, unsigned i) { return (uint8_t*)(b->buffers + 1024ULL * i); }
static uint8_t* in_buffer(struct btusb* b, unsigned i) { return (uint8_t*)(b->buffers + 4096 + 1024ULL * i); }
static uint8_t* out_buffer(struct btusb* b, unsigned i) { return (uint8_t*)(b->buffers + 8192 + (uint64_t)SLOT * i); }
static struct bt_dev* live_bt(struct btusb* b) { return b->bt ? bt_dev_get(b->index, b->gen) : NULL; }

/* ---- controller -> host ---- */
static void deliver(struct btusb* b, uint8_t type, const uint8_t* data, unsigned length) {
    struct bt_dev* d = live_bt(b); if (!d) return;
    uint8_t packet[BT_FRAME_MAX]; if (length + 1 > sizeof(packet)) { b->dropped++; return; }
    packet[0] = type; memcpy(packet + 1, data, length); bt_dev_receive(d, packet, length + 1);
}
static void event_bytes(struct btusb* b, const uint8_t* p, unsigned n) {
    while (n) {
        if (b->ev_have < 2) { b->ev[b->ev_have++] = *p++; n--; if (b->ev_have == 2) b->ev_need = 2U + b->ev[1]; }
        else { unsigned take = b->ev_need - b->ev_have < n ? b->ev_need - b->ev_have : n; memcpy(b->ev + b->ev_have, p, take); b->ev_have += take; p += take; n -= take; }
        if (b->ev_have >= 2 && b->ev_have == b->ev_need) { b->ev_have = 0; deliver(b, PKT_EVENT, b->ev, b->ev_need); }
    }
}
static void acl_bytes(struct btusb* b, const uint8_t* p, unsigned n) {
    while (n) {
        if (b->acl_have < 4) {
            b->acl[b->acl_have++] = *p++; n--;
            if (b->acl_have == 4) { b->acl_need = 4U + bt_get16(b->acl + 2); if (b->acl_need > sizeof(b->acl)) { b->dropped++; b->acl_have = 0; return; } }   /* resync at the next transfer */
        } else { unsigned take = b->acl_need - b->acl_have < n ? b->acl_need - b->acl_have : n; memcpy(b->acl + b->acl_have, p, take); b->acl_have += take; p += take; n -= take; }
        if (b->acl_have >= 4 && b->acl_have == b->acl_need) { b->acl_have = 0; deliver(b, PKT_ACL, b->acl, b->acl_need); }
    }
}
static void evt_complete(struct usb_pipe* pipe, uint32_t tag, unsigned actual, unsigned code) {
    struct btusb* b = pipe->owner;
    if (b->gone || code == 26) return;
    if ((code == 1 || code == 13) && actual <= b->evt_packet) event_bytes(b, evt_buffer(b, tag), actual);
    else if (code != 1 && code != 13) b->ev_have = 0;
    if (!pipe->halted) usb_pipe_queue(pipe, virt_to_phys(evt_buffer(b, tag)), b->evt_packet, tag); else usb_worker_kick();
}
static void in_complete(struct usb_pipe* pipe, uint32_t tag, unsigned actual, unsigned code) {
    struct btusb* b = pipe->owner;
    if (b->gone || code == 26) return;
    if ((code == 1 || code == 13) && actual <= 1024) acl_bytes(b, in_buffer(b, tag), actual);
    else if (code != 1 && code != 13) b->acl_have = 0;
    if (!pipe->halted) usb_pipe_queue(pipe, virt_to_phys(in_buffer(b, tag)), 1024, tag); else usb_worker_kick();
}
static void out_complete(struct usb_pipe* pipe, uint32_t tag, unsigned actual, unsigned code) {
    struct btusb* b = pipe->owner; (void)actual; (void)code;
    if (tag < OUT_SLOTS) b->out_busy &= ~(1U << tag);   /* the core sends more on the next credit (Number Of Completed Packets) */
}

/* ---- host -> controller ---- */
static int btusb_send(struct bt_dev* d, const uint8_t* packet, unsigned length) {
    struct btusb* b = d->driver_data;
    if (b->gone) return E_NODEV;
    if (length < 2) return E_INVAL;
    if (packet[0] == PKT_COMMAND) {
        if (b->commands.count >= 32 || length - 1 > 258) return E_AGAIN;
        struct bt_pkt* p = bt_pkt_new(length - 1); if (!p) return E_NOMEM;
        memcpy(p->data, packet + 1, length - 1); bt_queue_put(&b->commands, p); usb_worker_kick(); return 0;
    }
    if (packet[0] != PKT_ACL || length - 1 > SLOT || !b->acl_out.configured) return E_OPNOTSUPP;   /* SCO/ISO: not offered */
    for (unsigned i = 0; i < OUT_SLOTS; i++) if (!(b->out_busy & (1U << i))) {
        memcpy(out_buffer(b, i), packet + 1, length - 1); b->out_busy |= 1U << i;
        int r = usb_pipe_queue(&b->acl_out, virt_to_phys(out_buffer(b, i)), length - 1, i);
        if (r < 0) { b->out_busy &= ~(1U << i); return r; }
        return 0;
    }
    return E_AGAIN;
}
static bool btusb_can_send(struct bt_dev* d, unsigned type) {
    struct btusb* b = d->driver_data;
    if (type == PKT_COMMAND) return b->commands.count < 32;
    return b->out_busy != (1U << OUT_SLOTS) - 1;
}
static int btusb_setup(struct bt_dev* d) {
    struct btusb* b = d->driver_data;
    if (b->mediatek) vga_print("[64] BLUETOOTH btusb: MediaTek MT79xx: the btmtk firmware patch download (WMT 0xFC6F) is not implemented; trying the standard HCI init\n");
    return 0;
}
static const struct bt_driver driver = {"btusb", 1, btusb_send, btusb_can_send, btusb_setup};

/* ---- binding ---- */
static bool endpoints(struct usb_device* device, unsigned number, uint8_t* evt, uint16_t* evt_packet, uint8_t* evt_interval, uint8_t* in, uint16_t* in_packet, uint8_t* out, uint16_t* out_packet) {
    unsigned length; const uint8_t* c = usb_config(device, &length); bool inside = false; *evt = *in = *out = 0;
    for (unsigned at = 0; c && at + 2 <= length && c[at] >= 2 && at + c[at] <= length; at += c[at]) {
        if (c[at + 1] == 4 && c[at] >= 9) inside = c[at + 2] == number && c[at + 3] == 0;
        else if (inside && c[at + 1] == 5 && c[at] >= 7) {
            uint8_t address = c[at + 2], kind = c[at + 3] & 3; uint16_t packet = (uint16_t)((c[at + 4] | c[at + 5] << 8) & 0x7FF);
            if (kind == 3 && (address & 0x80) && !*evt) { *evt = address; *evt_packet = packet; *evt_interval = c[at + 6]; }
            else if (kind == 2 && (address & 0x80) && !*in) { *in = address; *in_packet = packet; }
            else if (kind == 2 && !(address & 0x80) && !*out) { *out = address; *out_packet = packet; }
        }
    }
    return *evt && *in && *out && *evt_packet && *in_packet && *out_packet;
}
static void release(struct btusb* b) {
    struct usb_pipe* all[3] = {&b->evt, &b->acl_in, &b->acl_out}; struct usb_pipe* drop[3]; unsigned n = 0;
    for (unsigned i = 0; i < 3; i++) if (all[i]->device && all[i]->configured) drop[n++] = all[i];
    if (n && !b->device->gone) usb_pipes_configure(b->device, NULL, 0, drop, n);
    for (unsigned i = 0; i < 3; i++) if (all[i]->device) usb_pipe_close(all[i]);
    if (b->buffers) arena_free(b->buffers, BUFFER_BYTES);
    b->buffers = 0; bt_queue_purge(&b->commands);
}
void btusb_attach(struct usb_device* device);
void btusb_attach(struct usb_device* device) {
    unsigned slot = 0; while (slot < BTUSB_DEVICES && devices[slot]) slot++;
    if (slot == BTUSB_DEVICES) { usb_log_device(device, "too many Bluetooth controllers"); return; }
    unsigned number = 0; bool found = false;
    for (unsigned i = 0; i < device->interface_count && !found; i++) {
        const struct usb_interface_info* f = &device->interfaces[i];
        if ((f->class_code == 0xE0 && f->subclass == 0x01 && f->protocol == 0x01) || (mediatek_id(device->vendor, device->product) && f->number == 0)) { number = f->number; found = true; }
    }
    uint8_t evt, in, out, interval = 1; uint16_t evt_packet = 16, in_packet = 64, out_packet = 64;
    if (!found || !endpoints(device, number, &evt, &evt_packet, &interval, &in, &in_packet, &out, &out_packet)) { usb_log_device(device, "Bluetooth: no event/ACL endpoints"); return; }
    if (evt_packet > 1024) evt_packet = 1024;
    struct btusb* b = (struct btusb*)storage_alloc(sizeof(struct btusb)); if (!b) return;
    memset(b, 0, sizeof(*b)); b->device = device; b->mediatek = mediatek_id(device->vendor, device->product); b->evt_packet = evt_packet; b->in_packet = in_packet;
    b->buffers = dma32_alloc(BUFFER_BYTES);
    if (!b->buffers) { storage_free((uint64_t)b, sizeof(struct btusb)); return; }
    memset((void*)b->buffers, 0, BUFFER_BYTES);
    b->evt = (struct usb_pipe){.owner = b, .complete = evt_complete, .address = evt, .type = USB_PIPE_INTERRUPT, .interval = interval, .max_packet = evt_packet};
    b->acl_in = (struct usb_pipe){.owner = b, .complete = in_complete, .address = in, .type = USB_PIPE_BULK, .max_packet = in_packet};
    b->acl_out = (struct usb_pipe){.owner = b, .complete = out_complete, .address = out, .type = USB_PIPE_BULK, .max_packet = out_packet};
    struct usb_pipe* add[3] = {&b->evt, &b->acl_in, &b->acl_out};
    if (!usb_pipe_open(device, &b->evt) || !usb_pipe_open(device, &b->acl_in) || !usb_pipe_open(device, &b->acl_out) || !usb_pipes_configure(device, add, 3, NULL, 0)) {
        usb_log_device(device, "Bluetooth: endpoint configuration failed"); release(b); storage_free((uint64_t)b, sizeof(struct btusb)); return;
    }
    for (unsigned i = 0; i < EVT_DEPTH; i++) usb_pipe_queue(&b->evt, virt_to_phys(evt_buffer(b, i)), evt_packet, i);
    for (unsigned i = 0; i < IN_DEPTH; i++) usb_pipe_queue(&b->acl_in, virt_to_phys(in_buffer(b, i)), 1024, i);
    devices[slot] = b; device->class_driver[3] = b;
    b->bt = bt_dev_register(&driver, b, b->mediatek ? "usb, MediaTek" : "usb");
    if (!b->bt) { usb_log_device(device, "Bluetooth: no controller slot"); devices[slot] = NULL; device->class_driver[3] = NULL; release(b); storage_free((uint64_t)b, sizeof(struct btusb)); return; }
    b->index = b->bt->index; b->gen = b->bt->generation;
    usb_log_device(device, b->mediatek ? "Bluetooth controller (MediaTek)" : "Bluetooth controller");
}
void btusb_detach(struct usb_device* device);
void btusb_detach(struct usb_device* device) {
    struct btusb* b = device->class_driver[3]; if (!b) return;
    b->gone = true;
    struct bt_dev* d = live_bt(b); if (d) bt_dev_unregister(d);
    b->bt = NULL; release(b);
    for (unsigned i = 0; i < BTUSB_DEVICES; i++) if (devices[i] == b) devices[i] = NULL;
    device->class_driver[3] = NULL; storage_free((uint64_t)b, sizeof(struct btusb));
}
/* Worker: queued commands as control transfers; halted pipes reset and refilled. */
bool btusb_service(void);
bool btusb_service(void) {
    bool work = false;
    for (unsigned i = 0; i < BTUSB_DEVICES; i++) {
        struct btusb* b = devices[i]; if (!b || b->gone) continue;
        struct bt_pkt* p;
        while ((p = bt_queue_take(&b->commands))) {
            work = true;
            int r = usb_control(b->device, 0x20, 0x00, 0, 0, p->data, (uint16_t)p->len);
            bt_pkt_free(p);
            if (r < 0) { b->dropped++; struct bt_dev* d = live_bt(b); if (d) d->stat[1]++; }
            if (!(b = devices[i]) || b->gone) break;
        }
        if (!b || b->gone) continue;
        if (b->evt.halted) { work = true; if (usb_pipe_stop(&b->evt)) for (unsigned k = 0; k < EVT_DEPTH; k++) usb_pipe_queue(&b->evt, virt_to_phys(evt_buffer(b, k)), b->evt_packet, k); b->ev_have = 0; }
        if (b->acl_in.halted) { work = true; if (usb_pipe_stop(&b->acl_in)) for (unsigned k = 0; k < IN_DEPTH; k++) usb_pipe_queue(&b->acl_in, virt_to_phys(in_buffer(b, k)), 1024, k); b->acl_have = 0; }
    }
    return work;
}
