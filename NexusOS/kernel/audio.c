/* ============================================================================
 * NexusOS — Audio Subsystem (Implementation) — Phase 38
 * ============================================================================
 * Device-agnostic software mixer + WAV parser + tone generator. Blends up to
 * AUDIO_MAX_STREAMS PCM voices (per-voice and master volume, nearest-neighbour
 * resampling, mono->stereo up-mix) into a 16-bit stereo buffer and hands it to
 * the registered sound device (ac97.c). With no device present it degrades to a
 * timed null sink so the shell commands still behave.
 *
 * Pure 32-bit integer math throughout — this is a freestanding -m32 kernel with
 * no libm and no 64-bit division helpers, so the tone generator uses Bhaskara
 * I's sine approximation and a Q16 phase accumulator.
 * ============================================================================ */

#include "audio.h"
#include "vga.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "ramfs.h"

extern volatile uint32_t system_ticks;   /* ~18.2 Hz tick (from kernel.c) */
#define TICK_MS 55                        /* ~55 ms per PIT tick          */

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */
static sound_device_t* g_dev = NULL;
static uint8_t  g_master = 80;            /* 0..100                        */
static uint32_t g_frames_played = 0;
static char     g_status[96];

static audio_voice_t voices[AUDIO_MAX_STREAMS];
static uint32_t      voice_frac[AUDIO_MAX_STREAMS];   /* Q16 source-pos fraction */

/* Mix scratch (stereo interleaved). Lives in BSS. */
static int16_t mixbuf[AUDIO_MIX_FRAMES * AUDIO_OUT_CHANNELS];

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static uint32_t out_rate(void) {
    if (g_dev && g_dev->rate) {
        uint32_t r = g_dev->rate();
        if (r) return r;
    }
    return AUDIO_OUT_RATE;
}

/* Bhaskara I sine approximation. deg in any range; returns Q15 [-32767,32767]. */
static int32_t sin_q15(int32_t deg) {
    deg %= 360; if (deg < 0) deg += 360;
    int32_t sign = 1;
    if (deg >= 180) { deg -= 180; sign = -1; }
    int32_t t   = deg * (180 - deg);          /* max 8100                  */
    int32_t num = 4 * t;                       /* max 32400                 */
    int32_t den = 40500 - t;                   /* min 32400                 */
    int32_t v   = (num * 32767) / den;         /* fits int32 (<1.07e9)      */
    return sign * v;
}

/* --------------------------------------------------------------------------
 * Sample WAV — generated at boot so `play startup.wav` works out of the box.
 * 22050 Hz mono 16-bit (deliberately != device rate, to exercise resampling):
 * a short rising two-note chime with a fade-out so it ends cleanly.
 * -------------------------------------------------------------------------- */
