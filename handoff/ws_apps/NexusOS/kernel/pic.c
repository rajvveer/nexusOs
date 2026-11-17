/* ============================================================================
 * NexusOS — PIC Driver (Implementation)
 * ============================================================================
 * Remaps the 8259 PIC so IRQs don't conflict with CPU exceptions.
 * Default: IRQ 0-7 → INT 8-15 (conflicts with exceptions!)
 * Remapped: IRQ 0-7 → INT 32-39, IRQ 8-15 → INT 40-47
 * ============================================================================ */

#include "pic.h"
#include "port.h"
#include "vga.h"

/* --------------------------------------------------------------------------
 * pic_init: Remap PIC1 & PIC2 to non-conflicting interrupt numbers
 * -------------------------------------------------------------------------- */
void pic_init(void) {
    /* Save current masks */
    uint8_t mask1 = port_byte_in(PIC1_DATA);
    uint8_t mask2 = port_byte_in(PIC2_DATA);

    /* ICW1: Start initialization sequence (cascade mode) */
    port_byte_out(PIC1_COMMAND, 0x11);
    io_wait();
    port_byte_out(PIC2_COMMAND, 0x11);
    io_wait();

    /* ICW2: Set interrupt vector offsets */
    port_byte_out(PIC1_DATA, 0x20);    /* PIC1 starts at INT 32 */
    io_wait();
    port_byte_out(PIC2_DATA, 0x28);    /* PIC2 starts at INT 40 */
    io_wait();

    /* ICW3: Tell PICs about each other */
    port_byte_out(PIC1_DATA, 0x04);    /* PIC1: slave on IRQ2 */
    io_wait();
    port_byte_out(PIC2_DATA, 0x02);    /* PIC2: cascade identity */
    io_wait();

    /* ICW4: Set 8086 mode */
    port_byte_out(PIC1_DATA, 0x01);
    io_wait();
    port_byte_out(PIC2_DATA, 0x01);
    io_wait();

    /* Restore masks (unmask all for now) */
    port_byte_out(PIC1_DATA, 0x00);
    port_byte_out(PIC2_DATA, 0x00);

    (void)mask1;
    (void)mask2;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("PIC remapped (IRQ 0-15 -> INT 32-47)\n");
}

/* --------------------------------------------------------------------------
 * pic_send_eoi: Send End Of Interrupt signal
 * -------------------------------------------------------------------------- */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        port_byte_out(PIC2_COMMAND, PIC_EOI);
    }
    port_byte_out(PIC1_COMMAND, PIC_EOI);
}

/* --------------------------------------------------------------------------
 * pic_set_mask: Mask (disable) a specific IRQ line
 * -------------------------------------------------------------------------- */
void pic_set_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = port_byte_in(port) | (1 << irq);
    port_byte_out(port, value);
}

/* --------------------------------------------------------------------------
 * pic_clear_mask: Unmask (enable) a specific IRQ line
 * -------------------------------------------------------------------------- */
void pic_clear_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = port_byte_in(port) & ~(1 << irq);
    port_byte_out(port, value);
}
