/* ============================================================================
 * NexusOS — PS/2 Mouse Driver (Implementation)
 * ============================================================================
 * Handles IRQ12 for PS/2 mouse input.
 * The PS/2 mouse sends 3-byte packets:
 *   Byte 0: [Y overflow][X overflow][Y sign][X sign][1][Middle][Right][Left]
 *   Byte 1: X movement (delta)
 *   Byte 2: Y movement (delta)
 *
 * Cursor is tracked in character-cell coordinates (0-79, 0-24).
 * Movement deltas are accumulated and scaled to character cells.
 * ============================================================================ */

#include "mouse.h"
#include "port.h"
#include "idt.h"
#include "vga.h"
#include "gui.h"
#include "framebuffer.h"

/* PS/2 controller ports */
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

/* PS/2 controller commands */
#define PS2_CMD_ENABLE_AUX   0xA8   /* Enable auxiliary (mouse) device */
#define PS2_CMD_GET_COMPAQ   0x20   /* Read controller config byte */
#define PS2_CMD_SET_COMPAQ   0x60   /* Write controller config byte */
#define PS2_CMD_WRITE_MOUSE  0xD4   /* Send next byte to mouse */

/* Mouse commands */
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_SET_RATE      0xF3

/* Mouse sensitivity — 1 = direct 1:1 pixel mapping */
#define MOUSE_SENSITIVITY 1

/* Mouse state */
static mouse_state_t state;

/* Packet assembly */
static uint8_t  mouse_cycle = 0;     /* Which byte of 3-byte packet we're on */
static uint8_t  mouse_packet[3];     /* Assembled packet */
static int      accum_x = 0;         /* Sub-cell X accumulator */
static int      accum_y = 0;         /* Sub-cell Y accumulator */

/* --------------------------------------------------------------------------
 * ps2_wait_input: Wait until PS/2 controller input buffer is empty
 * (ready to accept a command)
 * -------------------------------------------------------------------------- */
static void ps2_wait_input(void) {
    int timeout = 100000;
    while (timeout--) {
        if (!(port_byte_in(PS2_STATUS_PORT) & 0x02)) return;
    }
}

/* --------------------------------------------------------------------------
 * ps2_wait_output: Wait until PS/2 controller output buffer has data
 * -------------------------------------------------------------------------- */
static void ps2_wait_output(void) {
    int timeout = 100000;
    while (timeout--) {
        if (port_byte_in(PS2_STATUS_PORT) & 0x01) return;
    }
}

/* --------------------------------------------------------------------------
 * ps2_write_mouse: Send a command byte to the mouse device
 * -------------------------------------------------------------------------- */
static void ps2_write_mouse(uint8_t data) {
    ps2_wait_input();
    port_byte_out(PS2_COMMAND_PORT, PS2_CMD_WRITE_MOUSE);
    ps2_wait_input();
    port_byte_out(PS2_DATA_PORT, data);

    /* Wait for ACK (0xFA) */
    ps2_wait_output();
    port_byte_in(PS2_DATA_PORT);
}

/* --------------------------------------------------------------------------
 * mouse_callback: IRQ12 interrupt handler
 * Assembles 3-byte packets and updates cursor position.
 * -------------------------------------------------------------------------- */
