/* ============================================================================
 * NexusOS — PC Speaker Driver (Header)
 * ============================================================================
 * Uses PIT Channel 2 + port 0x61 to play tones.
 * ============================================================================ */

#ifndef SPEAKER_H
#define SPEAKER_H

#include "types.h"

/* Play a tone at given frequency (Hz) for duration (in timer ticks ~55ms each) */
void beep(uint32_t frequency, uint32_t duration_ticks);

/* Stop any playing sound */
void beep_stop(void);

/* Startup jingle */
void play_boot_sound(void);

/* Short UI feedback sounds */
void play_error_sound(void);
void play_success_sound(void);

/* Initialize speaker */
void speaker_init(void);

#endif /* SPEAKER_H */
