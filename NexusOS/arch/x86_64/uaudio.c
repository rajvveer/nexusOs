/* USB Audio Class 1.0 and 2.0 (UAC1/UAC2) devices as ALSA cards: USB headsets, DACs, microphones, webcam mics.
 *
 * Attach (USB worker, uaudio_attach): the configuration descriptor is walked for the AudioControl interface (header,
 * input/output terminals, feature/selector/mixer units, UAC2 clock sources/selectors/multipliers) and every
 * AudioStreaming interface's alternate settings (Type I PCM: channels, subframe size, bit resolution, UAC1 discrete or
 * continuous rates, the isochronous data endpoint, its synchronization type and explicit feedback endpoint). Per
 * direction the best alternate setting is chosen (48 kHz; capture also 24/16/8 kHz, interpolated up to 48 kHz; 16-bit
 * before 24/32-bit; stereo, mono, then more channels), its pipes are configured on the xHC, SET_INTERFACE selects it and
 * the rate is set (UAC1: endpoint SAMPLING_FREQ_CONTROL when the endpoint has one; UAC2: the clock source's
 * CS_SAM_FREQ_CONTROL after checking its RANGE, read back). The terminal paths (USB streaming IT -> ... -> OT for
 * playback, IT -> ... -> USB streaming OT for capture) give the feature units whose volume (UAC1 GET_MIN/MAX/RES/CUR,
 * UAC2 RANGE/CUR) and mute become ALSA controls "PCM Playback Volume/Switch" and "Mic Capture Volume/Switch" (1/256 dB
 * device units -> TLV dB scale); writes are applied by the USB worker (uaudio_service), since control transfers wait.
 *
 * Streaming (event processing and sound.c calls, kernel lock held): the card's engine (sound_hw.h, cyclic like HDA)
 * keeps UA_DEPTH isochronous TDs queued per direction, one per service interval. Playback copies the next packet's
 * frames from the card's S16 stereo 48 kHz mixer ring into a slot buffer (converted to the device's channels and
 * subframe size) when it queues the TD, and the device position advances when the TD completes; packet sizes follow
 * the explicit feedback endpoint of asynchronous devices (10.14 full speed / 16.16 high speed, auto-detected like
 * Linux) or the nominal rate. Capture converts every received packet (short packets are normal) into the capture ring.
 * A stopped direction asks the worker to flush its ring; a tag generation drops completions of an earlier run.
 * Hot-unplug removes the card (sound_card_remove: open streams get ENODEV). */
#include "usb.h"
#include "sound_hw.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
/* Device state lives as long as the device, not the program that was running when it was plugged in: counted
 * allocations (storage_alloc.c), which the kernel's per-program allocation audit (kernel.c run_image) leaves out. */
#ifndef NEXUS_HOST_TEST
#include "storage_util.h"
#endif

#define UA_SLOTS 64U          /* slot buffers per direction */
#define UA_DEPTH 24U          /* TDs queued while running (24 service intervals: 24 ms at full speed) */
#define UA_FEEDBACK_DEPTH 4U
#define UA_MAX_ALTS 16U
#define UA_MAX_ENTITIES 32U
#define UA_MAX_DEVICES 4U
#define UA_RATE 48000U
enum { AS_PLAY = 0, AS_CAPTURE = 1 };
enum { UAC_HEADER = 1, UAC_INPUT = 2, UAC_OUTPUT = 3, UAC_MIXER = 4, UAC_SELECTOR = 5, UAC_FEATURE = 6, UAC_CLOCK = 0x0A,
       UAC_CLOCK_SELECTOR = 0x0B, UAC_CLOCK_MULTIPLIER = 0x0C };
enum { FU_MUTE = 1, FU_VOLUME = 2 };

struct ua_entity {
    uint8_t id, type, source, clock, channels, sources[8], source_count;
    uint16_t terminal;                               /* terminals: wTerminalType */
    uint32_t controls[3];                            /* feature unit: bmaControls of master, channel 1, channel 2 */
    uint8_t control_size, clock_controls;            /* UAC1 bControlSize; UAC2 clock source bmControls */
};
struct ua_alt {
    uint8_t interface, alt, terminal, channels, subframe, bits, endpoint, attributes, interval, feedback, feedback_interval;
    uint16_t max_packet, feedback_packet;
    bool pcm, freq_control, continuous; uint32_t rates[8], rate_count, rate_min, rate_max;
};
struct ua_control { uint8_t unit, channel_first, channels, mute_channel; bool volume, mute; int16_t min, max, res; int index_volume, index_mute; };
struct ua_dir {
    bool present, running, flush, flushing; struct ua_alt alt; unsigned ratio;   /* capture: 48 kHz / device rate */
    struct usb_pipe data, feedback; uint64_t buffers, feedback_buffer; unsigned buffer_bytes;
    unsigned next_slot, inflight, fb_inflight, fb_next, frame_bytes; uint16_t generation;
    uint16_t slot_frames[UA_SLOTS];
    uint64_t copied, completed;                        /* playback: ring positions (frames) */
    uint64_t written; int16_t last[2];                 /* capture: frames written to the capture ring, interpolation */
    uint32_t q16, fraction; bool fb_seen;              /* frames per packet (16.16), from feedback or nominal */
    uint64_t packets, missed, underruns;
    struct ua_control control; uint32_t clock_rate;
};
struct uaudio {
    struct usb_device* device; bool uac2, gone; uint8_t control_interface;
    int card; struct sound_engine engine; int16_t* ring; int16_t* capture_ring;
    struct ua_entity entities[UA_MAX_ENTITIES]; unsigned entity_count;
    struct ua_dir dir[2];
    volatile uint32_t pending;                         /* control indices to write (worker) */
};
static struct uaudio* instances[UA_MAX_DEVICES];

