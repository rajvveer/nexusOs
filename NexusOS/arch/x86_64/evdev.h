/* Hot-plug input devices (evdev.c): /dev/input/eventN with the Linux evdev ABI and, for joysticks and gamepads,
 * /dev/input/jsN (joydev), plus their sysfs input class objects (name, id/..., capabilities/..., uevent with udev's
 * ID_INPUT_* properties) so libudev-based programs (Chrome's Gamepad API, SDL2) find them without udevd. Drivers:
 * HID game controllers (gamepad.c, through usbhid.c), Xbox controllers (xpad.c), uinput (uinput.c).
 * All calls: kernel lock held. Force feedback: effects are kept here (per device, FF_MAX); the driver's ff_play gets the
 * motor levels whenever the playing effects change and stops them when an effect's replay length runs out. */
#ifndef NEXUS_EVDEV_H
#define NEXUS_EVDEV_H
#include "native.h"

enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3, EV_MSC = 4, EV_SW = 5, EV_LED = 0x11, EV_SND = 0x12, EV_REP = 0x14, EV_FF = 0x15, EV_PWR = 0x16,
       EV_FF_STATUS = 0x17, EV_UINPUT = 0x0101 };
enum { SYN_REPORT = 0, SYN_DROPPED = 3, FF_RUMBLE = 0x50, FF_PERIODIC = 0x51, FF_CONSTANT = 0x52, FF_RAMP = 0x57, FF_GAIN = 0x60, FF_AUTOCENTER = 0x61 };
enum { BTN_MISC = 0x100, BTN_MOUSE = 0x110, BTN_JOYSTICK = 0x120, BTN_GAMEPAD = 0x130, BTN_SOUTH = 0x130, BTN_EAST = 0x131, BTN_C = 0x132,
       BTN_NORTH = 0x133, BTN_WEST = 0x134, BTN_Z = 0x135, BTN_TL = 0x136, BTN_TR = 0x137, BTN_TL2 = 0x138, BTN_TR2 = 0x139, BTN_SELECT = 0x13A,
       BTN_START = 0x13B, BTN_MODE = 0x13C, BTN_THUMBL = 0x13D, BTN_THUMBR = 0x13E, BTN_DIGI = 0x140, BTN_DPAD_UP = 0x220, BTN_DPAD_DOWN = 0x221,
       BTN_DPAD_LEFT = 0x222, BTN_DPAD_RIGHT = 0x223, KEY_MAX = 0x2FF };
enum { ABS_X = 0, ABS_Y = 1, ABS_Z = 2, ABS_RX = 3, ABS_RY = 4, ABS_RZ = 5, ABS_THROTTLE = 6, ABS_RUDDER = 7, ABS_WHEEL = 8, ABS_GAS = 9,
       ABS_BRAKE = 10, ABS_HAT0X = 0x10, ABS_HAT0Y = 0x11, ABS_MAX = 0x3F };
enum { BUS_USB = 0x03, BUS_VIRTUAL = 0x06, BUS_BLUETOOTH = 0x05 };
#define EVDEV_FF_MAX 16
#define EVDEV_NAME 80

struct evdev_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };
/* struct ff_effect (x86-64, 48 bytes) */
struct evdev_ff_effect {
    uint16_t type; int16_t id; uint16_t direction; uint16_t trigger_button, trigger_interval; uint16_t replay_length, replay_delay; uint16_t pad;
    union {
        struct { uint16_t strong_magnitude, weak_magnitude; } rumble;
        struct { uint16_t waveform, period; int16_t magnitude, offset; uint16_t phase; uint16_t attack_length, attack_level, fade_length, fade_level, pad; uint32_t custom_len; uint64_t custom_data; } __attribute__((packed, aligned(4))) periodic;
        struct { int16_t level; uint16_t attack_length, attack_level, fade_length, fade_level; } constant;
        uint8_t raw[32];
    } u;
};
_Static_assert(sizeof(struct evdev_ff_effect) == 48, "struct ff_effect");
struct evdev_desc {
    char name[EVDEV_NAME], phys[64], uniq[64];
    uint16_t bustype, vendor, product, version;
    uint8_t ev[4], key[96], rel[2], abs[8], msc[1], led[2], snd[1], ff[16], sw[2], prop[4];
    struct evdev_absinfo absinfo[ABS_MAX + 1];
    unsigned ff_max;
};
struct evdev;
struct evdev_ops {
    /* Motor levels (0..65535) of the effects playing now; called on every change and when they stop. Optional. */
    void (*ff_play)(struct evdev* device, unsigned strong, unsigned weak);
    /* uinput: effect uploads/erases and client-written events go to the owning process (uinput.c). Optional. */
    int (*ff_upload)(struct evdev* device, struct evdev_ff_effect* effect, const struct evdev_ff_effect* old);
    int (*ff_erase)(struct evdev* device, int id);
    void (*client_event)(struct evdev* device, unsigned type, unsigned code, int value);
};
static inline void evdev_set_bit(uint8_t* bits, unsigned bit) { bits[bit / 8] |= (uint8_t)(1U << (bit % 8)); }
static inline bool evdev_test_bit(const uint8_t* bits, unsigned bit) { return (bits[bit / 8] >> (bit % 8)) & 1; }

/* A new /dev/input/eventN (+ jsN for joysticks/gamepads); NULL if the tables are full or the description is invalid. */
struct evdev* evdev_create(const struct evdev_desc* desc, const struct evdev_ops* ops, void* context);
void evdev_destroy(struct evdev* device);                       /* unplug: open fds get ENODEV, nodes and sysfs go */
void* evdev_context(const struct evdev* device);
void evdev_emit(struct evdev* device, unsigned type, unsigned code, int value);   /* state + queue (no wake-up) */
void evdev_sync(struct evdev* device);                                           /* SYN_REPORT, wake readers */
int evdev_index(const struct evdev* device);                                      /* N of /dev/input/eventN */
int evdev_js_index(const struct evdev* device);                                   /* N of /dev/input/jsN, -1 */
int evdev_input_number(const struct evdev* device);                               /* N of sysfs inputN */
void uinput_init(void);
void evdev_poll(uint64_t now);                                                    /* effect durations */
bool evdev_is_joystick(const struct evdev_desc* desc);
#endif
