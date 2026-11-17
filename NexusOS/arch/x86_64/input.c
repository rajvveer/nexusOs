/* PS/2 hardware, with independent evdev readers and bounded IRQ work. */
#include "native.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define QUEUE_SIZE 256
#define KEY_BYTES 96
struct queued_event { uint64_t time; uint16_t type, code; int32_t value; };
struct input_event64 { uint64_t seconds, microseconds; uint16_t type, code; int32_t value; };
struct input_queue { struct queued_event events[QUEUE_SIZE]; uint64_t sequence; uint8_t keys[KEY_BYTES]; bool present; };
static struct input_queue inputs[2];
static uint64_t realtime_offset, mouse_time;
static unsigned prefix, pause_byte, mouse_used, mouse_type;
static uint8_t mouse_packet[4];
static bool resuming;   /* input_resume: program the controller again, the IRQ routes stay */
_Static_assert(sizeof(struct input_event64) == 24, "x86-64 input event ABI");

static bool send_port(uint16_t port, uint8_t byte) {
    uint64_t deadline = native_clock_ns() + 100000000;
    do {
        if (!(port_byte_in(0x64) & 2)) { port_byte_out(port, byte); return true; }
        __asm__ volatile("pause");
    } while (native_clock_ns() < deadline);
    return false;
}
static bool receive(bool mouse, uint8_t* byte) {
    uint64_t deadline = native_clock_ns() + 100000000;
    do {
        uint8_t status = port_byte_in(0x64);
        if (status & 1) {
            uint8_t data = port_byte_in(0x60);
            if (!(status & 0xC0) && !!(status & 0x20) == mouse) { *byte = data; return true; }
        }
        __asm__ volatile("pause");
    } while (native_clock_ns() < deadline);
    return false;
}
static bool command(bool mouse, uint8_t byte) {
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        uint8_t reply;
        if ((mouse && !send_port(0x64, 0xD4)) || !send_port(0x60, byte) || !receive(mouse, &reply)) return false;
        if (reply == 0xFA) return true;
        if (reply != 0xFE) return false;
    }
    return false;
}
static bool mouse_rate(unsigned rate) { return command(true, 0xF3) && command(true, rate); }
static void input_vector(unsigned vector, void* context) { (void)vector; (void)context; input_irq(); }
void input_init(void) {
    uint8_t config, id = 0;
    realtime_offset = native_realtime_ns() - native_clock_ns();
    if (!send_port(0x64, 0xAD) || !send_port(0x64, 0xA7)) return;
    for (unsigned i = 0; i < 64 && (port_byte_in(0x64) & 1); i++) port_byte_in(0x60);
    if (!send_port(0x64, 0x20) || !receive(false, &config)) return;
    config = (config | 0x40) & ~3U;
    if (!send_port(0x64, 0x60) || !send_port(0x60, config)) return;
    bool keyboard = send_port(0x64, 0xAE) && command(false, 0xF5) && command(false, 0xF0) && command(false, 2);
    bool mouse = send_port(0x64, 0xA8) && command(true, 0xF5) && command(true, 0xF6);
    if (mouse) mouse = mouse_rate(200) && mouse_rate(100) && mouse_rate(80) && command(true, 0xF2) && receive(true, &id);
    if (mouse && id == 3) mouse = mouse_rate(200) && mouse_rate(200) && mouse_rate(80) && command(true, 0xF2) && receive(true, &id);
    if (mouse && id != 0 && id != 3 && id != 4) mouse = false;
    mouse_type = id;
    if (mouse) mouse = command(true, 0xF4);
    if (keyboard) keyboard = command(false, 0xF4);
    config = (config & ~0x33U) | (keyboard ? 1 : 0x10) | (mouse ? 2 : 0x20);
    if (!send_port(0x64, 0x60) || !send_port(0x60, config)) return;
    inputs[0].present = keyboard; inputs[1].present = mouse;
    if (keyboard && !resuming && irq_attach_isa(1, 0, input_vector, NULL) < 0) keyboard = inputs[0].present = false;
    if (mouse && !resuming && irq_attach_isa(12, 0, input_vector, NULL) < 0) mouse = inputs[1].present = false;
    vga_print(keyboard ? "[64] PS/2 keyboard ready\n" : "[64] PS/2 keyboard unavailable\n");
    vga_print(mouse ? "[64] PS/2 mouse ready\n" : "[64] PS/2 mouse unavailable\n");
}
static void push(unsigned index, unsigned type, unsigned code, int value) {
    struct input_queue* q = &inputs[index];
    q->events[q->sequence++ % QUEUE_SIZE] = (struct queued_event){native_clock_ns(), type, code, value};
}
static void report(unsigned index) {
    push(index, 0, 0, 0); task_wake_channel(&inputs[index]); event_source_changed(&inputs[index], 1);
}
static void lost(unsigned index) {
    memset(inputs[index].keys, 0, KEY_BYTES); push(index, 0, 3, 0); report(index);
}
static bool key(unsigned index, unsigned code, bool down) {
    uint8_t* byte = &inputs[index].keys[code / 8]; unsigned mask = 1U << (code % 8); bool old = (*byte & mask) != 0;
    if (!down && !old) return false;
    if (down) *byte |= mask; else *byte &= ~mask;
    push(index, 1, code, down ? (old ? 2 : 1) : 0); return true;
}
static unsigned keycode(unsigned scan, bool extended) {
    if (!extended) return (scan >= 1 && scan <= 83) || (scan >= 86 && scan <= 88) ? scan : scan == 84 ? 99 : 0;
    switch (scan) {
    case 0x1C: return 96; case 0x1D: return 97; case 0x35: return 98; case 0x37: return 99; case 0x38: return 100;
    case 0x47: return 102; case 0x48: return 103; case 0x49: return 104; case 0x4B: return 105; case 0x4D: return 106;
    case 0x4F: return 107; case 0x50: return 108; case 0x51: return 109; case 0x52: return 110; case 0x53: return 111;
    case 0x5B: return 125; case 0x5C: return 126; case 0x5D: return 127;
    case 0x10: return 165; case 0x19: return 163; case 0x20: return 113; case 0x22: return 164; case 0x24: return 166;
    case 0x2E: return 114; case 0x30: return 115; case 0x32: return 150; case 0x5E: return 116; case 0x5F: return 142; case 0x63: return 143;
    default: return 0;
    }
}
static void keyboard_byte(uint8_t byte) {
    if (!byte || byte == 0xFF) { prefix = pause_byte = 0; lost(0); return; }
    if (pause_byte) {
        const uint8_t sequence[] = {0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
        if (byte == sequence[pause_byte]) {
            if (++pause_byte == sizeof(sequence)) { pause_byte = 0; key(0, 119, true); report(0); key(0, 119, false); report(0); }
            return;
        }
        pause_byte = 0;
    }
    if (byte == 0xE1) { prefix = 0; pause_byte = 1; return; }
    if (byte == 0xE0) { prefix = 1; return; }
    unsigned code = keycode(byte & 127, prefix != 0); prefix = 0;
    if (code && key(0, code, !(byte & 128))) report(0);
}
static void mouse_byte(uint8_t byte) {
    uint64_t now = native_clock_ns(); if (mouse_used && now - mouse_time > 100000000) mouse_used = 0; mouse_time = now;
    if (!mouse_used && !(byte & 8)) return;
    mouse_packet[mouse_used++] = byte;
    if (mouse_used < (mouse_type ? 4U : 3U)) return;
    mouse_used = 0; unsigned flags = mouse_packet[0];
    for (unsigned i = 0; i < (mouse_type == 4 ? 5U : 3U); i++) {
        bool down = i < 3 ? (flags & (1U << i)) != 0 : (mouse_packet[3] & (1U << (i + 1))) != 0;
        bool old = (inputs[1].keys[(272 + i) / 8] & (1U << (i % 8))) != 0;
        if (old != down) key(1, 272 + i, down);
    }
    if (!(flags & 0xC0)) {
        int x = mouse_packet[1] - ((flags & 0x10) ? 256 : 0), y = mouse_packet[2] - ((flags & 0x20) ? 256 : 0);
        if (x) push(1, 2, 0, x);
        if (y) push(1, 2, 1, -y);
    }
    if (mouse_type) {
        int wheel = mouse_type == 4 ? (mouse_packet[3] & 15) - ((mouse_packet[3] & 8) ? 16 : 0) : (int8_t)mouse_packet[3];
        if (wheel) push(1, 2, 8, -wheel);
    }
    report(1);
}
void input_irq(void) {
    for (unsigned i = 0; i < 64; i++) {
        uint8_t status = port_byte_in(0x64); if (!(status & 1)) break;
        uint8_t byte = port_byte_in(0x60); unsigned index = (status & 0x20) ? 1 : 0;
        if (!inputs[index].present) continue;
        if (status & 0xC0) { if (index) mouse_used = 0; else prefix = pause_byte = 0; lost(index); }
        else if (index) mouse_byte(byte); else keyboard_byte(byte);
    }
}
bool input_available(unsigned index) { return index < 2 && inputs[index].present; }
/* Early-boot console prompts (console_prompt.c): the next key event after *sequence (kernel callers, no reader). */
uint64_t input_sequence(unsigned index) { return index < 2 ? inputs[index].sequence : 0; }
bool input_next_key(unsigned index, uint64_t* sequence, unsigned* code, int* value) {
    if (index > 1) return false;
    struct input_queue* q = &inputs[index];
    if (q->sequence - *sequence > QUEUE_SIZE) *sequence = q->sequence - QUEUE_SIZE;
    while (*sequence != q->sequence) {
        struct queued_event e = q->events[(*sequence)++ % QUEUE_SIZE];
        if (e.type == 1) { *code = e.code; *value = e.value; return true; }
    }
    return false;
}
void input_open(unsigned index, struct input_reader* reader) { *reader = (struct input_reader){.sequence = inputs[index].sequence}; }
void* input_source(unsigned index) { return &inputs[index]; }
uint32_t input_ready(unsigned index, const struct input_reader* reader) { return reader->report || reader->sequence != inputs[index].sequence ? 1 : 0; }
int64_t input_read(unsigned index, struct input_reader* reader, uint64_t buffer, uint64_t count, bool nonblock) {
    if (!count) return 0;
    if (count < sizeof(struct input_event64)) return -22;
    while (!input_ready(index, reader)) {
        if (nonblock) return -11;
        int error = task_wait_channel(&inputs[index]); if (error) return error;
    }
    uint64_t amount = count / sizeof(struct input_event64); if (amount > QUEUE_SIZE) amount = QUEUE_SIZE;
    unsigned used = 0; struct input_queue* q = &inputs[index];
    while (used < amount && input_ready(index, reader)) {
        struct queued_event event; struct input_reader next = *reader;
        if (q->sequence - next.sequence > QUEUE_SIZE) {
            event = (struct queued_event){native_clock_ns(), 0, 3, 0}; next.sequence = q->sequence; next.report = true;
        } else if (next.report) { event = (struct queued_event){native_clock_ns(), 0, 0, 0}; next.report = false; }
        else event = q->events[next.sequence++ % QUEUE_SIZE];
        uint64_t address = buffer + used * sizeof(struct input_event64), time = event.time + (next.clock ? 0 : realtime_offset);
        if (address < buffer || !accessible(address, sizeof(struct input_event64), true)) {
            event_source_changed(q, 0); return used ? (int64_t)(used * sizeof(struct input_event64)) : -14;
        }
        struct input_event64 output = {time / 1000000000, time % 1000000000 / 1000, event.type, event.code, event.value};
        if (copy_to_user((void*)address, &output, sizeof(output))) { event_source_changed(q, 0); return used ? (int64_t)(used * sizeof(struct input_event64)) : -14; }
        *reader = next; used++;
    }
    event_source_changed(q, 0); return used * sizeof(struct input_event64);
}
static void bit(uint8_t* bytes, unsigned number) { bytes[number / 8] |= 1U << (number % 8); }
/* ---- Extra sources (USB HID, usbhid.c) share the two nodes: one keyboard, one pointer for the compositor. ---- */
static uint8_t source_keys[2][KEY_BYTES];
static bool pointer_absolute, pointer_extended, usb_only[2];
static int32_t absolute_value[2];
void input_source_attach(unsigned index, const uint8_t* key_bits, bool absolute) {
    if (index > 1) return;
    if (!realtime_offset) realtime_offset = native_realtime_ns() - native_clock_ns();
    if (!inputs[index].present) usb_only[index] = true;
    inputs[index].present = true;
    if (key_bits) for (unsigned i = 0; i < KEY_BYTES; i++) source_keys[index][i] |= key_bits[i];
    if (index) { pointer_extended = true; pointer_absolute |= absolute; }
}
void input_source_key(unsigned index, unsigned code, int value) {
    if (index > 1 || code >= KEY_BYTES * 8) return;
    if (value != 2) key(index, code, value != 0);
    else if (inputs[index].keys[code / 8] & (1U << (code % 8))) push(index, 1, code, 2);
}
void input_source_motion(unsigned type, unsigned code, int value) {
    if (type == 3 && code < 2) absolute_value[code] = value;
    push(1, type, code, value);
}
void input_source_report(unsigned index) { if (index < 2) report(index); }
int input_ioctl(unsigned index, struct input_reader* reader, uint64_t command, uint64_t argument) {
    if (command == 0x400445A0) {
        if (!accessible(argument, 4, false)) return -14;
        int clock = (int)uget32(argument); if (clock != 0 && clock != 1) return -22;
        reader->clock = clock; return 0;
    }
    if ((command & 0xFFFFFFFFC000FFFFULL) != (0x80004500U | (command & 255))) return -25;
    unsigned number = command & 255, size = (command >> 16) & 0x3FFF, length = 0;
    uint8_t data[KEY_BYTES] = {0}; bool fixed = false;
    if (number == 1 && size == 4) { *(uint32_t*)data = 0x10001; length = 4; fixed = true; }
    else if (number == 2 && size == 8) { uint16_t id[] = {0x11, 1, index ? (uint16_t)(mouse_type ? 2 : 1) : 1, 1}; memcpy(data, id, 8); length = 8; fixed = true; }
    else if (number == 6 || number == 7) {
        const char* name = number == 6 ? (index ? (usb_only[1] ? "Nexus USB Pointer" : "Nexus PS/2 Mouse") : (usb_only[0] ? "Nexus USB Keyboard" : "Nexus PS/2 Keyboard")) :
                           usb_only[index] ? (index ? "usb-xhci/input1" : "usb-xhci/input0") : (index ? "isa0060/serio1/input0" : "isa0060/serio0/input0");
        length = strlen(name) + 1; memcpy(data, name, length);
    } else if (number == 9) { length = 4; if (index) bit(data, 0); }
    else if (number == 0x18) { length = KEY_BYTES; memcpy(data, inputs[index].keys, length); }
    else if (number >= 0x20 && number <= 0x3F) {
        unsigned type = number - 0x20;
        if (!type) { length = 4; bit(data, 0); bit(data, 1); if (index) bit(data, 2); if (index && pointer_absolute) bit(data, 3); }
        else if (type == 1) {
            length = KEY_BYTES;
            if (index) for (unsigned i = 272; i < (mouse_type == 4 ? 277U : 275U); i++) bit(data, i);
            else {
                for (unsigned scan = 1; scan < 128; scan++) { unsigned code = keycode(scan, false); if (code) bit(data, code); code = keycode(scan, true); if (code) bit(data, code); }
                bit(data, 119);
            }
            for (unsigned i = 0; i < KEY_BYTES; i++) data[i] |= source_keys[index][i];
        } else if (type == 2) { length = 2; if (index) { bit(data, 0); bit(data, 1); if (mouse_type || pointer_extended) bit(data, 8); if (pointer_extended) bit(data, 6); } }
        else if (type == 3 && index && pointer_absolute) { length = 8; bit(data, 0); bit(data, 1); }
        else { length = size < sizeof(data) ? size : sizeof(data); }
    } else if (number >= 0x40 && number < 0x42 && index && pointer_absolute) { /* EVIOCGABS(ABS_X/ABS_Y) */
        int32_t info[6] = {absolute_value[number - 0x40], 0, 32767, 0, 0, 0}; length = sizeof(info); memcpy(data, info, length); fixed = true;
    } else return -25;
    if (size < length) length = size;
    if (length && !accessible(argument, length, true)) return -14;
    if (length && copy_to_user((void*)argument, data, length)) return -14;
    return fixed ? 0 : (int)length;
}
/* System sleep (pm.c registers this): after S3 or hibernation the controller and both devices come back with their
 * reset configuration (scanning off, no IRQs); program them as at boot. Suspend-to-idle keeps them powered. */
#include "power.h"
static void input_resume(void* context, unsigned state) {
    (void)context;
    if (state == PM_STATE_FREEZE) return;
    prefix = pause_byte = mouse_used = 0; resuming = true; input_init(); resuming = false;
    lost(0); lost(1);   /* keys held across the sleep are released (SYN_DROPPED for readers) */
}
const struct pm_ops input_pm = {"ps2", NULL, input_resume, NULL};