/* ---------- descriptor walk ---------- */
static struct ua_entity* entity(struct uaudio* u, unsigned id) {
    for (unsigned i = 0; i < u->entity_count; i++) if (u->entities[i].id == id) return &u->entities[i];
    return NULL;
}
static uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void parse_ac(struct uaudio* u, const uint8_t* d, unsigned size) {
    if (size < 4) return;
    unsigned subtype = d[2]; struct ua_entity e; memset(&e, 0, sizeof(e)); e.type = (uint8_t)subtype; e.id = d[3];
    if (subtype == UAC_HEADER) { u->uac2 = size >= 5 && le16(d + 3) >= 0x0200; return; }
    if (!e.id || u->entity_count >= UA_MAX_ENTITIES || entity(u, e.id)) return;     /* ID 0 is invalid; duplicates ignored */
    if (subtype == UAC_INPUT && size >= 8) {
        e.terminal = le16(d + 4);
        if (u->uac2) { if (size < 17) return; e.clock = d[7]; e.channels = d[8]; } else { if (size < 12) return; e.channels = d[7]; }
    } else if (subtype == UAC_OUTPUT && size >= 9) {
        e.terminal = le16(d + 4); e.source = d[7]; if (u->uac2) { if (size < 12) return; e.clock = d[8]; }
    } else if (subtype == UAC_FEATURE && size >= 6) {
        e.source = d[4];
        if (u->uac2) { for (unsigned c = 0; c < 3 && 5 + 4 * c + 4 <= size; c++) e.controls[c] = le32(d + 5 + 4 * c); e.control_size = 4; }
        else {
            e.control_size = d[5];
            if (e.control_size == 0 || e.control_size > 4) return;
            for (unsigned c = 0; c < 3 && 6 + e.control_size * (c + 1) <= size; c++) {
                uint32_t bits = 0; for (unsigned b = 0; b < e.control_size; b++) bits |= (uint32_t)d[6 + e.control_size * c + b] << (8 * b);
                e.controls[c] = bits;
            }
        }
    } else if ((subtype == UAC_SELECTOR || subtype == UAC_MIXER || subtype == UAC_CLOCK_SELECTOR) && size >= 5) {
        unsigned count = d[4];
        if (5 + count > size) return;
        e.source_count = (uint8_t)(count > 8 ? 8 : count);
        for (unsigned i = 0; i < e.source_count; i++) e.sources[i] = d[5 + i];
        e.source = e.source_count ? e.sources[0] : 0;
    } else if (subtype == UAC_CLOCK && u->uac2 && size >= 8) { e.clock_controls = d[5];
    } else if (subtype == UAC_CLOCK_MULTIPLIER && u->uac2 && size >= 7) { e.source = d[4];
    } else return;
    u->entities[u->entity_count++] = e;
}
/* Every AudioStreaming alternate setting with an endpoint, with its format. Malformed descriptors end the walk. */
static unsigned parse_streaming(struct uaudio* u, const uint8_t* data, unsigned length, struct ua_alt* alts) {
    unsigned count = 0; struct ua_alt* current = NULL; bool audio_control = false;
    for (unsigned at = 0; at + 2 <= length;) {
        unsigned size = data[at], type = data[at + 1];
        if (size < 2 || at + size > length) break;
        const uint8_t* d = data + at;
        if (type == 4 && size >= 9) {
            current = NULL; audio_control = d[5] == 1 && d[6] == 1;
            if (audio_control && !u->control_interface) u->control_interface = d[2];
            if (d[5] == 1 && d[6] == 2 && d[3] && d[4] && count < UA_MAX_ALTS) {
                current = &alts[count++]; memset(current, 0, sizeof(*current)); current->interface = d[2]; current->alt = d[3];
            }
        } else if (type == 0x24 && audio_control) parse_ac(u, d, size);
        else if (type == 0x24 && current && size >= 3) {
            if (d[2] == 1 && size >= 7 && !u->uac2) { current->terminal = d[3]; current->pcm = le16(d + 5) == 1; }
            else if (d[2] == 1 && size >= 16 && u->uac2) { current->terminal = d[3]; current->pcm = d[5] == 1 && (le32(d + 6) & 1); current->channels = d[10]; }
            else if (d[2] == 2 && size >= 6 && d[3] == 1) {
                if (u->uac2) { current->subframe = d[4]; current->bits = d[5]; }
                else if (size >= 8) {
                    current->channels = d[4]; current->subframe = d[5]; current->bits = d[6];
                    unsigned rates = d[7];
                    if (!rates && size >= 14) { current->continuous = true; current->rate_min = d[8] | d[9] << 8 | (uint32_t)d[10] << 16; current->rate_max = d[11] | d[12] << 8 | (uint32_t)d[13] << 16; }
                    for (unsigned r = 0; r < rates && r < 8 && 8 + 3 * r + 3 <= size; r++) current->rates[current->rate_count++] = d[8 + 3 * r] | d[9 + 3 * r] << 8 | (uint32_t)d[10 + 3 * r] << 16;
                }
            }
        } else if (type == 0x25 && current && size >= 4 && d[2] == 1) current->freq_control = !u->uac2 && (d[3] & 1) != 0;
        else if (type == 5 && current && size >= 7 && (d[3] & 3) == 1) {
            unsigned usage = (d[3] >> 4) & 3;
            if (usage == 1 || (current->endpoint && (d[2] & 0x80) != (current->endpoint & 0x80))) {   /* explicit feedback */
                current->feedback = d[2]; current->feedback_packet = le16(d + 4) & 0x7FF; current->feedback_interval = d[6];
            } else if (!current->endpoint) {
                current->endpoint = d[2]; current->attributes = d[3]; current->max_packet = le16(d + 4); current->interval = d[6];
                if (!u->uac2 && size >= 9 && d[8]) current->feedback = d[8];
            }
        }
        at += size;
    }
    return count;
}
/* The feature unit on a terminal's path: playback walks downstream from the streaming input terminal, capture
   upstream from the streaming output terminal. Loops in a hostile descriptor end at UA_MAX_ENTITIES hops. */
