/* Host simulator for arch/x86_64/btusb.c (QEMU has no USB Bluetooth controller model since 5.0): a simulated dongle
 * (interface E0/01/01: interrupt IN 0x81 with 16-byte packets, bulk IN 0x82 / OUT 0x02 with 64-byte packets) and a
 * recording stub of the Bluetooth core (bt_dev_register/receive/unregister). Checks: commands become class control
 * transfers from the worker (bmRequestType 0x20), events split over 16-byte interrupt packets are reassembled, several
 * ACL packets in one bulk transfer and one ACL packet over several transfers are separated/reassembled, ACL out loses
 * its H:4 byte, flow control (16 out slots), hostile input (oversized ACL length, error completions mid-packet, 20000
 * random fuzz streams), halted pipes reset by the service, MediaTek IDs bound, unplug, and that all memory comes back.
 * Run by tests/test_usbclass_sim.py (btusb). */
#define NEXUS_HOST_TEST 1
#include "../arch/x86_64/usb.h"
#include "../arch/x86_64/bt.h"
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
uint64_t storage_alloc(uint64_t bytes) { return arena_alloc(bytes); }
void storage_free(uint64_t address, uint64_t bytes) { arena_free(address, bytes); }
static bool quiet;
void vga_print(const char* text) { if (!quiet) fputs(text, stdout); }
void usb_log_device(const struct usb_device* device, const char* what) { (void)device; if (!quiet) printf("[usb] %s\n", what); }
static unsigned kicks;
void usb_worker_kick(void) { kicks++; }

/* ---------------- recording core stub ---------------- */
static struct bt_dev core; static bool registered; static const struct bt_driver* drv;
static uint8_t received[64][1100]; static unsigned received_length[64], received_count;
struct bt_dev* bt_dev_register(const struct bt_driver* driver, void* data, const char* why) {
    (void)why; assert(!registered); memset(&core, 0, sizeof(core)); core.index = 0; core.generation = 7; core.alive = true; core.driver = driver; core.driver_data = data;
    registered = true; drv = driver; return &core;
}
struct bt_dev* bt_dev_get(int index, uint32_t generation) { return registered && index == 0 && generation == 7 ? &core : NULL; }
void bt_dev_unregister(struct bt_dev* d) { assert(d == &core && registered); registered = false; }
void bt_dev_receive(struct bt_dev* d, const uint8_t* packet, unsigned length) {
    assert(d == &core && length >= 1 && length <= 1100);
    if (packet[0] == PKT_EVENT) assert(length == 3U + packet[2]);
    if (packet[0] == PKT_ACL) assert(length >= 5 && length == 5U + bt_get16(packet + 3));
    memcpy(received[received_count % 64], packet, length); received_length[received_count++ % 64] = length;
}
struct bt_pkt* bt_pkt_new(unsigned len) { uint64_t bytes = sizeof(struct bt_pkt) + len; struct bt_pkt* p = (void*)(uintptr_t)arena_alloc(bytes); p->len = len; p->alloc = (uint32_t)bytes; return p; }
void bt_pkt_free(struct bt_pkt* p) { if (p) arena_free((uint64_t)(uintptr_t)p, p->alloc); }
void bt_queue_put(struct bt_queue* q, struct bt_pkt* p) { p->next = NULL; if (q->tail) q->tail->next = p; else q->head = p; q->tail = p; q->count++; q->bytes += p->len; }
struct bt_pkt* bt_queue_take(struct bt_queue* q) { struct bt_pkt* p = q->head; if (!p) return NULL; q->head = p->next; if (!q->head) q->tail = NULL; q->count--; q->bytes -= p->len; return p; }
void bt_queue_purge(struct bt_queue* q) { struct bt_pkt* p; while ((p = bt_queue_take(q))) bt_pkt_free(p); }

