/* ============================================================================
 * NexusOS — PC Speaker Driver (Implementation)
 * ============================================================================
 * Controls the PC speaker through PIT Channel 2 and port 0x61.
 * PIT base frequency = 1,193,180 Hz. Divider = base / desired_freq.
 * ============================================================================ */

#include "speaker.h"
#include "vga.h"
#include "port.h"

/* PIT ports */
#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43
#define SPEAKER_PORT  0x61
#define PIT_FREQUENCY 1193180

/* System tick counter (from kernel.c) */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * beep: Play a tone at the given frequency for duration ticks
 * -------------------------------------------------------------------------- */
void beep(uint32_t frequency, uint32_t duration_ticks) {
    if (frequency == 0) return;

    /* Set PIT Channel 2 to the desired frequency */
    uint32_t divisor = PIT_FREQUENCY / frequency;

    /* Configure PIT Channel 2: square wave, lobyte/hibyte */
    port_byte_out(PIT_COMMAND, 0xB6);
    port_byte_out(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    port_byte_out(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    /* Enable speaker (bits 0 and 1 of port 0x61) */
    uint8_t tmp = port_byte_in(SPEAKER_PORT);
    port_byte_out(SPEAKER_PORT, tmp | 0x03);

    /* Wait for duration */
    uint32_t start = system_ticks;
    while ((system_ticks - start) < duration_ticks) {
        __asm__ volatile("hlt");
    }

    /* Disable speaker */
    tmp = port_byte_in(SPEAKER_PORT);
    port_byte_out(SPEAKER_PORT, tmp & 0xFC);
}

/* --------------------------------------------------------------------------
 * beep_stop: Force speaker off
 * -------------------------------------------------------------------------- */
void beep_stop(void) {
    uint8_t tmp = port_byte_in(SPEAKER_PORT);
    port_byte_out(SPEAKER_PORT, tmp & 0xFC);
}

/* --------------------------------------------------------------------------
 * play_boot_sound: Startup jingle (ascending tones)
 * -------------------------------------------------------------------------- */
void play_boot_sound(void) {
    beep(523, 2);   /* C5  ~110ms */
    beep(659, 2);   /* E5  ~110ms */
    beep(784, 2);   /* G5  ~110ms */
    beep(1047, 3);  /* C6  ~165ms */
}

/* --------------------------------------------------------------------------
 * play_error_sound: Low buzz for errors
 * -------------------------------------------------------------------------- */
void play_error_sound(void) {
    beep(200, 3);
}

/* --------------------------------------------------------------------------
 * play_success_sound: Quick high beep
 * -------------------------------------------------------------------------- */
void play_success_sound(void) {
    beep(880, 2);
}

/* --------------------------------------------------------------------------
 * speaker_init: Initialize speaker
 * -------------------------------------------------------------------------- */
void speaker_init(void) {
    beep_stop();
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("PC speaker initialized\n");
}
