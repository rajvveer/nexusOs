/* ============================================================================
 * NexusOS — Video Subsystem (Header) — Phase 40
 * ============================================================================
 * A RIFF/AVI container parser + media player. Decodes Motion-JPEG video frames
 * through the Phase 39 JPEG decoder, renders them with the Phase 39 framebuffer
 * presenter, and plays the PCM audio stream through the Phase 38 AC'97 mixer —
 * advancing video off the audio clock for A/V sync.
 * ============================================================================ */

#ifndef VIDEO_H
#define VIDEO_H

#include "types.h"

typedef struct {
    bool     valid;
    int      width, height;
    int      frame_count;
    uint32_t us_per_frame;     /* microseconds per video frame   */
    char     vcodec[5];        /* e.g. "MJPG"                     */
    bool     has_audio;
    int      audio_rate;
    int      audio_channels;
    int      audio_bits;
    int      audio_chunks;
} avi_info_t;

/* Parse the AVI header + stream metadata (does not decode frames). */
bool avi_parse(const uint8_t* data, uint32_t size, avi_info_t* out);

/* Decode + play the clip fullscreen until it ends or a key is pressed. */
void video_play(const uint8_t* data, uint32_t size, const char* title);

/* The embedded demo clip (kernel/sample_video.h). */
const uint8_t* video_demo(uint32_t* size_out);

/* One-time init (reports the embedded demo). */
void video_init(void);

#endif /* VIDEO_H */
