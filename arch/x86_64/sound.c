/* Native PCM playback behind the Linux ALSA kernel ABI (/dev/snd/controlC0, pcmC0D0p): the hardware-independent
   ALSA and mixing layer. The hardware engine (sound_hw.h) is AC'97 (ac97.c, used whenever an ICH AC'97 is present) or
   Intel High Definition Audio (hda.c, any PCI 04/03 controller: QEMU, VMware, real PCs).
   Up to STREAMS clients share pcmC0D0p (one substream each, like a multi-subdevice card). A client's committed frames
   are summed into an int32 ring at their mapped device position as soon as they are committed (writei or an
   mmap appl_ptr update) and clipped into the S16 DMA ring, so one client is bit-exact and needs no kernel timer.
   The engine plays the ring only up to the furthest committed frame (AC'97: the BDL halts at LVI) or loops over it
   while frames behind the DMA are cleared (HDA), so when every client runs dry the device plays silence instead of
   stale audio. hw_ptr comes from the engine's DMA position; no IRQ is used: state is refreshed on ioctl/poll and by
   sound_poll() from the scheduler loop.
   Capture (pcmC0D0c, when the engine has an input path): the input DMA writes a cyclic capture ring; every capture
   client maps device frames from the moment it started onto its own buffer, and sync copies the newly captured frames
   in (readi/read()/mmap all see them), so several recorders share one input. Overrun = avail reaching stop_threshold.
   Boot option snd.test-capture=counter|tone replaces the captured samples with a known pattern (left/right = low/high
   16 bits of the device frame number, or a 1 kHz sine) to test the path above the hardware end to end. Mixer controls
   (capture volume/switch, mic boost) come from the engine as ALSA control elements.
   Cards: card 0 is the PCI engine (AC'97/HDA) when there is one; USB Audio Class devices (uaudio.c) add and remove
   further cards at runtime (sound_card_add/remove, lowest free index, so a lone USB headset is card 0). Each card has
   its own engine, streams, mixer ring and capture ring; device nodes are numbered card * 3 + (0 controlCn, 1 pcmCnD0p,
   2 pcmCnD0c) and every entry point selects the card (`cur`) before the single-card code below runs; a stream
   re-selects its card after anything that may sleep (waits, user copies that fault). A removed card's open streams
   report -ENODEV (state DISCONNECTED) until they are closed; then its slot is freed. */
#include "native.h"
#include "sound_abi.h"
#include "sound_hw.h"
#include "power.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
uint64_t storage_alloc(uint64_t bytes);              /* storage_alloc.c: counted, outside the per-program audit */
void storage_free(uint64_t address, uint64_t bytes);

#define RATE SOUND_RATE
#define CHANNELS SOUND_CHANNELS
#define FRAME_BYTES SOUND_FRAME_BYTES
#define BUFFER_BYTES_MAX (256U * 1024U)
#define PERIOD_BYTES_MIN 256U
#define PERIOD_BYTES_MAX (128U * 1024U)
#define MAX_PAGES (BUFFER_BYTES_MAX / 4096U)
#define STREAMS 8U
#define RING_FRAMES SOUND_RING_FRAMES   /* > largest client buffer (65536 frames) + MARGIN */
#define MARGIN_FRAMES 2048U   /* ~43 ms: QEMU may already be copying this far beyond the observed DMA position */
#define UINT_MAX32 0xFFFFFFFFU
#define HW_INFO (NX_INFO_MMAP | NX_INFO_MMAP_VALID | NX_INFO_INTERLEAVED | NX_INFO_BLOCK_TRANSFER | NX_INFO_PAUSE | NX_INFO_PERFECT_DRAIN)
#define POLL_IN 1U
#define POLL_OUT 4U
#define POLL_ERR 8U
enum { E_INTR = -4, E_IO = -5, E_AGAIN = -11, E_NOMEM = -12, E_FAULT = -14, E_BUSY = -16, E_NXIO = -6, E_NOENT = -2,
       E_INVAL = -22, E_NOTTY = -25, E_PIPE = -32, E_NOSYS = -38, E_BADFD = -77 };

struct stream {
    int state; unsigned index, access, waiters; bool used, armed, linked, capture;
    struct memory_file* buffer; uint64_t pages[MAX_PAGES]; unsigned page_count;
    uint64_t buffer_size, period_size, periods, boundary;
    uint64_t hw_ptr, appl_ptr, avail_min, start_threshold, stop_threshold, silence_threshold, silence_size, avail_max;
    int tstamp_mode, tstamp_type; unsigned period_step;
    uint64_t trigger_ns, update_ns, wake_frames;
    uint64_t client_base, slave_base, mixed; /* while linked: device position of client frame p = slave_base + (p - client_base) */
    uint64_t writes, xruns, starts, played;
    struct card_state* owner;
};
/* Device timeline (absolute frames): played = DMA read position, queued = end of what the engine was given, cleared =
   ring zeroed below. idle_ns: since when a cyclic engine has had nothing to play. */
struct mix_state {
    int16_t* dst; int32_t* sum; uint64_t dst_physical;
    uint64_t played, queued, cleared;
    bool engine, halted, notifying; uint64_t read_ns, halt_ns, halt_pos, idle_ns, next_poll_ns, restarts;
};
static unsigned unknown_logged;
/* Capture timeline: written = absolute device frames captured; the run that started at run_base writes frame d to
   capture-ring index (d - run_base) % RING_FRAMES. test: 0 off, 1 counter pattern, 2 1 kHz tone. */
uint64_t sound_capture_ring;
struct cap_state {
    int16_t* ring; bool engine; unsigned test; uint64_t run_base, written, read_ns, idle_ns, overruns;
};
struct card_state { bool present, gone, owned; uint64_t generation; struct sound_engine hw_; struct stream streams_[STREAMS]; struct mix_state mix_; struct cap_state cap_; };
static struct card_state cards[SOUND_CARDS], *cur = &cards[0];
static uint64_t card_generations;
void (*sound_cards_changed)(unsigned card, bool added);   /* other listeners (optional) */
static void card_nodes_update(unsigned card, bool added);
#define hw (cur->hw_)
#define streams (cur->streams_)
#define mix (cur->mix_)
#define cap (cur->cap_)
static unsigned capture_test_pattern;
#ifndef SOUND_TRACE
#define SOUND_TRACE 0
#endif
/* Debug aid (SOUND_TRACE=1): events go to an in-memory log whose record 0 is a header {magic, count}; the host reads it
   from a live guest with QMP pmemsave at the logged physical `buffer=` address (contiguous, may lie above 4 GiB; `virt=`
   is the direct-map address for QMP memsave), so tracing adds no serial-port exits. Codes >= 0xF0 are internal events:
   F0 poll, F1 stop, F2 engine start, F3 LVI append, F4 open, F5 close, F6 not-ready poll, F7 link, F8 unlink. */
struct trace_record { uint32_t us, hwpos, appl, queued, extra; int32_t result; uint8_t command, state, flags, stream; };
static struct trace_record* trace_log; static unsigned trace_count; static uint64_t trace_base;
static void log_hex(const char* label, uint64_t value) {
    char text[19] = "0x"; for (int i = 0; i < 16; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (60 - 4 * i)) & 15];
    text[18] = 0; vga_print(label); vga_print(text);
}
void sound_log_hex(const char* label, uint64_t value) { log_hex(label, value); }
static void trace(const struct stream* s, uint32_t command, int64_t result, uint64_t extra) {
    if (!SOUND_TRACE) return;
    if (!trace_log) {
        trace_log = (struct trace_record*)arena_alloc(65537 * sizeof(struct trace_record)); trace_base = native_clock_ns();
        if (!trace_log) return;
        trace_log++; log_hex("[64] SOUND trace buffer=", virt_to_phys(trace_log - 1)); log_hex(" virt=", (uint64_t)(trace_log - 1)); vga_print("\n");
    }
    if (trace_count >= 65536) return;
    trace_log[trace_count++] = (struct trace_record){(uint32_t)((native_clock_ns() - trace_base) / 1000), s ? (uint32_t)s->hw_ptr : (uint32_t)mix.played,
        s ? (uint32_t)s->appl_ptr : 0, (uint32_t)mix.queued, (uint32_t)extra, (int32_t)result, (uint8_t)(command & 0xFF), s ? (uint8_t)s->state : 0xFF,
        (uint8_t)(mix.engine | mix.halted << 1), s ? (uint8_t)s->index : 0xFF};
    trace_log[-1].us = 0x53444E54U; trace_log[-1].hwpos = trace_count;
}
bool sound_available(void) { return cards[0].present; }
bool sound_capture_available(void) { return cards[0].present && cards[0].hw_.capture_start; }
/* Node card * 3 + kind exists: the card is present and has that direction. */
bool sound_node_available(unsigned node) {
    const struct card_state* c = node / 3 < SOUND_CARDS ? &cards[node / 3] : NULL;
    return c && c->present && (node % 3 == 0 || (node % 3 == 1 ? c->hw_.start != NULL : c->hw_.capture_start != NULL));
}
bool sound_card_present(unsigned index) { return index < SOUND_CARDS && cards[index].present; }
void* sound_engine_context(void) { return cur->hw_.context; }
const struct sound_control* sound_engine_control(unsigned index) { return index < cur->hw_.controls ? &cur->hw_.control[index] : NULL; }
/* Select the card of a device node; returns the node kind (0 control, 1 playback, 2 capture). */
static unsigned select_card(unsigned node) { cur = &cards[node / 3 < SOUND_CARDS ? node / 3 : 0]; return node % 3; }
static void buffer_selftest(void);
static const struct pm_ops sound_pm;   /* system sleep (below) */
/* AC'97 whenever one is present (the QEMU desktop launcher's device), otherwise Intel HD Audio. */
bool sound_init(void) {
    uint64_t dst = dma32_alloc(RING_FRAMES * FRAME_BYTES), sum = arena_alloc(RING_FRAMES * CHANNELS * 4ULL);
    uint64_t capture = dma32_alloc(RING_FRAMES * FRAME_BYTES);
    uint64_t dst_physical = dst ? virt_to_phys((void*)dst) : 0;
    capture_test_pattern = boot_option("snd.test-capture=counter") ? 1 : boot_option("snd.test-capture=tone") ? 2 : 0;
    cur = &cards[0];
    sound_capture_ring = capture ? virt_to_phys((void*)capture) : 0;
    if (!dst || !sum || !(ac97_probe(&hw, dst_physical) || hda_probe(&hw, dst_physical))) {
        arena_free(dst, RING_FRAMES * FRAME_BYTES); arena_free(sum, RING_FRAMES * CHANNELS * 4ULL); arena_free(capture, RING_FRAMES * FRAME_BYTES);
        memset(&hw, 0, sizeof(hw)); sound_capture_ring = 0; return false;
    }
    if (hw.capture_start) {
        cap.ring = (int16_t*)capture;
        cap.test = capture_test_pattern;
        vga_print(cap.test ? "[64] SOUND capture device ready (test pattern)\n" : "[64] SOUND capture device ready\n");
    } else { arena_free(capture, RING_FRAMES * FRAME_BYTES); sound_capture_ring = 0; }
    mix.dst = (int16_t*)dst; mix.dst_physical = dst_physical; mix.sum = (int32_t*)sum;
    for (unsigned i = 0; i < STREAMS; i++) streams[i].index = i;
    cur->present = true; cur->generation = ++card_generations; buffer_selftest();
    pm_register(&sound_pm);
    return true;
}
/* A USB Audio Class device (uaudio.c, USB worker): a new card at the lowest free index, with its own mixer and capture
   rings (CPU-copied by the engine: *playback is the S16 stereo mixer ring the engine reads, *capture the ring it
   fills). Returns the card index or -1. */