static struct ua_entity* feature_for(struct uaudio* u, unsigned terminal, bool capture) {
    unsigned id = terminal;
    for (unsigned hops = 0; hops < UA_MAX_ENTITIES && id; hops++) {
        if (capture) {
            struct ua_entity* e = entity(u, id); if (!e) return NULL;
            if (e->type == UAC_FEATURE) return e;
            if (e->type == UAC_INPUT) return NULL;
            id = e->source;
        } else {
            struct ua_entity* next = NULL;
            for (unsigned i = 0; i < u->entity_count && !next; i++) {
                struct ua_entity* e = &u->entities[i];
                if (e->type == UAC_INPUT || e->type == UAC_CLOCK || e->type == UAC_CLOCK_SELECTOR || e->type == UAC_CLOCK_MULTIPLIER) continue;
                if (e->source == id) next = e;
                for (unsigned k = 0; k < e->source_count && !next; k++) if (e->sources[k] == id) next = e;
            }
            if (!next || next->type == UAC_OUTPUT) return NULL;
            if (next->type == UAC_FEATURE) return next;
            id = next->id;
        }
    }
    return NULL;
}
static bool control_bit(const struct uaudio* u, const struct ua_entity* fu, unsigned channel, unsigned control) {
    uint32_t bits = fu->controls[channel];
    return u->uac2 ? ((bits >> (2 * (control - 1))) & 3) == 3 : (bits >> (control - 1)) & 1;
}

