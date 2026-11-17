/* Virtual test game controller, boot option "vpad" (QEMU emulates no game controller): an evdev + joydev device that
 * presents itself exactly like Linux xpad's Xbox 360 pad (045e:028e, "Microsoft X-Box 360 pad", same axes, buttons and
 * FF_RUMBLE), so browsers and SDL apply their standard Xbox mapping, and that moves through a fixed 4-second cycle
 * (hwnode_poll): 0-1 s A held, left stick right (32767); 1-2 s nothing held, left stick left (-32768); 2-3 s B held, left
 * stick down (32767 on ABS_Y); 3-4 s left trigger full (255), D-pad up, stick centred. Rumble requests are logged as
 * "[64] VPAD rumble strong=S weak=W" so a test can check that a page's vibrationActuator reached the device. */
#include "evdev.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

static struct evdev* pad; static uint64_t start_ns; static unsigned last_phase = 99;
static void log_number(const char* label, unsigned value) {
    char text[16]; unsigned n = 0; char digits[12]; unsigned k = 0; do { digits[k++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (k) text[n++] = digits[--k]; text[n] = 0; vga_print(label); vga_print(text);
}
static void ff_play(struct evdev* device, unsigned strong, unsigned weak) {
    (void)device; log_number("[64] VPAD rumble strong=", strong); log_number(" weak=", weak); vga_print("\n");
}
static const struct evdev_ops ops = {ff_play, NULL, NULL, NULL};
uint64_t vpad_next_event(uint64_t now) {                                 /* the next 1 s pattern step; ~0 without vpad */
    if (!pad) return ~0ULL;
    uint64_t elapsed = now > start_ns ? now - start_ns : 0;
    return start_ns + (elapsed / 1000000000ULL + 1) * 1000000000ULL;
}
void vpad_poll(uint64_t now) {
    if (!pad) return;
    unsigned phase = (unsigned)((now - start_ns) / 1000000000ULL % 4);
    if (phase == last_phase) return;
    last_phase = phase;
    evdev_emit(pad, EV_KEY, BTN_SOUTH, phase == 0); evdev_emit(pad, EV_KEY, BTN_EAST, phase == 2);
    evdev_emit(pad, EV_ABS, ABS_X, phase == 0 ? 32767 : phase == 1 ? -32768 : 0);
    evdev_emit(pad, EV_ABS, ABS_Y, phase == 2 ? 32767 : 0);
    evdev_emit(pad, EV_ABS, ABS_Z, phase == 3 ? 255 : 0);
    evdev_emit(pad, EV_ABS, ABS_HAT0Y, phase == 3 ? -1 : 0);
    evdev_sync(pad);
}
void vpad_init(void) {
    if (!boot_option("vpad")) return;
    static struct evdev_desc d; memset(&d, 0, sizeof(d));
    strcpy(d.name, "Microsoft X-Box 360 pad"); strcpy(d.phys, "virtual/vpad/input0");
    d.bustype = BUS_USB; d.vendor = 0x045E; d.product = 0x028E; d.version = 0x0114;
    evdev_set_bit(d.ev, EV_KEY); evdev_set_bit(d.ev, EV_ABS); evdev_set_bit(d.ev, EV_FF); evdev_set_bit(d.ff, FF_RUMBLE); d.ff_max = 16;
    static const uint16_t buttons[11] = {BTN_SOUTH, BTN_EAST, 0x133, 0x134, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
    for (unsigned i = 0; i < 11; i++) evdev_set_bit(d.key, buttons[i]);
    static const uint16_t sticks[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
    for (unsigned i = 0; i < 4; i++) { evdev_set_bit(d.abs, sticks[i]); d.absinfo[sticks[i]] = (struct evdev_absinfo){0, -32768, 32767, 16, 128, 0}; }
    evdev_set_bit(d.abs, ABS_Z); d.absinfo[ABS_Z] = (struct evdev_absinfo){0, 0, 255, 0, 0, 0};
    evdev_set_bit(d.abs, ABS_RZ); d.absinfo[ABS_RZ] = (struct evdev_absinfo){0, 0, 255, 0, 0, 0};
    evdev_set_bit(d.abs, ABS_HAT0X); d.absinfo[ABS_HAT0X] = (struct evdev_absinfo){0, -1, 1, 0, 0, 0};
    evdev_set_bit(d.abs, ABS_HAT0Y); d.absinfo[ABS_HAT0Y] = (struct evdev_absinfo){0, -1, 1, 0, 0, 0};
    pad = evdev_create(&d, &ops, NULL); start_ns = native_clock_ns();
    if (!pad) vga_print("[64] VPAD: no free input device\n");
}