int sound_card_add(const struct sound_engine* engine, int16_t** playback, int16_t** capture) {
    unsigned index = 0;
    while (index < SOUND_CARDS && (cards[index].present || cards[index].gone)) index++;
    if (index >= SOUND_CARDS) { vga_print("[64] SOUND no free card slot\n"); return -1; }
    struct card_state* c = &cards[index];
    uint64_t dst = storage_alloc(RING_FRAMES * FRAME_BYTES), sum = storage_alloc(RING_FRAMES * CHANNELS * 4ULL);   /* counted: device-lifetime */
    uint64_t in = engine->capture_start ? storage_alloc(RING_FRAMES * FRAME_BYTES) : 0;
    if (!dst || !sum || (engine->capture_start && !in)) {
        storage_free(dst, RING_FRAMES * FRAME_BYTES); storage_free(sum, RING_FRAMES * CHANNELS * 4ULL); storage_free(in, RING_FRAMES * FRAME_BYTES);
        vga_print("[64] SOUND no memory for a card\n"); return -1;
    }
    memset(c, 0, sizeof(*c)); c->hw_ = *engine; c->owned = true; c->generation = ++card_generations;
    c->mix_.dst = (int16_t*)dst; c->mix_.sum = (int32_t*)sum; c->cap_.ring = (int16_t*)in; c->cap_.test = capture_test_pattern;
    memset((void*)dst, 0, RING_FRAMES * FRAME_BYTES); memset((void*)sum, 0, RING_FRAMES * CHANNELS * 4ULL);
    if (in) memset((void*)in, 0, RING_FRAMES * FRAME_BYTES);
    for (unsigned i = 0; i < STREAMS; i++) c->streams_[i].index = i;
    c->present = true; *playback = c->mix_.dst; if (capture) *capture = c->cap_.ring;
    char digit[2] = {(char)('0' + index), 0};
    vga_print("[64] SOUND card "); vga_print(digit); vga_print(" added: "); vga_print(c->hw_.longname); vga_print("\n");
    event_source_changed(c, POLL_IN | POLL_OUT);
    card_nodes_update(index, true);
    if (sound_cards_changed) sound_cards_changed(index, true);
    return (int)index;
}
static void card_free(struct card_state* c) {
    if (c->owned) {
        storage_free((uint64_t)c->mix_.dst, RING_FRAMES * FRAME_BYTES); storage_free((uint64_t)c->mix_.sum, RING_FRAMES * CHANNELS * 4ULL);
        if (c->cap_.ring) storage_free((uint64_t)c->cap_.ring, RING_FRAMES * FRAME_BYTES);
    }
    unsigned index[STREAMS]; for (unsigned i = 0; i < STREAMS; i++) index[i] = c->streams_[i].index;
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < STREAMS; i++) c->streams_[i].index = index[i];
}
/* The device is gone (USB worker): engines stopped, open streams disconnected (-ENODEV); the slot is freed once every
   stream is closed (sound_close). */
void sound_card_remove(int index) {
    if (index < 0 || index >= (int)SOUND_CARDS || !cards[index].present) return;
    struct card_state* saved = cur; cur = &cards[index];
    if (mix.engine) hw.stop();
    if (cap.engine) hw.capture_stop();
    mix.engine = cap.engine = false; cur->present = false;
    bool open = false;
    for (unsigned i = 0; i < STREAMS; i++) if (streams[i].used) {
        struct stream* s = &streams[i]; open = true; s->linked = false; s->state = NX_STATE_DISCONNECTED; task_wake_channel(s);
    }
    char digit[2] = {(char)('0' + index), 0};
    vga_print("[64] SOUND card "); vga_print(digit); vga_print(" removed\n");
    event_source_changed(streams, POLL_IN | POLL_OUT | POLL_ERR); event_source_changed(cur, POLL_IN | POLL_OUT | POLL_ERR);
    if (open) cur->gone = true; else card_free(cur);
    cur = saved == &cards[index] && !open ? &cards[0] : saved;
    card_nodes_update((unsigned)index, false);
    if (sound_cards_changed) sound_cards_changed((unsigned)index, false);
}

/* ---------- client positions (mod boundary) ---------- */
static uint64_t fwd(const struct stream* s, uint64_t position, uint64_t frames) { position += frames; return position >= s->boundary ? position - s->boundary : position; }
static uint64_t dist(const struct stream* s, uint64_t a, uint64_t b) { return a >= b ? a - b : a + s->boundary - b; }
static uint64_t avail(const struct stream* s) {
    if (s->capture) return dist(s, s->hw_ptr, s->appl_ptr);                 /* captured, not yet read */
    int64_t value = (int64_t)(s->hw_ptr + s->buffer_size) - (int64_t)s->appl_ptr;
    if (value < 0) value += (int64_t)s->boundary; else if ((uint64_t)value >= s->boundary) value -= (int64_t)s->boundary;
    return (uint64_t)value;
}
static int64_t hw_avail(const struct stream* s) { return (int64_t)s->buffer_size - (int64_t)avail(s); }
static bool running(const struct stream* s) { return s->state == NX_STATE_RUNNING || s->state == NX_STATE_DRAINING; }
static uint64_t device_position(const struct stream* s, uint64_t position) { return s->slave_base + dist(s, position, s->client_base); }
/* Frames between the DMA position and this client's first mapped frame (a newly started client waits MARGIN). */
static uint64_t lead(const struct stream* s) { return s->linked && s->slave_base > mix.played ? s->slave_base - mix.played : 0; }
static int64_t delay(const struct stream* s) { return s->capture ? (int64_t)avail(s) : hw_avail(s) + (int64_t)lead(s); }
static bool any_linked(void) { for (unsigned i = 0; i < STREAMS; i++) if (streams[i].linked && !streams[i].capture) return true; return false; }

/* ---------- DMA engine (hardware in sound_hw.h) ---------- */
static void engine_reset(void) {
    hw.stop();
    mix.engine = mix.halted = false; mix.idle_ns = 0;
}
/* System sleep (power.h): the DMA stops; after the power loss the engine brings its controller back and the next
   feed restarts playback from the mixer timeline (clients keep their positions). */
static void capture_suspend(void);
static int sound_pm_suspend(void* context, unsigned state) { (void)context; (void)state; cur = &cards[0]; if (cur->present && !cur->owned) { engine_reset(); capture_suspend(); } return 0; }
static void sound_pm_resume(void* context, unsigned state) {
    (void)context; (void)state;
    cur = &cards[0];
    if (!cur->present || cur->owned || !hw.resume) return;
    if (!hw.resume()) vga_print("[64] SOUND: the audio controller did not come back after resume\n");
    mix.halt_ns = native_clock_ns(); mix.halt_pos = mix.played > mix.queued ? mix.played : mix.queued;
}
static const struct pm_ops sound_pm = {"sound", sound_pm_suspend, sound_pm_resume, NULL};
/* Device position from the engine; a halted engine has played everything it was given. A cyclic engine never halts
   on its own: a false read there is a DMA error, so it is stopped and the next feed starts it afresh. */
static void engine_read(uint64_t now) {
    uint64_t position = mix.queued;
    bool moving = hw.read(&position);
    mix.read_ns = now;
    if (!moving && hw.cyclic) { engine_reset(); mix.halt_ns = now; mix.halt_pos = mix.played; return; }
    if (!moving && !mix.halted) { mix.halted = true; mix.halt_ns = now; mix.halt_pos = mix.queued; }
    if (position > mix.played && (hw.cyclic || position <= mix.queued)) mix.played = position;
}
static void ring_clear(void) {
    uint64_t from = mix.cleared, to = mix.played;
    if (to <= from) return;
    if (to - from > RING_FRAMES) from = to - RING_FRAMES;
    while (from < to) {
        uint64_t index = from % RING_FRAMES, frames = to - from;
        if (frames > RING_FRAMES - index) frames = RING_FRAMES - index;
        memset(mix.dst + index * CHANNELS, 0, frames * FRAME_BYTES); memset(mix.sum + index * CHANNELS, 0, frames * CHANNELS * 4);
        if (hw.flush) hw.flush(mix.dst + index * CHANNELS, frames * FRAME_BYTES);
        from += frames;
    }
    mix.cleared = to;
}
/* Refresh the device position. While halted or idle the device clock keeps running (the codec plays silence), as on
   real hardware, so clients that stop feeding it reach their stop_threshold. */
static void engine_update(bool force) {
    uint64_t now = native_clock_ns();
    if (mix.engine && !mix.halted && (force || now - mix.read_ns >= 200000)) engine_read(now);
    if (!mix.engine && !mix.halt_ns) { mix.halt_ns = now; mix.halt_pos = mix.played > mix.queued ? mix.played : mix.queued; }
    if (!mix.engine || mix.halted) {
        uint64_t virtual_position = mix.halt_pos + (now - mix.halt_ns) / 1000 * RATE / 1000000;
        if (virtual_position > mix.played) mix.played = virtual_position;
        /* Keep a halted voice (it outputs silence) for 2 s so short pauses and xrun recoveries do not churn it. */
        if (mix.engine && !any_linked() && now - mix.halt_ns >= 2000000000ULL) engine_reset();
    } else if (hw.cyclic) {
        /* A looping DMA plays the cleared ring (silence) once every client ran dry; release it after 2 s idle. */
        if (any_linked() || mix.played < mix.queued) mix.idle_ns = 0;
        else if (!mix.idle_ns) mix.idle_ns = now;
        else if (now - mix.idle_ns >= 2000000000ULL) { engine_reset(); mix.halt_ns = now; mix.halt_pos = mix.played; }
    }
    ring_clear();
}
static bool engine_running(void) { return mix.engine && !mix.halted; }
/* Let the engine play up to the furthest mixed frame of any client; restart a halted or stopped one. */
static void engine_feed(void) {
    uint64_t target = mix.queued;
    for (unsigned i = 0; i < STREAMS; i++) {
        const struct stream* s = &streams[i];
        if (s->linked && !s->capture) { uint64_t end = device_position(s, s->mixed); if (end > target) target = end; }
    }
    if (target <= mix.queued && mix.engine) return;
    if (!mix.engine) {
        if (target <= mix.played) return;
        engine_reset();
        if (mix.queued < mix.played) mix.queued = mix.played;
        if (!hw.start(&mix.queued, target)) return;
        mix.engine = true; mix.halted = false; mix.halt_ns = 0; mix.read_ns = native_clock_ns();
        trace(NULL, 0xF2, (int64_t)mix.queued, target); return;
    }
    if (mix.halted && mix.queued < mix.played) mix.queued = mix.played; /* skip the silence the device already "played" */
    if (!hw.extend(&mix.queued, target)) return;
    if (mix.halted) { mix.halted = false; mix.halt_ns = 0; mix.restarts++; }
    trace(NULL, 0xF3, (int64_t)mix.queued, target);
}

/* ---------- capture engine ---------- */
static const int16_t sine_1khz[48] = {0, 1069, 2120, 3135, 4096, 4987, 5793, 6499, 7094, 7568, 7913, 8122, 8192, 8122, 7913, 7568,
    7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069, 0, -1069, -2120, -3135, -4096, -4987, -5793, -6499, -7094, -7568, -7913, -8122,
    -8192, -8122, -7913, -7568, -7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069};
static bool capture_linked(void) { for (unsigned i = 0; i < STREAMS; i++) if (streams[i].linked && streams[i].capture) return true; return false; }
static void capture_engine_stop(void) { if (cap.engine) { hw.capture_stop(); cap.engine = false; } }
static void notify(struct stream* s);
/* Refresh cap.written from the input DMA (at most every 200 us unless forced). */
static void capture_update(bool force) {
    if (!cap.engine) return;
    uint64_t now = native_clock_ns(), position = 0;
    if (!force && now - cap.read_ns < 200000) return;
    cap.read_ns = now;
    if (!hw.capture_read(&position)) {                   /* DMA error or halt: the recorders get an xrun and restart it */
        vga_print("[64] SOUND capture DMA stopped\n"); capture_engine_stop();
        for (unsigned i = 0; i < STREAMS; i++) if (streams[i].linked && streams[i].capture) {
            struct stream* s = &streams[i]; s->linked = false; s->state = NX_STATE_XRUN; s->xruns++; s->trigger_ns = now; notify(s);
        }
        return;
    }
    if (cap.run_base + position > cap.written) cap.written = cap.run_base + position;
}
/* System sleep (sound_pm_suspend): the input DMA stops with the power; linked recorders get an xrun (EPIPE, as
   ALSA's snd_pcm_recover handles after a suspend) and restart capture on the resumed controller. */