static void put32le(uint8_t* p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static void put16le(uint8_t* p, uint16_t v) { p[0]=v; p[1]=v>>8; }

static void audio_install_sample_wav(void) {
    fs_node_t* root = vfs_get_root();
    if (!root || vfs_finddir(root, "startup.wav")) return;

    /* Sized to fit the RAM-FS 4 KB per-file cap: 8 kHz mono ~0.25 s chime
     * (the resampler upsamples it to the 48 kHz device rate on playback). */
    const uint32_t rate   = 8000;
    const uint32_t frames = 2000;                 /* ~0.25 s               */
    const uint32_t dlen   = frames * 2;           /* mono 16-bit (4000 B)  */
    const uint32_t total  = 44 + dlen;            /* 4044 B < 4096         */

    uint8_t* wav = (uint8_t*)kmalloc(total);
    if (!wav) return;

    /* RIFF / WAVE header */
    wav[0]='R'; wav[1]='I'; wav[2]='F'; wav[3]='F';
    put32le(wav + 4, 36 + dlen);
    wav[8]='W'; wav[9]='A'; wav[10]='V'; wav[11]='E';
    wav[12]='f'; wav[13]='m'; wav[14]='t'; wav[15]=' ';
    put32le(wav + 16, 16);                        /* fmt chunk size        */
    put16le(wav + 20, 1);                         /* PCM                   */
    put16le(wav + 22, 1);                         /* mono                  */
    put32le(wav + 24, rate);
    put32le(wav + 28, rate * 2);                  /* byte rate             */
    put16le(wav + 32, 2);                         /* block align           */
    put16le(wav + 34, 16);                        /* bits/sample           */
    wav[36]='d'; wav[37]='a'; wav[38]='t'; wav[39]='a';
    put32le(wav + 40, dlen);

    /* Two notes (A5 then E6) with a linear fade-out. */
    int16_t* s = (int16_t*)(wav + 44);
    uint32_t half = frames / 2;
    audio_gen_tone(s,        half,          880, rate, 60);
    audio_gen_tone(s + half, frames - half, 1319, rate, 60);
    for (uint32_t i = 0; i < frames; i++) {
        int32_t fade = (int32_t)(frames - i) * 256 / frames;   /* 256..0 */
        s[i] = (int16_t)(((int32_t)s[i] * fade) >> 8);
    }

    fs_node_t* node = ramfs_create("startup.wav", FS_FILE);
    if (node) vfs_write(node, 0, total, wav);
    kfree(wav);
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
void audio_init(void) {
    g_dev = NULL;
    g_master = 80;
    g_frames_played = 0;
    memset(voices, 0, sizeof(voices));
    memset(voice_frac, 0, sizeof(voice_frac));

    audio_install_sample_wav();

    /* Note: the device driver (ac97_init) registers itself afterwards. */
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Audio mixer initialized (16-bit stereo, ");
    char b[12]; int_to_str(AUDIO_MAX_STREAMS, b); vga_print(b);
    vga_print(" voices), startup.wav ready\n");
}

void audio_register_device(sound_device_t* dev) {
    g_dev = dev;
    if (dev && dev->set_master) dev->set_master(g_master);
}

bool audio_have_device(void) { return g_dev && g_dev->ready && g_dev->ready(); }
const char* audio_device_name(void) { return g_dev ? g_dev->name : "none"; }
uint32_t audio_device_rate(void) { return out_rate(); }

/* --------------------------------------------------------------------------
 * Master volume
 * -------------------------------------------------------------------------- */
void audio_set_master_volume(uint8_t v) {
    if (v > 100) v = 100;
    g_master = v;
    if (g_dev && g_dev->set_master) g_dev->set_master(v);
}
uint8_t audio_get_master_volume(void) { return g_master; }

/* --------------------------------------------------------------------------
 * Mixer voice management
 * -------------------------------------------------------------------------- */
int audio_voice_add(const int16_t* data, uint32_t frames, uint16_t channels,
                    uint32_t rate, uint8_t volume, bool loop) {
    if (!data || frames == 0) return -1;
    if (channels != 1 && channels != 2) return -1;
    if (rate == 0) rate = AUDIO_OUT_RATE;
    if (volume > 100) volume = 100;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        if (!voices[i].active) {
            voices[i].data     = data;
            voices[i].frames   = frames;
            voices[i].pos      = 0;
            voices[i].rate     = rate;
            voices[i].channels = channels;
            voices[i].volume   = volume;
            voices[i].loop     = loop;
            voices[i].active   = true;
            voice_frac[i]      = 0;
            return i;
        }
    }
    return -1;   /* no free slot */
}

void audio_voice_clear(void) {
    memset(voices, 0, sizeof(voices));
    memset(voice_frac, 0, sizeof(voice_frac));
}

int audio_active_voices(void) {
    int n = 0;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) if (voices[i].active) n++;
    return n;
}

/* --------------------------------------------------------------------------
 * audio_render: produce `frames` of 16-bit stereo by mixing live voices.
 * Returns the number of voices that contributed (0 = silence).
 * -------------------------------------------------------------------------- */
static int audio_render(int16_t* out, uint32_t frames, uint32_t rate) {
    memset(out, 0, frames * AUDIO_OUT_CHANNELS * sizeof(int16_t));
    int contributed = 0;

    for (int v = 0; v < AUDIO_MAX_STREAMS; v++) {
        audio_voice_t* vc = &voices[v];
        if (!vc->active) continue;
        contributed++;

        /* Q16 source frames advanced per output frame. */
        uint32_t step = (vc->rate << 16) / rate;
        int32_t  gain = (int32_t)vc->volume * (int32_t)g_master;   /* 0..10000 */

        for (uint32_t f = 0; f < frames; f++) {
            if (!vc->active) break;
            uint32_t sp = vc->pos;
            int32_t l, r;
            if (vc->channels == 2) {
                l = vc->data[sp * 2];
                r = vc->data[sp * 2 + 1];
            } else {
                l = r = vc->data[sp];
            }
            l = (l * gain) / 10000;
            r = (r * gain) / 10000;
            out[f * 2]     = clamp16((int32_t)out[f * 2]     + l);
            out[f * 2 + 1] = clamp16((int32_t)out[f * 2 + 1] + r);

            /* advance source position (nearest-neighbour resample) */
            voice_frac[v] += step;
            vc->pos       += voice_frac[v] >> 16;
            voice_frac[v] &= 0xFFFF;
            if (vc->pos >= vc->frames) {
                if (vc->loop) { vc->pos = 0; voice_frac[v] = 0; }
                else          { vc->active = false; }
            }
        }
    }
    return contributed;
}

/* Sleep ~ms using the system tick (for the null-sink fallback). */
static void pace_ms(uint32_t ms) {
    uint32_t ticks = ms / TICK_MS;
    if (ticks == 0) ticks = 1;
    uint32_t start = system_ticks;
    while ((system_ticks - start) < ticks) __asm__ volatile("hlt");
}

/* --------------------------------------------------------------------------
 * audio_mix_run: render chunks and push to the device until every non-looping
 * voice has drained (or `max_ms` of wall time elapses — the runaway guard).
 * -------------------------------------------------------------------------- */
void audio_mix_run(uint32_t max_ms) {
    uint32_t rate = out_rate();
    uint32_t start = system_ticks;
    uint32_t budget_ticks = max_ms / TICK_MS + 1;
    bool have_dev = audio_have_device();

    while (audio_active_voices() > 0) {
        if ((system_ticks - start) >= budget_ticks) {
            audio_voice_clear();    /* time budget exhausted (e.g. a loop) */
            break;
        }
        audio_render(mixbuf, AUDIO_MIX_FRAMES, rate);
        if (have_dev) {
            g_dev->write(mixbuf, AUDIO_MIX_FRAMES);   /* blocks ~chunk dur */
        } else {
            pace_ms((AUDIO_MIX_FRAMES * 1000) / rate); /* null sink pacing */
        }
        g_frames_played += AUDIO_MIX_FRAMES;
    }
}

/* --------------------------------------------------------------------------
 * High-level playback
 * -------------------------------------------------------------------------- */
int audio_play_pcm(const int16_t* data, uint32_t frames, uint16_t channels,
                   uint32_t rate, uint8_t volume) {
    if (audio_voice_add(data, frames, channels, rate, volume, false) < 0)
        return -1;
    /* Worst-case duration + slack, capped to keep the guard sane. */
    uint32_t ms = (rate ? (frames * 1000) / rate : 1000) + 500;
    if (ms > 60000) ms = 60000;
    audio_mix_run(ms);
    return 0;
}

void audio_gen_tone(int16_t* buf, uint32_t frames, uint32_t freq_hz,
                    uint32_t rate, uint8_t amp_pct) {
    if (!buf || frames == 0 || rate == 0) return;
    if (amp_pct > 100) amp_pct = 100;
    uint32_t phase = 0;                          /* Q16 cycle fraction      */
    uint32_t step  = (freq_hz << 16) / rate;     /* cycle advance per frame */
    int32_t  amp   = (int32_t)32767 * amp_pct / 100;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t deg = (int32_t)((phase >> 6) & 1023) * 360 / 1024;
        buf[i] = clamp16((sin_q15(deg) * amp) / 32767);
        phase += step;
    }
}

