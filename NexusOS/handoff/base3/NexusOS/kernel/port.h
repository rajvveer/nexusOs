/* ============================================================================
 * NexusOS — Port I/O Utilities
 * ============================================================================
 * Inline functions for reading/writing to hardware I/O ports.
 * x86 uses IN/OUT instructions to communicate with hardware.
 * ============================================================================ */

#ifndef PORT_H
#define PORT_H

#include "types.h"

/* Read a byte from an I/O port */
static inline uint8_t port_byte_in(uint16_t port) {
    uint8_t result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* Write a byte to an I/O port */
static inline void port_byte_out(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

/* Read a word (16 bits) from an I/O port */
static inline uint16_t port_word_in(uint16_t port) {
    uint16_t result;
    __asm__ volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* Write a word (16 bits) to an I/O port */
static inline void port_word_out(uint16_t port, uint16_t data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

/* Small I/O delay (used when hardware needs time to react) */
static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/* Read a dword (32 bits) from an I/O port */
static inline uint32_t port_dword_in(uint16_t port) {
    uint32_t result;
    __asm__ volatile("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* Write a dword (32 bits) to an I/O port */
static inline void port_dword_out(uint16_t port, uint32_t data) {
    __asm__ volatile("outl %0, %1" : : "a"(data), "Nd"(port));
}

#endif /* PORT_H */