static void capture_suspend(void) {
    if (!cap.engine) return;
    capture_engine_stop();
    uint64_t now = native_clock_ns();
    for (unsigned i = 0; i < STREAMS; i++) if (streams[i].linked && streams[i].capture) {
        struct stream* s = &streams[i]; s->linked = false; s->state = NX_STATE_XRUN; s->xruns++; s->trigger_ns = now; notify(s);
    }
}
static bool capture_engine_start(void) {
    if (cap.engine) return true;
    if (!hw.capture_start || !hw.capture_start()) return false;
    cap.engine = true; cap.run_base = cap.written; cap.read_ns = native_clock_ns(); cap.idle_ns = 0;
    return true;
}
/* Copy device frames [device, device + frames) into the client buffer at client position `position`. */
static void capture_copy(struct stream* s, uint64_t device, uint64_t position, uint64_t frames) {
    while (frames) {
        uint64_t offset = position % s->buffer_size, byte = offset * FRAME_BYTES, index = (device - cap.run_base) % RING_FRAMES, n = frames;
        if (n > s->buffer_size - offset) n = s->buffer_size - offset;
        if (n > (4096 - byte % 4096) / FRAME_BYTES) n = (4096 - byte % 4096) / FRAME_BYTES;
        if (n > RING_FRAMES - index) n = RING_FRAMES - index;
        int16_t* out = (int16_t*)(s->pages[byte / 4096] + byte % 4096);
        if (!cap.test) {
            if (hw.capture_flush) hw.capture_flush(cap.ring + index * CHANNELS, n * FRAME_BYTES);
            memcpy(out, cap.ring + index * CHANNELS, n * FRAME_BYTES);
        } else for (uint64_t i = 0; i < n; i++) {
            uint64_t d = device + i;
            if (cap.test == 1) { out[2 * i] = (int16_t)(uint16_t)d; out[2 * i + 1] = (int16_t)(uint16_t)(d >> 16); }
            else out[2 * i] = out[2 * i + 1] = sine_1khz[d % 48];
        }
        device += n; position = fwd(s, position, n); frames -= n;
    }
}
static void set_hw_ptr(struct stream* s, uint64_t position, uint64_t now);
/* Bring a linked capture client's hw_ptr up to the captured position, copying the new frames in. Frames older than
   the client buffer (an overrun in progress) are skipped: the xrun check stops the stream anyway. */
static void capture_sync(struct stream* s) {
    capture_update(false);
    uint64_t have = device_position(s, s->hw_ptr);
    if (!s->linked || cap.written <= have) return;
    uint64_t frames = cap.written - have, skip = frames > s->buffer_size ? frames - s->buffer_size : 0;
    if (frames - skip > RING_FRAMES - 4096) skip = frames - (RING_FRAMES - 4096);
    uint64_t position = fwd(s, s->hw_ptr, skip % s->boundary);
    capture_copy(s, have + skip, position, frames - skip);
    set_hw_ptr(s, fwd(s, s->hw_ptr, frames % s->boundary), native_clock_ns());
}

/* ---------- mixing ---------- */
static int16_t clip(int32_t value) { return value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value; }
/* Add (sign +1) or remove (-1) client frames [from, to) at their device positions. Frames the DMA already read are
   skipped; removal also spares the MARGIN QEMU may be copying right now. */
static void mix_frames(const struct stream* s, uint64_t from, uint64_t to, int sign) {
    uint64_t floor = mix.played + (sign < 0 && engine_running() ? MARGIN_FRAMES : 0);
    while (from != to) {
        uint64_t offset = from % s->buffer_size, frames = dist(s, to, from), device = device_position(s, from);
        if (frames > s->buffer_size - offset) frames = s->buffer_size - offset;
        uint64_t byte = offset * FRAME_BYTES, index = device % RING_FRAMES;
        if (frames > (4096 - byte % 4096) / FRAME_BYTES) frames = (4096 - byte % 4096) / FRAME_BYTES;
        if (frames > RING_FRAMES - index) frames = RING_FRAMES - index;
        uint64_t skip = device >= floor ? 0 : floor - device > frames ? frames : floor - device;
        const int16_t* source = (const int16_t*)(s->pages[byte / 4096] + byte % 4096) + skip * CHANNELS;
        int32_t* sum = mix.sum + (index + skip) * CHANNELS; int16_t* dst = mix.dst + (index + skip) * CHANNELS;
        for (uint64_t i = 0; i < (frames - skip) * CHANNELS; i++) { int32_t v = sum[i] + sign * source[i]; sum[i] = v; dst[i] = clip(v); }
        if (hw.flush && frames > skip) hw.flush(dst, (frames - skip) * FRAME_BYTES);
        from = fwd(s, from, frames);
    }
}
/* Mix newly committed frames of a running client; frames already late (behind the DMA) are dropped. */
static void commit(struct stream* s) {
    if (s->capture) return;
    if (!s->linked || dist(s, s->appl_ptr, s->hw_ptr) > s->buffer_size) return;
    if (dist(s, s->mixed, s->hw_ptr) > s->buffer_size) s->mixed = s->hw_ptr;
    if (dist(s, s->appl_ptr, s->mixed) > s->buffer_size || s->mixed == s->appl_ptr) return;
    mix_frames(s, s->mixed, s->appl_ptr, 1); s->mixed = s->appl_ptr;
}
static void link_stream(struct stream* s) {
    if (s->capture) {                                    /* record from now on */
        if (!capture_engine_start()) { vga_print("[64] SOUND capture DMA failed to start\n"); return; }
        capture_update(true);
        s->client_base = s->hw_ptr; s->slave_base = cap.written; s->linked = true; s->update_ns = native_clock_ns(); s->starts++;
        trace(s, 0xF7, 1, s->slave_base); return;
    }
    engine_update(true);
    s->client_base = s->mixed = s->hw_ptr; s->slave_base = mix.played + (engine_running() ? MARGIN_FRAMES : 0);
    s->linked = true; s->update_ns = native_clock_ns(); s->starts++;
    trace(s, 0xF7, 0, s->slave_base);
    commit(s); engine_feed();
}
/* Withdraw the client's not-yet-copied frames; returns the first withdrawn frame (frames before it still play). */
static uint64_t unlink_stream(struct stream* s) {
    if (!s->linked) return s->hw_ptr;
    if (s->capture) { s->linked = false; trace(s, 0xF8, 1, 0); return s->hw_ptr; }
    uint64_t floor = mix.played + (engine_running() ? MARGIN_FRAMES : 0), from = s->hw_ptr;
    if (floor > s->slave_base) { uint64_t skip = floor - s->slave_base; if (skip < s->boundary) from = fwd(s, s->client_base, skip); }
    if (dist(s, s->mixed, s->hw_ptr) > s->buffer_size) from = s->hw_ptr;
    else if (dist(s, from, s->hw_ptr) > dist(s, s->mixed, s->hw_ptr)) from = s->mixed;
    if (dist(s, s->mixed, s->hw_ptr) <= s->buffer_size && from != s->mixed) mix_frames(s, from, s->mixed, -1);
    s->linked = false; s->mixed = s->hw_ptr;
    trace(s, 0xF8, 0, 0);
    return from;
}

/* ---------- client state ---------- */
static void set_hw_ptr(struct stream* s, uint64_t position, uint64_t now) {
    if (position == s->hw_ptr) return;
    s->played += dist(s, position, s->hw_ptr); s->hw_ptr = position; s->update_ns = now;
    uint64_t free = avail(s); if (free > s->avail_max) s->avail_max = free;
}
static void sync_stream(struct stream* s) {
    if (!s->linked) return;
    if (s->capture) { capture_sync(s); return; }
    uint64_t position = s->client_base;
    if (mix.played > s->slave_base) { uint64_t frames = mix.played - s->slave_base; position = fwd(s, s->client_base, frames % s->boundary); }
    set_hw_ptr(s, position, native_clock_ns());
}
static void notify(struct stream* s) {
    task_wake_channel(s);
    if (mix.notifying) return;
    mix.notifying = true; s->armed = false; event_source_changed(streams, POLL_OUT | POLL_IN); mix.notifying = false;
}
static void stop(struct stream* s, int state) {
    if (s->linked) { if (!s->capture) engine_update(true); sync_stream(s); unlink_stream(s); }
    s->trigger_ns = native_clock_ns();
    if (state == NX_STATE_XRUN) s->xruns++;
    trace(s, 0xF1, state, s->state);
    s->state = state; notify(s);
}
/* Refresh hw_ptr, then apply ALSA xrun/drain rules; returns -EPIPE when this update ended the stream. */
static int update(struct stream* s, bool force) {
    if (!running(s)) return 0;
    if (s->capture) {
        if (s->state == NX_STATE_DRAINING) { if (!avail(s)) { s->state = NX_STATE_SETUP; notify(s); } return 0; }
        capture_update(force); sync_stream(s);
        if (s->state != NX_STATE_RUNNING) return s->state == NX_STATE_XRUN ? E_PIPE : 0;   /* the input DMA stopped */
        if (avail(s) >= s->stop_threshold) { cap.overruns++; stop(s, NX_STATE_XRUN); return E_PIPE; }
        return 0;
    }
    engine_update(force); sync_stream(s);
    if (s->state == NX_STATE_DRAINING) { if (avail(s) >= s->buffer_size) { stop(s, NX_STATE_SETUP); return E_PIPE; } }
    else if (avail(s) >= s->stop_threshold) { stop(s, NX_STATE_XRUN); return E_PIPE; }
    commit(s); engine_feed();
    return 0;
}
static int start(struct stream* s, int state) {
    s->state = state; s->trigger_ns = native_clock_ns();
    link_stream(s);
    if (s->capture && !s->linked) { s->state = NX_STATE_PREPARED; return E_IO; }    /* the input DMA did not start */
    return update(s, true) == E_PIPE && s->state == NX_STATE_XRUN ? E_PIPE : 0;
}
static uint32_t ready_mask(const struct stream* s) {
    /* an avail_min beyond the buffer could never be met: alsa-lib's rate plugin rounds speech-dispatcher's "buffer - 4"
       up past the 48 kHz buffer, and its poll after an xrun + prepare waited forever */
    uint64_t wake = s->avail_min < s->buffer_size ? s->avail_min : s->buffer_size;
    if (s->capture) switch (s->state) {
    case NX_STATE_RUNNING: case NX_STATE_PREPARED: case NX_STATE_PAUSED: return avail(s) >= wake ? POLL_IN : 0;
    case NX_STATE_DRAINING: return avail(s) ? POLL_IN : POLL_IN | POLL_ERR;
    default: return POLL_IN | POLL_ERR;
    }
    switch (s->state) {
    case NX_STATE_RUNNING: case NX_STATE_PREPARED: case NX_STATE_PAUSED: return avail(s) >= wake ? POLL_OUT : 0;
    case NX_STATE_DRAINING: return 0;
    default: return POLL_OUT | POLL_ERR;
    }
}
/* Sleep (never spin) until woken by sound_poll or the frame deadline; signals end the wait. */
static int wait_frames(struct stream* s, uint64_t frames) {
    uint64_t now = native_clock_ns(), have = avail(s), ns = 1000000;
    /* sound_poll wakes the task once avail reaches `frames` (tickless: sound_next_event schedules that pass); the
       deadline, just after, is the backstop. */
    if (frames > have) { uint64_t need = (frames - have) * 1000000000ULL / RATE; if (need > ns) ns = need > 250000000 ? 250000000 : need + 2000000; }
    s->wake_frames = frames; s->waiters++;
    int error = task_wait_until(s, now + ns);
    cur = s->owner; s->waiters--;
    if (s->state == NX_STATE_DISCONNECTED) return -19;
    return error == -110 ? 0 : error;
}
static void card_poll(void);
void sound_poll(void) {
    struct card_state* saved = cur;
    for (unsigned i = 0; i < SOUND_CARDS; i++) if (cards[i].present) { cur = &cards[i]; card_poll(); }
    cur = saved;
}
static void card_poll(void) {
    uint64_t now = native_clock_ns();
    if (now < mix.next_poll_ns) return;
    mix.next_poll_ns = now + 1000000;
    if (hw.poll) hw.poll(now);
    bool active = false, deferred = mix.engine && hw.full && hw.full();
    for (unsigned i = 0; i < STREAMS; i++) {
        struct stream* s = &streams[i];
        if (!s->used || !running(s) || (!s->armed && !s->waiters && !deferred)) continue;
        active = true;
        int state = s->state; bool armed = s->armed;
        update(s, true);
        trace(s, 0xF0, armed * 100 + s->waiters * 10 + deferred, avail(s));
        if (s->state != state) continue; /* stop() already notified */
        if (s->armed && ready_mask(s)) notify(s);
        else if (s->waiters && avail(s) >= s->wake_frames) task_wake_channel(s);
    }
    /* An input DMA nobody records from is stopped after 2 s (short gaps between recordings keep it running). */
    if (cap.engine) {
        if (capture_linked()) cap.idle_ns = 0;
        else if (!cap.idle_ns) cap.idle_ns = now;
        else if (now - cap.idle_ns >= 2000000000ULL) capture_engine_stop();
        if (cap.engine) capture_update(false);
    }
    if (!active && mix.engine && !any_linked()) engine_update(true); /* let an idle engine finish and release the voice */
    /* A looping DMA must have the frames it passed cleared before it comes round again, whoever is (not) polling. */
    else if (hw.cyclic && mix.engine) engine_update(false);
}
/* tick.c (tickless idle): when sound_poll next has work for the boot CPU, instead of every 1 ms while anything plays
 * (~1000 wake-ups/s). A stream a task waits on (blocking read/write, poll/epoll) needs it when its avail reaches the
 * wake-up, avail_min, stop (xrun) or drain threshold, counted from the last hw_ptr update at the device rate; a full
 * descriptor list (AC'97) before half of what it holds has played; a looping DMA (HD Audio) a ring clear well within
 * its 2.7 s lap; idle engines their 2 s release; a running input DMA reads well within its lap. Streams nobody
 * waits on are brought up to date by their next call. Nothing while no engine runs and no stream waits. */