int audio_play_tone(uint32_t freq_hz, uint32_t ms, uint8_t volume) {
    if (freq_hz == 0 || ms == 0) return -1;
    if (ms > 5000) ms = 5000;
    uint32_t rate = out_rate();

    /* Synthesize a small buffer of whole cycles and loop it for the duration,
     * rather than allocating the entire tone (keeps heap pressure tiny). */
    uint32_t period = rate / freq_hz; if (period == 0) period = 1;
    uint32_t cycles = (rate / 20) / period;        /* ~50 ms of waveform   */
    if (cycles == 0) cycles = 1;
    uint32_t frames = period * cycles;

    int16_t* buf = (int16_t*)kmalloc(frames * sizeof(int16_t));
    if (!buf) return -1;

    audio_gen_tone(buf, frames, freq_hz, rate, 70);   /* headroom below clip */
    audio_voice_clear();
    if (audio_voice_add(buf, frames, 1, rate, volume, true) < 0) { kfree(buf); return -1; }
    audio_mix_run(ms);                                 /* budget stops the loop */
    kfree(buf);
    return 0;
}

/* --------------------------------------------------------------------------
 * WAV / RIFF parsing
 * -------------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static bool tag4(const uint8_t* p, const char* s) {
    return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

bool wav_parse(const uint8_t* data, uint32_t size, wav_info_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!data || size < 44) return false;
    if (!tag4(data, "RIFF") || !tag4(data + 8, "WAVE")) return false;

    uint32_t off = 12;
    bool have_fmt = false, have_data = false;
    while (off + 8 <= size) {
        const uint8_t* ch = data + off;
        uint32_t clen = rd32(ch + 4);
        uint32_t body = off + 8;
        if (tag4(ch, "fmt ") && body + 16 <= size) {
            out->format   = rd16(data + body + 0);
            out->channels = rd16(data + body + 2);
            out->rate     = rd32(data + body + 4);
            out->bits     = rd16(data + body + 14);
            have_fmt = true;
        } else if (tag4(ch, "data")) {
            out->data_off = body;
            out->data_len = clen;
            if (out->data_off + out->data_len > size)        /* clamp to file */
                out->data_len = (size > out->data_off) ? size - out->data_off : 0;
            have_data = true;
        }
        /* chunks are word-aligned */
        off = body + clen + (clen & 1);
        if (have_fmt && have_data) break;
    }
    if (!have_fmt || !have_data) return false;
    if (out->channels != 1 && out->channels != 2) return false;
    if (out->bits != 8 && out->bits != 16) return false;
    if (out->format != 1) return false;   /* PCM only */

    uint32_t frame_bytes = out->channels * (out->bits / 8);
    out->frames = frame_bytes ? out->data_len / frame_bytes : 0;
    out->valid  = out->frames > 0;
    return out->valid;
}

