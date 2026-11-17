/* ============================================================================
 * NexusOS — PIC Driver (8259 Programmable Interrupt Controller)
 * ============================================================================ */

#ifndef PIC_H
#define PIC_H

#include "types.h"

/* PIC I/O ports */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

/* End of Interrupt command */
#define PIC_EOI      0x20

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif /* PIC_H */