static uint64_t card_next_event(uint64_t now) {   /* the card `cur` */
    uint64_t next = ~0ULL, floor = mix.next_poll_ns > now ? mix.next_poll_ns : now;
    for (unsigned i = 0; i < STREAMS; i++) {
        const struct stream* s = &streams[i];
        if (!s->used || !running(s) || (!s->armed && !s->waiters)) continue;
        uint64_t want = s->state == NX_STATE_DRAINING ? s->buffer_size : s->stop_threshold;
        uint64_t wake = s->avail_min < s->buffer_size ? s->avail_min : s->buffer_size;   /* ready_mask's threshold */
        if (s->armed) { if (ready_mask(s)) want = 0; else if (wake < want) want = wake; }
        if (s->waiters && s->wake_frames < want) want = s->wake_frames;
        uint64_t have = avail(s), base = s->update_ns && s->update_ns < now ? s->update_ns : now, frames = want > have ? want - have : 0;
        if (frames > 10ULL * RATE) frames = 10ULL * RATE;
        uint64_t at = base + frames * 1000000000ULL / RATE;
        if (at < next) next = at;
    }
    if (mix.engine) {
        uint64_t at = now + 250000000ULL;                                     /* ring clear, idle/halt release */
        if (hw.full && hw.full() && mix.queued > mix.played) {
            uint64_t half = (mix.queued - mix.played) * 500000000ULL / RATE;
            at = now + (half < 1000000ULL ? 1000000ULL : half < 250000000ULL ? half : 250000000ULL);
        }
        if (at < next) next = at;
    }
    if (cap.engine && now + 250000000ULL < next) next = now + 250000000ULL;
    return next == ~0ULL ? next : next < floor ? floor : next;
}
uint64_t sound_next_event(uint64_t now) {   /* every present card (hw2 multi-card), the earliest */
    struct card_state* saved = cur; uint64_t next = ~0ULL;
    for (unsigned i = 0; i < SOUND_CARDS; i++) if (cards[i].present) { cur = &cards[i]; uint64_t at = card_next_event(now); if (at < next) next = at; }
    cur = saved; return next;
}

/* ---------- hw_params refinement (Linux snd_pcm_hw_refine semantics) ---------- */
static bool mask_empty(const struct nx_mask* m) { for (unsigned i = 0; i < 8; i++) if (m->bits[i]) return false; return true; }
static bool mask_test(const struct nx_mask* m, unsigned bit) { return (m->bits[bit / 32] >> (bit % 32)) & 1U; }
static unsigned mask_min(const struct nx_mask* m) { for (unsigned i = 0; i < 256; i++) if (mask_test(m, i)) return i; return 0; }
static bool mask_single(const struct nx_mask* m) {
    unsigned count = 0; for (unsigned i = 0; i < 256; i++) count += mask_test(m, i); return count == 1;
}
static int mask_refine(struct nx_mask* m, const struct nx_mask* v) {
    struct nx_mask old = *m; for (unsigned i = 0; i < 8; i++) m->bits[i] &= v->bits[i];
    if (mask_empty(m)) return E_INVAL;
    return memcmp(&old, m, sizeof(old)) != 0;
}
static void interval_none(struct nx_interval* i) { i->empty = 1; }
static bool interval_checkempty(const struct nx_interval* i) { return i->min > i->max || (i->min == i->max && (i->openmin || i->openmax)); }
static bool interval_single(const struct nx_interval* i) {
    return !i->empty && (i->min == i->max || (i->min + 1 == i->max && (i->openmin || i->openmax)));
}
static uint32_t interval_value(const struct nx_interval* i) { return i->openmin && !i->openmax ? i->max : i->min; }
static int interval_refine(struct nx_interval* i, const struct nx_interval* v) {
    int changed = 0;
    if (i->empty) return E_INVAL;
    if (i->min < v->min) { i->min = v->min; i->openmin = v->openmin; changed = 1; }
    else if (i->min == v->min && !i->openmin && v->openmin) { i->openmin = 1; changed = 1; }
    if (i->max > v->max) { i->max = v->max; i->openmax = v->openmax; changed = 1; }
    else if (i->max == v->max && !i->openmax && v->openmax) { i->openmax = 1; changed = 1; }
    if (!i->integer && v->integer) { i->integer = 1; changed = 1; }
    if (i->integer) {
        if (i->openmin) { i->min++; i->openmin = 0; }
        if (i->openmax) { i->max--; i->openmax = 0; }
    } else if (!i->openmin && !i->openmax && i->min == i->max) i->integer = 1;
    if (interval_checkempty(i)) { interval_none(i); return E_INVAL; }
    return changed;
}
static int interval_refine_first(struct nx_interval* i) {
    uint32_t last_max = i->max;
    if (i->empty) return E_INVAL;
    if (interval_single(i)) return 0;
    i->max = i->min; if (i->openmin) i->max++;
    i->openmax = i->openmax && i->max >= last_max; return 1;
}
static int interval_refine_last(struct nx_interval* i) {
    uint32_t last_min = i->min;
    if (i->empty) return E_INVAL;
    if (interval_single(i)) return 0;
    i->min = i->max; if (i->openmax) i->min--;
    i->openmin = i->openmin && i->min <= last_min; return 1;
}
static uint32_t mul32(uint32_t a, uint32_t b) { return !a ? 0 : UINT_MAX32 / a < b ? UINT_MAX32 : a * b; }
static uint32_t div32(uint32_t a, uint32_t b, uint32_t* r) { if (!b) { *r = 0; return UINT_MAX32; } *r = a % b; return a / b; }
static uint32_t muldiv32(uint32_t a, uint32_t b, uint32_t c, uint32_t* r) {
    uint64_t n = (uint64_t)a * b;
    if (!c) { *r = 0; return UINT_MAX32; }
    *r = (uint32_t)(n % c); n /= c;
    if (n >= UINT_MAX32) { *r = 0; return UINT_MAX32; }
    return (uint32_t)n;
}
static void interval_mul(const struct nx_interval* a, const struct nx_interval* b, struct nx_interval* c) {
    if (a->empty || b->empty) { interval_none(c); return; }
    c->empty = 0; c->min = mul32(a->min, b->min); c->openmin = a->openmin || b->openmin;
    c->max = mul32(a->max, b->max); c->openmax = a->openmax || b->openmax; c->integer = a->integer && b->integer;
}
static void interval_div(const struct nx_interval* a, const struct nx_interval* b, struct nx_interval* c) {
    uint32_t r;
    if (a->empty || b->empty) { interval_none(c); return; }
    c->empty = 0; c->min = div32(a->min, b->max, &r); c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = div32(a->max, b->min, &r);
        if (r) { c->max++; c->openmax = 1; } else c->openmax = a->openmax || b->openmin;
    } else { c->max = UINT_MAX32; c->openmax = 0; }
    c->integer = 0;
}
static void interval_muldivk(const struct nx_interval* a, const struct nx_interval* b, uint32_t k, struct nx_interval* c) {
    uint32_t r;
    if (a->empty || b->empty) { interval_none(c); return; }
    c->empty = 0; c->min = muldiv32(a->min, b->min, k, &r); c->openmin = r || a->openmin || b->openmin;
    c->max = muldiv32(a->max, b->max, k, &r);
    if (r) { c->max++; c->openmax = 1; } else c->openmax = a->openmax || b->openmax;
    c->integer = 0;
}
static void interval_mulkdiv(const struct nx_interval* a, uint32_t k, const struct nx_interval* b, struct nx_interval* c) {
    uint32_t r;
    if (a->empty || b->empty) { interval_none(c); return; }
    c->empty = 0; c->min = muldiv32(a->min, k, b->max, &r); c->openmin = r || a->openmin || b->openmax;
    if (b->min > 0) {
        c->max = muldiv32(a->max, k, b->min, &r);
        if (r) { c->max++; c->openmax = 1; } else c->openmax = a->openmax || b->openmin;
    } else { c->max = UINT_MAX32; c->openmax = 0; }
    c->integer = 0;
}
enum { RULE_FORMAT, RULE_SAMPLE_BITS, RULE_MUL, RULE_DIV, RULE_MULKDIV, RULE_MULDIVK };
struct rule { uint8_t kind, var, dep[2]; uint32_t k; };
#define NONE 0xFF
/* Same rules and order as snd_pcm_hw_constraints_init(). */
static const struct rule rules[] = {
    {RULE_FORMAT, NX_PARAM_FORMAT, {NX_PARAM_SAMPLE_BITS, NONE}, 0},
    {RULE_SAMPLE_BITS, NX_PARAM_SAMPLE_BITS, {NX_PARAM_FORMAT, NX_PARAM_SAMPLE_BITS}, 0},
    {RULE_DIV, NX_PARAM_SAMPLE_BITS, {NX_PARAM_FRAME_BITS, NX_PARAM_CHANNELS}, 0},
    {RULE_MUL, NX_PARAM_FRAME_BITS, {NX_PARAM_SAMPLE_BITS, NX_PARAM_CHANNELS}, 0},
    {RULE_MULKDIV, NX_PARAM_FRAME_BITS, {NX_PARAM_PERIOD_BYTES, NX_PARAM_PERIOD_SIZE}, 8},
    {RULE_MULKDIV, NX_PARAM_FRAME_BITS, {NX_PARAM_BUFFER_BYTES, NX_PARAM_BUFFER_SIZE}, 8},
    {RULE_DIV, NX_PARAM_CHANNELS, {NX_PARAM_FRAME_BITS, NX_PARAM_SAMPLE_BITS}, 0},
    {RULE_MULKDIV, NX_PARAM_RATE, {NX_PARAM_PERIOD_SIZE, NX_PARAM_PERIOD_TIME}, 1000000},
    {RULE_MULKDIV, NX_PARAM_RATE, {NX_PARAM_BUFFER_SIZE, NX_PARAM_BUFFER_TIME}, 1000000},
    {RULE_DIV, NX_PARAM_PERIODS, {NX_PARAM_BUFFER_SIZE, NX_PARAM_PERIOD_SIZE}, 0},
    {RULE_DIV, NX_PARAM_PERIOD_SIZE, {NX_PARAM_BUFFER_SIZE, NX_PARAM_PERIODS}, 0},
    {RULE_MULKDIV, NX_PARAM_PERIOD_SIZE, {NX_PARAM_PERIOD_BYTES, NX_PARAM_FRAME_BITS}, 8},
    {RULE_MULDIVK, NX_PARAM_PERIOD_SIZE, {NX_PARAM_PERIOD_TIME, NX_PARAM_RATE}, 1000000},
    {RULE_MUL, NX_PARAM_BUFFER_SIZE, {NX_PARAM_PERIOD_SIZE, NX_PARAM_PERIODS}, 0},
    {RULE_MULKDIV, NX_PARAM_BUFFER_SIZE, {NX_PARAM_BUFFER_BYTES, NX_PARAM_FRAME_BITS}, 8},
    {RULE_MULDIVK, NX_PARAM_BUFFER_SIZE, {NX_PARAM_BUFFER_TIME, NX_PARAM_RATE}, 1000000},
    {RULE_MULDIVK, NX_PARAM_PERIOD_BYTES, {NX_PARAM_PERIOD_SIZE, NX_PARAM_FRAME_BITS}, 8},
    {RULE_MULDIVK, NX_PARAM_BUFFER_BYTES, {NX_PARAM_BUFFER_SIZE, NX_PARAM_FRAME_BITS}, 8},
    {RULE_MULKDIV, NX_PARAM_PERIOD_TIME, {NX_PARAM_PERIOD_SIZE, NX_PARAM_RATE}, 1000000},
    {RULE_MULKDIV, NX_PARAM_BUFFER_TIME, {NX_PARAM_BUFFER_SIZE, NX_PARAM_RATE}, 1000000},
};
#define RULE_COUNT (sizeof(rules) / sizeof(rules[0]))
static struct nx_interval* param_interval(struct nx_hw_params* p, unsigned var) { return &p->intervals[var - NX_PARAM_SAMPLE_BITS]; }
/* Driver constraints: S16_LE stereo 48 kHz, interleaved RW or mmap; BUFFER_FRAMES_MIN..256 KiB buffer, integral periods.
   BUFFER_FRAMES_MIN (8192 = 170.7 ms) is a robustness floor for this cooperative kernel: Chrome asks for 85 ms, and
   every Chrome xrun traced under load followed a 104-164 ms gap between its writes. Lower it once kernel stalls shrink. */