int audio_play_wav(const uint8_t* data, uint32_t size, uint8_t volume) {
    wav_info_t w;
    if (!wav_parse(data, size, &w)) return -1;

    if (w.bits == 16) {
        /* Borrow the file's samples directly. */
        return audio_play_pcm((const int16_t*)(data + w.data_off),
                              w.frames, w.channels, w.rate, volume);
    }

    /* 8-bit unsigned PCM -> signed 16-bit (cap conversion size). */
    uint32_t samples = w.frames * w.channels;
    if (samples > AUDIO_OUT_RATE * 2u * 30u) samples = AUDIO_OUT_RATE * 2u * 30u;
    int16_t* buf = (int16_t*)kmalloc(samples * sizeof(int16_t));
    if (!buf) return -1;
    const uint8_t* src = data + w.data_off;
    for (uint32_t i = 0; i < samples; i++)
        buf[i] = (int16_t)(((int32_t)src[i] - 128) << 8);
    int rc = audio_play_pcm(buf, samples / w.channels, w.channels, w.rate, volume);
    kfree(buf);
    return rc;
}

/* --------------------------------------------------------------------------
 * Status
 * -------------------------------------------------------------------------- */
const char* audio_status(void) {
    /* "AC97 (codec ready), 48000 Hz, vol 80%" */
    char* p = g_status;
    const char* name = audio_device_name();
    while (*name) *p++ = *name++;
    if (audio_have_device()) {
        const char* s = " (ready), ";
        while (*s) *p++ = *s++;
    } else {
        const char* s = " (no codec), ";
        while (*s) *p++ = *s++;
    }
    char b[12];
    int_to_str((int)out_rate(), b);
    for (char* q = b; *q; q++) *p++ = *q;
    const char* hz = " Hz, vol ";
    while (*hz) *p++ = *hz++;
    int_to_str(g_master, b);
    for (char* q = b; *q; q++) *p++ = *q;
    *p++ = '%';
    *p = '\0';
    return g_status;
}

uint32_t audio_frames_played(void) { return g_frames_played; }