static void mouse_callback(struct registers* regs) {
    (void)regs;

    uint8_t status = port_byte_in(PS2_STATUS_PORT);

    /* Check that data is from the mouse (bit 5 set) and output buffer full */
    if (!(status & 0x20)) return;
    if (!(status & 0x01)) return;

    uint8_t data = port_byte_in(PS2_DATA_PORT);

    switch (mouse_cycle) {
        case 0:
            /* Byte 0: status byte — verify bit 3 is always set */
            if (data & 0x08) {
                mouse_packet[0] = data;
                mouse_cycle = 1;
            }
            /* If bit 3 not set, discard (out of sync) */
            break;

        case 1:
            /* Byte 1: X delta */
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;

        case 2:
            /* Byte 2: Y delta — packet complete */
            mouse_packet[2] = data;
            mouse_cycle = 0;

            /* Decode buttons */
            state.buttons = mouse_packet[0] & 0x07;
            if (mouse_packet[0] & 0x07) {
                state.clicked = true;
            }

            /* Decode X movement (sign-extend if negative) */
            int dx = mouse_packet[1];
            if (mouse_packet[0] & 0x10) {
                dx |= 0xFFFFFF00;  /* Sign extend */
            }

            /* Decode Y movement (sign-extend, inverted for screen coords) */
            int dy = mouse_packet[2];
            if (mouse_packet[0] & 0x20) {
                dy |= 0xFFFFFF00;  /* Sign extend */
            }

            /* Discard if overflow */
            if (mouse_packet[0] & 0xC0) break;

            /* Accumulate sub-cell movement */
            accum_x += dx;
            accum_y -= dy;  /* Y is inverted (mouse up = screen up = lower row) */

            if (fb_is_vesa()) {
                /* True pixel movement in VESA mode */
                int p_dx = accum_x / MOUSE_SENSITIVITY;
                int p_dy = accum_y / MOUSE_SENSITIVITY;
                if (p_dx != 0 || p_dy != 0) {
                    state.px += p_dx;
                    state.py += p_dy;
                    accum_x %= MOUSE_SENSITIVITY;
                    accum_y %= MOUSE_SENSITIVITY;

                    if (state.px < 0) state.px = 0;
                    if (state.px >= (int)fb_get_width()) state.px = fb_get_width() - 1;
                    if (state.py < 0) state.py = 0;
                    if (state.py >= (int)fb_get_height()) state.py = fb_get_height() - 1;

                    /* Downmap to character cells for hit testing */
                    state.x = state.px / 8;
                    state.y = state.py / 16;
                    state.moved = true;
                }
            } else {
                /* Original character cell based movement for VGA */
            int cell_dx = accum_x / MOUSE_SENSITIVITY;
            int cell_dy = accum_y / MOUSE_SENSITIVITY;

            if (cell_dx != 0 || cell_dy != 0) {
                state.x += cell_dx;
                state.y += cell_dy;
                accum_x %= MOUSE_SENSITIVITY;
                accum_y %= MOUSE_SENSITIVITY;

                /* Clamp to screen bounds */
                if (state.x < 0) state.x = 0;
                if (state.x >= GUI_WIDTH) state.x = GUI_WIDTH - 1;
                if (state.y < 0) state.y = 0;
                if (state.y >= GUI_HEIGHT) state.y = GUI_HEIGHT - 1;

                /* Extrapolate pixel coords purely for consistency */
                state.px = state.x * 8;
                state.py = state.y * 16;
                state.moved = true;
            }
            }
            break;
    }
}

/* --------------------------------------------------------------------------
 * mouse_init: Initialize the PS/2 mouse
 * -------------------------------------------------------------------------- */
void mouse_init(void) {
    /* Start cursor at center of screen */
    state.x = GUI_WIDTH / 2;
    state.y = GUI_HEIGHT / 2;
    state.px = state.x * 8;
    state.py = state.y * 16;
    state.buttons = 0;
    state.moved = false;
    state.clicked = false;
    mouse_cycle = 0;
    accum_x = 0;
    accum_y = 0;

    /* Enable the auxiliary (mouse) device */
    ps2_wait_input();
    port_byte_out(PS2_COMMAND_PORT, PS2_CMD_ENABLE_AUX);

    /* Enable IRQ12 in the PS/2 controller config byte */
    ps2_wait_input();
    port_byte_out(PS2_COMMAND_PORT, PS2_CMD_GET_COMPAQ);
    ps2_wait_output();
    uint8_t config = port_byte_in(PS2_DATA_PORT);
    config |= 0x02;   /* Bit 1: enable IRQ12 */
    config &= ~0x20;  /* Bit 5: clear disable flag for mouse clock */
    ps2_wait_input();
    port_byte_out(PS2_COMMAND_PORT, PS2_CMD_SET_COMPAQ);
    ps2_wait_input();
    port_byte_out(PS2_DATA_PORT, config);

    /* Send default settings to mouse */
    ps2_write_mouse(MOUSE_CMD_SET_DEFAULTS);

    /* Enable mouse packet streaming */
    ps2_write_mouse(MOUSE_CMD_ENABLE);

    /* Register IRQ12 handler (IRQ12 = interrupt 44) */
    register_interrupt_handler(44, mouse_callback);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Mouse driver initialized (PS/2, IRQ12)\n");
}

/* --------------------------------------------------------------------------
 * mouse_get_state: Return current mouse state
 * -------------------------------------------------------------------------- */
mouse_state_t mouse_get_state(void) {
    return state;
}

/* --------------------------------------------------------------------------
 * mouse_clear_events: Reset event flags
 * -------------------------------------------------------------------------- */
void mouse_clear_events(void) {
    state.moved = false;
    state.clicked = false;
}

/* --------------------------------------------------------------------------
 * mouse_has_event: Check for pending events
 * -------------------------------------------------------------------------- */
bool mouse_has_event(void) {
    return state.moved || state.clicked;
}