#define BUFFER_FRAMES_MIN 8192U
static void constraint(unsigned var, struct nx_interval* out) {
    *out = (struct nx_interval){0, UINT_MAX32, 0, 0, 0, 0};
    switch (var) {
    case NX_PARAM_CHANNELS: *out = (struct nx_interval){CHANNELS, CHANNELS, 0, 0, 1, 0}; break;
    case NX_PARAM_RATE: *out = (struct nx_interval){RATE, RATE, 0, 0, 1, 0}; break;
    case NX_PARAM_SAMPLE_BITS: case NX_PARAM_FRAME_BITS: case NX_PARAM_PERIOD_SIZE: out->integer = 1; break;
    case NX_PARAM_BUFFER_SIZE: out->min = BUFFER_FRAMES_MIN; out->max = BUFFER_BYTES_MAX / FRAME_BYTES; out->integer = 1; break;
    case NX_PARAM_PERIOD_BYTES: out->min = PERIOD_BYTES_MIN; out->max = PERIOD_BYTES_MAX; break;
    case NX_PARAM_PERIODS: out->min = 2; out->max = 1024; out->integer = 1; break;
    case NX_PARAM_BUFFER_BYTES: out->min = BUFFER_FRAMES_MIN * FRAME_BYTES; out->max = BUFFER_BYTES_MAX; out->integer = 1; break;
    }
}
static int apply_rule(struct nx_hw_params* p, const struct rule* r) {
    struct nx_interval t = {0, 0, 0, 0, 0, 0};
    if (r->kind == RULE_FORMAT) {
        /* Only S16_LE (16-bit physical width) can survive the format constraint. */
        const struct nx_interval* bits = param_interval(p, NX_PARAM_SAMPLE_BITS);
        struct nx_mask keep; memset(&keep, 0xFF, sizeof(keep));
        if (mask_test(&p->masks[NX_PARAM_FORMAT], NX_FORMAT_S16_LE) && (16 < bits->min || 16 > bits->max)) keep.bits[0] &= ~(1U << NX_FORMAT_S16_LE);
        return mask_refine(&p->masks[NX_PARAM_FORMAT], &keep);
    }
    if (r->kind == RULE_SAMPLE_BITS) {
        t.min = UINT_MAX32; t.max = 0;
        if (mask_test(&p->masks[NX_PARAM_FORMAT], NX_FORMAT_S16_LE)) t.min = t.max = 16;
        t.integer = 1; return interval_refine(param_interval(p, r->var), &t);
    }
    const struct nx_interval *a = param_interval(p, r->dep[0]), *b = param_interval(p, r->dep[1]);
    if (r->kind == RULE_MUL) interval_mul(a, b, &t);
    else if (r->kind == RULE_DIV) interval_div(a, b, &t);
    else if (r->kind == RULE_MULKDIV) interval_mulkdiv(a, r->k, b, &t);
    else interval_muldivk(a, b, r->k, &t);
    return interval_refine(param_interval(p, r->var), &t);
}
static int refine(struct nx_hw_params* p) {
    static const struct nx_mask masks[3] = {{{(1U << NX_ACCESS_MMAP_INTERLEAVED) | (1U << NX_ACCESS_RW_INTERLEAVED)}},
                                            {{1U << NX_FORMAT_S16_LE}}, {{1U}}};
    p->info = 0; p->fifo_size = 0;
    if (p->rmask & (1U << NX_PARAM_SAMPLE_BITS)) p->msbits = 0;
    if (p->rmask & (1U << NX_PARAM_RATE)) p->rate_num = p->rate_den = 0;
    for (unsigned k = 0; k < 3; k++) {
        if (mask_empty(&p->masks[k])) return E_INVAL;
        if (!(p->rmask & (1U << k))) continue;
        int changed = mask_refine(&p->masks[k], &masks[k]);
        if (changed < 0) return changed;
        if (changed) p->cmask |= 1U << k;
    }
    for (unsigned k = NX_PARAM_SAMPLE_BITS; k <= NX_PARAM_TICK_TIME; k++) {
        struct nx_interval* i = param_interval(p, k);
        if (i->empty) return E_INVAL;
        if (!(p->rmask & (1U << k))) continue;
        struct nx_interval c; constraint(k, &c);
        int changed = interval_refine(i, &c);
        if (changed < 0) return changed;
        if (changed) p->cmask |= 1U << k;
    }
    unsigned vstamps[NX_PARAM_TICK_TIME + 1], rstamps[RULE_COUNT] = {0}, stamp = 2, applied = 0; bool again;
    for (unsigned k = 0; k <= NX_PARAM_TICK_TIME; k++) vstamps[k] = (p->rmask & (1U << k)) ? 1 : 0;
    do {
        again = false;
        for (unsigned k = 0; k < RULE_COUNT; k++) {
            const struct rule* r = &rules[k]; bool due = false;
            for (unsigned d = 0; d < 2; d++) if (r->dep[d] != NONE && vstamps[r->dep[d]] > rstamps[k]) due = true;
            if (!due) continue;
            if (++applied > 100000) return E_INVAL;
            int changed = apply_rule(p, r);
            if (changed < 0) return changed;
            if (changed) { p->cmask |= 1U << r->var; vstamps[r->var] = stamp; again = true; }
            rstamps[k] = stamp++;
        }
    } while (again);
    return 0;
}
static void fixup(struct nx_hw_params* p) {
    if (!p->msbits) {
        struct nx_interval* bits = param_interval(p, NX_PARAM_SAMPLE_BITS);
        if (interval_single(bits)) p->msbits = interval_value(bits);
        if (mask_single(&p->masks[NX_PARAM_FORMAT])) p->msbits = 16;
    }
    if (!p->rate_den) {
        struct nx_interval* rate = param_interval(p, NX_PARAM_RATE);
        if (interval_single(rate)) { p->rate_num = interval_value(rate); p->rate_den = 1; }
    }
    if (!p->info) p->info = HW_INFO;
}
static int choose(struct nx_hw_params* p) {
    static const uint8_t vars[] = {NX_PARAM_ACCESS, NX_PARAM_FORMAT, NX_PARAM_SUBFORMAT, NX_PARAM_CHANNELS, NX_PARAM_RATE,
                                   NX_PARAM_PERIOD_TIME, NX_PARAM_BUFFER_SIZE, NX_PARAM_TICK_TIME};
    for (unsigned v = 0; v < sizeof(vars); v++) {
        unsigned var = vars[v]; int changed;
        if (var <= NX_PARAM_SUBFORMAT) {
            struct nx_mask* m = &p->masks[var]; changed = !mask_single(m);
            if (changed) { unsigned bit = mask_min(m); memset(m, 0, sizeof(*m)); m->bits[bit / 32] = 1U << (bit % 32); }
        } else changed = var == NX_PARAM_BUFFER_SIZE ? interval_refine_last(param_interval(p, var)) : interval_refine_first(param_interval(p, var));
        if (changed < 0) return changed;
        if (!changed) continue;
        p->cmask |= 1U << var; p->rmask = 1U << var;
        int error = refine(p); if (error) return error;
    }
    return 0;
}

/* ---------- client buffer (mmap-able shared memory file) ---------- */
/* s->pages[] holds its own reference to every frame, and the file refuses hole punching (seal 16, which only
   memory_file_punch checks for a device file): madvise(MADV_REMOVE) on a mapping can neither free a page that
   write_frames/mix_frames still use nor detach the mapping from the pages the mixer reads. */
static void buffer_release(struct stream* s) {
    for (unsigned i = 0; i < s->page_count; i++) page_free(s->pages[i]);
    if (s->buffer) memory_file_release(s->buffer);
    s->buffer = NULL; s->page_count = 0;
}
static int buffer_allocate(struct stream* s, uint64_t bytes) {
    struct memory_file* file = memory_file_create(2); if (!file) return E_NOMEM;
    unsigned count = (unsigned)((bytes + 4095) / 4096);
    file->size = (uint64_t)count * 4096; file->seals |= 16;
    for (unsigned i = 0; i < count; i++) {
        uint64_t frame = memory_file_page(file, i, true);
        if (!frame) { while (i--) page_free(s->pages[i]); memory_file_release(file); return E_NOMEM; }
        page_retain(frame); s->pages[i] = (uint64_t)phys_to_virt(frame);
    }
    s->buffer = file; s->page_count = count; return 0;
}
/* Boot check of the above: pages pinned, punching refused, everything returned on release. */
static void buffer_selftest(void) {
    struct stream* s = &streams[0]; uint64_t before = memory_used_pages();
    bool ok = !buffer_allocate(s, 2 * 4096) && page_references(s->pages[1]) == 2 &&
              memory_file_punch(s->buffer, 0, 2 * 4096) < 0 && page_references(s->pages[1]) == 2;
    buffer_release(s);
    if (!ok || memory_used_pages() != before) panic("sound buffer pinning self-test");
    vga_print("[64] PASS sound buffer pages pinned, hole punching refused\n");
}
static bool pcm_node(unsigned node) { return node == 1 || node == 2; }
/* reader->sequence = card generation << 8 | stream index + 1 (0 for a control node): an fd of an unplugged card never
   reaches a later card in the same slot. */
static bool reader_current(const struct input_reader* reader) { return reader && reader->sequence >> 8 == cur->generation; }
static struct stream* stream_of(const struct input_reader* reader) {
    unsigned i = reader ? (unsigned)(reader->sequence & 0xFF) : 0;
    return reader_current(reader) && i >= 1 && i <= STREAMS && streams[i - 1].used ? &streams[i - 1] : NULL;
}
struct memory_file* sound_mapping(unsigned node, uint64_t offset, uint64_t length, const struct input_reader* reader) {
    node = select_card(node);
    struct stream* s = pcm_node(node) && cur->present ? stream_of(reader) : NULL;
    if (!s || !s->buffer || offset || length > s->buffer->size) return NULL;
    return s->buffer;
}

/* ---------- user copies ---------- */
static bool get_user(uint64_t address, void* data, uint64_t size) {
    struct card_state* c = cur;   /* a faulting copy may sleep and let another task select another card */
    if (!accessible(address, size, false)) return false;
    bool ok = copy_from_user(data, (const void*)address, size) == 0; cur = c; return ok;
}
static bool put_user(uint64_t address, const void* data, uint64_t size) {
    struct card_state* c = cur;
    if (!accessible(address, size, true)) return false;
    bool ok = copy_to_user((void*)address, data, size) == 0; cur = c; return ok;
}
static struct nx_timespec timespec_ns(uint64_t ns) { return (struct nx_timespec){(int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL)}; }
static struct nx_timespec clock_stamp(const struct stream* s, uint64_t monotonic) {
    if (!monotonic) return (struct nx_timespec){0, 0};
    return timespec_ns(s->tstamp_type ? monotonic : monotonic + (native_realtime_ns() - native_clock_ns()));
}
static unsigned free_streams(void) { unsigned count = 0; for (unsigned i = 0; i < STREAMS; i++) count += !streams[i].used; return count; }
static void fill_info(struct nx_pcm_info* info, unsigned subdevice, bool capture) {
    memset(info, 0, sizeof(*info));
    strcpy((char*)info->id, hw.id); strcpy((char*)info->name, capture ? hw.capture_name : hw.pcm_name); info->stream = capture;
    strcpy((char*)info->subname, "subdevice #0"); info->subname[11] = (uint8_t)('0' + subdevice);
    info->subdevice = subdevice; info->subdevices_count = STREAMS; info->subdevices_avail = free_streams();
}

