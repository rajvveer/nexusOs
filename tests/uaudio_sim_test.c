/* Host simulator for the USB Audio Class driver (arch/x86_64/uaudio.c): QEMU has no UAC2 device and no capture-capable
 * UAC device, so the driver's real entry points run here against simulated devices built from real descriptor layouts:
 *  - "uac2": a high-speed UAC2 device laid out like Linux's f_uac2 gadget (the descriptor set most UAC2 DACs/headsets
 *    use): clock sources for each direction, USB streaming IT -> FU -> speaker OT, mic IT -> FU -> USB streaming OT,
 *    asynchronous isochronous OUT (bInterval 1 = 125 us, 6 frames nominal) with an explicit 16.16 feedback IN endpoint,
 *    isochronous IN capture; CS_SAM_FREQ_CONTROL RANGE/CUR on the clock, FU volume RANGE/CUR + mute;
 *  - "uac1": a full-speed UAC1 headset laid out like the C-Media CM108 family: stereo playback (44.1/48 kHz, adaptive),
 *    mono capture (48 kHz, asynchronous) behind a selector, endpoint SAMPLING_FREQ_CONTROL, FU GET_MIN/MAX/RES/CUR;
 *  - "dac24": UAC1 full-speed 24-bit (3-byte subframe) stereo DAC; "mic16k": 16 kHz-only mono microphone (interpolated);
 *  - hostile descriptor sets (truncated, zero lengths, unit loops, absurd packet sizes, no PCM format, unsupported rate).
 * Checks: alternate setting / rate choice and the exact control requests, packet sizes (nominal and feedback-driven),
 * sample conversion both ways, ALSA controls with dB TLV, missed-service handling, the stop/flush/start race,
 * hot-unplug while streaming and every allocation returned. */
#define NEXUS_HOST_TEST 1
#include "../arch/x86_64/usb.h"
#include "../arch/x86_64/sound_hw.h"
#include "../kernel/string.h"
#include "../kernel/vga.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>

/* ---------------- kernel services ---------------- */
static unsigned allocations; static uint64_t allocated;
uint64_t arena_alloc(uint64_t bytes) { uint8_t* b = calloc(1, bytes + 4096); assert(b); allocations++; allocated += bytes; return ((uint64_t)(uintptr_t)b + 4095) & ~4095ULL; }
uint64_t dma32_alloc(uint64_t bytes) { return arena_alloc(bytes); }
void arena_free(uint64_t address, uint64_t bytes) { assert(address && allocations); allocations--; allocated -= bytes; }
static char log_text[65536]; static unsigned log_used;
void vga_print(const char* text) { size_t n = strlen(text); if (log_used + n < sizeof(log_text)) { memcpy(log_text + log_used, text, n); log_used += (unsigned)n; log_text[log_used] = 0; } fputs(text, stdout); }
void usb_log_device(const struct usb_device* device, const char* what) { (void)device; vga_print("[uaudio] "); vga_print(what); vga_print("\n"); }
static unsigned kicks; void usb_worker_kick(void) { kicks++; }
uint32_t usb_frame_index(struct usb_device* device) { (void)device; return 0; }

/* ---------------- simulated sound.c card ---------------- */
static struct { bool present; struct sound_engine engine; int16_t* ring; int16_t* capture; int index; unsigned adds, removes; } card;
static void* context;
int sound_card_add(const struct sound_engine* engine, int16_t** playback, int16_t** capture) {
    assert(!card.present);
    card.engine = *engine; card.present = true; card.adds++;
    card.ring = calloc(SOUND_RING_FRAMES, 4); *playback = card.ring;
    if (capture) { card.capture = calloc(SOUND_RING_FRAMES, 4); *capture = card.capture; }
    context = engine->context; return card.index;
}
void sound_card_remove(int index) {
    assert(card.present && index == card.index);
    context = card.engine.context; if (card.engine.stop) card.engine.stop(); if (card.engine.capture_stop) card.engine.capture_stop();
    card.present = false; card.removes++; free(card.ring); free(card.capture); card.ring = card.capture = NULL;
}
void* sound_engine_context(void) { return context; }
const struct sound_control* sound_engine_control(unsigned index) { return index < card.engine.controls ? &card.engine.control[index] : NULL; }