/* ---------- class requests (worker) ---------- */
static int request(struct uaudio* u, bool get, unsigned code, unsigned selector, unsigned channel, unsigned unit, void* data, unsigned length) {
    return usb_control(u->device, get ? 0xA1 : 0x21, (uint8_t)code, (uint16_t)(selector << 8 | channel), (uint16_t)(unit << 8 | u->control_interface), data, (uint16_t)length);
}
static bool volume_range(struct uaudio* u, struct ua_control* c) {
    uint8_t buffer[64]; unsigned channel = c->channel_first;
    if (u->uac2) {
        int n = request(u, true, 0x02, FU_VOLUME, channel, c->unit, buffer, 14);    /* RANGE: wNumSubRanges + first range */
        if (n < 8 || !le16(buffer)) return false;
        c->min = (int16_t)le16(buffer + 2); c->max = (int16_t)le16(buffer + 4); c->res = (int16_t)le16(buffer + 6);
    } else {
        if (request(u, true, 0x82, FU_VOLUME, channel, c->unit, buffer, 2) != 2) return false; c->min = (int16_t)le16(buffer);
        if (request(u, true, 0x83, FU_VOLUME, channel, c->unit, buffer, 2) != 2) return false; c->max = (int16_t)le16(buffer);
        if (request(u, true, 0x84, FU_VOLUME, channel, c->unit, buffer, 2) != 2) c->res = 1; else c->res = (int16_t)le16(buffer);
    }
    if (c->min == (int16_t)0x8000) c->min = (int16_t)0x8001;   /* -inf is not a usable step */
    if (c->res <= 0) c->res = 1;
    return c->max > c->min && (c->max - c->min) / c->res <= 65535;
}
static int16_t volume_get(struct uaudio* u, const struct ua_control* c, unsigned channel) {
    uint8_t buffer[2] = {0, 0}; request(u, true, u->uac2 ? 0x01 : 0x81, FU_VOLUME, channel, c->unit, buffer, 2); return (int16_t)le16(buffer);
}
static void controls_setup(struct uaudio* u, struct ua_dir* d, bool capture) {
    struct ua_control* c = &d->control; c->index_volume = c->index_mute = -1;
    struct ua_entity* fu = feature_for(u, d->alt.terminal, capture);
    if (!fu) return;
    c->unit = fu->id;
    bool master_volume = control_bit(u, fu, 0, FU_VOLUME), master_mute = control_bit(u, fu, 0, FU_MUTE);
    c->channel_first = master_volume ? 0 : 1; c->channels = master_volume ? 1 : (control_bit(u, fu, 2, FU_VOLUME) ? 2 : 1);
    c->volume = (master_volume || control_bit(u, fu, 1, FU_VOLUME)) && volume_range(u, c);
    c->mute = master_mute || control_bit(u, fu, 1, FU_MUTE); c->mute_channel = master_mute ? 0 : 1;
    struct sound_engine* e = &u->engine;
    if (c->volume && e->controls < SOUND_CONTROLS) {
        struct sound_control* v = &e->control[e->controls]; c->index_volume = (int)e->controls++;
        strcpy(v->name, capture ? "Mic Capture Volume" : "PCM Playback Volume"); v->type = SOUND_CONTROL_INTEGER; v->channels = c->channels;
        v->min = 0; v->max = (c->max - c->min) / c->res; v->db_min = c->min * 100 / 256; v->db_step = c->res * 100 / 256; if (!v->db_step) v->db_step = 1;
        v->db_max = (c->min + v->max * c->res) * 100 / 256; if (!v->db_max) v->db_max = 1;   /* exact steps: TLV DB_MINMAX, like snd-usb-audio */
        for (unsigned k = 0; k < c->channels; k++) {
            int value = (volume_get(u, c, c->channel_first + k) - c->min) / c->res;
            v->value[k] = value < 0 ? 0 : value > v->max ? v->max : value;
        }
    }
    if (c->mute && e->controls < SOUND_CONTROLS) {
        struct sound_control* m = &e->control[e->controls]; c->index_mute = (int)e->controls++;
        strcpy(m->name, capture ? "Mic Capture Switch" : "PCM Playback Switch"); m->type = SOUND_CONTROL_BOOLEAN; m->channels = 1; m->max = 1;
        uint8_t muted = 0; request(u, true, u->uac2 ? 0x01 : 0x81, FU_MUTE, c->mute_channel, c->unit, &muted, 1);
        m->value[0] = !muted;
    }
}
static void control_write(struct uaudio* u, unsigned index) {
    for (unsigned k = 0; k < 2; k++) {
        struct ua_control* c = &u->dir[k].control; const struct sound_control* s = &u->engine.control[index];
        if ((int)index == c->index_volume)
            for (unsigned ch = 0; ch < c->channels; ch++) {
                int16_t value = (int16_t)(c->min + s->value[ch] * c->res); uint8_t data[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
                request(u, false, 0x01, FU_VOLUME, c->channel_first + ch, c->unit, data, 2);            /* SET_CUR / CUR */
            }
        if ((int)index == c->index_mute) {
            uint8_t muted = s->value[0] ? 0 : 1;
            request(u, false, 0x01, FU_MUTE, c->mute_channel, c->unit, &muted, 1);
        }
    }
}

/* ---------- sample rate ---------- */
static bool rate_listed(const struct ua_alt* a, uint32_t rate) {
    if (a->continuous) return rate >= a->rate_min && rate <= a->rate_max;
    for (unsigned i = 0; i < a->rate_count; i++) if (a->rates[i] == rate) return true;
    return false;
}
/* UAC2: the clock source feeding a terminal (through clock selectors' current input and multipliers). */
static unsigned clock_of(struct uaudio* u, unsigned terminal) {
    struct ua_entity* t = entity(u, terminal); unsigned id = t ? t->clock : 0;
    for (unsigned hops = 0; hops < 8 && id; hops++) {
        struct ua_entity* c = entity(u, id); if (!c) return 0;
        if (c->type == UAC_CLOCK) return id;
        if (c->type == UAC_CLOCK_SELECTOR) {
            uint8_t pin = 1; request(u, true, 0x01, 1, 0, id, &pin, 1);
            id = pin >= 1 && pin <= c->source_count ? c->sources[pin - 1] : c->source;
        } else if (c->type == UAC_CLOCK_MULTIPLIER) id = c->source;
        else return 0;
    }
    return 0;
}
static bool uac2_rate_ok(struct uaudio* u, unsigned clock, uint32_t rate) {
    if (!clock) return false;
    uint8_t buffer[2 + 12 * 16]; int n = request(u, true, 0x02, 1, 0, clock, buffer, sizeof(buffer));
    if (n < 14) return false;
    unsigned ranges = le16(buffer);
    for (unsigned i = 0; i < ranges && 2 + 12 * i + 12 <= (unsigned)n; i++) {
        uint32_t min = le32(buffer + 2 + 12 * i), max = le32(buffer + 6 + 12 * i), res = le32(buffer + 10 + 12 * i);
        if (rate >= min && rate <= max && (!res || (rate - min) % res == 0)) return true;
    }
    return false;
}
static bool set_rate(struct uaudio* u, struct ua_dir* d, uint32_t rate) {
    uint8_t data[4] = {(uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24)};
    if (u->uac2) {
        unsigned clock = clock_of(u, d->alt.terminal); struct ua_entity* c = clock ? entity(u, clock) : NULL;
        if (!c || !uac2_rate_ok(u, clock, rate)) return false;
        if ((c->clock_controls & 3) == 3 && request(u, false, 0x01, 1, 0, clock, data, 4) < 0) return false;
        uint8_t current[4] = {0, 0, 0, 0};
        if (request(u, true, 0x01, 1, 0, clock, current, 4) == 4 && le32(current) != rate) return false;
        d->clock_rate = rate; return true;
    }
    if (d->alt.freq_control && usb_control(u->device, 0x22, 0x01, 0x0100, d->alt.endpoint, data, 3) < 0) return false;
    d->clock_rate = rate; return true;
}

/* ---------- alternate-setting choice ---------- */
static int alt_score(struct uaudio* u, const struct ua_alt* a, bool capture, uint32_t* rate) {
    if (!a->pcm || !a->endpoint || ((a->endpoint & 0x80) != 0) != capture || !a->channels || a->channels > 8) return -1;
    if (a->subframe != 2 && a->subframe != 3 && a->subframe != 4) return -1;
    unsigned packet = (a->max_packet & 0x7FF) * (1 + ((a->max_packet >> 11) & 3));
    if (packet < (unsigned)a->channels * a->subframe || packet > 3072) return -1;
    static const uint32_t candidates[] = {48000, 24000, 16000, 8000};
    *rate = 0;
    for (unsigned i = 0; i < (capture ? 4U : 1U) && !*rate; i++) {
        bool ok = u->uac2 ? uac2_rate_ok(u, clock_of(u, a->terminal), candidates[i]) : rate_listed(a, candidates[i]);
        if (ok) *rate = candidates[i];
    }
    if (!*rate) return -1;
    int score = (*rate == UA_RATE ? 100 : 40 - (int)(UA_RATE / *rate)) + (a->subframe == 2 ? 20 : a->subframe == 3 ? 10 : 5);
    score += a->channels == 2 ? 6 : a->channels == 1 ? 4 : 2;
    return score;
}
static bool choose(struct uaudio* u, struct ua_alt* alts, unsigned count, bool capture, uint32_t* rate) {
    struct ua_dir* d = &u->dir[capture]; int best = -1, best_score = -1; *rate = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t r; int score = alt_score(u, &alts[i], capture, &r);
        if (score > best_score) { best = (int)i; best_score = score; *rate = r; }
    }
    if (best < 0) return false;
    d->alt = alts[best]; d->ratio = UA_RATE / *rate; d->frame_bytes = d->alt.channels * d->alt.subframe;
    return true;
}