/* ---------- PCM operations ---------- */
static int hwsync(struct stream* s) {
    switch (s->state) {
    case NX_STATE_RUNNING: case NX_STATE_DRAINING: return update(s, true);
    case NX_STATE_PREPARED: case NX_STATE_PAUSED: return 0;
    case NX_STATE_XRUN: return E_PIPE;
    default: return E_BADFD;
    }
}
static int apply_appl(struct stream* s, uint64_t position) {
    if (position == s->appl_ptr) return 0;
    if (position >= s->boundary) return E_INVAL;
    s->appl_ptr = position;
    if (s->linked && !s->capture) { commit(s); engine_feed(); }
    return 0;
}
static int hw_params(struct stream* s, struct nx_hw_params* p) {
    if (s->state != NX_STATE_OPEN && s->state != NX_STATE_SETUP && s->state != NX_STATE_PREPARED) return E_BADFD;
    if (s->buffer && s->buffer->references > 1) return E_BADFD;
    p->rmask = ~0U;
    int error = refine(p); if (!error) error = choose(p);
    if (error) return error;
    fixup(p);
    struct nx_interval *period = param_interval(p, NX_PARAM_PERIOD_SIZE), *buffer = param_interval(p, NX_PARAM_BUFFER_SIZE);
    uint64_t frames = buffer->min;
    if (!frames || frames * FRAME_BYTES > BUFFER_BYTES_MAX || !period->min) return E_INVAL;
    buffer_release(s);
    s->state = NX_STATE_OPEN;
    error = buffer_allocate(s, frames * FRAME_BYTES); if (error) return error;
    s->access = mask_min(&p->masks[NX_PARAM_ACCESS]);
    s->buffer_size = frames; s->period_size = period->min; s->periods = param_interval(p, NX_PARAM_PERIODS)->min;
    s->boundary = frames; while (s->boundary * 2 <= 0x7FFFFFFFFFFFFFFFULL - frames) s->boundary *= 2;
    s->tstamp_mode = 0; s->period_step = 1; s->avail_min = s->period_size; s->start_threshold = 1;
    s->stop_threshold = frames; s->silence_threshold = s->silence_size = 0;
    s->hw_ptr = s->appl_ptr = s->mixed = s->avail_max = 0; s->state = NX_STATE_SETUP;
    vga_print("[64] SOUND hw_params"); log_hex(" stream=", s->index); log_hex(" access=", s->access); log_hex(" buffer=", frames);
    log_hex(" period=", s->period_size); vga_print("\n");
    return 0;
}
static int sw_params(struct stream* s, struct nx_sw_params* p) {
    if (s->state == NX_STATE_OPEN) return E_BADFD;
    if (p->tstamp_mode < 0 || p->tstamp_mode > 1) return E_INVAL;
    if (p->proto >= NX_PROTOCOL(2, 0, 12) && p->tstamp_type > 2) return E_INVAL;
    if (!p->avail_min) return E_INVAL;
    if (p->silence_size >= s->boundary) { if (p->silence_threshold) return E_INVAL; }
    else if (p->silence_size > p->silence_threshold || p->silence_threshold > s->buffer_size) return E_INVAL;
    s->tstamp_mode = p->tstamp_mode;
    if (p->proto >= NX_PROTOCOL(2, 0, 12)) s->tstamp_type = (int)p->tstamp_type;
    s->period_step = p->period_step; s->avail_min = p->avail_min; s->start_threshold = p->start_threshold;
    s->stop_threshold = p->stop_threshold; s->silence_threshold = p->silence_threshold; s->silence_size = p->silence_size;
    p->boundary = s->boundary;
    return running(s) ? update(s, true) : 0;
}
static void fill_status(struct stream* s, struct nx_pcm_status* status) {
    memset(status, 0, sizeof(*status));
    status->state = s->state;
    if (s->state == NX_STATE_OPEN) return;
    status->trigger_tstamp = clock_stamp(s, s->trigger_ns);
    if (running(s)) update(s, true);
    status->tstamp = clock_stamp(s, running(s) && s->update_ns ? s->update_ns : native_clock_ns());
    status->appl_ptr = s->appl_ptr; status->hw_ptr = s->hw_ptr; status->avail = avail(s);
    status->delay = running(s) ? delay(s) : 0;
    status->avail_max = s->avail_max; s->avail_max = 0;
    status->audio_tstamp = timespec_ns(s->hw_ptr * 1000000000ULL / RATE);
    status->driver_tstamp = status->tstamp;
}
static int64_t write_frames(struct stream* s, uint64_t address, uint64_t frames, bool nonblock) {
    switch (s->state) {
    case NX_STATE_PREPARED: case NX_STATE_RUNNING: case NX_STATE_PAUSED: break;
    case NX_STATE_XRUN: return E_PIPE;
    default: return E_BADFD;
    }
    if (s->access != NX_ACCESS_RW_INTERLEAVED) return E_INVAL;
    if (!frames) return 0;
    if (frames > 0x7FFFFFFFULL / FRAME_BYTES) frames = 0x7FFFFFFFULL / FRAME_BYTES;
    if (s->state == NX_STATE_RUNNING) update(s, true);
    uint64_t done = 0, stalled = native_clock_ns(), last_hw = s->hw_ptr; int64_t error = 0;
    while (done < frames) {
        if (s->state == NX_STATE_XRUN || s->state == NX_STATE_DRAINING) { error = E_PIPE; break; }
        if (s->state != NX_STATE_PREPARED && s->state != NX_STATE_RUNNING && s->state != NX_STATE_PAUSED) { error = E_BADFD; break; }
        uint64_t space = avail(s);
        if (!space) {
            if (nonblock) { error = E_AGAIN; break; }
            uint64_t want = frames - done < s->avail_min ? frames - done : s->avail_min;
            int failed = wait_frames(s, want ? want : 1);
            if (failed) { error = failed; break; }
            if (running(s)) update(s, true);
            if (s->hw_ptr != last_hw || s->state == NX_STATE_PAUSED) { last_hw = s->hw_ptr; stalled = native_clock_ns(); }
            else if (native_clock_ns() - stalled > 10000000000ULL) { vga_print("[64] SOUND write timeout (DMA stalled)\n"); error = E_IO; break; }
            continue;
        }
        uint64_t count = frames - done < space ? frames - done : space, offset = s->appl_ptr % s->buffer_size;
        if (count > s->buffer_size - offset) count = s->buffer_size - offset;
        uint64_t source = address + done * FRAME_BYTES, bytes = count * FRAME_BYTES, byte = offset * FRAME_BYTES;
        if (!accessible(source, bytes, false)) { error = E_FAULT; break; }
        while (bytes) {
            uint64_t part = 4096 - byte % 4096; if (part > bytes) part = bytes;
            if (copy_from_user((void*)(s->pages[byte / 4096] + byte % 4096), (const void*)source, part)) { error = E_FAULT; break; }
            source += part; byte += part; bytes -= part;
        }
        cur = s->owner;
        s->appl_ptr = fwd(s, s->appl_ptr, count); done += count; s->writes++;
        if (s->linked) { commit(s); engine_feed(); }
        if (s->state == NX_STATE_PREPARED && hw_avail(s) >= (int64_t)s->start_threshold) {
            int failed = start(s, NX_STATE_RUNNING); if (failed) { error = failed; break; }
        }
    }
    return done ? (int64_t)done : error;
}
/* readi / read(): copy captured frames from the client buffer; PREPARED starts the capture at start_threshold. */
static int64_t read_frames(struct stream* s, uint64_t address, uint64_t frames, bool nonblock) {
    switch (s->state) {
    case NX_STATE_PREPARED: case NX_STATE_RUNNING: case NX_STATE_PAUSED: case NX_STATE_DRAINING: break;
    case NX_STATE_XRUN: return E_PIPE;
    default: return E_BADFD;
    }
    if (s->access != NX_ACCESS_RW_INTERLEAVED) return E_INVAL;
    if (!frames) return 0;
    if (frames > 0x7FFFFFFFULL / FRAME_BYTES) frames = 0x7FFFFFFFULL / FRAME_BYTES;
    if (s->state == NX_STATE_PREPARED && frames >= s->start_threshold) { int failed = start(s, NX_STATE_RUNNING); if (failed) return failed; }
    if (running(s)) update(s, true);
    uint64_t done = 0, stalled = native_clock_ns(), last_hw = s->hw_ptr; int64_t error = 0;
    while (done < frames) {
        if (s->state == NX_STATE_XRUN) { error = E_PIPE; break; }
        if (s->state == NX_STATE_SETUP) break;                                /* drained: 0 frames, as Linux */
        if (s->state != NX_STATE_PREPARED && s->state != NX_STATE_RUNNING && s->state != NX_STATE_PAUSED && s->state != NX_STATE_DRAINING) { error = E_BADFD; break; }
        uint64_t have = avail(s);
        if (!have) {
            if (s->state == NX_STATE_DRAINING) { s->state = NX_STATE_SETUP; notify(s); break; }
            if (nonblock) { error = E_AGAIN; break; }
            uint64_t want = frames - done < s->avail_min ? frames - done : s->avail_min;
            int failed = wait_frames(s, want ? want : 1);
            if (failed) { error = failed; break; }
            if (running(s)) update(s, true);
            if (s->hw_ptr != last_hw || s->state != NX_STATE_RUNNING) { last_hw = s->hw_ptr; stalled = native_clock_ns(); }
            else if (native_clock_ns() - stalled > 10000000000ULL) { vga_print("[64] SOUND read timeout (capture DMA stalled)\n"); error = E_IO; break; }
            continue;
        }
        uint64_t count = frames - done < have ? frames - done : have, offset = s->appl_ptr % s->buffer_size;
        if (count > s->buffer_size - offset) count = s->buffer_size - offset;
        uint64_t target = address + done * FRAME_BYTES, bytes = count * FRAME_BYTES, byte = offset * FRAME_BYTES;
        if (!accessible(target, bytes, true)) { error = E_FAULT; break; }
        while (bytes) {
            uint64_t part = 4096 - byte % 4096; if (part > bytes) part = bytes;
            if (copy_to_user((void*)target, (const void*)(s->pages[byte / 4096] + byte % 4096), part)) { error = E_FAULT; break; }
            target += part; byte += part; bytes -= part;
        }
        cur = s->owner;
        s->appl_ptr = fwd(s, s->appl_ptr, count); done += count; s->writes++;
    }
    return done ? (int64_t)done : error;
}
static int drain(struct stream* s, bool nonblock) {
    if (s->state == NX_STATE_OPEN) return E_BADFD;
    if (s->capture) {                                    /* stop recording; what was captured stays readable */
        (void)nonblock;
        if (s->state == NX_STATE_RUNNING || s->state == NX_STATE_PAUSED) {
            if (s->linked) { sync_stream(s); unlink_stream(s); }
            s->state = avail(s) ? NX_STATE_DRAINING : NX_STATE_SETUP; s->trigger_ns = native_clock_ns(); notify(s);
        } else if (s->state == NX_STATE_PREPARED || s->state == NX_STATE_XRUN) s->state = NX_STATE_SETUP;
        return 0;
    }
    if (s->state == NX_STATE_PAUSED) { s->state = NX_STATE_RUNNING; link_stream(s); }
    if (s->state == NX_STATE_PREPARED) { if (hw_avail(s) > 0) start(s, NX_STATE_DRAINING); else s->state = NX_STATE_SETUP; }
    else if (s->state == NX_STATE_RUNNING) s->state = NX_STATE_DRAINING;
    else if (s->state == NX_STATE_XRUN) s->state = NX_STATE_SETUP;
    if (s->state == NX_STATE_DRAINING) update(s, true);
    if (s->state == NX_STATE_DRAINING && nonblock) return E_AGAIN;
    uint64_t stalled = native_clock_ns(), last_hw = s->hw_ptr;
    while (s->state == NX_STATE_DRAINING) {
        int error = wait_frames(s, s->buffer_size); if (error) return error;
        update(s, true);
        if (s->hw_ptr != last_hw) { last_hw = s->hw_ptr; stalled = native_clock_ns(); }
        else if (native_clock_ns() - stalled > 10000000000ULL) { stop(s, NX_STATE_SETUP); vga_print("[64] SOUND drain timeout\n"); return E_IO; }
    }
    return 0;
}
static int pause_stream(struct stream* s, bool push) {
    if (s->capture) {                                    /* frames captured while paused are not recorded */
        if (push) { if (s->state != NX_STATE_RUNNING) return E_BADFD; sync_stream(s); unlink_stream(s); s->state = NX_STATE_PAUSED; }
        else {
            if (s->state != NX_STATE_PAUSED) return E_BADFD;
            s->state = NX_STATE_RUNNING; link_stream(s);
            if (!s->linked) { s->state = NX_STATE_PAUSED; return E_IO; }   /* the input DMA did not restart */
        }
        s->trigger_ns = native_clock_ns(); notify(s); return 0;
    }
    if (push) {
        if (s->state != NX_STATE_RUNNING) return E_BADFD;
        engine_update(true); sync_stream(s);
        uint64_t resume = unlink_stream(s); /* frames inside the copy margin still play before the pause takes effect */
        if (dist(s, resume, s->hw_ptr) <= dist(s, s->appl_ptr, s->hw_ptr)) set_hw_ptr(s, resume, native_clock_ns());
        s->mixed = s->hw_ptr; s->state = NX_STATE_PAUSED;
    } else {
        if (s->state != NX_STATE_PAUSED) return E_BADFD;
        s->state = NX_STATE_RUNNING; link_stream(s);
    }
    s->trigger_ns = native_clock_ns(); notify(s); return 0;
}
static int move_appl(struct stream* s, uint64_t address, bool forward) {
    uint64_t frames; int64_t result;
    if (!get_user(address, &frames, 8)) return E_FAULT;
    uint64_t zero = 0; if (!put_user(address, &zero, 8)) return E_FAULT;
    if (!frames) return 0;
    int error = hwsync(s); if (error) return error;
    if (forward) {
        int64_t limit = (int64_t)avail(s); if (limit < 0) limit = 0;
        result = (int64_t)frames > limit ? limit : (int64_t)frames;
        if (result > 0) apply_appl(s, fwd(s, s->appl_ptr, (uint64_t)result));
    } else {
        /* Already-mixed frames inside the copy margin can no longer be withdrawn. */
        int64_t limit = hw_avail(s);
        if (s->linked && !s->capture) {
            uint64_t floor = mix.played + (engine_running() ? MARGIN_FRAMES : 0), safe = s->hw_ptr;
            if (floor > s->slave_base) { uint64_t skip = floor - s->slave_base; if (skip < s->boundary) safe = fwd(s, s->client_base, skip); }
            int64_t withdrawable = dist(s, s->appl_ptr, safe) <= s->buffer_size ? (int64_t)dist(s, s->appl_ptr, safe) : 0;
            if (withdrawable < limit) limit = withdrawable;
        }
        if (limit < 0) limit = 0;
        result = (int64_t)frames > limit ? limit : (int64_t)frames;
        if (result > 0) {
            uint64_t target = dist(s, s->appl_ptr, (uint64_t)result) % s->boundary;
            if (s->linked && !s->capture && dist(s, s->mixed, target) <= s->buffer_size && s->mixed != target) { mix_frames(s, target, s->mixed, -1); s->mixed = target; }
            s->appl_ptr = target;
        }
    }
    return put_user(address, &result, 8) ? 0 : E_FAULT;
}
static int64_t pcm_ioctl(struct stream* s, uint32_t command, uint64_t argument, bool nonblock) {
    if (s->state == NX_STATE_DISCONNECTED && command != NX_PCM_IOCTL_HW_FREE && command != NX_PCM_IOCTL_DROP) return -19;
    switch (command) {
    case NX_PCM_IOCTL_PVERSION: { int32_t version = NX_PCM_VERSION; return put_user(argument, &version, 4) ? 0 : E_FAULT; }
    case NX_PCM_IOCTL_INFO: { struct nx_pcm_info info; fill_info(&info, s->index, s->capture); return put_user(argument, &info, sizeof(info)) ? 0 : E_FAULT; }
    case NX_PCM_IOCTL_TSTAMP: return 0;
    case NX_PCM_IOCTL_TTSTAMP: {
        int32_t type; if (!get_user(argument, &type, 4)) return E_FAULT;
        if (type < 0 || type > 2) return E_INVAL;
        s->tstamp_type = type; return 0;
    }
    case NX_PCM_IOCTL_USER_PVERSION: { int32_t version; return get_user(argument, &version, 4) ? 0 : E_FAULT; }
    case NX_PCM_IOCTL_HW_REFINE: case NX_PCM_IOCTL_HW_PARAMS: {
        struct nx_hw_params p; if (!get_user(argument, &p, sizeof(p))) return E_FAULT;
        int error;
        if (command == NX_PCM_IOCTL_HW_PARAMS) error = hw_params(s, &p);
        else { error = refine(&p); if (!error) fixup(&p); p.rmask = 0; }
        if (error) return error;
        return put_user(argument, &p, sizeof(p)) ? 0 : E_FAULT;
    }
    case NX_PCM_IOCTL_HW_FREE:
        if ((s->state != NX_STATE_SETUP && s->state != NX_STATE_PREPARED) || (s->buffer && s->buffer->references > 1)) return E_BADFD;
        buffer_release(s); s->state = NX_STATE_OPEN; return 0;
    case NX_PCM_IOCTL_SW_PARAMS: {
        struct nx_sw_params p; if (!get_user(argument, &p, sizeof(p))) return E_FAULT;
        int error = sw_params(s, &p);
        if (!put_user(argument, &p, sizeof(p))) return E_FAULT;
        return error == E_PIPE ? 0 : error;
    }
    case NX_PCM_IOCTL_STATUS: case NX_PCM_IOCTL_STATUS_EXT: {
        struct nx_pcm_status status; memset(&status, 0, sizeof(status));
        if (command == NX_PCM_IOCTL_STATUS_EXT && !get_user(argument, &status, sizeof(status))) return E_FAULT;
        fill_status(s, &status); return put_user(argument, &status, sizeof(status)) ? 0 : E_FAULT;
    }
    case NX_PCM_IOCTL_DELAY: {
        int error = hwsync(s); if (error) return error;
        int64_t frames = delay(s); return put_user(argument, &frames, 8) ? 0 : E_FAULT;
    }
    case NX_PCM_IOCTL_HWSYNC: return hwsync(s);
    case NX_PCM_IOCTL_SYNC_PTR: {
        struct nx_sync_ptr sync; if (!get_user(argument, &sync, sizeof(sync))) return E_FAULT;
        if (sync.flags & NX_SYNC_PTR_HWSYNC) { int error = hwsync(s); if (error) return error; }
        else update(s, false);
        if (!(sync.flags & NX_SYNC_PTR_APPL)) { int error = apply_appl(s, sync.c.control.appl_ptr); if (error) return error; }
        else sync.c.control.appl_ptr = s->appl_ptr;
        if (!(sync.flags & NX_SYNC_PTR_AVAIL_MIN)) s->avail_min = sync.c.control.avail_min;
        else sync.c.control.avail_min = s->avail_min;
        memset(&sync.s, 0, sizeof(sync.s));
        sync.s.status.state = s->state; sync.s.status.hw_ptr = s->hw_ptr;
        sync.s.status.tstamp = clock_stamp(s, s->update_ns);
        sync.s.status.audio_tstamp = timespec_ns(s->hw_ptr * 1000000000ULL / RATE);
        return put_user(argument, &sync, sizeof(sync)) ? 0 : E_FAULT;
    }
    case NX_PCM_IOCTL_CHANNEL_INFO: {
        struct nx_channel_info info; if (!get_user(argument, &info, sizeof(info))) return E_FAULT;
        if (s->state == NX_STATE_OPEN) return E_BADFD;
        if (info.channel >= CHANNELS) return E_INVAL;
        uint32_t channel = info.channel; memset(&info, 0, sizeof(info));
        info.channel = channel; info.offset = 0; info.first = channel * 16; info.step = CHANNELS * 16;
        return put_user(argument, &info, sizeof(info)) ? 0 : E_FAULT;
    }
    case NX_PCM_IOCTL_PREPARE:
        if (s->state == NX_STATE_OPEN) return E_BADFD;
        if (s->state == NX_STATE_PAUSED) stop(s, NX_STATE_SETUP);
        if (running(s)) return E_BUSY;
        s->hw_ptr = s->appl_ptr = s->mixed = s->avail_max = 0;
        s->state = NX_STATE_PREPARED; return 0;
    case NX_PCM_IOCTL_RESET:
        if (s->state != NX_STATE_RUNNING && s->state != NX_STATE_PREPARED && s->state != NX_STATE_PAUSED) return E_BADFD;
        if (s->state == NX_STATE_RUNNING && update(s, true) == 0 && s->state == NX_STATE_RUNNING) {
            unlink_stream(s); s->hw_ptr %= s->buffer_size; s->appl_ptr = s->hw_ptr; link_stream(s);
        } else { s->hw_ptr = 0; s->appl_ptr = 0; s->mixed = 0; }
        s->appl_ptr = s->hw_ptr; return 0;
    case NX_PCM_IOCTL_START:
        if (s->state != NX_STATE_PREPARED) return E_BADFD;
        if (!s->capture && hw_avail(s) <= 0 && s->stop_threshold < s->boundary) return E_PIPE;
        return start(s, NX_STATE_RUNNING);
    case NX_PCM_IOCTL_DROP:
        if (s->state == NX_STATE_OPEN) return E_BADFD;
        stop(s, NX_STATE_SETUP); return 0;
    case NX_PCM_IOCTL_DRAIN: return drain(s, nonblock);
    case NX_PCM_IOCTL_PAUSE: return pause_stream(s, argument != 0);
    case NX_PCM_IOCTL_REWIND: case NX_PCM_IOCTL_FORWARD: return move_appl(s, argument, command == NX_PCM_IOCTL_FORWARD);
    case NX_PCM_IOCTL_RESUME: return E_NOSYS;
    case NX_PCM_IOCTL_XRUN:
        if (s->state == NX_STATE_XRUN) return 0;
        if (s->state != NX_STATE_RUNNING) return E_BADFD;
        stop(s, NX_STATE_XRUN); return 0;
    case NX_PCM_IOCTL_WRITEI_FRAMES: case NX_PCM_IOCTL_READI_FRAMES: {
        struct nx_xferi transfer; if (!get_user(argument, &transfer, sizeof(transfer))) return E_FAULT;
        if (s->capture != (command == NX_PCM_IOCTL_READI_FRAMES)) return E_BADFD;
        int64_t result = s->capture ? read_frames(s, transfer.buffer, transfer.frames, nonblock) : write_frames(s, transfer.buffer, transfer.frames, nonblock);
        transfer.result = result;
        if (!put_user(argument, &transfer, sizeof(transfer))) return E_FAULT;
        return result < 0 ? result : 0;
    }
    case NX_PCM_IOCTL_LINK: case NX_PCM_IOCTL_UNLINK: return E_NOSYS;
    }
    return E_NOTTY;
}
/* Control elements: numid = index + 1, mixer interface, unique names. */
static void element_id(struct nx_elem_id* id, unsigned index) {
    memset(id, 0, sizeof(*id)); id->numid = index + 1; id->iface = NX_ELEM_IFACE_MIXER;
    memcpy(id->name, hw.control[index].name, sizeof(hw.control[index].name));
}
static int element_find(const struct nx_elem_id* id) {
    if (id->numid) return id->numid <= hw.controls ? (int)id->numid - 1 : -1;
    for (unsigned i = 0; i < hw.controls; i++)
        if (id->iface == NX_ELEM_IFACE_MIXER && !id->index && !strncmp((const char*)id->name, hw.control[i].name, sizeof(id->name))) return (int)i;
    return -1;
}
static int64_t control_ioctl(uint32_t command, uint64_t argument, bool* subscribed) {
    switch (command) {
    case NX_CTL_IOCTL_PVERSION: { int32_t version = NX_CTL_VERSION; return put_user(argument, &version, 4) ? 0 : E_FAULT; }
    case NX_CTL_IOCTL_CARD_INFO: {
        struct nx_card_info info; memset(&info, 0, sizeof(info));
        strcpy((char*)info.id, hw.id); strcpy((char*)info.driver, hw.driver); strcpy((char*)info.name, hw.name);
        strcpy((char*)info.longname, hw.longname); strcpy((char*)info.mixername, hw.mixer); strcpy((char*)info.components, hw.components);
        return put_user(argument, &info, sizeof(info)) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_ELEM_LIST: {
        struct nx_elem_list list; if (!get_user(argument, &list, sizeof(list))) return E_FAULT;
        list.count = hw.controls; list.used = 0;
        for (unsigned i = list.offset; i < hw.controls && list.used < list.space; i++, list.used++) {
            struct nx_elem_id id; element_id(&id, i);
            if (!put_user(list.pids + list.used * sizeof(id), &id, sizeof(id))) return E_FAULT;
        }
        return put_user(argument, &list, sizeof(list)) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_ELEM_INFO: {
        struct nx_elem_info info; if (!get_user(argument, &info, sizeof(info))) return E_FAULT;
        int index = element_find(&info.id); if (index < 0) return E_NOENT;
        const struct sound_control* c = &hw.control[index];
        memset(&info, 0, sizeof(info)); element_id(&info.id, (unsigned)index);
        info.type = c->type; info.count = c->channels;
        info.access = NX_ELEM_ACCESS_READWRITE | (c->db_step ? NX_ELEM_ACCESS_TLV_READ : 0);
        info.value.integer.min = c->min; info.value.integer.max = c->max; info.value.integer.step = c->type == SOUND_CONTROL_INTEGER;
        return put_user(argument, &info, sizeof(info)) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_ELEM_READ: case NX_CTL_IOCTL_ELEM_WRITE: {
        static struct nx_elem_value value;                /* 1224 bytes: off the kernel stack (serialised by the kernel lock) */
        if (!get_user(argument, &value, sizeof(value))) return E_FAULT;
        int index = element_find(&value.id); if (index < 0) return E_NOENT;
        struct sound_control* c = &hw.control[index];
        if (command == NX_CTL_IOCTL_ELEM_WRITE) {
            bool changed = false;
            for (unsigned k = 0; k < c->channels; k++) {
                if (value.value.integer[k] < c->min || value.value.integer[k] > c->max) return E_INVAL;
                changed |= c->value[k] != (int32_t)value.value.integer[k];
            }
            int32_t old[2] = {c->value[0], c->value[1]};
            for (unsigned k = 0; k < c->channels; k++) c->value[k] = (int32_t)value.value.integer[k];
            if (changed && hw.control_put && !hw.control_put((unsigned)index)) { c->value[0] = old[0]; c->value[1] = old[1]; return E_IO; }
            return 0;
        }
        memset(&value.value, 0, sizeof(value.value)); element_id(&value.id, (unsigned)index);
        for (unsigned k = 0; k < c->channels; k++) value.value.integer[k] = c->value[k];
        return put_user(argument, &value, sizeof(value)) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_TLV_READ: {
        struct nx_ctl_tlv header; if (!get_user(argument, &header, sizeof(header))) return E_FAULT;
        if (!header.numid || header.numid > hw.controls || !hw.control[header.numid - 1].db_step) return E_NOENT;
        if (header.length < 16) return -12;              /* ENOMEM: buffer too small, as Linux */
        const struct sound_control* c = &hw.control[header.numid - 1];
        uint32_t tlv[4] = {c->db_max ? NX_TLVT_DB_MINMAX : NX_TLVT_DB_SCALE, 8, (uint32_t)c->db_min, (uint32_t)(c->db_max ? c->db_max : c->db_step)};
        return put_user(argument + sizeof(header), tlv, sizeof(tlv)) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_ELEM_LOCK: case NX_CTL_IOCTL_ELEM_UNLOCK: {
        struct nx_elem_id id; if (!get_user(argument, &id, sizeof(id))) return E_FAULT;
        return element_find(&id) < 0 ? E_NOENT : 0;
    }
    case NX_CTL_IOCTL_SUBSCRIBE_EVENTS: {
        int32_t value; if (!get_user(argument, &value, 4)) return E_FAULT;
        if (value < 0) { value = *subscribed; return put_user(argument, &value, 4) ? 0 : E_FAULT; }
        *subscribed = value != 0; return 0;
    }
    case NX_CTL_IOCTL_PCM_NEXT_DEVICE: {
        int32_t device; if (!get_user(argument, &device, 4)) return E_FAULT;
        device = device < 0 ? 0 : -1; return put_user(argument, &device, 4) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_HWDEP_NEXT_DEVICE: case NX_CTL_IOCTL_RAWMIDI_NEXT_DEVICE: case NX_CTL_IOCTL_UMP_NEXT_DEVICE: {
        int32_t device = -1; return put_user(argument, &device, 4) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_PCM_INFO: {
        struct nx_pcm_info info; if (!get_user(argument, &info, sizeof(info))) return E_FAULT;
        if (info.stream < 0 || info.stream > 1) return E_INVAL;
        if (info.device) return E_NXIO;
        if (info.stream && !hw.capture_start) return E_NOENT;
        if (info.subdevice >= STREAMS) return E_NXIO;
        fill_info(&info, info.subdevice, info.stream == 1); return put_user(argument, &info, sizeof(info)) ? 0 : E_FAULT;
    }
    case NX_CTL_IOCTL_PCM_PREFER_SUBDEVICE: { int32_t value; return get_user(argument, &value, 4) ? 0 : E_FAULT; }
    case NX_CTL_IOCTL_POWER_STATE: { int32_t state = 0; return put_user(argument, &state, 4) ? 0 : E_FAULT; }
    }
    return E_NOTTY;
}

/* ---------- device-node entry points (node 0 = controlC0, 1 = pcmC0D0p) ---------- */
int sound_open_check(unsigned node, uint64_t flags) {
    (void)flags;
    if (!sound_node_available(node)) return -19;
    node = select_card(node);
    return pcm_node(node) && !free_streams() ? E_BUSY : 0;
}
void sound_open(unsigned node, struct input_reader* reader) {
    node = select_card(node);
    reader->sequence = cur->generation << 8;
    if (!pcm_node(node)) return;
    for (unsigned i = 0; i < STREAMS; i++) if (!streams[i].used) {
        struct stream* s = &streams[i];
        memset(s, 0, sizeof(*s)); s->index = i; s->used = true; s->state = NX_STATE_OPEN; s->capture = node == 2; s->owner = cur;
        reader->sequence = cur->generation << 8 | (i + 1); trace(s, 0xF4, 0, 0); return;
    }
}
void sound_close(unsigned node, struct input_reader* reader) {
    node = select_card(node);
    struct stream* s = pcm_node(node) ? stream_of(reader) : NULL;
    if (!s) return;
    if (cur->gone) {                                     /* the card was unplugged: free its slot with the last stream */
        buffer_release(s); s->used = false; reader->sequence = 0;
        bool any = false; for (unsigned i = 0; i < STREAMS; i++) any |= streams[i].used;
        if (!any) { struct card_state* c = cur; card_free(c); cur = &cards[0]; }
        return;
    }
    if (s->linked) { if (!s->capture) engine_update(true); sync_stream(s); unlink_stream(s); }
    if (s->writes || s->starts) {
        vga_print(s->capture ? "[64] SOUND close capture" : "[64] SOUND close"); log_hex(" stream=", s->index); log_hex(" starts=", s->starts); log_hex(" xruns=", s->xruns);
        log_hex(" writes=", s->writes); log_hex(" played=", s->played); vga_print("\n");
    }
    trace(s, 0xF5, 0, 0);
    buffer_release(s); s->used = false; s->state = NX_STATE_OPEN; s->armed = false; reader->sequence = 0; notify(s);
    engine_update(true);
}
int64_t sound_ioctl(unsigned node, uint64_t command, uint64_t argument, uint64_t flags, struct input_reader* reader) {
    node = select_card(node);
    if (!cur->present || !reader_current(reader)) return -19;
    struct stream* s = pcm_node(node) ? stream_of(reader) : NULL;
    if (pcm_node(node) && !s) return E_BADFD;
    uint64_t extra = 0;
    if (SOUND_TRACE && s && (uint32_t)command == NX_PCM_IOCTL_WRITEI_FRAMES && accessible(argument, sizeof(struct nx_xferi), false)) {
        struct nx_xferi xfer; copy_from_user(&xfer, (const void*)argument, sizeof(xfer)); const struct nx_xferi* x = &xfer; uint32_t peak = 0;
        if (x->frames < 65536 && accessible(x->buffer, x->frames * FRAME_BYTES, false))
            for (uint64_t i = 0; i < x->frames * CHANNELS; i++) { int32_t v = (int16_t)uget16(x->buffer + i * 2); if (v < 0) v = -v; if ((uint32_t)v > peak) peak = (uint32_t)v; }
        extra = x->frames + 100000ULL * peak;
    }
    int64_t result = s ? pcm_ioctl(s, (uint32_t)command, argument, (flags & 0x800) != 0) : control_ioctl((uint32_t)command, argument, &reader->report);
    if (s) trace(s, (uint32_t)command, result, extra);
    if (result == E_NOTTY && unknown_logged < 32) {
        unknown_logged++; vga_print(s ? "[64] SOUND unhandled pcm ioctl" : "[64] SOUND unhandled control ioctl");
        log_hex(" cmd=", command); vga_print("\n");
    }
    return result;
}
int64_t sound_io(unsigned node, uint64_t buffer, uint64_t count, bool write, uint64_t flags, struct input_reader* reader) {
    node = select_card(node);
    struct stream* s = pcm_node(node) ? stream_of(reader) : NULL;
    if (!s) return write ? E_INVAL : E_AGAIN;
    if (!cur->present) return -19;
    if (write == s->capture || count % FRAME_BYTES) return E_INVAL;
    int64_t frames = s->capture ? read_frames(s, buffer, count / FRAME_BYTES, (flags & 0x800) != 0)
                                : write_frames(s, buffer, count / FRAME_BYTES, (flags & 0x800) != 0);
    return frames < 0 ? frames : frames * (int64_t)FRAME_BYTES;
}
uint32_t sound_ready(unsigned node, const struct input_reader* reader) {
    node = select_card(node);
    struct stream* s = pcm_node(node) ? stream_of(reader) : NULL;
    if (!s) return 0;
    if (!cur->present) return POLL_IN | POLL_OUT | POLL_ERR;
    if (!mix.notifying) update(s, false);
    uint32_t mask = ready_mask(s);
    if (!mask) { s->armed = true; trace(s, 0xF6, 0, avail(s)); }
    return mask;
}
void* sound_source(unsigned node) { node = select_card(node); return pcm_node(node) ? (void*)streams : (void*)cur; }

/* ---------- device nodes of cards 1..3 (hwnode.c; card 0 has the fixed nodes in device.c) ---------- */
#include "hwnode.h"
static int64_t node_io(unsigned node, struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags) { return sound_io(node, buffer, count, write, flags, reader); }
static int64_t node_ioctl(unsigned node, struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) { return sound_ioctl(node, command, argument, flags, reader); }
static void node_open(unsigned node, uint64_t flags, struct input_reader* reader) { (void)flags; sound_open(node, reader); }
static void node_close(unsigned node, uint64_t flags, struct input_reader* reader) { (void)flags; sound_close(node, reader); }
static uint32_t node_ready(unsigned node, const struct input_reader* reader) { return sound_ready(node, reader); }
static void* node_source(unsigned node, const struct input_reader* reader) { (void)reader; return sound_source(node); }
static struct memory_file* node_mapping(unsigned node, uint64_t* offset, uint64_t length, struct input_reader* reader) { return sound_mapping(node, *offset, length, reader); }
static const struct hwnode_ops sound_node_ops = {sound_open_check, node_open, node_close, node_io, node_ioctl, node_ready, node_source, node_mapping, NULL};
static int card_nodes[SOUND_CARDS][3];      /* hwnode slot + 1 */
static uint8_t card_kinds[SOUND_CARDS];     /* kinds announced by an "add" uevent */
static unsigned alsa_minor(unsigned card, unsigned kind) { return card * 32 + (kind == 1 ? 16 : kind == 2 ? 24 : 0); }
static void node_name(char* name, unsigned card, unsigned kind) {
    char digit[2] = {(char)('0' + card), 0};
    strcpy(name, kind ? "pcmC" : "controlC"); strcat(name, digit); if (kind) strcat(name, kind == 1 ? "D0p" : "D0c");
}
static void uevent(const char* action, unsigned card, unsigned kind) {
    char name[16], path[64] = "/devices/virtual/sound/card", digit[2] = {(char)('0' + card), 0};
    node_name(name, card, kind); strcat(path, digit); strcat(path, "/"); strcat(path, name);
    char devname[40] = "DEVNAME=snd/", minor[16] = "MINOR="; strcat(devname, name);
    unsigned number = alsa_minor(card, kind); char digits[4] = {(char)('0' + number / 100), (char)('0' + number / 10 % 10), (char)('0' + number % 10), 0};
    strcat(minor, number >= 100 ? digits : number >= 10 ? digits + 1 : digits + 2);
    const char* pairs[] = {"SUBSYSTEM=sound", devname, "MAJOR=116", minor};
    storage_uevent(action, path, pairs, 4);
}
/* sound_card_add/remove: nodes for cards >= 1 with the card's directions; uevents (SUBSYSTEM=sound) for every node. */
static void card_nodes_update(unsigned card, bool added) {
    for (unsigned kind = 0; kind < 3; kind++) {
        if (!added) {
            if (card_nodes[card][kind]) hwnode_remove(card_nodes[card][kind] - 1);
            card_nodes[card][kind] = 0;
            if (card_kinds[card] & (1U << kind)) uevent("remove", card, kind);
            continue;
        }
        if (!sound_node_available(card * 3 + kind)) continue;
        if (card) {
            char path[40] = "/dev/snd/"; node_name(path + 9, card, kind);
            int slot = hwnode_add(path, 116, alsa_minor(card, kind), card * 3 + kind, 0660, 1000, &sound_node_ops, "sound");
            card_nodes[card][kind] = slot + 1;
        }
        card_kinds[card] |= (uint8_t)(1U << kind);
        uevent("add", card, kind);
    }
    if (!added) card_kinds[card] = 0;
}