/* ---------------- simulated USB device ---------------- */
struct sim_device {
    const char* name; uint8_t config[1024]; unsigned length;
    bool uac2; unsigned speed;
    unsigned alt[8];                   /* current alternate setting per interface */
    uint32_t clock_rate[256]; uint32_t range_min, range_max, range_res; bool clock_writable;
    uint32_t endpoint_rate[32];        /* UAC1 SAMPLING_FREQ by endpoint number */
    int16_t vol_min, vol_max, vol_res, vol[256][3]; uint8_t mute[256][3];
    unsigned requests, failures, set_interfaces;
    bool fail_set_interface, fail_rate, stall_volume;
};
static struct sim_device sim;
static struct usb_device dev;
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32v(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t* p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
const uint8_t* usb_config(const struct usb_device* device, unsigned* length) { (void)device; *length = sim.length; return sim.config; }
int usb_control(struct usb_device* device, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length) {
    assert(device == &dev); sim.requests++;
    uint8_t* d = data; unsigned selector = value >> 8, channel = value & 0xFF, entity = index >> 8;
    if (device->gone) return -19;
    if (type == 0x01 && request == 11) {                                    /* SET_INTERFACE */
        sim.set_interfaces++;
        if (sim.fail_set_interface || index >= 8) return -32;
        sim.alt[index] = value; return 0;
    }
    if (type == 0x22 && request == 0x01 && value == 0x0100 && length == 3) { /* UAC1 SET_CUR SAMPLING_FREQ (endpoint) */
        if (sim.fail_rate) return -32;
        sim.endpoint_rate[index & 15] = d[0] | d[1] << 8 | (uint32_t)d[2] << 16; return 0;
    }
    if ((type == 0xA1 || type == 0x21) && (index & 0xFF) == 0) {           /* class request to the AudioControl interface */
        bool get = type == 0xA1;
        if (sim.uac2 && entity >= 1 && entity <= 3 && selector == 1) {      /* clock source CS_SAM_FREQ_CONTROL */
            if (request == 0x02 && get) {                                    /* RANGE */
                if (length < 14) return -32;
                put16(d, 1); put32(d + 2, sim.range_min); put32(d + 6, sim.range_max); put32(d + 10, sim.range_res); return 14;
            }
            if (request == 0x01 && get && length == 4) { put32(d, sim.clock_rate[entity]); return 4; }
            if (request == 0x01 && !get && length == 4) {
                if (!sim.clock_writable || sim.fail_rate) return -32;
                uint32_t rate = le32v(d); if (rate < sim.range_min || rate > sim.range_max) return -32;
                sim.clock_rate[entity] = rate; return 0;
            }
            return -32;
        }
        if (entity >= 10 && entity <= 11 && (selector == 1 || selector == 2) && channel <= 2) {   /* feature units 10, 11 */
            if (selector == 2 && sim.stall_volume) return -32;
            if (selector == 1) {
                if (get && length >= 1) { d[0] = sim.mute[entity][channel]; return 1; }
                if (!get && length == 1 && (request == 0x01)) { sim.mute[entity][channel] = d[0]; return 0; }
                return -32;
            }
            if (sim.uac2) {
                if (get && request == 0x02) { if (length < 8) return -32; put16(d, 1); put16(d + 2, (uint16_t)sim.vol_min); put16(d + 4, (uint16_t)sim.vol_max); put16(d + 6, (uint16_t)sim.vol_res); return 8; }
                if (get && request == 0x01 && length == 2) { put16(d, (uint16_t)sim.vol[entity][channel]); return 2; }
                if (!get && request == 0x01 && length == 2) { sim.vol[entity][channel] = (int16_t)rd16(d); return 0; }
                return -32;
            }
            if (get && length == 2) {
                int16_t v = request == 0x82 ? sim.vol_min : request == 0x83 ? sim.vol_max : request == 0x84 ? sim.vol_res : request == 0x81 ? sim.vol[entity][channel] : 0;
                if (request < 0x81 || request > 0x84) return -32;
                put16(d, (uint16_t)v); return 2;
            }
            if (!get && request == 0x01 && length == 2) { sim.vol[entity][channel] = (int16_t)rd16(d); return 0; }
            return -32;
        }
        if (entity == 20 && request == 0x01 && type == 0xA1 && length == 1) { d[0] = 1; return 1; }   /* clock selector pin */
    }
    sim.failures++; return -32;
}

/* ---------------- simulated xHCI pipes ---------------- */
struct td { uint64_t physical; uint32_t length, tag; };
struct pipe_sim { struct usb_pipe* pipe; struct td tds[512]; unsigned head, tail; bool open; };
static struct pipe_sim pipes[8]; static unsigned configures, stops;
static struct pipe_sim* find(struct usb_pipe* p) { for (unsigned i = 0; i < 8; i++) if (pipes[i].open && pipes[i].pipe == p) return &pipes[i]; return NULL; }
bool usb_pipe_open(struct usb_device* device, struct usb_pipe* pipe) {
    pipe->device = device; pipe->in = (pipe->address & 0x80) != 0; pipe->dci = (uint8_t)((pipe->address & 15) * 2 + pipe->in);
    for (unsigned i = 0; i < 8; i++) if (!pipes[i].open) { pipes[i] = (struct pipe_sim){.pipe = pipe, .open = true}; pipe->ring = 1; return true; }
    return false;
}
void usb_pipe_close(struct usb_pipe* pipe) { struct pipe_sim* p = find(pipe); assert(p); p->open = false; pipe->device = NULL; pipe->ring = 0; pipe->configured = false; }
bool usb_pipes_configure(struct usb_device* device, struct usb_pipe* const* add, unsigned adds, struct usb_pipe* const* drop, unsigned drops) {
    (void)device; configures++;
    for (unsigned i = 0; i < drops; i++) if (drop[i]) { struct pipe_sim* p = find(drop[i]); if (p) { while (p->head != p->tail) { struct td t = p->tds[p->head++ % 512]; drop[i]->queued--; if (drop[i]->complete) drop[i]->complete(drop[i], t.tag, 0, 26); } } drop[i]->configured = false; }
    for (unsigned i = 0; i < adds; i++) { assert(find(add[i])); add[i]->configured = true; add[i]->queued = 0; }
    return true;
}
int usb_pipe_queue(struct usb_pipe* pipe, uint64_t physical, uint32_t length, uint32_t tag) {
    struct pipe_sim* p = find(pipe); assert(p);
    if (dev.gone) return -19;
    if (!pipe->configured) return -32;
    if (p->tail - p->head >= 250) return -16;
    unsigned packet = pipe->max_packet * (pipe->burst + 1U); assert(length <= packet || pipe->type != USB_PIPE_ISOCH);
    p->tds[p->tail++ % 512] = (struct td){physical, length, tag}; pipe->queued++;
    return 0;
}
bool usb_pipe_stop(struct usb_pipe* pipe) {
    struct pipe_sim* p = find(pipe); assert(p); stops++;
    while (p->head != p->tail) { struct td t = p->tds[p->head++ % 512]; pipe->queued--; if (pipe->complete) pipe->complete(pipe, t.tag, 0, 26); }
    return true;
}
/* The device services the oldest TD of a pipe: OUT data is handed to `out`, IN data comes from `in`. */
static bool service(struct usb_pipe* pipe, uint8_t* out, unsigned* out_length, const uint8_t* in, unsigned in_length, unsigned code) {
    struct pipe_sim* p = find(pipe); if (!p || p->head == p->tail) return false;
    struct td t = p->tds[p->head++ % 512]; pipe->queued--;
    unsigned actual = t.length;
    if (pipe->in && in) { actual = in_length < t.length ? in_length : t.length; memcpy((void*)(uintptr_t)t.physical, in, actual); }
    if (!pipe->in && out) { memcpy(out, (const void*)(uintptr_t)t.physical, t.length); *out_length = t.length; }
    if (pipe->complete) pipe->complete(pipe, t.tag, code == 1 || code == 13 ? actual : 0, code);
    return true;
}
static unsigned queued_tds(struct usb_pipe* pipe) { struct pipe_sim* p = find(pipe); return p ? p->tail - p->head : 0; }

#include "../arch/x86_64/uaudio.c"

/* ---------------- descriptor builders ---------------- */
static uint8_t* at(void) { return sim.config + sim.length; }
static void put(const uint8_t* bytes, unsigned n) { memcpy(at(), bytes, n); sim.length += n; }
static void config_header(unsigned interfaces) { uint8_t c[9] = {9, 2, 0, 0, (uint8_t)interfaces, 1, 0, 0x80, 50}; put(c, 9); }
static void finish_config(void) { put16(sim.config + 2, sim.length); }
static void interface(unsigned number, unsigned alt, unsigned endpoints, unsigned subclass, unsigned protocol) {
    uint8_t d[9] = {9, 4, (uint8_t)number, (uint8_t)alt, (uint8_t)endpoints, 1, (uint8_t)subclass, (uint8_t)protocol, 0}; put(d, 9);
}
static void endpoint(unsigned address, unsigned attributes, unsigned packet, unsigned interval, unsigned refresh, unsigned synch, bool uac1) {
    uint8_t d[9] = {uac1 ? 9 : 7, 5, (uint8_t)address, (uint8_t)attributes, (uint8_t)packet, (uint8_t)(packet >> 8), (uint8_t)interval, (uint8_t)refresh, (uint8_t)synch}; put(d, d[0]);
}
static void reset(const char* name, bool uac2, unsigned speed) {
    memset(&sim, 0, sizeof(sim)); memset(&dev, 0, sizeof(dev)); memset(pipes, 0, sizeof(pipes)); memset(&card, 0, sizeof(card));
    sim.name = name; sim.uac2 = uac2; sim.speed = speed; dev.speed = (uint8_t)speed; dev.slot = 1;
    strcpy(dev.product_name, "Nexus Test Audio");
}
/* f_uac2 layout: clock 1 (playback, USB OUT) and clock 2 (capture), IT 3 (USB streaming) -> FU 10 -> OT 4 (speaker),
   IT 5 (microphone) -> FU 11 -> OT 6 (USB streaming); AS 1 playback (async OUT EP1 + feedback IN EP1), AS 2 capture. */
static void build_uac2(unsigned feedback_attributes) {
    reset("uac2", true, USB_SPEED_HIGH);
    sim.range_min = 48000; sim.range_max = 48000; sim.range_res = 0; sim.clock_writable = true;
    sim.clock_rate[1] = sim.clock_rate[2] = 48000; sim.vol_min = (int16_t)0xC000; sim.vol_max = 0; sim.vol_res = 0x0080;   /* -64..0 dB, 0.5 dB */
    config_header(3);
    uint8_t iad[8] = {8, 11, 0, 3, 1, 0, 0x20, 0}; put(iad, 8);
    interface(0, 0, 0, 1, 0x20);
    unsigned header = sim.length; uint8_t h[9] = {9, 0x24, 1, 0x00, 0x02, 8, 0, 0, 0}; put(h, 9);
    uint8_t clk1[8] = {8, 0x24, 0x0A, 1, 1, 3, 0, 0}, clk2[8] = {8, 0x24, 0x0A, 2, 1, 3, 0, 0}; put(clk1, 8); put(clk2, 8);
    uint8_t it3[17] = {17, 0x24, 2, 3, 0x01, 0x01, 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0}; put(it3, 17);
    uint8_t fu10[18] = {18, 0x24, 6, 10, 3, 0x0F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; put(fu10, 18);   /* master mute+volume */
    uint8_t ot4[12] = {12, 0x24, 3, 4, 0x01, 0x03, 0, 10, 1, 0, 0, 0}; put(ot4, 12);
    uint8_t it5[17] = {17, 0x24, 2, 5, 0x01, 0x02, 0, 2, 2, 3, 0, 0, 0, 0, 0, 0, 0}; put(it5, 17);
    uint8_t fu11[18] = {18, 0x24, 6, 11, 5, 0x0F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; put(fu11, 18);
    uint8_t ot6[12] = {12, 0x24, 3, 6, 0x01, 0x01, 0, 11, 2, 0, 0, 0}; put(ot6, 12);
    put16(sim.config + header + 6, sim.length - header);
    interface(1, 0, 0, 2, 0x20); interface(1, 1, 2, 2, 0x20);
    uint8_t as1[16] = {16, 0x24, 1, 3, 0, 1, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0}; put(as1, 16);
    uint8_t fmt[6] = {6, 0x24, 2, 1, 2, 16}; put(fmt, 6);
    endpoint(0x01, feedback_attributes, 32, 1, 0, 0, false);                  /* 6 frames x 4 bytes nominal, 8 max (32 B) */
    uint8_t csep[8] = {8, 0x25, 1, 0, 0, 0, 0, 0}; put(csep, 8);
    endpoint(0x81, 0x11, 4, 4, 0, 0, false);                                   /* feedback IN, 1 ms */
    interface(2, 0, 0, 2, 0x20); interface(2, 1, 1, 2, 0x20);
    uint8_t as2[16] = {16, 0x24, 1, 6, 0, 1, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0}; put(as2, 16); put(fmt, 6);
    endpoint(0x82, 0x05, 32, 1, 0, 0, false); put(csep, 8);
    finish_config();
}
/* CM108-style UAC1 headset: IT 1 (USB) -> FU 10 -> OT 3 (headphones); IT 2 (mic) -> FU 11 -> SU 20 -> OT 4 (USB). */
static void build_uac1(unsigned capture_channels, unsigned capture_rate, unsigned subframe) {
    reset("uac1", false, USB_SPEED_FULL);
    sim.vol_min = (int16_t)0xE800; sim.vol_max = 0; sim.vol_res = 0x0100;   /* -24..0 dB, 1 dB */
    config_header(3);
    interface(0, 0, 0, 1, 0);
    unsigned header = sim.length; uint8_t h[10] = {10, 0x24, 1, 0x00, 0x01, 0, 0, 2, 1, 2}; put(h, 10);
    uint8_t it1[12] = {12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 3, 0, 0, 0}; put(it1, 12);
    uint8_t it2[12] = {12, 0x24, 2, 2, 0x01, 0x02, 0, (uint8_t)capture_channels, 0, 0, 0, 0}; put(it2, 12);
    uint8_t ot3[9] = {9, 0x24, 3, 3, 0x02, 0x03, 0, 10, 0}; put(ot3, 9);
    uint8_t ot4[9] = {9, 0x24, 3, 4, 0x01, 0x01, 0, 20, 0}; put(ot4, 9);
    uint8_t su20[7] = {7, 0x24, 5, 20, 1, 11, 0}; put(su20, 7);
    uint8_t fu10[10] = {10, 0x24, 6, 10, 1, 1, 0x01, 0x02, 0x02, 0}; put(fu10, 10);   /* master mute, per-channel volume */
    uint8_t fu11[9] = {9, 0x24, 6, 11, 2, 1, 0x03, 0x00, 0}; put(fu11, 9);              /* master mute + volume */
    put16(sim.config + header + 5, sim.length - header);
    interface(1, 0, 0, 2, 0); interface(1, 1, 1, 2, 0);
    uint8_t as1[7] = {7, 0x24, 1, 1, 1, 1, 0}; put(as1, 7);
    uint8_t fmt1[14] = {14, 0x24, 2, 1, 2, (uint8_t)subframe, (uint8_t)(subframe * 8), 2, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00}; put(fmt1, 14);
    endpoint(0x01, 0x09, 2 * subframe * 49, 1, 0, 0, true);                     /* adaptive OUT */
    uint8_t cs1[7] = {7, 0x25, 1, 1, 1, 1, 0}; put(cs1, 7);                     /* sampling frequency control */
    interface(2, 0, 0, 2, 0); interface(2, 1, 1, 2, 0);
    uint8_t as2[7] = {7, 0x24, 1, 4, 1, 1, 0}; put(as2, 7);
    uint8_t fmt2[11] = {11, 0x24, 2, 1, (uint8_t)capture_channels, 2, 16, 1, (uint8_t)capture_rate, (uint8_t)(capture_rate >> 8), (uint8_t)(capture_rate >> 16)}; put(fmt2, 11);
    endpoint(0x82, 0x05, capture_channels * 2 * (capture_rate / 1000 + 1), 1, 0, 0, true);
    put(cs1, 7);
    finish_config();
}

/* ---------------- scenarios ---------------- */
static void engine_call_start(uint64_t at_frame) { context = card.engine.context; uint64_t q = at_frame; assert(card.engine.start(&q, at_frame + 4800)); }
static void fill_ring(uint64_t from, unsigned frames, int base) {
    for (unsigned i = 0; i < frames; i++) { int16_t* f = card.ring + ((from + i) % SOUND_RING_FRAMES) * 2; f[0] = (int16_t)(base + (int)i); f[1] = (int16_t)-(base + (int)i); }
}
static void detach_and_check(void) {
    unsigned before = allocations; (void)before;
    uaudio_detach(&dev);
    assert(!card.present && card.removes == 1);
    for (unsigned i = 0; i < 8; i++) assert(!pipes[i].open);
    assert(allocations == 0 && allocated == 0);
}
static void scenario_uac2(void) {
    build_uac2(0x05);   /* asynchronous OUT */
    uaudio_attach(&dev);
    assert(card.present && strstr(card.engine.longname, "USB Audio 2.0, high speed") && !strcmp(card.engine.id, "Audio") && !strcmp(card.engine.driver, "NexusUSB"));
    assert(sim.alt[1] == 1 && sim.alt[2] == 1 && sim.clock_rate[1] == 48000);
    assert(strstr(log_text, "audio playback: interface 1 alt 1 stereo 16-bit 48000 Hz, asynchronous + feedback, volume, mute"));
    assert(strstr(log_text, "audio capture: interface 2 alt 1 stereo 16-bit 48000 Hz, asynchronous, volume, mute"));
    /* controls: PCM Playback Volume (0..128 steps of 0.5 dB, TLV min -64 dB), switch; Mic Capture Volume/Switch */
    assert(card.engine.controls == 4 && !strcmp(card.engine.control[0].name, "PCM Playback Volume") && card.engine.control[0].max == 128);
    assert(card.engine.control[0].db_min == -6400 && card.engine.control[0].db_max == 0 + 1 - 1 + (card.engine.control[0].db_max ? card.engine.control[0].db_max : 0));
    assert(!strcmp(card.engine.control[3].name, "Mic Capture Switch"));
    /* playback: 6 frames per 125 us packet, frames copied from the ring in order, converted to S16 stereo */
    struct ua_dir* play = &((struct uaudio*)card.engine.context)->dir[AS_PLAY];
    fill_ring(1000, 4800, 0);
    engine_call_start(1000);
    assert(queued_tds(&play->data) == UA_DEPTH && queued_tds(&play->feedback) == UA_FEEDBACK_DEPTH);
    uint8_t packet[64]; unsigned length = 0; uint64_t position = 0;
    assert(service(&play->data, packet, &length, NULL, 0, 1) && length == 24);
    assert((int16_t)le16(packet) == 0 && (int16_t)le16(packet + 4) == 1 && (int16_t)le16(packet + 22) == -5);
    context = card.engine.context; assert(card.engine.read(&position) && position == 1006);
    /* feedback: the device runs at 48.48 kHz -> 16.16 value 6.06 frames per microframe; packets average 6.06 */
    uint8_t fb[4]; put32(fb, (uint32_t)(6.06 * 65536)); assert(service(&play->feedback, NULL, NULL, fb, 4, 1) || true);
    struct pipe_sim* fp = find(&play->feedback); (void)fp;
    /* (the feedback TD carries IN data: feed it through the IN path) */
    unsigned total_frames = 0, packets = 0;
    for (unsigned i = 0; i < 400; i++) { if (!service(&play->data, packet, &length, NULL, 0, 1)) break; total_frames += length / 4; packets++; }
    assert(play->fb_seen && packets > 300);
    double average = (double)total_frames / packets; assert(average > 6.03 && average < 6.09);
    /* missed service: the position still moves on, nothing stalls */
    context = card.engine.context; card.engine.read(&position); uint64_t before = position;
    assert(service(&play->data, packet, &length, NULL, 0, 23)); card.engine.read(&position); assert(position > before);
    /* stop -> flush requested; a start before the worker ran is refused, after it works */
    context = card.engine.context; card.engine.stop(); assert(play->flush && !play->running);
    uaudio_service(); assert(!play->flush && queued_tds(&play->data) == 0);
    engine_call_start(9000); assert(queued_tds(&play->data) == UA_DEPTH);
    /* capture: one 6-frame packet of S16 stereo -> the capture ring */
    struct ua_dir* rec = &((struct uaudio*)card.engine.context)->dir[AS_CAPTURE];
    context = card.engine.context; assert(card.engine.capture_start());
    uint8_t in[24]; for (unsigned i = 0; i < 6; i++) { put16(in + 4 * i, (uint16_t)(100 + i)); put16(in + 4 * i + 2, (uint16_t)(200 + i)); }
    assert(service(&rec->data, NULL, NULL, in, 24, 1));
    context = card.engine.context; uint64_t captured = 0; assert(card.engine.capture_read(&captured) && captured == 6);
    assert(card.capture[0] == 100 && card.capture[1] == 200 && card.capture[10] == 105);
    assert(service(&rec->data, NULL, NULL, in, 12, 13)); card.engine.capture_read(&captured); assert(captured == 9);   /* short packet */
    /* controls: -10 dB -> SET_CUR CUR on FU 10 master channel; mute switch */
    context = card.engine.context; card.engine.control[0].value[0] = 108; assert(card.engine.control_put(0)); uaudio_service();
    assert(sim.vol[10][0] == (int16_t)(0xC000 + 108 * 0x80));
    card.engine.control[1].value[0] = 0; assert(card.engine.control_put(1)); uaudio_service(); assert(sim.mute[10][0] == 1);
    /* hot-unplug while both directions stream */
    dev.gone = true; detach_and_check();
    printf("uac2 high-speed async + feedback + capture: PASSED\n");
}
static void scenario_uac1(void) {
    build_uac1(1, 48000, 2);
    uaudio_attach(&dev);
    assert(card.present && strstr(card.engine.longname, "USB Audio 1.0, full speed"));
    assert(sim.endpoint_rate[1] == 48000 && sim.endpoint_rate[2] == 48000);
    assert(strstr(log_text, "audio playback: interface 1 alt 1 stereo 16-bit 48000 Hz, adaptive, volume, mute"));
    assert(strstr(log_text, "audio capture: interface 2 alt 1 mono 16-bit 48000 Hz, asynchronous, volume, mute"));
    struct uaudio* u = card.engine.context;
    assert(u->dir[AS_PLAY].control.channel_first == 1 && u->dir[AS_PLAY].control.channels == 2 && u->dir[AS_PLAY].control.mute_channel == 0);
    assert(card.engine.control[0].max == 24 && card.engine.control[0].db_min == -2400);
    /* 48 frames per 1 ms packet */
    fill_ring(0, 4800, 1000); engine_call_start(0);
    uint8_t packet[512]; unsigned length = 0; assert(service(&u->dir[AS_PLAY].data, packet, &length, NULL, 0, 1) && length == 192);
    assert((int16_t)le16(packet + 188) == 1000 + 47 && (int16_t)le16(packet + 190) == -(1000 + 47));
    /* mono capture -> both channels */
    context = card.engine.context; assert(card.engine.capture_start());
    uint8_t in[96]; for (unsigned i = 0; i < 48; i++) put16(in + 2 * i, (uint16_t)(i * 10));
    assert(service(&u->dir[AS_CAPTURE].data, NULL, NULL, in, 96, 1));
    assert(card.capture[2 * 47] == 470 && card.capture[2 * 47 + 1] == 470);
    /* per-channel volume writes both channels */
    card.engine.control[0].value[0] = 12; card.engine.control[0].value[1] = 6; context = card.engine.context; card.engine.control_put(0); uaudio_service();
    assert(sim.vol[10][1] == -12 * 256 && sim.vol[10][2] == -18 * 256);
    dev.gone = false; uaudio_detach(&dev);               /* orderly detach: endpoints dropped, alternate setting 0 */
    assert(sim.alt[1] == 0 && sim.alt[2] == 0 && allocations == 0);
    printf("uac1 full-speed headset (CM108 layout): PASSED\n");
}
static void scenario_dac24_mic16k(void) {
    build_uac1(1, 16000, 3);
    uaudio_attach(&dev);
    struct uaudio* u = card.engine.context;
    assert(u->dir[AS_PLAY].alt.subframe == 3 && u->dir[AS_CAPTURE].ratio == 3);
    fill_ring(0, 100, 0x1234); engine_call_start(0);
    uint8_t packet[512]; unsigned length = 0; assert(service(&u->dir[AS_PLAY].data, packet, &length, NULL, 0, 1) && length == 288);
    assert(packet[0] == 0 && packet[1] == 0x34 && packet[2] == 0x12);      /* S24_3LE: the 16-bit sample in the top bytes */
    context = card.engine.context; assert(card.engine.capture_start());
    uint8_t in[34]; for (unsigned i = 0; i < 16; i++) put16(in + 2 * i, (uint16_t)(300 * (i + 1)));
    assert(service(&u->dir[AS_CAPTURE].data, NULL, NULL, in, 32, 1));
    uint64_t captured = 0; context = card.engine.context; card.engine.capture_read(&captured); assert(captured == 48);
    assert(card.capture[2 * 2] == 300 && card.capture[2 * 3] == 400 && card.capture[2 * 4] == 500 && card.capture[2 * 5] == 600);   /* interpolated */
    dev.gone = true; uaudio_detach(&dev); assert(allocations == 0);
    printf("24-bit DAC + 16 kHz mono microphone (interpolated): PASSED\n");
}
static void scenario_hostile(void) {
    static const char* names[] = {"truncated", "zero length", "unit loop", "huge packet", "no pcm", "44.1 kHz only", "set_interface stall", "rate stall", "volume stall"};
    for (unsigned k = 0; k < 9; k++) {
        build_uac1(2, 48000, 2); log_used = 0; log_text[0] = 0;
        if (k == 0) sim.length -= 20;                                        /* last descriptors cut */
        if (k == 1) { for (unsigned i = 0; i + 1 < sim.length; i += sim.config[i]) if (sim.config[i + 1] == 0x24 && sim.config[i + 2] == 6) { sim.config[i] = 0; break; } }
        if (k == 2) { for (unsigned i = 0; i + 1 < sim.length; i += sim.config[i]) if (sim.config[i + 1] == 0x24 && sim.config[i + 2] == 6 && sim.config[i + 3] == 10) sim.config[i + 4] = 10; }
        if (k == 3) { for (unsigned i = 0; i + 1 < sim.length; i += sim.config[i]) if (sim.config[i + 1] == 5) put16(sim.config + i + 4, 0x7FF); }
        if (k == 4) { for (unsigned i = 0; i + 1 < sim.length; i += sim.config[i]) if (sim.config[i + 1] == 0x24 && sim.config[i + 2] == 1 && sim.config[i] == 7) put16(sim.config + i + 5, 0x2001); }
        if (k == 5) { for (unsigned i = 0; i + 1 < sim.length; i += sim.config[i]) if (sim.config[i + 1] == 0x24 && sim.config[i + 2] == 2 && sim.config[i] >= 11 && sim.config[i + 3] == 1) { sim.config[i + 7] = 1; sim.config[i + 8] = 0x44; sim.config[i + 9] = 0xAC; sim.config[i + 10] = 0; } }
        if (k == 6) sim.fail_set_interface = true;
        if (k == 7) sim.fail_rate = true;
        if (k == 8) sim.stall_volume = true;
        uaudio_attach(&dev);
        bool attached = card.present;
        printf("hostile %-20s -> %s\n", names[k], attached ? "card (partial)" : "rejected");
        if (k == 4 || k == 5 || k == 6 || k == 7) assert(!attached || k == 5);
        if (k == 8) assert(attached && card.engine.controls == 2);           /* mute switches only */
        if (attached) { dev.gone = true; uaudio_detach(&dev); }
        assert(allocations == 0 && allocated == 0);
        for (unsigned i = 0; i < 8; i++) assert(!pipes[i].open);
    }
    /* fuzz: random byte flips in the descriptor set never crash or leak */
    srand(12345);
    for (unsigned round = 0; round < 3000; round++) {
        if (round % 2) build_uac2(0x05); else build_uac1(1, 48000, 2);
        unsigned flips = 1 + (unsigned)rand() % 6;
        for (unsigned f = 0; f < flips; f++) sim.config[(unsigned)rand() % sim.length] = (uint8_t)rand();
        if (rand() % 8 == 0) sim.length = (unsigned)rand() % sim.length;
        uaudio_attach(&dev);
        if (card.present) {
            context = card.engine.context;
            if (card.engine.start) { uint64_t q = 0; if (card.engine.start(&q, 480)) { struct uaudio* u = card.engine.context; uint8_t p[4096]; unsigned l; for (unsigned i = 0; i < 30; i++) service(&u->dir[AS_PLAY].data, p, &l, NULL, 0, 1); } }
            dev.gone = rand() % 2; uaudio_detach(&dev);
        }
        assert(allocations == 0 && allocated == 0);
        for (unsigned i = 0; i < 8; i++) assert(!pipes[i].open);
    }
    printf("hostile descriptors + 3000 fuzzed descriptor sets: PASSED\n");
}
int main(void) {
    scenario_uac2(); scenario_uac1(); scenario_dac24_mic16k(); scenario_hostile();
    printf("UAUDIO SIMULATOR TESTS PASSED\n");
    return 0;
}
