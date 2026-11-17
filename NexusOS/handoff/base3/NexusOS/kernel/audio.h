/* ============================================================================
 * NexusOS — Audio Subsystem (Header) — Phase 38
 * ============================================================================
 * Device-agnostic audio core: a software mixer that blends multiple 16-bit PCM
 * streams (with per-stream + master volume), a WAV/RIFF parser, and a tone
 * generator. It drives a registered sound device (see ac97.c) through a small
 * ops interface — the audio analogue of net.c sitting above rtl8139.c.
 *
 * Device output format is fixed at signed 16-bit stereo interleaved; the mixer
 * up-mixes mono and nearest-neighbour resamples source streams to the device
 * rate. If no sound device is present, the core falls back to a timed "null
 * sink" so playback commands still behave (and pace) correctly.
 * ============================================================================ */

#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"

/* Device native output: 16-bit stereo. */
#define AUDIO_OUT_CHANNELS   2
#define AUDIO_OUT_RATE       48000      /* default device rate (Hz)            */
#define AUDIO_MAX_STREAMS    8          /* simultaneous mixer voices           */
#define AUDIO_MIX_FRAMES     4096       /* frames rendered per mix chunk       */

/* ----------------------------------------------------------------------------
 * Sound device interface — a driver (ac97.c) fills this and registers it.
 * `write` consumes interleaved 16-bit stereo frames at the device rate and
 * blocks until they have been handed to the hardware DMA engine.
 * -------------------------------------------------------------------------- */
typedef struct {
    const char* name;
    bool     (*ready)(void);                                   /* codec up?     */
    int      (*set_rate)(uint32_t hz);                         /* 0 on success  */
    int      (*write)(const int16_t* stereo, uint32_t frames); /* frames played */
    void     (*set_master)(uint8_t vol_0_100);                 /* hw master vol */
    uint32_t (*rate)(void);                                    /* current rate  */
} sound_device_t;

/* ----------------------------------------------------------------------------
 * Mixer voice — a single PCM stream being played. Source samples are borrowed
 * (not copied); the caller must keep them alive while the voice is active.
 * -------------------------------------------------------------------------- */
typedef struct {
    const int16_t* data;      /* interleaved source samples                    */
    uint32_t       frames;    /* total source frames                           */
    uint32_t       pos;       /* current source frame                          */
    uint32_t       rate;      /* source sample rate (Hz)                       */
    uint16_t       channels;  /* 1 = mono, 2 = stereo                          */
    uint8_t        volume;    /* 0..100 per-voice volume                       */
    bool           loop;      /* restart at end?                               */
    bool           active;    /* slot in use?                                  */
} audio_voice_t;

/* Parsed WAV header summary. */
typedef struct {
    bool     valid;
    uint16_t format;          /* 1 = PCM                                       */
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;            /* bits per sample (8 or 16)                     */
    uint32_t data_off;        /* byte offset of PCM data within the file       */
    uint32_t data_len;        /* PCM data length in bytes                      */
    uint32_t frames;          /* number of sample frames                       */
} wav_info_t;

/* --- Lifecycle ---------------------------------------------------------------*/
void audio_init(void);
void audio_register_device(sound_device_t* dev);
bool audio_have_device(void);
const char* audio_device_name(void);
uint32_t    audio_device_rate(void);

/* --- Master volume (0..100) --------------------------------------------------*/
void    audio_set_master_volume(uint8_t v);
uint8_t audio_get_master_volume(void);

/* --- Playback (all blocking; mix the live voices to the device) --------------*/
/* Add a one-shot PCM voice and block until every active voice has finished.   */
int audio_play_pcm(const int16_t* data, uint32_t frames, uint16_t channels,
                   uint32_t rate, uint8_t volume);
/* Generate and play a sine tone. */
int audio_play_tone(uint32_t freq_hz, uint32_t ms, uint8_t volume);
/* Parse a WAV blob and play its PCM data. */
int audio_play_wav(const uint8_t* data, uint32_t size, uint8_t volume);

/* Fill `buf` with `frames` of a mono 16-bit sine at freq_hz, amplitude
 * amp_pct (0..100) of full scale. Used to build mixer demo voices. */
void audio_gen_tone(int16_t* buf, uint32_t frames, uint32_t freq_hz,
                    uint32_t rate, uint8_t amp_pct);

/* --- Lower-level mixer control (for multi-stream demos) ----------------------*/
int  audio_voice_add(const int16_t* data, uint32_t frames, uint16_t channels,
                     uint32_t rate, uint8_t volume, bool loop);
void audio_voice_clear(void);
int  audio_active_voices(void);
/* Pump the mixer until all non-looping voices drain (or `max_ms` elapses).     */
void audio_mix_run(uint32_t max_ms);

/* --- WAV helper --------------------------------------------------------------*/
bool wav_parse(const uint8_t* data, uint32_t size, wav_info_t* out);

/* --- Status ------------------------------------------------------------------*/
const char* audio_status(void);
uint32_t    audio_frames_played(void);

#endif /* AUDIO_H */
