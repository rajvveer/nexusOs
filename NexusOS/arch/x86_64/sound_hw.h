/* Hardware PCM engines behind the hardware-independent ALSA/mixing layer (sound.c): AC'97 (ac97.c) and Intel High
   Definition Audio (hda.c). The mixer owns an S16_LE stereo 48 kHz ring of SOUND_RING_FRAMES frames at physical address
   `ring` (identity-mapped, physically contiguous, page aligned); device frame d (an absolute, monotonic frame count)
   lives at ring index d % SOUND_RING_FRAMES. An engine plays that ring by DMA and reports how far it got.
   Capture runs the other way: the engine's input DMA writes S16_LE stereo 48 kHz cyclically into the capture ring at
   physical address sound_capture_ring (same size, ring index 0 first after capture_start) and capture_read reports the
   frames written since then; sound.c copies them out to the ALSA capture clients. */
#ifndef NEXUS_SOUND_HW_H
#define NEXUS_SOUND_HW_H
#include "native.h"

#define SOUND_RATE 48000U
#define SOUND_CHANNELS 2U
#define SOUND_FRAME_BYTES 4U
#define SOUND_RING_FRAMES 131072U
#define SOUND_CONTROLS 8U
#define SOUND_CARDS 4U        /* card 0: PCI (AC'97/HDA) or the first USB device; 1..3: USB Audio Class (uaudio.c) */

/* An ALSA mixer element (CTL ELEM_INFO/READ/WRITE/TLV_READ): BOOLEAN switches or INTEGER volumes with a dB scale. */
enum { SOUND_CONTROL_BOOLEAN = 1, SOUND_CONTROL_INTEGER = 2 };
struct sound_control {
    char name[44];                    /* "Capture Volume", "Capture Switch", "Mic Boost Volume", ... */
    uint8_t type, channels;           /* SOUND_CONTROL_*, 1 or 2 values */
    int32_t min, max, value[2];       /* value: current setting, applied by control_put */
    int32_t db_min, db_step;          /* integer controls: dB at min and per step, in 1/100 dB (TLV DB_SCALE); 0/0 = none */
    int32_t db_max;                   /* != 0 (with db_step != 0): TLV DB_MINMAX min..max instead (USB: non-integer 1/100 dB steps) */
};
struct sound_engine {
    /* ALSA identity: CTL CARD_INFO (id, driver, name, longname, mixername, components) and PCM INFO (id, pcm_name). */
    char id[16], driver[16], name[32], longname[80], mixer[80], components[128], pcm_name[48];
    /* false: the DMA stops after the last queued frame (AC'97 LVI). true: the DMA loops over the whole ring until
       stopped (HDA cyclic BDL), so frames the device passed must be cleared and the position is read regularly. */
    bool cyclic;
    void (*stop)(void);                                  /* halt the DMA (the next start begins afresh) */
    /* Start the DMA at device frame *queued (>= the mixer's played position) and let it play at least up to target;
       advances *queued over what was handed to the device. false: nothing could be queued (engine stays stopped). */
    bool (*start)(uint64_t* queued, uint64_t target);
    bool (*extend)(uint64_t* queued, uint64_t target);   /* running DMA: queue up to target; false if nothing was added */
    bool (*read)(uint64_t* position);                    /* DMA position; false: halted (consumed all queued frames) */
    bool (*full)(void);                                  /* optional: the engine cannot take more right now */
    void (*flush)(const void* data, uint64_t bytes);     /* optional: push CPU writes to ring memory out to RAM */
    void (*poll)(uint64_t now);                          /* optional: periodic housekeeping (jack sensing) */
    bool (*resume)(void);                                /* after a power loss (system sleep): bring the controller and
                                                            codec back with the DMA stopped; false: the engine is gone */
    /* Capture (optional; capture_start NULL = no capture device). */
    char capture_name[48];                               /* PCM INFO name of the capture device */
    bool (*capture_start)(void);                         /* start the input DMA at capture-ring index 0 */
    void (*capture_stop)(void);
    bool (*capture_read)(uint64_t* position);            /* frames written since capture_start; false: DMA stopped */
    void (*capture_flush)(const void* data, uint64_t bytes); /* optional: drop CPU cache lines before reading DMA data */
    /* Mixer controls; control_put(i) applies control[i].value (already range-checked) to the hardware. */
    unsigned controls;
    struct sound_control control[SOUND_CONTROLS];
    bool (*control_put)(unsigned index);
    void* context;                                       /* engine instance (USB cards): sound_engine_context() */
};
/* Physical address of the capture ring (sound.c sets it before probing; 0 = none). */
extern uint64_t sound_capture_ring;
/* Probe and initialise the hardware, fill *engine; ring = physical address of the mixer's DMA ring. */
bool ac97_probe(struct sound_engine* engine, uint64_t ring);
bool hda_probe(struct sound_engine* engine, uint64_t ring);
/* Hot-plugged cards (USB Audio Class, USB worker): add returns the card index (-1: no slot/memory) and hands out the
   card's S16 stereo 48 kHz mixer ring the engine plays from and the capture ring it fills (both SOUND_RING_FRAMES,
   kernel memory, CPU-copied); remove disconnects open streams (-ENODEV) and frees the slot when the last one closes.
   While sound.c calls into an engine, sound_engine_context() is that engine's context pointer. */
int sound_card_add(const struct sound_engine* engine, int16_t** playback, int16_t** capture);
void sound_card_remove(int index);
void* sound_engine_context(void);
const struct sound_control* sound_engine_control(unsigned index);   /* the card's live control (values set by ALSA) */
bool sound_card_present(unsigned index);
bool sound_node_available(unsigned node);                 /* node = card * 3 + (0 control, 1 playback, 2 capture) */
extern void (*sound_cards_changed)(unsigned card, bool added);
/* "<label>0x<16 hex digits>" on the kernel log (sound.c). */
void sound_log_hex(const char* label, uint64_t value);
#endif
