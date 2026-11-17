/* ============================================================================
 * NexusOS — Intel AC'97 Audio Driver (Header) — Phase 38
 * ============================================================================
 * Driver for the Intel 82801AA AC'97 controller (QEMU's "-device AC97",
 * PCI 8086:2415, class 04:01). Programs the codec mixer through the NAM I/O
 * window and streams 16-bit stereo PCM out via the NABM bus-master engine
 * using a Buffer Descriptor List. Registers itself with the audio core
 * (audio.c) as the active sound device on a successful probe.
 * ============================================================================ */

#ifndef AC97_H
#define AC97_H

#include "types.h"

/* PCI identity (QEMU's AC97). */
#define AC97_VENDOR_INTEL   0x8086
#define AC97_DEVICE_82801AA 0x2415

/* Initialize: probe PCI, reset the codec, wire up the BDL, register with the
 * audio core. Safe to call even if no AC'97 device exists (it just no-ops). */
void ac97_init(void);

/* True once a codec has been found and brought up. */
bool ac97_present(void);

/* One-line status string for `sndinfo`. */
const char* ac97_status(void);

#endif /* AC97_H */