/* ---------- streaming ---------- */
static unsigned packet_microframes(const struct uaudio* u, unsigned interval) {
    if (u->device->speed == USB_SPEED_FULL || u->device->speed == USB_SPEED_LOW) {
        unsigned exponent = interval ? interval - 1 : 0; if (exponent > 4) exponent = 4;
        return 8U << exponent;                                              /* full speed: 2^(bInterval-1) frames */
    }
    unsigned exponent = interval ? interval - 1 : 0; if (exponent > 6) exponent = 6;
    return 1U << exponent;
}
static uint32_t nominal_q16(const struct uaudio* u, const struct ua_dir* d) {
    return (uint32_t)(((uint64_t)(UA_RATE / d->ratio) << 16) * packet_microframes(u, d->alt.interval) / 8000);
}
static unsigned max_packet_bytes(const struct ua_dir* d) { return (d->alt.max_packet & 0x7FF) * (1 + ((d->alt.max_packet >> 11) & 3)); }
static unsigned next_packet_frames(struct uaudio* u, struct ua_dir* d) {
    uint32_t q = d->q16 ? d->q16 : nominal_q16(u, d);
    uint32_t total = d->fraction + q; unsigned frames = total >> 16; d->fraction = total & 0xFFFF;
    if (frames * d->frame_bytes > max_packet_bytes(d)) frames = max_packet_bytes(d) / d->frame_bytes;
    return frames;
}
static void put_sample(uint8_t* out, unsigned subframe, int16_t value) {
    if (subframe == 2) { out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8); }
    else if (subframe == 3) { out[0] = 0; out[1] = (uint8_t)value; out[2] = (uint8_t)(value >> 8); }
    else { out[0] = out[1] = 0; out[2] = (uint8_t)value; out[3] = (uint8_t)(value >> 8); }
}
static int16_t get_sample(const uint8_t* in, unsigned subframe) { return (int16_t)(in[subframe - 2] | in[subframe - 1] << 8); }
static uint32_t tag_of(const struct ua_dir* d, unsigned slot) { return (uint32_t)d->generation << 16 | slot; }
static bool queue_out(struct uaudio* u) {
    struct ua_dir* d = &u->dir[AS_PLAY];
    unsigned slot = d->next_slot, frames = next_packet_frames(u, d);
    uint8_t* out = (uint8_t*)(d->buffers + (uint64_t)slot * d->buffer_bytes);
    for (unsigned i = 0; i < frames; i++) {
        const int16_t* frame = u->ring + ((d->copied + i) % SOUND_RING_FRAMES) * 2;
        for (unsigned ch = 0; ch < d->alt.channels; ch++) {
            int16_t value = d->alt.channels == 1 ? (int16_t)(((int32_t)frame[0] + frame[1]) / 2) : ch < 2 ? frame[ch] : 0;
            put_sample(out + (i * d->alt.channels + ch) * d->alt.subframe, d->alt.subframe, value);
        }
    }
    d->slot_frames[slot] = (uint16_t)frames;
    if (usb_pipe_queue(&d->data, virt_to_phys(out), frames * d->frame_bytes, tag_of(d, slot))) return false;
    d->copied += frames; d->next_slot = (slot + 1) % UA_SLOTS; d->inflight++;
    return true;
}
static bool queue_in(struct uaudio* u) {
    struct ua_dir* d = &u->dir[AS_CAPTURE];
    unsigned slot = d->next_slot, length = max_packet_bytes(d);
    if (length > d->buffer_bytes) length = d->buffer_bytes;
    uint8_t* in = (uint8_t*)(d->buffers + (uint64_t)slot * d->buffer_bytes);
    if (usb_pipe_queue(&d->data, virt_to_phys(in), length, tag_of(d, slot))) return false;
    d->next_slot = (slot + 1) % UA_SLOTS; d->inflight++;
    return true;
}
static void queue_feedback(struct uaudio* u) {
    struct ua_dir* d = &u->dir[AS_PLAY];
    while (d->feedback.configured && d->fb_inflight < UA_FEEDBACK_DEPTH) {
        uint8_t* buffer = (uint8_t*)(d->feedback_buffer + (uint64_t)d->fb_next * 64);
        unsigned length = d->alt.feedback_packet ? d->alt.feedback_packet : 4; if (length > 64) length = 64;
        if (usb_pipe_queue(&d->feedback, virt_to_phys(buffer), length, d->fb_next)) break;
        d->fb_inflight++; d->fb_next = (d->fb_next + 1) % UA_FEEDBACK_DEPTH;
    }
}
static void play_complete(struct usb_pipe* pipe, uint32_t tag, unsigned length, unsigned code) {
    (void)length;
    struct uaudio* u = pipe->owner; struct ua_dir* d = &u->dir[AS_PLAY];
    if (d->inflight) d->inflight--;
    if (!d->running || tag >> 16 != d->generation || code == 26) return;   /* an earlier run, or cancelled by a flush */
    d->packets++; if (code == 23) d->missed++;
    d->completed += d->slot_frames[tag & 0xFFFF];     /* a missed packet's frames are gone too: the clock moves on */
    while (d->running && d->inflight < UA_DEPTH && queue_out(u)) {}
}
/* Explicit feedback: frames per service interval. 3 bytes = 10.14 frames per 1 ms frame (full speed), 4 bytes = 16.16
   frames per microframe (high speed); some high-speed devices send the full-speed format: whichever lands near the
   nominal rate is taken, anything implausible is ignored. */