/* ---------------- simulated device ---------------- */
static struct usb_device dev;
static uint8_t config[256]; static unsigned config_length;
const uint8_t* usb_config(const struct usb_device* device, unsigned* length) { (void)device; *length = config_length; return config; }
static uint8_t controls[64][300]; static unsigned control_length[64], control_count; static int control_fail;
int usb_control(struct usb_device* device, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length) {
    assert(device == &dev && type == 0x20 && request == 0 && value == 0 && index == 0 && length <= 258);
    if (control_fail) return -32;
    memcpy(controls[control_count % 64], data, length); control_length[control_count++ % 64] = length; return length;
}
struct td { uint64_t physical; uint32_t length, tag; };
static struct { struct usb_pipe* pipe; struct td tds[64]; unsigned head, tail; bool open; uint8_t sent[64][1100]; unsigned sent_length[64], sent_count; bool hold; } pipes[3];
static int find_pipe(struct usb_pipe* p) { for (int i = 0; i < 3; i++) if (pipes[i].open && pipes[i].pipe == p) return i; return -1; }
bool usb_pipe_open(struct usb_device* device, struct usb_pipe* pipe) {
    pipe->device = device; pipe->in = (pipe->address & 0x80) != 0;
    for (int i = 0; i < 3; i++) if (!pipes[i].open) { memset(&pipes[i], 0, sizeof(pipes[i])); pipes[i].pipe = pipe; pipes[i].open = true; pipe->ring = 1; return true; }
    return false;
}
void usb_pipe_close(struct usb_pipe* pipe) { int i = find_pipe(pipe); assert(i >= 0); pipes[i].open = false; pipe->device = NULL; pipe->configured = false; }
bool usb_pipes_configure(struct usb_device* device, struct usb_pipe* const* add, unsigned adds, struct usb_pipe* const* drop, unsigned drops) {
    (void)device;
    for (unsigned i = 0; i < drops; i++) { int k = find_pipe(drop[i]); if (k >= 0) while (pipes[k].head != pipes[k].tail) { struct td t = pipes[k].tds[pipes[k].head++ % 64]; drop[i]->queued--; drop[i]->complete(drop[i], t.tag, 0, 26); } drop[i]->configured = false; }
    for (unsigned i = 0; i < adds; i++) { assert(find_pipe(add[i]) >= 0); add[i]->configured = true; add[i]->queued = 0; add[i]->halted = false; }
    return true;
}
int usb_pipe_queue(struct usb_pipe* pipe, uint64_t physical, uint32_t length, uint32_t tag) {
    int k = find_pipe(pipe); assert(k >= 0);
    if (dev.gone) return -19;
    if (!pipe->configured || pipe->halted) return -32;
    if (pipes[k].tail - pipes[k].head >= 32) return -16;
    if (!pipe->in) {
        assert(length <= 1100); memcpy(pipes[k].sent[pipes[k].sent_count % 64], (const void*)(uintptr_t)physical, length); pipes[k].sent_length[pipes[k].sent_count++ % 64] = length;
        if (pipes[k].hold) { pipes[k].tds[pipes[k].tail++ % 64] = (struct td){physical, length, tag}; pipe->queued++; return 0; }
        pipe->complete(pipe, tag, length, 1); return 0;
    }
    pipes[k].tds[pipes[k].tail++ % 64] = (struct td){physical, length, tag}; pipe->queued++;
    return 0;
}
bool usb_pipe_stop(struct usb_pipe* pipe) {
    int k = find_pipe(pipe); assert(k >= 0);
    while (pipes[k].head != pipes[k].tail) { struct td t = pipes[k].tds[pipes[k].head++ % 64]; pipe->queued--; pipe->complete(pipe, t.tag, 0, 26); }
    pipe->halted = false; return true;
}
static int pipe_of(uint8_t address) { for (int i = 0; i < 3; i++) if (pipes[i].open && pipes[i].pipe->address == address) return i; return -1; }
static bool transfer_in(uint8_t address, const uint8_t* data, unsigned length, unsigned code) {   /* the device completes the oldest IN TD */
    int k = pipe_of(address); if (k < 0 || pipes[k].head == pipes[k].tail) return false;
    struct usb_pipe* pipe = pipes[k].pipe; struct td t = pipes[k].tds[pipes[k].head++ % 64]; pipe->queued--;
    assert(length <= t.length);
    if (data) memcpy((void*)(uintptr_t)t.physical, data, length);
    if (code != 1 && code != 13) pipe->halted = true;
    pipe->complete(pipe, t.tag, code == 1 || code == 13 ? length : 0, code);
    return true;
}
static void release_out(void) { int k = pipe_of(0x02); while (pipes[k].head != pipes[k].tail) { struct td t = pipes[k].tds[pipes[k].head++ % 64]; pipes[k].pipe->queued--; pipes[k].pipe->complete(pipes[k].pipe, t.tag, t.length, 1); } }
static void event_in(const uint8_t* e, unsigned n) { for (unsigned at = 0; at < n; at += 16) assert(transfer_in(0x81, e + at, n - at < 16 ? n - at : 16, n - at < 16 ? 13 : 1)); }

#include "../arch/x86_64/btusb.c"

static void build_config(void) {
    static const uint8_t c[] = {9, 2, 0, 0, 2, 1, 0, 0xE0, 50,
        9, 4, 0, 0, 3, 0xE0, 0x01, 0x01, 0,
        7, 5, 0x81, 3, 16, 0, 1, 7, 5, 0x82, 2, 64, 0, 0, 7, 5, 0x02, 2, 64, 0, 0,
        9, 4, 1, 0, 2, 0xE0, 0x01, 0x01, 0, 9, 4, 1, 1, 2, 0xE0, 0x01, 0x01, 0, 7, 5, 0x83, 1, 9, 0, 1, 7, 5, 0x03, 1, 9, 0, 1};
    memcpy(config, c, sizeof(c)); config_length = sizeof(c); config[2] = (uint8_t)sizeof(c);
}
static void plug(uint16_t vendor, uint16_t product) {
    memset(&dev, 0, sizeof(dev)); dev.vendor = vendor; dev.product = product; dev.interface_count = 2;
    dev.interfaces[0] = (struct usb_interface_info){.number = 0, .class_code = 0xE0, .subclass = 0x01, .protocol = 0x01};
    dev.interfaces[1] = (struct usb_interface_info){.number = 1, .class_code = 0xE0, .subclass = 0x01, .protocol = 0x01};
    build_config(); received_count = 0; control_count = 0;
    assert(btusb_wanted(&dev)); btusb_attach(&dev); assert(registered && dev.class_driver[3]);
}
static void unplug(void) { dev.gone = true; btusb_detach(&dev); assert(!registered && !dev.class_driver[3]); for (int i = 0; i < 3; i++) assert(!pipes[i].open); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint64_t base_allocations = allocations;
    plug(0x8087, 0x0A2B);
    /* commands: queued by send, sent by the worker as class requests */
    static const uint8_t reset[4] = {PKT_COMMAND, 0x03, 0x0C, 0x00}, name[8] = {PKT_COMMAND, 0x14, 0x0C, 0x00};
    assert(drv->send(&core, reset, 4) == 0 && drv->send(&core, name, 4) == 0 && kicks);
    assert(btusb_service() && control_count == 2 && control_length[0] == 3 && !memcmp(controls[0], reset + 1, 3) && !memcmp(controls[1], name + 1, 3));
    /* a 255-byte event (Read Local Name complete) over 16-byte interrupt packets */
    uint8_t e[258]; e[0] = 0x0E; e[1] = 252; e[2] = 1; e[3] = 0x14; e[4] = 0x0C; e[5] = 0; for (unsigned i = 6; i < 254; i++) e[i] = (uint8_t)('A' + i % 26);
    event_in(e, 254);
    assert(received_count == 1 && received_length[0] == 255 && received[0][0] == PKT_EVENT && !memcmp(received[0] + 1, e, 254));
    /* ACL: two packets in one bulk transfer, then one packet over three transfers */
    uint8_t bulk[1024]; unsigned n = 0;
    for (unsigned p = 0; p < 2; p++) { bulk[n] = 0x01; bulk[n + 1] = 0x20; bulk[n + 2] = 10; bulk[n + 3] = 0; for (unsigned i = 0; i < 10; i++) bulk[n + 4 + i] = (uint8_t)(p * 50 + i); n += 14; }
    assert(transfer_in(0x82, bulk, n, 13) && received_count == 3 && received_length[1] == 15 && received[2][5] == 50);
    uint8_t big[4 + 600]; big[0] = 0x01; big[1] = 0x20; big[2] = (uint8_t)600; big[3] = (uint8_t)(600 >> 8); for (unsigned i = 0; i < 600; i++) big[4 + i] = (uint8_t)i;
    assert(transfer_in(0x82, big, 3, 13) && transfer_in(0x82, big + 3, 300, 13) && received_count == 3 && transfer_in(0x82, big + 303, 301, 13));
    assert(received_count == 4 && received_length[3] == 605 && !memcmp(received[3] + 1, big, 604));
    /* ACL out: the H:4 type byte is not sent; 16 slots, then EAGAIN until completions */
    uint8_t out[1 + 4 + 27] = {PKT_ACL, 0x01, 0x00, 27, 0}; for (unsigned i = 0; i < 27; i++) out[5 + i] = (uint8_t)(0xA0 + i);
    assert(drv->send(&core, out, sizeof(out)) == 0); int ko = pipe_of(0x02);
    assert(pipes[ko].sent_count == 1 && pipes[ko].sent_length[0] == 31 && !memcmp(pipes[ko].sent[0], out + 1, 31));
    pipes[ko].hold = true; unsigned accepted = 0; while (drv->send(&core, out, sizeof(out)) == 0) accepted++;
    assert(accepted == 16 && !drv->can_send(&core, PKT_ACL)); release_out(); pipes[ko].hold = false;
    assert(drv->can_send(&core, PKT_ACL) && drv->send(&core, out, sizeof(out)) == 0);
    uint8_t sco[4] = {PKT_SCO, 0, 0, 0}; assert(drv->send(&core, sco, 4) < 0);
    /* hostile: an ACL length beyond the buffer resynchronizes; an error completion mid-event resets the event stream */
    uint8_t evil[8] = {0x01, 0x20, 0xFF, 0x0F, 1, 2, 3, 4}; unsigned before = received_count;
    assert(transfer_in(0x82, evil, 8, 13) && received_count == before);
    assert(transfer_in(0x82, bulk, 14, 13) && received_count == before + 1);       /* the next transfer is clean again */
    assert(transfer_in(0x81, e, 16, 1) && transfer_in(0x81, NULL, 0, 4));           /* transaction error: halted */
    assert(btusb_service());                                                           /* reset + refilled */
    uint8_t ok[6] = {0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00}; before = received_count;
    event_in(ok, 6); assert(received_count == before + 1 && received_length[(before) % 64] == 7);
    control_fail = 1; assert(drv->send(&core, reset, 4) == 0 && btusb_service() && core.stat[1] == 1); control_fail = 0;
    quiet = true; srand(7); unsigned packets = received_count;
    for (unsigned round = 0; round < 20000; round++) {                                 /* fuzz: random streams on both IN pipes */
        uint8_t r[64]; unsigned len = (unsigned)rand() % 65; for (unsigned i = 0; i < len; i++) r[i] = (uint8_t)rand();
        if (rand() & 1) { if (len > 16) len = 16; transfer_in(0x81, r, len, (rand() % 50) ? 1 : 4); }
        else transfer_in(0x82, r, len, (rand() % 50) ? 13 : 6);
        if (round % 97 == 0) btusb_service();
    }
    btusb_service(); quiet = false;
    printf("fuzz: 20000 random transfers, %u packets reassembled, %llu dropped\n", received_count - packets, (unsigned long long)devices[0]->dropped);
    unplug();
    /* MediaTek MT7922 (0e8d:0608) binds by ID and logs the missing vendor firmware step at setup */
    plug(0x0E8D, 0x0608); assert(devices[0]->mediatek && drv->setup && drv->setup(&core) == 0); unplug();
    plug(0x13D3, 0x3568); assert(devices[0]->mediatek); unplug();
    memset(&dev, 0, sizeof(dev)); dev.vendor = 0x1234; dev.product = 0x5678; dev.interface_count = 1; dev.interfaces[0] = (struct usb_interface_info){.class_code = 0x03}; assert(!btusb_wanted(&dev));
    assert(allocations == base_allocations && allocated == 0);
    printf("BTUSB SIMULATOR TESTS PASSED\n");
    return 0;
}