static void feedback_complete(struct usb_pipe* pipe, uint32_t tag, unsigned length, unsigned code) {
    struct uaudio* u = pipe->owner; struct ua_dir* d = &u->dir[AS_PLAY];
    if (d->fb_inflight) d->fb_inflight--;
    if ((code == 1 || code == 13) && length >= 3) {
        const uint8_t* p = (const uint8_t*)(d->feedback_buffer + (uint64_t)(tag % UA_FEEDBACK_DEPTH) * 64);
        uint32_t raw = length >= 4 ? le32(p) : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16), nominal = nominal_q16(u, d);
        unsigned micro = packet_microframes(u, d->alt.interval);
        uint64_t candidates[3] = {u->device->speed == USB_SPEED_FULL ? (uint64_t)raw << 2 : (uint64_t)raw * micro,
                                  (uint64_t)raw * micro << 2, ((uint64_t)raw << 2) * micro / 8};
        for (unsigned i = 0; i < 3; i++)
            if (candidates[i] > nominal - nominal / 8 && candidates[i] < nominal + nominal / 8) { d->q16 = (uint32_t)candidates[i]; d->fb_seen = true; break; }
    }
    if (d->running) queue_feedback(u);
}
static void capture_complete(struct usb_pipe* pipe, uint32_t tag, unsigned length, unsigned code) {
    (void)code;
    struct uaudio* u = pipe->owner; struct ua_dir* d = &u->dir[AS_CAPTURE];
    if (d->inflight) d->inflight--;
    if (!d->running || tag >> 16 != d->generation || code == 26) return;
    d->packets++;
    const uint8_t* in = (const uint8_t*)(d->buffers + (uint64_t)(tag & 0xFFFF) * d->buffer_bytes);
    if (length > d->buffer_bytes) length = d->buffer_bytes;
    unsigned frames = d->frame_bytes ? length / d->frame_bytes : 0;
    if (code == 23) {                                  /* a missed packet: keep the timeline with silence */
        d->missed++; frames = (unsigned)(nominal_q16(u, d) >> 16);
        for (unsigned i = 0; i < frames * d->ratio; i++, d->written++) { int16_t* out = u->capture_ring + (d->written % SOUND_RING_FRAMES) * 2; out[0] = out[1] = 0; }
        frames = 0;
    }
    for (unsigned i = 0; i < frames; i++) {
        const uint8_t* f = in + i * d->frame_bytes;
        int16_t left = get_sample(f, d->alt.subframe), right = d->alt.channels > 1 ? get_sample(f + d->alt.subframe, d->alt.subframe) : left;
        for (unsigned k = 1; k <= d->ratio; k++, d->written++) {               /* linear interpolation up to 48 kHz */
            int16_t* out = u->capture_ring + (d->written % SOUND_RING_FRAMES) * 2;
            out[0] = (int16_t)(d->last[0] + (left - d->last[0]) * (int)k / (int)d->ratio);
            out[1] = (int16_t)(d->last[1] + (right - d->last[1]) * (int)k / (int)d->ratio);
        }
        d->last[0] = left; d->last[1] = right;
    }
    while (d->running && d->inflight < UA_DEPTH && queue_in(u)) {}
}

/* ---------- engine (sound_hw.h); sound.c selected this card, so the context is ours ---------- */
static struct uaudio* self(void) { return (struct uaudio*)sound_engine_context(); }
static void stop_direction(struct uaudio* u, struct ua_dir* d) {
    if (!d->running || u->gone) { d->running = false; return; }
    d->running = false; d->generation++; d->flush = true; usb_worker_kick();   /* the worker empties the ring */
}
static void ua_stop(void) { struct uaudio* u = self(); if (u) stop_direction(u, &u->dir[AS_PLAY]); }
static bool ua_start(uint64_t* queued, uint64_t target) {
    struct uaudio* u = self(); struct ua_dir* d = &u->dir[AS_PLAY];
    if (!u || u->gone || !d->data.configured || d->flushing) return false;   /* the worker is emptying the ring: next feed */
    d->generation++; d->running = true; d->flush = false; d->copied = d->completed = *queued; d->fraction = 0;
    while (d->inflight < UA_DEPTH && queue_out(u)) {}
    queue_feedback(u);
    if (d->copied == *queued) { d->running = false; return false; }
    if (target > *queued) *queued = target;
    return true;
}
static bool ua_extend(uint64_t* queued, uint64_t target) { if (target > *queued) *queued = target; return true; }
static bool ua_read(uint64_t* position) {
    struct uaudio* u = self(); struct ua_dir* d = &u->dir[AS_PLAY];
    *position = d->completed;
    if (d->running && d->inflight < UA_DEPTH / 2) while (d->inflight < UA_DEPTH && queue_out(u)) {}   /* refill after a lag */
    return d->running && !u->gone;
}
static bool ua_capture_start(void) {
    struct uaudio* u = self(); struct ua_dir* d = &u->dir[AS_CAPTURE];
    if (!u || u->gone || !d->data.configured || d->flushing) return false;
    d->generation++; d->running = true; d->flush = false; d->written = 0; d->last[0] = d->last[1] = 0;
    while (d->inflight < UA_DEPTH && queue_in(u)) {}
    if (!d->inflight) { d->running = false; return false; }
    return true;
}
static void ua_capture_stop(void) { struct uaudio* u = self(); if (u) stop_direction(u, &u->dir[AS_CAPTURE]); }
static bool ua_capture_read(uint64_t* position) {
    struct uaudio* u = self(); struct ua_dir* d = &u->dir[AS_CAPTURE];
    *position = d->written;
    if (d->running && d->inflight < UA_DEPTH / 2) while (d->inflight < UA_DEPTH && queue_in(u)) {}
    return d->running && !u->gone;
}
static bool ua_control_put(unsigned index) {
    struct uaudio* u = self(); const struct sound_control* live = sound_engine_control(index);
    if (!u || u->gone || !live || index >= SOUND_CONTROLS) return false;
    u->engine.control[index].value[0] = live->value[0]; u->engine.control[index].value[1] = live->value[1];   /* the worker sends these */
    u->pending |= 1U << index; usb_worker_kick();
    return true;
}

/* ---------- attach / detach (USB worker) ---------- */
static bool pipe_setup(struct uaudio* u, struct ua_dir* d, bool capture) {
    struct usb_device* device = u->device;
    unsigned bytes = 64; while (bytes < max_packet_bytes(d)) bytes <<= 1;
    d->buffer_bytes = bytes; d->buffers = dma32_alloc((uint64_t)UA_SLOTS * bytes);
    if (!d->buffers) return false;
    memset((void*)d->buffers, 0, (uint64_t)UA_SLOTS * bytes);
    bool high = device->speed == USB_SPEED_HIGH;
    d->data = (struct usb_pipe){.owner = u, .complete = capture ? capture_complete : play_complete, .address = d->alt.endpoint, .type = USB_PIPE_ISOCH,
                                .interval = d->alt.interval, .max_packet = (uint16_t)(d->alt.max_packet & 0x7FF),
                                .burst = (uint8_t)(high ? (d->alt.max_packet >> 11) & 3 : 0)};
    if (!usb_pipe_open(device, &d->data)) return false;
    struct usb_pipe* add[2] = {&d->data, NULL}; unsigned adds = 1;
    if (!capture && d->alt.feedback && (d->alt.feedback & 0x80) && ((d->alt.attributes >> 2) & 3) == 1) {  /* asynchronous: feedback pipe */
        d->feedback_buffer = dma32_alloc(4096);
        if (d->feedback_buffer) {
            memset((void*)d->feedback_buffer, 0, 4096);
            d->feedback = (struct usb_pipe){.owner = u, .complete = feedback_complete, .address = d->alt.feedback, .type = USB_PIPE_ISOCH,
                                            .interval = d->alt.feedback_interval ? d->alt.feedback_interval : d->alt.interval,
                                            .max_packet = d->alt.feedback_packet ? d->alt.feedback_packet : (device->speed == USB_SPEED_FULL ? 3 : 4)};
            if (usb_pipe_open(device, &d->feedback)) add[adds++] = &d->feedback;
        }
    }
    if (!usb_pipes_configure(device, add, adds, NULL, 0)) { usb_log_device(device, "audio endpoint configuration failed"); return false; }
    return true;
}
static void dir_release(struct uaudio* u, struct ua_dir* d) {
    d->running = false;
    if (d->present && !u->device->gone && u->device->slot) {   /* device still there: drop the endpoints, alternate setting 0 */
        struct usb_pipe* drop[2] = {&d->data, &d->feedback};
        usb_pipes_configure(u->device, NULL, 0, drop, d->feedback.configured ? 2 : 1);
        usb_control(u->device, 0x01, 11, 0, d->alt.interface, NULL, 0);
    }
    d->present = false;
    if (d->data.device) usb_pipe_close(&d->data);
    if (d->feedback.device) usb_pipe_close(&d->feedback);
    if (d->buffers) arena_free(d->buffers, (uint64_t)UA_SLOTS * d->buffer_bytes);
    if (d->feedback_buffer) arena_free(d->feedback_buffer, 4096);
    d->buffers = d->feedback_buffer = 0; d->inflight = d->fb_inflight = 0;
}
/* Pipes, SET_INTERFACE, sample rate; on any failure the direction is released (the device keeps working without it). */
static bool open_direction(struct uaudio* u, struct ua_alt* alts, unsigned count, bool capture) {
    struct ua_dir* d = &u->dir[capture]; uint32_t rate;
    if (!choose(u, alts, count, capture, &rate)) return false;
    if (!pipe_setup(u, d, capture)) { dir_release(u, d); return false; }
    d->present = true;
    if (usb_control(u->device, 0x01, 11, d->alt.alt, d->alt.interface, NULL, 0) < 0) { usb_log_device(u->device, "audio SET_INTERFACE failed"); dir_release(u, d); return false; }
    if (!set_rate(u, d, rate)) { usb_log_device(u->device, "audio sample rate not accepted"); dir_release(u, d); return false; }
    return true;
}
static bool id_used(const char* id) {
    for (unsigned i = 0; i < UA_MAX_DEVICES; i++) if (instances[i] && !strcmp(instances[i]->engine.id, id)) return true;
    return !strcmp(id, "NexusHDA") || !strcmp(id, "NexusAC97");
}
static void add_text(char* out, unsigned size, const char* text) { unsigned n = (unsigned)strlen(out); while (*text && n + 1 < size) out[n++] = *text++; out[n] = 0; }
/* ALSA identity like snd-usb-audio: card id = the last word of the product name (letters/digits, <= 15), unique. */
static void identity(struct uaudio* u) {
    struct sound_engine* e = &u->engine; const char* product = u->device->product_name[0] ? u->device->product_name : "USB Audio";
    const char* word = product; for (const char* p = product; *p; p++) if (*p == ' ' && p[1]) word = p + 1;
    unsigned n = 0;
    for (const char* p = word; *p && n < 15; p++) if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) e->id[n++] = *p;
    if (!n) { strcpy(e->id, "USB"); n = 3; }
    e->id[n] = 0;
    for (unsigned k = 1; id_used(e->id) && k < 10; k++) { if (n > 13) n = 13; e->id[n] = '_'; e->id[n + 1] = (char)('0' + k); e->id[n + 2] = 0; }
    /* driver "NexusUSB", not Linux's "USB-Audio": alsa-lib's cards/USB-Audio.conf would put dmix (IPC, /dev/snd/timer) in
       front of "default"; the kernel mixes clients itself, as for the PCI cards. */
    strcpy(e->driver, "NexusUSB"); e->name[0] = 0; add_text(e->name, sizeof(e->name), product);
    e->longname[0] = 0; add_text(e->longname, sizeof(e->longname), product);
    add_text(e->longname, sizeof(e->longname), u->uac2 ? " (USB Audio 2.0, " : " (USB Audio 1.0, ");
    add_text(e->longname, sizeof(e->longname), u->device->speed == USB_SPEED_HIGH ? "high speed)" : u->device->speed >= USB_SPEED_SUPER ? "super speed)" : "full speed)");
    strcpy(e->mixer, "USB Mixer"); strcpy(e->components, u->uac2 ? "USB:uac2" : "USB:uac1");
    strcpy(e->pcm_name, "USB Audio"); strcpy(e->capture_name, "USB Audio");
}
bool uaudio_wanted(const struct usb_device* device) {
    unsigned length; const uint8_t* data = usb_config(device, &length);
    for (unsigned at = 0; data && at + 9 <= length && data[at] >= 2 && at + data[at] <= length; at += data[at])
        if (data[at + 1] == 4 && data[at + 5] == 1 && data[at + 6] == 2) return true;     /* an AudioStreaming interface */
    return false;
}
static void log_dir(struct uaudio* u, const struct ua_dir* d, const char* what) {
    char text[160] = "audio "; add_text(text, sizeof(text), what);
    char number[16]; unsigned rate = UA_RATE / d->ratio;
    add_text(text, sizeof(text), ": interface "); number[0] = (char)('0' + d->alt.interface % 10); number[1] = 0; add_text(text, sizeof(text), number);
    add_text(text, sizeof(text), " alt "); number[0] = (char)('0' + d->alt.alt % 10); add_text(text, sizeof(text), number);
    add_text(text, sizeof(text), d->alt.channels == 1 ? " mono " : d->alt.channels == 2 ? " stereo " : " multichannel ");
    add_text(text, sizeof(text), d->alt.subframe == 2 ? "16-bit " : d->alt.subframe == 3 ? "24-bit " : "32-bit ");
    unsigned k = 0; char digits[8]; do { digits[k++] = (char)('0' + rate % 10); rate /= 10; } while (rate);
    unsigned m = 0; while (k) number[m++] = digits[--k]; number[m] = 0; add_text(text, sizeof(text), number); add_text(text, sizeof(text), " Hz");
    unsigned sync = (d->alt.attributes >> 2) & 3;
    add_text(text, sizeof(text), sync == 1 ? ", asynchronous" : sync == 2 ? ", adaptive" : sync == 3 ? ", synchronous" : "");
    if (d->feedback.configured) add_text(text, sizeof(text), " + feedback");
    if (d->control.index_volume >= 0) add_text(text, sizeof(text), ", volume");
    if (d->control.index_mute >= 0) add_text(text, sizeof(text), ", mute");
    usb_log_device(u->device, text);
}
void uaudio_attach(struct usb_device* device) {
    unsigned slot = 0; while (slot < UA_MAX_DEVICES && instances[slot]) slot++;
    if (slot >= UA_MAX_DEVICES) { usb_log_device(device, "too many USB audio devices"); return; }
    uint64_t memory = arena_alloc(sizeof(struct uaudio));
    if (!memory) { usb_log_device(device, "audio: out of memory"); return; }
    struct uaudio* u = (struct uaudio*)memory; memset(u, 0, sizeof(*u)); u->device = device; u->card = -1;
    for (unsigned k = 0; k < 2; k++) u->dir[k].control.index_volume = u->dir[k].control.index_mute = -1;
    unsigned length; const uint8_t* config = usb_config(device, &length);
    static struct ua_alt alts[UA_MAX_ALTS];            /* worker only: off its small stack */
    unsigned count = config ? parse_streaming(u, config, length, alts) : 0;
    bool play = open_direction(u, alts, count, false);
    bool record = open_direction(u, alts, count, true);
    if (!play && !record) { usb_log_device(device, "audio: no usable PCM stream (48 kHz playback, 8-48 kHz capture)"); arena_free(memory, sizeof(struct uaudio)); return; }
    struct sound_engine* e = &u->engine; identity(u);
    e->context = u; e->cyclic = true; e->stop = ua_stop; e->extend = ua_extend; e->read = ua_read;
    if (play) { e->start = ua_start; controls_setup(u, &u->dir[AS_PLAY], false); }
    if (record) { e->capture_start = ua_capture_start; e->capture_stop = ua_capture_stop; e->capture_read = ua_capture_read; controls_setup(u, &u->dir[AS_CAPTURE], true); }
    e->control_put = ua_control_put;
    if (play) log_dir(u, &u->dir[AS_PLAY], "playback");
    if (record) log_dir(u, &u->dir[AS_CAPTURE], "capture");
    instances[slot] = u;                               /* before sound_card_add: id uniqueness, worker service */
    u->card = sound_card_add(e, &u->ring, record ? &u->capture_ring : NULL);
    if (u->card < 0) {
        instances[slot] = NULL;
        dir_release(u, &u->dir[AS_PLAY]); dir_release(u, &u->dir[AS_CAPTURE]); arena_free(memory, sizeof(struct uaudio)); return;
    }
    device->class_driver[0] = u;
}
void uaudio_detach(struct usb_device* device) {
    struct uaudio* u = device->class_driver[0];
    if (!u) return;
    sound_card_remove(u->card);                        /* engines stopped, open streams disconnected */
    u->gone = true; u->dir[0].running = u->dir[1].running = false;
    dir_release(u, &u->dir[AS_PLAY]); dir_release(u, &u->dir[AS_CAPTURE]);   /* commands only if still plugged */
    for (unsigned i = 0; i < UA_MAX_DEVICES; i++) if (instances[i] == u) instances[i] = NULL;
    device->class_driver[0] = NULL;
    arena_free((uint64_t)u, sizeof(struct uaudio));
}
/* Worker: control writes requested by ALSA (volume/mute), rings of stopped directions emptied. */
bool uaudio_service(void) {
    bool work = false;
    for (unsigned i = 0; i < UA_MAX_DEVICES; i++) {
        struct uaudio* u = instances[i];
        if (!u || u->gone) continue;
        while (u->pending) {
            unsigned index = (unsigned)__builtin_ctz(u->pending); u->pending &= ~(1U << index);
            control_write(u, index); work = true;
            if (!instances[i] || u->gone) break;
        }
        for (unsigned k = 0; k < 2 && instances[i] == u; k++) {
            struct ua_dir* d = &u->dir[k];
            if (!d->flush || d->running) { d->flush = false; continue; }
            d->flush = false; work = true; d->flushing = true;   /* ua_start waits for this (it yields) */
            if (d->data.configured) usb_pipe_stop(&d->data);
            if (d->feedback.configured) usb_pipe_stop(&d->feedback);
            d->inflight = d->fb_inflight = 0; d->flushing = false;
        }
    }
    return work;
}
/* Counters for diagnostics (/proc/asound-like report in usbclass.c). */
bool uaudio_stats(unsigned index, unsigned* card, uint64_t stats[6]) {
    if (index >= UA_MAX_DEVICES || !instances[index]) return false;
    struct uaudio* u = instances[index]; *card = (unsigned)u->card;
    stats[0] = u->dir[0].packets; stats[1] = u->dir[0].missed; stats[2] = u->dir[1].packets; stats[3] = u->dir[1].missed;
    stats[4] = u->dir[0].q16; stats[5] = u->dir[0].fb_seen;
    return true;
}
